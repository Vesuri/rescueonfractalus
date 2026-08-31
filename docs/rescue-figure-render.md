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

## ⭐ The walk animation SLOWS DOWN as the pilot approaches — measured 2026-08-31

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
`a800dumps/rescue_pilot.a8s` ($0050/$0051 = `$03D0`, `$00B3` = `$5C`, shape ptr `$7E5B`, x/y in
window) — worth doing, because a shape pointer that landed in `$D000-$D7FF` would have routed every
cell through a hardware `bus_read` and faked the whole result.
⚠ The harness machine is `A500+` **with `--fast_memory=8192`**, so `mem[]` and code sit in
uncontended fast RAM while the overlay is chip. On a bare A500 these figures get worse.

✅ **APPEARANCE CONFIRMED (user, 2026-08-31): both the pilot AND the alien look correct** after the
rewrite. The alien is the load-bearing half of that check — `FORCE_ALIEN` makes the rescued figure
the alien, so it drives the *same* `draw_scaled_shape` with a different shape source, and it
exercises the overlay composite the creature and the walk share
(`docs/alien-jumpscare.md`). Together with the identical overlay fingerprints below, the Amiga-only
`ROF_PLOT_FIG` arm — which `make validate` cannot reach — is covered by both a mechanical and a
visual check.

### Measured, before -> after the clean-C rewrite of both routines

Ticks are ISR-subtracted raster lines; 313 t = 1 frame = 20 ms. `raw` includes the ISR firings that
land inside the draw, which is what the RTCLOK budget actually competes with.

| step | figure | draw before | draw after | raw before | raw after |
|---|---|---|---|---|---|
| `$0600` far | 2 rows | 91 t (6 ms) | **37 t (2 ms)** | 241 t | 37 t |
| `$0300` | 4 rows | 180 t (11 ms) | **79 t (5 ms)** | 264 t | 79 t |
| `$0200` | 8 rows | 317 t (20 ms) | **178 t (11 ms)** | 449 t | 262 t |
| `$0180` | 11 rows | 504 t (32 ms) | **259 t (16 ms)** | 728 t | 343 t |
| `$0100` near | 17 rows | 1059 t (**67 ms**) | **553 t (35 ms)** | 1656 t (**105 ms**) | 861 t (**55 ms**) |

**The near-figure draw halved, and its wall time went from 132 % of the 80 ms budget to 69 %** — so
it is now absorbed by the wait at every scale, which is the condition for a constant step rate.
Binary: 294,464 -> 293,904 B; the two routines 2,240 -> 1,682 B (`plot_clipped_pixel` 806 -> 110 B,
GCC specialises it now that the source value is a parameter rather than `cpu.A`).

### What the win came from (and what is left)

Per-plot cost barely moved: **984 -> 901 cycles per figure pixel** (-8 %). Essentially the whole
win is in `draw_scaled_shape`'s per-cell body: the row offset hoisted out of the column loop (the
6502 re-read it per cell), the `$7DD3` field selector as a closed form instead of three DEX/LSR/LSR
passes, the shape byte read from `mem[]` instead of through `bus_read`, and the two loop
accumulators kept in 16-bit locals with one write-back instead of per-iteration `mem[]` traffic.

⇒ **After the rewrite the draw is ~100 % plot cost** (276 plots x 901 cycles ~= the whole 553 t), so
`plot_clipped_pixel` is the only remaining lever.

### ⏳ FUTURE WORK — 35 ms for a small figure is still far too much (user, 2026-08-31)

Deferred, not closed: the user's verdict is *"35 ms is still a rather massive amount of time to
render a small figure on screen but let's mark this as future work."* At the `$0100` floor the
figure is ~17 rows x 12 pixels — call it ~204 cells — so **35 ms is ~170 us, about 1,200 68000
cycles, per 2-bit pixel.** The 80 ms budget is met and the reported slowdown is gone, so there is
no correctness or pacing pressure; this is purely "the number is absurd".

⚠ **This is NOT the closed flight-perf effort** (`CLAUDE.md` §Performance). The zoom is a cold,
once-per-animation-step routine that runs while the terrain is FROZEN and the flight loop is parked
inside `animate_zoom_sequence` — it shares no code with the terrain pipeline. Working on it does not
reopen anything.

