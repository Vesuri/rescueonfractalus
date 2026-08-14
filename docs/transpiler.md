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
`advance_history_6a4d`, `dl_lms_*`, `dl_doors_open_split_step`, …) lives entirely in `rof_native.c` this way.

## What `make validate` cannot see — and `make hostproof`, which can

`make validate` proves a twin against its `__t6502` oracle through the `mem[]` contract. Three whole
classes of change therefore have **no** coverage from it, and every one of them has shipped:

| blind spot | why validate can't see it |
|---|---|
| Amiga-only code (`rof_native_amiga.cpp`, `RescueOnFractalus.cpp` — ~516 `mem[]`-touching lines) | there is no 6502 oracle to diff against; that code is deliberately lossy |
| a pure **reordering** or **table-fold** inside a routine whose caller has been shed | the `VALIDATE_FUNCS` entry (and with it the oracle) was dropped when the twin went plain |
| a variation under `#ifdef ROF_PLATFORM_AMIGA` | the validate build compiles the *other* arm |

The proof for all three is the same and takes seconds: **compile the OLD and NEW bodies side by side
on the host and diff them over the whole input domain** (or a large randomized sample). ⚠ Note this is
the same method as the `MEMBASE`/`MEMVIEW`/`FCIBASE` flags, but a different mechanism — those compile
an Amiga-only *transformation* into the real validate harness; a hostproof compiles two *snapshots*
against each other in a standalone program, which is what lets it cover code the harness never links.

### ⚠⚠ The flag guard — why `make validate MEMBASE=1` used to be a vacuous green

`FCIBASE` / `MEMVIEW` / `MEMBASE` / `RELEASE` change only `CFLAGS`, and `%.o: %.c` never depended on
them. So `make validate MEMBASE=1` immediately after a plain `make validate` **relinked the stale
objects and re-ran all 28 fixtures against the code with the fold switched OFF** — a full PASS that
proved nothing. Caught 2026-08-14 by noticing `rof_native.o` was 15 minutes older than the
`validate_native` it had just been linked into. Exactly the class of trap CLAUDE.md already records
for the Amiga Makefile's `make clean` before `PROBES=1`.

Fixed by a **parse-time guard** in the root `Makefile`: a `$(shell …)` that compares the flag set
against `build/.flags` and, on a mismatch, *deletes* the objects before make builds its dependency
graph — so make simply sees them missing. No timestamps, no stamp prerequisite.

⚠ **Two tidier designs were built and both silently failed on 3 of 10 transitions** — Apple ships
**GNU Make 3.81 (2006)**: (a) one stamp file compared by *contents* under a `FORCE` rule (make caches
its stat of the stamp, so rewriting it inside its own recipe does not reliably mark dependents out of
date); (b) the flag set hashed into the stamp's *filename*, tried both as a `%.o` prerequisite and on
the concrete object list (still stale — the failing transitions just moved). **Half a guard is worse
than none**, because it makes a vacuous green look verified. If you touch it, re-run this matrix and
require 10/10:

| from → to | expected |
|---|---|
| (clean) → default | compiles |
| default → default | **unchanged** |
| default → `MEMVIEW=1` → `FCIBASE=1` → `MEMBASE=1` | recompiles each time |
| `MEMBASE=1` → `MEMBASE=1` | **unchanged** |
| `MEMBASE=1` → `MEMVIEW=1` → `RELEASE=1` → default | recompiles each time |
| `RELEASE=1` → `RELEASE=1`, default → default | **unchanged** |

`make hostproof` runs the seven that exist (`FN=<substr>` for one). Adding one is the price of any
change in the table above. ⚠ **A green hostproof means "the transformation is sound", not "the
shipping source still matches" —** each proof holds a verbatim snapshot that drifts silently as the
real routine is edited. Re-read the snapshot against its source before leaning on an old proof.
⚠ `tools/xorshift_triple_test.c` is deliberately excluded: it is a design search, not a proof.
