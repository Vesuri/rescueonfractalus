# SIZING companion to shape_probe.gdb: what each candidate is actually WORTH.  A shape probe says
# how much of a loop is redundant; it cannot say what fraction of the frame that loop is.  This
# reads the per-call beam-ticks of the three in-process differentials (rasterizer, edge plot,
# terrain_frame_setup) plus the throughput denominator (iterations per VBI frame), so a candidate
# can be sized BEFORE any asm is written.  See the "shape-probe the DENOMINATOR" lesson.
# Build: make clean && make -j4 VERIFY=1 PROBES=1 FIXED_RNG=1
# Run:   GDBSCRIPT=size_probe.gdb ./diag_run.sh 150
# NB a VERIFY build runs asm AND the C oracle on every call, so it is ~2x slower per frame —
# the per-call ticks are the usable number here, not the frame rate.
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 150
continue
printf "fdCalls=%u vbi=%u iter=%u\n", g_fdCalls, g_vbiCount, g_iterCount
printf "  iterations/VBI-frame = %lu/1000  -> ticks per iteration ~ %lu (313 ticks/frame)\n", \
  (g_vbiCount? 1000*g_iterCount/g_vbiCount : 0), \
  (g_iterCount? 313*g_vbiCount/g_iterCount : 0)
printf "RASTER calls=%lu mismatch=%lu  per-call asm=%lu C=%lu ticks\n", \
  g_rasterCalls, g_rasterMismatch, \
  (g_rasterCalls? g_rasAsmTicks/g_rasterCalls:0), (g_rasterCalls? g_rasCTicks/g_rasterCalls:0)
printf "  -> rasterizer TOTAL asm ticks = %lu  (calls x per-call)\n", g_rasAsmTicks
printf "EDGE   calls=%lu mismatch=%lu  per-call asm=%lu C=%lu ticks\n", \
  g_edgeCalls, g_edgeMismatch, \
  (g_edgeCalls? g_edgeAsmTicks/g_edgeCalls:0), (g_edgeCalls? g_edgeCTicks/g_edgeCalls:0)
printf "  -> edge plot TOTAL asm ticks = %lu\n", g_edgeAsmTicks
printf "TFS    calls=%lu mismatch=%lu  per-call asm=%lu C=%lu ticks\n", \
  g_tfsCalls, g_tfsMismatch, \
  (g_tfsCalls? g_tfsAsmTicks/g_tfsCalls:0), (g_tfsCalls? g_tfsCTicks/g_tfsCalls:0)
printf "  -> terrain_frame_setup TOTAL asm ticks = %lu\n", g_tfsAsmTicks
printf "SHARE denominators: calls per iteration -> raster=%lu edge=%lu tfs=%lu\n", \
  (g_iterCount? g_rasterCalls/g_iterCount:0), (g_iterCount? g_edgeCalls/g_iterCount:0), \
  (g_iterCount? g_tfsCalls/g_iterCount:0)
detach
quit
