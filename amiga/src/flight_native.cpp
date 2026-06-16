// Native in-game FLIGHT ports — the continuation of the launch cinematic.
//
// On the Atari the whole game is one program: display_setup ($5F1D) runs the
// launch cinematic (doors/tunnel/stars/planet) and RTSes back into game_entry
// ($3CDE), which does the flight init ($3E12-$3EA6) and then runs the flight
// main loop at $3EBA.  The Amiga is frame-driven (RescueOnFractalus::update per
// VBI), so — exactly like launch_native.cpp — we reproduce the mem[]-state
// subset of the init and run ONE heavy loop pass per frame, calling the genuine
// transpiled 6502 routines (src/gen/rof_gen.c, all linked into the build).
//
// SKIPPED (as launch_native skips its equivalents):
//   - HW-register writes: VBI vector $0222/$0223=$4FF5, DLI vector $0200/$0201=
//     $49EE, DLISTL/H ($D402/3)=$316B, DMACTL $D004=$40 — the Amiga copper/VBI
//     own the display, so these Atari display writes are not replayed.
//   - The VCOUNT busy-waits wait_vcount_eq ($3C75) / wait_vcount_ge_7a ($3C7B):
//     they spin on ANTIC VCOUNT $D40B, which the Amiga never advances → they
//     would hang.  The frame-driven loop provides the cadence instead.

#include "PaulaAudio.h"   // mem[] + int types (matches the other native modules)
#include "AtariZp.h"      // zp:: named mem[] offsets (heading/world pos/game state/...)
#include "../../src/gen/rof_native.h"  // typed C cores (clear_terrain_column_at, ...)
#include "FlightProf.h"   // per-frame VBI-count profiler
extern "C" volatile uint8_t mem[65536];

// ---- per-frame profiler ------------------------------------------------------
// g_flightProf accumulates per-phase deltas; read it from the debugger.
volatile struct FlightProf g_flightProf = { 0 };
// terrain_draw_frame object-loop sub-phase probe (rof_native.c, -DROF_TDRAW_PROF):
// beam lines spent in the fractal subdivision vs the projection+object-plot.
extern "C" unsigned long g_tdSubdiv = 0, g_tdProjPlot = 0;
extern "C" unsigned short flight_vbi_tick(void) {
    return (unsigned short)((mem[0x0013] << 8) | mem[0x0014]);  // RTCLOK $0013:$0014
}
extern "C" void flight_prof_reset(void) {
    g_flightProf.terrain = g_flightProf.stateEnemy = g_flightProf.render =
        g_flightProf.copper = g_flightProf.frames = g_flightProf.updateTot =
        g_flightProf.renderTot = g_flightProf.isrLines = g_flightProf.isrCalls = 0;
    g_flightProf.tFrameSetup = g_flightProf.tClear = g_flightProf.tDraw =
        g_flightProf.tCollision = 0;
    g_tdSubdiv = g_tdProjPlot = 0;
}
// Raster-beam line counter (0..~312 PAL), ~63.56us/line — a sub-frame clock the
// VBI ISR can use (RTCLOK is frozen for the whole ISR).  VPOSR bit0 = line bit 8.
static inline unsigned short beam_line(void) {
    unsigned short vpos  = *(volatile unsigned short*)0xDFF004;  // bit0 = V8
    unsigned short vhpos = *(volatile unsigned short*)0xDFF006;  // hi byte = V7..V0
    return (unsigned short)(((vpos & 1) << 8) | (vhpos >> 8));
}

extern "C" unsigned short rof_beam_line(void) { return beam_line(); }

