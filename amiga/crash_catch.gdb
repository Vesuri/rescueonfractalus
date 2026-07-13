# Run through the death cinematic + game-over music and catch any CPU exception (guru).
# If the inferior faults, gdb stops here and we print PC + registers + a backtrace.
continue
printf "=== STOPPED ===\n"
info registers pc sr
printf "VVBLKI=%02x%02x  $365B[0]=%02x\n", *(unsigned char*)(mem+0x0223), *(unsigned char*)(mem+0x0222), *(unsigned char*)(mem+0x365B)
bt
