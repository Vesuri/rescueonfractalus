# LEAN flight phase budget — where does one flight iteration actually go?
#
# obj_shape.gdb prints the same phase table, but only on an OBJ_SHAPE=1 build whose OP_TIME
# brackets add ~110 t/it INSIDE the DRAW phase (its own comment says so).  This script drops
# every OBJ_SHAPE/RS_SHAPE global so it runs on a LEAN probe build, where the only bracket cost
# is the 6 CL_PH pairs (~2.2 t each = ~26 t/it against a ~2400 t iteration).
#
# Rules honoured (flight-measurement-rules): assert live flight on every segment, quote the
# budget check (`covered`), divide ISR items by isrCalls (not g_vbiCount), and read ALL THREE
# flight-VBI brackets so work MOVED between them can't read as a win.
#
# Build: make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=phase_budget.gdb ./diag_run.sh 240
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u  $0623 emplace=%02x  $0621 saucer=%02x  $0624 fire=%02x  (start vbi=%u) ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], mem[0x624], g_vbiCount

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u painted %4u | expl %3u fire %4u impact %3u | head=%02x%02x alt=%02x | VVBLKI=$%02x%02x $3D=%02x\n", \
    g_vbiCount, g_fdCalls, g_clExplode, g_clEnemyFire, g_clImpact, \
    mem[0x2886], mem[0x2885], mem[0x28DA], mem[0x223], mem[0x222], mem[0x3D]
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
set $isrn = g_flightProf.isrCalls

printf "\n=== phase table, ticks/iteration (BOTH terrain passes bracketed) ===\n"
define ph
  printf "  %-26s %8lu t  (%5lu t/it)  %2lu%% of wall\n", $arg1, \
    (g_clPh[$arg0][0]+g_clPh[$arg0][1]), \
    ($it ? (g_clPh[$arg0][0]+g_clPh[$arg0][1])/$it : 0), \
    ($wall ? (100*(g_clPh[$arg0][0]+g_clPh[$arg0][1]))/$wall : 0)
end
ph 0 "SETUP  (frame_setup x2)"
ph 1 "CLEAR  (clear_col x2)"
ph 2 "DRAW   (terrain+obj x2)"
ph 3 "BOLT   (game_state_update)"
ph 4 "ENEMY  (enemy_check)"
ph 5 "FRAME  (ds_frame x2)"
set $sum = g_clPh[0][0]+g_clPh[0][1]+g_clPh[1][0]+g_clPh[1][1]+g_clPh[2][0]+g_clPh[2][1] \
         + g_clPh[3][0]+g_clPh[3][1]+g_clPh[4][0]+g_clPh[4][1]+g_clPh[5][0]+g_clPh[5][1]
printf "  %-26s %8lu t  (%5lu t/it)\n", "SUM of phases", $sum, ($it ? $sum/$it : 0)
printf "  %-26s %8lu t  (%5lu t/it)  %lu firings = %lu/it\n", "VBI ISR (probe span)", \
  g_isrBeamLines, ($it ? g_isrBeamLines/$it : 0), $isrn, ($it ? $isrn/$it : 0)
printf "  %-26s %8lu t  (%5lu t/it)\n", "WALL", $wall, ($it ? $wall/$it : 0)
printf "  covered %2lu%% of wall  <-- must be ~100%% or the split is meaningless\n", \
  ($wall ? (100*($sum+g_isrBeamLines))/$wall : 0)

# ── the flight VBI, all three brackets ──────────────────────────────────────────────────────
printf "\n=== flight VBI ISR, ticks(x1000) per firing over %lu firings ===\n", $isrn
printf "  handler total (probe-excluded)  %6lu   = %lu t/firing\n", \
  ($isrn ? (1000*g_flightProf.isrLines)/$isrn : 0), ($isrn ? g_flightProf.isrLines/$isrn : 0)
printf "  sprites bracket                 %6lu\n", ($isrn ? (1000*g_vbiSpriteLines)/$isrn : 0)
printf "  audio bracket                   %6lu\n", ($isrn ? (1000*g_vbiAudioLines)/$isrn : 0)
printf "  TOTAL                           %6lu   = %lu t/firing = %lu%% of all wall clock\n", \
  ($isrn ? (1000*(g_flightProf.isrLines+g_vbiSpriteLines+g_vbiAudioLines))/$isrn : 0), \
  ($isrn ? (g_flightProf.isrLines+g_vbiSpriteLines+g_vbiAudioLines)/$isrn : 0), \
  ($wall ? (100*(g_flightProf.isrLines+g_vbiSpriteLines+g_vbiAudioLines))/$wall : 0)
define vp
  printf "    %-24s %6lu\n", $arg1, ($isrn ? (1000*$arg0)/$isrn : 0)
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

printf "\n=== sfx cost drivers (per firing) ===\n"
printf "  event-ring entries drained : %lu total, %lu.%02lu per firing\n", g_sfxRingIters, \
  ($isrn ? g_sfxRingIters/$isrn : 0), ($isrn ? ((100*g_sfxRingIters)/$isrn)%100 : 0)
printf "  POKEY register writes      : %lu total, %lu.%02lu per firing (%lu%% changed)\n", \
  g_pokeyWrites, ($isrn ? g_pokeyWrites/$isrn : 0), ($isrn ? ((100*g_pokeyWrites)/$isrn)%100 : 0), \
  (g_pokeyWrites ? (100*g_pokeyChanged)/g_pokeyWrites : 0)
