# Planet-entry windscreen-band seed: the band (Amiga 172-179) comes from viewportBitmap rows
# 43-46 under the planet copper and from cockpitBitmap rows 0-7 under the tunnel copper, so it
# has to be decoded BEFORE the planet list goes live (renderFrame's vpEntryClear seed).
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1 FORCE_RELAUNCH=1
#   . ./env.sh && GDBSCRIPT=band_seed.gdb ./diag_run.sh 200
# src != 0 with bmp == 0 means the seed ran and wrote NOTHING -> a black band for one frame.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "planet entries seen: %u\n", g_pbN
printf "   #   vbi   src   bmp   mid  shadow   lastBase  entryClear kicked forceFull\n"
set $i = 0
while $i < 4 && $i < g_pbN
  printf "  %d  %5u  %04lx  %04lx   %02x  %08lx   %04x        %u       %u      %u\n", $i, g_pbVbi[$i], \
    g_pbSrc[$i], g_pbBmp[$i], g_pbMid[$i], g_pbShd[$i], g_pbLastBase[$i], \
    g_pbFlags[$i] & 1, (g_pbFlags[$i] >> 1) & 1, (g_pbFlags[$i] >> 2) & 1
  set $i = $i + 1
end
printf "vbi=%u liveCopper=%u\n", g_vbiCount, g_liveCopper
printf "mid MUST be non-zero at EVERY entry: it is the band's wide light-grey area, and a zero\n"
printf "there is the black bar.  A stale viewportShadow made the seed skip it silently (the seed\n"
printf "is a memoizing decode, so it has to invalidate the shadow for its own rows first).\n"
