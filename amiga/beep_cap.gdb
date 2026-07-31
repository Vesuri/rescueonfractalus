# Pilot-proximity-beep capture dump.  Requires out/RoF.exe built with
#   make PROBES=1 BEEP_CAP=1
# Run:  . ./env.sh && GDBSCRIPT=beep_cap.gdb ./diag_run.sh 26
# The VBI force-pushes SFX event $14 every 2 frames once flight is live (range-1 cadence);
# flush_paula records each frame into the g_bc* ring.  ch3 is the beep channel.
# kind: 0=silent/other 1=pure 2=poly4 3=poly5 4=noise 5=poly9-dist.  R=restart bitmask.
set pagination off
continue
echo \n==== SIGINT (beep capture) ====\n
printf "g_vbiCount=%u  VVBLKI=%04x  flightVbiInstall=%u  death(63D)=%02x  range(642)=%02x  gate72=%02x  ctr4B=%02x\n", \
  g_vbiCount, (mem[0x0222] | (mem[0x0223]<<8)), g_probeFlightVbi, mem[0x063D], mem[0x0642], mem[0x0072], mem[0x004B]
printf "g_bcOn=%u  cursor(g_bcIdx)=%u   [ring newest = cursor-1]\n", g_bcOn, g_bcIdx
echo idx vbi  R | F1 C1 F2 C2 F3 C3 F4 C4 ACTL | kind[0..3] vol[0..3]\n
set $i = 0
while $i < 320
  printf "%3u %4u %x | %02x %02x %02x %02x %02x %02x %02x %02x  %02x | [%u %u %u %u] [%2u %2u %2u %2u]\n", \
    $i, g_bcVbi[$i], g_bcRestart[$i], \
    g_bcPokey[$i][0], g_bcPokey[$i][1], g_bcPokey[$i][2], g_bcPokey[$i][3], \
    g_bcPokey[$i][4], g_bcPokey[$i][5], g_bcPokey[$i][6], g_bcPokey[$i][7], g_bcPokey[$i][8], \
    g_bcKind[$i][0], g_bcKind[$i][1], g_bcKind[$i][2], g_bcKind[$i][3], \
    g_bcVol[$i][0], g_bcVol[$i][1], g_bcVol[$i][2], g_bcVol[$i][3]
  set $i = $i + 1
end
echo ==== end ====\n
