# Rescue on Fractalus! — SFX event / voice-engine reference

Decoded from `rof.xex` (`disasm/rof_mem.bin`) 2026-07-31. This is the ground-truth map of
the in-flight / cinematic sound-effect system so we don't re-derive it every session. All
addresses are Atari 6502. The corresponding Amiga twins live in `rof_native.c` /
`PlatformAmiga.cpp` (POKEY→Paula in [[amiga-pokey-audio]]).

⚠ **"SFX" covers three unrelated things in this binary — know which one you are looking at.**
Confusing them has cost a session already.

| What | Driver | Where |
|---|---|---|
| The 33 envelope-driven **effects** (lasers, explosions, beeps) | `sfx_event_load $581C` + `sfx_voice_envelope_tick $548D` | this file, below |
| The **Standby attract theme** — a real composed tune | `sfx_voice_tick $70F9` + `sfx_seq_step $7148` | §The Standby theme |
| The two **jingles** (game over; level complete / high score) | `music_init_state $7238` + `music_player_tick $7253` | §The two jingles |

`station_audio $1B5B` is a fourth thing and is **not** music at all: it is clock-modulated
AUDC/AUDF sweeps keyed on the jiffy clock, with no pitched content anywhere. Don't go looking
for a melody in it. Its CH3 envelope is the **missile-relaunch blip**, and the
port sounds it (plus a fade-out at the Standby hand-off) per the v5.0 cartridge — see
`docs/rom-v50-diff.md` §4.8, which is also where the four `PRE_INSN_HOOKS` that do it are named.

All three players are transcribed to General MIDI by **`tools/export_midi.py`** (stdlib only,
`--out build/midi`), which is also the executable record of every decode detail below.

## The model

Sounds are **data-driven "events"**, identified by an event id (1..$21). Pushing an event
loads a set of per-voice parameters into one of **16 voice slots** (`Y` = slot, 0..$f). Each
frame a voice engine walks the slots, advances their frequency/volume **envelopes**, and a
**priority mixer** assigns the (up to) 4 POKEY hardware channels to the loudest slots.

### Key routines

