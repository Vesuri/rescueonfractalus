# buildStarSprites tail-wipe sizing check.  Needs `make PROBES=1 FORCE_RETURN=1` (FORCE_RETURN is
# what produces a SECOND launch, hence a second stars entry — the only case that can distinguish a
# correctly-sized wipe from one that vacuously wipes nothing).
#
# What to look for:
#   entry [0] = 0      — first entry, rings are MEMF_CLEAR, nothing to wipe (and the blit is SKIPPED:
#                        height 0 would mean 1024 rows to the blitter).
#   entry [1] > 0      — re-entry inherits the previous pass's window W; this is the whole point.
#                        Expect it near the 595-row measured scroll, and always <= 640 (kStarMaxScroll).
#   entries >= 2       — same as [1] if the run got that far.
# The old code wiped kTailSlots = 646 slots on EVERY entry.
set width 0
continue
echo \n==== SIGINT ====\n
printf "buildStarSprites entries=%u\n", g_seSprCalls
printf "  tail slots wiped: [0]=%u [1]=%u [2]=%u [3]=%u   (want [0]==0, [1]>0; old code = 646 always)\n", \
  g_seSprTail[0], g_seSprTail[1], g_seSprTail[2], g_seSprTail[3]
printf "  geometry: ringSlots=736 headSlots=90 tailSlots(max)=646  -> saved on entry N = 646-[N] slots\n"
printf "  last entry cost: kick=%lu convert=%lu drain=%lu beam ticks (313=1 frame)\n", \
  g_seSprKick, g_seSprConv, g_seSprDrain
printf "  first-entry total buildStarSprites=%lu ticks (~%lums) atVbi=%u\n", \
  g_starSprTicks, g_starSprTicks/16, g_starSprVbi
printf "vbi=%u VVBLKI=%02x%02x 060B=%02x  [060B==23 while launched]\n", \
  g_vbiCount, mem[0x0223], mem[0x0222], mem[0x060B]
# Scene-dependent reads LAST: s_scene is null at the trigger breakpoint, so this must come after
# the continue, and an abort here must not cost us the numbers above.
set $sc = 'PlatformAmiga.cpp'::s_scene
printf "live starWindow=%d starSpritesValid=%d starPhaseActive=%d\n", \
  $sc->starWindow, $sc->starSpritesValid, $sc->starPhaseActive
detach
quit
