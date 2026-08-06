# SPRITE SHAPE PROBE — what is the flight VBI's 29 t/firing SPRITE bracket actually doing?
#
# Context (b8637d8): game_vbi_isr brackets its three parts separately — handler
# (g_flightProf.isrLines) + sprites (g_vbiSpriteLines) + audio (g_vbiAudioLines).  The SPRITE
# bracket measured 29.06 t/firing = ~9% of ALL wall clock (the ISR fires 50x/s regardless of frame
# rate) and contains exactly two functions, neither ever profiled:
#     RescueOnFractalus::buildShotSprite()        (the player laser, Atari P2 $0E32 -> sprite ch4)
#     RescueOnFractalus::decodeScannerBlinkCells() (LR-scanner blink, 2 mode-4 cells)
# 29 t = ~13000 cycles, far more than either LOOKS like it should cost, so this splits it.
#
# ⚠ Read the LAP FLOOR line first.  Each SP_LAP is one rof_beam_line() + an accumulate; on a
# population this small the floor is a real fraction of a cheap part (the SFX_SHAPE lesson:
# nested brackets inflate the whole, so use the split as SHARES, never as absolutes).
#
# Build: make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1 SPRITE_SHAPE=1
# Run:   . ./env.sh && GDBSCRIPT=sprite_shape.gdb ./diag_run.sh 240
#
# ⚠ COMBAT implies AUTO_FIRE, so a shot is live almost every frame — that is the WORST case for
# buildShotSprite.  Re-run with COMBAT_QUIET=1 to see the idle-path cost, and compare: if the
# bracket is ~as expensive quiet, the cost is NOT the shot decode.
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u  $0623 emplace=%02x  $0621 saucer=%02x  (start vbi=%u) ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], g_vbiCount

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u painted %4u | VVBLKI=$%02x%02x $3D=%02x | shot $36=%02x $37=%02x $CB=%02x | range $0642=%02x\n", \
    g_vbiCount, g_fdCalls, mem[0x223], mem[0x222], mem[0x3D], \
    mem[0x36], mem[0x37], mem[0xCB], mem[0x642]
end

# ⚠ SNAPSHOT FIRST — every g_sp* counter accumulates from POWER-ON while g_flightProf.isrCalls
# counts FLIGHT firings only.  (Same scope trap that overstated the sfx cost by ~6%.)
seg 2600
set $s_isrn = g_flightProf.isrCalls
set $s_isrl = g_flightProf.isrLines
set $s_vsp  = g_vbiSpriteLines
set $s_vau  = g_vbiAudioLines
set $s_vfc  = g_vbiFullCalls
set $s_shc  = g_spShotCalls
set $s_snc  = g_spScanCalls
set $s_idle = g_spIdle
set $s_blk  = g_spBlank
set $s_act  = g_spActive
set $s_nrun = g_spNoRun
set $s_idlT = g_spIdleT
set $s_runT = g_spRunT
set $s_clrT = g_spClearT
set $s_decT = g_spDecT
set $s_copT = g_spCopT
set $s_scnT = g_spScanT
set $s_scnD = g_spScanDecodes
set $s_rows = g_spRows
set $s_topS = g_spTopSum
set $s_botS = g_spBotSum
set $s_nop  = g_spNop
set $s_nopT = g_spNopT
printf "  (snapshot in flight at vbi %u; everything below is a DELTA over this window)\n", g_vbiCount

seg 3300
seg 4000
seg 4700
seg 5400

set $n   = g_flightProf.isrCalls - $s_isrn
set $il  = g_flightProf.isrLines - $s_isrl
set $vsp = g_vbiSpriteLines - $s_vsp
set $vau = g_vbiAudioLines  - $s_vau
set $vfc = g_vbiFullCalls   - $s_vfc
printf "\n=== flight VBI over the in-flight window: %lu firings ===\n", $n
printf "  whole VBI = handler %lu.%02lu + sprites %lu.%02lu + audio %lu.%02lu = %lu.%02lu t/firing", \
  ($n ? $il/$n : 0), ($n ? ((100*$il)/$n)%100 : 0), \
  ($vfc ? $vsp/$vfc : 0), ($vfc ? ((100*$vsp)/$vfc)%100 : 0), \
  ($vfc ? $vau/$vfc : 0), ($vfc ? ((100*$vau)/$vfc)%100 : 0), \
  ($vfc ? ($il+$vsp+$vau)/$vfc : 0), ($vfc ? ((100*($il+$vsp+$vau))/$vfc)%100 : 0)
printf "  (%lu%% of ALL wall clock)\n", ($vfc ? (100*($il+$vsp+$vau))/(313*$vfc) : 0)
printf "  SPRITE bracket = %lu t over %lu firings = %lu.%02lu t/firing = %lu%% of all wall clock\n", \
  $vsp, $vfc, ($vfc ? $vsp/$vfc : 0), ($vfc ? ((100*$vsp)/$vfc)%100 : 0), \
  ($vfc ? (100*$vsp)/(313*$vfc) : 0)

