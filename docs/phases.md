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
  and regenerate so `mem[0x41]` becomes e.g. `game_state`.
- Keep the original-address provenance in comments for traceability.
- ~~Replace mechanical `goto`/register-shuffle patterns with idiomatic control
  flow.~~ **Reconsidered (2026-06-03) — see below.**

**Status (2026-06-03):** the *naming* half is effectively done. All 255 hot-path
functions + 212 vars named; zero `FUN_xxxx` tokens remain in `symbols.csv` or
`rof_gen.c`. The doc-comment notes carry intent + provenance. Renaming is
regen-safe via `symbols.csv` (never hand-edit `rof_gen.c`). 35 low-confidence
names (the speculative `saucer_*` cluster) are left as documented guesses —
**not worth polishing in isolation**; their semantics are best resolved by a
runtime trace, and confirmation falls out of Phase-6 work where relevant.

**Idiomatic control-flow refactor — DROPPED as a standalone goal.** Analysis
(2026-06-03): a blanket "make all the C idiomatic" pass is weeks of compiler-
engineering (flag-liveness, goto-structuring, register→local) for a *faithful
1:1 port* whose transliteration is valuable precisely because it's faithful-by-
construction, and it fights the regen pipeline (`rof_gen.c` is regenerated).
Crucially, the platform-abstraction design means the game *core* never needs to
be human-readable to run elsewhere. What an effective Amiga port actually needs
is **algorithmic understanding of the hardware-coupled subset**, which is Phase
6 work — not prettified C. So readability cleanup is done per-island, on demand,
only where you're about to reimplement.

**Exit** The C is documented (names + intent notes + address provenance) and
behaviourally faithful. Met.

---

## Phase 6 — Amiga backend

The naive "implement `platform.h`, recompile the same generated C" model holds
for **game logic** but breaks for code that must be *reimplemented* on alien
hardware, not recompiled. The codebase is three tiers; only Tier 3 is hard.

**Tier 1 — display rendering: already native.** The per-scanline ANTIC/GTIA/PMG
renderer lives in `PlatformSDL.cpp` as native C++ — no 6502. The Amiga port
*replaces this layer* (copper/blitter/bitplanes). This is the bulk of the
rendering work and is the clean "swap the platform layer" case.

**Tier 2 — DLI handlers: already a register-write schedule.** The DLIs in
`rof_manual.c` are flat lists of annotated `bus_write(0xD01x, …)` — essentially
"at scanline N set register R = V". Turning them into a copperlist is mostly
transcription, the lightest hardware lift.

**Tier 3 — the hardware-coupled hot path: the real work.** Terrain
generation/projection/collision (`terrain_gen_1/2/3`, `project_terrain_points`
`$A11F`, `terrain_collision_and_silhouette` `$AE53`, `divide_16x16` `$9D6F`, the RANDOM/LFSR
fractal) is transliterated 6502 that *cannot just recompile*: it fills an Atari
GTIA-mode-10 nibble buffer (wrong shape for Amiga bitplanes) and is the per-frame
hot path (per-instruction-macro overhead would miss frame rate on a 7 MHz 68000).
It needs a **native rewrite driven by algorithmic understanding**, not a port of
the transliteration.

### Strategy: dual implementation, transliteration as oracle

The current direct 6502→C build is kept as the **regenerable ground-truth
oracle**; native versions replace it **piece by piece**, each proven equivalent
before it ships. The shared `mem[]` + `cpu` state is the stable ABI seam, so any
function can be swapped for a native one that honours the same observable
contract — callers/callees don't notice.

Mechanism (built + proven 2026-06-03, see [[native-reimpl-seam]]):
- `transpile.py` `VALIDATE_FUNCS` emits the faithful transliteration under a
  `<name>__t6502` twin; the plain `<name>()` is hand-written native C in
  `src/gen/rof_native.c`. Both coexist (one shipped, one as oracle). Regen-safe;
  everything unlisted stays transliterated.
- `tools/validate_native.c` (`make validate`) runs both on identical randomized
  pre-states and diffs full `mem[]` (the contract) + cpu (incidental). The
  **contract is observable memory, not 6502 flags** — read the call sites to
  decide what actually matters (proven on `divide_16x16`: 0 memory diffs,
  ~200k cpu diffs that are dead because callers save/restore Y via `$009F`).
- Climb leaf→cluster. For the terrain hot path prefer a **coarse cluster seam**:
  validate the outer heightfield output, not each sub's exact mem effects, so the
  native renderer is free to use an Amiga-appropriate structure.

The host harness covers *logic* equivalence entirely on macOS — but *rendering*
(copper/blitter/sprites) can only be validated on Amiga.

### Tasks (rough dependency order)

0. **Toolchain + minimal display first.** Cross-compiler on macOS; FS-UAE/WinUAE;
   get a trivial bitplane fill + copper colour-split running. This is the
   highest-unknown, critical-path item — do it before writing native rendering
   code so toolchain bugs don't masquerade as algorithm bugs.
   **DE-RISKED (2026-06-04): see [amiga-spike.md](amiga-spike.md).** The full
   build→run→debug loop is proven by cross-compiling a complete dA JoRMaS
   production (C++ engine + 68k asm + embedded assets + copper/blitter/Paula) and
   running it in FS-UAE. Toolchain chosen: **BartmanAbyss `m68k-amiga-elf-gcc`**
   (not bebbo — matched gcc/gdb/elf2hunk/fs-uae for debugging). Target **68000**.
1. Implement `platform.h` for Amiga: **Video** (bitplanes + Copper), **Audio**
   (Paula 4ch ≈ POKEY 4ch), **Sprites** (hardware sprites/blitter for PMG +
   collision), **Input** (joystick/keyboard), **Timing** (VBL).
2. **DLI → copperlist** (Tier 2): extract the scanline/register schedule from
   `rof_manual.c`.
3. **Terrain renderer** (Tier 3): reverse the algorithm, write native, validate
   the heightfield against the `__t6502` oracle, draw via blitter/bitplanes.
4. **Sprite rework**: map Atari PMG (4 players + 4 missiles + 5th-player trick,
   HPOS-by-DLI, scalable widths) onto Amiga's 8 hardware sprites + bobs. The
   Phase-5 notes already record what each PM object *is* (cockpit pillars =
   missiles, AH gauge = P2, throttle = P1 repositioned by `$6da1`, etc.).
5. Test in emulator, then real hardware.

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
