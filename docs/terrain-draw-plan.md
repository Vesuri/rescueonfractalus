# Terrain draw perf — implementation plan (flight scene 7, the #1 cost)

Goal: cut `terrain_draw_frame $A31E` (≈82 ms/frame, the single biggest flight cost) **and**
eliminate the separate `mem[$1070]`→bitplane convert pass. User-chosen scope (2026-06-24):
the **full** rework — native dot-plot + blitter fill + skip conversion, **and** optimise the
fractal subdivision math (the dominant ~84%).

## Measurement (2026-06-24, headless beam probe `make PROBES=1` + `diag_run.sh 85`)

Object-loop sub-phase split (`g_tdSubdiv` / `g_tdProjPlot`, beam ticks, ISR not subtracted):

| Phase | ticks | share | what |
|---|---|---|---|
| `tdSubdiv` | 354326 | **~84 %** | `terrain_subdivide_column` recursion (midpoint-displace + the per-column ridge `PLOT`) |
| `tdProjPlot` | 68822 | ~16 % | `project_terrain_points` + `terrain_plot_object` (towers/pilots via `raster_fill_region`) |

`g_fDraw` whole-bucket ≈ 178010 ticks (ISR-subtracted). The `mem[$1070]`→bitplane convert
(`renderViewportModeD`) is a SEPARATE per-frame pass (not in `g_fDraw`) — **its cost is not yet
isolated** (Stage 0 below). ⚠ terrain-dependent, ±30 % run-to-run; trust large deltas only.

## Pipeline today

```
terrain_draw_frame $A31E  (per flight frame, cpu.X = level half index)
 ├─ object loop over $B67C draw order:
 │   ├─ project_terrain_points $A11F   (world→screen projection)         ┐ tdProjPlot
 │   ├─ terrain_plot_object   $A63B → _a/_b → raster_fill_region $AB9A   ┘ (objects)
 │   │      └─ plots tower/pilot cells into mem[$1070] via terrain_clip_row_top→plot
 │   └─ terrain_subdivide_column $B172  (fractal midpoint recursion)      ┐ tdSubdiv
 │        ├─ terrain_midpoint_displace $B2CC                              │ (terrain
 │        └─ terrain_column_rasterize  $B33D                              │  ridge)
 │             ├─ interpolates per-column heights  → $260E[X]  (== yForX) │
 │             └─ PLOT(): OR $BC00[X] into mem[$1070] at row $260E[X]     ┘  (ridge dot)
 └─ game-state tail (altitude/scroll/spawn)
        ↓  (separate pass, every flight frame)
renderViewportModeD(srcBase=$1070, stride=96, rows=47)   ← the convert to ELIMINATE
   full mem[$1070] → terrainBitmap (3bp interleaved, stride 120 = 40+40+40)
   via kModeDP1[256]/kModeDP2[256] LUTs + per-4-byte-group shadow compare.
```

## Key assets already present

- **`$260E[X]` = per-column terrain height = the `yForX` table** the user's asm loop indexes.
  Clamped to `$97`; `$FF` = off-top/filled. Produced by `terrain_column_rasterize` interpolation.
- `$BC00[X]` per-column bit mask (sub-column position in the byte), `$BD00[X]` byte offset,
  `$28CA/$28FA` row→bitmap-address table. (These drive the current `PLOT`.)
- **Amiga target:** `terrainBitmap` 3bp interleaved, **stride 120**, 43 visible mode-D rows
  (×2 line-doubled by `FlightCopperList`). The user's `multiplyBy120` = Y→row-byte-offset LUT
  (120 = this stride). `kModeDP1/P2` decode LUTs in `RescueOnFractalus.cpp`.
- **Blitter infra exists:** `framework/AmigaHardware` — `blitterClear/Copy/Fill/Wait` + async
  queue. Descending OR-row-down fill (BLTCON1 DESC, A=src row, D=dst row+1, minterm OR) feasible.
