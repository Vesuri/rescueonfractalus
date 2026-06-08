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

| standby_mem.bin — flat 64 KB RAM extracted from a800dumps/attract.a8s, which is
|   the STANDBY screen (scene 3: RESCUE ON FRACTALUS title + LEVEL 04 doors-closed
|   + cockpit; the $3000 DL). NOTE: this is a doors-closed capture — $2000=$88 flat
|   fill, and the Station-cinematic tables ($283E/$1BF4/$1BE5/$2313/$2603) are empty
|   filler here (they belong to scene 2, the Station cinematic). Per the Option-B
|   rework (docs/amiga-attract-plan.md, R1) this is to be replaced by a screen-3
|   snapshot from music_playing.a8s. Loaded verbatim into mem[] at startup.
	.section .rodata
	.balign 4
	.global rof_mem_bin
	.global rof_mem_bin_end
rof_mem_bin:
	.incbin "../disasm/standby_mem.bin"
rof_mem_bin_end:
