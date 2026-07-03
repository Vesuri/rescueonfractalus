# On-target differential verify for the hand-asm fill_vertical_span_core twin
# (build: make VERIFY=1 PROBES=1; drive: GDBSCRIPT=framedraw_verify.gdb ./raster_diff.sh framedraw 60).
# fill_vertical_span_core is called many times during the standby->doors door-frame draw
# (display_setup stretch A, ~vbi 162-221); the differential in rof_native.c
# (ROF_FRAMEDRAW_VERIFY) tallies asm-vs-C-oracle mismatches over the $1000 door field.
# Break well after the draw has run and report the tallies.
set pagination off
set confirm off
set remotetimeout 90
tbreak PlatformAmiga::renderFrame if g_fvsCalls >= 200
continue
printf "VERIFY fvsCalls=%lu mismatch=%lu firstBad=%lu vbi=%u\n", \
  g_fvsCalls, g_fvsMismatch, g_fvsFirstBad, g_vbiCount
detach
quit
