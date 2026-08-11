# Function / symbol rename backlog

Functions and memory cells whose current name (in `disasm/symbols.csv` → propagated everywhere
by the transpiler) contradicts what the code actually does, or which have no name at all.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth) and
regenerate (`make gen`). Hand-written twins (`rof_native.c`, `rof_native_amiga.cpp`,
`validate_native.c`, the Amiga platform layer, `transpile.py` comments) reference the names
directly and must be updated in the same pass.

⚠ **Whole-word replace misses the generated suffixes AND the `MEM_` prefix.** A `\bOLD\b` sweep
will NOT touch `OLD__t6502` / `OLD_core` / `test_OLD` (trailing `_`) **nor `MEM_OLD`** (leading
`_` — `\b` doesn't match between `_` and a letter). Sweep the **bare token without `\b`**
(`s/OLD/NEW/g`) so both `MEM_OLD` and the bare lvalue alias `OLD` are caught, then verify with
`grep -rn 'MEM_OLD\|\bOLD\b'`. (This bit the 2026-08-02 `player_lives`→`flight_mode_state` pass.)

> The backlog is otherwise **empty** (cleared 2026-08-02): every prior entry was investigated
> against the disasm and either renamed, had its `symbols.csv` comment corrected, or — for a
> genuinely dual-/multi-used cell — had **both roles documented directly in its `symbols.csv`
> description** (the source of truth), so the knowledge no longer needs a floating backlog.

## Backlog — found during the idiomatic-C rewrites (2026-08-03)

Surfaced while rewriting `game_state_update`, `setup_projection_params`, and the `ring_push*`
family into idiomatic C. Not yet renamed (needs a `symbols.csv` edit + `make gen` + twin sweep).

**Enemy-fire / target-blip state machine (`game_state_update` $A99C) — UNNAMED cells.**
The whole bolt line-plot working set is nameless. Suggested names:
- `$28EB` / `$28EC` — fire target cell (column / row) that seeds the bolt start → `fire_target_col` / `fire_target_row`.
- `$28F0` / `$28F2` — bolt plot start point (X / Y), seeded from the target cell → `fire_plot_x` / `fire_plot_y`.
- `$28EF` / `$28F1` / `$28F3` — `$80`-midpoint sub-pixel accumulators → `fire_plot_xfrac` / `fire_plot_yfrac` / `fire_plot_wfrac`.
- `$28F4` — bolt width seed (=1) → `fire_plot_width`.
- `$28F7` — random vertical step → `fire_plot_vstep`.
- `$28F8` — bolt row counter (seeded $FF) → `fire_plot_rowcount`.
- `$28F9` — bolt end row → `fire_plot_endrow`.
- `$28ED` — "shot queued" flag gating the fire → `fire_queued`.
- `$0624` — random mask AND-ed with RANDOM to reseed the fire countdown ($28EE) → `fire_delay_mask`.

**Misnamed / multi-role cells reused by the projection setup (`setup_projection_params` $AC93).**
These carry projection inputs here that have nothing to do with their `symbols.csv` names (classic
6502 ZP scratch sharing, like the `$0080-$008D` note below). Best fixed by documenting BOTH roles
in `symbols.csv` rather than renaming:
- `$0087`/`$0088` (`vbi_phase`/`vbi_flags`) — here = world-X fixed-point >> 4.
- `$0089`/`$008A` (`terrain_state`/`terrain_scroll_counter`) — here = world-Z >> 4.
- `$008B`/`$008C` (`dl_src_index`/`terrain_scroll_reload`) — here = pitch-depth delta << 2.
- `$0092` (`draw_row`) — here = heading_hi << 2.
- `$00A0-$00A3` (`draw_iter_count`/`scroll_accum_b0..b2`) — here = the sin/cos view vector.
- `$00A4`/`$00A5` (`scroll_accum_b3`/`scroll_accum_prev`) — here = the signed-pitch >> 1 per-row step.
- `$00A6` (`horizon_row_index`) — here = the horizon screen row (this one's name fits).
- `$28EE` (`lock_on_indicator_complete`) — in `game_state_update` it's the enemy-fire countdown
  timer, unrelated to lock-on completion. Dual role → document both in `symbols.csv`.
- `$0041` (`game_state`) — in `game_state_update` it's specifically the explosion/fire-frame counter.

**Genericly-named function.**
- `game_sub_55FC` ($55FC) — pushes entry Y (unmarked) into the $0719 event ring → `ring_push_y`
  or `ring_push_unmarked` (mirrors `ring_push_marked`).

**Door/viewport DL LMS vertical-scroll family (the Standby level-select "elevator" scroll + the
launch doors-open scroll).** These names describe an "index" / generic "terrain" op but actually
manipulate the launch DL's per-scanline mode-F LMS entries ($300A+) to scroll the $2000 door field
(or $1070 flight field) vertically. Found while implementing the post-mother-ship in-place level
scroll (2026-08-04).
- `dl_lms_fill` ($69F1) — writes the viewport DL's per-scanline LMS pointer words ($300A+, 3 bytes/
  scanline) from the row-address tables `$073D`/`$0793` (`row_base_lo`/`row_base_hi`), for row index
  X=`$008B`(`dl_src_index`) up to `$0086`(`row_count`). Not a generic "fill" — it's the LMS-window
  writer. → `dl_write_lms_window` (and `row_count` → `dl_lms_end_index`).
- `dl_lms_build` ($69E5) — set dest ptr $300A + end index $56, tail `dl_lms_fill`; i.e. rebuild the
  whole viewport LMS window from the current start index. → `dl_rebuild_lms_window`.
- `dl_index_dec` ($69E3) — DEC `$008B` then rebuild → scroll the viewport window DOWN one row (one
  smooth LMS scroll step). Name says "index dec" but the effect is a scroll step. → `dl_lms_scroll_step`.
- `dl_index_dec_or_reset` ($69DD) — reset `$008B`=0 then rebuild (no-scroll full window). → `dl_lms_reset_window`.
- `dl_src_index` (`$008B`) — the LMS-window START row index the above scroll through. → `dl_lms_start_row`.
- `scroll_terrain_dl` ($6953) — NOT generic terrain: the launch doors-OPEN split scroll (top half
  LMS entries shift up, bottom half shift down, splitting apart) via `dl_lms_scroll_up`/`_down` +
  `dl_lms_push_top`/`_bottom`. → `dl_doors_open_split_step`.

**Trig lookup tables + scratch (`sine_table_lookup` $9C55 / `trig_interp_lookup` $9BDB) — UNNAMED.**
- `$0077` / `$0078` — mid / hi bytes of the signed 24-bit trig result (`$0076` is `trig_result_lo`)
  → `trig_result_mid` / `trig_result_hi`.
- `$280E` — quadrant (angle >> 6) written by `sine_table_lookup` → `trig_quadrant`.
- `$280F` — the octant-fraction working copy consumed by `trig_interp_lookup` → `trig_octant_work`.
- `$2813-$2815` / `$2816-$2818` — the 24-bit angle / angle+1 samples, doubled per blend step
  → `trig_sample_angle` / `trig_sample_angle1`.
- `$9B98` / `$9B9C` — per-quadrant sign flag / index-reflect mask → `trig_quad_sign` / `trig_quad_reflect`.
- `$4EB9` / `$4EFA` — quarter-wave sine table hi / lo bytes → `trig_table_hi` / `trig_table_lo`.

## Backlog — found during the open-item triage (2026-08-10)

**`$00CD` `grafm_shadow` is NOT GRAFM — it is the SIZEP2 shadow.** `symbols.csv` describes it as
"GRAFM ($D00A) shadow pushed in the flight missile block", but **$D00A is SIZEP2** (GTIA: $D008-$D00B
= SIZEP0-3, $D00C = SIZEM, $D00D-$D010 = GRAFP0-3, $D011 = GRAFM). Both writers agree with SIZEP2 and
not with GRAFM:
- `build_player2_sprite $8C58` stores only `$00`/`$01`/`$03` into it (`$8da7`/`$8d9a`) — exactly the
  SIZEP2 encoding 1×/2×/4×, not an 8-bit missile bitmap.
- The flight VBI's `$5011-$5030` block is the **player-2** block (COLPM2 `$D014`, HPOSP2 `$D002`,
  `$D00A`, HPOSP3 `$D003`), not a missile block.
