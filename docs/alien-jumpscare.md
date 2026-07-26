# Alien "Jaggi" jump-scare — trace map (2026-07-25, IN PROGRESS / creature draw still unlocated)

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

**★ OPEN #1 — PERF (the blocker, ~7.5× too slow).** Measured per knock STEP (beam ticks, 313=1 frame=20ms;
faithful step = 5 frames = 1565): **draw `alien_creature_animate_draw` ≈ 8438 ticks (~540ms), render ≈ 3284 (~210ms),
wait ≈ 0** (no pacing left — draw+render already blow past 5 frames).

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

**OPEN #2 — COLOUR.** The creature renders in the viewport pens 0-3 (terrain palette), so likely the WRONG
hue. The attack colour is `$0047` (`$6D/$70/$D8` cycle, set by `alien_attack_tick $7AB8`). Pens during the
knock (measured): `$DA=10 $DB=b8 $DC=14 $DD=2a`. Wire `$0047` → the copper viewport pens during the knock
so the alien shows in its proper colour. Do AFTER perf (shape is confirmed; colour is polish).

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
