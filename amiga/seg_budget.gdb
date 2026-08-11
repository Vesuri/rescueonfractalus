# How much does DRAW t/it move WITHIN ONE RUN as the ship flies over different terrain?
#
# The point: in-flight terrain heights are generated from the read-clocked $D20A LFSR
# (scroll_field_columns -> gen_terrain_column, 4 samples per VBI), so DRAW's cost is a property of
# the terrain that happens to be in view.  If DRAW t/it swings by ~10% between windows of ONE
# binary, then a cross-build DRAW t/it delta of that size is not evidence of a code change — and
# the ledger's "+-3 t/it noise floor" (measured from SAME-BINARY repeats, which are near
# deterministic) does not apply to cross-build comparisons at all.
#
# SETUP (terrain_frame_setup, near-fixed work per iteration) is the built-in control: if SETUP
# holds flat across the same windows where DRAW swings, the swing is the view, not the machine.
#
# The `span` column is mem[$28DA], which terrain_draw_frame_core publishes at its END as the
# frame's terrain span extent — a render OUTPUT and a rough cost proxy, NOT the ship's altitude
# (docs/flight-perf-log.md §19.3).  $80 is its reset/invalid sentinel.
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u  $0623=%02x  $0621=%02x  (start vbi=%u) ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], g_vbiCount

set $sPd = g_clPh[2][0] + g_clPh[2][1]
set $sPs = g_clPh[0][0] + g_clPh[0][1]
set $sPf = g_clPh[5][0] + g_clPh[5][1]
set $sPi = g_clPhIter[0] + g_clPhIter[1]

printf "\n vbi  |  iters | DRAW t/it | SETUP t/it | FRAME t/it | span | live\n"
define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  set $sDd = (g_clPh[2][0] + g_clPh[2][1]) - $sPd
  set $sDs = (g_clPh[0][0] + g_clPh[0][1]) - $sPs
  set $sDf = (g_clPh[5][0] + g_clPh[5][1]) - $sPf
  set $sDi = (g_clPhIter[0] + g_clPhIter[1]) - $sPi
  if $sDi > 0
    printf "%5u | %6lu | %9lu | %10lu | %10lu | %3x | VVBLKI=$%02x%02x $3D=%02x\n", \
      g_vbiCount, $sDi, $sDd/$sDi, $sDs/$sDi, $sDf/$sDi, mem[0x28DA], \
      mem[0x223], mem[0x222], mem[0x3D]
  end
  set $sPd = g_clPh[2][0] + g_clPh[2][1]
  set $sPs = g_clPh[0][0] + g_clPh[0][1]
  set $sPf = g_clPh[5][0] + g_clPh[5][1]
  set $sPi = g_clPhIter[0] + g_clPhIter[1]
end

seg 2150
seg 2400
seg 2650
seg 2900
seg 3150
seg 3400
seg 3650
seg 3900
seg 4150
seg 4400
seg 4650
seg 4900
seg 5150
seg 5400

set $sIt = g_clPhIter[0] + g_clPhIter[1]
printf "\n=== whole-window totals ===\n"
printf "  DRAW  %5lu t/it   SETUP %5lu t/it   FRAME %5lu t/it   over %lu iterations\n", \
  ($sIt ? (g_clPh[2][0]+g_clPh[2][1])/$sIt : 0), ($sIt ? (g_clPh[0][0]+g_clPh[0][1])/$sIt : 0), \
  ($sIt ? (g_clPh[5][0]+g_clPh[5][1])/$sIt : 0), $sIt
printf "\n=== DRAW vs ship altitude ($28DA>>5) ===\n"
set $sB = 0
while $sB < 8
  if g_clAltIter[$sB] > 0
    printf "  alt bucket %d ($%02x-$%02x): %5lu iters  DRAW %5lu t/it\n", \
      $sB, $sB*32, $sB*32+31, g_clAltIter[$sB], g_clAltDraw[$sB]/g_clAltIter[$sB]
  end
  set $sB = $sB + 1
end
printf "\n=== iterations %lu, painted frames %lu ===\n", $sIt, g_fdCalls
detach
quit
