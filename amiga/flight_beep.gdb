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
printf "display_setup entries=%u (last @vbi %u)  |  g_iterCount(flight-loop iters)=%u\n", g_dsEntryN, g_dsEntryVbi, g_iterCount
printf "setup_level_clear_state x%u  (the ONLY player_lives=2 => flight-loop break => display_setup re-invoke => event $01):\n", g_lclN
set $i = 0
while $i < g_lclN && $i < 64
  printf "  @%-5u level_or_state(0004)=%02x range(0642)=%02x levelClearedFlag=%02x lives(0072)before=%02x 003A=%02x\n", \
    g_lclVbi[$i], g_lclCtx[$i][0], g_lclCtx[$i][1], g_lclCtx[$i][2], g_lclCtx[$i][3], g_lclCtx[$i][4]
  set $i = $i + 1
end
printf "LAST PATH-3 $5a78: A(result)=%02x CONSOLread(bus)=%02x TRIG0read=%02x memD01F=%02x  (bus!=mem=>routing with s_trig0State=1 => wrong read)\n", g_p3_a, g_p3_d01f, g_p3_d010, g_p3_memd01f
printf "s_trig0State NOW=%02x   $60(fire) events seen=%u  lastFireEdge(1=down 0=up)=%u\n", \
  s_trig0State, g_bcKeyFireN, g_bcKeyFireLastDown
echo --- CONSOL $D01F transitions (vbi -> value; $07=idle $06=START-held): ---\n
set $i = 0
while $i < g_bcD01FN && $i < 128
  printf "  @%-5u D01F=%02x\n", g_bcD01FVbi[$i], g_bcD01FVal[$i]
  set $i = $i + 1
end
echo --- keyboard events (last 128, chronological from g_bcKeyIdx): vbi raw down ---\n
set $i = 0
while $i < 512
  set $k = (g_bcKeyIdx + $i) % 512
  if g_bcKeyVbi[$k] != 0
    printf "  @%-5u raw=%02x down=%u%s\n", g_bcKeyVbi[$k], g_bcKeyRaw[$k], g_bcKeyDown[$k], (g_bcKeyRaw[$k]==0x60 ? "  <== FIRE $60" : "")
  end
  set $i = $i + 1
end
printf "=== stale $81 DRAIN-READ snapshot (nothing pushes $81, so it's written OOB) ===\n"
printf "g_drain81 count=%u  first at ring tail=%02x  head=%02x  ABS ADDR=0x%04x (HW-watch this)\n", g_drain81, g_drain81Tail, g_drain81Head, g_drain81Addr
printf "ring $0719..$0738 at the first $81 read:\n"
p/x g_drain81Ring
printf "=== event-$01 ($81) PUSH caller (first occurrence, vbi=%u, count=%u) ===\n", g_push81Vbi, g_push81N
printf "ra0 = ring_push_0719's caller (ring_push_marked[X=1 event] vs ring_push_unmarked[Y=$81 slot], or an inlined pusher):\n"
info symbol g_push81Ra0
p/x g_push81Ra0
printf "envelope RE-QUEUE (sfx_voice_envelope_tick $548d line ~8568): total=%u; re-queues of event $01 by originating slot:\n", g_rqN
set $i = 1
while $i < 15
  if g_rq01BySlot[$i] != 0
    printf "  slot %-2u re-queued id=$01  x%u  <== the self-sustaining poly4 chain\n", $i, g_rq01BySlot[$i]
  end
  set $i = $i + 1
end
echo   --- last re-queues (vbi: slot -> follow-on id): ---\n
set $i = 0
while $i < g_rqN && $i < 40
  set $k = (g_rqIdx + $i) % 128
  if g_rqVbi[$k] != 0
    printf "  @%-5u slot %-2u -> id=%02x%s\n", g_rqVbi[$k], g_rqSlot[$k], g_rqId[$k], (g_rqId[$k]==1 ? "  <== event $01" : "")
  end
  set $i = $i + 1
end
printf "event-$01 load x%u  (NOTE: PATH tag is STALE from launch — event $01 in flight comes from the ring-drain, not display_setup):\n", g_bc01N
set $i = 0
while $i < g_bc01N && $i < 64
  printf "  @%-5u PATH=%u | s4($0004)=%02x s6d(stage)=%02x s6c(sndAct)=%02x s644(sndEvt)=%02x rng=%02x CONSOLread=%02x TRIG0read=%02x\n", \
    g_bc01Vbi[$i], g_bc01Ctx[$i][7], \
    g_bc01Ctx[$i][0], g_bc01Ctx[$i][1], g_bc01Ctx[$i][2], g_bc01Ctx[$i][3], \
    g_bc01Ctx[$i][4], g_bc01Ctx[$i][5], g_bc01Ctx[$i][6]
  set $i = $i + 1
end
# Per-frame slot-5 ring dump SKIPPED for speed (reading 640 rows over the gdb stub took minutes).
# Re-enable by changing the loop bound below back to 640 if the per-frame trace is needed.
echo (per-frame ring dump skipped for speed; summaries above are the decisive data)\n
set $i = 0
while $i < 0
  set $k = (g_bcIdx + $i) % 640
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
