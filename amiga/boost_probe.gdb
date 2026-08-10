# Boost return-to-mother-ship reverse-tunnel decode cost.
#   cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1
#   . ./env.sh && GDBSCRIPT=boost_probe.gdb ./diag_run.sh 120
#
# The reverse cinematic no longer DECODES its ring field at all: draw_symmetric_span_loop /
# plot_terrain_span / draw_frame_guide_columns hand every $1000 write straight to the Amiga
# painter, and the copper picks between the ring bitmap and the star bitmap per band.  So the only
# decode left is the starfield (g_bStarDec, twice per cinematic).  The §2 block below is what
# proves the painted bitmap is right; the §0a blocks say which call sites fed it.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "=== boost decodes (starfield only — the rings are painted, not decoded) ===\n"
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
printf "=== ROF_TUNNEL_RECT hook ===\n"
printf "ring rects=%lu  door rects skipped=%lu\n", g_trCalls, g_trDoors
set $i = 0
while $i < 8
  printf "  [%d] rowTop=%3lu rowBot=%3lu  xL=%3lu xR=%3lu  byte %2lu..%2lu  colour=%lu\n", $i, \
    (g_trRows[$i] >> 8) & 0xff, g_trRows[$i] & 0xff, \
    (g_trCols[$i] >> 24) & 0xff, (g_trCols[$i] >> 16) & 0xff, \
    (g_trCols[$i] >> 8) & 0xff, g_trCols[$i] & 0xff, g_trPen[$i]
  set $i = $i + 1
end
# handoff doc §0a: which CALL SITE produced each rectangle, crossed with the render phase.
# src 1 = draw_frame_pattern_seq (the 43-rectangle static pre-draw), 2 = draw_ring_frame_step
# (forward descent), 3 = step_accum_sub_7e (boost reverse).  One draw_frame_pattern_seq is
# exactly 43 rectangles (sum of the $6E0F thickness table), so runs*43 should equal count.
printf "=== §2 direct-painter fidelity (tunnelBitmap vs a decode of the live $1000 field) ===\n"
printf "frames=%lu  bytes compared=%lu  mismatching=%lu\n", g_dpFrames, g_dpBytes, g_dpBad
printf "  EXTRA (bitmap has ink, field is background)=%lu   MISSING (field write not seen)=%lu\n", g_dpExtra, g_dpMissing
if g_dpBad > 0
  printf "  first: row=%lu byte=%lu  got=%02lx %02lx %02lx  want=%02lx %02lx %02lx\n", \
    (g_dpFirst >> 8) & 0xff, g_dpFirst & 0xff, \
    (g_dpGot >> 16) & 0xff, (g_dpGot >> 8) & 0xff, g_dpGot & 0xff, \
    (g_dpWant >> 16) & 0xff, (g_dpWant >> 8) & 0xff, g_dpWant & 0xff
  printf "  mismatch by COLUMN (byte 0..39):\n    "
  set $i = 0
  while $i < 40
    printf "%lu ", g_dpCol[$i]
    set $i = $i + 1
  end
  printf "\n  row42 field: "
  set $i = 0
  while $i < 40
    printf "%02x ", g_dpSrc42[$i]
    set $i = $i + 1
  end
  printf "\n  row42 bmpP1: "
  set $i = 0
  while $i < 40
    printf "%02x ", g_dpB1[$i]
    set $i = $i + 1
  end
  printf "\n"
end
printf "=== §0a: rectangles by CALL SITE x phase ===\n"
printf " src                              count   phase: none boostRet boostVp\n"
set $s = 0
while $s < 5
  printf "  %d %-28s %5lu        %8lu %8lu %8lu\n", $s, \
    ($s == 0 ? "(untagged)" : ($s == 1 ? "predraw @L_6047" : \
    ($s == 2 ? "draw_ring_frame_step" : ($s == 3 ? "step_accum_sub_7e" : "predraw @tunnel_prebuild")))), \
    g_trBySrc[$s], g_trPhase[$s*3], g_trPhase[$s*3+1], g_trPhase[$s*3+2]
  set $s = $s + 1
end
printf "=== §0a: run timeline (a run = one burst from one site; 43 rects = a full pre-draw) ===\n"
printf "runs seen=%lu\n", g_trRunN
printf "  #  src  vbi range     rects  phase  8D 8E\n"
set $i = 0
while $i < 16 && $i < g_trRunN
  printf "  %-2d  %d   %5u..%-5u  %4u   %s  %02x %02x\n", $i, g_trRunSrc[$i], \
    g_trRunVbi0[$i], g_trRunVbi1[$i], g_trRunCnt[$i], \
    (g_trRunPhase[$i] == 0 ? "none    " : (g_trRunPhase[$i] == 1 ? "boostRet" : "boostVp ")), \
    g_trRun8D[$i], g_trRun8E[$i]
  set $i = $i + 1
end
printf "=== tunnelPaintBegin claims (tunInst MUST be 0 — else a displayed bitmap was repainted) ===\n"
printf "claims=%u\n", g_tpbN
set $i = 0
while $i < 4 && $i < g_tpbN
  printf "  [%d] vbi=%u tunnelCopperInstalled=%u liveCopper=%u\n", $i, g_tpbVbi[$i], g_tpbTunInst[$i], g_tpbCopper[$i]
  set $i = $i + 1
end
