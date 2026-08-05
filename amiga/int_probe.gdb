# What do the interrupts we do NOT own cost us in flight, and is anything else running?
#
# Three questions, one run:
#  1. WHICH sources can interrupt us at all — INTENAR, sampled at VBI entry (g_irqIntena).
#     Anything enabled but unused is pure overhead per firing.
#  2. HOW OFTEN each shared chain fires and how long its server chain runs — the head/tail
#     tap pairs on EXTER (level 6, CIA-B) / PORTS (level 2, CIA-A) / BLIT (level 3,
#     blit-done).  Chain duration only: the exception + Kickstart stub + walker ahead of
#     the head tap is NOT included (the VERTB g_irqLat* number sizes that part).
#  3. Whether MULTITASKING costs anything — ExecBase->DispCount (task dispatches) and
#     IdleCount over the same window, plus TDNestCnt (Forbid nesting) and ThisTask.
#
# Rates are measured over a flight-only window (vbi 2600 -> 5600 = 60 s), so boot/standby
# activity can't contaminate them.  Pair with `make FIXED_RNG=1` for a pinned level.
#
# Build: make clean && make -j4 PROBES=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=int_probe.gdb ./diag_run.sh 200
set pagination off
set confirm off

set $exec = *(unsigned long *)0x4
# ExecBase: IntVects[16] at +84 (12 bytes each) -> ThisTask +276, IdleCount +280,
# DispCount +284, Quantum +288, Elapsed +290, SysFlags +292, IDNestCnt +294, TDNestCnt +295.
set $pThisTask = $exec + 276
set $pIdle     = $exec + 280
set $pDisp     = $exec + 284

tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 2600
continue
set $v0 = g_vbiCount
set $i0 = g_iterCount
set $eh0 = g_tapHeadCnt[0]
set $et0 = g_tapTailCnt[0]
set $ev0 = g_tapVsum[0]
set $es0 = g_tapHsum[0]
set $ph0 = g_tapHeadCnt[1]
set $pt0 = g_tapTailCnt[1]
set $pv0 = g_tapVsum[1]
set $ps0 = g_tapHsum[1]
set $bh0 = g_tapHeadCnt[2]
set $bt0 = g_tapTailCnt[2]
set $bv0 = g_tapVsum[2]
set $bs0 = g_tapHsum[2]
set $disp0 = *(unsigned long *)$pDisp
set $idle0 = *(unsigned long *)$pIdle
set $mv0 = g_tapVsum[2]
set $ms0 = g_tapHsum[2]
set $nv0 = g_tapVsum[3]
set $ns0 = g_tapHsum[3]
set $bi0 = g_blitIrqCnt
set $kb0 = g_kbCalls
set $pi0 = g_portsIrqCnt
printf "MARK0: vbi=%u iter=%u\n", $v0, $i0

tbreak RescueOnFractalus::renderFlightDirect if g_vbiCount >= 5600
continue
set $dv = (int)g_vbiCount - (int)$v0
set $di = (int)g_iterCount - (int)$i0
printf "MARK1: vbi=%u iter=%u   window = %d vbi frames (%d.%01d s), %d iterations\n", \
  g_vbiCount, g_iterCount, $dv, $dv/50, ($dv*10/50)%10, $di

echo \n==== 1. ENABLED INTERRUPT SOURCES (INTENAR at VBI entry) ====\n
printf "INTENAR=%04x  INTREQR=%04x   (master b14=%d)\n", \
  g_irqIntena, g_irqIntreq, (g_irqIntena>>14)&1
printf "  lvl1: TBE(b0)=%d  DSKBLK(b1)=%d  SOFTINT(b2)=%d\n", \
  g_irqIntena&1, (g_irqIntena>>1)&1, (g_irqIntena>>2)&1
printf "  lvl2: PORTS(b3)=%d\n", (g_irqIntena>>3)&1
printf "  lvl3: COPER(b4)=%d  VERTB(b5)=%d  BLIT(b6)=%d\n", \
  (g_irqIntena>>4)&1, (g_irqIntena>>5)&1, (g_irqIntena>>6)&1
printf "  lvl4: AUD0(b7)=%d AUD1(b8)=%d AUD2(b9)=%d AUD3(b10)=%d\n", \
  (g_irqIntena>>7)&1, (g_irqIntena>>8)&1, (g_irqIntena>>9)&1, (g_irqIntena>>10)&1
printf "  lvl5: RBF(b11)=%d  DSKSYN(b12)=%d\n", (g_irqIntena>>11)&1, (g_irqIntena>>12)&1
printf "  lvl6: EXTER(b13)=%d\n", (g_irqIntena>>13)&1

echo \n==== 2. SHARED-CHAIN RATE + DURATION over the window ====\n
echo chain  firings  tails  per-frame  per-iter  mean-cc  mean-us  permille-of-wall\n
# --- EXTER (level 6, CIA-B) ---
set $h = (int)g_tapHeadCnt[0] - (int)$eh0
set $t = (int)g_tapTailCnt[0] - (int)$et0
set $cc = 0
if $h > 0
  set $cc = ((int)(g_tapVsum[0]-$ev0)*227 + (int)(g_tapHsum[0]-$es0)) / $h
end
printf "EXTER  %7d %6d   %4d.%02d  %5d.%01d  %7d  %4d.%01d  %6d\n", \
  $h, $t, $h/$dv, (($h*100)/$dv)%100, $h/$di, (($h*10)/$di)%10, $cc, \
  ($cc*282)/1000, (($cc*282)/100)%10, ($h*(($cc*282)/1000))/($dv*20)
