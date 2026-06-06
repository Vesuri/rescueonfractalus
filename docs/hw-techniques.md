# Atari hardware techniques the game uses

What *Rescue on Fractalus!* actually **does** with the Atari custom chips —
the interpreted, game-specific picture that sits between the two neighbouring
docs:

- `atari-hardware.md` — generic primer (what each chip *is*) + the SDL/Amiga
  abstraction boundary.
- `hw-access.md` — the raw, generated table of *every* register access site.
- **this doc** — the techniques: how those accesses combine into the rendering,
  input, sound and memory tricks the game relies on. Names/addresses match
  `disasm/symbols.csv`; this is what `platform.h` has to reproduce *behaviourally*.

> Cross-ref: `startup-flow.md` covers *when* these are set up (the INITAD chain,
> `game_entry`, the main loop). This doc covers *how* they work.
>
> **§11 is the element-by-element screen anatomy** (terrain / canopy / cockpit /
> gauges / enemies → which ANTIC mode + chip renders each, with an Amiga plan) —
> jump there if you're planning the Amiga renderer.

---

## 1. Display lists

There is no single display list. `DLISTL/H` (`$D402/3`) is rewritten in ~9
places and the active list changes with game phase. Almost all are **built at
runtime by functions** — only the loader screen is static bytes in the binary.

### 1.1 Inventory

| When | DL address | Built by | Contents |
|---|---|---|---|
| Loader (INITAD) | `$B832` | **static data** (`init_B800 $B800` just points to it) | Homesoft "LOADING" screen — see §1.2 |
| Attract | `$B800` | `display_list_build $1C40` | 122 LMS text rows from `$0600`; up to 30 sprites scattered by `RANDOM` |
| Intro / init | `$5A82` | `display_list_init $5D29` | early text/title list in the `$5xxx` data region |
| **Flight** | **`$3120`** | `display_setup $5F1D` (final `DLISTL` write at `$6543`) | mode-D (GR.7) 4-colour terrain + cockpit — see §1.3 |
| (parallel) | `$3000` | `display_setup` + the `dl_lms_*` family | a mode-F LMS ring — built but **not** the on-screen gameplay terrain; role TBC (§1.3) |

### 1.2 The loader screen is not original game content

The only display list baked into the binary lives at `$B832` (XEX segment 13).
Decoding its opcodes:

```
$B832: 70 ×12      12 × "8 blank scanlines"  → 96 blank lines (vertical centering)
$B83E: 42 48 B8    ANTIC mode 2 (40-col text) + LMS → screen RAM @ $B848
$B841: 10          2 blank lines
$B842: 06          mode 6 (20-col double-width text)
$B843: 70          8 blank lines
$B844: 02          mode 2 (40-col text)
$B845: 41 32 B8    JVB — jump & wait VBLANK → $B832 (loop)
```

The screen RAM at `$B848` decodes (Atari internal codes → ASCII) to:

> `FILEVERSION BY … HOMESOFT … LOADING "RESCUE ON FRACTALUS !"`

So `rof.xex` is a **Homesoft repacker release**; this is their loader cracktro,
shown via INITAD while the 31.5 KB main block streams in. It is not Lucasfilm
code and does not need faithful reproduction — the SDL port skips it entirely.

### 1.3 The flight display list — mode D (GR.7) 4-colour

The on-screen flight terrain uses the **`$3120`** display list (this is the
*final* `DLISTL/H` write in `display_setup`, at `$6543`; an earlier write to
`$3000` is superseded). Verified against the ground-truth flight RAM dump and
the rendered frame (`atari002.png`):

- Terrain rows are **ANTIC mode D** (GR.7): **4 colours**, ~160 px wide
  (**2 colour-clocks per pixel** — *not* 320 hi-res), **2 scanlines per pixel
  row**. Roughly 47 mode-D rows ≈ 94 scanlines, starting at scanline ~42.
