# HONEST flight framerate: painted terrain frames per real 50 Hz vblank, sampled in SHORT
# segments so a crash / death cinematic inside the window cannot silently deflate the number.
#
# FPS = 50 * g_fpsFrames / g_vbiCount.  Both are plain integer counters, and the ratio is
# frames-per-EMULATED-vblank, so host speed and the gdb stub's own slowness cancel out
# completely — this is safe to read under the remote debugger.
#
# TWO TRAPS this script exists to avoid, both of which produced badly wrong numbers on
# 2026-08-06:
#  1. INSTRUMENTATION.  Build with `make FPSCOUNT=1` and NOTHING else.  It adds only the
#     headless auto-launch plus one increment per painted frame.  A PROBES build is 20-30%
#     slower (ROF_FLIGHT_PROBE's FP_TIME brackets = 2 CHIP register reads + a 16x16 multiply,
#     several times per iteration; ROF_TDRAW_PROF adds ~6 more per visible object pair), and
#     every flight FPS figure taken before this script was measured on one.
#  2. THE CRASH.  The auto-launch flies with NO input, so the ship eventually hits a mountain.
#     renderFlightDirect stops being called during the death cinematic while g_vbiCount keeps
#     ticking, so ANY wide window straddling the crash under-reports badly.  Each row below is
#     independently checkable: trust only rows with VVBLKI=$4ff5 AND $3D=00 AND an fd/vbi in
#     line with their neighbours.
#
# Build: make clean && make -j4 FPSCOUNT=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=fps_seg.gdb ./diag_run.sh 200
# (For a PROBES build, swap g_fpsFrames -> g_fdCalls to compare the two and size the probe cost.)
set pagination off
set confirm off

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  set $dv = (int)g_vbiCount   - (int)$pv
  set $df = (int)g_fpsFrames  - (int)$pf
  printf "vbi %5u  +%4d vbi  +%4d painted  fd/vbi=0.%03d  FPS=%2d.%d   VVBLKI=$%02x%02x $3D=%02x alt=%02x\n", \
    g_vbiCount, $dv, $df, ($df*1000)/$dv, (50*$df)/$dv, ((500*$df)/$dv)%10, \
    mem[0x223], mem[0x222], mem[0x3D], mem[0x28D9]
  set $pv = (int)g_vbiCount
  set $pf = (int)g_fpsFrames
end

# first checkpoint: the flight VBI ($4FF5) is up shortly after vbi ~1750
tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
set $pv = (int)g_vbiCount
set $pf = (int)g_fpsFrames
printf "=== start: vbi=%u frames=%lu VVBLKI=$%02x%02x ===\n", \
  g_vbiCount, g_fpsFrames, mem[0x223], mem[0x222]

seg 2100
seg 2300
seg 2500
seg 2700
seg 2900
seg 3100
seg 3300
seg 3500
seg 3700
seg 3900
seg 4100
seg 4300
seg 4500
seg 4700
seg 4900
detach
quit
