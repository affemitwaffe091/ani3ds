/*
 * ani3ds - ani-cli for the New 3DS.
 * Same source and flow as ani-cli 5.2.0 (miruro -> provider servers -> HLS),
 * playback through the hardware H.264 decoder, soft subtitles on screen.
 *
 * Top screen: lists.  Bottom screen: help, status, log.
 * Files: sdmc:/3ds/ani3ds/{history.txt,config.txt,log.txt}
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <sys/stat.h>
#include "net.h"
#include "scrape.h"
#include "subs.h"
#include "overlay.h"
#include "vp.h"

/* libctru's default main stack is 32 KB; ffmpeg's stream probing decodes
 * H.264 on this thread and needs much more (crashed in ff_h264 at 0.1) */
u32 __stacksize__ = 2 * 1024 * 1024;

#define VERSION "0.2.0"
#define DIR "sdmc:/3ds/ani3ds"
#define SOC_SIZE 0x200000
#define ROWS 24                 /* list rows on the top screen */
#define HIST_MAX 40

static PrintConsole top, bot;
static FILE *g_logf;

/* ------------------------------------------------------------ state */
/* key: "al:<anilist>:<miruro id>" (ani-cli keys history on AniList, the miruro id is only a cache),
 * "mi:<miruro id>" for shows without AniList id; anything else is a 0.1.x hianime slug */
typedef struct { char key[160]; char title[200]; char ep[16]; } hist_t;
static hist_t g_hist[HIST_MAX];
static int g_nhist;

static char g_mode[8] = "sub";
static int g_subs_on = 1;
static int g_maxq = 480;        /* largest stream height to pick when smaller ones exist */

static anime_t *g_res;          /* search results */
static int g_nres;
static char g_query[256];

static anime_t g_show;          /* the show whose episodes are listed */
static episode_t *g_eps;
static int g_neps;

static subs_t g_subs;           /* current subtitle track */
static char g_subs_url[1024];   /* ... and where it came from */
static source_t *g_src;         /* servers of the current episode */
static int g_nsrc;

/* ------------------------------------------------------------ output */
static void present(void) { gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }

static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (g_logf) { fputs(b, g_logf); fflush(g_logf); }
    consoleSelect(&bot);
    printf("%.118s", b);
    consoleSelect(&top);
}

static void bottom_help(const char *lines)
{
    consoleSelect(&bot);
    consoleClear();
    printf("\x1b[36;1mani3ds " VERSION "\x1b[0m  \x1b[33m%s\x1b[0m  subs \x1b[33m%s\x1b[0m  max \x1b[33m%dp\x1b[0m\n", g_mode, g_subs_on ? "on" : "off", g_maxq);
    printf("\x1b[2m---------------------------------------\x1b[0m\n");
    if (*lines) {   /* no second rule when there is no help text (playback screen) */
        printf("%s", lines);
        printf("\x1b[2m---------------------------------------\x1b[0m\n");
    }
    consoleSelect(&top);
}

static void busy(const char *what)
{
    consoleSelect(&bot);
    printf("\x1b[33m%s...\x1b[0m\n", what);
    consoleSelect(&top);
    present();
}

/* fit a string into n columns */
static void fit(char *out, const char *in, int n)
{
    int L = strlen(in);
    if (L <= n) { strcpy(out, in); return; }
    memcpy(out, in, n - 2);
    strcpy(out + n - 2, "..");
}

/* ------------------------------------------------------------ files */
static void save_config(void)
{
    FILE *f = fopen(DIR "/config.txt", "w");
    if (!f) return;
    fprintf(f, "mode=%s\nsubs=%d\nmaxq=%d\n", g_mode, g_subs_on, g_maxq);
    fclose(f);
}

