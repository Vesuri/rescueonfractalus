# Dropping `rof.xex` from the Amiga binary — the sparse boot image

The design record for the last item of the incbin cleanup (`amiga-release-packaging` memory
item 1). Read this before touching `src/xex_load.h`, `src/platform/amiga/XexImage.cpp`,
`src/platform/amiga/incbin.s` or `tools/make_xex_sparse.py`.

**Status: SHIPPED.** `assets/rof_boot_image.bin` (27,872 B) replaced the embedded `rof.xex`
(43,066 B); `RoF.exe` 334,100 → 318,764 B (and **291,480 B** after the 2026-08-14 relocation-table
pass — see the size ledger at the end). The removal is gated on **static reachability**
(§5), not on play-testing — because the combat path turned out not to be run-to-run
deterministic, so no A/B can ever clear it. `make FULLXEX=1` restores the original.

---

## 1. What is being removed, and what is NOT

`rof.xex` (43,066 B) is embedded so the port can reproduce the exact power-on `mem[]` the
Atari's loader produces: zero RAM, place each segment, run the INITAD chain ending at
`game_entry $3CDE`. The image places **40,810 distinct bytes**, of which **25,687 are 6502
instruction bytes**.

The port transliterates the 6502 to C, so **nothing is ever executed out of `mem[]`**. The
image is needed only as a *data* source — tables, charsets, display lists, screen RAM. So the
instruction bytes are candidates for removal, and the only way one can still matter is if
something reads it as data.

⚠ **This does not stop shipping the original game's code.** `src/gen/rof_gen.c` *is* that
code, transliterated, and it is compiled into `RoF.exe` regardless. What removal buys is
bytes, plus no directly-extractable copy of the original file. Do not re-argue the
redistribution point on this basis.

Sized per segment (`tools/xex_map.py` for the layout):

| range | bytes | instruction | non-instruction |
|---|---|---|---|
| `$3CDE-$B7FF` (segment 16, the main blob) | 31,522 | 23,959 | 7,563 |
| `$1B30-$283D` (station) | 3,342 | 1,053 | 2,289 |
| `$0B00-$1AA6` (PMG buffers etc.) | 4,007 | 400 | 3,607 |
| `$4000-$44FF` (station image, staged) | 1,280 | *1,249* | 31 |
| `$5000-$536F` (logo) | 880 | 877 | 3 |
| `$3800-$3BFF` (charset) | 1,024 | 0 | 1,024 |
| `$0400-$05FF` | 512 | 0 | 512 |
| others | small | | |

Only `$3CDE-$B7FF` is in scope so far (`REGION_LO/HI` in `tools/xex_deadset.py`). The logo
and station code are a separate question.

## 2. ⚠ Trap: `listing.txt` describes the FLATTENED image

The italicised row above is the trap. `$4000-$44FF` is the **station image**, placed by
segment 10 — but segment 16 (`$3CDE-$B7FF`) later overwrites that address range with real
code, and `listing.txt` only ever shows one flattened memory image. So the disassembly labels
`$4000` as code even though the bytes the *station* needs there are data. Same for the logo
staged at `$5000`.

Hence the rule in `dead_set()`: a byte is a candidate only if **its segment is the LAST
WRITER of that address** — the one whose content the disassembly actually describes. This is
the same "`listing.txt` lies at a boot address" trap recorded in the `logo-station-port`
memory, reached from a different direction.

## 3. The method — and why the obvious cheaper versions are wrong

**Zeroing is not an approximation of removal, it is exactly equivalent.** A dropped byte
reads as 0 at runtime, because `rof_load_stage_reset()` clears all 64 KB before any segment
is placed. So a same-length asset with the candidate bytes zeroed models the sparse asset
faithfully.

Three things that must be right, each learned by getting it wrong first:

1. **Keep the asset LENGTH identical, so the binary is byte-identical in size.** The first
   A/B compared a 13 KB-smaller build against the full one and showed two of twelve 4 KB RAM
   checksums changed — which reads exactly like a live byte. It was the RNG trap: a
   different-sized binary does a different number of main-loop iterations per vblank, so the
   read-clocked `$D20A` LFSR is read a different number of times and all derived state
   diverges. Same binary twice *was* stable, which is what makes this so convincing and so
   wrong. (`feedback-same-length-asset-ab` memory, `rng-level-determinism`.)
