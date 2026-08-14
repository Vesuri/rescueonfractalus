# mem_dump.gdb — frame-locked raw dump of the whole 64 KB mem[] image to /tmp/rof_mem.bin.
#
# For A/Bs where the thing under test lives INSIDE mem[] (e.g. "are these xex bytes dead?").
# A checksum probe cannot answer that: the blocks being summed CONTAIN the very bytes that
# were changed, so they differ trivially and the comparison is vacuous (the charset A/B read
# $3000-$BFFF as "changed" when all that changed was the zeroed code bytes themselves).
# Dump instead, then diff offline with the changed set MASKED OUT.
#
# Copy /tmp/rof_mem.bin aside after each run — the path is fixed so the script stays simple.
# Run:  . ./env.sh && GDBSCRIPT=mem_dump.gdb ./diag_run.sh 60 && cp /tmp/rof_mem.bin <arm>.bin
set width 0
set pagination off
break PlatformAmiga::renderFrame if g_vbiCount >= 2000
continue
echo \n==== FRAME-LOCKED DUMP ====\n
printf "vbiCount=%u bootScene=%u VVBLKI=%02x%02x state(41)=%02x\n", \
  g_vbiCount, g_bootScene, mem[0x0223], mem[0x0222], mem[0x0041]
dump binary memory /tmp/rof_mem.bin &mem[0] &mem[65536]
echo dumped /tmp/rof_mem.bin\n
echo ==== end ====\n