static void load_config(void)
{
    FILE *f = fopen(DIR "/config.txt", "r");
    if (!f) return;
    char l[128];
    while (fgets(l, sizeof l, f)) {
        if (!strncmp(l, "mode=dub", 8)) strcpy(g_mode, "dub");
        if (!strncmp(l, "mode=sub", 8)) strcpy(g_mode, "sub");
        if (!strncmp(l, "subs=", 5)) g_subs_on = atoi(l + 5) != 0;
        if (!strncmp(l, "maxq=", 5)) g_maxq = atoi(l + 5) >= 720 ? 720 : 480;
    }
    fclose(f);
}

static void load_history(void)
{
    g_nhist = 0;
    FILE *f = fopen(DIR "/history.txt", "r");
    if (!f) return;
    char l[512];
    while (g_nhist < HIST_MAX && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *t1 = strchr(l, '\t');
        if (!t1) continue;
        *t1++ = 0;
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = 0;
        hist_t *h = &g_hist[g_nhist++];
        snprintf(h->key, sizeof h->key, "%s", l);
        snprintf(h->ep, sizeof h->ep, "%s", t1);
        snprintf(h->title, sizeof h->title, "%s", t2);
    }
    fclose(f);
}

static void save_history(void)
{
    FILE *f = fopen(DIR "/history.txt", "w");
    if (!f) return;
    for (int i = 0; i < g_nhist; i++) fprintf(f, "%s\t%s\t%s\n", g_hist[i].key, g_hist[i].ep, g_hist[i].title);
    fclose(f);
}

static void make_key(const anime_t *a, char *out, size_t cap)
{
    if (a->al[0]) snprintf(out, cap, "al:%s:%s", a->al, a->id);
    else snprintf(out, cap, "mi:%s", a->id);
}

/* 1 = current format (al / id filled), 0 = a 0.1.x hianime entry */
static int parse_key(const char *key, anime_t *a)
{
    a->al[0] = a->id[0] = 0;
    if (!strncmp(key, "al:", 3)) {
        const char *c = strchr(key + 3, ':');
        int n = c ? (int)(c - key - 3) : (int)strlen(key + 3);
        snprintf(a->al, sizeof a->al, "%.*s", n, key + 3);
        if (c) snprintf(a->id, sizeof a->id, "%s", c + 1);
        return 1;
    }
    if (!strncmp(key, "mi:", 3)) { snprintf(a->id, sizeof a->id, "%s", key + 3); return 1; }
    return 0;
}

static int hist_find(const anime_t *a)
{
    for (int i = 0; i < g_nhist; i++) {
        anime_t h;
        if (!parse_key(g_hist[i].key, &h)) continue;
        if (a->al[0] && h.al[0] ? !strcmp(a->al, h.al) : (h.id[0] && !strcmp(a->id, h.id))) return i;
    }
    return -1;
}

/* move/insert the show to the top with its last watched episode */
static void history_update(const anime_t *a, const char *ep)
{
    hist_t h;
    make_key(a, h.key, sizeof h.key);
    snprintf(h.title, sizeof h.title, "%s", a->title);
    snprintf(h.ep, sizeof h.ep, "%s", ep);
    int i = hist_find(a);
    if (i < 0) { if (g_nhist < HIST_MAX) g_nhist++; i = g_nhist - 1; }
    memmove(&g_hist[1], &g_hist[0], i * sizeof h);
    g_hist[0] = h;
    save_history();
}

static const char *history_ep(const anime_t *a)
{
    int i = hist_find(a);
    return i >= 0 ? g_hist[i].ep : NULL;
}

/* ------------------------------------------------------------ generic list */
typedef void (*row_fn)(int i, char *out, int width);

/* draws `n` rows with selection `sel`, scrolled so sel is visible */
static void draw_list(const char *title, int n, int sel, row_fn row, const char *footer)
{
    consoleSelect(&top);
    consoleClear();
    char t[51];
    fit(t, title, 50);
    printf("\x1b[1;1H\x1b[36;1m%s\x1b[0m\n", t);
    printf("\x1b[2m-------------------------------------------------\x1b[0m");
    int first = sel - ROWS / 2;
    if (first > n - ROWS) first = n - ROWS;
    if (first < 0) first = 0;
    for (int r = 0; r < ROWS && first + r < n; r++) {
        int i = first + r;
        char line[64];
        row(i, line, 47);
        if (i == sel) { int L = strlen(line); while (L < 48) line[L++] = ' '; line[L] = 0; }
        printf("\x1b[%d;1H%s%s\x1b[0m", 3 + r, i == sel ? "\x1b[30;47m>" : " ", line);
    }
    if (n > ROWS) printf("\x1b[28;1H\x1b[2m%d/%d\x1b[0m", sel + 1, n);
    if (footer) printf("\x1b[29;1H%s", footer);
    present();
}

