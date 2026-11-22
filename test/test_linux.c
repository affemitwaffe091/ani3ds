/* native check of the scraper: ./t "query" [result#] [episode#] [sub|dub] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "net.h"
#include "scrape.h"
#include "subs.h"

static void lg(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }

int main(int argc, char **argv)
{
    const char *q = argc > 1 ? argv[1] : "frieren";
    int pick = argc > 2 ? atoi(argv[2]) : 0, epi = argc > 3 ? atoi(argv[3]) : 0;
    const char *mode = argc > 4 ? argv[4] : "sub";
    char err[256];
    net_log = lg;
    net_init(NULL);
    anime_t *a;
    int n = ani_search(q, &a, err, sizeof err);
    printf("search '%s': %d %s\n", q, n, n <= 0 ? err : "");
    for (int i = 0; i < n; i++) printf("  [%d] %s | %s | %s sub %d dub %d\n", i, a[i].slug, a[i].title, a[i].type, a[i].sub, a[i].dub);
    if (n <= pick) return 1;
    episode_t *e;
    int ne = ani_episodes(a[pick].slug, &e, err, sizeof err);
    printf("episodes: %d %s\n", ne, ne <= 0 ? err : "");
    for (int i = 0; i < ne && i < 3; i++) printf("  ep %s id %d '%s'\n", e[i].num, e[i].id, e[i].title);
    if (ne <= epi) return 1;
    stream_t st;
    int r = ani_resolve(e[epi].id, mode, &st, err, sizeof err);
    printf("resolve ep %s (%s): %d %s\n  embed %s\n  m3u8 %s\n  ref %s\n  subs %s\n", e[epi].num, mode, r, r ? err : "", st.embed, st.m3u8, st.referer, st.subs);
    if (r) return 1;
    long s;
    char *m = net_get(st.m3u8, st.referer, &s, NULL, err, sizeof err);
    printf("master HTTP %ld:\n%.400s\n", s, m ? m : err);
    free(m);
    if (st.subs[0]) {
        char *v = net_get(st.subs, st.referer, &s, NULL, err, sizeof err);
        subs_t sb;
        int nc = v ? subs_parse(&sb, v) : 0;
        printf("subs HTTP %ld, %d cues\n", s, nc);
        for (int i = 0; i < nc && i < 4; i++) printf("  %.2f-%.2f |%s|\n", sb.c[i].a, sb.c[i].b, sb.c[i].text);
        if (nc) printf("  at 300s: |%s|\n", subs_at(&sb, 300) ? subs_at(&sb, 300) : "(none)");
        free(v);
    }
    return 0;
}
