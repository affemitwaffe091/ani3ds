import sys
p=sys.argv[1]; s=open(p).read()
d="static char g_lastlog[160];\nstatic volatile int g_lastlog_new;\n\n"
assert s.count(d)==1
s=s.replace(d,"")
a="static void vp_status_cb(const char *title, const vp_state *st)\n"
assert s.count(a)==1
s=s.replace(a,"/* newest player log line; written by any thread, shown by the main thread */\n"+d+a)
open(p,"w").write(s); print("ok")
