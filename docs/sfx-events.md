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

## The saucer tone's noise waveform — CLOSED 2026-08-13 (user's call, not reproduced since)

Reported 2026-08-06 while ear-checking the SFX mixer asm twin: the high-pitched tone that plays
the whole time a flying saucer is present (that part is *correct*) **sometimes used the noise
waveform**. Never heard again in a week of flying; closed on the user's instruction. Reopen only
on a fresh report — these are the leads it was closed with, so nobody re-derives them.

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

**The fix is one observation: the period still loaded is ours to choose.** AUDxPER takes effect
immediately, so writing `kPaulaMinPer` (124) to each restarting channel *before* the off-window makes
2 sample periods ≈ 1.1 rasterlines, and the unchanged formula lands on its 7-line floor every time —
still ~12 sample periods of headroom. Per-restart wait **48.4 → 6.7 lines**; firing's ISR cost
**+96% → +39%**. `make FLUSHWAIT_OLD=1` restores the old sizing for an A/B; the probe rows are in
`amiga/fire_once.gdb`. Record: `docs/flight-perf-log.md` §25.1-25.2.

⚠ **The clamp was hiding a correctness bug, not just a cost.** At per > 12031 the old formula wanted
more than 110 lines and got clamped, so the shipping code was already under-waiting the rule it was
written to satisfy — for every slow outgoing note. Flooring the period is what makes the rule actually
hold.

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
