/*
 * miruro scraping, mirroring ani-cli 5.2.0 (miruro_search / miruro_episodes / miruro_media):
 *   search   GET /search?q=Q           -> page data items:[{id, external_ids:{anilist,mal}, title:{english,romaji}, ..}]
 *   episodes GET /watch/<id>/x?ep=1    -> episodes:[{number, title}, ..]
 *   media    GET /watch/<id>/x?ep=N    -> tracks:[{track:"ssub"|"sub"|"dub", providers:[{provider, subtitles:[..],
 *                                          servers:[{server, headers:{Origin,Referer}, streams:[{url, format}]}]}]}]
 * The data is SvelteKit's JavaScript (devalue) output, read with js.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "scrape.h"
#include "net.h"
#include "js.h"
#include "subs.h"

/* ------------------------------------------------------------ small helpers */

static void set_err(char *err, size_t n, const char *fmt, const char *a)
{
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

static void cpy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void url_encode_query(const char *q, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *q && n + 4 < cap; q++) {
        unsigned char c = *q;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = c;
        else if (c == ' ') out[n++] = '+';
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0;
}

static char *get_page(const char *url, const char *ref, const char *origin, char *err, size_t errlen)
{
    long st;
    char e[200];
    char *body = net_get2(url, ref, origin, &st, NULL, e, sizeof e);
    if (!body) { set_err(err, errlen, "connection error: %s", e); return NULL; }
    if (strstr(body, "<title>Just a moment")) {
        free(body);
        set_err(err, errlen, "blocked by Cloudflare%s", "");
        return NULL;
    }
    if (st < 200 || st >= 300) {
        char s[16];
        snprintf(s, sizeof s, "%ld", st);
        free(body);
        set_err(err, errlen, "HTTP %s from site", s);
        return NULL;
    }
    return body;
}

static jdoc *get_data(const char *url, char *err, size_t errlen)
{
    char *page = get_page(url, NULL, NULL, err, errlen);
    if (!page) return NULL;
    jdoc *d = js_page(page);
    free(page);
    if (!d || !js_roots(d)) { js_free(d); set_err(err, errlen, "site changed (no page data)%s", ""); return NULL; }
    return d;
}

/* ------------------------------------------------------------ search */

static int is_items(const jval *v) { return js_type(v) == J_ARR && js_get(js_at(v, 0), "external_ids"); }

static const char *nice_format(const char *f)
{
    static const struct { const char *in, *out; } m[] = {
        { "TV_SHORT", "Short" }, { "MOVIE", "Movie" }, { "SPECIAL", "Spec" }, { "MUSIC", "Music" },
    };
    if (!f) return "";
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) if (!strcmp(f, m[i].in)) return m[i].out;
    return f;
}

int ani_search(const char *query, anime_t **out, char *err, size_t errlen)
{
    char q[512], url[700];
    url_encode_query(query, q, sizeof q);
    snprintf(url, sizeof url, ANI_BASE "/search?q=%s", q);
    *out = NULL;
    jdoc *d = get_data(url, err, errlen);
    if (!d) return -1;
    const jval *items = js_find(d, "items", is_items);
    int cap = js_len(items) + 1, n = 0;
    anime_t *v = calloc(cap, sizeof *v);
    for (const jslot *s = js_first(items); s; s = js_next(s)) {
        const jval *it = js_val(s);
        const char *id = js_str(js_get(it, "id"));
        if (!id || !id[0]) continue;
        anime_t *a = &v[n];
        memset(a, 0, sizeof *a);
        cpy(a->id, sizeof a->id, id);
        const jval *ext = js_get(it, "external_ids");
        cpy(a->al, sizeof a->al, js_raw(js_at(js_get(ext, "anilist"), 0)));
        const jval *t = js_get(it, "title");
        const char *title = js_str(js_get(t, "english"));
        if (!title || !title[0]) title = js_str(js_get(t, "romaji"));
        if (!title || !title[0]) title = id;
        cpy(a->title, sizeof a->title, title);
        text_437(a->title);
        cpy(a->type, sizeof a->type, nice_format(js_str(js_get(it, "format"))));
        const jval *ec = js_get(it, "episode_counts");
        a->sub = (int)js_num(js_get(ec, "sub"), 0);
        a->dub = (int)js_num(js_get(ec, "dub"), 0);
        n++;
    }
    js_free(d);
    if (!n) { free(v); set_err(err, errlen, "no results%s", ""); return 0; }
    *out = v;
    return n;
}

/* ------------------------------------------------------------ episodes */

static int is_episodes(const jval *v) { return js_type(v) == J_ARR && js_get(js_at(v, 0), "number"); }

static int cmp_ep(const void *a, const void *b)
{
    double d = atof(((const episode_t *)a)->num) - atof(((const episode_t *)b)->num);
    return d < 0 ? -1 : d > 0;
}

