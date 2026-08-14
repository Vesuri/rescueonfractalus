# Toolchain & how to run the pipeline

All paths are relative to the project root (`Rescue on Fractalus/`). Host is
macOS (Apple Silicon).

## Installed

| Tool | Where | Purpose |
|---|---|---|
| OpenJDK 21 | `/opt/homebrew/opt/openjdk@21` (keg-only) | Ghidra runtime |
| Ghidra 12.1 | `tools/ghidra/ghidra_12.1_PUBLIC` | disassembly + analysis (headless) |
| Python 3.11 | `python3` (pyenv) | XEX tools, transpiler |
| clang / make | system | building the SDL host port (`make` at the repo root) |
| SDL2 | homebrew | the macOS dev/profiling backend |
| `m68k-amiga-elf-*` + `vasmm68k_mot` + `elf2hunk` + patched FS-UAE | `~/.local` (on `PATH` via `. amiga/env.sh`) | the Amiga cross-build, from BartmanAbyss `vscode-amiga-debug` — its gcc/gdb/elf2hunk/FS-UAE are mutually matched, which is what makes source-level debugging work |
| `atari800` | homebrew | the Atari ground-truth emulator (FIFO monitor mode) |

⚠ **The Amiga toolchain must live at a space-free path** (hence `~/.local`, not
this repo). GCC's LTO link bakes the install path into `COLLECT_LTO_WRAPPER` and
`posix_spawnp`s it, and a space — as in `…/Rescue on Fractalus/…` — breaks the
spawn. `amiga/env.sh` resolves symlinks with `pwd -P` so gcc only ever sees
space-free paths.

> **2026-08-12:** `tools/ghidra` is now a symlink to a shared install at
> `~/.local/share/ghidra`, also used by the Revs repo, rather than its own copy — saves
> ~900 MB of duplication between binary-only-port repos. No trailing slash on the
> `tools/ghidra` gitignore entry, since a trailing-slash pattern doesn't match a symlink
> to a directory. The extracted copy that had been here was found to be missing
> `support/` (pruned to ~180 MB at some point, breaking headless use); it was
> re-extracted from a fresh copy of the same `ghidra_12.1_PUBLIC_20260513.zip` release
> before being moved to the shared location.

> `brew install` on this machine triggers the Privileges.app elevation prompt,
> so kick off installs yourself or approve the prompt when one appears.

## Environment

Ghidra needs JDK 21 on `PATH`:

```sh
export JAVA_HOME="/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home"
export PATH="$JAVA_HOME/bin:$PATH"
```

## XEX inspection

```sh
python3 tools/xex_map.py  rof.xex            # segment table + memory regions
python3 tools/xex_load.py rof.xex disasm     # -> disasm/rof_mem.bin + rof_blocks.txt
```

`rof_mem.bin` is the post-load 64K image (all segments applied in load order,
matching real loader semantics, incl. the `$3CDE` overlap). Entry = `$3CDE`.

## Disassembly (headless Ghidra)

One-shot import + auto-analysis + listing export (re-creates the project):

```sh
GH="tools/ghidra/ghidra_12.1_PUBLIC"
ABS="$(pwd)"
"$GH/support/analyzeHeadless" tools/ghidra-proj RoF \
  -import disasm/rof_mem.bin \
  -processor "6502:LE:16:default" \
  -loader BinaryLoader \
  -scriptPath ghidra_scripts \
  -preScript  MarkEntries.java \
  -postScript ExportListing.java "$ABS/disasm/listing.txt"
```

Re-run a script against the **already-imported** program (no re-analysis):

```sh
"$GH/support/analyzeHeadless" tools/ghidra-proj RoF -process rof_mem.bin \
  -scriptPath ghidra_scripts -noanalysis \
  -postScript ExportListing.java "$ABS/disasm/listing.txt"
```

### The headless loop (how we iterate)

1. Export current state → `disasm/listing.txt` (+ xref/hw dumps).
2. Read the text here; work out what routines/variables do.
3. Append findings to `disasm/symbols.csv` (`addr,name,type,is_hw,note`).
4. Run an `ApplyNames` script → names/comments persist into `tools/ghidra-proj`.
5. Re-export and continue, subsystem by subsystem.

`tools/ghidra-proj` is the durable annotation database. The repo holds the
text exports, `symbols.csv`, and the generated C. **Do not** open the project in
the Ghidra GUI and headless at the same time (Ghidra locks projects).

## Ghidra scripts (`ghidra_scripts/`)

| Script | Purpose |
|---|---|
| `MarkEntries.java` | mark XEX entry points + every address in `entrypoints.csv` (DLIs are reachable only through indirect-jump tables, so Ghidra never finds them itself), then disassemble |
| `ExportListing.java` | dump `listing.txt` (functions + instructions + comments) — the transpiler's input |
| `ApplyNames.java` | apply `symbols.csv` names/comments to the project |
| `DumpHwAccesses.java` | xref dump of `$D000–$D7FF` + page-2 shadow accesses → `docs/hw-access.md` |
| `DumpCallGraph.java` | per-function caller/callee/indirect-jump summary → `disasm/callgraph.txt` |
| `DumpZeroPage.java` | per-address zero-page read/write census → `disasm/zeropage.csv` |

## What is tracked under `disasm/`

Only **`symbols.csv`** — the curated name source of truth. Everything else there
(`rof_mem.bin`, `listing.txt`, `rof_blocks.txt`, `callgraph.txt`, `zeropage.csv`)
is regenerable by the commands above and is git-ignored, as are `tools/ghidra`
(a symlink to the shared install) and `tools/ghidra-proj/`.
