# Manual-launch init-timing read for debug.sh:
#   cd amiga && make clean && make -j4 PROBES=1 NOAUTO=1
#   ./debug.sh <kickstart> read_init.gdb
# At the gdb prompt it `continue`s.  In the FS-UAE window: click it, press RETURN to
# launch (manual START), let it descend into terrain flight.  Then Ctrl-C in the gdb
# terminal — the reads below fire (the counters are stamped DURING init, so any time
# after you've reached flight is fine).
continue
echo \n==== BREAK: manual-launch init timing ====\n
printf "NOW: iterCount=%u vbi=%u VVBLKI=%02x%02x joySaved(4A)=%02x game_state(41)=%02x alt(34)=%02x DCnow=%02x\n", \
  g_iterCount, g_vbiCount, mem[0x0223], mem[0x0222], mem[0x004A], mem[0x0041], mem[0x0034], mem[0x00DC]
printf "entry(FlightCopper): vbi=%u  loopStart(L_3eba): vbi=%u  fadeStart(1st DC change): vbi=%u\n", \
  g_fadeEntryVbi, g_fadeLoopVbi, g_fadeStartVbi
printf "SPLIT: entry->loop(INIT)=%u frames (~%ums) | loop->fade(DESCENT)=%u frames (~%ums) | entry->fade TOTAL=%u frames (~%ums)\n", \
  (g_fadeLoopVbi-g_fadeEntryVbi), (g_fadeLoopVbi-g_fadeEntryVbi)*20, \
  (g_fadeStartVbi-g_fadeLoopVbi), (g_fadeStartVbi-g_fadeLoopVbi)*20, \
  (g_fadeStartVbi-g_fadeEntryVbi), (g_fadeStartVbi-g_fadeEntryVbi)*20
echo --- RTCLOK / ISR-rate: does the flight ISR advance RTCLOK once per vblank? ---\n
printf "rtJumpMax=%u rtJumpGt1Count=%u  rtZero=%u (nobody advanced RTCLOK)  rtDouble=%u (double-advanced)\n", \
  g_rtJumpMax, g_rtJumpGt1Count, g_rtZeroCount, g_rtDoubleCount
printf "isrCalls(flight VBI firings)=%lu  (from flight entry; compare to vbi span entry..now = %u)\n", \
  g_flightProf.isrCalls, (g_vbiCount - g_fadeEntryVbi)
printf "maxRenderGap=%u frames atVbi=%u (longest no-render stretch)\n", g_maxRenderGap, g_maxGapAtVbi
detach
quit
