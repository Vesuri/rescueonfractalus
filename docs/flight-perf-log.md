# Flight performance — the investigation log

The **narrative record** of the flight-scene (scene 7) performance work: what was tried, what it
measured, and why each candidate closed. This file is deliberately NOT in the agent's recall path —
it is the archive you grep when you want the reasoning behind a verdict.

**Live state lives elsewhere:**
- current numbers, ranked TODO, one-line closed list → the `flight-pc-profiler` memory
- measurement rules + harness traps → the `flight-measurement-rules` memory
- asm twin design/phases → `docs/asm-migration-plan.md`
- scene composition + instrument map → the `flight-scene` memory, CLAUDE.md

Rule of thumb when adding here: a verdict and its number belong in memory; the *story* belongs here.

---

## 1. What shipped, and what it measured

| date | commit | change | measured |
|---|---|---|---|
| 2026-06-24 | — | direct render replaces the mode-D→bitplane convert (`renderFlightDirect` stages 1-3) | ~172 vs ~339 ticks/frame |
| 2026-06-25 | afc509f | plane2 dots + terrain double-buffer (fixes plane1 flicker) | — |
| 2026-06-25/26 | 8b255fa | per-instrument cockpit dirty flags replace the 560-cell shadow scan | ~1662 → ~65 ticks/frame (~23×) |
| 2026-06-26 | 5872acc, 81bbde0, bcb56ed | dead GTIA/ANTIC `bus_write` pokes removed; POKEY write change-detect | VBI ~86 → ~53 beam-lines/firing |
| 2026-06-26 | 304c651, a19a547, cdd51d3 | HUD gauge sprites built once + `setY`; AH fill change-detected on pitch idx | perFrameWork ~79 → ~6 ticks/call |
| 2026-06-27 | — | plane2 scan bounded by the topmost skyline row (`minScan`), tracked free in the edge plot | scan ~324 → 113 ticks |
| 2026-06-27 | — | plane2 scan reads 4 bytes at a time; blitter ops overlapped with disjoint CPU work | fDirect ~478 → ~170 ticks/call |
| 2026-06-27 | 8673544 | `terrain_frame_setup` loop-1 invariants hoisted (~270 volatile reads/call gone) | setup −11% |
| 2026-06-28 | ec4a3c6 | **both terrain passes now display** (`g_flightRenderHalf`) — pass 1's frame used to be computed and thrown away | displayed frames +37% |
| 2026-06-30 | d2bff11, d2114aa, e27a292, 8b52207 | `terrain_column_rasterize_core` hand-asm + instruction shaving | asm/C 0.73 (~27% faster) |
| 2026-06-30 | c6285bb | `renderFlightDirect` plane-1 edge plot hand-asm | ~2.8× (26 vs ~78 ticks/call) |
| 2026-07-01 | — | `project_terrain_points` hand-asm (`ProjectTerrainAssembler.s`) | ~2.2-2.4× |
| 2026-07-01 | — | `terrain_frame_setup` loops hand-asm (`TerrainFrameSetupAssembler.s`) | ~23-26% |
| 2026-07-01 | — | `terrain_subdivide_column` hand-asm (`TerrainSubdivideAssembler.s`) | ~2.7% only — see §2.7 |
| 2026-07-01 | — | `mul_u8` bit-serial → byte-exact 64 KB `g_mulTable` | VBI `integ` ~13 → ~8/firing |
| 2026-07-05 | f6b13fd | `build_view_transform_matrix` + `signed_mul_8x16` asm (`BuildViewAssembler.s`) | ~3× |
| 2026-07-06 | a0199d7 | `g_mulTable` built at startup, not lazily inside the VBI | fixed a ~7 s flight-entry freeze |
| 2026-07-31 | 6cb213f, 3df1911 | incremental sprite clear; `expandShotRow` → 256-entry LUT; P3 scan early-break | P3 mirror 4.7% → 2.1% of the render frame |
| 2026-07-31 | — | `plot_scanline_down` Amiga byte-run fast path | — |
| 2026-07-31 | 69c8f6b | `ph2_fe`/`ph2_ff` dispatch collapsed to one `cmp`+`bcc.s` | far path 24c → 8c/column |
| 2026-08-03 | 0bc8d70 | object draw-order loop base-pointering | — |
| 2026-08-04 | 18b810e | `terrain_frame_setup` loop-1 asm rework | 90 → 72 beam-ticks/call (−20%) |
| 2026-08-04 | bcc1850 | `plot_scanline_up` fast path (fixed column, hoisted mask) | — |
| 2026-08-05 | b798c06, cd56e0c | **VERTB takeover** + OS interrupt audit | +9.4% throughput; see §4.1 |
| 2026-08-05 | c636951 | **rasterizer phase-2 restructure** | 24 → 15 beam-ticks/call (−36%) |
| 2026-08-05 | 1cab6f4, b73e8a6 | band composite cache + object-overlay dirty box; `terrain_frame_setup` u/v recurrence | +1.5% throughput; setup −16.4% CPU |
| 2026-08-05 | 855a7eb | `FIXED_RNG=1` pins the level across builds | measurement infra |
| 2026-08-06 | 34c6c91, 5a92d0b, 350e9ea | **the clear split** (§4.2) | blitter stall 3.78% → 1.97% of flight |
| 2026-08-06 | 0d2a614 | scaled object blit carries loop state in registers | spike call 862 → 723 t |
| 2026-08-06 | 611e856, a2f331f | SFX voice-priority mixer: pointer-walked scans, then a hand-asm twin | ISR 95.5 → 89.8 t/firing |
| 2026-08-06 | 538e811 | **`buildShotSprite`'s 94-byte scan deleted** — `mem[$2865/$2866]` already held it (§4.3) | sprite bracket 28.01 → 5.74 t/firing |

## 2. Closed candidates — the reasoning

### 2.1 Occlusion culling — dead at every scale (24346f9, cff080d)

Tried at three granularities. All NO, for **two different reasons**, which is the durable part.

| level | ceiling (draws nothing) | sound-bound hit | work actually removed | verdict |
|---|---|---|---|---|
| span-3/4 leaf group | 58% / 56% | 57% / 52% | ~2% of rasterizer | NO |
| whole rasterize CALL | 38% | 5% | 2% of draws | NO |
| whole SEGMENT (subdivide call) | 60-62% | 15-20% | 2.7% of draws | NO |

- *Too fine (leaf group).* A rejected DRAWDOT is already only 32 cycles (`moveq` + indexed byte
  load + `cmp` + taken branch), and the cull test must load **the same `COL_MAX` byte** — 22 cycles
  a column. Net ~13 (sp3) / ~22 (sp4) cycles a group ≈ 2% of the rasterizer. Even a *free perfect
  oracle* caps at ~17% of the rasterizer: the test eats ~85% of its own ceiling.
- *In between (rasterize call).* The calls that reach the rasterizer are the ones subdivide's
  height cascade already chose — the tall ones — so `max(ends) + W/2` clears almost nothing:
  ceiling 38% → sound hit 5%. Here **slack** kills it, not redundancy.
- *Too coarse (segment).* A culled segment averages **1.8 draws vs 14.1 overall**, and **93% never
  reach the rasterizer at all** — subdivide's own `spanLow` cascade already discards the same
  low/far segments for free. The cull is largely **redundant with an existing free test**.

**The height bound, settled** (this was the open question that made the sizing possible). Both
roughness sites displace a midpoint by half the remaining COLUMN span, and both take the
pre-displacement height as `ceil((a+b)/2)`, which can never pass `max(a,b)`. So the only rise above
the endpoints is the displacement chain `S(W) = floor(ceil(W/2)/2) + S(ceil(W/2))` → S(4)=1, S(8)=3,
S(16)=7, S(32)=15, S(64)=31 ⇒ **no drawn height exceeds `max(h_span,h_far) + W/2`, exactly.**
Verified on target: 0 false culls over 3233+2219 fires. The bare `max(ends)` the leaf version used
is **unsound**. Scan range is `[max(col,$2C), min(far,$D7)]` — **not `$D3`**; that off-by-one
produced 12 false culls in the first run.

**Not measured, if ever revisited:** cull at a span ≥ 5 where a hit skips a whole subtree. Blocker:
the displacement can push midpoints above both endpoints there, so you need `max(ends) + ~S/2` slack
*and* a min over S columns — i.e. a hierarchical min structure. A redesign, not a micro-opt.

### 2.2 DRAWDOT micro-opts / "incremental dot column" — capped at ~2%

**An accepted draw is NOT a plotted dot.** The dot goes in at the column's PREVIOUS top and the
per-frame `$6B` reset floor sits on `_sc == 43`, the single excluded scanline, so a column's first
accepted draw never writes. Measured over 489 half-frames (`g_rasDots`): 286590 draws → 39% accepted
→ **only 12% write a dot**. Therefore **DRAWDOT ≈ 23% of the rasterizer and tree traversal ≈ 77%**.

Phase-5's two plotCol tables (host-proven over all 65536 input pairs, 0 mismatch) measured **~0.9%**
against a ~6% cycle-count prediction. This also retires the old "incremental dot column ⇒ ~5%"
sizing — wrong denominator.

### 2.3 POKEY→Paula deferral — measured at zero (936538d probe; change reverted)

`update_paula_channel` only records into `want_*`; Paula is programmed once per frame by
`flush_paula`, so every recompute except the last per channel per frame *looks* like pure waste. And
it is: **29% of recomputes are redundant** (1875 of 6257) and a per-channel dirty bit collapses them
to 0. Whole-VBI result: **152.65 (deferred) vs 152.57 (immediate) = 0.05%.**

**Why it bought nothing: the redundant recompute was the cheap one.**
`sfx_voice_write_freq_ctrl` writes AUDF then AUDC, so the AUDF write recomputes against the OLD
AUDC — usually `vol == 0`, which takes the early-out (no poly select, no stride multiply). The
surviving recompute is the full path.

The "44% of the mixer call / 3.2% of all wall clock" leaf-split figure that pointed here was itself
misleading: most of that 44% is `rof_pokey_write`'s own call overhead, and the probe build's two
volatile counters were part of what it measured.

### 2.4 `terrain_plot_object` — there is nothing to hoist

The shape probe found **25 occupied map cells per iteration enter `terrain_plot_object` and only ONE
reaches `raster_scaled_object`**. That looked like 24 wasted visits. It is not: in the `< $FA`
branch everything before the `plot_step_hi >= $0D` test has a REQUIRED side effect —

- `terrain_point_distance()` latches the nearest point into `$2910-$2915` = the targeting-scope
  nearest-target state (that is its whole purpose);
- `terrain_clip_row_top()` calls `terrain_plot_pixel_core` — it **plots a pixel**; that single
  clipped dot is how a distant emplacement is visible at all;
- the fire-queue gate does a `bus_read(0xD20A)` RANDOM read, and RNG consumption is observable state;
- `$28FB/$28FC` (crosshair / target-visible) are set for any object with depth < `$37`.

`plot_step_hi >= $0D` gates **only** `raster_scaled_object`. The work is faithful and required.

Separately, the plotter's *measured* cost was over-attributed: the `FP_TIME` bracket's own floor is
~2.2 t/call, and the quiet control run reaches the plotter chain with ZERO A822 calls yet still
measures 110 t/it. Subtract the floor and the plotter's true combat cost is **~200 t/it = 3% of the
combat frame** — not the 69% the raw DRAW share implied.

### 2.5 The close-explosion spike — mechanism found, spike NOT eliminated (0d2a614)

`raster_scaled_object`'s cost is **quadratic in 1/step** (`0x2000/step` cols × `0x0C00/step` rows,
capped 32×12 = **384 cells**), and a DESTROYED emplacement takes `terrain_plot_object_a`'s
`occupant >= $FA` branch, which **quarters the step** — so an explosion both passes the `< $0D` gate
it used to fail AND blits at up to 4× scale, twice per iteration. 3 cells at depth 12, 384 at depth
0, mean 15 ⇒ **the mean is useless; bucket by call size.**

Register-carrying the loop state gave t/cell 5.28→3.53 (<16 cells) · 2.54→1.08 (16-63) · 2.95→1.34
(64-127) · 2.51→2.01 (256+). **One close blit is still ~723 t = 46 ms ≈ 2.3 painted frames**, because
it is dominated by the sheer 384 distinct cells and the source index advances ≥1 per cell (no
redundancy to exploit). The blit is only 0.5-0.8% of the AVERAGE combat frame — this was always a
spike fix, never an average win. Further reduction needs an asm twin, for a rare event.

### 2.6 Edge plot (160-column plane-1 scatter-OR) — ~0.05%

26 ticks/call = 0.87% of flight, already 2.6× the C. Shape: 50% of columns land on the same plane-1
row as their left neighbour (only 76/160 lookups and 92/156 byte-ORs are needed, mean 2.31 distinct
rows per 4-column byte) — but the per-column test that detects it costs ~13 cycles against a
~22-cycle lookup/RMW at ~50% odds ⇒ net ≈ 0. A free perfect oracle would still cap at ~0.4%.
Same floor that closed the leaf occlusion cull: **a check costs what the thing it skips costs.**

