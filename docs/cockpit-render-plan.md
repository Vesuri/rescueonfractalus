# Cockpit render perf — implementation plan (flight scene)

Goal: stop the per-frame full cockpit re-decode in flight. Replace the shadow-compare
scan with **per-writer dirty flags**, each mapping to a known set of cockpit cells, so the
render converts *exactly* the bytes that changed and does no extra work. **Eliminate the
shadow-compare everywhere** (a 560-byte volatile-CHIP-RAM scan is a no-go on the 68000).

## Measurement (2026-06-23, headless FS-UAE beam-clock probe `g_fCockpit`)

Per flight frame: terrain `draw` ≈ 82 ms (#1), **cockpit decode ≈ 24 ms (≈372 beam-ticks/scan,
scanned EVERY frame)**, collision ≈ 15 ms, setup ≈ 10 ms, clear ≈ 6 ms. The cockpit is ~17 %
of flight compute and almost entirely avoidable: `deriveRenderSignals` force-sets
`g_cockpitDirty=1` whenever `rsFlight` (RescueOnFractalus.cpp ~line 1249), so the decode runs
its full shadow scan every frame even though only a few instrument cells change.
(Probe: `cd amiga && make PROBES=1 && ./diag_run.sh 85`; reads `g_fCockpit`/`g_fCockpitScans`
+ `g_flightProf` via diag_timing.gdb. Flight is reached ~vbi 2200; needs ≥75 s delay.)

## Current decode (RescueOnFractalus.cpp render(), ~line 1528-1618) — to be replaced

Two shadow-compare loops over `mem[]` (volatile CHIP RAM):
- **modeD** rows 0-7: src `$350D+4`, 4 entries × 40 bytes, each byte → 2 identical scanlines.
- **mode4** rows 8-87: src `$332D+4`, 10 entries × 40 chars, each changed char → 8 scanlines ×
  3 planes. charset `$3800`, glyph = `ch & $7F`, bit7 → plane3 = `$FF` (colours 4-7).
- dest = `cockpitBitmap->data`, 3bp interleaved, `kRowBytes=120`, stride 48 (skip 4 overscan).
Shadows: `cockpitModeDShadow`, `cockpitMode4Shadow` — **delete these**.

## Instrument classification (CLAUDE.md "Instrument vocabulary" + user 2026-06-23)

| Type | Instruments | Cell work |
|---|---|---|
| **PMG** (no cells) | Artificial Horizon (P2 `buildAHSprite` — CLAUDE.md #6 was wrongly "cells", fixed), Altimeter (P0/M3), Energy (P1) | none |
| **1-2 cell lights** | Shields (#14), Mother Ship (#15), Air Lock (#16) | tiny on/off |
| **Multi-cell dials** | Thrust (#4), Dangerous Altitude (#5) | a vertical bar column |
| **Numeric 2×2** | Range (#17), Enemies Destroyed (#18), Pilot Quota (#19) | 2×2 char blocks |
| **Static** | dashboard frame, scope (#8), scanner (#13) outlines, modeD rows 0-7 | paint once on entry |

## Writers → cells (what flags which span)

Writers ALREADY change-detect (caches `$0647/$0645/$0646`, `$062E`, lock-on state) and today
set the shared `g_cockpitDirty`. Give each its own flag + decode only its cells:

- **Digits** `startup_init_native` (rof_native_amiga.cpp ~463), called from perFrameWork (MAIN
  thread). 2×2 blocks at dest +0/+1/+$30/+$31:
  - Digit1 `$33B4` (gated on `$0642`/`$0647`)
  - Digit2 `$3413` (tens) + `$3445` (units) (gated on `$0641`/`$0645`)
  - Digit3 `$3472` (tens) + `$34A4` (units) (gated on `$0628`/`$0646`, +$80 colour flag)
  - DL-stride bytes `$33DF`/`$33E0` (`$9E/$9D` or `$1E/$1D`) — also in the scanned region.
- **Lock-on** `lock_on_indicator_tick_native` (~538), called from the VBI (**ISR**). Cells
  `$3491-$3497` (7 mode-4 chars).
- **Thrust / Dangerous-Alt dials** `draw_object_column` / `draw_cockpit_dial_bar` /
  `draw_dial_bar_column` (native, rof_native.c ~5128). Writes via the `$4581` column-pointer
  table → cockpit cells; dest computed at runtime as `mem[$BB]|mem[$BC]<<8`. Does NOT flag
  dirty today. **Cells TBD** — see discovery below.
- **Status lights** (shields/mother-ship/air-lock) — **writer not yet located.** Find it
  (likely in the flight state-update / a transpiled routine writing the far-right mode-4 cols).

## Empirical cell-span discovery (do FIRST next session)

Don't reverse-engineer the `$4581` table by hand. Two options:
1. **Runtime-report**: have `draw_object_column` (and the status-light writer once found) record
   the actual dest addresses they write — gives exact spans with no table archaeology.
2. **Shadow-extent log**: temporarily keep the shadow loops but record min/max changed mode4/
   modeD cell address over a flight run (`make PROBES=1` + diag). The union of changed cells =
   the complete in-flight dirty set → confirms every span before the shadow is removed.

## Implementation steps

1. **Discover** the dial + status-light exact cell spans (above). Confirm modeD rows 0-7 never
   change in flight (expected static → paint once on entry).
2. Add **per-writer volatile flags** (e.g. `g_ckDigit1..3`, `g_ckLockon`, `g_ckStride`,
   `g_ckThrust`, `g_ckDangerAlt`, `g_ckShields/Mother/AirLock`). Volatile byte flags are
   ISR/main race-safe exactly like the current `g_cockpitDirty` (worst case: one redundant
   span decode — never a missed change).
3. Each writer sets ONLY its flag (replace the `g_cockpitDirty=1` lines). Keep the existing
   change-detection so a flag is set only on a real value change.
4. Refactor the decode into `decodeCockpitSpan(cellAddr, nCells)` (cell addr → bitplane pos via
   the existing entry/col → dest math, factored out). render() checks each flag, decodes its
   span, clears the flag. **No shadow, no full scan.**
5. **Flight entry** (and any scene entry needing a full paint): set all flags once (or a single
   `cockpitFull` that decodes the whole region one time), then steady-state is flag-driven.
6. Remove the per-frame force in `deriveRenderSignals` (line ~1249-1251) for `rsFlight`; delete
   `cockpitModeDShadow`/`cockpitMode4Shadow` and the shadow loops.
7. **Eliminate other shadow compares too** (user directive "everywhere"): audit the title
   ($32B7) and any compass/region decodes for shadow scans and convert them to dirty-flag /
   dirty-range (compass already targeted; title uses a shadow compare — convert).
8. Verify on FS-UAE (`run.sh`) that every instrument still updates (dials move with thrust,
   digits on rescue/kills, lock-on animates, lights toggle). Re-measure `g_fCockpit`
   (target: ~24 ms → ~0 on steady frames, a few ms when an instrument changes).

## Files

`src/platform/amiga/RescueOnFractalus.cpp` (render decode + deriveRenderSignals force),
`RescueOnFractalus.h` (shadow members), `src/platform/amiga/rof_native_amiga.cpp` (digit/
lock-on writers), `src/gen/rof_native.c` (`draw_object_column` dial writer). Probe:
`g_fCockpit` already in PlatformAmiga.cpp + diag_timing.gdb (keep for re-measurement).

## Risk

If any bitmap-cell writer isn't converted before the shadow is removed, that instrument
freezes. Mitigation: discovery step #1 enumerates every in-flight-changing cell first; keep the
shadow as a temporary backstop until all writers are confirmed converted, then delete it.
