# SFX SHAPE PROBE — where do the flight VBI's sfx ticks/firing actually go?
#
# Context (f1c3633): the flight VBI is ~93 t/firing in combat and its sfx block is the largest
# single item.  The ISR fires 50x/s regardless of frame rate, so every tick here is a flat tax on
# ALL wall clock, not something a slow frame dilutes.  Measured findings this probe established:
#   - the cost is sfx_reorder_voice_slot, ~3.2 calls/firing at ~6.7-7.1 t each;
#     sfx_event_load is a red herring at 0.22 calls/firing.
#   - it is NOT the Paula recompute, and build_poly_dist (the 1022-byte rebuild that looked like
#     the obvious suspect) is called ZERO times — do not "optimise" it.
#   - the reorder storm is driven by the envelope loop: every active DURATION envelope
#     (~2.1/firing) pushes a ring entry via game_sub_55FC, which the drain turns into a full
#     priority-mixer pass.  Envelope EXPIRIES are only 0.11/firing, so they are not the driver.
# The two branches:
#   entry bit7 SET   -> sfx_event_load        (load a voice from the $56D4.. tables, then
#                                              game_sub_55FC RE-PUSHES the slot => >=2 iterations
#                                              per event)
#   entry bit7 CLEAR -> sfx_reorder_voice_slot (the priority mixer: sfx_pick_top_voice /
#                                              sfx_pick_next_voice, each a 12-slot volatile scan,
#                                              plus 1-2 sfx_voice_write_freq_ctrl)
# Build: make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1 SFX_SHAPE=1
# Run:   . ./env.sh && GDBSCRIPT=sfx_shape.gdb ./diag_run.sh 240
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u  $0623 emplace=%02x  $0621 saucer=%02x  (start vbi=%u) ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], g_vbiCount

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u painted %4u | expl %3u fire %4u impact %3u | ring %6lu | head=%02x%02x roll=%02x%02x | VVBLKI=$%02x%02x $3D=%02x\n", \
    g_vbiCount, g_fdCalls, g_clExplode, g_clEnemyFire, g_clImpact, g_sfxRingIters, \
    mem[0x2886], mem[0x2885], mem[0x0029], mem[0x0028], mem[0x223], mem[0x222], mem[0x3D]
end
# ⚠ SNAPSHOT FIRST.  g_pSfx / g_pSfxRing / g_pSfxLoop / the g_sx* counters all accumulate from
# POWER-ON, and vbi_shared_tail ($534D) runs sfx_voice_envelope_tick on every standby/cinematic
# vblank too — while g_flightProf.isrCalls counts FLIGHT firings only.  Dividing one by the other
# mixes a whole-run numerator with a flight-only denominator and overstates the flight sfx cost by
# ~40% (there are ~1550 pre-flight vblanks in a 240 s run).  flight_prof_reset() exists but is
# never called and would not reset these anyway, so take the delta over a window that is entirely
# in flight instead.  This is the mirror image of the documented "divide by isrCalls, not
# g_vbiCount" rule — the same mistake, made on the numerator.
seg 2600
set $s_isrn = g_flightProf.isrCalls
set $s_isrl = g_flightProf.isrLines
set $s_sfx  = g_pSfx
set $s_ring = g_pSfxRing
set $s_loop = g_pSfxLoop
set $s_integ = g_pInteg
set $s_proj  = g_pProj
set $s_evl  = g_sxEvLoad
set $s_evlt = g_sxEvLoadT
set $s_reo  = g_sxReord
set $s_reot = g_sxReordT
set $s_top  = g_sxTopScan
set $s_next = g_sxNextScan
set $s_wrc  = g_sxWrCtrl
set $s_wrf  = g_sxWrFreq
set $s_push = g_sxRingPush
set $s_exp  = g_sxExpired
set $s_af   = g_sxActFreq
set $s_ad   = g_sxActDur
set $s_topT = g_sxTopScanT
set $s_nxtT = g_sxNextScanT
set $s_wrcT = g_sxWrCtrlT
set $s_pkT  = g_sxPokeyT
set $s_nop  = g_sxNop
set $s_nopT = g_sxNopT
set $s_leaf = g_sxLeafCalls
set $s_upcC = g_upcCalls
set $s_upcT = g_pUPC
set $s_pw   = g_pokeyWrites
set $s_pwc  = g_pokeyChanged
printf "  (snapshot taken in flight at vbi %u; all figures below are DELTAS over this window)\n", g_vbiCount

seg 3300
seg 4000
seg 4700
seg 5400

set $n  = g_flightProf.isrCalls - $s_isrn
set $il = g_flightProf.isrLines - $s_isrl
printf "\n=== flight VBI over the in-flight window: %lu firings, %lu t/firing ===\n", $n, \
  ($n ? $il/$n : 0)
define d
  printf "  %-30s %8lu t  %lu.%03lu t/firing  = %2lu%% of the ISR\n", $arg1, $arg0, \
    ($n ? $arg0/$n : 0), ($n ? ((1000*$arg0)/$n)%1000 : 0), ($il ? (100*$arg0)/$il : 0)
end
d (g_pSfx-$s_sfx)      "sfx total"
d (g_pSfxRing-$s_ring) "  ...event-ring drain"
d (g_pSfxLoop-$s_loop) "  ...14-slot envelope loop"
d (g_pInteg-$s_integ)  "integ (50Hz motion sim)"
d (g_pProj-$s_proj)    "proj (terrain scanline)"

