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

## THE OPEN UNKNOWN — where is the creature drawn?

After the reveal (`$0633` set, `$003D` advanced), each frame runs the attack tick (`$7AB8`:
shake/SFX/`$283D` + shape selectors `$0044=$70`, `$0047=$D8`). **No creature-draw call has been
found** in: `pilot_render` (reveal only sets flags), `game_sub_7EC7`/`game_sub_7F85` (sound), the
dispatcher (input/messages), or the attack tick (shake/SFX). `$0047=$D8` is a COLOUR (not a shape);
the zoom (`animate_zoom_sequence`) uses phase-fixed shapes `$7DEF/$7E25/$7E5B/$7E91` and doesn't run
post-reveal. `$0633` readers: `enemy_check $3FCD` (→ attack tick), airlock-msg `$46EE`, cleanup
`$6038`. **Next step to find the draw: capture a REAL (or airlock-triggered) alien and see what
display element carries the creature (bitmap field region vs PMG) — static analysis has not located
it.** Probe infra: `g_al*` in rof_native.c (Amiga+PROBES), read via `amiga/diag_alien.gdb`.

## Rename candidates (add to symbols.csv later)
- `$7EC7 game_sub_7EC7` → `rescue_knock_sfx` (sets up + loops the knock/attack SFX; blocks until
  systems-on during systems-off rescue).
- `$7F85 game_sub_7F85` → `sfx_seq_step` (table-driven SFX sequencer, tables `$81E2/$81E8/$820A`).
- `$003C anim_flag_003C` → `airlock_state` (0=closed; INC by airlock cmd; promoted `$80`/`$FF` in
  the reveal). `$281E` (unnamed) → `figure_is_alien` (1 when marker `$80`).
