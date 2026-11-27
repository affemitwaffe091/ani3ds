#ifndef JS_H
#define JS_H
/*
 * Just enough of a JavaScript literal parser to read the data a SvelteKit page
 * embeds (devalue output): objects with bare keys, arrays, strings, numbers,
 * and the "(function(a,b){a[0]=..;b.x=..;return {...}}(Array(2),{}))" form
 * that hoists shared values into variables. Variables resolve on access.
 */
typedef struct jval jval;
typedef struct jslot jslot;
typedef struct jdoc jdoc;
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ, J_REF } jtype;

/* parse every data chunk of a SvelteKit page (kit.start data + streamed .resolve() values) */
jdoc *js_page(const char *html);
/* parse one value starting at text (for tests) */
jdoc *js_parse(const char *text);
void js_free(jdoc *d);
int js_roots(const jdoc *d);

/* depth-first search for the first object member named key whose value passes ok (ok may be NULL) */
const jval *js_find(const jdoc *d, const char *key, int (*ok)(const jval *v));

jtype js_type(const jval *v);
const jval *js_get(const jval *obj, const char *key);   /* NULL if missing / not an object */
const jval *js_at(const jval *arr, int i);
int js_len(const jval *v);                              /* elements / members, 0 otherwise */
const char *js_str(const jval *v);                      /* string value, NULL otherwise */
const char *js_raw(const jval *v);                      /* string, or a number's source text */
double js_num(const jval *v, double def);
int js_true(const jval *v);

/* iteration over array elements / object members */
const jslot *js_first(const jval *v);
const jslot *js_next(const jslot *s);
const jval *js_val(const jslot *s);
const char *js_key(const jslot *s);
#endif
