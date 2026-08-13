# logo_sparkle.gdb — prove scene 1's sparkle actually runs, and that the Amiga sprite mirror
# tracks it.  Breakpoint-driven rather than wall-clock: the sparkle is a ~66-frame window ~6 s
# into a boot whose startup jitter is worth more than that.
#   GDBSCRIPT=logo_sparkle.gdb ./diag_run.sh 30
set width 0
break rof_logo.c:260
continue

set $sc = 'PlatformAmiga.cpp'::s_scene
set $spr = (unsigned short*)$sc->logoSparkle->data_
set $n = 0
while $n < 16
  printf "step %2d: vbi=%u rtclok=%02x $4C=%02x COLPM0=%02x HPOSP0=%02x | P0 $0C40: ", \
    $n, g_vbiCount, mem[0x0014], mem[0x004C], g_logoSparkleCol, mem[0xD000]
  set $i = 0
  while $i < 16
    printf "%02x", mem[0x0C40 + $i]
    set $i = $i + 1
  end
  printf " | sprite ctl=%04x %04x px=%04x %04x %04x\n", $spr[0], $spr[1], $spr[2], $spr[4], $spr[6]
  set $n = $n + 1
  continue
end

delete
printf "\nafter the loop: bootScene=%u vbi=%u HPOSP0=%02x COLPM0=%02x ctl=%04x %04x\n", \
  g_bootScene, g_vbiCount, mem[0xD000], g_logoSparkleCol, $spr[0], $spr[1]
detach
quit
