# Native four-bitplane cockpit migration plan

## Goal

Replace the Amiga cockpit's runtime Atari character/tile-to-bitplane conversion with native
Amiga planar artwork, then expand every non-terrain cockpit region to four bitplanes, and finally
replace the energy and altimeter sprites with playfield pens whose fill heights are controlled by
the Copper.

The migration is deliberately divided into pixel-identical stages. Enhanced artwork is a later
project and must not be mixed into this work. At the end of every phase, the cockpit must still
look exactly like the current faithful version.

The final display structure is:

| Region | Display lines | Final depth | Purpose |
|---|---:|---:|---|
| Cockpit top | 42 | 4 bitplanes | Message/status line, score, compass, and future surrounding artwork |
| Terrain viewport | 86 or 94, depending on scene/render path | 3 bitplanes | Existing terrain, doors, tunnel, planet, and flight rendering |
| Windscreen-bottom band | 8 | 4 bitplanes | Lower windscreen frame and clearance-band artwork |
| Dashboard | 80 | 4 bitplanes | Cockpit body and instruments |

Only the terrain viewport remains three-plane. The Copper therefore changes the active depth
`4 -> 3 -> 4` as it moves down a cockpit-bearing frame.

## Enhanced Graphics option and faithful fallback

The native four-plane cockpit must be selected by the existing **Enhanced Graphics** option: the
same WHDLoad `Custom4` toggle that selects the improved Lucasfilm Games logo. It must not acquire a
separate player-facing option.

The two modes are:

| Enhanced Graphics | Cockpit implementation |
|---|---|
| Off | Existing faithful cockpit representation, plane counts, dual-playfield composition, and gauge sprites |
| On | Native planar top/band/dashboard path described by this plan; initially pixel-identical, later able to load enhanced artwork |

This preserves an accessible faithful A/B reference after enhanced cockpit artwork is introduced.
During phases 1-3 the two paths should look identical; the toggle selects architecture, not yet a
different appearance. Once replacement artwork is added, only the enhanced path changes visually.

The selected mode must be fixed during startup. Do not support changing it while a game is running:
the two paths have different bitmap allocations, Copper layouts, plane counts, and sprite ownership.

For WHDLoad, extend the existing `Custom4` handling so it both installs the enhanced-logo override
and selects the enhanced cockpit configuration word. A suitable retained startup block should use
the same loader-patchable magic-word pattern as Enhanced Terrain and Enhanced Palette. For standalone
testing, the existing `ENHANCED_LOGO=1` path should select the same complete Enhanced Graphics mode;
it may be renamed to `ENHANCED_GRAPHICS=1` with `ENHANCED_LOGO=1` retained as a compatibility alias.

Only the selected path's CHIP bitmaps and sprites should be allocated. Do not keep both live display
sets in CHIP RAM merely to permit an unsupported runtime toggle. Both source implementations and
their embedded assets may remain in the executable, subject to the normal size audit.

## Why this order

There are three independent changes here, and combining them would make visual regressions hard
to locate:

1. Change the source representation from Atari characters/tiles to native planar data, without
   changing the number of displayed planes or the palette.
2. Add a zero fourth plane to the cockpit top, windscreen band, and dashboard, without changing
   their visible pen indices.
3. Re-encode the dashboard for a normal four-plane playfield, remove dual-playfield mode, and
   move the energy and altimeter displays from sprites into dedicated playfield pens.

This sequence separately proves the asset pipeline, four-plane display setup, and final pen/sprite
composition.

## Current implementation being replaced

The present cockpit bitmap is a persistent 320x88, three-plane, interleaved bitmap. Its first
eight rows contain the four doubled mode-D entries at `$350D`; the remaining 80 rows contain the
ten mode-4 dashboard rows at `$332D`.

`RescueOnFractalus::decodeCockpitSpan()` currently converts the live Atari representation into
that bitmap:

- Mode-D bytes are decoded from packed Atari 2-bpp samples.
- Mode-4 screen bytes select glyphs in the `$3800` cockpit character set.
- Character bit 7 selects the alternate value-3 colour.
- Each four-sample Atari row is expanded to eight Amiga lores pixels.
- The output is specially encoded as two playfields: BPL1+BPL3 form PF1 and BPL2 forms the
  light-grey PF2 stencil.

The writer-driven dirty registry avoids a full conversion every frame, but runtime conversion is
still needed for digits, dial cells, lock-on, status lights, and scanner cells. A full decode is
also performed after the Standby cockpit has been constructed.

The cockpit top is a separate two-plane bitmap. Its changing score/message text and compass are
also rendered by interpreting Atari character data at runtime.

The dashboard uses dual-playfield mode because PF2's light-grey stencil must sit in front of the
instrument sprites while the other cockpit colours sit behind them. The energy bar, altimeter
terrain bar, and altimeter ship bar are solid moving sprites. Their fixed-height sprite data
currently requires Copper colour blanking at the bottom of the dials to hide overhang.

## Scope

This migration includes:

