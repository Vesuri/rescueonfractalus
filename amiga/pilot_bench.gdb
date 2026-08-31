# Pilot-walk zoom draw bench.  Build:
#   make clean && make -j4 PILOT_BENCH=1 PROBES=1 PROFILE_NORING=1 FIXED_RNG=1
#   GDBSCRIPT=pilot_bench.gdb ./diag_run.sh 60
#
# Times one synthetic draw_scaled_shape per zoom step size from flight main-loop context
# (rof_pilot_bench, fires at flight iteration 40).  ticks = raster lines, ISR-subtracted:
# 313 t = 1 frame = 20 ms, so ms = t/15.73.  The zoom's own budget per animation step is the
# 4-RTCLOK-tick wait in animate_zoom_sequence = 4 frames = 1252 t = 80 ms at 50 Hz.
continue
echo \n==== SIGINT ====\n
printf "done=%u haveOverlay=%u  (haveOverlay must be 1 -- 0 means the mirror was skipped and every figure below is a floor)\n", \
  g_pbDone, g_pbHaveBmp
echo \n     step   rows x cols   plots     ticks      ms   %%of-4-frame-step   raw(t)  figRows  figCols\n
set $i = 0
while $i < 6
  set $st   = g_pbStep[$i]
  set $tk   = g_pbTicks[$i]
  set $rows = g_pbFigHi[$i] - g_pbFigLo[$i] + 1
  set $cols = g_pbFigCH[$i] - g_pbFigCL[$i] + 1
  printf "    $%04X  %3d rows  %2d cols  %8lu t  %6lu ms  %5lu %%      %6lu   %3d-%3d   %3d-%3d\n", \
    $st, $rows, $cols, $tk, ($tk*100)/1573, ($tk*100)/1252, g_pbRaw[$i], \
    g_pbFigLo[$i], g_pbFigHi[$i], g_pbFigCL[$i], g_pbFigCH[$i]
  set $i = $i + 1
end
printf "\nscaling: near/far draw cost = %lu / %lu = %lu.%02lux\n", \
  g_pbTicks[5], g_pbTicks[0], g_pbTicks[5]/g_pbTicks[0], ((g_pbTicks[5]*100)/g_pbTicks[0])%100
printf "\nper-plot: %lu calls in %lu t = %lu ns/plot (~%lu 68000 cycles at 7.09MHz)\n", \
  g_pbPlotN, g_pbPlotTicks, (g_pbPlotTicks*63560)/g_pbPlotN, (g_pbPlotTicks*451)/g_pbPlotN
printf "  => plots implied by the near draw: %lu t / (%lu t/plot) = ~%lu plots (analytic model says 284)\n", \
  g_pbTicks[5], g_pbPlotTicks/g_pbPlotN, (g_pbTicks[5]*g_pbPlotN)/g_pbPlotTicks
echo \n--- overlay fingerprint per step (the ONLY check on the Amiga-only ROF_PLOT_FIG arm) ---\n
set $i = 0
while $i < 6
  printf "  step $%04X  figHash=%lu\n", g_pbStep[$i], g_pbFigHash[$i]
  set $i = $i + 1
end
