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
#include <citro2d.h>
#include "vp.h"
#include "overlay.h"

#define TOP_W 400
#define TOP_H 240
#define SIG_N 240
#define VRING 6                 /* decoded frames waiting for display */
#define NBUF 20                 /* NDSP wave buffers */
#define BUF_FRAMES 2048         /* stereo s16 frames per wave buffer */
#define PKT_OVERHEAD 256        /* heap per queued packet besides its data: AVPacket, buffer refs, padding, node */
#define MEM_RESERVE (16u << 20) /* heap left free while buffering: ffmpeg/hls, download rings, fragmentation */
#define MEM_MIN (4u << 20)      /* smallest buffer budget */
#define AUD_CH 0

/* free bytes in the app heap: what malloc has not taken from the system yet plus its free lists */
extern u32 __ctru_heap_size;
static size_t heap_free(void)
{
    struct mallinfo mi = mallinfo();
    size_t used = mi.arena > mi.fordblks ? mi.arena - mi.fordblks : 0;
    return used < __ctru_heap_size ? __ctru_heap_size - used : 0;
}

static const vp_ui *g_ui;
void vp_set_ui(const vp_ui *ui) { g_ui = ui; }
volatile int vp_quit_req;
int vp_max_h = 480;   /* hls: largest variant height to prefer (smallest variant if none fits) */
double vp_start_at;
int vp_wide;

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
    volatile int quit, reader_eof, decoder_done, seek_req, reader_parked, decoder_parked, audio_parked;
    double seek_to;
    int reader_err;
    pqueue vq, aq;              /* read ahead, not decoded yet */
    pqueue pv, pa;              /* already decoded, kept for seeking back without the network */
    size_t cap_ahead, cap_past; /* byte budgets of the two (from the free heap at start) */

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
    int scr_w;                  /* top screen width: 400, or 800 in wide mode */
    int pic_w, pic_h;           /* the picture inside the decoded frame (MVD may shrink it) */
    u16 *xmap, *ymap;
    u8 *mvd_in; size_t mvd_in_cap;
    u8 *mvd_out;
    MVDSTD_Config cfg;
    double pts_list[48]; int npts;
    u16 *ring[VRING]; double ring_pts[VRING]; u32 ring_hash[VRING];
    int nslot;                  /* ring slots allocated (<= VRING); one stays free for the GPU */
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

    /* GPU display (like Core-2-Extreme's player): the full-size decoded frame is copied into
     * tiled RGB565 textures (at most 1024x1024 each) and drawn scaled with linear filtering */
    C3D_RenderTarget *target;
    int ntx, nty, tx[4], ty[4], tw[4], th[4];       /* tile grid over the padded picture */
    C3D_Tex rtex[16];           /* the frame on screen, rearranged by the GPU's transfer engine */
    u16 *stage;                 /* tile columns cut out of the frame (contiguous), per tile */
    Tex3DS_SubTexture rsub[16];
    int stex_ok, srows, gpu_ok;
    u64 cp_sum, cp_max, dec_sum, dec_max; int ncp, ndec;   /* debug timing: frame copy, packet decode */
    C3D_Tex stex;               /* subtitle text, 512x64 RGBA8 */
} vp_ctx;

static void pq_push(vp_ctx *c, pqueue *q, AVPacket *p)
{
    pnode *n = malloc(sizeof *n);
    n->p = p; n->next = NULL;
    LightLock_Lock(&c->lock);
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    q->count++;
    q->bytes += p->size + PKT_OVERHEAD;
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
        q->bytes -= n->p->size + PKT_OVERHEAD;
        p = n->p;
        free(n);
    }
    LightLock_Unlock(&c->lock);
    return p;
}
/* packet time in seconds on the player's clock (pts, else dts) */
static double pkt_time(const vp_ctx *c, const AVPacket *p, double tb)
{
    int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
    return ts != AV_NOPTS_VALUE ? ts * tb - c->start_time : -1;
}
/* keep a decoded packet for seeking back; the oldest kept ones go when over budget */
static void past_keep(vp_ctx *c, pqueue *q, AVPacket *p)
{
    pq_push(c, q, p);
    for (;;) {
        LightLock_Lock(&c->lock);
        int over = c->pv.bytes + c->pa.bytes > c->cap_past;
        pqueue *o = NULL;
        if (over) {   /* drop from whichever stream reaches further back */
            if (!c->pa.head) o = &c->pv;
            else if (!c->pv.head) o = &c->pa;
            else o = pkt_time(c, c->pv.head->p, c->vtb) <= pkt_time(c, c->pa.head->p, c->atb) ? &c->pv : &c->pa;
        }
        LightLock_Unlock(&c->lock);
        if (!o) break;
        AVPacket *d = pq_pop(c, o);
        if (!d) break;
        av_packet_free(&d);
    }
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
            if (t + d <= c->audio_from) { past_keep(c, &c->pa, p); return 1; }
        }
        c->audio_from = -1e9;
    }
    audio_decode(c, p);
    past_keep(c, &c->pa, p);
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