### 2.7 Subdivide — the recursion barely runs (017c0ff)

Per call (443 half-frames, 15287 calls): **1.23 inner iterations (81% at depth 0) · 0.40 midpoints ·
0.21 pushes · 0.12 pops · 0.61 rasterize · 0.40 skip.** The typical call makes ONE cascade decision
and rasterizes or skips, so the recursion-level optimisations the asm's structure invites are all
small. Two were implemented (`far == mid` reload elision — only 17% of inner far-loads; `submid`
reload — 50% of midpoints), measured at **0 ± 0.5%**, and ditched.

This also explains why the 2026-07-01 subdivide asm twin won only ~2.7% while the rasterizer twin
won 27%: **the differential bracket (and subdivide's real cost) is dominated by the raster leaf-fills
the recursion drives**, which are identical in both arms and cancel. Register-spill elimination only
pays when the spilled compute is the bottleneck.

Later, killing subdivide's dead `mem[]` round-trips (the `$8D-$91` entry load, flushed conditionally
with the BUDGET as a free dirty flag; the dead `$83/$85` stores before each rasterize) took its
asm-vs-oracle margin **7.4% → 12.5%**.

### 2.8 Fusion of the object draw-order loop + subdivide — ceiling ~1.8-2.5 points, not started

Two independent estimates that disagree by ~2×, so treat as a RANGE:
- **PC sampling** (273 samples): of subdivide's 7.7%, the caller-ABI blocks (asm prologue 0.7 +
  `sd_out` 0.4 + `sd_ret` 0.7) = 1.8 points; the rasterizer HANDOFF blocks = 2.3; genuine recursion
  = 3.0. Only 21 samples ⇒ ±25% relative.
- **Static 68000 count**: entry 378 + exit ~333 = ~711 of ~1250 cycles for a typical call.

⚠ The handoff third is **not** addressed by fusion — that is the *rasterizer's* call ABI, one level
down. Fusion buys the `movem` (164), the jsr/arg push (~70), the span byte↔word round trip (~230)
and the conditional exit flush.

**Judgement: ≤2.5 points for a large, risky, byte-identical asm rewrite of two hot functions — poor
effort/risk ratio, but not "too small to bother".** Two cheap slices, ~0.5 point each and low risk,
are worth taking if anyone is in there anyway: pass the span as args instead of through `$82-$86`
(kills 5 caller stores + 5 callee loads + 2 `lsl.w #8`), and drop the `sd_out` flush if
`$82-$86`/`$8D-$91`/`$9F` are proven dead on Amiga.

### 2.9 Other closed items

- **`build_poly_dist`** — the obvious explosion-pitch suspect (its cache key includes the AUDF
  divider). Called **zero** times.
- **Memoizing `sfx_pick_top_voice` behind a dirty flag** — rejected on validation grounds, not perf:
  the fixtures start each case from fresh random `mem[]`, so a MISSED invalidation passes
  `make validate` and corrupts audio at runtime.
- **PORTS (CIA-A) interrupt takeover** — implemented, measured, deliberately NOT enabled
  (`make PORTS_TAKEOVER=1`, off). The chain is `ciaa.resource` (~220 µs) + FS-UAE's **"UAE fs"**
  trap server (~90 µs) = an *emulator artifact*; a bare A500 reclaims ~0. Worse, taking the vector
  starves that trap server and **FS-UAE resets the machine** (2/2 runs) — it would break the very
  harness every measurement depends on.
- **`terrain_plot_pixel_core`'s Amiga-dead `$28E2/$80/$81/$B5` scratch writes** — ~15 plots/iteration
  ⇒ ~2 t/it; even inside a spike blit only ~4% of it.
- **"1 terrain pass instead of 2"** — user declined; it halves the ~2× displayed framerate.
- **Bitplane alternation instead of a 2nd CPU render/iteration** — investigated 2026-06-28 and
  measured NOT worth it. Steady flight already runs at the 2-convert/iteration floor and both
  converts are needed (the two `ds_frame` calls snapshot `$260E` at two different draw points). The
  Amiga must convert mode-D field → bitplanes; the Atari's flip is free because its display *is* the
  mode-D field.
- **Rasterizer-sets-plane2 directly** (2026-06-25, reverted) — two blockers: the displayed field's
  ~80 value-2 dots/frame are mostly written by a writer that was never identified (the full scan is
  robust precisely because it reads the FINAL field regardless of writer), and `renderFlightDirect`
  runs multiple times per game frame while the draw runs once, so "plot during the draw" is not
  idempotent. Payoff was only ~2-4% anyway.

## 3. 68000 codegen lessons harvested from the twins

General 68000 rules live in CLAUDE.md ("Optimising a native twin for the 68000"). These are the
specific, measured ones from this scene.

**From the `terrain_frame_setup` loop-1 rework (18b810e):**
- **Hoist loop-invariant *derived constants*, not just loads.** `SUB = alt_c + (alt_b != 0)` makes
  `diff = h - SUB` one word subtract and deletes the per-cell `tst`+branch; `K = (uint8)(-alt_b) >> 4`.
- **`lsl/lsr.w #8` costs 22 cycles** (6 + 2n) — treat every byte-pair↔word conversion as expensive.
  A 16-bit `((hi<<8)|lo) >> 4` is really two byte shifts: `(uint8)pair = (hi<<4)|(lo>>4)` and
  `pair>>8 = hi>>4`. Three killed per cell. Same idea: `rol.w #8,dN` instead of
  `move.w dN,dM / lsr.w #8,dM` when the source is dead.
- **Negate a 16-bit value byte-wise** (`neg.b` lo then `negx.b` hi — a MOVE between them is safe, it
  doesn't touch X) instead of assembling, negating and splitting. ~52 cycles.
- **`move.b` already sets N/Z** → a following `btst #7,dN` / `tst.b dN` is dead.
- Did NOT make the cut: full byte-wise `add.b`/`addx.b` for the u/v vectors (~100 cyc/cell more) —
  needs 8 data registers, leaving no scratch for the numerator/classify phases.

**From the four failed C restructurings of `terrain_column_rasterize_core` (all reverted):**
The C floor was 469 insns / 1568 bytes with 3 separate byte arrays indexed directly by `depth`.
(1) explicit per-array `uint8_t*` pointers: **+34 insns** (address-register pressure, GCC spills);
(2) hoisting `g_flightDotPlane` to a local: **+150 insns**; (3) packing `{col,height,frac}` into a
`uint32_t` stack: **2.4× worse** (1110 insns, 96 spills) — the 68000 has **no scaled-index
addressing**, so `uint32[depth]` costs a shift+add per access; (4) the textbook interleaved byte
stack walked by one pointer ±3: **+93 insns** — GCC emitted **zero `(a0)+`** and still used indexed
addressing. ⇒ **the interleaved-pointer-walk win needs hand-written asm.** It did: the asm twin won
27%.

**From the SFX mixer twin (a2f331f) — a "clean" pointer-walked asm loop LOST to GCC by 5%:**
GCC fully inlines AND unrolls the two 12-slot scans into straight-line **absolute-addressed** code:
`move.b (mem+$0706).l,d1` (16 cyc) + `beq.s` (10) = **26 cycles** per inactive slot. The textbook
pointer-walked loop is `tst.b (a0)+` 8 + `beq.s` 10 + `addq.l #1,a1` 8 + `dbra` 10 = **36** — ten
cycles worse, because loop bookkeeping costs more than autoincrement saves over absolute addressing.
v1 also imposed a 10-register `movem` (~180 cyc) against GCC's 3 (~68). What DID win: unroll +
`tst.b (a0)+` for the active test only (18 cyc/slot), a 5-register `movem`, table bases pinned in
address registers (12-cycle `d(An)` instead of 16-cycle absolute), and the leaf inlined as a local
`bsr` with args already in registers.

**Addressing-mode gotchas:**
- The 68000 has **no `(d16,An,Xn)`** — index displacement is only 8-bit. Add the index into the base
  once (`adda.l d7,a0`) then use plain `(d16,a0)`. (`ProjectTerrainAssembler.s`.)
- `bmi` cannot replace the per-column `cmp.b #$FF/beq` off-top skip in the edge plot: real `$260E`
  heights reach `$96` (~28% of columns have bit7 set); only `$FF` must skip.
- `signed_mul_8x16` is **sign-magnitude, not a true signed product** (P = m·|mc| unsigned, shift,
  re-sign = round toward zero), so unsigned `mulu.w` + conditional `neg` is byte-exact whereas
  `muls.w` + `asr` floors (off-by-one for negatives) and gets the wrong frac low bits.
- `mul_u8` is **not a plain product** (brute-force proven), hence the 64 KB `g_mulTable`.
  `MulTableAssembler.s` builds it via the symmetry `mul_u8(m,n) == mul_u8(n,m)` (triangle+mirror,
  ~32896 passes not 65536). There is no closed form — every fast formula misses ~1/3 of the domain.

## 4. The four structural wins, in detail

### 4.1 VERTB takeover — the OS was stealing the top of every vblank (b798c06, +9.4%)

Chasing an 8.0% "unresolved / Kickstart ROM" PC bucket. **The bucket itself was a sampler artifact**
— every sample is the SAME instruction, `$F811F8` = the first instruction of Kickstart's level-3
autovector stub, because FS-UAE's gdb stub takes its SIGINT break right after a pending interrupt has
been dispatched. The count tracks how many interrupts were taken, not how long ROM ran. Proof: the
takeover removed exec's chain walker AND all three OS VERTB servers (846 µs → 52 µs per firing) and
the bucket moved only 8.0% → 7.3%.

But the chase found the real thing: **the game VBI was 4th in exec's VERTB chain.** VERTB is raised at
beam line 0, so the beam position when our handler starts IS the whole pre-handler cost — measured
**line 12.70 mean / line 20 max = ~846 µs of every 20 ms frame**, because `AddIntServer(pri 0)` put us
behind graphics.library (pri 10) → gameport.device → timer.device. None of them do anything this game
uses.

| config | iterations/frame | vs original |
|---|---|---|
| `AddIntServer` pri 0 (4th) | 0.0409 | — |
| `AddIntServer` **pri 127** (head) | 0.0447 | **+9.4%** |
| full **vector takeover** | 0.0444 | +8.6% |

**The win is the ORDERING, not the removal** — head-of-chain → takeover is −0.8%, inside run spread.
Likely because the 846 µs delay also pushed the 3.4 ms handler later and the loop's vblank swap-waits
**quantise** an iteration to whole frames, so a sub-ms shift can cost a whole 20 ms frame on some
fraction of iterations — which is how ~4% of dispatch delay amplifies to ~9% of throughput.
**Corollary: saved compute can vanish into those swap-waits — always confirm with throughput.**

Takeover kept anyway (user's call): it drops ~780 µs/frame of provably useless work, and with the OS
servers off the vblank the benchmark became **bit-reproducible**. Implementation: replace
`SysBase->IntVects[INTB_VERTB]` (saved verbatim, restored before the LoadView/WaitTOF epilogue —
`WaitTOF()` needs graphics.library's server back). ⚠ As a vector handler, **clearing INTREQ is our
job**. `make VERTB_SERVER=1` restores the old chain for bisecting.

**OS interrupt audit (cd56e0c) — class exhausted.** EXTER: 0 firings. **BLIT: 6 per iteration, pure
waste — masked** (the dA JoRMaS framework re-arms `INTF_BLIT` after every blit for a blit-done ISR
this port doesn't have; every armed blit-done was a dispatch into graphics.library's handler).
843 → 0. Multitasking: `DispCount +0` (the VERTB takeover already removed exec's scheduling
heartbeat). COPER/AUD0-3/RBF/DSKSYN/TBE/DSKBLK: disabled in INTENAR. Leftovers total <0.1% on real
hardware.

### 4.2 The clear split (34c6c91 · 5a92d0b · 350e9ea) — 3.78% → 1.97% of flight

Blitter stall in `renderFlightDirect`, ticks per painted frame:

| stall | orig | +split | +plane3 one-shot |
|---|---|---|---|
| clear+copy drain | **43** | — | — |
| plane1-clear wait | — | 11 | 12 |
| plane3-clear wait | — | 11 | **0** |
| sky-fill wait | 14 | 16 | 16 |
| dot-clear wait | 1 | 3 | 1 |
| **total** | **59.8** | 41.6 | **30.2** |

1. **Split the clear by plane and await only what the CPU needs next.** The whole-buffer 3-plane
   clear + dot copy were drained together, but the next CPU work (the edge plot) touches **plane1
   alone**. So: clear plane1 → drain → kick the dot copy *while the blitter is idle* → edge plot runs
   on top of it → plane3 → sky fill. The copy's 17 ticks vanished entirely.
2. **The plane2 clear was pure waste** — the dot copy is a full A→D overwrite of every word it
   clears. −20% blitter work, free.
3. ⭐ **Then the cascade: once a region is no longer cleared each frame, every IDEMPOTENT writer into
   it becomes one-shot.** Dropping the per-frame plane3 clear made the crosshair (fixed bytes,
   visibility is a copper palette swap) a once-per-buffer draw, which made the band composite's
   40-long plane3 copy version-trackable (~30 of 40 skipped). **Generalise: after removing a clear,
   audit every writer into that region for redundancy — the clear was hiding them.** State needed: a
   per-destination-buffer validity latch plus invalidation wherever a foreign write can land (flight
   entry, since `terrainBitmap` is shared with the doors/tunnel/planet decode, plus the two rescue
   `s_cleanBmp` 3-plane copies).

⚠ Load-bearing detail: the blitter is ONE unit and **nothing drains the queue asynchronously**
(`INTF_BLIT` is masked and `processBlitterQueue()` only runs when a wait calls it), so "kick and
forget" means poking the blit REGISTERS directly, not queueing. The dot copy is issued while the
blitter is IDLE for exactly this reason. Note `blitterFillUp()` and `blitterDrain()` both begin by
draining the queue + `blitterWait()`, so they cancel any overlap set up before them.
Buffer layout: interleaved 120 B/row = plane1 at +0, plane2 (dots) at +40, plane3 at +80; sky fill
`(bp, 20w, 46 rows, mod 80)`, dot copy `(bp+40, 20w, 47 rows, mod 80)`.

### 4.3 The sprite bracket — a scan re-deriving what `mem[]` already held (538e811)

`g_vbiSpriteLines` split with `make SPRITE_SHAPE=1`: **`buildShotSprite` = 88% of the bracket**,
`decodeScannerBlinkCells` = 1% (its `mem[$33DF]` change-gate works; it was never the problem). Inside
buildShotSprite: run scan 53% · whole-sprite clear 25% · row decode+setX/setY 10% · copper 6%.

**The fix was not to optimise the scan but to delete it.** `build_player2_sprite` (`$8C58`, which
draws the P2 strip) already records the strip's start row in **`mem[$2865]`** and its length in
**`mem[$2866]`** — it needs them to erase exactly that region on its next call — and nothing else in
the binary writes either byte. Plus an incremental clear (only rows the decode won't overwrite; mean
run is 4.3 of 32 rows).

Sprite bracket **28.01 → 5.74 t/firing (−79%)**; whole flight VBI 132.50 → 110.07 t/firing (42% →
35% of all wall clock). End-to-end COMBAT 10.98 → 12.36 FPS. **Flat on quiet level 4** — with no
object/explosion strip active `mem[$0036] == 0` and buildShotSprite early-outs.

Verified with `make SHOT_VERIFY=1`: 3996 calls, **0 pixel mismatch**, plus 82 benign extent
differences all over all-zero rows (`anyLit = 0` ⇒ byte-identical invisible sprite; they happen when
`$0036` goes nonzero on a frame `$8C58` is skipped, leaving stale `$2865/$2866`). Side note: the old
scan was not even strictly correct — `draw_ah_ground_fill_p2` (`$40B0`) writes as low as `$0E87`,
inside the scanned `$0E34..$0E91` window, so the artificial-horizon fill could extend the run it
found. Reading the extent is immune.

### 4.4 Shape-probe results, 2026-08-05 (band block · terrain_frame_setup)

Denominators on the pinned level in ONE normal build (`fd_split.gdb`): **5695 ticks/iteration**, 1.9
`renderFlightDirect` calls/iteration, internal split per call **band+objects+crosshair 59 ·
clear/copy drain 49 · edge plot+fill kick 32 · sky-fill wait 19** — i.e. the bucket's biggest CPU
part was the one NOT on the TODO.

- **Band block (1cab6f4).** The band field is 99% static: **~0.9 of 160 bytes change per frame**, all
  in row 45 (the wing-clearance bar); rows 43/44/46 changed exactly twice in 420 frames (the entry
  transient). `ow != 0` (planes-1/2 overwrite) lives only at row 44 [19,20], row 45 [5,34], row 46
  [19,20]. Fix = cache the DECODED bytes per double-buffer half, refresh only changed longs, paint
  plane3 as a long COPY + RMW just the `ow` ranges. 59 → 43 ticks/call. Object overlay got a dirty
  COLUMN range beside the row range (it was scanning 40 bytes/row for object bytes that were 1.8% of
  them).
- **`terrain_frame_setup` u/v recurrence (b73e8a6), 67 → 56 ticks/call.** Loop 1's output arrays ARE
  its input arrays shifted by one, so the per-cell reassembly of `in_u`/`in_v` from memory (~100
  cycles) was redundant; u→d1, v→a2 carried across all 45 cells. Exactly 1 of 180 pattern cells is
  non-storing (`$B622[0]`); it re-syncs from memory. Other shape facts: the pattern-decode `btst`
  chain averages **67 cycles/cell** (a jump table would be ~36 — not done, ~5% more of the function,
  the best remaining item here); `dr == 00` on 408/420 frames; classify mix visible 35% / behind 33% /
  offscreen 32%; bits 0-3 of every pattern byte are always 0.
- **Rendering changes can't be judged headlessly → prove them with an in-process differential.**
  `make BAND_VERIFY=1` composites each frame BOTH ways and byte-compares (mismatch 0 / objLeak 0 over
  265 calls). ⚠ The harness MUST freeze the source field first: it is written by `game_sub_451d` from
  the flight VBI ISR, so two passes reading it separately disagree on ~1.5% of frames — that artifact
  was the first run's "mismatch=4".

## 5. Retracted conclusions — do not resurrect

| claim | status |
|---|---|
| "mem-bound ALGORITHMIC FLOOR — only an algorithmic change reaches 50 FPS; micro-opt EXHAUSTED" (2026-06-29) | **Disproven.** Hand-asm broke it with no faithfulness loss. The floor was GCC, not the algorithm. |
| "flight runs at ~2 FPS / ~6 FPS / 7.4 FPS combat" | **All probe-build artifacts.** Real: 14.4 FPS default, 12.4 combat (2026-08-06). |
| "combat costs 2.2× the frame" | **~1.54×.** The 2.2× came from a build where `COMBAT=1 FPSCOUNT=1` did not compile. |
| "combat's cost is the object plotter" (69% of the DRAW delta) | **Withdrawn** — numerology from an unsubtracted bracket floor. It is the SFX engine in the 50 Hz VBI. |
| "ground objects on screen are nearly free (0 / 1-2 / 3-5 buckets all flat)" | **Retracted** — the bucketing variable was ~1 per iteration, so nearly every frame fell in one bucket. |
| "`fps_ab`/`fps_multi` give the first cross-build-comparable absolute metric" | **Retracted.** Two builds on the same vbi window are not flying the same terrain. |
| "the `renderFrame` PC bucket is idle time waiting on vblank" | **No** — the spin costs 0.07%; that is GCC line attribution for surrounding orchestration. |
| "`mem[]` is in fast RAM (`&mem ≈ 0x264fe8`) so it isn't chip-DMA-contended" | **Retired as wrong-headed** — the target A500 has no real fast RAM; treat all RAM as uniformly expensive. |
| "the `ph2_loop` far-bisect math is irreducible; further gains are STRUCTURAL and faithfulness-bound" | **Disproven** by the phase-2 restructure (−36%, same subdivision count, byte-identical). |
| "the dot-plot is <0.3% of the draw, not a lever" | **Superseded** post-restructure — DRAW bodies are roughly half the rasterizer now — but see §2.2: the remaining levers there are still ~2%. |

---

## 6. 2026-08-07 — two wins of the same shape, and a harness trap that faked a third

### 6.1 `terr_blend` (8e02185) — loop-carried scratch living in volatile `mem[]`
`sample_terrain_height_bilerp`'s bit-serial blend ran three 8-step loops, fully unrolled by GCC, and
kept the fraction and both operands in `$27Fx` scratch, shifting them IN PLACE. That is 8 absolute
byte accesses per iteration (~128 of the ~170 cycles) purely to carry values from one iteration to
the next. Hoisting the three into registers needed the `mem[]` contract to still hold, and it turned
out to be trivial: the fraction shifts left 8 times and lo/hi right 8 times each, once per iteration
on BOTH branches, so all three end at 0 for every input. `tools/terr_blend_test.c` proves it over
all 2^24 triples (0 return mismatches, 0 mem mismatches).

Measured with `isr_ab.gdb` under `COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1` — the QUIET control, because
a plain COMBAT A/B drifted the fight (fire 241→220, impact 56→39) and contaminated the sfx bucket:

    proj   11.17 -> 8.95 t/firing   -19.9%    (~0.7% of all wall clock)

`integ` appeared to gain 1.50 t/firing in the combat run but held flat in the quiet control — that
part was the drifted fight, not the change. (integ does contain a second bilerp call, inside
`object_step_and_collide`, so it gains when objects are actually active; the quiet control cannot
show it.)

### 6.2 The object plane-1 overlay (36c4248) — a search for bytes the writers already knew
`renderFlightDirect` ORs the value-3 ground-object bits from `s_flightObjP1` into plane1 after the
sky fill. An earlier pass narrowed that walk from whole rows to a dirty bounding BOX, which made it
smaller but left it a SEARCH — and the search is almost all miss. `objp1_shape.gdb` (COMBAT,
FIXED_RNG, 358 painted frames): **447 box bytes visited per frame, 2.2% of them nonzero** (~10). The
COMBAT PC profile put 20/300 samples on the inner test, the largest single non-asm line.

The fix is not a faster search but no search: all three writers (`ROF_PLOT_DOT_P1`, `laser_dot_run`,
`laser_dot_column`) already compute the byte offset before OR-ing into it, so each appends it to
`g_objTouch` on the 0→nonzero transition and the apply walks the list. The box is still maintained,
so a 256-entry overflow just falls back to the old walk.

    PC profile   renderFlightDirect  10.0% -> 5.7%  (hot line gone from the drill)
    framerate    12.38 -> 13.09 FPS  (+5.7%), 12 of 15 disjoint segments up

Correctness: `make BAND_VERIFY=1` + `band_verify.gdb`, 300 painted frames, **objLeak=0** — no
nonzero byte remains anywhere in the scratch after the apply, which is exactly the claim that the
list covered every byte the box would have.

**Generalisable:** both wins are the same shape — *a hot loop redoing work its own callers already
did.* Neither was found by staring at the profile's top entry (the top two are asm and heavily
worked); both came from reading what a mid-sized bucket actually does. Worth sweeping for more.

### 6.3 CLOSED — the frame-sync vblank spin, and why it looked like 32%
`PlatformAmiga::renderFrame` ends with `while (g_vbiCount == last) {}`. `idle_probe.gdb` reported
**149 spin calls (42.7% of renderFrame) burning 32.3% of "flight wall"**, and the PC profile put
5-7% on that one line. Both were wrong, for two compounding reasons:

1. **The build was not invulnerable.** `prof_flight.sh` hardcoded its build and silently dropped
   `COMBAT=1`, so the ship crashed mid-window; after that `renderFlightDirect` stops painting while
   `renderFrame` keeps being called and spinning. The spin calls were the DEATH CINEMATIC.
2. **The denominator was wrong.** `idle_probe.gdb` prints power-on totals and divides `idleWall` by
   `renderWall` — the renderFrame subtotal, not wall clock. It also does not subtract the VBI ISR
   that runs inside the spin (and would run anyway, so is not recoverable).

Measured properly (`idle_window.gdb`: snapshot/delta, asserts `VVBLKI=$4ff5` and `$3D=00`,
normalises against `vbiDelta × 313`): **0 spin calls, 0.1% of wall clock.** With an invulnerable
build the bucket disappears from the PC profile entirely. Do not re-open.

`prof_flight.sh` now passes the measurement flags through and warns when neither `COMBAT` nor
`INVULNERABLE` is set.

---

## 7. 2026-08-07 (later) — the phase budget closed, and the handoff one level down

### 7.1 A 100%-covered map of a flight iteration (`amiga/phase_budget.gdb`, new)
`obj_shape.gdb` already printed the CL_PH phase table, but only on an `OBJ_SHAPE=1` build whose
`OP_TIME` brackets add ~110 t/it *inside* the DRAW phase. `phase_budget.gdb` is the same table with
every `OBJ_SHAPE`/`RS_SHAPE` global dropped, so it runs on a lean probe build, and it additionally
opens up the FRAME phase from brackets that already existed in the tree but were never printed next
to it. `COMBAT=1 FIXED_RNG=1 PROFILE_NORING=1 NO_TDRAW_PROF=1`, 271 iterations, **covered 100%**:

| phase | t/it (probe) | ~% of honest wall |
|---|---|---|
| DRAW (terrain+objects ×2) | 1483 | 46 |
| VBI ISR (104 t/firing × 3 brackets) | — | 25-33 |
| FRAME (ds_frame ×2) | 494 | 15 |
| SETUP (frame_setup ×2) | 168 | 5 |
| CLEAR + BOLT + ENEMY | 28 | ~1 |

**FRAME, opened up (t/it, probe):** perFrameWork/6 sprite builders **124** · renderFlightDirect
**260** (clear+copy 31 · edge+fillUp 78 · sky-fill blitter wait 35 · band+overlay 89 · 27 other) ·
cockpit scan **49** · updateFlightCopper **16**. **Nothing in FRAME is over 3% of wall** — so the PC
profile's "sprites 7.3%" is really ~3.8%, and that whole block can come off the TODO. (The profile
over-states every non-ISR bucket by ~1.4×, because ISR samples land on the Kickstart autovector
stub — 7% of samples for 25-33% of the time.)

### 7.2 Sized and CLOSED with numbers
- **`compute_row_xspans`** (a §6.1 terr_blend-shaped in-place `mem[$00B5]` accumulator): 31
  iterations × 2 calls/it = **~0.09% of wall**. Not worth the edit.
- **A hand-asm twin of `terrain_draw_objects`** (the 8.3% PC bucket, the largest non-asm one).
  Shape measured (`amiga/objloop_shape.gdb`, new): **72 pairs/pass, 34 both-endpoints-visible, 28
  culled at the primary, 10 companion-culled, 23 projections** — dead stable across 5 segments.
  Cost counted off the actual disassembly: **126 cycles per culled pair, 616 per visible pair**
  (240 of those the ten mem-to-mem vector copies), = ~53.5k cycles/it ≈ 5% of wall. The fat hand-asm
  could remove — `andi.l #255` on a promoted `uint8_t`, the redundant `moveq`+`move.b` zero-extends,
  a `lea` per table lookup, mem-to-mem `move.b` for the copies — totals **~0.8%**. Not worth an asm
  twin of the third-hottest function.

### 7.3 The subdivide→rasterizer span handoff — the 6.2 shape, one level down
`terrain_column_rasterize_core` and `terrain_subdivide_column_core` are both hand-written asm and
both keep the running cursor (col / height / frac) in registers — yet every call passed it through
`mem[$82/$84/$86]`: 3 stores in subdivide, 3 loads in the rasterizer prologue, 3 stores back at
`done`, then 3 loads plus two high-byte merges in subdivide. ~220 cycles per call of pure handoff,
plus ~38 of stack-argument ABI, for a value both sides already had in a register.

§2.8 had spotted it ("the handoff third is **not** addressed by fusion — that is the rasterizer's
call ABI, one level down") but never sized it, because nothing measured the CALL COUNT.
`amiga/ras_count.gdb` (new) does: **~49 rasterize calls per flight iteration** (17-31 per pass).
So ~11k cycles/it ≈ 1% of wall.

The public `terrain_column_rasterize_core` entry keeps its exact `mem[]` contract — it is now a thin
shim that loads the cursor into registers, runs the body and stores it back — so the C oracle,
`make validate` and `raster_verify.gdb` are untouched. The body becomes
`terrain_column_rasterize_span`: cursor in/out in `d0`/`d1`/`d4`, colBase in `a2` (subdivide already
holds `depth` there). `d4` being in/out also drops it from the `movem` (11 longs → 10).

⚠ **The trap, and it is generalisable: converting a `mem[]` handoff to a register handoff makes you
inherit the callee's zero-extension precondition — and the byte-width invariant held on only one
side.** The rasterizer adds `d4` with `add.w` in five places and relied on its own
`moveq #0,d4` reload to clean it; subdivide's `d4` has a DIRTY high byte (every writer there is a
bare `move.b`, which is exactly why its own `submid` does `move.w d4,d1 / and.w #$FF,d1` before
adding). Without an `and.w #$FF,d4` at the call site the differential reported **126 mismatches in
3812 subdivide calls, first at call 14, on `$84` (height)** — frac feeds the height accumulation.

Verified with **`make VERIFY=1 NO_RASTER_VERIFY=1 PROBES=1` + `subdiv_verify.gdb`** — that
combination is the one that puts the FAST path under a differential: it keeps the register handoff
live while the subdivide differential compares it, call for call, against the C oracle (which still
goes through the `mem[]` shim). **0 mismatches over 3686 calls** across the 5 SubPt stacks,
`$60`/`$80-$86` and the return value. All four build variants (`RASTER_C=1`, `SUBDIV_C=1`,
`VERIFY=1`, `VERIFY=1 NO_RASTER_VERIFY=1`) still link; the Makefile clears `RASTER_SPAN_ABI` under
the raster differential so every call there still goes through the C dispatcher.

**Framerate (`COMBAT=1 FPSCOUNT=1 FIXED_RNG=1` + `fps_seg.gdb`, ~2997-vbi window):**

| build | run 1 | run 2 |
|---|---|---|
| baseline | 13.09 *(recorded, 36c4248)* | **13.10** |
| span handoff | **13.38** | **13.18** |

Mean 13.10 → 13.28, **+1.4%**. ⚠ **The end-to-end harness cannot resolve this change**: the change
build's own two samples differ by 1.5%, as large as the effect, and only 6 of 15 individual segments
moved up in the paired run. All four points do fall on the right side of each other (both change
samples above both baseline samples), and that is the whole of the end-to-end evidence — it is
consistent with the ~1% static estimate but does not independently establish it.

**So this one is kept on the static count, not the framerate**: 12 fewer memory accesses and one
fewer stack-argument frame per call are visible in the disassembly, the call count is measured, and
the result is proven byte-identical. Useful side-note for the harness itself: the baseline
reproduced the recorded 13.09 to 0.1%.

## 8. 2026-08-07 (later still) — item 3: the combat DRAW delta, and the estimator that inflated it

Ranked item 3 was *"DRAW's unexplained +474 t/it in combat (1588 vs 1181 in the terrain remainder,
SAME pinned level, SAME straight trajectory) — larger than the whole object plotter, and nobody
knows why."* The standing hypothesis: bolt impacts disturb the attitude ⇒ more terrain in view.
`g_clAltDraw[8]`/`g_clAltIter[8]` had been written to test it and **never run**.

Five `COMBAT=1` vs `COMBAT=1 COMBAT_QUIET=1` pairs, level 40, `FIXED_RNG=1`, lean probes.

### 8.1 The viewpoint hypothesis — refuted, twice

Running the altitude table settles it immediately:

| alt bucket (`$28DA>>5`) | combat DRAW t/it | quiet | ratio | iteration share (c / q) |
|---|---|---|---|---|
| 0 (`$00-$1f`) | 1363 | 1107 | 1.231 | 0.443 / 0.425 |
| 1 (`$20-$3f`) | 1520 | 1201 | 1.266 | 0.055 / 0.041 |
| 4 (`$80-$9f`) | 1553 | 1203 | 1.291 | 0.502 / 0.535 |

The ratio is the same inside every matched bucket and the *mix* is near-identical, so the viewpoint
distribution explains none of the delta.

A second, stronger refutation came free. `g_clSubCalls` (new) counts terrain **tree entries** — one
`terrain_subdivide_column_core` call per both-endpoints-visible pair. It is a COUNT, so it carries
no bracket floor: **68 per iteration in BOTH builds, in all five runs.** Combat does not subdivide
more terrain. Whatever the delta is, it is not "more terrain in view".

### 8.2 Where inside DRAW (new: the `CL_SUB` three-segment split)

