# Low-energy warning: is blink_timer $006E driven by ONE driver?
# Needs `make clean && make PROBES=1 LOW_ENERGY=1` (LOW_ENERGY pins the bar in the critical band
# so the warning is armed for the whole run).
#
# The warning's flash ($00DE = $4E/$46) and its beep (event $1C) are produced by the SAME tick of
# $4131 — but only the flight main loop's BLINK point ($5197) pushes the beep.  So:
#
#   simRel   — blinks produced by $5197  = blinks that BEEP
#   pfwRel   — blinks produced by the per-rendered-frame call in perFrameWork() = SILENT blinks
#   *Tick    — armed ticks each driver consumed (the theft itself)
#
# PASS = ticks/blink is exactly 15 (the $0F reload), with reloads > 0 and armed frames > 0.
# FAIL = ticks/blink > 15: something else is consuming $006E, so only part of the flashes beep —
#        a fraction that moves with CPU speed (the user-reported "no sound / erratic / continuous").
#
# The same run also reports startup_init ($3FFA), the OTHER routine perFrameWork used to
# double-drive: its per-rendered-frame mirror re-pushes the event-$14 range beep.
set width 0
continue
echo \n==== SIGINT ====\n
printf "blink $006E now=%02x  gauge $062F=%02x  colour $00DE=%02x\n", \
  mem[0x6e], mem[0x62f], mem[0xde]
printf "sim ($5197)   ticks=%lu  reloads=%lu  -> ticks/blink=%lu (MUST be 15)\n", \
  g_blinkTickSim, g_blinkRelSim, (g_blinkRelSim ? g_blinkTickSim/g_blinkRelSim : 0)
printf "rendered frames that saw an ARMED timer=%lu (what a second driver would steal)\n", \
  g_blinkArmedFrames
printf "startup_init ($3FFA) calls: faithful=%lu  perFrameWork mirror=%lu (want 0)\n", \
  g_siFaith, g_ckSiNative
printf "  event-$14 range-beep pushes: faithful=%lu  mirror=%lu (want 0)\n", \
  g_siFaithPush, g_siNativePush
printf "vbi=%u\n", g_vbiCount
detach
quit