/* power-of-two pieces (8..1024) that add up exactly to n (n is a multiple of 16) */
static int split_pow2(int n, int *pos, int *len)
{
    int k = 0, at = 0;
    while (n > 0 && k < 4) {
        int p = 1024;
        while (p > n) p >>= 1;
        if (k == 3) { p = 8; while (p < n) p <<= 1; }   /* last piece covers the rest */
        pos[k] = at; len[k] = p; k++;
        at += p; n -= p;
    }
    return k;
}

/* tiled ("Morton") texture layout: u32 index of a pixel pair inside an 8x8 tile row */
static u16 g_xoff[512];
static void swz_init(void)
{
    for (int i = 0; i < 512; i++) {
        int x = i * 2;
        int m = (x & 1) | ((x & 2) << 1) | ((x & 4) << 2);
        g_xoff[i] = ((x >> 3) * 64 + m) / 2;
    }
}
static inline int swz_row(int y, int tw)
{
    return ((y >> 3) * (tw >> 3) * 64 + (((y & 1) << 1) | ((y & 2) << 2) | ((y & 4) << 3))) / 2;
}
/* copy a w x h block (multiples of 8) of a linear RGB565 picture into a tiled texture of width w:
 * tile by tile, so the texture is written front to back in whole 128-byte tiles */
static void swz_copy(u16 *dst, int tw, const u16 *src, int stride, int w, int h)
{
    (void)tw;
    u32 *d = (u32 *)dst;
    const int sw = stride / 2;           /* u32 per source row */
    for (int y0 = 0; y0 < h; y0 += 8) {
        const u32 *band = (const u32 *)(src + y0 * stride);
        for (int x0 = 0; x0 < w; x0 += 8, d += 32) {
            const u32 *s0 = band + x0 / 2;
            /* Morton order in u32 pairs: rows (0,1) (2,3) .. each pair of rows fills 4 u32 per 4 px */
            for (int r = 0; r < 8; r += 2) {
                const u32 *a = s0 + r * sw, *b = a + sw;
                u32 *t = d + ((r & 2) ? 4 : 0) + ((r & 4) ? 16 : 0);
                t[0] = a[0]; t[1] = b[0]; t[2] = a[1]; t[3] = b[1];
                t[8] = a[2]; t[9] = b[2]; t[10] = a[3]; t[11] = b[3];
            }
        }
    }
}

