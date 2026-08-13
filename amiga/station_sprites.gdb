# station_sprites.gdb — are the station's PMG elements mirrored into their sprite channels?
# Plain build (⚠ PROBES=1/FPSCOUNT=1 imply SKIPBOOT=1 and skip the scene).  Run:
#   . ./env.sh && GDBSCRIPT=station_sprites.gdb ./diag_run.sh <secs>
# ~20 s lands late in the ~3.7 s scroll, which is the only place ALL of it is live: the two
# converging dots need phase(8B) >= $59 and the missile dots >= $38 before display_scroll paints
# them.  phase $94 = scroll finished (everything stays painted, but the ship has stopped).
#
# ⚠ NEVER name a convenience variable $d0-$d7 / $a0-$a7 / $sp / $pc / $fp: those are m68k REGISTER
# names, so `set $d0 = <ptr>` silently writes register D0 and later subscripts fail with
# "cannot subscript something of type int32_t".
#
# What to read (channel map in RescueOnFractalus.h):
#   POS = VSTART<<8 | HSTART>>1 ; CTL = VSTOP<<8 | ATT/SV8/EV8/SH0  (ATT = bit 7 of the low byte).
#   ch0 is the whole spacecraft: P0 in plane A (the even data word) and P1 in plane B (the odd one),
#   so each row prints as A/B and the pen is B<<1|A -> COLOR17/18/19 = $06/$0A/$0E.  ⚠ ATT must be 0
#   (an attached pair is 4 planes and would land P1 on COLOR20 and the overlap on COLOR21).
#   HSTART $7F -> x = 0x81 + ($7F-$32)*2 + 3 = 286 = $11E, so POS low byte $8F.
#   Every station line is pmLineStation() = kDisplayTop + off - 8 - 16.
#   The pair colours are COLOR17/18/19 = $06/$0A/$0E (the multi-colour-player OR), COLOR25 the
#   cycling dot colour, COLOR29 = COLPF3 $34.
continue
echo \n==== SIGINT ====\n
set width 0
set $sc = 'PlatformAmiga.cpp'::s_scene
printf "scene=%u phase(8B)=%02x rtclokMid(13)=%02x shipOn(9C)=%02x shipStep(98)=%02x frame(9A)=%02x\n", \
  $sc->bootFieldScene, mem[0x008B], mem[0x0013], mem[0x009C], mem[0x0098], mem[0x009A]
printf "dotIdx=%u  HPOSM=%02x %02x %02x %02x\n", mem[0x0097] & 7, \
  mem[0xD004], mem[0xD005], mem[0xD006], mem[0xD007]

# --- the attached spacecraft: control words, then both planes row by row vs the PM pages --------
set $q0 = $sc->stationSpr[0]
set $s0 = (unsigned short *)($q0->data_)
printf "ship ch0 POS=%04x CTL=%04x  ATT=%d (must be 0)\n", $s0[0], $s0[1], ($s0[1] >> 7) & 1
printf "ship  y=%d  x=%d  rows=%d\n", ($s0[0] >> 8), (($s0[0] & 0xff) << 1) | ($s0[1] & 1), \
  (($s0[1] >> 8) - ($s0[0] >> 8))
set $r = 0
while $r < 20
  printf "  row%02d  A=%04x B=%04x  overlap=%04x\n", $r, $s0[2 + $r*2], $s0[3 + $r*2], \
    $s0[2 + $r*2] & $s0[3 + $r*2]
  set $r = $r + 1
end
printf "PM p0 ($3400): "
set $r = 0
while $r < 256
  if mem[0x3400 + $r] != 0
    printf "[%02x]=%02x ", $r, mem[0x3400 + $r]
  end
  set $r = $r + 1
end
printf "\nPM p1 ($3500): "
set $r = 0
while $r < 256
  if mem[0x3500 + $r] != 0
    printf "[%02x]=%02x ", $r, mem[0x3500 + $r]
  end
  set $r = $r + 1
end
printf "\n"

# --- the two converging dots -----------------------------------------------------------------
set $i = 1
while $i < 3
  set $q = $sc->stationSpr[$i]
  set $sd = (unsigned short *)($q->data_)
  printf "dot[%d] POS=%04x CTL=%04x data0=%04x  y=%d x=%d\n", $i, $sd[0], $sd[1], $sd[2], \
    ($sd[0] >> 8), (($sd[0] & 0xff) << 1) | ($sd[1] & 1)
  set $i = $i + 1
end

# --- the missile chains ----------------------------------------------------------------------
set $c = 0
while $c < 2
  set $qm = $sc->stationMsl[$c]
  set $sm = (unsigned short *)($qm->data_)
  set $k = 0
  while $k < 6
    printf "msl[%d][%d] POS=%04x CTL=%04x px=%04x  y=%d x=%d\n", $c, $k, $sm[$k*4], $sm[$k*4+1], \
      $sm[$k*4+2], ($sm[$k*4] >> 8), (($sm[$k*4] & 0xff) << 1) | ($sm[$k*4+1] & 1)
    set $k = $k + 1
  end
  set $c = $c + 1
end

# --- the live pair colours + sprite pointer operands (INDEX_SPRCOL=21, INDEX_SPRITES=33) ------
set $bc = $sc->bootFieldCopper
set $l = (unsigned long *)($bc->data_)
printf "pair0 col1/2/3: %08x %08x %08x\n", $l[21], $l[22], $l[23]
printf "pair1/2/3 col1: %08x %08x %08x\n", $l[24], $l[27], $l[30]
set $j = 0
while $j < 8
  printf "sprPT ch%d = %04x%04x\n", $j, $l[33 + $j*2] & 0xffff, $l[34 + $j*2] & 0xffff
  set $j = $j + 1
end
echo ==== end ====\n
