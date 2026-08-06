# On-target FUZZ of the hand-asm SFX mixer twin (build: make FUZZ=1).
# Runs once at boot from main(), BEFORE any game code, so it needs no flight and no delay.
# badCell $C0FF = a cpu register diff; $D2xx = the real POKEY shadow (the AUDC distortion +
# volume nibble); anything in $0600-$0820 = a mem[] cell.
set pagination off
set confirm off
tbreak PlatformAmiga::run
continue
printf "\n=== SFX mixer asm-vs-C FUZZ: %lu cases, %lu BAD (must be 0) ===\n", \
  g_sfxFuzzCases, g_sfxFuzzBad
printf "  firstBad case=%lu at y=%lu  badCell=$%04lx  asm=$%02lx C=$%02lx\n", \
  g_sfxFuzzFirstBad, g_sfxFuzzBadY, g_sfxFuzzBadCell, g_sfxFuzzBadAsm, g_sfxFuzzBadC
printf "  path coverage (all six must be non-zero or the fuzz proved less than it looks):\n"
printf "    idle slot: a<top=%lu  promote/move=%lu  a==top&y<idx=%lu\n", \
  g_sfxFuzzPath[0], g_sfxFuzzPath[1], g_sfxFuzzPath[2]
printf "    active slot: y>=$0D=%lu  next==top=%lu  move-to-next=%lu\n", \
  g_sfxFuzzPath[3], g_sfxFuzzPath[4], g_sfxFuzzPath[5]
detach
quit
