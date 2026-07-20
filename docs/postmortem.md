# Postmortem — binary-only 6502→C→Amiga port process

Retrospective on the *Rescue on Fractalus!* port pipeline (Ghidra → transliterate 6502 → C →
abstract hardware → platform backends), written to carry forward into a **future binary-only
port**. This is not a to-do for *this* project — it is a "what I'd do differently next time"
distilled from the decision trail in `CLAUDE.md`, the memory topic files, and git history.

> One-sentence version: **the biggest wins are all "build the discovery and validation
> infrastructure exhaustively up front instead of growing it reactively."** Complete
> entry-point seeding, a harness that cannot pass vacuously or hide endianness, and
> real-hardware measurement from frame one. Transpiler-output quality is a close second
> because it compounds across every regen.

Findings are ordered by leverage. The front end (Ghidra + transpiler) is where the leverage is.

---

## 1. Ghidra / control-flow discovery

### 1.1 Do an exhaustive indirect-dispatch sweep BEFORE generating any C  ★ highest leverage
The single most recurring pain was Ghidra silently missing code that is reachable only through
indirect jumps. The DLI chains (`$6DBB`, `$6DCF`), the event dispatcher, and the RTS-trick
dispatch tables were **discovered piecemeal over the life of the project**. The
`ghidra_scripts/entrypoints.csv` convention ("seed each DLI addr as a Ghidra entry point so it
persists in `listing.txt`") and the working-convention rule "record a newly-found DLI the moment
you find it" are both *scars* from getting burned repeatedly.

The set of dispatch patterns in a binary is finite and enumerable. Next time, write a one-time
analysis pass **before** the first C is generated that finds and seeds as entry points:
- every `JMP ($xxxx)` / computed-jump / RTS-dispatch idiom and the word tables they index;
- all hardware/OS vectors as roots — NMI/IRQ/RESET (`$FFFA-$FFFF`) plus platform shadow vectors
  (Atari: VDSLST `$0200`, VVBLKI/VVBLKD, VIMIRQ, etc.).

Front-loading this makes `listing.txt` complete on day one instead of a moving target, and
prevents the failure mode "I reasoned statically about a handler Ghidra never disassembled."

### 1.2 First-pass behavioral naming as a dedicated phase, not a trickle
`docs/rename.md` accumulated a backlog because many functions carried wrong/auto names for a long
time. On a binary-only RE project **the function names are your map** — every wrong name taxes
every subsequent reasoning step. `symbols.csv` → transpiler already made renames cheap, so the
infrastructure was right; the timing was reactive. Next time, spend a concentrated early pass
giving rough-but-directionally-correct names (`dli_colpf_writer`, `terrain_*`, …) before deep
work begins.

---

## 2. Transpiler quality

### 2.1 Make the transpiler emit clean, near-idiomatic C BEFORE mass-generating
The liveness-gated peephole folding (direct assignments, N/Z-flag elision, the `−1490` line
shrink of `rof_gen.c`) landed *late*. Every regen and every human read between "first generation"
and "peephole added" paid the tax of uglier, slower, harder-to-diff C.

The transpiler is a force multiplier: one improvement upgrades the entire corpus at once. Treat
"emit clean C" (liveness-gated flag elision, named `mem.h` accesses from `symbols.csv`, folded
load→store idioms) as a **prerequisite milestone**, not a later optimization. Cleaner generated C
also directly reduces how many functions ever need a hand-written native twin — which is the
expensive downstream work.

---

## 3. Validation — where I'd change the most

The nastiest bug class on this project was **passes `make validate` green, breaks at runtime.**
All three instances below are documented in `feedback-native-twin-validation-gaps`:

- **Vacuous green** — a `VALIDATE_FUNC` with no registered fixture prints `PASS` with zero
  comparisons run.
- **Endianness** — little-endian `mem[]` aliased as `uint16_t*`/`uint32_t*` reads correct on the
  little-endian SDL host but byte-swapped on the big-endian Amiga; `make validate` stays green
  while the Amiga renders garbage.
- **Live exit registers** — a twin's exit register consumed by a transpiled caller is a real
  contract, but the harness hides it as "incidental cpu diff" (the `audio_timer_setup` exit-A=0
  bug → level-select construction detour).

### 3.1 Make vacuous / hidden-failure passes structurally impossible
Bake the countermeasures into the harness from the **first** twin, not after each bites:
- **Fail (not warn)** when a `VALIDATE_FUNC` has no registered fixture — no silent
  zero-comparison passes.
- **Run the differential big-endian as well as host** (real or emulated target in CI), or at
  minimum a linter that flags any `uint16_t*`/`uint32_t*` alias of `mem[]`, so endianness cannot
  hide behind a green host run.
- **Auto-randomize inputs, including the gating bytes**, rather than hand-authoring fixtures that
  silently leave whole branches untested.

### 3.2 Stand up the reference + measurement loops as first-class infra at project start
Both the headless `atari800` reference-capture loop and the headless FS-UAE + gdb beam-probe loop
retroactively became "the thing that diagnosed timing/render bugs precisely where static reasoning
kept failing." Yet both read as tools built *reactively* once already stuck, and the rule
"validate against the 6502 + atari800, NOT PlatformSDL (SDL is an approximation)" is a *learned*
lesson rather than a starting assumption. Next time build both harnesses (and the DL-analyzer
equivalent) **first**, so "measure, don't theorize" is the default from frame one.

---

## 4. Scope / performance sequencing

### 4.1 Profile an end-to-end slow skeleton on real hardware before committing the approach
The retired **"algorithmic floor / 50 FPS impossible without an algorithm change"** conclusion
(2026-06-29) was later disproven by hand-asm — the ceiling was GCC, not the algorithm. That wrong
turn cost real time and nearly steered scope. A crude full-pipeline skeleton profiled **on the
real A500 early** would have shown terrain rasterize / project / subdivide as the ~167 ms hot core
up front — telling you exactly which handful of functions ever needed the native→asm treatment,
and that the algorithm was fine.

General principle for a fixed-hardware target: get *something* running end-to-end on the real
machine as early as possible. The target's constraints are the whole game, and armchair reasoning
about them was repeatedly wrong here.

---

## 5. Architecture — write the faithfulness-seam rule down first

The `rof_native.c` (faithful, validated, linked into both backends) vs `rof_native_amiga.cpp`
(genuinely Amiga-only, lossy, unvalidated) split is good architecture — but the *rule* for which
side a function lands on was clarified only after churn. "A faithful, pure-`mem[]` routine that
merely needs a small Amiga variation stays a validated twin in `rof_native.c` with the variation
under `#ifdef ROF_PLATFORM_AMIGA`" is a call you make hundreds of times. Write that decision rule
as the first architectural doc, before the first twin.

---

## Checklist to carry into the next binary-only port

- [ ] Enumerate + seed ALL entry points first: indirect-jump/RTS-dispatch tables and every
      hardware/OS/shadow vector. Make disassembly complete before generating C.
- [ ] One concentrated behavioral-naming pass up front; single source-of-truth symbol file feeding
      the transpiler.
- [ ] Invest in transpiler output quality (clean C, flag elision, named memory) *before*
      mass-generating — it compounds over every regen and shrinks the twin backlog.
- [ ] Validation harness from twin #1: fixture-or-fail (no vacuous green), cross-endian
      differential, auto-randomized inputs incl. gating bytes, exit-register contracts explicit.
- [ ] Build the emulator reference-capture loop AND the real-hardware measurement loop as
      first-class infra day one; never trust the dev-host backend as ground truth.
- [ ] Stand up a slow end-to-end skeleton on real hardware early and profile it before choosing
      what to optimize; don't declare an "algorithmic floor" without a hand-asm datapoint.
- [ ] Write the faithful-twin vs platform-specific seam rule down before the first twin.

---

## Next target: Revs (BBC Micro → Amiga, 1:1)

The next port is **Revs** — Geoff Crammond's 1985 BBC Micro F1 sim (real vehicle dynamics + true
3D Silverstone), targeting the **four-track expansion** version. The BBC executable is believed to
be **the same binary across all tracks** — it loads track data from disc — so the port is one
engine + swappable data, not four ports. Destination is still **Amiga**, still **1:1 faithful
preservation first**. Revs **never received an Amiga port**, so this is genuine preservation.

