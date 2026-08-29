# OBJECT-PLOTTER SHAPE PROBE — is the object plotter really combat's cost?
#
# The combat attribution (d05a72e) put 69% of the combat frame-time delta in DRAW and INFERRED
# the cause from a count: "~25 occupied cells enter terrain_plot_object per iteration, only ONE
# reaches raster_scaled_object, 25 x ~50 ticks ~= the +1161 t/it delta".  That arithmetic is
# numerology — 50 ticks is ~22000 68000 cycles, which a visit that bails on a table compare
# cannot spend.  This measures the split instead:
#
#   plotter chain ticks/it   (g_opTicks)   <- the whole per-object chain, both passes
#   scaled blit  ticks/it    (g_rsTicks)   <- NESTED inside the above, do not add them
#   DRAW ticks/it            (g_clPh[2])   <- the phase the two live inside
#
# If plotter << DRAW's combat delta, the delta is TERRAIN work and the next question is why —
# the leading alternative is that bolt impacts jitter the ship's attitude and a disturbed
# pitch/roll simply puts more terrain in the viewport (the earlier refutation only matched
# ALTITUDE, not attitude).  `make COMBAT=1` now flies straight (ROF_COMBAT_NO_JITTER), which is
# the controlled version of exactly that experiment.
#
# Build: make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1 OBJ_SHAPE=1
# Run:   . ./env.sh && GDBSCRIPT=obj_shape.gdb ./diag_run.sh 240
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u  $0623 emplace=%02x  $0621 saucer=%02x  $0624 fire=%02x  (start vbi=%u) ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], mem[0x624], g_vbiCount

# The ship-state bytes are printed on every segment so a COMBAT vs COMBAT_QUIET pair can be
# checked for TRAJECTORY agreement before their DRAW numbers are compared at all — that is the
# cross-build trap (FIXED_RNG pins the level, not the path through it).  ROF_COMBAT_NO_JITTER
# removes the impact attitude kicks, which were the main divergence source, so these should now
# track much more closely than they used to; if they still differ, say so instead of quoting a delta.
define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u painted %4u | expl %3u hit %3u fire %4u impact %3u | obj %4u draw %3u | head=%02x%02x oct=%02x rng=%02x pitch=%02x%02x | VVBLKI=$%02x%02x $3D=%02x\n", \
    g_vbiCount, g_fdCalls, g_clExplode, g_clShotHit, g_clEnemyFire, g_clImpact, \
    g_clObjEnter, g_clObjDraw, \
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

set $it = g_clPhIter[0] + g_clPhIter[1]
set $draw = g_clPh[2][0] + g_clPh[2][1]
set $wall = g_clIterWall[0] + g_clIterWall[1]
set $isrn = g_flightProf.isrCalls

# ⚠ THE BRACKET IS NOT FREE.  A quiet run reaches this with ZERO A822 calls, so nearly every
# one of its ~48 plotter visits per iteration is the two-load "empty cell" bail — yet the
# chain still measures ~110 t/it.  That floor IS the OP_TIME bracket (2x rof_subclock + 2
# volatile g_isrBeamLines reads ~= 2.2 t/call).  Subtract it before quoting the plotter's cost,
# or the probe's own overhead reads as the thing being measured.
printf "\n=== bracket-overhead floor (subtract this from the chain figure) ===\n"
printf "  %lu plotter brackets/it x ~2.2 t = ~%lu t/it of PROBE cost inside the chain number\n", \
  ($it ? g_opCalls/$it : 0), ($it ? (22*(g_opCalls/$it))/10 : 0)

printf "\n=== full phase table, ticks/iteration (both terrain passes bracketed) ===\n"
define ph
  printf "  %-24s %7lu t  (%5lu t/it)\n", $arg1, (g_clPh[$arg0][0]+g_clPh[$arg0][1]), \
    ($it ? (g_clPh[$arg0][0]+g_clPh[$arg0][1])/$it : 0)
end
ph 0 "SETUP  (frame_setup x2)"
ph 1 "CLEAR  (clear_col x2)"
ph 2 "DRAW   (terrain+obj x2)"
ph 3 "BOLT   (game_state_update)"
ph 4 "ENEMY  (enemy_check)"
ph 5 "FRAME  (ds_frame x2)"
set $sum = g_clPh[0][0]+g_clPh[0][1]+g_clPh[1][0]+g_clPh[1][1]+g_clPh[2][0]+g_clPh[2][1] \
         + g_clPh[3][0]+g_clPh[3][1]+g_clPh[4][0]+g_clPh[4][1]+g_clPh[5][0]+g_clPh[5][1]
