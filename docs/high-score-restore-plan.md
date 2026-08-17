# Restoring the high-score table + initials entry — implementation plan

**Status: planned, not implemented (2026-08-17).** Read `docs/high-score-initials.md` FIRST — it
carries the finding this plan acts on: the screen and the entry work, but the save block they run
on came off DISK and both sector reads are three NOPs in `rof.xex`, so the whole feature has been
dead since the disk-to-executable conversion. Restoring it is **restoration, not a new feature**
(user, 2026-08-17): the code is all there and faithful; what is missing is the block, the screen's
display list, and keyboard delivery.

User decisions already taken (2026-08-17):
- **Default table content: the ORIGINAL bytes, extracted — see W3.** (This started as "synthesize
  now, swap real sectors in later"; the user produced a cartridge ROM and a disk image, and the real
  table came out of them, so the synthesis step is cancelled.)
- **Persistence: a file next to the executable, created on first save.** Missing file ⇒ the
  embedded default; read-only medium ⇒ keep it in RAM for the session.

## 1. What the block is (all measured, do not re-derive)

The two sectors `$02CE`/`$02CF` load to `$3700`/`$3780`, i.e. one 256-byte block `$3700-$37FF`.
`game_init_5D50 $5D50` builds the DCB; `$5DBC` is the per-sector op (`STY DCOMND`, `DUNIT=1`, then
the patched-out SIO call). Called from `game_entry $3CF3` with `LDY #$52` ('R', read) and from
`name_entry_loop $5C54` with `LDY #$57` ('W', write) — so the game already saves the table itself;
we only have to make its two calls do I/O again.

Screen geometry (DL `$5E2E`, installed by `name_entry_loop` via DLISTL/H `$D402/$D403`):

| region | bytes | what |
|---|---|---|
| `$3700-$3713` | 20 | ANTIC **mode 7** heading row (double height). `$3700` must be `$28` = `'H'`, colour 0 |
| `$3714-$3727` | 20 | mode 6 column-header row. `$3714` must be `$EE` = `'N'`, colour 3 |
| `$3728-$37C7` | 8×20 | mode 6 **table rows** — the 8 entries. Bases are exactly the position table `$5CFA` = `$28,$3C,$50,$64,$78,$8C,$A0,$B4` (offsets from `$3700`) |
| `$37C8-$37ED` | 38 | **OFF-SCREEN validity signature** = `mem[$7BDB..]` = `"Copyright (c) 1987 Lucasfilm Ltd. v4.1"`. `validate_save_state $5D0D` compares `$37C7+Y` for `Y=$26..1`, so `$37C7` itself (the last on-screen byte) is a don't-care |
| `$37EE` | 1 | `level_progress` — persisted too; `game_entry` overwrites it with `$10` when the block fails to validate |
| `$37F0-$37F6` | 7 | live entry state (cursor, dirty, `$37F4`, the 2 level digits) — RAM, not really block data |

Cell encoding, confirmed against the Title template `$5A9F`: `char = (byte & $3F) + $20` in ATASCII
(so `'A'` = `$21`, `'0'` = `$10`, space = `$00`), bits 7-6 select the colour register (the Title
uses 1 for its mode-7 row, 0 / 2 / 3 for its mode-6 rows).

Row format, from the insert code at `$5C12`:

```
offset  0 1 2 3 4 5 6 7   8   9 10  11   12 .. 19
        N A M E . . . .       L L        S  S  S  S  S  S  S  S
        name (8, cleared)     level      score (8 chars, copied from $36B7)
```

