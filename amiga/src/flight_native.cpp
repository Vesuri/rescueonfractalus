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
extern "C" volatile uint8_t mem[65536];

// The transpile's 6502 register file (src/cpu/cpu.h: `Cpu6502 cpu`) — mirrored
// as a POD (see launch_native.cpp for why we don't #include cpu.h).
extern "C" {
typedef struct { uint8_t A, X, Y, S, N, V, Z, C, I, D; } Cpu6502;
extern Cpu6502 cpu;

// Flight init subroutines (game_entry $3E12-$3EA6) — genuine transpiled:
void clear_pm_state(void);            // $3FBF: zero player/missile state + PCOLR shadows
void clear_colors(void);              // $3CC3: zero colour shadows
void game_init_753B(void);            // $753B
void game_init_45A1(void);            // $45A1
void clear_terrain_lo_buffers(void);  // $6B63: zero $0E32/$0F32 buffers
void game_init_7558(void);            // $7558
void game_init_45EE(void);            // $45EE
void intro_random_setup(void);        // $6FBF: fresh-start RANDOM seeding ($0627==0)
void intro_setup_70B3(void);          // $70B3
void intro_sub_7498(void);            // $7498
void game_setup_7460(void);           // $7460: build row-addr table $073D/$0793 (C3/C4 base, C1 stride)
void game_setup_7483(void);           // $7483
void main_loop_body(void);            // $73C8: initial main-loop frame setup

// Flight main-loop heavy set ($3EBA pass A / $3EF5 pass B):
void terrain_frame_setup(void);       // $9E54: per-frame view-transform setup
void clear_terrain_column(void);      // $AD5F: clear a terrain column band (X = start column)
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

// Set during flight_init_native so transpiled frame-wait spin loops (wait_frames_60
// via main_loop_body) advance RTCLOK in compute time instead of waiting on the real
// VBI — kills the ~5s launch-pacing delay on the dev F-skip.  (Defined in PaulaAudio.cpp.)
extern volatile uint8_t g_fastForwardFrames;
}

// flight_vbi_native: the motion half of one flight frame (flight VBI $4FF5,
// $004A-gated subset).  Cockpit/HUD gauges from that block ($520F+) are deferred.
extern "C" void flight_vbi_native(void)
{
    if (mem[zp::joystickSaved] == 0) return;        // $51B2: LDA $004A / BEQ (skip when not flying)
    flight_control_integrate();          // $51B9 ($8E5B): joystick + throttle -> world pos
    update_terrain_scanline_proj();      // $51BC: project pitch/altitude for the new frame
    render_bcd_counter();                // top-bar score: BCD $0601 -> text line $32C5
}

// g_activeVbi: which VBI body the real INTB_VERTB ISR runs, mirroring how the Atari
// swaps the VVBLKI vector by phase.  0 = none (during scene init), 1 = standby/launch
// ($52D7 vbi_handler_standby), 2 = flight ($4FF5 vbi_handler_flight).  Set by the
// scene (initialize() -> 1, startFlight() -> 2) only AFTER that phase's state is ready.
extern "C" volatile uint8_t g_activeVbi = 0;

// standby_vbi_native: the faithful $52D7 per-frame body (defined in NativeHandlers.cpp).
extern "C" void standby_vbi_native(void);

// game_vbi_isr: the dispatcher the real Amiga vertical-blank interrupt calls.  This is
// where the Atari's per-frame VBI work belongs — run in the VBI, not the main loop —
// and it swaps body by phase exactly as the Atari swaps VVBLKI between $52D7 and $4FF5.
// On the Atari the 6502 registers are saved/restored by the OS VBLANK entry/exit
// (SYSVBV/XITVBV).  Our transpile funnels ALL 6502 register state through one shared
// global `cpu`, so we replicate that hardware save/restore here: the main-loop pass
// (flight_frame_native, or the cinematic native drivers) may be mid-instruction using
// `cpu` when this interrupt preempts it.  mem[] needs no saving — it is the shared
// "RAM", and the VBI bodies touch scratch disjoint from the main loop (as on the
// Atari, where $52D7/$4FF5 ran concurrently with the main loop and it worked).
extern "C" void game_vbi_isr(void)
{
    Cpu6502 saved = cpu;                            // == OS VBLANK PHA;TXA;PHA;TYA;PHA
    if      (g_activeVbi == 2) flight_vbi_native();  // $4FF5 in-flight VBI
    else if (g_activeVbi == 1) standby_vbi_native(); // $52D7 standby/launch VBI
    cpu = saved;                                    // == XITVBV PLA;TAY;PLA;TAX;PLA
}

