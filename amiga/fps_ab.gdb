# Cross-build flight throughput A/B, gated on the 50 Hz VBI clock (NOT on render frames).
#
# Gating on g_vbiCount is what makes this comparable across builds: ship motion is
# VBI-driven, so at a given vbi count every build is at the SAME point in the level flying
# the SAME terrain (pair with `make FIXED_RNG=1`, which pins WHICH level).  Gating on
# g_fdCalls instead — as the older verify scripts do — lets a slower build travel further
# before the window closes, so it samples different terrain.
#
# Two metrics over the window:
#  1. iterations completed per VBI frame — the honest throughput number, but only ~45
#     counts in a 1000-frame window, so its resolution is a coarse +-2%.
#  2. beam ticks per terrain pass (g_fDraw/g_fSetup/g_fClear deltas / iteration delta) —
#     thousands of counts, so this is the metric that can resolve a few %.  FP_TIME
#     subtracts OUR flight ISR (g_isrBeamLines) but NOT time stolen by OS interrupt
#     servers, so OS overhead shows up here.
#
# One iteration = two terrain passes = two displayed frames (the Atari-faithful
# double-buffer), so displayed FPS = 2 * 50 * iter/frame.
#
# Build: make clean && make -j4 PROBES=1 FIXED_RNG=1   [+ VERTB_SERVER=1 for the A side]
# Run:   . ./env.sh && GDBSCRIPT=fps_ab.gdb ./diag_run.sh 200
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 2600
continue
set $v0 = g_vbiCount
set $i0 = g_iterCount
set $d0 = g_fDraw
set $s0 = g_fSetup
set $c0 = g_fClear
set $r0 = g_fDirect
set $f0 = g_fdCalls
set $b0 = g_isrBeamLines
printf "MARK0: vbi=%u iter=%u draw=%lu\n", $v0, $i0, $d0

tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 5600
continue
printf "MARK1: vbi=%u iter=%u draw=%lu\n", g_vbiCount, g_iterCount, g_fDraw

set $dv = g_vbiCount - $v0
set $di = g_iterCount - $i0
printf "WINDOW: %d iters over %d vbi frames (%d.%01d s emulated)\n", \
  $di, $dv, $dv/50, ($dv*10/50)%10
printf "  iter/frame = 0.%04d  ->  effFPS = %d.%02d   ms/iteration = %d.%01d\n", \
  ($di*10000)/$dv, ($di*100)/$dv, (($di*10000)/$dv)%100, \
  ($dv*20)/$di, (($dv*200)/$di)%10
printf "PER-PASS beam ticks (1 tick = 63.56us; ISR-subtracted, OS-server time INCLUDED):\n"
printf "  draw=%d  setup=%d  clear=%d   sum=%d  (~%d.%01d ms)\n", \
  (g_fDraw-$d0)/$di, (g_fSetup-$s0)/$di, (g_fClear-$c0)/$di, \
  ((g_fDraw-$d0)+(g_fSetup-$s0)+(g_fClear-$c0))/$di, \
  (((g_fDraw-$d0)+(g_fSetup-$s0)+(g_fClear-$c0))*64/1000)/$di, \
  ((((g_fDraw-$d0)+(g_fSetup-$s0)+(g_fClear-$c0))*640/1000)/$di)%10
printf "  fDirect=%d ticks/call over %d calls   ISR=%d ticks/iter\n", \
  ((g_fdCalls-$f0) ? (g_fDirect-$r0)/(g_fdCalls-$f0) : 0), (g_fdCalls-$f0), \
  (g_isrBeamLines-$b0)/$di
printf "VERTB dispatch: mean %lu cc/firing (%lu.%01lu us) = %lu.%01lu%% of wall clock\n", \
  (g_irqLatCnt ? (g_irqLatVsum*227 + g_irqLatHsum)/g_irqLatCnt : 0), \
  (g_irqLatCnt ? ((g_irqLatVsum*227 + g_irqLatHsum)/g_irqLatCnt)*282/1000 : 0), \
  (g_irqLatCnt ? (((g_irqLatVsum*227 + g_irqLatHsum)/g_irqLatCnt)*282/100)%10 : 0), \
  (g_irqLatCnt ? ((g_irqLatVsum*227 + g_irqLatHsum)/g_irqLatCnt)*282/200000 : 0), \
  (g_irqLatCnt ? (((g_irqLatVsum*227 + g_irqLatHsum)/g_irqLatCnt)*282/20000)%10 : 0)
detach
quit
