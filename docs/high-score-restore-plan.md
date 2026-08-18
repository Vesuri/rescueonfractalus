# Restoring the high-score table + initials entry — implementation record

**Status: IMPLEMENTED (2026-08-18).** Read `docs/high-score-initials.md` first — it carries the
finding this work acts on: the screen and the entry are all there and faithful, but the save block
they run on came off DISK and both sector reads are three NOPs in `rof.xex`, so the whole feature
has been dead since the disk-to-executable conversion. Restoring it was **restoration, not a new
feature** (user, 2026-08-17): what was missing was the block, the screen's display list, and
keyboard delivery.

Sections 1 and 2 below are the design as built (W1-W7); §4 records where the plan and the code
diverged, and what is still open.

User decisions taken:
- **Default table content: the ORIGINAL bytes, extracted — see W3.**
- **Persistence: a file next to the executable, created on first save.** Missing file ⇒ the
  embedded default; read-only medium ⇒ keep it in RAM for the session.
- **Persistence timing (user, 2026-08-18): read at startup before multitasking and the interrupts
  are touched, write on exit after they are given back.** The WHDLoad half of that instruction —
  hook the save the game itself performs when the initials are entered — turned out not to be
  reachable; see §4.

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

The factory contents of all of that are in `src/rof_hiscore_factory.h` — see W3.

## 2. Work items

### W1 — make the two sector calls do I/O again ✅
- `tools/transpile.py` `PRE_INSN_HOOKS`: `0x5D86` and `0x5D9D` (the first NOP of each patched-out
  `JSR $5DBC`) → `ROF_SIO_HOOK`.
- ⚠ `$5DBC` is unreachable in `rof.xex`, so **Ghidra never disassembled it and it is not in
  `listing.txt`** — there is no transliteration of it to call. The hook therefore reproduces its
  four DCB stores itself (`STY $E9` / `#$03 → $E8` / `DCOMND` / `DUNIT=1`) and then calls
  `rof_sio_block()`. `cpu.Y` carries the command and survives the `JSR $3C73` at `$5D58` (that
  routine touches only A). `$5DBC` and `$5E0E` are recorded in `ghidra_scripts/entrypoints.csv`
  so a future Ghidra run picks them up.
- `src/rof_hiscore.c` (+ `.h`, in both Makefiles next to `rof_boot.c`): `rof_sio_block()` reads the
  DCB out of `mem[]` — `DCOMND $0302`, `DUNIT $0301`, `DBUFLO/HI $0304/$0305`, `DAUX1/2
  $030A/$030B` — moves 128 bytes for the addressed sector, `'R'` → read, `'W'`/`'P'` → write, and
  sets `DSTATS $0303`. Any other sector or command gets `$8A` (device timeout) and transfers
  nothing.

### W2 — persistence ✅
- `platform_c.h` / `Platform.h`: `hiscoreLoad(uint8_t* blk)` and `hiscoreSave(const uint8_t* blk)`,
  bridged as `platform_hiscore_load/save`. `hiscoreSave` returns *false* to DECLINE, which is a
  first-class answer, not a failure: `rof_hiscore.c` then keeps the block and its dirty flag and
  `rof_hiscore_flush()` retries later.
- SDL host: `RoF.hi` in the working directory, real file I/O in both directions at the moment the
  game asks, so the feature is testable natively.
- Amiga: **option (a), RAM shadow + flush on exit**, with the read moved to the front:
  `PlatformAmiga`'s ctor opens `dos.library` (the port's only OS file I/O, and its third library
  after `SysBase`/`graphics.library`) and reads `RoF.hi` *before* `run()` takes over anything; the
  mid-run save declines; `run()` sets `s_hiscoreWritable` and calls `rof_hiscore_flush()` after
  `Permit()` + `LoadView(savedView)`. A hard reset still loses the session's scores.
- ⚠ **`resload_SaveFile` is NOT reachable from the game**, so there is no separate WHDLoad arm.
  The slave is a `kick13.s` kickemu that `LoadSeg()`s the game as an ordinary CLI program
  (`whdload/RoFSlave.s:143-168`) — `resload` lives in the slave and the game never sees the
  pointer. `dos.library` is fully up under that kickemu, so the same path serves both launch
  methods.
