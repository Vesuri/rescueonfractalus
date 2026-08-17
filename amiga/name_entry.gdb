# HIGH-SCORE INITIALS entry readout (the path that is only reachable by dying).
# Build: make clean && make -j4 PROBES=1 NAME_ENTRY=1
# Run:   GDBSCRIPT=name_entry.gdb ./diag_run.sh 90
#
#   00E5  game-over latch: 5 while the game-over tune plays, 0 = standby_scoreboard_render's
#         L_596d spin has been released and name_entry_loop has been called
#   0049  the console/stick sample render_text_cell waits on (bit7 set = nothing pressed)
#   0003  set to $64 by the VBI's check_collision_sync on any input — name_entry_loop's wake
#   0091  $C0 = the attract banner state that also releases name_entry_loop
#   37F0  the initials cursor;  36B7..36BD = the score/initials line in Title RAM
# Three of the loops in this path used to compile to `bra.s .` on the Amiga (mem[] is not
# volatile there): if the two PC samples below are identical, one of them is back.
continue
echo \n==== SIGINT ====\n
printf "vbi=%u  VV=%04x  cop=%u  0004=%02x 060B=%02x 00E4=%02x\n", \
  g_vbiCount, (mem[0x0222] | (mem[0x0223] << 8)), g_liveCopper, \
  mem[0x0004], mem[0x060B], mem[0x00E4]
printf "gameover: 00E5=%02x 063D=%02x  input: 0049=%02x 0003=%02x 0091=%02x\n", \
  mem[0x00E5], mem[0x063D], mem[0x0049], mem[0x0003], mem[0x0091]
printf "score=%02x%02x%02x%02x  high=%02x%02x%02x%02x  cursor 37F0=%02x\n", \
  mem[0x0600], mem[0x0601], mem[0x0602], mem[0x0603], \
  mem[0x0605], mem[0x0606], mem[0x0607], mem[0x0608], mem[0x37F0]
# The save-state block validate_save_state ($5D0D) gates the entry on: $3700==$28, $3714==$EE,
# and 38 bytes at $37C7 equal to the copyright string at $7BDA.  NAME_ENTRY seeds all three.
printf "savestate: 3700=%02x (want 28)  3714=%02x (want ee)  37C7=%02x%02x%02x (want %02x%02x%02x)\n", \
  mem[0x3700], mem[0x3714], mem[0x37C7], mem[0x37C8], mem[0x37C9], \
  mem[0x7BDA], mem[0x7BDB], mem[0x7BDC]
printf "37EE=%02x 37F3=%02x 37F4=%02x  attract 00E2=%02x\n", \
  mem[0x37EE], mem[0x37F3], mem[0x37F4], mem[0x00E2]
printf "initials line 36B7:"
set $i = 0
while $i < 7
  printf " %02x", mem[0x36B7 + $i]
  set $i = $i + 1
end
printf "\nprobes: enter=%u pass=%u wait=%u glyph=%u\n", g_neEnter, g_nePass, g_neWait, g_neGlyph
printf "\n--- main-loop PC sample 1 ---\n"
info symbol $pc
x/2i $pc
stepi 400
printf "--- main-loop PC sample 2 (after 400 instructions) ---\n"
info symbol $pc
x/2i $pc
printf "0049 now=%02x  0003 now=%02x  vbi now=%u\n", mem[0x0049], mem[0x0003], g_vbiCount
