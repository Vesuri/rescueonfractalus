# FULL flight-VBI breakdown — every bucket, WINDOWED (the fix isr_ab/phase_budget only half make).
#
# Why this exists: isr_ab.gdb windows only sfx/integ/proj, and phase_budget.gdb prints the whole
# g_p* set but divides POWER-ON accumulators by the FLIGHT-ONLY isrCalls (vbi_shared_tail runs the
# sfx tick on standby vblanks too), which overstates the sfx share.  This script snapshots EVERY
# bucket at the window open and prints deltas, so the parts sum to the whole handler.
#
# Build: make clean && make -j4 COMBAT=1 [COMBAT_QUIET=1] PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=isr_full.gdb ./diag_run.sh 300
#
# All bucket rows are milli-ticks per firing (1000 = 1 raster line = 63.56 us).  The three
# game_vbi_isr brackets (handler + sprites + audio) are the whole-VBI total — quote THAT.
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2600
continue
printf "=== snapshot in flight at vbi %u (level %u, $0623=%02x $0621=%02x) ===\n", \
  g_vbiCount, mem[0x6D], mem[0x623], mem[0x621]
set $s_isrn = g_flightProf.isrCalls
set $s_isrl = g_flightProf.isrLines
set $s_vbi  = g_vbiCount
set $s_vsp  = g_vbiSpriteLines
set $s_vau  = g_vbiAudioLines
set $s_vfl  = g_vbiFlushLines
set $s_vno  = g_vbiNoiseLines
set $s_vfc  = g_vbiFullCalls
set $s_sfx  = g_pSfx
set $s_seng = g_pSfxEng
set $s_loop = g_pSfxLoop
set $s_ring = g_pSfxRing
set $s_integ = g_pInteg
set $s_proj  = g_pProj
set $s_draw  = g_pDrawBr
set $s_head  = g_pSimHead
set $s_atmo  = g_pAtmo
set $s_hud   = g_pHud
set $s_score = g_pScore
set $s_tail  = g_pTail
set $s_iters = g_sfxRingIters
set $s_upcC  = g_upcCalls
set $s_upcT  = g_pUPC
set $s_fd    = g_fdCalls

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 5400
continue

set $n  = g_flightProf.isrCalls - $s_isrn
set $il = g_flightProf.isrLines - $s_isrl
set $vsp = g_vbiSpriteLines - $s_vsp
set $vau = g_vbiAudioLines  - $s_vau
set $vfc = g_vbiFullCalls   - $s_vfc
printf "\n=== window vbi %u -> %u : %lu firings, %u painted ===\n", $s_vbi, g_vbiCount, $n, \
  (g_fdCalls-$s_fd)
printf "  VVBLKI=$%02x%02x $3D=%02x  (must be $4ff5 / 00)\n", mem[0x223], mem[0x222], mem[0x3D]
printf "\n  handler = %lu.%03lu   sprites = %lu.%03lu   audio = %lu.%03lu   t/firing\n", \
  ($n ? $il/$n : 0), ($n ? ((1000*$il)/$n)%1000 : 0), \
  ($vfc ? $vsp/$vfc : 0), ($vfc ? ((1000*$vsp)/$vfc)%1000 : 0), \
  ($vfc ? $vau/$vfc : 0), ($vfc ? ((1000*$vau)/$vfc)%1000 : 0)
printf "     audio bracket IS flush_paula %lu.%03lu  (noiseTick left the ISR)\n", \
  ($vfc ? (g_vbiFlushLines-$s_vfl)/$vfc : 0), ($vfc ? ((1000*(g_vbiFlushLines-$s_vfl))/$vfc)%1000 : 0)
# noiseTick now runs in the MAIN LOOP (PlatformAmiga::renderFrame), so it is NOT part of the VBI
# total above.  Still divided by VBI FIRINGS so the row stays directly comparable to the old
# in-ISR t/firing figure — the move shows up as the drop it is, not as a units change.
printf "     noiseTick (main loop, /firing for comparability) %lu.%03lu\n", \
  ($vfc ? (g_vbiNoiseLines-$s_vno)/$vfc : 0), ($vfc ? ((1000*(g_vbiNoiseLines-$s_vno))/$vfc)%1000 : 0)
printf "  *** WHOLE flight VBI = %lu.%03lu t/firing = %lu.%lu%% of ALL wall clock  <-- THE A/B NUMBER ***\n", \
  ($vfc ? ($il+$vsp+$vau)/$vfc : 0), ($vfc ? ((1000*($il+$vsp+$vau))/$vfc)%1000 : 0), \
  ($vfc ? (100*($il+$vsp+$vau))/(313*$vfc) : 0), ($vfc ? ((1000*($il+$vsp+$vau))/(313*$vfc))%10 : 0)

printf "\n=== handler buckets (milli-ticks per firing; sum should approach the handler row) ===\n"
define d
  printf "  %-30s %7lu   = %2lu%% of the handler\n", $arg1, ($n ? (1000*$arg0)/$n : 0), \
    ($il ? (100*$arg0)/$il : 0)
end
d (g_pSfx-$s_sfx)      "sfx total (tail-called)"
d (g_pSfxEng-$s_seng)  "  ...sfx engine step"
d (g_pSfxLoop-$s_loop) "  ...14-slot envelope loop"
d (g_pSfxRing-$s_ring) "  ...event-ring drain"
d (g_pInteg-$s_integ)  "integ (50Hz motion sim)"
d (g_pProj-$s_proj)    "proj (terrain scanline)"
d (g_pDrawBr-$s_draw)  "draw branch (P3/msg/score)"
d (g_pSimHead-$s_head) "sim head (obj/target/kbd)"
d (g_pAtmo-$s_atmo)    "atmosphere ramp"
d (g_pHud-$s_hud)      "HUD instruments"
d (g_pScore-$s_score)  "score fold"
d (g_pTail-$s_tail)    "tail (indicator + sfx)"
printf "  ---- accounted (integ+proj+draw+head+atmo+hud+score+tail) %7lu of handler %7lu\n", \
  ($n ? (1000*((g_pInteg-$s_integ)+(g_pProj-$s_proj)+(g_pDrawBr-$s_draw)+(g_pSimHead-$s_head) \
        +(g_pAtmo-$s_atmo)+(g_pHud-$s_hud)+(g_pScore-$s_score)+(g_pTail-$s_tail)))/$n : 0), \
  ($n ? (1000*$il)/$n : 0)

printf "\n=== sfx cost drivers ===\n"
printf "  ring entries drained   : %lu.%02lu per firing\n", \
  ($n ? (g_sfxRingIters-$s_iters)/$n : 0), ($n ? ((100*(g_sfxRingIters-$s_iters))/$n)%100 : 0)
printf "  update_paula_channel   : %lu.%02lu calls/firing, %lu milli-t/firing\n", \
  ($n ? (g_upcCalls-$s_upcC)/$n : 0), ($n ? ((100*(g_upcCalls-$s_upcC))/$n)%100 : 0), \
  ($n ? (1000*(g_pUPC-$s_upcT))/$n : 0)
detach
quit
