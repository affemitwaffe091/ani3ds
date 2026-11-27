/*
 * Video player core. Threads:
 *   reader  - av_read_frame() into per-stream packet queues (network bound)
 *   decoder - MVD hardware H.264 into a small ring of screen-sized frames,
 *             audio decode into NDSP wave buffers (runs on core 2 on New 3DS)
 *   main    - picks the frame due at the audio clock, draws the top screen,
 *             handles buttons (A pause, B back, LEFT/RIGHT seek 10 s)
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <malloc.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "libavutil/base64.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "vp.h"

#define TOP_W 400
#define TOP_H 240
#define SIG_N 240
#define VRING 6                 /* decoded frames waiting for display */
#define NBUF 20                 /* NDSP wave buffers */
#define BUF_FRAMES 2048         /* stereo s16 frames per wave buffer */
#define PKT_CAP_BYTES (10 * 1024 * 1024)
#define AUD_CH 0

static const vp_ui *g_ui;
void vp_set_ui(const vp_ui *ui) { g_ui = ui; }
volatile int vp_quit_req;
int vp_max_h = 480;   /* hls: largest variant height to prefer (smallest variant if none fits) */
double vp_start_at;

void vp_log(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (g_ui && g_ui->log) g_ui->log(b); else { fputs(b, stderr); fputc('\n', stderr); }
}

/* ------------------------------------------------------------------ packet queue */
typedef struct pnode { AVPacket *p; struct pnode *next; } pnode;
typedef struct { pnode *head, *tail; int count; size_t bytes; } pqueue;

typedef struct {
    /* shared state */
    LightLock lock;
    volatile int quit, reader_eof, decoder_done, seek_req, reader_parked, decoder_parked;
    double seek_to;
    int reader_err;
    pqueue vq, aq;

    AVFormatContext *fmt;
    int vidx, aidx;
    double vtb, atb;            /* time bases */
    double start_time;

    /* video */
    AVCodecParameters *vpar;
    int mvd_ok, mvd_first, need_key;
    int pw, ph;                 /* padded coded size */
    int ow, oh;                 /* MVD output size */
    int vis_w, vis_h, ox, oy;   /* visible area on screen */
    int cpu_scale;              /* MVD scaling unavailable: scale on CPU */
    int src_w, src_h;           /* size of the frames in the ring */
    u16 *xmap, *ymap;
    u8 *mvd_in; size_t mvd_in_cap;
    u8 *mvd_out;
    MVDSTD_Config cfg;
    double pts_list[48]; int npts;
    u16 *ring[VRING]; double ring_pts[VRING];
    int r_head, r_count;
    int avcc, nal_len;

    /* audio */
    AVCodecContext *actx;
    SwrContext *swr;
    AVFrame *aframe;
    int rate;
    ndspWaveBuf wb[NBUF];
    s16 *wbdata;
    double wb_pts[NBUF];
    double clock_last;              /* audio clock never steps back by small amounts */
    int q_head, q_tail;         /* submitted range [q_head, q_tail) */
    int fill;                   /* frames already in wb[q_tail] */
    double next_apts;           /* pts of the next sample to be written */
    int key_wait;               /* after a seek: audio waits until the video's first keyframe is known */
    double audio_from;          /* ... and audio before that keyframe is dropped (< -1e8: off) */
    double last_end_pts;
    int ndsp_ok, audio_started;
    volatile int dphase;            /* debug: what the decoder thread is doing */
    int frames_out, render_fail, nproc, npkt_v, shown, skipped;
    u32 last_hash; int fc_drawn, fc_new, fc_dec0, fps_drawn, fps_new, fps_dec; u64 fps_t;
    int loops, nblit; u64 work_sum, work_max, blit_sum, blit_max, vbl_sum;   /* debug timing */
    double last_cpts, dp_min, dp_max, dc_min, dc_max, prev_clock; char ptslog[400]; int nptslog;
    int nsig; u8 sig[SIG_N][16]; float sig_pts[SIG_N];
    Result last_r, last_rr;
    int drain, n_proc_out, n_render_out, n_drain_out;   /* MVD output bookkeeping */
} vp_ctx;

static void pq_push(vp_ctx *c, pqueue *q, AVPacket *p)
{
    pnode *n = malloc(sizeof *n);
    n->p = p; n->next = NULL;
    LightLock_Lock(&c->lock);
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    q->count++;
    q->bytes += p->size;
    LightLock_Unlock(&c->lock);
}
static AVPacket *pq_pop(vp_ctx *c, pqueue *q)
{
    LightLock_Lock(&c->lock);
    pnode *n = q->head;
    AVPacket *p = NULL;
    if (n) {
        q->head = n->next;
        if (!q->head) q->tail = NULL;
        q->count--;
        q->bytes -= n->p->size;
        p = n->p;
        free(n);
    }
    LightLock_Unlock(&c->lock);
    return p;
}
static void pq_clear(vp_ctx *c, pqueue *q)
{
    AVPacket *p;
    while ((p = pq_pop(c, q))) av_packet_free(&p);
}
static double pq_span(vp_ctx *c, pqueue *q, double tb)
{
    double r = 0;
    LightLock_Lock(&c->lock);
    if (q->head && q->tail && q->head->p->dts != AV_NOPTS_VALUE && q->tail->p->dts != AV_NOPTS_VALUE)
        r = (q->tail->p->dts - q->head->p->dts) * tb;
    LightLock_Unlock(&c->lock);
    return r;
}

/* ------------------------------------------------------------------ helpers */
static char *url_join(const char *base, const char *rel)
{
    char *out;
    if (strstr(rel, "://")) return strdup(rel);
    const char *sch = strstr(base, "://");
    if (!sch) return strdup(rel);
    if (rel[0] == '/' && rel[1] == '/') {
        size_t n = sch - base + 1;
        out = malloc(n + strlen(rel) + 1);
        memcpy(out, base, n); strcpy(out + n, rel);
        return out;
    }
    const char *host_end = sch + 3 + strcspn(sch + 3, "/?#");
    if (rel[0] == '/') {
        size_t n = host_end - base;
        out = malloc(n + strlen(rel) + 1);
        memcpy(out, base, n); strcpy(out + n, rel);
        return out;
    }
    const char *q = base + strcspn(base, "?#");
    const char *slash = q;
    while (slash > host_end && slash[-1] != '/') slash--;
    size_t n = slash > host_end ? (size_t)(slash - base) : (size_t)(host_end - base);
    out = malloc(n + 1 + strlen(rel) + 1);
    memcpy(out, base, n);
    if (slash <= host_end) out[n++] = '/';
    strcpy(out + n, rel);
    return out;
}

static int attr_int(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    return p ? atoi(p + strlen(key)) : 0;
}
static int attr_str(const char *line, const char *key, char *out, size_t cap)
{
    const char *p = strstr(line, key);
    out[0] = 0;
    if (!p) return 0;
    p += strlen(key);
    size_t n = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++; }
    else while (*p && *p != ',' && *p != '\r' && *p != '\n' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return 1;
}

