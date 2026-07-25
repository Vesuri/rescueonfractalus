# Alien "Jaggi" jump-scare — trace map (2026-07-25, IN PROGRESS / creature draw still unlocated)

The classic *Rescue on Fractalus!* jump-scare: a rescued figure runs to the ship, and at the
"knock" an alien jumps onto the canopy instead of a pilot boarding. This is the port target for
the flight-scene "alien-on-windshield" open item. Status: **trigger + gate mapped; the creature
DRAW routine is not yet located.**

## Trigger (who is an alien)

- **The pilot/alien fork is the map marker `$0A00[$28E6]`.** `pilot_render $7854` (native twin,
  rof_native.c): at landing phase (`$003D >= 3`) it sets `$281E = 1`, then reads the marker —
  `== $80` keeps `$281E = 1` (alien-designated), anything else DECs `$281E` to 0 (pilot).
  Validated: `a800dumps/rescue_pilot.a8s` (level 4) has marker `$C8` and is a real pilot.
- **Alien designation happens at SPAWN** via `$4E58` (scans `$0A00` slots with marker `$01`; those
  not in the `$2276` table become `$80`). The level-derived threshold `$061B`
  (`compute_stage_display_geometry $75F5`, from `level_stage $006D`) makes aliens impossible on
  levels 1–10 and increasingly likely after (see [[flight-scene]] for the table).
- ⚠ `pmg_enemy_update $7AB8` is MISNAMED — it's the per-frame **alien-attack tick** (ship-shake via
  `jitter_roll_pitch $AA95`, pounding SFX, phase toggle `$283D`); **no PMG**. See docs/rename.md.

## The knock, and the gate (why FORCE_ALIEN shows no visual)

At the knock (`pilot_render` phase 4, `L_79a8`, native rof_native.c ~line 3098):
- **`$281E != 0` (alien) and `$003C == 0` (airlock closed):** calls **`game_sub_7EC7`** every frame,
  which — because systems are off (`$003E != 0`) — enters a **blocking sound loop** driving
  `game_sub_7F85` (an SFX sequencer over tables `$81E2/$81E8/$820A`) once per RTCLOK frame until
  systems come back on. Pure SFX → **this is the "attack sounds", NO creature drawn.**
- **`$281E == 0` (pilot) and `$003C == 0`:** calls `animate_clear_colors_timed $7A17` (RTCLOK-gated
  colour sweep) — the pilot "waiting/knocking" animation.
- **Either fork reaches the REVEAL only when `$003C != 0`** (`$79D9 → $79E1`): the reveal sets
  `$0633 alien_trigger` (alien) or scores (pilot), sets the airlock light `$3388`, `INC $003D`.
- **`$003C` is set ONLY by the airlock command** (`event_sequence_dispatcher $4644`, event index 1
  at `$46EA` → `$471E INC $003C`), i.e. the **A key**. No auto-trigger exists in the dispatcher.

**Conclusion:** the jump-scare reveal (and the `$0633`-gated ship-shake) require the **airlock to be
opened**. `FORCE_ALIEN` (forces only the marker → `$281E`) without opening the airlock reaches only
the `game_sub_7EC7` sound loop → matches the observed "attack sounds + explosion, no shake, no
creature". `FORCE_ALIEN` + opening the airlock at the knock SHOULD reach the reveal (untested).

## ★ RESOLVED 2026-07-25 — the creature draw is `game_sub_7F85` → `$80C5` (airlock-CLOSED path)

**Two mistakes in the earlier hunt** (both now corrected):
1. **Wrong path.** The classic jump-scare (alien on the glass) is the airlock-**CLOSED** case, not
   the reveal. At the phase-4 knock (`pilot_render` L_79b2, `$281E!=0` alien): **`$003C==0` (airlock
   closed)** → `game_sub_7EC7`, which is NOT "sound only" — it seeds a creature-animation state
   (`$0632=1`, `$005E/$005F`, `$2921/$2924/$2926`, `$2930/$2931` position, `$0635/$0638/$0639`) then
   loops calling `game_sub_7F85` every frame while systems stay off. **`$003C!=0` (airlock OPEN)** →
   the reveal (`$0633` set) = the alien **BOARDS** = "ALIEN IN SHIP" + shake + energy drain, and draws
   **no** creature (verified: `g_alPlotCalls=0`, `g_alDrawShape=0` over 158 `$0633` frames). So opening
   the airlock is the *boarding* mechanic; the *scare* is keeping it closed.
