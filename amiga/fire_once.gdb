# DOES ANYTHING STAY SWITCHED ON AFTER A SHOT?  (make FIRE_ONCE=<vbl> PROBES=1)
#
# The question this answers is NOT "what does firing cost" (AUTO_FIRE + isr_full answers that) —
# it is "once the bolt is gone, is the frame back to what it was?".  So the run presses the
# trigger exactly ONCE and splits itself into three windows:
#
#   PRE   flight settled  ->  the trigger press
#   SHOT  the press       ->  press + 200 vbl   (the whole $01..$1A animation + its explosion)
#   POST  press + 250     ->  press + 1250      (long after; nothing shot-related should be live)
#
# ⭐ READ THE ISR ROW, NOT THE FPS ROW.  Ticks per VBI FIRING is the one flight metric that is
# legitimate across differently-paced windows (the ISR fires 50x/s regardless of frame rate) —
# see the flight-measurement-rules memory.  The FPS/compute rows move with the TERRAIN the ship
# happens to be over, which is different in every window by construction, so a few percent there
# means nothing on its own.  What is decisive is:
#   - PRE vs POST ISR t/firing equal            -> nothing persistent in the 50 Hz path
#   - shot-active firings == 0 in POST          -> $0036 really did return to 0 and stay there
#   - buildShotSprite active/idle back to PRE   -> the sprite path is genuinely idle again
#
# Build: make clean && make -j4 COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1 PROBES=1 SPRITE_SHAPE=1 \
#                              PROFILE_NORING=1 NO_TDRAW_PROF=1 FIRE_ONCE=600
# Run:   . ./env.sh && GDBSCRIPT=fire_once.gdb ./diag_run.sh 500
set pagination off
set confirm off

# Snapshot every accumulator this script windows.
define snap
  set $s_vbi  = (int)g_vbiCount
  set $s_isrn = (long)g_flightProf.isrCalls
  set $s_isrl = (long)g_flightProf.isrLines
  set $s_vsp  = (long)g_vbiSpriteLines
  set $s_vau  = (long)g_vbiAudioLines
  set $s_vfc  = (long)g_vbiFullCalls
  set $s_fd   = (long)g_fdCalls
  set $s_rc   = (long)g_rCalls
  set $s_rcmp = (long)g_rRenderCompute
  set $s_act  = (long)g_foActiveFirings
  set $s_spA  = (long)g_spActive
  set $s_spI  = (long)g_spIdle
  set $s_integ= (long)g_pInteg
end

# Print one window's deltas.  $arg0 = label.
define win
  set $d_vbi  = (int)g_vbiCount            - $s_vbi
  set $d_isrn = (long)g_flightProf.isrCalls - $s_isrn
  set $d_isrl = (long)g_flightProf.isrLines - $s_isrl
  set $d_vsp  = (long)g_vbiSpriteLines     - $s_vsp
  set $d_vau  = (long)g_vbiAudioLines      - $s_vau
  set $d_vfc  = (long)g_vbiFullCalls       - $s_vfc
  set $d_fd   = (long)g_fdCalls            - $s_fd
  set $d_rc   = (long)g_rCalls             - $s_rc
  set $d_rcmp = (long)g_rRenderCompute     - $s_rcmp
  set $d_act  = (long)g_foActiveFirings    - $s_act
  set $d_spA  = (long)g_spActive           - $s_spA
  set $d_spI  = (long)g_spIdle             - $s_spI
  set $d_integ= (long)g_pInteg             - $s_integ
  set $d_isr  = $d_isrl + $d_vsp + $d_vau
  printf "\n--- %-4s  vbi %u..%u (+%d)  VVBLKI=$%02x%02x $3D=%02x ---\n", \
    $arg0, $s_vbi, g_vbiCount, $d_vbi, mem[0x223], mem[0x222], mem[0x3D]
  printf "  *** WHOLE flight VBI %5lu.%03lu t/firing   <-- THE cross-window-valid number ***\n", \
    ($d_vfc ? $d_isr/$d_vfc : 0), ($d_vfc ? ((1000*$d_isr)/$d_vfc)%1000 : 0)
  printf "      handler %lu.%03lu  sprites %lu.%03lu  audio %lu.%03lu  integ %lu.%03lu\n", \
    ($d_isrn ? $d_isrl/$d_isrn : 0), ($d_isrn ? ((1000*$d_isrl)/$d_isrn)%1000 : 0), \
    ($d_vfc ? $d_vsp/$d_vfc : 0),    ($d_vfc ? ((1000*$d_vsp)/$d_vfc)%1000 : 0), \
    ($d_vfc ? $d_vau/$d_vfc : 0),    ($d_vfc ? ((1000*$d_vau)/$d_vfc)%1000 : 0), \
    ($d_isrn ? $d_integ/$d_isrn : 0), ($d_isrn ? ((1000*$d_integ)/$d_isrn)%1000 : 0)
  printf "  shot machinery: $0036 live on %4ld of %4ld flight vbl   buildShotSprite active %4ld / idle %4ld\n", \
    $d_act, $d_vfc, $d_spA, $d_spI
  printf "  frame:  painted %4ld  FPS %2ld.%ld   render compute %5ld t/call over %ld calls\n", \
    $d_fd, ($d_vbi ? (50*$d_fd)/$d_vbi : 0), ($d_vbi ? ((500*$d_fd)/$d_vbi)%10 : 0), \
    ($d_rc ? $d_rcmp/$d_rc : 0), $d_rc
