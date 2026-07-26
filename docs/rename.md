# Function / symbol rename backlog

Functions and memory cells whose current name (in `disasm/symbols.csv` → propagated everywhere
by the transpiler) contradicts what the code actually does, or which have no name at all.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth) and
regenerate (`make gen`). Hand-written twins (`rof_native.c`, `rof_native_amiga.cpp`,
`validate_native.c`, the Amiga platform layer, `transpile.py` comments) reference the names
directly and must be updated in the same pass.

⚠ **Whole-word replace misses the generated suffixes.** A `\bOLD\b` sweep will NOT touch
`OLD__t6502` (validation oracle), `OLD_core` (hand-written core helper), or `test_OLD`
(harness test fn) because `_` is a word char. After a rename, grep for `OLD[A-Za-z0-9_]*`
and `[A-Za-z0-9_]*OLD` and fix those variants too.

> Everything that was confidently applicable has been applied (functions, alien-jump-scare
> cluster, music/SFX var rows, etc.). What remains below is **DEFERRED on purpose** — each entry
> either aliases a cell that is dual-used across subsystems, or is a rename/var-row suggestion that
> needs a live check before a canonical name can be trusted. Do NOT promote these without resolving
> the aliasing / verifying the claim noted. ✓=confident, ?=verify.

## Deferred: aliased / dual-use cells (a single name would mislead one of the users)

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
- `$0091` `altitude_threshold` — named for its `copy_title_text_block_to_screen` use, but in
  `sfx_seq_step` it is **dual-used as scratch** for the last voice-parameter command byte. Leave
  `altitude_threshold`; do not rename.
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

## Deferred: `sfx_voice_envelope_tick` ($548D) per-slot envelope arrays

The whole cluster overlaps itself and the `$068x` flight cells above — treat as one aliasing
puzzle, name together after auditing the stride/overlap. Cells: `$06DB`/`$06E9`
(`freq_env_step[]`/`_phase[]`), `$0679`(`hud_field_679`)/`$06BF`/`$06CD`
(`freq_value[]`/`_delta[]`/`_target[]`), `$06A3`/`$06B1` (`dur_env_step[]`/`_phase[]`),
`$066B`(`sfx_voice_distort_0e`)/`$0687`/`$0695` (`prio_value[]`/`_delta[]`/`_target[]`),
`$06F7` (`slot_event_id[]`). (`$5406 env_gate_table` was the one non-overlapping table — applied.)

## Deferred: dual-role player buffers `$0C32/$0D32/$0E32/$0F32`

The four parallel 89-byte column buffers ($0100 apart). Scrolling terrain-height columns in
flight; reused as the canopy A-pillars (P0 $0C32 / P1 $0D32, RLE-decoded) + sparse star-field
players (P2 $0E32 / P3 $0F32) during the launch/stars cinematic. A name must reflect the shared
player-buffer role, not just "terrain" (`terrain_col_buf0..3` vs `star_player_buf*`). Deferred
pending a role-neutral name.

## Deferred: door-frame drawer scratch `$0080/$0081`

Currently `sync_flag`/`dl_ptr_lo`, but in the plot/span path (`set_row_ptr`/`plot_masked_pixel`/
`fill_*_span`) they are the 16-bit screen row pointer (`row_ptr_lo`/`_hi`). The DL-setup path may
use the current names correctly — audit both before renaming.

## Deferred: pilot-rescue cluster corrections (found 2026-07-11)

- **`$2830` — unnamed.** = the queued-landing-target slot flag. `reset_pilot_state_if_no_2830 $495F`
  reads it: if `$2830==0` (no target queued) it also clears `landing_seq_flag $003D`. Suggested
  name: `landing_target_queued_2830`.
- **`$0047 colpf0_value` / `set_colpf0_from_flag $47A3` — symbols.csv comment is BACKWARDS.** The
  comment says "If Y bit5 set A=$CA else A=$0047"; the disasm (`$47a6 BNE`) is the opposite —
  **bit5 CLEAR selects $CA**, bit5 SET selects `colpf0_value`.
- **`$003E clear_colors_done_003E` — symbols.csv comment is misleading.** The loops in
  `clear_colors_sweep_5x $7A89` / `animate_clear_colors_timed $7A17` **continue while `$003E != 0`
  and exit at `$003E == 0`**. So `$003E` nonzero = "keep sweeping", zero = "done".
- **`$007C` — unnamed** (animate_clear_colors_timed). A pacing accumulator for the colour-clear
  stepper (pulled down by 7 each pass; re-seeds RTCLOK on exit). Suggested: `clear_sweep_delay_007C`.

## Deferred: `alien_knock_setup_loop` ($7EC7) unnamed cells (found 2026-07-12)

