# Who owns each of the 16 exec interrupt vectors while we are running, and which of them are
# SERVER CHAINS?  Needed before touching any vector: only the chain vectors (iv_Data = a struct
# List) may be extended with AddIntServer; the rest are single handlers owned by a device, where
# AddIntServer would enqueue into that owner's private data.
#
# ExecBase->IntVects[16] starts at +84, 12 bytes each: iv_Data(0) iv_Code(4) iv_Node(8).
# A chain vector's iv_Code is exec's chain walker (all chains share the same few addresses) and
# its iv_Data points at a List whose lh_Head/lh_Tail are self-consistent; a single-handler
# vector's iv_Code is the device's own handler.  We print both, plus the iv_Node name, and try
# to walk iv_Data as a List (names only look sane for real chains).
#
# Also: PORTS traffic attribution (g_kbCalls = our CIA-A SP keyboard handler entries).
#
# Build: make clean && make -j4 PROBES=1 FIXED_RNG=1
# Run:   . ./env.sh && GDBSCRIPT=intvec_dump.gdb ./diag_run.sh 60
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFlightDirect
continue
printf "\n==== exec IntVects[] while RoF runs (vbi=%u) ====\n", g_vbiCount
set $exec = *(unsigned long *)0x4
printf "ExecBase=%08x\n", $exec
echo "idx name       iv_Data   iv_Code   iv_Node name\n"
set $i = 0
while $i < 16
  set $iv = $exec + 84 + $i*12
  set $dat = *(unsigned long *)$iv
  set $cod = *(unsigned long *)($iv + 4)
  set $nod = *(unsigned long *)($iv + 8)
  set $nm = 0
  if $nod != 0
    set $nm = *(unsigned long *)($nod + 10)
  end
  if $nm != 0
    printf "%2d  %08x  %08x  %s\n", $i, $dat, $cod, (char *)$nm
  else
    printf "%2d  %08x  %08x  -\n", $i, $dat, $cod
  end
  set $i = $i + 1
end
echo \n(vector order: 0 TBE 1 DSKBLK 2 SOFTINT 3 PORTS 4 COPER 5 VERTB 6 BLIT 7-10 AUD0-3\n
echo  11 RBF 12 DSKSYN 13 EXTER 14 INTEN 15 NMI.  5 VERTB should be OUR "RoF VBI".)\n

echo \n==== walk the candidate chains as Lists (garbage names => NOT a chain) ====\n
set $c = 3
while $c < 7
  printf "-- IntVects[%d] iv_Data=%08x:\n", $c, *(unsigned long *)($exec + 84 + $c*12)
  set $lh = *(unsigned long *)($exec + 84 + $c*12)
  set $n = 0
  if $lh > 0x400 && $lh < 0x1000000
    set $n = *(unsigned long *)$lh
  end
  set $k = 0
  while $n > 0x400 && $n < 0x1000000 && *(unsigned long *)$n != 0 && $k < 8
    printf "   [%d] node=%08x pri=%-4d name=%s\n", $k, $n, \
      *(signed char *)($n + 9), (char *)*(unsigned long *)($n + 10)
    set $n = *(unsigned long *)$n
    set $k = $k + 1
  end
  if $n > 0x400 && $n < 0x1000000 && $k < 8
    printf "   [%d] node=%08x pri=%-4d name=%s  (tail)\n", $k, $n, \
      *(signed char *)($n + 9), (char *)*(unsigned long *)($n + 10)
  end
  if $k == 0
    printf "   (empty or not a List)\n"
  end
  set $c = $c + 1
end
printf "-- IntVects[13] (EXTER) iv_Data=%08x:\n", *(unsigned long *)($exec + 84 + 13*12)
set $n = *(unsigned long *)(*(unsigned long *)($exec + 84 + 13*12))
set $k = 0
while $n > 0x400 && $n < 0x1000000 && *(unsigned long *)$n != 0 && $k < 8
  printf "   [%d] node=%08x pri=%-4d name=%s\n", $k, $n, \
    *(signed char *)($n + 9), (char *)*(unsigned long *)($n + 10)
  set $n = *(unsigned long *)$n
  set $k = $k + 1
end
if $n > 0x400 && $n < 0x1000000 && $k < 8
  printf "   [%d] node=%08x pri=%-4d name=%s  (tail)\n", $k, $n, \
    *(signed char *)($n + 9), (char *)*(unsigned long *)($n + 10)
end

echo \n==== PORTS attribution: our CIA-A SP keyboard handler ====\n
printf "g_kbCalls=%lu  (last 16 raw SDR bytes, newest last; keycode = ROR(~sdr,1)):\n", g_kbCalls
set $i = 0
while $i < 16
  set $s = g_kbRing[(g_kbRingIdx + $i) & 15]
  printf "  sdr=%02x -> code=%02x %s\n", $s, ((((unsigned char)~$s) >> 1) | (((unsigned char)~$s) << 7)) & 0xff, \
    (((((unsigned char)~$s) >> 1) | (((unsigned char)~$s) << 7)) & 0x80) ? "(up)" : "(down)"
  set $i = $i + 1
end
printf "PORTS tap firings so far=%lu (tails=%lu)\n", g_tapHeadCnt[1], g_tapTailCnt[1]
printf "EXTER tap firings so far=%lu\n", g_tapHeadCnt[0]
detach
quit
