# Standby top-bar title/copyright probe.  Logs $0091 (block selector) + palette pen
# $00D8 + audio/attract state each time copy_title_text_block_to_screen runs.
break copy_title_text_block_to_screen_core
commands
  silent
  printf "COPY 0091=%02x 00D8=%02x | 00E7=%02x 0655=%02x 060B=%02x 0004=%02x 00E2=%02x | seqptr=%02x noteT=%02x mute=%02x\n", mem[0x91], mem[0xD8], mem[0xE7], mem[0x655], mem[0x60B], mem[0x04], mem[0xE2], mem[0x73C], mem[0x73A], mem[0x73B]
  continue
end
break sfx_seq_step
commands
  silent
  printf "SEQ  writes 0091 (next); pre-0091=%02x seqptr=%02x\n", mem[0x91], mem[0x73C]
  continue
end
break sfx_voice_tick
commands
  silent
  printf "TICK 00E7=%02x 062D=%02x noteT=%02x\n", mem[0xE7], mem[0x62D], mem[0x73A]
  continue
end
continue
