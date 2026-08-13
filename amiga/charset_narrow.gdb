# charset_narrow.gdb — narrow the charset-vs-full-ROM divergence to exact bytes.
#
# charset_ab.gdb showed 10 of 12 RAM blocks identical but $0000-$0FFF and $3000-$3FFF
# differing (same binary twice = stable, so it is real, not run noise).  This dumps those
# two blocks at 128-byte granularity plus the raw ZP bytes, at the same fixed frame.
#
# Suspect from the static scan: LDA ($85),Y at $8111, whose pointer hi comes from
# $81A9[X] -- and $81A9[0] = $EC, i.e. $EC00+, inside the OS ROM.  Its results go to
# STA $8f,X for X=4..0, i.e. $008F-$0093, which is in the first differing block.
#
# Run:  . ./env.sh && GDBSCRIPT=charset_narrow.gdb ./diag_run.sh 60
set width 0
set pagination off
break PlatformAmiga::renderFrame if g_vbiCount >= 2000
continue
echo \n==== FRAME-LOCKED ====\n
printf "vbiCount=%u state(41)=%02x\n", g_vbiCount, mem[0x0041]
echo \n-- 128-byte sums, $0000-$0FFF --\n
set $b = 0
while $b < 0x1000
  set $acc = 0
  set $k = 0
  while $k < 128
    set $acc = ($acc + mem[$b + $k] * ($k + 1)) & 0xFFFF
    set $k = $k + 1
  end
  printf "  $%04x sum=%04x\n", $b, $acc
  set $b = $b + 128
end
echo \n-- 128-byte sums, $3000-$3FFF --\n
set $b = 0x3000
while $b < 0x4000
  set $acc = 0
  set $k = 0
  while $k < 128
    set $acc = ($acc + mem[$b + $k] * ($k + 1)) & 0xFFFF
    set $k = $k + 1
  end
  printf "  $%04x sum=%04x\n", $b, $acc
  set $b = $b + 128
end
echo \n-- raw ZP $0080-$00BF (the ($85),Y pointer + its STA $8f,X results) --\n
set $b = 0x80
while $b < 0xC0
  printf "  $%04x: %02x %02x %02x %02x %02x %02x %02x %02x\n", $b, \
    mem[$b+0], mem[$b+1], mem[$b+2], mem[$b+3], mem[$b+4], mem[$b+5], mem[$b+6], mem[$b+7]
  set $b = $b + 8
end
echo \n-- the pointer-hi source table $81A9[0..7] and its index $292A --\n
printf "  $292A=%02x  $81A1[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n", mem[0x292A], \
  mem[0x81A1], mem[0x81A2], mem[0x81A3], mem[0x81A4], \
  mem[0x81A5], mem[0x81A6], mem[0x81A7], mem[0x81A8]
printf "              $81A9[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n", \
  mem[0x81A9], mem[0x81AA], mem[0x81AB], mem[0x81AC], \
  mem[0x81AD], mem[0x81AE], mem[0x81AF], mem[0x81B0]
echo ==== end ====\n