// The transpile's 6502 register file (src/cpu/cpu.h: `Cpu6502 cpu`) — mirrored
// as a POD (see launch_native.cpp for why we don't #include cpu.h).
extern "C" {
typedef struct { uint8_t A, X, Y, S, N, V, Z, C, I, D; } Cpu6502;
extern Cpu6502 cpu;

// Flight init subroutines (game_entry $3E12-$3EA6) — genuine transpiled:
void clear_pm_state(void);            // $3FBF: zero player/missile state + PCOLR shadows
void clear_colors(void);              // $3CC3: zero colour shadows
void init_terrain_render_buffers(void); // $753B: prime height-max ($250F-$260E=$FF) + clear $1070 terrain bitmap
void fill_buffer2_region_ff(void);    // $45A1: fill 8x32 (stride 48) region at $2098 in the $2000 buffer with $FF
void clear_terrain_lo_buffers(void);  // $6B63: zero $0E32/$0F32 buffers
void unpack_terrain_seed_cols(void);  // $7558: RLE-unpack $4DFA->$0C32, $4E09->$0D32 terrain column buffers
void copy_terrain_seed_rows(void);    // $45EE: copy 8-byte seed rows ($4DD2->$0C88, $4DDA->$0D88, $4DE2->$0B88)
void intro_random_setup(void);        // $6FBF: fresh-start RANDOM seeding ($0627==0)
void intro_unmark_random_cells(void); // $70B3: randomly clear bit7 of $0900 grid cells
void intro_seed_object_map(void);     // $7498: clear $0A00, seed object markers from marked $0900 cells
void build_row_addr_table(void);      // $7460: build 85-entry row-addr table $073D/$0793 (C3/C4 base, C1 stride)
void copy_row_addr_subset(void);      // $7483: copy first 48 row-addr entries (reversed) -> $2932/$2962
void init_gameplay_state(void);       // $73C8: per-game/level gameplay init (run once from game_entry)

// Flight main-loop heavy set ($3EBA pass A / $3EF5 pass B):
void terrain_frame_setup(void);       // $9E54: per-frame view-transform setup
// clear_terrain_column ($AD5F) is reached via its typed core clear_terrain_column_core()
// (rof_native.h) instead of the cpu.X = N; clear_terrain_column() 6502-ABI form.
void terrain_draw_frame(void);        // $A31E: main per-frame terrain draw driver (X = half)
void terrain_collision(void);         // $AE53: terrain collision + final column fill (X)
void game_state_update(void);         // $A99C: game state machine
void enemy_check(void);               // $3FCD: enemy/event dispatch

// Flight VBI ($4FF5) per-frame motion core — the $004A-gated block at $51B9.
// On the Atari these run in the VBI (asynchronous, once/frame); the Amiga is
// frame-driven so we run them once per kFlight update, BEFORE the heavy render
// pass.  flight_control_integrate reads PORTA $D300 (the Amiga HW read returns
// $FF = neutral stick, so the ship flies straight) and integrates throttle into
// the world position $2885/$2887/$2889 + heading — i.e. it is what MAKES THE
// TERRAIN SCROLL.  update_terrain_scanline_proj projects the new pitch/altitude.
void flight_control_integrate(void);     // $8E5B
void update_terrain_scanline_proj(void); // $9833 (the JSR is at $51BC inside vbi_handler_flight)
void render_bcd_counter(void);           // $49A0: draw BCD score ($0601) to top line $32C5
void update_gauge_digits(void);          // $548D: in-game SFX voice engine + ring drain (Atari VBI tail $534D)
void reorder_sprite_slot(void);          // $5614: voice-priority mixer — assigns a POKEY channel ($0705) to slot cpu.Y
}

// flight_vbi_native: the motion half of one flight frame (flight VBI $4FF5,
// $004A-gated subset).  Cockpit/HUD gauges from that block ($520F+) are deferred.
extern "C" void flight_vbi_native(void)
{
    if (mem[zp::joystickSaved] == 0) return;        // $51B2: LDA $004A / BEQ (skip when not flying)
    unsigned short a = beam_line();      // sub-frame timer: RTCLOK is frozen for the whole ISR
    flight_control_integrate();          // $51B9 ($8E5B): joystick + throttle -> world pos
    update_terrain_scanline_proj();      // $51BC ($9833): project pitch/altitude
    render_bcd_counter();                // top-bar score: BCD $0601 -> text line $32C5
    update_gauge_digits();               // $548D: drain the SFX event ring -> POKEY/Paula (Atari VBI tail $534D)
    unsigned short b = beam_line();
    g_flightProf.isrLines += (b >= a) ? (unsigned short)(b - a)
                                      : (unsigned short)(b + 313 - a);  // PAL wrap (~313 lines)
    g_flightProf.isrCalls++;
}

// g_activeVbi: now only a "scene ready" gate (0 = scene still initialising, ISR does
// nothing — the snapshot's VVBLKI may be stale; nonzero = ready, dispatch on VVBLKI).
// The genuine transpiled chain swaps the real VVBLKI vector ($0222/$0223) per phase, and
// game_vbi_isr dispatches on THAT (see below), so this no longer selects the body.
extern "C" volatile uint8_t g_activeVbi = 0;

