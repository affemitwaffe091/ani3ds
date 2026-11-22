#!/usr/bin/env python3
# ani3ds 0.1.3: max-quality setting (480p default, 720p optional; L on the home screen), like ani-cli -q
import sys
p = sys.argv[1]
s = open(p).read()
def rep(a, b):
    global s
    assert s.count(a) == 1, a[:60]
    s = s.replace(a, b)
rep('#define VERSION "0.1.2"', '#define VERSION "0.1.3"')
rep('static int g_subs_on = 1;', 'static int g_subs_on = 1;\nstatic int g_maxq = 480;        /* largest stream height to pick when smaller ones exist */')
rep('    printf("\\x1b[36;1mani3ds " VERSION "\\x1b[0m  mode: \\x1b[33m%s\\x1b[0m  subs: \\x1b[33m%s\\x1b[0m\\n", g_mode, g_subs_on ? "on" : "off");',
    '    printf("\\x1b[36;1mani3ds " VERSION "\\x1b[0m  \\x1b[33m%s\\x1b[0m  subs \\x1b[33m%s\\x1b[0m  max \\x1b[33m%dp\\x1b[0m\\n", g_mode, g_subs_on ? "on" : "off", g_maxq);')
rep('    fprintf(f, "mode=%s\\nsubs=%d\\n", g_mode, g_subs_on);', '    fprintf(f, "mode=%s\\nsubs=%d\\nmaxq=%d\\n", g_mode, g_subs_on, g_maxq);')
rep('        if (!strncmp(l, "subs=", 5)) g_subs_on = atoi(l + 5) != 0;',
    '        if (!strncmp(l, "subs=", 5)) g_subs_on = atoi(l + 5) != 0;\n        if (!strncmp(l, "maxq=", 5)) g_maxq = atoi(l + 5) >= 720 ? 720 : 480;')
rep('    vp_set_ui(&g_ui);', '    vp_set_ui(&g_ui);\n    vp_max_h = g_maxq;')
rep('"A continue   X search\\nY remove from history\\nSELECT sub/dub   R subtitles on/off\\nSTART exit\\n"',
    '"A continue   X search\\nY remove from history\\nSELECT sub/dub   R subtitles on/off\\nL max quality 480p/720p   START exit\\n"')
rep(': "X search\\nSELECT sub/dub   R subtitles on/off\\nSTART exit\\n");',
    ': "X search\\nSELECT sub/dub   R subtitles on/off\\nL max quality 480p/720p   START exit\\n");')
rep('        if (raw & KEY_R) { g_subs_on = !g_subs_on; save_config(); continue; }',
    '        if (raw & KEY_R) { g_subs_on = !g_subs_on; save_config(); continue; }\n        if (raw & KEY_L) { g_maxq = g_maxq == 480 ? 720 : 480; save_config(); continue; }')
open(p, "w").write(s)
print("ok")
