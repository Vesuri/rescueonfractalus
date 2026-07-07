#!/usr/bin/env bash
# Interactive shot capture (free-run sampling): launch atari800 WITH a window on a flight
# savestate so the user can HOLD fire; the game runs in real-time and we periodically break,
# dump the full P2 page ($0E00) + shadows, and resume.  Each snapshot is keyed by the shot
# frame counter $0036 (1..$1a), so sorting the snapshots reconstructs the animation regardless
# of when each sample landed.  HOLD the FIRE button (Ctrl) the whole time this runs.
set -uo pipefail
STATE="${1:-a800dumps/launch_6_gameplay.a8s}"
OUT="${DL_OUT:-/tmp/shot_capture2.log}"
ATARI="${ATARI800:-atari800}"
SNAPS="${SNAPS:-60}"
GAP="${GAP:-0.2}"            # real-time run between snapshots (~12 frames)
FIFO="/tmp/shot2_a8mon.$$"

command -v gstdbuf >/dev/null || { echo "need gstdbuf"; exit 1; }
pkill -9 atari800 2>/dev/null || true; sleep 1
rm -f "$FIFO" "$OUT"; mkfifo "$FIFO"
sleep 100000 > "$FIFO" & HOLDER=$!
trap 'kill "$HOLDER" 2>/dev/null; pkill -9 atari800 2>/dev/null; rm -f "$FIFO"' EXIT

gstdbuf -o0 -e0 "$ATARI" -nobasic -state "$STATE" < "$FIFO" > "$OUT" 2>&1 &
A8=$!
echo ">>> atari800 window is up.  FOCUS IT and HOLD the FIRE button (Ctrl) now."
echo ">>> sampling $SNAPS times over ~$(python3 -c "print($SNAPS*($GAP+0.35))")s.  Keep holding..."
sleep 5

f=0
while [ "$f" -lt "$SNAPS" ]; do
  kill -INT "$A8" 2>/dev/null; sleep 0.15          # break to monitor
  printf '!echo ==SNAP%d==\nM 0030\n' "$f" > "$FIFO"
  # full P2 page $0E00-$0EFF (shot upper + AH ground lower)
  for a in 00 10 20 30 40 50 60 70 80 90 A0 B0 C0 D0 E0 F0; do printf 'M 0E%s\n' "$a" > "$FIFO"; done
  sleep 0.25
  printf 'CONT\n' > "$FIFO"                          # resume real-time
  sleep "$GAP"
  f=$((f+1))
done
sleep 0.5
echo "capture -> $OUT ($(grep -c '==SNAP' "$OUT") snapshots)"
