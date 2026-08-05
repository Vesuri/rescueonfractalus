# Flight-rasterizer STRUCTURAL shape dump.  Build: make clean && make -j4 PROBES=1 \
#   PROFILE_NORING=1 RASTER_C=1 SUBDIV_C=1 RAS_SHAPE=1   (the counters live in the C oracle,
# so RASTER_C=1 is mandatory; RAS_SHAPE is off by default so it can't inflate the asm-vs-C
# differential's beam-ticks).  Run: GDBSCRIPT=ras_shape.gdb ./diag_run.sh 100
#
# Reads out, per terrain_draw_frame half-frame: how many rasterize calls, how the phase-2
# work splits between far-bisects / two-column (fe) / one-column (ff) leaves, how often a
# DRAW actually plots (hidden-surface accept rate), and the two SPAN histograms that say
# which span values are worth specialising.  Baseline (2026-08-05, deep flight, 263 halves):
#   calls 6013 · far 84842 · draws 153150 · plots 56300 (36.8%) · fe 63172 · ff 26806
#   far-span: 3 -> 31.2%, 4 -> 16.5%  (=> ras_sp3 / ras_sp4 straight-line blocks)
set pagination off
set confirm off
continue
echo \n==== SIGINT ====\n
printf "frames(halves)=%lu rasterCalls=%lu subdivCalls=%lu iter=%u vbi=%u\n", \
  g_tdFrames, g_tdRasterCalls, g_tdSubdivCalls, g_iterCount, g_vbiCount
printf "far=%lu draws=%lu plots=%lu (accept=%lu%%) sat=%lu bail=%lu fe=%lu ff=%lu ph1adv=%lu ph1push=%lu\n", \
  g_tdRasBisect, g_tdRasDraw, g_tdPlots, (g_tdRasDraw? 100*g_tdPlots/g_tdRasDraw : 0), \
  g_rasSat, g_rasBail, g_rasFe, g_rasFf, g_rasPh1Adv, g_rasPh1Push
printf "dots=%lu  (%lu%% of draws, %lu%% of accepted) <- the ONLY draws that reach the plot code\n", \
  g_rasDots, (g_tdRasDraw? 100*g_rasDots/g_tdRasDraw : 0), (g_tdPlots? 100*g_rasDots/g_tdPlots : 0)
echo --- entry-span histogram (idx 1..8 exact, 9=9-12,10=13-16,11=17-24,12=25-32,13=33-64,14=65-128,15=129+) ---\n
set $i = 0
while $i < 16
  printf "%lu ", g_rasSpanHist[$i]
  set $i = $i + 1
end
printf "\n"
echo --- far-bisect span histogram (same buckets; 3+4 = what ras_sp3/ras_sp4 absorb) ---\n
set $i = 0
while $i < 16
  printf "%lu ", g_rasFarHist[$i]
  set $i = $i + 1
end
printf "\n"
echo --- whole-subtree occlusion culling: is a span-3/4 group ever fully hidden? ---\n
printf "sp3: groups=%lu  allRejected=%lu (%lu%%)  cheapCull=%lu (%lu%%)  edgeTrunc=%lu\n", \
  g_rasSp3Grp, g_rasSp3Occl, (g_rasSp3Grp? 100*g_rasSp3Occl/g_rasSp3Grp : 0), \
  g_rasSp3Cons[3], (g_rasSp3Grp? 100*g_rasSp3Cons[3]/g_rasSp3Grp : 0), g_rasSp3Edge
printf "     cheap-test early-out (cols passed before 1st failure) 0..3: %lu %lu %lu %lu\n", \
  g_rasSp3Cons[0], g_rasSp3Cons[1], g_rasSp3Cons[2], g_rasSp3Cons[3]
printf "sp4: groups=%lu  allRejected=%lu (%lu%%)  cheapCull=%lu (%lu%%)  edgeTrunc=%lu\n", \
  g_rasSp4Grp, g_rasSp4Occl, (g_rasSp4Grp? 100*g_rasSp4Occl/g_rasSp4Grp : 0), \
  g_rasSp4Cons[4], (g_rasSp4Grp? 100*g_rasSp4Cons[4]/g_rasSp4Grp : 0), g_rasSp4Edge
printf "     cheap-test early-out (cols passed before 1st failure) 0..4: %lu %lu %lu %lu %lu\n", \
  g_rasSp4Cons[0], g_rasSp4Cons[1], g_rasSp4Cons[2], g_rasSp4Cons[3], g_rasSp4Cons[4]
