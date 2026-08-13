# charset_probe.gdb — prove the $E000 charset overlay renders the same glyphs.
#
# The only ROM DATA the port reads is the Atari internal charset: glyph_ptr_from_index
# ($6773) builds $0084/$0085 = $E000 + (code<<3) and blit_glyph_8rows blits 8 rows
# through it; blit_label_row ($6E23 codes = "LEVEL"/"DROID") is the caller.  This probe
# prints the charset bytes actually consulted AND the blitted result, so a full-OS-ROM
# build and a charset-only build can be diffed byte-for-byte.
#
# Works on a PLAIN build.  Run:
#   . ./env.sh && GDBSCRIPT=charset_probe.gdb ./diag_run.sh 50
#
# Dest of the label row: blit_glyph_8rows walks the row-address tables $073D/$0793
# indexed by the row ($0092=$2E for blit_label_row), + column $009C=$1B.
continue
echo \n==== SIGINT ====\n
printf "bootScene=%u VVBLKI=%02x%02x state(41)=%02x vbiCount=%u\n", \
  g_bootScene, mem[0x0223], mem[0x0222], mem[0x0041], g_vbiCount
echo \n-- label glyph codes mem[$6E23..$6E2C] (LEVEL / DROID) --\n
printf "  %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x\n", \
  mem[0x6E23], mem[0x6E24], mem[0x6E25], mem[0x6E26], mem[0x6E27], \
  mem[0x6E28], mem[0x6E29], mem[0x6E2A], mem[0x6E2B], mem[0x6E2C]
echo \n-- charset rows for the 7 distinct glyphs (E120 D, E128 E, E148 I, E160 L, E178 O, E190 R, E1B0 V) --\n
set $g = 0
while $g < 7
  set $base = 0xE120
  if $g == 1
    set $base = 0xE128
  end
  if $g == 2
    set $base = 0xE148
  end
  if $g == 3
    set $base = 0xE160
  end
  if $g == 4
    set $base = 0xE178
  end
  if $g == 5
    set $base = 0xE190
  end
  if $g == 6
    set $base = 0xE1B0
  end
  printf "  $%04x: %02x %02x %02x %02x %02x %02x %02x %02x\n", $base, \
    mem[$base+0], mem[$base+1], mem[$base+2], mem[$base+3], \
    mem[$base+4], mem[$base+5], mem[$base+6], mem[$base+7]
  set $g = $g + 1
end
echo \n-- the whole charset page, 16-byte checksum-ish stripes ($E000-$E3FF) --\n
set $s = 0
while $s < 1024
  set $acc = 0
  set $k = 0
  while $k < 128
    set $acc = ($acc + mem[0xE000 + $s + $k] * ($k + 1)) & 0xFFFF
    set $k = $k + 1
  end
  printf "  $%04x+%04x sum=%04x\n", 0xE000, $s, $acc
  set $s = $s + 128
end
echo \n-- blitted label row: dest = ($0793[$2E]<<8 | $073D[$2E]) + $1B --\n
set $row = 0x2E
set $dst = (mem[0x0793 + $row] << 8) | mem[0x073D + $row]
printf "  rowbase=$%04x  col=$%02x  dest=$%04x\n", $dst, mem[0x009C], $dst + 0x1B
set $d = $dst + 0x1B
printf "  bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", \
  mem[$d+0], mem[$d+1], mem[$d+2], mem[$d+3], mem[$d+4], mem[$d+5], \
  mem[$d+6], mem[$d+7], mem[$d+8], mem[$d+9], mem[$d+10], mem[$d+11]
echo \n-- ROM ranges OUTSIDE the charset: must be untouched-and-unread (spot bytes) --\n
printf "  C000=%02x C800=%02x CFFF=%02x D800=%02x DC00=%02x E400=%02x E45C=%02x E462=%02x F000=%02x FFFF=%02x\n", \
  mem[0xC000], mem[0xC800], mem[0xCFFF], mem[0xD800], mem[0xDC00], \
  mem[0xE400], mem[0xE45C], mem[0xE462], mem[0xF000], mem[0xFFFF]
echo ==== end ====\n
