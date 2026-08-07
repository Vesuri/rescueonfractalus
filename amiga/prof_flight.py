#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# Flight per-function PC-profile analyzer for Rescue on Fractalus (Amiga).
#
# Reads the PC samples in amiga/.run/gdb-sample.log (produced by diag_sample.sh),
# keeps only the in-FLIGHT samples (VVBLKI=$4FF5), buckets them into logical
# function groups, and prints a table with a % share plus two status columns:
#
#   asm?     — is the hot code hand-written m68k asm (a .s label)?
#              ✓ = all asm · partly = mixed asm+C · no = no asm
#   native?  — for the NON-asm part, how converted is it (the clean-twin rules)?
#              asm            the group is pure asm (rules N/A)
#              clean          native twin in rof_native.c, no 6502 idioms left
#              partial(6502)  native twin but still has goto / LDA()/ADC()-style macros
#              TRANSPILED ✗   still the raw 6502-emulation body in rof_gen.c → convert it
#              C++ platform   Amiga backend C++ (native by nature; not a 6502 twin)
#              lib/ROM        memset / Kickstart / IRQ
#
# The clean-twin rules the `native?` column checks against:
#   1. be a native function (twin in rof_native.c, not the rof_gen.c transliteration)
#   2. proper C, no 6502 idioms (no LDA()/ADC()/ROL macros, no cpu-reg arithmetic)
#   3. a _core() helper for real argument passing where needed
#   4. no goto; locals instead of mem[] scratch
#   5. mem.h names for memory locations that have them
#   6. no bus_write calls that do nothing on the Amiga
#   7. comments describe intent, not the opcodes
#   8. unnamed/badly-named funcs+cells recorded in docs/rename.md
# (Rules 3/5/6/7/8 need human judgement; this tool auto-checks 1/2/4 — the
#  mechanical ones — and flags the rest as "clean" vs "partial" so you know
#  which twins still deserve a manual pass.)
#
# ─── FULL RECIPE (how to get a fresh profile) ──────────────────────────────
#   cd amiga
#   . ./env.sh                                   # fs-uae + m68k-amiga-elf-gdb on PATH
#   make clean && make -j4 PROBES=1 PROFILE_NORING=1
#       PROBES        → headless auto-launch (reaches flight with no keypress)
#       PROFILE_NORING→ compile out the RF_RING rescue-debug ring, whose per-frame
#                       rfPlaneSum scans otherwise eat ~half the samples
#   ./diag_sample.sh 20 0.3 250                  # warmup 20s (into steady flight),
#                                               # then 250 SIGINT PC-samples @ 0.3s
#   python3 prof_flight.py                       # this script (reads .run/gdb-sample.log)
#       python3 prof_flight.py --by-symbol       # per-symbol breakdown instead of groups
#       python3 prof_flight.py --drill FUNC      # resolve a function's hot PCs to source lines
#
# prof_flight.sh runs the build + sample + analyze steps for you.
# ---------------------------------------------------------------------------
import re, os, sys, collections, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
LOG  = os.path.join(HERE, ".run", "gdb-sample.log")
ELF  = os.path.join(HERE, "out", "RoF.elf")

