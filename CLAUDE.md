# Rescue on Fractalus! — Atari 8-bit → Amiga port

Reimplementing the 1985 Atari 8-bit game *Rescue on Fractalus!* on the Amiga from a
binary only (`rof.xex`, no source). Pipeline: decompile (Ghidra) → transliterate 6502
→ C → abstract hardware → platform backends (SDL on macOS for dev, Amiga A500 as
the real target). **Faithful 1:1 port** — parity before improvements; validate against
the Atari 6502 code + `atari800`, NOT PlatformSDL (SDL is an approximation).

## Phase vocabulary (use these names everywhere)

The boot→flight sequence has 7 canonical scenes (user-approved). Code ids are `SCENE_*`.

| # | Scene | What it is |
|---|---|---|
| 1 | **Logo** | Lucasfilm Games boot logo |
| 2 | **Station** | Space-station cinematic (stars scroll, station animates). `station_init $195D` |
| 3 | **Standby** | Cockpit + "RESCUE ON FRACTALUS!" title + LEVEL doors, awaiting START |
| 3b | **Title Screen** | Attract/level-select/results card: big mode-7 "RESCUE ON FRACTALUS!" + mode-6 copyright / STARTING LEVEL / RANKING LEVEL / LAST SCORE / HIGH SCORE on black, text pens cycle. Shown on Standby idle (attract timeout), on **SELECT or joystick-up from the initial Standby** (both faithful — same dispatch branch, measured 2026-08-03), or after a crash. DL `$5A82`, charset `$0400`, screen RAM `$365B`. (Was "Scoreboard".) |
| 4 | **Doors** | Hangar doors opening (start of launch) |
| 5 | **Tunnel** | Tunnel/descent |
| 6 | **Planet** | Planet approach |
| 7 | **Flight** | In-game terrain rendering (gameplay) |

The Amiga app's main class is `RescueOnFractalus` (flight is a continuation of it, not a
separate scene). Atari entry is `game_entry $3CDE`; main blob `$3CDE–$B7FF`.

## Reference docs — READ ON DEMAND (this file stays small on purpose)

Hard-won detail lives in `docs/`, not here. **Read the relevant one BEFORE working in its area**;
these are the derived facts we must not re-derive or contradict.

| Doc | Read it when |
|---|---|
| `docs/scene-composition.md` | Working on any scene's DL / screen modes / PMG / copper list / windscreen frame |
| `docs/instruments.md` | Touching the cockpit HUD, a named instrument, enemies or terrain objects |
| `docs/controls.md` | Touching input, the keyboard/console path, Standby SELECT, or BREAK/restart |
| `docs/perf-budget.md` | Quoting, sizing or judging ANY performance number; measuring an asm twin |
| `docs/m68k-optimisation.md` | Optimising a hot function or writing an asm twin (68000 rules) |
| `docs/headless-fsuae.md` | Writing a probe, driving FS-UAE headlessly, or suspecting a stale build |
| `docs/transpiler.md` | Working on `tools/transpile.py`, or when generated-code shape surprises you |
| `docs/asm-migration-plan.md` | Any asm twin work — the per-phase design record |
| `docs/flight-perf-log.md` | The perf investigation archive: what was tried, what it measured, what closed |
| `docs/sfx-events.md` | Audio: the 33 SFX events, the voice engine, captured POKEY streams |
| `docs/rename.md` | A function's name contradicts its behaviour (append to it — see conventions) |
| `docs/asset-extraction.md` | Touching the embedded boot image (`incbin.s`, `XexImage.cpp`, `xex_load.h`, `tools/make_xex_sparse.py`) or judging `RoF.exe`'s size. Carries the sparse-image format, the COVERAGE table the removal is gated on, and three A/B traps (same-length assets, dump-don't-checksum, ISR frame-lock) |
| `docs/logo-station-plan.md` | Scenes 1 (Logo) + 2 (Station): screen composition, the routines, and the Amiga port plan. ⚠ Also carries the correction that boot `INITAD $5000` is the LOGO, not `stage_5000` |
| `docs/startup-flow.md`, `docs/phases.md`, `docs/boost-cinematic-plan.md`, `docs/boost-tunnel-direct-handoff.md`, `docs/alien-jumpscare.md`, `docs/cockpit-render-plan.md`, `docs/terrain-render-plan.md`, `docs/terrain-draw-plan.md`, `docs/rescue-figure-render.md`, `docs/sprite-multiplex-plan.md`, `docs/amiga-attract-plan.md` | Per-area plans/records — check for one before designing |
| `docs/memory-map.md`, `docs/atari-hardware.md`, `docs/hw-access.md`, `docs/hw-techniques.md`, `docs/toolchain.md` | Atari/Amiga hardware + toolchain reference |

