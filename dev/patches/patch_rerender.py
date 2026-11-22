#!/usr/bin/env python3
"""Measurement showed: after render, mvd_out holds the previous picture and never changes later
(the wait-for-copy fix got 0 of 918). So the render itself picked the old picture - most likely
the hardware was still decoding the new frame. Fix: if the rendered picture equals the last one,
render again every 2 ms for up to 20 ms."""
import sys
def sub(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count: sys.exit(f"{path}: expected {count}x {old[:70]!r}, found {s.count(old)}")
    open(path, "w").write(s.replace(old, new))
V = "vp/vp.c"
s = open(V).read()
a = s.index("            if (vp_debug && c->nrh < 100) {\n")
b = s.index("            memcpy(c->ring[slot], c->mvd_out, osz);\n")
new = ("            (void)ir;\n"
       "            u32 h = frame_hash(c, (const u16 *)c->mvd_out);\n"
       "            if (h == c->hprev) {\n"
       "                /* MVD gave the previous picture again: it was most likely still decoding the\n"
       "                 * new frame. Ask again (render, not just re-read) for up to 20 ms; in a really\n"
       "                 * still scene the picture stays the same and we give up. */\n"
       "                u64 t0 = osGetTime(); int tries = 0;\n"
       "                while (h == c->hprev && osGetTime() - t0 < 20) {\n"
       "                    svcSleepThread(2000000);\n"
       "                    c->cfg.physaddr_outdata0 = osConvertVirtToPhys(c->mvd_out);\n"
       "                    Result r2 = mvdstdRenderVideoFrame(&c->cfg, true);\n"
       "                    if (r2 != MVD_STATUS_OK && r2 != 0) break;\n"
       "                    GSPGPU_InvalidateDataCache(c->mvd_out, osz);\n"
       "                    h = frame_hash(c, (const u16 *)c->mvd_out);\n"
       "                    tries++;\n"
       "                }\n"
       "                c->nwait++;\n"
       "                if (h != c->hprev) { c->nwait_ok++; c->nrh += tries; }\n"
       "                c->wait_ms += osGetTime() - t0;\n"
       "            }\n"
       "            c->hprev = h;\n")
s = s[:a] + new + s[b:]
open(V, "w").write(s)
sub(V, '            vp_log("dbg4: waited for a new picture %d times (%d got one), %llu ms in total",\n'
       '                   c->nwait, c->nwait_ok, c->wait_ms);\n',
       '            vp_log("dbg4: picture repeated %d times, re-render gave a new one %d times (%.1f tries avg), %llu ms in total",\n'
       '                   c->nwait, c->nwait_ok, c->nwait_ok ? (double)c->nrh / c->nwait_ok : 0.0, c->wait_ms);\n')
print("ok")
