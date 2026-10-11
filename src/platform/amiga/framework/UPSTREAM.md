# Framework upstream

Vendored from **dA JoRMaS / Template / C++**, the same lineage as Revs' copy; the hand-written
`*Assembler.s` were added on 2026-06-30 (`docs/asm-migration-plan.md`, Phase 1).

Framework base: AmigaXDev commit 2e02af5, the canonical tree in `$AMIGA_FRAMEWORK`. Compare with
it and take fixes from it with `python3 -m amigaxdev.framework diff "Rescue on Fractalus"` (run
from the directory above this repository); it maps this copy's `ROF_`/`rof_` prefixes.

Classes included: `AmigaHardware`, `Bitmap`, `CopperList`, `Palette`, `Sprite`, `Util`, with
`UtilAssembler.s`, `AmigaHardwareAssembler.s`, `BitmapAssembler.s`, `CopperListAssembler.s`.
Support files: `SASCCompat.h`, `compat-include/`.

Omitted: `Palette24Bit` (AGA only), `Production`/`Part`/`ProductionRunner` (replaced by a plain
`main()` and a VBI server loop), the module player.
