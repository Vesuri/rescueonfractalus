# STRUCTURAL shape dump for the two never-shape-probed flight buckets: renderFlightDirect's
# 160-column plane-1 scatter-OR edge plot, and terrain_frame_setup's per-cell loop-1 math.
# Build:  make clean && make -j4 PROBES=1 PROFILE_NORING=1 FIXED_RNG=1 \
#                              EDGE_SHAPE=1 TFSETUP_C=1 TFS_SHAPE=1
#   (TFSETUP_C=1 is mandatory — the loop-1 counters live in the C oracle, which the asm twin
#    replaces by default.  FIXED_RNG pins the level so the numbers are comparable across runs.)
# Run:    GDBSCRIPT=shape_probe.gdb ./diag_run.sh 100
#
# Reads out, per frame:
#  EDGE  — how much of the 160-column scatter is redundant work.  g_epLookups = the table lookups
#          a loop that cached the previous height would still need (vs 160); g_epORs = the byte-ORs
#          a loop that merged same-row masks within each 4-column plane-1 byte would issue (vs 160
#          minus $FF columns).  The distinct-rows-per-group histogram is the whole story: mass at
#          1-2 means the merge is worth writing, mass at 4 means the scatter is already minimal.
#  TFS   — the u/v recurrence test (g_tfsRecurBad MUST be 0 for the register-carried restructure to
#          be legal), the pattern-decode branch mix (how deep the per-cell btst chain runs), and the
#          classify mix.
set pagination off
set confirm off
continue
echo \n==== SIGINT ====\n
printf "iter=%u vbi=%u fdCalls=%u\n", g_iterCount, g_vbiCount, g_fdCalls
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
echo same-row RUN length histogram (1..7 exact, 8=8+):\n
set $i = 1
while $i < 9
  printf "  [%d] %lu", $i, g_epRunHist[$i]
  set $i = $i + 1
end
printf "\n"
echo \n---- BAND BLOCK (the g_fdBand bucket, 3 loops; needs BAND_SHAPE=1) ----\n
printf "ticks/call: object-overlay=%lu crosshair=%lu band-composite=%lu\n", \
  (g_bsBandFrames? g_bsObj/g_bsBandFrames:0), (g_bsBandFrames? g_bsCross/g_bsBandFrames:0), \
  (g_bsBandFrames? g_bsBand/g_bsBandFrames:0)
printf "  raw: obj=%lu cross=%lu band=%lu  (frames=%lu)\n", \
  g_bsObj, g_bsCross, g_bsBand, g_bsBandFrames
printf "object overlay: activeFrames=%lu rows/activeFrame=%lu/10 fullScan=%lu boxScan=%lu nonzero=%lu (%lu%% of box)\n", \
  g_bsObjFrames, (g_bsObjFrames? 10*g_bsObjRows/g_bsObjFrames:0), g_bsObjRows*40, g_bsObjBox, \
  g_bsObjBytes, (g_bsObjBox? 100*g_bsObjBytes/g_bsObjBox : 0)
printf "*** BAND FIELD STABILITY: changed bytes/frame = %lu/10 of 160  (clean frames=%lu/%lu, max chg=%lu)\n", \
  (g_bsBandFrames? 10*g_bsBandChanged/g_bsBandFrames:0), g_bsBandClean, g_bsBandFrames, g_bsBandMaxChg
printf "    overwrite(ow!=0) bytes/frame = %lu of 160  (the rest are plane3-only stores)\n", \
  (g_bsBandFrames? g_bsBandOwNz/g_bsBandFrames:0)
printf "    changes after the first 8 frames (entry transient excluded) = %lu\n", g_bsChgLate
echo     WHICH positions are dynamic / carry bars — per row, byte 0..39 (chg | ow):\n
set $r = 0
while $r < 4
  printf "      row %d chg: ", 43 + $r
  set $b = 0
  while $b < 40
    printf "%d ", g_bsChgPos[$r*40 + $b]
    set $b = $b + 1
  end
  printf "\n      row %d ow : ", 43 + $r
  set $b = 0
  while $b < 40
    printf "%d ", g_bsOwPos[$r*40 + $b]
    set $b = $b + 1
  end
  printf "\n"
  set $r = $r + 1
end
echo \n---- terrain_frame_setup LOOP 1 (45 cells/frame) ----\n
printf "frames=%lu cells=%lu\n", g_tfsFrames, g_tfsCells
printf "*** u/v RECURRENCE: ok=%lu BAD=%lu  (BAD must be 0 to carry u/v in registers)\n", \
  g_tfsRecurOK, g_tfsRecurBad
printf "draw_row table: dr00($B622)=%lu dr01($B64F)=%lu dr10($B5F5)=%lu dr11($B5C8)=%lu\n", \
  g_tfsDr[0], g_tfsDr[1], g_tfsDr[2], g_tfsDr[3]
printf "empty pattern at cell 0 (the dr00 seed case) = %lu\n", g_tfsY0Empty
printf "pattern mix: $80=%lu $80+$40=%lu $80+$20=%lu | $40=%lu $40+$20=%lu $40+$10=%lu | $20=%lu $10=%lu | empty=%lu\n", \
  g_tfsPat[0], g_tfsPat[1], g_tfsPat[2], g_tfsPat[3], g_tfsPat[4], g_tfsPat[5], \
  g_tfsPat[6], g_tfsPat[7], g_tfsPat[8]
printf "  (btst depth: $80*=1-2, $40*=2-4, $20=3, $10=4, empty=4 -> deep classes are the cost)\n"
printf "classify: visible$00=%lu behind$20=%lu behind$40=%lu offscreen$80=%lu | u_hi<0=%lu diff<0=%lu\n", \
  g_tfsCls[0], g_tfsCls[1], g_tfsCls[2], g_tfsCls[3], g_tfsUneg, g_tfsDiffNeg
detach
quit