# ─── logical grouping of raw symbols ───────────────────────────────────────
# Each rule maps a symbol (exact name or prefix) to a display group.  Order
# matters: first match wins.  Add rows here as the code grows.
def group_of(sym):
    # ⚠ This bucket's SHARE IS A SAMPLER ARTIFACT — do not treat it as reclaimable time.
    # Identified 2026-08-05: essentially every sample in it is the SAME instruction,
    # $F811F8 = the first instruction of Kickstart's level-3 autovector stub
    # (`movem.l d0-d1/a0-a1/a5-a6,-(sp)`), always with the same supervisor sp.  FS-UAE's
    # gdb stub takes its SIGINT break right after a pending interrupt has been dispatched,
    # so the sample lands on the vector's entry PC — the count tracks how many interrupts
    # were taken, not how long ROM ran.  Proof: taking the VERTB vector over removed
    # exec's chain walker AND the three OS VERTB servers (measured 846us -> 52us per
    # firing with amiga/irq_probe.gdb) and this bucket only moved 8.0% -> 7.3%.
    # Measure interrupt-dispatch cost with irq_probe.gdb's beam probe, never from here.
    if sym in ("<unresolved/ROM>",):                 return "exec IRQ-entry stub (sampler artifact)"
    # "ras_sp3*"/"ras_sp4*" are the 2026-08-05 restructure's straight-line span-3/span-4 leaf
    # groups; "done" is the rasterizer's shared writeback/epilogue label.  Without these rows
    # they show up as separate "misc:" lines and the rasterizer bucket reads ~half its real cost.
    # NB "load_far" is a TerrainSubdivideAssembler.s label, NOT a rasterizer one (the old
    # rasterizer had a same-named helper; the 2026-08-05 restructure dropped it, along with
    # the `draw`/`draw_ret` subroutine that DRAWDOT now inlines).  Listing it here cost the
    # subdivide bucket 1.6 points and handed them to the rasterizer.
    if sym.startswith(("ph1_", "ph2_", "ras_sp")) or sym in (
        "done", "terrain_column_rasterize_core_asm", "terrain_column_rasterize_span"):
                                                     return "rasterizer (ph1/ph2 + leaf draw)"
    if sym.startswith("sd_") or sym in ("submid", "push_mid", "load_far", "load_span",
                                        "terrain_subdivide_column_core_asm"):
                                                     return "subdivide (sd_*)"
    if sym.startswith("tf_") or sym == "terrain_frame_setup_core_asm":
                                                     return "terrain_frame_setup (tf_*)"
    if sym.startswith("ep_") or sym == "RescueOnFractalus::renderFlightDirect()":
                                                     return "renderFlightDirect (+edge-plot asm)"
    # NOT a busy-wait — measured 2026-08-06 (blit_shape.gdb): the frame-sync vblank spin is
    # 682 ticks over 3001 vbi = 0.07% of flight, 1 tick per call (the deferred-flip scheme
    # normally skips it entirely).  ~21/22 of this bucket's samples resolve to the spin's
    # source line, but that is GCC line attribution for the surrounding orchestration, not
    # time in the loop.  Do not mine this bucket for "idle time" — there is none.
    if sym == "PlatformAmiga::renderFrame()":        return "renderFrame (per-frame orch.)"
    if sym in ("game_main_loop", "game_main_loop_body"):
                                                     return "game_main_loop (flight-loop orch.)"
    if sym == "terrain_draw_objects":                return "object draw-order loop"
    if sym == "project_terrain_points_core_asm":     return "project_terrain_points"
    if sym in ("draw_dot", "terrain_plot_object", "terrain_plot_object_a",
               "terrain_plot_object_b", "raster_scaled_object", "plot_clipped_pixel",
               "terrain_clip_row_top", "terrain_point_distance",
               "set_plot_mask_and_halve_step", "terrain_plot_pixel_core"):
                                                     return "objects (dots / plot_object)"
    # Enemy gun-emplacement / player bolt: the per-pixel wedge scatter into g_flightDotPlane.
    # Only ever non-zero under a COMBAT=1 profile — a quiet run never fires a bolt at anything,
    # which is exactly why this class went unmeasured for so long.
    if sym.startswith(("laser_dot_", "plot_scanline_")) or sym in ("game_state_update",):
                                                     return "enemy fire / bolt plot"
    if sym.startswith(("flush_paula", "pokey_", "sfx_", "ring_push")) or sym in (
               "PlatformAmiga::noiseTick()", "voice_engine_tick"):
                                                     return "audio (POKEY->Paula, SFX ring)"
    if sym.startswith("RescueOnFractalus::build") or sym.startswith("Sprite::"):
                                                     return "sprites (P3/scope/scanner build)"
    if sym in ("rof_beam_line", "rof_subclock", "rof_time_now"):
                                                     return "[probe overhead — PROBES-only]"
    if sym == "memset":                              return "memset (clears)"
    return "misc: " + sym

