# Rescue on Fractalus! — Atari 8-bit → Amiga port

Reimplementing the 1985 Atari 8-bit game *Rescue on Fractalus!* on the Amiga from a
binary only (`rof.xex`, no source). Pipeline: decompile (Ghidra) → transliterate 6502
→ C → abstract hardware → platform backends (SDL on macOS for dev, Amiga A500/A1200 as
the real target). **Faithful 1:1 port** — parity before improvements; validate against
the Atari 6502 code + `atari800`, NOT PlatformSDL (SDL is an approximation).

## Phase vocabulary (use these names everywhere)

The boot→flight sequence has 7 canonical scenes (user-approved). Code ids are `SCENE_*`.

| # | Scene | What it is |
|---|---|---|
| 1 | **Logo** | Lucasfilm Games boot logo |
| 2 | **Station** | Space-station cinematic (stars scroll, station animates). `station_init $195D` |
| 3 | **Standby** | Cockpit + "RESCUE ON FRACTALUS!" title + LEVEL doors, awaiting START |
| 3b | **Scoreboard** | Shown after long idle on Standby (provisional name, not fully investigated) |
| 4 | **Doors** | Hangar doors opening (start of launch) |
| 5 | **Tunnel** | Tunnel/descent |
| 6 | **Planet** | Planet approach |
| 7 | **Flight** | In-game terrain rendering (gameplay) |

The Amiga app's main class is `RescueOnFractalus` (flight is a continuation of it, not a
separate scene). Atari entry is `game_entry $3CDE`; main blob `$3CDE–$B7FF`.

## Build / run / debug

### SDL (macOS dev + profiling)  — from repo root
```
make            # debug build (-O0 -g)  -> build/rof
make RELEASE=1  # release (-O2 -g)
make gen        # regenerate transliterated C from Ghidra disasm (tools/transpile.py)
make validate              # run the native-vs-transpiled equivalence suite
make validate FN="name"    # only tests whose name contains a substring
```

### Amiga cross-build (m68k-amiga-elf-gcc, NO_ASSEMBLER) — from `amiga/`
```
. env.sh        # put the ~/.local Amiga toolchain on PATH (source it first)
make            # build out/RoF.exe (+ RoF.elf for debug)
./run.sh        # boot in FS-UAE (Kickstart 3.1; left mouse button quits)
./debug.sh      # source-level debug via FS-UAE GDB stub (m68k-amiga-elf-gdb, port 2345)
```
Toolchain lives at `~/.local`. `OPT=-O2`/`NATIVE_OPT=-O3` by default; override for debug
backtraces with `make OPT='-O0' NATIVE_OPT='-O0'`.

### Atari reference (ground truth)
Drive the `atari800` debugger in **FIFO mode**. Always `kill -9` stray `atari800`/`fs-uae`
processes (avoids stale copies). RAM dumps: `tools/extract_a8s_ram.py` (RAM base is `0x86`).

## Transpiler / native-reimplementation architecture

The 6502 binary is transliterated to C, then hot/slow functions are replaced with native C
twins proven equivalent by a validation harness. **`disasm/symbols.csv` is the source of
truth for names** (the transpiler reads it; never hand-rename in generated files).

| File | Role |
|---|---|
| `tools/transpile.py` | The transpiler. Reads `disasm/listing.txt` + `symbols.csv` + `zeropage.csv` |
| `src/gen/rof_gen.c` | Generated 6502→C transliteration (regenerated; do NOT edit by hand) |
| `src/gen/rof_decl.h` | Generated forward decls + mid-function entry wrappers |
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
5. Once a transpiled caller is also shed, the `VALIDATE_FUNCS` entry can be dropped and the
   plain native twin lives directly in `rof_native.c`.

Transpiled code uses `mem[]` for RAM, a global `cpu` struct + flag-setting macros
(`LDA`/`CMP`/`ADC`…) per 6502 op, and `bus_read`/`bus_write` for hardware ($D000–$D7FF).
Per-op flag computation + bus dispatch is the overhead that native rewrites remove.

**Amiga specifics:** VBI bodies run in the *real* INTB_VERTB ISR (`game_vbi_isr` dispatches
on the live VVBLKI vector to standby `$52D7` / flight `$4FF5` / station `$1B30` native
bodies). Spin-wait points in transpiled code are `SPINWAIT_HOOKS` that drive one real Amiga
frame (`platform_tick_vbi(); platform_render_frame()`). Copper does the display; `bus_write`
to hardware is largely ignored on Amiga.

## Working conventions

- **Commit directly to `main`** (no feature branches). Commit each fix as soon as the user
  confirms it works — one logical change per commit.
- Ask the user at genuine decision points (they're an experienced retro-porter and want to
  steer architecture/scope choices).
- Persistent cross-session notes live in the auto-memory at
  `~/.claude/projects/.../memory/` (`MEMORY.md` is its index). Stable facts live *here* in
  CLAUDE.md; memory holds in-progress/learned specifics.
