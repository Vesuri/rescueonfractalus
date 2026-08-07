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