/* standard list navigation; returns key that ended it (A/B/X/Y/START/SELECT) */
static u32 list_nav(int *sel, int n, u32 k)
{
    if (!n) return 0;
    if (k & KEY_DOWN) *sel = (*sel + 1) % n;
    if (k & KEY_UP) *sel = (*sel - 1 + n) % n;
    if (k & KEY_RIGHT) { *sel += ROWS; if (*sel >= n) *sel = n - 1; }
    if (k & KEY_LEFT) { *sel -= ROWS; if (*sel < 0) *sel = 0; }
    if (k & KEY_R) { *sel += 100; if (*sel >= n) *sel = n - 1; }
    if (k & KEY_L) { *sel -= 100; if (*sel < 0) *sel = 0; }
    return k & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT);
}

static u32 wait_keys(void)
{
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k) return k;
        u32 h = hidKeysHeld();
        static int rep;
        if (h & (KEY_UP | KEY_DOWN)) { if (++rep > 18 && rep % 3 == 0) return h & (KEY_UP | KEY_DOWN); }
        else rep = 0;
        gspWaitForVBlank();
    }
    return KEY_START;
}

/* ------------------------------------------------------------ player glue */
static double g_dur, g_pos;
static int g_next_req;
static volatile int g_playing, g_woke;   /* lid opened again while the player ran */

/* newest player log line; written by any thread, shown by the main thread */
static char g_lastlog[160];
static volatile int g_lastlog_new;

/* 804 MHz always; the New 3DS L2 cache unless sdmc:/3ds/ani3ds/nol2 exists (turning it off did
 * not change the repeated pictures). Re-applied after HOME, where libctru resets it. */
static int g_l2;
static void cpu_config(void)
{
    FILE *f = fopen(DIR "/nol2", "r");
    g_l2 = f == NULL;
    if (f) fclose(f);
    Result r = -1;
    if (R_SUCCEEDED(ptmSysmInit())) { r = PTMSYSM_ConfigureNew3DSCPU(g_l2 ? 3 : 1); ptmSysmExit(); }
    if (g_logf) { fprintf(g_logf, "cpu: 804MHz, L2 %s (r=%08lx)\n", g_l2 ? "on" : "off", r); fflush(g_logf); }
}
static void cpu_hook(APT_HookType t, void *p)
{
    (void)p;
    if (t == APTHOOK_ONRESTORE || t == APTHOOK_ONWAKEUP) cpu_config();
    /* the console slept with the lid closed (the app is not runnable in sleep): Wi-Fi was off,
     * so the open connections are dead. Stop the player; play_episode reopens at the same spot. */
    if (t == APTHOOK_ONWAKEUP && g_playing) {
        g_woke = 1;
        vp_quit_req = 1;
        vp_net_abort = 1;
    }
}

/* after sleep the console needs a few seconds to rejoin the access point */
static int wait_wifi(int secs)
{
    for (int i = 0; i < secs * 10; i++) {
        u32 st = 0;
        if (R_SUCCEEDED(ACU_GetWifiStatus(&st)) && st) return 1;
        svcSleepThread(100000000LL);
    }
    return 0;
}

