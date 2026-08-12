# Phase 6 execution plan — native Amiga attract screen

Step-by-step plan to get *Rescue on Fractalus!* running on the Amiga, starting
from a framework and building up to the **cockpit + terrain attract screen** with
idiomatic Amiga hardware use (bitplanes, Copper colour splits, hardware sprites,
Paula audio). Designed to be executed **one milestone at a time, each a clean,
working, committed build**.

> **Architecture decision (2026-06-08, user-confirmed).**
> - **Native rendering**, *not* a `Platform.h` backend running the transliterated
>   6502 **rendering/terrain** code on the 68000 (too slow for live terrain at
>   7 MHz). The existing SDL build + `atari800` stay as the **visual parity
>   oracle**.
> - **The 6502 state machine drives everything — no approximation.** Timing,
>   animation, text switching, cockpit state: nothing is guessed. The m68k slice of
>   `rof_gen.c` runs the real attract logic each frame; the Amiga hardware renders
>   what `mem[]` says. Specific split:
>   - **Amiga VBI interrupt** does only what `vbi_handler_station $1B30` does:
>     increment timers (`$0080`, `$0014`/`$0013`). Lightweight, no rendering.
>   - **Amiga main loop** calls every attract animation function in the same order
>     as the Atari attract loop: `station_star_fade_in`, `station_audio`,
>     `station_anim_frame`, `station_sub_1EB4`, `pmg_colors_station`,
>     `station_sub_1F48`. These update `mem[]`.
>   - **After each frame**, the Amiga renderer reads from `mem[]` to produce
>     display output (blitter copies, character rendering, colour splits).
> - **Audio** runs the 6502-converted player. The transpiled POKEY-poking routines
>   + custom song/SFX data compile for m68k; POKEY register writes are translated
>   to **Paula** output. **No ModulePlayer, no TrackerPacker, no MOD.**
> - **Two-layer reuse, deliberately split:**
>   - **Hardware layer = dA JoRMaS classes** (`AmigaHardware`, `Bitmap`,
>     `CopperList`, `Sprite`, `Palette` — *not* `ModulePlayer`). The well-factored
>     chip-banging primitives.
>   - **Application skeleton = PETSCII-Robots / WHDLoadMenuAnimated style**, *not*
>     the dA JoRMaS demo `Production`/`Part`/`Script`/`ProductionRunner` timeline.
>     A game wants an explicit `main()` + VBI interrupt server + a real
>     `while (!quit) { poll input; update; render; WaitTOF }` loop with a small
>     state machine — which is exactly what RoF's **own** `Platform.h`/
>     `PlatformSDL` already is (the "PETSCIIRobots-SDL pattern" — same author,
>     same C64→Amiga lineage). The Amiga app is the sibling of that, kept native.
> - Code lives in a new top-level **`amiga/`** tree, committed to `main`.
>
> Target scene: the iconic **"RESCUE ON FRACTALUS!" title / cockpit attract**
> (cf. `atari000.png`): title message line, terrain viewport showing green
> fractal mountains, instrument-panel dashboard. See `hw-techniques.md` §11 for
> the element-by-element anatomy and `startup-flow.md` §6 for the cinematic.
>
> **⚠️ PARTIALLY SUPERSEDED — see "CRITICAL REVISION (2026-06-08c)" immediately
> below.** The per-frame function list above (`station_star_fade_in`, `station_audio`,
> `station_anim_frame`, `station_sub_1EB4`, `pmg_colors_station`, `station_sub_1F48`)
> is the **`station_init $195D` loop — which is the SPACE-STATION CINEMATIC,
> not the target attract screen.** The target ("RESCUE ON FRACTALUS!" + LEVEL-04
> doors + cockpit) is a *different* code path. Rework approach changed to **Option B
> (surgical native)**.

---

## CRITICAL REVISION (2026-06-08c) — wrong screen + wrong code path

Live atari800 capture (FIFO monitor, user pressed Fn+F8; PC + `DLIST` + `.a8s`
savestates) proved the Amiga attract was built on a **screen/code-path confusion**.
There are **three distinct screens**, not one:

| # | Screen | Active DL | Main region | GTIA | Audio | Driving code |
|---|---|---|---|---|---|---|
| 1 | Lucasfilm Games logo | `$6000` | mode-F bitmap `$60A3` | — | — | (boot) |
| 2 | **Space-station cinematic** | `$1C35`→`$B800` | ~190 mode-F rows from **`$0600`** stride 40 | **mode 9** | `station_audio` (dual-oscillator fade) | **`station_init $195D`** (PC caught at `$1A18`) |
| 3 | **TARGET attract** ("RESCUE ON FRACTALUS!", LEVEL 04, cockpit) | **`$3000`** | mode-6 title `$32B5` + ~86 mode-F `$2000` stride **46** + modeD `$350D` + mode4 `$332D` | **mode 10** | timer-IRQ music (`$54C0`/`$54EA`) | game/standby loop (PC `~$5A7E`) |
| 3b | **Scoreboard** (⚠ not yet investigated) | TBD | TBD | TBD | TBD | black high-score/stats text screen after a long idle on Standby: title + ©1985 + STARTING/RANKING LEVEL + LAST/HIGH SCORE (all caps); likely via `attract_timer $00E2` |

> **Note (2026-06-08, user):** scenes 1–3 are NOT a simple auto-cycle. Standby is
> the resting screen; a long idle (no START) brings up the **Scoreboard (3b)** —
> an unexamined black stats screen. The exact idle/attract cycle order is TBD.

**What was wrong in this plan:**
- The architecture decision + **M6b** run screen-2's functions (`station_audio`,
  `station_anim_frame`, `station_sub_*` = the `$195D` *cinematic*). That's why the
  Amiga plays the oscillator-fade cinematic audio and animates wrongly. **These are
  the wrong routines for the target.**
- **M6c** treats the mode-F terrain as **1bpp** — but on screen 3 it's **GTIA mode
  10** (each byte → two 4-bit nibbles → colour index). Rendering it 1bpp is exactly
  the "vertical stripes / colours off" in the door/LEVEL-04 region.
- **C6 audio note** (below) is half-right (SDL is the oracle) but mis-diagnosed it
  as "segment selection in `mem[]`". The real cause is the **wrong screen/code
  path** entirely.
- `station_audio`'s melody (phase `$0013=3`, table `$283E`) is moot for the target —
  it belongs to screen 2; the linked `standby_mem.bin` has `$283E`/`$1BF4`/`$1BE5`
  empty anyway. **`attract.a8s` == screen 3** captured "doors closed" (`$2000=$88`
  = flat GTIA-10 fill); not corrupt, just the wrong code running on it.

**What M6a got RIGHT (keep):** the `$3000` DL it audited *is* screen 3's DL,
confirmed live: `$3003 mode6 LMS $32B5` (title) → mode4/modeD → ~86× `mode-F LMS
$2000 stride $2E=46` (terrain, GTIA-10) → modeD `$350D` + mode4 `$332D` (cockpit).
The geometry/row-count work stands. (Note the `$026F` GPRIOR + `$0230` SDLSTL RAM
shadows read STALE in attract — the code writes `$D01B`/`$D402` directly; trust the
live `DLIST` / GTIA chunk, not the shadows.)

