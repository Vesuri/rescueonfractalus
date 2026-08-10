# Boost return-to-mother-ship reverse-tunnel decode cost.
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=boost_probe.gdb ./diag_run.sh 120
#
# decodeBoostViewport re-decodes the whole 86-row x 40-byte x 3-plane viewport every frame of the
# reverse-tunnel sub-phase (docs/boost-cinematic-plan.md item 2 waved this through as "brief").
# g_bTunTicks/g_bTunDec = what that actually costs (1 tick = 1 scanline = 63.56us; ISR firings are
# already subtracted).  g_bTunGroups = four-byte source groups that genuinely CHANGED between
# consecutive decodes, out of 860 per decode -- the ceiling on what a content-keyed shadow can skip.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "=== boost reverse-tunnel decode ===\n"
printf "stars decodes      : %lu  (%lu ticks)\n", g_bStarDec, g_bStarTicks
printf "tunnel decodes     : %lu  (%lu ticks)\n", g_bTunDec, g_bTunTicks
if g_bTunDec > 0
  printf "  ticks/decode     : %lu  (= %lu us)\n", g_bTunTicks / g_bTunDec, (g_bTunTicks / g_bTunDec) * 64
  printf "  changed groups   : %lu total, %lu/decode of 860 (%lu%%)\n", g_bTunGroups, g_bTunGroups / g_bTunDec, (g_bTunGroups * 100) / (g_bTunDec * 860)
end
if g_bStarDec > 0
  printf "  stars ticks/dec  : %lu\n", g_bStarTicks / g_bStarDec
end
printf "=== phase routing ===\n"
printf "vbi=%u  boostRet=%u boostVp=%u liveCopper=%u handoffHold=%lu\n", g_vbiCount, g_boostRet, g_boostVp, g_liveCopper, g_boostHandoffHoldFrames
printf "mem[$3A]=%02x mem[$8D]=%02x mem[$8E]=%02x VVBLKI=%02x%02x\n", mem[0x3A], mem[0x8D], mem[0x8E], mem[0x223], mem[0x222]
