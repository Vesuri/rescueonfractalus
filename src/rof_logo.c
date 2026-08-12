/* rof_logo.c — scene 1, the Lucasfilm Games logo (boot INITAD $5000).
 *
 * NOT a transliteration.  The logo lives in XEX segment 5 ($5000-$536F), which segment 16
 * ($3CDE-$B7FF) overwrites before the image Ghidra disassembles — so it is absent from
 * disasm/listing.txt, absent from rof_gen.c, and cannot be reached by the single-listing
 * pipeline.  ⚠ symbols.csv's `stage_5000` names the GAME routine that ends up at that
 * address; it is NOT this scene.  (docs/logo-station-plan.md §0/§1.)
 *
 * === NOT YET IMPLEMENTED (docs/logo-station-plan.md §5 step 5) ===================
 * Returning immediately makes the boot chain behave as ROF_BOOT_STATION: the logo's
 * segments are still placed, the station cinematic still plays.  The design is settled:
 *   - BAKE the finished bitmap (user decision).  The Atari draws it with playfield DMA
 *     OFF, so the drawing is never visible and a baked bitmap is bit-identical to what
 *     the player sees; the $5111 stroke plotter is deliberately NOT ported.
 *   - hand-write the $5000 sequencer + the $51EF VBI (a 4-channel POKEY sweep) here,
 *     with the sparkle on one hardware sprite.
 */
#include "cpu/cpu.h"
#include "rof_boot.h"

void rof_logo_run(void)
{
}
