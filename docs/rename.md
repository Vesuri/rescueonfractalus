# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate (`make gen`). Hand-written twins (`rof_native.c`, `rof_native_amiga.cpp`,
`validate_native.c`, the Amiga platform layer, `transpile.py` comments) reference the names
directly and must be updated in the same pass. Each entry: address, current name, what it
really does, suggested name.

⚠ **Whole-word replace misses the generated suffixes.** A `\bOLD\b` sweep will NOT touch
`OLD__t6502` (validation oracle), `OLD_core` (hand-written core helper), or `test_OLD`
(harness test fn) because `_` is a word char. After a rename, grep for `OLD[A-Za-z0-9_]*`
and `[A-Za-z0-9_]*OLD` and fix those variants too. (Conversely this is what keeps distinct
siblings like `clear_colors_timed`/`_sweep_5x`/`_done_003E` safe when renaming `clear_colors`.)

## Open: function renames

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|
| _(none — drained 2026-07-09; add new rows here)_ | | | |

## Notes
- **"scroll" in the `scroll_*` names = Atari LMS / buffer-shift scrolling**, not pixel
  motion: `scroll_terrain_dl` ($6953) shifts the viewport DL's per-scanline LMS pointers so
  the door halves slide apart vertically (`dl_lms_scroll_up`/`down` $69A9/$69C3);
  `scroll_field_columns` ($6AEE) shifts the $0C32-$0F32 buffers left a column (horizontal
  starfield scroll). The tunnel rings "rushing in" are **palette cycling** ($08D4-$08D9
  rotation), NOT a scroll.

## Open: unnamed memory locations (add `symbols.csv` var rows)

RAM/ZP cells read/written by the code but with **no `symbols.csv` var row** (the transpiler
emits them as raw `mem[0xNNNN]`). Add var rows so the native + transpiled code read as named
state. The confident, conflict-free cells were drained 2026-07-09 (see the Applied log).
**What remains here is DEFERRED on purpose** — each entry aliases a cell that is dual-used
across subsystems or needs a live check before a canonical name can be trusted. ✓=confident,
?=verify. Do NOT promote these without resolving the aliasing noted.

