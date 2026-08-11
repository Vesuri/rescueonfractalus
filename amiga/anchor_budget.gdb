# The phase budget over a window ANCHORED TO THE FLIGHT RISING EDGE instead of to an absolute
# vbi count.
#
# Why: FIXED_RNG re-pins the POKEY LFSR exactly at the flight rising edge, so from that instant the
# flight is deterministic and every build flies the SAME path over the SAME terrain.  What is NOT
# pinned is when that edge happens — it sits after the whole boot + attract + launch cinematic, and
# any commit that changes the launch path's duration moves it.  A window anchored at "vbi >= 1900"
# therefore covers a DIFFERENT STRETCH of the flight in each build, and DRAW t/it varies by ~15%
# from stretch to stretch (measured: 836-1141 across 250-vbi segments of one run).  Anchoring the
# window to the edge removes that confound.
#
# All convenience variables are $sXxx ($ps/$pc/$sp/$fp/$a0-$a7/$d0-$d7 are m68k REGISTER names and
# assigning to them writes the emulated CPU).
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if mem[0x223] == 0x4f && mem[0x222] == 0xf5
continue
set $sEntry = g_vbiCount
printf "=== FLIGHT ENTRY at vbi %u ===\n", $sEntry

# Snapshot the accumulators at entry+400 so the window measures ONLY the anchored stretch.
tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $sEntry + 400
continue
set $sD0 = g_clPh[2][0] + g_clPh[2][1]
set $sS0 = g_clPh[0][0] + g_clPh[0][1]
set $sC0 = g_clPh[1][0] + g_clPh[1][1]
set $sF0 = g_clPh[5][0] + g_clPh[5][1]
set $sI0 = g_clPhIter[0] + g_clPhIter[1]
set $sP0 = g_fdCalls
printf "=== window opens at vbi %u (entry+400): level=%u alt=%02x  iters so far %lu ===\n", \
  g_vbiCount, mem[0x6D], mem[0x28DA], (unsigned long)$sI0

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $sEntry + $arg0
  continue
  printf "entry+%4d (vbi %5u) | iters %4lu | painted %4lu | alt %02x | head %02x%02x | VVBLKI=$%02x%02x\n", \
    $arg0, g_vbiCount, (unsigned long)(g_clPhIter[0]+g_clPhIter[1] - $sI0), \
    (unsigned long)(g_fdCalls - $sP0), mem[0x28DA], mem[0x2886], mem[0x2885], \
    mem[0x223], mem[0x222]
end

seg 900
seg 1400
seg 1900
seg 2400
seg 2900
seg 3400
seg 3900

set $sIt = (g_clPhIter[0] + g_clPhIter[1]) - $sI0
printf "\n=== ANCHORED window (entry+400 .. entry+3900), %lu iterations ===\n", $sIt
printf "  SETUP %4lu  CLEAR %3lu  DRAW %5lu  FRAME %4lu  | painted %lu\n", \
  ($sIt ? ((g_clPh[0][0]+g_clPh[0][1]) - $sS0)/$sIt : 0), \
  ($sIt ? ((g_clPh[1][0]+g_clPh[1][1]) - $sC0)/$sIt : 0), \
  ($sIt ? ((g_clPh[2][0]+g_clPh[2][1]) - $sD0)/$sIt : 0), \
  ($sIt ? ((g_clPh[5][0]+g_clPh[5][1]) - $sF0)/$sIt : 0), \
  (unsigned long)(g_fdCalls - $sP0)
printf "=== iterations %lu, flight entry vbi %u ===\n", $sIt, $sEntry
detach
quit