| Addr | Name | Role |
|---|---|---|
| `$5815` | `game_sub_5815` (ring_push_marked) | Push event id `X` onto the SFX ring `$0719+` with bit7 SET (`ORA #$80`) → "load immediately" marker. Callers pass the event id in X. |
| `$55fc` | `ring_push_unmarked` (ring_push_0719) | Push a raw byte (bit7 clear) onto the ring at `$0073` = a **slot index** for the priority path. |
| `$581c` | `sfx_event_load` **(MISNOMER — it's the SFX event loader; see docs/rename.md)** | Load event: `X` = event id, `DEX` → table index. Reads the 12 parameter tables (below) and writes them into voice slot `Y=slot[X]`. |
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

## ⭐ A poly-distortion voice does NOT sound at its AUDF frequency

Applies to every voice in this file whose `dist` is poly4 / poly5 / poly5tone, and it is the
difference between a coherent transcription and nonsense. POKEY's polynomial counters run
continuously off the master clock and are only **sampled** at each frequency-divider underflow.
So between samples the counter advances by `stride = (AUDF+1) * base_div` (28 with AUDCTL bit0
clear), the visible bit sequence is the poly stream stepped by `stride % len`, and the
waveform's repetition period is therefore **a function of AUDF**:

```
audible fundamental = freq(AUDF) * 2 / period      period = repetition length in output samples
freq(AUDF)          = 1789790 / (56 * (AUDF+1))    pure tone: period 2, so it sounds at freq()
```

`PlatformAmiga.cpp` already models exactly this — `poly4_wave[stride % 15]` /
`poly5_wave[stride % 31]`, one pre-rendered shape per stride residue — which is why the port
plays these voices correctly. `tools/export_midi.py:wave_period()` is the same computation.

**How far off it gets, from the two real cases:**

| Voice | dist | period | Offset | Read as AUDF pitch | Read correctly |
|---|---|---|---|---|---|
| Jingle voice 6 (bass, both songs) | `$C0` poly4 | 5 for **every** note | ÷2.5, −15.9 semitones | `C3 G3 G#3 …` — wrong key | `G#1 D#2 E2 …` — the tonic |
| Theme channel 4 (drone) | `$22`/`$C2` | 62 / 15 / 5 | up to −5 octaves | `F#7 D3 B4 C4` — incoherent | root or fifth of all 13 chords |

Two things worth keeping from that: the offset is **constant per voice only by luck** (the
jingle bass happens to hit `stride % 15` ∈ {3,6,9,12}, all period 5, so its intervals survive a
naive reading and only the octave is wrong — the theme drone's period genuinely varies per
chord), and **landing on chord roots is the cheap self-check** that the correction is right, with
no emulator round trip.

## The Standby theme (`sfx_voice_tick $70F9` / `sfx_seq_step $7148`)

The attract tune, and the reason it resists being read as a note list: **no channel carries the
melody.** Score stream at `$71DB` (93 bytes, `$00`-terminated), scanned via `$073C`:

- **bit7 SET = a chord preset**, index `byte & $1F` (13 presets). Writes all four AUDF at once
  from four *parallel* tables — AUDF1 `$71AB`, AUDF2 `$719E`, AUDF3 `$7191`, AUDF4 `$71B8` —
  plus AUDC4 from `$71C5`, then keeps scanning. Harmony moves in blocks, independently of the
  notes. (A `$71C5` entry of 0 would fall through as a rest; none is 0.)
- **bit7 CLEAR = a note.** Low 5 bits index the duration table `$71D2` (9 entries) into `$073A`;
  the **high nibble** goes to `$073B` and selects which register is emphasised via
  `STA $D1FF,Y` → Y = 2/4/6 = AUDC1/AUDC2/AUDC3. High nibble 0 mutes all three (a rest).
- Channels 1–3 sound **together, at the same volume**, for the whole tune; the melody is
  whichever one is 2 louder. Channel 4 is never touched by the tick — a drone whose pitch and
  waveform change only on a chord preset.
- `$00` terminates and the player restarts at index 0 (`TAX`/`BEQ` back into the fetch), so the
  theme loops.
- Volume per tick: `v = min($073A>>1, 2)`, forced to 3 while the timer is high (the `CMP #3`
  carry feeds the `ADC #$A0`) — so a note holds at 3 and decays 2,2,1,1,0,0 over its last six
  ticks. The emphasised channel gets `v+2`.

The presets decode to clean triads in **F# major** (`C#-F#-A#`, `F-A-C`, `G#-C-D#`, `F#-B-D#`…)
with the drone on each chord's root or fifth, and the melody opens `F# C# F# A# F# | A# F# A# C#`
over a 17/3/4-tick figure.

⚠ **The theme ticks every OTHER frame.** Its only call site is `$5356`, gated by
`LDA $00E7 / BIT $062D / BNE`: `$00E7` is 1 while the theme plays (confirmed in
`a800dumps/music_playing.a8s`) and `$062D` is bumped once per frame, so the call happens only
when bit0 of the counter is clear. Miss this and the tune plays at **double speed**. The
**jingles are not gated** (`$5359`-`$535E`) and do run every frame.

That savestate is also the cheap way to check any theory about this driver without an emulator
round trip — its live `$073A`/`$073B`/`$073C` must agree with your model (seq ptr `$11` → score
byte `$23` → duration table[3] = 11 and sel 2, which is what the dump holds).

## ⚠ TV system sets the tempo AND the pitch — and the answer here is PAL

Both players are driven from the VBI, so **the field rate IS the tempo**, and the same master
clock divides down to the tone frequency. Getting this wrong makes every piece play 20% off, and
it is invisible to any amount of static reading of the 6502.

| | Field rate | Theme tick | Theme tempo | One pass |
|---|---|---|---|---|
| **PAL** (1773447 Hz, 312 lines) | 49.8607 Hz | 24.9304 Hz | **31.2 BPM** | 30.81 s |
| NTSC (1789790 Hz, 262 lines) | 59.9233 Hz | 29.9617 Hz | 37.5 BPM | 25.63 s |

**PAL is what both of our references run** — the project's atari800 ground truth
(`~/.atari800.cfg`: `DEFAULT_TV_MODE=PAL`) and the Amiga target (`amiga/run.sh` passes
`--ntsc_mode=0` to an A500+). So PAL is `tools/export_midi.py`'s default; `--tv ntsc` is there
for comparison. ⚠ The game is a US NTSC original, so **"faithful to the 1985 machine" and
"matches what we can hear" disagree by 20% here** — say which one you mean.

Pitch is barely affected: PAL's clock is 0.9% flat = 0.16 semitones, and it changes **0 of 252**
notes across all three pieces after rounding. Only the tempo matters.

## The two jingles (`music_init_state $7238` / `music_player_tick $7253`)

Song table at `$731E`: two 6-byte headers = `(stream_lo, stream_hi, level_loud, level_soft,
attack_delta, release_delta)`. Song 0's stream is `$7346` (game over, `LDY #5` at `$5896`);
song 1's is `$732A` (level complete / LIFT at `$61C3`, and the high-score entry at `$5BED` —
**the same tune**, per [[high-score-initials]]). Voices are indexed `X = 0/2/4/6`, so voice 6
writes AUDF4/AUDC4; distortion comes from `$73C1+X` = `$A0 $A0 $A0 $C0` — the top three are
pure, **voice 6 is poly4** (see the pitch rule above).

- Stream = `(duration, voice-command)` pairs, interleaved with instrument selects (`>= $C0`,
  table `$7375 + ((~cmd)*4)`, four AUDF bytes **voice 6 first**). The pointer advance at
  `$72BB`-`$72BF` skips **both** bytes of the pair — advancing past only the duration byte
  re-reads each command byte as the next event's leader and misaligns everything after event 1.
- The voice command packs four 2-bit codes, voice 6 in the top bits: `00` off, `01` hold/tie
  (nothing touched), `10` soft retrigger, `11` loud retrigger.
