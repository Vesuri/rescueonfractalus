# Interactive flight range-1 pilot-beep capture.  Build:  make PROBES=1 BEEP_CAP=1
# Connects to the running FS-UAE stub and `continue`s while you play.  The BEEP_CAP ring
# auto-arms in flight when the range-to-pilot digit $0642 ticks to 1 or 2, records 320
# frames of all 4 channels + range context, then freezes.  When you're done approaching a
# pilot, the harness SIGINTs gdb (like diag_run) and this dumps the frozen ring.
# kind: 0=silent/other 1=pure 2=poly4 3=poly5 4=noise 5=poly9-dist.
set pagination off
set confirm off
set remotetimeout 120
target remote 127.0.0.1:2345
echo \n>>> connected — fly toward a downed pilot; capture auto-arms at range<=2 <<<\n
continue
echo \n==== SIGINT — dumping ring ====\n
printf "range(642)=%02x descent(3d)=%02x pilotdist(79)=%02x g_bcIdx=%u g_bcOn=%u\n", mem[0x0642],mem[0x003D],mem[0x0079],g_bcIdx,g_bcOn
printf "slot5 NOW: dist=%02x vol=%02x freq=%02x chan=%02x  mixer top-prio=%02x top-voice=%02x\n", \
  mem[0x0662],mem[0x0670],mem[0x067E],mem[0x070A],mem[0x0714],mem[0x0715]
printf "sfx_engine_reset x%u @:", g_bcResetN
set $i = 0
while $i < g_bcResetN && $i < 64
  printf " %u", g_bcResetVbi[$i]
  set $i = $i + 1
end
echo \n
printf "display_setup entries=%u (last @vbi %u)\n", g_dsEntryN, g_dsEntryVbi
printf "event-$01 load x%u  (path 1=$6150[$006c&$0644==0] 2=$62f4[$0004] 3=$5a78[CONSOL/TRIG]):\n", g_bc01N
set $i = 0
while $i < g_bc01N && $i < 64
  printf "  @%-5u PATH=%u | s4($0004)=%02x s6d(stage)=%02x s6c(sndAct)=%02x s644(sndEvt)=%02x rng=%02x D01F=%02x s627=%02x\n", \
    g_bc01Vbi[$i], g_bc01Ctx[$i][7], \
    g_bc01Ctx[$i][0], g_bc01Ctx[$i][1], g_bc01Ctx[$i][2], g_bc01Ctx[$i][3], \
    g_bc01Ctx[$i][4], g_bc01Ctx[$i][5], g_bc01Ctx[$i][6]
  set $i = $i + 1
end
# ring WRAPS (holds the last 320 frames); walk chronologically from g_bcIdx (oldest).
# g_bcAux = slot-5 lifecycle: [0]dist [1]vol [2]freq [3]chan [4]mix-prio [5]mix-voice [6]ph [7]range
echo vbi | s5:dist vol freq chan | mix:prio voice | rng | F2 C2 F3 C3 | k v(0..3)\n
set $i = 0
while $i < 320
  set $k = (g_bcIdx + $i) % 320
  printf "%5u | %02x %2u %02x %02x | %02x %02x | %02x | %02x %02x %02x %02x | %u %u %u %u/%2u %2u %2u %2u\n", \
    g_bcVbi[$k], \
    g_bcAux[$k][0], g_bcAux[$k][1], g_bcAux[$k][2], g_bcAux[$k][3], \
    g_bcAux[$k][4], g_bcAux[$k][5], g_bcAux[$k][7], \
    g_bcPokey[$k][2], g_bcPokey[$k][3], g_bcPokey[$k][4], g_bcPokey[$k][5], \
    g_bcKind[$k][0], g_bcKind[$k][1], g_bcKind[$k][2], g_bcKind[$k][3], \
    g_bcVol[$k][0], g_bcVol[$k][1], g_bcVol[$k][2], g_bcVol[$k][3]
  set $i = $i + 1
end
echo ==== SFX event pushes (id @ vbi), cursor order ====\n
printf "g_bcPushIdx=%u\n", g_bcPushIdx
set $j = 0
while $j < 256
  printf "%02x@%u ", g_bcPushId[$j], g_bcPushVbi[$j]
  set $j = $j + 1
end
echo \n==== end ====\n
detach
quit
