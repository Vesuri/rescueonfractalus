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
