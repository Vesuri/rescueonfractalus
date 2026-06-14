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

| screen3_mem.bin — flat 64 KB RAM from a800dumps/music_playing.a8s (R1 Option-B).
|   Standby screen (scene 3: RESCUE ON FRACTALUS title + LEVEL 04 doors-closed +
|   cockpit; DL $3000).  Music ISR active at capture time ($54C0/$54EA).
|   Doors-closed: $2000 terrain = $88 GTIA-10 fill (decoded as nibble 8 = COLPM3).
	.section .rodata
	.balign 4
	.global rof_mem_bin
	.global rof_mem_bin_end
rof_mem_bin:
	.incbin "../disasm/screen3_mem.bin"
rof_mem_bin_end:

| rof.xex — the original Atari 8-bit segmented load file (read-only).  This is the
|   faithful initial memory image: load_xex_image() (PaulaAudio.cpp) zeroes mem[] and
|   places each XEX segment at its load address, so $00E7 (music gate) and all runtime
|   state start at genuine power-on values and the original setup code (game_entry
|   $3CDE) establishes them — instead of booting from a mid-Standby snapshot.
	.section .rodata
	.balign 4
	.global rof_xex
	.global rof_xex_end
rof_xex:
	.incbin "../rof.xex"
rof_xex_end:

| (tunnel.raw removed: the concentric tunnel rings are now drawn procedurally by
|  draw_tunnel_rings_native ($65FB port) into mem[$2000] at scene init, then
|  decoded — no pre-baked bitmap.)
