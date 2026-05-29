# Phase plan

Detailed breakdown of the phases summarized in `../PROJECT.md`. Each phase lists
its tasks, deliverables, and an **exit criterion** (how we know it's done).

Phases 1–5 happen on macOS with the SDL backend; Phase 6 adds the Amiga. Phase 5
("understand & rename in C") runs continuously alongside 3–4 but is called out
separately because it's the point of the C-first ordering.

---

## Phase 0 — Toolchain & project setup  *(largely done)*

**Tasks**
- Install OpenJDK 21 + Ghidra 12.1 (`tools/`).
- Parse the XEX: segment map (`tools/xex_map.py`) and post-load 64K image
  (`tools/xex_load.py` → `disasm/rof_mem.bin`).
- Headless Ghidra import → auto-analysis → listing export
  (`ghidra_scripts/MarkEntries.java`, `ExportListing.java`).
- Project docs and repo layout.

**Deliverable** A one-command regeneration of `disasm/listing.txt` from `rof.xex`.

**Exit** ✔ Achieved — 229 functions / 8,376 instructions disassembled.

---

## Phase 1 — Static map of the binary

Understand the *shape* of the program before translating it.

**Tasks**
- **Hardware-access map** (do first): headless script `DumpHwAccesses` dumping
  every read/write of `$D000–$D7FF` (GTIA/POKEY/PIA/ANTIC) and the key page-2 OS
  shadow registers (display list `$0230/1`, colors `$02C0–$02C8`, `CHBAS $02F4`,
  joystick `$0278/$0284`, etc.). → `docs/hw-access.md`.
- **Code/data separation**: many data tables are currently mis-disassembled as
  code. Mark data ranges (`MarkData` script) so the listing is trustworthy;
  optionally cross-check by round-tripping a region through `da65`.
- **Function inventory**: resolve the 4 loader stages (`$3C00`, `$5000`,
  `$1A97`, `$B800`) and the game entry `$3CDE`; give them provisional names.
- **Variable maps**: enumerate zero-page (`$00–$FF`) and RAM state usage →
  `docs/memory-map.md`; seed `disasm/symbols.csv`.
- **Display setup**: read the display list and screen-mode setup; identify
  Player/Missile graphics usage (the jaggis, rescued pilots, targeting).

**Deliverable** `docs/hw-access.md`, `docs/memory-map.md`, seeded
`disasm/symbols.csv`, a clean code/data split in `disasm/listing.txt`.

**Exit** Every hardware touch-point is named and categorized; code and data are
correctly separated; the loader→entry flow is understood.

---

## Phase 2 — C transliteration framework

Turn the disassembly into compiling C.

**Tasks**
- **6502 model** (`src/cpu/`): registers `A/X/Y/S`, status flags, the 64K
  `mem[]` array, and macros for addressing modes and flag updates (N/Z/C/V).
- **Memory bus** (`src/cpu/bus.{c,h}`): `bus_read(addr)` / `bus_write(addr,v)`
  routing RAM to `mem[]` and hardware/shadow addresses to the `platform` API.
- **Transpiler** (`tools/transpile.py`): consume `disasm/listing.txt` +
  `disasm/symbols.csv` and emit `src/gen/*.c` — one C function per 6502 routine,
  `JSR`→call, conditional branches→`goto` labels, `RTS`→`return`. Handle the
  hard cases explicitly: self-modifying code, `JMP (indirect)`, and RTS-based
  jump tables (PC-dispatch fallback or per-site annotation in `symbols.csv`).
- **Stub platform**: a no-op/headless `platform.h` implementation so the
  generated code links.

**Deliverable** `src/gen/*.c` that compiles and links against the CPU model and
a stub platform.

**Exit** The translated loader/init path runs under the stub platform without
crashing; spot-checked instruction behavior matches the emulator.

---

## Phase 3 — Platform abstraction + SDL backend (first runnable)

**Tasks**
- Finalize `src/platform/platform.h` from the Phase 1 hardware map — the API
  the game calls instead of touching chips. Provisional surface:
  - **Video**: framebuffer + palette; display-list "regions" and DLI
    (mid-screen color/mode change) hooks.
  - **Sprites**: Player/Missile graphics + collision registers.
  - **Audio**: 4 POKEY channels (AUDF/AUDC/AUDCTL); **exact POKEY RANDOM LFSR**
    (`$D20A`) — terrain generation likely seeds from it, so bit-exact matters.
  - **Input**: joystick + trigger, keyboard, paddles.
  - **Timing**: VBLANK (NMI) frame tick, DLI (raster) callbacks, POKEY timer IRQs.
- Implement `src/backend/sdl/` for macOS: framebuffer→texture, audio callback
  synth, keyboard/gamepad input, 50/60 Hz loop driving VBI + DLIs.
- Interpret the game's display list into the framebuffer.

**Deliverable** A macOS executable.

**Exit** The game boots to its title/cockpit on macOS via SDL; input responds.

---

## Phase 4 — Behavioral parity + the fractal renderer

**Tasks**
- Drive gameplay to correctness: fractal terrain (the crown jewel — midpoint
  displacement on the 6502, seeded from POKEY RANDOM), flight model, the jaggi
  aliens, downed-pilot rescue, combat/lasers, scoring, level progression.
- **Reference comparison**: run the original in Altirra (via Wine/CrossOver) or
  Atari800MacX side-by-side; diff behavior. Make POKEY RANDOM bit-exact so the
  terrain matches frame-for-frame.

**Deliverable** macOS/SDL build at behavioral parity with the original.

**Exit** 1:1 parity on representative play sessions; terrain reproduces exactly.

---

## Phase 5 — Understand & rename in C  *(continuous, formalized here)*

This is the second half of the "C-first" decision.

**Tasks**
- As routines are understood, add semantic names/types to `disasm/symbols.csv`
  and regenerate (or refactor) so `mem[0x41]` becomes e.g. `game_state`.
- Replace mechanical `goto`/register-shuffle patterns with idiomatic control
  flow where it doesn't risk fidelity; add comments capturing intent.
- Keep the original-address provenance in comments for traceability.

**Exit** The C reads as a documented reimplementation, not a transliteration,
while still matching the original's behavior.

---

## Phase 6 — Amiga backend

**Tasks**
- Implement `platform.h` for the Amiga:
  - **Video**: bitplanes + Copper; Copper colour changes for the DLI splits.
  - **Audio**: Paula 4 channels (a close match to POKEY's 4).
  - **Sprites**: hardware sprites and/or blitter for PMG + collision.
  - **Input**: joystick/keyboard; **Timing**: vertical-blank interrupt.
- Set up a cross-compiler (vbcc or bebbo's m68k-amigaos-gcc) on macOS.
- Test in WinUAE/FS-UAE, then on real hardware.

**Deliverable** An Amiga executable.

**Exit** Runs on the Amiga at parity with the SDL build.

---

## Risks / open questions

- **Indirect dispatch**: RTS-jump-tables and `JMP (ind)` are the main threat to
  a clean static transliteration; severity unknown until Phase 1's function pass.
- **Cycle-timed effects**: if the display relies on cycle-exact ANTIC/GTIA
  timing or mid-scanline register writes, the framebuffer abstraction must model
  it as per-region/per-scanline state (handled via DLI hooks).
- **Transpiler vs. hand translation**: the plan assumes a semi-automated
  transpiler for the ~8.4K instructions. If indirect dispatch is pervasive,
  fall back to hand-translating those functions. *(Confirm direction at Phase 2
  start.)*
- **Self-test / banked RAM**: confirm the game doesn't rely on RAM under the OS
  ROM beyond initial `PORTB` setup.