static void vp_status_cb(const char *title, const vp_state *st)
{
    consoleSelect(&bot);
    if (g_lastlog_new) {   /* newest player log line, above the status lines */
        g_lastlog_new = 0;
        printf("\x1b[5;1H\x1b[0J%s", g_lastlog);
    }
    int p = (int)st->pos, d = (int)st->dur;
    g_dur = st->dur;
    if (!st->buffering || st->pos > 0) g_pos = st->pos;
    printf("\x1b[26;1H\x1b[2K\x1b[36m%s %d:%02d", st->paused ? "PAUSED " : st->buffering ? "loading" : "playing", p / 60, p % 60);
    if (d > 0) printf("/%d:%02d", d / 60, d % 60);
    printf(" %dp %.0fkb/s\x1b[0m", st->height, st->net_kbps);
    printf("\x1b[25;1H\x1b[2K\x1b[33mframes/s  drawn %2d  new %2d  decoded %2d%s\x1b[0m",
           st->fps_drawn, st->fps_new, st->fps_dec, g_l2 ? "" : "  noL2");
    printf("\x1b[27;1H\x1b[2KA pause  B stop  </> 10s  L/R 1min");
    printf("\x1b[28;1H\x1b[2KY subtitles %s  X next episode", g_subs_on ? "off" : "on ");
    consoleSelect(&top);
    gfxFlushBuffers();
}

/* may run on the player's network/decoder threads: file + buffer only, no console */
static void vp_log_cb(const char *l)
{
    if (g_logf) { fprintf(g_logf, "%s\n", l); fflush(g_logf); }
    snprintf(g_lastlog, sizeof g_lastlog, "%.118s", l);
    g_lastlog_new = 1;
}

static void vp_overlay_cb(u16 *fb, double pts)
{
    overlay_draw(fb, g_subs_on ? subs_at(&g_subs, pts) : NULL);
}

static void vp_tick_cb(u32 k, double pts)
{
    if (k & KEY_Y) { g_subs_on = !g_subs_on; save_config(); }
    if (k & KEY_X) { g_next_req = 1; vp_quit_req = 1; }
}

static const vp_ui g_ui = { vp_log_cb, vp_status_cb, vp_overlay_cb, vp_tick_cb };

