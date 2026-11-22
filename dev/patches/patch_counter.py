#!/usr/bin/env python3
"""Bottom-screen frame counter (drawn / new pictures / decoded per second) + L2 cache test switch."""
import sys
def sub(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count: sys.exit(f"{path}: expected {count}x {old[:60]!r}, found {s.count(old)}")
    open(path, "w").write(s.replace(old, new))

# --- vp.h: rates in the status struct
sub("vp/vp.h", "    double net_kbps;\n} vp_state;",
    "    double net_kbps;\n    int fps_drawn, fps_new, fps_dec;   /* last second: frames drawn, of them a changed picture, decoded */\n} vp_state;")

# --- vp.c
V = "vp/vp.c"
sub(V, "    int frames_out, render_fail, nproc, npkt_v, shown, skipped;\n",
       "    int frames_out, render_fail, nproc, npkt_v, shown, skipped;\n"
       "    u32 last_hash; int fc_drawn, fc_new, fc_dec0, fps_drawn, fps_new, fps_dec; u64 fps_t;\n")
sub(V, "static void video_decode(vp_ctx *c, AVPacket *p)\n{",
       "/* hash of a whole frame as it will be drawn: equal hash = the same picture again */\n"
       "static u32 frame_hash(vp_ctx *c, const u16 *f)\n{\n"
       "    const u32 *w = (const u32 *)f;\n"
       "    u32 h = 2166136261u, n = (u32)(c->src_w * c->src_h) / 2;\n"
       "    for (u32 i = 0; i < n; i++) { h ^= w[i]; h *= 16777619u; }\n"
       "    return h;\n}\n\n"
       "static void video_decode(vp_ctx *c, AVPacket *p)\n{")
sub(V, "                blit(c, c->ring[show]);\n                c->shown++;\n",
       "                blit(c, c->ring[show]);\n                c->shown++;\n"
       "                {   /* frame counter for the bottom screen */\n"
       "                    u32 h = frame_hash(c, c->ring[show]);\n"
       "                    c->fc_drawn++;\n"
       "                    if (h != c->last_hash) c->fc_new++;\n"
       "                    c->last_hash = h;\n"
       "                }\n")
sub(V, "        if (g_ui && g_ui->status && now - last_status >= 250) {\n"
       "            vp_state s = { shown_pts, duration, pq_span(c, &c->vq, c->vtb), buffering, paused,\n"
       "                           c->vpar ? c->vpar->width : 0, c->vpar ? c->vpar->height : 0, vcodec, kbps };\n",
       "        if (now - c->fps_t >= 1000) {\n"
       "            double k = c->fps_t ? 1000.0 / (now - c->fps_t) : 0;\n"
       "            c->fps_drawn = (int)(c->fc_drawn * k + 0.5);\n"
       "            c->fps_new = (int)(c->fc_new * k + 0.5);\n"
       "            c->fps_dec = (int)((c->frames_out - c->fc_dec0) * k + 0.5);\n"
       "            c->fc_drawn = c->fc_new = 0; c->fc_dec0 = c->frames_out; c->fps_t = now;\n"
       "        }\n"
       "        if (g_ui && g_ui->status && now - last_status >= 250) {\n"
       "            vp_state s = { shown_pts, duration, pq_span(c, &c->vq, c->vtb), buffering, paused,\n"
       "                           c->vpar ? c->vpar->width : 0, c->vpar ? c->vpar->height : 0, vcodec, kbps };\n"
       "            s.fps_drawn = c->fps_drawn; s.fps_new = c->fps_new; s.fps_dec = c->fps_dec;\n")

# --- main3ds.c
M = "src/main3ds.c"
sub(M, '    printf(" %dp %.0fkb/s\\x1b[0m", st->height, st->net_kbps);\n',
       '    printf(" %dp %.0fkb/s\\x1b[0m", st->height, st->net_kbps);\n'
       '    printf("\\x1b[25;1H\\x1b[2K\\x1b[33mframes/s  drawn %2d  new %2d  decoded %2d%s\\x1b[0m",\n'
       '           st->fps_drawn, st->fps_new, st->fps_dec, g_l2 ? "  L2" : "");\n')
sub(M, "static void vp_status_cb(const char *title, const vp_state *st)\n{",
       "/* 804 MHz always; the New 3DS L2 cache only if sdmc:/3ds/ani3ds/l2 exists (test: the CPU read\n"
       " * old decoder pictures through it). Re-applied after HOME, where libctru resets it. */\n"
       "static int g_l2;\n"
       "static void cpu_config(void)\n{\n"
       "    FILE *f = fopen(DIR \"/l2\", \"r\");\n"
       "    g_l2 = f != NULL;\n"
       "    if (f) fclose(f);\n"
       "    Result r = -1;\n"
       "    if (R_SUCCEEDED(ptmSysmInit())) { r = PTMSYSM_ConfigureNew3DSCPU(g_l2 ? 3 : 1); ptmSysmExit(); }\n"
       "    if (g_logf) { fprintf(g_logf, \"cpu: 804MHz, L2 %s (r=%08lx)\\n\", g_l2 ? \"on\" : \"off\", r); fflush(g_logf); }\n"
       "}\n"
       "static void cpu_hook(APT_HookType t, void *p)\n{\n"
       "    (void)p;\n"
       "    if (t == APTHOOK_ONRESTORE || t == APTHOOK_ONWAKEUP) cpu_config();\n"
       "}\n\n"
       "static void vp_status_cb(const char *title, const vp_state *st)\n{")
sub(M, "    osSetSpeedupEnable(true);\n", "")
sub(M, '    g_logf = fopen(DIR "/log.txt", "w");\n',
       '    g_logf = fopen(DIR "/log.txt", "w");\n'
       '    static aptHookCookie cpu_cookie;\n'
       '    cpu_config();\n'
       '    aptHook(&cpu_cookie, cpu_hook, NULL);\n')
print("ok")
