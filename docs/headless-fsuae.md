# Headless FS-UAE measure→fix→verify loop

> Split out of `CLAUDE.md` 2026-08-10 to keep the always-loaded file small.
> **Read this when writing a probe, driving FS-UAE headlessly, or debugging a stale build.**
> `CLAUDE.md` §Build keeps the one-line summary and the `make clean` rule.

Works great — use it instead of guessing. The agent can drive FS-UAE + gdb itself, with no display
interaction, to measure real runtime state. This loop diagnosed several timing/render bugs precisely
where static reasoning kept failing — **measure, don't theorize.**

- **`. ./env.sh` MUST be sourced in the SAME shell command** as the run — it puts BOTH
  `fs-uae` (`~/.local/fs-uae`) and `m68k-amiga-elf-gdb` on PATH. The Bash tool doesn't
  persist a separate `. env.sh`, so `fs-uae` looks "not found" otherwise.
- **`amiga/diag_run.sh [delay]`** = the batch harness: boots `out/RoF.exe` under the
  FS-UAE gdb stub, runs `[delay]` seconds, SIGINTs gdb (breaks its `continue`), runs the
  print commands in **`amiga/diag_timing.gdb`**, and writes everything to
  `amiga/.run/gdb-out.log` (also echoes a filtered tail). Edit `diag_timing.gdb` to print
  whatever globals/`mem[0xNNNN]` you need (a `while $i < N ... end` loop dumps arrays).
  `-g` is always on (CORE_CFLAGS), so all globals are readable by name.
- **Build with probes:** `cd amiga && make clean && make -j4 PROBES=1` (→ `-DROF_FLIGHT_PROBE
  -DROF_TDRAW_PROF`); the `make clean` is mandatory (see the stale-build ⚠ below).
  This is OFF by default — the probes + auto-launch + timing accumulators are now PERMANENT,
  guarded code (committed), not throwaway edits. With probes off, the SDL build + `make validate`
  link cleanly. ⚠ `diag_run.sh`/`diag_sample.sh` need an `out/RoF.exe` built with `PROBES=1`.
- **Auto-launch (reach the launch cinematic with no keypress):** `PlatformAmiga.cpp`'s
  `vbiHandler` (under `ROF_FLIGHT_PROBE`) replicates a real RETURN press —
  `if (g_vbiCount==350) mem[0xD01Fu]=0x06;` (START down) then `=0x07` (up). ⚠ `0x00` (all
  console keys) triggers the DEMO DROID path, NOT a clean START.
- **Probe pattern:** for one-off probes add `volatile` globals under `#ifdef ROF_FLIGHT_PROBE`
  (defs in `PlatformAmiga.cpp`, hooks via the `FP_*` macros in `rof_native.c`), stamp
  `g_vbiCount`/`rof_subclock()` at milestones, and print them from `diag_timing.gdb`. Edit
  `diag_timing.gdb` freely to print whatever globals/`mem[0xNNNN]` you need (a
  `while $i < N ... end` loop dumps arrays); `-g` is always on so all globals are readable.
  Pure-compute stretches show up as `g_vbiCount` deltas (the real VBI ISR bumps the counter).
- ⚠ **A gdb script ABORTS THE WHOLE SOURCED FILE at the first unknown symbol**, so deleting a probe
  global silently kills every committed `.gdb` that still prints it — from that line onward, not just
  that column. `amiga/title_start.gdb` printed nothing but its header for a day after `d6bc58a`
  removed `g_tsRingB2` (fixed 020e045). **When you delete or rename a probe global, grep `amiga/*.gdb`
  for it**, and treat "the trace stopped after the header" as a stale script, not a dead probe.

## ⚠ The stale-build trap (full text)

**Always `make clean && make -j4 PROBES=1` before a headless probe run** (a plain `make`
may have built a non-probe binary in between), and **run `make clean` after toggling `PROBES`
OR after editing a widely-included header** (`RescueOnFractalus.h`, `PlatformAmiga.h`, the
framework headers, …). The Amiga Makefile does **not** track the `PROBES` flag or header
dependencies, so a partial rebuild links **stale object files** against new code. The failure
mode is NOT just a link error — it
often links a **working-but-wrong binary** that runs with **silent runtime breakage**
(e.g. struct layout / member-offset mismatches when a header changed, manifesting as
unrelated corrupted rendering or wrong behaviour). Treat any unexplained runtime
regression right after a header edit or a `PROBES` toggle as a stale build until a clean
rebuild rules it out — don't chase it as a logic bug first.

## Atari reference (ground truth)

Drive the `atari800` debugger in **FIFO mode**. Always `kill -9` stray `atari800`/`fs-uae`
processes (avoids stale copies). RAM dumps: `tools/extract_a8s_ram.py` (RAM base is `0x86`).
