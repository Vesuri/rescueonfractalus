# Reach the game-over Title screen (FORCE_DEATH), dump screen RAM $365B + title-decode state.
continue
printf "=== GAME OVER STATE ===\n"
printf "VVBLKI=%02x%02x  $365B[0]=%02x  g_titleToRender=%d  cellLo=%d cellHi=%d\n", *(unsigned char*)(mem+0x0223), *(unsigned char*)(mem+0x0222), *(unsigned char*)(mem+0x365B), g_titleToRender, g_titleCellLo, g_titleCellHi
printf "--- screen RAM $365B (6 rows x 20) ---\n"
set $r = 0
while $r < 6
  set $c = 0
  printf "row%d: ", $r
  while $c < 20
    printf "%02x ", *(unsigned char*)(mem + 0x365B + $r*20 + $c)
    set $c = $c + 1
  end
  printf "\n"
  set $r = $r + 1
end
printf "--- score/level source vars ---\n"
printf "$0066a=%02x $0686=%02x  $062b(level?)=%02x\n", *(unsigned char*)(mem+0x066a), *(unsigned char*)(mem+0x0686), *(unsigned char*)(mem+0x062b)
