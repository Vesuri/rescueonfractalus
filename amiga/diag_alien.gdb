# Alien-attack render diagnosis — read-back for the FORCE_ALIEN=1 PROBES=1 build.
#
# Workflow:
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_ALIEN=1
#   ./debug.sh                        # gdb connects to the FS-UAE stub
#   (gdb) continue                    # display goes live; PLAY the game
#   ... fly to a downed pilot, land, systems OFF, open the AIRLOCK (A key) ...
#   ... the rescue always resolves to the alien (FORCE_ALIEN) — SFX + shake fire ...
#   Ctrl-C in the gdb terminal to break, then:
#   (gdb) source diag_alien.gdb
#
# Interpretation:
#   g_alSeen=1                 -> the alien-attack state ($0633!=0) was entered (good).
#   g_alPlotCalls > 0          -> the creature IS drawn as a BITMAP via plot_clipped_pixel
#                                 (so the fix is in the Amiga composite path / the rescueFigure gate).
#   g_alPlotCalls == 0         -> it is NOT a bitmap: drawn via PMG / another primitive (probe further).
#   g_alDrawShape / g_alShapePtr -> whether the zoom ran during the attack, and which shape ($C3/$C4).
#   g_alFigLo/Hi               -> the figure-overlay row extent captured during the attack.
set pagination off
set confirm off
printf "==== ALIEN-ATTACK RENDER PROBE ====\n"
printf "g_forceAlienRescue = %u   (must be 1)\n", g_forceAlienRescue
printf "g_forceAirlockOpen = %u   (1 => airlock auto-opened at the knock; reveal reachable headless)\n", g_forceAirlockOpen
printf "-- rescue-wide (whole systems-off rescue, $003E!=0) --\n"
printf "g_alRescueSeen=%u  g_alRescueFrames=%lu  maxPhase($3D)=%02x  airlock($3C) seen=%u max=%02x\n", \
  g_alRescueSeen, g_alRescueFrames, g_alMaxPhase, g_alAirlock, g_alAirlockMax
printf "g_alRescuePlot=%lu  (plot_clipped_pixel calls across the whole rescue; pilot walk => >0)\n", g_alRescuePlot
printf "-- alien-specific ($0633!=0) --\n"
printf "g_alSeen=%u  g_alFrames=%lu\n", g_alSeen, g_alFrames
printf "g_alPlotCalls=%lu  (>0 => alien drawn as BITMAP via plot_clipped_pixel)\n", g_alPlotCalls
printf "g_alDrawShape=%lu  g_alShapePtr=$%04X\n", g_alDrawShape, g_alShapePtr
printf "g_alFig rows: lo=%d hi=%d\n", g_alFigLo, g_alFigHi
printf "-- distinct draw_scaled_shape ptrs during the rescue (figure frames = $7DEF/$7E25/$7E5B/$7E91) --\n"
printf "g_alShapeCount=%u :", g_alShapeCount
set $i = 0
while $i < g_alShapeCount
  printf " $%04X", g_alShapes[$i]
  set $i = $i + 1
end
printf "\n  (any ptr OUTSIDE the 4 figure frames => the creature IS a bitmap; only those 4 => PMG/other)\n"
printf "--- latest per-frame snapshot (g_alSnap) ---\n"
printf "  003C(anim)=%02x 003D(phase)=%02x 003E(sysoff)=%02x 0633(alien)=%02x\n", \
  g_alSnap[0], g_alSnap[1], g_alSnap[2], g_alSnap[3]
printf "  0044(timer/shape)=%02x 0047(colpf0/shape)=%02x 003A=%02x 283D(phase)=%02x\n", \
  g_alSnap[4], g_alSnap[5], g_alSnap[6], g_alSnap[7]
printf "  0079(range)=%02x 0041(gamestate)=%02x 2844=%02x 00DB=%02x\n", \
  g_alSnap[8], g_alSnap[9], g_alSnap[10], g_alSnap[11]
printf "  figLo=%02x figHi=%02x shapePtr=$%02x%02x\n", \
  g_alSnap[12], g_alSnap[13], g_alSnap[15], g_alSnap[14]
printf "-- creature blit ($80C5) capture — the jump-scare draw (airlock-CLOSED path) --\n"
printf "g_alCrSeen=%u  g_alCrWrites=%lu  (>0 => the creature DID blit into the field on Amiga)\n", \
  g_alCrSeen, g_alCrWrites
printf "g_alCrAddr range = $%04X .. $%04X   valOR=$%02x  (which mode-D pixel bits used)\n", \
  g_alCrAddrLo, g_alCrAddrHi, g_alCrValOr
printf "g_alCrRowBase=$%04X  rowStride=%d  pos $2930=%02x $2931=%02x\n", \
  g_alCrRowBase, g_alCrRowStride, g_alCrPos0, g_alCrPos1
printf "  (field base $1010? stride 96? -> row=(addr-base)/stride; half=(addr-base)%%stride)\n"
printf "-- creature COLOUR diagnosis (why invisible live) --\n"
printf "g_alKnockFrames=%lu  g_alComp=%lu  (comp>0 => creature IS composited+flipped LIVE => palette issue)\n", \
  g_alKnockFrames, g_alComp
