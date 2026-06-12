# Steady-state flight profiler: skip the initial fade, then measure a window.
# Two snapshots of g_flightProf; steady-state per-frame = (B-A)/(framesB-framesA).
set pagination off
set confirm off
break flight_frame_native
ignore 1 60
echo \n>>> warmed up ~60 frames (past the fade); snapshot A...\n
continue
echo \n=== SNAPSHOT A [terrain stateEnemy render copper frames updateTot renderTot isrLines isrCalls tFrameSetup tClear tDraw tCollision] ===\n
x/13wu &g_flightProf
printf "tdSubdiv=%lu tdProjPlot=%lu\n", g_tdSubdiv, g_tdProjPlot
ignore 1 120
echo \n>>> +120 more frames; snapshot B...\n
continue
echo \n=== SNAPSHOT B [terrain stateEnemy render copper frames updateTot renderTot isrLines isrCalls tFrameSetup tClear tDraw tCollision] ===\n
x/13wu &g_flightProf
printf "tdSubdiv=%lu tdProjPlot=%lu\n", g_tdSubdiv, g_tdProjPlot
quit
