/*
 * hianime scraping, mirroring ani-cli 5.1.8:
 *   search   GET /search?keyword=Q              -> film cards (slug, title, counts)
 *   episodes GET /api/theme/episode/list/<num>  -> JSON-wrapped html, ep-item blocks
 *   servers  GET /api/theme/episode/servers?episodeId=<id> -> server-item blocks
 *            pick data-type=<mode> + data-server-name="ZokoAnime", data-hash = base64(embed url)
 *   embed    GET <embed>  -> window.__P="<base64(json XOR 'otaku-embed-v1')>"
 *            json: "src": master .m3u8, "subtitles": [{default:true, src: .vtt}]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "scrape.h"
#include "net.h"

/* ------------------------------------------------------------ small helpers */

static void set_err(char *err, size_t n, const char *fmt, const char *a)
{
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

/* copy the text between p and the next `end` char (exclusive) */
static int copy_until(const char *p, char end, char *out, size_t cap)
{
    size_t n = 0;
    while (*p && *p != end && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return *p == end;
}

/* find `key` (e.g. data-id=") inside [from, lim) and copy the quoted value */
static int attr_in(const char *from, const char *lim, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    const char *p = strstr(from, key);
    if (!p || (lim && p >= lim)) return 0;
    return copy_until(p + strlen(key), '"', out, cap);
}

static const char *find_lim(const char *from, const char *lim, const char *needle)
{
    const char *p = strstr(from, needle);
    return (p && (!lim || p < lim)) ? p : NULL;
}

void html_unescape(char *s)
{
    static const struct { const char *e; const char *r; } map[] = {
        { "&#039;", "'" }, { "&#39;", "'" }, { "&quot;", "\"" }, { "&amp;", "&" },
        { "&lt;", "<" }, { "&gt;", ">" }, { "&nbsp;", " " }, { "&#038;", "&" },
    };
    char *w = s;
    for (char *r = s; *r;) {
        int hit = 0;
        if (*r == '&')
            for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
                size_t L = strlen(map[i].e);
                if (!strncmp(r, map[i].e, L)) {
                    size_t rl = strlen(map[i].r);
                    memcpy(w, map[i].r, rl);
                    w += rl; r += L; hit = 1;
                    break;
                }
            }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
    /* trim */
    char *a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    size_t n = strlen(a);
    while (n && isspace((unsigned char)a[n - 1])) n--;
    memmove(s, a, n);
    s[n] = 0;
}

/* remove every backslash, like ani-cli's sed 's|\\||g' (unescapes \" and \/) */
static void strip_backslashes(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != '\\') *w++ = *r;
    *w = 0;
}

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* returns malloc'd bytes (NUL-terminated), length in *outlen */
static unsigned char *b64_decode(const char *in, size_t *outlen)
{
    size_t L = strlen(in);
    unsigned char *out = malloc(L / 4 * 3 + 4);
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < L; i++) {
        int v = b64v((unsigned char)in[i]);
        if (v < 0) continue;            /* skips '=' padding and whitespace */
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (acc >> bits) & 0xff; }
    }
    out[n] = 0;
    if (outlen) *outlen = n;
    return out;
}

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

