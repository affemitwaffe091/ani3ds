#ifndef SCRAPE_H
#define SCRAPE_H
/* C port of ani-cli 5.1.8's hianime provider (search, episodes, ZokoAnime embed) */
#include <stddef.h>

#define ANI_BASE "https://hianime.at"

typedef struct {
    char slug[160];      /* e.g. frieren-beyond-journeys-end-481 */
    char title[200];
    char type[16];       /* TV, Movie, OVA ... */
    int sub, dub;        /* released episode counts (0 = unknown/none) */
} anime_t;

typedef struct {
    int id;              /* hianime episode id (for the servers api) */
    char num[16];        /* episode number as shown ("1", "12.5") */
    char title[120];
} episode_t;

typedef struct {
    char m3u8[1536];     /* master playlist */
    char referer[256];   /* the stream host wants the embed origin */
    char subs[1536];     /* default WebVTT subtitle track, or "" */
    char embed[512];
} stream_t;

/* all return a count / 0 on success, <0 on error with err filled */
int ani_search(const char *query, anime_t **out, char *err, size_t errlen);
int ani_episodes(const char *slug, episode_t **out, char *err, size_t errlen);
int ani_resolve(int episode_id, const char *mode /* "sub" | "dub" */, stream_t *st, char *err, size_t errlen);

/* helpers shared with the UI */
void html_unescape(char *s);
#endif
