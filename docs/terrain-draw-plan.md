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

## Rendering model (settled 2026-06-24 by ground-truth Atari field dumps — flight1/2/3.bin)

⚠ CORRECTED — the Atari fills the **SKY**, NOT the terrain. Decoding the real Atari `$1070`
field (3 frames) shows the value distribution: value0 = terrain body, value1 = sky (the FILLED
region, ~100 px/row across the upper rows, tapering down to the skyline), value2 = sparse dots
(texture, 37–220 px), value3 = unused (0). One frame (flight3) had no sky at all (pitched down).

So the layers are:
- **Terrain body (value 0 = color00, lighter brown)** — the **cleared value-0 background**. The field
  is zeroed each frame and left as-is below the skyline. **Never filled — it is free** (the dominant
  colour by default). This is the "lighter-brown filled body" — filled only in the sense of *being the
  background colour*, not by any pixel work.
- **Sky (value 1 = color01)** — the **FILLED region above the skyline**. Drawn by plotting the skyline
  and filling **upward** (skyline row → row 0). This is the only true fill, and the blitter's job.
- **Dots (value 2 = color02, darker brown)** — sparse texture, plotted individually, **no fill**.
- Value 3 (highlight) unused.

Consequence vs the earlier draft: the blitter fill operates on the **SKY bitplane (fill up to the
top)**, not a terrain body plane. The terrain body needs zero fill work.

### Where the fill actually is (traced from flight1.a8s, 2026-06-24)

The sky fill is **NOT in `terrain_draw_frame`** — it's in **`terrain_collision_and_silhouette $AE53`** (a misnomer:
it does collision detection AND the terrain silhouette render), which runs right after the draw in
the flight loop (`game_main_loop`: …setup → clear → `terrain_draw_frame`(g_fDraw) →
`terrain_collision_and_silhouette`(g_fColl)…). Per column (rof_native.c `terrain_collision_and_silhouette`, ~line 4492):
1. **CASCADE** — scan the 48 rows (`$1010` stride `$60`) top-to-bottom for the first non-empty
   cell → row `k` = the skyline (the ridge `terrain_draw_frame` plotted).
2. **WATERFALL** (line ~4515): `for (i=k-1;i>=1;i--){ p-=0x60; *p=0x55; }` — paint `$55`
   (value-1 = sky) into rows **above** the skyline, walking **UP**. ← the sky fill.
3. **RASTER** (`$B12F`): walk **down** from `{$0793:$073D}[k]` ORing the `$BF00`/`$BE00` voxel-mask
   tables → terrain body + value-2 dots below the skyline.

Full pipeline + costs: `terrain_draw_frame` (g_fDraw ~82ms) plots the ridge edge; `terrain_collision_and_silhouette`
(g_fColl ~11-15ms, ALREADY a native twin + optimized) waterfalls sky up + rasters body down;
`renderViewportModeD` (g_fConvert ~30ms) converts field→bitplanes.

So the user's "blitter fill the sky up" = port the WATERFALL; "dots without filling" = the RASTER's
`$BF00`-mask value-2 component. Both currently live in `terrain_collision_and_silhouette`, not the draw.

(Live dump confirmed: near rows fill solid; the `$2090`+ rows 43-46 are the wing-clearance band
`ff/aa/55`, not terrain.)

**Flight terrain palette** (`updateFlightCopper` → `setTerrainPalette(terr0,terr1,$20,$18)`):

| Amiga pen (color0n) | Atari src | role |
|---|---|---|
| color00 | `$00DC` | **terrain body** (lighter brown, salmon→brown fade) — the FILL layer |
| color01 | `$00DD` | sky |
| color02 | `$20`   | **dots** (darker brown texture) — the DOT layer |
| color03 | `$18`   | highlight |

⚠ Subtlety to pin in Stage 1 before coding the renderer: the field is GTIA-10 (PRIOR=$94 — pixel
value selects a register, NOT linear 2bpp), decoded on the Amiga via `kModeDP1/P2` (bit0→plane1,
bit1→plane2) so field value V→Amiga pen V. But "body" mapping to pen0 (no bits set = background)
means the fill model may be inverted (body is the default, sky/dots/highlight drawn over it).
Pin the true value→bitplane→pen relationship with `/atari-dl-analyzer` on a flight savestate (or a
runtime `mem[$1070]`+palette cross-check) as Stage 1's FIRST task — it decides which plane the
blitter fills and which carries the dots.

## Target architecture (user-confirmed)

