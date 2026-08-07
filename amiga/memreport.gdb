# RAM budget — what the game actually costs at runtime.
#
#   make clean && make -j4                      (any build; the report is unconditional)
#   . ./env.sh && GDBSCRIPT=memreport.gdb ./debug.sh      (or diag_run.sh 30 on a PROBES build)
#
# The LOAD IMAGE is readable offline (objdump -h out/RoF.elf, non-.debug sections).  What is
# not is the RUNTIME side: the framework AllocMem()s every bitmap, copper list, sprite and
# audio buffer in CHIP.  PlatformAmiga::run() snapshots exec's free pools before the scene's
# constructor and again after the display takeover, so the delta IS the allocation total.
#
# ⚠ The probe scripts boot FS-UAE as --amiga_model=A500+ --chip_memory=1024 --fast_memory=8192,
# i.e. 1 MB chip + 8 MB fast.  A bare A500 has neither; read the CHIP column as the one that
# has to fit, and remember exec + the loaded hunks are ALREADY subtracted from "before".
set pagination off
set confirm off

tbreak RescueOnFractalus::run
continue

printf "\n=== RAM at scene start ===\n"
printf "  free CHIP : %8lu -> %8lu   (allocated %lu bytes = %lu KB)\n", \
  g_memChipBefore, g_memChipAfter, (g_memChipBefore-g_memChipAfter), \
  (g_memChipBefore-g_memChipAfter)/1024
printf "  free FAST : %8lu -> %8lu   (allocated %lu bytes)\n", \
  g_memFastBefore, g_memFastAfter, (g_memFastBefore-g_memFastAfter)
printf "  free ANY  : %8lu -> %8lu   (allocated %lu bytes = %lu KB)\n", \
  g_memAnyBefore, g_memAnyAfter, (g_memAnyBefore-g_memAnyAfter), \
  (g_memAnyBefore-g_memAnyAfter)/1024
printf "  largest free CHIP block after setup: %lu bytes (%lu KB)\n", \
  g_memChipLargest, g_memChipLargest/1024
detach
quit