/* resolve + play episode index ei; returns 0 played, 1 next requested/finished, <0 error */
static int play_episode(int ei, char *msg, size_t msglen)
{
    episode_t *e = &g_eps[ei];
    char title[260], err[200];
    snprintf(title, sizeof title, "%s - Episode %s", g_show.title, e->num);
    bottom_help("");
    consoleSelect(&bot);
    printf("\x1b[36m%.40s\x1b[0m\nEpisode %s %.30s\n", g_show.title, e->num, e->title);
    consoleSelect(&top);
    consoleClear();
    printf("\x1b[12;1H   Fetching episode %s (%s)...\n   B is ignored until the stream opens.", e->num, g_mode);
    present();

    g_subs_url[0] = 0;
    subs_free(&g_subs);
    overlay_reset();
    double resume = 0;   /* > 0: continue there (after sleep, or on the next server after a failure) */
    int fails = 0;
    for (;;) {
        busy("finding servers");
        free(g_src);
        g_src = NULL;
        g_nsrc = ani_sources(g_show.id, e->num, g_mode, &g_src, err, sizeof err);
        if (g_nsrc <= 0) { snprintf(msg, msglen, "Episode %s: %s", e->num, err); return -1; }
        logf_("%d servers\n", g_nsrc);
        for (;;) {
            busy("checking streams");
            int k = ani_pick(g_src, g_nsrc, g_maxq, err, sizeof err);
            if (k < 0) { snprintf(msg, msglen, "Episode %s: %s", e->num, err); return -1; }
            source_t *s = &g_src[k];
            if (g_logf) fprintf(g_logf, "server %s %s/%s %dp\nstream %s\nreferer %s origin %s\nsubs %s\n",
                                s->track, s->provider, s->server, s->height, s->url, s->referer, s->origin, s->subs);
            logf_("server %s/%s %dp (%s)\n", s->provider, s->server, s->height,
                  !strcmp(s->track, "ssub") ? (s->subs[0] ? "soft subs" : "no subs") : !strcmp(s->track, "sub") ? "hard subs" : "dub");
            if (strcmp(g_subs_url, s->subs)) {   /* subtitles belong to the provider */
                subs_free(&g_subs);
                overlay_reset();
                snprintf(g_subs_url, sizeof g_subs_url, "%s", s->subs);
                if (s->subs[0]) {
                    busy("loading subtitles");
                    long st = 0;
                    char *v = NULL;
                    /* the subtitle host may only accept the Referer of another server of the same
                     * provider (bee: megaplay works, vidtube gets 403) */
                    for (int i = -1; i < g_nsrc && !(v && st == 200); i++) {
                        const source_t *h = i < 0 ? s : &g_src[i];
                        if (i >= 0 && (strcmp(h->provider, s->provider) || strcmp(h->subs, s->subs) || !strcmp(h->referer, s->referer))) continue;
                        free(v);
                        v = net_get2(s->subs, h->referer, h->origin, &st, NULL, err, sizeof err);
                    }
                    if (v && st == 200) logf_("subtitles: %d lines\n", subs_parse(&g_subs, v));
                    else logf_("subtitles failed (%ld %s)\n", st, v ? "" : err);
                    free(v);
                }
            }
            history_update(&g_show, e->num);
            busy(resume > 0 ? "reopening video" : "opening video");

            g_dur = 0;
            g_pos = resume;
            g_next_req = 0;
            g_woke = 0;
            vp_set_ui(&g_ui);
            vp_max_h = g_maxq;
            snprintf(vp_origin, sizeof vp_origin, "%s", s->origin);
            vp_start_at = resume;
            char perr[160];
            g_playing = 1;
            int r = vp_play(s->url, s->referer, title, perr, sizeof perr);
            g_playing = 0;
            vp_start_at = 0;
            consoleInit(GFX_TOP, &top);       /* the player owned the top screen */
            if (g_woke) {
                g_woke = 0;
                resume = g_pos > 3 ? g_pos - 2 : 0;
                consoleClear();
                printf("\x1b[12;1H   Welcome back - continuing at %d:%02d\n   (waiting for Wi-Fi)", (int)resume / 60, (int)resume % 60);
                present();
                logf_("woke up, continuing at %d:%02d\n", (int)resume / 60, (int)resume % 60);
                if (!wait_wifi(30)) { snprintf(msg, msglen, "No Wi-Fi after sleep (stopped at %d:%02d)", (int)resume / 60, (int)resume % 60); return -1; }
                break;   /* fresh server links: the old ones may have expired while asleep */
            }
            if (r < 0 && g_dur > 0 && g_pos > g_dur - 15) r = 0;   /* broke off in the end credits: count as played */
            if (r < 0) {
                logf_("\x1b[31m%s\x1b[0m\n", perr);
                if (g_pos > 10) resume = g_pos - 2;   /* it played for a while: continue on another server */
                s->height = -3;
                if (++fails < 4) continue;
                snprintf(msg, msglen, "Playback failed: %s", perr);
                return -1;
            }
            snprintf(msg, msglen, "Played episode %s", e->num);
            return g_next_req ? 1 : 0;
        }
    }
}

/* ani-cli's post-episode menu */
static void episode_session(int ei)
{
    char msg[256];
    for (;;) {
        int r = play_episode(ei, msg, sizeof msg);
        if (r == 1 && ei + 1 < g_neps) { ei++; continue; }   /* X during playback: straight to next */
        static const char *opts[] = { "next", "replay", "previous", "select episode", "back to search" };
        int sel = (r < 0 || ei + 1 >= g_neps) ? 1 : 0;
        for (;;) {
            consoleSelect(&top);
            consoleClear();
            char t[51];
            fit(t, g_show.title, 50);
            printf("\x1b[1;1H\x1b[36;1m%s\x1b[0m\n", t);
            printf("Episode %s of %d  (%s)\n\n", g_eps[ei].num, g_neps, g_mode);
            printf("%s%s\x1b[0m\n\n", r < 0 ? "\x1b[31m" : "\x1b[32m", msg);
            for (int i = 0; i < 5; i++) {
                int dis = (i == 0 && ei + 1 >= g_neps) || (i == 2 && ei == 0);
                printf("  %s%s %s\x1b[0m\n", i == sel ? "\x1b[30;47m>" : " ", dis ? "\x1b[2m" : "", opts[i]);
            }
            present();
            bottom_help("Up/Down choose, A ok, B episode list\nSELECT switch sub/dub\n");
            u32 k = wait_keys();
            if (k & KEY_DOWN) sel = (sel + 1) % 5;
            if (k & KEY_UP) sel = (sel + 4) % 5;
            if (k & KEY_SELECT) { strcpy(g_mode, strcmp(g_mode, "sub") ? "sub" : "dub"); save_config(); }
            if (k & (KEY_B | KEY_START)) return;
            if (k & KEY_A) {
                if (sel == 0 && ei + 1 < g_neps) { ei++; break; }
                if (sel == 1) break;
                if (sel == 2 && ei > 0) { ei--; break; }
                if (sel == 3) return;
                if (sel == 4) { g_neps = -1; return; }   /* signals "leave the show" */
            }
        }
    }
}

/* ------------------------------------------------------------ screens */
static void ep_row(int i, char *out, int w)
{
    char t[64];
    fit(t, g_eps[i].title, w - 8);
    snprintf(out, w + 1, "%5s  %s", g_eps[i].num, t);
}

/* like ani-cli's history: find the show's current miruro id by searching its title for the AniList id */
static int resolve_anilist(anime_t *a, char *err, size_t errlen)
{
    anime_t *r;
    busy("finding the show again");
    int n = ani_search(a->title, &r, err, errlen);
    for (int i = 0; i < n; i++)
        if (!strcmp(r[i].al, a->al)) { *a = r[i]; free(r); return 0; }
    if (n > 0) { free(r); snprintf(err, errlen, "not found again by its title"); }
    return -1;
}

static void show_screen(const anime_t *a)
{
    g_show = *a;
    char err[200];
    busy("loading episodes");
    free(g_eps); g_eps = NULL;
    g_neps = g_show.id[0] ? ani_episodes(g_show.id, &g_eps, err, sizeof err) : -1;
    if (g_neps <= 0 && g_show.al[0] && !resolve_anilist(&g_show, err, sizeof err)) {
        busy("loading episodes");
        g_neps = ani_episodes(g_show.id, &g_eps, err, sizeof err);
    }
    if (g_neps <= 0) {
        bottom_help("");
        logf_("\x1b[31m%s: %s\x1b[0m\n", g_show.title, err);
        present();
        return;
    }
    /* start on the episode after the last one watched */
    int sel = 0;
    const char *last = history_ep(&g_show);
    if (last) for (int i = 0; i < g_neps; i++) if (!strcmp(g_eps[i].num, last)) { sel = i + 1 < g_neps ? i + 1 : i; break; }
    for (;;) {
        char title[260];
        snprintf(title, sizeof title, "%s  (%d eps)", g_show.title, g_neps);
        draw_list(title, g_neps, sel, ep_row, last ? "\x1b[2mcursor starts after your last watched episode\x1b[0m" : NULL);
        bottom_help("A play   B back\nUp/Down move, Left/Right page\nL/R jump 100 episodes\nSELECT switch sub/dub\n");
        u32 k = list_nav(&sel, g_neps, wait_keys());
        if (k & (KEY_B | KEY_START)) return;
        if (k & KEY_SELECT) { strcpy(g_mode, strcmp(g_mode, "sub") ? "sub" : "dub"); save_config(); }
        if (k & KEY_A) {
            episode_session(sel);
            if (g_neps < 0) return;
            last = history_ep(&g_show);
            if (last) for (int i = 0; i < g_neps; i++) if (!strcmp(g_eps[i].num, last)) { sel = i; break; }
        }
    }
}

