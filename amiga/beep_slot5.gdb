# Range-1 poly4 probe — slot-5 lifecycle dump.  Requires out/RoF built with
#   make PROBES=1 BEEP_CAP=1
# Run:  . ./env.sh && GDBSCRIPT=beep_slot5.gdb ./diag_run.sh 20
#
# Question: on the Amiga, is SFX voice slot 5 (the event-$01 poly4 chirp) left with a NONZERO
# volume during flight (bug → audible warble), or cleared to 0 as on the Atari (silent leftover)?
# Atari flight ground truth (savestate dumps): slot5 dist=$40 freq=$1f but VOL=0, chan=0.
#
# The capture arms at flight entry and records slot-5 state per frame into g_bcAux:
#   [0]=$0662 dist  [1]=$0670 vol(&0f)  [2]=$067e freq  [3]=$070a chan(0=unassigned)
#   [4]=$0714 mixer top-prio  [5]=$0715 mixer top-voice  [6]=$06e0 slot5 freq-env phase  [7]=$0642 range
# Plus ungated logs of every sfx_engine_reset ($5433) and event-$01 load, by g_vbiCount, to see ORDER.
set pagination off
continue
echo \n==== SIGINT (slot-5 capture) ====\n
printf "g_vbiCount=%u  flightVbi=%u   slot5 NOW: dist=%02x vol=%02x freq=%02x chan=%02x  mixer top-prio=%02x top-voice=%02x\n", \
  g_vbiCount, g_probeFlightVbi, mem[0x0662], mem[0x0670], mem[0x067E], mem[0x070A], mem[0x0714], mem[0x0715]
printf "g_bcOn=%u  cursor=%u\n", g_bcOn, g_bcIdx
# --- reset + event-$01 order (ungated, from boot) ---
printf "sfx_engine_reset fired %u times, at g_vbiCount:", g_bcResetN
set $i = 0
while $i < g_bcResetN && $i < 64
  printf " %u", g_bcResetVbi[$i]
  set $i = $i + 1
end
echo \n
printf "event-$01 loaded %u times, at g_vbiCount:", g_bc01N
set $i = 0
while $i < g_bc01N && $i < 64
  printf " %u", g_bc01Vbi[$i]
  set $i = $i + 1
end
echo \n
# --- per-frame slot-5 ring (only print rows where slot5 is nonzero-vol OR holds a channel) ---
echo idx vbi | s5:dist vol freq chan | mix:prio voice | ph rng | ch-vols(0..3) kinds\n
set $i = 0
while $i < 320
  if g_bcAux[$i][1] != 0 || g_bcAux[$i][3] != 0
    printf "%3u %4u | %02x %2u %02x %02x | %02x %02x | %02x %02x | v %2u %2u %2u %2u  k %u %u %u %u\n", \
      $i, g_bcVbi[$i], \
      g_bcAux[$i][0], g_bcAux[$i][1], g_bcAux[$i][2], g_bcAux[$i][3], \
      g_bcAux[$i][4], g_bcAux[$i][5], g_bcAux[$i][6], g_bcAux[$i][7], \
      g_bcVol[$i][0], g_bcVol[$i][1], g_bcVol[$i][2], g_bcVol[$i][3], \
      g_bcKind[$i][0], g_bcKind[$i][1], g_bcKind[$i][2], g_bcKind[$i][3]
  end
  set $i = $i + 1
end
echo ==== end ====\n
