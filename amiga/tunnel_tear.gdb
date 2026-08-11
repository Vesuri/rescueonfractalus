# Reverse-tunnel "multi-coloured rectangle edges" — is the ring paint racing the beam?
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=tunnel_tear.gdb ./diag_run.sh 130
#
# A pen is three PLANES.  While fillColor is writing them a pixel carries a MIX of the old and the
# new pen — a colour that is in neither image.  The tall vertical ring edges are the worst case
# (one 4-px column, up to 86 rows).  g_tbBeamIn counts the paints during which the beam swept
# through the rows being painted, i.e. the frames that can show the artifact.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "=== tunnel vertical-edge paints while tunnelBitmap is ON SCREEN ===\n"
printf "paints=%lu   beam inside the painted rows=%lu\n", g_tbCalls, g_tbBeamIn
if g_tbCalls > 0
  printf "paint length (raster lines): mean=%lu  max=%u\n", g_tbLinesSum / g_tbCalls, g_tbLinesMax
  printf "entry beam line: min=%u  max=%u   (terrain region = lines 86..171)\n", g_tbEntryMin, g_tbEntryMax
end
printf "samples (beam swept the painted rows):\n"
set $i = 0
while $i < 12 && $i < g_tbN
  printf "  [%2d] beam %3u..%-3u   painted lines %3u..%-3u\n", $i, \
    g_tbIn[$i], g_tbOut[$i], g_tbY0[$i], g_tbY1[$i]
  set $i = $i + 1
end
printf "=== reverse-reveal K (first viewport row taken from tunnelBitmap; 43 = none, 0 = full) ===\n"
printf "min K reached=%u   boost vbi %u..%u\n", g_rkMin, g_rkFirstVbi, g_rkLastVbi
printf "frames per K: "
set $i = 43
while $i >= 0
  if g_rkHist[$i] > 0
    printf "K%d:%lu  ", $i, g_rkHist[$i]
  end
  set $i = $i - 1
end
printf "\n"
printf "K timeline (one line per change; rings occupy viewport rows K..85-K, stars outside):\n"
printf "  changes=%u\n", g_rkN
set $i = 0
while $i < 48 && $i < g_rkN
  printf "    vbi %5u  K=%2u  held %3u frames\n", g_rkVbi[$i], g_rkK[$i], g_rkHold[$i]
  set $i = $i + 1
end
printf "=== context ===\n"
printf "vbi=%u  boostRet=%u boostVp=%u liveCopper=%u\n", g_vbiCount, g_boostRet, g_boostVp, g_liveCopper
printf "ring rects=%lu  door rects skipped=%lu\n", g_trCalls, g_trDoors
printf "standby ISR cost (raster lines): last=%u max=%u over=%u count=%lu\n", \
  g_isrStbyLines, g_isrStbyMax, g_isrStbyOver, g_isrStbyCnt
