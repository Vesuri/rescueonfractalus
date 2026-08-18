# Scenes 1 (Logo) + 2 (Station) — what they are, and what it takes to run them on the Amiga

## ⭐ STATUS 2026-08-13 (read this first)

| Build-order step (§5) | State |
|---|---|
| 0. `SKIPBOOT` + SDL `ROF_START=logo\|station` | ✅ **DONE** (fd1ab66) |
| 1. Staged XEX load; Station on SDL | ✅ **DONE** — plays end to end, hands off to a Standby pixel-identical to a plain boot (fd1ab66) |
| 2. Restore `display_scroll`'s dropped writes | ✅ **DONE** (61b5613) — both PMG paints confirmed on screen |
| 3. Amiga Station: decoder, copper list, tall bitmap, scroll | ✅ **DONE** (120719e) |
| 4. Amiga Station: sprites | ✅ **DONE** (27a24ae, +ad10385) — **user-confirmed on screen** |
| 5. **Logo** | ✅ **DONE** — `src/rof_logo.c` written, verified headlessly (see §1.5), **user-confirmed on screen** |
| 5b. Logo→Station handover | ✅ **DONE** — the entry now blanks before rebuilding the shared list/bitmap (§1.6) |
| 6. Cleanup + docs | ✅ **DONE** — the dead `station_*_native` block is deleted (§2.5 defect 2); the §4 renames were already in |
| 6b. Station PMG placement + spacecraft colour | ✅ **DONE** — three real bugs, all §2.4a |
| 6c. **Star fade-in speed** | ✅ **DONE 2026-08-13** — 162 vblanks → **29**, fade steps now 1 vblank each like the Atari (§7) |

Deviations from the plan as written, all deliberate:
* **One `Gtia9CopperList` serves BOTH scenes** instead of separate Station/Logo lists — they are
  the same screen mode and differ only in leading blank lines, row count and palette hue, which
  are now `buildLayout()` parameters.  One field bitmap and one decoder likewise.
* **16 luminances, not 8.** `amiga/assets/atari_pal.h` ignores bit 0 of the colour byte (a GTIA
  colour *register* has three luminance bits) but GTIA mode 9 is the one case where all four
  count — that is what "one hue, sixteen luminances" means.  `tools/gen_gtia9_pal.py` extends the
  port's own YIQ formula to half steps; even entries reproduce `atariToOCS(hue | n)` exactly.
* **The logo bitmap is a generated C header, not `assets/logo.raw` + incbin.** It lives in mem[]
  at `$60A3` where the Atari puts it, so SDL renders it through its own ANTIC/GTIA path for free
  and there is no incbin/Makefile plumbing.
* **The Logo's display list is NOT baked** — `rof_logo_run()` will call the genuine
  `rle_decompress()` on the real RLE at `$52E7`, exactly as `$5000` does.

Two bugs the work turned up (both fixed, both worth remembering):
* `PlatformSDL::tickVBI` re-seeds `colHW[]`/`gprior` from the OS page-2 shadows every frame,
  standing in for the OS vblank's stage-2 shadow→hardware copy.  **Both boot scenes REPLACE the
  OS vblank** (`$51EF`/`$1B30` are reached before any OS stage-1/2 work and return with an
  RTI-equivalent — which is why each increments RTCLOK itself), so no copy happens on real
  hardware and both program GTIA directly.  Re-seeding forced GPRIOR back to 0 and the station's
  GTIA-mode-9 field came out as mono hi-res against an all-zero palette: a **black screen**.
* `game_vbi_isr` falls back to the STANDBY body for an unknown VVBLKI.  The staged loader zeroes
  mem[], so VVBLKI reads 0 from the stage load until `station_init`'s `$198D` — and in that window
  a whole standby VBI ran over the station's fresh memory, writing lock-on glyphs into its P0 PMG
  page at `$3492`, which the sprite mirror then drew as a phantom sprite.  Found with a hardware
  watchpoint after the arithmetic refused to explain a shape at a buffer offset the `$277A` table
  does not contain.  Now a boot scene dispatches ONLY its own vector.

---

*(Original derivation follows; it was investigation-only when written.)*

Ground truth used:
* `a800dumps/station.a8s` — live atari800 savestate, 5 frames into the Station cinematic
  (`RTCLOK_LOW $0014 = 5`, `VVBLKI $0222/3 = $1B30`, scroll pointer `$1C39/$1C3A = $B9B9`, i.e.
  exactly one scroll step done). **RAM base in the gunzipped `.a8s` is `0x86`**, but
  `tools/extract_a8s_ram.py` REFUSES it — its anchor is `sfx_seq_step $7148`, which is in the
  main block and is *not loaded yet* at Station time. Anchor on `station_init $195D` instead
  (`A9 00 8D 1D D0 8D 0E D4 8D 00 D4 8D 1A D0`, unique) and cross-check `$1B30`
  (`A9 35 A2 1C 8D 02 D4 8E 03 D4`).
* `a800dumps/logo.a8s` — same, mid-Logo (`RTCLOK_LOW = $6A`, `VVBLKI = $51EF`). Anchor on
  `stage_5000` (`A2 FF 86 90 E8 8E`), also base `0x86`.
* `rof.xex` segment payloads (`tools/xex_map.py`), and `disasm/listing.txt` for the code that
  survives into the final image.

> **The `docs/*-scene*.png` renders below are LOCAL artefacts** — the repo `.gitignore` covers
> `*.png`, like the `atari0NN.png` reference shots, so they are not committed and a fresh
> checkout will show broken image links. Regenerate them (or any GTIA-9 field) with: extract
> mem[] using the anchors above, then for each row emit `40` bytes as pairs of nibbles, each
> nibble `n` → Atari colour `hue | n` through `PlatformSDL.cpp`'s YIQ palette formula
> (`HUE_ANGLES`, `Y = 1.5·l² + 20.5·l + 4`, chroma 49). Station: base `$0600`, 122 rows,
> hue `$00`. Logo: base `$60A3`, 62 rows, hue `$10`.

---

## 0. The one structural fact that governs everything

`rof.xex` is a 20-segment load file and **the later segments overwrite the earlier ones.**
`disasm/listing.txt` (and therefore `rof_gen.c`) is a disassembly of the *final* image, so:

| Scene | Its code | Its data | In `listing.txt` / transpiled? |
|---|---|---|---|
| **Logo** | seg 5 `$5000-$536F` | inside that segment (stroke lists) + `$6000-$6A52` scratch | ❌ **No** — seg 16 (`$3CDE-$B7FF`) overwrites both `$5000` and `$6000` |
| **Station** | seg 8 `$195D-$1AA6` + seg 9 `$1B30-$283D` | seg 9 tables + the 122-row image | ✅ **Yes, fully** — nothing overwrites `$0B00-$283D` |

⚠ **`startup-flow.md` §2 item 1 is wrong** and so is `symbols.csv`'s `stage_5000` description.
INITAD `$5000` does **not** "prime GTIA from page-2 shadows" — that is the *game* routine that
later occupies `$5000`. The INITAD that runs at boot is **the Lucasfilm logo**, from segment 5.
Final image at `$5000` = `04 8D 09 D4 A5 D4 …`; segment 5 at `$5000` = `A2 FF 86 90 E8 8E …`.

Consequence: **the Station is ~90% already ported** (its transpiled routines are linked and the
$1B30 VBI is already dispatched); the **Logo needs new hand-written code**, because there is no
transpilation of it and there never will be from the current single-listing pipeline.

Both scenes are **ANTIC mode F under GTIA mode 9** — 80 fat pixels per line, 16 *luminance*
levels of one hue taken from `COLBK`. So on the Amiga both are **4 bitplanes, 320 px wide
(each GTIA pixel = 4 lores px), palette = 16 shades of one hue**. One decoder serves both.

---

## 1. Scene 1 — the Lucasfilm Games logo

![Lucasfilm Games logo, decoded from logo.a8s](logo-scene.png)

*(decoded from `a800dumps/logo.a8s` `$60A3`, 80×62 GTIA-9 pixels, hue 1 — 4× horizontal /
2× vertical for legibility)*

