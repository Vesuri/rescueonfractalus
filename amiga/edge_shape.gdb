# EDGE-ONLY structural shape dump — the EDGE section of shape_probe.gdb on its own.
#
# shape_probe.gdb bundles EDGE + BAND + TFS and gdb ABORTS on the first missing symbol, so it can
# only be used on a build that has all three flags on.  This one needs EDGE_SHAPE=1 alone, which
# is what you want when sizing renderFlightDirect's 160-column plane-1 scatter-OR.
#
# Build:  make clean && make -j4 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1 \
#                              COMBAT=1 COMBAT_QUIET=1 EDGE_SHAPE=1
# Run:    . ./env.sh && GDBSCRIPT=edge_shape.gdb ./diag_run.sh 120
#
# What each row decides:
#   $FF%              — the per-column `cmp.b #$FF / beq` off-top skip costs 16 cycles on every
#                       column that is NOT off-top.  A table sentinel that routes $FF to a scratch
#                       gutter byte removes it, at +26 cycles on the ones that ARE.  Break-even is
#                       $FF ≈ 38%.
#   byte-ORs needed   — how many of the 160 read-modify-write ORs survive if columns sharing a plane-1
#                       byte AND a row are merged into one OR with a combined mask.  Each OR is
#                       ~22 cycles of CHIP-RAM RMW, so this is the big one.
#   distinct-per-group— the whole story for that merge: mass at 1-2 means write it, mass at 4 means
#                       the scatter is already minimal.
set pagination off
set confirm off
continue
echo \n==== SIGINT ====\n
printf "iter=%u vbi=%u fdCalls=%u  VVBLKI=$%02x%02x $3D=%02x\n", \
  g_iterCount, g_vbiCount, g_fdCalls, mem[0x223], mem[0x222], mem[0x3D]
echo \n---- EDGE PLOT (renderFlightDirect, 160 cols/frame, 40 plane-1 bytes) ----\n
printf "frames=%lu  cols=%lu  $FF(skipped)=%lu (%lu%%)\n", \
  g_epFrames, g_epFrames*160, g_epFF, (g_epFrames? 100*g_epFF/(g_epFrames*160) : 0)
printf "same-height-as-prev-col=%lu (%lu%%)   same-ROW-as-prev-col=%lu (%lu%%)\n", \
  g_epSameH,   (g_epFrames? 100*g_epSameH/(g_epFrames*160) : 0), \
  g_epSameRow, (g_epFrames? 100*g_epSameRow/(g_epFrames*160) : 0)
printf "lookups needed (cache prev h) = %lu/frame  vs 160  -> %lu%% of the table reads\n", \
  (g_epFrames? g_epLookups/g_epFrames : 0), (g_epFrames? 100*g_epLookups/(g_epFrames*160) : 0)
printf "byte-ORs needed (merge in group) = %lu/frame  vs %lu (=160-$FF) -> %lu%% of the ORs\n", \
  (g_epFrames? g_epORs/g_epFrames : 0), \
  (g_epFrames? (g_epFrames*160-g_epFF)/g_epFrames : 0), \
  ((g_epFrames*160-g_epFF)? 100*g_epORs/(g_epFrames*160-g_epFF) : 0)
printf "clamped rows: row0(h>=150)=%lu  row46(h<=104)=%lu\n", g_epRow0, g_epRow46
printf "uniform 4-col groups=%lu/%lu (%lu%%)   adjacent uniform PAIRS (or.w)=%lu\n", \
  g_epGroupAllSame, g_epFrames*40, (g_epFrames? 100*g_epGroupAllSame/(g_epFrames*40) : 0), \
  g_epWordSame
echo distinct rows per 4-col group (0=all-$FF, 1..4):\n
set $i = 0
while $i < 5
  printf "  [%d] %lu", $i, g_epGroupDistinct[$i]
  set $i = $i + 1
end
printf "\n"
# ⭐ THE decision number for the filed "pairwise (0,1)/(2,3) merge": it is −36 cycles on a hit and
# +24 on a miss with the split path out of line, so it needs p > 40% to pay at all.  g_epSameRow's
# 52% is a proxy over every adjacent column; this is the exact pairing the merge would use.
printf "pairwise (0,1)/(2,3) same-height: %lu/%lu (%lu%%)  of which $FF==$FF: %lu (%lu%%)", \
  g_epPairSame, g_epPairs, (g_epPairs ? 100*g_epPairSame/g_epPairs : 0), \
  g_epPairSameFF, (g_epPairs ? 100*g_epPairSameFF/g_epPairs : 0)
printf "   [break-even 40%%]\n"
echo same-row RUN length histogram (1..7 exact, 8=8+):\n
set $i = 1
while $i < 9
  printf "  [%d] %lu", $i, g_epRunHist[$i]
  set $i = $i + 1
end
printf "\n"
detach
quit
