# BREAK-outside-flight regression check for the $5398 keyboard window.
#
# The $5398 poll (vbi_attract_poll) now consumes pending keycodes, which is what stops an ESC
# pressed in Standby from surviving into flight.  A pending BREAK ($80) must still get through:
# out of flight the restart is driven from MAIN-loop context by rof_check_restart, so
# rof_attract_poll_key deliberately leaves $80 pending.  This confirms it.
#
# Build: make clean && make -j4 PROBES=1 FORCE_BREAK_EARLY=1
# Run:   . ./env.sh && GDBSCRIPT=break_early.gdb ./diag_run.sh 150
#
# PASS: g_restartCount goes 0 -> >=1 after the vbi~240 BREAK injection, and VVBLKI lands back on
#       a real scene ($53CC card, or $52D7 after the auto-START rebuild) rather than sticking.
set pagination off
set confirm off

define snap
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u  VVBLKI=$%02x%02x  restarts=%u  $060B=%02x  $365B=%02x\n", \
    g_vbiCount, mem[0x223], mem[0x222], g_restartCount, mem[0x60B], mem[0x365B]
end

snap 200
snap 300
snap 400
snap 600
snap 900
snap 1200
snap 1600
snap 2000
detach
quit
