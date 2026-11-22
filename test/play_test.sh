#!/bin/sh
# on-console test: [install], launch ani3ds, open the first history entry, play episode 1,
# screenshot over time, then fetch log.txt.   usage: play_test.sh [install] [shots]
B=/root/3ds-rc/bridge/3ds
L=/tmp/pt.log
SHOTS=${2:-"28 98"}
: > $L
log() { echo "$(date +%T) $*" >> $L; }
if [ "$1" = install ]; then
  timeout 150 $B install /root/3ds-rc/ani3ds/build/ani3ds.cia >> $L 2>&1
  timeout 20 $B put /tmp/debugflag /3ds/ani3ds/debug >> $L 2>&1
  log installed
fi
timeout 20 $B launch 000400000A3D5100 >> $L 2>&1
sleep 5
for i in $(seq 1 20); do sleep 3; timeout 4 $B ping 2>/dev/null | grep -q pong && break; done
log "up"; sleep 4
timeout 5 $B press A >> $L 2>&1; sleep 6        # open show from history
timeout 5 $B press UP >> $L 2>&1; sleep 1       # cursor starts at ep 2 -> ep 1
timeout 5 $B press A >> $L 2>&1; log "play pressed"
T=0; for d in $SHOTS; do sleep $((d - T)); T=$d; n=$d; timeout 10 $B shot both -o /tmp/pt$n.png >> $L 2>&1 || log "shot $n failed"; log "shot at +${d}s"; done
timeout 15 $B cat /3ds/ani3ds/log.txt > /tmp/applog.txt 2>&1
log "end"
