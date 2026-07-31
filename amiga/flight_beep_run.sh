#!/usr/bin/env bash
# Interactive flight range-1 pilot-beep capture harness (headless-drivable).
#   Build first:  make clean && make -j4 PROBES=1 BEEP_CAP=1
#   Then:         . ./env.sh && ./flight_beep_run.sh   (runs in background)
# Launches FS-UAE (gdb stub) with a window on screen + attaches gdb (flight_beep.gdb)
# which `continue`s while YOU fly to a downed pilot in the window.  The BEEP_CAP ring
# auto-arms in flight when the range-to-pilot digit $0642 hits 1/2 and records 320
# frames of all 4 Paula channels + POKEY shadow + every SFX event push (id+vbi).
# When done approaching a pilot, LEAVE FS-UAE RUNNING and SIGINT the gdb pid (written
# to .run/flightbeep.gdbpid) to dump:  kill -INT "$(cat .run/flightbeep.gdbpid)"
# (closing FS-UAE first kills the target before the dump).  Output: .run/flightbeep-gdb.out
# See the pilot-proximity-beep memory for the analysis recipe.
set -uo pipefail
cd "$(dirname "$0")"
FSUAE="${FSUAE:-fs-uae}"; GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="${1:-${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}}"
[ -f "$ROM" ] || { echo "Kickstart ROM not found: $ROM (pass \$1 or set \$KICKSTART)"; exit 1; }
[ -f out/RoF.exe ] || { echo "build first: make PROBES=1 BEEP_CAP=1"; exit 1; }
RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"; GDBHOME="$RUN/gdbhome"
mkdir -p "$DH0/s" "$DH1" "$RUN/state" "$GDBHOME"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF.exe "$DH1/RoF"
rm -f "$RUN/state/"*.uss    # cold boot (wipe stale savestate)
pkill -9 fs-uae 2>/dev/null || true; pkill -9 m68k-amiga-elf-gdb 2>/dev/null || true; sleep 1
"$FSUAE" --amiga_model=A500+ --chip_memory=1024 --fast_memory=8192 \
  --kickstart_file="$ROM" --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --automatic_input_grab=0 --fullscreen=0 --window_width=720 --window_height=568 \
  --remote_debugger=20 --remote_debugger_port=2345 --remote_debugger_trigger=RoF \
  --ntsc_mode=0 --state_dir="$RUN/state" > "$RUN/fsuae-flightbeep.log" 2>&1 &
FSPID=$!
for i in $(seq 1 60); do
  kill -0 "$FSPID" 2>/dev/null || { echo "FS-UAE exited early"; cat "$RUN/fsuae-flightbeep.log"; exit 1; }
  lsof -nP -iTCP:2345 -sTCP:LISTEN >/dev/null 2>&1 && break
  sleep 1
done
echo ">>> FS-UAE up (pid=$FSPID). Fly to a pilot; then: kill -INT \$(cat $RUN/flightbeep.gdbpid) <<<"
env HOME="$GDBHOME" XDG_CACHE_HOME="$GDBHOME" "$GDB" -q -l 10 -x "${BEEPGDB:-flight_beep.gdb}" out/RoF.elf > "$RUN/flightbeep-gdb.out" 2>&1 &
GDBPID=$!; echo "$GDBPID" > "$RUN/flightbeep.gdbpid"
echo ">>> gdb pid=$GDBPID; dump lands in $RUN/flightbeep-gdb.out <<<"
wait "$GDBPID"
pkill -9 fs-uae 2>/dev/null || true
