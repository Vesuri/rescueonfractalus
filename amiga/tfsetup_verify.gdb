# On-target differential verify for the hand-asm terrain_frame_setup_core twin
# (build: make VERIFY=1 PROBES=1).  Runs into deep flight, then reports the per-call
# asm-vs-C-oracle comparison tallies from terrain_frame_setup_core (rof_native.c,
# ROF_TFSETUP_VERIFY path).  terrain_frame_setup has no rasterizer call, so its ticks
# are not raster-diluted.
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 320
continue
printf "VERIFY fdCalls=%u vbi=%u tfsCalls=%lu mismatch=%lu firstBad=%lu badAddr=0x%lx\n", \
  g_fdCalls, g_vbiCount, g_tfsCalls, g_tfsMismatch, g_tfsFirstBad, g_tfsBadAddr
printf "  PERF beam-ticks: asm=%lu C=%lu  (per-call asm=%lu C=%lu, lower=faster)\n", \
  g_tfsAsmTicks, g_tfsCTicks, \
  (g_tfsCalls? g_tfsAsmTicks/g_tfsCalls:0), (g_tfsCalls? g_tfsCTicks/g_tfsCalls:0)
detach
quit
