| Asset embedding for RoF Amiga build (equivalent of Bin2Hunk).
| Each asset: .global + .incbin.  Chip-RAM assets (.MEMF_CHIP) go into a chip
| hunk so DMA hardware (bitplanes, copper) can reach them.

| attract.raw — interleaved 4-bitplane 320x200 bitmap (chip RAM: bitplane DMA)
	.section .MEMF_CHIP,"a"
	.balign 4
	.global attract_raw
attract_raw:
	.incbin "assets/attract.raw"
