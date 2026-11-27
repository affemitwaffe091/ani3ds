/*
 * HTTP(S) for ani3ds: libcurl with a shared connection/TLS-session cache.
 * On the 3DS, curl's mbedtls backend would re-parse the CA file for every
 * connection (seconds each), so the CA chain is parsed once and injected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "net.h"
#include "pb.h"

#ifdef __3DS__
#include <3ds.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
static mbedtls_x509_crt g_cacert;
static int g_cacert_ok;
static LightLock g_locks[CURL_LOCK_DATA_LAST];
static CURLcode sslctx_cb(CURL *curl, void *sslctx, void *parm)
{
    if (g_cacert_ok) mbedtls_ssl_conf_ca_chain((mbedtls_ssl_config *)sslctx, &g_cacert, NULL);
    return CURLE_OK;
}
static void lk(CURL *h, curl_lock_data d, curl_lock_access a, void *u) { LightLock_Lock(&g_locks[d]); }
static void ulk(CURL *h, curl_lock_data d, void *u) { LightLock_Unlock(&g_locks[d]); }
#else
#include <pthread.h>
static pthread_mutex_t g_locks[CURL_LOCK_DATA_LAST];
static void lk(CURL *h, curl_lock_data d, curl_lock_access a, void *u) { pthread_mutex_lock(&g_locks[d]); }
static void ulk(CURL *h, curl_lock_data d, void *u) { pthread_mutex_unlock(&g_locks[d]); }
#endif

/* same desktop UA ani-cli sends; the site serves the full markup to it */
#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"

static CURLSH *g_share;
static void nolog(const char *fmt, ...) { (void)fmt; }
void (*net_log)(const char *fmt, ...) = nolog;

int net_init(const char *ca_bundle)
{
#ifdef __3DS__
    mbedtls_x509_crt_init(&g_cacert);
    if (ca_bundle) {
        int r = mbedtls_x509_crt_parse_file(&g_cacert, ca_bundle);
        g_cacert_ok = r >= 0;
        if (r != 0) net_log("[tls] CA bundle: %d (%s)\n", r, g_cacert_ok ? "partly ok" : "FAILED");
    }
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) LightLock_Init(&g_locks[i]);
#else
    (void)ca_bundle;
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&g_locks[i], NULL);
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) return -1;
    g_share = curl_share_init();
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, lk);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, ulk);
    return 0;
}

void net_exit(void)
{
    if (g_share) curl_share_cleanup(g_share);
    g_share = NULL;
    curl_global_cleanup();
}

/* used by the player's download threads too */
void pb_curl_prepare(void *ce)
{
    CURL *e = ce;
    curl_easy_setopt(e, CURLOPT_SHARE, g_share);
    curl_easy_setopt(e, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
#ifdef __3DS__
    curl_easy_setopt(e, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(e, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(e, CURLOPT_SSL_CTX_FUNCTION, sslctx_cb);
    curl_easy_setopt(e, CURLOPT_BUFFERSIZE, 65536L);
#endif
}

typedef struct { char *p; size_t n, cap; } buf_t;
static size_t on_body(char *d, size_t sz, size_t n, void *u)
{
    buf_t *b = u;
    size_t t = sz * n;
    if (b->n + t + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 65536;
        while (b->n + t + 1 > nc) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, d, t);
    b->n += t;
    b->p[b->n] = 0;
    return t;
}

char *net_get(const char *url, const char *referer, long *status, size_t *len, char *err, size_t errlen)
{
    return net_get2(url, referer, NULL, status, len, err, errlen);
}

char *net_get2(const char *url, const char *referer, const char *origin, long *status, size_t *len, char *err, size_t errlen)
{
    CURL *e = curl_easy_init();
    struct curl_slist *hl = NULL;
    buf_t b = { 0 };
    char eb[CURL_ERROR_SIZE] = "";
    *status = 0;
    if (err && errlen) err[0] = 0;
    if (!e) { if (err) snprintf(err, errlen, "curl init failed"); return NULL; }
    pb_curl_prepare(e);
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, eb);
    if (referer && referer[0]) curl_easy_setopt(e, CURLOPT_REFERER, referer);
    if (origin && origin[0]) {
        char h[300];
        snprintf(h, sizeof h, "Origin: %s", origin);
        hl = curl_slist_append(hl, h);
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, hl);
    }
    CURLcode rc = curl_easy_perform(e);
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(e);
    curl_slist_free_all(hl);
    if (rc != CURLE_OK) {
        if (err) snprintf(err, errlen, "%s", eb[0] ? eb : curl_easy_strerror(rc));
        net_log("GET failed %.120s: %s\n", url, eb[0] ? eb : curl_easy_strerror(rc));
        free(b.p);
        return NULL;
    }
    if (!b.p) { b.p = malloc(1); b.p[0] = 0; }
    if (len) *len = b.n;
    return b.p;
}
