# Instrument vocabulary — "Valkyrie Fighter Control Panel"

> Split out of `CLAUDE.md` 2026-08-10 to keep the always-loaded file small.
> **Read this when working on the cockpit / HUD / any named instrument, or on enemies and
> terrain objects.** Use these names in code/comments/commits.

The 19 cockpit instruments (game manual p.6), with their Amiga-screen position `x,y` and
`WxH` (user-supplied 2026-06-18). Positions are in the 320×216 display space; use them to
identify each instrument's Atari hardware source (player P0–P3 / missile M0–M3 /
mode-4·mode-D cockpit cell range).

| # | Instrument | x,y | WxH | Notes / source (✓=confirmed, ?=to verify) |
|---|---|---|---|---|
| 1 | **Score** | 304,20 | 16×8 | top-bar mode-6 text ~`$32E3` ✓ |
| 2 | **Compass** | 144,32 | 32×8 | top-bar mode-6 cells `$32C9-$32CA`, varies with heading octant `$280D` ✓ |
| 3 | **Wing Clearance Bars** | 40,128 | 240×10 | **BITMAP** (mode-D band field), NOT missiles — viewport row LMS `$2150` (flight) / `$1870` (planet), row 45. value-1 `$55`=salmon bar fill, value-2 `$AA`=centre marker, value-3 `$FF`=end caps. Width = clearance. ✓ (superseded the earlier missiles-M1/M2/M3 guess — see [[flight-scene]]) |
| 4 | **Thrust Level** | 8,152 | 40×60 | mode-4 dial-bar cells (x≈8-16), drawn via `$4581`/`draw_object_column` ✓ |
| 5 | **Dangerous Altitude** | 24,144 | 40×60 | mode-4 dial-bar cells (x≈24-32, e.g. `$3394`), lights near ground ✓ |
| 6 | **Artificial Horizon** | 56,138 | 32×28 | **PMG (NOT cells)** — dial frame is static $33xx bitmap; brown ground fill is Atari player P2 (COLPM2=`$26`, SIZEP2 dbl, buffer `$0E92-$0EB2`), boundary moves with pitch. Amiga = 2 sprites (`buildAHSprite`). See [[flight-scene]]. ✓ |
| 7 | **Altimeter** | 108,144 | 8×56 | **terrain-height bar = player P0** (`$0C98`, COLPM0 purple `$00D5`) + **ship-height bar = missile M3** (`$0B98`, light-blue `$00D6`). (CORRECTED 2026-07-07 from a firing capture — the old "P3 ship / P2 terrain, HPOSP2=`$00CB`" was WRONG: P3 is parked in flight and `$00CB` is the **laser shot**'s HPOSP2. See [[flight-pmg-map]].) |
| 8 | **Targeting Scope** | 136,151 | 50×33 | centre-lower mode-4 **bitmap** cells (x≈136); locked-target blip = cells `$2E-$31` + a generic P3 dome blob (`38 7C FE FE FE`); a flying saucer also mirrors as **P3** here. ✓ blip renders/updates on Amiga (DONE 2026-07-09, user-confirmed — was frozen; fixed via the lock-on dirty-hook pattern). ✓ (2026-07-07) |
| 9 | **Main Window** | — | — | the terrain viewport |
| 10 | **Cross Hairs** | 136,69 | 50×37 | **a "+" of PMG missiles**: M2 = vertical stem @ HPOS `$80` (centre), two segments (`$0B4D-5A`+`$64-71`) w/ a horizon gap; M3 @ `$74` + M1 @ `$85` (quad-width, `SIZEM=$CC`) = horizontal arms, lit only at the gap-centre line (`$0B5F`). Set in flight VBI `$505F-$5071`. Colour = Atari `$26` salmon (NOT grey). Visibility = the HPOS gate: `$A49A` sets `mem[$2840]=($28FC==0)?$00:$74` (`$00`=off-screen/hidden). **✓ PORTED 2026-07-09 as a plane3 overlay** (NOT a sprite — plane3 is free in the terrain body; `color04-07`=`$26` when visible, =terrain pens `color00-03` when hidden). See [[flight-pmg-map]] §3. |
| 11 | **Enemy Lock-On Indicator** | 136,193 | 48×6 | mode-4 cells `$3492-$3496` (`lock_on_indicator_tick $4229`, state `$007E`) ✓ |
| 12 | **Energy Level Indicator** | 204,144 | 8×56 | **P1 strip `$0D98`** gauge sprite, HPOSP1=`$00B5` (the working "right gauge") ✓ |
| 13 | **Long Range Scanner** | 232,138 | 32×28 | disc + housing = mode-4 cells (x≈232); the close-range proximity blink is the `$33DF/$33E0` pen swap (bit7 toggled at 50Hz by `startup_init $3FFA`, decoded by `decodeScannerBlinkCells`). **The flashing GUIDE DOT inside it is Atari missile M2, not a cell** — vertical = RANGE (`row = $1C - $28DA`, parked at `$1E`), horizontal = BEARING (HPOSM2 `$00CE`, which `$44D6` derives as `$28D9 + $AB` and clamps to `$B5`); colour COLPM2 `$26` ← `$00D0`. Amiga = sprite ch2 pen10 (COLOR22) via the SPR2PT dashboard re-point, `buildScannerDotSprite`. ⚠⚠ Two traps, both of which cost days — see [[feedback-vbi-driven-pmg-sprites]] and `docs/rename.md`: (a) the dashboard copper sets **PFxP=0, so every sprite is BEHIND the playfield** — the dot shows ONLY through the disc's transparent window, so `$1E`/`$B5` do not merely mis-place it, they HIDE it (that is how the original blinks it, and why our blink is now purely positional: pixels written once at init, never redrawn); (b) **both** coordinates must be PUSHED from the terrain DISPLAY pass (`rof_note_scanner_dot`), never sampled in the VBI — the two-pass render republishes each per pass and the free-running loop makes a VBI poll alias by CPU speed (invisible on a Fast-RAM A1200, fine on A500). ✓ |
| 14 | **Shields On** | 288,136 | 6×4 | tiny status light |
| 15 | **Mother Ship** | 300,140 | 6×4 | tiny status light |
| 16 | **Air Lock Open** | 312,144 | 6×4 | tiny status light |
| 17 | **Range To Pilot** | 284,155 | 8×10 | small digit |
| 18 | **Enemies Destroyed** | 276,171 | 8×10 | small digit |
| 19 | **Pilot Quota/Rescued** | 268,187 | 8×10 | small digit |

The **canopy posts** (cockpit window A-pillars) are a separate frame element = Atari players
P0 (`$0C32`, left) / P1 (`$0D32`, right), RLE-decoded from tables `$4DFA`/`$4E09`.

## ⚠ The bar instruments hang BELOW their dial — clip the pen at `kGaugeBottomLine` (2026-08-11)

Energy (#12), Altimeter terrain + ship (#7): the Atari **redraws** each player/missile strip every
frame (`draw_altimeter_bars $40E5`), so its bar is exactly `56 - top` rows and stops at the dial. The
Amiga keeps ONE **solid 56-row sprite and only moves its VSTART** (a deliberate perf trade — one `setY`
instead of 56 row decodes), so below the full value its bottom overhangs the dial by `top` rows. Every
cockpit copper list must therefore blank that pen at the dial bottom, `kGaugeBottomLine = 0x2c+144+56 =
line 244` — NOT at the floor line 252, which leaves 8 rows showing (user-reported in the reverse tunnel
and the post-mother-ship Standby, fixed 2026-08-11 in Standby/Doors/Tunnel/Planet; Flight already
blanks its three pens and additionally runs the dashboard at BPLCON2 PFxP=0).

Measured geometry of the energy column (gdb dump of `cockpitBitmap`, matching the screen pixel for
pixel — sprite hardware X `0x81+203` lands on screen x204):

| Amiga line | bitmap row | playfield at x204-211 |
|---|---|---|
| 188-243 | 16-71 | the dial slot: pen **0** = COLOR00 `$90` — the only rows where the bar belongs |
| 244-251 | 72-79 | dashboard closes over it: pen **2** = `$06` grey |
| 252-259 | 80-87 | floor: pen **0** = COLOR00 black |

**Sprite priority cannot substitute for the pen blank**: over a pen-0 playfield a sprite wins at every
BPLCON2 value, so the floor rows are only ever hidden by blanking the pen (see
`amiga/framework` notes + the `amiga-copper-lessons` memory). A bar at its FULL value fills the dial
exactly, so it hides this whole class of bug — set the value mid-range before judging one
(`make FORCE_MOTHERSHIP=1` halves energy at the B press).

## Enemies / terrain objects

Render paths mapped 2026-07-07 via live atari800 captures; detail in the `flight-pmg-map` memory.

The two enemy classes render differently. A **flying saucer = player
P3 PMG** (diamond `18 3C 7E FF 7E 3C`, grows with proximity), drawn in the viewport at `HPOSP3`=objX
and mirrored into the targeting scope (2nd P3 copy ~83 lines lower, scope-X via an HPOSP3 DLI); on the
Amiga reuse ch6/ch7 (altimeter, dashboard-only) via a copper `SPRxPT` mid-screen swap. **Ground objects
(gun emplacement, base, downed pilot) = terrain BITMAP value-2/3** (`terrain_plot_object $A63B`, from
`$0A00` map markers — pilot = `$64`); `terrain_plot_object $A63B` → `terrain_plot_pixel $A6D3` writes them
into the mode-D field via `($80),Y`. ✅ **These objects RENDER on the Amiga (DONE + user-confirmed 2026-07-09;
perf could be better but they're visible).** `renderFlightDirect` no longer converts the mode-D field for the
terrain body (rows 0–42) — the rasterizer writes plane2 dots straight to `g_flightDotPlane`, and the field is
read only for the windscreen band — so object pixels written into the field were being DROPPED; fixed by
hooking `terrain_plot_pixel` to also OR the object pixel into `g_flightDotPlane` (mirror `ROF_PLOT_DOT`, same
kRow120/kColMask4 geometry). See [[flight-pmg-map]]. (Flying saucers ARE fine — they're P3 PMG sprites, ported.) The **downed-pilot "blink" is a COLOUR-REGISTER CYCLE on `$00D9`** (hue 9, luminance
pulsing `$4`↔`$B`), NOT a graphics toggle → animated with a per-frame pen poke, don't redraw. **Flying saucer
P3 + scope mirror + altimeter multiplex are DONE (user-confirmed 2026-07-09); the M2/M1/M3 crosshair "+"
is DONE too (plane3 overlay, `$26` salmon, mem[$2840] visibility gate — user-confirmed 2026-07-09); the
downed-pilot blink, the targeting-scope blip, and the ground objects (emplacement/pilot/base/enemy fire)
all render on the Amiga too (user-confirmed 2026-07-09).**