static int video_setup(vp_ctx *c)
{
    int w = c->vpar->width, h = c->vpar->height;
    c->pw = round_up16(w); c->ph = round_up16(h);
    /* citro2d draws in 400x240 coordinates on the top screen also in wide mode (the 800 px
     * target just has twice the pixels across), so the layout is the same in both */
    int xs = 1;
    double s = fmin((double)TOP_W / w, (double)TOP_H / h);
    c->vis_w = (int)(w * s * xs) & ~1; c->vis_h = (int)(h * s) & ~1;
    if (c->vis_w > TOP_W) c->vis_w = TOP_W;
    if (c->vis_h > TOP_H) c->vis_h = TOP_H;
    c->ox = (TOP_W - c->vis_w) / 2; c->oy = (TOP_H - c->vis_h) / 2;
    /* MVD decodes at full size up to 1024 px across: its scaler drops whole lines (stair steps,
     * rows one step above black in dark scenes when it shrinks a lot), so the GPU scales instead,
     * with filtering. Wider video (720p and up) is shrunk mildly by MVD to 1024 across first:
     * full-size 1280x720 frames are too much for the decoder and the GPU's copies to keep 25 fps,
     * and at 0.8x MVD drops only every 5th line, which the GPU's averaging then hides. */
    if (c->pw > 1024) {
        double f = 1024.0 / c->pw;
        c->ow = 1024;
        c->oh = (int)(c->ph * f) & ~15;
        c->pic_w = (int)(w * f); c->pic_h = (int)(h * f);
        if (c->pic_h > c->oh) c->pic_h = c->oh;
    } else {
        c->ow = c->pw; c->oh = c->ph;
        c->pic_w = w; c->pic_h = h;
    }

    Result r = -1;
    for (int mb = 14; mb >= 5 && r; mb--)
        r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, mb * 1000000, NULL);
    if (r) { vp_log("mvdstdInit failed: %08lx (linear free %lu KB)", r, linearSpaceFree() / 1024); return -1; }
    c->mvd_ok = 1;
    c->cpu_scale = 1;   /* (debug maps sample the full-size frame) */
    c->src_w = c->ow; c->src_h = c->oh;
    swz_init();
    c->ntx = split_pow2(c->ow, c->tx, c->tw);
    c->nty = split_pow2(c->oh, c->ty, c->th);
    c->nslot = 0;
    /* frames in linear memory: MVD decodes straight into the next free one and the GPU's transfer
     * engine reads them (+4 KB: a tile transfer may read a little past the last row) */
    for (int i = 0; i < VRING; i++) {
        c->ring[i] = linearAlloc(c->ow * c->oh * 2 + 4096);
        if (!c->ring[i]) break;
        c->nslot++;
    }
    c->mvd_out = (u8 *)c->ring[0];
    c->stage = linearAlloc(c->ow * c->oh * 2);
    if (!c->stage) { vp_log("no linear memory for the tile buffer"); return -1; }
    {
        int n = 0;
        for (int a = 0; a < c->nty; a++)
            for (int b = 0; b < c->ntx; b++, n++) {
                C3D_Tex *t = &c->rtex[n];
                if (!C3D_TexInit(t, c->tw[b], c->th[a], GPU_RGB565)) { vp_log("no linear memory for the picture"); return -1; }
                C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
                C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            }
    }
    if (c->nslot < 3) { vp_log("no memory for frames (%d slots, linear free %lu KB)", c->nslot, linearSpaceFree() / 1024); return -1; }
    {   /* what of each tile is picture (the rest is the 16px padding) */
        int w = c->pic_w, h = c->pic_h, n = 0;
        for (int a = 0; a < c->nty; a++)
            for (int b = 0; b < c->ntx; b++, n++) {
                int uw = w - c->tx[b], uh = h - c->ty[a];
                if (uw > c->tw[b]) uw = c->tw[b];
                if (uh > c->th[a]) uh = c->th[a];
                if (uw < 0) uw = 0;
                if (uh < 0) uh = 0;
                Tex3DS_SubTexture *st = &c->rsub[n];
                st->width = uw; st->height = uh;
                st->left = 0; st->top = 1.0f;
                st->right = (float)uw / c->tw[b];
                st->bottom = 1.0f - (float)uh / c->th[a];
            }
    }
    mvdstdGenerateDefaultConfig(&c->cfg, c->pw, c->ph, c->ow, c->oh, NULL, NULL, NULL);
    c->mvd_first = 1;
    c->need_key = 1;
    c->avcc = c->vpar->extradata_size > 7 && c->vpar->extradata[0] == 1;
    c->nal_len = c->avcc ? (c->vpar->extradata[4] & 3) + 1 : 4;
    build_maps(c);
    vp_log("video %dx%d -> %dx%d on screen (mvd out %dx%d, %dx%d tiles, %d slots, %s)", w, h, c->vis_w, c->vis_h, c->ow, c->oh,
           c->ntx, c->nty, c->nslot, c->avcc ? "mp4/avcC" : "annex-b");
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
        int i = (c->r_head + k) % c->nslot;
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
    u32 step = n / 4096 ? n / 4096 : 1;
    for (u32 i = 0; i < n; i += step) { h ^= w[i]; h *= 16777619u; }
    return h;
}

