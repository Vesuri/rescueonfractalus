# Flight-VBI cost A/B — the END-TO-END number for a change inside the 50 Hz ISR.
#
# Build BOTH of these and run this script on each:
#   make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1
#   make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1 SFXMIX_C=1
# Run: . ./env.sh && GDBSCRIPT=isr_ab.gdb ./diag_run.sh 300
#
# Why this is a LEGITIMATE cross-build comparison when flight throughput is not: the ISR fires
# 50x/s regardless of frame rate, and the metric is ticks PER FIRING (isrLines/isrCalls), not
# work per frame.  So it does not depend on how far the main loop got, which is the trap that
# makes cross-build iter/frame numbers invalid.
# ⚠ It DOES still depend on the audio/combat workload being comparable, because a different
# event mix means a different number of mixer calls per firing.  That is what the ACTIVITY line
# is for: compare ring/firing, expl, fire and impact between the two runs and only trust the
# delta if they are close.  If they diverge, the two runs are not flying the same fight and the
# comparison is void — say so instead of quoting the number.
#
# ⚠ Read the handler cost as g_flightProf.isrLines / isrCalls.  NOT g_isrBeamLines (it
# deliberately includes a ~68 t/firing ZP-audit probe) and NOT over g_vbiCount (which counts the
# ~1900 boot/cinematic vblanks too).  And SNAPSHOT: g_pSfx/g_pSfxRing/g_pInteg/g_pProj all
# accumulate from POWER-ON (vbi_shared_tail runs the sfx tick on standby vblanks too) while
# isrCalls is flight-only — mixing the scopes overstates the sfx share by ~6%.
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2600
continue
printf "=== snapshot in flight at vbi %u (level %u, $0623=%02x $0621=%02x) ===\n", \
  g_vbiCount, mem[0x6D], mem[0x623], mem[0x621]
set $s_isrn = g_flightProf.isrCalls
set $s_isrl = g_flightProf.isrLines
set $s_sfx  = g_pSfx
set $s_ring = g_pSfxRing
set $s_loop = g_pSfxLoop
set $s_integ = g_pInteg
set $s_proj  = g_pProj
set $s_iters = g_sfxRingIters
set $s_fd   = g_fdCalls
set $s_vbi  = g_vbiCount

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 5400
continue

set $n  = g_flightProf.isrCalls - $s_isrn
set $il = g_flightProf.isrLines - $s_isrl
printf "\n=== window vbi %u -> %u : %lu firings ===\n", $s_vbi, g_vbiCount, $n
printf "  ACTIVITY  ring/firing=%lu.%02lu  expl=%u fire=%u impact=%u  painted=%u  head=%02x%02x roll=%02x%02x\n", \
  ($n ? (g_sfxRingIters-$s_iters)/$n : 0), ($n ? ((100*(g_sfxRingIters-$s_iters))/$n)%100 : 0), \
  g_clExplode, g_clEnemyFire, g_clImpact, (g_fdCalls-$s_fd), \
  mem[0x2886], mem[0x2885], mem[0x0029], mem[0x0028]
printf "  VVBLKI=$%02x%02x $3D=%02x  (must be $4ff5 / 00)\n", mem[0x223], mem[0x222], mem[0x3D]
printf "\n  *** flight VBI = %lu.%02lu t/firing  <-- THE A/B NUMBER ***\n", \
  ($n ? $il/$n : 0), ($n ? ((100*$il)/$n)%100 : 0)
define d
  printf "  %-28s %8lu t   %lu.%02lu t/firing  = %2lu%% of the ISR\n", $arg1, $arg0, \
    ($n ? $arg0/$n : 0), ($n ? ((100*$arg0)/$n)%100 : 0), ($il ? (100*$arg0)/$il : 0)
end
d (g_pSfx-$s_sfx)      "sfx total"
d (g_pSfxRing-$s_ring) "  ...event-ring drain"
d (g_pSfxLoop-$s_loop) "  ...14-slot envelope loop"
d (g_pInteg-$s_integ)  "integ (50Hz motion sim)"
d (g_pProj-$s_proj)    "proj (terrain scanline)"
detach
quit