printf "  %-24s %7lu t  (%5lu t/it)\n", "SUM", $sum, ($it ? $sum/$it : 0)
printf "  %-24s %7lu t  (%5lu t/it)  %lu firings = %lu/it\n", "VBI ISR (probe span)", \
  g_isrBeamLines, ($it ? g_isrBeamLines/$it : 0), $isrn, ($it ? $isrn/$it : 0)
printf "  covered %2lu%% of wall\n", ($wall ? (100*($sum+g_isrBeamLines))/$wall : 0)

# ── THE FLIGHT VBI ISR, PARTITIONED ─────────────────────────────────────────────────────────
# The ISR fires 50x/s no matter how slow the frame is, so at 92 t/firing it is ~29% of ALL wall
# clock in combat against ~20% quiet — plausibly a bigger slice of the combat delta than the
# object plotter.  It has never been partitioned UNDER COMBAT ("the flight VBI is ~irreducible"
# was measured on a quiet flight).  Values are ticks x1000 per firing, so 1000 = 1 tick.
printf "\n=== flight VBI ISR partition, ticks(x1000) per firing over %lu firings ===\n", $isrn
printf "  handler total (probe-excluded)  %6lu   = %lu t/firing\n", \
  ($isrn ? (1000*g_flightProf.isrLines)/$isrn : 0), ($isrn ? g_flightProf.isrLines/$isrn : 0)
define vp
  printf "    %-22s %6lu\n", $arg1, ($isrn ? (1000*$arg0)/$isrn : 0)
end
vp g_pInteg   "integ (50Hz motion)"
vp g_pProj    "proj  (terrain scanline)"
vp g_pSfx     "sfx   (voice envelope)"
vp g_pSfxEng  "  ...sfx engine step"
vp g_pSfxLoop "  ...sfx voice loop"
vp g_pSfxRing "  ...sfx event ring"
vp g_pDrawBr  "draw branch (P3/msg/score)"
vp g_pSimHead "sim head (obj/target/kbd)"
vp g_pAtmo    "atmosphere ramp"
vp g_pHud     "HUD instruments"
vp g_pScore   "score fold"
vp g_pTail    "tail (indicator+sfx)"

# ── WHY IS SFX SO EXPENSIVE IN COMBAT? ───────────────────────────────────────────────────────
# The partition above puts ~47% of the flight VBI in sfx_voice_envelope_tick, and the event-ring
# drain alone at ~24 t/firing = ~8% of ALL wall clock (the ISR runs at a fixed 50 Hz, so this is
# a flat tax, not something a slower frame dilutes).  Two very different fixes depending on the
# shape: MANY cheap ring iterations (the envelope-expiry re-queue churning) vs FEW expensive ones
# (sfx_event_load -> a changed POKEY register -> update_paula_channel's period divide).  These
# counters already existed in the tree; they just were never printed for a combat run.
printf "\n=== sfx cost drivers (per firing unless stated) ===\n"
printf "  event-ring entries drained : %lu total, %lu.%02lu per firing\n", g_sfxRingIters, \
  ($isrn ? g_sfxRingIters/$isrn : 0), ($isrn ? ((100*g_sfxRingIters)/$isrn)%100 : 0)
printf "  POKEY register writes      : %lu total, %lu.%02lu per firing\n", g_pokeyWrites, \
  ($isrn ? g_pokeyWrites/$isrn : 0), ($isrn ? ((100*g_pokeyWrites)/$isrn)%100 : 0)
printf "  ...of those CHANGED        : %lu  (%lu%% -- the rest are skipped by the change-detect)\n", \
  g_pokeyChanged, (g_pokeyWrites ? (100*g_pokeyChanged)/g_pokeyWrites : 0)
printf "  update_paula_channel       : %lu calls, %lu t total, %lu t/firing, %lu.%02lu t/call\n", \
  g_upcCalls, g_pUPC, ($isrn ? g_pUPC/$isrn : 0), \
  (g_upcCalls ? g_pUPC/g_upcCalls : 0), (g_upcCalls ? ((100*g_pUPC)/g_upcCalls)%100 : 0)
printf "  => update_paula_channel is %lu%% of the whole ISR budget\n", \
  (g_flightProf.isrLines ? (100*g_pUPC)/g_flightProf.isrLines : 0)

