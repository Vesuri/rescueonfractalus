# On-target differential verify for the hand-asm SFX voice-priority mixer twin
# (SfxMixerAssembler.s).  Build: make VERIFY=1 PROBES=1 COMBAT=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=sfxmix_verify.gdb ./diag_run.sh 240
#
# Each sfx_reorder_voice_slot call runs the asm twin and the C twin back-to-back on identical
# state (rof_native.c, ROF_SFXMIX_VERIFY) and compares mem[$0705-$0717] + mem[$D200-$D20F] +
# cpu.A/X/Y.  mismatch MUST be 0.  badCell names the first differing address ($C0FF = a cpu
# register diff, not a mem cell).
#
# ⚠ Judge speed by the asm/C RATIO, never by the absolute ticks/call: the same binary swings
# ~15% run-to-run on absolutes while the ratio holds to ~0.5%.  Both sides are bracketed with
# the SAME FP_TIME, so the bracket's own ~2.2 t floor is present in both and cancels in the
# ratio — but it also means the absolute per-call numbers are ~2 t too high on both sides.
#
# ⚠ Use COMBAT=1.  A quiet flight still calls the mixer (the standby/engine envelopes keep the
# ring busy) but combat is where it costs 22 t/firing, and the branch mix differs.
set pagination off
set confirm off

# The mixer runs from the flight VBI, so any in-flight breakpoint has thousands of calls behind
# it.  Sample twice so a suspiciously round result can be sanity-checked against a second point.
define report
  printf "VERIFY at vbi=%u fdCalls=%u : calls=%lu mismatch=%lu firstBad=%lu badCell=$%04lx\n", \
    g_vbiCount, g_fdCalls, g_sfxmixCalls, g_sfxmixMismatch, g_sfxmixFirstBad, g_sfxmixBadCell
  printf "  beam-ticks: asm=%lu C=%lu | per call asm=%lu.%03lu C=%lu.%03lu | ratio asm/C = %lu.%02lu%%\n", \
    g_sfxmixAsmTicks, g_sfxmixCTicks, \
    (g_sfxmixCalls ? g_sfxmixAsmTicks/g_sfxmixCalls : 0), \
    (g_sfxmixCalls ? ((1000*g_sfxmixAsmTicks)/g_sfxmixCalls)%1000 : 0), \
    (g_sfxmixCalls ? g_sfxmixCTicks/g_sfxmixCalls : 0), \
    (g_sfxmixCalls ? ((1000*g_sfxmixCTicks)/g_sfxmixCalls)%1000 : 0), \
    (g_sfxmixCTicks ? (100*g_sfxmixAsmTicks)/g_sfxmixCTicks : 0), \
    (g_sfxmixCTicks ? ((10000*g_sfxmixAsmTicks)/g_sfxmixCTicks)%100 : 0)
end

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2600
continue
report
set $sc0 = g_sfxmixCalls
set $sa0 = g_sfxmixAsmTicks
set $sb0 = g_sfxmixCTicks

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 5400
continue
report
printf "  window delta (vbi 2600->%u): calls=%lu  asm=%lu C=%lu  ratio asm/C = %lu.%02lu%%\n", \
  g_vbiCount, (g_sfxmixCalls-$sc0), (g_sfxmixAsmTicks-$sa0), (g_sfxmixCTicks-$sb0), \
  ((g_sfxmixCTicks-$sb0) ? (100*(g_sfxmixAsmTicks-$sa0))/(g_sfxmixCTicks-$sb0) : 0), \
  ((g_sfxmixCTicks-$sb0) ? ((10000*(g_sfxmixAsmTicks-$sa0))/(g_sfxmixCTicks-$sb0))%100 : 0)
printf "  VVBLKI=$%02x%02x $3D=%02x  (must be $4ff5 / 00 = live flight)  polyDist=%lu\n", \
  mem[0x223], mem[0x222], mem[0x3D], g_polyDistCalls
detach
quit
