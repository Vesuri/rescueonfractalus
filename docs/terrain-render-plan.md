# Native Amiga terrain renderer — execution plan

> ⚠️ **SUPERSEDED / HISTORICAL (marked 2026-07-20).** This plan is DONE and then some.
> The convert pass it targets (`renderViewportModeD` chunky→planar) no longer exists for the
> terrain body: `renderFlightDirect` computes the per-column silhouette from `$260E`, plots
> plane2 dots directly (`ROF_PLOT_DOT` → `g_flightDotPlane`), and the **blitter** fills the sky
> (`blitterFillUp`) + clears (`blitterClear`). The fractal math (rasterize/project/subdivide/
> build_view/frame_setup) that this plan called "out of scope / the real floor" is ALSO now
> hand-asm'd (see `asm-migration-plan.md`). Kept only as a record of the original design intent.
> Current flight-perf status lives in the `flight-scene` memory + `asm-migration-plan.md` tail.

Plan to replace the current flight-terrain rendering path (per-pixel 6502-style
silhouette plotting into an Atari ANTIC mode-D bitmap, then a per-byte
chunky→planar conversion in `renderViewportModeD`) with a **native Amiga
two-plane renderer**: compute the per-column silhouette, plot a sparse detail
plane, and let the **blitter** fill the solid terrain mass.

> **Status (2026-06-12):** PLAN ONLY — verified the data model, not yet
> implemented. The fractal-subdivision arithmetic (the real per-frame floor) is
> explicitly *out of scope* here; this plan targets the plotting + conversion
> stages only. See [[flight-must-be-native]], [[rof-stars-planet-phase]].

---

## 1. What the terrain bitmap actually is (verified)

Decoding a representative in-flight frame (`a800dumps/launch_6_gameplay.a8s`,
mode-D source `$1070`, stride 96, 43 rows × 40 shown bytes, 2 bits/pixel) gives
a definitive picture of the on-screen encoding:

| 2-bit value | Raw bytes | Meaning | Coverage (one frame) |
|---|---|---|---|
| `0` | `0x00` | terrain body (solid) | ~2330 px |
| `1` | `0x55` | sky (solid) | ~4400 px |
| `2` | `0x0a/0x20/0x80/0xaa…` | dots / detail (sparse) | ~154 px |
| `3` | — | highlight (palette pen3) | 0 px this frame, rare |

**Key findings:**

1. **The terrain is solid-filled, NOT a 1px contour.** Sky is a solid `0x55`
   block on top; terrain body is a solid `0x00` block filling up from the
   bottom. The boundary between them is, per column, the silhouette top
   `$260E[x]`. So the fill **already exists** in the current pipeline (a CPU
   pass upstream, keyed on `$260E`) — this plan *moves* it to the blitter, it
   does not add new fill work.

2. **The detail (dots/highlights) is a sparse, separate layer** — only ~150 px
   per frame, sprinkled along the ridge and densening in the near field
   (the fractal "dither"), plus object pixels. No second *solid* layer exists
   (no "closer solid mountain"); it is one solid mass + sparse detail.

3. **Current Amiga bitplane split already matches a two-layer model**
   (`renderViewportModeD`: `kModeDP1[s]`=plane1=bit0, `kModeDP2[s]`=plane2=bit1,
   plane3 always 0):
   - **plane1** (bit0) = sky ∪ highlight → the solid region
   - **plane2** (bit1) = dots ∪ highlight → the sparse region
   - **plane3** unused in flight → only 2 meaningful planes.

   Because the native renderer bypasses `renderViewportModeD` entirely, **we
   choose the planar encoding**. The Atari source uses `0`=terrain / `1`=sky;
   we may invert freely (e.g. plane1 = terrain, blitter-fill ridge→bottom) as
   long as the copper palette matches — identical output either way.

   Flight palette today (`RescueOnFractalus.cpp` `buildCopperList`, flight branch):
   pen0 = terrain body (brown `#530`), pen1 = sky (salmon `#c76`),
   pen2 = dots (dark `#300`), pen3 = highlight (`#962`).

---

## 2. Terrain vs. objects: what folds, what does not

Traced `terrain_plot_object` and its helpers (`src/gen/rof_native.c`):

- **Terrain → folds into a per-column height array.** `terrain_column_rasterize`
  writes `$260E[x]` (the silhouette top, clamped `$97`) and OR-plots the ridge
  detail. The solid body is derivable from `$260E` alone.
- **Objects → do NOT fold.** `raster_scaled_object` ($AB9A) blits a 12×32 cell
  sprite; `terrain_clip_row_top`/`terrain_plot_pixel` stamp individual 2-bit
  colour pixels. These carry shape + colour a single height-per-column cannot
  represent.
