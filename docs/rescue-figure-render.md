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
