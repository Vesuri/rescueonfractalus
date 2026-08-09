# Coverage check for the PER-CELL cockpit registries (lock-on strip + digit blocks).
#
# Those decoders re-render only the cells a writer flagged, so the risk is NOT arithmetic — the
# decode function is unchanged — it is a MISSED write leaving a stale cell.  An arithmetic
# differential is structurally blind to that (flight-measurement-rules: "a differential cannot
# clear a change whose risk is someone ELSE writes this").  So the test is: right after the
# targeted decode, decode the WHOLE group again and compare the destination bytes.  If the
# targeted decode was complete, the full decode is a no-op.
#
#   g_ckVerBad   MUST be 0        — a targeted decode left the strip stale
#   g_ckVerRaced  discarded calls — the VBI ISR rewrote a source cell between the two full passes
#   g_ckVerCalls  must be >> 0    — 0 bad out of 0 calls proves nothing
#
# Build: make clean && make -j4 COMBAT=1 CK_VERIFY=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=ck_verify.gdb ./diag_run.sh 400
#
# ⚠ Run under COMBAT=1 (not COMBAT_QUIET): the digit blocks only change on kills/rescues/score, so
# a quiet run exercises the lock-on strip well but barely touches the digits.
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2200
continue
printf "=== level=%u  VVBLKI=$%02x%02x $3D=%02x  (start vbi=%u) ===\n", \
  mem[0x6D], mem[0x223], mem[0x222], mem[0x3D], g_vbiCount

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u painted %4u | ckVer %6lu BAD %4lu (lock %lu / digit %lu) raced %5lu | lockFires %5lu cells %6lu | digitFires %4lu blocks %5lu | $3D=%02x\n", \
    g_vbiCount, g_fdCalls, g_ckVerCalls, g_ckVerBad, g_ckVerBadG[0], g_ckVerBadG[1], g_ckVerRaced, \
    g_ckLockFires, g_ckLockCells, g_ckDigitFires, g_ckDigitBlocks, mem[0x3D]
end

seg 2700
seg 3200
seg 3700
seg 4200

printf "\n=== VERDICT ===\n"
printf "  ckVerify calls   %8lu   (must be >> 0, else nothing was tested)\n", g_ckVerCalls
printf "  MISMATCH (BAD)   %8lu   <-- MUST BE 0: a targeted decode left a cell stale\n", g_ckVerBad
printf "    ...lock-on group %6lu bad / %lu checks\n", g_ckVerBadG[0], g_ckVerCallsG[0]
printf "    ...digit  group  %6lu bad / %lu checks\n", g_ckVerBadG[1], g_ckVerCallsG[1]
printf "  raced (discarded)%8lu   (%lu%% of calls — the ISR rewrote a source cell mid-check)\n", \
  g_ckVerRaced, (g_ckVerCalls ? (100*g_ckVerRaced)/g_ckVerCalls : 0)
printf "  lock-on: %lu fires, %lu cells decoded (%lu.%02lu/fire, was 7 every fire)\n", \
  g_ckLockFires, g_ckLockCells, \
  (g_ckLockFires ? g_ckLockCells/g_ckLockFires : 0), \
  (g_ckLockFires ? ((100*g_ckLockCells)/g_ckLockFires)%100 : 0)
printf "  digits : %lu fires, %lu blocks decoded (%lu.%02lu/fire, was 6 every fire)\n", \
  g_ckDigitFires, g_ckDigitBlocks, \
  (g_ckDigitFires ? g_ckDigitBlocks/g_ckDigitFires : 0), \
  (g_ckDigitFires ? ((100*g_ckDigitBlocks)/g_ckDigitFires)%100 : 0)
detach
quit
