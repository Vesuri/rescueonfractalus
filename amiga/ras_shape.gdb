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
detach
quit
