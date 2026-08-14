/* Native reimplementations of selected 6502 routines.
 *
 * Each function here REPLACES the transliterated version: its address is listed
 * in transpile.py's VALIDATE_FUNCS, so the transpiler emits the faithful
 * transliteration under a `__t6502` suffix and leaves the plain name to us.
 * All call sites invoke the plain name, so these are live in the real build.
 *
 * The contract is the OBSERVABLE MACHINE STATE at the call boundary — primarily
 * mem[] (zero-page + RAM). tools/validate_native.c proves each function here is
 * equivalent to its `__t6502` twin across a large random input corpus before it
 * ships. This is the regen-safe seam for the eventual Amiga reimplementation:
 * everything not listed in VALIDATE_FUNCS stays transliterated and regenerable.
 */
#include <stdint.h>
#include "../cpu/cpu.h"
#include "../cpu/bus.h"  /* bus_read/bus_write + ZP_IND_Y for indirect bitmap access */
#include "rof_decl.h"   /* declarations for transpiled routines native code calls */
#include "rof_native.h" /* typed cores shared with the hand-written Amiga ports */
#include "../cpu/m68k_math.h" /* rof_mulu16/rof_divu16/... hardware 16-bit mul/div */
#include "../platform/platform_c.h" /* platform_tick_vbi/render_frame/poll_events for the apex spin-waits */
#define ROF_MEM_ALIASES /* bare lvalue aliases: flight_mode_state == mem[MEM_flight_mode_state] */
#include "mem.h"        /* MEM_<name> offsets + bare aliases (symbols.csv var rows) */

/* ROF_MEM_VIEW — the qualifier for a LOCAL POINTER VIEW of mem[] inside a hot routine.
 *
 * Several routines here hoist `mem`-derived base pointers into locals so the 68000 gets
 * (d16,An) instead of absolute-long.  They were written when mem[] was still `volatile`
 * everywhere and so declared the views `volatile` too.  Since ROF_MEM_NONVOLATILE
 * (docs/flight-perf-log.md §20.2) mem[] itself is plain on the Amiga, and a volatile VIEW
 * puts the whole tax back on exactly the hottest loop: GCC must re-materialise every table
 * lookup as `lea (0,An,Dn.l),aM` + `move.b (aM),Dn` (20 cycles) instead of the single
 * `move.b (0,An,Dn.l),Dn` (14), cannot walk a pointer over a sequential table, and cannot
 * emit the memory-to-memory `move.b d16(An),d16(Am)` form for a mem[]->mem[] copy.
 *
 * The soundness argument is mem[]'s own (§20.2): nothing preempts these routines on the
 * Amiga, and every callee is an opaque call, i.e. already a reload barrier — so a plain view
 * performs exactly the same accesses in the same order.  The SDL host keeps `volatile`,
 * matching its own mem[] declaration, because there the VBI really is another thread.
 *
 * `make validate FN=<fn> MEMVIEW=1` compiles the Amiga shape on the HOST so the oracle can
 * prove it byte-identical (⚠ TEST ONLY — sound in the single-threaded validate harness,
 * NOT in the threaded SDL game).  Same pattern as FCIBASE. */
#if defined(ROF_PLATFORM_AMIGA) || defined(ROF_MEM_VIEW_HOSTTEST)
#define ROF_MEM_VIEW
#else
#define ROF_MEM_VIEW volatile
#endif

/* ROF_MEMBASE — the mem[] BASE-REGISTER fold, generalised.
 *
 * Dropping `volatile` (ROF_MEM_NONVOLATILE) only ALLOWS GCC to use a base register; it still
 * emits absolute-long for a plain constant subscript (`move.b (mem+$25).l,d3` = 16 cyc / 6
 * bytes where `move.b $25(An),d3` is 12 / 4) because its m68k cost model prices the two modes
 * alike.  Laundering `mem` through an empty asm emits no instruction but makes GCC lose the
 * knowledge that the pointer IS `mem`, so it must keep it in a callee-saved address register
 * and every access folds to (d16,An).  First proven on flight_control_integrate (038786d,
 * 4678 -> 4102 bytes, integ -5.7%); see the long note there for the full argument, including
 * why `&mem` escaping into the asm is what keeps the alias oracle honest for helpers GCC
 * inlines into the body.
 *
 * Usage — the `#define mem` has to be textual, so it cannot hide inside the macro:
 *     static void f(void) {
 *         ROF_MEMBASE_DECL(mb);
 *     #ifdef ROF_MEMBASE
 *     #define mem mb
 *     #endif
 *         ... body ...
 *     #ifdef ROF_MEMBASE
 *     #undef mem
 *     #endif
 *     }
 * Every `mem[...]` and every symbols.csv lvalue alias folds, because the alias expands to
 * `mem[...]` and is rescanned.
 *
 * ⚠ Only worth it where the body makes MANY mem[] accesses: the base costs one callee-saved
 * address register, and in a register-starved function that buys a spill instead.  Check the
 * .text size both ways before keeping one.
 * ⚠ Displacements are signed 16-bit, so this is free only for addresses below $8000.  Above
 * that GCC just picks another mode — correct, not free.
 * `make validate FN=<fn> MEMBASE=1` compiles the same transformation on the host (portable "r"
 * instead of m68k's "a") so the oracle can prove the macro rescan reached every use. */
#if defined(ROF_PLATFORM_AMIGA) && !defined(ROF_NO_MEMBASE)
#define ROF_MEMBASE 1
#define ROF_MEMBASE_REG "a"          /* m68k: force an ADDRESS register */
#elif defined(ROF_MEMBASE_HOSTTEST)
#define ROF_MEMBASE 1
#define ROF_MEMBASE_REG "r"          /* host equivalence test: any register */
#endif
#ifdef ROF_MEMBASE
#define ROF_MEMBASE_DECL(nm) uint8_t* nm; __asm__ ("" : "=" ROF_MEMBASE_REG (nm) : "0"((uint8_t*)mem))
#else
#define ROF_MEMBASE_DECL(nm) ((void)0)
#endif

/* Display hardware register writes (ANTIC/GTIA/PMG, $D000-$D4xx) are DEAD on the Amiga — the
 * copper owns the display and bus_write already skips non-POKEY hardware there — but LIVE on
 * SDL, whose hwWrite() feeds the reference renderer (displayListPtr / colHW[] / hposP[] / ...).
 * HW_WRITE keeps them for SDL and compiles them out on the Amiga.  (POKEY $D2xx audio writes
 * and $02xx OS-shadow writes stay as bus_write — both are live on the Amiga.) */
#ifdef ROF_PLATFORM_AMIGA
#define HW_WRITE(addr, val) ((void)0)
#else
#define HW_WRITE(addr, val) bus_write((addr), (val))
#endif

/* game_main_loop's restart trampoline (see the RESTART TRAMPOLINE note below) needs setjmp/
 * longjmp.  The Amiga m68k-gcc toolchain provides __builtin_setjmp; Apple clang 21 (arm64)
 * dropped it ("not supported for the current target"), which breaks every host rebuild of this
 * file, so the host validate/SDL builds use ISO <setjmp.h> instead — identical restart
 * semantics.  The Amiga path is unchanged (still __builtin, byte-for-byte). */
#ifdef ROF_PLATFORM_AMIGA
typedef void *rof_jmp_buf[5];
#define ROF_SETJMP(b)     __builtin_setjmp(b)
#define ROF_LONGJMP(b, v) __builtin_longjmp((b), (v))
#else
#include <setjmp.h>
typedef jmp_buf rof_jmp_buf;
#define ROF_SETJMP(b)     setjmp(b)
#define ROF_LONGJMP(b, v) longjmp((b), (v))
#endif

/* Optional flight/init timing probes (Amiga autoflight only; -DROF_FLIGHT_PROBE, i.e.
 * `make PROBES=1`).  Accumulate sub-frame timings of game_main_loop's flight init + the
 * per-frame phase split into the g_* globals (defined in PlatformAmiga.cpp, read from the
 * gdb stub via amiga/diag_timing.gdb).  Desktop/release builds compile these to no-ops, so
 * none of the probe globals are referenced there (keeps the SDL link clean).  The clock is
 * rof_subclock() = g_vbiCount*313 + beam_line (sub-frame resolution; see PlatformAmiga.cpp). */
#ifdef ROF_FLIGHT_PROBE
extern unsigned long rof_subclock(void);
extern volatile unsigned long g_probeDispSetup, g_probeGameInit, g_probeIntro,
    g_probeInitTotal, g_probeRowAddr;
extern volatile unsigned long g_iterMax, g_iterLast, g_iterPostDs,
    g_fSetup, g_fClear, g_fDraw, g_fColl, g_fState, g_fEnemy, g_clFrameTicks;
extern volatile unsigned short g_probeFlightVbi, g_iterCount, g_iterMaxAt;
extern void rof_ds_mile(int i);
#define DS_MILE(i) rof_ds_mile(i)
/* Cumulative flight-VBI ISR beam-lines (bumped by flight_vbi_native).  Subtracted in
 * FP_TIME so a phase's bucket excludes ISR firings that land in its window — otherwise
 * the ~5 ms 50 Hz VBI inflates the (short) clear/setup/collision buckets. */
extern volatile unsigned long g_isrBeamLines;
/* Clamp at 0: for a sub-ISR-duration phase (enemy/state), an ISR firing in its window can make
 * the subtracted ISR beam-span exceed the phase's own wall span -> a negative (unsigned-wrapped)
 * bucket.  Clamp so short buckets read 0 rather than ~4.29e9. */
#define FP_TIME(stmt, acc) do { unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
    stmt; unsigned long _d = rof_subclock() - _p, _id = g_isrBeamLines - _i; \
    (acc) += (_d > _id) ? (_d - _id) : 0; } while (0)
#define FP_ITER()      do { if (g_iterPostDs) { unsigned long _g = rof_subclock() - g_iterPostDs; \
                                g_iterLast = _g; if (_g > g_iterMax) { g_iterMax = _g; g_iterMaxAt = g_iterCount; } } \
                            g_iterCount++; } while (0)
#define FP_ITER_MARK() (g_iterPostDs = rof_subclock())
#else
#define FP_TIME(stmt, acc) do { stmt; } while (0)
#define FP_ITER()      ((void)0)
#define FP_ITER_MARK() ((void)0)
#define DS_MILE(i)     ((void)0)
#endif

/* terrain-draw shape counters: how often the hot inner ops run (see rof_native_amiga.cpp). */
#ifdef ROF_TDRAW_PROF
extern unsigned long g_tdMidpoints, g_tdPlots, g_tdRasterCalls, g_tdSubdivCalls;
extern unsigned long g_tdRaster;          /* beam ticks spent inside terrain_column_rasterize (subset of g_tdSubdiv) */
extern unsigned long g_tdRasBisect;       /* terrain_column_rasterize phase-2 "far" bisect-push iterations */
extern unsigned long g_tdRasDraw;         /* terrain_column_rasterize DRAW() invocations (incl. occluded; g_tdPlots = those that drew) */
extern unsigned short rof_beam_line(void);
#define TDCNT(c) (++(c))
/* bracket a statement, accumulating its raster-beam-line span (mod 313) into acc */
#define TDSPAN(stmt, acc) do { unsigned short _b0 = rof_beam_line(); stmt; \
    unsigned short _b1 = rof_beam_line(); \
    (acc) += (_b1 >= _b0) ? (unsigned long)(_b1 - _b0) : (unsigned long)(_b1 + 313 - _b0); } while (0)
#else
#define TDCNT(c) ((void)0)
#define TDSPAN(stmt, acc) do { stmt; } while (0)
#endif

/* ── SFX SHAPE PROBE (`make COMBAT=1 PROBES=1 SFX_SHAPE=1` + amiga/sfx_shape.gdb) ────────────
 * The combat re-attribution put ~47% of the flight VBI in sfx_voice_envelope_tick, with the
 * event-ring drain alone at ~24 t/firing = ~8% of ALL wall clock (the ISR runs at a fixed 50 Hz,
 * so a slower frame does not dilute it).  The drain does 3.39 entries/firing at ~7.2 t (~3200
 * cycles) each, and it is NOT the Paula recompute (update_paula_channel is only 5.8 t/firing).
 * This splits the two drain branches and counts the mixer scans they drive, so the fix is chosen
 * from the shape rather than guessed.
 *
 * ⚠ Deliberately NOT FP_TIME: that bracket costs ~2.2 t/call (2 rof_subclock + 2 volatile reads),
 * which on a 24 t/firing bucket sampled 3.4x per firing would be ~30% distortion — the exact
 * mistake that made the object plotter look 5x its real size.  These use a single beam-line read
 * per side (SX_SPAN, ~10x cheaper) and no ISR subtraction, which is right because we are already
 * INSIDE the ISR.  Counts are plain increments and cost nothing measurable. */
#define SX_CNT(c) ((void)0)
#define SX_SPAN(stmt, acc) do { stmt; } while (0)
#define SX_LEAF(stmt, acc) do { stmt; } while (0)
#define SX_NOP() ((void)0)

/* INTEG SHAPE PROBE (`make COMBAT=1 PROBES=1 INTEG_SHAPE=1`; read via amiga/integ_shape.gdb).
 * flight_control_integrate is 15.3 t/firing = 17% of the flight VBI = ~4.8% of ALL wall clock,
 * and nothing has ever measured INSIDE it.  This laps its 13 straight-line regions off ONE
 * running beam stamp — a single rof_beam_line() per boundary rather than an SX_SPAN's two —
 * so the per-bucket floor is one read, sampled as the empty lap g_inNop.  Subtract that floor
 * per bucket and read the rest as SHARES (the lap points block GCC reordering, so the probed
 * total exceeds the unprobed 15.3).  Counters say which branches the firing took. */
#define IN_DECL()    ((void)0)
#define IN_START()   ((void)0)
#define IN_LAP(acc)  ((void)0)
#define IN_CNT(c)    ((void)0)
#define O2_DECL()    ((void)0)
#define O2_START(c)  ((void)0)
#define O2_LAP(acc)  ((void)0)

/* ===========================================================================================
 * COMBAT-LOAD BENCHMARK  (`make COMBAT=1`, -DROF_COMBAT_LOAD; Amiga profiling aid, NOT faithful)
 *
 * WHY: headless flight (the PROBES/FPSCOUNT auto-launch) starts at the boot default LEVEL 4
 * ($3D1D writes level_stage $006D = 4), and compute_stage_display_geometry ($75F2) derives all
 * three difficulty knobs from that level:
 *     $0623 = (lvl==1) ? 0 : min(lvl,$2B)<<2     gun-emplacement density (a RANDOM/256 gate in
 *                                                intro_seed_object_map, per terrain peak)
 *     $0621 = (lvl< 4) ? 0 : $58 - 2*min(lvl,$22) flying-saucer spawn period (0 = never spawn)
 *     $0624 = ($2C - min(lvl,$28)) >> 1           enemy-fire delay mask (smaller = fires more)
 * At level 4 those are $10 / $50 / $14 = only 6.2% of peaks armed, a saucer spawn OPPORTUNITY
 * every 80 terrain-draw passes (half of them thrown away by a RANDOM gate, more by the
 * must-be-above-terrain gate), and enemy fire paced 10x slower than at level 40.  SPARSE, not
 * absent — which is why `make AUTO_FIRE=1` alone measured IDENTICAL FPS in 2026-07-31: in a short
 * window the shot statistically hit nothing.
 *
 * WHAT THIS DOES: forces level_stage to ROF_COMBAT_LEVEL before the launch, so the ENTIRE combat
 * cascade is produced by the faithful binary logic rather than by injected objects.  At the
 * default level 40 that is $0623=$A0 (63% of terrain peaks carry a jaggi gun emplacement),
 * $0621=$14 (a saucer every 20 draw passes) and $0624=$02 (enemy bolts fire almost continuously).
 * Two small unfaithful extras keep the load SUSTAINED and REPEATABLE for a long profiling run:
 *   - a flight-VBI top-up that re-seeds emplacement cells whose destroyed marker ($FA..$FF) has
 *     aged back to 0, so the map never empties out (COMBAT alone: same $0623 gate as the
 *     original seeding; with FORCE_EMPLACEMENTS=1: every peak, no gate).
 *   - FORCE_EMPLACEMENTS=1 additionally pins $0623=$FF -> EVERY peak is an emplacement, the
 *     saturated worst case.
 * Pair with AUTO_FIRE=1 (COMBAT implies it) so the emplacements are actually shot and explode,
 * and FIXED_RNG=1 so every build flies the same level.
 *
 * The g_cl* counters below exist to PROVE the load is real (and near-camera): they are compiled
 * only into a PROBES build, so the FPSCOUNT and PC-profile builds stay clean.  Distance here is
 * the object's $0051 depth step: raster_scaled_object only runs for depth < $0D, and SMALLER
 * depth = CLOSER = a bigger on-screen object, so g_clObjDist[] is the "how close did the targets
 * actually get" histogram and g_clObjNear counts the biggest (depth < 4) draws.
 * =========================================================================================== */
#if defined(ROF_COMBAT_LOAD) && defined(ROF_FLIGHT_PROBE)
extern volatile unsigned short g_clExplode, g_clEnemyFire, g_clImpact, g_clSaucer,
    g_clObjDraw, g_clObjNear, g_clReseed, g_clShotHit;
extern volatile unsigned long  g_clObjEnter;   /* cells with an occupant = ALL object work */
extern volatile unsigned short g_clObjDist[13];
extern volatile unsigned char  g_clLevel, g_cl0621, g_cl0623, g_cl0624;
extern volatile unsigned short g_clObjFrame;   /* ground objects drawn since the last painted frame */
#define CL_CNT(c)      (++(c))
#define CL_OBJ(depth)  do { unsigned char _d = (unsigned char)(depth); ++g_clObjDraw; ++g_clObjFrame; \
                            if (_d < 13) ++g_clObjDist[_d]; if (_d < 4) ++g_clObjNear; } while (0)

/* ── PER-PHASE FRAME DECOMPOSITION, split by combat state ────────────────────────────────────
 * The point: finish attributing the ~2x combat slowdown WITHOUT a cross-build throughput
 * comparison.  Bracketing every phase of one flight iteration inside ONE binary says what a
 * combat frame is actually MADE of, and splitting each phase by whether an explosion/bolt is
 * live says which phase inflates when it is.
 *
 * Two reasons the pre-existing g_fSetup/g_fClear/g_fDraw could never answer this:
 *  1. rof_subclock() raced the VERTB ISR and could run BACKWARDS -> unsigned underflow poisoned
 *     whole accumulators.  Fixed 2026-08-06 in PlatformAmiga.cpp (retry until the vbi count
 *     agrees either side of the beam read).
 *  2. **They only ever bracketed PASS 1.**  The flight loop renders the terrain field TWICE per
 *     iteration and pass 2 (`terrain_frame_setup(); clear_terrain_column_core(0x03);
 *     terrain_draw_frame_core(0x00);`) carried no brackets at all — so half of all terrain work
 *     was invisible and every "terrain is N% of the frame" number from these was ~half of the
 *     truth.  CL_PH now brackets BOTH passes into the same accumulators.
 * 6 brackets/iteration (12 clock reads) against a ~135 ms combat frame is negligible; per-OBJECT
 * bracketing is deliberately NOT done — that is what ROF_TDRAW_PROF does, and it distorts the
 * very loop it measures (and the object-count split already showed objects are nearly free). */
extern volatile unsigned long g_clPh[6][2];     /* ticks per phase, [state] 0 = explosion live */
extern volatile unsigned long g_clPhIter[2];    /* iterations per state */
extern volatile unsigned char g_clPhState;      /* latched once per iteration */
#define CL_PH_SETUP 0
#define CL_PH_CLEAR 1
#define CL_PH_DRAW  2
#define CL_PH_BOLT  3
#define CL_PH_ENEMY 4
#define CL_PH_FRAME 5
#define CL_PH(i, stmt, acc) do { \
    unsigned long _p = rof_subclock(); unsigned long _i0 = g_isrBeamLines; \
    stmt; \
    unsigned long _d = rof_subclock() - _p, _id = g_isrBeamLines - _i0; \
    unsigned long _t = (_d > _id) ? (_d - _id) : 0; \
    (acc) += _t; g_clPh[i][g_clPhState] += _t; \
    if ((i) == CL_PH_DRAW) g_clAltDraw[g_clAltBucket] += _t; } while (0)
/* Latch the state for the whole iteration, so every phase of one frame lands in one bucket, AND
 * accumulate the iteration's RAW wall time (no ISR subtraction) so the budget can be closed:
 *     wall = ISR + sum(phases) + unbracketed
 * Without that total there is no way to tell a complete decomposition from a partial one — and
 * the first run of this probe covered under half the iteration, which would have been invisible.
 *
 * ⚠ The explosion classifier is STICKY-over-the-previous-iteration, not "state at this instant".
 * A flight iteration is ~0.5-1 s of emulated time at these frame rates, so `$0041 != 0` sampled
 * once at the top says almost nothing about the iteration as a whole — the first version of this
 * probe latched instantaneously and duly reported a ~0 explosion effect, contradicting the
 * per-vblank split. The VBI sets g_clExplSeen whenever an explosion/bolt is live; this consumes
 * it. Still imperfect (it lags by one iteration) — treat the split as indicative and prefer the
 * per-phase totals, which need no classifier at all. */
#define CL_PH_ITER() do { \
    unsigned long _now = rof_subclock(); \
    if (g_clIterPrev) g_clIterWall[g_clPhState] += _now - g_clIterPrev; \
    g_clIterPrev = _now; \
    g_clPhState = g_clExplSeen ? 0u : 1u; g_clExplSeen = 0u; \
    g_clPhIter[g_clPhState]++; \
    g_clAltBucket = (unsigned char)(mem[0x28DA] >> 5); \
    g_clAltIter[g_clAltBucket]++; } while (0)
extern volatile unsigned long g_clIterWall[2], g_clIterPrev;
extern volatile unsigned char g_clExplSeen;
/* DRAW ticks + iterations bucketed by ship altitude ($28DA altimeter_alt_ref, >>5 = 8 buckets).
 * The point: the attribution showed DRAW (terrain + objects, both passes) is where combat's extra
 * frame time goes, but the object-COUNT split says objects are nearly free — so the suspicion is
 * that combat's ~30 bolt impacts (jitter_roll_pitch) throw the ship around and a disturbed
 * attitude/altitude simply puts MORE TERRAIN in view.  Comparing DRAW t/iteration at MATCHED
 * altitude between the combat and quiet runs tests exactly that, and it is a per-call comparison
 * rather than an end-to-end one, so the cross-build trajectory objection does not apply. */
extern volatile unsigned long g_clAltDraw[8], g_clAltIter[8];
extern volatile unsigned char g_clAltBucket;

/* ── DRAW SUB-SPLIT: *where inside* DRAW combat's extra time lands ───────────────────────────
 * DRAW is 46% of wall and reads +474 t/it in combat vs the quiet control at the SAME pinned
 * level and the same straight trajectory — more than the entire object plotter costs.  The
 * altitude bucketing above tests one hypothesis (a disturbed attitude puts more terrain in
 * view); this tests the complementary one by localising the delta inside the function, which
 * needs no cross-build reasoning at all.
 *   [0] head = the per-frame table fills + compute_row_xspans (fixed work, must NOT move)
 *   [1] obj  = terrain_draw_objects (projection / subdivision / rasterize / plot_object)
 *   [2] age  = the $0A00 near-max cell aging scan, bracketed separately inside the tail
 * tail = DRAW_total - head - obj, computed in the gdb script, so the function's four early
 * returns need no bracket of their own.
 * The aging scan is called out because it is 256 volatile byte reads gated on
 * map_cell_hit_marker, which only SHOT/BOLT impacts set — so it is structurally a combat-only
 * cost that the quiet control can never show. */
extern volatile unsigned long g_clDrawSub[3], g_clAgeScans, g_clDrawSubN;
/* Tree entries per iteration — one subdivide call per both-endpoints-visible pair.  A COUNT, not
 * a bracket, so it carries no probe floor: it says whether combat drives MORE terrain tree work
 * or the same amount of work more slowly. */
extern volatile unsigned long g_clSubCalls;
#define CL_SUB_BEG()  unsigned long _sp = rof_subclock(), _si = g_isrBeamLines; ++g_clDrawSubN
#define CL_SUB_END(i) do { unsigned long _d = rof_subclock() - _sp, _id = g_isrBeamLines - _si; \
    g_clDrawSub[i] += (_d > _id) ? (_d - _id) : 0; \
    _sp = rof_subclock(); _si = g_isrBeamLines; } while (0)
#define CL_AGE(stmt)  do { unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
    stmt; unsigned long _d = rof_subclock() - _p, _id = g_isrBeamLines - _i; \
    g_clDrawSub[2] += (_d > _id) ? (_d - _id) : 0; ++g_clAgeScans; } while (0)

/* ── THE CALIBRATION LOAD: how a phase bracket lies about combat ─────────────────────────────
 * WHY IT EXISTS.  Every phase of a combat iteration measures slower than the quiet control,
 * including ones whose work cannot vary with combat — terrain_frame_setup's fixed 45-cell loop
 * read +12%, ds_frame +18%.  Nothing in the harness could say whether that was a slower MACHINE
 * (DMA stolen by sprites/audio/blitter is indistinguishable from computation inside a bracket) or
 * the harness itself.  So: run a load whose cost CANNOT vary — fixed trip count, data-independent
 * — once per flight iteration, and measure it five different ways.
 *
 * WHAT IT FOUND (2026-08-07, COMBAT vs COMBAT_QUIET at level 40, FIXED_RNG, five paired runs):
 * the machine is NOT slower in combat, and the ordinary phase bracket says it is by ~15%.
 *
 *   row                                        combat   quiet   ratio
 *   MEM      1024 reads, ONE bracket            272.7   236.2   1.155   <- the lie
 *   MEM      the same 1024 reads, 8 ISR-FREE
 *            sub-windows (no subtraction)       226     224     1.010   <- the truth
 *   MEM x128 beam-locked to scanline 200         18.08   18.05  1.002   <- and again
 *   MEM      1024 reads, AUD+SPRITE DMA masked  269.1   237.8   1.132   (so not audio/sprite DMA)
 *   CPU      1024-step LFSR, one bracket         96.1    93.4   1.029
 *
 * ⚠ THE LESSON, and it applies to every bracket in this tree: `elapsed - g_isrBeamLines` is not a
 * safe estimator when the two builds have DIFFERENT ISR rates.  g_isrBeamLines is credited at ISR
 * EXIT, so an ISR straddling the bracket's start is over-subtracted and one straddling its end is
 * under-subtracted.  Those cancel only if the two ends are independent — and for a window near
 * the 313 t ISR period they are not, because start and end land at nearly the same phase of the
 * ISR cycle.  Combat's window sat at 87% of the period and the control's at 75%, so the bias did
 * not cancel between the builds either.  Short ISR-FREE windows need no subtraction and agree
 * with each other; trust those.
 *
 * The loads:
 *   MEM = 1024 scattered volatile byte reads out of mem[] — the traffic the terrain code is built
 *         from, so it feels chip-RAM contention the same way.
 *   CPU = a 1024-step 16-bit LFSR in registers (no 32-bit multiply — see m68k_math.h).  Still
 *         fetches its instructions, so it is not contention-free, just far less memory-hungry.
 * ⚠ This runs OUTSIDE the phase brackets, so it lands in the budget check's `unbracketed` line —
 * `covered` will read below 100% on a CALIBRATE build.  That is expected, not a broken budget. */
extern volatile unsigned long g_clCalMem, g_clCalCpu, g_clCalN, g_clCalSink, g_clCalNoDma,
    g_clCalIsrFree, g_clCalIsrFreeN, g_clCalLocked, g_clCalLockedN,
    g_clCalSplit, g_clCalSplitN;
extern unsigned short rof_beam_line(void);
#ifdef ROF_CALIBRATE
/* The same MEM loop with AUD0-3 + SPRITE DMA masked off around it.  Audio and sprite DMA steal
 * chip-RAM slots from the CPU, and combat has all four Paula channels busy where the quiet
 * control has almost none — so if MEM_NODMA converges between the two builds while MEM does not,
 * that IS the mechanism, and it is a real A500 effect rather than an emulator artifact.
 * Deliberately blunt: it re-enables both unconditionally (flight always has them on) and will
 * click the audio.  A measurement build only. */
#define CL_CAL_DMA(reg) (*(volatile unsigned short *)0xDFF096u = (unsigned short)(reg))
#define CL_CAL() do { \
    { unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
      unsigned long _s = 0; \
      for (int _k = 0; _k < 1024; _k++) _s += mem[0x0900 + (_k & 0xFF)]; \
      g_clCalSink = _s; \
      unsigned long _d = rof_subclock() - _p, _id = g_isrBeamLines - _i; \
      g_clCalMem += (_d > _id) ? (_d - _id) : 0; } \
    { unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
      unsigned long _s = 0; \
      CL_CAL_DMA(0x002Fu); \
      for (int _k = 0; _k < 1024; _k++) _s += mem[0x0900 + (_k & 0xFF)]; \
      CL_CAL_DMA(0x802Fu); \
      g_clCalSink += _s; \
      unsigned long _d = rof_subclock() - _p, _id = g_isrBeamLines - _i; \
      g_clCalNoDma += (_d > _id) ? (_d - _id) : 0; } \
    /* The same traffic in a window SHORT enough to usually fit between two ISR firings, counted
       ONLY when none fired inside it.  This is the one calibration that needs no ISR subtraction
       at all, so it separates a genuinely slower machine from an artifact of subtracting
       g_isrBeamLines (combat fires ~21 ISRs per iteration against the control's ~12, so any
       per-firing residue in the subtraction scales with exactly the thing being measured). */ \
    { unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
      unsigned long _s = 0; \
      for (int _k = 0; _k < 128; _k++) _s += mem[0x0900 + _k]; \
      g_clCalSink += _s; \
      unsigned long _d = rof_subclock() - _p; \
      if (g_isrBeamLines == _i) { g_clCalIsrFree += _d; ++g_clCalIsrFreeN; } } \
    /* ⭐ THE DECIDING ROW.  The SAME 1024 reads as the first row, but measured as eight short
       sub-windows and counted only where no ISR fired — so the identical work is priced with and
       without the subtraction, in one run.  Same inner body (the & 0xFF is kept) so the per-read
       cost is comparable.  Verdict: 226 t (combat) vs 224 t (quiet) against the single bracket's
       272.7 vs 236.2 — the whole gap was the estimator. */ \
    { for (int _j = 0; _j < 8; _j++) { \
        unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
        unsigned long _s = 0; \
        for (int _k = 0; _k < 128; _k++) _s += mem[0x0900 + ((_j*128 + _k) & 0xFF)]; \
        g_clCalSink += _s; \
        unsigned long _d = rof_subclock() - _p; \
        if (g_isrBeamLines == _i) { g_clCalSplit += _d; ++g_clCalSplitN; } } } \
    /* And once more BEAM-LOCKED: spin to a fixed scanline first, so both builds measure the same
       slice of the same frame.  This kills the last confound — bitplane DMA only steals CPU slots
       inside the display window, and the two builds do not sample the frame alike (the iteration
       top drifts with the ISR length).  Line 200 is mid-display and far from the vblank, so the
       window is ISR-free by construction.  It agrees with the split row: 18.08 vs 18.05. */ \
    { while (rof_beam_line() != 200u) { } \
      unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
      unsigned long _s = 0; \
      for (int _k = 0; _k < 128; _k++) _s += mem[0x0900 + _k]; \
      g_clCalSink += _s; \
      unsigned long _d = rof_subclock() - _p; \
      if (g_isrBeamLines == _i) { g_clCalLocked += _d; ++g_clCalLockedN; } } \
    { unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
      unsigned short _x = 0xACE1u; \
      for (int _k = 0; _k < 1024; _k++) \
          _x = (unsigned short)((_x >> 1) ^ ((unsigned short)(-(int)(_x & 1u)) & 0xB400u)); \
      g_clCalSink += _x; \
      unsigned long _d = rof_subclock() - _p, _id = g_isrBeamLines - _i; \
      g_clCalCpu += (_d > _id) ? (_d - _id) : 0; } \
    ++g_clCalN; } while (0)
#else
#define CL_CAL() ((void)0)
#endif

/* ── OBJECT-PLOTTER SHAPE PROBE (`make COMBAT=1 PROBES=1 OBJ_SHAPE=1`) ───────────────────────
 * The combat attribution said DRAW carries 69% of combat's extra frame time and that ~25
 * occupied cells enter terrain_plot_object per iteration while only ONE reaches
 * raster_scaled_object, and concluded the cost must be the per-visit plotter overhead.  That
 * step was an INFERENCE from a count ("25 x ~50 ticks ~= the +1161 t/it delta") — and 50 ticks
 * is ~22000 68000 cycles, which a visit that bails on a table compare cannot spend.  So measure:
 *   OP_TIME   brackets the whole per-object chain at its two call sites in the draw-order loop.
 *   RS_SHAPE  is flushed once per scaled blit with that call's own ISR-corrected tick span,
 *             BUCKETED BY CELL COUNT — the blit's cost is quadratic in 1/step (0x2000/step
 *             columns x 0x0C00/step rows, capped 32x12 = 384), so its mean says nothing about
 *             the near/exploding-object calls that are the actual complaint.  Comparing the same
 *             size bucket across builds keeps it a per-call metric.
 * ⚠ The OP_TIME bracket is NOT free: a QUIET run reaches it with zero A822 calls, so nearly all
 * ~48 visits/iteration are the two-load "empty cell" bail, yet the chain still measures ~110
 * t/it.  That floor IS the bracket (~2.2 t/call).  Subtract it before quoting a cost.
 * Per-cell counts accumulate in LOCALS and flush once per call, so the hot loops stay clean. */
#ifdef ROF_OBJ_SHAPE
#define ROF_OBJ_SHAPE_ON 1
extern volatile unsigned long g_opTicks, g_opCalls, g_opEmpty, g_opStep, g_opPathA, g_opPathB;
extern volatile unsigned long g_opaBusy, g_opaMask, g_opaBelow, g_opaDist, g_opaFire, g_opaDepth;
extern volatile unsigned long g_opbCross;
extern volatile unsigned long g_rsCalls, g_rsTicks, g_rsRows, g_rsCells, g_rsPlots, g_rsCellMax;
extern volatile unsigned long g_rsCellsByStep[14];
extern volatile unsigned short g_rsCallsByStep[14];
extern volatile unsigned long g_rsBktCalls[5], g_rsBktCells[5], g_rsBktTicks[5];
#define OP_CNT(c)  (++(c))
#define OP_TIME(stmt)  do { unsigned long _p = rof_subclock(); unsigned long _i = g_isrBeamLines; \
    stmt; unsigned long _d = rof_subclock() - _p, _id = g_isrBeamLines - _i; \
    g_opTicks += (_d > _id) ? (_d - _id) : 0; ++g_opCalls; } while (0)
/* cells -> size bucket: <16, 16-63, 64-127, 128-255, 256+ (the 384-cell cap is bucket 4). */
#define RS_SHAPE(step, rows, cells, plots, ticks) do { \
    unsigned char _s = (unsigned char)(step); if (_s > 13) _s = 13; \
    unsigned char _b = ((cells) >= 256u) ? 4u : ((cells) >= 128u) ? 3u \
                     : ((cells) >=  64u) ? 2u : ((cells) >= 16u) ? 1u : 0u; \
    ++g_rsCalls; ++g_rsCallsByStep[_s]; g_rsCellsByStep[_s] += (cells); \
    g_rsRows += (rows); g_rsCells += (cells); g_rsPlots += (plots); g_rsTicks += (ticks); \
    ++g_rsBktCalls[_b]; g_rsBktCells[_b] += (cells); g_rsBktTicks[_b] += (ticks); \
    if ((cells) > g_rsCellMax) g_rsCellMax = (cells); } while (0)
#else
#define OP_CNT(c)      ((void)0)
#define OP_TIME(stmt)  do { stmt; } while (0)
#define RS_SHAPE(step, rows, cells, plots, ticks) ((void)0)
#endif
#else
#define CL_CNT(c)      ((void)0)
#define CL_OBJ(depth)  ((void)0)
#define CL_PH(i, stmt, acc) FP_TIME(stmt, acc)
#define CL_PH_ITER()   ((void)0)
#define CL_SUB_BEG()   ((void)0)
#define CL_SUB_END(i)  ((void)0)
#define CL_AGE(stmt)   do { stmt; } while (0)
#define CL_CAL()       ((void)0)
#define OP_CNT(c)      ((void)0)
#define OP_TIME(stmt)  do { stmt; } while (0)
#define RS_SHAPE(step, rows, cells, plots, ticks) ((void)0)
#define CL_PH_DRAW_ALT 0
#define CL_PH_SETUP 0
#define CL_PH_CLEAR 1
#define CL_PH_DRAW  2
#define CL_PH_BOLT  3
#define CL_PH_ENEMY 4
#define CL_PH_FRAME 5
#endif

/* Rasterizer SHAPE probe (`make RASTER_C=1 RAS_SHAPE=1 PROBES=1`, read via
 * amiga/ras_shape.gdb).  Answers "where does terrain_column_rasterize_core's time actually
 * go" structurally rather than by PC sampling: the phase-2 entry-span and far-bisect-span
 * histograms plus the leaf / phase-1 mix.  This is what sized the 2026-08-05 phase-2
 * restructure (span 3 = 31.2% and span 4 = 16.5% of all far-bisects -> worth straight-lining;
 * 36.8% of DRAWs actually plot -> the hidden-surface reject path is the common one).
 *
 * Deliberately OFF even under PROBES: these are volatile counters inside the C ORACLE, so
 * leaving them on would inflate the oracle's beam-ticks and wreck the asm-vs-C in-process
 * differential's perf reading (measured: C 41 vs 15 ticks/call with them on, 20 vs 15 off). */
#define RSCNT(c)      ((void)0)
#define RSSAT(h)      ((void)0)
#define RSHIST(a, s)  ((void)0)
#define RSDOT(col, h) ((void)0)
#define RSOCCL(span, mh, chgt, hgt, pc, cm) ((void)0)
#define SDCNT(c)      ((void)0)
#define SDCLASS(sh, fh) ((void)0)
#define SEGPRE()      ((void)0)
#define SEGPOST()     ((void)0)
#define RCPRE(c, h, e, ch) ((void)0)
#define RCPOST()      ((void)0)

/* Direct-to-plane2 terrain dots (Amiga).  Instead of OR-ing each surface pixel into the mode-D
 * field and letting renderFlightDirect decode-scan it, terrain_column_rasterize writes the dots
 * STRAIGHT into the off-screen buffer's bitplane 2 (g_flightDotPlane; null on the first flight
 * frame -> skip), using the same kRow120/kColMask4 geometry the horizon plotter uses.  Mapping:
 * field column -> Amiga column = plotCol-48 (visible 0..159); height -> scanline = 150-h.
 *
 * LAG-BY-ONE so the topmost ridge pixel is NOT a dot.  On the Atari the rasterizer plots the
 * silhouette contour (one value-2 pixel per column per height, in strictly increasing height as
 * nearer terrain rises), and fill_terrain_silhouette then turns each column's TOPMOST pixel into
 * sky (COLPF0) while keeping the lower ones as the dark dots.  The Amiga builds the sky plane
 * (plane1) from $260E=COL_MAX via blitterFillUp, which already covers down to the crest row — so
 * if we also plotted a dot at COL_MAX the crest would be plane1&plane2 = value-3 COLPF2 (the tan
 * artifact).  Faithful fix: DRAW plots the column's PREVIOUS top (_oldMax) each time a higher
 * point supersedes it; the final/topmost point is never superseded, so it is never plotted as a
 * dot -> the crest stays pure sky.  The very first plot's _oldMax is the per-frame reset floor
 * ($67 -> scanline 47 on the outer columns, $6b -> scanline 43 on the inner ones — see the
 * $263A/$264E.. reset in terrain_frame_setup).  The range test admits scanlines 0..46 EXCEPT 43:
 * that lets real terrain dots reach the windscreen-band rows 44-46 (the terrain now renders full
 * height there) while still rejecting BOTH floor scanlines (43 for the $6b columns, 47 is >=47).
 * See RescueOnFractalus::renderFlightDirect. */
#ifdef ROF_PLATFORM_AMIGA
extern uint8_t* g_flightDotPlane;
extern uint8_t* g_flightObjP1;    /* object plane1 overlay (value-3 low bit), applied post sky-fill */
extern int g_objRowLo, g_objRowHi;
extern int g_objColLo, g_objColHi;   /* dirty BYTE-COLUMN range (0..39) of g_flightObjP1 */
/* Exact offsets of the bytes made nonzero in g_flightObjP1, so renderFlightDirect's post-sky-fill
 * apply walks ~10 entries instead of searching a ~447-byte box (see g_objTouch in
 * RescueOnFractalus.cpp).  Append on the 0->nonzero transition only, so there are no duplicates;
 * on overflow set the flag and let the apply fall back to the box walk (still maintained below).
 * ⚠ ROF_OBJ_TOUCH_CAP must match the definition in RescueOnFractalus.cpp (same extern-in-both-files
 * pattern as g_objRowLo/g_objColLo); a larger value here would overrun the array. */
#define ROF_OBJ_TOUCH_CAP 256
extern uint16_t g_objTouch[ROF_OBJ_TOUCH_CAP];
extern int g_objTouchN, g_objTouchOvf;
#define ROF_OBJ_TOUCH(off) do { \
    if (g_objTouchN < ROF_OBJ_TOUCH_CAP) g_objTouch[g_objTouchN++] = (uint16_t)(off); \
    else g_objTouchOvf = 1; \
} while (0)
extern const uint16_t kRow120[48];   /* row*120 (interleaved terrain scanline) */
extern const uint16_t kRow40[48];    /* row*40  (one mode-D plane / figure mask stride) */
extern const uint16_t kRow80[48];    /* row*80  (interleaved 2-plane figure stride) */
extern const uint8_t kColMask4[4];
extern void rof_flight_wait_dotclear(void);
#define ROF_PLOT_DOT(col, h) do { \
    if (g_flightDotPlane) { \
        int _ac = (int)(col) - 48; \
        int _sc = 150 - (int)(h);    /* height -> scanline */ \
        if ((unsigned)_ac < 160u && (unsigned)_sc < 47u && _sc != 43) {  /* rows 0..46 except the $6b floor at 43 */ \
            g_flightDotPlane[kRow120[_sc] + (_ac >> 2)] |= kColMask4[_ac & 3]; \
        } } } while (0)
/* Object value-3 low bit -> plane1 overlay (deferred to AFTER the sky fill; see g_flightObjP1 in
 * RescueOnFractalus.cpp).  Same geometry as ROF_PLOT_DOT; tracks the dirty scanline range so the
 * post-fill apply only walks the touched rows. */
#define ROF_PLOT_DOT_P1(col, h) do { \
    if (g_flightObjP1) { \
        int _ac = (int)(col) - 48; \
        int _sc = 150 - (int)(h); \
        if ((unsigned)_ac < 160u && (unsigned)_sc < 47u && _sc != 43) { \
            int _bc = _ac >> 2; \
            int _of = kRow120[_sc] + _bc; \
            if (!g_flightObjP1[_of]) ROF_OBJ_TOUCH(_of); \
            g_flightObjP1[_of] |= kColMask4[_ac & 3]; \
            if (_sc < g_objRowLo) g_objRowLo = _sc; \
            if (_sc > g_objRowHi) g_objRowHi = _sc; \
            if (_bc < g_objColLo) g_objColLo = _bc; \
            if (_bc > g_objColHi) g_objColHi = _bc; \
        } } } while (0)
/* Amiga sheds the mode-D field entirely: the dots come from ROF_PLOT_DOT (plane2) and the sky
 * from $260E (blitterFillUp), so nothing reads the field — fill_terrain_silhouette is skipped and
 * the windscreen band is blanked (see game_main_loop + renderFlightDirect).  So the rasterizer's
 * per-plot field write is dead here; drop it (one scattered volatile RMW per plot, in the hottest
 * loop) and leave rowLo/rowHi at their entry values (only the dropped field write used them). */
#define ROF_FIELD_PLOT(h) ((void)0)
/* Rescue-figure overlay (the pilot/alien walk-to-airlock zoom).  plot_clipped_pixel (only called
 * from draw_scaled_shape = the zoom) writes the figure as a BITMAP into the mode-D field, which the
 * Amiga sheds -> the figure would be dropped.  So mirror each opaque figure pixel into a small
 * scratch overlay (plane1 g_figP1 / plane2 g_figP2 + opaque mask g_figM, 43 rows x 40 bytes) that
 * renderFlightDirect composites over the preserved frozen terrain each paused frame.  Geometry
 * matches renderViewportModeD's verified field mapping: the field write lands at $1074+$30+r*96+b,
 * so Amiga row r = $96-y, plane byte b = (x>>2)-12, pixel-in-byte = x&3 (kColMask4).  Only the
 * zoom drives plot_clipped_pixel, so no gating is needed. */
extern uint8_t* g_figP1; extern uint8_t* g_figP2; extern uint8_t* g_figM;
extern int g_figRowLo, g_figRowHi;
extern int g_figColLo, g_figColHi;   /* dirty byte-column extent (0..39) — for the narrow-rect composite */
/* The figure planes g_figP1/g_figP2 live in a 2-plane INTERLEAVED chip Bitmap (row stride 80 =
 * plane1[40]+plane2[40]) so renderFlightDirect can composite it with the blitter; the mask g_figM
 * is a separate 1-plane bitmap (row stride 40).  So mask uses _im (stride 40), planes use _ip (80). */
#define ROF_PLOT_FIG(x, y, v2) do { \
    if (g_figP1) { \
        int _r = 0x96 - (int)(y); int _b = ((int)(x) >> 2) - 12; \
        if ((unsigned)_r < 43u && (unsigned)_b < 40u) { \
            uint8_t _m = kColMask4[(int)(x) & 3]; int _im = kRow40[_r] + _b, _ip = kRow80[_r] + _b; \
            g_figM[_im] |= _m; \
            if ((v2) & 1u) g_figP1[_ip] |= _m; \
            if ((v2) & 2u) g_figP2[_ip] |= _m; \
            if (_r < g_figRowLo) g_figRowLo = _r; \
            if (_r > g_figRowHi) g_figRowHi = _r; \
            if (_b < g_figColLo) g_figColLo = _b; \
            if (_b > g_figColHi) g_figColHi = _b; \
        } } } while (0)
/* Clear the scratch (dirty range only) at the START of each shape draw so it holds exactly the
 * current frame's figure — during the multi-frame RTCLOK wait renderFlightDirect keeps compositing
 * the same figure (no flicker); the next draw clears + refills it. */
#define ROF_CLEAR_FIG() do { \
    if (g_figM && g_figRowHi >= g_figRowLo) { \
        int _cl = g_figColLo, _ch = g_figColHi; \
        for (int _r = g_figRowLo; _r <= g_figRowHi; _r++) { \
            int _om = kRow40[_r], _op = kRow80[_r]; \
            for (int _b = _cl; _b <= _ch; _b++) { g_figM[_om + _b] = 0; g_figP1[_op + _b] = 0; g_figP2[_op + _b] = 0; } } \
        g_figRowLo = 99; g_figRowHi = -1; g_figColLo = 40; g_figColHi = -1; \
    } } while (0)
/* Alien jump-scare creature overlay.  The creature (alien_creature_animate_draw -> alien_shape_blit $80C5,
 * airlock-CLOSED knock) blits into the mode-D field (base $1010, stride 96, +$30 display half),
 * which the Amiga sheds for the terrain body -> dropped.  Mirror each written byte into the SAME
 * figure overlay the paused-rescue compositor already draws (the approach-figure zoom = landing
 * phase 3 and the creature = phase 4 never draw simultaneously, so g_figP1/P2/M is free).  Geometry
 * (measured, matches the figure overlay): field byte at addr A -> rel = A-$10A4, row r = rel/96,
 * plane byte b = rel%96.  The stored byte V is the FINAL composited field byte (creature over the
 * frozen terrain, same frame the compositor draws over), so writing V wholesale (all pixels opaque,
 * mask=$FF) reproduces the field exactly.  Decode V -> plane1/plane2 via the same kModeDP1/kModeDP2
 * tables renderViewportModeD uses.  Cleared each creature frame by ROF_CLEAR_FIG (in alien_knock_setup_loop). */
extern uint8_t kModeDP1[256]; extern uint8_t kModeDP2[256];
#ifdef ROF_FLIGHT_PROBE
extern volatile unsigned long g_alKnockFrames;   /* fwd decls (defined in the probe block below) */
extern volatile unsigned char g_alPen[6];
extern volatile unsigned long g_alTWait, g_alTDraw, g_alTRender;  /* per-step beam-tick accumulators */
extern volatile unsigned long g_alTHud;          /* beam ticks inside alien_shape_blit (creature rows) */
extern volatile unsigned long g_alHudCalls;      /* # alien_shape_blit calls during the knock */
#endif
#define ROF_PLOT_ALIEN(addr, V) do { \
    if (g_figP1) { \
        int _rel = (int)(addr) - 0x10A4; \
        if (_rel >= 0) { int _r = _rel / 96, _b = _rel % 96; \
            if ((unsigned)_r < 43u && (unsigned)_b < 40u) { \
                int _im = kRow40[_r] + _b, _ip = kRow80[_r] + _b; \
                uint8_t _ap1 = kModeDP1[(unsigned char)(V)], _ap2 = kModeDP2[(unsigned char)(V)]; \
                g_figM[_im]  = (uint8_t)(_ap1 | _ap2); /* silhouette: value-0 pixels transparent -> terrain shows */ \
                g_figP1[_ip] = _ap1; \
                g_figP2[_ip] = _ap2; \
                if (_b < g_figColLo) g_figColLo = _b; \
                if (_b > g_figColHi) g_figColHi = _b; \
                if (_r < g_figRowLo) g_figRowLo = _r; \
                if (_r > g_figRowHi) g_figRowHi = _r; \
            } } } } while (0)
#else
#define ROF_PLOT_DOT(col, h) ((void)0)
#define ROF_PLOT_DOT_P1(col, h) ((void)0)
#define ROF_PLOT_FIG(x, y, v2) ((void)0)
#define ROF_CLEAR_FIG() ((void)0)
#define ROF_PLOT_ALIEN(addr, V) ((void)0)
/* SDL/validate: OR the value-2 pixel into the mode-D field (the dots source SDL decodes, and the
 * surface fill_terrain_silhouette scans).  rowLo/rowHi are left in $80/$81 via WB() (faithful). */
#define ROF_FIELD_PLOT(h) do { \
    rowLo = M[0x28CA + (h)]; rowHi = M[0x28FA + (h)]; \
    uint16_t _a = (uint16_t)(rowLo | (rowHi << 8)) + M[MEM_terrain_col_byte_offset + plotCol]; \
    M[_a] |= M[MEM_terrain_col_pixel_mask + plotCol]; \
} while (0)
#endif

/* ---- direct tunnel-ring painting hooks (Amiga renderer) -------------------------------------
 * The boost reverse cinematic does NOT decode its ring field: draw_symmetric_span_loop hands each
 * concentric rectangle straight to the Amiga renderer, which paints it into the ring bitmap the
 * copper shows inside the reveal band.  So these are part of the shipped Amiga build, not a probe
 * or a build flag — the rings would simply be missing without them.
 *
 * ROF_TUNNEL_RECT: one rectangle.  Coordinates are the loop's own, in FIELD units: rowTop/rowBot
 * index the $073D/$0793 row-address table, xL/xR are GTIA pixel columns (2 per byte), byteLo/byteHi
 * are the horizontal edge's inclusive byte columns after the odd/even nudge.  rowBase identifies
 * the target field ($1000 rings vs $2000 doors) — the same table entry the loop already loaded, so
 * the hook costs an argument rather than a lookup.  colour is the GTIA nibble the fills use.
 * ROF_TUNNEL_COLS: the pre-draw's tail (draw_frame_guide_columns) plots three FULL-HEIGHT columns
 * through plot_pixel_masked_core, i.e. a field writer OUTSIDE the span loop that the rectangle hook
 * cannot see.  They land at the vanishing point, so a painter that skips them leaves the middle of
 * the tunnel empty.
 * ROF_TUNNEL_VSPAN: one vertical span PAIR from the fill_vertical_span shim — i.e. plot_terrain_span,
 * the third field writer.  It runs twice in the reverse cinematic and both matter: once with colour
 * 8 straight after the static pre-draw (the pass that ERASES that image back to background), and
 * then once per revealed row as emit_dl_coord_pairs tail-calls it with the cycling ring colour.
 * Measured 2026-08-10: without this the painted bitmap carried the erased pre-draw as 6% stale ink.
 *
 * Nothing here needs the field's previous contents: the 6502's two mask tables ($66E9/$66FB) are
 * exactly "set this nibble to colour, preserve the other", so every write is prev-independent.
 * That is the fact the whole no-decode design rests on. */
#ifdef ROF_PLATFORM_AMIGA
extern void platform_tunnel_rect(uint16_t rowBase, uint8_t rowTop, uint8_t rowBot,
                                 uint8_t xL, uint8_t xR, uint8_t byteLo, uint8_t byteHi,
                                 uint8_t colour);
extern void platform_tunnel_columns(uint16_t rowBase, uint8_t colL, uint8_t colR, uint8_t colR1,
                                    uint8_t colour);
extern void platform_tunnel_vspan(uint16_t rowBase, uint8_t r0, uint8_t r1, uint8_t colL,
                                  uint8_t colR, uint8_t colour);
#define ROF_TUNNEL_RECT(rowBase, rowTop, rowBot, xL, xR, byteLo, byteHi, colour) \
    platform_tunnel_rect((rowBase), (rowTop), (rowBot), (xL), (xR), (byteLo), (byteHi), (colour))
#define ROF_TUNNEL_COLS(rowBase, colL, colR, colR1, colour) \
    platform_tunnel_columns((rowBase), (colL), (colR), (colR1), (colour))
#define ROF_TUNNEL_VSPAN(rowBase, r0, r1, colL, colR, colour) \
    platform_tunnel_vspan((rowBase), (r0), (r1), (colL), (colR), (colour))
#else
#define ROF_TUNNEL_RECT(rowBase, rowTop, rowBot, xL, xR, byteLo, byteHi, colour) ((void)0)
#define ROF_TUNNEL_COLS(rowBase, colL, colR, colR1, colour) ((void)0)
#define ROF_TUNNEL_VSPAN(rowBase, r0, r1, colL, colR, colour) ((void)0)
#endif

/* Which CALL SITE produced the rectangle the hook is about to emit — a measurement aid, so the
 * painter's buckets are measured rather than labelled (docs/boost-tunnel-direct-handoff.md §0a):
 * 1 = draw_frame_pattern_seq @ L_6047, 2 = draw_ring_frame_step (forward descent),
 * 3 = step_accum_sub_7e (boost reverse), 4 = draw_frame_pattern_seq @ tunnel_prebuild_rings.
 * Sites 2 and 3 fire from the 50 Hz VBI ISR and can interrupt site 1's main-loop loop, so they
 * save and restore the tag around their span-loop call; site 1 re-arms it every iteration.
 * Without that the ISR would silently relabel the pre-draw's remaining rectangles. */
#if defined(ROF_PLATFORM_AMIGA) && defined(ROF_FLIGHT_PROBE)
extern volatile unsigned char g_trSrc;
extern volatile unsigned char g_trPreSite;
#define ROF_TR_SRC_SAVE(v)    const uint8_t v = g_trSrc
#define ROF_TR_SRC_SET(n)     g_trSrc = (unsigned char)(n)
#define ROF_TR_SRC_RESTORE(v) g_trSrc = (v)
#define ROF_TR_PRESITE(n)     g_trPreSite = (unsigned char)(n)
#else
#define ROF_TR_SRC_SAVE(v)    ((void)0)
#define ROF_TR_SRC_SET(n)     ((void)0)
#define ROF_TR_SRC_RESTORE(v) ((void)0)
#define ROF_TR_PRESITE(n)     ((void)0)
#endif

/* Flight terrain double-buffer: which field half renderFlightDirect should display.
 * The flight loop renders TWO field halves per iteration (pass 1 = back/offset-$30,
 * pass 2 = display/offset-0).  game_main_loop sets this before each ds_frame so BOTH
 * halves get shown (one ds_frame per pass) — faithful to the Atari, which alternated
 * the displayed half each vblank.  0 = display half (offset 0); 1 = back half (offset $30).
 * Defined here (writer's TU, linked by both SDL + Amiga); the Amiga renderFlightDirect
 * reads it, the SDL render path ignores it. */
volatile unsigned char g_flightRenderHalf = 0;

/* Amiga black-until-ready reveal gate (read by animatePalette in RescueOnFractalus.cpp).
 * Set at boot_standby_launch_driver entry — by then game_main_loop has drawn the cockpit + top bar and
 * scene.initialize has set up the sprites, so the window build is about to begin: the point
 * the user wants the screen to appear (cockpit pops in, then the window builds visibly).
 * Harmless on SDL (nothing reads it there). */
volatile unsigned char g_standbyRevealReady = 0;

/* Amiga door-field-ready gate (read by render() in RescueOnFractalus.cpp).  Latched on in
 * boot_standby_launch_driver once the doors/dots/LEVEL field has been drawn into $2000 (after
 * blit_message_block/blit_numeric_readout) but BEFORE delay_loop_c2_to_c9 ramps the green
 * background colour $0071.  The Amiga decodes $2000 -> terrainBitmap once when this rises, so
 * the door shapes exist before the fade and the live color03 (= atariToOCS(mem[$0071])) ramp
 * shows the dark->bright green build on the real door pixels (was gated on $00E7 = build end,
 * AFTER the fade -> doors popped in already-green).  Latches like g_standbyRevealReady so the
 * launch re-run of boot_standby_launch_driver doesn't transiently re-arm it.  Harmless on SDL. */
volatile unsigned char g_doorFieldReady = 0;

/* Amiga level-select door-scroll dirty flag (read by doorScrollVblankUpdate in
 * RescueOnFractalus.cpp).  Set whenever blit_numeric_readout rewrites the $2000 LEVEL digit
 * (during the SELECT "elevator" roll).  The scroll itself is a pure per-frame BPLxPT move (one
 * pointer poke); the field is re-decoded ONLY when this flag says the digit actually changed,
 * so the ISR does not re-convert the whole door field every frame.  Harmless on SDL. */
volatile unsigned char g_doorScrollFieldDirty = 0;
/* ...and WHICH $2000 field rows (0..$54, inclusive) that decode has to cover.  An EMPTY range
 * (row0 > row1) means "no range recorded" => decode the whole field.
 * This range is load-bearing, not an optimisation for its own sake: the full 172-row decode
 * measures 310 raster lines (~19.7 ms) — a whole PAL frame — and it runs inside the vblank ISR.
 * That overran the frame every time the LEVEL digit was rewritten mid-scroll, so one displayed
 * frame of the level-select elevator scroll was lost (seen as a one-frame stall then a 2-pixel
 * jump, with the dots landing a row low), and it occasionally pushed the same ISR's copper-list
 * rewrite behind the beam, which is what misaligned the cockpit for a frame.  A digit rewrite
 * only dirties 8 rows. */
volatile unsigned char g_doorDirtyRow0 = 0xFF;   /* first dirty field row (inclusive) */
volatile unsigned char g_doorDirtyRow1 = 0x00;   /* last  dirty field row (inclusive) */
/* Raise the door-scroll dirty flag AFTER the $2000 field has been fully written (call at the END
 * of a field-writing routine, never before).  The VBI ISR (doorScrollVblankUpdate) decodes the
 * tall door bitmap when this is set; if it were raised before the digit glyph is drawn, the ISR
 * could preempt the main-thread glyph write mid-store and decode a half-drawn / stale digit, then
 * clear the flag so no later decode corrects it (the intermittent "part of LEVEL NN not cleared /
 * half-drawn digit" in the fade-rebuild wrap).  Raising it only once the field is COMPLETE means
 * every decode captures a whole digit.  Amiga-only; no-op on SDL/validate. */
#ifdef ROF_PLATFORM_AMIGA
  /* Widen the pending range to the whole field WITHOUT raising the flag — for the routines that
   * rewrite $2000 wholesale (fill_region_2000, blit_message_block).  They are always followed by
   * the blit_numeric_readout that DOES raise it, so the "raise only when the field is complete"
   * invariant above is preserved exactly. */
  #define ROF_DOOR_FIELD_TOUCH_ALL()  do { g_doorDirtyRow0 = 0x00; g_doorDirtyRow1 = 0x54; } while (0)
  /* Widen by the 8 rows a glyph blit at row counter `rc` ($0092) just wrote — again without
   * raising the flag.  Hooked into blit_glyph_8rows, the choke point EVERY glyph write goes
   * through, so a caller cannot forget it (blit_label_row's "LEVEL" word was missed when only
   * blit_numeric_readout marked its rows, and the wiped word kept stale pixels across the
   * level-wrap rebuild).  build_line_addr_table_2000 makes row_base[i] = $2000 + $2E*i, so the
   * table INDEX is the field row, and blit_glyph_8rows starts at row_base[$0092] and walks UP one
   * row per glyph row => rows rc-7..rc.  If entry 0 isn't $2000 the row table has been repointed
   * (the cockpit readouts use $1000), so this glyph did not touch the door field at all. */
  #define ROF_DOOR_FIELD_TOUCH_ROWS(rc)  do { rof_door_field_touch_rows((unsigned char)(rc)); } while (0)
static void rof_door_field_touch_rows(unsigned char rc) {
    if (row_base_lo == 0x00 && row_base_hi == 0x20 && rc <= 0x54) {
        unsigned char r0 = (unsigned char)((rc >= 0x07) ? (rc - 0x07) : 0x00);
        if (r0 < g_doorDirtyRow0) g_doorDirtyRow0 = r0;
        if (rc > g_doorDirtyRow1) g_doorDirtyRow1 = rc;
    }
}
  /* Raise the flag.  The rows were marked as they were written; an EMPTY range here means the
   * writes went somewhere other than the $2000 field (or a marker was missed), and the consumer
   * then decodes the whole field — i.e. exactly the pre-range behaviour. */
  #define ROF_DOOR_FIELD_DIRTY()  do { g_doorScrollFieldDirty = 1; } while (0)
#else
  #define ROF_DOOR_FIELD_TOUCH_ALL()     do { } while (0)
  #define ROF_DOOR_FIELD_TOUCH_ROWS(rc)  do { } while (0)
  #define ROF_DOOR_FIELD_DIRTY()         do { } while (0)
#endif

/* ---------------------------------------------------------------------------
 * Idiomatic-C migration seam.
 *
 * Each VALIDATE_FUNCS routine is split into two halves:
 *   - a TYPED CORE `<canonical>_core` (e.g. divide_16x16_core,
 *     clear_terrain_column_core) that takes real C parameters / returns real
 *     values — the form we want all native code to converge on; and
 *   - a 6502-ABI SHIM keeping the transpiler-mandated `void name(void)` symbol
 *     that transpiled callers (rof_gen.c), the validate harness, and the
 *     register/memory calling convention bind to.  The shim just marshals
 *     mem[]/cpu <-> the core.
 * Hand-written native callers (the Amiga C++ ports, and the cores here) call
 * the TYPED CORE directly.  When a routine's last transpiled caller is shed, its
 * shim + VALIDATE_FUNCS entry can be deleted, leaving pure C.
 * ------------------------------------------------------------------------- */
static inline uint16_t rd16(uint16_t a) { return (uint16_t)(mem[a] | (mem[a + 1] << 8)); }
static inline void     wr16(uint16_t a, uint16_t v) { mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8); }

/* divide_16x16 @ $9D6F — restoring 16-bit divide, expressed as one native divide.
 *
 * Inputs : dividend $00B0(lo)/$00B1(hi), divisor $00AE(lo)/$00AF(hi),
 *          quotient accumulator $00B2 (its 8 bits are fully shifted out).
 * Outputs: remainder -> $00B0/$00B1, quotient -> $00B2,
 *          divisor left shifted by the normalization count -> $00AE/$00AF.
 * Domain : divisor in [0x0001, 0x7FFF] AND dividend < divisor.  The sole caller
 *          (project_terrain_points) scales the divisor up until it strictly
 *          exceeds the dividend before calling, so this always holds in-game; it
 *          is also exactly the domain in which the 8-bit quotient does not overflow.
 *
 * The 6502 routine normalizes by shifting BOTH operands left k times until the
 * divisor's top set bit reaches bit14, then runs 8 ROL-quotient (restoring
 * shift-subtract) steps.  Because dividend < divisor, the normalization scales
 * both by 2^k with no bit loss, so the whole thing reduces to:
 *
 *     quotient  $B2      = (dividend * 256) / divisor          (< 256)
 *     remainder $B0/$B1  = ((dividend * 256) % divisor) << k   (the 6502 leaves
 *                          the remainder still scaled by the normalization count)
 *     divisor   $AE/$AF  = divisor << k                        (shifted in place)
 *
 * Both operands are non-negative (divisor in [1,0x7FFF]; the caller pre-absolutes
 * the dividend and tracks sign separately), so this is an UNSIGNED divide: the
 * division/modulo is a single 68000 DIVU (not DIVS).  Only the tiny normalization
 * count loop remains (it just positions the side outputs).  The 6502 register/flag
 * exit state is intentionally NOT reproduced — both call sites save Y through
 * $009F and overwrite A/flags immediately (verified incidental/dead).
 */
/* Typed core: see rof_native.h for the contract. */
DivResult divide_16x16_core(uint16_t dividend, uint16_t divisor) {
    /* Normalization count k: divisor << k brings its top set bit to bit14. */
    unsigned k = 0;
    while (!((divisor << k) & 0x4000)) k++;

    uint32_t numerator = (uint32_t)dividend << 8;
    DivResult r;
    r.quotient        = (uint8_t)(numerator / divisor);            /* < 256 */
    r.remainder       = (uint16_t)((numerator % divisor) << k);
    r.shifted_divisor = (uint16_t)(divisor << k);
    return r;
}

/* 6502-ABI shim: dividend $B0/$B1, divisor $AE/$AF in; remainder -> $B0/$B1,
 * quotient -> $B2, shifted divisor -> $AE/$AF out.  (Exit cpu state is dead at
 * both call sites — see header note above — so it is intentionally untouched.) */
void divide_16x16(void) {
    DivResult r = divide_16x16_core(rd16(0x00B0), rd16(0x00AE));
    wr16(0x00B0, r.remainder);
    div_quotient = r.quotient;
    wr16(0x00AE, r.shifted_divisor);
}

/* clear_terrain_column @ $AD5F — clear one terrain column band + its object-table cells.
 *
 * Inputs : cpu.X = starting column offset into the terrain/object buffers.
 * Effect : zeroes 44 terrain rows (base $1010, stride $60) across 42 columns
 *          starting at X (the X index runs as a 6502 byte, so it WRAPS at $FF —
 *          replicated here for bit-identity), then zeroes a scattered set of
 *          object-table cells indexed by the ORIGINAL X (restored from $0094).
 * Outputs: mem[$0094] = original X; the 42x44 terrain band and the listed object
 *          cells set to 0.
 * Contract: memory only.  Both call sites (flight loop $3EBF / $3EFA) reload X
 *          and A immediately after the call, so the 6502 exit register/flag state
 *          is dead.  We still reproduce it (A=0, Y=0, X=original, N/Z per LDX X)
 *          so the validation harness shows zero incidental CPU drift.
 */
/* stretch-A per-function one-shot subclock profiler (standby->doors freeze hunt). */
#ifdef ROF_FLIGHT_PROBE
extern volatile unsigned long g_saTicks[16];
extern volatile unsigned char g_saPhase;
#define SA_TIMED(i, expr) do { g_saPhase = (i); unsigned long _t0 = rof_subclock(); expr; g_saTicks[i] += rof_subclock() - _t0; } while (0)
#else
#define SA_TIMED(i, expr) do { expr; } while (0)
#endif

/* Zero `n` bytes at `vp` through a non-volatile alias so the 68000 compiler can batch
 * the stores into aligned move.l (instead of one volatile move.b per byte).  The result
 * is byte-identical to a byte-by-byte zero (proven by make validate).  Aligns to a 4-byte
 * boundary first — mandatory on the 68000, where move.l faults on an odd address. */
static void zero_run(volatile uint8_t* vp, unsigned n) {
    uint8_t* b = (uint8_t*)vp;
    while (n && ((uintptr_t)b & 3u)) { *b++ = 0u; --n; }   /* align to 4 (move.l needs even) */
    uint32_t* p = (uint32_t*)b;                            /* post-incr -> move.l d0,(a0)+ */
    while (n >= 16u) { *p++ = 0u; *p++ = 0u; *p++ = 0u; *p++ = 0u; n -= 16u; }
    while (n >= 4u)  { *p++ = 0u; n -= 4u; }
    b = (uint8_t*)p;
    while (n) { *b++ = 0u; --n; }
}

/* Typed core: see rof_native.h. */
void clear_terrain_column_core(uint8_t startCol) {
    draw_color_idx = startCol;                       /* $AD5F: STX $0094 (save column) */

    /* $AD61-$ADEF: 42 columns ($2A) x 44 rows (base $1010, stride $60), all 0.  The 6502
     * walks column-major with byte-wide INX wrap; here we clear each row's 42 contiguous
     * columns in one batched run (row-major), splitting only if the byte column index
     * wraps past $FF (startCol > $D6 — never happens in flight, kept for bit-exactness). */
#ifndef ROF_PLATFORM_AMIGA
    /* The mode-D field BODY is SHED on Amiga: its only reader
     * (fill_terrain_silhouette) is #ifndef ROF_PLATFORM_AMIGA, ROF_FIELD_PLOT is a
     * no-op, and renderFlightDirect reads only the band cell $2094 (a scattered cell
     * cleared below, NOT in body columns 0-41).  So skip this 44-row x 42-col clear on
     * Amiga (~7% of the flight frame, cleared twice/frame).  SDL keeps it -> validate. */
    unsigned head = (startCol + 0x2Au <= 0x100u) ? 0x2Au : (0x100u - startCol);
    unsigned tail = 0x2Au - head;
    for (uint16_t row = 0x1010; row <= 0x2030; row += 0x60) {
        zero_run(mem + row + startCol, head);
        if (tail) zero_run(mem + row, tail);
    }
#endif

    /* $ADF0-$AE52: scattered object-table cells, indexed by the ORIGINAL column. */
    static const uint16_t cells[] = {
        0x2090, 0x2091, 0x2092, 0x2093, 0x2094,
        0x20BA, 0x20B9, 0x20B8, 0x20B7, 0x20B6, 0x20B5,
        0x20F0, 0x20F1, 0x20F2, 0x20F3,
        0x211A, 0x2119, 0x2118, 0x2117, 0x2116,
        0x2150, 0x2151, 0x2152,
        0x217A, 0x2179, 0x2178, 0x2177,
        0x21B0, 0x21DA, 0x21B1, 0x21D9, 0x21D8,
    };
    for (unsigned k = 0; k < sizeof cells / sizeof cells[0]; k++)
        mem[cells[k] + startCol] = 0x00;
}

/* 6502-ABI shim: cpu.X = starting column in.  Exit cpu state (A=0, Y=0,
 * X=original, N/Z per LDX X) is dead at both call sites but reproduced so the
 * validation harness shows zero incidental CPU drift. */
void clear_terrain_column(void) {
    uint8_t x0 = cpu.X;
    clear_terrain_column_core(x0);
    cpu.A = 0x00; cpu.Y = 0x00; cpu.X = x0;
    cpu.Z = (x0 == 0) ? 1 : 0; cpu.N = (x0 >> 7) & 1;
}

/* bin_to_bcd @ $4E84 — convert binary A (0-99) to packed BCD.
 *
 * The 6502 does this by repeated subtraction: Y counts how many times 10 fits
 * (= the tens digit), the leftover after one over-subtract is the ones digit
 * (stored to $00C1), and the result A = (tens << 4) | ones.  For A >= 100 the
 * tens digit exceeds 9 and the ASL x4 truncates to 8 bits — faithfully replicated
 * by the (uint8_t) cast on (tens << 4).  Callers consume the BCD byte in cpu.A
 * and the tens digit in cpu.Y; $00C1 holds the ones digit. */
uint8_t bin_to_bcd_core(uint8_t a, uint8_t *units, uint8_t *tens) {
    uint8_t t = (uint8_t)(a / 10);
    uint8_t u = (uint8_t)(a % 10);
    *units = u;
    *tens  = t;
    return (uint8_t)((uint8_t)(t << 4) | u);
}

/* 6502-ABI shim: entry A = value.  Exit: A = packed BCD, Y = tens, $00C1 = ones.
 * N/Z reflect the final A (the 6502's terminal ORA sets them from the result). */
void bin_to_bcd(void) {
    uint8_t units, tens;
    uint8_t bcd = bin_to_bcd_core(cpu.A, &units, &tens);
    row_table_stride = units;
    cpu.Y = tens;
    LDA(bcd);
}

/* copy_title_text_block_to_screen @ $782A — Standby per-frame altitude banner.
 *
 * 6502: A=$0091; if A<$C0 return; if A==$C0 and $00E2 has bit7 set return;
 * $0091=Y(entry); X=($A>=$E0 ? $27 [also $00D8=$44] : $13); then copy 20 bytes
 * $5A9F+X -> $32B6+Y for Y=$14..$01 (X and Y decrementing together).  Exit
 * A/X/Y are dead at the Standby-loop call site (incidental in validation). */
void copy_title_text_block_to_screen_core(uint8_t entryY) {
    uint8_t a = altitude_threshold;
    if (a < 0xC0) return;
    if (a == 0xC0 && (attract_timer & 0x80)) return;
    altitude_threshold = entryY;
    uint8_t x = (a >= 0xE0) ? 0x27 : 0x13;
    if (a >= 0xE0) text_color_pf0 = 0x44;
    for (uint8_t y = 0x14; y != 0; y--, x--)
        mem[0x32B6 + y] = mem[0x5A9F + x];
    platform_title_changed();   /* hook: $32B7 banner rewritten -> flag the title region dirty */
}

/* 6502-ABI shim: entry cpu.Y is the new $0091 gate value. */
void copy_title_text_block_to_screen(void) {
    copy_title_text_block_to_screen_core(cpu.Y);
}

/* init_row_coords_9c @ $6DDF — load 5 row/coordinate constants into $009C-$00A0.
 * Pure leaf; exit regs (A=$13) dead at call sites (incidental in validation). */
void init_row_coords_9c(void) {
    draw_x_left = 0x2E;
    draw_x_right = 0x30;
    draw_row_top = 0x2B;
    draw_row_bottom = 0x2A;
    draw_iter_count = 0x13;
}

/* clear_scroll_accum @ $6B71 — zero the PCOLR0-3 shadow $02C0-$02C3 and the
 * 24-bit scroll accumulator $00A1-$00A4 (plus $00A5).  Pure leaf. */
void clear_scroll_accum(void) {
    for (int i = 0; i < 4; i++) {       /* 6502: X = 3..0 */
        mem[0x02C0 + i] = 0x00;
        mem[MEM_scroll_accum_b0 + i] = 0x00;
    }
    scroll_accum_prev = 0x00;
}

/* wait_vcount_ge_7a @ $3C7B — block until the vertical beam has scanned past the visible
 * playfield, so the caller can swap the ANTIC display list / DLI vector without the change
 * being seen mid-frame.  EVERY caller follows it with writes to the DL pointer ($D402/$D403)
 * and/or the VDSLST shadow ($0200/$0201) — this is a tear-avoidance beam sync for that swap.
 *
 * Atari/SDL: spin on VCOUNT ($D40B, the vertical scan position at two-line resolution) until
 * it reaches $7A (scanline ~244, below the 200-line display).
 *
 * Amiga: NO-OP.  The display is copper-driven; the DL-pointer/VDSLST writes the caller makes
 * are all ignored (the real scene swap is a CopperList install that latches at the next
 * vblank), so there is no display list to tear and the beam sync guards nothing.  Skipping it
 * reclaims up to ~1 frame (~15 ms) of otherwise-pointless beam-wait per call — the bulk of the
 * tunnel->stars setup "burst" was this wait, not the copy/field work around it. */
void wait_vcount_ge_7a(void) {
#ifndef ROF_PLATFORM_AMIGA
    while (bus_read(0xD40B) < 0x7A)
        ;
#endif
}

/* copy_192_to_1800 @ $75A5 — set the $00BB/$00BC dest pointer to $180F and copy
 * 192 bytes $350C+Y -> $180F+Y for Y=$C0..$01 (i.e. $1810..$18CF).  The 6502
 * writes via bus_write(ZP_IND_Y($BB)); the dest is RAM, so this is a plain mem[]
 * copy.  Pure leaf. */
void copy_192_to_1800(void) {
    dl_y1 = 0x0F;
    dl_y2 = 0x18;
    /* Copy 192 bytes $350D->$1810.  NOTE: a uint32_t/word copy is IMPOSSIBLE here — the dst
     * ($1810) is even/4-aligned but the src ($350D) is ODD, so the two regions have different
     * alignment and the 68000 faults on any misaligned word/long access (there is no common
     * boundary to batch to).  Best available is a byte pointer-walk (move.b (a0)+,(a1)+)
     * through a non-volatile alias so GCC emits the mem-to-mem post-increment form instead of
     * recomputing two (d16,a0,d.w) indexed addresses per byte.  ISR-safe: the launch VBI never
     * writes the $1000 field ($1810 lives in it) and $350D is read-only cockpit data.  dst and
     * src are disjoint, so the forward walk is byte-identical to the original high->low order. */
    const uint8_t* s = (const uint8_t*)mem + 0x350D;
    uint8_t* d = (uint8_t*)mem + 0x1810;
    for (unsigned n = 0xC0u; n; n--)
        *d++ = *s++;
}

/* build_row_addr_table @ $7460 — build the 85-entry ($55) per-scanline base-
 * address table at $073D (lo) / $0793 (hi) from base $00C4:$00C3 and 8-bit stride
 * $00C1: entry[0] = base, entry[i+1] = entry[i] + stride (16-bit accumulate).
 * Pure leaf (mem-only; exit regs dead). */
void build_row_addr_table(void) {
    row_base_lo = row_table_base_lo;
    row_base_hi = row_table_base_hi;
    uint8_t stride = row_table_stride;
    for (uint8_t y = 0x00; y != 0x55; y++) {
        uint16_t lo = (uint16_t)mem[MEM_row_base_lo + y] + stride;   /* CLC; ADC stride */
        mem[0x073E + y] = (uint8_t)lo;
        mem[0x0794 + y] = (uint8_t)(mem[MEM_row_base_hi + y] + (lo >> 8));  /* ADC #0 + carry */
    }
}

/* build_line_addr_table_2000 @ $65DF — base $2000, stride $2E; tail build_row_addr_table. */
void build_line_addr_table_2000(void) {
    row_table_stride = 0x2E;
    row_table_base_lo = 0x00;
    row_table_base_hi = 0x20;
    build_row_addr_table();
}

/* build_line_addr_table_1000_stride @ $65D2 — base $1000, stride = entry cpu.A;
 * tail build_row_addr_table. */
void build_line_addr_table_1000_stride(void) {
    row_table_stride = cpu.A;
    row_table_base_lo = 0x00;
    row_table_base_hi = 0x10;
    build_row_addr_table();
}

/* build_line_addr_table_1000 @ $65D0 — A=$2E, tail build_line_addr_table_1000_stride. */
void build_line_addr_table_1000(void) {
    cpu.A = 0x2E;
    build_line_addr_table_1000_stride();
}

/* init_object_positions @ $6B85 — zero the scroll counters $08D1/$08D2/$08D3,
 * then build the 22-entry world-position word array $08A4(lo)/$08A5(hi) by adding
 * the 16-bit base $2EE0 to each word of the $6E2D source table (Y=$2A..$00 step 2).
 * Pure leaf. */
void init_object_positions(void) {
    obj_advance_lo = 0x00;
    obj_advance_hi = 0x00;
    obj_anim_frame = 0x00;
    /* Add the 16-bit base $2EE0 to each of the 22 words of the $6E2D source table, storing to
     * the $08A4(lo)/$08A5(hi) world-position array.  Walk src/dst with decrementing pointers
     * (matches the original Y=$2A..0 step-2 order) instead of recomputing base+Y indexed
     * addresses each step.  Byte-wise (not word) — the +$E0/carry math + little-endian layout
     * rule out a single word op. */
    const uint8_t* s   = (const uint8_t*)mem + 0x6E2D + 0x2A;   /* &src[$2A] */
    uint8_t*       dlo = (uint8_t*)mem + MEM_obj_pos_table + 0x2A;
    uint8_t*       dhi = (uint8_t*)mem + 0x08A5 + 0x2A;
    for (int i = 0; i < 22; i++) {
        uint16_t lo = (uint16_t)s[0] + 0xE0;                    /* CLC; ADC #$E0 */
        *dlo = (uint8_t)lo;
        *dhi = (uint8_t)(s[1] + 0x2E + (lo >> 8));              /* ADC #$2E + carry */
        s -= 2; dlo -= 2; dhi -= 2;
    }
}

/* audio_timer_setup @ $712D — silence the music gate ($00E7), $0655 and $00E5,
 * clear the POKEY audio timers ($D201/$D203/$D205/$D207) and set AUDCTL=$60.
 * The POKEY writes go through bus_write (Paula/ignored on Amiga; not in mem[], so
 * identical side effect in both runs).  Leaf.
 *
 * ⚠ FAITHFUL EXIT REGISTERS: the 6502 routine returns A=0 (its final `LDA #0`
 * feeds every store; STA leaves A untouched) and Y=$60 (`LDY #$60; STY $D208`).
 * sound_stop ($5A0E) RELIES on A=0: `JSR $712D; STA $022F; STA $006C` stores this
 * A into sound_active_flag ($006C).  As a `void` twin that left cpu.A stale,
 * sound_stop wrote the caller's stale A ($FF on the level-select→START path) into
 * $006C instead of 0, so the game_main_loop→boot_standby_launch_driver re-entry (enters at
 * $3D48, past the $3D42 zeroing) saw $006C!=0 and took the ~10 s Standby-
 * construction path instead of the fast L_6118 path.  make validate never caught
 * it (diffs mem[] only; exit cpu regs are treated as incidental).  Restore them. */
void audio_timer_setup(void) {
    mem[0x00E7] = 0x00;
    mem[0x0655] = 0x00;
    mem[0x00E5] = 0x00;
    bus_write(0xD201, 0x00);
    bus_write(0xD203, 0x00);
    bus_write(0xD205, 0x00);
    bus_write(0xD207, 0x00);
    bus_write(0xD208, 0x60);
    cpu.A = 0x00;   /* faithful exit A (sound_stop stores this into $006C/$022F) */
    cpu.Y = 0x60;   /* faithful exit Y */
}

/* Four preset terrain-height samples ($6B5F, 4 bytes): the non-flat heights the
 * generator picks from.  (Unnamed in symbols.csv — see docs/rename.md.) */
#define TERRAIN_HEIGHT_TABLE 0x6B5Fu

/* Base of the four parallel terrain-height column buffers ($0C32/$0D32/$0E32/$0F32,
 * 0x100 apart), one per starfield/terrain layer, indexed by column.  (Unnamed in
 * symbols.csv — see docs/rename.md.) */
#define TERRAIN_COL_BUF(layer, col) mem[0x0C32u + (layer) * 0x100u + (col)]

/* The 17-bit POKEY RANDOM LFSR state (x^17+x^5+1), defined (extern "C") in PlatformAmiga.cpp.
 * Exposed so fill_terrain_columns can hold it in a register across its whole loop instead of
 * paying a cross-TU rof_pokey_random() call per read. */
#ifdef ROF_PLATFORM_AMIGA
extern uint32_t rof_lfsr_state;
#endif

/* random_terrain_height @ $6B47 — pick one sparse terrain-height sample.
 * Reads the POKEY RANDOM register: 31 times out of 32 the ground is flat (height 0);
 * 1 time in 32 (when the low 5 random bits are all zero) it takes a second RANDOM read
 * and returns one of the four preset heights from TERRAIN_HEIGHT_TABLE.  Consumes 1 or 2
 * RANDOM reads, advancing the LFSR by exactly as many (bus_read routes $D20A to the direct
 * rof_pokey_random() LFSR step on Amiga; SDL keeps the reference path so validate matches).
 *
 * The value-returning core is the hot path — fill_terrain_columns runs it ~356x in the
 * one-shot tunnel->stars field build; returning a plain uint8_t lets the callers inline it
 * and skip the LDA() N/Z-flag computation and the void-shim call frame. */
static inline uint8_t random_terrain_height_core(void) {
    if ((bus_read(0xD20Au) & 0x1F) != 0)                       /* 31/32: flat ground */
        return 0;
    return mem[TERRAIN_HEIGHT_TABLE + (bus_read(0xD20Au) & 0x03)];   /* 1/32: preset height */
}
void random_terrain_height(void) {   /* 6502-ABI shim: result in A (validate + any A-consuming caller) */
    LDA(random_terrain_height_core());
}

/* fill_horizontal_span @ $665D — fill pattern $00B9 across a horizontal run on
 * two scanlines.  Row pointers $80/$81 = addr-table[$009E], $B7/$B8 = table[$009F].
 * The column endpoints come from $009C>>1 and $009D>>1 (one adjusted by +/-1 per
 * the shifted-out bit of $009D), giving the inclusive count $00DF = hi - lo and the
 * start index Y = hi.  Writes the pattern to ($80)+Y and ($B7)+Y down to lo.  Leaf. */
/* Fill the horizontal run [y_hi-cnt .. y_hi] of the uniform pattern byte `pat` on both
 * scanlines (base1, base2).  Since every written byte is `pat`, this is two memsets — the
 * FrameDrawAssembler.s twin broadcasts pat into a long and batches with move.l (the C is one
 * volatile byte-store per pixel, which GCC can't batch).  Byte-identical: same bytes set. */
void fill_horizontal_span_core_c(uint16_t base1, uint16_t base2, uint8_t y_hi, uint8_t cnt, uint8_t pat) {
    uint8_t y = y_hi;
    for (;;) {
        mem[(uint16_t)(base1 + y)] = pat;
        mem[(uint16_t)(base2 + y)] = pat;
        y = (uint8_t)(y - 1);
        if (((uint8_t)(cnt - 1)) & 0x80) break;           /* BPL: loop while N clear */
        cnt = (uint8_t)(cnt - 1);
    }
}
/* Shared verify-snapshot window for both span twins (the $1000 door field). */
#define FVS_WIN_BASE 0x1000u
#define FVS_WIN_LEN  0x1100u                         /* $1000..$20FF */
#if defined(ROF_FRAMEDRAW_ASM) && defined(ROF_FRAMEDRAW_VERIFY)
extern void fill_horizontal_span_core_asm(uint16_t base1, uint16_t base2, uint8_t y_hi, uint8_t cnt, uint8_t pat);
volatile unsigned long g_fhsCalls = 0, g_fhsMismatch = 0, g_fhsFirstBad = 0;
void fill_horizontal_span_core(uint16_t base1, uint16_t base2, uint8_t y_hi, uint8_t cnt, uint8_t pat) {
    extern volatile unsigned long g_fvsCalls;   /* reuse the FVS_WIN door-field snapshot window */
    g_fhsCalls++;
    uint8_t* const M = (uint8_t*)mem;
    static uint8_t snap[FVS_WIN_LEN], asmv[FVS_WIN_LEN];
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) snap[i] = M[FVS_WIN_BASE + i];
    fill_horizontal_span_core_asm(base1, base2, y_hi, cnt, pat);
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) asmv[i] = M[FVS_WIN_BASE + i];
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) M[FVS_WIN_BASE + i] = snap[i];
    fill_horizontal_span_core_c(base1, base2, y_hi, cnt, pat);
    int bad = 0;
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) if (M[FVS_WIN_BASE + i] != asmv[i]) bad = 1;
    if (bad) { if (!g_fhsMismatch) g_fhsFirstBad = g_fhsCalls; g_fhsMismatch++; }
}
#elif defined(ROF_FRAMEDRAW_ASM)
extern void fill_horizontal_span_core(uint16_t base1, uint16_t base2, uint8_t y_hi, uint8_t cnt, uint8_t pat);  /* FrameDrawAssembler.s */
#else
static void fill_horizontal_span_core(uint16_t base1, uint16_t base2, uint8_t y_hi, uint8_t cnt, uint8_t pat) {
    fill_horizontal_span_core_c(base1, base2, y_hi, cnt, pat);
}
#endif

void fill_horizontal_span(void) {
    uint8_t y1 = draw_row_top;
    sync_flag = mem[MEM_row_base_lo + y1];
    dl_ptr_lo = mem[MEM_row_base_hi + y1];
    uint8_t y2 = draw_row_bottom;
    frame_counter = mem[MEM_row_base_lo + y2];
    draw_row_ptr2_hi = mem[MEM_row_base_hi + y2];

    dl_ptr_hi = (uint8_t)(draw_x_left >> 1);            /* lo  = $9C>>1 */
    uint8_t c2  = (uint8_t)(draw_x_right & 1);             /* shifted-out bit of $9D */
    screen_ptr_lo = (uint8_t)(draw_x_right >> 1);            /* hi  = $9D>>1 */
    if (!c2) screen_ptr_lo = (uint8_t)(screen_ptr_lo - 1);    /* BCC: DEC hi */
    else     dl_ptr_hi = (uint8_t)(dl_ptr_hi + 1);    /*      INC lo */

    uint8_t y  = screen_ptr_lo;                             /* Y = hi (before SBC) */
    uint8_t lo = dl_ptr_hi;
    uint8_t cnt = (uint8_t)(screen_ptr_lo - dl_ptr_hi);   /* SEC SBC: count = hi - lo */
    span_pixel_count = cnt;
    uint8_t pat = draw_pattern_byte;
    /* Both row bases are loop-invariant (set above from the addr table) and the screen
     * field is RAM, so hoist them and write mem[] directly — no per-byte bus dispatch. */
    uint16_t base1 = (uint16_t)(sync_flag | (dl_ptr_lo << 8));
    uint16_t base2 = (uint16_t)(frame_counter | (draw_row_ptr2_hi << 8));
    fill_horizontal_span_core(base1, base2, y, cnt, pat);
    span_pixel_count = 0xFF;                                    /* faithful exit: count ran to -1 */
    cpu.Y = lo;                                           /* last Y set at loop top (incidental) */
}

/* Set one 2-bit pixel into the screen byte at rowBase+byteOff: OR in the pixel's colour bits,
 * then AND away the other pixel's bits.  Both masks are indexed by maskSel (which encodes the
 * colour AND which of the four 2-bit pixels in the byte to touch).  The screen field
 * ($1000/$2000) is plain RAM, so we read-modify-write mem[] directly (bus_read/bus_write would
 * range-check hardware on every pixel; there is no hardware here to write). */
static inline void plot_masked_pixel_core(uint16_t rowBase, uint8_t byteOff, uint8_t maskSel) {
    uint16_t a = (uint16_t)(rowBase + byteOff);
    mem[a] = (uint8_t)((mem[a] | mem[MEM_pixel_or_mask_tbl + maskSel])
                                & mem[MEM_pixel_and_mask_tbl + maskSel]);
}

/* Plot one pixel at pixel-column `col` on the row whose byte-address base is rowBase, in colour
 * `colour`.  Two pixels share a byte, so the byte offset is col>>1; odd columns select the high
 * pixel by biasing the mask index by 9 (the Atari's ADC #$08 + shifted-out LSR carry). */
static inline void plot_pixel_masked_core(uint16_t rowBase, uint8_t col, uint8_t colour) {
    uint8_t maskSel = (col & 1u) ? (uint8_t)(colour + 9u) : colour;
    plot_masked_pixel_core(rowBase, (uint8_t)(col >> 1), maskSel);
}

/* $66DE 6502-ABI shim (validation oracle): row pointer in $80/$81 (misnamed sync_flag/dl_ptr_lo
 * — see docs/rename.md), byte offset in Y, mask index in X. */
void plot_masked_pixel(void) {
    plot_masked_pixel_core((uint16_t)(sync_flag | (dl_ptr_lo << 8)), cpu.Y, cpu.X);
}

/* $66D5 6502-ABI shim: pixel column in A, colour from $0094 (draw_color_idx), row ptr $80/$81.
 * Transliterated oracle callers (fill_vertical_span__t6502 etc.) reuse the registers this leaves
 * — Y = byte offset, X = mask index — for a following plot_masked_pixel, so reproduce them. */
void plot_pixel_masked(void) {
    uint8_t col = cpu.A;
    uint8_t byteOff = (uint8_t)(col >> 1);
    uint8_t maskSel = (col & 1u) ? (uint8_t)(draw_color_idx + 9u) : draw_color_idx;
    plot_masked_pixel_core((uint16_t)(sync_flag | (dl_ptr_lo << 8)), byteOff, maskSel);
    cpu.Y = byteOff;
    cpu.X = maskSel;
}

/* set_row_ptr @ $66C8 — load the bitmap row pointer $0080/$0081 from the per-
 * scanline base-address table $073D(lo)/$0793(hi) indexed by entry cpu.Y. */
void set_row_ptr(void) {
    sync_flag = mem[MEM_row_base_lo + cpu.Y];
    dl_ptr_lo = mem[MEM_row_base_hi + cpu.Y];
}

/* set_row_ptr_from_count @ $66C6 — Y = row counter $0092, tail set_row_ptr. */
void set_row_ptr_from_count(void) {
    cpu.Y = draw_row;
    set_row_ptr();
}

/* fill_vertical_span @ $669C — plot a vertical run of pixels down rows $009F..$009E.
 * Per row: set the row pointer $80/$81 from the addr table, then masked-plot column
 * $009C (plot_pixel_masked, which also leaves the mask index in cpu.X) and the glyph
 * column $009D>>1 reusing that mask (plot_masked_pixel).  $00DF = $009E-$009F
 * is the inclusive row count; $0084 walks the row index.
 *
 * Typed core: within one call the two columns, the (shared) mask index and its OR/AND
 * mask bytes, and the byte offsets within a row are all loop-invariant — only the row
 * base changes.  Hoist them into locals so the per-row body is two addr-table reads +
 * two direct RAM read-modify-writes, instead of re-reading volatile zero page + the
 * mask tables on every pixel via plot_pixel_masked/plot_masked_pixel.  The screen
 * field ($1000/$2000) is RAM, so the plots go straight to mem[].  Domain: rows r0<=r1
 * indexing the $073D/$0793 table into bitmap RAM (the real caller's contract — see the
 * fixture in tools/validate_native.c).  This was the bulk of the pre-door ring-draw
 * freeze on the 68000. */
void fill_vertical_span_core_c(uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR, uint8_t maskSel) {
    uint8_t x    = (colL & 1u) ? (uint8_t)(maskSel + 9u) : maskSel;  /* mask index (both edges) */
    uint8_t orm  = mem[MEM_pixel_or_mask_tbl + x];
    uint8_t am   = mem[MEM_pixel_and_mask_tbl + x];
    uint8_t offL = (uint8_t)(colL >> 1);
    uint8_t offR = (uint8_t)(colR >> 1);
    uint8_t cnt  = (uint8_t)(r1 - r0);               /* $00DF; loop runs cnt+1 rows */
    uint8_t row  = r0;
    for (;;) {
        uint16_t base = (uint16_t)(mem[MEM_row_base_lo + row] | (mem[MEM_row_base_hi + row] << 8));
        uint16_t aL = (uint16_t)(base + offL);
        uint16_t aR = (uint16_t)(base + offR);
        mem[aL] = (uint8_t)((mem[aL] | orm) & am);
        mem[aR] = (uint8_t)((mem[aR] | orm) & am);
        row++;
        if (((uint8_t)(cnt - 1)) & 0x80) break;      /* BPL: stop after cnt+1 rows */
        cnt = (uint8_t)(cnt - 1);
    }
}

/* Dispatcher seam (asm-migration-plan), mirrors project_terrain_points_core.  On the Amiga
 * (ROF_FRAMEDRAW_ASM) fill_vertical_span_core is the hand-written m68k twin in
 * FrameDrawAssembler.s (scattered row-stride masked RMW — pointer-walked row table, masks
 * pinned in registers); elsewhere it is the clean-C oracle above.  This is the dominant cost
 * of the standby->doors door-frame draw (~71ms of the ~93ms span loop). */
#if defined(ROF_FRAMEDRAW_ASM) && defined(ROF_FRAMEDRAW_VERIFY)
/* On-target differential: run the asm twin on the live field, snapshot the door-field window
 * it may touch, restore it, run the C oracle on the same input, and compare.  The C oracle's
 * output stays LIVE so an asm bug can't corrupt the scene.  Read via amiga/framedraw_verify.gdb. */
extern void fill_vertical_span_core_asm(uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR, uint8_t maskSel);
volatile unsigned long g_fvsCalls = 0, g_fvsMismatch = 0, g_fvsFirstBad = 0;
void fill_vertical_span_core(uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR, uint8_t maskSel) {
    g_fvsCalls++;
    uint8_t* const M = (uint8_t*)mem;
    static uint8_t snap[FVS_WIN_LEN], asmv[FVS_WIN_LEN];
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) snap[i] = M[FVS_WIN_BASE + i];
    fill_vertical_span_core_asm(r0, r1, colL, colR, maskSel);
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) asmv[i] = M[FVS_WIN_BASE + i];
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) M[FVS_WIN_BASE + i] = snap[i];
    fill_vertical_span_core_c(r0, r1, colL, colR, maskSel);
    int bad = 0;
    for (unsigned i = 0; i < FVS_WIN_LEN; i++) if (M[FVS_WIN_BASE + i] != asmv[i]) bad = 1;
    if (bad) { if (!g_fvsMismatch) g_fvsFirstBad = g_fvsCalls; g_fvsMismatch++; }
}
#elif defined(ROF_FRAMEDRAW_ASM)
extern void fill_vertical_span_core(uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR, uint8_t maskSel);  /* FrameDrawAssembler.s */
#else
static void fill_vertical_span_core(uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR, uint8_t maskSel) {
    fill_vertical_span_core_c(r0, r1, colL, colR, maskSel);
}
#endif

void fill_vertical_span(void) {
    uint8_t r0 = draw_row_bottom, r1 = draw_row_top;
    uint8_t colL = draw_x_left, colR = draw_x_right, maskSel = draw_color_idx;
    fill_vertical_span_core(r0, r1, colL, colR, maskSel);
    /* Hand the pair to the platform painter.  draw_symmetric_span_loop deliberately calls
     * fill_vertical_span_CORE directly, so this shim is exactly plot_terrain_span's writes and
     * there is no double-paint.  Row r1's table entry identifies the field, as elsewhere. */
    ROF_TUNNEL_VSPAN((uint16_t)(mem[MEM_row_base_lo + r1] | (mem[MEM_row_base_hi + r1] << 8)),
                     r0, r1, colL, colR, maskSel);
    /* Faithful exit state: $0084 = last row + 1; $80/$81 = addr table[last row];
     * $00DF = $FF; cpu.X = mask index, cpu.Y = colR>>1 (cpu state is incidental). */
    screen_ptr_hi = (uint8_t)(r1 + 1);
    sync_flag = mem[MEM_row_base_lo + r1];
    dl_ptr_lo = mem[MEM_row_base_hi + r1];
    span_pixel_count = 0xFF;
    cpu.X = (colL & 1u) ? (uint8_t)(maskSel + 9u) : maskSel;
    cpu.Y = (uint8_t)(colR >> 1);
}

/* plot_pixel_2bpp @ $6C92 — pack the screen byte at ($80)+Y into a 2-bits-per-pixel
 * cell: read it, then 4 rounds of {force both top bits if either is set; ROL twice}
 * through the carry chain (seeded by the entry carry), a final ROL, and write back.
 * Preserves cpu.X (the 6502 saves/restores it on the stack).
 *
 * The transform output is a pure function of (input byte, entry carry) — no other input —
 * so the 4-round bit-serial pack is replaced by a 512-entry lookup (plot2bpp_lut[carry][byte]),
 * built once from the exact loop.  This is the hot per-pixel op of the planet-approach body
 * fill (draw_vline_pair calls it twice per row across many rows x 22 objects as the planet
 * fills the viewport = the launch-cinematic cineGap spike); the LUT drops it from ~13
 * ops/branch to one indexed read + one RMW.  Byte-identical (make validate). */
static uint8_t plot2bpp_lut[2][256];
static uint8_t plot2bpp_lut_ready = 0;
static void build_plot2bpp_lut(void) {
    for (int ci = 0; ci < 2; ci++)
        for (int in = 0; in < 256; in++) {
            uint8_t c = (uint8_t)ci, acc = (uint8_t)in;
            for (int i = 0; i < 4; i++) {
                if ((acc & 0xC0) == 0) acc |= 0xC0;
                uint8_t nc = (uint8_t)((acc >> 7) & 1); acc = (uint8_t)((acc << 1) | c); c = nc;
                nc = (uint8_t)((acc >> 7) & 1);          acc = (uint8_t)((acc << 1) | c); c = nc;
            }
            plot2bpp_lut[ci][in] = (uint8_t)((acc << 1) | c);   /* final ROL */
        }
    plot2bpp_lut_ready = 1;
}
void plot_pixel_2bpp(void) {
    if (!plot2bpp_lut_ready) build_plot2bpp_lut();
    dl_ptr_hi = 0xC0;                                     /* BIT mask side effect, set once */
    uint16_t a = ZP_IND_Y(0x80);                          /* screen field is RAM: direct mem[] */
    mem[a] = plot2bpp_lut[cpu.C & 1][mem[a]];             /* cpu.X untouched (preserved) */
}

/* draw_symmetric_span_loop @ $6642 — draw one concentric-rectangle group ($0096 nested
 * rectangles) of the door frame.  Each rectangle is a horizontal edge pair (top+bottom rows)
 * + a vertical edge pair (left+right columns) in the current pattern colour; after each, the
 * four edges step one pixel inward.
 *
 * The four edge coordinates ($9C/$9D left/right, $9E/$9F top/bottom) are held in LOCALS for
 * the whole run and passed straight to fill_horizontal_span_core / fill_vertical_span_core —
 * eliminating the per-rectangle ZP round-trip the old code paid (each fill_*_span shim re-read
 * those four coords + the pattern from zero page every call, ~10 volatile reads/rectangle).
 * The shims' ZP scratch is dead until return, so we write the coords + reproduce the exact
 * exit scratch the last fill pair left ONCE after the loop (byte-identical to the oracle). */
void draw_symmetric_span_loop(void) {
    uint8_t colour = draw_color_idx;
    uint8_t pat = (uint8_t)(colour | mem[MEM_pixel_or_mask_tbl + colour]);
    draw_pattern_byte = pat;

    uint8_t xL = draw_x_left, xR = draw_x_right, top = draw_row_top, bot = draw_row_bottom;
    uint8_t count = span_row_count;
    uint8_t lxL = xL, lxR = xR, ltop = top, lbot = bot;   /* last-drawn coords, for the exit scratch */
    for (;;) {
        uint16_t baseTop = (uint16_t)(mem[MEM_row_base_lo + top] | (mem[MEM_row_base_hi + top] << 8));
        uint16_t baseBot = (uint16_t)(mem[MEM_row_base_lo + bot] | (mem[MEM_row_base_hi + bot] << 8));
        uint8_t lo = (uint8_t)(xL >> 1);              /* left byte column  */
        uint8_t hi = (uint8_t)(xR >> 1);              /* right byte column */
        if (xR & 1) lo = (uint8_t)(lo + 1);           /* odd right edge -> nudge left in  */
        else        hi = (uint8_t)(hi - 1);           /* even right edge -> nudge right in */
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned long g_dfHCalls, g_dfHCols, g_dfVCalls, g_dfVRows;
          g_dfHCalls++; g_dfHCols += (unsigned)(uint8_t)(hi - lo) + 1u;
          g_dfVCalls++; g_dfVRows += (unsigned)(uint8_t)(top - bot) + 1u; }
#endif
        fill_horizontal_span_core(baseTop, baseBot, hi, (uint8_t)(hi - lo), pat);
        fill_vertical_span_core(bot, top, xL, xR, colour);
        /* Hand this rectangle to the platform so it can paint the ring straight into its bitmap.
         * Emitted HERE because this is the only point that knows a rectangle and its colour for
         * both the forward ring (draw_ring_frame_step) and the reverse one (step_accum_sub_7e).
         * baseTop identifies which field the row table currently points at. */
        ROF_TUNNEL_RECT(baseTop, top, bot, xL, xR, lo, hi, colour);
        lxL = xL; lxR = xR; ltop = top; lbot = bot;
        xL = (uint8_t)(xL - 1); xR = (uint8_t)(xR + 1);
        top = (uint8_t)(top + 1); bot = (uint8_t)(bot - 1);
        if ((uint8_t)(--count) == 0) break;           /* DEC $0096; BNE */
    }
    draw_x_left = xL; draw_x_right = xR; draw_row_top = top; draw_row_bottom = bot;
    span_row_count = 0;
    /* Reproduce the ZP scratch the fill_horizontal_span + fill_vertical_span shims left on the
     * final rectangle (coords lxL/lxR/ltop/lbot): dl_ptr_hi/screen_ptr_lo = the horizontal
     * lo/hi byte columns; $80/$81 = row addr[top]; $B7/$B8 = row addr[bot]; $84 = top+1;
     * $DF = $FF. */
    {
        uint8_t lo = (uint8_t)(lxL >> 1);
        uint8_t hi = (uint8_t)(lxR >> 1);
        if (lxR & 1) lo = (uint8_t)(lo + 1);
        else         hi = (uint8_t)(hi - 1);
        dl_ptr_hi     = lo;
        screen_ptr_lo = hi;
        frame_counter    = mem[MEM_row_base_lo + lbot];
        draw_row_ptr2_hi = mem[MEM_row_base_hi + lbot];
        sync_flag = mem[MEM_row_base_lo + ltop];
        dl_ptr_lo = mem[MEM_row_base_hi + ltop];
        screen_ptr_hi = (uint8_t)(ltop + 1);
        span_pixel_count = 0xFF;
    }
}

/* gen_terrain_column @ $6B2E — fill one column (index `col`) of all four parallel terrain-
 * height buffers with four independent random height samples (one per layer).  Still used by
 * scroll_field_columns (per-VBI append) + the standalone validate twin; fill_terrain_columns
 * inlines its own copy of this logic (see below). */
static void gen_terrain_column_core(uint8_t col) {
    TERRAIN_COL_BUF(0, col) = random_terrain_height_core();
    TERRAIN_COL_BUF(1, col) = random_terrain_height_core();
    TERRAIN_COL_BUF(2, col) = random_terrain_height_core();
    TERRAIN_COL_BUF(3, col) = random_terrain_height_core();
}
void gen_terrain_column(void) {   /* 6502-ABI shim: column index in Y, last height left in A */
    uint8_t col = cpu.Y;
    gen_terrain_column_core(col);
    cpu.A = TERRAIN_COL_BUF(3, col);
}

/* fill_terrain_columns @ $6AE5 — one-shot stars/planet field build: fill all 89 columns
 * (indices $59..$01, right-to-left; column 0 left untouched) of the four parallel terrain-
 * height buffers with fresh random samples.  Each sample is flat (0) 31/32 of the time, else
 * one of four preset heights picked by a second random read.
 *
 * Fully inlined into one function — this runs 356 samples in the tunnel->stars build burst,
 * and the per-sample rof_pokey_random / random_terrain_height / gen_terrain_column calls each
 * cost a 68000 jsr/rts + stack frame.  On Amiga the 17-bit LFSR (x^17+x^5+1) is held in a
 * local for the whole loop — one load at entry, one store at exit — instead of a memory
 * read+write per read: it's our own RNG (not POKEY-cycle-accurate), so a launch-VBI RANDOM
 * read landing mid-loop just reshuffles cosmetic terrain, no correctness concern.  SDL keeps
 * the reference bus_read($D20A) path so make validate stays byte-identical to the 6502 oracle. */
void fill_terrain_columns_core_c(void) {
#ifdef ROF_PLATFORM_AMIGA
    uint32_t s = rof_lfsr_state;                 /* LFSR in a register for the whole loop */
    /* Tap = bit16 ^ bit4.  bit16 via (s>>16) (GCC: clr.w+swap, 4cyc); bit4 from the LOW BYTE
     * ((uint8_t)s >> 4) so it's an 8cyc byte shift, not a 16cyc long lsr.l #4.  Returns the
     * new state's low byte; callers mask it with byte ANDs. */
  #define NEXT_RAND8() ( s = ((s << 1) | (((s >> 16) ^ ((uint8_t)s >> 4)) & 1u)) & 0x1FFFFu, (uint8_t)s )
#else
  #define NEXT_RAND8() bus_read(0xD20Au)
#endif
    for (uint8_t col = 0x59; col != 0; col--) {
        for (uint16_t base = 0x0C32u; base <= 0x0F32u; base += 0x100u) {
            uint8_t h = 0;                                        /* 31/32: flat ground */
            uint8_t r = NEXT_RAND8();
            if ((r & 0x1F) == 0)                                  /* 1/32: pick a preset height */
                h = mem[TERRAIN_HEIGHT_TABLE + (NEXT_RAND8() & 0x03)];
            mem[base + col] = h;
        }
    }
#undef NEXT_RAND8
#ifdef ROF_PLATFORM_AMIGA
    rof_lfsr_state = s;                          /* commit the advanced LFSR back once */
#endif
}

/* Dispatcher seam (asm-migration-plan Phase 3), mirrors project_terrain_points_core.  On the
 * Amiga (ROF_FILLTERR_ASM) fill_terrain_columns_core is the hand-written m68k twin in
 * FillTerrainAssembler.s (byte-width LFSR ANDs + displacement-addressed stores, which GCC won't
 * emit); elsewhere it is the clean-C oracle above. */
#if defined(ROF_FILLTERR_ASM) && defined(ROF_FILLTERR_VERIFY)
/* On-target differential (fires once — fill_terrain_columns is a one-shot): run the asm twin on
 * the live state from the current LFSR seed, snapshot the 4 buffers ($0C32..$0F8B window) + the
 * resulting LFSR, restore both, run the C oracle from the same seed, and compare.  The C oracle's
 * output stays LIVE so a bug can't corrupt the scene.  Read via amiga/fillterr_verify.gdb. */
extern void fill_terrain_columns_core_asm(void);
extern uint32_t rof_lfsr_state;
volatile unsigned long g_fillterrCalls = 0, g_fillterrMismatch = 0,
                       g_fillterrFirstBad = 0, g_fillterrLfsrBad = 0;
#define FILLTERR_WIN 0x35Au                      /* $0C32..$0F8B covers all 4 buffers + gaps */
void fill_terrain_columns_core(void) {
    g_fillterrCalls++;
    uint8_t* const M = (uint8_t*)mem;
    uint32_t seed = rof_lfsr_state;
    static uint8_t snap[FILLTERR_WIN], asmv[FILLTERR_WIN];
    for (unsigned i = 0; i < FILLTERR_WIN; i++) snap[i] = M[0x0C32u + i];
    fill_terrain_columns_core_asm();
    uint32_t asmLfsr = rof_lfsr_state;
    for (unsigned i = 0; i < FILLTERR_WIN; i++) asmv[i] = M[0x0C32u + i];
    rof_lfsr_state = seed;
    for (unsigned i = 0; i < FILLTERR_WIN; i++) M[0x0C32u + i] = snap[i];
    fill_terrain_columns_core_c();
    int bad = 0;
    for (unsigned i = 0; i < FILLTERR_WIN; i++) if (M[0x0C32u + i] != asmv[i]) bad = 1;
    if (bad) { if (!g_fillterrMismatch) g_fillterrFirstBad = g_fillterrCalls; g_fillterrMismatch++; }
    if (asmLfsr != rof_lfsr_state) g_fillterrLfsrBad++;
}
#elif defined(ROF_FILLTERR_ASM)
extern void fill_terrain_columns_core(void);     /* FillTerrainAssembler.s */
#else
void fill_terrain_columns_core(void) { fill_terrain_columns_core_c(); }
#endif

/* fill_terrain_columns @ $6AE5 — public entry (called by boot_standby_launch_driver; validated twin). */
void fill_terrain_columns(void) { fill_terrain_columns_core(); }

/* Advance the multi-byte scroll/distance accumulator ($00A1..$00A4) by $FF and return the
 * prospective new top byte (accumulator byte 3 + carry-out).  Byte 3 itself is NOT written
 * here — the caller decides whether to commit it.
 *
 * This faithfully reproduces a quirk of the Atari carry chain: after byte 1 is updated the
 * code does not reload a clean 0 before folding byte 2, so byte 2 becomes (byte1_new + byte2 +
 * carry) instead of (byte2 + carry).  Preserved exactly for byte-identical behaviour. */
/* Star-scroll generation counter: bumped once per emitted column by scroll_field_columns
 * (the stars/planet scroll, run in the standby VBI).  The Amiga star-sprite renderer reads
 * the delta since its last frame to learn how many rows the field scrolled up, so it can
 * advance the sprite window pointer + convert only the new rows (zero-copy scroll) instead of
 * rebuilding all rows.  A word so the main loop's read is atomic vs the VBI's bump. */
volatile unsigned short g_starScrollGen = 0;

/* add_multibyte_a1 @ $6AB5 — 32-bit little-endian accumulator add: operand `a` += byte 0
 * ($00A1); the carry chains up through bytes 1..3 ($00A2/$00A3/$00A4).  Returns the
 * PROSPECTIVE new top byte (byte 3 + final carry) WITHOUT storing it — the caller decides
 * whether to commit it to $00A4.  Faithful quirk ($6AC0): the byte-2 step reuses the
 * freshly-written byte-1 as its operand (the 6502 has no LDA #0 between those two ADCs). */
uint8_t add_multibyte_a1_core(uint8_t a) {
    uint16_t t = (uint16_t)a + scroll_accum_b0;                  /* byte 0 += operand       */
    scroll_accum_b0 = (uint8_t)t;
    uint8_t carry = (uint8_t)(t >> 8);
    t = (uint16_t)scroll_accum_b1 + carry;                       /* byte 1 += carry         */
    scroll_accum_b1 = (uint8_t)t;
    carry = (uint8_t)(t >> 8);
    t = (uint16_t)scroll_accum_b1 + scroll_accum_b2 + carry;     /* byte 2 += byte1_new+carry (quirk) */
    scroll_accum_b2 = (uint8_t)t;
    carry = (uint8_t)(t >> 8);
    return (uint8_t)(scroll_accum_b3 + carry);                   /* prospective new top byte */
}
/* 6502-ABI shim: operand in cpu.A, returns the top byte in cpu.A (callers CMP it). */
void add_multibyte_a1(void) { cpu.A = add_multibyte_a1_core(cpu.A); }

/* scroll_field_columns @ $6AEE — scroll the four parallel column buffers one column to the
 * left and generate a fresh rightmost column.  These four buffers ($0C32/$0D32/$0E32/$0F32,
 * TERRAIN_COL_BUF layers 0-3) are the scrolling terrain-height columns in flight and are
 * reused as the sparse star-field "players" during the stars/planet cinematic (same storage).
 * Called every VBI while a scroll is active; `gate` is the scroll-phase state ($0089).
 *
 *   gate < 4  : emit a column every frame (the fast, unpaced star scroll).
 *   gate >= 4 : pace the scroll with a distance accumulator that advances by $FF each frame —
 *               emit a column only on the frame its top byte ticks over.  When that top byte
 *               reaches $64 (100) the phase restarts (gate := 2); frames whose top byte is
 *               unchanged emit nothing (early return). */
void scroll_field_columns_core(uint8_t gate) {
    if ((int8_t)(gate - 4) >= 0) {                     /* paced phase: advance the accumulator */
        uint8_t top = add_multibyte_a1_core(0xFF);
        if (top == 0x64) {
            terrain_state = 0x02;                      /* distance 100 reached — restart the phase */
        } else {
            scroll_accum_b3 = top;
            if (scroll_accum_b3 == scroll_accum_prev)  /* top byte unchanged — no column this frame */
                return;
            scroll_accum_prev = scroll_accum_b3;
        }
    }
    /* Emit one column: shift all four buffers left (col y <- col y+1), dropping col 0. */
    g_starScrollGen++;                                 /* signal one row scrolled (star-sprite renderer) */
    for (uint8_t y = 0; y < 0x59; y++) {
        TERRAIN_COL_BUF(0, y) = TERRAIN_COL_BUF(0, y + 1);
        TERRAIN_COL_BUF(1, y) = TERRAIN_COL_BUF(1, y + 1);
        TERRAIN_COL_BUF(2, y) = TERRAIN_COL_BUF(2, y + 1);
        TERRAIN_COL_BUF(3, y) = TERRAIN_COL_BUF(3, y + 1);
    }
    sfx_toggle_8F >>= 1;                               /* advance the every-other-frame SFX toggle */
    gen_terrain_column_core(0x59);                     /* fill the new rightmost column (index $59) */
}

/* 6502-ABI shim: the scroll-phase gate ($0089) arrives in cpu.A (set by launch_anim_dispatch). */
void scroll_field_columns(void) {
    scroll_field_columns_core(cpu.A);
}

/* draw_frame_guide_columns @ $6620 — for 86 rows ($0092=$55..$00) set the row pointer
 * from the row counter, then masked-plot three columns ($009C, $009D, and
 * $00A0=$009D+1) into that row.  Tail of draw_frame_pattern_seq. */
void draw_frame_guide_columns(void) {
    uint8_t colL = draw_x_left, colR = draw_x_right;
    uint8_t colR1 = (uint8_t)(colR + 1);
    uint8_t colour = draw_color_idx;
    draw_iter_count = colR1;                              /* $A0 = $9D+1 (caller-visible scratch) */
    /* Walk the $073D/$0793 per-scanline base-address table with a descending pointer (row
     * $55..$00) instead of an indexed reload each row.  ISR-safe: build_row_addr_table (main
     * loop) is the only writer of this table. */
    const uint8_t* lop = (const uint8_t*)mem + MEM_row_base_lo + 0x55;
    const uint8_t* hip = (const uint8_t*)mem + MEM_row_base_hi + 0x55;
    for (int8_t row = 0x55; row >= 0; row--, lop--, hip--) {
        uint16_t rowBase = (uint16_t)(*lop | (*hip << 8));
        plot_pixel_masked_core(rowBase, colL,  colour);
        plot_pixel_masked_core(rowBase, colR,  colour);
        plot_pixel_masked_core(rowBase, colR1, colour);
    }
    /* Faithful exit scratch the 6502 leaves: row counter $92 ran to $FF, and the row pointer
     * $80/$81 holds table[0] (the last row set). */
    draw_row = 0xFF;
    sync_flag = mem[MEM_row_base_lo];
    dl_ptr_lo = mem[MEM_row_base_hi];
    /* Hand the three columns to the platform painter (row 0's table entry identifies the field,
     * the same way the rectangle hook uses baseTop). */
    ROF_TUNNEL_COLS((uint16_t)(sync_flag | (dl_ptr_lo << 8)), colL, colR, colR1, colour);
}

/* draw_frame_pattern_seq @ $65FB — the per-frame doors/tunnel frame drawer.  After
 * init_row_coords_9c seeds the edge coords, loop $00A0+1 (=20) times: read the next
 * span count from the $6E0F pattern table into $0096, draw one symmetric span pair,
 * and cycle the pattern selector $0094 through 1..6.  Finally DEC $0094 and tail
 * draw_frame_guide_columns.  (Entry A=$01 is positive so the $6602 BMI never fires.) */
void draw_frame_pattern_seq(void) {
    init_row_coords_9c();                                 /* seed the four edge coords $9C-$9F */
    draw_color_idx = 0x01;
    /* $6E0F = the door-frame span-thickness table (unnamed ROM table — see docs/rename.md):
     * one entry per concentric rectangle, index $13 down to 0 (20 rectangles).  Walk it with a
     * descending pointer; each entry is that rectangle's row count. */
    const uint8_t* thick = (const uint8_t*)mem + 0x6E0F + 0x13;
    uint8_t colour = 0x01;
    for (int8_t ring = 0x13; ring >= 0; ring--, thick--) {
        span_row_count = *thick;                          /* $0096 = this rectangle's thickness */
        ROF_TR_SRC_SET(g_trPreSite);                      /* re-armed every iteration: the ring VBI can preempt us */
        draw_symmetric_span_loop();                       /* draws it in the current colour */
        colour = (colour == 0x06) ? 0x01 : (uint8_t)(colour + 1);   /* cycle pattern colour 1..6 */
        draw_color_idx = colour;
    }
    draw_color_idx = (uint8_t)(colour - 1);               /* faithful: final DEC $0094 */
    draw_frame_guide_columns();                                /* tail: the three vertical guide columns */
}

/* ============================================================================
 * Standby/launch tunnel-ring + door-scroll cinematic driver (2026-07-11)
 *
 * These were hand-ported in rof_native_amiga.cpp, but they are pure mem[] 6502
 * logic — not Amiga-specific — so they belong here as faithful native twins.
 * Two Amiga specifics are guarded:
 *   - draw_ring_frame_step publishes the tunnel dirty-band globals (g_tun*) under
 *     #ifdef ROF_PLATFORM_AMIGA so RescueOnFractalus re-decodes only the band it
 *     just wrote (a full 86-row field scan is > 1 PAL frame on the 68000).
 *   - advance_history_6a4d skips its sfx_reorder_voice_slot tail on Amiga (the copper
 *     owns PMG and the confirmed standby cinematic omitted it); the validation/SDL
 *     build keeps the faithful tail so the twin matches its __t6502 oracle.
 * ============================================================================ */

/* advance_history_6a4d @ $6A4D — rotate the 6-byte colour ring $08D4-$08D9 up one slot (old
 * $08D9 wraps back into $08D4; feeds COLOR01-06).  If $008D (step_mode_flag) is negative, copy
 * $08D8 -> $0071 (display_flags).  Then bump $0685 ($0679[$0C]) by $06CC (history_ring_step),
 * saturating a wrap-to-0 result to $FF.  Atari tail: sfx_reorder_voice_slot (Y=$0C). */
void advance_history_6a4d(void) {
    uint8_t top = mem[0x08D9];
    mem[0x08D9] = mem[0x08D8];
    mem[0x08D8] = mem[0x08D7];
    mem[0x08D7] = mem[0x08D6];
    mem[0x08D6] = mem[0x08D5];
    mem[0x08D5] = color_ring;                             /* $08D5 <- $08D4 */
    color_ring = top;                                     /* $08D4 <- old $08D9 */
    if ((int8_t)step_mode_flag < 0) display_flags = mem[0x08D8];
    uint8_t s = (uint8_t)(mem[MEM_sfx_env_freq_val + 0x0C] + history_ring_step);
    mem[MEM_sfx_env_freq_val + 0x0C] = s ? s : 0xFF;
    cpu.Y = 0x0C;                                         /* the $0685 index is still live in Y */
    /* sfx_reorder_voice_slot ($5614) is the SFX VOICE-priority mixer (misnamed — NOT PMG): it
     * writes the swept AUDF2/AUDC2 (this slot's $0685 freq + $0677 vol) to POKEY via
     * sfx_voice_write_freq_ctrl.  It touches only mem[]/POKEY (Paula on Amiga), no display
     * hardware, so it is safe on the Amiga.  It was previously #ifndef'd out here on the wrong
     * assumption that it did copper-owned PMG work — that dropped the launch/tunnel noise-sweep
     * SFX (slot $0C), freezing ch1 at $b4/$8f as a constant buzz instead of the descending
     * whoosh that resolves.  Run it on both platforms (faithful; still matches the __t6502 oracle). */
    sfx_reorder_voice_slot();
}

/* dl_lms_scroll_up @ $69A9 — shift the top-half 3-byte LMS entries from $300C,X up to $3009,X
 * (one slot) until X reaches the top index $0097 (blit_row_counter). */
void dl_lms_scroll_up(void) {
    uint8_t x = 1, top = blit_row_counter;
    while (x != top) {
        mem[0x3009 + x] = mem[0x300C + x]; x++;
        mem[0x3009 + x] = mem[0x300C + x]; x += 2;
    }
}

/* dl_lms_scroll_down @ $69C3 — shift the bottom-half 3-byte LMS entries from $3087,Y down to
 * $308A,Y (one slot) until Y reaches the bottom index $0098 (dl_bottom_index). */
void dl_lms_scroll_down(void) {
    uint8_t y = 0x80, bot = dl_bottom_index;
    while (y != bot) {
        mem[0x308A + y] = mem[0x3087 + y]; y--;
        mem[0x308A + y] = mem[0x3087 + y]; y -= 2;
    }
}

/* dl_lms_push_top @ $6973 — write the 16-bit top push pointer $0080/$0081 (sync_flag lo /
 * dl_ptr_lo hi) into the top LMS entry at $300A,X (net X -= 3), then step the pointer up one
 * row (-$2E, the 46-byte mode-F stride). */
uint8_t dl_lms_push_top_core(uint8_t x) {
    mem[0x300A + x] = dl_ptr_lo; x--;                     /* hi byte */
    mem[0x300A + x] = sync_flag;                          /* lo byte */
    uint16_t p = (uint16_t)(sync_flag | (dl_ptr_lo << 8));
    p = (uint16_t)(p - 0x2E);
    sync_flag = (uint8_t)p; dl_ptr_lo = (uint8_t)(p >> 8);
    return (uint8_t)(x - 2);
}
void dl_lms_push_top(void) { cpu.X = dl_lms_push_top_core(cpu.X); }

/* dl_lms_push_bottom @ $698E — write the 16-bit bottom push pointer $0082/$0083 (dl_ptr_hi lo /
 * screen_ptr_lo hi) into the bottom LMS entry at $3089,Y (net Y += 3), then step the pointer
 * down one row (+$2E). */
uint8_t dl_lms_push_bottom_core(uint8_t y) {
    mem[0x3089 + y] = dl_ptr_hi; y++;                     /* lo byte */
    mem[0x3089 + y] = screen_ptr_lo;                      /* hi byte */
    uint16_t p = (uint16_t)(dl_ptr_hi | (screen_ptr_lo << 8));
    p = (uint16_t)(p + 0x2E);
    dl_ptr_hi = (uint8_t)p; screen_ptr_lo = (uint8_t)(p >> 8);
    return (uint8_t)(y + 2);
}
void dl_lms_push_bottom(void) { cpu.Y = dl_lms_push_bottom_core(cpu.Y); }

/* dl_doors_open_split_step @ $6953 — one door-open step: DEC $008A (terrain_scroll_counter); while it
 * is still non-zero, scroll both DL halves apart; on the step that reaches 0, arm the reveal
 * reload $008C=8 instead.  Then push a fresh leading LMS row into each half. */
void dl_doors_open_split_step(void) {
    if (--terrain_scroll_counter != 0) {
        dl_lms_scroll_down();
        dl_lms_scroll_up();
    } else {
        terrain_scroll_reload = 8;
    }
    dl_bottom_index = dl_lms_push_bottom_core(dl_bottom_index);
    blit_row_counter = dl_lms_push_top_core(blit_row_counter);
}

#ifdef ROF_PLATFORM_AMIGA
/* (The tunnel dirty-band publish that used to live here is gone.  Both ring directions are now
 * painted straight into the Amiga bitmap by the ROF_TUNNEL_* hooks as the spans are drawn, so
 * there is no $1000 decode left to bound and nothing to flag.  See docs/boost-tunnel-direct-
 * handoff.md.) */
/* Boost stars viewport ($2000) dirty flag — set by fill_region_2000 (the sole $2000 writer),
 * consumed by the boost stars render branch so it decodes only when the field content actually
 * changes (measured: twice per boost) instead of every frame.  See fill_region_2000. */
volatile uint8_t g_boostStarsDirty = 0;
#endif

/* draw_ring_frame_step @ $670D — draw ONE tunnel-ring frame group via draw_symmetric_span_loop
 * (thickness = $6E0F[$00A0]); when $00A0 < 6 (signed) clear $08D8 (the inner-ring colour)
 * instead.  Then DEC $00A0 and set $0088 = $00A0 + 1 — the gate that stops the ring cycle once
 * $00A0 wraps past 0 (boot_standby_launch_driver then advances to the stars/space phase). */
void draw_ring_frame_step(void) {
    uint8_t a0 = draw_iter_count;                         /* $00A0 */
    if ((int8_t)a0 >= 6) {                                /* CPY #$06; BMI -> clear branch */
        span_row_count = mem[0x6E0F + a0];                /* $0096 = ring thickness */
        ROF_TR_SRC_SAVE(trSave);                          /* ISR site: restore, we may have preempted the pre-draw */
        ROF_TR_SRC_SET(2);
        draw_symmetric_span_loop();                       /* steps $9C--/$9D++/$9E++/$9F-- */
        ROF_TR_SRC_RESTORE(trSave);
    } else {
        mem[0x08D8] = 0;                                  /* $671E: LDA #$00; STA $08D8 */
    }
    draw_iter_count--;                                    /* DEC $00A0 */
    vbi_flags = (uint8_t)(draw_iter_count + 1);           /* $0088 = $00A0 + 1 */
}

/* step_accum_add_75 @ $6A38 — advance the scroll accumulator by $75; if the new top byte is
 * unchanged, do nothing; else store it, and (when >= $90) step the tunnel-ring clear
 * (draw_ring_frame_step) before ALWAYS rotating the colour ring (advance_history_6a4d).  Per
 * $6A38 the CMP #$90 branch falls THROUGH into the rotation — the two are additive, so the
 * palette keeps cycling while the tunnel clears. */
void step_accum_add_75(void) {
    ROF_MEMBASE_DECL(mb);   /* 50 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t a = add_multibyte_a1_core(0x75);
    scroll_accum_b3 = a;
    if (a == scroll_accum_prev) return;                   /* top byte unchanged */
    scroll_accum_prev = a;
    if (a >= 0x90) draw_ring_frame_step();
    advance_history_6a4d();
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* step_accum_sub_7e @ $6A8F — the BOOST reverse ring-step (the $008D branch of
 * launch_anim_dispatch).  Subtract $7E from the scroll accumulator via sub_multibyte_a1;
 * if the top byte is unchanged do nothing.  Otherwise store it, and when it is < $14 use it
 * as an index into the ring-thickness table $6E0F -> $0096 and draw ONE reverse ring group
 * via draw_symmetric_span_loop (it also latches $008D = the accumulator top byte).  Finally,
 * while $008D is non-zero, INC $008E (the row-arm counter boot_standby_launch_driver spins on) and rotate
 * the colour ring via advance_history_6a4d (which copies $08D8 -> $0071 while $008D<0).
 */
void step_accum_sub_7e(void) {
    cpu.A = 0x7E;
    sub_multibyte_a1();                                    /* $08DA=$7E; 24-bit sub; A=new top */
    uint8_t a = cpu.A;
    scroll_accum_b3 = a;                                   /* $00A4 */
    if (a == scroll_accum_prev) return;                   /* CMP $00A5; BEQ -> return */
    scroll_accum_prev = a;                                /* $00A5 = A */
    if (a < 0x14) {                                        /* CMP #$14; BCC -> draw a ring group */
        step_mode_flag = a;                               /* $008D = A (TAY; STA) */
        span_row_count = mem[0x6E0F + a];                 /* $0096 = ring thickness */
        ROF_TR_SRC_SAVE(trSave);                          /* ISR site: restore, we may have preempted the pre-draw */
        ROF_TR_SRC_SET(3);
        draw_symmetric_span_loop();                       /* steps $9C--/$9D++/$9E++/$9F-- */
        ROF_TR_SRC_RESTORE(trSave);
    }
    if (step_mode_flag == 0) return;                      /* LDA $008D; BEQ -> return */
    mem[0x008E]++;                                        /* INC $008E */
    advance_history_6a4d();
}

/* Planet dirty-row extent: draw_vline_pair is the only writer of the $1000 stars/planet
 * field, so it records the min/max field row it touches here; the Amiga
 * renderViewportModeD (RescueOnFractalus.cpp) decodes only that band, then resets it.
 * Defined here (the writer's TU) so every build that links the native twins resolves it. */
volatile unsigned long g_planetRowLo = 9999, g_planetRowHi = 0;

/* draw_vline_pair_core @ $6C4D — plot a symmetric pair of vertical lines down the screen field.
 * Walks from startRow down to endRow (inclusive), and at each row fills two mirror columns:
 * col = (colSourceX >> 1) + 2 and its mirror $2F-col.  For rows >= $2B the two cells are packed
 * 2-bits-per-pixel (plot_pixel_2bpp: OR $C0 into the addressed cell); below $2B the raw `fill` byte
 * is stored directly.  startRow < endRow draws nothing.  Writes the screen field plus the faithful
 * exit scratch $0092/$0085/$0080/$0081/$0082 (see below); colSourceX is not mutated.
 *
 * Args replace the 6502 register/ZP ABI: startRow=A, endRow=$00B8, colSourceX=X, fill=$0084. */
static void draw_vline_pair_core(uint8_t startRow, uint8_t endRow,
                                 uint8_t colSourceX, uint8_t fill)
{
    draw_row = startRow;                                   /* $0092 = row counter (set even on the early-out) */
    if ((uint8_t)(startRow - endRow) & 0x80) return;       /* startRow below endRow -> nothing to draw */
    /* Loop-invariants (fixed across the whole vertical line): the two plot columns and the
     * plot_pixel_2bpp carries — hoist them out.  Inline set_row_ptr_from_count (a per-row addr-table
     * read) into a direct base read, and inline plot_pixel_2bpp's LUT.  This is the planet-approach
     * body-fill hot loop (the launch cineGap): the per-row body drops from set_row_ptr + 2 function
     * calls (each re-reading $80/$81 for ZP_IND_Y) to one table read + two indexed RMWs.  $80/$81 /
     * $0082 are only needed at exit, so write them once after the loop.  Byte-identical (make validate). */
    uint8_t col = (uint8_t)((colSourceX >> 1) + 2);        /* TXA; LSR; CLC; ADC #$02 */
    uint8_t y2  = (uint8_t)(0x2F - col);                   /* mirror column */
    const uint8_t* lutCol = plot2bpp_lut[1];               /* col: C=1 (CMP a>=$2B) */
    const uint8_t* lutMir = plot2bpp_lut[(uint8_t)(0x2F >= col) & 1];  /* mirror: C from SBC */
    if (!plot2bpp_lut_ready) build_plot2bpp_lut();
    encounter_count = col;                                 /* $0085: constant; final ZP value */
    uint8_t packed = 0;
    uint16_t base = 0;
    for (;;) {
        uint8_t a = draw_row;
        if (a & 0x80) { a = 0x00; draw_row = 0x00; }    /* CMP #0; BPL skips; clamp negative to 0 */
        if (a < g_planetRowLo) g_planetRowLo = a;          /* widen the dirty-row extent */
        if (a > g_planetRowHi) g_planetRowHi = a;
        base = (uint16_t)(mem[MEM_row_base_lo + a] | (mem[MEM_row_base_hi + a] << 8));  /* was set_row_ptr */
        if (a >= 0x2B) {                                   /* CMP #$2B; BCC -> else (direct store) path */
            uint16_t ac = (uint16_t)(base + col); mem[ac] = lutCol[mem[ac]];
            uint16_t am = (uint16_t)(base + y2);  mem[am] = lutMir[mem[am]];
            packed = 1;
        } else {
            mem[(uint16_t)(base + col)] = fill;                 /* screen field is RAM: direct mem[] */
            mem[(uint16_t)(base + y2)]  = fill;
        }
        uint8_t n = (uint8_t)(draw_row - 1);            /* DEC $0092 */
        draw_row = n;
        if ((uint8_t)(n - endRow) & 0x80) break;        /* CMP $00B8; BPL loops; break when N set */
    }
    /* faithful exit ZP: $80/$81 = addr table[last row processed]; $0082 = $C0 if any pack ran */
    sync_flag = (uint8_t)base; dl_ptr_lo = (uint8_t)(base >> 8);
    if (packed) dl_ptr_hi = 0xC0;
}

/* 6502-ABI shim: A = start row, X = column source, $00B8 = end row, $0084 = fill byte. */
void draw_vline_pair(void) {
    draw_vline_pair_core(cpu.A, mem[MEM_draw_row_ptr2_hi], cpu.X, mem[MEM_screen_ptr_hi]);
}

/* update_object_distance @ $6BED — recompute one approaching object's on-screen distance
 * and redraw its vertical "getting closer" marker.
 *
 * Given the object's current 16-bit distance and this frame's accumulated approach offset,
 * it computes the new distance (high byte floored at 0), writes it back to the object-position
 * table at [slot], and draws up to three stacked vertical line-pairs down the screen — each one
 * row shorter and in a different fill pattern ($FF, then $AA, then $55) — stopping early as the
 * row counter runs out.  Each line-pair is one draw_vline_pair call, which fills a column and its
 * mirror ($2F-col) from a start row down to an end row.
 *
 * SCRATCH-NAME WARNING: the 6502 reuses several named zero-page cells purely as scratch here, so
 * their mem.h symbol names are MEANINGLESS in this routine (see docs/rename.md):
 *   $0084 screen_ptr_hi   -> draw fill-pattern byte ($FF/$AA/$55)
 *   $0085 encounter_count -> subtrahend high byte (this frame's advance-hi input)
 *   $00B7 frame_counter   -> row counter / distance-lo scratch
 *   $00B8 draw_row_ptr2_hi-> distance-hi / draw end-row scratch
 *   $00B9 draw_pattern_byte / $00BA obj_pos_hi -> the object's 16-bit distance (minuend)
 * We keep the logic in locals and touch mem[] only where draw_vline_pair reads its inputs from
 * fixed cells ($0084 = pattern, $00B8 = end row; A = start row, X = column/slot) or where the
 * routine's final $00B7/$00B8/$0084 values must match the 6502 (the validation harness diffs full
 * memory).  No hardware ($D000-$D7FF) writes here, so there are no dead bus_writes to drop. */
static void update_object_distance_core(uint8_t slot,
                                        uint8_t distLo, uint8_t distHi,
                                        uint8_t subLo, uint8_t subHi)
{
    /* New distance = {distHi:distLo} - {subHi:subLo}.  The low byte wraps freely; the high byte
     * is floored at 0 (a negative result is clamped, but the low byte is NOT re-clamped). */
    uint8_t newLo  = (uint8_t)(distLo - subLo);
    uint8_t borrow = (distLo < subLo) ? 1 : 0;
    uint8_t newHi  = (uint8_t)(distHi - subHi - borrow);
    if (newHi & 0x80) newHi = 0;
    mem[MEM_obj_pos_table + slot]     = newLo;      /* $08A4[slot] = distance lo */
    mem[MEM_obj_pos_table + 1 + slot] = newHi;      /* $08A5[slot] = distance hi */

    /* The mem[$0084]/mem[$00B8] writes below are the 6502's faithful scratch side-effects (the
     * validation harness diffs full memory), left in place; the live draw inputs are passed to
     * draw_vline_pair_core as explicit arguments. */

    /* Draw #1: from the object's raw distance-hi (clamped to the last row $2E) down to the new
     * distance-hi, fill pattern $FF. */
    uint8_t endRow   = newHi;
    uint8_t startRow = (distHi < 0x2F) ? distHi : 0x2E;
    mem[MEM_screen_ptr_hi]    = 0xFF;
    mem[MEM_draw_row_ptr2_hi] = endRow;
    draw_vline_pair_core(startRow, endRow, slot, 0xFF);

    mem[MEM_screen_ptr_hi] = 0xAA;                  /* pattern for draw #2, set before the early-out */
    if (startRow == 0) { mem[MEM_frame_counter] = 0xFF; return; }   /* row-- underflowed -> done */
    uint8_t row = (uint8_t)(startRow - 1);

    /* Draw #2: same start row (one up), end row stepped up by this frame's advance-hi (floored). */
    endRow = (uint8_t)(endRow - mem[MEM_obj_advance_hi]);
    if (endRow & 0x80) endRow = 0;
    mem[MEM_draw_row_ptr2_hi] = endRow;
    draw_vline_pair_core(row, endRow, slot, 0xAA);

    mem[MEM_screen_ptr_hi] = 0x55;                  /* pattern for draw #3, set before the early-out */
    if (endRow == 0) { mem[MEM_frame_counter] = 0xFF; return; }     /* row = endRow, row-- underflowed */
    row    = (uint8_t)(endRow - 1);
    endRow = (uint8_t)(endRow - 1);                 /* draw #3 end row (endRow >= 1 here, so >= 0) */
    mem[MEM_frame_counter]    = row;
    mem[MEM_draw_row_ptr2_hi] = endRow;

    /* Draw #3 only while the row is still above the marker cut-off ($2B). */
    if (row < 0x2B) draw_vline_pair_core(row, endRow, slot, 0x55);
}

/* 6502-ABI shim: X = object slot; minuend (current distance) in {$00B9:$00BA}; subtrahend
 * (this frame's advance) in {$0084:$0085}.  These input cells' mem.h names are the misleading
 * scratch aliases noted in update_object_distance_core / docs/rename.md. */
void update_object_distance(void) {
    update_object_distance_core(cpu.X,
                                mem[MEM_draw_pattern_byte], mem[MEM_obj_pos_hi],
                                mem[MEM_screen_ptr_hi], mem[MEM_encounter_count]);
}

/* advance_object_positions @ $6BA8 — advance the scroll counters ($08D1++, the
 * 16-bit $08D2/$08D3 += $18) then, for each of the 22 object slots (X=$2A..$00 step 2),
 * build {$0085:$0084} = ($08A5[X] << 2) + $08D3 (and $00B9/$00BA = the slot's raw
 * lo/hi), and call update_object_distance to recompute + redraw that object. */
void advance_object_positions(void) {
    obj_anim_frame = (uint8_t)(obj_anim_frame + 1);              /* INC $08D1 */
    uint16_t s = (uint16_t)obj_advance_lo + 0x18;             /* CLC; ADC #$18 */
    obj_advance_lo = (uint8_t)s;
    obj_advance_hi = (uint8_t)(obj_advance_hi + (s >> 8));       /* ADC #$00 + carry */

    for (int x = 0x2A; x >= 0; x -= 2) {
        uint8_t lo = mem[MEM_obj_pos_table + x];
        draw_pattern_byte = lo;
        screen_ptr_hi = lo;
        uint8_t hi = mem[0x08A5 + x];
        obj_pos_hi = hi;
        encounter_count = hi;
        /* {$85:$84} = hi << 2 (via ASL $84; ROL into $85, twice) */
        uint8_t c0 = (uint8_t)((hi >> 7) & 1);             /* ASL A (=hi); $84 = hi<<1 */
        screen_ptr_hi = (uint8_t)(hi << 1);
        uint8_t a = c0;                                    /* LDA #0; ROL A -> c0 */
        uint8_t c1 = (uint8_t)((screen_ptr_hi >> 7) & 1);    /* ASL $0084 */
        screen_ptr_hi = (uint8_t)(screen_ptr_hi << 1);
        encounter_count = (uint8_t)((a << 1) | c1);            /* ROL A -> $85 */
        uint16_t t = (uint16_t)screen_ptr_hi + obj_advance_hi;  /* CLC; ADC $08D3 */
        screen_ptr_hi = (uint8_t)t;
        encounter_count = (uint8_t)(encounter_count + (t >> 8));   /* ADC #$00 + carry */
        cpu.X = (uint8_t)x;
        update_object_distance();
    }
}

/* clear_alien_knock_active @ $7F74 — $0632 = 0. */
void clear_alien_knock_active(void) { alien_knock_active = 0x00; }

/* alien_knock_setup_loop @ $7EC7 — set up the pilot-rescue SFX + zoom-animation state, then play a
 * 4-step descending-pitch sweep (each step stashes the pitch in $2930 and advances the SFX
 * frame via alien_creature_animate_draw).  If a colour-clear is active ($003E != 0 — systems off during a
 * rescue) it then runs the zoom animation loop, driving alien_creature_animate_draw once per RTCLOK frame
 * until $003E clears, and finally silences the audio channels.
 * (2 RANDOM reads here + 3 in the $3E!=0 block + 1 per alien_creature_animate_draw call.)
 * $2930/$2931/$005E/$005F/$061A/$0635 are unnamed scratch — see docs/rename.md.
 * VALIDATION: diffed against the oracle on the $003E==0 path (setup + sweep + tail) over a real
 * flight RAM snapshot (so alien_creature_animate_draw's table-driven call tree terminates).  The $003E!=0
 * zoom loop spins on RTCLOK (cleared externally by the VBI) — verified by inspection. */
void alien_knock_setup_loop(void) {
    alien_knock_active = 0x01;
    mem[0x005E] = 0x01;
    sound_table_idx = 0x09;                      /* $2924 */
    mem[0x2921] = 0x02;
    mem[0x005F] = 0x0B;
    mem[0x2922] = 0x00;
    mem[0x2926] = 0x00;
    sfx_state_0634 = (uint8_t)(sfx_state_0634 + 1);   /* INC $0634 */
    mem[0x0637] = 0x00;
    mem[0x063A] = 0x00;
    bus_write(0xD208, 0x00);                     /* AUDCTL = 0 */
    mem[0x0635] = 0x20;
    sfx_pitch_0638 = 0xBF;
    sfx_pitch_0639 = 0xD3;
    mem[0x2931] = (uint8_t)((bus_read(0xD20A) & 0x07) + 0x0C);   /* RANDOM&7 + $0C */

    /* Descending-pitch sweep: seed, then 4 steps (each stashes the pitch in $2930, advances
     * the SFX frame, then drops the pitch).  alien_creature_animate_draw is reached via sound_step_preserve_a
     * which SECs, so every SBC is a plain subtract. */
    uint8_t pitch = (uint8_t)((bus_read(0xD20A) & 0x07) + 0x21);
    mem[0x2930] = pitch; alien_creature_animate_draw(); pitch = (uint8_t)(pitch - 0x10);
    mem[0x2930] = pitch; alien_creature_animate_draw(); pitch = (uint8_t)(pitch - 0x0A);
    mem[0x2930] = pitch; alien_creature_animate_draw(); pitch = (uint8_t)(pitch - 0x04);
    mem[0x2930] = pitch; alien_creature_animate_draw(); pitch = (uint8_t)(pitch - 0x02);
    mem[0x2930] = pitch;

    if (clear_colors_done_003E == 0) {           /* no colour-clear active — done */
        clear_alien_knock_active();
        return;
    }

    /* $003E != 0: the zoom animation (systems off during a rescue). */
    {
        uint8_t rnd  = (uint8_t)(bus_read(0xD20A) & 0x03);
        uint16_t sum = (uint16_t)rnd + mem[0x061A];   /* CLC; ADC $061A */
        uint8_t a    = (uint8_t)sum;
        uint8_t carry = (uint8_t)(sum >> 8);          /* carry-out of the ADC */
        if (a & 0x80) a = 0x00;                       /* 7f37: clamp a negative sum to 0 (carry kept) */
        mem[0x292E] = (uint8_t)(a + 1 + carry);       /* 7f39: ADC #1 */
    }
    mem[0x005E] = 0x04;
    sound_table_idx = 0x0F;                      /* $2924 */
    mem[0x2921] = 0x05;
    mem[0x005F] = 0x12;
    do {
#ifdef ROF_PLATFORM_AMIGA
        /* Faithful wait-then-draw (6502 $7F4F), Amiga-adapted for display.  The Atari busy-waits 5
         * RTCLOK frames (the SFX interval) while ANTIC keeps displaying the mode-D field for free,
         * then calls $7F85.  The Amiga has no auto-display, so: busy-wait the interval (RTCLOK is
         * advanced by the hardware flight VBI; the COPPER keeps showing the last-composited creature
         * buffer continuously — the ANTIC stand-in, so NO re-render in the wait), then draw the new
         * creature and composite+display it ONCE.  (Re-compositing every frame of the wait — my first
         * cut — was ~5x the work per SFX step and the cause of the "horrible perf"; the creature only
         * changes once per $7F85, so one composite per step is both faithful and fast.) */
#ifdef ROF_FLIGHT_PROBE
        { unsigned long _tw = rof_subclock();
#endif
        while (RTCLOK_LOW <= 0x04) { platform_tick_vbi(); }   /* busy-wait; copper displays continuously */
#ifdef ROF_FLIGHT_PROBE
          g_alTWait += rof_subclock() - _tw; }
#endif
        RTCLOK_LOW = 0x00;
        ROF_CLEAR_FIG();                        /* fresh overlay for this step's creature */
#ifdef ROF_FLIGHT_PROBE
        g_alKnockFrames++;
        g_alPen[0]=mem[0x00DA]; g_alPen[1]=mem[0x00DB]; g_alPen[2]=mem[0x00DC];
        g_alPen[3]=mem[0x00DD]; g_alPen[4]=mem[0x0047]; g_alPen[5]=mem[0x0044];
        { unsigned long _td = rof_subclock();
#endif
        alien_creature_animate_draw();                        /* step SFX + blit the creature -> overlay */
#ifdef ROF_FLIGHT_PROBE
          g_alTDraw += rof_subclock() - _td; }
        { unsigned long _tr = rof_subclock();
#endif
        platform_render_frame();                /* composite + display it ONCE per SFX step */
#ifdef ROF_FLIGHT_PROBE
          g_alTRender += rof_subclock() - _tr; }
#endif
#else
        while (RTCLOK_LOW <= 0x04) { }          /* SDL/validate never enters this ($3E==0 fixture) */
        RTCLOK_LOW = 0x00;
        alien_creature_animate_draw();
#endif
    } while (clear_colors_done_003E != 0);
    cpu.A = 0x00;                                /* A = $3E (== 0 at loop exit) */
    silence_audio_channels();
}

/* clear_pm_state @ $3FBF — fill $00DA-$00DD, $02C0-$02C3 and $00D9 with the fill byte
   (entry cpu.A in the 6502 ABI).  _core takes the fill directly; the void shim preserves the
   6502-ABI entry for the transpiled callers + the validation harness. */
static void clear_pm_state_core(uint8_t fill) {
    for (int y = 0; y < 4; y++) { mem[MEM_audc_shadow_0 + y] = fill; mem[0x02C0 + y] = fill; }
    mem[0x00D9] = fill;
}
void clear_pm_state(void) { clear_pm_state_core(cpu.A); }

/* clear_terrain_lo_buffers @ $6B63 — zero the two low terrain buffers $0E32/$0F32 (96 bytes). */
void clear_terrain_lo_buffers(void) {
    for (int y = 0x5F; y >= 0; y--) { mem[0x0E32 + y] = 0x00; mem[0x0F32 + y] = 0x00; }
}

/* fill_four_bufs_ff @ $6899 — $FF into the four buffers $0C87/$0D87/$0E87/$0F87 at +8..+1. */
void fill_four_bufs_ff(void) {
    for (uint8_t y = 0x08; y != 0x00; y--) {
        mem[0x0C87 + y] = 0xFF; mem[0x0D87 + y] = 0xFF;
        mem[0x0E87 + y] = 0xFF; mem[0x0F87 + y] = 0xFF;
    }
}

/* fill_buf_08d4 @ $6890 — fill $08D4-$08D9 with entry cpu.A. */
void fill_buf_08d4(void) {
    uint8_t a = cpu.A;
    for (int y = 5; y >= 0; y--) mem[MEM_color_ring + y] = a;
}

/* copy_4byte_table_to_02c4 @ $5D3B — copy from $5D48+X downward into $02C4+Y.
 * The loop branch (BPL) tests the flag from DEX, so it is X-CONTROLLED, not a fixed
 * 4 iterations: it runs until X (decrementing from entry X) goes negative, with Y
 * decrementing and wrapping in parallel.  In-game entry X is 3 (the intended 4-byte
 * copy), but the faithful port reproduces the full X-bounded, Y-wrapping behaviour. */
void copy_4byte_table_to_02c4(void) {
    uint8_t x = cpu.X, y = 0x03;
    do {
        mem[0x02C4 + y] = mem[0x5D48 + x];
        y = (uint8_t)(y - 1);   /* DEY */
        x = (uint8_t)(x - 1);   /* DEX (sets the N tested by BPL) */
    } while (!(x & 0x80));      /* BPL: loop while X >= 0 */
}

/* reset_audctl_flags @ $70E7 — $00E7=1 (music gate on), AUDCTL($D208)=0, $073A=0,
 * $0090=0, $073C=$FF. */
void reset_audctl_flags(void) {
    mem[0x00E7] = 0x01;
    bus_write(0xD208, 0x00);
    sfx_note_timer = 0x00;
    sfx_reinit_gate = 0x00;
    sfx_seq_ptr = 0xFF;
}

/* game_init_first @ $5DDB — $0043 = 1. */
void game_init_first(void) { event_active_flag = 0x01; }

/* mark_grid_slot_active @ $7B74 — $0A00[$28E6] = 1. */
void mark_grid_slot_active(void) { mem[0x0A00 + grid_slot_index] = 0x01; }

/* push_grid_cell @ $70A9 — store $009C into $2500[$0098], then INC $0098. */
void push_grid_cell(void) {
    mem[0x2500 + dl_bottom_index] = draw_x_left;
    dl_bottom_index = (uint8_t)(dl_bottom_index + 1);
}

/* vobj_pos_to_pmstrip_index @ $41DA — Y = ($DC - $062F) >> 2 (also left in A).
 * Maps a vertical object position to a PMG strip index; result consumed via cpu.Y. */
void vobj_pos_to_pmstrip_index(void) {
    uint8_t a = (uint8_t)(0xDC - life_counter);   /* SEC; SBC $062F */
    a = (uint8_t)(a >> 2);                       /* LSR; LSR */
    cpu.A = a;
    cpu.Y = a;                                   /* TAY */
}

/* copy_terrain_seed_rows @ $45EE — copy three 8-byte seed rows from $4DD2/$4DDA/$4DE2
 * into the terrain buffers $0C88/$0D88/$0B88. */
void copy_terrain_seed_rows(void) {
    for (int y = 7; y >= 0; y--) {
        mem[0x0C88 + y] = mem[0x4DD2 + y];
        mem[0x0D88 + y] = mem[0x4DDA + y];
        mem[0x0B88 + y] = mem[0x4DE2 + y];
    }
}

/* copy_row_addr_subset @ $7483 — copy 48 entries of the row-addr table $073D(lo)/$0793(hi)
 * (Y=$2F..$00) into $2932(lo)/$2962(hi) (X=$00..$2F, reversed). */
void copy_row_addr_subset(void) {
    uint8_t x = 0x00;
    for (int y = 0x2F; y >= 0; y--) {
        mem[0x2932 + x] = mem[MEM_row_base_lo + y];
        mem[0x2962 + x] = mem[MEM_row_base_hi + y];
        x++;
    }
}

/* memset_or_copy @ $3C93 — fill the byte $00B7 across [dest .. dest+count], where dest
 * is the 16-bit pointer $00C1/$00C2 (post-incremented) and count is the 16-bit value
 * $00C4:$00C3 + 1 (each underflow of $00C3 borrows from $00C4; loop ends when both
 * reach $FF).  Writes through bus_write; $00B7 is re-read each pass (faithful). */
void memset_or_copy(void) {
    /* Fast path: the byte $00B7 is filled to [dest, dest+count) where dest=$C1/$C2 and
       count = ($C4:$C3)+1.  The per-byte bus_write below is pure overhead for plain RAM
       (its function call + two range checks + (zp),Y recompute per byte made loader_util's
       ~9.5 KB clear cost ~23 VBIs / ~0.46s of the boot path).  bus_write only differs from
       a direct mem[] store for the hardware range ($D000-$D7FF) and the page-2 OS shadows
       ($0200-$02FF, which notify the platform).  All real callers (loader_util,
       fill_region_2000, init_terrain_render_buffers, boot_standby_launch_driver) target plain RAM, so
       fill mem[] directly there and fall back to the faithful loop only when the region
       overlaps those ranges (or wraps $FFFF).  Leaves $C1-$C4/Y exactly as the loop would. */
    uint16_t start = (uint16_t)(ROF_PAIR16(row_table_stride, player_speed));
    uint32_t count = (uint32_t)(((uint32_t)row_table_base_hi << 8) | row_table_base_lo) + 1u;
    uint32_t end   = (uint32_t)start + count;                 /* unwrapped, for overlap test */
    int hw     = (start < 0xD800u) && (end > 0xD000u);
    int shadow = (start < 0x0300u) && (end > 0x0200u);
    if (!hw && !shadow && end <= 0x10000u) {
        uint8_t  val = frame_counter;
        uint16_t a   = start;
        for (uint32_t k = 0; k < count; k++) { mem[a] = val; a = (uint16_t)(a + 1); }
        row_table_stride = (uint8_t)(end & 0xFF);
        player_speed = (uint8_t)((end >> 8) & 0xFF);
        row_table_base_lo = 0xFF;
        row_table_base_hi = 0xFF;
        cpu.Y = 0x00;
        return;
    }
    for (;;) {
        cpu.Y = 0x00;
        bus_write(ZP_IND_Y(0x00C1), frame_counter);
        row_table_stride = (uint8_t)(row_table_stride + 1);             /* INC $C1 */
        if (row_table_stride == 0x00) player_speed = (uint8_t)(player_speed + 1);  /* INC $C2 on carry */
        row_table_base_lo = (uint8_t)(row_table_base_lo - 1);             /* DEC $C3 */
        if (row_table_base_lo != 0xFF) continue;                    /* loop while no underflow */
        row_table_base_hi = (uint8_t)(row_table_base_hi - 1);             /* DEC $C4 */
        if (row_table_base_hi != 0xFF) continue;
        return;
    }
}

/* copy_bytes_to_dst @ $3C61 — write entry cpu.A to the dest pointer $00BD/$00BE
 * (post-incremented), X times (entry cpu.X, 0 => 256), at offset cpu.Y; then bump the
 * source pointer $00BB/$00BC once. */
void copy_bytes_to_dst(void) {
    ROF_MEMBASE_DECL(mb);   /* 53 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t a = cpu.A;
    uint8_t x = cpu.X;
    do {
        bus_write(ZP_IND_Y(0x00BD), a);
        dl_y3 = (uint8_t)(dl_y3 + 1);             /* INC $BD */
        if (dl_y3 == 0x00) dl_y4 = (uint8_t)(dl_y4 + 1);  /* INC $BE on carry */
        x = (uint8_t)(x - 1);                                 /* DEX */
    } while (x != 0);                                         /* BNE */
    dl_y1 = (uint8_t)(dl_y1 + 1);                 /* INC $BB */
    if (dl_y1 == 0x00) dl_y2 = (uint8_t)(dl_y2 + 1);  /* INC $BC on carry */
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* --- boot_standby_launch_driver-subtree leaves (batch 2026-06-15): pure mem-effect leaves. --- */

/* draw_compass_heading @ $3FDE — copy 4 bytes from the table $4B0B (descending) into
 * $32E3[3..0].  The base index is ($281C + $3FF6[$2836]) & $FF; the 6502 $4B0B,Y
 * lookup uses an 8-bit Y, so the source addresses span $4B0B..$4C0A. */
void draw_compass_heading(void) {
    uint8_t y = (uint8_t)(terrain_index + mem[0x3FF6 + terrain_sub_index]);
    /* The dirty hook used to fire unconditionally, so renderFrame re-decoded the 4 mode-4
     * cells (4 x 8 scanlines x 2 planes) EVERY frame — ~0.4% of all wall clock — although the
     * heading glyphs only move when the terrain index does, i.e. while turning.  Flag on
     * CHANGE instead.  Safe to narrow because the cells have exactly two writers and both are
     * hooked: this one and game_sub_4606 (the housing init).  The mem[] writes themselves are
     * unchanged, so the twin stays byte-identical to the $3FDE oracle. */
    uint8_t changed = 0;
    for (int x = 3; x >= 0; x--) {
        uint8_t v = mem[0x4B0B + y];
        if (mem[0x32E3 + x] != v) { mem[0x32E3 + x] = v; changed = 1; }
        y = (uint8_t)(y - 1);
    }
    if (changed) platform_compass_changed();   /* hook: compass cells $32E3-$32E6 rewritten */
}

/* fill_buffer2_region_ff @ $45A1 — fill 8 runs of 32 bytes ($FF) starting at $2098
 * with stride $30 (so $2098.., $20C8.., ... covers the 256-byte field region). */
void fill_buffer2_region_ff(void) {
    row_table_stride = 0x98; player_speed = 0x20;
    for (int blk = 8; blk > 0; blk--) {
        for (int y = 0x1F; y >= 0; y--) {
            cpu.Y = (uint8_t)y;
            bus_write(ZP_IND_Y(0x00C1), 0xFF);
        }
        uint16_t p = (uint16_t)(row_table_stride | (player_speed << 8));
        p = (uint16_t)(p + 0x30);                     /* ADC #$30 / ADC #$00 (16-bit) */
        row_table_stride = (uint8_t)p; player_speed = (uint8_t)(p >> 8);
    }
}

/* game_sub_4606 @ $4606 — init the target-state cells: $32E3[0..3]=1, the trio
 * $3355/$3356/$3357 = $B4/$B5/$B6, $3388 = $B4, and $33DF/$33E0 = $1E/$1D. */
void game_sub_4606(void) {
    for (int y = 3; y >= 0; y--) mem[0x32E3 + y] = 0x01;
    platform_compass_changed();   /* hook: compass housing cells $32E3-$32E6 initialised */
    mem[0x3388] = 0xB4;
    special_state_color = 0xB4; mem[0x3356] = 0xB5; mem[0x3357] = 0xB6;
    mem[0x33DF] = 0x1E; mem[0x33E0] = 0x1D;
}

/* fill_message_buffer @ $480F — store entry cpu.A into the message buffer at
 * $32B6+X down to $32B6+1 (DEX;BNE — entry X=0 wraps to a full 256-byte fill). */
void fill_message_buffer(void) {
    uint8_t a = cpu.A, x = cpu.X;
    do {
        mem[0x32B6 + x] = a;
        x = (uint8_t)(x - 1);
    } while (x != 0);
    platform_title_changed();   /* hook: message buffer $32B7 cleared/filled -> flag title dirty */
}

/* intro_fill_display_params @ $4FE0 — build 8 display params $00CF..$00D6 from the
 * low nibble of $4DF1[Y] OR'd with the current colour phase $00C2; then poke the
 * last value to GTIA HITCLR $D019 (ignored on host/Amiga) and bump $00C2. */
void intro_fill_display_params(void) {
    uint8_t a = 0;
    for (int y = 7; y >= 0; y--) {
        a = (uint8_t)((mem[0x4DF1 + y] & 0x0F) | player_speed);
        mem[MEM_display_param_0 + y] = a;
    }
    bus_write(0xD019, a);
    player_speed = (uint8_t)(player_speed + 1);
}

/* match_code_sequence @ $5B45 — cheat/code matcher.  Entry cpu.A is the next typed
 * code; $063F is the match position.  On a match advance the position; when it
 * reaches 6 copy the 40-byte payload $5B17..$5B3E into $36AB..$36D2 and reset the
 * position.  Any mismatch resets the position.  Always sets $0049 = $FF. */
void match_code_sequence(void) {
    uint8_t x = code_seq_progress;
    if (cpu.A == mem[0x5B3F + x]) {
        x = (uint8_t)(x + 1);
        if (x == 6) {
            for (int y = 0x27; y >= 0; y--) mem[0x36AB + y] = mem[0x5B17 + y];
            x = 0;
        }
    } else {
        x = 0;
    }
    code_seq_progress = x;
    joystick_raw = 0xFF;
}

/* init_terrain_dl @ $68AD — fill the terrain display-list region: $2F75..$2FA3 = $88
 * (47 bytes), then every 3rd entry of $300A.. / $308B.. (LMS address pairs) gets the
 * pointer $2F74 (lo=$74, hi=$2F) for Y = $7E,$7B,...,3,0. */
void init_terrain_dl(void) {
    for (int y = 0x2F; y >= 1; y--) mem[0x2F74 + y] = 0x88;
    for (int y = 0x7E; y >= 0; y -= 3) {
        mem[0x300A + y] = 0x74; mem[0x308B + y] = 0x74;
        mem[0x300B + y] = 0x2F; mem[0x308C + y] = 0x2F;
    }
}

/* sfx_seq_step @ $7148 — advance the SFX theme sequencer to the next note.
 *
 * The attract/standby-theme "score" is a byte stream at $71DB.  Each byte is
 * either a NOTE (bit7 clear) or a VOICE-PARAMETER command (bit7 set); a $00 byte
 * marks the end and loops the stream back to index 0 (a valid stream has a
 * non-zero byte at index 0).  This routine walks $073C (the stream cursor)
 * forward, applying every voice-parameter command it passes — each reloads all
 * four POKEY AUDF presets plus AUDC4 from the per-voice tables ($71AB/$719E/
 * $7191/$71B8/$71C5, indexed by the command's low 5 bits) — until it reaches the
 * next note (or a voice command whose AUDC4 is 0, which doubles as a rest).  The
 * chosen byte then sets the note duration ($073A, from $71D2[note&$1F]) and the
 * gate/volume nibble ($073B = note>>4). */
void sfx_seq_step(void) {
    uint8_t x = sfx_seq_ptr;
    uint8_t note;
    for (;;) {
        x++;
        uint8_t cmd = mem[0x71DB + x];
        if (cmd == 0) {                        /* end marker: loop to stream start */
            x = 0;
            cmd = mem[0x71DB];                 /* valid stream => table[0] != 0 */
        }
        if ((int8_t)cmd >= 0) { note = cmd; break; }   /* note byte -> emit it */

        /* Voice-parameter command: load the four AUDF presets + AUDC4. */
        mem[0x0091] = cmd;                     /* scratch ($0091 misnamed, see rename.md) */
        uint8_t v = (uint8_t)(cmd & 0x1F);     /* voice index = low 5 bits */
        bus_write(0xD200, mem[0x71AB + v]);    /* AUDF1 */
        bus_write(0xD202, mem[0x719E + v]);    /* AUDF2 */
        bus_write(0xD204, mem[0x7191 + v]);    /* AUDF3 */
        bus_write(0xD206, mem[0x71B8 + v]);    /* AUDF4 */
        uint8_t audc4 = mem[0x71C5 + v];
        bus_write(0xD207, audc4);              /* AUDC4 */
        if (audc4 != 0) continue;              /* more commands follow -> keep scanning */
        note = 0; break;                       /* AUDC4 == 0 -> rest (6502: A = AUDC4 = 0) */
    }
    sfx_seq_ptr = x;
    sfx_note_timer = mem[0x71D2 + (note & 0x1F)];   /* note duration */
    sfx_voice_mute = (uint8_t)(note >> 4);          /* gate/volume nibble (0 = silent) */
}

/* sfx_voice_tick @ $70F9 — one tick of the SFX theme player (attract/standby
 * theme).  On the Atari this ran from the VBI tail every other frame (25 Hz);
 * the Amiga drives it from CIA-B Timer A at the same rate.  Counts the current
 * note's duration down ($073A) and fetches the next note (sfx_seq_step) on
 * underflow, then emits an amplitude that ramps down with the remaining
 * duration: AUDC = (timer>>1) + $A0, clamped to $A3.  While the note is gated
 * ($073B != 0) that amplitude drives POKEY voices 1-3 (+ a 4th voice slot at
 * $D200+gate, amplitude+2); a zero gate silences voices 1-3. */
void sfx_voice_tick(void) {
    uint8_t timer = (uint8_t)(sfx_note_timer - 1);
    sfx_note_timer = timer;
    if ((int8_t)timer < 0) {                   /* duration underflowed -> next note */
        sfx_seq_step();
        timer = sfx_note_timer;                /* reloaded by sfx_seq_step */
    }

    uint8_t half = (uint8_t)(timer >> 1);
    uint8_t audc = (half < 3) ? (uint8_t)(half + 0xA0) : 0xA3;

    if (sfx_voice_mute == 0) {                 /* gate closed -> silence voices 1-3 */
        bus_write(0xD201, 0);
        bus_write(0xD203, 0);
        bus_write(0xD205, 0);
    } else {                                   /* gate open -> play */
        bus_write(0xD201, audc);
        bus_write(0xD203, audc);
        bus_write(0xD205, audc);
        bus_write(0xD1FF + sfx_voice_mute, (uint8_t)(audc + 2));  /* 4th voice slot */
    }
}

/* music_init_state @ $7238 — copy 6 bytes $731E[Y..Y-5] (entry Y) into $0657[5..0],
 * clear $0651 + POKEY AUDCTL ($D208), and set $0653/$0655 = 1. */
void music_init_state(void) {
    uint8_t y = cpu.Y;
    for (int x = 5; x >= 0; x--) {
        mem[MEM_music_stream_ptr_lo + x] = mem[0x731E + y];
        y = (uint8_t)(y - 1);
    }
    music_note_timer = 0x00;
    bus_write(0xD208, 0x00);
    music_tempo_timer = 0x01;
    mem[0x0655] = 0x01;
}

/* music_player_tick @ $7253 — one 50 Hz tick of the note-stream tune player (the
 * engine behind the level-start and game-over/results jingles; a SEPARATE engine
 * from the CIA-B SFX sequencer that carries the attract theme).  Called from the
 * VBI tail while the music-active flag $0655 is set.
 *
 * Voice model: four POKEY voices, indexed v=0..3 by the byte offset x=v*2.  Each
 * voice has a software ADSR envelope kept as an (level,delta) pair —
 *   level = mem[$0648 + x]   (current amplitude, 0..127)
 *   delta = mem[$0649 + x]   (added to level every tick; +attack / -release)
 * and a pitch  audf = mem[$0650 + x]  (POKEY AUDF value).  Every tick the
 * envelope is integrated (level += delta, clamped to 0) and emitted as POKEY
 * AUDC = (level >> 3) EOR distortion_bits[x], giving a decaying note.
 *
 * Two countdown timers pace the stream:
 *   music_note_timer  ($0651) — while >0, just integrate envelopes; on the tick
 *       it reaches 0 all four voices switch to the release slope ($065C).
 *   music_tempo_timer ($0653) — once note_timer is 0, this counts down the note's
 *       remaining frames; when it hits 0 the next stream step is decoded.
 *
 * Command stream (base ptr $0657/$0658, decoded via ZP scratch $99/$9A): a run of
 * optional instrument commands ($C0..$FF, each reloads all four voices' AUDF from
 * the 4-byte preset table $7375) followed by one duration byte (<$C0 = the tempo
 * reload) and one note byte packing four 2-bit voice codes (MSB pair first):
 *   00 = note off (silence the voice)      10 = note on, attack level $065A
 *   01 = tie (leave the voice unchanged)   11 = note on, attack level $0659
 * A note-on also loads the attack slope ($065B) and writes the voice's AUDF.
 * A $00 stream byte marks end-of-song and stops playback via audio_timer_setup. */
void music_player_tick(void) {
    ROF_MEMBASE_DECL(mb);   /* 72 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    if (music_note_timer != 0) {
        /* Note still sounding: integrate only.  When the note timer expires this
         * tick, flip every voice to the release slope so the note decays. */
        if (--music_note_timer == 0) {
            uint8_t release_delta = mem[0x065C];
            mem[0x0649] = release_delta;   /* voice 0 delta */
            mem[0x064B] = release_delta;   /* voice 1 delta */
            mem[0x064D] = release_delta;   /* voice 2 delta */
            mem[0x064F] = release_delta;   /* voice 3 delta */
        }
    } else if (--music_tempo_timer == 0) {
        /* Tempo expired: decode the next step of the command stream.  The Atari
         * used ZP $99/$9A as the indirect read pointer; mirror those writes so the
         * exit state is byte-identical to the 6502 oracle. */
        mem[0x0099] = music_stream_ptr_lo;
        mem[0x009A] = music_stream_ptr_hi;
        uint16_t base = (uint16_t)(music_stream_ptr_lo | (music_stream_ptr_hi << 8));
        uint8_t y = 0;

        /* Consume any leading instrument commands ($C0..$FF). */
        uint8_t b;
        for (;;) {
            b = mem[base + y];
            if (b == 0) { audio_timer_setup(); return; }   /* end of song -> stop */
            if (b < 0xC0) break;                            /* duration byte follows */
            uint8_t idx = (uint8_t)((b ^ 0xFF) << 2);       /* (~b)*4 = preset index */
            mem[0x0656] = mem[0x7375 + idx];                /* voice 3 AUDF */
            mem[0x0654] = mem[0x7375 + (uint8_t)(idx + 1)]; /* voice 2 AUDF */
            mem[0x0652] = mem[0x7375 + (uint8_t)(idx + 2)]; /* voice 1 AUDF */
            mem[0x0650] = mem[0x7375 + (uint8_t)(idx + 3)]; /* voice 0 AUDF */
            y++;
        }

        /* Duration byte: reload the tempo timer and restart the 4-tick note timer. */
        y++;
        music_tempo_timer = b;
        music_note_timer = 0x04;

        /* Advance the stream pointer past the instrument+duration+note bytes
         * (6502: lo = $99 + Y + 1 with carry into $0658). */
        uint16_t adv = (uint16_t)(music_stream_ptr_lo + y + 1);
        music_stream_ptr_lo = (uint8_t)adv;
        if (adv > 0xFF) music_stream_ptr_hi++;

        /* Note byte: four 2-bit voice codes, MSB pair = voice 3 down to voice 0. */
        uint8_t note = mem[base + y];
        for (int x = 6; x >= 0; x -= 2) {
            uint8_t code = note & 0xC0;
            if (code == 0x00) {                    /* note off */
                mem[0x0648 + x] = 0;               /* level = 0 */
                mem[0x0649 + x] = 0;               /* delta = 0 */
            } else if (code != 0x40) {             /* $80/$C0 = note on ($40 = tie) */
                mem[0x0648 + x] = (code == 0xC0) ? mem[0x0659] : mem[0x065A];
                mem[0x0649 + x] = mem[0x065B];     /* attack slope */
                bus_write(0xD200 + x, mem[0x0650 + x]);  /* set voice AUDF (POKEY) */
            }
            note = (uint8_t)(note << 2);           /* next voice's code to MSB pair */
        }
    }

    /* Envelope integrate + emit for all four voices (runs every tick). */
    for (int x = 6; x >= 0; x -= 2) {
        uint8_t level = (uint8_t)(mem[0x0648 + x] + mem[0x0649 + x]);
        if (level & 0x80) level = 0;               /* clamp negative sum to silence */
        mem[0x0648 + x] = level;
        uint8_t audc = (uint8_t)((level >> 3) ^ mem[0x73C1 + x]);
        bus_write(0xD201 + x, audc);               /* AUDC = volume | distortion (POKEY) */
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* count_up_to_level @ $75B8 — bump $0604 (binary, via INC) and a parallel BCD counter
 * $00C3 (SED/ADC #1) until $0604 reaches the target $006D.  $00C3 counts in decimal. */
void count_up_to_level(void) {
    row_table_base_lo = 0x00;
    do {
        level_count_acc = (uint8_t)(level_count_acc + 1);                       /* INC $0604 — binary */
        cpu.A = row_table_base_lo; cpu.C = 0; cpu.D = 1; ADC(0x01); cpu.D = 0; /* SED;CLC;ADC #1;CLD */
        row_table_base_lo = cpu.A;
    } while (level_count_acc != level_stage);
}

/* alien_field1_fill @ $811F — advance HUD text field 1.  If its source cursor ($0081) has
 * reached the field limit ($2928) just bump the cursor.  Otherwise copy 5 source bytes
 * (from the field-1 pointer $0087/$0088, indexed by the cursor) verbatim into cells
 * $9B..$9F and advance the cursor by 5.  ($0087/$0088 are the "vbi_phase/vbi_flags" cells
 * reused here as a text-source pointer — see docs/rename.md.) */
void alien_field1_fill(void) {
    uint8_t cursor = dl_ptr_lo;                                  /* $0081 field-1 cursor */
    if (cursor >= alien_field1_limit) { dl_ptr_lo = (uint8_t)(cursor + 1); return; }  /* $2928 */
    uint16_t src = (uint16_t)(vbi_phase | (vbi_flags << 8));     /* $0087/$0088 field-1 ptr */
    for (uint8_t cell = 0x9B; cell <= 0x9F; cell++)              /* cells $9B..$9F */
        mem[cell] = bus_read((uint16_t)(src + cursor++));
    dl_ptr_lo = cursor;
}

/* alien_field3_fill @ $8168 — advance HUD text field 3 (font-glyph row).  If its cursor
 * ($0083) has reached the fixed limit $A8 just bump it.  Otherwise copy 7 font bytes from
 * the glyph table $35CD (indexed by the cursor) into cells $94..$9A and advance the cursor
 * by 7.  (Shares cells $94..$9A with field 2 — the two are mutually exclusive per row via
 * their cursors, whichever last runs the fill path wins.) */
void alien_field3_fill(void) {
    uint8_t cursor = screen_ptr_lo;                             /* $0083 field-3 cursor */
    if (cursor >= 0xA8) { screen_ptr_lo = (uint8_t)(cursor + 1); return; }
    for (uint8_t cell = 0x94; cell <= 0x9A; cell++)             /* cells $94..$9A */
        mem[cell] = mem[(uint16_t)(0x35CD + cursor++)];
    screen_ptr_lo = cursor;
}

/* clear_message_buffer @ $480B — clear the 14-byte message buffer: set X=$0E, A=$00
 * and tail-call the (native) fill_message_buffer, zeroing $32B7..$32C4. */
void clear_message_buffer(void) {
    cpu.X = 0x0E;
    cpu.A = 0x00;
    fill_message_buffer();
}

/* plot_pixel_col93 @ $66D3 — plot the pixel whose column index is $0093: load A=$0093
 * and tail-call the (native) plot_pixel_masked. */
void plot_pixel_col93(void) {
    cpu.A = blit_pixel_x;
    plot_pixel_masked();
}

/* random_digit @ $5A59 — POKEY RANDOM rejection-sampled to a decimal digit 0-9
 * (re-roll the low nibble while it is >= $0A).  Result in cpu.A. */
void random_digit(void) {
    uint8_t a;
    do { a = (uint8_t)(bus_read(0xD20A) & 0x0F); } while (a >= 0x0A);
    cpu.A = a; CMP(0x0A);                 /* exit flags from the accepting CMP */
}

/* random_alpha_index @ $5A4D — POKEY RANDOM rejection-sampled to a letter index:
 * re-roll (RANDOM & $1F) while >= $1A (26 letters), then + $21 (carry clear from the
 * accepting CMP) giving a code in $21..$3A.  Result in cpu.A. */
void random_alpha_index(void) {
    uint8_t a;
    do { a = (uint8_t)(bus_read(0xD20A) & 0x1F); } while (a >= 0x1A);
    cpu.A = a; cpu.C = 0; ADC(0x21);
}

/* test_marked_neighbor @ $7047 — probe the $0900 marker map for a marked (negative)
 * cell at three offsets from the base index $009C+$009A: the base, base+$009A, and
 * base+$009B.  Returns the third cell's value in cpu.A iff all three are marked,
 * else 0.  Indices wrap 8-bit ($0900..$09FF). */
void test_marked_neighbor(void) {
    uint8_t fx = (uint8_t)(draw_x_left + grid_offset_a);
    if (!(mem[0x0900 + fx] & 0x80)) { LDA(0x00); return; }
    uint8_t idx2 = (uint8_t)(fx + grid_offset_a);
    if (!(mem[0x0900 + idx2] & 0x80)) { LDA(0x00); return; }
    uint8_t idx3 = (uint8_t)(fx + grid_offset_b);
    uint8_t a3 = mem[0x0900 + idx3];
    if (a3 & 0x80) { LDA(a3); return; }
    LDA(0x00);
}

/* fill_region_2000 @ $3C83 — fill the byte $00B7 across $2000..$2F73 (count $0F73+1
 * = $0F74 bytes): seed the dest ptr $C1/$C2 = $2000 and the 16-bit count $C4:$C3 =
 * $0F73, then tail-call the (native) memset_or_copy. */
void fill_region_2000(void) {
#ifdef ROF_PLATFORM_AMIGA
    /* Amiga: fill_region_2000 is the SOLE writer of the boost stars viewport field ($2000) —
     * measured on-Amiga, it re-fills the whole field exactly twice during the boost stars
     * sub-phase (a value-8 background, then a black one), and the star fade in between is
     * palette-only.  Publish a dirty flag so the boost render decodes $2000 into the tunnel
     * bitmap ONLY on those (rare) content changes instead of a full 86-row / ~56ms decode
     * EVERY frame.  Harmless for the Standby door-field fills that also call this (nobody
     * consumes the flag outside the boost stars branch, which forces it true on entry anyway). */
    g_boostStarsDirty = 1;
    /* ...and the Standby door field lives at $2000 too: this wipes ALL of it, so widen the
     * door-decode dirty range to the whole field (the flag itself stays down until the
     * blit_numeric_readout that ends the rebuild raises it). */
    ROF_DOOR_FIELD_TOUCH_ALL();
#endif
    row_table_stride = 0x00; player_speed = 0x20;   /* dest $2000 */
    row_table_base_lo = 0x73; row_table_base_hi = 0x0F;   /* count $0F73 */
    memset_or_copy();
}

/* silence_audio_channels @ $7F60 — write entry A to the SFX gate $0634 and POKEY
 * AUDF1/2/3/4 ($D201/$D203/$D205/$D207), set AUDCTL ($D208) = $60, then tail-call
 * the (native) clear_alien_knock_active.  POKEY writes go through bus_write (HW, no mem[]). */
void silence_audio_channels(void) {
    sfx_state_0634 = cpu.A;
    bus_write(0xD201, cpu.A); bus_write(0xD203, cpu.A);
    bus_write(0xD205, cpu.A); bus_write(0xD207, cpu.A);
    cpu.A = 0x60;
    bus_write(0xD208, cpu.A);
    clear_alien_knock_active();
}

/* init_terrain_render_buffers @ $753B — set the whole terrain boundary table
 * $260E..$270D = $FF (256 bytes), then fill $00B7 across $1070..$220F (count $11A0)
 * by seeding the dest ptr $C1/$C2 = $1070 + count $C4:$C3 = $119F and tail-calling
 * the (native) memset_or_copy. */
void init_terrain_render_buffers(void) {
    for (int i = 0; i < 256; i++) mem[MEM_terrain_height_max + i] = 0xFF;
    row_table_stride = 0x70; player_speed = 0x10;   /* dest $1070 */
    row_table_base_lo = 0x9F; row_table_base_hi = 0x11;   /* count $119F */
    memset_or_copy();
}

/* init_terrain_col_tables @ $7813 — build the per-column mode-D plot lookup tables used by
 * the terrain rasterizer.  terrain_col_pixel_mask[X] is screen column X's intra-byte pixel
 * mask: the high bit of X's 2-bits-per-pixel cell, which cycles $80,$20,$08,$02 across each
 * group of 4 columns (mode-D packs 4 px/byte).  terrain_col_byte_offset[X] is X's byte offset
 * within a bitmap row; cleared here and rebuilt every frame by terrain_draw_frame.  Together
 * they are the Atari lookup-table form of the Amiga renderFlightDirect kMask[c&3] / (c>>2).
 * (The original seeds $80 and rotates >>2-with-ROR to synthesise the same period-4 pattern.) */
void init_terrain_col_tables(void) {
    static const uint8_t kColMask[4] = { 0x80, 0x20, 0x08, 0x02 };
    for (int x = 0; x < 256; x++) mem[MEM_terrain_col_pixel_mask + x] = kColMask[x & 3];
    for (int x = 0; x < 256; x++) mem[MEM_terrain_col_byte_offset + x] = 0x00;
}

/* game_sub_7B54 @ $7B54 — maybe seed the wind/drift cell $2849 with a random value.
 * Bails if $003A is negative or $2849 already nonzero.  Otherwise a = (RANDOM|$08)&$3F,
 * shifted right by 1 ($0004 nonzero), 3 ($0004==0 && $062B nonzero), or 0 (both zero). */
void game_sub_7B54(void) {
    if (mem[0x003A] & 0x80) return;
    if (level_cleared_flag != 0x00) return;
    uint8_t a = (uint8_t)((bus_read(0xD20A) | 0x08) & 0x3F);
    if (level_or_state != 0x00) {
        a = (uint8_t)(a >> 1);
    } else if (bcd_osc_dir != 0x00) {
        a = (uint8_t)(a >> 3);
    }
    level_cleared_flag = a;
}

/* rng_signed_jitter @ $687D — add or subtract the entry magnitude (cpu.A, saved to
 * $00B7) from the base $0085, choosing the sign from the high bit of a POKEY RANDOM
 * read: negative => $0085 - mag, else $0085 + mag.  Result in cpu.A. */
void rng_signed_jitter(void) {
    frame_counter = cpu.A;
    uint8_t base = encounter_count;
    uint8_t r = bus_read(0xD20A);
    cpu.Y = r;                                 /* LDY sets N from the RANDOM byte */
    cpu.A = base;
    if (r & 0x80) { cpu.C = 1; SBC(frame_counter); }
    else          { cpu.C = 0; ADC(frame_counter); }
}

/* init_cockpit_bar_cells @ $45C5 — seed the cockpit bar graphic cells: $BE into the
 * four corner pairs ($2107/8, $2137/8, $21C7/8, $21F7/8) and $AA into the two middle
 * pairs ($2167/8, $2197/8).  Pure (no input). */
void init_cockpit_bar_cells(void) {
    mem[0x2107] = 0xBE; mem[0x2108] = 0xBE; mem[0x2137] = 0xBE; mem[0x2138] = 0xBE;
    mem[0x21C7] = 0xBE; mem[0x21C8] = 0xBE; mem[0x21F7] = 0xBE; mem[0x21F8] = 0xBE;
    mem[0x2167] = 0xAA; mem[0x2168] = 0xAA; mem[0x2197] = 0xAA; mem[0x2198] = 0xAA;
}

/* add_and_show_bcd_counter @ $497D — add the 16-bit delta $0045/$0046 into the 4-byte
 * score $0600-$0603 (big-endian, $0603 = low) and re-render it.  The 6502 uses SED but
 * our ADC ignores the decimal flag (matching the transliterated oracle), so the add is
 * binary; tail-calls the (native) render_bcd_counter (which then clears $0045/$0046). */
void add_and_show_bcd_counter(void) {
    cpu.D = 1;   /* SED ($497d): the 4-byte score counter $0600-$0603 is packed BCD, so the
                  * ADC chain must add in decimal (09+01 -> 10) with a decimal inter-digit carry. */
    cpu.A = mem[0x0603]; cpu.C = 0; ADC(bcd_delta_lo); mem[0x0603] = cpu.A;
    cpu.A = mem[0x0602];            ADC(bcd_delta_hi); mem[0x0602] = cpu.A;
    cpu.A = score_display;            ADC(0x00);        score_display = cpu.A;
    cpu.A = mem[0x0600];            ADC(0x00);        mem[0x0600] = cpu.A;
    cpu.D = 0;   /* CLD */
    render_bcd_counter();
}

/* plot_char_bounded @ $49D9 — plot one score/HUD digit at column Y through the dest
 * pointer $00C5/$00C6, with leading-zero suppression.  X is the running "seen a nonzero
 * digit" flag: plot unconditionally if X!=0; else if the digit A!=0 plot and set X;
 * else (a leading zero) plot only when the column Y has reached the threshold $0619
 * (forcing at least one "0").  The char code is digit + $50.  Y is always advanced. */
void plot_char_bounded(void) {
    uint8_t a = cpu.A, x = cpu.X, y = cpu.Y;
    int plot = 1;
    if (x == 0) {
        if (a != 0) {
            x = (uint8_t)(x + 1);                 /* INX — first nonzero digit */
        } else if (y < zero_suppress_pos) {
            plot = 0;                             /* leading zero below threshold: blank */
        } else {
            x = (uint8_t)(x + 1);                 /* INX — forced zero at/after threshold */
        }
    }
    if (plot) {
        a = (uint8_t)(a + 0x50);                  /* CLC; ADC #$50 (char code) */
        cpu.Y = y;
        uint16_t dst = (uint16_t)((digit_dst_ptr_lo | (digit_dst_ptr_hi << 8)) + y);
        bus_write(dst, a);
        /* Title Screen (scene 3b): if this digit landed in the $365B text window it is a
           STARTING LEVEL / score digit — flag the cell so the Amiga redraws just it (no
           full-screen repaint).  Off-screen / cockpit dests ($32C5, $37F5, ...) are clamped
           away inside the hook.  No-op on SDL / validate headless. */
        platform_title_screen_dirty(dst, 1u);
    }
    cpu.A = a;
    cpu.X = x;
    cpu.Y = (uint8_t)(y + 1);                     /* INY */
}

/* emit_bcd_byte_digits @ $49CE — plot both digits of a packed-BCD byte (entry A) via
 * plot_char_bounded: high nibble (A>>4) then low nibble (A&$0F), sharing the running
 * suppress flag X and column Y across the two calls. */
void emit_bcd_byte_digits(void) {
    uint8_t a = cpu.A;
    cpu.A = (uint8_t)(a >> 4);
    plot_char_bounded();
    cpu.A = (uint8_t)(a & 0x0F);
    plot_char_bounded();
}

/* mark_slot_and_countdown_char @ $7B39 — mark the active grid slot then show the
 * countdown glyph (both native). */
void mark_slot_and_countdown_char(void) {
    mark_grid_slot_active();
    countdown_show_char_0620();
}

/* mark_slot_and_inc_count @ $7B7D — mark the active grid slot then set place params +
 * bump the count (both native). */
void mark_slot_and_inc_count(void) {
    mark_grid_slot_active();
    set_place_params_inc_count();
}

/* Bare-RTS / no-op stubs (each a standalone RTS in the 6502; native = empty).  Porting
 * them removes them from the transpiled set and lets their tail-callers go native. */
void return_stub_40af(void) { }
void terrain_obj_skip_return(void) { }
void ret_stub_6a26(void) { }
void draw_bar_loop_end(void) { }
void terrain_plot_return(void) { }
void terrain_distance_clamp_return(void) { }
void plot_line_done(void) { }

/* scan_grid_neighbors @ $7069 — probe the four diagonal neighbours of the current grid
 * cell (offset pairs in $009A/$009B) via test_marked_neighbor; for each one that comes
 * back marked (negative), push the cell with push_grid_cell.  Both callees are native. */
void scan_grid_neighbors(void) {
    static const uint8_t off[4][2] = { {0x01,0x10}, {0xFF,0xF0}, {0x10,0xFF}, {0xF0,0x01} };
    for (int i = 0; i < 4; i++) {
        grid_offset_a = off[i][0];
        grid_offset_b = off[i][1];
        test_marked_neighbor();
        if (cpu.N) push_grid_cell();
    }
}

/* intro_reset_score_slots @ $4FCE — clear two score slots ($066A/$0686 = 0), set $0678
 * = $0C, and enqueue event Y=$0D via the (native) ring_push_unmarked. */
void intro_reset_score_slots(void) {
    sfx_voice_distort_0d = 0x00; mem[0x0686] = 0x00; hud_field_678 = 0x0C;
    cpu.Y = 0x0D;
    ring_push_unmarked();
}

/* init_event_state_5815_x16 @ $7AA8 — seed event state ($0044 = entry A, $3388 = $B4,
 * $003C = 0) and enqueue marked event X=$16 via the (native) ring_push_marked. */
void init_event_state_5815_x16(void) {
    timer_or_counter = cpu.A; mem[0x3388] = 0xB4; airlock_state = 0x00;
    cpu.X = 0x16;
    ring_push_marked();
}

/* rle_run_fill @ $3C58 — RLE run expansion: entry A = run length (-> X), bump the source
 * pointer $00BB/$00BC, read the run byte at ($00BB)+Y, and replicate it A times into the
 * dest pointer $00BD/$00BE via the (native) copy_bytes_to_dst. */
void rle_run_fill(void) {
    cpu.X = cpu.A;                                  /* TAX — run length */
    dl_y1 = (uint8_t)(dl_y1 + 1);
    if (dl_y1 == 0x00) dl_y2 = (uint8_t)(dl_y2 + 1);
    cpu.A = bus_read(ZP_IND_Y(0x00BB));             /* the run byte (uses entry cpu.Y) */
    copy_bytes_to_dst();
}

/* blit_glyph_8rows @ $678B — blit an 8-row glyph into the bitmap.  The initial row
 * pointer $0080/$0081 comes from the row-addr table (set_row_ptr_from_count via $0092),
 * then walks UP one screen row ($2E bytes) per glyph row.  For each of rows 7..0 the
 * glyph byte ($0084[Y]) is shifted left bit-by-bit; each set bit plots a pixel at the
 * running column $0093 (base $009C) via plot_pixel_col93 (mask index from $0094=$0095).
 * Finally the column base $009C advances by 8.  The 6502 PHA/PLA preserves the shifted
 * byte across the plot call — reproduced here with a local. */
void blit_glyph_8rows(void) {
    ROF_MEMBASE_DECL(mb);   /* 70 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    draw_color_idx = blit_color_src;
    set_row_ptr_from_count();                       /* $80/$81 = table[$0092] */
    blit_row_counter = 0x07;
    do {
        blit_pixel_x = draw_x_left;                  /* column base for this row */
        cpu.Y = blit_row_counter;
        uint8_t a = bus_read(ZP_IND_Y(0x84));       /* glyph byte for row $0097 */
        do {
            uint8_t carry = (uint8_t)(a >> 7);      /* ASL A: bit shifted out */
            a = (uint8_t)(a << 1);
            if (carry) plot_pixel_col93();          /* plot at column $0093 */
            blit_pixel_x = (uint8_t)(blit_pixel_x + 1);
        } while (a != 0x00);                        /* loop while bits remain */
        uint16_t p = (uint16_t)(sync_flag | (dl_ptr_lo << 8));
        p = (uint16_t)(p - 0x2E);                   /* row pointer up one screen row */
        sync_flag = (uint8_t)p; dl_ptr_lo = (uint8_t)(p >> 8);
        blit_row_counter = (uint8_t)(blit_row_counter - 1);
    } while (!(blit_row_counter & 0x80));                /* BPL: loop while $0097 >= 0 */
    draw_x_left = (uint8_t)(draw_x_left + 0x08);
    /* Amiga: record the 8 field rows this glyph just wrote, so the door-field decode can redo
     * only those instead of all 85 (see ROF_DOOR_FIELD_TOUCH_ROWS).  No-op unless the row table
     * is the $2000 one. */
    ROF_DOOR_FIELD_TOUCH_ROWS(draw_row);
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* intro_seed_object_map @ $7498 — seed the intro object map.  Clear $0A00[0..255], place
 * a $C8 entry (slot Y = $10 if $006D==1 else $08) via obj_table_scan_a_c8, optionally place
 * a value-1 entry (slot Y=4) via obj_table_scan_replace when $006D >= 4, then sprinkle $64
 * markers across $0A00 at stride-$43 offsets where RANDOM < $0623 and $0900[X] is marked.
 * All scan callees are native and consume RANDOM identically on both sides. */
void intro_seed_object_map(void) {
    for (int i = 0; i < 256; i++) mem[0x0A00 + i] = 0x00;
    cpu.Y = (level_stage == 0x01) ? 0x10 : 0x08;
    obj_table_scan_a_c8();
    if (level_stage >= 0x04) {
        cpu.Y = 0x04; cpu.A = 0x01;
        obj_table_scan_replace();
    }
    uint8_t x = 0x00;
    do {
        if (bus_read(0xD20A) < mem[0x0623]) {        /* CMP $0623; BCS skip */
            if (mem[0x0900 + x] & 0x80)              /* BPL skip; act when marked */
                mem[0x0A00 + x] = 0x64;
        }
        x = (uint8_t)(x + 0x43);
    } while (x != 0x00);
}

/* intro_unmark_random_cells @ $70B3 — sometimes (always if $006D==1, else 1/8 on RANDOM&7==0)
 * sweep the $0900 marker map and clear the marked bit ($7F mask) of cells whose index has
 * bits 7 and 3 clear, gated on RANDOM < a per-pass threshold $00C3 = (RANDOM&$70)|$80.
 * RANDOM read counts are matched to the 6502 exactly (none on the $006D==1 early path). */
void intro_unmark_random_cells(void) {
    uint8_t y = (uint8_t)(level_stage - 1);          /* LDY $006D; DEY */
    if (y != 0x00) {                                 /* BEQ skips the RANDOM&7 gate */
        if ((bus_read(0xD20A) & 0x07) != 0x00) return;  /* BNE -> return */
    }
    row_table_base_lo = (uint8_t)((bus_read(0xD20A) & 0x70) | 0x80);
    uint8_t i = 0x00;
    do {
        if (!(i & 0x80) && !(i & 0x08)) {
            if (bus_read(0xD20A) < row_table_base_lo)
                mem[0x0900 + i] = (uint8_t)(mem[0x0900 + i] & 0x7F);
        }
        i = (uint8_t)(i + 1);
    } while (i != 0x00);
}

/* sfx_engine_reset @ $5433 — clear the music/voice state tables ($066B..$06F7 in 11
 * parallel 14-entry columns + $0705 block + $0714) and the ring head $0073/$0074, then
 * seed the voice-priority slots ($0705[2..8] / POKEY AUDF via $D1FF+X) and the music
 * timer fields $0712/$0713 = 2/6, AUDCTL ($D208) = $60.  Pure init (POKEY via bus_write). */
void sfx_engine_reset(void) {
    alt_ring_head = 0x00; ring_tail_0719 = 0x00;
    static const uint16_t cols[11] = { 0x066B, 0x0705, 0x0687, 0x0695, 0x06A3, 0x06B1,
                                        0x06BF, 0x06CD, 0x06DB, 0x06E9, 0x06F7 };
    for (int y = 0x0E; y >= 1; y--)                 /* DEY;BNE: Y=$0E..1 */
        for (int c = 0; c < 11; c++) mem[cols[c] + y] = 0x00;
    sfx_top_prio_val = 0x00;
    sfx_top_voice_idx = 0x02;
    for (int y = 4; y >= 1; y--) {                  /* DEY;BNE: Y=4..1 */
        uint8_t a = (uint8_t)(y << 1);              /* TYA; ASL */
        mem[MEM_sfx_voice_reg_idx + y] = a;
        bus_write(0xD1FF + a, 0x00);                /* TAX; STA $D1FF,X (POKEY) */
    }
    mem[0x0706] = 0x00; mem[0x0708] = 0x00;
    mem[0x0712] = 0x02; mem[0x0713] = 0x06;
    bus_write(0xD208, 0x60);                        /* AUDCTL */
}

/* show_cockpit_message @ $47B8 — render a HUD message (entry Y = message id, bit7 set
 * suppresses the buffer clear, bit6 selects the alt colour $5A in $063E) into the message
 * buffer $32B7+ from the glyph tables $481E / $491A (indexed by $4927[id]+offset).  Y==9
 * also sets the colour $00D8=$38.  A glyph with bit7 set is the end marker: stored masked,
 * and if nonzero terminates the string. */
void show_cockpit_message(void) {
    ROF_MEMBASE_DECL(mb);   /* 103 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t y = cpu.Y;
    if (y & 0x80) y = (uint8_t)(y & 0x7F);          /* bit7: skip clear */
    else clear_message_buffer();
    timer_or_counter = 0x00;
    msg_flash_timer = 0xFF;
    if (y >= 0x40) {                                /* bit6: alt colour */
        y = (uint8_t)(y & 0x3F);
        msg_flash_timer = 0x5A;
    }
    if (y == 0x09) text_color_pf0 = 0x38;
    uint8_t x = mem[MEM_cockpit_msg_index + y];
    dl_y1 = y;
    uint8_t pos = 0x00;
    for (;;) {
        uint8_t a = (dl_y1 >= 0x15) ? mem[0x491A + x] : mem[MEM_cockpit_msg_strings + x];
        if (a & 0x80) {                             /* end marker */
            a = (uint8_t)(a & 0x7F);
            mem[MEM_cockpit_msg_buffer + pos] = a;
            if (a != 0) break;
        }
        mem[MEM_cockpit_msg_buffer + pos] = a;
        pos = (uint8_t)(pos + 1);
        x = (uint8_t)(x + 1);
        if (pos == 0x0E) break;
    }
    platform_title_changed();   /* hook: HUD message rendered into $32B7 -> flag title dirty */
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* game_sub_6811 @ $6811 — scatter random dots (6 outer passes, growing mask $0082 and
 * threshold $0084).  Each dot: re-roll two coords ($0093/$0092 = RANDOM & $0082) until at
 * least one reaches the threshold $0084, then plot it at a jittered row/col — row via
 * rng_signed_jitter($2A)+set_row_ptr, col via rng_signed_jitter($2F)+plot_pixel_masked.
 * The mask index $0094 cycles 4..6 then 1..6.  All callees are native. */
void game_sub_6811(void) {
    draw_color_idx = 0x04;
    span_pixel_count = 0x06;
    dl_ptr_hi = 0x07;
    screen_ptr_hi = 0x00;
    do {
        span_row_count = (uint8_t)((dl_ptr_hi | 0x07) >> 2);   /* ORA #7; LSR; LSR */
        do {
            for (;;) {                                        /* re-roll until accepted */
                blit_pixel_x = (uint8_t)(bus_read(0xD20A) & dl_ptr_hi);
                uint8_t v92 = (uint8_t)(bus_read(0xD20A) & dl_ptr_hi);
                draw_row = v92;
                if (v92 >= screen_ptr_hi) break;                /* BCS accept */
                if (blit_pixel_x >= screen_ptr_hi) break;        /* BCC retry inverted */
            }
            encounter_count = 0x2A;
            cpu.A = draw_row;
            rng_signed_jitter();                              /* jittered row in A */
            cpu.Y = cpu.A;
            set_row_ptr();
            encounter_count = 0x2F;
            cpu.A = blit_pixel_x;
            rng_signed_jitter();                              /* jittered col in A */
            plot_pixel_masked();
            span_row_count = (uint8_t)(span_row_count - 1);
        } while (span_row_count != 0x00);
        dl_ptr_hi = (uint8_t)(dl_ptr_hi + 0x07);
        screen_ptr_hi = (uint8_t)(screen_ptr_hi + 0x06);
        draw_color_idx = (uint8_t)(draw_color_idx + 1);
        if (draw_color_idx == 0x07) draw_color_idx = 0x01;
        span_pixel_count = (uint8_t)(span_pixel_count - 1);
    } while (span_pixel_count != 0x00);
}

/* plot_terrain_span @ $692A — draw a run of vertical spans.  The span count $0096 comes
 * from $6E0F[Y] (entry Y; +2 when Y==0); each pass calls the (native) fill_vertical_span
 * and steps the column pair $009C--/$009D++.  Afterwards the row window is shifted by the
 * span value $0085 (=$6E0F[Y]): $009E += $0085, $009F -= $0085. */
void plot_terrain_span(void) {
    uint8_t a = mem[0x6E0F + cpu.Y];
    span_row_count = a;
    if (cpu.Y == 0x00) {
        span_row_count = (uint8_t)(span_row_count + 1);
        span_row_count = (uint8_t)(span_row_count + 1);
    }
    encounter_count = a;
    do {
        fill_vertical_span();
        draw_x_left = (uint8_t)(draw_x_left - 1);
        draw_x_right = (uint8_t)(draw_x_right + 1);
        span_row_count = (uint8_t)(span_row_count - 1);
    } while (span_row_count != 0x00);
    draw_row_top = (uint8_t)(draw_row_top + encounter_count);
    draw_row_bottom = (uint8_t)(draw_row_bottom - encounter_count);
}

/* trigger_effect_4a @ $7AA6 — fire event effect $4A via init_event_state_5815_x16. */
void trigger_effect_4a(void) {
    cpu.A = 0x4A;
    init_event_state_5815_x16();
}

/* terrain_plot_skip_return @ $A6F8 — bare RTS (no-op). */
void terrain_plot_skip_return(void) { }

/* render_bcd_low_bytes @ $49AE — render the two low score bytes $0602/$0603 as digit
 * pairs via the (native) emit_bcd_byte_digits. */
void render_bcd_low_bytes(void) {
    cpu.A = mem[0x0602]; emit_bcd_byte_digits();
    cpu.A = mem[0x0603]; emit_bcd_byte_digits();
}

/* set_zsupp_pos_clear_delta @ $49C5 — set the zero-suppress threshold $0619 = entry Y,
 * clear the 16-bit delta $0045/$0046, then render a digit pair via emit_bcd_byte_digits
 * (entry A = the BCD byte). */
void set_zsupp_pos_clear_delta(void) {
    zero_suppress_pos = cpu.Y;
    cpu.Y = 0x00;
    bcd_delta_lo = cpu.Y;
    bcd_delta_hi = cpu.Y;
    emit_bcd_byte_digits();
}

/* save_color_clear_y_bit5 @ $47B2 — stash colour $00D8 = entry A, clear bit5 of the
 * message id (entry Y), and render it via the (native) show_cockpit_message. */
void save_color_clear_y_bit5(void) {
    text_color_pf0 = cpu.A;
    cpu.Y = (uint8_t)(cpu.Y & 0xDF);
    show_cockpit_message();
}

/* show_message_with_d8 @ $4958 — set the cockpit-message colour ($00D8 = $48), then
 * render the message (entry Y = message id) via the (native) show_cockpit_message. */
void show_message_with_d8(void) {
    text_color_pf0 = 0x48;
    show_cockpit_message();                     /* uses cpu.Y (id) */
}
static void show_message_with_d8_core(uint8_t id) { cpu.Y = id; show_message_with_d8(); }

/* show_message_id_a @ $4956 — stash the message/event mode ($0072 = entry A), then show
 * the message via show_message_with_d8 (entry Y = message id). */
void show_message_id_a(void) {
    flight_mode_state = cpu.A;                       /* $0072 (event/message mode) = A */
    show_message_with_d8();                     /* uses cpu.Y (id) */
}
static void show_message_id_a_core(uint8_t a, uint8_t id) {
    cpu.A = a; cpu.Y = id; show_message_id_a();
}

/* show_ace_or_message @ $493D — the ACE-rank / pilot-message driver.  Entry Y = message id.
 *   • rescue-active flag $003A bit7 CLEAR: show the fixed message id $CD and latch the
 *     current event glyph ($00DF = $1C).
 *   • $003A bit7 SET: a normal pilot message — re-init the HUD counter ($0676) via
 *     store_676_init(A=1), reset the pilot-rescue state if no landing target is queued, then
 *     re-show the saved message id (A = Y = id) via show_message_id_a. */
void show_ace_or_message(void) {
    if (!(mem[0x003A] & 0x80)) {                /* $3A bit7 CLEAR -> fixed message $CD */
        span_pixel_count = 0x1C;                /* $00DF = current event glyph $1C */
        show_message_with_d8_core(0xCD);        /* Y = $CD (fixed message id) */
        return;
    }
    uint8_t id = cpu.Y;                         /* $00BB = saved entry id */
    dl_y1 = id;
    cpu.A = 0x01; store_676_init();             /* A = 1 (HUD counter re-init) */
    reset_pilot_state_if_no_2830();
    show_message_id_a_core(id, id);             /* A = Y = saved id */
}

/* level_clear_fx_loop @ $7B94 — the level-cleared visual/audio flourish.  Sets the
 * landing-inhibit flag ($283C), then runs 15 frames each queuing a pair of ring events
 * (ids 1 and 2 via ring_push_unmarked) with $066C/$066D holding the frame counter, then a
 * 60-frame screen-flash loop poking terrain_pen1_fade ($00DB) with RANDOM|4.  Finally
 * clears $283C.  Frame-driven (wait_frames_1). */
void level_clear_fx_loop(void) {
    landing_inhibit_flag = (uint8_t)(landing_inhibit_flag + 1);   /* INC $283C */
    for (uint8_t x = 1; x < 0x10; x++) {        /* X = 1..15 */
        mem[0x066C] = x; mem[0x066D] = x;
        cpu.Y = 0x01; ring_push_unmarked();          /* queue ring event id 1 */
        cpu.Y = 0x02; ring_push_unmarked();          /* queue ring event id 2 */
        wait_frames_1();
    }
    for (uint8_t x = 0x3C; x != 0; x--) {       /* 60-frame flash */
        wait_frames_1();
        terrain_pen1_fade = (uint8_t)(bus_read(0xD20A) | 0x04);   /* $00DB = RANDOM | 4 */
    }
    landing_inhibit_flag = 0x00;
}

static void cockpit_dial_update_core(uint8_t v);   /* defined below ($8371); used here via the direct core call */

/* event_sequence_dispatcher @ $4644 — the in-flight keyboard-command handler.  Entry X = the
 * KBCODE the POKEY keyboard IRQ left (see CLAUDE.md "Controls").  Matches it against the command
 * table $4816[0..7] and dispatches by the matched SLOT (0=Land 1=? 2=ACE/msg 3=Systems 4=AirLock
 * 5=Boosters/thrust-up 6=? 7=BREAK/restart) plus the current flight mode ($0072 flight_mode_state).
 * Faithful to the 6502 stack tricks the takeover (slot 7) uses: a PHA;PHA;PHA;RTI that the
 * transpiler models as PLP;return, and a PLA x8 unwind — both stack-only, so the fixture ignores
 * the stack page and cpu diffs are incidental.  All callees are native/transpiled twins. */
void event_sequence_dispatcher(void) {
    ROF_MEMBASE_DECL(mb);   /* 136 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* Consume the pending-event flag; if it was negative, restore the saved display params. */
    uint8_t pending = event_pending_flag;               /* $063B */
    event_pending_flag = 0x00;
    span_pixel_count = 0x00;                             /* $00DF = 0 */
    if (pending & 0x80) {                                /* old $063B negative */
        for (int y = 0x0F; y >= 0; y--)
            mem[MEM_display_param_0 + y] = mem[MEM_attract_palette_src + y];   /* $07E9 -> $00CF */
    }

    /* Match the keycode (entry X) against the command table; slot = highest matching index. */
    uint8_t key = cpu.X;
    int slot;
    for (slot = 7; slot >= 0; slot--)
        if (key == mem[0x4816 + slot]) break;
    if (slot < 0) return;                                /* not a command key */

    if (level_or_state != 0) return;                     /* commands only during gameplay ($0004==0) */

    if (slot == 7) {                                     /* BREAK — event takeover / restore */
        if (event_active_flag != 0) {                    /* $0043: takeover active -> end it */
            event_active_flag = 0x00;
            joystick_saved = saved_joy_4a;               /* $004A = $0630 */
            cpu.S = (uint8_t)(cpu.S + 8);                /* PLA x8: discard the pushed IRQ frame */
            cpu.Y = 0x01;                                /* Y=0 after the loop, INY -> 1 */
            refresh_hud_fields_0d_0e();                  /* uses cpu.Y */
            refresh_hud_field_0b();
            return;
        }
        if (sfx_state_0634 != 0) return;                 /* $0634: SFX busy -> ignore */
        saved_joy_4a = joystick_saved;                   /* $0630 = $004A */
        joystick_saved = 0x00;
        for (int x = 6; x >= 0; x -= 2) mem[0xD201 + x] = 0x00;   /* clear AUDC1-4 ($D201/3/5/7) */
        zp_flag_05 = 0x00;                               /* $0005 = 0 */
        copy_display_params_to_buffer();
        event_active_flag = (uint8_t)(event_active_flag + 1);    /* INC $0043 */
        /* Faked RTI: push a $52BB return frame + status $04, then RTI.  The transpiler models
         * this as PLP;return (on HW $52BB is the flight-loop continuation); replicate the stack
         * writes so the ignored stack page still matches the oracle. */
        mem[0x0100 + cpu.S] = 0x52; cpu.S--;
        mem[0x0100 + cpu.S] = 0xBB; cpu.S--;
        mem[0x0100 + cpu.S] = 0x04; cpu.S--;
        cpu.S = (uint8_t)(cpu.S + 1);                    /* PLP: pull the status byte back off */
        return;
    }

    /* --- slots 0..6 --- */
    if (event_active_flag != 0) return;                  /* $0043: ignore commands during takeover */
    span_pixel_count = 0x21;                             /* $00DF = $21 (default event glyph) */
    if (slot == (int)flight_mode_state) return;               /* CPY $0072: no-op if slot == current mode */

    if (slot == 3) {                                     /* Systems key: toggle systems ($003E) */
        if (landing_seq_flag < 0x02) return;             /* only >= landing phase 2 */
        if (clear_colors_done_003E == 0) {               /* systems ON -> turn OFF */
            copy_display_params_to_buffer();
            clear_colors_done_003E = (uint8_t)(clear_colors_done_003E + 1);   /* INC $003E */
            colpf0_value = 0x3A;
            timer_or_counter = 0x2F;
        } else {                                         /* systems OFF -> turn back ON */
            msg_flash_timer = 0xFF;
            cpu.A = 0x00; clear_pilot_rescue_state();    /* clears $003E + pilot flags */
            timer_or_counter = 0x0E;
        }
        return;
    }

    if (slot == 1) {                                     /* alien / windscreen event */
        if (alien_trigger != 0) {                        /* $0633 */
            colpf0_value = 0x28;                         /* $0047 = $28 */
            span_pixel_count = 0x17;                     /* $00DF = $17 */
            cpu.Y = 0x71; set_colpf0_from_flag();        /* show message id $71 */
            return;
        }
        if (clear_colors_done_003E == 0) return;         /* $003E == 0 */
        if (airlock_state == 0) {                       /* start the shake: INC $003C */
            airlock_state = (uint8_t)(airlock_state + 1);
            mem[0x3388] = 0x34;                          /* cockpit cell */
            span_pixel_count = 0x16;                     /* $00DF = $16 */
            cpu.Y = 0x01; set_colpf0_from_flag();        /* Y = slot (1) */
            return;
        }
        if (airlock_state & 0x80) return;               /* $003C negative */
        if (landing_seq_flag == 0x04 && mem[0x007A] == 0) return;
        airlock_state = (uint8_t)(airlock_state - 1);  /* DEC $003C */
        mem[0x3388] = 0xB4;
        span_pixel_count = 0x16;                         /* $00DF = $16 */
        cpu.Y = 0x0A; set_colpf0_from_flag();
        return;
    }

    if (slot == 2) { cpu.Y = 0x02; show_ace_or_message(); return; }   /* Y = slot */

    if (life_counter == 0) return;                       /* $062F: no fuel/lives -> ignore */

    if (slot == 4) {                                     /* Air Lock */
        flight_mode_state = 0xFF;                             /* $0072 = $FF */
        if (landing_seq_flag != 0) {                     /* $003D */
            if (pilot_prev == 0) timer_or_counter = 0x00;    /* $0044 = $288E (== 0) */
            reset_pilot_state_if_no_2830();              /* leaves A = 0 */
            pitch_pos_lo = 0x00; pitch_pos_hi = 0x00;    /* $0025/$0026 = A(0) */
            cockpit_dial_update_core(0x07);
        } else if (dial_value != 0) {                    /* $006F: nudge dial down */
            dial_value = (uint8_t)(dial_value - 1);
            redraw_dial_from_6f();
        }
        clear_message_buffer();
        return;
    }

    if (landing_seq_flag != 0) return;                   /* $003D: slots 5/0/6 only when landed */

    if (slot == 5) {                                     /* Boosters — thrust up (cap 6) */
        flight_mode_state = 0xFF;
        if (0x06 >= dial_value) {                        /* CMP #6 vs $6F; INC unless dial > 6 */
            dial_value = (uint8_t)(dial_value + 1);
            redraw_dial_from_6f();
        }
        clear_message_buffer();
        return;
    }

    if (slot == 0) {                                     /* Land */
        if (terrain_clearance >= 0x64) {                 /* too high to land */
            span_pixel_count = 0x1C;                     /* $00DF = $1C */
            cpu.A = 0xFF; cpu.Y = 0xC5; show_message_id_a();
        } else {
            cockpit_dial_update_core(0x08);
            cpu.A = 0x00; cpu.Y = 0x04; show_message_id_a();
        }
        return;
    }

    /* slot == 6 */
    flight_mode_state = (uint8_t)slot;                        /* $0072 = Y (= 6) */
    cpu.Y = (uint8_t)slot; set_colpf0_from_flag();
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* ===========================================================================
 *  Pilot-rescue state-machine cluster (native twins of the pilot_render group).
 *  These drive the landing/rescue sequence: the colour-sweep-done flag $003E,
 *  the landing-sequence counter $003D, the pilot visibility flags, and the HUD
 *  message/colour bookkeeping the sequence uses.  Faithful, mem[]-only.
 * =========================================================================== */

/* set_colpf0_from_flag @ $47A3 — choose the cockpit playfield colour by the message
 * id's bit5: ids with bit5 set use the fixed $CA "flash" colour, others use the live
 * colpf0_value shadow; then render the message via save_color_clear_y_bit5 (which
 * needs A = colour and Y = id).  Entry Y = message id.  (The Atari also poked COLPF0
 * $D016 here — dropped; the copper owns colour on the Amiga.)
 * NOTE: bit5 CLEAR selects $CA (per the $47a6 BNE), the opposite of the old header
 * comment; verified against disasm + make validate. */
void set_colpf0_from_flag(void) {
    cpu.A = (cpu.Y & 0x20) ? colpf0_value : 0xCA;
    save_color_clear_y_bit5();                 /* consumes A (colour) + Y (id) */
}

/* clear_pilot_rescue_state @ $4968 — reset the pilot-rescue state: clear the
 * colour-sweep-done flag and both pilot-visibility flags.  All three take the entry
 * A (callers always pass 0). */
void clear_pilot_rescue_state(void) {
    uint8_t v = cpu.A;
    clear_colors_done_003E = v;
    pilot_visible = v;
    pilot_prev    = v;
}

/* reset_pilot_state_if_no_2830 @ $495F — when no landing target is queued
 * ($2830 == 0), also clear the landing-sequence counter $003D; then fall into
 * clear_pilot_rescue_state with A = 0. */
void reset_pilot_state_if_no_2830(void) {
    cpu.A = 0x00;
    if (mem[0x2830] == 0x00)                    /* $2830: queued-landing-target slot (unnamed) */
        landing_seq_flag = 0x00;
    clear_pilot_rescue_state();                 /* consumes A (= 0) */
}

/* copy_display_params_to_buffer @ $4971 — snapshot the 16 live display parameters
 * ($00CF..$00DE) into the palette-strobe buffer ($07E9..$07F8); the event/systems
 * path saves them here before running a colour-clear sweep. */
void copy_display_params_to_buffer(void) {
    for (int i = 0; i < 16; i++)
        mem[MEM_attract_palette_src + i] = mem[MEM_display_param_0 + i];
}

/* audf2_sweep_clear_colors @ $6DF4 — pitch-sweep the AUDF2 audio channel: for
 * frame_wait_count ($4C) frames, subtract audf2_sweep_step ($08DC) from audf2_sweep_val
 * ($08DB) and emit the new value to AUDF2 ($D202, POKEY -> Paula), one step per frame.
 * frame_wait_count is clobbered by the per-frame wait, so it is saved and restored. */
void audf2_sweep_clear_colors(void) {
    uint8_t saved = frame_wait_count;            /* PHA: preserve across the sweep */
    uint8_t n = frame_wait_count;                /* frame/step count (0 => 256, per DEY;BNE) */
    do {
        audf2_sweep_val = (uint8_t)(audf2_sweep_val - audf2_sweep_step);
        bus_write(0xD202, audf2_sweep_val);      /* AUDF2 pitch — real audio via Paula */
        wait_frames_1();                         /* one vertical-blank period */
        n--;
    } while (n != 0);
    frame_wait_count = saved;                    /* PLA -> $4C */
}

/* clear_colors_sweep_5x @ $7A89 — the systems-off / rescue colour-clear timer: up to 5
 * passes (Y=5..0), each waiting max(clear_color_count_007D, $14) frames while the
 * sweep-active flag clear_colors_done_003E stays nonzero, queuing a $1A ring event per
 * completed pass.  Aborts as soon as $003E clears (systems back on).
 * EXIT Z IS LOAD-BEARING: pilot_render tests it (`BEQ`/`BNE` after the call) — Z=1 when
 * aborted early ($003E==0), Z=0 when all passes completed (the 6502's final DEY -> $FF). */
void clear_colors_sweep_5x(void) {
    for (int pass = 0; pass < 6; pass++) {       /* Y = 5,4,3,2,1,0 via DEY;BPL */
        uint8_t frames = clear_color_count_007D;
        if (frames < 0x14) frames = 0x14;        /* minimum sweep length */
        do {
            wait_frames_1();
            if (clear_colors_done_003E == 0) {   /* aborted — leave Z=1 for the caller */
                cpu.A = 0x00; cpu.Z = 1; cpu.N = 0;
                return;
            }
            frames--;
        } while (frames != 0);
        cpu.X = 0x1A; ring_push_marked();        /* queue this pass's colour-clear event */
    }
    cpu.Z = 0; cpu.N = 1;                         /* all passes done — leave Z=0 for the caller */
}

/* animate_clear_colors_timed @ $7A17 — the once-per-256-frames colour-clear stepper for the
 * rescue/landing sequence.  Runs only on the RTCLOK==0 tick.  Two modes by the step counter
 * anim_counter_007B ($7B):
 *   • $7B == 0: queue two $19 ring events around a 10-frame wait, double the clear-colour count
 *     ($7D), re-seed RTCLOK from $007C, and return.
 *   • $7B >= $80: finish the sequence — mark the grid slot / show the countdown glyph, set
 *     landing_seq_flag = 2, timer_or_counter = 9.
 *   • else ($7B in 1..$7F): pick this tick's loop count from RANDOM (biased 3..7, halved unless
 *     $7B has reached 7), then run that many passes: each queues a $1B event + waits $007D frames
 *     (unless airlock_state is set / already aborted), decays $7B, and while $7B < $0F grows $7D
 *     by 2 and pulls $007C down by 7.  Finally re-seeds RTCLOK from $007C.
 * (6502 note: the $7B==0 path's 7a2e branch is ALWAYS taken because ring_push_marked exits with
 * Z=0 from its TAX of X=$19 — not from the ASL — so it never falls into the $7B-sign branch.) */
void animate_clear_colors_timed(void) {
    ROF_MEMBASE_DECL(mb);   /* 71 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    if (RTCLOK_LOW != 0) return;                         /* 7a17/7a19 */

    uint8_t y = anim_counter_007B;                       /* 7a1b LDY $7B */
    if (y == 0) {
        anim_counter_007B--;                             /* 7a1f DEC $7B (0 -> $FF) */
        cpu.X = 0x19; ring_push_marked();                /* 7a23 */
        wait_frames_10();                                /* 7a26 */
        clear_color_count_007D = (uint8_t)(clear_color_count_007D << 1);  /* 7a29 ASL $7D */
        cpu.X = 0x19; ring_push_marked();                /* 7a2b */
        RTCLOK_LOW = mem[0x007C];                        /* 7a2e always -> L_7a84 */
        return;
    }

    if (y & 0x80) {                                      /* 7a30 BPL not taken (step counter high) */
        mark_slot_and_countdown_char();                  /* 7a32 */
        landing_seq_flag = 0x02;                         /* 7a37 */
        timer_or_counter  = 0x09;                        /* 7a3b */
        return;                                          /* 7a3d */
    }

    /* L_7a3e — loop count from RANDOM. */
    uint8_t a = (uint8_t)(bus_read(0xD20A) & 0x07);      /* 7a3e/7a41 (the one RANDOM read) */
    if (a < 0x03) a |= 0x03;                             /* 7a43/7a45/7a47 */
    if (y < 0x07) a >>= 1;                               /* 7a49/7a4b/7a4d */
    y = a;                                               /* 7a4e TAY */

    int skip_wait = 0;                                   /* first iteration enters at L_7a6d? */
    if (y != 0) {
        row_table_stride = clear_color_count_007D;       /* 7a54/7a56 $C1 = $7D (inner-count scratch) */
    } else {
        y = 1;                                           /* 7a51 INY */
        skip_wait = 1;                                   /* 7a52 -> L_7a6d */
    }

    for (;;) {
        if (!skip_wait) {                                /* L_7a58 */
            if (airlock_state == 0) {                   /* 7a58/7a5a */
                cpu.X = 0x1B; ring_push_marked();        /* 7a5c/7a5e */
                uint8_t x = row_table_stride;            /* 7a61 LDX $C1 */
                do {                                     /* L_7a63 */
                    wait_frames_1();
                    if (clear_colors_done_003E == 0) return;   /* 7a66/7a68 aborted */
                    x--;                                 /* 7a6a */
                } while (x != 0);                        /* 7a6b */
            }
        }
        skip_wait = 0;

        uint8_t step = anim_counter_007B;                /* 7a6d LDA $7B */
        if (step != 0) anim_counter_007B--;              /* 7a6f/7a71 */
        if (step < 0x0F) {                               /* 7a73/7a75 CMP #$0F; BCS */
            clear_color_count_007D++;                    /* 7a77 */
            clear_color_count_007D++;                    /* 7a79 */
            mem[0x007C] = (uint8_t)(mem[0x007C] - 7);    /* 7a7b/7a7d/7a7f SBC #6 with C=0 */
        }
        y--;                                             /* 7a81 DEY */
        if (y == 0) break;                               /* 7a82 BNE L_7a58 */
    }
    RTCLOK_LOW = mem[0x007C];                            /* 7a84/7a86 */
#ifdef ROF_MEMBASE
#undef mem
#endif
}

#ifdef ROF_PLATFORM_AMIGA
/* Debug test aid — force every rescued figure to be the alien jump-scare (see the marker note
 * in pilot_render below).  Default 0 (no change); set to 1 at runtime, or build FORCE_ALIEN=1
 * to default it on.  Amiga-only so the validation oracle is unaffected. */
volatile uint8_t g_forceAlienRescue =
#ifdef ROF_FORCE_ALIEN_RESCUE
    1;
#else
    0;
#endif

/* Debug test aid — auto-open the airlock at the phase-4 knock so a headless run can reach the
 * alien-attack REVEAL without an interactive A-key press.  The reveal is gated on $003C != 0
 * (event_sequence_dispatcher slot-1 first press does INC $003C 0->1 + lights the airlock cell
 * $3388); this replicates exactly that first press, on the frame the knock is reached.  Default
 * 0; `make FORCE_AIRLOCK=1` (or poke at runtime) turns it on.  Amiga-only so the oracle is
 * unaffected.  Use with FORCE_ALIEN=1 to designate the alien in the first place. */
volatile uint8_t g_forceAirlockOpen =
#ifdef ROF_FORCE_AIRLOCK_OPEN
    1;
#else
    0;
#endif

/* Debug test aid (range-1 poly4) — headlessly simulate a downed pilot at range 1 so the
 * pilot/scanner code paths run without an interactive fly-over.  Poke g_forcePilotR1=1 at
 * runtime (gdb) once flight is stable; the flight loop then holds pilot-visible ($003E) +
 * pilot distance ($0079=1, range digit 1) each iteration so pilot_render runs at close range.
 * Off by default; Amiga-only so the oracle/validation are unaffected. */
volatile uint8_t g_forcePilotR1 = 0;
#endif

#if defined(ROF_PLATFORM_AMIGA) && defined(ROF_FLIGHT_PROBE)
/* Alien-attack render diagnosis probe (2026-07-25).  Latches, while the alien is active
 * ($0633 alien_trigger != 0 during a FORCE_ALIEN rescue), whether/how the creature is drawn.
 * Read from gdb after playing to a rescue.  g_alPlotCalls>0 ⇒ the alien IS drawn as a bitmap
 * via plot_clipped_pixel (so the fix is in the Amiga composite path); ==0 ⇒ it's drawn some
 * other way (PMG / different primitive).  g_alSnap = latest per-frame state snapshot. */
extern int g_figRowLo, g_figRowHi;
volatile unsigned char  g_alSeen = 0;        /* ever entered the alien-attack state ($0633!=0) */
volatile unsigned long  g_alFrames = 0;      /* pilot_render passes with $0633!=0 */
volatile unsigned long  g_alPlotCalls = 0;   /* plot_clipped_pixel calls while $0633!=0 */
volatile unsigned long  g_alDrawShape = 0;   /* draw_scaled_shape calls while $0633!=0 */
volatile unsigned short g_alShapePtr = 0;    /* $C3/$C4 shape ptr during an alien-frame draw */
volatile int            g_alFigLo = 99, g_alFigHi = -1;  /* g_figRowLo/Hi extent during attack */
/* Broader rescue-wide capture (confirms the sequence actually happened, regardless of $0633): */
volatile unsigned char  g_alRescueSeen = 0;  /* ever in a systems-off rescue ($003E!=0) */
volatile unsigned long  g_alRescueFrames = 0;/* pilot_render passes with $003E!=0 */
volatile unsigned long  g_alRescuePlot = 0;  /* plot_clipped_pixel calls while $003E!=0 (whole rescue) */
volatile unsigned char  g_alMaxPhase = 0;    /* max $003D reached during a rescue */
volatile unsigned char  g_alAirlock = 0;     /* $003C ever went nonzero (airlock opened) */
volatile unsigned char  g_alAirlockMax = 0;  /* max $003C seen */
/* g_alSnap: [0]$3C [1]$3D [2]$3E [3]$0633 [4]$44 [5]$47 [6]$3A [7]$283D [8]$79 [9]$41
 *           [10]$2844 [11]$DB [12]figLo [13]figHi [14]$C3 [15]$C4 */
volatile unsigned char  g_alSnap[16];
static void rof_alien_probe(void) {
    if (mem[0x003E]) {                       /* systems-off rescue active */
        g_alRescueSeen = 1; g_alRescueFrames++;
        if (mem[0x003D] > g_alMaxPhase) g_alMaxPhase = mem[0x003D];
        if (mem[0x003C]) { g_alAirlock = 1; if (mem[0x003C] > g_alAirlockMax) g_alAirlockMax = mem[0x003C]; }
    }
    if (mem[0x0633]) { g_alSeen = 1; g_alFrames++; }
    /* snapshot latest state whenever a rescue OR alien is active */
    if (mem[0x003E] || mem[0x0633]) {
        g_alSnap[0]=mem[0x003C]; g_alSnap[1]=mem[0x003D]; g_alSnap[2]=mem[0x003E];
        g_alSnap[3]=mem[0x0633]; g_alSnap[4]=mem[0x0044]; g_alSnap[5]=mem[0x0047];
        g_alSnap[6]=mem[0x003A]; g_alSnap[7]=mem[0x283D]; g_alSnap[8]=mem[0x0079];
        g_alSnap[9]=mem[0x0041]; g_alSnap[10]=mem[0x2844]; g_alSnap[11]=mem[0x00DB];
        g_alSnap[12]=(unsigned char)(g_figRowLo & 0xFF); g_alSnap[13]=(unsigned char)(g_figRowHi & 0xFF);
        g_alSnap[14]=mem[0x00C3]; g_alSnap[15]=mem[0x00C4];
        if (g_figRowLo < g_alFigLo) g_alFigLo = g_figRowLo;
        if (g_figRowHi > g_alFigHi) g_alFigHi = g_figRowHi;
    }
}
/* Decisive bitmap-vs-not capture: record up to 12 DISTINCT draw_scaled_shape shape pointers
 * ($C3/$C4) seen during the systems-off rescue.  The pilot/figure frames are $7DEF/$7E25/
 * $7E5B/$7E91 — if a shape OUTSIDE that set appears, the alien creature IS a bitmap (and we
 * port it via the existing figure path); if ONLY those 4 ever appear, the creature is drawn
 * some other way (PMG / a non-draw_scaled_shape primitive). */
volatile unsigned short g_alShapes[12];
volatile unsigned char  g_alShapeCount = 0;
static void rof_alshape_note(unsigned short p) {
    for (unsigned i = 0; i < g_alShapeCount; i++) if (g_alShapes[i] == p) return;
    if (g_alShapeCount < 12) g_alShapes[g_alShapeCount++] = p;
}
#define ROF_ALIEN_PROBE()       rof_alien_probe()
#define ROF_ALIEN_PLOT()        do { if (mem[0x003E]) g_alRescuePlot++; if (mem[0x0633]) g_alPlotCalls++; } while (0)
#define ROF_ALIEN_DRAWSHAPE()   do { unsigned short _p = (unsigned short)(mem[0x00C3] | (mem[0x00C4] << 8)); \
    if (mem[0x003E] || mem[0x0633]) rof_alshape_note(_p); \
    if (mem[0x0633]) { g_alDrawShape++; g_alShapePtr = _p; } } while (0)

/* Creature-blit capture (alien_creature_animate_draw -> $80C5 `STA ($8D),Y`, hooked at $80E9 via a
 * PRE_INSN_HOOK).  The jump-scare creature is drawn ONLY here (airlock-closed knock), into the
 * mode-D field via a runtime-built row table ($073D/$0793), so the field->overlay geometry can't
 * be derived statically.  Record the write extent + row-table base/stride + the sample so a real
 * closed-airlock knock reveals the exact mapping we need for the plane hook. */
volatile unsigned long  g_alCrWrites   = 0;       /* total $80C5 byte-stores */
volatile unsigned int   g_alCrAddrLo   = 0xFFFFu; /* min field address written */
volatile unsigned int   g_alCrAddrHi   = 0;       /* max field address written */
volatile unsigned char  g_alCrValOr    = 0;       /* OR of all stored bytes (which pixels used) */
volatile unsigned int   g_alCrRowBase  = 0;       /* $073D[0]|$0793[0]<<8 (row-table base) */
volatile int            g_alCrRowStride = 0;      /* $073D[1] - $073D[0] (row stride) */
volatile unsigned char  g_alCrPos0     = 0;       /* $2930 at first write (start row idx) */
volatile unsigned char  g_alCrPos1     = 0;       /* $2931 at first write (col offset) */
volatile unsigned char  g_alCrSeen     = 0;       /* creature blit ran at least once */
/* Colour diagnosis: the viewport pen ramp ($00DA-$DD, read by updateFlightCopper into the flight
 * copper color01/02/03) + the attack colour ($0047) + $0044, snapshot each knock frame.  If these
 * render the creature's pens (2 = $DA, 3 = $DB, ...) near the sky/background colour, the shape is
 * composited but invisible until the normal palette returns (the observed restore-frame flash). */
volatile unsigned char  g_alPen[6];               /* [0]$DA [1]$DB [2]$DC [3]$DD [4]$47 [5]$44 */
volatile unsigned long  g_alKnockFrames = 0;      /* alien_knock_setup_loop loop iterations (knock frames) */
volatile unsigned long  g_alTWait = 0;            /* beam ticks in the 5-frame busy-wait (all steps) */
volatile unsigned long  g_alTDraw = 0;            /* beam ticks in alien_creature_animate_draw (all steps) */
volatile unsigned long  g_alTRender = 0;          /* beam ticks in platform_render_frame (all steps) */
volatile unsigned long  g_alTHud = 0;             /* beam ticks inside alien_shape_blit (creature rows) */
volatile unsigned long  g_alHudCalls = 0;         /* # alien_shape_blit calls during the knock */
#else
#define ROF_ALIEN_PROBE()       ((void)0)
#define ROF_ALIEN_PLOT()        ((void)0)
#define ROF_ALIEN_DRAWSHAPE()   ((void)0)
#endif

/* Creature-blit write hook (PRE_INSN_HOOK at $80E9, emitted into rof_gen.c in ALL builds, so this
 * must exist everywhere; the body is probe/Amiga-only).  addr = ($8D)+Y target, val = byte stored. */
void rof_alien_crwrite(unsigned int addr, unsigned char val) {
#if defined(ROF_PLATFORM_AMIGA) && defined(ROF_FLIGHT_PROBE)
    if (!g_alCrSeen) {
        g_alCrSeen = 1;
        g_alCrRowBase   = (unsigned int)(mem[0x073D] | (mem[0x0793] << 8));
        g_alCrRowStride = (int)((unsigned int)(mem[0x073E] | (mem[0x0794] << 8)) - g_alCrRowBase);
        g_alCrPos0 = mem[0x2930];
        g_alCrPos1 = mem[0x2931];
    }
    g_alCrWrites++;
    g_alCrValOr |= val;
    if (addr < g_alCrAddrLo) g_alCrAddrLo = addr;
    if (addr > g_alCrAddrHi) g_alCrAddrHi = addr;
#else
    (void)addr; (void)val;
#endif
}

/* pilot_render @ $7854 — the pilot/rescue render + rescue state machine.  Seeds the lock-on /
 * landing state from the pilot range ($0079), then runs a per-frame loop (L_78d6) that animates the
 * alien knock ($0633 / alien_attack_tick), and:
 *   • systems ON ($003E==0): finish/abort the rescue and return (via level_clear_fx_loop);
 *   • systems OFF ($003E!=0): step the landing sequence $003D — colour sweeps, the descending audio,
 *     the zoom, and the L_78d6 hold loop.  This is FAITHFUL: the real Atari hard-holds here too (see
 *     [[flight-scene]]); the loop exits only when systems come back on ($003E->0 via the VBI).
 * The systems-off freeze fix lives in the L_78d6 / L_79d0 frame-yields below (Amiga: keep the display
 * + VBI live while spinning — the transpiled twin got these from SPINWAIT_HOOKS, so this native twin
 * must reproduce them or the freeze returns).  This is a faithful mirror of the transpiled CFG (goto
 * labels match the $76xx/$79xx addresses); cpu.A/X/Y propagation is kept where a value feeds a store
 * or callee, and the delicate carry chains use the cpu-op macros (byte-exact to the oracle).
 * VALIDATION: only the $003E==0 path is diffed (the $003E!=0 loop can't advance RTCLOK headless — the
 * $793d/$797f arithmetic there is inspection-only).  $004D/$0079/$281E/$005A/$28E5/$28E9/$003F/$0040
 * are unnamed scratch — see docs/rename.md. */
void pilot_render(void) {
    ROF_MEMBASE_DECL(mb);   /* 190 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    lock_on_indicator_state = 0x80;                      /* 7856 $7E */
    mem[0x004D] = 0x80;
    mem[0x2830] = 0x80;
    mem[0x007A] = 0x80;
    /* Seed colour ($47) / timer ($44) + landing phase from the pilot range $0079. */
    {
        uint8_t r = mem[0x0079];                         /* 785f */
        if (r & 0x80)       { cpu.X = 0x2A; cpu.Y = 0x68; }   /* 7863 very near */
        else if (r >= 0x08) { cpu.X = 0x1A; cpu.Y = 0x67; }   /* 786e mid */
        else                { cpu.Y = 0x46; landing_seq_flag = (uint8_t)(landing_seq_flag + 1); }  /* 7875 close: INC $003D (X = entry X) */
    }
    colpf0_value = cpu.X;                                /* 7879 STX $47 */
    timer_or_counter = cpu.Y;                            /* 787b STY $44 */

    if (landing_seq_flag >= 0x03) {                      /* 787f CMP #3; BCC L_78d6 */
        /* Landing well underway: seed the colour-sweep + audio params + grid slot. */
        mem[0x0642] = 0x00;
        plot_step_lo = 0x00;                             /* $50 */
        cpu.A = 0x00; cpu.C = 1; ADC(mem[0x0079]);       /* A = 0 + $0079 + 1 (C=1 from the CMP #3 that reached here) */
        plot_step_hi = cpu.A;                            /* 788c $51 */
        SBC(0x12); EOR(0xFF); ASL_A();                   /* 788e-7892 */
        anim_counter_007B = cpu.A;                       /* 7893 $7B */
        mem[0x007C] = 0xA5;
        clear_color_count_007D = 0x0F;                   /* $7D */
        mem[0x28D9] = 0x80; mem[0x28DA] = 0x80;
        mem[0x0079] = 0x80;
        mem[0x281E] = 0x01;
        uint8_t v = mem[0x0A00 + grid_slot_index];       /* 78af $0A00,X (X=$28E6) */
#ifdef ROF_PLATFORM_AMIGA
        /* DEBUG TEST AID (g_forceAlienRescue): the rescued-figure fork is PURELY the map
         * marker here — $80 = alien jump-scare (keeps $281E=1 → on airlock-open $003C promotes
         * to $80 → alien_trigger $0633 set), anything else = a real pilot that boards ($281E
         * DEC'd to 0).  (Validated: a800dumps/rescue_pilot.a8s has marker $C8 and is a real
         * pilot.)  Whether a figure is designated alien is normally decided at spawn ($4E58).
         * ⚠ 2026-07-25: forcing the marker here does NOT reproduce the real alien jump-scare —
         * the probe (g_al*) shows the rescue stalls at the phase-4 knock ($003D never passes 4,
         * $0633 never sets, no shake, no creature); the real alien is designated at SPAWN and
         * auto-jumps at the knock via a timing-gated path this late render-time force can't reach.
         * Kept only as a partial test lever.  Off by default; poke g_forceAlienRescue=1 or build
         * FORCE_ALIEN=1.  Amiga-only + default-0, so the validation oracle + `make validate` are
         * unaffected. */
        if (g_forceAlienRescue) v = 0x80;
#endif
        if (v == 0x80) {                                 /* 78b2/78b4 */
            uint8_t g = mem[0x061B];                      /* 78b6 */
            if (g != 0 && g >= bus_read(0xD20A)) goto L_78d6;   /* 78b9/78be/78c0 */
            v = g;                                       /* A = $061B for the CMP #$C9 below */
        } else {
            mem[0x281E] = (uint8_t)(mem[0x281E] - 1);    /* 78c3 DEC $281E */
        }
        game_state = (uint8_t)(game_state + 1);          /* 78c6 INC $0041 */
        cpu.Y = 0x0C;
        if (v == 0xC9) { timer_or_counter = 0x55; cpu.Y = 0x68; }   /* 78ca-78d2 */
        terrain_pen1_fade = cpu.Y;                        /* 78d4 STY $DB */
    }

L_78d6:
#ifdef ROF_PLATFORM_AMIGA
    if (mem[0x003E]) { platform_tick_vbi(); platform_render_frame(); }   /* freeze fix: keep display+VBI live while systems-off */
#endif
    ROF_ALIEN_PROBE();                                   /* diag: latch rescue/alien state each L_78d6 pass */
    if (alien_trigger != 0) {                            /* 78d6/78d9 */
        if (RTCLOK_LOW & 0x08) {                         /* 78db/78dd/78df */
            if (mem[0x2844] == 0) { mem[0x2844] = (uint8_t)(mem[0x2844] + 1); alien_attack_tick(); }  /* 78e1-78e9 */
        } else {
            mem[0x2844] = 0x00;                          /* 78ef */
        }
    }

    /* L_78f2 */
    if (clear_colors_done_003E != 0) goto L_792e;        /* 78f2/78f4: systems OFF -> rescue loop */

    /* --- systems ON ($003E==0): finish/abort the rescue, then return. --- */
    if (airlock_state != 0) trigger_effect_4a();        /* 78f6/78f8/78fa */
    {
        uint8_t phase = landing_seq_flag;                /* 78fd LDY $003D */
        mem[0x2830] = 0x00;
        landing_seq_flag = 0x00;
        game_state = 0x00;
        pitch_pos_lo = 0x00; pitch_pos_hi = 0x00;
        if (phase != 0x04 && phase != 0x03) return;      /* 790c-7914: only phases 3/4 continue */
    }
    /* L_7915 */
    if (mem[0x281E] == 0) {                              /* 7915/7918 */
        cpu.A = 0x49; timer_or_counter = cpu.A; mark_slot_and_countdown_char();   /* 791a-791e */
    } else {
        cpu.A = 0x40; timer_or_counter = cpu.A; mark_slot_and_inc_count();        /* 7924-7928 */
    }
    level_clear_fx_loop();                               /* 792b */
    return;

L_792e:
    {
        uint8_t phase = landing_seq_flag;                /* 792e */
        if (phase < 0x03) goto L_78d6;                   /* 7930/7932 */
        if (phase == 0x04) goto L_79a8;                  /* 7934/7936/7938 */
        if (phase > 0x04) goto L_78d6;                   /* 793b BCS */
    }
    /* phase == 3: colour-sweep bit-shuffle + descending audio (inspection-only path). */
    mem[0x28E9] = plot_step_hi;                          /* 7941 */
    mem[0x005A] = bus_read(0xD20A);                      /* 7947 */
    if (mem[0x0040] >= 0x6C) mem[0x28E5] = bus_read(0xD20A);   /* 7949-7952 */
    else mem[0x28E5] = 0x00;                             /* 7955 */
    do {                                                 /* L_795a */
        cpu.A = mem[0x003F]; ASL_A();                    /* 795a/795c: C = bit7 of $3F */
        ROR_M(0x005A);                                   /* 795d ROR $5A */
        LSR_M(0x28E5);                                   /* 795f */
        LSR_M(0x28E9);                                   /* 7962 (Z from this) */
    } while (mem[0x28E9] != 0);                          /* 7965 BNE */
    if (plot_step_hi == 0x01) {                          /* 7967/7969/796b */
        mem[0x005A] = (uint8_t)(mem[0x005A] | 0x40);     /* 796d-7971 */
        if (plot_step_lo == 0x00) {                      /* 7973/7975 */
            uint8_t v40 = mem[0x0040];                   /* 7977 */
            if (v40 < 0x6C) {                            /* 7979/797b BCS L_7995 */
                RTCLOK_LOW = v40;                        /* 797d STA $14 */
                cpu.A = bus_read(0xD20A); cpu.C = 0;     /* 797f (C=0: v40<$6C) */
                ADC(bus_read(0xD20A));                   /* 7982 */
                ROR_A(); LSR_A(); LSR_A();               /* 7985-7987 */
                ADC(RTCLOK_LOW);                         /* 7988 ADC $14 */
                if (mem[0x281E] != 0) ADC(0x20);         /* 798a-798f */
                RTCLOK_LOW = cpu.A;                      /* 7991 STA $14 */
                landing_seq_flag = (uint8_t)(landing_seq_flag + 1);   /* 7993 INC $003D */
            }
            mem[0x28E5] = 0xFF;                          /* 7995/7997/799f */
        } else {
            mem[0x28E5] = (uint8_t)(mem[0x28E5] | 0x80); /* 799a-799f */
        }
    }
    /* L_79a2 */
    if (landing_seq_flag != 0x04) {                      /* 79a2/79a4/79a6 */
        animate_zoom_sequence();                         /* L_7a0f: CMP #4 gave C=0 (phase 3<4) -> run zoom */
        goto L_78d6;                                     /* 7a14 */
    }

L_79a8:
    if (mem[0x007A] != 0) {                              /* 79a8/79aa */
        if (RTCLOK_LOW != 0) goto L_78d6;                /* 79ac/79ae -> L_7a0c -> L_7a14 -> L_78d6 */
        mem[0x007A] = 0x00;                              /* 79b0 */
    }
    /* L_79b2 */
#ifdef ROF_PLATFORM_AMIGA
    /* FORCE-AIRLOCK test aid: at the knock (phase 4, alien-designated, systems off, airlock still
     * closed) simulate the A-key first press so the headless probe reaches the reveal below instead
     * of stalling in alien_knock_setup_loop's knock-SFX loop.  Equivalent to the dispatcher slot-1 path. */
    if (g_forceAirlockOpen && mem[0x281E] != 0 && clear_colors_done_003E != 0 && airlock_state == 0) {
        airlock_state = 0x01;                           /* INC $003C (0 -> 1) */
        mem[0x3388] = 0x34;                              /* light the Air Lock cockpit cell */
    }
#endif
    if (mem[0x281E] != 0) {                              /* 79b2/79b5 */
        cpu.A = airlock_state;                          /* 79b7 */
        if (cpu.A == 0) {                                /* 79b9 */
            game_state = cpu.A;                          /* 79bb (=0) */
            alien_knock_setup_loop();                             /* 79bd */
            goto L_78d6;                                 /* 79c0/79c9 -> L_7a0c -> L_7a14 */
        }
        airlock_state = 0x80;                           /* 79c3/79c5 -> L_79d9 */
        goto L_79d9;
    }
    /* L_79cc */
    if (airlock_state == 0) { animate_clear_colors_timed(); goto L_78d6; }   /* 79cc/79ce L_7a09 -> L_7a0c */
L_79d0:
    while (mem[0x06FF] != 0) {                           /* 79d0/79d3 sound-busy spin */
        platform_tick_vbi(); platform_render_frame();    /* SPINWAIT: keep audio/VBI live */
    }
    airlock_state = 0xFF;                               /* 79d5/79d7 */
L_79d9:
    clear_colors_sweep_5x();                             /* 79d9 (exit Z: aborted if $3E cleared) */
    if (cpu.Z) goto L_78d6;                              /* 79dc/79de */
    clear_message_buffer();                              /* 79e1 */
    wait_frames_20();                                    /* 79e4 */
    cpu.A = airlock_state;                              /* 79e7 */
    if (cpu.A == 0x80) {                                 /* 79e9/79eb */
        mark_grid_slot_active();                         /* 79ed (leaves cpu.A = $3C for the next store) */
        alien_trigger = cpu.A;                           /* 79f0 */
        colpf0_value = 0xD8;                             /* 79f5 */
        cpu.A = 0x70;                                    /* 79f7 */
    } else {
        bcd_oscillate_counter_0628();                    /* 79fc */
        cpu.A = 0x0A;                                    /* 79ff */
    }
    /* L_7a01 */
    init_event_state_5815_x16();                         /* 7a01 (entry A) */
    landing_seq_flag = (uint8_t)(landing_seq_flag + 1);  /* 7a04 INC $003D */
    goto L_78d6;                                         /* 7a06 -> L_7a0c -> L_7a14 */
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* shift_object_table_up @ $6A0F — shift the display-list LMS address pairs up by 3 bytes
 * ($3007/$3008[Y] -> $300A/$300B[Y]) for entry-A iterations, stepping Y down by 3. */
void shift_object_table_up(void) {
    screen_ptr_hi = cpu.A;
    uint8_t y = 0xFF;
    do {
        mem[0x300A + y] = mem[0x3007 + y];
        mem[0x300B + y] = mem[0x3008 + y];
        y = (uint8_t)(y - 3);
        screen_ptr_hi = (uint8_t)(screen_ptr_hi - 1);
    } while (screen_ptr_hi != 0x00);
}

/* set_coord_y_e0 @ $6805 — set the glyph source pointer $0084/$0085 = $E0(entry A + $80)
 * and blit it via the (native) blit_glyph_8rows. */
void set_coord_y_e0(void) {
    cpu.C = 0; ADC(0x80);                 /* CLC; ADC #$80 (entry A) */
    screen_ptr_hi = cpu.A;
    cpu.Y = 0xE0;
    encounter_count = cpu.Y;
    blit_glyph_8rows();
}

/* glyph_ptr_from_index @ $6773 — compute the glyph source pointer for index (entry A):
 * $0084/$0085 = $E000 + (index << 3) (with the 6502's single-ROL carry capture), then blit
 * via the (native) blit_glyph_8rows.  Reproduced with the cpu shift/ADC macros. */
void glyph_ptr_from_index(void) {
    cpu.Y = 0x00;
    encounter_count = cpu.Y;
    ASL_A(); ASL_A(); ASL_A();            /* index << 3 (carry from last shift) */
    screen_ptr_hi = cpu.A;
    ROL_M(0x0085);                        /* capture the last carry */
    cpu.C = 0; cpu.A = screen_ptr_hi; ADC(0x00); screen_ptr_hi = cpu.A;
    cpu.A = encounter_count; ADC(0xE0); encounter_count = cpu.A;   /* + $E000 */
    blit_glyph_8rows();
}

/* draw_glyph_2rows @ $4099 — draw a 2x2-byte glyph (rows $30 apart) from the table
 * $4AE3[index..index+3] (entry A), OR'd with $00BF, through the dest pointer $00BB at
 * column offsets 0,1,$30,$31. */
void draw_glyph_2rows(void) {
    uint8_t x = cpu.A;
    uint8_t y = 0x00;
    for (;;) {
        cpu.Y = y;
        bus_write(ZP_IND_Y(0x00BB), (uint8_t)(mem[0x4AE3 + x] | bar_col_threshold));
        x = (uint8_t)(x + 1);
        y = (uint8_t)(y + 1);
        if (y == 0x02) y = 0x30;          /* CPY #2; BNE; LDY #$30 */
        if (y == 0x32) break;             /* CPY #$32; BNE loop */
    }
}

/* rle_expand_list @ $757B — expand a list of RLE runs from the source pointer $00BB: each
 * (count,value) pair is expanded into the dest via the (native) rle_run_fill; a count of 0
 * terminates. */
void rle_expand_list(void) {
    ROF_MEMBASE_DECL(mb);   /* 61 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    cpu.Y = 0x00;
    for (;;) {
        cpu.A = bus_read(ZP_IND_Y(0x00BB));   /* ($BB)+0 = run length */
        if (cpu.A == 0x00) return;
        rle_run_fill();
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* rle_decompress @ $3C3D — decompress an RLE stream from the source pointer $00BB into the
 * dest $00BD/$00BE.  A byte < $C0 is a literal (copied once via copy_bytes_to_dst); a byte
 * >= $C0 is a run marker whose low 6 bits are the length (length 0 = $C0 terminator), with
 * the following byte the repeated value (via rle_run_fill). */
void rle_decompress(void) {
    cpu.Y = 0x00;
    for (;;) {
        cpu.A = bus_read(ZP_IND_Y(0x00BB));
        if (cpu.A < 0xC0) {                   /* CMP #$C0; BCC literal */
            cpu.X = 0x01;
            copy_bytes_to_dst();
        } else {
            cpu.A = (uint8_t)(cpu.A & 0x3F);
            if (cpu.A == 0x00) return;        /* $C0 terminator */
            rle_run_fill();
        }
    }
}

/* loader_util @ $3C00 — clear three regions with byte $00B7=0 via the (native) memset_or_copy:
 * $32B5..$32FC ($48 bytes), $1000..$2FFF ($2000 bytes), and $0B00..$0FFF ($500 bytes). */
void loader_util(void) {
    frame_counter = 0x00;
    row_table_stride = 0xB5; player_speed = 0x32; row_table_base_lo = 0x47; row_table_base_hi = 0x00;
    memset_or_copy();
    row_table_stride = 0x00; player_speed = 0x10; row_table_base_lo = 0xFF; row_table_base_hi = 0x1F;
    memset_or_copy();
    row_table_stride = 0x00; player_speed = 0x0B; row_table_base_lo = 0xFF; row_table_base_hi = 0x04;
    memset_or_copy();
}

/* game_init_77DF @ $77DF — build the 256-entry PMG/strip bit tables $BE00 and $BF00: for
 * each X (0..255) a rolling value seeded from X feeds, over 4 inner passes, 2 bits at a time
 * (via $780B[]/$780F[] lookups and LSR/ROR) into $BE00[X] and $BF00[X].  Pure; the carry-chain
 * ROR-into-memory is reproduced with the cpu macros. */
void game_init_77DF(void) {
    uint8_t x = 0x00;
    do {
        cpu.A = x;                                  /* TXA */
        row_table_stride = 0x04;
        do {
            uint8_t saved = cpu.A;                  /* PHA */
            cpu.A &= 0x03;
            uint8_t y = cpu.A;                      /* TAY */
            cpu.A = mem[0x780B + y];
            LSR_A(); ROR_M(0xBE00 + x);
            LSR_A(); ROR_M(0xBE00 + x);
            cpu.A = mem[0x780F + y];
            LSR_A(); ROR_M(0xBF00 + x);
            LSR_A(); ROR_M(0xBF00 + x);
            cpu.A = saved;                          /* PLA */
            LSR_A(); LSR_A();
            row_table_stride = (uint8_t)(row_table_stride - 1);
        } while (row_table_stride != 0x00);
        x = (uint8_t)(x + 1);                       /* INX */
    } while (x != 0x00);
}

/* plot_clipped_pixel @ $7D38 — plot one clipped HUD/radar pixel.  Entry A = source value
 * ($0058; 0 means "read the existing screen byte").  The pixel at ($004F,$004E) is plotted
 * only inside the window Y∈[$6C,$97) and X∈[$28,$D8) and when its packed column index stays
 * below $00B3.  The screen row pointers $0080 / $00C1 come from the row-addr table indexed by
 * $97-$004E; the pixel is masked into ($00C1)+col via $4F3B[X&3]/$7DEB[X&3].  $004F is always
 * incremented. */
void plot_clipped_pixel(void) {
    ROF_ALIEN_PLOT();                       /* diag: count bitmap-figure plots during the alien attack */
    plot_pixel_mask = cpu.A;
    uint8_t x = terrain_pt_coord_a;
    uint8_t y = terrain_pt_coord_b;
    if (y >= 0x6C && y < 0x97 && x >= 0x28 && x < 0xD8) {
        uint8_t ry = (uint8_t)(0x97 - terrain_pt_coord_b);
        cpu.C = 0;
        cpu.A = mem[MEM_row_base_lo + ry]; sync_flag = cpu.A; ADC(0x30); row_table_stride = cpu.A;
        cpu.A = mem[MEM_row_base_hi + ry]; dl_ptr_lo = cpu.A; ADC(0x00); player_speed = cpu.A;
        cpu.A = x;
        LSR_A(); LSR_A();
        cpu.C = 0; ADC(0xF8);
        uint8_t col = cpu.A;
        CMP(mem[0x00B3]);
        if (!cpu.C) {                               /* BCS skip => plot when col < $00B3 */
            cpu.Y = col;                            /* exit-Y contract (kept on both paths) */
            uint8_t mx = (uint8_t)(x & 0x03);
#ifndef ROF_PLATFORM_AMIGA
            /* SDL/validate: composite the figure pixel into the mode-D field — read the current
             * terrain (a transparent source keeps it), mask, OR the figure colour in, write back.
             * On the Amiga this whole ($80),Y-read + ($C1),Y-RMW is DEAD: during the paused
             * pilot-zoom nothing reads the mode-D field (renderFlightDirect composites the frozen
             * terrain + the g_fig* overlay) — which is precisely why the overlay exists.  So drop
             * it (up to 3 indirect bus accesses per figure pixel) and feed the overlay instead. */
            uint8_t a = plot_pixel_mask;
            if (a == 0x00) a = bus_read(ZP_IND_Y(0x0080));
            a &= mem[0x4F3B + mx];
            blit_color_src = a;
            uint8_t b = bus_read(ZP_IND_Y(0x00C1));
            b &= mem[0x7DEB + mx];
            b |= blit_color_src;
            bus_write(ZP_IND_Y(0x00C1), b);
#else
            /* Amiga: mirror opaque figure pixels into the rescue-figure overlay (see ROF_PLOT_FIG).
             * plot_pixel_mask==0 = a transparent (value-0) copy of the terrain — not part of the
             * figure, so skip it. */
            if (plot_pixel_mask != 0) {
                uint8_t v2 = (uint8_t)((plot_pixel_mask >> (6 - 2 * mx)) & 3u);
                if (v2) ROF_PLOT_FIG(x, y, v2);
            }
#endif
        }
    }
    terrain_pt_coord_a = (uint8_t)(terrain_pt_coord_a + 1);
}

/* unpack_bitmap_4d3e @ $74D7 — unpack a bitmap by bit-reversing bytes between buffers whose
 * pointers come from the $4D3E word table.  Outer 8 passes x middle 4 passes: src ptr $00C1 =
 * table[$0084], dst ptr $00C3 = table[$0080-$0081]; the byte loop reads ($C1)+Y, reverses its
 * bits (8x ASL/ROR into $0085), writes ($C3)+Y, stopping when a reversed byte is 0 or Y reaches
 * $80.  Indices step $0084/$0081 by 2 (middle) and $0080/$0084 by $12/$0A (outer). */
void unpack_bitmap_4d3e(void) {
    screen_ptr_hi = 0x00;
    sync_flag = 0x10;
    span_pixel_count = 0x08;
    do {
        dl_ptr_lo = 0x00;
        dl_ptr_hi = 0x04;
        do {
            uint8_t ys = screen_ptr_hi;
            row_table_stride = mem[0x4D3E + ys]; player_speed = mem[0x4D3F + ys];
            uint8_t yd = (uint8_t)(sync_flag - dl_ptr_lo);
            row_table_base_lo = mem[0x4D3E + yd]; row_table_base_hi = mem[0x4D3F + yd];
            uint8_t y = 0x00;
            for (;;) {
                cpu.Y = y;
                cpu.A = bus_read(ZP_IND_Y(0x00C1));
                for (int k = 0; k < 8; k++) { ASL_A(); ROR_M(0x0085); }  /* reverse bits */
                cpu.A = encounter_count;
                cpu.Y = y;
                bus_write(ZP_IND_Y(0x00C3), cpu.A);
                if (cpu.A == 0x00) break;            /* BEQ */
                y = (uint8_t)(y + 1);
                if (y & 0x80) break;                 /* BPL: loop while Y < $80 */
            }
            screen_ptr_hi = (uint8_t)(screen_ptr_hi + 2);
            dl_ptr_lo = (uint8_t)(dl_ptr_lo + 2);
            dl_ptr_hi = (uint8_t)(dl_ptr_hi - 1);
        } while (dl_ptr_hi != 0x00);
        sync_flag = (uint8_t)(sync_flag + 0x12);
        screen_ptr_hi = (uint8_t)(screen_ptr_hi + 0x0A);
        span_pixel_count = (uint8_t)(span_pixel_count - 1);
    } while (span_pixel_count != 0x00);
}

/* intro_random_setup @ $6FBF — depth-first maze/grid generation on the $0900 16x16 cell map.
 * Fill all 256 cells with RANDOM&$3F|$80 (bit7 = unvisited), seed a start cell, then DFS: at
 * each step scan_grid_neighbors pushes marked neighbours onto the $2500 stack ($0098); pick a
 * random marked direction (BIT RANDOM -> one of 4 offset pairs matching scan_grid_neighbors,
 * retried until test_marked_neighbor confirms it), carve through the wall + next cell (bit7
 * cleared), advance; when a cell has no marked neighbours, pop the stack until it empties. */
void intro_random_setup(void) {
#if defined(ROF_PLATFORM_AMIGA) && defined(ROF_FIXED_RNG)
    /* Benchmark determinism (`make FIXED_RNG=1`, OFF by default).  Our $D20A is a read-clocked
       LFSR from a fixed power-on seed, NOT the Atari's free-running one, so "which level you
       get" is decided purely by how many RANDOM reads the Logo/Station attract happened to make
       before this call.  A code change shifts the free-running main loop against the real-time
       50Hz VBI, shifts that read count, and silently hands the next build a DIFFERENT LEVEL —
       which is why cross-build beam-tick numbers were never comparable.  Re-pinning the LFSR
       here (immediately before the only fresh-start level seeding: this fn -> $70B3 -> $7498,
       which build the $0900 marker grid and $0A00 object map) makes every build generate the
       SAME level.  Amiga+probe-only: the SDL/validate oracle path is untouched. */
    extern uint32_t rof_lfsr_state;
    rof_lfsr_state = 0x1F0A5u;
#endif
    uint8_t x = 0x00, y = 0x00;
    do {                                            /* fill $0900[0..255] = RANDOM&$3F|$80 */
        mem[0x0900 + x] = (uint8_t)((bus_read(0xD20A) & 0x3F) | 0x80);
        x = (uint8_t)(x + 1);
        y = (uint8_t)(y - 1);
    } while (y != 0x00);
    dl_bottom_index = 0x01;
    cpu.A = bus_read(0xD20A); draw_x_left = cpu.A;
    mem[0x0900 + cpu.A] = (uint8_t)(bus_read(0xD20A) & 0x3F);
    for (;;) {                                      /* L_6fe3 */
        mem[0x0099] = dl_bottom_index;
        scan_grid_neighbors();
        if (dl_bottom_index == mem[0x0099]) {           /* no neighbours pushed -> backtrack */
            dl_bottom_index = (uint8_t)(dl_bottom_index - 1);
            if (dl_bottom_index == 0x00) return;
            draw_x_left = mem[0x2500 + dl_bottom_index];
            continue;
        }
        for (;;) {                                  /* L_6ff0: pick a marked direction */
            uint8_t r = bus_read(0xD20A);           /* BIT: N=bit7, V=bit6 */
            uint8_t a, b;
            int V = (r >> 6) & 1, Nf = (r >> 7) & 1;
            if (V) { if (Nf) { a = 0xF0; b = 0x01; } else { a = 0x10; b = 0xFF; } }
            else   { if (Nf) { a = 0xFF; b = 0xF0; } else { a = 0x01; b = 0x10; } }
            grid_offset_a = a; grid_offset_b = b;
            test_marked_neighbor();
            if (cpu.N) break;                       /* BPL retry inverted: proceed when marked */
        }
        uint8_t cx = (uint8_t)(draw_x_left + grid_offset_a);
        mem[0x0900 + cx] = (uint8_t)(bus_read(0xD20A) & 0x3F);
        cx = (uint8_t)(cx + grid_offset_a);
        mem[0x0900 + cx] = (uint8_t)(bus_read(0xD20A) & 0x3F);
        draw_x_left = cx;
    }
}

/* emit_dl_coord_pairs @ $68CF — emit display-list LMS coordinate pairs.  Copy $0096 (=$6E0F[Y])
 * row-addr table entries into the DL region $300A/$300B (Y descending from $00C3, X descending
 * from $00C4), then the same count into $308B/$308C (Y ascending from $00C1, X ascending from
 * $00C2).  Advance $00C3/$00C1 by +-3*$6E0F[$00B9], then tail-call the (native) plot_terrain_span. */
void emit_dl_coord_pairs(void) {
    cpu.A = mem[0x6E0F + cpu.Y];
    span_row_count = cpu.A;
    uint8_t count = cpu.A;                          /* PHA */
    uint8_t xi = row_table_base_hi, yi = row_table_base_lo;
    do {
        mem[0x300A + yi] = mem[MEM_row_base_lo + xi];
        mem[0x300B + yi] = mem[MEM_row_base_hi + xi];
        yi = (uint8_t)(yi - 3);
        xi = (uint8_t)(xi - 1);
        span_row_count = (uint8_t)(span_row_count - 1);
    } while (span_row_count != 0x00);
    row_table_base_hi = xi;
    span_row_count = count;                            /* PLA */
    xi = player_speed; yi = row_table_stride;
    do {
        mem[0x308B + yi] = mem[MEM_row_base_lo + xi];
        mem[0x308C + yi] = mem[MEM_row_base_hi + xi];
        yi = (uint8_t)(yi + 3);
        xi = (uint8_t)(xi + 1);
        span_row_count = (uint8_t)(span_row_count - 1);
    } while (span_row_count != 0x00);
    player_speed = xi;
    cpu.Y = draw_pattern_byte;
    cpu.C = 1; cpu.A = row_table_base_lo;
    SBC(mem[0x6E0F + cpu.Y]); SBC(mem[0x6E0F + cpu.Y]); SBC(mem[0x6E0F + cpu.Y]);
    row_table_base_lo = cpu.A;
    cpu.C = 0; cpu.A = row_table_stride;
    ADC(mem[0x6E0F + cpu.Y]); ADC(mem[0x6E0F + cpu.Y]); ADC(mem[0x6E0F + cpu.Y]);
    row_table_stride = cpu.A;
    plot_terrain_span();
}

/* compute_stage_display_geometry @ $75F5 — derive the throttle/speed gauge
 * parameter block from the current gauge value $006D (held in X throughout).
 * Pure-compute leaf: a chain of clamps/shifts of X writing the gauge geometry
 * cells $0617-$062A (+ $08A2, scratch $00C1), with the displayed value run
 * through the native bin_to_bcd for $0628.  No loops.  Contract: mem[] — exit
 * regs are dead at the boot_standby_launch_driver call site; the PHA/PLA byte at $01FF that
 * the 6502 leaves behind (S=$FF in the harness) is masked in validate_native.c. */
void compute_stage_display_geometry(void) {
    ROF_MEMBASE_DECL(mb);   /* 61 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t x = level_stage;

    /* $75F5-$7600: P = min((X>>1)+2, $14) */
    uint8_t p = (uint8_t)((x >> 1) + 2);
    if (p >= 0x14) p = 0x14;

    /* $7602-$7609: $062A = P; $0628 = BCD(P) via native bin_to_bcd */
    stage_geom_height_062A = p;
    cpu.A = p;
    bin_to_bcd();                       /* sets $00C1=ones, cpu.Y=tens, cpu.A=BCD */
    score_bcd = cpu.A;

    /* $760d-$7612: $061F = (P>>1)+1 */
    stage_geom_061F = (uint8_t)((p >> 1) + 1);

    /* $7615-$761d: $0620 = (P>=5) ? P-5 : 0 */
    stage_geom_0620 = (p >= 5) ? (uint8_t)(p - 5) : 0;

    /* $7620-$7632: $0623 = (X==1) ? 0 : (min(X,$2B) << 2) */
    mem[0x0623] = (x == 1) ? 0 : (uint8_t)(((x >= 0x2B) ? 0x2B : x) << 2);

    /* $7635-$7650: $0621 = $0622 = (X<4) ? 0 : $58 - 2*min(X,$22) */
    {
        uint8_t v;
        if (x < 4) {
            v = 0;
        } else {
            uint8_t c = (x >= 0x23) ? 0x22 : x;
            row_table_stride = (uint8_t)(c << 1);
            v = (uint8_t)(0x58 - row_table_stride);
        }
        mem[0x0621] = v;
        mem[0x0622] = v;
    }

    /* $7653-$7662: $0624 = ($2C - min(X,$28)) >> 1 */
    {
        uint8_t c = (x >= 0x28) ? 0x28 : x;
        row_table_stride = c;
        mem[0x0624] = (uint8_t)((0x2C - c) >> 1);
    }

    /* $7665: $00C1 = X (raw) — base for the next two derivations */
    row_table_stride = x;

    /* $7667-$7675: r=$2A-X (8-bit); if r bit7 set -> 4, else max(r>>1, 4).
     * BMI/CMP test bit7 of the 8-bit SBC result, not the true sign. */
    {
        uint8_t r = (uint8_t)(0x2A - x);
        uint8_t v;
        if (r & 0x80) {
            v = 4;
        } else {
            v = (uint8_t)(r >> 1);
            if (v < 4) v = 4;
        }
        lockon_step_reload = v;
    }

    /* $7678-$7699: r=$1A-X (8-bit) -> $061A, then clamp/scale into $061B.
     * BMI tests bit7 of the 8-bit result (skips the LSR when set). */
    {
        uint8_t r = (uint8_t)(0x1A - x);
        uint8_t a;
        if (r & 0x80) {
            a = r;                          /* bit7 set, no LSR */
        } else {
            a = (uint8_t)(r >> 1);
        }
        mem[0x061A] = a;
        if ((a & 0x80) || a < 2) a = 2;     /* $7683-$768a: clamp min 2 */
        a = (a >= 8) ? 0xFF : (uint8_t)(a << 5);   /* $768b-$7698 */
        mem[0x061B] = a;
    }

    /* $769c-$76a2: $061C = (X>>2)+5 */
    mem[0x061C] = (uint8_t)((x >> 2) + 5);

    /* $76a5-$76b2: $00C1 = X>>2; $0625 = (X>>2 <= 8) ? 8-(X>>2) : 0 */
    {
        uint8_t v = (uint8_t)(x >> 2);
        row_table_stride = v;
        mem[0x0625] = (v <= 8) ? (uint8_t)(8 - v) : 0;
    }

    /* $76b5-$76c7: X>=$10 -> $08A2 = min(X-$10,$17), $0617 = 0; else $0617 = X */
    if (x >= 0x10) {
        uint8_t d = (uint8_t)(x - 0x10);
        mem[0x08A2] = (d >= 0x18) ? 0x17 : d;
        stage_geom_0617 = 0;
    } else {
        stage_geom_0617 = x;
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* blit_label_row @ $6750 — blit a 5-glyph label row.  Index base = (mem[$0004]!=0)
 * ? 5 : 0; for the 5 codes mem[$6E23+base..base+4] call native glyph_ptr_from_index
 * (-> $E000 charset glyph -> blit_glyph_8rows) at fixed row $0092=$2E, column $009C=$1B.
 * Contract: mem[] (blit chain writes the $073D/$0793 row-addr-table bitmap; the
 * blit_glyph_8rows PHA/PLA stack scribble is masked in validate_native.c). */
void blit_label_row(void) {
    draw_row = 0x2E;
    digit_dst_ptr_lo = (level_or_state != 0) ? 0x05 : 0x00;   /* LDX $0004; BEQ -> keep, else 5 */
    draw_x_left = 0x1B;
    digit_dst_ptr_hi = 0x05;                                /* loop count */
    do {
        cpu.A = mem[0x6E23 + digit_dst_ptr_lo];
        glyph_ptr_from_index();
        digit_dst_ptr_lo++;
        digit_dst_ptr_hi--;
    } while (digit_dst_ptr_hi != 0x00);
}

/* blit_message_block @ $672D — draw the message frame: from row $0092=$54 stepping
 * up by 8 while non-negative, set the row ptr ($80/$81 = table[$0092]) and plot the
 * three frame pixels at columns $15/$2E/$47; then tail-call blit_label_row.  $0094=0
 * (the plot_pixel_masked column-page base).  Contract: mem[] (same row-table bitmap). */
void blit_message_block(void) {
    draw_row = 0x54;
    draw_color_idx = 0x00;
    do {
        set_row_ptr_from_count();
        cpu.A = 0x15; plot_pixel_masked();
        cpu.A = 0x2E; plot_pixel_masked();
        cpu.A = 0x47; plot_pixel_masked();
        draw_row = (uint8_t)(draw_row - 0x08);
    } while (!(draw_row & 0x80));                  /* BPL: loop while bit7 clear */
    blit_label_row();
    /* Amiga: spans the whole $2000 field (rows $54 down to 0) — widen the door-decode dirty
     * range without raising the flag; the trailing blit_numeric_readout raises it. */
    ROF_DOOR_FIELD_TOUCH_ALL();
}

/* draw_digit_low_nibble @ $4095 — A = (A & $0F) << 2, then tail draw_glyph_2rows
 * (the 2x2 glyph for that index).  Macros keep the N/Z/C flags identical to the
 * 6502 at the draw_glyph_2rows entry. */
void draw_digit_low_nibble(void) {
    AND(0x0F); ASL_A(); ASL_A();
    draw_glyph_2rows();
}

/* draw_2digit_value @ $4084 — render a packed-BCD byte as two 2x2 glyphs: draw the
 * high nibble ((A>>2)&$3C) at dest $BB/$BC, advance the dest pointer ($BD/$BE ->
 * $BB/$BC), then tail draw_digit_low_nibble for the low nibble.  PHA/PLA preserves
 * the entry byte across the first draw (kept in a local; the $01FF scribble is masked). */
void draw_2digit_value(void) {
    uint8_t saved = cpu.A;                 /* PHA */
    LSR_A(); LSR_A(); AND(0x3C);
    draw_glyph_2rows();
    dl_y1 = dl_y3;
    dl_y2 = dl_y4;
    LDA(saved);                            /* PLA */
    draw_digit_low_nibble();
}

/* glyph_ptr_shift3 @ $6802 — A <<= 3, then tail set_coord_y_e0 (builds the $E0xx
 * glyph source pointer from A and blits via blit_glyph_8rows). */
void glyph_ptr_shift3(void) {
    ASL_A(); ASL_A(); ASL_A();
    set_coord_y_e0();
}

/* render_bcd_top_byte @ $49C0 — entry point of the score render chain for the top
 * (most-significant) BCD byte: Y=5 (zero-suppress threshold), X=$0600 (running
 * suppress flag), then tail set_zsupp_pos_clear_delta (-> emit_bcd_byte_digits emits
 * the two nibbles of the entry-A byte). */
void render_bcd_top_byte(void) {
    LDY(0x05);
    LDX(mem[0x0600]);
    set_zsupp_pos_clear_delta();
}

/* unpack_terrain_seed_cols @ $7558 — seed the two terrain seed columns by running the
 * native rle_expand_list twice: source $4DFA -> dest $0C32, then source $4E09 -> $0D32.
 * Pure pointer setup + two native calls. */
void unpack_terrain_seed_cols(void) {
    dl_y1 = 0xFA; dl_y2 = 0x4D;   /* src $4DFA */
    dl_y3 = 0x32; dl_y4 = 0x0C;   /* dst $0C32 */
    rle_expand_list();
    dl_y1 = 0x09; dl_y2 = 0x4E;   /* src $4E09 */
    dl_y3 = 0x32; dl_y4 = 0x0D;   /* dst $0D32 */
    rle_expand_list();
}

/* game_init_7588 @ $7588 — fill $32FD..$332C (Y=$2F..0) with $AA, then decompress the
 * shape stream at $6E6E into $332D via the native rle_decompress. */
void game_init_7588(void) {
    for (uint8_t y = 0x2F; ; y--) {
        mem[0x32FD + y] = 0xAA;
        if (y == 0) break;
    }
    dl_y3 = 0x2D; dl_y4 = 0x33;   /* dst $332D */
    dl_y1 = 0x6E; dl_y2 = 0x6E;   /* src $6E6E */
    rle_decompress();
}

/* render_bcd_digits_supp_all @ $49BA — Y=7 (zero-suppress threshold), X=0 (suppress
 * flag), then set_zsupp_pos_clear_delta.  The 6502 does LDX #0 then BEQ, so the branch
 * to set_zsupp_pos_clear_delta is unconditional (the render_bcd_top_byte tail is dead). */
void render_bcd_digits_supp_all(void) {
    LDY(0x07);
    LDX(0x00);
    set_zsupp_pos_clear_delta();
}

/* blit_numeric_readout @ $67C3 — render the cockpit numeric readout.  Row $0092=$38.
 * If $0004!=0: draw 4 glyphs (codes $060F/$060D/$0610/$060E) via glyph_ptr_from_index /
 * glyph_ptr_shift3 at column $009C=$1F.  Else: clamp $006D to $63, BCD it, and draw the
 * tens ((BCD>>1)&$78 via set_coord_y_e0) and units ((BCD&$0F)<<3 via glyph_ptr_shift3) at
 * column $009C=$27.  The PHA/PLA across the tens draw is kept in a local (the $01FF
 * scribble is masked in the test). */
void blit_numeric_readout(void) {
    draw_row = 0x38;
    if (level_or_state != 0) {
        draw_x_left = 0x1F;
        cpu.A = sfx_rand_freq_hi1; glyph_ptr_from_index();
        cpu.A = sfx_rand_freq_lo1; glyph_ptr_shift3();
        cpu.A = sfx_rand_freq_hi2; glyph_ptr_from_index();
        cpu.A = sfx_rand_freq_lo2; glyph_ptr_shift3();
        ROF_DOOR_FIELD_DIRTY();
        return;
    }
    draw_x_left = 0x27;
    cpu.A = level_stage;
    if (cpu.A >= 0x63) { cpu.A = 0x63; level_stage = 0x63; }
    bin_to_bcd();                       /* A = packed BCD */
    uint8_t bcd = cpu.A;                /* PHA */
    LSR_A(); AND(0x78);                 /* tens index */
    set_coord_y_e0();
    LDA(bcd);                           /* PLA */
    AND(0x0F);                          /* units index */
    glyph_ptr_shift3();
    ROF_DOOR_FIELD_DIRTY();
}

/* dl_write_lms_window @ $69F1 — build display-list LMS coordinate words.  For X=$008B up to
 * (but excluding) $0086, copy the pair ($073D[X], $0793[X]) into ($C5/$C6)+Y with Y
 * advancing by 3 per X-step; only the first INY's wrap bumps the high byte $00C6 (the
 * faithful 6502 quirk — the trailing INY INY is unchecked).  Then if $008B!=0 shift the
 * object table up, else no-op.  Dest is RAM (DL region $30xx) so a plain mem[] store
 * matches the 6502's STA ($C5),Y (rendered bus_write, non-HW -> mem[]). */
void dl_write_lms_window(void) {
    uint8_t x = dl_src_index;
    uint8_t y = 0x00;
    do {
        uint16_t base = (uint16_t)(ROF_PAIR16(digit_dst_ptr_lo, digit_dst_ptr_hi));
        mem[(uint16_t)(base + y)] = mem[MEM_row_base_lo + x];
        y++;                                       /* INY */
        if (y == 0x00) digit_dst_ptr_hi++;              /* BNE skip; INC $C6 on wrap */
        base = (uint16_t)(ROF_PAIR16(digit_dst_ptr_lo, digit_dst_ptr_hi));
        mem[(uint16_t)(base + y)] = mem[MEM_row_base_hi + x];
        y = (uint8_t)(y + 2);                      /* INY INY (unchecked) */
        x++;                                       /* INX */
    } while (x != row_count);
    if (dl_src_index == 0x00) return;               /* ret_stub_6a26 (no-op) */
    cpu.A = dl_src_index;
    shift_object_table_up();
}

/* draw_ah_ground_fill_p2 @ $40B0 — flight HUD: the artificial-horizon ground-fill column.
 * Cache-gated on ($0026,$0027): if unchanged since last frame, no-op.  Else re-latch the
 * cache, decode a 0x15-byte column from table $4B57[X..] into P2 buffer $0E87+Y (Y walked),
 * cap with $FF.  Faithful twin of the $40B0 oracle (dl_y3 loop counter ends 0). */
void draw_ah_ground_fill_p2(void) {
    uint8_t x = ring_cur_3, y = ring_cur_4;
    if (x == canopy_pillar_x_cache && y == canopy_pillar_y_cache) return;
    canopy_pillar_x_cache = x;
    x = mem[0x455B + x];                       /* X = table[$455B + ring_cur_3] */
    canopy_pillar_y_cache = y;
    mem[0x0E87 + y] = 0x00;
    /* ⭐ Same treatment as draw_altimeter_bars: the 21-byte copy is contiguous on BOTH sides,
     * but the 6502 indices are 8-bit, so GCC masks and re-adds a base for each of the two
     * accesses every step (~2x58 cycles a byte).  Walk two pointers instead (20 cycles a byte)
     * whenever neither index wraps inside the 21 steps — which is every live frame.  The
     * wrapping case keeps the exact indexed shape (there the address is not monotonic). */
    uint8_t n = 0x15;                          /* dl_y3 loop count */
    if (y <= (uint8_t)(0xFF - 0x15) && x <= (uint8_t)(0xFF - 0x15)) {
        uint8_t *d = (uint8_t *)&mem[0x0E87 + y + 1];
        const uint8_t *s = (const uint8_t *)&mem[0x4B57 + x];
        do { *d++ = *s++; } while (--n != 0);
        y = (uint8_t)(y + 0x15); x = (uint8_t)(x + 0x15);   /* the loop's exit values */
    } else {
        do { y++; mem[0x0E87 + y] = mem[0x4B57 + x]; x++; } while (--n != 0);
    }
    dl_y3 = 0x00;                              /* oracle DEC_M($dl_y3) leaves it 0 */
    mem[0x0E88 + y] = 0xFF;
}

/* draw_altimeter_bars @ $40E5 — flight HUD altimeter: two cache-gated bar columns.
 * Terrain bar (cache $2875 vs viewport_top_row): clear $0C97+Y downward to 0, then $FF-fill
 * $0C98+Y up to col $38.  Ship bar (cache $2876 vs viewport_bottom_row): mask the 4 edge
 * bytes $0B96..$0B99 (&$3F, &$3F, |$C0, |$C0).  Faithful twin of the $40E5 oracle. */
void draw_altimeter_bars(void) {
    uint8_t y = viewport_top_row;
    if (y != altimeter_terrain_cache) {
        altimeter_terrain_cache = y;
        /* ⭐ Both runs are CONTIGUOUS, so walk a pointer instead of re-deriving mem[base+yy]
         * per byte.  The 6502 index is 8-bit, which is what forced the old shape: GCC has to
         * mask every step, and at -O3 it unrolls the fill x8 into five instructions a byte —
         *     move.b d1,d2 / addq.b #k,d2 / andi.l #255,d2 / addi.l #3224,d2 / move.b #-1,(a0,d2.l)
         * = ~58 cycles to store ONE byte, up to 56 of them, twice.  Measured 2026-08-12: the
         * five HUD draws were 5.2 t of the flight VBI's 54 and this pair is most of it.
         * A `*p-- = 0` / `*p++ = $FF` walk is 12 cycles a byte, ~5x less, byte-identical. */
        {   /* DEY;BPL clear: writes $0C97+y down to $0C97+0.  yy and p step together, so the
             * 8-bit wrap that ENDS the loop needs no mask, and 0x0C97+yy never wraps 16 bits. */
            uint8_t yy = y;
            uint8_t *p = (uint8_t *)&mem[0x0C97 + yy];
            do { *p-- = 0x00; } while (((--yy) & 0x80) == 0);
        }
        {   /* INY;CPY#$38;BCC fill (write-then-check).  ⚠ Here the 8-bit wrap is INSIDE the
             * loop: a start >= $38 writes one byte and stops, but a start of $FF wraps to $00
             * and fills $0C98..$0CCF as well — the address is NOT monotonic there.  So the
             * pointer walk takes only the non-wrapping case; the tail keeps the exact 6502
             * shape.  (Live flight only ever passes yy < $38; the fixture proves the rest.) */
            uint8_t yy = altimeter_terrain_cache;
            if (yy < 0x38) {
                uint8_t *p = (uint8_t *)&mem[0x0C98 + yy];
                do { *p++ = 0xFF; } while (++yy < 0x38);
            } else {
                do { mem[0x0C98 + yy] = 0xFF; yy++; } while (yy < 0x38);
            }
        }
    }
    y = viewport_bottom_row;
    if (y != altimeter_ship_cache) {
        altimeter_ship_cache = y;
        mem[0x0B96 + y] &= 0x3F;
        mem[0x0B97 + y] &= 0x3F;
        mem[0x0B98 + y] |= 0xC0;
        mem[0x0B99 + y] |= 0xC0;
    }
}

/* dispatch_43cb_half_70 @ $43C7 — flight HUD: Y = terrain_clearance>>1, tail the native
 * draw_dial_bar_column (which gates on Y).  Faithful twin of the $43C7 oracle. */
void dispatch_43cb_half_70(void) {
    cpu.A = cpu.Y = (uint8_t)(terrain_clearance >> 1);   /* LDA;LSR;TAY (A also = clearance>>1) */
    draw_dial_bar_column();
}

/* update_altitude_digit_display @ $44D6 — flight HUD altitude readout.
 * Clamp a digit index (0x1C-altimeter_alt_ref, else 0x1E), and if it changed vs the previous
 * (draw_pattern_byte) rewrite 3 glyph cells $0B91+ (&$CF old, |$30 new).  Then derive the
 * altitude colour/glyph ($062x+$AB, clamped to $B5 outside [$A3,$B4)).  Faithful $44D6 twin. */
void update_altitude_digit_display(void) {
    uint8_t oldP = draw_pattern_byte;
    uint8_t a = (uint8_t)(0x1C - altimeter_alt_ref);     /* SEC;SBC */
    if ((a & 0x80) || a >= 0x1D) a = 0x1E;               /* BMI / CMP#$1D BCS -> 0x1E */
    draw_pattern_byte = a;
    if (oldP != a) {
        uint8_t y = oldP;
        for (uint8_t k = 3; k != 0; k--) { mem[0x0B91 + y] &= 0xCF; y++; }
        y = a;
        for (uint8_t k = 3; k != 0; k--) { mem[0x0B91 + y] |= 0x30; y++; }
    }
    uint8_t c = (uint8_t)(altimeter_color_base + 0xAB);  /* CLC;ADC#$AB */
    if (c < 0xA3 || c >= 0xB4) c = 0xB5;                 /* keep only if $A3<=c<$B4 */
    altitude_color_or_glyph = c;
}

/* draw_dial_bar_column @ $43CB — update one cockpit dial-bar column to value Y.
 * No-op if (Y>=9 && $062E==8) or Y already == $062E; otherwise latch $062E=Y, set the
 * bar params ($00BF=Y threshold, $00BE=$FF loop end, $00BD=$07 start) and draw via the
 * native draw_object_column (entry A=$07). */
void draw_dial_bar_column(void) {
    uint8_t y = cpu.Y;
    if (y >= 0x09 && mem[0x062E] == 0x08) return;
    if (y == mem[0x062E]) return;
    mem[0x062E] = y;
    bar_col_threshold = y;
    dl_y4 = 0xFF;
    dl_y3 = 0x07;
    cpu.A = 0x07;
    draw_object_column();
}

/* draw_player3_object @ $42A7 — render the player-3 "lock-on" indicator sprite.  Entry A
 * selects the object: A>=3 uses a fixed config (HPOSP3=0, size $0C, col $6C); A<3 looks up
 * HPOS/size from $4566/$4569 and derives the column/size index from $2824 via $456C/$457A.
 * Then (if $2835!=0) clears a strip of the PMG buffers $0F1E/$0F71, and finally — for the
 * original entry A in $03..$1F — computes a screen position and blits the sprite mask
 * (bytes via the $4D3E/$4D3F pointer; RANDOM-dithered when $2826==0) into $0F1E/$0F71,
 * writing HPOSP3 ($D003) and the size clamp $00CC.
 *
 * Faithful transliteration.  HW touched via bus_read/bus_write ($D00B HPOSP3 hi?, $D003,
 * $D20A RANDOM — seeded identically in both runs).  Indexed RAM stores ($0F1E/$0F71,Y/X)
 * are plain mem[].  GOTCHA reproduced: the SBC #$55 at $42D3 inherits its borrow from
 * which CMP #$8B branch was taken (the $8C clamp path carries C=1, the fall-through C=0).
 * Contract: mem[] (exit regs dead); the entry PHA/PLA scribble at $01FF is masked. */
void draw_player3_object(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t entryA = cpu.A;                 /* PHA */
    uint8_t a, x, y = 0;

    if (cpu.A >= 0x03) {                     /* CMP #3; BCC L_42bc */
        bus_write(0xD00B, 0x00);
        player3_vsize = 0x0C;
        x = 0xFF;
        a = 0x6C;                            /* BNE L_42ef (always) */
    } else {
        /* L_42bc: A in {0,1,2} */
        y = cpu.A;                           /* TAY */
        bus_write(0xD00B, mem[0x4566 + y]);
        player3_vsize = mem[0x4569 + y];
        /* SEC; LDA $2824; CMP #$8B; (>= -> A=$8C,C=1) (< -> A=$2824,C=0); SBC #$55 */
        uint8_t v = player3_ytop, carry;
        if (v >= 0x8B) { a = 0x8C; carry = 1; }
        else           { a = v;    carry = 0; }
        a = (uint8_t)((int)a - 0x55 - (1 - carry));
        if (a & 0x80) a = 0x00;              /* BPL skip; else LDA #0 */
        a >>= 2;                             /* LSR; LSR */
        if (y == 0x00) {                     /* CPY #1; BMI L_42e9 (Y==0) */
            y = a; x = mem[0x456C + y]; a = 0x54;
        } else {
            a >>= 1;                         /* LSR */
            y = a; x = mem[0x457A + y]; a = 0x5E;
        }
    }
    /* L_42ef */
    player3_shape_mask = x;
    bar_col_threshold = a;
    x = player3_prev_rows;
    if (x != 0x00) {                         /* clear strip */
        y = player3_draw_y;
        if (y < 0x14) y = 0x14;
        do {
            mem[0x0F1E + y] = 0x00;
            if (y < 0x4F) mem[0x0F71 + y] = 0x00;
            y++;
            x--;
        } while (x != 0x00);
    }
    /* L_4312: PLA */
    a = entryA;
    if (a & 0x80) { mem[0x006A]--; return; } /* BPL L_4318; else DEC $006A */
    if (a >= 0x20) return;                    /* CMP #$20; BCS return */
    if (a >= 0x15) {
        y = 0x49;
    } else if (a >= 0x0C) {
        y = 0x48;
    } else {
        uint8_t chainval;
        if (a >= 9)      chainval = 7;
        else if (a >= 7) chainval = 6;
        else if (a >= 5) chainval = 5;
        else             chainval = a;       /* a in 0..4 */
        y = (uint8_t)(player3_xbase + 9 * chainval);
    }
    /* L_4352 */
    a = player3_xpos;
    if (a < 0x07) a = 0x07;
    if (a >= 0x71) a = 0x71;
    if (y < 0x2D) a = (uint8_t)(a - mem[0x4D11 + y]);   /* CPY #$2D; BCS skip; SEC; SBC */
    player3_draw_y = a;
    x = a;                                   /* TAX */
    y = (uint8_t)(y << 1);                   /* TYA; ASL; TAY */
    dl_y1 = mem[0x4D3E + y];
    dl_y2 = mem[0x4D3F + y];
    y = 0x00;
    for (;;) {                               /* L_437c */
        uint8_t aa = (player3_dither_flag != 0) ? player3_dither_flag : bus_read(0xD20A);
        uint16_t bb = ROF_PAIR16(dl_y1, dl_y2);
        aa &= bus_read((uint16_t)(bb + y));
        if (aa == 0x00) break;               /* BEQ L_43a5 */
        if (x >= 0x14) {
            if (x < 0x73) mem[0x0F1E + x] = aa;
            y++;                             /* INY (only when x>=$14) */
            if (x >= 0x2E && x < 0x4F) {
                aa &= player3_shape_mask;
                mem[0x0F71 + x] = aa;
            }
        }
        x++;                                 /* INX */
        if (x & 0x80) break;                 /* BPL L_437c (loop while x<$80) */
    }
    /* L_43a5 */
    player3_prev_rows = y;
    {
        uint8_t s = (uint8_t)(player3_vsize + player3_ytop);   /* CLC; ADC */
        player3_hpos = s;
        bus_write(0xD003, s);
        if (s >= 0x8E) s = 0x8D;             /* CMP #$8E; BCC L_43be; LDA #$8D */
        else if (s < bar_col_threshold) s = bar_col_threshold;   /* CMP $BF; BCS skip; LDA $BF */
        player3_bottom_y = s;
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* dl_rebuild_lms_window @ $69E5 — set the DL-fill dest pointer ($C5/$C6=$300A) and end index
 * ($0086=$56), then tail dl_write_lms_window. */
void dl_rebuild_lms_window(void) {
    digit_dst_ptr_lo = 0x0A; digit_dst_ptr_hi = 0x30;
    row_count = 0x56;
    dl_write_lms_window();
}

/* game_init_76CB @ $76CB — build the flight-mode display list and LMS tables in $30xx-$32xx.
 * Copies header bytes from $77C3/$77C9/$77D2 into the DL mirrors, fills mode lines/blanks,
 * pokes the fixed LMS/scroll constants, then builds two row-address tables (stride $60 from
 * $1070 / $10A0) and emits them into the DL via dl_write_lms_window.  All-native callees, all writes
 * land in safe RAM. */
void game_init_76CB(void) {
    ROF_MEMBASE_DECL(mb);   /* 182 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t a, y;
    for (y = 0x08; ; y--) {                      /* L_76cd */
        a = mem[0x77C9 + y];
        mem[0x3000 + y] = a; mem[0x3120 + y] = a; mem[0x316B + y] = a; mem[0x3210 + y] = a;
        a = mem[0x77C3 + y];
        mem[0x310B + y] = a; mem[0x3156 + y] = a;
        if (y == 0) break;
    }
    for (y = 0x0E; ; y--) {                      /* L_76ea */
        a = mem[0x77D2 + y];
        mem[0x3111 + y] = a; mem[0x315C + y] = a; mem[0x3201 + y] = a; mem[0x32A6 + y] = a;
        if (y == 0) break;
    }
    y = 0x87;                                    /* L_7700 */
    for (;;) {
        mem[0x3174 + y] = 0x4D; mem[0x3219 + y] = 0x4D;
        y--;
        if (y == 0xFF) break;
    }
    mem[0x3129] = 0x4D;
    mem[0x312A] = 0x00;
    mem[0x312B] = 0x10;
    y = 0x7E;                                    /* L_771c (step -3) */
    for (;;) {
        mem[0x3009 + y] = 0x4F; mem[0x308A + y] = 0x4F;
        y = (uint8_t)(y - 3);
        if (y & 0x80) break;
    }
    for (y = 0x28; ; y--) {                      /* L_772b */
        mem[0x312C + y] = 0x0D;
        if (y == 0) break;
    }
    mem[0x306F] = 0xCF; mem[0x30ED] = 0xCF; mem[0x30F9] = 0xCF; mem[0x3105] = 0xCF;
    mem[0x3008] = 0x8D; mem[0x3128] = 0x8D; mem[0x313C] = 0x8D; mem[0x3151] = 0x8D; mem[0x3155] = 0x8D;
    mem[0x3007] = 0x84;
    mem[0x31F2] = 0xCD; mem[0x31FE] = 0xCD; mem[0x3297] = 0xCD; mem[0x32A3] = 0xCD;
    mem[0x311E] = 0x00; mem[0x311F] = 0x30;
    mem[0x3169] = 0x20; mem[0x316A] = 0x31;
    mem[0x320E] = 0x6B; mem[0x320F] = 0x31;
    mem[0x32B3] = 0x10; mem[0x32B4] = 0x32;
    row_table_stride = 0x60;
    row_table_base_lo = 0x70; row_table_base_hi = 0x10;      /* base $1070 */
    build_row_addr_table();
    digit_dst_ptr_lo = 0x75; digit_dst_ptr_hi = 0x31;      /* dest $3175 */
    dl_src_index = 0x00; row_count = 0x2F;
    dl_write_lms_window();
    row_table_base_lo = 0xA0; row_table_base_hi = 0x10;      /* base $10A0 */
    build_row_addr_table();
    digit_dst_ptr_lo = 0x1A; digit_dst_ptr_hi = 0x32;      /* dest $321A */
    dl_write_lms_window();
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* setup_initials_ptr @ $5A63 — point the digit dest at $3694, render $006D as zero-suppressed
 * BCD ($3694=0 clears the running suppress flag), via bin_to_bcd + render_bcd_digits_supp_all. */
void setup_initials_ptr(void) {
    digit_dst_ptr_lo = 0x94; digit_dst_ptr_hi = 0x36;
    cpu.A = level_stage;
    bin_to_bcd();
    cpu.Y = 0x00;
    mem[0x3694] = 0x00;
    render_bcd_digits_supp_all();
    /* Title Screen (scene 3b) level select: this renders the STARTING LEVEL digit into
       $3694/$3695 — the tens cell is cleared directly (above) rather than via
       plot_char_bounded, so flag both cells explicitly so the Amiga redraws them (a
       decreasing level must blank the old tens digit).  No-op on SDL / validate headless. */
    platform_title_screen_dirty(0x3694, 2u);
}

/* startup_init @ $3FFA — refresh the level/score HUD digits when their source values
 * change.  When $0642 is 1 or 2 and ($0642 & $004B)==0, optionally push event $14 (if
 * $0072!=0) and select the $9E vs $1E indicator base.  Then: redraw the level digit
 * ($0647, into $33B4) if changed; redraw the 2-digit field ($0641, into $3413/$3445) if
 * changed; and redraw the score field ($0628 | $00BF, into $3472/$34A4) if changed —
 * $00BF gets bit7 when $062B!=0 and ($0C & $004B)==0.
 *
 * The PHA/ring_push_marked/PLA bracket is reproduced with the real op-macros + native
 * ring_push_marked so the CPU stack page matches the oracle exactly (ring_push's internal
 * PLA reads the byte this PHA pushed — eliminating the PHA would diverge mem[$01FF] and the
 * pulled index).  Digit dests are fixed safe screen RAM.  Contract: mem[].
 *
 * ⚠ This runs in the flight VBI and shares the $0645/$0646/$0647 change caches with the Amiga's
 * startup_init_native (perFrameWork), so it is usually the one that CONSUMES a digit change — and
 * it writes the glyph cells through draw_glyph_2rows, which raises no dirty flag.  On the Amiga
 * those cells only reach the display through the cockpit decode, so an unflagged write showed a
 * STALE digit (user-reported: the pilot-range readout #17 sometimes kept an old value) until some
 * unrelated writer happened to repaint the block.  digit_block_dirty() closes that: each rewritten
 * 2x2 block registers its four cells (two DL rows) for re-decode.  No-op off Amiga. */
static inline void digit_block_dirty(uint16_t dest) {
    platform_cockpit_dirty(dest, 2);
    platform_cockpit_dirty((uint16_t)(dest + 0x30), 2);
}

void startup_init(void) {
    ROF_MEMBASE_DECL(mb);   /* 44 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    bar_col_threshold = 0x00;
    cpu.Y = 0x1E;
    LDA(range_to_pilot);
    uint8_t v = cpu.A;
    int skip = (v < 0x01) || (v >= 0x03) || ((v & collision_flags) != 0);
    if (!skip) {
        PHA();
        if (flight_mode_state != 0x00) {            /* LDA $0072; CMP #0; BEQ skips ring_push */
            cpu.X = 0x14;
            ring_push_marked();
        }
        PLA();
        cpu.Y = 0x9E;
    }
    /* L_401e */
    mem[0x33DF] = cpu.Y;
    cpu.Y = (uint8_t)(cpu.Y - 1);             /* DEY */
    mem[0x33E0] = cpu.Y;
    if (cpu.A != digit_cache_647) {                /* CMP $0647; BEQ L_4038 */
        dl_y1 = 0xB4; dl_y2 = 0x33;
        digit_cache_647 = cpu.A;
        draw_digit_low_nibble();
        digit_block_dirty(0x33B4);                 /* #17 Range To Pilot */
    }
    /* L_4038 */
    LDA(placed_item_count_bcd);
    if (cpu.A != shield_or_damage) {                /* CMP $0645; BEQ L_4056 */
        shield_or_damage = cpu.A;
        dl_y3 = 0x45; dl_y4 = 0x34;
        dl_y1 = 0x13; dl_y2 = 0x34;
        draw_2digit_value();
        digit_block_dirty(0x3413); digit_block_dirty(0x3445);   /* #18 Enemies Destroyed */
    }
    /* L_4056 */
    if (bcd_osc_dir != 0x00) {
        if ((0x0C & collision_flags) == 0) bar_col_threshold = 0x80;   /* LDA #$0C; BIT $004B; BNE skips */
    }
    /* L_4065 */
    LDA(score_bcd);
    cpu.Y = cpu.A;                             /* TAY */
    ORA(bar_col_threshold);
    if (cpu.A == digit_cache_646) return;          /* CMP $0646; BEQ return_stub_40af */
    digit_cache_646 = cpu.A;
    cpu.A = cpu.Y;                             /* TYA (the raw $0628 value) */
    dl_y3 = 0xA4; dl_y4 = 0x34;
    dl_y1 = 0x72; dl_y2 = 0x34;
    draw_2digit_value();
    digit_block_dirty(0x3472); digit_block_dirty(0x34A4);       /* #19 Pilot Quota/Rescued */
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* dl_lms_scroll_step @ $69E3 — DEC $008B, then tail dl_rebuild_lms_window (rebuild the DL from the
 * decremented index). */
void dl_lms_scroll_step(void) {
    dl_src_index = (uint8_t)(dl_src_index - 1);
    dl_rebuild_lms_window();
}

/* dl_lms_reset_window @ $69DD — reset $008B=0, then dl_rebuild_lms_window.  The 6502 does LDA #0
 * then BEQ, so the branch is unconditional (the dl_lms_scroll_step tail is dead). */
void dl_lms_reset_window(void) {
    dl_src_index = 0x00;
    dl_rebuild_lms_window();
}

/* draw_scaled_shape @ $7C9A — scale and blit a 2-bit shape into the HUD via the clipped
 * plotter.  First a divide-by-repeated-subtraction: $C2:$C1=$0600, subtract the step
 * $0051:$0050 until it borrows, decrementing $004F each pass; the leftover $004F (->$28DE)
 * is the per-row inner count reload.  Then 14 blank plots, and the nested row(outer,$0055
 * accum to $12)/col(inner,$0053 accum to $0C) loop: each cell derives a mask-byte offset via
 * the $7DA9/$7DBB/$7DD3 tables + the ($C3) mask pointer, extracts a 2-bit field (X from
 * $7DD3 selects how many >>2 steps — DEX/BMI: 0 for X==0 or X>=$81, else min(X,3)), maps it
 * through $7DA5 and plots it.  plot_clipped_pixel is native (clips OOB).  HW-free except the
 * mask read through ($C3) routed via bus_read.  Step must be nonzero or the loops never end. */
void draw_scaled_shape(void) {
    ROF_MEMBASE_DECL(mb);   /* 48 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    ROF_ALIEN_DRAWSHAPE();   /* diag: count zoom-shape draws + latch shape ptr during the alien attack */
    ROF_CLEAR_FIG();      /* Amiga: reset the rescue-figure overlay for this frame's shape (no-op on SDL) */
    player_speed = 0x06;
    row_table_stride = 0x00;
    {
        uint8_t carry = 1;                              /* SEC */
        do {
            int r = (int)row_table_stride - plot_step_lo - (1 - carry);
            row_table_stride = (uint8_t)r; carry = (r >= 0);
            r = (int)player_speed - plot_step_hi - (1 - carry);
            player_speed = (uint8_t)r; carry = (r >= 0);
            terrain_pt_coord_a = (uint8_t)(terrain_pt_coord_a - 1);   /* DEC (carry unaffected) */
        } while (carry);
    }
    shape_row_width = terrain_pt_coord_a;

    mem[0x0054] = 0x00; mem[0x0055] = 0x00;
    mem[0x0053] = 0x0E;
    do {                                                /* L_7cc2: 14 blank plots */
        cpu.A = 0x00; plot_clipped_pixel();
        mem[0x0053] = (uint8_t)(mem[0x0053] - 1);
    } while (mem[0x0053] != 0x00);
    terrain_pt_coord_b = (uint8_t)(terrain_pt_coord_b - 1);

    do {                                                /* L_7ccd (rows) */
        terrain_pt_coord_a = shape_row_width;
        mem[0x0052] = 0x00; mem[0x0053] = 0x00;
        cpu.A = 0x00; plot_clipped_pixel();
        do {                                            /* L_7cdb (cols) */
            uint8_t y = mem[0x0055];
            unsigned t = (unsigned)mem[0x0053] + shape_col_base;   /* CLC; ADC $28DF */
            uint8_t x = (uint8_t)t;
            unsigned t2 = (unsigned)mem[0x7DA9 + y] + mem[0x7DBB + x] + (t > 0xFF ? 1 : 0);
            y = (uint8_t)t2;
            x = mem[0x7DD3 + x];
            uint16_t c3 = (uint16_t)(row_table_base_lo | (row_table_base_hi << 8));
            uint8_t a = bus_read((uint16_t)(c3 + y));
            for (int i = 0; i < 3; i++) {               /* DEX; BMI; LSR;LSR */
                x = (uint8_t)(x - 1);
                if (x & 0x80) break;
                a = (uint8_t)(a >> 2);
            }
            a &= 0x03;
            cpu.A = mem[0x7DA5 + a];
            plot_clipped_pixel();
            unsigned s = (unsigned)mem[0x0052] + plot_step_lo;
            mem[0x0052] = (uint8_t)s;
            mem[0x0053] = (uint8_t)((unsigned)mem[0x0053] + plot_step_hi + (s > 0xFF ? 1 : 0));
        } while (mem[0x0053] < 0x0C);
        cpu.A = 0x00; plot_clipped_pixel();
        cpu.A = 0x00; plot_clipped_pixel();
        terrain_pt_coord_b = (uint8_t)(terrain_pt_coord_b - 1);
        unsigned u = (unsigned)mem[0x0054] + plot_step_lo;
        mem[0x0054] = (uint8_t)u;
        mem[0x0055] = (uint8_t)((unsigned)mem[0x0055] + plot_step_hi + (u > 0xFF ? 1 : 0));
    } while (mem[0x0055] < 0x12);
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* reorder_cell_bits @ $8181 — reorder a source byte into a mode-D cell byte and fold
 * it into the running accumulator ($0084).  The Atari does this with a 22-op ROL-A / ROR-$84 /
 * ROR-A carry chain; that chain is exactly equivalent to a fixed bit permutation (proven over
 * all 2^17 inputs):
 *   - the result byte = the source byte with its four 2-bit groups placed in REVERSE order
 *     (bits[1:0]->[7:6], [3:2]->[5:4], [5:4]->[3:2], [7:6]->[1:0]);
 *   - the accumulator becomes (result << 1) | oldbit7, i.e. it retains only bit7 of its prior
 *     value and shifts the fresh result in above it;
 *   - the 6502 carry ends up holding that prior bit7.
 * The entry carry and the accumulator's low 7 bits do NOT affect the output.  Returns the cell
 * byte in A.  (Was a faithful ROL/ROR macro chain — replaced with the equivalent permutation,
 * which drops all the $0084 bus traffic and lets callers keep the accumulator in a register.) */
static uint8_t reorder_cell_bits_core(uint8_t src, uint8_t *accum) {
    uint8_t cell = (uint8_t)(((src & 3) << 6) | (((src >> 2) & 3) << 4)
                           | (((src >> 4) & 3) << 2) | ((src >> 6) & 3));
    *accum = (uint8_t)((cell << 1) | (*accum >> 7));
    return cell;
}

void reorder_cell_bits(void) {
    uint8_t accum = screen_ptr_hi;               /* $0084 */
    uint8_t prev7 = (uint8_t)(accum >> 7);
    cpu.A = reorder_cell_bits_core(cpu.A, &accum);
    screen_ptr_hi = accum;
    cpu.C = prev7;                                /* faithful: final carry = prior $0084 bit7 */
    cpu.N = (uint8_t)(cpu.A >> 7); cpu.Z = (cpu.A == 0);
}

/* read_console_trig_delta @ $5A78 — A = (CONSOL & 1) - TRIG0, reading the two active-low HW
 * inputs through bus_read ($D01F START bit, $D010 trigger 0). */
void read_console_trig_delta(void) {
    LDA(bus_read(0xD01F));
    AND(0x01);
    SEC();
    SBC(bus_read(0xD010));
}

/* validate_save_state @ $5D0D — verify the saved-state header: $3700==$28, $3714==$EE, and
 * the 38 bytes $37C7+$26..+1 match the reference $7BDA+$26..+1.  Sets the Z flag (valid =>
 * Z set from the final DEY; any mismatch => Z clear from the failing CMP).  No memory writes. */
void validate_save_state(void) {
    LDA(mem[0x3700]); CMP(0x28);
    if (!cpu.Z) return;
    LDA(mem[0x3714]); CMP(0xEE);
    if (!cpu.Z) return;
    cpu.Y = 0x26;
    for (;;) {
        LDA(mem[0x7BDA + cpu.Y]); CMP(mem[0x37C7 + cpu.Y]);
        if (!cpu.Z) return;
        DEY();
        if (cpu.Z) return;          /* BNE: loop while Y!=0 */
    }
}

/* cockpit_dial_update @ $4430 — set a cockpit dial/gauge to value `v` and redraw its bar.
 *   - Stores `v` as the live dial value (dial_value, $006F).
 *   - Derives the bar's fill index (dial_draw_index, $0022): 0 for the "empty" sentinel
 *     v==8, otherwise a lookup in the dial bar-value table ($4457) indexed by v plus the
 *     dial's base offset ($0625).  (dial_draw_index is a side output consumed by other dial
 *     code; the bar itself is redrawn from `v`, not from dial_draw_index.)
 *   - Redraws the dial bar for value `v` via draw_cockpit_dial_bar.
 * ($4457 = dial bar-value table, $0625 = dial base offset — both still unnamed, see
 * docs/rename.md.)  No hardware writes here — dial_value/dial_draw_index are plain RAM. */
static void cockpit_dial_update_core(uint8_t v) {
    dial_value = v;
    dial_draw_index = (v == 8) ? 0
                    : mem[0x4457 + (uint8_t)(v + mem[0x0625])];
    cpu.A = v;                    /* draw_cockpit_dial_bar is a 6502-ABI twin: value in A */
    draw_cockpit_dial_bar();
}

void cockpit_dial_update(void) { cockpit_dial_update_core(cpu.A); }

/* alien_field0_fill @ $8105 — advance HUD text field 0.  If its source cursor ($0080) has
 * reached the field limit ($2927) just bump the cursor.  Otherwise pack the next 5 source
 * bytes (from the field-0 pointer $0085/$0086, indexed by the cursor) into cells $93..$8F
 * (high cell first) via reorder_cell_bits — threading the shared accumulator $0084 —
 * and advance the cursor by 5.  ($0085/$0086 are the "encounter_count/row_count" cells
 * reused here as a text-source pointer — see docs/rename.md.) */
void alien_field0_fill(void) {
    uint8_t cursor = sync_flag;                                  /* $0080 field-0 cursor */
    if (cursor >= alien_field0_limit) { sync_flag = (uint8_t)(cursor + 1); return; }  /* $2927 */
    uint16_t src  = (uint16_t)(encounter_count | (row_count << 8));  /* $0085/$0086 field-0 ptr */
    uint8_t  accum = screen_ptr_hi;                              /* $0084 packing accumulator */
    for (uint8_t cell = 0x93; ; cell--) {                        /* cells $93..$8F, high first */
        mem[cell] = reorder_cell_bits_core(bus_read((uint16_t)(src + cursor++)), &accum);
        screen_ptr_hi = accum;   /* publish each step: a source read aliasing $0084 must see it live */
        if (cell == 0x8F) break;
    }
    sync_flag = cursor;
}

/* alien_field2_fill @ $8138 — advance HUD text field 2.  If its source cursor ($0082) has
 * reached the field limit ($2929) just bump the cursor.  Otherwise consume 7 source bytes
 * (from the field-2 pointer $0089/$008A, indexed by the cursor) into cells $94..$9A:
 *   - when the pack flag $292D==0 they are copied verbatim, ascending ($94..$9A);
 *   - otherwise each is packed via reorder_cell_bits and written descending ($9A..$94),
 *     threading the shared accumulator $0084.
 * Advances the cursor by 7 either way.  ($0089/$008A are the "terrain_state/terrain_scroll_
 * counter" cells reused here as a text-source pointer — see docs/rename.md.) */
void alien_field2_fill(void) {
    uint8_t cursor = dl_ptr_hi;                                  /* $0082 field-2 cursor */
    if (cursor >= alien_field2_limit) { dl_ptr_hi = (uint8_t)(cursor + 1); return; }  /* $2929 */
    uint16_t src = (uint16_t)(terrain_state | (terrain_scroll_counter << 8));  /* $0089/$008A ptr */
    if (mem[0x292D] == 0x00) {                                   /* verbatim copy, ascending */
        for (uint8_t cell = 0x94; cell <= 0x9A; cell++)
            mem[cell] = bus_read((uint16_t)(src + cursor++));
    } else {                                                     /* packed, high cell first */
        uint8_t accum = screen_ptr_hi;                          /* $0084 */
        for (uint8_t cell = 0x9A; ; cell--) {
            mem[cell] = reorder_cell_bits_core(bus_read((uint16_t)(src + cursor++)), &accum);
            screen_ptr_hi = accum;   /* publish each step: a source read aliasing $0084 stays faithful */
            if (cell == 0x94) break;
        }
    }
    dl_ptr_hi = cursor;
}

/* alien_shape_blit @ $80C5 — compose ONE 17-cell row of the animated overlay into the
 * mode-D viewport field.  (Despite the "alien_shape_blit" name this is the alien-creature /
 * overlay row blitter — see docs/rename.md.)  Steps:
 *   1. clear the 17-byte cell buffer $8F..$9F;
 *   2. refill it from the four field sources (alien_field0_fill..3);
 *   3. for each cell, expand it through the $BE00 shape/bit table, AND it with the matching
 *      byte of the SOURCE row ($8B/$8C indirect = the mask), OR the raw cell back in, and store
 *      the result into the DEST row ($8D/$8E indirect, which is the source row + $30);
 *   4. advance the source pointer $8B/$8C by one field row ($60) and set the dest pointer
 *      $8D/$8E = $8B/$8C + $30 for the next call.
 * The mask/dest are true indirect accesses (kept on bus_read/bus_write so the validation oracle
 * stays byte-identical even when the harness aims the pointer at hardware space); the cell buffer
 * and $0084 are always zero-page, so they use direct mem[].  The row-pointer reconstruction is
 * hoisted out of the inner loop (it is loop-invariant) — the reason this rewrite is faster. */
#ifdef ROF_PLATFORM_AMIGA
/* --- The overlay-mirror WINDOW for one creature row (alien_shape_blit, below) ---------------
 * The mirror used to re-derive, for each of the 17 cells, the wrap (`b >= 96`), two unsigned
 * range tests, two row-table lookups, the three overlay base pointers and four extent compares
 * — ~40 instructions per cell, 731 cells per knock step, all but a handful of them recomputing
 * the same thing.  They ARE the same thing: the drawn cells always form ONE CONTIGUOUS y-window
 * on ONE row, so all of it hoists out of the loop and the per-cell cost collapses to two table
 * reads, three stores and a pair of compares.
 *
 * Why one window: b = figB0 + y with y in [0,16] and figB0 in (-96,96), so at most one 96-wrap.
 *   figB0 in [0,39]  -> b in [figB0, figB0+16], never wraps; drawn while b <= 39.
 *   figB0 in [40,79] -> b in [40,95]: past the 40-byte bitmap but short of the wrap => NOTHING.
 *   figB0 in [80,95] -> wraps to b-96 in [0,15] once y >= 96-figB0; the unwrapped part is >= 40
 *                       so it draws nothing, and the wrapped part always does.
 *   figB0 < 0 (dstRow below the field base) -> drawn once y >= -figB0.
 * The [40,79] hole is what makes the two cases mutually exclusive, i.e. the row constant.
 *
 * PROVEN EXHAUSTIVELY ON THE HOST — tools/alien_mirror_test.c walks all 65536 dstRow values x
 * all 17 y and compares (drawn?, row, col) against the original per-cell form: 1114112 cases,
 * 0 mismatches, and the window is contiguous with a constant row in every one.  (The method is
 * [[rasterizer-restructure]]'s: shape the algebra, prove it off-target, then write the code.) */
struct alien_mirror_win {
    int lo, hi;                 /* y-window that draws; lo > hi means "this row draws nothing" */
    int row, off;               /* the constant overlay row; byte column = y + off */
    uint8_t *mask, *p1, *p2;    /* pre-offset so [y] addresses the slot directly ((d8,An), no index) */
};

static void alien_mirror_window(uint16_t dstRow, struct alien_mirror_win *w) {
    int figBase = (int)dstRow - 0x10A4;
    int figR0 = rof_divs16(figBase, 96), figB0 = rof_mods16(figBase, 96);
    if (figB0 >= 80) {                       /* the wrapped group */
        w->row = figR0 + 1; w->off = figB0 - 96;
        w->lo  = 96 - figB0; w->hi = 16;
    } else {                                 /* unwrapped; empty for figB0 in [40,79] (hi < lo) */
        w->row = figR0; w->off = figB0;
        w->lo  = (figB0 < 0) ? -figB0 : 0;
        w->hi  = 39 - figB0;
        if (w->hi > 16) w->hi = 16;
    }
    if ((unsigned)w->row >= 43u) { w->lo = 1; w->hi = 0; }   /* row off-bitmap => draw nothing */
    if (w->lo <= w->hi) {
        /* Bases carry `off`, which is negative when the row starts below the field base; the
         * window guarantees every y actually used lands back inside the buffer. */
        w->mask = g_figM  + kRow40[w->row] + w->off;
        w->p1   = g_figP1 + kRow80[w->row] + w->off;
        w->p2   = g_figP2 + kRow80[w->row] + w->off;
    } else {
        w->mask = w->p1 = w->p2 = (uint8_t *)0;
    }
}

/* --- PASS 2 of the row: mirror the window, then publish its dirty extents once ---------------
 * Split out of the 17-cell loop deliberately.  Interleaved, the mirror shared registers with the
 * faithful cell work (mem base, cell index, the mask/dest row pointers, the $8B alias test) and
 * GCC ran out: it spilled the three overlay pointers to the stack and re-`lea`d both decode
 * tables on EVERY cell, 14-20 cycles an access.  Alone in its own function nothing else is live,
 * so the four walks become `(a0)+` post-increments against a precomputed end — CLAUDE.md's
 * "the ADDRESSING, not the work" recipe, whose exit test must be a POINTER COMPARE or ivopts
 * folds the pointers back into one index register plus invariant bases.
 *
 * `vals` holds pass 1's 17 computed cell values (vals[y]); yLo..yHi is the y span pass 1 covered
 * while this window was live — the whole row, unless the never-taken self-modify branch swapped
 * windows mid-loop, which flushes the segment before re-windowing.  Reordering the overlay writes
 * after the faithful ones is safe: the overlay is chip RAM outside mem[], nothing reads it until
 * renderFlightDirect composites, and each y hits a distinct slot. */
static void alien_mirror_flush(const struct alien_mirror_win *w, const uint8_t *vals,
                               int yLo, int yHi) {
    int lo = w->lo > yLo ? w->lo : yLo;
    int hi = w->hi < yHi ? w->hi : yHi;
    if (lo > hi) return;                                  /* nothing drawn under this window */
    {
        const uint8_t *v    = vals + lo;
        const uint8_t *vEnd = vals + hi + 1;              /* pointer compare, not a counter */
        uint8_t *m  = w->mask + lo;
        uint8_t *q1 = w->p1   + lo;
        uint8_t *q2 = w->p2   + lo;
        do {
            /* Opaque ONLY where the creature pixel is nonzero (silhouette), so combineWithMask
             * lets the frozen terrain (s_cleanBmp) show through the value-0 holes. */
            uint8_t s = *v++;
            uint8_t a = kModeDP1[s], b = kModeDP2[s];
            *m++  = (uint8_t)(a | b);
            *q1++ = a;
            *q2++ = b;
        } while (v != vEnd);
    }
    if (w->row < g_figRowLo) g_figRowLo = w->row;
    if (w->row > g_figRowHi) g_figRowHi = w->row;
    { int bLo = lo + w->off, bHi = hi + w->off;
      if (bLo < g_figColLo) g_figColLo = bLo;
      if (bHi > g_figColHi) g_figColHi = bHi; }
}
#endif /* ROF_PLATFORM_AMIGA */


void alien_shape_blit(void) {
    ROF_MEMBASE_DECL(mb);   /* 46 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
#if defined(ROF_PLATFORM_AMIGA) && defined(ROF_FLIGHT_PROBE)
    unsigned long _thud = 0;
    if (mem[0x0632]) { g_alHudCalls++; _thud = rof_subclock(); }
#endif
    for (uint8_t cell = 0x8F; cell <= 0x9F; cell++) mem[cell] = 0x00;   /* clear cell buffer */
    alien_field0_fill();
    alien_field1_fill();
    alien_field2_fill();
    alien_field3_fill();

    uint16_t srcRow = (uint16_t)(dl_src_index | (terrain_scroll_reload << 8));  /* $8B/$8C mask row */
    uint16_t dstRow = (uint16_t)(step_mode_flag | (mem[0x008E] << 8));          /* $8D/$8E dest row */
#ifdef ROF_PLATFORM_AMIGA
    /* Alien jump-scare mirror geometry: the whole per-cell map (wrap, range tests, row lookups,
     * base pointers, extents) collapses to ONE window per row — see alien_mirror_window above,
     * proven exhaustively by tools/alien_mirror_test.c.  The table bases go in locals too, so the
     * loop addresses them as (d8,An) instead of reloading g_figM/g_figP1/g_figP2 and re-`lea`ing
     * kModeDP1/kModeDP2 out of absolute memory on every one of the 17 cells. */
    const int alienKnock = (mem[0x0632] != 0) && (g_figP1 != 0);
    struct alien_mirror_win w;
    uint8_t vals[17];                    /* pass 1's cell values, mirrored by pass 2 below */
    int winTopY = 0x10;                  /* highest y the live window has covered */
    if (alienKnock) alien_mirror_window(dstRow, &w);
    else { w.lo = 1; w.hi = 0; w.row = w.off = 0; w.mask = w.p1 = w.p2 = (uint8_t *)0; }
#endif
    for (uint8_t y = 0x10; ; y--) {                          /* 17 cells, high offset first */
        uint8_t cell = mem[0x8F + y];
        screen_ptr_hi = cell;                                /* $0084 = cell (before the mask read) */
        uint8_t v = (uint8_t)(mem[0xBE00 + cell] & bus_read((uint16_t)(srcRow + y)));
        v |= cell;                                           /* ORA $0084 (== cell) */
        uint16_t dst = (uint16_t)(dstRow + y);
#ifdef ROF_PLATFORM_AMIGA
        /* During the airlock-CLOSED knock ($0632) the mode-D field body is SHED on the Amiga:
         * renderFlightDirect's rescueFigure branch composites the figure overlay over the FROZEN
         * terrain and RETURNS before ever reading the field (the band-row decode is past that
         * return), so this per-cell field write has no consumer -> skip it, mirror to the overlay
         * only.  In real play alienKnock is ALWAYS true here: this routine (despite its name) is
         * the alien-creature blitter, reached only via alien_creature_animate_draw <- alien_knock_setup_loop, which sets
         * $0632 first -- there is no non-knock caller.  The kept-write path below is thus taken
         * only by the validation oracle (#else, non-Amiga) so alien_shape_blit stays byte-
         * identical to its __t6502 twin, plus as a defensive guard should $0632 ever be clear. */
        if (!alienKnock) bus_write(dst, v);
#else
        bus_write(dst, v);
#endif
#ifdef ROF_PLATFORM_AMIGA
        /* Alien jump-scare: during the airlock-CLOSED knock ($0632, set by alien_knock_setup_loop) this
         * blitter draws the creature into the viewport field, which the Amiga sheds -> mirror each
         * byte into the paused-rescue figure overlay so renderFlightDirect composites it.  (This is
         * the ONLY caller path -- see the write-skip note above -- so $0632 is set for every real
         * call; the gate is a guard for the validation harness / any future non-knock use.) */
        if (alienKnock) {
            vals[y] = v;                    /* pass 2 (alien_mirror_flush) does the overlay work */
#if defined(ROF_FLIGHT_PROBE)
            rof_alien_crwrite(dst, v);      /* capture probe: NOT in the bench, it is per-CELL
                                             * and the shipping build has no such call */
#endif
        }
#endif
        /* The 6502 re-reads the $8B/$8D pointer cells every pass, so a store that aliased them
         * (only a bogus off-field pointer could — never in real play) must be reflected next pass.
         * One cheap range test preserves the loop-invariant hoist for the real case. */
        if (dst >= 0x8B && dst <= 0x8E) {
            srcRow = (uint16_t)(dl_src_index | (terrain_scroll_reload << 8));
            dstRow = (uint16_t)(step_mode_flag | (mem[0x008E] << 8));
#ifdef ROF_PLATFORM_AMIGA
            if (alienKnock) {
                /* Never taken in real play.  Mirror + commit the segment this window earned over
                 * the cells already walked (y..winTopY, descending), then re-window on the new
                 * row — so a mid-row pointer change still draws exactly what the per-cell form
                 * would have. */
                alien_mirror_flush(&w, vals, (int)y, winTopY);
                alien_mirror_window(dstRow, &w);
                winTopY = (int)y - 1;
            }
#endif
        }
        if (y == 0x00) break;
    }
#ifdef ROF_PLATFORM_AMIGA
    /* PASS 2: mirror the whole row's window in one tight walk and publish its extents once
     * (min/max is order-independent, so the result is identical to updating it cell by cell). */
    if (alienKnock) alien_mirror_flush(&w, vals, 0, winTopY);
#endif

    /* advance $8B/$8C += $60, then $8D/$8E = $8B/$8C + $30 (both 16-bit).  Read the pointer cells
     * fresh — a self-modifying store above may have changed them (matches the 6502, which reloads). */
    uint16_t nextSrc = (uint16_t)((dl_src_index | (terrain_scroll_reload << 8)) + 0x60);
    dl_src_index          = (uint8_t)nextSrc;
    terrain_scroll_reload = (uint8_t)(nextSrc >> 8);
    uint16_t nextDst = (uint16_t)(nextSrc + 0x30);
    step_mode_flag = (uint8_t)nextDst;
    mem[0x008E]    = (uint8_t)(nextDst >> 8);
#if defined(ROF_PLATFORM_AMIGA) && defined(ROF_FLIGHT_PROBE)
    if (_thud) g_alTHud += rof_subclock() - _thud;
#endif
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* Advance one voice of the alien_creature_animate_draw frame sequencer (voices A and B share this shape).
 * Walk the value table $81E8 forward one step; while still inside a run just return that value.
 * When the run ends (value 0) pick the next run through the link table $81E2 — resetting the
 * link index to 0 or 3 (chosen by whether the sustain counter $292E is still live) whenever it
 * strays outside 1..3 — and return the value at the linked position.
 *   posAddr  = the voice's table-position cell ($2924 for A, $005F for B)
 *   loopAddr = the voice's link-index cell   ($005E for A, $2921 for B) */
static uint8_t seq_voice_step(unsigned posAddr, unsigned loopAddr) {
    uint8_t pos = (uint8_t)(mem[posAddr] + 1);
    mem[posAddr] = pos;
    uint8_t v = mem[0x81E8 + pos];
    if (v == 0) {                                   /* run finished — follow the link */
        uint8_t idx = mem[loopAddr];
        if (idx == 0 || idx >= 4) { idx = mem[0x292E] ? 0 : 3; mem[loopAddr] = idx; }
        uint8_t link = mem[0x81E2 + idx];
        mem[posAddr] = link;
        v = mem[0x81E8 + link];
    }
    return v;
}

/* alien_creature_animate_draw @ $7F85 — step the alien-creature animation and draw one full frame of it.
 * (Misnamed "sfx_seq" — it is the creature animator/blitter; see docs/rename.md.)  Runs once per
 * knock SFX step.  Three independent "voices" (A, B, C) each advance through frame tables to pick
 * this frame's three shape indices; those index the shape-parameter tables ($81A1/$81A9/$81B1/
 * $81B9 for A and B, $81C1/$81C9/$81D1/$81D9 for C) to load the four field-source pointers/limits
 * consumed by alien_shape_blit; then the row-blit loop draws rows $2930..$2B of the creature.
 * The tail retires the SFX step: while the sustain counter $292E holds it queues two ring markers
 * ($1A/$1B); when it underflows it silences audio and hands off to the audio IRQ. */
void alien_creature_animate_draw(void) {
    ROF_MEMBASE_DECL(mb);   /* 74 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* --- Voice A / B: table-walked frame values --- */
    uint8_t frameA = seq_voice_step(0x2924, 0x005E);   /* $2924 pos, $005E link */
    mem[0x292A] = frameA;
    uint8_t frameB = seq_voice_step(0x005F, 0x2921);   /* $005F pos, $2921 link */
    mem[0x292B] = frameB;

    /* --- Voice C: randomly-picked frame value (avoiding the last two picks $2922/$2923) --- */
    uint8_t vposC = (uint8_t)(mem[0x2926] + 1);
    mem[0x2926] = vposC;
    uint8_t frameC = mem[0x820A + vposC];
    if (frameC == 0) {                                 /* run finished — pick a fresh target */
        uint8_t pick;
        do { pick = (uint8_t)((bus_read(0xD20A) & 7) + 1); }   /* POKEY RANDOM, 1..8 */
        while (pick == mem[0x2922] || pick == mem[0x2923]);
        mem[0x2923] = mem[0x2922];
        mem[0x2922] = pick;
        uint8_t link = mem[0x8201 + pick];
        mem[0x2926] = link;
        frameC = mem[0x820A + link];
    }
    mem[0x292C] = frameC;

    /* --- Load the field-0/1 source pointers + cursors + limits from frames A and B --- */
    encounter_count  = mem[0x81A1 + frameA];   /* $0085 field-0 ptr lo */
    row_count        = mem[0x81A9 + frameA];   /* $0086 field-0 ptr hi */
    sync_flag        = mem[0x81B1 + frameA];   /* $0080 field-0 cursor */
    mem[0x2927]      = mem[0x81B9 + frameA];   /* field-0 limit */
    vbi_phase        = mem[0x81A1 + frameB];   /* $0087 field-1 ptr lo */
    vbi_flags        = mem[0x81A9 + frameB];   /* $0088 field-1 ptr hi */
    dl_ptr_lo        = mem[0x81B1 + frameB];   /* $0081 field-1 cursor */
    mem[0x2928]      = mem[0x81B9 + frameB];   /* field-1 limit */

    /* --- Load the field-2 source from frame C.  Values >= $0C select the "packed" glyph set
     *     (index -= $0B) and set the pack flag $292D; else the verbatim set. --- */
    mem[0x292D] = 0x00;
    uint8_t idxC = frameC;
    if (idxC >= 0x0C) { idxC = (uint8_t)(idxC - 0x0B); mem[0x292D] = 0x01; }
    terrain_state          = mem[0x81C1 + idxC];   /* $0089 field-2 ptr lo */
    terrain_scroll_counter = mem[0x81C9 + idxC];   /* $008A field-2 ptr hi */
    dl_ptr_hi              = mem[0x81D1 + idxC];   /* $0082 field-2 cursor */
    mem[0x2929]           = mem[0x81D9 + idxC];   /* field-2 limit */
    screen_ptr_lo = 0xE7;                          /* $0083 field-3 cursor (>= $A8 => field 3 idle) */

    /* --- Compute the first row's source/dest pointers = rowtable[$2930] + $2931, dest = +$30 --- */
    uint8_t rowIdx = mem[0x2930];
    uint16_t base  = (uint16_t)(mem[MEM_row_base_lo + rowIdx] | (mem[MEM_row_base_hi + rowIdx] << 8));
    uint16_t maskRow = (uint16_t)(base + mem[0x2931]);
    dl_src_index          = (uint8_t)maskRow;            /* $8B */
    terrain_scroll_reload = (uint8_t)(maskRow >> 8);     /* $8C */
    uint16_t dstRow = (uint16_t)(maskRow + 0x30);
    step_mode_flag = (uint8_t)dstRow;                    /* $8D */
    mem[0x008E]    = (uint8_t)(dstRow >> 8);             /* $8E */

    /* --- Draw the creature: one alien_shape_blit per field row from $2930 up to $2B --- */
    uint8_t r = mem[0x2930];
    do {
        mem[0x292F] = r;
        alien_shape_blit();
        r++;
    } while (r < 0x2C);

    /* --- Retire this SFX step.  Only act when voice A or B is at a run boundary (0 or $0D). --- */
    uint8_t vposA = mem[0x2924], vposB = mem[0x005F];
    if (vposA == 0 || vposA == 0x0D || vposB == 0 || vposB == 0x0D) {
        uint8_t sustain = (uint8_t)(mem[0x292E] - 1);
        mem[0x292E] = sustain;
        if (sustain & 0x80) {              /* underflowed past 0 -> end the SFX */
            mem[0x004A] = 0x00;            /* joystick_saved = 0 */
            silence_audio_channels();
            audio_irq_handler();
        } else {                            /* still sustaining -> queue ring markers $1A, $1B */
            cpu.X = 0x1A; ring_push_marked();
            cpu.X = (uint8_t)(cpu.X + 1); ring_push_marked();
        }
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* render_bcd_counter @ $49A0 — render the 3-byte packed-BCD score ($0601-$0603,
 * 6 digits) to the top text line $32C5..$32CA with leading-zero suppression.
 * Flight ISR routine; the first transpiled-on-the-VBI-path fn ported native.
 *
 * Faithfully reproduces the self-contained fall-through chain $49A0-$49ED
 * (render_bcd_top_byte/_low_bytes/emit_bcd_byte_digits/plot_char_bounded): a
 * running X flag ($0600 entry) suppresses leading zeros until the first nonzero
 * digit; digit column Y < threshold $0619(=5) keeps a zero blank, else it is
 * forced (so a zero score still shows one "0").  Char code = digit + $50.
 * Contract: memory only — exit cpu is dead (called from flight_vbi_native).  The
 * 6502's emit_bcd_byte_digits PHA/PLA leaves a dead scribble at $01FF (S=$FF in
 * the harness); excluded from this fn's contract via set_ignore (validate_native.c).
 */
void render_bcd_counter(void) {
    digit_dst_ptr_lo = 0xC5; digit_dst_ptr_hi = 0x32;      /* $49A0: dest ptr $32C5 */
    uint8_t x = mem[0x0600];                      /* $49C2: LDX $0600 (running suppress flag) */
    zero_suppress_pos = 0x05;                           /* $49C5: STY $0619 (zero-suppress threshold) */
    bcd_delta_lo = 0x00; bcd_delta_hi = 0x00;       /* $49C8-$49CC: clear 16-bit delta */
    uint8_t y = 0;
    static const uint16_t bcd[3] = { 0x0601, 0x0602, 0x0603 };
    for (int i = 0; i < 3; i++) {
        uint8_t v = mem[bcd[i]];
        uint8_t nib[2] = { (uint8_t)(v >> 4), (uint8_t)(v & 0x0F) };  /* hi then lo nibble */
        for (int n = 0; n < 2; n++) {
            uint8_t a = nib[n];
            int write;                            /* plot_char_bounded $49D9 */
            if      (x != 0)              write = 1;            /* nonzero seen -> always write */
            else if (a != 0)           { x++; write = 1; }      /* first nonzero digit */
            else if (y < zero_suppress_pos)    write = 0;             /* leading zero below thresh -> blank */
            else                       { x++; write = 1; }      /* forced write at/after thresh */
            if (write) mem[0x32C5 + y] = (uint8_t)(a + 0x50);
            y++;
        }
    }
    platform_title_changed();   /* hook: score digits rendered into $32C5-$32CA -> flag title dirty */
}

/* init_proj_scratch_pointers @ $9B87 — set game_state=1 and 3 ZP scratch/ptr bytes.
 * Trivial leaf of the update_terrain_scanline_proj subtree.  Memory-only contract. */
void init_proj_scratch_pointers(void) {
    game_state = 0x01;   /* game_state = 1 */
    anim_counter_2 = 0x3C;
    mem[0x00DC] = 0x38;
    audc_shadow_0 = 0x34;
}

/* ring_push_0719_core — push one event byte into the 32-entry $0719 SFX event ring.
 * alt_ring_head ($0073) is the write cursor: a corrupt cursor is clamped into 0..$1F, the
 * byte is stored, then the cursor pre-decrements with wraparound ($00 -> $1F).  The ring is
 * drained by the $548D voice engine each flight VBI.  Pure: no registers, no stack. */
static void ring_push_marked_core(uint8_t id);   /* defined below ($5815 typed entry) */
static void ring_push_0719_core(uint8_t ev) {
    uint8_t head = alt_ring_head;
    if (head >= 0x20) head = 0x1F;
    mem[MEM_event_ring_0719 + head] = ev;
    alt_ring_head = (head == 0x00) ? 0x1F : (uint8_t)(head - 1);
}

/* ring_push_marked @ $5815 — push (entry X)|$80 into the $0719 event ring ($80 = the "marked"
 * bit the SFX engine keys on); X preserved.
 * ring_push_unmarked @ $55FC — push entry Y into the ring; X preserved.
 * Both are 6502-ABI entries (for the transpiled callers + the validation oracle).  The 6502
 * PHA'd the saved index and ring_push_0719's PLA;TAX handed it back, so cpu.A/X AND the lone
 * $01xx stack-page byte are part of the contract; reproduce them (net cpu.S unchanged). */
void ring_push_marked(void) {
    uint8_t id = cpu.X;
    mem[0x0100 | cpu.S] = id;                     /* PHA (the one stack-page side-effect) */
    ring_push_0719_core((uint8_t)(id | 0x80));
    cpu.A = id; cpu.X = id;                        /* PLA;TAX -> index handed back */
    cpu.N = (id >> 7) & 1; cpu.Z = (id == 0) ? 1 : 0;
}
void ring_push_unmarked(void) {
    SX_CNT(g_sxRingPush);
    uint8_t id = cpu.X;
    mem[0x0100 | cpu.S] = id;                     /* PHA X */
    ring_push_0719_core(cpu.Y);                    /* push Y (unmarked) */
    cpu.A = id; cpu.X = id;                        /* PLA;TAX */
    cpu.N = (id >> 7) & 1; cpu.Z = (id == 0) ? 1 : 0;
}

/* sample_terrain_height_bilerp @ $9A36 — bilinear-sample the 16x16 height map $0900.
 *
 * Fetch 4 corner heights into $27F0-$27F3 (row = $2800<<4, col = $27FE low nibble,
 * X wraps within the nibble, row +$10 for the second), then 3 bit-serial fractional
 * blends ($27FD horizontally twice -> $27F4/$27F5, then $27FF vertically -> R), then
 * scale R += R>>4 + R>>5 into $0062.  $27F0-$27F5/$27FA/$27FB are shifted in place
 * (scratch, part of the mem contract).  Bounded 8-iter loops -> random mem is safe.
 *
 * The blend's ADC adds the byte AFTER an in-place LSR plus the bit that LSR shifted
 * out (rounding) — reproduced exactly. */
/* ---- terr_blend's 8-iteration bit-serial loop as two 4 KB tables -------------------------
 *
 * SHAPE (amiga/integ_shape.gdb, 2026-08-07): sample_terrain_height_bilerp is ~79% of the flight
 * VBI's `proj` bucket AND 78% of the `obj` bucket inside `integ` — together ~11 t per ISR firing
 * = ~10% of the whole flight VBI = ~3.5% of ALL wall clock — and 82% of the bilerp is these
 * three blends.  It is the single biggest block in the 50 Hz ISR after the sfx engine.
 *
 * WHY IT COLLAPSES: BOTH branches of the loop shift BOTH operands exactly once, so at the start
 * of iteration i the operands are always L0>>i and H0>>i whatever path the fraction took, and
 * the term added is
 *       T(X,i) = (X >> (i+1)) + ((X >> i) & 1)        (round-half-up of (X>>i)/2)
 * with X = H when f's bit (7-i) is set, else L.  A is a plain 8-bit accumulator, so the total is
 * a sum mod 256 and therefore ORDER-INDEPENDENT — which makes it separable into "the terms f
 * selects from H" plus "the terms f does not select, from L".  The second is the first applied
 * to ~f.  Splitting the selector by nibble gives two 256x16 tables instead of one 256x256:
 *
 *       A = Bhi[H][f>>4] + Blo[H][f&15] + Bhi[L][15-(f>>4)] + Blo[L][15-(f&15)]   (mod 256)
 *
 * 8 KB total.  (The one-table 64 KB form works too and needs only 2 lookups, but the non-chip
 * load image is already 490 KB against a 512 KB slow bank on a 512+512 A500 — see the RAM
 * budget in amiga/memreport.gdb — and the two forms measured the same speed.)
 *
 * EXACTNESS: tools/terr_blend_table_test.c checks the identity over ALL 2^24 (f,L,H) triples —
 * 0 mismatches, for both the 8 KB and 64 KB forms.  The mem[] contract is unchanged and is the
 * one tools/terr_blend_test.c already proved: the fraction shifts left 8 times and each operand
 * right 8 times, so all three cells end at 0 for every input.
 *
 * `make BLEND_LOOP=1` restores the bit-serial loop so the win can be re-measured in one tree
 * (GDBSCRIPT=isr_ab.gdb; the metric is whole-VBI t/firing, which IS cross-build-legitimate). */
uint8_t g_blendHi[256u * 16u];      /* Bhi[(X<<4)|n] — f's bits 7..4, i.e. iterations 0..3 */
uint8_t g_blendLo[256u * 16u];      /* Blo[(X<<4)|n] — f's bits 3..0, i.e. iterations 4..7 */
static int g_blendTableReady = 0;

/* Public one-shot builder: call ONCE at startup, like rof_mul_table_init (main.cpp).  8192
 * entries of at most 8 adds — microseconds, not the mul table's seconds — but building it in
 * the flight VBI would still be a visible hitch.  Idempotent. */
void rof_blend_table_init(void) {
    if (g_blendTableReady) return;
    for (unsigned X = 0; X < 256; X++) {
        for (unsigned n = 0; n < 16; n++) {
            uint8_t sh = 0, sl = 0;
            for (int i = 0; i < 4; i++)                     /* f bits 7..4 -> iterations 0..3 */
                if ((n >> (3 - i)) & 1)
                    sh = (uint8_t)(sh + (uint8_t)((X >> (i + 1)) + ((X >> i) & 1)));
            for (int i = 4; i < 8; i++)                     /* f bits 3..0 -> iterations 4..7 */
                if ((n >> (7 - i)) & 1)
                    sl = (uint8_t)(sl + (uint8_t)((X >> (i + 1)) + ((X >> i) & 1)));
            g_blendHi[(X << 4) | n] = sh;
            g_blendLo[(X << 4) | n] = sl;
        }
    }
    g_blendTableReady = 1;
}

static uint8_t terr_blend(uint16_t fa, uint16_t lo, uint16_t hi) {
    /* Callers never pass overlapping addresses (fa is $27FA/$27FB, lo/hi are $27F0-$27F5). */
    unsigned f = mem[fa];
#ifdef ROF_BLEND_LOOP
    /* A/B baseline (`make BLEND_LOOP=1`): the original bit-serial loop, register-hoisted. */
    uint8_t L = mem[lo], H = mem[hi], A = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(f >> 7);
        f = (uint8_t)(f << 1);                        /* ASL fraction -> carry=bit */
        if (bit) {                                    /* B1: LSR lo; LSR hi; ADC hi */
            L = (uint8_t)(L >> 1);                    /* LSR lo (carry discarded) */
            uint8_t c = (uint8_t)(H & 1);
            H = (uint8_t)(H >> 1);
            A = (uint8_t)(A + H + c);                 /* ADC hi (+ shifted-out bit) */
        } else {                                      /* bit0: LSR hi; LSR lo; ADC lo */
            H = (uint8_t)(H >> 1);
            uint8_t c = (uint8_t)(L & 1);
            L = (uint8_t)(L >> 1);
            A = (uint8_t)(A + L + c);                 /* ADC lo */
        }
    }
#else
    unsigned L = (unsigned)mem[lo] << 4, H = (unsigned)mem[hi] << 4;
    unsigned fh = f >> 4, fl = f & 15;                /* ~f's nibbles are 15-fh / 15-fl */
    uint8_t A = (uint8_t)(g_blendHi[H | fh] + g_blendLo[H | fl]
                        + g_blendHi[L | (15u - fh)] + g_blendLo[L | (15u - fl)]);
#endif
    mem[fa] = 0; mem[lo] = 0; mem[hi] = 0;            /* the shifted-out final state */
    return A;
}
void sample_terrain_height_bilerp(void) {
    O2_DECL(); O2_START(g_blCalls); O2_LAP(g_blNop);   /* empty lap = this level's floor */
    if (!g_blendTableReady) rof_blend_table_init();    /* safety net if init was skipped */
    uint8_t row = (uint8_t)(map_z_hi << 4);                       /* $9A36: $0061 */
    terrain_lerp_index = row;
    uint8_t y = (uint8_t)((map_x_hi & 0x0F) | row);
    mem[0x27F0] = mem[0x0900 + y];
    y = (uint8_t)(((uint8_t)(y + 1) & 0x0F) | row);
    mem[0x27F1] = mem[0x0900 + y];
    row = (uint8_t)(row + 0x10); terrain_lerp_index = row;                  /* $0061 += $10 */
    y = (uint8_t)((map_x_hi & 0x0F) | row);
    mem[0x27F2] = mem[0x0900 + y];
    y = (uint8_t)(((uint8_t)(y + 1) & 0x0F) | row);
    mem[0x27F3] = mem[0x0900 + y];

    O2_LAP(g_blFetch);
    mem[0x27FA] = map_x_lo; mem[0x27F4] = terr_blend(0x27FA, 0x27F0, 0x27F1);
    O2_LAP(g_blB1);
    mem[0x27FA] = map_x_lo; mem[0x27F5] = terr_blend(0x27FA, 0x27F2, 0x27F3);
    O2_LAP(g_blB2);
    mem[0x27FB] = map_z_lo; uint8_t R = terr_blend(0x27FB, 0x27F4, 0x27F5);
    O2_LAP(g_blB3);

    terrain_height_sample = R;                                                 /* $9AFA */
    uint8_t hi4 = (uint8_t)(R >> 4);                                 /* LSRx4 / TAY */
    terrain_height_sample = (uint8_t)(hi4 + terrain_height_sample);                      /* CLC; ADC $0062 */
    uint8_t c = (uint8_t)(hi4 & 1);                                  /* TYA; LSR -> carry */
    terrain_height_sample = (uint8_t)((hi4 >> 1) + terrain_height_sample + c);           /* ADC $0062 */
    O2_LAP(g_blTail);
}

/* game_sub_451d @ $451D — fill 14 cells of $2159+Y / $2189+Y from table $4553[X],
 * with X/$BD evolving per the $BB threshold.  Self-contained loop (no callees).
 * Inputs: cpu.A/X/Y.  Memory-only contract (validated with random entry regs).
 */
void game_sub_451d(void) {
    uint8_t A = cpu.A, X = cpu.X, Y = cpu.Y;
    dl_y1 = A;                                   /* $451D STA $BB (entry A) */
    dl_y3 = X;                                   /* $451F STX $BD */
    A = (uint8_t)((A & 0x03) | dl_y3);           /* $4521 AND#3; ORA $BD */
    dl_y3 = A;                                   /* $4525 STA $BD */
    A = (uint8_t)((A & 0x04) ^ 0x04);                  /* $4527 AND#4; EOR#4 */
    X = A;                                             /* $452B TAX */
    dl_y1 = (uint8_t)(dl_y1 >> 2);         /* $452C LSR $BB; LSR $BB */
    A = (uint8_t)(Y | dl_y1);                    /* $4530 TYA; ORA $BB */
    dl_y1 = A;                                   /* $4533 STA $BB */
    bar_col_threshold = 0x0E;                                /* $4535 loop count 14 */
    /* ⭐ Both destinations advance by one per step, so walk pointers instead of re-deriving
     * mem[base+Y] from an 8-bit index each time (see draw_altimeter_bars for the cycle count
     * this shape costs).  Only the non-wrapping case, which is every real call — the two
     * callers pass Y = 0 and Y = $10 — since a wrap makes the address non-monotonic. */
    if (Y <= (uint8_t)(0xFF - 0x0E)) {
        uint8_t *d1 = (uint8_t *)&mem[0x2159 + Y];
        uint8_t *d2 = (uint8_t *)&mem[0x2189 + Y];
        do {
            if (Y >= dl_y1) { X = dl_y3; dl_y3 = (uint8_t)(X & 0x04); }
            uint8_t v = mem[0x4553 + X];
            *d1++ = v; *d2++ = v;
            Y = (uint8_t)(Y + 1);
            bar_col_threshold = (uint8_t)(bar_col_threshold - 1);
        } while (bar_col_threshold != 0);
    } else
    do {
        if (Y >= dl_y1) {                        /* $4539 CPY $BB; BCC skip */
            X = dl_y3;                           /* $453D LDX $BD */
            dl_y3 = (uint8_t)(X & 0x04);         /* $453F TXA; AND#4; STA $BD */
        }
        uint8_t v = mem[0x4553 + X];                   /* $4544 LDA $4553,X */
        mem[0x2159 + Y] = v;                           /* $4547 STA $2159,Y */
        mem[0x2189 + Y] = v;                           /* $454A STA $2189,Y */
        Y = (uint8_t)(Y + 1);                          /* $454D INY */
        bar_col_threshold = (uint8_t)(bar_col_threshold - 1);      /* $454E DEC $BF */
    } while (bar_col_threshold != 0);                        /* $4550 BNE */
}

/* enter_terrain_special_state @ $9B0D — set $2877/$3355 + (gated on $062F/$066C)
 * seed flags $0688/$0689/$0696/$0697/$06A4/$06A5 and push 2 ring events, then
 * tail ring_push_marked(X=$13).  exit_terrain_special_state @ $9B4C is the inverse.
 * Both call the native ring_push_unmarked/ring_push_marked (cpu.X preserved across them,
 * ring content = cpu.Y).  Memory contract; validated with random entry regs + mem. */
void enter_terrain_special_state(void) {
    vobj_speed_frac = (uint8_t)(throttle_accum_lo | 0x40);          /* $9B0D */
    if (life_counter == 0) return;                         /* $9B14 BNE; else RTS */
    special_state_color = 0x34;                                   /* $9B1A */
    if (mem[0x066C] != 0x0A) {                            /* $9B1F CMP $066C; BEQ skip */
        mem[0x0696] = 0x0A; mem[0x0697] = 0x0A;           /* $9B26 */
        mem[0x0688] = 1; mem[0x0689] = 1;                 /* $9B2E */
        mem[0x06A4] = 1; mem[0x06A5] = 1;                 /* $9B34 */
        cpu.Y = 1; ring_push_unmarked();                       /* $9B3A push Y=1 */
        cpu.Y = (uint8_t)(cpu.Y + 1); ring_push_unmarked();    /* $9B3D INY; $9B3E push Y=2 */
    }
    if (throttle_accum_hi != 0) throttle_accum_hi = (uint8_t)(throttle_accum_hi - 1);  /* $9B41 */
    cpu.X = 0x13; ring_push_marked();                     /* $9B47 LDX #$13; JMP $5815 */
}
void exit_terrain_special_state(void) {
    uint8_t s = mem[0x066C];                              /* $9B4C */
    if (s == 0) return;                                   /* BEQ RTS */
    if (s < 0x02) {                                       /* $9B51 CMP #2; BCS skip-gate */
        if (mem[0x06A4] == 0) return;                     /* $9B55 */
        if (mem[0x0696] == 0) return;                     /* $9B5A */
    }
    special_state_color = 0xB4;                                   /* $9B5F */
    vobj_speed_frac = 0; mem[0x0696] = 0; mem[0x0697] = 0;    /* $9B64 */
    mem[0x0688] = 0xFF; mem[0x0689] = 0xFF;               /* $9B6F */
    mem[0x06A4] = 1; mem[0x06A5] = 1;                     /* $9B77 */
    cpu.Y = 1; ring_push_unmarked();                           /* $9B7F push Y=1 */
    cpu.Y = (uint8_t)(cpu.Y + 1); ring_push_unmarked();        /* $9B82 INY; $9B83 push Y=2 */
}

/* {hi:mem[lo]} = signed({hi:mem[lo]}) >> 4 (arithmetic), via 4x (CMP #$80; ROR A;
 * ROR lo) — the 6502 sign-extending shift used by update_terrain_horizon_lr. */
static uint8_t asr16_4(uint8_t hi, uint16_t lo) {
    for (int i = 0; i < 4; i++) {
        uint8_t cin  = (hi >= 0x80) ? 1 : 0;        /* CMP #$80 -> C = sign */
        uint8_t cout = (uint8_t)(hi & 1);           /* ROR A: bit0 out */
        hi = (uint8_t)((cin << 7) | (hi >> 1));     /* ROR A */
        uint8_t l = mem[lo];
        mem[lo] = (uint8_t)((cout << 7) | (l >> 1)); /* ROR lo */
    }
    return hi;
}

/* update_terrain_horizon_lr @ $992D — alternately ($2834/$2833 toggles) update the
 * left ($282E) or right ($282F) horizon: build map coords {$27FD..$2800} from
 * {$2801..$2804} +/- the >>4 deltas {$2805..$2808} (from $2809-$280C), sample the
 * height map, derive the horizon row, and on a change cache it ($2841/$2842) and
 * repaint via game_sub_451d.  All callees native; bounded loops -> random mem safe. */
void update_terrain_horizon_lr(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* $992D: $2834 every-other gate */
    uint8_t v = horizon_phase_toggle, c = (uint8_t)(v & 1);
    horizon_phase_toggle = (uint8_t)(v >> 1);
    if (!c) { horizon_phase_toggle = (uint8_t)(horizon_phase_toggle + 1); return; }

    /* $9936: {$2806:$2805} = asr4({$280A:$2809}); {$2808:$2807} = asr4({$280C:$280B}) */
    mem[0x2805] = mem[0x2809]; mem[0x2806] = asr16_4(mem[0x280A], 0x2805);
    mem[0x2807] = mem[0x280B]; mem[0x2808] = asr16_4(mem[0x280C], 0x2807);

    /* $9964: $2833 toggle -> second (right) half if odd */
    uint8_t v2 = mem[0x2833], c2 = (uint8_t)(v2 & 1);
    mem[0x2833] = (uint8_t)(v2 >> 1);
    int second = c2;
    if (!second) mem[0x2833] = (uint8_t)(mem[0x2833] + 1);

    uint16_t c2801 = ROF_PAIR16(mem[0x2801], mem[0x2802]);
    uint16_t c2803 = ROF_PAIR16(mem[0x2803], mem[0x2804]);
    uint16_t d05   = ROF_PAIR16(mem[0x2805], mem[0x2806]);
    uint16_t d07   = ROF_PAIR16(mem[0x2807], mem[0x2808]);

    if (!second) {
        /* $996C: coords = {2801}-{d07}, {2803}+{d05} */
        uint16_t m = (uint16_t)(c2801 - d07), p = (uint16_t)(c2803 + d05);
        map_x_lo = (uint8_t)m; map_x_hi = (uint8_t)(m >> 8);
        map_z_lo = (uint8_t)p; map_z_hi = (uint8_t)(p >> 8);
        sample_terrain_height_bilerp();                 /* $9992 */
        terrain_height_sample = (uint8_t)(terrain_height_sample >> 1);       /* $9995 LSR $0062 */
        mem[0x27F7] = (uint8_t)(terrain_height_sample >> 1);       /* $27F7 = $0062>>1 */
        uint8_t A = (uint8_t)(scaled_depth_hi >> 1);         /* $999D */
        A = (uint8_t)(A + mem[0x0024]);                  /* CLC; ADC $0024 */
        A = (uint8_t)(A + 0x0F);                         /* CLC; ADC #$0F */
        uint8_t sub = terrain_height_sample;
        uint8_t r = (A >= sub) ? (uint8_t)(A - sub) : 0; /* SEC; SBC $0062; BCS/clamp0 */
        mem[0x27F6] = r;
        uint8_t h = (0x38 >= r) ? (uint8_t)(0x38 - r) : 0;  /* $38 - $27F6, clamp0 */
        horizon_left = h;
        if (h == horizon_left_prev) return;                    /* $99BE CMP $2841; BEQ */
        horizon_left_prev = h;
        cpu.A = h; cpu.X = 0; cpu.Y = 0; game_sub_451d(); /* $99C6 X=0,Y=0; JMP $451D */
    } else {
        /* $99CE: coords = {2801}+{d07}, {2803}-{d05} */
        uint16_t m = (uint16_t)(c2801 + d07), p = (uint16_t)(c2803 - d05);
        map_x_lo = (uint8_t)m; map_x_hi = (uint8_t)(m >> 8);
        map_z_lo = (uint8_t)p; map_z_hi = (uint8_t)(p >> 8);
        sample_terrain_height_bilerp();                 /* $99F4 */
        terrain_height_sample = (uint8_t)(terrain_height_sample >> 1);       /* $99F7 LSR $0062 */
        mem[0x27F8] = (uint8_t)(terrain_height_sample >> 1);       /* $27F8 = $0062>>1 */
        uint8_t A = (uint8_t)(scaled_depth_hi >> 1);         /* $99FF */
        A = (uint8_t)(A - mem[0x0024]);                  /* SEC; SBC $0024 */
        A = (uint8_t)(A + 0x0F);                         /* CLC; ADC #$0F */
        uint8_t sub = terrain_height_sample;
        uint8_t r = (A >= sub) ? (uint8_t)(A - sub) : 0; /* SEC; SBC $0062; clamp0 */
        mem[0x27F6] = r;
        uint8_t h = (0x38 >= r) ? (uint8_t)(0x38 - r) : 0;
        horizon_right = h;
        if (h == horizon_right_prev) return;                    /* $9A20 CMP $2842; BEQ */
        horizon_right_prev = h;
        cpu.X = 4; cpu.Y = 0x10;                          /* $9A28 X=4,Y=$10 */
        cpu.A = (uint8_t)(0x38 - horizon_right);            /* SEC; LDA #$38; SBC $282F */
        game_sub_451d();                                  /* JMP $451D */
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* update_terrain_scanline_proj @ $9833 — top of the in-flight terrain projection subtree.
 * Once per frame it derives everything the rasterizer/horizon code needs from the current
 * ship position:
 *   - the map cell the ship sits over (world position >> 4) and the sub-cell depth;
 *   - the sampled terrain height under that cell, and the visible viewport span;
 *   - the above-terrain clearance, advancing to the next depth row (and toggling the
 *     terrain "special state") when the ship sinks into the terrain;
 *   - the L/R horizon; and the landing/launch projection phase state machine.
 * All callees are native; the function reads no entry registers and writes only memory
 * (validated byte-for-byte against the $9833 oracle). No hardware is touched. */
static void update_terrain_scanline_proj_impl(void) {
    ROF_MEMBASE_DECL(mb);                     /* 89 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* Map cell coords = world position >> 4.  Stored to the bilinear-sampler inputs
     * (map_x/map_z), plus a scratch pair and a mirror pair the horizon code reads. */
    uint16_t mx = (uint16_t)(ROF_PAIR16(world_x_lo, world_x_hi) >> 4);
    mem[0x2270] = (uint8_t)mx; map_x_lo = (uint8_t)mx; mem[0x2801] = (uint8_t)mx;
    mem[0x2271] = (uint8_t)(mx >> 8); map_x_hi = (uint8_t)(mx >> 8); mem[0x2802] = (uint8_t)(mx >> 8);
    uint16_t mz = (uint16_t)(ROF_PAIR16(world_z_lo, world_z_hi) >> 4);
    mem[0x2272] = (uint8_t)mz; map_z_lo = (uint8_t)mz; mem[0x2803] = (uint8_t)mz;
    mem[0x2273] = (uint8_t)(mz >> 8); map_z_hi = (uint8_t)(mz >> 8); mem[0x2804] = (uint8_t)(mz >> 8);

    /* Sub-cell depth fixed-point = (clamp(depth_step,$3F) : depth_frac) << 2. */
    uint8_t step = terrain_depth_step;
    if (step >= 0x40) step = 0x3F;
    uint16_t depth = (uint16_t)(ROF_PAIR16(terrain_depth_frac, step) << 2);
    scaled_depth_hi = (uint8_t)(depth >> 8);   /* $2275 */
    mem[0x2274]     = (uint8_t)depth;          /* $2274: depth fraction low byte */

    /* Sample terrain height under the map cell -> terrain_height_sample ($0062).
     * Quantise it to quarters and turn that into the top visible scanline ($281A). */
    sample_terrain_height_bilerp();
    uint8_t hq = terrain_height_sample;
    hq = (uint8_t)((hq >> 1) + (hq & 1));      /* round /2 */
    hq = (uint8_t)((hq >> 1) + (hq & 1));      /* round /2 -> height/4 */
    mem[0x27F9] = hq;
    mem[0x281A] = (hq <= 0x37) ? (uint8_t)(0x37 - hq) : 0;

    /* Advance the depth row by comparing this row's depth against the terrain height. */
    uint8_t height = terrain_height_sample;
    if (scaled_depth_hi >= height) {
        /* Still above the terrain: record clearance, leave the special state if in it. */
        mem[0x0070] = (uint8_t)(scaled_depth_hi - height);
        if (mem[0x283C] == 0) exit_terrain_special_state();
    } else {
        /* Sank into the terrain: enter the special state and step to the next depth row. */
        if (flight_mode_state != 0) enter_terrain_special_state();
        terrain_depth_step++;
        terrain_depth_frac = 0;
        mem[0x0070] = 0;
        if (roll_pos_hi & 0x80) {              /* roll below level -> nudge back up */
            roll_pos_hi++;
            roll_pos_lo = 0;
        }
    }

    /* Bottom of the visible span; once the depth step passes $37, grow the top span instead. */
    uint8_t d = terrain_depth_step;
    if (d <= 0x37) {
        mem[0x281B] = (uint8_t)(0x37 - d);
    } else {
        uint8_t top = (uint8_t)(mem[0x281A] + (d - 0x37));
        if (top >= 0x38) top = 0x38;
        mem[0x281A] = top;
        mem[0x281B] = 0;
    }

    update_terrain_horizon_lr();

    /* Landing/launch projection phase, gated on $066C:
     *   >=8  -> arm phase 1 + (re)init the projection scratch pointers (game_state=1);
     *   4..7 -> hold;
     *   <4   -> if phase was armed, disarm it and clear game_state. */
    uint8_t phase = mem[0x066C];
    if (phase >= 0x08) {
        mem[0x2879] = 1;
        init_proj_scratch_pointers();
        return;
    }
    if (mem[0x2879] == 0) return;
    if (phase >= 0x04) return;
    mem[0x2879] = 0;
    game_state = 0;
#ifdef ROF_MEMBASE
#undef mem
#endif
}
#ifdef ROF_FLIGHT_PROBE
extern volatile unsigned long g_pProj, g_pInteg;
void update_terrain_scanline_proj(void) { unsigned long _p = rof_subclock(); update_terrain_scanline_proj_impl(); g_pProj += rof_subclock() - _p; }
#else
void update_terrain_scanline_proj(void) { update_terrain_scanline_proj_impl(); }
#endif

/* signed_mul_8x16 @ $9C97 — fixed-point signed multiply.
 *
 * Inputs : cpu.A      = 8-bit multiplier (treated as an unsigned fraction),
 *          $00AA/$00AB = 16-bit signed multiplicand (lo/hi).
 *          cpu.C      = entry carry (see note below).
 * Outputs: $00A8/$00A9 = signed 16-bit product (lo/hi), $00AC = fractional byte,
 *          $00AA/$00AB = |multiplicand| (negated in place when input was < 0),
 *          $00AD = original sign byte (= input $00AB).
 *
 * Algorithm (faithful to the 6502): take |multiplicand| via $00AB's sign, then an
 * 8-step shift-add where the multiplier byte lives in $00AC and is rotated out one
 * bit per step into a 24-bit {A9:A8:AC} register; re-apply the sign at the end.
 *
 * ENTRY CARRY: the first `ROR $AC` ($9CB2) rotates the caller's carry into bit7 of
 * $00AC.  That injected bit is shifted back out across the 8 rotations and never
 * reaches an add decision, so it does NOT affect $00A8/$00A9 — but it does land in
 * the (functionally dead) final $00AC, so we thread it to stay byte-identical.
 *
 * Contract: memory only.  All four call sites (a0ad/a0c4/a0e2/a0f9) reload A and
 * the flags immediately after the call, so the 6502 exit register state is dead.
 */
void signed_mul_8x16(void) {
    uint8_t  m       = cpu.A;                 /* 9C97: multiplier                      */
    uint8_t  signhi  = mem[0x00AB];           /* entry hi byte = sign byte             */
    mul_sign = signhi;                     /* 9C9F/9CA1: save sign byte             */

    /* |multiplicand|: the 6502 negates {$AA:$AB} in place when it is negative.  A
       16-bit two's-complement negate of a negative value always leaves the carry
       clear, so the bit the first `ROR $AC` injects into $AC is 0 in that case;
       for a non-negative multiplicand it is the caller's entry carry. */
    uint16_t mag;
    uint8_t  ac_bit0;
    if (signhi & 0x80) {                      /* 9CA3 BMI */
        mag = (uint16_t)(-(int)(uint16_t)(mem[0x00AA] | (signhi << 8)));
        mem[0x00AA] = (uint8_t)mag; mem[0x00AB] = (uint8_t)(mag >> 8);
        ac_bit0 = 0;
    } else {
        mag = (uint16_t)(mem[0x00AA] | (signhi << 8));
        ac_bit0 = cpu.C;                      /* entry carry -> first ROR $AC          */
    }

    /* Although this is a signed multiply, the 6502 does NOT do it with a signed
       product: the multiplier ($AC, the entry A) is an UNSIGNED 8-bit fraction
       (0..255), so a signed multiply would mis-handle multipliers >= 0x80.  It
       multiplies the magnitudes and re-applies the sign separately.  Hence the
       core is an unsigned 8x16 product (one 68000 MULU, NOT MULS):
           P = multiplier * |multiplicand|   (24-bit)
       $A9/$A8 hold P>>8.  The fractional byte $AC ends up as the product's low 7
       bits (P bits 6..0) shifted up into bits 7..1, with the injected carry left
       in bit0 (P's bit7 is shifted out, never stored).  The original sign is then
       re-applied to the 16-bit {$A9:$A8}. */
    uint32_t P      = (uint32_t)m * mag;
    uint16_t prod16 = (uint16_t)(P >> 8);
    if (signhi & 0x80) prod16 = (uint16_t)(-(int)prod16);   /* 9D55/9D57 re-apply sign */

    mem[0x00A8] = (uint8_t)prod16;  mem[0x00A9] = (uint8_t)(prod16 >> 8);
    mul_div_frac = (uint8_t)(((P & 0x7F) << 1) | ac_bit0);
}

/* sine_table_lookup @ $9C55 — quarter-wave sine/cosine table lookup.
 *
 * Inputs : $0075 = angle (0..255 = full circle).  Tables in mem[]:
 *          $9B98[quad] = sign flag (0 = positive, else negate the result),
 *          $9B9C[quad] = index-reflect mask (EOR) for the descending quadrants,
 *          $4EB9[idx]  = result hi byte, $4EFA[idx] = result lo byte.
 * Outputs: $280E = quadrant (angle >> 6), $0076/$0077/$0078 = signed 24-bit value.
 *
 * The angle's top 2 bits select the quadrant; the low 6 bits index a 64-entry
 * quarter-wave table, reflected (idx ^ mask) for the two descending quadrants.
 * Negative quadrants two's-complement the 24-bit {0078:0077:0076} value.
 *
 * Contract: memory only.  Its only callers (inside trig_interp_lookup) reload
 * A/flags from $0076.. immediately after, so the 6502 exit registers are dead.
 */
void sine_table_lookup(void) {
    uint8_t angle = trig_angle;                       /* $0075: 0..255 = full circle */
    uint8_t quad  = (uint8_t)(angle >> 6);            /* top 2 bits = quadrant */
    uint8_t idx   = (uint8_t)(angle & 0x3F);          /* low 6 bits = quarter-wave index */
    mem[0x280E] = quad;                               /* $280E = quadrant (see docs/rename.md) */

    /* Reflect the index for the descending quadrants, then read the 16-bit magnitude. */
    uint8_t  y = (uint8_t)(idx ^ mem[0x9B9C + quad]);            /* $9B9C = per-quadrant reflect mask */
    uint16_t v = ROF_PAIR16(mem[0x4EFA + y], mem[0x4EB9 + y]);  /* $4EB9/$4EFA = sine table hi/lo */

    /* Store the signed 24-bit result in {$0078:$0077:$0076}; negate it in the negative quadrants. */
    uint32_t r = (mem[0x9B98 + quad] == 0) ? v                  /* $9B98 = sign: 0 = positive */
                                           : ((uint32_t)(-(int32_t)v) & 0xFFFFFFu);  /* 24-bit two's complement */
    trig_result_lo = (uint8_t)r;                      /* $0076 */
    mem[0x0077]    = (uint8_t)(r >> 8);
    mem[0x0078]    = (uint8_t)(r >> 16);
}

/* trig_interp_lookup @ $9BDB — interpolate the sine table between angle & angle+1.
 *
 * Inputs : $0075 = angle, $280D = 3-bit octant fraction.
 * Outputs: $0076/$0077/$0078 = interpolated signed 24-bit value.  Scratch:
 *          $2813-$2815 (angle sample, doubled each step), $2816-$2818 (angle+1
 *          sample, doubled each step), $280E/$280F.
 *
 * Looks up sine at angle+1 ($2816-) and at angle ($2813-, also the running
 * accumulator $0076-), then blends over 3 fraction bits: each step adds whichever
 * sample the next bit selects, then doubles both samples (24-bit <<1).
 *
 * Contract: memory only (both callers reload A/flags from $0077 after the call).
 * Calls the native sine_table_lookup (above), itself validated byte-identical.
 */
void trig_interp_lookup(void) {
    /* Sample the sine at angle+1, then at angle (the base). */
    trig_angle++;
    sine_table_lookup();
    uint32_t s1 = (uint32_t)trig_result_lo | ((uint32_t)mem[0x0077] << 8) | ((uint32_t)mem[0x0078] << 16);
    trig_angle--;
    sine_table_lookup();
    uint32_t s0  = (uint32_t)trig_result_lo | ((uint32_t)mem[0x0077] << 8) | ((uint32_t)mem[0x0078] << 16);
    uint32_t acc = s0;                   /* the interpolation accumulator starts at the angle sample */

    /* Blend across the 3 octant-fraction bits (low bit first): each step adds the selected
       sample (angle+1 if the bit is set, else angle), then doubles both samples (24-bit <<1). */
    uint8_t frac = trig_octant;          /* $280D */
    for (int step = 0; step < 3; step++) {
        acc = (acc + ((frac & 1) ? s1 : s0)) & 0xFFFFFFu;
        frac >>= 1;
        s0 = (s0 << 1) & 0xFFFFFFu;
        s1 = (s1 << 1) & 0xFFFFFFu;
    }

    /* Publish the interpolated signed 24-bit value in {$0078:$0077:$0076}, and leave the
       doubled samples ($2813-$2815 angle, $2816-$2818 angle+1) and the consumed fraction
       ($280F) in their scratch cells exactly as the 6502 does (part of the mem contract). */
    trig_result_lo = (uint8_t)acc;
    mem[0x0077] = (uint8_t)(acc >> 8);
    mem[0x0078] = (uint8_t)(acc >> 16);
    mem[0x2813] = (uint8_t)s0; mem[0x2814] = (uint8_t)(s0 >> 8); mem[0x2815] = (uint8_t)(s0 >> 16);
    mem[0x2816] = (uint8_t)s1; mem[0x2817] = (uint8_t)(s1 >> 8); mem[0x2818] = (uint8_t)(s1 >> 16);
    mem[0x280F] = frac;
}

/* compute_row_xspans @ $AD2B — per-row horizontal span endpoints.
 *
 * Inputs : $00A6 = centre seed, $00A4/$00A5 = 16-bit step (int/frac).
 * Outputs: $271E = seed; $271F..$272D (15 entries) = seed + cumulative +step
 *          going up; $270E..$271D (16 entries) = seed - cumulative step going
 *          down.  $00B5 = fractional accumulator (scratch).
 *
 * Two fixed-point ramps from the seed: a 16-bit {A4:A5} step is accumulated in
 * {value:B5}, the integer byte stored per row.  Contract: memory only (the
 * caller reloads A immediately; the exit X=$FF is dead).
 */
void compute_row_xspans(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    row_span_seed = horizon_row_index;                 /* AD2B: centre seed */

    uint8_t b5 = 0x00; mem[0x00B5] = 0x00;     /* AD30-AD32 */
    for (uint8_t x = 0; x < 0x0F; x++) {       /* AD34: upward, X=0..0x0E */
        uint16_t t = (uint16_t)b5 + scroll_accum_prev;          /* CLC; B5 += A5 */
        uint8_t c = (t > 0xFF) ? 1 : 0; b5 = (uint8_t)t; mem[0x00B5] = b5;
        t = (uint16_t)mem[MEM_row_span_seed + x] + scroll_accum_b3 + c;  /* (271E+X) + A4 + carry */
        mem[0x271F + x] = (uint8_t)t;
    }

    b5 = 0x00; mem[0x00B5] = 0x00;             /* AD48-AD4A */
    for (int xi = 0x0F; xi >= 0; xi--) {       /* AD4C: downward, X=0x0F..0 */
        uint8_t x = (uint8_t)xi;
        uint16_t t = (uint16_t)b5 + (uint8_t)~scroll_accum_prev + 1;   /* SEC; B5 -= A5 */
        uint8_t c = (t > 0xFF) ? 1 : 0; b5 = (uint8_t)t; mem[0x00B5] = b5;
        t = (uint16_t)mem[0x270F + x] + (uint8_t)~scroll_accum_b3 + c;  /* (270F+X) - A4 - borrow */
        mem[0x270E + x] = (uint8_t)t;
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* check_target_in_window @ $AC42 — latch a target index after 2 consecutive hits.
 *
 * Gated on $0036==0 and $004A!=0.  When $0063 is negative it range-tests the
 * screen coords $2912/$2913 (a wrap-around [0x0C,0xF5) band); otherwise it tests
 * world coords $0064 in [0x48,0x98) and $0066 in [0x24,0x60).  A passing frame
 * increments the hit counter $2837; the second consecutive hit latches the index
 * ($2910 in the negative branch, else 1) into $2838 and resets the counter.  Any
 * failing frame resets $2837.  Contract: memory only (caller reloads A).
 */
void check_target_in_window(void) {
    if (object_anim_frame != 0) return;              /* AC42 BNE end */
    if (joystick_saved == 0) return;              /* AC46 BEQ end */

    uint8_t x = 0;
    int latch = 0, reset = 0;

    if (object_index_signed & 0x80) {                  /* AC4A BPL -> positive; here negative */
        uint8_t a = mem[0x2912];
        if (a >= 0x0C && a < 0xF5) reset = 1;
        else {
            a = mem[0x2913];
            if (a >= 0x0C && a < 0xF5) reset = 1;
            else { x = mem[0x2910]; latch = 1; }       /* AC64 */
        }
    } else {                                   /* AC6A: $0063 >= 0 */
        uint8_t a = object_pos_x_lo;
        if (a < 0x48 || a >= 0x98) reset = 1;
        else {
            a = object_pos_y_lo;
            if (a < 0x24 || a >= 0x60) reset = 1;
            else { x = 0x01; latch = 1; }              /* AC7E */
        }
    }

    if (latch) {                               /* AC80 */
        uint8_t cnt = (uint8_t)(target_hit_streak + 1); target_hit_streak = cnt;
        if (cnt < 0x02) return;                /* AC88 BCC end (counter kept) */
        target_latched_idx = x;                       /* AC8A latch, then fall through to reset */
        reset = 1;
    }
    if (reset) target_hit_streak = 0x00;             /* AC8D */
}

/* obj_table_set_active @ $4E58 — activate the first eligible object slot.
 *
 * Scans all 256 entries of the object-flag table $0A00 in steps of $43 (which,
 * being coprime to 256, visits every index exactly once).  The first entry that
 * equals 1 AND whose index does not appear in the active-index table $2276..$22A2
 * (45 entries) is promoted to $80 and the scan returns.  $281F holds the current
 * index (scratch).  Contract: memory only (caller reloads A; exit X dead).
 */
void obj_table_set_active(void) {
    uint8_t idx = 0x00;
    do {
        obj_active_idx = idx;                     /* 4E5A */
        if (mem[0x0A00 + idx] == 0x01) {       /* 4E5D-4E62 */
            int found = 0;
            for (int xi = 0x2C; xi >= 0; xi--) /* 4E66: search active table */
                if (mem[0x2276 + xi] == obj_active_idx) { found = 1; break; }
            if (!found) { mem[0x0A00 + idx] = 0x80; return; }   /* 4E71-4E79 */
        }
        idx = (uint8_t)(idx + 0x43);           /* 4E7A: CLC; ADC #$43; TAX */
    } while (idx != 0x00);                     /* 4E81 BNE */
}

/* ring_push_0719 @ $55FF — 6502-ABI entry: push cpu.A into the ring (via ring_push_0719_core),
 * then PLA;TAX hands the caller's saved index back in A and X.  Callers (ring_push_unmarked /
 * ring_push_marked) PHA'd that index first, so the pulled value IS part of the contract — the
 * harness checks cpu.A/X/S here, not just mem[].  (Body is ring_push_0719_core, defined above.) */
void ring_push_0719(void) {
    ring_push_0719_core(cpu.A);

    cpu.S++; cpu.A = mem[0x0100 | cpu.S];       /* 5611 PLA */
    cpu.X = cpu.A;                              /* 5612 TAX */
    cpu.N = (cpu.A >> 7) & 1; cpu.Z = (cpu.A == 0) ? 1 : 0;
}

/* compute_heading_sincos @ $9BA0 — sin/cos of the 16-bit heading $2885/$2886.
 *
 * Forms an 8-bit angle in $0075 = ($2886 << 2) | ($2885 >> 6) and the octant
 * fraction $280D = ($2885 >> 3) & 7, then calls trig_interp_lookup at the angle
 * (sin -> $2809/$280A) and at angle+$40 = +90deg (cos -> $280B/$280C).
 * Contract: memory only (callers reload A).  Calls native trig_interp_lookup.
 */
void compute_heading_sincos(void) {
    uint8_t a75 = heading_hi, h = heading_lo, c;
    c = h >> 7; h = (uint8_t)(h << 1); a75 = (uint8_t)((a75 << 1) | c);   /* ASL;ROL $75 */
    c = h >> 7;                          a75 = (uint8_t)((a75 << 1) | c); /* ASL;ROL $75 */
    trig_angle = a75;
    trig_octant = (uint8_t)((heading_lo >> 3) & 0x07);

    trig_interp_lookup();                /* sin */
    mem[0x2809] = mem[0x0077];
    mem[0x280A] = mem[0x0078];

    trig_angle = (uint8_t)(trig_angle + 0x40);   /* +90 deg */
    trig_interp_lookup();                /* cos */
    mem[0x280B] = mem[0x0077];
    mem[0x280C] = mem[0x0078];
}

/* build_view_transform_matrix @ $A0A3 — two rotated view-space components.
 *
 * With sin/cos in $0089/$0087 and a 16-bit vector in $00A0:$00A1 / $00A2:$00A3,
 * computes (via four signed_mul_8x16 calls and 16-bit add/sub/negate):
 *   $22A3:$22D1 = [A1:A0]*$89 - [A3:A2]*$87
 *   $22FF:$232D = -([A3:A2]*$89 + [A1:A0]*$87)
 *
 * CARRY THREADING: native signed_mul_8x16 reads the entry carry (-> its $AC byte)
 * and does NOT modify cpu.C.  The 6502 here leaves carry untouched before the
 * first two muls (so they see build_view's entry carry) and feeds the step-2
 * subtract's carry into the last two muls.  We mirror that exactly by writing
 * cpu.C after each add/sub block.  Contract: memory (caller reloads A; final
 * carry is incidental).  Calls native signed_mul_8x16.
 *
 * build_view_transform_matrix_core_c is the SDL/validate oracle; on the Amiga a hand-asm
 * twin (BuildViewAssembler.s) replaces it via the ROF_BUILDVIEW_ASM seam below.  The asm
 * inlines the four signed_mul_8x16 calls as a single `mulu.w` each: the multiply core is an
 * UNSIGNED 8x16 product P = m*|mc| (m<=255, |mc|<=0x8000 both fit a word), so one mulu.w is
 * byte-exact — the bit-serial $9C97 loop is NOT needed (unlike mul_u8 $9821, which is a
 * round-half-up multiply and needs the g_mulTable lookup).
 */
void build_view_transform_matrix_core_c(void) {
    mem[0x00AA] = draw_iter_count; mem[0x00AB] = scroll_accum_b0;
    cpu.A = terrain_state; signed_mul_8x16();
    mem[0x22A3] = mem[0x00A8]; mem[0x22D1] = mem[0x00A9];

    mem[0x00AA] = scroll_accum_b1; mem[0x00AB] = scroll_accum_b2;
    cpu.A = vbi_phase; signed_mul_8x16();
    {   /* SEC; $22A3:$22D1 -= $A8:$A9 */
        uint8_t c = 1; uint16_t t;
        t = (uint16_t)mem[0x22A3] + (uint8_t)~mem[0x00A8] + c; c = t >> 8; mem[0x22A3] = (uint8_t)t;
        t = (uint16_t)mem[0x22D1] + (uint8_t)~mem[0x00A9] + c; c = t >> 8; mem[0x22D1] = (uint8_t)t;
        cpu.C = c & 1;                   /* feeds the next signed_mul's $AC byte */
    }

    mem[0x00AA] = scroll_accum_b1; mem[0x00AB] = scroll_accum_b2;
    cpu.A = terrain_state; signed_mul_8x16();
    mem[0x22FF] = mem[0x00A8]; mem[0x232D] = mem[0x00A9];

    mem[0x00AA] = draw_iter_count; mem[0x00AB] = scroll_accum_b0;
    cpu.A = vbi_phase; signed_mul_8x16();
    {   /* CLC; $22FF:$232D += $A8:$A9 */
        uint8_t c = 0; uint16_t t;
        t = (uint16_t)mem[0x00A8] + mem[0x22FF] + c; c = t >> 8; mem[0x22FF] = (uint8_t)t;
        t = (uint16_t)mem[0x00A9] + mem[0x232D] + c; c = t >> 8; mem[0x232D] = (uint8_t)t;
        cpu.C = c & 1;
    }
    {   /* SEC; $22FF:$232D = 0 - $22FF:$232D (16-bit negate) */
        uint8_t c = 1; uint16_t t;
        t = (uint16_t)0 + (uint8_t)~mem[0x22FF] + c; c = t >> 8; mem[0x22FF] = (uint8_t)t;
        t = (uint16_t)0 + (uint8_t)~mem[0x232D] + c; c = t >> 8; mem[0x232D] = (uint8_t)t;
        cpu.C = c & 1;
    }
}

/* Dispatcher seam (asm-migration-plan Phase 3), mirrors project_terrain_points_core.
 * On the Amiga (ROF_BUILDVIEW_ASM) build_view_transform_matrix is the hand-written m68k
 * twin in BuildViewAssembler.s; elsewhere it is the clean-C oracle above.  Memory-only
 * contract: the twin writes exactly the 10 cells below (the four output pairs $22A3:$22D1
 * and $22FF:$232D, plus signed_mul_8x16's ZP side effects $00A8-$00AD from the last call). */
#if defined(ROF_BUILDVIEW_ASM) && defined(ROF_BUILDVIEW_VERIFY)
/* On-target differential (single run, deterministic): run the asm twin on the real state,
 * snapshot the 10 cells it wrote, restore them, run the C oracle on the same inputs, compare.
 * The C oracle's output is left LIVE so flight stays correct on an asm bug.  Reads via
 * amiga/buildview_verify.gdb. */
extern void build_view_transform_matrix_asm(void);
volatile unsigned long g_bvCalls = 0, g_bvMismatch = 0, g_bvFirstBad = 0, g_bvBadAddr = 0;
volatile unsigned long g_bvAsmTicks = 0, g_bvCTicks = 0;
void build_view_transform_matrix(void) {
    g_bvCalls++;
    uint8_t* const M = (uint8_t*)mem;
    static const uint16_t cells[10] = { 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD,
                                        0x22A3, 0x22D1, 0x22FF, 0x232D };
    uint8_t snap[10], asmv[10];
    for (int i = 0; i < 10; i++) snap[i] = M[cells[i]];
    FP_TIME(build_view_transform_matrix_asm(), g_bvAsmTicks);
    for (int i = 0; i < 10; i++) asmv[i] = M[cells[i]];
    for (int i = 0; i < 10; i++) M[cells[i]] = snap[i];
    FP_TIME(build_view_transform_matrix_core_c(), g_bvCTicks);
    int bad = 0, first = !g_bvMismatch;
    for (int i = 0; i < 10; i++) if (M[cells[i]] != asmv[i]) { bad = 1; if (first) { g_bvBadAddr = cells[i]; first = 0; } }
    if (bad) { if (!g_bvMismatch) g_bvFirstBad = g_bvCalls; g_bvMismatch++; }
}
#elif defined(ROF_BUILDVIEW_ASM)
extern void build_view_transform_matrix(void);  /* BuildViewAssembler.s */
#else
void build_view_transform_matrix(void) { build_view_transform_matrix_core_c(); }
#endif

/* setup_projection_params @ $AC93 — per-frame view/projection setup for the terrain renderer.
 * Called once per frame by terrain_frame_setup, ahead of build_view_transform_matrix.  From
 * the ship's world position and attitude it derives the fixed-point inputs the projection maths
 * consumes downstream:
 *
 *   • {vbi_flags:vbi_phase} ($0088:$0087)               = world X ({world_x_hi:world_x_lo}) >> 4
 *   • {terrain_scroll_counter:terrain_state} ($008A:$0089) = world Z ({world_z_hi:world_z_lo}) >> 4
 *       — the 12.4 fixed-point map coords logically scaled down by 16 into projection units.
 *   • {terrain_scroll_reload:dl_src_index} ($008C:$008B) = pitch-depth delta
 *       {terrain_depth_step:terrain_depth_frac} << 2, but saturated to a negative hi byte ($FF)
 *       when the delta's hi byte is steep (>= $40); the low byte then keeps its raw value.
 *   • draw_row ($0092)                                   = heading_hi ($2886) << 2.
 *   • the sin/cos view vector $00A0-$00A3                = $2809-$280C, latched after
 *       compute_heading_sincos() rebuilds it for the current heading.
 *   • horizon_row_index ($00A6)                          = 6 - hi(roll {roll_pos_hi:roll_pos_lo} << 2)
 *       — the screen row the horizon sits on.
 *   • {scroll_accum_b3:scroll_accum_prev} ($00A4:$00A5)  = signed pitch
 *       {pitch_shadow_hi:pitch_shadow_lo} >> 1 (arithmetic) — the per-row pitch step.
 *   • player3_xbase ($2822)                              = clamp(pitch_shadow_hi + 4, 0..8)
 *       — an altitude/attitude index.
 *
 * Two faithfulness details preserved from the 6502 original: the pitch-delta << 2 uses ROL, so
 * when it saturates, its "hi byte >= $40" flag is what the 6502 carry-threaded into draw_row's
 * bit1 (see below); and the low bits of draw_row are exactly what ROL A;ROL A leaves.  Contract:
 * memory only (the caller reloads registers; cpu diffs are incidental).  Byte-identical to the
 * __t6502 oracle (make validate FN=setup_projection_params).
 */
void setup_projection_params(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* World X/Z scaled into projection units (16-bit logical >> 4). */
    uint16_t wx = ROF_PAIR16(world_x_lo, world_x_hi) >> 4;
    vbi_phase = (uint8_t)wx; vbi_flags = (uint8_t)(wx >> 8);
    uint16_t wz = ROF_PAIR16(world_z_lo, world_z_hi) >> 4;
    terrain_state = (uint8_t)wz; terrain_scroll_counter = (uint8_t)(wz >> 8);

    /* Pitch-depth delta << 2.  A steep delta (hi byte >= $40) saturates to a negative ($FFxx)
       result, leaving the low byte untouched.  This "steep" test is also the carry the 6502
       fed into draw_row's bit1 below, so capture it. */
    int deltaSteep = (terrain_depth_step >= 0x40);
    if (deltaSteep) {
        dl_src_index = terrain_depth_frac;              /* $008B unchanged */
        terrain_scroll_reload = 0xFF;
    } else {
        uint16_t d = (uint16_t)(ROF_PAIR16(terrain_depth_frac, terrain_depth_step) << 2);
        dl_src_index = (uint8_t)d;
        terrain_scroll_reload = (uint8_t)(d >> 8);
    }

    /* heading_hi << 2, with the exact low bits ROL A;ROL A leaves: bit0 = heading_hi bit7,
       bit1 = the "delta steep" carry threaded in from the block above. */
    draw_row = (uint8_t)((heading_hi << 2) | (deltaSteep << 1) | (heading_hi >> 7));

    /* Rebuild the sin/cos view vector for the current heading, then latch it into $00A0-$00A3. */
    compute_heading_sincos();
    draw_iter_count = mem[0x2809]; scroll_accum_b0 = mem[0x280A];   /* $00A0-$00A3 = $2809-$280C */
    scroll_accum_b1 = mem[0x280B]; scroll_accum_b2 = mem[0x280C];

    /* Horizon screen row = 6 - hi(roll << 2). */
    uint16_t roll = (uint16_t)(ROF_PAIR16(roll_pos_lo, roll_pos_hi) << 2);
    horizon_row_index = (uint8_t)(0x06 - (uint8_t)(roll >> 8));

    /* Per-row pitch step = signed pitch >> 1 (arithmetic), split hi/lo. */
    int16_t pitch     = (int16_t)ROF_PAIR16(mem[0x0023], mem[0x0024]);  /* {pitch_shadow_hi:_lo} */
    int16_t pitchHalf = (int16_t)(pitch >> 1);
    scroll_accum_b3   = (uint8_t)((uint16_t)pitchHalf >> 8);
    scroll_accum_prev = (uint8_t)pitchHalf;

    /* The 6502 preserved pitch_shadow_hi across the shift with PHA/PLA; we use a local, but the
       PHA still deposits that byte on the stack page — reproduce that lone side-effect so the
       twin stays byte-identical to the oracle (net cpu.S is unchanged). */
    mem[0x0100 | cpu.S] = mem[0x0024];

    /* Altitude/attitude index = clamp(pitch_shadow_hi + 4, 0..8): negative -> 0, >= 9 -> 8. */
    uint8_t idx = (uint8_t)(mem[0x0024] + 0x04);
    if (idx & 0x80)      idx = 0x00;
    else if (idx >= 0x09) idx = 0x08;
    player3_xbase = idx;
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* set_plot_mask_and_halve_step @ $AB7B — pick a plot base ptr + quarter the step.
 *
 * Inputs : cpu.A (low 2 bits select an entry in tables $A7E9/$A7ED).
 * Effect : $28DC/$28DD = base ptr from the tables; if the index is 0, $0058=$FF
 *          (full plot mask); {$0051:$0050} >>= 2 (step / 4).
 * Contract: memory only (callers reload A/flags).
 */
void set_plot_mask_and_halve_step(void) {
    uint8_t x = cpu.A & 0x03;
    plot_base_ptr_lo = mem[0xA7E9 + x];
    plot_base_ptr_hi = mem[0xA7ED + x];
    uint8_t a = x;                             /* TXA */
    if (x == 0) { plot_pixel_mask = 0xFF; a = 0xFF; }   /* idx 0: full mask, A=$FF */
    uint8_t c = 0;
    for (int i = 0; i < 2; i++) {              /* LSR $51; ROR $50 (x2) = >>2 */
        c = plot_step_hi & 1; plot_step_hi = (uint8_t)(plot_step_hi >> 1);
        uint8_t v = plot_step_lo; uint8_t nc = v & 1;
        plot_step_lo = (uint8_t)((v >> 1) | (c << 7));
        c = nc;
    }
    /* Replicate the transpiled exit registers: unlike the other leaves a caller
       (terrain_plot_object_a path) may use the index X without reloading it. */
    cpu.X = x; cpu.A = a; cpu.C = c;
    cpu.N = (plot_step_lo >> 7) & 1; cpu.Z = (plot_step_lo == 0) ? 1 : 0;
}

/* terrain_point_distance @ $A8AF — Manhattan distance of a point to screen centre.
 *
 * Inputs : cpu.A (saved to $290E), $004F/$004E (point), $0051 (bias), $2915 (best).
 * Effect : computes |$004F-$80| + |$80-$004E| (+$0051); if it overflows, exceeds
 *          $2915, or carries, returns early (via terrain_distance_clamp_return,
 *          left transpiled — an empty RTS).  Otherwise latches the new nearest
 *          point into $2912-$2915 + $2910/$2911 and its distance into $2914.
 * Contract: memory only (callers reload A; SEC/CLC set carry before each op so
 *           entry carry is irrelevant).
 */
void terrain_point_distance(void) {
    uint8_t A, c;
    mem[0x290E] = cpu.A;

    A = terrain_pt_coord_a; c = 1;                    /* SEC; SBC #$80 */
    { uint16_t t = (uint16_t)A + (uint8_t)~0x80 + c; c = t >> 8; A = (uint8_t)t; }
    mem[0x290B] = A;
    if (A & 0x80) {                            /* negate -> |$004F-$80| */
        c = 1; uint16_t t = (uint16_t)0 + (uint8_t)~mem[0x290B] + c; c = t >> 8; A = (uint8_t)t;
    }
    terrain_distance_acc = A;

    A = 0x80; c = 1;                           /* SEC; A=$80; SBC $004E */
    { uint16_t t = (uint16_t)A + (uint8_t)~terrain_pt_coord_b + c; c = t >> 8; A = (uint8_t)t; }
    mem[0x290C] = A;
    if (A & 0x80) {                            /* negate -> |$80-$004E| */
        c = 1; uint16_t t = (uint16_t)0 + (uint8_t)~mem[0x290C] + c; c = t >> 8; A = (uint8_t)t;
    }

    c = 0; { uint16_t t = (uint16_t)A + terrain_distance_acc + c; c = t >> 8; A = (uint8_t)t; }  /* CLC; ADC $290D */
    if (c) { terrain_distance_clamp_return(); return; }
    terrain_distance_acc = A;
    c = 0; { uint16_t t = (uint16_t)A + plot_step_hi + c; c = t >> 8; A = (uint8_t)t; }  /* CLC; ADC $0051 */
    if (c) { terrain_distance_clamp_return(); return; }
    if (A >= mem[0x2915]) { terrain_distance_clamp_return(); return; }  /* CMP $2915; BCS */

    mem[0x2915] = A;
    mem[0x2914] = terrain_distance_acc;
    mem[0x2912] = mem[0x290B];
    mem[0x2913] = mem[0x290C];
    mem[0x2910] = mem[0x290E];
    mem[0x2911] = mem[0x290F];
    terrain_distance_clamp_return();
}

/* terrain_midpoint_displace @ $B2CC — midpoint of two view-space points (the
 * fractal subdivision step).  Indexed by cpu.X into delta tables $25B4/$25D2/
 * $25F0/$24E2/$23E2; adds them to $0082-$0086, halves with sign-extension into
 * {$008E:$008D} and {$0090:$008F}, and (when the $0086 sum is negative) offsets
 * {$0090:$008F} by +-((midpoint-$0082:$0083)>>1) depending on the final carry.
 * Contract: memory only.  Faithful carry threading (the ROR/CMP idioms and the
 * 16-bit add/sub borrow chains are carry-sensitive).
 */
void terrain_midpoint_displace(void) {
    const uint8_t x = cpu.X;
    /* Native 16-bit fixed-point form of the fractal midpoint step (was 6502 byte-pair
       arithmetic with per-op carry threading).  Each midpoint = the signed value
       (base + delta + 1) arithmetic-shifted right by 1 — the SEC+ADC rounding then the
       CMP #$80 / ROR sign-extension.  The displacement = the unsigned (mid - base) >> 1
       (the LSR/ROR pair).  On uint16_t these collapse to single 68000 word ops; the byte
       pairs ($82/$83, $84/$85) are little-endian.  Bit-exact rewrite: arithmetic >>1 =
       (v>>1)|(v&0x8000); logical >>1 = v>>1.  Validated byte-identical vs the __t6502
       oracle (make validate). */
    const uint16_t base1 = (uint16_t)(dl_ptr_hi | (screen_ptr_lo << 8));   /* {$83:$82} */
    const uint16_t base2 = (uint16_t)(screen_ptr_hi | (encounter_count << 8));   /* {$85:$84} */

    const uint16_t sum1 = (uint16_t)(base1 + (uint16_t)(mem[0x25B4 + x] | (mem[0x25D2 + x] << 8)) + 1u);
    const uint16_t mid1 = (uint16_t)((sum1 >> 1) | (sum1 & 0x8000u));      /* B2CC arith >>1 */
    const uint16_t sum2 = (uint16_t)(base2 + (uint16_t)(mem[0x25F0 + x] | (mem[0x24E2 + x] << 8)) + 1u);
    uint16_t       mid2 = (uint16_t)((sum2 >> 1) | (sum2 & 0x8000u));      /* B2E0 */

    const uint16_t r86  = (uint16_t)(row_count + mem[0x23E2 + x] + 1u);  /* B2F4 $86 + delta + 1 */
    const uint8_t  d91  = (uint8_t)r86;

    step_mode_flag = (uint8_t)mid1; mem[0x008E] = (uint8_t)(mid1 >> 8);
    altitude_threshold = d91;

    if (d91 & 0x80u) {                          /* B2FC BPL: skip displacement unless $91 negative */
        const uint16_t disp = (uint16_t)((mid1 - base1) & 0xFFFFu) >> 1;   /* (mid-base) >>1 logical */
        mid2 = (r86 & 0x100u) ? (uint16_t)(mid2 + disp)    /* B31F add  (carry from the $86 add) */
                              : (uint16_t)(mid2 - disp);   /* B301 sub */
        mem[0x00B5] = (uint8_t)disp; mem[0x00B6] = (uint8_t)(disp >> 8);
    }
    sfx_toggle_8F = (uint8_t)mid2; sfx_reinit_gate = (uint8_t)(mid2 >> 8);
}

/* terrain_plot_pixel @ $A6D3 — draw one object pixel into the terrain viewport.
 *
 * The flight viewport is a 2-bits-per-pixel bitmap ("mode D"): four screen columns share
 * one byte of a raster row, each column occupying a fixed 2-bit field.  This routine sets
 * the pixel at (row, col) to an object's value class and OR-blends it over whatever terrain
 * is already there, so ground objects (gun emplacement / base / downed pilot / enemy fire)
 * draw on top of the landscape:
 *   valueMask $AA -> only the HIGH bit of each 2-bit pixel  => value-2 (object body / dot pen)
 *   valueMask $FF -> both bits                              => value-3 (highlight pen, COLPF2)
 * Rows >= $97 are below the bitmap and clipped (nothing drawn).
 *
 * Per-column geometry comes from two tables rebuilt once per frame:
 *   terrain_col_byte_offset[col] ($BD00) = which byte of the row holds this column
 *   terrain_col_pixel_mask[col]  ($BC00) = the column's HIGH-bit position ($80/$20/$08/$02)
 * The row's base address comes from the $28CA (lo) / $28FA (hi) row-address tables.
 *
 * The 6502 stashes several intermediates in ZP ($80/$81 field pointer — a scratch reuse of
 * the sync_flag/dl_ptr_lo cells; $B5 mask-build scratch; $28E2 the saved row).  Those are
 * dead after return here but the transliterated oracle leaves them, so the SDL/validate path
 * reproduces them for byte-identity.  The core takes typed args and never touches cpu, so the
 * shim preserves X and Y (callers walk them with INX/DEY); exit A/flags are dead at every call
 * site.  On the Amiga the field write is dropped (see below).
 */
static inline void terrain_plot_pixel_core(uint8_t row, uint8_t col, uint8_t valueMask) {
    if (row >= 0x97) return;                              /* below the bitmap: clip */

    /* Field row-base pointer, from the $28CA (lo) / $28FA (hi) row-address tables.
       ORDER MATTERS: the saved-row write ($28E2) must precede the table reads, because the $28CA
       table OVERLAPS $28E2 (row $18 -> $28CA+$18 == $28E2), so at row $18 the table read is meant
       to pick up the row we just stored.  The 6502 stores $28E2 first for exactly this reason;
       reading the table first would grab the stale pre-store byte. */
    terrain_plot_row = row;                               /* $28E2 (saved for the caller's Y) */
    sync_flag = mem[0x28CA + row];                        /* $80 field ptr lo */
    dl_ptr_lo = mem[0x28FA + row];                        /* $81 field ptr hi */

    const uint8_t himask = mem[MEM_terrain_col_pixel_mask + col];  /* column's HIGH-bit position */
    mem[0x00B5] = himask;                                 /* $B5 mask-build scratch (oracle leaves it) */

    /* Both bits of this column's 2-bpp pixel, narrowed to the object's value class. */
    const uint8_t pixelBits = (uint8_t)((himask >> 1) | himask) & valueMask;

#ifndef ROF_PLATFORM_AMIGA
    /* SDL/validate: OR the pixel bits into the mode-D field byte (what fill_terrain_silhouette /
       the SDL decoder read back).  Base from $80/$81 (== the oracle's ($80),Y) and route through
       bus_read/bus_write, NOT a direct mem[] RMW: the row base is a caller pointer, so a byte
       offset can reach the $D000-$D7FF hardware page where the two differ — the validate harness's
       randomized inputs exercise exactly that. */
    const uint16_t addr = (uint16_t)((sync_flag | (dl_ptr_lo << 8)) + mem[MEM_terrain_col_byte_offset + col]);
    bus_write(addr, (uint8_t)(bus_read(addr) | pixelBits));
#else
    /* Amiga: DROP the field write.  Same case as the rasterizer's ROF_FIELD_PLOT no-op —
       renderFlightDirect builds the display straight from $260E + the plane2 dot buffer and never
       reads the field body (rows 0-42) back, so the field write is dead weight (one scattered
       indirect RMW per object pixel, in the object-plot loop).  The pixel is mirrored to the
       bitplanes instead, exactly matching the field value it would have produced:
         value bit1 (set for value-2 AND value-3) -> plane2  (ROF_PLOT_DOT, rows 0-46)
         value bit0 (set only for value-3)         -> plane1  (ROF_PLOT_DOT_P1, post-fill overlay,
                                                               so value-3 shows COLPF2 not COLPF1)
       Mirroring plane1 directly here would seed spurious blitterFillUp sky streaks -> deferred. */
    if (valueMask & himask)                  ROF_PLOT_DOT(col, row);       /* -> plane2 */
    if (valueMask & (uint8_t)(himask >> 1))  ROF_PLOT_DOT_P1(col, row);    /* -> plane1 (post-fill) */
#endif
}

void terrain_plot_pixel(void) {
    terrain_plot_pixel_core(cpu.Y, cpu.X, plot_pixel_mask);   /* X/Y unchanged = the oracle's exit */
}

/* terrain_clip_row_top @ $A6CB — clip a column's top against the per-column row
 * limit $260E[X]; plot the pixel only when the scanline Y is strictly above it.
 * Contract: memory.  X/Y preserved; exit A/flags dead (callers reload).
 */
void terrain_clip_row_top(void) {
    uint8_t y = cpu.Y;                       /* TYA */
    uint8_t lim = mem[MEM_terrain_height_max + cpu.X];       /* CMP $260E,X */
    if (y < lim)  { terrain_plot_skip_return(); return; }   /* BCC skip */
    if (y == lim) { terrain_plot_skip_return(); return; }   /* BEQ skip */
    terrain_plot_pixel_core(cpu.Y, cpu.X, plot_pixel_mask);
}

/* raster_scaled_object @ $AB9A — fill a 12x32 cell grid, plotting set bits.
 *
 * Ensures the step {$0051:$0050} is nonzero; subtracts it from $1000 (counting
 * into $004F) to find the start row; then walks 12 rows x 32 columns, advancing
 * the {$0053:$0052}/{$0055:$0054} fixed-point accumulators by the step.  For each
 * cell it tests the bit AC3A[col&7] in the source buffer at ($00C3),col>>3 (the
 * column index reflected via $1F-col when $28DF is set) and, when set, plots it
 * through terrain_clip_row_top (native) at ($004F,$004E).
 *
 * THIS IS THE SCALED OBJECT BLIT, and its cost is quadratic in 1/step: the loops run
 * 0x2000/step columns by 0x0C00/step rows, capped at 32x12 = 384 cells.  Measured in combat:
 * 3 cells at depth 12, 384 at depth 0, mean 15 — so the mean says nothing about the calls that
 * matter.  A DESTROYED emplacement takes terrain_plot_object_a's `occupant >= $FA` branch, which
 * QUARTERS the step, so an explosion both passes the `< $0D` gate it used to fail and blits at up
 * to 4x the scale; the field is rendered twice per iteration.  That is the "an enemy exploding
 * close by drops the frame rate" spike.
 *
 * The per-cell work was pure transliteration: two byte-pair accumulators re-read and re-written
 * through volatile mem[] every cell, the source pointer re-assembled from ($C3),Y every cell, and
 * $4F/$4E round-tripped for the plot coordinates — ~15 volatile accesses per cell for arithmetic
 * that is single 16-bit word ops on a 68000.  All of it is carried in locals now and only the
 * state the oracle leaves behind is written back (nothing reads these cells mid-call, and none of
 * $4E-$55/$C3/$C4 is in the flight VBI's audited ZP write-set, so the hoist is ISR-safe).
 *
 * Contract: memory (the bitmap writes + the accumulators).  Exit X=$28E1 is
 * restored (callers read it); A/Y/flags are dead.  Faithful carry threading:
 * the start-row subtract chains carry ACROSS iterations (one SEC before the
 * loop, none inside), a 6502 quirk reproduced exactly.
 */
void raster_scaled_object(void) {
#ifdef ROF_OBJ_SHAPE_ON
    /* Shape counters + this call's own tick span, all in locals; flushed once at the end so the
       12x32 loops keep exactly the memory traffic they would have without the probe.  The entry
       depth is captured BEFORE the nonzero-step fixup, since that is what the caller's < $0D
       gate tested. */
    unsigned long _rows = 0, _cells = 0, _plots = 0;
    const uint8_t _step0 = plot_step_hi;
    const unsigned long _t0 = rof_subclock(), _i0 = g_isrBeamLines;
#endif

    if (plot_step_hi == 0) { plot_step_lo = 0x00; plot_step_hi++; }   /* AB9A nonzero step */

    /* Loop invariants, read ONCE.  The transliteration re-read every one of these out of
       volatile mem[] on every cell. */
    const uint16_t step     = ROF_PAIR16(plot_step_lo, plot_step_hi);          /* {$51:$50} */
    const uint16_t plotBase = ROF_PAIR16(plot_base_ptr_lo, plot_base_ptr_hi);  /* shape source */
    const uint8_t  reflect  = shape_col_base;                                          /* $28DF */

    /* ABAD start-row search: subtract the step from $1000 until it borrows, counting $4F down
       once per pass (the borrowing pass included).  The 6502's SEC sits outside the loop, but the
       loop only continues while carry is SET, so every continuing pass enters with carry=1 — this
       is a plain repeated 16-bit subtract, not a cross-iteration carry chain.  $C3/$C4 are left
       dead here: the outer loop always runs at least once and rewrites both before any read. */
    uint8_t col = terrain_pt_coord_a;
    {
        uint16_t acc = 0x1000;
        for (;;) {
            const uint8_t borrow = (acc < step);
            acc = (uint16_t)(acc - step);
            col--;                                                /* DEC $4F */
            if (borrow) break;                                    /* BCS: fall through on no-borrow */
        }
    }
    shape_row_width = col;                                        /* ABBD */

    uint8_t  row     = terrain_pt_coord_b;   /* $4E, one step down per row */
    uint16_t outer   = 0x0000;               /* {$55:$54} row accumulator */
    uint16_t inner   = 0x0000;               /* {$53:$52} column accumulator */
    uint16_t rowBase = 0x0000;               /* {$C4:$C3} this row's shape-source pointer */

    do {                                                          /* ABC8 outer (<= 12 rows) */
        /* {$C4:A} = $55 << 2, then + the shape source pointer (one 16-bit add, wrapping). */
        rowBase = (uint16_t)(plotBase + (uint16_t)((outer >> 8) << 2));
        col     = shape_row_width;                                /* faithful re-read of $28DE */
        inner   = 0x0000;

        do {                                                      /* ABEA inner (<= 32 cols) */
            uint8_t a = (uint8_t)(inner >> 8);                    /* LDA $53 */
            if (reflect != 0) a = (uint8_t)(0x1F - a);            /* $28DF set: mirror the shape */
            if (bus_read((uint16_t)(rowBase + (a >> 3))) & mem[0xAC3A + (a & 7)]) {
                cpu.X = col; cpu.Y = row;                         /* the plot's 6502-ABI args */
                terrain_clip_row_top();
#ifdef ROF_OBJ_SHAPE_ON
                _plots++;
#endif
            }
#ifdef ROF_OBJ_SHAPE_ON
            _cells++;
#endif
            col++;                                                /* INC $4F */
            inner = (uint16_t)(inner + step);
        } while ((uint8_t)(inner >> 8) < 0x20);                   /* CMP #$20; BCC */

        row--;                                                    /* DEC $4E */
        outer = (uint16_t)(outer + step);
#ifdef ROF_OBJ_SHAPE_ON
        _rows++;
#endif
    } while ((uint8_t)(outer >> 8) < 0x0C);                       /* CMP #$0C; BCS done */

    /* Write back exactly the exit state the transliterated oracle leaves in mem[]. */
    mem[0x0052] = (uint8_t)inner;   mem[0x0053] = (uint8_t)(inner >> 8);
    mem[0x0054] = (uint8_t)outer;   mem[0x0055] = (uint8_t)(outer >> 8);
    row_table_base_lo = (uint8_t)rowBase;  row_table_base_hi = (uint8_t)(rowBase >> 8);
    terrain_pt_coord_a = col;
    terrain_pt_coord_b = row;

#ifdef ROF_OBJ_SHAPE_ON
    { const unsigned long _d = rof_subclock() - _t0, _id = g_isrBeamLines - _i0;
      RS_SHAPE(_step0, _rows, _cells, _plots, (_d > _id) ? (_d - _id) : 0); }
#endif
    cpu.X = terrain_cur_obj_idx;                                          /* AC36: LDX $28E1 */
}

/* terrain_plot_object_a @ $A822 — plot one terrain object (gun tower etc.), variant A.
 *
 * Indexed by cpu.X into the object tables.  Early-outs if the slot is busy
 * ($2487/$242D nonzero).  Sets the source ptr $28DC/$28DD, step {$0051:$0050},
 * screen pos $004E/$004F; if the object's flag byte $0A00[$2276[X]] >= $FA it is
 * a "mask" object (set_plot_mask), else it plots a marker pixel (point_distance +
 * clip_row_top) and may latch a targeting record (gated on $0034/$006A/$003E/
 * RANDOM/$28ED).  Finally, when the step's hi byte < $0D, fills via
 * raster_scaled_object.  Reads POKEY RANDOM once.  All callees native/empty.
 * Contract: memory; exit X=$28E1 (restored by the tail raster_scaled_object or the
 * explicit a868 path); other regs dead.
 */
void terrain_plot_object_a(void) {
    if (mem[0x2487 + cpu.X] != 0) { OP_CNT(g_opaBusy); terrain_obj_skip_return(); return; }   /* A822 */
    if (mem[0x242D + cpu.X] != 0) { OP_CNT(g_opaBusy); terrain_obj_skip_return(); return; }   /* A827 */
    plot_base_ptr_lo = 0xF9; plot_base_ptr_hi = 0xA6;
    plot_pixel_mask = 0xFF;
    plot_step_hi = mem[0x232E + cpu.X];
    plot_step_lo = mem[0x2300 + cpu.X];
    terrain_pt_coord_b = mem[0x245A + cpu.X];
    cpu.Y = terrain_pt_coord_b;                          /* TAY */
    terrain_pt_coord_a = mem[0x2400 + cpu.X];
    cpu.X = mem[0x2276 + cpu.X];                  /* LDA $2276,X; TAX */
    cpu.A = mem[0x0A00 + cpu.X];

    if (cpu.A >= 0xFA) {                           /* CMP #$FA; BCC a860 */
        OP_CNT(g_opaMask);
        set_plot_mask_and_halve_step();
    } else {                                       /* a860 */
        cpu.A = cpu.Y;                             /* TYA */
        cpu.X = terrain_pt_coord_a;                       /* LDX $4F */
        if (cpu.A < mem[MEM_terrain_height_max + cpu.X]) { OP_CNT(g_opaBelow); cpu.X = terrain_cur_obj_idx; return; }  /* CMP 260E,X; BCC a868 */
        OP_CNT(g_opaDist);
        cpu.A = 0x80; terrain_point_distance();    /* a86c-a86e */
        cpu.X--; terrain_clip_row_top(); cpu.X++;  /* DEX; a872; INX */
        if (terrain_depth_step < 0x37) {                  /* CMP #$37; BCS a8a1 */
            OP_CNT(g_opaFire);
            mem[0x28FC] = 0x01; mem[0x28FB] = 0x01;
            /* gates: $6A negative, $3E==0, RANDOM negative, $28ED==0 (short-circuit
               matches the 6502: the $D20A read only happens if the prior gates pass) */
            if ((mem[0x006A] & 0x80) && clear_colors_done_003E == 0 &&
                (bus_read(0xD20A) & 0x80) && mem[0x28ED] == 0) {
                mem[0x28EB] = cpu.X; mem[0x28EC] = cpu.Y;
                mem[0x28ED] = plot_step_hi;
                CL_CNT(g_clEnemyFire);           /* an emplacement queued a bolt at us */
            }
        }
    }
    /* L_a8a1 */
    if (plot_step_hi >= 0x0D) { OP_CNT(g_opaDepth); cpu.X = terrain_cur_obj_idx; return; }   /* CMP #$0D; BCS a868 */
    shape_col_base = 0x00;
    CL_OBJ(plot_step_hi);                        /* combat-load: a ground object is drawn */
    raster_scaled_object();
}

/* terrain_plot_object_b @ $A90A — plot one terrain object, variant B (4 plot points).
 *
 * Like A822 but: column-reflect flag $28DF/$290F from $23B5[X]&1; uses point at
 * ($004E vs $260E[$004F]); after the fill it may additionally stamp a 2x2 cross of
 * pixels (clip_row_top x4 around $2400[X]/$245A[X]) when $00A7==0 && $003E==0 and
 * the object flag is in [2,$F8).  No RANDOM read.  All callees native/empty.
 * Contract: memory; exit X=$28E1; other regs dead.
 */
void terrain_plot_object_b(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    if (mem[0x2487 + cpu.X] != 0) { terrain_distance_clamp_return(); return; }   /* A90A */
    if (mem[0x242D + cpu.X] != 0) { terrain_distance_clamp_return(); return; }   /* A90F */
    plot_base_ptr_lo = 0xF1; plot_base_ptr_hi = 0xA7;
    cpu.A = (uint8_t)(mem[0x23B5 + cpu.X] & 0x01);            /* LDA $23B5,X; AND #1 */
    mem[0x290F] = cpu.A; shape_col_base = cpu.A;
    terrain_pt_coord_a = mem[0x2400 + cpu.X];
    terrain_pt_coord_b = mem[0x245A + cpu.X];
    plot_step_hi = mem[0x232E + cpu.X];
    plot_step_lo = mem[0x2300 + cpu.X];
    cpu.Y = mem[0x2276 + cpu.X];                  /* LDY $2276,X */
    cpu.A = mem[0x0A00 + cpu.Y];
    if (cpu.A >= 0xFA) set_plot_mask_and_halve_step();         /* CMP #$FA; BCC a94a */

    cpu.Y = terrain_pt_coord_a;                          /* L_a94a: LDY $4F */
    cpu.A = terrain_pt_coord_b;                          /* LDA $4E */
    if (cpu.A >= mem[MEM_terrain_height_max + cpu.Y]) {           /* CMP 260E,Y; BCC a965 */
        cpu.A = 0x00; terrain_point_distance();   /* a953-a955 */
        plot_pixel_mask = 0xAA;
        if (plot_step_hi < 0x0D) { CL_OBJ(plot_step_hi); raster_scaled_object(); }  /* CMP #$0D; BCS a965 */
    }

    /* L_a965 */
    if (mem[0x00A7] == 0 && clear_colors_done_003E == 0) {   /* BNE a998 (both) */
        cpu.Y = mem[0x2276 + cpu.X];              /* LDY $2276,X */
        cpu.A = mem[0x0A00 + cpu.Y];
        if (cpu.A >= 0x02 && cpu.A < 0xF8) {      /* CMP #2 BCC; CMP #$F8 BCS -> [2,$F8) */
            OP_CNT(g_opbCross);
            plot_pixel_mask = 0xFF;
            cpu.Y = mem[0x245A + cpu.X];          /* LDY $245A,X */
            cpu.X = mem[0x2400 + cpu.X];          /* LDA $2400,X; TAX */
            terrain_clip_row_top();               /* a986 */
            cpu.X--; cpu.Y--; terrain_clip_row_top();   /* DEX; DEY; a98b */
            cpu.X--; cpu.Y++; terrain_clip_row_top();   /* DEX; INY; a990 */
            cpu.X++; cpu.Y++; terrain_clip_row_top();   /* INX; INY; a995 */
        }
    }
    /* L_a998 */
    cpu.X = terrain_cur_obj_idx;
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* terrain_plot_object @ $A63B — per-object terrain raster dispatch.
 *
 * Indexed by entry cpu.X.  Early-outs on an empty object ($0A00[$2276[X]]==0) or
 * a too-near zero-distance object.  Saves X to $28E1.  If the object's $0900 flag
 * is negative it routes to the A822 plotter; otherwise (for object codes in
 * [2,$F8) with $00A7==0) it computes the half-width into $0059/$005A (ABS via the
 * sign bit, rounded) and the span coords $28E4-$28EA/$0079, then falls through to
 * the A90A plotter.  Tail-calls native terrain_plot_object_b (the trailing
 * terrain_clip_row_top in the transpile is dead code after the return).
 * Contract: memory; carry threaded through the width arithmetic.
 */
void terrain_plot_object(void) {
    uint8_t A, c;
    cpu.Y = mem[0x2276 + cpu.X];                              /* LDY $2276,X */
    if (mem[0x0A00 + cpu.Y] == 0) { OP_CNT(g_opEmpty); terrain_plot_return(); return; }   /* a641 BEQ */
    /* Combat-load: every cell that has an OCCUPANT, i.e. all real object work.  Counted here and
     * not at raster_scaled_object because the plotters do most of their work BEFORE that call
     * (distance, row clip, the half-width span, the RANDOM fire-queue gate) and skip it entirely
     * for depth >= $0D.  Counting only the raster calls is what produced the misleading "ground
     * objects on screen are nearly free" reading — that counter saw ~1 per iteration while this
     * one sees the whole population. */
    CL_CNT(g_clObjEnter);
    if (mem[0x232E + cpu.X] == 0 && mem[0x2300 + cpu.X] < 0x22) {      /* a646/a64b */
        OP_CNT(g_opStep); terrain_plot_return(); return;
    }
    terrain_cur_obj_idx = cpu.X;                                      /* a64f STX $28E1 */
    if (mem[0x0900 + cpu.Y] & 0x80) { OP_CNT(g_opPathA); terrain_plot_object_a(); return; }  /* a655 BPL -> N set: A822 */
    OP_CNT(g_opPathB);

    int go = 0;                                              /* a65a: take the a66c path? */
    if (mem[0x00A7] == 0) {
        uint8_t v = mem[0x0A00 + cpu.Y];
        if (v >= 0x02 && v < 0xF8) go = 1;                   /* CMP #2 BCC; CMP #$F8 BCC a66c */
    }
    if (go) {
        A = mem[0x22D2 + cpu.X];                             /* a66c half-width */
        mem[0x0059] = A; mem[0x005A] = A;
        c = A >> 7;                                          /* ASL A -> carry = sign bit */
        if (c) {                                             /* negative: $5A = $FF - $5A */
            uint16_t t = (uint16_t)0xFF + (uint8_t)~mem[0x005A] + c; c = t >> 8; mem[0x005A] = (uint8_t)t;
            c = 1;                                           /* SEC */
        }
        { uint8_t v = mem[0x0059], nc = v & 1;               /* ROR $59 */
          mem[0x0059] = (uint8_t)((v >> 1) | (c << 7)); c = nc; }
        if (c) mem[0x0059]++;                                /* round */

        A = mem[0x232E + cpu.X]; mem[0x28E4] = A;            /* a683 */
        if (A >= mem[0x005A]) {                              /* CMP $5A; BCC a6c8 */
            c = 0; { uint16_t t = (uint16_t)A + mem[0x005A] + c; c = t >> 8; A = (uint8_t)t; }  /* CLC ADC $5A */
            if (A < terrain_span_max) {                            /* CMP $28EA; BCS a6c8 */
                terrain_span_max = A;
                mem[0x005A] = (uint8_t)(mem[0x005A] >> 1);    /* LSR $5A */
                c = 1; { uint16_t t = (uint16_t)A + (uint8_t)~mem[0x005A] + c; c = t >> 8; A = (uint8_t)t; }
                mem[0x0079] = A;                              /* SEC; SBC $5A; STA $79 */
                mem[0x28E7] = mem[0x0059];
                mem[0x28E8] = mem[0x28E4];
                grid_slot_index = mem[0x2276 + cpu.X];
                mem[0x0040] = 0x64;
                if (mem[0x242D + cpu.X] == 0 && mem[0x2487 + cpu.X] == 0) {   /* both BNE a6c8 */
                    mem[0x003F] = mem[0x2400 + cpu.X];
                    mem[0x0040] = mem[0x245A + cpu.X];
                }
            }
        }
    }
    terrain_plot_object_b();                                      /* a6c8 tail */
}

/* terrain_column_rasterize @ $B33D — draw ONE terrain-surface segment into the silhouette
 * bitmap by recursive midpoint subdivision.
 *
 * The flight terrain is a fractal height-field.  terrain_subdivide_column produces a sparse
 * set of surface control points; this routine traces the surface CONTOUR (the terrain/sky
 * silhouette edge) between two of them — ONE pixel per screen column, NOT an area fill.  The
 * solid area fill (sky above the skyline, terrain body below) is a separate later pass,
 * fill_terrain_silhouette ($AE53).  On entry:
 *   - the running cursor {col,height,frac} is the segment's LEFT end;
 *   - control-point slot [0] (CTL_*[0]) is the RIGHT end (column / height / fraction).
 * It bisects the column interval down to single columns and plots one surface pixel at each:
 *     midpoint column = (left.col + right.col) / 2
 *     midpoint height = (left.height + right.height) / 2, then — once the running sub-pixel
 *                       fraction rolls past a bit — displaced up or down by half the remaining
 *                       horizontal span (the fractal roughness step), saturating at 0 / $FF.
 * Midpoints are pushed onto a small stack (the CTL_* arrays indexed by `depth`; each slot's
 * +1 neighbour doubles as the parent's interpolated output).  Every plotted pixel is OR'd
 * into the mode-D bitmap and recorded in the per-column max-height map (COL_MAX); a column is
 * drawn only if its new surface rises ABOVE what is already there — painter's hidden-surface,
 * so nearer terrain in front stays visible.  Heights clamp to $97.
 *
 * Two phases: (1) LEFT-CLIP — the cursor may start left of the viewport edge ($2C); bisect and
 * fast-forward it to the edge, discarding off-screen midpoints.  (2) TRACE — walk columns right
 * to the right edge ($D4), bisecting toward each control point and plotting the surface pixel of
 * the final one or two columns of each leaf before popping to the next point.
 *
 * Args: entryDepth = the bisection-stack depth to start at (the 6502 entry cpu.Y); colBase =
 * the caller's column base, saved to $60 (the 6502 entry cpu.X) and not otherwise used.
 * Contract: memory only.  Validated bit-exact against the 6502 oracle from a real flight
 * snapshot (random mem[] would not terminate — the control columns must be a realistic
 * ascending set).
 */
void terrain_column_rasterize_core_c(uint8_t entryDepth, uint8_t colBase) {
    TDCNT(g_tdRasterCalls);
    /* Non-volatile alias: the control-point stack, the max-height map and the plot tables are
       all main-loop-owned (the flight VBI touches none of them — CLAUDE.md ZP audit), so
       dropping the volatile barrier lets the compiler forward the bisection's push-then-read
       recurrence.  Writes land at the SAME addresses -> mem[] residue byte-identical. */
    uint8_t* const M = (uint8_t*)mem;
    /* Surface cursor + plot scratch held in registers across the recursion; flushed to ZP at
       every exit (WB) so the state the validator diffs is byte-identical (spills are dead). */
    uint8_t col    = dl_ptr_hi;       /* $82: current column (segment left end / running col)  */
    uint8_t height = screen_ptr_hi;   /* $84: surface height at the cursor                      */
    uint8_t frac   = row_count;       /* $86: sub-pixel fraction accumulator (the slope carry)  */
    uint8_t rowLo  = sync_flag;       /* $80/$81: bitmap row address of the last pixel plotted  */
    uint8_t rowHi  = dl_ptr_lo;
    uint8_t b5     = mem[0x00B5];      /* $B5: observable scratch (last displacement / depth)    */
    uint8_t depth  = entryDepth;      /* bisection-stack depth (= control-point index)          */
    uint8_t plotCol;                  /* the screen column currently being drawn                */

    #define CTL_COL(d)    M[MEM_blit_color_src + (d)]      /* $95[]: stack of control-point columns    */
    #define CTL_HEIGHT(d) M[MEM_terrain_ctl_height + (d)]  /* $EA[]: their heights                     */
    #define CTL_FRAC(d)   M[MEM_terrain_ctl_frac + (d)]    /* $F4[]: their fractions                   */
    #define COL_MAX(c)    M[MEM_terrain_height_max + (c)]  /* $260E[]: topmost height drawn per column */
    #define WB() do { dl_ptr_hi=col; screen_ptr_hi=height; row_count=frac; \
                      sync_flag=rowLo; dl_ptr_lo=rowHi; mem[0x00B5]=b5; } while(0)
    /* Plot the surface pixel at plotCol/h, keeping only the topmost height per column.  The
       height indexes the height->bitmap-row tables; the column gives the byte offset + bit
       mask.  A column whose height saturates at $97 is flagged $FF ("full") in the max map. */
    #define DRAW(h) do { uint8_t _h=(h); TDCNT(g_tdRasDraw); \
        if (_h > COL_MAX(plotCol)) { \
            uint8_t _oldMax = COL_MAX(plotCol);  /* the column's previous top — now a confirmed body dot */ \
            COL_MAX(plotCol) = _h; \
            if (_h >= 0x97) { COL_MAX(plotCol) = 0xFF; _h = 0x97; } \
            TDCNT(g_tdPlots); b5 = depth; \
            ROF_FIELD_PLOT(_h); /* SDL/validate: OR value-2 into the mode-D field (the dots source) */ \
            RSSAT(_h); RSDOT(plotCol, _oldMax); \
            ROF_PLOT_DOT(plotCol, _oldMax); /* Amiga: lag-plot the PREVIOUS top into plane2 (see below) */ \
            (void)_oldMax;                  /* both readers are no-ops on the host */ \
        } } while(0)

    mem[0x0060] = colBase;                   /* save the caller's column base ($60) */

    const uint8_t endCol = CTL_COL(0);       /* right endpoint of this segment */
    if (endCol < 0x2D) { RSCNT(g_rasBail); WB(); return; }     /* endpoint left of the viewport -> nothing on screen */
    if (endCol < col)  { RSCNT(g_rasBail); WB(); return; }     /* endpoint behind the cursor    -> empty segment     */
    if (endCol == col) {                     /* one column wide -> plot it and done */
        plotCol = endCol;
        DRAW(CTL_HEIGHT(0));
        WB(); return;
    }

    /* ---- phase 1: left-clip.  Bisect the cursor->endpoint span; advance the cursor onto each
       midpoint that is still off-screen, and push the first one that lands in-view, until the
       cursor reaches the viewport's left edge ($2C).  The midpoint height is interpolated the
       same way as the fill below: average of the two ends, then (once the fraction rolls) a
       roughness displacement of half the remaining horizontal span.  Note the two paths take
       that span from a different base — the advance path has already moved the cursor to the
       midpoint, the push path has not. ---- */
    depth = 0;
    for (;;) {
        if (col >= 0x2C) { plotCol = col;
                           RSHIST(g_rasSpanHist, (unsigned)(uint8_t)(CTL_COL(depth) - col)); break; }
        const uint8_t mid   = (uint8_t)(((unsigned)col + CTL_COL(depth)) >> 1);
        const unsigned fsum = (unsigned)frac + CTL_FRAC(depth) + 1u;       /* fraction accumulate (9-bit) */
        const unsigned havg = ((unsigned)height + CTL_HEIGHT(depth)) >> 1; /* height midpoint */
        if (mid <= 0x2C) {                               /* midpoint still off-screen: take it as the cursor */
            RSCNT(g_rasPh1Adv);
            col  = mid;
            frac = (uint8_t)fsum;
            if (!(frac & 0x80)) height = (uint8_t)havg;  /* fraction not yet rolled: no roughness */
            else {
                const uint8_t disp = (uint8_t)((uint8_t)(CTL_COL(depth) - col) >> 1);  /* half remaining span */
                b5 = disp;
                if (fsum >= 0x100u) { unsigned t = havg + disp; height = (t > 0xFF) ? 0xFF : (uint8_t)t; }  /* up, sat $FF */
                else height = (havg >= disp) ? (uint8_t)(havg - disp) : 0;                                /* down, floor 0 */
            }
        } else {                                         /* midpoint in-view: push it as control point depth+1 */
            RSCNT(g_rasPh1Push);
            CTL_COL(depth + 1)  = mid;
            CTL_FRAC(depth + 1) = (uint8_t)fsum;
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)havg;
            else {
                const uint8_t disp = (uint8_t)((uint8_t)(mid - col) >> 1);
                b5 = disp;
                if (fsum >= 0x100u) { unsigned t = havg + disp; mh = (t > 0xFF) ? 0xFF : (uint8_t)t; }
                else mh = (havg >= disp) ? (uint8_t)(havg - disp) : 0;
            }
            CTL_HEIGHT(depth + 1) = mh;
            depth++;
        }
    }

    /* ---- phase 2: trace.  Walk columns rightward, plotting one surface pixel per column.
       `gap` = plotCol - control-point column,
       which wraps just below the control point: $FF = one column short, $FE = two short.
         gap < $FE  -> still far: bisect (push an interpolated midpoint) and stay put;
         gap == $FE -> plot the two remaining columns (interpolated, then endpoint) and pop;
         gap == $FF -> plot the endpoint column and pop.
       A pop drops to the parent control point and restores its fraction.  Done at the right
       edge ($D4) or when the stack underflows. ---- */
    for (;;) {
        if (plotCol >= 0xD4) { WB(); return; }           /* past the right edge */
        col = plotCol;                                   /* the cursor tracks the running column (disp base) */
        const uint8_t gap = (uint8_t)(plotCol - CTL_COL(depth));
        if (gap == 0xFE) {                               /* two columns short: fill both, then pop */
            RSCNT(g_rasFe);
            DRAW((uint8_t)(((unsigned)CTL_HEIGHT(depth) + height + 1u) >> 1));  /* interpolated column */
            plotCol++;
            height = CTL_HEIGHT(depth);
            DRAW(height);                                /* endpoint column */
            plotCol++;
            if (depth-- == 0) { WB(); return; }              /* pop; underflow -> done */
            frac = CTL_FRAC(depth + 1);                       /* restore this leaf's midpoint fraction ($F5[depth]) */
        } else if (gap == 0xFF) {                        /* one column short: plot endpoint, pop */
            RSCNT(g_rasFf);
            height = CTL_HEIGHT(depth);
            DRAW(height);
            plotCol++;
            if (depth-- == 0) { WB(); return; }
            frac = CTL_FRAC(depth + 1);                       /* $F5[depth] */
        } else {                                         /* far: bisect, push an interpolated midpoint */
            TDCNT(g_tdRasBisect);
            RSHIST(g_rasFarHist, (unsigned)(uint8_t)(CTL_COL(depth) - plotCol));
            const uint8_t mid   = (uint8_t)(((unsigned)plotCol + CTL_COL(depth)) >> 1);
            CTL_COL(depth + 1)  = mid;
            const unsigned fsum = (unsigned)frac + CTL_FRAC(depth) + 1u;
            CTL_FRAC(depth + 1) = (uint8_t)fsum;
            const unsigned hsum = (unsigned)height + CTL_HEIGHT(depth);   /* 9-bit; its LSB rounds the disp */
            const unsigned havg = hsum >> 1;
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)havg;             /* no roughness */
            else if (fsum >= 0x100u) {                                   /* roughness up */
                const uint8_t disp = (uint8_t)((uint8_t)(mid - col) >> 1);
                b5 = disp;
                unsigned t = havg + disp + (hsum & 1u);
                mh = (t > 0xFF) ? 0xFF : (uint8_t)t;
            } else {                                                    /* roughness down */
                const uint8_t disp = (uint8_t)((uint8_t)(mid - col - 1u) >> 1);
                b5 = disp;
                unsigned t = havg + (unsigned)(uint8_t)~disp + (hsum & 1u);
                mh = (t > 0xFF) ? (uint8_t)t : 0;
            }
            RSOCCL((unsigned)(uint8_t)(CTL_COL(depth) - plotCol), mh, CTL_HEIGHT(depth),
                   height, plotCol, &COL_MAX(plotCol));
            CTL_HEIGHT(depth + 1) = mh;
            depth++;
        }
    }

    #undef CTL_COL
    #undef CTL_HEIGHT
    #undef CTL_FRAC
    #undef COL_MAX
    #undef WB
    #undef DRAW
}

/* Dispatcher seam (asm-migration-plan Phase 2).  The clean-C body above
 * (terrain_column_rasterize_core_c) is the validation/SDL oracle; on the Amiga,
 * when ROF_RASTERIZE_ASM is set, terrain_column_rasterize_core is the hand-written
 * m68k twin in TerrainRasterizeAssembler.s (link-swapped, plain C linkage — no
 * SAS/C wrapper since it's our own function).  Verified byte-identical to the
 * oracle by headless render-diff of terrainBitmap + the plane2 dot buffer
 * (the deterministic auto-flight).  noinline so the call site binds to whichever
 * implementation is linked rather than inlining the C body into the caller. */
#if defined(ROF_RASTERIZE_ASM) && defined(ROF_RASTERIZE_VERIFY)
/* On-target differential check (single run, deterministic): each call runs the asm
 * twin on the real state, snapshots the regions it wrote, restores them, runs the C
 * oracle on the same inputs, and compares.  The C oracle's output is left LIVE so the
 * flight stays correct regardless of an asm bug.  g_rasterMismatch / g_rasterFirstBad /
 * g_rasterCalls are read via gdb (raster_verify.gdb).  Compares $260E (the per-column
 * max-height output), the $82/$84/$86/$60 writeback, and the plane2 dot buffer. */
extern void terrain_column_rasterize_core_asm(uint8_t entryDepth, uint8_t colBase);
extern uint8_t* g_flightDotPlane;
volatile unsigned long g_rasterCalls = 0, g_rasterMismatch = 0, g_rasterFirstBad = 0;
/* Same-run perf comparison: bracket the asm and the C oracle back-to-back on the same
 * inputs (FP_TIME subtracts ISR beam-lines).  Snapshot/restore is OUTSIDE the brackets,
 * so these tally only the two implementations' own beam-ticks — directly comparable in a
 * single run (sidesteps the cross-run VBI/main-loop interleave nondeterminism). */
volatile unsigned long g_rasAsmTicks = 0, g_rasCTicks = 0;
#define RAS_HMAP 0xD8          /* $260E[0..0xD7] max-height window (cols + slack) */
#define RAS_DOT  5120          /* plane2 dot window: kRow120[42]+39 < 5120        */
static uint8_t ras_snapH[RAS_HMAP], ras_asmH[RAS_HMAP];
static uint8_t ras_snapZ[4],        ras_asmZ[4];
static uint8_t ras_snapD[RAS_DOT],  ras_asmD[RAS_DOT];
static void ras_cpy(uint8_t* d, const uint8_t* s, unsigned n) { while (n--) *d++ = *s++; }
static int  ras_cmp(const uint8_t* a, const uint8_t* b, unsigned n) { while (n--) if (*a++ != *b++) return 1; return 0; }
void terrain_column_rasterize_core(uint8_t entryDepth, uint8_t colBase) {
    g_rasterCalls++;
    uint8_t* const M = (uint8_t*)mem;
    uint8_t* const dp = g_flightDotPlane;
    ras_cpy(ras_snapH, &M[0x260E], RAS_HMAP);
    ras_snapZ[0]=M[0x82]; ras_snapZ[1]=M[0x84]; ras_snapZ[2]=M[0x86]; ras_snapZ[3]=M[0x60];
    if (dp) ras_cpy(ras_snapD, dp, RAS_DOT);
    FP_TIME(terrain_column_rasterize_core_asm(entryDepth, colBase), g_rasAsmTicks);
    ras_cpy(ras_asmH, &M[0x260E], RAS_HMAP);
    ras_asmZ[0]=M[0x82]; ras_asmZ[1]=M[0x84]; ras_asmZ[2]=M[0x86]; ras_asmZ[3]=M[0x60];
    if (dp) ras_cpy(ras_asmD, dp, RAS_DOT);
    ras_cpy(&M[0x260E], ras_snapH, RAS_HMAP);
    M[0x82]=ras_snapZ[0]; M[0x84]=ras_snapZ[1]; M[0x86]=ras_snapZ[2]; M[0x60]=ras_snapZ[3];
    if (dp) ras_cpy(dp, ras_snapD, RAS_DOT);
    FP_TIME(terrain_column_rasterize_core_c(entryDepth, colBase), g_rasCTicks);
    int bad = ras_cmp(&M[0x260E], ras_asmH, RAS_HMAP);
    if (M[0x82]!=ras_asmZ[0] || M[0x84]!=ras_asmZ[1] || M[0x86]!=ras_asmZ[2] || M[0x60]!=ras_asmZ[3]) bad = 1;
    if (dp && ras_cmp(dp, ras_asmD, RAS_DOT)) bad = 1;
    if (bad) { if (!g_rasterMismatch) g_rasterFirstBad = g_rasterCalls; g_rasterMismatch++; }
}
#elif defined(ROF_RASTERIZE_ASM)
extern void terrain_column_rasterize_core(uint8_t entryDepth, uint8_t colBase); /* TerrainRasterizeAssembler.s */
#else
__attribute__((noinline)) void terrain_column_rasterize_core(uint8_t entryDepth, uint8_t colBase) {
    terrain_column_rasterize_core_c(entryDepth, colBase);
}
#endif

/* 6502-ABI shim: entry cpu.Y = start depth, cpu.X = column base. */
void terrain_column_rasterize(void) { terrain_column_rasterize_core(cpu.Y, cpu.X); }

/* terrain_subdivide_column @ $B172 — the coarse fractal LOD pass over one terrain segment.
 *
 * Works in (column, height) space.  A "point" is a 16-bit column, a 16-bit height, and an
 * 8-bit fraction.  The running `span` is one endpoint of the current segment; the other
 * endpoint sits on a small stack (STK_*[depth]).  The routine recursively bisects the
 * segment — each midpoint is the average of the two endpoints, with the height nudged by a
 * fraction-gated roughness displacement (same scheme as terrain_column_rasterize, one level
 * coarser) — and for each leaf a CASCADE decides what to do:
 *   - SUBDIVIDE further  : the segment is still wide AND steep -> push the midpoint, descend
 *   - RASTERIZE the leaf : it is narrow (<$14 cols) or shallow -> call terrain_column_rasterize
 *                          to fill its pixels
 *   - SKIP               : a degenerate / behind-the-camera segment
 * Recursion is bounded by a budget ($9F, starts $14) and a stack depth < $0F.
 *
 * Three phases: (1) ENTRY GUARD — bail if the span column is already at/past the segment's
 * far endpoint.  (2) DESCEND — bisect, pulling the near end inward or pushing midpoints, until
 * the column high byte settles non-negative.  (3) LEAF + UNWIND — run the cascade on each
 * leaf, then pop the stack and repeat.
 *
 * Args: startDepth = the stack index to start at (the 6502 entry cpu.X, normally 0);
 * rasterEntryDepth = forwarded as terrain_column_rasterize's start depth (the 6502 entry cpu.Y,
 * left untouched here).  Contract: memory.  Validated bit-exact against the 6502 oracle from a
 * real flight snapshot (it drives terrain_column_rasterize, which random mem[] can't terminate).
 */
/* ---- terrain_subdivide_column_core sub-point stacks -----------------------------------------
 * The recursion works on "sub-points": a terrain endpoint = {column (16-bit), height (16-bit),
 * fraction (8-bit)}.  Parent endpoints awaiting processing live on a stack indexed by recursion
 * depth, kept as 5 PARALLEL byte arrays in main RAM (struct-of-arrays) at the fixed bases below
 * — scratch the flight VBI never touches.  We model one sub-point as a struct and load/store a
 * whole slot at once; reads go through the non-volatile mem alias so the compiler drops any
 * field a caller doesn't use (no extra loads vs. the old hand-indexed access). */
typedef struct { uint16_t col, hgt; uint8_t frac; } SubPt;
/* The 16-bit column/height live as parallel LO/HI byte arrays in mem[]; load/store split the
   word byte-wise (little-endian, explicit — endianness-neutral) at the mem[] boundary, but the
   recursion's arithmetic runs fully 16-bit (one 68000 word op instead of two byte ops). */
#define SUBPT_COL_LO 0x25B4
#define SUBPT_COL_HI 0x25D2
#define SUBPT_HGT_LO 0x25F0
#define SUBPT_HGT_HI 0x24E2
#define SUBPT_FRAC   0x23E2
static inline SubPt subpt_load(const uint8_t *M, int depth) {
    SubPt p;
    p.col  = (uint16_t)(M[SUBPT_COL_LO + depth] | (M[SUBPT_COL_HI + depth] << 8));
    p.hgt  = (uint16_t)(M[SUBPT_HGT_LO + depth] | (M[SUBPT_HGT_HI + depth] << 8));
    p.frac = M[SUBPT_FRAC + depth];
    return p;
}
static inline void subpt_store(uint8_t *M, int depth, SubPt p) {
    M[SUBPT_COL_LO + depth] = (uint8_t)p.col; M[SUBPT_COL_HI + depth] = (uint8_t)(p.col >> 8);
    M[SUBPT_HGT_LO + depth] = (uint8_t)p.hgt; M[SUBPT_HGT_HI + depth] = (uint8_t)(p.hgt >> 8);
    M[SUBPT_FRAC   + depth] = p.frac;
}
/* Bisect the segment [span..far]: the midpoint is the signed average of the two endpoints, with
   the midpoint HEIGHT displaced by half the column span when the summed fraction rolls past a bit
   (the fractal roughness step), saturating in 16-bit.  $B5/$B6 hold the displacement when applied
   (observable residue). */
static inline SubPt subdiv_midpoint(SubPt span, SubPt far, uint8_t *M) {
    TDCNT(g_tdMidpoints);
    uint16_t colSum = (uint16_t)(span.col + far.col + 1u);
    uint16_t midCol = (uint16_t)((colSum >> 1) | (colSum & 0x8000u));   /* signed average */
    uint16_t hgtSum = (uint16_t)(span.hgt + far.hgt + 1u);
    uint16_t midHgt = (uint16_t)((hgtSum >> 1) | (hgtSum & 0x8000u));
    uint16_t fracSum = (uint16_t)(span.frac + far.frac + 1u);
    SubPt mid;
    SDCNT(g_sdMid);
    mid.frac = (uint8_t)fracSum;
    mid.col  = midCol;
    if (mid.frac & 0x80u) {                                  /* roughness: displace the height */
        SDCNT(g_sdRough);
        uint16_t disp = (uint16_t)(midCol - span.col) >> 1;
        midHgt = (fracSum >= 0x100u) ? (uint16_t)(midHgt + disp) : (uint16_t)(midHgt - disp);
        M[0x00B5] = (uint8_t)disp; M[0x00B6] = (uint8_t)(disp >> 8);
    }
    mid.hgt = midHgt;
    return mid;
}

#ifdef ROF_SUBDIV_DEFER_RESIDUE
/* DEFERRED EXIT RESIDUE — the twin's `sd_out` (and the object entry's guard bail) parks the span
 * and midpoint HERE with one MOVEM.W instead of writing them byte-swapped into mem[$82-$86] /
 * mem[$8D-$91] on each of the ~68 subdivide calls an iteration.  28 cycles against 100 for the
 * span (two 22-cycle `ror.w #8` plus three absolute stores) and 28 against 132 for the midpoint.
 *
 * Layout mirrors what MOVEM.W d2-d4 / d5-d7 writes — the LOW WORD of each register, in the
 * 68000's native big-endian order, so no byte swapping happens on the hot path at all:
 *   [0] span.col  [1] span.hgt  [2] span.frac   (d4's high byte is dirty by design; ignored)
 *   [3] mid.col   [4] mid.hgt   [5] mid.frac
 *
 * terrain_draw_objects seeds this from mem[] before its loop and writes it back after, so at
 * every point OUTSIDE the loop mem[] holds exactly the residue the per-call flush left.  The
 * midpoint half is still written CONDITIONALLY (only when a midpoint actually ran), which is
 * what makes the seed load-bearing: on a call that computes none, the scratch must already hold
 * what mem[$8D-$91] holds, exactly as the old code relied on mem[] already holding it.
 * See the DEFER note in TerrainSubdivideAssembler.s for the reader survey that licenses this. */
uint16_t g_sdResidue[6] __attribute__((aligned(4)));
void rof_subdiv_residue_seed(void) {
    g_sdResidue[0] = (uint16_t)(ROF_PAIR16(dl_ptr_hi, screen_ptr_lo));
    g_sdResidue[1] = (uint16_t)(ROF_PAIR16(screen_ptr_hi, encounter_count));
    g_sdResidue[2] = row_count;
    g_sdResidue[3] = (uint16_t)(ROF_PAIR16(step_mode_flag, mem[0x008E]));
    g_sdResidue[4] = (uint16_t)(ROF_PAIR16(sfx_toggle_8F, sfx_reinit_gate));
    g_sdResidue[5] = altitude_threshold;
}
void rof_subdiv_residue_publish(void) {
    dl_ptr_hi      = (uint8_t)g_sdResidue[0]; screen_ptr_lo   = (uint8_t)(g_sdResidue[0] >> 8);
    screen_ptr_hi  = (uint8_t)g_sdResidue[1]; encounter_count = (uint8_t)(g_sdResidue[1] >> 8);
    row_count      = (uint8_t)g_sdResidue[2];
    step_mode_flag = (uint8_t)g_sdResidue[3]; mem[0x008E]     = (uint8_t)(g_sdResidue[3] >> 8);
    sfx_toggle_8F  = (uint8_t)g_sdResidue[4]; sfx_reinit_gate = (uint8_t)(g_sdResidue[4] >> 8);
    altitude_threshold = (uint8_t)g_sdResidue[5];
}
#define SD_RESIDUE_SEED()    rof_subdiv_residue_seed()
#define SD_RESIDUE_PUBLISH() rof_subdiv_residue_publish()
#else
#define SD_RESIDUE_SEED()    ((void)0)
#define SD_RESIDUE_PUBLISH() ((void)0)
#endif

/* SubPt slot 0 = the companion endpoint's projected vector.  Under ROF_SUBDIV_OBJ_SEED0 the
 * shipping caller no longer writes this on every visible pair (5 memory-to-memory MOVE.B, 20
 * cycles each, 68.1 pairs an iteration): the asm twin's depth-0 arm reads the far endpoint
 * straight out of obj1's object arrays and only materialises slot 0 when the recursion descends.
 * This helper is what remains — terrain_draw_objects publishes it ONCE per pass for the last
 * visible pair, and the differential harness runs it around BOTH arms so the C oracle keeps its
 * slot-0 view (docs/flight-perf-log.md §24.2, and §17.3 for why widening the oracle's view beats
 * narrowing the comparison). */
#ifdef ROF_SUBDIV_OBJ_SEED0
static void rof_subdiv_seed0(uint8_t obj) {
    ROF_MEM_VIEW uint8_t *const M = (ROF_MEM_VIEW uint8_t *)mem;
    M[0x25B4] = M[0x2400 + obj];   /* col lo  */
    M[0x25D2] = M[0x242D + obj];   /* col hi  */
    M[0x25F0] = M[0x245A + obj];   /* hgt lo  */
    M[0x24E2] = M[0x2487 + obj];   /* hgt hi  */
    M[0x23E2] = M[0x23B5 + obj];   /* frac    */
}
#define SD_SEED0(obj) rof_subdiv_seed0(obj)

#ifdef ROF_SEED0_VERIFY
/* Does publishing slot 0 ONCE PER PASS leave what the per-pair seed used to?
 *
 * No differential can answer this and neither can a cross-build run: nothing in the linked image
 * reads these five cells (survey, §24.2), and two builds that differ in render speed fly
 * different ground within a few frames (§19), so the cells legitimately diverge.  A probe that
 * re-read obj1's vector at the end would also be vacuous — that is what the publish itself does.
 *
 * So: shadow the vector AT THE TIME OF EACH PAIR (which is what the eager seed captured) and
 * compare the shadow to what the publish left.  That tests the actual claim — that obj1's
 * projected vector cannot move between its pair and the end of the pass, because the only
 * writers are project_terrain_points (gated by the $24B4 bit-4 already-projected flag) and
 * terrain_frame_setup (long returned).  `make SEED0_VERIFY=1 PROBES=1` + amiga/seed0_verify.gdb. */
volatile unsigned long g_seed0Passes = 0, g_seed0Mismatch = 0, g_seed0BadIdx = 0;
volatile unsigned long g_seed0BadWant = 0, g_seed0BadGot = 0;
static uint8_t seed0_shadow[5];
static int     seed0_have;
static const uint16_t seed0_src[5] = { 0x2400, 0x242D, 0x245A, 0x2487, 0x23B5 };
static const uint16_t seed0_dst[5] = { 0x25B4, 0x25D2, 0x25F0, 0x24E2, 0x23E2 };
static void rof_seed0_shadow(uint8_t obj) {
    ROF_MEM_VIEW uint8_t *const M = (ROF_MEM_VIEW uint8_t *)mem;
    for (int i = 0; i < 5; i++) seed0_shadow[i] = M[seed0_src[i] + obj];
    seed0_have = 1;
}
static void rof_seed0_check(void) {
    ROF_MEM_VIEW uint8_t *const M = (ROF_MEM_VIEW uint8_t *)mem;
    if (!seed0_have) return;
    g_seed0Passes++;
    for (int i = 0; i < 5; i++)
        if (M[seed0_dst[i]] != seed0_shadow[i]) {
            if (!g_seed0Mismatch) { g_seed0BadIdx = i; g_seed0BadWant = seed0_shadow[i]; g_seed0BadGot = M[seed0_dst[i]]; }
            g_seed0Mismatch++;
        }
    seed0_have = 0;
}
#define SD_SEED0_SHADOW(obj) rof_seed0_shadow(obj)
#define SD_SEED0_CHECK()     rof_seed0_check()
#else
#define SD_SEED0_SHADOW(obj) ((void)0)
#define SD_SEED0_CHECK()     ((void)0)
#endif

#else
#define SD_SEED0(obj) ((void)0)
#define SD_SEED0_SHADOW(obj) ((void)0)
#define SD_SEED0_CHECK()     ((void)0)
#endif

/* Returns the final recursion depth (the 6502 left it in X); callers that care put it in cpu.X.
 * This clean-C body is the SDL/validate oracle; on the Amiga the hand-asm twin
 * (TerrainSubdivideAssembler.s) replaces it via the ROF_SUBDIV_ASM seam below. */
uint8_t terrain_subdivide_column_core_c(uint8_t startDepth, uint8_t rasterEntryDepth) {
    TDCNT(g_tdSubdivCalls);
    /* Non-volatile alias for the sub-point stacks (main-RAM scratch the flight VBI never touches)
       so the compiler may cache/forward across the recursion; writes land at the same addresses
       -> mem[] residue byte-identical. */
    uint8_t* const M = (uint8_t*)mem;
    int depth      = startDepth;     /* current sub-point stack index (recursion depth)    */
    uint8_t budget = 0;              /* $9F: remaining recursion budget (set to $14 below) */
    /* The running span endpoint and the last computed midpoint live in registers across the
       recursion, flushed back to ZP at every exit (label `out:`) for byte-identical residue. */
    SubPt span = { (uint16_t)(dl_ptr_hi | (screen_ptr_lo << 8)),                     /* $82:$83 column */
                   (uint16_t)(screen_ptr_hi | (encounter_count << 8)), row_count };  /* $84:$85 height, $86 frac */
    SubPt mid  = { (uint16_t)(step_mode_flag | (mem[0x008E] << 8)),                  /* $8D:$8E column */
                   (uint16_t)(sfx_toggle_8F | (sfx_reinit_gate << 8)), altitude_threshold }; /* $8F:$90 height, $91 frac */

    /* Entry guard: signed-compare the span column against the segment's far endpoint (stack[0]);
       if the span already sits at/past it there is nothing to subdivide.  $B5 keeps the
       sign-flipped endpoint high byte (observable residue). */
    SubPt far0 = subpt_load(M, 0);
    SDCNT(g_sdCalls);
    mem[0x00B5] = (uint8_t)((far0.col >> 8) ^ 0x80);
    if ((int16_t)span.col >= (int16_t)far0.col) {
        SDCNT(g_sdBail);
        return (uint8_t)depth;                            /* span >= far endpoint -> done */
    }
    budget = 0x14;
    /* SHAPE: is the far endpoint at the current depth already in registers?  True exactly
       after a push (we just wrote that slot from `mid`); false at entry and after a pop. */
    int farKnown = 0; (void)farKnown;

    /* Descend: bisect repeatedly.  If the midpoint column is "near" (negative, or < $28) pull the
       span's near end onto it and keep bisecting; otherwise push the midpoint as a parent
       sub-point and go one level deeper.  Stop once the span column settles non-negative (segment
       now in the $00xx column range).  Bounded by the recursion budget and a stack depth < $0F. */
    for (;;) {
        if (!(span.col & 0x8000)) break;
        if (budget-- == 0) goto out;                      /* budget exhausted */
        if (farKnown) SDCNT(g_sdP2Known);   /* submid's far load here is redundant */
        mid = subdiv_midpoint(span, subpt_load(M, depth), M);
        if ((mid.col & 0x8000) || mid.col < 0x28) {
            SDCNT(g_sdP2Adopt);
            span = mid;                                   /* near midpoint: adopt it as the span */
            /* far@depth is unchanged by an adopt, so it stays known if it already was */
        } else {
            SDCNT(g_sdP2Push);
            subpt_store(M, depth + 1, mid);               /* push the midpoint, descend */
            if (++depth >= 0x0F) goto out;                /* stack full */
            farKnown = 1;                                 /* far@newdepth == the mid we just wrote */
        }
    }

    /* Leaf + unwind.  For each leaf (span column now in $00xx and < $D8): run the cascade to
       decide skip / subdivide / rasterize; rasterize fills the leaf via the pixel renderer.
       Then pop the parent endpoint off the stack and repeat. */
    for (;;) {
        if (span.col > 0xFF)  goto out;                   /* span column escaped $00xx -> done */
        if (span.col >= 0xD8) goto out;                   /* span column out of range -> done  */

        int rasterize    = 0;
        int recurseAgain = 0;        /* steep leaf: re-enter the subdivide path next iteration */
        for (;;) {
            SubPt far = subpt_load(M, depth);             /* the segment's far endpoint */
            SDCNT(g_sdInner);
            if (farKnown) SDCNT(g_sdInnerFarKnown);       /* this load_far is redundant */
            farKnown = 0;
            SDCNT(g_sdDepthHist[depth & 0x0F]);
            /* Subdivide while the far endpoint's column still escapes the $00xx range; once it is
               in $00xx, fall to the cascade. */
            int subdivide = recurseAgain ? (recurseAgain = 0, 1) : (far.col > 0xFF);
            if (subdivide && far.col > 0xFF) SDCNT(g_sdFarEsc);  /* far.hgt just loaded is dead here */
            if (subdivide) {
                if (budget-- == 0) goto out;              /* budget exhausted */
                mid = subdiv_midpoint(span, far, M);
                subpt_store(M, depth + 1, mid);           /* push the midpoint */
                if (++depth >= 0x0F) goto out;            /* stack full */
                farKnown = 1;                             /* far@newdepth == the mid we just wrote */
                continue;
            }

            /* CASCADE: decide skip / subdivide / rasterize.  Picks which height to judge by (the
               span's vs the far sub-point's) from sign + magnitude vs the $6C threshold, then
               applies a width/steepness test.  "low" = negative or < $6C. */
            const int spanLow = (span.hgt & 0x8000) || span.hgt < 0x6C;
            SDCLASS(span.hgt, far.hgt);
            int doWidthTest = 0;       /* run the width/steepness test below */
            int useSpanHeight = 0;     /* test the span height (1) or the far sub-point height (0) */
            if (spanLow) {             /* judge by the far sub-point height, default to skip */
                if (far.hgt & 0x8000) { /* skip */ }
                else if (far.hgt > 0xFF) { doWidthTest = 1; useSpanHeight = 1; }
                else if (far.hgt < 0x6C) { /* skip */ }
                else { doWidthTest = 1; useSpanHeight = 1; }
            } else {                   /* judge by the far sub-point height, default to rasterize */
                if (far.hgt & 0x8000) { doWidthTest = 1; }
                else if (far.hgt > 0xFF) rasterize = 1;
                else if (far.hgt < 0x6C) { doWidthTest = 1; }
                else rasterize = 1;
            }
            if (doWidthTest) {
                /* Width/steepness: width = (far column - span column), low byte.  Narrow (<$14
                   cols) -> rasterize.  Otherwise rasterize only if the chosen height is shallower
                   than width/4 (height - width/4 >= 0); a steeper leaf subdivides further. */
                const uint8_t width = (uint8_t)(far.col - span.col);
                if (width < 0x14) rasterize = 1;
                else {
                    const uint8_t q = (uint8_t)(width >> 2); mem[0x00B5] = q;  /* $B5 observable */
                    const uint16_t hgt = useSpanHeight ? span.hgt : far.hgt;
                    if (!((uint16_t)(hgt - q) & 0x8000u)) rasterize = 1;      /* shallow -> rasterize */
                    else { SDCNT(g_sdSteep); recurseAgain = 1; continue; }   /* steep -> subdivide */
                }
            }
            break;   /* decision made (skip or rasterize) */
        }

        if (rasterize) SDCNT(g_sdRas); else SDCNT(g_sdSkip);
        if (rasterize) {
            /* Set up the pixel renderer's control point [0] from the leaf's far endpoint
               (column / height / fraction), clamping each 16-bit height to 0 or $FF by its
               sign, then fill the leaf's columns. */
            if (span.hgt > 0xFF)
                span.hgt = (span.hgt & 0xFF00) | ((span.hgt & 0x8000) ? 0x00 : 0xFF);
            SubPt leaf = subpt_load(M, depth);
            uint8_t leafHgt = (leaf.hgt > 0xFF) ? ((leaf.hgt & 0x8000) ? 0x00 : 0xFF) : (uint8_t)leaf.hgt;
            terrain_ctl_height = leafHgt;              /* $EA[0]: control-point height   */
            blit_color_src     = (uint8_t)leaf.col;    /* $95[0]: control-point column   */
            terrain_ctl_frac   = leaf.frac;            /* $F4[0]: control-point fraction */
            /* terrain_column_rasterize consumes the span $82/$84/$86 and rewrites them via its
               own write-back; flush the span before the call and reload those three after
               ($83/$85 it never touches). */
            dl_ptr_hi = (uint8_t)span.col; screen_ptr_lo = (uint8_t)(span.col >> 8);
            screen_ptr_hi = (uint8_t)span.hgt; encounter_count = (uint8_t)(span.hgt >> 8); row_count = span.frac;
            RCPRE((uint8_t)span.col, (uint8_t)span.hgt, blit_color_src, terrain_ctl_height);
            TDSPAN(terrain_column_rasterize_core(rasterEntryDepth, (uint8_t)depth), g_tdRaster);
            RCPOST();
            span.col = (span.col & 0xFF00) | dl_ptr_hi;        /* rasterizer rewrote the low bytes */
            span.hgt = (span.hgt & 0xFF00) | screen_ptr_hi;
            span.frac = row_count;
        }

        /* pop the parent endpoint off the stack and continue with it */
        if (depth == 0) goto out;
        SDCNT(g_sdPop);
        span = subpt_load(M, depth);
        depth--;                       /* far@depth-1 was NOT just written -> stays unknown */
    }

out:
    /* Flush the register-resident span + midpoint back to ZP (byte-identical residue), stash the
       remaining budget ($9F, via draw_row_bottom), and return the recursion depth. */
    dl_ptr_hi = (uint8_t)span.col; screen_ptr_lo = (uint8_t)(span.col >> 8);
    screen_ptr_hi = (uint8_t)span.hgt; encounter_count = (uint8_t)(span.hgt >> 8); row_count = span.frac;
    step_mode_flag = (uint8_t)mid.col; mem[0x008E] = (uint8_t)(mid.col >> 8);
    sfx_toggle_8F = (uint8_t)mid.hgt; sfx_reinit_gate = (uint8_t)(mid.hgt >> 8); altitude_threshold = mid.frac;
    draw_row_bottom = budget;
    return (uint8_t)depth;
}
/* Dispatcher seam (asm-migration-plan Phase 3), mirrors terrain_column_rasterize_core.
 * On the Amiga (ROF_SUBDIV_ASM) terrain_subdivide_column_core is the hand-written m68k
 * twin in TerrainSubdivideAssembler.s; elsewhere it is the clean-C oracle above. */
#if defined(ROF_SUBDIV_ASM) && defined(ROF_SUBDIV_VERIFY)
/* On-target differential check (single run, deterministic).  Snapshot everything the
 * subdivide writes, run the asm twin, capture its outputs, restore, run the C oracle on
 * the same inputs, compare.  The C oracle's output is left LIVE (flight stays correct on
 * an asm bug).  Compared: the 5 SubPt stacks ($23E2/$24E2/$25B4/$25D2/$25F0, 16 each),
 * the ZP residue ($60 + $80-$91 + $9F + $B5/$B6) and the return value.  We do NOT snapshot
 * $260E / the dot plane: subdivide's own control flow is independent of them (it works in
 * column/height space from the stacks), and terrain_column_rasterize's COL_MAX writes are
 * idempotent under the repeated identical calls a correct asm twin makes. */
extern uint8_t terrain_subdivide_column_core_asm(uint8_t startDepth, uint8_t rasterEntryDepth);
volatile unsigned long g_subdivCalls = 0, g_subdivMismatch = 0, g_subdivFirstBad = 0;
volatile unsigned long g_subdivAsmTicks = 0, g_subdivCTicks = 0;
/* First-mismatch detail: kind 1=return, 2=stack, 3=zp; idx = which; asm/c = the values. */
volatile unsigned long g_subdivBadKind = 0, g_subdivBadIdx = 0, g_subdivBadAsm = 0, g_subdivBadC = 0;
volatile unsigned long g_subdivBadRetA = 0, g_subdivBadRetC = 0;
#define SUBV_STK 5
static const uint16_t subv_stkBase[SUBV_STK] = { 0x23E2, 0x24E2, 0x25B4, 0x25D2, 0x25F0 };
static uint8_t subv_snapS[SUBV_STK][16], subv_asmS[SUBV_STK][16];
/* ZP residue window: $60, then $80..$91, $9F, $B5, $B6 (16 discrete bytes). */
static const uint16_t subv_zp[16] = { 0x60, 0x80,0x81,0x82,0x83,0x84,0x85,0x86,
                                      0x8D,0x8E,0x8F,0x90,0x91, 0x9F, 0xB5,0xB6 };
static uint8_t subv_snapZ[16], subv_asmZ[16];
/* Split into the four phases so BOTH entries (the mem[]-contract core and the object-indexed
   one below) get a real differential without duplicating the snapshot/compare. */
/* ⚠ The deferred-residue twin does not write $82-$86 / $8D-$91 itself — terrain_draw_objects
   publishes them once per pass.  Rather than drop those ten cells from the compare window (which
   is EXACTLY the blindness that let a previous residue attempt read 0 mismatch over 5104 calls
   while breaking three live consumers), the harness runs the same seed/publish pair around each
   call: seed before the asm arm so a call that computes no midpoint inherits the right value,
   publish before capturing so the compare still tests all sixteen ZP bytes. */
static void subv_snapshot(void) {
    uint8_t* const M = (uint8_t*)mem;
    for (int s = 0; s < SUBV_STK; s++) for (int i = 0; i < 16; i++) subv_snapS[s][i] = M[subv_stkBase[s] + i];
    for (int i = 0; i < 16; i++) subv_snapZ[i] = M[subv_zp[i]];
    SD_RESIDUE_SEED();
}
static void subv_capture_and_restore(void) {
    uint8_t* const M = (uint8_t*)mem;
    SD_RESIDUE_PUBLISH();
    for (int s = 0; s < SUBV_STK; s++) for (int i = 0; i < 16; i++) subv_asmS[s][i] = M[subv_stkBase[s] + i];
    for (int i = 0; i < 16; i++) subv_asmZ[i] = M[subv_zp[i]];
    for (int s = 0; s < SUBV_STK; s++) for (int i = 0; i < 16; i++) M[subv_stkBase[s] + i] = subv_snapS[s][i];
    for (int i = 0; i < 16; i++) M[subv_zp[i]] = subv_snapZ[i];
}
static void subv_compare(uint8_t asmRet, uint8_t cRet) {
    uint8_t* const M = (uint8_t*)mem;
    int bad = 0, first = !g_subdivMismatch;
    if (cRet != asmRet) { bad = 1; if (first) { g_subdivBadKind=1; g_subdivBadRetA=asmRet; g_subdivBadRetC=cRet; } }
    for (int s = 0; s < SUBV_STK; s++) for (int i = 0; i < 16; i++)
        if (M[subv_stkBase[s] + i] != subv_asmS[s][i]) {
            bad = 1;
            if (first && g_subdivBadKind != 2) { g_subdivBadKind=2; g_subdivBadIdx=(unsigned long)s*16+i; g_subdivBadAsm=subv_asmS[s][i]; g_subdivBadC=M[subv_stkBase[s]+i]; }
        }
    for (int i = 0; i < 16; i++)
        if (M[subv_zp[i]] != subv_asmZ[i]) {
            bad = 1;
            if (first && g_subdivBadKind != 2 && g_subdivBadKind != 3) { g_subdivBadKind=3; g_subdivBadIdx=subv_zp[i]; g_subdivBadAsm=subv_asmZ[i]; g_subdivBadC=M[subv_zp[i]]; }
        }
    if (bad) { if (first) g_subdivFirstBad = g_subdivCalls; g_subdivMismatch++; }
}
uint8_t terrain_subdivide_column_core(uint8_t startDepth, uint8_t rasterEntryDepth) {
    g_subdivCalls++;
    subv_snapshot();
    uint8_t asmRet;
    FP_TIME(asmRet = terrain_subdivide_column_core_asm(startDepth, rasterEntryDepth), g_subdivAsmTicks);
    subv_capture_and_restore();
    uint8_t cRet;
    FP_TIME(cRet = terrain_subdivide_column_core_c(startDepth, rasterEntryDepth), g_subdivCTicks);
    subv_compare(asmRet, cRet);
    return cRet;
}
#elif defined(ROF_SUBDIV_ASM)
extern uint8_t terrain_subdivide_column_core(uint8_t startDepth, uint8_t rasterEntryDepth); /* TerrainSubdivideAssembler.s */
#else
__attribute__((noinline)) uint8_t terrain_subdivide_column_core(uint8_t startDepth, uint8_t rasterEntryDepth) {
    return terrain_subdivide_column_core_c(startDepth, rasterEntryDepth);
}
#endif

/* terrain_subdivide_column_obj — the OBJECT-INDEXED entry to the same routine.
 *
 * terrain_draw_objects seeds the running span from the primary endpoint's projected vector.  It
 * did that by copying 5 bytes THROUGH mem[$82-$86] (five memory-to-memory MOVE.B, ~120 cycles)
 * so the subdivide prologue could load them straight back out — the rasterizer span-handoff
 * shape (asm-migration-plan Phase 5) one level up.  Passing the object id instead lets the
 * callee load the span from $2400/$242D/$245A/$2487/$23B5 directly, and (d16,a0) is cheaper
 * than the absolute-long loads it replaces.  ~68 calls/iteration => ~0.9% of flight wall clock.
 *
 * This C version IS the oracle for the new ABI's observable behaviour, and deliberately so: it
 * is literally the old caller code — seed the 5 cells, then run the mem[]-contract entry.  The
 * asm twin skips the seeding and instead publishes those 5 bytes on the ONE path where they are
 * observable (the entry-guard bail, which unlike every other exit does not flush the span).
 */
static inline uint8_t terrain_subdivide_column_obj_c(uint8_t startDepth, uint8_t rasterEntryDepth,
                                                    uint8_t obj0) {
    volatile const uint8_t *o0 = mem + obj0;
    mem[MEM_dl_ptr_hi]       = o0[0x2400];
    mem[MEM_screen_ptr_lo]   = o0[0x242D];
    mem[MEM_screen_ptr_hi]   = o0[0x245A];
    mem[MEM_encounter_count] = o0[0x2487];
    mem[MEM_row_count]       = o0[0x23B5];
#if defined(ROF_SUBDIV_ASM) && defined(ROF_SUBDIV_VERIFY)
    return terrain_subdivide_column_core_c(startDepth, rasterEntryDepth);   /* stay on the oracle */
#else
    return terrain_subdivide_column_core(startDepth, rasterEntryDepth);
#endif
}
/* ⚠ ROF_SUBDIV_OBJ1ARG: the shipping ABI passes obj0 ALONE.  `startDepth` is the literal 0x00
 * at the one call site below and `rasterEntryDepth` is dead under the rasterizer's register ABI
 * (terrain_column_rasterize_core_c above assigns it to `depth` and then does `depth = 0` before
 * any read), so both were pure call overhead: a `move.l` + a `clr.l -(sp)` + wider stack
 * clean-up in the caller and the arg decode in the callee, ~64 cycles over 68 calls/iteration.
 * The flag is tied to RASTER_SPAN_ABI, which is exactly the condition that kills rasterEntryDepth.
 * The oracle keeps all three parameters — it IS the general routine — and is handed the two
 * constants. */
/* ⚠ ROF_SUBDIV_OBJ_SEED0 adds obj1 — see SD_SEED0 above.  The oracle needs no such argument:
 * it reads the far endpoint from slot 0, which the harness (or, off-Amiga, the caller) seeds. */
#ifdef ROF_SUBDIV_OBJ_SEED0
#define SUBDIV_OBJ(startDepth, rasEnt, obj0, obj1) terrain_subdivide_column_obj(obj0, obj1)
#elif defined(ROF_SUBDIV_OBJ1ARG)
#define SUBDIV_OBJ(startDepth, rasEnt, obj0, obj1) terrain_subdivide_column_obj(obj0)
#else
#define SUBDIV_OBJ(startDepth, rasEnt, obj0, obj1) terrain_subdivide_column_obj(startDepth, rasEnt, obj0)
#endif

#if defined(ROF_SUBDIV_ASM) && defined(ROF_SUBDIV_VERIFY)
/* Same in-process differential as the core entry, so the FAST path is the one under test.
 * (`make VERIFY=1 NO_RASTER_VERIFY=1 PROBES=1` + subdiv_verify.gdb — plain VERIFY=1 sends the
 * nested rasterize calls through the C dispatcher, i.e. not this routine's fast path.  That
 * build also has no RASTER_SPAN_ABI, hence no OBJ1ARG, so each VERIFY config exercises the same
 * ABI its shipping counterpart would.) */
#ifdef ROF_SUBDIV_OBJ_SEED0
extern uint8_t terrain_subdivide_column_obj_asm(uint8_t obj0, uint8_t obj1);
uint8_t terrain_subdivide_column_obj(uint8_t obj0, uint8_t obj1) {
    g_subdivCalls++;
    subv_snapshot();
    /* ⚠ The shipping caller no longer seeds slot 0, so the harness must — for BOTH arms, not
       just the oracle's.  Narrowing the compare window instead is exactly the blindness that
       let a previous residue attempt read 0 mismatch over 5104 calls while breaking three live
       consumers (§17.3).  With the seed here, all 16 entries of all 5 stacks stay compared AND
       the differential is testing the thing that actually changed: that reading the far endpoint
       out of obj1's arrays gives what reading slot 0 gave. */
    SD_SEED0(obj1);
    uint8_t asmRet;
    FP_TIME(asmRet = terrain_subdivide_column_obj_asm(obj0, obj1), g_subdivAsmTicks);
    subv_capture_and_restore();
    SD_SEED0(obj1);
    uint8_t cRet;
    FP_TIME(cRet = terrain_subdivide_column_obj_c(0x00, 0x00, obj0), g_subdivCTicks);
    subv_compare(asmRet, cRet);
    return cRet;
}
#elif defined(ROF_SUBDIV_OBJ1ARG)
extern uint8_t terrain_subdivide_column_obj_asm(uint8_t obj0);
uint8_t terrain_subdivide_column_obj(uint8_t obj0) {
    g_subdivCalls++;
    subv_snapshot();
    uint8_t asmRet;
    FP_TIME(asmRet = terrain_subdivide_column_obj_asm(obj0), g_subdivAsmTicks);
    subv_capture_and_restore();
    uint8_t cRet;
    FP_TIME(cRet = terrain_subdivide_column_obj_c(0x00, 0x00, obj0), g_subdivCTicks);
    subv_compare(asmRet, cRet);
    return cRet;
}
#else
extern uint8_t terrain_subdivide_column_obj_asm(uint8_t startDepth, uint8_t rasterEntryDepth,
                                                uint8_t obj0);
uint8_t terrain_subdivide_column_obj(uint8_t startDepth, uint8_t rasterEntryDepth, uint8_t obj0) {
    g_subdivCalls++;
    subv_snapshot();
    uint8_t asmRet;
    FP_TIME(asmRet = terrain_subdivide_column_obj_asm(startDepth, rasterEntryDepth, obj0), g_subdivAsmTicks);
    subv_capture_and_restore();
    uint8_t cRet;
    FP_TIME(cRet = terrain_subdivide_column_obj_c(startDepth, rasterEntryDepth, obj0), g_subdivCTicks);
    subv_compare(asmRet, cRet);
    return cRet;
}
#endif
#elif defined(ROF_SUBDIV_OBJ_ABI)
#ifdef ROF_SUBDIV_OBJ_SEED0
extern uint8_t terrain_subdivide_column_obj(uint8_t obj0, uint8_t obj1); /* TerrainSubdivideAssembler.s */
#elif defined(ROF_SUBDIV_OBJ1ARG)
extern uint8_t terrain_subdivide_column_obj(uint8_t obj0);            /* TerrainSubdivideAssembler.s */
#else
extern uint8_t terrain_subdivide_column_obj(uint8_t startDepth, uint8_t rasterEntryDepth,
                                            uint8_t obj0);   /* TerrainSubdivideAssembler.s */
#endif
#else
/* SDL / SUBDIV_C=1: the C oracle above is the implementation. */
static inline uint8_t terrain_subdivide_column_obj(uint8_t startDepth, uint8_t rasterEntryDepth,
                                                  uint8_t obj0) {
    return terrain_subdivide_column_obj_c(startDepth, rasterEntryDepth, obj0);
}
#endif

#undef SUBPT_COL_LO
#undef SUBPT_COL_HI
#undef SUBPT_HGT_LO
#undef SUBPT_HGT_HI
#undef SUBPT_FRAC
/* 6502-ABI shim: entry cpu.X = start depth, cpu.Y = the rasterizer's start depth. */
void terrain_subdivide_column(void) { cpu.X = terrain_subdivide_column_core(cpu.X, cpu.Y); }

/* terrain_jitter_column @ $A613 — add a small random wobble to the active object's position.
 *
 * Writes two random jitter offsets (3 POKEY RANDOM reads total):
 *   - {$0068:$2829} = RANDOM + RANDOM; the add's carry forms the high byte, inverted when the
 *     ship is in the near half of the column range (object_pos_x_lo < $6C) — a signed X wobble.
 *   - {$0069:$282C} = RANDOM - $80, sign-extended (high byte $FF when the result is negative)
 *     — a signed Y wobble centred on 0.
 * Contract: memory only.  Reads POKEY RANDOM (the harness seeds it identically per run); no
 * hardware writes (the original's tail RTS at terrain_plot_return is a no-op and dropped). */
void terrain_jitter_column(void) {
    uint8_t r1 = bus_read(0xD20A);
    uint8_t r2 = bus_read(0xD20A);
    uint16_t sum = (uint16_t)r1 + r2;
    mem[0x2829] = (uint8_t)sum;
    uint8_t carry = (uint8_t)(sum >> 8);
    mem[0x0068] = (object_pos_x_lo < 0x6C) ? (uint8_t)(carry ^ 0xFF) : carry;

    uint8_t r3 = bus_read(0xD20A);
    mem[0x282C] = (uint8_t)(r3 - 0x80);
    mem[0x0069] = (r3 < 0x80) ? 0xFF : 0x00;
}

/* terrain_frame_setup @ $9E54 — terrain view-transform setup (flight top #2).
 *
 * Runs once per frame after setup_projection_params + build_view_transform_matrix (which
 * fill the rotation vector $00A0-$00A3 and the input column vectors $22A3/$22D1/$22FF/$232D):
 *
 *  LOOP 1 (45 terrain cells): for each cell, a bit pattern selects how to rotate/translate
 *  that column's two vectors — the screen-X numerator {$22D2:$22A4}[Y] and the depth divisor
 *  {$232E:$2300}[Y] — by the view rotation ±(rot_a,rot_b), nudging the column's screen index.
 *  It then derives the screen-Y numerator {$2388:$235B}[Y] from the sampled terrain height
 *  minus the altitude term, and classifies the column's visibility into $24B4[Y]
 *  ($80 off-screen, $40/$20 behind, $00 visible).  These arrays feed project_terrain_points.
 *
 *  LOOP 2 (object draw order $B67C[]): collapses adjacent visible/hidden object pairs,
 *  clearing the hidden one's column ($232E=0, $2300=$20, $24B4=0) so it isn't drawn.
 *
 * The two divide-scratch carry chains and the build_view carry are dead/masked here, so this
 * is plain 16-bit arithmetic.  Memory contract only (the flight loop reloads regs); no hardware.
 *
 * Split into the two projection calls (kept in C — separate targets) + terrain_frame_setup_core,
 * the two loops.  The core is the ROF_TFSETUP_ASM asm seam's target; its inputs (draw_row $92,
 * terrain_scroll_counter $8A, vbi_flags $88, rot $A0-$A3, alt $8B/$8C, the $22A3/$22D1/$22FF/$232D
 * input vectors, $0900 heights) are all VBI-stable, so the differential need only snapshot the
 * loop outputs. */

void terrain_frame_setup_core_c(void) {
    /* draw_row's top two bits pick the cell-pattern source table ($B5xx/$B6xx) and the
     * starting column screen index; only the 00 case seeds output column 0 from its input. */
    uint8_t dr = draw_row;
    uint8_t src_lo, src_hi;
    if (!(dr & 0x80)) {
        if (dr & 0x40) { src_lo = 0x4F; src_hi = 0xB6; }
        else {
            mem[0x22A4] = mem[0x22A3];           /* seed col-0 outputs from inputs */
            mem[0x22D2] = mem[0x22D1];
            mem[0x2300] = mem[0x22FF];
            mem[0x232E] = mem[0x232D];
            src_lo = 0x22; src_hi = 0xB6;
        }
    } else {
        if (dr & 0x40) { src_lo = 0xC8; src_hi = 0xB5; }
        else           { src_lo = 0xF5; src_hi = 0xB5; }
    }
    sync_flag = src_lo; dl_ptr_lo = src_hi;
    uint16_t srcptr = (uint16_t)(src_lo | (src_hi << 8));

    uint8_t b6 = (uint8_t)((terrain_scroll_counter & 0x0F) << 4);  /* column high-nibble base */
    uint8_t X  = vbi_flags & 0x0F;                                 /* running column index */
    mem[0x00B4] = (uint8_t)(X | mem[0x00B5]);                      /* (written once; dead here) */

    /* Loop-1 invariants: the view rotation vector {rot_a,rot_b} = $00A0-$00A3, and the
     * altitude terms $008B/$008C — read many times per cell, written by neither loop. */
    const uint16_t rot_a = (uint16_t)((scroll_accum_b0 << 8) | draw_iter_count);  /* {$A1:$A0} */
    const uint16_t rot_b = (uint16_t)((scroll_accum_b2 << 8) | scroll_accum_b1);  /* {$A3:$A2} */
    const uint8_t  alt_b = dl_src_index;          /* $8B */
    const uint8_t  alt_c = terrain_scroll_reload; /* $8C */

    uint8_t b5 = 0;                               /* loop-1 cell-pattern / low-byte scratch */
    uint8_t Y = 0;
    do {
        /* Decode the cell's rotation pattern (MSB first) into a rotate/translate of this
         * column's vectors u = {$22D2:$22A4}, v = {$232E:$2300}, and a ±1 column-index step. */
        uint8_t pat = mem[srcptr + Y];
        uint16_t in_u = (uint16_t)((mem[0x22D1 + Y] << 8) | mem[0x22A3 + Y]);
        uint16_t in_v = (uint16_t)((mem[0x232D + Y] << 8) | mem[0x22FF + Y]);
        uint16_t u = 0, v = 0;
        int fired = 1;
        if (pat & 0x80) {                         /* +rot_a / -rot_b, then maybe a 2nd step */
            b6 = (uint8_t)(b6 + 0xF0);
            u = (uint16_t)(in_u + rot_a); v = (uint16_t)(in_v - rot_b);
            if      (pat & 0x40) { X--; u = (uint16_t)(u - rot_b); v = (uint16_t)(v - rot_a); }
            else if (pat & 0x20) { X++; u = (uint16_t)(u + rot_b); v = (uint16_t)(v + rot_a); }
        } else if (pat & 0x40) {                  /* -rot_a / +rot_b, then maybe a 2nd step */
            b6 = (uint8_t)(b6 + 0x10);
            u = (uint16_t)(in_u - rot_a); v = (uint16_t)(in_v + rot_b);
            if      (pat & 0x20) { X--; u = (uint16_t)(u - rot_b); v = (uint16_t)(v - rot_a); }
            else if (pat & 0x10) { X++; u = (uint16_t)(u + rot_b); v = (uint16_t)(v + rot_a); }
        } else if (pat & 0x20) {                  /* -rot_b / -rot_a */
            X--; u = (uint16_t)(in_u - rot_b); v = (uint16_t)(in_v - rot_a);
        } else if (pat & 0x10) {                  /* +rot_b / +rot_a */
            X++; u = (uint16_t)(in_u + rot_b); v = (uint16_t)(in_v + rot_a);
        } else {
            fired = 0;                            /* pattern empty: leave column unchanged */
        }
        if (fired) {
            mem[0x22A4 + Y] = (uint8_t)u; mem[0x22D2 + Y] = (uint8_t)(u >> 8);
            mem[0x2300 + Y] = (uint8_t)v; mem[0x232E + Y] = (uint8_t)(v >> 8);
        }

        /* Screen-Y numerator = (height[col] - alt_c : -alt_b) >> 4, high nibble flipped on
         * borrow.  The column index combines the running low nibble with b6's high nibble. */
        uint8_t y_lo = (uint8_t)(0u - alt_b);
        int     no_borrow1 = (alt_b == 0);
        X = (uint8_t)((X & 0x0F) | b6);
        uint8_t h = mem[0x0900 + X];
        mem[0x23B5 + Y] = h;
        int     diff = (int)h - alt_c - (no_borrow1 ? 0 : 1);
        uint8_t y_hi = (uint8_t)diff;
        uint16_t pair = (uint16_t)((((uint16_t)y_hi << 8) | y_lo) >> 4);
        uint8_t r_hi = (uint8_t)(pair >> 8);
        b5 = (uint8_t)pair;                       /* the 6502 leaves the shifted low byte in $B5 */
        if (diff < 0) r_hi ^= 0xF0;
        mem[0x2388 + Y] = r_hi;
        mem[0x235B + Y] = b5;
        mem[0x2276 + Y] = X;

        /* Visibility class for project_terrain_points: compare |screen-X| against depth. */
        uint8_t v_hi = mem[0x232E + Y];
        uint8_t cls;
        if (v_hi & 0x80) {
            cls = 0x80;                           /* depth negative -> off-screen */
        } else if (v_hi == 0 && mem[0x2300 + Y] < 0x20) {
            cls = 0x80;                           /* too close -> off-screen */
        } else {
            uint8_t u_hi = mem[0x22D2 + Y];
            if (u_hi & 0x80) {                    /* screen-X negative: classify by |u| vs v */
                uint16_t nu = (uint16_t)(0u - (uint16_t)(((uint16_t)u_hi << 8) | mem[0x22A4 + Y]));
                b5 = (uint8_t)nu;
                uint8_t nu_hi = (uint8_t)(nu >> 8);
                if      (nu_hi <  v_hi) cls = 0x00;
                else if (nu_hi != v_hi) cls = 0x40;
                else cls = ((uint8_t)nu < mem[0x2300 + Y]) ? 0x00 : 0x40;
            } else {                              /* screen-X positive */
                if      (u_hi <  v_hi) cls = 0x00;
                else if (u_hi != v_hi) cls = 0x20;
                else cls = (mem[0x22A4 + Y] < mem[0x2300 + Y]) ? 0x00 : 0x20;
            }
        }
        mem[0x24B4 + Y] = cls;
        Y++;
    } while (Y != 0x2D);

    /* Persist the last cell's scratch (the 6502 leaves it in $B5/$B6; loop 2 reuses $B5). */
    mem[0x00B5] = b5;
    mem[0x00B6] = b6;

    /* Loop 2: walk the object draw order, collapsing adjacent visible/hidden pairs. */
    uint8_t y2 = 0;
    for (;;) {
        uint8_t obj = mem[0xB67C + y2];
        mem[0x28DB] = obj;
        y2++;
        uint8_t cls = mem[0x24B4 + obj];

        int check_following = 0, extra_step = 0;
        if (cls & 0x80) {                         /* current off-screen */
            check_following = 1;
        } else if (cls != 0) {                    /* current partially behind */
            extra_step = 1;
        } else {                                  /* current fully visible */
            uint8_t nxt = mem[0xB67C + y2];
            y2++;
            if (mem[0x24B4 + nxt] & 0x80) {        /* next off-screen -> hide the next object */
                mem[0x00B5] = y2;
                mem[0x232E + nxt] = 0x00; mem[0x2300 + nxt] = 0x20; mem[0x24B4 + nxt] = 0x00;
                y2 = mem[0x00B5];
                if (y2 == 0) check_following = 1;
            }
        }
        if (check_following) {
            uint8_t nxt = mem[0xB67C + y2];
            y2++;
            if (mem[0x24B4 + nxt] == 0) {          /* next visible -> hide the current object */
                mem[0x00B5] = y2;
                uint8_t prev = mem[0x28DB];
                mem[0x232E + prev] = 0x00; mem[0x2300 + prev] = 0x20; mem[0x24B4 + prev] = 0x00;
                y2 = mem[0x00B5];
                if (y2 == 0) extra_step = 1;
            }
        }
        if (extra_step) y2++;
        if (y2 >= 0x0C) return;
    }
}

/* Dispatcher seam (asm-migration-plan Phase 3), mirrors terrain_column_rasterize_core.
 * On the Amiga (ROF_TFSETUP_ASM) terrain_frame_setup_core is the hand-written m68k twin
 * in TerrainFrameSetupAssembler.s; elsewhere it is the clean-C oracle above. */
#if defined(ROF_TFSETUP_ASM) && defined(ROF_TFSETUP_VERIFY)
/* On-target differential (single run, deterministic).  All loop INPUTS are VBI-stable, so we
 * snapshot only the OUTPUTS: the $2270-$24E7 vector/class/height block (contains the $22A3.. input
 * vectors too, which the loops don't write) + the ZP outputs $80/$81/$B4/$B5/$B6 + $28DB.  Run the
 * asm twin, capture, restore, run the C oracle, compare.  C output left LIVE. */
extern void terrain_frame_setup_core_asm(void);
volatile unsigned long g_tfsCalls = 0, g_tfsMismatch = 0, g_tfsFirstBad = 0, g_tfsBadAddr = 0;
volatile unsigned long g_tfsAsmTicks = 0, g_tfsCTicks = 0;
/* Start at $2276 — the lowest byte terrain_frame_setup writes.  $2270-$2274 are map/depth
 * scratch written by the ASYNC flight VBI (update_terrain_scanline_proj), so including them
 * gave false mismatches (the VBI fires between the asm and C runs). */
#define TFS_BLK_LO 0x2276
#define TFS_BLK_N  0x28A            /* $2276..$24FF (covers all loop-1 + loop-2 outputs) */
static uint8_t tfs_snapB[TFS_BLK_N], tfs_asmB[TFS_BLK_N];
static const uint16_t tfs_zp[6] = { 0x80, 0x81, 0xB4, 0xB5, 0xB6, 0x28DB };
static uint8_t tfs_snapZ[6], tfs_asmZ[6];
void terrain_frame_setup_core(void) {
    g_tfsCalls++;
    uint8_t* const M = (uint8_t*)mem;
    for (int i = 0; i < TFS_BLK_N; i++) tfs_snapB[i] = M[TFS_BLK_LO + i];
    for (int i = 0; i < 6; i++) tfs_snapZ[i] = M[tfs_zp[i]];
    FP_TIME(terrain_frame_setup_core_asm(), g_tfsAsmTicks);
    for (int i = 0; i < TFS_BLK_N; i++) tfs_asmB[i] = M[TFS_BLK_LO + i];
    for (int i = 0; i < 6; i++) tfs_asmZ[i] = M[tfs_zp[i]];
    for (int i = 0; i < TFS_BLK_N; i++) M[TFS_BLK_LO + i] = tfs_snapB[i];
    for (int i = 0; i < 6; i++) M[tfs_zp[i]] = tfs_snapZ[i];
    FP_TIME(terrain_frame_setup_core_c(), g_tfsCTicks);
    int bad = 0, first = !g_tfsMismatch;
    for (int i = 0; i < TFS_BLK_N; i++) if (M[TFS_BLK_LO + i] != tfs_asmB[i]) { bad = 1; if (first) { g_tfsBadAddr = TFS_BLK_LO + i; first = 0; } }
    for (int i = 0; i < 6; i++) if (M[tfs_zp[i]] != tfs_asmZ[i]) { bad = 1; if (first) { g_tfsBadAddr = tfs_zp[i]; first = 0; } }
    if (bad) { if (!g_tfsMismatch) g_tfsFirstBad = g_tfsCalls; g_tfsMismatch++; }
}
#elif defined(ROF_TFSETUP_ASM)
extern void terrain_frame_setup_core(void);  /* TerrainFrameSetupAssembler.s */
#else
__attribute__((noinline)) void terrain_frame_setup_core(void) { terrain_frame_setup_core_c(); }
#endif

/* terrain_frame_setup @ $9E54 — projection setup (kept C) + the two transform loops (asm seam). */
void terrain_frame_setup(void) {
    setup_projection_params();
    build_view_transform_matrix();
    terrain_frame_setup_core();
}

/* Project one signed axis for object/column X: ratio numerator/divisor -> screen coord.
 *   numerator = {num_hi:num_lo}[X] (signed), divisor = {div_hi:div_lo}[X] (unsigned depth).
 * The divisor is normalised by doubling until it strictly exceeds |numerator| (so the
 * quotient fits a byte), then divide_16x16 gives an 8-bit quotient.  That quotient, scaled
 * back up by the doubling count and by 3/2, is offset from screen centre ($0080) — added
 * for a positive numerator, subtracted for a negative one — into {out_hi:out_lo}[X].
 * If 8 doublings still don't fit, the point is off-screen: clamp to the edge ($40 / $C0). */
static void project_axis(uint8_t X,
                         uint16_t num_lo, uint16_t num_hi,
                         uint16_t div_lo, uint16_t div_hi,
                         uint16_t out_lo, uint16_t out_hi) {
    int      neg     = mem[num_hi + X] & 0x80;
    int16_t  numer   = (int16_t)((mem[num_hi + X] << 8) | mem[num_lo + X]);
    uint16_t absn    = neg ? (uint16_t)(-numer) : (uint16_t)numer;
    uint16_t divisor = (uint16_t)((mem[div_hi + X] << 8) | mem[div_lo + X]);

    int count = 0, fits = 0;
    while (1) {
        if (absn < divisor) { fits = 1; break; }
        divisor = (uint16_t)(divisor << 1);
        if (++count == 8) break;
    }
    if (!fits) {                                    /* never fit -> off-screen edge */
        mem[out_hi + X] = neg ? 0xC0 : 0x40;
        mem[out_lo + X] = 0x00;
        return;
    }

    draw_row_bottom = (uint8_t)count;               /* $9F: the 6502 saved Y across the divide */
    uint8_t q = (uint8_t)divide_16x16_core(absn, divisor).quotient;

    if (count == 0) {
        /* No scaling needed: fold the raw quotient into the low byte (hi byte = 0). */
        uint8_t mag = (uint8_t)((((q >> 1) + q + 1) >> 2) & 0x7F);  /* (q*3/2 + 1) / 4 */
        mem[out_lo + X] = neg ? (uint8_t)(0x80 - mag) : (uint8_t)(0x80 + mag);
        mem[out_hi + X] = 0x00;
    } else {
        /* Undo the divisor scaling (<<count), apply the 3/2 perspective factor, /4. */
        uint16_t scaled = (uint16_t)(q << count);
        uint16_t mag    = (uint16_t)((scaled + (scaled >> 1)) >> 2);
        mem[0x00B5]     = (uint8_t)(mag >> 8);      /* $B5: fold scratch (in contract) */
        uint16_t screen = neg ? (uint16_t)(0x80 - mag) : (uint16_t)(0x80 + mag);
        mem[out_lo + X] = (uint8_t)screen;
        mem[out_hi + X] = (uint8_t)(screen >> 8);
    }
}

/* project_terrain_points @ $A11F — per-object world->screen projection (flight top #3).
 *
 * Input: cpu.X = object/column index.  Marks the object "projected this frame", then runs
 * two perspective divides (project_axis) sharing the depth divisor {$232E:$2300}[X]:
 *   - screen X: {$22D2:$22A4}[X] / depth  ->  {$242D:$2400}[X]
 *   - screen Y: {$2388:$235B}[X] / depth  ->  {$2487:$245A}[X]
 * Then it adds a per-screen-band scroll offset (chosen from the screen-X result) into the
 * screen-Y coordinate.  divide_16x16's scratch ($AE-$B2) is dead after return and excluded
 * from the validation contract, so project_axis uses the typed divide_16x16_core() directly.
 * Memory contract only (the caller reloads registers); no hardware is touched.
 *
 * The $A31C fall-through into terrain_draw_frame is dead (the divides are exhaustive).
 *
 * Split into a typed core + a 6502-ABI shim (like the rasterizer): the clean-C core
 * project_terrain_points_core_c(X) is the SDL/validate oracle; on the Amiga a hand-asm
 * twin (ProjectTerrainAssembler.s) replaces it via the ROF_PROJECT_ASM seam below. */
void project_terrain_points_core_c(uint8_t X) {
    mem[0x24B4 + X] |= 0x10;                          /* flag: object projected this frame */

    project_axis(X, 0x22A4, 0x22D2, 0x2300, 0x232E, 0x2400, 0x242D);   /* screen X */
    project_axis(X, 0x235B, 0x2388, 0x2300, 0x232E, 0x245A, 0x2487);   /* screen Y */

    /* Pick the band scroll offset from the screen-X coordinate, add it (signed) to screen Y. */
    uint8_t x_hi = mem[0x242D + X];
    uint8_t off;
    if (x_hi & 0x80)    off = mem[0x270E];                              /* X off left edge */
    else if (x_hi != 0) off = mem[0x272D];                             /* X off right edge */
    else                off = mem[0x270E + (mem[0x2400 + X] >> 3)];     /* on-screen band */

    uint16_t y_lo = (uint16_t)mem[0x245A + X] + off;
    mem[0x245A + X] = (uint8_t)y_lo;
    mem[0x2487 + X] = (uint8_t)(mem[0x2487 + X] + ((off & 0x80) ? 0xFF : 0x00) + (y_lo >> 8));
}

/* Dispatcher seam (asm-migration-plan Phase 3), mirrors terrain_column_rasterize_core.
 * On the Amiga (ROF_PROJECT_ASM) project_terrain_points_core is the hand-written m68k
 * twin in ProjectTerrainAssembler.s; elsewhere it is the clean-C oracle above. */
#if defined(ROF_PROJECT_ASM) && defined(ROF_PROJECT_VERIFY)
/* On-target differential check (single run, deterministic): run the asm twin on the
 * real state, snapshot the cells it wrote, restore them, run the C oracle on the same
 * inputs, and compare.  The C oracle's output is left LIVE so flight stays correct on
 * an asm bug.  Reads via amiga/project_verify.gdb.  project_terrain_points writes only
 * X-indexed cells ($2400/$242D/$245A/$2487/$24B4 + X) plus $009F and $00B5, so the
 * snapshot is exact (no wide window needed). */
extern void project_terrain_points_core_asm(uint8_t X);
volatile unsigned long g_projCalls = 0, g_projMismatch = 0, g_projFirstBad = 0;
volatile unsigned long g_projAsmTicks = 0, g_projCTicks = 0;
void project_terrain_points_core(uint8_t X) {
    g_projCalls++;
    uint8_t* const M = (uint8_t*)mem;
    const uint16_t cells[7] = { (uint16_t)(0x2400+X), (uint16_t)(0x242D+X),
                                (uint16_t)(0x245A+X), (uint16_t)(0x2487+X),
                                (uint16_t)(0x24B4+X), 0x009F, 0x00B5 };
    uint8_t snap[7], asmv[7];
    for (int i = 0; i < 7; i++) snap[i] = M[cells[i]];
    FP_TIME(project_terrain_points_core_asm(X), g_projAsmTicks);
    for (int i = 0; i < 7; i++) asmv[i] = M[cells[i]];
    for (int i = 0; i < 7; i++) M[cells[i]] = snap[i];
    FP_TIME(project_terrain_points_core_c(X), g_projCTicks);
    int bad = 0;
    for (int i = 0; i < 7; i++) if (M[cells[i]] != asmv[i]) bad = 1;
    if (bad) { if (!g_projMismatch) g_projFirstBad = g_projCalls; g_projMismatch++; }
}
#elif defined(ROF_PROJECT_ASM)
extern void project_terrain_points_core(uint8_t X);  /* ProjectTerrainAssembler.s */
#else
__attribute__((noinline)) void project_terrain_points_core(uint8_t X) {
    project_terrain_points_core_c(X);
}
#endif

/* 6502-ABI shim: object/column index in cpu.X (memory-only contract; caller reloads regs). */
void project_terrain_points(void) { project_terrain_points_core(cpu.X); }

/* fill_terrain_silhouette @ $AE53 — per-column terrain silhouette fill (flight top #4).
 * NOT collision: the symbol's old "$B12F crash handler" was a misread — $B12F is the raster-fill
 * entry, and there is no ship test here.  Real ship/object collision is object_step_and_collide
 * ($9552), which uses the $0900/$0A00 cell maps, not this bitmap.
 *
 * Input: cpu.X = starting column.  Iterates 42 columns (Y = X..X+41), and for each:
 *  - CASCADE: scan the 48 bitmap rows (terrain_row0, stride $60) top-to-bottom for the first
 *    non-empty cell; its index k (0..$2F, or $30 if all empty) is the terrain surface row.
 *  - WATERFALL: paint $55 (sky) into rows k-1..1 (everything above the surface; row 0 left alone).
 *  - RASTER ($B12F): walk the column's bitmap downward from row_base {hi:lo}[k] stepping $60,
 *    ORing terrain_fill_or_mask in and chaining via terrain_fill_chain_mask until it & the
 *    running mask underflows.
 *
 * Builds the mode-D bitmap that renderFlightDirect reads for the dots plane (the rasterizer's
 * |= reads it back too).  Cascade/waterfall are plain loops (byte-equivalent to the 6502's
 * unrolled compare ladder + store fall-through); the raster loop is faithful.  The fill loop
 * only terminates on real terrain tables, so it is validated against a flight RAM snapshot.
 * Contract: memory only (caller reloads regs).
 */
/* _core takes the start column directly (was passed in cpu.X); the void shim below preserves
   the 6502-ABI entry for the validation harness.  See clear_terrain_column for the pattern. */
void fill_terrain_silhouette_core(uint8_t startCol) {
    /* Idiomatic rewrite (was a per-instruction transliteration that re-read/-wrote the
     * $80/$81/$95/$96 ZP scratch ~13x per fill iteration and recomputed i*$60 each scan
     * step).  Every 68000 memory access is slow, so the running pointer / masks / counter
     * live in registers and only the final ZP state (which the oracle leaves behind, and
     * the harness checks) is written back.  Bitmap via a non-volatile alias (the main loop
     * owns terrain_row0+; the fill-mask tables are read-only).  mem[]-identical. */
    uint8_t* const M = (uint8_t*)mem;
    uint8_t Y   = startCol;                              /* ae53 TXA;TAY (start column) */
    uint8_t col = 0x2A;                                  /* ae55 42 columns ($9F) */
    uint16_t fPtr = 0; uint8_t f95 = 0, f96 = 0;         /* captured final ZP state */

    do {                                                 /* L_ae59 — one column */
        /* Topmost non-empty of the 48 rows (base terrain_row0, stride $60); $30 if all empty.
         * Walk a row pointer by +$60 — no per-row multiply/index. */
        uint8_t* p = M + 0x1010 + Y;                     /* $1010 = terrain_row0 (data sym; no MEM_*) */
        int k = 0x30;
        for (int i = 0; i < 0x30; i++) {
            if (*p != 0) { k = i; break; }
            p += 0x60;
        }
        /* Waterfall $55 (sky) into rows k-1..1.  The scan left p AT row k (or row $30 if none),
         * so step it back down by $60 — reuse it, still no multiply.
         *
         * On the Amiga only the bottom 4 rows (waterfall rows 44-47) are ever read back, so the
         * fill stops at row 44 there.  renderFlightDirect builds the main viewport's sky plane
         * from $260E (blitterFillUp) and its plane2 dot scan skips sky bytes ($55 & $AA == 0,
         * identical to the 0 clear_terrain_column already wrote), so rows 1..43 of this fill feed
         * nothing on screen.  The lone reader is the windscreen-bottom band convert (scanlines
         * 172-179): it decodes field rows 43-46, which — via the $1074-vs-$1010 base skew of
         * 100 = 1 row + 4 bytes — are exactly THESE waterfall rows 44-47.  So when the skyline
         * descends into the band (k>=45) those corner cells must hold $55 to show sky, not the
         * cleared 0 (black).  The full fill is kept for SDL so make validate stays mem[]-identical. */
#ifdef ROF_PLATFORM_AMIGA
        const int wf_lo = 44;       /* band rows 44-47 only — the sole cells read back */
#else
        const int wf_lo = 1;        /* faithful full above-surface sky fill (validation oracle) */
#endif
        for (int i = k - 1; i >= wf_lo; i--) { p -= 0x60; *p = 0x55; }

        /* Column body fill, walking a pointer down by $60 via the row_base[k] pointer tables,
         * ORing terrain_fill_or_mask until terrain_fill_chain_mask[v] & the running mask = 0. */
        uint16_t base = (uint16_t)(M[MEM_row_base_lo + k] | (M[MEM_row_base_hi + k] << 8));  /* b12f-b137 */
        uint8_t* fp = M + base + Y;
        uint8_t m95 = 0x00, m96 = 0x55;                  /* b139-b13f */
        for (;;) {                                       /* L_b141 */
            uint8_t v = *fp;                              /* ($80),Y */
            *fp = (uint8_t)((v & m95) | m96 | M[MEM_terrain_fill_or_mask + v]);
            uint8_t e = (uint8_t)(M[MEM_terrain_fill_chain_mask + v] & m96);
            if (e == 0) break;                            /* b152 */
            m96 = e;                                      /* b154 */
            m95 = M[MEM_terrain_fill_chain_mask + e];     /* b157-b15a */
            fp += 0x60;                                   /* b15c-b167 ($80/$81 += $60) */
        }
        fPtr = (uint16_t)((uint16_t)(fp - M) - Y);        /* final $80/$81 base */
        f95 = m95; f96 = m96;                             /* last column's surviving ZP state */
        Y++;                                              /* b169 */
    } while (--col != 0);                                /* b16a-b16c */

    /* Final ZP state the 6502 leaves (only the last column's values survive). */
    sync_flag = (uint8_t)fPtr; dl_ptr_lo = (uint8_t)(fPtr >> 8);
    blit_color_src = f95;           span_row_count = f96;
    draw_row_bottom = 0x00;
}
void fill_terrain_silhouette(void) { fill_terrain_silhouette_core(cpu.X); }

/* terrain_draw_frame @ $A31E — render one frame of the fractal planet surface.
 *
 * Draws the terrain into a mode-D (2bpp, 4px/byte) silhouette bitmap and tracks the
 * per-column ridge line in terrain_height_max ($260E).  Downstream, the Amiga's
 * renderFlightDirect turns those into two bitplanes: the SKY plane (filled above the
 * terrain_height_max ridge of each column) and the DOTS plane (the mode-D value-2/3
 * surface pixels this function plots).  Each viewport column is mapped to a bitmap
 * (byte offset, pixel mask) via terrain_col_byte_offset/terrain_col_pixel_mask.
 *
 * Input: entryX = the draw base / double-buffer half ($00 display, $30 back).  Phases:
 *  1. INIT: build terrain_col_byte_offset (col -> row byte offset) for this frame, plus the
 *     per-column work arrays that seed each column's starting row at the horizon ($67/$6B).
 *  2. SETUP: stash the roll position, compute_row_xspans (the per-row L/R extents that form
 *     the perspective viewport trapezoid), seed the span/parallax accumulators to centre.
 *  3. DRAW LOOP ($A3AB, walks the $B67C back-to-front draw order until $90): for each active
 *     object, project_terrain_points (world->screen) + terrain_plot_object, then
 *     terrain_subdivide_column — the fractal midpoint-displacement renderer that fills the
 *     terrain between objects, OR-plotting each column into the bitmap and updating $260E.
 *  4. TAIL: altimeter/pitch HUD math, lock-on convergence ($004D), check_target_in_window,
 *     obj_table_set_active, the $0A00 cell bump, and a random enemy-spawn attempt.
 *
 * Structured C; the draw loop keeps A/X/Y index scratch.  Native callees that consume a
 * register get cpu.X set first: project_terrain_points/terrain_plot_object take the object
 * index, terrain_subdivide_column takes 0, ring_push_marked takes $14.  Reads POKEY RANDOM
 * $D20A (the harness seeds it identically per case).  Contract: memory only.
 */
/* Optional sub-phase probe (Amiga autoflight only; -DROF_TDRAW_PROF): split the
 * terrain_draw_frame object loop's cost into the fractal subdivision vs the
 * projection+object-plot, via the raster beam line.  Desktop builds compile these
 * to no-ops (no HW beam there).  Read g_tdSubdiv / g_tdProjPlot from the debugger. */
#ifdef ROF_TDRAW_PROF
extern unsigned short rof_beam_line(void);
extern unsigned long g_tdSubdiv, g_tdProjPlot, g_tdFrames;
extern unsigned long g_tdPairs, g_tdCulled, g_tdVisPairs, g_tdProjCount;
#define TDPAIR(v) (++(v))
#define PB(v) unsigned short v = rof_beam_line()
#define PE(v,acc) do { unsigned short _e = rof_beam_line(); \
    (acc) += (_e >= (v)) ? (unsigned long)(_e - (v)) : (unsigned long)(_e + 313 - (v)); } while (0)
#else
#define TDPAIR(v) ((void)0)
#define PB(v) ((void)0)
#define PE(v,acc) ((void)0)
#endif

/* Object draw-order pass of terrain_draw_frame_core (below), split out into its own function
 * PURELY FOR CODEGEN — the behaviour is unchanged.
 *
 * Inlined into terrain_draw_frame_core, which is itself inlined into the enormous
 * game_main_loop, GCC ran out of registers and spilled the loop's entire working set
 * (order_idx, obj0, obj1 and BOTH per-object base pointers) to the frame, reloading them —
 * and re-materialising `lea mem,aN` — every iteration.  The PC profile showed that spill
 * traffic, not the work: the bucket stayed at 11.2% even with the ROF_TDRAW_PROF
 * instrumentation compiled out (measured 2026-08-04).  On its own, with `noinline` to stop
 * GCC folding it straight back in, the loop has registers to spare.
 *
 * Two hints beyond that, both worth keeping: the draw-order table and the mem base are hoisted
 * into locals so they stay in address registers (instead of a 32-bit `addi.l #0xB67C` + `lea
 * mem` per lookup), and each of the ten per-pair vector copies is written as one
 * memory-to-memory `mem[k] = oN[j]` so GCC emits a single MOVE.B (d16,An),(d16,An) rather than
 * a load into a data register plus an absolute-long store.
 *
 * The loop itself is pure BOOKKEEPING — it does no drawing (its callees do), yet with zero
 * objects on the map it still costs ~4.5% of flight wall clock, over exactly 72 pairs per pass.
 * Three pieces of that were the 6502's, not ours, and are gone (2026-08-08):
 *   - the $24B4 visibility class gets its OWN base register, so a cull test is one
 *     `move.b (0,a5,dn.l)` instead of `lea (0,a2,dn.l),aN` + `move.b (9396,aN)`;
 *   - the draw-order index is an `unsigned`, not a `uint8_t` — see below;
 *   - $28DB and the $272E reload are 6502 register-save residue — see below.
 * Counted off the disassembly, see the log. */
__attribute__((noinline)) static void terrain_draw_objects(void) {
    /* ROF_MEM_VIEW, not a hard `volatile` — see its definition at the top of this file.  With
       the views volatile GCC re-materialises each table lookup as `lea (0,An,Dn.l),aM` +
       `move.b (aM),Dn` (20 cyc) instead of `move.b (0,An,Dn.l),Dn` (14), cannot walk `order`
       as a pointer, and splits each of the five per-pair vector copies into a load + a store
       (24 cyc) instead of one `move.b d16(An),d16(Am)` (20). */
    ROF_MEM_VIEW uint8_t *M = (ROF_MEM_VIEW uint8_t *)mem;              /* one base register for the whole loop */
    ROF_MEM_VIEW const uint8_t *order = (ROF_MEM_VIEW const uint8_t *)mem + 0xB67C; /* draw-order table base */
    ROF_MEM_VIEW const uint8_t *cls = (ROF_MEM_VIEW const uint8_t *)mem + 0x24B4;   /* per-object visibility class, own base */
#ifdef __mc68000__
    /* Hide the bases from the optimiser.  Left as plain initialisers GCC knows they are the
       address of a symbol and constant-folds every use back into an absolute-long address
       (`addi.l #443882` / `adda.l #397166` per lookup, MOVE.B Dn,(xxx).L = 16 cycles per store).
       Forced into address registers it uses (d16,An) / (0,An,Dn.w) instead — 12 cycles, and no
       per-iteration address arithmetic at all. */
    __asm__ ("" : "+a" (M));
    __asm__ ("" : "+a" (order));
    __asm__ ("" : "+a" (cls));
#endif
    /* NOT a uint8_t.  The index only ever takes the values 0,2,4..$90 — every path adds 1 or 2
       and the loop's sole exit is the == $90 test — so it cannot wrap, and a byte type made GCC
       spend an `andi.l #255` plus a `moveq #0`/`move.b` zero-extend at each of the three places
       it indexes a table with it. */
    unsigned order_idx = 0;
    uint8_t obj0 = 0;
    /* The subdivide twin parks its exit residue in g_sdResidue rather than writing mem[$82-$86] /
       mem[$8D-$91] on every call; seed it here so a pass whose calls all bail (or compute no
       midpoint) still publishes what mem[] already held, then write it through after the loop. */
    SD_RESIDUE_SEED();
#ifdef ROF_SUBDIV_OBJ_SEED0
    /* Same deal one level up for SubPt slot 0 (SD_SEED0): the twin reads the far endpoint out of
       obj1's arrays, so the five per-pair copies are gone and only the LAST visible pair's value
       is observable.  -1 = no visible pair this pass, in which case slot 0 keeps what it held.
       ⚠ Re-reading obj1's vector after the loop is sound because it cannot move in between: the
       only writers of $2400/$242D/$245A/$2487+obj are project_terrain_points (which the $24B4
       bit-4 "already projected" flag stops from running twice for one object in a pass) and
       terrain_frame_setup (long returned); $23B5+obj is written only by frame setup.  Everything
       else in the loop's closure — plot_object, the rasterizer — only reads them. */
    int lastO1 = -1;
#endif
    for (;;) {
        TDPAIR(g_tdPairs);
        obj0 = order[order_idx++];                   /* primary endpoint of the next pair */
        /* $28DB (collapse_cur_obj) is the 6502's save slot for X across the calls below — the C
           keeps obj0 in a register, so the per-pair store is pure mem[] residue.  Nothing reads
           it in between (its only other user is terrain_frame_setup, which has long returned),
           and the loop always ends on the pair at $8E, so the oracle's final value is just the
           last obj0: written ONCE after the loop instead of 72 times. */
        if (cls[obj0] & 0xA0) {                      /* primary off-screen or culled: skip the pair */
            TDPAIR(g_tdCulled);
            order_idx++;
        } else {
            uint8_t obj1 = order[order_idx++];       /* companion endpoint */
            uint8_t cls1 = cls[obj1];                /* companion visibility class (read once) */
            M[0x272E] = (uint8_t)order_idx;          /* scratch-save the index across the calls */
            if (!(cls1 & 0xC0)) {                    /* companion on-screen and not culled */
#ifndef ROF_SUBDIV_OBJ_SEED0
                ROF_MEM_VIEW const uint8_t *o1 = M + obj1; /* base for obj1's per-object arrays */
#endif
                TDPAIR(g_tdVisPairs);
                if (!(cls1 & 0x10))                  /* project the companion unless already projected */
                    { TDPAIR(g_tdProjCount); PB(_pp1); project_terrain_points_core(obj1); cpu.X = obj1; OP_TIME(terrain_plot_object()); PE(_pp1, g_tdProjPlot); }
#ifdef ROF_SUBDIV_OBJ_SEED0
                lastO1 = obj1;                       /* subdivide reads far@0 from obj1 itself */
                SD_SEED0_SHADOW(obj1);               /* SEED0_VERIFY only — see rof_seed0_shadow */
#else
                /* seed subdivide sub-point [0] with the companion's projected vector */
                M[0x25B4]=o1[0x2400]; M[0x25D2]=o1[0x242D]; M[0x25F0]=o1[0x245A];
                M[0x24E2]=o1[0x2487]; M[0x23E2]=o1[0x23B5];
#endif
                if (!(cls[obj0] & 0x10))             /* project the primary unless already projected */
                    { TDPAIR(g_tdProjCount); PB(_pp2); project_terrain_points_core(obj0); cpu.X = obj0; OP_TIME(terrain_plot_object()); PE(_pp2, g_tdProjPlot); }
                /* Subdivide, with the primary's projected vector as the running span endpoint.
                   The five bytes used to be copied into $82-$86 here purely for the callee's
                   prologue to load back out; terrain_subdivide_column_obj takes the object id
                   and loads them itself (see the ABI note at its definition). */
                CL_CNT(g_clSubCalls);                /* tree entries: a pure COUNT, no bracket */
                PB(_sd); SEGPRE(); SUBDIV_OBJ(0x00, (uint8_t)order_idx, obj0, obj1); SEGPOST(); PE(_sd, g_tdSubdiv);
                /* The oracle reloads the index from $272E here, and re-increments it when the
                   reload reads 0 — the 6502 shares that INY with the primary-culled path.  Both
                   are dropped: no callee writes $272E (it is this routine's private scratch),
                   and the reload can never be 0 (the index is even and <= $90).  The store above
                   stays, because the residue it leaves is observable. */
            }
        }
        if (order_idx == 0x90) break;
    }
    M[0x28DB] = obj0;                                /* the $28DB residue the oracle leaves */
#ifdef ROF_SUBDIV_OBJ_SEED0
    if (lastO1 >= 0) SD_SEED0((uint8_t)lastO1);      /* the deferred SubPt slot-0 residue */
    SD_SEED0_CHECK();                                /* SEED0_VERIFY only */
#endif
    SD_RESIDUE_PUBLISH();                            /* the deferred $82-$86 / $8D-$91 residue */
}

/* terrain_draw_frame @ $A31E — render one frame's terrain + objects (the flight top-level draw).
 *
 * For one double-buffer half (entryX = $00 display / $30 back):
 *   1. SETUP — build the per-column bitmap byte-offset map, reset the per-row span tables to
 *      their empty sentinels and the frame's span extents to the neutral midpoint, then compute
 *      the row x-spans (compute_row_xspans).
 *   2. DRAW — walk the object draw order ($B67C): project each visible object to screen space
 *      (project_terrain_points + terrain_plot_object) and fractal-subdivide the terrain segment
 *      between paired objects (terrain_subdivide_column).  This is the bulk of the frame cost.
 *   3. BOOKKEEPING — derive the difficulty level from the max span, converge the lock-on
 *      magnitude $4D from the pitch/roll rates (arming the lock-on indicator), age near-max
 *      terrain-map cells, and occasionally spawn a random enemy above the terrain.
 *
 * Contract: memory only.  _core takes the half/column-base directly; the void shim keeps the
 * 6502-ABI entry for validation.  entryX is half 0 ($00) or 1 ($30).  Reads POKEY RANDOM
 * (harness seeds it identically per run).
 */
void terrain_draw_frame_core(uint8_t entryX) {
    ROF_MEMBASE_DECL(mb);   /* 146 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    CL_SUB_BEG();                                        /* DRAW sub-split: start the head segment */
    mem[0x00A7] = entryX;                                /* remember which double-buffer half we're drawing */
#ifdef ROF_PLATFORM_AMIGA
    /* Signal the flight renderer that a fresh terrain frame is being drawn, so renderFlightDirect
     * repaints (rather than preserving the last frame during a rescue pause).  See g_flightTerrainFresh. */
    extern volatile int g_flightTerrainFresh;
    g_flightTerrainFresh = 1;
#endif
#ifdef ROF_TDRAW_PROF
    g_tdFrames++;                                        /* per-frame normalizer for g_tdSubdiv/g_tdProjPlot */
#endif
#ifdef ROF_PLATFORM_AMIGA
    /* The rasterizer ORs dots straight into the off-screen buffer's plane2 (g_flightDotPlane);
       make sure that buffer's kicked clear has finished before the first dot lands. */
    rof_flight_wait_dotclear();
#endif
    {
        /* Map each viewport screen column to its byte offset within a bitmap row.  The mode-D
           bitmap packs 4 columns per byte, so 4 consecutive columns share one byte offset and
           the offset advances by 1 every 4 columns: terrain_col_byte_offset[col] = entryX +
           col/4, for the viewport columns (bytes $BD20..$BDD7).  entryX is the draw base (the
           double-buffer half: $00 = display, $30 = back).  The rasterizer plots column X via
           bitmap[row_addr + terrain_col_byte_offset[X]] |= terrain_col_pixel_mask[X].
           Filled as 46 long stores: each 4-column group is a uniform byte (offset value
           broadcast to all 4 lanes), which is endianness-neutral and lets us walk a long
           pointer; the region is 4-aligned, the flight VBI never touches it, and entryX
           ($00/$30) keeps the per-group +1 from carrying across a lane. */
        uint32_t grp = (uint32_t)entryX * 0x01010101u;
        uint32_t *p   = (uint32_t *)(mem + MEM_terrain_col_byte_offset + 0x20);
        uint32_t *end = (uint32_t *)(mem + MEM_terrain_col_byte_offset + 0xD8);
        while (p < end) { *p++ = grp; grp += 0x01010101u; }
        mem[0x00B3] = (uint8_t)(entryX + 0x5C);          /* $00B3 = far column id (entryX + $5C) */
    }
    {   /* L_a351: $263A/$26CE[0..$13] = $67 (20 bytes, 2-aligned) -> uniform word stores.
           ⚠ GCC actually splits these back into 40 individual byte stores (checked in the
           disassembly).  Batching them the same way as the $6B run below was TRIED and is worse:
           with two interleaved volatile long pointers over a 5-trip loop GCC emitted a redundant
           volatile READ before every byte store.  40 bytes is ~0.05% of wall, so it is not worth
           chasing further — left as-is deliberately. */
        uint16_t *a = (uint16_t *)(mem + 0x263A), *b = (uint16_t *)(mem + 0x26CE);
        for (int i = 0; i < 0x0A; i++) { a[i] = 0x6767; b[i] = 0x6767; }
    }
    /* L_a35e: the oracle writes $6B as four $21-byte runs at $264E/$266F/$2690/$26B1 — but those
       ABUT: $264E+$21 == $266F, $266F+$21 == $2690, $2690+$21 == $26B1.  So the four together are
       ONE CONTIGUOUS 132-byte run $264E..$26D1, which is exactly 33 longs — this is the COL_MAX
       ($260E) horizon reset for columns $40..$C3.  (The old comment claimed $266F/$26B1 were "odd
       addresses" so batching would fault; they are odd only as OFFSETS — every address here is even,
       which is all move.l needs on a 68000.)
       Batched as 33 long stores instead of 132 volatile byte stores: $6B broadcast to all four
       lanes is endianness-neutral, so the store is identical on the big-endian Amiga and the
       little-endian validate host (same condition as the $BD00 fill above).  The region is
       main-loop-owned — the rasterizer's COL_MAX writes are the only other writer, and no flight-VBI
       path touches $26xx (the $260E+x struct writes in rof_native_amiga.cpp are the STATION scene's
       animation channels).  GCC had fully UNROLLED the byte loop into 132 `lea` + `move.b
       #$6B,(0,aN,aM.l)` pairs plus repeated `lea mem,aN` rematerialisations, ~26 cycles a byte.
       ⚠ Order vs the $67 fill above is preserved, and it matters: $26CE..$26D1 belongs to BOTH
       runs and must end up $6B.  It does, because this fill still comes second.
       ⚠ The pointer MUST stay volatile.  A plain `uint32_t*` loop here is recognised by GCC as a
       memset and turned into `jsr memset` — and this build's freestanding memset
       (support/gcc8_c_support.c) is a byte-at-a-time `move.b d0,(a0)+` / `cmpa.l` / `bne` loop at
       ~24 cycles a byte, i.e. it hands all 132 byte writes straight back.  volatile is what keeps
       the 33 long stores. */
    {
        volatile uint32_t *p = (volatile uint32_t *)(mem + 0x264E);
        for (int i = 0; i < 33; i++) p[i] = 0x6B6B6B6Bu;
    }

    mem[0x2907] = roll_pos_lo;                           /* stash the current roll into the param block */
    mem[0x2908] = roll_pos_hi;
    compute_row_xspans();
    /* reset this frame's span extents to the neutral $80 midpoint (terrain_span_max + $0079
       only on the display pass, so the back pass inherits the display pass's max). */
    mem[0x28E7]=0x80; mem[0x28E8]=0x80; mem[0x2912]=0x80;
    mem[0x2913]=0x80; mem[0x2914]=0x80; mem[0x2915]=0x80;
    if (mem[0x00A7] == 0) { terrain_span_max = 0x80; mem[0x0079] = 0x80; }
    /* clear the per-frame object-visibility counters; bump one when $6A is non-negative */
    mem[0x28ED]=0x00; mem[0x28FB]=0x00; mem[0x28FC]=0x00;
    if (!(mem[0x006A] & 0x80))
        mem[0x28FC] = (uint8_t)(mem[0x28FC] + 1);

    /* Object draw-order pass — the bulk of the frame cost.  $B67C[] lists object ids in
       back-to-front draw order, grouped in pairs: a primary endpoint and its companion (the
       two ends of one terrain segment).  For each pair whose endpoints are both visible we
       project the endpoints to screen space, then fractal-subdivide the terrain spanning
       between them.  $24B4[obj] is the per-object visibility class: bit7 off-screen, bit6/bit5
       cull flags, bit4 "already projected this frame".  project_terrain_points and
       terrain_plot_object take the object id in the 6502 X register (still 6502-ABI twins).
       The pass ends once the draw-order index reaches $90.  Body split into
       terrain_draw_objects() above — for codegen only, see the comment there. */
    CL_SUB_END(0);                                       /* head done */
    terrain_draw_objects();
    CL_SUB_END(1);                                       /* object draw-order pass done */

    /* Publish this frame's span extents for the HUD / next frame. */
    mem[0x28D9] = mem[0x28E7];
    mem[0x28DA] = mem[0x28E8];
    /* Difficulty level = (max span >> 2) + 1, clamped to 9, while the span is valid (non-negative).
       On a level drop while not crashed, queue a level-change marker (ring slot $14). */
    if (!(mem[0x0079] & 0x80)) {
        uint8_t lvl = (uint8_t)((mem[0x0079] >> 2) + 1);
        if (lvl >= 0x0A) lvl = 0x09;
        range_to_pilot = lvl;
        if (lvl != mem[0x283F]) {
            uint8_t prev = mem[0x283F];
            mem[0x283F] = lvl;
            if (lvl < prev && mem[0x003D] == 0)
                { cpu.X = 0x14; ring_push_marked(); }
        }
    } else {
        range_to_pilot = 0x00;
    }
    /* Derive HUD sprite positions from the span extents ($2912/$2913).  The shift is a
       sign-preserving (arithmetic) >>1. */
    mem[0x2847] = (uint8_t)(((mem[0x2912] >> 1) | (mem[0x2912] & 0x80)) + 0x7D);
    mem[0x2845] = (uint8_t)(mem[0x2913] + 0x15);
    mem[0x2916] = mem[0x2914];
    mem[0x2839] = mem[0x2910];
    mem[0x283A] = mem[0x2911];
    if (level_or_state != 0) check_target_in_window();
    mem[0x2840] = mem[0x28FC] ? 0x74 : 0x00;             /* light the target marker if any object was visible */
    /* Stage the per-frame parameter block (object positions, depth, roll) consumed downstream. */
    mem[0x28FD]=mem[0x2270]; mem[0x28FE]=mem[0x2271]; mem[0x28FF]=mem[0x2272];
    mem[0x2900]=mem[0x2273]; mem[0x2901]=mem[0x2274]; mem[0x2902]=scaled_depth_hi;
    mem[0x2903]=mem[0x2809]; mem[0x2904]=mem[0x280A]; mem[0x2905]=mem[0x280B]; mem[0x2906]=mem[0x280C];
    mem[0x2909]=mem[0x2907]; mem[0x290A]=mem[0x2908];
    /* Per-row half-deltas: $28B6[y] = arithmetic-rounded (row_span_seed - $270E[y]) >> 1, 32 rows. */
    for (int y = 0x1F; y >= 0; y--) {
        uint8_t a = (uint8_t)(row_span_seed - mem[0x270E + y]);
        mem[0x28B6 + y] = (uint8_t)(((a >> 1) | (a & 0x80)) + (a & 1));
    }

    /* This frame's contribution (v) to the lock-on magnitude, plus an extra +1 (carryIn) in the
       "no objects" case. */
    uint8_t v; int carryIn;
    if (mem[0x28FB] == 0) {                              /* no objects this frame: use the decay constant */
        v = mem[0x061C]; carryIn = 1;
    } else if (mem[0x003D] != 0) {                       /* crashed: no contribution */
        v = 0x00; carryIn = 0;
    } else {                                             /* otherwise v = |Δpitch| + |Δroll| in scaled units */
        #define SAR1(x) ((uint8_t)(((uint8_t)(x) >> 1) | ((uint8_t)(x) & 0x80)))  /* arithmetic >>1 */
        uint16_t pitch16 = ROF_PAIR16(pitch_pos_lo, pitch_pos_hi);
        uint16_t v2919   = ROF_PAIR16(mem[0x2919], mem[0x291A]);
        /* pitch delta: high byte of each 16-bit value scaled by 8 (<<3), then differenced + abs */
        uint8_t base = (uint8_t)((uint16_t)(pitch16 << 3) >> 8);
        uint8_t dx   = (uint8_t)((uint16_t)(v2919  << 3) >> 8);
        dx = (uint8_t)(dx - base);
        if (dx & 0x80) dx = (uint8_t)(0u - dx);
        /* roll delta: roll_velocity vs $291B, each arithmetic >>2, then differenced + abs */
        uint8_t c4 = SAR1(SAR1(roll_velocity));
        uint8_t dy = SAR1(SAR1(mem[0x291B]));
        dy = (uint8_t)(dy - c4);
        if (dy & 0x80) dy = (uint8_t)(0u - dy);
        v = (uint8_t)(dy + dx); carryIn = 0;
        #undef SAR1
    }
    /* Fold this frame's contribution into the lock-on magnitude $4D, clamp out of the negative
       range, then decay it by $061C (floor 0).  Magnitude >= $20 arms the lock-on indicator. */
    uint8_t mag = (uint8_t)((uint16_t)v + mem[0x004D] + carryIn);
    if (mag & 0x80) mag = 0x7F;                                       /* clamp negative -> $7F */
    mag = (mag >= mem[0x061C]) ? (uint8_t)(mag - mem[0x061C]) : 0x00; /* decay, floor 0 */
    mem[0x004D] = mag;
    {
        uint8_t ind = lock_on_indicator_state;
        if (mag >= 0x20) { if (!(ind & 0x80)) ind = 0x80; }          /* arm   */
        else             { if (ind == 0x80)   ind = 0x00; }          /* disarm */
        lock_on_indicator_state = ind;
    }
    /* Randomly activate a dormant object, with probability scaled by how far past level 6 we are. */
    if (game_state != 0 && level_stage >= 0x06
        && (uint8_t)(level_stage - 0x06) >= bus_read(0xD20A))
        obj_table_set_active();
    /* Age every near-max ($FA-$FF) terrain-map cell by one, re-flagging if any remain. */
    if (map_cell_hit_marker != 0) {
        CL_AGE({
        map_cell_hit_marker = 0;
        for (int x = 0; x < 0x100; x++) {
            if (mem[0x0A00 + x] > 0xF9) {
                mem[0x0A00 + x]++;
                map_cell_hit_marker++;
            }
        }
        });
    }
    /* Occasionally spawn a random enemy above the terrain; bail out on any failed precondition:
       a free object slot, not crashed, a configured spawn period, and the per-spawn countdown
       reaching zero (then reload it). */
    if (!(object_index_signed & 0x80)) return;
    if (mem[0x003D] != 0) return;
    if (mem[0x0621] == 0) return;
    if (--mem[0x0622] != 0) return;
    mem[0x0622] = mem[0x0621];                           /* reload the spawn period */
    uint8_t r1 = bus_read(0xD20A);                       /* random screen column candidate */
    if (r1 & 0x80) return;
    uint16_t sx = (uint16_t)r1 + 0x40;
    uint8_t spawnX = (uint8_t)sx;
    /* random height = base $6E + 5-bit jitter (+ carry out of sx) */
    uint8_t h = (uint8_t)((bus_read(0xD20A) & 0x1F) + 0x6E + (sx >> 8));
    if (h <= mem[MEM_terrain_height_max + spawnX]) return;   /* must sit above the terrain here */
    uint8_t yt = (uint8_t)(0x80 - h);                    /* convert height to a screen y */
    object_pos_y_lo = (uint8_t)((yt << 1) + 0x42);
    object_pos_x_lo = (uint8_t)(spawnX - 0x10);
    object_pos_x_hi = 0; object_pos_y_hi = 0; mem[0x0068] = 0; mem[0x0069] = 0;
    terrain_jitter_column();
    mem[0x006A] = 0x7F; object_index_signed = 0x7F; mem[0x2845] = 0x7F;  /* mark the slot occupied */
    indicator_pos = 0x01;
    mem[0x282D] = terrain_depth_step;
    CL_CNT(g_clSaucer);                                  /* combat-load: a saucer spawned */
#ifdef ROF_MEMBASE
#undef mem
#endif
}
void terrain_draw_frame(void) { terrain_draw_frame_core(cpu.X); }

/* ===========================================================================
 * flight_control_integrate subtree — ISR de-transpile (the last transpiled code
 * on the flight VBI path).  Leaves first.  See [[flight-must-be-native]].
 * ======================================================================== */

/* reset_flags_ff @ $4E98 — LDA #$FF then STA $006A/$0063/$2826.
 * All four callers overwrite A or branch immediately after → mem-only contract. */
void reset_flags_ff(void) {
    mem[0x006A] = 0xFF;
    object_index_signed = 0xFF;
    player3_dither_flag = 0xFF;
}

/* load_velocity_from_param_block @ $94BF — seed the object velocity accumulators
 * $2854-$2863 from the param block $28FD-$290A: zero the low fraction bytes, copy
 * the mid bytes, clamp $285B = min($2902+8, $FF), sign-extend-and-x4 the two 16-bit
 * components {$2903,$2904}->$285C-$285E and {$2905,$2906}->$285F-$2861, build
 * $2862/$2863 from {$2909,$290A} (arithmetic >>1 via CMP#$80;ROR, then -1), then
 * tail-push X=$11 to the event ring.  Outputs are mem[]; the ring push goes through
 * the same native ring_push_marked the oracle calls, so it's equivalence-safe. */
void load_velocity_from_param_block(void) {
    /* Copy straight-through fields. */
    mem[0x2854] = 0x00;
    mem[0x2857] = 0x00;
    mem[0x2855] = mem[0x28FD];
    mem[0x2856] = mem[0x28FE];
    mem[0x2858] = mem[0x28FF];
    mem[0x2859] = mem[0x2900];
    mem[0x285A] = mem[0x2901];

    /* $285B = saturating $2902 + 8 (clamp to $FF on overflow). */
    {
        unsigned v = (unsigned)mem[0x2902] + 0x08;
        vel_z_clamp_hi = (v > 0xFF) ? 0xFF : (uint8_t)v;
    }

    /* {$285E:$285D:$285C} = sign_extend24({$2904:$2903}) << 2. */
    {
        int32_t v = (int32_t)(int16_t)(uint16_t)(mem[0x2903] | (mem[0x2904] << 8)) << 2;
        mem[0x285C] = (uint8_t)v;
        mem[0x285D] = (uint8_t)(v >> 8);
        mem[0x285E] = (uint8_t)(v >> 16);
    }

    /* {$2861:$2860:$285F} = sign_extend24({$2906:$2905}) << 2. */
    {
        int32_t v = (int32_t)(int16_t)(uint16_t)(mem[0x2905] | (mem[0x2906] << 8)) << 2;
        mem[0x285F] = (uint8_t)v;
        mem[0x2860] = (uint8_t)(v >> 8);
        mem[0x2861] = (uint8_t)(v >> 16);
    }

    /* {$2863:$2862} = arith(sign_extend16({$290A:$2909}) >> 1), then high byte -= 1. */
    {
        int16_t v = (int16_t)(int16_t)(uint16_t)(mem[0x2909] | (mem[0x290A] << 8)) >> 1;
        mem[0x2862] = (uint8_t)v;
        mem[0x2863] = (uint8_t)((v >> 8) - 1);
    }

    cpu.X = 0x11;                             /* queue event $11 (ring_push_marked reads cpu.X) */
    ring_push_marked();
}

/* bcd_inc_counter_0641 @ $7B88 — CLC; SED; ADC #1; CLD: increment the BCD counter
 * $0641 in decimal (09 -> 10, not 0A).  mem-only contract. */
void bcd_inc_counter_0641(void) {
    cpu.A = placed_item_count_bcd; cpu.C = 0; cpu.D = 1; ADC(0x01); cpu.D = 0;
    placed_item_count_bcd = cpu.A;
}

/* set_place_params_inc_count @ $7B80 — $0045=0, $0046=1, then bump the counter. */
void set_place_params_inc_count(void) {
    bcd_delta_lo = 0x00;
    bcd_delta_hi = 0x01;
    bcd_inc_counter_0641();                   /* 7b8d tail (native) */
}

/* trigger_object_explosion @ $96D9 — INC $0041, seed the explosion sprite/anim
 * pointers ($00DA-$00DD, $28EE), then push X=$0F to the event ring. */
void trigger_object_explosion(void) {
    CL_CNT(g_clExplode);                       /* combat-load: an explosion started */
    game_state = (uint8_t)(game_state + 1);  /* 96d9 INC $0041 */
    mem[0x00DB] = 0x7E;                        /* 96db-96dd */
    anim_counter_2 = 0x7C;                        /* 96df-96e1 */
    mem[0x00DC] = 0x76;                        /* 96e3-96e5 */
    audc_shadow_0 = 0x78;                        /* 96e7-96e9 */
    lock_on_indicator_complete = 0x02;                        /* 96eb-96ed */
    cpu.X = 0x0F;                              /* 96f0 LDX #$0F */
    ring_push_marked();                        /* 96f2 (native; reads cpu.X) */
}

/* reset_object_slot @ $9677 — mark object slot $0036=$80, push X=$0E to the ring. */
void reset_object_slot(void) {
    object_anim_frame = 0x80;                        /* 9677-9679 */
    cpu.X = 0x0E;                              /* 967b LDX #$0E */
    ring_push_marked();                        /* 967d (native; reads cpu.X) */
}

/* enqueue_indicator_event @ $B756 — write indicator HUD params ($0673 from $0035,
 * $0665=$A0, $0681 = ($0014 & 5) + 2), then refresh display field Y=$08. */
void enqueue_indicator_event(void) {
    indicator_evt_pos = indicator_pos;                 /* b756-b758 */
    indicator_evt_char = 0xA0;                        /* b75b-b75d */
    indicator_evt_phase = (uint8_t)((RTCLOK_LOW & 0x05) + 0x02);  /* b760-b767 (AND#5 max 5, +2 no carry) */
    cpu.Y = 0x08;                              /* b76a LDY #$08 */
    ring_push_unmarked();                           /* b76c (native; reads cpu.Y) */
}

/* object_integrate_position @ $930E — integrate an object's 24-bit world position.
 * Four add/subtract blocks update {$2827,$0064,$0065} and {$2828,$0066,$0067}: subtract
 * the velocity $2850/$2851 and decel $2829/$0068; add velocity $2852/$2853 and $282C/$0069.
 * Each block sign-extends its operand's high byte into the 3rd byte via a carry-conditional
 * INC/DEC.  Tail: if both high bytes settled to 0, derive the screen blip $2821 from a
 * heading table $93F3[$0063] (arith >>1 gated by $282D) + $0066, and $2824 from $0064;
 * else $2824 = 0.  mem-only contract (starts SEC; X/Y loaded from mem; no RANDOM). */
void object_integrate_position(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* Each axis is a 24-bit accumulator {hi:mid:lo}; each block adds/subtracts a
     * sign-extended 16-bit operand.  The 6502 does this as a 2-byte add/subtract
     * plus a carry-conditional INC/DEC of the high byte = ordinary signed modular
     * arithmetic, so a signed 32-bit local masked back to 3 bytes is byte-identical. */
    int32_t x = mem[0x2827] | (object_pos_x_lo << 8) | (object_pos_x_hi << 16);
    int32_t y = mem[0x2828] | (object_pos_y_lo << 8) | (object_pos_y_hi << 16);

    x -= (int16_t)ROF_PAIR16(mem[0x2850], mem[0x2851]);   /* 930e: -= velocity */
    y += (int16_t)ROF_PAIR16(mem[0x2852], mem[0x2853]);   /* 932f: += velocity */
    x -= (int16_t)ROF_PAIR16(mem[0x2829], mem[0x0068]);   /* 9350: -= decel    */
    y += (int16_t)ROF_PAIR16(mem[0x282C], mem[0x0069]);   /* 936f: += accel    */

    mem[0x2827]    = (uint8_t)x;  object_pos_x_lo = (uint8_t)(x >> 8);  object_pos_x_hi = (uint8_t)(x >> 16);
    mem[0x2828]    = (uint8_t)y;  object_pos_y_lo = (uint8_t)(y >> 8);  object_pos_y_hi = (uint8_t)(y >> 16);

    /* Once both high bytes settle to 0, derive the screen blip from a heading table
     * (arith >>1 once the sensor angle $282D passes $20, zeroed past $30) + the Y
     * accumulator's mid byte; else clear the blip Y. */
    if (object_pos_x_hi != 0 || object_pos_y_hi != 0) {
        player3_ytop = 0x00;
        return;
    }
    int8_t a = (int8_t)mem[0x93F3 + object_index_signed];        /* 9398 heading table */
    uint8_t ang = mem[0x282D];
    if (ang >= 0x30) a = 0x00;
    if (ang >= 0x20) a = (int8_t)(a >> 1);                       /* arithmetic >>1 */
    player3_xpos = (uint8_t)((uint8_t)a + object_pos_y_lo);
    player3_ytop = object_pos_x_lo;
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* jitter_roll_pitch @ $AA95 — per-frame random walk of the pitch ($0029) and roll
 * ($0026) accumulators, plus a decay of $002E.  $0029 drifts toward $F4 (down up to
 * 2, never past); $0026 walks toward $FB (down) when RANDOM is negative or toward $05
 * (up) when positive, up to 2 and never past; $002E -= 8 with a floor of 0.
 * Reads POKEY RANDOM $D20A once (harness seeds it identically).  mem-only contract. */
void jitter_roll_pitch(void) {
    uint8_t A, Y;
#ifdef ROF_COMBAT_NO_JITTER
    /* COMBAT benchmark only (`make COMBAT=1`; restore with COMBAT_JITTER=1): hold the attitude
     * accumulators still so the ship flies straight.  Every enemy bolt impact calls this from
     * game_state_update's $A99C impact path, and with the AUTO_FIRE autopilot the resulting bank
     * steered the ship into a mountain within seconds — the benchmark window was spent shooting
     * terrain instead of emplacements (measured: 46 impacts, only 3 kills).  It also made the
     * COMBAT vs COMBAT_QUIET pair fly visibly different trajectories, which is the cross-build
     * trap that invalidates their comparison; with this on, both hold heading $0200 / roll 0.
     * The RANDOM read and the $002E decay are KEPT, so only the steering is suppressed. */
    (void)bus_read(0xD20A);                            /* aaa5, kept so RNG consumption is unchanged */
    A = throttle_accum_hi;                             /* aac4 */
    throttle_accum_hi = (A >= 0x08) ? (uint8_t)(A - 0x08) : 0x00;
    (void)Y;
    return;
#endif
    Y = roll_pos_hi;                                   /* aa95 */
    if (Y != 0xF4) { Y--; if (Y != 0xF4) Y--; }        /* aa97-aaa0 */
    roll_pos_hi = Y;                                   /* aaa1 */
    Y = pitch_pos_hi;                                   /* aaa3 */
    A = bus_read(0xD20A);                              /* aaa5 LDA $D20A */
    if (A & 0x80) { if (Y != 0xFB) { Y--; if (Y != 0xFB) Y--; } }  /* aaa8 BPL -> neg: toward $FB */
    else          { if (Y != 0x05) { Y++; if (Y != 0x05) Y++; } }  /*           pos: toward $05 */
    pitch_pos_hi = Y;                                   /* aac1 */
    A = throttle_accum_hi;                                   /* aac4 SEC; SBC #$08; clamp 0 */
    throttle_accum_hi = (A >= 0x08) ? (uint8_t)(A - 0x08) : 0x00;
}

/* mul_u8 @ $9821 — shift-add multiply driven by the multiplicand bits in $28D6: while
 * $006B != 0, shift $28D6 left and, for each 1 bit shifted out, add (the LSR'd) $006B
 * into the accumulator.  Both $006B and $28D6 are consumed (shifted to 0/out).  RESULT
 * IS RETURNED IN cpu.A — the caller reads it (e.g. BIT right after) — so the contract
 * is mem[$006B]/mem[$28D6] AND cpu.A. */
void mul_u8(void) {
    uint8_t A = 0x00, c = 0;                    /* 9821 LDA #0 */
    goto check;                                 /* 9823 BEQ (A==0) */
add:                                            /* 9825 */
    c = mul_multiplicand & 1; mul_multiplicand >>= 1;     /* LSR $006B */
    { uint16_t t = (uint16_t)A + mul_multiplicand + c; c = (uint8_t)(t >> 8); A = (uint8_t)t; }  /* ADC $006B */
check:                                          /* 9829 */
    { uint8_t v = mem[0x28D6]; c = (uint8_t)(v >> 7); mem[0x28D6] = (uint8_t)(v << 1); }    /* ASL $28D6 */
    if (c) goto add;                            /* 982c BCS */
    c = mul_multiplicand & 1; mul_multiplicand >>= 1;     /* 982e LSR $006B */
    if (mul_multiplicand != 0) goto check;           /* 9830 BNE (Z from the shifted result) */
    cpu.A = A;                                  /* 9832 RTS — result in A */
}

/* Byte-exact 256x256 product table for mul_u8's bit-serial multiply.  mul_u8 is NOT a plain
 * product: it's a round-half-up-per-bit multiply with multiplicand-precision truncation (proven
 * by brute force: it differs from round(M*N/256) on ~1/3 of the input domain), so no single
 * 68000 mulu/muls reproduces it.  A lookup table does — byte-exact — and turns the ~8-iteration
 * shift/add/branch loop into one indexed load.  Filled ONCE (lazily) from the exact bit-serial
 * reference; costs 64KB of RAM (BSS).  The three native call sites (compute_obj_rel_angle_scale x2,
 * flight_control_integrate) use this instead of calling mul_u8 and leave the operands $6B/$28D6
 * untouched: mul_u8 consumes them, but those consumed values are dead in all callers (proven
 * byte-identical by make validate FN=compute_obj_rel_angle_scale/flight_control_integrate).
 * mul_u8 itself stays for the still-transpiled caller + as the validation oracle. */
/* Non-static: MulTableAssembler.s (ROF_MULTABLE_ASM) fills this directly. */
uint8_t     g_mulTable[256u * 256u];
static int  g_mulTableReady = 0;
static uint8_t mul_u8_bitserial(uint8_t M, uint8_t N) {
    uint8_t A = 0, c = 0; goto ck;
ad: c = M & 1; M >>= 1; { uint16_t t = (uint16_t)A + M + c; c = (uint8_t)(t >> 8); A = (uint8_t)t; }
ck: { uint8_t v = N; c = (uint8_t)(v >> 7); N = (uint8_t)(v << 1); }
    if (c) goto ad;
    c = M & 1; M >>= 1;
    if (M != 0) goto ck;
    return A;
}
/* C reference fill: every entry via the exact bit-serial multiply (the validation oracle
 * and the SDL/non-asm shipping build).  The asm twin uses mul_u8's symmetry to compute
 * only the triangle, so filling ALL entries here keeps the oracle independent. */
static void mul_table_fill_c(uint8_t* t) {
    for (unsigned m = 0; m < 256; m++)
        for (unsigned n = 0; n < 256; n++)
            t[(m << 8) | n] = mul_u8_bitserial((uint8_t)m, (uint8_t)n);
}
#if defined(ROF_MULTABLE_ASM)
extern void mul_table_build_asm(void);          /* fills g_mulTable via the symmetric triangle */
#endif
#if defined(ROF_MULTABLE_ASM) && defined(ROF_MULTABLE_VERIFY)
static uint8_t g_mulTableRef[256u * 256u];
volatile unsigned long g_mulTableMismatch = 0xFFFFFFFFu;  /* read via gdb; 0 = byte-identical */
#endif
/* Public one-shot builder: call ONCE at startup (before flight) so the ~64KB table is
 * never built lazily inside the flight VBI ISR — that first-flight build (65536 bit-serial
 * products) took ~3.6s on the 7MHz 68000 and froze the ISR + whole display at flight entry.
 * Idempotent; safe to call anytime. */
void rof_mul_table_init(void) {
    if (g_mulTableReady) return;
#if defined(ROF_MULTABLE_ASM) && defined(ROF_MULTABLE_VERIFY)
    mul_table_fill_c(g_mulTableRef);            /* oracle: full bit-serial */
    mul_table_build_asm();                       /* twin: symmetric triangle -> g_mulTable */
    { unsigned long bad = 0;
      for (unsigned i = 0; i < 256u * 256u; i++) if (g_mulTable[i] != g_mulTableRef[i]) bad++;
      g_mulTableMismatch = bad; }
#elif defined(ROF_MULTABLE_ASM)
    mul_table_build_asm();
#else
    mul_table_fill_c(g_mulTable);
#endif
    g_mulTableReady = 1;
}
static uint8_t mul_u8_lookup(uint8_t M, uint8_t N) {
    if (!g_mulTableReady) rof_mul_table_init();   /* safety fallback if init was skipped */
    return g_mulTable[((unsigned)M << 8) | N];
}

/* compute_target_blip_position @ $9713 — derive the target-blip screen coords $0021/$0027
 * from the nearest-point range latch $27F7/$27F8, the depth $0034, sensor masks
 * $1027/$1057, and the parallax samples $2912/$2913.  Several early returns.  mem-only
 * (starts SEC; all inputs from mem; no entry regs).  NOTE the $976b SBC reads a
 * PATH-DEPENDENT carry (0 when reached via the $975f BCC, 1 via the $9763 BMI). */
void compute_target_blip_position(void) {
    /* Blip X ($0021) = signed half-difference of the range latch $27F7/$27F8; near
     * targets with a tiny delta clear it. */
    pitch_velocity = (uint8_t)(mem[0x27F7] - mem[0x27F8]);           /* 9713: range delta */
    uint8_t avg = (uint8_t)(mem[0x27F7] + mem[0x27F8]) >> 1;         /* 971c: range midpoint */
    if (avg < terrain_depth_step) {
        uint8_t mag = pitch_velocity;
        if (mag & 0x80) mag = (uint8_t)-mag;                        /* |delta| */
        if (mag < 0x10) pitch_velocity = 0x00;
    }

    if (mem[0x005D] == 0) { roll_velocity = 0xC8; return; }         /* 9738: no ground proximity */

    /* Blip Y ($0027) base: $38, or $00 when both sensor masks are clear AND the
     * clearance $0070 has reached $2A. */
    uint8_t base = 0x38;
    if ((mem[0x1027] & 0xAA) == 0 && (mem[0x1057] & 0xAA) == 0 && mem[0x0070] >= 0x2A)
        base = 0x00;
    roll_velocity = base;

    /* Depth term added to the base: $C8 when deep ($0034 >= $2A) and not pitched down;
     * otherwise ($20 - depth, with a path-dependent borrow) arith-halved, negatives kept. */
    uint8_t term;
    if (terrain_depth_step >= 0x2A && !(roll_pos_hi & 0x80)) {
        term = 0xC8;                                                /* 9765 */
    } else {
        int carry = (terrain_depth_step >= 0x2A);                   /* 0 via BCC(975f), 1 via BMI(9763) */
        uint8_t a = (uint8_t)((int8_t)(uint8_t)(0x20 - terrain_depth_step - (1 - carry)) >> 1);
        term = (a & 0x80) ? a : 0x00;                               /* 9770 BMI keep else 0 */
    }
    roll_velocity = (uint8_t)(term + roll_velocity);                /* 9774 */

    /* Parallax samples nudge the blips when out of their dead-bands. */
    uint8_t px = mem[0x2912];                                       /* 9779 */
    if (px >= 0x30 && px < 0xD1) return;
    pitch_velocity = (uint8_t)((uint8_t)(px << 1) + pitch_velocity);

    uint8_t py = mem[0x2913];                                       /* 978b */
    if (py >= 0x20 && py < 0xE1) return;
    roll_velocity = (uint8_t)((uint8_t)((py << 1) ^ 0xFF) + roll_velocity);
}

/* obj_table_scan_replace @ $4E1C — place the entry value (cpu.A) into a free object
 * slot.  Retries up to cpu.Y times ($288C): each pass picks a RANDOM start index, then
 * scans with stride $43 (coprime to 256 -> visits all indices, returns to start) for a
 * slot with $0900,X < $30 and $0A00,X == 0 whose index isn't already in the $2276[0..$2C]
 * table; the first match gets the entry value written to $0A00,X.  Reads entry A/Y and
 * POKEY RANDOM $D20A (harness seeds it); mem-only effects ($281D/$288B/$288C/$0A00,X). */
void obj_table_scan_replace(void) {
    uint8_t X;
    obj_scan_fill_val = cpu.A;                          /* 4e1c entry value */
    obj_scan_pass_count = cpu.Y;                           /* 4e1f retry count */
    do {                                           /* L_4e22 */
        X = bus_read(0xD20A);                      /* 4e22 LDX $D20A */
        obj_scan_start_idx = X;                           /* 4e25 */
        for (;;) {                                 /* L_4e28 scan with stride $43 */
            if (mem[0x0900 + X] < 0x30 && mem[0x0A00 + X] == 0) {  /* 4e28-4e32 */
                uint8_t inTable = 0;
                for (int y = 0x2C; y >= 0; y--)    /* L_4e37: X already in $2276[]? */
                    if (X == mem[0x2276 + y]) { inTable = 1; break; }
                if (!inTable) { mem[0x0A00 + X] = obj_scan_fill_val; break; }  /* 4e3f-4e45 write + done */
            }
            X = (uint8_t)(X + 0x43);               /* 4e48 X += $43 */
            if (X == obj_scan_start_idx) break;           /* 4e4d-4e50 full cycle -> stop */
        }
        obj_scan_pass_count = (uint8_t)(obj_scan_pass_count - 1);  /* 4e52 DEC $288C */
    } while (obj_scan_pass_count != 0);                    /* 4e55 BNE L_4e22 */
}

/* obj_table_scan_a_c8 @ $4E1A — preset A=$C8, then place it via obj_table_scan_replace
 * (caller supplies Y = retry count). */
void obj_table_scan_a_c8(void) {
    cpu.A = 0xC8;                /* 4e1a */
    obj_table_scan_replace();    /* native; reads cpu.A/Y */
}

/* obj_table_scan_y1_c8 @ $4E18 — preset Y=1 (single pass), tail to obj_table_scan_a_c8. */
void obj_table_scan_y1_c8(void) {
    cpu.Y = 0x01;                /* 4e18 */
    obj_table_scan_a_c8();       /* native */
}

/* HUD display-field refresh chain ($4EA2-$4EB7).  Each link stores the entry value
 * and/or sets the field index Y, then tail-chains; ring_push_unmarked (native) pushes the
 * indexed field to the display.  store_676_init/set_hud_fields read entry cpu.A. */
void refresh_hud_fields_0d_0e(void) {            /* $4EB2 (Y set by caller) */
    ring_push_unmarked();                             /* 4eb2 */
    cpu.Y++;                                     /* 4eb5 INY */
    ring_push_unmarked();                             /* 4eb6 */
}
void refresh_hud_field_0d_entry(void) {          /* $4EB0 */
    cpu.Y = 0x0D;                                /* 4eb0 */
    refresh_hud_fields_0d_0e();
}
void refresh_hud_field_0b(void) {                /* $4EAB */
    cpu.Y = 0x0B;                                /* 4eab */
    ring_push_unmarked();                             /* 4ead */
    refresh_hud_field_0d_entry();
}
void set_hud_fields_678_679(void) {              /* $4EA5 */
    hud_field_678 = cpu.A;                          /* 4ea5 */
    sfx_env_freq_val = cpu.A;                          /* 4ea8 */
    refresh_hud_field_0b();
}
void store_676_init(void) {                      /* $4EA2 */
    timer_676 = cpu.A;                          /* 4ea2 */
    set_hud_fields_678_679();
}

/* step_object_along_axes @ $9473 — step the object's screen pos $0023/$0024 by ±$14
 * along the sign of $0024 (decrement clamps to 0), then either decrement depth
 * $0033/$0034 by $30 (when $0070 != 0) or, on first settle ($003D==0 && $0678==0),
 * bump $003D and (if $0063 >= 0) reset the gameplay flags.  mem-only contract. */
void step_object_along_axes(void) {
    /* Step the 16-bit screen pos {$0024:$0023} by $14 toward 0: increment when it's
     * negative, else decrement clamped at 0. */
    uint16_t pos = mem[0x0023] | (mem[0x0024] << 8);
    if (mem[0x0024] & 0x80) {                              /* 9473: negative -> increment */
        pos = (uint16_t)(pos + 0x14);
    } else {                                               /* 9485: decrement, clamp 0 */
        pos = (pos >= 0x14) ? (uint16_t)(pos - 0x14) : 0;
    }
    mem[0x0023] = (uint8_t)pos;
    mem[0x0024] = (uint8_t)(pos >> 8);

    if (mem[0x0070] != 0) {                                /* 9498: still above terrain -> sink depth $30 */
        uint16_t depth = terrain_depth_frac | (terrain_depth_step << 8);
        depth = (uint16_t)(depth - 0x30);
        terrain_depth_frac = (uint8_t)depth;
        terrain_depth_step = (uint8_t)(depth >> 8);
    } else if (mem[0x003D] == 0 && hud_field_678 == 0) {  /* 94ac: first settle -> latch + reset flags */
        mem[0x003D]++;
        if (!(object_index_signed & 0x80)) reset_flags_ff();
    }
}

/* Write one cockpit dial cell and register it for the Amiga's writer-driven cockpit decode.
 * The cell address comes from the $4581 column-pointer table; only writes that land in the
 * cockpit mode4/modeD screen RAM ($332D-$355C) are tracked (PMG-buffer destinations from the
 * same table fall outside the range and are just written).  bus_write to this RAM range is a
 * plain mem[] store on every platform (no hardware/shadow path), so store directly. */
static void dial_cell_write(uint16_t addr, uint8_t val) {
    if (addr >= 0x332Du && addr < 0x355Du && mem[addr] != val) platform_cockpit_dirty(addr, 1u);
    mem[addr] = val;
}

/* draw_object_column @ $43E8 — draw one vertical cockpit dial/indicator bar, column by column.
 * Draws columns from `startCol` down to (dl_y4 + 1) inclusive.  Each column's target cell
 * address is read from the $4581 pointer table (indexed by 2*column).  A column below the lit
 * threshold (bar_col_threshold) is drawn "lit" (glyph bit7 set), otherwise "empty".  Odd
 * columns are two cells tall (a lower cell is written as well).  Shared by the dial bars
 * (setup_dial_bar_draw), the left-indicator bar (draw_dial_bar_column) and player-3 lock-on.
 * ($4581 = per-column cell-pointer table, still unnamed — see docs/rename.md.) */
static void draw_object_column_core(uint8_t startCol) {
    const uint8_t threshold = bar_col_threshold;   /* $BF: columns below this are lit */
    const uint8_t stop      = dl_y4;               /* $BE: loop ends when the counter reaches this */
    /* The first column drawn is the entry index; every column after that is taken from the
       $BD counter (which the callers seed equal to startCol).  Faithful to the 6502 loop:
       draw column, DEC $BD, reload column from $BD, repeat until it hits the stop index. */
    uint8_t counter = dl_y3;
    uint8_t col = startCol;
    uint16_t ptr = 0;
    int odd = 0;
    for (;;) {
        ptr = ROF_PAIR16(mem[0x4581 + col * 2], mem[0x4582 + col * 2]);
        odd = col & 1;                             /* odd columns are 2 cells tall */
        const int lit = col < threshold;
        uint8_t glyph = odd ? 0xB7 : 0xB4;         /* lit glyph (odd / even column) */
        if (!lit) glyph &= 0x7F;                    /* empty glyph clears bit7 */
        dial_cell_write(ptr, glyph);
        if (odd) dial_cell_write((uint16_t)(ptr + 1), lit ? 0xB8 : 0x38);   /* lower cell */
        counter--;
        col = counter;
        if (col == stop) break;
    }
    /* Leave the 6502 scratch in its post-loop state (the last column drawn): the column
       pointer ($BB/$BC), the 2-tall flag ($C0), and the counter ($BD, now == stop). */
    dl_y1 = (uint8_t)ptr;
    dl_y2 = (uint8_t)(ptr >> 8);
    object_col_flag = odd ? 0xB7 : 0x00;
    dl_y3 = counter;
}

void draw_object_column(void) { draw_object_column_core(cpu.A); }

/* setup_dial_bar_draw @ $444A — draw a dial bar with fill limit `limit`: set the lit
 * threshold ($BF) and stop index ($BE = 7, i.e. draw columns 15..8), then draw the column.
 * (draw_object_column_core owns the loop counter $BD, leaving it at the stop index.) */
static void setup_dial_bar_draw_core(uint8_t limit) {
    bar_col_threshold = limit;   /* $BF: columns below this are lit */
    dl_y4 = 0x07;                /* $BE: stop index */
    dl_y3 = 0x0F;                /* $BD: seed the loop counter ($4450 LDA #$0F; STA $BD) —
                                    draw_object_column_core reads its counter from here */
    draw_object_column_core(0x0F);
}
void setup_dial_bar_draw(void) { setup_dial_bar_draw_core(cpu.A); }

/* draw_cockpit_dial_bar @ $4447 — draw a cockpit dial bar for value `v`: the lit-column
 * threshold is v + 8 (values 0..7 fill 8..15 columns). */
static void draw_cockpit_dial_bar_core(uint8_t v) {
    setup_dial_bar_draw_core((uint8_t)(v + 0x08));
}
void draw_cockpit_dial_bar(void) { draw_cockpit_dial_bar_core(cpu.A); }

/* object_step_and_collide @ $9552 — advance an object's position accumulators by its
 * velocity ($2854-$285B += $285C-$2863, 3-byte chained adds with the 12-bit map coords
 * mirrored to $27FD-$2800 and the altitude $285B clamped on overflow), run the player
 * hit test, then resolve terrain/object collision at the derived cell index $2864:
 * sample the terrain height, and on a real collision explode the occupant and dispatch
 * the pickup type ($64/$80/other -> $0044/$004D/$007E + counters).  Straight-line (no
 * loops).  PHA/PLA at $9641/$964d use the real 6502 stack (cpu.S) because the nested
 * trigger_object_explosion -> ring_push consumes it; the same native chain runs in both
 * the native and oracle paths, so it stays equivalent.  mem-only contract. */
void object_step_and_collide(void) {
    ROF_MEMBASE_DECL(mb);   /* 78 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    O2_DECL(); O2_START(g_osCalls); O2_LAP(g_osNop);   /* empty lap = this level's floor */
    /* Integrate the three position accumulators by their velocities.  X and Z are 24-bit
     * with the high byte masked to 12-bit map coords (mirrored to $27FD-$2800); altitude
     * is 16-bit with a signed saturating clamp. */
    {   /* X: {$2856:$2855:$2854} += {$285E:$285D:$285C} */
        uint32_t v = mem[0x2854] | (mem[0x2855] << 8) | (mem[0x2856] << 16);
        v += mem[0x285C] | (mem[0x285D] << 8) | (mem[0x285E] << 16);
        uint8_t lo = (uint8_t)(v >> 8), hi = (uint8_t)(v >> 16) & 0x0F;
        mem[0x2854] = (uint8_t)v;
        mem[0x2855] = lo; map_x_lo = lo;
        mem[0x2856] = hi; map_x_hi = hi;
    }
    {   /* Z: {$2859:$2858:$2857} += {$2861:$2860:$285F} */
        uint32_t v = mem[0x2857] | (mem[0x2858] << 8) | (mem[0x2859] << 16);
        v += mem[0x285F] | (mem[0x2860] << 8) | (mem[0x2861] << 16);
        uint8_t lo = (uint8_t)(v >> 8), hi = (uint8_t)(v >> 16) & 0x0F;
        mem[0x2857] = (uint8_t)v;
        mem[0x2858] = lo; map_z_lo = lo;
        mem[0x2859] = hi; map_z_hi = hi;
    }
    uint8_t altCarry;
    {   /* altitude: {$285B:$285A} += {$2863:$2862}, saturate on signed over/underflow */
        uint16_t lo  = (uint16_t)mem[0x285A] + mem[0x2862];
        mem[0x285A]  = (uint8_t)lo;
        uint16_t sum = (uint16_t)vel_z_clamp_hi + mem[0x2863] + (lo >> 8);
        altCarry = (uint8_t)(sum >> 8);
        uint8_t a = (uint8_t)sum;
        if (altCarry) { if (!(mem[0x2863] & 0x80)) a = 0xFF; }   /* +overflow -> $FF */
        else          { if (mem[0x2863] & 0x80)   a = 0x00; }    /* -underflow -> $00 */
        vel_z_clamp_hi = a;
    }

    O2_LAP(g_osAcc);

    /* The player hit-test reads the altitude add's carry-out as its entry carry. */
    cpu.C = altCarry;
    if (!(object_index_signed & 0x80)) { IN_CNT(g_osHitCalls); check_player_proximity_hit(); }
    O2_LAP(g_osHit);

    /* Map cell index $2864 = (Z high-nibble << 4) | X high-nibble. */
    uint8_t zHi = (uint8_t)((mem[0x2859] + (mem[0x2858] >> 7)) << 4);
    uint8_t xHi = (uint8_t)((mem[0x2856] + (mem[0x2855] >> 7)) & 0x0F);
    uint8_t cell = (uint8_t)(zHi | xHi);
    dl_y1 = zHi;                                 /* $00BB scratch (the 95d4 store) */
    map_cell_index = cell;

    /* An occupant present + tall enough + both screen coords outside the [$30,$D0) centre
     * band explodes immediately; otherwise sample the terrain height first. */
    int explode = mem[0x0A00 + cell] != 0;
    if (explode) {
        unsigned h = mem[0x0900 + cell] + 0x10;
        if (h > 0xFF) h = 0xFF;
        explode = h >= vel_z_clamp_hi &&
                  !(mem[0x2855] >= 0x30 && mem[0x2855] < 0xD0) &&
                  !(mem[0x2858] >= 0x30 && mem[0x2858] < 0xD0);
    }
    if (explode) IN_CNT(g_osExplode);
    O2_LAP(g_osCell);
    if (!explode) {
        IN_CNT(g_osLerpCalls);
        sample_terrain_height_bilerp();
        O2_LAP(g_osLerp);
        if (terrain_height_sample < vel_z_clamp_hi) { IN_CNT(g_osEarlyRet); return; }
        if (mem[0x2855] >= 0x30 && mem[0x2855] < 0xD0) { reset_object_slot(); O2_LAP(g_osTail); return; }
        if (mem[0x2858] >= 0x30 && mem[0x2858] < 0xD0) { reset_object_slot(); O2_LAP(g_osTail); return; }
    }

    /* $9635: explode the occupant, then dispatch by its pickup type. */
    uint8_t occ = mem[0x0A00 + cell];
    if (occ == 0 || occ >= 0xF8) { reset_object_slot(); O2_LAP(g_osTail); return; }
    mem[0x0100 | cpu.S] = occ; cpu.S--;          /* PHA: preserve occ across the explosion (mem-equivalent) */
    CL_CNT(g_clShotHit);                         /* combat-load: our shot destroyed an occupant */
    mem[0x0A00 + cell] = 0xFC;
    map_cell_hit_marker = 0xFC;
    trigger_object_explosion();
    cpu.S++; occ = mem[0x0100 | cpu.S];          /* PLA */
    if (occ < 0x64) { reset_object_slot(); O2_LAP(g_osTail); return; }
    if (occ != 0x64) {
        if (occ == 0x80) { set_place_params_inc_count(); timer_or_counter = 0x40; }
        else             { countdown_show_char_0620(); timer_or_counter = 0x49; }
        reset_object_slot(); O2_LAP(g_osTail); return;
    }
    set_place_params_inc_count();                /* occ == $64 (pilot pickup) */
    mem[0x004D] = 0x28;
    lock_on_indicator_state = (uint8_t)(lock_on_indicator_state | 0x80);
    reset_object_slot();
    O2_LAP(g_osTail);
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* reset_indicator_event @ $B786 — clear $0035, then enqueue the indicator event. */
void reset_indicator_event(void) {
    indicator_pos = 0x00;            /* b786-b788 */
    enqueue_indicator_event();     /* b78a (native) */
}

/* countdown_show_char_0620 @ $7B3C — when the countdown $0620 is running, place the
 * countdown glyph into a free object slot (value $C9 with 1 retry when $0620==1, else
 * the default $C8 via obj_table_scan_y1_c8), then decrement $0620.  mem-only. */
void countdown_show_char_0620(void) {
    uint8_t Y = stage_geom_0620;                          /* 7b3c */
    if (Y == 0) return;                               /* 7b3f BEQ */
    if (Y == 1) { cpu.A = 0xC9; cpu.Y = Y; obj_table_scan_replace(); }  /* 7b41-7b47 */
    else        { obj_table_scan_y1_c8(); }           /* 7b4d (sets Y=1, A=$C8) */
    stage_geom_0620 = (uint8_t)(stage_geom_0620 - 1);         /* 7b50 DEC $0620 */
}

/* check_object_in_target_box @ $93BD — if the object has settled ($0065==0 && $0067==0)
 * and its screen pos is inside the target box ($0064 in [$34,$AC), $0066 in [$1A,$6A)),
 * trigger the in-box event: set $003B=$20, enqueue the indicator, jitter, and arm the
 * box markers $2892/$3355/$2891.  mem-only. */
void check_object_in_target_box(void) {
    if (object_pos_x_hi != 0 || object_pos_y_hi != 0) return;          /* 93bd-93c3 */
    uint8_t a = object_pos_x_lo;
    if (a < 0x34 || a >= 0xAC) return;                         /* 93c5-93cd */
    a = object_pos_y_lo;
    if (a < 0x1A || a >= 0x6A) return;                         /* 93cf-93d7 */
    vobj_path_flag = 0x20;                                        /* 93d9-93db */
    reset_indicator_event();                                  /* 93dd (native) */
    jitter_roll_pitch();                                      /* 93e0 (native) */
    intro_sfx_delay = 0x00;                                       /* 93e3-93e5 */
    special_state_color = 0x34;                                       /* 93e8-93ea */
    intro_phase_counter = 0x1E;                                       /* 93ed-93ef */
}

/* check_player_proximity_hit @ $9680 — test whether object slot $0036 is within hit
 * range of the player: |($006A+4)-$0036| < 4, then a Manhattan-ish distance from
 * ($0038,$0039) vs ($2824,$2821) (folded via >>1>>1) compared to the per-shape
 * threshold $96F5[$0036].  On a hit: clear $2826 and fire the pickup/explosion chain.
 * Reads ENTRY CARRY ($9682 ADC #$04).  mem-only contract. */
void check_player_proximity_hit(void) {
    ROF_MEMBASE_DECL(mb);   /* 67 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* Reject unless object slot $0036 is within hit range of the player ship index:
     * |($006A + 4 + entryC) - $0036| < 4.  (The +4 ADC rotates in the entry carry.) */
    uint8_t near = (uint8_t)(mem[0x006A] + 0x04 + (cpu.C & 1));      /* 9680: entry carry */
    uint8_t ds = (uint8_t)(near - object_anim_frame);
    if (near < object_anim_frame) ds = (uint8_t)-ds;
    if (ds >= 0x04) return;

    /* Folded screen distance |row-$0C-blipY| + |($0039+$42)-blipX|, halved twice,
     * compared to the per-shape threshold $96F5[$0036]. */
    int b1 = (vobj_row_count < 0x0C);                              /* borrow out of SBC #$0C */
    int dyt = (int)(uint8_t)(vobj_row_count - 0x0C) - player3_ytop - b1;  /* then SBC $2824 */
    uint8_t dy = (uint8_t)dyt;
    if (dyt < 0) dy = (uint8_t)-dy;
    dl_y1 = dy;                                                     /* $00BB scratch (969f) */

    uint8_t lhsX = (uint8_t)(mem[0x0039] + 0x42);
    uint8_t dx = (uint8_t)(lhsX - player3_xpos);
    if (lhsX < player3_xpos) dx = (uint8_t)-dx;

    if ((uint8_t)((dx + dy) >> 2) >= mem[0x96F5 + object_anim_frame]) return;

    /* HIT: clear the dither flag, fire the pickup/explosion chain, add 250 to the score,
     * and jitter the terrain. */
    player3_dither_flag = 0x00;
    reset_indicator_event();
    reset_object_slot();
    trigger_object_explosion();
    bcd_delta_lo = 0x50;
    bcd_delta_hi = 0x02;
    bcd_inc_counter_0641();
    terrain_jitter_column();
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* compute_obj_rel_angle_scale @ $97A0 — build a 10-bit relative angle from $2885/$2886,
 * derive sign/quadrant ($2882, $28D7/$28D8 indices), look up scale factors in $4EB9, and
 * multiply by $002E (two mul_u8 passes) to produce the scaled coords $002B and $2881.
 * NOTE: the first ROL A rotates in the ENTRY CARRY -> contract includes entry C. */
void compute_obj_rel_angle_scale(void) {
    /* Rotate {$2886:$2885} left twice through the entry carry to form the 10-bit angle;
     * only the high byte ($006B) feeds the rest. */
    uint16_t w = heading_lo | (heading_hi << 8);
    int carry = cpu.C & 1;                       /* ENTRY CARRY -> first ROL */
    for (int i = 0; i < 2; i++) { int nc = (w >> 15) & 1; w = (uint16_t)((w << 1) | carry); carry = nc; }
    mul_multiplicand = (uint8_t)(w >> 8);
    uint8_t hi = mul_multiplicand;

    /* Top two bits give the quadrant: bit7 -> X-axis sign ($002C), bit6 -> which of the
     * scale-index pair $28D7/$28D8 gets complemented; $2882 = XOR of the two sign bits. */
    uint8_t signX = (hi & 0x80) ? 0xFF : 0x00;
    uint8_t signSel = (hi & 0x40) ? 0xFF : 0x00;
    uint8_t sel = (hi >> 6) & 1;
    mem[0x002C] = signX;
    mem[0x2882] = (uint8_t)(signSel ^ signX);

    /* Low 6 bits index the scale table; one of the pair folds to its complement + 1. */
    uint8_t ang = (uint8_t)(hi & 0x3F);
    mem[0x28D7] = ang;
    mem[0x28D8] = ang;
    mem[0x28D7 + sel] = (uint8_t)((ang ^ 0x3F) + 1);

    /* X scale: $4EB9[$28D8] * throttle -> $002B, complemented (CLC;SBC = -x-1) when $002C set. */
    mem[0x28D6] = throttle_accum_hi;
    mul_multiplicand = mem[0x4EB9 + mem[0x28D8]];
    cpu.A = mul_u8_lookup(mul_multiplicand, mem[0x28D6]);  /* byte-exact table (was mul_u8) */
    mem[0x002B] = cpu.A;
    if (cpu.A == 0) mem[0x002C] = 0x00;
    if (mem[0x002C] != 0) mem[0x002B] = (uint8_t)(mem[0x002C] - mem[0x002B] - 1);

    /* Z scale: $4EB9[$28D7] * throttle -> $2881, complemented when $2882 set. */
    mem[0x28D6] = throttle_accum_hi;
    mul_multiplicand = mem[0x4EB9 + mem[0x28D7]];
    cpu.A = mul_u8_lookup(mul_multiplicand, mem[0x28D6]);  /* byte-exact table (was mul_u8) */
    mem[0x2881] = cpu.A;
    if (cpu.A == 0) mem[0x2882] = 0x00;
    if (mem[0x2882] != 0) mem[0x2881] = (uint8_t)(mem[0x2882] - mem[0x2881] - 1);
}

/* flight_control_integrate @ $8E5B — the master per-frame flight step (called from the
 * in-flight VBI at $51B9).  In one pass it:
 *   - reads the joystick (PORTA $D300) and derives this frame's pitch/roll rates;
 *   - integrates the ship's pitch angle ($25/$26), roll/heading angle ($28/$29),
 *     throttle ($2D/$2E), compass heading ($2885/$2886) and 24-bit world position
 *     (world_x $2887, world_z $2889, depth $33/$34), clamping each to its legal range;
 *   - refreshes the cockpit HUD / engine-sound fields;
 *   - steps the currently-active world object and rolls the 7-frame attitude history ring
 *     that delays the canopy-pillar / horizon geometry.
 * It reads RANDOM ($D20A) for the throttle jitter and the lock-on countdown.  No entry
 * registers/carry are read; the validation harness compares full mem[] against the
 * transliterated $8E5B oracle (flight_control_integrate__t6502).
 *
 * No bus_write() here is a no-op on the Amiga — the only hardware touched is the two
 * bus_read()s (PORTA joystick + POKEY RANDOM), both of which are real reads on Amiga. */
/* ⭐ PRICING STEP FOR THE ASM TWIN (2026-08-12) — read this before writing one.
 *
 * The disassembly of this function at -O2 is 4678 bytes / 1245 instructions, and **362 of those
 * instructions carry an absolute-LONG operand** (`move.b 656b1 <mem+0x25>,d3`) against only 19
 * that use a base register.  GCC even emits `lea mem,a2` and `lea cpu,a3` in the prologue and
 * then addresses almost everything absolutely anyway.  On the 68000 `move.b (xxx).L,Dn` is 16
 * cycles / 6 bytes where `move.b d16(An),Dn` is 12 / 4 — so the function is paying 4 cycles and
 * 2 bytes per access for nothing.  That is the single biggest thing a hand-asm twin would fix,
 * and it needs no asm at all: `mem` only has to be laundered through an address register so GCC
 * cannot constant-fold it back to an absolute.
 *
 * The empty asm below does exactly that and nothing else — no instruction is emitted, GCC simply
 * loses the knowledge that `mbase == mem` and is forced to keep it in a (callee-saved) address
 * register across the whole body.  Every `mem[...]` and every `symbols.csv` lvalue alias
 * (`pitch_pos_lo` -> `mem[0x25]` -> `mbase[0x25]`) then becomes `d16(An)`.  This is safe by
 * construction: same array, same accesses, same order, and `make validate FN=flight_control`
 * still byte-compares the whole thing against the $8E5B transliteration.
 *
 * ⚠ Every address this function touches is below $8000 (highest is $291E), so the signed 16-bit
 * displacement reaches all of them from a single base.  A future edit that reaches above $7FFF
 * is still CORRECT (GCC picks another mode), just not free.
 *
 * ⛔ NOT the same thing as the closed `register uint8_t* m asm("a5")` global-register idea
 * ([[feedback-volatile-codegen-tax]]): this base is function-local and callee-saved, so the hand
 * written .s files and the VBI are unaffected.
 *
 * `make FCI_NOBASE=1` reverts to plain absolute addressing for an A/B.
 *
 * ⭐ AND IT IS PROVEN, not argued.  `make validate FN=flight_control_integrate FCIBASE=1` compiles
 * this same source transformation on the HOST (portable "r" constraint instead of m68k's "a") and
 * byte-compares the result against the $8E5B transliteration over 8000 cases: **0 mem mismatch**.
 * That is what closes the real risk here, which is not a GCC bug — it is the macro rescan quietly
 * failing to reach some use (every access in the body is a subscript, and every symbols.csv alias
 * expands to `mem[...]`, but "I checked" is not a measurement).  ⚠ The host arm casts `volatile`
 * off `mem`, which is only sound because the validate harness is single-threaded; it is a TEST
 * configuration, never a host shipping one. */
#if defined(ROF_PLATFORM_AMIGA) && !defined(ROF_FCI_NOBASE)
#define ROF_FCI_BASE 1
#define ROF_FCI_BASE_REG "a"          /* m68k: force an ADDRESS register */
#elif defined(ROF_FCI_BASE_HOSTTEST)
#define ROF_FCI_BASE 1
#define ROF_FCI_BASE_REG "r"          /* host equivalence test: any register */
#endif
static void flight_control_integrate_impl(void) {
#ifdef ROF_FCI_BASE
    uint8_t* mbase;
    __asm__ ("" : "=" ROF_FCI_BASE_REG (mbase) : "0"((uint8_t*)mem));
#define mem mbase
#endif
    IN_DECL(); IN_START();
    /* ---- Steering: derive the roll rate (and a dial-based pitch trim) from the stick ----
     * Only while joystick_saved==2 (active flight) and no colour-clear sweep in progress. */
    if (joystick_saved == 0x02 && clear_colors_done_003E == 0) {
        /* base trim from the cockpit dial index (carries dial bit1 into the +$20 add) */
        pitch_velocity = (uint8_t)((dial_draw_index >> 2) + 0x20 + ((dial_draw_index >> 1) & 1));
        uint8_t joy = bus_read(0xD300);                 /* PORTA, active-low direction bits */
        if (!(joy & 0x04))      pitch_velocity ^= 0xFF; /* bit2 released -> invert trim */
        else if (joy & 0x08)    pitch_velocity = 0x00;  /* bit3 -> zero trim */

        uint8_t roll_rate;
        if (mem[0x005D] == 0) {
            roll_rate = 0xD0;                           /* near-ground: forced auto-level */
        } else if (!(joy & 0x01)) {                     /* stick up */
            roll_rate = 0xD0;
            if (roll_pos_hi == 0xF4 && roll_pos_lo == 0x00) roll_rate = 0xFF;  /* at -limit */
        } else if (!(joy & 0x02)) {                     /* stick down */
            roll_rate = 0x30;
            if (roll_pos_hi == 0x0B && roll_pos_lo == 0xFF) roll_rate = 0x01;  /* at +limit */
        } else {
            roll_rate = 0x00;                           /* neutral */
        }
        roll_velocity = roll_rate;
    }

    /* Targeting blip position (skipped during level 0 / intro). */
    if (level_or_state != 0) { IN_CNT(g_inBlipCalls); compute_target_blip_position(); }
    IN_LAP(g_inHead);

    /* ---- Per-state dispatch; every branch falls through to the $3E shutdown check ---- */
    if (flight_mode_state == 0x02) {
        /* Crash/landing auto-attitude: force roll, set pitch trim = -(pitch_pos >> 6). */
        roll_velocity = 0x30;
        uint8_t v = (uint8_t)(ROF_PAIR16(pitch_pos_lo, pitch_pos_hi) >> 6);
        pitch_velocity = (uint8_t)(0u - v);
        dial_draw_index = 0xF0;
        draw_cockpit_dial_bar_core(0x00);
    } else if (mem[0x003D] != 0) {
        /* Landing sequence: enter the $3355 special state once (engine_state $066C latch). */
        if (mem[0x283C] == 0 && mem[0x066C] != 0x01) {
            mem[0x066C] = 0x01; mem[0x066D] = 0x01;
            cpu.Y = 0x01; ring_push_unmarked();
            cpu.Y = 0x02; ring_push_unmarked();
            special_state_color = 0x34;
        }
    } else if (flight_mode_state == 0) {
        step_object_along_axes();
    } else {
        /* Active flight: arm the HUD once (timer_676), then shadow the pitch angle. */
        if (timer_676 != 0x01) {
            mem[0x066C] = 0x00; mem[0x066D] = 0x00;
            cpu.Y = 0x01; ring_push_unmarked();
            cpu.Y = 0x02; ring_push_unmarked();
            cpu.A = 0x01; store_676_init();
        }
        mem[0x0023] = pitch_pos_lo;
        mem[0x0024] = pitch_pos_hi;
    }
    IN_LAP(g_inDisp);

    /* ---- Colour-clear shutdown: tear the special state back down and bail ---- */
    if (clear_colors_done_003E != 0) {
        if (mem[0x066C] != 0x00) {
            mem[0x066C] = 0x00; mem[0x066D] = 0x00;
            cpu.Y = 0x01; ring_push_unmarked();
            cpu.Y = 0x02; ring_push_unmarked();
            special_state_color = 0xB4;
        }
        return;
    }

    /* ---- Pitch auto-level: when no pitch input, bleed pitch_pos toward 0 by ~(pos*32>>8) ---- */
    if (pitch_velocity == 0) {
        IN_CNT(g_inAutoP);
        uint16_t pp  = ROF_PAIR16(pitch_pos_lo, pitch_pos_hi);
        uint8_t  sub = (uint8_t)((pp << 5) >> 8);
        if ((pp >> 11) & 1) {                           /* shift carry set */
            int diff = (int)pitch_pos_lo - sub;
            pitch_pos_lo = (uint8_t)diff;
            if (diff >= 0) pitch_pos_hi++;
        } else {
            int diff = (int)pitch_pos_lo - sub - 1;
            if (diff >= 0)                              pitch_pos_lo = (uint8_t)diff;
            else if ((uint8_t)(pitch_pos_hi - 1) & 0x80) { pitch_pos_hi = 0; pitch_pos_lo = 0; }
            else                                        { pitch_pos_hi--; pitch_pos_lo = (uint8_t)diff; }
        }
    }

    /* ---- Roll auto-level: same idea, ~(pos*4>>8), only when no roll input and not landing ---- */
    if (mem[0x003D] == 0 && roll_velocity == 0) {
        IN_CNT(g_inAutoR);
        uint16_t rp  = ROF_PAIR16(roll_pos_lo, roll_pos_hi);
        uint8_t  sub = (uint8_t)((rp << 2) >> 8);
        if ((rp >> 14) & 1) {
            int diff = (int)roll_pos_lo - sub;
            roll_pos_lo = (uint8_t)diff;
            if (diff >= 0) roll_pos_hi++;
        } else {
            int diff = (int)roll_pos_lo - sub - 1;
            if (diff >= 0)                             roll_pos_lo = (uint8_t)diff;
            else if ((uint8_t)(roll_pos_hi - 1) & 0x80) { roll_pos_hi = 0; roll_pos_lo = 0; }
            else                                       { roll_pos_hi--; roll_pos_lo = (uint8_t)diff; }
        }
    }

    IN_LAP(g_inLevel);

    /* ---- Throttle: add a RANDOM-modulated kick, scaled by the dial index ----
     * loops_y also picks the shift count for the throttle clamp below. */
    uint8_t loops_y;
    if (dial_draw_index == 0) {
        loops_y = 3;
    } else {
        IN_CNT(g_inThrKick);
        loops_y = 1;
        /* base = dial - 2*roll_pos_hi (two chained 6502 subtracts preserve the borrow) */
        int t = (int)dial_draw_index - roll_pos_hi;
        int borrow = (t < 0) ? 1 : 0;
        uint8_t base = (uint8_t)((t & 0xFF) - roll_pos_hi - borrow);
        uint16_t kick = (uint16_t)((((dial_draw_index >> 1) | 0x07) & bus_read(0xD20A))
                                   + base + (dial_draw_index & 1));
        if (kick > 0xFF) kick = 0xFF;
        uint16_t acc = (uint16_t)throttle_accum_lo + (uint8_t)kick;
        throttle_accum_lo = (uint8_t)acc;
        if (acc > 0xFF) throttle_accum_hi++;
    }

    /* ---- Throttle clamp (near-ground only): cap the throttle to (hi+2)<<loops_y ---- */
    if (mem[0x005D] != 0) {
        IN_CNT(g_inThrClamp);
        uint16_t hi2 = (uint16_t)throttle_accum_hi + 2;
        uint32_t v = ((uint32_t)(uint8_t)(hi2 >> 8) << 16)
                   | ((uint32_t)(uint8_t)hi2 << 8) | throttle_accum_lo;
        for (uint8_t i = 0; i < loops_y; i++) v <<= 1;
        int carry = (v >> 24) & 1;
        int lo = (int)throttle_accum_lo - (uint8_t)(v >> 8) - (carry ? 0 : 1);
        throttle_accum_lo = (uint8_t)lo;
        int hi = (int)throttle_accum_hi - (uint8_t)(v >> 16) - (lo < 0 ? 1 : 0);
        throttle_accum_hi = (uint8_t)hi;
        if (hi < 0) {                                   /* over the cap -> zero + rearm HUD */
            throttle_accum_lo = 0; throttle_accum_hi = 0;
            if (timer_676 != 0) { cpu.A = 0; store_676_init(); }
        }
    }

    IN_LAP(g_inThr);

    /* ---- Integrate + clamp pitch angle ($25/$26) to [-5 .. +4] = [$FB .. $04] ---- */
    {
        int8_t   dv  = (int8_t)pitch_velocity;
        uint16_t sum = (uint16_t)pitch_pos_lo + (uint8_t)pitch_velocity;
        pitch_pos_lo = (uint8_t)sum;
        if (dv < 0) { if (!(sum >> 8)) pitch_pos_hi--; }
        else        { if ( (sum >> 8)) pitch_pos_hi++; }
        if (!(pitch_pos_hi & 0x80)) {
            if (pitch_pos_hi >= 0x05) { pitch_pos_lo = 0xFF; pitch_pos_hi = 0x04; }
        } else {
            if (pitch_pos_hi <  0xFB) { pitch_pos_lo = 0x00; pitch_pos_hi = 0xFB; }
        }
    }

    /* ---- Integrate + clamp roll/heading angle ($28/$29) to [$F4 .. $0B] ---- */
    {
        int8_t   dv  = (int8_t)roll_velocity;
        uint16_t sum = (uint16_t)roll_pos_lo + (uint8_t)roll_velocity;
        roll_pos_lo = (uint8_t)sum;
        if (dv < 0) { if (!(sum >> 8)) roll_pos_hi--; }
        else        { if ( (sum >> 8)) roll_pos_hi++; }
        if (!(roll_pos_hi & 0x80)) {
            if (roll_pos_hi >= 0x0C) { roll_pos_lo = 0xFF; roll_pos_hi = 0x0B; }
        } else {
            if (roll_pos_hi <  0xF4) { roll_pos_lo = 0x00; roll_pos_hi = 0xF4; }
        }
    }

    /* ---- Roll-derived magnitudes: |roll<<3| -> $28D6, and (throttle_hi * that)<<3 -> fwd step ---- */
    {
        uint16_t rp = ROF_PAIR16(roll_pos_lo, roll_pos_hi);
        uint8_t  hi = (uint8_t)((rp << 3) >> 8);
        mem[0x0020] = hi;                               /* sign source for the multiply below */
        mem[0x28D6] = ((rp >> 13) & 1) ? (uint8_t)(0u - hi) : hi;
    }
    {
        mul_multiplicand = throttle_accum_hi;
        cpu.A = mul_u8_lookup(mul_multiplicand, mem[0x28D6]);  /* byte-exact table (was mul_u8) */
        int16_t signed_step = (mem[0x0020] & 0x80) ? -(int16_t)cpu.A : (int16_t)cpu.A;
        uint16_t shifted = (uint16_t)signed_step << 3;
        mem[0x2883] = (uint8_t)shifted;                 /* forward/depth step lo */
        mem[0x2884] = (uint8_t)(shifted >> 8);          /* forward/depth step hi */
    }
    IN_LAP(g_inAttc);

    /* ---- Heading: heading += (signed pitch_pos >> 4); carry feeds the angle-scale call ---- */
    {
        int16_t step    = (int16_t)((int16_t)ROF_PAIR16(pitch_pos_lo, pitch_pos_hi) >> 4);
        uint8_t step_lo = (uint8_t)step, step_hi = (uint8_t)(step >> 8);
        dl_y1 = step_lo; dl_y2 = step_hi;               /* scratch the 6502 left behind */
        uint16_t hlo = (uint16_t)heading_lo + step_lo;
        heading_lo = (uint8_t)hlo;
        uint16_t hhi = (uint16_t)heading_hi + step_hi + (hlo >> 8);
        heading_hi = (uint8_t)(hhi & 0x3F);
        cpu.C = (uint8_t)(hhi >> 8);                    /* entry carry for the call */
        compute_obj_rel_angle_scale();                  /* -> world velocity $2B/$2C, $2881/$2882 */
    }
    IN_LAP(g_inAng);

    /* ---- Integrate world position + depth accumulator ---- */
    {
        uint16_t x = (uint16_t)(ROF_PAIR16(world_x_lo, world_x_hi)
                              + ROF_PAIR16(mem[0x002B], mem[0x002C]));
        world_x_lo = (uint8_t)x; world_x_hi = (uint8_t)(x >> 8);

        uint16_t z = (uint16_t)(ROF_PAIR16(world_z_lo, world_z_hi)
                              + ROF_PAIR16(mem[0x2881], mem[0x2882]));
        world_z_lo = (uint8_t)z; world_z_hi = (uint8_t)(z >> 8);

        uint16_t d = (uint16_t)(ROF_PAIR16(terrain_depth_frac, terrain_depth_step)
                              + ROF_PAIR16(mem[0x2883], mem[0x2884]));
        terrain_depth_frac = (uint8_t)d;
        uint8_t step = (uint8_t)(d >> 8);
        if (step == 0xFF) step = 0x00;
        if (step >= 0x50) {                             /* depth past the far cap */
            if (flight_mode_state == 0x02) {
                if (step >= 0x60) level_ready_flag = 0xFF;  /* landing: signal level ready */
            } else {
                terrain_depth_frac = 0xFF; step = 0x4F;     /* clamp at the cap */
            }
        }
        terrain_depth_step = step;
    }

    /* ---- Canopy-pillar Y pair (each = angle_hi + offset + sign-of-lo) ---- */
    mem[0x2873] = (uint8_t)(roll_pos_hi  + 0x0C + (roll_pos_lo  >> 7));
    mem[0x2871] = (uint8_t)(mem[0x0024]  + 0x05 + (mem[0x0023]  >> 7));

    /* ---- Terrain table index from heading (or RTCLOK when heading is frozen) ---- */
    {
        uint8_t a = (mem[0x283D] != 0) ? (uint8_t)(0x3A - RTCLOK_LOW)
                                       : (uint8_t)(0x3A - heading_hi);
        a &= 0x3F;
        terrain_sub_index = a & 0x03;
        terrain_index     = a >> 2;
    }

    /* ---- Engine-sound pitch fields = ~(throttle << 1, high byte) ---- */
    {
        uint8_t v = (uint8_t)(~((throttle_accum_hi << 1) | (throttle_accum_lo >> 7)));
        mem[0x0686] = v;
        mem[0x0687] = (uint8_t)(v - 0x04);
    }
    IN_LAP(g_inPos);

    refresh_hud_field_0d_entry();
    IN_LAP(g_inHud);

    /* ---- Near-ground flag $5D + lock-on RANDOM countdown $2917 ---- */
    if (dial_draw_index == 0xF0) {
        mem[0x005D] = 0xF0;
    } else if (life_counter != 0) {
        mem[0x2917] = 0xFF;
        mem[0x005D] = 0xFF;
    } else {
        mem[0x005D] = 0x00;
        uint8_t cnt = mem[0x2917];
        if (cnt != 0) {
            mem[0x2917] = cnt - 1;
            if (cnt >= bus_read(0xD20A)) mem[0x005D] = 0xFF;
        }
    }

    /* ---- Engine-sound voice slot ($066B[$0C]) value from dial + depth ---- */
    {
        uint8_t voice;
        if (dial_draw_index == 0)             voice = 0x00;
        else if (dial_draw_index < 0xF0)      voice = 0x04;
        else {
            voice = (uint8_t)((terrain_depth_step >> 3) ^ 0x0F);
            if (voice < 0x04) voice = 0x04;
        }
        mem[MEM_sfx_env_prio_val + 0x0C] = voice;   /* $0677 */
    }
    /* ---- HUD field ($0679[$0C]) = same ~(throttle<<1) value, floored at $0C ---- */
    {
        uint8_t fld = (uint8_t)(~((throttle_accum_hi << 1) | (throttle_accum_lo >> 7)));
        if (fld < 0x0C) fld = 0x0C;
        mem[MEM_sfx_env_freq_val + 0x0C] = fld;            /* $0685 */
        cpu.Y = 0x0C; ring_push_unmarked();
    }
    IN_LAP(g_inLock);

    /* ---- Object velocity from the delayed history ring ($2919/$291A/$291B) ---- */
    {
        int16_t vx = (int16_t)ROF_PAIR16(mem[0x2919], mem[0x291A]);
        int16_t hx = (int16_t)(vx >> 1);
        mem[0x2850] = (uint8_t)hx; mem[0x2851] = (uint8_t)(hx >> 8);

        uint16_t vy = (uint16_t)((int16_t)(int8_t)mem[0x291B] << 3);
        mem[0x2852] = (uint8_t)vy; mem[0x2853] = (uint8_t)(vy >> 8);
    }

    /* ---- Two 16-bit object-position accumulators (sign-extended ring deltas) ---- */
    {
        int8_t  a   = (int8_t)((int8_t)mem[0x291A] >> 1);
        uint16_t s  = (uint16_t)mem[0x2829] + (uint8_t)a;
        mem[0x2829] = (uint8_t)s;
        mem[0x0068] = (uint8_t)(mem[0x0068] + (a < 0 ? 0xFF : 0x00) + (uint8_t)(s >> 8));
    }
    {
        int8_t  a   = (int8_t)((int8_t)mem[0x291B] >> 3);
        uint16_t s  = (uint16_t)mem[0x282C] + (uint8_t)a;
        mem[0x282C] = (uint8_t)s;
        mem[0x0069] = (uint8_t)(mem[0x0069] + (a < 0 ? 0xFF : 0x00) + (uint8_t)(s >> 8));
    }
    IN_LAP(g_inObjv);

    /* ---- Step the active world object, then integrate its position by the ring velocity ---- */
    {
        uint8_t af = object_anim_frame;
        if (af != 0) {
            if (!(af & 0x80)) {
                if (af == 0x01) { IN_CNT(g_inObjLoad); load_velocity_from_param_block(); }
                else            { IN_CNT(g_inObjStep); object_step_and_collide(); }
            }
            int lo = (int)mem[0x284E] - mem[0x2850];
            mem[0x284E]    = (uint8_t)lo;
            vobj_row_count = (uint8_t)(vobj_row_count - mem[0x2851] - (lo < 0 ? 1 : 0));
            uint16_t s = (uint16_t)mem[0x284F] + mem[0x2852];
            mem[0x284F] = (uint8_t)s;
            mem[0x0039] = (uint8_t)(mem[0x0039] + mem[0x2853] + (uint8_t)(s >> 8));
        }
    }
    IN_LAP(g_inObj);

    /* ---- Decrement the object slot index; dispatch its per-frame work ---- */
    {
        uint8_t y = object_index_signed;
        if (!(y & 0x80)) {
            y = (uint8_t)(y - 1);
            object_index_signed = y;
            if (y & 0x80) {                             /* slot ran out */
                reset_flags_ff();
                if (player3_dither_flag != 0) { IN_CNT(g_inObjBox); check_object_in_target_box(); }
            } else {
                mem[0x006A] = y >> 2;
                int jitter;
                if      (y == 0x5A)            jitter = 1;
                else if (level_stage < 0x1F)  jitter = 0;
                else if (y == 0x3C)           jitter = 1;
                else if (level_stage < 0x3D)  jitter = 0;
                else                          jitter = (y == 0x28);
                if (jitter) { IN_CNT(g_inJitter); terrain_jitter_column(); }
                IN_CNT(g_inObjPos);
                object_integrate_position();
            }
        } else {
            IN_CNT(g_inSlotIdle);
        }
    }
    IN_LAP(g_inSlot);

    /* ---- Roll the 7-frame attitude history ring (delays the canopy/horizon geometry) ---- */
    {
        uint8_t y = mem[0x291E] + 1;
        if (y >= 0x07) y = 0x00;
        mem[0x291E] = y;
        mem[0x2919] = mem[0x2893 + y]; mem[0x2893 + y] = pitch_pos_lo;
        mem[0x291A] = mem[0x289A + y]; mem[0x289A + y] = pitch_pos_hi;
        mem[0x291B] = mem[0x28A1 + y]; mem[0x28A1 + y] = roll_velocity;
        mem[0x291C] = mem[0x28A8 + y]; mem[0x28A8 + y] = mem[0x2871];
        mem[0x291D] = mem[0x28AF + y]; mem[0x28AF + y] = mem[0x2873];
    }
    IN_LAP(g_inTail);
    IN_LAP(g_inNop);          /* empty lap = the per-bucket floor; subtract it from each bucket */
#ifdef ROF_FCI_BASE
#undef mem
#endif
}
#ifdef ROF_FLIGHT_PROBE
void flight_control_integrate(void) { unsigned long _p = rof_subclock(); flight_control_integrate_impl(); g_pInteg += rof_subclock() - _p; }
#else
void flight_control_integrate(void) { flight_control_integrate_impl(); }
#endif

/* ===========================================================================
 * Flight main-loop de-transpile (2026-06-12): game_state_update + enemy_check
 * and their callees — the last transpiled code on the flight per-frame path
 * (flight_frame_native).  Leaves first.
 * ------------------------------------------------------------------------- */

#ifdef ROF_PLATFORM_AMIGA
/* Enemy-fire wedge dot span (Amiga).  The enemy laser bolt (plot_scanline_down, plot_pixel_mask
 * = $FF = value-3) plots a SOLID horizontal run of columns per scanline into BOTH the plane2 dot
 * buffer (g_flightDotPlane) and the plane1 object overlay (g_flightObjP1) — every column, since
 * $FF & any col mask is set.  The per-pixel path routes each column through terrain_plot_pixel_core
 * -> ROF_PLOT_DOT/ROF_PLOT_DOT_P1 (a scattered single-bit OR + call overhead each).  Batch it: the
 * scanline is fixed (kRow120[sc] base constant), so OR whole bytes ($FF for full 4-col groups,
 * partial masks at the ends) — the same dots, ~4x fewer stores + no per-pixel call.  kColMask4
 * packs 4 cols/byte (2 bits each).  Column window = the ROF_PLOT_DOT gate [48,208) & sc in
 * [0,47)\{43}; uint8 X-wrap reproduced as a second run. */
static int laser_dot_run(uint8_t* p2, uint8_t* p1, unsigned c0, unsigned c1) {
    if (c0 < 48u) c0 = 48u;
    if (c1 > 208u) c1 = 208u;
    if (c0 >= c1) return 0;
    unsigned a = c0 - 48u, a1 = c1 - 48u;              /* _ac = col - 48 */
    if (p1) {                                          /* dirty byte-column range (see g_objColLo) */
        const int bl = (int)(a >> 2), bh = (int)((a1 - 1u) >> 2);
        if (bl < g_objColLo) g_objColLo = bl;
        if (bh > g_objColHi) g_objColHi = bh;
    }
    while (a < a1) {
        unsigned byte = a >> 2; uint8_t m = 0;
        do { m |= kColMask4[a & 3u]; a++; } while ((a & 3u) && a < a1);   /* accumulate this byte's cols */
        p2[byte] |= m;
        if (p1) {
            if (!p1[byte]) ROF_OBJ_TOUCH(p1 + byte - g_flightObjP1);
            p1[byte] |= m;
        }
    }
    return 1;
}
/* The OTHER half of the bolt (plot_scanline_up): a vertical run of `count` pixels in ONE column,
 * walking UP the screen (row, row-1, ...) = DOWN the dot buffer (scanline 150-row upwards).  The
 * column is fixed, so the byte offset and the 2-bit column mask are loop invariants and the run is
 * a strided pointer walk (+120/scanline) — one OR per pixel with no per-pixel call, no table
 * lookups and no re-derived geometry.  Same ROF_PLOT_DOT/_P1 gate as laser_dot_span (column window
 * [48,208), scanline [0,47) minus the $6b floor at 43). */
static void laser_dot_column(int rowStart, unsigned col, unsigned count) {
    if (!g_flightDotPlane || count == 0u) return;
    if (col < 48u || col >= 208u) return;
    unsigned ac = col - 48u;
    uint8_t m = kColMask4[ac & 3u];
    int lo = 150 - rowStart;                            /* first scanline (rows walk upward) */
    int hi = lo + (int)count - 1;                       /* last scanline */
    if (lo < 0)  lo = 0;
    if (hi > 46) hi = 46;
    if (lo == 43) lo++;                                 /* the $6b floor scanline is never plotted */
    if (hi == 43) hi--;
    if (lo > hi) return;
    unsigned b = (ac >> 2) + kRow120[lo];
    uint8_t* p2 = g_flightDotPlane + b;
    uint8_t* p1 = g_flightObjP1 ? g_flightObjP1 + b : (uint8_t*)0;
    int n = hi - lo + 1;
    if (p1) {
        for (int sc = lo; n--; sc++) {
            if (sc != 43) {
                *p2 |= m;
                if (!*p1) ROF_OBJ_TOUCH(p1 - g_flightObjP1);
                *p1 |= m;
            }
            p2 += 120; p1 += 120;
        }
        if (lo < g_objRowLo) g_objRowLo = lo;           /* ROF_PLOT_DOT_P1 dirty scanline range */
        if (hi > g_objRowHi) g_objRowHi = hi;
        {   const int bc = (int)(ac >> 2);              /* one fixed column for the whole run */
            if (bc < g_objColLo) g_objColLo = bc;
            if (bc > g_objColHi) g_objColHi = bc; }
    } else {
        for (int sc = lo; n--; sc++) {
            if (sc != 43) *p2 |= m;
            p2 += 120;
        }
    }
}
static void laser_dot_span(uint8_t row, unsigned xstart, unsigned width) {
    if (!g_flightDotPlane || width == 0u) return;
    int sc = 150 - (int)row;                            /* height/row -> dot scanline (as ROF_PLOT_DOT) */
    if ((unsigned)sc >= 47u || sc == 43) return;        /* row outside the dot band / the $6b floor */
    int rb = kRow120[sc];
    uint8_t* p2 = g_flightDotPlane + rb;
    uint8_t* p1 = g_flightObjP1 ? g_flightObjP1 + rb : (uint8_t*)0;
    unsigned end = xstart + width;                      /* columns visited: X, X+1, ... (uint8 wrap) */
    int plotted = laser_dot_run(p2, p1, xstart, end < 256u ? end : 256u);
    if (end > 256u) plotted |= laser_dot_run(p2, p1, 0u, end - 256u);
    if (p1 && plotted) {                                /* ROF_PLOT_DOT_P1 dirty scanline range */
        if (sc < g_objRowLo) g_objRowLo = sc;
        if (sc > g_objRowHi) g_objRowHi = sc;
    }
}
#endif

/* plot_scanline_down @ $AAD4 — line-plot loop walking DOWN the screen.
 * Walks the {$28EF:$28F0} fixed-point X position by the step {$5B:$5C} (which is
 * first decremented by $40) and the sub-step {$28F3:$28F4}; for each scanline Y
 * (from $28F2 down to $6C) it draws $28FA pixels across, advancing X by 1 and
 * plotting via terrain_plot_pixel (native) when the column X is in [$2C,$D4).
 * Inputs: $28EF-$28F4 position/step, $5B/$5C step, $58 plot mask (all set by the
 * caller).  Contract: memory (the bitmap + the accumulators); exit cpu dead.
 * plot_line_done $AB26 is a bare RTS -> absorbed as `return`. */
void plot_scanline_down(void) {
    /* aad4 SEC; LDA $5B; SBC #$40; STA $5B; BCC -> DEC $5C */
    uint8_t step = plot_x_step_lo;
    int borrow = (step < 0x40);                  /* SBC #$40 with C=1 */
    plot_x_step_lo = (uint8_t)(step - 0x40);
    if (borrow) plot_x_step_hi--;                    /* BCC L_aadf else DEC $5C */
    cpu.X = mem[0x28F0];                          /* LDX $28F0 */
    cpu.Y = mem[0x28F2];                          /* LDY $28F2 */
    if (cpu.Y < 0x6C) return;                     /* CPY #$6C; BCC plot_line_done */
    for (;;) {                                    /* L_aae9 (per scanline) */
        mem[0x28FA] = mem[0x28F4];                /* LDA $28F4; STA $28FA */
#ifdef ROF_PLATFORM_AMIGA
        if (plot_pixel_mask == 0xFF) {
            /* value-3 solid wedge (the enemy bolt): batch the scanline's run into byte ORs instead
               of a per-pixel scatter (see laser_dot_span).  X advances by the full width and $28FA
               reaches 0 — the per-pixel loop's exit state — so the accumulator arithmetic below and
               the faithful mem[]/cpu contract are unchanged.  (The Amiga sheds the mode-D field, so
               the per-pixel path's $28E2/$B5/field writes are already dead here.) */
            unsigned w = mem[0x28FA];
            laser_dot_span(cpu.Y, cpu.X, w);
            cpu.X = (uint8_t)(cpu.X + w);
            mem[0x28FA] = 0;
        } else
#endif
        do {                                      /* L_aaef (across the row) */
            if (cpu.X >= 0x2C && cpu.X < 0xD4)    /* CPX #$2C BCC; CPX #$D4 BCS */
                terrain_plot_pixel_core(cpu.Y, cpu.X, plot_pixel_mask);             /* aaf7 (preserves X/Y) */
            cpu.X++;                              /* INX */
            mem[0x28FA]--;                        /* DEC $28FA */
        } while (mem[0x28FA] != 0);               /* BNE L_aaef */
        /* ab00 CLC; LDA $28F3; ADC $28F9; STA $28F3; BCC; INC $28F4; CLC */
        { uint16_t t = (uint16_t)mem[0x28F3] + mem[0x28F9];
          mem[0x28F3] = (uint8_t)t; if (t & 0x100) mem[0x28F4]++; }
        /* ab10 LDA $28EF; ADC $5B (C=0); STA $28EF; LDA $28F0; ADC $5C; STA $28F0; TAX */
        { uint16_t t = (uint16_t)mem[0x28EF] + plot_x_step_lo;
          mem[0x28EF] = (uint8_t)t; int c = (t >> 8) & 1;
          uint16_t u = (uint16_t)mem[0x28F0] + plot_x_step_hi + c;
          mem[0x28F0] = (uint8_t)u; cpu.X = (uint8_t)u; }   /* TAX */
        cpu.Y--;                                  /* DEY */
        if (cpu.Y < 0x6C) return;                 /* CPY #$6C; BCS L_aae9 else done */
    }
}

/* plot_scanline_up @ $AB27 — Bresenham-style point plotter walking UP the screen.
 * From {$28F1:$28F2} (Y) and X=$28F0, walks via the {$28F1:$28F3}/{$28F7:$28F9}
 * step accumulators, drifting X by ±1 per scanline by the sign of $5C; plots
 * $28FA pixels up each column via terrain_plot_pixel (native).  Bounds: X in
 * [$2C,$D4), Y >= $6C.  Contract: memory; exit cpu dead.  plot_line_done absorbed. */
void plot_scanline_up(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    cpu.X = mem[0x28F0];                          /* LDX $28F0 */
    cpu.Y = mem[0x28F2];                          /* LDY $28F2 */
    for (;;) {                                    /* L_ab2d */
        if (cpu.X < 0x2C) return;                 /* CPX #$2C; BCC done */
        if (cpu.X >= 0xD4) return;                /* CPX #$D4; BCS done */
        if (cpu.Y < 0x6C) return;                 /* CPY #$6C; BCC done */
        mem[0x28FA] = mem[0x28F4];                /* LDA $28F4; STA $28FA */
#ifdef ROF_PLATFORM_AMIGA
        if (plot_pixel_mask == 0xFF) {
            /* value-3 solid bolt: batch the column's vertical run (see laser_dot_column).  The
               loop below plots at Y, Y-1, ... and stops either when Y drops under $6C (the DEC
               $28FA is skipped on that exit) or when the counter reaches 0 — reproduce both exits
               exactly so the mem[]/cpu contract is unchanged.  ($28E2/$80/$81/$B5, the per-pixel
               path's other residue, are dead here as in plot_scanline_down.) */
            unsigned n     = mem[0x28FA];
            unsigned neff  = n ? n : 256u;        /* $28FA==0 wraps: 256 pixels */
            unsigned avail = (unsigned)cpu.Y - 0x6Cu + 1u;   /* rows down to the $6C floor */
            if (avail <= neff) {                  /* Y-exhaustion wins the tie (tested first) */
                laser_dot_column(cpu.Y, cpu.X, avail);
                cpu.Y = 0x6B;
                mem[0x28FA] = (uint8_t)(n - (avail - 1u));
            } else {
                laser_dot_column(cpu.Y, cpu.X, neff);
                cpu.Y = (uint8_t)(cpu.Y - neff);
                mem[0x28FA] = 0;
            }
        } else
#endif
        do {                                      /* L_ab3f (up the column) */
            terrain_plot_pixel_core(cpu.Y, cpu.X, plot_pixel_mask);                 /* ab3f (preserves X/Y) */
            cpu.Y--;                              /* DEY */
            if (cpu.Y < 0x6C) break;              /* CPY #$6C; BCC L_ab4c */
            mem[0x28FA]--;                        /* DEC $28FA */
        } while (mem[0x28FA] != 0);               /* BNE L_ab3f */
        /* L_ab4c CLC; LDA $28F3; ADC $28F9; STA $28F3; BCC; INC $28F4; CLC */
        { uint16_t t = (uint16_t)mem[0x28F3] + mem[0x28F9];
          mem[0x28F3] = (uint8_t)t; if (t & 0x100) mem[0x28F4]++; }
        /* L_ab5c LDA $28F1; ADC $28F7 (C=0); STA $28F1; LDA $28F2; ADC $28F8; STA $28F2; TAY */
        { uint16_t t = (uint16_t)mem[0x28F1] + mem[0x28F7];
          mem[0x28F1] = (uint8_t)t; int c = (t >> 8) & 1;
          uint16_t u = (uint16_t)mem[0x28F2] + mem[0x28F8] + c;
          mem[0x28F2] = (uint8_t)u; cpu.Y = (uint8_t)u; }   /* TAY */
        /* ab6f LDA $5C; BPL -> INX else DEX */
        if (plot_x_step_hi & 0x80) cpu.X--;          /* DEX (X drift) */
        else                    cpu.X++;          /* INX */
    }
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* plot_scanline_rand_dir @ $AACF — RANDOM picks the walk direction. */
void plot_scanline_rand_dir(void) {
    if (bus_read(0xD20A) & 0x80) { plot_scanline_up(); return; }  /* BMI */
    plot_scanline_down();
}

/* game_state_update @ $A99C — the enemy gun-emplacement fire / target-blip state machine, run
 * once per flight main-loop pass.  A countdown ($28EE) paces fire events; when it elapses with a
 * shot queued ($28ED != 0) it seeds a growing bolt and draws it with the scanline plotters, bumps
 * the explosion-frame counter ($0041 = game_state), and pushes the matching SFX ring events.
 * $007E == 7 selects the impact variant (its own wedge geometry + colour flash + roll/pitch jitter).
 * Contract: memory only (exit cpu dead).  All callees are native.
 * (Several cells here are unnamed — see docs/rename.md: $28EB/$28EC target cell, $28EF-$28F9
 * bolt line-plot state, $0624 fire-delay mask, $28ED shot-queued flag.) */
void game_state_update(void) {
    ROF_MEMBASE_DECL(mb);                     /* 89 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* Pace the next fire.  When the countdown underflows, pick a fresh random delay
       ($0624 & RANDOM), clear the fire/queue state, and (unless crashed) reset the blip colour. */
    uint8_t timer = (uint8_t)(lock_on_indicator_complete - 1);
    lock_on_indicator_complete = timer;
    if (timer & 0x80) {                                   /* underflowed */
        lock_on_indicator_complete = mem[0x0624] & bus_read(0xD20A);
        if (player3_dither_flag == 0) reset_flags_ff();
        if (landing_seq_flag == 0)    special_state_color = 0xB4;
        game_state = 0x00;
        mem[0x28ED] = 0x00;
        return;
    }
    if (timer != 0)       return;                         /* not time to fire yet */
    if (mem[0x28ED] == 0) return;                         /* no shot queued */

    /* Seed the bolt line-plot from the target cell ($28EB/$28EC): start point ($28F0/$28F2),
       the $80-midpoint sub-pixel accumulators ($28EF/$28F1/$28F3), a width seed ($28F4=1), a
       solid plot mask, and a random signed horizontal step {plot_x_step_hi:plot_x_step_lo}. */
    mem[0x28F0] = mem[0x28EB];
    mem[0x28F2] = mem[0x28EC];
    mem[0x28F4] = 0x01;
    mem[0x28EF] = 0x80; mem[0x28F1] = 0x80; mem[0x28F3] = 0x80;
    plot_pixel_mask = 0xFF;
    mem[0x28F8] = 0x00;
    {
        uint8_t r = bus_read(0xD20A);                     /* RANDOM */
        plot_x_step_lo = (uint8_t)(r << 1);
        plot_x_step_hi = (r & 0x80) ? 0xFF : 0x00;        /* sign of the doubled random */
    }
    /* Override the drift direction from the delayed pitch-history field (ring_cur_1 $291A):
       steeply negative -> leftward (hi=0); positive and >= 2 -> rightward (hi=$FF). */
    {
        uint8_t p = ring_cur_1;
        if (p & 0x80) { if (p < 0xFF) plot_x_step_hi = 0x00; }
        else          { if (p >= 0x02) plot_x_step_hi = 0xFF; }
    }
    mem[0x28F7] = (uint8_t)(bus_read(0xD20A) << 1);        /* random vertical step */
    mem[0x28F8] = 0xFF;                                    /* row counter = 0 - 1 */

    if (lock_on_indicator_state == 0x07) {
        /* Impact variant: aim the wedge at the fixed impact point (col $67, row $6B) offset by the
           target cell, draw the horizontal wedge, flash the explosion colours, push impact SFX. */
        uint8_t dx = (uint8_t)(0x67 - mem[0x28EB]);
        plot_x_step_lo = (uint8_t)(dx << 1);
        plot_x_step_hi = (dx & 0x80) ? 0xFF : 0x00;
        mem[0x28F9] = (uint8_t)(0x6B - mem[0x28EC]);      /* end row */
        CL_CNT(g_clImpact);      /* combat-load: a bolt HIT us — the near-camera wedge */
        plot_scanline_down();
        vobj_path_flag = 0x10;
        game_state++;                                     /* explosion-frame counter */
        terrain_pen1_fade = 0xBE; anim_counter_2 = 0xBC;
        terrain_pen0_fade = 0xB6; audc_shadow_0 = 0xB8;
        special_state_color = 0x34;
        ring_push_marked_core(0x03); ring_push_marked_core(0x04);   /* impact SFX events */
        ring_push_marked_core(0x0A); ring_push_marked_core(0x0B);
        jitter_roll_pitch();
        return;
    }

    /* Normal shot: end row from the downed-object distance ($004D), draw the drifting bolt,
       flash terrain pen 1, seed the explosion-frame counter on the first frame, push fire SFX. */
    mem[0x28F9] = (uint8_t)(((uint8_t)(mem[0x004D] ^ 0xFF) >> 3) + 0x0C);
    plot_scanline_rand_dir();
    terrain_pen1_fade = 0xBE;
    if (game_state == 0) anim_counter_2 = 0x28;
    game_state++;
    ring_push_marked_core(0x07); ring_push_marked_core(0x02);
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* alien_attack_tick @ $7AB8 — per-frame enemy PMG update.  When RANDOM is negative,
 * occasionally repositions the enemy ($0044/$0047 to $6D or $70, $283D phase) and
 * jitters roll/pitch; then advances $003B unless level-clear ($0072==2); finally
 * pushes ring events $1A/$1B.  Reads RANDOM once.  Contract: memory; exit cpu dead. */
void alien_attack_tick(void) {
    uint8_t a = bus_read(0xD20A);                 /* 7ab8 LDA $D20A */
    if (a & 0x80) {                               /* 7abb BPL L_7af3 (return) */
        if ((a & 0x0F) != 0) {                    /* 7abd AND #$0F; BEQ L_7add */
            a &= 0x0F;
            if (!(mem[0x003A] & 0x80)) {          /* 7ac1 LDY $003A; BMI L_7acb */
                timer_or_counter = 0x6D;               /* 7ac5 LDY #$6D; STY $44 */
                colpf0_value = 0x6D;               /* STY $47 */
            }
            if ((a & 0x03) != 0) {                /* 7acb AND #$03; BEQ L_7add */
                mem[0x283D] = (uint8_t)(a & 0x01);/* 7acf AND #$01; STA $283D */
                timer_or_counter = 0x70;               /* 7ad4 LDY #$70; STY $44 */
                colpf0_value = 0x70;               /* STY $47 */
                jitter_roll_pitch();              /* 7ada */
            }
        }
        /* L_7add LDA $0072; CMP #$02; BEQ L_7aea */
        if (flight_mode_state != 0x02) {
            vobj_path_flag = (uint8_t)(vobj_path_flag + 0x04);   /* 7ae3 CLC; LDA $3B; ADC #$04; STA $3B */
        }
        /* L_7aea LDX #$1A; ring; INX; ring */
        cpu.X = 0x1A; ring_push_marked();         /* 7aea */
        cpu.X++;      ring_push_marked();         /* 7aef INX (X=$1B) */
    }
    /* L_7af3: RANDOM was positive -> nothing */
}

/* enemy_check @ $3FCD — event dispatch.  $063D (event trigger) -> intro_cinematic_loop
 * (LEFT TRANSPILED: its closure reaches the whole-program init/teardown; it has
 * 0 callers in steady flight); else $0633 (alien trigger) -> alien_attack_tick.
 * Contract: memory; exit cpu dead. */
void enemy_check(void) {
    if (event_trigger != 0) { intro_cinematic_loop(); return; }   /* 3fcd LDA $063D; BNE */
    if (alien_trigger != 0) alien_attack_tick();            /* 3fd5 LDA $0633; BEQ skip */
}

/* ===========================================================================
 * IN-GAME SFX ENGINE — the $548D voice/gauge engine and its subtree.
 *
 * Run once per flight VBI (the Atari VBI tail $534D -> $548D).  These drain the
 * $0719 event ring that the native flight code (enemy_check / alien_attack_tick /
 * ring_push_marked / the apex below) fills, advance per-voice envelopes, and
 * write POKEY AUDF/AUDC — which on the Amiga route through bus_write ->
 * platform_hw_write -> Paula, so the effects are audible automatically.
 *
 * They use the shared `cpu` register file for entry/exit registers exactly as
 * the 6502 ABI / their __t6502 twins do, so $548D threads X/Y/A into its callees
 * faithfully.  This is RACE-SAFE in flight: game_vbi_isr() save/restores `cpu`
 * around the whole VBI body (mirroring the OS VBLANK), and the only other `cpu`
 * users (the main-loop natives) are bracketed by that save/restore.  POKEY
 * register writes ($D1FE+X / $D1FF+X) are masked in validation (hardware side
 * effect, not mem[] state); the static AUDF/AUDC/AUDCTL writes already go through
 * bus_write in the oracle too, so those need no mask.
 * ========================================================================= */

/* sfx_voice_write_freq @ $5667 — write AUDF for voice (cpu.Y) to POKEY $D1FE+X,
 * where X = the POKEY register index mem[$0705+Y]; skip if 0 (inactive slot). */
void sfx_voice_write_freq(void) {
    SX_CNT(g_sxWrFreq);
    uint8_t y = cpu.Y;
    uint8_t x = mem[MEM_sfx_voice_reg_idx + y];
    cpu.X = x;
    if (x == 0) return;
    cpu.A = mem[MEM_sfx_env_freq_val + y];
    bus_write((uint16_t)(0xD1FE + x), cpu.A);            /* AUDFn */
}

/* sfx_voice_write_freq_ctrl @ $5673 — write AUDF ($D1FE+X) freq AND AUDC ($D1FF+X)
 * = (prio/vol nibble $066B+Y & $0F) | (distortion $065D+Y) for voice cpu.Y. */
void sfx_voice_write_freq_ctrl(void) {
    SX_CNT(g_sxWrCtrl);
    uint8_t y = cpu.Y;
    uint8_t x = mem[MEM_sfx_voice_reg_idx + y];
    cpu.X = x;
    if (x == 0) return;
    /* Shape probe: the two POKEY writes are the bulk of this leaf (each is a rof_pokey_write
     * call + a change-detect + possibly a whole update_paula_channel recompute), so they are
     * bracketed separately from the mem[] loads — an asm twin of the MIXER cannot touch them. */
    SX_LEAF(bus_write((uint16_t)(0xD1FE + x), mem[MEM_sfx_env_freq_val + y]), g_sxPokeyT); /* AUDFn */
    cpu.A = (uint8_t)((mem[MEM_sfx_env_prio_val + y] & 0x0F) | mem[MEM_sfx_voice_distortion + y]);
    SX_LEAF(bus_write((uint16_t)(0xD1FF + x), cpu.A), g_sxPokeyT);   /* AUDCn ctrl */
}

/* sfx_pick_top_voice @ $568A — scan slots X=1..12; latch the active slot
 * ($0705+X != 0) with the smallest priority nibble below $10 into
 * $0716 (running min) / $0714 (value) / $0715 (index). */
void sfx_pick_top_voice(void) {
    SX_CNT(g_sxTopScan);
    /* Hot: ~2.4 calls per 50 Hz VBI firing, inside sfx_reorder_voice_slot (measured 24% of the
       whole flight VBI in combat).  Two changes, both byte-identical: walk the two slot arrays
       with autoincrement pointers instead of recomputing mem[base+x] every step (12 indexed
       volatile byte reads -> 12 (aN)+ reads, no index arithmetic), and keep the running minimum
       in a register instead of re-reading $0716 for every compare.  The pointers stay volatile,
       so each byte is still read exactly once, in order — only the addressing changes.  $0716 is
       still stored on every improvement so the exit state is untouched, and $0714/$0715 keep
       their previous values when no slot wins, exactly as before. */
    sfx_scan_prio = 0x10;
    uint8_t best = 0x10;                                 /* running min, mirrors $0716 */
    const volatile uint8_t* r = &mem[MEM_sfx_voice_reg_idx + 1];
    const volatile uint8_t* v = &mem[MEM_sfx_env_prio_val + 1];
    uint8_t x = 1;                                       /* body runs x = 1..12 */
    for (;;) {
        if (*r != 0) {                                   /* LDA $0705+X; BEQ skip */
            const uint8_t a = (uint8_t)(*v & 0x0F);
            if (a < best) {                              /* CMP $0716; BCS skip (a>=M) */
                best = a;
                sfx_scan_prio = a;
                sfx_top_prio_val = a;
                sfx_top_voice_idx = x;
            }
        }
        if (x >= 0x0C) break;                            /* CPX #$0C; BCC loop */
        x++; r++; v++;
    }
    cpu.X = x;
}

/* sfx_pick_next_voice @ $56AF — scan slots X=1..12; among EMPTY slots and the
 * excluded slot $0715, latch the largest priority nibble into $0716 / index
 * $0717.  (Faithful to the code: BEQ considers empty slots, else only X==$0715.) */
void sfx_pick_next_voice(void) {
    SX_CNT(g_sxNextScan);
    /* Same treatment as sfx_pick_top_voice above: autoincrement pointer walk, the running
       max ($0716) and the excluded-slot index ($0715, loop-invariant — nothing in the loop
       writes it) held in registers.  Note the compare is >=, so the LAST maximum wins; that
       is preserved. */
    sfx_scan_prio = 0x00;
    uint8_t best = 0x00;                                 /* running max, mirrors $0716 */
    const uint8_t topIdx = sfx_top_voice_idx;            /* $0715, invariant across the scan */
    const volatile uint8_t* r = &mem[MEM_sfx_voice_reg_idx + 1];
    const volatile uint8_t* v = &mem[MEM_sfx_env_prio_val + 1];
    uint8_t x = 1;
    for (;;) {
        if (*r == 0 || x == topIdx) {                    /* BEQ -> consider; else only $0715 */
            const uint8_t a = (uint8_t)(*v & 0x0F);
            if (a >= best) {                             /* CMP $0716; BCC skip (a<M) */
                best = a;
                sfx_scan_prio = a;
                sfx_next_voice_idx = x;
            }
        }
        if (x >= 0x0C) break;
        x++; r++; v++;
    }
    cpu.X = x;
}

/* sfx_engine_step @ $5553 — the explosion/noise voice.  Entry cpu.A = mem[$0634]
 * (the engine state, set by the $548D caller): A==1 -> the descending-pitch
 * branch ($5585); else the RANDOM-reseeded noise branch.  Reads POKEY RANDOM
 * ($D20A) twice on reseed; writes AUDF1/AUDF3/AUDC1/AUDC3/AUDCTL. */
void sfx_engine_step(void) {
    if (cpu.A != 0x01) {
        /* L_555a: noise branch */
        uint8_t v = (uint8_t)(sfx_timer_0636 - 1);          /* DEC $0636 */
        sfx_timer_0636 = v;
        if (v & 0x80) {                                  /* BPL skip; underflow -> reseed */
            sfx_timer_0636 = (uint8_t)((bus_read(0xD20A) & 0x03) + 1);   /* RANDOM&3 +1 */
            uint8_t r = bus_read(0xD20A);
            bus_write(0xD204, (uint8_t)(r | 0x70));      /* AUDC3 */
            bus_write(0xD200, (uint8_t)((r & 0x7F) | 0x17)); /* AUDF1 */
        }
        /* L_557b */
        bus_write(0xD208, 0x04);                         /* AUDCTL */
        cpu.A = (uint8_t)(0x03 | 0xA0);                  /* A=3 -> L_55d3 ORA #$A0 */
        bus_write(0xD201, cpu.A);                        /* AUDC1 */
        bus_write(0xD205, cpu.A);                        /* AUDC3 */
        return;                                          /* (Y/$0637 untouched on this path) */
    }
    /* L_5585: descending-pitch branch */
    uint8_t y = (uint8_t)(mem[0x0635] - 1);              /* DEY */
    if (y & 0x80) y = 0x07;                              /* BPL skip; LDY #7 */
    mem[0x0635] = y;
    uint8_t d = mem[0x55DC + y];
    uint8_t a1 = (uint8_t)(sfx_pitch_0638 - d);             /* SEC; SBC $55DC,Y */
    sfx_pitch_0638 = a1; bus_write(0xD200, a1);             /* AUDF1 */
    uint8_t a3 = (uint8_t)(sfx_pitch_0639 - d);             /* SEC; SBC */
    sfx_pitch_0639 = a3; bus_write(0xD204, a3);             /* AUDF3 */
    uint8_t yy = mem[0x0637];
    uint8_t g  = (uint8_t)(mem[0x063A] - 1);             /* DEC $063A */
    mem[0x063A] = g;
    if (g & 0x80) {                                      /* BPL fails -> underflow */
        yy = (uint8_t)(yy + 2);                          /* INY; INY */
        if (yy >= 0x0F) {                                /* CPY #$0F; BCC skip */
            yy = 0x0F;
            mem[0x063A] = 0x32;
        }
    } else if (g == 0) {                                 /* BNE L_55cf; (g==0) */
        yy = (uint8_t)(yy - 1);                          /* DEY */
        if (yy == 0x06) sfx_state_0634++;                   /* CPY #6; BNE skip; INC $0634 */
        mem[0x063A]++;                                   /* INC $063A */
    }
    /* L_55cf */
    mem[0x0637] = yy;
    cpu.A = (uint8_t)(yy | 0xA0);                        /* TYA; ORA #$A0 */
    bus_write(0xD201, cpu.A);                            /* AUDC1 */
    bus_write(0xD205, cpu.A);                            /* AUDC3 */
    cpu.Y = yy;
}

/* sfx_event_load_core @ $581C — load a new voice into slot mem[$56D4+(event_id-1)] from the
 * SFX event parameter tables.  event_id is 1-based; 0 (or any value whose -1 goes negative)
 * is a no-op.  Populates the voice's distortion/vol/freq/envelope/follow-on fields, then pushes
 * the slot to the event ring via ring_push_unmarked (which leaves cpu.Y = the slot).  Idiomatic
 * entry — the 6502-ABI shim sfx_event_load() below marshals cpu.X into event_id. */
static void sfx_event_load_core(uint8_t event_id) {
    uint8_t i = (uint8_t)(event_id - 1);                 /* DEX */
    if (i & 0x80) return;                                /* BMI L_5876: event_id 0 -> no-op */
    uint8_t y = mem[0x56D4 + i];                         /* voice slot */
    uint8_t ctl = mem[0x56F5 + i];
    mem[MEM_sfx_voice_distortion + y] = (uint8_t)(ctl & 0xF0);         /* distortion */
    mem[MEM_sfx_env_prio_val + y] = (uint8_t)(ctl & 0x0F);         /* prio/vol */
    mem[MEM_sfx_env_freq_val + y] = mem[0x5716 + i];               /* freq */
    mem[0x06A3 + y] = mem[0x5737 + i];               /* duration */
    mem[0x0687 + y] = mem[0x5758 + i];
    mem[0x0695 + y] = mem[0x5779 + i];
    mem[0x06B1 + y] = mem[0x579A + i];
    uint8_t e = mem[0x57BB + i];
    mem[0x06DB + y] = e;
    if (e != 0) {                                    /* BEQ L_586d (skip these 3) */
        mem[0x06BF + y] = mem[0x57DC + i];
        mem[0x06CD + y] = mem[0x57E4 + i];
        mem[0x06E9 + y] = mem[0x57EC + i];
    }
    mem[0x06F7 + y] = mem[0x57F4 + i];               /* L_586d */
    cpu.Y = y;
    ring_push_unmarked();                                 /* push slot Y to the ring */
}
/* sfx_event_load @ $581C — 6502-ABI shim: event id in cpu.X, saves/restores X/Y (PHA/PLA),
 * exit cpu.A = the (pulled) event id, exactly like the transliterated oracle. */
void sfx_event_load(void) {
    uint8_t savedX = cpu.X, savedY = cpu.Y;
    sfx_event_load_core(cpu.X);
    cpu.Y = savedY; cpu.X = savedX;                      /* PLA;TAY; PLA;TAX */
    cpu.A = savedX;
}

/* sfx_reorder_voice_slot @ $5614 — the voice-priority mixer.  Entry cpu.Y = the voice slot
 * just touched.  ⚠ The entry cpu.X is NOT read: `5619 TXA; 561a BNE` tests X as the JSR at
 * 5616 LEFT it, i.e. x = mem[$0705+Y] — the voice's POKEY register index — so the two-way
 * branch is "is this slot ACTIVE?", not a caller-supplied mode.  x == 0 (idle slot) takes the
 * 561c promote/compact path (steal the current top's register); x != 0 takes the 5641 demote
 * path (sfx_pick_next_voice, then move the top's register to the next-best slot).  Either way
 * the moved voice is re-emitted (sfx_voice_write_freq_ctrl) and the top voice re-latched
 * (sfx_pick_top_voice).  Y is saved/restored (PHA/PLA); X is clobbered.
 * (The old comment here called cpu.X "a selector (0 -> ... ; !=0 -> ...)" — wrong about WHERE
 * the value comes from, though the code below has always read it in the right place.  Building
 * the asm twin from that comment produced a first-call mismatch; see SfxMixerAssembler.s.)
 *
 * HOT: the biggest single item inside the 50 Hz flight VBI (3.32 calls/firing at 6.70 t =
 * 22.4 t/firing = ~7% of ALL wall clock in combat — the ISR fires 50x/s regardless of frame
 * rate, so it is a flat tax).  On the Amiga a hand-asm twin (SfxMixerAssembler.s) replaces
 * this whole chain — all three leaves inlined, table bases pinned in address registers —
 * via the ROF_SFXMIX_ASM seam below; this stays the SDL/validate oracle. */
void sfx_reorder_voice_slot_c(void) {
    uint8_t savedY = cpu.Y;                              /* TYA; PHA */
    SX_NOP();                                            /* empty-bracket floor, 1 per call */
    SX_LEAF(sfx_voice_write_freq_ctrl(), g_sxWrCtrlT);    /* 5616 (writes voice cpu.Y) */
    cpu.Y = savedY;
    int do_pick_top = 1;
    if (cpu.X == 0) {                                    /* TXA; BNE L_5641 -> here X==0 */
        uint8_t a = (uint8_t)(mem[MEM_sfx_env_prio_val + cpu.Y] & 0x0F);
        if (a < sfx_top_prio_val) {                           /* CMP $0714; BCC L_5664 */
            do_pick_top = 0;
        } else {
            int move = (a != sfx_top_prio_val)                /* BNE L_562d */
                     || (cpu.Y >= sfx_top_voice_idx);          /* equal: CPY $0715; BCC L_5664 else move */
            if (move) {                                  /* L_562d */
                uint8_t tx = sfx_top_voice_idx;
                mem[MEM_sfx_voice_reg_idx + cpu.Y] = mem[MEM_sfx_voice_reg_idx + tx];
                mem[MEM_sfx_voice_reg_idx + tx] = 0x00;
                SX_LEAF(sfx_voice_write_freq_ctrl(), g_sxWrCtrlT);   /* 563b (re-uses cpu.Y) */
            } else {
                do_pick_top = 0;                         /* a==top && Y<topidx -> L_5664 */
            }
        }
    } else {                                             /* L_5641: X!=0 */
        if (cpu.Y < 0x0D) {                              /* CPY #$0D; BCS L_5661 */
            SX_LEAF(sfx_pick_next_voice(), g_sxNextScanT);
            uint8_t tx = sfx_top_voice_idx;
            if (tx != sfx_next_voice_idx) {                     /* LDX $0715; CPX $0717; BEQ L_5661 */
                cpu.Y = sfx_next_voice_idx;                     /* LDY $0717 */
                mem[MEM_sfx_voice_reg_idx + cpu.Y] = mem[MEM_sfx_voice_reg_idx + tx];
                mem[MEM_sfx_voice_reg_idx + tx] = 0x00;
                SX_LEAF(sfx_voice_write_freq_ctrl(), g_sxWrCtrlT);  /* 565e */
            }
        }
    }
    if (do_pick_top) SX_LEAF(sfx_pick_top_voice(), g_sxTopScanT);  /* L_5661 */
    cpu.Y = savedY;                                      /* L_5664: PLA; TAY */
    cpu.A = savedY;
}

/* Dispatcher seam (asm-migration-plan Phase 6), mirrors project_terrain_points_core.
 * On the Amiga (ROF_SFXMIX_ASM) sfx_reorder_voice_slot is the hand-written m68k twin in
 * SfxMixerAssembler.s; elsewhere it is the clean-C twin above.  `make SFXMIX_C=1` falls back
 * to the C on the Amiga too — needed for the SFX_SHAPE probe, whose SX_LEAF brackets live in
 * the C bodies the asm replaces (the documented "asm twins kill their C shape-counters"). */
#if defined(ROF_SFXMIX_ASM) && defined(ROF_SFXMIX_VERIFY)
/* On-target differential (single run, deterministic): run the asm twin on the real state,
 * snapshot everything it can write, restore, run the C twin on the same inputs, compare.
 * The C twin's output is left LIVE so audio stays correct on an asm bug.  Read via
 * amiga/sfxmix_verify.gdb.
 *
 * The window is exact: the chain writes only mem[$0705-$0717] (the voice reg-idx table plus
 * the four scan cells $0714-$0717), the POKEY mirror mem[$D200-$D20F], and cpu.A/X/Y.
 *
 * ⚠⚠ THE ORDER MUST ALTERNATE, because the callee is NOT pure.  Unlike the other asm twins in
 * this tree, this chain calls out to rof_pokey_write -> update_paula_channel, which sits behind
 * several global caches (pokey[] change-detect, poly_dist_* rebuild key, noiseOn[]).  Whichever
 * side runs FIRST warms them and the second side gets its Paula work for free — measured as the
 * asm being 24-34% "slower" when it always ran first, and rewinding pokey[] alone did NOT fix
 * it (the other caches remain).  So each call runs the pair in alternating order and each side
 * accumulates its own ticks; the bias then lands equally on both and cancels in the ratio.
 * Consequence: the LIVE result is the asm's on odd calls and the C's on even ones — fine only
 * because mismatch is 0.  If you ever see mismatch != 0 here, audio state is half-corrupt by
 * construction; fix the asm, don't chase the audio. */
extern void sfx_reorder_voice_slot_asm(void);
extern void rof_pokey_shadow_save(uint8_t* dst);
extern void rof_pokey_shadow_load(const uint8_t* src);
volatile unsigned long g_sfxmixCalls = 0, g_sfxmixMismatch = 0, g_sfxmixFirstBad = 0;
volatile unsigned long g_sfxmixBadCell = 0, g_sfxmixAsmTicks = 0, g_sfxmixCTicks = 0;
#define SFXMIX_NCELL 35   /* $0705-$0717 (19) + $D200-$D20F (16) */
static uint16_t sfxmix_cell(int i) {
    return (uint16_t)(i < 19 ? 0x0705 + i : 0xD200 + (i - 19));
}
void sfx_reorder_voice_slot(void) {
    const unsigned long n = ++g_sfxmixCalls;
    uint8_t* const M = (uint8_t*)mem;
    uint8_t snap[SFXMIX_NCELL], firstv[SFXMIX_NCELL], pokeySnap[16];
    const Cpu6502 cpuSnap = cpu;
    const int asmFirst = (int)(n & 1u);              /* alternate — see the header comment */
    for (int i = 0; i < SFXMIX_NCELL; i++) snap[i] = M[sfxmix_cell(i)];
    rof_pokey_shadow_save(pokeySnap);

    if (asmFirst) FP_TIME(sfx_reorder_voice_slot_asm(), g_sfxmixAsmTicks);
    else          FP_TIME(sfx_reorder_voice_slot_c(),   g_sfxmixCTicks);
    for (int i = 0; i < SFXMIX_NCELL; i++) firstv[i] = M[sfxmix_cell(i)];
    const Cpu6502 cpuFirst = cpu;

    for (int i = 0; i < SFXMIX_NCELL; i++) M[sfxmix_cell(i)] = snap[i];
    cpu = cpuSnap;
    rof_pokey_shadow_load(pokeySnap);

    if (asmFirst) FP_TIME(sfx_reorder_voice_slot_c(),   g_sfxmixCTicks);
    else          FP_TIME(sfx_reorder_voice_slot_asm(), g_sfxmixAsmTicks);

    int bad = 0;
    for (int i = 0; i < SFXMIX_NCELL; i++)
        if (M[sfxmix_cell(i)] != firstv[i]) { bad = 1; g_sfxmixBadCell = sfxmix_cell(i); }
    if (cpu.A != cpuFirst.A || cpu.X != cpuFirst.X || cpu.Y != cpuFirst.Y) {
        bad = 1; g_sfxmixBadCell = 0xC0FF;
    }
    if (bad) { if (!g_sfxmixMismatch) g_sfxmixFirstBad = n; g_sfxmixMismatch++; }
}
#elif defined(ROF_SFXMIX_ASM)
extern void sfx_reorder_voice_slot(void);   /* SfxMixerAssembler.s */
#else
void sfx_reorder_voice_slot(void) { sfx_reorder_voice_slot_c(); }
#endif

#if defined(ROF_SFXMIX_ASM) && defined(ROF_SFXMIX_FUZZ)
/* ── ON-TARGET FUZZ of the asm mixer twin (`make FUZZ=1`, read via amiga/sfxmix_fuzz.gdb) ─────
 * WHY THIS EXISTS, and why the flight differential was not enough: sfxmix_verify only compares
 * the two twins on states flight actually reaches, so a path flight rarely takes (e.g. the
 * y >= $0D early-out at $5641, or a voice reg index outside the POKEY window) can be wrong and
 * never show up.  `make validate` does cover those — but only for the C twin, on the host; it
 * cannot run m68k asm.  This closes that gap: randomised mem[] state, every path, on target.
 *
 * Runs ONCE at startup from main() (before any game code), so it is free at runtime.  It seeds
 * mem[$0600-$0820] — wide enough to cover every cell the chain READS ($065D/$066B/$0679 columns,
 * $0705+ reg idx, $0714-$0717) and every cell it can WRITE (the moves index $0705 by an
 * arbitrary byte, so up to $0705+$FF = $0804) — plus cpu.Y over 0..15 so the $0D boundary and
 * both scan branches are hit.  Everything is restored afterwards: the window, pokey[], the
 * pending-Paula flag and cpu, so the game boots into untouched state. */
#define SFXF_LO 0x0600u
#define SFXF_HI 0x0820u
#define SFXF_N  (SFXF_HI - SFXF_LO)
extern void rof_pokey_want_reset(void);
volatile unsigned long g_sfxFuzzCases = 0, g_sfxFuzzBad = 0, g_sfxFuzzFirstBad = 0;
volatile unsigned long g_sfxFuzzBadCell = 0, g_sfxFuzzBadY = 0;
volatile unsigned long g_sfxFuzzBadAsm = 0, g_sfxFuzzBadC = 0;
/* Path coverage, derived from the inputs in the driver (no instrumentation inside the twins):
 * 0 = idle slot, a < top ($5624 done)          1 = idle slot, promote ($562d move)
 * 2 = idle slot, a == top and y < topidx       3 = active slot, y >= $0D ($5643 early-out)
 * 4 = active slot, next-best == top (no move)  5 = active slot, move to next-best ($5650) */
volatile unsigned long g_sfxFuzzPath[6] = { 0, 0, 0, 0, 0, 0 };
static unsigned long sfxf_rng = 0x1D872B41uL;
static uint8_t sfxf_rand(void) {
    sfxf_rng ^= sfxf_rng << 13; sfxf_rng ^= sfxf_rng >> 17; sfxf_rng ^= sfxf_rng << 5;
    return (uint8_t)(sfxf_rng >> 11);
}
void sfx_mixer_fuzz(unsigned long cases) {
    uint8_t* const M = (uint8_t*)mem;
    static uint8_t keep[SFXF_N], seed[SFXF_N], asmv[SFXF_N], pokeyKeep[16], pokeySeed[16];
    const Cpu6502 cpuKeep = cpu;
    for (unsigned i = 0; i < SFXF_N; i++) keep[i] = M[SFXF_LO + i];
    rof_pokey_shadow_save(pokeyKeep);

    /* Seed only the columns the chain READS ($065D-$0717) — seeding the whole compare window
     * cost 544 PRNG calls/case and made boot take minutes on the 7 MHz 68000.  The compare still
     * spans $0600-$0820 to catch a stray write; the unseeded parts are restored identically
     * before each of the two runs, so they compare equal unless something writes out of bounds. */
    for (unsigned long t = 0; t < cases; t++) {
        for (unsigned i = 0; i < SFXF_N; i++) seed[i] = keep[i];
        for (unsigned a = 0x065Du; a <= 0x0717u; a++) seed[a - SFXF_LO] = sfxf_rand();
        for (int i = 0; i < 16; i++) pokeySeed[i] = sfxf_rand();
        const uint8_t y = (uint8_t)(sfxf_rand() & 0x0F);   /* 0..15: straddles the $0D boundary */
        /* tx = mem[$0715] is a voice SLOT index in the real game, so keep it 0..15; that also
         * bounds the two moves' writes to $0705+0..15 instead of $0705+$FF. */
        seed[0x0715u - SFXF_LO] &= 0x0F;
        /* ⚠ BIAS, or the interesting branches never run.  With uniform bytes mem[$0705+y] is 0
         * only 1/256 of the time, so the first fuzz run hit the three idle-slot paths 10/2/0
         * times out of 4543 — and the exact-priority-TIE path ($5628 CPY $0715, the y-vs-topidx
         * tiebreak) not at all.  Force an idle slot in half the cases, and inside those force
         * a == $0714 in a quarter so the tiebreak is exercised from both sides. */
        {
            const uint8_t sel = sfxf_rand();
            if (sel & 1) {
                seed[(0x0705u + y) - SFXF_LO] = 0;              /* idle slot -> the 561c path */
                if ((sel & 6) == 0) {
                    const uint8_t a = (uint8_t)(sfxf_rand() & 0x0F);
                    seed[(0x066Bu + y) - SFXF_LO] = a;          /* a == top: exact tie */
                    seed[0x0714u - SFXF_LO] = a;
                }
            } else if ((sel & 6) == 0) {
                seed[(0x0705u + y) - SFXF_LO] = (uint8_t)(2 + (sfxf_rand() % 7));  /* in-range reg */
            }
        }

        /* Classify the path from the seeded inputs, for the coverage report. */
        {
            const uint8_t x0 = seed[(0x0705u + y) - SFXF_LO];
            if (x0 == 0) {
                const uint8_t a   = (uint8_t)(seed[(0x066Bu + y) - SFXF_LO] & 0x0F);
                const uint8_t top = seed[0x0714u - SFXF_LO];
                if (a < top)                                   g_sfxFuzzPath[0]++;
                else if (a != top || y >= seed[0x0715u - SFXF_LO]) g_sfxFuzzPath[1]++;
                else                                           g_sfxFuzzPath[2]++;
            } else if (y >= 0x0D)                              g_sfxFuzzPath[3]++;
            else {
                /* the pick_next result is not known without running it; count both buckets by
                 * the post-run $0715 vs $0717 comparison instead (done after the asm run). */
            }
        }

        for (unsigned i = 0; i < SFXF_N; i++) M[SFXF_LO + i] = seed[i];
        rof_pokey_shadow_load(pokeySeed);
        cpu.A = sfxf_rand(); cpu.X = sfxf_rand(); cpu.Y = y;
        const Cpu6502 cpuIn = cpu;

        sfx_reorder_voice_slot();                       /* the asm twin */
        for (unsigned i = 0; i < SFXF_N; i++) asmv[i] = M[SFXF_LO + i];
        uint8_t asmPokey[16]; rof_pokey_shadow_save(asmPokey);
        const Cpu6502 cpuAsm = cpu;

        if (seed[(0x0705u + y) - SFXF_LO] != 0 && y < 0x0D)
            g_sfxFuzzPath[asmv[0x0715u - SFXF_LO] == asmv[0x0717u - SFXF_LO] ? 4 : 5]++;

        for (unsigned i = 0; i < SFXF_N; i++) M[SFXF_LO + i] = seed[i];
        rof_pokey_shadow_load(pokeySeed);
        cpu = cpuIn;
        sfx_reorder_voice_slot_c();                     /* the oracle */
        uint8_t cPokey[16]; rof_pokey_shadow_save(cPokey);

        g_sfxFuzzCases++;
        int bad = 0;
        for (unsigned i = 0; i < SFXF_N; i++)
            if (M[SFXF_LO + i] != asmv[i]) {
                bad = 1;
                if (!g_sfxFuzzBad) { g_sfxFuzzBadCell = SFXF_LO + i;
                                     g_sfxFuzzBadAsm = asmv[i]; g_sfxFuzzBadC = M[SFXF_LO + i]; }
            }
        /* pokey[] is the real AUDF/AUDC the channel ends up with — the distortion + volume
         * nibble the reported bugs are about — so compare it, not just the mem[] mirror. */
        for (int i = 0; i < 16; i++)
            if (cPokey[i] != asmPokey[i]) {
                bad = 1;
                if (!g_sfxFuzzBad) { g_sfxFuzzBadCell = (unsigned long)(0xD200 + i);
                                     g_sfxFuzzBadAsm = asmPokey[i]; g_sfxFuzzBadC = cPokey[i]; }
            }
        if (cpu.A != cpuAsm.A || cpu.X != cpuAsm.X || cpu.Y != cpuAsm.Y) {
            bad = 1;
            if (!g_sfxFuzzBad) g_sfxFuzzBadCell = 0xC0FFu;
        }
        if (bad) { if (!g_sfxFuzzBad) { g_sfxFuzzFirstBad = g_sfxFuzzCases; g_sfxFuzzBadY = y; }
                   g_sfxFuzzBad++; }
    }

    for (unsigned i = 0; i < SFXF_N; i++) M[SFXF_LO + i] = keep[i];
    rof_pokey_shadow_load(pokeyKeep);
    rof_pokey_want_reset();            /* drop the garbage Paula "want" the fuzz just recorded */
    cpu = cpuKeep;
}
#endif

/* Advance one envelope phase by its step, wrapping like the 6502 BCD-ish accumulator:
 * a sum that goes negative ($80+) wraps forward by $0A; otherwise it saturates just below
 * the $2D table size (at $2C). */
static uint8_t sfx_phase_wrap(uint8_t step, uint8_t phase) {
    uint16_t s = (uint16_t)step + phase;
    uint8_t  a = (uint8_t)s;
    if (a & 0x80)       a = (uint8_t)(a + 0x0A + (uint8_t)(s >> 8));
    else if (a >= 0x2D) a = 0x2C;
    return a;
}

/* sfx_voice_envelope_tick @ $548D — per-frame voice/gauge envelope engine + event-ring drain.
 *
 * 1. Run the active sound-effect generator (sfx_engine_step) when armed ($0634).
 * 2. For each of the 14 voice/gauge slots (Y=$0E..1), advance two parallel envelopes:
 *      - frequency: step its phase; while the phase indexes a live entry in the $5406 gate
 *        table, add the per-step delta to the frequency field and emit it (sfx_voice_write_freq);
 *        expire the slot when the field reaches its target;
 *      - duration/priority: same, on the 4-bit priority field, re-queuing via ring_push_unmarked.
 *    A slot that finished either envelope re-queues its event id (bit7-marked) on the ring.
 * 3. Drain the $0719 event ring (tail $0074 -> head $0073, wrapping at $1F): bit7-set entries
 *    start a new voice (sfx_event_load), the rest reorder a sprite slot.
 *
 * Contract: memory only.  No hardware writes here (the AUDF pokes live inside the callees). */
#ifdef ROF_FLIGHT_PROBE
extern volatile unsigned long g_pSfxEng, g_pSfxLoop, g_pSfxRing;
#endif
static void sfx_voice_envelope_tick_impl(void) {
#ifdef ROF_FLIGHT_PROBE
    unsigned long _e0 = rof_subclock();
#endif
    if (sfx_state_0634 != 0) {
        cpu.A = sfx_state_0634;          /* sfx_engine_step reads its mode from A */
        sfx_engine_step();
    }
#ifdef ROF_FLIGHT_PROBE
    g_pSfxEng += rof_subclock() - _e0; unsigned long _l0 = rof_subclock();
#endif

    /* ⭐ ADDRESSING, not work.  All eleven per-slot arrays live in $066B..$06F7 — a 140-byte
     * window — so ONE pointer at $06DB+y reaches every one of them with a 16-bit displacement
     * in [-112,+28].  Written as mem[BASE + y] instead, GCC has to materialise each address
     * from the 8-bit slot index every time (with `volatile mem[]` that was
     * `lea off(a0),a1 / move.b (0,a2,a1.l),d` = 22 cycles a byte), and the loop runs 14 times
     * a firing whether or not any envelope is active.  The skip path — both step bytes zero,
     * which is the common case — becomes two 12-cycle displacement loads and a branch.
     * `s` is a plain (non-volatile) view: nothing preempts the flight VBI, so the only thing
     * the qualifier was buying here was worse code.  mem[] semantics are unchanged. */
    uint8_t expired = 0;
    uint8_t *s = (uint8_t *)&mem[0x06DB + 0x0E];     /* slot 14; walks down with y */
    for (uint8_t y = 0x0E; y != 0; y--, s--) {
        expired = 0;                     /* per-slot "an envelope finished" flag ($0718) */

        /* Frequency envelope.  Offsets from $06DB: phase +14, delta -28, target -14,
         * value ($0679) -98, event id +28. */
        if (s[0] != 0) {                  /* nonzero step = active */
            SX_CNT(g_sxActFreq);
            uint8_t ph = sfx_phase_wrap(s[0], s[14]);
            s[14] = ph;
            if (mem[0x5406 + ph] == 0) SX_CNT(g_sxEnvGated);
            if (mem[0x5406 + ph] != 0) {  /* gate table: zero entry pauses the step */
                uint8_t f = (uint8_t)(s[-98] + s[-28]);
                s[-98] = f;
                if (f == s[-14]) { s[0] = 0; expired++; }  /* hit target */
                cpu.Y = y; sfx_voice_write_freq();
            }
        }

        /* Duration / priority envelope (priority field kept to 4 bits).  Offsets from $06DB:
         * step -56, phase -42, delta -84, target -70, value ($066B) -112. */
        if (s[-56] != 0) {
            SX_CNT(g_sxActDur);
            uint8_t ph = sfx_phase_wrap(s[-56], s[-42]);
            s[-42] = ph;
            if (mem[0x5406 + ph] != 0) {
                uint8_t p = (uint8_t)((s[-112] + s[-84]) & 0x0F);
                s[-112] = p;
                if (p == s[-70]) { s[-56] = 0; expired++; }
                cpu.Y = y; ring_push_unmarked();
            }
        }

        /* Either envelope finished -> re-queue this slot's event id (bit7-marked) on the ring. */
        if (expired != 0) {
            SX_CNT(g_sxExpired);
            cpu.X = s[28];
            ring_push_marked();
        }
    }
    sfx_voice_expired_flag = expired;     /* final state = the last slot's flag */
#ifdef ROF_FLIGHT_PROBE
    g_pSfxLoop += rof_subclock() - _l0; unsigned long _r0 = rof_subclock();
#endif

    /* Drain the event ring from tail down to head, wrapping the index at $1F. */
    if (alt_ring_head  > 0x1F) alt_ring_head  = 0x1F;
    if (ring_tail_0719 > 0x1F) ring_tail_0719 = 0x1F;
    while (ring_tail_0719 != alt_ring_head) {
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned long g_sfxRingIters; g_sfxRingIters++; }
#endif
        uint8_t entry = mem[MEM_event_ring_0719 + ring_tail_0719];
        cpu.A = entry;                    /* preserve the 6502 register state the callees see */
        if (entry & 0x80) {               /* new-voice request */
            cpu.X = (uint8_t)(entry & 0x7F);
            SX_CNT(g_sxEvLoad); SX_SPAN(sfx_event_load(), g_sxEvLoadT);
        } else {                          /* sprite-slot reorder request */
            cpu.Y = entry;
            SX_CNT(g_sxReord); SX_SPAN(sfx_reorder_voice_slot(), g_sxReordT);
        }
        uint8_t t = (uint8_t)(ring_tail_0719 - 1);
        ring_tail_0719 = (t & 0x80) ? 0x1F : t;
    }
#ifdef ROF_FLIGHT_PROBE
    g_pSfxRing += rof_subclock() - _r0;
#endif
}
#ifdef ROF_FLIGHT_PROBE
extern volatile unsigned long g_pSfx;
void sfx_voice_envelope_tick(void) { unsigned long _p = rof_subclock(); sfx_voice_envelope_tick_impl(); g_pSfx += rof_subclock() - _p; }
#else
void sfx_voice_envelope_tick(void) { sfx_voice_envelope_tick_impl(); }
#endif

/* Typed argument wrappers over the (still-transpiled) callees that take their argument in a
 * 6502 register, so vbi_handler_flight's call sites stay idiomatic (no inline cpu.X/Y/A pokes).
 * Each marshals the arg into the register the transpiled body reads.  This is the inverse of
 * the usual `_core` seam — here the callee is the transpiled cpu-ABI function, so the wrapper
 * is the typed face; when these callees are themselves made native, the wrapper becomes the
 * direct typed call.  (Register contracts confirmed against the $4FF5 disasm:
 *   build_player2_sprite  reads X  -> stored to $2867;
 *   draw_player3_object   reads A  (the $006A object byte; the oracle PHA/PLA-preserves it);
 *   set_colpf0_from_flag  reads Y  (the message index; it falls into show_cockpit_message);
 *   event_sequence_dispatcher reads X (and A=X) — the keyboard command id;
 *   ring_push_marked      reads X  -> pushed as X|$80 onto the $0719 event ring.) */
static void draw_player3_object_core(uint8_t obj_byte)       { cpu.A = obj_byte;        draw_player3_object(); }
static void set_colpf0_from_flag_core(uint8_t msg_index)     { cpu.Y = msg_index;       set_colpf0_from_flag(); }
static void event_sequence_dispatcher_core(uint8_t keycode)  { cpu.A = cpu.X = keycode; event_sequence_dispatcher(); }
/* ring_push_marked_core — typed entry for native callers: push (id | $80) into the event ring.
 * The 6502 ring_push_marked PHA'd the id, so reproduce that one stack-page byte for byte-identity
 * with validated callers' oracles (a dead byte on the Amiga); no cpu register contract needed. */
static void ring_push_marked_core(uint8_t id) { mem[0x0100 | cpu.S] = id; ring_push_0719_core((uint8_t)(id | 0x80)); }

/* build_player2_sprite @ $8C58 — the depth-scaled object/explosion P2 sprite builder.  Runs every
 * flight VBI frame while object_anim_frame ($0036) != 0 (an object approaching / an emplacement
 * exploding), rebuilding the P2 strip $0E32 at a vertical scale derived from range ($0038/$0039).
 * Faithful CFG twin of the $8C58 oracle (goto labels = the $8Cxx/$8Dxx addresses); the transpiled
 * form recomputed 6502 N/Z/C flags + a bus dispatch on every op in its erase / accumulate / draw
 * loops — this native form drops all of that (the win) while writing byte-identical mem[].
 * Entry: anim_frame = the 6502 entry X (stored to $2867).  All state is mem[]; the 3 HW writes
 * (HPOSP2/COLPM2/SIZEP2) go through bus_write to mirror the oracle (ignored on the Amiga). */
static void build_player2_sprite_core(uint8_t anim_frame) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t a, x, y;

    /* 8c58: erase the previous strip region ($0E32+$2865, for $2866 rows). */
    if (mem[0x2866] != 0) {
        dl_y3 = mem[0x2866];
        y = mem[0x2865];
        do { mem[0x0E32 + y] = 0x00; y++; dl_y3--; } while (dl_y3 != 0);
    }
    /* L_8c6c */
    mem[0x286E] = 0x00;
    sizep2_shadow = 0x00;
    dl_y3 = 0x01;
    mem[0x2867] = anim_frame;                       /* 8c75 STX $2867 */

    /* 8c78: from the object row count derive a table index + a scaled accumulator. */
    x = vobj_row_count;                             /* 8c78/8c7a */
    mem[0x2868] = (uint8_t)(vobj_row_count & 0x03); /* 8c7b/8c7d */
    a = (uint8_t)(x >> 2);                          /* 8c80-8c82 */
    {                                               /* 8c83 SEC; 8c84 SBC #$10 */
        int c = (a >= 0x10);
        a = (uint8_t)(a - 0x10);
        if (!c) { a = mem[0x28B6]; goto L_8cae; }   /* 8c86 BCS L_8c8d; else 8c88/8c8b BCC L_8cae */
    }
    /* L_8c8d */
    if (a >= 0x1F) { a = mem[0x28D5]; goto L_8cae; } /* 8c8d CMP #$1F; 8c91/8c94 BCS L_8cae */
    /* L_8c96: accumulate 4 table entries */
    y = a; x = 0x03; a = 0x00;
    do {                                            /* L_8c9b */
        a = (uint8_t)(a + mem[0x28B6 + y]);         /* CLC; ADC $28B6,Y */
        if (x == mem[0x2868]) y++;                  /* CPX $2868; BNE; INY */
        x--;                                        /* DEX */
    } while (!(x & 0x80));                          /* BPL */
    { int c;                                        /* 8ca8-8cad CMP #$80;ROR x2 */
      c = (a >= 0x80); a = (uint8_t)((c << 7) | (a >> 1));
      c = (a >= 0x80); a = (uint8_t)((c << 7) | (a >> 1)); }
L_8cae:
    mem[0x2869] = a;
    x = mem[0x2867];
    if (x < 0x1A) goto L_8cc7;                      /* 8cb4/8cb6 */
    if (x == 0x1A) goto L_8cba;                     /* 8cb8 (fall = Z) */
    goto L_8d30;                                    /* 8cc4 */
L_8cba:
    object_anim_frame = 0x00;                       /* 8cbc */
    mem[0x284A] = 0x01;                             /* 8cc0 */
    return;
L_8cc7:
    mem[0x284A] = x;                                /* 8cc7 */
    a = mem[0x8DB5 + x];                            /* 8cca */
    a = (uint8_t)(a + mem[0x0039]);                 /* 8cce */
    a = (uint8_t)(a + mem[0x2869]);                 /* 8cd1 */
    if (a & 0x80) a = 0x58;                         /* 8cd4/8cd6 */
    x = a;                                          /* 8cd8 TAX */
    a = object_anim_frame;                          /* 8cd9 */
    if (a >= 0x08) a = 0x07;                        /* 8cdb/8cdf */
    y = a;                                          /* 8ce1 TAY */
    a = mem[0x8E2B + y];                            /* 8ce2 -> fall into L_8ce5 */
L_8ce5:
    y = a;                                          /* 8ce5 TAY */
    mem[0x2865] = x;                                /* 8ce6 STX $2865 */
    if (landing_inhibit_flag != 0 && figure_is_alien != 0 && mem[0x007A] == 0)
        goto L_8d00;                                /* 8ce9-8cf5 */
L_8cf7:
    dl_y4 = dl_y3;                                  /* 8cf7/8cf9 */
    a = mem[0x8DD0 + y];                            /* 8cfb */
    if (a != 0) goto L_8d20;                        /* 8cfe */
L_8d00:
    mem[0x2866] = (uint8_t)(x - mem[0x2865]);       /* 8d00-8d05 SEC;TXA;SBC $2865 */
    object_anim_frame++;                            /* 8d08 */
    a = (uint8_t)(vobj_row_count - mem[0x286E]);    /* 8d0a-8d0d SEC;LDA $0038;SBC $286E */
    hposp2_shadow = a;                              /* 8d10 */
    bus_write(0xD002, a);                           /* 8d12 HPOSP2 */
    bus_write(0xD014, mem[0x0037]);                 /* 8d17 COLPM2 */
    bus_write(0xD00A, sizep2_shadow);                /* 8d1a-8d1c SIZEP2 (P2 width 1x/2x/4x) */
    return;
L_8d20:
    if (x < 0x56) { mem[0x0E32 + x] = a; x++; }     /* 8d20-8d27 */
    dl_y4--;                                        /* 8d28 */
    if (dl_y4 != 0) goto L_8d20;                    /* 8d2a */
    y++;                                            /* 8d2c */
    goto L_8cf7;                                    /* 8d2d */
L_8d30:
    x = mem[0x284A];                                /* 8d30 */
    a = mem[0x286A];                                /* 8d33 */
    if (a == 0) {                                   /* 8d36 BNE L_8d54 */
        mem[0x0037] = 0x0E;                         /* 8d3a */
        y = mem[0x284A];                            /* 8d3c */
        if (y >= 0x09) y = 0x08;                    /* 8d3f/8d43 */
        mem[0x286C] = mem[0x8E52 + y];              /* 8d45-8d48 */
        a = mem[0x8E4A + y];                        /* 8d4b */
        mem[0x286D] = a;                            /* 8d4e */
        mem[0x286A] = a;                            /* 8d51 */
    }
    /* L_8d54 */
    if (a >= mem[0x286C]) {                         /* 8d54/8d57 */
        mem[0x0037] = 0x3E;                         /* 8d5b */
        mem[0x286B] = 0xFF;                         /* 8d5f */
    }
    /* L_8d62 */
    a = (uint8_t)(a + mem[0x286B]);                 /* 8d62/8d63 */
    mem[0x286A] = a;                                /* 8d66 */
    y = a;                                          /* 8d69 TAY */
    a = (uint8_t)(mem[0x8DB5 + x] - mem[0x8E3E + y]); /* 8d6a-8d6e SEC;SBC $8E3E,Y */
    a = (uint8_t)(a + mem[0x0039]);                 /* 8d71/8d72 */
    a = (uint8_t)(a + mem[0x2869]);                 /* 8d74/8d75 */
    if (a & 0x80) a = 0x58;                         /* 8d78/8d7a */
    x = a;                                          /* 8d7c TAX */
    a = mem[0x286B];                                /* 8d7d */
    if (!(a & 0x80)) goto L_8d8c;                   /* 8d80 BPL */
    mem[0x0037]--;                                  /* 8d82 */
    if (y >= mem[0x286D]) goto L_8d8c;              /* 8d84/8d87 BCS */
    goto L_8cba;                                    /* 8d89 */
L_8d8c:
    a = y;                                          /* 8d8c TYA */
    if (a < 0x07) goto L_8dae;                      /* 8d8d/8d8f BCC L_8dae */
    if (a < 0x0A) {                                 /* 8d91/8d93 BCC L_8da2 (7<=a<$0A) */
        mem[0x286E] = 0x04;                         /* 8da2/8da4 */
        sizep2_shadow = 0x01;                        /* 8da7/8da9 */
        dl_y3 = 0x02;                               /* 8dab INY(=2); 8dac STY $BD */
    } else {                                        /* a>=$0A: 8d97 fall-through */
        mem[0x286E] = 0x0C;                         /* 8d97 */
        sizep2_shadow = 0x03;                        /* 8d9a/8d9c */
        dl_y3 = 0x04;                               /* 8d9e INY(=4); 8dac STY $BD */
    }
L_8dae:
    y = a;                                          /* 8dae TAY */
    a = mem[0x8E32 + y];                            /* 8daf */
    goto L_8ce5;                                    /* 8db2 */
#ifdef ROF_MEMBASE
#undef mem
#endif
}
void build_player2_sprite(void) { build_player2_sprite_core(cpu.X); }   /* 6502-ABI shim (entry X) */

/* update_p3_indicator_stripe @ $4467 — rewrites the P3 scope-indicator PM buffer ($0F98) when the
 * object cursor ($2846) differs from p3_object_state ($2845): clears 5 bytes at the old cursor,
 * advances/clamps the cursor, and (if non-zero) copies a 5-byte glyph from table $44C7 selected by
 * $2839/$283A; always finishes by latching $00CC + SIZEP3 from the object bottom-Y ($2847).  Runs
 * 50Hz in the flight VBI whenever a P3 object/target is active.  Faithful native twin of the $4467
 * oracle (byte-identical mem[]; the copy-loop counter dl_y1 is left at its final 0, as the oracle
 * does — no register args, exit regs incidental). */
void update_p3_indicator_stripe(void) {
    uint8_t y = mem[0x2846];                              /* 4467 cursor */
    if (y != p3_object_state) {                           /* 446a/446d CPY $2845; BNE */
        for (uint8_t x = 5; x != 0; x--) mem[0x0F98 + y++] = 0x00;   /* 4471-4478 clear 5 bytes */
        y = p3_object_state;                              /* 447a LDY $2845 */
        if ((y & 0x80) || y >= 0x2B) y = 0x00;            /* 447d BMI / 447f CPY #$2B / 4483 TAY(0) */
        mem[0x2846] = y;                                  /* 4484 */
        p3_object_state = y;                              /* 4487 */
        if (y == 0) return;                               /* 448b BEQ L_44c6 (skip the stripe rewrite) */
        uint8_t x;                                        /* 448d select the glyph-table base */
        if (mem[0x2839] & 0x80)        x = 0x00;          /* 4490 BPL not taken ($2839 negative) → X=0 */
        else if (mem[0x283A] != 0)     x = 0x0A;          /* 449a BNE L_44a1 → X=$0A */
        else                           x = 0x05;          /* 449c → X=5 */
        for (uint8_t n = 5; n != 0; n--)                  /* L_44a7: copy 5 bytes from $44C7 */
            mem[0x0F98 + y++] = mem[0x44C7 + x++];
        dl_y1 = 0x00;                                     /* $00BB: the copy loop leaves it at 0 */
    }
    /* L_44b3 */
    uint8_t a = mem[0x2847];                              /* 44b5 */
    if (a < 0x6C || a >= 0x8D) a = 0x00;                  /* 44b8-44be: keep only $6C <= a < $8D */
    player3_bottom_y = a;                                 /* 44c1 $00CC */
    bus_write(0xD00B, 0x00);                              /* 44c3 SIZEP3 = Y(0) */
}

/* ===========================================================================================
 * Enemy lock-on indicator (#11) — the six targeting lights at the bottom centre of the
 * cockpit (glyph cells $3491-$3497).  A small state machine on $007E
 * (lock_on_indicator_state) sweeps the lights on, holds them randomly blinking, then sweeps
 * them back off:
 *     state 0        one-shot init: light all six ($A9) and begin the fill sweep (-> state 1)
 *     state 1..6     fill sweep: light one more cell ($29) per timer tick
 *     state 7        fully lit: latch "locked on" ($0048 + $28EE) once
 *     state $80      random blink: flip one cell's colour bit each timer tick
 *     state $81..    reverse sweep: clear the cells one per phase tick, back toward off
 * The standby VBI runs this through the planet descent; the flight VBI runs it via
 * lock_on_indicator_dispatch.  Every glyph write calls platform_lockon_changed() (a no-op on
 * the host/SDL build) so the Amiga cockpit decoder re-renders the strip — the transpiled
 * originals raised no such dirty signal, which froze the lights once flight began.
 * ========================================================================================= */

/* Write one glyph into the indicator strip and flag THAT CELL for re-decode on the Amiga
 * (the hook takes the cell's offset from $3491: the blink rewrites one cell at a time, so a
 * strip-wide flag made the decoder re-render all seven). */
static inline void lockon_write(uint16_t addr, uint8_t glyph) {
    mem[addr] = glyph;
    platform_lockon_changed((uint8_t)(addr - 0x3491u));
}

/* $428D lock_on_indicator_return — shared exit landing pad; the 6502 parked a bare RTS here. */
void lock_on_indicator_return(void) { /* no-op */ }

/* $4285 lock_on_indicator_write_cell — store `glyph` into cell $3491+idx, then enqueue a
 * sprite/object refresh event (ring id $12). */
static void lock_on_indicator_write_cell_core(uint8_t idx, uint8_t glyph) {
    lockon_write((uint16_t)(0x3491u + idx), glyph);
    ring_push_marked_core(0x12);
}
void lock_on_indicator_write_cell(void) {   /* 6502 ABI: A = glyph, Y = cell index */
    lock_on_indicator_write_cell_core(cpu.Y, cpu.A);
}

/* $4258 lock_on_indicator_fill_cells — light all six indicator glyphs ($A9) at once: the fill sweep's
 * opening frame, also used to (re)initialise the strip at game start.
 * (Misnamed "lock_on_indicator_fill_cells" — really lock_on_indicator_fill_cells; see docs/rename.md.) */
void lock_on_indicator_fill_cells(void) {
    for (uint8_t i = 0; i <= 5; i++) lockon_write((uint16_t)(0x3492u + i), 0xA9);
}

/* $4265 lock_on_indicator_step — fill-sweep tick: light one more cell.  `state` is the
 * current $007E (1..7).  Idles until the step timer underflows. */
static void lock_on_indicator_step_core(uint8_t state) {
    uint8_t t = (uint8_t)(mem[MEM_anim_step_timer] - 1u);      /* count down (signed underflow) */
    mem[MEM_anim_step_timer] = t;
    if (!(t & 0x80u)) return;                                  /* still >= 0: not time yet */
    mem[MEM_anim_step_timer] = mem[MEM_lockon_step_reload];    /* underflowed: reload delay */
    if (state == 7) {                              /* all six lit: latch "locked on" once */
        if (mem[MEM_lock_on_indicator_active] == 0) {
            mem[MEM_lock_on_indicator_active]   = 1;
            mem[MEM_lock_on_indicator_complete] = 1;
        }
        return;
    }
    mem[MEM_lock_on_indicator_state]++;                       /* advance the sweep (1..6) */
    lock_on_indicator_write_cell_core(state, 0x29);          /* light cell $3491+state */
}
void lock_on_indicator_step(void) { lock_on_indicator_step_core(cpu.A); }

/* $428E lock_on_indicator_phase_advance — reverse-sweep driver (state $81..): clear one cell
 * per phase tick, walking $007E back down toward off.  `state` is the current $007E; $0631
 * (lock_on_indicator_phase) is halved each call and only every other call does real work. */
static void lock_on_indicator_phase_advance_core(uint8_t state) {
    uint8_t phase = mem[MEM_lock_on_indicator_phase];
    mem[MEM_lock_on_indicator_phase] = (uint8_t)(phase >> 1);
    if (phase & 1) return;                                    /* skip this tick */
    mem[MEM_lock_on_indicator_phase]++;
    uint8_t n = (uint8_t)(state & 0x0Fu);
    uint8_t idx;
    if (n == 7) { mem[MEM_lock_on_indicator_state] -= 2u; idx = 6; }  /* skip the parity gap */
    else        { mem[MEM_lock_on_indicator_state] -= 1u; idx = n; }
    lock_on_indicator_write_cell_core(idx, 0xA9);            /* restore glyph $A9 */
}
void lock_on_indicator_phase_advance(void) { lock_on_indicator_phase_advance_core(cpu.A); }

/* $4229 lock_on_indicator_tick — the indicator state machine (dispatch on $007E). */
void lock_on_indicator_tick(void) {
    ROF_MEMBASE_DECL(mb);   /* fold mem[] onto a base register (see ROF_MEMBASE) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    uint8_t state = mem[MEM_lock_on_indicator_state];
    if (state >= 0x80u) {
        if (state >= 0x81u) { lock_on_indicator_phase_advance_core(state); return; }
        /* state == $80: random blink — toggle one cell's colour bit each timer tick. */
        uint8_t t = (uint8_t)(mem[MEM_anim_step_timer] - 1u);   /* count down (signed underflow) */
        mem[MEM_anim_step_timer] = t;
        if (!(t & 0x80u)) return;                               /* still >= 0: not time yet */
        uint8_t r = (uint8_t)(bus_read(0xD20A) & 7u);        /* POKEY RANDOM (LFSR on Amiga) */
        mem[MEM_anim_step_timer] = r;                        /* random inter-blink delay */
        uint8_t idx = (r >= 6u) ? (uint8_t)(r >> 1) : r;
        lockon_write((uint16_t)(0x3492u + idx), (uint8_t)(mem[0x3492u + idx] ^ 0x80u));
        return;
    }
    if (state != 0) { lock_on_indicator_step_core(state); return; }   /* fill sweep 1..7 */
    /* state == 0: one-shot init — clear the "locked" latch and start the fill sweep. */
    mem[MEM_lock_on_indicator_active] = 0;
    mem[MEM_lock_on_indicator_state]++;                      /* -> state 1 */
    mem[MEM_anim_step_timer] = mem[MEM_lockon_step_reload];
    lock_on_indicator_fill_cells();                                         /* light all six glyphs */
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* $4225 lock_on_indicator_dispatch — the flight/standby entry point.  While an event owns the
 * indicator ($0043 != 0) it stays put; otherwise it runs one tick of the animation. */
void lock_on_indicator_dispatch(void) {
    if (mem[MEM_event_active_flag] != 0) return;
    lock_on_indicator_tick();
}

/* VBI-section sub-profiling (PROBE-only): partition the $4FF5 handler into the chunks NOT
 * already covered by the integ/proj/sfx wrappers, so the headless probe can report exactly
 * which part of the handler costs what.  Same rof_subclock() unit as g_pInteg/g_pProj/g_pSfx
 * (sub-frame ticks; 313 = 1 PAL frame).  Accumulated over isrCalls firings; per-call = acc/isrCalls.
 *   g_pDrawBr  — the whole "draw" parity branch (player3 object / message flash / score)
 *   g_pSimHead — the "sim" branch head up to JOIN (obj dispatch + target routing + blink + kbd)
 *   g_pAtmo    — the atmosphere colour-ramp block
 *   g_pHud     — the 5 HUD instrument draws (AH fill / altimeter / compass / dispatch_43cb / digit)
 *   g_pScore   — the per-~30-frame BCD score fold + redraw
 *   g_pTail    — the tail (compute_indicator_pos + sfx_voice_envelope_tick; sfx also in g_pSfx)
 * (integ/proj/sfx stay measured by their own wrappers; isrLines = whole handler.) */
#ifdef ROF_FLIGHT_PROBE
extern volatile unsigned long g_pDrawBr, g_pSimHead, g_pAtmo, g_pHud, g_pScore, g_pTail;
  #define VP_T0()      unsigned long _vp = rof_subclock()
  #define VP_ACC(acc)  do { (acc) += rof_subclock() - _vp; } while (0)
#else
  #define VP_T0()      ((void)0)
  #define VP_ACC(acc)  ((void)0)
#endif

/* vbi_handler_flight @ $4FF5 — the in-flight vertical-blank handler (native twin).
 *
 * Runs once per frame while flying.  It advances the jiffy clock, then SPLITS its per-frame
 * work across two alternating frames using the lock-on parity counter ($0643): on "draw" frames
 * it updates the player-3 object / message flash / score; on "sim" frames it runs the motion
 * integrator + terrain projection + the HUD cells + score.  Both frame types finish with the
 * indicator + SFX-envelope tail.  (On the SDL build it ALSO pushes the Atari GTIA/ANTIC display
 * shadows to hardware each frame — the `#ifndef ROF_PLATFORM_AMIGA` block below; that is dead on
 * the Amiga, where the copper owns the display.)
 *
 * Faithful 1:1 re-expression of the $4FF5 6502 handler (validated byte-identical to the
 * __t6502 oracle via `make validate FN=vbi_handler_flight`).  Amiga simplifications, all
 * mem[]-neutral so they don't affect the validation:
 *   - the GTIA/ANTIC per-frame register pushes are DEAD (the copper owns the display); kept
 *     only for the SDL approximate display, under #ifndef ROF_PLATFORM_AMIGA.
 *   - the windscreen-"static" raster loop ($5278-$52b1) is DEAD (it only pokes GTIA colour
 *     registers, beam-synced; the copper has no equivalent) — SDL-only, same guard.
 * POKEY ($D2xx) writes are kept on both platforms (they drive Paula on the Amiga).
 *
 * Sub-functions (flight_control_integrate / update_terrain_scanline_proj / the HUD draws /
 * sfx_voice_envelope_tick / …) are called by name — identical in both twins, so the validation
 * isolates exactly this handler's orchestration + inline state updates.
 */
void vbi_handler_flight(void) {
    ROF_MEMBASE_DECL(mb);                     /* 178 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    mem[0x00C7] = 0x00;                       /* reset the DLI dispatch index for this frame ($00C7) */

#ifdef ROF_PLATFORM_AMIGA
    /* Cockpit status lights = four cells that game_sub_4606 inits to the OFF glyphs $B4/$B5/$B6/$B4:
     *   $3355 = Shields (#14)   $3356/$3357 = Mother Ship (#15, a 2-cell pair)   $3388 = Air Lock (#16).
     * Each toggles $34..$36 (ON — lit pixel uses COLPF2 = light salmon $2C) <-> $B4..$B6 (OFF — same
     * glyph, bit7 set → COLPF3 = dark salmon $26).  Their writers (enter/exit_terrain_special_state,
     * check_object_in_target_box, the event / game_state_update paths, the $50fa VBI pair-write) raise
     * no dirty flag, so the Amiga never re-decoded them and the lights stayed frozen.  A change-detect
     * here (every flight VBI frame) flags each cell for re-decode when it flips — robust to whichever
     * writer changed it.  All four are in the $332D-$355D mode-4 range, so platform_cockpit_dirty routes
     * through the g_ckDial registry.  (Measured 2026-07-13: shields.a8s $3355=$34 ON; airlock.a8s
     * $3388=$34 ON; both OFF = $B4 elsewhere.)  Amiga-only. */
    {
        static const uint16_t kStatusCells[4] = { 0x3355u, 0x3356u, 0x3357u, 0x3388u };
        static uint8_t s_lastStatus[4] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu };
        for (uint8_t k = 0; k < 4; k++) {
            uint8_t v = mem[kStatusCells[k]];
            if (v != s_lastStatus[k]) { s_lastStatus[k] = v; platform_cockpit_dirty(kStatusCells[k], 1u); }
        }
    }
    /* The Long Range Scanner (#13) close-range blink cells $33DF/$33E0 are NOT handled here:
     * startup_init() ($3FFA, in the joystick_saved block below) toggles their bit7 $1E/$1D<->
     * $9E/$9D every frame when the pilot range ($0642) is 1 or 2 and ($0642 & the $004B counter)
     * ==0 (bit7 swaps the mode-4 pen COLPF2 $2C <-> COLPF3 $26 — a two-speed proximity blink:
     * range 1 tests bit0 => ~2-frame period, range 2 tests bit1 => ~4-frame).  Routing them
     * through platform_cockpit_dirty would decode in the main-loop render() (throttled to the
     * ~5-6fps terrain rate) so the blink ran far too slow.  Instead they are decoded straight
     * into the cockpit bitmap at 50Hz in the VBI ISR by PlatformAmiga::flightScannerTick()
     * (called from game_vbi_isr right after this handler returns). */
#endif

#ifndef ROF_PLATFORM_AMIGA
    /* Per-frame GTIA/ANTIC shadow push — SDL display only (dead on Amiga). */
    uint8_t dp5 = display_param_5;
    bus_write(0xD008, 0x00); bus_write(0xD009, 0x00);   /* SIZEP0/SIZEP1 — P0/P1 widths (cleared) */
    bus_write(0xD409, 0x04);                            /* CHBASE */
    bus_write(0xD01A, dp5); bus_write(0xD012, dp5); bus_write(0xD013, dp5);
    bus_write(0xD014, mem[0x0037]);
    bus_write(0xD016, text_color_pf0);
    bus_write(0xD017, display_param_8);
    bus_write(0xD015, mem[0x00D9]);
    bus_write(0xD002, mem[0x00CB]);
    bus_write(0xD00A, mem[0x00CD]);
    bus_write(0xD003, player3_hpos);
    bus_write(0xD01B, mem[0x026F]);
#endif

    /* Advance the jiffy clock; on its 256-frame wrap, while an event is pending, EOR-strobe
     * the 15-entry attract palette shadow ($07E9 -> display_param_0..$0E). */
    if ((uint8_t)(++mem[MEM_RTCLOK_LOW]) == 0 &&
        (event_active_flag | clear_colors_done_003E) != 0) {
        mem[MEM_RTCLOK]++;
        if ((uint8_t)(++mem[MEM_event_pending_flag]) & 0x80) {
            event_pending_flag = 0x80;
            for (int y = 0x0E; y >= 0; y--)
                mem[MEM_display_param_0 + y] = (uint8_t)((mem[0x07E9 + y] ^ RTCLOK) & 0xF6);
        }
    }

#ifndef ROF_PLATFORM_AMIGA
    /* Wing-clearance missile HPOS (M3/M2/M1 from $2840, +$0C, +$05) — dead on Amiga (the
     * clearance bars are a bitmap there). */
    unsigned hp = mem[0x2840];
    bus_write(0xD007, (uint8_t)hp);
    hp += 0x0C; bus_write(0xD006, (uint8_t)hp);
    hp = (hp & 0xFF) + 0x05 + (hp >> 8); bus_write(0xD005, (uint8_t)hp);
    bus_write(0xD00C, 0xCC);
#endif

#ifndef ROF_PLATFORM_AMIGA
    /* Re-entrancy guard ($0005): a VBI that fires before the previous one finished is a no-op.
     * Dead on the Amiga — the VERTB interrupt cannot re-enter while this ISR is running, and
     * nothing else reads $0005 (the guard is its only reader). */
    if (zp_flag_05 != 0) { os_xitvbv(); return; }
    zp_flag_05 = 0xCC;                        /* mark "in VBI" (cleared at the normal exit) */
#endif

    /* Double-buffer flip: while actively flying and mid-swap, point ANTIC at the terrain
     * field half selected by game_phase, then clear it.  Dead on Amiga (the copper flips the
     * bitplane pointers); the field-half selection itself is faithful, so keep clearing
     * game_phase on both platforms. */
    if (joystick_saved != 0 && game_phase != 0) {
#ifndef ROF_PLATFORM_AMIGA
        if (game_phase == 0x01) { bus_write(0xD402, 0x6B); bus_write(0xD403, 0x31); }
        else                    { bus_write(0xD402, 0x10); bus_write(0xD403, 0x32); }
#endif
        game_phase = 0x00;
    }

#ifdef ROF_COMBAT_LOAD
    /* COMBAT-LOAD top-up (see the header block).  A destroyed emplacement is marked $FC and
     * aged $FD..$FF -> 0 by terrain_draw_frame_core, so over a long profiling run the map
     * would slowly empty and the load would fade.  Re-seed emptied PEAK cells (the $0900
     * bit7 = "marked" cells intro_seed_object_map targets) with the same $64 marker, using
     * the same $0623 density gate — the FORCE_EMPLACEMENTS build pins that to $FF so every
     * peak comes back.  One 16-cell slice per frame (a full sweep every 16 frames) keeps the
     * per-frame cost off the profile: 16 byte tests, not 256. */
    if (joystick_saved != 0) {
        static uint8_t s_clSlice = 0;
        uint16_t base = (uint16_t)(s_clSlice << 4);
        s_clSlice = (uint8_t)((s_clSlice + 1) & 0x0F);
        for (uint16_t i = base; i < base + 16; i++) {
            if (mem[0x0A00 + i] != 0) continue;                 /* occupied, or still aging */
            if (!(mem[0x0900 + i] & 0x80)) continue;            /* not a peak: no emplacement */
            if (bus_read(0xD20A) >= mem[0x0623]) continue;      /* same density gate as $7498 */
            mem[0x0A00 + i] = 0x64;
            CL_CNT(g_clReseed);
        }
    }
#endif

#ifdef ROF_FORCE_SAUCER
    /* PROFILING AID (Amiga, -DROF_FORCE_SAUCER): headless flight has no enemies, so the P3
     * saucer sprite path (buildViewportP3Sprite / buildScopeP3Sprite) never runs under load.
     * Inject a static flying-saucer diamond into the P3 PMG buffer ($0F00) viewport + scope
     * windows every VBI frame and park HPOSP3 / scope-X mid-screen, so the Amiga per-frame P3
     * mirror runs with a REAL object.  Hold $006A >= $80 (!= $FF) so draw_player3_object does
     * NOT run and clear our injected rows.  NOT faithful — off unless the flag is set. */
    if (joystick_saved != 0) {
        static const uint8_t kSaucer[7] = { 0x18, 0x3C, 0x7E, 0xFF, 0x7E, 0x3C, 0x18 };
        for (int i = 0; i < 7; i++) {
            mem[0x0F00 + 0x50 + i] = kSaucer[i];   /* viewport copy (buildViewportP3 scans $32..$85) */
            mem[0x0F00 + 0xA0 + i] = kSaucer[i];   /* scope copy    (buildScopeP3 scans $98..$B8) */
        }
        mem[0x2870] = 0x50;   /* HPOSP3 shadow → viewport X */
        mem[0x00CC] = 0x50;   /* scope X */
        mem[0x006A] = 0xFE;   /* bit7 set, != $FF → draw_player3_object skips its redraw/clear */
    }
#endif

    /* Alternate per-frame work via the lock-on parity counter ($00C8). */
    if ((uint8_t)(--mem[MEM_lock_on_indicator_tick_parity]) != 0) {
        /* ===== "draw" frame: lock-on flash colour, player-3 object, message, score ===== */
        VP_T0();

        /* Cycle the lock-on flash colour ($00D9) within a small range. */
        uint8_t d9 = mem[0x00D9], a = (uint8_t)(d9 >> 1), c;
        if (d9 & 1) { a = (uint8_t)(a - 1); c = (a >= 0x4B); }   /* SBC #$01 ; CMP #$4B */
        else        { a = (uint8_t)(a + 1); c = (a >= 0x4E); }   /* ADC #$01 ; CMP #$4E */
        mem[0x00D9] = (uint8_t)((a << 1) | c);                   /* ROL */

        if (object_anim_frame != 0 && event_active_flag == 0)
            build_player2_sprite_core(object_anim_frame);
        if (intro_phase_counter != 0) intro_sound_and_tick();

        if (joystick_saved != 0) {
            if (mem[0x006A] & 0x80) {                /* $006A bit7 set */
                if (mem[0x006A] == 0xFF) draw_player3_object_core(mem[0x006A]);
                update_p3_indicator_stripe();
            } else {
                if (mem[0x2845] == 0x7F) update_p3_indicator_stripe();
                draw_player3_object_core(mem[0x006A]);
            }
            mem[MEM_collision_flags]--;
            startup_init();
            if (mem[0x003A] & 0x80) {                /* update the shields-status cockpit cell */
                uint8_t v = (mem[MEM_collision_flags] & 0x08) ? 0xB6 : 0x36;
                mem[0x3357] = v;
                mem[0x3356] = (uint8_t)(v - 1);
            }
            if (vobj_path_flag != 0) vobj_update_active();
        }

        /* Message-flash timer + cockpit COLPF0. */
        if (!(msg_flash_timer & 0x80)) {
            if ((uint8_t)(--mem[MEM_msg_flash_timer]) & 0x80) {  /* just expired (0 -> $FF) */
                clear_message_buffer();
                if (level_or_state != 0) { colpf0_value = 0xEA; set_colpf0_from_flag_core(0x33); }
            }
        } else if (timer_or_counter != 0) {
            set_colpf0_from_flag_core(timer_or_counter);
        }

        if (bcd_delta_hi != 0) add_and_show_bcd_counter();
        VP_ACC(g_pDrawBr);
        /* -> tail */
    } else {
        /* ===== "sim" frame: target/lock-on logic, then the motion sim + terrain + HUD ===== */
        VP_T0();
        lock_on_indicator_dispatch();   /* native: flags platform_lockon_changed() at each cell write */
        lock_on_indicator_tick_parity = 0x02;        /* reload the parity counter */

        /* Decide where this frame joins the common chain:
         *   RESET  ($5178) — latch the target object, then continue through BLINK
         *   BLINK  ($5197) — update_blink_timer, then KEYWIN
         *   KEYWIN ($519A) — the keyboard-command window, then JOIN
         *   JOIN   ($51B2) — the motion sim / terrain / HUD (when flying)
         */
        enum { RESET, BLINK, KEYWIN, JOIN } at;
        if (level_or_state != 0) {
            if      (target_latched_idx != 0) at = RESET;
            else if (joystick_saved != 0)     at = BLINK;
            else                              at = JOIN;
        } else {
            if      (event_active_flag != 0)  at = KEYWIN;
            else if (joystick_saved != 0x02)  at = JOIN;          /* A==2 (parity) vs joystick */
            else if (bus_read(0xD010) != 0)   at = BLINK;         /* fire NOT pressed (TRIG0=$D010) */
            else if (clear_colors_done_003E == 0) at = RESET;
            else { msg_flash_timer = clear_colors_done_003E; timer_or_counter = 0x4F; at = BLINK; }
        }

        if (at == RESET) {                           /* latch the first target object */
            if (object_anim_frame == 0) {
                mem[0x286A] = 0x00; mem[0x0039] = 0x00; target_latched_idx = 0x00;
                mem[0x286B] = 0x01; object_anim_frame = 0x01;
                vobj_row_count = 0x7C; mem[0x0037] = 0x78; mem[0x286C] = 0x0C;
            }
            at = BLINK;
        }
        if (at == BLINK) { update_blink_timer_006e(); at = KEYWIN; }
        if (at == KEYWIN) {
            /* The in-flight keyboard-command window: on the Atari a POKEY keyboard/BREAK IRQ
             * fires in the 1-instruction CLI gap and leaves the keycode (or $80=BREAK) in X;
             * the Amiga delivers it via platform_flight_irq_key() (one-shot, $FF = none). */
            uint8_t k = 0xFF, kk = platform_flight_irq_key();
            if (kk != 0xFFu) k = kk;
            if (k == 0x80) { game_loop_reset_trampoline(); return; }   /* BREAK -> restart */
            if (!(k & 0x80)) {                       /* a real key */
                event_sequence_dispatcher_core(k);
                if (span_pixel_count != 0) ring_push_marked_core(span_pixel_count);
            }
            at = JOIN;
        }
        VP_ACC(g_pSimHead);
        /* JOIN ($51B2): run the motion sim + terrain + HUD only while actively flying. */
        if (joystick_saved != 0) {
            flight_control_integrate();
            update_terrain_scanline_proj();

            /* Atmosphere colour-ramp: pick an altitude band, copy its 4-entry palette set,
             * and slowly advance the fade phase ($08A1 countdown -> $08A2 -> $08A3 via $364B). */
            { VP_T0();
            if (game_state == 0 && !(event_pending_flag & 0x80)) {
                int idx;
                if (terrain_depth_step < 0x32) idx = 0;
                else { idx = (terrain_depth_step - 0x32) >> 1; if (idx > 6) idx = 6; }
                uint8_t y = (uint8_t)(idx + mem[0x08A3]);
                audc_shadow_0  = mem[0x07F9 + y];
                mem[0x00DC]    = mem[0x0823 + y];
                mem[0x00DB]    = mem[0x084D + y];
                anim_counter_2 = mem[0x0877 + y];
                if (stage_geom_0617 == 0 && (uint8_t)(--mem[0x08A1]) == 0) {
                    uint8_t p = (uint8_t)(++mem[0x08A2]);
                    mem[0x08A3] = mem[0x364B + ((p >> 2) & 0x0F)];
                }
            }
            VP_ACC(g_pAtmo); }

            /* HUD cells (the 5 instrument draws). */
            { VP_T0();
            draw_ah_ground_fill_p2();          /* misnamed: actually the artificial-horizon ground fill */
            draw_altimeter_bars();
            draw_compass_heading();
            dispatch_43cb_half_70();
            update_altitude_digit_display();
            VP_ACC(g_pHud); }

            /* Score: every ~30 frames ($00DF countdown) fold the pending delta + redraw. */
            { VP_T0();
            if ((uint8_t)(--mem[MEM_sfx_voice_distortion]) == 0) {
                if (level_cleared_flag != 0 && (uint8_t)(--mem[MEM_level_cleared_flag]) == 0) {
                    setup_level_clear_state();
                }
                if (mem[0x0070] != 0) { mem[MEM_bcd_delta_lo]++; add_and_show_bcd_counter(); }
                sfx_voice_distortion = 0x1E;
            }
            VP_ACC(g_pScore); }
        }
        /* -> tail */
    }

    /* ===== tail ($523E): indicator + SFX envelope, then the idle windscreen static ===== */
    { VP_T0();
    if (event_active_flag == 0) {
        if (indicator_pos != 0) compute_indicator_pos();
        sfx_voice_envelope_tick();
        if (game_state == 0 && alien_knock_active == 0) {
#ifndef ROF_PLATFORM_AMIGA
            /* Windscreen "static" dither — SDL display only (dead on Amiga: GTIA colour pokes,
             * beam-synced; the copper has no equivalent).  Nudge the threshold $062C toward a
             * band target, then dither COLPF0/1/COLBK per scanline from POKEY RANDOM. */
            uint8_t band = mem[0x08A2] & 0x3F;
            int upd = -1;
            if (band == 0x28) { unsigned s = mem[0x062C] + 0x0F; if (!(s & 0x100)) upd = (int)(s & 0xFF); }
            else if (band == 0x30) { int s = (int)mem[0x062C] - 0x0F; if (s >= 0) upd = s; }
            if (upd >= 0) {
                mem[0x062C] = (uint8_t)upd;
                for (;;) { uint8_t v = bus_read(0xD40B); if (!(v & 0x80) && v >= 0x19) break; }
                bus_write(0xD20F, 0xFB);
                do {
                    uint8_t r = bus_read(0xD20A), col;
                    if (r >= mem[0x062C]) { bus_write(0xD016, 0x20); col = 0xF0; }
                    else                  { col = 0x00; bus_write(0xD016, 0x00); }
                    bus_write(0xD017, col); bus_write(0xD01A, col);
                    while (bus_read(0xD20A) & 0x80) { }
                } while (bus_read(0xD40B) < 0x46);
                bus_write(0xD20F, 0x03);
            }
#endif
        }
    }
    VP_ACC(g_pTail); }

#ifndef ROF_PLATFORM_AMIGA
    zp_flag_05 = 0x00;                        /* clear the re-entrancy guard (SDL only; see above) */
#endif
    os_xitvbv();
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* wait_frames_core — busy-wait `frames` vertical-blank periods, driving one real
 * Amiga frame (VBI tick + render) per iteration so the display animates and the
 * async $4FF5 flight ISR keeps advancing RTCLOK while we wait.
 *
 * Faithful to the Atari's wait_timer_4c_frames ($3CB2): zero the RTCLOK low byte, then
 * count up to `frames`.  The 6502 loops on an EXACT-equality test (CMP $14 / BNE),
 * safe on hardware because the CPU polls $14 thousands of times per frame so it
 * never skips the target value.  On the Amiga RTCLOK is bumped asynchronously by
 * the flight ISR and a single driven frame can span several real VBIs, so $14 can
 * step OVER the target — an exact-equality test would then stall a full 256-tick
 * lap waiting for the value to come round again.  We instead exit as soon as the
 * target is reached OR passed: `remaining` (target − RTCLOK, mod 256) is 1..127
 * while the target is still ahead, 0 exactly on the tick, and >=128 once passed. */
static void wait_frames_core(uint8_t frames) {
    RTCLOK_LOW = 0;
    for (;;) {
        uint8_t remaining = (uint8_t)(frames - RTCLOK_LOW);
        if (remaining == 0 || remaining >= 0x80)
            break;                       /* target reached (0) or just passed (>=128) */
        platform_tick_vbi();
        platform_render_frame();
    }
}

/* wait_timer_4c_frames @ $3CB2 — wait the caller-set frame count in frame_wait_count ($4C) of
 * vertical-blank periods.  Every caller entry ($3CB1 wait_frames, $3CBE,
 * $3CC3, $3CCA..$3CD9) PHA's the accumulator before routing here; the closing PLA
 * restores it, so the accumulator survives the wait.  (No hardware writes: it only
 * touches the RTCLOK/timer zero-page cells, so nothing to shed on the Amiga.) */
void wait_timer_4c_frames(void) {
    wait_frames_core(frame_wait_count);
    PLA();                               /* restore the accumulator each caller PHA'd */
}

/* wait_frames @ $3CB1 — preserve the accumulator across a frame_wait_count-frame
 * wait: PHA, then fall into wait_timer_4c_frames (which PLA's it back on exit). */
void wait_frames(void) {
    PHA();
    wait_timer_4c_frames();
}

/* init_gameplay_state @ $73C8 — per-game/level gameplay init (run ONCE from
 * game_main_loop).  Seeds the heading/object/timer arrays + lives, draws the
 * compass, unpacks the cockpit bitmap, seeds the cockpit bar cells, plots the
 * initial horizon spans, RANDOM-seeds one flag, and tail-calls cockpit_dial_update.
 *
 * Every leaf it calls is already native; this twin just sheds the $73C8 body.
 * NOT validated by `make validate`: like the apex it calls wait_frames
 * (the wait_timer_4c_frames spin-pacer — a deliberate ~$1E-frame delay driven by the
 * SPINWAIT hook), which would hang the equivalence harness.  Verified on FS-UAE.
 * Faithful 1:1 with the $73C8 disasm; mem[$004C] (set once to $1E here, never
 * re-written) is the wait target for all four waits, and A round-trips PHA/PLA. */
void init_gameplay_state(void) {
    ROF_MEMBASE_DECL(mb);   /* 94 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    terrain_index = 0x0E;                                /* 73c8 */
    heading_hi = 0x02;                                /* 73cf */
    lock_on_indicator_tick_parity = 0x02;                                /* 73d2 */
    for (uint8_t y = 0x0E; y != 0; y--) {              /* 73d5 Y=$0E..1 (Y=0 not stored) */
        mem[0x0B4C + y] = 0x20;
        mem[0x0B63 + y] = 0x20;
    }
    mem[0x0B5F] = 0xCC;                                /* 73e4 */
    mem[0x006A] = 0xCC;                                /* 73e7 */
    throttle_accum_hi = 0x4D;                                /* 73eb */
    terrain_depth_step = 0x4F;                                /* 73ef */
    sfx_voice_distortion = 0x1E;                                /* 73f3 */
    frame_wait_count = 0x1E;                                /* 73f6 — wait_timer_4c_frames target (all 4 waits) */
    cpu.A = 0x1E; wait_frames();                 /* 73f8 — A preserved (PHA/PLA) */
    for (int y = 0x1E; y >= 0; y--)                    /* 73fb TAY (Y=A=$1E); 73fe Y=$1E..0 */
        mem[0x0E94 + y] = 0xFF;
    for (int y = 0x5F; y >= 0; y--)                    /* 7406 Y=$5F..0 */
        mem[0x2210 + y] = 0xFF;
    object_index_signed = 0xFF;                                /* 740c */
    player3_dither_flag = 0xFF;                                /* 740e */
    flight_mode_state = 0xFF;                                /* 7411 */
    mem[0x0079] = 0xFF;                                /* 7413 */
    for (int y = 0x37; y >= 0; y--)                    /* 7419 Y=$37..0 */
        mem[0x0B98 + y] = 0xC0;
    mem[0x004D] = 0xC0;                                /* 741f */
    cpu.A = 0xC0; wait_frames();                 /* 7421 */
    draw_compass_heading();                            /* 7424 */
    unpack_bitmap_4d3e();                              /* 7427 */
    wait_frames();                               /* 742a */
    init_cockpit_bar_cells();                          /* 742d */
    wait_frames();                               /* 7430 */
    cpu.A = 0x00; cpu.X = 0x00; cpu.Y = 0x00; game_sub_451d();   /* 7433-7437 */
    cpu.A = 0x38; cpu.X = 0x04; cpu.Y = 0x10; game_sub_451d();   /* 743a-7440 */
    wait_frames();                               /* 7443 */
    roll_pos_hi = 0xF4;                                /* 7446 */
    draw_pattern_byte = 0x91;                                /* 744a */
    obj_pos_hi = 0x0B;                                /* 744e */
    if ((uint8_t)bus_read(0xD20A) < 0x1F)              /* 7452 CMP#$1F; BCS skip */
        mem[0x003A] = (uint8_t)(mem[0x003A] + 1);      /* 7459 INC $3A (when A < $1F) */
    cockpit_dial_update_core(0x07);                    /* 745b-745d tail call */
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* boot_standby_launch_driver @ $5F1D — the orchestration APEX: main game display setup + the
 * Standby/attract idle loop + the launch (doors/tunnel/stars/planet) cinematic
 * driver.  game_main_loop ($3D48) calls it at the top of every game pass; it RTSes
 * back into game_main_loop's flight setup once START (or the demo timeout) fires.
 *
 * NOT validated by `make validate`: the body busy-waits on VBI-updated state
 * ($0655/$0089/$0014/$067E/$0684/$0686), the attract timer and live HW input,
 * which would hang the equivalence harness.  Correctness is verified on FS-UAE by
 * behaviour, exactly like RescueOnFractalus::run().  This is a FAITHFUL
 * transcription of the $5F1D transliteration: cpu/mem state is preserved bit-for-
 * bit so the (still-transpiled) leaf callees and the wait_frames_NN / wait_frames_1 /
 * wait_frames frame primitives behave identically.  Each original spin-wait
 * SPINWAIT-hook point becomes one real Amiga frame via ds_frame() (the same
 * platform_tick_vbi + platform_render_frame the transpiler injected); the PHA-based
 * frame primitives drive frames internally and are called as-is.
 *
 * The only deviation from a verbatim copy is fidelity: where the 6502 consumes the
 * exit A of init_row_coords_9c (a pure-mem native leaf that does not set cpu.A),
 * the faithful exit value A=$13 is loaded explicitly before it is stored to $00B9. */
static inline void ds_frame(void) { platform_tick_vbi(); platform_render_frame(); }

#ifdef ROF_PLATFORM_AMIGA
/* Tunnel-ring pre-build (user-directed 2026-07-03): the launch-time draw_frame_pattern_seq
 * (the L_635f site below) plots ~5900 pixels into mem[$1000] on a no-render stretch = the
 * standby->doors freeze.  Build the identical ring field HERE instead, off the launch path,
 * and snapshot the ZP scratch the draw leaves; at the launch site we replay that scratch
 * instead of re-plotting.  The geometry is deterministic (init_row_coords_9c constants + the
 * $6E0F table + a fixed colour cycle, no runtime input), so it is byte-identical to the launch
 * draw; mem[$1000] is untouched between the two points (measured), so the pre-built field
 * survives.  Construction ZP + the resting $2000 addr table are restored around the build so
 * the rest of construction is unaffected.
 *
 * ⚠ THE HACK IS NOT DELETABLE, and direct bitplane painting did not make it so (measured
 * 2026-08-10, PROBES=1 with the prebuild forced off): the cost it dodges is the 6502's OWN
 * mem[$1000] plot — SA_TIMED bucket `framepat` = 1378 beam ticks = 86 ms — not the decode that
 * painting removed.  Forcing the launch-site draw took the standby->doors render gap from 1
 * frame to 5 (~100 ms).  So keep it, and keep the Amiga painter's claim/prime paired with it.
 *
 * ⚠ WHERE IT IS CALLED FROM MATTERS.  Because the rings are now PAINTED into tunnelBitmap
 * synchronously (no deferred decode to postpone), this must not run while that bitmap is on
 * screen.  On the boost return it would be: the T6 handoff hold is still displaying the last
 * reverse-ring frame for ~13 frames after the ring ends.  The call site is therefore AFTER
 * delay_loop_c2_to_c9() — past the standby reveal, so staticStandby has taken the display and
 * tunnelBitmap is free.  (The old site, before g_doorFieldReady, was fine only because the
 * decode it flagged could be deferred a frame; the paint cannot.) */
volatile unsigned char g_tunnelPrebuilt = 0;
static struct { uint8_t s80,s81,s84,s92,s94,s96,s9c,s9d,s9e,s9f,sa0,sb7,sb8,sb9,sdf; }
    g_tunnelPrebuildExit;

static void tunnel_prebuild_rings(void) {
    uint8_t zp[0x80];
    for (int i = 0; i < 0x80; i++) zp[i] = mem[0x80 + i];   /* protect construction ZP */
    build_line_addr_table_1000();      /* row-addr table for base $1000 (the draw needs it) */
    platform_tunnel_rings_begin();     /* Amiga: claim + prime the ring bitmap for the FORWARD LUT */
    ROF_TR_PRESITE(4);                 /* tag the rectangles as coming from THIS site, not L_6047 */
    draw_frame_pattern_seq();          /* plot the rings into $1000 (and paint them, span by span) */
    ROF_TR_PRESITE(1);
    /* snapshot the draw's exit ZP scratch (its write-set) for the launch-site replay */
    g_tunnelPrebuildExit.s80 = sync_flag;        g_tunnelPrebuildExit.s81 = dl_ptr_lo;
    g_tunnelPrebuildExit.s84 = screen_ptr_hi;    g_tunnelPrebuildExit.s92 = draw_row;
    g_tunnelPrebuildExit.s94 = draw_color_idx;   g_tunnelPrebuildExit.s96 = span_row_count;
    g_tunnelPrebuildExit.s9c = draw_x_left;      g_tunnelPrebuildExit.s9d = draw_x_right;
    g_tunnelPrebuildExit.s9e = draw_row_top;     g_tunnelPrebuildExit.s9f = draw_row_bottom;
    g_tunnelPrebuildExit.sa0 = draw_iter_count;  g_tunnelPrebuildExit.sb7 = frame_counter;
    g_tunnelPrebuildExit.sb8 = draw_row_ptr2_hi; g_tunnelPrebuildExit.sb9 = draw_pattern_byte;
    g_tunnelPrebuildExit.sdf = span_pixel_count;
    for (int i = 0; i < 0x80; i++) mem[0x80 + i] = zp[i];   /* undo all ZP changes */
    build_line_addr_table_2000();      /* re-establish the resting $2000 addr table + regs */
    g_tunnelPrebuilt = 1;
}

static void tunnel_prebuild_replay_exit(void) {
    sync_flag = g_tunnelPrebuildExit.s80;        dl_ptr_lo = g_tunnelPrebuildExit.s81;
    screen_ptr_hi = g_tunnelPrebuildExit.s84;    draw_row = g_tunnelPrebuildExit.s92;
    draw_color_idx = g_tunnelPrebuildExit.s94;   span_row_count = g_tunnelPrebuildExit.s96;
    draw_x_left = g_tunnelPrebuildExit.s9c;      draw_x_right = g_tunnelPrebuildExit.s9d;
    draw_row_top = g_tunnelPrebuildExit.s9e;     draw_row_bottom = g_tunnelPrebuildExit.s9f;
    draw_iter_count = g_tunnelPrebuildExit.sa0;  frame_counter = g_tunnelPrebuildExit.sb7;
    draw_row_ptr2_hi = g_tunnelPrebuildExit.sb8; draw_pattern_byte = g_tunnelPrebuildExit.sb9;
    span_pixel_count = g_tunnelPrebuildExit.sdf;
}
#endif

/* standby_scoreboard_render @ $587B — render the Standby / Title-Screen scoreboard, then hand off to the
 * idle input loop.  This is the apex of the Standby scene: the "cockpit" is a static template
 * blitted into screen RAM overlaid with live score / high-score / level / initials digits, and
 * input is polled by the loop this tail-calls.  NOT in `make validate` — it tail-calls the
 * live-input standby_level_select_loop / sound_retrigger_random loops and busy-waits on the
 * $00E5 latch, which would hang the headless harness; verified on FS-UAE by behaviour.
 *
 * Game-state block at $0600:  $0600-$0603 = current score (4 bytes, most-significant first),
 * $0604 = level, $0605-$0608 = high score, $060A = a secondary counter.  Digits are rendered into
 * Title-Screen RAM: $36A8 (counter), $36B7 (current score), $36CB (high score), $37F5 (level).
 *
 * cpu.A/cpu.Y are set only at the callee boundaries whose 6502 entry actually reads them
 * (bin_to_bcd / render_bcd_* / emit_bcd_byte_digits consume A; music_init_state consumes Y). */
void standby_scoreboard_render(void) {
    ROF_MEMBASE_DECL(mb);   /* 99 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* Blit the 120-byte Standby template ($5A9F..$5B16) into Title-Screen RAM ($365B..$36D2).
     * (The 6502 copies offsets $78..$01 downward; offset 0 is deliberately left untouched.) */
    for (int y = 0x78; y >= 1; y--)
        mem[0x365A + y] = mem[0x5A9E + y];
    zp_flag_03   = 0x00;    /* $03 = 0 (the copy loop leaves Y at 0) */
    joystick_raw = 0xFF;    /* $49 = neutral joystick latch */

    int skip_hiscore = 0;
    uint8_t e5 = mem[0x00E5];         /* game-active / results latch (unnamed $00E5) */
    if (e5 != 0) {
        /* Results / level-start entry: stash the latch, pause ~60 frames, (re)load the tune, and
         * skip the high-score panel while a level is actually in progress ($0004 != 0). */
        game_var_37F4 = e5;
        cpu.A = 0xFF;     /* wait_frames_save_a PHAs the caller's A (still $FF from above) */
        cpu.Y = e5;       /* survives into music_init_state, which indexes its song header by Y */
        wait_frames_save_a();
        music_init_state();
        skip_hiscore = (level_or_state != 0);
    } else if (game_var_E4 == 0) {
        /* Plain standby (not mid name-entry): restore the display and clear the banner threshold. */
        restore_display_if_E7();
        altitude_threshold = 0x00;
    }

    if (!skip_hiscore) {
        /* High-score update: if the current score ($0600..$0603) beats the stored high score
         * ($0605..$0608), copy it over.  Unsigned big-endian compare from the top byte down;
         * the first differing byte decides, so break either way once one is found. */
        for (int y = 0; y < 4; y++) {
            uint8_t cur = mem[0x0600 + y], hi = mem[0x0605 + y];
            if (cur == hi) continue;
            if (cur > hi)
                for (int j = 3; j >= 0; j--) mem[0x0605 + j] = mem[0x0600 + j];
            break;
        }

        /* Render the secondary counter ($060A -> $36A8) and the high score ($0605..$0608 -> $36CB). */
        digit_dst_ptr_lo = 0xA8; digit_dst_ptr_hi = 0x36;
        cpu.A = mem[0x060A]; bin_to_bcd(); render_bcd_digits_supp_all();
        digit_dst_ptr_lo = 0xCB; digit_dst_ptr_hi = 0x36;
        cpu.A = mem[0x0605]; render_bcd_digits_supp_all();   /* high score already BCD */
        cpu.A = mem[0x0606]; emit_bcd_byte_digits();
        cpu.A = mem[0x0607]; emit_bcd_byte_digits();
        cpu.A = mem[0x0608]; emit_bcd_byte_digits();
        setup_initials_ptr();
    }

    /* Render the current score ($0600..$0603 -> $36B7). */
    digit_dst_ptr_lo = 0xB7; digit_dst_ptr_hi = 0x36;
    cpu.A = mem[0x0600]; render_bcd_digits_supp_all();
    cpu.A = score_display; emit_bcd_byte_digits();   /* $0601 */
    render_bcd_low_bytes();                           /* $0602, $0603 */

    /* Render the level ($0604 -> $37F5), or blank both digit bytes when the level is 0. */
    digit_dst_ptr_lo = 0xF5; digit_dst_ptr_hi = 0x37;
    if (level_count_acc != 0) {
        cpu.A = level_count_acc; bin_to_bcd(); render_bcd_digits_supp_all();
    } else {
        mem[0x37F5] = 0x00; mem[0x37F6] = 0x00;
    }

    /* If the tune player is idle, zero the current-score record ($0600..$0604). */
    if (sound_active_flag == 0)
        for (int y = 4; y >= 0; y--) mem[0x0600 + y] = 0x00;

    row_table_base_lo = 0x04;    /* $C3 = 4: the repeat counter standby_level_select_loop decrements */

    if (game_var_E4 != 0) {
        if (mem[0x36BD] != 0) {
            name_entry_loop();          /* mid initials-entry: keep polling it, refresh the attract timer */
            attract_timer = 0x64;
        } else {
            game_var_E4 = 0x00;         /* entry finished: leave name-entry mode + restore the display */
            restore_display_if_E7();
        }
    }

    display_list_init();
    bus_write(0x022F, 0x22);   /* SDMCTL = normal playfield+PMG DMA.  LIVE on the Amiga too: the
                                * empty-copper guard in RescueOnFractalus.cpp reads mem[$022F]. */

    if (level_or_state != 0) { sound_retrigger_random(); return; }   /* level in progress: re-seed SFX */

    /* Standby idle: if the game-active latch is already clear, enter the level-select loop directly;
     * otherwise wait for it to clear (frame-driven) then run one initials-entry pass first. */
    if (mem[0x00E5] == 0) { standby_level_select_loop(); return; }
    do { platform_tick_vbi(); platform_render_frame(); } while (mem[0x00E5] != 0);
    name_entry_loop();
    attract_timer = 0x64;
    standby_level_select_loop();
#ifdef ROF_MEMBASE
#undef mem
#endif
}

void boot_standby_launch_driver(void) {
    ROF_MEMBASE_DECL(mb);   /* 956 absolute-long mem[] operands folded to (d16,An) */
#ifdef ROF_MEMBASE
#define mem mb
#endif
    /* 5f1d */
    if (!plot2bpp_lut_ready) build_plot2bpp_lut();   /* eager: keep the LUT build off the planet hot path */
    /* Amiga: arm the one-shot door-field ($2000) decode for THIS Standby build.  The renderer
       decodes the door "LEVEL NN" bitmap once on the g_doorFieldReady 0→1 edge (set at L_6118,
       right after blit_message_block/blit_numeric_readout redraw $2000 with the selected
       level_stage $6D).  It is otherwise latched at 1 forever, so a fast re-entry from the
       Title/level-select (which rebuilds $2000 with a new level but never leaves to a launched/
       viewport scene) would never re-decode → doors stuck on the first-boot level.  Clearing it
       at every boot_standby_launch_driver entry restores the edge, so each fresh Standby build re-decodes the
       doors exactly once with the current level.  (No-op on SDL, which never reads it.) */
    g_doorFieldReady = 0;
    /* g_standbyRevealReady: RESET to 0 at boot_standby_launch_driver ENTRY so the screen stays black
       through the whole ~30-frame paced construction below (it is set back to 1 at the construction-
       done point, $6118 region, alongside g_doorFieldReady).  It is a latch, so a re-entry (post-
       crash / post-BREAK restart / START-from-the-$53CC-card) would otherwise arrive with it already
       1 and the half-built standby — including a STALE, not-yet-decoded viewportBitmap under the
       doors copper — would show for the ~30 build frames (the doors' TOP HALF black until the
       g_doorFieldReady 0→1 edge re-decodes it: bug 3).  On first boot it starts 0 so this is a no-op;
       resetting it makes EVERY fresh Standby build masked, matching first-boot.  Must NOT be set to 1
       here (that reveals the half-built screen + forces a full render() per construction frame).
       (No-op on SDL, which never reads it.) */
    g_standbyRevealReady = 0;
    bus_write(0x02C7, 0x06);
    build_line_addr_table_2000();
    dl_lms_reset_window();
    frame_counter = 0;
    mem[0x08A2] = 0;
    mem[0x062C] = 0;
    for (int i = 0; i < 0x0A; i++)        /* L_5f34 */
        mem[0x0088 + i] = 0;
    row_table_stride = 0;
    player_speed = 0x10;
    row_table_base_lo = 0x0F;
    row_table_base_hi = 0x08;
    memset_or_copy();
    wait_vcount_30();
    bus_write(0x0222, 0xD7);
    bus_write(0x0223, 0x52);
    wait_vcount_ge_7a();
    bus_write(0x0200, 0xC2);             /* VDSLST shadow (stars DLI) */
    bus_write(0x0201, 0x6C);
    HW_WRITE(0xD402, 0x20);              /* DLISTL/H: stars display list $3120 */
    HW_WRITE(0xD403, 0x31);
    dl_y1 = 0x59;
    dl_y2 = 0x6E;
    dl_y3 = 0x31;
    dl_y4 = 0x0B;
    cpu.A = 0x0B;                         /* rle_expand_list takes its count in A */
    rle_expand_list();
    bus_write(0x026F, 0x14);             /* GPRIOR shadow */
    HW_WRITE(0xD01B, 0x14);              /* PRIOR */
    for (int i = 0; i <= 0x56; i++) {     /* clear the canopy-post player buffers $0C31/$0D31 */
        mem[0x0C31 + i] = 0;
        mem[0x0D31 + i] = 0;
    }
    HW_WRITE(0xD00C, 0);                 /* SIZEM */
    for (int i = 0; i <= 3; i++)          /* SIZEP0-3 (shadowed in mem[$D008+]) */
        mem[0xD008 + i] = 3;
    HW_WRITE(0xD000, 0x2D);              /* HPOSP0 */
    HW_WRITE(0xD002, 0x2D);              /* HPOSP2 */
    mem[0x00B5] = 0xBE;
    HW_WRITE(0xD003, 0xBE);              /* HPOSP3 */
    wait_frames_1();
    HW_WRITE(0xD40E, 0xC0);             /* NMIEN */
    bus_write(0x022F, 0x3F);            /* SDMCTL shadow */
    HW_WRITE(0xD01D, 0x03);             /* GRACTL */
    bus_write(0x02C0, 0x00);            /* PCOLR0 shadow */
    bus_write(0x02C8, 0x06);           /* COLOR4/border shadow */
    frame_counter = 0x88;
    fill_region_2000();
    wait_vcount_ge_7a();
    bus_write(0x0200, 0xAD);             /* VDSLST shadow (launch/tunnel DLI) */
    bus_write(0x0201, 0x6C);
    HW_WRITE(0xD402, 0x00);              /* DLISTL/H: launch display list $3000 */
    HW_WRITE(0xD403, 0x30);
    fill_four_bufs_ff();
    if (sound_active_flag == 0) goto L_6118;
    clear_message_buffer();
    for (int i = 0; i <= 4; i++)          /* copy palette seed $6B80..$6B84 -> $A1..$A5 */
        mem[0x00A1 + i] = mem[0x6B80 + i];
    cpu.A = anim_counter_2;               /* fill_buf_08d4 takes its fill value in A */
    fill_buf_08d4();
    game_sub_6811();
    if (anim_counter_2 != 0) {            /* ramp display_flags from anim_counter_2 down to $1F */
        uint8_t x = anim_counter_2;
        do {
            display_flags = x;
            wait_frames_2();
        } while (--x != 0x1F);
    }
    display_flags = 0;
    {
        uint8_t idx = anim_counter_2 & 0x0F;   /* colour index, floored at 2 */
        if (idx == 0) idx = 0x02;
        do {                                   /* fill $08D4 + clear colours, idx..$0E */
            cpu.A = idx;                        /* fill_buf_08d4 takes its fill value in A */
            fill_buf_08d4();
            wait_frames_1();
        } while (++idx != 0x0F);
    }
    build_line_addr_table_1000();
    if (alien_trigger != 0) {             /* one-shot: clear the alien trigger + set colour $C8 */
        alien_trigger = 0;
        cpu.Y = 0x00;
        cpu.A = 0xC8;
        save_color_clear_y_bit5();        /* takes A + Y */
    }
    draw_frame_pattern_seq();             /* L_6047; reloads its own Y ($00A0) */
    init_row_coords_9c();
    draw_pattern_byte = 0x13;             /* init_row_coords_9c leaves cpu.A; faithful exit A=$13 */
    draw_color_idx = 0x08;
    do {                                  /* plot terrain spans for rows $13..$00 */
        cpu.Y = draw_pattern_byte;        /* plot_terrain_span takes its row in Y */
        plot_terrain_span();
    } while (!(--draw_pattern_byte & 0x80));
    cpu.A = 0x07;                         /* draw_cockpit_dial_bar takes its value in A */
    draw_cockpit_dial_bar();
    for (uint8_t y = 0; y != 0x06; y++) {          /* animate $08D4[y] down $0E..$00, one frame each */
        uint8_t x = 0x0E;
        do {
            mem[0x08D4 + y] = x;
            cpu.A = x;                    /* wait_frames takes its value in A */
            wait_frames();
        } while (!(--x & 0x80));
    }
    fill_region_2000();
    cpu.A = 0x03;                         /* A=3 feeds wait_frames throughout the loop */
    frame_wait_count = 0x03;
    mem[0x27A3] = 0x03;
    for (uint8_t y = 0x90; y != 0x9B; y++) {       /* sweep $08D9 = $90..$9A, one frame each */
        mem[0x08D9] = y;
        wait_frames();
    }
    init_terrain_dl();
    for (int i = 0; i <= 5; i++)          /* L_6090 */
        mem[0x08D4 + i] = mem[0x6E68 + i];
    step_mode_flag = 1;
    draw_color_idx = 1;
    history_ring_step = 1;
    init_row_coords_9c();
    draw_pattern_byte = 0x13;             /* faithful init_row_coords_9c exit A=$13 */
    row_table_base_lo = 0x7E;
    row_table_base_hi = 0x2A;
    player_speed = 0x2A;
    cpu.A = 0x2A;                         /* clear_message_buffer preserves A -> row_table_stride */
    clear_message_buffer();
    row_table_stride = 0x2A;
    do {                                  /* emit DL coord pairs for rows $13..$00, VBI-paced */
        cpu.Y = draw_pattern_byte;        /* emit_dl_coord_pairs takes its row in Y */
        if (draw_pattern_byte == 0) {     /* last row: latch full-step mode + its colour */
            step_mode_flag = 0xFF;
            display_flags = mem[0x08D8];
        }
        while (mem[0x008E] == 0) ds_frame();   /* wait for the VBI to arm the next row */
        mem[0x008E] = 0;
        emit_dl_coord_pairs();
        if (++draw_color_idx == 0x07) draw_color_idx = 1;
        draw_pattern_byte--;
    } while (!(draw_pattern_byte & 0x80));
    step_mode_flag = 1;
    init_row_coords_9c();
    draw_color_idx = 0x08;
    for (int i = 1; i <= 8; i++) {         /* clear canopy-wedge buffers $0C88/$0D88[1..8] (offset 0 skipped) */
        mem[0x0C87 + i] = 0;
        mem[0x0D87 + i] = 0;
    }
    display_flags = 0xC0;
    /* spin until the VBI clears step_mode_flag ($008D) */
    while (step_mode_flag != 0) ds_frame();
    draw_frame_guide_columns();
    for (uint8_t y = 0; y != 0x08; y++) {  /* clear colours + fill wedge buffers $0C88/$0D88[0..7] = $FF */
        wait_frames_1();
        mem[0x0C88 + y] = 0xFF;
        mem[0x0D88 + y] = 0xFF;
    }
    cpu.A = 0x08;                         /* Y=8 at loop exit; draw_cockpit_dial_bar takes it in A */
    draw_cockpit_dial_bar();
    build_line_addr_table_2000();
L_6118:
    sfx_engine_reset();
    wait_frames_5();
    blit_color_src = 0x07;
    frame_counter = 0x88;
    fill_region_2000();
    blit_message_block();
    blit_numeric_readout();
    g_doorFieldReady = 1;   /* Amiga: doors/LEVEL drawn into $2000 — decode now, before the green fade */
    /* Standby construction complete (cockpit/top-bar/doors/LEVEL all drawn) — reveal now.  The
       black EmptyCopperList held the screen (and skipped all rendering) through the build above;
       the first render() after this decodes the finished bitmaps in one frame (the initial
       terrainDirty/cockpitForceFull/g_titleToRender signals are still set — render never ran). */
    g_standbyRevealReady = 1;
    dl_lms_reset_window();
    delay_loop_c2_to_c9();
#ifdef ROF_PLATFORM_AMIGA
    /* Build the tunnel rings into $1000 (and paint them into tunnelBitmap) off the launch hot
       path — this is the standby->doors freeze fix.  It sits AFTER delay_loop_c2_to_c9 because
       that loop renders frames: by here the standby reveal has happened and staticStandby owns
       the display, so on the BOOST return the T6 hold has let go of tunnelBitmap and the paint
       cannot be seen.  $1000 is untouched from here to the launch site. */
    SA_TIMED(10, tunnel_prebuild_rings());   /* `rings` bucket: the 6502 plot + the direct paint */
#endif
    mem[0x00E3] = 0xFF;
    /* ---- L_6141..L_634a: standby/launch dispatch + attract/rebuild event loop ----
       This region is IRREDUCIBLE control flow (overlapping back-edges L_6347->L_622d and
       L_62a9->L_6268, plus the attract loop L_62f6<->L_634a with three exits: game_main_loop,
       rebuild, and launch).  Faithfully structuring it would need state-flag emulation that is
       less clear than the gotos, so the goto form is kept verbatim; only the straight-line
       LD#imm;store idioms within each basic block are folded. */
    if (cockpit_flag != 0) goto L_6141;
    goto L_62eb;
L_6141:
    if (sound_active_flag != 0) goto L_6153;
    if (sound_event_flag == 0) goto L_614d;
    goto L_6332;
L_614d:
    wait_frames_save_a();
    goto L_634f;
L_6153:
    if (osc_step_counter == 0) goto L_61f8;
L_615b:
    /* $E3 = osc_step_counter - stage_geom_height_062A (single subtract) */
    mem[0x00E3] = (uint8_t)(osc_step_counter - stage_geom_height_062A);
    if (!(mem[0x00E3] & 0x80)) goto L_6174;
    /* osc < height: shrink height by osc, carry A=osc / Y=old-height into the join */
    cpu.Y = stage_geom_height_062A;
    stage_geom_height_062A = (uint8_t)(stage_geom_height_062A - osc_step_counter);
    cpu.A = osc_step_counter;
    goto L_6178;
L_6174:
    cpu.A = stage_geom_height_062A;
    cpu.Y = cpu.A;
L_6178:
    span_row_count = cpu.A;
    cpu.A = cpu.Y;                        /* set_0628_bcd_redisplay takes A */
    set_0628_bcd_redisplay();
    cpu.Y = 0x0C;
    cpu.A = 0x78;
    save_color_clear_y_bit5();           /* takes A + Y */
L_6185:
    bcd_delta_lo = 0;
    bcd_delta_hi = 0x05;
    decrement_bcd_0628_restart();
    if (--span_row_count != 0) goto L_6185;
    if (mem[0x00E3] & 0x80) goto L_61f2;
    if (mem[0x00E3] == 0)   goto L_61b7;
    wait_frames_save_a();
    set_0628_bcd_redisplay();
    text_color_pf0 = 0x7C;
L_61a4:
    bcd_delta_hi = 0x10;
    cpu.X = 0x10;                         /* sfx_event_load takes X */
    sfx_event_load();
    wait_frames_5();
    decrement_bcd_0628_restart();
    if (--mem[0x00E3] != 0) goto L_61a4;
L_61b7:
    wait_frames_save_a();
    cpu.A = 0x2A;
    cpu.Y = 0x83;
    save_color_clear_y_bit5();           /* takes A + Y */
    cpu.Y = 0x0B;                        /* music_init_state takes Y */
    music_init_state();
L_61c6:
    /* spin: wait for the music/VBI to clear $0655 (one frame/iter) */
    ds_frame();
    if (mem[0x0655] != 0) goto L_61c6;
    row_table_stride = level_stage;
    if (level_stage < mem[0x060A]) goto L_61d7;
    mem[0x060A] = level_stage;           /* raise the recorded max level */
L_61d7:
    count_up_to_level();
    if (fresh_start_flag != 0) goto L_61f5;
L_61df:
    span_row_count = 0x02;
L_61e3:
    bcd_delta_hi = row_table_base_lo;
    reinit_and_redraw_via_delay();
    if (--span_row_count != 0) goto L_61e3;
    if (--row_table_stride != 0) goto L_61df;   /* $00C1 outer counter */
L_61f2:
    wait_frames_save_a();
L_61f5:
    clear_message_buffer();
L_61f8:
    if (!(mem[0x00E3] & 0x80)) goto L_6219;
    if (level_stage >= 0x05) goto L_620e;
    grid_offset_b = 0x01;
    mem[0x0609] = 0x04;
    goto L_6216;
L_620e:
    mem[0x0609] = level_stage;
    grid_offset_b = (uint8_t)(level_stage - 0x03);
L_6216:
    goto L_62e7;
L_6219:
    grid_offset_b = (uint8_t)(level_stage + 1);   /* STA $9B; INC $9B */
    {
        uint8_t a = (uint8_t)(level_stage + 0x04);  /* clamp level+4 to $63 */
        if (a >= 0x64) a = 0x63;
        mem[0x0609] = a;
        sfx_reinit_gate = a;
    }
L_622d:
    cpu.Y = 0x00;                         /* Y feeds the L_6244 loop's mem[$6595+Y] */
    frame_counter = 0;
    draw_pattern_byte = 0x4F;
    bus_write(0xD203, 0x44);              /* POKEY AUDCTL — live on Amiga (Paula) */
    audf2_sweep_val = 0x52;
    audf2_sweep_step = 0x01;
L_6244:
    cpu.Y = frame_counter;                /* $6595,Y and $8B fed to dl_lms_scroll_step */
    frame_wait_count = mem[0x6595 + frame_counter];
    audf2_sweep_clear_colors();
    dl_src_index = draw_pattern_byte;
    dl_lms_scroll_step();
    dl_src_index = 0;
    draw_pattern_byte--;                  /* $B9 */
    if (++frame_counter != 0x13) goto L_6244;   /* $B7 */
    wait_frames();
    dl_src_index = 0x53;
L_6268:
    cpu.X = 0x05;
L_626a:
    if (dl_src_index != 0x3E) { cpu.Z = 0; goto L_6289; }   /* spin: wait for the VBI to reach $3E */
    dl_src_index = 0x56;
    if (cpu.X != 0x03) goto L_6288;
    blit_color_src = 0x08;               /* on the 3rd pass: re-blit the score readout, bump level */
    blit_numeric_readout();
    blit_color_src--;                    /* $95 */
    level_stage++;                       /* $6D */
    blit_numeric_readout();
    cpu.X = 0x03;
L_6288:
    cpu.Z = (--cpu.X == 0);              /* DEX sets Z */
L_6289:
    if (!cpu.Z) goto L_626a;
    dl_src_index = 0x56;
L_628f:
    if (dl_src_index != 0x0F) goto L_628f;   /* spin: wait for the VBI to reach $0F */
    draw_pattern_byte = 0x0F;            /* A == dl_src_index == $0F at spin exit */
    dl_src_index = 0;
    if ((bus_read(0xD300) & 0x01) == 0) goto L_62a9;   /* PORTA joystick (live on Amiga) */
    if ((bus_read(0xD01F) & 0x02) != 0) goto L_62b4;   /* CONSOL SELECT  (live on Amiga) */
L_62a9:
    dl_src_index = 0x56;
    if (level_stage < mem[0x0609]) goto L_6268;
L_62b4:
    audf2_sweep_step = 0xFF;
L_62b9:
    cpu.Y = draw_pattern_byte;            /* $6598,Y and $8B fed to dl_lms_scroll_step */
    frame_wait_count = mem[0x6598 + draw_pattern_byte];
    dl_src_index = draw_pattern_byte;
    dl_lms_scroll_step();
    dl_src_index = 0;
    audf2_sweep_clear_colors();
    if (--draw_pattern_byte != 0) goto L_62b9;   /* $B9 */
    frame_wait_count++;
    wait_frames();
    cpu.A = 0x55;                        /* shift_object_table_up takes A */
    shift_object_table_up();
    frame_wait_count = (uint8_t)(frame_wait_count << 1);  /* ASL $4C */
    wait_frames();
    dl_lms_reset_window();
    bus_write(0xD203, 0);                /* POKEY */
L_62e7:
    if (sfx_reinit_gate == 0) goto L_62ee;
L_62eb:
    ds_frame();
    reset_audctl_flags();
L_62ee:
    attract_timer = 0x64;
    if (level_or_state != 0) goto L_634f;
L_62f6:
    /* Amiga: idle-loop top = the convergence point after both in-place level-rebuild branches
       (L_622d door-scroll and intro_screen_build_seq).  Re-latch the door-field-ready gate that
       the in-place SELECT cycle cleared at L_6332, BEFORE ds_frame renders, so the 0->1 edge
       fires the door re-decode with the finished LEVEL-NN field.  Idempotent on the normal idle
       path (already 1).  Harmless on SDL (never read). */
    g_doorFieldReady = 1;
    ds_frame();
    if (cockpit_flag != 0) goto L_6309;
    /* 6502 $62F6 does LDY $060B before the JSR; copy_title_text_block_to_screen uses
     * that entry Y as the value it writes back to the $0091 block-selector (STY $0091).
     * cockpit_flag is 0 on this idle path, so pass it explicitly — a stale cpu.Y here
     * would leave $0091 un-reset and freeze the top-bar title/copyright swap. */
    copy_title_text_block_to_screen_core(cockpit_flag);
    if ((bus_read(0xD01F) & 0x04) != 0) goto L_6309;   /* CONSOL OPTION */
    if (++level_or_state != 0) goto L_6311;            /* $0004 */
L_6309:
    if (!(attract_timer & 0x80)) goto L_6324;
    if (mem[0x00E7] != 0) goto L_631e;
L_6311:
    sound_active_flag = cockpit_flag;
    if (cockpit_flag != 0) goto L_631b;
    cockpit_flag++;                       /* $060B */
L_631b:
    game_main_loop(); return;
L_631e:
    if (altitude_threshold == 0xC0) goto L_6311;
L_6324:
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_ipDispatch; g_ipDispatch++; }
#endif
    if ((bus_read(0xD300) & 0x01) == 0) goto L_6332;   /* PORTA joystick */
    if ((bus_read(0xD01F) & 0x02) != 0) goto L_634a;   /* CONSOL SELECT */
L_6332:
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_ipInPlace; g_ipInPlace++; }
#endif
    if (cockpit_flag == 0) goto L_6311;
    /* Amiga: the post-mother-ship SELECT in-place level cycle rewrites the door field $2000
       (with the new LEVEL-NN digits) via the L_622d door-scroll or intro_screen_build_seq
       WITHOUT re-entering boot_standby_launch_driver, so the g_doorFieldReady latch would
       stay 1 and the render's edge-triggered door re-decode would never fire (the new level
       digits never appeared).  Re-arm the edge: clear it here (the render holds the frozen
       old doors while it is 0 — see the rsBoostReturn hold in RescueOnFractalus.cpp) and set
       it back at the L_62f6 idle-loop top once the rebuild has finished, giving one re-decode
       of the finished field.  Harmless on SDL (never read). */
    g_doorFieldReady = 0;
    audio_timer_setup();
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_ipDoorScroll, g_ipIntroWrap;
      if (level_stage < mem[0x0609]) g_ipDoorScroll++; else g_ipIntroWrap++; }
#endif
    if (level_stage < mem[0x0609]) goto L_6347;
    intro_screen_build_seq();
    goto L_634a;
L_6347:
    goto L_622d;
L_634a:
    platform_poll_events();
    read_console_trig_delta();
    if (cpu.Z) goto L_62f6;
L_634f:
    DS_MILE(0);
    SA_TIMED(0, audio_timer_setup());
    SA_TIMED(1, rle_unpack_to_07f9());                 /* consumes Y */
    if (cockpit_flag == 0) {
        cpu.A = 0x00;                     /* fill_message_buffer takes A + X */
        cpu.X = 0x16;
        SA_TIMED(2, fill_message_buffer());
    }
    mem[0x08A3] = 0x23;                    /* L_635f */
    cockpit_flag = 0x23;
    sound_active_flag = 0x23;
    cpu.Y = (level_or_state != 0) ? 0x13 : 0x0B;   /* Y feeds save_color_clear_y_bit5 */
    cpu.A = 0xEA;
    SA_TIMED(3, save_color_clear_y_bit5());            /* takes A + Y */
    osc_step_counter = 0;
#ifdef ROF_COMBAT_LOAD
    /* COMBAT-LOAD BENCHMARK (see the header block): pin the starting level so
     * compute_stage_display_geometry below derives a real combat difficulty.  Everything
     * downstream — emplacement density $0623, saucer period $0621, enemy-fire mask $0624,
     * terrain roughness — then comes out of the faithful binary logic.  The headless
     * auto-launch would otherwise fly level 1, which has none of the three. */
    level_stage = (uint8_t)ROF_COMBAT_LEVEL;
#endif
    if (level_or_state == 0) {
        uint8_t prev = mem[0x0626];
        mem[0x0626] = level_stage;
        if (level_stage == prev) {        /* level unchanged from last run -> bump replay counter */
            mem[0x0627]++;
            goto L_63a7;
        }
        if (level_stage < prev) {         /* level dropped -> clear score/HUD state */
            for (int i = 0; i <= 4; i++)
                mem[0x0600 + i] = 0;
            for (int i = 0; i <= 5; i++)
                mem[0x32C5 + i] = 0;
        }
    }
    fresh_start_flag = 0;                  /* L_63a1 ($0627) */
    SA_TIMED(4, compute_stage_display_geometry());
#ifdef ROF_COMBAT_QUIET
    /* The CONTROL for the combat benchmark: same forced level, so the same terrain, the same
     * sim tuning and the same binary shape — but no combat at all ($0623 = no emplacements,
     * $0621 = no saucers; the Makefile also drops AUTO_FIRE).  Comparing COMBAT against this
     * attributes the delta to COMBAT rather than to "level 40 has rougher terrain than the
     * level-1 auto-launch", which is what a plain no-COMBAT baseline would really be measuring. */
    mem[0x0621] = 0; mem[0x0622] = 0; mem[0x0623] = 0;
#else
#ifdef ROF_COMBAT_LOAD
    /* Saucer spawn period.  $0621 bottoms out at $14 even at level $22+, and it counts down
     * once per terrain_draw_frame pass — at flight's real frame rate that is one spawn
     * OPPORTUNITY every ~4 s, half of which the RANDOM gate throws away and more of which the
     * "must sit above the terrain" gate rejects.  A 30 s probe window measured ZERO saucers,
     * so the saucer sprite path (draw_player3_object + the Amiga P3 viewport/scope mirrors)
     * never entered the profile at all.  Over-drive the period so a saucer is nearly always
     * up, and closes to point-blank often.  Deliberately UNFAITHFUL; combat_probe.gdb prints
     * the value it ran with so the number is never quoted as the level's own. */
    mem[0x0621] = (uint8_t)ROF_COMBAT_SAUCER;
    mem[0x0622] = (uint8_t)ROF_COMBAT_SAUCER;
#endif
#endif  /* ROF_COMBAT_QUIET */
#if defined(ROF_COMBAT_LOAD) && defined(ROF_FORCE_EMPLACEMENTS)
    /* Saturated worst case: $0623 is the RANDOM gate intro_seed_object_map tests before
     * turning a terrain peak into a gun emplacement, so $FF = EVERY peak carries one. */
    mem[0x0623] = 0xFF;
#endif
#if defined(ROF_COMBAT_LOAD) && defined(ROF_FLIGHT_PROBE)
    g_clLevel = level_stage; g_cl0621 = mem[0x0621];
    g_cl0623 = mem[0x0623];  g_cl0624 = mem[0x0624];
#endif
L_63a7:
    cpu.X = 0x1D;                         /* sfx_event_load takes X */
    SA_TIMED(5, sfx_event_load());
    SA_TIMED(6, vobj_draw_dispatch());
    SA_TIMED(7, render_bcd_counter());
    clear_scroll_accum();
    {
        uint8_t a = 0x3A;
        for (int8_t y = 0x05; y >= 0; y--) {   /* $08D4[5..0] = $3A,$38,$36,$34,$32,$30 */
            mem[0x08D4 + y] = a;
            a -= 0x02;
        }
    }
    placed_item_count_bcd = 0;
    SA_TIMED(8, startup_init());
#ifdef ROF_FLIGHT_PROBE
    g_saPhase = 20;   /* after startup_init, before build_line_addr_table_1000 */
#endif
    build_line_addr_table_1000();
#ifdef ROF_PLATFORM_AMIGA
    if (g_tunnelPrebuilt) {
        /* rings already plotted into $1000 during standby construction (tunnel_prebuild_rings)
           and decoded into tunnelBitmap at the reveal — just replay the draw's exit ZP scratch
           the descent's draw_ring_frame_step relies on, skipping the ~140ms re-plot. */
        tunnel_prebuild_replay_exit();
    } else
#endif
    {
        /* Fallback only — the pre-build above normally leaves g_tunnelPrebuilt set.  Claim + prime
           first so the draw's spans paint with the forward LUT into a clean bitmap. */
        platform_tunnel_rings_begin();
        SA_TIMED(9, draw_frame_pattern_seq());             /* consumes Y */
    }
    DS_MILE(1);                          /* end of stretch A (L_634f -> here: pure compute, no ds_frame) */
#ifdef ROF_FLIGHT_PROBE
    g_saPhase = 22;   /* after the ring branch / DS_MILE(1), before sfx_event_load */
#endif
    cpu.X = 0x01;                        /* sfx_event_load takes X */
    sfx_event_load();
#ifdef ROF_FLIGHT_PROBE
    g_saPhase = 23;   /* after sfx_event_load, entering the $067E door-wait ds_frame loop */
#endif
    do { ds_frame(); } while (mem[0x067E] != 0x1F);   /* wait for the door VBI to set $067E==$1F */
#ifdef ROF_FLIGHT_PROBE
    g_saPhase = 24;   /* door-wait done, HUD setup before the door-open sweep */
#endif
    mem[0x06E0] = 0;
    sync_flag = 0x8C;
    dl_ptr_lo = 0x17;
    dl_ptr_hi = 0xBA;
    screen_ptr_lo = 0x17;
    dl_bottom_index = 0x02;
    blit_row_counter = 0x7F;
    terrain_scroll_counter = 0x2B;
    cpu.A = 0x07;                        /* draw_cockpit_dial_bar takes A */
    draw_cockpit_dial_bar();
    mem[0x0684] = 0x7F;
    timer_676 = 0x09;
    mem[0x0686] = 0xFB;
    mem[0x0687] = 0xFF;
    cpu.A = 0x01;                        /* set_hud_fields_678_679 takes A */
    set_hud_fields_678_679();
    do {                                 /* door-open HUD sweep, one 10-frame tick per step */
        wait_frames_10();
        hud_field_678++;
        sfx_env_freq_val++;
        if (hud_field_678 == 0x03) {     /* mid-sweep: arm the door/ring state */
            mem[0x06CA] = 0xFF;
            history_ring_step = 0xFF;
            mem[0x06CD] = 0xFF;
            mem[0x06E6] = 0x01;
            mem[0x06E8] = 0x01;
            mem[0x06E9] = 0x01;
            mem[0x06F4] = 0;
            mem[0x06F6] = 0;
            mem[0x06F7] = 0;
        }
        timer_676--;
        refresh_hud_field_0b();
    } while (timer_676 != 0x04);
    /* wait for the gauge-fill VBI counter $0684 to reach $64 */
    do { ds_frame(); } while (mem[0x0684] != 0x64);
    cpu.Y = 0x05;                        /* sfx_reorder_voice_slot takes Y (also indexes $066B) */
    mem[0x066B + cpu.Y] = 0;
    sfx_reorder_voice_slot();
    /* wait for $0684 to reach $1D */
    do { ds_frame(); } while (mem[0x0684] != 0x1D);
    mem[0x06E6] = 0xFF;
    /* wait for the door-swoosh VBI counter $0686 to reach $64 */
    do { ds_frame(); } while (mem[0x0686] != 0x64);
    DS_MILE(2);                          /* door/HUD sweep done (VBI-paced) */
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned long g_sbATicks, g_sbAIsr;
      unsigned long _a0 = rof_subclock(), _ai = g_isrBeamLines;
#endif
    init_row_coords_9c();
    cpu.A = 0x00;                        /* draw_cockpit_dial_bar takes A */
    draw_color_idx = 0;
    draw_cockpit_dial_bar();
    cpu.Y = 0x0C;                        /* sfx_reorder_voice_slot takes Y */
    mem[0x066B + cpu.Y] = 0x0F;
    mem[0x0679 + cpu.Y] = 0xB4;
    sfx_reorder_voice_slot();
    timer_676 = 0x02;
    mem[0x06E8] = 0;
    mem[0x06E9] = 0;
    cpu.A = 0x01;                        /* set_hud_fields_678_679 takes A */
    set_hud_fields_678_679();
    vbi_flags = 0x01;
    frame_wait_count = 0x0F;
#ifdef ROF_FLIGHT_PROBE
      g_sbATicks = rof_subclock() - _a0; g_sbAIsr = g_isrBeamLines - _ai; }
#endif
    do {                                 /* wait out the door-swoosh, decrementing $0677 to 8 */
        wait_frames();
        if (mem[0x0677] == 0x08)
            frame_wait_count = 0x01;
        else
            mem[0x0677]--;
    } while (vbi_flags != 0);
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned long g_sbCTicks, g_sbCIsr;
      unsigned long _c0 = rof_subclock(), _ci = g_isrBeamLines;
#endif
    cpu.A = 0x04;                        /* draw_cockpit_dial_bar takes A */
    mem[0x0677] = 0x04;
    draw_cockpit_dial_bar();
    cpu.Y = 0x0C;                        /* sfx_reorder_voice_slot takes Y */
    mem[0x0679 + cpu.Y] = 0x65;
    sfx_reorder_voice_slot();
    cpu.Y = 0x0B;
    mem[0x066B + cpu.Y] = 0x01;
    mem[0x0679 + cpu.Y] = 0x0E;
    sfx_reorder_voice_slot();
    for (int i = 0; i <= 3; i++)          /* L_64ed */
        mem[0xD008 + i] = 0x01;
    HW_WRITE(0xD000, 0x38);              /* HPOSP0 */
    mem[0x00B5] = 0x62;
    HW_WRITE(0xD002, 0x8E);              /* HPOSP2 */
    HW_WRITE(0xD003, 0xB8);              /* HPOSP3 */
    clear_scroll_accum();
#ifdef ROF_FLIGHT_PROBE
      g_sbCTicks = rof_subclock() - _c0; g_sbCIsr = g_isrBeamLines - _ci; }
#endif
    DS_MILE(3);                          /* end of stretch B (init_row_coords/reorder/L_650b clear) */
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned long g_burstClrTicks, g_burstClrIsr;
      unsigned long _b0 = rof_subclock(), _bi = g_isrBeamLines;
#endif
    /* Clear the 46-byte ($2E) line buffer of each of the 45 viewport rows ($2C..0) — this
     * zeroes the $1000 stars/planet field so undrawn rows read as black.  The $073D/$0793
     * line-pointer table was built at build_line_addr_table_1000() (base $1000, stride $2E)
     * above and NOT rebuilt since, so the 45 row bases are exactly $1000 + i*$2E: one
     * contiguous 2070-byte span $1000..$1815.  Clear it in a SINGLE batched zero_run (one
     * align + ~517 move.l) instead of 45 calls that each re-read the table and re-align.
     * ISR-safe (main-loop-owned $1000 field; the launch VBI writes the $2000 ring / scrolls
     * $0C32-$0F32, never $1000).  Leaves $C1/$C2 = the final ($073D[0],$0793[0]) = $00/$10
     * that the per-row walk used to leave, for faithful mem state. */
    zero_run(mem + 0x1000, 45u * 0x2Eu);
    row_table_stride = 0x00;   /* $C1 (final row-ptr lo) */
    player_speed     = 0x10;   /* $C2 (final row-ptr hi) */
#ifdef ROF_FLIGHT_PROBE
      g_burstClrTicks = rof_subclock() - _b0; g_burstClrIsr = g_isrBeamLines - _bi; }
#endif
    DS_MILE(4);                          /* L_650b line-buffer clear done */
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned long g_burstMidTicks, g_burstMidIsr;
      unsigned long _m0 = rof_subclock(), _mi = g_isrBeamLines;
#endif
    copy_192_to_1800();
    DS_MILE(9);                          /* copy_192_to_1800 done */
    mem[0x00DC] = 0;
    display_flags = 0;
    dl_param_lo = 0x10;
    dl_param_hi = 0x18;
    wait_vcount_ge_7a();
    bus_write(0x0200, 0xC2);             /* VDSLST shadow (stars DLI) */
    bus_write(0x0201, 0x6C);
    HW_WRITE(0xD402, 0x20);              /* DLISTL/H: stars display list $3120 */
    HW_WRITE(0xD403, 0x31);
    init_object_positions();
#ifdef ROF_FLIGHT_PROBE
      g_burstMidTicks = rof_subclock() - _m0; g_burstMidIsr = g_isrBeamLines - _mi; }
#endif
    DS_MILE(10);                         /* init_object_positions done */
    terrain_state = 0x7F;
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned long g_fillTerrTicks, g_fillTerrIsr;
      unsigned long _ft0 = rof_subclock(), _fi = g_isrBeamLines;
      fill_terrain_columns();
      g_fillTerrTicks = rof_subclock() - _ft0; g_fillTerrIsr = g_isrBeamLines - _fi; }
#else
    fill_terrain_columns();
#endif
    DS_MILE(5);                          /* end of stretch C (fill_terrain_columns done) */
    for (uint8_t a = 0x00; a != 0x0D; a++) {   /* fade colour shadows $02C0..$02C3 up $00..$0C */
        for (int8_t x = 0x03; x >= 0; x--)
            mem[0x02C0 + x] = a;
        wait_frames_2();
    }
    DS_MILE(6);                          /* $6557 colour fade done (VBI-paced) */
    cpu.A = 0x30;                        /* build_line_addr_table_1000_stride takes A */
    build_line_addr_table_1000_stride();
    /* wait until terrain_state drops below $04 (CMP #$04; BPL) */
    do { ds_frame(); } while (!((uint8_t)(terrain_state - 0x04) & 0x80));
    DS_MILE(7);                          /* $656e terrain_state<4 wait done */
    RTCLOK_LOW = 0;
    do {                                  /* L_6578: every other frame, advance object positions */
        ds_frame();
        if (RTCLOK_LOW >= 0x02) {
            RTCLOK_LOW = 0;
#ifdef ROF_FLIGHT_PROBE
            { extern volatile unsigned long g_aopMax, g_aopMaxVbi;
              unsigned long _a0 = rof_subclock();
              advance_object_positions();
              unsigned long _d = rof_subclock() - _a0;
              if (_d > g_aopMax) { g_aopMax = _d; g_aopMaxVbi = rof_subclock()/313u; } }
#else
            advance_object_positions();
#endif
            if (mem[0x1002] == 0xFF)      /* planet reached */
                terrain_state = 0;
        }
        /* L_6590 */
    } while (terrain_state != 0);
    DS_MILE(8);                          /* $6578 stars-approach done -> flight */
    return;
#ifdef ROF_MEMBASE
#undef mem
#endif
}

/* ===========================================================================================
 * STATION CINEMATIC (scene 2) — the two BULK-MEMORY routines.
 *
 * The rest of the attract scene stays transliterated on purpose: measured on target with
 * amiga/star_fade.gdb, its per-frame path (station_audio -> station_missile_drift /
 * station_pm_shape_tick, pmg_colors_station, and the rof_manual.c animation subs) already runs
 * one iteration per vblank — the Atari's own rate — so a twin would buy nothing.  These two are
 * the exceptions, because they move BULK memory through the 6502 emulation: 1200 bytes x 14
 * passes for the fade, 340 display-list entries plus 122 rows for the build.
 *
 * Transliterated, the fade alone took **162 vblanks against the Atari's 15** (~1000 68000
 * cycles per emulated byte — every 6502 op stores one to three `cpu` flag bytes through abs.l
 * moves, and `LDA ($90),Y` re-derives its address out of mem[] on every iteration).
 * =========================================================================================== */

/* station_star_fade_in @ $1E79 — the starfield's fade-in, run once from station_init ($19F4),
 * before the attract loop starts scrolling the picture in.
 *
 * Fourteen passes ONE FRAME APART, each brightening every non-zero GTIA-9 nibble in the star
 * rows $2CB8-$3167 by one luminance step.  display_list_build seeds up to 30 of those rows with
 * a single pixel of value 1 ($10 or $01, the pair at $1C3E), so every star climbs 1 -> 15 in
 * lockstep and ends at $F0/$0F, full brightness.  In GTIA mode 9 the nibble IS the pen, so that
 * is the entire effect: 14 frames, one luminance step each.
 *
 * Two 6502 quirks here are load-bearing, not cleanups:
 *   - `AND #$F0` then `ADC #$10` DISCARDS the low nibble whenever the high one is non-zero, so a
 *     byte holding two pixels loses its right-hand one on the first pass.  The real starfield
 *     never has both nibbles set in one byte (one pixel per row), but the fixture's random mem[]
 *     does, and byte-identity means reproducing it.
 *   - the walk pointer is re-seeded BEFORE the pass counter is tested, so the fifteenth and last
 *     wait has no pass after it — which is why $0090/$0091 read $2CB8, not $3168, at the return.
 * The transliteration's tail call to station_sub_1EB4 is DEAD code: $1EB2's `BNE $1E8C` is only
 * ever reached with Z clear (from a CMP that just mismatched), so it always branches back into
 * the walk and never falls through. */
void station_star_fade_in(void) {
    /* $0090/$0091 is this routine's walk pointer; its sfx_reinit_gate / altitude_threshold
       names belong to other users of those cells (see docs/rename.md).  Held in mem[] at every
       point the 6502 could be observed from: seeded before each frame wait, and left at the
       walk's end address after each pass. */
    uint8_t adcIn = 0, adcOp = 0;   /* the last brightening ADC's operands — see the exit flags */
    for (int pass = 14; pass >= 0; pass--) {
        mem[0x0090] = 0xB8;                                    /* 1e7d  pointer = $2CB8 */
        mem[0x0091] = 0x2C;
        cpu.A = 0x2C;                       /* 1e81's LDA #$2C — the byte wait_frames_1 PHA's */
        wait_frames_1();                                       /* 1e85  one frame per step */
        if (pass == 0) break;               /* 1e88  DEX/BPL: X wraps to $FF after the 15th seed */
        for (uint16_t a = 0x2CB8; a != 0x3168; a++) {          /* 1e8c  the 1200-byte walk */
            const uint8_t v = mem[a];
            if (v == 0) continue;                              /* 1e8e  blank cell */
            /* The accumulator going into the ADD is the ANDed high nibble, or — down the
               low-nibble arm, which RELOADS the cell at $1E9A — the whole byte. */
            adcIn = (uint8_t)((v & 0xF0) ? (v & 0xF0) : v);
            adcOp = (uint8_t)((v & 0xF0) ? 0x10 : 0x01);
            mem[a] = (uint8_t)(adcIn + adcOp);                 /* 1e95 / 1e9c  one step brighter */
        }
        mem[0x0090] = 0x68;                 /* 1ea6  the walk ends with the pointer at $3168 */
        mem[0x0091] = 0x31;
    }
    /* Exit registers and flags.  All seven are DEAD at the one call site — $19F7 is `LDA #$FF`
       and the attract loop opens `LDA $13 / CMP #$04` — but matching them keeps the harness's
       cpu-diff count at 0, so a future REAL divergence is visible instead of buried in noise
       (the trap in the native-twin-validation-gaps note).
         A   = $2C, restored by the last wait's closing PLA.
         X   = $FF and N/Z from the DEX that fell through to the RTS.
         C   = 1 from the `CMP #$31` that ended pass 14's walk (equal ⇒ carry set).
         V   = the signed overflow of the LAST brightening ADD, the only V-setting op here. */
    cpu.X = 0xFF;
    cpu.N = 1; cpu.Z = 0;
    cpu.C = 1;
    cpu.V = (uint8_t)((((uint8_t)~(adcIn ^ adcOp) & (adcIn ^ (uint8_t)(adcIn + adcOp))) >> 7) & 1);
    cpu.Y = 0;                              /* 1e79  LDY #0, never touched again */
}

/* display_list_build @ $1C40 — build the attract scene's 340-entry ANTIC display list at $B800
 * and seed the starfield it shows.  Run once from station_init ($19B9).
 *
 * Two runs of mode-F LMS entries (`4F lo hi` = one mode-F row, load this address):
 *   1. 122 entries walking the station picture at $0600 in 40-byte rows — the image the scroll
 *      later slides down into view.
 *   2. 218 entries of sky.  Each points at the SHARED blank row $2C90 unless a RANDOM draw picks
 *      it as one of up to 30 "encounter" rows (1 in 8, while $0085 lasts), in which case it gets
 *      a 40-byte row of its own from $2CB8 up, holding ONE pixel: a random column < 40
 *      (rejection-sampled from RANDOM & $3F) set to $10 or $01 from the pair at $1C3E — value 1,
 *      the dimmest luminance, which station_star_fade_in then brightens to 15.
 * Then the 3-byte JVB at $1C3B is copied in after the last entry (display_scroll walks it down
 * the list to keep the window 192 rows), and the animation state is reset.
 *
 * ⚠ RANDOM ($D20A) is a READ-CLOCKED LFSR, so the twin has to read it exactly as often as the
 * 6502 does and in the same order: one draw per candidate row (skipped entirely once the
 * encounter budget is spent — C's && short-circuit is doing that job), then the rejection loop
 * plus one index draw for each row that becomes a star. */
void display_list_build(void) {
    uint16_t dl  = 0xB800;                            /* 1c40  $0081/$0082 display-list cursor */
    uint16_t src = 0x0600;                            /* 1c48  $0083/$0084 the row pointed at */

    for (int row = 0x7A; row != 0; row--) {           /* 1c50  122 picture rows */
        mem[dl]     = 0x4F;                           /* 1c54  mode F + LMS */
        mem[dl + 1] = (uint8_t)src;                   /* 1c5b  the LMS address, pre-increment */
        mem[dl + 2] = (uint8_t)(src >> 8);            /* 1c65 */
        src += 40;                                    /* 1c5e/1c67  16-bit, via the ADC carry */
        dl  += 3;                                     /* 1c6b */
    }

    uint8_t encounters = 0x1E;                        /* 1c79  $0085 — at most 30 star rows */
    uint8_t x          = 0xDA;                        /* 1c85  LDX #$DA, live in X until a star */
    src = 0x2CB8;                                     /* 1c7d  the first star row */
    for (int row = 0xDA; row != 0; row--) {           /* 1c85  218 sky rows */
        mem[dl] = 0x4F;                               /* 1c8d */
        if (encounters != 0 && (bus_read(0xD20A) & 0x07) == 0) {   /* 1c8f  1 in 8 */
            mem[dl + 1] = (uint8_t)src;               /* 1ca8  a row of its own */
            mem[dl + 2] = (uint8_t)(src >> 8);        /* 1cad */
            uint8_t col;                              /* 1cb1  reject until the column is < 40 */
            do { col = (uint8_t)(bus_read(0xD20A) & 0x3F); } while (col >= 0x28);
            x = (uint8_t)(bus_read(0xD20A) & 0x01);   /* 1cbb  which of the two seed values */
            mem[src + col] = mem[0x1C3E + x];         /* 1cc1  $10 or $01 = luminance 1 */
            encounters--;                             /* 1cc6 */
            src += 40;                                /* 1cc8  next star row */
        } else {
            mem[dl + 1] = 0x90;                       /* 1c9b  the shared blank row $2C90 */
            mem[dl + 2] = 0x2C;                       /* 1ca0 */
        }
        dl += 3;                                      /* 1cd3 */
    }

    for (int y = 2; y >= 0; y--)                      /* 1ce2  the 3-byte JVB, high byte first */
        mem[dl + y] = mem[0x1C3B + y];

    terrain_state          = 0;   /* 1cec  $0089 — here the animation's phase INDEX */
    dl_src_index           = 0;   /* 1cf0  $008B — here the animation's phase COUNTER */
    terrain_scroll_counter = 4;   /* 1cf4  $008A — here the per-phase hold TIMER */

    /* The ZP cursors are part of the mem contract, so publish their exit values once (nothing
       reads them mid-build: the routine makes no call that can yield, and the station VBI —
       already installed by $198D — only touches DLISTL/H, COLBK and RTCLOK). */
    mem[0x0081] = (uint8_t)dl;    mem[0x0082] = (uint8_t)(dl >> 8);
    mem[0x0083] = (uint8_t)src;   mem[0x0084] = (uint8_t)(src >> 8);
    mem[0x0085] = encounters;
    mem[0x0086] = 0;                                  /* 1cde  DEC'd to zero by the row loop */

    /* Exit registers and flags, matched so the harness reports 0 cpu diffs (they are all dead at
       the one call site — $19BC reloads A immediately — but see station_star_fade_in's note).
         A/N/Z come from the closing `LDA #$04`, not from the JVB copy's DEY.
         Y   = $FF, that copy's DEY/BPL exit.
         X   = the last star's RANDOM & 1, or still $DA if no row became a star.
         C/V = the carry and overflow of the LAST `ADC #$03` display-list advance, which is
               $F9 + 3 on the 340th entry ⇒ both clear, whatever the RANDOM draws did. */
    cpu.A = 4;
    cpu.X = x;
    cpu.Y = 0xFF;
    cpu.N = 0; cpu.Z = 0; cpu.C = 0; cpu.V = 0;
}

/* game_main_loop @ $3D48 — the second half of the orchestration apex.  game_entry
 * (-> init_game_vars_attract_timer) chains here; it does the one-time game init
 * (display list, sound, PMG, player, cockpit gated on $060B), then loops:
 *   L_3e0f  boot_standby_launch_driver()      (Standby/attract + launch cinematic; RTSes after launch)
 *           flight init ($3e12)  (clear PMG/colours, VVBLKI=$4FF5, terrain seed, gameplay state)
 *   L_3eba  the in-game flight loop (two terrain passes/iteration for double-buffering),
 *           level-clear handoff at L_3f59, then back to L_3eba or L_3e0f.
 * The flight loop never returns (the user-quit path longjmps out of the pump); the
 * L_631b boot_standby_launch_driver->game_main_loop tail call and the L_3e0f boot_standby_launch_driver() call
 * mirror the genuine mutual structure (both are TCO'd / return-normally, as in the
 * shipped transpiled build — no unbounded recursion).
 *
 * Same seam/contract as boot_standby_launch_driver: NOT in `make validate` (it spin-waits on VBI
 * state, and never returns), verified on FS-UAE.  Faithful transcription preserving
 * cpu/mem so the (mostly-native) leaf callees behave identically; the two hooked
 * spin labels L_3eba/L_3f6d become one real Amiga frame via ds_frame(); the
 * wait_vcount_30/wait_frames_10/wait_frames_1/wait_frames frame primitives drive
 * frames internally and are called as-is.  No leaf exit-register is consumed here
 * (every post-call read reloads from mem), so no fidelity fix-ups are needed.
 *
 * ⚠ RESTART TRAMPOLINE (2026-07-13): on the Atari the game-over / attract restart is a tail
 * JMP to game_main_loop ($3D48) — it never grows the stack.  But game_main_loop CALLS
 * enemy_check NON-tail (code follows at L_3ef5), so the base frame is still live when a death
 * re-enters game_main_loop (enemy_check -> intro_cinematic_loop -> intro_teardown_fade_loop ->
 * init_game_vars_attract_timer -> game_main_loop).  Each death then pushes a fresh, LARGE
 * game_main_loop frame that never unwinds -> stack overflow + memory corruption (guru),
 * observed on the first death.  The old "TCO'd, no unbounded recursion" note above was wrong
 * (proven by the FS-UAE backtrace).  Fix: anchor a setjmp at the base (outermost) entry and
 * turn every re-entry into a longjmp back to it — the nested frames are reclaimed and the loop
 * restarts from the top, exactly as the JMP does.  game_main_loop is not `make validate`d and
 * consumes no leaf exit register, so wrapping it is safe. */
static void game_main_loop_body(void);
void game_main_loop(void) {
    static rof_jmp_buf s_gmlJmp;
    static int   s_gmlActive = 0;
    if (s_gmlActive) ROF_LONGJMP(s_gmlJmp, 1);         /* re-entry (restart): unwind to the base anchor */
    s_gmlActive = 1;
    (void)ROF_SETJMP(s_gmlJmp);                        /* restarts resume here with a fresh body frame */
    game_main_loop_body();
    s_gmlActive = 0;                                   /* body returned (it never should) — re-arm cleanly */
}
static void game_main_loop_body(void) {
    /* ---- One-time hardware + game init (runs once; the loop below never returns) ---- */
    bus_write(0x022F, 0);                     /* SDMCTL shadow: display DMA off while we rebuild */
#ifndef ROF_PLATFORM_AMIGA
    bus_write(0xD01D, 0);                     /* GTIA GRACTL off (dead on Amiga: copper owns display) */
#endif
    for (int i = 0; i < 8; i++) {             /* zero the 8 GTIA colour regs + the $36CA state block */
        mem[0xD00D + i] = 0;
        mem[0x36CA + i] = 0;
    }
    bus_write(0x02C8, 0);                      /* COLBK shadow = black */
    wait_vcount_30();
    bus_write(0x0222, 0xCC);                   /* VVBLKI = $53CC (attract/standby VBI); Amiga VBI reads this */
    bus_write(0x0223, 0x53);
    display_list_init();
    rtclok_frac = 0;
    mem[MEM_terrain_pen0_fade] = 0;            /* $00DC */
    game_phase = 0;
    mem[MEM_dli_dispatch_index] = 0;           /* $00C7 */
    for (int i = 0; i < 0x19; i++)             /* clear the $062C dither/threshold state block */
        mem[MEM_static_dither_threshold + i] = 0;
    life_counter = 0;
#ifndef ROF_PLATFORM_AMIGA
    bus_write(0xD407, 0x08);                   /* ANTIC PMBASE (dead on Amiga) */
    bus_write(0xD409, 0x04);                   /* ANTIC CHBASE (dead on Amiga) */
#endif
    bus_write(0xD20F, 0x03);                   /* POKEY SKCTL: init serial/keyboard (live: audio) */
    loader_util();
    init_terrain_col_tables();
    game_init_77DF();
    game_init_7588();
    game_init_76CB();
    bus_write(0xD20E, 0xC0);                   /* POKEY IRQEN: enable timer/keyboard IRQs */
    if (cockpit_flag != 0) standby_scoreboard_render();  /* draw the static cockpit bitmap when enabled */
    lock_on_indicator_fill_cells();
    cpu.Y = 0x09; draw_dial_bar_column();      /* Y = dial column index (callee arg) */
    game_sub_4606();
    draw_cockpit_dial_bar_core(0x08);          /* dial index 8 */
    bus_write(0x02C6, 0x2C);                   /* COLOR2 shadow */
    bus_write(0x02C7, 0x26);                   /* COLOR3 shadow */
    for (int i = 0; i <= 8; i++)               /* seed the $00CF palette from the $4DF1 table */
        mem[0x00CF + i] = mem[0x4DF1 + i];
    for (int i = 0; i < 3; i++)                /* $0645 shield/damage cells = $80 */
        mem[MEM_shield_or_damage + i] = 0x80;
    lock_on_indicator_state = 0x80;
    sfx_voice_distort_0b = 0x80;
    sfx_voice_distort_0c = 0x80;
    startup_init();
    sfx_voice_distort_06 = 0xA0;
    sfx_voice_distort_0d = 0xA0;
    sfx_env_prio_val = 0xA0;
    sfx_event_load_core(0x1F);                     /* init input slot $1F, then $20 */
    sfx_event_load_core(0x20);
#ifndef ROF_PLATFORM_AMIGA
    bus_write(0xD40E, 0x40);                   /* ANTIC NMIEN: enable DLI (dead on Amiga) */
#endif
    display_flags = 0xC0;
    dl_param_lo = 0x0D;                         /* windscreen-band DL params ($350D) */
    dl_param_hi = 0x35;
    for (;;) {  /* game / attract outer loop — re-entered after each level or crash */
#ifdef ROF_FLIGHT_PROBE
    { unsigned long _ds = rof_subclock(); boot_standby_launch_driver(); g_probeDispSetup = rof_subclock() - _ds; }
#else
    boot_standby_launch_driver();                           /* Standby/attract + launch cinematic (RTSes after launch) */
#endif
    clear_pm_state_core(0x2A);                 /* PMG clear fill */
    wait_frames_1();
    dl_param_lo = 0x0D;                         /* windscreen-band DL params ($350D) */
    dl_param_hi = 0x35;
    frame_counter = 0;
    zp_flag_05 = 0;
    for (int i = 0; i < 0x2C; i++)             /* clear the $0020 ZP working set */
        mem[0x0020 + i] = 0;
    for (int i = 0; i < 0xA6; i++)             /* clear the $2830 object-state block */
        mem[0x2830 + i] = 0;
    screen_state = 0;
    init_terrain_render_buffers();
    fill_buffer2_region_ff();
    clear_terrain_lo_buffers();
    unpack_terrain_seed_cols();
    cpu.A = 0x45; wait_vcount_eq();            /* A = target VCOUNT (callee arg) */
    bus_write(0x0222, 0xF5);                   /* VVBLKI = $4FF5 (flight VBI); Amiga VBI reads this */
    bus_write(0x0223, 0x4F);
    for (int i = 0; i < 0x57; i++)             /* clear the $0B31 PMG DMA buffer */
        mem[0x0B31 + i] = 0;
    bus_write(0x026F, 0x11);                   /* GPRIOR shadow: PMG priority */
    copy_terrain_seed_rows();
    wait_vcount_ge_7a();
    bus_write(0x0200, 0xEE);                   /* VDSLST = $49EE (DLI vector shadow) */
    bus_write(0x0201, 0x49);
#ifndef ROF_PLATFORM_AMIGA
    bus_write(0xD402, 0x6B);                   /* ANTIC DLISTL/H = $316B (dead on Amiga: copper owns display) */
    bus_write(0xD403, 0x31);
    bus_write(0xD004, 0x40);                   /* GTIA HPOSP0 (dead on Amiga) */
#endif
#ifdef ROF_FLIGHT_PROBE
    { g_probeFlightVbi = (unsigned short)(rof_subclock() / 313u);
      unsigned long _t0 = rof_subclock();
    init_gameplay_state();
      unsigned long _t1 = rof_subclock(); g_probeGameInit = _t1 - _t0;
    if (fresh_start_flag == 0) {               /* run the intro only on a fresh start */
        intro_random_setup();
        intro_unmark_random_cells();
        intro_seed_object_map();
    }
      g_probeIntro = rof_subclock() - _t1;
      g_probeInitTotal = rof_subclock() - _t0; }
#else
    init_gameplay_state();
    if (fresh_start_flag == 0) {               /* run the intro only on a fresh start */
        intro_random_setup();
        intro_unmark_random_cells();
        intro_seed_object_map();
    }
#endif
    row_table_stride = 0x60;
    row_table_base_lo = 0x10;
    row_table_base_hi = 0x10;
#ifdef ROF_FLIGHT_PROBE
    { unsigned long _r = rof_subclock(); build_row_addr_table(); copy_row_addr_subset();
      g_probeRowAddr = rof_subclock() - _r; }
#else
    build_row_addr_table();
    copy_row_addr_subset();
#endif
    if (level_or_state == 0) {                 /* fresh level start */
        timer_or_counter = 0x54;
        joystick_saved = 0x02;
    } else {
        joystick_saved = 0x01;
    }
    for (;;) {                           /* L_3eba: in-game flight loop (one frame/iteration) */
    FP_ITER();
#ifdef ROF_PLATFORM_AMIGA
    /* ESC freeze/pause ($0043 event_active_flag): on the Atari the dispatcher's slot-7 takeover
       does a faked RTI to $52BB (`JMP $52BB` self-loop), abandoning the whole main-loop context so
       terrain_draw_frame + game_state_update + enemy_check all halt — the sim/enemies fully freeze.
       Only the VBI (an NMI, immune to the I flag) keeps firing: it runs the colour-cycle screensaver
       (its $5039 EOR-strobe of the $00CF-$00DD display params, which the DLIs / updateFlightCopper
       push to the viewport pens) and polls the ESC-again unfreeze (the KEYWIN dispatcher, which
       clears $0043).  We can't abandon the C call stack, so mirror the OBSERVABLE behaviour: spin
       driving real frames (VBI + render, so the strobe reaches the copper and the unpause key is
       seen) while running NO game logic, until the VBI clears $0043.  BREAK (Del) still restarts —
       the KEYWIN dispatcher longjmps out via game_loop_reset_trampoline before the spin sees it. */
    while (event_active_flag != 0) { g_flightRenderHalf = 0; ds_frame(); }
#endif
    CL_PH_ITER();                    /* latch the combat state for this whole iteration */
    CL_CAL();                        /* price the MACHINE (CALIBRATE builds only; see CL_CAL) */
    g_flightRenderHalf = 0;          /* this ds_frame shows the DISPLAY half (prev pass 2, offset 0) */
    CL_PH(CL_PH_FRAME, ds_frame(), g_clFrameTicks);
    FP_ITER_MARK();
    /* --- PASS 1: render terrain field BACK half (draw col-base $30; clear/collision $33). ---
       On the Atari this half is double-buffered against the display (offset-0) half below; the
       two halves alternate on screen each vblank.  (clear/collision use the draw base + 3.) */
    CL_PH(CL_PH_SETUP, terrain_frame_setup(), g_fSetup);
    CL_PH(CL_PH_CLEAR, clear_terrain_column_core(0x33), g_fClear);
    CL_PH(CL_PH_DRAW,  terrain_draw_frame_core(0x30), g_fDraw);
#ifndef ROF_PLATFORM_AMIGA
    /* Builds the mode-D field's sky + dot texture (the SDL display + the silhouette scan).  On the
       Amiga nothing reads the field (dots->plane2, sky->$260E, band blanked), so it is skipped —
       its ZP residue ($80/$81/$95/$96) is overwritten by the next terrain_frame_setup / control-
       point rebuild before any read.  See ROF_FIELD_PLOT + renderFlightDirect. */
    FP_TIME(fill_terrain_silhouette_core(0x33), g_fColl);
#endif
    /* Display pass 1's BACK half (offset $30).  This is the second shown frame per iteration —
       on the Atari both halves alternate on screen; here we render+show each as it completes, so
       neither pass is dropped (smoother motion, ~2x displayed framerate). */
    g_flightRenderHalf = 1;
    CL_PH(CL_PH_FRAME, ds_frame(), g_clFrameTicks);
    pilot_state = game_state;
    CL_PH(CL_PH_BOLT,  game_state_update(), g_fState);
    game_phase = 0x02;
    CL_PH(CL_PH_ENEMY, enemy_check(), g_fEnemy);
    if (life_counter < 0x0E) {
        if (level_or_state != 0) {
            game_sub_7B54();
        } else if (mem[0x003A] == 0x01) {    /* level-complete signal on a fresh level */
            level_cleared_flag = 0x01;
        }
    }
    /* L_3ef5 */
    /* --- PASS 2: render terrain field DISPLAY half (draw col-base $00; clear/collision $03). ---
       This offset-0 half is what the Amiga port currently shows every frame. ---
       ⚠ These three carried NO timing brackets until 2026-08-06, so every historical
       "terrain = N% of the frame" figure from g_fSetup/g_fClear/g_fDraw was taken over PASS 1
       ONLY — i.e. roughly half the real terrain work.  They accumulate into the same buckets as
       pass 1 now, so the phase totals finally cover the whole iteration. */
    CL_PH(CL_PH_SETUP, terrain_frame_setup(), g_fSetup);
    CL_PH(CL_PH_CLEAR, clear_terrain_column_core(0x03), g_fClear);
    CL_PH(CL_PH_DRAW,  terrain_draw_frame_core(0x00), g_fDraw);
#ifndef ROF_PLATFORM_AMIGA
    fill_terrain_silhouette_core(0x03);   /* Amiga: skipped — see PASS 1 note */
#endif
    if (game_state != 0) pilot_state = game_state;   /* keep pilot_state while game_state is active */
    game_state_update();
    enemy_check();
#if defined(ROF_PLATFORM_AMIGA) && defined(ROF_FLIGHT_PROBE)
    { extern volatile uint8_t g_forcePilotR1;
      if (g_forcePilotR1 && g_iterCount > 8) {       /* headless range-1 pilot simulation */
          clear_colors_done_003E = 0x01;             /* $003E: pilot visible (pilot_visible gate) */
          mem[0x0079] = 0x01;                        /* pilot distance -> range digit 1 */
          pilot_prev = 0x01; pilot_visible = 0x01;   /* force the render gate */
      } }
#endif
    if (pilot_prev != 0 && pilot_visible != 0) pilot_render();  /* render only when prev && current visible */
    pilot_prev = pilot_visible;
    pilot_visible = (pilot_state == 0) ? clear_colors_done_003E : 0x00;
    game_phase = 0x01;
    if (clear_colors_done_003E != 0x01) {            /* not in the colour-clear (death) phase */
        if (flight_mode_state != 0x02) {
            if (mem[0x003D] != 0) {                  /* a death handoff is pending */
                mem[0x003D] = 0x02;
                timer_or_counter = 0x0E;
            }
        }
    }
    /* Stay in the flight loop unless this life is over (flight_mode_state == 2). */
    if (flight_mode_state != 0x02) continue;
    /* Life over — set up the level-clear / crash handoff. */
    joystick_saved = 0x03;               /* flight_mode_state(2) + 1 (used if we keep flying below) */
    cpu.Y = 0x80;
    mem[0x28D9] = cpu.Y;
    mem[0x28DA] = cpu.Y;
    if (terrain_depth_step < 0x40) continue;   /* still descending: keep flying */
    /* Spin (showing the display half) until the level-ready flag goes negative (bit7 set). */
    do {
        g_flightRenderHalf = 0;
        ds_frame();
    } while (!(level_ready_flag & 0x80));
    lock_on_indicator_state = cpu.Y;     /* faithful: Y held since the LDY $80 above */
    /* A = 0 is threaded (unchanged) through wait_frames_10 / wait_frames' PHA/PLA into
       joystick_saved and the buffer-clear fill byte — MUST stay 0 or the PMG buffers get
       cleared with garbage (crash after game over). */
    cpu.A = 0x00;
    bcd_osc_dir = cpu.A;
    range_to_pilot = cpu.A;
    wait_frames_10();
    joystick_saved = cpu.A;              /* 0 (preserved through wait_frames_10) */
    wait_frames();
    { uint8_t fill = cpu.A;              /* 0 (preserved through wait_frames) = the clear byte */
      for (int i = 1; i <= 0xA3; i++)    /* clear $0F1D.. (offset 0 left intact) */
          mem[0x0F1D + i] = fill;
      for (int i = 0; i <= 0x1E; i++)    /* clear $0E8F.. */
          mem[0x0E8F + i] = fill; }
    if (indicator_light_state == 0x4E)   /* turn the indicator light off if it was on */
        indicator_light_state = 0x46;
    game_sub_4606();
    { uint8_t a = anim_counter_2;        /* clamp the animation counter to $2A */
      if (a >= 0x2B) { a = 0x2A; anim_counter_2 = 0x2A; }
      game_state = a;
      display_flags = a;
      clear_pm_state_core(a);             /* clear_pm_state consumes A = the clamped value */
    }
    mem[0x066E] = 0;
    ring_push_unmarked();
    break;                               /* restart the outer loop (re-run boot_standby_launch_driver) */
    }   /* end inner in-game flight loop (L_3eba) */
    }   /* end outer game / attract loop (L_3e0f) */
}
