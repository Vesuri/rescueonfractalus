# station_sprites.gdb — are the station's five PMG elements mirrored into their sprite channels?
# Plain build.  Run:  . ./env.sh && GDBSCRIPT=station_sprites.gdb ./diag_run.sh <secs>
#
# What to read (channel map in RescueOnFractalus.h):
#   Each row prints the sprite's two control words plus its first data word.
#   POS = VSTART<<8 | HSTART>>1 ; CTL = VSTOP<<8 | ATT/SV8/EV8/SH0.
#   P0/P1 must sit at HSTART $7F -> POS low byte $B0 (x = 0x81 + ($7F-$32)*2 = 0x161, >>1 = $B0).
#   P2/P3 x come from $1F30/$1F38[i], i = $0097 & 7, and share COLOR25.
#   The missile chains are up to 6 single-line entries each, terminated by a 0,0 pair.
#   The pair colours are COLOR17 (=$06), COLOR21 (=$0A), COLOR25 (cycling), COLOR29 (=$34).
continue
echo \n==== SIGINT ====\n
set $sc = 'PlatformAmiga.cpp'::s_scene
printf "scene=%u phase(8B)=%02x  dotIdx=%u  HPOSM=%02x %02x %02x %02x\n", \
  $sc->bootFieldScene, mem[0x008B], mem[0x0097] & 7, \
  mem[0xD004], mem[0xD005], mem[0xD006], mem[0xD007]
set $i = 0
while $i < 4
  set $d = (unsigned short *)$sc->stationSpr[$i]->data_
  printf "spr[%d] POS=%04x CTL=%04x data0=%04x%04x\n", $i, $d[0], $d[1], $d[2], $d[3]
  set $i = $i + 1
end
set $c = 0
while $c < 2
  set $d = (unsigned short *)$sc->stationMsl[$c]->data_
  set $k = 0
  while $k < 6
    printf "msl[%d][%d] POS=%04x CTL=%04x px=%04x\n", $c, $k, $d[$k*4], $d[$k*4+1], $d[$k*4+2]
    set $k = $k + 1
  end
  set $c = $c + 1
end
# The live pair colours + sprite pointer operands in the copper list (INDEX_SPRCOL=21, SPRITES=33).
set $l = (unsigned long *)$sc->bootFieldCopper->data_
printf "pairCol: %08x %08x %08x %08x\n", $l[21], $l[24], $l[27], $l[30]
printf "sprPT ch0=%04x%04x ch4=%04x%04x ch6=%04x%04x\n", \
  $l[33] & 0xffff, $l[34] & 0xffff, $l[41] & 0xffff, $l[42] & 0xffff, $l[45] & 0xffff, $l[46] & 0xffff
echo ==== end ====\n
