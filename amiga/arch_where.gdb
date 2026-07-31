set pagination off
set confirm off
continue
printf "\n==== SIGINT ====\n"
printf "g_vbiCount   = %u\n", g_vbiCount
printf "g_dsEntryN   = %u\n", g_dsEntryN
printf "g_iterCount  = %u\n", g_iterCount
printf "g_saPhase    = %d (launch-cinematic milestone in display_setup)\n", g_saPhase
printf "VVBLKI 0222/0223 = %02x%02x  (4FF5=flight, 53CC=standby/title, 52D7=standby)\n", mem[0x0223], mem[0x0222]
printf "060B(cockpit)=%02x 0004(lvl_or_state)=%02x 006B?=  006c=%02x 006d(stage)=%02x\n", mem[0x060B], mem[0x0004], mem[0x006C], mem[0x006D]
printf "067E=%02x 0684=%02x 0686=%02x 0655=%02x 0088=%02x 008C=%02x\n", mem[0x067E], mem[0x0684], mem[0x0686], mem[0x0655], mem[0x0088], mem[0x008C]
printf "0642(range)=%02x 0079=%02x 003D(descent)=%02x 003A=%02x 0072(lives)=%02x\n", mem[0x0642], mem[0x0079], mem[0x003D], mem[0x003A], mem[0x0072]
echo --- backtrace ---\n
bt 12
