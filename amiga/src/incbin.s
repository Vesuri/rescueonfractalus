| Asset embedding for RoF Amiga build (equivalent of Bin2Hunk).
| Each asset: .global + .incbin.  Chip-RAM assets (.MEMF_CHIP) go into a chip
| hunk so DMA hardware (bitplanes, copper) can reach them.

| attract.raw — interleaved 4-bitplane 320x200 bitmap (chip RAM: bitplane DMA)
	.section .MEMF_CHIP,"a"
	.balign 4
	.global attract_raw
attract_raw:
	.incbin "assets/attract.raw"

| terrain.raw — interleaved 2-bitplane 320x86 terrain viewport (rows 42-127)
|   3 colours: black (idx 0), grey sky (idx 1), green mountains (idx 2)
	.section .MEMF_CHIP,"a"
	.balign 4
	.global terrain_raw
terrain_raw:
	.incbin "assets/terrain.raw"

| rof.xex — Atari XEX ROM image (fast RAM: CPU access only, no DMA)
|   Loaded into mem[] at startup so audio_attract can read its tables ($1BE5 etc.)
	.section .rodata
	.balign 4
	.global rof_xex
	.global rof_xex_end
rof_xex:
	.incbin "../rof.xex"
rof_xex_end:
