# Workbench-startup protocol probe (src/main.cpp wbGetStartupMessage/wbReplyStartupMessage).
# Run: GDBSCRIPT=wbstartup.gdb ./diag_run.sh 40
# diag_run.sh launches the game from a startup-sequence, i.e. a CLI launch, so this probe's
# job is to show pr_CLI != 0 and NO message taken -- the unchanged path.  A real Workbench
# (icon) launch cannot be driven from this harness.
break main
continue
echo \n==== at main ====\n
# FindTask(NULL) -> the Process; pr_CLI at +172, pr_MsgPort at +92.
set $me = *(unsigned long *)((*(unsigned long *)0x4) + 276)
printf "ThisTask     = %08x\n", $me
printf "pr_CLI       = %08x   (non-zero = CLI/WHDLoad launch, no WBStartup message)\n", \
  *(unsigned long *)($me + 172)
# An EMPTY exec List has lh_Head == &lh_Tail, not 0 -- compare, do not test for zero.
printf "pr_MsgPort empty = %d   (must be 1: a CLI launch is sent no message)\n", \
  (*(unsigned long *)($me + 92 + 20) == ($me + 92 + 24))
delete
continue
echo \n==== SIGINT: still running? ====\n
printf "g_vbiCount = %u\n", g_vbiCount
