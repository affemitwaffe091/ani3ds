#!/usr/bin/env python3
"""Screenshot the 3DS in the middle of upcoming subtitle cues (to verify the overlay).
Reads the playback position from the app's debug log, the cues from the episode's VTT."""
import re, subprocess, sys, time
B = "/root/3ds-rc/bridge/3ds"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2

def sh(args, t=30):
    return subprocess.run(args, capture_output=True, text=True, timeout=t).stdout

log = sh([B, "cat", "/3ds/ani3ds/log.txt"])
vtt_url = re.findall(r"^subs (\S+)", log, re.M)[-1]
vtt = sh(["curl", "-sL", "-A", "Mozilla/5.0", "-e", "https://zokoanime.video/", vtt_url])
def ts(s):
    p = s.replace(",", ".").split(":")
    return sum(float(x) * 60 ** i for i, x in enumerate(reversed(p)))
cues = [(ts(a), ts(b), t.strip()) for a, b, t in
        re.findall(r"([\d:.]+) --> ([\d:.]+)[^\n]*\n(.*?)(?:\n\n|\Z)", vtt, re.S)]
taken = 0
while taken < N:
    t_req = time.time()
    log = sh([B, "cat", "/3ds/ani3ds/log.txt"])
    pos = float(re.findall(r"^dbg: .*pos=([\d.]+)", log, re.M)[-1])
    est = pos + (time.time() - t_req) / 2 + 1.0          # dbg lines are written every 2 s
    nxt = [c for c in cues if c[0] > est + 6 and c[1] - c[0] >= 2.5]
    if not nxt:
        print("no more cues"); break
    a, b, txt = nxt[0]
    target = a + min(1.5, (b - a) / 2)
    time.sleep(max(0, target - est - (time.time() - t_req) / 2))
    out = f"/tmp/sub{taken}.png"
    sh([B, "shot", "top", "-o", out])
    print(f"{out}: est pos {est:.1f}, cue {a:.2f}-{b:.2f} {txt!r}", flush=True)
    taken += 1
