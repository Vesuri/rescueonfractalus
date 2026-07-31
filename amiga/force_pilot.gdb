set pagination off
set confirm off
tbreak rof_native.c:10409 if g_iterCount > 12
commands
  set variable g_forcePilotR1 = 1
  printf ">>> forced pilot-at-range-1 ON at iter=%u vbi=%u\n", g_iterCount, g_vbiCount
  continue
end
continue
printf "\n==== SIGINT ====\n"
printf "iter=%u vbi=%u dsN=%u lclN=%u\n", g_iterCount, g_vbiCount, g_dsEntryN, g_lclN
printf "event-$01 LOADS=%u  |  event-$01 PUSHES (v&7f==1)=%u  |  total pushes=%u  requeues=%u\n", g_bc01N, g_bcPush01N, g_bcPushN, g_rqN
printf "slot5 vol($0670)=%02x dist($0662)=%02x freq($067e)=%02x  ring head=%02x tail=%02x\n", mem[0x0670], mem[0x0662], mem[0x067E], mem[0x0073], mem[0x0074]
printf "0079=%02x 0642(range)=%02x 003D=%02x 003E=%02x 0041(gs)=%02x\n", mem[0x0079], mem[0x0642], mem[0x003D], mem[0x003E], mem[0x0041]
echo --- last 30 pushes (full byte; bit7=event): ---\n
set $i = 0
while $i < 30
  set $k = (g_bcPushIdx + 226 + $i) % 256
  printf "%02x@%u ", g_bcPushId[$k], g_bcPushVbi[$k]
  set $i = $i + 1
end
echo \n
