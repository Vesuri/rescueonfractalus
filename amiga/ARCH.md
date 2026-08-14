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
chain), avoids hand-installing a level-3 handler and the VBR plumbing that goes
with it, and is WHDLoad-safe. The overhead (~dozen 68000 cycles per VBI to walk
the server list) is negligible at 50 Hz.

## Two-layer split

| Layer | What it is |
|-------|------------|
| **Hardware** | the vendored framework classes `AmigaHardware`, `Bitmap`, `CopperList`, `Sprite`, `Palette`, `Util` (hand-written m68k asm via vasm + GCC bridges; `Sprite`/`Palette` are C++ everywhere) |
| **App skeleton** | `main()` + `AddIntServer` VBI server + `while(!quit){poll;update;render;waitVBI}` state machine |

There is no demo-style timeline layer and no tracker/MOD replay: audio runs the
6502-converted POKEY player and translates its writes to Paula directly.

## Build

`make` from `amiga/` (ASSEMBLER on by default — vasm assembles the framework
`*Assembler.s`; pass `CPPFLAGS+=-DNO_ASSEMBLER` for the portable C++ bodies).
Toolchain on PATH via `. amiga/env.sh`.