Cross-session in-progress notes live in the auto-memory (`~/.claude/projects/.../memory/`,
`MEMORY.md` is its index) — open bugs, current measurements, method lessons. Stable facts live
here or in `docs/`; memory holds what is still moving.

## Build / run / debug

### SDL (macOS dev + profiling) — from repo root
```
make            # debug build (-O0 -g)  -> build/rof
make RELEASE=1  # release (-O2 -g)
make gen        # regenerate transliterated C from Ghidra disasm (tools/transpile.py)
make validate              # run the native-vs-transpiled equivalence suite (~7 min)
make validate FN="name"    # only tests whose name contains a substring — prefer this
```

### Amiga cross-build (m68k-amiga-elf-gcc) — from `amiga/`
```
. env.sh        # put the ~/.local Amiga toolchain on PATH (source it first, SAME command)
make            # build out/RoF.exe (+ RoF.elf for debug)
./run.sh        # boot in FS-UAE (Kickstart 3.1; left mouse button quits)
./debug.sh      # source-level debug via FS-UAE GDB stub (m68k-amiga-elf-gdb; prints its $DEBUG_PORT)
```
**Never `pkill fs-uae` / `pkill gdb`** in these scripts or by hand: several Amiga projects run
their own emulator at the same time.  The run/debug/probe scripts source
`~/.local/share/amiga/fsuae_common.sh` (shared, outside every repo; `$FSUAE_COMMON` overrides the
path), which kills only the pid this directory's previous run recorded in `.run/fsuae.pid` and
gives each project its own gdb-stub `$DEBUG_PORT`.  Stop a stranger's emulator by pid, or not at
all.

Toolchain lives at `~/.local`. `OPT=-O2`/`NATIVE_OPT=-O3` by default; override for debug
backtraces with `make OPT='-O0' NATIVE_OPT='-O0'`.

Hand-written m68k asm is the norm for hot paths + framework routines (`vasmm68k_mot -m68010
-Felf` assembles the `.s`). Each twin has an `ROF_<NAME>_ASM` seam + a `make <NAME>_C=1`
C-fallback; verify with `make VERIFY=1 PROBES=1` + the matching `amiga/*_verify.gdb`
(in-process differential vs the C oracle — **NOT** cross-run render-diff). See
`docs/asm-migration-plan.md`.

⚠ **`make clean` before any `PROBES=1` build and after editing a widely-included header.** The
Amiga Makefile tracks neither, so a partial rebuild links stale objects into a
**working-but-wrong** binary with silent runtime breakage. Treat any unexplained regression right
after a header edit or a `PROBES` toggle as a stale build first. Full text + the whole headless
harness: `docs/headless-fsuae.md`.

### Headless FS-UAE loop — **measure, don't theorize**
The agent can drive FS-UAE + gdb itself with no display interaction, and should: this loop has
diagnosed timing/render bugs precisely where static reasoning kept failing.
`. ./env.sh` (same shell command) then `amiga/diag_run.sh [delay]`, editing `amiga/diag_timing.gdb`
to print whatever globals/`mem[0xNNNN]` you need. Needs a `PROBES=1` build. Details, the probe
pattern and the auto-launch trick: `docs/headless-fsuae.md`. Atari ground truth = `atari800` in
FIFO mode; RAM dumps via `tools/extract_a8s_ram.py` (RAM base `0x86`).

## Transpiler / native-reimplementation architecture

The 6502 binary is transliterated to C, then hot/slow functions are replaced with native C
twins proven equivalent by a validation harness. **`disasm/symbols.csv` is the source of
truth for names** (the transpiler reads it; never hand-rename in generated files).

| File | Role |
|---|---|
| `tools/transpile.py` | The transpiler. Reads `disasm/listing.txt` + `symbols.csv` (functions + named-memory var rows) |
| `src/gen/rof_gen.c` | Generated 6502→C transliteration (regenerated; do NOT edit by hand) |
| `src/gen/rof_decl.h` | Generated forward decls + mid-function entry wrappers |
| `src/gen/mem.h` | Generated `MEM_<name>` offset macros (477) for named Atari RAM/shadow addresses, usable in C and C++, plus an opt-in `ROF_MEM_ALIASES` block of bare lvalue aliases (`rof_gen.c` enables it). Built from `symbols.csv` var rows |
| `src/gen/rof_manual.c` | Hand-written stubs for self-modifying routines (DLI handlers etc.) |
| `src/gen/rof_native.c` | Hand-written native twins (idiomatic C `_core` + 6502-ABI shim) |
| `tools/validate_native.c` | The `make validate` harness |

