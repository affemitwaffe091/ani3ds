#!/usr/bin/env python3
# vp: MVD hands out frames in decode order (B-frames are not reordered), but the display queue
# was first-in-first-out, so every frame after a B-frame jump looked "late" and about half of all
# frames were dropped (13 of 25 fps shown). Make the queue a set of slots with timestamps: the
# decoder fills any free slot, the display shows the newest due frame and never goes back in time.
p = "vp/vp.c"
c = open(p).read()
def rep(a, b):
    global c
    assert c.count(a) == 1, a[:70]
    c = c.replace(a, b)

rep("    int r_head, r_count;", "    int r_head, r_count;\n    int r_used[VRING];              /* slot holds a decoded frame (any order) */\n    double last_shown_pts;")

# decoder: take any free slot
rep("        int slot = (c->r_head + c->r_count) % VRING;",
    "        int slot = 0;\n        while (slot < VRING - 1 && c->r_used[slot]) slot++;")
rep("""            c->ring_pts[slot] = t;
            c->r_count++;""", """            c->ring_pts[slot] = t;
            c->r_used[slot] = 1;
            c->r_count++;""")

# seek: empty all slots
rep("    c->r_count = 0;\n", "    c->r_count = 0;\n    memset(c->r_used, 0, sizeof c->r_used);\n    c->last_shown_pts = -1;\n")

# start of playback: earliest queued frame
rep("            wall_base = c->r_count ? c->ring_pts[c->r_head] : shown_pts;", """            wall_base = shown_pts;
            for (int i = 0; i < VRING; i++)
                if (c->r_used[i] && c->ring_pts[i] >= 0 && (wall_base == shown_pts || c->ring_pts[i] < wall_base)) wall_base = c->ring_pts[i];""")

# display: pick by timestamp
old_a = "            while (c->r_count > 1 && c->ring_pts[(c->r_head + 1) % VRING] <= clock + 0.02) {"
old_b = "                show = c->r_head;\n"
i = c.index(old_a); j = c.index(old_b, i) + len(old_b)
c = c[:i] + """            /* newest frame that is due; older due frames and anything behind the last shown frame are late */
            double best = -1e18;
            for (int i = 0; i < VRING; i++)
                if (c->r_used[i] && c->ring_pts[i] <= clock + 0.02 && c->ring_pts[i] > best) { best = c->ring_pts[i]; show = i; }
            for (int i = 0; i < VRING; i++) {
                if (!c->r_used[i] || i == show) continue;
                int behind = c->ring_pts[i] >= 0 && c->ring_pts[i] <= c->last_shown_pts;
                if ((show >= 0 && c->ring_pts[i] <= best) || behind) {
                    c->r_used[i] = 0;
                    c->r_count--;
                    c->skipped++;          /* late frame dropped */
                }
            }
""" + c[j:]
rep("""                LightLock_Lock(&c->lock);
                c->r_head = (c->r_head + 1) % VRING;
                c->r_count--;
                LightLock_Unlock(&c->lock);
                gfxFlushBuffers();""", """                LightLock_Lock(&c->lock);
                c->r_used[show] = 0;
                c->r_count--;
                if (c->ring_pts[show] >= 0) c->last_shown_pts = c->ring_pts[show];
                LightLock_Unlock(&c->lock);
                gfxFlushBuffers();""")
# a fresh context starts with "nothing shown yet"
rep("    c->vidx = c->aidx = -1;", "    c->vidx = c->aidx = -1;\n    c->last_shown_pts = -1;")
open(p, "w").write(c)
print("ok")
