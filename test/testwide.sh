#!/bin/sh
# widescreen 720p test: Lord of Mysteries (1696x720), first in history
B=/root/3ds-rc/bridge/3ds
L=/tmp/twide.log
: > $L
log() { echo "$(date +%T) $*" >> $L; }
rm -f /tmp/twide_*.png /tmp/applogwide.txt
timeout 150 $B install /root/3ds-rc/ani3ds/build/ani3ds.cia >> $L 2>&1; log installed
timeout 20 $B launch 000400000A3D5100 >> $L 2>&1
sleep 5
for i in $(seq 1 20); do sleep 3; timeout 4 $B ping 2>/dev/null | grep -q pong && break; done
log up; sleep 4
timeout 5 $B press A >> $L 2>&1; sleep 7          # open, cursor on ep 2
timeout 10 $B shot top -o /tmp/twide_list.png >> $L 2>&1
timeout 5 $B press A >> $L 2>&1; log "play pressed"
sleep 30; timeout 10 $B shot both -o /tmp/twide_a.png >> $L 2>&1 || log "shot failed (crash?)"; log "shot +30"
sleep 45; timeout 10 $B shot both -o /tmp/twide_b.png >> $L 2>&1 || log "shot failed (crash?)"; log "shot +75"
timeout 20 $B cat /3ds/ani3ds/log.txt > /tmp/applogwide.txt 2>&1 || log "log fetch failed"
log end