- The 42-line top portion of every cockpit-bearing screen.
- The score and changing status/message text displayed in that portion.
- The compass housing and changing compass states.
- The eight-line windscreen-bottom band in Standby, Doors, Tunnel, Planet, and Flight.
- The complete 80-line dashboard.
- All changing dashboard tiles, digits, lights, and dial states.
- Energy and both altimeter indicators.
- All affected Copper lists and palette-update paths.
- Build-time asset generation and host/on-target verification.
- Startup selection through the existing Enhanced Graphics/`Custom4` option.

The following are not removed from the faithful Enhanced-Graphics-off path in this migration:

- The three-plane terrain/viewport renderer.
- Canopy-pillar and windscreen-corner sprites in the viewport.
- Artificial-horizon fill sprites.
- Targeting-scope, long-range-scanner, player-shot, and gameplay-object sprites.
- The original Atari screen RAM writes needed for faithful game logic and validation.
- Character rendering on non-cockpit screens, unless shared code must be generalized to support
  the new cockpit-specific planar path.
- Enhanced cockpit artwork. This plan only prepares for it.

## Non-negotiable constraints

### Visual fidelity

- Phases 1-3 must be pixel-identical to the current Amiga presentation.
- Palette cycling during pause, the death fade, and all scene-entry/exit transitions must remain
  faithful.
- The Atari implementation and captured Atari states remain the behavioral source of truth.
- PlatformSDL is useful for diagnostics but is not the visual oracle.

### Performance

- No phase may introduce a measurable performance regression on the A500 target.
- The expected final result is faster because runtime character/tile conversion, full cockpit
  decoding, gauge sprite construction/mirroring, and several sprite/Copper updates disappear.
- The fourth plane increases display DMA, so performance neutrality must be measured rather than
  inferred.
- The fourth plane must be active only in the cockpit top, windscreen band, and dashboard. It must
  be disabled throughout the terrain viewport.
- Small planar patches should be copied directly as bytes. Do not introduce a general pixel
  converter, per-pixel loops, software multiplication/division, or a blitter setup whose overhead
  exceeds these tiny copies.
- Gauge Copper data should be regenerated only when a gauge height or relevant colour changes.
- Pointer publication must remain synchronized with vblank. Colour-only Copper changes may be
  prepared outside the ISR only if the existing synchronization rules make that safe.
- Measure both toggle positions. Enhanced Graphics off must remain at the faithful baseline;
  Enhanced Graphics on must be neutral or faster despite its additional bitplane DMA.

### Memory

- A modest increase in chip and executable memory is accepted.
- Expanding the currently visible regions to four planes adds approximately 6,880 bytes of live
  chip bitmap storage compared with their present depths:
  - two additional top planes: 3,360 bytes;
  - one additional band plane: 320 bytes;
  - one additional dashboard plane: 3,200 bytes.
- Embedded source assets and patch atlases add executable/data size separately.
- The existing `__chip` macro specifically targets `.MEMF_CHIP`, which this project's linker script
  deliberately converts to a CHIP BSS hunk because its current users are runtime-generated audio
  buffers. Do not put initializers in that particular section: its contents are intentionally
  discarded.
- This is not a restriction on Amiga CHIP data hunks. Planar assets that the hardware can consume
  as-is should preferably live in a distinct initialized CHIP data hunk, using the toolchain's
  `.INCBIN.MEMF_CHIP`/`INCBIN_CHIP` convention or an equivalent explicitly writable CHIP data
  section. They can then be displayed directly without a startup copy.
- Use a separate allocated CHIP bitmap only when the surface must be conditionally allocated,
  independently restored, double-buffered, resized, or modified in a way that should not alter the
  embedded source. Account for the fact that an executable-resident CHIP data hunk consumes CHIP
  RAM even when Enhanced Graphics is off; optional assets may instead belong to the WHDLoad
  Enhanced Graphics provider or an enhanced-only standalone build.

## Target architecture

### Independent display surfaces

Replace the combined cockpit bitmap with three explicitly owned surfaces:

- `cockpitTopBitmap`: 320x42; two planes during phase 1, four thereafter.
- `cockpitBandBitmap`: 320x8; three planes during phase 1, four thereafter.
- `dashboardBitmap`: 320x80; three planes during phase 1, four thereafter.

The separation is important. A bitmap interleaved for four planes has a different row stride from
a three-plane bitmap. Reusing one allocation for mixed-depth regions would require special modulo
handling and would preserve unwanted coupling between the band and dashboard.

It also makes the eventual art workflow straightforward: the top, band, and dashboard become
independent indexed assets with explicit pen contracts.

These surfaces belong only to the Enhanced Graphics path. The faithful path retains its present
allocations and Copper composition. Initialization must choose one set or the other, not allocate
both.

### Native planar source assets

Add an offline asset generator that produces Amiga-ready interleaved planar data. Its inputs should
be deterministic, checked-in source data rather than a screenshot. During the fidelity phases it
may derive that data from:

- the verified `$3800` cockpit tile set;
- canonical visible screen data for the top, band, and dashboard;
- the existing compass and digit lookup tables;
- explicitly described dynamic instrument states.

The generated outputs should include:

