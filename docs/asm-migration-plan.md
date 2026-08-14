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

> **⛔⛔ THE FLIGHT-PERF ROSTER IS CLOSED (user decision, 2026-08-14).** Every "still on the table" /
> "priced, not done" item below is a **historical record, not a TODO** — do not start flight-perf
> work and do not re-open a closed candidate on the strength of the unmet 25 FPS target (it stands at
> 24.84 and stays there). Reasoning: `docs/flight-perf-log.md` §2.0 + §25.

> **Companion docs:** `docs/flight-perf-log.md` holds the flight-perf investigation archive — the
> closed candidates with their numbers (§2), the 68000 codegen lessons harvested from these twins
> (§3), and the retracted conclusions (§5). The `flight-pc-profiler` memory is now a **reference**
> (shipping shares + which-harness-answers-what), **not** a ranked TODO; measurement rules in
> `flight-measurement-rules`.

## Current asm roster (moved verbatim from CLAUDE.md 2026-08-10)

Hand-written m68k asm is the norm for hot paths + framework routines (`-DNO_ASSEMBLER` is gone;
`vasmm68k_mot -m68010 -Felf` assembles the `.s`). Done: the framework `*Assembler.s` (GCC bridges);
the flight terrain rasterizer (`TerrainRasterizeAssembler.s`: `terrain_column_rasterize_core` +
`flight_edge_plot_asm`); `project_terrain_points` (`ProjectTerrainAssembler.s`);
`terrain_subdivide_column` (`TerrainSubdivideAssembler.s`); `terrain_frame_setup` loops
(`TerrainFrameSetupAssembler.s`); `fill_terrain` (`FillTerrainAssembler.s`); the SFX
voice-priority mixer chain (`SfxMixerAssembler.s` — the first twin outside the terrain
pipeline; it lives in the 50Hz VBI, so it taxes wall clock regardless of frame rate). Each has an
`ROF_<NAME>_ASM` seam + a `make <NAME>_C=1` C-fallback. Verify asm twins with `make VERIFY=1 PROBES=1`
+ the matching `amiga/*_verify.gdb` (in-process differential vs the C oracle — NOT cross-run
render-diff).

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
  on PATH). The build links ELF (`m68k-amiga-elf-ld` → `out/RoF.elf`) then `Elf2Hunk` → `out/RoF`, so
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
- **`mul_u8` bit-serial multiply → 64KB lookup table (2026-07-01, NOT asm — a C table).** `mul_u8`
  ($9821) is NOT a plain product: it's a round-half-up-per-bit multiply with multiplicand-precision
  truncation (brute-forced: differs from round(M*N/256) on ~1/3 of the 65536-pair domain; exact form =
  Σ round-half-up(M/2^(i+1)) over set bits). So NO single 68000 mulu/muls reproduces it byte-identically —
  the user's "just use mulu" can't be byte-exact. Instead: a lazily-filled 64KB byte table
  `g_mulTable[(M<<8)|N]` (rof_native.c) turns the ~8-iteration loop into one indexed load, byte-EXACT.
  The 3 NATIVE call sites (compute_obj_rel_angle_scale ×2, flight_control_integrate) use `mul_u8_lookup`
  and leave the operands $6B/$28D6 untouched (mul_u8 consumes them, but those consumed values are dead —
  read only by mul_u8, always written before use). `make validate` needed $6B/$28D6 added to the ignore
  mask for those two tests (proven dead, like divide_16x16's scratch). mul_u8 itself is untouched (still
  the oracle + a still-transpiled caller). ⚠ GOTCHA that cost a detour: a "restore the operands" test
  FALSELY passed because BOTH native+oracle callers shared the modified mul_u8 — the real oracle consumes,
  so the operand diff is real and must be masked, not ignored. **Bonus: the muls run in the flight VBI
  (fires ~20×/iter), so flight_control_integrate's `integ` dropped ~13→~8 ticks/firing (measured).** Cost:
  64KB BSS on the A500. (The other bit-serial multiply, signed_mul_8x16, 8× in build_view, would need a
  256×65536=16MB table → left as-is.)
- **`terrain_frame_setup` loops (~10ms)** (`TerrainFrameSetupAssembler.s`, new file, 2026-07-01):
  **~26% faster (per-call 91 vs 123 beam-ticks), byte-identical (0 mismatch / deep flight fdCalls=320);
  make validate green.** A GOOD win (unlike subdiv) — this function has NO rasterizer call, so its
  bracket is its own compute. The two projection calls (setup_projection_params/build_view_transform_matrix)
  stay in the C `terrain_frame_setup` wrapper; the asm twin is just the two transform loops
  (`terrain_frame_setup_core`). Win source: loop 1 touches ~15 Y-indexed arrays spanning ~530 bytes
  ($2276..$24E0); GCC can only reach them with 8-bit displacements so it juggles several base registers +
  `lea disp8(base),aN`+`move (aN)` per access, whereas the asm uses `a0=mem+Y` and a single
  `move.b (d16,a0)` (16-bit disp covers the whole cluster). rot_a/rot_b in a3/a4 (used via `add.w aN,dN`),
  a5=mem+$0900 (height index by X). Seam `ROF_TFSETUP_ASM`; `make TFSETUP_C=1` fallback; tfsetup_verify.gdb.
  ⚠ VERIFY GOTCHA (cost ~1 iteration): the differential's snapshot block must EXCLUDE $2270-$2275 — the
  async flight VBI (`update_terrain_scanline_proj`) writes map/depth scratch there, giving false mismatches;
  tfsetup's real outputs are $2276+. (Same async-VBI-contamination class as the nested subdiv verify.)
- **`terrain_subdivide_column` (~16% of draw)** (`TerrainSubdivideAssembler.s`, new file, 2026-07-01):
  byte-identical (in-process differential, **0 mismatch over 6836 deep-flight calls** to fdCalls=320;
  `make validate` green) but the perf win is **modest — ~0.6 beam-tick/call (asm 153149 vs C 157365, ~2.7%
  end-to-end)**. GCC compiled the C to 1173 insns with 129 frame-slot spills (it can't pin the recursion's
  span/mid/far/depth/budget), and the asm register-pins all of it (span d2-d4, mid d5-d7, depth/budget/
  rasterEntryDepth in a2-a4, `a1=mem+depth` for single-(d16,a1) SubPt-stack access, `jsr` to the asm
  rasterizer for leaf fills). ⚠ **Lesson: the differential bracket includes the raster leaf-fills the
  recursion drives (identical in both asm/C), which DOMINATE the per-call time — so eliminating the
  register spills (subdivide's OWN compute, a small slice) barely moves the total.** Kept because it is
  correct + never-slower + removes the spills, but the ISA-level win here is much smaller than the
  rasterizer/project got. Seam: `ROF_SUBDIV_ASM` in `rof_native.c` (`terrain_subdivide_column_core_c` is
  the oracle). Verify: `make VERIFY=1 NO_RASTER_VERIFY=1 PROBES=1` + `subdiv_verify.gdb` (NO_RASTER_VERIFY
  keeps the rasterizer plain-asm so the two differentials don't nest — a nested double-verify gave false
  mismatches). `make SUBDIV_C=1` falls back to the C.
- **`project_terrain_points` (~20% of draw)** (`ProjectTerrainAssembler.s`, new file, 2026-07-01):
  ~2.2–2.4× faster (per-call 5 vs 11 beam-ticks; 15420 vs 34615 over 3082 deep-flight calls).
  The per-object world→screen projection: two perspective divides (each a single `divu.w` after the
  `<<count` normalization loop — GCC was marshalling args to the `_core`/`divide_16x16_core` calls and
  re-reading `mem[]` volatile per axis) + the band-offset tail, all inlined with `a0 = mem+X` so every
  per-object cell is `(disp16,a0)`. Reached via the `ROF_PROJECT_ASM` seam in `rof_native.c`
  (`project_terrain_points_core_c` kept as the SDL/validate oracle). Byte-identical over 3082 calls
  (in-process differential, 0 mismatch); `make validate` green. `make PROJECT_C=1` falls back to the C.
  Verify: `make VERIFY=1 PROBES=1` + `GDBSCRIPT=project_verify.gdb ./raster_diff.sh`.
- **`build_view_transform_matrix` (setup path)** (`BuildViewAssembler.s`, new file, 2026-07-05):
  ~3× faster (per-call 2 vs 6 beam-ticks), byte-identical (in-process differential, 0 mismatch;
  `make validate` green). **Retires the bit-serial-multiply TODO.**
  KEY FINDING: `signed_mul_8x16` ($9C97) is *named* signed but its product core is a plain UNSIGNED
  8×16 multiply P = m·|mc| (m≤255, |mc|≤$8000, both fit a word) with the multiplicand's sign re-applied
  to the 16-bit result P>>8 = **round toward zero** (not floor). So each product is exactly ONE `mulu.w`
  on the magnitudes; `muls.w`+`asr` would be WRONG — it floors (off-by-one for negatives, e.g. m=1
  mc=-1: 6502→0 but muls+asr→-1) and its low bits are the two's-complement of P, not P, so the $00AC
  frac would differ too. (Contrast mul_u8 $9821, a round-half-up multiply that needed g_mulTable.) The
  two multiplicands' magnitudes+signs are formed ONCE (the only byte-wise work — mem is little-endian
  so a word read would byte-swap) and held in registers; each of the four products is `move.w;mulu.w;
  lsr.l #8;` + a conditional `neg.w`. NO `bsr`, NO `divs` — 4 `mulu.w` total. The asm reproduces the
  last (mul4) call's ZP side effects ($00A8-$00AD) + the two output pairs ($22A3:$22D1, $22FF:$232D).
  The step-2 subtract's "no-borrow" carry is threaded into mul4's frac ($00AC) via `scc`.
  Reached via the `ROF_BUILDVIEW_ASM` seam in rof_native.c (`build_view_transform_matrix_core_c` kept
  as the SDL/validate oracle); `make BUILDVIEW_C=1` falls back to the C. Verify: `make VERIFY=1 PROBES=1`
  + `GDBSCRIPT=buildview_verify.gdb ./raster_diff.sh`. (`signed_mul_8x16` native/oracle left untouched —
  still used by the C oracle path + the still-transpiled `build_view_transform_matrix__t6502`.)

