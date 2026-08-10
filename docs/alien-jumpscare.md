# Alien "Jaggi" jump-scare — trace map

**STATUS (2026-08-10):** trigger, gate and creature draw are all mapped and PORTED — the creature
renders, animates and composites (user-confirmed).  The open half is **perf**, and the two things
worth knowing before touching it are both in **§SESSION 8** at the bottom: there is now a
**headless bench** (`make ALIEN_BENCH=1 PROBES=1`, 50 s, repeats to the exact tick — you no longer
have to fly to measure), and **every figure recorded before it was inflated ~35% by its own
per-cell probe.**  **OPEN #2 (colour) is CLOSED as NOT-A-BUG — §8.7: `$0047` cannot reach the viewport,
it is dead during the knock, and the creature's pens already match the Atari's, measured.**
⚠ The sections below are a CHRONOLOGICAL record:
every "OPEN" and every number above session 8 has been superseded at least once.
**Read §SESSION 8 first, then §SESSION 7.**
(This header used to read "IN PROGRESS / creature draw still unlocated" — resolved 2026-07-25.)

The classic *Rescue on Fractalus!* jump-scare: a rescued figure runs to the ship, and at the
"knock" an alien jumps onto the canopy instead of a pilot boarding. This is the port target for
the flight-scene "alien-on-windshield" open item. Status: **trigger + gate mapped; the creature
DRAW routine is not yet located.**

## Trigger (who is an alien)

- **The pilot/alien fork is the map marker `$0A00[$28E6]`.** `pilot_render $7854` (native twin,
  rof_native.c): at landing phase (`$003D >= 3`) it sets `$281E = 1`, then reads the marker —
  `== $80` keeps `$281E = 1` (alien-designated), anything else DECs `$281E` to 0 (pilot).
  Validated: `a800dumps/rescue_pilot.a8s` (level 4) has marker `$C8` and is a real pilot.
- **Alien designation happens at SPAWN** via `$4E58` (scans `$0A00` slots with marker `$01`; those
  not in the `$2276` table become `$80`). The level-derived threshold `$061B`
  (`compute_stage_display_geometry $75F5`, from `level_stage $006D`) makes aliens impossible on
  levels 1–10 and increasingly likely after (see [[flight-scene]] for the table).
- ⚠ `alien_attack_tick $7AB8` is MISNAMED — it's the per-frame **alien-attack tick** (ship-shake via
  `jitter_roll_pitch $AA95`, pounding SFX, phase toggle `$283D`); **no PMG**. See docs/rename.md.

## The knock, and the gate (why FORCE_ALIEN shows no visual)

At the knock (`pilot_render` phase 4, `L_79a8`, native rof_native.c ~line 3098):
- **`$281E != 0` (alien) and `$003C == 0` (airlock closed):** calls **`alien_knock_setup_loop`** every frame,
  which — because systems are off (`$003E != 0`) — enters a **blocking sound loop** driving
  `alien_creature_animate_draw` (an SFX sequencer over tables `$81E2/$81E8/$820A`) once per RTCLOK frame until
  systems come back on. Pure SFX → **this is the "attack sounds", NO creature drawn.**
- **`$281E == 0` (pilot) and `$003C == 0`:** calls `animate_clear_colors_timed $7A17` (RTCLOK-gated
  colour sweep) — the pilot "waiting/knocking" animation.
- **Either fork reaches the REVEAL only when `$003C != 0`** (`$79D9 → $79E1`): the reveal sets
  `$0633 alien_trigger` (alien) or scores (pilot), sets the airlock light `$3388`, `INC $003D`.
- **`$003C` is set ONLY by the airlock command** (`event_sequence_dispatcher $4644`, event index 1
  at `$46EA` → `$471E INC $003C`), i.e. the **A key**. No auto-trigger exists in the dispatcher.

**Conclusion:** the jump-scare reveal (and the `$0633`-gated ship-shake) require the **airlock to be
opened**. `FORCE_ALIEN` (forces only the marker → `$281E`) without opening the airlock reaches only
the `alien_knock_setup_loop` sound loop → matches the observed "attack sounds + explosion, no shake, no
creature". `FORCE_ALIEN` + opening the airlock at the knock SHOULD reach the reveal (untested).

## ★ RESOLVED 2026-07-25 — the creature draw is `alien_creature_animate_draw` → `$80C5` (airlock-CLOSED path)

**Two mistakes in the earlier hunt** (both now corrected):
1. **Wrong path.** The classic jump-scare (alien on the glass) is the airlock-**CLOSED** case, not
   the reveal. At the phase-4 knock (`pilot_render` L_79b2, `$281E!=0` alien): **`$003C==0` (airlock
   closed)** → `alien_knock_setup_loop`, which is NOT "sound only" — it seeds a creature-animation state
   (`$0632=1`, `$005E/$005F`, `$2921/$2924/$2926`, `$2930/$2931` position, `$0635/$0638/$0639`) then
   loops calling `alien_creature_animate_draw` every frame while systems stay off. **`$003C!=0` (airlock OPEN)** →
   the reveal (`$0633` set) = the alien **BOARDS** = "ALIEN IN SHIP" + shake + energy drain, and draws
   **no** creature (verified: `g_alPlotCalls=0`, `g_alDrawShape=0` over 158 `$0633` frames). So opening
   the airlock is the *boarding* mechanic; the *scare* is keeping it closed.
