# boot_chain.gdb — is the staged INITAD boot chain (src/rof_boot.c) progressing?
#
# Works on a PLAIN build (no PROBES needed): every symbol here exists in every build.
# Run:  . ./env.sh && GDBSCRIPT=boot_chain.gdb ./diag_run.sh <seconds>
#
# What to read:
#   g_bootScene 1 = Logo on screen, 2 = Station, 0 = neither (game running).
#   $008B  = the station scroll phase counter, 0 -> $94 (148) over ~184 frames.
#   $1C39/$1C3A = the display list's moving JMP operand, $B9BC -> $B800 (-3 per step).
#   $0013  = RTCLOK_MID; station_init exits at >= 4 (~17 s).
#   $0222/$0223 = the live VVBLKI: $1B30 station, $52D7 standby, $53CC card, $4FF5 flight.
# A hang shows as g_vbiCount advancing while $008B / RTCLOK stand still.
continue
echo \n==== SIGINT ====\n
printf "bootScene=%u loadBusy=%u activeVbi=%u vbiCount=%u\n", \
  g_bootScene, g_bootLoadBusy, g_activeVbi, g_vbiCount
printf "VVBLKI=%02x%02x  RTCLOK mid(13)=%02x low(14)=%02x  sync(80)=%02x\n", \
  mem[0x0223], mem[0x0222], mem[0x0013], mem[0x0014], mem[0x0080]
printf "station: phase(8B)=%02x timer(8A)=%02x idx(89)=%02x  scrollPtr=%02x%02x (end $B800)\n", \
  mem[0x008B], mem[0x008A], mem[0x0089], mem[0x1C3A], mem[0x1C39]
printf "starSeeds 1C3E=%02x 1C3F=%02x  starRow0 2CB8=%02x%02x  blankRow 2C90=%02x\n", \
  mem[0x1C3E], mem[0x1C3F], mem[0x2CB8], mem[0x2CB9], mem[0x2C90]
printf "image row0 $0600=%02x%02x%02x%02x  DL[0] $B800=%02x %02x%02x  DL[121]=%02x %02x%02x\n", \
  mem[0x0600], mem[0x0601], mem[0x0602], mem[0x0603], \
  mem[0xB800], mem[0xB801], mem[0xB802], mem[0xB800+363], mem[0xB800+364], mem[0xB800+365]
printf "game: state(41)=%02x doorRdy=%u revealRdy=%u\n", \
  mem[0x0041], g_doorFieldReady, g_standbyRevealReady
echo ==== end ====\n
