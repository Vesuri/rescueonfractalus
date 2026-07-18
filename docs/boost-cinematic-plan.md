# BOOSTERS "return to mother ship" reverse cinematic — port plan

## 0. STATUS 2026-07-19 — START HERE (boost render DONE incl. T6 handoff; one perf item remains)

The whole boost cinematic (ascent → stars → reverse tunnel → standby) renders and is committed+pushed.
Render bugs fixed through 2026-07-17 (commits 792e6d0, c347749, 6abc6cd, 53f4d86, 45f02c7, f824503):
- forward stars-behind-planet sprite priority (PlanetCopperList BPLCON2 PFxP=1);
- boost stars black-on-salmon + bowtie (defer the copper install until the star pens $08D4-9 seed);
- tunnel reveal shape: SYMMETRIC centre band [K, 85-K] decoded linearly, growing from the centre.

**2026-07-18 session — DONE + user-verified (all pushed):**
- **T4 native twin** `step_accum_sub_7e $6A8F` + the Amiga `g_tun*`/`g_tunnelFieldDirty` dirty-band
  publish (b7ea184; `make validate FN=step_accum_sub_7e` = 4000 cases, 0 mem mismatch).
- **Boost tunnel COLOURS re-derived from the 6502 DLIs (ground truth, not screenshots)** (31d9bd8):
  the copper mirrors dli_sub_6cf1/6cd7 (GTIA mode-10) — see the colour model in the durable lessons
  below. Fixes the teal edges / wrong clear. A **boost-only** GTIA→pen LUT (`kGtia10BoostP`) maps the
  outermost ring (value-2) → color00 and the background (value-8) → color02, so the windscreen-band
  corner triangle (mode-D value-0 → color00) is the SAME register as the outermost ring. Latch
  `boostRingRevealed` ($008D<0) keeps the triangle black until the outer rows are drawn, then follows
  the ring. FORWARD tunnel keeps its own `kGtia10P` LUT (value-8→color00), untouched — the paths
  branch on `rsBoostViewport`.
- **Band-triangle teal stripe** (8481ec0): the late band color00 flip landed ~16px into the band's
  first line (the cockpit bitplane-pointer moves overrun (kCockpitLine-1,0xE0)). Fixed with an EARLY
  band-top color00 copper slot (`INDEX_BANDTOP_COL00`, before the bitplane moves) + `disableBandReveal()`
  no-op of the late slot. See the copper lesson below.