2. **Wrong routine + wrong probe gate.** The creature is NOT drawn by `plot_clipped_pixel` or
   `draw_scaled_shape` (the two the `g_al*` probe hooked), and NOT while `$0633` is set (the gate the
   probe used). It is drawn by **`game_sub_7F85` ($7F85)** — misnamed "sfx_seq_step"; it animates the
   frame tables (`$81E2/$81E8/$820A`) *and* draws — via **`$80C5`**: a masked bitmap blit
   `LDA $BE00,X / AND ($8B),Y / ORA shapeByte / STA ($8D),Y`, 17 bytes wide, row stride `$60` (=96,
   the mode-D row), into the `+$30` display half, positioned from runtime row-address tables
   **`$073D`/`$0793`** (built at `$7464/$7469`) indexed by `$2930`/`$2931`. Shape geometry comes from
   tables `$81A1/$81A9/$81B1/$81B9` + `$81C1/$81C9/$81D1/$81D9` (loaded into ZP `$80-$8A`).

**Why it's invisible on the Amiga (same class as the fixed ground-objects bug):** `game_sub_7F85` is
transpiled (`rof_gen.c:13267`) and DOES run on the Amiga (it's the knock loop that already produces
the SFX). Its `$80C5` writes land in the mode-D field, but `renderFlightDirect` renders the terrain
body from `$260E` + `g_flightDotPlane` and only reads the field for the band rows (43-46) → the
creature's field-body writes are dropped. Exactly the ground-object case (fixed by hooking
`terrain_plot_pixel` to also OR into `g_flightDotPlane`) — but `$80C5` is a *different, un-hooked*
plot routine, and its shape is a **masked multi-value blit** (not single dots), so the port needs to
mirror its field writes into the Amiga flight plane(s) with the value→pen mapping.

**NEXT (fix): (1)** re-gate the probe to the closed path (`$0632!=0` or `$281E!=0 && $003C==0 &&
$003E!=0`) and hook `game_sub_7F85`/`$80C5` to CONFIRM on the Amiga it draws into the field + capture
the row/col extent; **(2)** mirror `$80C5`'s masked writes into the Amiga flight planes (analogous to
`ROF_PLOT_DOT`/`g_flightDotPlane`, mapping the mode-D field values it writes → planes/pens). ⚠ Test
with **`FORCE_ALIEN=1` and airlock CLOSED** (do NOT set `FORCE_AIRLOCK` — that takes the boarding
path). Reaching + HOLDING the knock is the trigger. Probe infra: `g_al*` in rof_native.c.

## Rename candidates (add to symbols.csv later)
- `$7EC7 game_sub_7EC7` → `alien_knock_setup_loop` (seeds the jump-scare creature-animation state
  `$0632/$005E/$005F/$2921/$2924/$2930/$2931/$0635/$0638/$0639`, then loops `game_sub_7F85` per
  frame while systems-off — the airlock-CLOSED knock; NOT "sfx only").
- `$7F85 game_sub_7F85` → **`alien_creature_animate_draw`** (⚠ NOT an SFX sequencer — it steps the
  frame tables `$81E2/$81E8/$820A` AND draws the creature via `$80C5`).
- `$80C5 (FUN_80c5)` → `alien_shape_blit` (masked 17-byte-wide bitmap blit into the mode-D viewport
  field: `$BE00` mask table, `($8D),Y` write, row stride `$60`, row addrs `$073D`/`$0793`).
- `$003C anim_flag_003C` → `airlock_state` (0=closed → jump-scare; INC by airlock cmd → boarding;
  promoted `$80`/`$FF` in the reveal). `$281E` (unnamed) → `figure_is_alien` (1 when marker `$80`).
- `$0632` → `alien_knock_active` (set 1 by `$7EC7`, cleared at `$7F76`; gates VBI work at `$5250`).