set $nop  = g_spNop  - $s_nop
set $nopT = g_spNopT - $s_nopT
set $fl   = $nop ? (1000*$nopT)/$nop : 0
printf "\n=== LAP FLOOR: %lu empty laps, %lu.%03lu t each — subtract ONE per lap below ===\n", \
  $nop, $fl/1000, $fl%1000

set $shc = g_spShotCalls - $s_shc
set $snc = g_spScanCalls - $s_snc
set $idl = g_spIdle   - $s_idle
set $blk = g_spBlank  - $s_blk
set $act = g_spActive - $s_act
set $idlT = g_spIdleT  - $s_idlT
set $runT = g_spRunT   - $s_runT
set $clrT = g_spClearT - $s_clrT
set $decT = g_spDecT   - $s_decT
set $copT = g_spCopT   - $s_copT
set $scnT = g_spScanT  - $s_scnT
set $shotT = $idlT + $runT + $clrT + $decT + $copT

printf "\n=== the two callees (whole-body, lap sums) ===\n"
printf "  buildShotSprite        : %6lu calls  %7lu t  %lu.%02lu t/call  %lu.%02lu t/firing  %2lu%% of the bracket\n", \
  $shc, $shotT, ($shc ? $shotT/$shc : 0), ($shc ? ((100*$shotT)/$shc)%100 : 0), \
  ($vfc ? $shotT/$vfc : 0), ($vfc ? ((100*$shotT)/$vfc)%100 : 0), ($vsp ? (100*$shotT)/$vsp : 0)
printf "  decodeScannerBlinkCells: %6lu calls  %7lu t  %lu.%02lu t/call  %lu.%02lu t/firing  %2lu%% of the bracket\n", \
  $snc, $scnT, ($snc ? $scnT/$snc : 0), ($snc ? ((100*$scnT)/$snc)%100 : 0), \
  ($vfc ? $scnT/$vfc : 0), ($vfc ? ((100*$scnT)/$vfc)%100 : 0), ($vsp ? (100*$scnT)/$vsp : 0)
printf "    ...of which decoded (value flipped): %lu (%lu%% of calls)\n", \
  (g_spScanDecodes - $s_scnD), ($snc ? (100*(g_spScanDecodes-$s_scnD))/$snc : 0)
printf "  unaccounted (call plumbing + bracket floors): %lu t = %lu%% of the bracket\n", \
  ($vsp > ($shotT+$scnT) ? $vsp - $shotT - $scnT : 0), \
  ($vsp ? (100*($vsp > ($shotT+$scnT) ? $vsp-$shotT-$scnT : 0))/$vsp : 0)

printf "\n=== buildShotSprite: which PATH ===\n"
printf "  idle (mem[$0036]==0, early out) : %6lu (%lu%%)   of which the once-off BLANK: %lu\n", \
  $idl, ($shc ? (100*$idl)/$shc : 0), $blk
printf "  active (full rebuild)           : %6lu (%lu%%)   of which found NO run: %lu\n", \
  $act, ($shc ? (100*$act)/$shc : 0), (g_spNoRun - $s_nrun)

printf "\n=== buildShotSprite: the ACTIVE path, part by part (net of one lap floor per call) ===\n"
define part
  set $raw = $arg0
  set $cnt = $arg1
  set $net = $raw > ($fl*$cnt)/1000 ? $raw - ($fl*$cnt)/1000 : 0
  printf "  %-34s %7lu t raw  %7lu t net  %lu.%02lu t/call  %2lu%% of buildShotSprite\n", \
    $arg2, $raw, $net, ($cnt ? $net/$cnt : 0), ($cnt ? ((100*$net)/$cnt)%100 : 0), \
    ($shotT ? (100*$net)/$shotT : 0)
end
part $runT $act "94-byte run scan (no early exit)"
part $clrT $act "clear all 64 sprite words"
part $decT $act "row decode + setX/setY"
part $copT $act "setHudSprite + setShotColor"
part $idlT $idl "idle early-out"

printf "\n=== what the run scan finds (active calls only) ===\n"
set $rows = g_spRows - $s_rows
printf "  rows decoded: %lu total, %lu.%02lu mean, %lu max (of %d scanned)\n", \
  $rows, ($act ? $rows/$act : 0), ($act ? ((100*$rows)/$act)%100 : 0), g_spRowsMax, 94
printf "  mean top = $%02lx, mean bot = $%02lx  (scan window is $34..$91)\n", \
  ($act ? (g_spTopSum-$s_topS)/$act : 0), ($act ? (g_spBotSum-$s_botS)/$act : 0)
printf "  => an early exit after the run would skip ~%lu of 94 bytes/call\n", \
  ($act ? (94 - (((g_spBotSum-$s_botS)/$act) - 0x34 + 1)) : 0)
detach
quit
