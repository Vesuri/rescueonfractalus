# Does the joystick FIRE button advance Logo -> Station -> Standby?  (user QoL request, 2026-08-14)
#
# Build: `make clean && make PROBES=1 SKIPBOOT=0 FORCE_BOOT_FIRE=1`
#   SKIPBOOT=0 is REQUIRED — PROBES alone IMPLIES SKIPBOOT, which skips the very scenes under test.
#   FORCE_BOOT_FIRE holds fire over vbi 100..160 (mid-Logo, on into the Station), then releases.
# Run: `GDBSCRIPT=boot_fire.gdb ./diag_run.sh 14`
#
# ⚠ Fire is injected from C, NOT from this script: a `set var` on the flag did not stick — the
# FS-UAE stub serves memory reads but drops writes (measured 2026-08-14).  Same reason every FORCE_*
# harness drives its input from C.
#
# Pass criteria, against the measured no-press CONTROL (bootScene=1/VVBLKI=$51EF at 10 s; then
# bootScene=2/VVBLKI=$1B30, phase $94, STILL waiting in the Station's RTCLOK timeout at 26 s):
#   logoEnd     — vbl the Station's vector went live = the Logo handed off.  Want ~100-110 (the
#                 press), NOT ~280 (the Logo's natural length).
#   stationEnd  — vbl the Station handed off.  Want just after logoEnd, NOT the RTCLOK timeout
#                 (~vbi 1300+, which is what the control run sits in).
#   bootScene   — 0, and VVBLKI past $1B30 ($52D7 Standby or beyond).
# Both stamps are taken off the live VVBLKI every vblank, so they measure the transitions rather
# than inferring them from wherever the sample landed.
set width 0
continue
echo \n==== SIGINT ====\n
printf "logoEnd=%u  stationEnd=%u  (control: ~280 and ~1300+)\n", g_bfLogoEnd, g_bfStationEnd
printf "now: bootScene=%u (want 0) VVBLKI=%02x%02x (want past 1b30) vbi=%u RTCLOKmid=%02x\n", \
  g_bootScene, mem[0x0223], mem[0x0222], g_vbiCount, mem[0x0013]
printf "  station phase(8B)=%02x  game state(41)=%02x  consol=%02x joyTrig0=%02x\n", \
  mem[0x008B], mem[0x0041], 'PlatformAmiga.cpp'::s_consolState, 'PlatformAmiga.cpp'::s_joyTrig0
printf "  joy polls=%lu portaStuck=%02x b2Edges=%lu\n", g_joyPolls, g_joyPortaStuck, g_joyB2Edges
detach
quit
