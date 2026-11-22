#!/usr/bin/env python3
# vp: MVD outputs frames in display order, but a frame's timestamp was taken (smallest pending
# packet pts) the moment it was decoded - when only the newest packet's pts was pending, so labels
# came out in decode order and the display dropped every second frame as "late". Like FourthTube:
# queue frames unlabeled (FIFO = display order) and label them when the display looks at them.
p = "vp/vp.c"
c = open(p).read()
def rep(a, b):
    global c
    assert c.count(a) == 1, a[:70]
    c = c.replace(a, b)

rep("""        pts_insert(c, ts == AV_NOPTS_VALUE ? -1 : ts * c->vtb - c->start_time);""",
    """        LightLock_Lock(&c->lock);
        pts_insert(c, ts == AV_NOPTS_VALUE ? -1 : ts * c->vtb - c->start_time);
        LightLock_Unlock(&c->lock);""")

a = "        double t = pts_take_min(c);\n        if (!rr && !c->mvd_first) {"
b = "            if (c->frames_out++ == 0) vp_log(\"first frame decoded\");\n        }\n"
i = c.index(a); j = c.index(b, i) + len(b)
c = c[:i] + """        /* no timestamp yet: frames leave MVD in display order, but the packets of B-frames shown
         * before a P-frame arrive after it, so the label is taken when the display picks the frame up
         * (label_queue). Like FourthTube, the discarded first output does not consume a timestamp. */
        if (!rr && !c->mvd_first) {
            LightLock_Lock(&c->lock);
            c->ring_pts[slot] = PTS_UNSET;
            c->r_count++;
            LightLock_Unlock(&c->lock);
            if (c->frames_out++ == 0) vp_log("first frame decoded");
        }
""" + c[j:]

rep("static double pts_take_min(vp_ctx *c)\n", """#define PTS_UNSET (-1000.0)
static double pts_take_min(vp_ctx *c);
/* lock held: give the first n queued frames (display order) their timestamps */
static void label_queue(vp_ctx *c, int n)
{
    for (int k = 0; k < n && k < c->r_count; k++) {
        int i = (c->r_head + k) % VRING;
        if (c->ring_pts[i] != PTS_UNSET) continue;
        double t = c->npts ? pts_take_min(c) : -1;
        c->ring_pts[i] = t;
        if (vp_debug && t >= 0) {   /* timestamp spacing check */
            double dp = t - c->last_cpts;
            if (c->last_cpts > 0) { if (dp < c->dp_min) c->dp_min = dp; if (dp > c->dp_max) c->dp_max = dp; }
            c->last_cpts = t;
            if (c->nptslog < 16) {
                size_t L = strlen(c->ptslog);
                snprintf(c->ptslog + L, sizeof c->ptslog - L, "%.3f ", t);
                if (++c->nptslog == 16) vp_log("first frame pts: %s", c->ptslog);
            }
        }
    }
}
static double pts_take_min(vp_ctx *c)
""")

rep("""        int vready = c->vidx < 0 || c->r_count >= 2 || c->decoder_done;""",
    """        LightLock_Lock(&c->lock);
        label_queue(c, 2);
        LightLock_Unlock(&c->lock);
        int vready = c->vidx < 0 || c->r_count >= 2 || c->decoder_done;""")

rep("""            while (c->r_count > 1 && c->ring_pts[(c->r_head + 1) % VRING] <= clock + 0.02) {
                c->r_head = (c->r_head + 1) % VRING;
                c->r_count--;""", """            label_queue(c, 2);
            while (c->r_count > 1 && c->ring_pts[(c->r_head + 1) % VRING] <= clock + 0.02) {
                c->r_head = (c->r_head + 1) % VRING;
                c->r_count--;
                label_queue(c, 2);""")
open(p, "w").write(c)
print("ok")
