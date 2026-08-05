# On-target differential verify for the hand-asm rasterizer (build: make VERIFY=1 PROBES=1).
# Runs into steady flight, then reports the per-call asm-vs-C-oracle comparison tallies
# accumulated by terrain_column_rasterize_core (rof_native.c, ROF_RASTERIZE_VERIFY path).
set pagination off
set confirm off
# 150, not 320: a VERIFY build runs the asm twin AND the C oracle on every call, so it is
# ~2x slower per frame — while the auto-flight's crash is driven by the 50 Hz VBI and so
# happens at the same WALL-CLOCK time.  A 320 threshold is simply never reached (the run
# ends in the post-crash title card with the tallies unprinted).  150 fdCalls is still
# ~3000 rasterizer calls.  raster_diff.sh also prints the tallies after its SIGINT, so an
# unreached breakpoint degrades to "fewer calls" rather than "no data".
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 150
continue
printf "VERIFY fdCalls=%u vbi=%u rasterCalls=%lu mismatch=%lu firstBad=%lu\n", \
  g_fdCalls, g_vbiCount, g_rasterCalls, g_rasterMismatch, g_rasterFirstBad
printf "  PERF beam-ticks: asm=%lu C=%lu  (per-call asm=%lu C=%lu, lower=faster)\n", \
  g_rasAsmTicks, g_rasCTicks, \
  (g_rasterCalls? g_rasAsmTicks/g_rasterCalls:0), (g_rasterCalls? g_rasCTicks/g_rasterCalls:0)
printf "EDGE calls=%lu mismatch=%lu  ticks asm=%lu C=%lu (per-call asm=%lu C=%lu)\n", \
  g_edgeCalls, g_edgeMismatch, g_edgeAsmTicks, g_edgeCTicks, \
  (g_edgeCalls? g_edgeAsmTicks/g_edgeCalls:0), (g_edgeCalls? g_edgeCTicks/g_edgeCalls:0)
detach
quit
