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

## ★★ RENDERS 2026-07-25 (session 2) — creature shows + animates + lights blink (user-confirmed). PERF is the open blocker.

**What now works (committed 9e8a83c → e36b6a1):**
- **Mirror:** `hud_build_text_row` ($80C5, the native twin the game actually runs — NOT the transpiled
  oracle) mirrors each creature byte, gated on `$0632` (alien_knock_active), into the paused-rescue
  overlay **`g_figP1/P2/M`** via `ROF_PLOT_ALIEN` (rof_native.c). Geometry MEASURED + matches the figure
  overlay: field base `$1010`, stride 96; `rel = A-$10A4`, `r = rel/96`, `b = rel%96`; byte decoded via
  `kModeDP1/kModeDP2` (now `extern "C"`). `game_sub_7EC7` clears the overlay each step (`ROF_CLEAR_FIG`).
- **Render path fix (THE key unlock):** the knock is a *blocking* loop in `game_sub_7EC7` (main loop
  never runs). The old faithful pre-wait spinwait `while(RTCLOK<=4){render}` NEVER rendered on the Amiga
  (`g_alRF=0`, frozen screen) because RTCLOK is advanced by the **hardware flight VBI**, not renderFrame,
  so after the slow `$7F85` draw RTCLOK was already >4 → wait was a no-op. Restructured to faithful
  **wait-then-draw with ONE composite per SFX step**: busy-wait the 5-frame interval (copper displays the
  last-composited creature continuously = the ANTIC stand-in, NO re-render), then `ROF_CLEAR_FIG` + draw
  + `platform_render_frame()` once. renderFrame → `renderFlightDirect`'s **rescueFigure branch** (active
  at systems-off phase≥3) composites the overlay over the frozen terrain. (`g_alComp=28/28`, `g_alRF=28`,
  VVBLKI=`$4FF5`, rsFlight=1 — all confirmed.)
- **Airlock-OPEN path** = the *boarding* mechanic ("ALIEN IN SHIP" + shake + energy drain, `$0633` set),
  draws NO creature. The jump-scare is the airlock-CLOSED knock. (`FORCE_AIRLOCK` = boarding test only.)

**★ OPEN #1 — PERF (the blocker, ~7.5× too slow).** Measured per knock STEP (beam ticks, 313=1 frame=20ms;
faithful step = 5 frames = 1565): **draw `game_sub_7F85` ≈ 8438 ticks (~540ms), render ≈ 3284 (~210ms),
wait ≈ 0** (no pacing left — draw+render already blow past 5 frames).

**★ MEASURED 2026-07-25 (session 3) — the hud-vs-wrapper split: `hud_build_text_row` is ~100% of
`game_sub_7F85`; the wrapper is ~0%.** (`g_alTHud` ≈ `g_alTDraw` per step, `hud/draw`=111% within async-VBI
noise; 40 hud calls/step.) So native-twinning the `$7F85` *wrapper* does NOT help — the lever is the row
composer + cutting per-row work.

**★ DONE 2026-07-25 (session 3, commit fa1d06b) — clean-C rewrite of the whole draw tree** (foundation for
the next perf pass; all byte-identical `make validate`, both backends build):
- `pack_byte_to_5bit_cells` → a fixed bit-permutation (no more 22-op ROL/ROR chain; **dropped ~12 `$0084`
  bus ops/call**, ~480 pack calls/step) + a `_core` so callers keep the accumulator in a register.
- `hud_fill_field0/1/2/3` → hoisted the ZP source-pointer reconstruction out of the per-byte loop.
- `hud_build_text_row` → hoisted the mask/dest row-pointer bases out of the 17-cell inner loop.
- `game_sub_7F85` → now a validated native twin (clean sequencer + setup + blit loop), no longer transpiled.

**★ NEXT (perf, now on a clean base):** (1) re-measure the per-step draw to quantify the win; (2) the
remaining cost is the faithful 17-cell blit × ~40 rows + ~480 pack calls — escalate the row composer to the
same levers as the terrain path (hand-asm the inner blit / pack, or CUT rows: on the Amiga only the
`ROF_PLOT_ALIEN` mirror output is observable, so rows whose output falls entirely outside the figure region
do dead work — but the cursor/pointer state threads through all rows, so skipping needs care). (3)
`ROF_PLOT_ALIEN` still does 2 divisions/byte (rel/96, rel%96) — cache the row (17 consecutive bytes share a
row) to drop ~1 div/byte. And the **render 210ms** is the normal slow-flight-frame cost (flight is ~5-6 FPS);
even a perfect draw leaves ~2× from the render — may need a lighter composite path for the paused knock
(skip the full renderFrame per-frame work). bus_read/bus_write are inline+cheap for RAM (ruled out).

**OPEN #2 — COLOUR.** The creature renders in the viewport pens 0-3 (terrain palette), so likely the WRONG
hue. The attack colour is `$0047` (`$6D/$70/$D8` cycle, set by `pmg_enemy_update $7AB8`). Pens during the
knock (measured): `$DA=10 $DB=b8 $DC=14 $DD=2a`. Wire `$0047` → the copper viewport pens during the knock
so the alien shows in its proper colour. Do AFTER perf (shape is confirmed; colour is polish).

**OPEN #3 — >1 MINUTE to REACH the alien.** Separate from the knock: the rescue APPROACH (colour sweeps
`animate_clear_colors_timed`/`clear_colors_sweep_5x` + the figure zoom `animate_zoom_sequence`) + general
slow flight rendering. NOT fixed by the `$7F85` twin. Its own follow-up.

**Probe infra (all committed, Amiga+PROBES):** `g_al*` in rof_native.c (creature capture, pen/timing,
composite counters), read via **`amiga/diag_alien.gdb`**. Build `make PROBES=1 FORCE_ALIEN=1`; test with
airlock CLOSED (do NOT set FORCE_AIRLOCK). `g_alTDraw/g_alTRender/g_alKnockFrames` = the per-step timing.

**(Superseded) original NEXT plan — now done:** re-gate the probe to `$0632` + hook `$80C5` + mirror the
masked writes into the flight planes → all implemented; creature renders. Remaining = perf + colour above.

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