int ani_episodes(const char *id, episode_t **out, char *err, size_t errlen)
{
    *out = NULL;
    char url[300];
    snprintf(url, sizeof url, ANI_BASE "/watch/%s/x?ep=1", id);
    jdoc *d = get_data(url, err, errlen);
    if (!d) return -1;
    const jval *eps = js_find(d, "episodes", is_episodes);
    int cap = js_len(eps) + 1, n = 0;
    episode_t *v = calloc(cap, sizeof *v);
    for (const jslot *s = js_first(eps); s; s = js_next(s)) {
        const jval *e = js_val(s);
        const char *num = js_raw(js_get(e, "number"));
        if (!num || !num[0] || js_num(js_get(e, "number"), -1) < 0) continue;
        cpy(v[n].num, sizeof v[n].num, num);
        cpy(v[n].title, sizeof v[n].title, js_str(js_get(e, "title")));
        text_437(v[n].title);
        n++;
    }
    js_free(d);
    /* like ani-cli's sort -nu */
    qsort(v, n, sizeof *v, cmp_ep);
    int w = 0;
    for (int i = 0; i < n; i++)
        if (!w || atof(v[w - 1].num) != atof(v[i].num)) v[w++] = v[i];
    n = w;
    if (!n) { free(v); set_err(err, errlen, "no episodes found%s", ""); return 0; }
    *out = v;
    return n;
}

/* ------------------------------------------------------------ media */

static int is_tracks(const jval *v) { return js_type(v) == J_ARR && js_get(js_at(v, 0), "track"); }

/* ani-cli: language en / en-XX / eng / English, or a label starting with English */
static int is_english(const jval *s)
{
    const char *l = js_str(js_get(s, "language")), *lb = js_str(js_get(s, "label"));
    if (l && (!strcmp(l, "en") || !strncmp(l, "en-", 3) || !strcmp(l, "eng") || !strcasecmp(l, "english"))) return 1;
    return lb && !strncasecmp(lb, "english", 7);
}

/* the first English track, or the last English one marked default */
static const char *english_subs(const jval *prov)
{
    const char *pick = NULL;
    for (const jslot *s = js_first(js_get(prov, "subtitles")); s; s = js_next(s)) {
        const jval *t = js_val(s);
        const char *f = js_str(js_get(t, "file"));
        if (!f || !f[0] || !is_english(t)) continue;
        if (!pick || js_true(js_get(t, "default"))) pick = f;
    }
    return pick;
}

static void origin_of(const char *url, char *out, size_t cap)
{
    const char *h = strstr(url, "://");
    if (!h) { out[0] = 0; return; }
    size_t n = (h + 3 - url) + strcspn(h + 3, "/?#");
    snprintf(out, cap, "%.*s", (int)n, url);
}

static int add_provider(const char *track, const jval *prov, int want_subs, source_t **v, int *n, int *cap)
{
    const char *subs = english_subs(prov);
    if ((want_subs == 1 && !subs) || (want_subs == 0 && subs)) return 0;
    const char *pname = js_str(js_get(prov, "provider"));
    int added = 0;
    for (const jslot *ss = js_first(js_get(prov, "servers")); ss; ss = js_next(ss)) {
        const jval *srv = js_val(ss);
        const char *name = js_str(js_get(srv, "server"));
        if (!name || !name[0]) continue;
        const jval *hd = js_get(srv, "headers");
        const char *ref = js_str(js_get(hd, "Referer")), *org = js_str(js_get(hd, "Origin"));
        for (const jslot *st = js_first(js_get(srv, "streams")); st; st = js_next(st)) {
            const jval *strm = js_val(st);
            const char *url = js_str(js_get(strm, "url"));
            if (!url || strncmp(url, "http", 4)) continue;
            if (*n == *cap) { *cap *= 2; *v = realloc(*v, *cap * sizeof **v); }
            source_t *s = &(*v)[(*n)++];
            memset(s, 0, sizeof *s);
            cpy(s->track, sizeof s->track, track);
            cpy(s->provider, sizeof s->provider, pname);
            cpy(s->server, sizeof s->server, name);
            cpy(s->url, sizeof s->url, url);
            cpy(s->referer, sizeof s->referer, ref);
            if (org && org[0]) cpy(s->origin, sizeof s->origin, org);
            else if (ref) origin_of(ref, s->origin, sizeof s->origin);
            cpy(s->subs, sizeof s->subs, subs);
            const char *fmt = js_str(js_get(strm, "format"));
            s->hls = fmt ? strstr(fmt, "hls") != NULL : strstr(url, ".m3u8") != NULL;
            if (!s->hls) {   /* direct file: trust the listed quality ("720p"), unknown = may try */
                int q = atoi(js_raw(js_get(strm, "quality")) ? js_raw(js_get(strm, "quality")) : "");
                s->height = q > 0 ? q : 0;
            }
            added++;
        }
    }
    return added;
}

