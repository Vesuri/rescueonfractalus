# LEVEL-COMPLETE readout: bonus tally -> jingle -> the LIFT (the elevator that scrolls the LEVEL
# doors up to the next level).  Reproduces the EAB report "after completing a level the lift gets
# stuck after lifting to the next level; the cockpit lights still blink, the music is stuck, and it
# never returns to the main loop".
# Build: make clean && make -j4 PROBES=1 FORCE_RETURN=1 LEVEL_DONE=1   [LEVEL_DONE_LEVEL=n]
# Run:   GDBSCRIPT=level_done.gdb ./diag_run.sh 120
#
#   0629/062A  rescued pilots / level quota — LEVEL_DONE pins the first to the second
#   00E3       $0629-$062A: bit7 clear = quota MET = the level-complete branch
#   006D/0609  level_stage / the lift's top level (= min(level+4,$63) when the quota was met)
#   008B       the LMS scroll index the lift's two spins wait on; the VBI decrements it by 1 a frame
#   liftClear/Scroll/Pass/SpinB  L_6219 / L_622d / L_6268 / L_628f reached
continue
echo \n==== SIGINT ====\n
printf "vbi=%u  VV=%04x  cop=%u  003A=%02x 006C=%02x 060B=%02x\n", \
  g_vbiCount, (mem[0x0222] | (mem[0x0223] << 8)), g_liveCopper, \
  mem[0x003A], mem[0x006C], mem[0x060B]
printf "quota: 0629=%02x 062A=%02x 00E3=%02x   level: 006D=%02x 0609=%02x  scroll: 008B=%02x\n", \
  mem[0x0629], mem[0x062A], mem[0x00E3], mem[0x006D], mem[0x0609], mem[0x008B]
printf "score: %02x%02x%02x%02x  0628=%02x  music 0655=%02x  msg 00D8=%02x\n", \
  mem[0x0600], mem[0x0601], mem[0x0602], mem[0x0603], mem[0x0628], mem[0x0655], mem[0x00D8]
printf "lift: clear=%u scroll=%u pass=%u spinB=%u\n", \
  g_liftClear, g_liftScroll, g_liftPass, g_liftSpinB
# WHERE the main loop is.  Two PC samples with 400 instructions in between: an identical PC is a
# tight loop the main loop cannot leave (the VBI keeps running regardless, which is why the screen
# still animates).  `info symbol` names the routine; x/2i shows the loop itself.
printf "\n--- main-loop PC sample 1 ---\n"
info symbol $pc
x/2i $pc
stepi 400
printf "--- main-loop PC sample 2 (after 400 instructions) ---\n"
info symbol $pc
x/2i $pc
printf "008B now=%02x  vbi now=%u\n", mem[0x008B], g_vbiCount
