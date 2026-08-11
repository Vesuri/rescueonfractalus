continue
echo \n==== SIGINT (wide-object / SIZEPn probe) ====\n
printf "vbiNOW=%u iterCount=%u  flightCopper=0x%08x\n", g_vbiCount, g_iterCount, g_flightCopperAddr

# ---- Did the widening path actually run? -----------------------------------------------------
# A headless COMBAT+AUTO_FIRE run must show non-zero 2x/4x buckets.  If they are all in the 1x
# bucket the burst never peaked in this window and a visual check would prove nothing.
printf "shot scale 1x/2x/4x = %u / %u / %u\n", g_wideShotScale[0], g_wideShotScale[1], g_wideShotScale[2]
printf "P3   scale 1x/2x/4x = %u / %u / %u\n", g_wideP3Scale[0],   g_wideP3Scale[1],   g_wideP3Scale[2]
printf "widest wide-object run = %u rows   contested (denied) = %u\n", g_wideMaxRows, g_wideDenied
# The 2x->1x step with the wide segment 0 still displayed (it is double buffered).  Each of these
# was a frame the OLD immediate both-chain blank showed with its right half missing; the deferred
# release now blanks the off-screen chain instead.  Must be > 0 for this run to have tested it.
printf "burst releases with the DISPLAYED chain still lit (deferred) = %u\n", g_wideLateBlank
printf "mem: 0036(shot)=%02x  00CD(SIZEP2)=%02x  00CB(HPOSP2)=%02x  0037(COLPM2)=%02x\n", \
  mem[0x0036], mem[0x00CD], mem[0x00CB], mem[0x0037]
printf "     006A(p3mode)=%02x 0063(objidx)=%02x  2870(P3 X)=%02x  SIZEP3 latch=%02x\n", \
  mem[0x006A], mem[0x0063], mem[0x2870], g_sizep3_shadow

# ---- The chains: [extension][lower element][0,0] in ONE buffer --------------------------------
# header[0]=VSTART, header[2]=VSTOP, header[3]=flags.  The chained element's control words sit at
# +4 + 4*extRows.  The whole point of the check: its VSTART must be STRICTLY GREATER than the
# extension's VSTOP, or the channel's post-VSTOP re-fetch races that line and the energy bar /
# altimeter / left band triangle never arms.
echo --- wide-object sprite chains (ch5 energy, ch6 altimeter, ch1 leftTri) ---\n
set $s = 0
while $s < 3
  set $a = g_wideExtAddr[$s]
  # extension heights: ch5/ch6 = 94 rows, ch1 = 85 rows
  set $h = 94
  if $s == 2
    set $h = 85
  end
  set $b = $a + 4 + $h * 4
  printf "  seg%d @0x%08x  ext VSTART=%3u VSTOP=%3u flags=%02x   chained VSTART=%3u VSTOP=%3u flags=%02x", \
    $s + 1, $a, *(unsigned char*)$a, *(unsigned char*)($a+2), *(unsigned char*)($a+3), \
    *(unsigned char*)$b, *(unsigned char*)($b+2), *(unsigned char*)($b+3)
  if *(unsigned char*)$b > *(unsigned char*)($a+2)
    printf "  OK\n"
  else
    printf "  *** VSTART NOT PAST VSTOP ***\n"
  end
  set $s = $s + 1
end

# ---- Copper SPRxPT entries: ch1/ch5/ch6 must enter at the CHAIN HEADS -------------------------
echo --- flight copper sprite-pointer moves (reg 0x120..0x13E) ---\n
# Stop at the list's own terminator (copperWait(255,254) = 0xfffffffe).  A fixed 600-word scan
# overruns the ~360-word flight list and reports sprite moves out of the NEXT allocation.
set $c = g_flightCopperAddr
set $i = 0
set $go = 1
while $i < 700 && $go
  set $w = *(unsigned int*)($c + $i*4)
  if $w == 0xfffffffe
    set $go = 0
  else
    set $hi = ($w >> 16) & 0x1ff
    if $hi >= 0x120 && $hi <= 0x13e
      printf "  [%3d] reg=%03x val=%04x  (ch%d %s)\n", $i, $hi, ($w & 0xffff), ($hi-0x120)/4, ($hi & 2) ? "PTL" : "PTH"
    end
  end
  set $i = $i + 1
end
printf "  (list length = %d words)\n", $i
echo ==== end ====\n
