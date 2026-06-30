# Assembler migration plan

Two goals, in order:
1. **Phase 1** — stop forcing `-DNO_ASSEMBLER` and bring the framework's hand-written m68k
   assembler routines into use (they were shipped with the dA JoRMaS framework but replaced by
   portable C++ bodies early on "to keep things simple / make everything build").
2. **Phase 2** — write an optimized hand-asm `terrain_column_rasterize_core` (the flight terrain
   rasterizer — ~61% of the draw; four C/GCC restructurings all regressed because the 68000 is
   register-starved + has no scaled-index + GCC won't emit `(a0)+`; hand-asm is the only way to
   capture the interleaved-pointer-walk win). See [[flight-scene]] dead-ends.

> **There is NO `NO_ASSEMBLER` constraint.** It was an early simplification, now being removed.
> The shipping build should use hand-written asm for hot paths.

---

## Background facts (verified 2026-06-30)

- `-DNO_ASSEMBLER` is passed in `amiga/Makefile:131` (`CCFLAGS`, → `CPPFLAGS`). It keeps `ASSEMBLER`
  **undefined** in `framework/Util.h:11` (`#if !defined(ASSEMBLER) && !defined(NO_ASSEMBLER)`),
  which selects the `#ifndef ASSEMBLER` C++ bodies in the framework `.cpp`s.
- The framework headers declare the asm entry points under `#if defined(ASSEMBLER) && defined(__SASC)`
  (SAS/C `__asm register(...)` decls), and the `.cpp`s provide the **GCC bridge** under
  `#if defined(ASSEMBLER) && !defined(__SASC)` — inline `__asm volatile("jsr _mangled" : ...)` that
  marshals args into the SAS/C register ABI (e.g. `AmigaHardware.cpp:577+` for `getVBR`/`isLongFrame`/
  `isBlitterBusy`). So flipping the flag activates an *existing* path — but only where a GCC bridge
  wrapper exists.
- The `*Assembler.s` files are **Motorola/vasm syntax** (`xdef`, `equ`, `;` comments, `$` hex,
  register-prefix-optional), NOT GNU `gas`. The project's `m68k-amiga-elf-as` is GNU `gas` (see
  `incbin.s`/`gcc8_a_support.s`). **`vasmm68k_mot` is installed at `~/.local/vasmm68k_mot`** (not yet
  on PATH). The build links ELF (`m68k-amiga-elf-ld` → `out/RoF.elf`) then `Elf2Hunk` → `RoF.exe`, so
  vasm must emit ELF: `vasmm68k_mot -Felf -m68000`.
- Source `.s` available to vendor:
  `amiga-spike/examples/hC74/src/{AmigaHardware,Bitmap,CopperList,Util}Assembler.s`,
  `tmp/attackofthepetsciirobots/PlatformAmigaAssembler.s` (+ check upstream for Palette/Sprite).
  The hC74 example only actually asm's `sqrt` (`UtilAssembler_sqrt.s` via vasm + a `shim.cpp` wrapper)
  and uses C++ bodies for the rest — so "enable everything" is genuinely per-routine work, not a flip.

## Phase 0 — scrub the `NO_ASSEMBLER` references (do as each is no longer load-bearing)

`CLAUDE.md:150` already reworded. Remaining (remove/adjust as the flag goes away — do NOT remove the
`Makefile:131` flag or the `Util.h:11` gate until Phase 1 actually links the asm, or the build breaks):
- `amiga/Makefile:1, 9, 95` (comments) and `:131` (the `-DNO_ASSEMBLER` flag itself — remove in P1).
- `amiga/ARCH.md:34, 43`; `src/platform/amiga/framework/UPSTREAM.md:12, 15`.
- `src/platform/amiga/framework/Util.h:6` (comment); keep the `:11` gate (it auto-enables `ASSEMBLER`
  once the flag is gone — harmless, leave it).
- `docs/amiga-attract-plan.md:298, 356, 387, 403` — historical plan; annotate "superseded by
  asm-migration-plan" rather than rewrite.

## Phase 1 — bring the framework asm into use  ✅ DONE 2026-06-30

