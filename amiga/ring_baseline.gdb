set pagination off
set confirm off
continue
printf "\n==== SIGINT ====\n"
printf "vbi=%u iter=%u dsN=%u\n", g_vbiCount, g_iterCount, g_dsEntryN
printf "pushes total=%u  event-$01 pushes (v&7f==1)=%u\n", g_bcPushN, g_bcPush01N
printf "event-$01 LOADS (input_init X=1)=%u   requeues total=%u\n", g_bc01N, g_rqN
printf "ring head($0073)=%02x tail($0074)=%02x  slot5 vol($0670)=%02x dist($0662)=%02x freq($067e)=%02x\n", mem[0x0073], mem[0x0074], mem[0x0670], mem[0x0662], mem[0x067E]
echo --- ring buffer $0719..$0738 ---\n
set $i = 0
while $i < 32
  printf "%02x ", mem[0x0719 + $i]
  set $i = $i + 1
end
echo \n--- last 24 pushes (full byte, bit7=marked event): ---\n
set $i = 0
while $i < 24
  set $k = (g_bcPushIdx + 104 + $i) % 256
  printf "%02x@%u ", g_bcPushId[$k], g_bcPushVbi[$k]
  set $i = $i + 1
end
echo \n
