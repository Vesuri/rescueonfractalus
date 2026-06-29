# Default flight/init timing dump for diag_run.sh.  Requires an out/RoF.exe built with
# `make PROBES=1` (ROF_FLIGHT_PROBE) — the globals below only exist in that build.
# Edit freely to print whatever globals / mem[0xNNNN] a given investigation needs.
continue
echo \n==== SIGINT ====\n
printf "vbi=%u VVBLKI=%02x%02x joy=%u 060B=%02x\n", g_vbiCount, mem[0x0223], mem[0x0222], mem[0x004A], mem[0x060B]
printf "reveal=%u D01F=%02x\n", g_standbyRevealReady, mem[0xD01F]
echo --- Title Screen probe (rsTitle = VVBLKI==53cc && 365B==72) ---\n
printf "365B=%02x (R=72?) 365C=%02x COLPF0-3=%02x %02x %02x %02x\n", mem[0x365B], mem[0x365C], mem[0x02C4], mem[0x02C5], mem[0x02C6], mem[0x02C7]
echo --- flight control integ ($0027 pitch-rate: 00=level at neutral; $D0=nose-dive bug) ---\n
printf "pitch(28/29)=%02x%02x pitchRate(27)=%02x roll(25/26)=%02x%02x thr(2D/2E)=%02x%02x alt(34)=%02x 005D=%02x D300=%02x\n", \
  mem[0x0028], mem[0x0029], mem[0x0027], mem[0x0025], mem[0x0026], mem[0x002D], mem[0x002E], mem[0x0034], mem[0x005D], mem[0xD300]
printf "throttle-feeders: 0021=%02x 0022=%02x 0072=%02x 0676=%02x | dial 006F=%02x 0625=%02x (idx=%02x)\n", \
  mem[0x0021], mem[0x0022], mem[0x0072], mem[0x0676], mem[0x006F], mem[0x0625], (mem[0x006F]+mem[0x0625])&0xff
printf "event-seq: 003C=%02x 003D=%02x 003E=%02x 0043=%02x 063B=%02x 0634=%02x\n", \
  mem[0x003C], mem[0x003D], mem[0x003E], mem[0x0043], mem[0x063B], mem[0x0634]
echo --- flight init (sub-frame ticks; 313/frame) ---\n
printf "flightVbi=%u dispSetup=%lu gameInit=%lu intro=%lu rowAddr=%lu initTotal=%lu\n", \
  g_probeFlightVbi, g_probeDispSetup, g_probeGameInit, g_probeIntro, g_probeRowAddr, g_probeInitTotal
echo --- main-loop per-iteration gap (sub-frame ticks) ---\n
printf "iterCount=%u iterLast=%lu iterMax=%lu iterMaxAt=%u\n", g_iterCount, g_iterLast, g_iterMax, g_iterMaxAt
printf "fdCalls=%lu (renderFlightDirect converts) -> per-iteration = fdCalls/iterCount\n", g_fdCalls
echo --- flight phase split (accumulated sub-frame ticks; 313 ticks=1 frame=20ms) ---\n
printf "setup=%lu clear=%lu draw=%lu coll=%lu state=%lu enemy=%lu\n", \
  g_fSetup, g_fClear, g_fDraw, g_fColl, g_fState, g_fEnemy
echo --- g_flightProf (RTCLOK ticks=20ms; per-frame = field/frames) ---\n
printf "frames=%lu terrain=%lu tDraw=%lu stateEnemy=%lu render=%lu copper=%lu renderTot=%lu updateTot=%lu isrLines=%lu isrCalls=%lu\n", \
  g_flightProf.frames, g_flightProf.terrain, g_flightProf.tDraw, g_flightProf.stateEnemy, g_flightProf.render, g_flightProf.copper, g_flightProf.renderTot, g_flightProf.updateTot, g_flightProf.isrLines, g_flightProf.isrCalls
echo --- VBI body sub-profile (beam ticks; per-ISR-call = total/isrCalls) ---\n
printf "per-call: proj=%lu integ=%lu sfx=%lu isr=%lu\n", \
  (g_flightProf.isrCalls? g_pProj/g_flightProf.isrCalls:0), \
  (g_flightProf.isrCalls? g_pInteg/g_flightProf.isrCalls:0), \
  (g_flightProf.isrCalls? g_pSfx/g_flightProf.isrCalls:0), \
  (g_flightProf.isrCalls? g_flightProf.isrLines/g_flightProf.isrCalls:0)
echo --- cockpit decode (beam ticks; 313=20ms) ---\n
printf "cockpitTicks=%lu cockpitScans=%lu (per-scan ticks=%lu)\n", g_fCockpit, g_fCockpitScans, (g_fCockpitScans? g_fCockpit/g_fCockpitScans : 0)
echo --- terrain_draw_frame sub-phase (ROF_TDRAW_PROF; cumulative — normalize by frames) ---\n
printf "tdSubdiv=%lu tdProjPlot=%lu tdFrames=%lu\n", g_tdSubdiv, g_tdProjPlot, g_tdFrames
printf "  per-frame: tdSubdiv=%lu tdProjPlot=%lu (beam ticks/frame)\n", \
  (g_tdFrames? g_tdSubdiv/g_tdFrames : 0), (g_tdFrames? g_tdProjPlot/g_tdFrames : 0)
printf "  subdiv split/frame: rasterize=%lu recursion=%lu (tdSubdiv - tdRaster)\n", \
  (g_tdFrames? g_tdRaster/g_tdFrames : 0), (g_tdFrames? (g_tdSubdiv-g_tdRaster)/g_tdFrames : 0)
