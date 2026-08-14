#!/usr/bin/env bash
# Like diag_run.sh but fires repeated SIGINTs to PC-sample the main thread across the
# launch door window. START=interval between samples (s); COUNT=number of samples;
# WARMUP=seconds before first sample.
set -uo pipefail
cd "$(dirname "$0")"
. "${FSUAE_COMMON:-$HOME/.local/share/amiga/fsuae_common.sh}"
FSUAE="${FSUAE:-fs-uae}"
GDB="${GDB:-m68k-amiga-elf-gdb}"
ROM="${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}"
WARMUP="${1:-12}"
INTERVAL="${2:-0.5}"
COUNT="${3:-50}"

RUN=.run; DH0="$RUN/dh0"; DH1="$RUN/dh1"; GDBHOME="$RUN/gdbhome"
mkdir -p "$DH0/s" "$DH1" "$RUN/state" "$GDBHOME"
printf 'cd dh1:\nRoF\n' > "$DH0/s/startup-sequence"
cp -f out/RoF "$DH1/RoF"

fsuae_claim_port
"$FSUAE" \
  --amiga_model=A500+ --chip_memory=1024 --fast_memory=8192 \
  --kickstart_file="$ROM" \
  --hard_drive_0="$DH0" --hard_drive_1="$DH1" \
  --automatic_input_grab=0 --fullscreen=0 --window_width=720 --window_height=568 \
  --remote_debugger=20 --remote_debugger_port="$DEBUG_PORT" --remote_debugger_trigger=RoF \
  --ntsc_mode=0 --state_dir="$RUN/state" > "$RUN/fsuae-dbg.log" 2>&1 &
FSUAE_PID=$!
fsuae_track "$FSUAE_PID"
echo "FS-UAE pid=$FSUAE_PID; waiting for stub..."
for i in $(seq 1 60); do
  kill -0 "$FSUAE_PID" 2>/dev/null || { echo "FS-UAE exited early"; exit 1; }
  lsof -nP -iTCP:"$DEBUG_PORT" -sTCP:LISTEN >/dev/null 2>&1 && break
  sleep 1
done

cat > "$RUN/connect.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 90
target remote 127.0.0.1:$DEBUG_PORT
EOF

# Sample command STREAM on stdin, not a `while` loop in a -x script.  A SIGINT that
# lands inside the print block (instead of inside `continue`) raises gdb's Quit; that
# unwinds a sourced `while` loop for good, after which gdb reads stdin, hits EOF and
# exits — which used to truncate a 250-sample run at ~30.  Fed as a flat stream, a Quit
# only aborts the ONE block it interrupted: gdb reads the next `continue` and keeps
# sampling.  (diag_sample.gdb is kept for reference / manual use.)
{
  echo "set pagination off"
  echo "set confirm off"
  echo "set height 0"
  i=0
  while [ "$i" -lt $((COUNT + 40)) ]; do
    echo "continue"
    echo "printf \"S$i vbi=%u VVBLKI=%02x%02x 365B=%02x sp=%08x pc=0x%x SYM=\", g_vbiCount, mem[0x0223], mem[0x0222], mem[0x365B], \$sp, \$pc"
    echo "info symbol \$pc"
    i=$((i + 1))
  done
  echo "detach"
  echo "quit"
} > "$RUN/sample_cmds.gdb"

env HOME="$GDBHOME" XDG_CACHE_HOME="$GDBHOME" \
  "$GDB" -q -l 10 -x "$RUN/connect.gdb" out/RoF.elf \
  < "$RUN/sample_cmds.gdb" > "$RUN/gdb-sample.log" 2>&1 &
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
fsuae_stop
# Normalise the log back to the two-line "S<i> ... SYM=" / "<symbol> + N in section" shape
# prof_flight.py parses.  Reading commands from stdin makes gdb echo its "(gdb) " prompt, and
# since the sample printf ends without a newline the prompt+symbol land on the SAME line.
# Strip the prompts and split at SYM=.  (Raw stream kept alongside for debugging.)
cp -f "$RUN/gdb-sample.log" "$RUN/gdb-sample.raw.log"
sed -e 's/(gdb) //g' -e 's/ SYM=/ SYM=\'$'\n''/' "$RUN/gdb-sample.raw.log" > "$RUN/gdb-sample.log"

echo "=== samples by scene (VVBLKI: 4ff5=flight, 52d7=standby/cinematic) ==="
grep -oE "VVBLKI=[0-9a-f]+" "$RUN/gdb-sample.log" | sort | uniq -c | sort -rn
echo "=== hot functions (all scenes; use prof_flight.py for the flight-only profile) ==="
grep -E "in .* \(" "$RUN/gdb-sample.log" | sed -E 's/.* in ([A-Za-z0-9_:]+).*/\1/' | sort | uniq -c | sort -rn | head