/* marker pixels in the output corners: changed = MVD wrote a picture */
#define MVD_MARK 0x1111
static void mark_lines(vp_ctx *c, int flush)
{
    u16 *o = (u16 *)c->mvd_out; int w = c->src_w, h = c->src_h;
    u16 *px[4] = { o, o + w - 1, o + (h - 1) * w, o + h * w - 1 };
    for (int i = 0; i < 4; i++) {
        u32 a = (u32)px[i] & ~31u;
        if (flush) svcFlushProcessDataCache(CUR_PROCESS_HANDLE, a, 32);
        else svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, a, 32);
    }
}
static void mvd_target(vp_ctx *c)
{
    LightLock_Lock(&c->lock);
    int slot = (c->r_head + c->r_count) % c->nslot;
    LightLock_Unlock(&c->lock);
    c->mvd_out = (u8 *)c->ring[slot];
    c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->mvd_out);
}
static void mvd_mark(vp_ctx *c)
{
    mvd_target(c);
    u16 *o = (u16 *)c->mvd_out; int w = c->src_w, h = c->src_h;
    o[0] = o[w - 1] = o[(h - 1) * w] = o[h * w - 1] = MVD_MARK;
    mark_lines(c, 1);
}
static int mvd_written(vp_ctx *c)
{
    u16 *o = (u16 *)c->mvd_out; int w = c->src_w, h = c->src_h;
    mark_lines(c, 0);
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
    if (c->quit || c->seek_req) return 0;
    int slot = 0;
    while (slot < c->nslot - 1 && c->ring[slot] != (u16 *)c->mvd_out) slot++;
    {   /* a few points of the picture for the 'new frames' counter */
        u16 *o = (u16 *)c->mvd_out;
        u32 n = c->src_w * c->src_h, h = 2166136261u;
        for (int k = 1; k < 16; k++) {
            u16 *px = o + n / 16 * k + c->src_w / 2;
            svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)px & ~31u, 32);
            h ^= *px; h *= 16777619u;
        }
        c->ring_hash[slot] = h;
    }
    /* no timestamp yet: label_queue gives the smallest pending pts when the display takes it */
    LightLock_Lock(&c->lock);
    c->ring_pts[slot] = PTS_UNSET;
    c->r_count++;
    LightLock_Unlock(&c->lock);
    if (c->frames_out++ == 0) vp_log("first frame decoded");
    /* the next picture needs a free slot that is not the one on screen: wait for room.
     * Keep the sound going meanwhile: after a seek the audio starts at the target but the video
     * only at the next keyframe, and the display takes frames only when the audio clock reaches
     * them (the audio has its own thread) */
    for (;;) {
        LightLock_Lock(&c->lock);
        int full = c->r_count >= c->nslot - 1;
        LightLock_Unlock(&c->lock);
        if (!full || c->quit || c->seek_req) break;
        svcSleepThread(3000000);
    }
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
        if (bytes > c->cap_ahead && (c->aidx < 0 || acount > 30)) { svcSleepThread(10000000); continue; }
        /* the budget is an estimate: also stop reading if the heap itself gets low */
        static int nread, low;
        if (++nread % 128 == 0 || low) low = heap_free() < (6u << 20);
        if (low && bytes > (2u << 20)) { svcSleepThread(50000000); continue; }
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
        if (c->vidx >= 0 && c->mvd_ok) {
            LightLock_Lock(&c->lock);
            int room = c->r_count < c->nslot - 1;
            LightLock_Unlock(&c->lock);
            if (room) {
                AVPacket *p = pq_pop(c, &c->vq);
                if (p && c->key_wait && (p->flags & AV_PKT_FLAG_KEY)) {   /* first keyframe after a seek */
                    int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
                    c->audio_from = ts != AV_NOPTS_VALUE ? ts * c->vtb - c->start_time : -1e9;
                    c->key_wait = 0;
                }
                if (p) {
                    c->dphase = 3;
                    u64 d0 = svcGetSystemTick(), cp_before = c->cp_sum;
                    video_decode(c, p);
                    u64 dt = svcGetSystemTick() - d0 - (c->cp_sum - cp_before);   /* without the frame copies */
                    c->dec_sum += dt; c->ndec++;
                    if (dt > c->dec_max) c->dec_max = dt;
                    past_keep(c, &c->pv, p); did = 1; c->dphase = 0;
                }
            }
        }
        if (!did) {
            LightLock_Lock(&c->lock);
            int empty = !c->vq.count && !c->aq.count;
            LightLock_Unlock(&c->lock);
            if (c->reader_eof && empty) c->decoder_done = 1;
            svcSleepThread(2000000);
        }
    }
}

/* audio has its own thread (as in Core-2-Extreme's player): copying full-size video frames on
 * the decoder thread can never delay the sound */
static void audio_thread(void *arg)
{
    vp_ctx *c = arg;
    while (!c->quit) {
        if (c->seek_req) { c->audio_parked = 1; svcSleepThread(3000000); continue; }
        c->audio_parked = 0;
        if (!c->key_wait && audio_free_slots(c) >= 3 && audio_step(c)) continue;
        LightLock_Lock(&c->lock);
        int empty = !c->aq.count;
        LightLock_Unlock(&c->lock);
        if (c->reader_eof && empty && c->fill) audio_submit(c);
        svcSleepThread(2000000);
    }
}

/* ------------------------------------------------------------------ display */
static void sub_upload(vp_ctx *c, const u8 *m, int rows)
{
    u32 *d = (u32 *)c->stex.data;
    memset(d, 0, 512 * 64 * 4);
    for (int y = 0; y < rows && y < 64; y++) {
        int rb = (y >> 3) * 64 * 64 + (((y & 1) << 1) | ((y & 2) << 2) | ((y & 4) << 3));
        for (int x = 0; x < 400; x++) {
            u8 v = m[y * 400 + x];
            if (!v) continue;
            int i = rb + (x >> 3) * 64 + ((x & 1) | ((x & 2) << 1) | ((x & 4) << 2));
            d[i] = v == 2 ? 0xFFFFFFFF : 0x000000FF;
        }
    }
    C3D_TexFlush(&c->stex);
}

