# Flight sprite-multiplex plan (Amiga)

Goal: a **principled, flexible** mapping of the Atari flight PMG objects onto the Amiga's 8
sprite channels, so elements can be moved/added without re-deriving hardware timing each time.
Replaces the ad-hoc per-element allocation that produced the ch7 altimeter-ship regression.

## 1. The key realisation — the Atari already multiplexes its players vertically

Each Atari player buffer ($0C/0D/0E/0F00) holds DIFFERENT content at different scanlines: e.g.
**P0** is the *left canopy A-pillar* in the viewport AND the *altimeter terrain bar* down in the
dashboard — one hardware object, reused top-to-bottom. So the natural Amiga mapping is: **pin one
Amiga sprite channel to each Atari PMG object and re-point it (SPRxPT) at region boundaries to
follow that object's own vertical multiplex.** The Atari PMG assignment IS the Amiga assignment.

The only impedance mismatch: an Atari player at `SIZEP` quad (~32px) is TWO 16px Amiga sprites, so
the wide elements (A-pillar+triangle frame, AH ground-fill) consume a *pair* of channels.

## 2. Three sprite regions + the ONE arming rule

Flight scanline regions (PAL, `FlightCopperList`):

| Region     | Scanlines | Sprite elements                                          |
|------------|-----------|----------------------------------------------------------|
| Viewport   | 86–171    | canopy A-pillars, laser shot, viewport-P3 (saucer), crosshair |
| Band       | 172–179   | windscreen corner triangles (+ frame edge missiles)      |
| Dashboard  | 180–267   | AH fill, altimeter (terrain+ship), energy, scope-P3      |

Boundary re-point line = **180/181** (AH re-points at 180 first-thing after the cockpit WAIT;
scope + altimeter-ship at 181 to avoid crowding the bitmap-pointer fetch — see FlightCopperList).

**THE ARMING RULE (the whole plan rests on this).** An Amiga sprite channel only re-arms on the
control-word fetch that follows a **VSTOP**. So a reused channel must present **exactly ONE active
sprite whose VSTOP == the boundary (180)** and **no earlier VSTOP** — otherwise the channel
auto-fetches the outgoing sprite's `0,0` terminator, disarms, and the boundary re-point comes too
late (there is no later VSTOP to trigger a fetch). Re-pointing SPRxPT alone does NOT re-arm.

⚠ **And the re-point must land in the window [line 179 after that channel's data fetch … line 180's
post-VSTOP control re-fetch].** A re-point deferred to line 181 fires *after* the re-fetch (which
already read the terminator → disarmed) and never arms — *the* bug that hid the altimeter-ship + the
scope dome for two sessions (the "sprites don't display until 188 so re-point one line lower"
reasoning was wrong: arming is gated on the VSTOP line, not the display line). Verified by gdb copper
dump (`amiga/sprite_probe.gdb`).

Emit it as ONE `WAIT(179, 0xC0)` (the measured sweet spot on FS-UAE), sprite re-points FIRST then the
6 cockpit bitmap pointers. `0xC0` lands the 8 sprite re-points in line 179's tail (safe: SPRxPT pokes
don't touch bitplanes and each channel's line-179 data is already fetched, so this only affects the
line-180 control re-fetch = the arming fetch), and those 8 moves carry the bitmap-ptr moves into their
window: after the band's DDFSTOP (re-pointing BPLxPT can't corrupt line 179's right edge) yet before
line 180's DDFSTRT (no left-edge glitch). The hpos is narrow: `0xB0` is too early (bitmaps land before
DDFSTOP → garbage bitplane), `0xE0` too late (the 14 moves overrun DDFSTRT → left-edge glitch).

Two valid ways to satisfy the rule:
- **Pattern A — tall sprite:** the upper-region sprite runs (transparently padded) from its
  VSTART down to VSTOP 180. Used by: the canopy posts (ch0/ch2, 86→180), the AH multiplex, and
  **viewport-P3 (ch7)** after this plan's fix. A tall mostly-transparent sprite costs the same
  DMA as a short one (sprites fetch 2 words/armed line regardless) — it is the cheap, robust default.