end

# PRE: open once flight has settled.
tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 1900
continue
printf "=== flight at vbi %u (level %u) — waiting for the single trigger press ===\n", \
  g_vbiCount, mem[0x6D]
snap

# ...closes the instant the press lands.
tbreak RescueOnFractalus::renderFrame if g_foFireVbi != 0
continue
win "PRE"
snap
set $fire = (long)g_foFireVbi
printf "\n>>> TRIGGER PRESSED at vbi %ld <<<\n", $fire

# RECOVERY CURVE — 50-vbl slices from the press.  A single "SHOT" window averages the spike
# with its own tail and cannot say how long the game actually feels slower; these can.
# ⚠ snap AFTER printing, or every slice runs from the press and you get cumulative averages that
# hide the peak (the first version of this script did exactly that: the real 116 t/firing peak at
# press+100..148 read as a 93 running mean).
define slice
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $fire + $arg0
  continue
  win $arg1
  snap
end
slice  50 "0-50"
slice 100 "50-100"
slice 150 "100-150"
slice 200 "150-200"
slice 250 "200-250"
slice 300 "250-300"
printf "  (shot machinery last live at vbi %lu = press + %ld)\n", \
  (long)g_foEndVbi, (long)g_foEndVbi - $fire

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $fire + 1000
continue
win "POST"

printf "\n=== verdict inputs ===\n"
printf "  total flight vbl the shot machinery was live: %lu\n", (long)g_foActiveFirings
printf "  last live at vbi %lu (press was %ld) -> %ld vbl after the press\n", \
  (long)g_foEndVbi, $fire, (long)g_foEndVbi - $fire
printf "  re-arms ($0036 0 -> non-zero with the trigger long released): %lu\n", (long)g_foRearms
printf "  state: $0004=%02x $286C=%02x $286D=%02x $284A=%02x $2867=%02x $0037=%02x $006A=%02x $003D=%02x\n", \
  g_foSnap[0], g_foSnap[1], g_foSnap[2], g_foSnap[3], \
  g_foSnap[4], g_foSnap[5], g_foSnap[6], g_foSnap[7]
# The trace is what separates "stuck at one value" from "cycling the animation forever".
# $0036 = object_anim_frame, $286A = the explosion zoom accumulator, $286B = its step.
printf "\n=== %lu consecutive flight vbl from press+800:  $0036 / $286A / $286B ===\n", \
  (long)g_foTraceN
set $i = 0
while $i < (int)g_foTraceN
  printf "  %02x %02x %02x", g_foTrace36[$i], g_foTrace6A[$i], g_foTrace6B[$i]
  set $i = $i + 1
  if ($i % 8) == 0
    printf "\n"
  end
end
printf "\n"
detach
quit
