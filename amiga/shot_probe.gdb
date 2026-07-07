set pagination off
continue
echo \n==== SIGINT ====\n
set $vv = mem[0x0222] | (mem[0x0223]<<8)
printf "VVBLKI=%04x vbi=%u | shot $36=%02x $37=%02x HPOS$cb=%02x SIZE$cd=%02x\n", $vv, g_vbiCount, mem[0x0036], mem[0x0037], mem[0x00cb], mem[0x00cd]
printf "P2 shot buf $0E34..$0E91 (nonzero rows):\n"
set $o = 0x34
while $o <= 0x91
  if mem[0x0E00+$o] != 0
    printf "  off%02x line%d : %02x\n", $o, 86+$o-0x32, mem[0x0E00+$o]
  end
  set $o = $o + 1
end