2. **Wrong routine + wrong probe gate.** The creature is NOT drawn by `plot_clipped_pixel` or
   `draw_scaled_shape` (the two the `g_al*` probe hooked), and NOT while `$0633` is set (the gate the
   probe used). It is drawn by **`alien_creature_animate_draw` ($7F85)** — misnamed "sfx_seq_step"; it animates the
   frame tables (`$81E2/$81E8/$820A`) *and* draws — via **`$80C5`**: a masked bitmap blit
   `LDA $BE00,X / AND ($8B),Y / ORA shapeByte / STA ($8D),Y`, 17 bytes wide, row stride `$60` (=96,
   the mode-D row), into the `+$30` display half, positioned from runtime row-address tables
   **`$073D`/`$0793`** (built at `$7464/$7469`) indexed by `$2930`/`$2931`. Shape geometry comes from
   tables `$81A1/$81A9/$81B1/$81B9` + `$81C1/$81C9/$81D1/$81D9` (loaded into ZP `$80-$8A`).

**Why it's invisible on the Amiga (same class as the fixed ground-objects bug):** `alien_creature_animate_draw` is
transpiled (`rof_gen.c:13267`) and DOES run on the Amiga (it's the knock loop that already produces
the SFX). Its `$80C5` writes land in the mode-D field, but `renderFlightDirect` renders the terrain
body from `$260E` + `g_flightDotPlane` and only reads the field for the band rows (43-46) → the
creature's field-body writes are dropped. Exactly the ground-object case (fixed by hooking
`terrain_plot_pixel` to also OR into `g_flightDotPlane`) — but `$80C5` is a *different, un-hooked*
plot routine, and its shape is a **masked multi-value blit** (not single dots), so the port needs to
mirror its field writes into the Amiga flight plane(s) with the value→pen mapping.

## ★★ RENDERS 2026-07-25 (session 2) — creature shows + animates + lights blink (user-confirmed). PERF is the open blocker.

**What now works (committed 9e8a83c → e36b6a1):**
- **Mirror:** `alien_shape_blit` ($80C5, the native twin the game actually runs — NOT the transpiled
  oracle) mirrors each creature byte, gated on `$0632` (alien_knock_active), into the paused-rescue
  overlay **`g_figP1/P2/M`** via `ROF_PLOT_ALIEN` (rof_native.c). Geometry MEASURED + matches the figure
  overlay: field base `$1010`, stride 96; `rel = A-$10A4`, `r = rel/96`, `b = rel%96`; byte decoded via
  `kModeDP1/kModeDP2` (now `extern "C"`). `alien_knock_setup_loop` clears the overlay each step (`ROF_CLEAR_FIG`).
- **Render path fix (THE key unlock):** the knock is a *blocking* loop in `alien_knock_setup_loop` (main loop
  never runs). The old faithful pre-wait spinwait `while(RTCLOK<=4){render}` NEVER rendered on the Amiga
  (`g_alRF=0`, frozen screen) because RTCLOK is advanced by the **hardware flight VBI**, not renderFrame,
  so after the slow `$7F85` draw RTCLOK was already >4 → wait was a no-op. Restructured to faithful
  **wait-then-draw with ONE composite per SFX step**: busy-wait the 5-frame interval (copper displays the
  last-composited creature continuously = the ANTIC stand-in, NO re-render), then `ROF_CLEAR_FIG` + draw
  + `platform_render_frame()` once. renderFrame → `renderFlightDirect`'s **rescueFigure branch** (active
  at systems-off phase≥3) composites the overlay over the frozen terrain. (`g_alComp=28/28`, `g_alRF=28`,
  VVBLKI=`$4FF5`, rsFlight=1 — all confirmed.)
- **Airlock-OPEN path** = the *boarding* mechanic ("ALIEN IN SHIP" + shake + energy drain, `$0633` set),
  draws NO creature. The jump-scare is the airlock-CLOSED knock. (`FORCE_AIRLOCK` = boarding test only.)

**★ OPEN #1 — PERF (the blocker, ~7.5× too slow).** ⚠ **SUPERSEDED — these are the session-4 numbers;
the current ones are 2.6× / 238 ms / 18 ms in §SESSION 7.  This paragraph is the figure that leaked into
the `flight-scene` open item and stayed there for two weeks.** Measured per knock STEP (beam ticks,
313=1 frame=20ms; faithful step = 5 frames = 1565): **draw `alien_creature_animate_draw` ≈ 8438 ticks
(~540ms), render ≈ 3284 (~210ms), wait ≈ 0** (no pacing left — draw+render already blow past 5 frames).

**★ MEASURED 2026-07-25 (session 3) — the hud-vs-wrapper split: `alien_shape_blit` is ~100% of
`alien_creature_animate_draw`; the wrapper is ~0%.** (`g_alTHud` ≈ `g_alTDraw` per step, `hud/draw`=111% within async-VBI
noise; 40 hud calls/step.) So native-twinning the `$7F85` *wrapper* does NOT help — the lever is the row
composer + cutting per-row work.

**★ DONE 2026-07-25 (session 3, commit fa1d06b) — clean-C rewrite of the whole draw tree** (foundation for
the next perf pass; all byte-identical `make validate`, both backends build):
- `reorder_cell_bits` → a fixed bit-permutation (no more 22-op ROL/ROR chain; **dropped ~12 `$0084`
  bus ops/call**, ~480 pack calls/step) + a `_core` so callers keep the accumulator in a register.
