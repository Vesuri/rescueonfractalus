# Pilot-rescue "walk-to-airlock" figure render — RESOLVED (resume-frame dot dropout)

Status (2026-07-21): **FIXED + user-confirmed.** The resume-frame 1-frame terrain-dot dropout is gone,
the pilot zoom stays clean, resume has dots, no crash. Root cause + fix below; the investigation
history (how it was measured) is kept because the measurement technique + the two dead-end approaches
are the durable lessons.

**2026-07-22 follow-up — SECOND resume bug (systems toggled with NO pilot animation) FIXED + user-confirmed
(commit 49172b4).** `$3E` (rescueActive) also goes active when systems are switched off with no pilot in
range: no walk runs (`$3D` never reaches 3 → `rescueFigure` never true), so `s_clean` is NEVER snapshotted
that pause and holds STALE data from a prior rescue (or is empty). The `$3E`-edge restore fired anyway on
resume, copying that stale/empty plane2 over the correct fresh dots → resume showed a stale/wrong (or empty)
frame. Fix: gate the arming on `s_cleanValid` (`if (s_prevRescueActive && !rescueActive && s_cleanValid)`),
so the restore fires ONLY when a real rescue-figure pause captured `s_clean` this cycle. In the no-animation
case the clear was never suppressed (`s_flightRescuePause` stayed false), so the normal render path already
draws correct dots — no restore needed. **Durable lesson: `$3E`/rescueActive is NOT "pilot rescue" — it is
"systems off," which happens with or without a pilot; only `s_cleanValid` distinguishes a real walk pause.**
(Bisect ruled out the same session's `terrain_plot_pixel`/`plot_clipped_pixel` field-RMW-drop commits —
this bug was pre-existing, present at fc16a15.)

**Root cause (confirmed by a live gdb ring-buffer capture, `amiga/diag_rescue.gdb`):** on the single
frame the rescue ends, `flightKickBackClear` had suppressed the off-screen buffer's pre-clear for the
whole pause (`flightClearPending==null`), so the terrain rasterizer ORed the resume frame's fresh dots
into an un-cleared buffer and `renderFlightDirect`'s safety clear then wiped BOTH planes — measured: the
painted buffer's plane2 byte-sum collapsed ~10209→1290 for exactly one displayed frame, then
self-corrected once the pre-clear re-armed.

**Fix (`renderFlightDirect`, keyed on the `$3E` latch — NOT `rescueFigure`):** detect the `$3E`
(clear_colors_done) nonzero→zero edge = the true rescue END, and latch a **one-shot plane2 restore**
that runs on the next terrain-rendering frame: copy the FROZEN terrain dots captured in `s_clean` at
pause entry into the painted buffer's plane2 (rows 0-42). The ship is stationary across a rescue so the
frozen dots are a byte-identical stand-in for that one frame; `s_clean` is the pre-figure snapshot so no
ghost. Touches ONLY the normal path — no flip-skip, no change to the pause dirty-rect. **Why `$3E` and
not `rescueFigure`:** `rescueFigure = ($3E!=0 && $3D>=3)` also goes false on the mid-zoom frames where
`pilot_render` drops `$3D` to 2 (`$7a37`), so any recovery keyed on `rescueFigure`/"was in pause" fires
DURING the zoom and corrupts it (that was the failing attempt). `$3E` is a clean single latch (set once
at rescue start, cleared once at end — measured `01→00` one edge), so the edge fires exactly once at the
real resume. Also hardened: `s_clean` is now `alignas(4)` (the existing snapshot casts it to `uint32_t*`
and long-accesses it — a 68000 bus error if a BSS-layout change pushes the byte-array to an odd address).

## ⛔ The walk animation SLOWS DOWN as the pilot approaches — FIXED, and the PERF IS CLOSED

