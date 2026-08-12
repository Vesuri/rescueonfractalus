# In-place level-select elevator scroll: is doorScrollVblankUpdate driving it at all?
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=$CLAUDE_JOB_DIR/tmp/ipscroll.gdb ./diag_run.sh 150
set pagination off
continue
echo \n==== SIGINT ====\n
printf "in-place cycle: dispatch=%u inPlace=%u doorScroll=%u introWrap=%u\n", g_ipDispatch, g_ipInPlace, g_ipDoorScroll, g_ipIntroWrap
printf "scroll ISR:     setTerrainRuns writes=%u  late(>=171)=%u  worst line=%u  decodes=%u\n", g_dsRunWrites, g_dsRunLate, g_dsRunMaxLn, g_dsDecodes
printf "state:          liveCopper=%u  doorFieldReady=%u  standbyRevealReady=%u  boostRet=%u boostVp=%u\n", g_liveCopper, g_doorFieldReady, g_standbyRevealReady, g_boostRet, g_boostVp
printf "dl_src_index $008B=%u   $003A=%u   level_stage $006D=%u   $0609=%u\n", mem[0x8b], mem[0x3a], mem[0x6d], mem[0x609]
printf "run-rewrite ring (newest last):\n"
set $i = 0
while $i < 24
  if g_dsRunLine[$i] != 0
    printf "  [%2d] line=%3u dec=%u runs=%u\n", $i, g_dsRunLine[$i], g_dsRunDec[$i], g_dsRunN[$i]
  end
  set $i = $i + 1
end
printf "next ring slot=%u\n", g_dsRunIdx
