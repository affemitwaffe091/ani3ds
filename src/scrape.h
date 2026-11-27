#ifndef SCRAPE_H
#define SCRAPE_H
/* C port of ani-cli 5.2.0's miruro provider (search, episodes, media tracks) */
#include <stddef.h>

#define ANI_BASE "https://miruro.cx"

typedef struct {
    char id[96];         /* miruro show id (used in /watch/<id>/..) */
    char al[16];         /* AniList id: what history is keyed on (ids can change, AniList does not) */
    char title[200];     /* english, else romaji; converted to the console charset */
    char type[16];       /* TV, Movie, OVA ... */
    int sub, dub;        /* released episode counts (0 = unknown/none) */
} anime_t;

typedef struct {
    char num[16];        /* episode number as listed ("1", "12.5") */
    char title[120];
} episode_t;

typedef struct {
    char track[8];       /* ssub (soft subs), sub (hard subs), dub */
    char provider[24], server[64];
    char url[1536];      /* hls master playlist (or a direct file) */
    char referer[256], origin[128];
    int hls;
    char subs[1024];     /* this provider's English subtitle file, or "" */
    int height;          /* set by ani_pick: 0 not checked, >0 best fitting variant, -1 none fits, -2 failed;
                          * the caller sets -3 when playback failed, so the next ani_pick skips it */
    int width;
} source_t;

/* all return a count / index, or <0 on error with err filled */
int ani_search(const char *query, anime_t **out, char *err, size_t errlen);
int ani_episodes(const char *id, episode_t **out, char *err, size_t errlen);
/* every server for one episode, in the order they should be tried */
int ani_sources(const char *id, const char *ep, const char *mode /* "sub" | "dub" */, source_t **out, char *err, size_t errlen);
/* check master playlists and return the source to play: in the first group (track + subs)
 * that has anything fitting, the first one with a variant of exactly maxq, else the tallest
 * that fits (<= maxq, H.264). Sources with height -3 (failed to play) are skipped. */
int ani_pick(source_t *v, int n, int maxq, char *err, size_t errlen);
#endif
