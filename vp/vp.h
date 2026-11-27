#ifndef VP_H
#define VP_H
/* PlayerBox video player for New 3DS: HLS / MP4 (H.264 + AAC/MP3/Opus),
 * hardware H.264 decoding (MVD), fullscreen on the top screen. */
#include <3ds/types.h>

typedef struct { volatile u64 bytes; volatile int opens, seeks, waiting; } vp_net_stats_t;
extern vp_net_stats_t vp_net_stats;
extern volatile int vp_net_abort;
extern char vp_referer[1024];
extern char vp_origin[256];       /* sent as Origin with every request when set (miruro's stream hosts want it) */
extern double vp_start_at;        /* start playback at this position (seconds) when > 0 */
void vp_net_set_priority(s32 prio);
char *vp_fetch_text(const char *url, long *status);
void vp_log(const char *fmt, ...);

typedef struct {
    double pos, dur, buffered;   /* seconds */
    int buffering, paused;
    int width, height;           /* source video size */
    const char *codec;
    double net_kbps;
    int fps_drawn, fps_new, fps_dec;   /* last second: frames drawn, of them a changed picture, decoded */
} vp_state;

typedef struct {
    void (*log)(const char *line);                       /* diagnostics */
    void (*status)(const char *title, const vp_state *s); /* ~4x per second, for the bottom screen */
    /* ani3ds additions (optional, may be NULL) */
    void (*overlay)(u16 *fb, double pts);   /* top framebuffer (rotated 240x400 RGB565) after each new frame */
    void (*tick)(u32 kdown, double pts);    /* once per UI loop iteration, with that iteration's keys */
} vp_ui;
extern volatile int vp_quit_req;
extern int vp_max_h;                        /* hls: prefer the largest variant up to this height (default 480) */
extern int vp_debug;                        /* verbose ffmpeg + network logging */            /* set (e.g. from tick) to leave vp_play like B does */
void vp_set_ui(const vp_ui *ui);

/* Blocking: plays fullscreen on the top screen until B, the end, or an error.
 * Returns 0 when playback ran, <0 on error (err gets a short reason). */
int vp_play(const char *url, const char *referer, const char *title, char *err, size_t errlen);
#endif
