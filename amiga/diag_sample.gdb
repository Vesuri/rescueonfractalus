set pagination off
set confirm off
set height 0
# Statistical PC sampling: each SIGINT (sent by the harness) stops the inferior; print
# where the main thread is, then continue. Run across the launch door window.
set $i = 0
while $i < 200
  continue
  printf "S%d vbi=%u 060B=%u 004A=%u iter=%u pc=0x%x SYM=", $i, g_vbiCount, mem[0x060B], mem[0x004A], g_iterCount, $pc
  info symbol $pc
  set $i = $i + 1
end
detach
quit
