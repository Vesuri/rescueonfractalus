set pagination off
set confirm off
set height 0
# Statistical PC sampling: each SIGINT (sent by the harness) stops the inferior; print
# where the main thread is, then continue. Run across the launch door window.
# NOTE for flight PC-profiling: build with `-DROF_PROFILE_NORING` (e.g.
#   make PROBES=1 OPT="-O2 -DROF_PROFILE_NORING" NATIVE_OPT="-O3 -DROF_PROFILE_NORING"),
# else the ROF_FLIGHT_PROBE rescue-debug ring (rfPlaneSum, 4x full-plane byte-sum walks
# per frame — it never freezes in steady flight) eats ~half the samples and swamps the
# profile.  (Setting g_rfFrozen=1 from gdb does NOT stick on the remote target.)
set $i = 0
while $i < 400
  continue
  printf "S%d vbi=%u 060B=%u 004A=%u iter=%u pc=0x%x SYM=", $i, g_vbiCount, mem[0x060B], mem[0x004A], g_iterCount, $pc
  info symbol $pc
  set $i = $i + 1
end
detach
quit
