# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate (`make gen`). Hand-written twins (`rof_native.c`, `rof_native_amiga.cpp`,
`validate_native.c`, the Amiga platform layer, `transpile.py` comments) reference the names
directly and must be updated in the same pass. Each entry: address, current name, what it
really does, suggested name.

## Open: function renames

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|
| _(none — drained 2026-06-30; add new rows here)_ | | | |

## Notes
- **"scroll" in the `scroll_*` names = Atari LMS / buffer-shift scrolling**, not pixel
  motion: `scroll_terrain_dl` ($6953) shifts the viewport DL's per-scanline LMS pointers so
  the door halves slide apart vertically (`dl_lms_scroll_up`/`down` $69A9/$69C3);
  `scroll_field_columns` ($6AEE) shifts the $0C32-$0F32 buffers left a column (horizontal
  starfield scroll). The tunnel rings "rushing in" are **palette cycling** ($08D4-$08D9
  rotation), NOT a scroll.

## Open: unnamed memory locations (add `symbols.csv` var rows)

RAM/ZP cells read/written by the in-flight code but with **no `symbols.csv` var row** (the
transpiler emits them as raw `mem[0xNNNN]`). Add var rows so the native + transpiled code read
as named state. ✓ = confident, ? = verify before promoting the suggested name to canonical.
Surfaced 2026-06-30 while de-transpiling the flight subtree; the four ✓ cells were applied that
day (see the Applied log) and removed from the lists below — everything remaining is ?-tentative.

**`cockpit_dial_update` ($4430) — dial bar derivation (surfaced 2026-07-02 during its de-6502 cleanup):**
- `$0625` → `dial_base_offset` ? — added to the dial value before the `$4457` table lookup
  (`ADC $0625` at $443d); the per-dial base index selecting which dial's bar-value run to read.
- `$4457` → `dial_bar_value_table` ? — the bar-value lookup table read at `$4457,Y` (Y = dial
  value + `$0625`); maps a dial value to its drawn bar fill.
- `$4581` → `dial_column_ptr_table` ? — per-column 16-bit target-cell pointer table (indexed by
  `2*column` in `draw_object_column`); each entry is the cockpit screen-RAM address that column's
  bar glyph is drawn into.
- ZP `$00BB/$00BC` (`dl_y1`/`dl_y2`) → `dial_col_ptr_lo`/`_hi` ? and `$00BD` (`dl_y3`) →
  `dial_col_counter` ? — in `draw_object_column` these are the current column's cell pointer and
  the loop counter, NOT display-list Y coordinates; the `dl_y*` names are misleading here (they
  may be reused as DL coords elsewhere — verify before renaming).

**`vbi_handler_flight` ($4FF5) — display-shadow pushes ($4FF7-$5036):**
- `$00CB` → `hposp2_shadow` ? — pushed to HPOSP2 ($D002) (altimeter terrain-bar X).
- `$00CD` → `grafm_shadow` ? — pushed to GRAFM ($D00A) (wing-clearance missile graphics).
- `$026F` → `prior_shadow` ? — pushed to PRIOR ($D01B) (GTIA priority).
- `$0037` → `colpf1_shadow_or_terrain_h` ? — pushed to COLPF1 ($D014); also force-set to $78 in
  the target-latch reset.  Dual use unclear — verify.

**`vbi_handler_flight` — atmosphere colour-ramp ($51BF-$520C, altitude→palette fade):**
- `$08A1` → `atmo_fade_countdown` ?, `$08A2` → `atmo_fade_phase` ?, `$08A3` → `atmo_band_base` ? —
  the slow-fade phase counters ($08A1 counts down → bumps $08A2 → indexes $364B → $08A3).
- `$00DB` → `terrain_pen1_fade` ? — the second salmon→brown terrain pen (paired with the applied
  `$00DC` terrain_pen0_fade), written from the lookup tables.
- `$07F9` → `atmo_audc_table`, `$0823` → `atmo_pen0_table`, `$084D` → `atmo_pen1_table`,
  `$0877` → `atmo_anim_table` ? — 4 parallel altitude-band lookup tables (indexed by depth band
  + `$08A3`).  `$364B` → `atmo_fade_step_table` ? — the $08A2→$08A3 step table.
