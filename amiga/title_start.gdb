# Title Screen -> START -> launch scene-routing trace.
# Build: make clean && make PROBES=1 FORCE_RETURN=1 ATTRACT_NOW=2 TITLE_START=1
#   (or the initial-standby A/B: make clean && make PROBES=1 NOAUTO=1 ATTRACT_NOW=1 TITLE_START=1)
# Run:   GDBSCRIPT=title_start.gdb ./diag_run.sh 220
#
# Dumps the phase gates + the live copper id from the moment the Title Screen appears, so the
# frame where the launch cinematic routes to the WRONG branch is visible.
#   VV   $0222/3 VVBLKI: 53CC=title/transitional  52D7=standby+launch cinematic  4FF5=flight
#   cop  1=title 2=standby 3=planet/stars 4=flight 5=fwd tunnel 6=doors
#        8=boost-handoff-hold 9=black 10=in-place level cycle 11=boost REVERSE tunnel
#   flg  bit0 doorFieldReady  bit1 standbyRevealReady  bit2 rsBoostViewport  bit3 rsBoostReturn
#        bit4 $008D!=0        bit5 $008E!=0
# Rows are RECORDED ON CHANGE (VV / cop / flg / $060B), so one row = one routing state.
continue
echo \n==== SIGINT ====\n
printf "transitions=%u  (recorded on change, from the FIRST Standby reveal)\n", g_tsRingN
# ⚠ No per-row BPLCON2 column: d6bc58a measured that a 68000 read of $DFF104 returns the FLOATING
# BUS, so nothing the Amiga side samples into this ring can be trusted.  The only honest read is
# gdb's own, at the bottom of this script (and per-install in amiga/b2_probe.gdb).
printf "  vbi    VV   3A 8D 8E 60B cop flg  62F sprY\n"
set $i = 0
while $i < g_tsRingN
  printf "%6u  %04x  %02x %02x %02x  %02x  %2u  %2x  %02x  %3u\n", \
    g_tsRingVbi[$i], g_tsRingVV[$i], g_tsRing3A[$i], g_tsRing8D[$i], g_tsRing8E[$i], \
    g_tsRing60B[$i], g_tsRingCop[$i], g_tsRingFlg[$i], g_tsRing62F[$i], g_tsRingSprY[$i]
  set $i = $i + 1
end
printf "\nNOW: vbi=%u VV=%04x 3A=%02x 8D=%02x 8E=%02x 60B=%02x cop=%u boostRet=%u boostVp=%u doorRdy=%u revealRdy=%u\n", \
  g_vbiCount, (mem[0x0222] | (mem[0x0223] << 8)), mem[0x003A], mem[0x008D], mem[0x008E], \
  mem[0x060B], g_liveCopper, g_boostRet, g_boostVp, g_doorFieldReady, g_standbyRevealReady
printf "attract: 00E2=%02x 062D=%02x  title RAM 365B=%02x  DMA 022F=%02x  E5=%02x 0004=%02x\n", \
  mem[0x00E2], mem[0x062D], mem[0x365B], mem[0x022F], mem[0x00E5], mem[0x0004]
# BPLCON2 ($104) ownership audit.  Per list: the value it MOVEs, or ffff = it emits none and the
# CPU owns its priority (Standby/Doors/Title, by design).  PF2P=bits5-3, PF1P=bits2-0; 0009 = pair 0
# (canopy posts) in front + the gauge BEHIND the playfield, 0024 = all sprites in front (the
# gauge-on-top bug).  cpu= the last value setSpritePriority wrote, n= how many transitions wrote it.
# Direct read-back of Denise's registers from the emulator.  Both are write-only on real hardware,
# so this is only meaningful if UAE returns the stored value — BPLCON0 is the control: it must read
# back as a plausible bitplane-count word (e.g. 3201 = 3 planes + ECSENA) for BPLCON2 to be trusted.
printf "LIVE regs: BPLCON0($DFF100)=%04x  BPLCON2($DFF104)=%04x\n", \
  *(unsigned short*)0xDFF100, *(unsigned short*)0xDFF104
printf "BPLCON2 lists: standby=%04x doors=%04x tunnel=%04x planet=%04x flight=%04x title=%04x | cpu=%04x n=%lu\n", \
  g_cl2Standby, g_cl2Doors, g_cl2Tunnel, g_cl2Planet, g_cl2Flight, g_cl2Title, g_cl2Cpu, g_cl2CpuN
