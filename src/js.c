/* see js.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "js.h"

typedef struct chunk { struct chunk *next; size_t used, cap; double data[]; } chunk;

typedef struct jenv {
    struct jenv *parent;
    int n;
    const char **names;
    jval **vals;
} jenv;

struct jslot { const char *key; jval *v; jslot *next; };
struct jval {
    jtype t;
    const char *s;          /* string, number source text, or variable name */
    double num;
    jslot *first, *last;
    int n;
    jenv *env;              /* J_REF: scope the name belongs to */
};
struct jdoc { chunk *mem; jslot *roots, *last; int nroots; };

typedef struct { const char *p; jdoc *d; jenv *env; int depth, err; } P;

/* ------------------------------------------------------------ memory */
static void *amem(jdoc *d, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    chunk *c = d->mem;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 ? n : 65536;
        c = malloc(sizeof *c + cap);
        if (!c) return NULL;
        c->next = d->mem;
        c->used = 0;
        c->cap = cap;
        d->mem = c;
    }
    void *r = (char *)c->data + c->used;
    c->used += n;
    memset(r, 0, n);
    return r;
}

static jval *node(P *p, jtype t)
{
    jval *v = amem(p->d, sizeof *v);
    if (!v) { p->err = 1; return NULL; }
    v->t = t;
    return v;
}

static jslot *add(P *p, jval *parent, const char *key, jval *v)
{
    jslot *s = amem(p->d, sizeof *s);
    if (!s) { p->err = 1; return NULL; }
    s->key = key;
    s->v = v;
    if (parent->last) parent->last->next = s; else parent->first = s;
    parent->last = s;
    parent->n++;
    return s;
}

static char *dupn(P *p, const char *s, size_t n)
{
    char *o = amem(p->d, n + 1);
    if (!o) { p->err = 1; return NULL; }
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

/* ------------------------------------------------------------ lexing */
static void ws(P *p)
{
    for (;;) {
        while (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r') p->p++;
        if (p->p[0] == '/' && p->p[1] == '*') {
            const char *e = strstr(p->p + 2, "*/");
            p->p = e ? e + 2 : p->p + strlen(p->p);
        } else return;
    }
}

static int is_id0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
static int is_id(int c) { return is_id0(c) || (c >= '0' && c <= '9'); }

static int ident(P *p, const char **s, size_t *n)
{
    ws(p);
    if (!is_id0((unsigned char)*p->p)) return 0;
    *s = p->p;
    while (is_id((unsigned char)*p->p)) p->p++;
    *n = p->p - *s;
    return 1;
}

static int word(const char *s, size_t n, const char *w) { return strlen(w) == n && !memcmp(s, w, n); }

static int expect(P *p, char c)
{
    ws(p);
    if (*p->p != c) { p->err = 1; return 0; }
    p->p++;
    return 1;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *put_utf8(char *w, unsigned cp)
{
    if (cp < 0x80) *w++ = cp;
    else if (cp < 0x800) { *w++ = 0xC0 | cp >> 6; *w++ = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { *w++ = 0xE0 | cp >> 12; *w++ = 0x80 | (cp >> 6 & 63); *w++ = 0x80 | (cp & 63); }
    else { *w++ = 0xF0 | cp >> 18; *w++ = 0x80 | (cp >> 12 & 63); *w++ = 0x80 | (cp >> 6 & 63); *w++ = 0x80 | (cp & 63); }
    return w;
}

static unsigned hexn(const char **r, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n && hexv((unsigned char)**r) >= 0; i++) v = v * 16 + hexv((unsigned char)*(*r)++);
    return v;
}

/* p->p at the opening quote; returns the decoded UTF-8 text */
static char *string(P *p)
{
    char q = *p->p++;
    const char *s = p->p, *e = s;
    while (*e && *e != q) { if (*e == '\\' && e[1]) e++; e++; }
    if (!*e) { p->err = 1; return NULL; }
    char *o = amem(p->d, (e - s) + 4), *w = o;
    if (!o) { p->err = 1; return NULL; }
    for (const char *r = s; r < e;) {
        if (*r != '\\') { *w++ = *r++; continue; }
        r++;
        char c = *r++;
        unsigned cp;
        switch (c) {
        case 'n': *w++ = '\n'; break;
        case 't': *w++ = '\t'; break;
        case 'r': *w++ = '\r'; break;
        case 'b': case 'f': case 'v': *w++ = ' '; break;
        case '0': break;
        case '\r': if (*r == '\n') r++; break;
        case '\n': break;
        case 'x': w = put_utf8(w, hexn(&r, 2)); break;
        case 'u':
            if (*r == '{') { r++; cp = hexn(&r, 6); if (*r == '}') r++; }
            else {
                cp = hexn(&r, 4);
                if (cp >= 0xD800 && cp < 0xDC00 && r[0] == '\\' && r[1] == 'u') {
                    const char *r2 = r + 2;
                    unsigned lo = hexn(&r2, 4);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); r = r2; }
                }
            }
            if (cp) w = put_utf8(w, cp);
            break;
        default: *w++ = c;
        }
    }
    *w = 0;
    p->p = e + 1;
    return o;
}

/* ------------------------------------------------------------ parsing */
static jval *value(P *p);

static jval *number(P *p)
{
    const char *s = p->p;
    if (*p->p == '-' || *p->p == '+') p->p++;
    while ((*p->p >= '0' && *p->p <= '9') || *p->p == '.' || *p->p == 'e' || *p->p == 'E' ||
           ((*p->p == '-' || *p->p == '+') && (p->p[-1] == 'e' || p->p[-1] == 'E')))
        p->p++;
    if (p->p == s) { p->err = 1; return NULL; }
    jval *v = node(p, J_NUM);
    if (!v) return NULL;
    v->s = dupn(p, s, p->p - s);
    v->num = strtod(v->s, NULL);
    return v;
}

/* comma separated values up to `close`, appended to `into` */
static int list(P *p, jval *into, char close)
{
    for (;;) {
        ws(p);
        if (*p->p == close) { p->p++; return 1; }
        if (*p->p == ',') { p->p++; add(p, into, NULL, node(p, J_NULL)); continue; }   /* hole */
        jval *v = value(p);
        if (!v) return 0;
        add(p, into, NULL, v);
        ws(p);
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == close) { p->p++; return 1; }
        p->err = 1;
        return 0;
    }
}

static jval *object(P *p)
{
    p->p++;   /* { */
    jval *o = node(p, J_OBJ);
    if (!o) return NULL;
    for (;;) {
        ws(p);
        if (*p->p == '}') { p->p++; return o; }
        const char *key, *ks;
        size_t kn;
        if (*p->p == '"' || *p->p == '\'') key = string(p);
        else if (ident(p, &ks, &kn)) key = dupn(p, ks, kn);
        else if ((*p->p >= '0' && *p->p <= '9') || *p->p == '-') { jval *k = number(p); key = k ? k->s : NULL; }
        else { p->err = 1; return NULL; }
        if (!key || !expect(p, ':')) return NULL;
        jval *v = value(p);
        if (!v) return NULL;
        add(p, o, key, v);
        ws(p);
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == '}') { p->p++; return o; }
        p->err = 1;
        return NULL;
    }
}

typedef struct asg { int var; const char *key; int idx; jval *v; struct asg *next; } asg;

static int env_index(jenv *e, const char *s, size_t n)
{
    for (int i = 0; i < e->n; i++) if (word(s, n, e->names[i])) return i;
    return -1;
}

static jval *deref(const jval *v);

/* after "(" : function(params){ assignments; return value }(args)  or  function..{..})(args) */
static jval *iife(P *p)
{
    const char *s;
    size_t n;
    if (!ident(p, &s, &n) || !word(s, n, "function")) { p->err = 1; return NULL; }
    ws(p);
    if (is_id0((unsigned char)*p->p)) ident(p, &s, &n);   /* named function */
    if (!expect(p, '(')) return NULL;
    jenv *e = amem(p->d, sizeof *e);
    if (!e) { p->err = 1; return NULL; }
    e->parent = p->env;
    int cap = 8;
    e->names = amem(p->d, cap * sizeof *e->names);
    for (;;) {
        ws(p);
        if (*p->p == ')') { p->p++; break; }
        if (!ident(p, &s, &n)) { p->err = 1; return NULL; }
        if (e->n == cap) {
            const char **nn = amem(p->d, cap * 2 * sizeof *nn);
            if (!nn) { p->err = 1; return NULL; }
            memcpy(nn, e->names, cap * sizeof *nn);
            e->names = nn;
            cap *= 2;
        }
        e->names[e->n++] = dupn(p, s, n);
        ws(p);
        if (*p->p == ',') p->p++;
    }
    e->vals = amem(p->d, (e->n ? e->n : 1) * sizeof *e->vals);
    if (!expect(p, '{')) return NULL;

    jenv *outer = p->env;
    p->env = e;
    asg *first = NULL, *last = NULL;
    jval *ret = NULL;
    for (;;) {
        if (!ident(p, &s, &n)) { p->err = 1; break; }
        if (word(s, n, "return")) {
            ret = value(p);
            ws(p);
            if (*p->p == ';') p->p++;
            if (!ret || !expect(p, '}')) ret = NULL;
            break;
        }
        asg *a = amem(p->d, sizeof *a);
        if (!a) { p->err = 1; break; }
        a->var = env_index(e, s, n);
        a->idx = -1;
        ws(p);
        if (*p->p == '.') {
            p->p++;
            if (!ident(p, &s, &n)) { p->err = 1; break; }
            a->key = dupn(p, s, n);
        } else if (*p->p == '[') {
            p->p++;
            jval *k = value(p);
            if (!k || !expect(p, ']')) { p->err = 1; break; }
            if (k->t == J_NUM) a->idx = (int)k->num;
            else a->key = k->s;
        } else { p->err = 1; break; }
        if (!expect(p, '=')) break;
        a->v = value(p);
        if (!a->v) break;
        ws(p);
        if (*p->p == ';' || *p->p == ',') p->p++;
        if (last) last->next = a; else first = a;
        last = a;
    }
    p->env = outer;
    if (!ret || p->err) { p->err = 1; return NULL; }

    ws(p);
    int wrapped = 1;                         /* (function(){..}(args)) : the caller's ")" follows the args */
    if (*p->p == ')') { p->p++; wrapped = 0; }   /* (function(){..})(args) */
    jval *args = node(p, J_ARR);
    if (!args || !expect(p, '(') || !list(p, args, ')')) return NULL;
    if (wrapped && !expect(p, ')')) return NULL;
    int i = 0;
    for (jslot *sl = args->first; sl && i < e->n; sl = sl->next) e->vals[i++] = sl->v;
    for (; i < e->n; i++) e->vals[i] = node(p, J_NULL);

    for (asg *a = first; a; a = a->next) {
        if (a->var < 0) continue;
        jval *t = deref(e->vals[a->var]);
        if (!t) continue;
        if (t->t == J_ARR && a->idx >= 0) {
            while (t->n <= a->idx) add(p, t, NULL, node(p, J_NULL));
            jslot *sl = t->first;
            for (int k = 0; k < a->idx; k++) sl = sl->next;
            sl->v = a->v;
        } else if (t->t == J_OBJ || t->t == J_ARR) {
            char kb[16];
            const char *key = a->key;
            if (!key) { snprintf(kb, sizeof kb, "%d", a->idx); key = dupn(p, kb, strlen(kb)); }
            jslot *sl;
            for (sl = t->first; sl; sl = sl->next) if (sl->key && !strcmp(sl->key, key)) break;
            if (sl) sl->v = a->v; else add(p, t, key, a->v);
        }
    }
    return ret;
}

