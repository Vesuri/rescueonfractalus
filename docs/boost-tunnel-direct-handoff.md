# Boost reverse tunnel — direct bitplane painting

**Goal (user directive, 2026-08-10):** the reverse tunnel was painted by the 6502 into a GTIA field
in `mem[]`, which the Amiga then decoded to bitplanes. That model was wrong — we know exactly what
the ring code draws, so **draw it straight into the bitmap**. No field decode, and no content
shadow: the shadow (236592c, 847 → 190 ticks/frame) was a *memoized decode*, and the point was to
delete the decode, not to memoize it faster.

**STATUS: landed 2026-08-10 (fd28b05), user-confirmed on screen.** The reverse cinematic no longer
decodes its ring field at all. Boost decode cost went from ~65900 beam ticks per cinematic (324 full
ring decodes + 2 star decodes) to **1621** — the two starfield decodes, nothing else. The visual
check also covers the one change the §3 differential cannot see: the `!rsBoostViewport` guard on
`render()`'s terrain-view branch (without it the brief in-boost `rsStars` window decodes the $1000
RING field as a mode-D field straight over the starfield).

---

## 1. What the ring field's writers actually are (measured, not inferred)

The 2026-08-10 session before this one bucketed the painter's rectangles by render phase and then
*labelled* the largest bucket "the forward launch's tunnel descent". That label was never measured
and it was wrong. Tagging each rectangle with its **call site** (`g_trSrc`, set at each
`draw_symmetric_span_loop` call site) and crossing it with the phase gives, over one `FORCE_RETURN`
boost:

| run | call site | vbi | rects | phase | $8D/$8E |
|---|---|---|---|---|---|
| 0 | pre-draw @ `tunnel_prebuild_rings` | 75–86 | 43 | none | 00/00 |
| 1 | `draw_ring_frame_step` (forward descent) | 831–848 | 15 | none | 00/00 |
| 2 | pre-draw @ `L_6047` | 2184–2195 | 43 | boostVp | **00/00 = the STARS sub-phase** |
| 3 | `step_accum_sub_7e` (reverse rings) | 2569–2661 | 43 | boostVp | 13/6c |
| 4 | pre-draw @ `tunnel_prebuild_rings` | 2678–2689 | 43 | boostRet | 00/7f |

