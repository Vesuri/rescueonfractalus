# Enhanced Amiga palette resolution

## 0. Status and scope

**Assessment / implementation plan, 2026-09-13. No implementation has landed.**

The port currently reproduces the Atari colour bytes faithfully, including the
Atari's limited luminance resolution. That is the correct parity baseline, but it
also means several fades deliberately waste colour resolution which OCS can show.
This document inventories those effects and proposes an Amiga-only enhancement
which leaves the emulated Atari state, gameplay timing, geometry and audio intact.

The key distinction is between:

- regular GTIA colour registers: 4 hue bits + 3 effective luminance bits; bit 0
  is ignored, giving 128 hardware colours;
- GTIA mode 9 pixels: a genuine 4-bit, 16-level luminance value; and
- Amiga OCS colour registers: 4 bits per RGB channel, giving 4096 colours.

The first implementation goal is not a generic smoothing filter. It is to use
otherwise-invisible odd values as half-luminance steps in known, intentional
palette ramps. Longer time-of-day transitions require a separate interpolator.

## 1. What the port does today

`amiga/assets/atari_pal.h` contains the calibrated Atari NTSC to 12-bit OCS
table. It intentionally maps each adjacent pair to the same value (`$06/$07`,
for example), because Atari colour-register bit 0 is unused:

```text
$00/$01 -> $000, $02/$03 -> $222, ...
```

The original game logic remains responsible for animation. It changes Atari
colour bytes in `mem[]`; the Amiga render path reads the live shadows, calls
`atariToOCS()`, and writes the result into the appropriate Copper-list colour
operands. The important consumers are:

- `updateStandbyCopper()` — mothership doors and Standby;
- `updatePlanetCopper()` — launch stars and planet approach;
- `updateFlightCopper()` — terrain, cockpit, death tint and atmosphere cycle;
- `updateDoorsCopper()` / `updateTunnelCopper()` — the forward and reverse
  launch cinematics;
- the EmptyCopperList branch in `render()` — the final full-screen death fade;
- `buildLogoSparkle()` — the Lucasfilm-logo sparkle.

These paths do not use the framework `Palette` class. `Palette` already has a
17-position RGB fade table, but the game passes live individual colours directly
to specialised Copper lists. It is useful implementation precedent, not a fade
facility that can simply be enabled.

### 1.1 GTIA mode 9 is already different

The Logo and Station bitmap fields use dedicated 16-entry mode-9 palettes in
`Gtia9CopperList.cpp`. Their odd entries are already generated/interpolated
instead of going through the ordinary paired lookup.

The Station starfield fade (`station_star_fade_in`, `$1E79`) advances every
non-zero mode-9 pixel from luminance 1 to 15, one step per frame. It therefore
already uses the Atari mode's full 16-level source resolution. A few adjacent
greys still coincide after calibrated-to-OCS rounding, but improving that would
mean changing the brightness curve or the animation duration, not merely exposing
unused Atari colour-register values.

## 2. Effect inventory

### 2.1 High-value fade candidates

| Effect | Original mechanism | Current effective steps | Assessment |
|---|---|---:|---|
| Launch stars after the tunnel | `$02C0-$02C3 = $00..$0C`, two frames per raw value | 13 programmed, 7 distinct | Excellent half-step candidate |
| Mothership door fade-in | `$0071 = $C2..$C8`, two frames per raw value | 7 programmed, 4 distinct | Excellent half-step candidate |
| Mothership level-wrap fade-down | `$0071 = $C8..$C0`, two frames per raw value | 9 programmed, 5 distinct | Excellent half-step candidate |
| Logo sparkle | COLPM0 `$0F..$00`, two frames per raw value | 16 programmed, about 8 distinct | Strongest additional case not in the original list |
| Boost/background salmon-to-black | `$0071` decremented to `$1F`, then zero | Odd/even pairs repeat | Excellent half-step candidate |
| Boost star/ring pens | each `$08D4..$08D9` descends `$0E..$00` | 15 programmed, 8 distinct | Excellent half-step candidate |
| Launch/boost teal buildup | `$08D9 = $90..$9A` | 11 programmed, 6 distinct | Good half-step candidate |
| Ship-death palette convergence | `$00CF..$00D6` rebuilt while phase `$20..$2A` advances | repeated/irregular holds from ignored bit 0 | Good targeted candidate |
| Ship-death full-screen fade | DMA off; `$00D4` decrements through salmon to black | odd/even pairs repeat | Excellent half-step candidate |
| Flight-entry terrain and later time of day | four table-driven terrain pens | discrete table transitions | Excellent candidate, but needs temporal RGB interpolation |

There are two different effects which can be called "stars fading in":

1. The Station scene's mode-9 bitmap fade already has 16-level source data and
   should initially remain unchanged.
2. The launch star sprites are coloured through regular GTIA shadows and only
   show seven distinct greys. This is the clear improvement target.