Implemented in one pass (the whole framework, not class-by-class, since the `ASSEMBLER`
gate is global): all four `*Assembler.s` (`Util`, `AmigaHardware`, `Bitmap`, `CopperList`)
vendored into `src/platform/amiga/framework/` (dotted-label sanitised — see `UPSTREAM.md`),
vasm rule added (`vasmm68k_mot -m68010 -Felf`), `-DNO_ASSEMBLER` removed, and the
`AmigaHardware` blitter-queue statics aliased to their SAS/C mangled names via
`ROF_SASC_ALIAS` (`AmigaHardware.h`) so the asm `xref`s resolve.  `blitterFillUp` (a
RoF-added blitter with no asm twin) was moved out of the `#ifndef ASSEMBLER` block to the
unconditional masked-compositing section.  The GCC bridges were already present.
**Verified:** builds clean (PROBES + non-PROBES) and the headless auto-flight reaches a
sustained flight scene (`VVBLKI=4ff5`, terrain rendering, no guru) — exercising the
CopperList/Bitmap/AmigaHardware/Util asm across every scene.  Phase 2 (hand-asm rasterizer)
now unblocked — the vasm→ELF→link pipeline is proven.

### Original plan (kept for reference)

Do it **class-by-class**, verifying rendering is unchanged after each (the asm bodies must be
behaviour-identical to the C++ bodies — this is a faithfulness-neutral swap, and the perf win here is
modest: the blitter/Util routines aren't the flight bottleneck). The real value of P1 is **proving the
vasm→ELF→link→GCC-bridge pipeline end-to-end**, which de-risks Phase 2.

1. **env.sh**: add `~/.local` (or wherever `vasmm68k_mot` lives) to PATH.
2. **Makefile**: add a vasm rule, e.g.
   `obj/%.o : %.s ; vasmm68k_mot -Felf -m68000 -quiet -o $@ $<` (resolve include dirs `-I` for any
   `INCLUDE` the .s use; match the SDK paths the example used). Add the vendored `*Assembler.o` to
   `OBJECTS`. **Remove `-DNO_ASSEMBLER`** from `CCFLAGS` (and confirm it's not also in `CPPFLAGS`
   separately) so `Util.h` defines `ASSEMBLER`.
3. **Vendor** the chosen `*Assembler.s` into `src/platform/amiga/framework/` (record provenance in
   `UPSTREAM.md`).
4. **GCC bridge audit**: for every asm routine now selected, confirm a `#if defined(ASSEMBLER) &&
   !defined(__SASC)` wrapper exists in the `.cpp` (pattern: `register T r __asm("dN"); __asm volatile("jsr
   _mangled" : "=r"(r) : inputs : clobbers);`). `AmigaHardware` has several; `Bitmap`/`CopperList`/`Util`
   will likely need wrappers added. Get the exact mangled names from the `.s` `xdef`s (e.g.
   `_sqrt__4UtilFUl`). This is the bulk of the effort.
5. **Order**: start with the smallest/independent — `Util::sqrt`/`ungzip` (the example already proves
   this one) → `AmigaHardware` blitter ops (bridges mostly present) → `Bitmap` → `CopperList`. Build +
   `./run.sh` (visual) + a headless render-diff after each.
6. **Risks**: vasm ELF symbol/section compatibility with `m68k-amiga-elf-ld`; exact SAS/C name mangling;
   the SAS/C register-arg ABI vs the GCC bridge; CHIP-vs-FAST section directives in the `.s`. If a
   class fights back, leave it on the C++ body (the gate is per-symbol via the wrappers) and move on —
   partial enablement is fine.
7. Finish Phase 0 scrub once `-DNO_ASSEMBLER` is gone and the build is green.

## Phase 2 — hand-asm `terrain_column_rasterize_core`  ✅ DONE 2026-06-30 (commit e27a292)

Implemented as `src/platform/amiga/TerrainRasterizeAssembler.s` (vasm), reached via the
`ROF_RASTERIZE_ASM` seam in `rof_native.c` (the clean-C body kept as
`terrain_column_rasterize_core_c`, the validate/SDL oracle).  **Result: byte-identical to
the oracle over ~6700 real-flight calls, ~20% fewer beam-ticks than GCC's C** (asm 94724 vs
C 117956, same-run back-to-back) — the interleaved-pointer-walk win the four C attempts
couldn't get.  Key realisations vs the original plan below:
- The asm mirrors only the **Amiga path** of the oracle: field-plot is a no-op, the plane2
  dot plot is active, and the ONLY live writeback is `$82/$84/$86` (read back by the
  subdivide caller, verified) + `$60`.  `$95/$EA/$F4` (depth>0), `$80/$81`, `$B5` are dead,
  so the control-point stack is a PRIVATE register-walked scratch buffer (a3 ±3) — no mem[].
