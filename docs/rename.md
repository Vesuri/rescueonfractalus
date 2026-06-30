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

## 2026-06-30 — unnamed memory locations surfaced de-transpiling `vbi_handler_flight` ($4FF5)

These RAM/ZP cells are read/written by the in-flight VBI but have **no `symbols.csv` var row**
(the transpiler emits them as raw `mem[0xNNNN]`).  Add var rows so the native + transpiled code
read as named state.  Behaviour is from the $4FF5 handler context; ✓ = confident, ? = verify.

**Display-shadow pushes (the per-frame GTIA/ANTIC register copy at $4FF7-$5036):**
- `$00C7` → `dli_dispatch_index` ✓ — the DLI/NMI chain index, reset to 0 at the top of every VBI.
- `$00CB` → `hposp2_shadow` ? — pushed to HPOSP2 ($D002) (altimeter terrain-bar X).
- `$00CD` → `grafm_shadow` ? — pushed to GRAFM ($D00A) (wing-clearance missile graphics).
- `$026F` → `prior_shadow` ? — pushed to PRIOR ($D01B) (GTIA priority).
- `$00D9` → `lockon_flash_color` ✓ — colour-cycled in the "draw" frame (LSR/SBC/ADC/ROL toggle
  through $4B..$4E), pushed to $D015; the enemy/lock-on flash (matches CLAUDE.md instrument map).
- `$0037` → `colpf1_shadow_or_terrain_h` ? — pushed to COLPF1 ($D014); also force-set to $78 in
  the target-latch reset.  Dual use unclear — verify.

**Atmosphere colour-ramp ($51BF-$520C) — altitude→palette fade:**
- `$08A1` → `atmo_fade_countdown` ?, `$08A2` → `atmo_fade_phase` ?, `$08A3` → `atmo_band_base` ? —
  the slow-fade phase counters ($08A1 counts down → bumps $08A2 → indexes $364B → $08A3).
- `$00DC` → `terrain_pen0_fade` ✓ / `$00DB` → `terrain_pen1_fade` ? — the salmon→brown terrain
  pens written from the lookup tables (matches the FlightCopperList terrain-fade note).
- `$07F9` → `atmo_audc_table`, `$0823` → `atmo_pen0_table`, `$084D` → `atmo_pen1_table`,
  `$0877` → `atmo_anim_table` ? — 4 parallel altitude-band lookup tables (indexed by depth band
  + `$08A3`).  `$364B` → `atmo_fade_step_table` ? — the $08A2→$08A3 step table.
- `$07E9` → `attract_palette_src` ? — source of the RTCLOK-EOR palette strobe → `display_param_0..E`.

**Target-latch / object indices (the "sim" frame $5178 block):**
- `$2845` → `p3_object_state` ?, `$006A` → `p3_object_mode` ? — gate the player-3 object draw path.
- `$0039`/`$286A`/`$286B`/`$286C` → `target_obj_*` ? — the latched target object indices/coords.
- `$003A` → bit7 gates the shields-cell update; verify (game-state/health flag?).

**Cockpit cells / misc:**
- `$3356`/`$3357` → `cockpit_shields_cell` ? — the mode-4 cockpit cell pair written ($36/$B6 ±1)
  for the Shields-On status indicator (#14).
- `$062C` → `static_dither_threshold` ? — the windscreen-"static" POKEY-RANDOM dither threshold
  (SDL-only raster effect; dead on Amiga).  `$0632` → `static_enable_flag` ? — gates that block.
- `$2840` → `wing_bar_hpos_base` ✓ — base HPOS for the wing-clearance missiles (M3/M2/M1 = +0,+$0C,+$11).

**`flight_control_integrate` ($8E5B) — unnamed memory (found during the clean-C rewrite):**
The master flight step reads/writes many still-unnamed cells. Addresses + observed behaviour:
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

**`update_terrain_scanline_proj` ($9833) — unnamed memory (found during the clean-C rewrite):**
The top of the flight projection subtree. Cells still lacking mem.h names:
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

**`project_terrain_points` ($A11F) — unnamed memory (found during the clean-C rewrite):**
Per-object world->screen projection; X = object/column index into these 22-entry arrays:
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