- Envelope: a new event gives every voice 4 attack ticks, then **all** voices switch to
  `release_delta` for the remaining `duration` ticks — so an event lasts `4 + duration` ticks.
  Per tick `accum += delta` as an 8-bit `ADC`, clamped to 0 if the result looks negative
  (`$7309 BPL`), and `AUDC = (accum >> 3) ^ dist` — the volume nibble **is** the envelope.
- `music_init_state` leaves `$0651`(attack)=0, AUDCTL=0, `$0653`(duration)=`$0655`(playing)=1,
  so the first tick immediately loads event 1. **AUDCTL = 0 for both players**, so every channel
  is 8-bit on the 64 kHz clock — no 16-bit joins, no 15 kHz base.

⚠ **The game-over jingle has a ritardando** and the player has no tempo: an event only names a
length in ticks, so the slowdown appears as the lengths `49,49,49,57,65,73,81,81` — musically one
repeated note value getting slower. Anything that treats those as literal durations at a fixed
tempo (a transcription, an A/B against a capture) will read the tail as meaningless tuplets.

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

## The saucer tone's noise waveform — CLOSED 2026-08-31 (the DMA-restart off-window)

Reported 2026-08-06 while ear-checking the SFX mixer asm twin: the high-pitched tone that plays
the whole time a flying saucer is present (that part is *correct*) **sometimes used the noise
waveform**. Provisionally closed 2026-08-13 as not-reproduced; **attributed 2026-08-31 to the
DMA-restart off-window** (§…flooring the period was NOT sufficient) — the saucer tone is a pitched
voice, so a channel arriving from the engine drone's `noise_buf` played noise at its onset until
the 4096-word loop wrapped, and "sometimes" is the DMA phase. ⚠ Attributed, not *proven*: it was
never reproduced after the report, so there was nothing left to A/B. It matches the mechanism and
all four confirmed siblings; the four suspect leads below are kept only in case a saucer-specific
symptom outlives the fix.

**Cleared of the mixer asm twin (a2f331f) — do not re-litigate that part:** `make FUZZ=1` +
`amiga/sfxmix_fuzz.gdb` ran 7458 randomised on-target cases with **0 mismatch across all six
paths** (incl. the `$5628` priority tie-break), comparing `pokey[0..15]` — i.e. the AUDF/AUDC
distortion and volume nibbles themselves. `make validate FN=sfx` ties the C twin to the `$5614`
oracle (11 fixtures). The user also A/B'd `RoF-asm.exe` against a `make SFXMIX_C=1` build by ear:
no audible difference.

**The four live suspects, in cost order, if it ever comes back:**
1. **A wrong event id pushed** — the cheapest explanation, and it has a precedent of exactly this
   shape: the range-1 "wrong sound" was an Amiga twin (`startup_init_native`) open-coding a ring
   push with a live register as the event id, pushing `$01` (poly4) instead of `$14` (pure).
2. **A stale `$00DF`.** `$51ab` (the flight-VBI keyboard-command window) pushes a *variable* as an
   event id — `LDX mem[$00DF]; BNE; JSR $5815`. Faithful, but it means whatever glyph the keyboard
   dispatcher last left in `$00DF` becomes a sound event.
3. **Voice slot 3 is shared by pure AND noise events** — `$14`/`$21`/`$0a`/`$0c`/`$10`/`$12`/`$1d`/
   `$1e` are pure; **`$13` is noise** (slot 3, dist `$00`, vol 8, freq `$ff`). A stray `$13` into
   slot 3 would sound exactly like the report.
4. **An Amiga-only DMA-phase hazard, i.e. no wrong byte at all.** `update_paula_channel`'s own
   comment documents a note's onset playing as noise instead of a square *intermittently, depending
   on the DMA phase*, because Paula latches PTR/LEN only at the next DMA loop wrap. ⚠ This is why a
   timing-only change (a faster ISR, say) can alter how OFTEN this is audible without changing a
   single POKEY write — so an A/B by ear cannot exonerate a timing change here.

