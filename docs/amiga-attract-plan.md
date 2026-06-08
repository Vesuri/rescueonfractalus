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
>   - **Amiga VBI interrupt** does only what `vbi_handler_attract $1B30` does:
>     increment timers (`$0080`, `$0014`/`$0013`). Lightweight, no rendering.
>   - **Amiga main loop** calls every attract animation function in the same order
>     as the Atari attract loop: `pmg_update_attract`, `audio_attract`,
>     `attract_anim_frame`, `attract_sub_1EB4`, `pmg_colors_attract`,
>     `attract_sub_1F48`. These update `mem[]`.
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

---

## Source-material roles (what we take from where)

| Source | What we reuse | What we DON'T take |
|---|---|---|
| `dA JoRMaS/Template/C++` | the hardware classes `AmigaHardware`/`Bitmap`/`CopperList`/`Sprite`/`Palette`; the proven **GCC build** (`Makefile`, `GCCRuntime.cpp` runtime + L3 int trampoline, `SASCCompat.h`, `incbin.s`, `compat-include/`, `run.sh`/`debug.sh`) | `ModulePlayer`/`TrackerPackerReplayV3.1.s` (we don't do tracker audio), `Production`/`Part`/`Script`/`ProductionRunner` (demo timeline), `ExampleProduction`/`ExamplePart`, `main.cpp` |
| `dA JoRMaS/Productions/JRm-bS75/Source` (`GameCopperList`, `GamePart` sprite code) | the **serious copper + sprite pattern**: subclass `CopperList` → `GameCopperList` with a `writeCopperlist(...)` that **rebuilds the per-scanline copper every frame** (`showScroller`/`showHorizonBack`/`showObjects`/`showHorizonParallax`/`showRoad`/`showSprites`), per-scanline `WAIT` colour/mode changes, **all 8 hardware sprites** driven together (`showSprites(s0..s7)`, grouped as `BikeSprite`; `setBikeSpritePosition`/`showCurrentBikeSprite`), double-buffered copperlists | the racing-game logic (road geometry, bike physics) |
| `tmp/attackofthepetsciirobots` (`Platform.h`, `PlatformAmiga.cpp`, `petrobots.cpp` main loop) | the **app skeleton**: `main()` → system setup → install VBI int server (`AddIntServer(INTB_VERTB,…)`) → game-style state-machine loop; non-blocking `readKeyboard`/`readJoystick`; `renderFrame(waitForNextFrame)` = `WaitTOF`; chip-RAM alloc + interleaved bitplane bitmap; blitter tile/rect ops | its tile/map engine, gzip/Bin2Hunk specifics (we use `incbin`) |
| `dA JoRMaS/Utilities/WHDLoadMenu` (`WHDLoadMenuAnimated.cpp`, `AmigaView`, `AmigaCopperList`, `MenuView` loop, `Joystick`/`Keyboard`) | the **animated-copperlist idioms**: double-buffered copper lists swapped per frame (`LOFlist = copperList->data()`), per-frame copper edits (`setFade`/`setColor`/sprite-pointer swap/`setXOffset`), palette-fade infra, `WaitTOF`+poll loop, sprite-frame animation (the bouncing-ball pattern) | the WHDLoad menu logic itself |
| RoF repo: `src/gen/rof_gen.c` attract + audio routines + `cpu`/`bus`/`mem[]`, `PlatformSDL.cpp` POKEY synth | **attract state machine**: `vbi_handler_attract`, `attract_anim_frame`, `attract_sub_1EB4`, `pmg_update_attract`, `pmg_colors_attract`, `attract_sub_1F48` compiled for m68k — these update `mem[]` each frame; **audio**: `audio_attract` + song/SFX data → Paula via POKEY→Paula backend; `PlatformSDL`'s `audioCallback` algorithm as the spec | the rendering/terrain core (native instead); the full game loop (game routines only added when needed) |
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
      `AttractScene` object owns the per-frame `update()`/`render()`. Exit on left
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
- [ ] In `AttractScene::initialize()`: a `Bitmap` over the incbin'd data, a
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
      Amiga build. For the attract that's `audio_attract $1B5B` (+ whatever it
      calls) and its data tables; the game path later adds `music_player_tick`,
      `audio_timer_setup`/`audio_irq_handler`, `engine_sound_update`. Keep this a
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
- [ ] **Port the POKEY RANDOM LFSR.** `audio_attract` reads `$D20A` (17-bit LFSR)
      for volumes — port `Platform::pokeyRandomStep` so `bus_read($D20A)` is
      bit-exact (small; also future-proofs terrain parity).
- [ ] **Drive it per frame from the VBI server.** The attract loop calls
      `audio_attract` each frame — call the converted function from the VBI
      handler (or main loop), then let the Paula backend hold the tones until the
      next update.
