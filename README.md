# Rescue on Fractalus! — Amiga

An Amiga port of *Rescue on Fractalus!*, the 1985 Lucasfilm Games classic for the Atari 8-bit.

You fly a Valkyrie fighter down into the acid atmosphere of Fractalus to pick up downed
pilots, dodging gun emplacements and saucers in fractal mountain terrain. Land next to a
pilot, shut your engines down, and wait for them to knock on the air lock — but check who is
knocking before you open it.

This is a faithful 1:1 reimplementation, rebuilt from the original game rather than emulated.

> **Work in progress.** The game is playable from boot through flight, landings and rescues.
> It is not a finished release yet.

## Requirements

- An Amiga with **1 MB of RAM** — it does **not** fit an unexpanded 512 KB A500
- Developed and tested on an **A500+** under **Kickstart 3.1**
- PAL

## How to run

Copy `RoF` to your Amiga and start it — from Workbench, or from a Shell:

```
RoF
```

Nothing else is needed; all game data is inside the executable. There is no installer or
WHDLoad slave yet.

On an emulator, point a hard drive at the folder containing `RoF` and run it from there.

## Controls

| Key | Action |
|---|---|
| **Arrow keys** | Steer (pitch and roll) |
| **Left Shift** | Fire torpedo |
| **`-`** / **`=`** | Thrust down / up |
| **L** | Land |
| **B** | Boosters (return to the mother ship) |
| **A** | Air lock — open it when a pilot knocks |
| **S** | Systems — engines and shields on/off |
| **Esc** | Pause / unpause |
| **F1** | Start |
| **F2** | Level select |
| **F3** | Demo mode |
| **Help** | Abort mission and restart (score lost, high scores kept) |

Thrust is on the two keys just right of `0`, because those are the physical positions of the
Atari 800's `< -` and `> =` thrust keys.

⚠ Once you have landed, press **S** to shut the engines and shields down. A downed pilot will
not leave cover while they are on — and the energy field will kill anything that comes close.

At the Standby screen, press **F1** to launch. **F2** (or arrow up) opens the level selector,
where arrow up/down chooses a starting level.

*Emulator note:* the **Help** key is usually mapped to your host keyboard's **End** key.

## The manual

[`docs/manual.md`](docs/manual.md) is the original Lucasfilm manual, transcribed in full — the
story, the mission briefing, what every cockpit instrument does, and the pilot rankings. Read
it if you want to actually understand the dashboard.

## Credits

*Rescue on Fractalus!* was created by the Lucasfilm Games Division. David Fox directed the
project and created the concept; Loren Carpenter of the Lucasfilm Computer Graphics Project
did the 3-D fractal landscape generation and co-created the concept; Charlie Kellner did
animation, music, sound and flight dynamics; with Gary Winnick, James St. Louis, Douglas
Crockford, David Levine and Peter Langston. Full credits are in the manual.

This port is an unofficial, non-commercial fan project, not affiliated with or endorsed by
Lucasfilm.

Developer documentation lives in [`CLAUDE.md`](CLAUDE.md) and [`docs/`](docs/).