**✅⛔ CLOSED by the user, 2026-09-01: "mark this perf adequate now". Do NOT re-open the approach
figure's draw cost, and do not re-open a closed candidate for a bigger margin.** The near-figure
draw is **262 t = 16.6 ms, 20 % of the 80 ms per-step budget** (was 1059 t = 67 ms = 132 %, i.e.
draw-bound and visibly slowing). Two clean-C passes, both `make validate`d at 0 mem mismatch with
identical overlay fingerprints, and the appearance confirmed on both figures. The table, what paid,
and what was tried and reverted are below — kept because the *method* generalises, not because
there is work left here.

**User report:** the run animation visibly slows on the 68000 as the pilot gets closer (larger on
screen); it does not on the Atari. This was real, it is a cost-scaling problem, and the first two
explanations offered for it (PAL-vs-NTSC's 20 %, and RTCLOK wait quantisation) were both wrong —
they predict a *constant* factor, not one that tracks the figure's size.

### Why the cost scales with the figure's area

`draw_scaled_shape`'s loop bounds are not constant: it draws `$1200/step` rows x `$0C00/step`
columns, where `step` = `$0051:$0050` is an INVERSE scale. `animate_zoom_sequence` walks `step` down
by `$10` per zoom frame to a `$0100` floor, so the plotted cell count grows as 1/step^2 —
**29 plots at the far end, 284 at the floor, ~10x.**

### The budget, and why it tips over

Each animation step is `wait until RTCLOK_LOW reaches 4` (= 4 frames = 80 ms at 50 Hz) and *then*
the draw. RTCLOK keeps running during the draw, so a draw shorter than the wait is **absorbed and
invisible**; one longer than it makes the step draw-bound. Hence a slowdown that appears only as the
figure grows past the point where the draw exceeds 80 ms.

### The instrument: `make PILOT_BENCH=1 PROBES=1 PROFILE_NORING=1` + `amiga/pilot_bench.gdb`

The pilot path still has **no headless harness** (see the section below), so `rof_pilot_bench`
(`rof_native.c`) times ONE synthetic `draw_scaled_shape` per step size from flight main-loop context
at iteration 40, plus the bare per-plot cost amortised over 1000 calls under a single bracket. It
reproduces to ~1 % at the near end. Under the flag `ROF_ALIEN_PLOT`/`ROF_ALIEN_DRAWSHAPE` compile
out, so it measures the shipping shape — a per-plot probe on a per-plot loop is exactly what
inflated every jump-scare figure by ~35 % (`docs/alien-jumpscare.md` §8.2).

⚠ **What it does NOT do:** it never reaches a rescue, so nothing appears on screen and it proves
COST, never APPEARANCE. Its seeded inputs were cross-checked against the live values in
`a800dumps/rescue_pilot.a8s` (`plot_step` = `$03D0`, `plot_col_limit` = `$5C`, shape ptr `$7E5B`, x/y in
window) — worth doing, because a shape pointer that landed in `$D000-$D7FF` would have routed every
cell through a hardware `bus_read` and faked the whole result.
⚠ The harness machine is `A500+` **with `--fast_memory=8192`**, so `mem[]` and code sit in
uncontended fast RAM while the overlay is chip. On a bare A500 these figures get worse.

✅ **APPEARANCE CONFIRMED TWICE (user, 2026-08-31 after pass 1 and 2026-09-01 after pass 2): both
the pilot AND the alien jump-scare render correctly.** The alien is the load-bearing half of that check — `FORCE_ALIEN` makes the rescued figure
the alien, so it drives the *same* `draw_scaled_shape` with a different shape source, and it
exercises the overlay composite the creature and the walk share
(`docs/alien-jumpscare.md`). Together with the identical overlay fingerprints below, the Amiga-only
Amiga overlay arm — which `make validate` cannot reach — is covered by both a mechanical and a
visual check.

### Measured, across the two clean-C passes

Ticks are ISR-subtracted raster lines; 313 t = 1 frame = 20 ms. `raw` includes the ISR firings that
land inside the draw, which is what the RTCLOK budget actually competes with. The zoom's own budget
per animation step is the 4-RTCLOK-tick wait in `animate_zoom_sequence` = 4 frames = 1252 t = 80 ms.

| step | figure | 6502 transliteration | pass 1: clean C | pass 2: row-invariant hoist | raw now |
|---|---|---|---|---|---|
| `$0600` far | 2 rows | 91 t (6 ms) | 37 t (2 ms) | **24 t (1 ms)** | 24 t |
| `$0400` | 4 rows | — | 77 t (5 ms) | **40 t (2 ms)** | 40 t |
| `$0300` | 4 rows | 180 t (11 ms) | 80 t (5 ms) | **49 t (3 ms)** | 49 t |
| `$0200` | 8 rows | 317 t (20 ms) | 171 t (11 ms) | **91 t (5 ms)** | 91 t |
| `$0180` | 11 rows | 504 t (32 ms) | 259 t (16 ms) | **132 t (8 ms)** | 132 t |
| `$0100` near | 17 rows | 1059 t (**67 ms**) | 553 t (35 ms) | **262 t (16 ms)** | 346 t (22 ms) |

**The near figure went from 132 % of the 80 ms budget to 20 %** — a 4.0x cut overall, 2.1x in
pass 2 alone. Per CELL (the unit the cost actually scales in — take the `$0100`/`$0180` slope, not
a 3-point fit, which is unstable here): **1112 -> ~490 68000 cycles.** Release binary
293,904 -> 295,864 B (+0.67 %). The bench is deterministic to the tick here, so a 3 % move is real.

### What pass 2's win came from

The lever is not instruction count — three separate rounds that each removed real instructions from
the per-pixel body (a byte-column accumulator, the `kModeDP1/2` decode replacing two variable
shifts, the redundant mask accumulator) measured **within noise of each other**, because
`draw_scaled_shape` is register-starved and GCC just moved the spills around. What paid was
removing *work*, not instructions:

1. **`plot_clipped_pixel` split into a per-RUN / per-ROW / per-PIXEL half** (`rof_plotrun`,
   rof_native.c). y changes only in the outer loop, so the row index, both `$073D`/`$0793` table
   reads and the field pointers are computed once per row; the four published pointer bytes,
   `plot_pixel_mask` and both cursors are carried in registers and published once by
   `rof_plotrun_close`. On the Amiga the `g_fig*` pointers and the four dirty extents come out of
   the loop too — they are `uint8_t*`/`int` globals, so a store through any of them forces GCC to
   re-read all of them through absolute-long, ~8 long accesses per pixel. **553 -> 366 t.**
2. **⭐ The per-COLUMN table (`rof_figcol`) — the big one.** Every row of a draw plots the same
   columns at the same x (x is `plot_row_start_x + 1 + c`, reloaded identically each row), so the
   x window test, the byte column, the column limit test, the overlay mask and byte, *and* both
   shape-table lookups (`$7DBB`, `$7DD3` → a shift amount) are **row-invariant**. Built once per
   draw; what is left per cell is one shape byte, one pen byte and the plane merge. **378 -> 291 t**
   (286 t before the `__mulsi3` fix below made the struct 8 bytes).
   ⚠ This goes past the Candidate-1 note below, which assumed the x-derived half had to stay per
   pixel. It does not — x is a function of the column index.
3. **The transparent pad plots folded** (14 leading + 3 per row = 68 of 284 "plots"). On the Amiga a
   transparent plot has no effect beyond the cursor, the source byte and the row-pointer publish,
   all row-invariant, so `rof_plotrun_pad` collapses a run into a few instructions. Landed together
   with pulling the cell loop's hot state (plane pointers, accumulators, extents) into locals:
   **291 -> 262 t** for the pair, not separately attributable.

Two things that measured WORSE and were reverted — both are the same lesson, that the cell loop is
bound by GCC's register allocation, not by the instruction stream:
* **Moving the cell loop out of line** (`noinline`, run state copied in/out per row): **+100 t**
  at first, and **+53 t** even with only the hot fields copied. Inlined with the hot state in
  locals is what wins; `always_inline` on the helpers is load-bearing.
* A 7-byte `rof_figcol`: the cell loop's pointer difference `/ sizeof` became a **`__mulsi3`
  call** — a CLAUDE.md hard-rule violation the objdump audit catches. Hence the explicit `pad`
  byte holding `sizeof` at 8, and an index loop rather than a pointer-difference loop.

### ⛔ Levers NOT taken — recorded so nobody re-derives them, not as a TODO

The user closed this at 16.6 ms. These are what a further pass would have attacked; **do not start
one without the user asking.**
* At `$0100` a cell still costs ~490 cycles for ~10 operations. The disassembly shows three spill
  stores and a reload at the top of the loop, plus four `andi.l` zero-extensions (14 cycles each)
  that are C promotion rules rather than work. A hand-written m68k twin of `rof_plotrun_cells`
  should reach ~200 cycles/cell (~110 t / 7 ms) — the project's normal answer for a loop this
  shape (`docs/asm-migration-plan.md`).
* Merging `kModeDP1/2` into one 512-byte 16-bit table: one word read instead of two byte reads and
  two `lea`, ~20 cycles/cell (~4 %).
* **Candidate 3 (pre-rendered figure frames) stays unattractive** — `step` walks `$10` at a time
  from the far end to the `$0100` floor, so the (phase, step) set is in the hundreds.

### The old Candidate list (1 and 2 are DONE, kept for the reasoning)

**Candidate 1 (done, pass 2 item 1): hoist the y-derived work out of the per-pixel call.**
Everything `plot_clipped_pixel` derives from y is constant for a whole row. Per pixel it used to
redo the `$97 - y` row index and both `row_base_lo/row_base_hi` table reads, the 16-bit
`readPtr + $30`, **four `mem[]` stores** (`$0080/$0081/$00C1/$00C2`) that write the *same* value
12-15 times a row, and the `y < $6C || y >= $97` half of the window test. Byte-identical by
construction: the stores are last-write-wins with an identical value.

**Candidate 2 (done, and it measured as a WASH): the three chip-RAM read-modify-writes**
(mask + plane1 + plane2 = 6 chip accesses per pixel). Accumulating a row's plane bytes in
registers and flushing once per byte column is in (`accB`/`accP1`/`accP2`; the mask byte is their
OR, so it is not accumulated separately) — but on its own it measured 366 -> 373 t, i.e. the saved
RMWs were paid straight back in spills. It only became worth keeping once item 2 above freed the
registers.

### The faithfulness gate

Both routines are `make validate`d twins with real fixtures. `plot_clipped_pixel`'s fixture only ever
generated **in-window** coordinates with a high `plot_col_limit`, so all three clip paths were untested — it
now runs half its 50,000 cases fully random to cover them, because those paths still advance the x
cursor and still publish the row pointers, which `draw_scaled_shape` depends on for a fully-clipped
row. Both: **0 mem mismatch.**

⚠⚠ **`test_draw_scaled_shape` was a near-vacuous green and had to be fixed** (pass 2). It seeded the
coordinates from the flight snapshot, whose `$004F` is `$FB` — so after the divide every plotted x
landed outside the `[$28,$D8)` window and **not one case ever reached the composite**; it tested only
the cursor and pointer bookkeeping. It now forces plotting coordinates on half its cases (a y start
whose row walk sometimes runs off the window bottom, an x start inside the window, and a
`plot_col_limit` that half the time cuts the row short), and adds 300 cases with `step` in
`$0080..$00BF` so the row is wider than `ROF_FIGCOL_MAX` and the **generic per-cell fallback loop**
runs at all. In the GAME that loop is unreachable: `animate_zoom_sequence` is the only caller and
`$7C7F-$7C83` clamps `step` to exactly `$0100` when the `SBC #$10` walk would take the high byte to
zero, so the row is never wider than 12 columns. The fallback exists for generality, and the
fixture is the only thing that reaches it. Proven by a transition test: poisoning the fast path gives 2902 mismatches,
poisoning the fallback 250 of its 300 cases, both zero when restored. **Do not trust either path's
green without that transition test.** The cpu diffs are total and expected: the clean versions no longer
compute 6502 flags. Verified safe — all five `JSR $7D38` sites are inside `draw_scaled_shape` and
each overwrites Y or A before reading either, and `animate_zoom_sequence` reads neither after the
call, so even the `TAY` at `$7D71` publishes nothing (the `cpu.Y` store was therefore dropped).

## Scene / trigger

- Rescue figure = the approaching pilot/alien drawn as a **bitmap** into the mode-D flight field by
  `plot_clipped_pixel` (via `draw_scaled_shape` ← `animate_zoom_sequence`), NOT PMG. Verified from
  `a800dumps/rescue_pilot.a8s` (which is AT the rescue state: `$288F`… actually `$003E=01`, `$003D=03`).
- Amiga trigger condition (in `renderFlightDirect`): **`rescueFigure = (mem[0x003E] != 0 && mem[0x003D] >= 3)`**.
- Amiga capture: `plot_clipped_pixel` mirrors each opaque figure pixel into a scratch overlay via
  `rof_plotrun_pixel`/`rof_plotrun_cells`'s Amiga arm (rof_native.c), cleared per shape by
  `ROF_CLEAR_FIG`. `g_figP1/P2/M` + `g_figRowLo/Hi`.

## How the committed render works (DO NOT casually change this — it is delicately balanced)

`RescueOnFractalus::renderFlightDirect()` `rescueFigure` branch (~line 1465) + `flightKickBackClear()`
(~1714), both in `src/platform/amiga/RescueOnFractalus.cpp`:

1. **Entry**: snapshot the clean frozen frame once (`flightDisplayed->data` → `s_clean`, 47×120 all-3-
   planes). Mark the displayed buffer seeded; the other seeded on first use.
2. **Each pause frame** (dirty-rect): pick `back` = off-screen buffer; if not yet seeded, full-copy
   `s_clean` → `back`; else **erase only the previous figure's row box** (restore plane1+plane2 from
   `s_clean`); then composite the new figure (`s_figP1/P2` over `s_figM` mask); flip via the VBI
   (`flightPendingFlip`/`flightSwapPending`/`flightVblankSwap`).
