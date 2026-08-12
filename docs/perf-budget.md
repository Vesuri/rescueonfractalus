# Flight performance budget — judge every number against this

> Split out of `CLAUDE.md` 2026-08-10 to keep the always-loaded file small.
> **Read this before quoting, sizing or judging any performance number.** The headline target
> and the two hard measurement rules are summarised in `CLAUDE.md` §Performance.
> Companions: `docs/asm-migration-plan.md` (the asm twins), `docs/flight-perf-log.md` (the full
> investigation archive), the `flight-pc-profiler` + `flight-measurement-rules` memories (ranked
> TODO and per-change history).

**⭐ TARGET (user decision, 2026-08-08): 25 FPS = 40 ms/frame, judged on the BEST-CASE baseline
(`COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1`, **24.88 FPS after the 2026-08-12 codegen-tax + edge-plot
pass ⇒ ~+0.5% of throughput to go**; `docs/flight-perf-log.md` §23).
Combat-load slowdown is expected and does NOT have to reach 25.** 50 FPS remains the ideal, not
the bar.

⚠⚠ **That 24.88 is an ABSOLUTE reading and the absolute wanders ~1.2% between builds.** §23
re-measured the previous standing commit and got **24.09 where the ledger said 24.38** — same
commit, same flags, same script. So the claim that survives is the **in-session A/B, +3.25%**, and
"are we at 25 yet?" cannot be answered by one reading. **Rebuild and re-run the baseline in the
same session before diffing anything against it.** Spending 10 ms on *anything* is HALF the budget. The A500 (7 MHz 68000)
is slow — there is no room for half measures; be conscious of absolute milliseconds, always.

Units: 1 probe "tick" = 1 raster scanline = 63.56 µs; a PAL frame = 313 ticks = 20 ms. The flight
VBI ISR fires once per real frame; its per-firing cost is mostly the faithful 50Hz sim+audio. The
terrain draw dominates flight compute.

⭐ **The ISR was opened up and cut 14.7% on 2026-08-12 — 66.95 → 57.09 t/firing, 21.3% → 18.2% of
all wall clock** (probe build, quiet arm; `docs/flight-perf-log.md` §20). The "there is no 5-point
win in the ISR" filing was right about each item and wrong about the sum: five changes, none over
4 t/firing, add up to a sixth of it. Use **`amiga/isr_full.gdb`** for the breakdown — it is the
only script that WINDOWS every bucket (isr_ab windows three of them; phase_budget divides
power-on accumulators by a flight-only call count). ⚠ And read a single bucket carefully: the
"the ISR fires 50×/s so per-firing cost is cross-build legitimate" argument holds for the TOTAL,
but the cache-gated buckets (HUD, draw branch, proj) do work proportional to how much the view is
changing and therefore carry the same trajectory noise as DRAW t/it (§19).

