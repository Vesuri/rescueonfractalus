# Joystick (port 1) poll check.  Needs `make PROBES=1`.
#
# PRIMARY question this answers: with NOTHING plugged into port 1, is pollJoystick() perfectly
# quiet?  A stuck direction bit or a spurious Land injection would break keyboard play, and it is
# the realistic failure mode — JOY1DAT is a pair of quadrature counters (so a MOUSE in port 1 would
# decode as directions) and POTINP bit 14 floats with no button wired.
#
#   polls          — must be ~= the vbi count (proves the poll actually runs; a 0 proves nothing else)
#   portaStuck     — OR of every direction bit ever seen pressed.  MUST be 00.
#   b2Edges        — Land/Launch injections.  MUST be 0.
#   trigLow        — frames with fire held.  MUST be 0.
#   portaLast/raw  — last decoded PORTA and the raw JOY1DAT/POTINP words.
#
# It CANNOT confirm that a real stick steers correctly — that needs a physical joystick (or an
# FS-UAE-mapped pad) and a human at the controls.  See docs/controls.md.
set width 0
continue
echo \n==== SIGINT ====\n
printf "joy polls=%lu  portaStuck=%02x (want 00)  b2Edges=%lu (want 0)  trigLow=%lu (want 0)\n", \
  g_joyPolls, g_joyPortaStuck, g_joyB2Edges, g_joyTrigLow
printf "  portaLast=%02x (FF = centred)  rawJOY1DAT=%04x  rawPOTINP=%04x (bit14 set = button2 up)\n", \
  g_joyPortaLast, g_joyRawJoy, g_joyRawPot
printf "  vbi=%u  -> polls/vbi = %lu (want ~1)\n", g_vbiCount, (g_vbiCount ? g_joyPolls/g_vbiCount : 0)
printf "live PORTA hwRead would return: keyboard=%02x & joystick=%02x\n", \
  'PlatformAmiga.cpp'::s_portaState, 'PlatformAmiga.cpp'::s_joyPorta
printf "live TRIG0: keyboard=%02x & joystick=%02x | pendingFlightKey=%02x (FF = none)\n", \
  'PlatformAmiga.cpp'::s_trig0State, 'PlatformAmiga.cpp'::s_joyTrig0, \
  'PlatformAmiga.cpp'::s_pendingFlightKey
detach
quit
