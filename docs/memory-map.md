# Memory map — Rescue on Fractalus! Atari 8-bit

Generated from Phase 1 analysis (2026-05-29). All addresses are post-load
(after all XEX segments applied). See `disasm/symbols.csv` for the machine-
readable symbol table; this document adds context and structure.

## Address space overview

```
$0000–$00FF   Zero page       — registers & heavily-used game variables
$0100–$01FF   Stack           — 6502 stack (not explicitly used as data)
$0200–$02FF   OS page 2       — OS vectors + hardware shadow registers
$0300–$03FF   OS page 3       — OS data (loader area at $03F8)
$0400–$05FF   BSS area        — zeroed at game load (512 bytes)
$0600–$13FF   Screen data     — 122 rows × 40 bytes = $0600–$1309; also game vars $0600–$062B+
$1010–$12B0   Terrain tables  — 8 rows × $60 bytes pre-loaded heightmap (inside $0B00–$1AA6 segment)
$1400–$1AA6   Code seg 1 tail — attract mode / audio / display init code
$1B30–$283D   Code seg 2      — more attract + game init code
$2840–$37FF   BSS / game RAM  — uninitialized game state, display list RAM, level data
$3120         display_list_game     (built at runtime)
$316B         display_list_alt      (built at runtime)
$3800–$3BFF   BSS             — 1024 bytes zeroed (loaded as all-zeros in XEX segment 15)
$3C00–$3CDD   Loader stubs    — 9 tiny loader-stage helpers
$3CDE–$B7FF   Main game code  — ~31.5 KB; the bulk of the game
$B800–$B8AB   init_B800 stub  — INITAD segment; runs once; display list at $B832 after code
$B832         display_list_init     (initial; embedded in init_B800)
$C000–$FFFF   OS ROM area     — OS ROM + hardware registers ($D000–$D7FF)
```

## Zero page ($0000–$00FF)

| Addr | Name | Description |
|---|---|---|
| `$02` | zp_flag_02 | Zeroed in game_entry; purpose TBD |
| `$03` | zp_flag_03 | Multi-sub game flag; read by cockpit_display |
| `$04` | level_or_state | Level number / game phase; $00=fresh; drives branching in game_entry |
| `$05` | zp_flag_05 | Zeroed in game_entry |
| `$09` | zp_flag_09 | Set $01 by XEX INITAD signal |
| `$0C/$0D` | zp_entry_lo/hi | Final XEX run vector ($3CDE) |
| `$12/$13/$14` | RTCLOK+2/1/0 | OS jiffy clock (fastest at $14; used for timing waits and attract timer) |
| `$34` | terrain_pos? | Compared to $40 in main loop exit condition (pilot rescue trigger?) |
| `$3A` | level_cleared | $01 → set level_cleared_flag ($2849) |
| `$3D` | landing_flag | 0 → skip rescuing; non-zero → approach |
| `$3E` | prev_game_state | Previous game state (compared to $0041) |
| `$41` | game_state | Global state flag; written from 11+ functions; key control variable |
| `$42` | game_phase | Phase 0=init / 1=active / 2=post-level |
| `$44` | pacing_timer | Countdown timer ($54/$0E); game loop pacing |
| `$45/$46` | player_y_lo/hi | Vertical position 16-bit |
| `$47` | player_dir | Direction or facing |
| `$49/$4A` | joystick_raw/saved | Joystick + processed value; $49 → $061D at entry start |
| `$4B` | collision_flags | BIT-tested in startup_init (bit 7 = PM collision?) |
| `$4C` | timer_4C | General-purpose counter/timer (12 writes across subs) |
| `$6C` | game_var_6C | Zeroed at $3D42 in game_entry |
| `$6D` | game_var_6D | Set $04 then cleared; loop counter in game_entry init |
| `$71` | display_flags | $C0 at init → $00 attract → $2A game mode flags |
| `$72` | player_lives | Life/continue counter; compared to 2 in main loop |
| `$7E` | score_display | Set $80 at game start |
| `$80` | vbi_sync | VBI sync flag: VBI sets, main code waits then clears |
| `$81/$82` | dl_ptr | 16-bit pointer to display list being built ($B800 during attract) |
| `$83/$84` | screen_ptr | 16-bit pointer into screen data during DL build |
| `$85` | encounter_count | Encounter counter ($1E max); decremented in display_list_build |
| `$86` | row_count | Row loop counter in display_list_build |
| `$87/$88` | vbi_phase/flags | VBI phase and display flags |
| `$B7` | frame_counter | Frame counter; zeroed each attract/game cycle; used for timing |
| `$BB–$BE` | dl_y1–y4 | Display region Y limits / scroll offsets (different per game mode) |
| `$BF` | attract_count | Attract mode counter (zeroed in startup_init) |
| `$C1` | player_x | Horizontal position ($60 before game loop) |
| `$C2` | player_speed | Ship speed ($10 in display_setup) |
| `$C3` | player_altitude | Altitude / Y position ($0F in display_setup) |
| `$C4` | altitude_limit | Altitude floor limit ($08 in display_setup) |
| `$C7` | game_var_C7 | Zeroed at $3D76 |
| `$CF–$D7` | display_params_0-8 | 9-byte display config table (loaded from $4DF1: `04 26 2C 90 00 06 44 9A 78`) |
| `$D9` | clear_pm_state_D9 | Zeroed by clear_pm_state ($3FBF) |
| `$DA–$DD` | pm_state | Player state 4 bytes (zeroed by clear_pm_state; $DD clamped to $2A) |
| `$DC` | game_var_DC | Zeroed at $3D72 |
| `$DE` | anim_counter_1 | Animation counter (clamped at $46; counts from $4E in loop) |
| `$E2` | attract_timer | $64 = 100 ticks at game_entry start |
| `$E4/$E6` | game_vars_E4/E6 | Zeroed at game_entry |

