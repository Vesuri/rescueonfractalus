# How many terrain_column_rasterize_core CALLS per flight iteration?
#
# Needed to size the subdivide<->rasterizer SPAN HANDOFF: both are hand-asm and both hold the
# span in registers, yet they pass it through mem[$82/$84/$86] — subdivide stores 3 bytes, the
# rasterizer loads them, works, stores them back at `done:`, subdivide reloads them (merging the
# high bytes back with and.w/or.w).  Counted off the disassembly that round trip is ~220 cycles
# per call plus ~38 of stack-arg ABI.  Whether that is 0.5% or 2% of wall clock depends entirely
# on the CALL COUNT — which nothing in the tree measures.
#
# g_tdRasterCalls / g_tdSubdivCalls are incremented in the C cores, which the default build
# replaces with asm ("asm twins kill their C shape-counters"), so this needs RASTER_C=1 SUBDIV_C=1.
# The C bodies are slower, but a CALL COUNT per terrain pass is a property of the terrain and the
# view, not of how fast the code runs, and both numbers come from the SAME run.
#
# Build: make clean && make -j4 RASTER_C=1 SUBDIV_C=1 PROBES=1 COMBAT=1 FIXED_RNG=1 PROFILE_NORING=1
# Run:   . ./env.sh && GDBSCRIPT=ras_count.gdb ./diag_run.sh 245
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u  $0623 emplace=%02x (start vbi=%u) ===\n", mem[0x6D], mem[0x623], g_vbiCount
set $r0 = (unsigned long)g_tdRasterCalls
set $s0 = (unsigned long)g_tdSubdivCalls
set $i0 = (unsigned long)(g_clPhIter[0] + g_clPhIter[1])

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  set $dr = (unsigned long)g_tdRasterCalls - $r0
  set $ds = (unsigned long)g_tdSubdivCalls - $s0
  set $di = (unsigned long)(g_clPhIter[0] + g_clPhIter[1]) - $i0
  if $di > 0
    printf "vbi %5u | +%3lu iters | raster %6lu = %4lu/iter (%4lu/pass) | subdiv %5lu = %3lu/iter | raster/subdiv = %lu.%02lu | VVBLKI=$%02x%02x $3D=%02x\n", \
      g_vbiCount, $di, $dr, $dr/$di, $dr/(2*$di), $ds, $ds/$di, \
      ($ds ? $dr/$ds : 0), ($ds ? ((100*$dr)/$ds)%100 : 0), \
      mem[0x223], mem[0x222], mem[0x3D]
  end
  set $r0 = (unsigned long)g_tdRasterCalls
  set $s0 = (unsigned long)g_tdSubdivCalls
  set $i0 = (unsigned long)(g_clPhIter[0] + g_clPhIter[1])
end

seg 2200
seg 2500
seg 2800
seg 3100
seg 3400
seg 3700

printf "\ntotals: rasterCalls=%lu subdivCalls=%lu iterations=%lu\n", \
  g_tdRasterCalls, g_tdSubdivCalls, (g_clPhIter[0] + g_clPhIter[1])
detach
quit
