/*
 * Subtitle overlay for the top screen. The top framebuffer is 240x400 stored
 * column-major and rotated: pixel (x, y) lives at fb[x * 240 + 239 - y].
 * Text uses libctru's 8x8 console font (CP437, MSB = leftmost pixel) with a
 * 1px black outline. The rendered text is cached as a mask and only rebuilt
 * when the cue changes, so each frame just stamps the mask.
 */
#include <3ds.h>
#include <string.h>
#include "overlay.h"

extern const u8 default_font_bin[];

#define W 400
#define H 240
#define MAXL 4
#define LINE_H 10
#define MASK_H (MAXL * LINE_H + 2)
#define MAXC 48

static u8 g_mask[MASK_H][W];      /* 0 none, 1 outline, 2 text */
static const char *g_cur;          /* cue currently in the mask */
static int g_rows;                 /* mask height in use */

static void put_char(int x0, int y0, unsigned char ch)
{
    const u8 *g = default_font_bin + ch * 8;
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (g[r] & (0x80 >> b)) {
                int x = x0 + b, y = y0 + r;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && xx < W && yy >= 0 && yy < MASK_H && !g_mask[yy][xx]) g_mask[yy][xx] = 1;
                    }
            }
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (g[r] & (0x80 >> b) && x0 + b < W && y0 + r < MASK_H) g_mask[y0 + r][x0 + b] = 2;
}

/* word-wrap `text` into at most MAXL lines of MAXC chars */
static int wrap(const char *text, char lines[MAXL][MAXC + 1])
{
    int n = 0;
    const char *p = text;
    while (*p && n < MAXL) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        while (seg > 0 && n < MAXL) {
            size_t take = seg;
            if (take > MAXC) {
                take = MAXC;
                while (take > 0 && p[take] != ' ') take--;
                if (!take) take = MAXC;
            }
            size_t L = take;
            while (L && p[L - 1] == ' ') L--;
            memcpy(lines[n], p, L);
            lines[n][L] = 0;
            if (L) n++;
            p += take;
            seg -= take;
            while (seg && *p == ' ') { p++; seg--; }
        }
        p = nl ? nl + 1 : p + seg;
    }
    return n;
}

static void build(const char *text)
{
    memset(g_mask, 0, sizeof g_mask);
    g_cur = text;
    g_rows = 0;
    if (!text) return;
    char lines[MAXL][MAXC + 1];
    int n = wrap(text, lines);
    g_rows = n * LINE_H + 2;
    for (int i = 0; i < n; i++) {
        int len = strlen(lines[i]);
        int x = (W - len * 8) / 2;
        for (int k = 0; k < len; k++) put_char(x + k * 8, 1 + i * LINE_H, (unsigned char)lines[i][k]);
    }
}

void overlay_draw(u16 *fb, const char *text)
{
    if (text != g_cur) build(text);
    if (!g_rows) return;
    int y0 = H - 4 - g_rows;
    for (int x = 0; x < W; x++) {
        u16 *col = fb + x * H;
        for (int r = 0; r < g_rows; r++) {
            u8 m = g_mask[r][x];
            if (m) col[H - 1 - (y0 + r)] = m == 2 ? 0xFFFF : 0x0000;
        }
    }
}

void overlay_reset(void) { build(NULL); }
