# Title Screen -> START -> launch scene-routing trace.
# Build: make clean && make PROBES=1 FORCE_RETURN=1 ATTRACT_NOW=2 TITLE_START=1
#   (or the initial-standby A/B: make clean && make PROBES=1 NOAUTO=1 ATTRACT_NOW=1 TITLE_START=1)
# Run:   GDBSCRIPT=title_start.gdb ./diag_run.sh 220
#
# Dumps the phase gates + the live copper id from the moment the Title Screen appears, so the
# frame where the launch cinematic routes to the WRONG branch is visible.
#   VV   $0222/3 VVBLKI: 53CC=title/transitional  52D7=standby+launch cinematic  4FF5=flight
#   cop  1=title 2=standby 3=planet/stars 4=flight 5=fwd tunnel 6=doors
#        8=boost-handoff-hold 9=black 10=wrap-fade 11=boost REVERSE tunnel
#   flg  bit0 doorFieldReady  bit1 standbyRevealReady  bit2 rsBoostViewport  bit3 rsBoostReturn
#        bit4 $008D!=0        bit5 $008E!=0
# Rows are RECORDED ON CHANGE (VV / cop / flg / $060B), so one row = one routing state.
continue
echo \n==== SIGINT ====\n
printf "transitions=%u  (recorded on change, from the Title appearing)\n", g_tsRingN
printf "  vbi    VV   3A 8D 8E 60B cop flg\n"
set $i = 0
while $i < g_tsRingN
  printf "%6u  %04x  %02x %02x %02x  %02x  %2u  %x\n", \
    g_tsRingVbi[$i], g_tsRingVV[$i], g_tsRing3A[$i], g_tsRing8D[$i], g_tsRing8E[$i], \
    g_tsRing60B[$i], g_tsRingCop[$i], g_tsRingFlg[$i]
  set $i = $i + 1
end
printf "\nNOW: vbi=%u VV=%04x 3A=%02x 8D=%02x 8E=%02x 60B=%02x cop=%u boostRet=%u boostVp=%u doorRdy=%u revealRdy=%u\n", \
  g_vbiCount, (mem[0x0222] | (mem[0x0223] << 8)), mem[0x003A], mem[0x008D], mem[0x008E], \
  mem[0x060B], g_liveCopper, g_boostRet, g_boostVp, g_doorFieldReady, g_standbyRevealReady
printf "attract: 00E2=%02x 062D=%02x  title RAM 365B=%02x  DMA 022F=%02x  E5=%02x 0004=%02x\n", \
  mem[0x00E2], mem[0x062D], mem[0x365B], mem[0x022F], mem[0x00E5], mem[0x0004]
