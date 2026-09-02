# Function / symbol rename backlog

Functions and memory cells whose current name (in `disasm/symbols.csv` → propagated everywhere
by the transpiler) contradicts what the code actually does, or which have no name at all.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth) and
regenerate (`make gen`). Hand-written twins (`rof_native.c`, `rof_native_amiga.cpp`,
`validate_native.c`, the Amiga platform layer, `transpile.py` comments) reference the names
directly and must be updated in the same pass.

> **When you find a misnamed function, append a new section here** (address, current name, actual
> behaviour, suggested name) — per `CLAUDE.md` §Working conventions, file it the moment you find
> it, don't defer.
>
> This file is the *pending* list plus the durable rules below. Applied renames are **not** logged
> here — they are in `git log`.

**Pending: nothing.** The list below the rules is what has been *ruled out*, not what is waiting.

## How to apply a rename safely (the traps that have actually bitten)

⚠ **1. Whole-word replace misses the generated suffixes AND the `MEM_` prefix.** A `\bOLD\b` sweep
will NOT touch `OLD__t6502` / `OLD_core` / `test_OLD` (trailing `_`) **nor `MEM_OLD`** (leading
`_` — `\b` doesn't match between `_` and a letter). Sweep the **bare token without `\b`**
(`s/OLD/NEW/g`) so both `MEM_OLD` and the bare lvalue alias `OLD` are caught, then verify with
`grep -rn 'MEM_OLD\|\bOLD\b'`.

⚠⚠ **2. Sequential bare-token sweeps are ORDER-DEPENDENT.** Two ways this bites: one old name
being a PREFIX of another (`dl_index_dec` inside `dl_index_dec_or_reset`, so the short rule first
produces a plausible name that exists nowhere), and a **SWAP**, where each new name *is* another
old name and any ordering collapses both onto one. **Do the whole batch as a single pass with one
longest-first regex alternation** and a dict lookup per match — every occurrence is then rewritten
exactly once, which makes both hazards structurally impossible and needs no temp tokens:

```python
pat = re.compile('|'.join(sorted(MAP, key=len, reverse=True)))
new, n = pat.subn(lambda m: MAP[m.group(0)], text)
```

⚠⚠ **3. The sweep rewrites the `(was OLD)` provenance notes you just wrote.** If you put
"`… NOT an init (was pmg_missile_init)`" into a `symbols.csv` description and then sweep
`symbols.csv` too, the note becomes "`(was station_missile_drift)`" — a tautology reading as if
nothing changed. Write descriptions *after* the sweep (or exclude `symbols.csv` from it). Better
still: per `docs/rename.md`'s own rule and `feedback-src-comment-hygiene`, **state the rule, not
the history** — a description should say what the cell *is* ("NOT an altitude"), not what it used
to be called. Cheap detector for a row that names itself:

```
python3 -c 'import re,pathlib
for l in pathlib.Path("disasm/symbols.csv").read_text().splitlines():
    m,w = re.match(r"\$[0-9A-F]+,([a-z0-9_]+),",l), re.search(r"\(was ([A-Za-z0-9_]+)\)",l)
    if m and w and m.group(1)==w.group(1): print(l[:90])'
```

⚠ **4. `grep -r` in this repo skips gitignored files**, and `src/gen/rof_gen.c` +
`src/gen/rof_decl.h` are gitignored. Harmless for the sweep itself (they are regenerated) but it
means a `grep -r` survey **understates the blast radius and can report a false zero**. Enumerate
with a Python `rglob` scan when you need the real file list. Same family as
`feedback-verify-a-zero-result`.

⚠ **5. `listing.txt` is the FINAL image, so a boot-phase routine at an address may not be the
routine at that address.** Check `tools/xex_map.py` for a later XEX segment covering it before
concluding what a name refers to. (This is why boot `INITAD $5000` is the Lucasfilm logo, not
`stage_5000`.)

**The order of operations:**
1. Verify each proposed name against `disasm/listing.txt` **before** applying it — proposals have
   been wrong, and an existing description has asserted the exact opposite of the truth.
2. Edit `disasm/symbols.csv` with exact full-line replacements that **assert a unique match**, so a
   stale expectation fails loudly instead of silently doing nothing.
3. `make gen` — this regenerates `rof_gen.c` / `rof_decl.h` / `mem.h`, so those need no sweep.
   ⚠ Adding a *var* row is not cosmetic: it changes how the transpiler emits **every** access to
   that address (to `mem[MEM_<name> + i]` or a bare lvalue alias), so re-run the **full**
   `make validate`, not just `FN=<the renamed thing>`. (Renaming an existing var row does not
   change the emitted form, but run the full suite anyway — it is 7 minutes.)
4. Sweep the hand-written files (twins, probes, `.s`, `.gdb`, `transpile.py`, `docs/`).
5. **Sweep the prose too, not just the identifiers.** A token sweep leaves every comment that
   spells the concept in words — "the per-row pitch step", "stash the roll position", local
   variables named `pitch16`/`roll_rate` — saying the opposite of the code beside it. Grep the
   touched files for the bare English words afterwards.
6. `make validate` + `make hostproof` + a full `make clean` Amiga build.

## The multi-role convention (why some cells keep a "wrong" name)

6502 zero page is shared scratch: one cell routinely carries unrelated meanings in unrelated
routines. **A cell used by more than ~2 subsystems keeps its generic/first name and documents the
other roles in its `symbols.csv` description** — renaming it to fit one caller mislabels all the
others, and the description is where a reader actually looks. Only rename a cell that is genuinely
single-purpose.

## ⛔ Declined proposals — do not re-apply

- `row_count $0086` → ~~`dl_lms_end_index`~~. It is a **generic row/loop counter shared by ~8
  routines** (`display_list_build`'s 218-row loop, the terrain fns at `$77A8`/`$801D`/`$A41F`/
  `$B1D4-$B515`) that *also* happens to hold the LMS window end index. A `dl_lms_*` name would
  mislead every other caller.
- `dl_src_index $008B` → ~~`dl_lms_start_row`~~. Same reason, ~8 other users (`display_scroll`
  `$1CF0-$1D9A`, `$537C`, `$624E-$62C7`, `$77A4`, `$8077`). Both cells document the LMS role in
  their descriptions instead.
- `plot_step_lo`/`plot_step_hi` `$0050`/`$0051` → ~~`figure_scale_lo`/`_hi`~~, and the same for
  `plot_row_start_x $28DE`. The approach-figure zoom is **not** the only owner of the scaled-shape
  plotter's working set: the terrain ground-object rasteriser shares every cell of it
  (`terrain_plot_object_a/b` load the step from `$2300[X]`/`$232E[X]`,
  `set_plot_mask_and_halve_step` halves it, `raster_scaled_object` runs its accumulators off it,
  and `plot_step_hi < $0D` is the object DISTANCE gate — `docs/instruments.md`,
  `docs/flight-perf-log.md` §2.4). Any `figure_*` name mislabels that whole path, so the shared
  cells keep generic `plot_*` names. `figure_scale` is wrong twice over: the value is an INVERSE
  scale (bigger step = **smaller** figure). Both cells state the inverse-scale and 1/step² cost
  rule in their `symbols.csv` descriptions instead.

## Investigated → intentionally left UNNAMED (do not re-litigate)

These were verified and deliberately have no `symbols.csv` name: each is either pure single-use
scratch not worth a name, or multi-role scratch where any single name would mislead the other
callers. Listed so they aren't re-flagged as "unnamed → needs a name".

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

`$00C1`/`$00C2` (`row_table_stride`/`player_speed`) and `$004E`/`$004F`
(`terrain_pt_coord_b`/`_a`) are the same case in the other direction: their names are right for
`game_setup_7460` / `boot_standby_launch_driver` / the terrain point path that gave them, and
wrong for the scaled-shape plotter, which borrows them as its destination row pointer and its y/x
pixel cursor. They keep their names and document the borrowed roles in their descriptions — so a
future batch must not pick a plotter-specific name for any of the four.

The `$0080-$008D` display/VBI/terrain ZP cells are additionally reused by the alien-creature
composer (documented in `docs/alien-jumpscare.md` + the twin comments); they keep their
primary-use names — the composer reuse is inherent 6502 ZP sharing, not a misnomer.

## Station cinematic (scene 2) ZP roles

Every one of these cells has a name from a *different* subsystem, and in the attract scene it means
something else entirely. Verified against `disasm/listing.txt` `$1910-$1F2F`. None is worth
renaming on its own — like `$0086` they are genuinely polysemous 6502 ZP scratch — but a reader of
the station routines needs the mapping, and **a future batch rename must not pick a
station-specific name for any of them**.

| cell | current name | its STATION role |
|---|---|---|
| `$0089` | `terrain_state` | the animation's phase INDEX into the hold table `$1DE2` |
| `$008A` | `terrain_scroll_counter` | the per-phase hold TIMER (counts down to the next scroll step) |
| `$008B` | `dl_src_index` | the animation's phase COUNTER, 0..`$94` — also the render-side gate for the star re-decode |
| `$008C` | `terrain_scroll_reload` | `station_audio`'s melody step index, and `== 6` is `station_missile_drift`'s reseed trigger |
| `$008D` | `step_mode_flag` | the missile drift's `+2`/frame accumulator |
| `$008E`/`$008F` | (unnamed) / `sfx_toggle_8F` | the missiles' second, 16-bit `+$CB`/frame drift accumulator |
| `$0090`/`$0091` | `sfx_reinit_gate` / `altitude_threshold` | **two different station uses:** `station_star_fade_in`'s 16-bit walk pointer over `$2CB8-$3168`, and `station_sub_1E2A`'s P0 shape SOURCE pointer (with `$0092`/`$0093` the P1 one) |
| `$0094`/`$0095` | (unnamed) | the spacecraft shape's destination scanline, and its row count |
| `$0096`/`$0097` | `span_row_count` / `blit_row_counter` | `pmg_colors_station`'s 7-frame divider and its 8-entry table index |
| `$0098` | `dl_bottom_index` | the spacecraft animation's frame counter (stops at `$B4`) |
| `$009A`/`$009B` | `grid_offset_a` / `grid_offset_b` | the spacecraft's shape index (clamped to `$0C`) and its per-shape hold count |
| `$009C` | `draw_x_left` | the shape animation's ENABLE gate — `station_audio` sets it to 2 in RTCLOK phase 2, `station_pm_shape_tick` clears it when the animation ends |

## Method note: which evidence settles a "which way round is it?" question

Twice now a name's direction was argued from the disassembly and produced two confident,
mutually contradictory answers — the pitch/roll axes, and whether `$44D6`'s blob was an altimeter
pointer or the scanner dot. Both were settled in minutes by a **user flight report**, not by more
static reasoning. When a rename hinges on which of two physical quantities a cell holds, fly it.

Two cheaper disproofs worth trying before that: look for the **original's own compensation table**
(if it equals what your model implies, the model is confirmed from the binary alone — see
`flight-scene` §Wide objects), and read a **captured register dump offline** rather than doing
another emulator run.

## `event_sequence_dispatcher` ($4644) — the header comment's slot map is wrong

The command-key table at `$4816` is `00 3F 15 3E 07 06 80 1C`, i.e. slot 0 = L (Land), 1 = A (Air
Lock), 2 = **B (Boosters)**, 3 = S (Systems), 4/5 = cursor right/left (thrust), 6 = BREAK (`$80`),
7 = ESC (`$1C`).  The native twin's behaviour follows the table correctly; only the comment block
above it mislabels the slots.  Fix the comment, not the code.

