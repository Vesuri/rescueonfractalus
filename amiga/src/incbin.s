| Asset embedding for RoF Amiga build (equivalent of Bin2Hunk).
| Each asset: .global + .incbin.  Chip-RAM assets (.MEMF_CHIP) go into a chip
| hunk so DMA hardware (bitplanes, copper) can reach them.

| cockpit.raw — interleaved 2-bitplane 320x104 cockpit panel (rows 136-240)
|   palette: 0=dark, 1=panel grey, 2=medium grey, 3=orange-red lights
	.section .MEMF_CHIP,"a"
	.balign 4
	.global cockpit_raw
cockpit_raw:
	.incbin "assets/cockpit.raw"

| rof_mem.bin — flat 64 KB post-loader memory snapshot (fast RAM: CPU only, no DMA)
|   Loaded verbatim into mem[] at startup.  This is the game's memory state
|   immediately after the Atari OS loader finished, at the entry to game_entry().
|   All attract-mode tables ($2603 channel table, $0600 bitmap, audio state) are
|   already in their correct initial state — no XEX parsing or attract_mode_init
|   emulation required.
	.section .rodata
	.balign 4
	.global rof_mem_bin
	.global rof_mem_bin_end
rof_mem_bin:
	.incbin "../disasm/rof_mem.bin"
rof_mem_bin_end:
