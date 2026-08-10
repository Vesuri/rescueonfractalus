# The tunnel — direct bitplane painting (both directions)

**Goal (user directive, 2026-08-10):** the tunnel was painted by the 6502 into a GTIA field
in `mem[]`, which the Amiga then decoded to bitplanes. That model was wrong — we know exactly what
the ring code draws, so **draw it straight into the bitmap**. No field decode, and no content
shadow: the shadow (236592c, 847 → 190 ticks/frame) was a *memoized decode*, and the point was to
delete the decode, not to memoize it faster.  §1-§4 are the reverse (boost) tunnel, which went
first; §5 is the forward launch tunnel, which reused all of it.

**STATUS: reverse tunnel landed 2026-08-10 (fd28b05), user-confirmed on screen; the FORWARD
tunnel followed the same day — see §5.  Neither direction reads `$1000` back any more.** The reverse cinematic no longer
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

## 5. The FORWARD tunnel — DONE 2026-08-10

The forward launch tunnel now paints straight to the bitplanes too, so **no part of either tunnel
round-trips through `mem[]`**.  Both decodes are gone:

| deleted | what fed it | when it ran |
|---|---|---|
| `platform_tunnel_rings_drawn` → full-field decode (rows 0-85, ~847 ticks ≈ 54 ms) | the 43-rectangle pre-draw + 3 guide columns | once, at the standby reveal |
| `decodeTunnelBand()` → 4 thin strips from the `g_tun*` bounds | `draw_ring_frame_step`'s expanding clear | once per descent step |

Gone with them: `decodeTunnelRect`, `decodeTunnelBand`, `tunnelSrcBase`, `g_tunnelFieldDirty`,
`g_tunBandMode`, the four `g_tun*` bound pairs and their publishes in `draw_ring_frame_step` /
`step_accum_sub_7e`, and the whole `boostOwnsTunnel` defer gate.

### What it actually took

1. **An owner, not an armed flag.**  `boostRingsArmed` became `tunnelOwner` ∈ {`None`, `Forward`,
   `Boost`}.  Both directions share every field writer and every bit of geometry; **the entire
   direction split is which LUT `tunnelPen()` reads** (`kGtia10P*` vs `kGtia10BoostP*`), and the
   prime pen falls out of the same call (`tunnelPen(8)`).  Forward value-8 → pen 0, so the forward
   prime is a plain blitter clear.
2. **Claim + prime BEFORE the draw, at `platform_tunnel_rings_begin()`** (the renamed
   `platform_tunnel_rings_drawn`, moved from after the draw to before it).  The boost keeps claiming
   at its copper install; the forward path claims inside `tunnel_prebuild_rings`.
3. **`tunnel_prebuild_rings()` MOVED past `delay_loop_c2_to_c9()`** — see the trap below.
4. Boost release drops to `None`, not `Forward`, so nothing can paint until a pre-draw claims.

### ⛔ The premise this section used to carry was WRONG — the pre-build hack is NOT deletable

The old §5 said `tunnel_prebuild_rings` / `tunnel_prebuild_replay_exit` / `g_tunnelPrebuilt` existed
only to dodge the full-field decode, so a direct painter would make them deletable and the launch
site could go back to the faithful `draw_frame_pattern_seq()` call.  **Measured, and it is not
true.**  Forcing the launch-site draw (`PROBES=1`, pre-build branch disabled):

- `SA_TIMED` bucket `framepat` = **1378 ticks ≈ 86 ms** — that is the 6502's OWN `mem[$1000]` plot,
  which is faithful and stays no matter how the Amiga renders.
- the standby→doors render gap went **1 frame → 5 frames (~100 ms)**.

So the cost the hack dodges is the plot, not the decode.  The hack stays, and the painter's
claim/prime is paired with it.  *Generalisable:* before believing "removing X makes workaround Y
deletable", measure what Y was actually dodging — here the decode was the smaller half.

### ⚠ Traps this cost real time on

