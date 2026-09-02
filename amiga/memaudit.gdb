# Memory-leak audit: walk exec's REAL free lists before takeover and after the restore.
#
#   make clean && make FORCE_QUIT=900 && . ./env.sh
#   GDBSCRIPT=memaudit.gdb ./diag_run.sh 90
#
# AvailMem (memreport.gdb) only gives a total.  This walks every MemHeader's mh_First
# MemChunk chain, so a leak shows up as a chunk that shrank or vanished, and pure
# fragmentation (same total, more chunks) is distinguishable from a real leak.
#
# Offsets into struct MemHeader (exec/memory.h), Node = 14 bytes:
#   +14 mh_Attributes  +16 mh_First  +20 mh_Lower  +24 mh_Upper  +28 mh_Free
# struct MemChunk: +0 mc_Next  +4 mc_Bytes.  ExecBase->MemList is a List at +$142.
set pagination off
set confirm off

define dumpfree
  set var $eb = *(unsigned long *)4
  set var $mh = *(unsigned long *)($eb + 0x142)
  set var $tail = $eb + 0x142 + 4
  set var $hdr = 0
  while $mh != $tail && $mh != 0
    set var $attr = *(unsigned short *)($mh + 14)
    set var $free = *(unsigned long  *)($mh + 28)
    printf "  MH%d attr=%04x lower=%08lx upper=%08lx free=%lu\n", \
      $hdr, $attr, *(unsigned long *)($mh + 20), *(unsigned long *)($mh + 24), $free
    set var $mc = *(unsigned long *)($mh + 16)
    set var $n = 0
    while $mc != 0
      printf "    chunk %2d  %08lx  %8lu\n", $n, $mc, *(unsigned long *)($mc + 4)
      set var $mc = *(unsigned long *)($mc)
      set var $n = $n + 1
    end
    printf "    (%d chunks)\n", $n
    set var $mh = *(unsigned long *)$mh
    set var $hdr = $hdr + 1
  end
end

define audioregs
  printf "  DMACONR=%04x  ADKCONR=%04x  INTENAR=%04x  INTREQR=%04x\n", \
    *(unsigned short *)0xdff002, *(unsigned short *)0xdff010, \
    *(unsigned short *)0xdff01c, *(unsigned short *)0xdff01e
end

tbreak PlatformAmiga::run
continue
printf "\n=== FREE LISTS BEFORE TAKEOVER ===\n"
dumpfree
audioregs

# audioShutdown is the last thing scene.shutdown() does; catch the state right after it.
tbreak PlatformAmiga::audioShutdown
continue
finish
printf "\n=== AFTER audioShutdown ===\n"
audioregs

tbreak rof_hiscore_flush
continue
finish
printf "\n=== FREE LISTS AFTER FULL RESTORE ===\n"
dumpfree
audioregs
printf "\n=== END ===\n"
detach
quit