static void res_row(int i, char *out, int w)
{
    char t[64], info[24];
    if (g_res[i].dub) snprintf(info, sizeof info, "%d/%d", g_res[i].sub, g_res[i].dub);
    else snprintf(info, sizeof info, "%d", g_res[i].sub);
    char ty[8];
    snprintf(ty, sizeof ty, "%.5s", g_res[i].type);
    char *sp = strchr(ty, ' ');
    if (sp) *sp = 0;
    int tw = w - 15;
    fit(t, g_res[i].title, tw);
    snprintf(out, w + 1, "%-*s %-5s %8s", tw, t, ty, info);
}

static int ask_query(void)
{
    SwkbdState kb;
    char buf[256];
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, sizeof buf - 1);
    swkbdSetHintText(&kb, "Search anime");
    swkbdSetInitialText(&kb, g_query);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM || !buf[0]) return 0;
    snprintf(g_query, sizeof g_query, "%s", buf);
    return 1;
}

static void search_screen(void)
{
    if (!ask_query()) return;
    char err[200];
    busy("searching");
    free(g_res); g_res = NULL;
    g_nres = ani_search(g_query, &g_res, err, sizeof err);
    if (g_nres <= 0) { bottom_help(""); logf_("\x1b[31m'%s': %s\x1b[0m\n", g_query, err); return; }
    int sel = 0;
    for (;;) {
        char title[300];
        snprintf(title, sizeof title, "Results for \"%s\"", g_query);
        draw_list(title, g_nres, sel, res_row, "\x1b[2mnumbers: episodes sub/dub\x1b[0m");
        bottom_help("A open   B back   X new search\nSELECT switch sub/dub\n");
        u32 k = list_nav(&sel, g_nres, wait_keys());
        if (k & (KEY_B | KEY_START)) return;
        if (k & KEY_SELECT) { strcpy(g_mode, strcmp(g_mode, "sub") ? "sub" : "dub"); save_config(); }
        if (k & KEY_X) { search_screen(); return; }
        if (k & KEY_A) {
            show_screen(&g_res[sel]);
            if (g_neps < 0) return;
        }
    }
}

static void hist_row(int i, char *out, int w)
{
    char t[64];
    fit(t, g_hist[i].title, w - 10);
    snprintf(out, w + 1, "%-*s  ep %s", w - 10, t, g_hist[i].ep);
}

/* a 0.1.x entry keeps a hianime slug: find the show on miruro by its title (asking when it is
 * not an exact match), then rewrite the entry with the new key and keep the episode */
static void legacy_open(int hi)
{
    hist_t old = g_hist[hi];
    char err[200];
    busy("finding the show on the new site");
    free(g_res); g_res = NULL;
    g_nres = ani_search(old.title, &g_res, err, sizeof err);
    if (g_nres <= 0) { bottom_help(""); logf_("\x1b[31m'%s': %s\x1b[0m\nY removes the entry\n", old.title, err); return; }
    int pick = -1;
    for (int i = 0; i < g_nres && pick < 0; i++) if (!strcasecmp(g_res[i].title, old.title)) pick = i;
    if (pick < 0) {
        int sel = 0;
        for (;;) {
            char title[300];
            snprintf(title, sizeof title, "Which one is \"%s\"?", old.title);
            draw_list(title, g_nres, sel, res_row, "\x1b[2mold history entry: pick the show once\x1b[0m");
            bottom_help("A this one   B back\n");
            u32 k = list_nav(&sel, g_nres, wait_keys());
            if (k & (KEY_B | KEY_START)) return;
            if (k & KEY_A) { pick = sel; break; }
        }
    }
    anime_t a = g_res[pick];
    int dup = hist_find(&a);
    if (dup >= 0 && dup != hi) {   /* already watched with 0.2: drop the old entry */
        memmove(&g_hist[hi], &g_hist[hi + 1], (g_nhist - hi - 1) * sizeof g_hist[0]);
        g_nhist--;
    } else {
        make_key(&a, g_hist[hi].key, sizeof g_hist[hi].key);
        snprintf(g_hist[hi].title, sizeof g_hist[hi].title, "%s", a.title);
    }
    save_history();
    show_screen(&a);
}