2. **Dump `mem[]`, do not checksum it.** A checksum probe cannot answer "are these bytes
   read?", because the blocks it sums *contain* the bytes under test: zeroing 23,418 code
   bytes made `charset_ab.gdb` report `$3000-$BFFF` changed when that was only the zeroed
   bytes showing up in their own checksum. `amiga/mem_dump.gdb` dumps the raw 64 KB and
   `tools/xex_dead_diff.py` diffs it with the candidate set **masked out**.
3. **Frame-lock on the VBI ISR, not on `renderFrame`.** `renderFrame` runs below 50 Hz in
   flight, so `break … if g_vbiCount >= N` cannot land on a fixed frame — it read 5000 in one
   arm and 5006 in the other, silently making the two dumps incomparable. `mem_dump_flight.gdb`
   breaks at the `g_vbiCount++` site in the ISR (fires every vblank) on an **equality**
   condition.

`tools/xex_deadset.py` is imported by both the generator and the diff **on purpose**: if the
two computed the mask separately they could drift, and a mask wider than the bytes actually
zeroed would swallow a real difference and report a false "0 unexpected".

Driver: `amiga/xex_dead_ab.sh <label> <vbi> <seconds> [make flags…]` — builds both arms,
asserts the two `RoF.exe` sizes match, dumps at the same frame, diffs. Adds `FIXED_RNG=1`
unless given.

## 4. The sparse format

    chunk  := u16 load_addr, u16 len, len bytes      (big-endian, 68000-native)
    stream := chunk*

No stage marker: **a stage ends at the chunk covering INITAD (`$02E2-$02E3`)**, the same rule
`xex_parse_stage` uses for segments. So `xex_sparse_stage` is a structural twin of it and
`rof_boot.c`'s four-stage walk is unchanged.

**The generator emits a write wherever its SIMULATED `mem[]` disagrees with the target, not
merely where a byte is live.** This is load-bearing. A naive "drop the dead bytes" filter
would be wrong: because segment 16 buries the logo at `$5000` and the station image at
`$4000`, a dead byte there must still be written **as 0** to *clobber* the earlier data —
otherwise the station image survives into the game stage. The simulation catches those 2,160
addresses automatically. ⚠ **Never "optimise" the blob by dropping its zero runs.**

`$02E2/$02E3` are force-emitted even when they match the simulated state: INITAD `$5000` is
the bytes `00 50`, and that leading zero would otherwise be omitted, splitting the very chunk
the loader detects a stage boundary on.

Chunks bridge gaps of up to **4** unneeded bytes (`MERGE_GAP`) rather than starting a new
chunk — 4 is the header size and measurably the optimum. Bridging is free: an unneeded byte's
value already equals the simulated state, so writing it is a no-op.

| merge gap | blob (aggressive set) | chunks |
|---|---|---|
| 0 | 22,789 B | 1,361 |
| **4** | **20,051 B** | **265** |
| 8 | 20,292 B | 162 |
| 32 | 21,320 B | 53 |

**Shipped result: 43,066 B → 27,872 B (−15,194), 238 chunks**, dropping the 15,467-byte
conservative set of §5. (The wider 23,418-byte set would give 20,051 B; `load(conservative=
False)` in `xex_deadset.py` still builds it, but nothing vouches for the extra 7,951 bytes —
see §5.)

Two self-checks, both refuse-to-ship:

* the generator replays its own output and asserts it reproduces the zeroed image at all four
  stage boundaries **and** at the end;
* `tools/test_xex_sparse.c` proves the **C** loader equals the **C** segment loader —
  `cc -O2 -o /tmp/t tools/test_xex_sparse.c && /tmp/t <zeroed.xex> <sparse.bin>`. This exists
  because the Amiga *cannot* answer it by A/B: the sparse asset is 15 KB smaller, so the
  binary is a different size, and §3.1 applies. Comparing the two loaders as pure functions
  over one target image is deterministic and takes a millisecond.

