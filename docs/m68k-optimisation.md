# Optimising a native twin for the 68000 (hard-won; apply when a function is hot)

> Split out of `CLAUDE.md` 2026-08-10 to keep the always-loaded file small.
> **Read this before optimising any hot function or writing an asm twin.** The two rules that
> must not be violated even without reading this file (RAM is uniformly slow; never emit a 32-bit
> software mul/div) are stated in `CLAUDE.md` §Hard rules.
> Companions: `docs/perf-budget.md` (what the numbers mean), `docs/asm-migration-plan.md`,
> `docs/flight-perf-log.md`.

The transliteration→native step gets you a *correct* twin; it is NOT fast. The transliterated
style (`mem[addr]` for every access, `bus_read`/`bus_write`, per-op temporaries) is memory-bound
on the 68000. **The dominant cost is the number of memory accesses, not arithmetic** — the
68000 has no cache, every load/store goes to RAM, and `mem[]` is `volatile` (shared with the
VBI/audio ISRs) so the compiler can't cache, batch, or reorder a single access.

⚠ **RAM is slow REGARDLESS of address — do NOT reason in terms of "FAST RAM vs CHIP RAM".** The
target is a bare **A500 with NO real fast RAM**. Any "fast RAM" an A500 has is almost always
"slow RAM" (trapdoor/ranger) on the SAME bus as chip RAM, and even genuine fast RAM is not much
faster. So treat **every** memory access — `mem[]`, chip bitmaps/sprites, the stack (hence every
subroutine call/return) — as uniformly expensive. The lever is **reducing the number of reads and
writes**, full stop; never justify one buffer being cheaper than another by which "kind" of RAM it
lives in, and never dismiss a copy as cheap because it's "fast RAM". (This has been a recurring
mistake — the old `&mem[0]`≈`0x264fe8` "it's fast RAM" note was wrong-headed and is retired.)
A zero-copy scheme that avoids moving data beats any scheme that moves it, independent of address.
Rewrite hot functions in idiomatic C:

- **Keep loop scratch / running pointers / loop-invariants in locals (registers), not `mem[]`.**
  The transliteration re-reads/-writes ZP scratch every iteration (e.g. `terrain_collision_and_silhouette` hit
  `$80/$81/$95/$96` ~13×/iter). Hoist them into locals; write back only the *final* value the
  6502 oracle leaves in `mem[]` (the harness only compares post-return state, so intermediate
  ZP writes that the next iteration overwrites are dead — skip them). Cache invariants
  (`$00A0-$00A3` etc.) into locals once before the loop.
- **Pointer-walk with autoincrement, never multiply+index in a loop.** Replace
  `M[base + i*stride + Y]` (a 68000 `mulu` + indexed load each step) with a pointer advanced by
  `p += stride` / `p -= stride` (`move (a0)+` / `-(a0)`). Reuse a walked pointer across phases
  where the geometry allows (collision's scan leaves the pointer at row k, so the waterfall
  steps it back down with no fresh multiply).
  ⚠ **This rule kills a `mulu`+index — it does NOT beat an UNROLLED absolute scan.** Over a
  short fixed-length array GCC often emits straight-line absolute code (`move.b (base+i).l,dn`
  16 cyc + `beq.s` 10 = 26/element), which is *cheaper* than a pointer loop (`tst.b (a0)+` 8 +
  `beq.s` 10 + `addq.l #1,a1` 8 + `dbra` 10 = 36/element): the 18 cycles of loop bookkeeping
  exceed the 8 that autoincrement saves on addressing. Measured on the SFX mixer's 12-slot
  scans, where a "clean" pointer-walked asm twin came out **5% slower than the C**; the fix was
  to unroll as well and keep `(a0)+` only for the per-element test. **So: disassemble what GCC
  emitted BEFORE designing the asm** — if it already inlined and unrolled, you must beat
  straight-line code, and the headroom is small. Watch the prologue too: a 10-register `movem`
  costs ~180 cycles against GCC's 3-register ~68, which can exceed the whole win.
  ⚠⚠ **GCC UNDOES this rule when a loop walks 3+ pointers — and the exit test is the one-line fix.**
  ivopts strength-reduces N pointer IVs into ONE index register plus N invariant bases, so every
  access becomes `(0,An,Dn.L)`: **14 cycles of EA for a long against `(An)+`'s 8**, on top of losing
  the free increment. The full recipe, all three parts measured (log §14/§15):
  1. **Exit test = a POINTER COMPARE against a precomputed end** (`do { … } while (p != pEnd);`).
     `for (int n = count; n--; )` invites the strength-reduction; the pointer compare forces one IV
     to be a real pointer and GCC then keeps them all. Band paint **170 → 121 cyc/long**.
  2. **Post-increment EVERY pointer.** Leaving one as `*p` with a separate `addq` cost **16
     cyc/long** — GCC emitted `move.l (a0),d0` plus two `addq.l #4` instead of two `(a0)+`.
  3. **Constant trip count ⇒ `#pragma GCC unroll N`**, which deletes the loop bookkeeping outright
     (`(d16,An)` displacement compares, no counter at all). Change-detect scan **70 → 46**.
  Also **split a fused loop that carries pointers only its RARE path needs** — the band's scan was
  maintaining a decode-loop bound on every unchanged long. Four wins in this tree have now turned on
  this one pathology, so **read the disassembly of any hot multi-pointer loop before assuming its
  cost is the work it does.**