printf "\n=== ring drain, split by branch (SX_SPAN beam ticks) ===\n"
set $reo  = g_sxReord  - $s_reo
set $reot = g_sxReordT - $s_reot
set $evl  = g_sxEvLoad  - $s_evl
set $evlt = g_sxEvLoadT - $s_evlt
printf "  sfx_event_load        : %6lu calls (%lu.%02lu/firing)  %7lu t  %lu.%02lu t/call\n", \
  $evl, ($n ? $evl/$n : 0), ($n ? ((100*$evl)/$n)%100 : 0), $evlt, \
  ($evl ? $evlt/$evl : 0), ($evl ? ((100*$evlt)/$evl)%100 : 0)
printf "  sfx_reorder_voice_slot: %6lu calls (%lu.%02lu/firing)  %7lu t  %lu.%02lu t/call  <-- THE ITEM\n", \
  $reo, ($n ? $reo/$n : 0), ($n ? ((100*$reo)/$n)%100 : 0), $reot, \
  ($reo ? $reot/$reo : 0), ($reo ? ((100*$reot)/$reo)%100 : 0)
printf "  reorder = %lu.%02lu t/firing = %lu%% of the ISR\n", \
  ($n ? $reot/$n : 0), ($n ? ((100*$reot)/$n)%100 : 0), ($il ? (100*$reot)/$il : 0)

printf "\n=== INSIDE sfx_reorder_voice_slot: leaf split (SX_SPAN, floor-corrected) ===\n"
# The empty bracket sampled once per reorder call IS the floor.  Report it first, in
# milli-ticks, then subtract one floor per leaf call from each leaf total: without that
# correction a cheap leaf reads as ~the bracket and the split is meaningless.
set $nop  = g_sxNop  - $s_nop
set $nopT = g_sxNopT - $s_nopT
set $leaf = g_sxLeafCalls - $s_leaf
set $fl   = $nop ? (1000*$nopT)/$nop : 0
printf "  empty-bracket floor : %lu samples, %lu.%03lu t each  (%lu leaf calls => %lu t of floor)\n", \
  $nop, $fl/1000, $fl%1000, $leaf, ($fl*$leaf)/1000
define leaf
  set $raw = $arg0
  set $cnt = $arg1
  set $net = $raw - ($fl*$cnt)/1000
  printf "  %-26s %6lu calls  %7lu t raw  %7lu t net  %lu.%03lu t/call net  %2lu%% of reorder\n", \
    $arg2, $cnt, $raw, $net, ($cnt ? $net/$cnt : 0), ($cnt ? ((1000*$net)/$cnt)%1000 : 0), \
    ($reot ? (100*$net)/$reot : 0)
end
leaf (g_sxWrCtrlT-$s_wrcT)  (g_sxWrCtrl-$s_wrc)    "voice_write_freq_ctrl"
leaf (g_sxPokeyT-$s_pkT) (2*(g_sxWrCtrl-$s_wrc)) "  ..its 2 rof_pokey_write"
leaf (g_sxTopScanT-$s_topT) (g_sxTopScan-$s_top)   "pick_top_voice (12-slot)"
leaf (g_sxNextScanT-$s_nxtT) (g_sxNextScan-$s_next) "pick_next_voice (12-slot)"
set $nest = (g_sxWrCtrlT-$s_wrcT) + (g_sxTopScanT-$s_topT) + (g_sxNextScanT-$s_nxtT)
printf "  reorder's OWN logic (reordT - leaf raw - its own nop) : %lu t, %lu%% of reorder\n", \
  ($reot - $nest - $nopT), ($reot ? (100*($reot - $nest - $nopT))/$reot : 0)
printf "  ^ ASM TARGET = own logic + the two scans; write_freq_ctrl's cost is mostly rof_pokey_write\n"

printf "\n=== what those branches drive (per firing) ===\n"
define per
  printf "  %-28s %8lu total   %lu.%02lu per firing\n", $arg1, $arg0, \
    ($n ? $arg0/$n : 0), ($n ? ((100*$arg0)/$n)%100 : 0)
end
per (g_sxTopScan-$s_top)   "sfx_pick_top_voice (12-slot)"
per (g_sxNextScan-$s_next) "sfx_pick_next_voice (12-slot)"
per (g_sxWrCtrl-$s_wrc)    "voice_write_freq_ctrl"
per (g_sxWrFreq-$s_wrf)    "voice_write_freq"
per (g_sxRingPush-$s_push) "game_sub_55FC ring pushes"
per (g_sxExpired-$s_exp)   "envelope expiries (re-push)"
per (g_sxActFreq-$s_af)    "active freq envelopes"
per (g_sxActDur-$s_ad)     "active duration envelopes"
printf "  POKEY writes %lu (%lu%% changed)   update_paula_channel %lu calls, %lu.%02lu t/call\n", \
  (g_pokeyWrites-$s_pw), \
  ((g_pokeyWrites-$s_pw) ? (100*(g_pokeyChanged-$s_pwc))/(g_pokeyWrites-$s_pw) : 0), \
  (g_upcCalls-$s_upcC), \
  ((g_upcCalls-$s_upcC) ? (g_pUPC-$s_upcT)/(g_upcCalls-$s_upcC) : 0), \
  ((g_upcCalls-$s_upcC) ? ((100*(g_pUPC-$s_upcT))/(g_upcCalls-$s_upcC))%100 : 0)
printf "\n=== build_poly_dist (1022-byte rebuild) : %lu calls in-window (0 = path not taken) ===\n", \
  g_polyDistCalls
detach
quit
