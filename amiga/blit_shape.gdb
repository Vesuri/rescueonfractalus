# WHERE does the flight frame STALL?  Per-call-site beam ticks spent waiting, ISR-corrected.
#
# The PC profile says ~4-5% of flight sits in bW_waitUntilBlitterNotBusy / processBlitterQueue /
# blitterDrain and ~7% on ONE line (PlatformAmiga.cpp's frame-sync vblank spin) — but a PC sample
# cannot name the CALL SITE, and the fix for a stall is per-site (give the CPU work to do during
# it, or split the blit so only the part the CPU needs is awaited).  So bracket every wait.
#
# ⚠ The frame-sync spin is the interesting one: `pace_ab.gdb` reports g_idleFrames = 0.00
# frames/iteration, which was read as "flight is 100% compute-bound, there is no wait".  That
# counter is INTEGER FRAMES (g_vbiCount delta across the spin) and the deferred-flip scheme
# normally SKIPS the spin entirely — so it can read 0 while sub-frame stalls elsewhere are real.
# g_rIdleWall (already in the tree, never printed until now) is the same window in beam TICKS.
#
# ⚠ gdb convenience variables must NOT be named $a0..$a7 / $d0..$d7 — those are m68k REGISTER
# names, so `set $a0 = ...` writes the CPU register instead (it silently clobbered a0/d0 at the
# breakpoint, then failed with "Argument to arithmetic operation not a number").  Use $sXxx.
# Build: make clean && make -j4 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 BLIT_SHAPE=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=blit_shape.gdb ./diag_run.sh 200
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2000
continue
set $sVbi = (int)g_vbiCount
set $sFd = (int)g_fdCalls
set $sDot = (int)g_bwDotClear
set $sCC = (int)g_bwClearCopy
set $sSky = (int)g_bwSkyFill
set $sPend = (int)g_bwPendClear
set $sFlip = (int)g_bwFlip
set $sIdle = (int)g_rIdleWall
set $sWall = (int)g_rRenderWall
set $sRc = (int)g_rCalls

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 5000
continue
set $dv = (int)g_vbiCount - $sVbi
set $df = (int)g_fdCalls  - $sFd
set $dk = (int)g_rCalls   - $sRc
# total flight beam ticks in the window = vbi frames * 313 scanlines
set $tot = $dv * 313

printf "window: %d vbi (%d ticks)  painted=%d  renderFrame=%d   VVBLKI=$%02x%02x $3D=%02x\n", \
  $dv, $tot, $df, $dk, mem[0x223], mem[0x222], mem[0x3D]
printf "-- STALLS (beam ticks, ISR-corrected) --------------------------------\n"
printf "  frame-sync vblank spin  %7d  = %2d.%d%% of flight   (%d/renderFrame)\n", \
  (int)g_rIdleWall-$sIdle, (100*((int)g_rIdleWall-$sIdle))/$tot, \
  ((1000*((int)g_rIdleWall-$sIdle))/$tot)%10, ((int)g_rIdleWall-$sIdle)/$dk
printf "  blit: clear+copy drain  %7d  = %2d.%d%%             (%d/painted)\n", \
  (int)g_bwClearCopy-$sCC, (100*((int)g_bwClearCopy-$sCC))/$tot, \
  ((1000*((int)g_bwClearCopy-$sCC))/$tot)%10, ((int)g_bwClearCopy-$sCC)/$df
printf "  blit: sky-fill wait     %7d  = %2d.%d%%             (%d/painted)\n", \
  (int)g_bwSkyFill-$sSky, (100*((int)g_bwSkyFill-$sSky))/$tot, \
  ((1000*((int)g_bwSkyFill-$sSky))/$tot)%10, ((int)g_bwSkyFill-$sSky)/$df
printf "  blit: dot-clear wait    %7d  = %2d.%d%%             (terrain_draw entry)\n", \
  (int)g_bwDotClear-$sDot, (100*((int)g_bwDotClear-$sDot))/$tot, \
  ((1000*((int)g_bwDotClear-$sDot))/$tot)%10
printf "  blit: pending-clear     %7d  = %2d.%d%%\n", \
  (int)g_bwPendClear-$sPend, (100*((int)g_bwPendClear-$sPend))/$tot, \
  ((1000*((int)g_bwPendClear-$sPend))/$tot)%10
printf "  blit: flip drains       %7d  = %2d.%d%%\n", \
  (int)g_bwFlip-$sFlip, (100*((int)g_bwFlip-$sFlip))/$tot, \
  ((1000*((int)g_bwFlip-$sFlip))/$tot)%10
printf "  renderFrame scene wall  %7d  = %2d.%d%%   (whole scene->renderFrame(), for scale)\n", \
  (int)g_rRenderWall-$sWall, (100*((int)g_rRenderWall-$sWall))/$tot, \
  ((1000*((int)g_rRenderWall-$sWall))/$tot)%10
detach
quit