### 1.1 Screen

| | |
|---|---|
| Display list | `$6000`: 8 × `$70` (blank 8) = **64 blank lines**, then **62 × mode F** (`4F` LMS `$60A3`, then 61 × `$0F`), then `41 00 60` = JVB → `$6000` |
| Screen RAM | `$60A3`, 40 bytes/row, 62 rows = 2480 bytes |
| GTIA | `PRIOR $D01B = $41` → **GTIA mode 9** (16 luminances), priority 1 |
| Hue | `COLBK $D01A = $10` and `COLPF2 $D018 = $10` → **hue 1, gold** |
| DMACTL | `$D400 = $3A` → normal playfield + **player DMA**, one-line res, **no missile DMA** |
| PMBASE | `$D407 = $08` → `$0800`; P0 = `$0C00`. `GRACTL $D01D = $02` (players only) |
| DLI | none (no DL byte has bit 7; `VDSLST` is the OS default `$C055`) |
| VBI | `VVBLKI = $51EF` |

### 1.2 The code (segment 5, disassembled from the XEX)

* **`$5000` — the whole scene, as one blocking routine.** It:
  1. RLE-expands `$52E7` → `$6000` (`JSR $3C3D`). The RLE is only 11 bytes
     (`C8 70 / 4F A3 60 / FD 0F / 41 00 60 / C0`) and produces **only the display list** — the
     bitmap is *drawn*, not unpacked. (`$52F2-$536F` is dead filler: a stale copy of the routine's
     own code.)
  2. `DLISTL/H = $6000`, `DMACTL = $20` (**DL DMA only — playfield off, screen blank**).
  3. `VVBLKI = $51EF`.
  4. Builds a 64-entry row-pointer table in ZP `$007F`/`$00BF` = `$6053 + 40n`
     (so row index 2 = `$60A3` = screen row 0).
  5. Draws **"LUCASFILM"** — three passes of `JSR $5111` (stroke data at `$525F`, `Y=0`).
  6. `PMBASE = $08`, `PRIOR = $41`, `JSR $3C7B` (wait `VCOUNT ≥ $7A`), `COLBK = COLPF2 = $10`,
     **`DMACTL = $3A` → the finished logo is revealed all at once**, `IRQEN = $40`, `GRACTL = $02`.
  7. Copies 10 bytes `$5254-$525D` → ZP `$90-$99` (the VBI's state), builds the P0 sparkle shape
     (`$0C40-$0C4E = $10`, `$0C45-$0C49 = $38` → a 15-scanline "✦"), then **blocks on `$90`**.
  8. When `$90` hits 0, draws **"GAMES"** — two passes of `JSR $5117` (same stroke engine,
     `Y=$66`, row `$7B=$36`) — then **blocks until `$91` goes negative**.
  9. The sparkle: `HPOSP0 = $C1`, `COLPM0 = $08`, then `COLPM0 = $0F…$00` with `JSR $3CCA`
     (wait 2 frames) between each, while progressively zeroing/masking the P0 shape rows.
     16 steps × 2 frames = 32 frames. Then `HPOSP0 = 0`, **RTS** → the loader resumes.
* **`$5111`/`$5117` — the stroke plotter.** ⭐ Fully decoded 2026-08-13, and **verified**: a
  Python re-implementation of it reproduces `src/rof_logo_field.h` byte for byte (0 mismatches in
  2480 + 143). Kept as `tools/plot_logo_ref.py`, which is now the bake's reproducible oracle.

  **There is no bitmap in the file.** The picture is generated from **130 bytes** at
  `$525F-$52E0` plus a 7-entry pattern table — ~18× compression, which is why they plotted it.

  *Stroke list `$525F-$52E0`.* A stream of items, each one byte `PPPCCCCC`: `CCCCC` (1-31) is a
  repeat count; `PPP` = 1..7 selects `mem[$52DF + PPP]` as the pattern; `PPP` = 0 means **the next
  byte is a literal 8-bit pattern**; a whole byte of `$00` ends the strip. The table is
  `$52DF: EF 00 0C 78 8C C0 C4 FC` — slot 0 (`$EF`) is unreachable *as a table entry* (`PPP`=0
  means literal), and it isn't wasted: **the stroke list deliberately overruns into the table**,
  taking GAMES' last literal from `$52DF` and its terminator from `$52E0` (= pattern 1 = `$00`).

  *Geometry.* Each pattern byte is one **cell**: 8 px wide (LSB = rightmost), **2 scanlines
  tall**, and it advances the row cursor by 2 (`$51A3 INY INY`) — so a repeat count draws a
  vertical stroke. `$5111` is **9 strips of 8 columns** (cols 4-76), each exactly **30 cells = 60
  scanlines**; `$5117` is **4 strips × 5 cells** at cols 19-51, rows 49-60. The stroke index runs
  *continuously* across strips, so the `$00` terminators ARE the strip boundaries.
  ⚠ Two fall-throughs decide the whole shape and are easy to misread as `RTS`-terminated:
  **`$51BB` falls into `$51C0`**, so it plots rows Y-1 AND Y; and **`$51A8`'s `DEX` falls into
  `$51BB`**, making the "fat" brush a full 2-column × 4-row block. Read either as ending in `RTS`
  and you get a logo with every other scanline blank.

  *The emboss — three passes, three shade formulas.* `$7D` counts 2→1→0 across the three
  `JSR $5111` calls and selects the luminance ($51C0-$51EE is the read-modify-write nibble poke;
  the carry out of `LSR` picks high vs low nibble):

  | pass | `$7D` | origin | brush | shade at row index `i` |
  |---|---|---|---|---|
  | 1 | 2 | col 5, row 4 | fat 2×4 | constant **2** → drop shadow, offset +1px/+1row |
  | 2 | 1 | col 4, row 3 | fat 2×4 | `i>>2` (band 8 forced to 7) → outline |
  | 3 | 0 | col 4, row 3 | thin 2×1 | `((i-2)>>2) ^ 15` → the bright core |

  Passes 2 and 3 are *complementary* ramps — that is what makes it read as chrome: at the top the
  glyph is `$F` with a near-black outline, at the bottom `$3` with a bright `$D` outline. The
  `==8 → 7` special case is exactly the crossover band where the two ramps meet; forcing it makes
  outline equal core there and hides the seam. GAMES gets only two passes (both thin, `$78` is 1
  by then): shade 2 shadow, then `i>>2` = luminance 13→15.

  *One detail worth keeping:* the `$7F`/`$BF` pointer table's base is `$6053` = the screen minus
  two rows. That slack is deliberate — the fat brush reaches 3 scanlines above its cell, and on
  the first cell that lands in the harmless gap between the display list (`$6000-$604A`) and the
  screen.
* **`$51EF` — the VBI.** `INC $14`; then a two-phase audio sweep:
  * while `$90 != 0`: `JSR $523C` (writes `AUDC1-4` from `$92>>3 | $A0`), `INC $92`, then for
    `X = 6,4,2,0`: `AUDF(x) = mem[$93+x]`, `mem[$93+x] += mem[$5258+x]`, `DEC $90`.
    Seeds: `$93/$95/$97/$99 = $2A/$22/$00/$87`, steps `$01/$02/$03/$00`.
  * when `$90 == 0`: count `$91` down from `$7F`; at 0 set `AUDCTL = $60`, `AUDF1 = $FA`,
    `AUDF3 = $FF`, `$92 = $24`; thereafter `JSR $523C` + `DEC $92`, and `DEC $90` when `$92` wraps.
  * `JMP $E462` (XITVBV).
* Timing: **≈86 + 128 + 32 ≈ 250 frames ≈ 4-5 s.**

### 1.3 Amiga plan for the Logo

**✅ DECIDED (user, 2026-08-12): bake the bitmap, hand-write the sequencer + VBI.**
The stroke plotter is NOT ported.

The plotter runs with **playfield DMA off** — the drawing process is *never visible*. So a baked
bitmap is not an approximation, it is bit-identical to what the player sees. Take the finished
2480 bytes from `logo.a8s` `$60A3-$6A52`, ship as `amiga/assets/logo.raw` (incbin), decode with
the shared GTIA-9 → 4-bitplane routine at scene init (~10 KB chip, ~15 ms one-off).
*(Rejected alternative, recorded so it isn't re-litigated: port the `$5111` stroke plotter. It
would be ~180 lines with the same 2480 bytes as a byte-exact oracle, but it buys nothing the
player can see. `make validate` cannot reach it either way — the transpiler cannot see segment 5.)*

**How to produce `logo.raw`:** extract mem[] from `a800dumps/logo.a8s` (anchor on
`A2 FF 86 90 E8 8E` at `$5000`, RAM base `0x86` — see §0), take `$60A3 .. $60A3+2479`, write it
out verbatim. Keep it as the raw GTIA-9 nibble field rather than pre-converted bitplanes so the
one shared decoder (§1.3 table) handles both scenes and the asset stays inspectable.
⚠ Do **not** regenerate it from a screenshot — the dump is the byte-exact source.

### 1.4 HAND-OFF: what is done and what is left (2026-08-12)

**DONE and committed:**

| Piece | Where |
|---|---|
| The baked bitmap, **both phases** | `src/rof_logo_field.h` (generated), `tools/gen_logo_field.py` |
| The copper list (4bp, 16 gold luminances, 64 blank lines + 62 rows) | `Gtia9CopperList`, already parameterised — `buildLayout(field, kLogoTopLines=64, kLogoRows=62, kGtia9Pal1, nullSprite)` |
| The GTIA-9 → 4-plane decoder + the shared 320×340 field bitmap | `RescueOnFractalus.cpp` `gtia9Row()` / `bootFieldBitmap` |
| Scene routing | `rof_boot_chain()` sets `g_bootScene = ROF_BOOTSCENE_LOGO` around `rof_logo_run()`; `renderBootScene()` already has the logo branch and builds the logo layout |
| The stub to fill in | `src/rof_logo.c` — `rof_logo_run()` returns immediately, so the chain currently behaves as `ROF_BOOT_STATION` |

**TWO PHASES — the reveal is not the end of the drawing.** `$5000` draws "LUCASFILM", reveals it
(DMACTL `$3A`), blocks 86 frames on ZP `$90`, and only THEN draws "GAMES".  Both states are
captured from real savestates and differ in exactly one **11 × 13-byte rectangle** (rows 50-60,
bytes 13-25), so `rof_logo_field.h` carries the LUCASFILM field whole plus that overlay
(`ROF_LOGO_GAMES_ROW/COL/ROWS/COLS`).  Paste the overlay into mem[] at the GAMES cue and mark the
field dirty so `renderBootScene` re-decodes those rows.

**`$51EF` — the VBI, disassembled from segment 5 (verified against `lucasfilm.a8s`: at RTCLOK
`$14` its four AUDF accumulators are exactly their `$5254` seeds plus 12 steps of 1/2/3/0):**

```
INC $14                                  ; it increments RTCLOK itself — it REPLACES the OS vblank
if ($90 != 0):                           ; phase 1, 86 frames ($90 seeded $56)
    if ($90 & $80) -> XITVBV             ; ...and idles forever once $90 wraps to $FF
    audc_sweep(); $92++
    for X in 6,4,2,0: AUDF(X) = $93+X ; $93+X += step[X]     ; steps $5258+X = 01 02 03 00
    $90--
else if ($91 & $80):                     ; phase 3, 36 frames
    audc_sweep(); if (--$92 == 0) $90--  ; -> $90 = $FF, silent
else:                                    ; phase 2, 127 frames of silence
    if (--$91 == 0): AUDCTL=$60; AUDF1=$FA; AUDF3=$FF; $92=$24
JMP $E462 (XITVBV)

audc_sweep ($523C):  A = ($92 >> 3) | $A0
    AUDC1 = A ; AUDC2 = A ; A += 0 (carry!) ; AUDC3 = A ; AUDC4 = A ^ $60
```
ZP `$90..$99` ← `$5254..$525D` = `56 7F 00 2A 01 22 02 00 03 87`
(so `$90`=$56, `$91`=$7F, `$92`=0, and the AUDF seeds `$93/$95/$97/$99` = `2A/22/00/87`).

**`$5000` — the sequencer, disassembled from segment 5.** Everything it calls already exists:
`rle_decompress()` (`$3C3D`), `wait_vcount_ge_7a()` (`$3C7B`, a no-op here), `wait_frames_2()`
(`$3CCA`).  Steps, in order:
1. `$90 = $FF`; AUDCTL = 0; NMIEN = 0; `$78 = $BD = 0`.
2. `$BB/$BC = $52E7`, `$BD/$BE = $6000`; **`rle_decompress()`** → the display list (8 × `$70`,
   `4F A3 60`, 61 × `$0F`, `41 00 60` = JVB `$6000`; verified byte-for-byte in the dump).
3. DLISTL/H = `$6000`; **DMACTL = `$20`** (DL DMA only — the drawing is invisible, which is what
   makes baking faithful).
4. **VVBLKI = `$51EF`.**
5. Build the 64-entry row-pointer table `$7F`/`$BF` = `$6053 + 40n`. *(Not needed — that is the
   stroke plotter's addressing.)*
6. Three `JSR $5111` passes = "LUCASFILM". **→ paste `kLogoField` to `$60A3` instead.**
7. PMBASE = `$08` (P0 at `$0C00`); PRIOR = `$41`; `wait_vcount_ge_7a()`; COLBK = COLPF2 = `$10`;
   **DMACTL = `$3A` (the reveal)**; NMIEN = `$40`; GRACTL = `$02`.
8. Copy `$5254-$525D` → ZP `$90-$99`; fill P0 rows `$0C40-$0C4E` = `$10` and `$0C45-$0C49` = `$38`
   (the 15-scanline "✦").
9. **Block while `$90 != 0`** (86 frames — `platform_tick_vbi(); platform_render_frame();`).
10. Two `JSR $5117` passes = "GAMES". **→ paste `kLogoGames` instead.**
11. **Block while `$91` is positive** (127 frames).
12. The sparkle: `$80 = $FF`, `$81 = $FE`, P0 row `$0C47 = $FE`, HPOSP0 = `$C1`, COLPM0 = `$08`,
    `wait_frames_2()`; then `for Y = $0F down to 0`: COLPM0 = Y, `wait_frames_2()`, zero P0 rows
    `$0C40+Y` and `$0C40+$82` (`$82` starts `$FF`, `$82++` each pass), and while `$82 <= 3` also
    `for X = 4..0: $0C45+X &= $80 & $81` then `LSR $80; ASL $81` — an erosion from both ends and
    inward.  16 × 2 = 32 frames.  Finally HPOSP0 = 0, **RTS**.

**Amiga specifics still to write:**
* `bus_write($D000-$D007)` is now a mem[] shadow (added for the station's missiles), so HPOSP0 is
  readable; **COLPM0 (`$D012`) is NOT** — expose it as a `g_logoSparkleCol` global from
  `rof_logo.c` (the same trick the station avoids by reading `pmg_colors_station`'s source table).
* The sparkle is one sprite on ch0 (pair 0 → COLOR17).  Read the P0 page at `$0C00` with the same
  `pmRun()` + `kDoubleGlyph` mirror the station uses; buffer offset → raster line is
  `kDisplayTop + off - 8`, X is `0x81 + (hpos - 0x32) * 2` → `$C1` = 415.
* `game_vbi_isr` needs a `vbi == 0x51EF -> rof_logo_vbi()` case (the boot-scene branch there
  currently falls through to inert), and `rof_logo_run()` should
  `platform_register_vbi(0x51EF, rof_logo_vbi)` for SDL.
* `make LOGO_START=1` (`ROF_LOGO_START_ABORT`, already in the Makefile) should let START abort the
  wait loops — OFF by default, since the Atari's `$5000` is unconditional.

Total timing: **86 + 127 + 36-ish + 32 ≈ 280 frames ≈ 5.6 s** (the plan's earlier ≈250 estimate
missed that phase 2 is 127 frames of silence, not 128 of sound).

### 1.5 SHIPPED 2026-08-13 — what was written, and how it was verified

`src/rof_logo.c` is the whole scene: `rof_logo_run()` (the `$5000` sequencer) and
`rof_logo_vbi()` (the `$51EF` VBI), both hand-written from the segment-5 disassembly with the
`$5000`-page address on every step. It reads its seeds/steps out of **mem[] at `$5254`/`$5258`**
rather than transcribing them — segment 5 is resident while the logo runs — and calls the genuine
`rle_decompress()` / `wait_vcount_ge_7a()` / `wait_frames_2()`. Only the two plot passes are
replaced (by pasting `kLogoField` / `kLogoGames`); §1.4's steps 1-12 are otherwise all there,
including the `$5037` pointer table (dead here, but it is `$5000`'s own work and ZP survives into
the next stage) and the sparkle's write to `$0D3F` on its first erosion pass.

Supporting changes: `g_logoFieldGen` / `g_logoSparkleCol` (rof_boot.h) · a `$51EF` case in
`game_vbi_isr` · `decodeLogoField()` + `buildLogoSparkle()` + `logoVblankUpdate()` and a
`logoSparkle` sprite in the renderer · `platform_register_vbi(0x51EF, …)` for SDL.

**One real bug, found by measuring:** `PlatformAmiga::renderFrame` owns RTCLOK for every VVBLKI
whose own body does not advance it — a list that had `$1B30` and `$4FF5` on it. `$51EF` advances
its own (faithfully: VVBLKI *replaces* the OS stage-1), so RTCLOK ran at **2 per vblank** and the
sparkle's `wait_frames_2` pacing came out at 2× — the whole 32-frame fade in 16. `$51EF` is now on
that exclusion list. Nothing else in the scene noticed, because the two long holds spin on ZP
`$90`/`$91`, which only the ISR moves.

**Verified headlessly** (`amiga/logo_probe.gdb` + `amiga/logo_sparkle.gdb`, both usable on a plain
`make` build — ⚠ `PROBES=1` would imply `SKIPBOOT=1` and skip the scene under test):

| Checked | Result |
|---|---|
| Display list unpacked from `$52E7` | `70×8 / 4F A3 60 / 0F×61 / 41 00 60` ✓ |
| Field pasted, both phases | row 0 = `00 00 ff ff f0 2f ff f0 …` and the GAMES rect rows 50/55/60 all byte-exact vs `rof_logo_field.h` ✓ |
| GTIA-9 decode + row stride | source nibble → pen across 4 planes, checked at rows 0, 60 and the last row 61 ✓ |
| Copper geometry | WIN_WAIT line 107, BOT_WAIT line 169 (64 blank + 62 rows) ✓ |
| The `$51EF` sweep's arithmetic | at 39 frames in, `$93/$95/$97/$99` = seed + 39×(1/2/3/0) exactly ✓ |
| The sparkle | 2 frames/step ✓, COLPM0 `$0F`→`$00` ✓, shape eroded from both ends with the `$0C45-$0C49` mask closing in ✓, sprite at x=415 y=100, VSTART/VSTOP shrinking one row per end per step, disarmed when the page empties ✓ |
| Whole chain | Logo → Station → game, `g_bootScene` 1 → 2 → 0 ✓ |

The sparkle's **placement** was the one thing headless verification could not settle (`pmX`/`pmLine`
are the station's constants, which transfer exactly — `$3A` and the station's `$3E` differ only in
missile DMA, so the playfield width bits are identical). Looked at on screen 2026-08-13: the line is
right, and the column wanted **+3 lores px** — `kLogoSparkleXAdj`, applied on top of `pmX(HPOSP0)` in
`buildLogoSparkle`. It is the only boot-scene element whose position is judged against *baked
artwork* rather than against another mem[]-derived element, so it is the only one with a trim.

### 1.6 The Logo→Station handover — blank before rebuilding the shared list (fixed 2026-08-13)

**Symptom (user):** the station's initial render appeared *on top of* the Lucasfilm logo.

**Cause.** Both scenes share ONE `Gtia9CopperList` and ONE field bitmap (the deviation recorded at
the top of this file). `renderBootScene`'s scene-entry branch therefore **rewrote the very list the
copper was executing** — geometry, palette, all four bitplane pointers, every sprite operand — and
then `decodeStationField` overwrote the very bitmap it was fetching. Nothing else covered that
window: renderFrame's black-until-ready hold sits *below* the boot-scene branch, and
`rof_boot_chain`'s stage loads render no frames at all, so the logo's list simply stayed live across
the gap. Measured with **`amiga/boot_gap.gdb`** (breakpoint-driven — a wall-clock delay cannot hit
this window):

```
buildLayout  vbi= 73  bootScene=1 prevScene=0 bootInst=0 emptyInst=1 top=64 rows=62   logo: safe
install bootField vbi=76                                                             (decode was behind black)
buildLayout  vbi=337  bootScene=2 prevScene=1 bootInst=1 emptyInst=0 top= 8 rows=192  ⚠ LIVE list
decodeStation IN  vbi=338                                                            ~70 ms, on screen
```

`bootInst=1` is the whole bug: the list being rewritten was the live one. Geometry jumped 64→8
leading lines and the palette gold→grey while the bitmap still held the logo, and the station's
picture then painted itself in over ~4 displayed frames. It is also exactly the mid-frame
bitplane-pointer swap `CLAUDE.md`'s copper rule forbids.

**Fix.** The entry branch installs the black `EmptyCopperList` and returns; the rebuild happens on
the *next* call, when nothing is displaying either resource. `PlatformAmiga::renderFrame` waits for a
real VBI on its way out, which is where `COP1LC` latches, so one frame is all the handshake needs —
no explicit beam wait. `bootFieldCopperInstalled` is cleared with it, which also parks
`stationVblankUpdate`/`logoVblankUpdate` so the ISR cannot poke a half-rebuilt list. The Logo's own
entry skips the step (the `EmptyCopperList` from `initialize()` is already live), so it costs the
Logo nothing. Same run after the fix:

```
install empty(pre) vbi=337  bootScene=2 prevScene=1
buildLayout        vbi=338  bootScene=2 prevScene=1 bootInst=0 emptyInst=1 top=8 rows=192
```

≈5 frames (~110 ms) of black now separate the two scenes — which is also the **faithful** answer:
`station_init` ($195D) opens by writing `DMACTL = 0` and does not reveal the field until it is built.

---

## 2. Scene 2 — the space-station cinematic

| the 122-row station image (`$0600`, stride 40) | the composed frame at the END of the scroll (`ptr = $B800`, 192 rows) |
|---|---|
| ![station image](station-scene.png) | ![final frame](station-scene-final-frame.png) |

*(both decoded from `a800dumps/station.a8s` as GTIA-9, hue 0)*

### 2.1 Screen

| | |
|---|---|
| Display list | starts at **`$1C35`**: `70 60 70` = **23 blank scanlines** (blank-8 + blank-**7** + blank-8), then `01 xx xx` = **JMP to the moving window start** (`mem[$1C39/$1C3A]`). ⚠ An earlier version of this table said "one `$70`" — that cost 15 lines of PMG misalignment, see §2.4a |
| DL body | built at **`$B800`**: **122** mode-F entries LMS `$0600 + 40n` (the station image) followed by **218** mode-F entries that are *mostly* the shared blank row **`$2C90`**, with up to 30 of them (1-in-8, `RANDOM`) pointing at their own row `$2CB8 + 40k` — **the stars** |
| Terminator | a `41 35 1C` (JVB → `$1C35`) that is **moved down 3 bytes per scroll step**, so the visible window is always exactly **192 mode-F rows** (`$0240 / 3`) |
| GTIA | `PRIOR $D01B = $71` → **GTIA mode 9**, priority 1, multi-colour players, **5th player** (missiles = `COLPF3`) |
| Hue | the `$1B30` VBI writes `COLBK $D01A = 0` every frame → **hue 0, greyscale** |
| DMACTL | `$D400 = $3E` → normal playfield, **player + missile DMA**, one-line res |
| PMBASE | `$D407 = $30` → `$3000`; missiles `$3300`, P0 `$3400`, P1 `$3500`, P2 `$3600`, P3 `$3700` |
| Fixed colours | `COLPF3 $D019 = $34`, `COLPM0 $D012 = $06`, `COLPM1 $D013 = $0A`, `HPOSP0 = HPOSP1 = $7F` |
| DLI | **none** — no DL byte has bit 7 set and `VDSLST` is the OS default `$C0CE`. ⚠ `$1E01` is named `station_pm_shape_tick` but is **not a DLI** (see §4) |
| VBI | `VVBLKI = $1B30` — writes `DLISTL/H = $1C35`, `COLBK = 0`, `INC $0080`, `INC RTCLOK` |

### 2.2 The scroll — the heart of the scene

`mem[$1C39/$1C3A]` is the operand of the DL's `JMP`. It starts at **`$B9BC`** (26 rows into the
star section) and `display_scroll ($1CF7)` **decrements it by 3 per step**, so one *new row* is
revealed at the top each step and the whole picture slides down. It stops at **`$B800`** —
exactly **148 steps** (`$B9BC - $B800 = $1BC = 148 × 3`), which is why the phase counter
`$008B` terminates at `$94 = 148`.

At the same time it writes the 3-byte JVB (`$1C3B-$1C3D`) to `ptr + $0240`, keeping the window
at 192 rows. So: **the station image scrolls down out of the starfield and ends up occupying the
top 122 rows.**

Pacing (`station_anim_frame $1D9A` + hold-time table `$1DE2` = `06 04 03 03 02 02 01 02 01 02 01
01 02 01 01 01 01 02`):
* steps 0-17 — ease **in** (table forwards),
* steps 18-129 — 1 frame/step (constant),
* steps 130-147 (`$008B ≥ $82`) — ease **out** (table backwards).
≈ 36 + 112 + 36 = **≈184 frames ≈ 3.7 s**, then it idles.

Exit (`station_loop $1A01`): `RTCLOK_MID $0013 ≥ 4` (≈17 s NTSC), or `CH $02FC != $FF`
(any key), or `CONSOL $D01F == $06` (START). `station_exit $1A2F` then tail-calls
`screen_page_swap` and returns.

### 2.3 The moving parts, per frame

| Routine | What it really does |
|---|---|
| `station_audio $1B5B` | 4-channel POKEY sweep, phase-gated on `RTCLOK_MID`; at phase 3 it forces `AUDF2` to 0 and plays an **`AUDC2`** envelope from the RLE-expanded table at **`$283E`** (228 bytes, unpacked from `$1BF4` by `station_init`), then tail-jumps to `$1E01`. ⚠ Four v5.0 audio fixes are grafted onto this routine and onto `station_init`/`station_exit` — `docs/rom-v50-diff.md` §4.8 |
| `$1E01` (+`$1E2A`) | animates the **P0/P1 shapes** at `$3400`/`$3500` from the table set `$272C/$2739/$2746/$2753/$2760/$277A/$276D` — a 13-frame shape cycle. Not a DLI |
| `$1910` | drifts `HPOSM0-3` (`$D004-$D007`); called from `station_audio` |
| `station_anim_frame $1D9A` | the scroll pacer (above) → `display_scroll` |
| `display_scroll $1CF7` | scroll pointer, JVB move, **plus** the two PMG paints below |
| `station_sub_1EB4 $1EB4` | every 3rd frame: copies a **102-byte vertical stripe** (one byte per row, stride 40) from one of 8 frames at `$1FE3 + 0x66·k` (tables `$2313` hi / `$231B` lo) into the image at **`$077A`** (fwd) or **`$077C`** (rev) — column 18 / 20, rows 9-110. An 8-frame animation of two 2-pixel-wide vertical strips |
| `station_sub_1F48 $1F48` → `$1F51` | walks a **6-entry linked list of animation channels** at `$2603` (15-byte records). Measured chain: |

```
 ch  X    reload cyc  bytes×rows  src(reset→cur)   dest      image row,col
  0  $00    04    07     7 × 12   $2323 → $2377   $0C36      row 39 col 30
  1  $0F    04    07     7 × 12   $2323 → $2377   $110E      row 70 col 30
  2  $1E    03    05     2 ×  7   $256F → $257D   $111E      row 71 col  6
  3  $2D    06    04     1 ×  7   $25B5 → $25B5   $09C6      row 24 col  6
  4  $3C    09    05     2 ×  1   $25D1 → $25D1   $1505      row 96 col  5
  5  $4B    09    05     2 ×  4   $25DB → $25DB   $0D86      row 48 col  6
```

(dest stride is 40 bytes/row; `$260B < 0` means "reload the timer from POKEY `RANDOM`".)
Total mutated image bytes per frame: **< 200**.

### 2.4 PMG

* **`station_star_fade_in $1E79` is NOT a PMG routine** — see §4. It walks `$2CB8-$3167`
  (the *star rows*) and brightens each non-zero nibble by one luminance step, **14 times, one
  frame apart** (`JSR $3CC3` per pass). Star seeds are `$1C3E/$1C3F = $10` and `$01`
  → after 14 passes they are `$F0`/`$0F` = full brightness. **It is the starfield fade-in**, run
  once from `station_init $19F4` before the loop. The current Amiga native list drops it as
  "only modifies PMG RAM, not displayed" — that is wrong, and without it the stars stay at
  luminance 1 (nearly invisible).
* `pmg_colors_station $1F0B` — every 7 frames, steps an 8-entry cycle:
  `HPOSP2 ← $1F30[i]` (`42 46 4A 4E 52 56 5A 5E`), `HPOSP3 ← $1F38[i]`
  (`B8 B4 B0 AC A8 A4 A0 9C`), `COLPM2 = COLPM3 ← $1F40[i]` (`48 38 28 18 F8 E8 D8 C8`).
  Two coloured dots converging from the sides. (The `symbols.csv` description says `COLPF3`;
  it is `COLPM2/3`.)
* `display_scroll` PMG paints — **RESTORED 2026-08-12** (they were dropped by the native twin
  in `rof_manual.c`; both are confirmed on screen in the SDL render).  ⚠ The scanline ranges in
  the first version of this section were wrong — the guards are `BMI` then `CMP #$20 / BCC`, so
  each element is live while its index is in **`[$20,$7F]`**, not `< $20`:
  * X = `$008B - $39` (the 6502 does `SEC/SBC #$94` then `CLC/ADC #$5B`); while X ∈ `[$20,$7F]`
    — i.e. **steps 89..148, scanlines 32..91** — writes `$C0` to `$3600+X … $3602+X` and
    `$3700+X … $3702+X` and clears row X-1 → a **3-scanline, 2-px blob on P2 and P3 that walks
    down one scanline per scroll step**.
  * for `Y = 7..0`: `X = $008B - $94 + $1D8A[Y]` (offsets `4D 69 57 62 3A 5C 7C 6D`); if
    X ∈ `[$20,$7F]`, writes `$1D92[Y]` (`C3 03 0C 0C 30 30 30 C0`) to `$3300+X` and clears
    `$32FF+X` → **eight single-scanline missile dots** (the 5th player, `COLPF3 = $34`) at
    scanlines ~58..124, each with its own phase offset.  The pattern byte picks WHICH missiles
    (2 bits each), hence which `HPOSMn` column the dot appears in.
* `display_scroll`'s **DL-ring JVB move** ($1D19-$1D3C) is restored too: while `ptr < $B9BC` it
  stages `ptr + $0240` in ZP `$81/$82` and copies the 3-byte JVB (`$1C3B-$1C3D` = `41 35 1C`)
  there, holding the window at exactly 192 rows.  It always lands one row BELOW the new window,
  so it never overwrites a row that is still displayed — which is why the Amiga's decode-once
  bitmap does not have to care about it.

### 2.4a The three PMG bugs found on screen 2026-08-13 (all fixed)

The scene had been signed off, and then three separate things showed up next to each other.

**1. Every element 3 lores px left of where it belongs.** A flat trim, and the *logo's* sparkle
wanted exactly the same 3 px, so it is not an element's constant but the shared HPOS→Amiga-X
mapping: the port's playfield left edge sits 3 px off the `$81` `pmX()` assumes. Folded into
`pmX()` in `RescueOnFractalus.cpp` and the logo's old `kLogoSparkleXAdj` deleted (net effect on the
sparkle: nil, it keeps the +3 the user already confirmed).

**2. Every station element 16 scanlines too low — and the cause is the display-list table above.**
`$1C35` is `70 60 70`, i.e. **23** leading blank scanlines, but `kStationTopLines` is 8, so the
station's *picture* is drawn 15 lines higher than its PMG. The anchor that proves which of the two
is wrong is the **logo**: its DL genuinely is `8 × $70` = 64, `kLogoTopLines` = 64 agrees, and the
sparkle's line is confirmed on screen — so `pmLine()`'s `off - 8` is right and the station's layout
is what disagrees. (That anchor also pins ANTIC's display-list start at absolute scanline 8:
64 + 8 = the 72 `pmLine()` implies.) Fixed with `kStationPmYAdj`, subtracted from every station
element by `pmLineStation()`, rather than by raising `kStationTopLines` to 23 — which would move
the whole already-accepted composition down and push region C's copper `WAIT` to line 258, past the
8-bit vertical compare. The constant is **16**, the value measured on screen; the derivation gives
15 and the odd line is inside the uncertainty of the PM-buffer-index-vs-scanline offset.
⚠ It is scene-wide, so it moves the spacecraft too — user-confirmed separately, and unavoidable
since that element is placed through the same `$277A` → `pmLine()` path.

**3. The spacecraft's fuselage was dark grey where atari800 shows near-white** (`#4f4f4f` vs
`#d3d3d3`). **`station_init` writes PRIOR `$71` and bit 5 is GTIA's multi-colour-player enable**:
where P0 and P1 overlap the output is `COLPM0 | COLPM1` = `$06 | $0A` = **`$0E`**, brighter than
either. The ship's centre column is set in *both* pages — that column IS the fuselage — so
mirroring P0 and P1 onto two separate sprite channels painted it in the higher-priority channel's
`$06`.