3. `flightKickBackClear` **SUPPRESSES the per-frame blitter clear** while `s_flightRescuePause` is set
   (leaves `flightClearPending = nullptr`) so the buffers RETAIN the frozen terrain for the erase-box.

The double buffer **flips every pause frame**, so `flightDisplayed` alternates each frame; `back`,
`flightClearPending`, and the per-buffer `s_bufSeeded`/`s_boxLo/Hi` are all tracked to keep the
displayed / written / cleared buffers straight.

## Root cause of the resume-frame dot dropout

On the RESUME frame (`rescueFigure` just went false → normal path):
1. During the pause, `flightKickBackClear` suppressed the clear ⇒ `flightClearPending = nullptr`, and
   the resume buffer holds stale seeded frozen terrain (NOT cleared).
2. The terrain rasterizer runs (main loop) and ORs this frame's fresh dots into that un-pre-cleared
   `back` buffer's plane2 (`g_flightDotPlane`).
3. `renderFlightDirect` normal path hits its safety clear: `if (flightClearPending != back)
   AmigaHardware::blitterClear(bp, 60, 47, 0)` (~line 1567). Because `flightClearPending == nullptr`,
   it FIRES — **after** the dots are already there — wiping all 47 rows incl. plane2.
4. `renderFlightDirect` never repaints plane2 (dots come ONLY from the rasterizer's upstream draw), so
   that one frame shows no dots. Next frame `flightClearPending` is armed again ⇒ clean.

The catch-22: the dirty-rect NEEDS the suppress-clear (to retain frozen terrain for the erase-box), but
the resume needs a PRE-CLEARED buffer. You can't tell during the pause which frame is the last, so you
can't pre-clear the resume buffer from `flightKickBackClear` ahead of time.

## What does NOT work (do NOT repeat)

0. **Keying the resume recovery on `rescueFigure` / "was in the pause" (`s_wasRescuePause`)** — corrupts
   the ZOOM, not the resume. `rescueFigure` flickers false mid-zoom (see the `$3E` vs `$3D` note above),
   so the recovery fires while the figure is animating. The `$3E` nonzero→zero edge is the correct key
   (RESOLVED, above). This is a *different* failure from the two shear regressions below.

## Earlier shear dead-ends (both regressed into "modulo-like" interleaved shear — do NOT repeat)

Both attempts were reverted; neither was committed. Confirmed via **git bisect (user tested each build)**
that the committed code is clean and these uncommitted patches were the cause:

1. **Frame-skip** (`s_wasRescuePause`): on the first normal frame after the pause, skip the paint/flip
   (`return`) + set `g_flightTerrainFresh=0`, so `flightKickBackClear` re-clears and the next frame is
   clean. → modulo shear (corrupted the display).
2. **No-suppress + full-restore**: remove the suppress-clear (clear every frame) and full-copy
   `s_clean` → `back` each pause frame instead of erase-box. → same modulo shear.

**LESSON: any change to the dirty-rect's clear / flip / seed buffer flow desyncs which buffer is being
displayed vs written vs cleared during the rapid per-frame flips of the pause → interleaved/modulo
shear.** The state machine is tightly coupled. Do NOT modify it blind. (Also: `2158300` is NOT broken —
it "worked when it landed" and still does; only the resume-frame dot dropout is real.)

## How to investigate next session (measure, don't guess)

1. **Instrument the rescue path** under `ROF_FLIGHT_PROBE`: log per-frame through a rescue —
   `flightDisplayed` (which of terrainBitmap/Back), `flightClearPending`, the chosen `back`,
   `rescueFigure`, `g_figRowLo/Hi`, and a cheap per-plane checksum of BOTH terrain buffers. Watch how
   the buffers desync across the pause flips and at the pause→normal transition.
2. **Reaching a rescue**: the headless auto-flight (`diag_run.sh`) can't get to a pilot rescue — it needs
   interactive play (fly to a pilot, land, systems off → zoom → systems on). So either drive it
   interactively with the trace over the gdb stub / a probe ring buffer read afterward, OR study the
   6502 timing from `a800dumps/rescue_pilot.a8s` (already at `$003E=01`/`$003D=03` = rescueFigure) on
   atari800 to understand exactly when `$003D/$003E` flip (does `rescueFigure` toggle mid-animation, or
   only cleanly once at the end? that determines whether transition-detection is even safe).
3. **Then** design a fix against real state. Candidate directions (verify against the trace first):
   - Pre-clear the resume buffer safely: figure out the last-pause-frame condition, or restructure so
     the resume frame's buffer is guaranteed clean *before* the rasterizer draws — WITHOUT breaking the
     pause's retain-buffer requirement.
   - Or make the resume frame restore plane2 dots from `s_clean` (frozen dots ≈ correct since the ship
     was stationary) after the safety clear — a one-frame approximation, but touches only the normal
     path, not the pause state machine.
   - Whatever the fix: change as LITTLE of the buffer/clear/flip flow as possible, and re-test entry +
     pause + perf + exit each iteration.

## Key code

- `src/platform/amiga/RescueOnFractalus.cpp`: `renderFlightDirect()` `rescueFigure` branch (~1465);
  safety clear (~1567); `flightKickBackClear()` (~1714); `flightVblankSwap()` (~1696);
  `s_clean`/`s_cleanValid` (~270); figure scratch `s_figP1/P2/M`, `g_figRowLo/Hi` (~259).
- `src/gen/rof_native.c`: `rof_plotrun` + `rof_figcol` (the plot split, above
  `plot_clipped_pixel_core`); `ROF_CLEAR_FIG`; `g_flightDotPlane`,
  `g_flightTerrainFresh`.
- Double-buffer discipline: see CLAUDE.md "⚠ Swapping copper bitplane POINTERS must happen in the VBI
  ISR" — torn/wrong flips show as exactly this modulo shear.
