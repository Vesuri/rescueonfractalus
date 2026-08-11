# ESC-pause colour cycle ("screensaver" strobe) verification.
#   build: cd amiga && make clean && make -j4 PROBES=1 FORCE_PAUSE=1
#   run:   . ./env.sh && GDBSCRIPT=pause_cycle.gdb ./diag_run.sh 45
#
# FORCE_PAUSE injects ESC during flight and clocks the genuine $5039 strobe every 8 frames (see the
# Makefile note).  The strobe rewrites ALL of $00CF-$00DD; on the Atari the flight VBI $4FF5 and the
# dashboard DLI $4A78 reload the whole cockpit from those bytes every frame, so the top bar, the
# canopy pillars and the dashboard body must cycle WITH the terrain.
#
# This prints the strobe source bytes next to the live FlightCopperList words the copper actually
# executes — NOT the updateFlightCopper cache, which would only prove the cache agrees with itself.
# Every colour MOVE is dumped in list order, so the title-region slots (first COLOR00-03 block, plus
# COLOR17/COLOR21 = the canopy posts) and the cockpit slots (the later COLOR00-07 block) both show.
#
# PASS = the COLOR words track the $00CF-$00DD bytes as they move.  atariToOCS maps an Atari
# hue/luma byte to $0RGB, so a changing source byte MUST give a changing word; a word frozen at its
# base ($0000 black / $0444 grey / $0666 / etc.) while its source byte moves is the bug.
set pagination off
set confirm off

define dumppens
  printf "  vbi=%u  $0043 event_active=%02x  $063B pending=%02x  $0012 RTCLOK=%02x\n", \
    g_vbiCount, mem[0x0043], mem[0x063B], mem[0x0012]
  printf "  VVBLKI=%02x%02x  pendingKey=%02x  $004A joy=%02x  $0630 joySave=%02x  $0014 jiffy=%02x  $003E=%02x\n", \
    mem[0x0223], mem[0x0222], s_pendingFlightKey, mem[0x004A], mem[0x0630], mem[0x0014], mem[0x003E]
  printf "  FORCE_PAUSE: escTries=%u freezeVbi=%u  (freezeVbi==0 => the pause NEVER engaged, reading is vacuous)\n", \
    g_fpTries, g_fpFreezeVbi
  # ⚠ Those two are meaningful ONLY in a FORCE_PAUSE build.  Without -DROF_FORCE_PAUSE nothing
  # references them, --gc-sections drops them, and gdb prints whatever code now sits at the stale
  # symbol address (a matching pair like escTries=freezeVbi=18663 = $48E7, a MOVEM.L opcode, is the
  # tell).  Read them only when the run was built with the flag.
  printf "  strobe $00CF-$00DE:"
  set $i = 0xCF
  while $i <= 0xDE
    printf " %02x", mem[$i]
    set $i = $i + 1
  end
  printf "\n"
  printf "    (CF=%02x compass/ckpt-PF0  D0=%02x AH  D1=%02x ckpt-PF2  D2=%02x dash-BK  D3=%02x ckpt-BK\n", \
    mem[0xCF], mem[0xD0], mem[0xD1], mem[0xD2], mem[0xD3]
  printf "     D4=%02x TOPBAR-BK+PILLARS  D7=%02x topbar-PF1  D8=%02x topbar-text  DE=%02x energy[no cycle])\n", \
    mem[0xD4], mem[0xD7], mem[0xD8], mem[0xDE]
  set $cl = (unsigned long*)s_scene->flightCopper->data_
  printf "  copper colour MOVEs (idx:reg=value):"
  set $j = 0
  set $n = 0
  while $j < 1400
    set $w = $cl[$j]
    set $r = ($w >> 16) & 0x1FF
    if $r >= 0x180 && $r <= 0x1BE
      printf " %d:%03x=%04x", $j, $r, $w & 0xFFF
      set $n = $n + 1
    end
    if $w == 0xFFFFFFFE
      set $j = 1400
    end
    set $j = $j + 1
  end
  printf "\n  (%d colour MOVEs)\n", $n
end

# Sample the strobe at three points a few strobe steps apart.  Sample 1 is wherever diag_run.sh's
# SIGINT lands (give it >=40 s: the freeze engages at flight+150 frames); samples 2 and 3 advance
# ~12 frames each, past at least one forced jiffy wrap, so each lands on a different strobe step.
# ⚠ Advance with an UNCONDITIONAL tbreak in a loop, never a per-frame conditional breakpoint — gdb
# evaluates those host-side, so a condition on a once-per-frame function costs a stub round trip
# every frame and the run never gets anywhere.
define advance12
  set $k = 0
  while $k < 12
    tbreak RescueOnFractalus::render
    continue
    set $k = $k + 1
  end
end

continue
echo \n==== PAUSE STROBE: sample 1 ====\n
dumppens

advance12
echo \n==== PAUSE STROBE: sample 2 ====\n
dumppens

advance12
echo \n==== PAUSE STROBE: sample 3 ====\n
dumppens

detach
quit