**Candidate 1 (largest, and provably faithful): hoist the y-derived work out of the per-pixel call.**
Everything `plot_clipped_pixel` derives from y is **constant for a whole row** — y changes only in
`draw_scaled_shape`'s outer loop, never in the column loop, and the 14 leading / 2 trailing
transparent plots share the row's y too. Per pixel it currently redoes:
* the `$97 - y` row index and both `row_base_lo/row_base_hi` table reads,
* the 16-bit `readPtr + $30`,
* **four `mem[]` stores** (`$0080/$0081/$00C1/$00C2`) that write the *same value* 12-15 times a row,
* the `y < $6C || y >= $97` half of the window test.

A row-batched plotter would do all of that **once per row instead of ~15 times**, and it is
byte-identical by construction: the stores are last-write-wins with an identical value, so the
final memory state cannot change. ⚠ What must stay per pixel: the x cursor advances on every call
including clipped ones, and both the `x` window test and the `col >= $00B3` test depend on x. So the
split is "y-derived work per row, x-derived work per pixel" — not "clip per row".

**Candidate 2: the three chip-RAM read-modify-writes in `ROF_PLOT_FIG`** (mask + plane1 + plane2),
i.e. 6 chip accesses per pixel, contending with display DMA. Accumulating a row's mask/plane words
in registers and writing whole words per row-span would cut this to ~3 word writes per byte column.
Naturally falls out of Candidate 1's restructure.

**Candidate 3: pre-rendered figure frames.** The same idea already filed for the jump-scare creature
(`docs/alien-jumpscare.md` §6.3): the animation draws from a bounded set of (phase, step)
combinations, so each distinct frame could be composed once into a chip `Bitmap` and blitted
thereafter, removing the per-pixel CPU work rather than shaving it. ⚠ Same design risk as there —
the set must be *proven* bounded first. `step` walks $10 at a time to a floor and phase is 0..7, so
the set is finite but not small; count it before believing in it.

**How to work on it:** `make PILOT_BENCH=1 PROBES=1 PROFILE_NORING=1` + `amiga/pilot_bench.gdb` is
the instrument (reproduces to ~1 %), `make validate FN=plot_clipped_pixel` / `FN=draw_scaled_shape`
is the faithfulness gate, and the bench's `figHash` column is the ONLY check on the Amiga-only
`ROF_PLOT_FIG` arm — `make validate` runs the host build and never reaches it. Record the before/
after in the table above.

⚠ The user's original hypothesis — "the figure is drawn to `mem[]` and then converted to bitplanes"
— is **not** what costs: `plot_clipped_pixel`'s Amiga arm already drops the `($80),Y` read and the
`($C1),Y` read-modify-write entirely and mirrors straight into the overlay planes. The cost was the
6502-ABI scaffolding around each pixel.

### The faithfulness gate

Both routines are `make validate`d twins with real fixtures. `plot_clipped_pixel`'s fixture only ever
generated **in-window** coordinates with a high `$00B3`, so all three clip paths were untested — it
now runs half its 50,000 cases fully random to cover them, because those paths still advance the x
cursor and still publish the row pointers, which `draw_scaled_shape` depends on for a fully-clipped
row. Both: **0 mem mismatch.** The cpu diffs are total and expected: the clean versions no longer
compute 6502 flags. Verified safe — all five `JSR $7D38` sites are inside `draw_scaled_shape` and
each overwrites Y or A before reading either, and `animate_zoom_sequence` reads neither after the
call, so even the `TAY` at `$7D71` publishes nothing (the `cpu.Y` store was therefore dropped).

## Scene / trigger

- Rescue figure = the approaching pilot/alien drawn as a **bitmap** into the mode-D flight field by
  `plot_clipped_pixel` (via `draw_scaled_shape` ← `animate_zoom_sequence`), NOT PMG. Verified from
  `a800dumps/rescue_pilot.a8s` (which is AT the rescue state: `$288F`… actually `$003E=01`, `$003D=03`).
- Amiga trigger condition (in `renderFlightDirect`): **`rescueFigure = (mem[0x003E] != 0 && mem[0x003D] >= 3)`**.
- Amiga capture: `plot_clipped_pixel` mirrors each opaque figure pixel into a scratch overlay via
  `ROF_PLOT_FIG` (rof_native.c), cleared per shape by `ROF_CLEAR_FIG`. `g_figP1/P2/M` + `g_figRowLo/Hi`.

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
- `src/gen/rof_native.c`: `ROF_PLOT_FIG` / `ROF_CLEAR_FIG` (~165); `g_flightDotPlane`,
  `g_flightTerrainFresh`.
- Double-buffer discipline: see CLAUDE.md "⚠ Swapping copper bitplane POINTERS must happen in the VBI
  ISR" — torn/wrong flips show as exactly this modulo shear.
