# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate. Each entry: address, current name, what it really does, suggested name.

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|
| `$3FDE` | `terrain_lookup` | Draws the **compass heading indicator**: copies 4 glyph bytes from table `$4B0B` (indexed by heading `($281C + $3FF6[$2836]) & $FF`) into the compass cells `$32E3-$32E6` (the mode-4 line below the title text, screen x=144 y=32). Nothing to do with terrain. | `draw_compass_heading` |
| `$548D` | `update_gauge_digits` | Per-frame **SFX voice/gauge envelope engine**: runs `sfx_engine_step`, advances the 14 voice/gauge slots' freq/duration/priority envelopes (emits AUDF via `sfx_voice_write_freq`, re-queues finished slots), then **drains the `$0719` event ring** (`input_init` / `reorder_sprite_slot`). Has nothing to do with "digits". | `sfx_voice_envelope_tick` |
| `$3FFA` | `update_cockpit_digits_native` | Comment in NativeHandlers.cpp says it's a direct translation of **`startup_init` @ `$3FFA`** — name/address mismatch; it writes the cockpit 2×2 digit blocks but the canonical label is `startup_init`. Verify which is correct. | (confirm `$3FFA` identity) |
| `$5433` | `font_display_init` | flight_native.cpp comment: "**sfx_engine_reset** $5433 (mislabelled font_display_init in symbols.csv)". It resets the SFX engine, not fonts. | `sfx_engine_reset` |
| `$5367` | `sound_event_dispatch` | Named "sound event" but is the per-frame **scroll/animation priority dispatcher**: fires `step_accum_add_75` (tunnel ring), `scroll_terrain_columns`, `dl_index_dec`, or `scroll_terrain_dl` (door scroll) in priority order. Not a sound dispatcher. | `scroll_event_dispatch` |
| `$782A` | `copy_altitude_graphic_to_screen` | Copies the **Standby title text banner** — a 20-byte block selected by `$0091` (`$5A9F` "RESCUE ON FRACTALUS!" or `$5AB3` "©1985 LUCASFILM LTD") — into screen RAM `$32B7-$32CA`, and sets `$00D8=$44` for the copyright block. Nothing to do with "altitude": the name derives from `$0091` (`altitude_threshold`), which the SFX sequencer repurposes as the title-block selector during Standby. | `copy_title_text_block_to_screen` |
| `$40B0` | `update_bar_gauge_291c` | On `$291C`/`$291D` change, copies a 21-byte shape (table `$4B57`, shape selected by `$455B[$291C]`) into player buffer **`$0E87` (P2)** at vertical position `$291D`, `$FF`-capped. CONFIRMED (user, opposite-diagonal slope shapes across frames): this draws the **canopy A-pillars** (slanted window posts), NOT a gauge/altimeter. | `draw_canopy_pillar_p2` |
| `$40E5` | `update_gauge_281a` | Two unrelated instruments: (1) on `$281A` change, `$FF`-fills player **`$0C98` (P0)** from offset `$281A` down to `$38` — the **Altimeter #7 terrain-height bar** (`$281A` tracks terrain height `$0062`; CONFIRMED). (2) on `$281B` change, masks/ORs the **`$0B96..$0B99` missile edge bytes** = **Wing Clearance Bars #3** (M1/M2/M3). | split: `draw_altimeter_terrain_bar` + `update_wing_clearance_edges` |
| `$75F5` | `compute_gauge_geometry_from_006D` | Derives a set of **display coordinates from `level_stage` `$006D`** into `$061F-$0625,$0617,$0618,$061A-$061C,$062A,$08A2` (`$0628 = BCD($006D)`). Feeds the engine-sound pitch ramp and the lock-on-indicator animation step — i.e. **level-stage-driven display geometry, not a gauge readout**. | `compute_stage_display_geometry` (confirm; drop "gauge") |

## "Gauge" naming — variables (symbols.csv)
These zero-page / RAM vars carry "gauge" in their **name** but are not the cockpit
Energy/Thrust gauge. Rename alongside the functions above.
- `$2872` `bar_gauge_x_cache`, `$2874` `bar_gauge_y_cache` — change-detect caches (`$291C`/`$291D`) for `update_bar_gauge_291c` (`$40B0`, the **canopy pillar** P2 draw); → `canopy_pillar_*_cache`.
- `$2875` `gauge_281a_cache` — change-detect cache (`$281A`) for the **altimeter terrain-height bar** (`$40E5` part 1) → `altimeter_terrain_cache`. `$2876` `gauge_281b_cache` — cache (`$281B`) for the **Wing Clearance Bars #3** edges (`$40E5` part 2) → `wing_clearance_cache`.
- `$0617` `gauge_field_0617`, `$0618` `gauge_step_reload`, `$061F` `gauge_field_061F`, `$0620` `gauge_field_0620`, `$062A` `gauge_height_062A` — **outputs of `compute_gauge_geometry_from_006D`** (`$75F5`), i.e. stage-derived display geometry, not gauge state. Note `$0618` is specifically the **lock-on indicator #11** animation step reload (copied into `$00E6`/`animStepTimer` on underflow) — rename toward `lockon_*`/`stage_geom_*`, not `gauge_*`.
- "gauge" also appears only in the *description* (not the name) of `$0035` `indicator_pos`, `$006D` `level_stage`, `$006F` `dial_value`, `$0073` `alt_ring_head`, `$0074` `ring_tail_0719`, `$0719` `event_ring_0719`, `$00E6` `animStepTimer`, `$007E` `lockOnIndicatorState` — those names are fine; only the prose should stop calling them "gauge".

## "Gauge" naming — Amiga platform layer (hand-written C++, NOT symbols.csv)
The hand-written Amiga identifiers named `gauge*` all refer to the **Energy Level
Indicator (#12)** — the P1 strip `$0D98` (`pm_shape_strip`, driven by the `vobj_*`
functions `$4184`/`$41DA`/`$41E8`), HPOSP1 = `$00B5`, colour ramp `$00DE` (← table
`$4DEA`), shown as Amiga sprite 2 (COLOR21 = `$1AA`). CLAUDE.md's instrument table maps
that exact source to #12; the code's "throttle gauge" wording predates that vocabulary.
Rename toward `energyLevelIndicator*` (these are manual edits, not a transpiler regen):
- `RescueOnFractalus`: `gaugeSprite`, `buildGaugeSprite()`, `rsGauge`, `sbGauge` →
  `energyIndicatorSprite` / `buildEnergyIndicatorSprite()` / `rsEnergyIndicator` / …
- copper-list setters `setGaugeColor()` + last-poked shadows `sbGaugeCol`/`plGaugeCol`/
  `flGaugeCol`/`drGaugeCol`/`tnGaugeCol`, and the `INDEX_GAUGE_COL` slot (COLOR21) →
  `setEnergyIndicatorColor()` / `*EnergyCol` / `INDEX_ENERGY_COL`.
- ⚠ EXCEPTION: `AtariZp.h` `gaugeStepReload` (= `$0618`) is the **lock-on #11** step
  reload (see the variable note above), NOT the energy indicator — rename separately.

## Notes
- `$4229` `lock_on_indicator_tick` was *previously* mislabelled `update_gauge_digits`
  (already corrected in symbols.csv) — kept here only as context for the `$548D` confusion.
- `$548D` `update_gauge_digits` (SFX engine, row above) is the other big "gauge" misnomer;
  it surfaces in the VBI descriptions of `$52D7`/`$534D` ("update_gauge_digits") and in the
  Amiga comments — all resolve once `$548D` → `sfx_voice_envelope_tick`.
