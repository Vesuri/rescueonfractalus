---
name: atari-dl-analyzer
description: >-
  Analyze an Atari 8-bit ANTIC display list (and its DLIs) for the Rescue on Fractalus
  port. Given a savestate (and optionally a DL address), produces a clear region-by-region
  description of what the display list does — the scanline ranges, the screen mode + screen
  RAM (LMS) of each region, the live GTIA colour / player / missile / priority state at those
  scanlines, and what each DLI handler writes (and from which mem[] source). Use this whenever
  you need to understand how a scene composes its screen before generating the matching Amiga
  CopperList / poke code, instead of re-deriving it by hand from disasm + dumps.
---

# Atari display-list analyzer

Reusable replacement for the ad-hoc "drive atari800, DLIST, BLINE/GTIA, disassemble the
DLI" investigation we keep repeating. The output is a behavioural **spec** of a display
list (not a copper list) that you then translate into the appropriate Amiga
`CopperList` / `update*Copper` poke code.

It answers, per scene:
- which scanlines are waited / where each region starts and ends;
- which ANTIC screen mode applies in each region (and the LMS screen-RAM address);
- which DLIs fire on which rows and **what colours/positions they modify**;
- the **live** GTIA state (COLBK, COLPF0-3, COLPM0-3, GRAFM, PRIOR, HPOSM/P) at each
  scanline — i.e. the actual on-screen result, including mid-frame DLI changes that
  static analysis of the shadows (`$02C0-$02C8`) cannot see.

## Prerequisites
- `atari800` (v5.x) and `gstdbuf` (`brew install coreutils`) on PATH.
- A savestate of the scene of interest in `a800dumps/` (e.g. `launch_1_title.a8s` =
  Standby, `launch_5_planet.a8s`, `flight1.a8s`). Capture new ones from the FIFO monitor
  with `SAVESTATE <name>.a8s` if needed.
- `kill -9` any stray `atari800` first (`pkill -9 atari800`).

## How to run

The skill scripts live in this directory (`.claude/skills/atari-dl-analyzer/`).

**1. Capture** (headless; drives the emulator's monitor — no display interaction):
```
.claude/skills/atari-dl-analyzer/dl_capture.sh a800dumps/launch_1_title.a8s [ymin ymax step]
```
- Defaults sweep scanlines 8..240 (~90 s). **Narrow the range** once you know roughly where
  the region is (e.g. `120 150 1` for a cockpit-top band) — ~0.4 s per scanline.
- Writes the raw log to `/tmp/dl_capture.log` (override with `DL_OUT=`).
- It runs `DLIST` once, then `BLINE y; CONT; GTIA` for each scanline, tagging each GTIA
  dump with `==L<y>==`.

**2. Report** — turn the capture into the region-by-region spec:
```
python3 .claude/skills/atari-dl-analyzer/dl_report.py /tmp/dl_capture.log [--ram a800dumps/<scene>.bin]
```
Prints two sections: the **DL structure** (modes/LMS/DLI flags, relative scanlines) and the
**live GTIA state by scanline** collapsed into constant-state runs (absolute scanlines). The
DL `MODE D` / distinctive regions let you align the two.

**3. DLI sources** — to learn *which mem[] feeds* a colour (decides baked-constant vs
per-frame poke on the Amiga), disassemble the DLI handler. Find its address from the live
`VDSLST` vector (`$0200/$0201`) or the dispatcher slot table, then:
```
python3 .claude/skills/atari-dl-analyzer/dl_report.py --ram a800dumps/<scene>.bin --dli 0x6D7C
```
Lists the handler's `STA $D0xx` writes with their source (`#$imm` vs from reg/mem). A write
sourced from a zero-page address that *ramps* (e.g. a fade) → make it a per-frame copper poke;
a hard immediate → bake it constant.

**Static-only DL walk** (no emulator, fast structural map from a RAM dump):
```
python3 .claude/skills/atari-dl-analyzer/dl_report.py --ram a800dumps/<scene>.bin --dl 0x3000
```

## Reading the result → Amiga copper
- A region's **value-0** pixels show `COLBK`; value-1/2/3 show `COLPF0/1/2` (map modes) or
  per the GTIA-10/text mapping. Match the Amiga `color00..03` for that scanline band to the
  GTIA values shown.
- `MODE D` rows are line-doubled on the Amiga (2 scanlines each); account for that when
  converting relative DL scanlines to the Amiga band height.
- A colour that **differs across the sweep within one DL region** means a DLI changed it
  mid-region → the Amiga copper needs a WAIT + MOVE at that scanline (a palette split), not a
  single baked value. (This is exactly the Standby windscreen-band COLBK green→black split.)
- `GRAFM`/`HPOSM` nonzero in a band = missiles are part of the composition (port as Amiga
  sprites behind the bitplanes); `PRIOR & $10` = 5th-player (missiles take `COLPF3`).

## Notes / gotchas (kept so they are not rediscovered)
- atari800 stdout is block-buffered to a pipe — `dl_capture.sh` uses `gstdbuf -o0` so dumps
  flush. If a run shows no output, that is the cause.
- `BLINE` ypos is the absolute frame scanline; the visible display starts ~line 8. The DL
  walk gives scanlines **relative** to DL start, so align the two reports by the distinctive
  region (e.g. the mode-D band), not by assuming a fixed offset.
- The Amiga display maps hardware scanline = Amiga-display-y + `kDisplayTop` (0x2c/44): e.g.
  hardware scanline 172 = display y128.
- RAM base in `.a8s` is anchored at $7148 (see `tools/extract_a8s_ram.py`); `--ram` accepts
  either a `.bin` (flat 64K) or a `.a8s` (auto-extracted).
