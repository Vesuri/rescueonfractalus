| Asset embedding for the RoF Amiga build.
| Each asset: .global + .incbin.  Chip-RAM assets (.MEMF_CHIP) go into a chip
| hunk so DMA hardware (bitplanes, copper) can reach them.

| rof_boot_image.bin — the SPARSE boot memory image (tools/make_xex_sparse.py), generated
|   from ../rof.xex + ../disasm/listing.txt.  Produces the same faithful initial mem[] the
|   original Atari loader does — $00E7 (music gate) and all runtime state at genuine power-on
|   values, established by the original setup code via the INITAD chain ending at game_entry
|   $3CDE — but omits 15,467 of the 6502 INSTRUCTION bytes.  The port transliterates the code
|   to C, so nothing is ever executed out of mem[]; the image is only a DATA source.
|   43,066 B -> 27,872 B (-35%).  Format + the stage rule: xex_load.h (xex_sparse_stage).
|   Which bytes go is decided by STATIC reachability (tools/xex_deadset.py reachable_map),
|   not by play-testing: every byte any absolute, indexed or immediate-built-pointer read
|   could touch is KEPT.  Path coverage cannot gate this — the combat path is not
|   run-to-run deterministic (two runs of ONE binary differ in 1024 bytes).
|   ⚠ Do NOT hand-edit or "optimise" the blob: its zero runs are load-bearing CLOBBERS of
|     earlier segments (segment 16 buries the logo staged at $5000 and the station image at
|     $4000), which is why the generator simulates mem[] rather than filtering dead bytes.
|   `make FULLXEX=1` embeds the original ../rof.xex instead and switches the loader back to
|   the segment walk — the A/B to reach for if the sparse image is ever suspected.
	.section .rodata
	.balign 4
	.global rof_boot_image
	.global rof_boot_image_end
rof_boot_image:
.ifdef ROF_FULL_XEX
	.incbin "../rof.xex"
.else
	.incbin "assets/rof_boot_image.bin"
.endif
rof_boot_image_end:

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
