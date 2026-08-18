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
# ⚠ Without this, long printf rows WRAP and the earlier ones are lost from the capture.
set width 0
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
# and 38 bytes at $37C7 equal to the copyright string at $7BDA.  NOTHING FAKES THESE any more —
# they arrive through the restored $5D86 SIO call (rof_sio_block, src/rof_hiscore.c), so a
# mismatch here means the loader is broken, not that the feature is absent.
# NB the compare runs $37C7+Y for Y=$26..1, so $37C8 is the first byte that must match $7BDB.
printf "savestate: 3700=%02x (want 28)  3714=%02x (want ee)  37C8=%02x%02x%02x (want %02x%02x%02x)\n", \
  mem[0x3700], mem[0x3714], mem[0x37C8], mem[0x37C9], mem[0x37CA], \
  mem[0x7BDB], mem[0x7BDC], mem[0x7BDD]
# SIO + persistence outcome: DSTATS $0303 (1 = the last transfer succeeded), the DCB's last
# sector $030A/$030B, and whether the block is still waiting to be written to RoF.hi.
printf "sio: DSTATS=%02x DCOMND=%02x sector=%02x%02x  reads=%u writes=%u err=%u\n", \
  mem[0x0303], mem[0x0302], mem[0x030B], mem[0x030A], \
  g_hsSioRead, g_hsSioWrite, g_hsSioErr
printf "persist: fromFile=%u dirty=%u written=%u   (fromFile=0 = the FACTORY table, which is fine on a first run)\n", \
  g_hsFromFile, g_hsDirty, g_hsWritten
# The table itself, decoded: row 0's name cells are (byte & $3F) + $20 in ATASCII.
printf "table row0:"
set $i = 0
while $i < 20
  printf " %02x", mem[0x3728 + $i]
  set $i = $i + 1
end
printf "\nheading:"
set $i = 0
while $i < 20
  printf " %02x", mem[0x3700 + $i]
  set $i = $i + 1
end
printf "\n"
# Which text screen the renderer picked: the entry screen is DL $5E2E, the Title card $5A82.
printf "dlist=%04x (5e2e = the entry screen, 5a82 = the Title card)  hs dirty=[%d..%d] (lo>hi = idle)\n", \
  g_atariDlist, g_hsCellLo, g_hsCellHi
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
