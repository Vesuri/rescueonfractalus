# Default flight/init timing dump for diag_run.sh.  Requires an out/RoF.exe built with
# `make PROBES=1` (ROF_FLIGHT_PROBE) — the globals below only exist in that build.
# Edit freely to print whatever globals / mem[0xNNNN] a given investigation needs.
continue
echo \n==== SIGINT ====\n
printf "vbi=%u VVBLKI=%02x%02x joy=%u 060B=%02x\n", g_vbiCount, mem[0x0223], mem[0x0222], mem[0x004A], mem[0x060B]
echo --- flight control integ ($0027 pitch-rate: 00=level at neutral; $D0=nose-dive bug) ---\n
printf "pitch(28/29)=%02x%02x pitchRate(27)=%02x roll(25/26)=%02x%02x thr(2D/2E)=%02x%02x alt(34)=%02x 005D=%02x D300=%02x\n", \
  mem[0x0028], mem[0x0029], mem[0x0027], mem[0x0025], mem[0x0026], mem[0x002D], mem[0x002E], mem[0x0034], mem[0x005D], mem[0xD300]
echo --- flight init (sub-frame ticks; 313/frame) ---\n
printf "flightVbi=%u dispSetup=%lu gameInit=%lu intro=%lu rowAddr=%lu initTotal=%lu\n", \
  g_probeFlightVbi, g_probeDispSetup, g_probeGameInit, g_probeIntro, g_probeRowAddr, g_probeInitTotal
echo --- main-loop per-iteration gap (sub-frame ticks) ---\n
printf "iterCount=%u iterLast=%lu iterMax=%lu iterMaxAt=%u\n", g_iterCount, g_iterLast, g_iterMax, g_iterMaxAt
echo --- flight phase split (accumulated sub-frame ticks) ---\n
printf "setup=%lu clear=%lu draw=%lu coll=%lu state=%lu enemy=%lu\n", \
  g_fSetup, g_fClear, g_fDraw, g_fColl, g_fState, g_fEnemy
echo --- terrain_draw_frame sub-phase (ROF_TDRAW_PROF) ---\n
printf "tdSubdiv=%lu tdProjPlot=%lu\n", g_tdSubdiv, g_tdProjPlot
echo --- renderFrame no-yield gap (real VBI frames) ---\n
printf "maxGap=%u atVbi=%u vvblki=%04x 060B=%02x 004A=%02x\n", \
  g_maxRenderGap, g_maxGapAtVbi, g_maxGapVvblki, g_maxGap060B, g_maxGap004A
echo --- atmosphere terrain-pen range (salmon->brown fade) ---\n
printf "dc=%u..%u dd=%u..%u\n", g_dcMin, g_dcMax, g_ddMin, g_ddMax
echo --- flight-transition spin-wait diagnostics ---\n
printf "maxPollSpin=%u frames (at vbi %u)  rtDouble=%u (at %u)  rtZero=%u (at %u)  torn=%u last=%04x\n", \
  g_maxPollSpinFrames, g_maxPollSpinAtVbi, g_rtDoubleCount, g_rtDoubleAtVbi, g_rtZeroCount, g_rtZeroAtVbi, \
  g_rtTornCount, g_rtLastTornVec
echo --- RTCLOK skip ($4FF5 renderFrame delta; >1 enables equality-wait wrap) ---\n
printf "rtJumpMax=%u rtJumpGt1Count=%u (at vbi %u)\n", g_rtJumpMax, g_rtJumpGt1Count, g_rtJumpAtVbi
detach
quit
