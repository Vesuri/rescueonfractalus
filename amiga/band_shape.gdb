# Shape of the g_fdBand bucket ONLY (the windscreen-band composite + its two neighbours).
#
# shape_probe.gdb prints this section too, but it prints the EDGE and TFS sections FIRST and a gdb
# script ABORTS on the first missing symbol — so on a BAND_SHAPE-only build it never reaches the
# band lines.  This script is the band half on its own.
#
# What it answers:
#   * the three sub-brackets of g_fdBand (object overlay / crosshair / band composite paint), so a
#     change to one of them has the other two as same-run controls;
#   * the per-row, per-byte ow!=0 map — which bytes of each band row take the plane-1/2 RMW.  Read
#     it in groups of FOUR (the long-wise paint's granularity): a group with no `1` in it is a long
#     the paint can skip, a group with any `1` is a long it must do.
#
# Build: make clean && make -j4 BAND_SHAPE=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 \
#                              FIXED_RNG=1 COMBAT=1 COMBAT_QUIET=1
# Run:   . ./env.sh && GDBSCRIPT=band_shape.gdb ./diag_run.sh 200
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2200
continue
printf "=== start: level=%u  VVBLKI=$%02x%02x $3D=%02x  (must be $4ff5 / 00) ===\n", \
  mem[0x6D], mem[0x223], mem[0x222], mem[0x3D]

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u | frames %5lu | obj %4lu cross %4lu band %5lu (x1000 t/frame) | VVBLKI=$%02x%02x $3D=%02x\n", \
    g_vbiCount, g_bsBandFrames, \
    (g_bsBandFrames? 1000*g_bsObj/g_bsBandFrames:0), \
    (g_bsBandFrames? 1000*g_bsCross/g_bsBandFrames:0), \
    (g_bsBandFrames? 1000*g_bsBand/g_bsBandFrames:0), \
    mem[0x223], mem[0x222], mem[0x3D]
end

seg 2800
seg 3400
seg 4000
seg 4600

printf "\n---- g_fdBand split (x1000 ticks/painted frame) ----\n"
printf "  object overlay %lu   crosshair %lu   band composite(paint) %lu\n", \
  (g_bsBandFrames? 1000*g_bsObj/g_bsBandFrames:0), \
  (g_bsBandFrames? 1000*g_bsCross/g_bsBandFrames:0), \
  (g_bsBandFrames? 1000*g_bsBand/g_bsBandFrames:0)
printf "  raw: obj=%lu cross=%lu band=%lu  frames=%lu\n", \
  g_bsObj, g_bsCross, g_bsBand, g_bsBandFrames
printf "\n---- band field stability ----\n"
printf "  changed bytes/frame = %lu/10 of 160   clean frames = %lu/%lu   max chg = %lu\n", \
  (g_bsBandFrames? 10*g_bsBandChanged/g_bsBandFrames:0), \
  g_bsBandClean, g_bsBandFrames, g_bsBandMaxChg
printf "  overwrite(ow!=0) bytes/frame = %lu of 160\n", \
  (g_bsBandFrames? g_bsBandOwNz/g_bsBandFrames:0)
echo \n---- per row: chg = ever changed, ow = ever ow!=0 (byte 0..39, read in 4s) ----\n
set $r = 0
while $r < 4
  printf "  row %d chg: ", 43 + $r
  set $b = 0
  while $b < 40
    printf "%c", (g_bsChgPos[$r*40 + $b] ? 49 : 46)
    if ($b & 3) == 3
      printf " "
    end
    set $b = $b + 1
  end
  printf "\n  row %d ow : ", 43 + $r
  set $b = 0
  while $b < 40
    printf "%c", (g_bsOwPos[$r*40 + $b] ? 49 : 46)
    if ($b & 3) == 3
      printf " "
    end
    set $b = $b + 1
  end
  printf "\n"
  set $r = $r + 1
end
detach
quit
