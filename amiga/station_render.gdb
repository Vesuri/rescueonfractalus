# station_render.gdb — is the Amiga station render path actually doing its job?
# Works on a PLAIN build.  Run:  . ./env.sh && GDBSCRIPT=station_render.gdb ./diag_run.sh <secs>
#
# What to read:
#   bootFieldScene 2 = the station's layout is live (1 = the logo's).
#   winRow must track (($1C39/$1C3A) - $B800) / 3 — that IS the scroll.
#   starRows = how many of the 340 display-list rows are star rows (display_list_build caps
#     them at 30 via encounter_count = $1E); 0 would mean the LMS range test missed them.
#   nonZero  = non-zero bytes in a 4 KB slice of the decoded field.  0 = the decode never ran
#     or decoded blank source; the station hull should light up thousands of them.
continue
echo \n==== SIGINT ====\n
# ⚠ AFTER the continue: gdb evaluates a convenience variable EAGERLY, and s_scene is still
# null at the remote_debugger_trigger breakpoint (PlatformAmiga::run has not set it yet), so
# setting $sc above the continue silently reads the whole scene from address 0.
set $sc = 'PlatformAmiga.cpp'::s_scene
printf "bootScene=%u bootFieldScene=%u installed=%u  vbiCount=%u\n", \
  g_bootScene, $sc->bootFieldScene, $sc->bootFieldCopperInstalled, g_vbiCount
printf "scrollPtr=%02x%02x  winRow=%u  expect=%u\n", \
  mem[0x1C3A], mem[0x1C39], $sc->stationWindowRow, \
  ((mem[0x1C3A]*256 + mem[0x1C39]) - 0xB800) / 3
printf "starRows=%u  dirtyCount=%u dirtyFull=%u  phase(8B)=%02x\n", \
  $sc->stationStarRows, g_stationDirtyCount, g_stationDirtyFull, mem[0x008B]
printf "bitmap=%p  copper=%p\n", $sc->bootFieldBitmap->data, $sc->bootFieldCopper->data_
# Count non-zero bytes over rows 0..25 of the decoded field (26 * 160 = 4160 bytes).
set $p = (unsigned char *)$sc->bootFieldBitmap->data
set $i = 0
set $nz = 0
while $i < 4160
  if $p[$i] != 0
    set $nz = $nz + 1
  end
  set $i = $i + 1
end
printf "nonZero(first 26 rows)=%u / 4160\n", $nz
# The copper list's window bitplane pointers (INDEX_WIN_BPL = 62) — 4 planes, hi/lo MOVE pairs.
set $d = (unsigned long *)$sc->bootFieldCopper->data_
printf "WIN_WAIT=%08x  BPL1=%08x %08x  BPL2=%08x %08x\n", $d[59], $d[62], $d[63], $d[64], $d[65]
printf "TOP_WAIT n/a  BOT_WAIT=%08x  term=%08x\n", $d[70], $d[81]
echo ==== end ====\n
