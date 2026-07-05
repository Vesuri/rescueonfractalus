# On-target differential verify for the hand-asm build_view_transform_matrix twin
# (build: make VERIFY=1 PROBES=1).  Runs into steady flight, then reports the
# per-call asm-vs-C-oracle comparison tallies accumulated by
# build_view_transform_matrix (rof_native.c, ROF_BUILDVIEW_VERIFY path).
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 320
continue
printf "VERIFY fdCalls=%u vbi=%u bvCalls=%lu mismatch=%lu firstBad=%lu badAddr=0x%lx\n", \
  g_fdCalls, g_vbiCount, g_bvCalls, g_bvMismatch, g_bvFirstBad, g_bvBadAddr
printf "  PERF beam-ticks: asm=%lu C=%lu  (per-call asm=%lu C=%lu, lower=faster)\n", \
  g_bvAsmTicks, g_bvCTicks, \
  (g_bvCalls? g_bvAsmTicks/g_bvCalls:0), (g_bvCalls? g_bvCTicks/g_bvCalls:0)
detach
quit