# ─── per-symbol classification (grep the source tree; cached) ──────────────
_src_cache = {}
def _read(path):
    if path not in _src_cache:
        try:    _src_cache[path] = open(path, errors="replace").read()
        except FileNotFoundError: _src_cache[path] = ""
    return _src_cache[path]

def _asm_files():
    d = os.path.join(ROOT, "src", "platform", "amiga")
    return [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".s")]

# 6502-idiom markers that mean a native twin is not fully cleaned (rules 2/4).
_IDIOM = re.compile(r"\bgoto\b|\b(?:LDA|LDX|LDY|STA|STX|STY|ADC|SBC|CMP|CPX|CPY|"
                    r"ROL|ROR|ASL|LSR|ORA|EOR|AND|BIT|INC|DEC)\(")

def _twin_body(name):
    """Return the C body of `name` from rof_native.c, or None."""
    txt = _read(os.path.join(ROOT, "src", "gen", "rof_native.c"))
    m = re.search(r"^(?:void|static\s+\w[\w ]*?)\s+" + re.escape(name) + r"\s*\([^;]*\)\s*\{",
                  txt, re.M)
    if not m: return None
    i = m.end(); depth = 1
    while i < len(txt) and depth:
        c = txt[i]
        if c == "{": depth += 1
        elif c == "}": depth -= 1
        i += 1
    return txt[m.end():i]

def classify(sym):
    """→ (kind, native_label)  kind ∈ asm|clean|partial|transpiled|cpp|lib|rom"""
    if sym == "<unresolved/ROM>":            return ("rom", "lib/ROM")
    if sym == "memset":                      return ("lib", "lib/ROM")
    base = sym.split("(")[0]
    if "::" in base:                         return ("cpp", "C++ platform")
    # hand-asm label?
    for f in _asm_files():
        if re.search(r"^" + re.escape(base) + r":", _read(f), re.M):
            return ("asm", "asm")
    # native twin in rof_native.c?
    body = _twin_body(base)
    if body is not None:
        return ("partial", "partial(6502)") if _IDIOM.search(body) else ("clean", "clean")
    # still the raw transliteration in rof_gen.c?
    gen = _read(os.path.join(ROOT, "src", "gen", "rof_gen.c"))
    if re.search(r"^void\s+" + re.escape(base) + r"\s*\(void\)\s*\{", gen, re.M):
        return ("transpiled", "TRANSPILED ✗")
    return ("rom", "lib/ROM")

# ─── sample parsing ────────────────────────────────────────────────────────
def load_flight_samples():
    if not os.path.exists(LOG):
        sys.exit(f"no sample log at {LOG} — run ./diag_sample.sh first (see recipe in this file)")
    lines = open(LOG, errors="replace").read().splitlines()
    out, i = [], 0
    while i < len(lines):
        m = re.match(r"^S\d+ .*VVBLKI=([0-9a-f]+).*pc=0x([0-9a-f]+)", lines[i])
        if m:
            symline = lines[i + 1].strip() if i + 1 < len(lines) else ""
            out.append((m.group(1), m.group(2), symline)); i += 2
        else:
            i += 1
    return [s for s in out if s[0] == "4ff5"]   # VVBLKI=$4FF5 = in flight

def fname(symline):
    if "in ?? ()" in symline or "No symbol" in symline or not symline:
        return "<unresolved/ROM>"
    m = re.match(r"^(.*?)\s+\+\s+\d+\s+in section", symline) or \
        re.match(r"^(.*?)\s+in section", symline)
    return m.group(1) if m else symline

