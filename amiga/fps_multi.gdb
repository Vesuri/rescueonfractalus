# Two-window variant of fps_ab.gdb.  The single-window throughput number is QUANTISED: the flight
# loop's two vblank swap-waits round an iteration up to whole frames, so a sub-ms change in CPU work
# can move a fraction of iterations across a frame boundary and shift throughput by ~2% in EITHER
# direction regardless of whether the work got cheaper.  Sampling two disjoint windows (different
# terrain / flight phase) tells a real trend from that lottery: a genuine win moves both windows the
# same way.  Build: make clean && make -j4 PROBES=1 FIXED_RNG=1 ; Run: GDBSCRIPT=fps_multi.gdb ./diag_run.sh 190
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 2600
continue
set $v0 = g_vbiCount
set $i0 = g_iterCount
tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 5600
continue
set $v1 = g_vbiCount
set $i1 = g_iterCount
tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 8600
continue
set $v2 = g_vbiCount
set $i2 = g_iterCount
printf "WINDOW A: %d iters / %d vbi -> iter/frame = 0.%04d\n", \
  $i1-$i0, $v1-$v0, (($i1-$i0)*10000)/($v1-$v0)
printf "WINDOW B: %d iters / %d vbi -> iter/frame = 0.%04d\n", \
  $i2-$i1, $v2-$v1, (($i2-$i1)*10000)/($v2-$v1)
printf "TOTAL   : %d iters / %d vbi -> iter/frame = 0.%04d\n", \
  $i2-$i0, $v2-$v0, (($i2-$i0)*10000)/($v2-$v0)
printf "  (still in flight? VVBLKI=$%02x%02x should be $4ff5)\n", mem[0x225], mem[0x224]
detach
quit
