#!/usr/bin/env bash
# xex_dead_ab.sh — one scenario of the "are rof.xex's code bytes dead?" A/B.
#
#   ./xex_dead_ab.sh <label> <vbi> <seconds> [make flags...]
#
# Builds the SAME make flags twice, once with the real rof.xex and once with a same-length
# copy whose candidate-dead instruction bytes are zeroed (tools/xex_deadset.py), dumps mem[]
# at the SAME emulated frame in both, and diffs with the dead set masked out.
#
# Three things it exists to get right:
#  * the two binaries must be the same LENGTH, or the comparison is invalid — a shorter
#    binary does a different number of main-loop iterations per vblank and diverges the
#    read-clocked $D20A LFSR (feedback-same-length-asset-ab).  It asserts this.
#  * the frame lock breaks in the VBI ISR (fires every vblank) on an EQUALITY condition, not
#    on renderFrame, which runs below 50 Hz in flight and cannot hit a fixed frame.
#  * `make clean` between arms: this Makefile tracks neither .incbin deps nor PROBES
#    toggles, so a partial rebuild links a stale, working-but-wrong binary.
#
# Adds FIXED_RNG=1 unless the caller passes it, so the flown level and terrain are pinned.
set -uo pipefail
cd "$(dirname "$0")"
ROOT=".."
LABEL="${1:?label}"; VBI="${2:?vbi}"; SECS="${3:?seconds}"; shift 3
FLAGS=("$@")
case " ${FLAGS[*]} " in *" FIXED_RNG="*) ;; *) FLAGS+=(FIXED_RNG=1);; esac

PROBE="/tmp/rof_dead_${LABEL}.gdb"
cat > "$PROBE" <<EOF
set width 0
set pagination off
break 'PlatformAmiga.cpp':2235 if g_vbiCount == ${VBI}
continue
echo \\n==== FRAME-LOCKED DUMP ====\\n
printf "vbiCount=%u bootScene=%u VVBLKI=%02x%02x state(41)=%02x\\n", \\
  g_vbiCount, g_bootScene, mem[0x0223], mem[0x0222], mem[0x0041]
dump binary memory /tmp/rof_mem.bin &mem[0] &mem[65536]
echo dumped\\n
echo ==== end ====\\n
EOF

python3 "$ROOT/tools/xex_deadset.py" /tmp/rof_codezero.xex || exit 1
cp -f "$ROOT/rof.xex" /tmp/rof_xex_orig.bin || exit 1
trap 'cp -f /tmp/rof_xex_orig.bin "$ROOT/rof.xex"' EXIT       # always put the real asset back

declare -A SIZE
for ARM in intact zeroed; do
  if [ "$ARM" = intact ]; then cp -f /tmp/rof_xex_orig.bin "$ROOT/rof.xex"
  else                         cp -f /tmp/rof_codezero.xex  "$ROOT/rof.xex"; fi
  echo "=== [$LABEL/$ARM] make clean && make ${FLAGS[*]} ==="
  make clean >/dev/null 2>&1
  if ! make "${FLAGS[@]}" > "/tmp/rof_dead_${LABEL}_${ARM}.buildlog" 2>&1; then
    echo "BUILD FAILED — see /tmp/rof_dead_${LABEL}_${ARM}.buildlog"; exit 1
  fi
  # NOT `stat -f%z`: env.sh puts GNU coreutils on PATH, where -f means --file-system, so the
  # BSD form fails and SIZE would be EMPTY — making the equality check below pass vacuously.
  SIZE[$ARM]=$(wc -c < out/RoF.exe | tr -d ' ')
  echo "    RoF.exe = ${SIZE[$ARM]} B"
  rm -f /tmp/rof_mem.bin
  GDBSCRIPT="$PROBE" ./diag_run.sh "$SECS" 2>&1 | sed -n '/==== FRAME-LOCKED/,/==== end/p'
  if [ ! -s /tmp/rof_mem.bin ]; then
    echo "NO DUMP for $ARM (frame $VBI never reached in ${SECS}s?)"; exit 1
  fi
  cp -f /tmp/rof_mem.bin "/tmp/rof_dead_${LABEL}_${ARM}.bin"
done

if [ "${SIZE[intact]}" != "${SIZE[zeroed]}" ]; then
  echo "!! binaries differ in size (${SIZE[intact]} vs ${SIZE[zeroed]}) — comparison INVALID"
  exit 1
fi
echo "=== both arms ${SIZE[intact]} B (code identical) ==="
python3 "$ROOT/tools/xex_dead_diff.py" \
  "/tmp/rof_dead_${LABEL}_intact.bin" "/tmp/rof_dead_${LABEL}_zeroed.bin" "[$LABEL]"
