# Scene composition reference (DL / screen modes / PMG / windscreen-frame elements)

> Split out of `CLAUDE.md` 2026-08-10 to keep the always-loaded file small.
> **Read this when working on a specific scene's screen composition / copper list.**
> Scene names and numbers are in `CLAUDE.md` §Phase vocabulary.

Hard-won base findings so we don't re-derive them. **Verify addresses against a live dump
before trusting** (use `/atari-dl-analyzer` + headless `atari800`).

**Launch cockpit DL (Standby / Doors / Tunnel / Planet share it) = `$3000`**, set *directly*
in ANTIC `DLISTL/H`; the `$0230` shadow points at `$B832` (a stale blank text DL — don't walk
it). Layout (DL-relative scanlines): y+20 **mode 6** top bar (Score `$32E3`, Compass `$32C9`);
y+32 mode 4; y+40 mode D; y+42..127 **mode F** viewport (one LMS/scanline stepping `$1000+`,
stride `$2E`) rendered under **GTIA mode 10** (`PRIOR=$94`, 9-colour — pixel value selects a
COLPM*/COLPF* register, NOT normal 1bpp); y+128..135 **mode D `$350D`** windscreen-bottom band
(4 rows × 2 = Amiga scanlines 172-179); y+136..215 **mode 4 `$332D`** dashboard.

**Launch DLI chain** (`VDSLST=$6CAD`): dispatch on index `$C7` through word table **`$6DBB`** =
`[$4A0C, $6CD7, $6CF1, $6D28, $6D42, $6D4F, $6D7C, $6D99]`; tail `$4A05` does `INC $C7`; `$4ACD`
resets `$C7`. Key: `$6CD7` PMG colours `COLPM1/2/3=mem[$08D7/8/9]`+`PRIOR=$94`; `$6CF1`
`COLBK=mem[$0071]`+`COLPF0/1/2=mem[$08D4/5/6]`; `$6D4F` `COLPF=$04/$06/$2C` grey; `$6D7C`
`COLBK=$00`. (DLIs are reachable only via this indirect-jump table, so Ghidra never disassembles
them → absent from `listing.txt`; seed each DLI addr as a Ghidra entry point to persist them —
now done via `ghidra_scripts/entrypoints.csv`.)

**Stars/Planet DLI chain (`VDSLST=$6CC2`, a SEPARATE dispatcher from `$6CAD` above):** `$6CC2`
dispatches `$C7` through word table **`$6DCF`** (not `$6DBB`). Its key handlers, top→bottom:
`$6D0E` viewport playfield pens `COLPF0/1/2=$24/$28/$2A` (the 3-tone planet/star body); `$6D67`
**windscreen-bottom band frame** — writes ONLY `COLPF0=$04`/`COLPF1=$06` (the two cockpit greys)
and **deliberately leaves COLPF2 and COLBK untouched**, so COLPF2 stays `$2A` (the planet's
brightest tone) = the salmon seen through the corner triangles, and COLBK stays black; `$6DA1`
dashboard `COLPF2=$2C`+`COLBK=$90` (dark blue). ⚠ `$6D4F` (`$04/$06/$2C`) is a LAUNCH-chain
handler that governs the **dashboard**, NOT this band — don't use it for the stars/planet band.
**Faithful copper rule:** if a DLI leaves a register untouched, the CopperList must too — emit
only the MOVEs the DLI actually makes (e.g. `PlanetCopperList`'s band block emits just
`color01`/`color02`, inheriting `color00`/`color03` from the viewport).

**Windscreen-frame PMG (measured `launch_1_title`):**
- **Pillars (window edges):** MISSILES — M0+M1 (right), M2+M3 (left), `SIZEM=$00`, **grey**
  (`COLPF3=$06`, 5th-player). `HPOS` converges inward going down (perspective); `GRAFM` cycles
  per region. (NOT players. Amiga `leftPost`/`rightPost` 16px sprites approximate these.)
- **Corner-triangle wedge:** PLAYERS P0 (left `HPOS$2D`) / P1 (right `HPOS$BE`), **`SIZEP=$03`
  quad (~32px)**, `GRAFP=$FF` at scanlines 136-143 only, **green** (`COLPM0/1=mem[$0071]`),
  buffer `$0C88-$0C8F`/`$0D88-$0D8F`. The narrowing comes from the grey frame masking the block.

**Windscreen-corner triangle per scene:**
| Scene | Implementation |
|---|---|
| **Standby** | band mode-D corners = `COLBK` (DLI background, green). On the Atari COLBK is ONE register, green continuously from the viewport top through the band (measured `launch_1_title`: COLBK=`$C8` y50-136 → `$00` y138). The Amiga mirrors this: the door field decodes COLBK (GTIA-10 value 8) → **`color00`** via `kNibbleColour` (8→pen0; road dots value-0→pen3), so `color00`=green flows from the terrain region straight into the band — no per-band poke. `INDEX_DASH_BG` flips `color00`→black below. (Was a `setBandBgColor` `color00` split; removed.) The green quad-player wedge is present/full below it. |
| **Doors** | band green (`color00`), inherited the same way as Standby — COLBK is green across the WHOLE Doors viewport *including the tunnel reveal* (measured `doors_mid`: rings are playfield pens `$34/$36/$38` over green COLBK), so `color00`=green set once on band0 flows through all 3 terrain bands into the band. (Was `setBandBgColor`; removed.) Reveal hasn't started. |
| **Tunnel** | the green wedge **recedes top-down** = the green→purple reveal. `FUN_6a27` (called from `$538D` in `launch_anim_dispatch $5367`) does `DEC $008C` (wedge height 8→0) + clears `$0C88+` one line/frame, **gated behind `$0088==0 && $0089==0 && $008B==0`** (ring tick paused). Native `launch_anim_dispatch_native` was missing this `$008C` branch; restored — recede now runs on the Amiga, rendered by `TunnelCopperList::setBandReveal`. |
| **Flight (7)** | The windscreen-bottom band (bitmap rows 43-46 = scanlines 172-179) is rendered as PART of the direct terrain render (2026-07-05, `renderFlightDirect`): terrain fills all **47 rows** (skyline clamp→row 46 + sky-fill rows 0-45), so planes 1&2 hold live terrain full-width and the band's **L/R 32px show real terrain**. The grey windscreen frame is on the otherwise-unused **plane3** across the middle (`color04-07` all = frame grey `$00D4`); the salmon wing-clearance bars (mode-D field value 1) + centre marker (value 2) OVERWRITE planes 1&2 as holes in plane3 (bar→`color01` salmon, fades with terrain) and plane-2 terrain **dots reach band rows 44-46** (scanline 43 excluded = the `$6B` COL_MAX reset floor). The band overlay is a RMW after the sky fill, sourced from the live mode-D band field (`mem[$1074+43*96]`, written per frame by `game_sub_451d`). Corner triangles are separate **PMG sprites** on top. See [[flight-scene]]. |
| **Planet/Stars (6)** | **bitmap**, NOT PMG (the planet is the mode-D viewport bitmap). The windscreen-bottom band ($1810, Amiga scanlines 172-179) is the bottom 4 mode-D viewport rows under the `$6D67` frame palette: black bg + two greys (`$04/$06`) + `COLPF2=$2A` (planet) — value-2-dominant bitmap reads as the grey frame, value-3 edges = the salmon planet in the corner gaps. `PlanetCopperList` band block emits only `color01/color02` (mirrors `$6D67`). The grey **edge pillars** (5th-player missiles, `COLPF3=$06`) are ported (as with all scenes' corner frame — DONE 2026-07-05). |