- `alien_field0_fill/1/2/3` → hoisted the ZP source-pointer reconstruction out of the per-byte loop.
- `alien_shape_blit` → hoisted the mask/dest row-pointer bases out of the 17-cell inner loop.
- `alien_creature_animate_draw` → now a validated native twin (clean sequencer + setup + blit loop), no longer transpiled.

**★★ SESSION 4 (2026-07-26) — measured wins committed; per-step ~750→~379ms (~50% faster).** Progression
(noisy ±30% run-to-run; trust the trend): step ~750ms → ~440 → **~379ms**.  All committed, both backends build,
draw tree still `make validate` byte-identical.
- **DRAW (commit 1e75d7e): division-free mirror = the big win, ~540→~235ms.**  `ROF_PLOT_ALIEN` mapped each
  field byte→overlay (row,col) with `int rel/96` + `rel%96` PER CELL — on the 68000 that's a SOFTWARE 32-bit
  divide (`__divsi3`, ~1000 cyc) TWICE per cell × ~646 cells = ~140ms/step of pure division.  Fixed: the 17
  cells are consecutive + `dstRow` loop-invariant, so one divide per ROW (col = figB0+y, single 96-wrap).
  Entirely `#ifdef ROF_PLATFORM_AMIGA` so validation untouched.
- **"Cut dead rows" — REJECTED (not faithful).** The blit OVERWRITES + is displayed; `ROF_PLOT_ALIEN` mirrors
  every byte OPAQUE (`g_figM=0xFF`).  A skipped row = transparent (terrain shows through) ≠ a drawn blank row
  (opaque bg).  Loop maps almost entirely onto on-screen field rows (`_r≈7-45`); ~0 safely-cuttable rows.
- **RENDER (commit 777c62f + 81ecbfd): ~207→~152ms.**  Two findings via probes (`g_alTRScene/Idle/FlipWait`,
  `g_alVSwap*` in diag_alien.gdb): (a) the flip busy-wait is NOT the cost — `while(flightSwapPending)` is only
  ~13ms/step, ISR fully live (~22 swaps/step); (b) the ~194ms is the **dirty-rect COMPOSITE on the AMIGA
  PLANAR back-buffer** (erase 80B/row + masked cookie-cut draw 40B/plane/row = ~18k chip-RAM byte accesses/step
  — genuinely ~190ms on the 68000, no cache + display-DMA contention).  Fixed the *lighter knock render path*
  (skip the 6 sprite rebuilds — correct but wait-bound so no wall win) + **32-bit-word composite** (4-aligned,
  byte-lane-preserving so endianness-neutral) → composite ~194→~136ms.  Only ~30% because it's
  chip-RAM/ISR-bound, NOT access-count bound.

**★★ SESSION 5 (2026-07-26) — the composite is now a BLITTER cookie-cut (Bitmap::combineWithMask). Awaiting the
user's interactive perf/visual confirmation.** Done + build-clean (both backends; draw tree still `make validate`
byte-identical — all changes are `#ifdef ROF_PLATFORM_AMIGA`):
- **The alien composite AND the pilot-run composite are the SAME path** — both write the `g_figP1/P2/M` overlay
  (alien via `ROF_PLOT_ALIEN`/`alien_shape_blit`, pilot via `ROF_PLOT_FIG`/`plot_clipped_pixel`) and both are
  drawn by `renderFlightDirect`'s `rescueFigure` branch. So this one change speeds up BOTH.
- **The per-frame erase+draw is now ONE `Bitmap::combineWithMask` blit** (4-channel A=mask B=figure C=clean D=dest;
  dest = (clean & ~mask) | (figure & mask)). It erases the previous figure AND draws the new one in a single
  blitter pass, offloading the whole ~136 ms/step CPU composite to the blitter (parallel to CPU + the 50 Hz ISR).
  Composites the UNION of the previous per-buffer box + the current figure rows (ROF_CLEAR_FIG already zeroed the
  old mask, so clean is written back over the old figure). Only planes 1+2 touched; plane3 (frame) stays clean.