- Planet path precedent: dirty-row extent (`g_planetRowLo/Hi`) + `ViewportCopperList`/poke-only
  copper. The flight path currently does a FULL convert each frame (no dirty rows).

## Target architecture (user-confirmed)

1. Keep the faithful subdivision producing `$260E[]` (byte-validatable), but **optimise it** as a
   68000-tuned native twin (it's the 84 %).
2. Replace ridge `PLOT`-into-`mem[$1070]` **and** `renderViewportModeD` with a native Amiga step:
   read `$260E[]` (yForX) → plot ridge dots straight into `terrainBitmap` bitplanes via the tight
   `multiplyBy120` LUT loop → **blitter fill-down** (OR each row onto the next) for the body.
3. No `mem[$1070]` round-trip, no convert pass.

## Execution stages (each independently verifiable)

- **Stage 0 — measure the convert win (do FIRST).** Add a beam probe around the flight
  `renderViewportModeD` call (a `g_fConvert` accumulator like `g_fDraw`) so we know the concrete
  ms that eliminating the convert buys. Cheap; grounds the rest. `diag_run.sh 85`.

- **Stage 1 — native dot-plot from `$260E[]` → bitplanes (Amiga-only, behind a flag).** New
  renderer reads `$260E[0..$D4]`, maps each column→(byteCol, 2-bit mask) and height→`multiplyBy120`
  row offset, ORs the ridge dot into `terrainBitmap`. Run it ALONGSIDE the existing convert first
  and visually diff (FS-UAE `run.sh`); the ridge must match.

- **Stage 2 — blitter fill-down for the body.** Replace the existing fill with a descending
  blitter OR (row N → row N+1) over the terrain band. ⚠ **OPEN:** locate the current terrain-BODY
  fill first — `raster_fill_region` is per-OBJECT; the ridge `PLOT` is one pixel/column. The body
  fill mechanism is unconfirmed (user to point at the routine, or pin via runtime dump of
  `mem[$1070]` mid-flight: is it filled or outline-only?).

- **Stage 3 — cut the round-trip.** Stop the 6502 ridge `PLOT` into `mem[$1070]` (subdivision still
  computes `$260E[]`); drop `renderViewportModeD` for flight. ⚠ **Complication:** objects
  (`raster_fill_region`/`terrain_clip_row_top`) and any terrain jitter ALSO write `mem[$1070]` and
  rely on the convert. Eliminating the convert requires EITHER porting object plotting to native
  bitplane writes too, OR keeping a reduced convert for the object layer only. Decide here.

- **Stage 4 — optimise the subdivision math (the 84 %).** 68000-tune `terrain_subdivide_column`,
  `terrain_column_rasterize` (the `b380` interpolation loop), `terrain_midpoint_displace`: hoist ZP
  scratch to registers, pointer-walk arrays, drop dead intermediate writes — same recipe as
  `terrain_collision`/`terrain_frame_setup`. **Must stay byte-identical** on `$260E[]` + math state
  via `make validate` (these are already native twins with `__t6502` oracles).

## Faithfulness / validation note

`$260E[]` (heights) + all subdivision math stay byte-validatable (`make validate`). The bitplane
ridge+fill output becomes **visual-only** (like the copper lists) once it no longer flows through
`mem[$1070]` — verify by eye on FS-UAE, not by oracle diff.

## Files

`src/gen/rof_native.c` (`terrain_draw_frame`, `terrain_subdivide_column`,
`terrain_column_rasterize`, `terrain_midpoint_displace`, `raster_fill_region`),
`src/platform/amiga/RescueOnFractalus.cpp` (`renderViewportModeD`, new dot-plot renderer),
`src/platform/amiga/FlightCopperList.*`, `framework/AmigaHardware.*` (blitter fill),
probes in `PlatformAmiga.cpp` + `diag_timing.gdb`.
