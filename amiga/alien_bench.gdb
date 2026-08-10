# Alien-knock creature-draw bench — read-back for a `make ALIEN_BENCH=1 PROBES=1` build.
#
#   cd amiga && make clean && make -j4 ALIEN_BENCH=1 PROBES=1
#   . ./env.sh && GDBSCRIPT=alien_bench.gdb ./diag_run.sh 45
#
# Times ONE synthetic 43-row creature step (the shape alien_creature_animate_draw drives) from
# main-loop context at boot, so the knock can be A/B'd without flying to a rescue.
#
# Reference: a real user-flown knock at 1dea72e measured 3725 ticks/step for this work.
# If g_abTicks is in that neighbourhood the bench is representative; if it is wildly off, the
# seed no longer matches real play and the DELTA between builds is all it can be trusted for.
# 313 ticks = 1 PAL frame = 20 ms.  The faithful budget for a whole step is 1565 t (5 frames).
set pagination off
set confirm off
# diag_run.sh SIGINTs gdb after its delay to break out of this continue, then the prints run.
continue
printf "==== ALIEN CREATURE-DRAW BENCH ====\n"
printf "done=%u  rows=%lu  (want done=1 rows=43)\n", g_abDone, g_abRows
printf "ticks/step  = %lu  (ISR-corrected)   = %lu ms\n", g_abTicks, (g_abTicks * 20) / 313
printf "ticks/step  = %lu  (raw, incl. ISR)\n", g_abRaw
printf "ticks/row   = %lu\n", (g_abRows ? g_abTicks / g_abRows : 0)
printf "vs faithful 1565 t/step: %lu%%\n", (g_abTicks * 100) / 1565
printf "reference: 3725 t/step measured on a real knock at 1dea72e\n"
printf "===================================\n"
printf "-- where a row goes: ALIEN_BENCH_SPLIT builds only --\n"
printf "  clear=%lu  fills=%lu  loop=%lu  (total ticks over all 43 rows)\n", \
  g_abTClear, g_abTFills, g_abTLoop
printf "  per row: clear=%lu fills=%lu loop=%lu  | sum=%lu vs step %lu\n", \
  (g_abRows ? g_abTClear/g_abRows : 0), (g_abRows ? g_abTFills/g_abRows : 0), \
  (g_abRows ? g_abTLoop/g_abRows : 0), \
  (g_abRows ? (g_abTClear+g_abTFills+g_abTLoop)/g_abRows : 0), \
  (g_abRows ? g_abTicks/g_abRows : 0)
printf "  flush(pass2)=%lu total, %lu/row\n", g_abTFlush, (g_abRows ? g_abTFlush/g_abRows : 0)