- Base planar images for the top, band, and dashboard.
- A planar glyph/patch atlas for changing score and message characters.
- Planar compass patches.
- Planar cockpit-digit patches.
- Planar patches for lock-on, status lights, scanner blink cells, and changing dial cells.
- Metadata mapping each patch to its destination rectangle and semantic state.
- A human-viewable indexed preview image for each region.

The runtime must never decode Atari glyph rows into pixels. Selecting a preconverted patch and
copying its plane bytes is allowed; interpreting the tile's packed pixels is not.

### Compatibility boundary

Atari screen RAM should continue to be written exactly as before so that native-vs-transpiled
validation and game behavior remain intact. Initially, the current dirty hooks can remain the
notification mechanism:

```text
Atari-compatible writer
    -> existing dirty notification
    -> address/state-to-patch lookup
    -> direct planar patch copy
```

After the new path is proven, internal names and tables can become semantic, such as `drawDigit`,
`setLockOn`, and `setShieldLight`, while retaining the Atari memory writes as compatibility state.
This cleanup must not be combined with the first representation change.

The faithful toggle-off path continues consuming its existing screen-RAM representation. Therefore,
removing a decoder in the phase descriptions below means removing it from the enhanced path and its
per-frame execution, not deleting code still required by faithful mode. Shared helpers may only be
deleted after a separate decision to make the faithful mode use preconverted assets too; that is not
required by this plan.

## Phase 0: establish the baseline and coverage

### 0.0 Wire the Enhanced Graphics selection contract

Before building the alternate renderer, define one startup-time `enhancedGraphics` selection used by
both the existing logo override and the new cockpit path:

- WHDLoad `Custom4=Enhanced Graphics` installs/enables both features.
- The ordinary WHDLoad setting with Custom4 off selects both the original logo and faithful cockpit.
- A standalone enhanced build enables both through the same build option/configuration word.
- The selection is read once during initialization and remains immutable for the run.
- Copper-list creation, bitmap allocation, sprite allocation, and render dispatch all use the same
  snapped value; no subsystem may independently infer Enhanced Graphics from the presence of an
  asset or hook.

Add a diagnostic value to the baseline capture so every screenshot/probe states which path was
selected.

### 0.1 Capture reference states

Capture Amiga screenshots and raw bitmap/plane dumps for at least:

- Initial valid Standby after the cockpit is revealed.
- Doors opening at several positions.
- Tunnel before, during, and after the corner-band reveal.
- Planet entry, including the first frame and a second launch.
- Normal Flight.
- Full, middle, low, and empty energy.
- Several independent terrain-height and ship-height altimeter combinations.
- Artificial horizon at high, neutral, and low pitch.
- Scope object present and absent.
- Scanner dot present, absent, and blinking.
- Lock-on inactive, filling, lit, and blinking.
- Shields, Mother Ship, and Air Lock lights in both states.
- Every decimal digit in every dashboard digit position.
- Score/message text at minimum and maximum used lengths.
- Pause colour cycling.
- Death fade and post-fade blanking.
- Return to mother ship followed by relaunch.

Preserve enough state with each capture to identify the active scene, relevant Atari memory values,
palette values, and Copper list.

### 0.2 Inventory every writer

Build a table of every address and routine that changes a cockpit-visible cell. At minimum it must
include:

- score/message screen data;
- compass cells;
- all five 2x2 dashboard digit blocks;
- `$33DF/$33E0` scanner blink cells;
- `$3491-$3497` lock-on cells;
- thrust and dangerous-altitude dial cells;
- Shields, Mother Ship, and Air Lock status cells;
- initialization-only writes currently covered by `decodeCockpitFull()`;
- any scene-transition writer affecting the eight-line band.

The phase cannot complete while a visible writer is merely assumed static. Exercise event-driven
items such as kills, rescues, quota changes, mother-ship proximity, and airlock state.

### 0.3 Add deterministic comparison tools

Create a host-side proof that can render both representations:

1. Run the existing decoder into a reference bitmap.
2. Construct the same state from the base planar asset plus planar patches.
3. Compare every byte of every active plane.
4. Report the exact region, plane, row, and byte of the first mismatch.

Also add an indexed-pixel comparison that applies the intended palette mapping. This is necessary
because phase 3 deliberately changes bitplane encoding while preserving the visible result.

### Phase 0 acceptance gate

- Custom4 off selects the original logo and faithful cockpit path.
- Custom4 on selects the improved logo and the alternate cockpit path, which is still visually
  faithful at this phase.
- Every dynamic region has a known writer and test state.
- Reference captures cover every scene and instrument category.
- The comparison tool detects deliberate one-bit differences.
- No production rendering code has changed.

## Phase 1: native planar sources at the current depths

This phase changes only where pixels come from. The visible plane counts, palette indices,
dual-playfield mode, sprite use, and Copper composition remain unchanged.

### 1.1 Introduce the separated surfaces

- Allocate the top at two planes.
- Allocate the band at three planes.
- Allocate the dashboard at three planes.
- Update each Copper list to point at the separate surfaces while retaining its current mode and
  palette behavior.
