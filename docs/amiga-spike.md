# Amiga toolchain spike — results & lessons

A standalone spike (in `amiga-spike/`) that de-risks **Phase 6 task 0** (Amiga
cross-compiler + emulator + minimal display) *before* writing any Rescue on
Fractalus rendering code, so toolchain bugs can't masquerade as algorithm bugs.
It proves the full **build → run → debug** loop by cross-compiling a complete real
demoscene production — the dA JoRMaS **JRm-hC74** (SAS/C C++ engine + Motorola
assembler + Bin2Hunk binary assets) — and running it in FS-UAE.

That production exercises every capability the ROF Amiga backend needs: a C++
engine, hand-written 68k assembler, embedded binary data, copper/blitter/sprite
banging, Paula audio, and interrupts. **Status: fully working** (cube + HAM6
environment-mapped vectors + music, on an emulated OCS A500).

This document is the durable record; the working reference tree is
`amiga-spike/examples/hC74/`, and the full SAS/C→GCC porting + build guide is in
the dA JoRMaS repo at `Productions/JRm-hC74/GCC-PORT.md`.

---

## Toolchain decision

Use **BartmanAbyss `vscode-amiga-debug` v1.8.2** (`m68k-amiga-elf-gcc` 15.1.0),
**not** bebbo `m68k-amigaos-gcc` (the placeholder named in `phases.md`). Reason:
Bartman's gcc / gdb / elf2hunk / patched FS-UAE are mutually matched
(DWARF ↔ gdb ↔ in-emulator stub), which is what makes **source-level debugging**
work. Native arm64 on Apple Silicon (only FS-UAE is x86_64 / Rosetta 2).

Components (extracted from the `.vsix`, which is a zip): `m68k-amiga-elf-{gcc,as,
ld,objdump,gdb}`, `elf2hunk` (ELF→Amiga HUNK), `vasmm68k_mot` (Motorola-syntax
assembler), patched `fs-uae`.

**Critical install constraint:** install to a **space-free path** (we use
`~/.amiga-spike-tc`, symlinked from `amiga-spike/toolchain`). GCC's LTO link encodes
the install path into `COLLECT_LTO_WRAPPER` and `posix_spawnp`s it; a space (as in
`…/Rescue on Fractalus/…`) breaks the spawn. `env.sh` resolves the symlink with
`pwd -P` so gcc only ever sees space-free paths.

---

## Build / run / debug loop

- **Build**: gcc `-m68000 -msoft-float -O2 -nostdlib -fno-rtti -fno-exceptions
  -std=gnu++17 -fpermissive` → ELF → `elf2hunk` → HUNK `.exe`. Hand asm via
  `vasmm68k_mot -Felf -no-opt`. (`make` in `amiga-spike/examples/hC74/`.)
- **Run**: `fs-uae --amiga_model=… --hard_drive_0=<dir> --hard_drive_1=<dir>`
  with directory hard-drives (dh0 = boot + `startup-sequence`, dh1 = the `.exe`).
  **KS 3.1 auto-boots directory HDs; KS 1.3 stalls.** Always pass
  `--window_width/--window_height` or FS-UAE stalls on a `glViewport` error from a
  non-GUI shell.
- **Debug = working** (headless gdb attach, mirrors the VS Code extension):
  launch FS-UAE with `--remote_debugger=20 --remote_debugger_port=2345
  --remote_debugger_trigger=<exe>`; attach with `HOME=<cachedir>
  m68k-amiga-elf-gdb … -ex 'target remote 127.0.0.1:2345'`. **`HOME` must be set**
  (the main blocker), use **`127.0.0.1`** not `localhost`, and **never** probe the
  port first (the stub accepts one client). The patched FS-UAE has a built-in
  CPU-exception breakpoint (`continue` stops at a fault with `backtrace`) and
  hardware `watch <sym>` works. Runtime PCs resolve straight to ELF symbols
  (load offset 0). `amiga-spike/examples/hC74/debug.sh` wraps this.