`terrain_draw_frame_core` split into head (per-frame table fills + `compute_row_xspans`) /
obj (`terrain_draw_objects`) / tail (derived, so the function's four early returns need no bracket):

| segment | combat t/it | quiet t/it |
|---|---|---|
| head | 65 | 60 |
| **obj** | **1553** | **1099** |
| tail | 54 | 33 |

92% of DRAW is the object draw-order loop, and effectively all of the delta is there. The `$0A00`
near-max cell aging scan — 256 volatile byte reads gated on `map_cell_hit_marker`, which only
SHOT/BOLT impacts set, so structurally a combat-only cost — is **16 t/it over ~70 scans**. Real but
small; it is inside the tail figure above.

With `OBJ_SHAPE=1` the obj segment splits further: plotter chain 319 vs 102 t/it over 48 calls/it in
both (the ~105 t/it bracket floor is therefore the whole of the quiet figure) ⇒ **~215 t/it of real
object work**, driven by 21 occupied cells visited per iteration against 1-2.

### 8.3 ⭐ The finding that matters more: a phase bracket exaggerates combat by ~15%

Every phase read slower in combat, **including ones whose work cannot vary with it** —
`terrain_frame_setup` +12%, `ds_frame` +18%. That is the signature of a slower *machine*, and a
phase bracket cannot tell DMA stolen by sprites/audio/blitter from computation. So `CALIBRATE=1`
runs a fixed-trip, data-independent load once per flight iteration and measures it five ways:

| row | combat | quiet | ratio |
|---|---|---|---|
| MEM 1024 reads, ONE bracket | 272.7 | 236.2 | **1.155** |
| MEM the same 1024 reads, 8 **ISR-free** sub-windows | 226 | 224 | **1.010** |
| MEM ×128 **beam-locked** to scanline 200 | 18.08 | 18.05 | **1.002** |
| MEM 1024 reads, AUD+SPRITE DMA masked | 269.1 | 237.8 | 1.132 |
| CPU 1024-step LFSR (registers), one bracket | 96.1 | 93.4 | 1.029 |

Masking audio and sprite DMA changes nothing, so it is not contention. Two independent
subtraction-free estimators agree the machine is identical. **The 15% is `elapsed −
g_isrBeamLines`.** `g_isrBeamLines` is credited at ISR **exit** (`rof_native_amiga.cpp`
`flight_vbi_native`), so an ISR straddling the bracket's start is over-subtracted and one
straddling its end is under-subtracted. Those cancel only when the two ends are independent — and
for a window near the 313 t ISR period they are not, because start and end land at nearly the same
phase of the ISR cycle. Combat's window sat at **87%** of the period, the control's at **75%**, so
the bias did not cancel between the builds either.

**Consequence:** every cross-build phase comparison in this tree is inflated on the side with more
ISR firings (combat ~21/iteration vs ~12-16). That includes the historical "combat costs N×" phase
tables. Same-build comparisons are unaffected; so are short brackets.

### 8.4 What item 3's number actually was

The object plotter doing real object work (~215 t/it), harness inflation of order 15% of the quiet
baseline, the aging scan (16 t/it), and a small per-tree-entry residual at an identical call count —
consistent with the two builds' trajectories separating late in the window (the `seg` lines show
combat's altitude leaving `$80` where the control holds it). Nothing in it is extra terrain.

New tooling, all off by default: `make CALIBRATE=1`, `amiga/calibrate.gdb`, the `CL_SUB`/`CL_AGE`
split and `g_clSubCalls` in `rof_native.c`, and the extra block in `combat_probe.gdb`.

---

## 9. 2026-08-07 (last) — ranked item 1: inside `integ`, and the bilerp blend

### 9.1 Two corrections before the numbers

**`integ` runs on HALF the ISR firings.** `vbi_handler_flight` alternates its per-frame work on
the `$00C8` parity counter, so `flight_control_integrate` fired 1399× per 2798 ISR firings. Every
recorded "15.3 t/firing" is therefore an average over all firings; per CALL it is **30.7 t**. The
same halving applies to `proj` (`update_terrain_scanline_proj`), its neighbour in the same branch.

**`compute_target_blip_position` is never called in flight** — 0 of 1399 firings, because `$0004`
(`level_or_state`) is 0. It is not a cost.

### 9.2 The shape (`amiga/integ_shape.gdb`, new)

13 regions lapped off ONE running beam stamp (a single `rof_beam_line()` per boundary, not an
`SX_SPAN`'s two), plus an empty lap as the per-bucket floor and a path counter per conditional
callee. Floor-corrected the regions sum to 30.24 t against the unprobed 30.68 — so the split is
the function, not the probe.

| region | net t/call | share |
|---|---|---|
| **obj — `object_step_and_collide`** | **11.2** | **37%** |
| ang — heading + `compute_obj_rel_angle_scale` | 3.18 | 10% |
| thr — throttle kick + clamp | 2.39 | 8% |
| slot — slot dispatch | 2.19 | 7% |
| pos · attc · level · head · objv · lock · hud · tail · disp | 0.47–2.00 each | 3–6% |

Level 2 said the bucket is not really `integ`'s at all: inside `object_step_and_collide`,
**`sample_terrain_height_bilerp` is 78%**; inside the bilerp, **the three `terr_blend` calls are
82%**. And the bilerp ran 3326× in the window — 1232 from `integ`, ~2094 from `proj`, where it is
likewise ~79% of the bucket. Summed it was **~11 t per ISR firing ≈ 10% of the whole flight VBI ≈
3.5% of all wall clock**: the biggest single block in the 50 Hz ISR after the sfx engine.

### 9.3 The fix — an 8-iteration bit-serial loop that is a separable sum

`terr_blend`'s loop shifts BOTH operands exactly once on BOTH branches. So at the start of
iteration `i` the operands are always `L0>>i` / `H0>>i` whatever path the fraction took, and the
term added is `T(X,i) = (X>>(i+1)) + ((X>>i)&1)` with `X = H` when f's bit `7-i` is set, else `L`.
`A` is a plain 8-bit accumulator ⇒ the total is a sum mod 256 ⇒ **order-independent** ⇒ separable
into "the terms f selects, from H" plus "the terms it does not, from L" — and the second is the
first applied to `~f`. Splitting the selector by nibble gives two 256×16 tables:

    A = Bhi[H][f>>4] + Blo[H][f&15] + Bhi[L][15-(f>>4)] + Blo[L][15-(f&15)]   (mod 256)

`tools/terr_blend_table_test.c` checks it over **all 2^24 (f,L,H) triples: 0 mismatches**, for
this 8 KB form and for the 2-lookup 64 KB single-table form. The mem[] contract is untouched
(the one `terr_blend_test.c` already proved). `make validate` stays 0 mem mismatch on the bilerp
and all four of its callers.

**8 KB, not 64 KB, on a RAM budget measured rather than guessed** (`amiga/memreport.gdb`, new —
`AvailMem` snapshotted either side of the scene constructor). Load image 505 KB + **158,544 bytes
of runtime CHIP `AllocMem`** = ~675 KB total, ~170 KB of it chip-mandatory. The game therefore
does NOT fit a bare 512 KB A500 and needs 1 MB. On a 512+512 machine the non-chip image is the
binding constraint: 490 KB against a 512 KB slow bank. +8 KB leaves 13 KB spare; +64 KB would
have overflowed it by 42 KB. Both forms measured the same speed, so the 64 KB one buys nothing.

### 9.4 Measured (`isr_ab.gdb`, `make BLEND_LOOP=1` restores the loop in the same tree)

`COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1`, both sides bit-reproducible
over two runs:

| | loop | tables | delta |
|---|---|---|---|
| **whole flight VBI** | **109.35** | **104.12** t/firing | **−5.23 (−4.8%)** |
| handler | 88.50 | 82.46 | −6.04 |
| `integ` | 14.79 | 12.96 | −1.83 (−12%) |
| `proj` | 9.91 | 6.22 | −3.69 (−37%) |
| sfx total | 36.77 | 36.42 | −0.35 |
| painted frames in window | 359 | 372 | +3.6% |

**−5.23 t/firing = −1.67% of ALL wall clock.** The two builds did not fly quite the same fight
(expl 21→27, fire 213→200) but the fight-sensitive bucket — sfx — moved only 1%, and `integ`+`proj`
account for 5.52 of the 6.04 t handler delta. `.bss` +8,196 bytes; `.text` +486 (the builder),
and the bilerp itself shrank 1644 → 1176 bytes.

### 9.5 The lesson worth keeping

**A lapped shape probe over-states the buckets it splits — read it as SHARES, never as absolutes.**
The probe put the three blends at 7.45 t/call; the end-to-end A/B recovered 5.52 t/firing ÷ 1.19
bilerp calls/firing = **4.6 t/call**, i.e. the probe over-read by ~40%. The RANKING was right and
the sizing was 40% optimistic — which is exactly the failure mode the "shares, not absolutes"
rule exists for. Estimate given before measuring: −2.3%. Actual: −1.67%.

And structurally this is the FOURTH win of the same shape as §6/§7: not the profile's top entry,
but a hot leaf doing work that its own structure had already made redundant. Here the redundancy
was in the algorithm itself — an accumulator whose associativity nobody had checked.

## 10. 2026-08-08 — clearing the ranked list: items 2-5, and two harness lessons

Four items filed in the `flight-pc-profiler` ranked list, done in order. All four are small
(0.2-1.2% each); together the static counts sum to ~2.5%. Read §10.5 before quoting a framerate
for them.

### 10.1 Item 2 — `terrain_draw_objects` copied the span into `mem[]` for subdivide to load back

The caller-side half of the §7.3 handoff shape. `terrain_draw_objects` seeded the running span by
copying the primary endpoint's projected vector into `mem[$82-$86]` — five memory-to-memory
`MOVE.B`, 24 cycles each confirmed off the disassembly — purely so
`terrain_subdivide_column_core`'s prologue could load the same five bytes straight back out.

`terrain_subdivide_column_obj` takes the object id instead and loads the span from
`$2400/$242D/$245A/$2487/$23B5` itself, which also makes those loads cheaper: `(d16,a0)` is 12
cycles against 16 for the absolute-long they replace.

⚠ **The one path where the caller's writes were observable is the entry-guard bail.** `sd_out`
flushes `$82-$86` from `d2/d3/d4` on every other exit, but `sd_ret` does not — so on that path the
caller's five writes WERE the visible residue. The asm publishes them there instead (the same
values, since `d2/d3/d4` were just loaded from the same arrays). **Measured before writing it:** the
bail fires on **137 of 12630** real subdivide calls (1.1%), so it costs ~1 cycle/call amortised.

Net off the disassembly: caller −120 +12 (third arg push), callee +12 = **~−96 cycles per call ×
~68 calls/iteration = ~6.5k cycles/it ≈ 0.66% of wall.** Correctness on the FAST path:
`make VERIFY=1 NO_RASTER_VERIFY=1 PROBES=1` + `subdiv_verify.gdb`, **0 mismatch over 5196 calls**
against a C oracle that is literally the old caller code. The verify wrapper is split into
snapshot/capture/compare helpers so both entries get a real differential without duplication.

### 10.2 Item 3 — the six sprite builders ran BEFORE the blits the CPU then stalled on

`renderFlightDirect` kicks two blits and blocks on both: the plane1 clear (the edge plot ORs the
skyline into it) and the sky fill (the band overlay needs it). Meanwhile the six flight sprite
builders are pure CPU on SPRITE buffers — they touch neither the terrain bitmap nor the dot side
buffer, so they have no dependency on either blit. They just happened to run in `perFrameWork()`,
called BEFORE `render()`, so all 124 t/it of them landed outside both blitter shadows.

`perFrameWork` now only marks them owed and `renderFlightDirect` runs them in the two shadows.
**The split is measured, not guessed:** the first attempt put the altimeter pair + AH in the early
slot and they turned out to be only ~2 ticks together, leaving the plane1 clear still stalling 10;
moving the scanner dot across (the independent one of the three, since both P3 builders share the
target state) covers it.

| wait (beam ticks per painted frame) | before | after |
|---|---|---|
| plane1 clear | 12 | 2 |
| sky fill | 17 | 1 |
| dot clear | 12 | 5 |
| **total over a ~938k-tick window** | **13856** | **2538** |

**−1.21% of wall clock**, reproduced identically on a second run; `renderFrame`'s whole scene
bracket 21.3% → 19.3% of flight. This is the biggest of the four.

⚠ `renderFlightDirect` has three early-return paths (no bitmaps / the rescue-figure pause / a frame
with no fresh terrain), so `buildFlightSpritesFlush()` after `render()` is the safety net — without
it the altimeter/AH/scope would freeze on exactly those frames. The knock fast path at
`renderFrame`'s top still skips the builders entirely as before (it returns before `perFrameWork`,
so nothing is ever owed there).

### 10.3 Item 4 — `terrain_frame_setup`'s pattern decode is a top-nibble jump table

The oracle's decode is a nested `btst`/`beq` chain 2-4 levels deep at 18-20 cycles a level. But it
never tests the pattern byte's low nibble on any path, and every branch outcome collapses to two
independent signs:

    u += sA*rot_a + sB*rot_b     v += -sA*rot_b + sB*rot_a
    b6 += (sA>0) ? $F0 : (sA<0) ? $10 : 0        X += sB

with `(sA,sB)` a pure function of the top nibble. So `pat & $F0` IS the table index: one `and.w` +
one `add.w` + one `jmp (d8,PC,Dn.W)` replaces the chain. Blocks are 32 bytes so the longest body
fits with a uniform `bra.w` exit; a `dcb.b` pad after each block fails to assemble if one grows too
big. The table must sit inline — `jmp (d8,PC,Dn.W)` has only an 8-bit displacement.