- Preserve the different band sources and color inheritance rules used by Standby, Doors, Tunnel,
  Planet, and Flight.

This step should initially copy the output of the existing decoder into the new surfaces. Its only
purpose is to prove geometry, pointers, modulos, and scene transitions before changing assets.

### 1.2 Replace the static dashboard build

- Load/copy the native three-plane dashboard base asset at the Standby build edge.
- Apply the current state of every dynamic dashboard element as a planar patch.
- Remove the need for a full tile decode during initialization.
- Preserve the persistent dashboard across Standby, Doors, Tunnel, Planet, and Flight.

### 1.3 Replace dynamic dashboard conversion

Convert one independently testable instrument group at a time:

1. Status lights.
2. Lock-on indicator.
3. Scanner blink cells.
4. Decimal digit blocks.
5. Thrust and dangerous-altitude dial cells.

For each group:

- Keep its existing dirty notification.
- Replace `decodeCockpitSpan()` with a direct copy from its planar patch table.
- Run the exhaustive host comparison for every possible state.
- Verify the real event on the Amiga before moving to the next group.

The patch layout should be byte-aligned and interleaved in the same order as the destination so
the copy loop only advances source and destination pointers.

### 1.4 Replace top conversion

- Initialize the static top background from native two-plane data.
- Replace runtime score/message glyph expansion with preconverted two-plane glyph patches.
- Replace compass tile decoding with preconverted compass patches.
- Preserve all existing text contents, character spacing, truncation/clearing behavior, and
  palette selection.

Only the cockpit-specific top path changes. Shared full-screen title/results/high-score text remains
on the existing font renderer.

### 1.5 Replace band conversion

- Standby/Doors/Tunnel must obtain the launch band from native planar data rather than converting
  `$350D` bytes while rendering.
- Preserve the Tunnel's moving color/reveal behavior.
- Planet must preserve the `$1810` handoff and the requirement that the first visible band frame
  is valid even on a slow 68000 and on a second launch.
- Flight must continue to combine live terrain and the windscreen frame correctly. Any live band
  generation should write native planes directly rather than pass through Atari packed pixels.

The band is only eight lines, but it is timing-sensitive and differs by scene. Treat each scene as
an explicit test case rather than hiding the differences behind one generic conversion.

### 1.6 Remove the decoder from the enhanced path

Once all enhanced-path callers have migrated, remove from that path:

- calls to `decodeCockpitSpan()` and `decodeCockpitFull()`;
- the cockpit 2-bpp decode LUTs;
- `cockpitForceFull` as a decoding mechanism;
- decoder verification buffers and timing counters;
- obsolete comments and probes tied to character-to-bitplane conversion.

Retain dirty notifications that still trigger planar patch copies. Keep the decoder implementation
linked for Enhanced Graphics off unless the faithful path is later changed through a separately
approved migration.

The artificial-horizon static detail currently copied out of cockpit BPL3 must be sourced from a
preconverted asset or, temporarily, copied from the new planar dashboard. Prefer generating that
sprite detail plane offline so it no longer depends on dashboard layout.

### Phase 1 acceptance gate

- All active planes compare byte-for-byte with the old renderer for every captured state.
- With Enhanced Graphics on, there is no runtime Atari character/tile-to-bitplane conversion in a
  cockpit-bearing scene.
- With Enhanced Graphics off, the original rendering path and appearance remain available.
- Initial Standby no longer incurs the full cockpit decode pause.
- Every dynamic instrument updates at the correct cadence, including the 50 Hz scanner blink.
- Performance is unchanged or improved on the same A500 workload.

## Phase 2: enable four planes with plane 4 unused

This phase expands all three cockpit surfaces without changing their visible encoding.

### 2.1 Expand the assets and surfaces

- Expand the top from two to four interleaved planes.
- Expand the band from three to four interleaved planes.
- Expand the dashboard from three to four interleaved planes.
- Add zero plane data to every base image and patch.
- Add build-time assertions that all newly introduced plane bits are zero.

For the top, both planes 3 and 4 begin zero. For the band and dashboard, plane 4 begins zero.

### 2.2 Update renderers and patch copies

- Change all top and dashboard row-stride calculations to the four-plane stride.
- Make all patch-copy helpers aware of four destination planes.
- Ensure clearing or shortening a message clears all four planes in the affected rectangle.
- Audit code with hard-coded row sizes such as 80 or 120 bytes and replace cockpit-specific values
  with surface-derived values or named constants.

### 2.3 Update every cockpit Copper list

For the Enhanced Graphics layouts of Standby, Doors, Tunnel, Planet, and Flight:

- Enter four-plane mode at the top of the display.
- Switch to three-plane mode at the viewport boundary.
- Switch back to four-plane mode at the windscreen-band boundary.
- Remain four-plane through the dashboard.
- Set correct pointers and 120-byte interleaved modulos for four-plane surfaces.
- Restore all region palettes explicitly where the plane-count transition makes inherited state
  unsafe, while preserving intentional Atari DLI inheritance.

### 2.4 Protect the Flight line-179 handoff