// standby_vbi_native: the faithful $52D7 per-frame body (defined in NativeHandlers.cpp).
extern "C" void standby_vbi_native(void);
// vbi_handler_station ($1B30): the attract-mode VBI (transpiled).  It sets the $0080
// sync flag the station_init attract loop spins on, and bumps RTCLOK itself.
extern "C" void vbi_handler_station(void);

// game_vbi_isr: the dispatcher the real Amiga vertical-blank interrupt calls.  This is
// where the Atari's per-frame VBI work belongs — run in the VBI, not the main loop —
// and it swaps body by phase exactly as the Atari swaps VVBLKI between $52D7 and $4FF5.
// On the Atari the 6502 registers are saved/restored by the OS VBLANK entry/exit
// (SYSVBV/XITVBV).  Our transpile funnels ALL 6502 register state through one shared
// global `cpu`, so we replicate that hardware save/restore here: the main-loop pass
// (game_main_loop's terrain_draw_frame, or the cinematic native drivers) may be mid-instruction using
// `cpu` when this interrupt preempts it.  mem[] needs no saving — it is the shared
// "RAM", and the VBI bodies touch scratch disjoint from the main loop (as on the
// Atari, where $52D7/$4FF5 ran concurrently with the main loop and it worked).
extern "C" void game_vbi_isr(void)
{
    if (!g_activeVbi) return;                        // scene still initialising — stay inert
    // Dispatch on the LIVE VVBLKI vector ($0222/$0223), exactly as the Atari OS VBLANK
    // jumps through it.  The genuine station_init/display_setup/game_main_loop install
    // $1B30 (attract) / $52D7 (standby+cinematic) / $4FF5 (flight) in turn, so the right
    // body runs automatically — including across the internal cinematic->flight switch
    // inside game_main_loop.  An unknown or half-written vector falls back to standby
    // (harmless for the odd frame during a two-byte vector update).
    uint16_t vbi = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
    Cpu6502 saved = cpu;                            // == OS VBLANK PHA;TXA;PHA;TYA;PHA
    if      (vbi == 0x4FF5) flight_vbi_native();     // $4FF5 in-flight VBI
    else if (vbi == 0x1B30) vbi_handler_station();   // $1B30 attract VBI (sets $0080 + RTCLOK)
    else                    standby_vbi_native();    // $52D7 standby/launch VBI (and fallback)
    cpu = saved;                                    // == XITVBV PLA;TAY;PLA;TAX;PLA
    paula_noise_tick();                             // refresh poly17 noise sample (no cpu use)
}

// sfx_engine_reset_native: faithful replica of the SFX engine reset $5433 (mislabelled
// font_display_init in symbols.csv), called on the Atari during game init ($3D35) and at
// launch ($6118).  Clears the $0719 event ring (head/tail $0073/$0074) and the 14 voice-
// slot envelope arrays, assigns the 4 physical POKEY channels to voice slots 1..4
// ($0705 = {2,4,6,8}) and mutes their AUDC, seeds the mixer scratch, and sets AUDCTL=$60.
// On the Atari this runs at game init ($3D35) and launch ($6118) so update_gauge_digits
// starts from a clean, silent state.
// ⚠ CURRENTLY UNWIRED on the Amiga (the genuine native game_main_loop chain does its own
// init); kept for wiring into the native flight/launch path alongside seed_engine_drone_native.
extern "C" void sfx_engine_reset_native(void)
{
    mem[0x0073] = 0x00;                       // ring head
    mem[0x0074] = 0x00;                       // ring tail
    for (int y = 1; y <= 0x0E; y++) {         // $543b: clear voice-slot arrays, slots 1..14
        mem[0x066B + y] = 0; mem[0x0705 + y] = 0; mem[0x0687 + y] = 0;
        mem[0x0695 + y] = 0; mem[0x06A3 + y] = 0; mem[0x06B1 + y] = 0;
        mem[0x06BF + y] = 0; mem[0x06CD + y] = 0; mem[0x06DB + y] = 0;
        mem[0x06E9 + y] = 0; mem[0x06F7 + y] = 0;
    }
    mem[0x0714] = 0x00;                        // $545f mixer "top priority" value
    mem[0x0715] = 0x02;                        // $5462 mixer "top slot" index
    for (int y = 4; y >= 1; y--) {             // $5467: assign POKEY channels to slots 4..1
        uint8_t a = (uint8_t)(y << 1);         // TYA; ASL -> 8,6,4,2
        mem[0x0705 + y] = a;                   // voice slot y -> POKEY reg index a
        platform_hw_write((uint16_t)(0xD1FF + a), 0x00);  // mute AUDCn
    }
    mem[0x0706] = 0x00; mem[0x0708] = 0x00;    // $5477/$547a mixer scratch
    mem[0x0712] = 0x02; mem[0x0713] = 0x06;    // $547d/$5482
    platform_hw_write(0xD208, 0x60);           // $5487 AUDCTL = $60
}

