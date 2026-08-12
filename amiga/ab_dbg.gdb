set pagination off
set confirm off
printf "g_abDone=%u g_abRows=%lu g_abTicks=%lu\n", g_abDone, g_abRows, g_abTicks
printf "g_vbiCount=%u\n", g_vbiCount
printf "g_figP1=%p g_figM=%p\n", g_figP1, g_figM
printf "VVBLKI=$%04x  mem[0x0632]=%02x\n", (mem[0x0223]<<8)|mem[0x0222], mem[0x0632]
info symbol $pc
