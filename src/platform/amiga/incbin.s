| Asset embedding for the RoF Amiga build.
| Each asset: .global + .incbin.  Chip-RAM assets (.MEMF_CHIP) go into a chip
| hunk so DMA hardware (bitplanes, copper) can reach them.
	.include "rof_data_layout.inc"

| User-owned cartridge data.  The normal/release executable reserves only BSS; the
| WHDLoad slave finds rof_data_descriptor and loads three ranges from the installed
| complete rof.rom.  An explicit STANDALONE_DATA build embeds the ignored package made
| by tools/extract_rom_data.py.  The descriptor itself contains no original data.
	.section .data
	.balign 4
	.global rof_data_descriptor
rof_data_descriptor:
	.long 0x526f4621,0x44415441       | 'RoF!','DATA'
	.word 1,0
.ifdef ROF_STANDALONE_DATA
	.long 0x52444621                  | 'RDF!' — package populated
.else
	.long 0
.endif
	.long rof_game_data
	.long ROF_DATA_PACKAGE_SIZE_ASM

.ifdef ROF_STANDALONE_DATA
	.section .rodata
.else
	.section .bss
.endif
	.balign 4
	.global rof_game_data
	.global rof_game_data_end
rof_game_data:
.ifdef ROF_STANDALONE_DATA
	.incbin "generated/rof_data.bin"
.else
	.space ROF_DATA_PACKAGE_SIZE_ASM
.endif
rof_game_data_end:

| atari_charset.bin — the Atari internal CHARACTER SET, $E000-$E3FF (1 KB, 128 glyphs x
|   8 rows).  This is the ONLY thing the port reads out of the Atari OS ROM: the
|   message/label renderer (glyph_ptr_from_index $6773 -> $E000+code*8, and set_coord_y_e0
|   $6805 -> $E080+) blits the "LEVEL 04" / "DEMO DROID" text from it.
|   load_xex_image() copies it into mem[] at $E000 after the XEX segments.
|   It REPLACED the full 14,336-byte atari_osrom.bin (-13,312 B): the only non-charset ROM
|   references in the whole binary are JSR $E45C (SETVBV) / JMP $E462 (XITVBV), which are OS
|   *calls* that the port implements as no-op stubs (os_setvbv / os_xitvbv, rof_gen.c),
|   because VBI dispatch belongs to the platform layer.  Bound proof: xex_load.h
|   (xex_overlay_charset) — the glyph pointer is built with 3 ASLs and ONE ROL, so its high
|   byte is $E0 or $E1 and can never reach $E2.
	.section .rodata
	.balign 4
	.global atari_charset
	.global atari_charset_end
atari_charset:
	.incbin "assets/atari_charset.bin"
atari_charset_end:

| Faithful cockpit lookup atlas, generated at startup from the cartridge-provided glyphs.
| It is always BSS: no original or derived game graphics live in the executable.
	.section .bss
	.balign 4
	.global rof_cockpit_planar
	.global rof_cockpit_planar_end
rof_cockpit_planar:
	.space 17952
rof_cockpit_planar_end:

| Optional standalone-build copy of the WHDLoad enhanced logo assets.  The normal
| executable does not carry these: the slave owns them and supplies them through the
| external hook.  `make ENHANCED_LOGO=1` defines ROF_ENHANCED_LOGO for both this file
| and ExternalHooks.cpp, which makes that same hook point at the blobs below even when
| the game is started directly from Workbench/Shell or amiga/run.sh.
.ifdef ROF_ENHANCED_LOGO
	.balign 4
	.global rof_enhanced_logo
	.global rof_enhanced_games
	.global rof_enhanced_palette
rof_enhanced_logo:
	.incbin "../whdload/assets/enhanced_logo.bin"
rof_enhanced_games:
	.incbin "../whdload/assets/enhanced_games.bin"
rof_enhanced_palette:
	.incbin "../whdload/assets/enhanced_logo.pal"
.endif

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
|    so --gc-sections could not drop it.  Deleting the embed cut 64 KB off RoF.
|
| ⚠ The lesson: a dead .rodata blob does NOT get collected the way a dead chip-hunk one does.
|   Check `objdump -r out/RoF.elf | grep <sym>` for a reloc count of 0, not just C references.
