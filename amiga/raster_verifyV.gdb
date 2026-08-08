# VBI-PINNED variant of raster_verify.gdb (2026-08-08).  Use this one for any A/B of two
# BUILDS, never the fdCalls variant: raster_verify.gdb's `g_fdCalls >= 150` breakpoint is never
# reached in a VERIFY build, so its window is defined by raster_diff.sh's wall-clock SIGINT --
# a faster build then lands DEEPER into the flight, on different terrain.  That matters because
# the asm arm has special-cased blocks per span and the C oracle does not, so the asm/C ratio is
# a function of the span mix: two windows 30 vbi apart moved the ratio by ~2%.  The sim advances
# with the VBI (not with painted frames), so pinning g_vbiCount pins the camera path and both
# builds see the SAME terrain -- measured 1598 rasterCalls in both arms, vs 1598/1522 when
# pinning fdCalls instead.
#
# WARNING: pinning the window does NOT make the PERF column usable.  Calibrated 2026-08-08 by
# adding two nops to DRAWDOT (a known +190 cycles/call): the instrument reported -0.6%, i.e.
# a slowdown read as a speed-up.  A ~12-tick bracket against an ~80-tick ISR is the Phase-7b/8
# regime where FP_TIME's credited-at-exit g_isrBeamLines subtraction dominates.  Below roughly
# 500 cycles/call this is a CORRECTNESS instrument only -- size the change off the disassembly.
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
tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 14400
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
