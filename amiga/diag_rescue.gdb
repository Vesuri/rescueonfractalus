# Pilot-rescue resume-frame dot-dropout ring dump.  Requires out/RoF built with
# `make PROBES=1` (ROF_FLIGHT_PROBE) — these globals only exist in that build.
#
# WORKFLOW (interactive — a rescue can't be reached headlessly):
#   1. cd amiga && make clean && make -j4 PROBES=1
#   2. ./debug.sh          # boots under the FS-UAE gdb stub; in the gdb console type `continue`
#   3. Play: fly to a downed pilot, land, systems OFF -> figure zoom -> systems ON (resume).
#      The ring auto-FREEZES ~24 renderFlightDirect calls after the pause ends (g_rfFrozen=1),
#      so the rescue+resume window is preserved.
#   4. In the gdb console: Ctrl-C to break, then:  source diag_rescue.gdb
#
# Records are a ring of RF_RING_N slots dumped in PHYSICAL order; sort by FRAME when reading.
# Columns: FRAME  PATH(1=pause 2=hold 3=normal)  RESC  3D 3E  DISP BACK CLR (0=terrainBitmap
# 1=terrainBitmapBack 2=null)  FRESH  FIG[lo,hi]  | A:p1/p2  B:p1/p2  (byte-sum checksums of
# terrainBitmap(A)/terrainBitmapBack(B) plane1/plane2 — plane2 = terrain dots; a drop to ~0 on
# the displayed buffer's plane2 == the dot dropout).
printf "=== rescue ring: idx=%u count=%lu frozen=%u sawRescue=%u postResume=%u (N=%u) ===\n", \
  g_rfIdx, g_rfCount, g_rfFrozen, g_rfSawRescue, g_rfPostResume, g_rfN
printf "slot FRAME PATH RESC WASR 3D 3E DISP BACK CLR FRESH FIGlo FIGhi  A:p1/p2      B:p1/p2      Scln:p1/p2\n"
set $i = 0
while $i < 128
  printf "%3d %6lu  %d    %d    %d    %02x %02x   %d    %d    %d    %d    %4d %4d  %7lu/%-6lu %7lu/%-6lu %7lu/%-6lu\n", \
    $i, g_rfFrame[$i], g_rfPath[$i], g_rfResc[$i], g_rfWasR[$i], g_rf3D[$i], g_rf3E[$i], \
    g_rfDisp[$i], g_rfBack[$i], g_rfClr[$i], g_rfFresh[$i], g_rfFigLo[$i], g_rfFigHi[$i], \
    g_rfP1a[$i], g_rfP2a[$i], g_rfP1b[$i], g_rfP2b[$i], g_rfScP1[$i], g_rfScP2[$i]
  set $i = $i + 1
end
printf "=== end ring (sort rows by FRAME; watch p2 of the DISP buffer collapse on the resume frame) ===\n"
