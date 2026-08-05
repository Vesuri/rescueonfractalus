# Why is PlatformAmiga::renderFrame's "wait next real VBI" spin costing time in flight?
# g_rCalls  = renderFrame invocations while the flight VBI ($4FF5) is installed
# g_fdCalls = renderFlightDirect calls that actually PAINTED (past the !g_flightTerrainFresh
#             early return) -> those set flightFlipDeferred and SKIP the spin.
# So (g_rCalls - g_fdCalls) is how many flight renderFrame calls fell through to the spin,
# and g_rIdleWall is the beam ticks they burned there.
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 200
continue
printf "fdCalls=%lu rCalls=%lu  spin-calls=%ld (%.1f%% of renderFrame)\n", \
  (unsigned long)g_fdCalls, g_rCalls, (long)g_rCalls-(long)g_fdCalls, \
  100.0*((double)g_rCalls-(double)g_fdCalls)/(double)g_rCalls
printf "idleWall=%lu ticks (%.1f%% of flight wall)  renderWall=%lu  compute=%lu\n", \
  g_rIdleWall, 100.0*(double)g_rIdleWall/((double)g_rIdleWall+(double)g_rRenderWall), \
  g_rRenderWall, g_rRenderCompute
printf "per spin-call: %.1f ticks\n", \
  ((long)g_rCalls>(long)g_fdCalls) ? (double)g_rIdleWall/((double)g_rCalls-(double)g_fdCalls) : 0.0
printf "vbi=%u fadeLoopVbi=%lu\n", g_vbiCount, (unsigned long)g_fadeLoopVbi
detach
quit
