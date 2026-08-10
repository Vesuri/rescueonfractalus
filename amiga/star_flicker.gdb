# Star/instrument sprite flicker probe (the A1200 report).  Needs `make PROBES=1`.
#   . ./env.sh && GDBSCRIPT=star_flicker.gdb ./diag_run.sh 45                 # A500+ (target)
#   . ./env.sh && AMIGA_MODEL=A1200 GDBSCRIPT=star_flicker.gdb ./diag_run.sh 45
#
# The star update runs entirely in the VBI ISR and has three beam deadlines.  All three must hold
# on EVERY machine, so re-run this on the fastest CPU you can before calling it fixed:
#   pubLate  = the copper SPRxPT operand was published at or after scanline 16, where the copper
#              has already read it — the sprite then fetches star pixels as its control words
#              (garbage VSTART/VSTOP) and the stars + the channel-2 throttle gauge drop out.
#              MUST be 0.  (This is what the old perFrameWork publish got wrong: 31 of 32
#              advances landed at line <= 16 on a 68040.)
#   late     = the control words / new rows finished after scanline 25, past the sprite's control
#              DMA fetch.  MUST be 0.
continue
echo \n==== SIGINT ====\n
printf "vbi=%u  VVBLKI=%02x%02x\n", g_vbiCount, mem[0x0223], mem[0x0222]
printf "starVbi: entryLine<=%u  pubLine<=%u  LATE-PUB(>=16)=%u atVbi=%u   [deadline: copper reads SPRxPT at line 16]\n", \
  g_starVbiEntryLineMax, g_starPubLineMax, g_starPubLate, g_starPubLateAtVbi
printf "         exitLine<=%u atVbi=%u  late(>25)=%u / calls=%u  [deadline: sprite ctrl DMA fetch ~line 25]\n", \
  g_starVbiExitLine, g_starVbiExitLineAtVbi, g_starVbiLateCount, g_starVbiCalls
printf "         perFrameWork stars calls=%u (build-only now)\n", g_pfwStarCalls
detach
quit