---

## Transferable lessons (the ones that cost time)

These are hardware/ABI gotchas that will recur in the ROF Amiga backend.

1. **Target the 68000, not the 68020.** `-m68020` emits 020-only constructs
   (scaled-index addressing, 32-bit `mulu.l`/`divu.l`, bit-field ops like
   `bfextu`) that **illegal-instruction-trap on a 68000** (A500). 68000 code also
   runs on the A1200's 68EC020 — universal target. Verify with an `objdump` scan.

2. **Hardware registers MUST be `volatile`.** Two real bugs from non-volatile
   register access under `-O2`:
   - A spin-wait flag (`hasQueuedBlits`) the BLIT interrupt sets, read in
     `while (flag || isBlitterBusy())` → `-O2` short-circuited `||` to an infinite
     `bra.s .`.
   - A register-playback loop storing through a non-volatile `(uint16_t*)addr` →
     the loop optimiser **peeled a duplicate of the final store**; since it was
     `bltsize` (the blit trigger), it re-fired the blitter mid-blit → corruption.
   Fix: `volatile` on the flag and on **all** `0xdffxxx` register pointers
   (double-pointer regs need `T* volatile *`, so the *store* is volatile not the
   pointee). General rule: every custom-chip access goes through a `volatile`
   pointer.

3. **Calling SAS/C register-argument asm from gcc.** This gcc rejects
   `__asm("d1")` on function *parameters* but accepts `register T v __asm("d1")`
   *locals* (the NDK `LPx` mechanism). Bridge name-mangling + register-ABI in one
   thin wrapper: gcc-mangled C++ signature, body marshals args into register
   locals and `jsr`s the raw SAS/C symbol (`"jsr _sqrt__4UtilFUl"`). Member fns
   put `this` in `a0`. Proven live; see GCC-PORT.md §4.

4. **AGA isn't detected until `SetPatch` runs.** KS 3.1 leaves
   `GfxBase->ChipRevBits0` AA bits clear until SetPatch's `SetChipRev`. Code that
   branches on AGA (`& GFXF_AA_LISA`) silently takes the OCS path on an A1200.
   Either run SetPatch in the boot sequence, or test on a genuine OCS model. (ROF
   targets OCS/A500-class first anyway.)

5. **Asset row stride vs. copy-loop width = diagonal shear.** A copy loop moving
   N longs/row from an asset whose true stride is M≠N longs/row shears every row.
   Trust the **link list** (`smakefile`) for the real asset dimensions, not the
   stale filenames lying around. (Here: 320-wide assets, 336-wide `.o` left over.)

6. **Assemble linked 68k asm with `-no-opt`.** `vasmm68k_mot` defaults to shrinking
   bare `bra/bsr/bmi` to short (`.s`), which shifts PC-relative jump tables in
   hand-written code (broke the music replay's dispatch tables → chip-RAM
   corruption). `-no-opt` keeps branch sizes as authored.

7. **`-nostdlib` C++ runtime shims** you will need on Amiga: `operator new/delete`
   via `AllocMem`/`FreeMem`, `__cxa_pure_virtual`, and the library-base globals
   (`SysBase` from `*(void**)4` in an init-array ctor; `GfxBase` after
   `OpenLibrary`, since the NDK `proto/*.h` inlines use a global base).

---

## Relevance to the ROF port

- **De-risks Phase 6 task 0**: the build/run/debug loop, embedded data, hand asm,
  and C++ engine are all proven on this exact toolchain.
- The **dual-implementation oracle seam** (`__t6502` twin + `make validate`, see
  phases.md / native-reimpl) is the macOS-side equivalence harness; this spike is
  the Amiga-side runtime that the validated native code will eventually target.
- ROF's own renderer (copper/blitter/bitplanes/Paula) can reuse the
  patterns proven here. The hC74 framework (`AmigaHardware`, `Bitmap`,
  `CopperList`) is a worked example of OCS copper/blitter banging from C++.