/* For an HLS master playlist, choose one H.264 variant <= 480p (the 3DS
 * screen is 240p; MVD handles up to 720p) and build a small master that only
 * contains it (and its audio rendition), so ffmpeg does not probe every
 * quality level. Returns a URL to open (data: or http) or NULL = use as is. */
static char *hls_choose(const char *url)
{
    long st;
    char *txt = vp_fetch_text(url, &st);
    if (!txt) return NULL;
    if (!strstr(txt, "#EXT-X-STREAM-INF")) { free(txt); return NULL; }
    char best_uri[2048] = "", best_audio[128] = "";
    int best_score = -1, best_bw = 0;
    char *save = NULL;
    char *line = strtok_r(txt, "\n", &save);
    char inf[1024] = "";
    int have_inf = 0;
    /* first pass: variants */
    while (line) {
        size_t L = strlen(line);
        if (L && line[L - 1] == '\r') line[L - 1] = 0;
        if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) { snprintf(inf, sizeof inf, "%s", line); have_inf = 1; }
        else if (have_inf && line[0] && line[0] != '#') {
            char codecs[256], res[32], audio[128];
            attr_str(inf, "CODECS=", codecs, sizeof codecs);
            attr_str(inf, "RESOLUTION=", res, sizeof res);
            attr_str(inf, "AUDIO=", audio, sizeof audio);
            int bw = attr_int(inf, "BANDWIDTH=");
            int w = 0, h = 0;
            if (res[0]) sscanf(res, "%dx%d", &w, &h);
            int h264 = !codecs[0] || strstr(codecs, "avc1") || strstr(codecs, "avc3");
            int bad = strstr(codecs, "hvc1") || strstr(codecs, "hev1") || strstr(codecs, "av01") || strstr(codecs, "vp09") || strstr(codecs, "dvh");
            if (h264 && !bad) {
                /* prefer the largest variant up to 480p; otherwise the smallest one */
                int score = (h && h <= vp_max_h && w <= (vp_max_h > 480 ? 1920 : 1152)) ? 100000 + h : (h ? 50000 - h : 1000 + bw / 1000);
                if (score > best_score || (score == best_score && bw > best_bw)) {
                    best_score = score; best_bw = bw;
                    snprintf(best_uri, sizeof best_uri, "%s", line);
                    snprintf(best_audio, sizeof best_audio, "%s", audio);
                }
            }
            have_inf = 0;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(txt);
    if (!best_uri[0]) { vp_log("hls: no H.264 variant in master playlist"); return NULL; }
    char *vuri = url_join(url, best_uri);
    vp_log("hls: picked variant (score %d, %d bps)", best_score, best_bw);
    if (!best_audio[0]) return vuri;

    /* the variant has a separate audio rendition: find its URI */
    txt = vp_fetch_text(url, &st);
    char aurl[2048] = "";
    if (txt) {
        save = NULL;
        line = strtok_r(txt, "\n", &save);
        int best_def = -1;
        while (line) {
            if (!strncmp(line, "#EXT-X-MEDIA:", 13) && strstr(line, "TYPE=AUDIO")) {
                char grp[128], u[2048];
                attr_str(line, "GROUP-ID=", grp, sizeof grp);
                if (!strcmp(grp, best_audio) && attr_str(line, "URI=", u, sizeof u) && u[0]) {
                    int def = strstr(line, "DEFAULT=YES") ? 1 : 0;
                    if (def > best_def) { best_def = def; snprintf(aurl, sizeof aurl, "%s", u); }
                }
            }
            line = strtok_r(NULL, "\n", &save);
        }
        free(txt);
    }
    if (!aurl[0]) return vuri;   /* audio is muxed into the variant */
    char *aabs = url_join(url, aurl);
    size_t cap = strlen(vuri) + strlen(aabs) + 512;
    char *m3u = malloc(cap);
    snprintf(m3u, cap,
             "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"a\",DEFAULT=YES,AUTOSELECT=YES,URI=\"%s\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=%d,AUDIO=\"a\"\n%s\n", aabs, best_bw ? best_bw : 1000000, vuri);
    free(vuri); free(aabs);
    size_t len = strlen(m3u);
    size_t b64 = AV_BASE64_SIZE(len);
    char *out = malloc(64 + b64);
    strcpy(out, "data:application/vnd.apple.mpegurl;base64,");
    av_base64_encode(out + strlen(out), b64, (const uint8_t *)m3u, len);
    free(m3u);
    return out;
}

static int interrupt_cb(void *u) { return vp_net_abort; }

/* ------------------------------------------------------------------ audio */
static void audio_reclaim(vp_ctx *c)
{
    while (c->q_head != c->q_tail && c->wb[c->q_head].status == NDSP_WBUF_DONE)
        c->q_head = (c->q_head + 1) % NBUF;
}
static int audio_queued(vp_ctx *c) { return (c->q_tail - c->q_head + NBUF) % NBUF; }
static double audio_clock(vp_ctx *c)
{
    LightLock_Lock(&c->lock);
    audio_reclaim(c);
    double t;
    if (c->q_head == c->q_tail) t = c->last_end_pts;
    else {
        /* NDSP updates the playing buffer's sequence id together with the sample position; the DONE
         * flags used for q_head lead/lag them at every buffer boundary (every 46 ms), which made the
         * clock jump +-1 buffer and the display dropped about half of the video frames as late */
        u16 seq = ndspChnGetWaveBufSeq(AUD_CH);
        u32 pos = ndspChnGetSamplePos(AUD_CH);
        int found = -1;
        for (int i = 0; i < NBUF; i++)
            if (c->wb[i].sequence_id == seq && c->wb[i].nsamples) { found = i; break; }
        if (found >= 0) t = c->wb_pts[found] + (double)pos / c->rate;
        else t = c->wb_pts[c->q_head] + (double)pos / c->rate;
    }
    if (t < c->clock_last && c->clock_last - t < 0.25) t = c->clock_last;   /* jitter, not a seek */
    c->clock_last = t;
    LightLock_Unlock(&c->lock);
    return t;
}
static double audio_ahead(vp_ctx *c)
{
    LightLock_Lock(&c->lock);
    audio_reclaim(c);
    double s = audio_queued(c) * (double)BUF_FRAMES / c->rate;
    LightLock_Unlock(&c->lock);
    return s;
}
static int audio_free_slots(vp_ctx *c)
{
    LightLock_Lock(&c->lock);
    audio_reclaim(c);
    int f = (NBUF - 1) - audio_queued(c);
    LightLock_Unlock(&c->lock);
    return f;
}
static void audio_submit(vp_ctx *c)
{
    if (!c->fill) return;
    ndspWaveBuf *b = &c->wb[c->q_tail];
    b->nsamples = c->fill;
    DSP_FlushDataCache(b->data_pcm16, c->fill * 4);
    LightLock_Lock(&c->lock);
    c->last_end_pts = c->wb_pts[c->q_tail] + (double)c->fill / c->rate;
    ndspChnWaveBufAdd(AUD_CH, b);
    c->q_tail = (c->q_tail + 1) % NBUF;
    LightLock_Unlock(&c->lock);
    c->fill = 0;
}
/* append decoded samples; returns 0 when the queue is full (try again later) */
static int audio_room(vp_ctx *c)
{
    LightLock_Lock(&c->lock);
    audio_reclaim(c);
    int ok = audio_queued(c) < NBUF - 1;
    LightLock_Unlock(&c->lock);
    return ok;
}
static void audio_write(vp_ctx *c, const s16 *pcm, int frames)
{
    while (frames > 0) {
        while (!audio_room(c) && !c->quit && !c->seek_req) svcSleepThread(4000000);
        if (c->quit || c->seek_req) return;
        if (c->fill == 0) c->wb_pts[c->q_tail] = c->next_apts;
        int n = BUF_FRAMES - c->fill;
        if (n > frames) n = frames;
        memcpy(c->wb[c->q_tail].data_pcm16 + c->fill * 2, pcm, n * 4);
        c->fill += n; pcm += n * 2; frames -= n;
        c->next_apts += (double)n / c->rate;
        if (c->fill == BUF_FRAMES) audio_submit(c);
    }
}
static void audio_decode(vp_ctx *c, AVPacket *p)
{
    if (avcodec_send_packet(c->actx, p) < 0) return;
    while (avcodec_receive_frame(c->actx, c->aframe) == 0) {
        AVFrame *f = c->aframe;
        int64_t ts = f->best_effort_timestamp;
        if (ts != AV_NOPTS_VALUE) {
            double t = ts * c->atb - c->start_time;
            if (fabs(t - c->next_apts) > 0.15) { audio_submit(c); c->next_apts = t; }
        }
        int maxout = swr_get_out_samples(c->swr, f->nb_samples);
        s16 *tmp = malloc(maxout * 4 + 64);
        uint8_t *outp[1] = { (uint8_t *)tmp };
        int got = swr_convert(c->swr, outp, maxout, (const uint8_t **)f->extended_data, f->nb_samples);
        if (got > 0) audio_write(c, tmp, got);
        free(tmp);
    }
}

/* decode one queued audio packet; 0 = none queued. After a seek the picture can only start at the
 * next keyframe (with these HLS streams up to a whole segment after the target), so audio before
 * it is dropped and both start there together. */
static int audio_step(vp_ctx *c)
{
    AVPacket *p = pq_pop(c, &c->aq);
    if (!p) return 0;
    if (c->audio_from > -1e8) {
        int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
        if (ts != AV_NOPTS_VALUE) {
            double t = ts * c->atb - c->start_time, d = p->duration > 0 ? p->duration * c->atb : 0;
            if (t + d <= c->audio_from) { av_packet_free(&p); return 1; }
        }
        c->audio_from = -1e9;
    }
    audio_decode(c, p);
    av_packet_free(&p);
    return 1;
}

/* ------------------------------------------------------------------ video (MVD) */
static int round_up16(int v) { return (v + 15) & ~15; }

static void build_maps(vp_ctx *c)
{
    free(c->xmap); free(c->ymap);
    c->xmap = malloc(c->vis_w * sizeof(u16));
    c->ymap = malloc(c->vis_h * sizeof(u16));
    for (int x = 0; x < c->vis_w; x++) {
        int v = c->cpu_scale ? (int)((long)x * c->vpar->width / c->vis_w) : x;
        c->xmap[x] = v < c->src_w ? v : c->src_w - 1;
    }
    for (int y = 0; y < c->vis_h; y++) {
        int v = c->cpu_scale ? (int)((long)y * c->vpar->height / c->vis_h) : y;
        c->ymap[y] = v < c->src_h ? v : c->src_h - 1;
    }
}

static int video_setup(vp_ctx *c)
{
    int w = c->vpar->width, h = c->vpar->height;
    c->pw = round_up16(w); c->ph = round_up16(h);
    double s = fmin((double)TOP_W / w, (double)TOP_H / h);
    c->vis_w = (int)(w * s) & ~1; c->vis_h = (int)(h * s) & ~1;
    if (c->vis_w > TOP_W) c->vis_w = TOP_W;
    if (c->vis_h > TOP_H) c->vis_h = TOP_H;
    c->ox = (TOP_W - c->vis_w) / 2; c->oy = (TOP_H - c->vis_h) / 2;
    /* MVD scales the padded picture to ow x oh; we show the top-left vis area */
    c->ow = ((int)(c->pw * s) + 7) & ~7;
    c->oh = ((int)(c->ph * s) + 1) & ~1;
    if (c->ow > 512) c->ow = 512;

    Result r = -1;
    for (int mb = 14; mb >= 5 && r; mb--)
        r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, mb * 1000000, NULL);
    if (r) { vp_log("mvdstdInit failed: %08lx (linear free %lu KB)", r, linearSpaceFree() / 1024); return -1; }
    c->mvd_ok = 1;
    c->cpu_scale = 0;
    c->src_w = c->ow; c->src_h = c->oh;
    c->mvd_out = linearAlloc(c->pw * c->ph * 2);   /* MVD renders here, then it is copied */
    if (!c->mvd_out) { vp_log("no linear memory for the decoder output"); return -1; }
    for (int i = 0; i < VRING; i++) {
        c->ring[i] = malloc(c->pw * c->ph * 2);    /* big enough for CPU scaling too */
        if (!c->ring[i]) { vp_log("no memory for frame %d", i); return -1; }
    }
    mvdstdGenerateDefaultConfig(&c->cfg, c->pw, c->ph, c->ow, c->oh, NULL, NULL, NULL);
    c->mvd_first = 1;
    c->need_key = 1;
    c->avcc = c->vpar->extradata_size > 7 && c->vpar->extradata[0] == 1;
    c->nal_len = c->avcc ? (c->vpar->extradata[4] & 3) + 1 : 4;
    build_maps(c);
    vp_log("video %dx%d -> %dx%d on screen (mvd out %dx%d, %s)", w, h, c->vis_w, c->vis_h, c->ow, c->oh, c->avcc ? "mp4/avcC" : "annex-b");
    return 0;
}

