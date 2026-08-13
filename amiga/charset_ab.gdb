# charset_ab.gdb — FRAME-LOCKED A/B: does dropping the non-charset OS ROM change anything?
#
# The charset-only build leaves $C000-$CFFF / $D800-$FFFF as ZERO where the full-OS-ROM
# build had real ROM bytes there.  So if any code path read ROM outside the $E000 charset,
# the two builds' downstream RAM would diverge.  This probe checksums the whole RAM image
# at a FIXED emulated frame, so the two runs are comparable byte-for-byte:
#
#   full ROM :  git stash the charset commit / restore assets/atari_osrom.bin, rebuild, run
#   charset  :  the shipping build
#   identical checksums => nothing outside $E000-$E3FF was ever read.
#
# A wall-clock SIGINT cannot do this (charset_probe.gdb sampled vbi 2263 vs 2212 and the
# standby animation had moved on) — hence the conditional breakpoint on g_vbiCount.
#
# Works on a PLAIN build.  Run:
#   . ./env.sh && GDBSCRIPT=charset_ab.gdb ./diag_run.sh 60
set width 0
set pagination off
# Frame-lock: stop at a fixed emulated frame, well after the Standby has settled.
break PlatformAmiga::renderFrame if g_vbiCount >= 2000
continue
echo \n==== FRAME-LOCKED ====\n
printf "vbiCount=%u bootScene=%u VVBLKI=%02x%02x state(41)=%02x\n", \
  g_vbiCount, g_bootScene, mem[0x0223], mem[0x0222], mem[0x0041]
echo \n-- RAM checksums, 4 KB blocks $0000-$BFFF (position-weighted) --\n
set $blk = 0
while $blk < 0xC000
  set $acc = 0
  set $k = 0
  while $k < 0x1000
    set $acc = ($acc + mem[$blk + $k] * (($k & 0x1F) + 1)) & 0xFFFFFF
    set $k = $k + 1
  end
  printf "  $%04x-$%04x  sum=%06x\n", $blk, $blk + 0xFFF, $acc
  set $blk = $blk + 0x1000
end
echo \n-- charset page $E000-$E3FF --\n
set $acc = 0
set $k = 0
while $k < 0x400
  set $acc = ($acc + mem[0xE000 + $k] * (($k & 0x1F) + 1)) & 0xFFFFFF
  set $k = $k + 1
end
printf "  sum=%06x   'L' ($E160): %02x %02x %02x %02x %02x %02x %02x %02x\n", $acc, \
  mem[0xE160], mem[0xE161], mem[0xE162], mem[0xE163], \
  mem[0xE164], mem[0xE165], mem[0xE166], mem[0xE167]
echo \n-- non-charset ROM spots (expected: real bytes on the full-ROM build, 00 on charset) --\n
printf "  C800=%02x D800=%02x DC00=%02x E400=%02x E45C=%02x F000=%02x FFFF=%02x\n", \
  mem[0xC800], mem[0xD800], mem[0xDC00], mem[0xE400], mem[0xE45C], mem[0xF000], mem[0xFFFF]
echo ==== end ====\n
