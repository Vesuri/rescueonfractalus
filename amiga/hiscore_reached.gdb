# Does the plain (no-PROBES) `make NAME_ENTRY=1 SKIPBOOT=1` build reach the high-score screen
# unattended?  Reads only globals that exist in EVERY build — mem[] and the DLIST latch — because
# the g_ne* probe counters do not exist without PROBES.
# Run: GDBSCRIPT=hiscore_reached.gdb ./diag_run.sh 120
set width 0
continue
echo \n==== SIGINT ====\n
printf "vbi=%u  VVBLKI=%04x  dlist=%04x (5e2e = the entry screen)\n", \
  g_vbiCount, (mem[0x0222] | (mem[0x0223] << 8)), g_atariDlist
printf "gate: 3700=%02x (want 28)  3714=%02x (want ee)  37C8=%02x%02x%02x (want %02x%02x%02x)\n", \
  mem[0x3700], mem[0x3714], mem[0x37C8], mem[0x37C9], mem[0x37CA], \
  mem[0x7BDB], mem[0x7BDC], mem[0x7BDD]
printf "sio: reads=%u writes=%u err=%u   persist: fromFile=%u dirty=%u written=%u\n", \
  g_hsSioRead, g_hsSioWrite, g_hsSioErr, g_hsFromFile, g_hsDirty, g_hsWritten
printf "hs dirty range=[%d..%d]  (lo>hi = nothing redrawn this frame -- no flicker)\n", \
  g_hsCellLo, g_hsCellHi
printf "heading:"
set $i = 0
while $i < 20
  printf " %02x", mem[0x3700 + $i]
  set $i = $i + 1
end
printf "\nrow0   :"
set $i = 0
while $i < 20
  printf " %02x", mem[0x3728 + $i]
  set $i = $i + 1
end
printf "\n"