**Misnamed (rename in a future batch):**
- `$2840` `wing_bar_hpos_base` → **`crosshair_hpos_base`** (named 2026-06-30b, WRONG). It is the
  Targeting-Crosshair (#10) HPOS base, NOT the wing-clearance bars (those are the mode-D band
  BITMAP, per CLAUDE.md #3). The flight VBI ($505F) pushes `HPOSM3=mem[$2840]`, `M2=+$0C`, `M1=+$11`
  (the "+" reticle's stem + arms); `$A49A` sets it `= ($28FC==0) ? $00 : $74` = the visibility gate
  ($00 pushes the missiles off-screen). Confirmed: `flight1.bin`=$00 (hidden), `flight_saucer1`/
  `flight_emplacement_scope`=$74 (visible). Also fix the `$00CD grafm_shadow` comment ($00CD is the
  laser SIZEP2, per flight-pmg-map) and `$00CB hposp2_shadow` "wing-clearance" prose.

**Aliased / dual-use cells (a single name would mislead one of the users):**
- `$0037` (`cockpit_dial_update`/`vbi_handler_flight`) — pushed to COLPF1 ($D014) but also
  force-set to `$78` in the target-latch reset. Dual use unclear — verify before naming
  (`colpf1_shadow_or_terrain_h`?).
- `$0020` — used as `cockpit_dial_update` DL scratch AND as pure ROL scratch for the
  `$28D6`/fwd-step sign in `flight_control_integrate`. Keep the DL name; do not rename.
- `$00BB/$00BC/$00BD` (`dl_y1`/`dl_y2`/`dl_y3`) — in `draw_object_column` these are the
  current column's cell pointer (`dial_col_ptr_lo`/`_hi`) + loop counter (`dial_col_counter`),
  NOT DL Y coords. They may be genuine DL coords elsewhere — verify both uses before renaming.
- `$00A0-$00A3` (`draw_iter_count`/`scroll_accum_b0..b2`) — dual-used in `terrain_frame_setup`
  as the view-transform rotation vector `{rot_a=$A1:$A0, rot_b=$A3:$A2}`. Wants a union/overlay,
  not a flat rename.
- `$00B5` — `proj_fold_scratch` in `project_terrain_points`, but also loop-1/loop-2 scratch in
  `terrain_frame_setup`. Cross-function scratch; naming for one misleads the other.
- `$00B4`/`$00B6` — transient scratch in `terrain_frame_setup` (written once, dead within the
  fn). Low value; skip unless a cleanup needs them.
- `$0686`/`$0687` (would-be `engine_sound_pitch_a`/`_b` in `flight_control_integrate`) and
  `$066C`/`$066D` (would-be `engine_state_a`/`_b`) — these fall inside the `sfx_voice_envelope_tick`
  per-slot arrays ($0679/$066B/$0687/$0695/… each base+Y, ~13 entries). The flight code reads
  them as scalars, the audio code as array elements. Genuinely aliased — resolve which owns the
  bytes before naming either side.
- `$2829`/`$0068` and `$282C`/`$0069` — 16-bit accumulators used as object-position accumulators
  in `flight_control_integrate` (`obj_accum_*`) AND as the random jitter offsets in
  `terrain_jitter_column` (`obj_jitter_x/y_*`). One name can't cover both.
- `$284E`/`$0038` (`vobj_row_count`) and `$284F`/`$0039` (would-be `obj_pos2_*`) — `$0038`/`$0039`
  already carry other names and `$0039` is also a target-latch index. Dual-context; verify.
- `$0039`/`$286A`/`$286B`/`$286C` (would-be `target_obj_*`, latched target object indices/coords)
  and `$003A` bit7 (gates the shields-cell update) — `$0039` clash above; `$003A` role unclear.
  Verify the target-latch block before naming.

**Music note-stream player state (`music_player_tick $7253` + `music_init_state $7238`)** —
all confident (behaviour fully traced 2026-07-10 during the native rewrite); add `symbols.csv`
var rows. Four voices, indexed by byte offset `x = voice*2` (voices 0-3). ✓
- `$0648` (stride 2: `$0648/$064A/$064C/$064E`) → **`music_env_level`** — per-voice software
  envelope amplitude (0-127); integrated each tick, emitted as `AUDC = (level>>3) EOR distortion`.
- `$0649` (stride 2: `$0649/$064B/$064D/$064F`) → **`music_env_delta`** — per-voice envelope
  slope added to the level each tick (+attack on note-on, −release once the note timer expires).
- `$0650` (stride 2: `$0650/$0652/$0654/$0656`) → **`music_voice_audf`** — per-voice POKEY AUDF
  (pitch), loaded from the preset table on an instrument command, written to `$D200+x` on note-on.
- `$0659` → **`music_note_on_level_c0`** — envelope level loaded on a `11` (`$C0`) voice code.
- `$065A` → **`music_note_on_level_80`** — envelope level loaded on a `10` (`$80`) voice code.
- `$065B` → **`music_attack_delta`** — envelope slope loaded into `music_env_delta` on note-on.
- `$065C` → **`music_release_delta`** — envelope slope all voices switch to when the note timer
  reaches 0 (the note's decay/release phase).
- `$7375` (table, 4 bytes/entry, indexed `(~cmd)*4`) → **`music_instrument_audf_table`** — AUDF
  preset per instrument command (`$C0..$FF`); 4 bytes = the four voices' pitches (voice 3 first).
- `$73C1` (table, stride 2) → **`music_audc_distortion_table`** — per-voice POKEY AUDC distortion
  bits, EORed with the volume nibble before the `$D201+x` write.

Also: the `$7238 music_init_state` var-row comments in `symbols.csv` are **stale** — they say
`$0653=$0655=2`, but the code sets `$0651=0`, `$0653=1`, `$0655=1` (verified against the
disassembly + the native twin). Fix the `$0651`/`$0653` comments in the same pass.

**SFX theme sequencer state (`sfx_voice_tick $70F9` + `sfx_seq_step $7148`)** — the
attract/standby-theme player (distinct from the note-stream `music_player_tick` above and the
in-flight SFX engine `$548D`). Traced 2026-07-10 during the native rewrite. ✓
- `$0091` is `altitude_threshold` (named for its `copy_title_text_block_to_screen` use), but in
  `sfx_seq_step` it is **dual-used as scratch** for the last voice-parameter command byte. A single
  name misleads one caller — leave `altitude_threshold` and note the aliasing (do not rename).
- `$71DB` (byte stream) → **`sfx_seq_stream`** — the theme "score": note bytes (bit7 clear),
  voice-parameter commands (bit7 set), `$00` = end/loop-to-start marker.
- `$71D2` (table, indexed `note & $1F`) → **`sfx_note_duration_table`** — per-note duration
  loaded into `sfx_note_timer $073A`.
- `$71AB`/`$719E`/`$7191`/`$71B8` (tables, indexed by the command's low 5 bits) →
  **`sfx_audf1_table`/`sfx_audf2_table`/`sfx_audf3_table`/`sfx_audf4_table`** — the four POKEY
  AUDF (pitch) presets per voice-parameter command.
- `$71C5` (table, same index) → **`sfx_audc4_table`** — the AUDC4 preset; a `0` here also acts as
  a rest (ends the command scan with the note treated as silent).

**`sfx_voice_envelope_tick` ($548D) — per-slot envelope arrays (base+Y, Y=$0E..1):** the whole
cluster overlaps itself and the `$068x` flight cells above — treat as one aliasing puzzle, name
together after auditing the stride/overlap. Cells: `$06DB`/`$06E9` (`freq_env_step[]`/`_phase[]`),
`$0679`(`hud_field_679`)/`$06BF`/`$06CD` (`freq_value[]`/`_delta[]`/`_target[]`), `$06A3`/`$06B1`
(`dur_env_step[]`/`_phase[]`), `$066B`(`sfx_voice_distort_0e`)/`$0687`/`$0695`
(`prio_value[]`/`_delta[]`/`_target[]`), `$06F7` (`slot_event_id[]`). (`$5406 env_gate_table` was
the one non-overlapping table here — already applied.)

**Dual-role player buffers — `$0C32/$0D32/$0E32/$0F32`:** the four parallel 89-byte column
buffers ($0100 apart). Scrolling terrain-height columns in flight; reused as the canopy A-pillars
(P0 $0C32 / P1 $0D32, RLE-decoded) + sparse star-field players (P2 $0E32 / P3 $0F32) during the
launch/stars cinematic. A name must reflect the shared player-buffer role, not just "terrain"
(`terrain_col_buf0..3` vs `star_player_buf*`). Deferred pending a role-neutral name.

**Door-frame drawer scratch — `$0080/$0081`:** currently `sync_flag`/`dl_ptr_lo`, but in the
plot/span path (`set_row_ptr`/`plot_masked_pixel`/`fill_*_span`) they are the 16-bit screen row
pointer (`row_ptr_lo`/`_hi`). The DL-setup path may use the current names correctly — audit both
before renaming.

**Description-only enhancements (keep the name, fix the `symbols.csv` note):**
- `$0089` `terrain_state` — in `scroll_field_columns` it is the scroll-phase gate (0=idle,
  <4=scroll every frame, >=4=distance-accumulator-paced, reset to 2 at distance 100). Note the
  stars/planet scroll-pacing role.
- `scroll_field_columns` ($6AEE) — accurate for the data, but the note hides that the same
  column shift renders as a VERTICAL star scroll when the buffers are the star players.

---

# Applied log (newest first)

## Applied 2026-07-09 (batch var rows + function renames)
`symbols.csv` → `make gen` → `make validate` PASS (0 mem mismatch on every twin).

**Function renames** (`symbols.csv` + all hand-written referrers — `rof_native.c`,
`rof_native_amiga.cpp`, `PlatformAmiga.cpp`, `platform_c.h`, `validate_native.c`,
`transpile.py` comments, `docs/memory-map.md`, `docs/startup-flow.md`; plus the `__t6502`
oracle / `_core` helper / `test_*` harness variants):
- `clear_colors`→`wait_frames_1` ($3CC3) — PHA + `LDA #$01` falling into the frame-wait; waits
  1 frame preserving A. NOT a colour clear (the old note was wrong). (Distinct siblings
  `clear_colors_timed`/`_sweep_5x`/`_done_003E` are real colour clearers — left untouched.)
- `push_a_wait_frames`→`wait_frames` ($3CB1).
- `wait_frames_4c`→`wait_timer_4c_frames` ($3CB2) — the `4c` was the `$004C` address it reads.
- `wait_setcount`→`wait_frames_set_count` ($3CC6).
- `timer_4C`→`frame_wait_count` ($004C, var) — the frame-wait target count.
- `game_sub_4258`→`lock_on_indicator_fill_cells` ($4258) — lights all six lock-on glyphs.
- `obj_state_dispatch_0043`→`lock_on_indicator_dispatch` ($4225).
- `plot_glyph_pixel_masked`→`plot_masked_pixel` ($66DE) — no glyph; OR/ANDs one 2-bit pixel.
- `draw_shape_rows_loop`→`draw_frame_guide_columns` ($6620) — three vertical guide columns.

Also fixed the `$007E lock_on_indicator_state` note (state machine 0/1..7/$80/$81.., NOT a
"score/rescued-pilot counter").

**New var rows** (confident, conflict-free cells from the unnamed-memory backlog):
- cockpit dials: `$0625 dial_base_offset`, `$4457 dial_bar_value_table`,
  `$4581 dial_column_ptr_table`.
- vbi_handler_flight shadows: `$00CB hposp2_shadow`, `$00CD grafm_shadow`,
  `$2845 p3_object_state`, `$006A p3_object_mode`, `$3356 cockpit_shields_cell`,
  `$3357 cockpit_shields_cell_hi`, `$062C static_dither_threshold`.
- atmosphere fade: `$08A1 atmo_fade_countdown`, `$08A2 atmo_fade_phase`, `$08A3 atmo_band_base`,
  `$00DB terrain_pen1_fade`, `$07F9 atmo_audc_table`, `$0823 atmo_pen0_table`,
  `$084D atmo_pen1_table`, `$0877 atmo_anim_table`, `$364B atmo_fade_step_table`,
  `$07E9 attract_palette_src`.
- flight_control_integrate: `$005D ground_proximity_flag`, `$003D landing_seq_flag`,
  `$0023/$0024 pitch_shadow_lo/hi`, `$002B/$002C world_dx_lo/hi`, `$2881/$2882 world_dz_lo/hi`,
  `$2883/$2884 fwd_step_lo/hi`, `$28D6 roll_mag_scaled`, `$283C landing_inhibit_flag`,
  `$283D heading_freeze_flag`, `$2917 lockon_rand_countdown`,
  `$2871/$2873 canopy_pillar_y_left/right`, `$2850-$2853 obj_vel_x_lo/hi`/`obj_vel_y_lo/hi`,
  `$2919-$291D ring_cur_0..4`, `$291E ring_head`, `$2893 ring_pitch_lo`, `$289A ring_pitch_hi`,
  `$28A1 ring_roll_vel`, `$28A8 ring_pillar_l`, `$28AF ring_pillar_r`.
- update_terrain_scanline_proj: `$2270-$2273 map_x/z_scratch_lo/hi`,
  `$2801-$2804 map_x/z_mirror_lo/hi`, `$2274 scaled_depth_lo`, `$27F9 terrain_height_q2`,
  `$281A/$281B viewport_top/bottom_row`, `$0070 terrain_clearance`, `$2879 proj_phase_flag`.
- project_terrain_points: `$24B4 obj_proj_flags`, `$22A4/$22D2 obj_num1_lo/hi`,
  `$235B/$2388 obj_num2_lo/hi`, `$2300/$232E obj_divisor_lo/hi`,
  `$2400/$242D obj_screen_x_lo/hi`, `$245A/$2487 obj_screen_y_lo/hi`,
  `$270E/$272D band_scroll_offset0/1`.
- terrain_frame_setup: `$22A3/$22D1 obj_num1_in_lo/hi`, `$22FF/$232D obj_divisor_in_lo/hi`,
  `$2276 obj_col_index`, `$23B5 obj_height_sample`, `$0900 terrain_height_map`,
  `$B67C obj_draw_order`, `$28DB collapse_cur_obj`.
- data tables: `$6B5F terrain_height_table`, `$6E0F frame_span_thickness_tbl`,
  `$5406 env_gate_table`.

(`$026F` prior_shadow and `$0632` static_enable_flag were dropped — already named GPRIOR /
var_0632. `$2275` scaled_depth_hi already existed, confirming `$2274 scaled_depth_lo`.)

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

---

**`$5978 engine_sound_update` → `standby_level_select_loop`** (found 2026-07-10). NOT engine
sound. It is the **Standby / Title-Screen idle loop with the starting-level select**: reads
joystick up/down (PORTA `$D300` bits 0/1) + SELECT (CONSOL `$D01F` bit1) + the SHIFT-key
status (SKSTAT `$D20F` bit3, active-low), and increments/decrements `level_stage $006D`
(`$59CC` decrement / `$59E4` increment, wrap at `level_progress $37EE`), re-rendering the
STARTING-LEVEL digit via `setup_initials_ptr $5A63`. Loops (mutual tail-recursion with
`sound_check_trigger $5A17`, TCO'd) once per ~2 frames (`wait_frames_2 $3CCA`) until a
console/trigger delta launches the game. Called from `cockpit_display $587B` tail. (The
"engine sound" name likely came from the `$006D` ramp resembling a pitch ramp.) Its callee
`sound_check_trigger $5A17` and the `$5A0E sound_stop` are part of the same loop, not audio.

---

**Pilot-rescue cluster corrections (found 2026-07-11, native-izing pilot_render group):**
- **`$2830` — unnamed.** = the queued-landing-target slot flag. `reset_pilot_state_if_no_2830 $495F`
  reads it: if `$2830==0` (no target queued) it also clears `landing_seq_flag $003D`. Suggested
  name: `landing_target_queued_2830`.
- **`$0047 colpf0_value` / `set_colpf0_from_flag $47A3` — symbols.csv comment is BACKWARDS.** The
  comment says "If Y bit5 set A=$CA else A=$0047"; the disasm (`$47a6 BNE`) is the opposite —
  **bit5 CLEAR selects $CA**, bit5 SET selects `colpf0_value`. Verified via `make validate`.
- **`$003E clear_colors_done_003E` — symbols.csv comment is misleading.** It says "Set non-zero to
  abort wait_frames_1 sweep loops"; the loops in `clear_colors_sweep_5x $7A89` /
  `animate_clear_colors_timed $7A17` actually **continue while `$003E != 0` and exit at `$003E == 0`**
  (`$7a94`/`$7a66` `LDA $3E; BEQ return`). So `$003E` nonzero = "keep sweeping", zero = "done". This is
  the flag the Systems key toggles and the pilot_render L_78d6↔L_792e hold-loop exit (see [[flight-scene]]).

**`$007C` — unnamed** (found 2026-07-11, animate_clear_colors_timed). A pacing accumulator
for the colour-clear stepper: `animate_clear_colors_timed $7A17` pulls it down by 7 each pass
(when the step counter `$7B < $0F`) and re-seeds RTCLOK ($0014) from it on exit, so it sets the
inter-tick delay of the sweep. Suggested name: `clear_sweep_delay_007C`.

**game_sub_7EC7 ($7EC7) unnamed cells** (found 2026-07-12, native-izing the rescue SFX/zoom
setup). These are set/used by `game_sub_7EC7` + `game_sub_7F85`:
- `$005E`/`$005F` — SFX voice-slot indices (set 1/$0B then 4/$12); indexed into `$81E8`/`$81E2`.
- `$2921` — a second voice-slot index (set 2 then 5).
- `$2922`/`$2923`/`$2926` — recently-used voice-value history (reroll excludes `$2922`/`$2923`).
- `$2924 sound_table_idx` — already named; `$2930` — current SFX pitch (descending sweep);
  `$2931` — pitch base offset (RANDOM&7 + $0C); `$292E` — zoom step count; `$2927`-`$292B` —
  per-voice shape/params from the `$81xx` tables.
- `$0635` — set $20 (unnamed); `$0637`/`$063A` — cleared (unnamed); `$061A` — zoom base (read).
  Suggested names deferred (need cross-referencing with the full SFX engine).

**update_object_distance ($6BED) / draw_vline_pair ($6C4D) — scratch reuse of named ZP cells**
(found 2026-07-22, clean-C rewrite of both twins). These routines reuse several cells purely as
local scratch, so the cells' current symbol names are MEANINGLESS in this context (the names come
from their PRIMARY use in unrelated code):
- `$0084 screen_ptr_hi` — here it is the **draw fill-pattern byte** ($FF/$AA/$55 in
  update_object_distance; the byte draw_vline_pair stores for rows < $2B). Not a screen pointer.
  Suggested (context) name: `draw_fill_byte`.
- `$0085 encounter_count` — here it is the **object-distance subtrahend high byte** on entry to
  update_object_distance, then overwritten by draw_vline_pair with the **plot column** `(X>>1)+2`.
  Not the encounter counter.
- `$00B7 frame_counter` — here it is **object-distance-lo / draw row-counter scratch**. Not the
  VBI frame counter. Suggested: `obj_dist_lo` (or a generic `draw_row_scratch`).
- `$00B8 draw_row_ptr2_hi` — here it is the **object-distance high byte / draw END-row**. Suggested:
  `obj_dist_hi`. (symbols.csv already notes this reuse.)
- `$00B9 draw_pattern_byte` — here it is the **object-distance minuend low byte** (the object's
  current 16-bit distance lo, set by advance_object_positions). Suggested: `obj_dist_in_lo`.
- `$00BA obj_pos_hi` — the object-distance minuend high byte; name is acceptable.
- `$0082 dl_ptr_hi` — draw_vline_pair sets it to `$C0` purely as a "any 2bpp pack happened" marker
  (from plot_pixel_2bpp's BIT $0082), not a display-list pointer here.

Not misnamed but worth confirming: `draw_vline_pair` draws a column and its `$2F`-mirror as a
symmetric pair of vertical spans (the approaching-object trail) — the name is fine.

**game_main_loop_body ($3D48) — unnamed OS-shadow + game-state cells** (found 2026-07-22, clean-C
rewrite). These are written directly (raw `mem[]` / `bus_write`) with no `symbols.csv` name; most are
well-known Atari OS shadow registers and should get standard names:
- `$0222`/`$0223` — **VVBLKI** vertical-blank-immediate vector shadow (set to `$53CC` attract, then
  `$4FF5` flight). Suggested: `vvblki_lo`/`vvblki_hi`. ⚠ The Amiga `game_vbi_isr` dispatches on this
  live vector — NOT dead.
- `$0200`/`$0201` — **VDSLST** display-list-interrupt vector shadow (set to `$49EE`). Suggested:
  `vdslst_lo`/`vdslst_hi`.
- `$022F` — **SDMCTL** (DMA control shadow), cleared. Suggested: `sdmctl`.
- `$02C8` — **COLOR4/COLBK** background-colour shadow, cleared. Suggested: `color_bak_shadow`.
- `$02C6`/`$02C7` — **COLOR2/COLOR3** shadows (set `$2C`/`$26`). Suggested: `color2_shadow`/`color3_shadow`.
- `$026F` — **GPRIOR** priority shadow (set `$11`). Suggested: `gprior_shadow`.
- `$066E` — cleared once at the level-clear handoff (part of a `$066B`+ block); unnamed. Needs analysis.
- `$28D9`/`$28DA` — both set to `$80` at the level-clear handoff (object/anim state); unnamed.
- `$0F1D`+ (0xA3 bytes) / `$0E8F`+ (0x1F bytes) — PMG/player buffers cleared with a `wait_frames`
  residue byte at the handoff; part of the `$0Cxx`-`$0Fxx` player pages.
- `$0B31`+ (0x57 bytes) — missile/DMA buffer region cleared per level; part of the `$0B00` PMG page.
- `$0020`+ (0x2C bytes) / `$2830`+ (0xA6 bytes) — per-level ZP + object-state clears.
- `$003A` — read at level-clear check (`==1` sets `level_cleared_flag`); unnamed. Needs analysis.
- `$003D` — death/handoff phase byte (`!=0` → set to `2`); unnamed. Suggested: `death_phase`.
