# Is the once-per-pass SubPt slot-0 publish equal to what the per-pair seed used to leave?
#
# ROF_SUBDIV_OBJ_SEED0 stops terrain_draw_objects copying obj1's projected vector into slot 0 on
# every visible pair; the asm reads the far endpoint out of the object arrays instead and only
# materialises slot 0 when the recursion descends, with the caller publishing the LAST visible
# pair's vector once the loop is done.
#
# ⚠ This needs its own instrument.  NOTHING in the linked image reads those five cells (survey:
# docs/flight-perf-log.md §24.2), so no differential can see a wrong publish; and a cross-build
# A/B cannot either, because a render-speed change makes the two builds fly different ground
# within a few frames (§19) — the cells then differ legitimately.  A probe that simply re-read
# obj1's vector at the end would be vacuous, since that is what the publish does.
#
# So the C side (ROF_SEED0_VERIFY) shadows the vector AT THE TIME OF EACH PAIR and compares the
# shadow against what the publish left.  That tests the real claim: obj1's projected vector
# cannot move between its pair and the end of the pass.
#
# Build: make clean && make SEED0_VERIFY=1 PROBES=1 FIXED_RNG=1 COMBAT=1 COMBAT_QUIET=1
# Run:   . ./env.sh && GDBSCRIPT=seed0_verify.gdb ./diag_run.sh 240
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 150
continue
printf "SEED0 fd=%u vbi=%u passes=%lu mismatch=%lu  (first bad: cell %lu want %02lx got %02lx)\n", \
  g_fdCalls, g_vbiCount, g_seed0Passes, g_seed0Mismatch, \
  g_seed0BadIdx, g_seed0BadWant, g_seed0BadGot
detach
quit