static void home(void)
{
    int sel = 0;
    for (;;) {
        load_history();
        if (sel >= g_nhist) sel = g_nhist ? g_nhist - 1 : 0;
        if (g_nhist) draw_list("ani3ds - continue watching", g_nhist, sel, hist_row, NULL);
        else {
            consoleSelect(&top);
            consoleClear();
            printf("\x1b[1;1H\x1b[36;1mani3ds " VERSION "\x1b[0m  - ani-cli for the New 3DS\n\n");
            printf("Nothing watched yet.\n\nPress \x1b[33mX\x1b[0m to search for an anime.\n");
            present();
        }
        bottom_help(g_nhist ? "A continue   X search\nY remove from history\nSELECT sub/dub   R subtitles on/off\nL max quality 480p/720p   START exit\n"
                            : "X search\nSELECT sub/dub   R subtitles on/off\nL max quality 480p/720p   START exit\n");
        u32 raw = wait_keys();
        if (raw & KEY_R) { g_subs_on = !g_subs_on; save_config(); continue; }
        if (raw & KEY_L) { g_maxq = g_maxq == 480 ? 720 : 480; save_config(); continue; }
        u32 k = list_nav(&sel, g_nhist, raw & ~(KEY_L | KEY_R));
        if (!g_nhist) k = raw;
        if (k & KEY_START) return;
        if (k & KEY_SELECT) { strcpy(g_mode, strcmp(g_mode, "sub") ? "sub" : "dub"); save_config(); }
        if (k & KEY_X) search_screen();
        if ((k & KEY_Y) && g_nhist) {
            memmove(&g_hist[sel], &g_hist[sel + 1], (g_nhist - sel - 1) * sizeof g_hist[0]);
            g_nhist--;
            save_history();
        }
        if ((k & KEY_A) && g_nhist) {
            anime_t a;
            memset(&a, 0, sizeof a);
            snprintf(a.title, sizeof a.title, "%s", g_hist[sel].title);
            if (parse_key(g_hist[sel].key, &a)) show_screen(&a);
            else legacy_open(sel);
            sel = 0;
        }
    }
}

int main(int argc, char **argv)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bot);
    mkdir("sdmc:/3ds", 0777);
    mkdir(DIR, 0777);
    romfsInit();
    g_logf = fopen(DIR "/log.txt", "w");
    static aptHookCookie cpu_cookie;
    cpu_config();
    aptHook(&cpu_cookie, cpu_hook, NULL);
    net_log = logf_;

    consoleSelect(&bot);
    printf("ani3ds starting...\n");
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (!n3ds) printf("\x1b[31mThis is not a New 3DS: video needs the\nNew 3DS hardware decoder.\x1b[0m\n");
    u32 *socbuf = memalign(0x1000, SOC_SIZE);
    Result rc = socbuf ? socInit(socbuf, SOC_SIZE) : -1;
    if (R_FAILED(rc)) printf("\x1b[31msocInit failed: %08lx\x1b[0m\n", (unsigned long)rc);
    acInit();                    /* Wi-Fi status, to wait for the access point after sleep */
    aptSetSleepAllowed(true);    /* closing the lid sleeps; playback continues when it opens */
    u64 t0 = osGetTime();
    if (net_init("romfs:/ca-bundle")) printf("\x1b[31mnetwork init failed\x1b[0m\n");
    printf("ready (%.1fs)\n", (osGetTime() - t0) / 1000.0);
    load_config();
    {   /* sdmc:/3ds/ani3ds/debug present -> verbose player log in log.txt */
        FILE *f = fopen(DIR "/debug", "r");
        if (f) { vp_debug = 1; fclose(f); printf("debug logging on\n"); }
    }

    home();

    subs_free(&g_subs);
    free(g_res); free(g_eps); free(g_src);
    net_exit();
    acExit();
    if (g_logf) fclose(g_logf);
    socExit();
    free(socbuf);
    romfsExit();
    gfxExit();
    return 0;
}
