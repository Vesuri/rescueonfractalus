# diag_freeze.gdb — diagnose the pilot-rescue / Systems-off flight FREEZE.
#
# Theory (static trace): during a pilot rescue the flight main loop calls
# pilot_render ($7854), which contains three ds_frame()-driven wait loops that
# redraw nothing but the last terrain buffer:
#   (1) while (mem[0x06FF] != 0) ds_frame();          <- waits on an SFX voice-state byte
#   (2) clear_colors_sweep_5x()   -> wait_frames_1 while mem[0x003E] != 0
#   (3) animate_clear_colors_timed() -> wait_frames_1 while mem[0x003E] != 0
# If one of these never sees its exit condition on the Amiga, pilot_render parks
# there -> the viewport freezes.  This script tells us WHICH loop, from the live
# call stack + the bytes the loops key off.
#
# USAGE (from amiga/):
#   1. Build a clean-backtrace debug build:
#        make clean && make OPT='-O0' NATIVE_OPT='-O0'
#   2. Run:  ./debug.sh "" diag_freeze.gdb
#   3. At the gdb prompt type:  continue
#   4. On screen: reach flight, trigger a pilot rescue (or press S to toggle
#      Systems) until the viewport FREEZES.
#   5. Press Ctrl-C in the gdb terminal to break in, then type:  diagfreeze
#   6. Type  continue , wait a couple seconds, Ctrl-C, and  diagfreeze  again.
#      If pilot_render is on the stack BOTH times -> the loop is parked (the bug).

define diagfreeze
  printf "\n==== FREEZE DIAGNOSIS ====\n"
  printf "PC=%#x\n", $pc
  echo --- backtrace (look for pilot_render / clear_colors_sweep_5x / animate_clear_colors_timed / wait_frames_1) ---\n
  bt
  printf "\n--- rescue / sweep state bytes ---\n"
  printf "  003C anim_flag        = %02x\n", mem[0x003C]
  printf "  003D landing_seq_flag = %02x\n", mem[0x003D]
  printf "  003E clear_colors_done= %02x   (S/Systems toggles this; sweeps loop while !=0)\n", mem[0x003E]
  printf "  06FF sfx voice[8]     = %02x   (loop 1 waits for this to reach 0)\n", mem[0x06FF]
  printf "  0634 sfx_state        = %02x   (sound-effect generator armed?)\n", mem[0x0634]
  printf "  00E7 music gate       = %02x   060B cockpit_flag = %02x\n", mem[0x00E7], mem[0x060B]
  printf "--- pilot / flight state ---\n"
  printf "  288D pilot_visible    = %02x   288E pilot_prev = %02x\n", mem[0x288D], mem[0x288E]
  printf "  0041 game_state       = %02x   0034 altitude   = %02x   0072 lives = %02x\n", mem[0x0041], mem[0x0034], mem[0x0072]
  printf "  VVBLKI = %02x%02x  (4FF5 = in-flight VBI)\n", mem[0x0223], mem[0x0222]
  printf "==========================\n"
end

# diagmore — extra state that gates the L_78d6<->L_792e hold loop in pilot_render.
define diagmore
  printf "\n==== HOLD-LOOP GATES ====\n"
  printf "  0079 pilot counter   = %02x   (entry INC landing_seq_flag only if <8 & <0x80)\n", mem[0x0079]
  printf "  0633 alien_trigger   = %02x   2844 = %02x  (loop head branch)\n", mem[0x0633], mem[0x2844]
  printf "  2830 = %02x  281E = %02x  061B = %02x\n", mem[0x2830], mem[0x281E], mem[0x061B]
  printf "  0014 RTCLOK = %02x\n", mem[0x0014]
  printf "=========================\n"
end

# ---- escape tests: poke a candidate, continue ~2s, Ctrl-C, diagfreeze to see if it ran on ----
#   try_ls3  : force landing_seq_flag(003D)=3  -> tests the "landing_seq_flag>=3" exit
#   try_3e0  : force clear_colors_done(003E)=0 -> tests the "$003E==0" exit (Systems-on path)
# If the game resumes (terrain redraws, pilot_render leaves the stack) that byte IS the escape.
define try_ls3
  set variable mem[0x003D] = 3
  printf ">>> poked landing_seq_flag(003D)=3; `continue`, watch screen, Ctrl-C, diagfreeze\n"
end
define try_3e0
  set variable mem[0x003E] = 0
  printf ">>> poked clear_colors_done(003E)=0; `continue`, watch screen, Ctrl-C, diagfreeze\n"
end

echo \n>>> diag_freeze loaded. Type `continue`, reproduce the freeze on screen,\n
echo     Ctrl-C to break in, then `diagfreeze` + `diagmore`.  Repeat once to confirm parked.\n
echo     Then test the escape: `try_ls3` (or `try_3e0`), `continue`, watch, Ctrl-C, `diagfreeze`. <<<\n
