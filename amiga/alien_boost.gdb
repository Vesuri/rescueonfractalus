# "Alien aboard + BOOSTERS" ascent diagnosis.
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1 ALIEN_ABOARD=1 FIXED_RNG=1
#   . ./env.sh && GDBSCRIPT=alien_boost.gdb ./diag_run.sh 90
#
# FORCE_RETURN arms the mother ship at flight+250 and presses B at flight+340; ALIEN_ABOARD sets
# $0633 at flight+120, so the boost happens with the alien aboard.  $0034 (terrain_depth_step) is
# the ascent progress: game_main_loop leaves flight only once it reaches $40.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "vbi=%u  VVBLKI=$%02x%02x  0072(mode)=%02x 0633(alien)=%02x\n", \
  g_vbiCount, mem[0x223], mem[0x222], mem[0x72], mem[0x633]
printf "mode-2 VBI frames = %u   main-loop iters in that window = %u  (vbi/iter = %u)\n", \
  g_abVbi, (unsigned short)(g_abIterN - g_abIter0), \
  ((g_abIterN - g_abIter0) ? g_abVbi / (unsigned short)(g_abIterN - g_abIter0) : 0)
printf "0034 depth_step: first=%02x last=%02x max=%02x   (needs $40 to leave flight)\n", \
  g_ab34First, g_ab34Last, g_ab34Max
printf "pitch 0029/002A=%02x%02x  roll 0026=%02x  throttle 002E=%02x\n", \
  g_ab29Last, g_ab2ALast, g_ab26Last, g_ab2ELast
printf "004A(joystick_saved)=%02x  0004=%02x  pendingFlightKey=%02x\n", \
  g_ab4ALast, g_ab04Last, g_abKeyPend
printf "BREAK injected at vbi=%u  keywin calls=%u  $52B4 frames=%u  blanks=%u\n", \
  g_abBreakVbi, g_abKeyWin, g_ab52B4, g_blankForRestartCount
printf "--- samples every 25 mode-2 VBI: 0034 / 0029 / 002E ---\n"
set $i = 0
while $i < g_abNSamp
  printf "  [%2d] 34=%02x 29=%02x 2E=%02x\n", $i, g_abSamp34[$i], g_abSamp29[$i], g_abSamp2E[$i]
  set $i = $i + 1
end