- [ ] Build, run (listen vs the SDL build). **Commit:**
      `feat(amiga): 6502-converted audio player → Paula backend`.

**Gotchas:** `volatile` on the frame-ready flag (the `-O2` infinite-spin bug from
the spike); Paula needs the waveform in **chip RAM**, length/period set before
the channel DMA is enabled; watch the AUDF→period scaling for the PAL clock
(3546895 Hz) vs NTSC. The audio-core slice must not drag in rendering code —
stub any `bus_write` to display registers.

### M6a — ANTIC display list audit
**Goal:** establish the exact rendering model by reading the Atari attract display
list from the memory dumps. Every subsequent milestone depends on this.

- [ ] Parse `a800dumps/attract.a8s` (binary Atari state snapshot): locate the
      display list (DLISTL=$D402, DLISTH=$D403 registers give the address; VBI
      writes $1C35 = $1C×256+$35, so DL is at $1C35). Walk the DL byte-by-byte:
      decode each entry (mode bits 3-0, flags bits 7-4, optional LMS address).
      Record: ANTIC mode, scanline range, screen RAM address, for every section.
- [ ] **Confirmed by code analysis (do not re-derive):**
      The title/terrain section is **ANTIC mode F** (1bpp bitmap, 320 px, 1
      scanline/entry, 40 bytes/row), 122 entries pointing into `$0600`. Title text
      and terrain texture are **pre-rendered pixels** stored in ROM source buffers;
      `attract_sub_1EB4` copies them into `$0600`. No font needed for this section.
- [ ] Identify the cockpit section: expected to be ANTIC mode 2 (40-column
      character mode, 8×8 glyphs, 8 scanlines/row) or similar. Record the CHBASE
      register value (screen RAM for charset pointer is at `$D409`; check the
      attract state dump for its value).
- [ ] Map each ANTIC section to a Copper action: bitmap region → blit from
      `mem[]`; character region → character render; DLI colour changes → Copper
      WAIT+MOVE.
- [ ] Document the full mapping in `amiga/ARCH.md` (update §Attract).
- [ ] **Commit:** `docs(amiga): ANTIC display list audit — confirmed mode map`.

### M6b — Full 6502 attract loop on m68k
**Goal:** the m68k runs every attract-state-machine function the Atari runs each
frame. After this milestone `mem[]` is updated correctly every frame, making all
subsequent rendering accurate.

- [ ] **VBI interrupt**: add to the Amiga VBI handler (in `main.cpp`):
      `INC_M(0x0080)`, `INC_M(0x0014)` + carry into `$0013`. This is all
      `vbi_handler_attract` does beyond DLIST/COLBK writes (which the Copper
      handles). Do NOT call DLIST/COLBK writes — the Copper owns those.
- [ ] **Main loop (expand `AttractScene::update`)**: call, in order:
      `pmg_update_attract()`, `audio_attract()`, `attract_anim_frame()`,
      `attract_sub_1EB4()`, `pmg_colors_attract()`, `attract_sub_1F48()`. These
      are already compiled for m68k. Verify they don't drag in rendering code
      (stub any remaining display-register `bus_write` calls not already stubbed).
- [ ] Remove the current hand-rolled timer increment from `update()` (it's now
      done by the VBI) and the manual call to `audio_attract()` (now in the
      sequence above).
- [ ] Verify `mem[$0600..$06FF+]` changes each frame as the attract animation
      cycles (log a few bytes; compare against the SDL build's `atari800` debugger
      state at the same frame).
- [ ] **Commit:** `feat(amiga): full attract state machine running on m68k`.

**Gotchas:** `attract_sub_1EB4` accesses tables at `$2313`/`$231B` — these must
be loaded into `mem[]` from the XEX. Verify the XEX loader populates them.
`pmg_update_attract` / `pmg_colors_attract` write PMG RAM at `$3400–$3500` and
POKEY/GTIA registers — stub those bus writes; we drive sprite positions separately.

### M6c — Bitmap region: blit `mem[$0600]` to title+terrain bitplane
**Goal:** the Amiga title/terrain region shows exactly what the Atari's mode-F
bitmap shows: the pre-rendered title text and terrain texture driven by the
running 6502 attract routines.

- [ ] The title+terrain area is 122 scanlines of 40-byte (320px, 1bpp) bitmap
      data in `mem[$0600..$1BFF]` (122 × 40 = 4880 bytes). Every frame, after
      calling the attract functions, blitter-copy that range from fast RAM (where
      `mem[]` lives) into the title+terrain region of the Amiga bitplane (chip RAM).
      Blitter: source = `&mem[0x0600]`, dest = title plane ptr, width = 40 bytes,
      height = 122 rows, modulos = 0 (contiguous).
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
- Input/gameplay, collision (`terrain_collision`), enemy/saucer logic, the full
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