- **Buffers moved to CHIP RAM as Bitmaps** (blitter can't read BSS): `s_cleanBmp` (3bp interleaved, ==terrain),
  `s_figBmp` (2bp interleaved, figure planes — row stride now 80), `s_figMaskBmp` (1bp mask, stride 40). The plot
  macros' figure-plane index changed 40→80 (mask stays 40). Snapshot/seed are now `Bitmap::copy` blits. ~11 KB chip.
- **★ FRAMEWORK FIX (unblocks ALL Bitmap methods): `BitmapAssembler.s` `bsr`→`jsr`.** The Bitmap asm twins
  (copy/clear/copyWithMask/line/fill) called the blitter primitives with `bsr` = PC-relative-16 → left an
  `R_68K_PC16` relocation the GCC-path `Elf2Hunk` REJECTS ("Unsupported relocation type R_68K_PC16"). That is why
  these methods were unusable from the app until now (framework originated under SAS/C). `jsr` emits `R_68K_32`,
  which Elf2Hunk supports. **This makes the whole Bitmap blit suite linkable** — the basis for the future
  pre-rendered-chip-frames rework (pilot zoom, tunnel clear, etc.). Do NOT revert to bsr.
- **★ BLITTER-QUEUE DRAIN BUG (fixed) — the initial glitch.** `Bitmap::combineWithMask` on a 2-plane figure
  enqueues ONE BLIT PER PLANE (and `Bitmap::copy` of an interleaved bitmap is multi-blit); the framework starts
  each *queued* blit from the blitter-done INTERRUPT, async.  `AmigaHardware::blitterWait()` only spins on
  `isBlitterBusy()` → it returns after just the FIRST blit, so flipping showed a buffer with plane1 composited
  but **plane2 missing** (half the figure's value bits) — read as "lower half not rendered / vertical jump /
  bitplanes off", intermittent on the alien, obvious on the moving pilot.  FIX: new **`AmigaHardware::blitterDrain()`**
  (the same `setInterrupts(INTF_BLIT,false); while(hasQueuedBlits) processBlitterQueue(); blitterWait();` idiom
  `blitterFillUp` uses) fully drains the queue before the flip.  ⚠ RULE: after any multi-blit Bitmap op, use
  `blitterDrain()` (NOT `blitterWait()`) before reusing/reading/flipping the target.  User-confirmed glitch-free.
- **⚠ PERF RESULT: the blitter composite is ~NEUTRAL (~119 ms vs the old ~136 ms CPU loop); per-step still ~379 ms.**
  Measured (glitch-free, 40 steps): draw `alien_creature_animate_draw` ~3851 ticks (~245 ms), render ~2103 (~134 ms; composite
  ~1870 = ~119 ms, flipWait ~227 = ~14 ms).  WHY no win: the knock is a BLOCKING loop, so the CPU has NO
  concurrent work to overlap the blitter with — it just `blitterDrain()`s.  Offloading to the blitter only pays
  off when the CPU runs in parallel.  **★ The bottleneck is now the DRAW: `alien_shape_blit` = ~245 ms = ~65%
  of the step (hud ≈99–103% of the wrapper).**
**★★ SESSION 6 (2026-07-26) — narrow-rect composite + row-offset mul-tables + pointer-walk loops.** Both backends
build; `make validate FN=hud` green (all changes `#ifdef ROF_PLATFORM_AMIGA`).
- **Narrow-rect composite.** The plot macros now track the figure's byte-COLUMN extent (`g_figColLo/Hi`) alongside
  the rows; the composite unions it (per buffer, word-aligned) and `combineWithMask`s only that sub-rect instead of
  full 320 px/row.  `ROF_CLEAR_FIG` also narrows its clear to that column range.  Safe because ROF_CLEAR_FIG keeps
  the mask nonzero EXACTLY on the current figure → any covering rect composites with no ghosting.
- **Row-offset mul-tables `kRow40`/`kRow80` (siblings of the existing `kRow120`).** `row*stride` with a NON-sequential
  row (the plot macros, row derived from x/y) was a `__mulsi3`; routed through the tables.  `kRow120` was already
  used by the terrain plot macros; added 40/80 for the figure planes (mask 40, interleaved figure 80).
- **Pointer-walk for SEQUENTIAL loops (user-directed).** A sequential `for r … buf[kRow120[r]]` is better as a
  walked pointer (`p += 120`) than even a table read.  Converted rfPlaneSum, the resume-restore plane2 copy, the
  object-overlay apply, the crosshair stems, and the compass decode.  Tables stay for the non-sequential macros.
- **✅ VERIFIED (PROFILE_NORING=1, 50 steps, user-run) — the composite is now CHEAP and the render is a non-issue.**
  ⚠ **All the earlier "~119/116 ms composite" numbers were PROBE POLLUTION**: with the `RF_RING` debug ring on,
  `renderFlightDirect` runs `rfPlaneSum` ×6 (six full 47×40 volatile scans) per frame ≈ 85% of the "render" sample.
  With `PROFILE_NORING=1` the real split is:
  - **`composite+drain` (blitter, `g_alTComp`) = 72 ticks ≈ 4.6 ms/step** (was a ~136 ms CPU loop — blitter +
    narrow-rect DID work).  Bitmap addrs `fig=$15188 clean=$12578 mask=$15EF8`, all `<0x200000` → **blitter path
    confirmed** (no CPU fallback).
  - **render total = 343 ticks ≈ 22 ms/step** (scene 331: flipWait 248 ≈ 16 ms is now the biggest render cost,
    composite 72, misc ~11).
  - **DRAW = 4260 ticks ≈ 271 ms/step (~93% of the step); `alien_shape_blit` ≈ 104% of it.**  THE bottleneck.
- **✅ TERRAIN-RETENTION FIX (user-confirmed).** The alien mirror marked the whole 17-cell blit rect opaque
  (`g_figM=0xFF`), so `combineWithMask` cleared the rect to pen 0 instead of letting the frozen terrain show —
  because on the Amiga the mode-D field body is SHED (blank), so the mirrored `v` is creature-on-blank (bug was
  in the CPU version too).  Fixed: alien mask = the SILHOUETTE (`kModeDP1[v]|kModeDP2[v]`, opaque only where the
  mode-D value is nonzero) → value-0 pixels transparent → `s_cleanBmp` terrain shows through.  (Pilot `ROF_PLOT_FIG`
  already built a per-pixel mask, so it was fine.)  Caveat if ever needed: this treats value-0 as transparent; the
  fully faithful mask would derive opacity from the `$BE00[cell]` transparency table + shape byte.
- **✅ STALE-FIGURE FLASH FIX (user-confirmed).** Switching systems off for a NEW rescue could flash the
  PREVIOUS rescue's alien for one frame, before the run-up animation.  Cause = the figure OVERLAY
  (`s_figBmp`/`s_figMaskBmp`): after a knock it still holds the alien silhouette (ROF_CLEAR_FIG only clears at
  the START of the next draw), and the new rescue's first composite can run BEFORE its first figure draw
  populates the overlay → composites the stale alien.  (The terrain double-buffers ALSO retain the composited
  figure — measured: restoring only them did NOT fix it.)  Fix: on the true systems-back-on edge
  (`$3E` nonzero→zero, `s_resumeClearPend`), restore clean terrain into BOTH buffers from `s_cleanBmp` AND
  `clear()` the overlay + reset the figure extents / per-buffer erase boxes.  Keyed on the edge (not every
  non-rescueFigure frame) so the pilot approach's mid-zoom `$3D` dips don't trigger it; fires for any rescue.
- **★ NEXT = the DRAW (`alien_shape_blit`), nothing else moves the needle.**  Per-cell creature blit into the
  mode-D field (17 cells × ~44 rows) + the 4 hud_fill_field calls/row + the Amiga overlay mirror.  Levers: (a) the
  field `bus_write` is DEAD on the Amiga (the body is shed; only the overlay mirror matters) → skip it during the
  knock; (b) hand-asm the row composer; (c) the big one — **pre-rendered chip-Bitmap creature frames** blitted
  straight in (removes the per-cell CPU work entirely).  The render side (composite/flip) is done — do NOT keep
  optimising it (it's ~7% of the step).
- **⚠ TODO (user-directed, tracked): per-pixel MULTIPLIES in the plot macros.** `r*40`/`r*80`/`r*120` per cell is
  a 68000 `mulu` in hot loops. Build GLOBAL mul-tables for the common widths (40/80/120) and route every plot/
  composite index through them. Audit `ROF_PLOT_*`, the hud inline, `ROF_PLOT_DOT`, etc.

**(Superseded) NEXT SESSION — THE RENDER LEVER = the BLITTER (user-directed).** The composite is Amiga PLANAR with a mask
= textbook blitter cookie-cut.  The framework ALREADY has the primitives: `AmigaHardware::blitterCopy` (erase =
block copy) + `blitterCopyWithMask` (cookie-cut draw).  Offloads the whole ~136ms composite to the blitter
(runs PARALLEL to CPU + the 50Hz VBI ISR).  **★ INVESTIGATION ANGLE (user, 2026-07-26): the compositing
buffers — is the dest (`terrainBitmap`/`Back` = `bp2`) a `Bitmap` already (YES, chip)?  Are the SOURCES
(`s_clean`, `s_figP1/P2/M`) chip?  (NO — plain BSS today, the blitter can't read them.)  If we make the
figure/clean buffers `Bitmap`s in CHIP RAM, can we composite with the `Bitmap` class's OWN methods, which are
already blitter-based + asm-optimized?**  At minimum these composite loops should be all-asm; but `Bitmap`
likely already provides the blit/cookie-cut methods — prefer reusing them over hand-rolling `blitterCopyWithMask`.
This is the concrete first step of the future "anim frames = pre-made chip `Bitmap`s" rework the user described.
(Also still open, lower priority: the draw's ~44 hud rows × masked blit is now near the C floor; hand-asm only
if the blitter render still leaves the draw dominant.)  bus_read/bus_write are inline+cheap for RAM (ruled out).

## ★★ SESSION 7 (2026-08-10, HEAD 1dea72e) — RE-MEASURED: the step is 2.6× off, not 7.5×

Re-measured because the `flight-scene` open item still quoted the SESSION-4 numbers (draw ~540 ms +
render ~210 ms ⇒ "~7.5× too slow"), which sessions 5 and 6 had already superseded twice.  Interactive
knock (user-flown), **50 steps = one complete knock**, `PROBES=1 FORCE_ALIEN=1 PROFILE_NORING=1`,
read with `amiga/diag_alien.gdb`.  Beam ticks; 313 t = 1 frame = 20 ms; faithful step = 5 frames = 1565 t.

| per step | session 6 (2026-07-26) | **session 7 (2026-08-10)** | |
|---|---|---|---|
| draw `alien_creature_animate_draw` | 4260 t ≈ 271 ms | **3725 t ≈ 238 ms** | **93% of the step** |
| render (`platform_render_frame`) | 343 t ≈ 22 ms | **280 t ≈ 18 ms** | 7% |
| wait (SFX interval pacing) | ~0 | **1 t** | no headroom left |
| **step total** | ~4603 t ≈ 294 ms (2.9×) | **≈4006 t ≈ 256 ms (2.6×)** | vs 1565 t faithful |

- **−13% since session 6, and nothing was aimed at it** — the credit goes to **bf4c7cd** ("skip the
  dead mode-D field write during the knock", ~748 volatile stores/step), which shipped right after
  session 6 and was never measured.  Session 6's own lever (a) is therefore now spent.
- **`alien_shape_blit` = 104% of the draw** (44 calls/step, 2212 total; 752 creature-cell writes/step)
  — the wrapper is still ~0%, matching session 3's 99-103%.  Two independent runs a fortnight apart
  agreeing to 1 point makes this the most reproducible number in the file.
- **Render breakdown confirms it is FINISHED:** of 280 t, **flipWait 228 t is idle vblank wait**,
  composite+drain (blitter) **37 t ≈ 2.4 ms**, misc ~15.  Bitmap addrs `fig=$1CE38 clean=$1A228
  mask=$1DBA8`, all `<$200000` ⇒ blitter path, no CPU fallback.  ⛔ Do not optimise the render again.
- **⇒ The ONLY remaining lever is the draw**, i.e. session 6's (b) hand-asm the row composer or (c)
  pre-rendered chip-`Bitmap` creature frames.  At 93% of the step, halving it lands the step at ~1.3×.
- **`ROF_PLOT_ALIEN` is DEAD CODE** — zero call sites (`rof_native.c:860`), the mirror having moved
  inline into `alien_shape_blit` in 1e75d7e.  It still carries the `_rel / 96` + `_rel % 96` the
  open item kept listing as a perf TODO; deleting it is cleanup, not a win.
- **A knock is a FIXED ~50 steps** — the alien then smashes through the windscreen and the game ends
  (user).  Letting a run go longer gathers **no** extra steps; more samples means another rescue.
- ⚠ **No headless harness.** A `FORCE_KNOCK` seed of the phase-4 knock state (systems off + close
  range + phase 3 + airlock closed, so pilot_render's entry INC lands on 4) was written and then
  abandoned on the user's call — a synthetic entry into that state machine is not worth trusting for
  a perf number.  The knock stays user-flown; see [[interactive-fsuae-gdb]] for the SIGINT recipe.
- ⚠ `PROFILE_NORING=1` remains load-bearing (session 6: without it the debug ring's `rfPlaneSum`
  scans are ~85% of the "render" sample).  ⚠ A `PROBES=1` build shows 9 `jsr __udivsi3` sites
  (probe math); the CLAUDE.md audit is only meaningful on a **plain** build — verified empty at 1dea72e.

## ★★ SESSION 8 (2026-08-10, 50869ee + aedac9c) — a headless bench, a 35% measurement artifact, −22.7%

**Net for the session: the creature draw went 1638 → 1266 t/step (104 → 80 ms), −22.7%**, confirmed
independently on a real user-flown knock (−23.2% per row).  Two changes: the window mirror (§3,
−21.7%) and the two-pass row (§4, −1.3%).  **The per-cell shaving is now spent — see §6.**

### 1. The bench — `make ALIEN_BENCH=1 PROBES=1` + `amiga/alien_bench.gdb` (⭐ use this, not a flight)
Times ONE synthetic 43-row creature step (`rof_alien_bench`, rof_native.c) from main-loop context
at boot, seeded from the game's own frame tables.  **50 seconds headless, and it repeats to the
EXACT tick** (1638/1638/1638, 1283/1283) — a precision instrument, unlike anything else in this
project.  `ALIEN_BENCH_SPLIT=1` adds per-phase brackets, but its six `rof_subclock` calls per row
cost ~19% of the step: **take proportions from a SPLIT run and absolutes from a plain one.**
⚠ It proves COST, never APPEARANCE — the creature still needs eyes on it.

### 2. ⚠⚠ Every jump-scare figure ever recorded was inflated ~35% by its own instrument
`rof_alien_crwrite` — the capture probe — runs **per CELL** (731×/step) and exists only in PROBES
builds.  Measured: **826 ticks/step, ~35% of what a probe build calls "the draw"**.  Sessions 4-7
all measured probe builds, so **the shipping game has always been materially faster than the doc
said**, on top of the real wins.  The bench excludes it (`&& !defined(ROF_ALIEN_BENCH)`) and so
measures the shipping shape.  ⭐ Generalisable: *price the probe before believing the profile* —
a per-item probe on a per-item loop can outweigh the item.

### 3. The win — one window per row instead of a map per cell (−21.7%)
The mirror re-derived, per cell, what is constant per ROW.  `b = figB0 + y` (y in [0,16], figB0 in
(-96,96)) wraps at most once, and **figB0 in [40,79] draws nothing** (past the 40-byte bitmap,
short of the wrap) — which makes the wrapped and unwrapped cases mutually exclusive, so the row is
CONSTANT and the drawn cells are ONE CONTIGUOUS y-WINDOW.  Row, bases and the four dirty-extent
compares all hoist out; a cell is now two table reads, three stores and two compares.
**Proven off-target first** — `tools/alien_mirror_test.c`, all 65536 dstRow × 17 y = 1,114,112
cases, 0 mismatches ([[rasterizer-restructure]]'s method).

| bench, shipping shape | before | after |
|---|---|---|
| step (43 rows) | 1638 t = 104 ms | **1283 t = 81 ms** (−21.7%) |
| the 17-cell loop | 1082 t | **779 t** (−28%) |

Split after the change (SPLIT build): **loop 779 t (52%) · fills 375 t (25%) · clear 64 t (4%)**,
rest instrument.  A cell is still ~480 cycles, so the loop remains the target.

### 4. The two-pass row (aedac9c) — right codegen, wrong size: −1.3%
Pass 1 walks the 17 cells doing only the faithful work and records each `v`; pass 2
(`alien_mirror_flush`) mirrors the window in one walk and publishes the extents.  Safe to reorder:
the overlay is chip RAM outside `mem[]`, nothing reads it until `renderFlightDirect` composites,
and each y hits a distinct slot.  The never-taken `$8B` self-modify branch flushes the current
segment before re-windowing, so a mid-row pointer change still draws what the per-cell form would.

The codegen came out exactly as intended — alone in its own function nothing else is live, so GCC
emits `(a0)+` reads, three post-increment stores, both table bases pinned across iterations and a
×4 unroll; **the cell loop went 779 → 434 t** and the function shrank 224 bytes.  **But the flush
costs back 290 t of the 345 t saved, so the row is only −1.3%** (1283 → 1266 t/step, reproducible
to the tick).  ⭐ **The lesson: the mirror was never ~40% of the cell — that was my read of the
disassembly, not a measurement.  Price a block by ABLATION before restructuring around it.**

Kept (user's call): the win is real, and pass 1 is register-rich for the first time, which is what
makes the remaining pass-1 shaves possible at all.

### 5. Tried, measured, REJECTED — do not re-open without new numbers
**Folding `kModeDP1`/`kModeDP2` into one 512-byte table + addressing plane2 as `p1[40+y]`** (both
free a register, aimed at the stack spills GCC emits per cell): **+6.5%** (1500 → 1597).  Reverted.

### 6. What is left, ranked — ⛔ the shaving is spent; only #3 can reach 1×
Split after the two-pass (SPLIT build; its brackets inflate the total, take proportions only):
**pass1 loop 434 · fills 425 · flush 290 · clear 117**.
1. **Pass-1 shaves, ~3% and ~1-2%** — hoist `bus_read`'s hardware-range test out of the loop (GCC
   emits it per cell and parks the RAM path out of line), and walk the cell buffer instead of
   `mem[0x8F + y]`.  Both only became possible now that pass 1 has registers to spare.
2. **The fills, ~25%** — ~226 cycles per source byte through `bus_read` + `reorder_cell_bits` +
   volatile cell writes.  Fragments into ~1% pieces; no single idea is sized above that.
3. ⭐ **Pre-rendered chip-`Bitmap` creature frames** (session 6's option (c)) — the ONLY remaining
   idea that removes the per-cell CPU work instead of shaving it, and the only one that could reach
   the faithful 1565 t.  The animation draws from a bounded set of frame-table combinations, so each
   distinct frame could be composed once and blitted thereafter.  ⚠ Real design risk: the frames are
   composed from FOUR field sources through the `$BE00` shape table, and the set has to be *proven*
   bounded before caching it.  Everything in 1-2 together is maybe 6% and lands the step at ~1.7×.

### 7. OPEN #2 (COLOUR) — ⛔ NOT A BUG. The premise was wrong and the creature's hue is already faithful (2026-08-10)

**The old text of this item said:** *"the creature renders in the viewport pens 0-3 (terrain palette), so
likely the WRONG hue … wire `$0047` → the copper viewport pens during the knock."* ⛔ **Both halves are
retracted.** Traced statically end-to-end:

- **`$0047` is `colpf0_value`, and it never reaches the viewport on the Atari.** Its only consumer is
  `set_colpf0_from_flag $47A3`: `A = (Y & $20) ? mem[$0047] : $CA`, which falls into
  `save_color_clear_y_bit5 $47B2` → **`$00D8` = the top-bar / cockpit-message text colour**, plus a
  direct `$D016` (COLPF0) poke from main-loop/VBI context. That poke governs only the scanlines above
  the first DLI (the mode-6 top bar) — **DLI `$4A1F` reloads COLPF0 from `$00DD` for the viewport on
  every frame** (`rof_manual.c`), so a main-loop COLPF0 write can never colour the creature. Wiring
  `$0047` to the viewport pens would have been an *invention*, and it would have recoloured the frozen
  terrain too (creature and terrain share the four mode-D pens).
- **The creature is drawn in the viewport pens BY DESIGN, and that is faithful.** It is a mode-D bitmap
  blitted into the viewport field, so its pens ARE the viewport pens: value0→COLBK`$00DC`,
  1→COLPF0`$00DD`, 2→COLPF1`$00DA`, 3→COLPF2`$00DB` — exactly what `updateFlightCopper` already maps to
  `color00-03`. There is no separate creature palette to wire.
- **★ The actual defect: during the knock the Amiga never refreshed the copper colours at all.**
  `renderFrame`'s "lighter knock render path" (added for perf) jumped straight to `renderFlightDirect()`
  and returned — skipping `updateFlightCopper` — so **the entire flight palette was frozen at whatever
  the last pre-knock frame published**, while the Atari's DLI chain keeps reloading all of it every
  frame of the scare.
- **⛔ `$0047` has NO live consumer during the knock — MEASURED, and this retracts my own first answer.**
  I predicted the flight VBI's "draw" half would keep feeding it through: that branch is
  `else if (timer_or_counter != 0) set_colpf0_from_flag_core(mem[$0044]);` (`vbi_handler_flight`, the
  `$00C8` parity branch, ungated by `joystick_saved`, so it *does* run with systems off). But the
  user-flown capture shows **`$0044 = 00` through the knock**, so the branch never fires: `$00D8` sits
  at the `$CA` constant and `$0047` (`$29` in that capture — not the `$24` the old note quoted, and
  none of the `$6D/$70/$D8` cycle either) is simply **dead** for the whole scare. `alien_attack_tick`,
  which is what cycles `$0047`, runs only on the `$0633` BOARDING path — never during the knock.
  ⇒ There is nothing to wire from `$0047`, by either route.

**MEASURED VERDICT — the creature's colour is already FAITHFUL; OPEN #2 is not a rendering bug.**
Knock capture: pens `$DA=10` (pen2 dots, near-black) · `$DB=b8` (pen3 highlight, bright green) ·
`$DC=14` (pen0 body, dark brown) · `$DD=2a` (pen1 sky, salmon), `valOR=$ff` so the blit uses all four
mode-D values. Those are exactly the four bytes DLI `$4A1F` loads into COLPF1/COLPF2/COLBK/COLPF0, and
exactly what `updateFlightCopper` maps to `color02/03/00/01`. **The Amiga shows the Atari's colours.**
And `g_alPenChg = 0 of 8 knock frames` ⇒ the palette does not move during the scare either.

**FIX (`RescueOnFractalus.cpp`, `renderFrame` knock path) — correctness-only, NOT a visible change:**
refresh the flight copper's colour slots
during the knock — `if (flightCopper && flightCopperInstalled) updateFlightCopper(false);` before
`renderFlightDirect()`. Only the *colours* come back; `deriveRenderSignals()` (the six per-frame sprite
rebuilds that made the light path worth having) stays skipped. Cost is ~15 `mem[]` byte reads plus
poke-on-change — nothing against the ~80 ms creature draw. Amiga-only file ⇒ `make validate` and the SDL
build are untouched; plain + `PROBES=1 FORCE_ALIEN=1` both build clean, softmul audit empty.

It is worth keeping anyway: the Atari reloads the whole palette from `mem[]` every frame of the knock,
so a frozen copper is a latent trap for anything that ever *does* move a pen in there (a future
`animate_clear_colors_timed` port, the death-cinematic ramp, an ESC pause during a rescue). It costs
~15 byte reads. But it fixes nothing you can see today — do not credit it with one.

**Probe (`diag_alien.gdb`).** Prints the pens **as published to the copper** beside the live `mem[]`
ones, plus `g_alPenCalls` / `g_alPenChg`. ⚠ **Read them together:** a refresh that never ran reports
`chg = 0` too, so only `calls == knock frames && chg == 0` means "the palette is static"; `calls == 0`
means `flightCopperInstalled` was false and the zero is vacuous ([[feedback-native-twin-validation-gaps]]
§6). The first capture predated `g_alPenCalls`, so its `chg = 0` is one re-run short of airtight.

⚠ Also fixed in the script: the `wrapper` figure printed `536870149` (unsigned underflow). `g_alTHud`
counts the **4 un-bracketed `alien_creature_animate_draw` calls `alien_knock_setup_loop` makes before
the timed loop**, while `g_alTDraw` brackets only the loop — so on a short capture `hud > draw`
(here 27913 vs 21810, `hud/draw = 127%`, 54 calls/step against the true 44). The hud-vs-wrapper split
is only meaningful on a full ~50-step knock.

**Still genuinely open in this area (known, documented, NOT this fix):** the alien mask treats mode-D
value 0 as transparent (§6/session 6). The Atari blit is `(mask & field) | cell` written OPAQUELY, so
value-0 pixels inside the creature rect overwrite terrain with COLBK; on the Amiga the frozen terrain
shows through instead. Session 6 chose this deliberately (the Amiga sheds the field body, so the read
half of that AND is blank) and the user confirmed the result looks right — reopen only if the creature
ever reads as "see-through".

**OPEN #3 — >1 MINUTE to REACH the alien.** Separate from the knock: the rescue APPROACH (colour sweeps
`animate_clear_colors_timed`/`clear_colors_sweep_5x` + the figure zoom `animate_zoom_sequence`) + general
slow flight rendering. NOT fixed by the `$7F85` twin. Its own follow-up.

**Probe infra (all committed, Amiga+PROBES):** `g_al*` in rof_native.c (creature capture, pen/timing,
composite counters), read via **`amiga/diag_alien.gdb`**. Build `make PROBES=1 FORCE_ALIEN=1`; test with
airlock CLOSED (do NOT set FORCE_AIRLOCK). `g_alTDraw/g_alTRender/g_alKnockFrames` = the per-step timing.

**(Superseded) original NEXT plan — now done:** re-gate the probe to `$0632` + hook `$80C5` + mirror the
masked writes into the flight planes → all implemented; creature renders. Remaining = perf + colour above.

## Rename cluster — APPLIED (symbols.csv, 2026-07-26)
The alien jump-scare names are now canonical in `disasm/symbols.csv` (and everywhere the
transpiler propagates them):
- `$7EC7` `alien_knock_setup_loop` — seeds the creature-animation state
  (`$0632/$005E/$005F/$2921/$2924/$2930/$2931/$0635/$0638/$0639`) then loops
  `alien_creature_animate_draw` per frame while systems-off (the airlock-CLOSED knock).
- `$7F85` `alien_creature_animate_draw` — steps the frame tables `$81E2/$81E8/$820A` AND draws
  the creature via `$80C5` (NOT an SFX sequencer).
- `$80C5` `alien_shape_blit` — masked 17-byte-wide bitmap blit into the mode-D viewport field
  (`$BE00` mask, `($8D),Y` write, row stride `$60`, row addrs `$073D`/`$0793`).
- `$8105/$811F/$8138/$8168` `alien_field0_fill`..`alien_field3_fill`; `$8181` `reorder_cell_bits`.
- `$003C` `airlock_state` (0=closed → jump-scare; nonzero → boarding/reveal);
  `$281E` `figure_is_alien` (1 when marker `$80`); `$0632` `alien_knock_active`;
  `$2927/$2928/$2929` `alien_field0_limit`..`alien_field2_limit`; `$7AB8` `alien_attack_tick`.
