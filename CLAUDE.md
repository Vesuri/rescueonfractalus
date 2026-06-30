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
| 3b | **Title Screen** | Attract/level-select/results card: big mode-7 "RESCUE ON FRACTALUS!" + mode-6 copyright / STARTING LEVEL / RANKING LEVEL / LAST SCORE / HIGH SCORE on black, text pens cycle. Shown on Standby idle, joystick-up, or after a crash. DL `$5A82`, charset `$0400`, screen RAM `$365B`. (Was "Scoreboard".) |
| 4 | **Doors** | Hangar doors opening (start of launch) |
| 5 | **Tunnel** | Tunnel/descent |
| 6 | **Planet** | Planet approach |
| 7 | **Flight** | In-game terrain rendering (gameplay) |

The Amiga app's main class is `RescueOnFractalus` (flight is a continuation of it, not a
separate scene). Atari entry is `game_entry $3CDE`; main blob `$3CDE–$B7FF`.

## Scene composition reference (DL / screen modes / PMG / windscreen-frame elements)

Hard-won base findings so we don't re-derive them. **Verify addresses against a live dump
before trusting** (use `/atari-dl-analyzer` + headless `atari800`).

**Launch cockpit DL (Standby / Doors / Tunnel / Planet share it) = `$3000`**, set *directly*
in ANTIC `DLISTL/H`; the `$0230` shadow points at `$B832` (a stale blank text DL — don't walk
it). Layout (DL-relative scanlines): y+20 **mode 6** top bar (Score `$32E3`, Compass `$32C9`);
y+32 mode 4; y+40 mode D; y+42..127 **mode F** viewport (one LMS/scanline stepping `$1000+`,
stride `$2E`) rendered under **GTIA mode 10** (`PRIOR=$94`, 9-colour — pixel value selects a
COLPM*/COLPF* register, NOT normal 1bpp); y+128..135 **mode D `$350D`** windscreen-bottom band
(4 rows × 2 = Amiga scanlines 172-179); y+136..215 **mode 4 `$332D`** dashboard.

**Launch DLI chain** (`VDSLST=$6CAD`): dispatch on index `$C7` through word table **`$6DBB`** =
`[$4A0C, $6CD7, $6CF1, $6D28, $6D42, $6D4F, $6D7C, $6D99]`; tail `$4A05` does `INC $C7`; `$4ACD`
resets `$C7`. Key: `$6CD7` PMG colours `COLPM1/2/3=mem[$08D7/8/9]`+`PRIOR=$94`; `$6CF1`
`COLBK=mem[$0071]`+`COLPF0/1/2=mem[$08D4/5/6]`; `$6D4F` `COLPF=$04/$06/$2C` grey; `$6D7C`
`COLBK=$00`. (DLIs are reachable only via this indirect-jump table, so Ghidra never disassembles
them → absent from `listing.txt`; seed each DLI addr as a Ghidra entry point to persist them —
now done via `ghidra_scripts/entrypoints.csv`.)

**Stars/Planet DLI chain (`VDSLST=$6CC2`, a SEPARATE dispatcher from `$6CAD` above):** `$6CC2`
dispatches `$C7` through word table **`$6DCF`** (not `$6DBB`). Its key handlers, top→bottom:
`$6D0E` viewport playfield pens `COLPF0/1/2=$24/$28/$2A` (the 3-tone planet/star body); `$6D67`
**windscreen-bottom band frame** — writes ONLY `COLPF0=$04`/`COLPF1=$06` (the two cockpit greys)
and **deliberately leaves COLPF2 and COLBK untouched**, so COLPF2 stays `$2A` (the planet's
brightest tone) = the salmon seen through the corner triangles, and COLBK stays black; `$6DA1`
dashboard `COLPF2=$2C`+`COLBK=$90` (dark blue). ⚠ `$6D4F` (`$04/$06/$2C`) is a LAUNCH-chain
handler that governs the **dashboard**, NOT this band — don't use it for the stars/planet band.
**Faithful copper rule:** if a DLI leaves a register untouched, the CopperList must too — emit
only the MOVEs the DLI actually makes (e.g. `PlanetCopperList`'s band block emits just
`color01`/`color02`, inheriting `color00`/`color03` from the viewport).