**Making a function native (the regen-safe seam):**
1. Add its address to `VALIDATE_FUNCS` in `tools/transpile.py`.
2. The transpiler then emits its transliteration under a `__t6502` suffix (kept as the
   validation **oracle**), and the plain name is linked from `rof_native.c`.
3. In `rof_native.c` write two halves: a typed idiomatic `<name>_core(...)` and a
   `void <name>(void)` 6502-ABI shim that marshals `mem[]`/`cpu` ↔ the core.
4. `make validate FN=<name>` runs both on the same inputs and diffs full `mem[]` state.
   ⚠ Step 1 only makes the transpiler emit the oracle — the twin is NOT tested until you ALSO
   register a fixture (`test_mem_contract(_regs)(…)` or a custom `test_<name>()`) in
   `validate_native.c`'s `main()`. An unregistered function prints `PASS` with no `"N cases"`
   line = a vacuous green (zero comparisons run). See [[feedback-native-twin-validation-gaps]] §4.
5. Once a transpiled caller is also shed, the `VALIDATE_FUNCS` entry can be dropped and the
   plain native twin lives directly in `rof_native.c`.

**Which file a twin belongs in:** `rof_native.c` = FAITHFUL, `make validate`d twins linked into
BOTH backends; `rof_native_amiga.cpp` = genuinely Amiga-only, unvalidated code. A faithful
pure-`mem[]` routine that merely needs a small Amiga variation **stays in `rof_native.c`** with the
variation under `#ifdef ROF_PLATFORM_AMIGA`. Full rationale: `docs/transpiler.md`.

Transpiled code uses `mem[]` for RAM, a global `cpu` struct + flag-setting macros
(`LDA`/`CMP`/`ADC`…) per 6502 op, and `bus_read`/`bus_write` for hardware ($D000–$D7FF).
⚠ **`mem[]` is NOT `volatile` in the Amiga C core** (`ROF_MEM_NONVOLATILE`, on by default;
`make MEMNVC=1` reverts). It still is on the SDL host, which runs the VBI on a real thread — so
any new mem[] spin-wait must keep an opaque call in its body. Why + the cycle counts: `src/cpu/cpu.h`.
Per-op flag computation + bus dispatch is the overhead that native rewrites remove. Named RAM
addresses are emitted as `mem[MEM_<name> + i]` or bare lvalue aliases from `symbols.csv`; the
transpiler also peephole-folds dead 6502 load→store idioms (both detailed in `docs/transpiler.md`).

**Amiga specifics:** VBI bodies run in the *real* INTB_VERTB ISR (`game_vbi_isr` dispatches
on the live VVBLKI vector to standby `$52D7` / flight `$4FF5` / station `$1B30` native
bodies). Spin-wait points in transpiled code are `SPINWAIT_HOOKS` that drive one real Amiga
frame (`platform_tick_vbi(); platform_render_frame()`). Copper does the display; `bus_write`
to hardware is largely ignored on Amiga.

## Performance — the headline