int ani_sources(const char *id, const char *ep, const char *mode, source_t **out, char *err, size_t errlen)
{
    *out = NULL;
    char url[300];
    snprintf(url, sizeof url, ANI_BASE "/watch/%s/x?ep=%s", id, ep);
    jdoc *d = get_data(url, err, errlen);
    if (!d) return -1;
    const jval *tracks = js_find(d, "tracks", is_tracks);
    /* ani-cli tries ssub, then sub; here: soft subs with an English track, then hard subs,
     * then soft subs without English text. dub is dub only. */
    static const struct { const char *track; int subs; } sub_order[] = { { "ssub", 1 }, { "sub", -1 }, { "ssub", 0 } },
                                                          dub_order[] = { { "dub", -1 } };
    int dub = !strcmp(mode, "dub");
    int passes = dub ? 1 : 3;
    int cap = 32, n = 0;
    source_t *v = calloc(cap, sizeof *v);
    for (int p = 0; p < passes; p++) {
        const char *want = dub ? dub_order[p].track : sub_order[p].track;
        int ws = dub ? dub_order[p].subs : sub_order[p].subs;
        for (const jslot *t = js_first(tracks); t; t = js_next(t)) {
            const jval *tr = js_val(t);
            const char *name = js_str(js_get(tr, "track"));
            if (!name || strcmp(name, want)) continue;
            for (const jslot *pv = js_first(js_get(tr, "providers")); pv; pv = js_next(pv))
                add_provider(name, js_val(pv), ws, &v, &n, &cap);
        }
    }
    js_free(d);
    if (!n) {
        free(v);
        if (!tracks) set_err(err, errlen, "miruro has no video for this episode%s", "");
        else set_err(err, errlen, "no %s source for this episode", mode);
        return -1;
    }
    *out = v;
    return n;
}

/* best variant height that fits maxq in an HLS master playlist (same rules as the player's
 * hls_choose: H.264 only, <= maxq high, <= 1152 wide at 480p / 1920 at 720p) */
static int master_fit(const char *m3u, int maxq, int *wout)
{
    int best = -1, bw = 0, have_inf = 0, any = 0;
    char inf[1024] = "";
    for (const char *l = m3u; *l;) {
        const char *e = l + strcspn(l, "\r\n");
        size_t L = e - l;
        if (L > 18 && !strncmp(l, "#EXT-X-STREAM-INF:", 18)) { snprintf(inf, sizeof inf, "%.*s", (int)L, l); have_inf = 1; }
        else if (have_inf && L && l[0] != '#') {
            any = 1;
            have_inf = 0;
            const char *c = strstr(inf, "CODECS=\"");
            char codecs[256] = "";
            if (c) { c += 8; snprintf(codecs, sizeof codecs, "%.*s", (int)strcspn(c, "\""), c); }
            int h264 = !codecs[0] || strstr(codecs, "avc1") || strstr(codecs, "avc3");
            if (strstr(codecs, "hvc1") || strstr(codecs, "hev1") || strstr(codecs, "av01") || strstr(codecs, "vp09")) h264 = 0;
            int w = 0, h = 0;
            const char *r = strstr(inf, "RESOLUTION=");
            if (r) sscanf(r + 11, "%dx%d", &w, &h);
            if (!h) h = 1;   /* unknown size: the player decides */
            if (h264 && h <= maxq && w <= (maxq > 480 ? 1920 : 1152) && h > best) { best = h; bw = w; }
        }
        l = *e ? e + 1 : e;
        if (*l == '\n') l++;
    }
    if (!any) best = 1;   /* a media playlist, not a master: one quality, size unknown */
    if (wout) *wout = bw;
    return best;
}

static int probe(source_t *s, int maxq)
{
    if (!s->hls) return s->height > 0 ? (s->height <= maxq ? s->height : -1) : 1;
    long st;
    char e[200];
    char *m = net_get2(s->url, s->referer, s->origin, &st, NULL, e, sizeof e);
    int h;
    if (!m || st != 200 || !strstr(m, "#EXTM3U")) h = -2;
    else h = master_fit(m, maxq, &s->width);
    free(m);
    return h;
}

static int same_group(const source_t *a, const source_t *b)
{
    return !strcmp(a->track, b->track) && !a->subs[0] == !b->subs[0];
}

int ani_pick(source_t *v, int n, int maxq, char *err, size_t errlen)
{
    int best = -1, probes = 0;
    const source_t *skip = NULL;
    for (int i = 0; i < n && probes < 8; i++) {
        source_t *s = &v[i];
        /* groups (soft subs, hard subs, soft without English) are in preference order:
         * a fitting stream of an earlier group wins over a taller one of a later group */
        if (best >= 0 && !same_group(&v[best], s)) break;
        if (s->height == -3) continue;   /* failed to play */
        /* a provider's servers on the same host are mirrors of the same encodes: once one
         * answered, skip the others (different hosts, like bee's VidPlay, can differ) */
        if (skip && !strcmp(skip->track, s->track) && !strcmp(skip->provider, s->provider) &&
            !strcmp(skip->referer, s->referer)) continue;
        if (!s->height || !s->hls) { s->height = probe(s, maxq); probes++; }
        int h = s->height;
        if (h >= maxq) return i;
        if (h > 0 && (best < 0 || h > v[best].height)) best = i;
        if (h == -1 || h > 0) skip = s;
    }
    if (best >= 0) return best;
    char m[32];
    snprintf(m, sizeof m, "%dp", maxq);
    set_err(err, errlen, "no stream up to %s found (only larger ones?)", m);
    return -1;
}
