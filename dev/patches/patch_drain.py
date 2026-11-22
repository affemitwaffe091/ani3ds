#!/usr/bin/env python3
"""MVD output protocol from Video player for 3DS (its v1.5.2 fixed B-frame stutter this way):
a picture can be written into the configured output buffer while a NAL unit is processed, and
more can be pending afterwards that only render calls hand out. Writes are detected with marker
pixels in the buffer corners; after a picture came out of processing, every pending one is
drained (render until nothing new) before the next packet is processed."""
import sys
def sub(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count: sys.exit(f"{path}: expected {count}x {old[:70]!r}, found {s.count(old)}")
    open(path, "w").write(s.replace(old, new))
V = "vp/vp.c"
sub(V, "    u32 hprev; int nrh, nwait, nwait_ok; u64 wait_ms; char rhlog[100];\n} vp_ctx;",
       "    int drain, n_proc_out, n_render_out, n_drain_out;   /* MVD output bookkeeping */\n} vp_ctx;")
# helpers before video_decode
sub(V, "static void video_decode(vp_ctx *c, AVPacket *p)\n{",
       "/* marker pixels in the output corners: changed = MVD wrote a picture */\n"
       "#define MVD_MARK 0x1111\n"
       "static void mvd_mark(vp_ctx *c)\n{\n"
       "    u16 *o = (u16 *)c->mvd_out; int w = c->src_w, h = c->src_h;\n"
       "    o[0] = o[w - 1] = o[(h - 1) * w] = o[h * w - 1] = MVD_MARK;\n"
       "    GSPGPU_FlushDataCache(o, w * h * 2);\n}\n"
       "static int mvd_written(vp_ctx *c)\n{\n"
       "    u16 *o = (u16 *)c->mvd_out; int w = c->src_w, h = c->src_h;\n"
       "    GSPGPU_InvalidateDataCache(o, w * h * 2);\n"
       "    return o[0] != MVD_MARK || o[w - 1] != MVD_MARK || o[(h - 1) * w] != MVD_MARK || o[h * w - 1] != MVD_MARK;\n}\n"
       "static void mvd_set_out(vp_ctx *c)\n{\n"
       "    c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->mvd_out);\n"
       "    MVDSTD_SetConfig(&c->cfg);\n}\n"
       "/* ask MVD for a pending picture; 1 = one was written to mvd_out */\n"
       "static int mvd_render_one(vp_ctx *c)\n{\n"
       "    mvd_mark(c);\n"
       "    for (int spin = 0; spin < 500 && !c->quit; spin++) {\n"
       "        Result r = mvdstdRenderVideoFrame(NULL, false);\n"
       "        if (mvd_written(c)) return 1;\n"
       "        if (r == MVD_STATUS_BUSY) { svcSleepThread(200000); continue; }\n"
       "        if (r != MVD_STATUS_OK && r != 0) {\n"
       "            c->last_rr = r;\n"
       "            if (c->render_fail++ < 5) vp_log(\"mvd render failed %08lx\", r);\n"
       "            if (!c->cpu_scale) { mvd_switch_to_cpu_scale(c); mvd_set_out(c); mvd_mark(c); continue; }\n"
       "        }\n"
       "        return 0;\n"
       "    }\n"
       "    return 0;\n}\n"
       "/* copy the picture in mvd_out into the display queue (waits for a free slot); 0 = quit/seek */\n"
       "static int mvd_emit(vp_ctx *c)\n{\n"
       "    c->dphase = 6;\n"
       "    for (;;) {\n"
       "        LightLock_Lock(&c->lock);\n"
       "        int full = c->r_count == VRING;\n"
       "        LightLock_Unlock(&c->lock);\n"
       "        if (!full || c->quit || c->seek_req) break;\n"
       "        svcSleepThread(3000000);\n"
       "    }\n"
       "    if (c->quit || c->seek_req) return 0;\n"
       "    LightLock_Lock(&c->lock);\n"
       "    int slot = (c->r_head + c->r_count) % VRING;\n"
       "    LightLock_Unlock(&c->lock);\n"
       "    memcpy(c->ring[slot], c->mvd_out, c->src_w * c->src_h * 2);\n"
       "    /* no timestamp yet: label_queue gives the smallest pending pts when the display takes it */\n"
       "    LightLock_Lock(&c->lock);\n"
       "    c->ring_pts[slot] = PTS_UNSET;\n"
       "    c->r_count++;\n"
       "    LightLock_Unlock(&c->lock);\n"
       "    if (c->frames_out++ == 0) vp_log(\"first frame decoded\");\n"
       "    return 1;\n}\n\n"
       "static void video_decode(vp_ctx *c, AVPacket *p)\n{")
# replace the processing + render part of video_decode
s = open(V).read()
a = s.index("    c->dphase = 4;\n    Result r = mvdstdProcessVideoFrame(b, n, 0, NULL);\n")
b = s.index("/* ------------------------------------------------------------------ threads */")
new = ("    /* pictures still pending from the last packet come out first */\n"
       "    if (c->drain) {\n"
       "        c->dphase = 5;\n"
       "        while (!c->quit && !c->seek_req && mvd_render_one(c)) {\n"
       "            c->n_drain_out++;\n"
       "            if (!mvd_emit(c)) return;\n"
       "        }\n"
       "        c->drain = 0;\n"
       "    }\n"
       "    if (c->quit || c->seek_req) return;\n"
       "    mvd_mark(c);\n"
       "    mvd_set_out(c);\n"
       "    c->dphase = 4;\n"
       "    Result r = mvdstdProcessVideoFrame(b, n, 0, NULL);\n"
       "    c->nproc++;\n"
       "    c->last_r = r;\n"
       "    if (c->mvd_first) r = mvdstdProcessVideoFrame(b, n, 0, NULL);\n"
       "    c->mvd_first = 0;\n"
       "    if (MVD_CHECKNALUPROC_SUCCESS(r)) {\n"
       "        int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;\n"
       "        LightLock_Lock(&c->lock);\n"
       "        pts_insert(c, ts == AV_NOPTS_VALUE ? -1 : ts * c->vtb - c->start_time);\n"
       "        LightLock_Unlock(&c->lock);\n"
       "    } else {\n"
       "        static int warned;\n"
       "        if (warned++ < 5) vp_log(\"mvd: frame rejected %08lx\", r);\n"
       "    }\n"
       "    if (mvd_written(c)) {            /* a picture came out while processing */\n"
       "        c->n_proc_out++;\n"
       "        c->drain = 1;                /* more may be pending: drain before the next packet */\n"
       "        mvd_emit(c);\n"
       "    } else if (MVD_CHECKNALUPROC_SUCCESS(r)) {\n"
       "        c->dphase = 5;\n"
       "        if (mvd_render_one(c)) { c->n_render_out++; mvd_emit(c); }\n"
       "    }\n"
       "}\n\n")
s = s[:a] + new + s[b:]
open(V, "w").write(s)
sub(V, '            vp_log("dbg4: picture repeated %d times, re-render gave a new one %d times (%.1f tries avg), %llu ms in total",\n'
       '                   c->nwait, c->nwait_ok, c->nwait_ok ? (double)c->nrh / c->nwait_ok : 0.0, c->wait_ms);\n',
       '            vp_log("dbg4: mvd pictures: %d while processing, %d by render, %d drained afterwards (packets %d)",\n'
       '                   c->n_proc_out, c->n_render_out, c->n_drain_out, c->nproc);\n')
sub(V, "    c->need_key = 1;\n    c->reader_eof = 0;\n", "    c->need_key = 1;\n    c->drain = 0;\n    c->reader_eof = 0;\n")
print("ok")
