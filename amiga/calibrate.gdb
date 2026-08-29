# PRICE THE MACHINE, NOT THE WORK — and find out how much a phase bracket exaggerates combat.
#
# Two questions, settled here 2026-08-07 over five COMBAT vs COMBAT_QUIET pairs at level 40 with
# FIXED_RNG:
#
# 1. Is combat's DRAW delta a dearer viewpoint?  NO.  combat_probe.gdb's altitude table shows the
#    same ~1.25 ratio in EVERY matched altitude bucket with a near-identical bucket mix, and the
#    terrain tree-entry count printed below is 68/iteration in BOTH builds, every run.  Combat
#    does not draw more terrain.
#
# 2. Is the machine slower in combat (DMA stolen by sprites/audio/blitter)?  ALSO NO — and the
#    harness said otherwise.  A fixed-trip, data-independent load measured by an ordinary phase
#    bracket reads +15% in combat; the SAME load measured as short ISR-free windows reads +1%.
#    The gap is the estimator: `elapsed - g_isrBeamLines` credits an ISR at its EXIT, so an ISR
#    straddling the bracket start is over-subtracted and one straddling the end under-subtracted,
#    and those cancel only when the two ends are independent.  For a window near the 313 t ISR
#    period they are not — and combat's window sat at 87% of the period against the control's 75%.
#
# ⇒ Read the ISR-FREE and BEAM-LOCKED rows, not the single-bracket rows, and treat every
#   cross-build phase comparison in this tree as inflated on the side with more ISR firings.
#
# Build BOTH, run each, compare the two logs (OBJ_SHAPE=1 is required — the plotter block at the
# bottom reads its counters, and it is what splits DRAW's machine-corrected residual between the
# object plotter and the terrain tree):
#   make clean && make -j4 COMBAT=1 PROBES=1 FIXED_RNG=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 CALIBRATE=1 OBJ_SHAPE=1
#   make clean && make -j4 COMBAT=1 COMBAT_QUIET=1 PROBES=1 FIXED_RNG=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 CALIBRATE=1 OBJ_SHAPE=1
# Run: . ./env.sh && GDBSCRIPT=calibrate.gdb ./diag_run.sh 220
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u  $0623 emplace=%02x  $0621 saucer=%02x  $0624 fire=%02x  (start vbi=%u) ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], mem[0x624], g_vbiCount

# Trajectory bytes on every segment.  FIXED_RNG pins the LEVEL, not the path through it, so the
# combat and quiet runs must be checked for agreement BEFORE their per-phase numbers are compared
# — if heading/octant/altitude diverge the two builds are flying over different terrain and any
# delta is contaminated.  ROF_COMBAT_NO_JITTER (implied by COMBAT) removes the impact attitude
# kicks, which was the main divergence source.
define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u painted %4u | expl %3u fire %4u impact %3u | obj %4u draw %3u | head=%02x%02x oct=%02x rng=%02x pitch=%02x%02x | VVBLKI=$%02x%02x $3D=%02x\n", \
    g_vbiCount, g_fdCalls, g_clExplode, g_clEnemyFire, g_clImpact, g_clObjEnter, g_clObjDraw, \
    mem[0x2886], mem[0x2885], mem[0x280D], mem[0x28DA], mem[0x0029], mem[0x0028], \
    mem[0x223], mem[0x222], mem[0x3D]
end
seg 2400
seg 2900
seg 3400
seg 3900
seg 4400
seg 4900
seg 5400

set $it   = g_clPhIter[0] + g_clPhIter[1]
set $wall = g_clIterWall[0] + g_clIterWall[1]
set $draw = g_clPh[2][0] + g_clPh[2][1]

printf "\n=== THE CALIBRATION (%lu iterations) — fixed work, so this is the MACHINE ===\n", g_clCalN
printf "  MEM  1024 volatile mem[] byte reads : %8lu t total, %lu.%02lu t/iteration\n", g_clCalMem, \
  (g_clCalN ? g_clCalMem/g_clCalN : 0), (g_clCalN ? ((100*g_clCalMem)/g_clCalN)%100 : 0)
printf "  CPU  1024-step LFSR, registers only : %8lu t total, %lu.%02lu t/iteration\n", g_clCalCpu, \
  (g_clCalN ? g_clCalCpu/g_clCalN : 0), (g_clCalN ? ((100*g_clCalCpu)/g_clCalN)%100 : 0)
printf "  MEM again, AUD+SPRITE DMA masked    : %8lu t total, %lu.%02lu t/iteration\n", g_clCalNoDma, \
  (g_clCalN ? g_clCalNoDma/g_clCalN : 0), (g_clCalN ? ((100*g_clCalNoDma)/g_clCalN)%100 : 0)
printf "  MEM x128, ISR-FREE windows only     : %8lu t over %lu windows, %lu.%02lu t/window\n", \
  g_clCalIsrFree, g_clCalIsrFreeN, \
  (g_clCalIsrFreeN ? g_clCalIsrFree/g_clCalIsrFreeN : 0), \
  (g_clCalIsrFreeN ? ((100*g_clCalIsrFree)/g_clCalIsrFreeN)%100 : 0)
printf "  MEM x128, BEAM-LOCKED to line 200   : %8lu t over %lu windows, %lu.%02lu t/window\n", \
  g_clCalLocked, g_clCalLockedN, \
  (g_clCalLockedN ? g_clCalLocked/g_clCalLockedN : 0), \
  (g_clCalLockedN ? ((100*g_clCalLocked)/g_clCalLockedN)%100 : 0)
printf "  MEM x1024 as 8 ISR-FREE sub-windows : %8lu t over %lu windows, %lu.%02lu t/128-read\n", \
  g_clCalSplit, g_clCalSplitN, \
  (g_clCalSplitN ? g_clCalSplit/g_clCalSplitN : 0), \
  (g_clCalSplitN ? ((100*g_clCalSplit)/g_clCalSplitN)%100 : 0)
printf "    => same work as row 1, ISR-subtraction-free: %lu t per 1024 reads\n", \
  (g_clCalSplitN ? (8*g_clCalSplit)/g_clCalSplitN : 0)
printf "  (compare each against the OTHER build's figure; that ratio is the machine slowdown.\n"
printf "   MEM-masked isolates audio/sprite DMA; ISR-FREE needs no ISR subtraction; BEAM-LOCKED\n"
printf "   additionally pins the frame phase, so it is the cleanest of the four.  Ratios that\n"
printf "   shrink as you go down the list mean the earlier rows were measuring the harness.)\n"

printf "\n=== phase table, ticks/iteration (%lu iterations) ===\n", $it
define ph
  printf "  %-24s %5lu t/it\n", $arg1, ($it ? (g_clPh[$arg0][0]+g_clPh[$arg0][1])/$it : 0)
end
ph 0 "SETUP  (frame_setup x2)"
ph 1 "CLEAR  (clear_col x2)"
ph 2 "DRAW   (terrain+obj x2)"
ph 3 "BOLT   (game_state_update)"
ph 4 "ENEMY  (enemy_check)"
ph 5 "FRAME  (ds_frame x2)"
printf "  %-24s %5lu t/it   [%lu firings/it, %lu t/firing real handler]\n", "VBI ISR (probe span)", \
  ($it ? g_isrBeamLines/$it : 0), \
  ($it ? g_flightProf.isrCalls/$it : 0), \
  (g_flightProf.isrCalls ? g_flightProf.isrLines/g_flightProf.isrCalls : 0)
printf "  %-24s %5lu t/it\n", "wall", ($it ? $wall/$it : 0)

printf "\n=== where inside DRAW, and how much tree work drove it ===\n"
set $head = g_clDrawSub[0]
set $objs = g_clDrawSub[1]
printf "  head (fills+xspans)    : %5lu t/it\n", ($it ? $head/$it : 0)
printf "  obj  (draw-order loop) : %5lu t/it\n", ($it ? $objs/$it : 0)
printf "  tail (derived)         : %5lu t/it   [$0A00 aging scan %lu t/it, %lu scans]\n", \
  ($it ? ($draw - $head - $objs)/$it : 0), ($it ? g_clDrawSub[2]/$it : 0), g_clAgeScans
# THE COUNT that separates "more work" from "same work, slower machine".  One subdivide call per
# both-endpoints-visible pair = one terrain tree entry.  No bracket, so no probe floor.
printf "  terrain tree entries   : %5lu subdivide calls/it  (%lu total)\n", \
  ($it ? g_clSubCalls/$it : 0), g_clSubCalls
printf "  => obj segment per tree entry: %lu.%02lu t  <-- the per-unit-of-work cost\n", \
  (g_clSubCalls ? $objs/g_clSubCalls : 0), (g_clSubCalls ? ((100*$objs)/g_clSubCalls)%100 : 0)

# ── SPLIT THE RESIDUAL: object plotter vs terrain tree (needs OBJ_SHAPE=1) ───────────────────
# The tree-entry count above is the same in both builds, so combat is not subdividing more
# terrain.  That leaves two places for DRAW's machine-corrected residual: the plotter chain
# (project_terrain_points + terrain_plot_object + the scaled blit nested in it), which combat
# obviously drives harder, or deeper work per tree entry.  The chain is bracketed, so subtract
# its ~2.2 t/call probe floor before quoting it — the floor is the same in both builds, which is
# why the DIFFERENCE is trustworthy even though the absolute is not.
printf "\n=== object plotter chain vs terrain tree, inside the obj segment ===\n"
printf "  plotter chain : %5lu t/it over %lu calls/it  (floor ~%lu t/it at 2.2 t/call)\n", \
  ($it ? g_opTicks/$it : 0), ($it ? g_opCalls/$it : 0), ($it ? (22*(g_opCalls/$it))/10 : 0)
printf "    ...scaled blit (NESTED): %5lu t/it over %lu calls\n", \
  ($it ? g_rsTicks/$it : 0), g_rsCalls
printf "  occupied cells visited: %lu/it   reached the blit: %lu total\n", \
  ($it ? g_clObjEnter/$it : 0), g_clObjDraw
printf "  tree + loop (obj - chain): %5lu t/it  = %lu.%02lu t per tree entry\n", \
  ($it ? ($objs > g_opTicks ? ($objs - g_opTicks)/$it : 0) : 0), \
  (g_clSubCalls ? ($objs > g_opTicks ? ($objs - g_opTicks)/g_clSubCalls : 0) : 0), \
  (g_clSubCalls ? ((100*($objs > g_opTicks ? $objs - g_opTicks : 0))/g_clSubCalls)%100 : 0)
detach
quit
