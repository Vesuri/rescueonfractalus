# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate. Each entry: address, current name, what it really does, suggested name.

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|
| $3CB1 | `push_a_thunk_3cb2` | `PHA`, then falls into the $3CB2 spin-wait (RTCLOK_LOW $14 → $4C frames), then `PLA;RTS`. Just preserves A across a frame-wait — there is no "thunk" (no deferred closure pushed). | `push_a_wait_frames` |
| $3CB2 | `wait_frames_60` | Spin-waits until RTCLOK_LOW($14) reaches the **caller-set** `$4C` frame count — NOT a fixed 60. The 60 (`#$3C`) is loaded only by the *separate* entry $3CBE, which sets $4C then jumps in. | `wait_frames_4c` |
| $670D | `advance_message_column` | Draws ONE tunnel-ring frame group via `draw_symmetric_span_loop` (count = $6E0F[$A0]), clears $08D8 (inner ring colour) when $A0<6, then `DEC $A0` / `$88=$A0` — the per-ring-tick incremental tunnel-ring draw + counter step. No message/text ($6E0F is a ring-thickness table, not glyphs). | `draw_ring_frame_step` |
| $6AEE | `scroll_terrain_columns` | $0089-gated: shifts the four $0C32/$0D32/$0E32/$0F32 PMG buffers left one column + appends via `gen_terrain_column`. During the gated phase those buffers are the **starfield/PMG** field (scenes 4-6), not flight terrain — "terrain" is misleading. ⚠ verify stars-vs-planet-surface first; `gen_terrain_column` may share the issue. | `scroll_field_columns` (tentative) |

## Notes
- **`scroll_event_dispatch` ($5367) — name OK, NOTE is stale/wrong.** The name was a
  deliberate rename (was `sound_event_dispatch`, applied 2026-06-19) and is fine: it's the
  per-frame launch-cinematic priority dispatcher. But its `symbols.csv` note still reads
  "Tests **sound flags** $008D/88/89/8B … → **sfx** subs 6a8f/6a38/6aee/69e3/6a27/6953" —
  those are NOT sound flags or sfx: they're the animation-state flags and the ring-step
  ($6A38), column-scroll ($6AEE), DL-index ($69E3), **corner-reveal recede** ($6A27), and
  door-scroll ($6953) handlers. Fix the note when the table above is applied.
- **`scroll_event_dispatch_native`** (hand-written C++, `rof_native_amiga.cpp`) is the Amiga
  twin of $5367 — keep its name in sync with whatever $5367 settles on (currently fine).

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
