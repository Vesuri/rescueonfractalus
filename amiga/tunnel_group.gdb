# Per-GROUP cost of the tunnel ring draw+paint (the "outermost ring's drawing is clearly visible"
# report).  One draw_symmetric_span_loop call = ONE ring group of $6E0F[i] nested rectangles in ONE
# colour, run from the 50 Hz VBI ISR: over 313 ticks it drops a displayed frame.
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=tunnel_group.gdb ./diag_run.sh 130
set pagination off
continue
echo \n==== SIGINT ====\n
printf "=== per-SITE ring-group cost (1 tick = 1 raster line = 63.56us; 313 ticks = 1 PAL frame) ===\n"
printf "site                       calls  rects   totT   plotT  paintT  maxT   t/call\n"
set $i = 0
while $i < 5
  if g_tgSiteCalls[$i] > 0
    printf "  %d %-22s %5lu  %5lu  %5lu  %5lu  %5lu  %5lu  %6lu\n", $i, \
      ($i==1 ? "pre-draw L_6047" : ($i==2 ? "forward descent" : ($i==3 ? "boost reverse ring" : ($i==4 ? "pre-build (Amiga)" : "doors/other")))), \
      g_tgSiteCalls[$i], g_tgSiteRects[$i], g_tgSiteTot[$i], g_tgSitePlot[$i], \
      g_tgSitePaint[$i], g_tgSiteMax[$i], \
      g_tgSiteTot[$i] / g_tgSiteCalls[$i]
  end
  set $i = $i + 1
end
printf "=== the two cinematics' groups, in order (site 2 = forward descent, 3 = boost reverse) ===\n"
printf "groups logged=%lu\n", g_tgN
printf "   #  site  vbi   rects  ticks  plot  paint  frames\n"
set $i = 0
while $i < 40 && $i < g_tgN
  printf "  %2d   %d   %5u   %3u   %5lu %5lu  %5lu   %2lu.%02lu\n", $i, g_tgSite[$i], g_tgVbi[$i], \
    g_tgRects[$i], g_tgTot[$i], g_tgPlot[$i], g_tgPaint[$i], \
    g_tgTot[$i] / 313, ((g_tgTot[$i] % 313) * 100) / 313
  set $i = $i + 1
end
printf "=== plot_terrain_span (the reveal's OTHER span source, emit_dl_coord_pairs tail) ===\n"
printf "calls=%lu  totT=%lu  plotT=%lu  paintT=%lu  maxT=%lu\n", g_tpCalls, g_tpTot, g_tpPlot, g_tpPaint, g_tpMax
if g_tpCalls > 0
  printf "t/call=%lu\n", g_tpTot / g_tpCalls
end
printf "=== span volume (both fields) ===\n"
printf "horiz calls=%lu cols=%lu   vert calls=%lu rows=%lu\n", g_dfHCalls, g_dfHCols, g_dfVCalls, g_dfVRows
printf "=== context ===\n"
printf "vbi=%u ring rects=%lu door rects=%lu  liveCopper=%u\n", g_vbiCount, g_trCalls, g_trDoors, g_liveCopper
printf "reveal K: min=%u  boost vbi %u..%u  changes=%u\n", g_rkMin, g_rkFirstVbi, g_rkLastVbi, g_rkN
set $i = 0
while $i < 48 && $i < g_rkN
  printf "    vbi %5u  K=%2u  held %3u frames\n", g_rkVbi[$i], g_rkK[$i], g_rkHold[$i]
  set $i = $i + 1
end
printf "=== reserved window $18C0-$18CF at the first two $1000 viewport decodes ===\n"
printf "(the ONLY part of the stars/planet read range nothing overwrites first, so the only place\n"
printf " the tunnel field's residue is observable.  MUST be identical to a make TUNPLOT=1 run.)\n"
printf "snaps=%u\n", g_tkN
set $j = 0
while $j < 2 && $j < g_tkN
  printf "  vbi %5u :", g_tkVbi[$j]
  set $i = 0
  while $i < 16
    printf " %02x", g_tkWin[$j][$i]
    set $i = $i + 1
  end
  printf "\n"
  set $j = $j + 1
end
