# Object draw-order loop (terrain_draw_objects) SHAPE probe.
#
# The PC profile puts 8.3% of ALL wall clock on terrain_draw_objects' OWN body — the
# order-table walk, the visibility tests, the ten per-pair mem-to-mem vector copies and the
# $272E index save/restore.  Its callees (project / plot_object / subdivide) are separate
# symbols, so that share is pure bookkeeping.  An analytical estimate of that bookkeeping
# came out ~2.4%, i.e. 3.5x smaller — so the PAIR COUNTS have to be measured before any
# rewrite is sized.  (Rule: size a sub-bucket with a counter, never by reading the profile
# harder.)
#
# Prints, per terrain_draw_frame pass:
#   pairs   = loop iterations           (g_tdPairs)
#   culled  = primary off-screen/culled (g_tdCulled)   -> the cheap early-continue path
#   vis     = both endpoints visible    (g_tdVisPairs) -> the path that does the 10 copies
#   proj    = project+plot calls        (g_tdProjCount)
# pairs - culled - vis = pairs whose COMPANION was culled (read obj1, then fall through).
#
# Build: make clean && make -j4 COMBAT=1 PROBES=1 FIXED_RNG=1 PROFILE_NORING=1
#        (ROF_TDRAW_PROF must stay ON — do NOT pass NO_TDRAW_PROF=1)
# Run:   . ./env.sh && GDBSCRIPT=objloop_shape.gdb ./diag_run.sh 220
set pagination off
set confirm off

define osnap
  set $p0 = (unsigned long)g_tdPairs
  set $c0 = (unsigned long)g_tdCulled
  set $v0 = (unsigned long)g_tdVisPairs
  set $j0 = (unsigned long)g_tdProjCount
  set $f0 = (unsigned long)g_tdFrames
end

define oseg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  set $df = (unsigned long)g_tdFrames - $f0
  if $df > 0
    printf "vbi %5u passes %4u | pairs/pass %5u  culled %5u  vis %5u  proj %5u | VVBLKI=$%02x%02x $3D=%02x\n", \
      g_vbiCount, $df, \
      ((unsigned long)g_tdPairs     - $p0) / $df, \
      ((unsigned long)g_tdCulled    - $c0) / $df, \
      ((unsigned long)g_tdVisPairs  - $v0) / $df, \
      ((unsigned long)g_tdProjCount - $j0) / $df, \
      mem[0x223], mem[0x222], mem[0x3D]
  end
  osnap
end

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== level=%u ($006D)  $0623 emplace-gate=%02x  $0621 saucer-period=%02x  $0624 fire-mask=%02x ===\n", \
  mem[0x6D], mem[0x623], mem[0x621], mem[0x624]
printf "=== start: vbi=%u VVBLKI=$%02x%02x $3D=%02x ===\n", g_vbiCount, mem[0x223], mem[0x222], mem[0x3D]
osnap

oseg 2200
oseg 2500
oseg 2800
oseg 3100
oseg 3400
oseg 3700
oseg 4000

printf "=== end: vbi=%u VVBLKI=$%02x%02x $3D=%02x  totalPasses=%u ===\n", \
  g_vbiCount, mem[0x223], mem[0x222], mem[0x3D], g_tdFrames
detach
quit