- **Batch bulk clears/copies with `move.l` through a NON-VOLATILE alias** of `mem[]`
  (`uint8_t* M = (uint8_t*)mem;`). Casting away `volatile` lets the compiler emit 4-byte stores
  and a tight loop. SAFE only for buffers the ISR doesn't touch concurrently — the main loop
  owns the `$1010+` terrain field (verified the flight VBI never writes it); ZP and ISR-shared
  regions must stay `volatile`. `move.l` needs an even/4-aligned address (odd → 68000 address
  fault) — align first (see `zero_run`). ⚠ The win is from **`move.l` batching of SEQUENTIAL
  bytes**, NOT from dropping `volatile` per se. For SCATTERED single-byte access (e.g. the terrain
  rasterizer's per-column PLOT + Y-walked interpolation arrays) a non-volatile alias is a measured
  **no-op** — GCC already keeps the base in a register, so there is nothing to batch. Don't chase
  volatile-vs-non-volatile for scattered access; that whole class of "cheaper mem access" is
  exhausted there — the cost is instruction count / algorithm, not the `volatile` barrier.
  ⚠ **Endianness when aliasing `mem[]` as `uint16_t*`/`uint32_t*`:** `mem[]` is little-endian
  (6502: `mem[a]`=lo). The Amiga 68000 is **big-endian**, so a word/long read through such an
  alias returns the **byte-swapped** value — and worse, the SDL validation host is little-endian,
  so `make validate` passes GREEN while the Amiga silently renders garbage. So do NOT alias for
  general 16/32-bit values; lift them into `uint16_t`/`int16_t` LOCALS and touch `mem[]` byte-wise
  at the boundaries (`mem[a] | (mem[a+1]<<8)`), as the rasterizer/`MIDPOINT` twins do. The ONE
  safe alias case is a **uniform-byte broadcast** store (e.g. fill 4 lanes with the same byte via
  `grp = b*0x01010101u`, walk a `uint32_t*`): all bytes equal ⇒ endianness-neutral (identical on
  host + Amiga). Used for `terrain_draw_frame_core`'s `$BD00` column-id fill (commit ac3a9a8) —
  46 long stores; still needs the 4-aligned + ISR-untouched + non-overflowing-lane conditions.
  ⚠⚠ **GCC CAN SILENTLY UNDO THE BATCHING — always re-read the disassembly.** A uniform fill written
  as a plain `uint32_t*` loop is recognised as a **memset** and becomes `jsr memset`, and this build's
  freestanding memset (`support/gcc8_c_support.c`) is a byte-at-a-time `move.b d0,(a0)+`/`cmpa.l`/
  `bne` loop at ~24 cycles a byte — i.e. it hands every byte write straight back and the "batching"
  is worth nothing. Keeping the pointer **`volatile`** is what pins the long stores (commit 688069d,
  `terrain_draw_frame_core`'s `$264E..$26D1` fill). ⚠ But volatile is not a free win either: over a
  SHORT trip count with two interleaved volatile long pointers GCC emitted a redundant volatile READ
  before every byte store — measured on the adjacent `$67` fill and reverted. So: batch, then LOOK at
  what was emitted; the source saying `move.l` guarantees nothing.
  ⚠ Also worth knowing: **"odd address" can mean odd OFFSET, not odd address.** The four `$6B` runs
  at `$264E/$266F/$2690/$26B1` carried a comment saying they could not be batched because
  `$266F`/`$26B1` are odd — they are odd only as offsets from `$260E`; every actual address is even,
  which is all `move.l` needs on a 68000 (it faults on ODD, 4-alignment is a 68020+ perf matter). And
  those four `$21`-byte runs ABUT, so they are really one contiguous 132-byte fill = 33 longs.
- **Skip redundant work the original wasted.** Avoid re-decoding/-scanning what hasn't changed
  (per-writer dirty flags; dirty row/cell ranges, cf. planet viewport `g_planetRowLo/Hi` and the
  cockpit plan `docs/cockpit-render-plan.md`). Shadow-compare scans are themselves a full
  volatile scan — a 68000 no-go; prefer dirty flags.
- **A transliterated loop's 6502 shape can BLIND GCC's loop analysis — that costs far more than the
  instructions it emits.** Two habits do it, and both look harmless: a loop counter/index typed
  `uint8_t` because the 6502 held it in a register (every use then pays an `andi.l #255` + a
  `moveq`/`move.b` zero-extend, and the wrap semantics hide the stride), and a `mem[]` round trip
  the 6502 needed to save a register across a `JSR` (which, being `volatile`, is an opaque write GCC
  must assume changes the index). Remove both and GCC can suddenly see a constant stride and a fixed
  trip count. On `terrain_draw_objects` (log §11) that turned an un-analysable loop into a ×3 unroll
  with ONE exit test — amortising the loop tail 22 → 6 cycles a pair, the largest single component
  of the win. **So when a hot loop's index is a byte or round-trips through `mem[]`, fix that
  first and re-read the disassembly before designing anything cleverer.**
- **When the idiomatic-C twin is still hot, ESCALATE to hand-written m68k asm** (vasm). GCC won't
  emit `(a0)+`, has no scaled index, and spills under the register pressure these loops create — so
  the C floor is GCC's floor, not the algorithm's. In asm you control the regs (pin the working set,
  walk a private stack with `(a3)±3`), force the addressing, and shave every redundant insn
  (`movea` copies, `and.w #$FF` after a `sub.b` into an already-zero-extended reg, `moveq#0;move.b`
  → `move.l` of a clean reg). This beat the C on `terrain_column_rasterize_core` (~27%) where four C
  restructurings had all regressed. Verify with the in-process differential (see `docs/perf-budget.md`),
  NOT cross-run. See `docs/asm-migration-plan.md` + `TerrainRasterizeAssembler.s`.
- **⚠ NEVER emit a 32-bit software multiply/divide (`__mulsi3`/`__divsi3`/`__udivsi3`/`__modsi3`/
  `__umodsi3`).** The 68000 has NO 32-bit mul/div — GCC lowers any `uint32_t`/`int32_t` `*` / `/` / `%`
  into those slow (~200-600 cyc) software routines. It has only `MULU.W`/`MULS.W` (16×16→32) and
  `DIVU.W`/`DIVS.W` (32÷16→16q+16r). Use the helpers in **`src/cpu/m68k_math.h`** — `rof_mulu16`,
  `rof_divu16`, `rof_modu16`, `rof_muls16`, `rof_divs16`, `rof_mods16` (inline asm on Amiga, plain-C
  on the SDL/validate host) — wherever a product's factors fit 16 bits and a quotient fits 16 bits
  (verify the ranges!). Techniques when a value looks 32-bit: fold constant factors with the exact
  identity `⌊n/(a·b)⌋ = ⌊⌊n/a⌋/b⌋` so the runtime divide shrinks to 16-bit (see `pokey_period`);
  reduce with `(a·b)%m = ((a%m)·(b%m))%m` (see `build_poly_dist`); replace a small-modulus wrap in a
  loop with compare-subtract (`if (x>=m) x-=m`); clamp an input so `2·x` stays <2^16. **Audit after
  any perf/math change:** `m68k-amiga-elf-objdump -d out/RoF.elf | grep -E '__(u?div|u?mod|mul)si3'`
  must be empty (bodies unreferenced → not even linked). The whole codebase was swept clean 2026-08-02.
- **⭐ NEVER write a 6502 16-bit pair as `((uint16_t)hi << 8) | lo` — use `ROF_PAIR16(lo, hi)`**
  (`src/cpu/m68k_math.h`). `or.b` can only reach bits 0..7, so from the `|` form GCC assembles the
  **big-endian** word and then byte-swaps it back:
  `moveq#0 / move.b LO,d0 / lsl.l #8,d0 / or.b HI,d0 / ror.w #8,d0` — **74 cycles, 46 of them
  shifting** (`lsl.l #8` = 24, `ror.w #8` = 22) to place one byte. `lo + 256*hi` is byte-identical
  for byte operands (256·hi has a zero low byte, so `+` and `|` cannot differ) and makes GCC load HI
  first and shift it as a WORD, so no swap is needed: **58 cycles (−16)**. And when two pairs are
  ADDED, GCC factors the two `lsl.w #8` into one — `(lo_a+lo_b) + 256*(hi_a+hi_b)`, correct mod 2^16
  because the shift discards the carry out of bit 15 — taking a pair-add site from 152 to **98
  (−54)**. ⚠ Flipping the `|` operands, `__builtin_bswap16`, and splitting into two locals ALL
  canonicalize to the same expensive form; `+ 256*hi` is the only reformulation that escapes it.
  ⚠ Each site costs ~2 bytes and up to ~10 instructions of re-allocation in the containing function,
  so **`.text` size is not the acceptance test — the test is that the site's `ror.w #8` is gone AND
  the function's instruction mix improved** (2026-08-14: 2 of 10 functions got worse and were
  reverted). Audit with
  `objdump -d out/RoF.elf | awk '/^[0-9a-f]+ <.*>:/{f=$2} /ror\.w #8/{c[f]++} END{for(k in c) print c[k],k}' | sort -rn`
  — 134 sites tree-wide before the 2026-08-14 sweep, 89 after (`docs/flight-perf-log.md` §25.2).