# --- PORTS (level 2, CIA-A: keyboard) ---
set $h = (int)g_tapHeadCnt[1] - (int)$ph0
set $t = (int)g_tapTailCnt[1] - (int)$pt0
set $cc = 0
if $h > 0
  set $cc = ((int)(g_tapVsum[1]-$pv0)*227 + (int)(g_tapHsum[1]-$ps0)) / $h
end
printf "PORTS  %7d %6d   %4d.%02d  %5d.%01d  %7d  %4d.%01d  %6d\n", \
  $h, $t, $h/$dv, (($h*100)/$dv)%100, $h/$di, (($h*10)/$di)%10, $cc, \
  ($cc*282)/1000, (($cc*282)/100)%10, ($h*(($cc*282)/1000))/($dv*20)
# (BLIT is not tappable — single vector, see below.  Level-6/level-2 chains only here.)
# --- PORTS chain SPLIT: cumulative time from the head tap, by priority slot ---
set $h = (int)g_tapHeadCnt[1] - (int)$ph0
if $h > 0
  set $c2 = ((int)(g_tapVsum[2]-$mv0)*227 + (int)(g_tapHsum[2]-$ms0)) / $h
  set $c3 = ((int)(g_tapVsum[3]-$nv0)*227 + (int)(g_tapHsum[3]-$ns0)) / $h
  printf "  PORTS split: ciaa.resource(pri120) = %d cc (%d.%01d us);  UAE fs(pri20) = %d cc (%d.%01d us)\n", \
    $c2, ($c2*282)/1000, (($c2*282)/100)%10, $c3-$c2, (($c3-$c2)*282)/1000, ((($c3-$c2)*282)/100)%10
  printf "  (\"UAE fs\" is FS-UAE's host-filesystem/clipboard server — it does NOT exist on a real A500.)\n"
end
echo \n
# --- BLIT (level 3, single vector owned by graphics.library; counted by our own handler) ---
set $bi = (int)g_blitIrqCnt - (int)$bi0
printf "BLIT interrupts: %d over the window = %d.%02d per frame, %d.%01d per ITERATION\n", \
  $bi, $bi/$dv, (($bi*100)/$dv)%100, $bi/$di, (($bi*10)/$di)%10
printf "  at the VERTB-measured ~52us dispatch each that is ~%d.%01d ms per iteration = %d.%01d%% of wall\n", \
  ($bi*52)/($di*1000), (($bi*52)/($di*100))%10, ($bi*52)/($dv*200), (($bi*52)/($dv*20))%10
printf "  (nothing in this port consumes it: blitterWait polls DMACONR, blitterDrain spin-drains.)\n"
printf "PORTS attribution: our CIA-A SP keyboard handler ran %d times in the window\n", \
  (int)g_kbCalls - (int)$kb0
printf "  (0 with the PORTS tap firing => the traffic is another CIA-A source / another server.)\n"
printf "PORTS takeover (default build): our own level-2 handler ran %d times = %d.%02d/frame\n", \
  (int)g_portsIrqCnt - (int)$pi0, ((int)g_portsIrqCnt - (int)$pi0)/$dv, \
  ((((int)g_portsIrqCnt - (int)$pi0)*100)/$dv)%100
printf "  (STORM CHECK: this should track the ~11/s the OS chain saw.  Much higher means the\n"
printf "   source re-raises until an OS server services it, and PORTS_SERVER=1 is required.)\n"
echo \n
printf "(1 cc = 282ns.  permille-of-wall = firings * mean-us / window-us, in 1/1000ths.\n"
printf " A tails SHORTFALL vs firings means a server ahead of the tail returned non-zero and\n"
printf " ended the walk, so that chain's mean-cc is under-counted.\n"
printf " These durations EXCLUDE exception entry + the Kickstart stub + exec's chain walker\n"
printf " (VERTB measures ~52us of that per firing) — so per firing this is a LOWER bound.)\n"

echo \n==== 3. MULTITASKING ACTIVITY over the window ====\n
set $dd = (int)(*(unsigned long *)$pDisp) - (int)$disp0
printf "DispCount +%d (%d.%02d per frame)   IdleCount +%d\n", \
  $dd, $dd/$dv, (($dd*100)/$dv)%100, (int)(*(unsigned long *)$pIdle) - (int)$idle0
set $tt = *(unsigned long *)$pThisTask
printf "ThisTask=%08x name=%s pri=%d   TDNestCnt=%d IDNestCnt=%d  Quantum=%d Elapsed=%d\n", \
  $tt, (char *)*(unsigned long *)($tt + 10), *(signed char *)($tt + 9), \
  *(signed char *)($exec + 295), *(signed char *)($exec + 294), \
  *(unsigned short *)($exec + 288), *(unsigned short *)($exec + 290)
printf "(DispCount counts task dispatches: ~0 over the window => multitasking costs us nothing.)\n"

echo \n==== 4. throughput baseline (same window) ====\n
printf "iter/frame = 0.%04d   effFPS = %d.%02d\n", \
  ($di*10000)/$dv, ($di*100)/$dv, (($di*10000)/$dv)%100
set $lat = 0
if g_irqLatCnt > 0
  set $lat = (int)((g_irqLatVsum*227 + g_irqLatHsum)/g_irqLatCnt)
end
printf "VERTB dispatch (line 0 -> our handler) = %d cc = %d.%01d us/firing\n", \
  $lat, ($lat*282)/1000, (($lat*282)/100)%10
detach
quit
