#!/usr/bin/env python3
# vp (debug only): fingerprint every displayed frame (4x4 grid of mean luma over the visible picture)
# for the first SIG_N frames, together with its timestamp, and log them all once collected.
# Compared offline with a reference decode this shows exactly which source frame was on screen.
p = "vp/vp.c"
c = open(p).read()
def rep(a, b):
    global c
    assert c.count(a) == 1, a[:70]
    c = c.replace(a, b)

rep("    double last_cpts, dp_min, dp_max, dc_min, dc_max, prev_clock; char ptslog[400]; int nptslog;",
    "    double last_cpts, dp_min, dp_max, dc_min, dc_max, prev_clock; char ptslog[400]; int nptslog;\n"
    "    int nsig; u8 sig[SIG_N][16]; float sig_pts[SIG_N];")
rep("#define VRING 6 ", "#define SIG_N 240\n#define VRING 6 ")

rep("static void blit(vp_ctx *c, const u16 *src)\n{", """/* debug: 4x4 grid of mean luma over the visible picture of a decoded frame */
static void frame_sig(vp_ctx *c, const u16 *src, u8 out[16])
{
    for (int gy = 0; gy < 4; gy++)
        for (int gx = 0; gx < 4; gx++) {
            u32 sum = 0, n = 0;
            int x0 = c->vis_w * gx / 4, x1 = c->vis_w * (gx + 1) / 4;
            int y0 = c->vis_h * gy / 4, y1 = c->vis_h * (gy + 1) / 4;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    u16 px = src[c->ymap[y] * c->src_w + c->xmap[x]];
                    u32 r = (px >> 11) & 31, g = (px >> 5) & 63, b = px & 31;
                    sum += (r * 8 * 77 + g * 4 * 150 + b * 8 * 29) >> 8;
                    n++;
                }
            out[gy * 4 + gx] = n ? sum / n : 0;
        }
}

static void blit(vp_ctx *c, const u16 *src)
{""")

rep("""                blit(c, c->ring[show]);
                c->shown++;""", """                blit(c, c->ring[show]);
                c->shown++;
                if (vp_debug && c->nsig < SIG_N) {
                    frame_sig(c, c->ring[show], c->sig[c->nsig]);
                    c->sig_pts[c->nsig] = c->ring_pts[show];
                    if (++c->nsig == SIG_N) {   /* dump once (stalls playback ~1 s, debug only) */
                        for (int i = 0; i < SIG_N; i++) {
                            char h[40];
                            for (int k = 0; k < 16; k++) snprintf(h + k * 2, 3, "%02x", c->sig[i][k]);
                            vp_log("sig %d %.3f %s", i, c->sig_pts[i], h);
                        }
                    }
                }""")
open(p, "w").write(c)
print("ok")
