# planet_edge.gdb -- what the enhanced planet painter leaves in the viewport planes.
#
# Prints, per paint, every pen transition down one bar's byte column: "<row>:<plane0><plane1>",
# so a column reads as highlight (ff00 = pen 1), rim (00ff = pen 2), body (ffff = pen 3).  Diff
# consecutive paints to see how far each edge moved -- the marker's vertical resolution.  Offset
# 8 picks displayed byte column 8 (bar slot 10); change it for another bar.
#
# The window is paints 61-70: early enough that the planet is on screen, late enough that the
# painter owns the rows rather than the ordinary decode.
#
# ⚠ Take the bitmap pointer AFTER the first `continue`, as below.  gdb captures a convenience
# variable's VALUE, and before the first break s_scene is still null -- an earlier version read
# address 0 all run and reported a static image.
#
# Build: make clean && make -j4 PROBES=1 STANDALONE_DATA=1 ROM=../rof.rom ENHANCED_TERRAIN=1
# Run:   . ./env.sh && GDBSCRIPT=planet_edge.gdb ./diag_run.sh 300
set width 0
break RescueOnFractalus::paintPlanetNative
continue
set $bm = (unsigned char *)this->viewportBitmap->data
set $n = 0
printf "PLANET-EDGE: enhanced=%d bm=%p\n", g_flightEnhancedTerrain, $bm
commands 1
silent
set $n = $n + 1
if $n > 60 && $n < 70
  printf "paint %u dist20=%02x%02x adv=%02x |", $n, mem[0x08B9], mem[0x08B8], mem[0x08D3]
  set $y = 0
  set $p0 = 0
  set $p1 = 0
  while $y < 86
    set $o = $y * 120 + 8
    if $bm[$o] != $p0 || $bm[$o+40] != $p1
      set $p0 = $bm[$o]
      set $p1 = $bm[$o+40]
      printf " %u:%02x%02x", $y, $p0, $p1
    end
    set $y = $y + 1
  end
  printf "\n"
end
continue
end
continue
detach
quit