**CURRENT MEASURED BUDGET (per iteration, deep flight, iterCount=130, all asm in):** terrain draw both
passes **~167ms (dominant ~47%)** [rasterize ~64% (asm'd, near instruction floor) · project ~20% ·
subdivide ~16%] · VBI ~71ms (3.6ms × ~20 firings/iter, faithful 50Hz sim+audio, hard to cut) ·
renderFlightDirect ~24ms · setup+clear ~31ms. **Target stays 50fps A500 (25 acceptable).**

**⚠️ STALE — the list below is DONE (corrected 2026-07-20).** All three "next targets" plus
`terrain_frame_setup` are hand-asm'd and ON by default (`ROF_PROJECT_ASM`, `ROF_SUBDIV_ASM`,
`ROF_RASTERIZE_ASM`, `ROF_BUILDVIEW_ASM`, `ROF_FRAMEDRAW_ASM` — see `amiga/Makefile`). The
faithful CPU terrain math is at the 68000 instruction floor. **Piecemeal faithful asm is
EXHAUSTED as a lever.** Do NOT pick from this list — it is kept only as the historical trail.

**~~OPEN — next target (user to pick next session):~~** (all DONE — see above)
1. ~~`project_terrain_points`/plot~~ — DONE, `ProjectTerrainAssembler.s` (~2.2×).
2. ~~`terrain_subdivide_column`~~ — DONE, `TerrainSubdivideAssembler.s`.
3. ~~Deeper rasterize restructure~~ — `TerrainRasterizeAssembler.s` (~27%, near instruction floor).
   ~~Also `terrain_frame_setup`~~ — DONE, `TerrainFrameSetupAssembler.s`.

## Phase 4 — the rasterizer's PHASE-2 RESTRUCTURE ✅ DONE 2026-08-05 (−36% beam-ticks)

This is the "structural, not micro-opt" step the [[flight-pc-profiler]] bottleneck map kept
pointing at (`ph2_loop` was the single richest bucket in the whole flight frame and per-insn
shaves there had gone thin). **Result: `terrain_column_rasterize_core` 24 → 15 beam-ticks/call
(−36%)** measured by the in-process differential with the C oracle as the stable reference
(oracle read 35 vs 34 ticks/call across the two runs); **0 mismatch over 1827/2007 real-flight
calls**; `make validate FN=terrain_column_rasterize` green (4000 cases, 0 mem mismatch). The
PC profile share went **34.5% → 29.8%** of the flight frame.

