# Catch the game-over-corruption GURU live, before FS-UAE resets the machine.
#   ./debug.sh "" catch_guru.gdb
# FS-UAE resets the Amiga on a guru, so gdb otherwise only sees the post-reset
# ROM.  We break in the 68000 fault-handler entry points (bus/addr/illegal),
# read from the live vector table AFTER KS has installed them (deferred via a
# one-shot breakpoint on the game's VBI, which runs only once boot is done).
set pagination off
set confirm off
handle SIGSEGV stop nopass
handle SIGBUS  stop nopass
handle SIGILL  stop nopass

tbreak game_vbi_isr
commands
  silent
  set $busH = *(unsigned int*)0x8
  set $adrH = *(unsigned int*)0xc
  set $illH = *(unsigned int*)0x10
  break *$busH
  break *$adrH
  break *$illH
  printf "\n>>> fault handlers armed: bus=%08x addr=%08x ill=%08x\n", $busH, $adrH, $illH
  printf ">>> NOW switch to FS-UAE, fly, and crash.\n\n"
  continue
end

printf "\n>>> booting (wait for the 'fault handlers armed' line, then fly & crash)...\n"
continue

# --- reached only on a fault-handler hit / SIGSEGV / SIGBUS / SIGILL ---
printf "\n=========== GURU / FAULT ===========\n"
printf "VVBLKI=%02x%02x  365B=%02x  platform=%08x  vbi=%u\n", \
  *(unsigned char*)(mem+0x0223), *(unsigned char*)(mem+0x0222), \
  *(unsigned char*)(mem+0x365B), platform, g_vbiCount
info registers
printf "--- 68000 group-0 frame @ SSP:  SSW | accessAddr | IR | SR | faultPC ---\n"
x/8xw $sp
printf "--- deeper supervisor stack ---\n"
x/40xw $sp
bt
printf "====================================\n"
