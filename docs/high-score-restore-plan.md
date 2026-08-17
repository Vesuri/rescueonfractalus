# Restoring the high-score table + initials entry — implementation plan

**Status: planned, not implemented (2026-08-17).** Read `docs/high-score-initials.md` FIRST — it
carries the finding this plan acts on: the screen and the entry work, but the save block they run
on came off DISK and both sector reads are three NOPs in `rof.xex`, so the whole feature has been
dead since the disk-to-executable conversion. Restoring it is **restoration, not a new feature**
(user, 2026-08-17): the code is all there and faithful; what is missing is the block, the screen's
display list, and keyboard delivery.

User decisions already taken (2026-08-17):
- **Default table content: synthesize now, swap in real sectors later.** Keep it in ONE embeddable
  256-byte block so a genuine dump of sectors 718/719 replaces it with no code changes.
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
- Amiga: `PROGDIR:RoF.hi`, 256 bytes, offset `(sector - 0x02CE) * 128`; dos.library
  Open/Seek/Read/Write/Close. Create on first write. A missing/short file ⇒ read returns 0 ⇒ W3's
  default; a failed write is swallowed (read-only medium = the table lives for the session only).
  ⚠ Check whether dos.library is already open at startup (see W4/scout notes) — if the port only
  ever consumed the embedded xex image, this is the first real file I/O and needs library init +
  teardown on the quit path.
- ⚠ WHDLoad: `slv_CurrentDir` is empty while the installer uses `#sub-dir "data"`
  (`docs/whdload-slave.md`, open loose end). `PROGDIR:` sidesteps the current directory, but the
  install must still be writable — verify on the WHDLoad install, not only from the CLI.
- SDL host: the same two functions against a file in the working directory, so the feature is
  testable natively.

### W3 — the default block
- `rof_hiscore_default(uint8_t blk[256])` in `src/rof_hiscore.c`, built from a small
  `{text, colour}` row table + an ATASCII→internal conversion, so editing it is editing text.
- Used when the file is absent. Structured so a real sector dump can be dropped in as a plain
  256-byte array instead — keep the "where the bytes come from" decision in one function.

### W4 — draw the entry screen (the visible half)
Today the game installs DL `$5E2E` and the Amiga ignores DLIST writes, so the Title card stays on
screen while the entry runs behind it — the user's screenshot: right palette, right jingle, stale
content. Needed:
- A render signal for "the entry screen is up". The honest source is the DLIST write itself:
  capture `$D402/$D403` in `bus_write` into a global (the SDL backend already feeds its renderer
  from those writes) and derive `rsInitials = (dlist == 0x5E2E)` in `deriveRenderSignals`.
- Decode + copper for a mode-7 row at `$3700` plus nine mode-6 rows at `$3714 + n*20`. This is the
  *same shape* as the Title screen (mode-7 row + five mode-6 rows from `$365B`), so the intended
  route is to give the existing Title cell decoder a base address + row count instead of writing a
  second decoder, and to reuse/clone its copper list with the entry palette.
- The entry writes cells continuously (`render_text_cell`), so the dirty-cell path has to cover the
  `$3700` window too — the Title dirty range is expressed in `$365B`-relative cells today.
- ⚠ Colour registers: the entry screen's palette comes from the same `mem[]` colour cells the
  jingle/entry code pokes (the user already sees them change), so the copper must MOVE exactly the
  registers the Atari DL/DLI writes and no others (the CLAUDE.md rule).

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

### W6 — the entry's own VBI
`name_entry_loop` runs under VVBLKI `$5E0E` (set by `game_init_5D50`). Hand-disassembled: it
decrements CDTMV1 `$0218/$0219` and, when it expires, `JMP ($0226)`, else `JMP $E462` (XITVBV).
**Nothing in the binary ever writes `$0218/$0219/$0226`** (raw-binary scan), so it is a no-op VBI.
So there is nothing to port — but `game_vbi_isr` must be checked to confirm an unrecognised VVBLKI
runs NO body (in particular not the standby `$52D7` one) while `$5E0E` is installed.
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
