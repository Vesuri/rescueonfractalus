# SELECT/BREAK probe dump for diag_run.sh.
continue
echo \n==== SIGINT ====\n
printf "vbi=%u  VVBLKI=%02x%02x  365B=%02x  level_stage(6D)=%02x  060B=%02x  003A=%02x  0609=%02x  consol=%02x\n", \
  g_vbiCount, mem[0x0223], mem[0x0222], mem[0x365B], mem[0x006D], mem[0x060B], mem[0x003A], mem[0x0609], s_consolState
printf "title 365B..: %02x %02x %02x %02x %02x %02x | STARTING LEVEL 3694/5 = %02x %02x | game_state(41)=%02x\n", \
  mem[0x365B], mem[0x365C], mem[0x365D], mem[0x365E], mem[0x365F], mem[0x3660], mem[0x3694], mem[0x3695], mem[0x0041]
printf "HIGH SCORE $0605-08 = %02x %02x %02x %02x  (seeded 12 34 56 78)\n", \
  mem[0x0605], mem[0x0606], mem[0x0607], mem[0x0608]
