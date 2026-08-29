# Function / symbol rename backlog

Functions and memory cells whose current name (in `disasm/symbols.csv` → propagated everywhere
by the transpiler) contradicts what the code actually does, or which have no name at all.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth) and
regenerate (`make gen`). Hand-written twins (`rof_native.c`, `rof_native_amiga.cpp`,
`validate_native.c`, the Amiga platform layer, `transpile.py` comments) reference the names
directly and must be updated in the same pass.

> ## ✅ THE BACKLOG IS EMPTY (cleared 2026-08-12)
>
> Every entry filed on 2026-08-03, -08-10 and -08-12 has been **applied** — see
> §"Applied 2026-08-12" for the list, including the **two proposals that were deliberately
> declined** and why. Previous clearings: 2026-08-02.
>
> **When you find a misnamed function, append a new section here** (address, current name, actual
> behaviour, suggested name) — per `CLAUDE.md` §Working conventions, file it the moment you find
> it, don't defer.

## How to apply a rename safely (the two traps that have actually bitten)

⚠ **1. Whole-word replace misses the generated suffixes AND the `MEM_` prefix.** A `\bOLD\b` sweep
will NOT touch `OLD__t6502` / `OLD_core` / `test_OLD` (trailing `_`) **nor `MEM_OLD`** (leading
`_` — `\b` doesn't match between `_` and a letter). Sweep the **bare token without `\b`**
(`s/OLD/NEW/g`) so both `MEM_OLD` and the bare lvalue alias `OLD` are caught, then verify with
`grep -rn 'MEM_OLD\|\bOLD\b'`. (This bit the 2026-08-02 `player_lives`→`flight_mode_state` pass.)

⚠⚠ **2. A bare-token sweep is ORDER-DEPENDENT when one old name is a PREFIX of another.**
`dl_index_dec` is a prefix of `dl_index_dec_or_reset`, so sweeping the short one first turns
`dl_index_dec_or_reset` into `dl_lms_scroll_step_or_reset` — a plausible-looking name that exists
nowhere. **Substitute the LONGEST old name first**, and assert up front that no *new* name contains
any *old* name as a substring (or a later rule re-substitutes inside a name the pass just wrote).
The 2026-08-12 pass automated both checks in its sweep script.

⚠⚠ **3. The sweep rewrites the `(was OLD)` provenance notes you just wrote.** If step 2 puts
"`… NOT an init (was pmg_missile_init)`" into a `symbols.csv` description and step 4 then sweeps
`symbols.csv` as well, the note becomes "`(was station_missile_drift)`" — a tautology that reads as
if nothing changed. This bit the 2026-08-12 pass on **all ten** renamed rows. Either write the
provenance notes *after* the sweep, or exclude `symbols.csv` from it. Cheap detector:

```
# any row whose name equals its own "(was …)" note
python3 -c 'import re,pathlib
for l in pathlib.Path("disasm/symbols.csv").read_text().splitlines():
    m,w = re.match(r"\$[0-9A-F]+,([a-z0-9_]+),",l), re.search(r"\(was ([A-Za-z0-9_]+)\)",l)
    if m and w and m.group(1)==w.group(1): print(l[:90])'
```

**The order of operations that worked** (2026-08-12, 198 substitutions across 21 files):
1. Verify each proposed name against `disasm/listing.txt` **before** applying it — several
   2026-08-03 proposals were right, but two were not (below), and one existing description
   asserted the exact opposite of the truth (`$00CD`).
2. Edit `disasm/symbols.csv` with exact full-line replacements that **assert a unique match**, so a
   stale expectation fails loudly instead of silently doing nothing.
3. `make gen` — this regenerates `rof_gen.c` / `rof_decl.h` / `mem.h`, so those need no sweep.
   ⚠ Adding a *var* row is not cosmetic: it changes how the transpiler emits **every** access to
   that address (to `mem[MEM_<name> + i]` or a bare lvalue alias), so re-run the **full**
   `make validate`, not just `FN=<the renamed thing>`.
4. Sweep the hand-written files (twins, probes, `.s`, `.gdb`, `transpile.py`, `docs/`).
5. `make validate` + a full `make clean` Amiga build.

## The multi-role convention (why some cells keep a "wrong" name)

6502 zero page is shared scratch: one cell routinely carries unrelated meanings in unrelated
routines. **A cell used by more than ~2 subsystems keeps its generic/first name and documents the
other roles in its `symbols.csv` description** — renaming it to fit one caller mislabels all the
others, and the description is where a reader actually looks. Only rename a cell that is genuinely
single-purpose.

## Applied 2026-08-12

**Renamed — functions** (all verified against `listing.txt` first):

| Addr | Was | Now |
|---|---|---|
| `$55FC` | `game_sub_55FC` | `ring_push_unmarked` |
| `$6953` | `scroll_terrain_dl` | `dl_doors_open_split_step` |
| `$69DD` | `dl_index_dec_or_reset` | `dl_lms_reset_window` |
| `$69E3` | `dl_index_dec` | `dl_lms_scroll_step` |
| `$69E5` | `dl_lms_build` | `dl_rebuild_lms_window` |
| `$69F1` | `dl_lms_fill` | `dl_write_lms_window` |
| `$1910` | `pmg_missile_init` | `station_missile_drift` |
| `$1E01` | `dli_handler_station` | `station_pm_shape_tick` |
| `$1E79` | `pmg_update_station` | `station_star_fade_in` |

`$55FC` takes the **Y** register and pushes it **unmarked**; `ring_push_marked $5815` takes **X**
and sets bit 7. Both fall into the shared tail `ring_push_0719 $55FF`. `ring_push_unmarked` was
chosen over the also-proposed `ring_push_y` to mirror the existing family name; the register is in
the description.

**Renamed — one variable.** `$00CD` `grafm_shadow` → **`sizep2_shadow`**. `$D00A` **is** SIZEP2
(`$D008-$D00B` = SIZEP0-3; GRAFM is `$D011`), so both the old name and its explicit "NOT SIZEP2"
claim were backwards. Its only writer is `build_player2_sprite` (storing the `$00`/`$01`/`$03`
width encoding) and its only publishers are `vbi_handler_flight $5028` and `$8D1A`, both to
`$D00A`. The stale `SIZEP2`/`GRAFP2` labels in `rof_native.c` were corrected in the same pass
(`$D014` is COLPM2, `$D00A` is SIZEP2), as was a third mislabel found next to them: `$D008`/`$D009`
are SIZEP0/SIZEP1, not "P2/P3 horizontal pos". `rof_manual.c:61`'s `SIZEP2` label on `$D00A` was
already right.

**Named — 23 previously-unnamed cells** (new dated section at the tail of `symbols.csv`): the
13-cell enemy-fire / bolt line-plot working set (`fire_target_col/row`, `fire_queued`,
`fire_plot_x/y/xfrac/yfrac/wfrac/width/vstep/rowcount/endrow`, `fire_delay_mask`) and the 10 trig
lookup entries (`trig_result_mid/hi`, `trig_quadrant`, `trig_octant_work`,
`trig_sample_angle`/`_angle1`, `trig_quad_sign`/`_reflect`, `trig_table_hi`/`_lo`). Each was
confirmed single-subsystem before naming: the bolt set is touched only by `terrain_sub_A822`
(latches the target), `game_state_update` (the state machine), `plot_scanline_down` (the plotter,
called from nowhere else) and `terrain_gen_2` (clears the flag); the trig cells only by
`sine_table_lookup` / `trig_interp_lookup`.

**Descriptions corrected, name kept** — `$1F0B pmg_colors_station` (writes COLPM2/COLPM3
`$D014`/`$D015` + HPOSP2/3 on a 7-frame cadence, **not** COLPF3), and the 13 multi-role cells per
the convention above: `$0041`, `$0086`, `$0087`, `$0088`, `$0089`, `$008A`, `$008B`, `$008C`,
`$0092`, `$00A4`, `$00A5`, `$28EE`. All nine projection roles were re-derived from
`setup_projection_params $AC93` before being written down — which turned up a further error in the
*existing* text: `$00A4`/`$00A5` are fed by **pitch** (`pitch_shadow_hi/lo $0024`/`$0023`), not by
heading.

**⛔ Two 2026-08-03 proposals DECLINED — do not re-apply:**
- `row_count $0086` → ~~`dl_lms_end_index`~~. It is a **generic row/loop counter shared by ~8
  routines** (`display_list_build`'s 218-row loop, the terrain fns at `$77A8`/`$801D`/`$A41F`/
  `$B1D4-$B515`) that *also* happens to hold the LMS window end index. A `dl_lms_*` name would
  mislead every other caller.
- `dl_src_index $008B` → ~~`dl_lms_start_row`~~. Same reason, ~8 other users (`display_scroll`
  `$1CF0-$1D9A`, `$537C`, `$624E-$62C7`, `$77A4`, `$8077`) — **and the 2026-08-03 entry contradicted
  itself**, listing `$008B` in its own projection group as a document-both-roles case one paragraph
  above proposing the rename. Both cells now document the LMS role in their descriptions instead.

**`$5000 stage_5000`** — nothing to rename: it has **no `symbols.csv` row**, the name comes from
`ghidra_scripts/entrypoints.csv` and correctly describes the *game* routine at `$5000` in the
**final** image. The error was the *attribution* of the boot `INITAD $5000`, which is the Lucasfilm
Games logo (XEX segment 5, overwritten by segment 16 — so not in `listing.txt` and not transpiled).
Corrected in `docs/startup-flow.md` §1/§4 and commented in `entrypoints.csv`. **General trap:
`listing.txt` is the FINAL image, so a boot-phase routine at an address may not be the routine at
that address** — check `tools/xex_map.py` for a later segment covering it first.

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

## Station cinematic (scene 2) ZP roles — recorded 2026-08-13 while making `$1E79`/`$1C40` native

Every one of these cells has a name from a *different* subsystem, and in the attract scene it means
something else entirely. Verified against `disasm/listing.txt` `$1910-$1F2F` (not guessed). None is
worth renaming on its own — like `$0086` they are genuinely polysemous 6502 ZP scratch — but a
reader of the station routines needs the mapping, and a future batch rename must not pick a
station-specific name for any of them.

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

## ⚠ `pitch_*` / `roll_*` ARE swapped — settled 2026-08-14 by a live flight test

**Resolved the same day it was raised.** The user flew the new joystick and reported the axes behave
as an aircraft: **stick back raises the nose, stick forward lowers it.** Stick forward/back is PORTA
bits 0/1 (verified against `$8e74-$8eac`), and those bits write **`$0027`** — so `$0027` is the
**pitch** rate, not the roll rate, and `$0028/$0029` is the **pitch** accumulator. That matches
`amiga/diag_timing.gdb` and **contradicts `symbols.csv`**, i.e. the names below really are swapped.

**One loose end before a batch rename:** if `$0025/$0026` is therefore ROLL, then
`$0023/$0024 pitch_shadow` (its snapshot) is a *roll* shadow — yet this file's own `$00A4`/`$00A5`
note has it feeding the canopy-pillar Y, which reads as a pitch effect. Re-check that one consumer;
the axis question itself is closed.

The original conflicting evidence, kept for the record. The two axes at issue are
`$0021 pitch_velocity` / `$0025-$0026 pitch_pos` versus `$0027 roll_velocity` /
`$0028-$0029 roll_pos`.

`flight_control_integrate` reads the stick like this (`rof_native.c`, steering block):

| PORTA bit | physical stick | writes |
|---|---|---|
| 2 / 3 | left / right | `pitch_velocity $0021` |
| 0 / 1 | up / down (forward / back) | `roll_velocity $0027` |

- **The manual says** forward/back = **dive/climb** (pitch) and left/right = **bank** (roll). Taken
  with the table above, that makes `$0027` the PITCH rate and `$0021` the ROLL rate — i.e. both
  names are **swapped**.
- **`amiga/diag_timing.gdb` agrees with the manual**, and it was written during a real nose-dive
  investigation: its header calls `$0027` "pitch-rate: 00=level at neutral; $D0=nose-dive bug" and it
  prints `pitch(28/29)` / `roll(25/26)` — the opposite assignment to `symbols.csv`.
- **But `$0023`/`$0024 pitch_shadow` (a snapshot of `pitch_pos $0025/$0026`) feeds the canopy-pillar
  Y**, per this file's own earlier `$00A4`/`$00A5` note — and the pillars move vertically with PITCH,
  which argues `$0025/$0026` really is pitch and the names are RIGHT.
- The **bit assignment itself is not in doubt**: PORTA stick-0 bits 0/1/2/3 = up/down/left/right,
  verified against `$8e74-$8eac` (comment in `PlatformAmiga.cpp`).

The tie-breaker was exactly as cheap as it looked — **fly one axis and see** — and it went to the
manual + `diag_timing.gdb` reading (see the resolution at the top of this section). Note the method:
the answer came from a **user flight report**, not from more static reasoning about the disassembly,
which had already produced two confident and mutually contradictory answers.

## `$44D6 update_altitude_digit_display` — writes the LR-Scanner's M2 blob, not a digit display

**Address:** `$44D6` (twin: `update_altitude_digit_display`, `src/gen/rof_native.c`)
**Suggested:** `move_scanner_dot_row` (or `move_m2_dashboard_blob` if the instrument stays in doubt)

Nothing in it draws a digit. It clamps an index to `[0,$1C]∪{$1E}`, and when that index *changed*
since its last call it moves a 3-row blob of **missile-M2 bit pairs** inside the dashboard band of
the missile DMA buffer — clearing 3 cells at the old row (`&$CF`) and setting 3 at the new
(`|$30`) — then derives one colour/glyph byte into `altitude_color_or_glyph`. It is the **only**
`$30`-mask writer in `$0B88-$0BB8` (grep of all three generated files), and that blob is what the
Amiga port mirrors as the Long Range Scanner (#13) guide dot.

**Resolved which way it goes:** the index is derived from `altimeter_alt_ref` (`$1C -
altimeter_alt_ref`), an ALTITUDE-sounding source, whereas the scanner's range-to-pilot lives in
`$0642` — so either the blob was an altimeter pointer the port had mis-mapped to the scanner dot, or
`altimeter_alt_ref` is misnamed. A user flight report settled it: **the dot tracks bearing and range
to the pilot**, so the instrument mapping is right and the suspect name is the SOURCE.

⚠ Therefore also **`$28DA altimeter_alt_ref`** — derivation now DONE, it is confirmed misnamed and
does NOT double as an altitude. Its ONLY reader is `$44D6` (`$1C - $28DA` → the scanner-dot row);
nothing reads it as an altitude, and the real altimeter bar is driven by `terrain_clearance`
(`dispatch_43cb_half_70`), not this. Its value is a NEAREST-scanned-object quantity:
`terrain_plot_object` (`$A683`) stashes each object's `$232E` byte in `$28E4` and, for the closest one
to pass the depth test, copies it to `$28E8` (`rof_native.c:6803/6812`); `terrain_draw_frame` resets
`$28E8=$80` every frame and publishes `$28DA=$28E8` (`8335/8358`); init/death/level-clear reset it to
`$80` (`3727`, `12666`). So the per-frame `$80`↔valid swing (instrumented live this session) IS the
guide-dot blink, and `$28DA` is a pilot/nearest-object RANGE quantity. **Suggested:**
`scanner_target_range` (or `nearest_object_row`). Its sibling **`$28D9 altimeter_color_base`** is the
same story — a snapshot of `$28E7 = $0059` (the object half-width) — so `altitude_color_or_glyph`
(derived from it in `$44D6`) is suspect too. Not yet renamed in `symbols.csv` (batch-rename via the
transpiler, per CLAUDE.md). Same method note as the `$0021`/`$0027` entry below: the original
which-way tie-breaker was flying it, not static reasoning about the disassembly.

Also note the blob is only ever **moved, never cleared**, so the dot cannot blink by its M2 bits
going away (the proximity blink is the `$33DF/$33E0` pen swap). A comment in
`buildScannerDotSprite` claiming otherwise was corrected.

⚠⚠ **`$00CE altitude_color_or_glyph` — CONFIRMED misnamed: it is the guide dot's HPOSM2 (horizontal
position / target BEARING), not a colour or a glyph.** `$44D6` derives it as `$28D9 + $AB`, keeping the
result only when it lands in `[$A3,$B4)` and substituting **`$B5`** otherwise. Measured on the Amiga
mirror, that window maps to hardware sprite X `359..389` — inside the LR-scanner disc's transparent
window — while the `$B5` substitute maps to X `395`, i.e. a PARK position outside it. Because the
dashboard copper sets `PFxP=0` (all sprites BEHIND the playfield), a parked dot is not merely
mispositioned but completely **hidden** behind the opaque cockpit bitmap. So `$B5` is how the original
hides the dot horizontally, and the name should say position, not colour. **Suggested:**
`scanner_dot_hpos` (and `$28D9` → `scanner_target_bearing`, superseding the "suspect" note above).
Not yet renamed in `symbols.csv` (batch-rename via the transpiler, per CLAUDE.md).

Consequence for the port, recorded because it cost several wrong fixes: the dot has TWO aliased inputs,
not one. `$28DA` (range → row) and `$28D9` (bearing → X) are BOTH republished per terrain pass, and
`$44D6` samples both in the 50 Hz VBI while the flight loop free-runs — so on a fast CPU either can
alias to its off-disc/parked sentinel. Fixing only the row leaves the dot drawn, correctly coloured,
and invisible. Both coordinates must be pushed together from the display pass's publish point.
