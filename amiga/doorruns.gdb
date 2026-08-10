# WHEN does the level-select door scroll rewrite the LIVE standby copper list?
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=doorruns.gdb ./diag_run.sh 150
#
# doorScrollVblankUpdate() runs in the INTB_VERTB ISR and rewrites StandbyCopperList's terrain runs
# + the cockpit region IN PLACE, on the list the copper is currently executing.  That is only safe
# while the beam is still ABOVE the region being rewritten.  The cockpit region's WAIT is
# kCockpitLine-1 = 171; a rewrite landing at/after line 171 writes the cockpit's 6 BPLxPT moves
# behind the beam, so the already-satisfied WAIT fires mid-line -> cockpit shifted horizontally for
# one frame.  That is the reported "one misaligned-cockpit frame between levels".
#
# Read: late>0 means the bug fired.  The ring correlates each rewrite's beam line with whether the
# ~11 ms full-field decode (dec=1, i.e. the LEVEL digit changed that frame) ran ahead of it.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "setTerrainRuns writes=%u  late(>=171)=%u  worst beam line=%u  full-field decodes=%u\n", g_dsRunWrites, g_dsRunLate, g_dsRunMaxLn, g_dsDecodes
printf "door decode cost: last=%u lines  MAX=%u lines  (PAL frame = 312; entry line=%u, rows=%u)\n", g_dsDecLines, g_dsDecLinesMax, g_dsDecEntryLn, g_dsDecRows
printf "standby VBI ISR cost: last=%u lines  MAX=%u lines  over-a-frame=%u of %u  (PAL = 312)\n", g_isrStbyLines, g_isrStbyMax, g_isrStbyOver, g_isrStbyCnt
printf "decodes (last 24) as rows/why -- 8 = a LEVEL-digit rewrite, 85 = whole field;\n"
printf "  why: 1=dirty,no range  2=dirty,marked range  3=renderFrame terrainDirty\n  "
set $j = 0
while $j < 24
  printf "%u/%u ", g_dsDecRing[$j], g_dsDecWhyRing[$j]
  set $j = $j + 1
end
printf "\n(next decode slot=%u)\n", g_dsDecRingIdx
printf "ring (newest last), line = beam line at the rewrite, dec = full decode ran first:\n"
set $i = 0
while $i < 24
  if g_dsRunLine[$i] != 0
    printf "  [%2d] line=%3u  dec=%u  runs=%u\n", $i, g_dsRunLine[$i], g_dsRunDec[$i], g_dsRunN[$i]
  end
  set $i = $i + 1
end
printf "next ring slot=%u\n", g_dsRunIdx
