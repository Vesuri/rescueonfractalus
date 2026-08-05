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
detach
quit