- **setjmp host-build fix** (8f0b0ec): Apple clang 21 (arm64) dropped `__builtin_setjmp`, breaking every
  host rebuild of `rof_native.c` (game_main_loop's restart trampoline) — both `make validate` and the
  SDL `make`. Host-only ISO `<setjmp.h>` shim; Amiga path unchanged.

**2026-07-19 session — T6 reverse-tunnel→standby handoff DONE + user-verified (commits d8d7c18, 89a57f1):**
- **T6 handoff hold (3 bugs, d8d7c18)** — when the reverse ring ends ($008D clears) rsBoostViewport goes
  false but display_setup spends ~13 frames finishing the next-level door field before latching
  g_doorFieldReady; in that window render() fell through to the forward doors/tunnel path.
  (1) HOLD the last reverse-ring frame while `rsBoostReturn && !g_doorFieldReady` — staticStandby takes
  over on the g_doorFieldReady 0→1 edge (kills the black-top/green-doors flash; measured exactly 13 hold
  frames). (2) Gate the forward-tunnel decode on `rsBoostReturn` not `rsBoostViewport` — the still-ticking
  reverse-ring VBI kept setting g_tunnelFieldDirty, re-decoding $1000 rings into the held tunnelBitmap
  (rings reappeared over the dark-green field). (3) The Standby door decoder now stamps
  `viewportLastBase=$2000` so the next renderViewportModeD($1000) full-clears — else a re-launch's planet
  re-entered with the same $1000 base + viewportForceFull consumed, leaving stale LEVEL-NN text / band
  remnants under the starfield. ⚠ the earlier `viewportForceFull=true` on the rsStars edge was WRONG — it
  also caught a brief rsStars phase DURING the boost and cleared viewportBitmap mid-cinematic.
- **Row-by-row band-triangle recede (89a57f1)** — the band corner triangle now transitions teal→dark-green
  one row at a time (mirror of the forward doors→tunnel green→purple reveal), instead of snapping. The
  recede runs inside the T6 hold: display_setup fills the wedge buffer $0C88-$0C8F with $FF top-down (one
  row/frame); mirror it with band-top = dark-green ($0071=$C0) + `setBandReveal(k, teal=$08D8)` where k =
  first still-empty wedge row (rows 0..k-1 green, k..7 teal, k growing 0→8). Measured on the Amiga.

**OPEN / next-session items:**
1. ✅ **VERIFIED (user-confirmed 2026-07-18)** — the reveal (f824f82) grows cleanly from the centre
   in real play, and the pink-vs-teal tunnel ring cycle looks right.
2. **Reverse-ring PERF decode-consume** (was T4): the twin + publish are DONE; REMAINING is to wire
   `decodeBoostViewport` to CONSUME the `g_tun*` publish (cheap incremental `decodeTunnelBand`) instead
   of the full 86-row decode. See §5. **This is the only remaining boost item.**
3. ✅ **T6 — standby handoff DONE (2026-07-19, above).**

**Durable lessons from this cycle (don't relearn):**
- **Copper colour latency:** a copper's colour MOVEs must already hold the right values BEFORE cop1lc
  points at it — a colour write the same frame the bitmap is decoded shows the OLD colour that frame
  (writes lag the bitmap by one frame). If a palette source is seeded a frame late by the faithful game
  code (e.g. $08D4-9), DEFER installing/showing content until it's valid. (Same 1-frame rule as bitplane
  pointer swaps — CLAUDE.md.)
- **The boost tunnel is the SAME animating $1000 field as the forward tunnel** — decode it linearly
  (row r → $1000+r*46, as decodeTunnelRect). Do NOT decode from the $3000 DL LMS as an address (its
  rev-strand addresses are mirrored/misform it); the DL is only a per-row REVEAL FLAG.
- **The reveal is a SYMMETRIC centre-out row band**, not the field growing and not a linear slice. A
  lopsided/linear slice of a nested-rectangle tunnel reads as a bowtie/staircase. Derive the band from
  the FWD strand only (rows 0-42); the Amiga rev strand converts 57-85 (not 43-, unlike the Atari) so it
  can't be used directly. Measured Atari: rv_8.6 (22 rows in) = rows 32-53.
- The probe infra (transpile.py $3C75 VCOUNT hook, PlatformAmiga.cpp FORCE_RETURN, amiga/Makefile
  FORCE_RETURN, the regenerated rof_gen.c) is intentionally UNCOMMITTED (working tree). `make gen`
  preserves the rof_gen.c hook. Build headless probes with `make clean && make -j4 PROBES=1 FORCE_RETURN=1`.
- **DERIVE COLOURS FROM THE 6502 DLIs, NOT SCREENSHOTS** (user directive 2026-07-18). The faithful
  boost tunnel colour model is just the launch DLI chain mirrored: GTIA mode-10 (PRIOR=$94), value→reg
  = **0→COLPM0 $02C0 · 1/2/3→COLPM1/2/3 $08D7/$08D8/$08D9 · 4/5/6→COLPF0/1/2 $08D4/$08D5/$08D6 ·
  8→COLBK $0071** (dli_sub_6cf1/6cd7 in rof_manual.c). The live $1000 field is value-2-dominant
  (value-2 = the outermost ring/surround = COLPM2 = $08D8; there is NO value-8 in the tunnel body — so
  color00 is free there). Mirror the regs live from mem[] each frame; the ring cycle (advance_history
  rotating $08D4-9) then just works.
- **Copper colour change at a region boundary must be emitted BEFORE that region's bitplane-pointer
  moves.** The 6 cockpit BPLxPT moves after the (kCockpitLine-1,0xE0) WAIT overrun ~16px into the
  band's first line, so a color00 flip placed AFTER them lands 16px late (a stripe). Fix = a dedicated
  early copper slot right after the region WAIT, before the pointers (INDEX_BANDTOP_COL00). See the
  amiga-copper-lessons memory. To make a shared copper MOVE a no-op, retarget it to color31 ($1be).
- **Faithful twins that only need an Amiga tweak stay in rof_native.c under #ifdef ROF_PLATFORM_AMIGA**
  (step_accum_sub_7e is the T4 example: pure mem[] twin + the g_tun* publish guarded). Not the .cpp.

--------------------------------------------------------------------------------

Status 2026-07-13: the phase ROUTING is committed (e9043d4) but the RENDER is a broken mess.
User verdict: "not nearly good enough." Root theme (user's diagnosis, now confirmed by a 3-agent
code trace): **the transpiled/native 6502 LOGIC is already running correctly — every colour ramp,
star fade, and ring draw fires. Only the Atari→Amiga CONVERSION is wrong** (wrong copper, wrong
bitplane decode source/stride, dropped sprites, un-wired fade register). Do NOT rewrite the game
logic; fix the conversion layer. A dedicated **BoostCopperList** is sanctioned if cleaner than
forcing TunnelCopperList/PlanetCopperList (user approved 2026-07-13).

Reference frames: Amiga (broken) `~/Pictures/Screenshots/rof_boost_frames_fix/` (60fps, 2032 frames);
Atari (truth) `~/Pictures/Screenshots/rof_boost_frames_atari/`. Recording of the broken run:
`Screen Recording 2026-07-13 at 19.45.54.mov`.

--------------------------------------------------------------------------------
## 1. THE MECHANISM (what actually runs — all correct on the Amiga already)

The ENTIRE cinematic body lives in **`display_setup $5F1D`** (NATIVE twin `rof_native.c:8541`,
oracle `rof_gen.c:6400`), re-entered from `game_main_loop` after the ascent. It drives real Amiga
frames through the SPINWAIT/`ds_frame` hooks, so its colour pokes DO execute per frame. The reverse
tunnel rings are animated by the standby VBI `vbi_handler_standby $52D7` → `launch_anim_dispatch
$5367` → `step_accum_sub_7e $6A8F`, which `display_setup` syncs to by spinning on counter `$008E`.

Sequence (all under VBI `$52D7` + DLI chain `VDSLST=$6CAD`, `$0200==$AD`, gate `$003A==$FF`):

1. **Ascent** (flight VBI `$4FF5`): `game_main_loop` dock branch `$3F50` (`player_lives $0072==2`),
   ascend while depth `$0034` `$40`→`$60`, spin on `level_ready_flag $283B` bit7, seed
   `display_flags $0071 = $2A` (salmon), zero COLPM shadows, JMP `display_setup`. ✅ WORKS on Amiga
   (FlightCopperList).
2. **Salmon→black viewport fade** (`display_setup` ~`rof_native.c:8617`): ramp **`$0071` (COLBK)**
   from `$2A` salmon → `$1F` → `$00`, 2 frames/step. Then ramp the ring `$08D4-$08D9` UP `idx..$0E`.
3. **Stars fade one-by-one** (~`rof_native.c:8654`): for each of the 6 ring slots `$08D4..$08D9`,
   ramp its luminance `$0E`→`$00` (1 frame/step) — pens go black one at a time. Then fade the blue
   space IN on `$08D9 = $90..$9A`, then re-seed the 6-slot star palette from ROM `$6E68`.
4. **Reverse tunnel** (~`rof_native.c:8676`): `step_mode_flag $008D = 1` (then `$FF`), emit DL/LMS
   coord pairs one ring row at a time, spinning on `$008E` (bumped by `step_accum_sub_7e`). While
   `$008D` is NEGATIVE, `advance_history_6a4d` copies `$08D8 → $0071` every rotation (COLBK tracks
   the decaying ring = the continuous fade under the rings). Ends when `$008D` clears.
5. **Standby (next level)**: green "LEVEL NN" door field ($2000), then the standby fade-in.

Colour-register map (from the `$6CAD` DLI chain):
- **`$0071` (display_flags) → COLBK** (whole viewport bg) via DLI `$6CF1`. ← salmon→black fade reg.
- **`$08D4/5/6` → COLPF0/1/2** and **`$08D7/8/9` → COLPM1/2/3** via DLIs `$6CF1`/`$6CD7`.
- The 6-byte ring `$08D4-$08D9` (`color_ring`) = all star/playfield/ring pens.

The viewport bitmap FIELD: base **`$1000`**, GTIA-10 / mode-F, 2bpp-packed (4px/byte). Stride
**48 (`$30`) during the STARS sub-phase**, **46 (`$2E`) during the RINGS sub-phase** — the row-base
tables `$073D/$0793` (which BOTH the plotters and the DL LMS use, via `emit_dl_coord_pairs`) are
re-seeded per sub-phase by `build_line_addr_table_1000_stride $65D2`. ⚠ VERIFY the 48-vs-46 stars
stride against a boost-stars savestate before coding (see Open Questions).

--------------------------------------------------------------------------------
## 1b. T0 GROUND-TRUTH RESULTS (2026-07-13, `/atari-dl-analyzer` on measured savestates)

Savestates captured by free-running `a800dumps/boost.a8s` (= "just after B", whole
cinematic accessible): **`a800dumps/boost_stars.a8s`** (T≈6s, `$008D=00`) and
**`a800dumps/boost_tunnel.a8s`** (T≈11s, `$008D=01`, `$008E=41`). Both `$0200=AD`.
Phase timeline from `boost.a8s`: ascent `$0200=EE` T1–4 (depth `$0034` $2E→$65) → stars
`$0200=AD $008D=00` T5–8 → tunnel `$008D=01` T9–15 → standby `$008E=$80`.

**⇒ Boost stars and tunnel are STRUCTURALLY IDENTICAL — one copper + one decode path
handles both; only the `mem[]` pen VALUES differ per sub-phase (read live by the copper).**

Both sub-phases (measured, `dl_report.py`):
- DL = the launch-cockpit DL `$3000` (mode-6 top bar `$32B5` · mode-4 · mode-D · **mode-F
  viewport y50–127** · mode-D band `$350D` y128–135 · mode-4 dashboard `$332D` y136–215).
- Viewport LMS = base **`$1000`**, **stride `$2E` = 46** (`$1000,$102E,$105C,…`), GTIA-10
  (PRIOR=`$94`). **⚠ Both stars AND tunnel are stride 46 — the plan's "stars=48" was WRONG.**
- Value→register map (identical both phases; GTIA-10 nibble): **value-8 = background →
  COLBK ← `$0071`**; COLPF0/1/2 ← `$08D4/5/6`; COLPM1/2/3 ← `$08D7/8/9`; **COLPF3 = `$06`
  (constant grey) = pillar/frame grey** (5th-player missiles).
- Band (mode-D y128–135): COLPF0=`$04`, COLPF1=`$06` (greys), COLPF2=`$2C` (orange),
  **COLBK ← `$0071`** (so band corners fade with the body — the launch band, NOT `$6D67`).
- Stars pens (T6): `$08D4-9` blue-ish (`92 90 9A 98 96 94`, fading). Tunnel pens (T11):
  cycling teal. Only difference between the two phases.

**Open Questions — all RESOLVED:**
1. **Stride/encoding:** 46 for BOTH (not 48/46), GTIA-10. `decodeTunnelRect` already
   hardcodes stride 46 → plan §2 "stride mismatch" (bug #2) is a **NON-ISSUE**.
2. **Pillar grey:** **COLPF3 = `$06`** (fixed), NOT `$02C8`. (Bug #3 confirmed + source.)
3. **Band corners:** track **`$0071`** (COLBK) → corners fade with the body. Confirmed.
4. **Reverse tunnel reuses `$1000`:** YES — same base+stride → stars & rings share ONE bitmap.

**Amiga decode detail (RescueOnFractalus.cpp:1148):** GTIA value-0 is remapped to pen7
(exit-clear black), value-8 → pen0 = **color00**. So the starfield/viewport background is
**color00**, and `updateTunnelCopper` currently pins `color00 = mem[$08D8]` (line 2309/2320
— the FORWARD-tunnel corner-reveal trick). **The real bug #1 fix = wire color00 ← `mem[$0071]`.**
Since `advance_history_6a4d` copies `$08D8→$0071` every tunnel rotation, `color00←$0071`
renders correctly for BOTH stars (independent fade) and tunnel (tracks the ring). ← the crux.

--------------------------------------------------------------------------------
## 1c. ⚠ THE ACTUAL ROOT CAUSE (2026-07-14 — overturns §1/§2's "$1000 throughout")

The palette fixes in §1b were treating a symptom that isn't reachable.  Measured by dumping
BOTH the source fields and the decoded bitmap on the Amiga + Atari (recon PNGs + histograms):

**The boost viewport source buffer SWITCHES between `$2000` (stars) and `$1000` (tunnel).**
The `$3000` launch DL's mode-F LMS is rewritten mid-cinematic (static DL walk of the frozen
savestates):
- **STARS (`$008D==0`):** `$3000` DL mode-F LMS = **`$2000`..`$2F46`**, stride 46 → displays the
  **`$2000` field = the starfield** (value-8 bg 99.5% + ~33 sparse dots; renders black + dots,
  = Atari frame 260).  `$1000` is empty / being built here.
- **TUNNEL (`$008D!=0`):** `emit_dl_coord_pairs` has rewritten the DL LMS to **`$1000`**..`$1F18`
  (the concentric ring field).

The committed boost branch ALWAYS decoded `$1000`, so during stars it showed the empty/half-built
ring field (a white **bowtie** in false-colour) instead of the starfield — exactly the user's
"bowtie when it should be stars".  **`$1000` in stars is NOT a starfield; the starfield is in
`$2000`.**  The Amiga DOES populate `$2000` correctly (verified: value-8=6849 + ~31 dots, matches
Atari) — it was just never decoded.

**FIX (done):** `tunnelSrcBase` member; the boost branch sets it `= (mem[$008D]==0) ? $2000 :
$1000` before `decodeTunnelRect`; forward-tunnel path resets it to `$1000`.  Verified headlessly:
`tunnelSrcBase=2000` during stars, decoded bitmap = pen0 99.5% + ~124 sparse dot pixels = the
starfield.  (The `$3000` DL is NOT maintained in Amiga `mem[]` — it renders via copper — so the
switch keys on the faithful game flag `$008D`, not the DL LMS bytes.)

OPEN: the stars→tunnel transition on the Atari is a row-by-row DL-LMS rewrite (rows flip `$2000`→
`$1000` as `$008E` counts); the Amiga switch is whole-buffer on `$008D`, so the transition may snap
rather than sweep.  Acceptable for now; revisit if the handoff looks abrupt.

--------------------------------------------------------------------------------
## 2. ROOT-CAUSE MAP (broken Amiga frame → conversion bug)

| Amiga frame | Symptom | Conversion bug |
|---|---|---|
| 438 | canopy PILLARS vanish on copper switch | TunnelCopper wires posts but colours them from `mem[$02C8]` (title bg), which is BLACK during boost → invisible. Wrong colour source (should be the pillar grey). |
| 438 | black glitch at band-left edge | palette poked a frame too late at the copper switch. |
| 445-452 | viewport snaps BLACK; only band-corner triangles salmon; no stars | **viewport COLBK not wired to `$0071`.** TunnelCopper's viewport `color00` is pinned to the ring corner `$08D8`, not the fade register `$0071`. The whole-viewport salmon→black ramp is invisible. |
| 452-868 | stars empty/black; runs but wrong content; VERY slow | (a) decode uses `decodeTunnelRect` stride **46** but stars field is stride **48** → misaligned/empty. (b) full-field decode every frame (no dirty band) = far over budget. |
| 877-1064 | teal palette CORRECT but tunnel geometry distorted (bowtie) until complete | palette conversion works (updateTunnelCopper tracks `$08D4-9`); geometry distortion = decode reads a partial/incrementally-built field each frame with wrong stride, and full-decode shows inconsistent intermediate states. |
| 1064 | pillars overdrawn at tunnel completion | pillar sprite priority / band overlap. |
| 1288-1425 | broken partial dark-green door field (1399) then snaps correct (1425) | standby handoff conversion: the `$2000` door field + the band-triangle row-by-row clear (`clear_slot_0c87_0d87 $6A27`, transpiled) not reflected; copper switch timing. Atari target = uniform green + pillars (A638). |

--------------------------------------------------------------------------------
## 3. PROPOSED ARCHITECTURE — a dedicated `BoostCopperList`

The boost viewport needs, under ONE fixed `$6CAD`-style copper: (a) viewport **COLBK that tracks
`mem[$0071]`** every frame (the salmon→black fade + the reverse-ring COLBK), (b) playfield/ring pens
tracking `mem[$08D4-$08D9]`, (c) canopy PILLARS (grey), (d) the windscreen band + corner triangles,
(e) a line-doubled mode-D/GTIA-10 viewport bitmap decoded from `$1000`. This is a hybrid of
PlanetCopperList (viewport bitmap + single-register COLBK fade lever `setPlanetBgColor`) and
TunnelCopperList (ring-pen cycle). Rather than overload either, build a **BoostCopperList** modelled
on PlanetCopperList's layout with:
- `setViewportBg(colBk)` ← `atariToOCS(mem[0x0071])` each frame (the fade; single clean lever).
- `setViewportPens(p0..p2)` ← `atariToOCS(mem[0x08D4/5/6])` (playfield pens; also the ring cycle
  during the tunnel — same registers).
- `setPostColor(grey)` ← the correct pillar grey (NOT `$02C8`; find the flight/launch pillar-grey
  source — likely `$06`/COLPF3 or a fixed grey; verify).
- band + corner-triangle handling mirroring the `$6CAD` band DLI `$6D67`-equivalent for this chain.
- fixed line-doubled viewport reading a `boostBitmap` (or reuse `viewportBitmap`).

One copper for ALL boost sub-phases (stars, rings, standby-green) — only the decode source + the
pens/COLBK change per frame, driven by live mem[]. This avoids the copper-switch pillar drop and the
"wrong copper" mismatches.

--------------------------------------------------------------------------------
## 4. IMPLEMENTATION TASKS (ordered; each visually verifiable via the capture harness)

**T0 — Ground-truth the stars/rings field.** Create boost-stars + boost-tunnel atari800 savestates
(drive `atari800` to those moments) and run `/atari-dl-analyzer` on each: confirm DL, LMS base+stride
(48 vs 46), GTIA mode, and the live COLBK/COLPF/COLPM sources per scanline. Resolves the stride +
encoding questions before any code. (We only have `boost.a8s`=ascent + `mothership.a8s`=pre-B today.)

**T1 — BoostCopperList (viewport COLBK fade).** Build the dedicated copper (§3). Wire `setViewportBg`
← `mem[$0071]` and route `rsBoostViewport` to it instead of TunnelCopper. Verify: frames 445-452 show
the WHOLE viewport fading salmon→black (matches Atari A204), not snapping black.

**T2 — Pillars.** Colour the boost posts from the correct grey source, not `$02C8`. Verify: pillars
stay visible through the whole cinematic (A202/A204/A638).

**T3 — Stars decode (stride + content).** Parameterize the decode stride; decode the stars sub-phase
at stride 48 (GTIA-10 LUT) into the boost bitmap; consume the `g_planetRowLo/Hi` dirty band that
`draw_vline_pair` ALREADY publishes (no new publish needed) instead of full-field decode. Verify:
starfield appears + fades pen-by-pen (A242), and it's fast.

**T4 — Native `step_accum_sub_7e $6A8F` twin + reverse dirty-band publish.** It is the ONLY reverse-
chain driver still transpiled-only and it does NOT publish `g_tun*`. Write a native twin (mirror the
already-native `draw_ring_frame_step $670D`, incl. its `#ifdef ROF_PLATFORM_AMIGA` `g_tun*` /
`g_tunnelFieldDirty` publish) so the ring sub-phase uses the cheap `decodeTunnelBand` incremental
decode at stride 46. Register in `VALIDATE_FUNCS` + a `make validate` fixture. Verify: tunnel rings
render as clean nested rectangles (A323/A400), fast, no bowtie.

**T5 — Ring pens.** Confirm the boost tunnel ring pens track `$08D4-$08D9` on the new copper (teal,
cycling) — the palette conversion already works; just re-wire onto BoostCopperList.

**T6 — Standby handoff + band-triangle clear.** Port `clear_slot_0c87_0d87 $6A27` behaviour (the
row-by-row corner/band-triangle clear, A629→A638) and fix the reverse-tunnel→standby copper switch so
it lands on the uniform-green viewport + pillars (A638) without the broken partial door field
(fix frame 1399). May need an Amiga-only publish in `$6A27` or a boost-standby decode path.

**T7 — End-to-end timing + polish.** Re-capture the full cinematic; frame-compare against the Atari
set. Confirm cadence roughly matches (the fade/star ramps are frame-paced; if the Amiga is far
slower, profile — likely the residual full-decodes or the transpiled `step_accum_sub_7e` before T4).

--------------------------------------------------------------------------------
## 5. NATIVE-CONVERSION TARGETS (transpiled-only fns in the chain)

| Addr | Name | Status | Action |
|---|---|---|---|
| `$6A8F` | `step_accum_sub_7e` | TRANSPILED ONLY, no `g_tun*` publish | **T4: native twin + Amiga dirty-band publish** |
| `$6A27` | `clear_slot_0c87_0d87` | TRANSPILED ONLY, no Amiga publish | **T6: native twin or Amiga-only band-clear publish** |
| `$5367` | `launch_anim_dispatch` | transpiled | runs fine; leave (or fold into a native reverse driver) |
| `$52D7` | `vbi_handler_standby` | transpiled | runs fine; leave |

Already-native callees (reuse as-is): `display_setup $5F1D`, `draw_symmetric_span_loop $6642`,
`fill_horizontal_span $665D`, `fill_vertical_span $669C`, `advance_history_6a4d $6A4D`,
`draw_vline_pair $6C4D` (publishes `g_planetRowLo/Hi`), `update_object_distance $6BED`,
`advance_object_positions $6BA8`, `scroll_field_columns $6AEE`, `fill_terrain_columns $6AE5`,
`draw_ring_frame_step $670D` (the g_tun* publish MODEL), `build_row_addr_table $7460`,
`build_line_addr_table_1000(_stride) $65D0/$65D2`.

--------------------------------------------------------------------------------
## 6. OPEN QUESTIONS (resolve in T0 before coding)

1. **Stars field stride/encoding.** Is the stars sub-phase truly stride 48 under GTIA-10 (so decode =
   kGtia10 LUT + stride 48)? Or mode-D 2bpp (kModeD LUT, like the forward `$6CC2` planet)? T0 DL
   analysis settles it. The rings are confirmed GTIA-10 stride 46 (forward tunnel renders fine).
2. **Pillar grey source.** Which mem[] byte holds the boost pillar grey (COLPF3 `$06`? a fixed grey?)
   — needed for T2. (Flight uses its own `flLeftPost` colour path; find the launch-cockpit analog.)
3. **Band corner triangles during the fade.** On Atari the band-corner triangles show the fading
   COLBK too; confirm the band block should also track `$0071` (so the corners fade with the body),
   vs the `$6D67`-style band that leaves COLPF2 as the bright tone.
4. **Does the reverse tunnel reuse the `$1000` field the stars left, or clear it first?** Affects
   whether T3's stars decode and T4's ring decode can share one bitmap.

--------------------------------------------------------------------------------
## 7. VERIFICATION HARNESS (already built this session)

- Build: `cd amiga && make clean && make -j4 PROBES=1 FORCE_RETURN=1` (FORCE_RETURN auto-fires the
  boost ~7s into flight; PlatformAmiga.cpp vbiHandler probe). ⚠ `make clean` mandatory on PROBES
  toggle (stale-build trap).
- Deterministic routing probe: `g_boostRet/g_boostVp/g_liveCopper` (RescueOnFractalus.cpp) +
  `$CLAUDE_JOB_DIR/tmp/boost_route.gdb` via `GDBSCRIPT=... ./diag_run.sh <delay>` (`. ./env.sh` first).
- Visual: `./run.sh` (real FS-UAE; gdb freezes the display) + `screencapture` (needs macOS Screen
  Recording permission — granted this session). Dense-capture scripts in `$CLAUDE_JOB_DIR/tmp/
  boost_capture{4,5}.sh`; colour-per-frame scan via `magick ... -crop -resize 1x1` to find phases.
  Then `ffmpeg -i <mov> frame_%04d.png` to extract for frame-compare.
- The uncommitted probe infra (transpile.py `$3C75` VCOUNT hook, PlatformAmiga.cpp FORCE_RETURN,
  amiga/Makefile FORCE_RETURN) stays UNCOMMITTED per user decision — see boost-return-cinematic memory.

--------------------------------------------------------------------------------
## 8. BOOST-CINEMATIC FUNCTION REFERENCE (all functions in the chain)

### Atari 6502 / transpiled+native logic (drives the cinematic; runs faithfully on both backends)
| Addr | Name | File | Role in the boost |
|---|---|---|---|
| `$3D48/$3E0F` | `game_main_loop` / `game_main_loop_body` | rof_native.c | Loops over `display_setup`; the dock branch (`$3F50`, `player_lives $0072==2`) runs the ASCENT (depth `$0034` $40→$60), then breaks back to re-enter `display_setup` for the reverse cinematic. |
| `$4644` | `event_sequence_dispatcher` | rof_gen.c | In-flight keyboard dispatch; routes the **B** key ($15) to the boosters handler. |
| `$493D` | boosters handler (misnamed — see docs/rename.md) | rof_gen.c | Gated on `$003A` negative: sets mother-ship light `$0676=1`, clears rescue state, sets **`$0072=2`** (handoff sentinel), shows "FIRE BOOSTERS". |
| `$7BC6` | `setup_level_clear_state` | rof_gen.c | Mother-ship arrival: sets **`$003A=$FF`**, lights the indicator, "MOTHER SHIP!". (The boost render gate keys on `$003A==$FF`.) |
| `$5F1D` | **`display_setup`** | rof_native.c:8541 | **The cinematic APEX.** Re-entered after ascent; runs construction → salmon→black fade (`display_flags $0071` ramp) → draws the `$2000` starfield + `$1000` ring field → star-pen fade → reverse tunnel (`emit_dl_coord_pairs` loop) → standby handoff. Drives real Amiga frames via `ds_frame()`. |
| `$52D7` | `vbi_handler_standby` | rof_gen.c | The VBI active for the whole cinematic (VVBLKI). Calls `launch_anim_dispatch`. |
| `$5367` | `launch_anim_dispatch` | rof_gen.c | Per-frame step dispatch by flag precedence; the BOOST uses the **`$008D` reverse ring** branch → `step_accum_sub_7e`. |
| `$6A8F` | `step_accum_sub_7e` | rof_gen.c (⚠ transpiled-only, no `g_tun*` publish) | The `$008D` reverse ring-step; bumps `$008E` (the row-arm counter `display_setup` spins on). T4 target: native twin + Amiga dirty-band publish. |
| `$6A4D` | `advance_history_6a4d` | rof_native.c | During the reverse ring (`$008D<0`) copies `$08D8→$0071` each rotation (COLBK tracks the decaying ring). |
| `$6A27` | `clear_slot_0c87_0d87` | rof_gen.c (transpiled-only) | Row-by-row corner/band-triangle clear at the standby handoff. T6 target. |
| — | `draw_frame_pattern_seq` (`$6047`/`L_635f`) | rof_native.c | Plots the concentric ring field into **`$1000`** (deterministic geometry). |
| — | `emit_dl_coord_pairs` | rof_gen.c | Reverse-tunnel row emit: rewrites the `$3000` DL's mode-F **LMS from `$2000`→`$1000` row-by-row** as `$008E` counts (the gradual reveal; the Amiga currently switches the whole buffer, not per-row — issue 3). |
| — | `fill_region_2000` | rof_native.c | Fills the **`$2000`** field (the starfield background = value-8), one frame after the boost viewport first installs (source of issue 1's stale-frame). |
| `$65D0/$65D2` | `build_line_addr_table_1000(_stride)` | rof_native.c | (Re)seeds the `$073D/$0793` row-base tables for base `$1000`, stride 46. |

### Amiga rendering / conversion layer (src/platform/amiga/)
| Symbol | File | Role |
|---|---|---|
| `deriveRenderSignals()` | RescueOnFractalus.cpp | Sets `rsBoostReturn = standbyVbi && mem[$003A]==$FF` and `rsBoostViewport = rsBoostReturn && (mem[$008D]!=0 \|\| mem[$008E]==0)`. |
| `render()` boost branch | RescueOnFractalus.cpp (~1851) | When `rsBoostViewport`: picks `tunnelSrcBase = (mem[$008D]==0)?$2000:$1000`, decodes into `tunnelBitmap`, installs `TunnelCopperList`. Clears the bitmap on the install edge (issue-1 garbage-frame guard). |
| `tunnelSrcBase` | RescueOnFractalus.h | Decode source base: `$1000` rings / `$2000` starfield. |
| `decodeTunnelRect()` | RescueOnFractalus.cpp | GTIA-10 nibble → 3-bitplane decode of `tunnelSrcBase` (stride 46) into `tunnelBitmap`. |
| `updateTunnelCopper()` | RescueOnFractalus.cpp | Per-frame palette; BOOST mode (`rsBoostViewport`): viewport color00←`$0071` (fade, not `$08D8`), pillars←fixed `$06` grey (not `$02C8`), band corners track `$0071`. |
| `TunnelCopperList` | TunnelCopperList.cpp | The launch-cockpit copper reused for boost. Now emits **BPLCON2 PFxP=4** (sprites-on-top; fixes pillars-behind-stars — the flight copper leaves PFxP=0). Canopy-post sprites, band, viewport bitmap. |
| `buildPostSprites()` / `decodePostRLE()` | RescueOnFractalus.cpp | Decodes the canopy pillars ONCE from the constant tables `$4DFA`/`$4E09` into the `leftPost`/`rightPost` sprites. |

### Fixed so far vs OPEN
- **FIXED + confirmed (2026-07-14):** stars decode the `$2000` field (not `$1000`); boost palette (color00/pillars/band); BPLCON2 sprites-on-top (pillars visible over stars + tunnel).
- **FIXED (logic, 1-frame, not visually confirmable):** issue-1 stale-`$2000` garbage frame → clear-on-install.
- **FIXED + screencapture-confirmed (2026-07-16, commit 792e6d0):** issue-3 the stars→tunnel row-by-row REVEAL (the bowtie flash). `decodeBoostViewport()` decodes each viewport row from its per-row mode-F LMS in the LIVE `$3000` launch DL (`mem[$300A+3r]|mem[$300B+3r]<<8`) — `emit_dl_coord_pairs` (native twin, driven by `display_setup`) maintains those DL bytes faithfully on the Amiga. LMS in `[$1000,$2000)` = revealed rings (decode straight from that LMS → also gets the rev strand's mirrored bottom-half addresses); else stars from `$2000+row*46` (NOT the raw `$2f74` leftover, which points one row past the stars field). The reverse-ring emit converts rows from the CENTRE (42/57) outward over 20 VBI steps, so the rings grow from the centre — a mid-reveal frame shows the innermost rings emerging over black. **⚠ KEY: the `$3000` DL bytes ARE maintained on the Amiga** (the earlier "uniform `$2f74` / DL not maintained in mem[]" note was just step-0 before any emit; mid-reveal shows the exact sweep matching an offline simulation of the emit pointer math). No T4 twin was needed for this. Do NOT use `$073D/$0793` (plotter target).
- **OPEN:** Reverse-ring perf (full-decode/frame; T4 native `$6A8F` twin + dirty band). Standby handoff / band-triangle clear (`$6A27`; T6). Pink-vs-teal ring cycle to verify.