- `$07E9` → `attract_palette_src` ? — source of the RTCLOK-EOR palette strobe → `display_param_0..E`.

**`vbi_handler_flight` — target-latch / object indices (the "sim" frame $5178 block):**
- `$2845` → `p3_object_state` ?, `$006A` → `p3_object_mode` ? — gate the player-3 object draw path.
- `$0039`/`$286A`/`$286B`/`$286C` → `target_obj_*` ? — the latched target object indices/coords.
- `$003A` → bit7 gates the shields-cell update; verify (game-state/health flag?).

**`vbi_handler_flight` — cockpit cells / misc:**
- `$3356`/`$3357` → `cockpit_shields_cell` ? — the mode-4 cockpit cell pair written ($36/$B6 ±1)
  for the Shields-On status indicator (#14).
- `$062C` → `static_dither_threshold` ? — the windscreen-"static" POKEY-RANDOM dither threshold
  (SDL-only raster effect; dead on Amiga).  `$0632` → `static_enable_flag` ? — gates that block.

**`flight_control_integrate` ($8E5B):**
- `$005D` → `ground_proximity_flag` ? — gates auto-level pitch rate (8e8a) and throttle braking
  (8ff7); set to $F0/$FF/$00 from the dial/life/`$2917` logic at 91a3-91bc. (Landing/near-ground.)
- `$003D` → `landing_seq_flag` ? — when nonzero, runs the "in-box"/$3355 special-state path
  (8ef5/8f2b) and suppresses the pitch/roll auto-level (8f9a); also driven by `step_object_along_axes`.
- `$0023`/`$0024` → `pitch_shadow_lo`/`pitch_shadow_hi` ? — snapshot of pitch_pos ($25/$26) taken at
  8f20; later feeds the canopy-pillar Y at $2871 (9 15a).
- `$002B`/`$002C` → `world_dx_lo`/`world_dx_hi` ? — per-frame world-X velocity added to world_x
  ($2887/$2888); produced by `compute_obj_rel_angle_scale`. (`$002C` reloaded+discarded at 910c.)
- `$2881`/`$2882` → `world_dz_lo`/`world_dz_hi` ? — per-frame world-Z velocity added to world_z.
- `$2883`/`$2884` → `fwd_step_lo`/`fwd_step_hi` ? — signed forward/depth step = (throttle_hi*roll)
  <<3; added to the depth accumulator ($33/$34) at 9121.
- `$28D6` → `roll_mag_scaled` ? — |(roll_pos<<3)>>8|; the multiplier fed to `mul_u8` for fwd_step.
- `$0020` → reused here purely as ROL scratch for the $28D6/fwd-step sign (NOT its display-list role).
- `$283C` → `landing_inhibit_flag` ? — when set, blocks the $3355=$34 special-state entry (8f2b).
- `$283D` → `heading_freeze_flag` ? — selects terrain index angle source: RTCLOK_LOW if set, else
  heading_hi (9164).
- `$066C`/`$066D` → `engine_state_a`/`engine_state_b` ? — paired flag set to 0/1 around the
  game_sub_55FC HUD refresh + $3355 special-state transitions (8f0a/8f37/8f54).
- `$0686`/`$0687` → `engine_sound_pitch_a`/`engine_sound_pitch_b` ? — ~(throttle<<1 hi); $0687=$0686-4.
- `$2917` → `lockon_rand_countdown` ? — DEC'd, compared to RANDOM to latch `$005D`=$FF (91a5-91b9).
- `$2871`/`$2873` → `canopy_pillar_y_left`/`canopy_pillar_y_right` ? — pitch/roll-derived pillar Y
  (915a/9150); pushed into the 7-deep history ring ($28A8/$28AF).
- `$2850`-`$2853` → `obj_vel_x_lo/hi`/`obj_vel_y_lo/hi` ? — object velocity from the delayed ring
  history ($2919/$291A/$291B), arithmetic-shifted; integrated into the object pos at 9267.
- `$2829`/`$0068` and `$282C`/`$0069` → `obj_accum_*` ? — 16-bit object-position accumulators.
- `$284E`/`$0038`(vobj_row_count) and `$284F`/`$0039` → `obj_pos2_*` ? — second object pos pair.
- `$2919`-`$291D` → `ring_cur_0`..`ring_cur_4` ? — current (delayed-by-7-frames) read of the history
  ring fields (pitch lo/hi, roll vel, pillar L/R).
- `$291E` → `ring_head` ? — 0..6 rotating index of the 7-entry attitude history ring.
- `$2893`/`$289A`/`$28A1`/`$28A8`/`$28AF` → `ring_pitch_lo[]`/`ring_pitch_hi[]`/`ring_roll_vel[]`/
  `ring_pillar_l[]`/`ring_pillar_r[]` ? — the five 7-entry history-ring arrays.

**`update_terrain_scanline_proj` ($9833):**
- `$2270`/`$2271` and `$2272`/`$2273` → `map_x_scratch_lo/hi`/`map_z_scratch_lo/hi` ? — the
  LSR working bytes for world>>4; end up equal to map_x ($27FD/$27FE) / map_z ($27FF/$2800).
- `$2801`-`$2804` → `map_x_mirror_lo/hi`/`map_z_mirror_lo/hi` ? — a second copy of the map
  coords (sampler reads $27FD-$2800; the mirror is consumed by update_terrain_horizon_lr's deltas).
- `$2274` → `scaled_depth_lo` ? — paired low byte of the <<2 depth fixed-point (hi = scaled_depth_hi $2275).
- `$27F9` → `terrain_height_q2` ? — sampled height rounded down by /4 (two rounding halvings).
- `$281A`/`$281B` → `viewport_top_row`/`viewport_bottom_row` ? — the visible terrain span limits
  derived from height + depth step; $281A extends (clamped $38) once depth step passes $37.
- `$0070` → `terrain_clearance` ? — ship-above-terrain clearance (scaled_depth_hi - height), 0 on
  contact; gates step_object_along_axes' depth brake and the engine-sound path.
- `$283C` → `landing_inhibit_flag` ? (also seen in flight_control_integrate) — when 0, allows
  exit_terrain_special_state.
- `$2879` → `proj_phase_flag` ? — 0/1 latch of the $066C landing/launch projection state machine.
- `$066C` → `engine_state_a` ? (paired $066D, also in flight_control_integrate) — its value
  (<4 / 4-7 / >=8) selects the projection phase.

**`project_terrain_points` ($A11F)** — X = object/column index into these 22-entry arrays:
- `$24B4` → `obj_proj_flags[]` ? — per-object cull/state byte; bit4 = "already projected this frame".
- `$22A4`/`$22D2` → `obj_num1_lo[]`/`obj_num1_hi[]` ? — half-1 signed numerator (screen X source).
- `$235B`/`$2388` → `obj_num2_lo[]`/`obj_num2_hi[]` ? — half-2 signed numerator (screen Y source).
- `$2300`/`$232E` → `obj_divisor_lo[]`/`obj_divisor_hi[]` ? — shared perspective divisor (depth).
- `$2400`/`$242D` → `obj_screen_x_lo[]`/`obj_screen_x_hi[]` ? — half-1 projected coord output.
- `$245A`/`$2487` → `obj_screen_y_lo[]`/`obj_screen_y_hi[]` ? — half-2 projected coord output.
- `$270E`/`$272D` → `band_scroll_offset[]` ? — per-screen-band scroll offset table added to the
  half-2 output ($272D = $270E+$1F; indexed by half-1 X >> 3 when half-1 hi byte is 0).
- `$00B5` → `proj_fold_scratch` ? — fold high-byte scratch (= projection magnitude >> 8); in the
  contract but dead after return. ($AE/$AF/$B0/$B1/$B2 are divide_16x16 scratch, already excluded.)

**`terrain_frame_setup` ($9E54):**
- `$22A3`/`$22D1` and `$22FF`/`$232D` → `obj_num1_in_lo/hi[]` / `obj_divisor_in_lo/hi[]` ? — the
  INPUT column vectors (offset -1 from the $22A4/$2300 outputs project_terrain_points consumes);
  built by build_view_transform_matrix, rotated/translated per cell into the $22A4.. outputs.
- `$2276` → `obj_col_index[]` ? — per-column height-map index ((X&0x0F)|b6) stored for later use.
- `$23B5` → `obj_height_sample[]` ? — per-column terrain height read from $0900[index].
- `$0900` → `terrain_height_map` ? — 16x16 (256-byte) height grid sampled here and by $9A36.
- `$B67C` → `obj_draw_order[]` ? — 12-entry object draw-order list walked by loop 2.
- `$28DB` → `collapse_cur_obj` ? — current object index saved across the pair-collapse in loop 2.
- `$00A0`-`$00A3` (named draw_iter_count/scroll_accum_b0..b2) are DUAL-USED here as the
  view-transform rotation vector {rot_a=$A1:$A0, rot_b=$A3:$A2} — the mem.h names are misleading
  in this context; consider a union/overlay name.
- `$00B4` → `proj_setup_scratch` ? — = (vbi_flags&0x0F) | $B5; written once, dead within the fn.
- `$00B5`/`$00B6` → loop-1 cell-pattern / column-high-nibble scratch (b5/b6); $B5 reused in loop 2
  to stash the scan index.

**`terrain_jitter_column` ($A613):**
- `$2829`/`$0068` and `$282C`/`$0069` → `obj_jitter_x_*` / `obj_jitter_y_*` ? — the random
  horizontal/vertical jitter offsets it writes (same cells the flight_control_integrate
  obj-accumulators feed; see that section's `obj_accum_*`).
- `$A63A` `terrain_plot_return` is an empty RTS (a shared return target), not a real callee —
  the clean rewrite drops the no-op tail call.

**`sfx_voice_envelope_tick` ($548D)** — per-slot envelope arrays (Y=$0E..1):
- `$06DB`/`$06E9` → `freq_env_step[]`/`freq_env_phase[]` ? — frequency envelope increment
  (nonzero = active) and its wrapping phase accumulator.
- `$0679`(hud_field_679)/`$06BF`/`$06CD` → `freq_value[]`/`freq_delta[]`/`freq_target[]` ? — the
  frequency field, its per-step delta, and the target that expires the envelope.
- `$06A3`/`$06B1` → `dur_env_step[]`/`dur_env_phase[]` ? — duration/priority envelope step + phase.
- `$066B`(sfx_voice_distort_0e)/`$0687`/`$0695` → `prio_value[]`/`prio_delta[]`/`prio_target[]` ? —
  the 4-bit priority/distortion field, its delta, and expiry target.
- `$06F7` → `slot_event_id[]` ? — event id re-queued (bit7-marked) on the ring when a slot expires.
- `$5406` → `env_gate_table` ? — ROM ramp/gate table; a zero entry pauses the envelope step.
- `$0718` (sfx_voice_expired_flag) is named; reset per slot, set on expiry.

---

# Applied log (newest first)

## Applied 2026-06-30b (batch rename + var rows, committed)
Functions (`symbols.csv` → `make gen` → `make validate` PASS, 0 mem mismatch on every twin):
- `raster_fill_region`→`raster_scaled_object` ($AB9A) — scaled object-sprite rasteriser (samples a
  distance-scaled source bitmap through the $52-$55 step accumulators), NOT a region/flood fill.
- `draw_canopy_pillar_p2`→`draw_ah_ground_fill_p2` ($40B0) — Artificial-Horizon ground fill into the
  P2 buffer ($0E87+), NOT the canopy pillars (those are P0/P1 $0C32/$0D32).

New var rows (the ✓-confident cells from the 2026-06-30 unnamed-memory backlog):
- `$00C7` `dli_dispatch_index`, `$00D9` `lockon_flash_color`, `$00DC` `terrain_pen0_fade`,
  `$2840` `wing_bar_hpos_base`.

Hand-written files updated in the same pass: `rof_native.c`, `validate_native.c`, `transpile.py`,
`RescueOnFractalus.cpp`, `docs/terrain-{draw,render}-plan.md`; the `$2872`/`$2874` cache-var notes
were repointed to `draw_ah_ground_fill_p2`. Amiga `out/RoF.exe` rebuilt clean.

## 2026-06-26 (applied 2026-06-30b)
- **`$40B0` `draw_canopy_pillar_p2` → `draw_ah_ground_fill_p2`** — see the 2026-06-30b entry above.
  Despite the old name it did NOT draw the canopy A-pillars (those are P0/P1 `$0C32`/`$0D32`, built
  once by `unpack_terrain_seed_cols`); it copies a 21-byte `$FF`-terminated slope from table `$4B57`
  (index `$455B[$291C]`) into the P2 buffer at `$0E87+` — the Artificial Horizon ground fill
  (`$0E92-$0EB2`, the brown ground whose boundary moves with pitch). Change-detected on
  `$291C`/`$291D` vs cache `$2872`/`$2874`, so it redraws only when attitude changes.

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

**Unnamed terrain field-generator memory (rof_native.c, cleaned 2026-07-03 during the
tunnel→stars build-burst work):**
- `$6B5F` — 4-entry preset terrain-height table the RNG picks from (1/32 of samples).
  Temp macro `TERRAIN_HEIGHT_TABLE` in rof_native.c; suggest a symbols.csv name
  `terrain_height_table`.
- `$0C32/$0D32/$0E32/$0F32` — the four parallel 89-byte terrain-height column buffers
  (`$0100` apart), filled by `gen_terrain_column` / scrolled by `scroll_field_columns`.
  Temp macro `TERRAIN_COL_BUF(layer,col)` in rof_native.c; suggest symbols.csv names
  `terrain_col_buf_0..3`. ⚠ DUAL-USE: at gameplay init these same buffers hold the canopy
  A-pillars (P0 `$0C32` / P1 `$0D32`, RLE-decoded) + starfield (P2 `$0E32` / P3 `$0F32`) —
  see CLAUDE.md "canopy POSTS"; a name should reflect the shared player-buffer role, not
  just "terrain".

**Door-frame drawer functions + memory (rof_native.c, cleaned 2026-07-03 during the
standby→doors freeze work — `draw_frame_pattern_seq` subtree):**
- `$0080/$0081` — currently named `sync_flag` / `dl_ptr_lo`, but in the plot/span path these
  are the **16-bit screen row pointer** (lo/hi) that `set_row_ptr` loads from the $073D/$0793
  table and that `plot_glyph_pixel_masked` / `fill_*_span` dereference. Misleading. Suggest
  `row_ptr_lo` / `row_ptr_hi` (and audit other `sync_flag`/`dl_ptr_lo` uses — the names may be
  correct in the DL-setup path but not here).
- `$6E0F` — unnamed ROM table = the **door-frame / tunnel-ring span-thickness table** (one entry
  per concentric rectangle), read by `draw_frame_pattern_seq`, `draw_ring_frame_step`,
  `plot_terrain_span`, `step_accum_sub_7e`, `emit_dl_coord_pairs`. Suggest
  `frame_span_thickness_tbl`.
- `plot_glyph_pixel_masked` ($66DE) — no glyph is involved; it OR/ANDs one 2-bit pixel into a
  screen byte. Suggest `plot_masked_pixel`.
- `draw_shape_rows_loop` ($6620) — draws three **vertical guide columns** ($9C, $9D, $9D+1) down
  all 86 rows, not "shape rows". Suggest `draw_frame_guide_columns`.

**Frame-wait spin-pacers (rof_native.c, cleaned 2026-07-05 during the flight-entry
delay investigation — `push_a_wait_frames`/`wait_frames_4c` native twins):**
- `clear_colors` ($3CC3) — MISNAMED. The disasm is `PHA; LDA #$01` falling into
  `wait_setcount` ($3CC6) = **wait 1 frame** (accumulator preserved), NOT a colour clear.
  The symbols.csv comment "Clears player color shadows PCOLR0-3 + COLBK" is wrong. Suggest
  `wait_frames_1`.
- `push_a_wait_frames` ($3CB1) — named after the 6502 mechanism (PHA). It waits `timer_4C`
  ($4C) vertical-blank periods, preserving the accumulator. Suggest `wait_frames` (or
  `wait_frames_preserve_a`).
- `wait_frames_4c` ($3CB2) — the `4c` is the `$004C` address it reads. Suggest
  `wait_timer_4c_frames` / `wait_frame_count` (waits the caller-set `timer_4C` count).
- `timer_4C` / `$004C` — named after its address; it is the **frame-wait target count** set by
  every caller entry before routing through `wait_frames_4c`. Suggest `frame_wait_count`.
- `wait_setcount` ($3CC6) — OK-ish (stores the count then falls into the wait spin), but could
  be `wait_frames_set_count` for symmetry.