- **A paint cannot be deferred the way a decode could.**  Commit 8339175 handled the post-boost
  pre-build by keeping `g_tunnelFieldDirty` set and decoding a frame later, off-screen.  The painter
  writes synchronously, so that escape is gone — and the post-boost pre-build fires ~40 frames after
  the reverse ring, inside the window where the T6 handoff hold is still displaying `tunnelBitmap`.
  Fix: move the call site past `delay_loop_c2_to_c9()` (that loop renders frames, so by then
  `g_doorFieldReady` has flipped and staticStandby owns the display).  Verified, not reasoned — the
  `g_tpb*` claim log records `tunnelCopperInstalled` at every claim and both read 0
  (boot vbi 96, post-boost vbi 2707).
- **⛔⛔ The §2 differential is too slow to leave running, and its failure looks like a game bug.**
  It is ~10k volatile `mem[]` reads per frame, which pushes render past one vblank.  The launch
  cinematic polls counters the VBI decrements with `while (mem[$0684] != $64)` — one frame per poll —
  so a slow render **steps over the target value and spins forever**.  This presented as "the tunnel
  descent never runs" (`draw_ring_frame_step` = 0 rectangles, 659 frames stuck in the tunnel branch),
  and it cost a bisect to find that the probe, not the change, was the bug.  It now lives behind
  **`make TUNDIFF=1`**, off even under `PROBES`, and the live arm is gated to the descent
  (`mem[$0088] != 0`).  **A run with `TUNDIFF=1` measures FIDELITY, never TIMING.**
- **`make clean` on every flag toggle** (again).  Building plain and then `PROBES=1` without a clean
  produced `undefined reference to g_isrBeamLines`.

### Verification (all re-run at this change)

| check | result |
|---|---|
| forward boot pre-draw, full 86 rows | **0** bad |
| forward descent, 71 frames | **0** bad / 244,240 bytes |
| forward, plain run (pre-draw + descent) | **0** bad / 247,680 bytes |
| boost path (pre-draw + reverse rings), 138 frames | **1** bad / 422,560 bytes — the known multi-writer race, same as at landing |
| post-boost pre-build claim | `tunnelCopperInstalled=0`, `liveCopper=2` (staticStandby) |
| `make validate` | 0 mem mismatch on all 8 touched twins |
| standby→doors gap | 1 frame — **unchanged** |
| tunnel→stars gap | 5 frames — **unchanged** |
| `dsMile` launch milestones | match baseline within noise |

### The one honest regression

The forward **static pre-draw** got more expensive: the whole pre-build bracket
(`SA_TIMED` slot 10, `prebuild(plot+paint)`) is **4201 ticks ≈ 263 ms**, of which the 6502 plot is
~1378, so the paint is **~2800 ticks ≈ 177 ms** against the ~847 ticks (54 ms) of the decode it
replaced.  The reason is structural: the decode writes each of the 86×40×3 bytes exactly once (and
batches by the long), while the painter draws 43 *nested outlines* — the vertical edges alone are
Σ heights ≈ 3784 row-columns × 3 planes of masked word RMW.

It is off every hot path (standby idle, static screen, music is VBI-driven) and no measured
cinematic gap moved, which is why it was left.  **If it ever needs fixing, the lever is the blitter,
not the loop:** the pre-build runs in MAIN-LOOP context (only the descent's `draw_ring_frame_step`
is ISR), so a blitter masked-fill path in `Bitmap::fillColor` — `BLTADAT=0xffff` +
`BLTAFWM/BLTALWM` as the mask, `BLTBDAT` = the plane value, C=D=dest, minterm `0xca`, as that
function's own comment already spells out — would apply.  It needs a main-loop-vs-ISR context flag,
for which `platform_tunnel_rings_begin()` is the natural place to set and a matching `_end()` the
natural place to clear.

### Still not done after that

Nothing.  The doors keep their own `$2000` decode by design, and every hook skips
`rowBase >= 0x2000`.