### Ground-truth captures (committed in `a800dumps/`)
- `logo.a8s` (+ `logo_6000_6FFF.bin`) — screen 1.
- `attract_target.a8s` (+ `_ram.bin`) — screen 3, doors closed.
- `music_playing.a8s` (+ `_ram.bin`) — screen 3, music live. **Use this as the boot
  snapshot.** (RAM base in the gunzipped `.a8s` = `0x85`; `ram = data[0x85:0x85+65536]`.)
- Audio finding: at the music frame, the VBI music engine is idle (`$0655/$0651/`
  `$0653=0`) and `station_audio` phase is static — the attract music is the
  **POKEY timer-IRQ** routine (`VTIMR1=$54EA`, `VTIMR2/4=$54C0`; song data near
  `$731E`). POKEY AUDF/AUDC are write-only (not in flat RAM) — parse the `.a8s`
  POKEY chunk if literal notes are needed.

### Rework approach — OPTION B (surgical native), user-confirmed 2026-06-08
Keep the native/snapshot architecture (do **not** run the full transpiled standby
loop). Target screen 3. Supersedes M6b–M6e below.

**R1 — Static screen-3 render** (no animation, no audio yet)
- New boot snapshot: extract screen-3 RAM from `music_playing.a8s` →
  `disasm/screen3_mem.bin`; point `incbin.s` at it; drop `standby_mem.bin` + the
  RTCLOK-reset hack; stop calling `station_audio`/`station_sub_*`.
- Render the `$3000` DL natively: terrain/door = **GTIA mode 10** (`$2000`/46,
  nibble→palette, ~4 bitplanes + Copper colour splits per the DLI schedule);
  title = mode-6 `$32B5` (already correct); cockpit = modeD `$350D` (2bpp) + mode4
  `$332D` (charset `$3800`). Validate static frame vs `attract_target.a8s`.

**R2 — Animation (native reimpl)**
- Reverse-engineer the screen-3 drivers near `$5A78`/`boot_standby_launch_driver`: title flip
  (RESCUE ↔ ©1985 LUCASFILM + palette change), door open/close, blinking lights
  (NOT `station_anim_frame`, which is `$195D`). Native-C reimpl mutating `mem[]`.

**R3 — Music (extract + native Paula)**
- Reverse-engineer the timer-IRQ routine `$54C0`/`$54EA` + song format (data near
  `$731E`); extract the attract song stream; build a native Paula sequencer →
  existing POKEY→Paula backend (NOT IRQ emulation).

**R4 — Integration + parity** vs the captures.

**Recommended start: R1** (mostly understood; reuses the SDL GTIA-10/cockpit
analysis in the `rof-project` memory; immediate visual confirmation).

---

## Source-material roles (what we take from where)