- **Pattern B — single late-start sprite:** the channel is idle in the viewport, its FIRST armed
  sprite starts mid-screen (e.g. band triangle VSTART 172) and runs to VSTOP 180. One VSTOP → clean
  re-point. Used by the band triangles (ch1/ch3).

The bug that motivated this plan: viewport-P3 was an 8-row sprite parked at VSTART 86 → VSTOP ~94
→ early disarm → the ch7 altimeter-ship re-point at 181 never armed. Fixed by Pattern A (tall
86→180). This rule now applies to ANY future viewport↔dashboard reuse (e.g. the crosshair).

## 3. Channel × region map (target allocation)

`P#`/`M#` = the Atari PMG object the channel mirrors. `pair` = the SIZEP-quad companion channel.

| ch | pair | Viewport 86–171        | Band 172–179          | Dashboard 180–267        | X source                    |
|----|------|------------------------|-----------------------|--------------------------|-----------------------------|
| 0  | 0/1  | left A-pillar (P0)     | left-tri inner (P0)   | AH fill left (P2)        | post X → AH X               |
| 1  | 0/1  | idle                  | left-tri outer (P0)   | AH fill right (P2)       | (pair of ch0)               |
| 2  | 2/3  | right A-pillar (P1)   | right-tri inner (P1)  | **Long-Range-Scanner dot (M2)** | mem[$00CE] (bearing)  |
| 3  | 2/3  | idle                  | right-tri outer (P1)  | **scope-P3 dome (P3)**   | **fixed scope-box X**       |
| 4  | 4/5  | laser shot (P2)       | (laser)               | idle (free)              | shot X `mem[$00CB]`         |
| 5  | 4/5  | free                  | free                  | energy (P1)              | energy X                    |
| 6  | 6/7  | free                  | free                  | altimeter terrain (P0)   | altimeter X                 |
| 7  | 6/7  | **viewport-P3 (P3)**  | (P3 tall, transparent)| altimeter ship (M3)      | viewport `mem[$2870]` → altimeter X |

Free viewport capacity for future elements: ch1, ch3 (86–171), ch5, ch6 (86–179). The **crosshair
(M2)** — not yet ported — slots onto ch5 or ch6 as a Pattern-A tall sprite (reticle drawn at rows
113–149, transparent-padded to 180, re-point to the gauge at 181). No new channel needed.

## 4. Two P3 copies at DIFFERENT X — resolved

The saucer/emplacement is Atari P3 written to TWO buffer windows in one loop (`draw_player3_object
$42A7`): the **viewport copy** (`$0F00 $32–$85`) and the **scope copy** (`$0F00 $98–$B8`, ~$53
lower). On the Amiga these are on **separate channels** (ch7 viewport, ch3 scope), so each carries
its own SPRxPOS → independent X. (A hardware sprite latches X once at VSTART; you can only change a
channel's X across disjoint vertical bands via a SPRxPT re-point to a second control block — which
is exactly what the region re-points already do.)

- **Viewport-P3 X = `mem[$2870]`** (the real HPOSP3, tracks the target). Correct.
- **Scope-P3 X = `mem[$00CC]`** (FOUND 2026-07-08 — the scope-X source the memory had hypothesised).
  The flight **dashboard DLI `$4A78`** (VDSLST=`$49EE` chain, table `$4AD9/$4ADE` = [`$4A11`,`$4A1F`,
  `$4A40`,`$4A78`,`$4ACD`]; DLIs absent from `listing.txt` — now seeded in `entrypoints.csv`) reloads
  `HPOSP3 ← mem[$00CC]` before the scope band, and `draw_player3_object $43C4` sets `$CC` = the
  viewport X *clamped* into the scope-box window. For a saucer `$CC==$2870` (so X was ~right); they
  diverge for other target positions, so `$CC` is the correct general source. Applied to
  `buildScopeP3Sprite` (was `mem[$2870]`). ⚠ hw-X `0x81+(cc-$32)*2` is HARDWARE X, not screen px:
  `$7C`→hw `0x115`→screen ~148 = scope-box centre (earlier "x≈277 far right" was reading hw-X as
  screen px — the X was closer than it looked; the more likely miscalibration is **Y**, see below).