- **Objects only *read* `$260E`, never write it** — `terrain_clip_row_top` is a
  pure occlusion test (`if (y <= $260E[x]) skip; else plot`). Terrain occludes
  objects; objects never occlude terrain via the buffer; objects don't disturb
  the height array. This is the clean seam the plan relies on.

**Draw-order consequence.** Today `terrain_draw_frame`'s loop ($A3AB)
*interleaves* per `$B67C` entry: object projection/plot **and** terrain
subdivision, back-to-front. The restructure splits this into "all terrain first,
then all objects." Effects:

- terrain↔terrain: `$260E` = max per column → order-independent. **Safe.**
- object↔object: painter's order preserved if the object pass keeps `$B67C`
  order. **Safe.**
- **terrain→object: changes.** Objects then test against the *complete* terrain
  silhouette instead of the partial-at-time-of-draw one. Arguably more correct,
  but a behavioral delta → **cannot be validated by mem-diff; needs visual
  validation** against atari800.

---

## 3. Proposed per-frame architecture

```
1. CLEAR silhouette state
   - $260E[x] -> horizon baseline. terrain_draw_frame already seeds parts
     ($263A+ = 0x67 at $A351; $264E/$266F/$2690/$26B1 = 0x6B at $A35E).
   - OPEN: confirm who clears the full $260E array + the bitmap each frame
     (the current solid fill implies an upstream sky/terrain fill pass —
     locate it; the native path must own the equivalent).

2. TERRAIN PASS  (compute only)
   - Run the fractal subdivision for all columns, but strip the bitmap RMW
     out of terrain_column_rasterize: keep the `$260E[x] = A` writes, drop PLOT().
   - Output: $260E[x] = per-column silhouette top.

3. SOLID LAYER  (blitter)
   - From $260E[x], set the silhouette boundary per column on the "fill" plane,
     then blitter inclusive-fill that plane (top->ridge for sky, or ridge->bottom
     for terrain — encoding choice). Runs async / off-CPU; overlap with CPU work.

4. DETAIL LAYER  (sparse, no fill)
   - Plot the value-2/3 dots + highlights as individual pixels on the other plane.
     This is where the ridge dots (was terrain_column_rasterize's PLOT) and object
     pixels land. NO fill.

5. OBJECT PASS
   - project_terrain_points + terrain_plot_object in $B67C order, occlusion-tested
     against the now-complete $260E, painting detail on top.
```

**Net change vs. today:** deletes the per-pixel terrain RMW *and* the entire
`renderViewportModeD` chunky→planar pass; offloads the solid fill to the blitter.

---

## 4. Performance expectation (honest)

Real but **not transformative**. This removes the plot RMW + the C2P conversion
pass and moves fill to the blitter (free wall-clock when overlapped). It does
**not** touch the ~104 ms fractal-subdivision arithmetic that computes `$260E`
— which the profiler identifies as the per-frame floor (`terrain_draw_frame`
≈48% of frame; subdivision is the dominant cost; see [[flight-must-be-native]]).
A large FPS win still requires attacking subdivision separately; this rendering
restructure is **complementary** to that, not a substitute.

---

## 5. Validation strategy

- **Steps 1–2 (the `$260E` height output)** stay mem-diffable against the
  `__t6502` oracle: keep a `_core` that computes `$260E` only and diff it via
  `make validate FN=terrain_column_rasterize` (the height array is a pure mem
  contract). See [[native-reimpl-seam]].
- **Steps 3–5** diverge from the Atari bitmap *by design* (different planar
  encoding, blitter fill, reordered object pass) → **visual/screenshot parity
  against atari800**, not mem-diff. Use the existing SDL/atari800 oracle.

---

## 6. Open items to resolve before coding (⚠ HISTORICAL — the plan shipped; see the header)

> All four were answered by the renderer that actually shipped (`renderFlightDirect` +
> `blitterFillUp`/`blitterClear`). Kept as the original design intent — **not a TODO.**

1. **Full `$260E` clear + bitmap clear owner** — the current solid sky/terrain
   fill must come from an upstream pass (not in `terrain_draw_frame`). Locate it;
   the native renderer must reproduce its silhouette-keyed fill.
2. **Fill polarity / blitter mode** — decide plane1 = sky (fill top→ridge) vs.
   plane1 = terrain (fill ridge→bottom); pick the cheaper blitter fill direction
   and set the palette to match.
3. **Detail-plane source** — confirm all value-2/3 pixels are reproducible from
   (ridge dots + object pixels); check a near-field frame for any denser detail
   that this plan's sparse-plot assumption would miss.
4. **Hand-written 68k plot loop** — the `yForX → multiplyBy120 → or.b mask`
   loop (use **`or.b`**, not `move.b`; 3 interleaved planes, 120-byte stride,
   lo-res 2px columns) targets step 4's detail plane and/or the boundary set in
   step 3; spec it against the chosen encoding.