// seed_engine_drone_native: install the continuous engine-drone voices for flight.
//
// GROUND TRUTH (atari800 -pokeyrec of real played flight + host build/rof voice dump,
// 2026-06-13): the steady-flight engine is THREE voice slots 12/13/14 ($0C/$0D/$0E):
//   slot 12: distort $80 (NOISE)  prio 4 -> POKEY ch2 (AUDC2=$84) — the engine body
//   slot 13: distort $A0 (pure)   prio 1 -> POKEY ch1 (AUDC1=$A1) — a tracking whine
//   slot 14: distort $A0 (pure)   prio 1 -> POKEY ch3 (AUDC3=$A1) — a tracking whine
// Their FREQUENCY is rewritten every frame by flight_control_integrate ($8E5B @ $91DC:
// $0679+$0C/$0D/$0E from the ship pitch $002D/$002E) — which already runs in
// flight_vbi_native — so the engine pitch tracks throttle.  All the Atari does to START
// it is set the distortion (cold-init $3DE2/$3DE8), the priorities, and run the launch
// engine-ramp ($63FF-$64E8) whose end-state hands these three voices their POKEY channels
// via reorder_sprite_slot ($5614).  We install the launch end-state directly here, then let
// the mixer + flight_control_integrate + update_gauge_digits (all ported) sustain it.
//
// ⚠ CURRENTLY UNWIRED: the genuine flight path (native game_main_loop's inline $3E12-$3EB8
// flight init) does not yet call this, so genuine-flight has no engine drone.  Wire it into
// the native game_main_loop flight init when verifying in-flight audio.
//
// Voice-array layout (base + slot index): distort $065D+Y, priority $066B+Y, freq $0679+Y.
extern "C" void seed_engine_drone_native(void)
{
    mem[0x006C] = 0x23;                   // sound_active_flag (launch $6367; flight steady = $23)
    // distortion (the cold-init $3DE2/$3DE8 writes): slot12 noise, slots13/14 pure
    mem[0x065D + 0x0C] = 0x80;            // $0669 slot 12 distort = NOISE
    mem[0x065D + 0x0D] = 0xA0;            // $066A slot 13 distort = pure
    mem[0x065D + 0x0E] = 0xA0;            // $066B slot 14 distort = pure
    // priorities (launch end-state): slot12=4, slots13/14=1.  flight_control_integrate
    // rewrites slot12's each frame; 13/14 stay as seeded.
    mem[0x066B + 0x0C] = 0x04;            // $0677 slot 12 priority
    mem[0x066B + 0x0D] = 0x01;            // $0678 slot 13 priority
    mem[0x066B + 0x0E] = 0x01;            // $0679 slot 14 priority
    // initial freqs (flight_control_integrate overwrites these next frame from pitch)
    mem[0x0679 + 0x0C] = 0x65;            // $0685 slot 12 freq
    mem[0x0679 + 0x0D] = 0x64;            // $0686 slot 13 freq
    mem[0x0679 + 0x0E] = 0x68;            // $0687 slot 14 freq
    // hand each voice a POKEY channel via the priority mixer (launch does this with the
    // $5614 calls at $6493/$64D7/$64E6).  X=0 = the "newly active voice" promote path.
    cpu.X = 0x00; cpu.Y = 0x0C; reorder_sprite_slot();   // slot 12 -> a channel (noise body)
    cpu.X = 0x00; cpu.Y = 0x0D; reorder_sprite_slot();   // slot 13 -> a channel
    cpu.X = 0x00; cpu.Y = 0x0E; reorder_sprite_slot();   // slot 14 -> a channel
}