⚠ **The old "mem-bound ALGORITHMIC FLOOR / 50 FPS needs an algorithmic change / micro-opt EXHAUSTED"
conclusion (2026-06-29) is RETIRED — it was disproven.** Hand-written m68k ASM broke that floor with
NO faithfulness loss (the C *was* near GCC's floor; the floor was GCC, not the algorithm). Done
2026-06-30: `terrain_column_rasterize_core` asm twin (~27% faster than the C oracle) + instruction-
shaving (another ~9%); `renderFlightDirect`'s plane-1 edge-plot asm (~2.8×). See
`docs/asm-migration-plan.md` + [[flight-scene]]. **The lever for hot code is hand-asm (control the
regs, force `(a0)+`/`(d8,a0)`, shave every redundant insn), not the transliteration model.**

## Current measured budget

**Per iteration, deep flight, all asm in:** terrain draw both passes ~167ms
(dominant) [rasterize ~64% (asm'd) · project_terrain_points ~20% (asm'd, ~2.2× — `ProjectTerrainAssembler.s`)
· subdivide ~16% (asm'd — `TerrainSubdivideAssembler.s`; ⚠ the old note here said its bracket is dominated
by the raster leaf-fills it drives "so removing GCC's spills barely helps" — TRUE of spills, but it made
the whole twin look spent and it is NOT: **~50% of a subdivide call is MARSHALLING**, and 2026-08-08 took
~245 cyc/call out of it, `docs/asm-migration-plan.md` §Phase 8)] · VBI ~3.63ms/firing × ~20
firings/iter (was 4.25ms before the 2026-08-12 ISR pass — §20) · renderFlightDirect ~24ms ·
setup+clear ~31ms [terrain_frame_setup loops asm'd ~26% — `TerrainFrameSetupAssembler.s`]. **Two bit-serial
loops are now byte-exact lookup tables:** `mul_u8` → the 64KB `g_mulTable` (mul_u8 is NOT a plain product so
no single mulu is byte-identical — see docs/asm-migration-plan.md), and `terr_blend` → the 8KB
`g_blendHi`/`g_blendLo` nibble pair, which took the whole flight VBI 109.35 → 104.12 t/firing (−1.67% of ALL
wall clock; `sample_terrain_height_bilerp` was ~10% of the ISR). ⚠ The VBI's `integ`/`proj` buckets run on
only HALF the ISR firings (the `$00C8` parity alternation) — per CALL they cost double their t/firing figure.

⚠ **RAM budget (measured 2026-08-07, `amiga/memreport.gdb`): load image ~505KB + 158,544 bytes of runtime
CHIP `AllocMem` = ~675KB, ~170KB chip-mandatory — so the port needs 1MB and does NOT fit a bare 512KB A500.
On a 512+512 machine the binding limit is the ~499KB non-chip image against the 512KB slow bank; check it
before adding another table.**

**✅ The rasterize restructure is DONE (2026-08-05, c636951): −36% beam-ticks/call (24→15), share 34.5%→29.8%,
byte-identical.** It did NOT need fewer subdivisions — it dropped the control-point COLUMN representation for
a tracked `span` (so the ccol load / `gap` subtract / midpoint store all vanish), put the TOS control-point
height in a register, and straight-lined spans 3+4 (47.7% of all far-bisects, found by shape-probing the
algorithm's input distribution — `make RASTER_C=1 RAS_SHAPE=1 PROBES=1` + `amiga/ras_shape.gdb` — not by PC
sampling). Design, the shape data and the evaluated-and-rejected follow-ups: `docs/asm-migration-plan.md`
§Phase 4. **Recipe worth reusing: shape-probe the algorithm → prove the algebra on the HOST
(`tools/ras_restructure_test.c`, 1.6M randomised cases) → then write the asm → then the on-target
differential, A/B'd against the SAME C oracle.** (The old "NEXT: signed_mul_8x16" item was already
STALE — `BuildViewAssembler.s` retired it 2026-07-05: its product core is a plain unsigned `mulu.w`,
not a bit-serial multiply. Don't re-open it.)

**⭐ Phase 5 + the number that should steer the next attempt (2026-08-05, `docs/asm-migration-plan.md`
§Phase 5).** DRAWDOT's column arithmetic → two plotCol tables (0 mismatch, host-proven over all 65536
input pairs) measured only **~0.9%**, and the shape probe says why: **an accepted draw is NOT a plotted
dot.** The dot is plotted at the column's PREVIOUS top and the per-frame `$6B` reset floor sits on the
one excluded scanline, so a column's first accepted draw never writes. Measured over 489 half-frames:
286590 draws → 39% accepted → **only 12% actually write a dot**. So **DRAWDOT is ~23% of the rasterizer
and the other ~77% is tree traversal**; every remaining per-plot micro-opt is capped at ~2% (this also
retires the "incremental dot column ⇒ ~5%" estimate — wrong denominator). Subdivide's dead mem[]
round-trips (the `$8D-$91` entry load — a pure round-trip, flushed conditionally with the BUDGET as a
free dirty flag — and the dead `$83/$85` stores before each rasterize) took its asm-vs-oracle margin
**7.4% → 12.5%**.

**⚠ Two more candidates measured and CLOSED 2026-08-05 — do NOT re-open them (`docs/asm-migration-plan.md`
§Phase 5):** *whole-subtree (leaf) occlusion culling* — P is HIGH (58%/56% of span-3/span-4 groups fully
hidden, and 98% of misses bail on the first compare, so the old ~81% break-even fear was wrong) but it is
worth only ~2% of the rasterizer, because a rejected DRAWDOT is already just 32 cycles and the cull test
must re-read the same `COL_MAX` byte; and *subdivide's far-endpoint reload elisions* — the recursion is
too SHALLOW to matter (1.23 inner iterations / 0.40 midpoints per call), measured 0 ± 0.5% and reverted.
**The generalisable rule: the reject path is at the floor — any "check before drawing" scheme re-reads
the very byte the check was meant to avoid, so it can only recover the ~10 cycles of bookkeeping around
that load.**

## ⭐ Where flight actually stands — 23.91 FPS best case (2026-08-12, after the ISR pass) / 16.00 combat (4d25815, STALE)

**23.91 FPS best case — 1435 painted / 3001 vbi, all 15 segments valid** (per-segment 19.5–26.8),
the best recorded, measured on the lean arm after the flight-VBI pass (`docs/flight-perf-log.md`
§20). **The control was re-measured in the SAME session on the SAME flags: 1335 / 3000 = 22.25 FPS,
so the pass is +7.5%** — well outside this metric's ~1% trajectory noise (§19.8), and it is the
in-session A/B, not a diff against a ledger row. **25 FPS now needs ~+4.6%.**
⚠ The combat row below is from 4d25815 and is stale.

### The 2026-08-08 baseline it replaces — 20.60 FPS best case / 16.00 combat (2026-08-08, 4d25815)

**`make FPSCOUNT=1` + `GDBSCRIPT=fps_seg.gdb ./diag_run.sh 200` is the ONLY way to quote a
flight framerate.** It adds just the headless auto-launch and one increment per painted terrain
frame; `g_vbiCount` is bumped by the real VERTB handler in every build, so
**FPS = 50 · g_fpsFrames / g_vbiCount** — frames per *emulated* vblank, which makes it immune to
host speed and to the gdb stub. Both arms re-measured **2026-08-08 at 4d25815**, ~3000-vbi window,
all 15 segments valid:
**best case (`COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1`) 1235 painted / 2998 vbi = 20.60 FPS** (17.0–22.6
per segment) · **combat load (`COMBAT=1 FIXED_RNG=1`) 959 / 2997 = 16.00 FPS** (12.2–18.7). Both arms
are level 40, which is the only legitimate combat framing. Combat costs **22.3% of throughput =
+28.8% frame time** — unchanged from e35d904's 22.2% / +28.6%, because every change since is in the
terrain pipeline that both loads run (quiet 18.41 → 20.60 = +11.9%, combat 14.32 → 16.00 = +11.7%,
which is the consistency check). ⚠ `COMBAT_QUIET` also drops `ROF_AUTO_FIRE`, so the best case is
"no enemies AND no firing"; and the combat run's altitude varies across segments where the quiet
run's does not, so **size changes off the quiet arm, never the combat one.** (⚠ The "altitude" in
that last clause is `mem[$28DA]`, which is really the frame's terrain SPAN EXTENT, a render output
— see `docs/flight-perf-log.md` §19.3.)

