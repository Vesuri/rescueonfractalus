# tools/

Everything here is reachable from a build, a documented workflow, or a `make` target — nothing is
scratch.  Grouped by what it is FOR; the file's own docstring/header has the usage.

## The transliteration pipeline (build-critical)

| Tool | Role |
|---|---|
| `transpile.py` | The 6502→C transpiler. Reads `disasm/listing.txt` + `disasm/symbols.csv`, writes `src/gen/rof_gen.c`, `rof_decl.h`, `mem.h`. `make gen`. |
| `validate_native.c` | The `make validate` harness: runs each native twin and its `__t6502` oracle on the same inputs and diffs full `mem[]`. |
| `xex_load.py` | Builds the post-load flat 64K image + the block manifest Ghidra imports. First step of the disassembly pipeline. |
| `xex_map.py` | Prints a XEX segment map (which segment loads where, and which are load-time pokes). |

## The Amiga boot image (build-critical)

| Tool | Role |
|---|---|
| `xex_deadset.py` | Derives the candidate-dead byte set from the disassembly — what the sparse image is allowed to drop. |
| `make_xex_sparse.py` | Builds the sparse boot image `amiga/assets/rof_sparse.bin` that `incbin.s` embeds. |
| `xex_dead_diff.py` | A/B proof for a dead-set change: diffs two frame-locked `mem[]` dumps with the candidate set masked out. |
| `check_chip_bss.py` | Link-time guard: fails the Amiga build if a `__chip` variable carries a static initialiser (it would be silently discarded — `docs/asset-extraction.md` §6.1). |
| `make_whdload_manual.py` | Renders the WHDLoad install's manual; run by `whdload/create_release.sh`. |

## Baked-asset generators — these keep a committed artifact reproducible

| Tool | Produces |
|---|---|
| `gen_logo_field.py` | `src/rof_logo_field.h` — replays the Atari's own `$5111` stroke plotter, so the baked logo is derived, not a magic blob. |
| `plot_logo_ref.py` | The proof for the above: re-plots from `rof.xex` and diffs against the committed header (expect 0 mismatches). |
| `gen_gtia9_pal.py` | The GTIA mode-9 16-luminance OCS palettes used by the boot scenes' copper lists. |

## Host-side equivalence proofs (`make hostproof`)

`alien_mirror_test.c`, `dot_table_test.c`, `ras_fused_midpoint_test.c`, `ras_restructure_test.c`,
`terr_blend_table_test.c`, `terr_blend_test.c`, `test_xex_sparse.c`.

Each compiles the OLD and NEW form of one routine side by side and diffs them over its whole input
domain (or a large random sample) — the only validation that reaches Amiga-only code, pure
reorderings and `#ifdef ROF_PLATFORM_AMIGA` arms.  ⚠ Each holds a *verbatim snapshot* of the
routine it was written against: green means "the transformation is sound", not "the shipping source
still matches the snapshot".  See the `hostproof` block in the root `Makefile`.

`xorshift_triple_test.c` sits with them but is NOT one of them: it is the design SEARCH that picked
the noise PRNG's shift triple (full period over GF(2), ranked by 68000 cycle cost).  It does not
check the triple the code ships, so `make hostproof` deliberately skips it; re-run it by hand if the
generator ever changes.

## Ground truth + diagnosis

| Tool | Role |
|---|---|
| `extract_a8s_ram.py` | Pulls the flat 64 KB RAM image out of an `atari800` savestate, anchoring on a ROM signature rather than a hard-coded offset. The Atari-side ground truth for every mem[] comparison. |
| `station_bmp.py` | Renders the Amiga GTIA-9 field dumped by `amiga/station_dump.gdb` to a PNG — pixel-checkable, unlike an emulator window. |
| `decode_terrain.py` | Decodes a dumped flight `terrainBitmap` (`amiga/dump_terrain.sh`) into a per-row histogram + ASCII map. |
| `compare.py` | Side-by-side + magenta diff of a port screenshot against its `atari800` reference (pure Python, no PIL). |

## Not in git

`ghidra` is a symlink to the shared install, `ghidra-proj/` is the local annotation project, and
`__pycache__/` is Python's — all three are gitignored.
