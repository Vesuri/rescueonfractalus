# COMBAT-LOAD verification + framerate, sampled in short segments.
#
# Answers, in one run, the two questions that must be settled BEFORE any combat timing is
# quoted:  (1) is there actually combat happening?  (2) how close did it get to the camera?
# A run whose g_clExplode / g_clObjNear stay 0 is the 2026-07-31 "firing into the void"
# mistake repeating itself — throw the numbers away and fix the setup first.
#
# Build: make clean && make -j4 COMBAT=1 PROBES=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=combat_probe.gdb ./diag_run.sh 220
#
# ⚠ This is a PROBES build, so its FPS is ~35% below the honest one — use it to verify the
# LOAD, then re-measure the rate with `make COMBAT=1 FPSCOUNT=1 FIXED_RNG=1` + fps_seg.gdb.
set pagination off
set confirm off

define cseg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  set $dv = (int)g_vbiCount - (int)$pv
  set $df = (int)g_fdCalls  - (int)$pf
  printf "vbi %5u +%4d  painted %4d  fd/vbi=0.%03d | expl %4u shotHit %4u | fire %5u impact %4u | saucer %3u | obj %5u near %4u | reseed %4u | VVBLKI=$%02x%02x $3D=%02x\n", \
    g_vbiCount, $dv, $df, ($df*1000)/$dv, \
    g_clExplode, g_clShotHit, g_clEnemyFire, g_clImpact, g_clSaucer, \
    g_clObjDraw, g_clObjNear, g_clReseed, \
    mem[0x223], mem[0x222], mem[0x3D]
  set $pv = (int)g_vbiCount
  set $pf = (int)g_fdCalls
end

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
set $pv = (int)g_vbiCount
set $pf = (int)g_fdCalls
printf "=== combat setup: level=%u  $0623 emplace-gate=%02x  $0621 saucer-period=%02x  $0624 fire-mask=%02x ===\n", \
  g_clLevel, g_cl0623, g_cl0621, g_cl0624
printf "=== start: vbi=%u VVBLKI=$%02x%02x ===\n", g_vbiCount, mem[0x223], mem[0x222]

cseg 2200
cseg 2500
cseg 2800
cseg 3100
cseg 3400
cseg 3700
cseg 4000
cseg 4300
cseg 4600
cseg 4900
cseg 5200
cseg 5500

printf "\n=== ground-object draw distance histogram ($0051 depth; 0 = closest/biggest, 12 = far) ===\n"
set $i = 0
while $i < 13
  printf "  depth %2d : %6u\n", $i, g_clObjDist[$i]
  set $i = $i + 1
end
printf "totals: explode=%u shotHit=%u enemyFire=%u impact=%u saucer=%u objDraw=%u objNear=%u reseed=%u\n", \
  g_clExplode, g_clShotHit, g_clEnemyFire, g_clImpact, g_clSaucer, g_clObjDraw, g_clObjNear, g_clReseed

# THE headline measurement: framerate split by combat state, WITHIN ONE RUN.  Cross-BUILD
# end-to-end is invalid here (a combat build and a quiet build fly different trajectories), but
# this split is one binary / one trajectory / one terrain, so the states ARE comparable.
printf "\n=== framerate by combat state (same run, same trajectory — this is the honest one) ===\n"
define cstate
  printf "  %-10s: %6lu vbi  %5lu painted  FPS=%2lu.%lu\n", $arg1, g_clVbi[$arg0], g_clFrm[$arg0], \
    (g_clVbi[$arg0] ? (50*g_clFrm[$arg0])/g_clVbi[$arg0] : 0), \
    (g_clVbi[$arg0] ? ((500*g_clFrm[$arg0])/g_clVbi[$arg0])%10 : 0)
end
cstate 0 "EXPLOSION"
cstate 1 "SAUCER"
cstate 2 "OUR SHOT"
cstate 3 "QUIET"

# The PERSISTENT load: framerate vs how many ground objects the frame actually rastered.
# "QUIET" above still has emplacements on screen — it only means no explosion is animating —
# so this is the split that prices objects-on-screen rather than the explosion animation.
printf "\n=== framerate by ground objects rastered per frame (same run) ===\n"
printf "  0 objects : %6lu vbi  %5lu painted  FPS=%2lu.%lu\n", g_clVbiObj[0], g_clFrmObj[0], \
  (g_clVbiObj[0] ? (50*g_clFrmObj[0])/g_clVbiObj[0] : 0), (g_clVbiObj[0] ? ((500*g_clFrmObj[0])/g_clVbiObj[0])%10 : 0)
printf "  1-2       : %6lu vbi  %5lu painted  FPS=%2lu.%lu\n", g_clVbiObj[1], g_clFrmObj[1], \
  (g_clVbiObj[1] ? (50*g_clFrmObj[1])/g_clVbiObj[1] : 0), (g_clVbiObj[1] ? ((500*g_clFrmObj[1])/g_clVbiObj[1])%10 : 0)
printf "  3-5       : %6lu vbi  %5lu painted  FPS=%2lu.%lu\n", g_clVbiObj[2], g_clFrmObj[2], \
  (g_clVbiObj[2] ? (50*g_clFrmObj[2])/g_clVbiObj[2] : 0), (g_clVbiObj[2] ? ((500*g_clFrmObj[2])/g_clVbiObj[2])%10 : 0)
printf "  6+        : %6lu vbi  %5lu painted  FPS=%2lu.%lu\n", g_clVbiObj[3], g_clFrmObj[3], \
  (g_clVbiObj[3] ? (50*g_clFrmObj[3])/g_clVbiObj[3] : 0), (g_clVbiObj[3] ? ((500*g_clFrmObj[3])/g_clVbiObj[3])%10 : 0)
detach
quit
