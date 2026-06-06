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
