#!/usr/bin/env python3
# vp: let MVD render each frame straight into its ring slot instead of a shared buffer that was
# copied right after mvdstdRenderVideoFrame() - MVD is still writing at that point (visible as
# horizontal bands from several frames at 720p). The main thread invalidates the slot's cache
# lines right before drawing it.
p = "vp/vp.c"
c = open(p).read()
def rep(a, b):
    global c
    assert c.count(a) == 1, a[:70]
    c = c.replace(a, b)

# ring slots in linear memory (MVD writes them by DMA), sized for the MVD output
rep("""    c->mvd_out = linearAlloc(c->pw * c->ph * 2);
    if (!c->mvd_out) return -1;
    for (int i = 0; i < VRING; i++) {
        c->ring[i] = malloc(c->pw * c->ph * 2);
        if (!c->ring[i]) return -1;
    }""", """    for (int i = 0; i < VRING; i++) {   /* MVD renders straight into these (linear memory) */
        c->ring[i] = linearAlloc(c->src_w * c->src_h * 2);
        if (!c->ring[i]) { vp_log("no linear memory for frame %d", i); return -1; }
    }""")

rep("""static void mvd_switch_to_cpu_scale(vp_ctx *c)
{
    vp_log("MVD scaling refused - scaling on the CPU");
    c->cpu_scale = 1;
    c->ow = c->pw; c->oh = c->ph;
    c->src_w = c->pw; c->src_h = c->ph;""", """static int mvd_switch_to_cpu_scale(vp_ctx *c)
{
    vp_log("MVD scaling refused - scaling on the CPU");
    c->cpu_scale = 1;
    c->ow = c->pw; c->oh = c->ph;
    c->src_w = c->pw; c->src_h = c->ph;
    /* only happens on the first frame, the ring is empty: regrow the slots to full size */
    for (int i = 0; i < VRING; i++) {
        linearFree(c->ring[i]);
        c->ring[i] = linearAlloc(c->src_w * c->src_h * 2);
        if (!c->ring[i]) { vp_log("no linear memory for full-size frames"); return -1; }
    }""")
rep("""    mvdstdGenerateDefaultConfig(&c->cfg, c->pw, c->ph, c->pw, c->ph, NULL, NULL, NULL);
    build_maps(c);
}""", """    mvdstdGenerateDefaultConfig(&c->cfg, c->pw, c->ph, c->pw, c->ph, NULL, NULL, NULL);
    build_maps(c);
    return 0;
}""")

rep("""    c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->mvd_out);
    c->dphase = 4;""", """    c->dphase = 4;""")

old_start = "    if (r == MVD_STATUS_FRAMEREADY) {\n        c->dphase = 5;\n"
old_end = "            if (c->frames_out++ == 0) vp_log(\"first frame decoded\");\n        }\n"
i = c.index(old_start); j = c.index(old_end, i) + len(old_end)
c = c[:i] + """    if (r == MVD_STATUS_FRAMEREADY) {
        /* wait for a free ring slot, then let MVD render the frame straight into it */
        c->dphase = 6;
        for (;;) {
            LightLock_Lock(&c->lock);
            int full = c->r_count == VRING;
            LightLock_Unlock(&c->lock);
            if (!full || c->quit || c->seek_req) break;
            svcSleepThread(3000000);
        }
        if (c->quit || c->seek_req) { c->mvd_first = 0; return; }
        LightLock_Lock(&c->lock);
        int slot = (c->r_head + c->r_count) % VRING;
        LightLock_Unlock(&c->lock);
        c->dphase = 5;
        c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->ring[slot]);
        Result rr = mvdstdRenderVideoFrame(&c->cfg, true);
        if (rr == MVD_STATUS_OK) rr = 0;   /* libctru reports success as MVD_STATUS_OK */
        if (rr && !c->cpu_scale) {
            vp_log("mvd render (scaled) failed %08lx", rr);
            if (mvd_switch_to_cpu_scale(c)) { c->mvd_first = 0; return; }
            c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->ring[slot]);
            rr = mvdstdRenderVideoFrame(&c->cfg, true);
            if (rr == MVD_STATUS_OK) rr = 0;
        }
        if (rr) { c->last_rr = rr; if (c->render_fail++ < 5) vp_log("mvd render failed %08lx", rr); }
        double t = pts_take_min(c);
        if (!rr && !c->mvd_first) {
            LightLock_Lock(&c->lock);
            c->ring_pts[slot] = t;
            c->r_count++;
            LightLock_Unlock(&c->lock);
            if (c->frames_out++ == 0) vp_log("first frame decoded");
        }
""" + c[j:]

rep("""                blit(c, c->ring[show]);""", """                /* the slot was written by MVD (DMA): drop stale cache lines before reading it */
                GSPGPU_InvalidateDataCache(c->ring[show], c->src_w * c->src_h * 2);
                blit(c, c->ring[show]);""")
rep("    for (int i = 0; i < VRING; i++) free(c->ring[i]);", "    for (int i = 0; i < VRING; i++) if (c->ring[i]) linearFree(c->ring[i]);")
open(p, "w").write(c)
print("ok")
