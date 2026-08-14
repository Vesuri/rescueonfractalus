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
- **`amiga/diag_run.sh [delay]`** = the batch harness: boots `out/RoF` under the
  FS-UAE gdb stub, runs `[delay]` seconds, SIGINTs gdb (breaks its `continue`), runs the
  print commands in **`amiga/diag_timing.gdb`**, and writes everything to
  `amiga/.run/gdb-out.log` (also echoes a filtered tail). Edit `diag_timing.gdb` to print
  whatever globals/`mem[0xNNNN]` you need (a `while $i < N ... end` loop dumps arrays).
  `-g` is always on (CORE_CFLAGS), so all globals are readable by name.
- **Build with probes:** `cd amiga && make clean && make -j4 PROBES=1` (→ `-DROF_FLIGHT_PROBE
  -DROF_TDRAW_PROF`); the `make clean` is mandatory (see the stale-build ⚠ below).
  This is OFF by default — the probes + auto-launch + timing accumulators are now PERMANENT,
  guarded code (committed), not throwaway edits. With probes off, the SDL build + `make validate`
  link cleanly. ⚠ `diag_run.sh`/`diag_sample.sh` need an `out/RoF` built with `PROBES=1`.
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

## ⚠ The gdb stub takes READS, not WRITES — inject every input from C

**`set var <a probe global> = 1` from a `.gdb` script does not stick.** Measured 2026-08-14: a script
set a force-fire flag, printed it back on the very next line, and got `0` — while `printf` of other
globals in the same script returned correct live values. So the FS-UAE remote-debugger stub serves
memory *reads* fine and silently drops *writes*: no error, no warning, just a probe that never fires.

That is the reason **every `FORCE_*` harness in `PlatformAmiga.cpp` drives its input from C** — a
build flag plus a `g_vbiCount` window (`FORCE_RETURN`, `FORCE_SELECT`, `FORCE_BOOT_FIRE`, …) — rather
than the more obvious "just poke it from gdb". Follow that pattern: gate the injection on a
`-D` flag, pick the window by vbl, and let the `.gdb` script only *read* the outcome.

Corollary for the readout: **stamp the transition you care about from C, every vblank**, instead of
inferring it from the state at whichever moment the SIGINT lands. `FORCE_BOOT_FIRE`'s
`g_bfLogoEnd`/`g_bfStationEnd` (the vbl each boot cinematic handed off, taken off the live VVBLKI)
are the pattern — they turn "the sample happened to show Standby" into "the hand-off was at vbi 107,
against a no-press control of ~280".

## ⚠ Don't touch the startup-sequence form — and re-verify the harness if you do

`run.sh` / `diag_run.sh` write **two** lines, `cd dh1:` then `RoF`. That looks like it could be
one `dh1:RoF` line. It cannot: tried 2026-08-14 and reverted, because with the path form gdb
resolves this build's symbols against base **`$7500`** instead of the usual **`~$21f8e0`**, and
then **no breakpoint is ever hit** — so every run reads as a hang, in both scripts, for any probe.
Only the symptom is established, not the mechanism: `$7500` is also what appears when the program
never loads at all, so `--remote_debugger_trigger=RoF` most likely stops matching and no segment
base is reported.

Consequence worth knowing: **this harness therefore requires KS 2.0+**, because `cd` is a
ROM-resident Shell builtin only from 2.0 on — a KS 1.3 boot dies with `Unknown command cd` before
loading the game. That is a *harness* limit, not the game's: the game is 1.3-clean, and the
WHDLoad install boots 1.3 through its own slave (`docs/whdload-slave.md`).

The general rule this cost a session to learn: **after editing a shared harness script, re-run a
known-good control on an unchanged binary in the same session.** The tell here was two probe legs
failing at once — including a KS 3.1 *control* that had passed twenty minutes earlier. Had only
the leg under test failed, the edit would have looked innocent and the "1.3 doesn't work"
conclusion would have shipped. And do not widen a load-bearing shared script for a one-off
capability nothing needs.

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
