#!/usr/bin/env bash
# Run the Amiga RoF build in FS-UAE as an ECS A500+ (ECS Denise needed for
# BPLCON3 border-blanking; OCS A500 ignores it).
#   ./run.sh [path-to-kickstart-rom]
# Use KS 3.1 (auto-boots directory HDs). Left mouse button quits.
# Override ROM via $1 or $KICKSTART.
set -euo pipefail
cd "$(dirname "$0")"

FSUAE="${FSUAE:-fs-uae}"
ROM="${1:-${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}}"
[ -f "$ROM" ] || { echo "Kickstart ROM not found: $ROM  (pass as \$1 or set \$KICKSTART)"; exit 1; }
[ -f out/RoF.exe ] || { echo "build first: make"; exit 1; }

RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"
mkdir -p "$DH0/s" "$DH1" "$RUN/state"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF.exe "$DH1/RoF"

pkill -9 fs-uae 2>/dev/null || true; sleep 1
exec "$FSUAE" \
  --amiga_model=A500+ \
  --chip_memory=1024 --fast_memory=8192 \
  --kickstart_file="$ROM" \
  --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --automatic_input_grab=0 --fullscreen=0 --window_width=720 --window_height=568 \
  --ntsc_mode=0 --state_dir="$RUN/state"