- The filename is deliberately RELATIVE. `PROGDIR:` does not exist under WHDLoad (the
  Shell/Workbench sets it up; the slave's `LoadSeg` does not), so a plain relative name is the one
  spelling that resolves sensibly everywhere: the CLI's current directory for a Shell run,
  `slv_CurrentDir` under WHDLoad. Where that lands is still the unresolved `slv_CurrentDir` vs
  `#sub-dir "data"` loose end — `docs/whdload-slave.md:259-265`.
- Still untested: the FASTMEMSIZE cost of holding `dos.library` open (`docs/whdload-slave.md:125`
  lists dos/filesystem structures as an untuned `?`).

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

`rof_hiscore_init()` does exactly that: zero, `memcpy` the 200 bytes, copy 38 bytes from
`mem[$7BDA + 1]`, set `$37EE = $10`, then let `platform_hiscore_load()` replace the whole thing if
a saved block exists. Called from both backends immediately before `game_entry()` — it must follow
the image load, since the signature comes out of `mem[]`.

### W4 — draw the entry screen ✅
- **The render signal comes from the DLIST write, as planned.** `bus.h` latches `$D402/$D403` into
  `g_atariDlist` on the Amiga (the writes were dropped there before); `deriveRenderSignals` sets
  `rsHiScore = (vvblki == $53CC) && (g_atariDlist == $5E2E)` and `rsTitle` excludes it.
  ⚠ The `$53CC` half is NOT redundant: `name_entry_loop` has exits at `$5BA2` and `$5C8B/$5C92`
  that return **without** calling `display_list_init $5D29`, so the latch can still read `$5E2E`
  after the screen is gone. On the Atari that is harmless because the next scene installs its own
  DL; the VBI vector is the equivalent signal here.
- Decoder: `decodeTitleCells` was split into `decodeTextCells(base, rowY, rowVdup, lo, hi)` —
  20 columns, charset `$0400`, mode 6/7 double width and `pen = colour-select + 1` are common to
  both screens; only the screen RAM and the row geometry differ. `decodeTitleCells` is now a
  one-line wrapper.
- Row geometry, DERIVED from `$5E2E`'s own blank-line counts (`6x $70`, mode 7 = 16 scanlines,
  `4x $70`, mode 6, `$70`, mode 6, then `7x ($20, mode 6)`):
  `kHiScoreRowY[10] = { 48, 96, 112, 123, 134, 145, 156, 167, 178, 189 }`, `vdup = { 2, 1 x 9 }`.
  The same derivation reproduces the existing `kTitleRowY` from `$5A82` exactly, which is what
  confirms the model.
- **Writer-driven dirty range, `g_hsCellLo/Hi`** — the exact twin of the Title card's. The ten
  transliterated `STA $37xx,X` sites report through `rof_hiscore_screen_dirty()`
  (`PRE_INSN_HOOKS`, a shared `_hs_dirty(base)` generator): `render_text_cell $5CA7`'s four
  ($5CC6 blank-on-RETURN, $5CDA rub-out, $5CE7 the glyph, $5CF4 the cursor) and `name_entry_loop`'s
  six ($5C06 the row shift, $5C18/$5C1B the level digits, $5C26 the score, $5C2B the blanked name,
  $5C44 the highlight). Marking at the write, not scanning at the read — the
  `standby-level-scroll` lesson. The call is DIRECT (declared in `rof_hiscore.h`, no-op body on
  SDL), not a virtual through the platform bridge, because the row shift issues one per byte.
  ⚠ **Two earlier attempts were wrong, in instructive ways.**
  1. *Decode all 200 cells every frame.* `decodeTextCells` clears a cell's own 2-byte column in
     all three planes before OR-ing the glyph in, and there is one bitmap with no double buffer —
     so every cell it touches is BLANK for a moment and flashes black if the beam is over it.
     User-observed: *"random single bytes flicker black across different frames."* Redrawing an
     unchanged cell is a CORRECTNESS bug here, not just waste.
  2. *A 200-byte shadow compare.* It fixed the flicker but paid 400 memory accesses a frame to
     learn that nothing had moved — straight into the "RAM is uniformly slow, reduce the NUMBER of
     accesses" rule (user, 2026-08-18). The dirty range costs nothing on an idle frame.
  The whole-bitmap clear happens on ENTRY only — the shared bitmap still holds the Title card.
