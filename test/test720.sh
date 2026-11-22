#!/bin/sh
# 720p test: relaunch ani3ds, open the first history entry (cursor lands on the next episode), play it
B=/root/3ds-rc/bridge/3ds
L=/tmp/t720.log
: > $L
log() { echo "$(date +%T) $*" >> $L; }
rm -f /tmp/t720_*.png /tmp/applog720.txt
timeout 20 $B launch 000400000A3D5100 >> $L 2>&1
sleep 5
for i in $(seq 1 20); do sleep 3; timeout 4 $B ping 2>/dev/null | grep -q pong && break; done
log up; sleep 4
timeout 5 $B press A >> $L 2>&1; sleep 7          # open show from history
timeout 10 $B shot top -o /tmp/t720_list.png >> $L 2>&1
timeout 5 $B press A >> $L 2>&1; log "play pressed"
sleep 30; timeout 10 $B shot both -o /tmp/t720_a.png >> $L 2>&1; log "shot +30"
sleep 50; timeout 10 $B shot both -o /tmp/t720_b.png >> $L 2>&1; log "shot +80"
timeout 20 $B cat /3ds/ani3ds/log.txt > /tmp/applog720.txt 2>&1
log end
