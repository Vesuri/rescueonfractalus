# logo_probe.gdb — boot scene 1 (the Lucasfilm Games logo).  Works on a PLAIN `make` build:
# everything it reads is a normal global, and PROBES=1 would imply SKIPBOOT=1 and skip the
# very scene under test.  Drive with:  GDBSCRIPT=logo_probe.gdb ./diag_run.sh <seconds>
#
# ⚠ Every convenience variable is set AFTER the continue: gdb evaluates them eagerly, and at
# the remote_debugger_trigger breakpoint s_scene is still null (see the logo-station-port memory).
continue
echo \n==== SIGINT ====\n

echo --- where are we? (bootScene 1=Logo 2=Station 0=none) ---\n
printf "bootScene=%u loadBusy=%u vbi=%u VVBLKI=%02x%02x RTCLOK=%02x\n", \
  g_bootScene, g_bootLoadBusy, g_vbiCount, mem[0x0223], mem[0x0222], mem[0x0014]

echo --- the $51EF VBI's own state (ZP $90-$99) ---\n
printf "$90=%02x $91=%02x $92=%02x | AUDF acc $93=%02x $95=%02x $97=%02x $99=%02x\n", \
  mem[0x0090], mem[0x0091], mem[0x0092], mem[0x0093], mem[0x0095], mem[0x0097], mem[0x0099]
printf "seeds $5254: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", \
  mem[0x5254], mem[0x5255], mem[0x5256], mem[0x5257], mem[0x5258], mem[0x5259], \
  mem[0x525A], mem[0x525B], mem[0x525C], mem[0x525D], mem[0x525E]

echo --- the unpacked display list at $6000 (want: 8x70, 4F A3 60, 0F.., 41 00 60) ---\n
printf "%02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x | %02x %02x %02x | tail %02x %02x %02x\n", \
  mem[0x6000], mem[0x6001], mem[0x6002], mem[0x6003], mem[0x6004], mem[0x6005], mem[0x6006], mem[0x6007], \
  mem[0x6008], mem[0x6009], mem[0x600A], mem[0x600B], mem[0x600C], mem[0x600D], \
  mem[0x6048], mem[0x6049], mem[0x604A]

echo --- the field at $60A3: row 0 cols 0-13, and row 52 cols 12-25 (the GAMES rect) ---\n
printf "gen=%u | row0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", \
  g_logoFieldGen, \
  mem[0x60A3], mem[0x60A4], mem[0x60A5], mem[0x60A6], mem[0x60A7], mem[0x60A8], mem[0x60A9], \
  mem[0x60AA], mem[0x60AB], mem[0x60AC], mem[0x60AD], mem[0x60AE], mem[0x60AF], mem[0x60B0]
# The GAMES overlay: rows 50-60, cols 13-25.  All zero until the paste; then rows 50/55/60 must
# read dd dd 0d dd d0 dd dd d0 dd dd 0d dd d0 / e2 ee 2e ee e2 ... / 02 22 20 20 02 ...
set $g = 0
while $g < 3
  set $a = 0x60A3 + (50 + $g*5)*40 + 13
  printf "GAMES row %2d @ $%04x: ", 50 + $g*5, $a
  set $i = 0
  while $i < 13
    printf "%02x ", mem[$a + $i]
    set $i = $i + 1
  end
  printf "\n"
  set $g = $g + 1
end

echo --- the sparkle: HPOSP0 shadow, COLPM0 mirror, player-0 page $0C40-$0C4F ---\n
printf "HPOSP0=%02x COLPM0=%02x | ", mem[0xD000], g_logoSparkleCol
set $i = 0
while $i < 16
  printf "%02x ", mem[0x0C40 + $i]
  set $i = $i + 1
end
printf "\n"

echo --- renderer side (s_scene: which layout is live, and the sparkle sprite words) ---\n
set $sc = 'PlatformAmiga.cpp'::s_scene
printf "s_scene=%p bootFieldScene=%u installed=%u logoFieldGen=%u logoSparkleCol=%03x\n", \
  $sc, $sc->bootFieldScene, $sc->bootFieldCopperInstalled, $sc->logoFieldGen, $sc->logoSparkleCol
set $spr = (unsigned short*)$sc->logoSparkle->data_
printf "sprite@%p ctl=%04x %04x data=%04x %04x %04x %04x %04x %04x\n", \
  $spr, $spr[0], $spr[1], $spr[2], $spr[4], $spr[6], $spr[8], $spr[10], $spr[12]

echo --- decoded bitmap: source nibble n must land as pen n across the 4 planes ---\n
# row 0 bytes 0-7 (source 00 00 ff ff f0 2f ff f0), row 60 bytes 0-7 (source 00 00 00 22 22 22 22
# 00 -> pen 2 = plane1 ONLY), row 61 (all zero: the last row, so a stride slip shows up here).
set $bm = (unsigned char*)$sc->bootFieldBitmap->data
set $r = 0
while $r < 3
  set $row = 0
  if $r == 1
    set $row = 60
  end
  if $r == 2
    set $row = 61
  end
  set $p = 0
  while $p < 4
    printf "row %2d plane%d: ", $row, $p
    set $i = 0
    while $i < 8
      printf "%02x ", $bm[$row*160 + $p*40 + $i]
      set $i = $i + 1
    end
    printf "\n"
    set $p = $p + 1
  end
  set $r = $r + 1
end

echo --- copper geometry (want WIN_WAIT line 107 = 0x6b, BOT_WAIT line 169 = 0xa9) ---\n
set $cl = (unsigned long*)$sc->bootFieldCopper->data_
printf "d[59] WIN_WAIT=%08x  d[70] BOT_WAIT=%08x  d[62..63] BPL1PT=%08x %08x  bitmap=%p\n", \
  $cl[59], $cl[70], $cl[62], $cl[63], $bm

detach
quit
