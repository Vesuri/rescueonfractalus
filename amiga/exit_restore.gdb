# Exit-path verification: does PlatformAmiga::run()'s system restore run to completion?
#
# Needs `make FORCE_QUIT=<vbl>` (the click cannot be injected from here — the FS-UAE stub drops
# memory writes; see docs/headless-fsuae.md).  Run e.g.
#   make clean && make FORCE_QUIT=900 && GDBSCRIPT=exit_restore.gdb ./diag_run.sh 90
#
# rof_hiscore_flush is the LAST statement of run(), reached only after the whole restore
# (VBI off -> display off -> COP1LC=copinit -> free -> VERTB vector back -> DMACON/INTENA
# back -> Permit + LoadView + WaitTOF x2).  Reaching it at all is the pass condition: a
# hang or a use-after-free in the teardown stops somewhere earlier instead.
set pagination off
set confirm off

break rof_hiscore_flush
continue

printf "RESTORE-COMPLETE\n"
printf "  DMACONR  now=%04x  saved=%04x\n", *(unsigned short *)0xdff002, 'PlatformAmiga.cpp'::s_savedDmacon
printf "  INTENAR  now=%04x  saved=%04x\n", *(unsigned short *)0xdff01c, 'PlatformAmiga.cpp'::s_savedIntena
delete
continue
# COP1LC and BPLCON3 are WRITE-ONLY — there is no read path to assert them from here, so what
# this script proves is the two readable halves plus liveness: the restore ran to completion,
# and DMACON/INTENA came back bit-for-bit.  ⚠ In THIS harness the boot shell has no window open,
# so the OS has no View and no bitplane DMA (saved DMACONR = 02d0, no RASTER) — which is why the
# Workbench half of the restore still needs an eyes-on run.sh check, not this script.
