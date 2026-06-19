set pagination off
set confirm off
set height 0
# Statistical PC sampling: each SIGINT (sent by the harness) stops the inferior; print
# where the main thread is, then continue. Run across the launch door window.
set $i = 0
while $i < 50
  continue
  printf "S%d vbi=%u door=%u 060B=%u pc=0x%x\n", $i, g_vbiCount, mem[0x008A], mem[0x060B], $pc
  frame 0
  set $i = $i + 1
end
detach
quit
