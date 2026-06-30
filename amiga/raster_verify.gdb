# On-target differential verify for the hand-asm rasterizer (build: make VERIFY=1 PROBES=1).
# Runs into steady flight, then reports the per-call asm-vs-C-oracle comparison tallies
# accumulated by terrain_column_rasterize_core (rof_native.c, ROF_RASTERIZE_VERIFY path).
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 320
continue
printf "VERIFY fdCalls=%u vbi=%u rasterCalls=%lu mismatch=%lu firstBad=%lu\n", \
  g_fdCalls, g_vbiCount, g_rasterCalls, g_rasterMismatch, g_rasterFirstBad
printf "  PERF beam-ticks: asm=%lu C=%lu  (per-call asm=%lu C=%lu, lower=faster)\n", \
  g_rasAsmTicks, g_rasCTicks, \
  (g_rasterCalls? g_rasAsmTicks/g_rasterCalls:0), (g_rasterCalls? g_rasCTicks/g_rasterCalls:0)
detach
quit
