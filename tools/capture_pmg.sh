#!/usr/bin/env bash
# Comprehensive PMG capture (free-run sampling): windowed atari800 on a flight savestate;
# the user HOLDS fire and shoots terrain / enemies while we periodically break and dump ALL
# FIVE PMG pages ($0B00 missiles + $0C00-$0FFF players P0-P3) plus zero-page + $28xx shadows.
# Reconstruct what every player/missile was doing in the viewport (shot, enemy, explosion, HUD).
set -uo pipefail
STATE="${1:-a800dumps/launch_6_gameplay.a8s}"
OUT="${DL_OUT:-/tmp/pmg_capture.log}"
ATARI="${ATARI800:-atari800}"
SNAPS="${SNAPS:-40}"
GAP="${GAP:-0.25}"
FIFO="/tmp/pmg_a8mon.$$"

command -v gstdbuf >/dev/null || { echo "need gstdbuf"; exit 1; }
pkill -9 atari800 2>/dev/null || true; sleep 1
rm -f "$FIFO" "$OUT"; mkfifo "$FIFO"
sleep 100000 > "$FIFO" & HOLDER=$!
trap 'kill "$HOLDER" 2>/dev/null; pkill -9 atari800 2>/dev/null; rm -f "$FIFO"' EXIT

gstdbuf -o0 -e0 "$ATARI" -nobasic -state "$STATE" < "$FIFO" > "$OUT" 2>&1 &
A8=$!
echo ">>> atari800 window up. FOCUS IT, HOLD FIRE (Ctrl), and shoot terrain + enemies."
echo ">>> sampling $SNAPS times over ~$(python3 -c "print(int($SNAPS*($GAP+0.55)))")s. Keep firing at things..."
sleep 5

f=0
while [ "$f" -lt "$SNAPS" ]; do
  kill -INT "$A8" 2>/dev/null; sleep 0.15
  printf '!echo ==SNAP%d==\n' "$f" > "$FIFO"
  # zero page (HPOS/col/size shadows live here), $28xx object table, all 5 PMG pages
  for a in 00 20 40 60 80 A0 C0 E0; do printf 'M 00%s\n' "$a" > "$FIFO"; done
  for a in 30 40 50 60 70; do printf 'M 28%s\n' "$a" > "$FIFO"; done
  for pg in 0B 0C 0D 0E 0F; do for a in 00 10 20 30 40 50 60 70 80 90 A0 B0 C0 D0 E0 F0; do printf 'M %s%s\n' "$pg" "$a" > "$FIFO"; done; done
  sleep 0.45
  printf 'CONT\n' > "$FIFO"
  sleep "$GAP"
  f=$((f+1))
done
sleep 0.5
echo "capture -> $OUT ($(grep -c '==SNAP' "$OUT") snapshots)"