printf "\n=== scaled blit ticks by CALL SIZE (the spike, not the mean) ===\n"
set $i = 0
while $i < 5
  if g_rsBktCalls[$i] > 0
    printf "  bucket %d : %5lu calls  %7lu cells  %7lu t  (%3lu t/call, %lu.%02lu t/cell)\n", $i, \
      g_rsBktCalls[$i], g_rsBktCells[$i], g_rsBktTicks[$i], \
      g_rsBktTicks[$i]/g_rsBktCalls[$i], \
      g_rsBktTicks[$i]/g_rsBktCells[$i], ((100*g_rsBktTicks[$i])/g_rsBktCells[$i])%100
  end
  set $i = $i + 1
end

printf "\n=== THE SPLIT (%lu iterations; 1 tick = 63.56us, 313 t = one 20 ms PAL frame) ===\n", $it
printf "  wall                     %8lu t  (%5lu t/it)\n", $wall, ($it ? $wall/$it : 0)
printf "  DRAW phase (terrain+obj) %8lu t  (%5lu t/it) = %2lu%% of wall\n", $draw, \
  ($it ? $draw/$it : 0), ($wall ? (100*$draw)/$wall : 0)
printf "  plotter chain (x2 passes)%8lu t  (%5lu t/it) = %2lu%% of DRAW  <-- THE NUMBER\n", \
  g_opTicks, ($it ? g_opTicks/$it : 0), ($draw ? (100*g_opTicks)/$draw : 0)
printf "    ...of which scaled blit%8lu t  (%5lu t/it) = %2lu%% of the chain (NESTED)\n", \
  g_rsTicks, ($it ? g_rsTicks/$it : 0), (g_opTicks ? (100*g_rsTicks)/g_opTicks : 0)
printf "  terrain remainder        %8lu t  (%5lu t/it) = %2lu%% of DRAW\n", \
  ($draw > g_opTicks ? $draw - g_opTicks : 0), \
  ($it ? ($draw > g_opTicks ? ($draw - g_opTicks)/$it : 0) : 0), \
  ($draw ? (100*($draw > g_opTicks ? $draw - g_opTicks : 0))/$draw : 0)

printf "\n=== plotter-chain visit shape (%lu calls, %lu per iteration) ===\n", \
  g_opCalls, ($it ? g_opCalls/$it : 0)
printf "  ticks per call (x1000)   %lu\n", (g_opCalls ? (1000*g_opTicks)/g_opCalls : 0)
printf "  empty cell   (bail 1) : %8lu\n", g_opEmpty
printf "  step too small (bail 2): %8lu\n", g_opStep
printf "  -> A822 (peak/emplacement): %8lu\n", g_opPathA
printf "  -> A90A (other object)    : %8lu\n", g_opPathB
printf "  A822: slot busy           : %8lu\n", g_opaBusy
printf "  A822: occupant >= $FA     : %8lu   (dying/exploding: step/4 => up to 16x the cells)\n", g_opaMask
printf "  A822: below column clip   : %8lu\n", g_opaBelow
printf "  A822: ran distance+clip   : %8lu\n", g_opaDist
printf "  A822: reached fire gate   : %8lu\n", g_opaFire
printf "  A822: depth >= $0D, no blit: %8lu\n", g_opaDepth
printf "  A90A: trailing 4px cross  : %8lu\n", g_opbCross

printf "\n=== scaled blit (raster_scaled_object) ===\n"
printf "  calls %lu (%lu per iteration)   rows %lu   cells %lu   plots %lu   worst call %lu cells\n", \
  g_rsCalls, ($it ? g_rsCalls/$it : 0), g_rsRows, g_rsCells, g_rsPlots, g_rsCellMax
printf "  per call: %lu rows, %lu cells, %lu plots (%lu%% of cells plot)\n", \
  (g_rsCalls ? g_rsRows/g_rsCalls : 0), (g_rsCalls ? g_rsCells/g_rsCalls : 0), \
  (g_rsCalls ? g_rsPlots/g_rsCalls : 0), (g_rsCells ? (100*g_rsPlots)/g_rsCells : 0)
printf "  ticks per call (x1000): %lu   ticks per cell (x1000): %lu\n", \
  (g_rsCalls ? (1000*g_rsTicks)/g_rsCalls : 0), (g_rsCells ? (1000*g_rsTicks)/g_rsCells : 0)
printf "  cells by entry depth $0051 (0 = closest/biggest; the < $0D gate lets 0..12 through):\n"
set $i = 0
while $i < 14
  if g_rsCallsByStep[$i] > 0
    printf "    depth %2d : %5u calls  %7lu cells  (%3lu cells/call)\n", $i, \
      g_rsCallsByStep[$i], g_rsCellsByStep[$i], g_rsCellsByStep[$i]/g_rsCallsByStep[$i]
  end
  set $i = $i + 1
end
detach
quit
