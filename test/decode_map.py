#!/usr/bin/env python3
"""For the k-th picture the 3DS rendered: which source frame (display index) was it, compared with
the k-th packet in decode order? Reveals the rule the MVD uses to pick the output picture."""
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
args = ["ffprobe", "-v", "error", "-user_agent", "Mozilla/5.0", "-headers", "Referer: https://zokoanime.video/\r\n",
        "-allowed_extensions", "ALL", "-extension_picky", "0", "-read_intervals", "%+11", "-select_streams", "v:0", "-of", "csv=p=0"]
fr = subprocess.run(args + ["-show_entries", "frame=pict_type", url], capture_output=True, text=True).stdout.split()
pk = subprocess.run(args + ["-show_entries", "packet=pts", url], capture_output=True, text=True).stdout.split()
pts = [int(x.strip(",")) for x in pk if x.strip(",").lstrip("-").isdigit()]
order = sorted(pts)
disp = [order.index(p) for p in pts]          # display index of each packet, in decode order
typ = [t.strip(",") for t in fr]
D = lambda a, b: sum(abs(x - y) for x, y in zip(a, b))
sigs = [bytes.fromhex(h) for h in re.findall(r"^sig \d+ [-\d.]+ ([0-9a-f]{32})$", open(log).read(), re.M)]
print("decode order (display idx/type):", " ".join(f"{d}{typ[d] if d < len(typ) else '?'}" for d in disp[:40]))
rows = []
for k, s in enumerate(sigs[:90]):
    e = k + 2 + off                              # sig k has pts 0.08 + 0.04k
    if not any(s) or e < 6 or e + 6 >= len(ref): continue
    j = min(range(e - 6, e + 7), key=lambda j: (D(s, ref[j]), abs(j - e)))
    rows.append((k, j, D(s, ref[j])))
print("k: shown(display idx) / decode-order packet k+1 display idx / max decoded so far")
line = []
for k, j, d in rows:
    n = k + 1                                     # +1: the first output (mvd_first) is discarded
    dk = disp[n] if n < len(disp) else -1
    mx = max(disp[:n + 1]) if n < len(disp) else -1
    line.append(f"{k}:{j}{typ[j]}/{dk}/{mx}")
for i in range(0, len(line), 8): print("  ".join(line[i:i + 8]))
