# mem_dump_flight.gdb — EXACT-frame dump deep in flight.
#
# renderFrame runs below 50 Hz in flight, so "break renderFrame if g_vbiCount >= N" cannot
# land on a fixed frame (it read 5000 in one arm and 5006 in the other, making the dumps
# incomparable).  Break in the VBI ISR at the g_vbiCount++ site instead: it fires every
# vblank, so the equality condition pins the exact frame in both arms.
set width 0
set pagination off
break 'PlatformAmiga.cpp':2235 if g_vbiCount == 5000
continue
echo \n==== FRAME-LOCKED DUMP ====\n
printf "vbiCount=%u bootScene=%u VVBLKI=%02x%02x state(41)=%02x\n", \
  g_vbiCount, g_bootScene, mem[0x0223], mem[0x0222], mem[0x0041]
dump binary memory /tmp/rof_mem.bin &mem[0] &mem[65536]
echo dumped /tmp/rof_mem.bin\n
echo ==== end ====\n
