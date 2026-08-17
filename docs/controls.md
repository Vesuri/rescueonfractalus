# Controls (Atari manual → Amiga port)

> Split out of `CLAUDE.md` 2026-08-10 to keep the always-loaded file small.
> **Read this when touching input, the keyboard/console path, Standby SELECT, or BREAK/restart.**

The Atari controls (game manual) and the Amiga key chosen for each in the port. Two distinct
input paths: **console/joystick** (PIA PORTA `$D300` directional bits + TRIG0 `$D010` fire +
CONSOL `$D01F` START/SELECT/OPTION, all active-low, polled), and **in-flight keyboard commands**
(POKEY keyboard IRQ `irq_handler $462A` → KBCODE → `event_sequence_dispatcher $4644`, delivered on
the Amiga via the CIA-A keyboard ISR; the transpiler `PRE_INSN_HOOKS[$519c]` feeds the keycode into
the flight VBI's CLI window). Faithful 1:1 — the dispatcher logic is the Atari binary's.

⚠ **There are TWO keyboard windows, and both must consume.** The game runs with IRQs masked
(`$3D27 SEI`) and POKEY's keyboard IRQ armed (`IRQEN=$C0` at `$3DA1`), so a keypress stays PENDING
until a one-instruction `CLI`/`SEI` gap opens and `irq_handler $462A` drops `KBCODE&$3F` (or `$80`
for BREAK) into X. There are exactly two such gaps: **`$519c`** in the flight VBI, and **`$539a`**
inside the `$5398` console/attract poll that BOTH standby-family VBI bodies run every frame
(`$52D7` at `$533C`, the `$53CC` card at `$5400`). `$5398` does NOT route the code to
`event_sequence_dispatcher` — out of flight a command key only resets the attract timeout — but it
DOES consume it. Porting `$5398` without its window (the state before 2026-08-08) left a keycode
pressed outside flight latched across the whole launch cinematic for the first `$519c` window to
dispatch: pressing ESC on Standby then froze the game (`$0043`) the instant flight began.
`rof_attract_poll_key()` deliberately leaves a pending `$80` alone, because out of flight the
restart must longjmp from main-loop context (`rof_check_restart`).

| Atari control | Action | Atari KBCODE | Amiga key (rawkey) | Path |
|---|---|---|---|---|
| START | Start the game | — (CONSOL) | **F1 ($50)** | CONSOL $D01F bit0 |
| BREAK | Restart (score lost, highs kept) → `game_loop_reset` | $80 | **Help ($5F)** | kbd cmd $519c |
| ESC | Freeze/pause mission (toggle) | $1c | Esc ($45) | kbd cmd |
| CURSOR RIGHT | Increase Thrust (Y4) | $07 (Ctrl-`*`, masked) | **`=`/`+` ($0C)** | kbd cmd |
| CURSOR LEFT | Decrease Thrust (Y5) | $06 (Ctrl-`+`, masked) | **`-` ($0B)** | kbd cmd |
| L | Land | $00 | L ($28) | kbd cmd |
| S | Systems | $3e | S ($21) | kbd cmd |
| A | Air Lock | $3f | A ($20) | kbd cmd |
| B | Boosters | $15 | B ($35) | kbd cmd |
| (joystick) | Steer — forward/back = dive/climb, left/right = bank | — | **real joystick in port 1**, or **arrow keys** ($4C/$4D/$4F/$4E) — the keys **in flight only** | PORTA $D300 bits 0/1/2/3 = up/down/left/right |
| (trigger) | Fire (Launch AMB Torpedo) | — | **joystick button 1**, or **Left Shift ($60)** — the key **in flight only** | TRIG0 $D010 |
| (2nd trigger) | **Land / Launch the ship** (manual: "the Apple's second fire button will land or launch") | $00 (as L) | **joystick button 2** | POTINP bit 14 → the L command key, kbd cmd $519c |
| Joystick up / down | Starting level up / down (inside the level-selector card only) | — | **joystick forward/back**, or **arrow up/down** ($4C/$4D) | PORTA $D300 bits 0/1 |
| (trigger, pre-game) | Start the game | — | **joystick button 1** | TRIG0, via `read_console_trig_delta $5A78` = `(CONSOL & $01) - TRIG0` |
| SELECT | Open the level-selector card (initial Standby) / cycle level in place (post-mother-ship Standby) | — | **F2 ($51)** | CONSOL $D01F bit1 |
| SHIFT+SELECT | *Lower* the starting level instead of raising it (selector card) | — | **either Shift ($60/$61) + F2** | SKSTAT $D20F bit3 |
| OPTION | Demo (DEMO DROID) | — | **F3 ($52)** | CONSOL $D01F bit2 |
| SYSTEM RESET | Reboot disk | — | not mapped (hardware reset, not application-controlled) | — |

Thrust sits on the two keys immediately **right of `0`** (`-` and `=`/`+`) because those are the
physical positions carrying the Atari 800's `< -` / `> =` legends, i.e. the keys the original uses
for thrust down/up. The path is raw-keycode based with no shift decoding, so `=` fires thrust-up
unshifted as well as as `+`.

Implementation: `PlatformAmiga.cpp` `kFlightKeys` (one-shot command keycodes), `s_portaState`/
`s_trig0State` (held joystick/fire level read by `hwRead`), `s_consolState` (CONSOL bits 0/1/2 =
START/SELECT/OPTION, driven bitwise from F1/F2/F3 edges in `keyboardHandler`). SDL build
delivers none of these (`flightIrqKey`→$FF, PORTA neutral) — Amiga-only for now.

## The real Amiga joystick (port 1) — `pollJoystick()`

The manual's "plug your joystick into the second port" lands exactly right on the Amiga: port 0 is
the mouse, port 1 (the second connector) is the joystick — and port 1 is also what the 6502 binary
polls. **`$D011`/TRIG1 and PORTA bits 4-7 are never read anywhere in the game**, so there is no
second-stick path and nothing to choose.

- Sampled **once per vblank** from `vbiHandler`, just ahead of `game_vbi_isr` — the rate the Atari's
  own VBI polled at, and the rate an edge-triggered button needs (the main loop runs well under
  50 Hz in flight and would drop short presses). Four register reads.
- ⚠ **The keyboard's stick emulation (arrow keys + Left Shift) is read in FLIGHT ONLY; the real
  stick is live in every scene.** PORTA/TRIG0 are polled outside flight too — Standby's
  `read_console_trig_delta $5A78` launches the game on the trigger, and the selector card cycles the
  starting level on up/down — so an ungated keyboard put Left Shift on "launch" and the arrows on
  "change level" in the selector. `hwRead` gates the keyboard levels on `keyboardStickLive()`
  (VVBLKI == `$4FF5`, the flight VBI); the gate is on the READ, not on the key edges, so a key still
  held across a scene boundary cannot strand an active-low bit in the new scene. **Measured A/B**
  (Left Shift + arrow-up held from vbi 150, `NOAUTO=1`): ungated → VVBLKI `$4FF5` with 7 flight
  iterations by vbi 1975, gated → still `$52D7` with 0. Consequences, both accepted:
  Left Shift no longer skips the Logo/Station cinematics (F1, any key, the timeout and the real
  stick's fire still do), and in the selector card the level is cycled by F2 or a real stick, not by
  the arrow keys.
- The `FORCE_SELECT` probe drives its synthetic joystick-up through a THIRD level, `s_probePorta`,
  ANDed into PORTA ungated: it tests the selector card, which is exactly what the gate shuts out.
  Every other `FORCE_*` harness injects on the real-stick side (`s_joyPorta`/`s_joyTrig0`) and is
  unaffected — but `pollJoystick()` overwrites those every vblank *after* the probe blocks run.
- **The stick drives the SAME PORTA bits as the arrow keys**, so stick and keyboard are
  indistinguishable downstream. Kept in separate `s_joyPorta`/`s_joyTrig0` levels and merged in
  `hwRead` by **AND** — both are active-low, so either source pressing wins, and a centred or
  unplugged port ($FF/$01) leaves keyboard behaviour bit-for-bit unchanged. **The keyboard mappings
  are not a fallback to be removed.**
- ⚠ **`JOY1DAT` is a pair of quadrature counters, not four direction bits**, and the layout is
  crosswise: `forward = bit8^bit9`, `left = bit9`, `back = bit0^bit1`, `right = bit1` (Hardware
  Reference Manual, "Reading the joystick"). A **mouse** plugged into port 1 therefore decodes as
  directions — that is the realistic failure mode, not a wrong bit.
- Button 1 = **CIA-A PRA bit 7** (`CIAF_GAMEPORT1`), active-low; bit 6 is port 0, which
  `AmigaHardware::isLeftMouseButtonPressed` already uses. Button 2 = **POTINP bit 14** (DATRY, port 1
  pin 9), active-low — the same technique `isRightMouseButtonPressed` uses on port 0's bit 10, so no
  `POTGO` setup is introduced. Measured idle `POTINP` = `$5500`, i.e. all four pot pins read high.
- Button 2 is an **EDGE**, not a level: `event_sequence_dispatcher` takes a one-shot event id, so a
  held button would re-issue Land every frame.
- "Fire button starts the game" and "push forward/pull back = level up/down" needed **no wiring** —
  they are `read_console_trig_delta $5A78` (`(CONSOL & $01) - TRIG0`) and the `& 0x01` PORTA read in
  `boot_standby_launch_driver` respectively, both already faithful.
- **Verified headlessly with nothing plugged in** (`make PROBES=1` + `amiga/joy_probe.gdb`): 2722
  polls over 2722 vblanks, no direction bit ever low, no Land injection, no fire. That is the
  regression that mattered; **the probe proves quietness, never that a real stick steers correctly**.
- **A real stick IS now confirmed** (user, 2026-08-14): the axes behave as an aircraft — stick back
  raises the nose, stick forward lowers it, matching the manual's Forward = Dive / Back = Climb.

## SHIFT — SKSTAT $D20F bit3, the level-selector's decrement

`standby_level_select_loop $5978` picks its direction at `$59be`, and SHIFT is half of that decision:

```
LDA $D300 / AND #$02   ; joystick DOWN?   -> decrement
LDA $D20F / AND #$08   ; else SHIFT held? -> decrement   (ACTIVE-LOW: bit3 clear = held)
                       ; else             -> increment
```

`hwRead` used to hardwire SKSTAT to a flat idle `$FF`, so bit3 always read "not pressed" and
**Shift+F2 raised the level like a bare F2**. (That flat `$FF` was itself a fix: returning the POKEY
write-shadow read as permanently shift-HELD, so every press decremented.) Both Amiga shift keys now
drive `s_shiftMask` — one bit each, so releasing one while the other is held stays held — and
`hwRead($D20F)` clears bit3 from it. Ungated, unlike the stick emulation: SHIFT is a real key the
Atari reads in any scene, `$59c5` is the binary's ONLY `$D20F` read, and in flight Left Shift doubles
as the trigger with no reader to conflict.

⚠ **Joystick-down decrements on the same branch**, which is why this was invisible with a stick
plugged in. Since the arrow keys are flight-only, arrow-down does NOT decrement here — on the
keyboard the level comes down with Shift+F2.

**Measured A/B** (`FORCE_SELECT` pulsing joystick-up on the card): the level's own SEQUENCE is the
signal, because both branches wrap — `DEC`, and on 0 reload the max; `INC`, and at the cap reset to
1 — so a single end value cannot show direction. bit3=1 gave `4 5 6 7 8 9 10 11`; bit3=0 gave
`4 5 4 3 2 1 16 15`.

## Skipping the two boot cinematics — START *or* fire (divergence, ON by default)

**User decision, 2026-08-14 (quality of life, knowingly unfaithful).** The Atari runs both boot
cinematics to their own end; the Logo's `$5000` is an unconditional blocking routine with no exit
check anywhere. Both are now skippable:

| scene | skips on | mechanism |
|---|---|---|
| 1 Logo | START (F1) **or** the REAL joystick's fire (Left Shift is flight-only, see above) | `logo_aborted()` in `src/rof_logo.c` — reads CONSOL bit0 and TRIG0 directly, so it is platform-neutral (SDL serves TRIG0 from SPACE/Z) |
| 2 Station | START (F1), any keyboard key, fire, or its RTCLOK timeout | START/key/timeout are the **faithful** checks at `$1a01`; fire is added by `PlatformAmiga::hwRead` |

- ⚠ **`make LOGO_START=0` restores the faithful unconditional Logo**, and deliberately disables
  *both* of its exits, not just START. (It was the opposite until 2026-08-14 — opt-in via
  `LOGO_START=1`, faithfulness winning. The flag is a `-D` the Makefile does not track: **`make
  clean` when toggling it.**)
- ⚠ **The Station's exit check is TRANSLITERATED code** (`$1a01`: RTCLOK `$0013 >= 4`, any key
  `$02FC != $FF`, or `CONSOL == $06`) and **never reads TRIG0** — so it cannot be hand-edited to add
  fire. Instead `hwRead($D01F)` reports **fire AS START**, scoped to `VVBLKI == $1B30`, the Station's
  own VBI vector (installed by `station_init` at `$198d`: `LDA #$30 / LDX #$1B`). That scoping is
  what keeps the fold invisible everywhere else — the Logo has its own TRIG0 check, and Standby
  already responds to TRIG0 through `read_console_trig_delta $5A78`.
- A **held** fire therefore runs Logo → Station → Standby → launch, since `$5A78` reads TRIG0 as a
  level. Each *tap* advances one step. Both are wanted.
- **Verified headlessly**, `make PROBES=1 SKIPBOOT=0 FORCE_BOOT_FIRE=1` + `amiga/boot_fire.gdb`
  (fire held vbi 100-160): the Logo handed off at **vbi 107** and the Station at **vbi 178**, against
  a measured no-press control of **~280** and **~1300+**. ⚠ `SKIPBOOT=0` is required — `PROBES`
  implies `SKIPBOOT`, which skips the very scenes under test.
- ⚠ **Inject the button from C, not gdb.** A `set var` on the force flag from the gdb script did not
  stick: the FS-UAE stub serves memory *reads* but drops writes (measured 2026-08-14). That is why
  every `FORCE_*` harness in `PlatformAmiga.cpp` drives its input from C.

## Standby SELECT — two contexts

Gated on the mother-ship flag `$003A` (measured 2026-08-03 via FS-UAE `FORCE_SELECT`/`FORCE_RETURN`
probes; all logic is the faithful transpiled binary):

- **Initial Standby (`$003A==0`):** SELECT (F2) opens the *separate* level-selector card
  (scene 3b) — the binary jumps to `standby_scoreboard_render $587B` → installs the `$53CC` VBI,
  writes the title text into `$365B` (`=$72`), so the Amiga's `rsTitle` renderer engages. Inside
  the card, **joystick up/down** cycles `level_stage $006D`, re-rendered via the
  `platform_title_screen_dirty($3694,2)` hook. **NOTE (measured, clean test):** joystick-up from
  the cockpit ALSO opens the card — the standby dispatch (`rof_native.c` L_6324) reads joystick-up
  and SELECT into the SAME branch, so this is FAITHFUL binary behaviour (the game manual agrees:
  "joystick up/down → starting level, Standby"). An earlier "joystick-up does nothing" note was a
  test-harness artifact and is retracted.
- **Post-mother-ship Standby (`$003A==$FF`, after a boost/return cinematic):** SELECT cycles the
  level *in place* in the cockpit (VVBLKI stays `$52D7`, no separate card) — the door field `$2000`
  is rebuilt with the new LEVEL digits (`$622d` door-scroll if `level_stage < max $0609`, else
  `intro_screen_build_seq $65a8` fade-rebuild wrap). This in-place rebuild does NOT re-enter
  `boot_standby_launch_driver`, so it can't rely on the driver-entry `g_doorFieldReady` reset —
  instead the driver's SELECT dispatch clears `g_doorFieldReady` at **L_6332** (before the rebuild)
  and re-latches it at the **L_62f6** idle-loop top (after both rebuild branches converge), and that
  0→1 edge arms the Amiga `terrainDirty` door re-decode (RescueOnFractalus.cpp `deriveRenderSignals`)
  with the finished field — without which the new level digits never appeared (fixed 2026-08-04,
  commit 86e07f7). Joystick up/down does NOT cycle here. This is the
  `rsBoostReturn = standbyVbi && mem[$003A]==$FF` tail. Repro headlessly with `FORCE_RETURN=1` (the
  genuine boot→flight→boosters→standby path; the standby dispatch is measured by the
  `g_ipDispatch/g_ipInPlace/g_ipDoorScroll/g_ipIntroWrap` `ROF_FLIGHT_PROBE` counters).

## BREAK (Help) — restart via `__builtin_setjmp`/`longjmp` (like the quit path)

`game_loop_reset` restarts through a 6502 RTS stack trick C can't reproduce, and it fires from the
VBI ISR where longjmp is unsafe.  Instead: the trampoline ($52BE) leaves its observable side-effect
VVBLKI=`$52B4` in mem[]; the MAIN-loop pump gate **`rof_check_restart()`** (called from `renderFrame`,
`pollEvents`, AND `renderFlightDirect` — the flight loop busy-waits in the latter and never reaches
renderFrame, so a BREAK pressed mid-flight would otherwise stay stuck at `$52B4` = the black+brown
viewport) detects it and longjmps `g_restartJmp`.  run() then replicates the faithful `$3D1F→$3D48`
init (skips the `$3D0C` clear, so the **high score `$0605-$0608` is kept**) and calls `game_main_loop`
→ the `$53CC` level-selector card + standby music (measured on the Atari: BREAK from ANY scene → that
card).  SYSTEM RESET is a hardware reset, not an application key — deliberately not mapped.