# ─── asm?/native? aggregation for a set of symbol→count ────────────────────
def summarize(counts):
    tot = sum(counts.values())
    kinds = collections.Counter()
    natl  = {}
    for s, c in counts.items():
        k, lbl = classify(s); kinds[k] += c; natl[k] = lbl
    asm = kinds.get("asm", 0)
    asm_col = "✓" if asm == tot else ("partly" if asm else "no")
    nonasm = [k for k in kinds if k != "asm"]
    if not nonasm:
        nat = "asm"
    else:
        prio = ["transpiled", "partial", "clean", "cpp", "lib", "rom"]
        pick = min(nonasm, key=lambda k: prio.index(k) if k in prio else 99)
        nat = natl[pick]
        if len([k for k in kinds if k in ("asm","clean","partial","transpiled","cpp")]) > 1:
            nat += " (mixed)"
    return asm_col, nat

def main():
    args = sys.argv[1:]
    flight = load_flight_samples()
    tot = len(flight)
    if "--drill" in args:
        # The log's pc= values are RUNTIME-relocated; offline gdb/the ELF use link
        # addresses.  The stable key is the OFFSET (" + N in section") from the live
        # info-symbol line, added to the symbol's ELF base.
        target = args[args.index("--drill") + 1]
        offs = []
        for _, _, sl in flight:
            if fname(sl) != target: continue
            m = re.search(r"\+\s+(\d+)\s+in", sl)
            offs.append(int(m.group(1)) if m else 0)
        print(f"{target}: {len(offs)} flight samples")
        try:
            out = subprocess.run(["m68k-amiga-elf-objdump", "-tC", ELF],
                                 capture_output=True, text=True).stdout
        except FileNotFoundError:
            sys.exit("m68k-amiga-elf-objdump not found — source ./env.sh first")
        base = None
        for ln in out.splitlines():
            if ln.rstrip().endswith(" " + target) or ln.rstrip().endswith("\t" + target):
                base = int(ln.split()[0], 16); break
        if base is None:
            sys.exit(f"could not find ELF symbol base for {target}")
        # addr2line -ife gives the INLINE stack (innermost first): func1 / file1:line1 /
        # func2 / file2:line2 ...  Big -O3 apexes (game_main_loop, terrain_draw_frame) inline
        # their leaves, so the innermost frame is the real hot function — attribute to it, and
        # show the enclosing inline chain so you can see "zero_run <- clear_terrain_column_core".
        loc = collections.Counter()
        for off in offs:
            out = subprocess.run(["m68k-amiga-elf-addr2line", "-ife", ELF, "0x%x" % (base + off)],
                                 capture_output=True, text=True).stdout.strip().splitlines()
            funcs = [out[j] for j in range(0, len(out), 2)]
            flines = [out[j] for j in range(1, len(out), 2)]
            inner = funcs[0] if funcs else "?"
            line0 = flines[0].split("/")[-1] if flines else "?"
            chain = " <- ".join(funcs[:3])
            loc[(inner, line0, chain)] += 1
        for (fn, l, chain), c in loc.most_common():
            print(f"  {c:3d}  {fn} @ {l}   [{chain}]")
        return

    by_sym = collections.Counter(fname(sl) for _, _, sl in flight)
    if "--by-symbol" in args:
        print(f"=== flight PC profile — by symbol ({tot} samples) ===")
        print(f"{'%':>5}  {'asm?':<7}{'native?':<16}symbol")
        for s, c in by_sym.most_common():
            a, n = summarize({s: c})
            print(f"{100.0*c/tot:5.1f}  {a:<7}{n:<16}{s}")
        return

    groups = collections.defaultdict(collections.Counter)
    for s, c in by_sym.items():
        groups[group_of(s)][s] += c
    rows = []
    for g, counts in groups.items():
        c = sum(counts.values()); a, n = summarize(counts)
        rows.append((c, g, a, n))
    rows.sort(reverse=True)
    print(f"=== Flight per-function profile (ring off, {tot} samples) ===")
    print(f"{'%':>5}  {'asm?':<7}{'native?':<24}function")
    for c, g, a, n in rows:
        print(f"{100.0*c/tot:5.1f}  {a:<7}{n:<24}{g}")

if __name__ == "__main__":
    main()
