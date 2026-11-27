/* native check of the scraper: ./t "query" [result#] [episode#] [sub|dub] [maxq] */
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
    int maxq = argc > 5 ? atoi(argv[5]) : 720;
    char err[256];
    net_log = lg;
    net_init(NULL);
    anime_t *a;
    int n = ani_search(q, &a, err, sizeof err);
    printf("search '%s': %d %s\n", q, n, n <= 0 ? err : "");
    for (int i = 0; i < n && i < 12; i++) printf("  [%d] %s al:%s | %s | %s sub %d dub %d\n", i, a[i].id, a[i].al, a[i].title, a[i].type, a[i].sub, a[i].dub);
    if (n <= pick) return 1;
    episode_t *e;
    int ne = ani_episodes(a[pick].id, &e, err, sizeof err);
    printf("episodes: %d %s\n", ne, ne <= 0 ? err : "");
    for (int i = 0; i < ne; i++) if (i < 3 || i >= ne - 2) printf("  ep %s '%s'\n", e[i].num, e[i].title);
    if (ne <= epi) return 1;
    source_t *src;
    int ns = ani_sources(a[pick].id, e[epi].num, mode, &src, err, sizeof err);
    printf("sources ep %s (%s): %d %s\n", e[epi].num, mode, ns, ns <= 0 ? err : "");
    for (int i = 0; i < ns; i++)
        printf("  [%d] %-4s %-5s %-24.24s hls %d ref %s origin %s subs %s\n", i, src[i].track, src[i].provider, src[i].server,
               src[i].hls, src[i].referer, src[i].origin, src[i].subs[0] ? "yes" : "-");
    if (ns <= 0) return 1;
    int k = ani_pick(src, ns, maxq, err, sizeof err);
    for (int i = 0; i < ns; i++) if (src[i].height) printf("  probed [%d] %s/%s -> %d (%dw)\n", i, src[i].provider, src[i].server, src[i].height, src[i].width);
    printf("pick (max %dp): %d %s\n", maxq, k, k < 0 ? err : "");
    if (k < 0) return 1;
    source_t *s = &src[k];
    printf("  url %s\n  ref %s origin %s\n  subs %s\n", s->url, s->referer, s->origin, s->subs);
    int first = k;
    for (int f = 0; f < 4; f++) {   /* as if playback failed: mark it and pick again */
        src[k].height = -3;
        int k2 = ani_pick(src, ns, maxq, err, sizeof err);
        printf("fallback %d after [%d] failed: %d %s %s %dp\n", f, k, k2, k2 < 0 ? err : src[k2].provider,
               k2 >= 0 ? src[k2].server : "", k2 >= 0 ? src[k2].height : 0);
        if (k2 < 0) break;
        k = k2;
    }
    for (int i = 0; i < ns; i++) if (src[i].height == -3) src[i].height = 0;
    s = &src[first];
    long st;
    for (int rep = 0; rep < 2; rep++) {   /* the player fetches the master again: must still work */
        char *m = net_get2(s->url, s->referer, s->origin, &st, NULL, err, sizeof err);
        printf("master fetch %d: HTTP %ld\n%.500s\n", rep, st, m ? m : err);
        free(m);
    }
    if (s->subs[0]) {
        char *v = net_get2(s->subs, s->referer, s->origin, &st, NULL, err, sizeof err);
        subs_t sb;
        int nc = v ? subs_parse(&sb, v) : 0;
        printf("subs HTTP %ld, %d cues (%.20s)\n", st, nc, v ? v : "");
        for (int i = 0; i < nc && i < 6; i++) printf("  %.2f-%.2f |%s|\n", sb.c[i].a, sb.c[i].b, sb.c[i].text);
        if (nc) printf("  at 300s: |%s|\n", subs_at(&sb, 300) ? subs_at(&sb, 300) : "(none)");
        free(v);
    }
    return 0;
}
