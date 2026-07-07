#!/usr/bin/env bash
# Force-fire the player laser in atari800 and capture the P2 shot buffer across
# successive frames.  Pokes the $5178 shot-init vars into a flight savestate,
# then dumps mem[$0E00 page] + shadows once per frame while draw_player_shot
# ($8c58) animates the bolt ($0036 = 1,2,3.. until $1a).
set -uo pipefail
STATE="${1:-a800dumps/launch_6_gameplay.a8s}"
OUT="${DL_OUT:-/tmp/shot_capture.log}"
ATARI="${ATARI800:-atari800}"
FRAMES="${FRAMES:-14}"
FIFO="/tmp/shot_a8mon.$$"

command -v gstdbuf >/dev/null || { echo "need gstdbuf"; exit 1; }
pkill -9 atari800 2>/dev/null || true; sleep 1
rm -f "$FIFO" "$OUT"; mkfifo "$FIFO"
sleep 100000 > "$FIFO" & HOLDER=$!
trap 'kill "$HOLDER" 2>/dev/null; pkill -9 atari800 2>/dev/null; rm -f "$FIFO"' EXIT

gstdbuf -o0 -e0 "$ATARI" -nobasic -state "$STATE" < "$FIFO" > "$OUT" 2>&1 &
A8=$!
sleep 4
kill -INT "$A8" 2>/dev/null; sleep 2

dump() {   # tag ; dump shot region + shadows
  printf '!echo ==FRAME%d==\n' "$1" > "$FIFO"
  printf 'M 0030\nM 0E30\nM 0E40\nM 0E50\nM 0E60\nM 0E70\nM 0E80\nM 0E90\nM 0EA0\nM 0EB0\n' > "$FIFO"
  printf 'M 2838\nM 2860\n' > "$FIFO"
  sleep 0.5
}

# advance a few frames so we are solidly mid-flight
for i in 1 2 3 4; do printf 'BLINE 8\nCONT\n' > "$FIFO"; sleep 0.3; done

# poke the fire-shot init (matches $5178-$5194 + zero the erase-length $2866/$2865)
printf 'C 36 01\nC 37 78\nC 38 7c\nC 39 00\nC 286a 00\nC 286b 01\nC 286c 0c\nC 2838 00\nC 2865 00\nC 2866 00\n' > "$FIFO"
sleep 0.5

f=0
while [ "$f" -lt "$FRAMES" ]; do
  printf 'BLINE 8\nCONT\n' > "$FIFO"; sleep 0.35   # advance one frame
  dump "$f"
  f=$((f+1))
done
sleep 1
printf 'CONT\n' > "$FIFO"; sleep 0.5
echo "capture -> $OUT ($(grep -c '==FRAME' "$OUT") frames)"