1. Keep the faithful subdivision producing `$260E[]` (byte-validatable), but **optimise it** as a
   68000-tuned native twin (it's the 84 %).
2. Replace ridge `PLOT`-into-`mem[$1070]` **and** `renderViewportModeD` with a native Amiga step:
   - **Sky bitplane (value 1):** plot the skyline per `$260E[]` (yForX) via the tight `multiplyBy120`
     LUT loop → **blitter fill UP** (OR each row onto the row above, skyline → row 0). This is the
     only fill.
   - **Terrain body (value 0):** nothing — it's the background colour (color00). Free.
   - **Dot bitplane (value 2):** plot the darker-brown texture dots, no fill.
3. No `mem[$1070]` round-trip, no convert pass.

## Stage 1 grounding (2026-06-24, decoded from a800dumps/flight1.bin — DONE before coding)

The field model + column mapping, confirmed from the real flight `mem[$1070]` field + tables:
- **Value→layer→Amiga pen** (mode-D decode `kModeDP1/P2`, bit0→plane1, bit1→plane2): value0=body
  (pen0, the cleared background — FREE), value1=sky (plane1, the FILLED region above the skyline),
  value2=dots (plane2, sparse texture), value3=wing-clearance band only (rows 43-46, not terrain).
- **The ridge PLOT (`$BC00`=`80,20,08,02`) writes value-2** (the high bit of each 2-bit pixel) at the
  skyline row — i.e. the ridge edge is the DOT layer.  The **sky fill (value1) and body come from
  `terrain_collision_and_silhouette`** (WATERFALL up / RASTER down), NOT from `$260E` or the ridge plot.
  So a $260E-only renderer reproduces the body/sky split + ridge dots, but the texture dots + objects
  still come from collision/object plotters → Stage 3's "port those too vs keep a reduced convert".
- **Column→pixel mapping is a perspective fisheye**: `$BD00[X]` = byte offset (4 logical columns per
  field byte), `$BC00[X]` = the 2-bit pixel mask (`80/20/08/02` cycling = pixel X&3). The left ~32-44
  logical columns ($260E=$FF, off-top) collapse onto byte 0 (margin); visible terrain maps to field
  bytes `$30+`. `$28CA/$28FA[ai]` = per-skyline-row bitmap row-address table (the PLOT's row base).
- **Headless verification (better than the "visual diff" below):** dump `terrainBitmap` chip RAM via
  the gdb stub (like `diag_run.sh`) + decode to pixels in python and PIXEL-DIFF the new direct-render
  output against the convert's output. Automatable; no eyeballing. Where they differ = exactly the
  layers not yet ported (dots/objects) — guides the work. (Eyeball/screenshot only as a final sanity.)

## Execution stages (each independently verifiable)

- **Stage 0 — measure the convert win.** ✅ DONE (`g_fConvert` ≈ 62656 cumulative / ~190 frames ≈
  330 beam ticks/frame ≈ 21 ms — the per-frame cost eliminating the convert would buy).

- **Stage 1 — native skyline+dot plot from `$260E[]` → bitplanes (Amiga-only, behind a flag).** New
  renderer reads `$260E[0..$D4]`, maps each column→(byteCol, 2-bit mask) and height→`multiplyBy120`
  row offset. Plot the skyline into the **sky bitplane (value 1)** and the texture into the **dot
  bitplane (value 2)**. Body left as the value-0 background. Run ALONGSIDE the existing convert and
  visually diff (FS-UAE `run.sh`); skyline + dots must match. (Sky not yet filled — just the
  skyline until Stage 2.)

- **Stage 2 — blitter fill UP for the sky bitplane.** Ascending blitter OR (row N → row N-1,
  BLTCON1 DESC walks addresses downward; minterm A|B→D) over the terrain band, on the **sky
  bitplane only** — propagate the skyline upward to row 0 so everything above the skyline is sky.
  The terrain body (value 0) and the dot bitplane are never filled. (NB: open question — confirm
  whether the Atari plots the skyline AS value-1 and fills, or fills sky as a separate step from
  the value-2 dots; flight3 had dots but no sky. Pin which `$260E`-derived edge feeds the sky fill.)

- **Stage 3 — cut the round-trip.** Stop the 6502 ridge `PLOT` into `mem[$1070]` (subdivision still
  computes `$260E[]`); drop `renderViewportModeD` for flight. ⚠ **Complication:** objects
  (`raster_fill_region`/`terrain_clip_row_top`) and any terrain jitter ALSO write `mem[$1070]` and
  rely on the convert. Eliminating the convert requires EITHER porting object plotting to native
  bitplane writes too, OR keeping a reduced convert for the object layer only. Decide here.

- **Stage 4 — optimise the subdivision math (the 84 %).** ✅ DONE 2026-06-24 (commits on main).
  68000-tuned all three, byte-identical (`make validate` 0 mem mismatch, real-flight snapshot):
  - **Hoisted** ZP scalars to register locals in all three (midpoint $8D-$91/$B5/$B6; column_rasterize
    $82/$84/$86/$80/$81/$B5 span+PLOT scratch; subdivide cpu.X recursion index + $9F budget), writing
    each ZP byte back once.  Explicit hoisting — NOT relying on the compiler caching the non-volatile
    `cpu` struct.
  - **Rewrote `terrain_midpoint_displace` in native 16-bit fixed-point** — each midpoint =
    (base+delta+1) arithmetic->>1, displacement = (mid-base)>>1 logical; collapses the 6502 byte-pair
    carry chains into single 68000 word ops (arith >>1 = (v>>1)|(v&0x8000)).
  - **Converted `terrain_column_rasterize` interpolation to plain C** — the "CLC ADC; ROR" averaging =
    (A+m)>>1; the $86 fraction = a 9-bit add picking a (base-$82)>>1 height correction w/ 0/$FF
    saturation.  ⚠ b446's b52e correction threads the height-average ROR carry (avg9&1) into its
    ADC/SBC (unlike b380's fresh c=0) — kept exactly.
  - **`terrain_subdivide_column`** left with carry-macro arithmetic in the cascade — NOT hot (runs once
    per leaf), and its memory traffic is already hoisted; converting would be cosmetic.

  **Measurement (per-frame normalizer `g_tdFrames` added — raw `g_tdSubdiv` is cumulative over a
  jittery frame count, so compare per-frame):** `tdSubdiv` ≈ **1935 → ~1554 beam ticks/frame, ≈20%**
  (baseline median of 2 runs vs current median of 5; ±~10% run-to-run, so trust the cluster, not
  single runs).  The hoisting+midpoint gave the bulk (~15%); column_rasterize ~6% more.  **The
  remaining `tdSubdiv` (~1554) is MEMORY-bound** (recursion stack push/pop, PLOT into mem[$1070],
  array indexing) — not arithmetic-bound, so further math tuning won't help.  The next flight-render
  lever is Stages 1-3 (eliminate the PLOT round-trip + the `renderViewportModeD` convert pass,
  `fConvert` ≈ 62656 cumulative / ~30 ms).

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
