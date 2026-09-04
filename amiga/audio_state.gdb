# audio_state.gdb — dump the LIVE per-frame Paula state of all four channels from the launch on.
#
# Feeds tools/paula_model.py, an offline DAC model: given (period, volume, loop length) per channel
# per frame, the model reproduces exactly what Paula emits, so an audible artefact can be located in
# the emitted signal instead of guessed at from a recording.  Loop length IS the waveform identity:
#   1 = wave_pure   15 = poly4   31 = poly5   465/511 = poly_dist   4096 = noise_buf
#
#   make clean && make -j4 PROBES=1 AUDIO_TRACE=1
#   . ./env.sh && GDBSCRIPT=audio_state.gdb ./diag_run.sh 300
#
# ⚠ Dump the arrays with `dump binary value`, never a 600-iteration print loop: the gdb stub is slow
# enough that a verbose loop gets truncated by diag_run.sh's post-SIGINT window and you silently
# read a partial capture as a complete one.
set pagination off
set confirm off
continue
echo \n==== SIGINT ====\n
printf "vbi=%u  frames captured=%u / 600  | 1st launch at vbi %u | 060B=%02x 003A=%02x VVBLKI=$%02x%02x\n", \
  'PlatformAmiga.cpp'::g_vbiCount, g_asIdx, g_atStart[0], \
  mem[0x060B], mem[0x003A], mem[0x0223], mem[0x0222]
dump binary value /tmp/as_idx.bin g_asIdx
dump binary value /tmp/as_vbi.bin g_asVbi
dump binary value /tmp/as_per.bin g_asPer
dump binary value /tmp/as_len.bin g_asLen
dump binary value /tmp/as_vol.bin g_asVol
echo dumped /tmp/as_*.bin\n
