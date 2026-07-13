# At game-over, dump the decoded titleScreenBitmap at the LAST SCORE digit (row4 col19) and the
# LEVEL digit (row2 col18) to see whether decodeTitleCells actually rendered them.
continue
set $sc = (RescueOnFractalus*)&'PlatformAmiga::run()::scene'
set $b = (unsigned char*)$sc->titleScreenBitmap->data
printf "titleScreenBitmap->data = %p\n", $b
printf "cellLo=%d cellHi=%d  titleInstalled=%d\n", g_titleCellLo, g_titleCellHi, $sc->titleScreenCopperInstalled
printf "g_renderFrameCount=%lu  g_vbiCount(approx via subclock)\n", g_renderFrameCount
printf "--- LAST SCORE digit (row4 y=170, col19 bx=38) plane1 8 rows ---\n"
set $i = 0
while $i < 8
  printf "r%d: %02x %02x\n", $i, $b[(170+$i)*120 + 38], $b[(170+$i)*120 + 39]
  set $i = $i + 1
end
printf "--- LEVEL digit (row2 y=136, col18 bx=36) plane1 8 rows ---\n"
set $i = 0
while $i < 8
  printf "r%d: %02x %02x\n", $i, $b[(136+$i)*120 + 36], $b[(136+$i)*120 + 37]
  set $i = $i + 1
end
printf "--- title 'R' (row0 y=56, col0 bx=0) plane1 rows (sanity: should be non-zero) ---\n"
set $i = 0
while $i < 8
  printf "r%d: %02x %02x\n", $i, $b[(56+$i)*120 + 0], $b[(56+$i)*120 + 1]
  set $i = $i + 1
end
