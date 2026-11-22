import sys
p=sys.argv[1]; s=open(p).read()
a='''    printf("%s", lines);
    printf("\\x1b[2m---------------------------------------\\x1b[0m\\n");'''
assert s.count(a)==1, "help"
s=s.replace(a,'''    if (*lines) {   /* no second rule when there is no help text (playback screen) */
        printf("%s", lines);
        printf("\\x1b[2m---------------------------------------\\x1b[0m\\n");
    }''')
assert s.count('#define VERSION "0.1.1"')==1
s=s.replace('#define VERSION "0.1.1"','#define VERSION "0.1.2"')
open(p,"w").write(s); print("ok")