⭐ **The fix is that an Amiga sprite is ALREADY two bitplanes** — the two words per line are
`SPRxDATA` and `SPRxDATB`, and the pen is `DATB<<1 | DATA` → COLOR17/18/19 for pair 0. Every other
PMG mirror in the port writes `glyph, 0`, i.e. plane A only, which is why each gets one colour and
why `setPairColor` only ever set colour 1. Put **P0 in plane A and P1 in plane B of ONE sprite** and
the three pens are COLPM0 / COLPM1 / their OR — the multi-colour player exactly, on channel 0
alone. `stationSpr[]` therefore shrank to 3 (ship, P2 dot, P3 dot) and channel 1 is a null sprite.

⚠ **Do NOT reach for an attached pair.** That was tried first and is wrong: ATT makes the two
channels **four** planes, so P1 alone lands on pen 4 = COLOR20 (which nothing in this list sets) and
the overlap on pen 5 = COLOR21 — which *is* set, to the converging dots' cycling colour. The
headless probe could not see it: the control words, the ATT bit and COLOR17/18/19 were all exactly
as intended, and only the on-screen colours said otherwise. The reference the user supplied (a
14×17 px capture = 7 Atari px × 17 scanlines, i.e. the frame-12 shape at 2× horizontal) decoded to
`#4f4f4f` / `#898989` / `#d3d3d3` = `$06` / `$0A` / `$0E`, matching the `$272C` table decode row for
row — which is what pinned the pen assignment.

