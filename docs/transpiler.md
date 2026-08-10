# Transpiler internals — generated-code shape

> Split out of `CLAUDE.md` 2026-08-10 to keep the always-loaded file small.
> **Read this when working on `tools/transpile.py`, or when the shape of generated
> `rof_gen.c` code is surprising.** The parts you need for everyday work — the file table, the
> native-twin seam, and which file a twin belongs in — stay in `CLAUDE.md` §Transpiler.

## Named memory accesses (`mem.h`) — full rationale

`symbols.csv` is the single source of truth not just for
function names but for named RAM/shadow addresses. The transpiler builds a `VAR_NAMES` map from
its var rows and emits named accesses instead of raw `mem[$NNNN]`: a direct access becomes a bare
lvalue alias (`level_stage = cpu.X`, via the `ROF_MEM_ALIASES` block in `mem.h`), while
indexed / RMW / `bus_write` forms use `mem[MEM_<name> + i]`. `rof_native.c` is auto-converted to
the same named forms; `rof_native_amiga.cpp` + `RescueOnFractalus.cpp` use `mem[MEM_*]` directly.
(The old `zp::`/`AtariZp.h` namespace was removed — these are general RAM/shadow addresses, not
zero page.)

## Peephole folding (liveness-gated)

The transpiler runs a backward-CFG register/flag liveness
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

## Which file a native twin belongs in — full rationale

`rof_native.c` = FAITHFUL twins (byte-identical to the
`__t6502` oracle, `make validate`d, linked into BOTH the SDL and Amiga backends). `rof_native_amiga.cpp`
= genuinely Amiga-only code (deliberately "lossy" — drops HW-register writes, routes audio to Paula,
frame-driven entry points), NOT validated. **A faithful, pure-`mem[]` 6502 routine that merely needs a
small Amiga variation does NOT go in the .cpp — it stays a validated twin in `rof_native.c` with the
variation guarded by `#ifdef ROF_PLATFORM_AMIGA`** (Amiga-only code, e.g. a dirty-band publish or a
skipped Atari HW tail) or `#ifndef ROF_PLATFORM_AMIGA` (Atari-faithful behavior the validation/SDL
build keeps so the twin still matches its oracle). Amiga-only globals a twin writes go in that twin's TU
(the writer's TU), also under `#ifdef ROF_PLATFORM_AMIGA` (cf. `g_planetRowLo/Hi`, `g_tun*`). Precedent:
the tunnel-ring/door-scroll standby cinematic (`draw_ring_frame_step`, `step_accum_add_75`,
`advance_history_6a4d`, `dl_lms_*`, `scroll_terrain_dl`, …) lives entirely in `rof_native.c` this way.