static jval *value(P *p)
{
    if (++p->depth > 200) { p->err = 1; return NULL; }
    jval *v = NULL;
    ws(p);
    char c = *p->p;
    const char *s;
    size_t n;
    if (c == '{') v = object(p);
    else if (c == '[') {
        p->p++;
        v = node(p, J_ARR);
        if (v && !list(p, v, ']')) v = NULL;
    } else if (c == '"' || c == '\'') {
        v = node(p, J_STR);
        if (v && !(v->s = string(p))) v = NULL;
    } else if (c == '(') {
        p->p++;
        ws(p);
        if (!strncmp(p->p, "function", 8) && !is_id((unsigned char)p->p[8])) v = iife(p);
        else { v = value(p); if (v && !expect(p, ')')) v = NULL; }
    } else if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
        if ((c == '-' || c == '+') && is_id0((unsigned char)p->p[1])) { p->p++; v = value(p); }  /* -Infinity */
        else v = number(p);
    } else if (ident(p, &s, &n)) {
        if (word(s, n, "true") || word(s, n, "false")) { v = node(p, J_BOOL); if (v) v->num = s[0] == 't'; }
        else if (word(s, n, "null") || word(s, n, "undefined") || word(s, n, "NaN") || word(s, n, "Infinity")) v = node(p, J_NULL);
        else if (word(s, n, "void") || word(s, n, "new")) {
            v = value(p);
            if (v && s[0] == 'v') v = node(p, J_NULL);
        } else {
            /* dotted name, maybe called: Array(3), Object.create(null), Date(..) */
            const char *name = s;
            size_t nl = n;
            for (;;) {
                ws(p);
                if (*p->p != '.') break;
                p->p++;
                if (!ident(p, &s, &n)) { p->err = 1; break; }
                nl = (s + n) - name;
            }
            ws(p);
            if (!p->err && *p->p == '(') {
                p->p++;
                jval *args = node(p, J_ARR);
                if (args && list(p, args, ')')) {
                    jval *a0 = args->first ? args->first->v : NULL;
                    if (word(name, nl, "Array") && args->n == 1 && a0->t == J_NUM) {
                        v = node(p, J_ARR);
                        for (int i = 0; v && i < (int)a0->num && i < 100000; i++) add(p, v, NULL, node(p, J_NULL));
                    } else v = a0 ? a0 : node(p, J_OBJ);
                }
            } else if (!p->err) {
                v = node(p, J_REF);
                if (v) { v->s = dupn(p, name, nl); v->env = p->env; }
            }
        }
    } else p->err = 1;
    p->depth--;
    if (p->err) return NULL;
    return v;
}

/* ------------------------------------------------------------ access */
static jval *deref(const jval *v)
{
    for (int hop = 0; v && v->t == J_REF && hop < 32; hop++) {
        const jval *r = NULL;
        size_t n = strlen(v->s);
        for (jenv *e = v->env; e && !r; e = e->parent) {
            int i = env_index(e, v->s, n);
            if (i >= 0) r = e->vals[i];
        }
        v = r;
    }
    return (v && v->t == J_REF) ? NULL : (jval *)v;
}

jtype js_type(const jval *v) { v = deref(v); return v ? v->t : J_NULL; }

const jval *js_get(const jval *o, const char *key)
{
    o = deref(o);
    if (!o || o->t != J_OBJ) return NULL;
    for (jslot *s = o->first; s; s = s->next)
        if (s->key && !strcmp(s->key, key)) return deref(s->v);
    return NULL;
}

const jval *js_at(const jval *a, int i)
{
    a = deref(a);
    if (!a || (a->t != J_ARR && a->t != J_OBJ) || i < 0) return NULL;
    jslot *s = a->first;
    while (s && i--) s = s->next;
    return s ? deref(s->v) : NULL;
}

