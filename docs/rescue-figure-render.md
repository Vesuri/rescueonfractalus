# Pilot-rescue "walk-to-airlock" figure render — OPEN bug + investigation plan

Status (2026-07-21): the committed dirty-rect render (`2158300`) **works** — entry (systems off →
pilot zoom) is clean, the pause animates, perf is fine. The **one open bug** is a **1-frame terrain-dot
dropout on the RESUME frame** (systems back ON → first flight frame shows terrain silhouette + sky but
NO plane2 dots; self-corrects the next frame). This doc is the handoff for fixing it *properly next
session* — two blind attempts regressed it (see "What NOT to do"), so fix it with measurement.

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

## What does NOT work (both regressed into "modulo-like" interleaved shear — do NOT repeat)

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