## 5. Enemy render paths (do not conflate)

- **Flying saucer:** viewport = P3 (ch7) + scope = P3 (ch3). Both present together, grow with
  proximity. (Dump `flight_saucer1.bin`: viewport `$0F4B/$4C`, scope `$0F9F`.)
- **Gun emplacement (ground object):** viewport = **terrain BITMAP value-2** (via
  `terrain_plot_object $A63B`, already plotted into plane2 — low-contrast, NOT a sprite), scope =
  P3 dome (ch3). (Dump `flight_emplacement_scope.bin`: P3 nonzero ONLY at `$0FAB–AF` = the scope
  dome; NOTHING in the viewport window $32–$85.) ⇒ "can't find gun emplacements" is a viewport
  **contrast/gameplay** issue, NOT a P3-sprite bug — verify against a live emplacement.
- **Downed pilot:** viewport = terrain bitmap value-2; the "blink" = COLPM3 colour cycle on
  `mem[$00D9]` (couple it to the P3 pen once P3 is on screen).

## 6. Status / work items

- [x] **ch7 arming (altimeter ship gone)** — viewport-P3 made a Pattern-A tall sprite (86→180).
- [x] **scope-P3 X** — now `mem[$00CC]` (found the source = dashboard DLI `$4A78`).
- [x] **table-driven re-points** — `setDashboardSprite(ch)` + `kDashRepoints` table (FlightCopperList).
- [ ] **scope-P3 Y calibration** — dome maps to screen ~195–207; verify vs the scope box on FS-UAE.
- [ ] **viewport-P3 vs scope-P3 X/Y calibration** — with a live saucer on screen.
- [x] **crosshair (M2, viewport)** — done as a plane3 overlay (NOT a sprite; see flight-pmg-map).
- [x] **Long Range Scanner dot (#13, M2 dashboard)** — done: ch2 dashboard re-point (kDashRepoints {2,...}),
      buildScannerDotSprite reads missile buf $0B00 + mem[$00CE], red pen10=COLOR22 (commit f8bffc4, confirmed).
- [x] **scanner close-range blink — FIXED + user-confirmed 2026-07-21 (at both speeds).** NOT the M2 dot and
      NOT a colour/DLI trick: it is the two **mode-4 bitmap cells $33DF/$33E0** (LR scanner bottom row, screen
      (240,160)) toggling their **bit 7** ($1E/$1D<->$9E/$9D).  In ANTIC mode 4 bit7 selects the colour
      interpretation, so the toggle swaps those pixels between COLPF2 $2C (salmon) and COLPF3 $26 (red) — a
      colour blink driven by the cell value, which is why every colour-register/shadow probe stayed constant.
      Driver = startup_init() ($3FFA, in the flight VBI): bit7 set when pilot range mem[$0642] is 1 or 2 AND
      (range & the free-running counter mem[$004B])==0.  **The range picks which counter bit is tested → the two
      speeds:** range 1 → bit0 → ~2-frame period (fast); range 2 → bit1 → ~4-frame (slow); range 0 or >=3 → solid.
      Measured on atari800 (per-frame cell capture of a800dumps/lrscanner.a8s, range 2: $33DF toggled $9E<->$1E;
      colour regs/charset/shadows all constant).  **Amiga bug:** startup_init() toggled mem[] every frame but the
      DECODE ran only in the main-loop render() (throttled to the ~5-6fps terrain rate) → blink far too slow, range
      1≈2.  **Fix:** decode $33DF/$33E0 straight into the cockpit bitmap at 50Hz IN THE VBI ISR —
      PlatformAmiga::flightScannerTick() (called from game_vbi_isr's flight branch beside flightShotTick) →
      RescueOnFractalus::decodeScannerBlinkCells() (change-detect + decodeCockpitSpan, cheap).  Mirrors the
      laser-sprite-at-50Hz precedent.
- [ ] **gun-emplacement visibility** — confirm it's the bitmap-contrast path, not a spawn bug.

All code changes build clean (Amiga cross-build); FS-UAE visual verification pending.
</content>
</invoke>
