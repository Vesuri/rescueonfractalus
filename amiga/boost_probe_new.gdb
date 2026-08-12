# Boost return-to-mother-ship reverse-tunnel decode cost.
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=boost_probe.gdb ./diag_run.sh 120
#
# The reverse tunnel no longer decodes anything per frame: the rings are painted straight into
# tunnelBitmap by drawTunnelRect as the 6502 draws them, the starfield sits in viewportBitmap, and
# the reveal is a copper band split (TunnelCopperList::setRevealBands).  So the checks are:
#   tunnel decodes  -- must be 0 (the per-frame decode this replaced; 1 tick = 1 scanline = 63.56us)
#   ring rects      -- >0, and door rects skipped, and EARLY must be 0 (a rect painted before the
#                      boost-entry clear would have been wiped = lost rings)
#   reveal sweep    -- K must walk 43 -> 0; stuck at 43 = the band never opens (all starfield),
#                      stuck at 0 = it snapped open instead of growing.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "=== boost decode (stars only -- the reverse tunnel decodes NOTHING now) ===\n"
printf "stars decodes      : %lu  (%lu ticks)\n", g_bStarDec, g_bStarTicks
if g_bStarDec > 0
  printf "  stars ticks/dec  : %lu  (= %lu us)\n", g_bStarTicks / g_bStarDec, (g_bStarTicks / g_bStarDec) * 64
end
printf "=== band-corner split (greenLine: 0 = door colour whole band, 8 = carried ring colour) ===\n"
set $i = 0
while $i < 9
  printf "  greenLine %d : tunnel %lu  stars %lu\n", $i, g_bwLine[$i], g_bwLineStars[$i]
  set $i = $i + 1
end
printf "wedge $0C88-8F now: %02x %02x %02x %02x %02x %02x %02x %02x\n", \
  mem[0xC88], mem[0xC89], mem[0xC8A], mem[0xC8B], mem[0xC8C], mem[0xC8D], mem[0xC8E], mem[0xC8F]
printf "=== phase routing ===\n"
printf "vbi=%u  boostRet=%u boostVp=%u liveCopper=%u handoffHold=%lu\n", g_vbiCount, g_boostRet, g_boostVp, g_liveCopper, g_boostHandoffHoldFrames
printf "mem[$3A]=%02x mem[$8D]=%02x mem[$8E]=%02x VVBLKI=%02x%02x\n", mem[0x3A], mem[0x8D], mem[0x8E], mem[0x223], mem[0x222]
printf "=== direct ring painter (ROF_TUNNEL_RECT hook) ===\n"
printf "boost ring rects painted=%lu  door rects skipped=%lu\n", g_trCalls, g_trDoors
printf "forward-tunnel rects (expected, keeps its decode)=%lu   EARLY-in-boost (MUST be 0)=%lu\n", g_trFwd, g_trEarly
set $i = 0
while $i < 8
  printf "  [%d] rowTop=%3lu rowBot=%3lu  xL=%3lu xR=%3lu  byte %2lu..%2lu  colour=%lu\n", $i, \
    (g_trRows[$i] >> 8) & 0xff, g_trRows[$i] & 0xff, \
    (g_trCols[$i] >> 24) & 0xff, (g_trCols[$i] >> 16) & 0xff, \
    (g_trCols[$i] >> 8) & 0xff, g_trCols[$i] & 0xff, g_trPen[$i]
  set $i = $i + 1
end
printf "=== copper reveal sweep (K = first viewport row showing rings; 43 = closed) ===\n"
printf "tunnel frames=%lu\n", g_revealFrames
set $i = 0
while $i < 44
  if g_revealK[$i] > 0
    printf "  K=%2d : %lu frames\n", $i, g_revealK[$i]
  end
  set $i = $i + 1
end
printf "=== direct-painter fidelity (tunnelBitmap vs a decode of the live $1000 field) ===\n"
printf "bytes compared=%lu  mismatching=%lu\n", g_dpBytes, g_dpBad
if g_dpBad > 0
  printf "  first: row=%lu byte=%lu  got p1/p2/p3=%02lx %02lx %02lx  want=%02lx %02lx %02lx\n", \
    (g_dpFirst >> 8) & 0xff, g_dpFirst & 0xff, \
    (g_dpGot >> 16) & 0xff, (g_dpGot >> 8) & 0xff, g_dpGot & 0xff, \
    (g_dpWant >> 16) & 0xff, (g_dpWant >> 8) & 0xff, g_dpWant & 0xff
end
printf "=== boost entry branch ===\n"
printf "entry runs=%lu  bgPen=%lu  row42 after fill p1/p2/p3=%02lx %02lx %02lx\n", \
  g_beEntry, g_bePen, (g_beAfter >> 16) & 0xff, (g_beAfter >> 8) & 0xff, g_beAfter & 0xff
printf "  EXTRA (bitmap has ink, field is background)=%lu   MISSING (field write not seen)=%lu\n", g_dpExtra, g_dpMissing
printf "  mismatch by COLUMN (byte 0..39):\n    "
set $i = 0
while $i < 40
  printf "%lu ", g_dpCol[$i]
  set $i = $i + 1
end
printf "\n  mismatch by ROW (0..85):\n    "
set $i = 0
while $i < 86
  printf "%lu ", g_dpRow[$i]
  set $i = $i + 1
end
printf "\n"
printf "=== live $1000 field, row 42 displayed bytes 4..43 ===\n    "
set $i = 4
while $i < 44
  printf "%02x ", mem[0x1000 + 42*46 + $i]
  set $i = $i + 1
end
printf "\n=== live $1000 field, row 20 displayed bytes 4..43 ===\n    "
set $i = 4
while $i < 44
  printf "%02x ", mem[0x1000 + 20*46 + $i]
  set $i = $i + 1
end
printf "\n"
printf "=== row 42 snapshot (done=%lu): field / bitmap p1 p2 p3 ===\n", g_dpSnapDone
printf "  field: "
set $i = 0
while $i < 40
  printf "%02x ", g_dpSrc42[$i]
  set $i = $i + 1
end
printf "\n  bmpP1: "
set $i = 0
while $i < 40
  printf "%02x ", g_dpB1[$i]
  set $i = $i + 1
end
printf "\n  bmpP2: "
set $i = 0
while $i < 40
  printf "%02x ", g_dpB2[$i]
  set $i = $i + 1
end
printf "\n  bmpP3: "
set $i = 0
while $i < 40
  printf "%02x ", g_dpB3[$i]
  set $i = $i + 1
end
printf "\n"
printf "ring-layer refreshes from the field (dirty-gated fallback)=%lu\n", g_bTunDec2
