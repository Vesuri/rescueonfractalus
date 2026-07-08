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
echo ==== end ====\n