static char *get_page(const char *url, const char *ref, char *err, size_t errlen)
{
    long st;
    char e[200];
    char *body = net_get(url, ref, &st, NULL, e, sizeof e);
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

/* ------------------------------------------------------------ search */

static int parse_count(const char *chunk, const char *lim, const char *cls)
{
    const char *p = find_lim(chunk, lim, cls);
    if (!p) return 0;
    p = find_lim(p, lim, "</i>");
    if (!p) return 0;
    return atoi(p + 4);
}

int ani_search(const char *query, anime_t **out, char *err, size_t errlen)
{
    char q[512], url[700];
    url_encode_query(query, q, sizeof q);
    snprintf(url, sizeof url, ANI_BASE "/search?keyword=%s", q);
    *out = NULL;
    char *page = get_page(url, NULL, err, errlen);
    if (!page) return -1;
    /* the "top 10" sidebar repeats the card markup: cut it off */
    char *side = strstr(page, "id=\"main-sidebar\"");
    if (side) *side = 0;

    int cap = 32, n = 0;
    anime_t *v = calloc(cap, sizeof *v);
    const char *card_tag = strstr(page, "class=\"flw-item") ? "class=\"flw-item" : "<div class=\"film-detail\">";
    const char *p = strstr(page, card_tag);
    while (p) {
        const char *next = strstr(p + 1, card_tag);
        const char *h3 = find_lim(p, next, "<h3 class=\"film-name\">");
        if (h3) {
            anime_t a;
            memset(&a, 0, sizeof a);
            char href[512];
            const char *lim = next;
            if (attr_in(h3, lim, "href=\"", href, sizeof href) && attr_in(h3, lim, "title=\"", a.title, sizeof a.title)) {
                char *slash = strrchr(href, '/');
                snprintf(a.slug, sizeof a.slug, "%s", slash ? slash + 1 : href);
                char *qm = strchr(a.slug, '?');
                if (qm) *qm = 0;
                html_unescape(a.title);
                a.sub = parse_count(p, h3, "tick-sub");
                a.dub = parse_count(p, h3, "tick-dub");
                const char *fi = find_lim(h3, lim, "class=\"fdi-item\">");
                if (fi) { copy_until(fi + 17, '<', a.type, sizeof a.type); html_unescape(a.type); }
                if (a.slug[0]) {
                    if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
                    v[n++] = a;
                }
            }
        }
        p = next;
    }
    free(page);
    if (!n) { free(v); set_err(err, errlen, "no results%s", ""); return 0; }
    *out = v;
    return n;
}

/* ------------------------------------------------------------ episodes */

int ani_episodes(const char *slug, episode_t **out, char *err, size_t errlen)
{
    *out = NULL;
    const char *dash = strrchr(slug, '-');
    char url[400];
    snprintf(url, sizeof url, ANI_BASE "/api/theme/episode/list/%s", dash ? dash + 1 : slug);
    char *page = get_page(url, NULL, err, errlen);
    if (!page) return -1;
    strip_backslashes(page);
    /* season links to other shows use the same markup; keep only this slug's episodes */
    char want[220];
    snprintf(want, sizeof want, "/watch/%s?ep=", slug);

    int cap = 64, n = 0;
    episode_t *v = calloc(cap, sizeof *v);
    const char *p = strstr(page, "ep-item");
    while (p) {
        const char *next = strstr(p + 7, "ep-item");
        char num[16], id[24], href[400];
        if (attr_in(p, next, "data-number=\"", num, sizeof num) &&
            attr_in(p, next, "data-id=\"", id, sizeof id) &&
            attr_in(p, next, "href=\"", href, sizeof href) && strstr(href, want)) {
            episode_t e;
            memset(&e, 0, sizeof e);
            e.id = atoi(id);
            snprintf(e.num, sizeof e.num, "%s", num);
            const char *en = find_lim(p, next, "class=\"ep-name");
            if (en) { attr_in(en, next, "title=\"", e.title, sizeof e.title); html_unescape(e.title); }
            if (e.id > 0) {
                if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
                v[n++] = e;
            }
        }
        p = next;
    }
    free(page);
    if (!n) { free(v); set_err(err, errlen, "no episodes found%s", ""); return 0; }
    *out = v;
    return n;
}

/* ------------------------------------------------------------ stream */

/* in a flat JSON text, find the first "src":"..." after `from` whose value contains `must` */
static int json_src(const char *from, const char *lim, const char *must, char *out, size_t cap)
{
    const char *p = from;
    while ((p = strstr(p, "\"src\":\"")) && (!lim || p < lim)) {
        p += 7;
        char tmp[2048];
        copy_until(p, '"', tmp, sizeof tmp);
        if (!must || strstr(tmp, must)) { snprintf(out, cap, "%s", tmp); return 1; }
    }
    out[0] = 0;
    return 0;
}

int ani_resolve(int episode_id, const char *mode, stream_t *st, char *err, size_t errlen)
{
    memset(st, 0, sizeof *st);
    char url[300];
    snprintf(url, sizeof url, ANI_BASE "/api/theme/episode/servers?episodeId=%d", episode_id);
    char *page = get_page(url, NULL, err, errlen);
    if (!page) return -1;
    strip_backslashes(page);

    char hash[2048] = "", want_type[32];
    snprintf(want_type, sizeof want_type, "data-type=\"%s\"", mode);
    const char *p = strstr(page, "server-item");
    while (p && !hash[0]) {
        const char *next = strstr(p + 11, "server-item");
        if (find_lim(p, next, want_type) && find_lim(p, next, "data-server-name=\"ZokoAnime\""))
            attr_in(p, next, "data-hash=\"", hash, sizeof hash);
        p = next;
    }
    free(page);
    if (!hash[0]) { set_err(err, errlen, "no %s source (ZokoAnime) for this episode", mode); return -1; }

    unsigned char *emb = b64_decode(hash, NULL);
    snprintf(st->embed, sizeof st->embed, "%s", (char *)emb);
    free(emb);
    if (strncmp(st->embed, "http", 4)) { set_err(err, errlen, "bad embed link%s", ""); return -1; }
    /* the stream host wants the embed site as referer */
    {
        const char *h = strstr(st->embed, "://");
        size_t o = h ? (size_t)(h + 3 - st->embed) + strcspn(h + 3, "/") : strlen(st->embed);
        snprintf(st->referer, sizeof st->referer, "%.*s/", (int)o, st->embed);
    }

    page = get_page(st->embed, NULL, err, errlen);
    if (!page) return -1;
    const char *bp = strstr(page, "window.__P=\"");
    if (!bp) { free(page); set_err(err, errlen, "embed page changed (no player config)%s", ""); return -1; }
    bp += 12;
    const char *be = strchr(bp, '"');
    if (!be) { free(page); set_err(err, errlen, "embed page truncated%s", ""); return -1; }
    char *b64 = malloc(be - bp + 1);
    memcpy(b64, bp, be - bp);
    b64[be - bp] = 0;
    free(page);

    size_t jl;
    unsigned char *js = b64_decode(b64, &jl);
    free(b64);
    static const char key[] = "otaku-embed-v1";
    for (size_t i = 0; i < jl; i++) js[i] ^= (unsigned char)key[i % (sizeof key - 1)];
    strip_backslashes((char *)js);

    const char *json = (const char *)js;
    const char *subs = strstr(json, "\"subtitles\":[");
    const char *subs_end = subs ? strchr(subs, ']') : NULL;
    /* master playlist: a src containing .m3u8 that is not a subtitle */
    const char *q = json;
    while (json_src(q, NULL, ".m3u8", st->m3u8, sizeof st->m3u8)) {
        const char *at = strstr(q, st->m3u8);
        if (!(subs && at > subs && at < subs_end)) break;
        q = at + 1;
        st->m3u8[0] = 0;
    }
    if (subs && subs_end) {
        const char *d = strstr(subs, "\"default\":true");
        if (d && d < subs_end) {
            /* the src of the object containing "default":true */
            const char *ob = d;
            while (ob > subs && *ob != '{') ob--;
            const char *oe = strchr(d, '}');
            json_src(ob, oe, NULL, st->subs, sizeof st->subs);
        }
        if (!st->subs[0]) json_src(subs, subs_end, ".vtt", st->subs, sizeof st->subs);
    }
    free(js);
    if (!st->m3u8[0]) { set_err(err, errlen, "no video in player config%s", ""); return -1; }
    return 0;
}
