# Atari 8-bit hardware — primer & the abstraction boundary

A field guide for someone coming from the C64/Amiga. It covers just enough of
the Atari 400/800/XL/XE architecture to recognize what the game's code is doing
when it touches hardware, and maps each feature to its SDL and Amiga equivalent.
These are the things `platform.h` must abstract.

## The chips

The Atari 8-bit splits work across four custom chips plus the 6502 (here a
6502C "SALLY" at ~1.79 MHz NTSC / 1.77 MHz PAL). Unlike the C64's single VIC-II,
the Atari has **three** display/IO chips:

| Chip | Range | Role |
|---|---|---|
| **GTIA** (or CTIA) | `$D000–$D01F` | Color output, **Player/Missile graphics** (sprites), collision detection, console switches, GTIA hi-color modes. |
| **POKEY** | `$D200–$D21F` | 4-channel **sound**, keyboard scan, paddles, **RANDOM** number generator, serial I/O, IRQ **timers**. |
| **PIA** (6520) | `$D300–$D31F` | Joystick port registers (`PORTA/PORTB`), and on XL/XE `PORTB` does **memory banking** (OS ROM enable, RAM under ROM). |
| **ANTIC** | `$D400–$D41F` | The **display processor**: a coprocessor that reads a *display list* (a program) from RAM via DMA and drives the screen; also fine scrolling, the **DLI/VBI** interrupts, and `WSYNC`. |

ROM/OS lives at `$C000–$CFFF` and `$D800–$FFFF`; hardware registers sit in the
`$D000–$D7FF` window between them.

## ANTIC and the display list — the big conceptual difference

There is no single "screen mode" register. Instead **ANTIC executes a display
list**: a small program in RAM, one instruction per horizontal region, telling
it which graphics mode to use for the next rows, when to fetch a new memory
scanline, where the screen data is, and when to trigger an interrupt. Key pieces:

- **DLISTL/DLISTH** (`$D402/3`, OS shadow **SDLSTL/SDLSTH** `$0230/1`): pointer
  to the display list.
- **Display-list instructions** select ANTIC modes (text modes 2–7, bitmap
  modes 8–F). The cockpit + 3D view of *Rescue* is almost certainly a custom
  display list mixing a bitmap mode for the viewport with character/bitmap rows
  for the dashboard.
- **DLI (Display List Interrupt)**: a flagged display-list line fires an NMI
  mid-screen, used to change colors/registers per region (e.g. sky vs. terrain
  vs. cockpit). Vector **VDSLST** `$0200/1`. This is how you get more colors than
  the register count suggests.
- **VBI (Vertical Blank Interrupt)**: NMI once per frame; the game's main logic
  tick. Vectors **VVBLKI/VVBLKD** `$0222/$0224`.
- **WSYNC** (`$D40A`): write to halt the CPU until the next scanline — used to
  time mid-screen changes.

**Abstraction:** model the display list as an ordered list of *regions*, each
with a mode + data pointer, and expose **DLI hooks** (per-region register
changes) and a **VBI tick**.
- *SDL:* render regions into a 32-bit framebuffer → texture; run DLI/VBI as
  callbacks on a 50/60 Hz loop.
