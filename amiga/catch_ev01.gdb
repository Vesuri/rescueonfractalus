set pagination off
set confirm off
set remotetimeout 240
tbreak rof_native.c:10409 if g_iterCount > 12
commands
  set variable g_forcePilotR1 = 1
  set variable mem[0x003D] = 3
  printf ">>> force ON iter=%u vbi=%u\n", g_iterCount, g_vbiCount
  continue
end
break rof_native.c:8440 if g_iterCount > 15
commands
  printf "\n>>> input_init(X=1) EVENT $01 LOAD in flight! vbi=%u iter=%u  ring tail($0074)=%02x head($0073)=%02x  entry=mem[0719+tail]=%02x\n", g_vbiCount, g_iterCount, mem[0x0074], mem[0x0073], mem[0x0719 + mem[0x0074]]
  bt 10
  continue
end
continue
printf "\n==== SIGINT ====\n"
printf "iter=%u vbi=%u ev01_loads=%u push01=%u lclN=%u dsN=%u\n", g_iterCount, g_vbiCount, g_bc01N, g_bcPush01N, g_lclN, g_dsEntryN
printf "0079=%02x 0642=%02x 003D=%02x 003E=%02x\n", mem[0x0079], mem[0x0642], mem[0x003D], mem[0x003E]