- `entryDepth` is effectively dead (the C assigns it to `depth` then overwrites with 0 before
  the loops); the asm ignores it.
- **Verification — NOT render-diff.** Cross-run render-diff is unusable: the async 50Hz VBI
  desyncs frames vs the free-running main loop, so a faster rasterizer lands the dump on a
  different sim frame (measured: vbi 2204 vs 2217 at the same `fdCalls`).  Instead an
  **in-process differential** (`make VERIFY=1 PROBES=1` + `amiga/raster_verify.gdb`): the
  `terrain_column_rasterize_core` wrapper runs the asm + the C oracle back-to-back on
  identical inputs each call, byte-compares `$260E` + writeback + the plane2 dot buffer,
  leaves the C output live, and tallies mismatches + same-run beam-ticks for each.

### Original plan (kept for reference)

Independent of P1 (can be a standalone vasm `.s` with plain C linkage — no SAS/C wrapper needed since
it's our own function), but do P1 first per the goal order, and because P1 establishes the vasm rule +
verification harness this reuses.

### ABI / entry
- C signature: `void terrain_column_rasterize_core(uint8_t entryDepth, uint8_t colBase)`. GCC m68k
  passes args **on the stack** (verified in the current disasm: `move.b 49(sp),d4` for colBase). Confirm
  exact offsets/promotion (`4(sp)`/`6(sp)` as int-promoted bytes) from a fresh `objdump` of the C build
  before writing the prologue. Return: none. Keep the 6502-ABI shim `terrain_column_rasterize(void)` in C
  calling the (asm) core.
- Keep the C `_core` compiled as `terrain_column_rasterize_core_c` (the SDL/validate oracle + an Amiga
  build-time toggle/fallback). On Amiga, link the asm as `terrain_column_rasterize_core`.

### Algorithm (the interleaved-pointer-walk the user designed)
The win GCC couldn't realize:
- **One** interleaved control-point stack `[col,hgt,frac]` per level in a scratch buffer; `a0` walks it
  by `±3` (`addq #3,a0` / `subq #3,a0`); current point = `(0,a0)`/`(1,a0)`/`(2,a0)`, push target =
  `(3,a0)`/`(4,a0)`/`(5,a0)`. One base register, cheap displacement/auto-inc addressing, **no index
  register, no scaling** — exactly what the 68000 wants and GCC refused to emit.
- Replicate the C `_core` byte-for-byte: phase-1 left-clip; phase-2 trace (`gap==$FE`/`$FF`/bisect); the
  9-bit fraction accumulate; roughness up/down (saturate $FF / floor 0); `DRAW` = `COL_MAX` ($260E)
  hidden-surface test + clamp-to-$97/$FF + `b5=depth` + the plane2 dot plot (`g_flightDotPlane`, with the
  `_sc=150-oldMax < 43` and `_ac=col-48 < 160` bounds, via `kRow120[]`/`kColMask4[]` — all externs).
- ZP residue ($95/$EA/$F4 stack, $80/$81, $B5): **dead on Amiga** (audited — seeded from level 0 by the
  caller, never read back; the byte-reusers self-init). So the asm need NOT write the canonical stacks
  back at all (no flush). Keep only the WB writes that are observably live, if any — verify against the
  audit. (For the validation harness build, a C oracle is used, so the asm's Amiga-only deadness is fine.)

### Verification (there is no `make validate` for m68k — build the equivalent)
1. **On-target equivalence harness** (the m68k analogue of `make validate`): a small `#ifdef`'d test that,
   for each fixture, seeds `mem[]` + the control-point inputs identically, calls **both**
   `terrain_column_rasterize_core_c` (C oracle) and the asm version, and byte-diffs the touched `mem[]`
   regions ($260E max-height map + the plane2 dot buffer). Run headless under FS-UAE; report pass/fail
   via the gdb stub (like `diag_run.sh`). Reuse representative inputs from `tools/validate_native.c`'s
   real-flight corpus (port a handful, or capture live via a probe). **0 mismatch is the gate.**
2. **Runtime render-diff**: `amiga/dump_terrain.sh` + `tools/decode_terrain.py`, asm-build vs C-build
   terrain bitmap, byte-identical over a headless flight.
3. **Perf**: cycle-count the inner loop by hand (the deterministic signal — the beam probe is too noisy
   for sub-15% deltas, see [[flight-scene]]); confirm the `(a0)+`/`(d8,a0)` addressing + freed registers
   beat the C baseline's indexed `(0,aN,depth)` across 3 bases. Cross-check with `g_tdRaster` only as a
   sanity sign, not a precise number.

### Success criteria
Byte-identical to the C oracle (harness + render-diff) AND measurably fewer inner-loop cycles. If hand
register allocation still can't beat the C baseline (unlikely — we control the regs now), the faithful
rasterizer is genuinely at its floor and the flight target is ~25 fps.

## Verification — what was ACTUALLY built (2026-06-30, supersedes the plan above)
**NOT render-diff.** Cross-run render-diff (plan step 2) is unusable: the async 50Hz VBI and the
free-running main loop interleave differently when the rasterizer's speed changes, so the dump lands
on a different sim frame (measured: vbi 2204 vs 2217 at the same `fdCalls`). Instead an **in-process
differential** (`make VERIFY=1 PROBES=1`, read via `amiga/raster_verify.gdb`): each call runs the asm
twin + the C oracle back-to-back on the SAME live inputs, byte-compares the outputs ($260E + the
writeback + the plane2 dot buffer), leaves the C output LIVE (flight stays correct on an asm bug), and
tallies mismatches + same-run beam-ticks (`g_rasAsmTicks`/`g_rasCTicks`, `g_edge*`). The `.s` carries
a `ifnd ROF_RASTERIZE_VERIFY` second xdef so the C wrapper can own the symbol in verify builds. Same-run
back-to-back timing sidesteps the cross-run noise → perf deltas ARE measurable. `make RASTER_C=1`
builds the C oracle as the live path (fallback / A-B).

## Phase 3 — DONE so far (2026-06-30), ONE target still OPEN
Done this session (all byte-identical, in-process differential 0 mismatch over thousands of calls):
- **`terrain_column_rasterize_core`** (`TerrainRasterizeAssembler.s`): the big one, ~27% faster than
  the C oracle. Then **instruction-shaving** (`movea.l a5,a0` dropped, redundant `and.l`/`and.w`/`moveq`
  removed, `moveq#0;move.b` → `move.l` for already-clean regs) → another ~9%. Commits d2114aa, e27a292,
  8b52207.
- **`renderFlightDirect` plane-1 edge-plot** (`flight_edge_plot_asm`, same `.s`): ~2.8× faster
  (26 vs ~78 beam-ticks/call), via 4-col unroll + immediate masks + `kHeightRowOff[256]` folded
  clamp (the user's hypothetical-renderer asm structure). Commit c6285bb. ⚠ Note: `bmi` CANNOT replace
  the per-column `cmp.b #$FF/beq` off-top skip — real heights reach $96 (~28% of columns have bit7 set,
  verified from a $260E dump); only $FF must skip.

**CURRENT MEASURED BUDGET (per iteration, deep flight, iterCount=130, all asm in):** terrain draw both
passes **~167ms (dominant ~47%)** [rasterize ~64% (asm'd, near instruction floor) · project ~20% ·
subdivide ~16%] · VBI ~71ms (3.6ms × ~20 firings/iter, faithful 50Hz sim+audio, hard to cut) ·
renderFlightDirect ~24ms · setup+clear ~31ms. **Target stays 50fps A500 (25 acceptable).**

**OPEN — next target (user to pick next session):**
1. **`project_terrain_points`/plot (~20% of draw)** — RECOMMENDED (clean numeric fn; multiplies +
   `divide_16x16`; best value/risk; reuse the differential pipeline).
2. `terrain_subdivide_column` (~16% of draw) — the fractal LOD cascade driving the rasterizer; more
   structural complexity.
3. Deeper rasterize restructure (keep the control-point stack TOP in registers, skip store-then-reload
   each bisect step) — biggest potential on the largest chunk, but fights 68000 register pressure (all
   8 d-regs + a2-a6 already live; this is what defeated the C attempts) → high risk.
Also still available: `terrain_frame_setup` (~10ms). Same vasm + in-process-differential pattern.