Set/used by `alien_knock_setup_loop` + `alien_creature_animate_draw`. Suggested names deferred
(need cross-referencing with the full SFX engine):
- `$005E`/`$005F` — SFX voice-slot indices (set 1/$0B then 4/$12); indexed into `$81E8`/`$81E2`.
- `$2921` — a second voice-slot index (set 2 then 5).
- `$2922`/`$2923`/`$2926` — recently-used voice-value history (reroll excludes `$2922`/`$2923`).
- `$2930` — current SFX pitch (descending sweep); `$2931` — pitch base offset (RANDOM&7 + $0C);
  `$292E` — zoom step count; `$2927`-`$292B` — per-voice shape/params from the `$81xx` tables.
- `$0635` — set $20; `$0637`/`$063A` — cleared; `$061A` — zoom base (read).

### The creature composer reuses these ZP cells with a DIFFERENT meaning than their names
`alien_shape_blit`/`alien_field*_fill` carry names from `display_list_build`/VBI code but the
composer tree reuses them as text-source state. Consider per-context aliases (or just document):
- `$0080 sync_flag` / `$0081 dl_ptr_lo` / `$0082 dl_ptr_hi` / `$0083 screen_ptr_lo` = the four field
  **cursors** (field 0/1/2/3 source index).
- `$0084 screen_ptr_hi` = the `reorder_cell_bits` **packing accumulator**.
- `$0085/$0086 encounter_count/row_count` = field-0 **source pointer** (lo/hi).
- `$0087/$0088 vbi_phase/vbi_flags` = field-1 **source pointer**.
- `$0089/$008A terrain_state/terrain_scroll_counter` = field-2 **source pointer**.
- `$008B/$008C dl_src_index/terrain_scroll_reload` = the composer **mask-row pointer**.
- `$008D/$008E step_mode_flag/(unnamed)` = the composer **dest-row pointer** (mask row + `$30`).
- `$292A/$292B/$292C` = the three per-frame voice values; `$2921/$005E/$005F/$2924/$2926` = voice
  positions/link indices; `$2922/$2923` = voice-C last-two picks; `$292D` = field-2 pack flag;
  `$292E` = SFX sustain counter; `$292F` = the draw-loop row counter; `$2930/$2931` = draw start row/col.

## Deferred: scratch reuse of named ZP cells (update_object_distance $6BED / draw_vline_pair $6C4D)

These routines reuse several cells purely as local scratch, so the cells' current symbol names are
MEANINGLESS in this context (the names come from their PRIMARY use in unrelated code):
- `$0084 screen_ptr_hi` — here the **draw fill-pattern byte** ($FF/$AA/$55). Suggested: `draw_fill_byte`.
- `$0085 encounter_count` — object-distance subtrahend hi byte, then the **plot column** `(X>>1)+2`.
- `$00B7 frame_counter` — object-distance-lo / draw row-counter scratch. Suggested: `obj_dist_lo`.
- `$00B8 draw_row_ptr2_hi` — object-distance hi byte / draw END-row. Suggested: `obj_dist_hi`.
- `$00B9 draw_pattern_byte` — object-distance minuend lo byte. Suggested: `obj_dist_in_lo`.
- `$0082 dl_ptr_hi` — draw_vline_pair sets it `$C0` purely as a "any 2bpp pack happened" marker.

## Deferred: `game_main_loop_body` ($3D48) unnamed OS-shadow + game-state cells

Written directly (raw `mem[]` / `bus_write`) with no `symbols.csv` name; most are standard Atari
OS shadow registers:
- `$0222`/`$0223` — **VVBLKI** vblank-immediate vector shadow (`$53CC` attract, then `$4FF5`
  flight). Suggested: `vvblki_lo`/`vvblki_hi`. ⚠ The Amiga `game_vbi_isr` dispatches on this vector.
- `$0200`/`$0201` — **VDSLST** DLI vector shadow ($49EE). Suggested: `vdslst_lo`/`vdslst_hi`.
- `$022F` — **SDMCTL** (DMA control shadow), cleared. Suggested: `sdmctl`.
- `$02C8` — **COLOR4/COLBK** background shadow, cleared. Suggested: `color_bak_shadow`.
- `$02C6`/`$02C7` — **COLOR2/COLOR3** shadows ($2C/$26). Suggested: `color2_shadow`/`color3_shadow`.
- `$026F` — **GPRIOR** priority shadow ($11). Suggested: `gprior_shadow`.
- `$28D9`/`$28DA` — both set `$80` at the level-clear handoff (object/anim state); unnamed.
- `$003A` — read at level-clear check (`==1` sets `level_cleared_flag`); role unclear.
- `$003D` — death/handoff phase byte (`!=0` → set to `2`); unnamed. Suggested: `death_phase`.
- `$066E`, and the `$0F1D`/`$0E8F`/`$0B31`/`$0020`/`$2830` block clears — need analysis.

## Verify-later comment fixes

- `$00CD grafm_shadow` — comment says "pushed to GRAFM ($D00A)…wing-clearance missile graphics",
  but flight-pmg-map suggests `$00CD` is the laser **SIZEP2**, not GRAFM. Verify against a live
  capture before rewording (kept as-is pending that check).
