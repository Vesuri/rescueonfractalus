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

**Trig lookup tables + scratch (`sine_table_lookup` $9C55 / `trig_interp_lookup` $9BDB) — UNNAMED.**
- `$0077` / `$0078` — mid / hi bytes of the signed 24-bit trig result (`$0076` is `trig_result_lo`)
  → `trig_result_mid` / `trig_result_hi`.
- `$280E` — quadrant (angle >> 6) written by `sine_table_lookup` → `trig_quadrant`.
- `$280F` — the octant-fraction working copy consumed by `trig_interp_lookup` → `trig_octant_work`.
- `$2813-$2815` / `$2816-$2818` — the 24-bit angle / angle+1 samples, doubled per blend step
  → `trig_sample_angle` / `trig_sample_angle1`.
- `$9B98` / `$9B9C` — per-quadrant sign flag / index-reflect mask → `trig_quad_sign` / `trig_quad_reflect`.
- `$4EB9` / `$4EFA` — quarter-wave sine table hi / lo bytes → `trig_table_hi` / `trig_table_lo`.

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
