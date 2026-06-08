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

| rof.xex — Atari XEX ROM image (fast RAM: CPU access only, no DMA)
|   Loaded into mem[] at startup so audio_attract can read its tables ($1BE5 etc.)
	.section .rodata
	.balign 4
	.global rof_xex
	.global rof_xex_end
rof_xex:
	.incbin "../rof.xex"
rof_xex_end:
