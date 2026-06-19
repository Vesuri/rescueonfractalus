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

## Instrument vocabulary — "Valkyrie Fighter Control Panel" (use these names everywhere)

The 19 cockpit instruments (game manual p.6), with their Amiga-screen position `x,y` and
`WxH` (user-supplied 2026-06-18). Use these names in code/comments/commits. Positions are
in the 320×216 display space; use them to identify each instrument's Atari hardware source
(player P0–P3 / missile M0–M3 / mode-4·mode-D cockpit cell range).

| # | Instrument | x,y | WxH | Notes / source (✓=confirmed, ?=to verify) |
|---|---|---|---|---|
| 1 | **Score** | 304,20 | 16×8 | top-bar mode-6 text ~`$32E3` ✓ |
| 2 | **Compass** | 144,32 | 32×8 | top-bar mode-6 cells `$32C9-$32CA`, varies with heading octant `$280D` ✓ |
| 3 | **Wing Clearance Bars** | 40,128 | 240×10 | **missiles M1/M2/M3**, HPOSM3=`$2840`(+`$0C`/+`$11`), SIZEM=`$CC`, GRAFM=`$00CD` ✓ |
| 4 | **Thrust Level** | 8,152 | 40×60 | mode-4 dial-bar cells (x≈8-16), drawn via `$4581`/`draw_object_column` ✓ |
| 5 | **Dangerous Altitude** | 24,144 | 40×60 | mode-4 dial-bar cells (x≈24-32, e.g. `$3394`), lights near ground ✓ |
| 6 | **Artificial Horizon** | 56,138 | 32×28 | mode-4 cells cols ~11-14 (x56-88); bank+climb — only changes when rolling |
| 7 | **Altimeter** | 108,144 | 8×56 | **Player P3** cyan bar COLPM3=`$00D9`(`$9x`) ship-height + P2 (COLPM2=`$0037`) terrain-height; HPOSP3=`$2870`, HPOSP2=`$00CB` |
| 8 | **Targeting Scope** | 136,151 | 50×33 | centre-lower mode-4 cells (x≈136) |
| 9 | **Main Window** | — | — | the terrain viewport |
| 10 | **Cross Hairs** | 136,69 | 50×37 | centre of Main Window |
| 11 | **Enemy Lock-On Indicator** | 136,193 | 48×6 | mode-4 cells `$3492-$3496` (`lock_on_indicator_tick $4229`, state `$007E`) ✓ |
| 12 | **Energy Level Indicator** | 204,144 | 8×56 | **P1 strip `$0D98`** gauge sprite, HPOSP1=`$00B5` (the working "right gauge") ✓ |
| 13 | **Long Range Scanner** | 232,138 | 32×28 | mode-4 cells (x≈232) |
| 14 | **Shields On** | 288,136 | 6×4 | tiny status light |
| 15 | **Mother Ship** | 300,140 | 6×4 | tiny status light |
| 16 | **Air Lock Open** | 312,144 | 6×4 | tiny status light |
| 17 | **Range To Pilot** | 284,155 | 8×10 | small digit |
| 18 | **Enemies Destroyed** | 276,171 | 8×10 | small digit |
| 19 | **Pilot Quota/Rescued** | 268,187 | 8×10 | small digit |

The **canopy posts** (cockpit window A-pillars) are a separate frame element = Atari players
P0 (`$0C32`, left) / P1 (`$0D32`, right), RLE-decoded from tables `$4DFA`/`$4E09`.

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

### Headless FS-UAE measure→fix→verify loop (works great — use it instead of guessing)
The agent can drive FS-UAE + gdb itself, with no display interaction, to measure real
runtime state. This loop diagnosed several timing/render bugs precisely where static
reasoning kept failing — **measure, don't theorize.**
- **`. ./env.sh` MUST be sourced in the SAME shell command** as the run — it puts BOTH
  `fs-uae` (`~/.local/fs-uae`) and `m68k-amiga-elf-gdb` on PATH. The Bash tool doesn't
  persist a separate `. env.sh`, so `fs-uae` looks "not found" otherwise.
- **`amiga/diag_run.sh [delay]`** = the batch harness: boots `out/RoF.exe` under the
  FS-UAE gdb stub, runs `[delay]` seconds, SIGINTs gdb (breaks its `continue`), runs the
  print commands in **`amiga/diag_timing.gdb`**, and writes everything to
  `amiga/.run/gdb-out.log` (also echoes a filtered tail). Edit `diag_timing.gdb` to print
  whatever globals/`mem[0xNNNN]` you need (a `while $i < N ... end` loop dumps arrays).
  `-g` is always on (AUDIO_CFLAGS), so all globals are readable by name.
- **Build with probes:** `cd amiga && make PROBES=1` (→ `-DROF_FLIGHT_PROBE -DROF_TDRAW_PROF`).
  This is OFF by default — the probes + auto-launch + timing accumulators are now PERMANENT,
  guarded code (committed), not throwaway edits. With probes off, the SDL build + `make validate`
  link cleanly. ⚠ `diag_run.sh`/`diag_sample.sh` need an `out/RoF.exe` built with `PROBES=1`.
- **Auto-launch (reach the launch cinematic with no keypress):** `PlatformAmiga.cpp`'s
  `vbiHandler` (under `ROF_FLIGHT_PROBE`) replicates a real RETURN press —
  `if (g_vbiCount==350) mem[0xD01Fu]=0x06;` (START down) then `=0x07` (up). ⚠ `0x00` (all
  console keys) triggers the DEMO DROID path, NOT a clean START.
- **Probe pattern:** for one-off probes add `volatile` globals under `#ifdef ROF_FLIGHT_PROBE`
  (defs in `PlatformAmiga.cpp`, hooks via the `FP_*` macros in `rof_native.c`), stamp
  `g_vbiCount`/`rof_subclock()` at milestones, and print them from `diag_timing.gdb`. Edit
  `diag_timing.gdb` freely to print whatever globals/`mem[0xNNNN]` you need (a
  `while $i < N ... end` loop dumps arrays); `-g` is always on so all globals are readable.
  Pure-compute stretches show up as `g_vbiCount` deltas (the real VBI ISR bumps the counter).

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
- **Misnamed functions:** whenever you encounter a function whose name clearly contradicts
  what it does, append it to `docs/rename.md` immediately (address, current name, actual
  behaviour, suggested name). Do not rename piecemeal in generated files — `disasm/symbols.csv`
  is the source of truth; batch-rename later via the transpiler.
- Ask the user at genuine decision points (they're an experienced retro-porter and want to
  steer architecture/scope choices).
- Persistent cross-session notes live in the auto-memory at
  `~/.claude/projects/.../memory/` (`MEMORY.md` is its index). Stable facts live *here* in
  CLAUDE.md; memory holds in-progress/learned specifics.
