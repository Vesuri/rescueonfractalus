| AmigaOS version string for RoF.
|
| Format (https://wiki.amigaos.net/wiki/Version_Strings):
|     $VER: <name> <version>.<revision> (<dd>.<mm>.<yyyy>)
| <name> is the game's TITLE, not the file name — that is the convention for a product.
| The WHDLoad slave uses its own FILE name instead ("$VER: RoF.slave",
| whdload/RoFSlave.s) — a slave is a component, the executable is the product.
|
| NOTHING REFERENCES THIS STRING, AND NOTHING MAY.  AmigaDOS `Version`, WHDLoad's crash
| report and every archive-inspection tool find a program's version by SCANNING the file
| for the "$VER: " magic — so the string only has to be PRESENT in the load image.  That
| is exactly the shape --gc-sections exists to delete, so the section carries the ELF
| SHF_GNU_RETAIN flag ("R" in the .section flags), which tells the linker to keep it with
| no relocation pointing at it.  A plain `.rodata` here would survive only by accident (by
| sharing a section with incbin.s's anchored blobs — see the collection lesson at the foot
| of incbin.s), and would silently vanish the day that anchor moved.
| Re-verify after ANY change to LDFLAGS, elf2hunk flags or this file:
|     strings out/RoF | grep '\$VER:'      # must print the line below
|
| ⚠ The version number lives in THREE places — keep them in step:
|     src/platform/amiga/version.s           this file (the executable)
|     whdload/RoFSlave.s                     slv_info + the slave's own $VER:
|     whdload/RoF Install/ReadMe             the History section
|
| The date is deliberately HARDCODED rather than stamped at build time (the usual
| `incbin "T:date"` idiom).  A build-time date would make every rebuild of an unchanged
| tree differ, and byte-comparing two builds is a standard tool in this repo — the
| cross-build image A/B in docs/asset-extraction.md and the "is this a stale build?"
| check in docs/headless-fsuae.md both rely on identical input giving identical output.
	.section .rodata.version,"aR"
	.balign 2
	.asciz "$VER: Rescue on Fractalus! 0.93 (31.08.2026)"
	.balign 2