echo --- terrain_subdivide_column shape (helper mix per call; needs SUBDIV_C=1 too) ---\n
printf "calls=%lu (bail=%lu) mid=%lu (rough=%lu) inner=%lu pop=%lu ras=%lu skip=%lu\n", \
  g_sdCalls, g_sdBail, g_sdMid, g_sdRough, g_sdInner, g_sdPop, g_sdRas, g_sdSkip
printf "  ph2: adopt=%lu push=%lu (far already in regs: %lu)   ph3: farEsc=%lu steep=%lu\n", \
  g_sdP2Adopt, g_sdP2Push, g_sdP2Known, g_sdFarEsc, g_sdSteep
printf "  *** REDUNDANT far reloads: inner=%lu/%lu (%lu%%)  +ph2=%lu  => %lu of %lu total loads\n", \
  g_sdInnerFarKnown, g_sdInner, (g_sdInner? 100*g_sdInnerFarKnown/g_sdInner : 0), \
  g_sdP2Known, (g_sdInnerFarKnown+g_sdP2Known), (g_sdInner+g_sdP2Adopt+g_sdP2Push+g_sdPop)
echo --- PER-SEGMENT occlusion cull (one subdivide call from the object draw-order loop) ---\n
printf "segments=%lu  offscreen=%lu  drewNOTHING=%lu (%lu%%) <- the CEILING for any cull\n", \
  g_segCalls, g_segOffscr, g_segNoDraw, (g_segCalls? 100*g_segNoDraw/g_segCalls : 0)
printf "  SOUND bound max(ends)+W/2 : culls=%lu (%lu%%)  UNSOUND-fires=%lu (MUST be 0)\n", \
  g_segSound, (g_segCalls? 100*g_segSound/g_segCalls : 0), g_segSoundBad
printf "  NAIVE bound max(ends)     : culls=%lu (%lu%%)  UNSOUND-fires=%lu (proves the slack)\n", \
  g_segNaive, (g_segCalls? 100*g_segNaive/g_segCalls : 0), g_segNaiveBad
printf "  test cost: hits=%lu cols=%lu (%lu/hit)   misses=%lu cols=%lu (%lu/miss)\n", \
  g_segSound, g_segScanCull, (g_segSound? g_segScanCull/g_segSound : 0), \
  g_segMisses, g_segScanMiss, (g_segMisses? g_segScanMiss/g_segMisses : 0)
printf "  WORK REMOVED by the sound cull: draws=%lu/%lu (%lu%%)  rasterCalls=%lu/%lu (%lu%%)\n", \
  g_segDrawsCull, g_tdRasDraw, (g_tdRasDraw? 100*g_segDrawsCull/g_tdRasDraw : 0), \
  g_segRasCull, g_tdRasterCalls, (g_tdRasterCalls? 100*g_segRasCull/g_tdRasterCalls : 0)
echo --- SAME test one level down: cull a whole terrain_column_rasterize CALL ---\n
printf "rasCalls=%lu  acceptedNOTHING=%lu (%lu%%) <- ceiling   SOUND culls=%lu (%lu%%)  UNSOUND=%lu\n", \
  g_rcCalls, g_rcNoAccept, (g_rcCalls? 100*g_rcNoAccept/g_rcCalls : 0), \
  g_rcSound, (g_rcCalls? 100*g_rcSound/g_rcCalls : 0), g_rcSoundBad
printf "  cost: hits=%lu cols=%lu (%lu/hit)  misses=%lu cols=%lu (%lu/miss)   draws removed=%lu/%lu (%lu%%)\n", \
  g_rcSound, g_rcScanHit, (g_rcSound? g_rcScanHit/g_rcSound : 0), \
  g_rcMisses, g_rcScanMiss, (g_rcMisses? g_rcScanMiss/g_rcMisses : 0), \
  g_rcDrawsCull, g_tdRasDraw, (g_tdRasDraw? 100*g_rcDrawsCull/g_tdRasDraw : 0)
echo --- clipped segment width histogram (1..8 exact, 9=9-12,10=13-16,11=17-24,12=25-32,13=33-64,14=65-128,15=129+) ---\n
set $i = 0
while $i < 16
  printf "%lu ", g_segWidthHist[$i]
  set $i = $i + 1
end
printf "\n"
echo --- inner-loop depth histogram (0..15) ---\n
set $i = 0
while $i < 16
  printf "%lu ", g_sdDepthHist[$i]
  set $i = $i + 1
end
printf "\n"
detach
quit
