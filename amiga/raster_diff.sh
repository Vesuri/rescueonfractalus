#!/usr/bin/env bash
# Generic FS-UAE + gdb driver for the rasterizer asm verification: boots to deep flight
# and runs $GDBSCRIPT (default raster_verify.gdb = the in-process asm-vs-C-oracle
# differential; build with `make VERIFY=1 PROBES=1`).  Usage:
#   GDBSCRIPT=raster_verify.gdb ./raster_diff.sh verify 420
# Source ../env.sh first (fs-uae + gdb on PATH).  Needs out/RoF.exe built PROBES=1.
set -uo pipefail
cd "$(dirname "$0")"
FSUAE="${FSUAE:-fs-uae}"; GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}"
LABEL="${1:-verify}"; DELAY="${2:-90}"; GDBSCRIPT="${GDBSCRIPT:-raster_verify.gdb}"
RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"; GDBHOME="$RUN/gdbhome"
mkdir -p "$DH0/s" "$DH1" "$RUN/state" "$GDBHOME"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF.exe "$DH1/RoF"
pkill -9 fs-uae 2>/dev/null || true; sleep 1
"$FSUAE" --amiga_model=A500+ --chip_memory=1024 --fast_memory=8192 \
  --kickstart_file="$ROM" --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --automatic_input_grab=0 --fullscreen=0 --window_width=720 --window_height=568 \
  --remote_debugger=20 --remote_debugger_port=2345 --remote_debugger_trigger=RoF \
  --ntsc_mode=0 --state_dir="$RUN/state" > "$RUN/fsuae-dbg.log" 2>&1 &
FSUAE_PID=$!
for i in $(seq 1 60); do
  kill -0 "$FSUAE_PID" 2>/dev/null || { echo "FS-UAE exited early"; exit 1; }
  lsof -nP -iTCP:2345 -sTCP:LISTEN >/dev/null 2>&1 && break; sleep 1
done
cat > "$RUN/connect.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 90
target remote 127.0.0.1:2345
EOF
env HOME="$GDBHOME" XDG_CACHE_HOME="$GDBHOME" \
  "$GDB" -q -l 10 -x "$RUN/connect.gdb" -x "$GDBSCRIPT" out/RoF.elf \
  > "$RUN/gdb-$LABEL.log" 2>&1 &
GDB_PID=$!
echo "gdb pid=$GDB_PID; running up to ${DELAY}s for the breakpoint..."
for i in $(seq 1 "$DELAY"); do kill -0 "$GDB_PID" 2>/dev/null || break; sleep 1; done
kill -INT "$GDB_PID" 2>/dev/null || true; sleep 2; kill -9 "$GDB_PID" 2>/dev/null || true
pkill -9 fs-uae 2>/dev/null || true
[ -f "$RUN/diff_heights.bin" ] && cp -f "$RUN/diff_heights.bin" "$RUN/heights-$LABEL.bin"
[ -f "$RUN/diff_terrain.bin" ] && cp -f "$RUN/diff_terrain.bin" "$RUN/terrain-$LABEL.bin"
echo "=== $LABEL ==="; grep -E "BREAK|dumped|VERIFY|mismatch" "$RUN/gdb-$LABEL.log" | tail -6