| Source | What we reuse | What we DON'T take |
|---|---|---|
| `dA JoRMaS/Template/C++` | the hardware classes `AmigaHardware`/`Bitmap`/`CopperList`/`Sprite`/`Palette`; the proven **GCC build** (`Makefile`, `GCCRuntime.cpp` runtime + L3 int trampoline, `SASCCompat.h`, `incbin.s`, `compat-include/`, `run.sh`/`debug.sh`) | `ModulePlayer`/`TrackerPackerReplayV3.1.s` (we don't do tracker audio), `Production`/`Part`/`Script`/`ProductionRunner` (demo timeline), `ExampleProduction`/`ExamplePart`, `main.cpp` |
| `dA JoRMaS/Productions/JRm-bS75/Source` (`GameCopperList`, `GamePart` sprite code) | the **serious copper + sprite pattern**: subclass `CopperList` → `GameCopperList` with a `writeCopperlist(...)` that **rebuilds the per-scanline copper every frame** (`showScroller`/`showHorizonBack`/`showObjects`/`showHorizonParallax`/`showRoad`/`showSprites`), per-scanline `WAIT` colour/mode changes, **all 8 hardware sprites** driven together (`showSprites(s0..s7)`, grouped as `BikeSprite`; `setBikeSpritePosition`/`showCurrentBikeSprite`), double-buffered copperlists | the racing-game logic (road geometry, bike physics) |
| `tmp/attackofthepetsciirobots` (`Platform.h`, `PlatformAmiga.cpp`, `petrobots.cpp` main loop) | the **app skeleton**: `main()` → system setup → install VBI int server (`AddIntServer(INTB_VERTB,…)`) → game-style state-machine loop; non-blocking `readKeyboard`/`readJoystick`; `renderFrame(waitForNextFrame)` = `WaitTOF`; chip-RAM alloc + interleaved bitplane bitmap; blitter tile/rect ops | its tile/map engine, gzip/Bin2Hunk specifics (we use `incbin`) |
| `dA JoRMaS/Utilities/WHDLoadMenu` (`WHDLoadMenuAnimated.cpp`, `AmigaView`, `AmigaCopperList`, `MenuView` loop, `Joystick`/`Keyboard`) | the **animated-copperlist idioms**: double-buffered copper lists swapped per frame (`LOFlist = copperList->data()`), per-frame copper edits (`setFade`/`setColor`/sprite-pointer swap/`setXOffset`), palette-fade infra, `WaitTOF`+poll loop, sprite-frame animation (the bouncing-ball pattern) | the WHDLoad menu logic itself |
| RoF repo: `src/gen/rof_gen.c` attract + audio routines + `cpu`/`bus`/`mem[]`, `PlatformSDL.cpp` POKEY synth | **attract state machine**: `vbi_handler_station`, `station_anim_frame`, `station_sub_1EB4`, `station_star_fade_in`, `pmg_colors_station`, `station_sub_1F48` compiled for m68k — these update `mem[]` each frame; **audio**: `station_audio` + song/SFX data → Paula via POKEY→Paula backend; `PlatformSDL`'s `audioCallback` algorithm as the spec | the rendering/terrain core (native instead); the full game loop (game routines only added when needed) |
| RoF repo (`atari000.png`, `tools/compare.py`, docs) | the **parity oracle** + the per-element Amiga technique map (`hw-techniques.md` §11.8) | running the 6502 rendering core on Amiga |

> **One skeleton, OS-friendly vs takeover — decide at M0.** PETSCII is
> *OS-friendly* (`OpenScreen` CUSTOMBITMAP + `AddIntServer`, multitasking-safe,
> WHDLoad-able). WHDLoadMenuAnimated / dA JoRMaS lean *takeover* (`LoadView`,
> direct `dmacon`, own copperlist) for full Copper freedom. For a Copper-heavy
> attract that becomes a game, **lean takeover** (own copperlist, raw Paula
> registers written by the POKEY→Paula audio backend) but keep the explicit
> PETSCII-style loop. Note the choice in `amiga/ARCH.md`; it only affects M0 setup
> + how the audio backend pokes Paula, not the milestones.

---

## Foundations (already established — don't re-litigate)

- **Toolchain** (`amiga-spike.md`, `Template/C++/GCC-PORT.md`): BartmanAbyss
  `vscode-amiga-debug` v1.8.2, installed at **`~/.local`**
  (`~/.local/opt/bin/m68k-amiga-elf-gcc` 15.1.0 + gdb/as/objdump,
  `~/.local/{elf2hunk,vasmm68k_mot,fs-uae}`, `~/.local/support/`). **Not on
  `PATH` by default** — `amiga/env.sh` exports it.
- **Target 68000** (`-m68000 -mcpu=68000 -msoft-float`), `-nostdlib`,
  `-fno-rtti -fno-exceptions`. Run as **OCS A500, 1 MB chip + 8 MB fast, KS 3.1**
  (auto-boots directory HDs; KS 1.3 stalls). All custom-chip register pointers
  **`volatile`**. vasm asm assembled with **`-no-opt`** (else `bra.w`→`bra.s`
  shifts PC-relative jump tables → corruption — the music-replay crash bug).
- **Per-element Amiga technique** is already worked out in `hw-techniques.md`
  §11.8 — that table is the contract for milestones M3–M5.

### Prerequisites (resolved)
- **KS 3.1 Kickstart ROM:** `~/Documents/RetroPie/BIOS/kick31.rom`
  (→ `kick40063.A600`, 512 KB — auto-boots directory HDs). `amiga/env.sh` should
  default `KICKSTART` to it; the run scripts read `$KICKSTART` or `$1`.
  (KS 1.3 `kick13.rom` and 2.0 `kick20.rom` also live there — don't use them,
  KS 1.3 stalls on directory HDs.)

### Parity workflow (used every milestone)
1. Capture the reference frame from the **working SDL build** (ground truth):
   `ROF_SHOT_FIRST=… ROF_START=attract ./build/rof` → `rof000.png` (or use the
   committed `atari000.png`).
2. Build + run the Amiga version; screenshot FS-UAE (F12 menu, or `--screenshot`).
3. `tools/compare.py amiga_shot.png atari000.png` → side-by-side + diff count.
   Tune palette/positions until close. Parity is "looks right", not pixel-exact
   (OCS 12-bit palette ≠ NTSC; document residuals).

---

## Current-state corrections (2026-06-08b) — fix these before resuming M6

The M2–M5 code (`amiga/src/StandbyScene.cpp`, `amiga/framework/CopperList.cpp`)
shipped with several incorrect assumptions. They must be corrected first because
every later milestone (M6a–M6e) builds raster splits and blits on top of this
geometry and timing model. Each item below is a concrete edit with the *why*.

### C1 — Colour-register writes are IMMEDIATE, not one-scanline-latent ★ root error

The previous "one-line-early colours, pointers next line" scheme
(`StandbyScene::buildCopperList`, the two-WAIT pattern at `kTerrainLine-1` /
`kTerrainLine` and `kCockpitLine-1` / `kCockpitLine`) is based on a **false
premise**. There is **no one-scanline pipeline latency** on OCS colour registers
— a Copper `MOVE` to `COLORxx` takes effect immediately, at the very next pixel
the beam draws.

**The real technique for a clean horizontal colour split:** make the Copper
`WAIT` for the **end of the *previous* scanline** (a horizontal position out in
the right border / horizontal-blank, i.e. the overscan area), then issue the
colour `MOVE`s there. The writes land during H-blank so the new palette is
already active for the first visible pixel of the target line. No artefact, no
"blue stripe", and no wasted scanline.

Replace the two-WAIT-per-boundary pattern with a **single WAIT in the overscan of
the line before the boundary**, doing bitplane pointers *and* colours together —
**pointers FIRST, colours after**:

```c
// Switch to region B starting at raster line R (= kTerrainLine / kCockpitLine).
// Wait until late on line R-1 (out in the right border / H-blank), then write.
d[idx++] = copperWait(R - 1, 0xE0);          // end of previous line, in overscan
d[idx++] = copperMove(bpl1pth, ...);          // bitplane pointers FIRST
d[idx++] = copperMove(bpl1ptl, ...);
d[idx++] = copperMove(bpl2pth, ...);
d[idx++] = copperMove(bpl2ptl, ...);
d[idx++] = copperMove(color00, fadeColor(..., f));   // colours after — immediate
d[idx++] = copperMove(color01, ...);
d[idx++] = copperMove(color02, ...);
d[idx++] = copperMove(color03, ...);
```

- **Pointers before colours, deliberately.** The two register groups are not
  equally timing-critical. If the **bitplane pointers** miss their H-blank window,
  bitplane DMA fetches from the wrong address and the *entire* display goes off
  (catastrophic). If a **colour** MOVE slips a little late, it just applies a few
  pixels into the line (a cosmetic edge artefact). So give the pointers the
  earliest slots in the WAIT block and let colours take whatever follows. (This is
  the opposite of the old code's colours-first ordering, which was rationalised by
  the now-debunked latency premise.)
- `0xE0` is a horizontal position in the right border; tune within ~0xC0–0xE2 if
  needed, but it must be a position the beam actually reaches on line `R-1`
  (a value past the max H-count makes the Copper hang until line wrap).
- Delete the comments in `buildCopperList` that explain the bogus "OCS colour
  pipeline has a one-scanline latency" — they document a non-existent effect.

### C2 — Total display height is 216, not 200; top is 0x2c, not 0x44

`StandbyScene.cpp:38` has `kH = 200`. The Atari attract frame is **216 visible
scanlines** (confirm against `atari000.png`; cf. `hw-techniques.md` §11.1
"Total ≈ 216 scanlines"). Set `kH = 216`.

The display window must begin at the standard PAL top **scanline 0x2c (44)**.
`CopperList::setPlayfield` computes `DIWSTRT.y = centerY - height/2`. With the
default `centerY = 0xa8` and the old `kH = 200`, the top came out at
`168 - 100 = 0x44` — wrong. To anchor the top at `0x2c` with `kH = 216`:

```
centerY = kDisplayTop + kH/2 = 0x2c + 108 = 152 = 0x98
```

Pass `centerY` explicitly to `setPlayfield` (don't rely on the `0xa8` default).
Then verify the resulting `DIWSTRT`/`DIWSTOP`/`DIWHIGH` actually place the top at
0x2c in FS-UAE (`DIWSTOP.y` = 260 > 256, so the V8 bit in `DIWHIGH` must be set —
the framework's hardcoded `diwhigh = 0x2100` already carried the old 268 case, so
confirm it still does for 260).

### C3 — Region boundary lines must be DERIVED constants, not magic numbers

`StandbyScene.cpp:45-46` hardcodes `kTerrainLine = 110`, `kCockpitLine = 196`.
These happened to be self-consistent with the *wrong* top (0x44) but are wrong
for 0x2c. Replace the magic numbers with derived constants so the geometry is
self-documenting and re-anchoring the top can't desync them:

```c
static const uint16_t kDisplayTop    = 0x2c;                      // DIWSTRT.y (PAL std top)
static const uint16_t kTitleHeight   = 42;                        // title region (display lines)
static const uint16_t kTerrainHeight = /* from M6a audit */;      // terrain region
static const uint16_t kTerrainLine   = kDisplayTop + kTitleHeight;        // = 0x56 (86)
static const uint16_t kCockpitLine   = kTerrainLine + kTerrainHeight;
```

So with the display starting at `0x2c`, the green terrain region begins at
`0x2c + 42 = 0x56` exactly. Sprite Y (`leftPost/rightPost->setY`) and the Copper
WAITs all reference these constants.

> **Open: reconcile region heights to sum to 216.** title 42 + terrain + cockpit
> must equal **216**. The current bitmaps are `title 42`, `terrain 86` (`kHT`),
> `cockpit 104` — that sums to **232**, inconsistent with 216. `kTitleHeight = 42`
> is correct (matches `title.raw`). Derive `kTerrainHeight` and the cockpit height
> from the **M6a ANTIC display-list audit** of the *attract* DL (not the gameplay
> `$3210` DL), then regenerate `terrain.raw`/`cockpit.raw` to those exact row
> counts. **Each bitmap's row count must equal its region's display-line count.**

### C4 — Use C++ implementations only (do NOT define ASSEMBLER)

Keep building with `NO_ASSEMBLER` (the whole-plan build, per M0). The
`#if defined(ASSEMBLER)` register-marshalling wrappers in
`CopperList.cpp:51-78` are bypassed; the pure-C++ `showSprite`/`showBitmap`
bodies (`CopperList.cpp:80-105`) are used. This keeps stepping in gdb
straightforward (no asm thunks, runtime PCs resolve to C++ symbols). Only
introduce the asm path later if a profiled hot spot demands it.

### C5 — Drop the redundant `d[0] = copperWait(16, 0)` preamble

`StandbyScene::buildCopperList` writes `d[0] = copperWait(16, 0)` then starts at
`idx = 1`. This is redundant: `CopperList::CopperList` (`CopperList.cpp:24-27`)
already initialises `data_[0] = copperWait(16, 0)` (and the terminating
`data_[length-1] = copperWait(255, 254)`) at allocation. Remove the manual `d[0]`
write from `buildCopperList`; keep `idx = 1` so the constructor's preamble is
preserved. (The earlier `amiga-copper-lessons` "d[0] preamble" note is satisfied
by the constructor — don't re-do it per frame.)

### C6 — Wrong audio segment: use the SDL build as the working oracle

> **⚠️ RE-DIAGNOSED in CRITICAL REVISION (2026-06-08c).** Root cause is NOT segment
> selection in `mem[]` — it's the **wrong code path**: the Amiga runs the `$195D`
> cinematic (`station_audio`), not the target screen 3's timer-IRQ music. See the
> revision's R3. Keep the "SDL is the oracle" principle below; ignore the
> "segment-selection state" hypothesis.

The Amiga attract currently plays the wrong segment (pre-attract cinematic music,
not the attract melody). **The SDL build (`build/rof` + `atari800`) plays the
*correct* attract music** — so it's the ground-truth oracle for what the audio
state machine must do, exactly like `atari000.png` is the visual oracle.

Don't guess the Amiga side from scratch — trace the working SDL path first:
- Run the SDL build into the attract (`ROF_START=attract`) and capture the call
  sequence / `mem[]` state that precedes correct attract music: which init runs
  before `station_audio`, what selects the melody segment vs the cinematic, and
  the values of the relevant POKEY shadow / song-pointer locations at that point.
  Use the `atari800` debugger (FIFO mode) to diff the working segment-select
  state against what the Amiga has when it plays the wrong one.
- The likely culprit is **initialization / segment-selection state in `mem[]`**,
  not the POKEY→Paula backend (M5) itself: the Amiga loads the XEX into `mem[]`
  but may not run the same pre-attract init the Atari does, so `station_audio`
  reads a stale/default song pointer and plays the cinematic. Mirror whatever
  the SDL build does to reach the attract-music state before the first
  `station_audio` call.
- This ties into **M6b** (running the full 6502 attract loop on m68k): once the
  real attract state machine runs in order, the correct segment selection should
  fall out — verify the song-pointer `mem[]` bytes match the SDL build's at the
  same frame.

### Correction checklist
- [ ] C1: replace two-WAIT colour scheme with single end-of-previous-line WAIT
      (overscan); colours + pointers together; immediate writes; delete latency
      comments. Applies to both terrain and cockpit boundaries.
- [ ] C2: `kH = 216`; pass explicit `centerY = 0x98` to `setPlayfield`; verify
      top renders at 0x2c and DIWHIGH covers `DIWSTOP.y = 260`.
- [ ] C3: replace magic `kTerrainLine`/`kCockpitLine` with derived constants from
      `kDisplayTop`/`kTitleHeight`/`kTerrainHeight`.
- [ ] C3-open: confirm region heights sum to 216 via M6a; regenerate
      `terrain.raw`/`cockpit.raw` to the audited row counts.
- [ ] C4: confirm build stays `NO_ASSEMBLER`; C++ bodies in use.
- [ ] C5: remove redundant `d[0]` write in `buildCopperList`.
- [ ] C6: trace the SDL build's working attract-music path; mirror its
      init/segment-selection `mem[]` state on the Amiga (verify in M6b).
- [ ] Rebuild, run in FS-UAE, `tools/compare.py` vs `atari000.png` — terrain top
      at 0x56, no boundary stripe, full 216-line frame.

---

## Milestones

Each milestone ends in a **single commit** with a green build that **runs in
FS-UAE without crashing**. Commit message convention: `feat(amiga): …`.
Don't start the next milestone until the current one runs and is committed.

### M0 — Scaffold `amiga/`, app skeleton boots
**Goal:** the build produces a HUNK exe whose **own `main()`** sets up the system,
installs a VBI interrupt server, and runs a `while (!quit)` loop showing a
solid Copper-driven background colour. Proves toolchain + build + run + the
PETSCII-style skeleton (no demo `ProductionRunner`).

- [x] `mkdir amiga/`. Vendor the **hardware layer + GCC build** from
      `dA JoRMaS/Template/C++` into `amiga/framework/`: `AmigaHardware`,
      `Bitmap`, `CopperList`, `Sprite`, `Palette`, `Util`, plus `GCCRuntime.cpp`,
      `SASCCompat.h`, `incbin.s`, `compat-include/`, `Makefile`, `run.sh`,
      `debug.sh`. **Omit** `ModulePlayer`/`TrackerPackerReplayV3.1.{h,s}` (no
      tracker audio — see M5), `Production*`, `Part*`, `Script*`,
      `ProductionRunner*`, `ExampleProduction*`, `ExamplePart*`, `main.cpp`. Drop
      SAS/C cruft (`smakefile`, `*.info`, `SCoptions`, `Debug/`). Record upstream
      version in `amiga/framework/UPSTREAM.md`. (The RoF audio-core slice —
      `cpu`/`bus`/`mem[]` + the converted audio routines — is added at M5, not now.)
      With no framework `.s`, **`make NO_ASSEMBLER=1` is the build for the whole
      plan** (no vasm step needed).
- [x] Write `amiga/ARCH.md` recording the takeover-vs-OS-friendly choice and the
      two-layer split (hw classes + PETSCII-style skeleton).
- [x] Add `amiga/env.sh` exporting the `~/.local` toolchain onto `PATH`
      (`opt/bin`, root, `fs-uae`) + a default `KICKSTART`. Verify the link
      succeeds from inside the space-containing repo path (Template avoids LTO,
      so it should; if not, symlink to a space-free path à la the spike).
- [x] Write the skeleton (model: `petrobots.cpp` `main()` + `PlatformAmiga`):
      `amiga/src/main.cpp` → system setup (own copperlist / `LoadView`; or
      OS-friendly `OpenScreen` per ARCH.md), `AddIntServer(INTB_VERTB, …)` for a
      VBI server that ticks a frame counter, then
      `while (!quit) { pollInput(); update(); render(); WaitTOF(); }`. A tiny
      `StandbyScene` object owns the per-frame `update()`/`render()`. Exit on left
      mouse / joystick fire (WHDLoadMenu `Joystick`/`Keyboard` polling pattern).
- [x] Build the `CopperList` to set background `COLOR00` to a recognisable blue.
- [x] `make NO_ASSEMBLER=1`; `./run.sh`. Confirm the blue screen + clean exit.
- [ ] **Commit:** `feat(amiga): scaffold app skeleton (PETSCII-style loop + dA JoRMaS hw layer); blank screen boots`.

**Gotchas:** no spaces on the build path (LTO — Template avoids it, verify);
`HOME` must be set for the gdb path (debug only); `127.0.0.1` not `localhost`;
restore system state on exit (the skeleton's teardown).

### M1 — Asset pipeline + static title/cockpit bitplane image
**Goal:** the whole attract frame as one multi-bitplane bitmap with a flat
palette, shown via `CopperList::showBitmap`. "RESCUE ON FRACTALUS!" + cockpit
silhouette + terrain block visible (colours approximate; refined in M2).

- [ ] Stand up the asset converter. **Option A (preferred):** build
      `dA JoRMaS/Utilities/RAWConverter` (Qt) → interleaved bitplanes + 12-bit
      palette, the exact format `Bitmap`/`showBitmap` expect. **Option B
      (fallback):** a small `amiga/tools/png2raw.py` (PIL) emitting interleaved
      planes + OCS palette. *Pick A; fall back to B only if Qt fights us.*
- [ ] Source art: capture a clean attract frame from the SDL build (or crop the
      committed `atari000.png`) to **320×256** (PAL lores), quantise to ≤16
      colours (4 bitplanes). Convert → `amiga/assets/attract.raw`.
- [ ] Embed via `incbin.s` (`.incbin "assets/attract.raw"`), chip-RAM section.
- [ ] In `StandbyScene::initialize()`: a `Bitmap` over the incbin'd data, a
      `CopperList` with `setPlayfield(320,256,4,interleaved=true)` +
      `showBitmap()` + a flat 16-colour `Palette`; enable bitplane DMA.
- [ ] Build, run, screenshot, `compare.py`. **Commit:**
      `feat(amiga): asset pipeline + static attract bitplane image`.

**Gotchas:** interleaved vs chunky must match the `Bitmap` `interleaved` flag;
word-align width to 16; DDFSTRT/DDFSTOP/DIWSTRT come from `setPlayfield`.

### M2 — Copper colour splits (replace the Atari DLIs) ★ the showcase
**Goal:** instead of one flat palette, a **Copper list rewrites colour registers
at the region boundaries**, reproducing the Atari DLI register schedule. The
idiomatic "more colours than registers" technique and the heart of the look.

> **Reference pattern — `JRm-bS75/GameCopperList`.** Subclass the base
> `CopperList` into an `AttractCopperList` with a `writeCopperlist()` that emits
> the whole list (region colour splits, later sprite pointers) and is **rebuilt
> per frame** for animation — exactly how bS75 rebuilds its per-scanline road
> copper each frame. Double-buffer two instances and flip (bS75 + WHDLoadMenu).

- [ ] Transcribe the DLI schedule. Sources: `hw-techniques.md` §2.1 (the
      sequenced `dli_handler_game $49EE` slots) + the hand-authored handlers in
      `src/gen/rof_manual.c` (`dli_sub_4a11/4a1f/4a40/4a78/4acd`, cockpit `$6Dxx`
      slots) — each is "at scanline N, set register R = V". Build the table
      `{scanlineY, register, value}`.
- [ ] Emit `WAIT(y)` + `MOVE(reg,val)` Copper instructions at each boundary:
      terrain palette at terrain top (COLBK=$14 brown, COLPF2=$B8 salmon,
      COLPF1=$10, COLPF0=$2A per §11.2 → Amiga COLORxx), title-line colours up
      top, grey/teal cockpit-panel colours below the terrain, throttle fill band.
      Use `CopperList::setColor`/`setPalette` at a raster-line `listIndex`.
- [ ] **Animated** attract colour cycle (WHDLoadMenu `AmigaCopperList` idiom):
      double-buffer two copper lists and rewrite the palette MOVEs per frame in
      the VBI/`update()` (Atari sweeps `$08D4–$08D9` over ~90 frames). Swap
      `LOFlist` each frame like `MenuView`.
- [ ] Build, run, compare. **Commit:**
      `feat(amiga): copper colour splits reproduce DLI register schedule`.

**Gotchas:** Copper `WAIT` Y is PAL display coordinates — map the Atari NTSC
scanlines (§11.1, ~216 lines) onto the PAL 256-line display; proportions matter
more than absolute lines. Modify the **off-screen** copper list, then flip.

### M3 — Terrain viewport as a 2-bitplane region
**Goal:** the terrain window per `hw-techniques.md` §11.2 — a 4-colour
(2-bitplane) bitmap, **one fixed palette**, sky/ground/mountain as pixel values.
For the attract milestone use a **static converted terrain image** (live fractal
generation is a separate later phase — Tier 3).

- [ ] Convert a representative terrain frame (SDL/atari800) → 2-bitplane
      `amiga/assets/terrain.raw` (~160 px × ~94 rows viewport).
- [ ] Composite: dual-playfield (terrain planes behind cockpit planes) **or**
      blit the terrain bitmap into the viewport rows each frame
      (`Bitmap::copy`, blitter). Keep the §11.2 palette as the M2 terrain band.
- [ ] Build, run, compare. **Commit:**
      `feat(amiga): terrain viewport (static, 2-bitplane fixed palette)`.

> **Deferred — live fractal terrain (Tier 3).** Reverse `terrain_gen_1/2/3`,
> `project_terrain_points`, the RANDOM/LFSR fractal; write native C; validate the
> heightfield against the `__t6502` oracle via `make validate` (the
> `native-reimpl-seam`); draw via blitter. The real engine work — **out of scope
> for the attract milestone** (the attract is a static/looping scene).

### M4 — Cockpit chrome & gauges as hardware sprites
**Goal:** map the Atari Player/Missile chrome onto Amiga hardware sprites per
§11.3/§11.8. Phase-5 notes already say what each PM object *is*.

- [ ] **Canopy frame posts** (Atari missiles M0–M3, pinned at window edges) → two
      static vertical hardware sprites (left/right frame).
- [ ] **Instrument gauges** (AH = P2, throttle = P1, centre gauge = P0) → sprites
      or small blits into the dashboard bitmap, positioned via Copper sprite
      pointers. Static positions are fine for the attract.
- [ ] `Sprite` + `CopperList::showSprite`; colours from §11.3 PCOLR (~$2A green).
- [ ] **Reference pattern — `JRm-bS75`** drives all 8 hardware sprites together
      (`GameCopperList::showSprites(s0..s7)`, grouped as a `BikeSprite` with
      `setBikeSpritePosition`/`showCurrentBikeSprite`) and **WHDLoadMenu
      `AmigaView`** swaps sprite pointers per frame (`setSprites`) for animation —
      use these for laying the canopy posts + gauges into the copper list and any
      per-frame sprite motion.
- [ ] Build, run, compare. **Commit:**
      `feat(amiga): cockpit frame + gauges as hardware sprites`.

### M5 — Audio: 6502-converted player → Paula
**Goal:** run the game's **own** audio player (the transpiled POKEY-poking
routines + the game's custom song/SFX data) and translate its POKEY register
writes to Paula. No tracker, no MOD, no `ModulePlayer` — this *is* the real
player, so the attract jingle is bit-faithful by construction.

- [ ] **Compile the RoF audio-core slice for m68k.** Bring `cpu`/`bus`/`mem[]`
      (`src/cpu/`) + the converted audio functions from `rof_gen.c` into the
      Amiga build. For the attract that's `station_audio $1B5B` (+ whatever it
      calls) and its data tables; the game path later adds `music_player_tick`,
      `audio_timer_setup`/`audio_irq_handler`, `standby_level_select_loop`. Keep this a
      *narrow* slice — link only what the player references.
- [ ] **POKEY→Paula bus backend.** Route `bus_write` for `$D200–$D20F`
      (AUDF1-4/AUDC1-4/AUDCTL/SKCTL) into a POKEY model that drives Paula:
      each channel points at a short waveform in chip RAM (square wave; a noise
      buffer for poly/noise AUDC modes), **Paula period = f(AUDF divider +
      AUDCTL clock-select/16-bit-chain)**, **AUDxVOL = AUDC low nibble**. Mirror
      the exact mapping already written in `PlatformSDL.cpp`
      (`updateChannelFreq`/`updateChannelCtrl`/`audioCallback`: `POKEY_CLOCK`,
      `÷28`/`÷114`, the `0x20`/`0x40` 1.79 MHz bits, the `0x08`/`0x10` 16-bit
      chains) — that's the spec, just emit Paula registers instead of summing
      samples.
- [ ] **Port the POKEY RANDOM LFSR.** `station_audio` reads `$D20A` (17-bit LFSR)
      for volumes — port `Platform::pokeyRandomStep` so `bus_read($D20A)` is
      bit-exact (small; also future-proofs terrain parity).
- [ ] **Drive it per frame from the VBI server.** The attract loop calls
      `station_audio` each frame — call the converted function from the VBI
      handler (or main loop), then let the Paula backend hold the tones until the
      next update.
- [ ] Build, run (listen vs the SDL build). **Commit:**
      `feat(amiga): 6502-converted audio player → Paula backend`.

**Gotchas:** `volatile` on the frame-ready flag (the `-O2` infinite-spin bug from
the spike); Paula needs the waveform in **chip RAM**, length/period set before
the channel DMA is enabled; watch the AUDF→period scaling for the PAL clock
(3546895 Hz) vs NTSC. The audio-core slice must not drag in rendering code —
stub any `bus_write` to display registers.

### M6a — ANTIC display list audit ✓ DONE
**Goal:** establish the exact rendering model by reading the Atari attract display
list from the memory dumps. Every subsequent milestone depends on this.

- [x] Parse `a800dumps/attract.a8s` (binary Atari state snapshot): locate the
      display list (DLISTL=$D402, DLISTH=$D403 registers give the address; VBI
      writes $1C35 = $1C×256+$35, so DL is at $1C35). Walk the DL byte-by-byte:
      decode each entry (mode bits 3-0, flags bits 7-4, optional LMS address).
      Record: ANTIC mode, scanline range, screen RAM address, for every section.
- [ ] **Confirmed by code analysis (do not re-derive):**
      The title/terrain section is **ANTIC mode F** (1bpp bitmap, 320 px, 1
      scanline/entry, 40 bytes/row), 122 entries pointing into `$0600`. Title text
      and terrain texture are **pre-rendered pixels** stored in ROM source buffers;
      `station_sub_1EB4` copies them into `$0600`. No font needed for this section.
- [ ] Identify the cockpit section: expected to be ANTIC mode 2 (40-column
      character mode, 8×8 glyphs, 8 scanlines/row) or similar. Record the CHBASE
      register value (screen RAM for charset pointer is at `$D409`; check the
      attract state dump for its value).
- [ ] Map each ANTIC section to a Copper action: bitmap region → blit from
      `mem[]`; character region → character render; DLI colour changes → Copper
      WAIT+MOVE.
- [ ] Document the full mapping in `amiga/ARCH.md` (update §Attract).
- [x] **Committed** as part of M6b commit (findings in plan, no separate commit needed).

**M6a confirmed results (attract.a8s, DL at $3000):**
- Blank preamble: 27 sl (8+8+8+3)
- Title text: Mode 6 × 1 row = 8 sl at $32B5 ("RESCUE ON FRACTALUS")
- Gap: 3 sl blank
- Subtitle strip: Mode 4 × 1 row = 8 sl (charset art)
- Separator: Mode D × 1 row = 2 sl
- **Bitmap (Mode F): 86 rows × 1 sl = 86 sl**, starting at $2000, stride 46 bytes/row
- Mode D transition: 4 rows × 2 sl = 8 sl (at $350D — artificial horizon)
- **Cockpit (Mode 4): 10 rows × 8 sl = 80 sl**, screen RAM at $332D
- Total: 222 sl (27 blank preamble + 195 content); Amiga kH=216 clips 6 blank top lines — correct
- Amiga geometry confirmed: title 42 sl (covers 27 blank + start of content), terrain/bitmap 86 sl, cockpit 88 sl (8+80) — sums to 216 ✓
- **CHBAS shadow ($02F4) = $02** → custom charset at $0200
- Cockpit screen RAM: $332D; bitmap screen RAM: $2000 (stride 46, not 40 — ANTIC ignores 6 padding bytes)

### M6b — Full 6502 attract loop on m68k
> **⚠️ SUPERSEDED by CRITICAL REVISION (2026-06-08c).** The functions wired up here
> (`station_audio`, `station_anim_frame`, `station_sub_1EB4/1F48`, `pmg_*_attract`)
> are the **`$195D` space-station CINEMATIC**, not the target attract screen. This is
> the core bug. Replaced by Option-B R1 (drop these calls) + R2 (native reimpl of
> screen-3's own drivers near `$5A78`). Kept below for history.

**Goal:** the m68k runs every attract-state-machine function the Atari runs each
frame. After this milestone `mem[]` is updated correctly every frame, making all
subsequent rendering accurate.

- [x] **VBI interrupt**: `main.cpp` `vbiHandler()` now increments
      `mem[0x0080]`, `mem[0x0014]` + carry into `mem[0x0013]`. DLIST/COLBK
      writes from the Atari VBI skipped — Copper owns those.
- [x] **Main loop (expanded `StandbyScene::update`)**: calls in order:
      `station_star_fade_in()`, `station_audio()`, `station_anim_frame()`,
      `station_sub_1EB4()`, `pmg_colors_station()`, `station_sub_1F48()`.
- [x] Removed manual timer increment from `update()` and standalone
      `station_audio()` call (subsumed into the ordered sequence).
- [x] Fixed missing `mem[0x0080]++` (was never incremented before — animation
      state machine was stalled).
- [ ] Verify `mem[$0600..$06FF+]` changes each frame as the attract animation
      cycles (log a few bytes; compare against the SDL build's `atari800` debugger
      state at the same frame).
- [ ] **Commit:** `feat(amiga): full attract state machine running on m68k`.

**Gotchas:** `station_sub_1EB4` accesses tables at `$2313`/`$231B` — these must
be loaded into `mem[]` from the XEX. Verify the XEX loader populates them.
`station_pm_shape_tick` / `pmg_colors_station` write PMG RAM at `$3400–$3500` and
POKEY/GTIA registers — stub those bus writes; we drive sprite positions separately.
⚠ **Corrected 2026-08-12:** `station_star_fade_in` (`$1E79`, was `pmg_update_station`) is NOT one
of them — it touches no PMG RAM at all, it brightens the mode-F **star rows** `$2CB8-$3167`. The
PMG shape writer is `station_pm_shape_tick` (`$1E01`, was `dli_handler_station`).
See `docs/logo-station-plan.md` §4.

### M6c — Bitmap region: blit `mem[$0600]` to title+terrain bitplane
> **⚠️ SUPERSEDED by CRITICAL REVISION (2026-06-08c).** Two errors: (1) the terrain
> source for the *target* (screen 3) is `$2000` stride 46, **not `$0600`** (`$0600`
> is the cinematic's source); (2) it must be decoded as **GTIA mode 10** (4-bit
> nibble → colour), **not 1bpp** — the 1bpp read is the "vertical stripes". Replaced
> by Option-B R1. Kept below for history.

**Goal:** the Amiga title/terrain region shows exactly what the Atari's mode-F
bitmap shows: the pre-rendered title text and terrain texture driven by the
running 6502 attract routines.

- [ ] **Use the CPU (not the blitter) for all rendering in this milestone.**
      The 68000 CPU can write 40 bytes × 86 rows = 3440 bytes in well under a
      frame — the blitter is overkill and complicates debugging.
- [ ] **Title region (42 lines):** render Mode 6 text ("RESCUE ON FRACTALUS!")
      and Mode 4 subtitle strip using a CPU character-rendering loop (see M6d
      charset extraction). The 27-line blank preamble and the 3-line gap map to
      color00 (background) — just leave those rows zeroed. Effective content rows
      within the 42-line title bitmap: ~rows 0-27 black, rows 28-35 title text,
      rows 36-41 subtitle/separator.
- [ ] **Terrain region (86 lines):** `station_sub_1EB4()` writes 1bpp pixels into
      the Mode F screen RAM at `mem[$2000]` (stride 46 bytes/row; ANTIC ignores
      6 padding bytes). CPU-copy 40 bytes per row from `mem[0x2000 + row * 46]`
      into `terrainBitmap->data` (interleaved stride = 80 bytes per row: 40 bytes
      bpl1 then 40 bytes bpl2; 1bpp → only bpl1 used, bpl2 stays zero for
      color index 0/1 only).
- [ ] The Copper palette for this region is 2 colours per ANTIC section (COLBK +
      COLPF2 for the title, terrain colours for the terrain band) — set by the
      Copper at the appropriate scanline, values read from `mem[$D01A]` (COLBK)
      and similar shadow registers updated by the attract routines.
- [ ] Drop `title.raw` / `terrain.raw` as static assets; the bitmapped content is
      now live from `mem[]`. The terrain static image was an approximation — the
      terrain pixels that appear in the attract (`terrain_gen` output baked into
      the ROM buffers) will emerge correctly from the 6502 attract routines.
- [ ] **Commit:** `feat(amiga): title/terrain bitmap driven by 6502 attract routines`.

**Gotchas:** `mem[]` is in fast RAM; the blitter source must be fast-RAM-capable
(it is — Amiga blitter can read from fast RAM and write to chip RAM). Width must
be word-aligned. The blit must complete before the Copper reaches the terrain
scanline — trigger it at VBI start, not mid-frame.

### M6d — Cockpit character display + font extraction
**Goal:** the cockpit panel renders exactly using the Atari custom character set,
with blinking lights driven by the 6502 blink timer.

#### Font extraction
- [ ] From the ANTIC audit (M6a): read the CHBASE register value from the attract
      state dump to find the charset base address. Each character = 8 bytes;
      256 characters = 2048 bytes. Extract that range from the appropriate
      `a800dumps/mem_*.bin` file.
- [ ] Horizontal scaling: ANTIC mode 2 renders 40 columns × 8px = 320px, matching
      Amiga lores 320px — **no horizontal stretching**. Convert the charset
      verbatim: each character is an 8×8 1bpp glyph, MSB = leftmost pixel (same
      as Amiga bitplane convention). Embed as `amiga/assets/charset.raw` in chip
      RAM via `incbin.s`.
- [ ] The diamond-shaped blinking lights are characters `$4E` (lit) and `$46`
      (unlit/off) in the custom charset. Verify their glyph data after extraction.

#### Cockpit rendering
- [ ] The cockpit screen RAM (address from M6a CHBASE audit) in `mem[]` holds
      character codes that change each frame. After calling the attract functions,
      walk the cockpit screen RAM row by row: for each character code, blit its
      8×8 glyph from `charset.raw` into the cockpit bitplane at the correct
      position. Use the Amiga blitter (cookie-cutter mode: 8-bit-wide source mask
      + destination write).
- [ ] **Blinking lights**: `update_blink_timer_006e` is a game-mode routine; check
      whether the attract loop calls an equivalent. Find what drives the light
      character toggle during attract and ensure that function is called each frame
      (it may already be in the attract loop sequence, or may need adding).
      The blink state is in `mem[$00DE]`; the Amiga just reads that and renders the
      current character — no independent Amiga blink timer.
- [ ] Cockpit Copper palette: 4 colours for the character mode region, values from
      the attract DLI colour registers, read from `mem[]` each frame.
- [ ] **Commit:** `feat(amiga): cockpit character display with extracted Atari font`.

#### 6502→proper-C migration track (when needed)
> Some transliterated routines in `rof_gen.c` use constructs that don't compile
> cleanly or behave incorrectly on m68k (computed indirect jumps, self-modifying
> code patterns, stack tricks). When a required routine falls into this category:
> 1. Write a proper-C reimplementation in `amiga/src/` that produces the same
>    `mem[]` mutations as the oracle.
> 2. Gate it with `VALIDATE_FUNCS`: the `__t6502` oracle twin (the transliterated
>    version) runs alongside; `make validate` diffs their `mem[]` outputs.
>    This is the existing `native-reimpl-seam` — reuse the same pattern.
> 3. Once validated, the proper-C version replaces the call in the attract loop.
>
> Priority candidates: any routine that uses `platform_indirect_jmp` (computed
> jump targets), writes to its own code, or manipulates the stack in ways the
> transliterator can't follow. Audit before M6b; fix only what actually breaks.

### M6e — Parity polish
**Goal:** frame-accurate visual match with the SDL reference.

- [ ] Capture SDL reference at a fully-faded-in frame (`ROF_SHOT_FIRST=100`).
      Capture Amiga screenshot. `tools/compare.py` side-by-side.
- [ ] Fix any remaining geometry issues: Copper split scanlines, sprite positions,
      character cell alignment.
- [ ] OCS 12-bit palette calibration: read colour register values from `mem[]`
      after the attract routines run; convert Atari NTSC colours to nearest OCS
      12-bit using the `PlatformSDL` NTSC→RGB algorithm as the reference.
- [ ] (Optional) test on real Amiga hardware.
- [ ] Write `amiga/PARITY.md`: known OCS residuals (12-bit palette quantisation
      error, PAL vs NTSC scan-line count delta), deferred elements.
- [ ] **Commit:** `feat(amiga): attract-screen parity pass`.

---

## Out of scope for this plan (later phases)
- Live fractal terrain generation + flight sim (Tier 3 native rewrite + oracle
  validation) — the attract static terrain emerges from the ROM buffers.
- Input/gameplay, collision (`terrain_collision_and_silhouette`), enemy/saucer logic, the full
  launch cinematic (doors/tunnel/stars/planet, `startup-flow.md` §6).
- These reuse the same skeleton + hardware layer + the `native-reimpl-seam`;
  sequence them after the attract proves the rendering/audio stack end-to-end.

## Definition of done (attract milestone)
The Amiga executable boots in FS-UAE (OCS A500) and shows the RESCUE ON
FRACTALUS! attract screen — title text (alternating with copyright line, timed
exactly as the 6502 code dictates), terrain bitmap, cockpit panel with blinking
lights — with Copper colour splits, sprite canopy posts, and Paula audio. Parity
with the SDL build is exact for timing and content; residuals are limited to the
OCS 12-bit palette quantisation documented in `PARITY.md`.

---

## SESSION STATUS (2026-06-08/09) — ACTUAL IMPLEMENTATION STATE

The plan above (M6a-M6e) is largely superseded by the Option-B rework. Here
is the accurate current state.

### What is working

All render for the Standby scene (scene 3, `$3000` DL) is complete and
visually verified on real FS-UAE:

- **Terrain** (`$2000`, stride 46): GTIA mode-10 nibble decode → 2bp bitmap.
  Colour 0=COLPM0 (black dots, $02C0=$00 patched), col3=COLBK (green, $0071=$C8).
- **Title** (`$32B7`-`$32CA`, 20 chars): mode-6 1bpp, charset `$0400`
  (CHBAS=$04 from VBI; DLI fires at scanY=28 = after title). `kDoubleGlyph[256]`
  lookup table. Palette: col0=COLBK (grey), col1=COLPF1 (blue).
  Pre-initialised with Block1 "rescue on fractalus" text from `$5A9F`.
- **Cockpit** (modeD `$350D` + mode4 `$332D`): `decode2bppByte()` for both.
  Palette: col0=COLBK, col1=COLPF0 (black dots), col2=COLPF1 (pink body).
- **Sprites**: canopy posts (Sprite 0/1, slanted staircase).
- **All palettes**: dynamic from `mem[]` via `atariToOCS()` (YIQ formula).
- **Music**: `sfx_voice_tick_native()` / `sfx_seq_step_native()` — fully native
  68000, no transpiled code (GC'd). SFX sequence drives Paula via POKEY→PaulaAudio.
- **Blink lights**: `update_blink_timer_006e_native()` drives `mem[$00DE]`.
- **VBI timers**: main.cpp interrupt server (RTCLOK), `vbi_handler_game_native()`
  ($062D cascade), blink timer.
- **Performance**: terrain+cockpit dirty-flag (render once); title 20-byte shadow
  compare; no full reconvert per frame.

### Architecture

`amiga/src/` files:
- `StandbyScene.cpp/.h` — scene lifecycle, render, update, copper list
- `SfxPlayer.cpp` — `sfx_voice_tick_native`, `sfx_seq_step_native`
- `NativeHandlers.cpp` — `vbi_handler_game_native`, `update_blink_timer_006e_native`
- `PaulaAudio.cpp` — POKEY→Paula bridge
- `main.cpp` — VBI interrupt server, main loop

### Snapshot patches (applied in initialize())

All from `music_playing.a8s` being mid-animation vs the correct standby state:
- `mem[$0071]=$C8` — terrain COLBK (green)
- `mem[$00D5]=$78` — title COLPF1 (blue text)
- `mem[$00D8]=$06` — title COLBK (grey background)
- `mem[$02C0]=$00` — terrain COLPM0 (black dots)
- `$32B7-$32CA` = Block1 — "RESCUE ON FRACTALUS!" title text

### Open items

1. **Standby animation**: `vbi_handler_2` ($4FF5) calls `startup_init` ($3FFA)
   and `vbi_deferred_dispatch` ($534D) each VBI — not called. Needed for cockpit
   digit updates, instrument animation. update() should call all standby-loop
   functions as named native functions.
2. **Title alternation**: Static. $0091 only incremented by `station_star_fade_in`
   ($1E79, Screen-2 function). Screen-3 standby is always static.
3. **R3b music timing**: sfx runs at 50Hz VBI, not POKEY timer rate.
4. **Scene 3b Scoreboard**: `attract_timer $00E2` → hi-score screen. Not analysed.
5. **Descent / door-open sequence**: next major feature after standby is complete.

### Title alternation (added 2026-06-09)

**Mechanism discovered:** `sfx_seq_step_native` writes SFX voice-param command bytes
from `$71DB` to `mem[$0091]` as a side effect. Bytes `$C0`/`$C4` (in `[$C0,$E0)`)
→ Block1 "rescue on fractalus"; `$E2` (≥`$E0`) → Block2 "©1985 LUCASFILM LTD".
`copy_altitude_graphic_to_screen_native` ($782A) fires when `$0091≥$C0`, copies the
20-char block to `$32B7-$32CA`, resets `$0091=0`. The SFX music loop drives the
alternation — `$C0` appears at seq[1], `$E2` at seq[12], `$C4` at seq[24].

**Implementation:** `copy_altitude_graphic_to_screen_native()` added to NativeHandlers.cpp.
Called from `update()` when `mem[$060B]==0` (cleared on START press). `mem[$0091]=$C0`
seeded in `initialize()` so the title is correct on frame 1.

---

## SESSION STATUS (2026-06-10) — launch cinematic through stars/planet

The full post-START **launch cinematic now runs on the Amiga**, continuing past the
Standby screen all the way to the planet zoom. Built incrementally, each beat a commit
on `main`; verified in FS-UAE. (Open item 5 "Descent / door-open sequence" above is
DONE; items 1–4 superseded by the 2026-06-09 sessions.)

Done & committed:
- **Doors + Tunnel** reveal — native `dl_doors_open_split_step` + ring palette cycle via the
  `$5367` dispatcher; tunnel rings drawn in code (`draw_frame_pattern_seq`).
- **Launch effects 1–4** — STAND BY + score, throttle-gauge fill (vobj→sprite), left
  indicator lights — faithful ports calling the linked transpile.
- **Stars/space + Planet** (`3c1e4ca`) — faithful tunnel→stars trigger
  (`draw_ring_frame_step $670D` + the corrected always-rotate `step_accum_add_75`);
  new **mode-D-from-`$1000` viewport** render path (DL `$3120`, VDSLST `$6CC2`, 43×48B
  wide, central 40, 2bpp, `$6D0E`/`$6D67` palette) decoded via precomputed byte→bitplane
  tables; stars setup + native scroll; planet zoom via `advance_object_positions`
  (`$6BA8`); per-byte incremental decodes for the `$1000`/`$2000` fields.

Open (tracked in memory `rof-stars-planet-phase`):
1. Tunnel-clear/palette **sync drift** near the very end of the tunnel exit.
2. **Star sprites** — the dots are event-queue/PMG-driven (`$5614`/`$548D`/`$5667`);
   that system isn't ported yet, so the stars beat is currently black space + planet.
3. **Flight hand-off** (`$6594` RTS → gameplay) — the flight terrain renderer is not
   yet on the Amiga, so the cinematic currently ends on the risen planet.
4. **Scene 3b Scoreboard** — still not analysed.