**⭐ TARGET (user decision, 2026-08-08): 25 FPS = 40 ms/frame on the BEST-CASE baseline**
(`COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1`). Standing measurement: **24.88 FPS best case**
(2026-08-12, 33f0663) ⇒ **~+0.5% of throughput to go**; combat 16.00 (stale, 4d25815). Combat may sit
lower; it is not the bar. ⚠⚠ **That figure is a property of (build × WINDOW).** The same binary
reads **24.88 on `fps_seg.gdb`'s standard vbi 1900-4900 and 23.57 on vbi 4900-7900** (log §23.0),
and the same commit re-measured 24.38 then 24.09 across sessions. So: **never move the segment
list, rebuild and re-run the baseline in the SAME session, and quote the A/B** (§23's was +3.25%),
never the absolute. 50 FPS is the ideal, not the target. The A500 is a 7 MHz 68000 — spending 10 ms on *anything*
is half the budget; be conscious of absolute milliseconds always. The profile is FLAT (nothing
>32%), so no single function closes the gap — five or six honest 5-point wins do. Surface numbers
honestly.

Three rules that must survive without opening the doc:
- **`make FPSCOUNT=1` + `GDBSCRIPT=fps_seg.gdb ./diag_run.sh 400` is the ONLY way to quote a
  framerate** (400 s: 200 no longer reaches the last segment), and it over-reads wins — under ~3% is noise. Quote a static cycle count or a
  differential ratio as the win; quote FPS only as the standing baseline. ⚠ **`phase_budget.gdb`'s
  t/it rows are NOT a safer alternative — they carry ~±10% of trajectory noise and must never be
  diffed across builds** (`docs/flight-perf-log.md` §19: a change that cannot touch flight code
  moved DRAW +111 t/it). Use the phase budget for SHARES, `fps_seg` for progress.
- **Measure an asm twin with the in-process differential**, never cross-run (a render-speed change
  shifts the RNG read count and flies a *different level*). `make FIXED_RNG=1` for every perf run.
  ⭐ And **before costing a twin at all, check whether GCC is merely addressing `mem[]` absolutely**
  — `abs.l` is 16 cycles against `d16(An)`'s 12, and it picks the former even for a plain constant
  subscript. `ROF_MEMBASE_DECL(mb)` + `#define mem mb` over the body (rof_native.c) folds them all
  for three lines; it is in 13 flight routines now. ⚠ **Fold at the CALLEE, not at a giant caller**
  (all of `game_main_loop_body` was +36 bytes; `terrain_draw_frame_core`, which GCC inlines into it,
  was −222 there and −510 inside it), and **keep a fold only when the function's own `.text` drops
  in a WHOLE-TU size diff** — one fold shrank itself by 38 bytes and grew its two inline sites by
  280. Same for a local `volatile` VIEW of `mem[]`: it puts the whole §20.2 tax back on that one
  routine (use `ROF_MEM_VIEW`). Host proof: `make validate MEMBASE=1 MEMVIEW=1`. Recipe:
  `docs/asm-migration-plan.md` §Phase 12 + `docs/flight-perf-log.md` §23.
- **Every framerate figure in an older note or commit is wrong — re-measure, don't quote.**

Everything else — the per-phase budget, the RAM budget, the closed candidates (do not re-open),
the measurement traps — is in **`docs/perf-budget.md`**. The ranked TODO is in the
`flight-pc-profiler` memory.

## Hard rules (violating these costs a day)

- **Faithfulness first.** Byte-identical twins: `make validate FN=<name>` must show **0 mem
  mismatch** (incidental exit-`cpu` diffs are fine). Validate against the 6502 + `atari800`.
- **RAM is uniformly slow — there is no "fast RAM" on the target A500.** Optimise by reducing the
  NUMBER of reads/writes, never by moving data to a "cheaper" buffer. (`docs/m68k-optimisation.md`)
- **NEVER emit a 32-bit software mul/div** (`__mulsi3`/`__divsi3`/`__udivsi3`/`__modsi3`/
  `__umodsi3`) — the 68000 has none. Use `src/cpu/m68k_math.h`'s 16-bit helpers, and audit with
  `m68k-amiga-elf-objdump -d out/RoF.elf | grep -E '__(u?div|u?mod|mul)si3'` (must be empty).
- **Copper bitplane POINTER swaps happen in the VBI ISR, never mid-frame** — a torn pointer
  garbages the whole viewport for a frame. Colour-only pokes mid-frame are tolerable. See the
  `amiga-copper-lessons` memory.
- **A DLI that leaves a register untouched ⇒ the CopperList must too.** Emit only the MOVEs the
  DLI actually makes. (`docs/scene-composition.md`)

## Working conventions

- **Commit directly to `main`** (no feature branches). Commit each fix as soon as the user
  confirms it works — one logical change per commit.
- **Misnamed functions:** whenever you encounter a function whose name clearly contradicts
  what it does, append it to `docs/rename.md` immediately (address, current name, actual
  behaviour, suggested name). Do not rename piecemeal in generated files — `disasm/symbols.csv`
  is the source of truth; batch-rename later via the transpiler.
- **Newly-found DLIs:** whenever you identify a DLI handler address, add it to
  `ghidra_scripts/entrypoints.csv` so Ghidra disassembles it into `listing.txt` (DLIs are
  reachable only via indirect-jump tables, so Ghidra never finds them on its own). Same spirit
  as the `docs/rename.md` rule — record it the moment you find it, don't defer.
- **Keep this file small.** New hard-won detail goes in the matching `docs/` file (add a row to
  the index above if it's a new one), not here. This file is re-read in full on every turn of
  every session; `docs/` is read only when relevant.
- Ask the user at genuine decision points (they're an experienced retro-porter and want to
  steer architecture/scope choices).
