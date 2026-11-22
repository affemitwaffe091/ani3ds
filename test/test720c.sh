#!/bin/sh
# 720p re-test after the cache fix: Frieren is now first in history (last ep 1), max quality already 720p
B=/root/3ds-rc/bridge/3ds
L=/tmp/t720.log
: > $L
log() { echo "$(date +%T) $*" >> $L; }
rm -f /tmp/t720_*.png /tmp/applog720.txt
timeout 150 $B install /root/3ds-rc/ani3ds/build/ani3ds.cia >> $L 2>&1; log installed
timeout 20 $B launch 000400000A3D5100 >> $L 2>&1
sleep 5
for i in $(seq 1 20); do sleep 3; timeout 4 $B ping 2>/dev/null | grep -q pong && break; done
log up; sleep 4
timeout 10 $B shot bot -o /tmp/t720_home.png >> $L 2>&1
timeout 5 $B press A >> $L 2>&1; sleep 7          # open Frieren (cursor on ep 2)
timeout 5 $B press LEFT >> $L 2>&1; sleep 1       # page up -> episode 1
timeout 10 $B shot top -o /tmp/t720_list.png >> $L 2>&1
timeout 5 $B press A >> $L 2>&1; log "play pressed"
sleep 75; timeout 10 $B shot top -o /tmp/t720_a.png >> $L 2>&1; log "shot +75"
sleep 3;  timeout 10 $B shot top -o /tmp/t720_b.png >> $L 2>&1; log "shot +78"
sleep 3;  timeout 10 $B shot top -o /tmp/t720_c.png >> $L 2>&1; log "shot +81"
timeout 20 $B cat /3ds/ani3ds/log.txt > /tmp/applog720.txt 2>&1
log end
