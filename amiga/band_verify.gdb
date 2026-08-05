# In-process differential for the cached windscreen-band composite + the box-narrowed object
# overlay (build: make clean && make -j4 BAND_VERIFY=1 PROBES=1 FIXED_RNG=1).
# Both counters MUST be 0: the band is composited both the cached and the original per-byte way and
# byte-compared over all 4 band rows x 3 planes, and the object scratch is swept for any nonzero
# byte left OUTSIDE the dirty box the narrowed apply walks.
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 300
continue
printf "BAND VERIFY: calls=%lu mismatch=%lu firstBadOffset=%lu | objLeak=%lu  (both must be 0)\n", \
  g_bandCalls, g_bandMismatch, g_bandFirstBad, g_objLeak
printf "  fdCalls=%u vbi=%u\n", g_fdCalls, g_vbiCount
detach
quit
