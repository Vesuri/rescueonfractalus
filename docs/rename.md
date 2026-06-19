# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate. Each entry: address, current name, what it really does, suggested name.

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|

## Notes
- _(empty — previous backlog applied 2026-06-19; see below)_

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
