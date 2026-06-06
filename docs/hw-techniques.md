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
| **Flight** | `$3000` (via `$3120`) | `display_setup $5F1D` + the `dl_lms_*` family | the dynamic terrain LMS ring — see §1.3 |

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

### 1.3 The flight display list — a dynamic LMS ring (the core trick)

The flight view at `$3000` is almost entirely an **LMS list: one ANTIC
instruction per terrain scanline**, whose 16-bit operand points into the terrain
pixel buffers. It is split into two strands that meet at the horizon:

- `$300A` — top strand, filled **downward**
- `$3089` — bottom strand, filled **upward** (mirrored)

Construction and per-frame maintenance:

| Function | Role |
|---|---|
| `init_terrain_dl $68AD` | seed every LMS operand to `$2F74` (a buffer pre-filled with `$88` = flat); blank `$2F75–$2FA3` |
| `game_setup_7460 $7460` | build the per-scanline base-address tables `$073D` (lo) / `$0793` (hi): 85 entries from `$C3:$C4` (the `$1000+` terrain buffer) stepping by stride `$C1` per row |
| `dl_lms_build/fill $69E5/$69F1` | copy those tables into the `$300A` strand (3-byte stride, ~86 rows) |
| `emit_dl_coord_pairs $68CF` | fill forward (`$300A`) and reverse (`$3089`) strands symmetrically |
| `scroll_terrain_dl $6953` | **every frame**: scroll the two LMS halves (`dl_lms_scroll_up/down`) and push a new ship-coordinate row at top/bottom (`dl_lms_push_top/bottom`) |

**The key idea:** the terrain pixels never move — only the **LMS pointers** in
the display list are scrolled each frame, so ANTIC pans the terrain vertically
"for free" as the ship's altitude changes. This is why `$3000`, `$3120`,
`$300A`, `$3089` are all **zero in the static image**: they exist only at
runtime.

### 1.4 No hardware fine scrolling

Notably, the game writes `HSCROL`/`VSCROL` (`$D404/5`) **never**. The smooth
terrain motion is entirely the LMS-pointer rewriting above — a software scroll,
not ANTIC's hardware fine-scroll. The port can move the viewport however it
likes as long as the per-frame terrain offset matches.

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
by `$C7`), so different screen bands (sky / terrain / cockpit) get different
colour-register and position changes — the classic Atari "more colours than the
register count allows" technique. `VDSLST` (`$0200/1`) is also swapped between
`$49EE` and `$6CC2` (`dli_handler_game2`) depending on phase.

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
   LMS-pointer changes every frame (the terrain scroll) — §1.3, §1.4.
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
(`a800dumps/flight_ram_0000_BFFF.bin`, `DLIST=$3000`) and the drawing functions.

Confidence is marked: **[C]** confirmed from dump + code, **[~]** inferred /
partially verified.

### 11.1 Vertical layout (the `$3000` display list, top → bottom)

| Scanlines | ANTIC mode | Screen data | What it is |
|---|---|---|---|
| 20 | blank | — | top centering |
| 8 | **mode 6** (20×8, 5-colour text) | `$32B5` | **status / message line** ("MANUAL", "TRANSMITTING", …) |
| 4 | blank | — | gap |
| 8 | **mode 4** (40×8, 4-colour text) | `$32C9` | top label strip (charset art) |
| 2 | mode D | — | horizon transition row |
| **86** | **mode F** (GR.8, 320×1 hi-res) | **`$1000+`** (LMS ring) | **the terrain / 3-D view** |
| 8 | **mode D** (GR.7, 160×2, 4-colour) | `$350D` | dashboard bitmap strip (scanner/horizon) |
| 80 | **mode 4** (40×8, 4-colour text) | `$332D` | **the cockpit dashboard panel** |
| — | JVB → `$3000` | — | wait for vblank, loop |

Total ≈ 216 scanlines. **[C]**

**Bitmap vs. character — the split:** ANTIC modes divide cleanly into two
families, and the screen mixes both:

- **Bitmap (map) modes — pixels straight from a screen buffer, no charset:**
  - **mode F** (GR.8, 1 bpp hi-res) — the **terrain / 3-D view** (`$1000+`).
  - **mode D** (GR.7, 2 bpp) — the horizon transition row and the **`$350D`
    dashboard strip**.
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

### 11.2 The terrain / 3-D view — mode F + DLI colour bands

