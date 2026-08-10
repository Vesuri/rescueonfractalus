# decodeBoostViewport content-shadow differential.
#   cd amiga && make clean && make -j4 BOOST_VERIFY=1 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=boost_verify.gdb ./diag_run.sh 130
#
# g_bvBad must be 0.  g_bvGroups is the number of groups actually CHECKED (source still matched the
# shadow at verify time) — if it is 0 the test proved nothing.  g_bvRace counts groups whose source
# moved between the decode and the check: benign by construction (the next frame re-decodes them),
# but a large share means the window is racy enough that the check is only sampling.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "=== boost decode shadow differential ===\n"
printf "checked=%lu  BAD=%lu  race=%lu\n", g_bvGroups, g_bvBad, g_bvRace
if g_bvBad > 0
  printf "first mismatch: row=%lu group=%lu key=%08lx\n", g_bvFirstRow, g_bvFirstGrp, g_bvFirstKey
  printf "  p1 got=%08lx want=%08lx\n", g_bvGotP1, g_bvWantP1
  printf "  p2 got=%08lx want=%08lx\n", g_bvGotP2, g_bvWantP2
  printf "  p3 got=%08lx want=%08lx\n", g_bvGotP3, g_bvWantP3
end
printf "decodes: stars=%lu tunnel=%lu   groups stored=%lu\n", g_bStarDec, g_bTunDec, g_bTunGroups
printf "vbi=%u mem[$3A]=%02x mem[$8D]=%02x mem[$8E]=%02x\n", g_vbiCount, mem[0x3A], mem[0x8D], mem[0x8E]
