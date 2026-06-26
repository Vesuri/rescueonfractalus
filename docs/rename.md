# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate (`make gen`). Hand-written twins (`rof_native.c`, `rof_native_amiga.cpp`,
`validate_native.c`, the Amiga platform layer, `transpile.py` comments) reference the names
directly and must be updated in the same pass. Each entry: address, current name, what it
really does, suggested name.

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|
| $AB9A | `raster_fill_region` | Not a region/flood fill: samples a **distance-scaled source object bitmap** (source ptr `($00C3)` + the `$AC3A` per-bit table) through the `$0052-$0055` fixed-point step accumulators over a 12×32 sampling grid, plotting each set source bit via `terrain_clip_row_top`. I.e. it draws a scaled object's texture/detail (the terrain "dots" when called with plot-mask `$0058=$AA`). Name hides that it's an object-sprite rasteriser. | `raster_scaled_object` (or `plot_scaled_object_cells`) |

## Notes
- **"scroll" in the `scroll_*` names = Atari LMS / buffer-shift scrolling**, not pixel
  motion: `scroll_terrain_dl` ($6953) shifts the viewport DL's per-scanline LMS pointers so
  the door halves slide apart vertically (`dl_lms_scroll_up`/`down` $69A9/$69C3);
  `scroll_field_columns` ($6AEE) shifts the $0C32-$0F32 buffers left a column (horizontal
  starfield scroll). The tunnel rings "rushing in" are **palette cycling** ($08D4-$08D9
  rotation), NOT a scroll.

---

## Applied 2026-06-24 (batch rename, committed)
Functions (`symbols.csv`, regenerated + `make validate` PASS, 0 mem mismatch):
- `push_a_thunk_3cb2`→`push_a_wait_frames` ($3CB1) — PHA + frame-wait, no "thunk".
- `wait_frames_60`→`wait_frames_4c` ($3CB2) — waits the caller-set $4C count, not a fixed 60.
- `advance_message_column`→`draw_ring_frame_step` ($670D) — incremental tunnel-ring draw, not text.
- `scroll_terrain_columns`→`scroll_field_columns` ($6AEE) — shifts the $0C32-$0F32 starfield/PMG
  buffers (scenes 4-6), not flight terrain.
- `scroll_event_dispatch`→`launch_anim_dispatch` ($5367) — launch-cinematic priority dispatcher
  (only 2 of its 6 steps are "scroll"); also fixed the stale "sound flags"/"sfx" prose in notes.
- `terrain_collision`→`terrain_collision_and_silhouette` ($AE53) — besides the ship-vs-terrain
  collision scan it renders the terrain silhouette (sky fill + body + dot texture) every flight
  frame; collision is a side-effect of the same row scan.

Amiga platform twin renamed together: `scroll_event_dispatch_native`→`launch_anim_dispatch_native`.

## Applied 2026-06-19 (batch rename, committed)
The earlier backlog was applied wholesale. For the record, the renames made:

**Functions (symbols.csv):** `terrain_lookup`→`draw_compass_heading` ($3FDE),
`sound_event_dispatch`→`scroll_event_dispatch` ($5367),
`font_display_init`→`sfx_engine_reset` ($5433),
`update_gauge_digits`→`sfx_voice_envelope_tick` ($548D),
`update_bar_gauge_291c`→`draw_canopy_pillar_p2` ($40B0),
`update_gauge_281a`→`draw_altimeter_bars` ($40E5),
`compute_gauge_geometry_from_006D`→`compute_stage_display_geometry` ($75F5),
`copy_altitude_graphic_to_screen`→`copy_title_text_block_to_screen` ($782A).
($3FFA stays `startup_init` — already correct.)

**Variables (symbols.csv):** `bar_gauge_x_cache`→`canopy_pillar_x_cache` ($2872),
`bar_gauge_y_cache`→`canopy_pillar_y_cache` ($2874),
`gauge_281a_cache`→`altimeter_terrain_cache` ($2875),
`gauge_281b_cache`→`altimeter_ship_cache` ($2876),
`gauge_field_0617`→`stage_geom_0617` ($0617),
`gauge_step_reload`→`lockon_step_reload` ($0618),
`gauge_field_061F`→`stage_geom_061F` ($061F),
`gauge_field_0620`→`stage_geom_0620` ($0620),
`gauge_height_062A`→`stage_geom_height_062A` ($062A).

**Amiga platform layer (hand-written C++):** `gauge*`→`energyIndicator*`/`*Energy*`
(`gaugeSprite`→`energyIndicatorSprite`, `buildGaugeSprite`→`buildEnergyIndicatorSprite`,
`rsGauge`→`rsEnergyIndicator`, `sbGauge`→`sbEnergyIndicator`,
`setGaugeColor`→`setEnergyIndicatorColor`,
`sb/pl/fl/dr/tnGaugeCol`→`*EnergyCol`, `INDEX_GAUGE_COL`→`INDEX_ENERGY_COL`,
`gaugeCol`→`energyCol`); `gaugeStepReload`→`lockonStepReload` (AtariZp.h, lock-on #11,
NOT energy); `update_cockpit_digits_native`→`startup_init_native` ($3FFA identity).

Descriptive "gauge" prose still appears in some comments (Atari hardware-channel /
"throttle gauge" descriptions) — left as-is where accurate; sweep opportunistically.

## 2026-06-26

- **`$40B0` `draw_canopy_pillar_p2` → `draw_ah_ground_fill_p2`** (suggested). Despite the
  name it does NOT draw the canopy A-pillars (those are P0/P1 `$0C32`/`$0D32`, built once by
  `unpack_terrain_seed_cols`). It copies a 21-byte `$FF`-terminated slope from table `$4B57`
  (index `$455B[$291C]`) into the P2 buffer at `$0E87+` — i.e. the **Artificial Horizon ground
  fill** (`$0E92-$0EB2`, the brown ground whose boundary moves with pitch; see the AH note in
  the flight-scene memory). Change-detected on `$291C`/`$291D` vs cache `$2872`/`$2874`, so it
  redraws only when attitude changes (cheap most VBI firings — NOT a per-frame cost).