The Flight viewport-to-cockpit transition is already tightly timed. Adding BPL4 pointer MOVEs to
the existing line-179 group may push BPL1-BPL3 pointer writes beyond line 180's fetch deadline.

Use one of these approaches, in preference order:

1. Preload the inactive BPL4 pointer earlier in the frame, before the band enables four planes.
2. Move other constant pointer/color work earlier without changing visible timing.
3. Only if proven necessary, redesign the handoff with an earlier safe WAIT.

Do not simply append two pointer MOVEs to the existing group. Verify pointer completion and the
first fetched line on an A500-class configuration.

The same principle applies at the top and viewport boundaries: a pointer for an inactive plane may
be prepared early, but BPL1-BPL3 pointers that are actively used by the viewport must not be changed
until their fetches are complete.

### 2.5 Verify DMA impact

Measure the same fixed workload before and after enabling the fourth planes. The comparison must use
the same build flags, trajectory, emulator configuration, and measurement window.

Record:

- frame-rate A/B;
- relevant beam/timing counters;
- chip-memory delta;
- Copper-list word counts and latest write positions;
- any change in missed or late rendering work.

### Phase 2 acceptance gate

- The added planes are zero in every captured state.
- Screenshots and indexed-pixel comparisons are identical to phase 1.
- The first line of every region is stable with no horizontal corruption.
- Planet entry and relaunch band seeding remain correct.
- There is no measurable performance regression.
- Memory growth matches the documented budget closely enough to explain any difference.

If the performance gate fails, do not proceed on the assumption that phase 3 will recover it.
Identify the DMA or CPU cause first and revise the representation or Copper schedule.

## Phase 3: normal four-plane dashboard and Copper-controlled gauges

This phase changes the dashboard's bit encoding and sprite composition. The top and band remain
normal four-plane surfaces with their phase-2 appearance until enhanced artwork is introduced.

### 3.1 Define the dashboard pen contract

Re-encode the dual-playfield semantics as normal four-plane pen indices. A recommended initial
layout is:

| Pen | Faithful meaning |
|---:|---|
| 0 | Dark-grey background and transparent opening for sprites |
| 1 | Region background: divider black, dashboard blue, or floor black |
| 2 | Light-grey bezel/stencil detail |
| 3 | Bright salmon detail |
| 4 | Dark red/bit-7 detail |
| 8 | Energy gauge mask |
| 9 | Altimeter terrain-height mask |
| 10 | Altimeter ship-height mask |
| 5-7, 11-15 | Reserved for enhanced cockpit artwork |

Using pens 8-10 for the gauges exercises plane 4 immediately and ensures the new path is tested
before enhanced graphics depend on it.

Pen 0 is deliberately special. With the remaining dashboard sprites behind the playfield, a zero
pixel lets a sprite show through; where no sprite is present it displays COLOR00, the cockpit's
dark-grey background. Every static bezel/detail pixel that must cover a sprite uses a nonzero pen.

### 3.2 Generate newly encoded assets and patches

- Re-encode the dashboard base image for the normal four-plane pen contract.
- Re-encode all dynamic dashboard patches to the same contract.
- Preserve the exact visible color selected for every old dual-playfield combination.
- Compare final indexed pixels, not raw plane bytes, because the representation intentionally
  changes here.

### 3.3 Remove dual-playfield mode

At the dashboard boundary:

- Clear `DBLPF` in `BPLCON0`.
- Replace the dual-playfield priority value with a single-playfield `BPLCON2` value that places
  the remaining cockpit sprites behind nonzero playfield pixels.
- Keep those sprites visible through pen-0 instrument openings.

Verify the priority with an isolated on-target test. In particular:

- Artificial-horizon fill appears inside its window but not over its markings or bezel.
- Targeting-scope imagery remains clipped to its intended opening.
- Scanner dots do not leak through surrounding artwork.
- Viewport sprite priority is restored independently above the cockpit.

Once the playfield supplies the foreground markings, remove the artificial-horizon sprite's baked
detail plane and `initializeAHDetailPlanes()`.

### 3.4 Add gauge masks to the dashboard artwork

Draw three full-height mask columns into the dashboard asset:

- Energy level.
- Altimeter terrain height.
- Altimeter ship height.

Each mask uses its dedicated pen and exists only inside its 56-line dial opening. There must be no
mask pixels below the dial. Consequently, the current fixed Copper blank at the dial bottom is no
longer needed to hide a solid sprite's overhang.

### 3.5 Convert gauge values to Copper transitions

At the dashboard entry, initialize all three gauge pens to the inactive dial-background colour.
For each gauge, convert its current value to the first visible fill scanline. At that scanline the
Copper changes the gauge's color register to its active colour. Pixels above the transition remain
indistinguishable from the empty dial; pixels at and below it display the filled bar.

The transition calculation must reproduce the Atari strip's exact boundary, including:

- full value;
- empty value;
- the one-line inclusive/exclusive behavior;
- parking/sentinel states used during transitions;
- palette changes during pause and death.

The three transition lines are independent and may occur in any vertical order. Copper records
must therefore be emitted in increasing raster order. Use either:

- a three-item fixed sorting network; or
- six prearranged event orderings selected by comparisons.

Avoid a general sorter or allocation. When two transitions share a line, either combine their
MOVEs under one WAIT or emit repeated legal WAITs for that line. All valid gauge transitions occur
before the Copper vertical-counter wrap at line 255.

Only rebuild these Copper records when a height or relevant active/inactive colour changes.

### 3.6 Remove gauge sprites

After the pen-based gauges are proven, remove:

- energy sprite allocation and solid-data construction;
- altimeter terrain sprite allocation and construction;
- altimeter ship sprite allocation and construction;
- their `setX()`/`setY()` update paths;
- sprite mirroring used by the Flight wide-object chains;
- gauge sprite pointer updates and dashboard repoints;
- gauge sprite color registers and setters;
- fixed bottom-of-dial sprite color blanking;
- gauge-specific sprite probes and debug globals.

Update the Flight sprite chains:

- Channel 5's viewport wide-object extension terminates instead of chaining into energy.
- Channel 6's viewport wide-object extension terminates instead of chaining into altimeter terrain.
- Channel 7 no longer receives a dashboard altimeter-ship repoint.

Update the other cockpit scenes so channel 2 is no longer reserved/repointed for the energy bar.
Do not immediately reuse the freed channels; first prove the simplified configuration.

### 3.7 Preserve live palette behavior

The new playfield pens must continue to follow the original display parameters:

- Energy active color follows `$00DE`.
- Altimeter terrain follows `$00D5`.
- Altimeter ship follows `$00D6`.
- Inactive gauge colors follow the dashboard background applicable above their transitions.
- Pause cycling and death fades update the same subsets and ranges as the current faithful path.
- A DLI/Copper region that intentionally leaves a register untouched must continue to inherit it.

### Phase 3 acceptance gate

- With Enhanced Graphics on, dual-playfield mode is absent from the dashboard in all cockpit
  Copper-list layouts.
- With Enhanced Graphics off, the existing dual-playfield and gauge-sprite layout is unchanged.
- Energy and both altimeter components use playfield pens, not sprites.
- Every reference gauge height matches the current version scanline-for-scanline.
- Remaining dashboard sprites are clipped correctly by the normal playfield.
- Pause, death, return, and relaunch palette behavior remains faithful.
- No sprite corruption results from shortening the Flight chains.
- Performance is unchanged or improved relative to phase 2 and relative to the original baseline.

## Phase 4: cleanup and enhanced-art handoff

Do this only after the three fidelity phases are accepted.

- Replace address-oriented rendering names with instrument-oriented names where it improves the
  asset interface.
- Keep Atari memory writes intact but stop treating Atari screen RAM as the authoritative Amiga
  pixel source.
- Remove obsolete dirty flags if their semantic writers can call patch updates directly without
  changing behavior or interrupt safety.
- Document the top, band, and dashboard pen assignments and reserved pens.
- Document which pixels must remain pen 0 to expose sprites.
- Provide indexed preview/export tooling suitable for editing replacement artwork.
- Make base images and instrument-state atlases the only artwork inputs an artist needs to touch.
- Update `docs/fonts.md`, `docs/instruments.md`, `docs/scene-composition.md`, and the historical
  cockpit-render notes to describe the new implementation.

Enhanced artwork should begin as a separate change after this cleanup. That keeps any intended
visual differences out of the fidelity migration's regression history.

## Scene-by-scene checklist

### Standby

- Enhanced Graphics off continues to build and select the current Standby Copper layout.
- Enhanced Graphics on begins in four-plane mode for the top, including score/message and compass,
  switches to the existing three-plane viewport at line 86, and returns to four planes at the
  windscreen band at line 172.
- The top-to-viewport transition must restore the current terrain pointers, 80-byte three-plane
  modulo, and the four terrain pens before the first viewport fetch.
- `StandbyCopperList::setTerrainRuns()` currently emits a variable number of three-plane terrain
  runs and then appends the cockpit region. Its enhanced layout must reserve enough Copper words
  for the larger band/dashboard handoff without changing the variable-run geometry.
- The four-plane band remains derived from the faithful `$350D` image. Preserve COLOR00's green
  inheritance from the final door-field run; do not replace it with a convenient constant.
- At line 180, switch pointers and modulo from the four-plane band to the four-plane dashboard.
  The plane count can remain four, but the dashboard palette/background split must still occur at
  the same raster positions as today.
- Preserve the line-182 divider-to-blue change and line-252 dashboard-to-floor change.
- Until phase 3, retain the energy sprite and its line-244 pen blank in the enhanced layout. In
  phase 3 only the enhanced layout replaces it with the energy mask pen; the faithful layout keeps
  channel 2 unchanged.
- Preserve `rsEnergyIndicator` visibility semantics during the static Standby and launch setup.
- The new top/band/dashboard surfaces must be complete before the black-until-ready gate publishes
  the enhanced Standby list. No partially copied planar artwork may become visible.
- Verify initial boot Standby, post-title-card Standby, post-mother-ship Standby, and relaunch.

### Doors