`make FULLXEX=1` embeds the original `../rof.xex` and switches the loader back to the segment
walk (verified: it rebuilds to exactly the pre-sparse 334,100 B) — the A/B to reach for if the
sparse image is ever suspected.

## 5. ⭐ What actually gates the removal: static reachability, NOT play-testing

The plan was to walk every scene with the §3 A/B. **That plan died on the combat path, and the
way it died is the most useful thing in this document.**

### 5.1 Combat is not run-to-run deterministic

The combat A/B reported ~1,074 unexpected bytes in 168 runs, spread across ZP and `$06xx` —
which reads exactly like a live byte being consumed. The control says otherwise: **two runs of
the SAME binary differ in 1,024 bytes across 161 runs.** The A/B "signal" and the noise floor
are the same size, so the comparison carries no information at all.

| arm | differing bytes | runs |
|---|---|---|
| intact vs zeroed | 1,074 | 168 |
| **same binary, twice** | **1,024** | **161** |
| quiet flight, same binary, twice | **0** | 0 |
| Standby, same binary, twice | **0** | 0 |

So Standby and quiet flight *are* deterministic (their §3 results stand), but combat — where
the RNG is consulted far more often — is not, and **no amount of re-running fixes that.**
⚠ **Always take the same-binary control before believing a cross-build `mem[]` diff.** It is
the same lesson as `feedback-same-length-asset-ab`, one level up: there, a size change faked a
difference; here, the path itself does.

### 5.2 So the gate is static reachability

`reachable_map()` in `tools/xex_deadset.py` marks every byte **any** instruction could read as
data, deliberately pessimistically:

| source | window |
|---|---|
| exact absolute read `LDA $1234` | that byte |
| indexed absolute `LDA $1234,X` | `base .. base+255` (whole window; real tables are far shorter) |
| indirect via a ZP pointer built from an immediate pair | `ptr .. ptr+255` |
| `JMP ($1234)` | its 2-byte vector |

Result over the 23,418 instruction bytes of `$3CDE-$B7FF`:

| | bytes |
|---|---|
| named by an **exact** absolute read | **0** |
| inside an indexed `base+255` window | 5,695 |
| inside an immediate-built pointer window | (overlapping) |
| **touched by nothing at all → dropped** | **15,467** |
| touched by something → **kept** | 7,951 |

**Zero** of them is named by an exact absolute read — that is the strong, coverage-independent
result. The shipped asset keeps all 7,951 bytes anything could reach and drops only the
15,467 nothing can, so the correctness argument does not depend on having played every scene.

### 5.3 Residual risk, stated plainly

A pointer **computed at runtime from a table** is invisible to §5.2 and could in principle
reach a dropped byte. What backs that gap up:

* the §3 A/B on the two deterministic paths (Standby, quiet flight) showed **0** unexpected
  differences for the *wider* 23,418-byte set — i.e. it cleared even the bytes now kept;
* every dropped byte is one Ghidra decoded as an instruction, so a live one would mean a data
  table that Ghidra mis-disassembled *and* is reached only by a computed pointer;
* smoke-verified after the switch: station stage byte-identical to the full-xex run
  (`phase=$94`, `scrollPtr=$B800`, `starSeeds 10/01`, `$0600=0000f000`, `DL[0]=4f 0006`),
  Standby reached (`VVBLKI=$52D7`, `$0041=03`), the `$6E23` `"LEVEL"/"DROID"` table — which
  lives inside segment 16 — intact, all 7 charset glyphs unchanged, flight reached
  (`VVBLKI=$4FF5`), shipping-build mul/div audit clean.

⚠ If a scene ever misbehaves in a way that smells like corrupt data, **`make FULLXEX=1` is the
first thing to try** — it is a one-flag A/B against the original image.

## 6. Where the rest of `RoF.exe` goes

Accounting for the 384,176 B `PROBES` build by parsing its **hunk** structure (ELF section
sizes do not show it — Elf2Hunk adds the symbol and relocation hunks itself, which is how
33 KB of `HUNK_SYMBOL` went unnoticed until now; see `a0dff0e`):

