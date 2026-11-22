#!/usr/bin/env python3
"""Align the 3DS's displayed-frame fingerprints with a reference decode at a constant offset and
check whether every displayed frame is the right source frame (anime is often drawn 'on twos',
so identical neighbouring source frames are normal and not a decoder fault)."""
import os, re, subprocess, sys
log, url, cache = sys.argv[1], sys.argv[2], sys.argv[3]
sigs = [(int(a), float(b), bytes.fromhex(h)) for a, b, h in
        re.findall(r"^sig (\d+) ([-\d.]+) ([0-9a-f]{32})$", open(log).read(), re.M)]
W, H = 64, 32
if not os.path.exists(cache):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-user_agent", "Mozilla/5.0",
                          "-headers", "Referer: https://zokoanime.video/\r\n",
                          "-allowed_extensions", "ALL", "-extension_picky", "0", "-i", url, "-t", "11",
                          "-vsync", "passthrough",
                          "-vf", f"scale={W}:{H},format=rgb24", "-f", "rawvideo", "-"],
                         capture_output=True).stdout
    open(cache, "wb").write(raw)
raw = open(cache, "rb").read()
fs = W * H * 3
ref = []
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
D = lambda a, b: sum(abs(x - y) for x, y in zip(a, b))
held = sum(1 for k in range(1, len(ref)) if D(ref[k], ref[k - 1]) <= 4)
print(f"{len(sigs)} displayed, {len(ref)} reference frames; reference frames identical to the previous one: {held}/{len(ref)-1}")
vis = [(round(p / 0.04), s) for _, p, s in sigs if any(s)]
best = None
for off in range(-40, 41):
    ds = [D(s, ref[i + off]) for i, s in vis if 0 <= i + off < len(ref)]
    if len(ds) < len(vis) * 0.8: continue
    ds.sort(); med = ds[len(ds) // 2]
    if best is None or med < best[0]: best = (med, off)
med, off = best
print(f"best constant offset: displayed #i = reference #(i{off:+d})  median dist {med}")
ds = [(i, D(s, ref[i + off])) for i, s in vis if 0 <= i + off < len(ref)]
bad = [(i, d) for i, d in ds if d > med + 12]
print(f"frames compared {len(ds)}, clearly wrong {len(bad)}: {bad[:20]}")
# also: does the 3DS change picture on the same frames as the reference?
ch3 = [i for (i, s), (j, t) in zip(vis[1:], vis[:-1]) if D(s, t) > 4]
chr_ = [i for i, _ in vis[1:] if 0 <= i + off < len(ref) and D(ref[i + off], ref[i + off - 1]) > 4]
print(f"picture changes: 3DS {len(ch3)}, reference over the same span {len(chr_)}, same positions {len(set(ch3) & set(chr_))}")

# per reference picture change: did the 3DS show the new picture on time, one frame late/early, or never?
pos = {i: s for i, s in vis}
res = {"on time": 0, "1 late": 0, "1 early": 0, "2+ late": 0, "missing": 0}
miss = []
for i in sorted(pos):
    k = i + off
    if k < 1 or k + 3 >= len(ref) or i - 1 not in pos: continue
    jump = D(ref[k], ref[k - 1])
    if jump <= 8: continue
    def is_new(j):
        return j in pos and D(pos[j], ref[k]) + 6 < D(pos[j], ref[k - 1])
    if is_new(i) and not is_new(i - 1): res["on time"] += 1
    elif is_new(i - 1) and not is_new(i - 2): res["1 early"] += 1
    elif is_new(i + 1): res["1 late"] += 1
    elif is_new(i + 2) or is_new(i + 3): res["2+ late"] += 1
    else: res["missing"] += 1; miss.append((i, jump))
print("reference picture changes (>8):", res)
print("missing (displayed#, size):", miss[:30])

# picture ids: expected (reference at the aligned position) vs shown (best match within +-4 frames)
pid = [0] * len(ref)
for k in range(1, len(ref)): pid[k] = pid[k - 1] + (D(ref[k], ref[k - 1]) > 8)
a = int(sys.argv[4]) if len(sys.argv) > 4 else 100
exp, got = [], []
for i in range(a, a + 60):
    k = i + off
    if i not in pos or not (4 <= k < len(ref) - 4): continue
    c = min(range(k - 4, k + 5), key=lambda j: (D(pos[i], ref[j]), abs(j - k)))
    exp.append(pid[k]); got.append(pid[c])
print("expected:", " ".join(f"{x % 100:2d}" for x in exp))
print("shown:   ", " ".join(f"{x % 100:2d}" for x in got))
