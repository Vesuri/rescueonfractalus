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

| attract_mem.bin — flat 64 KB RAM extracted from attract.a8s Atari800 snapshot
|   Captured during attract mode: all state is correct ($2603 channel table,
|   $0600 bitmap, $2313/$231B source tables, audio state).  Loaded verbatim into
|   mem[] at startup so attract functions run with the right initial conditions.
	.section .rodata
	.balign 4
	.global rof_mem_bin
	.global rof_mem_bin_end
rof_mem_bin:
	.incbin "../disasm/attract_mem.bin"
rof_mem_bin_end:
