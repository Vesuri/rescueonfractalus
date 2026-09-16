continue
echo \n==== SIGINT (sprite/copper probe) ====\n
printf "vbiNOW=%u iterCount=%u  flightCopper=0x%08x\n", g_vbiCount, g_iterCount, g_flightCopperAddr
printf "altimShipSpr=0x%08x  viewportP3Spr=0x%08x  scopeP3Spr=0x%08x\n", g_altimShipSprAddr, g_viewportP3SprAddr, g_scopeP3SprAddr
printf "mem: 2870(vpX)=%02x  CC(scopeX)=%02x  D6(shipCol)=%02x  D9(P3col)=%02x  281B(shipTop)=%02x  0063(objidx)=%02x\n", \
  mem[0x2870], mem[0x00CC], mem[0x00D6], mem[0x00D9], mem[0x281B], mem[0x0063]
# --- sprite control words: header[0]=VSTART, header[2]=VSTOP, header[3]=flags ---
echo --- altimeter ship sprite control words (VSTART/VSTOP/flags) ---\n
set $a = g_altimShipSprAddr
printf "  altimShip: VSTART=%u VSTOP=%u flags=%02x  first data words=%04x %04x\n", \
  *(unsigned char*)$a, *(unsigned char*)($a+2), *(unsigned char*)($a+3), *(unsigned short*)($a+4), *(unsigned short*)($a+6)
echo --- viewport P3 sprite control words ---\n
set $v = g_viewportP3SprAddr
printf "  viewportP3: VSTART=%u VSTOP=%u flags=%02x  data[0..3]=%04x %04x %04x %04x\n", \
  *(unsigned char*)$v, *(unsigned char*)($v+2), *(unsigned char*)($v+3), \
  *(unsigned short*)($v+4), *(unsigned short*)($v+6), *(unsigned short*)($v+8), *(unsigned short*)($v+10)
echo --- scan flight copper list for ALL sprite-pointer moves (reg 0x120..0x13E) ---\n
set $c = g_flightCopperAddr
set $i = 0
while $i < 600
  set $w = *(unsigned int*)($c + $i*4)
  set $hi = ($w >> 16) & 0x1ff
  if $hi >= 0x120 && $hi <= 0x13e
    printf "  [%3d] reg=%03x val=%04x  (ch%d %s)\n", $i, $hi, ($w & 0xffff), ($hi-0x120)/4, ($hi & 2) ? "PTL" : "PTH"
  end
  set $i = $i + 1
end
echo --- dashboard priority and playfield-gauge colour moves ---\n
set $i = 1
while $i < 600
  set $w = *(unsigned int*)($c + $i*4)
  set $hi = ($w >> 16) & 0x1ff
  if $hi == 0x104 || $hi == 0x19c || $hi == 0x19e
    printf "  [%3d] prev=%08x reg=%03x val=%04x\n", $i, *(unsigned int*)($c + ($i-1)*4), $hi, ($w & 0xffff)
  end
  set $i = $i + 1
end
echo --- enhanced gauge rectangle pen audit (expected left=14 right=15, 8x56 each) ---\n
set $badAlt = 0
set $badEnergy = 0
set $y = 16
while $y < 72
  set $x = 0
  while $x < 8
    set $ax = 108 + $x
    set $ex = 204 + $x
    set $am = 0x80 >> ($ax & 7)
    set $em = 0x80 >> ($ex & 7)
    set $ab = g_cockpitBmpAddr + $y*160 + ($ax >> 3)
    set $eb = g_cockpitBmpAddr + $y*160 + ($ex >> 3)
    set $ap = ((*(unsigned char*)$ab & $am) != 0) | (((*(unsigned char*)($ab+40) & $am) != 0) << 1) | (((*(unsigned char*)($ab+80) & $am) != 0) << 2) | (((*(unsigned char*)($ab+120) & $am) != 0) << 3)
    set $ep = ((*(unsigned char*)$eb & $em) != 0) | (((*(unsigned char*)($eb+40) & $em) != 0) << 1) | (((*(unsigned char*)($eb+80) & $em) != 0) << 2) | (((*(unsigned char*)($eb+120) & $em) != 0) << 3)
    if $ap != 14
      set $badAlt = $badAlt + 1
    end
    if $ep != 15
      set $badEnergy = $badEnergy + 1
    end
    set $x = $x + 1
  end
  set $y = $y + 1
end
printf "  altitude bad pixels=%d/448  energy bad pixels=%d/448\n", $badAlt, $badEnergy
echo ==== end ====\n