## OS page 2 vectors & shadows ($0200–$02FF) — key entries

| Addr | Name | Value(s) during game | Notes |
|---|---|---|---|
| `$0200/$01` | VDSLST | $49EE → $6CC2 | DLI vector; changes from game_entry to display_setup |
| `$0216/$17` | VIMIRQ | $462A | IRQ handler (POKEY timer IRQ) |
| `$0222/$23` | VVBLKI | $1B30→$53CC→$4FF5→$52D7 | VBI handler; cycles through 4 addresses as game progresses |
| `$022F` | SDMCTL | $22 (normal) / $00 (disabled) | Playfield + PM DMA control shadow |
| `$0230/$31` | SDLSTL/H | $B832→$3120/$316B | Display list pointer shadow |
| `$026F` | GPRIOR | $71 (attract) / $11 (game) | GTIA priority mode |
| `$02C0–$C3` | PCOLR0–3 | varies | Player 0–3 colors (attract: set by pmg_colors_station) |
| `$02C4–$C8` | COLOR0–4 | $9C $08 varies | Playfield 0–4 colors (init_B800: COLOR0=$9C sky blue, COLOR1=$08 dark) |
| `$02F4` | CHBAS | $04 | Character set base = page 4 ($0400–$04FF = custom font) |
| `$02FC` | CH | $FF (no key) | Keyboard shadow |

## Screen data / game RAM ($0400–$13FF)

| Range | Name | Size | Contents |
|---|---|---|---|
| `$0400–$05FF` | bss_area | 512 | Zero-filled BSS (zeroed by XEX loader segment 12) |
| `$0600–$062B` | game_state_area | 44 | Game state vars (zeroed at game_entry; see $0627/$062F etc.) |
| `$0600–$1309` | screen_data | 4874 | 122 rows × 40 bytes; display list rows point into this |
| `$1010–$130F` | terrain_tables | 768 | 8 rows × $60 bytes pre-loaded terrain heightmap (within code seg 1) |

**Important $06xx game state vars:**

