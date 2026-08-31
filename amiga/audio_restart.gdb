# audio_restart.gdb — is the Paula DMA-restart off-window long enough?
#
# flush_paula holds a channel's DMA off so Paula reaches idle and latches the new AUDxLC/LEN.  The
# countdown ALREADY IN FLIGHT keeps the OLD period (AUDxPER only changes the reload value), so the
# window must cover outgoing_per + kPaulaMinPer ticks = per/227 rasterlines.  Under-waiting is not
# silent: the channel plays the OLD waveform, at the NEW volume, until its own loop wraps —
# cur_len samples later.  Which is why only a LONG outgoing loop (poly_dist 511, noise_buf 4096)
# has to be waited for; wave_pure/poly4/poly5 (1/15/31) wrap by themselves in well under a frame.
#
# Rows print needLines = outPer/227.  A row with needLines > wl AND outLen > 31 is a real bug.
# g_fpLenHist is the same question for flight: buckets [3]/[4] are the long loops, and while they
# stay 0 in flight the long-wait branch is never taken there, so flight timing cannot change.
#
# The user's reported case:
#   make clean && make -j4 PROBES=1 FORCE_RETURN=1 FORCE_RELAUNCH=1 AUDIO_TRACE=1
#   . ./env.sh && GDBSCRIPT=audio_restart.gdb ./diag_run.sh 300
# ⚠ A FORCE_DEATH build ENDS BY ITSELF before diag_run.sh's SIGINT — for that one add a
# `tbreak RescueOnFractalus::renderFrame if g_vbiCount >= <n>` before the continue.
set pagination off
set confirm off
continue
echo \n==== SIGINT ====\n
printf "vbi=%u restarts=%u | 1st launch at vbi %u, relaunch at vbi %u | 060B=%02x 003A=%02x VVBLKI=$%02x%02x\n", \
  'PlatformAmiga.cpp'::g_vbiCount, g_arIdx, g_atStart[0], g_atStart[1], \
  mem[0x060B], mem[0x003A], mem[0x0223], mem[0x0222]
printf "outLen histogram over the whole run (flight probe): pure=%lu poly4=%lu poly5=%lu polydist=%lu noise=%lu\n", \
  g_fpLenHist[0], g_fpLenHist[1], g_fpLenHist[2], g_fpLenHist[3], g_fpLenHist[4]
set $i = 0
set $bad = 0
while $i < g_arIdx
  set $need = g_arPer[$i] / 227
  if $need > g_arWl[$i] && g_arLen[$i] > 31
    set $bad = $bad + 1
    printf "%4d vbi=%5u ch=%u outPer=%5u outLen=%5u wl=%3u needLines=%3u  UNDER-WAIT\n", \
       $i, g_arVbi[$i], g_arCh[$i], g_arPer[$i], g_arLen[$i], g_arWl[$i], $need
  end
  if g_arLen[$i] > 31
    printf "%4d vbi=%5u ch=%u outPer=%5u outLen=%5u wl=%3u needLines=%3u  (long loop)\n", \
       $i, g_arVbi[$i], g_arCh[$i], g_arPer[$i], g_arLen[$i], g_arWl[$i], $need
  end
  set $i = $i + 1
end
printf "LONG-LOOP restarts still under-waited: %d (must be 0)\n", $bad