**Windscreen-frame PMG (measured `launch_1_title`):**
- **Pillars (window edges):** MISSILES — M0+M1 (right), M2+M3 (left), `SIZEM=$00`, **grey**
  (`COLPF3=$06`, 5th-player). `HPOS` converges inward going down (perspective); `GRAFM` cycles
  per region. (NOT players. Amiga `leftPost`/`rightPost` 16px sprites approximate these.)
- **Corner-triangle wedge:** PLAYERS P0 (left `HPOS$2D`) / P1 (right `HPOS$BE`), **`SIZEP=$03`
  quad (~32px)**, `GRAFP=$FF` at scanlines 136-143 only, **green** (`COLPM0/1=mem[$0071]`),
  buffer `$0C88-$0C8F`/`$0D88-$0D8F`. The narrowing comes from the grey frame masking the block.

**Windscreen-corner triangle per scene:**
| Scene | Implementation |
|---|---|
| **Standby** | band mode-D corners = `COLBK` (DLI background, green). On the Atari COLBK is ONE register, green continuously from the viewport top through the band (measured `launch_1_title`: COLBK=`$C8` y50-136 → `$00` y138). The Amiga mirrors this: the door field decodes COLBK (GTIA-10 value 8) → **`color00`** via `kNibbleColour` (8→pen0; road dots value-0→pen3), so `color00`=green flows from the terrain region straight into the band — no per-band poke. `INDEX_DASH_BG` flips `color00`→black below. (Was a `setBandBgColor` `color00` split; removed.) The green quad-player wedge is present/full below it. |
| **Doors** | band green (`color00`), inherited the same way as Standby — COLBK is green across the WHOLE Doors viewport *including the tunnel reveal* (measured `doors_mid`: rings are playfield pens `$34/$36/$38` over green COLBK), so `color00`=green set once on band0 flows through all 3 terrain bands into the band. (Was `setBandBgColor`; removed.) Reveal hasn't started. |
| **Tunnel** | the green wedge **recedes top-down** = the green→purple reveal. `FUN_6a27` (called from `$538D` in `launch_anim_dispatch $5367`) does `DEC $008C` (wedge height 8→0) + clears `$0C88+` one line/frame, **gated behind `$0088==0 && $0089==0 && $008B==0`** (ring tick paused). Native `launch_anim_dispatch_native` was missing this `$008C` branch; restored — recede now runs on the Amiga, rendered by `TunnelCopperList::setBandReveal`. |
| **Flight (7)** | triangles are **bitmap value-2 (COLPF1)**, NOT PMG. |
| **Planet/Stars (6)** | **bitmap**, NOT PMG (the planet is the mode-D viewport bitmap). The windscreen-bottom band ($1810, Amiga scanlines 172-179) is the bottom 4 mode-D viewport rows under the `$6D67` frame palette: black bg + two greys (`$04/$06`) + `COLPF2=$2A` (planet) — value-2-dominant bitmap reads as the grey frame, value-3 edges = the salmon planet in the corner gaps. `PlanetCopperList` band block emits only `color01/color02` (mirrors `$6D67`). The grey **edge pillars** are still the 5th-player missiles (`COLPF3=$06`) — not yet ported. |

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
| 6 | **Artificial Horizon** | 56,138 | 32×28 | **PMG (NOT cells)** — dial frame is static $33xx bitmap; brown ground fill is Atari player P2 (COLPM2=`$26`, SIZEP2 dbl, buffer `$0E92-$0EB2`), boundary moves with pitch. Amiga = 2 sprites (`buildAHSprite`). See [[flight-scene]]. ✓ |
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

## Controls (Atari manual → Amiga port)

