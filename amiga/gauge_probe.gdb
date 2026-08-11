# Why does the energy-gauge sprite draw over the cockpit dashboard?  Three candidate mechanisms,
# measured together at the POST-MOTHER-SHIP Standby install (the one that inherits 0024):
#   (a) BPLCON2 priority wrong for the scene       -> printed pre/post at every Standby install
#   (b) the dashboard PEN under the gauge column is 0 (COLOR00 background, which EVERY sprite
#       beats regardless of BPLCON2)               -> cockpit-bitmap dump + the Atari cells
#   (c) the sprite is taller / lower than its dial -> control words + fuel
# Build: make clean && make PROBES=1 FORCE_RETURN=1   (FORCE_RETURN now halves energy at the boost,
#        without which the bar exactly fills its dial and NOTHING hangs below it)
# Run:   GDBSCRIPT=gauge_probe.gdb ./diag_run.sh 120
set pagination off

# Cockpit bitmap: 3bp interleaved, 320 wide -> row stride 120, plane p at +40*p.
# Byte 25 = pixels x200..207, byte 26 = x208..215; the gauge slot is x204..211.
# Standby draws bitmap row r at scanline 172+r (flight at 164+r: its showBitmap skips 8 rows).
# The mode-4 dashboard starts at bitmap row 8 = Atari screen RAM $332D, 48 bytes/DL row,
# 8 scanlines/row, 4 bytes of left overscan cropped -> byte 25 of row r comes from
# $332D + ((r-8)/8)*48 + 29.
define gaugedump
  printf "  fuel $062F=%02x  VV=%04x  cop=%u  BPLCON2=%04x BPLCON0=%04x\n", \
    mem[0x062F], (mem[0x0222] | (mem[0x0223] << 8)), g_liveCopper, \
    *(unsigned short*)0xDFF104, *(unsigned short*)0xDFF100
  printf "  energy spr @%08x ctl=%04x %04x -> VSTART=%u VSTOP=%u(+%u*256) X=%u\n", \
    g_energySprAddr, *(unsigned short*)g_energySprAddr, *(unsigned short*)(g_energySprAddr+2), \
    (*(unsigned short*)g_energySprAddr >> 8), (*(unsigned short*)(g_energySprAddr+2) >> 8), \
    ((*(unsigned short*)(g_energySprAddr+2) >> 1) & 1), \
    ((*(unsigned short*)g_energySprAddr & 0xFF) * 2 + (*(unsigned short*)(g_energySprAddr+2) & 1))
  printf "  Atari dash cells under the gauge (entry.col29/30):"
  set $e = 0
  while $e < 10
    printf " %d:%02x%02x", $e, mem[0x332D + $e*48 + 29], mem[0x332D + $e*48 + 30]
    set $e = $e + 1
  end
  printf "\n   row sbLine | p0 p1 p2 (x200-207) | p0 p1 p2 (x208-215)\n"
  set $r = 60
  while $r < 88
    set $b = g_cockpitBmpAddr + $r*120
    printf "   %3d  %3d   | %02x %02x %02x            | %02x %02x %02x\n", \
      $r, 172+$r, \
      *(unsigned char*)($b+25), *(unsigned char*)($b+40+25), *(unsigned char*)($b+80+25), \
      *(unsigned char*)($b+26), *(unsigned char*)($b+40+26), *(unsigned char*)($b+80+26)
    set $r = $r + 1
  end
end

set $b2pre = 0
break rof_b2_mark_pre
commands
  silent
  set $b2pre = *(unsigned short*)0xDFF104
  printf "standby install vbi=%-6u INHERITED BPLCON2=%04x  (live BPLCON0=%04x)\n", \
    g_vbiCount, $b2pre, *(unsigned short*)0xDFF100
  continue
end
break rof_b2_mark_post
commands
  silent
  printf "                          after write  =%04x\n", *(unsigned short*)0xDFF104
  if $b2pre == 0x24
    echo ---- POST-MOTHER-SHIP standby install (inherited the tunnel's 0024) ----\n
    gaugedump
  end
  continue
end
continue
echo \n==== SIGINT ====\n
printf "NOW: vbi=%u installs=%lu\n", g_vbiCount, g_b2MarkPreN
printf "BPLCON2 MOVE audit per built list (ffff = none, CPU-owned): standby=%04x doors=%04x tunnel=%04x planet=%04x flight=%04x title=%04x\n", \
  g_cl2Standby, g_cl2Doors, g_cl2Tunnel, g_cl2Planet, g_cl2Flight, g_cl2Title
gaugedump