- *Amiga:* bitplanes + a **Copper** list (the Amiga's own display coprocessor —
  a near-perfect analog of ANTIC's display list); Copper `MOVE`s replace DLI
  color changes; vertical-blank interrupt replaces VBI.

## GTIA — color, sprites, collisions

- **Color registers**: `COLPF0–3`/`COLBK` (`$D016–$D01A`), with OS shadows
  `COLOR0–4` `$02C4–$02C8` and player colors `PCOLR0–3` `$02C0–$02C3`. The Atari
  palette is **128 colors** (16 hues × 8 luma; 256 on GTIA). Code usually writes
  the shadows; the OS VBI copies them to GTIA.
- **Player/Missile Graphics (PMG)** = sprites: up to 4 "players" (8 px wide,
  full screen tall, any height bitmap) + 4 "missiles". Registers: horizontal
  positions **HPOSP0–3** `$D000–$D003`, sizes `SIZEP0–3`, graphics `GRAFP0–3`,
  and a base pointer **PMBASE** `$D407` to the PM bitmap in RAM. In *Rescue*
  these are the jaggis, the rescued pilots at the airlock, and likely targeting.
- **Collision detection** (hardware!): read-only registers `$D000–$D00F` report
  player/playfield/missile overlaps. Cleared by writing `HITCLR` `$D01E`.
- **Console switches** **CONSOL** `$D01F`: START/SELECT/OPTION buttons.

**Abstraction:** a sprite list (position/size/bitmap/color) + a collision query;
the game writes sprites, reads collisions.
- *SDL:* blit sprites, compute collisions in software.
- *Amiga:* hardware sprites and/or blitter objects; blitter for collision, or
  software collision to match Atari semantics exactly.

## POKEY — sound, RANDOM, input, timers

- **Audio**: 4 channels, each **AUDFn** (frequency) + **AUDCn** (control:
  distortion/noise + volume), at `$D200–$D207`, plus **AUDCTL** `$D208` (clock
  options, hi-pass, 16-bit pairing). Square waves + several polynomial-counter
  "noise" settings.
- **RANDOM** `$D20A`: a free-running 17-bit LFSR read as a pseudo-random byte.
  **Likely used to seed the fractal terrain** — so for a faithful 1:1 terrain we
  must reproduce POKEY's LFSR *bit-exactly*, not just "some RNG".
- **Keyboard**: **KBCODE** `$D209` + IRQ; usually read via OS shadow **CH**
  `$02FC`.
- **Paddles / pots**: `POT0–7` `$D200–$D207` (read side), **ALLPOT** `$D208`.
- **IRQ timers**: POKEY can generate timer IRQs (AUDF as timers); vectors
  VTIMR1/2/4 `$0210–$0215`. The init code we saw sets **VIMIRQ** `$0216/7`.

**Abstraction:** a 4-voice synth fed AUDF/AUDC/AUDCTL; a deterministic RANDOM
matching POKEY's LFSR; input state; optional timer-IRQ callbacks.
- *SDL:* software synth in the audio callback; `SDL` input; timers via the loop.
- *Amiga:* **Paula**'s 4 channels map almost 1:1 to POKEY's 4 (feed waveforms);
  CIA timers for timer IRQs; joystick/keyboard via the OS or hardware.

## PIA — controllers & memory banking

- **PORTA** `$D300`: joysticks (OS shadows **STICK0–3** `$0278`, triggers
  **STRIG0–3** `$0284`). Directions are 4 bits per stick; trigger is one bit.
- **PORTB** `$D301`: on XL/XE, a memory-management register — enables/disables
  OS ROM, BASIC, and selects RAM under ROM. The XEX pokes this at load
  (`$FF` = normal config). We mostly need to honor the initial setup.

**Abstraction:** joystick/trigger state in the input API; PORTB banking handled
once at init (the C build simply has all RAM available).

## OS shadow registers & vectors (page 2–3)

The Atari OS maintains RAM "shadow" copies of many hardware registers and copies
them to the chips during the VBI. Game code very often writes the **shadow**, not
the chip. The ones to watch (all in `$0200–$02FF`):

| Shadow | Addr | Mirrors |
|---|---|---|
| SDLSTL/H | `$0230/1` | ANTIC display-list pointer |
| COLOR0–4 | `$02C4–$02C8` | GTIA playfield/background colors |
| PCOLR0–3 | `$02C0–$02C3` | GTIA player colors |
| CHBAS | `$02F4` | ANTIC character-set base (set by the `$B800` stub) |
| SDMCTL | `$022F` | ANTIC DMA control (DMA enable, PM DMA, width) |
| GPRIOR | `$026F` | GTIA priority / GTIA modes / PM priority |
| STICK0–3 | `$0278–$027B` | joystick directions |
| STRIG0–3 | `$0284–$0287` | joystick triggers |
| CH / CHACT | `$02FC / $02F3` | keyboard char / char display control |
| RTCLOK | `$0012–$0014` | jiffy (frame) clock, ticked by VBI |
| VDSLST | `$0200/1` | DLI vector |
| VVBLKI/D | `$0222 / $0224` | VBI vectors |
| VIMIRQ | `$0216/7` | immediate IRQ vector (set by `game_entry`) |

**Implication for the bus:** `bus_write` must treat these page-2 shadows as
*meaningful*, not plain RAM — many are the real control surface the game uses.
The Phase 1 hardware-access map will list exactly which ones this game touches.

## Summary: what `platform.h` must expose

1. **Video** — framebuffer + palette; display-list regions; DLI (raster) hooks;
   VBI (frame) tick; `WSYNC`-style scanline sync if needed.
2. **Sprites** — Player/Missile graphics (position/size/shape/color) + collision.
3. **Audio** — 4 POKEY voices (AUDF/AUDC/AUDCTL) + bit-exact RANDOM LFSR.
4. **Input** — joystick directions + triggers, keyboard, console switches, paddles.
5. **Timing/IRQ** — VBI, DLI, optional POKEY timer IRQs; jiffy clock.

Everything else (RAM, zero page, the loader's banking poke) stays inside the
emulated 64K memory and the CPU model.

## References

- *Altirra Hardware Reference Manual* (Avery Lee) — the definitive modern HW doc.
- *Mapping the Atari* (rev. ed.) — the address-by-address memory map.
- *De Re Atari* — Atari's official ANTIC/GTIA/POKEY programming guide.
