# OBJECT PLANE-1 OVERLAY SHAPE — size the byte scan on RescueOnFractalus.cpp:2484.
#
# renderFlightDirect ORs the value-3 ground-object low bits from the s_flightObjP1 scratch into
# plane1 after the sky fill, walking the dirty bounding BOX one BYTE at a time and clearing as it
# goes.  The COMBAT+invulnerable PC profile puts 20/300 samples (6.7% of ALL flight) on that inner
# line, which makes it the largest single non-asm line in the profile.
#
# This prints the box the walk actually visits vs the bytes in it that are nonzero, so a candidate
# can be sized before it is written (the density decides whether the win is a wider access or a
# tighter range).
#
# Build: make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1 BAND_SHAPE=1
# Run:   . ./env.sh && GDBSCRIPT=objp1_shape.gdb ./diag_run.sh 220
# (shape_probe.gdb prints this too, but it also needs EDGE_SHAPE=1 and dies without it.)
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2200
continue
printf "=== snapshot in flight at vbi %u (VVBLKI=$%02x%02x $3D=%02x — must be $4ff5 / 00) ===\n", \
  g_vbiCount, mem[0x223], mem[0x222], mem[0x3D]
set $sFr  = (int)g_bsObjFrames
set $sRow = (int)g_bsObjRows
set $sBox = (int)g_bsObjBox
set $sByt = (int)g_bsObjBytes
set $sTk  = (int)g_bsObj
set $sBf  = (int)g_bsBandFrames
set $sFd  = (int)g_fdCalls
set $sVbi = (int)g_vbiCount

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 5000
continue

set $dFr  = (int)g_bsObjFrames - $sFr
set $dRow = (int)g_bsObjRows   - $sRow
set $dBox = (int)g_bsObjBox    - $sBox
set $dByt = (int)g_bsObjBytes  - $sByt
set $dTk  = (int)g_bsObj       - $sTk
set $dBf  = (int)g_bsBandFrames- $sBf
set $dFd  = (int)g_fdCalls     - $sFd
set $dVbi = (int)g_vbiCount    - $sVbi

printf "\n=== window vbi %d -> %u (%d vbi, %d painted frames) ===\n", $sVbi, g_vbiCount, $dVbi, $dFd
printf "  VVBLKI=$%02x%02x $3D=%02x\n", mem[0x223], mem[0x222], mem[0x3D]
printf "\n  frames with a dirty box : %d of %d painted (%d%%)\n", \
  $dFr, $dFd, ($dFd ? (100*$dFr)/$dFd : 0)
printf "  rows per dirty frame    : %d.%d\n", ($dFr ? $dRow/$dFr : 0), ($dFr ? (10*$dRow/$dFr)%10 : 0)
printf "  BOX bytes visited       : %d total, %d per dirty frame\n", $dBox, ($dFr ? $dBox/$dFr : 0)
printf "  of which NONZERO        : %d (%d.%d%% of the box)  <-- the real work\n", \
  $dByt, ($dBox ? (100*$dByt)/$dBox : 0), ($dBox ? (1000*$dByt)/$dBox%10 : 0)
printf "  row-range-only walk would visit %d (%dx the box)\n", \
  $dRow*40, ($dBox ? ($dRow*40)/$dBox : 0)
printf "\n  g_fdBand overlay bracket: %d t over %d band frames = %d.%d t/frame\n", \
  $dTk, $dBf, ($dBf ? $dTk/$dBf : 0), ($dBf ? (10*$dTk/$dBf)%10 : 0)
printf "  wall clock in the window = %d t; overlay = %d.%d%% of it\n", \
  $dVbi*313, (100*$dTk)/($dVbi*313), (1000*$dTk)/($dVbi*313)%10
detach
quit
