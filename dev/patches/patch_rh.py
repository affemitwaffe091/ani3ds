#!/usr/bin/env python3
"""Measure when the rendered picture really lands in mvd_out (first 100 frames, debug), then a
candidate fix: wait (max 30 ms) until mvd_out holds a picture different from the last one.
Sig dump moved to the end of playback (it stalled playback ~1.4 s). L2 back on by default."""
import sys
def sub(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count: sys.exit(f"{path}: expected {count}x {old[:70]!r}, found {s.count(old)}")
    open(path, "w").write(s.replace(old, new))
V = "vp/vp.c"
sub(V, "    Result last_r, last_rr;\n} vp_ctx;",
       "    Result last_r, last_rr;\n"
       "    u32 hprev; int nrh, nwait, nwait_ok; u64 wait_ms; char rhlog[100];\n} vp_ctx;")
sub(V, "        if (!rr && !c->mvd_first) {\n"
       "            GSPGPU_InvalidateDataCache(c->mvd_out, c->src_w * c->src_h * 2);\n"
       "            memcpy(c->ring[slot], c->mvd_out, c->src_w * c->src_h * 2);\n",
       "        if (!rr && !c->mvd_first) {\n"
       "            u32 osz = c->src_w * c->src_h * 2;\n"
       "            Result ir = GSPGPU_InvalidateDataCache(c->mvd_out, osz);\n"
       "            if (vp_debug && c->nrh < 100) {\n"
       "                /* debug: when does the new picture actually appear in mvd_out? */\n"
       "                u32 h1 = frame_hash(c, (const u16 *)c->mvd_out), h2, h3;\n"
       "                svcSleepThread(4000000);  GSPGPU_InvalidateDataCache(c->mvd_out, osz); h2 = frame_hash(c, (const u16 *)c->mvd_out);\n"
       "                svcSleepThread(16000000); GSPGPU_InvalidateDataCache(c->mvd_out, osz); h3 = frame_hash(c, (const u16 *)c->mvd_out);\n"
       "                char *L = c->rhlog + strlen(c->rhlog);\n"
       "                *L++ = h1 == c->hprev ? '=' : 'n';   /* new picture right after render? */\n"
       "                *L++ = h2 == h1 ? '.' : '4';         /* changed within the next 4 ms */\n"
       "                *L++ = h3 == h2 ? '.' : 'L';         /* changed later (4-20 ms) */\n"
       "                *L++ = ' '; *L = 0;\n"
       "                c->hprev = h3;\n"
       "                if (++c->nrh % 20 == 0) { vp_log(\"rh %s inv=%08lx\", c->rhlog, ir); c->rhlog[0] = 0; }\n"
       "            } else {\n"
       "                /* candidate fix: if mvd_out still holds the previous picture, wait for the new one */\n"
       "                u32 h = frame_hash(c, (const u16 *)c->mvd_out);\n"
       "                if (h == c->hprev) {\n"
       "                    u64 t0 = osGetTime();\n"
       "                    while (h == c->hprev && osGetTime() - t0 < 30) {\n"
       "                        svcSleepThread(2000000);\n"
       "                        GSPGPU_InvalidateDataCache(c->mvd_out, osz);\n"
       "                        h = frame_hash(c, (const u16 *)c->mvd_out);\n"
       "                    }\n"
       "                    c->nwait++;\n"
       "                    if (h != c->hprev) c->nwait_ok++;\n"
       "                    c->wait_ms += osGetTime() - t0;\n"
       "                }\n"
       "                c->hprev = h;\n"
       "            }\n"
       "            memcpy(c->ring[slot], c->mvd_out, osz);\n")
sub(V, "                    if (++c->nsig == SIG_N) {   /* dump once (stalls playback ~1 s, debug only) */\n"
       "                        for (int i = 0; i < SIG_N; i++) {\n"
       "                            char h[40];\n"
       "                            for (int k = 0; k < 16; k++) snprintf(h + k * 2, 3, \"%02x\", c->sig[i][k]);\n"
       "                            vp_log(\"sig %d %.3f %s\", i, c->sig_pts[i], h);\n"
       "                        }\n"
       "                    }\n",
       "                    c->nsig++;   /* dumped when playback ends */\n")
sub(V, "    if (c->mvd_out) linearFree(c->mvd_out);\n",
       "    if (vp_debug && c->nsig == SIG_N)\n"
       "        for (int i = 0; i < SIG_N; i++) {\n"
       "            char h[40];\n"
       "            for (int k = 0; k < 16; k++) snprintf(h + k * 2, 3, \"%02x\", c->sig[i][k]);\n"
       "            vp_log(\"sig %d %.3f %s\", i, c->sig_pts[i], h);\n"
       "        }\n"
       "    if (c->mvd_out) linearFree(c->mvd_out);\n")
sub(V, "                   c->dp_min * 1000, c->dp_max * 1000, c->dc_min * 1000, c->dc_max * 1000);\n",
       "                   c->dp_min * 1000, c->dp_max * 1000, c->dc_min * 1000, c->dc_max * 1000);\n"
       "            vp_log(\"dbg4: waited for a new picture %d times (%d got one), %llu ms in total\",\n"
       "                   c->nwait, c->nwait_ok, c->wait_ms);\n")
M = "src/main3ds.c"
sub(M, '    FILE *f = fopen(DIR "/l2", "r");\n    g_l2 = f != NULL;\n',
       '    FILE *f = fopen(DIR "/nol2", "r");\n    g_l2 = f == NULL;\n')
sub(M, 'g_l2 ? "  L2" : ""', 'g_l2 ? "" : "  noL2"')
sub(M, "/* 804 MHz always; the New 3DS L2 cache only if sdmc:/3ds/ani3ds/l2 exists (test: the CPU read\n"
       " * old decoder pictures through it).",
       "/* 804 MHz always; the New 3DS L2 cache unless sdmc:/3ds/ani3ds/nol2 exists (turning it off did\n"
       " * not change the repeated pictures).")
print("ok")
