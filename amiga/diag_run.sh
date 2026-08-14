#!/usr/bin/env bash
# Launch FS-UAE gdb-stub, connect gdb, let RoF run, SIGINT gdb after a delay so it
# breaks in and prints the standby-build timing probes.
set -uo pipefail
cd "$(dirname "$0")"
. "${FSUAE_COMMON:-$HOME/.local/share/amiga/fsuae_common.sh}"
FSUAE="${FSUAE:-fs-uae}"
GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}"
DELAY="${1:-14}"
# Emulated machine.  Default A500+ (the target, + ECS Denise for BPLCON3 border-blanking).
# $AMIGA_MODEL=A1200 re-runs the same probe on a 68020 to expose beam-timing races that a
# faster CPU moves into the danger window.
MODEL="${AMIGA_MODEL:-A500+}"
# Optional extra fs-uae args, e.g. EXTRA_ARGS="--cpu=68040 --jit_compiler=1".
EXTRA_ARGS="${EXTRA_ARGS:-}"

RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"; GDBHOME="$RUN/gdbhome"
mkdir -p "$DH0/s" "$DH1" "$RUN/state" "$GDBHOME"
# ⚠ Do NOT "simplify" this to a single `dh1:RoF` line.  Tried 2026-08-14 and reverted: with
# the path form, gdb resolves this file's symbols against base $7500 instead of the usual
# ~$21f8e0 and then NO breakpoint is ever hit, so every run looks like a hang.  ($7500 is
# also what you get when the program never loads at all, so the likeliest reading is that
# --remote_debugger_trigger=RoF stops matching and no segment base is ever reported — but
# only the symptom was confirmed, not the mechanism.)  The `cd` is load-bearing; leave it.
# (Consequence: this harness needs KS 2.0+, since `cd` is only a ROM-resident Shell builtin
# from 2.0 on — a KS 1.3 boot dies here with "Unknown command cd".  The game itself is
# 1.3-clean; the WHDLoad install boots 1.3 through its own slave, not through this script.)
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF "$DH1/RoF"

fsuae_claim_port
"$FSUAE" \
  --amiga_model="$MODEL" --chip_memory=1024 --fast_memory=8192 \
  --kickstart_file="$ROM" \
  --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --automatic_input_grab=0 --fullscreen=0 --window_width=720 --window_height=568 \
  $EXTRA_ARGS \
  --remote_debugger=20 --remote_debugger_port="$DEBUG_PORT" --remote_debugger_trigger=RoF \
  --ntsc_mode=0 --state_dir="$RUN/state" > "$RUN/fsuae-dbg.log" 2>&1 &
FSUAE_PID=$!
fsuae_track "$FSUAE_PID"
echo "FS-UAE pid=$FSUAE_PID; waiting for stub..."
for i in $(seq 1 60); do
  kill -0 "$FSUAE_PID" 2>/dev/null || { echo "FS-UAE exited early; see $RUN/fsuae-dbg.log"; exit 1; }
  lsof -nP -iTCP:"$DEBUG_PORT" -sTCP:LISTEN >/dev/null 2>&1 && break
  sleep 1
done

cat > "$RUN/connect.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 90
target remote 127.0.0.1:$DEBUG_PORT
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
fsuae_stop
echo "=== gdb output (filtered) ==="
grep -v "Internal error: pc" "$RUN/gdb-out.log" | grep -vE "^warning:" | tail -40