187 rectangles total, reproducing the old 86 "armed" / 101 "not armed" split exactly. So the 101
were runs 0+1+4 — **all forward-tunnel work** (boot pre-build, forward descent, and the *next*
launch's pre-build). The boost's own content is runs 2+3 and was already reaching the painter.
`draw_frame_pattern_seq` is exactly 43 rectangles (the sum of the `$6E0F` thickness table), which
is what makes these counts self-checking.

**The consequence that mattered:** run 2 paints the static ring image into `tunnelBitmap` *while
that bitmap is on screen showing the starfield*, and the stars decode is dirty-gated, so nothing
overwrote it — black/white rectangles over the faded stars for ~385 vbi ≈ 7.7 s (user-observed).
One bitmap cannot hold both the stars and the rings while the boundary between them moves. That is
why the copper band split below is a *prerequisite* for the painter, not an optional follow-up.

### The three field writers

A painter that only hooks `draw_symmetric_span_loop` is wrong in both directions. All three of
these write `$1000`, and the differential in §3 found the two that were missing:

1. **`draw_symmetric_span_loop`** — the concentric rectangles (`ROF_TUNNEL_RECT`).
2. **`draw_frame_guide_columns`** — three FULL-HEIGHT columns via `plot_pixel_masked_core`, the
   tail of every `draw_frame_pattern_seq`. They sit at the vanishing point (`ROF_TUNNEL_COLS`).
3. **`plot_terrain_span`** → `fill_vertical_span` — vertical span pairs (`ROF_TUNNEL_VSPAN`). This
   one runs **twice** in the cinematic and both matter: at `L_6047`+5 with **colour 8**, which
   *erases* the static pre-draw back to background, and then once per revealed row, because
   `emit_dl_coord_pairs` **tail-calls** `plot_terrain_span` with the cycling ring colour. Missing
   it left the erased pre-draw in the bitmap as 6.1% stale ink.
   `draw_symmetric_span_loop` calls `fill_vertical_span_`**`core`** directly, so hooking the shim
   is exactly `plot_terrain_span`'s writes with no double-paint.

**The fact the whole design rests on:** the 6502's nibble masks are prev-independent — tables
`$66E9` (OR) / `$66FB` (AND), 18 bytes each = 9 colours × 2 nibble positions, i.e. "set this nibble
to colour, preserve the other". A direct painter never needs the field's old contents.

---

## 2. The design as built

**Two bitmaps, and the copper picks per band.** `TunnelCopperList::setRevealBands(K, ringBase,
starBase)` splits the terrain region: rings from `tunnelBitmap` in `[K, 85-K]`, the starfield from
`viewportBitmap` outside it. `K` = the first revealed row (43 = nothing, 0 = rings fill it), read
from the live `$3000` DL LMS words by `boostRevealK()`. Geometry mirrors `DoorsCopperList`'s
sliding gap with **g2 = 43 − K**, so both cinematics share one proven band layout. All three bands
inherit the region-top BPLCON0/modulo/palette, so bands 1 and 2 need only a WAIT + 6 pointer moves;
they park as copper NOPs otherwise, and the **forward** descent asks for `setRevealBands(0, …)` =
one full-height ring band, which is its old layout exactly.

- ⚠ **`boostRevealK()` must be derived from the FORWARD strand (rows 0..42) alone and mirrored.**
  Reading the converted rows directly gives an asymmetric set (fwd rows 42→0 but rev rows 57→85)
  and renders as a bowtie.
- ⚠ **`tunnelCopper` is now double-buffered** (`tunnelCopper[2]` + `tunnelActive`, swapped by
  `showTunnelCopper()`). The reveal moves band WAIT lines *and bitplane pointers*, and a pointer
  poked on the live list can be read half-written — a torn pointer garbages the whole viewport.
  Because every slot is rewritten into the back buffer, the old `tn*` poke-on-change cache is gone;
  it would have been comparing against the wrong buffer. The T6 handoff hold still pokes the live
  list, but only **colours**, which are safe mid-frame.
- **Priming: to the field's BACKGROUND pen, not pen 0.** The painter writes only rectangles; the
  decode it replaced also wrote everything *between* them, which is the untouched field value `$88`
  = GTIA value 8. Priming to 0 left 6.1% of the revealed band wrong and read on screen as teal
  "gates opening" instead of a rectangle growing from the centre. Derive it from the LUT
  (`boostPen`/`kGtia10Boost*`) so it follows the boost's value-8 remap.
- **Arming.** `boostRingsArmed` is set at the boost copper install and cleared the moment
  `rsBoostViewport` goes false. It must not survive: run 4 above is the *next* launch's forward
  pre-build, 17 frames later, and it would land in the bitmap the T6 hold is still displaying.
- **Stars → `viewportBitmap`**, which is genuinely free during the boost (`renderViewportModeD` is
  skipped by `boostOwnsTunnel`, and the Standby door field decodes into `doorScrollBitmap`).
  ⚠ `perFrameWork`'s `rsStars && viewportForceFull` blitter clear needs its `!rsBoostViewport`
  guard or it blanks the starfield mid-cinematic; and `decodeBoostStars` stamps
  `viewportLastBase = $2000` so the next stars/planet decode full-clears it away.

### Geometry

- Field pixel column p → Amiga `x = (p>>1 − 4) * 8 + (p&1)*4` (displayed byte = `(p>>1) − 4`,
  8 Amiga px per byte). Field row → bitmap row 1:1. Everything is 4 px granular.
- ⚠ **FAITHFUL QUIRK, do not "fix":** `fill_vertical_span_core` picks its nibble mask from **xL's
  parity** and applies that same mask at BOTH columns. `paintVSpan` mirrors it. The guide columns
  do *not* share it — `plot_pixel_masked_core` is per-pixel, so each column takes its own parity.
- ⚠ **Row names are inverted vs the screen:** `draw_row_bottom` (`$009F`) is the SMALL row index
  (screen top); `draw_row_top` (`$009E`) the large one.
- Every hook skips `rowBase >= 0x2000` — the doors share these routines but keep their own decode.
- The painters run in the **50 Hz VBI ISR**, so `Bitmap::fillColor` is CPU-side: starting blits
  there would race the main loop's queued ones.

---

## 3. The harness that proves it (`amiga/boost_probe.gdb`)

An in-process differential: decode the live `$1000` field through the boost LUT and diff it against
what the painter actually left in `tunnelBitmap`, over the revealed band `[K, 85-K]`. Race-aware by
construction — the VBI writes the field while this reads it, so a small transient count is expected.

- **EXTRA** = the bitmap has ink where the field is pure background (`$88`) → the painter drew
  something the field does not have, or stale ink was never cleared.
- **MISSING** = the field has content the painter never saw → an unhooked field writer.

```
cd amiga && . ./env.sh
make clean && make -j4 PROBES=1 FORCE_RETURN=1
GDBSCRIPT=boost_probe.gdb ./diag_run.sh 130
```

Result at landing: **64 frames, 163760 bytes compared, 1 mismatching byte** (EXTRA 0, MISSING 1 —
the multi-writer race). Before the `ROF_TUNNEL_VSPAN` hook it was 10156 (EXTRA 9708). This harness
caught every wrong turn in this work *before* it was visible on screen; use it before looking at
the screen, not after.

`make validate` green on all eight touched twins (`fill_vertical_span` 8000 cases,
`draw_symmetric_span_loop` 4000, `draw_frame_pattern_seq` 3000, `draw_frame_guide_columns` 8000,
`draw_ring_frame_step` 4000, `step_accum_sub_7e` 4000, `plot_terrain_span` 4000,
`emit_dl_coord_pairs` 8000 — 0 mem mismatch each).

---

## 4. Traps that cost real time here

- ⛔ **Never gate the field decode on a dirty flag.** Tried; rendered a **bowtie**. The field is a
  multi-writer reveal with no "content settled" signal, so a skipped frame HOLDS a mid-update
  state. `docs/boost-cinematic-plan.md` item 2 already said so. (`decodeBoostStars`'s
  `g_boostStarsDirty` gate is *not* this: `fill_region_2000` is the sole `$2000` writer, so there
  is no mid-update state to freeze.)
- ⛔ **`make clean` on every flag toggle.** A stale-object link produced a *working-but-wrong*
  binary twice in one session.
- ⚠ **Frame counts are not comparable run-to-run** without `FIXED_RNG` — the level, and therefore
  when the boost starts, varies. One run showed 324 reverse-tunnel frames, the next 64.
- ⚠ **`--gc-sections` drops probe globals nothing writes**, and gdb then prints whatever is at the
  stale address. `g_bTunDec` read `1223110688` this way after its writer was deleted. Delete dead
  counters rather than leaving them "reading 0".
- ⚠ **A probe that reads the same source as the code under test is vacuous.** Do not *label* a
  bucket — tag it with the thing that actually distinguishes it (here, the call site).

---

## 5. NEXT SESSION: the same treatment for the FORWARD tunnel

**Goal:** the forward tunnel's clear/descent should paint straight to the bitplanes too, so no part
of the tunnel round-trips through `mem[]`. Everything needed is already in place — the hooks fire
for the forward path today and are simply gated off by `boostRingsArmed`.

### Two decodes to delete

| decode | source | when |
|---|---|---|
| `platform_tunnel_rings_drawn` → full-field decode (`g_tunBandMode==0`, rows 0-85) | the 43-rectangle pre-draw + 3 guide columns | once, at the standby reveal |
| `decodeTunnelBand()` → 4 thin strips from the `g_tun*` bounds | `draw_ring_frame_step`'s expanding clear | once per descent step |

Both are already covered by hooks that ALREADY FIRE (measured, §1 table): run 0 = `src 4`
(`tunnel_prebuild_rings` → `draw_frame_pattern_seq`, 43 rectangles + `draw_frame_guide_columns`),
run 1 = `src 2` (`draw_ring_frame_step`, 15 rectangles over 14 steps). Deleting the decodes also
deletes `decodeTunnelRect`, `tunnelSrcBase`, `g_tunnelFieldDirty`, the `g_tun*` band-bound publish
in `draw_ring_frame_step`/`step_accum_sub_7e`, and the `boostOwnsTunnel` defer logic.

### The prize beyond the decode itself

`tunnel_prebuild_rings` / `tunnel_prebuild_replay_exit` / `g_tunnelPrebuilt` exist for exactly ONE
reason: the launch-time `draw_frame_pattern_seq` + its full-field decode was the ~140 ms
standby→doors freeze, so the whole thing was moved to standby construction behind the black
curtain and the launch site replays its exit ZP scratch. **A direct painter removes the cost that
hack was dodging**, so the hack itself becomes deletable and the launch site can go back to the
faithful `draw_frame_pattern_seq()` call. That is the real win here — the decode is a cinematic
cost, not a flight-loop one.

### The pen difference, precisely (this is the "unfortunate difference in the pens")

It is only a LUT choice, and both LUTs are built in the same loop in `initialize()`:

| GTIA value | forward `kGtia10P*` | boost `kGtia10BoostP*` |
|---|---|---|
| 0 (exit clear) | pen 7 | pen 7 |
| 2 (outermost ring) | pen 2 | **pen 0** |
| 8 (background / corner) | **pen 0** | **pen 2** |
| others | pen = value | pen = value |

So `boostPen(colour)` becomes `tunnelPen(colour, boost)`, and the §2 prime pen follows the same
rule — derive it from `kGtia10P*[0x88]` for the forward path instead of `kGtia10BoostP*[0x88]`.

### Concrete order of work

1. **Measure first (one probe run, the harness exists).** Build `PROBES=1` *without* `FORCE_RETURN`
   and read the `§0a run timeline`. On the FORCE_RETURN boot the L_6047 pre-draw never ran on the
   forward path — `src 1` totalled exactly 43 rectangles and all 43 were in the boost phase, while
   boot showed only `src 4`. **Confirm that on a plain launch**, because if `L_6047` *does* run
   there it brings its colour-8 `plot_terrain_span` erase and the `emit_dl_coord_pairs` reveal
   spans with it. All three hooks already cover those, so only the arming changes — but know it
   before designing the arming, not after. (The branch that skips it is somewhere in the driver's
   early returns, e.g. the `mem[$00E5]==0 → standby_level_select_loop(); return;` at rof_native.c
   ~11244; that is a code-reading question, the probe is the answer.)
2. **Replace `boostRingsArmed` with an owner** (`none` / `boost` / `forward`). The hard part is
   already solved and must be reused, not re-derived: `boostOwnsTunnel` encodes the two cases that
   look identical from `mem[]` — the T6 handoff EDGE frame vs the next level's standby pre-build.
   `$003A` stays `$FF` into the next level and `g_doorFieldReady` is 1 at both, so **the only
   signal that separates them is which copper is live** (`tunnelCopperInstalled`). See commit
   8339175.
3. **Prime before the pre-draw, not at the copper install.** The forward pre-build runs during
   standby construction behind the black `EmptyCopperList`, long before any tunnel copper exists,
   so the boost's "prime at install" point is too late. Prime at construction entry.
4. **Verify numerically before looking**: point the §2 differential at the forward path — full 86
   rows (no reveal band), `kGtia10P*` — and require EXTRA 0 / MISSING 0 as the boost path now gets.
5. **Then look**, and specifically at the **2nd launch after a boost**: a stale `tunnelBitmap`
   there is the exact trap 8339175 fixed (doors opening onto salmon/cycling with no rings).

### Still not done after that

- Nothing else: the doors keep their own `$2000` decode by design, and every hook already skips
  `rowBase >= 0x2000`.
