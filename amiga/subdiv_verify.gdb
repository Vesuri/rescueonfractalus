# On-target differential verify for the hand-asm terrain_subdivide_column twin
# (build: make VERIFY=1 PROBES=1).  Runs into deep flight, then reports the per-call
# asm-vs-C-oracle comparison tallies accumulated by terrain_subdivide_column_core
# (rof_native.c, ROF_SUBDIV_VERIFY path).
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 320
continue
printf "VERIFY fdCalls=%u vbi=%u subdivCalls=%lu mismatch=%lu firstBad=%lu\n", \
  g_fdCalls, g_vbiCount, g_subdivCalls, g_subdivMismatch, g_subdivFirstBad
printf "  PERF beam-ticks: asm=%lu C=%lu  (per-call asm=%lu C=%lu, lower=faster)\n", \
  g_subdivAsmTicks, g_subdivCTicks, \
  (g_subdivCalls? g_subdivAsmTicks/g_subdivCalls:0), (g_subdivCalls? g_subdivCTicks/g_subdivCalls:0)
printf "  FIRSTBAD kind=%lu (1=ret 2=stack 3=zp) idx=%lu asm=0x%lx C=0x%lx  ret asm=%lu C=%lu\n", \
  g_subdivBadKind, g_subdivBadIdx, g_subdivBadAsm, g_subdivBadC, g_subdivBadRetA, g_subdivBadRetC
detach
quit