| hunk | bytes | note |
|---|---|---|
| `HUNK_CODE` | 229,404 | transliterated 6502 + native twins + framework + asm twins |
| `HUNK_DATA` | 63,176 | of which 43,066 was `rof.xex`, 1,024 charset, 14,656 chip data |
| `HUNK_RELOC32` | 48,992 | 12,051 fixups — the **`abs.l` addressing tax again**, in file size this time |
| `HUNK_SYMBOL` | 42,520 | debug symbols; now stripped by `elf2hunk -s` |
| `HUNK_BSS` | (191,560) | declared only, no file bytes |

Cumulative for the release-packaging pass: **446,224 → 318,764 B (−28.6%)**.

| step | commit | exe |
|---|---|---|
| baseline | | 446,224 |
| drop two dead incbin blobs (`screen3_mem.bin`, `cockpit.raw`) | `70704ed` | 380,636 |
| 14 KB OS ROM → 1 KB charset | `1a614d1` | 367,204 |
| `elf2hunk -s` (strip `HUNK_SYMBOL`) | `a0dff0e` | 334,100 |
| `rof.xex` → sparse boot image | `0b03c33` | 318,764 |
| *(the 2026-08-14 perf work put ~1.9 KB back — joystick, edge-plot merge)* | | 320,716 |
| `ROF_MEMBASE` fold ×20 + `ROF_PAIR16` ×7 | `882b5e2` | 306,364 |
| *(the `$VER:` version string — 44 B, `src/platform/amiga/version.s`)* | | 306,408 |
| gate the unconditional flight-VBI profiler (§6.2) | | 306,112 |
| **`.MEMF_CHIP` → chip BSS hunk** | this | **291,480** |

Cumulative for the release-packaging pass: **446,224 → 291,480 B (−34.7%)**.

⭐ **The relocation table was the second-biggest thing in the file, and the base fold is what
shrinks it.** The 2026-08-14 pass folded 20 more functions (`ROF_MEMBASE`, `rof_native.c`), taking
`abs.l` mem[] operands **5,540 → 3,988** and RELOC32 entries **11,355 → 9,632 — i.e. 45,420 →
38,528 B of pure relocation table**, for −14,352 B of `RoF.exe` overall. Two lessons that make this
repeatable live in `docs/m68k-optimisation.md` §ROF_MEMBASE: the size test is **`RoF.exe`, not
`.text`** (a fold can grow a function's code and still shrink the file, because each folded operand
also deletes a 4-byte reloc — `boot_standby_launch_driver` is exactly that case), and a `.part.0`
split must be read as one function.

### 6.1 ⭐ `.MEMF_CHIP` was 14,654 bytes of literal zeros (2026-08-14)

The five `__chip` POKEY→Paula waveform buffers (`noise_buf` 8 KB, `poly_dist_buf` 4 KB,
`poly5_wave`, `poly4_wave`, `wave_pure`) are all **filled at runtime** by
`PlatformAmiga::audioInit()` — but GCC emits a section-attributed variable into a PROGBITS
section whether or not it has an initialiser, so all 14,654 bytes shipped in the file. Measured
**100.0 % zero** (14,652 of 14,654). `amiga/memf_chip_bss.ld` re-links `.MEMF_CHIP` as
`(NOLOAD)`, i.e. `SHT_NOBITS`, and Elf2Hunk turns that into **`HUNK_BSS` with the `MEMF_CHIP`
flag** — chip RAM is still allocated and still cleared (LoadSeg zeroes a BSS hunk), it just is
not stored. The chip RAM *requirement* is unchanged, so the WHDLoad `CHIPMEMSIZE` needs no
retune. **−14,632 B for zero runtime cost.**

Two things this brought with it:
* ⚠ **A static initialiser on a `__chip` variable is now silently discarded.** `wave_pure` was
  `= { 0x7F, 0x81 }` and would have come up as two zero bytes — i.e. the default waveform for
  every channel would have been silence, presenting as an audio bug with no bad arithmetic
  anywhere. Guarded: `amiga/tools/check_chip_bss.py` fails the link if any object contributes a
  nonzero byte, and the guard was transition-tested by re-adding the initialiser.
