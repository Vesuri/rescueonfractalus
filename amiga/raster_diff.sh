#!/usr/bin/env bash
# Generic FS-UAE + gdb driver for the rasterizer asm verification: boots to deep flight
# and runs $GDBSCRIPT (default raster_verify.gdb = the in-process asm-vs-C-oracle
# differential; build with `make VERIFY=1 PROBES=1`).  Usage:
#   GDBSCRIPT=raster_verify.gdb ./raster_diff.sh verify 420
# Source ../env.sh first (fs-uae + gdb on PATH).  Needs out/RoF built PROBES=1.
set -uo pipefail
cd "$(dirname "$0")"
. ./env.sh
. "$FSUAE_COMMON"
GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="$KICKSTART"
LABEL="${1:-verify}"; DELAY="${2:-90}"; GDBSCRIPT="${GDBSCRIPT:-raster_verify.gdb}"
RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"; GDBHOME="$RUN/gdbhome"
mkdir -p "$DH0/s" "$DH1" "$RUN/state" "$GDBHOME"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF "$DH1/RoF"
fsuae_claim_port
# Quiet debug run (no sound, window behind the others): fsuae_options in $FSUAE_COMMON;
# it also passes $EXTRA_ARGS.
KICKSTART="$ROM" AMIGA_MODEL=A500+ CHIP_KB=1024 FAST_KB=8192 DEBUG=1
fsuae_options
FSUAE_LOG="$RUN/fsuae-dbg.log" fsuae_launch \
  --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --window_width=720 --window_height=568 \
  --remote_debugger_trigger=RoF --state_dir="$RUN/state"
for i in $(seq 1 60); do
  kill -0 "$FSUAE_PID" 2>/dev/null || { echo "FS-UAE exited early"; exit 1; }
  lsof -nP -iTCP:"$DEBUG_PORT" -sTCP:LISTEN >/dev/null 2>&1 && break; sleep 1
done
sleep 2   # the stub BINDS a moment before it will accept — without this gdb intermittently
          # dies on "could not connect: Connection refused" and the whole run yields no data
cat > "$RUN/connect.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 90
target remote 127.0.0.1:$DEBUG_PORT
EOF
env HOME="$GDBHOME" XDG_CACHE_HOME="$GDBHOME" \
  "$GDB" -q -l 10 -x "$RUN/connect.gdb" -x "$GDBSCRIPT" out/RoF.elf \
  > "$RUN/gdb-$LABEL.log" 2>&1 &
GDB_PID=$!
echo "gdb pid=$GDB_PID; running up to ${DELAY}s for the breakpoint..."
for i in $(seq 1 "$DELAY"); do kill -0 "$GDB_PID" 2>/dev/null || break; sleep 1; done
# SIGINT breaks gdb's `continue`; the rest of $GDBSCRIPT (the printf tallies) then runs.  Give
# it a real grace period — over the remote stub those printfs read dozens of globals and take
# seconds, and killing early is why an unreached breakpoint used to yield an EMPTY log.
kill -INT "$GDB_PID" 2>/dev/null || true; sleep 8; kill -9 "$GDB_PID" 2>/dev/null || true
fsuae_stop
[ -f "$RUN/diff_heights.bin" ] && cp -f "$RUN/diff_heights.bin" "$RUN/heights-$LABEL.bin"
[ -f "$RUN/diff_terrain.bin" ] && cp -f "$RUN/diff_terrain.bin" "$RUN/terrain-$LABEL.bin"
# PERF/FIRSTBAD were missing from this filter, so the per-call beam-tick comparison — the whole
# point of the differential — never reached the console and had to be grepped out of the log.
echo "=== $LABEL ==="
grep -E "BREAK|dumped|VERIFY|PERF|EDGE|FIRSTBAD|mismatch" "$RUN/gdb-$LABEL.log" | tail -8