static int mvd_switch_to_cpu_scale(vp_ctx *c)
{
    vp_log("MVD scaling refused - scaling on the CPU");
    c->cpu_scale = 1;
    c->ow = c->pw; c->oh = c->ph;
    c->src_w = c->pw; c->src_h = c->ph;
    mvdstdGenerateDefaultConfig(&c->cfg, c->pw, c->ph, c->pw, c->ph, NULL, NULL, NULL);
    build_maps(c);
    return 0;
}

static u8 *mvd_buf(vp_ctx *c, size_t need)
{
    if (need > c->mvd_in_cap) {
        if (c->mvd_in) linearFree(c->mvd_in);
        c->mvd_in_cap = need + 65536;
        c->mvd_in = linearAlloc(c->mvd_in_cap);
        if (!c->mvd_in) c->mvd_in_cap = 0;
    }
    return c->mvd_in;
}

static size_t put_nal(u8 *dst, const u8 *src, size_t n)
{
    dst[0] = 0; dst[1] = 0; dst[2] = 1;
    memcpy(dst + 3, src, n);
    return n + 3;
}

static void pts_insert(vp_ctx *c, double t)
{
    if (c->npts == 48) { memmove(c->pts_list, c->pts_list + 1, 47 * sizeof(double)); c->npts--; }
    int i = c->npts;
    while (i > 0 && c->pts_list[i - 1] > t) { c->pts_list[i] = c->pts_list[i - 1]; i--; }
    c->pts_list[i] = t;
    c->npts++;
}
#define PTS_UNSET (-1000.0)
static double pts_take_min(vp_ctx *c);
/* lock held: give the first n queued frames (display order) their timestamps */
static void label_queue(vp_ctx *c, int n)
{
    for (int k = 0; k < n && k < c->r_count; k++) {
        int i = (c->r_head + k) % VRING;
        if (c->ring_pts[i] != PTS_UNSET) continue;
        double t = c->npts ? pts_take_min(c) : -1;
        c->ring_pts[i] = t;
        if (vp_debug && t >= 0) {   /* timestamp spacing check */
            double dp = t - c->last_cpts;
            if (c->last_cpts > 0) { if (dp < c->dp_min) c->dp_min = dp; if (dp > c->dp_max) c->dp_max = dp; }
            c->last_cpts = t;
            if (c->nptslog < 16) {
                size_t L = strlen(c->ptslog);
                snprintf(c->ptslog + L, sizeof c->ptslog - L, "%.3f ", t);
                if (++c->nptslog == 16) vp_log("first frame pts: %s", c->ptslog);
            }
        }
    }
}
static double pts_take_min(vp_ctx *c)
{
    if (!c->npts) return -1;
    double t = c->pts_list[0];
    memmove(c->pts_list, c->pts_list + 1, (c->npts - 1) * sizeof(double));
    c->npts--;
    return t;
}

/* hash of a whole frame as it will be drawn: equal hash = the same picture again */
static u32 frame_hash(vp_ctx *c, const u16 *f)
{
    const u32 *w = (const u32 *)f;
    u32 h = 2166136261u, n = (u32)(c->src_w * c->src_h) / 2;
    for (u32 i = 0; i < n; i++) { h ^= w[i]; h *= 16777619u; }
    return h;
}

/* marker pixels in the output corners: changed = MVD wrote a picture */
#define MVD_MARK 0x1111
static void mvd_mark(vp_ctx *c)
{
    u16 *o = (u16 *)c->mvd_out; int w = c->src_w, h = c->src_h;
    o[0] = o[w - 1] = o[(h - 1) * w] = o[h * w - 1] = MVD_MARK;
    GSPGPU_FlushDataCache(o, w * h * 2);
}
static int mvd_written(vp_ctx *c)
{
    u16 *o = (u16 *)c->mvd_out; int w = c->src_w, h = c->src_h;
    GSPGPU_InvalidateDataCache(o, w * h * 2);
    return o[0] != MVD_MARK || o[w - 1] != MVD_MARK || o[(h - 1) * w] != MVD_MARK || o[h * w - 1] != MVD_MARK;
}
static void mvd_set_out(vp_ctx *c)
{
    c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->mvd_out);
    MVDSTD_SetConfig(&c->cfg);
}
/* ask MVD for a pending picture; 1 = one was written to mvd_out */
static int mvd_render_one(vp_ctx *c)
{
    mvd_mark(c);
    for (int spin = 0; spin < 500 && !c->quit; spin++) {
        Result r = mvdstdRenderVideoFrame(&c->cfg, false);   /* stock libctru refuses NULL (-1) */
        if (mvd_written(c)) return 1;
        if (r == MVD_STATUS_BUSY) { svcSleepThread(200000); continue; }
        if (r != MVD_STATUS_OK && r != 0) {
            c->last_rr = r;
            if (c->render_fail++ < 5) vp_log("mvd render failed %08lx", r);
            /* only a real MVD error (not libctru's -1..-3 argument errors) means scaling was refused */
            if (!c->cpu_scale && r < -3) { mvd_switch_to_cpu_scale(c); mvd_set_out(c); mvd_mark(c); continue; }
        }
        return 0;
    }
    return 0;
}
/* copy the picture in mvd_out into the display queue (waits for a free slot); 0 = quit/seek */
static int mvd_emit(vp_ctx *c)
{
    c->dphase = 6;
    for (;;) {
        LightLock_Lock(&c->lock);
        int full = c->r_count == VRING;
        LightLock_Unlock(&c->lock);
        if (!full || c->quit || c->seek_req) break;
        /* keep the sound going while the picture waits: after a seek the audio starts at the
         * target but the video only at the next keyframe (up to a segment later), and the display
         * takes frames only when the audio clock reaches them - without this the ring stays full,
         * audio runs dry and the clock never gets there */
        if (c->aidx >= 0 && audio_free_slots(c) >= 3) {
            c->dphase = 7;
            int got = audio_step(c);
            c->dphase = 6;
            if (got) continue;
        }
        svcSleepThread(3000000);
    }
    if (c->quit || c->seek_req) return 0;
    LightLock_Lock(&c->lock);
    int slot = (c->r_head + c->r_count) % VRING;
    LightLock_Unlock(&c->lock);
    memcpy(c->ring[slot], c->mvd_out, c->src_w * c->src_h * 2);
    /* no timestamp yet: label_queue gives the smallest pending pts when the display takes it */
    LightLock_Lock(&c->lock);
    c->ring_pts[slot] = PTS_UNSET;
    c->r_count++;
    LightLock_Unlock(&c->lock);
    if (c->frames_out++ == 0) vp_log("first frame decoded");
    return 1;
}

static void video_decode(vp_ctx *c, AVPacket *p)
{
    if (c->need_key) {
        if (!(p->flags & AV_PKT_FLAG_KEY)) return;
        c->need_key = 0;
    }
    const u8 *ex = c->vpar->extradata;
    if (c->mvd_first && c->avcc) {
        /* hand SPS and PPS to the decoder first */
        int nsps = ex[5] & 31, off = 6;
        for (int k = 0; k < 2; k++) {
            int cnt = k == 0 ? nsps : ex[off++];
            for (int i = 0; i < cnt && off + 2 <= c->vpar->extradata_size; i++) {
                int len = (ex[off] << 8) | ex[off + 1];
                off += 2;
                if (off + len > c->vpar->extradata_size) break;
                u8 *b = mvd_buf(c, len + 16);
                size_t n = put_nal(b, ex + off, len);
                Result r = mvdstdProcessVideoFrame(b, n, 0, NULL);
                if (!MVD_CHECKNALUPROC_SUCCESS(r)) vp_log("mvd: param set rejected %08lx", r);
                off += len;
            }
        }
    }
    u8 *b = mvd_buf(c, p->size * 2 + 64);
    if (!b) return;
    size_t n = 0;
    if (c->avcc) {
        int so = 0;
        while (so + c->nal_len <= p->size) {
            int len = 0;
            for (int i = 0; i < c->nal_len; i++) len = (len << 8) | p->data[so + i];
            so += c->nal_len;
            if (len <= 0 || so + len > p->size) break;
            n += put_nal(b + n, p->data + so, len);
            so += len;
        }
    } else {
        memcpy(b, p->data, p->size);
        n = p->size;
    }
    if (vp_debug && c->npkt_v++ < 2) {
        char t[160]; int k = 0;
        for (size_t i = 0; i + 3 < n && k < 150; i++)
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) k += snprintf(t + k, sizeof t - k, "%d ", b[i + 3] & 31);
        t[k] = 0;
        vp_log("pkt %d bytes key=%d nal types: %s", p->size, !!(p->flags & AV_PKT_FLAG_KEY), t);
    }
    /* pictures still pending from the last packet come out first */
    if (c->drain) {
        c->dphase = 5;
        while (!c->quit && !c->seek_req && mvd_render_one(c)) {
            c->n_drain_out++;
            if (!mvd_emit(c)) return;
        }
        c->drain = 0;
    }
    if (c->quit || c->seek_req) return;
    mvd_mark(c);
    mvd_set_out(c);
    c->dphase = 4;
    Result r = mvdstdProcessVideoFrame(b, n, 0, NULL);
    c->nproc++;
    c->last_r = r;
    if (c->mvd_first) r = mvdstdProcessVideoFrame(b, n, 0, NULL);
    c->mvd_first = 0;
    if (MVD_CHECKNALUPROC_SUCCESS(r)) {
        int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
        LightLock_Lock(&c->lock);
        pts_insert(c, ts == AV_NOPTS_VALUE ? -1 : ts * c->vtb - c->start_time);
        LightLock_Unlock(&c->lock);
    } else {
        static int warned;
        if (warned++ < 5) vp_log("mvd: frame rejected %08lx", r);
    }
    if (mvd_written(c)) {            /* a picture came out while processing */
        c->n_proc_out++;
        c->drain = 1;                /* more may be pending: drain before the next packet */
        mvd_emit(c);
    } else if (MVD_CHECKNALUPROC_SUCCESS(r)) {
        c->dphase = 5;
        if (mvd_render_one(c)) { c->n_render_out++; mvd_emit(c); }
    }
}

/* ------------------------------------------------------------------ threads */
static void reader_thread(void *arg)
{
    vp_ctx *c = arg;
    while (!c->quit) {
        if (c->seek_req) { c->reader_parked = 1; svcSleepThread(5000000); continue; }
        c->reader_parked = 0;
        LightLock_Lock(&c->lock);
        size_t bytes = c->vq.bytes + c->aq.bytes;
        int acount = c->aq.count;
        LightLock_Unlock(&c->lock);
        if (bytes > PKT_CAP_BYTES && (c->aidx < 0 || acount > 30)) { svcSleepThread(10000000); continue; }
        AVPacket *p = av_packet_alloc();
        int r = av_read_frame(c->fmt, p);
        if (r == AVERROR(EAGAIN)) { av_packet_free(&p); svcSleepThread(5000000); continue; }
        if (r < 0) {
            av_packet_free(&p);
            if (c->seek_req) continue;
            c->reader_err = r == AVERROR_EOF ? 0 : r;
            c->reader_eof = 1;
            while (!c->quit && !c->seek_req) svcSleepThread(20000000);
            if (c->quit) break;
            continue;
        }
        if (p->stream_index == c->vidx) pq_push(c, &c->vq, p);
        else if (p->stream_index == c->aidx) pq_push(c, &c->aq, p);
        else av_packet_free(&p);
    }
}

static void decoder_thread(void *arg)
{
    vp_ctx *c = arg;
    while (!c->quit) {
        if (c->seek_req) { c->decoder_parked = 1; svcSleepThread(3000000); continue; }
        c->decoder_parked = 0;
        int did = 0;
        if (c->aidx >= 0 && !c->key_wait && audio_free_slots(c) >= 3) {
            c->dphase = 2;
            did = audio_step(c);
            c->dphase = 0;
        }
        if (c->vidx >= 0 && c->mvd_ok) {
            LightLock_Lock(&c->lock);
            int room = c->r_count < VRING;
            LightLock_Unlock(&c->lock);
            if (room) {
                AVPacket *p = pq_pop(c, &c->vq);
                if (p && c->key_wait && (p->flags & AV_PKT_FLAG_KEY)) {   /* first keyframe after a seek */
                    int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
                    c->audio_from = ts != AV_NOPTS_VALUE ? ts * c->vtb - c->start_time : -1e9;
                    c->key_wait = 0;
                }
                if (p) { c->dphase = 3; video_decode(c, p); av_packet_free(&p); did = 1; c->dphase = 0; }
            }
        }
        if (!did) {
            LightLock_Lock(&c->lock);
            int empty = !c->vq.count && !c->aq.count;
            LightLock_Unlock(&c->lock);
            if (c->reader_eof && empty) {
                if (c->fill) audio_submit(c);
                c->decoder_done = 1;
            }
            svcSleepThread(2000000);
        }
    }
}

/* ------------------------------------------------------------------ display */
/* debug: 4x4 grid of mean luma over the visible picture of a decoded frame */
static void frame_sig(vp_ctx *c, const u16 *src, u8 out[16])
{
    for (int gy = 0; gy < 4; gy++)
        for (int gx = 0; gx < 4; gx++) {
            u32 sum = 0, n = 0;
            int x0 = c->vis_w * gx / 4, x1 = c->vis_w * (gx + 1) / 4;
            int y0 = c->vis_h * gy / 4, y1 = c->vis_h * (gy + 1) / 4;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    u16 px = src[c->ymap[y] * c->src_w + c->xmap[x]];
                    u32 r = (px >> 11) & 31, g = (px >> 5) & 63, b = px & 31;
                    sum += (r * 8 * 77 + g * 4 * 150 + b * 8 * 29) >> 8;
                    n++;
                }
            out[gy * 4 + gx] = n ? sum / n : 0;
        }
}

static void blit(vp_ctx *c, const u16 *src)
{
    {   /* clear the letterbox/pillarbox bars every frame: overlays drawn there (subtitles)
         * are not covered by the next frame and would pile up in both framebuffers */
        u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        int bot = TOP_H - c->oy - c->vis_h;   /* rows below the picture (stored first in a column) */
        for (int x = 0; x < TOP_W; x++) {
            u16 *col = fb + x * TOP_H;
            if (x < c->ox || x >= c->ox + c->vis_w) { memset(col, 0, TOP_H * 2); continue; }
            if (bot > 0) memset(col, 0, bot * 2);
            if (c->oy > 0) memset(col + TOP_H - c->oy, 0, c->oy * 2);
        }
    }
    for (int fbi = 0; fbi < 1; fbi++) {
        u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        const int sw = c->src_w;
        for (int x = 0; x < c->vis_w; x++) {
            u16 *col = fb + (c->ox + x) * TOP_H + (TOP_H - 1 - c->oy);
            const u16 *s = src + c->xmap[x];
            const u16 *ym = c->ymap;
            for (int y = 0; y < c->vis_h; y++) col[-y] = s[ym[y] * sw];
        }
    }
}
static void clear_top(void)
{
    for (int i = 0; i < 2; i++) {
        u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        memset(fb, 0, TOP_W * TOP_H * 2);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

/* ------------------------------------------------------------------ seek */
static void do_seek(vp_ctx *c, double target)
{
    if (target < 0) target = 0;
    c->seek_req = 1;
    vp_net_abort = 1;                 /* unblock a reader waiting on the network */
    for (int i = 0; i < 300 && !(c->reader_parked && c->decoder_parked); i++) svcSleepThread(10000000);
    vp_net_abort = 0;
    pq_clear(c, &c->vq); pq_clear(c, &c->aq);
    int64_t ts = (int64_t)((target + c->start_time) * AV_TIME_BASE);
    int r = avformat_seek_file(c->fmt, -1, INT64_MIN, ts, ts, 0);
    if (r < 0) vp_log("seek failed (%d)", r);
    pq_clear(c, &c->vq); pq_clear(c, &c->aq);
    if (c->actx) avcodec_flush_buffers(c->actx);
    if (c->ndsp_ok) ndspChnWaveBufClear(AUD_CH);
    LightLock_Lock(&c->lock);
    c->q_head = c->q_tail = 0;
    c->fill = 0;
    for (int i = 0; i < NBUF; i++) c->wb[i].status = NDSP_WBUF_DONE;
    c->r_count = 0;
    c->npts = 0;
    c->next_apts = target;
    c->last_end_pts = target;
    LightLock_Unlock(&c->lock);
    c->need_key = 1;
    c->key_wait = c->aidx >= 0 && c->vidx >= 0 && c->mvd_ok;
    c->audio_from = -1e9;
    c->drain = 0;
    c->reader_eof = 0;
    c->decoder_done = 0;
    c->audio_started = 0;
    c->seek_req = 0;
}

/* ------------------------------------------------------------------ main entry */
int vp_debug;
static void vp_av_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level()) return;
    char b[400];
    vsnprintf(b, sizeof b, fmt, vl);
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    if (n) vp_log("ff: %s", b);
}