- **Base layer [C]:** 86 rows of **ANTIC mode F** (GR.8, hi-res 320×1, 1 bit per
  pixel). Each row is an LMS instruction whose operand points into the terrain
  pixel buffer at `$1000+` (46-byte stride); the operands are scrolled every
  frame (§1.3) so the terrain pans vertically. `GPRIOR=$11` → it is *normal* GR.8,
  **not** a GTIA 9/10/11 mode, so within a scanline it is two colours
  (COLPF2 background hue + COLPF1 luminance for set pixels).
- **Colour [C]:** more than two colours come from **DLIs** flagged on terrain
  rows (`$306F`, `$30ED`, `$30F9`, `$3105`) driving the sequenced DLI dispatcher
  (§2.1) — sky band vs. mountain band vs. haze get different COLPF/COLBK. This is
  how the famous sky-gradient-over-mountains look is produced from a 1-bpp mode.
- **Player overlay [~]:** the four players also carry terrain data —
  `gen_terrain_column` / `fill_terrain_columns ($6AE5)` write per-column pixels
  into the player graphics buffers `$0C32/$0D32/$0E32/$0F32` (= P0/P1/P2/P3).
  During flight the players sit in two edge stacks (11.3), so this appears to add
  coloured terrain detail at the left/right margins the central hi-res field
  doesn't cover. (Exact visual contribution not 100 % pinned from one frame.)

> **Amiga:** a chunky/planar terrain bitmap with per-scanline colour changes via
> the Copper (the natural analogue of the DLI colour bands). The LMS-pointer
> vertical scroll maps to either a Copper-driven bitplane-pointer offset per line
> or a normal blit/scroll. Resolution target ~320×~86 in the view area.

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

- **Panel artwork [C]:** the bottom ~10 rows are **ANTIC mode 4** (40×8,
  4-colour text) reading screen data at `$332D` through a **custom character set
  at `$3800`** (`CHBASE=$04`). The glyphs are dashboard art, not text. The static
  panel is laid down once at init (`game_init_7588 $7588`, RLE-decompressed).
- **Analogue bar gauges [C]:** drawn as **columns of characters poked into the
  mode-4 dashboard cells** — `setup_dial_bar_draw ($444A)` / `draw_object_column
  ($43E8)` plot a vertical bar via a target-address table at `$4581`, whose
  entries point into `$33xx/$34xx` (i.e. *into the `$332D` mode-4 region* — **not**
  P/M). These are the fuel / altitude / scan-strength style bar indicators.
- **Numeric readouts [C]:** BCD digits (score, level, counts) rendered into the
  same mode-4 cells via `render_bcd_digits_supp_all ($49BA)` / `render_bcd_counter`
  / `bin_to_bcd`, with leading-zero suppression; dest pointer `$C5/$C6`.
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

### 11.7 The mode-D bitmap strip (`$350D`)

A short **ANTIC mode D** (GR.7, 160×2, 4-colour) band — 4 DL entries, 8
scanlines — sits between the terrain and the dashboard, screen data at `$350D`.
Its content is a small 4-colour bitmap; **[~]** its precise role (long-range
scanner display vs. the dashboard's top curved graphic vs. a horizon/aim strip)
is not yet pinned — it is the one region whose semantics need a follow-up. The
DLI `dli_handler_game2 ($6CC2)` fires around here.

> **Amiga:** small 4-colour bitmap region; decide once its role is confirmed.

### 11.8 Summary — recommended Amiga technique per element

| Element | Atari mechanism | Suggested Amiga approach |
|---|---|---|
| Terrain 3-D view | mode F hi-res + DLI colour bands + LMS scroll | terrain bitmap + Copper colour-per-line + line scroll |
| Sky/mountain colours | per-band DLIs | Copper list |
| Canopy side frames | fixed-position missiles (+edge players) | static hardware sprites (or bake into panel) |
| Enemy saucers / guns | Player 3 (moving) | moving hardware sprites |
| Falling object / shot | P1 strip `$0D98` | sprite or blit |
| Rescued pilot | `pilot_render` (player/playfield) | sprite/blit |
| Dashboard panel | mode-4 charset art `$332D`/`$3800` | static bitmap (blit once) |
| Bar gauges | char-cell columns (`draw_object_column`) | small blits into panel bitmap |
| Numeric readouts | BCD digits in mode-4 cells | digit blits |
| Status/message line | mode 6 text `$32B5` | text blit + flash |
| Mode-D strip `$350D` | GR.7 bitmap | 4-colour bitmap (role TBC) |
