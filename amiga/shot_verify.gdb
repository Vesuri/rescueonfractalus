# SHOT EXTENT DIFFERENTIAL — buildShotSprite's mem[$2865]/mem[$2866] read vs the old 94-byte scan.
#
# buildShotSprite used to find the P2 strip's top/bottom by scanning all 94 bytes of $0E34..$0E91
# with no early exit (16.4 of the sprite bracket's 29 beam ticks = ~5% of ALL wall clock, since the
# flight VBI fires 50x/s regardless of frame rate).  build_player2_sprite ($8C58) already records
# the strip's start row in $2865 and its length in $2866 — it needs them to erase exactly that
# region on its next call — and nothing else in the binary writes either byte, so the scan was pure
# re-derivation.  This is an Amiga-only render mirror, so `make validate` cannot check it: instead
# SHOT_VERIFY=1 reinstates the scan alongside the new read and compares them every call.
#
# Build: make clean && make -j4 COMBAT=1 PROBES=1 FIXED_RNG=1 SHOT_VERIFY=1
# Run:   . ./env.sh && GDBSCRIPT=shot_verify.gdb ./diag_run.sh 240
# PASS = "bad 0".  Any nonzero prints the disagreeing pair plus $0036/$2865/$2866 at that call.
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2000
continue
printf "=== level=%u  (start vbi=%u) ===\n", mem[0x6D], g_vbiCount

define seg
  tbreak RescueOnFractalus::renderFrame if g_vbiCount >= $arg0
  continue
  printf "vbi %5u | VVBLKI=$%02x%02x $3D=%02x | $36=%02x $2865=%02x $2866=%02x | calls %6lu bad %lu benign %lu\n", \
    g_vbiCount, mem[0x223], mem[0x222], mem[0x3D], mem[0x36], mem[0x2865], mem[0x2866], \
    g_svCalls, g_svBad, g_svBenign
end
seg 2600
seg 3400
seg 4200
seg 5000
seg 5800

printf "\n=== RESULT: %lu compared, %lu PIXEL MISMATCH, %lu benign (extent differs, all-zero rows) ===\n", \
  g_svCalls, g_svBad, g_svBenign
if g_svCalls != 0
  printf "  last extent disagreement: new top=$%02lx rows=%lu   oracle top=$%02lx rows=%lu   anyLit=%lu\n", \
    g_svLastTop, g_svLastRows, g_svOraTop, g_svOraRows, g_svLastLit
  printf "  state at that call: $0036=%02lx  $2865=%02lx  $2866=%02lx\n", \
    g_svLast36, g_svLast2865, g_svLast2866
end
detach
quit
