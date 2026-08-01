# Catch the NON-RING writer that plants $81 into the $0719 event ring at $0724.
#
# The two ring push-loggers both live inside ring_push_0719, and g_push81Ra0 stayed 0
# across a full capture => NOTHING pushes $81 through the ring.  So the $81 the drain
# reads at $0724 (ring index 11) is written OOB by some other store.  Prime suspect:
# sfx_event_load_core's column writes `mem[$06F7+y]` etc. with a garbage voice slot y
# ($06F7+$2D == $0724), but this watch is writer-agnostic — it traps ANY store that
# sets $0724 to $81, from boot, and prints the backtrace + i/y/ring context.
#
# UNGATED (no g_vbiCount gate): the plant may happen during standby/boot, not only in
# flight.  Fly to a downed pilot (range 1/2) to reproduce the range-1 case.
set pagination off
set confirm off
set remotetimeout 120
target remote 127.0.0.1:2345
echo \n>>> connected — press RETURN to launch, then fly to a downed pilot (range 1/2) <<<\n

# HW data watchpoint on the absolute ring slot; only stop when it BECOMES $81.
watch mem[0x0724] if mem[0x0724] == 0x81
commands
  printf "\n>>> $81 PLANTED at $0724 (ring[11]) @vbi=%u  head(73)=%02x tail(74)=%02x range(642)=%02x pilotdist(79)=%02x\n", \
         g_vbiCount, mem[0x0073], mem[0x0074], mem[0x0642], mem[0x0079]
  printf ">>> ring $0719..$0738: "
  set $i = 0
  while $i < 32
    printf "%02x ", mem[0x0719 + $i]
    set $i = $i + 1
  end
  printf "\n>>> writer backtrace:\n"
  bt 20
  printf "===== end backtrace (continuing — SIGINT gdb to stop) =====\n"
  continue
end
continue
echo \n==== detaching ====\n
detach
quit
