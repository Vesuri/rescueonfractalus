| Loader-patchable startup configuration for RoF.
|
| Each setting lives in a retained writable block prefixed by two magic longwords.
| This lets a loader locate and patch it without baking executable hunk offsets into
| the loader.  The SHF_GNU_RETAIN flag ("R") keeps these otherwise unreferenced magic
| blocks when linking with --gc-sections.

| WHDLoad-configurable BPLCON3 value.
	.section .data.bplcon3,"awR"
	.balign 4
	.long 0x526f4621		| 'RoF!'
	.long 0x42504c33		| 'BPL3'
	.globl rof_bplcon3_value
rof_bplcon3_value:
	.word 0x0c10			| $0c00 | BPLCON3_BRDNTRAN (blanking off)
	.word 0			| keep the retained block longword-sized

| Startup-selected flight terrain renderer.  The scene snapshots this during
| initialize(); changing it afterwards is deliberately unsupported.
	.section .data.terrain_renderer,"awR"
	.balign 4
	.long 0x526f4621		| 'RoF!'
	.long 0x54455252		| 'TERR'
	.globl rof_enhanced_terrain_value
rof_enhanced_terrain_value:
.ifdef ROF_ENHANCED_TERRAIN
	.word 1			| enhanced native-resolution terrain build
.else
	.word 0			| 0 = original 2x2 unless a loader patches this word
.endif
	.word 0			| keep the retained block longword-sized

| Startup-selected palette interpretation.  The renderer snapshots this during
| initialize(); only positively identified fades use the enhanced half-luma table.
	.section .data.enhanced_palette,"awR"
	.balign 4
	.long 0x526f4621		| 'RoF!'
	.long 0x4550414c		| 'EPAL'
	.globl rof_enhanced_palette_value
rof_enhanced_palette_value:
.ifdef ROF_ENHANCED_PALETTE
	.word 1			| enhanced-fades build
.else
	.word 0			| 0 = faithful Atari colour resolution unless a loader patches this word
.endif
	.word 0			| keep the retained block longword-sized

| Startup-selected enhanced artwork/rendering path. WHDLoad Custom4 patches this together with
| installing the enhanced-logo hook, so one player-facing option owns all enhanced graphics.
	.section .data.enhanced_graphics,"awR"
	.balign 4
	.long 0x526f4621		| 'RoF!'
	.long 0x45475246		| 'EGRF'
	.globl rof_enhanced_graphics_value
rof_enhanced_graphics_value:
.ifdef ROF_ENHANCED_GRAPHICS
	.word 1			| enhanced standalone build: logo + cockpit path
.else
	.word 0			| faithful graphics unless a loader patches this word
.endif
	.word 0			| keep the retained block longword-sized
