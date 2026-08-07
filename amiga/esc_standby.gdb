# "ESC in Standby soft-hangs on the way into flight" repro / verification.
#
# Build: make clean && make -j4 PROBES=1 FORCE_ESC_STANDBY=1
# Run:   . ./env.sh && GDBSCRIPT=esc_standby.gdb ./diag_run.sh 120
#
# FORCE_ESC_STANDBY injects ESC (KBCODE $1c) once while the STANDBY VBI ($52D7) is live, then
# the normal auto-launch drives START.  Only the flight VBI's $519c CLI window consumes the
# pending keycode, so on an unfixed build the ESC is delivered on the first flight VBI firing
# and dispatches the freeze takeover:
#
#   BROKEN : VVBLKI reaches $4ff5 but $0043 (event_active_flag) latches non-zero and the
#            painted-frame counter STOPS advancing (the main loop is parked in the
#            `while (event_active_flag)` spin) -> the soft hang.
#   FIXED  : $0043 stays 00 and the painted-frame counter climbs normally.
set pagination off
set confirm off

define snap
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u  VVBLKI=$%02x%02x  $0043=%02x  $0004=%02x  $003D=%02x  fd=%lu\n", \
    g_vbiCount, mem[0x223], mem[0x222], mem[0x43], mem[0x04], mem[0x3D], g_fdCalls
end

snap 300
snap 1000
snap 1500
snap 1800
snap 2000
snap 2200
snap 2400
snap 2600
snap 2800
snap 3000
detach
quit