⚠ **Nobody ever identified which event the saucer tone actually is.** The saucer spawn itself
(`terrain_draw_frame_core`'s spawn branch) pushes **no** sound event, so the push is elsewhere —
look for a per-frame one keyed on object presence / target-visible (`$28FB`/`$28FC`,
`mem[$006A]==$7F`).

**The method that would settle it** (it is what settled range-1): capture the POKEY stream on both
sides with a saucer on screen and diff AUDC — `atari800 -pokeyrec -pokeyrec-ascii
-pokeyrec-interval 312` for ground truth vs `ROF_BEEP_CAP` on the Amiga (retired 2026-08-15 —
recover it from git history). Build
`make COMBAT=1 COMBAT_SAUCER=<small>` so saucers appear constantly; the boot default is level 4,
where they are sparse enough to cost minutes of flying.

**Also closed, as FAITHFUL: the thrust beep going quieter while a saucer is active.** In this
engine the **priority nibble IS the volume nibble** (`$066B+Y` → AUDC low nibble) and there are
only 4 POKEY channels, so `sfx_reorder_voice_slot` demoting a loser genuinely makes another voice
quieter. Related data point from the game's own tables: the thrust key's beep is event **`$21`**
(slot 3, pure, vol 4) and its follow-on is event **`$06` = a noise burst, vol 15** — so *some*
noise after a command beep is correct.

## ⭐ The Paula DMA-restart wait — floor the PERIOD, not the wait (2026-08-14)

`flush_paula` batches the frame's waveform changes through one DMA off → wait → on, because Paula
latches AUDxLC/LEN only at a loop wrap and a restart needs the channel held OFF for **>2 sample
periods of the period STILL LOADED** (HW manual §5-2-7). The wait used to be sized from the slowest
**outgoing** period, `wl = 2*max_per/227 + 4` clamped to [7,110] rasterlines.

**Why that was expensive in flight:** `update_paula_channel` selects a poly4/poly5 waveform by stride
residue (`poly4_wave[stride%15]`), so any voice that *sweeps* AUDF — the laser, an explosion — changes
its waveform POINTER every 50 Hz firing, and therefore restarts every frame for the length of the
sound. Measured: 32% of firings restarting, outgoing period averaging 13785, `wl` pinned at the **110
clamp** = 7 ms of busy-wait, **39.4 t/firing = the entire audio bracket and 32% of the whole flight
VBI**.

**The fix was one observation: the period still loaded is ours to choose.** Writing `kPaulaMinPer`
(124) to each restarting channel *before* the off-window makes the post-reset sample period ~0.5
rasterlines, and the formula lands on its 7-line floor every time. Per-restart wait **48.4 → 6.7
lines**; firing's ISR cost **+96% → +39%**. `make FLUSHWAIT_OLD=1` restores the old sizing for an
A/B. Record: `docs/flight-perf-log.md` §25.1-25.2.

⚠ **The clamp was hiding a correctness bug, not just a cost.** At per > 12031 the old formula wanted
more than 110 lines and got clamped, so the shipping code was already under-waiting the rule it was
written to satisfy — for every slow outgoing note.

### ⛔⭐ …and flooring the period was NOT sufficient — the premise "AUDxPER takes effect immediately" is wrong (fixed 2026-08-31)

**A write to AUDxPER changes only the RELOAD value. The countdown already in flight keeps its old
duration**, and the channel reaches idle only after that countdown expires *and then* one more
sample at the new period. Verified in `tmp/fs-uae/audio.cpp`: `AUDxPER` sets `cdp->per` and
deliberately does **not** touch `cdp->evtime`; `audio_state_channel2` leaves state 2/3 for
`zerostate` only via state 3's period event. So the honest window is **`outgoing_per +
kPaulaMinPer` ticks**, and the 7-line floor (1589 ticks) covers it only while the outgoing period
is under ~1465 — i.e. above ~1200 Hz. Every drone, bass and explosion tail is below that.

⭐⭐ **FS-UAE reports this failure out loud, and that is the cheapest instrument in the box.** Its
audio log line

```
Audio 3 DMA wait hack DISABLED. OFF=0021d686, ON=0021d740, PER=9392
```

means *the channel was still in state 2/3 when DMA was re-enabled* — the reset never happened —
and `PER=` is the countdown still in flight. `OFF`/`ON` are the two `dmacon` writes: their
difference (0xBA) matches `flush_paula`'s own two stores in `objdump`, which is how you prove the
lines are yours. A `FORCE_DEATH` run produced **59 of them, all from that one off→on pair.** The
hack that would have papered over it needs `usehacks()` = `cpu_model >= 68020 || m68k_speed != 0`,
so it is **off for a cycle-exact 68000** — as is real hardware. It appears in `.run/gdb-out.log`,
not `fsuae-dbg.log`.

**A missed reset is not silent failure**, and that is what decides how much it costs: Paula still
latches the new AUDxLC/LEN at the **outgoing loop's own wrap**, `cur_len` samples later.

| outgoing waveform | words | wrap after | missed reset is |
|---|---|---|---|
| `wave_pure` | 1 | 2 samples | invisible |
| `poly4_wave` / `poly5_wave` | 15 / 31 | ≤31 samples | <1 frame of stale timbre on a sweeping sound |
| `poly_dist_buf` | 511 | 511 samples | up to seconds of the WRONG waveform |
| `noise_buf` | 4096 | 4096 samples | 0.14 s at per 124, far worse at a drone period |

So the shipping rule is: **size the wait from the outgoing period only for channels whose outgoing
loop is longer than 31 words** (`wl = max_per/227 + 3`, clamped [7,160]). Where nothing is wrong the
behaviour is byte-identical: `max_per` stays 0, so `wl` clamps to the same 7-line floor as before.

**Measured cost** (`make PROBES=1 FORCE_RETURN=1 FORCE_RELAUNCH=1 AUDIO_TRACE=1` +
`amiga/audio_restart.gdb`, two boot → launch → flight → mother ship → relaunch → flight runs of
~14720 vblanks, 470-511 restarts each): long-loop restarts still under-waited **0** in both. Only
**37 / 43** restarts wait more than the floor, for **1607 / 1901 extra rasterlines in total =
0.04-0.06% of the run's beam time**. Worst single ISR spike **66 lines (1.3 ms, 21% of a frame)**,
from a drone at per 14301. So this is not a re-opening of §25 — quote the rasterline count, not
FPS: 0.05% is an order of magnitude below what `fps_seg` can resolve (⭐ a change this small is a
coin flip on SIGN there, so measuring it by FPS would produce a confidently wrong number in either
direction).

⚠ **`g_fpLenHist` is the wrong instrument for the flight-safety question**, even though it buckets
by exactly this length: `poly_dist_buf`'s live length is `poly_dist_len/2` — **465** words for the
gated-poly4 mode — so it lands in the histogram's `>=31` poly5 bucket, not its `>=511` one. Use the
per-restart rows.

**The lever not taken** (recorded so it is not re-derived): parking a silent channel's period at
`kPaulaMinPer` in `update_paula_channel`'s `vol == 0` arm would make every release→attack restart
land on the floor, since a silent channel's period is inaudible. It would remove most of the 37.
Not shipped — the measured cost did not justify a second behaviour change in the same fix.

⭐ **This is why "the first launch is always right".** Out of boot every channel sits on
`wave_pure`, where a missed reset is 2 samples long. After a flight they are parked on `noise_buf`
and `poly_dist_buf`, and the next note onset inherits up to a second of the old waveform at the new
volume. That is the whole family of reports: the post-mother-ship relaunch's three-note bleep
arriving as noise, the tunnel drone louder and buzzier, the game-over jingle's first notes as noise
on every channel — and the long-standing §Still open "a voice occasionally sounds the NOISE waveform
at onset".

⚠ **`build_poly_dist` measures 0 calls in flight and in the first ~13 s of standby** —
`g_polyDistCalls` is 0 in every window before/during/after a shot, and 0 over 2130 vblanks of a
`SKIPBOOT=0` logo → station → standby run. So it is **not** a flight cost; do not re-open it as one.
⛔⛔ **It is NOT dead code and must not be deleted.** `a800dumps/music_playing_ram.bin` (Standby tune
playing) has `AUDCTL=$E3` — POLY9 set — with **ch1 `AUDC=$07` vol 7 and ch2 `AUDC=$8E` vol 14 both in
the noise distortion**, which is precisely this path and precisely the user-confirmed d401d7d fix.
The zero count means the attract THEME had not armed yet (timeout is minutes; the window was ~13 s),
not that the path is unreachable. **A zero call-count proves "not taken in this window", never "dead".** It was the
filed prime suspect for `update_paula_channel`'s cost (§22.3). Do not re-open it as a *flight* item.
Its inner loop does have a proven-free simplification if the MUSIC path is ever profiled (a 1022-byte
rebuild is ~215 rasterlines): `if (kBit9[p9] == (out^1)) out ^= 1;` is unconditionally
`out = kBit9[p9]`, so a level table collapses it to one indexed load, and the `gateAlways` arm never
reads `kBit5` so its `p5` step hoists out. Byte-identical over all 31682 reachable (s5,s9,gate)
triples on the host; not shipped.

## POKEY 16-bit chain — the joined pair sounds on the HIGH channel (fixed 2026-08-18)

`AUDCTL` bit4 `CH1_CH2` ($10) joins ch0+ch1 and bit3 `CH3_CH4` ($08) joins ch2+ch3 into one 16-bit
counter. The port's POKEY→Paula model computed the joined divider but sounded it on the **low**
channel and left the high one running independently — backwards on both halves. Symptom of record:
the Station's 4.1 `AUDCTL=$29` + `AUDF3=$28` asked the low half for 21826 Hz, clamped to Paula's
minimum period 124, a squeal on every missile blip (`docs/rom-v50-diff.md` §4.8; that scene now
adopts 5.0's un-chained `$01` for its own reasons, so the two fixes are independent).

What real POKEY does — atari800's accurate model, `tmp/atari800/src/mzpokeysnd.c`
`Update_c0divstart` … `Update_c3divstart`:

| | divider (base-clock counts) |
|---|---|
| unchained, divided clock | `AUDF + 1`, times `mdivk` (28, or 114 with AUDCTL bit0) |
| unchained, 1.79 MHz (ch0/ch2 only) | `AUDF + 4` master-clock ticks — the base divider is bypassed |
| chained, **HIGH** half (ch1/ch3) | `AUDF_lo + 256*AUDF_hi + 1` times `mdivk`; `+ 7` at 1.79 MHz |
| chained, **LOW** half (ch0/ch2) | `256` — the counter no longer reloads from AUDF, it free-runs the full byte wrap. It keeps sounding (an unpitched buzz); software conventionally sets its AUDC volume to 0. |

The clock select and the join bit belong to the **pair**, not the channel: `CH1_179` ($40) and
`CH1_CH2` ($10) govern ch0+ch1; `CH3_179` ($20) and `CH3_CH4` ($08) govern ch2+ch3. So a chained
high half takes its clock from the *low* half's 1.79 bit, which the old code — testing `ch == 0 ||
ch == 2` — could never see.

**In the port** (`src/platform/amiga/PlatformAmiga.cpp`): one resolver, `pokey_divider()`, is the
single source of truth. It returns the divider plus the master-clock ticks per base count (1 when
the channel runs off 1.79 MHz), and *all three* consumers go through it — the Paula period, the
poly9 distortion stride, and the poly4/poly5 waveform-table stride — so they cannot disagree. They
previously each re-derived the chain, and the two stride sites got it wrong in a third way (they
ignored the chain and the 1.79 clock entirely). `PlatformSDL::updateChannelFreq` had the same
inversion *and* had `CH1_179`/`CH3_179` and `CH1_CH2`/`CH3_CH4` swapped; both fixed to match.

Two consequences worth knowing:

* **`AUDF1`/`AUDF3` writes now update the high half too.** A chained pair's divider contains the
  low half's AUDF, so `rof_pokey_write` re-derives ch1/ch3 on a `$D200`/`$D204` write when the
  matching join bit is set (this is atari800's `chan_mask`). Without it the pitch only moved when
  the *high* byte was written.
* **The `+4` at 1.79 MHz is a behaviour change outside the chain.** The unchained 1.79 path counted
  `AUDF+1`. Live callers: the Standby theme's `AUDCTL=$E3` puts ch0 and ch2 on 1.79 MHz — ch0
  (`AUDF1=$06`) is far above the audible cap and silent either way, and ch2 (`AUDF3=$7C`) moves
  7159 → 6991 Hz, a 2.4% shift on a noise-distortion voice. Nothing else sets $40/$20 with a
  sounding voice.

**Proof:** `make hostproof FN=pokey` — `tools/pokey_divider_test.c` checks the resolver's
divider × base_div against the mzpokeysnd rules transcribed independently, exhaustively over all
256 AUDCTL × 4 channels × 65536 AUDF pairs (67108864 cases), and checks `poly_stride_mod`'s two
branches (it keeps the unchained stride at the one MULU.W + one DIVU.W it always cost, and only
the chain pays the reduce-first identity) against the exact 64-bit product on both sides of its
cutoff. It is not vacuous: the pre-fix model fails it at `audctl=$08 ch=2`. ⚠ The proof holds a verbatim *snapshot* of `pokey_divider()` — green
means the resolver is right, not that the shipping source still matches the copy.

Not modelled, deliberately: mzpokeysnd reloads the low half with `AUDF_lo` on the single cycle the
16-bit pair itself reloads (`c0divstart_p`), a once-per-16-bit-period jitter that a fixed-period
Paula wavetable cannot carry.

## The poly5 gate wraps poly4 too — AUDC $40 (fixed 2026-08-18)

The airlock footsteps (the six the pilot takes *inside* the lock, after it opens) sounded like
clean square bleeps on the Amiga where the Atari has a distorted quality. They are **event `$1A`**,
pushed six times by `clear_colors_sweep_5x $7A89`, one per pass, ≥20 frames apart. Its parameters,
read from the event tables rather than inferred: **AUDC `$46`, AUDF `$08`** — distortion `$40`.

`$40` is poly4 **gated by poly5**. POKEY's poly5 gate encloses *all three* distortion families,
not just the noise one — atari800 `pokeysnd.c:797`:

```c
if ((audc & POKEY_NOTPOLY5) || bit5[P5]) {       /* <- the gate */
    if (audc & POKEY_PURETONE)   toggle = TRUE;
    else if (audc & POKEY_POLY4) toggle = (bit4[P4] == !(*out_ptr));
    else                         /* poly9 / poly17 */ ;
}
```

so `$C0` is ungated poly4 but `$40` is a different, rougher waveform. The port rendered `$40` as
ungated poly4 (an approximation `build_poly_one` carried as "close-enough raspy buzz"). At AUDF
`$08` the stride is `9*28 = 252`, so `s4 = 12` and the ungated poly4 phase orbit is `{0,3,6,9,12}`
— **period 5 samples, i.e. a pure tone**. That is the square bleep.

Why it can't be a per-stride table like the other two: `$40`'s shape depends on BOTH poly phases,
so the table would need `15*31 = 465` shapes. It is rendered per channel instead, into the
existing `poly_dist_buf` — `build_poly_dist` now takes a mode (`POLY_DIST_P9_GATED` /
`P9_UNGATED` / `P4_GATED`) and `want_poly_dist` owns the cache-key/repoint logic for all three.
Length **930 bytes = 15*31*2**, the exact `(p4, p5, out)` state-space bound, which fits the
1022-byte buffer.

⚠ **Skip the transient or the buffer clicks.** Inside the gate the output is *forced* to the poly4
bit, so the state map is not invertible and `(0,0,0)` need not lie on its own cycle: start emitting
there and **240 of the 465 strides** have `dst[0]` ≠ the sample that truly follows `dst[929]` — a
click at every DMA wrap, ~7.6/s at the footsteps' pitch. The transient is at most **6** samples
over all strides, and every reachable cycle length (1, 3, 5, 15, 31, 93, 155, 465) divides 930, so
skipping 6 and then emitting 930 is always a whole number of cycles. (The two per-stride tables,
`poly4_wave` 30 B and `poly5_wave` 62 B, were checked for the same defect and are clean.)

**Proof:** `make hostproof FN=poly` — `tools/poly_dist_test.c` renders all 465 strides and walks
the atari800 rule alongside for 3× the buffer length (1297350 samples, 0 mismatches), asserts the
transient bound and the cycle-divides-length property, and *reports* the 240/465 figure so the skip
is not mistaken for superstition later. ⚠ Snapshot caveat as always.

**Cost.** The rebuild is ~936 VBI iterations, the same order as the poly9 path. Only two events use
`$40` — `$1A` and `$01` (the level-start chirp) — and **neither runs during flight**. `$1A` has no
frequency envelope, so its stride never changes: one build, then five cache hits. `$01` sweeps
`$3a→$1f`, so it rebuilds once per frame for ~27 frames in a non-terrain scene.

### A voice occasionally sounds the NOISE waveform at onset — CLOSED 2026-08-31 (user-confirmed)

⭐ **It was the DMA-restart off-window, above** (§…flooring the period was NOT sufficient). The
airlock footsteps are event `$1A`, AUDC `$46` = gated poly4 → `poly_dist_buf`; the voice they
follow on slot 8 is one of the noise events `$16`/`$17`/`$18` → `noise_buf`, 4096 words. A
long→long restart under a 7-line window never reset the channel, so the footstep's onset played
the **outgoing noise buffer** until it wrapped. Fixed by sizing the window from the outgoing
period whenever the outgoing loop is long; user-confirmed by ear ("works great now").

⭐ **One cause, one fix, five reports** — this is the whole family, and none of them was a wrong
POKEY byte: the post-mother-ship relaunch bleep, the tunnel drone "louder/more distorted", the
game-over jingle's first notes on every channel, these footsteps, and §The saucer tone. Every
"the port picked the wrong waveform" theory chased for weeks was looking at *selection* when the
defect was in *when Paula latches it*. **If a wrong onset waveform is ever reported again**, the
two candidates below stay eliminated — go to the slot-8 priority-mixer handover.

Reported 2026-08-18: sometimes one of the first two airlock footsteps comes out as noise, and
(once, historically) the saucer tone did. ⚠ **The mothership bonus-points counter was reported
alongside these and is NOT part of the evidence — the user's later reading is that it is SUPPOSED
to be the noise waveform**, which the event data supports (the scoring family's `$13` is dist `$00`
noise vol 8, next to the pure-tone `$12`/`$1D`/`$1E`). Do not re-open it as a symptom.

What remains has the shape of a one-frame wrong `want_ptr` at onset. Two candidates are already
eliminated, so do not re-derive them:

* **Not `noiseTick` starvation.** It only refreshes `noise_buf`'s *contents*; Paula loops the
  buffer regardless, and waveform selection plus the pointer switch (`flush_paula`) are both VBI
  work. It also cannot touch the distortion buffers — `poly4_wave`/`poly5_wave` are immutable after
  init and `poly_dist_buf` is built in the VBI.
* **Not AUDF-written-before-AUDC against a stale AUDC shadow.** `sfx_voice_write_freq_ctrl $5673`
  writes AUDF and AUDC back-to-back in the same tick, so the AUDC write's `want_set` always
  overrides the AUDF write's before the frame's `flush_paula`.

⭐⭐ **What actually settled it was neither a capture nor a theory: the emulator was already
reporting the bug in its own log.** `.run/gdb-out.log` carried 59 `Audio 3 DMA wait hack DISABLED
… PER=9392` lines, each one FS-UAE saying "this channel was still mid-sample when you re-enabled
its DMA". **Read the emulator's log before building an instrument** — and read the emulator's
SOURCE (`tmp/fs-uae/audio.cpp`) before trusting a hardware-manual paraphrase: "AUDxPER takes effect
immediately" was the paraphrase that cost this bug, and `AUDxPER`'s six lines disprove it.

⚠ **Do not close this on a quiet session.** A play-through right after the `$40` fix showed no wrong
waveforms, but the chain resolver is a no-op unless AUDCTL sets bits 3-6 (nothing live does), and
the `$40` work changed how a voice is *rendered*, never which waveform it is *selected* to play
(`is_noise` is a function of AUDC alone). The saucer instance was already closed once on exactly
this evidence ("not reproduced since", 2026-08-13). What WOULD be real progress: the airlock
footsteps share **slot 8** with the noise events `$16`/`$17`/`$18`, so a mis-ordered priority-mixer
handover there is the first thing to look at — with a capture, not a theory.

## ⛔ CORRECTION: `a800dumps/*.bin` $D200-$D20F is STALE RAM, not POKEY (2026-08-18)

**Any statement derived from reading POKEY registers out of an `a800dumps` RAM image is void.**
The proof is one line and needs no emulator: the window is **byte-identical in every dump** —
`attract`, `standby`, `flight1/2/3`, `doors`, `tunnel`, `descent1/2`, `launch_*`, `plane_only`,
`music_playing_ram` all read `06 7C 3B 07 7C 8E 07 7B E3`, and two others read `E4 E4 … 00`. POKEY
registers cannot be the same during the attract tune and during flight. `tools/extract_a8s_ram.py`
extracts the 64 KB **RAM array**; on an XL/XE, $D000-$D7FF in that array is the RAM *under* the I/O
area, which this game never maps in — so it holds whatever the loader left there.

RAM inside the game's own blob ($3CDE-$B7FF) is real and every other derivation from these dumps
stands. Only the $D000-$D7FF window is junk. **To capture POKEY state, break on `rof_pokey_write`
(or read atari800's POKEY state, not its RAM array).**

### What this invalidates: AUDCTL bit7 is never set, so the poly9 path is DEAD CODE

The 2026-07-10 "distortion-0 bass rendered as white noise" fix was justified by *"the Standby tune's
`AUDCTL=$E3` has bit7 set → poly9"*. That `$E3` is the stale-RAM byte above. Two independent checks
say AUDCTL bit7 can never be set in 4.1:

* **Exhaustive binary scan.** `rof.xex` contains exactly **8** stores to $D208 (`8D 08 D2`). Their
  values are `$60` (×4), `$29`, `$04`, `$00`, and one register-sourced store at `$1A5A` whose
  register is `$00` (`LDA #$00` at `$1A4D`). None has bit7.
* **The one indirect path is out of range.** `sfx_voice_tick`'s fourth-voice write
  `bus_write(0xD1FF + sfx_voice_mute, audc + 2)` looks like it could scribble into AUDCTL, but
  `sfx_voice_mute = note >> 4` and a NOTE byte has bit7 clear, so the gate is 0..7 and the write
  spans **$D200-$D206 only**. It can never reach $D208.

So `build_poly_dist`'s `POLY_DIST_P9_*` arms are unreachable, and the earlier session's note that
"the Standby *theme* needs it" is wrong on both counts — the theme has **no distortion-$00/$80
voice at all** (voices 1-3 take `$A0`-`$A3` pure from `sfx_voice_tick`; AUDC4 is only `$22`
poly5tone or `$C2` ungated poly4, from table `$71C5`). ⚠ Do not delete the poly9 code on this note
alone — 5.0 or a later revision may set bit7 — but do not treat it as live either.

### What this means is actually wrong: poly17 is a 2-LEVEL square, we play full-range white noise

With bit7 never set, **all 9 noise-distortion events take the `noise_buf` fallback**: `$06 $09 $0E
$11 $13 $16 $17 $18 $19` — the engine drone, explosions, the descent beep, the slot-8 sounds that
share a channel with the airlock footsteps. Real POKEY's poly17 output is the same 2-level ±square
as poly4/poly9, gated by poly5; `noise_buf` is full-range uniform bytes. The code's own comment
concedes the trade ("smoother hiss vs POKEY's harsher buzz").

### ⛔ Rendering it faithfully was TRIED and is WORSE — reverted 2026-08-18, do not retry as-is

The obvious fix — render the 2-level poly5-gated poly17 square instead of uniform noise — was built
and proven correct (the LFSR verified against atari800 step for step, maximal length 131071, the
shared-buffer decimation argument checked for every reachable stride, the incremental refill proven
seamless). It still sounded **worse**, and the user's verdict was to revert. The reason is not a bug
in the rendering; it is a defect the old approximation was *hiding*:

* **Our buffer is 8192 samples; poly17's real period is 131071 underflows.** We loop **16× faster
  than the hardware**. Full-range uniform noise MASKS that repeat — one random-looking block sounds
  like the next. A 2-level ±square does not: the repeating pattern reads as a distinct pitched,
  rhythmic artifact. Making the waveform more faithful made a pre-existing inaudible defect
  audible.
* ✅ **`noiseTick` refilled per RENDERED frame, not per unit time — FIXED.** The refill is now paced
  off `g_vbiCount` (bumped in the real 50 Hz vblank ISR): 2 longwords per VBI, ~400 B/s, capped at
  8 VBIs of catch-up after a stall. It is the same rate on every CPU and in every scene, and it
  stays below the drone's ~626 B/s read rate so the write pointer never laps Paula. The bug cut
  both ways: a still-largely-transpiled scene (the Station) starved the refill and exposed the loop,
  while a 25 MHz+ accelerator churned the buffer several times past 50 Hz and made the mother-ship
  launch engine harsh — that accelerator report is what closed it. **The loop itself is untouched.**

**So the loop, not the waveform, is the thing to attack first.** Any retry must eliminate the
repeat, not just improve the timbre. A full-period buffer is 131071 bytes — impossible in chip RAM.
Continuous double-buffered regeneration at Paula's read rate is the real answer, and its cost scales
with that rate: the engine drone alone reads ~626 B/s (~22k cycles/s, ~0.3% CPU — affordable), but
the worst case is a short burst at the Paula floor, ~28.6 kB/s (~1M cycles/s, ~14% — not affordable
in flight). The fixed time base is now in place (above), so what a retry still has to add is the
*adaptive* half: drive the refill rate from the fastest active noise voice rather than the flat
2 longwords/VBI.

⭐ **The transferable lesson: a more faithful waveform can be worse when the surrounding
approximation was load-bearing.** Full-range noise was not just "less accurate timbre" — it was
also concealing a 16×-too-short DMA loop. Check what an approximation is *hiding* before replacing
it.
