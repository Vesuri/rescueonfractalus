# `rof.rom` — the v5.0 cartridge, and every way it differs from the v4.1 `rof.xex`

`rof.rom` is a 64 KB Atari 8-bit **cartridge** release of the game, version **5.0**; `rof.xex` is the
**4.1** disk-derived executable we port. They are the same program: after undoing the relocation,
628 of 721 byte-level mismatch sites are mechanically explained as relocated address operands or one
systematic variable-block shift. What is left — the actual differences — is the small list in §4.

**5.0 is the CONSOLE (XEGS) version; 4.1 is the COMPUTER version, and the computer version is what we
implement.** Do not "upgrade" the port to 5.0 behaviour wholesale — most of what 5.0 changes exists
because the XE Game System has no keyboard and no disk. Three things make this ROM worth keeping
around anyway:

1. **Fixes.** 5.0 carries genuine corrections to 4.1 (§4.5, §4.6). Those are welcome.
2. **The boot chain.** 4.1's Logo → Station → Standby sequencing is entangled with the `.xex`
   loader — segments run via `INITAD` and are then overwritten by later segments. The cart has no
   loader, so its boot code spells the intended order out in one routine. **For that transition,
   `rof.rom` is the authoritative source, not `rof.xex`.** See §3.
3. **The factory high-score block**, which 5.0 ships in ROM because a cart has no disk to read it
   from (`docs/high-score-restore-plan.md`, `src/rof_hiscore_factory.h`).

---

## 1. Cartridge format

XEGS-style banking: 8 × 8 KB banks at `$8000–$9FFF`, selected by writing the bank number to `$D500`;
bank 7 is also hard-wired at `$A000–$BFFF`. Cart control block at the top of bank 7:

| addr | file off | value | meaning |
|---|---|---|---|
| `$BFFA` | `$FFFA` | `$A122` | start vector (`cart_boot`, §3) |
| `$BFFC` | `$FFFC` | `$00` | cart present |
| `$BFFD` | `$FFFD` | `$04` | boot-cartridge flag |
| `$BFFE` | `$FFFE` | `$A443` | init vector — a bare `RTS` |

Bank use, by file offset:

| bank | file off | contents |
|---|---|---|
| 0 | `$0000–$1FFF` | game image, copied to RAM `$4000–$5FFF` |
| 1 | `$2000–$3FFF` | game image, copied to RAM `$6000–$7FFF` |
| 2 | `$4000–$5FFF` | game image, **left paged in** at `$8000–$9FFF` |
| 3 | `$6000–$7FFF` | charset, the `$3700` save block, packed boot data, unpackers |
| 4 | `$8000–$93FF` | Logo + Station code and data (`$9400–$9FFF` = `$FF`) |
| 5–6 | `$A000–$DFFF` | all `$FF` — unused, ~16 KB free |
| 7 | `$E000–$FFFF` | game image + cart loader, **fixed** at `$A000–$BFFF` |

Only ~40 KB of the 64 KB is used.

## 2. The runtime memory map, and the address map

The cart copies two banks into RAM and runs the other two from ROM. Because ROM then owns
`$8000–$BFFF`, the RAM buffers and code that 4.1 keeps up there had to move, so the whole image is
**relocated**:

| cart runtime | comes from | 4.1 equivalent |
|---|---|---|
| `$0400–$05FF` | bank 3 `$8000`, copied | `$0400–$05FF` charset (5 glyphs differ, §4.3) |
| `$3700–$37FF` | bank 3 `$8200`, copied | the disk save block (§4.1) |
| `$3800–$3BFF` | RAM work buffers | `$BC00–$BFFF` |
| `$3C00–$3FFF` | bank 3 `$8300`, copied | `$3800–$3BFF` (byte-identical, 1024/1024) |
| `$4000–$9FFF` | banks 0,1 copied + bank 2 in place | `$5B4B–$B7FF` |
| `$A000–$BFFF` | bank 7 in place | `$3C00–$5B4A` |

So the two relocation deltas are, to a first approximation:

```
cart = xex + $6477     for xex $3C00 .. $5B4A      (into the fixed bank)
cart = xex - $1B4B     for xex $5B4B .. $B7FF      (down into RAM/bank 2)
```

Both drift by a few bytes across the image wherever code was inserted or removed; the drift steps
are exactly the change sites in §4. The 4.1 `$B800–$B8AB` segment — which sets `SDLST = $B832`,
pokes `CHBAS` and spins on `RTCLOK` — is the **disk loading screen** and has no counterpart in the
cart, as expected.

### Re-deriving the map

Nothing here needs to be taken on faith; the recipe is cheap:

1. Build the cart runtime image: bank 0 → `$4000`, bank 1 → `$6000`, bank 2 → `$8000`,
   bank 7 → `$A000`.
2. Build the 4.1 RAM image by walking `rof.xex`'s segment headers.
3. `difflib.SequenceMatcher(None, a, b, autojunk=False)` over the two ranges above, keeping matching
   blocks ≥ 6 bytes. Delta changes between consecutive blocks are the insertions/deletions.
