# Verify the chip-BSS conversion of `.MEMF_CHIP` (amiga/memf_chip_bss.ld).
#   build: make PROBES=1     drive: GDBSCRIPT=chipbss_verify.gdb ./diag_run.sh 40
#
# The five __chip waveform buffers no longer ship their contents in RoF — the hunk is
# HUNK_BSS with the MEMF_CHIP flag, so AmigaDOS allocates it from chip RAM and LoadSeg
# clears it, and PlatformAmiga::audioInit() fills it.  Three things have to hold, and none
# of them can be seen in a host-side size diff:
#   * the hunk really landed in CHIP RAM     (address < $200000 on an A500)
#   * wave_pure is { $7F, $81 }              (it lost its static initialiser)
#   * the tables were actually generated     (a buffer of zeros = silence, and zeros is
#                                             exactly what a BSS hunk starts as)
set pagination off
set confirm off
set width 0
set remotetimeout 90
tbreak PlatformAmiga::renderFrame if g_vbiCount > 400
continue
set $wp = (unsigned char *) &'PlatformAmiga.cpp'::wave_pure
set $nb = (unsigned char *) &'PlatformAmiga.cpp'::noise_buf
set $p4 = (unsigned char *) &'PlatformAmiga.cpp'::poly4_wave
set $p5 = (unsigned char *) &'PlatformAmiga.cpp'::poly5_wave
printf "chip? wave_pure=%p noise_buf=%p poly4=%p poly5=%p (all must be < 0x200000)\n", \
  $wp, $nb, $p4, $p5
printf "wave_pure = %02x %02x   (must be 7f 81)\n", $wp[0], $wp[1]
# Any nonzero byte proves the generator ran; sum a slice of each table.
set $s = 0
set $i = 0
while $i < 256
  set $s = $s + $nb[$i]
  set $i = $i + 1
end
printf "noise_buf[0..255] sum = %u (must be nonzero)\n", $s
set $s = 0
set $i = 0
while $i < 30
  set $s = $s + $p4[$i]
  set $i = $i + 1
end
printf "poly4_wave[0][0..29] sum = %u (must be nonzero)\n", $s
set $s = 0
set $i = 0
while $i < 62
  set $s = $s + $p5[$i]
  set $i = $i + 1
end
printf "poly5_wave[0][0..61] sum = %u (must be nonzero)\n", $s
printf "vbi=%u\n", g_vbiCount
detach
quit
