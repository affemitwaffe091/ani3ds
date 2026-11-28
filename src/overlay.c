/*
 * Subtitle overlay for the top screen. The top framebuffer is 240x400 stored
 * column-major and rotated: pixel (x, y) lives at fb[x * 240 + 239 - y].
 * Text uses libctru's 8x8 console font (CP437, MSB = leftmost pixel): as is at
 * 100 %, smoothed (Scale2x + box filter, anti-aliased) at 75-200 %, with a 1px
 * black outline. The
 * rendered text is cached as a mask and only rebuilt when the cue or the size
 * changes, so each frame just stamps the mask.
 */
#include <3ds.h>
#include <string.h>
#include "overlay.h"

extern const u8 default_font_bin[];

#define W 400
#define H 240
#define MAXL 4
#define MAX_PCT 200
#define MASK_H (MAXL * (8 * MAX_PCT / 100 + 2) + 2)
#define MAXC 66                    /* chars per line at the smallest size */

static u8 g_mask[MASK_H][W];      /* high nibble: text coverage 0-15, low nibble: outline 0-15 */
static const char *g_cur;          /* cue currently in the mask */
static int g_rows;                 /* mask height in use */
static int g_pct = 100;            /* text size in percent of the 8x8 font */
static int g_force;                /* size changed: rebuild even if the cue is the same */

/* Smooth glyphs for sizes other than 100 %: the 8x8 glyph is blown up to 32x32 with Scale2x
 * twice (rounds the stair steps of diagonals and curves, keeps straight edges sharp), then
 * box-filtered down to the target size, which gives anti-aliased edges. Cached per char. */
static u32 g_hi[256][32];          /* 32x32 bitmaps, bit 31 = leftmost pixel */
static u8 g_hi_ok[256];

static void scale2x(const u8 *in, int n, u8 *out)   /* n x n -> 2n x 2n, 0/1 pixels */
{
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int e = in[y * n + x];
            int up = y > 0 ? in[(y - 1) * n + x] : 0, dn = y < n - 1 ? in[(y + 1) * n + x] : 0;
            int lf = x > 0 ? in[y * n + x - 1] : 0, rt = x < n - 1 ? in[y * n + x + 1] : 0;
            int e0 = e, e1 = e, e2 = e, e3 = e;
            if (up != dn && lf != rt) {
                if (lf == up) e0 = lf;
                if (up == rt) e1 = rt;
                if (lf == dn) e2 = lf;
                if (dn == rt) e3 = rt;
            }
            u8 *o = out + (2 * y) * 2 * n + 2 * x;
            o[0] = e0; o[1] = e1; o[2 * n] = e2; o[2 * n + 1] = e3;
        }
}

static const u32 *glyph_hi(unsigned char ch)
{
    if (!g_hi_ok[ch]) {
        static u8 a[8 * 8], b[16 * 16], c[32 * 32];
        const u8 *g = default_font_bin + ch * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) a[y * 8 + x] = (g[y] >> (7 - x)) & 1;
        scale2x(a, 8, b);
        scale2x(b, 16, c);
        for (int y = 0; y < 32; y++) {
            u32 r = 0;
            for (int x = 0; x < 32; x++) r |= (u32)c[y * 32 + x] << (31 - x);
            g_hi[ch][y] = r;
        }
        g_hi_ok[ch] = 1;
    }
    return g_hi[ch];
}

/* text coverage of one glyph into the mask (high nibble), gs x gs pixels at x0, y0 */
static void put_char(int x0, int y0, unsigned char ch, int gs)
{
    if (gs == 8) {   /* 100 %: the plain console font, pixel for pixel */
        const u8 *g = default_font_bin + ch * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (g[y] & (0x80 >> x) && x0 + x >= 0 && x0 + x < W && y0 + y < MASK_H) g_mask[y0 + y][x0 + x] = 0xF0;
        return;
    }
    const u32 *h = glyph_hi(ch);
    for (int y = 0; y < gs; y++) {
        int ya = y * 32 / gs, yb = (y + 1) * 32 / gs;
        for (int x = 0; x < gs; x++) {
            int xa = x * 32 / gs, xb = (x + 1) * 32 / gs, n = 0;
            u32 m = (xb - xa >= 32 ? 0xFFFFFFFFu : ((1u << (xb - xa)) - 1) << (32 - xb));
            for (int yy = ya; yy < yb; yy++) n += __builtin_popcount(h[yy] & m);
            int cov = (n * 15 + (yb - ya) * (xb - xa) / 2) / ((yb - ya) * (xb - xa));
            int xx = x0 + x, yy = y0 + y;
            if (cov && xx >= 0 && xx < W && yy < MASK_H) g_mask[yy][xx] = cov << 4;
        }
    }
}

/* outline: the strongest text coverage in the 3x3 neighbourhood, into the low nibble */
static void outline(int rows)
{
    static u8 t[MASK_H][W];
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < W; x++) t[y][x] = g_mask[y][x] >> 4;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < W; x++) {
            int o = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && yy < rows && xx >= 0 && xx < W && t[yy][xx] > o) o = t[yy][xx];
                }
            g_mask[y][x] |= o;
        }
}

/* word-wrap `text` into at most MAXL lines of maxc chars */
static int wrap(const char *text, char lines[MAXL][MAXC + 1], int maxc)
{
    int n = 0;
    const char *p = text;
    while (*p && n < MAXL) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        while (seg > 0 && n < MAXL) {
            size_t take = seg;
            if (take > (size_t)maxc) {
                take = maxc;
                while (take > 0 && p[take] != ' ') take--;
                if (!take) take = maxc;
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
    int gs = (8 * g_pct + 50) / 100, lh = gs + 2, maxc = (W - 4) / gs;
    if (maxc > MAXC) maxc = MAXC;
    char lines[MAXL][MAXC + 1];
    int n = wrap(text, lines, maxc);
    g_rows = n * lh + 2;
    for (int i = 0; i < n; i++) {
        int len = strlen(lines[i]);
        int x = (W - len * gs) / 2;
        for (int k = 0; k < len; k++) put_char(x + k * gs, 1 + i * lh, (unsigned char)lines[i][k], gs);
    }
    outline(g_rows);
}

void overlay_set_size(int pct)
{
    if (pct < OVERLAY_MIN_PCT) pct = OVERLAY_MIN_PCT;
    if (pct > MAX_PCT) pct = MAX_PCT;
    if (pct != g_pct) { g_pct = pct; g_force = 1; }
}

int overlay_size(void) { return g_pct; }

void overlay_draw(u16 *fb, const char *text)
{
    if (text != g_cur || g_force) { build(text); g_force = 0; }
    if (!g_rows) return;
    int y0 = H - 4 - g_rows;
    for (int x = 0; x < W; x++) {
        u16 *col = fb + x * H;
        for (int r = 0; r < g_rows; r++) {
            u8 m = g_mask[r][x];
            if ((m >> 4) >= 8) col[H - 1 - (y0 + r)] = 0xFFFF;
            else if ((m & 15) >= 8) col[H - 1 - (y0 + r)] = 0x0000;
        }
    }
}

const u8 *overlay_mask(const char *text, int *rows, int *changed)
{
    static int dirty = 1;
    if (text != g_cur || g_force) { build(text); g_force = 0; dirty = 1; }
    *changed = dirty;
    dirty = 0;
    *rows = g_rows;
    return &g_mask[0][0];
}

void overlay_reset(void) { build(NULL); }