4. Classify each gap: it is *not* a real difference if the differing bytes are a 16-bit operand whose
   4.1 value maps, through the map above, onto the cart value — or if it is a `$06xx` variable in the
   shifted window (§4.4). Everything unexplained is a real change; disassemble both sides.

⚠ Almost every remaining "data" difference is a **split lo/hi pointer table** being relocated, not a
tuning change. Check that hypothesis before believing a table was retuned: pair the lo and hi bytes
into addresses and run them through the map.

## 3. The authoritative Logo → Station → Standby chain

`cart_boot $A122` (bank 7). Bank numbers are the value written to `$D500`; `$A083` is a cart-only
helper that waits for vcount 30 and then points `VVBLKD` back at the OS `$E462`.

```
bank 4 ; JSR $8000      LOGO           (bank-local; 4.1 runs this via INITAD $5000)
         JSR $A083      wait + hand VBI deferred back to the OS
bank 3 ; JSR $955C      unpack packed boot data (bank 3 $8700 -> $3600)
bank 4 ; JSR $834C      STATION        (bank-local; ~4.1 station_init $195D)
         JSR $A083      wait + hand VBI deferred back to the OS
bank 3 ; JSR $960D      charset  bank3 $8000/$8100 -> $0400/$0500,
                        then bank3 $8300 +4 pages -> $3C00
         JSR $9601      save block  bank3 $8200 -> $3700          <-- the high-score table
bank 0 ; JSR $A0EC      copy 8 KB $8000 -> $4000
bank 1 ; JSR $A0EC      copy 8 KB $8000 -> $6000
bank 2 ; (left paged in at $8000-$9FFF for the rest of the run)
         clear $0600-$060C, $04, $37F4, $E4 ; $6D=4 ; $41=3
         SEI ; VVBLKI = $AAA1
         JSR audio_timer_setup      (4.1 $712D)
         JSR sfx_engine_reset       (4.1 $5433)
         $E2 = $64  (attract timer) ; clear $022F, $D01D, $02C8, $D00D+, $36CA+
         JSR wait_vcount_30         (4.1 $3C73)
         VVBLKD = $B85B
         JSR display_list_init      (4.1 $5D29)
         clear $02,$DC,$42,$C7, $0629+, $062C ; AUDCTL/SKCTL setup
         JSR loader_util            (4.1 $3C00)
         JSR init_terrain_col_tables(4.1 $7813)
         JSR game_init_77DF         (4.1 $77DF)
         JSR game_init_7588         (4.1 $7588)
         JSR $76CB                  (compute_stage_display_geometry region)
         if $060B: JSR standby_scoreboard_render   (4.1 $587B)
         cockpit draws: $4258, $43CB (Y=9), $4606, $4447 (A=8)
         ... -> STANDBY
```

⚠ The two bank-local scene entries are **not** yet mapped symbol-for-symbol onto 4.1. Bank 4's copy
of the Logo and Station code is relocated with a *drifting* delta and its data is repacked, so a
single-delta alignment fails (`bank4 $834C` best-matches 4.1 `$1957` at only 28/40 bytes). Deriving
that mapping properly needs a per-region alignment inside bank 4 and has not been done.

## 4. The differences

### 4.1 The high-score save block ships in ROM, and the disk code is gone

| | 4.1 | 5.0 |
|---|---|---|
| `game_init_5D50` (builds two SIO DCBs for sectors `$02CE`/`$02CF` into `$3700`/`$3780`, with both `JSR DSKINV` calls replaced by three `NOP`s) | present, 190 bytes, inert | **a single `RTS`** (cart `$4274`) |
| `$5DBC` (sets `DCOMND`, the write helper; also NOPped) | present | gone |
| the `$3700` block | never filled ⇒ the gate in `validate_save_state` always fails | **copied from bank 3 `$8200` at boot** ⇒ the gate passes |

`name_entry_loop`'s call site survives in 5.0 — it just calls the `RTS`. So 5.0 **shows the initials
entry and keeps a high-score table, but has no persistence at all**: the table is factory-fresh on
every power-up. `rof.rom` offset `$6200` is that 256-byte block (`$3700 = $28`, `$3714 = $EE`,
signature at `$37C7`); it is the source of `src/rof_hiscore_factory.h`.

### 4.2 The initials entry is rewritten for a joystick

`render_text_cell` grows ~133 bytes and becomes a joystick letter-wheel: `$D300` (PORTA) bits 0/1
step the character, `$D010` (TRIG0) commits, a blink counter in `$37F7` drives new cursor draw/erase
calls, and the character is clamped to `$61..$7C` with `$7D`/`$7E` as control markers. A new
name-entry-active flag `$0644` is set on entry and cleared on RETURN.

This is a console-input scheme and is **not** what the computer version does — it is recorded here,
deliberately not folded into `docs/high-score-restore-plan.md`.

### 4.3 Five charset glyphs changed — the ones the joystick wheel needs

Exactly the five whose inverse-video codes are `$7B–$7F`:

| char | 4.1 | 5.0 |
|---|---|---|
| `$3B` `;` | `[` | **blank** (the SPACE option) |
| `$3C` `<` | `\` | **left arrow ◄** (backspace) |
| `$3D` `=` | `]` | new symbol `00 62 95 85 B5 95 72` |
| `$3E` `>` | up-arrow | row 4 filled in (`C6`) |
| `$3F` `?` | underline in row 7 | underline in row 6 |

### 4.4 Console keys are translated into keyboard codes

A new 64-byte routine at cart `$42F7` reads `$D01F` (CONSOL), debounces it, and returns a synthesized
key code in X. It is called from the two places 4.1 samples the keyboard ISR with
`LDX #$FF / CLI / SEI`:

- the flight VBI, `vbi_handler_flight` (cart `$B61A`)
- the `$539A` keyboard hook in `launch_anim_dispatch` (cart `$B828`), guarded by
  `LDA $0644 / BNE` so it is skipped during initials entry

This is the XEGS being playable without a keyboard, and it is the reason for the one systematic
variable move in the build: **the `$06xx` window `$0610–$0644` shifted down by 3** (3 disk-related
bytes at `$0610–$0612` were deleted), freeing `$0642`/`$0643`/`$0644` at the top for the last CONSOL
value, the 7-frame debounce timer, and the name-entry flag. `$0600–$060F` and `$0645+` are unchanged.

`read_console_trig_delta` also changed semantics: 4.1 computes `(CONSOL & 1) − TRIG0`; 5.0 does
`if CONSOL == $06 return 0 else return TRIG0`.

### 4.5 Screen blanking on transitions — a candidate fix

5.0 adds `STA $D400` (DMACTL = 0) and `STA $D01A` (COLBK = 0) in three places 4.1 has nothing:

- the BREAK/reset trampoline (4.1 `$52BE`, cart `$B73D`)
- twice in `boot_standby_launch_driver` (4.1 `$5FC1` and `$5FD0`), which also gains
  `LDA $022F / ORA #$20 / STA $022F` to re-enable DL DMA afterwards

Reads as a fix for garbage or flicker across scene changes. Nothing here is console-specific, so it
is a legitimate fix to adopt — see the open items.

### 4.6 A terrain height clamp — the one real render change

In `terrain_subdivide_column`, immediately after the early-out test and before `$9F = $14`, 5.0
inserts 57 bytes that normalize four values into −32..+31:

```
LDA v / BMI neg / AND #$1F / JMP done / neg: ORA #$E0 / done: STA v
```

applied to `$83`, `$85`, `$25D2`, `$24E2` — the fractal midpoint-displacement corner heights and the
two terrain-field cells they were just compared against. ⚠ It **wraps, it does not saturate**: a
runaway height becomes a *low* column, not a correctly-tall one.

### 4.7 Smaller items

- **Extra gate in the flight VBI:** 5.0 inserts `LDA $284B / BEQ skip` in front of the branch at 4.1
  `$5178` (the `$36`/`$2838` landing path).
- **A 98-byte table dropped:** 4.1 `$B566–$B5C7`, a symmetric `2F 2E 2D 2C 2B…` ramp inside
  `terrain_column_rasterize`, is absent from 5.0. No pointer to it was found in 4.1, but indexed
  access from a nearby base cannot be ruled out.
- **Logo data is packed:** 4.1 carries `$0B00–$1AA6` expanded; the cart stores it packed in bank 3
  and expands it at boot via the unpacker at bank 3 `$955C`. Packaging only.
- **Zero page:** the Station code uses `$A1` where 4.1 uses `$A2`.
- **Version string:** `…Lucasfilm Ltd. v4.1` → `v5.0`, at 4.1 `$7BDA` / cart `$608F`. Both say
  **1987**. (The 4.1 *disk* image's own copy of the block says 1985 — see
  `docs/high-score-restore-plan.md`.)

## 5. Open items

Recorded, not scheduled.

- **Station audio.** The Station sequence sounds different between the two builds, and in particular
  **a wrong note plays on the Station → Standby transition**. 5.0 is the reference for this, since
  the transition itself is cart-authoritative (§3). Not investigated.
- **Is §4.6 the fix for the terrain spike?** A viewport-height column that gets flat-topped by the
  window edge has been seen in the Amiga build. Before adopting the clamp, settle two things:
  (a) does `atari800` on `rof.xex` show the same spike — if yes it is faithful and the clamp is a 5.0
  fix, if no the clamp is masking a port bug upstream; and (b) do `$83`/`$85`/`$25D2`/`$24E2` ever
  actually leave −32..+31 in flight? A probe on the range answers (b) far more cheaply than any
  render diff, and a "never" there means the clamp is dead code and cannot explain the artifact.
- **§4.5's blanking** is a low-risk fix to adopt; it has not been evaluated against the Amiga
  backend, which blanks differently (`docs/break-restart.md`'s ISR-vblank technique).
- **Bank 4's Logo/Station code is not symbol-mapped onto 4.1** (§3).
