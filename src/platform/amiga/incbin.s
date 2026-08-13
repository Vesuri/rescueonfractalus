| Asset embedding for RoF Amiga build (equivalent of Bin2Hunk).
| Each asset: .global + .incbin.  Chip-RAM assets (.MEMF_CHIP) go into a chip
| hunk so DMA hardware (bitplanes, copper) can reach them.

| rof.xex — the original Atari 8-bit segmented load file (read-only).  This is the
|   faithful initial memory image: load_xex_image() (XexImage.cpp) zeroes mem[] and
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

| atari_osrom.bin — the Atari OS ROM the game depends on (the platform ROM, like the
|   Amiga kickstart): $C000-$CFFF (4 KB) then $D800-$FFFF (10 KB), skipping the
|   $D000-$D7FF hardware range.  Includes the internal character set at $E000 that the
|   message/label renderer (glyph_ptr_from_index $6773 reads $E000+code*8) uses for the
|   "LEVEL 04" text.  load_xex_image() copies it into mem[] after the XEX segments.
	.section .rodata
	.balign 4
	.global atari_osrom
	.global atari_osrom_end
atari_osrom:
	.incbin "assets/atari_osrom.bin"
atari_osrom_end:

| Removed embeds (kept as notes so nobody re-adds one "because incbin.s mentions it"):
|  - tunnel.raw: the concentric tunnel rings are drawn procedurally by
|    draw_frame_pattern_seq ($65FB, src/gen/rof_native.c) at scene init, then decoded.
|  - cockpit.raw (8,960 B): the cockpit panel is decoded from the Atari cockpit bitmap in
|    mem[] with a fully dynamic palette, so the pre-baked 2-bitplane image had no reader
|    left.  --gc-sections was already dropping it (its own .MEMF_CHIP section), so this
|    costs the binary nothing — it only stops shipping a dead asset.
|  - screen3_mem.bin (65,536 B): the flat 64 KB Standby RAM snapshot the port booted from
|    before the STAGED rof.xex load (XexImage.cpp) replaced it.  It had NO reader in C, but
|    unlike cockpit.raw it was still LINKED IN: .rodata is one section that rof_xex anchors,
|    so --gc-sections could not drop it.  Deleting the embed cut 64 KB off RoF.exe.
|
| ⚠ The lesson: a dead .rodata blob does NOT get collected the way a dead chip-hunk one does.
|   Check `objdump -r out/RoF.elf | grep <sym>` for a reloc count of 0, not just C references.