/* one new picture: the frame's tiles scaled to the screen with linear filtering, subtitles on top */
static void gpu_draw(vp_ctx *c, int slot, double pts)
{
    C3D_FrameBegin(0);   /* waits until the last frame is drawn (textures free); no extra VBlank wait */
    {   /* linear frame -> tiled textures, all by the GPU's transfer engine (no CPU copy), queued
         * ahead of this frame's drawing: TextureCopy cuts each tile's columns out of the frame
         * into a contiguous block, DisplayTransfer converts that block to the tiled layout */
        const u16 *src = c->ring[slot];
        u16 *st = c->stage;
        int n = 0;
        for (int a = 0; a < c->nty; a++)
            for (int b = 0; b < c->ntx; b++, n++) {
                if (!c->rsub[n].width || !c->rsub[n].height) continue;
                int tw = c->tw[b], rows = (c->rsub[n].height + 7) & ~7;
                const u16 *in = src + c->ty[a] * c->src_w + c->tx[b];
                const u16 *blk = in;
                if (tw != c->src_w) {
                    GX_TextureCopy((u32 *)in, GX_BUFFER_DIM(tw * 2 / 16, (c->src_w - tw) * 2 / 16),
                                   (u32 *)st, GX_BUFFER_DIM(tw * 2 / 16, 0), tw * rows * 2, GX_TRANSFER_RAW_COPY(1));
                    blk = st;
                }
                GX_DisplayTransfer((u32 *)blk, GX_BUFFER_DIM(tw, rows), (u32 *)c->rtex[n].data, GX_BUFFER_DIM(tw, rows),
                    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
                st += tw * rows;
            }
    }
    C2D_TargetClear(c->target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(c->target);
    float sx = (float)c->vis_w / c->pic_w, sy = (float)c->vis_h / c->pic_h;
    /* plain bilinear filtering only looks at 2x2 source pixels per screen pixel: shrinking 2-3x
     * that aliases (grain and dark gradients turn into dotted patterns). Average four bilinear
     * taps half a source pixel apart instead (~3x3 pixel footprint, like a small box filter). */
    int taps = (sx < 0.9f || sy < 0.9f) ? 4 : 1;
    for (int k = 0; k < taps; k++) {
        float dx = taps > 1 ? ((k & 1) ? 0.5f : -0.5f) * sx : 0;
        float dy = taps > 1 ? ((k & 2) ? 0.5f : -0.5f) * sy : 0;
        C2D_ImageTint tint;
        C2D_AlphaImageTint(&tint, 1.0f / (k + 1));    /* running average: 1, 1/2, 1/3, 1/4 */
        int n = 0;
        for (int a = 0; a < c->nty; a++)
            for (int b = 0; b < c->ntx; b++, n++) {
                if (!c->rsub[n].width || !c->rsub[n].height) continue;
                C2D_Image img = { &c->rtex[n], &c->rsub[n] };
                C2D_DrawImageAt(img, c->ox + c->tx[b] * sx + dx, c->oy + c->ty[a] * sy + dy, 0.5f + k * 0.01f,
                                k ? &tint : NULL, sx, sy);
            }
    }
    if (c->stex_ok && g_ui && g_ui->sub_mask) {
        int rows = 0, changed = 0;
        const u8 *m = g_ui->sub_mask(pts, &rows, &changed);
        if (rows > 64) rows = 64;
        if (changed) sub_upload(c, m, rows);
        if (m && rows > 0) {
            Tex3DS_SubTexture st = { 400, (u16)rows, 0, 1.0f, 400.0f / 512, 1.0f - rows / 64.0f };
            C2D_Image img = { &c->stex, &st };
            C2D_DrawImageAt(img, 0, TOP_H - 4 - rows, 0.6f, NULL, 1, 1);
        }
    }
    C3D_FrameEnd(0);
}
static void clear_top(int scr_w)
{
    for (int i = 0; i < 2; i++) {
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        memset(fb, 0, scr_w * TOP_H * 3);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

/* ------------------------------------------------------------------ seek */
/* join kept + queued packets of one stream (they are consecutive) and split them again at `at`:
 * everything before it becomes the kept part, `at` and after the queue. Lock held. */
static void requeue(pqueue *past, pqueue *q, pnode *at)
{
    pnode *all = past->head;
    if (past->tail) past->tail->next = q->head; else all = q->head;
    pnode *tail = q->tail ? q->tail : past->tail;
    int cnt = past->count + q->count;
    size_t bytes = past->bytes + q->bytes;
    memset(past, 0, sizeof *past);
    for (pnode *n = all; n && n != at; n = n->next) {
        if (!past->head) past->head = n;
        past->tail = n;
        past->count++;
        past->bytes += n->p->size + PKT_OVERHEAD;
    }
    if (past->tail) past->tail->next = NULL;
    q->head = at;
    q->tail = at ? tail : NULL;
    q->count = cnt - past->count;
    q->bytes = bytes - past->bytes;
}

/* seek inside the packets in memory (decoder parked). Starts at the last keyframe before the
 * target (late frames are skipped by the display) unless that is more than 3 s back and a later
 * keyframe is there. 0 = the target is not in memory. */
static int seek_in_memory(vp_ctx *c, double target, double *audio_start)
{
    if (c->vidx < 0) return 0;
    LightLock_Lock(&c->lock);
    pnode *kb = NULL, *ka = NULL;
    double tb = 0, ta = 0, last = -1;
    pqueue *qs[2] = { &c->pv, &c->vq };
    for (int k = 0; k < 2; k++)
        for (pnode *n = qs[k]->head; n; n = n->next) {
            double t = pkt_time(c, n->p, c->vtb);
            if (t > last) last = t;
            if (!(n->p->flags & AV_PKT_FLAG_KEY) || t < 0) continue;
            if (t <= target) { kb = n; tb = t; }
            else if (!ka) { ka = n; ta = t; }
        }
    /* the target has to be covered: some read-ahead after it, or the whole file read */
    /* a keyframe at or shortly after the target; one far after it means the target itself was
     * already dropped from memory (seeking back after a long skip): load it from the network */
    int ok = (kb || (ka && ta - target <= 4.0)) && (last >= target + 1.0 || (c->reader_eof && last >= target));
    pnode *at = NULL;
    double audio_from = target;
    if (ok) {
        if (kb && (target - tb <= 3.0 || !ka)) at = kb;
        else { at = ka; audio_from = ta; }
        requeue(&c->pv, &c->vq, at);
        pnode *aat = NULL;   /* audio: from the first packet still sounding at audio_from */
        pqueue *as[2] = { &c->pa, &c->aq };
        for (int k = 0; k < 2 && !aat; k++)
            for (pnode *n = as[k]->head; n; n = n->next) {
                double t = pkt_time(c, n->p, c->atb), d = n->p->duration > 0 ? n->p->duration * c->atb : 0;
                if (t < 0 || t + d > audio_from) { aat = n; break; }
            }
        requeue(&c->pa, &c->aq, aat);
    }
    LightLock_Unlock(&c->lock);
    if (!ok) return 0;
    *audio_start = audio_from;
    vp_log("seek %.1fs from memory (keyframe %.1fs)", target, at == kb ? tb : ta);
    return 1;
}

static void do_seek(vp_ctx *c, double target)
{
    if (target < 0) target = 0;
    c->seek_req = 1;
    for (int i = 0; i < 300 && !(c->decoder_parked && c->audio_parked); i++) svcSleepThread(10000000);
    double audio_start = -1e9;
    int mem = seek_in_memory(c, target, &audio_start);
    if (!mem) {
        vp_net_abort = 1;                 /* unblock a reader waiting on the network */
        for (int i = 0; i < 300 && !(c->reader_parked && c->decoder_parked && c->audio_parked); i++) svcSleepThread(10000000);
        vp_net_abort = 0;
        pq_clear(c, &c->vq); pq_clear(c, &c->aq);
        pq_clear(c, &c->pv); pq_clear(c, &c->pa);
        int64_t ts = (int64_t)((target + c->start_time) * AV_TIME_BASE);
        int r = avformat_seek_file(c->fmt, -1, INT64_MIN, ts, ts, 0);
        if (r < 0) vp_log("seek failed (%d)", r);
        pq_clear(c, &c->vq); pq_clear(c, &c->aq);
    }
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
    /* from the network the first keyframe is not known yet: audio waits for it; from memory the
     * start was chosen above */
    c->key_wait = !mem && c->aidx >= 0 && c->vidx >= 0 && c->mvd_ok;
    c->audio_from = mem ? audio_start : -1e9;
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
    Thread treader = NULL, tdec = NULL, taud = NULL;
    char *open_url = NULL;
    GSPGPU_FramebufferFormat old_fmt = gfxGetScreenFormat(GFX_TOP);
    bool old_3d = gfxIs3D();
    err[0] = 0;
    LightLock_Init(&c->lock);
    c->vidx = c->aidx = -1;
    c->audio_from = -1e9;
    c->scr_w = vp_wide ? 2 * TOP_W : TOP_W;
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

    {   /* use the free heap for packets: 3/4 read ahead, 1/4 kept behind for seeking back */
        size_t fr = heap_free();
        size_t budget = fr > MEM_RESERVE + MEM_MIN ? fr - MEM_RESERVE : MEM_MIN;
        c->cap_ahead = budget / 4 * 3;
        c->cap_past = budget - c->cap_ahead;
        vp_log("memory: heap %lu/%lu KB free, linear %lu KB free -> buffer %lu KB ahead, %lu KB behind",
               (unsigned long)(fr >> 10), (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(linearSpaceFree() >> 10),
               (unsigned long)(c->cap_ahead >> 10), (unsigned long)(c->cap_past >> 10));
    }
    /* top screen: double buffered, 2D, drawn by the GPU */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);   /* citro2d renders 8 bits per channel: no 565 banding */
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSet3D(false);
    gfxSetWide(c->scr_w > TOP_W);
    clear_top(c->scr_w);
    if (!c->gpu_ok) {
        C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
        C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
        C2D_Prepare();
        /* (C2D_CreateScreenTarget is always 400 wide) */
        c->target = C3D_RenderTargetCreate(TOP_H, c->scr_w, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (c->target)
            C3D_RenderTargetSetOutput(c->target, GFX_TOP, GFX_LEFT,
                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
        c->gpu_ok = 1;
    }
    if (C3D_TexInit(&c->stex, 512, 64, GPU_RGBA8)) {
        C3D_TexSetFilter(&c->stex, GPU_NEAREST, GPU_NEAREST);
        memset(c->stex.data, 0, 512 * 64 * 4);
        C3D_TexFlush(&c->stex);
        c->stex_ok = 1;
    }

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    treader = threadCreate(reader_thread, c, 96 * 1024, prio + 1, -2, false);
    tdec = threadCreate(decoder_thread, c, 96 * 1024, prio - 1, n3ds ? 2 : 1, false);
    if (tdec) vp_log("decoder thread on core %d", n3ds ? 2 : 1);
    if (!tdec) { tdec = threadCreate(decoder_thread, c, 96 * 1024, prio - 1, -2, false); vp_log("decoder thread on the app core (core 2 refused)"); }
    if (c->aidx >= 0) {
        taud = threadCreate(audio_thread, c, 64 * 1024, prio - 2, n3ds ? 2 : 1, false);
        if (!taud) taud = threadCreate(audio_thread, c, 64 * 1024, prio - 2, -2, false);
    } else c->audio_parked = 1;
    if (!treader || !tdec || (c->aidx >= 0 && !taud)) { snprintf(err, errlen, "cannot start threads"); goto stop; }

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
            while (c->r_count > 1 && c->ring_pts[(c->r_head + 1) % c->nslot] <= clock + 0.02) {
                c->r_head = (c->r_head + 1) % c->nslot;
                c->r_count--;
                label_queue(c, 2);
                c->skipped++;          /* late frame dropped */
            }
            if (c->r_count > 0 && (c->ring_pts[c->r_head] <= clock + 0.02 || c->ring_pts[c->r_head] < 0))
                show = c->r_head;
            LightLock_Unlock(&c->lock);
            if (show >= 0) {
                u64 b0 = svcGetSystemTick();
                gpu_draw(c, show, c->ring_pts[show] >= 0 ? c->ring_pts[show] : shown_pts);
                c->shown++;
                {   /* frame counter for the bottom screen */
                    u32 h = c->ring_hash[show];
                    c->fc_drawn++;
                    if (h != c->last_hash) c->fc_new++;
                    c->last_hash = h;
                }
                if (c->ring_pts[show] >= 0) shown_pts = c->ring_pts[show];
                LightLock_Lock(&c->lock);
                c->r_head = (c->r_head + 1) % c->nslot;
                c->r_count--;
                LightLock_Unlock(&c->lock);
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
            vp_log("dbg6: frame copy n=%d avg %.1f max %.1f ms | packet decode n=%d avg %.1f max %.1f ms",
                   c->ncp, c->ncp ? c->cp_sum / tms / c->ncp : 0, c->cp_max / tms,
                   c->ndec, c->ndec ? c->dec_sum / tms / c->ndec : 0, c->dec_max / tms);
            c->cp_sum = c->cp_max = c->dec_sum = c->dec_max = 0; c->ncp = c->ndec = 0;
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
    vp_log("stopping: threads");
    c->quit = 1;
    vp_net_abort = 1;
    if (treader) { threadJoin(treader, U64_MAX); threadFree(treader); }
    if (tdec) { threadJoin(tdec, U64_MAX); threadFree(tdec); }
    if (taud) { threadJoin(taud, U64_MAX); threadFree(taud); }
    vp_log("stopping: threads done");
out:
    vp_net_abort = 1;
    if (c->ndsp_ok) { ndspChnWaveBufClear(AUD_CH); ndspChnReset(AUD_CH); ndspExit(); }
    if (c->fmt) avformat_close_input(&c->fmt);
    vp_net_abort = 0;
    pq_clear(c, &c->vq); pq_clear(c, &c->aq);
    pq_clear(c, &c->pv); pq_clear(c, &c->pa);
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
    if (c->wbdata) linearFree(c->wbdata);
    vp_log("stopping: gpu");
    for (int i = 0; i < VRING; i++) if (c->ring[i]) linearFree(c->ring[i]);
    if (c->stage) linearFree(c->stage);
    if (c->gpu_ok) {
        for (int n = 0; n < c->ntx * c->nty; n++) if (c->rtex[n].data) C3D_TexDelete(&c->rtex[n]);
        if (c->stex_ok) C3D_TexDelete(&c->stex);
        if (c->target) C3D_RenderTargetDelete(c->target);
        C2D_Fini();
        C3D_Fini();
    }
    free(c->xmap); free(c->ymap);
    free(open_url);
    free(c);
    gfxSetScreenFormat(GFX_TOP, old_fmt);
    gfxSetWide(false);
    gfxSet3D(old_3d);
    if (err[0]) { vp_log("error: %s", err); ret = -1; }
    return ret;
}
