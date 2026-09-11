# Reverse-tunnel COLOR00 / OCS-border trace.
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=boost_col00.gdb ./diag_run.sh 130
#
# On OCS the unblanked border is color00, so color00 must equal the colour of the pixels at the
# viewport's horizontal EDGES for the whole cinematic.  This prints, per boost frame, the
# DISPLAYED list's color00 + pen2 + the reveal K it was published with, against the pen actually
# at the viewport's left edge in each of the two source bitmaps (ring row 43, stars row 5).
# Reading it: col00 must equal whichever pen edgeR names — pen0's colour before the reveal ends
# ($8D 01, the value-8 surround), pen0's colour after the pen swap too (edgeR then names the
# value-2 outer ring), and pen2's colour once the wipe has covered the edges ($8D 00).
# A col00 of 020 (= atariToOCS($C0)) anywhere before $8D reaches 00 is the dark-green flash that
# the pen swap landing outside pass A's $008D<0 window produces.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "=== pokeTunnelOuterRing firings (each pokes the LIVE list's color00 = $0071) ===\n"
printf "count=%u\n", g_borN
set $i = 0
while $i < 8 && $i < g_borN
  printf "  [%d] vbi=%5lu  $94=%02lx  $71=%02lx\n", $i, (g_borW[$i] >> 16), \
    (g_borW[$i] >> 8) & 0xff, g_borW[$i] & 0xff
  set $i = $i + 1
end
printf "=== per boost render frame (last %d), oldest first ===\n", 240
printf "total=%lu  idx=%u\n", g_bcTot, g_bcIdx
printf "  MISMATCH when col00 != the colour of the edge pen it should continue\n"
printf "    vbi col00 pen2  Kdsp Kpub edgeR/S  $8D $8E $88 $94 $71 $08D8 br bandTopReg\n"
set $n = g_bcTot
set $base = 0
if $n > 240
  set $n = 240
  set $base = g_bcIdx
end
set $j = 0
while $j < $n
  set $i = $base + $j
  while $i >= 240
    set $i = $i - 240
  end
  printf "  %5u  %03x  %03x   %2u   %2u    %u/%u     %02x  %02x  %02x  %02x  %02x  %02x   %x   %03x\n", \
    g_bcVbi[$i], g_bcCol0[$i], g_bcPen2[$i], g_bcKdisp[$i], g_bcKpub[$i], \
    (g_bcEdge[$i] >> 4) & 7, g_bcEdge[$i] & 7, \
    g_bc8D[$i], g_bc8E[$i], g_bc88[$i], g_bc94[$i], g_bc71[$i], g_bcD8[$i], g_bcBr[$i], g_bcBTop[$i]
  set $j = $j + 1
end
printf "=== context ===\n"
printf "vbi=%u boostRet=%u boostVp=%u liveCopper=%u handoffHold=%lu\n", g_vbiCount, g_boostRet, g_boostVp, g_liveCopper, g_boostHandoffHoldFrames
printf "mem[$3A]=%02x mem[$8D]=%02x mem[$8E]=%02x mem[$71]=%02x mem[$94]=%02x\n", mem[0x3A], mem[0x8D], mem[0x8E], mem[0x71], mem[0x94]
printf "reveal K: min=%u  boost vbi %u..%u  changes=%u\n", g_rkMin, g_rkFirstVbi, g_rkLastVbi, g_rkN
