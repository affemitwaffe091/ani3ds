#!/usr/bin/env python3
"""Match the 3DS's displayed-frame fingerprints (log 'sig i pts hex') against a reference decode
(display order) of the same stream: prints, per displayed frame, which source frame it was."""
import re, subprocess, sys
log, url = sys.argv[1], sys.argv[2]
sigs = [(int(a), float(b), bytes.fromhex(h)) for a, b, h in
        re.findall(r"^sig (\d+) ([-\d.]+) ([0-9a-f]{32})$", open(log).read(), re.M)]
W, H = 64, 32
raw = subprocess.run(["ffmpeg", "-v", "error", "-user_agent", "Mozilla/5.0",
                      "-headers", "Referer: https://zokoanime.video/\r\n",
                      "-allowed_extensions", "ALL", "-extension_picky", "0", "-i", url, "-t", "11",
                      "-vf", f"scale={W}:{H},format=rgb24", "-f", "rawvideo", "-"],
                     capture_output=True).stdout
ref = []
fs = W * H * 3
for f in range(len(raw) // fs):
    fr = raw[f * fs:(f + 1) * fs]
    s = []
    for gy in range(4):
        for gx in range(4):
            tot = n = 0
            for y in range(gy * H // 4, (gy + 1) * H // 4):
                for x in range(gx * W // 4, (gx + 1) * W // 4):
                    o = (y * W + x) * 3
                    tot += (fr[o] * 77 + fr[o + 1] * 150 + fr[o + 2] * 29) >> 8; n += 1
            s.append(tot // n)
    ref.append(bytes(s))
print(f"{len(sigs)} displayed, {len(ref)} reference frames")
prev = -1; back = 0
rows = []
for i, pts, s in sigs:
    if not any(s): rows.append(f"{i:3d} {pts:6.3f}  (black)"); continue
    best = min(range(len(ref)), key=lambda k: sum(abs(a - b) for a, b in zip(s, ref[k])))
    d = sum(abs(a - b) for a, b in zip(s, ref[best]))
    flag = ""
    if prev >= 0 and best < prev: flag = "  <-- BACK"; back += 1
    rows.append(f"{i:3d} {pts:6.3f}  ref#{best:3d} ({best*0.04:6.3f}s) dist {d:3d}{flag}")
    prev = best
print("\n".join(rows))
print("backward steps:", back)