- Enhanced Graphics off continues using the current Doors list and sprite assignments.
- The enhanced list uses a four-plane top, the existing three-plane moving door/tunnel bands, and
  a four-plane windscreen band/dashboard.
- Preserve all three moving viewport-band pointer sets and their geometry. The fourth cockpit plane
  must not become active inside a door or tunnel reveal band.
- Preserve the green COLOR00 background continuously through the viewport and windscreen band.
  The launch DLI does not introduce an independent band background here, so the enhanced Copper
  list must retain that inheritance.
- At the viewport-to-band transition, establish the four-plane band pointers and 120-byte modulo
  without disturbing the final moving door fetch.
- Retain the same dashboard palette split and floor timing as Standby.
- Until phase 3, retain the energy sprite and its clipping color change. After phase 3, remove it
  only from the enhanced layout and leave the faithful layout's sprite channel map intact.
- Test the first and last frame of the Doors scene as well as intermediate openings. Boundary bugs
  can otherwise be hidden by the large moving field.
- Test the reverse/post-flight path if Doors is shown during return, not only forward launch.

### Tunnel

- Enhanced Graphics off continues using the current Tunnel list.
- The enhanced list uses a four-plane top, three-plane tunnel viewport, and four-plane band and
  dashboard. Tunnel rings and their cycling colors remain entirely in the three-plane viewport.
- Preserve the green-to-purple receding windscreen-corner behavior driven by the band reveal. The
  new fourth plane begins zero and must not alter any of the existing value-0/1/2 band pixels.
- Preserve the fact that the band background/color can carry forward from the final tunnel palette
  rather than being reloaded as an unrelated cockpit constant.
- `setBandTopColor00()` is timed immediately after the band WAIT for the boost expansion path. Its
  enhanced-layout MOVE must remain at the same effective raster position; extra BPL4 setup must be
  scheduled around it rather than pushing it late.
- Preserve forward launch, reverse tunnel, boost expansion, and direct boost-to-flight handoffs.
- Until phase 3, retain the energy sprite and line-244 clipping in the enhanced layout. Replace it
  with the energy pen only after the normal-playfield dashboard is proven.
- Verify the final tunnel frame and first Planet/Flight frame together, because a correct isolated
  Tunnel frame can still hand stale pointers or palette state to the next Copper list.

### Stars/Planet

- Stars and Planet share `PlanetCopperList`; both toggle positions must preserve that shared scene
  routing rather than creating a second scene identity.
- Enhanced Graphics off keeps the existing two/three/three-plane layout, line-doubled viewport,
  star sprites, and channel-2 dashboard gauge repoint.
- Enhanced Graphics on uses a four-plane top, the existing line-doubled three-plane viewport, and
  a four-plane band/dashboard. Star and planet rendering algorithms remain unchanged.
- The band is currently displayed from viewport rows 43-46 (`$1810`) while the dashboard skips the
  first eight rows of the shared cockpit bitmap. The enhanced layout must instead publish a valid
  independent four-plane band. Its first three planes must reproduce those viewport rows exactly;
  plane 4 remains zero until enhanced band artwork is authored.
- Preserve the `$6D67` palette rule: the band explicitly changes only the two frame colors and
  inherits the other relevant colors from the planet viewport. Do not normalize it to the launch
  band palette.
- Seed all four band-plane pointers/data before selecting the Planet Copper list. The existing
  memoized viewport decoder can skip an apparently unchanged band after its destination was
  cleared, so the enhanced path must invalidate that state or use an unconditional band publish.
- Verify both first launch and relaunch after mother-ship return. The second entry is the important
  stale-data case.
- The low P0 star sprite currently owns channel 2 in the viewport and relies on a dashboard repoint
  to arm the energy gauge. In phase 3, the enhanced path removes the gauge repoint, but the outgoing
  star sprite must still end/terminate correctly at the boundary. Do not change the faithful chain.
- Verify the transition from Tunnel into Stars/Planet and from Planet into Flight, including the
  first visible band scanline and inherited colors.
- Verify the reverse Planet/Stars presentation during return to the mother ship.

### Flight

- Preserve the three-plane terrain renderer and its line-doubling/enhanced-renderer choices.
- Preserve the live terrain beneath the four-plane windscreen frame band.
- Keep the line-179 handoff within the measured fetch deadline.
- Preserve A-pillars, corner triangles, crosshair, shot, AH, scope, viewport-P3, and scanner sprites.
- Shorten channels 5 and 6's chains and remove channel 7's altimeter repoint only in phase 3.
- Verify rescue pauses and early-return render paths where sprite updates currently require a
  safety flush.
- Enhanced Graphics off retains the complete current Flight Copper list, dual-playfield dashboard,
  and sprite chains as the faithful comparison path.

## Performance verification strategy

Performance must be checked at each commit boundary rather than only at the end.

### CPU-side expectations

Expected savings include:

- removal of the one-time full cockpit decode;
- removal of runtime glyph/tile bit interpretation;
- smaller direct patch copies;
- removal of dirty-cell scans once semantic updates replace them;
- removal of energy/altimeter solid-sprite position and mirror work;
- fewer dashboard sprite pointer and color updates.

