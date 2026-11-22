/* WebVTT / SRT cues -> plain text in code page 437 (libctru's console font) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "subs.h"

static int parse_ts(const char *p, double *out, const char **end)
{
    /* [hh:]mm:ss.mmm  (SRT uses ',') */
    int f[3] = { 0, 0, 0 }, nf = 0;
    while (nf < 3) {
        if (*p < '0' || *p > '9') return 0;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        f[nf++] = v;
        if (*p == ':') { p++; continue; }
        break;
    }
    double ms = 0;
    if (*p == '.' || *p == ',') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') { ms += (*p++ - '0') * scale; scale /= 10; }
    }
    double t = nf == 3 ? f[0] * 3600.0 + f[1] * 60 + f[2] : nf == 2 ? f[0] * 60.0 + f[1] : f[0];
    *out = t + ms;
    if (end) *end = p;
    return 1;
}

/* UTF-8 code point -> CP437 byte (0 = drop) */
static unsigned char to437(unsigned cp)
{
    if (cp < 0x80) return (unsigned char)cp;
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201B: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2026: return 0xFF;            /* expanded to "..." below */
    case 0x266A: case 0x266B: return 0x0E; /* music note glyph */
    case 0xA0: return ' ';
    case 0xE4: return 0x84; case 0xC4: return 0x8E; case 0xF6: return 0x94; case 0xD6: return 0x99;
    case 0xFC: return 0x81; case 0xDC: return 0x9A; case 0xDF: return 0xE1;
    case 0xE9: return 0x82; case 0xE8: return 0x8A; case 0xEA: return 0x88; case 0xC9: return 0x90;
    case 0xE0: return 0x85; case 0xE1: return 0xA0; case 0xE2: return 0x83; case 0xE7: return 0x87;
    case 0xED: return 0xA1; case 0xEF: return 0x8B; case 0xEE: return 0x8C;
    case 0xF3: return 0xA2; case 0xF4: return 0x93; case 0xFA: return 0xA3; case 0xFB: return 0x96;
    case 0xF1: return 0xA4; case 0xD1: return 0xA5; case 0xBF: return 0xA8; case 0xA1: return 0xAD;
    case 0x101: return 'a'; case 0x113: return 'e'; case 0x12B: return 'i'; case 0x14D: return 'o'; case 0x16B: return 'u';
    case 0x14C: return 'O'; case 0x16A: return 'U';
    default: return cp < 0x3000 ? '?' : 0;   /* CJK etc. cannot be drawn */
    }
}

/* strip <tags> and {\an8}-style overrides, decode entities + UTF-8 */
static char *clean_text(const char *s, size_t n)
{
    char *o = malloc(n * 3 + 1);
    size_t w = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c == '<') { while (i < n && s[i] != '>') i++; i++; continue; }
        if (c == '{' && i + 1 < n && s[i + 1] == '\\') { while (i < n && s[i] != '}') i++; i++; continue; }
        if (c == '&') {
            static const struct { const char *e; char r; } m[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&nbsp;", ' ' }, { "&quot;", '"' }, { "&#39;", '\'' } };
            int hit = 0;
            for (size_t k = 0; k < sizeof m / sizeof *m; k++) {
                size_t L = strlen(m[k].e);
                if (i + L <= n && !strncmp(s + i, m[k].e, L)) { o[w++] = m[k].r; i += L; hit = 1; break; }
            }
            if (hit) continue;
        }
        unsigned cp = c;
        int extra = 0;
        if (c >= 0xF0) { cp = c & 7; extra = 3; }
        else if (c >= 0xE0) { cp = c & 15; extra = 2; }
        else if (c >= 0xC0) { cp = c & 31; extra = 1; }
        i++;
        for (int k = 0; k < extra && i < n; k++) cp = (cp << 6) | (s[i++] & 63);
        if (cp == '\r') continue;
        unsigned char b = to437(cp);
        if (b == 0xFF) { o[w++] = '.'; o[w++] = '.'; o[w++] = '.'; }
        else if (b) o[w++] = b;
    }
    /* trim trailing newlines/spaces */
    while (w && (o[w - 1] == '\n' || o[w - 1] == ' ')) w--;
    o[w] = 0;
    return o;
}

static int cmp_cue(const void *a, const void *b)
{
    double d = ((const cue_t *)a)->a - ((const cue_t *)b)->a;
    return d < 0 ? -1 : d > 0;
}

int subs_parse(subs_t *s, const char *vtt)
{
    memset(s, 0, sizeof *s);
    int cap = 256;
    s->c = calloc(cap, sizeof *s->c);
    const char *p = vtt;
    while ((p = strstr(p, "-->"))) {
        /* start time is at the beginning of this line */
        const char *ls = p;
        while (ls > vtt && ls[-1] != '\n') ls--;
        double a, b;
        const char *e;
        while (*ls == ' ') ls++;
        const char *q = p + 3;
        while (*q == ' ') q++;
        if (!parse_ts(ls, &a, NULL) || !parse_ts(q, &b, &e)) { p += 3; continue; }
        const char *t = strchr(e, '\n');
        if (!t) break;
        t++;
        /* text runs until a blank line */
        const char *te = t;
        while (*te) {
            const char *nl = strchr(te, '\n');
            const char *ln = te;
            size_t L = nl ? (size_t)(nl - ln) : strlen(ln);
            while (L && ln[L - 1] == '\r') L--;
            if (!L) break;
            te = nl ? nl + 1 : ln + strlen(ln);
            if (!nl) break;
        }
        if (s->n == cap) { cap *= 2; s->c = realloc(s->c, cap * sizeof *s->c); }
        char *txt = clean_text(t, te - t);
        if (txt[0]) { s->c[s->n].a = a; s->c[s->n].b = b; s->c[s->n].text = txt; s->n++; }
        else free(txt);
        p = te;
    }
    qsort(s->c, s->n, sizeof *s->c, cmp_cue);
    return s->n;
}

const char *subs_at(subs_t *s, double t)
{
    if (!s || !s->n) return NULL;
    /* binary search for the last cue starting <= t, then look back a little for overlaps */
    int lo = 0, hi = s->n - 1, idx = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (s->c[m].a <= t) { idx = m; lo = m + 1; } else hi = m - 1;
    }
    for (int i = idx; i >= 0 && i > idx - 4; i--)
        if (t >= s->c[i].a && t < s->c[i].b) return s->c[i].text;
    return NULL;
}

void subs_free(subs_t *s)
{
    for (int i = 0; i < s->n; i++) free(s->c[i].text);
    free(s->c);
    memset(s, 0, sizeof *s);
}
