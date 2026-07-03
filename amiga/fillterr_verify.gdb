# On-target differential verify for the hand-asm fill_terrain_columns twin
# (build: make VERIFY=1 PROBES=1; drive: GDBSCRIPT=fillterr_verify.gdb ./raster_diff.sh fillterr 60).
# fill_terrain_columns is a ONE-SHOT (the stars/planet field build at the tunnel->stars
# transition, ~vbi 877), so the differential in fill_terrain_columns_core (ROF_FILLTERR_VERIFY)
# fires exactly once; break the frame after it ran and report the asm-vs-C-oracle tallies.
set pagination off
set confirm off
set remotetimeout 90
tbreak PlatformAmiga::renderFrame if g_fillterrCalls >= 1
continue
printf "VERIFY fillterrCalls=%lu mismatch=%lu firstBad=%lu lfsrBad=%lu vbi=%u\n", \
  g_fillterrCalls, g_fillterrMismatch, g_fillterrFirstBad, g_fillterrLfsrBad, g_vbiCount
detach
quit
