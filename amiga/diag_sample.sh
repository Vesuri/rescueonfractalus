#!/usr/bin/env bash
# Like diag_run.sh but fires repeated SIGINTs to PC-sample the main thread across the
# launch door window. START=interval between samples (s); COUNT=number of samples;
# WARMUP=seconds before first sample.
set -uo pipefail
cd "$(dirname "$0")"
FSUAE="${FSUAE:-fs-uae}"
GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}"
WARMUP="${1:-12}"
INTERVAL="${2:-0.5}"
COUNT="${3:-50}"

RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"; GDBHOME="$RUN/gdbhome"
mkdir -p "$DH0/s" "$DH1" "$RUN/state" "$GDBHOME"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF.exe "$DH1/RoF"

pkill -9 fs-uae 2>/dev/null || true; sleep 1
"$FSUAE" \
  --amiga_model=A500+ --chip_memory=1024 --fast_memory=8192 \
  --kickstart_file="$ROM" \
  --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --automatic_input_grab=0 --fullscreen=0 --window_width=720 --window_height=568 \
  --remote_debugger=20 --remote_debugger_port=2345 --remote_debugger_trigger=RoF \
  --ntsc_mode=0 --state_dir="$RUN/state" > "$RUN/fsuae-dbg.log" 2>&1 &
FSUAE_PID=$!
echo "FS-UAE pid=$FSUAE_PID; waiting for stub..."
for i in $(seq 1 60); do
  kill -0 "$FSUAE_PID" 2>/dev/null || { echo "FS-UAE exited early"; exit 1; }
  lsof -nP -iTCP:2345 -sTCP:LISTEN >/dev/null 2>&1 && break
  sleep 1
done

cat > "$RUN/connect.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 90
target remote 127.0.0.1:2345
EOF

env HOME="$GDBHOME" XDG_CACHE_HOME="$GDBHOME" \
  "$GDB" -q -l 10 -x "$RUN/connect.gdb" -x diag_sample.gdb out/RoF.elf \
  > "$RUN/gdb-sample.log" 2>&1 &
GDB_PID=$!
echo "gdb pid=$GDB_PID; warmup ${WARMUP}s then ${COUNT} samples @ ${INTERVAL}s..."
sleep "$WARMUP"
for i in $(seq 1 "$COUNT"); do
  kill -0 "$GDB_PID" 2>/dev/null || break
  kill -INT "$GDB_PID" 2>/dev/null || true
  sleep "$INTERVAL"
done
sleep 1
kill -9 "$GDB_PID" 2>/dev/null || true
pkill -9 fs-uae 2>/dev/null || true
echo "=== samples (door phase = 060B=35) ==="
grep -E "^S[0-9]" "$RUN/gdb-sample.log"
echo "=== hot functions (door phase only) ==="
grep -E "in .* \(" "$RUN/gdb-sample.log" | sed -E 's/.* in ([A-Za-z0-9_:]+).*/\1/' | sort | uniq -c | sort -rn | head
