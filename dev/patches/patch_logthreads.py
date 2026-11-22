#!/usr/bin/env python3
# ani3ds: route player log lines to the file only; the bottom screen shows the
# newest one from the main thread (status callback). Other threads must not
# touch the console: it would draw into whichever screen is selected.
import sys
p = sys.argv[1]
s = open(p).read()
old = '''static void vp_log_cb(const char *l)
{
    if (g_logf) { fprintf(g_logf, "%s\\n", l); fflush(g_logf); }
    consoleSelect(&bot);
    printf("\\x1b[5;1H\\x1b[0J%.118s", l);   /* keep only the newest line, above the status */
    consoleSelect(&top);
}'''
new = '''static char g_lastlog[160];
static volatile int g_lastlog_new;

/* may run on the player's network/decoder threads: file + buffer only, no console */
static void vp_log_cb(const char *l)
{
    if (g_logf) { fprintf(g_logf, "%s\\n", l); fflush(g_logf); }
    snprintf(g_lastlog, sizeof g_lastlog, "%.118s", l);
    g_lastlog_new = 1;
}'''
assert s.count(old) == 1, "vp_log_cb"
s = s.replace(old, new)
old2 = '''    consoleSelect(&bot);
    int p = (int)st->pos, d = (int)st->dur;
    g_dur = st->dur;'''
new2 = '''    consoleSelect(&bot);
    if (g_lastlog_new) {   /* newest player log line, above the status lines */
        g_lastlog_new = 0;
        printf("\\x1b[5;1H\\x1b[0J%s", g_lastlog);
    }
    int p = (int)st->pos, d = (int)st->dur;
    g_dur = st->dur;'''
assert s.count(old2) == 1, "status"
s = s.replace(old2, new2)
open(p, "w").write(s)
print("ok")