// flight_init_native: port the mem[]-state subset of game_entry $3E12-$3EA6.
// The terrain row-addr table is rebuilt for the FLIGHT viewport: base $1010,
// stride $60=96 (verified vs the flight DL $316B, which LMSes mode-D rows from
// $1070=$1010+96 onward at +$60 each — row 0 is the off-screen scroll margin).
extern "C" void flight_init_native(void)
{
    g_fastForwardFrames = 1;     // resolve main_loop_body's wait_frames_60 calls instantly
    cpu.A = 0x2A; clear_pm_state();                          // $3E12: LDA #$2A / JSR $3FBF
    clear_colors();                                          // $3E17: $3CC3
    mem[0x3157] = 0x0D; mem[0x3158] = 0x35;                  // $3E1A: cinematic-DL LMS bytes ($350D)
    mem[zp::frameCounter] = 0x00; mem[0x0005] = 0x00;                  // $3E24
    for (uint16_t i = 0x20; i <= 0x4B; i++) mem[i] = 0;      // $3E2A: clear zp $20-$4B
    for (uint16_t i = 0; i < 0xA6; i++) mem[0x2830 + i] = 0; // $3E32: clear $2830-$28D5
    mem[zp::screenState] = 0x00;                                      // $3E3A
    game_init_753B();                                        // $3E3D
    game_init_45A1();                                        // $3E40
    clear_terrain_lo_buffers();                              // $3E43 ($6B63)
    game_init_7558();                                        // $3E46
    // $3E49 LDA #$45 / JSR $3C75 (wait_vcount_eq) — SKIP (VCOUNT busy-wait)
    // $3E4E-$3E55 VBI vector $0222/$0223=$4FF5 — SKIP (Amiga VBI)
    for (uint16_t i = 0; i < 0x57; i++) mem[0x0B31 + i] = 0; // $3E58: clear $0B31-$0B87
    mem[zp::gpriorShadow] = 0x11;                                      // $3E62: GPRIOR/PRIOR shadow
    game_init_45EE();                                        // $3E67
    // $3E6A JSR $3C7B (wait_vcount_ge_7a) — SKIP (VCOUNT busy-wait)
    // $3E6D-$3E74 DLI vector $0200/$0201=$49EE — SKIP (Amiga copper DLI)
    // $3E77-$3E83 DLISTL/H=$316B, DMACTL $D004=$40 — SKIP (Amiga copper)
    main_loop_body();                                        // $3E86 ($73C8)
    if (mem[zp::freshStartFlag] == 0) {                                  // $3E89: fresh start
        intro_random_setup();                                // $3E8E ($6FBF)
        intro_setup_70B3();                                  // $3E91
        intro_sub_7498();                                    // $3E94
    }
    mem[zp::rowTableStride] = 0x60;                                      // $3E97: stride $60 = 96
    mem[zp::rowTableBaseLo] = 0x10; mem[zp::rowTableBaseHi] = 0x10;                  // $3E9B/$3E9F: base $1010
    game_setup_7460();                                       // $3EA3: build $073D/$0793 row-addr table
    game_setup_7483();                                       // $3EA6
    if (mem[zp::levelOrState] == 0) {                                  // $3EA9
        mem[zp::timerOrCounter] = 0x54; mem[zp::joystickSaved] = 0x02;              // $3EAD/$3EB8 (A=2 path)
    } else {
        mem[zp::joystickSaved] = 0x01;                                  // $3EB6/$3EB8 (A=1 path)
    }
    g_fastForwardFrames = 0;     // back to real-time frame pacing for steady-state flight
}

// flight_frame_native: one pass of the flight main-loop heavy set.  The Atari
// $3EBA loop runs the set TWICE per iteration for double buffering — pass A
// ($3EBA: X=$33/$30, $0042=2) writes the offset-48 half of the stride-96 buffer;
// pass B ($3EF5: X=$03/$00, $0042=1) writes the offset-0 half (the one the F0
// renderer displays).  One Amiga frame = one pass; we alternate each call, the
// natural double-buffer cadence.  Returns mem[$0072] (==2 -> level complete).
//
// NOTE (F0 scope): the per-pass HUD/pilot tail ($3EDB game_sub_7B54, $3F14
// pilot_render) is deferred to milestone F3; this runs the terrain + state +
// enemy set, which is the dominant per-frame cost being measured.
static uint8_t flightParity = 0;   // 0 = pass A (offset-48 half), 1 = pass B (offset-0 half)

extern "C" void flight_reset_parity_native(void) { flightParity = 0; }

extern "C" uint8_t flight_frame_native(void)
{
    terrain_frame_setup();                                         // $9E54 / $3EF5
    if (flightParity == 0) {
        cpu.X = 0x33; clear_terrain_column();                       // $3EBD
        cpu.X = 0x30; terrain_draw_frame();                       // $3EC2 (offset-48 half)
        cpu.X = 0x33; terrain_collision();                   // $3EC9
        mem[zp::pilotState] = mem[zp::gameState];                           // $3ECC: LDA $0041 / STA $288F
        game_state_update();                                 // $3ED1
        mem[zp::gamePhase] = 0x02;                                  // $3ED4
        enemy_check();                                       // $3ED8
    } else {
        cpu.X = 0x03; clear_terrain_column();                       // $3EFA
        cpu.X = 0x00; terrain_draw_frame();                       // $3EFF (offset-0 half, displayed)
        cpu.X = 0x03; terrain_collision();                   // $3F04
        if (mem[zp::gameState]) mem[zp::pilotState] = mem[zp::gameState];          // $3F07: conditional
        game_state_update();                                 // $3F0E
        enemy_check();                                       // $3F11
        mem[zp::gamePhase] = 0x01;                                  // $3F36
    }
    flightParity ^= 1u;
    return mem[zp::playerLives];                                      // $3F50: ==2 -> level complete
}
