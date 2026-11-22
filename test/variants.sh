#!/bin/sh
# which quality variants does each episode's ZokoAnime master playlist offer?
cd /root/3ds-rc/ani3ds
probe() {  # query result# episode#
  out=$(timeout 40 ./t "$1" "$2" "$3" sub 2>&1)
  v=$(printf '%s\n' "$out" | sed -n '/^master/,/^subs/p' | grep -o 'RESOLUTION=[0-9x]*' | sed 's/RESOLUTION=//' | tr '\n' ' ')
  printf '%-16s ep%-3s %s\n' "$1" "$(($3 + 1))" "${v:-$(printf '%s\n' "$out" | grep -m1 '^resolve')}"
}
for e in 0 1 2 3 4 5 9; do probe frieren 0 $e; done
for e in 0 1 5; do probe "jujutsu kaisen" 0 $e; done
for e in 0 1099 1150; do probe "one piece" 0 $e; done
for e in 0 3; do probe "dandadan" 0 $e; done
