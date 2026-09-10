# Cockpit render — DONE (writer-driven decode) + remaining TODO

## Update 2026-07-02 — full-repaint cost slashed + scene-entry repaint dropped
- **`cockpitForceFull` now fires ONLY on the standby-build edge** (`g_doorFieldReady` 0→nonzero), NOT on
  stars/flight entry. The single persistent `cockpitBitmap` is shared by every copper list (never cleared
  on a scene switch) and `standby_scoreboard_render` ($587B) writes no cockpit cells, so the dashboard decoded once
  at the standby build survives Standby→Doors→Tunnel→Stars→Flight; dial/digit deltas are caught
  incrementally. The old stars-/flight-ENTRY repaints were redundant and cost a ~580ms tunnel→stars freeze
  + a ~700ms flight-entry freeze (77ad113).
- **`decodeCockpitFull` 216ms→66ms**: (a) `decode2bppByte` → 256-entry LUT `s_dec2bppP1/P2` (accf1d1;
  killed a per-call variable-shift loop, ~1-bit/cycle on the 68000); (b) hoisted the per-cell SIGNED
  `entry=off/48`, `col=off%48` out of the loop — GCC lowered them to `__divsi3`/`__modsi3` SUBROUTINE
  CALLS *per cell* (1120 calls/full repaint, ~70% of cost). All callers pass single-row/single-region
  spans, so the divide is once-per-span now (e94a656). Byte-identical.
- Further cockpit-decode asm was considered and **declined** by the user (66ms one-shot at standby is fine).

Implemented 2026-06-25/26. The old per-frame full cockpit scan (shadow-compare over ~560
mode4/modeD cells, run ~3×/game-frame in flight — the #1 flight cost) is GONE. Decode is now
**writer-driven per instrument**: each writer raises one boolean and `render()` decodes only that
instrument's cells. Idle frames cost three byte reads. Flight-only cost: **~1662 → ~65 ticks/frame
(~23×)**; ~15–30 of ~180 flight frames decode anything.

Single-byte flags are atomic on the 68000, so writers on the VBI ISR (lock-on, dials, digits) need
no `Disable()`/`Enable()` — an early Disable-in-ISR version WEDGED interrupt
delivery (game never auto-started); **do NOT reintroduce it**.

Files: registry + decode in `RescueOnFractalus.cpp` (`decodeCockpitSpan`/`decodeCockpitFull`,
`g_ck*` flags, `cockpitForceFull` one-time full repaint on scene entry). Writers in
`rof_native_amiga.cpp` (lock-on sets `g_ckLockon` directly) and `rof_native.c`
(dials via `platform_cockpit_dirty` → `PlatformAmiga::cockpitDirty` → `rof_cockpit_dial_dirty`,
range-guarded $332D-$355D). `make validate FN=draw_object_column` passes (hook is a no-op on SDL).

## HOOKED writers (confirmed converted)
- **Digits** `startup_init` (`rof_native.c`, flight VBI): `digit_block_dirty()` →
  `platform_cockpit_dirty` → the per-cell dial registry, for the five 2×2 blocks
  $33B4 / $3413 / $3445 / $3472 / $34A4 (+ bottom row at +$30); the DL-stride pair $33DF/$33E0 is
  decoded at 50 Hz in the VBI by `flightScannerTick`.  ⚠ There is no digit registry and no
  `startup_init_native` any more: a per-rendered-frame mirror in `perFrameWork` used to own both,
  and it double-drove a routine that pushes the event-$14 range beep.
- **Lock-on** `lock_on_indicator_tick_native`: set `g_ckLockon`; render decodes $3491-$3497.
- **Dials (thrust #4 / dangerous-alt #5)** `draw_object_column`: bar cells come from the $4581
  column table (NOT a fixed span) → the dial keeps per-cell precision (`g_ckDialFlag[addr-$332D]`,
  480 mode4 cells, walked only when `g_ckDial` set). A fixed bounding-box decode was tried and
  measured ~4× worse (re-paints dozens of static cells per bar move).

## Status lights #14/#15/#16 — HOOKED 2026-07-13 (commits 9b5ece3, 0358cf6)
The four cockpit status-light cells (init'd by `game_sub_4606` to the OFF glyphs $B4/$B5/$B6/$B4) toggle
$34/$35/$36 (ON — lit pixel COLPF2 light $2C) <-> $B4/$B5/$B6 (OFF — same glyph, bit7 set → COLPF3 dark $26);
`decodeCockpitSpan` maps that value-3 pixel to PF1 code 2 for ON and code 3 when bit7 is set, so
ON uses COLOR02=$2C and OFF uses COLOR03=$26:
- **$3355 = Shields (#14)** — confirmed (shields.a8s $34).
- **$3388 = Air Lock (#16)** — confirmed (airlock.a8s $34; row1 col43, NON-adjacent to shields).
- **$3356/$3357 = Mother Ship (#15)** pair — hooked, UNVERIFIED (never seen ON; verify by flying near it,
  else pin the exact cell from a mother-ship savestate the way $3388 was found).
Their writers raise no dirty flag, so a 4-cell change-detect at the top of the native `vbi_handler_flight`
(`#ifdef ROF_PLATFORM_AMIGA`) flags each for re-decode when it flips. Lock-on #11 hooked earlier (a3557dc).

## TODO — remaining unhooked writers
- **Targeting Scope (#8) / Long Range Scanner (#13) cells** — the scanner GUIDE DOT (#13) is done (M2 hardware
  sprite, commit f8bffc4), not a cell; confirm the scope/scanner static cells are correct after entry.
- **Score / kills / quota digits** ($3413/$3445/$3472/$34A4) — hooked, but only fire on game
  events; confirm they update on a rescue/kill.
