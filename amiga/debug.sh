#!/usr/bin/env bash
# Source-level debug the Amiga RoF build via FS-UAE's GDB stub.
#   ./debug.sh [path-to-kickstart] [script.gdb]
# Build first, then run this. `continue` at the gdb prompt runs the program.
# HOME/XDG_CACHE_HOME must be set for gdb; connect to 127.0.0.1 (not localhost).
set -uo pipefail
cd "$(dirname "$0")"
. ./env.sh
. "$FSUAE_COMMON"

GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="${1:-$KICKSTART}"
[ -f "$ROM" ] || { echo "Kickstart ROM not found: $ROM  (pass as \$1 or set \$KICKSTART)"; exit 1; }
[ -f out/RoF.elf ] || { echo "build first: make"; exit 1; }

RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"; GDBHOME="$RUN/gdbhome"
mkdir -p "$DH0/s" "$DH1" "$RUN/state" "$GDBHOME"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF "$DH1/RoF"
# Stage only a locally supplied cartridge; it is never part of a release.
if [ -f "${ROF_ROM:-../rof.rom}" ]; then
  mkdir -p "$DH1/data"
  cp -f "${ROF_ROM:-../rof.rom}" "$DH1/data/rof.rom"
fi

fsuae_claim_port
# Quiet debug run (no sound, window behind the others): fsuae_options in $FSUAE_COMMON;
# it also passes $EXTRA_ARGS.
KICKSTART="$ROM" AMIGA_MODEL=A500+ CHIP_KB=1024 FAST_KB=8192 DEBUG=1
fsuae_options
FSUAE_LOG="$RUN/fsuae-dbg.log" fsuae_launch \
  --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --window_width=720 --window_height=568 \
  --remote_debugger_trigger=RoF --state_dir="$RUN/state"
echo "FS-UAE (gdb stub) pid=$FSUAE_PID; waiting for stub..."

for i in $(seq 1 60); do
  kill -0 "$FSUAE_PID" 2>/dev/null || { echo "FS-UAE exited early; see $RUN/fsuae-dbg.log"; exit 1; }
  lsof -nP -iTCP:"$DEBUG_PORT" -sTCP:LISTEN >/dev/null 2>&1 && break
  sleep 1
done

PREAMBLE="$RUN/connect.gdb"
{ printf 'set pagination off\nset confirm off\nset remotetimeout 90\n'
  printf 'target remote 127.0.0.1:%s\n' "$DEBUG_PORT"   # $DEBUG_PORT: see fsuae_common.sh
  cat <<'EOF'
echo \n>>> connected. `continue` runs; Ctrl-C breaks back in. <<<\n
EOF
} > "$PREAMBLE"

if [ "${2:-}" ] && [ -f "${2:-}" ]; then
  exec env HOME="$GDBHOME" XDG_CACHE_HOME="$GDBHOME" \
    "$GDB" -q -l 10 -x "$PREAMBLE" -x "$2" out/RoF.elf
fi
exec env HOME="$GDBHOME" XDG_CACHE_HOME="$GDBHOME" \
  "$GDB" -q -l 10 -x "$PREAMBLE" out/RoF.elf