int js_len(const jval *v) { v = deref(v); return v && (v->t == J_ARR || v->t == J_OBJ) ? v->n : 0; }
const char *js_str(const jval *v) { v = deref(v); return v && v->t == J_STR ? v->s : NULL; }
const char *js_raw(const jval *v) { v = deref(v); return v && (v->t == J_STR || v->t == J_NUM) ? v->s : NULL; }
double js_num(const jval *v, double def)
{
    v = deref(v);
    if (!v) return def;
    if (v->t == J_NUM || v->t == J_BOOL) return v->num;
    if (v->t == J_STR && v->s[0]) { char *e; double d = strtod(v->s, &e); return e != v->s ? d : def; }
    return def;
}
int js_true(const jval *v) { v = deref(v); return v && v->t == J_BOOL && v->num != 0; }

const jslot *js_first(const jval *v)
{
    v = deref(v);
    return v && (v->t == J_ARR || v->t == J_OBJ) ? v->first : NULL;
}
const jslot *js_next(const jslot *s) { return s ? s->next : NULL; }
const jval *js_val(const jslot *s) { return s ? deref(s->v) : NULL; }
const char *js_key(const jslot *s) { return s ? s->key : NULL; }

static const jval *find_in(const jval *v, const char *key, int (*ok)(const jval *), int depth, long *budget)
{
    v = deref(v);
    if (!v || depth > 48 || --*budget < 0 || (v->t != J_OBJ && v->t != J_ARR)) return NULL;
    if (v->t == J_OBJ)
        for (jslot *s = v->first; s; s = s->next)
            if (s->key && !strcmp(s->key, key)) {
                const jval *r = deref(s->v);
                if (r && (!ok || ok(r))) return r;
            }
    for (jslot *s = v->first; s; s = s->next) {
        const jval *r = find_in(s->v, key, ok, depth + 1, budget);
        if (r) return r;
    }
    return NULL;
}

const jval *js_find(const jdoc *d, const char *key, int (*ok)(const jval *v))
{
    if (!d) return NULL;
    long budget = 400000;
    for (jslot *s = d->roots; s; s = s->next) {
        const jval *r = find_in(s->v, key, ok, 0, &budget);
        if (r) return r;
    }
    return NULL;
}

/* ------------------------------------------------------------ documents */
static jdoc *doc_new(void) { return calloc(1, sizeof(jdoc)); }

static void add_root(jdoc *d, const char *at)
{
    P p = { at, d, NULL, 0, 0 };
    jval *v = value(&p);
    if (!v || p.err) return;
    jslot *s = amem(d, sizeof *s);
    if (!s) return;
    s->v = v;
    if (d->last) d->last->next = s; else d->roots = s;
    d->last = s;
    d->nroots++;
}

jdoc *js_parse(const char *text)
{
    jdoc *d = doc_new();
    if (d) add_root(d, text);
    return d;
}

jdoc *js_page(const char *html)
{
    jdoc *d = doc_new();
    if (!d) return NULL;
    /* kit.start(app, element, { node_ids: [..], data: [ ... ], form: .. }) */
    const char *k = strstr(html, "kit.start(");
    if (k) {
        const char *dt = strstr(k, "data:");
        if (dt) add_root(d, dt + 5);
    }
    /* streamed promises: __sveltekit_xxx.resolve(id, () => value) */
    for (const char *r = html; (r = strstr(r, ".resolve(")); r++) {
        const char *ar = strstr(r, "=>");
        const char *close = strstr(r, "</script>");
        if (ar && (!close || ar < close)) add_root(d, ar + 2);
    }
    return d;
}

int js_roots(const jdoc *d) { return d ? d->nroots : 0; }

void js_free(jdoc *d)
{
    if (!d) return;
    for (chunk *c = d->mem, *n; c; c = n) { n = c->next; free(c); }
    free(d);
}
