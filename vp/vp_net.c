/*
 * Network streams for the player: libavformat's http/https protocol (pbnet.c in
 * our ffmpeg) calls these. Each open stream has a curl download thread that
 * fills a ring buffer; reads block until data arrives; seeks restart the
 * download with an HTTP Range request (or skip ahead if close).
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>
#include "pb.h"
#include "vp.h"

#define RING_SIZE   (1536 * 1024)
#define SKIP_MAX    (384 * 1024)     /* forward seeks shorter than this just read ahead */

typedef struct {
    char *url;
    Thread th;
    LightLock lock;
    CondVar cv;                   /* signalled on any state change */
    u8 *ring;
    size_t rpos, fill;
    int64_t pos;                  /* stream offset of ring read position */
    int64_t req_start;            /* offset the current request started at */
    int64_t size;                 /* total size or -1 */
    long status;
    volatile int stop, done, headers, ranged;
    int err;
    char errbuf[CURL_ERROR_SIZE];
} vstream;

volatile int vp_net_abort;        /* set by the player to unblock everything */
char vp_referer[1024];
static s32 g_net_prio = 0x30;
vp_net_stats_t vp_net_stats;

void vp_net_set_priority(s32 prio) { g_net_prio = prio; }

static size_t on_write(char *d, size_t sz, size_t n, void *u)
{
    vstream *s = u;
    size_t total = sz * n, done = 0;
    LightLock_Lock(&s->lock);
    while (done < total) {
        while (s->fill == RING_SIZE && !s->stop && !vp_net_abort)
            CondVar_WaitTimeout(&s->cv, &s->lock, 100 * 1000000LL);
        if (s->stop || vp_net_abort) { LightLock_Unlock(&s->lock); return 0; }
        size_t wpos = (s->rpos + s->fill) % RING_SIZE;
        size_t room = RING_SIZE - s->fill;
        size_t chunk = RING_SIZE - wpos;
        if (chunk > room) chunk = room;
        if (chunk > total - done) chunk = total - done;
        memcpy(s->ring + wpos, d + done, chunk);
        s->fill += chunk;
        done += chunk;
        vp_net_stats.bytes += chunk;
        CondVar_Broadcast(&s->cv);
    }
    LightLock_Unlock(&s->lock);
    return total;
}

static size_t on_header(char *d, size_t sz, size_t n, void *u)
{
    vstream *s = u;
    size_t len = sz * n;
    LightLock_Lock(&s->lock);
    if (len > 5 && !strncmp(d, "HTTP/", 5)) {
        const char *sp = memchr(d, ' ', len);
        s->status = sp ? strtol(sp + 1, NULL, 10) : 0;
    } else if (len > 15 && !strncasecmp(d, "Content-Range:", 14)) {
        const char *sl = memchr(d, '/', len);
        if (sl && sl[1] != '*') s->size = strtoll(sl + 1, NULL, 10);
        s->ranged = 1;
    } else if (len > 15 && !strncasecmp(d, "Content-Length:", 15) && s->status == 200) {
        s->size = s->req_start + strtoll(d + 15, NULL, 10);
    } else if (len <= 2 && s->status >= 200 && s->status < 300) {
        /* end of the final header block */
        s->headers = 1;
        CondVar_Broadcast(&s->cv);
    }
    LightLock_Unlock(&s->lock);
    return len;
}

static void dl_thread(void *arg)
{
    vstream *s = arg;
    CURL *e = curl_easy_init();
    pb_curl_prepare(e);
    curl_easy_setopt(e, CURLOPT_URL, s->url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, s->errbuf);
    struct curl_slist *hl = NULL;
    char line[1100];
    if (vp_referer[0]) {
        snprintf(line, sizeof line, "Referer: %s", vp_referer);
        hl = curl_slist_append(hl, line);
    }
    hl = curl_slist_append(hl, "Accept: */*");
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, hl);
    if (s->req_start > 0) {
        char r[48];
        snprintf(r, sizeof r, "%lld-", (long long)s->req_start);
        curl_easy_setopt(e, CURLOPT_RANGE, r);
    }
    CURLcode rc = curl_easy_perform(e);
    long code = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(e);
    curl_slist_free_all(hl);
    LightLock_Lock(&s->lock);
    if (!s->stop) {
        if (rc != CURLE_OK) s->err = (int)rc;
        else if (code >= 400) s->err = (int)code;
        if (s->err && !s->errbuf[0]) snprintf(s->errbuf, sizeof s->errbuf, "HTTP %ld", code);
    }
    if (code) s->status = code;
    s->done = 1;
    s->headers = 1;
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
}

