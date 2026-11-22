#!/usr/bin/env python3
"""Which frame types (I/P/B) of the source does the 3DS actually show? Uses align_sig's cached
reference decode plus ffprobe's per-frame pict_type (display order)."""
import re, subprocess, sys
log, url, cache, off = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
W, H = 64, 32
raw = open(cache, "rb").read(); fs = W * H * 3
def sig(fr):
    s = []
    for gy in range(4):
        for gx in range(4):
            tot = n = 0
            for y in range(gy * H // 4, (gy + 1) * H // 4):
                for x in range(gx * W // 4, (gx + 1) * W // 4):
                    o = (y * W + x) * 3; tot += (fr[o] * 77 + fr[o + 1] * 150 + fr[o + 2] * 29) >> 8; n += 1
            s.append(tot // n)
    return bytes(s)
ref = [sig(raw[f * fs:(f + 1) * fs]) for f in range(len(raw) // fs)]
out = subprocess.run(["ffprobe", "-v", "error", "-user_agent", "Mozilla/5.0",
                      "-headers", "Referer: https://zokoanime.video/\r\n", "-allowed_extensions", "ALL",
                      "-extension_picky", "0", "-read_intervals", "%+11", "-select_streams", "v:0",
                      "-show_entries", "frame=pict_type,pts_time", "-of", "csv=p=0", url],
                     capture_output=True, text=True).stdout
typ = [l.split(",")[1].strip() if "," in l else "?" for l in out.split("\n") if l.strip()]
print("source frame types (first 60):", "".join(typ[:60]))
D = lambda a, b: sum(abs(x - y) for x, y in zip(a, b))
sigs = [(round(float(p) / 0.04), bytes.fromhex(h)) for a, p, h in
        re.findall(r"^sig (\d+) ([-\d.]+) ([0-9a-f]{32})$", open(log).read(), re.M)]
shown, cnt = [], {}
for i, s in sigs:
    k = i + off
    if not any(s) or not (6 <= k < len(ref) - 6): continue
    j = min(range(k - 6, k + 7), key=lambda j: (D(s, ref[j]), abs(j - k)))
    shown.append(j)
uniq = sorted(set(shown))
for j in uniq: cnt[typ[j] if j < len(typ) else "?"] = cnt.get(typ[j] if j < len(typ) else "?", 0) + 1
allc = {}
for j in range(min(uniq), max(uniq) + 1): allc[typ[j]] = allc.get(typ[j], 0) + 1
print("source frames in the span by type:", allc)
print("distinct source frames the 3DS showed, by type:", cnt)
print("shown types in order:", "".join(typ[j] for j in shown[:80]))
