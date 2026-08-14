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
| (joystick) | Steer — forward/back = dive/climb, left/right = bank | — | **real joystick in port 1**, or **arrow keys** ($4C/$4D/$4F/$4E) | PORTA $D300 bits 0/1/2/3 = up/down/left/right |
| (trigger) | Fire (Launch AMB Torpedo) | — | **joystick button 1**, or **Left Shift ($60)** | TRIG0 $D010 |
| (2nd trigger) | **Land / Launch the ship** (manual: "the Apple's second fire button will land or launch") | $00 (as L) | **joystick button 2** | POTINP bit 14 → the L command key, kbd cmd $519c |
| Joystick up / down | Starting level up / down (inside the level-selector card only) | — | **joystick forward/back**, or **arrow up/down** ($4C/$4D) | PORTA $D300 bits 0/1 |
| (trigger, pre-game) | Start the game | — | **joystick button 1** | TRIG0, via `read_console_trig_delta $5A78` = `(CONSOL & $01) - TRIG0` |
| SELECT | Open the level-selector card (initial Standby) / cycle level in place (post-mother-ship Standby) | — | **F2 ($51)** | CONSOL $D01F bit1 |
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
  regression that mattered; **the probe proves quietness, never that a real stick steers correctly** —
  that needs a physical joystick (or an FS-UAE-mapped pad) and a human at the controls.

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
