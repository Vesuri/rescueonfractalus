# renderFlightDirect internal split (build: make PROBES=1 PROFILE_NORING=1 FIXED_RNG=1) plus the
# throughput denominator, so each part can be expressed as a % of flight.  Companion to
# shape_probe.gdb / size_probe.gdb.
set pagination off
set confirm off
continue
echo \n==== SIGINT ====\n
printf "fdCalls=%lu iter=%u vbi=%u\n", g_fdCalls, g_iterCount, g_vbiCount
printf "DENOM: iterations/VBI-frame=%lu/1000 -> ticks/iteration=%lu ; fdCalls/iteration=%lu/10\n", \
  (g_vbiCount? 1000*g_iterCount/g_vbiCount : 0), (g_iterCount? 313*g_vbiCount/g_iterCount : 0), \
  (g_iterCount? 10*g_fdCalls/g_iterCount : 0)
printf "per-call ticks:  clear=%lu edge=%lu fill=%lu scan=%lu band=%lu   SUM=%lu\n", \
  (g_fdCalls? g_fdClear/g_fdCalls:0), (g_fdCalls? g_fdEdge/g_fdCalls:0), \
  (g_fdCalls? g_fdFill/g_fdCalls:0),  (g_fdCalls? g_fdScan/g_fdCalls:0), \
  (g_fdCalls? g_fdBand/g_fdCalls:0), \
  (g_fdCalls? (g_fdClear+g_fdEdge+g_fdFill+g_fdScan+g_fdBand)/g_fdCalls:0)
printf "raw totals:      clear=%lu edge=%lu fill=%lu scan=%lu band=%lu\n", \
  g_fdClear, g_fdEdge, g_fdFill, g_fdScan, g_fdBand
detach
quit