### DMA-side cost

The new fourth planes increase bitplane DMA over approximately 130 non-terrain scanlines. Removing
three gauge sprites recovers some sprite DMA, but the balance cannot be established accurately from
byte counts alone because CPU contention depends on when chip-bus accesses occur.

### Measurement rule

- Use identical clean builds and fixed flags.
- Use the same deterministic flight trajectory and measurement window.
- Compare phase N directly with phase N-1 in the same session.
- Treat differences below the established measurement noise honestly.
- Inspect beam deadlines in addition to aggregate frame rate.
- Do not accept a measurable regression on the theory that a later phase may compensate for it.

## Suggested implementation/commit sequence

Keep every step independently buildable and reviewable:

1. Add the loader-patchable Enhanced Graphics selection and make Custom4/standalone builds drive
   both the logo and cockpit choice, without changing rendering yet.
2. Add baseline capture data and the old-vs-new planar comparison tool.
3. Add the offline cockpit planar asset generator and previews.
4. Add enhanced-only top, band, and dashboard ownership without changing their data source.
5. Load the native three-plane dashboard base in the enhanced path.
6. Convert enhanced-path dashboard dynamic groups to planar patches one group at a time.
7. Convert enhanced-path cockpit-specific top text and compass to planar patches.
8. Convert enhanced-path band sources to native planar data/direct planar generation.
9. Remove runtime cockpit decoder calls and their probes from the enhanced path.
10. Expand the enhanced top, band, dashboard, and patches to four planes with new planes zero.
11. Update the enhanced layouts of all Copper lists to `4 -> 3 -> 4`; verify every scene boundary,
    timing, and performance.
12. Re-encode the enhanced dashboard for a normal four-plane pen contract.
13. Put remaining enhanced-dashboard sprites behind the normal playfield and remove enhanced-path
    AH baked detail.
14. Add the three gauge masks and Copper transition scheduler to the enhanced path.
15. Remove the gauge sprites, chains, repoints, colors, and clipping workarounds from the enhanced
    path only.
16. Run the complete toggle-off/toggle-on fidelity and performance matrix and update documentation.
17. Begin enhanced cockpit artwork only in a subsequent series of changes, still selected by the
    same Enhanced Graphics option.

## Principal risks and mitigations

| Risk | Mitigation |
|---|---|
| A dynamic Atari screen writer is missed | Complete the writer inventory and exercise every event before removing the decoder |
| Flight's line-179 pointer group becomes too late | Preload inactive BPL4 pointers and measure the first dashboard fetch on target |
| A four-plane interleaved stride is used while displaying three planes | Keep top, band, dashboard, and viewport as separate surfaces with explicit modulos |
| Planet's band is blank on entry or relaunch | Seed it before Copper publication and invalidate/avoid stale memoized source state |
| Gauge Copper WAITs are emitted out of order | Use a fixed three-event ordering implementation and exhaustive value tests |
| Gauge boundary is one scanline wrong | Compare full/mid/empty masks against the original 56-row Atari strips |
| Remaining sprites leak over cockpit artwork | Reserve pen 0 for openings and verify the single-playfield `BPLCON2` setting on target |
| Pause/death colors stop following Atari parameters | Preserve the existing live parameter sources and their intentionally different ranges |
| Fourth-plane DMA reduces CPU time | Limit it to non-terrain regions and enforce the per-phase performance gate |
| Initializers placed in the project's BSS-only `.MEMF_CHIP` section are discarded | Put hardware-ready assets in a distinct initialized CHIP data hunk such as `.INCBIN.MEMF_CHIP`; allocate/copy only when mutability or conditional residency requires it |
| New asset pipeline becomes another opaque format | Generate indexed previews, document pen semantics, and keep generation deterministic |

## Definition of done

The migration is complete when:

- The existing Enhanced Graphics/Custom4 setting selects both the improved logo and enhanced
  cockpit; there is no additional cockpit option.
- With Enhanced Graphics on, every non-terrain cockpit region is displayed with four normal Amiga
  bitplanes where intended.
- With Enhanced Graphics on, cockpit source artwork and all dynamic cockpit patches are stored in
  native planar form and no cockpit character/tile-to-bitplane conversion runs at runtime.
- With Enhanced Graphics on, the dashboard no longer uses dual-playfield mode; energy, altimeter
  terrain, and altimeter ship are Copper-height-controlled playfield pens; and their three sprite
  paths and associated chains/repoints are absent.
- With Enhanced Graphics off, the original faithful cockpit implementation and appearance remain
  available and unaffected.
- In both modes, the terrain viewport remains on its existing three-plane renderer.
- All other cockpit and gameplay sprites retain correct priority and clipping.
- Before enhanced art is introduced, every toggle-on fidelity capture matches toggle-off and the
  pre-migration appearance.
- Toggle-off performance matches the original baseline, and toggle-on performance is unchanged or
  improved on the target A500 configuration.
- The memory increase is measured and documented.
- The asset interface is ready for separately authored 16-color enhanced cockpit graphics.
