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
printf "--- live state right now ---\n"
printf "  0633=%02x 003C=%02x 003D=%02x 003E=%02x\n", \
  mem[0x0633], mem[0x003C], mem[0x003D], mem[0x003E]
printf "--- rescueFigure gate (Amiga composite): needs 003E!=0 && 003D>=3 ---\n"
printf "  g_figRowLo=%d g_figRowHi=%d  (live overlay extent)\n", g_figRowLo, g_figRowHi
printf "===================================\n"
