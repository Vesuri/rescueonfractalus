# Headless FS-UAE measure→fix→verify loop — Rescue on Fractalus specifics

The general harness rules (the shared launcher, sourcing `env.sh` in the same shell command, the
gdb stub serving reads but dropping writes, scripts aborting at the first unknown symbol, the
`cd dh1:` startup-sequence form and the `$7500` base, stale builds, rerunning a known-good control
after editing a shared script) are maintained in AmigaXDev `docs/reference/headless-fsuae.md`.
`AGENTS.md` §Build keeps the one-line summary and the `make clean` rule. **Measure, don't
theorize:** this loop diagnosed timing and render bugs where static reasoning kept failing.

## The harness

- **`amiga/diag_run.sh [delay]`** boots `out/RoF` under the gdb stub, runs `[delay]` seconds,
  SIGINTs gdb, runs the print commands in **`amiga/diag_timing.gdb`** and writes everything to
  `amiga/.run/gdb-out.log` (it also echoes a filtered tail). Edit `diag_timing.gdb` to print the
  globals or `mem[0xNNNN]` you need (a `while $i < N ... end` loop dumps arrays); `-g` is always
  on (`CORE_CFLAGS`), so every global is readable by name. FS-UAE's own log is
  `amiga/.run/fsuae/Cache/Logs/fs-uae.log.txt`.
- **Data:** the default executable loads the ROM itself; `run.sh`, `debug.sh` and `diag_run.sh`
  stage `${ROF_ROM:-../rof.rom}` into the emulated game's `data/rof.rom`. An embedded
  `STANDALONE_DATA=1 ROM=../rof.rom` build remains supported.
- **Probe build:** `cd amiga && make clean && make -j4 PROBES=1 STANDALONE_DATA=1 ROM=../rof.rom`
  (`-DROF_FLIGHT_PROBE -DROF_TDRAW_PROF`). `diag_run.sh` and `diag_sample.sh` need it. The probes,
  auto-launch and timing accumulators are permanent guarded code; with probes off, the SDL build
  and `make validate` link cleanly.
- **Auto-launch:** under `ROF_FLIGHT_PROBE`, `PlatformAmiga.cpp`'s `vbiHandler` replicates a
  RETURN press — `if (g_vbiCount==350) mem[0xD01Fu]=0x06;` (START down), then `0x07` (up). ⚠ `0x00`
  (all console keys) takes the DEMO DROID path, not a clean START.
- **Probe pattern:** add `volatile` globals under `#ifdef ROF_FLIGHT_PROBE` (definitions in
  `PlatformAmiga.cpp`, hooks through the `FP_*` macros in `rof_native.c`), stamp
  `g_vbiCount`/`rof_subclock()` at milestones and print them from `diag_timing.gdb`.
  Pure-compute stretches show up as `g_vbiCount` deltas. When you delete or rename a probe global,
  grep `amiga/*.gdb` (`amiga/title_start.gdb` printed only its header for a day after `d6bc58a`).
- **Inputs come from C:** every `FORCE_*` harness in `PlatformAmiga.cpp` (`FORCE_RETURN`,
  `FORCE_SELECT`, `FORCE_BOOT_FIRE`, …) is a `-D` flag plus a `g_vbiCount` window; the `.gdb` only
  reads. Stamp the transition you care about from C every vblank (`FORCE_BOOT_FIRE`'s
  `g_bfLogoEnd`/`g_bfStationEnd`: "hand-off at vbi 107, against a no-press control of ~280").
- **Startup-sequence:** `run.sh`/`diag_run.sh` write `cd dh1:` then `RoF`; a single `dh1:RoF` line
  was tried and reverted. The harness therefore needs KS 2.0+ (`cd` is a ROM builtin from 2.0); the
  game itself is 1.3-clean and the WHDLoad install boots 1.3 ([whdload-slave.md](whdload-slave.md)).

## The stale-build trap

The Amiga Makefile tracks neither the `PROBES` flag nor header dependencies. `make clean` before
every probe build and after editing a widely included header (`RescueOnFractalus.h`,
`PlatformAmiga.h`, the framework headers, …); a partial rebuild links a working-but-wrong binary
(struct offsets from the old header). Treat an unexplained regression right after a header edit
or a `PROBES` toggle as a stale build until a clean rebuild rules it out.

## Atari reference (ground truth)

Drive the `atari800` debugger in **FIFO mode**. Stop a stale `atari800` you started by its pid,
never by name. RAM dumps: `tools/extract_a8s_ram.py` (RAM base `0x86`).