- Copper + bitmap: **reused**, not duplicated (a second `Bitmap::allocate(320,216,3,true)` is
  ~25.9 KB of CHIP). `textScreenKind` (`kTextNone/kTextTitle/kTextHiScore`) is what makes a switch
  between the two force a fresh decode, since the copper object itself is shared. It needs no new
  clear sites: every place that clears `titleScreenCopperInstalled` already forces the next text
  screen through its full-decode entry path whatever the kind.
- Palette: unchanged. The entry loads `$02C4-$02C7` from its own table (`$5B99` → `$5D3B`, giving
  `$9A/$E8/$C8/$A6` where the Title card has `$38/$2A/$46/$56`), and `updateTitleScreenCopper()`
  already reads those live shadows and reproduces the `$53ED` pen cycle — so it serves both.

### W5 — deliver keystrokes outside flight ✅ (and the plan was half wrong about it)
⚠ **The premise "`$539A` has no hook, so no typed key has ever reached `$0049` outside flight" was
stale for the Amiga.** `$5398` is a native twin there (`vbi_attract_poll`, `rof_native_amiga.cpp`),
it already opens the window via `rof_attract_poll_key()` and already stores the keycode to `$0049`,
and it is the only thing that calls `$5398` in either standby-family VBI. No `PRE_INSN_HOOKS` entry
was added: on the Amiga the transpiled `$5398` never runs, and on SDL no key is ever delivered, so
the hook would have been dead code that could only do harm (consuming a pending `$80` and
double-firing the BREAK restart).

What WAS missing is the keyboard MAP: `kFlightKeys` carried only the eight command keys, so no
letter could ever be typed. It is now `kAtariKeys`, the whole Atari keyboard — A-Z, 0-9, RETURN,
BACKSPACE, space and the punctuation the `$5E50` translation table accepts. The eight command keys
are unchanged and needed no special case: they ARE ordinary letters (`L`=$00, `A`=$3f, `B`=$15,
`S`=$3e), matched by keycode downstream.
- Atari `-` (`$0e`) and `=` (`$0f`) are deliberately absent: those two Amiga keys are the thrust
  pair (`docs/controls.md`), so they type `+`/`*` in the entry. Cosmetic, and the thrust mapping is
  user-confirmed.
- The BREAK interaction needed no work — `rof_attract_poll_key()` already LEAVES a pending `$80`
  for `rof_check_restart` to drive from main-loop context.
- The AUTHOR easter egg (`match_code_sequence $5B45`, sequence `$3F $0B $2D $39 $08 $28` = A U T H
  O R) should now be reachable; not yet confirmed by eye.

### W6 — the `$5E0E` VBI ✅ — **it was code, not a check**
The plan expected to confirm that `game_vbi_isr` runs no body for an unrecognised VVBLKI. It does
the opposite: an unknown vector **falls back to `standby_vbi_native()`** (inert only during a boot
scene). And the `$5E0E` window is not microseconds — it contains the `wait_vcount_30` spin at
`$5DA0`, which can span a whole frame. So a full standby VBI body would have run under it. There is
now an explicit inert case for `$5E0E` in `game_vbi_isr`.
The rest of the analysis stands: `$5E0E` decrements CDTMV1 `$0218/$0219` and `JMP ($0226)` on
expiry, and nothing in the binary ever writes `$0218/$0219/$0226`, so the faithful body is nothing.

### W7 — harness ✅
- `make NAME_ENTRY=1` is now the hands-off "show me the high-score screen" build: it presses START
  for you once Standby's idle loop is polling (the auto-launch was `PROBES`/`FPSCOUNT`-only before,
  which left the one manual step in an otherwise automatic path), then arranges the death and the
  qualifying score. Pair with `SKIPBOOT=1` to skip the ~30 s of Logo + Station.
  `amiga/hiscore_reached.gdb` checks that build unattended — it reads only globals that exist
  without `PROBES`.
- It keeps the fast death + qualifying score and **no longer fakes the block** —
  the real loader has to get the entry past the gate, which is exactly what `g_neEnter` vs
  `g_nePass` distinguishes.
- Under `PROBES=1` it now also TYPES the initials (A, C, E, RETURN injected through
  `s_pendingFlightKey`, one every 12 frames once `g_nePass` is set and the DL is `$5E2E`), so the
  insert, the game's own save call and the deferred write are measured rather than left to a
  keyboard that a headless run has no way to press. The plain interactive build has no PROBES and
  still lets the letters be picked by hand, real hardware included.
