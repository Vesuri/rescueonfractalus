# Flight profiler (use with a -DROF_AUTOFLIGHT build: flight starts on frame 1).
# Let ~30 flight frames accumulate, then read g_flightProf (a memory READ — works
# even though gdb data WRITES to the loaded hunks don't take effect).
set pagination off
set confirm off

break flight_frame_native
ignore 1 30
echo \n>>> accumulating ~30 flight frames...\n
continue

echo \n=== g_flightProf [terrain stateEnemy render copper frames updateTot renderTot isrLines isrCalls] ===\n
echo (terrain/stateEnemy/render/copper/updateTot/renderTot in VBI ticks ~20ms; isrLines in raster lines ~63.56us)\n
x/9wu &g_flightProf
quit
