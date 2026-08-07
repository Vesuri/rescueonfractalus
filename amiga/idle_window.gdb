# FRAME-SYNC SPIN — how much of flight wall clock does the main loop burn waiting for a vblank
# it did not need?  (idle_probe.gdb answers a different question: it prints totals from POWER-ON
# and divides idleWall by renderWall, which is the renderFrame subtotal, NOT wall clock.)
#
# PlatformAmiga::renderFrame ends with `while (g_vbiCount == last) {}`.  renderFlightDirect calls
# that PAINTED set flightFlipDeferred and skip it; every other flight renderFrame call falls
# through and blocks until the next vblank.  Those non-painting calls come from the transpiled
# code's SPINWAIT_HOOKS, which drive one Amiga frame per 6502 spin iteration.
#
# ⚠ Two corrections this script makes that the raw counter does not:
#   1. Denominator = the WINDOW's wall clock (vbi delta x 313 ticks), not renderWall.
#   2. g_rIdleWall counts beam ticks with the VBI ISR still firing inside the spin (the ISR is
#      what ends it).  That ISR would run anyway, so it is NOT recoverable: subtract it via
#      g_isrBeamLines-style accounting.  Here we bound it with the measured ISR cost per firing
#      times the number of vblanks the spin actually consumed (g_idleFrames).
#
# Build: make clean && make -j4 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=idle_window.gdb ./diag_run.sh 200
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2000
continue
printf "=== snapshot in flight at vbi %u (VVBLKI=$%02x%02x $3D=%02x — must be $4ff5 / 00) ===\n", \
  g_vbiCount, mem[0x223], mem[0x222], mem[0x3D]
set $sVbi   = (int)g_vbiCount
set $sIdle  = (int)g_rIdleWall
set $sWall  = (int)g_rRenderWall
set $sRc    = (int)g_rCalls
set $sFd    = (int)g_fdCalls
set $sIdleF = (int)g_idleFrames
set $sIsrL  = (int)g_flightProf.isrLines
set $sIsrN  = (int)g_flightProf.isrCalls

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 4800
continue

set $dVbi   = (int)g_vbiCount - $sVbi
set $dIdle  = (int)g_rIdleWall - $sIdle
set $dWall  = (int)g_rRenderWall - $sWall
set $dRc    = (int)g_rCalls - $sRc
set $dFd    = (int)g_fdCalls - $sFd
set $dIdleF = (int)g_idleFrames - $sIdleF
set $dIsrL  = (int)g_flightProf.isrLines - $sIsrL
set $dIsrN  = (int)g_flightProf.isrCalls - $sIsrN
set $spins  = $dRc - $dFd
set $tot    = $dVbi * 313

printf "\n=== window vbi %d -> %u : %d vbi = %d ticks of wall clock ===\n", \
  $sVbi, g_vbiCount, $dVbi, $tot
printf "  VVBLKI=$%02x%02x $3D=%02x   painted=%d  renderFrame calls=%d  => spin calls=%d (%d%%)\n", \
  mem[0x223], mem[0x222], mem[0x3D], $dFd, $dRc, $spins, ($dRc ? (100*$spins)/$dRc : 0)

printf "\n  frame-sync spin : %6d t = %d.%d%% of wall clock", \
  $dIdle, (100*$dIdle)/$tot, ((1000*$dIdle)/$tot)%10
printf "   (%d t per spin call)\n", ($spins ? $dIdle/$spins : 0)
printf "  vblanks consumed by the spin (g_idleFrames) = %d over %d spin calls\n", $dIdleF, $spins

# The ISR fires inside the spin and would run regardless, so it is not recoverable.
set $isrPer = ($dIsrN ? $dIsrL/$dIsrN : 0)
set $isrIn  = $dIdleF * $isrPer
printf "  ISR running inside the spin  : ~%d t  (%d vblanks x %d t/firing, NOT recoverable)\n", \
  $isrIn, $dIdleF, $isrPer
printf "  => RECOVERABLE idle          : ~%d t = %d.%d%% of wall clock\n", \
  ($dIdle-$isrIn), (100*($dIdle-$isrIn))/$tot, ((1000*($dIdle-$isrIn))/$tot)%10

printf "\n  (renderFrame body = %d t = %d%% of wall clock, for scale)\n", \
  $dWall, (100*$dWall)/$tot
detach
quit