Suggested: `$00CD` → `sizep2_shadow`, and fix the description. Also correct the two stale comments in
`rof_native.c` `build_player2_sprite_core` (`bus_write(0xD014, …)` is labelled `SIZEP2` but is COLPM2;
`bus_write(0xD00A, …)` is labelled `GRAFP2` but is SIZEP2) and `rof_manual.c:61`'s `SIZEP2` label,
which happens to be right, on `$D00A`. This is the register the flight open item "laser-impact
explosion renders at 1× width" needs (`docs/flight-scene.md` / the `flight-scene` memory).

**ACTED ON 2026-08-11** (the widening is implemented; the rename itself is still pending): the
Amiga burst/P3 widening reads `mem[$00CD]` as SIZEP2 via `sizepScale()` in
`src/platform/amiga/RescueOnFractalus.cpp`, masking bits 1-0 exactly as GTIA does. ⚠ Do NOT compare
the shadow to `0`/`1`/`3` — `a800dumps/saucerbigpause.a8s` has `$00CD = $62` with no shot active
(stale zero page; `build_player2_sprite` is its only writer and had not run since the last shot).
Value `2` is also a second encoding of "normal", so only the masked pair is meaningful.

## Investigated → intentionally left UNNAMED (do not re-litigate)

These were verified (2026-08-02) and deliberately have no `symbols.csv` name: each is either
pure single-use scratch not worth a name, or multi-role scratch where any single name would
mislead the other callers. Listed so they aren't re-flagged as "unnamed → needs a name".

- `$0020` — pure ROL scratch in `flight_control_integrate` (`$9093-$90B4`, builds the `$28D6`
  fwd-step sign). No use outside that function.
- `$00B4` — dead store (single write `$9EA7` in `terrain_frame_setup`, never read).
- `$00B5` — cross-function terrain-math fold/shift scratch, reloaded fresh by ~15 terrain fns
  (~60 refs); any semantic name misleads all but one caller.
- `$00B6` — transient scratch in `terrain_frame_setup` + `FUN_b2cc/b33d`; no cross-frame lifetime.
- `$0039` — multi-role: target-latch index / sprite depth-lo (`build_player2_sprite`) / hi byte of
  an object-position accumulator; no single meaning.
- `$284E`/`$284F` — low bytes of the two 16-bit object-position accumulators in
  `flight_control_integrate` whose hi bytes are `$0038`/`$0039` (themselves multi-role).
- `$286A`/`$286B`/`$286C` — object/spawn-slot latch (flight-VBI `$517C-$5194`) **reused** as the
  sprite vertical-scale accumulator/step/limit in `build_player2_sprite` (`$8D33-$8D97`). Do NOT
  name `target_obj_*` — misleading.
- `$008E` — composer dest-row-ptr hi (`$808B`) AND a `step_accum_sub_7e` INC target; multi-role.

The `$0080-$008D` display/VBI/terrain ZP cells are additionally reused by the alien-creature
composer (documented in `docs/alien-jumpscare.md` + the twin comments); they keep their
primary-use names — the composer reuse is inherent 6502 ZP sharing, not a misnomer.
