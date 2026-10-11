# Optimising a native twin for the 68000 — Rescue on Fractalus specifics

The general rules (memory accesses are the cost on an A500 with no real Fast RAM, pointer walks
and the 3+-pointer exit-test recipe, `move.l` batching and the `memset` trap, never aliasing
little-endian `mem[]` through wide pointers, byte-typed indices, escalating to asm, no 32-bit
mul/div, pair builds, size versus cycles) are maintained in AmigaXDev
`docs/reference/m68k-optimisation.md`; read it before optimising a hot function. The two rules
that must hold without reading anything are in `AGENTS.md` §Hard rules. Companions:
[perf-budget.md](perf-budget.md), [asm-migration-plan.md](asm-migration-plan.md),
[flight-perf-log.md](flight-perf-log.md).

## Helpers and audits in this tree

- **16-bit arithmetic:** `src/cpu/m68k_math.h` — `rof_mulu16`, `rof_divu16`, `rof_modu16`,
  `rof_muls16`, `rof_divs16`, `rof_mods16` (inline asm on Amiga, plain C on the host). Examples of
  the folding techniques: `pokey_period` (`⌊n/(ab)⌋ = ⌊⌊n/a⌋/b⌋`), `build_poly_dist`
  (`(ab) mod m`). Audit after any perf or math change:
  `m68k-amiga-elf-objdump -d out/RoF.elf | grep -E '__(u?div|u?mod|mul)si3'` must be empty.
- **Pair loads:** spell every new 6502 16-bit pair `ROF_PAIR16(lo, hi)` (`lo + 256*hi`, no
  `ror.w #8`). Count swap sites with
  `objdump -d out/RoF.elf | awk '/^[0-9a-f]+ <.*>:/{f=$2} /ror\.w #8/{c[f]++} END{for(k in c) print c[k],k}' | sort -rn`.
  ⛔ **The sweep is finished — do not re-run it.** Converting the remaining hand-written sites cost
  +699 instructions for −52 swaps (3 of ~50 functions improved), and converting the
  `ZP_IND_Y`/`ZP_IND_X` macros (`src/cpu/cpu.h`, ~50 sites) cost +517 instructions / +808 bytes for
  −57 swaps; both reverted ([flight-perf-log.md](flight-perf-log.md) §25.2, §26).
- **`mem[]` base folding:** `ROF_MEMBASE_DECL(mb)` + `#define mem mb` over a body turns `abs.l`
  operands into `(d16,An)`; `ROF_MEM_VIEW` for a local view. Host proof:
  `make validate MEMBASE=1 MEMVIEW=1`. Two acceptance tests that can disagree:
  - for cycles, the function's own `.text` in a whole-TU diff falls (fold at the callee, not a
    giant caller: all of `game_main_loop_body` was +36 B while `terrain_draw_frame_core` was −222);
  - for bytes, `out/RoF` falls: folding `boot_standby_launch_driver` grew its `.text` by 794 B yet
    shrank `out/RoF` by ~1.6 KB (−299 relocations). Keep such folds in boot/standby code.
  - Sum a function and its `.part.N` siblings before judging (`animate_clear_colors_timed`: net
    −168 B). Count operands with `objdump -d out/RoF.elf | grep -oE '<mem\+0x[0-9a-f]+>' | wc -l`
    and RELOC32 entries in the hunk ([asset-extraction.md](asset-extraction.md)).

## Measured examples

| Change | Result | Record |
|---|---|---|
| Band paint: pointer-compare exit instead of a counted loop | 170 → 121 cycles/long | log §14/§15 |
| Leaving one pointer as `*p` + `addq` | +16 cycles/long | log §14/§15 |
| Change-detect scan: `#pragma GCC unroll` on a constant trip count | 70 → 46 | log §14/§15 |
| `terrain_draw_objects`: byte index and `mem[]` round trip removed | ×3 unroll, loop tail 22 → 6 cycles a pair | log §11 |
| SFX mixer 12-slot scans: pointer-walked asm twin vs GCC's unrolled absolute code | asm 5% slower; fixed by unrolling as well | [asm-migration-plan.md](asm-migration-plan.md) |
| Second expansion of the plot macro in the object plane1 overlay | +750 B, `terrain_plot_pixel_core` outlined to `.part.0`; one expansion + runtime slot offset: +248 B | — |
| `terrain_draw_frame_core` `$BD00` column-id fill as a uniform-byte broadcast | 46 long stores (ac3a9a8) | — |
| Same function's `$264E..$26D1` fill: `volatile` pointer keeps the long stores (else `jsr memset`) | 688069d; the adjacent `$67` fill with two volatile pointers got worse and was reverted | — |
| The four `$6B` runs at `$264E/$266F/$2690/$26B1` | even addresses (odd only as offsets) and abutting: one 132-byte fill | — |
| `terrain_column_rasterize_core` in asm (`TerrainRasterizeAssembler.s`) | ~27% faster than C where four C restructurings regressed | [asm-migration-plan.md](asm-migration-plan.md) |
| Scattered single-byte access through a non-volatile alias (rasterizer) | no-op: nothing to batch | — |

The terrain field from `$1010` is owned by the main loop (the flight VBI never writes it), so a
non-volatile alias is safe there; ZP and ISR-shared regions stay `volatile`. `mem[]` is not
`volatile` in the Amiga core by default (`ROF_MEM_NONVOLATILE`; `make MEMNVC=1` reverts).