The mothership doors also combine two mechanisms. Their opening is display-list
geometry and cannot be made smoother with palette resolution. The green field's
fade-in, and the fade-down/up around an in-place level-wrap rebuild, are genuine
palette fades.

Ship destruction is similarly composite. Its initial random terrain flashing is
intentional noise. The subsequent cockpit-to-salmon convergence and the final
solid salmon-to-black screen are the parts that should be smoothed.

### 2.2 Colour effects which should not be treated as fades

The following change colours, but smoothing them would change their character:

- object-impact and enemy-fire flashes;
- the level-complete 60-frame `RANDOM | 4` screen flash;
- the Title Screen text cycle and the long-pause palette strobe;
- lock-on, message, gauge-warning and station-dot cycles;
- tunnel colour-ring rotation itself, as distinct from the explicit pen fades.

`animate_clear_colors_timed` and `clear_colors_sweep_5x` are misleading names in
this context. They primarily schedule frame waits and sound events during the
pilot/airlock sequence; they do not progressively fade the display palette.

### 2.3 Small fidelity issue adjacent to this work

The targeting reticle uses the stale constant flight COLPF3. The Atari death
routine writes that hardware register during the death tint, but the Amiga path
currently leaves the visible reticle constant (and normally hides it unless a
target is locked). This is a fidelity omission, not a resolution problem. If the
death palette code is touched, decide separately whether to wire it; do not count
it as a smoothing improvement.

## 3. Proposed architecture

### 3.1 Preserve a faithful mode

Introduce the work behind an `ENHANCED_PALETTE` build option initially. The
faithful path must keep using `atariToOCS()` unchanged. This gives direct A/B
captures and protects the parity baseline while the desired look is evaluated.

Do not change `mem[]`, the native/transliterated routines, wait counts, game
state, audio scheduling, or scene geometry. Enhanced colour is an Amiga display
interpretation layered on the existing state.

### 3.2 Add a generated 16-luminance-per-hue lookup

Add a second conversion helper, provisionally `atariToOCSHalfLuma()`, backed by a
generated table:

- even colour bytes reproduce `atariToOCS()` bit-for-bit;
- odd bytes are calibrated half-luminance points within the same hue;
- the top odd value (`hue | $0F`) is generated/extrapolated within that hue and
  must never interpolate into the next hue;
- all values are precomputed 12-bit OCS words, so the render hot path remains a
  single lookup.

This follows the existing mode-9 palette generation model. An extra 256-word
table costs 512 bytes. If size becomes material, a 16x16 table is semantically
the same; avoid runtime colour-space calculations on the A500.

Do **not** replace the existing lookup globally. Atari bit 0 is sometimes random
noise and sometimes hidden control state. The lock-on colour, for example, uses
bit 0 as its direction flag. Globally displaying it as luminance would leak
internal state into the image and introduce shimmer. Random flashes would also
gain an unintended brightness component.

### 3.3 Route only known ramps through the new lookup

Use the enhanced helper only for colour operands belonging to a positively
identified fade phase:

1. Logo sparkle COLPM0.
2. Launch star colour while `$02C0-$02C3` perform the `$00..$0C` ramp.
3. Standby/door `$0071` during fade-down and fade-up.
4. Boost `$0071`, the six `$08D4-$08D9` pen fades, and the `$90..$9A` buildup.
5. Death-tint `$00CF-$00D6` and the EmptyCopperList `$00D4` fade.

The settled endpoint colours remain the ordinary calibrated values. Prefer
existing scene/phase signals where they uniquely identify a ramp. If a phase
cannot be identified robustly, add a small Amiga-only hook at the writer rather
than infer it from a colour range that may occur elsewhere.

Be mindful of the Copper publication model:

- Standby and Planet poke stable lists on change.
- Doors and Tunnel populate a back Copper list and publish it at vblank.
- Tunnel ring drawing also has a live-list, one-step-ahead colour poke to align
  the palette with single-buffered bitmap drawing.

The enhanced conversion belongs before those existing setters; it must not alter
their pointer, WAIT, buffering or publication rules.

### 3.4 Time-of-day interpolation is a separate feature

The atmosphere block in `vbi_handler_flight` selects an altitude band, then reads
four colours from `$07F9`, `$0823`, `$084D` and `$0877`. `$08A1` is the slow
countdown, `$08A2` is the phase, and `$08A3` is selected from the 16-entry step
table at `$364B`.

Those table colours are mostly even. A half-luminance lookup alone will not
improve their transitions. The Amiga renderer should instead synthesize the four
terrain pens between the current and next atmosphere-table states:

1. Derive the current altitude sub-band exactly as the flight VBI does.
2. Resolve current and next atmosphere base entries from the live phase.
3. Convert both four-pen endpoints with the faithful lookup.
4. Interpolate the OCS R/G/B nibbles using a small precomputed fraction table.
5. Publish one coherent four-pen result to `FlightCopperList`.
6. Use that same result for the windscreen colours which inherit terrain pens and
   for the hidden-crosshair palette, so masking remains exact.