printf "renderFrame during knock: g_alRF=%lu  VVBLKI=$%04x (want 4FF5)  rsFlight=%u  rsViewport=%u\n", \
  g_alRF, g_alVV, g_alRFfl, g_alRFvw
printf "-- knock STEP timing (beam ticks; 313=1 frame=20ms; a step should be ~5 frames=1565) --\n"
printf "steps=%lu  wait=%lu  draw(7F85)=%lu  render=%lu  (totals)\n", \
  g_alKnockFrames, g_alTWait, g_alTDraw, g_alTRender
printf "  per-step avg: wait=%lu draw=%lu render=%lu ticks  (draw>>1565 => native-twin 7F85; render>>1565 => render)\n", \
  (g_alKnockFrames ? g_alTWait/g_alKnockFrames : 0), \
  (g_alKnockFrames ? g_alTDraw/g_alKnockFrames : 0), \
  (g_alKnockFrames ? g_alTRender/g_alKnockFrames : 0)
printf "  hud split: g_alHudCalls=%lu  g_alTHud=%lu ticks  (hud vs wrapper: draw-hud = wrapper cost)\n", \
  g_alHudCalls, g_alTHud
printf "  per-step: hud=%lu ticks (%lu calls/step)  wrapper=%ld ticks  hud/draw=%lu%%\n", \
  (g_alKnockFrames ? g_alTHud/g_alKnockFrames : 0), \
  (g_alKnockFrames ? g_alHudCalls/g_alKnockFrames : 0), \
  (g_alKnockFrames ? ((long)g_alTDraw-(long)g_alTHud)/(long)g_alKnockFrames : 0), \
  (g_alTDraw ? (g_alTHud*100)/g_alTDraw : 0)
printf "    (wrapper<0 / hud>100%% is EXPECTED on a short capture, NOT a measurement of the wrapper:\n"
printf "     g_alTHud also counts the 4 un-bracketed alien_creature_animate_draw calls that\n"
printf "     alien_knock_setup_loop makes BEFORE the timed loop, while g_alTDraw brackets only the loop.\n"
printf "     Their weight decays as steps grow; only judge the split on a full ~50-step knock.)\n"
printf "renderFlightDirect during knock: g_alRFD=%lu  rescueFig=%lu  cleanValid=%lu  (locates the drop-out)\n", \
  g_alRFD, g_alRFDresc, g_alRFDclean
printf "  render split: scene(composite+flipwait)=%lu  frameSync(vbi==last wait)=%lu ticks/step\n", \
  (g_alKnockFrames ? g_alTRScene/g_alKnockFrames : 0), \
  (g_alKnockFrames ? g_alTRIdle/g_alKnockFrames : 0)
printf "    of scene: flipWait(while flightSwapPending)=%lu ticks/step  (rest = composite + OTHER renderFrame work)\n", \
  (g_alKnockFrames ? g_alTFlipWait/g_alKnockFrames : 0)
printf "    of scene: composite+drain(blitter)=%lu ticks/step  => scene - flipWait - composite = OTHER renderFrame work\n", \
  (g_alKnockFrames ? g_alTComp/g_alKnockFrames : 0)
printf "    bitmap addrs (want <0x200000 => blitter path, not CPU fallback): fig=$%08X clean=$%08X mask=$%08X\n", \
  g_figBmpAddr, g_cleanBmpAddr, g_maskBmpAddr
printf "    flightVblankSwap during knock: ran=%lu  didFlip=%lu  (ran/step=%lu; if ran>>steps ISR is live)\n", \
  g_alVSwapRun, g_alVSwapCleared, (g_alKnockFrames ? g_alVSwapRun/g_alKnockFrames : 0)
printf "viewport pens during knock: $DA=%02x $DB=%02x $DC=%02x $DD=%02x  attack $47=%02x $44=%02x\n", \
  g_alPen[0], g_alPen[1], g_alPen[2], g_alPen[3], g_alPen[4], g_alPen[5]
printf "  (Amiga color02<-$DA, color03<-$DB; if creature pens ~= sky/background, shape is invisible)\n"
printf "  as PUBLISHED to the copper: $DA=%02x $DB=%02x $DC=%02x $DD=%02x  text $D8=%02x  bandgrey $D4=%02x\n", \
  g_alPenPub[0], g_alPenPub[1], g_alPenPub[2], g_alPenPub[3], g_alPenPub[4], g_alPenPub[5]
printf "  g_alPenCalls=%lu of %lu knock frames reached updateFlightCopper; g_alPenChg=%lu moved a slot\n", \
  g_alPenCalls, g_alKnockFrames, g_alPenChg
printf "    (calls==frames && chg==0 => the palette really IS static through the scare, refresh is\n"
printf "     correctness-only.  calls==0 => the refresh never ran (flightCopperInstalled false) and the\n"
printf "     chg=0 is VACUOUS.  chg>0 => the old frozen-palette path was showing stale colours.)\n"
printf "--- live state right now ---\n"
printf "  0633=%02x 003C=%02x 003D=%02x 003E=%02x\n", \
  mem[0x0633], mem[0x003C], mem[0x003D], mem[0x003E]
printf "--- rescueFigure gate (Amiga composite): needs 003E!=0 && 003D>=3 ---\n"
printf "  g_figRowLo=%d g_figRowHi=%d  (live overlay extent)\n", g_figRowLo, g_figRowHi
printf "===================================\n"
