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
# Read the knobs straight out of mem[] rather than from the g_cl* copies, so this line also works
# on a build WITHOUT COMBAT=1 — that is how the "what level does the auto-launch actually fly"
# question gets settled by measurement instead of by reading the boot code.
printf "=== level=%u ($006D)  $0623 emplace-gate=%02x  $0621 saucer-period=%02x  $0624 fire-mask=%02x ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], mem[0x624]
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
# ── PER-PHASE FRAME DECOMPOSITION (the attribution) ─────────────────────────────────────────
# ticks = raster scanlines, 63.56 us each; 313 = one PAL frame = 20 ms.  ISR-subtracted, so a
# phase excludes VBI firings inside its window.  BOTH terrain passes are bracketed (they were
# not before 2026-08-06) and ds_frame is bracketed, so PHASE SUM should ~= the whole iteration.
# state 0 = an explosion/bolt was live for that iteration, 1 = not.  Same binary, same run.
define phase
  set $t0 = g_clPh[$arg0][0]
  set $t1 = g_clPh[$arg0][1]
  printf "  %-22s expl %6lu t (%4lu t/it, %2lu.%01lu ms)   noexpl %6lu t (%4lu t/it, %2lu.%01lu ms)\n", \
    $arg1, \
    $t0, (g_clPhIter[0] ? $t0/g_clPhIter[0] : 0), \
    (g_clPhIter[0] ? ($t0*20)/(g_clPhIter[0]*313) : 0), (g_clPhIter[0] ? (($t0*200)/(g_clPhIter[0]*313))%10 : 0), \
    $t1, (g_clPhIter[1] ? $t1/g_clPhIter[1] : 0), \
    (g_clPhIter[1] ? ($t1*20)/(g_clPhIter[1]*313) : 0), (g_clPhIter[1] ? (($t1*200)/(g_clPhIter[1]*313))%10 : 0)
end
printf "\n=== per-phase frame decomposition, ticks/iteration (1 tick = 63.56us, 313 = 20ms) ===\n"
printf "  iterations: explosion=%lu  no-explosion=%lu\n", g_clPhIter[0], g_clPhIter[1]
phase 0 "SETUP  (frame_setup x2)"
phase 1 "CLEAR  (clear_col x2)"
phase 2 "DRAW   (terrain+obj x2)"
phase 3 "BOLT   (game_state_update)"
phase 4 "ENEMY  (enemy_check)"
phase 5 "FRAME  (ds_frame x2)"
set $s0 = g_clPh[0][0]+g_clPh[1][0]+g_clPh[2][0]+g_clPh[3][0]+g_clPh[4][0]+g_clPh[5][0]
set $s1 = g_clPh[0][1]+g_clPh[1][1]+g_clPh[2][1]+g_clPh[3][1]+g_clPh[4][1]+g_clPh[5][1]
printf "  %-22s expl %6lu t (%4lu t/it, %2lu.%01lu ms)   noexpl %6lu t (%4lu t/it, %2lu.%01lu ms)\n", \
  "SUM", \
  $s0, (g_clPhIter[0] ? $s0/g_clPhIter[0] : 0), \
  (g_clPhIter[0] ? ($s0*20)/(g_clPhIter[0]*313) : 0), (g_clPhIter[0] ? (($s0*200)/(g_clPhIter[0]*313))%10 : 0), \
  $s1, (g_clPhIter[1] ? $s1/g_clPhIter[1] : 0), \
  (g_clPhIter[1] ? ($s1*20)/(g_clPhIter[1]*313) : 0), (g_clPhIter[1] ? (($s1*200)/(g_clPhIter[1]*313))%10 : 0)
printf "  (an iteration paints 2 terrain frames, so ms/painted-frame = ms/it / 2)\n"
# CLOSE THE BUDGET.  wall = ISR + sum(phases) + unbracketed.  Without this the decomposition
# cannot be trusted: the first version of this probe covered under HALF the iteration and looked
# perfectly plausible.  Quote a phase share only when `covered` below is near 100%.
set $it = g_clPhIter[0] + g_clPhIter[1]
set $wall = g_clIterWall[0] + g_clIterWall[1]
set $sum  = $s0 + $s1
printf "\n=== budget check (all states, %lu iterations) ===\n", $it
printf "  wall           %8lu t  (%4lu t/it, %3lu.%01lu ms/it)\n", $wall, \
  ($it ? $wall/$it : 0), ($it ? ($wall*20)/($it*313) : 0), ($it ? (($wall*200)/($it*313))%10 : 0)
# NB the denominator is g_flightProf.isrCalls (FLIGHT VBI firings), not g_vbiCount — the latter
# counts every vblank since power-on, including the ~1900 of boot/cinematic, and dividing by it
# understated the per-firing cost by ~30%.
printf "  VBI ISR        %8lu t  (%4lu t/it)  = %2lu%% of wall   [%lu firings, %lu t each]\n", \
  g_isrBeamLines, ($it ? g_isrBeamLines/$it : 0), \
  ($wall ? (100*g_isrBeamLines)/$wall : 0), g_flightProf.isrCalls, \
  (g_flightProf.isrCalls ? g_isrBeamLines/g_flightProf.isrCalls : 0)
printf "  phases (sum)   %8lu t  (%4lu t/it)  = %2lu%% of wall\n", $sum, \
  ($it ? $sum/$it : 0), ($wall ? (100*$sum)/$wall : 0)