printf "  shape/frame: midpoints=%lu plots=%lu rasterCalls=%lu subdivCalls=%lu\n", \
  (g_tdFrames? g_tdMidpoints/g_tdFrames:0), (g_tdFrames? g_tdPlots/g_tdFrames:0), \
  (g_tdFrames? g_tdRasterCalls/g_tdFrames:0), (g_tdFrames? g_tdSubdivCalls/g_tdFrames:0)
printf "  raster split/frame: bisectSteps=%lu drawAttempts=%lu actualPlots=%lu (per call: bisect=%lu draw=%lu)\n", \
  (g_tdFrames? g_tdRasBisect/g_tdFrames:0), (g_tdFrames? g_tdRasDraw/g_tdFrames:0), (g_tdFrames? g_tdPlots/g_tdFrames:0), \
  (g_tdRasterCalls? g_tdRasBisect/g_tdRasterCalls:0), (g_tdRasterCalls? g_tdRasDraw/g_tdRasterCalls:0)
printf "fConvert=%lu (flight mem[1070]->bitplane convert; same units as draw=%lu)\n", g_fConvert, g_fDraw
printf "fDirect=%lu (Stage-1 direct $260E->bitplane render; cmp vs fConvert)\n", g_fDirect
printf "  fDirect breakdown/call (ticks): clear=%lu edge=%lu fill=%lu scan=%lu band=%lu\n", \
  (g_fdCalls? g_fdClear/g_fdCalls:0), (g_fdCalls? g_fdEdge/g_fdCalls:0), (g_fdCalls? g_fdFill/g_fdCalls:0), \
  (g_fdCalls? g_fdScan/g_fdCalls:0), (g_fdCalls? g_fdBand/g_fdCalls:0)
echo --- energy strip $0D98..$0DD0 (57 bytes; is it a solid contiguous run = rectangle?) ---\n
printf "062F(fuel)=%02x | energyAddr=%08x energy SV=%02x EV=%02x\n", mem[0x062F], g_energySprAddr, *(unsigned char*)g_energySprAddr, *(unsigned char*)(g_energySprAddr+2)
set $i = 0
while $i < 57
  printf "%02x ", mem[0x0D98 + $i]
  set $i = $i + 1
end
printf "\n"
echo --- altimeter sprites ($281A/$281B bar tops; sprite hdr SV/EV bytes) ---\n
printf "281A=%02x 281B=%02x | altimAddr=%08x shipAddr=%08x\n", mem[0x281A], mem[0x281B], g_altimSprAddr, g_altimShipSprAddr
printf "altim  hdr: SV=%02x EV=%02x ctl=%02x | ship hdr: SV=%02x EV=%02x ctl=%02x\n", \
  *(unsigned char*)g_altimSprAddr, *(unsigned char*)(g_altimSprAddr+2), *(unsigned char*)(g_altimSprAddr+3), \
  *(unsigned char*)g_altimShipSprAddr, *(unsigned char*)(g_altimShipSprAddr+2), *(unsigned char*)(g_altimShipSprAddr+3)
echo --- render/glue gap: ds_frame() per flight renderFrame call (beam ticks; 313=20ms) ---\n
printf "rCalls=%lu renderWall=%lu renderCompute=%lu idleWall=%lu\n", \
  g_rCalls, g_rRenderWall, g_rRenderCompute, g_rIdleWall
printf "  per-call: renderWall=%lu renderCompute=%lu idleWall=%lu\n", \
  (g_rCalls? g_rRenderWall/g_rCalls:0), (g_rCalls? g_rRenderCompute/g_rCalls:0), (g_rCalls? g_rIdleWall/g_rCalls:0)
printf "  compute split/call: perFrame=%lu renderFn=%lu copper=%lu (renderFn incl fDirect+cockpit)\n", \
  (g_rCalls? g_rPerFrame/g_rCalls:0), (g_rCalls? g_rRenderFn/g_rCalls:0), (g_rCalls? g_rCopper/g_rCalls:0)
echo --- renderFrame no-yield gap (real VBI frames) ---\n
printf "maxGap=%u atVbi=%u vvblki=%04x 060B=%02x 004A=%02x\n", \
  g_maxRenderGap, g_maxGapAtVbi, g_maxGapVvblki, g_maxGap060B, g_maxGap004A
echo --- flight VBI ZP write-set (firings; cells $00-$FF the VBI changed) ---\n
printf "vbiZpFirings=%lu\n", g_vbiZpFirings
set $i = 0
while $i < 256
  if g_vbiZpTouched[$i] != 0
    printf "%02x ", $i
  end
  set $i = $i + 1
end
printf "\n"
echo --- rasterizer ZP cells: 1=VBI-WRITES-IT(unsafe to alias) 0=safe ---\n
printf "60=%d 80=%d 81=%d 82=%d 83=%d 84=%d 85=%d 86=%d 8D=%d 8E=%d 8F=%d 90=%d 91=%d 95=%d 96=%d 9F=%d B5=%d B6=%d EA=%d EB=%d F4=%d F5=%d\n", \
  g_vbiZpTouched[0x60], g_vbiZpTouched[0x80], g_vbiZpTouched[0x81], g_vbiZpTouched[0x82], g_vbiZpTouched[0x83], \
  g_vbiZpTouched[0x84], g_vbiZpTouched[0x85], g_vbiZpTouched[0x86], g_vbiZpTouched[0x8D], g_vbiZpTouched[0x8E], \
  g_vbiZpTouched[0x8F], g_vbiZpTouched[0x90], g_vbiZpTouched[0x91], g_vbiZpTouched[0x95], g_vbiZpTouched[0x96], \
  g_vbiZpTouched[0x9F], g_vbiZpTouched[0xB5], g_vbiZpTouched[0xB6], g_vbiZpTouched[0xEA], g_vbiZpTouched[0xEB], \
  g_vbiZpTouched[0xF4], g_vbiZpTouched[0xF5]
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
