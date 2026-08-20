# In-process differential for the long-striding PMG run scans (pmgScanFirst / pmgScanBounds),
# used by the scope-P3, viewport-P3 and scanner-dot sprite mirrors.
#
# Why a differential and not `make validate`: these are Amiga-only render mirrors of the Atari PMG
# buffers, not a mem[] contract, so the validation harness cannot see them at all.  Every call
# re-derives the run with the ORIGINAL per-byte loop and compares against what the helper returned
# (the helper's answer is the one that goes live, so a surviving mismatch is the helper's fault).
#
# ⚠ g_scanHit is the load-bearing line.  g_scanBad == 0 over a run where the buffers were ALWAYS
# empty proves only that "found nothing" agrees with "found nothing" — run this with COMBAT=1 (no
# COMBAT_QUIET) so saucers/emplacements actually put an object in the P3 buffer, and check that
# g_scanHit is a healthy fraction of g_scanCalls before believing the zero.
#
# Build: make clean && make -j4 COMBAT=1 SCAN_VERIFY=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=scan_verify.gdb ./diag_run.sh 240
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2000
continue
printf "=== start: level=%u  $0623 emplace=%02x  $0621 saucer=%02x  VVBLKI=$%02x%02x $3D=%02x ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], mem[0x223], mem[0x222], mem[0x3D]

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u | calls %6lu  hits %6lu  raced %4lu  BAD %lu | VVBLKI=$%02x%02x $3D=%02x\n", \
    g_vbiCount, g_scanCalls, g_scanHit, g_scanRaced, g_scanBad, mem[0x223], mem[0x222], mem[0x3D]
end

seg 2600
seg 3200
seg 3800
seg 4400
seg 5000
seg 5600

printf "\n=== scan differential ===\n"
printf "  calls        %lu\n", g_scanCalls
printf "  found a run  %lu  (%lu%% — must be well above 0 or the test proved nothing)\n", \
  g_scanHit, (g_scanCalls ? (100*g_scanHit)/g_scanCalls : 0)
printf "  raced (ISR rewrote the buffer mid-check, discarded) %lu  (%lu%%)\n", \
  g_scanRaced, (g_scanCalls ? (100*g_scanRaced)/g_scanCalls : 0)
printf "  of which the SCANNER DOT %lu  (its check fires only on a change-gate miss; 0 = the\n", g_scanDotCalls
printf "                              dot was never compared and the zero below is vacuous)\n"
printf "  MISMATCHES   %lu   <-- must be 0\n", g_scanBad
printf "  last bad top oracle=%d helper=%d\n", \
  (short)(g_scanLastBadTop >> 16), (short)(g_scanLastBadTop & 0xFFFF)
printf "  last bad bot oracle=%d helper=%d\n", \
  (short)(g_scanLastBadBot >> 16), (short)(g_scanLastBadBot & 0xFFFF)
detach
quit