**Verified three ways, because 7 of the 16 blocks are unreachable from the real data.** Host-
exhaustive: the derived `(sA,sB)` table matches the oracle branch-for-branch on all 256 pattern
bytes. On target: `tfsetup_verify.gdb`, 0 mismatch over 60 calls across the whole `$2276-$24FF`
output block + the `$80/$81/$B4/$B5/$B6/$28DB` residue — that covers the 9 nibbles the four real
tables contain (`{0,1,2,4,5,6,8,A,C}`; every byte's low nibble is 0). Blocks 3,7,9,B,D,E,F are
host-proven only, since no table can produce them.

Paired differential runs stopped at the SAME 60 calls (`FIXED_RNG`, vbi 17960 vs 17954): asm
3587 → 3379 beam ticks, and normalising by the unchanged C arm's own 2.3% drift gives −2.1 to −3.5
ticks/call ⇒ **−4 to −7 t/it ≈ 0.2% of wall**, against −6.8 predicted from the real tables (89 → 55
cycles/cell weighted by `$B622`'s mix). Table costs 512 bytes of code.

### 10.4 Item 5 — the four `$6B` runs are ONE contiguous 132-byte fill

`$264E/$266F/$2690/$26B1` ABUT (`$264E+$21 == $266F`, and so on), so the four `$21`-byte runs are
one contiguous 132-byte run `$264E..$26D1` = exactly 33 longs (the COL_MAX `$260E` horizon reset for
columns `$40..$C3`). `$6B` broadcast to all four lanes is endianness-neutral, and every address is
even — all `move.l` needs on a 68000. The old comment claimed `$266F/$26B1` were "odd addresses" so
batching would fault; **they are odd only as OFFSETS.**

⚠ **The pointer must stay `volatile`.** A plain `uint32_t*` loop is recognised as a memset and
becomes `jsr memset` — and this build's freestanding memset (`support/gcc8_c_support.c`) is a
byte-at-a-time `move.b d0,(a0)+`/`cmpa.l`/`bne` loop at ~24 cycles a byte, handing all 132 byte
writes straight back. That is a NEW trap worth remembering: hand-batching a fill can be silently
undone by GCC's memset recognition, and whether that is a win or a loss depends entirely on the
freestanding CRT's memset.

Measured (`combat_probe.gdb`, paired runs): **DRAW head 67 → 53 t/it**, with `obj` (1536 → 1534) and
`tail` (53 → 53) unchanged as controls, so the delta is isolated to the code changed. **−14 t/it ≈
−0.39% of wall**, against −11.6 predicted.

**Tried and reverted:** the neighbouring `$67` fill at `$263A/$26CE`. GCC splits those `uint16_t`
stores back into 40 byte stores, but batching them the same way is WORSE — with two interleaved
volatile long pointers over a 5-trip loop GCC emitted a redundant volatile READ before every byte
store. 40 bytes is ~0.05% of wall, so it is noted in the comment rather than chased.

### 10.5 The two lessons

1. **An internal control beats a cross-build phase bracket.** §8 established that a phase bracket
   over-reports the build with more ISR firings by ~15%, which makes cross-build brackets suspect.
   Item 5 dodged that entirely: the DRAW split has three sub-brackets and only ONE of them could
   possibly move, so `obj` and `tail` holding still (1536→1534, 53→53) is direct evidence that the
   −14 t/it in `head` is real and not drift. **Prefer a measurement that contains its own control.**
2. **GCC can undo hand-batching.** See the memset trap above. Always re-read the disassembly after
   a batching change — the source saying `move.l` guarantees nothing.

## 11. 2026-08-08 (later) — pricing the object draw-order walk, the last unpriced item in the pipeline

The PC profile's third-largest bucket, `terrain_draw_objects`, is the loop that walks the `$B67C`
draw-order table. Its callees (project / plot_object / subdivide / rasterize) are separate symbols,
so the bucket is pure BOOKKEEPING — and it holds **8.3% of wall with literally zero objects on the
map**. It was the one item in the terrain pipeline nobody had costed properly; §7.2 had counted the
hand-asm headroom (~0.8%) but not the *work*.

### 11.1 Shape — re-measured on the target (quiet) baseline

`objloop_shape.gdb`, `COMBAT=1 COMBAT_QUIET=1 PROBES=1 FIXED_RNG=1 PROFILE_NORING=1`, 462 passes
over vbi 1900→4003, every segment at `VVBLKI=$4ff5`/`$3D=00`:

    pairs/pass 72   primary-culled 28   both-visible 34   projections 24

Dead stable across all 7 segments and **identical to the combat shape** (§7.2's 72/28/34/23), so the
recorded numbers transfer to the target baseline unchanged. 72 − 28 − 34 = 10 companion-culled.

The trip count is not data-dependent: `$B67C` is static ROM data (dumped from `rof.xex`: 144 bytes,
72 pairs over 45 distinct object ids `$00-$2C`, no pair with `obj0 == obj1`), every path through the
body advances the index by exactly 2, and the only exit is `idx == $90`. **72 iterations, always.**

### 11.2 The honest price — 4.4% of wall, not 8.3%

Counted off the lean-build disassembly (cycles per pair, callees excluded):

| path | per pair | count/pass | cycles/pass |
|---|---|---|---|
| primary-culled | 120 | 28 | 3360 |
| companion-culled | 216 | 10 | 2160 |
| both-visible | 476 | 34 | 16184 |
| | | | **21704** |

× 2 passes = **43.4k cycles/iteration ≈ 4.4% of wall** (the §10.1 denominator, ~985k cycles/it).
**So the PC profile over-states this bucket by ~1.8×** — consistent with the standing ~1.4× ISR-stub
warning plus call-boundary attribution. Any plan sized off the 8.3% was sized off roughly double the
real thing.

### 11.3 What came out — three pieces of 6502 register-save residue, and a free unroll

None of it is drawing; all of it is the transliteration keeping the 6502's habits:

1. **`$28DB` (`collapse_cur_obj`) was stored every pair.** It is the 6502's save slot for X across
   the calls; the C already keeps `obj0` in a register. Nothing reads it in between — its only other
   user is `terrain_frame_setup`, long returned — and the loop always ends on the pair at `$8E`, so
   the oracle's final value is just the last `obj0`. Written ONCE after the loop.
2. **The `$272E` reload after `terrain_subdivide_column_obj` was a pure round trip**, plus a dead
   `if (idx == 0) idx++` (the 6502 shares that INY with the culled path; the reload is even and
   `<= $90`, so it can never be 0). The STORE stays — that residue is observable.
3. **The draw-order index was a `uint8_t`**, costing an `andi.l #255` plus a `moveq`/`move.b`
   zero-extend at each of the three places it indexes a table. It cannot wrap, so it is now an
   `unsigned`.
4. **`$24B4` (the visibility class) got its own address-register base**, so a cull test no longer
   materialises the per-object base pointer; that `lea` is now built only on the visible path, where
   the five vector copies need it. GCC also keeps the pointer live across the calls, which makes the
   primary's second class read 8 cycles instead of 12.

**The unroll is a bonus that falls out of (2)+(3).** With the reload gone and the index no longer a
byte, GCC can finally see that the stride is a constant 2 and the trip count is exactly 72 — so it
unrolls ×3 with a single `cmpi.l #144,d2` exit test, amortising the loop tail from 22 cycles a pair
to 6. That is the largest single component of the culled path's saving.

| path | before | after | Δ |
|---|---|---|---|
| primary-culled | 120 | **80** | −40 |
| companion-culled | 216 | **160** | −56 |
| both-visible | 476 | **406** | −70 |
| **cycles/pass** | **21704** | **17644** | **−4060** |

**−8120 cycles/iteration = −0.82% of wall**, and the walk drops 4.4% → 3.6%. The prologue grows from
7 saved registers to 11 (+64 cycles per call × 2 calls/it) and the code from 355 to ~800 bytes;
both are already inside the figure above / negligible against the ~499 KB image.

**Correctness: `make validate FN=terrain_draw_frame` — 2000 cases (real flight snapshot, X in
{0,$30}), 0 mem mismatch.** This function is a validated twin, so unlike the asm work in §9-10 the
proof is exhaustive over the fixture and needs no on-target differential.

### 11.4 What is left in the walk, and why it stops here

3.6% of wall remains, in three roughly equal pieces: the five per-pair vector copies that seed
subdivide's sub-point [0] (~0.83%), the per-pair table + class loads (~1.1%), and the stack-argument
call ABI into subdivide/project (~0.47%).

- **The five copies cannot go the way §10.1's five did.** Those went through `$82-$86`, which the
  subdivide asm holds in registers and only publishes on the entry-guard bail. These write SubPt[0],
  which subdivide genuinely reads back from memory — the entry guard reads the column, and `load_far`
  reads the whole point on every inner iteration at depth 0. The stores are load-bearing.
- **A hand-asm twin would be worth maybe 1.4% but is effectively unverifiable.** Every other asm twin
  here is proven by an in-process differential: run both implementations on the same inputs, compare.
  That is impossible for this one — it calls `terrain_plot_object`, which reads POKEY RANDOM and
  writes the terrain bitmap, the object arrays and HUD state, so a second run is neither
  side-effect-free nor repeatable. It would be the first asm twin in the tree with no differential.
  **Not taken.** If it is ever revisited, the price of admission is an RNG-replay harness, not the asm.

### 11.5 End-to-end, for what it is worth

`make COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1 FPSCOUNT=1` + `fps_seg.gdb`, same vbi 1901→4900 window as
the recorded baseline, all 15 segments valid (`VVBLKI=$4ff5`, `$3D=00`, alt bucket `$80` throughout):
**1122 painted / 3001 vbi = 18.69 FPS**, against the recorded **18.41** (1105/3001). That is +1.5%,
against +0.83% predicted from the static count — i.e. **the harness cannot resolve this change**
(one sample each, and `FIXED_RNG` pins the level, not the trajectory). The number to quote is the
static one. Recorded here only as a smoke test: the game still flies the same level, paints every
segment, and nothing regressed.

## 12. 2026-08-09 — attacking FRAME: three run-scans at 80 cycles a byte

FRAME had been the second-biggest phase for weeks (375 t/it, ~20% of the shipping frame) and the
`flight-pc-profiler` note said of it only that "nothing here exceeds ~3% of wall individually, so it
is 5-point-win territory". That was true of the buckets that had been *printed*. One had not been.

### 12.1 The bucket nobody had looked at

`renderFlightDirect` has five `FD_LAP` accumulators and `phase_budget.gdb` printed four of them.
The fifth, `g_fdScan`, was accumulated in the tree and never read by any script — so the split
looked like `clear/copy 45 + edge+fillup 70 + fill wait 2 + band+overlay 71 = 188` against a
`g_fDirect` of 259, with 71 t/it unattributed and quietly ascribed to "the sprite builders, which
run in the blitter shadows anyway". Printing it: **`late sprite` = 55 t/it**, the two P3 sprite
mirrors, on their own, in one slot.

**Generalisable: a probe that is accumulated but never printed is worse than no probe**, because
the residual it leaves gets explained away rather than measured. Grep the tree for `FP_TIME`/`FD_LAP`
accumulators and check every one appears in some script.

### 12.2 What they were doing — 78-84 cycles to test one byte

Three per-frame sprite mirrors locate their object by scanning a window of the Atari PMG buffers:

| mirror | window | bytes | cyc/byte | cyc/frame |
|---|---|---|---|---|
| viewport P3 | `$0F32-$0F85` | 84 | 78 | 6552 |
| scope P3 | `$0F98-$0FB8` | 33 | 78 | 2574 |
| scanner dot | `$0B88-$0BB8` (mask `$30`) | 49 | 84 | 4116 |

All three are written the obvious way, `for (int o = lo; o <= hi; o++) if (mem[base + o] & m)`, and
GCC compiles each byte test into `move.l d,d1 / addi.l #base,d1 / move.b (0,a2,d1.l),d1 / ... /
cmpi.l #hi,d / bne`. **Essentially all of it is addressing** — the `int` index plus the volatile
`mem[]` base defeat any strength reduction, so the loop re-derives a long-indexed effective address
every iteration. ~13.2k cycles a frame, in three loops that in the quiet baseline normally find
nothing at all.

Cross-check before touching anything: 2 × 9126 cycles = 40 t/it for the two P3 scans against a
measured `late sprite` bracket of 55, i.e. 73% of it. The count and the bracket agree.

### 12.3 The fix, and why it is a scan and not a published extent

`buildShotSprite` had exactly this shape and was fixed in 538e811 by reading the extent its writer
already publishes (`$2865`/`$2866`). The P3/missile buffers publish no extent — `draw_player3_object`
does track `player3_draw_y`/`player3_prev_rows`, but claiming them means proving no other writer
touches those windows, and the whole family of scenes shares those buffers (the stars phase uses
`$0F32` as a starfield player). So these keep the scan and make it cheap: `pmgScanFirst` /
`pmgScanBounds` walk a pointer and test **four bytes at a time**.

- A test against a mask replicated into all four lanes is **byte-order independent** — nonzero iff
  some byte matches, whichever end they come from — so aliasing `mem[]` as a `volatile uint32_t` is
  safe here, unlike a value read (CLAUDE.md's endianness rule). No value leaves the long: once a
  long tests nonzero its bytes are re-read individually through `mem[]`.
- **Alignment is taken from the pointer, not the `mem[]` offset**, so nothing depends on where the
  linker put `mem[]`. (`$0F32` is even but not long-aligned; the prologue peels 2 bytes.)
- Counted loop, not `for (; p + 3 <= e; p += 4)`: written that way GCC re-derives the limit inside
  the loop (52 cyc/long). A countdown lets it hoist the end pointer — 44 cyc/long = **11 cyc/byte**
  for the unmasked scans, 17 for the masked one.

### 12.4 ⚠ The differential caught what reasoning would have shipped

`make SCAN_VERIFY=1` + `amiga/scan_verify.gdb` re-derives the run the original byte-loop way on
every call and compares. First run: **24 mismatches in 2244 calls** — every one a run whose top or
bottom had moved by exactly one row. The P3/missile buffers are ISR-written
(`draw_player3_object` clears and redraws the strip), so an ISR landing between the helper and the
oracle makes them read different bytes.

The temptation is to write that off as "obviously a race" and ship. Instead the oracle is now run
**twice, once either side of the helper**, and a call whose two oracle passes disagree is discarded
as `raced`. Re-run: **0 mismatches, 31 raced (1%), 59% of calls with a real object present.** All 24
reappeared on the other side of the ledger. A genuine helper bug cannot depend on the buffer
changing, so this separates the two mechanically rather than by assertion.
⭐ **The reusable trick: when a differential's two arms read live ISR-written state, sandwich the
oracle. It costs one extra pass in a test build and converts "probably a race" into evidence.**
(`band_verify` solves the same problem the other way, by freezing the source.)

### 12.5 Measured — and the third of it that comes straight back

`phase_budget.gdb`, quiet arm, same vbi 1901→5400 window, all 7 segments valid:

| bracket (t/it) | before | after | Δ |
|---|---|---|---|
| clear/copy (scanner scan) | 45 | 34 | −11 |
| late sprite (two P3 scans) | 55 | 20 | **−35** (−34.6 predicted) |
| **fill wait** | **2** | **15** | **+13** |
| `renderFlightDirect` | 259 | **230** | **−29** |

The targeted brackets moved as counted. But `buildFlightSpritesLate` sat in the **shadow of the
sky-fill blit**, so making it cheaper just means the CPU reaches the `blitterWait` sooner and stalls
there instead: 13 of the 35 t/it are handed straight back. Net −29 t/it = **~1.9% of the shipping
frame**, not the 3.5% the raw cycle count promised.

⭐⭐ **Generalisable, and it cuts against a habit this log has: cheapening CPU work that sits inside
a blit's shadow only pays for the part that EXCEEDED the blit.** §10.2 moved work *into* the
shadows and was measured as a win for exactly the same reason, in the same place — the shadow is a
fixed-size bucket, and both directions have to be priced against how full it currently is. The
repayment is to move other independent work in (§12.6).

⚠ DRAW read +86 t/it and the cockpit scan +12 in the same run. Both are trajectory drift — this run
flew a varying altitude where the recorded baseline was uniformly `$80` — and neither is downstream
of the change. Read the brackets that CONTAIN the change and the one bracket mechanically coupled to
it; treat the rest of a cross-run phase table as noise.

### 12.6 Repaying the shadow — the band's cache refresh moves into it

The windscreen-band composite splits cleanly along the dependency. **Step 1** (change-detect the
mode-D band field against the per-half shadow, refresh the decode caches) reads only `mem[]` and the
caches and writes only the caches. **Step 2** (the paint) writes `vrow`, the very bitmap the sky fill
is filling. So step 1 was hoisted above the `blitterWait` and step 2 left below it — pure code
motion, no logic change.

Sampling the band field a few hundred cycles earlier adds no exposure: it is ISR-written and a torn
read was already documented as harmless and self-correcting. The `BAND_VERIFY` source freeze moves
with `srow`, so both passes still see identical bytes; `band_verify.gdb` reports **calls=300
mismatch=0 objLeak=0**.

Measured, same window, and this run's trajectory matches the recorded baseline (uniform `alt=$80`),
with the untouched siblings as the internal control — **SETUP 142→141, DRAW 985→970, cockpit 40→39**:

| bracket (t/it) | baseline | after §12.3 | after §12.6 |
|---|---|---|---|
| clear/copy | 45 | 34 | 33 |
| edge+fillup | 69 | 70 | 70 |
| late sprite | 55 | 20 | **56** ← step 1 moved IN |
| **fill wait** | 2 | **15** | **2** ← stall fully reabsorbed |
| band+overlay | 71 | 73 | **34** ← step 1 moved OUT |
| `renderFlightDirect` | 259 | 230 | **215** |
| **FRAME** | **375** | 352 | **329** |

The shadow swallowed the whole 13 t/it it had taken back. Both commits together: **FRAME 375 → 329
= −46 t/it = ~3.1% of the shipping frame** (1505 t/it at 20.80 FPS).

### 12.7 End-to-end

`make COMBAT=1 COMBAT_QUIET=1 FIXED_RNG=1 FPSCOUNT=1` + `fps_seg.gdb`, vbi 1902→4900, **all 15
segments valid** (`VVBLKI=$4ff5`, `$3D=00`): **1303 painted / 2998 vbi = 21.73 FPS**, against the
recorded **20.80** (1248/3000). Per-segment **17.5 – 24.1** against the baseline's 17.2 – 22.8.

+4.5% throughput against the +3.2% predicted from the phase brackets — agreement within this
harness's documented upward bias, not independent confirmation of the size. The number to quote as
the win is the bracket delta; the framerate is the standing baseline. What *is* worth noting is that
the whole per-segment distribution moved, both ends, which is the one end-to-end signal this harness
gives that a single mean does not.

**Target check: 25 FPS from 21.73 is a 1.15× gap = −13% of frame time still to go** (it was −18%).

## 13. 2026-08-09 (later) — the cockpit scan: the filed candidate was 6.5x too big, and the real one was next door

The ranked list said: "**the cockpit digit decode, 39 t/it (~2.6% of wall)** — `render()` decodes all
five digit 2x2 blocks whenever `g_ckDigits` is set at all... a 5-bit mask would cut it ~5x", with a
⚠ attached: *measure first which group fires; `g_fCockpitScans` says something decodes on ~0.9 of
iterations, but it could be `g_ckDial` rather than the digits.* Taking that ⚠ seriously is the whole
story of this entry.

### 13.1 The measurement, which cost one run and redirected the session

Three per-group brackets in the cockpit block (`g_ckDigitT/g_ckLockT/g_ckDialT` + fire counts),
printed by `phase_budget.gdb`. Quiet baseline, 474 iterations, 7/7 segments live, covered 100%:

| group | t/it | fires | per iteration | cells decoded |
|---|---|---|---|---|
| **lock-on (#11)** | **24** | 415 | **0.88** | 7 every fire |
| digits (#17-19) | 6 | 27 | 0.05 | 22 cells every fire |
| dial (#4/#5) | 2 | 15 | 0.03 | 4.06 |

So the digit decode is **~0.4% of wall, not 2.6%** — it is expensive *per fire* and fires almost
never — and the "~0.9 of iterations" the note could not attribute is the **lock-on indicator**, which
had never been named as a perf item at all. The writer counter settled the digit sub-question in the
same run: `writeDigit` 1.00 calls per fire, 0 stride flips, i.e. exactly one of the five blocks
changes when any does.

**The lock-on's mechanism.** Its random-blink state (`$007E == $80`) reads POKEY RANDOM for a 0..7
frame delay and flips **bit7 of one cell** when the delay underflows — ~9 times a second at 50Hz,
against a ~21Hz render, so it dirties the strip on 44% of painted frames. `platform_lockon_changed()`
was a strip-wide boolean, so each of those re-decoded all seven cells of `$3491-$3497`.

Fix (b9258cb): the hook carries the cell index; the strip keeps per-cell byte flags over 8 bytes so
the walk is two long tests; consecutive dirty cells merge into one `decodeCockpitSpan` (the 6-cell
fill sweep still costs one call). **24 -> 9 t/it, 7 -> 1.50 cells per fire.**

⭐ **Why byte flags and not a bitmask.** The writer is the VBI ISR and the consumer the main loop. A
`mask |= bit` from the ISR can be lost inside the consumer's byte-wide `mask = 0`; a per-cell byte
cannot, because each cell owns its byte and every store is atomic on the 68000. Clear-then-decode
then makes the worst case a duplicate decode, never a dropped one. Same reasoning as the dial
registry — the difference is only that 7 cells fit in two long tests, so it needs no 480-entry walk.

### 13.2 The bug the perf change uncovered — and a class of risk a differential cannot see

Making the digits per-block (one registry slot each) is the same edit, and it broke something. There
are **two writers of the digit cells**: the faithful `startup_init` ($3FFA) in the flight VBI, and
the Amiga's `startup_init_native` from `perFrameWork`. They share the `$0645/$0646/$0647` change
caches, so the VBI one — 25Hz against render's ~21Hz — usually *consumes* the change, and it writes
its glyphs through `draw_glyph_2rows`, **which raises no dirty flag at all**. The old strip-wide flag
hid that: any native fire repainted all five blocks and swept up whatever the VBI had written. Per
block, it no longer does.

This is not a new bug, it is an old one made visible: the user had independently noticed the **Range
To Pilot readout (#17) sometimes showing a stale value** — which is `$33B4`, the block whose cache
(`digit_cache_647`) the VBI writer owns. 368affe hooks the faithful writer (each rewritten 2x2 block
registers its four cells through the existing per-cell registry) and *then* takes the per-block
decode: **6 -> 1 t/it, 6 blocks -> 1.00 per fire.** `make validate FN=startup_init` 0 mismatch/20000.

⭐⭐ **`make CK_VERIFY=1` + `amiga/ck_verify.gdb`, and the reason it had to exist.** The risk in a
targeted-decode change is not arithmetic — `decodeCockpitSpan` is untouched, so every in-process
differential in the tree would have reported a clean 0 — it is a **missed write**. The harness tests
that instead: snapshot the group's destination bytes, decode the whole group again, and assert the
second decode changed nothing. *A targeted decode is complete iff the full decode is a no-op.*

Three things it took to make that instrument trustworthy, each a reusable correction:
- **Snapshot the SOURCE before the change under test, not after.** The first version snapped the
  source inside the verifier, i.e. after `decodeLockonDirty` had already read the cells — so an ISR
  write landing in that gap looked like a coverage hole. It reported exactly **1 bad in 284** lock-on
  checks, which is the most dangerous kind of result: small enough to hand-wave, large enough to
  block. Moving the snapshot ahead of the whole cockpit block took it to 0 and cost nothing.
- **Bracket the whole block, not each group** — a group's cells may legitimately be decoded by a
  *later* group's registry (the hooked faithful writer flags through the dial registry, which runs
  after the digits), so a per-group check reports a false hole.
- **Verify-every-frame was too slow to reach the thing being verified.** 12 groups x 3 full decodes
  per frame slowed the launch cinematic so much the probe sat in Standby for the whole window:
  `VVBLKI=$52d7`, `painted 0`, and a cheerful `0 bad / 23352 checks`. The live-flight assertion
  caught it; gating on "this frame decoded something" restored flight and still gives ~3.7k checks.

Before the writer hook, on the same instrument: **20 bad / 209 digit checks.** After: **0 bad in live
flight over 306 lock-on and 3366 digit checks, 4 raced.**

### 13.3 What it came to

Whole cockpit scan **39/42 -> 22 t/it** (the 42 and the 22 both carry the ~3 t/it of new group
brackets), with **DRAW 961 -> 964 and SETUP 142 -> 141 as untouched internal controls** in the same
runs. That is ~**1.4% of the 1440 t/it shipping frame**. End-to-end `fps_seg` reads **21.82 FPS**
(1309/3000, 15/15 segments live) against the standing 21.73 — **+0.4%, which is agreement with the
bracket, not evidence of it**; a 1.4% change is below this harness's ±2% resolution.

⭐ **The generalisable lesson, and it is about the ranked list itself, not the code: a filed size is a
hypothesis, and this one was wrong by 6.5x in the item's favour *and* pointed at the wrong function.**
The note that saved it was the ⚠ its own author had attached — "measure which group fires before
believing the ~2%". One counter, one run. **When a ranked item's size came from reading code rather
than from a counter, the counter is the first thing to write, and it is cheap.** The corollary is
worth as much: the item next to it, never ranked at all, was 4x bigger.

## 14. 2026-08-09 (last) — the band paint RMW goes long-wise: 146 cycles a byte -> 121 a long

The last sized, unclaimed item left in FRAME after §12/§13. `renderFlightDirect`'s windscreen-band
composite paints the wing-clearance bars over the live terrain with a read-modify-write per byte,
across each row's cached `ow != 0` range:

```c
for (int n = hi - lo + 1; n--; d1++, d2++, ow++, p1s++, p2s++) {
    const uint8_t m = *ow;
    if (!m) continue;
    *d1 = (uint8_t)((*d1 & ~m) | *p1s);      // plane1: salmon bar
    *d2 = (uint8_t)((*d2 & ~m) | *p2s);      // plane2: centre marker
}
```

### 14.1 The filed size was right, and the disassembly is why

Unlike §13's item, this one's number came from the disassembly rather than from reading the source,
and it held. GCC compiled the loop with a single index IV and five invariant bases, so **every one of
the seven memory accesses is `(0,An,Dn.L)`** — 14 cycles of EA on a 68000, against 8 for `(An)+`:

```
95c4: move.b (0,a5,d0.l),d1   ; ow      14      95da: and.b  (0,a3,d0.l),d1   ; plane2  14
95c8: beq.s  ...              ;          8      95de: or.b   (0,a2,d0.l),d1   ;         14
95ca: not.b  d1               ;          4      95e2: move.b d1,(0,a3,d0.l)   ;         14
95cc: move.b d1,d6            ;          4      95e6: cmp.l  a1,d0            ;          6
95ce: and.b  (0,a4,d0.l),d6   ; plane1  14      95e8: beq.s  ...              ;          8
95d2: or.b   (0,a0,d0.l),d6   ;         14      95ea: addq.l #1,d0            ;          8
95d6: move.b d6,(0,a4,d0.l)   ;         14      95ec: bra.s  ...              ;         10
```
= **146 cycles per painted byte**, 56 per skipped one.

### 14.2 Why the long-wise version is byte-identical, and why it needs no zero test

`kBandOW = kBandP1 | kBandP2`, so a byte with `ow == 0` also has `p1 == p2 == 0` and
`(d & ~0) | 0 == d`. Rounding each row's `[lo,hi]` out to whole 4-byte groups therefore pulls in
lanes that are a **genuine no-op RMW**, not an approximation — and since AND/OR/NOT are per-lane, the
`uint32_t` alias is byte-order neutral (the safe case of CLAUDE.md's endianness rule, same as the
plane3 long copy directly above it). Destination and caches are 4-aligned: `bp` is `AllocMem`'d, the
scanline stride is 120, the plane stride 40, the cache row bases multiples of 40 — plus
`__attribute__((aligned(4)))` on `s_bandP1c/P2c/OWc` to pin it.

**A per-long `if (!m) skip` was measured out rather than assumed in.** `amiga/band_shape.gdb` (new —
`shape_probe.gdb` prints this section too, but only after EDGE and TFS lines that a BAND_SHAPE-only
build has no symbols for, and a gdb script aborts on the first missing one) dumps the per-row ow map:

```
row 43 ow : .... .... .... .... .... .... .... .... .... ....     (no bar bytes at all)
row 44 ow : .... .... .... .... ...1 1... .... .... .... ....
row 45 ow : .... .111 1111 1111 1111 1111 1111 1111 111. ....
row 46 ow : .... .... .... .... ...1 1... .... .... .... ....
```

Read in fours, **no group inside any row's range is all-zero** — 12 longs, every one of them real
work. The test would have been pure cost.

### 14.3 The end-pointer test is what got `(An)+` out of GCC

The first cut (`for (int n = gn; n--; )`) kept GCC on the same one-index-five-bases form it had
chosen for the byte loop: **170 cycles a long** (18 for the indexed `move.l`, 20 for each indexed
`and.l`/`or.l`). Changing the exit test to a pointer compare against a precomputed end
(`do { ... } while (ow4 != ow4End);`) forces one of the five to be a real IV and ivopts then keeps
all five:

```
80cc: move.l (a3)+,d0   12      80d8: and.l  (a1),d0    14
80ce: not.l  d0          6      80da: or.l   (a5)+,d0   14
80d0: move.l (a2),d1    12      80dc: move.l d0,(a1)+   12
80d2: and.l  d0,d1       8      80de: cmpa.l d5,a3       6
80d4: or.l   (a6)+,d1   14      80e0: beq.w  ...        12
80d6: move.l d1,(a2)+   12
```
= **121 cycles per long** (120 on the unrolled second copy, which exits `bne.s`). So 4 bytes of band
went 584 -> 121.

⭐ **Generalisable, and cheaper than the alternatives: when GCC strength-reduces a multi-pointer walk
into indexed addressing, make the loop's exit test a pointer compare.** On a 68000 that swap is worth
6 cycles per long access. The two escalations considered first were both worse: a `noinline` helper
pays ~150 cycles of stack-passed args and prologue per row against the ~230 it recovers, and inline
asm is an outsized instrument for a ~1% item.

### 14.4 What it measured

`amiga/band_shape.gdb`, quiet baseline, ~690 painted frames each arm, and the two neighbouring
sub-brackets are the same-run control (they are essentially all bracket floor and cannot move):

| bracket (t/painted frame) | before | after |
|---|---|---|
| object plane1 overlay | 1.89 | 1.66 |
| crosshair | 1.73 | 1.52 |
| **band composite (paint)** | **17.41** | **9.19** |

**-8.23 t/frame = -47% of the paint = -16.5 t/it ≈ -1.15% of the 1434 t/it shipping frame.** The
static count predicts -7.1 t/frame (32 painted bytes x 146 + 2 skipped x 56 = 4784 cycles, against 12
longs x 121 = 1452 plus ~150 of extra per-row group arithmetic); measurement and count agree to 17%,
with the controls drifting 0.2 t/frame = 3% of the effect.

`BAND_VERIFY` (which composites the frame both ways and byte-compares all 4 rows x 3 planes against
the ORIGINAL per-byte loop, kept live for exactly this): **0 mismatch / 300 calls, objLeak 0.**

End-to-end `fps_seg`: **22.12 FPS** (1327 painted / 3000 vbi, 15/15 segments live, 17.7-24.6) against
the standing 21.82. **+1.4% against a predicted 1.15% is agreement, not evidence** — it is inside the
±2% single-window noise, and three of this run's segments sat at `alt=f7/fd/08` where the baseline
was uniformly `alt=80`, i.e. the trajectory diverged as it always does. The bracket and the cycle
count are the claim.

### 14.5 What is left in FRAME

Only the change-detect loop (~0.45%: 40 volatile long compares/frame at ~90 cycles, GCC indexing
because the loop juggles six pointers — and §14.3 is now the known fix for exactly that shape).

## 15. 2026-08-09 (last) — the band's change-detect scan, and the blit shadow that turned out not to be one

The last sized item in FRAME, and the third instance of §14's pathology in the same 60 lines of code.
The band's decode cache is refreshed by comparing 40 longs of the mode-D field against a per-half
shadow. The compare was fused with the (rare) decode, so the loop carried **six** pointers — field,
shadow, and the four decode outputs — and GCC strength-reduced them all to one index plus six bases:

```
7b9a: lea    (0,a2,a0.l),a4    12       7bfa: addq.l #4,a0       8
7b9e: move.l (a4),d0           12       7bfc: addq.l #4,a1       8   <- bound for the RARE path
7ba0: cmp.l  (0,a0,d6.l),d0    20       7bfe: moveq  #40,d0      4
7ba4: beq.s  ...(taken)        10       7c00: cmp.l  a0,d0       6
                                        7c02: bne.s  ...        10
```
**90 cycles to compare one UNCHANGED long**, ×40 a frame, on a path that in half of all frames finds
nothing at all — and 8 of those cycles maintain `a1`, a loop bound only the rare decode path reads.

### 15.1 Three things had to line up to reach 46

Splitting the scan out (two pointers) and applying §14's pointer-compare exit test got 70, not the
58 predicted: GCC emitted `move.l (a0),d0` plus two `addq.l #4` because only `f` post-incremented.
Post-incrementing **both** and adding `#pragma GCC unroll 10` — the trip count is a compile-time
constant — removed the loop entirely:

```
7b6c: move.l  (a2),d0    12        7b74: move.l 4(a2),d0   16
7b6e: cmp.l   (a3),d0    14        7b78: cmp.l  4(a3),d0   18
7b70: bne.w   ...        12        7b7c: bne.w  ...        12
```
**46 cycles a long** (38 for the first), no bookkeeping at all. The scan needs no index, because the
decode below rescans from `g = 0`; all it has to report is "something differs". `rowChanged` went
with it — reaching the decode now means the scan tripped, so the `owLo/owHi` re-derive is
unconditional, which is idempotent even in the rare case where the ISR heals the difference in
between.

⭐ **Worth remembering as a pair with §14: the pointer-compare exit test is what stops the
strength-reduction, and `#pragma GCC unroll` is what pays off a constant trip count.** Neither alone
got there — 90 → 70 → 46.

### 15.2 The blit shadow was NOT a shadow, and that is the more valuable finding

This loop sits between `blitterFillUp` and its `blitterWait`, so §12.5 says the win should be
absorbed: cheapening CPU work inside a blit's shadow only pays for the part that exceeded the blit.
The prediction was that some of the 9.3 t/it would come back as `fill wait` growth. It did not:

| bracket (t/it) | before | after |
|---|---|---|
| late sprite (holds the scan) | 54 | **45** |
| **fill wait** | 4 | **1** |
| renderFlightDirect | 198 | **189** |
| FRAME | 299 | **291** |
| DRAW *(control)* | 971 | **971** |
| SETUP *(control)* | 140 | 142 |

**−9 t/it, matching the static count to within 0.3, and the wait went DOWN.** So the 2-4 t/it those
wait brackets had been reading was **their own floor, not a stall** — the sky-fill blit had already
finished by the time the CPU arrived, at W = 54 and still at W = 45.

⭐ **The reusable correction: "wait ≈ 2 t/it" is not evidence that a shadow is full — it is at the
FD_LAP floor, and a floor-valued bracket cannot distinguish "just barely stalled" from "not stalled
at all". Read the wait as a DELTA across the change, in both directions.** Here the delta settled in
one run what the absolute could not, and it also brackets the blit: combining this with §12.5 (W=35
→ wait 15) puts the sky-fill at **B ≈ 45-50 t/it**. So at W = 45 the shadow is now essentially
exactly full — **the NEXT saving taken out of this slot will start being absorbed**, and moving more
work into it is worth nothing. That number prices every future scheduling decision in
`renderFlightDirect`.

`BAND_VERIFY` (the cache path against the original per-byte composite, which is exactly the
instrument for "did the refresh miss a change"): **0 mismatch / 300 calls, objLeak 0.**

⚠ Noise floor from the same run: `cockpit scan` 20 → 17 and `band+overlay` 18 → 20 are brackets this
change cannot touch, so ±3 t/it is the drift on a sub-bracket of that size. The −9 is well outside
it; DRAW landing on 971 t/it in both runs is the tightest control the harness has produced.

### 15.3 A harness trap this session paid for

The two runs overlapped, and the tell was unmistakable: **the first task's trailing
`grep .run/gdb-out.log` printed the SECOND run's output.** gdb streams into that log live, so a
complete-looking table can be read while `diag_run.sh` is still in its ~25 s shutdown — whose final
`pkill -9 fs-uae` can kill the emulator the next run just started. Wait for the task notification,
not for the log to look finished. (Recorded in [[flight-measurement-rules]].)

### 15.4 The end-to-end harness read this real win BACKWARDS — and that is the expected outcome

`fps_seg`, 15/15 segments live, ~3000-vbi window: **21.92 FPS (1315 painted / 3000)** against
304f7bf's 22.12 (1327). **−0.9%, for a change the phase budget measured at −9 t/it = −0.64%.**

Nothing is wrong with either number. A ~0.6% change is a quarter of this harness's ±2%
single-window noise, and `FIXED_RNG` pins the LEVEL, not the trajectory — the 304f7bf run had three
segments off `alt=$80`, this one has one, so the two flew different frames. The phase-budget arm is
the trustworthy instrument here and it is not close: same build for both arms of the comparison,
sibling brackets as controls, and **DRAW landing on exactly 971 t/it in both runs.**

⚠ **So the standing baseline is now 21.92, recorded lower than the previous row even though the
frame got 9 t/it cheaper.** That is the honest entry. The temptation to re-run until the number
goes up is exactly what [[flight-measurement-rules]] forbids ("never quote the better of two runs"),
and re-running a `FIXED_RNG` binary is n=1 anyway — two runs of it are bit-identical. If a future
session wants real end-to-end variance it must use `fps_multi.gdb`'s disjoint windows, and even
then the resolution floor stands: **at this point in the project, individual wins are below what any
framerate measurement can see, and the phase budget is the only instrument that can price them.**

## 16. 2026-08-09 — `terrain_subdivide_column` RE-PRICED (analysis only, nothing shipped)

The standing ⭐⭐⭐ "START HERE" item said subdivide is "the biggest bucket that has been examined
only once". That framing predates **ea8edc0** (the 1-arg object ABI) and **54e7999** (the ZP-residue
reader survey), and it does not survive them. Nothing was changed here; this section exists so the
next session starts from the numbers instead of the framing.

### 16.1 The shipping call path

`grep` of the linked build: **there is exactly ONE subdivide call site in flight** —
`SUBDIV_OBJ(0x00, order_idx, obj0)` in `terrain_draw_objects` (rof_native.c ~L7955), which resolves
to `terrain_subdivide_column_obj(obj0)` under `ROF_SUBDIV_OBJ1ARG`. The `_core` entry is reached only
through the 6502-ABI shim `terrain_subdivide_column()`, i.e. the validation oracle. **So all tuning
belongs to the OBJ entry, whose span already comes from the object arrays, not from `$82-$86`.**

### 16.2 The dominant path, hand-counted

Shape (unchanged, `ras_shape.gdb` 2026-08-08): 68.4 calls/iteration · 1.21 inner iterations ·
**0.55 rasterize calls, so 45% of calls only ever skip** · 0.40 midpoints · 0.11 pops · 1.1%
entry-guard bails. Counting the 45% path off `TerrainSubdivideAssembler.s`:

| block | cyc/call | verdict |
|---|---|---|
| `sd_out` flush (`$82-$86` span, `$8D-$91` mid guard, `$9F`) | ~140 | **the only live item — see 16.3** |
| prologue + epilogue `movem` (8 longs) + `rts` | 164 | all 8 registers live; see below |
| span load from the object arrays | 112 | **at the 68000's floor** |
| entry guard (`$B5` + far0.col + the compare) | 96 | `$B5` has a live in-flight reader |
| entry setup (`a0`/`a1`/`a2`) | 44 | already minimal under the 1-arg ABI |
| **actual work** (phase 2, phase 3, inner, cascade, pop) | **~182** | |
| **total** | **~742** | **~75% marshalling** |

**Why the span load cannot use the entry guard's word trick.** The guard replaces a 22-cycle
`lsl.w #8` with `move.w mem+SDCOL_HI,d0`, which works because `SDCOL_HI` is a fixed EVEN address and
the 68000 is big-endian, so slot 0's byte lands in bits 8-15. Neither precondition holds elsewhere:
the object arrays are at `$242D`/`$2487` (**odd**), so `mem + $242D + obj0` is even only for odd
`obj0`; and inside the body the base is `a1 = mem + depth` with depth variable, so a word read faults
on half the depths. A `hi<<8` word table was priced too: `moveq/move.b/add.w/move.w(0,a4,d0.w)` = 46
against the current 50, i.e. **+4 saved per load against +16 for putting `a4` back in the `movem`** —
a net loss. **The two `lsl.w #8` are the floor.**

**Why the `movem` cannot shrink.** d2/d3/d4 = span, d5/d6/d7 = mid, a2 = depth, a3 = budget, with
d0/d1/a0/a1 as the ABI scratch. Dropping `a2` (deriving depth from `a1 - mem`) was counted: −16 on
the `movem` pair and −8 per push/pop, against +16 for computing the return value and +4 on each of
two compares — **a wash**.

**Why `sd_out`'s two `ror.w #8` are the floor.** The alternative byte-wise flush is 58 cycles
against 42, `movep` writes bytes 2 apart (`$82`/`$84`, not `$82`/`$83`), and `swap` is a 16/16
exchange, not a byte one.

### 16.3 The one live item, and the reason it was NOT taken unilaterally

`sd_out` writes `$82-$86` on **every** call — but subdivide itself no longer reads them (the OBJ
entry loads its span from the object arrays), so the flush is now **pure residue**. If nothing
between two subdivide calls reads those cells in flight, it can be deferred to **once per frame**:
~100 cycles × 67.4 calls = **~6.7k cycles/iteration ≈ 1.0-1.5% of the frame**, the largest single
item left outside DRAW's inner loops.

⚠ **It is also exactly the risk class that burned the previous session.** §"CLOSED — subdivide's
exit ZP residue": the drop was built, `subdiv_verify` read **0 mismatch over 5104 calls**, and three
live consumers were broken — because a differential compares what the function WRITES and is
structurally blind to a reader outside the call. Deferring the flush makes `subdiv_verify` stop
comparing those cells, i.e. it **installs the same blindness**. The known readers are in other
scenes (`dl_lms_push_bottom_core` RMWs `$82/$83` as the door-scroll LMS pointer; `alien_field0_fill`
seeds from `$84`), which a once-per-frame flush would still satisfy — but "still satisfy" has to be
*proved by a reader survey*, not assumed, and the survey must cover the `.s` files and the linked
disassembly, not just the C.

**Decision deferred to the user (2026-08-09): survey first, then decide.** Nothing built.

### 16.4 Also priced here, both small and both fully inside `subdiv_verify`

- **Inline `submid` + `push_mid`** at their call sites: `bsr`+`rts` is 34 cycles, two of them per
  midpoint × 0.40 midpoints/call ≈ **27 cyc/call ≈ 0.33% of the frame**. `submid` has two call sites
  (phase 2 and `sd_dosub`), so inlining duplicates it.
- **Classify `far.hgt` on its HIGH byte first**, the way the `far.col` escape already does: negative
  ⇔ `hi & $80`, `> $FF` ⇔ `hi != 0`, `< $6C` ⇔ `hi == 0 && lo < $6C`. That skips the 22-cycle
  `lsl.w #8` on the common path, ~24 cyc × 1.03 inner iterations ≈ **0.27%**. ⚠ `sd_wtFarH` needs the
  full 16-bit `far.hgt`, so the negative path must still assemble it — **shape-probe how often
  `far.hgt < 0` before building this**, or the win could invert.
