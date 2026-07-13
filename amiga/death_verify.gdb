# Death-cinematic verification: continue until the harness SIGINT, then dump the state.
# (Used with diag_run.sh + a FORCE_DEATH build; the vbiHandler arms $063D once in flight.)
continue
printf "=== DEATH STATE ===\n"
printf "VVBLKI   = %02x%02x\n", *(unsigned char*)(mem+0x0223), *(unsigned char*)(mem+0x0222)
printf "g_flightBlank = %d\n", g_flightBlank
printf "$063D event = %02x\n", *(unsigned char*)(mem+0x063D)
printf "$2891 phase = %02x\n", *(unsigned char*)(mem+0x2891)
printf "$00C2 speed = %02x\n", *(unsigned char*)(mem+0x00C2)
printf "$0041 gstate= %02x\n", *(unsigned char*)(mem+0x0041)
printf "$00D4 COLBK = %02x\n", *(unsigned char*)(mem+0x00D4)
printf "$00CF param0= %02x\n", *(unsigned char*)(mem+0x00CF)
printf "$365B title = %02x  (0x72 => Title screen text present)\n", *(unsigned char*)(mem+0x365B)
