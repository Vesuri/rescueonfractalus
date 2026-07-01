# On-target differential verify for the hand-asm project_terrain_points twin
# (build: make VERIFY=1 PROBES=1).  Runs into steady flight, then reports the
# per-call asm-vs-C-oracle comparison tallies accumulated by
# project_terrain_points_core (rof_native.c, ROF_PROJECT_VERIFY path).
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 320
continue
printf "VERIFY fdCalls=%u vbi=%u projCalls=%lu mismatch=%lu firstBad=%lu\n", \
  g_fdCalls, g_vbiCount, g_projCalls, g_projMismatch, g_projFirstBad
printf "  PERF beam-ticks: asm=%lu C=%lu  (per-call asm=%lu C=%lu, lower=faster)\n", \
  g_projAsmTicks, g_projCTicks, \
  (g_projCalls? g_projAsmTicks/g_projCalls:0), (g_projCalls? g_projCTicks/g_projCalls:0)
detach
quit