One sprite now carrying both players forces the run to be taken over **P0 | P1**, not P0 alone,
which fixed a latent bug: `pmRun()` stops at the first zero byte, and **8 of the 13 shape frames
have an interior P0-only zero row** (frame 12's row 14) — those were silently truncating the ship —
while frames 3 and 4 *start* with a P1-only row, so the shape was also drawn one scanline high. The
union has no interior hole in any frame.

Verified headlessly with `amiga/station_sprites.gdb` at 31 s (the ship only launches in
`station_audio`'s RTCLOK_MID phase 3, the last ~2 s of the scene — a 15 s or 20 s sample shows
empty PM pages and reads as "no ship"):

```
ship ch0 POS=338f CTL=3500  ATT=0
ship  y=51  x=286  rows=2      p0: [1f]=10   p1: [1e]=10 [1f]=38   <- union starts at $1E, P0 at $1F
  row00  A=0000 B=0300  overlap=0000   P1 alone   -> pen 2 = COLOR18 $0A
  row01  A=0300 B=0fc0  overlap=0300   fuselage   -> pen 3 = COLOR19 $0E, pen 2 either side
pair0 col1/2/3: 0555 0888 0ddd    = atariToOCS($06 / $0A / $0E)
dot[2] y=111 x=204   dot[3] y=111 x=360        (phase $94 -> pmLineStation($5B) = 111)
msl y=78,97,112,129,144  x=140/432             (HPOSM0/1 parked at 0, M2 $C8, M3 $36)
```

### 2.5 What already exists in the port

Good news — most of it:

* `station_init ($195D)`, `display_list_build ($1C40)`, `station_audio ($1B5B)`,
  `station_star_fade_in ($1E79)`, `pmg_colors_station ($1F0B)`, `vbi_handler_station ($1B30)`,
  `initad_1A97`, `rle_decompress ($3C3D)`, `wait_timer_4c_frames ($3CB2)` are all **transpiled**
  in `src/gen/rof_gen.c`.
* `screen_page_swap ($1A62)` is hand-written in `rof_manual.c` (the 6502 is self-modifying).
* `station_anim_frame`, `station_sub_1EB4`, `station_sub_1F48` have **native twins** in
  `rof_manual.c` (moved there from the old `station_native.cpp`).
* **`SPINWAIT_HOOKS[0x1A18]`** already drives a real Amiga frame per loop iteration, and
  `[0x3CB8]` covers the fade-in's frame waits.
* `game_vbi_isr` **already dispatches `VVBLKI == $1B30` to `vbi_handler_station()`**.
* `os_setvbv`/`os_xitvbv` are no-op stubs.

So `initad_1A97()` is a **single, faithful entry point for the whole cinematic**; it returns when
the player presses START (F1) or the ~17 s timeout expires.

Two defects to fix first:

1. **`rof_manual.c`'s `station_display_scroll()` drops the DL-ring JVB move and both PMG paints.**
   The JVB move is only needed if the renderer walks the Atari DL (it can instead derive
   "192 rows from `mem[$1C39/$1C3A]`"), but the P2/P3 + missile paints are real content. This is a
   **shared** file, so the SDL build is missing them too.
2. ✅ **DONE 2026-08-13** — `rof_native_amiga.cpp`'s dead `station_*_native` block was stale:
   unreferenced, duplicating the twins now in `rof_manual.c`, and `station_setup()` contained a
   hack that force-wrote `$2313`/`$231B` to all-`$88`. That hack was for the old flat
   `rof_mem.bin` snapshot; in the pristine XEX those tables are already correct
   (`$1FE3, $2049, $20AF, $2115, $217B, $21E1, $2247, $22AD`). **Deleted, not revived** — the
   file now carries only a note in its place. `rof_native.h` went with it (nothing left used it),
   and `PlatformAmiga.cpp`'s comment now names `station_init`'s `$1A0E` poll rather than the
   deleted `station_poll_start_native`.

### 2.6 Amiga render plan for the Station

**Tall-bitmap + pointer scroll.** The Atari scrolls by moving a DL pointer; the Amiga can scroll
by moving bitplane pointers, which makes the scroll free.

* **One 4bp bitmap, 320 × 340 rows** = 340 × 40 × 4 = **54,400 bytes chip**.
  (Current runtime chip use is ~158 KB of 1 MB — comfortable; see `docs/perf-budget.md`.)
  Row *i* of the bitmap = DL entry *i* at `$B800 + 3i`; decode 40 bytes from that entry's LMS.
  Blank rows are simply left cleared.
* **Decode once** after `display_list_build` (~54 k table lookups ≈ 70 ms, one-off, invisible).
* **Per frame**: re-decode only what changed —
  * the 24-ish star rows while the fade-in runs (24 × 40 B; diff in longs = 240 compares),
  * the dirty station-image rows (< 200 bytes) — cheapest via dirty-range hooks in the two
    twins we already own (`station_sub_1EB4`, `station_chan_step`).
  Total per-frame cost ≈ 1 ms. This scene is nowhere near the flight budget.
* **Scroll**: `startRow = (mem[$1C39] | mem[$1C3A]<<8) - $B800) / 3`; set the four plane pointers
  to `data + startRow*160 + plane*40`. ⚠ **In the VBI ISR only** — copper bitplane-pointer swaps
  mid-frame garbage the frame (`amiga-copper-lessons`).
* **Palette**: 16 greys, `atariToOCS(lum)`; pen 0 = `COLBK` = 0.
* **Sprites**:
  * P2 + P3 blobs → 2 Amiga sprites. `COLPM2/3` and `HPOSP2/3` are written straight to GTIA
    (`bus_write` drops them on Amiga), so read them from the source tables:
    `i = mem[$0097] & 7`, `x = 0x81 + (mem[$1F30 + i] - 0x32) * 2`, colour
    `atariToOCS(mem[$1F40 + i])`.
  * P0/P1 (the `$1E2A` shape cycle at `$3400`/`$3500`, `HPOS = $7F`, `COLPM0 = $06`,
    `COLPM1 = $0A`) → **ONE sprite, one player per bitplane** (P0 in `SPRxDATA`, P1 in `SPRxDATB`),
    not 2 separate sprites and not an attached pair: PRIOR `$71` enables multi-colour players, so
    the overlap must come out `COLPM0 | COLPM1` (§2.4a).
  * The 8 missile dots (5th player, `COLPF3 = $34`) → 1-2 more sprites, or defer as polish.
* **Audio**: `station_audio` already routes POKEY → Paula. ⚠ Check that the standby SFX tick
  (`sfx_voice_tick`, driven from the VBI body) is inert while `VVBLKI == $1B30`, or it will
  fight `station_audio` for the Paula channels.
* **Input**: F1 = START already maps to `CONSOL $D01F` bit 0. `CH $02FC` is never written on the
  Amiga, so the "any key exits" branch simply never fires — acceptable (START + timeout remain).
* **Scene routing**: `deriveRenderSignals()` gains `rsStation = (vvblki == 0x1B30)`;
  `renderFrame()` gains a `staticStation` branch alongside `staticTitle`.

---

## 3. Boot-chain integration — where the two scenes hook in

The real Atari runs them from the loader's INITAD chain, interleaved with segment loading.
`rof.xex` is already embedded whole in `.rodata` (`incbin.s` → `rof_xex`), and
`load_xex_image()` walks it with `xex_parse()`.

### ✅ DECIDED (user, 2026-08-12): staged load

Split `load_xex_image()` into three calls that stop at the INITAD segments:

```
zero mem[]; overlay OS ROM
xex_parse(segments 1..7)      →  logo_run()          // INITAD $5000
xex_parse(segments 8..11)     →  initad_1A97()       // INITAD $1A97  (= page swap + station)
xex_parse(segments 12..20)    →  game_entry()        // INITAD $3CDE
```

Each phase then sees exactly the memory the Atari sees, with no save/restore anywhere: segment 10
(`$4000-$44FF`) *is* the station image at the moment `screen_page_swap` moves it to
`$0600-$0AFF`, and segment 16 naturally overwrites `$4000` afterwards. `game_entry` ends up with
a byte-identical image to today's, except for the scratch the scenes legitimately leave behind
(`$283E-$37FF`, `$B800+`) — which the real machine also leaves behind, so leaving it is the
faithful choice. Note `$0600-$0AFF` comes out **clean** by construction: the exit
`screen_page_swap` swaps the (zero) `$4000-$44FF` back into it.

Suggested shape: `xex_parse()` in `src/xex_load.h` gains a segment-index window (or a
"stop after the segment that writes INITAD" callback) so the three calls share one walk; the
platform TUs keep supplying their own `RofMemWrite`. Zero-RAM and the OS-ROM overlay stay
one-shot, before stage 1.

### Option B — mem[] injection (the "swap content at will" route) — NOT TAKEN

Keep the single full load, and around the Station do:

```
save   S = mem[$4000..$44FF]                    // 1280 B
inject mem[$4000..$44FF] = xex segment-10 payload
initad_1A97()                                   // its two screen_page_swaps cancel out
restore mem[$4000..$44FF] = S
```

This works exactly because the entry and exit page swaps are symmetric. It is 1280 bytes of
save + 1280 of inject and needs no loader change — but it is a *second* code path that has to be
kept in sync with the real boot order, and it does not help the Logo at all (which additionally
needs `$5000-$536F` restored and `$6000-$6A52` saved/restored — 3.5 KB of windows). If the Logo
bitmap is baked (§1.3) the Logo needs no mem[] at all, and Option B becomes viable for the
Station alone.

Kept only as a record of why it was rejected: it is a *second* boot path to keep in sync with the
real load order, and (unlike A) it gives SDL nothing.

### 3.1 Dev skip ✅ DECIDED (user, 2026-08-12) — required, not optional

Both scenes must be skippable, because **every existing probe/harness assumes boot reaches
Standby quickly**. `amiga/diag_run.sh [delay]` and the `GDBSCRIPT=fps_seg.gdb ./diag_run.sh 200`
perf recipe are calibrated in wall-clock seconds; the Logo (~4-5 s) plus the Station (~4 s of
scroll and up to a 17 s idle) would silently invalidate every one of them — including the
standing 22.49 FPS baseline, which must stay comparable.

* **Amiga:** `make SKIPBOOT=1` → `-DROF_SKIP_BOOT_SCENES`. With it defined, `run()` does the whole
  load in one `xex_parse()` and calls `game_entry()` — i.e. **byte-for-byte today's behaviour**.
* **⚠ Propose (confirm before building): `PROBES=1` / `FPSCOUNT=1` should imply `SKIPBOOT=1`**
  unless `SKIPBOOT=0` is passed. Otherwise every calibrated delay in `amiga/*.gdb` and every
  `docs/flight-perf-log.md` comparison silently shifts, and the first symptom is a mystery perf
  "regression" — precisely the class of confound `docs/flight-perf-log.md` §19 already burned a
  session on.
* **SDL:** no new flag needed — extend the existing `ROF_START` env var (`PlatformSDL.cpp`
  `rofStartStage()`) with `logo` and `station` stages. **`standby` stays the default**, so the
  boot scenes are opt-in on SDL and no existing SDL workflow changes.
* Faithful skips remain in addition to the flag: START (F1) already exits the Station
  immediately, and the ~17 s `RTCLOK_MID ≥ 4` timeout still applies. Making START *also* abort
  the Logo would be convenient but is NOT faithful (the Atari's logo is an unconditional blocking
  routine) — decide separately if it turns out to be wanted.

---

## 4. Corrections to record — ✅ ALL APPLIED 2026-08-12

The three renames below are **done** in `disasm/symbols.csv` (and swept through every hand-written
twin, probe and doc); the two description-only fixes are in as well. Names here are the NEW ones.

* **`station_star_fade_in $1E79`** (was `pmg_update_station`) — not a PMG routine. It is the
  **starfield luminance fade-in**: 14 passes over the mode-F star rows `$2CB8-$3167`, one frame
  apart, brightening each non-zero nibble.
* **`station_pm_shape_tick $1E01`** (was `dli_handler_station`) — **not a DLI**. `VDSLST` is never
  installed during the Station scene and no DL entry sets bit 7; `$1E01` is tail-jumped to from
  `station_audio` (`$1BD7`) and drives the P0/P1 shape cycle at `$3400`/`$3500`.
* **`station_missile_drift $1910`** (was `pmg_missile_init`) — it is a per-frame missile *drift*
  (`HPOSM0-3`), called from `station_audio`, not an init.
* **`pmg_colors_station $1F0B`** — name kept, description fixed: it writes **`COLPM2`/`COLPM3`**
  (`$D014`/`$D015`) and `HPOSP2/3`, not `COLPF3`.
* **`stage_5000 $5000`** — the name/description belong to the *game* routine at that address in
  the final image. The boot INITAD `$5000` is the **Lucasfilm logo** (segment 5, overwritten
  later). It has no `symbols.csv` row to fix (the name lives in
  `ghidra_scripts/entrypoints.csv`), so the correction went into `docs/startup-flow.md` §2/§4 and
  that entrypoints comment.

## 5. Build order (the plan of record)

All three design decisions are settled (§1.3, §3, §3.1). Work the list top-down; each step is one
commit with a green build.

0. **`SKIPBOOT` first**, before either scene exists — add `-DROF_SKIP_BOOT_SCENES` and the
   `ROF_START=logo|station` SDL stages, wired so the *current* behaviour is what the flag
   selects. Then every later step can be A/B'd against a known-good boot, and the perf harness
   never breaks. (Also settle the `PROBES ⇒ SKIPBOOT` question here — §3.1.)
1. **Split the loader** (staged `xex_parse`) and run the **Station on SDL** — everything it needs
   is already transpiled, so this should be close to free, and it gives the `atari800` parity
   oracle before any copper code exists.
2. **Restore the two dropped `display_scroll` writes** in `rof_manual.c` (the DL-ring JVB move and
   the P2/P3 + missile paints). Shared file — fixes SDL and Amiga together. Verify on SDL against
   `station.a8s`.
3. **Amiga Station, part 1:** the shared GTIA-9 → 4-plane decoder + `StationCopperList` + the tall
   340-row bitmap + the VBI pointer scroll. Station image, stars and scroll only — no sprites.
4. **Amiga Station, part 2:** P2/P3 + P0/P1 sprites, then the eight missile dots.
5. **Logo:** bake `assets/logo.raw`, `LogoCopperList`, `logo_run()` + `logo_vbi_native()`, sparkle
   sprite.
6. ✅ **Cleanup (DONE):** the dead `station_*_native` block in `rof_native_amiga.cpp` is deleted
   (§2.5 defect 2); the five renames (§4) were already applied on 2026-08-12.
   Followed by 6b, the three on-screen PMG bugs in §2.4a.

Things to watch, in the order they will bite:

* The tall bitmap's plane pointers move **in the VBI ISR only** (`amiga-copper-lessons`).
* `sfx_voice_tick` must be inert while `VVBLKI == $1B30`, or it fights `station_audio` for Paula.
* `make clean` before any `PROBES=1` build and after touching a shared header (`CLAUDE.md`).
* `station_star_fade_in` is the **star fade-in** — if the stars look black, that is the routine.

## 6. Decisions taken (2026-08-12, user)

| Question | Decision |
|---|---|
| Boot-chain integration | **Staged XEX load** (§3 Option A). Option B is recorded as rejected. |
| Logo bitmap | **Bake** from `logo.a8s` (§1.3). The stroke plotter is not ported. |
| Dev skip | **Required** (§3.1): `make SKIPBOOT=1` on Amiga, `ROF_START=logo\|station` on SDL with `standby` still the default. |

One sub-question deliberately left for the implementing session: whether `PROBES=1`/`FPSCOUNT=1`
should imply `SKIPBOOT=1` (§3.1). Recommend yes.

---

## 7. Star fade-in speed — the 10× slowdown, measured and fixed (2026-08-13)

**Report:** "the stars fade in way slower than on the Atari", with the suspicion that the port was
re-decoding the whole GTIA-9 screen every frame.

**Measured first, with `amiga/star_fade.gdb`** (breaks `station_star_fade_in` / `station_audio` /
the decode entry points and stamps `g_vbiCount` at each; needs a plain `make` build, because
`PROBES=1`/`FPSCOUNT=1` imply `SKIPBOOT` and skip the scene):

| | vblanks for the whole fade | per brightening step |
|---|---|---|
| Atari (`$1E79`: 14 passes, `JSR $3CC3` between) | **15** | 1 |
| Amiga, before | **162** (3.24 s) | ~10.5 |
| after the two native twins | 53 | ~2.4 |
| after narrowing the per-frame decode | 35 | **1** |
| after narrowing the one-off field decode | **29** (580 ms) | **1** |

**It was not the decode.** Per frame the port re-decoded only the ~17 star ROWS (680 source bytes,
~12 ms) — about one vblank of the eleven. The other ~8 were the **transliterated `$1E79` itself**:
sampling its ZP walk pointer `$90/$91` once per vblank showed it advancing **~135 bytes per
vblank** through its 1200-byte pass, i.e. **~1000 68000 cycles per emulated byte** (every 6502 op
stores one to three `cpu` flag bytes through `abs.l` moves, and `LDA ($90),Y` re-derives its
address out of `mem[]` every iteration — ~75 cycles per emulated instruction, a useful rule of
thumb for any other bulk-memory 6502 routine).

**What shipped, in the order the numbers demanded:**
1. **`station_star_fade_in` ($1E79) and `display_list_build` ($1C40) as native twins**
   (`VALIDATE_FUNCS` → `rof_native.c`), 20000 cases each, **0 mem mismatch and 0 cpu diffs**.
   The two 6502 quirks the fade twin must keep: `AND #$F0 / ADC #$10` DISCARDS the low nibble, and
   the walk pointer is re-seeded *before* the pass counter is tested (so `$90/$91` read `$2CB8`,
   not `$3168`, at the return). `display_list_build` must also read RANDOM exactly as often as the
   6502 does — it is a read-clocked LFSR.
2. **The per-frame star decode narrowed from 40 bytes a row to the bytes that can change** (17
   against 680). Sound because `display_list_build` gives each star row exactly ONE non-zero byte
   and the fade can only ever clear a byte, never light a new one — so the non-zero span recorded
   at entry is a superset of everything the fade will touch. Offsets are pre-baked, so the
   per-frame path has no multiply and no display-list read. **This is what made a step fit in one
   vblank.**
3. **The one-off 340-row field decode: a two-slot source cache.** `display_list_build` points every
   non-star sky row at the SAME blank row `$2C90`, so 218 of the 340 entries decode to identical
   bytes; they become a 40-longword copy. Two slots, not one, because the star rows are interleaved
   among the blanks and would otherwise evict the blank row every time.

**What was deliberately NOT converted, on measurement:** the scene's per-frame routines
(`station_audio` → `station_missile_drift` / `station_pm_shape_tick` / `station_sub_1E2A`,
`pmg_colors_station`) and the `station_init` spin-wait apex. The attract loop already runs **one
iteration per vblank — the Atari's own rate — in every RTCLOK phase**, including the late phases
that drive the spacecraft shape animation, so a twin buys nothing. Their misleading ZP names are
recorded in `docs/rename.md` instead. ⚠ And their POKEY/HPOS/COLPM `bus_write`s are **not** dead on
the Amiga: `bus.h` shadows `$D000-$D007` and the PMG mirror reads those back (see its comment).

**The residual 29 vs 15 is entirely the one-off scene entry**, which lands inside the fade's FIRST
step: `renderBootScene` needs two frames to enter (blank the shared list, then rebuild) and
`station_init`'s `$19CD` sync spin only runs one, so the ~10-vblank field build happens with the
fade already counting. Everything after it is frame-exact. Shaving it further means a cheaper
`gtia9Row` (a word-at-a-time variant is worth maybe 20%) or restructuring the two-frame entry —
which would trade a real invariant (blank before rebuilding a SHARED copper list, 792f638) for
moving the same total time earlier. Left alone deliberately.

⚠ **Verify the BITMAP, not `mem[]`.** The narrowing's risk is writing the wrong place in the field,
which a `mem[]` check cannot see (that is the vacuous-probe trap). `star_fade.gdb` therefore reads
the four plane bytes at each recorded star offset and asserts pen 15: **17/17, bad=0**.
