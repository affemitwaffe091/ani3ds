#ifndef OVERLAY_H
#define OVERLAY_H
#include <3ds/types.h>
/* draw subtitle text (CP437, '\n' separated; NULL = nothing) onto the top framebuffer */
void overlay_draw(u16 *fb, const char *text, int wide);
void overlay_reset(void);
#endif