### Measure first — the shape data that sized it
Added a permanent shape probe (`make RASTER_C=1 RAS_SHAPE=1 PROBES=1` + `amiga/ras_shape.gdb`;
off by default because its volatile counters live in the C oracle and would wreck the
differential's perf reading). Deep flight, 263 half-frames / 6013 calls:

| quantity | value | what it says |
|---|---|---|
| draws / call | 25.5 (153150 total) | one DRAW per terrain column |
| accept rate | **36.8%** (56300 plots) | the hidden-surface REJECT path is the common one |
| far-bisects | 84842 | ≈ 0.55 per column |
| fe / ff leaves | 63172 / 26806 | fe:ff ≈ 2.4:1 |
| phase 1 | 2309 advances + 1356 pushes | **0.6 iterations per call — cold, ignore it** |
| far-bisect span | **3 → 31.2%, 4 → 16.5%** | half of all bisects are the two smallest cases |
| entry span | mostly 17–64 | segments are wide; the tree is deep enough to matter |

The span-3/4 dominance is just the shape of a binary subdivision tree (most internal nodes are
penultimate), and it is the whole reason step 5 below pays.

### The five changes (all in `TerrainRasterizeAssembler.s`)
1. **No control-point COLUMNS at all** — replaced by a tracked `span = cp[depth].col - plotCol`
   in `d2`. Kills, per far iteration: the `cp[depth].col` load, the `gap = plotCol - ccol`
   subtract, the `mid = (plotCol+ccol)>>1` computation AND its store. Identities:
   `mid = plotCol + (span>>1)`, `child = span>>1`, `parent-after-child = span - (span>>1)`,
   `disp_up = (span>>1)>>1`, `disp_down = ((span>>1)-1)>>1`, `gap==$FF ⇔ span==1`,
   `gap==$FE ⇔ span==2`. Rests on `floor((a+b)/2) = a + floor((b-a)/2)` and on a finished
   subtree leaving plotCol exactly AT its control point's column. Phase 1 tracks the same span.
2. **TOS control-point HEIGHT in a register** (`d6`) — the pushed midpoint height is simply left
   there (never stored), the parent's is spilled on the way down, leaves read it free.
3. **`r = hsum&1` folded into `ceil`**: `havg + (hsum&1) == (hsum+1)>>1` — one register and two
   instructions fewer, and it freed the register budget for 1 and 2.
4. **`plotCol >= $D4` and `col = plotCol` moved OUT of the loop head** into the leaf handlers —
   the only places plotCol changes, so the far path's re-test was always redundant.
5. **Spans 3 and 4 are straight-line blocks** (`ras_sp3` / `ras_sp4`): no push, no pop, no
   dispatch, no stack traffic, DRAW inlined — the whole 3- or 4-column leaf group in one block.
   They absorb 47.7% of the far-bisects, 98.6% of the ff leaves and 86% of the fe leaves;
   loop-top dispatches drop **174820 → 93986** for the same picture.
   (DRAW is inlined at all 11 sites — the old `bsr`/`rts` pair was 34 cycles per column.)

Net on the hot far path: **230 → 140 cycles** (hand-counted from the objdump). Code size is
unchanged (~1560 bytes) despite the 11 inline DRAWs.

### Verification recipe (reusable — this is the strong part)
1. **Host equivalence FIRST** — `tools/ras_restructure_test.c` runs a verbatim copy of the C
   oracle's Amiga path and the restructured algorithm on identical inputs and diffs `$260E`, the
   whole plane-2 dot buffer, the `$82/$84/$86` writeback and the draw/plot counts:
   **1.6M randomised cases, 0 mismatches**, over four input regimes (realistic, phase-1-heavy,
   right-edge-heavy, fully adversarial bytes). `cc -O2 -o /tmp/rast tools/ras_restructure_test.c`.
   Doing the algebra proof on the host, before writing a line of m68k, is what made a ~400-line
   asm rewrite tractable — every identity in step 1 above was validated there first.
2. **Then on-target**: `make clean && make -j4 VERIFY=1 PROBES=1` +
   `GDBSCRIPT=raster_verify.gdb ./raster_diff.sh <label> 300`.
3. **A/B the two asm versions against the SAME C oracle** (`git show HEAD:...s` into place,
   rebuild, re-run). The oracle's ticks/call is the stable reference; raw `effFPS` and the PC
   profile share are both too noisy to size a change this size.

### Evaluated and NOT done (with the numbers, so nobody re-derives them)
- **Whole-subtree occlusion culling. MEASURED 2026-08-05 (commit 24346f9) — verdict NO; see
  Phase 5 below for the numbers.** The idea was sound and P turned out HIGH (58% of span-3 and
  56% of span-4 groups are fully hidden), but the saving is only ~2% of the rasterizer because a
  rejected DRAWDOT is already 32 cycles and the test has to re-read the same `COL_MAX` byte.
  Both older sizings on this line were wrong: the "~44% break-even" ignored that 98% of misses
  bail on the FIRST compare (so a miss is cheap), and the "saving = ~136" over-counted what a
  fully-rejected group actually costs. Do not re-derive — re-run the committed probe instead.
- **Incremental dot column** (mask in a register rotated `ror.b #2` + the plane byte pointer in
  `a1`, advanced on the mask's wrap) instead of the per-draw `cmp #208/sub #48/and #3/lsr`
  arithmetic: ~86 cycles saved per ACCEPTED draw (36.8% of them) for ~20 per column ⇒ only
  ~5% of the rasterizer, and the `plotCol` range gate needs either a padded dot buffer or a
  per-column compare that eats the win. Not worth the risk yet.
  ⚠ **2026-08-05: that ~5% is on the WRONG DENOMINATOR — see Phase 5 below. An accepted draw is
  not a plotted dot; only 12% of draws ever reach this code, so the real ceiling here is ~2%.**
- **Running `COL_MAX` pointer** for the consecutive columns inside `ras_sp3`/`ras_sp4`: the
  `lea` + `addq.l #1,a1` per column costs more than the 6 cycles/access it saves. Measured
  negative on paper; don't.
- **`$97` saturation via a 256-byte store-value table** (`move.b (aX,d0.w),(a2,d5.w)` replacing
  `move.b`+`cmp`+`bcs`): ~10 cycles per accepted draw ⇒ ~1.5%. Marginal.

## Phase 5 — dot-plot tables + subdivide's dead round-trips (2026-08-05)

Two changes, both byte-identical, plus the measurement that should steer the NEXT one.

### `terrain_column_rasterize_core` — DRAWDOT column tables (~0.9%, commit 8bf4a48)
`_ac = plotCol-48`, the `(unsigned)_ac < 160` gate, `_ac>>2` and `$C0 >> (2*(_ac&3))` are all
pure functions of plotCol ⇒ two byte tables indexed by the RAW column (`kDotColMask` /
`kDotColOff`, RescueOnFractalus.cpp). `kDotColMask == 0` outside `[48,208)` is impossible for a
real 2-bit mask, so the mask fetch IS the range gate. 13 instructions (~104 cycles) → 6 (~62).
The second table base came from **a4: the control-point stack base is just SP** (nothing touches
the stack between `lea -CPBUF(sp),sp` and `done` now that DRAW is inlined), so the depth==0 test
is `cmpa.l sp,a3`.
Proof: `tools/dot_table_test.c` diffs (offset, mask) against the `ROF_PLOT_DOT` oracle over the
ENTIRE domain — all 65536 `(plotCol, oldMax)` byte pairs, 0 mismatches. On target 0 mismatch /
2698 calls; `make validate FN=terrain_column_rasterize` green.

### ⭐ The measurement that matters: **an accepted draw is NOT a plotted dot**
New permanent shape counter `g_rasDots` (`RSDOT`, ROF_RAS_SHAPE). Deep flight, 489 half-frames:

| quantity | value |
|---|---|
| draws | 286590 (24.7/call) |
| accepted (COL_MAX updated) | 112124 = **39%** |
| **dots actually written** | 34450 = **12% of draws, 30% of accepted** |

Why: the dot is plotted at the column's **PREVIOUS** top, and the per-frame `$6B` reset floor puts
that at `_sc == 43` — the single scanline the gate excludes. So a column's FIRST accepted draw
never writes; only later ones do. Consequences, both load-bearing:
- The table change saves 42 cycles × 0.12 ≈ 5 of a ~269-cycle draw ⇒ the measured 0.9%, exactly.
- **DRAWDOT is only ~23% of the rasterizer** (~61 of 269 cycles/draw at the measured 61/27/12
  reject / accept-no-dot / dot mix). The other ~77% is TREE TRAVERSAL. Any further per-plot
  micro-opt is capped at ~2%; the lever is the traversal, or not emitting the draw at all.
- **Whole-subtree occlusion culling** looked like the best-sized candidate here (61% of draws
  rejected, ras_sp3+ras_sp4 absorb 47.7% of far-bisects). It was measured next, and closed —
  see below.

### `P(all-3-rejected)`: occlusion culling measured and CLOSED (commit 24346f9)

New permanent counters `g_rasSp3Grp/Occl/Edge/Cons[]`, `g_rasSp4*` (`ras_occl_probe`, ROF_RAS_SHAPE,
hooked into the C oracle's far-bisect branch — the oracle has no literal `ras_sp3` block, a
"span-3 group" there is a far-bisect whose span is 3). Build `make clean && make -j4 PROBES=1
PROFILE_NORING=1 RASTER_C=1 SUBDIV_C=1 RAS_SHAPE=1`, run `GDBSCRIPT=ras_shape.gdb ./diag_run.sh 150`.
411 half-frames of real flight:

| | groups | all-rejected (exact) | cheap `max()` test | misses bailing on the 1st compare | edge-truncated |
|---|---|---|---|---|---|
| span 3 | 40576 | 23755 = **58%** | 23148 = 57% | 17053/17428 = **98%** | 267 = 0.7% |
| span 4 | 21201 | 11884 = **56%** | 11083 = 52% | 9851/10118 = **97%** | 288 = 1.4% |

**P is high and misses are cheap — and it still does not pay.** Skipping a fully-hidden group
saves only ~172 (sp3) / ~232 (sp4) cycles, because a rejected DRAWDOT is already just 32 cycles
(`moveq` + indexed byte load + `cmp` + taken branch). The cull test must load the SAME `COL_MAX`
byte and compare it — 22 cycles a column — costing ~132 / ~174 back, plus a `#$D3`/`#$D2` guard
to replace the group's mid-block `$D4` right-edge exit (the edge-truncated groups above are not
cullable as written). Net ~13 / ~22 cycles a group ≈ **100 cycles per rasterize call against
~4200 = ~2% of the rasterizer, ~0.6% of the frame.** A FREE perfect oracle would reach ~17% of
the rasterizer, so **the test eats ~85% of its own ceiling.**

**Generalisable lesson:** the reject path is already at the floor — any "check before drawing"
scheme re-reads the very byte the check was meant to avoid, and can only recover the ~10 cycles
of bookkeeping around that load. The sub-variants die the same way: *cull only span-4* is the
better half but ~1%, and a *coarse min-summary array* (one load covering 4 columns) needs a 4-min
recomputed on every accepted draw (~70 cycles × 9.6/call) = a large net loss.
**Not measured, if ever revisited:** culling at a LARGER span, where one hit skips a whole subtree.
Blocker: for span ≥ 5 the roughness displacement can lift midpoints above both endpoints, so
`max(ends)` is unsound — it needs `+ ~S/2` slack (far less likely to fire) *and* a min over S
columns (S loads = as costly as the draws). That is a redesign, not a micro-opt.

### `terrain_subdivide_column_core` — dead mem[] round-trips (commit 17622b6)
- The `mid` ($8D-$91) **entry load is a pure round-trip**: every read of `mid` is preceded by an
  assignment from `subdiv_midpoint`, so the oracle loads it only so `out:` can write the same
  bytes back. Dropped; the flush is now conditional — and **the budget is the dirty flag for
  free** ($14 after the entry guard, decremented once immediately before each of the two `bsr
  submid` sites ⇒ `budget != $14` ⇔ a midpoint ran).
- `sd_doras` flushed the span HIGH bytes **$83/$85**, which the rasterizer never reads (it takes
  $82/$84/$86 + $95/$EA/$F4 and writes back only $82/$84/$86) and which `sd_out` rewrites anyway.
Measured A/B against the same in-run C oracle, both runs fdCalls=150 / ~5120 calls: the asm's
margin over the oracle goes **7.4% → 12.5%** (ratio 0.9261 → 0.8755, ~1.2 beam-ticks/call).
0 mismatch / 5117 calls (all 5 SubPt stacks + the ZP residue incl. $8D-$91 and $83/$85 + the
return value); `make validate FN=terrain_subdivide` green.
⚠ Read the ratio, not the raw per-call figure: the differential's bracket includes the rasterizer
leaf-fills the recursion drives, identical in both arms and dominant.

**ACTUAL open lever (2026-07-20, user-directed):** the remaining waste is mem[]→bitplane
*conversion* on the hot path, NOT the terrain math. All hot-path graphics should write bitplanes
directly. Prime suspect = **object drawing** (ground objects / downed pilot / enemy fire): the
`terrain_plot_object` → `terrain_plot_pixel` path still does the full faithful mode-D field RMW
(`bus_read`/`bus_write` via `($80),Y` indirect — slowest access) for every object pixel, then
*additionally* mirrors to the bitplane (`ROF_PLOT_DOT`); on Amiga the field write is dead weight
for terrain-body rows (only band rows 43-46 are read back). And `draw_scaled_shape` (rescue-figure
zoom) writes a bitmap into the field via `plot_clipped_pixel`. **Measure first** with the current
all-asm build (`make PROBES=1` + `diag_run.sh`) before committing scope.

---

## Phase 6 — the SFX voice-priority mixer ✅ DONE 2026-08-06 (commit a2f331f)

**First asm twin outside the terrain pipeline.** `SfxMixerAssembler.s` replaces
`sfx_reorder_voice_slot` ($5614) and inlines all three leaves — `sfx_voice_write_freq_ctrl`
($5673), `sfx_pick_top_voice` ($568A), `sfx_pick_next_voice` ($56AF) — into one routine.
Seam: `ROF_SFXMIX_ASM`, fallback `make SFXMIX_C=1` (which `SFX_SHAPE=1` forces, because the
shape probe's `SX_LEAF` brackets live in the C bodies the asm replaces).

**Why a non-rendering routine was worth asm'ing.** It sits in the 50 Hz flight VBI, which fires
50×/s *regardless of frame rate* — so unlike the terrain pipeline it is a flat tax on all wall
clock (~31% of it), not something a slow frame dilutes. In combat: ISR ~96 t/firing, sfx block
43 t, event-ring drain 27.7 t, and this chain alone 22.4 t/firing (3.32 calls × 6.70 t).

### Measure first — the leaf split that resized the job (commit e6e296a)
The plan had sized this at "30-40% of 21.8 t/firing = 2-3% of all wall clock". Floor-corrected
`SX_SPAN` brackets on each leaf said otherwise, per 6.70 t call:

| part | t/call | share | asm-addressable? |
|---|---|---|---|
| the two `rof_pokey_write` calls (C++ change-detect + `update_paula_channel`) | 2.95 | 44% | **no** |
| `write_freq_ctrl`'s own mem[] loads + call plumbing | 1.05 | 16% | yes |
| `sfx_pick_next_voice` (12-slot scan) | 1.04 | 16% | yes |
| `sfx_pick_top_voice` (12-slot scan) | 0.90 | 13% | yes |
| reorder's own compare/move logic | 0.85 | 13% | yes |

Only 57% was reachable ⇒ ceiling ~1.5%, not 2-3%. **Method:** `g_sxNop`/`g_sxNopT` is an EMPTY
bracket sampled once per call = the bracket's own floor (0.29 t); subtract one floor per leaf
call. The five added brackets took the call's own reading 6.70 → 10.83 t, so the split is only
usable as *shares*.

### ⚠⚠ v1 was 5% SLOWER than the C — the most transferable lesson in this document
GCC fully inlines **and fully unrolls** both 12-slot scans into straight-line
absolute-addressed code:

```
move.b (mem+$0706).l,d1   ; 16 cyc
beq.s  next               ; 10 cyc      => 26 cycles for an inactive slot
```

The textbook pointer-walked loop v1 used is *worse*:

```
tst.b  (a0)+   ;  8
beq.s  .nx     ; 10
addq.l #1,a1   ;  8
dbra   d2,.lp  ; 10        => 36 cycles for an inactive slot
```

Ten cycles per slot × 12 slots × 1.14 scans/call. **On the 68000 the loop bookkeeping (18 cyc)
costs more than autoincrement saves over absolute addressing (8 cyc).** This is the boundary of
CLAUDE.md's "pointer-walk with autoincrement, never multiply+index in a loop" rule: that rule
kills a `mulu`+index, it does **not** beat an unrolled absolute scan. v1 also imposed a
10-register `movem` (~180 cycles of prologue+epilogue) against GCC's 3 (~68) — a handicap on its
own larger than everything the twin saved.

⇒ **New standing step: disassemble what GCC actually emitted before designing the asm.** If it
already inlined and unrolled, the headroom is small and the design must beat *straight-line*
code, not a loop.

### What v2 keeps (the parts where hand-asm genuinely wins)
- **Unrolled** scans (a `PT_SLOT`/`PN_SLOT` macro × 12), but with `tst.b (a0)+` for the per-slot
  active test — 18 cyc/slot vs GCC's 26. Autoincrement wins *once the loop is gone*.
- A **5-register** `movem` (`d2/d5/a2-a4`). `cpu.Y` needs no register at all: nothing in the
  chain writes it, so the epilogue just copies `cpu.Y` into `cpu.A`.
- The four table bases pinned in address registers, chosen so one base reaches several columns
  with an 8-bit displacement: `a2 = mem+$0705` also gives `$0714-$0717` at `$0F-$12(a2)`;
  `a4 = mem+$066B` also gives freq at `$0E(a4)` and distortion at `-$0E(a4)`. Every mem[]/cpu
  touch is then a 12-cycle `d(An)` instead of a 16-cycle absolute.
- The slot index is a compile-time constant per unrolled slot, so the `$0715`/`$0717` store
  needs no counter register, and `pick_next`'s exclusion test is an 8-cycle `cmpi.b #slot,d2`.

### Faithfulness traps specific to this twin
- **`bus_write`'s hardware-range test must be reproduced.** The POKEY writes are
  `bus_write($D1FE+x)` / `bus_write($D1FF+x)` with `x = mem[$0705+y]` an arbitrary byte, and
  bus.h routes only $D200-$D20F to `rof_pokey_write`; anything else in $D000-$D7FF is dropped
  **without touching mem[]**. So AUDF lands only for x ∈ [2,17], AUDC only for x ∈ [1,16] —
  hence a `cmp.w #16 / bcc` guard on each, not an unconditional call.
- **The entry `cpu.X` is NOT an input.** `5619 TXA; 561a BNE 5641` tests X as the JSR at 5616
  *left* it (= `mem[$0705+Y]`), so the branch is "is this slot ACTIVE?". The C twin always read
  it in the right place but its doc comment called cpu.X "a selector"; building the asm from
  that comment produced a first-call mismatch. Comment corrected in e6e296a.
- `cpu.A` is computed even when the AUDC write is dropped; each scan's `$0716` init happens even
  when no slot wins (while `$0714/$0715/$0717` keep their old values).

### Verification — and the two harness traps it exposed
`make VERIFY=1 PROBES=1 COMBAT=1 FIXED_RNG=1` + **`amiga/sfxmix_verify.gdb`**: 0 mismatch over
8443 on-target calls, comparing `mem[$0705-$0717]`, the POKEY mirror `mem[$D200-$D20F]` and
cpu.A/X/Y. `make validate FN=sfx` stays green on all 11 sfx fixtures.

- ⚠ **The differential must ALTERNATE the two implementations' order**, unlike every other twin
  here, because the callee is **not pure**: `rof_pokey_write` → `update_paula_channel` sits
  behind several global caches, so whoever runs first warms them and the second side gets its
  Paula work free. Always-asm-first read the asm as **24-34% slower**, and rewinding `pokey[]`
  alone did not fix it. Alternating per call puts the bias on both sides equally so it cancels
  in the ratio. **Generalise: check the callee for caches before trusting a fixed-order A/B.**
- ⚠ **`$a0`/`$a1` are m68k register names in gdb.** `set $a0 = g_fooTicks` writes the target's
  address register — it corrupted the run past the first breakpoint and produced a *plausible*
  "21% faster" reading that was pure garbage. Name convenience variables `$sa0`/`$sc0`/….

### Result
Per-call **asm/C = 0.77-0.80** over two runs (the shared ~2.2 t `FP_TIME` floor is in both, so
the true body speedup is larger). End-to-end via the new **`amiga/isr_ab.gdb`**: flight VBI
**95.52 → 89.78 t/firing**, activity-normalised for the two runs' unequal fight (ring/firing
3.57 vs 3.74) ⇒ **−4.7 t/firing = −5% of the ISR ≈ 1.5% of ALL wall clock**. `integ` (16.72 vs
16.81) and `proj` (11.22 vs 11.10) confirm the runs were otherwise comparable.

`isr_ab.gdb` is a **legitimate cross-build A/B**, unlike iter/frame throughput: the metric is
ticks *per firing* and the ISR runs 50×/s regardless of how far the main loop got. It still
depends on a comparable audio/combat workload, which is what its ACTIVITY line is for.

### NEXT in this area — the sibling the leaf split exposed (NOT started)
The **POKEY→Paula write path is 44% of the mixer call ≈ 3.2% of all wall clock and nothing has
been tried on it.** `update_paula_channel` only records into `want_*`; `flush_paula` programs
Paula once per frame from `game_vbi_isr` — so every recompute except the last one per channel
per frame is waste, and `rof_pokey_write` could just set a per-channel dirty bit. It is also
strictly *more* correct: `sfx_voice_write_freq_ctrl` writes AUDF then AUDC for the same channel,
so a changed pair recomputes twice and the first pass runs against the stale AUDC — a transient
`update_paula_channel`'s own comment describes as a hazard it works around.
⚠ **Size it first:** 6383 upc calls / 2798 firings = 2.28/firing over only 4 channels, so the
redundancy may be ~0.5-1 call/firing (≈0.4-0.8% of wall), not the full 3.2%. And it is not
`mem[]`-observable, so `make validate` cannot check it — verify by ear + `ROF_BEEP_CAP`.

---

## Phase 7 — the rasterizer's SPAN-5..8 FUSION ✅ DONE 2026-08-08 (−8.0% of the rasterizer)

The first re-shape-probe of `terrain_column_rasterize_core` since the Phase-4 restructure, run on
the **quiet** (target) baseline this time: `make clean && make -j4 PROBES=1 PROFILE_NORING=1
RASTER_C=1 SUBDIV_C=1 RAS_SHAPE=1 COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1` +
`GDBSCRIPT=ras_shape.gdb ./diag_run.sh 150`. 481 half-frames / 9102 calls / 216780 draws.

### What the probe said that the PC profile could not
The far-bisect span histogram is nearly unchanged from 2026-08-05 (span 3 = 31.2%, span 4 = 16.6%),
but the interesting column is the one nobody had read: **spans 5-8 are 12404/9147/6494/4855 =
27.5% of all far-bisects, which is 53% of the ones `ras_sp3`/`ras_sp4` do NOT already absorb.**
Reconstructing what the asm's loop top actually dispatches on (total 132581 = 14.6/call):

| span | 3 | >=9 | 4 | 2 | 5 | 6 | 7 | 8 | 1 |
|---|---|---|---|---|---|---|---|---|---|
| share | 28% | 22% | 15% | 9% | 9% | 7% | 5% | 4% | 0.5% |

**A node of span 5..8 bisects into a child of span `S>>1` = 2/3/3/4 and a parent remainder of
`S-(S>>1)` = 3/3/4/4 — so BOTH halves are already straight-line leaf blocks.** Everything the
generic path does to get between them is therefore dead weight: the push's two spills, the
`addq.l #3,a3`/`subq.l #3,a3`, the whole `RASPOP`, the child's `cmpa.l sp,a3` underflow test
(which at depth+1 can never fire), its exit `bra`, and **two** loop-top dispatches.

### The change (4 new blocks, `ras_s5`/`s6`/`s7`/`s8`)
Each block computes the bisect midpoint, inlines the child leaf, restores two registers and
falls into `ras_sp3` / `ras_sp4` for its parent half. State handling vs the generic push:
- the child's control height (mh1) lives in **d6**, exactly as a pushed one would;
- the node's OWN control height goes to **d2** (span is dead once dispatched) rather than
  `1(a3)`, and comes back with a register move — `ras_s8`'s child had to be restructured to
  free d2 (it parks its own midpoint in d1 and sets `height = mh2` *before* the first DRAW,
  which is safe because DRAWDOT never reads d3);
- **fsum1** still goes to `5(a3)` for c = 3 and 4 (those child bodies overwrite d4 with their
  own fsum); for c = 2 the `fe` child never touches d4, so it simply stays there;
- a3 never moves, so `2(a3)` is still this node's cfrac when the parent half reads it, and the
  child's cfrac needs **no reload** — it is `and.w #$FF,d1` off the fsum1 already in a register.
- the roughness displacement becomes an IMMEDIATE (`disp_up = c>>1`, `disp_down = (c-1)>>1`),
  and the oracle's `t = ceil + ~disp; mh = (t > $FF) ? t&$FF : 0` is exactly `ceil - (disp+1)`
  floored at 0 — an 8-bit borrow spelled as a 9-bit add, so `subq`/`bcc`/`moveq` covers it.

Dispatch: `cmp.b #4,d2 / bls.s ph2_small` is untouched (span 3, the most common case, keeps its
cheapest path); the new `cmp.b #8,d2 / bls.s ph2_mid` sits between it and `ph2_far`, so only the
span>=9 path pays for the new test (+16 cycles on 3.2 nodes/call).

### Verification
1. **Host, exhaustive** — `tools/ras_fused_midpoint_test.c` enumerates every reachable
   `(hsum 0..510, fsum 0..511)` against the oracle's generic far-branch midpoint for each child
   span, **2 093 056 cases, 0 mismatches**, and includes `ras_sp3`/`ras_sp4`'s already-shipping
   constants as controls (a failure there would indict the model, not the new blocks).
   `cc -O2 -o /tmp/rasfused tools/ras_fused_midpoint_test.c && /tmp/rasfused`.
2. **On target** — `make clean && make -j4 VERIFY=1 PROBES=1 FIXED_RNG=1 COMBAT=1
   COMBAT_QUIET=1` + `GDBSCRIPT=raster_verify.gdb ./raster_diff.sh fused 300`:
   **0 mismatch / 1611 calls** (≈5800 fused-block executions) on `$260E` + the `$82/$84/$86`
   writeback + the whole plane-2 dot buffer.
3. **The shipping FAST path** (the subdivide→rasterizer private register ABI, which plain
   `VERIFY=1` bypasses) separately under `VERIFY=1 NO_RASTER_VERIFY=1` + `subdiv_verify.gdb`.

### Result
A/B against the SAME C oracle, near-identical windows (1619 vs 1611 calls, vbi 14558 vs 14557):

| | asm t/call | oracle t/call | ratio |
|---|---|---|---|
| pre-fusion | 14.97 | 32.73 | 0.4574 |
| **fused** | **13.70** | 32.55 | **0.4210** |

**−8.0% of the rasterizer.** The oracle arm moved −0.55% — its own noise floor — which is what
makes the −8% signal rather than drift. At the rasterizer's measured 23.6% of wall that is
**~1.9% of all wall clock**. End-to-end (`FPSCOUNT=1`, `fps_seg.gdb`, all 15 segments valid) read
**19.49 FPS against the standing 18.41**, i.e. +5.9% — consistent in direction and bigger than the
static prediction, but a cross-build end-to-end delta is confounded (`FIXED_RNG` pins the level,
not the trajectory), so **the defensible number is the differential's −8%.**

### Evaluated, NOT done
- **Fall-through layout.** Each block ends `bra ras_sp3`/`bra ras_sp4` (10 cycles). Ordering the
  blocks so the two biggest consumers fall through instead is worth ~21 cycles/call ≈ 0.3% of the
  rasterizer, at the cost of a much harder-to-review file order.
- **Biasing d5 so the right-edge test is free.** `cmp.b #$D4,d5 / bcc done` runs ~15×/call = ~300
  cycles. Keeping `plotCol - $D4` in d5 (negative in range) makes the test a bare `bpl` after the
  existing `addq`, at the price of rebasing a1/a2/a4 by +$D4 and un-biasing `col` once at `done`
  — net ~76 cycles/call ≈ 1.2% of the rasterizer, but it touches EVERY DRAWDOT and every leaf.

---

## Phase 7b — `project_terrain_points`: three dead things (2026-08-08)

Not a restructure, just three items visible in the disassembly of the existing twin:
- **The depth divisor `{$232E:$2300}[X]` is shared by both axes and written by neither**, yet
  `PAXIS` re-assembled it from its two bytes each time (`moveq` + 2 × `move.b (d16,a0)` +
  `lsl.w #8` = 50 cycles). Loaded once into d3; each axis takes a `move.w d3,d2` copy for its
  scaling loop to shift — 2×50 becomes 50 + 2×4. **−42.**
- **`and.l #$FFFF,d0` after each DIVU was dead.** Every later use of d0 is a `.w`/`.b` op
  (`lsl.w d5,d0`, `move.w d0,d1`, `add.w d0,d1`, `lsr.w #8,d0`), so the remainder left in the
  high word can never reach memory. **−28.**
- **d7 held X only to `adda.l d7,a0`** (d0 does that) and d4 was never used, so the entry
  `movem` drops 6 longs → 4. **−32.**
- Plus: every branch in the file was assembling as a WORD branch (the build passes vasm
  `-no-opt`), and all of them are in `.s` range. A not-taken word `Bcc` is 12 cycles against a
  short one's 8, and the scaling loop's `bcs .fits` is not-taken once per doubling. **~−35.**

**~140 cycles/call.** `project_verify.gdb`: **0 mismatch**, twice. ⚠ Its *magnitude* reading
(per-call asm 6.59 → 4.22 beam-ticks, reproduced on two runs) is **not trustworthy at this
size** — a project call is only ~4-10 scanlines against an ISR of ~80, so `FP_TIME`'s
credited-at-exit `g_isrBeamLines` subtraction distorts a large minority of samples, and the
distortion's sign depends on the phase relationship, which shifts when the bracket gets shorter.
Size this one from the disassembly, not from the differential.

---

## Phase 8 — `terrain_subdivide_column`: half the call was MARSHALLING (2026-08-08, 962fd79)

The #2 flight bucket (PC profile 8.3% + 2.8% for the obj entry, ISR-corrected ⇒ ~11% of wall)
and the one hot twin nobody had revisited since its Phase-3 asm was written. **Measure the
shape first** — `amiga/ras_shape.gdb` already prints it, on the quiet baseline (16342 calls /
239 iterations, i.e. **68.4 calls per flight iteration**):

| per call | value |
|---|---|
| inner-loop iterations | **1.21** |
| rasterize calls | **0.55** (45% of calls only ever *skip*) |
| midpoints (`submid`+`push_mid`) | 0.40 |
| pops | 0.11 · phase-2 adopts 0.19 · entry-guard bails 1.1% |
| `far.col > $FF` escapes | 2900 / 19794 inner iterations = **14.6%** |

Hand-counted off the disassembly, a call is **~1400 cycles** and **~50% of it is prologue,
span load, entry guard, flush and epilogue** — for a body that averages one rasterize call.
That framing, not the profile, is what found the seven items:

1. **`rasterEntryDepth` is dead** under `ROF_RASTER_SPAN_ABI`. It exists only to be forwarded
   as the rasterizer's first C argument, and the register-ABI entry does not take it. `a4`, its
   load, and its long in the movem at **both** ends. **−36.**
2. **`sd_phase3`'s two exits are one test.** `span.col > $FF` and `span.col >= $D8` (unsigned)
   are the same predicate: any value with a non-zero high byte is `>= $0100 > $D8`. Holds for a
   negative `span.col` off the pop path too (`>= $8000`). **−24 × 1.11.**
3. **`load_far` inlined and split on the high byte.** `far.col > $FF` ⇔ `hi != 0`, so the
   escape is decided *before* the low byte and the 22-cycle `lsl.w #8`; the 85% that continue
   then have `far.col` = the low byte alone. Kills the `bsr`/`rts` (34) as a bonus.
   **−76 × 1.21.**
4. **`btst #15,Dn` → `tst.w Dn`** at all eight sign tests (10 cycles → 4), `btst #7,d7` →
   `tst.b d7` in `submid`. **−6 each.**
5. **The cascade's branches were all WORD** (`-no-opt` never sizes them) and all in `.s` range.
   A not-taken word `Bcc` is 12 against a short one's 8, and the cascade runs 3-6 per inner
   iteration. **With 3, the inner loop's common path goes 268 → 168 cycles.**
6. **`sd_out`'s span flush is two WORD stores.** `mem[]` is little-endian, the 68000
   big-endian, so `{lo,hi}` is one store of the byte-swapped register — 42 cycles against 58.
   `mem` is now `aligned(4)` (cpu.c) so the precondition is stated, not inherited from GCC.
7. **Two `bra`s to the very next instruction** (`spanlow`→`wtSpanH`, `wtFarH`→`dosub`), 10
   cycles each. vasm found them for free: `bra.s` with displacement 0 is illegal, so marking
   them short is what surfaced that they were no-ops.

**~245 cycles/call ≈ 17.5% of the twin ≈ 1.9% of all wall clock.** End-to-end (`FPSCOUNT=1
FIXED_RNG=1 COMBAT=1 COMBAT_QUIET=1`, `fps_seg.gdb`, vbi 1901→4900, all 15 segments valid):
**1203 painted / 3000 vbi = 20.05 FPS** against the standing 19.49, per-segment 16.4–22.1
against 16.0–21.7.

### ⚠ The differential's magnitude was junk here — and said the opposite
`subdiv_verify.gdb` (`VERIFY=1 NO_RASTER_VERIFY=1`, the shipping fast path): **0 mismatch /
5175 calls** ✓. Its PERF column read **asm 11.91 → 12.20 t/call (+2.4%)** — a *regression* —
while its own C control arm drifted **+1.0%**. A subdivide bracket is ~12 beam-ticks against an
ISR of ~80, exactly the regime Phase 7b documented: `FP_TIME` credits `g_isrBeamLines` at ISR
*exit*, so a large minority of samples straddle an ISR, and the bias flips sign as the bracket
shortens. **Same lesson, opposite sign, and worth restating: at this bracket size the
differential is a CORRECTNESS instrument only.** The disassembly diff settled it — every
changed line is a removal or a strictly cheaper equivalent, so no reading of "+2.4%" can be
about the code.

### The bug it DID catch (and why the baseline run mattered)
Making the two prologue `movem`s conditional without the epilogue popped 9 longs after pushing
8; the game died on the first flight frame (`fdCalls=1`, `vbi` frozen at 2139). A 120 s and a
300 s run returning *identical* numbers is the tell that a machine has stopped, not that it is
slow. Confirming it against a **stashed baseline build** is what separated "my change" from
"this harness config never worked" in one run.

### Measured, not taken (⛔ CLOSED with the roster 2026-08-14 — a record, not a queue)
- **`$95`/`$F4`/`$EA` are a third mem[] handoff between these same two asm twins.** `sd_doras`
  writes them (24+24+16) purely for `terrain_column_rasterize_span`'s prologue to read back
  (~52). Passing `a1` and letting the rasterizer read the SubPt slot itself is worth
  **~104 cycles per rasterize call ≈ 57/subdivide call ≈ 0.55% of wall** — but `$95` is
  `blit_color_src`, a polysemous ZP cell with non-flight readers, and `subdiv_verify` does not
  compare those three, so the deadness would have to be *proved* rather than checked. Priced,
  not done.
- **The entry guard's `$B5` write** (28 cycles/call ≈ 0.26%) is dead on the Amiga by the same
  argument the rasterizer already uses for `$B5`/`$95`/`$EA`/`$F4` — but `$B5` **is** in
  `subdiv_verify`'s compare set, so removing it blinds the instrument. Not worth it.

---

## Phase 9 — the rasterizer's loop-top DISPATCH and its exit branches (2026-08-08)

Two independent items in `TerrainRasterizeAssembler.s`, both pure control flow — no algebra, no
state change, so the whole risk is register liveness and branch range.

### 9.1 The loop-top compare chain → a 256-entry PC-relative jump table

Phase 7 changed the dispatch MIX without repricing the dispatch. Post-fusion it is span >= 9
**44%** · spans 5-8 **49.5%** · span 4 5.5% · span 1 1% · spans 2-3 ~0, over **~7.3 dispatches a
call** (derived from the `ras_shape.gdb` far-bisect histogram: the fused blocks absorb 5.86 of the
6.27 span-3/4 nodes a call, so almost no span 3 reaches the loop top any more). That is essentially
a binary choice with a long tail — and the chain priced it backwards, because **every branch in it
assembles as a WORD branch under `-no-opt`, and a not-taken word `Bcc` is 12 cycles against a short
one's 8**:

| span | >= 9 | 5 | 6 | 7 | 8 | 4 | 3 | 2 | 1 |
|---|---|---|---|---|---|---|---|---|---|
| chain cycles | 34 | 50 | 62 | 82 | 94 | 48 | 36 | 68 | 70 |

Weighted: **49.5 cycles a dispatch.** The table is a flat **36** for every span —
`move.w d2,d0` + two `add.w` (12) + `jmp ras_jt(pc,d0.w)` (14) + the slot's `bra.w` (10) — and
needs no compare at all. **−13.5 × 7.3 = −99 cycles a call**, and it is now MIX-INDEPENDENT, which
matters because a span-9..16 fusion would move the mix again.

- 256 entries (1024 bytes in `section code`), so no range guard is needed; slots 9..255 all hold
  `bra.w ph2_far`, so even a corrupted index lands on real code rather than mid-instruction.
- The table must sit immediately after the `jmp`: `jmp (d8,PC,Dn.W)` has only an **8-bit
  displacement** (the index is added afterwards and is unbounded). `ph2_far` therefore moved below
  the table. Same constraint `TerrainFrameSetupAssembler.s`'s `tf_jt` already documents.
- `RASPOP` gained a `moveq #0,d2` before its `move.b (a3),d2` (+4 × 3.23 pops = **+13**): the index
  is taken from d2's low word, so bits 8-15 must be 0. Every writer of d2 in the file already
  leaves them clear, but that was an unstated invariant and the table makes it load-bearing.

⚠ **The bug this cost a run, and it is the generalisable one: a dispatch must not modify the value
it dispatches on.** The first cut scaled in place (`add.w d2,d2` twice, 4 cycles cheaper) on the
strength of the file's own note that "span is dead once dispatched". That is true of the LEAF
blocks — but `ph2_far` reads the span twice (`move.w d2,d0`, `sub.b d0,d2`) immediately on entry.
The game died on its first flight frame (`fdCalls=1 rasterCalls=3` after 300 s — the
different-length-runs tell from Phase 8 again). Scaling into **d0** costs 4 cycles and is safe:
all nine targets write d0 before reading it.

### 9.2 The `done` exits were word branches — 13 of the 17 are now short

`cmp.b #$D4,d5 / bcc done` (the right-edge bound) and `cmpa.l sp,a3 / beq done` (the depth-0 exit)
run **~18 times a call** and are essentially never taken — exactly one exit fires per call. Under
`-no-opt` every one of them was a word `Bcc` at 12 cycles not-taken. One 4-byte `bra done`
trampoline per block (`rdn_ff`/`rdn_fe`/`rdn_sp3`/`rdn_sp4`/`rdn_s5`/`rdn_s6`/`rdn_s7`) brings 13
of the 17 into short range: **−4 × 14.2 not-taken = −57**, `+10` for the one taken exit now going
via a trampoline = **−47 cycles a call**. vasm ENFORCES the ±127 range, so a block that later grows
past it fails to assemble rather than going quietly wrong. The four remaining sites are the
mid-block bound tests in `ras_sp4`/`ras_s6`/`ras_s7`/`ras_s8`, each ~140 bytes from the nearest
legal stub point (a stub may only sit after an unconditional branch); worth ~9 cycles a call, left
alone.

### Result

**−133 cycles/call ≈ −3.1% of the rasterizer ≈ −0.69% of all wall clock**, counted off the
disassembly against the measured shape.

| verification | result |
|---|---|
| `raster_verify.gdb` (`VERIFY=1 PROBES=1`, the C-ABI entry) — `$260E` + the `$82/$84/$86` writeback + the whole plane-2 dot buffer | **0 mismatch / 1616 calls** |
| `subdiv_verify.gdb` (`VERIFY=1 NO_RASTER_VERIFY=1`, the shipping private-register fast path) | **0 mismatch / 5122 calls** |
| A/B against the SAME C oracle, comparable windows (1616 vs Phase 7's 1611 calls) | asm **13.40** t/call, oracle 33.01, ratio **0.4210 → 0.4060 = −3.6%**; the oracle arm drifted +1.4%, its noise floor |
| end-to-end (`FPSCOUNT=1 FIXED_RNG=1 COMBAT=1 COMBAT_QUIET=1`, `fps_seg.gdb`, vbi 1901→4900, all 15 segments valid) | 1211 / 2997 = **20.20 FPS** against the standing 20.05, per-segment 16.5–22.3 |

⚠ **The framerate cannot resolve this and is not the claim.** +0.75% end-to-end against a harness
whose single-window noise is ±2% is agreement, not evidence; the defensible numbers are the static
−133 cycles/call and the differential's −3.6% ratio (2.5× its own oracle-arm drift). Quote those.

---

## Phase 10 — the rasterizer's two per-column taxes (2026-08-08, 08ef7c3 + 7604664)

Neither is a restructure. Both are things the hottest inner code paid on EVERY column for a
case that is in the minority, found by pricing the two sequences that run most often in the
function rather than by looking at the profile.

**First, the audit that came up empty**, so nobody repeats it. Phase 8's "what fraction of
this twin is MARSHALLING?" was the standing lead, aimed at `project_terrain_points`,
`terrain_frame_setup` and `terrain_plot_object`. Counted off the sources, all three are dead:

| twin | calls/it | marshalling | why it cannot pay |
|---|---|---|---|
| `project_terrain_points` | 48 | ~136 of ~1440 cyc = **9%** | 84 of the 136 is the `movem` for d2/d3/d5/d6, which the two PAXIS expansions genuinely need |
| `terrain_frame_setup` | **2** | ~196 cyc of ~22000 = **0.9%** | a 45-cell loop amortises an 11-long `movem` to nothing — non-starter by construction |
| `terrain_plot_object` | 48 | — | the whole bucket is ~2.8% of wall and the quiet hot path is already a two-load early-out |

Phase 8's ~50% was a property of *subdivide* (68.4 calls/iteration for a body averaging 1.21
inner iterations), not a general law. **The question to ask is not "is there marshalling?" but
"is the call count high AND the body small?"** — only subdivide had both.

### 10.1 DRAWDOT: read COL_MAX into d7, not d1 (08ef7c3)

The macro is inlined at 23 sites and runs 23.8 times a call, so its first three instructions
are the most-executed sequence in the file:

```
moveq   #0,d1                4
move.b  (a2,d5.w),d1        14      oldMax = COL_MAX(plotCol)
cmp.b   d1,d0                4
```

The `moveq` exists only for the **accepted** path, where oldMax becomes a word index into
`kDrawDotRowOff` — and only 36% of draws are accepted. It cannot simply be dropped while the
value lands in d1, because DRAWDOT itself leaves the row byte-offset (up to 5640) in d1, so
the second DRAW of a leaf block would compare against a dirty high byte.

**d7 can hold it.** Every writer of d7 in this file is either a `move.b` inside DRAWDOT or
phase 1's `move.l d1,d7`, whose value is a column `<= $AA` (col < `$2C` plus child `<= $7F`),
so bits 8-15 are already clear. The accepted path pays 4 back with `move.w d7,d1` before the
doubling — `add.w d7,d7` in place would put oldMax's bit 7 into d7's high byte and break the
invariant for the *next* draw.

**-4 on every draw, +4 on the 8.6 accepted = ~-61 cycles/call.** The invariant is now
load-bearing; the one path that reaches a draw before phase 1 has written d7 (the
`endCol == col` one-column early-out) clears it explicitly.

### 10.2 Bias plotCol by -$D4 so the right-edge test is a bare `bpl` (7604664)

`plotCol >= $D4` is tested ~14 times a call and fires exactly **once**. Each site was
`cmp.b #$D4,d5` + `bcc` = 16 cycles not-taken. Biased, "still on screen" means "negative",
which the column step has already computed.

- **It cannot be a signed BYTE.** In `[$2C,$D4)` the biased column is -168..-1. So d5 is
  stepped `addq.w #1,d5` and tested as a WORD, and the three plotCol-indexed bases
  (`mem+$260E`, `kDotColMask`, `kDotColOff`) are rebased by `+$D4` — the `.w` index
  sign-extends, so the negative index lands back on the real entry. a5/a6 are indexed by the
  height, not the column, and are untouched.
- **Nine sites** take the branch straight off the step's own N flag: **-8 each**.
- **Four cannot.** They sit after `RASPOP`, and the oracle's loop genuinely pops —
  `frac = CTL_FRAC(depth+1)` included — *before* testing the bound (`rof_native.c`: the test
  is at the loop top and `col = plotCol` follows it). An early exit there must still carry the
  popped frac, so the pop cannot move: they get `tst.w d5`, **-4 each**.
- Three of the fused blocks move the test ahead of their `move.l d2,d6` / `move.b 5(a3),d4`
  restores and grow a stub that redoes the frac restore on the way out. d6 is callee-saved and
  not an output, so **only frac has to be redone**.
- **Two exit tails.** Leaf blocks store the biased d5 into a0, so `done` adds `$D4` back —
  into a CLEAN d0, because the subdivide fast path merges the result with `or.w d0,d2` and
  `move.w d5,a0` is a sign-extending MOVEA.W. ⚠ The C-ABI shim only keeps the low byte, so
  `raster_verify` alone would NOT have caught a dirty high byte here — `subdiv_verify` is the
  instrument that covers it. The paths exiting before phase 2 biases anything (the two
  trivial-segment early-outs, the one-column case, ph2_enter's own bound test) leave a plain
  column in a0 and take `done_raw`.

**~-110 cycles/call** weighted by the far-bisect histogram, against +12 to un-bias col once.

### Result

| verification | result |
|---|---|
| `raster_verify.gdb` (`VERIFY=1 PROBES=1`, C-ABI entry) | 10.1: **0 / 1751** · 10.2: **0 / 1753** |
| `subdiv_verify.gdb` (`VERIFY=1 NO_RASTER_VERIFY=1`, the shipping fast path) | 10.1: **0 / 5110** · 10.2: **0 / 5117** |
| A/B vs the SAME C oracle, comparable windows (1751-1762 calls, vbi 14294-14347) | ratio **0.4127 -> 0.3934 -> 0.3845**, i.e. -4.7% then -2.3% = **-6.8%**, against oracle-arm drifts of +1.9% and +1.1% |
| end-to-end (`FPSCOUNT=1 FIXED_RNG=1 COMBAT=1 COMBAT_QUIET=1`, `fps_seg.gdb`, vbi 1902->4902, all 15 segments valid) | 1237 / 3000 = **20.62 FPS** against the standing 20.20; per-segment 16.6-23.0 vs 16.5-22.3 |

**The claim is the differential's -6.8% of the rasterizer** (~1.5% of wall at its ~21.5%
share); the static counts say ~170 cycles of a ~4200-cycle call, i.e. ~4%. +2.1% end-to-end
against a harness with ±2% single-window noise is agreement, not evidence — and note this
harness has over-read every win in the table's history.

### Evaluated, NOT done
- **Fall-through layout for the fused blocks** (Phase 7's leftover, ~21 cyc/call) is now worth
  LESS than it was: after 10.2 the block tail is `bpl.s rdn_s5` / `move.l d2,d6` / `bra
  ras_sp3`, so making ras_sp3 fall through means moving `rdn_s5` out of short range and
  turning the `bpl` into a word branch — +4 not-taken eats 4 of the 10 saved. ~8-20 cyc/call
  = 0.2-0.5% of the rasterizer, for a file order in which the blocks no longer read 3,4,5,6,7,8.
  Not worth it in the file where two crashes this week came from layout/liveness slips.
- **DRAWDOT's `$97` saturation** (`cmp.b #$97,d0` + `bcs` = 18 cycles on every accept = ~154
  a call) is the biggest remaining per-draw item. A 256-byte `sat[h]` table would make it one
  `move.b (aX,d0.w),(a2,d5.w)` — but **there is no free address register** (a0=col, a1/a2/a4/
  a5/a6 = tables, a3 = the control-point stack). The branchless `scs`/`not`/`or` form prices at
  40 cycles against the common path's 36. Closed unless a register frees up.
- **Sentinel-slot elimination of the `cmpa.l sp,a3` depth-0 test** (14 cycles × ~4 pops a call):
  seed a slot below the base whose span dispatches to `done` and the test disappears. **Wrong:**
  the oracle at depth 0 returns WITHOUT the pop, so it keeps the running frac, while RASPOP
  would overwrite d4 with the depth-0 slot's `mem[$F4]`. Observable — frac is an output.

---

## Phase 11 — the span-9..16 fusion: PRICED, one third of it built, family CLOSED (2026-08-08, 4d25815)

The standing #1 item was "fuse the rasterizer's spans 9..16 — the b71a405 trick one more level
up", filed at ~1.9% of wall. The user's instruction was to **price the two candidate designs
before writing either**. That was the right call: the filed number was 2.5× optimistic, the
cheap design was 1.5× under-counted, and the whole family turned out to be worth ~1% of wall.

### The anchor: price it as a fraction of the change it copies

Spans 9..16 are structurally the SAME change as Phase 7, one level up — the child (span 4..8)
and the parent (span 5..8) are both already blocks, and the scaffolding between them is the same
instruction set. So count that scaffolding once and convert through a change that was *measured*,
which avoids the absolute call cost entirely (the doc's own "~4200 cycles/call" and Phase 5's
"269 cyc/draw ⇒ ~6400" disagree by 1.5×).

Per span-9..16 node, excluding its own entry dispatch and the fsum/midpoint arithmetic (which
every design keeps):

| item | cyc |
|---|---|
| child-span arithmetic (`move.w`/`lsr`/`sub.b`/`move.l`) + variable disp shift | 28 |
| the three spills: post-child span `(a3)` · chgt `1(a3)` · fsum `5(a3)` | 32 |
| `addq.l #3,a3` + `bra ph2_loop` | 18 |
| child loop-top dispatch | 36 |
| child tail `cmpa.l sp,a3` + `beq.s` (can never fire at depth+1) | 14 |
| RASPOP | 44 |
| child tail `tst.w d5` + `bpl.s` + `bra ph2_loop` | 22 |
| parent loop-top dispatch | 36 |
| **total** | **230** |

The same count on a span-5..8 node gives **182 removed**, and that change measured **−8.0% of
the rasterizer at 3.61 nodes/call**. Spans 9-16 are **1.94 nodes/call** (17507 of the far-bisect
histogram). Everything below is that ratio.

### The four designs

| design | cyc/node | cyc/call | % of rasterizer | % of wall | lines |
|---|---|---|---|---|---|
| **(A)** as filed — freeze a3, cfrac in a register | 160 | 310 | 3.8% | 0.76% | ~950 |
| **(A′)** full inline, a3 still advances | 144 | 279 | 3.4% | 0.68% | ~950 |
| **(B)** `bsr`/`rts` | 110 | 213 | 2.6% | 0.52% | ~500 |
| **(C)** child-dispatch stubs only ← **BUILT** | 52-94 | 116 | 1.4% | 0.28% | ~100 |

Corrections to what was filed, all of which changed the decision:

1. **(A) is ~160 cyc/node, not ~196**, and its ~1.9% of wall was sized on node count with no
   empirical anchor.
2. **(B) is ~110 cyc/node, not ~74.** The sketch missed that the stub knows `c` and `p` as
   *constants*, so the span arithmetic, the post-child-span spill and the variable disp shift
   (32 cycles) go too. (B) is **~76% of (A′)'s win for ~53% of the code**, not 38%/75% — the
   premise that made (B) look pointless was arithmetic, not judgement.
3. **The d7 blocker was void.** The second control-height stash does not need a register: it
   belongs at `1(a3)`, which the generic push already writes and RASPOP already reads, so it is
   free. Phase 10.1 was never at risk. *(Generalisable: "no free register" is only a blocker if
   the value has to be in a register — check for a slot the existing code already maintains.)*
4. **(A) as filed has an aliasing hazard (A′) does not.** Freezing a3 forces every `2(a3)` in the
   duplicated chain to `5(a3)` — but for c=5..8 the child *itself* parks its fsum1 at `5(a3)`,
   which is then the outer node's slot. Keeping the a3 advance costs 16 cyc/node and makes the
   duplicates textually identical to the originals except their tail.
5. **(A′) needs 8 duplicated chains, not 6.** S=10 and S=11 share c=5 but differ in parent, and a
   fall-through tail cannot branch to a variable parent. That is the whole reason (B) exists.

### Why the family is closed

Each level can only be fused once the level below it is, for fewer nodes and roughly double the
code: 9-16 = 1.94/call, 17-32 = 0.97, 33-64 = 0.30, 65+ = 0.02. **Fusing everything above span 8,
to infinity, is 3.23 nodes/call × 144 = 465 cyc/call = 5.7% of the rasterizer ≈ 1.1% of wall.**

### What was built — (C), the part that needs no duplication

`ras_f9`..`ras_f16` + the `FARFUSE` macro: `ph2_far` with `c`/`p`/disp as immediates, branching
straight into the child block. Every existing block untouched, a3 unchanged, no register or stack
change; the child's own tail still pops and dispatches generically, so the parent half is reached
exactly as before. The one instruction that got dearer is the constant span store
(`move.b #p,(a3)` 12 against `move.b d2,(a3)` 8) — 12 is the floor either way, since
`moveq`+`move.b` is also 12.

| verification | result |
|---|---|
| `raster_verify` (C-ABI entry) | **0 mismatch / 1611 calls** |
| `subdiv_verify` (shipping private-register fast path — covers the returned `col`'s width) | **0 / 5172** |
| end-to-end `fps_seg.gdb`, 15/15 valid | 1235/2998 = **20.60** vs the standing 20.62 — unchanged, as predicted |

### ⚠⚠ The differential's PERF column is SIGN-WRONG at this scale — now demonstrated, not inferred

Phase 7b and Phase 8 both said "at this bracket size the differential is a correctness instrument
only". This session **calibrated it**, which is worth more than either assertion:

| build | asm t/call | oracle t/call | ratio | truth |
|---|---|---|---|---|
| baseline | 12.205 | 32.049 | 0.38084 | — |
| + the stubs | 12.586 | 32.335 | 0.38900 (+2.1%) | **−116 cyc/call** |
| + the stubs + 2 `nop`s in DRAWDOT | 12.403 | 32.073 | 0.38672 (−0.6%) | **+190 cyc/call** |

**A known +190 cycles/call reads as a 0.6% speed-up.** There is no usable gain and the sign is
not reliable. A ~12-tick bracket against an ~80-tick ISR is the regime where `FP_TIME`'s
credited-at-ISR-exit `g_isrBeamLines` subtraction dominates. **Below roughly 500 cycles/call,
size the change off the disassembly and say so; do not report the PERF column at all.**

A second, independent trap found on the way, and it applies to every future build-vs-build A/B:
**`raster_verify.gdb`'s window is wall-clock, not work.** Its `g_fdCalls >= 150` breakpoint is
never reached in a VERIFY build (the comment in the script says so), so `raster_diff.sh`'s SIGINT
defines the window — and a faster build lands DEEPER into the flight, on different terrain. The
asm arm has a special-cased block per span and the oracle does not, so **the asm/C ratio is a
function of the span mix**: 30 vbi of drift moved it ~2%. Pinning `fdCalls` does not fix it
either — the sim advances with the VBI, not with painted frames (1598 vs 1522 calls at the same
fdCalls). **`amiga/raster_verifyV.gdb` pins `g_vbiCount`**, which gave 1598 calls in both arms.

---

## Phase 8b — subdivide's helper inlining + the far.hgt high-byte split (2026-08-11, c2a90e5)

The two items Phase 8 sized and left, both inside `subdiv_verify`'s compare set. Full write-up with
the shape tables and the measurement: `docs/flight-perf-log.md` §18. What belongs here is the three
design records:

- **`submid` / `push_mid` / `load_span` are vasm MACROS now** (`SUBMID` / `PUSHMID` / `LOADSPAN`),
  expanded at their call sites — one source copy each, so the arithmetic still has one home. Local
  labels inside them MUST carry `\@`; the same idiom is in `BuildViewAssembler.s` and
  `ProjectTerrainAssembler.s`. 49.5 expansions/iteration × 34 cycles of `bsr`+`rts`.
- **`far.hgt` is classified on its high byte**, like `far.col` at the same site. The win is not the
  shift alone — the cascade's sign and `> $FF` tests collapse too, because `hi == 0` decides both.
  The assembled 16-bit value has ONE consumer (`sd_wtFarH`) reached on 0.8% of leaves, so it lives
  in a cold out-of-line block (`sd_fhWide` / `sd_fhNeg`) placed BEFORE `sd_inner` — dropping it
  after the leaf code pushed `sd_doras` out of five branches' `.s` range.
- **Block order is now load-bearing in this file.** `sd_dosub` sits past `sd_ret` because inlining
  made it ~140 bytes; the far.col escape reaches it via the `sd_dosubT` trampoline. ⭐ **Price a
  branch widening by its NOT-TAKEN frequency** (`Bcc.w` not taken is 12 vs `Bcc.s`'s 8): 10 extra
  cycles on the 12.1 escapes beat 4 extra on all 83.6 inner iterations, 121 cyc/it against 286.
  Anyone editing this file should expect a vasm "branch destination out of range" as the normal
  feedback signal for a size change, not as a mistake.

Also folded in: the two width-test variants (`sd_wtSpanH` / `sd_wtFarH`) differed in ONE instruction,
so `useSpanHeight` is an extra entry point above a shared body rather than a duplicated block.

## Phase 12 — `flight_control_integrate`: PRICED, the addressing half SHIPPED without asm (2026-08-12)

`flight_control_integrate` ($8E5B) is the last sized item in the flight VBI and the only one the
`flight-pc-profiler` menu still names as an asm-twin candidate ("8.4 t/firing — an asm twin is the
only lever, 400 faithful lines"). This phase is the mandatory pricing step in front of that twin —
the same discipline `SfxMixerAssembler.s`'s header preaches, and for the same reason: **that twin's
first attempt came out 5% SLOWER than GCC.**

### 12.1 What the disassembly actually said

At `-O2` the function is **4678 bytes / 1245 instructions**, and:

| | count |
|---|---|
| instructions with an absolute-LONG `<mem+…>` operand | **321** |
| ...with an absolute-LONG `<cpu+…>` operand | 41 |
| instructions using the `a2`/`a3` base registers | **19** |

GCC emits `lea mem,a2` and `lea cpu,a3` in the prologue **and then addresses almost everything
absolutely anyway.** On the 68000 `move.b (xxx).L,Dn` is 16 cycles / 6 bytes against
`move.b d16(An),Dn`'s 12 / 4 — so ~290 sites were paying 4 cycles and 2 bytes for nothing. That is
the single biggest thing a hand-asm twin of this function would have fixed, and it is also the one
thing that needs no asm at all.

⚠ This is NOT the `volatile` tax ([[feedback-volatile-codegen-tax]]) — `mem[]` is already
non-volatile in the Amiga C core. GCC simply prices `abs.l` and `d16(An)` alike in its m68k cost
model and picks whichever the RTL happened to produce.

### 12.2 The fix that shipped — launder the base through an empty asm

```c
uint8_t* mbase;
__asm__ ("" : "=a"(mbase) : "0"((uint8_t*)mem));
#define mem mbase        /* …body…  #undef mem at the end */
```

No instruction is emitted. GCC just loses the knowledge that `mbase == mem`, so it must keep the
value in a callee-saved address register for the whole body, and every `mem[…]` — including every
`symbols.csv` lvalue alias, since `pitch_pos_lo` → `mem[0x25]` → `mbase[0x25]` on macro rescan —
becomes `d16(An)`.

Sound because `&mem` escapes into the asm, so GCC's alias oracle must treat `*mbase` as possibly
aliasing `mem` (the standard `RELOC_HIDE`/`OPTIMIZER_HIDE_VAR` idiom). That matters: the helpers
GCC inlines into this function (`ring_push_unmarked`, `refresh_hud_field_0d_entry`, …) still reference
`mem` directly, and a compiler that thought the two could not alias would be free to reorder
across them.

⛔ Not the closed `register uint8_t* m asm("a5")` global-register idea — this base is
function-local and callee-saved, so the hand-written `.s` files and the VBI are untouched.

`make FCI_NOBASE=1` restores absolute addressing for the A/B.

**Measured** (`amiga/fire_once.gdb` PRE window, `COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1 PROBES=1`,
t/firing — the one flight metric that is cross-build-legitimate):

| | no base | base fold | delta |
|---|---|---|---|
| `integ` | 8.532 | **8.045** | **−5.7%** |
| whole flight VBI | 59.949 | **59.072** | −0.88 t/firing = **−1.5%** |
| function size | 4678 B | **4102 B** | −576 B (288 operands folded) |

−0.88 t/firing × 50 firings/s ÷ 15650 = **−0.28% of ALL wall clock**, for three lines and no
faithfulness surface. The handler row moves −0.66 and `integ` accounts for −0.49 of it, which is
the internal consistency check (`integ` is inside the handler).

### 12.3 What is LEFT for an actual `.s`, and why it was not written

After the fold, `integ` is 8.05 t/firing = 16.1 t/CALL (it runs on half the firings, `$00C8`
parity) ≈ 2.6% of all wall clock. An asm twin has to beat GCC on what remains, and the remainder is
harder than it looks:

- **~55% of the call is callees**, which the twin would `jsr` exactly as GCC does — no win there.
  The ones actually reached in quiet flight are `rof_pokey_random` (×1-2),
  `compute_obj_rel_angle_scale`, `object_integrate_position`. `object_step_and_collide` /
  `load_velocity_from_param_block` are gated on `$0036 != 0`, which is **2 of 216 flight vblanks**
  in the quiet arm — i.e. essentially never, and NOT a cost to design around.
- ⚠⚠ **GCC INLINES several helpers into this function** (`ring_push_unmarked` ×5, `store_676_init`,
  `refresh_hud_field_0d_entry`, `reset_flags_ff`, `mul_u8_lookup`, `terrain_jitter_column`).
  A twin that `bsr`s them instead LOSES to GCC on call overhead, so it has to re-implement each in
  asm too. `ring_push_unmarked` alone is 152 bytes × 5 sites. **This, not the 400 lines of arithmetic,
  is what makes the twin big** — and it is invisible from the C.
- The genuinely asm-shaped wins left are the register allocation (GCC spends 7 registers on a
  `movem` and still reloads) and the 6502 16-bit carry idioms (`lsl.l #8 / or.b / ror.w #8` to
  assemble a pair of bytes; `moveq #0,dN / not.b / and.l / sub.l` for a masked subtract).

Realistic residual: **~0.2-0.4% of all wall clock**, against a ~4 KB hand-written `.s` on the
routine that integrates the ship's position — where a byte-level divergence is not a glitch, it is
a different flight. Priced and left for the user to schedule; the fold above took the free half.

**If it is scheduled, the design is fixed already:**
- Seam `ROF_INTEG_ASM` + `make INTEG_C=1` fallback, mirroring `ROF_SFXMIX_ASM`.
- `a2 = mem`, `a3 = cpu` (every address this function touches is below $8000 — highest is $291E —
  so one base reaches all of them through a signed d16).
- Verification is an in-process differential like `sfxmix_verify`, NOT `make validate`: the fold
  and the twin are both Amiga-only, and `make validate` cannot see either. The compare window is
  wide — this function writes ZP $20-$3E plus $2871/$2873/$2883/$2884/$2885-$288A/$28D6/$0686/$0687
  /$0677/$0685/$2850-$2853/$2829/$282C/$0068/$0069/$284E/$284F/$0039/$005D/$2917/$2919-$291E and
  the five $2893/$289A/$28A1/$28A8/$28AF ring rows — so snapshot ZP $00-$FF plus $2820-$2930 plus
  $0670-$0690 and `cpu`.
- ⚠ The callees are NOT pure (they push the SFX event ring, read POKEY RANDOM), so the differential
  must alternate the order and restore between halves, exactly as `sfxmix_verify` does and for the
  same reason.

### 12.4 SCHEDULED by the user (2026-08-14), and STILL not written — the residual went into C instead

The user cleared the block ("I'm not sure why flight_control_integrate is gated on me — if you get it
working, that's just great"). Before writing the `.s`, §12.3's own list of "genuinely asm-shaped wins
left" was re-read, and its **first entry turned out not to need asm at all.**

The opcode histogram of the shipping `flight_control_integrate_impl` (1245 instructions) is the tell:
**377 `move.b`, 23 `lsl.l`, 19 `ror.w`, 21 `lsr.w`, 26 `or.b`.** The shift traffic is the "6502 16-bit
carry idiom" §12.3 named — and it is not the 6502's fault, it is one C spelling. `((uint16_t)hi << 8)
| lo` forces GCC to build the BIG-endian word (because `or.b` reaches only bits 0..7) and byte-swap it
back: `moveq#0 / move.b LO / lsl.l #8 / or.b HI / ror.w #8` = **74 cycles, 46 of them shifting**.
`lo + 256*hi` is byte-identical for byte operands and compiles to **58 (−16)**; a pair-ADD goes
**152 → 98 (−54)** because GCC factors the two `lsl.w #8` into one. Now `ROF_PAIR16` in
`src/cpu/m68k_math.h`; rule + audit command in `docs/m68k-optimisation.md`, full record in
`docs/flight-perf-log.md` §25.5.

**All 19 of this function's pair loads are gone for +10 instructions**, and 26 more across seven other
shipping flight routines (134 sites tree-wide → 89). Static win ≈ **0.22% of all wall clock**, of which
~0.08% is this function — i.e. **the same order as the whole twin's filed 0.2-0.4% residual**, for a
mechanical C edit gated by `make validate` instead of 4 KB of hand asm on the routine that integrates
the ship's position.

**So the twin is still NOT written, and the recommendation is now stronger than "priced":** the two
remaining asm-shaped items are GCC's register allocation and the five helpers it INLINES
(`ring_push_unmarked` ×5 at 152 bytes, `store_676_init`, `refresh_hud_field_0d_entry`,
`reset_flags_ff`, `mul_u8_lookup`) which a twin would have to re-implement in asm to avoid losing on
call overhead. That is the bulk of the work for what is now a *smaller* residual than before. The
design in §12.3 stands if it is ever wanted; nothing about it has been invalidated.

⚠ Generalises past this function: **before costing an asm twin, check whether the "asm-shaped" items
on your own list are actually C spellings.** Two of the three levers this twin was justified by
(absolute addressing in §12.2, the carry idioms here) were compiler-output problems with C fixes, and
between them they account for most of what the twin was ever worth.