int vp_play(const char *url, const char *referer, const char *title, char *err, size_t errlen)
{
    vp_ctx *c = calloc(1, sizeof *c);
    int ret = -1;
    Thread treader = NULL, tdec = NULL;
    char *open_url = NULL;
    GSPGPU_FramebufferFormat old_fmt = gfxGetScreenFormat(GFX_TOP);
    bool old_3d = gfxIs3D();
    err[0] = 0;
    LightLock_Init(&c->lock);
    c->vidx = c->aidx = -1;
    c->audio_from = -1e9;
    vp_net_abort = 0;
    vp_quit_req = 0;
    snprintf(vp_referer, sizeof vp_referer, "%s", referer ? referer : "");
    memset(&vp_net_stats, 0, sizeof vp_net_stats);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    vp_net_set_priority(prio - 1);
    av_log_set_level(vp_debug ? AV_LOG_VERBOSE : AV_LOG_ERROR);
    if (vp_debug) av_log_set_callback(vp_av_log_cb);
    u64 t_open = osGetTime();

    vp_log("opening %.180s", url);
    const AVInputFormat *ifmt = NULL;
    const char *path_end = url + strcspn(url, "?#");
    int is_m3u8 = path_end - url > 5 && !strncmp(path_end - 5, ".m3u8", 5);
    if (is_m3u8 || strstr(url, "m3u8")) {
        open_url = hls_choose(url);
        ifmt = av_find_input_format("hls");
    }
    c->fmt = avformat_alloc_context();
    c->fmt->interrupt_callback.callback = interrupt_cb;
    c->fmt->probesize = 2 * 1024 * 1024;
    c->fmt->max_analyze_duration = 3 * AV_TIME_BASE;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "protocol_whitelist", "http,https,crypto,data,file,tcp,tls", 0);
    /* hls would try to reuse the previous segment's connection through ffmpeg's own http
     * protocol (not built here); the fallback then leaks one stream (1.5 MB + a thread) per segment */
    av_dict_set(&opts, "http_persistent", "0", 0);
    int r = avformat_open_input(&c->fmt, open_url ? open_url : url, ifmt, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        char e[128]; av_strerror(r, e, sizeof e);
        snprintf(err, errlen, "cannot open stream (%s)", e);
        c->fmt = NULL;
        goto out;
    }
    vp_log("opened input (%s) in %.1fs", c->fmt->iformat ? c->fmt->iformat->name : "?", (osGetTime() - t_open) / 1000.0);
    r = avformat_find_stream_info(c->fmt, NULL);
    vp_log("stream info done at %.1fs", (osGetTime() - t_open) / 1000.0);
    if (r < 0) { snprintf(err, errlen, "cannot read stream info"); goto out; }
    c->vidx = av_find_best_stream(c->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    c->aidx = av_find_best_stream(c->fmt, AVMEDIA_TYPE_AUDIO, -1, c->vidx, NULL, 0);
    if (c->vidx < 0 && c->aidx < 0) { snprintf(err, errlen, "no audio or video found"); goto out; }
    for (unsigned i = 0; i < c->fmt->nb_streams; i++)
        if ((int)i != c->vidx && (int)i != c->aidx) c->fmt->streams[i]->discard = AVDISCARD_ALL;
    c->start_time = c->fmt->start_time != AV_NOPTS_VALUE ? (double)c->fmt->start_time / AV_TIME_BASE : 0;
    double duration = c->fmt->duration > 0 ? (double)c->fmt->duration / AV_TIME_BASE : 0;

    const char *vcodec = "none";
    if (c->vidx >= 0) {
        AVStream *st = c->fmt->streams[c->vidx];
        c->vpar = st->codecpar;
        c->vtb = av_q2d(st->time_base);
        vcodec = avcodec_get_name(c->vpar->codec_id);
        if (c->vpar->codec_id != AV_CODEC_ID_H264) {
            snprintf(err, errlen, "video codec %s is not supported (only H.264)", vcodec);
            goto out;
        }
        /* the New 3DS decoder's system module crashes on 1080p input (seen on hardware, takes the
         * whole console down), and 3DS Wi-Fi could not keep up with those bitrates anyway */
        if (c->vpar->height > 720 || c->vpar->width > 1920) {   /* widescreen 720p (e.g. 1696x720) allowed */
            snprintf(err, errlen, "only available in %dx%d here - too big for the 3DS", c->vpar->width, c->vpar->height);
            goto out;
        }
        if (video_setup(c)) { snprintf(err, errlen, "hardware video decoder unavailable"); goto out; }
    }
    if (c->aidx >= 0) {
        AVStream *st = c->fmt->streams[c->aidx];
        c->atb = av_q2d(st->time_base);
        const AVCodec *ac = avcodec_find_decoder(st->codecpar->codec_id);
        if (ac) {
            c->actx = avcodec_alloc_context3(ac);
            avcodec_parameters_to_context(c->actx, st->codecpar);
            if (avcodec_open2(c->actx, ac, NULL) < 0) { avcodec_free_context(&c->actx); }
        }
        if (!c->actx) { vp_log("audio codec %s not supported - playing without sound", avcodec_get_name(st->codecpar->codec_id)); c->aidx = -1; }
    }
    if (c->aidx >= 0) {
        c->rate = c->actx->sample_rate > 0 ? c->actx->sample_rate : 44100;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&c->swr, &stereo, AV_SAMPLE_FMT_S16, c->rate,
                                &c->actx->ch_layout, c->actx->sample_fmt, c->rate, 0, NULL) < 0 || swr_init(c->swr) < 0) {
            vp_log("audio resampler failed - no sound");
            c->aidx = -1;
        }
        c->aframe = av_frame_alloc();
    }
    if (c->aidx >= 0) {
        Result nr = ndspInit();
        if (R_FAILED(nr)) { vp_log("ndspInit failed %08lx (is sdmc:/3ds/dspfirm.cdc there?) - no sound", nr); c->aidx = -1; }
        else {
            c->ndsp_ok = 1;
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            ndspChnReset(AUD_CH);
            ndspChnSetInterp(AUD_CH, NDSP_INTERP_LINEAR);
            ndspChnSetRate(AUD_CH, c->rate);
            ndspChnSetFormat(AUD_CH, NDSP_FORMAT_STEREO_PCM16);
            float mix[12] = { 1.0f, 1.0f };
            ndspChnSetMix(AUD_CH, mix);
            c->wbdata = linearAlloc(NBUF * BUF_FRAMES * 4);
            for (int i = 0; i < NBUF; i++) {
                memset(&c->wb[i], 0, sizeof c->wb[i]);
                c->wb[i].data_pcm16 = c->wbdata + i * BUF_FRAMES * 2;
                c->wb[i].status = NDSP_WBUF_DONE;
            }
            ndspChnSetPaused(AUD_CH, true);
        }
    }
    vp_log("streams: video %s%s, audio %s, duration %.0fs", vcodec, c->vidx >= 0 ? "" : "",
           c->aidx >= 0 ? avcodec_get_name(c->actx->codec_id) : "none", duration);

    /* top screen: 16-bit, double buffered, 2D */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSet3D(false);
    clear_top();

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    treader = threadCreate(reader_thread, c, 96 * 1024, prio + 1, -2, false);
    tdec = threadCreate(decoder_thread, c, 96 * 1024, prio - 1, n3ds ? 2 : 1, false);
    if (tdec) vp_log("decoder thread on core %d", n3ds ? 2 : 1);
    if (!tdec) { tdec = threadCreate(decoder_thread, c, 96 * 1024, prio - 1, -2, false); vp_log("decoder thread on the app core (core 2 refused)"); }
    if (!treader || !tdec) { snprintf(err, errlen, "cannot start threads"); goto stop; }

    int paused = 0, started = 0;
    u64 wall0 = 0;
    double wall_base = 0, last_status = 0, shown_pts = 0;
    u64 last_bytes = 0, last_bytes_t = osGetTime();
    double kbps = 0;
    if (vp_start_at > 1 && (duration <= 0 || vp_start_at < duration - 2)) {   /* resume */
        vp_log("starting at %.0fs", vp_start_at);
        do_seek(c, vp_start_at);
        shown_pts = vp_start_at;
    }
    vp_start_at = 0;
    while (aptMainLoop()) {
        u64 it0 = svcGetSystemTick();
        c->loops++;
        hidScanInput();
        u32 k = hidKeysDown();
        if (g_ui && g_ui->tick) g_ui->tick(k, shown_pts);
        if ((k & KEY_B) || vp_quit_req) break;
        if (k & KEY_A) {
            paused = !paused;
            if (c->ndsp_ok) ndspChnSetPaused(AUD_CH, paused || !started);
            if (!paused && c->aidx < 0) { wall0 = osGetTime(); wall_base = shown_pts; }
        }
        if ((k & (KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) && duration > 0) {
            double step = (k & (KEY_L | KEY_R)) ? 60 : 10;
            double tgt = shown_pts + ((k & (KEY_RIGHT | KEY_R)) ? step : -step);
            if (tgt > duration - 1) tgt = duration - 1;
            if (c->ndsp_ok) ndspChnSetPaused(AUD_CH, true);
            do_seek(c, tgt);
            started = 0;
            shown_pts = tgt;
        }

        /* prebuffer before starting (and after a seek / underrun) */
        LightLock_Lock(&c->lock);
        label_queue(c, 2);
        LightLock_Unlock(&c->lock);
        int vready = c->vidx < 0 || c->r_count >= 2 || c->decoder_done;
        int aready = c->aidx < 0 || audio_ahead(c) > 0.4 || c->decoder_done;
        if (!started && vready && aready && !paused) {
            started = 1;
            if (c->ndsp_ok) ndspChnSetPaused(AUD_CH, false);
            wall0 = osGetTime();
            wall_base = c->r_count ? c->ring_pts[c->r_head] : shown_pts;
        }
        double clock;
        if (c->aidx >= 0) clock = audio_clock(c);
        else clock = (started && !paused) ? wall_base + (osGetTime() - wall0) / 1000.0 : wall_base;
        int buffering = !started || (c->aidx >= 0 && audio_ahead(c) == 0 && !c->decoder_done);
        if (vp_debug && started && !paused) {   /* clock step per loop */
            double dc = clock - c->prev_clock;
            if (dc < c->dc_min) c->dc_min = dc;
            if (dc > c->dc_max) c->dc_max = dc;
        }
        c->prev_clock = clock;

        /* show the newest frame that is due (late frames are dropped) */
        if (started && c->vidx >= 0) {
            int show = -1;
            LightLock_Lock(&c->lock);
            label_queue(c, 2);
            while (c->r_count > 1 && c->ring_pts[(c->r_head + 1) % VRING] <= clock + 0.02) {
                c->r_head = (c->r_head + 1) % VRING;
                c->r_count--;
                label_queue(c, 2);
                c->skipped++;          /* late frame dropped */
            }
            if (c->r_count > 0 && (c->ring_pts[c->r_head] <= clock + 0.02 || c->ring_pts[c->r_head] < 0))
                show = c->r_head;
            LightLock_Unlock(&c->lock);
            if (show >= 0) {
                u64 b0 = svcGetSystemTick();
                blit(c, c->ring[show]);
                c->shown++;
                {   /* frame counter for the bottom screen */
                    u32 h = frame_hash(c, c->ring[show]);
                    c->fc_drawn++;
                    if (h != c->last_hash) c->fc_new++;
                    c->last_hash = h;
                }
                if (vp_debug && c->nsig < SIG_N) {
                    frame_sig(c, c->ring[show], c->sig[c->nsig]);
                    c->sig_pts[c->nsig] = c->ring_pts[show];
                    c->nsig++;   /* dumped when playback ends */
                }
                if (g_ui && g_ui->overlay) g_ui->overlay((u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), c->ring_pts[show] >= 0 ? c->ring_pts[show] : shown_pts);
                if (c->ring_pts[show] >= 0) shown_pts = c->ring_pts[show];
                LightLock_Lock(&c->lock);
                c->r_head = (c->r_head + 1) % VRING;
                c->r_count--;
                LightLock_Unlock(&c->lock);
                gfxFlushBuffers();
                gfxSwapBuffers();
                u64 bt = svcGetSystemTick() - b0;
                c->blit_sum += bt; c->nblit++;
                if (bt > c->blit_max) c->blit_max = bt;
            }
        } else if (c->vidx < 0) shown_pts = clock;

        /* finished? */
        if (c->decoder_done && c->r_count == 0 && (c->aidx < 0 || audio_ahead(c) == 0)) {
            if (c->reader_err) { char e[96]; av_strerror(c->reader_err, e, sizeof e); snprintf(err, errlen, "stream error: %s", e); }
            break;
        }

        u64 now = osGetTime();
        if (now - last_bytes_t >= 1000) {
            kbps = (vp_net_stats.bytes - last_bytes) * 8.0 / (now - last_bytes_t);
            last_bytes = vp_net_stats.bytes;
            last_bytes_t = now;
        }
        static u64 last_dbg;
        if (vp_debug && now - last_dbg >= 2000) {
            last_dbg = now;
            vp_log("dbg: started=%d ring=%d vq=%d aq=%d ahead=%.2f phase=%d frames=%d shown=%d skip=%d rfail=%d nproc=%d r=%08lx rr=%08lx pos=%.1f",
                   started, c->r_count, c->vq.count, c->aq.count, c->aidx >= 0 ? audio_ahead(c) : -1.0, c->dphase,
                   c->frames_out, c->shown, c->skipped, c->render_fail, c->nproc, c->last_r, c->last_rr, shown_pts);
            const double tms = SYSCLOCK_ARM11 / 1000.0;
            vp_log("dbg2: loops=%d work avg %.1f max %.1f ms | draw n=%d avg %.1f max %.1f ms | vblank wait avg %.1f ms",
                   c->loops, c->loops ? c->work_sum / tms / c->loops : 0, c->work_max / tms,
                   c->nblit, c->nblit ? c->blit_sum / tms / c->nblit : 0, c->blit_max / tms,
                   c->loops ? c->vbl_sum / tms / c->loops : 0);
            vp_log("dbg3: frame pts gap min %.1f max %.1f ms | clock step per loop min %.1f max %.1f ms",
                   c->dp_min * 1000, c->dp_max * 1000, c->dc_min * 1000, c->dc_max * 1000);
            vp_log("dbg4: mvd pictures: %d while processing, %d by render, %d drained afterwards (packets %d)",
                   c->n_proc_out, c->n_render_out, c->n_drain_out, c->nproc);
            c->dp_min = c->dc_min = 1e9; c->dp_max = c->dc_max = -1e9;
            c->loops = c->nblit = 0;
            c->work_sum = c->work_max = c->blit_sum = c->blit_max = c->vbl_sum = 0;
        }
        if (now - c->fps_t >= 1000) {
            double k = c->fps_t ? 1000.0 / (now - c->fps_t) : 0;
            c->fps_drawn = (int)(c->fc_drawn * k + 0.5);
            c->fps_new = (int)(c->fc_new * k + 0.5);
            c->fps_dec = (int)((c->frames_out - c->fc_dec0) * k + 0.5);
            c->fc_drawn = c->fc_new = 0; c->fc_dec0 = c->frames_out; c->fps_t = now;
        }
        if (g_ui && g_ui->status && now - last_status >= 250) {
            vp_state s = { shown_pts, duration, pq_span(c, &c->vq, c->vtb), buffering, paused,
                           c->vpar ? c->vpar->width : 0, c->vpar ? c->vpar->height : 0, vcodec, kbps };
            s.fps_drawn = c->fps_drawn; s.fps_new = c->fps_new; s.fps_dec = c->fps_dec;
            g_ui->status(title, &s);
            last_status = now;
        }
        u64 wt = svcGetSystemTick() - it0;
        c->work_sum += wt;
        if (wt > c->work_max) c->work_max = wt;
        u64 v0 = svcGetSystemTick();
        gspWaitForVBlank();
        c->vbl_sum += svcGetSystemTick() - v0;
    }
    ret = 0;
stop:
    c->quit = 1;
    vp_net_abort = 1;
    if (treader) { threadJoin(treader, U64_MAX); threadFree(treader); }
    if (tdec) { threadJoin(tdec, U64_MAX); threadFree(tdec); }
out:
    vp_net_abort = 1;
    if (c->ndsp_ok) { ndspChnWaveBufClear(AUD_CH); ndspChnReset(AUD_CH); ndspExit(); }
    if (c->fmt) avformat_close_input(&c->fmt);
    vp_net_abort = 0;
    pq_clear(c, &c->vq); pq_clear(c, &c->aq);
    if (c->actx) avcodec_free_context(&c->actx);
    if (c->swr) swr_free(&c->swr);
    if (c->aframe) av_frame_free(&c->aframe);
    if (c->mvd_ok) mvdstdExit();
    if (c->mvd_in) linearFree(c->mvd_in);
    if (vp_debug && c->nsig == SIG_N)
        for (int i = 0; i < SIG_N; i++) {
            char h[40];
            for (int k = 0; k < 16; k++) snprintf(h + k * 2, 3, "%02x", c->sig[i][k]);
            vp_log("sig %d %.3f %s", i, c->sig_pts[i], h);
        }
    if (c->mvd_out) linearFree(c->mvd_out);
    if (c->wbdata) linearFree(c->wbdata);
    for (int i = 0; i < VRING; i++) free(c->ring[i]);
    free(c->xmap); free(c->ymap);
    free(open_url);
    free(c);
    gfxSetScreenFormat(GFX_TOP, old_fmt);
    gfxSet3D(old_3d);
    if (err[0]) { vp_log("error: %s", err); ret = -1; }
    return ret;
}
