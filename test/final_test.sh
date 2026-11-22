#!/bin/sh
# full flow: install + play episode 1, screenshot during a subtitle cue, stop with B, screenshot the menu
B=/root/3ds-rc/bridge/3ds
T=/root/3ds-rc/ani3ds/test
rm -f /tmp/pt*.png /tmp/sub*.png /tmp/menu.png
sh $T/play_test.sh install "40"
python3 -I $T/subshot.py 1 > /tmp/subshot.log 2>&1
timeout 5 $B press B > /dev/null 2>&1; sleep 3
timeout 10 $B shot both -o /tmp/menu.png > /dev/null 2>&1
echo "final done $(date +%T)" >> /tmp/pt.log
