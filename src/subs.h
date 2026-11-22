#ifndef SUBS_H
#define SUBS_H
typedef struct { double a, b; char *text; } cue_t;
typedef struct { cue_t *c; int n; int last; } subs_t;

/* parse WebVTT (or SRT) text; tags are stripped, text converted to the 8x8 font's charset */
int subs_parse(subs_t *s, const char *vtt);
/* text of the cue showing at time t (seconds), or NULL */
const char *subs_at(subs_t *s, double t);
void subs_free(subs_t *s);
#endif
