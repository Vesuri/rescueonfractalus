# Cockpit render — DONE (writer-driven decode) + remaining TODO

## Update 2026-07-02 — full-repaint cost slashed + scene-entry repaint dropped
- **`cockpitForceFull` now fires ONLY on the standby-build edge** (`g_doorFieldReady` 0→nonzero), NOT on
  stars/flight entry. The single persistent `cockpitBitmap` is shared by every copper list (never cleared
  on a scene switch) and `cockpit_display` ($587B) writes no cockpit cells, so the dashboard decoded once
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

Single-byte flags are atomic on the 68000, so writers on the VBI ISR (lock-on, dials) and the main
thread (digits) need no `Disable()`/`Enable()` — an early Disable-in-ISR version WEDGED interrupt
delivery (game never auto-started); **do NOT reintroduce it**.

Files: registry + decode in `RescueOnFractalus.cpp` (`decodeCockpitSpan`/`decodeCockpitFull`,
`g_ck*` flags, `cockpitForceFull` one-time full repaint on scene entry). Writers in
`rof_native_amiga.cpp` (digits/lock-on set `g_ckDigits`/`g_ckLockon` directly) and `rof_native.c`
(dials via `platform_cockpit_dirty` → `PlatformAmiga::cockpitDirty` → `rof_cockpit_dial_dirty`,
range-guarded $332D-$355D). `make validate FN=draw_object_column` passes (hook is a no-op on SDL).

## HOOKED writers (confirmed converted)
- **Digits** `startup_init_native`: set `g_ckDigits`; render decodes the five 2×2 blocks
  $33B4 / $3413 / $3445 / $3472 / $34A4 (+ bottom row at +$30) and the DL-stride pair $33DF/$33E0.
- **Lock-on** `lock_on_indicator_tick_native`: set `g_ckLockon`; render decodes $3491-$3497.
- **Dials (thrust #4 / dangerous-alt #5)** `draw_object_column`: bar cells come from the $4581
  column table (NOT a fixed span) → the dial keeps per-cell precision (`g_ckDialFlag[addr-$332D]`,
  480 mode4 cells, walked only when `g_ckDial` set). A fixed bounding-box decode was tried and
  measured ~4× worse (re-paints dozens of static cells per bar move).

## TODO — UNHOOKED writers (these instruments may FREEZE in flight; hunt when needed)
Verify each VISUALLY in flight (`run.sh`); if frozen, find the writer + add a dirty-flag call.
- **Shields light (#14) / $3355 `special_state_color`** — CONFIRMED changing, but written by
  TRANSPILED `enter_terrain_special_state` ($9B0D) / `exit_terrain_special_state` ($9B4C) /
  `check_object_in_target_box` ($93BD), none hooked. HIGHEST priority.
- **Mother Ship (#15) / Air Lock (#16) lights** — writers not located (event-driven).
- **Targeting Scope (#8) / Long Range Scanner (#13)** — didn't change in neutral flight; confirm
  they're static after entry (or event-driven), not needing a per-frame writer.
- **Score / kills / quota digits** ($3413/$3445/$3472/$34A4) — hooked, but only fire on game
  events; confirm they update on a rescue/kill.
