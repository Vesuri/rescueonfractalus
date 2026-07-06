# One-shot verify that MulTableAssembler.s builds g_mulTable byte-identically to the full
# C bit-serial oracle.  Build:  make clean && make -j4 PROBES=1 VERIFY=1
# Run:  GDBSCRIPT=multable_verify.gdb ./diag_run.sh 8
# g_mulTableMismatch is set once at startup (rof_mul_table_init memcmps the asm-built
# g_mulTable against the C-built g_mulTableRef); 0 = byte-identical.
continue
echo \n==== mul_table verify ====\n
printf "g_mulTableReady = %d  g_mulTableMismatch = %lu  (ready=1 & mismatch=0 => asm byte-identical)\n", g_mulTableReady, g_mulTableMismatch
detach
quit
