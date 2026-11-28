#ifndef OVERLAY_H
#define OVERLAY_H
#include <3ds/types.h>
/* draw subtitle text (CP437, '\n' separated; NULL = nothing) onto the top framebuffer */
void overlay_draw(u16 *fb, const char *text);
void overlay_reset(void);
/* subtitle text size in percent of the 8x8 font (default 100, OVERLAY_MIN_PCT..200) */
#define OVERLAY_MIN_PCT 75
void overlay_set_size(int pct);
int overlay_size(void);
/* the text as a 400-wide mask for GPU drawing: per pixel high nibble = text coverage 0-15,
 * low nibble = black outline coverage 0-15 (text wins) */
const u8 *overlay_mask(const char *text, int *rows, int *changed);
#endif