* A host-side size diff cannot see whether the buffers are still *correct* at runtime, and no
  differential can either (nothing reads them but Paula's DMA). `amiga/chipbss_verify.gdb` is the
  instrument: it checks the addresses are below `$200000` (really chip), that `wave_pure` reads
  `7f 81`, and that each table has a nonzero byte (a BSS hunk starts as zeros, so "the generator
  ran" and "the hunk was cleared" look identical unless you sum the contents).

### 6.2 What is left

Release build, 291,480 B — `HUNK_CODE` 216,848 · `HUNK_RELOC32` 38,632 · `HUNK_DATA` 35,908
(27,872 of it the boot image) · `HUNK_BSS` 16 (198,100 B declared).

**The relocation table is still the second-biggest thing in the file** — 13.3 % of it, 9,620
fixups, of which **4,347 point at `mem` and 2,148 at `cpu`**, i.e. 67 % are the `abs.l` addressing
tax. Each operand folded to `d16(An)` deletes 2 code bytes *and* a 4-byte reloc, so this is the
one remaining large lever that costs no performance (it is the same `ROF_MEMBASE` transformation
as the perf work, and `cpu` has no equivalent fold yet). The unfolded `mem` operands are mostly in
`rof_gen.c`, which needs transpiler support rather than a hand edit.

Everything else that was checked and found not worth doing:
* **Debug symbols: already gone.** `elf2hunk -s` strips `HUNK_SYMBOL`; the release `.exe` has no
  `HUNK_SYMBOL` and no `HUNK_DEBUG` block at all. `-g` in `CFLAGS` costs the file **zero** bytes —
  `.debug_*` and `.comment` are non-`SHF_ALLOC`, so Elf2Hunk never sees them, and `RoF.elf` keeps
  the full DWARF for `debug.sh` and every `amiga/*.gdb`.
* **No verify/oracle code survives `--gc-sections`** in a default build: no `*_core_c` oracle, no
  `*_verify` wrapper. Checked by symbol sweep, not by reading the `#ifdef`s.
* **Unreferenced vendored framework asm: 48 bytes.** vasm emits one `code` section per file so
  `--gc-sections` cannot split it, but the only unreferenced routines are three `Bitmap` stubs and
  `getVBR`, together 48 B of the 16,668 B `code` hunk. Measured, then dropped as noise.
* **`.data` is 93 % zeros** (2,591 of 2,777) but it is essentially one object — the
  `static RescueOnFractalus scene` inside `PlatformAmiga::run()`, 2,628 B, with a handful of
  nonzero members that legitimately keep it out of `.bss`. Nothing cheap here.

And one lever that is real but **costs frame rate, so it is the user's call, not a free win**
(clean-build A/B, same tree, 2026-08-14):

| build | exe | Δ |
|---|---|---|
| shipping (`OPT=-O2 -fomit-frame-pointer`, `NATIVE_OPT=-O3 -funroll-loops`) | 291,480 | — |
| `NATIVE_OPT='-O3'` (drop `-funroll-loops` only) | 262,420 | −29,356 (−10.1 %) |
| `NATIVE_OPT='-O2'` | 236,020 | −55,756 (−19.1 %) |
| `OPT='-Os -fomit-frame-pointer'` (`rof_native.c` still `-O3 -funroll-loops`) | 275,180 | −16,596 (−5.7 %) |
| both `-Os` | 198,856 | −92,920 (−31.9 %) |

`rof_native.c` is 115,266 B of the 216,848 B code hunk and holds the hot flight twins, which is
exactly why it gets `-O3 -funroll-loops` — but it also holds a lot of cold one-shot drivers
(`boot_standby_launch_driver` alone is 14,564 B), and those pay the unrolling for nothing. A
per-function `__attribute__((optimize("Os")))` on the cold set would take most of that 56 KB
without touching flight; it needs a hot/cold classification, which the PC profiler can supply.