- `amiga/name_entry.gdb` now also prints the SIO tallies (`g_hsSioRead/Write/Err`), the persistence
  outcome (`g_hsFromFile/g_hsDirty/g_hsWritten`), the live `g_atariDlist`, and the heading + row-0
  cells. ⚠ It reads GLOBALS only — an earlier draft called `rof_hiscore_dirty()` and read
  file-statics, neither of which is reliable over the FS-UAE stub.
- **`tools/hiscore_block_test.c`, registered in `make hostproof`**, is the real safety net: there is
  no 6502 oracle for any of this (the code it replaces was deleted from the binary), so the proof
  transcribes the game's OWN checks — `validate_save_state $5D0D`, the `$5D50` DCB, the `$5C54`
  write — and asserts the factory block passes the original gate, that a write round-trips, that a
  declined save stays dirty until flushed, that a reloaded block wins over the factory table, and
  that a foreign sector is refused rather than aliased onto the table.

## 3. What the end-to-end run measured

`make clean && make PROBES=1 NAME_ENTRY=1` + `GDBSCRIPT=name_entry.gdb ./diag_run.sh 150`, which
under PROBES dies, types A-C-E-RETURN and then quits cleanly so the exit flush actually runs:

```
savestate: 3700=28 (want 28)  3714=ee (want ee)  37C8=436f70 (want 436f70)
sio: DSTATS=01 DCOMND=57 sector=02cf  reads=2 writes=2 err=0
persist: fromFile=0 dirty=1 written=0        <- mid-run save DECLINED, as designed
probes: enter=1 pass=1 wait=32 glyph=8
table row0: a1 a3 a5 80 80 80 80 80 00 00 00 00 00 00 51 52 53 54 55 50
```

`pass=1` **with nothing faked** is the headline: the restored SIO call filled the block, the
signature matched the running binary's, and the original `validate_save_state $5D0D` accepted it.
`a1 a3 a5` decodes to "ACE" and `51 52 53 54 55 50` to "123450"; the `80`s are the `$5C3D-$5C44`
highlight pass. `dirty=1 written=0` is the deferral working — the write only goes out after
`run()` restores the OS.

And it does: `.run/dh1/RoF.hi`, 256 bytes, next to the executable (the CLI's cwd), decoding as

```
$3700 'HIGHEST SCORING ACES'   $3764 'PSL       3     2037'
$3714 'NAME    LEVEL  SCORE'   $3778 'GARY      3     1138'
$3728 'ACE           123450'   $378C 'CROCK     3      525'
$373C 'FOX       6    35000'   $37A0 'JSL       2      421'
$3750 'LOREN     5    17000'   $37B4 'DL        1      327'  -> shifted off the bottom
$37C8 "Copyright (c) 1987 Lucasfilm Ltd. v4.1"   $37EE = $10
```

i.e. the insert, the row shift, the signature and `level_progress` all round-trip through the file.

## 4. Where this diverged from the plan, and what is still open

- W5's premise was stale (see W5) and W6 was code, not a check (see W6).
- **`resload_SaveFile` is unreachable from the game**, so the WHDLoad save is the same deferred
  dos.library write as a Shell run (see W2). If a mid-run WHDLoad save is wanted, the slave would
  have to publish `resload` to the game somehow — a new design, not a tweak.
- **Where `RoF.hi` lands under WHDLoad depends on the unresolved `slv_CurrentDir` vs
  `#sub-dir "data"` inconsistency** (`docs/whdload-slave.md:259-265`). That loose end now has a
  second consumer.
- **Holding `dos.library` open costs untuned FASTMEMSIZE** (`docs/whdload-slave.md:125`).
- Not yet confirmed by eye: the entry screen's appearance in a real `run.sh` session, and the
  AUTHOR easter egg.

## 5. Order + risks (as planned)

W1+W2+W3 first (the block validates, the entry runs for real, `enter=1 pass=1` without any fake
seeding), then W5 (typing works, easter egg as the smoke test), then W4 (the screen). W6 is a check,
not code. W4 is the largest and the only one with copper/timing risk.

Known traps: the ISR-cost rule (a heavy decode in the vblank ISR silently drops a displayed frame —
`standby-level-scroll` memory); a scene entry that rebuilds a SHARED copper list must blank first
(`amiga-copper-lessons`); and `make clean` before any `PROBES=1` build. The three spins in this path
already carry `SPINWAIT_HOOKS` yields (2026-08-17) — do not remove them: without a yield they
compile to `bra.s .` on the Amiga.
