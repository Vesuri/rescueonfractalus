#!/usr/bin/env bash
# Launch FS-UAE gdb-stub, connect gdb, let RoF run, SIGINT gdb after a delay so it
# breaks in and prints the standby-build timing probes.
set -uo pipefail
cd "$(dirname "$0")"
FSUAE="${FSUAE:-fs-uae}"
GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}"
DELAY="${1:-14}"

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
  kill -0 "$FSUAE_PID" 2>/dev/null || { echo "FS-UAE exited early; see $RUN/fsuae-dbg.log"; exit 1; }
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
  "$GDB" -q -l 10 -x "$RUN/connect.gdb" -x "${GDBSCRIPT:-diag_timing.gdb}" out/RoF.elf \
  > "$RUN/gdb-out.log" 2>&1 &
GDB_PID=$!
echo "gdb pid=$GDB_PID; running for ${DELAY}s..."
sleep "$DELAY"
kill -INT "$GDB_PID" 2>/dev/null || true
# give gdb time to print + detach
for i in $(seq 1 20); do kill -0 "$GDB_PID" 2>/dev/null || break; sleep 1; done
kill -INT "$GDB_PID" 2>/dev/null || true
sleep 2
kill -9 "$GDB_PID" 2>/dev/null || true
pkill -9 fs-uae 2>/dev/null || true
echo "=== gdb output (filtered) ==="
grep -v "Internal error: pc" "$RUN/gdb-out.log" | grep -vE "^warning:" | tail -40