static int start_request(vstream *s, int64_t off)
{
    s->rpos = s->fill = 0;
    s->pos = s->req_start = off;
    s->stop = s->done = s->headers = s->err = 0;
    s->status = 0;
    s->errbuf[0] = 0;
    s->th = threadCreate(dl_thread, s, 48 * 1024, g_net_prio, -2, false);
    return s->th ? 0 : -1;
}

static void stop_request(vstream *s)
{
    if (!s->th) return;
    LightLock_Lock(&s->lock);
    s->stop = 1;
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
    threadJoin(s->th, U64_MAX);
    threadFree(s->th);
    s->th = NULL;
}

/* wait for the response headers; 0 on success */
static int wait_headers(vstream *s)
{
    LightLock_Lock(&s->lock);
    while (!s->headers && !vp_net_abort)
        CondVar_WaitTimeout(&s->cv, &s->lock, 100 * 1000000LL);
    int st = s->status, err = s->err;
    LightLock_Unlock(&s->lock);
    if (vp_net_abort) return -1;
    if (err || st >= 400 || st == 0) return -5;
    return 0;
}

int pbnet_open(const char *url, void **handle, int64_t *size)
{
    vstream *s = calloc(1, sizeof *s);
    if (!s) return -12;
    s->ring = malloc(RING_SIZE);
    if (!s->ring) { free(s); return -12; }
    s->url = strdup(url);
    s->size = -1;
    LightLock_Init(&s->lock);
    CondVar_Init(&s->cv);
    vp_net_stats.opens++;
    if (start_request(s, 0) || wait_headers(s)) {
        vp_log("open failed: %.200s (%ld %s)", url, s->status, s->errbuf);
        stop_request(s);
        free(s->ring); free(s->url); free(s);
        return -5;
    }
    if (vp_debug) vp_log("net open %.160s -> HTTP %ld, %lld bytes", url, s->status, (long long)s->size);
    *size = s->size;
    *handle = s;
    return 0;
}

int pbnet_read(void *h, unsigned char *buf, int n)
{
    vstream *s = h;
    LightLock_Lock(&s->lock);
    while (!s->fill && !s->done && !vp_net_abort) {
        vp_net_stats.waiting = 1;
        CondVar_WaitTimeout(&s->cv, &s->lock, 100 * 1000000LL);
    }
    vp_net_stats.waiting = 0;
    if (vp_net_abort) { LightLock_Unlock(&s->lock); return -0x54495845; /* AVERROR_EXIT */ }
    if (!s->fill) {
        int err = s->err;
        LightLock_Unlock(&s->lock);
        return err ? -5 : 0;
    }
    size_t take = (size_t)n < s->fill ? (size_t)n : s->fill;
    size_t first = RING_SIZE - s->rpos;
    if (first > take) first = take;
    memcpy(buf, s->ring + s->rpos, first);
    if (take > first) memcpy(buf + first, s->ring, take - first);
    s->rpos = (s->rpos + take) % RING_SIZE;
    s->fill -= take;
    s->pos += take;
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
    return (int)take;
}

int64_t pbnet_seek(void *h, int64_t pos)
{
    vstream *s = h;
    if (pos == s->pos) return pos;
    if (pos > s->pos && pos - s->pos <= SKIP_MAX) {
        unsigned char tmp[4096];
        while (s->pos < pos) {
            int want = pos - s->pos > (int64_t)sizeof tmp ? (int)sizeof tmp : (int)(pos - s->pos);
            if (pbnet_read(s, tmp, want) <= 0) break;
        }
        if (s->pos == pos) return pos;
    }
    stop_request(s);
    if (s->size >= 0 && pos >= s->size) {
        s->rpos = s->fill = 0;
        s->pos = pos;
        s->done = 1;
        return pos;
    }
    vp_net_stats.seeks++;
    if (start_request(s, pos) || wait_headers(s)) return -5;
    if (pos > 0 && s->status == 200) {
        vp_log("server does not support seeking: %.200s", s->url);
        return -38;   /* AVERROR(ENOSYS) */
    }
    return pos;
}

void pbnet_close(void *h)
{
    vstream *s = h;
    stop_request(s);
    free(s->ring);
    free(s->url);
    free(s);
}

/* fetch a small text resource completely (playlists) */
char *vp_fetch_text(const char *url, long *status)
{
    void *h;
    int64_t size;
    *status = 0;
    if (pbnet_open(url, &h, &size) < 0) return NULL;
    vstream *s = h;
    size_t cap = 65536, n = 0;
    char *out = malloc(cap);
    for (;;) {
        if (n + 4096 + 1 > cap) { cap *= 2; out = realloc(out, cap); }
        int r = pbnet_read(h, (unsigned char *)out + n, 4096);
        if (r <= 0) break;
        n += r;
        if (n > 4 * 1024 * 1024) break;
    }
    out[n] = 0;
    *status = s->status;
    pbnet_close(h);
    return out;
}
