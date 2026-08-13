#!/usr/bin/env bash
# Run the Amiga RoF build in FS-UAE as an ECS A500+ (ECS Denise needed for
# BPLCON3 border-blanking; OCS A500 ignores it).
#   ./run.sh [path-to-kickstart-rom]
# Use KS 3.1 (auto-boots directory HDs). Left mouse button quits.
# Override ROM via $1 or $KICKSTART.
#
# Run a DIFFERENT binary than out/RoF.exe with $ROF_EXE — handy for A/B-ing two builds by
# eye or ear without rebuilding between each look, e.g.
#   ROF_EXE=RoF-asm.exe ./run.sh      vs      ROF_EXE=RoF-cmixer.exe ./run.sh
set -euo pipefail
cd "$(dirname "$0")"
. "${FSUAE_COMMON:-$HOME/.local/share/amiga/fsuae_common.sh}"

FSUAE="${FSUAE:-fs-uae}"
ROM="${1:-${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}}"
[ -f "$ROM" ] || { echo "Kickstart ROM not found: $ROM  (pass as \$1 or set \$KICKSTART)"; exit 1; }
EXE="${ROF_EXE:-out/RoF.exe}"
# Emulated machine: A500+ by default (the target; ECS Denise for BPLCON3 border-blanking).
# `AMIGA_MODEL=A1200 ./run.sh` checks the port on a faster CPU — beam-timing races that the
# slow A500 happens to land safely show up there.
MODEL="${AMIGA_MODEL:-A500+}"
[ -f "$EXE" ] || { echo "not found: $EXE  (build first: make, or set \$ROF_EXE)"; exit 1; }

RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"
mkdir -p "$DH0/s" "$DH1" "$RUN/state"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f "$EXE" "$DH1/RoF"
echo "running $EXE"

# ⚠ ALWAYS start from a clean FS-UAE state.  diag_run.sh / the gdb-stub harnesses share this
# --state_dir, and they leave a .uss saved while the CPU was halted on the grey first frame —
# resuming that makes ANY build look frozen and grey, which has cost hours of false bisecting.
# There is no reason to resume state here (the game needs none), so just wipe it every run.
rm -f "$RUN"/state/*.uss

# Screenshots: this fsemu-core FS-UAE takes them with HOST-KEY + S = hold F12, press S.
# The screenshot code reads the FSEMU_SCREENSHOTS_DIR env var (the --screenshots_output_dir
# config key is parsed but ignored by the fsemu core), so set it here.  Dir must exist.
SHOTS="${FSEMU_SCREENSHOTS_DIR:-$HOME/Pictures/Screenshots}"
mkdir -p "$SHOTS"
export FSEMU_SCREENSHOTS_DIR="$SHOTS"

fsuae_stop_previous
# After the exec this shell IS fs-uae, so record $$ as the emulator pid.
fsuae_track_self
exec "$FSUAE" \
  --amiga_model="$MODEL" \
  --chip_memory=1024 --fast_memory=8192 \
  --kickstart_file="$ROM" \
  --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --joystick_port_0=none --joystick_port_1=none \
  --automatic_input_grab=0 --fullscreen=0 --window_width=720 --window_height=568 \
  --ntsc_mode=0 --state_dir="$RUN/state" \
  --screenshots_output_dir="$SHOTS"
