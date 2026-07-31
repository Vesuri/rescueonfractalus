# Rescue on Fractalus! — SFX event / voice-engine reference

Decoded from `rof.xex` (`disasm/rof_mem.bin`) 2026-07-31. This is the ground-truth map of
the in-flight / cinematic sound-effect system so we don't re-derive it every session. All
addresses are Atari 6502. The corresponding Amiga twins live in `rof_native.c` /
`PlatformAmiga.cpp` (POKEY→Paula in [[amiga-pokey-audio]]).

## The model

Sounds are **data-driven "events"**, identified by an event id (1..$21). Pushing an event
loads a set of per-voice parameters into one of **16 voice slots** (`Y` = slot, 0..$f). Each
frame a voice engine walks the slots, advances their frequency/volume **envelopes**, and a
**priority mixer** assigns the (up to) 4 POKEY hardware channels to the loudest slots.

### Key routines

| Addr | Name | Role |
|---|---|---|
| `$5815` | `game_sub_5815` (ring_push_marked) | Push event id `X` onto the SFX ring `$0719+` with bit7 SET (`ORA #$80`) → "load immediately" marker. Callers pass the event id in X. |
| `$55fc` | `game_sub_55FC` (ring_push_0719) | Push a raw byte (bit7 clear) onto the ring at `$0073` = a **slot index** for the priority path. |
| `$581c` | `input_init` **(MISNOMER — it's the SFX event loader; see docs/rename.md)** | Load event: `X` = event id, `DEX` → table index. Reads the 12 parameter tables (below) and writes them into voice slot `Y=slot[X]`. |
| `$548d` | `FUN_548d` (sfx_voice_envelope_tick) | Per-frame: for each slot Y=$e..1, advance the **freq envelope** (`$6db+Y` phase → `$679+Y` freq += `$6bf+Y` toward target `$6cd+Y`) and **vol envelope** (`$6a3+Y` phase → `$66b+Y` vol += `$687+Y` toward `$695+Y`). When a phase completes it pushes the **follow-on event** `$6f7+Y` (event-chaining). Then drains the ring. |
| `$5614` / `$568a` / `$56af` | priority mixer | Pick the loudest ≤4 slots, assign POKEY channels, evict lower-priority voices. |
| `$5673` | `FUN_5673` (sfx_voice_write_freq_ctrl) | Write a slot's freq → `AUDF`, dist\|vol → `AUDC` for its assigned POKEY channel (`$05+Y`). |

### Per-voice slot state (base + Y)

| Base | Meaning | Loaded from table |
|---|---|---|
| `$065d+Y` | distortion (AUDC hi nibble) | `$56f5` hi nibble |
| `$066b+Y` | volume (AUDC lo nibble) | `$56f5` lo nibble |
| `$0679+Y` | frequency (AUDF) | `$5716` |
| `$06a3+Y` | vol-env phase enable/step | `$5737` |
| `$0687+Y` | vol-env delta | `$5758` |
| `$0695+Y` | vol-env target | `$5779` |
| `$06b1+Y` | vol-env phase-position step | `$579a` |
| `$06db+Y` | freq-env phase enable/step | `$57bb` (nonzero ⇒ load the extra freq-env block below) |
| `$06bf+Y` | freq-env delta | `$57dc` |
| `$06cd+Y` | freq-env target | `$57e4` |
| `$06e9+Y` | freq-env phase-position (clamped 0..$2c) | `$57ec` |
| `$06f7+Y` | **follow-on event id** (pushed when a phase completes) | `$57f4` |

The phase-position indexes a gate table **`$5406`** (0x2d bytes): the envelope only advances
on ticks where `$5406[pos]!=0`. `$5406` = `01 00 00 00 00 00 00 00 00 00 01 00 00 00 00 01
00 00 00 00 01 00 00 00 01 00 00 01 00 00 01 00 01 00 01 00 01 00 01 00 01 01 01 01 01` — so
low positions tick slowly, high positions every frame (this is what makes the freq sweeps
audibly "ramp").

### Event chaining

When a voice's freq- **or** vol-envelope reaches its target, `$0718` is bumped and the engine
pushes `$06f7+Y` (the follow-on event). Chains let one push play a multi-part sound
(e.g. `$04→$05`, `$07→$08`, `$0a→$0c`, `$0b→$0d`, `$0f→$09`, `$11→$06`, `$16→$17→$18`,
`$1d→$1e→$12`). **Event `$00` = no-op** (loader `DEX`→$ff, `BMI` returns). Only events
`$01–$08` (table index 0..7) have the freq-env enabled (`$57bb`=1); all others are
fixed-frequency, volume-enveloped only.

## The 33 events (id → sound)

`dist`: pure=$a0, poly5=$20, poly4=$40, poly5tone=$60 (tone+poly5), noise/combo=$00/$80.
`fΔ`/`ftgt` only matter when the freq-env is enabled (events $01–$08).

| id | slot | dist | vol | freq | fΔ | ftgt | follow | what it is (best guess) |
|----|------|------|-----|------|----|----|--------|-------------------------|
| $01 |  5 | **poly4** | 4 | $3a | -1 | $1f | — | **poly4 down-sweep $3a→$1f** — the "systems ready" chirp loaded by the `$63xx` docking/level-start sequence at `$63d2`. **Only load site.** |
| $02 |  5 | — | 0 | 0 | +4 | $fc | $15 | clear slot 5 (chains $15) |
| $03 |  5 | — | 0 | 0 | +4 | $fc | — | clear slot 5 |
| $04 |  6 | poly5 | 1 | 0 | +1 | $fc | $05 | (chains $05) |
| $05 |  6 | poly5 | 15 | $0f | +1 | $fc | — | loud poly5 sweep |
| $06 |  4 | noise | 15 | 0 | +6 | $ff | — | loud noise burst |
| $07 |  6 | poly5 | 0 | $07 | +1 | $fc | $08 | (chains $08) |
| $08 |  6 | poly5 | 7 | $07 | +1 | $fc | — | poly5 sweep |
| $09 |  7 | noise | 15 | $ff | +$1f | $2d | — | noise |
| $0a |  3 | pure | 1 | $ff | -4 | $2d | $0c | (chains $0c) |
| $0b |  4 | poly4 | 1 | $43 | -4 | $2d | $0d | (chains $0d) |
| $0c |  3 | pure | 15 | $ff | -4 | $2d | — | loud pure |
| $0d |  4 | poly4 | 15 | $43 | -4 | $2d | — | loud poly4 |
| $0e |  4 | noise | 15 | $bf | -1 | $2d | — | **engine drone** (noise F$bf) |
| $0f |  7 | noise | 0 | $ff | -4 | $2d | $09 | (chains $09) |
| $10 |  3 | pure | 8 | $1e | -4 | $2d | — | pure (loaded at `$61aa`) |
| $11 |  4 | noise | 10 | 0 | +$2d | 0 | $06 | (chains $06) |
| $12 |  3 | pure | 8 | $0f | +$2d | $15 | — | pure |
| $13 |  3 | noise | 8 | $ff | +$2d | 0 | — | noise |
| $14 |  3 | **pure** | **3** | **$2f** | — | — | — | **RANGE-TO-PILOT / LR-scanner-blink beep** — pushed by `startup_init $3FFA` at `$4016` (gated: range digit `$0642`∈{1,2}, `(digit&$004b)==0`, `$0072`≠0) and by the flight loop at `$a469`. Static pure ~665 Hz blip. **This is the correct range-1 sound.** |
| $15 |  5 | — | 0 | 0 | +$2d | 0 | — | clear slot 5 |
| $16 |  8 | noise | 7 | 0 | +$2d | 0 | $17 | (chains $17); pushed in the descent/`$79ff` path |
| $17 |  8 | noise | 1 | 0 | +$2d | $08 | $18 | (chains $18) |
| $18 |  8 | noise | 7 | $40 | +$2d | 0 | — | noise |
| $19 |  9 | noise | 1 | $18 | — | — | — | **descent beep noise** (`$7a17` loop) |
| $1a |  8 | poly4 | 6 | $08 | +$15 | $0c | — | poly4 (pmg_enemy / `clear_colors_sweep_5x $7a89`) |
| $1b |  9 | poly5tone | 14 | $b1 | — | $0d | — | **descent beep tone** (`$7a5e`) |
| $1c | 10 | poly4 | 3 | $28 | +5 | 0 | — | poly4 |
| $1d |  3 | pure | 8 | $3f | — | — | $1e | (chains $1e); loaded at `$63a9` |
| $1e |  3 | pure | 8 | $1f | — | — | $12 | (chains $12) |
| $1f |  1 | pure | 0 | $fe | +8 | $09 | — | loaded at `$3df5` (silent, sets up ch1) |
| $20 |  2 | pure | 0 | $ff | — | — | — | loaded at `$3df9` (silent, sets up ch2) |
| $21 |  3 | pure | 4 | $0b | — | — | $06 | pure |

Push/load sites of note (`JSR/JMP $5815` = ring push; `JSR $581c` = direct load):
`$4016`→$14, `$a469`→$14, `$63d2`→**$01** (direct, only site), `$63a9`→$1d, `$61aa`→$10,
`$3df5/$3df9`→$1f/$20, `$7a23`→$19, `$7a5e`→$1b, `$7a9d`/`$7aec`→$1a, `$7af0`→$1b (INX),
`$7ab5`→$16, `$7bd8`→$1d.

## Atari ground truth — pilot-approach POKEY streams (captured 2026-07-31)

Method: `atari800 -nobasic -state <a8s> -pokeyrec -pokeyrec-ascii -pokeyrec-interval 312`,
run **forward** (savestate resumed via the FIFO monitor, quit cleanly so pokeyrec flushes).
One line/frame = `AUDF1 AUDC1 AUDF2 AUDC2 AUDF3 AUDC3 AUDF4 AUDC4 AUDCTL`.

- **`lrscanner.a8s` (range 1–2, the reference):** the range beep is one channel toggling
  `C=$a3` (pure, **vol 3, F=$2f**) for ~6 frames ↔ `C=$81` (vol 1) for ~2 frames — a pulsing
  **pure** blip. Drone on the other channels (pure `$a1`, F≈$c2). **AUDCTL=$60. NO poly4
  anywhere.** This is what the Amiga *should* produce at range 1.
- **`longrange.a8s`:** same pure pulse, farther out (drone freqs slowly sweeping up).
- **`descent1/descent2.a8s`:** these are the **planet-approach descent** (launch cinematic),
  NOT a pilot approach — a multi-channel pure down-sweep then the ch1 noise sweep `$b4/$8f`
  (the launch buzz fixed in 5615dcc). Unrelated to the range-1 beep.
- **`rescue_pilot.a8s` / `rescue.a8s`:** NOT relevant to the range-1 beep (per user).

## Bearing on the Amiga range-1 "wrong sound" bug

The Amiga plays a **poly4 down-sweep $3a→$1f (AUDC=$44, vol 4)** at range-1 flyover; the Atari
plays only the **pure pulse beep** (event $14) — confirmed above, no poly4. Those exact
params (`poly4, vol4, freq $3a, fΔ −1, target $1f`) are **uniquely event $01**, and event
$01 is loaded **only** by the `$63xx` sequence at `$63d2` — never by the range/approach code.
So on the Amiga the poly4 is either (a) a **stale slot-5 voice** left from a prior event-$01
load (e.g. the level-start chirp) that the priority mixer re-promotes when the range beep
competes for a channel, or (b) an Amiga-specific mis-load. It is **not** a fresh push from the
approach path. See [[pilot-proximity-beep]] for the open investigation + capture harness.
