# Is a throughput delta REAL WORK or just vblank quantisation?
#
# fps_ab/fps_multi measure iterations per VBI frame, and the flight loop rounds each pass up to a
# frame boundary (renderFrame's "wait for next real VBI" spin + the deferred-flip drain).  So a
# change of a few microseconds can move a pass across a boundary and shift throughput by a whole
# frame per iteration (~4.5%) in EITHER direction — and because the whole run sits on the same
# side of that boundary, BOTH windows of fps_multi move together and it reads like signal.
#
# This splits frames-per-iteration into COMPUTE and WAIT using integer VBI counters that the beam
# race cannot poison (g_idleFrames, g_flipWaitFrames):
#     frames/iter = compute + idle-spin + flip-drain
# If two builds differ only in the wait terms, the work is unchanged and the throughput delta is
# quantisation.  If the compute term moves, the work really changed.
#
# Build: make clean && make -j4 PROBES=1 FIXED_RNG=1 [BLIT_IRQ=1]
# Run:   . ./env.sh && GDBSCRIPT=pace_ab.gdb ./diag_run.sh 200
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 2600
continue
set $v0 = g_vbiCount
set $i0 = g_iterCount
set $id0 = g_idleFrames
set $fw0 = g_flipWaitFrames
set $fc0 = g_flipWaitCalls
set $fd0 = g_fdCalls
set $rc0 = g_rCalls

tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 5600
continue
set $dv = (int)g_vbiCount - (int)$v0
set $di = (int)g_iterCount - (int)$i0
set $id = (int)g_idleFrames - (int)$id0
set $fw = (int)g_flipWaitFrames - (int)$fw0
set $fc = (int)g_flipWaitCalls - (int)$fc0
set $fd = (int)g_fdCalls - (int)$fd0
set $rc = (int)g_rCalls - (int)$rc0

printf "WINDOW: %d iters / %d vbi frames -> iter/frame = 0.%04d (effFPS %d.%02d)\n", \
  $di, $dv, ($di*10000)/$dv, ($di*100)/$dv, (($di*10000)/$dv)%100
printf "frames per iteration: %d.%02d  (= %d vbi / %d iters)\n", \
  $dv/$di, (($dv*100)/$di)%100, $dv, $di
printf "  idle-spin frames  : %d.%02d/iter  (%d over the window, renderFrame vblank pacing)\n", \
  $id/$di, (($id*100)/$di)%100, $id
printf "  flip-drain frames : %d.%02d/iter  (%d over the window, %d drains)\n", \
  $fw/$di, (($fw*100)/$di)%100, $fw, $fc
printf "  => COMPUTE frames : %d.%02d/iter  (frames - both waits = the actual work)\n", \
  ($dv-$id-$fw)/$di, ((($dv-$id-$fw)*100)/$di)%100
printf "calls: renderFlightDirect painted %d, renderFrame %d, ISR ticks/iter %d\n", \
  $fd, $rc, (int)(g_isrBeamLines)/$di
# ⚠ CROSS-BUILD SANITY: is this build even flying the same ship state at the same vbi?  Ship
# motion is VBI-driven and FIXED_RNG pins the LEVEL, but POKEY $D20A is a READ-CLOCKED LFSR, so
# any RANDOM read whose count depends on render speed makes a faster build draw different
# numbers from here on.  If these bytes differ between two builds at the same vbi, they are NOT
# flying the same thing and their iter/frame numbers are not comparable.
printf "SHIP@vbi%u: heading=%02x%02x octant=%02x alt=%02x/%02x roll=%02x%02x scroll=%02x%02x\n", \
  g_vbiCount, mem[0x2886], mem[0x2885], mem[0x280D], mem[0x28D9], mem[0x28DA], \
  mem[0x0029], mem[0x0028], mem[0x00A4], mem[0x00A5]
printf "  objects: %02x %02x %02x %02x  RANDOM-driven state\n", \
  mem[0x2854], mem[0x2855], mem[0x2856], mem[0x2857]
detach
quit
