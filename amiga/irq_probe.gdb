# Size the flight PC-profile's "unresolved / Kickstart ROM" bucket (~8% of samples,
# almost all at $F811F8 = exec's level-3 autovector entry).
#
# Two things decide whether that 8% is REAL CPU cost or a sampling artifact:
#   1. the level-3 interrupt RATE  — INTENAR bits 4/5/6 (COPER/VERTB/BLIT) are the only
#      sources that can raise level 3.  Only VERTB enabled => exactly 50 interrupts/s.
#   2. the per-interrupt DISPATCH cost — VERTB is raised at the start of line 0, so the
#      beam position at the first instruction of OUR AddIntServer handler is the whole
#      pre-handler cost (instruction completion + exception stacking + exec's wrapper +
#      any higher-priority VERTB server).  1 line = 227 colour clocks = 63.56 us.
#
# Also walks ExecBase->IntVects[VERTB].iv_Data (the server List) to name everyone in the
# chain — anything ahead of "RoF VBI" is OS work we pay for on every frame.
#
# Build:  make clean && make -j4 PROBES=1 FIXED_RNG=1
# Run:    . ./env.sh && GDBSCRIPT=irq_probe.gdb ./diag_run.sh 75
set pagination off
set confirm off
continue
echo \n==== SIGINT ====\n

printf "vbi=%u  VVBLKI=%02x%02x (4ff5 = in flight)\n", g_vbiCount, mem[0x0223], mem[0x0222]

echo \n--- level-3 interrupt SOURCES (only these can enter $F811F8) ---\n
printf "INTENAR=%04x INTREQR=%04x\n", g_irqIntena, g_irqIntreq
printf "  enabled: COPER(b4)=%d VERTB(b5)=%d BLIT(b6)=%d   [level 3 = COPER|VERTB|BLIT]\n", \
  (g_irqIntena>>4)&1, (g_irqIntena>>5)&1, (g_irqIntena>>6)&1
printf "  => level-3 rate = 50/s (VERTB) %s\n", \
  (((g_irqIntena>>4)&1) || ((g_irqIntena>>6)&1)) ? "+ COPER/BLIT (see above)" : "and nothing else"

echo \n--- exec dispatch overhead per VERTB (beam travel since line 0) ---\n
printf "samples=%lu  vsum=%lu hsum=%lu  max=line %u + %u cc\n", \
  g_irqLatCnt, g_irqLatVsum, g_irqLatHsum, g_irqLatVmax, g_irqLatHmax
# mean colour-clocks since line 0; 1 cc = 282 ns, so mean_ns = cc*282 and, at 50 VERTB/s,
# the share of wall clock is mean_ns*50/1e9 = mean_ns/20e6  (printed in tenths of a %).
set $cc = g_irqLatCnt ? (g_irqLatVsum*227 + g_irqLatHsum)/g_irqLatCnt : 0
printf "  mean = %lu.%02lu lines = %lu cc = %lu.%01lu us/firing\n", \
  (g_irqLatCnt ? g_irqLatVsum/g_irqLatCnt : 0), \
  (g_irqLatCnt ? (g_irqLatVsum*100/g_irqLatCnt)%100 : 0), \
  $cc, ($cc*282)/1000, (($cc*282)/100)%10
printf "  => at 50 VERTB/s: %lu.%01lu ms/s = %lu.%01lu%% of wall clock\n", \
  ($cc*282*50)/1000000, (($cc*282*50)/100000)%10, \
  ($cc*282)/200000, (($cc*282)/20000)%10

echo \n--- VERTB server chain (ExecBase->IntVects[5].iv_Data list; head runs FIRST) ---\n
set $exec = *(unsigned long *)0x4
set $lh   = *(unsigned long *)($exec + 144)
printf "ExecBase=%08x  VERTB server List=%08x\n", $exec, $lh
set $n = *(unsigned long *)$lh
set $k = 0
while $n != 0 && *(unsigned long *)$n != 0 && $k < 12
  printf "  [%d] node=%08x pri=%-4d name=%s\n", $k, $n, \
    *(signed char *)($n + 9), (char *)*(unsigned long *)($n + 10)
  set $n = *(unsigned long *)$n
  set $k = $k + 1
end
if $n != 0 && $k < 12
  printf "  [%d] node=%08x pri=%-4d name=%s  (tail)\n", $k, $n, \
    *(signed char *)($n + 9), (char *)*(unsigned long *)($n + 10)
end

echo \n--- reference: flight ISR body cost (for scale) ---\n
printf "isrCalls=%lu isrLines=%lu  per-firing=%lu lines (~%lu us)\n", \
  g_flightProf.isrCalls, g_flightProf.isrLines, \
  (g_flightProf.isrCalls? g_flightProf.isrLines/g_flightProf.isrCalls:0), \
  (g_flightProf.isrCalls? g_flightProf.isrLines*64/g_flightProf.isrCalls:0)
detach
quit