⚠⚠ **The same confound applies to `phase_budget.gdb`'s t/it rows, which had been treated as
exempt.** `FIXED_RNG` pins the level and re-pins the LFSR at the flight rising edge, but the flown
path is still coupled to the render rate, so each build flies its own ground and DRAW's cost is a
property of the view. Proven 2026-08-12: moving the probe auto-launch's START press 120 frames
later — no flight-code change at all — moves DRAW by **+111 t/it**, the full size of the "8%
regression" that was filed as the top perf item. **Cross-build t/it deltas carry ~±10% of
trajectory noise; price a change with an in-process differential or a static cycle count.**
Full account: `docs/flight-perf-log.md` §19.

⚠ **This harness routinely OVER-reads a win** — 18.41 → 19.49 read +5.9% for two changes whose
differentials predicted +1.9%. A cross-build end-to-end delta is confounded (`FIXED_RNG` pins the
LEVEL, not the trajectory), and single-window noise is ±2%, so anything under ~3% is agreement, not
evidence. **Quote the static cycle count or a differential ratio as the win and the FPS row only as
the standing baseline.** Per-change history + the full table: the `flight-pc-profiler` memory.

⚠ **Every framerate figure in an older note or commit is wrong — do not quote one, re-measure.**
Two traps, both of which produced badly wrong numbers before this was built:
- **Instrumentation.** Even a lean `PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1` build reads 9.4
  FPS — ~35% slower, because `FP_TIME` is two CHIP register reads plus a 16×16 multiply several
  times per iteration. The older "~2 fps"/"~6 FPS" figures came from probe builds and/or the
  `FP_TIME` accumulators (known-poisoned: `rof_beam_line` races the ISR's `g_vbiCount++`).
  Corollary: the PC profile's own shares are taken on a probe build, so buckets containing
  `FP_TIME` brackets (`renderFlightDirect`, `game_main_loop`) are inflated.
- **The crash.** The auto-launch flies with NO input and eventually hits a mountain;
  `renderFlightDirect` then stops while `g_vbiCount` keeps ticking, so a wide window straddling
  the death cinematic under-reports (a 3000-vbi window read 9.0 where its live segments read
  12–17). Sample in SHORT windows and discard any row not at `VVBLKI=$4ff5` / `$3D=00`.

**So the 25 FPS target is a 1.21× gap on the best case = remove 18% of the frame** (from the combat
baseline it would be 1.56× / −36%, which is explicitly NOT the bar). The
profile is FLAT (nothing >32%), so no single function gets there — but **the smaller gains are
worth taking**: at this gap, five or six honest 5-point wins reach the target. Size a
candidate, and if the win is real and the risk is low, do it. What is CLOSED is only what
*measured* at ~0 or negative (the occlusion family, Phase-5 dot tables, subdivide reload
elisions) — closed on data, not on pessimism about small numbers. The ranked TODO lives in the
`flight-pc-profiler` memory.

## Measuring an asm twin

⚠ **Measure asm twins with the in-process differential** (`make VERIFY=1 PROBES=1` +
`amiga/raster_verify.gdb`): asm + C oracle run back-to-back on the SAME inputs in ONE run, byte-compared
+ beam-ticks tallied per-impl. **Cross-run comparison (render-diff, or beam-probe
asm-build-vs-C-build) is NOT valid by default** — the async 50Hz VBI desyncs frames vs the free-running
main loop when render speed changes (vbi 2204 vs 2217 at the same `fdCalls`), and that shift changes the
POKEY RANDOM read count, which picks a **different level** (measured: plain vs `RASTER_C=1` generated
entirely different `$0900`/`$0A00` maps, 23 vs 25 pilots).
- ⭐ **`make FIXED_RNG=1`** re-pins the `$D20A` LFSR before the fresh-start level seeding and on the
  flight rising edge, so **every build flies the same level** (proven byte-identical across builds via
  `amiga/rngcheck.gdb`). **Use it for every perf measurement.** OFF by default — it removes real
  gameplay variety, so never judge rendering or gameplay from a `FIXED_RNG` build. Note the level
  seeding happens at **vbi≈1732 (~35 s)** on the launch path, not at power-on: give probes ≥45 s.
- ⚠ **The differential's metric is the asm/C RATIO, not the absolute ticks/call** — the same binary
  swings 15% run-to-run on absolutes while the ratio holds to ~0.5%. And **run any baseline ≥2× after
  a rebuild** before believing a delta; an n=1 baseline once produced a bogus "4% regression" verdict.
  Its bracket also INCLUDES nested callees (subdivide's includes the rasterizer), so it is not that
  function's own cost — the PC profile is.

**Target: A500, 25 FPS on the BEST-CASE baseline (user decision 2026-08-08); combat may sit lower.**
Surface the numbers honestly.

## Correctness + measurement

Every rewrite must stay byte-identical — `make validate FN=<name>`
diffs full `mem[]` state vs the transliterated `__t6502` oracle (exit `cpu` regs are usually dead
→ "incidental cpu diffs" are fine; **0 mem mismatch is mandatory**). Measure on real hardware via
the headless beam probe (`make PROBES=1` + `diag_run.sh`); `FP_TIME` subtracts the flight-VBI ISR
beam-lines (`g_isrBeamLines`) so a phase bucket excludes ISR firings in its window. ⚠ Per-phase
flight numbers are **terrain-dependent and noisy run-to-run** (±30%) — trust large deltas, not
small ones, and confirm the real cause by reasoning about access counts.

## Flight VBI ($4FF5) ZP write-set

Audited 1872 firings + static cross-check. The in-flight
VBI writes only `$14 $20 $27-$2E $33 $34 $42 $44 $4B $5D $61 $62 $70 $73 $74 $B9-$BF $CE $D8-$DC
$E6` (attitude/throttle/altitude, HUD/atmosphere colour ramp, object+sprite state). It writes
NONE of the terrain rasterizer/subdivide cells (`$60/$80/$81/$82-$86/$8D-$91/$95/$96/$9F/$B5/$B6/
$EA/$EB/$F4/$F5`) nor `$260E`. So the main-loop terrain renderer's working set is disjoint from
the VBI's — useful when reasoning about ISR-safety of a main-loop optimisation.
