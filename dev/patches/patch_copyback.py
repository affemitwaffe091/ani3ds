#!/usr/bin/env python3
# vp: back to FourthTube's frame path. Rendering MVD output straight into the display slots left
# stale slot contents on screen (frames up to 0.6 s old, measured by fingerprinting the displayed
# frames against a reference decode): render into one shared linear buffer and copy it at once into
# a normal-memory slot, like FourthTube (with the lazy timestamps from patch_lazypts).
p = "vp/vp.c"
c = open(p).read()
def rep(a, b):
    global c
    assert c.count(a) == 1, a[:70]
    c = c.replace(a, b)

rep("""    for (int i = 0; i < VRING; i++) {   /* MVD renders straight into these (linear memory) */
        c->ring[i] = linearAlloc(c->src_w * c->src_h * 2);
        if (!c->ring[i]) { vp_log("no linear memory for frame %d", i); return -1; }
    }""", """    c->mvd_out = linearAlloc(c->pw * c->ph * 2);   /* MVD renders here, then it is copied */
    if (!c->mvd_out) { vp_log("no linear memory for the decoder output"); return -1; }
    for (int i = 0; i < VRING; i++) {
        c->ring[i] = malloc(c->pw * c->ph * 2);    /* big enough for CPU scaling too */
        if (!c->ring[i]) { vp_log("no memory for frame %d", i); return -1; }
    }""")
rep("""    /* only happens on the first frame, the ring is empty: regrow the slots to full size */
    for (int i = 0; i < VRING; i++) {
        linearFree(c->ring[i]);
        c->ring[i] = linearAlloc(c->src_w * c->src_h * 2);
        if (!c->ring[i]) { vp_log("no linear memory for full-size frames"); return -1; }
    }
""", "")
rep("""        c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->ring[slot]);
        Result rr = mvdstdRenderVideoFrame(&c->cfg, true);""", """        c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->mvd_out);
        Result rr = mvdstdRenderVideoFrame(&c->cfg, true);""")
rep("""            c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->ring[slot]);
            rr = mvdstdRenderVideoFrame(&c->cfg, true);""", """            c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->mvd_out);
            rr = mvdstdRenderVideoFrame(&c->cfg, true);""")
rep("""        if (!rr && !c->mvd_first) {
            LightLock_Lock(&c->lock);
            c->ring_pts[slot] = PTS_UNSET;""", """        if (!rr && !c->mvd_first) {
            GSPGPU_InvalidateDataCache(c->mvd_out, c->src_w * c->src_h * 2);
            memcpy(c->ring[slot], c->mvd_out, c->src_w * c->src_h * 2);
            LightLock_Lock(&c->lock);
            c->ring_pts[slot] = PTS_UNSET;""")
rep("""                /* the slot was written by MVD (DMA): drop stale cache lines before reading it */
                GSPGPU_InvalidateDataCache(c->ring[show], c->src_w * c->src_h * 2);
""", "")
rep("    for (int i = 0; i < VRING; i++) if (c->ring[i]) linearFree(c->ring[i]);", "    for (int i = 0; i < VRING; i++) free(c->ring[i]);")
open(p, "w").write(c)
print("ok")
