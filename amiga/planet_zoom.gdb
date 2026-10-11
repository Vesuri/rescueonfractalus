# planet_zoom.gdb -- cost of the planet-approach zoom (scene 6), per cinematic step.
#
# paintPlanetNative is called exactly once per zoom step (from advance_object_positions, the only
# writer of the planet marker bars), so "vblanks between consecutive calls" IS the zoom's frame
# time: the step itself is fixed work, everything else in the delta is the renderer.  Compare the
# figure across builds -- including against a non-ENHANCED_TERRAIN build, whose 2x2 decode of the
# same bars is the faithful baseline the enhanced painter has to stay near.
# ⚠ Do NOT break on advance_object_positions: GCC inlines it in some builds, and gdb then plants
# the breakpoint on a cold out-of-line copy that never executes (silent 0-hit run).
#
# Build: make clean && make -j4 PROBES=1 STANDALONE_DATA=1 ROM=../rof.rom ENHANCED_TERRAIN=1
# Run:   . ./env.sh && GDBSCRIPT=planet_zoom.gdb ./diag_run.sh 90
set width 0
break RescueOnFractalus::paintPlanetNative
continue

set $v0 = 'PlatformAmiga.cpp'::g_vbiCount
set $pv = $v0
set $n = 1
set $worst = 0
printf "PLANET-ZOOM entry: vbi=%u  enhanced=%d\n", $v0, g_flightEnhancedTerrain
commands 1
silent
set $n = $n + 1
set $d = 'PlatformAmiga.cpp'::g_vbiCount - $pv
if $d > $worst
  set $worst = $d
end
printf "  step %u at vbi=%u (+%u vbl)  dist0=%02x%02x adv=%02x\n", $n, 'PlatformAmiga.cpp'::g_vbiCount, $d, mem[0x08A5], mem[0x08A4], mem[0x08D3]
set $pv = 'PlatformAmiga.cpp'::g_vbiCount
continue
end
continue

printf "PLANET-ZOOM: %u steps over %u vblanks = %u.%u vbl/step (worst %u)\n", \
  $n, ($pv - $v0), ($pv - $v0) / $n, ((($pv - $v0) * 10) / $n) % 10, $worst
detach
quit
