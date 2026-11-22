#!/usr/bin/env python3
"""Why do only some episodes have 360p? For a sample of episodes, record: offered variants,
Last-Modified of each variant's first segment, the encoder signature inside the first segment,
codec parameters, and the embed's video_id.  Gentle: sequential requests with small pauses."""
import base64, json, re, subprocess, sys, time, urllib.parse, urllib.request

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
BASE = "https://hianime.at"
REF = "https://zokoanime.video/"

def get(url, ref=None, rng=None, head=False):
    """HTTP via curl (python's resolver hangs in this environment). Returns (headers dict, body)."""
    time.sleep(0.3)
    cmd = ["curl", "-sL", "--max-time", "25", "-A", UA, "-D", "/tmp/sv_hdr.txt", "-o", "/tmp/sv_body.bin"]
    if ref: cmd += ["-e", ref]
    if rng: cmd += ["-r", rng.split("=", 1)[1]]
    if head: cmd += ["-I"]
    subprocess.run(cmd + [url], timeout=40)
    hd = {}
    for line in open("/tmp/sv_hdr.txt", encoding="latin1").read().splitlines():
        if ":" in line:
            k, v = line.split(":", 1); hd[k.strip().title()] = v.strip()
    body = b"" if head else open("/tmp/sv_body.bin", "rb").read()
    return hd, body

def search(q):
    _, b = get(BASE + "/search?keyword=" + urllib.parse.quote_plus(q))
    page = b.decode("utf-8", "replace").split('id="main-sidebar"')[0]
    out = []
    for m in re.finditer(r'<h3 class="film-name">\s*<a href="[^"]*/([^"/?]+)"\s*title="([^"]*)"', page):
        out.append((m.group(1), m.group(2).replace("&#039;", "'")))
    return out

def episodes(slug):
    _, b = get(f"{BASE}/api/theme/episode/list/{slug.rsplit('-', 1)[1]}")
    t = b.decode().replace("\\", "")
    eps = []
    for chunk in t.split("ep-item")[1:]:
        n = re.search(r'data-number="([^"]*)"', chunk); i = re.search(r'data-id="(\d+)"', chunk)
        if n and i and f"/watch/{slug}?ep=" in chunk: eps.append((n.group(1), i.group(1)))
    return eps

def master_for(ep_id, mode):
    _, b = get(f"{BASE}/api/theme/episode/servers?episodeId={ep_id}")
    t = b.decode().replace("\\", "")
    h = None
    for chunk in t.split("server-item")[1:]:
        if f'data-type="{mode}"' in chunk and 'data-server-name="ZokoAnime"' in chunk:
            h = re.search(r'data-hash="([^"]*)"', chunk).group(1); break
    if not h: return None, None
    embed = base64.b64decode(h + "==").decode()
    _, b = get(embed)
    blob = re.search(r'window\.__P="([^"]*)"', b.decode()).group(1)
    d = base64.b64decode(blob + "==")
    k = b"otaku-embed-v1"
    js = json.loads(bytes(c ^ k[i % len(k)] for i, c in enumerate(d)))
    return js, embed

def probe_segment(url):
    hd, _ = get(url, REF, head=True)
    lm = hd.get("Last-Modified", "?")
    _, data = get(url, REF, rng="bytes=0-393215")
    m = re.search(rb"(x264 - core \d+[^\x00]{0,40})|(x265[^\x00]{0,40})|(Lavc[\d.]+ [a-z0-9_]+)|(NVENC|Rec2020|Apple)", data)
    sig = m.group(0).decode("latin1")[:60] if m else "none"
    ref_m = re.search(rb"ref=(\d+)", data); crf = re.search(rb"crf=([\d.]+)", data)
    extra = (f" ref={ref_m.group(1).decode()}" if ref_m else "") + (f" crf={crf.group(1).decode()}" if crf else "")
    with open("/tmp/seg_probe.ts", "wb") as f: f.write(data)
    pr = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                         "stream=profile,level,width,height,r_frame_rate", "-of", "csv=p=0", "/tmp/seg_probe.ts"],
                        capture_output=True, text=True).stdout.strip().replace("\n", " ")
    return lm, sig + extra, pr

def sample(query, ep_numbers, mode="sub", pick=0):
    res = search(query)
    if not res: print(f"{query}: no results"); return
    slug, title = res[pick]
    eps = episodes(slug)
    for want in ep_numbers:
        if want > len(eps): print(f"{title[:22]:22} only {len(eps)} eps"); continue
        e = eps[want - 1] if want > 0 else eps[want]
        try:
            js, embed = master_for(e[1], mode)
            if not js: print(f"{title[:22]:22} ep{e[0]:>4} {mode}: no ZokoAnime"); continue
            src = js["src"]
            _, mb = get(src, REF)
            variants = re.findall(r"RESOLUTION=\d+x(\d+)[^\n]*\n([^\n#]+)", mb.decode())
            print(f"{title[:22]:22} ep{e[0]:>4} {mode} vid={js.get('video_id')} variants={[v[0] for v in variants]}", flush=True)
            for h, rel in variants:
                vurl = urllib.parse.urljoin(src, rel.strip())
                _, pl = get(vurl, REF)
                seg = next(l for l in pl.decode().splitlines() if l and not l.startswith("#"))
                seg = urllib.parse.urljoin(vurl, seg)
                lm, sig, pr = probe_segment(seg)
                print(f"      {h:>5}p  modified {lm}  enc: {sig}  [{pr}]", flush=True)
        except Exception as ex:
            print(f"{title[:22]:22} ep{e[0]:>4} {mode}: error {ex}", flush=True)

if __name__ == "__main__":
    import sys
    for arg in sys.argv[1:]:
        q, eps_, *m = arg.split(":")
        sample(q, [int(x) for x in eps_.split(",")], mode=(m[0] if m else "sub"))
    if len(sys.argv) > 1: sys.exit(0)
    sample("frieren", [1, 2, 3, 10, 28])
    sample("frieren", [1, 2], mode="dub")
    sample("jujutsu kaisen", [1, 2, 6, 24])
    sample("dandadan", [1, 2, 4, 12])
    sample("one piece", [1, 2, 1100, -1])
    sample("lord of mysteries", [1, 2, 5])
    sample("solo leveling", [1, 2, 12])
    sample("spy x family", [1, 2, 25])
