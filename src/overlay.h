#ifndef OVERLAY_H
#define OVERLAY_H
#include <3ds/types.h>
/* draw subtitle text (CP437, '\n' separated; NULL = nothing) onto the top framebuffer */
void overlay_draw(u16 *fb, const char *text);
void overlay_reset(void);
/* the text as a 400-wide mask (0 none, 1 outline, 2 text) for GPU drawing */
const u8 *overlay_mask(const char *text, int *rows, int *changed);
#endif
