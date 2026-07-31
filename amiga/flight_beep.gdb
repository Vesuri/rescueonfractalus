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
echo idx vbi rng | F0 C0 F1 C1 F2 C2 F3 C3 | k3 v3 | vDist1234 | ring h/t\n
set $i = 0
while $i < 320
  printf "%3u %5u %02x | %02x %02x %02x %02x %02x %02x %02x %02x | k%u v%2u | %02x %02x %02x %02x | %02x/%02x\n", \
    $i, g_bcVbi[$i], g_bcAux[$i][0], \
    g_bcPokey[$i][0], g_bcPokey[$i][1], g_bcPokey[$i][2], g_bcPokey[$i][3], \
    g_bcPokey[$i][4], g_bcPokey[$i][5], g_bcPokey[$i][6], g_bcPokey[$i][7], \
    g_bcKind[$i][3], g_bcVol[$i][3], \
    g_bcAux[$i][4], g_bcAux[$i][5], g_bcAux[$i][6], g_bcAux[$i][7], \
    g_bcAux[$i][2], g_bcAux[$i][3]
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
