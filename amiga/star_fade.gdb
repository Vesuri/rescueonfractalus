# star_fade.gdb — how long does the Station's star fade-in take on the Amiga?
# Needs a build with the boot scenes IN (plain `make`, i.e. NOT PROBES=1/FPSCOUNT=1, which imply
# SKIPBOOT).  Run: . ./env.sh && GDBSCRIPT=star_fade.gdb ./diag_run.sh 40
#
# The Atari does 14 brightening passes one frame apart (station_star_fade_in $1E79, JSR $3CC3
# between passes) = 15 vblanks = 300 ms.  This prints the same span in real Amiga vblanks, plus
# the per-frame cadence of the re-decode that pays for it.
set width 0
break station_star_fade_in
break station_audio
break RescueOnFractalus::decodeStationStars
break RescueOnFractalus::decodeStationField
break RescueOnFractalus::buildStationSprites
continue

# ---- stop 1: the fade is about to start -------------------------------------------------
set $v0 = 'PlatformAmiga.cpp'::g_vbiCount
set $sc = 'PlatformAmiga.cpp'::s_scene
printf "STAR-FADE entry: vbi=%u  starRows=%u  008B=%02x\n", $v0, $sc->stationStarRows, mem[0x008B]
# Survey the star region display_list_build left behind: how many bytes are non-zero at all, and
# how many are exactly one nibble of value 1 ($10 or $01, the two seeds at $1C3E/$1C3F)?
set $nz = 0
set $seed = 0
set $i = 0
while $i < 1200
  if mem[0x2CB8 + $i] != 0
    set $nz = $nz + 1
    if mem[0x2CB8 + $i] == 0x10
      set $seed = $seed + 1
    end
    if mem[0x2CB8 + $i] == 0x01
      set $seed = $seed + 1
    end
  end
  set $i = $i + 1
end
printf "  $2CB8-$3167 (1200 bytes): non-zero=%u  of which $10/$01=%u\n", $nz, $seed
# offset of the first star byte, so the per-vblank line can watch one pixel brighten
set $star0 = 0
set $i = 0
while $i < 1200
  if mem[0x2CB8 + $i] != 0
    if $star0 == 0
      set $star0 = $i
    end
  end
  set $i = $i + 1
end
printf "  watching star byte at $%04x\n", 0x2CB8 + $star0
delete 1

# ---- each per-frame star re-decode, until station_audio says the fade is over -----------
set $n = 0
set $nf = 0
set $ns = 0
commands 3
silent
set $n = $n + 1
printf "  stars #%u at vbi=%u (+%u)  starRows=%u dirtyCount=%u dirtyFull=%u\n", $n, 'PlatformAmiga.cpp'::g_vbiCount, ('PlatformAmiga.cpp'::g_vbiCount - $v0), $sc->stationStarRows, g_stationDirtyCount, g_stationDirtyFull
continue
end
commands 4
silent
set $nf = $nf + 1
printf "  FULL FIELD decode #%u at vbi=%u (+%u)  dirtyFull=%u\n", $nf, 'PlatformAmiga.cpp'::g_vbiCount, ('PlatformAmiga.cpp'::g_vbiCount - $v0), g_stationDirtyFull
continue
end
commands 5
silent
set $ns = $ns + 1
printf "  vbl +%u: fadePtr=%02x%02x RTCLOK=%02x%02x%02x star0=%02x\n", ('PlatformAmiga.cpp'::g_vbiCount - $v0), mem[0x0091], mem[0x0090], mem[0x0012], mem[0x0013], mem[0x0014], mem[0x2CB8 + $star0]
continue
end
continue

# ---- stop 2: first station_audio call = the attract loop, i.e. the fade has returned ----
set $v1 = 'PlatformAmiga.cpp'::g_vbiCount
printf "STAR-FADE end:   vbi=%u  -> %u vblanks = %u ms   [Atari: 15 vblanks = 300 ms]   starsDecodes=%u fullDecodes=%u spriteBuilds=%u\n", $v1, ($v1 - $v0), ($v1 - $v0) * 20, $n, $nf, $ns
set $nz2 = 0
set $full = 0
set $i = 0
while $i < 1200
  if mem[0x2CB8 + $i] != 0
    set $nz2 = $nz2 + 1
    if mem[0x2CB8 + $i] == 0xF0
      set $full = $full + 1
    end
    if mem[0x2CB8 + $i] == 0x0F
      set $full = $full + 1
    end
  end
  set $i = $i + 1
end
printf "  after the fade: non-zero=%u  of which $F0/$0F (luminance 15)=%u\n", $nz2, $full

# ---- and the BITMAP, not just mem[]: does every star reach the field at full brightness? -----
# Pen 15 means all four planes carry that nibble, so each plane byte must read $F0 or $0F (the
# fade's two end values).  Checking mem[] alone would be vacuous — the narrowed re-decode's whole
# risk is writing the WRONG PLACE in the bitmap.
set $bm = (unsigned char *)$sc->bootFieldBitmap->data
set $ok = 0
set $bad = 0
set $i = 0
while $i < $sc->stationStarRows
  set $o = $sc->stationStarDst[$i]
  set $p0 = $bm[$o]
  if ($p0 == 0xF0 || $p0 == 0x0F) && $bm[$o+40] == $p0 && $bm[$o+80] == $p0 && $bm[$o+120] == $p0
    set $ok = $ok + 1
  else
    set $bad = $bad + 1
    printf "  BAD star %u: dst=%u src=$%04x len=%u planes %02x %02x %02x %02x\n", $i, $o, $sc->stationStarSrc[$i], $sc->stationStarLen[$i], $p0, $bm[$o+40], $bm[$o+80], $bm[$o+120]
  end
  set $i = $i + 1
end
printf "  BITMAP: %u/%u stars at pen 15 in all four planes (bad=%u)\n", $ok, $sc->stationStarRows, $bad

# ---- then the ATTRACT LOOP's own cadence: one station_audio call per iteration --------------
# The scroll rides this loop (station_anim_frame -> display_scroll, one row per step, 148 steps),
# so the vblanks-per-iteration here is the scroll's speed.  The Atari runs one iteration a frame.
delete 3
delete 4
delete 5
set $pv = $v1
set $ni = 0
commands 2
silent
set $ni = $ni + 1
printf "  attract iter %u at vbi=%u (+%u)  008B=%02x dlPtr=%02x%02x\n", $ni, 'PlatformAmiga.cpp'::g_vbiCount, ('PlatformAmiga.cpp'::g_vbiCount - $pv), mem[0x008B], mem[0x1C3A], mem[0x1C39]
set $pv = 'PlatformAmiga.cpp'::g_vbiCount
continue
end
continue

# ---- SIGINT lands here: summarise the attract loop -----------------------------------------
printf "ATTRACT: %u iterations over %u vblanks = %u.%u vbl/iter  (Atari: 1)  008B=%02x of $94\n", \
  $ni, ('PlatformAmiga.cpp'::g_vbiCount - $v1), ('PlatformAmiga.cpp'::g_vbiCount - $v1) / $ni, \
  ((('PlatformAmiga.cpp'::g_vbiCount - $v1) * 10) / $ni) % 10, mem[0x008B]
detach
quit