The Atari controls (game manual) and the Amiga key chosen for each in the port. Two distinct
input paths: **console/joystick** (PIA PORTA `$D300` directional bits + TRIG0 `$D010` fire +
CONSOL `$D01F` START/SELECT/OPTION, all active-low, polled), and **in-flight keyboard commands**
(POKEY keyboard IRQ `irq_handler $462A` → KBCODE → `event_sequence_dispatcher $4644`, delivered on
the Amiga via the CIA-A keyboard ISR; the transpiler `PRE_INSN_HOOKS[$519c]` feeds the keycode into
the flight VBI's CLI window). Faithful 1:1 — the dispatcher logic is the Atari binary's.

| Atari control | Action | Atari KBCODE | Amiga key (rawkey) | Path |
|---|---|---|---|---|
| START | Start the game | — (CONSOL) | RETURN ($44) | CONSOL $D01F bit0 |
| BREAK | Restart (score lost, highs kept) → `game_loop_reset` | $80 | Del ($46) | kbd cmd $519c |
| ESC | Freeze/pause mission (toggle) | $1c | Esc ($45) | kbd cmd |
| CURSOR RIGHT | Increase Thrust (`INC $006F`, Y5) | $06 (Ctrl-`+`, masked) | **. period ($39)** | kbd cmd |
| CURSOR LEFT | Decrease Thrust (`DEC $006F`, Y4) | $07 (Ctrl-`*`, masked) | **, comma ($38)** | kbd cmd |
| L | Land | $00 | L ($28) | kbd cmd |
| S | Systems | $3e | S ($21) | kbd cmd |
| A | Air Lock | $3f | A ($20) | kbd cmd |
| B | Boosters | $15 | B ($15) | kbd cmd |
| (joystick) | Steer (pitch/roll) | — | **arrow keys** ($4C/$4D/$4F/$4E) | PORTA $D300 bits 0/1/2/3 = up/down/left/right |
| (trigger) | Fire | — | **Control ($63)** | TRIG0 $D010 |
| SELECT / SHIFT-SELECT | Level up / down (Standby) | — | not wired | CONSOL |
| OPTION | Demo (DEMO DROID) | — | not wired | CONSOL |
| SYSTEM RESET | Reboot disk | — | not wired (Amiga reset) | — |

Implementation: `PlatformAmiga.cpp` `kFlightKeys` (one-shot command keycodes), `s_portaState`/
`s_trig0State` (held joystick/fire level read by `hwRead`). SDL build delivers none of these
(`flightIrqKey`→$FF, PORTA neutral) — Amiga-only for now.

## Build / run / debug

### SDL (macOS dev + profiling)  — from repo root
```
make            # debug build (-O0 -g)  -> build/rof
make RELEASE=1  # release (-O2 -g)
make gen        # regenerate transliterated C from Ghidra disasm (tools/transpile.py)
make validate              # run the native-vs-transpiled equivalence suite
make validate FN="name"    # only tests whose name contains a substring
```

### Amiga cross-build (m68k-amiga-elf-gcc) — from `amiga/`
> Hand-written m68k asm is the norm for hot paths + framework routines (`-DNO_ASSEMBLER` is gone;
> `vasmm68k_mot -m68010 -Felf` assembles the `.s`). Done: the framework `*Assembler.s` (GCC bridges),
> the flight terrain rasterizer (`TerrainRasterizeAssembler.s`: `terrain_column_rasterize_core` +
> `flight_edge_plot_asm`). Verify asm twins with `make VERIFY=1 PROBES=1` + `amiga/raster_verify.gdb`
> (in-process differential vs the C oracle — NOT cross-run render-diff). `make RASTER_C=1` falls back
> to the C. See `docs/asm-migration-plan.md`.
```
. env.sh        # put the ~/.local Amiga toolchain on PATH (source it first)
make            # build out/RoF.exe (+ RoF.elf for debug)
./run.sh        # boot in FS-UAE (Kickstart 3.1; left mouse button quits)
./debug.sh      # source-level debug via FS-UAE GDB stub (m68k-amiga-elf-gdb, port 2345)
```
Toolchain lives at `~/.local`. `OPT=-O2`/`NATIVE_OPT=-O3` by default; override for debug
backtraces with `make OPT='-O0' NATIVE_OPT='-O0'`.

⚠ **Always `make clean && make -j4 PROBES=1` before a headless probe run** (a plain `make`
may have built a non-probe binary in between), and **run `make clean` after toggling `PROBES`
OR after editing a widely-included header** (`RescueOnFractalus.h`, `PlatformAmiga.h`, the
framework headers, …). The Amiga Makefile does **not** track the `PROBES` flag or header
dependencies, so a partial rebuild links **stale object files** against new code. The failure
mode is NOT just a link error — it
often links a **working-but-wrong binary** that runs with **silent runtime breakage**
(e.g. struct layout / member-offset mismatches when a header changed, manifesting as
unrelated corrupted rendering or wrong behaviour). Treat any unexplained runtime
regression right after a header edit or a `PROBES` toggle as a stale build until a clean
rebuild rules it out — don't chase it as a logic bug first.

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
  `-g` is always on (CORE_CFLAGS), so all globals are readable by name.
- **Build with probes:** `cd amiga && make clean && make -j4 PROBES=1` (→ `-DROF_FLIGHT_PROBE
  -DROF_TDRAW_PROF`); the `make clean` is mandatory (see the stale-build ⚠ above).
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
| `tools/transpile.py` | The transpiler. Reads `disasm/listing.txt` + `symbols.csv` (functions + named-memory var rows) |
| `src/gen/rof_gen.c` | Generated 6502→C transliteration (regenerated; do NOT edit by hand) |
| `src/gen/rof_decl.h` | Generated forward decls + mid-function entry wrappers |
| `src/gen/mem.h` | Generated `MEM_<name>` offset macros (305) for named Atari RAM/shadow addresses, usable in C and C++, plus an opt-in `ROF_MEM_ALIASES` block of bare lvalue aliases (`rof_gen.c` enables it). Built from `symbols.csv` var rows |
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

**Named memory accesses (`mem.h`):** `symbols.csv` is the single source of truth not just for
function names but for named RAM/shadow addresses. The transpiler builds a `VAR_NAMES` map from
its var rows and emits named accesses instead of raw `mem[$NNNN]`: a direct access becomes a bare
lvalue alias (`player_lives = cpu.X`, via the `ROF_MEM_ALIASES` block in `mem.h`), while
indexed / RMW / `bus_write` forms use `mem[MEM_<name> + i]`. `rof_native.c` is auto-converted to
the same named forms; `rof_native_amiga.cpp` + `RescueOnFractalus.cpp` use `mem[MEM_*]` directly.
(The old `zp::`/`AtariZp.h` namespace was removed — these are general RAM/shadow addresses, not
zero page.)

**Peephole folding (liveness-gated):** the transpiler runs a backward-CFG register/flag liveness
fixpoint (over A/X/Y/N/Z/C/V; function exits = all-live) and folds the faithful-but-ugly 6502
load→store idioms into direct C assignments when the loaded register **and** the N/Z flags it set
are provably dead after the store and the sequence is straight-line (no end is a branch
target / split / injected hook):
- `LD{R}(#imm); ST{R} addr` → `addr = imm;` (or `bus_write(addr, imm)` for hw/shadow), including a
  run of consecutive same-reg stores (`LDA #0; STA a; STA b` → `a = 0; b = 0;`).
- `LDA $x; STA $y` → `$y = $x;` (load-memory, **single store only**, so a later store in a run
  can't change what an earlier-read source should hold).
Indexed/indirect modes count their index register as a read, and `JSR` reads+clobbers everything,
so no live value/flag is ever dropped. This shrank `rof_gen.c` by ~1490 lines vs the pre-peephole
baseline while keeping `make validate` fully green (0 `mem[]` mismatch).

**Amiga specifics:** VBI bodies run in the *real* INTB_VERTB ISR (`game_vbi_isr` dispatches
on the live VVBLKI vector to standby `$52D7` / flight `$4FF5` / station `$1B30` native
bodies). Spin-wait points in transpiled code are `SPINWAIT_HOOKS` that drive one real Amiga
frame (`platform_tick_vbi(); platform_render_frame()`). Copper does the display; `bus_write`
to hardware is largely ignored on Amiga.

### Performance budget (judge every number against this)

**Target: 50 FPS on the Amiga = 20 ms/frame. Fallback 25 FPS = 40 ms/frame, only if 50 is
genuinely not doable.** Spending 10 ms on *anything* is HALF the budget. The A500 (7 MHz 68000)
is slow — there is no room for half measures; be conscious of absolute milliseconds, always.

Units: 1 probe "tick" = 1 raster scanline = 63.56 µs; a PAL frame = 313 ticks = 20 ms. The flight
VBI ISR fires once per real frame; its per-firing cost (~56 ticks ≈ 3.6 ms) is mostly the faithful
50Hz sim+audio (hard to cut). The terrain draw dominates flight compute.

⚠ **The old "mem-bound ALGORITHMIC FLOOR / 50 FPS needs an algorithmic change / micro-opt EXHAUSTED"
conclusion (2026-06-29) is RETIRED — it was disproven.** Hand-written m68k ASM broke that floor with
NO faithfulness loss (the C *was* near GCC's floor; the floor was GCC, not the algorithm). Done
2026-06-30: `terrain_column_rasterize_core` asm twin (~27% faster than the C oracle) + instruction-
shaving (another ~9%); `renderFlightDirect`'s plane-1 edge-plot asm (~2.8×). See
`docs/asm-migration-plan.md` + [[flight-scene]]. **The lever for hot code is hand-asm (control the
regs, force `(a0)+`/`(d8,a0)`, shave every redundant insn), not the transliteration model.**

**CURRENT MEASURED BUDGET (per iteration, deep flight, all asm in):** terrain draw both passes ~167ms
(dominant) [rasterize ~64% (asm'd) · project_terrain_points ~20% · subdivide ~16%] · VBI ~71ms (3.6ms
× ~20 firings/iter, faithful) · renderFlightDirect ~24ms · setup+clear ~31ms. NEXT asm targets (open):
project_terrain_points (recommended), terrain_subdivide_column, terrain_frame_setup.

⚠ **Measure asm twins with the in-process differential** (`make VERIFY=1 PROBES=1` +
`amiga/raster_verify.gdb`): asm + C oracle run back-to-back on the SAME inputs in ONE run, byte-compared
+ beam-ticks tallied per-impl. This is DETERMINISTIC. **Cross-run comparison (render-diff, or beam-probe
asm-build-vs-C-build) is NOT** — the async 50Hz VBI desyncs frames vs the free-running main loop when
render speed changes (vbi 2204 vs 2217 at the same `fdCalls`).
**Target: A500, 50 FPS goal (25 FPS acceptable fallback only if 50 is genuinely not doable).**
Surface the numbers honestly.

### Optimising a native twin for the 68000 (hard-won; apply when a function is hot)

The transliteration→native step gets you a *correct* twin; it is NOT fast. The transliterated
style (`mem[addr]` for every access, `bus_read`/`bus_write`, per-op temporaries) is memory-bound
on the 68000. **The dominant cost is the number of memory accesses, not arithmetic** — the
68000 has no cache, every load/store goes to RAM, and `mem[]` is `volatile` (shared with the
VBI/audio ISRs) so the compiler can't cache, batch, or reorder a single access. (`mem[]` lives
in FAST RAM — verified `&mem[0]`≈`0x264fe8` — so this is raw access latency, NOT chip-DMA
contention; don't bother reasoning about chip-vs-fast.) Rewrite hot functions in idiomatic C:

- **Keep loop scratch / running pointers / loop-invariants in locals (registers), not `mem[]`.**
  The transliteration re-reads/-writes ZP scratch every iteration (e.g. `terrain_collision_and_silhouette` hit
  `$80/$81/$95/$96` ~13×/iter). Hoist them into locals; write back only the *final* value the
  6502 oracle leaves in `mem[]` (the harness only compares post-return state, so intermediate
  ZP writes that the next iteration overwrites are dead — skip them). Cache invariants
  (`$00A0-$00A3` etc.) into locals once before the loop.
- **Pointer-walk with autoincrement, never multiply+index in a loop.** Replace
  `M[base + i*stride + Y]` (a 68000 `mulu` + indexed load each step) with a pointer advanced by
  `p += stride` / `p -= stride` (`move (a0)+` / `-(a0)`). Reuse a walked pointer across phases
  where the geometry allows (collision's scan leaves the pointer at row k, so the waterfall
  steps it back down with no fresh multiply).
- **Batch bulk clears/copies with `move.l` through a NON-VOLATILE alias** of `mem[]`
  (`uint8_t* M = (uint8_t*)mem;`). Casting away `volatile` lets the compiler emit 4-byte stores
  and a tight loop. SAFE only for buffers the ISR doesn't touch concurrently — the main loop
  owns the `$1010+` terrain field (verified the flight VBI never writes it); ZP and ISR-shared
  regions must stay `volatile`. `move.l` needs an even/4-aligned address (odd → 68000 address
  fault) — align first (see `zero_run`). ⚠ The win is from **`move.l` batching of SEQUENTIAL
  bytes**, NOT from dropping `volatile` per se. For SCATTERED single-byte access (e.g. the terrain
  rasterizer's per-column PLOT + Y-walked interpolation arrays) a non-volatile alias is a measured
  **no-op** — GCC already keeps the base in a register, so there is nothing to batch. Don't chase
  volatile-vs-non-volatile for scattered access; that whole class of "cheaper mem access" is
  exhausted there — the cost is instruction count / algorithm, not the `volatile` barrier.
  ⚠ **Endianness when aliasing `mem[]` as `uint16_t*`/`uint32_t*`:** `mem[]` is little-endian
  (6502: `mem[a]`=lo). The Amiga 68000 is **big-endian**, so a word/long read through such an
  alias returns the **byte-swapped** value — and worse, the SDL validation host is little-endian,
  so `make validate` passes GREEN while the Amiga silently renders garbage. So do NOT alias for
  general 16/32-bit values; lift them into `uint16_t`/`int16_t` LOCALS and touch `mem[]` byte-wise
  at the boundaries (`mem[a] | (mem[a+1]<<8)`), as the rasterizer/`MIDPOINT` twins do. The ONE
  safe alias case is a **uniform-byte broadcast** store (e.g. fill 4 lanes with the same byte via
  `grp = b*0x01010101u`, walk a `uint32_t*`): all bytes equal ⇒ endianness-neutral (identical on
  host + Amiga). Used for `terrain_draw_frame_core`'s `$BD00` column-id fill (commit ac3a9a8) —
  46 long stores; still needs the 4-aligned + ISR-untouched + non-overflowing-lane conditions.
- **Skip redundant work the original wasted.** Avoid re-decoding/-scanning what hasn't changed
  (per-writer dirty flags; dirty row/cell ranges, cf. planet viewport `g_planetRowLo/Hi` and the
  cockpit plan `docs/cockpit-render-plan.md`). Shadow-compare scans are themselves a full
  volatile scan — a 68000 no-go; prefer dirty flags.
- **When the idiomatic-C twin is still hot, ESCALATE to hand-written m68k asm** (vasm). GCC won't
  emit `(a0)+`, has no scaled index, and spills under the register pressure these loops create — so
  the C floor is GCC's floor, not the algorithm's. In asm you control the regs (pin the working set,
  walk a private stack with `(a3)±3`), force the addressing, and shave every redundant insn
  (`movea` copies, `and.w #$FF` after a `sub.b` into an already-zero-extended reg, `moveq#0;move.b`
  → `move.l` of a clean reg). This beat the C on `terrain_column_rasterize_core` (~27%) where four C
  restructurings had all regressed. Verify with the in-process differential (see the budget section),
  NOT cross-run. See `docs/asm-migration-plan.md` + `TerrainRasterizeAssembler.s`.

**Correctness + measurement:** every rewrite must stay byte-identical — `make validate FN=<name>`
diffs full `mem[]` state vs the transliterated `__t6502` oracle (exit `cpu` regs are usually dead
→ "incidental cpu diffs" are fine; **0 mem mismatch is mandatory**). Measure on real hardware via
the headless beam probe (`make PROBES=1` + `diag_run.sh`); `FP_TIME` subtracts the flight-VBI ISR
beam-lines (`g_isrBeamLines`) so a phase bucket excludes ISR firings in its window. ⚠ Per-phase
flight numbers are **terrain-dependent and noisy run-to-run** (±30%) — trust large deltas, not
small ones, and confirm the real cause by reasoning about access counts.

**Flight VBI ($4FF5) ZP write-set (audited 1872 firings + static cross-check):** the in-flight
VBI writes only `$14 $20 $27-$2E $33 $34 $42 $44 $4B $5D $61 $62 $70 $73 $74 $B9-$BF $CE $D8-$DC
$E6` (attitude/throttle/altitude, HUD/atmosphere colour ramp, object+sprite state). It writes
NONE of the terrain rasterizer/subdivide cells (`$60/$80/$81/$82-$86/$8D-$91/$95/$96/$9F/$B5/$B6/
$EA/$EB/$F4/$F5`) nor `$260E`. So the main-loop terrain renderer's working set is disjoint from
the VBI's — useful when reasoning about ISR-safety of a main-loop optimisation.

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
- Ask the user at genuine decision points (they're an experienced retro-porter and want to
  steer architecture/scope choices).
- Persistent cross-session notes live in the auto-memory at
  `~/.claude/projects/.../memory/` (`MEMORY.md` is its index). Stable facts live *here* in
  CLAUDE.md; memory holds in-progress/learned specifics.
