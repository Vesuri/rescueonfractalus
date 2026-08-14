# The WHDLoad install — slave design, build and tuning

The end-user WHDLoad install for the Amiga port. Read this before touching
`whdload/RoFSlave.s`, the install package, or the memory sizes.

Files:

| Path | What |
|---|---|
| `whdload/RoFSlave.s` | the slave source — assembled **on the Amiga** with `basm` |
| `whdload/makefile` | Amiga-side build (`basm`), mirrors the Stunt Car Racer arrangement |
| `whdload/RoF Install/` | the install package, derived from WHDLoad's Install Template |
| `whdload/RoF Install/Manual` | the Lucasfilm manual as plain text — **generated**, `python3 tools/make_whdload_manual.py` from `docs/manual.md`; checked in, and `Manual.info` (a copy of the template's MultiView icon) gives it a Workbench icon |
| `whdload/create_release.sh` | host-side: collects exe + slave + package into `whdload/dist/RoF.lha` |

## 0. Why this is a kickemu slave and not a 20-line loader

This is the one decision everything else follows from, so it is recorded first.

A plain WHDLoad slave runs the installed program with **no operating system at all** —
WHDLoad sets `execbase` (`$4`) to `$f0000001` specifically so that any OS access takes an
address error. From WHDLoad's own `howto`: *"At the time the Slave and the installed
program is executed, absolutely no OS exists nor is accessible nor makes any sense to
access!"*

That model fits a trackloader game. It does not fit this port, because the port is an
ordinary AmigaDOS executable whose entire display takeover **is** operating system calls:

| Call | Where | Count |
|---|---|---|
| `AllocMem` / `FreeMem` | every bitmap, copper list, sprite, audio buffer, `operator new` | 18 / 8 |
| `OpenLibrary("graphics.library",33)` / `CloseLibrary` | `PlatformAmiga` ctor/dtor | 2 / 1 |
| `LoadView` / `WaitTOF` | save + restore the OS view | 4 / 7 |
| `SysBase->IntVects[INTB_VERTB]`, `Disable` / `Enable` | the VBI vector takeover | 8 / 8 |
| `OpenResource("ciaa.resource")` + `AddICRVector` | the CIA-A SP keyboard handler | — |
| `Forbid` / `Permit`, `AvailMem` | scheduler off, the RAM report | 2 / 7 |

Three ways to bridge that were weighed (2026-08-14). The user chose the kickemu, with
`kick13.s` rather than `kick31.s`:

1. **A game-side OS shim** — an `ROF_WHDLOAD` path replacing those entry points with a bump
   allocator over WHDLoad's BaseMem/ExpMem plus raw level-2/level-3 autovectors. Cheapest at
   runtime and needs no ROM image, but it forks the startup code and puts ~150 lines of
   unvalidatable Amiga-only C++ under the game. **Declined** — the whole point of the install
   is to ship the same binary everyone else runs.
2. **A fake exec/graphics inside the slave** — keeps the binary byte-identical but moves the
   struct-offset fragility into asm that `make validate` cannot see. **Declined.**
3. **kickemu (chosen)** — WHDLoad's own answer for OS-heavy programs. `kick13.s` boots a real
   Kickstart into WHDLoad's space; the game runs inside it as a normal CLI program,
   **completely unmodified**. Costs a user-supplied ROM image and memory.

`kick13` rather than `kick31` because the game is 1.3-clean and the 1.3 ROM image is
`$40000` (256 KB) against 3.1's `$80000` (512 KB) — that difference comes straight off the
memory requirement. 1.3-clean was checked two ways: the game asks for `graphics.library`
v33 (= KS 1.2) and touches nothing newer, and the freestanding GCC CRT is OS-free (`_start`
runs `init_array`, calls `main`, runs `fini_array`, `rts` — it reads `SysBase` from `$4` and
opens no library). Confirmed running under `kick34005.A500` by the user, 2026-08-14.

## 1. How the slave runs the game

`HDINIT` mounts the slave's `ws_CurrentDir` (`data`) as `DH0:`, and `BOOTDOS` makes
`kick13.s` synthesise a startup-sequence that calls `_bootdos` as a **real CLI process**
with `dos.library` fully up. So `_bootdos` is just what a Shell would do: `OldOpenLibrary`
dos, `LoadSeg("RoF")`, `jsr` the first hunk + 4.

When the game returns (the player quit), the slave calls `resload_Abort(TDREASON_OK)`
rather than dropping back to the Shell — with `BLACKSCREEN` the user cannot see a 1.3
prompt, and on a 68000 could not exit from one (below).

⚠ **The QuitKey only works on a 68010 or better.** WHDLoad reads it through the relocated
VBR, which a plain 68000 does not have (`Docs/en/opt.html`: *"this works only if the VBR is
moved by WHDLoad … requires at least a 68010"*). On a stock A500 the way out is the game's
own quit — left mouse button — which returns through `_bootdos` and aborts cleanly. That is
why `_bootdos` aborts unconditionally instead of returning to the CLI: WHDLoad's own
`_bootdos` examples make that a choice (their `QUIT_AFTER_PROGRAM_EXIT` switch), but here
returning would strand a 68000 user with no exit at all.

### The left-mouse quit stays as it is (user decision, 2026-08-14)

`rof_check_restart()` (`PlatformAmiga.cpp:1694`) quits on a bare
`isLeftMouseButtonPressed()`, polled from both `renderFrame` and the frame pump — so one
unconfirmed click, anywhere in the game including mid-mission, unwinds straight out. Making
it need **both** buttons was offered (a second read of POTGOR bit 10 beside the existing
CIA-A PRA read) and **declined**: documented instead, in the install ReadMe and in the
project `README.md`. So do not "harden" it later as a tidy-up — it is a deliberate choice,
and on a 68000 it is the only exit that exists.

## 2. The memory sizes — where the numbers come from

`kick13.s` derives the WHDLoad request from two symbols:

```
ws_BaseMemSize = CHIPMEMSIZE                  chip, mapped from address 0
ws_ExpMem      = $40000 + FASTMEMSIZE         $40000 = the 1.3 ROM image
```

Currently `CHIPMEMSIZE = $60000` (384 KB) and `FASTMEMSIZE = $90000` (576 KB), i.e. a total
request of **1216 KB**. With WHDLoad and the host OS on top that means a **2 MB machine** —
this install asks for more than the bare executable does, and that is the price of the
kickemu. (Plain `RoF` from Workbench still needs only 1 MB; see `README.md`.)

What has to fit:

| | Bytes | Source |
|---|---|---|
| chip: runtime `AllocMem(MEMF_CHIP)` | 217,408 | **measured** — `amiga/memreport.gdb`, KS 3.1 / A500+ |
| chip: `.MEMF_CHIP` hunk | 14,654 | `objdump -h out/RoF.elf`. Since 2026-08-14 a **BSS** hunk (`amiga/memf_chip_bss.ld`) — 14,632 B smaller in the *file*, identical chip RAM **requirement**, so `CHIPMEMSIZE` is unaffected |
| chip: the 1.3 boot's own use | ? | CLI screen bitmap, copper, fonts |
| fast: non-chip hunks | 436,321 | `.text` 200,386 + `code` 16,666 + `.rodata` 33,048 + `.init_array` 4 + `.data` 2,777 + `.bss` 183,440 |
| fast: `operator new` | 1,272 | **measured**, same run |
| fast: the CLI stack | 16,384 | `STACKSIZE` (V33's default is 4000) |
| fast: exec/dos/filesystem structures | ? | |

The two `?` rows are why the sizes are set generously rather than tightly. **They have not
been tuned against a real run yet** — nothing below has been measured under the slave,
because the slave is assembled on the Amiga.

### Re-tuning them

`make RoFTune.slave` builds the same slave with `DEBUG` and `MEMFREE = $200`. `MEMFREE`
patches `exec.AllocMem` to record the smallest free chunk ever seen; after a **full
playthrough** (boot cinematic, standby, title card, launch, flight, a landing and a rescue,
a crash, the mother ship) make a WHDLoad dump and read the memory dump file:

* `$200` — the low-water mark of free **chip** memory
* `$204` — the same for **fast**

Subtract those from `CHIPMEMSIZE` / `FASTMEMSIZE`, round up to a multiple of `$1000`, leave
a margin, and put the results back into `RoFSlave.s`. `MEMFREE` also traps a trashed memory
list (its `AvailMem(LARGEST)` walks every free block), which makes the TUNE slave the right
one to reach for if the install misbehaves.

⚠ Do not tune from the numbers in §2's table alone: they are host-side measurements of the
game under the *real* OS, and say nothing about what the emulated 1.3 needs beside it.

## 3. Building it

The slave is **not** part of the host cross-build. It `INCLUDE`s WHDLoad's `kick13.s`,
which is written for BASM / ASM-One / Asm-Pro / PhxAss — vasm is not in its translator list
— so it is assembled on the Amiga exactly like the Stunt Car Racer slave:

```
; on the Amiga, with Includes: and Sources: assigned (see whdload/makefile)
cd RoF:whdload
make
```

Then, on the host:

```
cd amiga && . env.sh && make          # the game -> amiga/out/RoF
cd ../whdload && ./create_release.sh  # -> whdload/dist/RoF.lha
```

The WHDLoad development package is expected at `~/.local/share/amiga/WHDLoad` — outside
every repo, the same convention as `fsuae_common.sh`. That is where the Install Template,
`kick13.s`, the autodocs and the `C/WHDLoad` executable were taken from.

## 4. What was changed in the Install Template, and why

The package is WHDLoad's stock `Install Template` with the smallest possible set of edits
(the template's own ReadMe insists on that — variables, not deletions):

* `Install.info` tooltypes: `APPNAME=Example` → `APPNAME=Rescue on Fractalus!`, and
  `MINUSER=AVERAGE` → `MINUSER=NOVICE`. Average exists to make the installer ask the
  expert-mode questions that DIC/RawDIC/Patcher need; this install runs none of them.
  ⚠ These are length-prefixed Amiga icon strings — the 4-byte big-endian length before each
  one must be fixed too, not just the bytes.
* `(set #prefix "RoF")`, pinned, where the template has `(set #prefix @app-name)`. The two
  are deliberately split: `@app-name` is the TITLE the prompts show ("Rescue on
  Fractalus!"), while `#prefix` is a FILE name — the installed drawer, `RoF.slave` and the
  icon `RoF.inf` — and it must stay `RoF`, which is what the executable is called too.
  Leaving them equal would make the install look for `Rescue on Fractalus!.slave`.
* `(set #sub-dir "data")` — matches `ws_CurrentDir` in the slave, so `HDINIT` mounts the
  drawer the executable is actually in.
* The `RawDIC` / `Patcher` / `DIC` `P_ChkRun` calls were removed. They abort the install if
  those programs are not in the path, and requiring them here would fail on machines that
  have no reason to have them. The `WHDLoad` check stays.
* The `3 = DOS (Files)` branch of `P_MakeImages` was replaced. Every template branch copies
  from an original disk or image; this game ships as one self-contained executable inside
  the archive, so the branch is one `copyfiles` of `RoF` — plus a soft warning if
  `Devs:Kickstarts/kick34005.A500` is missing, so the user learns that while the ReadMe is
  still on screen rather than at the first failed launch.

⚠ `Install` is **ISO-8859-1**, not UTF-8 (it contains e.g. `français`). Edit it with
something that round-trips latin-1, or the accented bytes get mangled.

## 5. Not done / open

* **The sizes in §2 are untuned** — see §2's re-tuning recipe. This is the one thing that
  should happen before a public release.
* **The icon is the template's placeholder** (`RoF.inf`, a plain 36×22 icon). A real one,
  and optional NewIcon / OS3.5 colour variants, would be dropped in as
  `RoF.newicon` / `RoF.colicon` — the install picks up any suffix it finds and offers the
  user a choice, no script change needed (`P_SelectIcon`, and the suffix table at the top
  of `Install`).
* **`slv_info` credits "Amiga port by Vesuri"**, matching the Stunt Car Racer slave. Change
  it in `RoFSlave.s` if the release should read differently.
* The version/date string is literal in `RoFSlave.s` (`$VER: RoF.slave 0.9 (14.08.2026)`).
  Stunt Car Racer stamps it from `WDate >T:date`; this one is bumped by hand — deliberately,
  so an unchanged tree rebuilds byte-identically.
* ⚠ **The version number lives in three files** and nothing checks that they agree:
  `src/platform/amiga/version.s` (the executable's own `$VER: Rescue on Fractalus! 0.9`),
  `RoFSlave.s` (`slv_info` *and* the slave's `$VER:`), and `RoF Install/ReadMe`
  (the History section). Bump all three together.
* **There is nothing to save, so there is no `resload_SaveFile` path** — and that is
  faithful, not an omission: the Atari original keeps HIGH SCORE in RAM only and loses it at
  power-off. If a persistent high score is ever wanted it would be an *addition* to the game,
  not a slave feature. The install template's `#highs-file` is left at its default `highs`;
  no such file ships, and `P_CopyFile` skips what does not exist.

## Related

* `docs/asset-extraction.md` — why the executable is 306,364 B and self-contained
* `README.md` — the player-facing requirements for the plain (non-WHDLoad) executable
* `~/.local/share/amiga/WHDLoad/Src/sources/whdload/kick.readme` — every `kick13.s` symbol
* `~/.local/share/amiga/WHDLoad/Docs/en/` — the WHDLoad manual (`howto`, `opt`, `cache`)
