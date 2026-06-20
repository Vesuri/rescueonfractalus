#!/usr/bin/env bash
# atari-dl-analyzer — headless capture step.
#
# Drives atari800 (FIFO monitor, fully headless) on a savestate to capture the raw
# data the analyzer needs:
#   1. DLIST            — the active display list (modes / LMS / DLI flags / JVB)
#   2. a per-scanline GTIA sweep (BLINE <y>; CONT; GTIA) over [ymin,ymax] — the LIVE
#      colour / player / missile / priority state the DLIs produce at each scanline.
# The raw log is then fed to dl_report.py.
#
# Why this works (kept here so it is not re-derived every session):
#   * `kill -INT atari800` drops the emulator into its built-in monitor, which reads
#     commands from stdin.  We feed stdin from a FIFO held open by a background sleep.
#   * `gstdbuf -o0 -e0` makes atari800 stdout UNBUFFERED so small SHOW/GTIA/DLIST
#     dumps actually flush to the log (otherwise they sit in a pipe buffer).
#   * `BLINE <y>` breaks when the beam reaches scanline y; `CONT` runs to it; `GTIA`
#     prints the full register file LIVE at that scanline (so DLI-set, mid-frame
#     values are captured, not just the top-of-frame shadows).
#   * `!echo ==L<y>==` tags each GTIA block so the parser can key it to a scanline.
#
# Usage:  dl_capture.sh <state.a8s> [ymin] [ymax] [step]
#   defaults: ymin=8 ymax=240 step=1   (narrow the range to go faster: ~0.4s/scanline)
#   env: ATARI800=<path>  DL_OUT=<logpath>  (default /tmp/dl_capture.log)
set -uo pipefail
STATE="${1:?usage: dl_capture.sh <state.a8s> [ymin ymax step]}"
YMIN="${2:-8}"; YMAX="${3:-240}"; STEP="${4:-1}"
OUT="${DL_OUT:-/tmp/dl_capture.log}"
ATARI="${ATARI800:-atari800}"
FIFO="/tmp/dl_a8mon.$$"

command -v gstdbuf >/dev/null || { echo "need gstdbuf (brew install coreutils)"; exit 1; }
pkill -9 atari800 2>/dev/null || true; sleep 1
rm -f "$FIFO" "$OUT"; mkfifo "$FIFO"
sleep 100000 > "$FIFO" &          # holder: keeps the FIFO open for the whole session
HOLDER=$!
trap 'kill "$HOLDER" 2>/dev/null; pkill -9 atari800 2>/dev/null; rm -f "$FIFO"' EXIT

gstdbuf -o0 -e0 "$ATARI" -nobasic -state "$STATE" < "$FIFO" > "$OUT" 2>&1 &
A8=$!
sleep 4
kill -0 "$A8" 2>/dev/null || { echo "atari800 failed to start; see $OUT"; tail -5 "$OUT"; exit 1; }
kill -INT "$A8" 2>/dev/null        # drop to monitor
sleep 2

printf 'DLIST\n' > "$FIFO"; sleep 2

echo "sweeping scanlines $YMIN..$YMAX step $STEP (~$(( (YMAX-YMIN)/STEP * 4 / 10 ))s)..."
y=$YMIN
while [ "$y" -le "$YMAX" ]; do
  printf 'BLINE %d\nCONT\n!echo ==L%d==\nGTIA\n' "$y" "$y" > "$FIFO"
  sleep 0.4
  y=$((y+STEP))
done
sleep 1
echo "capture -> $OUT  ($(grep -c '==L' "$OUT") scanlines, $(grep -c 'MODE\|BLANK\|JVB' "$OUT") DL entries)"
