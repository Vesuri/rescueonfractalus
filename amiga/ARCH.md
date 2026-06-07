# Amiga architecture notes — Rescue on Fractalus

## Display: takeover (not OS-friendly)

We use the **takeover** approach for full Copper freedom:

- `LoadView(NULL)` + `WaitTOF()` × 2 to suspend the OS display.
- Own copper list pointed at by `cop1lc` directly (not `MakeScreen`/`LoadRGB4`).
- `*dmaconPointer = DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER` — only copper DMA
  on at first; bitplane / blitter DMA enabled as needed.
- `WaitTOF()` (graphics.library) still works after `LoadView(NULL)` because it
  polls the beam counter directly (`vposr`); exec is running normally.
- On exit: `RemIntServer`, restore saved DMA/interrupt masks, `LoadView(savedView)`,
  `WaitTOF()` × 2, close libraries.

Why takeover vs OS-friendly (`OpenScreen CUSTOMBITMAP + AddIntServer`): RoF needs
per-scanline Copper rewrites (terrain/cockpit colour splits, sprite pointer patches)
every frame. Takeover avoids the OS re-inserting its own system copperlist after
every `WaitTOF`, which would clobber our copper or require `MrgCop` overhead. It
also lets us write Paula registers directly without going through the audio device.

## VBI: exec `AddIntServer`, not raw autovector

Even in takeover mode we use exec's `AddIntServer(INTB_VERTB, …)`. This keeps
exec's level-3 handler intact (CIA timers, exec task scheduling on the BLIT/VERTB
chain), avoids the `installLevel3Interrupt` / VBR plumbing from
`ProductionRunnerAssembler.s`, and is WHDLoad-safe. The overhead (~dozen 68000
cycles per VBI to walk the server list) is negligible at 50 Hz.

## Two-layer split

| Layer | Source | What we take |
|-------|--------|--------------|
| **Hardware** | `dA JoRMaS / Template / C++` | `AmigaHardware`, `Bitmap`, `CopperList`, `Sprite`, `Palette`, `Util` (C++ bodies, NO_ASSEMBLER) |
| **App skeleton** | PETSCII-Robots / WHDLoadMenuAnimated pattern | `main()` + `AddIntServer` VBI server + `while(!quit){poll;update;render;waitVBI}` state machine |

The dA JoRMaS demo `Production`/`Part`/`Script`/`ProductionRunner` timeline and
`ModulePlayer` / TrackerPacker replay are **not used**. The audio approach (M5)
runs the 6502-converted POKEY player and translates writes to Paula directly.

## Build

`make` from `amiga/` (defaults to NO_ASSEMBLER; no vasm step needed).
Toolchain on PATH via `. amiga/env.sh`.