Sixteen interpolation positions across an original atmosphere interval should be
enough to remove visible stepping while staying cheap. It also avoids 32-bit
multiply/divide, which is prohibited on the 68000 target. The precise interval
and whether blending anticipates or follows an Atari table boundary should be
settled from an A/B video; anticipating the next endpoint can arrive exactly at
the original boundary without extending the day/night cycle.

Altitude-driven palette changes must not be accidentally lag-filtered. Interpolate
the two time-of-day palettes at the *current* altitude sub-band each frame rather
than smoothing the final published colour blindly.

### 3.5 Station mode-9 stars: defer

Do not alter `station_star_fade_in` in the first implementation. It already uses
one genuine mode-9 luminance step per frame. More temporal levels would require a
longer fade, and making its grey palette uniformly `$000,$111,...,$FFF` would
discard the calibrated Atari luminance anchors. Revisit only after an enhanced
launch-star A/B demonstrates that the Station fade still looks materially worse.

## 4. Implementation stages

### Stage 1 — conversion table and smallest isolated effects

- Add/reuse a generator for the 16-luminance-per-hue OCS table.
- Add `atariToOCSHalfLuma()` without changing `atariToOCS()`.
- Apply it to the Logo sparkle and launch-star fade.
- Add a host test proving all even entries are identical to the faithful table,
  odd entries stay within their hue, and each ramp is monotonic where the
  calibrated endpoints permit it.

These are isolated and make the visual premise easy to judge.

### Stage 2 — mothership and boost ramps

- Route the door fade-up/fade-down `$0071` through the enhanced lookup.
- Cover both the normal launch and the post-level/boost return.
- Route only the explicit `$08D4-$08D9` and `$08D9` buildup fades, leaving normal
  colour-ring rotation faithful.
- Verify the live-list ahead poke still matches the bitmap group being drawn.

### Stage 3 — death cinematic

- Enhance the pre-blank `$00CF-$00D6` convergence coherently across the top bar,
  cockpit body, gauges, compass, terrain-facing colours and canopy posts.
- Enhance the EmptyCopperList `$00D4` full-screen fade.
- Keep the preceding RANDOM flash unchanged.
- Make an explicit decision on the locked-reticle fidelity issue.

### Stage 4 — continuous atmosphere/time of day

- Implement the four-pen phase interpolator in the Amiga renderer.
- Add endpoint and wraparound tests for the `$364B` cycle.
- Check every altitude sub-band and phase transition.
- Measure its per-frame cost on the A500; use table lookups and 16-bit operations.

### Stage 5 — decide defaults

Compare faithful and enhanced captures on real/representative OCS output. Decide
whether `ENHANCED_PALETTE` remains optional or becomes the default Amiga look.
Keep a faithful build available regardless.

## 5. Verification

This is intentionally outside the byte-identical 6502 contract: the game memory
must remain identical while only Copper colours differ. Validation therefore
needs both invariants and visual captures.

### 5.1 Automated invariants

- The complete `mem[]` state and frame counts are identical with the option on
  and off for each covered sequence.
- Every even source colour produces exactly the old OCS word.
- Every fade begins and ends on the same colour and frame as the faithful build.
- No enhanced conversion is used by lock-on direction state, RANDOM flashes,
  Title cycling, pause strobing, or settled static palettes.
- All four time-of-day terrain pens share one interpolation fraction.
- No 32-bit software multiply/divide helper appears in the Amiga binary.

Add a focused host proof for the generated table/interpolator. Existing native
validation remains useful to prove that no original game routine changed, but it
cannot declare the Amiga-only visual enhancement correct.

### 5.2 On-Amiga captures

Capture the source byte and displayed OCS word once per vblank for:

- Logo sparkle `$0F..$00`;
- launch stars `$00..$0C`;
- door `$C8..$C0` and `$C2..$C8` paths;
- boost background, six pen fades and teal buildup;
- death pre-blank tint and post-blank `$00D4` ramp;
- one complete dusk or dawn atmosphere boundary at several altitudes.

For the short fades, assert that odd frames now carry a distinct in-between OCS
word while even frames and endpoints are unchanged. Also inspect video rather
than relying only on colour logs: OCS nibble rounding can legitimately collapse
an interval, and a mathematically distinct colour is not automatically a better
perceptual transition.

## 6. Recommended priority

1. Logo sparkle and launch stars — clearest benefit, least coupling.
2. Door and boost fades — strong benefit, but more Copper phase/buffering care.
3. Ship-death ramps — coherent multi-region update required.
4. Time of day — largest qualitative gain and largest design surface.
5. Station mode-9 stars — no change unless direct comparison justifies departing
   from the current calibrated, frame-exact implementation.