The port lives in a **new repo**, copying tooling from this one where it applies.

### What transfers cleanly from this project
- The whole **transpiler front end** — it's 6502, so the transliteration model, `mem.h` named-access
  scheme, the native-twin seam + `make validate` harness, and the `#ifdef ROF_PLATFORM_AMIGA`
  faithfulness split are all reusable.
- The **Amiga backend** knowledge: copper discipline, the 50 FPS / 20 ms A500 budget, hand-asm for
  the hot loop, VBI-ISR pointer-swap rule.
- Every checklist item above — and #1 (exhaustive entry-point seeding) bites *harder* here: the BBC
  MOS leans on indirect vectors, and Revs uses self-modifying code and dispatch tricks. Enumerate
  every dispatch/vector before generating a line of C.

### What is genuinely new (build this first)
- **No atari800.** The reference/ground-truth loop must be rebuilt for the BBC. Decision (2026-07-20,
  after checking capabilities — see sources below):
  - **jsbeeb (Matt Godbolt) = the scriptable cycle-diff ORACLE.** Node/headless by design, with a
    proven pedigree in automated cycle-accurate BBC regression testing. Build the deterministic
    differential harness ("validate the physics math cycle-for-cycle") on this.
  - **b2 (Tom Seddon) = interactive accurate debugger + HTTP automation surface.** The **debug
    build** exposes an HTTP API on `localhost:48075` (curl-driven): `reset` (named config/MOS),
    `mount`/`run` a disc image (matters for Revs's disc-loaded track data), `paste` input, and
    **`peek`/`poke`** memory ranges — the direct analogue of the atari800 FIFO boot/poke/dump loop.
    Caveats: b2 is a **GUI app (no true headless mode** — run under Xvfb, or use the separate
    libretro core which drops the HTTP API); and the HTTP API surface seen is peek/poke/reset/run/
    mount/paste — **breakpoints / register reads / single-step / cycle counts appeared GUI-only**,
    not over HTTP. Re-check a current b2 version: if the HTTP API gains registers/breakpoints, b2
    could carry BOTH roles like atari800 did.
  - **MAME bbc driver = fully-headless Lua-scriptable fallback** (`-video none`, full breakpoint/
    register/step debugger) if scripted break-and-inspect without a framebuffer is needed — but its
    BBC accuracy is historically less trusted than the dedicated emulators, which matters for a
    physics-exact port. Reserve, don't lead with it.
- **Video hardware is different.** BBC = 6845 CRTC + Video ULA (palette/mode tricks), NOT ANTIC/GTIA.
  The `/atari-dl-analyzer` skill needs a CRTC+ULA re-tool for scene composition analysis.
- **The physics hot path replaces the terrain rasterizer** as the eventual hand-asm target on the
  A500. Different loop shape, same lever (control the regs, force `(a0)+`, verify with an in-process
  differential, not cross-run).

Sources for the emulator decision:
- b2 README — https://github.com/tom-seddon/b2
- b2 Debug-version.md (HTTP API: reset/peek/poke/mount/run/paste) —
  https://github.com/tom-seddon/b2/blob/master/doc/Debug-version.md
- b2-libretro (headless core variant) — https://github.com/zoltanvb/b2-libretro
- jsbeeb — https://github.com/mattgodbolt/jsbeeb