# Clamp: phases+ISR can exceed wall by a hair because the wall sample is taken at the iteration
# TOP while an ISR firing can straddle that boundary.  Printing the raw signed-negative remainder
# as unsigned produced a nonsense 11-digit "unbracketed" figure.
set $acct = $sum + g_isrBeamLines
printf "  covered        %2lu%% of wall", ($wall ? (100*$acct)/$wall : 0)
if $acct < $wall
  printf "  -> unbracketed %lu t/it\n", ($it ? ($wall - $acct)/$it : 0)
else
  printf "  -> fully accounted (overlap %lu t/it at iteration boundaries)\n", \
    ($it ? ($acct - $wall)/$it : 0)
end
# ⚠ g_isrBeamLines above is the FULL probe-ISR span and deliberately INCLUDES the 2x256-iteration
# ZP write-set audit (~70 lines/firing of pure probe overhead) so the main-loop buckets come out
# right.  The REAL flight-VBI handler cost is g_flightProf.isrLines / isrCalls — that is the
# number to quote, and the only one that means anything for the shipping build.
printf "\n=== flight VBI handler, REAL cost (ZP-audit probe excluded) ===\n"
printf "  all states : %lu firings, %lu t total, %lu t/firing (%lu%% of a 313-t PAL frame)\n", \
  g_flightProf.isrCalls, g_flightProf.isrLines, \
  (g_flightProf.isrCalls ? g_flightProf.isrLines/g_flightProf.isrCalls : 0), \
  (g_flightProf.isrCalls ? (100*g_flightProf.isrLines)/(g_flightProf.isrCalls*313) : 0)
printf "  EXPLOSION  : %lu firings, %lu t/firing\n", g_clIsrN[0], \
  (g_clIsrN[0] ? g_clIsr[0]/g_clIsrN[0] : 0)
printf "  no-explosion: %lu firings, %lu t/firing\n", g_clIsrN[1], \
  (g_clIsrN[1] ? g_clIsr[1]/g_clIsrN[1] : 0)
# DRAW cost vs ship altitude.  Run this on BOTH the combat and the quiet build and compare a row
# to the SAME row: equal DRAW t/it at matched altitude means combat's extra terrain cost is not
# combat rendering at all, it is combat throwing the ship to a cheaper/dearer viewpoint.
printf "\n=== object work inside DRAW ===\n"
printf "  cells WITH an occupant (all object work) : %lu  = %lu per iteration\n", \
  g_clObjEnter, ($it ? g_clObjEnter/$it : 0)
printf "  ...of those, reached raster_scaled_object: %u  = %lu per iteration\n", \
  g_clObjDraw, ($it ? g_clObjDraw/$it : 0)
printf "\n=== DRAW (terrain+obj x2) vs ship altitude ($28DA>>5) ===\n"
set $b = 0
while $b < 8
  if g_clAltIter[$b] > 0
    printf "  alt bucket %d ($%02x-$%02x): %5lu iters  DRAW %5lu t/it (%3lu.%01lu ms)\n", \
      $b, $b*32, $b*32+31, g_clAltIter[$b], g_clAltDraw[$b]/g_clAltIter[$b], \
      (g_clAltDraw[$b]*20)/(g_clAltIter[$b]*313), ((g_clAltDraw[$b]*200)/(g_clAltIter[$b]*313))%10
  end
  set $b = $b + 1
end
# ── WHERE INSIDE DRAW ────────────────────────────────────────────────────────────────────────
# The altitude table above tests the "combat throws the ship to a dearer viewpoint" hypothesis by
# matching rows across the combat and quiet runs.  This table answers the complementary question
# with no cross-build reasoning at all: which SEGMENT of terrain_draw_frame_core carries the
# delta.  tail is DERIVED (DRAW - head - obj) so the function's early returns need no bracket.
printf "\n=== where inside DRAW (t/iteration; 2 calls per iteration) ===\n"
set $draw = g_clPh[2][0] + g_clPh[2][1]
set $head = g_clDrawSub[0]
set $objs = g_clDrawSub[1]
set $age  = g_clDrawSub[2]
set $tail = $draw - $head - $objs
printf "  DRAW total (both passes) : %6lu t/it\n", ($it ? $draw/$it : 0)
printf "    head  (fills+xspans)   : %6lu t/it  = %2lu%% of DRAW\n", \
  ($it ? $head/$it : 0), ($draw ? (100*$head)/$draw : 0)
printf "    obj   (draw-order loop): %6lu t/it  = %2lu%% of DRAW\n", \
  ($it ? $objs/$it : 0), ($draw ? (100*$objs)/$draw : 0)
printf "    tail  (derived)        : %6lu t/it  = %2lu%% of DRAW\n", \
  ($it ? $tail/$it : 0), ($draw ? (100*$tail)/$draw : 0)
printf "      ...of which the $0A00 aging scan: %6lu t/it  [%lu scans over %lu draw calls = %lu%%]\n", \
  ($it ? $age/$it : 0), g_clAgeScans, g_clDrawSubN, \
  (g_clDrawSubN ? (100*g_clAgeScans)/g_clDrawSubN : 0)
printf "  (3 sub-brackets x 2 passes add ~13 t/it of floor to DRAW; identical in both builds)\n"
detach
quit
