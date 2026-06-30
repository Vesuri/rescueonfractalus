# Framework upstream

Vendored from **dA JoRMaS / Template / C++** (commit-less local copy, 2026-06-08).

Classes included: `AmigaHardware`, `Bitmap`, `CopperList`, `Sprite`, `Palette`, `Util`.
Support files: `SASCCompat.h`, `compat-include/`.

**Hand-written m68k asm (vendored 2026-06-30, asm-migration-plan Phase 1):**
- `UtilAssembler.s`, `AmigaHardwareAssembler.s`, `BitmapAssembler.s`, `CopperListAssembler.s`
  — vendored from `amiga-spike/examples/hC74/src/*Assembler.s` (the dA JoRMaS originals),
  with the **dotted-in-middle local labels sanitised** for vasm
  (`sed -E 's/([A-Za-z0-9])\.([A-Za-z_][A-Za-z0-9_]+)/\1_\2/g'`, e.g. `cl.cpu`→`cl_cpu`,
  `u.topLeftXOk`→`u_topLeftXOk`) — vasm mot reads a leading `.` as a local-label marker.
  Assembled by `vasmm68k_mot -m68010 -Felf` (the only >68000 instruction is `movec vbr,d0`
  in `getVBR`, which the C++ `getVBR` already emits via raw bytes through `Supervisor()`).
  GCC reaches them through register-marshalling bridges under
  `#if defined(ASSEMBLER) && !defined(__SASC)` in the matching `.cpp`s; `AmigaHardware.h`
  aliases the blitter-queue statics to their SAS/C mangled names (`ROF_SASC_ALIAS`) so the
  asm's `xref`s resolve.  `Palette`/`Sprite` have no asm counterpart (pure C++ everywhere).

**Omitted intentionally:**
- `ModulePlayer` / `TrackerPackerReplayV3.1` — RoF uses a 6502-converted POKEY→Paula backend (M5).
- `Production`, `Part`, `Script`, `ProductionRunner`, `ExampleProduction`, `ExamplePart` — replaced by the PETSCII-style `main()`+`AddIntServer`+`while(!quit)` skeleton.
- `GCCRuntime.cpp` — a modified version lives in `../src/GCCRuntime.cpp` (no `ProductionRunner` dependency; VBI via `AddIntServer`).
- SAS/C artefacts: `smakefile`, `*.info`, `SCoptions`, `Debug/`.

Build with plain `make` from the `amiga/` root (ASSEMBLER on by default); pass
`make CPPFLAGS+=-DNO_ASSEMBLER` to force the portable C++ bodies (no vasm step).
