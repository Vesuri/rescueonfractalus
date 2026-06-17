# Framework upstream

Vendored from **dA JoRMaS / Template / C++** (commit-less local copy, 2026-06-08).

Classes included: `AmigaHardware`, `Bitmap`, `CopperList`, `Sprite`, `Palette`, `Util`.
Support files: `SASCCompat.h`, `compat-include/`.

**Omitted intentionally:**
- `ModulePlayer` / `TrackerPackerReplayV3.1` — RoF uses a 6502-converted POKEY→Paula backend (M5).
- `Production`, `Part`, `Script`, `ProductionRunner`, `ExampleProduction`, `ExamplePart` — replaced by the PETSCII-style `main()`+`AddIntServer`+`while(!quit)` skeleton.
- `GCCRuntime.cpp` — a modified version lives in `../src/GCCRuntime.cpp` (no `ProductionRunner` dependency; VBI via `AddIntServer`).
- All `*Assembler.s` — `make NO_ASSEMBLER=1` is the build for this plan (C++ bodies throughout; no vasm step).
- SAS/C artefacts: `smakefile`, `*.info`, `SCoptions`, `Debug/`.

Build with `make NO_ASSEMBLER=1` from the `amiga/` root (or plain `make` — the Makefile defaults to NO_ASSEMBLER).