- Wide playfield (`SDMCTL=$3F`) → **48 bytes per row**; the terrain bitmap lives
  at **`$1000`** and the dump's row data is laid out at exactly 48-byte stride,
  matching mode-D-wide (not the mode-F ring's 46-byte stride).
- The terrain colours are **one constant mode-D palette** set by a single DLI at
  the top of the terrain (slot 1, `$4A1F`): COLBK=`$14` (brown ground),
  COLPF2=`$B8` (salmon sky), COLPF1=`$10` (dark mountain edge), COLPF0=`$2A`.
  Sky / ground / mountain are **different 2-bit pixel values in that one
  palette** (encoded in the bitmap) — there is **no DLI sky/ground colour split**
  inside the terrain. (The salmon→brown boundary follows the irregular mountain
  silhouette, not a straight raster line.) Later DLIs recolour for the cockpit,
  not the terrain — §2.1.

**Vertical motion:** the fractal terrain is re-projected/redrawn into the
`$1000` bitmap each frame (`terrain_gen_1/2/3`, run twice per frame in the inner
loop); the single LMS operand at `$3129` is also rewritten per frame (`$770B`)
for coarse vertical positioning. It is **not** a per-line-LMS scroll.

> **A correction / caveat.** A *separate* display list at `$3000` is also built
> at runtime — a genuine **per-scanline LMS "ring"** (two strands `$300A`
> downward / `$3089` upward, maintained by `init_terrain_dl $68AD`,
> `dl_lms_build/fill $69E5/$69F1`, `emit_dl_coord_pairs $68CF`,
> `scroll_terrain_dl $6953`, using the per-scanline base tables `$073D/$0793`
> from `game_setup_7460`). But it is **mode F (46-byte stride)** and does **not**
> match the 48-byte mode-D buffer that is actually on screen, so it is *not* the
> visible gameplay terrain. Its role is unresolved — possibly a different
> view/sub-mode (descent/landing?) or vestigial. (An earlier version of this doc
> wrongly presented this ring as *the* terrain renderer; it isn't.)

### 1.4 No hardware fine scrolling

Notably, the game writes `HSCROL`/`VSCROL` (`$D404/5`) **never**. Vertical
terrain motion comes from re-drawing the bitmap + the coarse `$3129` LMS update
(§1.3), not ANTIC's hardware fine-scroll. The port can move the viewport however
it likes as long as the per-frame terrain projection matches.

---

## 2. Display-list interrupts (DLI) and VBIs

### 2.1 The DLI is a sequenced dispatcher (`dli_handler_game $49EE`)

Rather than one fixed mid-screen handler, the game runs a **multiplexed DLI**:

```
49ee: save A→$C8, Y→$CA
      Y = $C7              ; per-interrupt sequence index
      if Y >= 5: restore, RTI
      indirect JMP via table $4AD9(lo)/$4ADE(hi)  → one of up to 5 sub-handlers
```

Each successive DLI down the frame runs the *next* routine in the list (indexed
by `$C7`, reset to 0 each VBI). The five slots are: slot 0 `$4A11` (COLPF0);
slot 1 `$4A1F` (sets the full **terrain** palette just above the terrain);
slot 2 `$4A40` (begins the **cockpit** transition — COLPF2→gray, players,
PRIOR); slot 3 `$4A78` (cockpit-panel colours, PRIOR=`$04`); slot 4 `$4ACD`
(COLBK, wraps `$C7`).

So the DLIs mostly establish the terrain palette **once** at the top and then
recolour for the cockpit lower down — they do **not** create a sky/ground colour
split within the terrain (that split is bitmap-encoded; §11.2). They are still
the classic "more colours than the register count allows" technique, just across
the terrain→cockpit boundary rather than within the terrain. `VDSLST` (`$0200/1`)
is swapped between `$49EE` and the cockpit handlers `$6CC2`/`$6CAD` depending on
phase.

### 2.2 VBI handlers swap by phase

`VVBLKI` (`$0222/3`) is repointed as the game changes state (see
`startup-flow.md` §6 for the timeline):

| Phase | VBI |
|---|---|
| Attract | `vbi_handler_attract $1B30` — rewrites DLIST + COLBK, ticks RTCLOK |
| `game_entry` init | `vbi_handler_1 $53CC` |
| Gameplay | `vbi_handler_game $52D7` → tail `vbi_deferred_dispatch $534D` |
| Outer reset hand-off | `vbi_handler_2 $4FF5` |

`vbi_deferred_dispatch` is where per-frame housekeeping lives: SFX/music ticks
and gauge-digit updates, exiting via `os_xitvbv` (`$E462`). `NMIEN` (`$D40E`) is
set to `$C0` to enable both VBI and DLI NMIs.

---

## 3. Player/Missile graphics (GTIA)

- `PMBASE` (`$D407`) is set so the P/M data lives in a fixed page; `GRACTL`
  (`$D01D`) and `DMACTL/SDMCTL` (`$D400`/`$022F`=`$22`) enable P/M DMA.
- The four players + missiles are used for moving sprites over the terrain
  playfield: the ship's gunsight/reticle, enemy saucers/gun emplacements, and
  the rescued pilot. `pmg_missile_init $1910` sets missile X positions;
  `pmg_colors_attract $1F0B` and the `display_setup` block set `HPOSPx`,
  `SIZEPx`, `COLPMx`/`PCOLRx`.
- Horizontal position is set directly via `HPOSP0-3` / `HPOSM0-3`
  (`$D000–$D007`); vertical position is by where the shape bytes sit in the P/M
  buffer (there is no vertical position register on Atari).
- `update_p3_indicator_stripe $4467` reuses player 3 (`SIZEP3 $D00B`) as a
  cockpit indicator stripe — a P/M channel doing HUD duty, not a sprite.

---

## 4. Collision is done in software, not hardware

A notable divergence from typical Atari games: **the GTIA hardware collision
registers (`$D000–$D00F` on read) are never read** (0 sites). Terrain collision
is computed in software by `terrain_collision $AE53`, which compares the ship's
position against 8 terrain rows and dispatches to the landing/crash handler.
The port therefore does **not** need to emulate GTIA collision latches for the
core game — it must reproduce the `terrain_collision` arithmetic exactly.

---

## 5. Input

| Source | Register | Use |
|---|---|---|
| Joystick | `PORTA` (`$D300`, PIA) — read 7× | pitch/roll/throttle in `flight_control_integrate $8E5B` |
| Fire button | `TRIG0` (`$D010`, GTIA read) — read 3× | laser trigger (`read_console_trig_delta $5A78`, engine/SFX gating) |
| Console keys | `CONSOL` (`$D01F`, GTIA) | START dismisses attract (`== $06`); SELECT/OPTION |
| Keyboard | `CH` (`$02FC` shadow) | any-key exit from attract; initials entry (`name_entry_loop $5B6C`) |

`check_collision_sync $5398` reads `CONSOL` + `TRIG0` + `PORTA` together as the
combined input poll. (`$D010` is GTIA's TRIG0 on *read*; it is `GRAFP3` only on
*write* — the symbol map names the write side.)

---

## 6. Sound (POKEY)

- Four channels: `AUDF1-4`/`AUDC1-4` (`$D200–$D207`), global `AUDCTL` (`$D208`),
  `SKCTL` (`$D20F`).
- **Two sound paths:**
  1. *Deferred, VBI-driven* — `vbi_deferred_dispatch $534D` calls the SFX voice
     tick and `music_player_tick` each frame (engine drone, effects, melody).
  2. *Timer-IRQ-driven* — `audio_timer_setup $712D` programs POKEY timers and
     `IRQEN=$C0` (timer 1+2 IRQs); `audio_irq_handler $8237` rewrites `AUDF1-4`
     + `AUDCTL` from the interrupt for tighter-timed audio.
- `music_init_state $7238` loads a 6-byte song header and sets `AUDCTL=1`
  (clock config). The attract melody is `audio_attract $1B5B`.
- Engine pitch tracks gameplay: `engine_sound_update $5978` ramps a pitch value
  toward `level_progress $37EE` based on throttle/trigger bits.

The port routes all of this through the platform audio callback; the OS VBI
vectors (`$E45C/5F/62`) are no-ops because the platform layer owns VBI dispatch.

---

## 7. RANDOM as the fractal seed

POKEY's `RANDOM` register (`$D20A`) is read in **59 places** — far more than a
typical game. It is the entropy source for the procedural content:

- the fractal terrain generation (`terrain_gen_*`, the `$A0xx` transform code),
- attract-screen sprite placement (`display_list_build`),
- enemy/saucer behaviour (`saucer_anim_tick $4229` toggles bits via `RANDOM&7`),
- intro effects (`intro_random_setup $6FBF`, `intro_sound_and_tick $4F9F`).

**Parity implication:** faithful terrain requires a bit-exact POKEY `RANDOM`
(17-bit LFSR) in the platform layer, not a host PRNG — the mountains *are* the
RNG sequence.

---

## 8. Page-flip double buffering (`screen_page_swap $1A62`)

A **self-modifying** routine that swaps 5 × 256-byte pages between `$40xx` and
`$06xx`, rotating double-buffer pages. Used at level/screen transitions (called
from `initad_1A97` and `attract_exit`) so a freshly built screen can be swapped
in without tearing. Self-modification means the port can't just transliterate it
naively — it's a candidate for the native-reimplementation seam (see
`native-reimpl-seam` / `rof_native.c`).

---

## 9. Memory banking (PIA PORTB)

The game is a 64K XL/XE program. `PORTB` (`$D301`) is poked **once, by the XEX
loader** (segment 1, value `$FF`) to configure RAM under the OS ROM; the game
code itself never touches `$D301` afterwards. In the flat 64K image used by the
port there is no OS ROM to bank, so this is a no-op — but it explains why the
binary assumes RAM is visible across the whole `$0000–$FFFF` space (minus the
`$D000–$D7FF` hardware window).

---

## 10. Summary — what the platform layer must reproduce behaviourally

1. **ANTIC display-list execution** with per-line LMS, and the ability to follow
   the mode-D (GR.7) 4-colour terrain re-projected each frame — §1.3, §1.4.
2. **Sequenced DLIs** + phase-swapped VBIs firing at the right scanlines — §2.
3. **P/M graphics** positioned by `HPOSx` + buffer offset, with sizes/colours — §3.
4. **POKEY** 4-channel audio via *both* the VBI and timer-IRQ paths — §6.
5. A **bit-exact `RANDOM` LFSR** — §7 (non-negotiable for terrain parity).
6. Software collision (`terrain_collision`) needs no hardware support — §4.
7. Input mapping: PORTA joystick, TRIG0 fire, CONSOL/CH — §5.

---

## 11. Screen anatomy — what renders each region (flight screen)

This maps every visible part of the gameplay screen to the exact hardware
feature that draws it, so the Amiga port can pick the right technique per
element. Decoded from the ground-truth flight RAM dump
(`a800dumps/flight_ram_0000_BFFF.bin`), the rendered frame (`atari002.png`), and
the drawing functions. The on-screen flight DL is **`$3120`** (§1.3).

Confidence is marked: **[C]** confirmed from dump + code, **[~]** inferred /
partially verified. Instrument names are from the game manual's *Instrument
Detail* (p. 6).

### 11.1 Vertical layout (the `$3120` display list, top → bottom)

| Scanlines | ANTIC mode | Screen data | What it is |
|---|---|---|---|
| 20 | blank | — | top centering |
| 8 | **mode 6** (20×8, 5-colour text) | `$32B5` | **status / message line** ("MANUAL", "TRANSMITTING", …) |
| 4 | blank | — | gap |
| 8 | **mode 4** (40×8, 4-colour text) | `$32C9` | top label strip (charset art) |
| 2 | mode D | — | horizon transition row |
| ~94 (≈47 rows × 2) | **mode D** (GR.7, ~160×2, **4-colour**) | **`$1000`** (48-byte rows) | **the terrain / 3-D view** |
| 8 | **mode D** (GR.7, 160×2, 4-colour) | `$350D` | **Artificial Horizon** (bank + climb) — see 11.7 |
| 80 | **mode 4** (40×8, 4-colour text) | `$332D` | **the cockpit dashboard panel** |
| — | JVB → `$3120` | — | wait for vblank, loop |

Total ≈ 216 scanlines. **[C]**

**Bitmap vs. character — the split:** ANTIC modes divide cleanly into two
families, and the screen mixes both:

- **Bitmap (map) modes — pixels straight from a screen buffer, no charset:**
  - **mode D** (GR.7, 2 bpp, 4-colour) — the **terrain / 3-D view** (`$1000`),
    the horizon transition row, and the **`$350D`** Artificial Horizon strip.
- **Character (text) modes — screen buffer holds glyph indices into a charset:**
  - **mode 6** (5-colour) — the **status/message line** (`$32B5`).
  - **mode 4** (4-colour) — the **top label strip** and the **whole cockpit
    dashboard** (`$332D`), via the custom charset at `$3800`. The dashboard art,
    bar gauges and numeric readouts are all *characters*, not pixels — the game
    redraws gauges/digits by poking glyph codes into char cells, never by
    plotting pixels there.

So: **terrain and the `$350D` strip are pure bitmap; everything in the cockpit
furniture (status line + entire dashboard, gauges, digits) is character-based.**
The Player/Missile overlays (11.3) are a third, separate layer on top of both.

Overlaid on all of the above (full frame) are the GTIA Player/Missile channels —
see 11.3.

### 11.2 The terrain / 3-D view — mode D (GR.7), 4-colour

- **Base layer [C]:** ~47 rows of **ANTIC mode D** (GR.7): 2 bits per pixel →
  **4 colours per scanline**, ~160 px wide (**2 colour-clocks per pixel**, *not*
  320 hi-res), **2 scanlines per pixel row** (the visible "2×2 pixels"). Wide
  playfield (`SDMCTL=$3F`) = **48 bytes per row**; the bitmap is at **`$1000`**
  (dump rows align to 48-byte stride). `GPRIOR=$11` (no GTIA 9/10/11 mode).
- **Colour [C]:** the terrain is **one constant 4-colour palette**, set by a
  single DLI at the top of the terrain (slot 1, `$4A1F`): COLBK=`$14` (brown
  ground), COLPF2=`$B8` (salmon sky), COLPF1=`$10` (dark mountain edge),
  COLPF0=`$2A`. **Sky, ground and mountain are different 2-bit pixel values in
  that one palette** (drawn into the bitmap) — *not* a DLI colour split. The
  salmon→brown horizon follows the mountain silhouette (irregular), confirming
  it is bitmap-encoded, not a raster line. Later DLIs (slot 2 `$4A40` sets
  COLPF2=`$06` = the gray frame, slots 3–4) recolour for the **cockpit**
  transition, not the terrain.
- **Per-frame redraw [C]:** the fractal terrain is re-projected into the `$1000`
  bitmap each frame by `terrain_gen_1/2/3` (run twice per frame in the inner
  loop), with the single LMS operand at `$3129` rewritten (`$770B`) for coarse
  vertical positioning. There is a separate terrain heightmap at `$1010+`
  (96-byte rows, read by `terrain_collision $AE53`).
- **Player overlay [~]:** the four players also carry terrain data —
  `gen_terrain_column` / `fill_terrain_columns ($6AE5)` write per-column pixels
  into the player graphics buffers `$0C32/$0D32/$0E32/$0F32` (= P0/P1/P2/P3).
  During flight the players sit in two edge stacks (11.3), so this appears to add
  coloured terrain detail at the left/right margins. (Exact visual contribution
  not 100 % pinned from one frame.)

> **Amiga:** a 2-bitplane (4-colour) terrain bitmap, ~160×94 in the view area,
> using **one fixed palette** (sky/ground/mountain are just pixel values — no
> per-band colour change needed). The terrain is re-projected per frame, so a
> normal blit/redraw fits — no hardware scroll needed. A Copper colour change is
> only needed at the **cockpit** boundary below the terrain.

### 11.3 The canopy frame & sprites — Player/Missile graphics

PMBASE=`$08`, single-line resolution → Missiles `$0B00`, P0 `$0C00`, P1 `$0D00`,
P2 `$0E00`, P3 `$0F00`. Positions/sizes are set in `display_setup` and re-stamped
every frame by `vbi_handler_game ($52D7)`:

| Channel | HPOS | Size | Role | Conf |
|---|---|---|---|---|
| **Missiles M2,M3** | `$3B`,`$39` (left edge) | — | **canopy left frame post** — fixed full-height vertical bar | [C] pos, [~] role |
| **Missiles M0,M1** | `$C5`,`$C3` (right edge) | — | **canopy right frame post** | [C] pos, [~] role |
| **P0** | `$2D` (left) | normal | left-edge terrain/frame column (buffer `$0C32`) | [~] |
| **P2** | `$2D` (left) | normal | stacked with P0 at left edge | [~] |
| **P1** | `$BE` (right) | double | right-edge terrain/frame column (buffer `$0D32`); also a **vertical falling object** drawn at `$0D98` (e.g. dropped object / shot) | [~] |
| **P3** | `$BE` (right) / dynamic | quad | right-edge column; **also the enemy saucer/gun sprite** (`draw_player3_object $42A7`, HPOSP3 moved dynamically at `$43B2`) and the **P3 cockpit indicator stripe** (`update_p3_indicator_stripe $4467`, SIZEP3) | [C] multi-use |

Key facts: the missiles are **pinned at the four screen-edge X positions every
frame** (left ≈`$39–3B`, right ≈`$C3–C5`) — i.e. the cockpit window's vertical
frame, not projectiles. The players are stacked at the same two edges. P3 is the
most multiplexed channel (saucer **and** HUD duty in the same buffer at different
vertical offsets). All P/M colours come from the `PCOLR0-3` shadows (`$02C0-3`,
all `$2A` green in this frame) and can be re-tinted by DLI.

> **Amiga:** these are all narrow, mostly-fixed vertical elements — cheapest as
> **hardware sprites** (canopy posts = static sprites; enemy saucer = a moving
> sprite). The "player carries terrain detail" trick is Atari-specific and need
> not be reproduced literally — fold that detail into the terrain bitmap instead.

### 11.4 Enemies & rescuable objects

- **Enemy saucers / gun emplacements [C]:** rendered through **Player 3**
  (`saucer_anim_tick $4229` animates the shape, `draw_player3_object $42A7`
  writes the P3 buffer, HPOSP3 tracks the enemy X). Behaviour driven by
  `enemy_check` and `game_state_update ($A99C)`.
- **Vertical falling object [~]:** a shape strip at `$0D98` in the **P1** buffer
  (`vobj_draw_dispatch $41E4` / `vobj_erase_row $4207`), position from `$062F`.
- **Rescued pilot [~]:** `pilot_render` toggled by `$288D/$288E`; appears in the
  terrain/door area — channel not definitively isolated (likely reuses a player
  or playfield cells when the pilot is at the airlock).

> **Amiga:** moving hardware sprites for saucers/shots; the pilot can be a sprite
> or a blitter object over the terrain.

### 11.5 The cockpit dashboard — mode-4 charset artwork + poked cells

Instrument names below are from the game manual's *Instrument Detail* (p. 6, the
"Valkyrie Fighter Control Panel"): **1 Score, 2 Compass, 3 Wing Clearance Bars,
4 Thrust Level, 5 Dangerous Altitude, 6 Artificial Horizon, 7 Altimeter**.
Items 1–5, 7 live in the mode-4 char panel here; item 6 is the mode-D strip
(11.7).

