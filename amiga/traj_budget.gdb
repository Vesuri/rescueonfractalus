# Do two builds fly the SAME PATH?
#
# world_x {$2888:$2887} / world_z {$288A:$2889} are the ship's ground-plane position, integrated
# by flight_control_integrate in the 50 Hz VBI ISR.  If they match at matched entry-offsets, the
# trajectory is identical between builds and any DRAW t/it difference cannot be "it flew somewhere
# dearer".  If they diverge, the sim is coupled to the render rate and cross-build DRAW t/it is
# meaningless by construction.
#
# ⚠ mem[$28DA] is NOT altitude: terrain_draw_frame_core writes it per DRAW pass as the frame's
# terrain span extent (rof_native.c ~8135), so it is a render OUTPUT, not ship state.  Printed
# here only as a cost proxy.
#
# All convenience variables are $sXxx ($ps/$pc/$sp/$fp/$a0-$a7/$d0-$d7 are m68k REGISTER names).
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if mem[0x223] == 0x4f && mem[0x222] == 0xf5
continue
set $sEntry = g_vbiCount
printf "=== FLIGHT ENTRY at vbi %u ===\n", $sEntry

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $sEntry + 400
continue
set $sD0 = g_clPh[2][0] + g_clPh[2][1]
set $sS0 = g_clPh[0][0] + g_clPh[0][1]
set $sF0 = g_clPh[5][0] + g_clPh[5][1]
set $sI0 = g_clPhIter[0] + g_clPhIter[1]
set $sP0 = g_fdCalls

printf "\n offset | world_x | world_z | rollv  | rng  | iters | painted\n"
define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $sEntry + $arg0
  continue
  printf "entry+%4d |   %04x  |   %04x  |   %02x   |  %02x  | %5lu | %5lu\n", \
    $arg0, (mem[0x2888]<<8)|mem[0x2887], (mem[0x288A]<<8)|mem[0x2889], mem[0x0021], \
    mem[0x28DA], (unsigned long)(g_clPhIter[0]+g_clPhIter[1] - $sI0), \
    (unsigned long)(g_fdCalls - $sP0)
end

seg 900
seg 1400
seg 1900
seg 2400
seg 2900
seg 3400
seg 3900

set $sIt = (g_clPhIter[0] + g_clPhIter[1]) - $sI0
printf "\n=== ANCHORED window (entry+400 .. entry+3900), %lu iterations ===\n", $sIt
printf "  SETUP %4lu  DRAW %5lu  FRAME %4lu  | painted %lu\n", \
  ($sIt ? ((g_clPh[0][0]+g_clPh[0][1]) - $sS0)/$sIt : 0), \
  ($sIt ? ((g_clPh[2][0]+g_clPh[2][1]) - $sD0)/$sIt : 0), \
  ($sIt ? ((g_clPh[5][0]+g_clPh[5][1]) - $sF0)/$sIt : 0), \
  (unsigned long)(g_fdCalls - $sP0)
printf "=== iterations %lu, flight entry vbi %u ===\n", $sIt, $sEntry
detach
quit