printf "  update_paula_channel       : %lu calls, %lu t/firing, %lu.%02lu t/call\n", \
  g_upcCalls, ($isrn ? g_pUPC/$isrn : 0), \
  (g_upcCalls ? g_pUPC/g_upcCalls : 0), (g_upcCalls ? ((100*g_pUPC)/g_upcCalls)%100 : 0)

# ── the FRAME phase, opened up ──────────────────────────────────────────────────────────────
# FRAME (= ds_frame x2 = platform_tick_vbi + platform_render_frame) is the 2nd biggest phase,
# and renderFlightDirect's four internal laps account for only about half of it.  These three
# brackets already existed in the tree (RescueOnFractalus::renderFrame) but were never printed
# next to the phase table, so the remainder was never attributed.
printf "\n=== FRAME phase opened up, ticks/iteration (%lu renderFrame calls) ===\n", g_rCalls
printf "  FRAME phase total       %6lu t/it\n", ($it ? (g_clPh[5][0]+g_clPh[5][1])/$it : 0)
printf "   perFrameWork (sprites) %6lu t/it   <- 6 flight sprite builders + HUD digits\n", \
  ($it ? g_rPerFrame/$it : 0)
printf "   render() total         %6lu t/it\n", ($it ? g_rRenderFn/$it : 0)
printf "     ...renderFlightDirect%6lu t/it\n", ($it ? g_fDirect/$it : 0)
printf "     ...cockpit scan      %6lu t/it   (%lu scans)\n", ($it ? g_fCockpit/$it : 0), g_fCockpitScans
printf "   updateFlightCopper     %6lu t/it\n", ($it ? g_rCopper/$it : 0)

# ── the cockpit scan, split PER GROUP ───────────────────────────────────────────────────────
# g_fCockpit lumps digits + lock-on + dial together, so "something decoded on ~0.9 of iterations"
# cannot tell a 22-cell digit block from a 1-cell dial cell.  fires/it and t/it per group; the
# writer counters size a per-digit mask (writeDigit calls per digit fire == blocks that changed).
printf "\n=== cockpit scan per group (total %lu t/it, %lu scans) ===\n", \
  ($it ? g_fCockpit/$it : 0), g_fCockpitScans
printf "  digits  %6lu t/it  %5lu fires (%lu.%02lu/it), %lu blocks decoded (%lu.%02lu/fire, was 6)\n", \
  ($it ? g_ckDigitT/$it : 0), \
  g_ckDigitFires, ($it ? g_ckDigitFires/$it : 0), ($it ? ((100*g_ckDigitFires)/$it)%100 : 0), \
  g_ckDigitBlocks, (g_ckDigitFires ? g_ckDigitBlocks/g_ckDigitFires : 0), \
  (g_ckDigitFires ? ((100*g_ckDigitBlocks)/g_ckDigitFires)%100 : 0)
printf "  lockon  %6lu t/it  %5lu fires (%lu.%02lu/it), %lu cells decoded (%lu.%02lu/fire, was 7)\n", \
  ($it ? g_ckLockT/$it : 0), g_ckLockFires, \
  ($it ? g_ckLockFires/$it : 0), ($it ? ((100*g_ckLockFires)/$it)%100 : 0), g_ckLockCells, \
  (g_ckLockFires ? g_ckLockCells/g_ckLockFires : 0), \
  (g_ckLockFires ? ((100*g_ckLockCells)/g_ckLockFires)%100 : 0)
printf "  dial    %6lu t/it  %5lu fires (%lu.%02lu/it), %lu cells decoded (%lu.%02lu/fire)\n", \
  ($it ? g_ckDialT/$it : 0), g_ckDialFires, \
  ($it ? g_ckDialFires/$it : 0), ($it ? ((100*g_ckDialFires)/$it)%100 : 0), g_ckDialCells, \
  (g_ckDialFires ? g_ckDialCells/g_ckDialFires : 0), \
  (g_ckDialFires ? ((100*g_ckDialCells)/g_ckDialFires)%100 : 0)
printf "  writers: startup_init_native %lu calls, writeDigit %lu (%lu.%02lu per digit fire), stride flips %lu\n", \
  g_ckSiNative, g_ckWdigCalls, \
  (g_ckDigitFires ? g_ckWdigCalls/g_ckDigitFires : 0), \
  (g_ckDigitFires ? ((100*g_ckWdigCalls)/g_ckDigitFires)%100 : 0), g_ckStrideFlips

printf "\n=== renderFlightDirect internals, ticks/iteration over %lu calls ===\n", g_fdCalls
printf "  clear/copy  %6lu t (%4lu t/it)\n", g_fdClear, ($it ? g_fdClear/$it : 0)
printf "  edge+fillup %6lu t (%4lu t/it)\n", g_fdEdge,  ($it ? g_fdEdge/$it  : 0)
printf "  late sprite %6lu t (%4lu t/it)  <- g_fdScan: buildFlightSpritesLate, in the fill's shadow\n", \
  g_fdScan, ($it ? g_fdScan/$it : 0)
printf "  fill wait   %6lu t (%4lu t/it)\n", g_fdFill,  ($it ? g_fdFill/$it  : 0)
printf "  band+overlay%6lu t (%4lu t/it)\n", g_fdBand,  ($it ? g_fdBand/$it  : 0)
printf "  SUM of laps %6lu t (%4lu t/it)  vs g_fDirect %lu t/it (rest = entry/probe)\n", \
  (g_fdClear+g_fdEdge+g_fdScan+g_fdFill+g_fdBand), \
  ($it ? (g_fdClear+g_fdEdge+g_fdScan+g_fdFill+g_fdBand)/$it : 0), ($it ? g_fDirect/$it : 0)

printf "\n=== iterations %lu, painted frames %lu ===\n", $it, g_fdCalls
detach
quit