| Addr | Name | Notes |
|---|---|---|
| `$060B` | cockpit_flag | Non-zero → cockpit_display runs |
| `$060C` | screen_state | Cleared; display/mode state |
| `$061D/$1E` | joystick_saved | Joystick raw $49/$4A copied here at start |
| `$0627` | fresh_start_flag | 0 = first run (play intro); non-zero = skip intro |
| `$062F` | life_counter | Compared to $0E (14); pilot rescue fuel/lives |
| `$0633` | alien_trigger | Non-zero → pmg_enemy_update called each frame |
| `$063D` | event_trigger | Non-zero → game_sub_4f3f called each frame |
| `$0642` | game_phase_flag | 0=intro / 1–2=active / 3=transition |
| `$0645–47` | shield_damage | Set $80 at game start |

## Main game RAM ($2800+)

| Addr | Name | Notes |
|---|---|---|
| `$2836` | terrain_sub_index | Index into display terrain lookup |
| `$2849` | level_cleared_flag | Set when level completes |
| `$283B` | level_ready_flag | BPL-polled (sign bit = ready); synchronises level generation |
| `$288D–$8F` | pilot_visible/prev/state | Pilot rescue state machine |
| `$288E` | pilot_prev | Previous frame pilot flag (edge detect) |

## Terrain tables

Eight pre-loaded "strips" of terrain, each $60 (96) bytes wide, accessed by
column index. Used by `terrain_collision_and_silhouette` to find terrain height at ship X:

```
$1010  row 0   CMP $1010,Y
$1070  row 1   CMP $1070,Y   (Y = ship column from game_entry caller)
$10D0  row 2
$1130  row 3
$1190  row 4
$11F0  row 5
$1250  row 6
$12B0  row 7   ← lowest/closest to ground
```

`terrain_collision_and_silhouette` ($AE53, 799 bytes) scans rows 0→7; first non-$00 row at
the ship column → land at that strip, jump to handler at `$B12F` with X=row.

## Display lists

Three display list locations; only one active at a time:

| Address | Name | When active |
|---|---|---|
| `$B832` | display_list_init | Immediately after init_B800 INITAD runs |
| `$B800` | display_list_attract | After display_list_build ($1C40) builds attract mode DL |
| `$3120` | display_list_game | During game; DLISTL/H = $20/$31; set by display_setup |
| `$316B` | display_list_alt | Alternate game DL; set by display_setup for different screens |

## Custom font

Loaded/referenced at page `$04` (address `$0400`). `CHBAS = $04` set by
`init_B800`. `CHBASE = $04` set directly at `$3D8A` in game_entry. The BSS at
`$0400–$05FF` (loaded as zeros) is filled with font data at runtime.

## Key interrupt handlers summary

| Handler | Address | Set by | Trigger |
|---|---|---|---|
| `irq_handler` | `$462A` | game_entry ($3D28) | POKEY IRQ (timer 1+2) |
| `vbi_handler_station` | `$1B30` | station_init | VBI during attract |
| `vbi_handler_1` | `$53CC` | game_entry ($3D63) | VBI early in-game init |
| `vbi_handler_2` | `$4FF5` | game_entry ($3E50) | VBI mid-game-entry setup |
| `vbi_handler_game` | `$52D7` | display_setup ($5F50) | VBI during gameplay |
| `dli_handler_game` | `$49EE` | game_entry ($3E6D) | DLI (mid-screen) |
| `dli_handler_game2` | `$6CC2` | display_setup ($5F5F) | DLI during gameplay |
| `audio_irq_handler` | `$8237` | (timer) | Timer-driven sound update |

## POKEY RANDOM usage

41 read sites across 24 functions. NOT a single tight loop — used pervasively:
- **display_list_build** ($1C40): 3 reads — random encounter/enemy placement on screen
- **terrain_gen_2** ($A31E): 2 reads — terrain generation randomization
- **intro_random_setup** ($6FBF): 5 reads — startup randomization  
- **pilot_render** ($7854): 3 reads — pilot/rescue visual effects
- **game_state_update** ($A99C): 2 reads — game event randomization
- Many other functions: 1–2 reads each for various probabilistic effects

**Critical**: POKEY's 17-bit LFSR must be reproduced bit-exactly in the SDL
and Amiga implementations for terrain/encounter positions to match the original.
See `docs/atari-hardware.md` for the LFSR specification.