- **Panel artwork [C]:** the bottom ~10 rows are **ANTIC mode 4** (40×8,
  4-colour text) reading screen data at `$332D` through a **custom character set
  at `$3800`** (`CHBASE=$04`). The glyphs are dashboard art, not text. The static
  panel is laid down once at init (`game_init_7588 $7588`, RLE-decompressed).
- **Analogue bar gauges [C]:** drawn as **columns of characters poked into the
  mode-4 dashboard cells** — `setup_dial_bar_draw ($444A)` / `draw_object_column
  ($43E8)` plot a vertical bar via a target-address table at `$4581`, whose
  entries point into `$33xx/$34xx` (i.e. *into the `$332D` mode-4 region* — **not**
  P/M). These are the manual's bar-style indicators: **Thrust Level (4)**,
  **Dangerous Altitude (5)**, **Wing Clearance Bars (3)**, and the **Altimeter
  (7)** (a dual bar — red = terrain height, light-blue = ship height; "altitude
  above ground" = how much light-blue shows). **[~]** on which `$4581` slot is
  which gauge.
- **Numeric readouts [C]:** BCD digits — the **Score (1)** and level/counts —
  rendered into the same mode-4 cells via `render_bcd_digits_supp_all ($49BA)` /
  `render_bcd_counter` / `bin_to_bcd`, with leading-zero suppression; dest
  pointer `$C5/$C6`.
- **Compass (2) [~]:** a heading/direction indicator; small char-cell element in
  the panel (exact cells not isolated).
- **Per-frame refresh [C]:** `update_gauge_digits ($548D)` runs each VBI (from
  `vbi_deferred_dispatch $534D`), BCD-stepping the gauge digit arrays
  (`$0679/$06A3/$06B1/$06BF/$06CD/$06DB/$06E9/$066B`) toward their targets.
- **Gauge geometry [~]:** `compute_gauge_geometry_from_006D` derives bar heights
  from the level/stage (`$006D`) — gauges grow with level.

> **Amiga:** the dashboard is a static image (one blit at level start) plus a
> handful of dynamic regions. Draw bar gauges and digits as small blits into the
> dashboard bitmap; no need to emulate the charset indirection — bake the glyph
> art into a tile sheet or just blit rectangles.

### 11.6 Status / message line — mode 6

The single **mode-6** row (`$32B5`, 5-colour 20-column text) is the cockpit
message line. `show_cockpit_message ($47B8)` decodes a message ID into the
14-byte buffer at `$32B7` (ATASCII+`$40`, hi-bit-terminated) and sets a flash
timer (`$063E`); `clear_message_buffer ($480B)` blanks it. Shows mode strings
("MANUAL"), events ("TRANSMITTING", pilot pickups), etc. **[C]**

> **Amiga:** ordinary text blit with a flashing colour; trivial.

### 11.7 The Artificial Horizon — mode-D bitmap strip (`$350D`)

A short **ANTIC mode D** (GR.7, 160×2, 4-colour) band — 4 DL entries, 8
scanlines — sits between the terrain and the dashboard, screen data at `$350D`.
This is the manual's **Artificial Horizon (6)**: "indicates your ship's current
bank (left/right) and climb (up/down)." That is exactly why this one instrument
is a **bitmap** while the rest of the dashboard is character cells — it needs a
freely tiltable/sliding horizon line that char cells can't express. The DLI
`dli_handler_game2 ($6CC2)` fires around here. **[~]** on the exact draw routine
(identity is manual-confirmed + matches the bitmap mode and position).

> **Amiga:** small 4-colour bitmap region; draw a rotated/offset horizon line per
> frame from the ship's bank/pitch (`$0025/26` pitch, `$0028/29` roll).

### 11.8 Summary — recommended Amiga technique per element

| Element | Atari mechanism | Suggested Amiga approach |
|---|---|---|
| Terrain 3-D view | mode D (GR.7) 4-colour, re-projected per frame | 2-bitplane (4-colour) bitmap, redrawn per frame |
| Sky/mountain colours | one constant mode-D palette (bitmap pixel values) | single fixed palette |
| Canopy side frames | fixed-position missiles (+edge players) | static hardware sprites (or bake into panel) |
| Enemy saucers / guns | Player 3 (moving) | moving hardware sprites |
| Falling object / shot | P1 strip `$0D98` | sprite or blit |
| Rescued pilot | `pilot_render` (player/playfield) | sprite/blit |
| Dashboard panel | mode-4 charset art `$332D`/`$3800` | static bitmap (blit once) |
| Bar gauges (Thrust, Dangerous Alt, Wing Clearance, Altimeter) | char-cell columns (`draw_object_column`) | small blits into panel bitmap |
| Score + numeric readouts | BCD digits in mode-4 cells | digit blits |
| Compass | char-cell element | small blit |
| Artificial Horizon | GR.7 bitmap strip `$350D` | tiltable horizon line, drawn per frame |
| Status/message line | mode 6 text `$32B5` | text blit + flash |