- `$5BB5-$5BE4`: rank search — compares `$370C+X` (the row's score field) against `$36B7+Y` (the
  current score *as display chars* on the Title's LAST SCORE line), 8 chars, big-endian. `C` clear
  ⇒ the player beats this row ⇒ insert here.
- `$5BF5-$5C10`: shift rows down by 20 bytes from the cursor to make room.
- `$5C12-$5C30`: write level digits into `+9/+10`, the score into `+12..+19`, blank the name
  `+0..+7`. Then the player types the name, `render_text_cell $5CA7` blitting each glyph into
  `$3700+X`.

Default block to synthesize (`rof_hiscore_default()`): heading `"HIGH SCORES"` colour 0, column
header starting `'N'` colour 3, 8 rows with a blank name/level and a `"00000000"` score field, and
**the signature copied at runtime from `mem[$7BDA]`** — never a second literal copy of the string,
so it cannot drift out of match. `$37EE = $10`.

## 2. Work items

### W1 — make the two sector calls do I/O again
- `tools/transpile.py` `PRE_INSN_HOOKS`: `0x5D86` and `0x5D9D` (the first NOP of each patched-out
  `JSR $5DBC`) → `rof_sio_block();`.
- New shared `src/rof_hiscore.c` (+ `.h`, added to both Makefiles' source lists next to
  `rof_boot.c`): `rof_sio_block()` reads the DCB out of `mem[]` — `DCOMND $0302`, `DUNIT $0301`,
  `DBUFLO/HI $0304/$0305`, `DAUX1/2 $030A/$030B` — and does 128 bytes for the addressed sector,
  `'R'` → read, `'W'`/`'P'` → write, then sets `DSTATS $0303 = 1`. Sectors other than `$02CE/$02CF`
  are a no-op with a failure status (nothing else in the binary asks for one).
- ⚠ `$5DBC` also stores the command through `$E8/$E9` and is itself reached only by those two calls;
  leave it transliterated and untouched — the hook replaces the missing SIO, not the DCB setup.

### W2 — persistence
- `platform_c.h`: `int platform_hiscore_read(uint16_t sector, uint8_t *dst128);` and
  `int platform_hiscore_write(uint16_t sector, const uint8_t *src128);` (1 = success).
- SDL host: the same two functions against a file in the working directory, so the feature is
  testable natively. Trivial — `PlatformSDL.cpp` already does file I/O.
- ⚠⚠ **The Amiga side is NOT trivial, and this is the one item whose design is still open.** Scouted
  facts: the port opens exactly two libraries — `SysBase` (`GCCRuntime.cpp:10-11`) and
  `graphics.library` (`PlatformAmiga.cpp:2858-2871`) — has **no dos.library, no `proto/dos.h`, no
  file I/O at all** (its only input is the incbin'd boot image), and builds `-nostdlib` with no `-l`
  at all (the proto headers inline the LVO jumps, so that part is not an obstacle). Worse, the write
  the game performs happens MID-RUN (`name_entry_loop $5C54`), and `run()` executes the game
  between `Forbid()`/`Permit()` with the OS display and the INTB_VERTB/PORTS/BLIT vectors hijacked
  (`PlatformAmiga.cpp:2897-3004`, which carries its own ⚠ about what may run there). A DOS call
  Wait()s on a packet — it breaks the Forbid and can raise a filesystem requester. So pick one:
  - **(a) RAM shadow + flush on exit.** The hook writes into a 256-byte shadow and sets a dirty
    flag; the actual file write happens in the dtor / after the view is restored. Safe, no Forbid
    violation, no requester. Cost: a hard reset or power-off loses the session's scores.
  - **(b) Mid-run DOS I/O**, bracketed to leave the Forbid and restore it. Matches the original's
    timing exactly. Needs care with our ISRs and is the riskiest.
  - **(c) WHDLoad-idiomatic**: `resload_SaveFile` (+ the installer's `#highs-file`, currently left
    at the default `highs` with nothing shipped — `docs/whdload-slave.md:278-283`), with (a) or (b)
    for the plain CLI run.
  Recommendation: **(a) now**, keeping the platform functions' shape so (b)/(c) can replace the
  backend later.
- ⚠ "Next to the executable" is not expressible as `PROGDIR:` under WHDLoad: `PROGDIR:` is set up by
  the Shell/Workbench, and the slave `LoadSeg()`s the game directly (`whdload/RoFSlave.s:143-168`),
  so it does not exist there. A plain relative filename resolves against the slave's
  `slv_CurrentDir`, which is `"data"` in the checked-in slave — and that is exactly the
  `slv_CurrentDir` vs `#sub-dir "data"` inconsistency already flagged in
  `docs/whdload-slave.md:259-265`. Decide that loose end before choosing the path, and write down
  where the file lands for each launch method (CLI run, `run.sh`, WHDLoad install).
- Also note `docs/whdload-slave.md:125`: dos/filesystem structures are an untuned `?` in the RAM
  budget — opening dos.library adds FASTMEMSIZE pressure that needs re-measuring.

### W3 — the default block — ✅ **the ORIGINAL bytes, already extracted**
No synthesis needed after all (2026-08-17): **`src/rof_hiscore_factory.h`** holds the real
factory-fresh table, 200 bytes for `$3700-$37C7`:

```
HIGHEST SCORING ACES        (mode 7, colour 0 — first byte $28, which is what validate_save_state wants)
NAME    LEVEL  SCORE        (colour 3 — first byte $EE, likewise)
FOX       6    35000        DRAGON    4    10000        JSL       2      421
LOREN     5    17000        PSL       3     2037        DL        1      327
                            GARY      3     1138
                            CROCK     3      525
```

Provenance, two independent dumps agreeing byte for byte: lifted from the **Atari 8-bit 64 KB
cartridge release (v5.0)** at cart-image offset `$6200` — a cart has no disk, so it carries the
table in ROM — and cross-checked against a **v4.1 disk image** whose factory block sits at ATR file
offset `$03218` (sector 101, with the signature immediately after it at `$032E0`, confirming the
layout). The heading row, the column-header row and every entry row that that disk's owner had not
played over (FOX / LOREN / DRAGON) are identical; the disk copy had COLBEAR/LARRY/LORD in it, so the
cart copy is the pristine one. The row format the header shows also confirms the offsets derived
from the insert code: name 0-7, level 9-10, score right-aligned 12-19.

⚠⚠ **Do not hardcode the signature, and do not take the disk block wholesale.** The two v4.1 builds
disagree: the disk block's own signature reads `"Copyright (c) 1985 Lucasfilm Ltd. v4.1"`, while our
`rof.xex` compares against `$7BDA` = `"XCopyright (c) 1987 Lucasfilm Ltd. v4.1"`. Copy 38 bytes from
`mem[$7BDA + 1]` at load time (the `+1` is why `$37C7` is the never-compared byte) so the block
always matches whatever binary it is running under. Then `$37EE` (`level_progress`) `= $10`.

So `rof_hiscore_default(uint8_t blk[256])` = `memcpy` the 200 bytes, copy the signature out of
`mem[]`, set `$37EE`, zero the rest.

### W4 — draw the entry screen (the visible half)
Today the game installs DL `$5E2E` and the Amiga ignores DLIST writes, so the Title card stays on
screen while the entry runs behind it — the user's screenshot: right palette, right jingle, stale
content. Needed:
- **The render signal must come from the DLIST write, not from VVBLKI.** `rsTitle` is
  `(vvblki == $53CC) && (mem[$365B] == $72)` (`RescueOnFractalus.cpp:5736-5741`) and the entry runs
  under that same `$53CC` VBI with `$365B` still `'R'` — so VVBLKI cannot tell the entry screen from
  the Title card. The discriminator is the display list itself (`$5E2E` vs `$5A82`): capture the
  `$D402/$D403` writes in `bus_write` into a global (the SDL backend already feeds its renderer from
  them) and derive `rsHiScore = (dlist == 0x5E2E)` next to the `rsTitle` line. `rsTitle` must then
  exclude it, or the Title branch will keep winning.
- The decode is nearly free — the Title decoder is already general over the colour bits and over
  mode 7 (`kTitleRowVdup[0] == 2`, i.e. mode 7 = write each glyph scanline twice; horizontal
  doubling to 16 px/char is the same for both modes). Three things are hardcoded and need
  parameterising (`RescueOnFractalus.cpp`):
  1. `decodeTitleCells(cellLo, cellHi)` at `:6234` — the base `0x365B` (`:6238`), and the file-scope
     `kTitleRowY[6]` / `kTitleRowVdup[6]` (`:6222-6223`). Target shape:
     `decodeTextCells(Bitmap*, uint16_t base, const short* rowY, const uint8_t* rowVdup, int lo, int hi)`.
     20 columns/row is baked into the walk (`:6241`, `:6265`) and the entry screen is also 20 wide.
  2. `decodeTitleScreen()` at `:6269` — hardcodes the clear size and `decodeTitleCells(0,119)`;
     needs `nCells = rows*20` (the entry screen is 10 rows = 200 cells).
  3. `rof_title_screen_dirty()` at `:98` — hardcodes `0x365B` and the `119` clamp. Add a sibling
     `rof_hiscore_screen_dirty()` with its own `g_hsCellLo/g_hsCellHi`, plus the
     `PlatformAmiga::` hook + `platform_c.h` + `platform_cbridge.cpp` entries mirroring
     `PlatformAmiga.cpp:1602` / `platform_c.h:85` / `platform_cbridge.cpp:67`.
     ⚠ The entry writes cells continuously (`render_text_cell` → `$3700+X`) and **nothing marks them
     dirty today**, so the new writer needs its own dirty call — cheapest at the `$3700` write site,
     the choke point, not at the callers (the `standby-level-scroll` lesson).
  4. ⚠ `kTitleRowY[]` for the new screen must be **derived from DL `$5E2E`'s blank-line counts**, not
     transcribed by eye — the existing table is literals and the entry DL has a different blank
     pattern (`70 70 70 70 70 70 | 47 mode7 | 70 70 70 70 | 06 | 70 | 06 | 20 06 ×7`).
- Copper: `TitleScreenCopperList` (`TitleScreenCopperList.cpp`) is a geometry-fixed full-screen 3bp
  playfield with `color00` black and four text pens — exactly what this screen needs, so either
  instantiate a second one over a second bitmap (**⚠ `Bitmap::allocate(320,216,3,true)` is ~25.9 KB
  of CHIP** — see `docs/asset-extraction.md` before spending it) or reuse the same bitmap + copper
  and re-decode on entry. Reuse is the better default; ⚠ a scene entry that rebuilds a SHARED
  copper list or bitmap must **blank first** (`amiga-copper-lessons`).
- Whichever way: a new `hiScoreCopperInstalled` flag has to be force-cleared at every other scene's
  install site, the way `titleScreenCopperInstalled` is at ~12 sites (`RescueOnFractalus.cpp:3913,
  4144, 4250, 4311, 4344, 4449, 4509, 4563, 4588, 4645, 4713, 5439`) — miss one and the screen
  silently stops re-decoding.
- ⚠ Colour registers: the entry palette comes from the same `mem[]` colour cells the entry/jingle
  code pokes (the user can already see them change), and `updateTitleScreenCopper()`
  (`RescueOnFractalus.cpp:4761-4785`) already reproduces the `$53ED` pen cycle from `$02C4+X` /
  `$0002` / `$0013`. Emit only the MOVEs the Atari actually makes (the CLAUDE.md DLI rule).

### W5 — deliver keystrokes outside flight
`check_collision_sync $5398` is the standby/title VBI's console sampler and uses the same
one-instruction CLI window as the flight VBI: `LDX #$FF / CLI ($539A) / SEI`, then `CPX #$80` (BREAK)
and `STX $0049` (the keycode). The flight window has a `PRE_INSN_HOOKS` entry at `$519c`
(`platform_flight_irq_key()`); **`$539A` has none, so no typed key has ever reached `$0049` outside
flight.** That is why the initials entry ignores the keyboard.
- Add the same hook at `0x539A`.
- ⚠ Check the BREAK interaction first: `$539C` routes `X == $80` into `game_loop_reset_trampoline
  $52BE`, which is exactly the faithful path the port already emulates via `rof_check_restart()`.
  Delivering `$80` here must not double-fire the restart (see the `break-restart` memory).
- Bonus, and a cheap end-to-end test of this item: the **AUTHOR easter egg**. `match_code_sequence
  $5B45` matches the six keycodes at `$5B3F` and then copies `$5B17` — `"BY D.FOX L.CARPENTER" /
  "C.KELLNER P.LANGSTON"` — over the Title's LAST/HIGH SCORE lines. It is unreachable today for the
  same missing-hook reason, and those two lines are already rendered by the Amiga, so it should
  light up with W5 alone, before W4 exists.

### W6 — the `$5E0E` VBI (a check, not code)
`game_init_5D50` installs VVBLKI `$5E0E` **only for the duration of the disk access** — it pushes
`$0222/$0223` at `$5D50-$5D57` and pops them back at `$5DA3-$5DA8`. Hand-disassembled, `$5E0E`
decrements CDTMV1 `$0218/$0219` and, when it expires, `JMP ($0226)`, else `JMP $E462` (XITVBV) —
and **nothing in the binary ever writes `$0218/$0219/$0226`** (raw-binary scan), so it is a quiet
no-op VBI held over the SIO call. With the hook doing the I/O synchronously the window is a few
microseconds. Nothing to port; just confirm `game_vbi_isr` runs NO body for an unrecognised VVBLKI
(in particular not the standby `$52D7` one).
`$5E0E` is absent from `disasm/listing.txt` (vector-only, so Ghidra never found it): add it to
`ghidra_scripts/entrypoints.csv` per the project convention.

### W7 — harness
- Keep `make NAME_ENTRY=1` for a fast death + a score that qualifies, but **drop its fake-block
  seeding** once W1-W3 land — the real block will validate on its own, and a flag that fakes the
  gate would hide a broken loader.
- `amiga/name_entry.gdb` + the `g_neEnter/g_nePass/g_neWait/g_neGlyph` counters already distinguish
  "called" from "past the gate"; add the block's validity bytes and the file-I/O outcome.
- End-to-end acceptance: die with a qualifying score → the table screen appears → type initials →
  they land in the right row → the block is written → **reboot and the score is still there**.

## 3. Order + risks

W1+W2+W3 first (the block validates, the entry runs for real, `enter=1 pass=1` without any fake
seeding), then W5 (typing works, easter egg as the smoke test), then W4 (the screen). W6 is a check,
not code. W4 is the largest and the only one with copper/timing risk.

Known traps: the ISR-cost rule (a heavy decode in the vblank ISR silently drops a displayed frame —
`standby-level-scroll` memory); a scene entry that rebuilds a SHARED copper list must blank first
(`amiga-copper-lessons`); and `make clean` before any `PROBES=1` build. The three spins in this path
already carry `SPINWAIT_HOOKS` yields (2026-08-17) — do not remove them: without a yield they
compile to `bra.s .` on the Amiga.
