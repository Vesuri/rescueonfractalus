# CD32 gamepad check.  Needs `make PROBES=1`; add `CD32_FORCE=1` to exercise the READ itself on a
# host with no emulated pad (FS-UAE without a configured CD32 controller fails the presence check,
# so present= would otherwise be 0 and the read would never run).
#
# The two questions it answers:
#
#   1. Detection.  present= is what the ~2.5 s detection window latched for the whole session, and
#      latched-at names the vbl.  The last three SAMPLES and the agreeing streak are printed too,
#      because the verdict alone cannot distinguish "no pad" from "a pad that never agreed".  With
#      nothing (or a plain joystick) in port 1 present MUST be 0 and the samples read 0000 — a plain
#      stick leaves DATRY high, so the marker bits fail.
#   2. Cost.  lines/linesMax time one read against the real beam.  It runs inside the vblank ISR,
#      where overrunning the frame silently DROPS a displayed frame, so this is the number to quote
#      — never the ~100-200 us the instruction count suggests.  PAL is 312 lines/frame, and the ISR
#      does much else besides, so anything past a few lines wants looking at.
#
#   word/bad — the last per-frame word and how many reads failed the presence check mid-session.
#              With CD32_FORCE=1 and no pad, bad== reads is the CORRECT result (all released).
#
# It CANNOT confirm the BUTTON MAP: that needs a physical pad (or an FS-UAE-mapped one) and a human
# pressing them.  The map is docs/controls.md §CD32.
set width 0
continue
echo \n==== SIGINT ====\n
printf "cd32 present=%u (latched at vbl %u)  last 3 detect samples=%04x %04x %04x  streak=%u\n", \
  g_cd32Present, g_cd32Latch, g_cd32Probe[0], g_cd32Probe[1], g_cd32Probe[2], g_cd32Streak
printf "  per-frame reads=%lu  bad=%lu  lastWord=%04x\n", g_cd32Reads, g_cd32Bad, g_cd32Word
printf "  read cost: last=%u lines  max=%u lines  (PAL frame = 312)\n", g_cd32Lines, g_cd32LinesMax
printf "live TRIG0 joystick side=%02x  pendingFlightKey=%02x (FF = none)  CONSOL=%02x\n", \
  'PlatformAmiga.cpp'::s_joyTrig0, 'PlatformAmiga.cpp'::s_pendingFlightKey, \
  'PlatformAmiga.cpp'::s_consolState
printf "  vbi=%u\n", g_vbiCount
detach
quit
