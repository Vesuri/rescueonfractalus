# Catch the stray writer of mem[$D01F]=$06 (START "held") during flight.
# HW watchpoint fires only during flight (g_vbiCount>1500) when CONSOL bit0 goes CLEAR
# (the bug value), prints the writer's backtrace, and STOPS (then the script quits).
# You only need to LAUNCH (press RETURN) and be briefly in flight — no pilot needed, since
# the stray write recurs every flight frame.
set pagination off
set confirm off
set remotetimeout 120
target remote 127.0.0.1:2345
echo \n>>> connected — press RETURN to launch, then fly a moment <<<\n
watch mem[0xD01F] if (mem[0xD01F] & 1) == 0 && g_vbiCount > 1500
commands
  printf "\n>>> STRAY D01F <- %02x @vbi=%u (060B=%02x 0004=%02x) — writer backtrace:\n", mem[0xD01F], g_vbiCount, mem[0x060B], mem[0x0004]
  bt 15
  printf "===== end backtrace =====\n"
end
continue
echo \n==== detaching ====\n
detach
quit
