// Native launch-cinematic ports.
//
// The post-START launch cinematic ("STAND BY" + score, throttle gauge, doors,
// tunnel) runs LINEARLY inside the Atari display_setup ($5F1D), which draws each
// phase then busy-waits (wait_frames_60) before the next.  The Amiga is frame-
// driven (StandbyScene::update per VBI), so we reproduce display_setup as a set
// of one-shot phase steps invoked at the right moments rather than one blocking
// routine.
//
// Each phase's DRAWING is done by the genuine 6502 routines: the full transpile
// (src/gen/rof_gen.c) is already linked into the Amiga build (it backs the SFX),
// so we call those functions directly instead of re-implementing them — they are
// the 6502 code "converted to native C", with zero divergence risk.  Being one-
// shot, their CPU-emulation overhead is irrelevant.  The interrupt-time audio is
// fully native (SfxPlayer.cpp) and never touches the shared `cpu` register
// struct, so driving cpu-based transpiled code from the main loop is race-free.

#include "PaulaAudio.h"   // mem[] + int types (matches the other native modules)
extern "C" volatile uint8_t mem[65536];

// The transpile's 6502 register file (src/cpu/cpu.h: `Cpu6502 cpu`).  We mirror
// its POD layout here rather than #include cpu.h, which drags in the real
// <stdint.h> and clashes with the Amiga framework's compat typedefs.  Field
// order/types must match cpu.h exactly so this binds to the same `cpu` symbol.
extern "C" {
typedef struct { uint8_t A, X, Y, S, N, V, Z, C, I, D; } Cpu6502;
extern Cpu6502 cpu;

// Genuine transpiled launch routines (src/gen/rof_gen.c):
void save_color_clear_y_bit5(void);   // $47B2: $00D8=A, Y&=$DF, -> show_cockpit_message ($47B8)
void render_bcd_counter(void);        // $49A0: render 6-digit BCD score ($0600-3) to $32C5-$32CA
void game_sub_4447(void);             // $4447: $00BF=A+8 -> setup_dial_bar_draw -> draw_object_column

// Stars/planet phase (display_setup $64C8-$6594):
void clear_scroll_accum(void);                  // $6B71: zero $02C0-3 + scroll accum $A1-A5
void copy_192_to_1800(void);                    // $75A5: seed the $1810 lower viewport bank
void build_line_addr_table_1000_stride(void);   // $65D2: $073D/$0793 row-addr table, $1000 stride $C1
void init_object_positions(void);               // $6B85: seed planet object table $08A4..
void fill_terrain_columns(void);                // $6AE5: fill height buffers $0C32-$0F32 (POKEY RANDOM)
void scroll_terrain_columns(void);              // $6AEE: one column scroll step (A = $0089 on entry)
void advance_object_positions(void);            // $6BA8: one planet zoom step -> draw_vline_pair to $1000
void draw_player3_object(void);                 // $42A7: the planet as a scaled Player-3 object
}

// launch_show_standby_native: port of display_setup $635F-$63AF (cinematic
// effect 1).  Replaces the attract title scroll with the static "STAND BY..."
// message and the score (0 -> "     0") on the cockpit message line.
//
//   $635F  LDA #$23 / STA $08A3 / STA $060B / STA $006C
//   $6369  LDY #$0B (="STAND BY...";  #$13 if $0004!=0)
//   $6371  LDA #$EA ; save_color_clear_y_bit5  (sets $00D8, writes the message)
//   $6376  LDA #$00 / STA $0629
//   $6391  clear score field $0600-4 and on-screen cells $32C5-$32CA
//   $63A1  STA $0627 (A=0)
//   $63AF  render_bcd_counter  (writes "     0")
//
// Setting $060B=$23 makes StandbyScene::update() stop calling the attract title
// writer (copy_text_block_to_screen_native is gated on $060B==0), so the
// RESCUE/(C)1985 toggle ceases and "STAND BY..." persists.
extern "C" void launch_show_standby_native(void)
{
    mem[0x08A3] = 0x23;
    mem[0x060B] = 0x23;   // stops the attract title toggle
    mem[0x006C] = 0x23;

    cpu.A = 0xEA;
    cpu.Y = (mem[0x0004] == 0) ? 0x0B : 0x13;
    save_color_clear_y_bit5();          // -> show_cockpit_message: "STAND BY..." into $32B7

    mem[0x0629] = 0x00;

    // The genuine code gates this clear (carry skips it on the first level
    // change), relying on an earlier full message-line clear that the Amiga's
    // shortcut never ran; the score cells still hold attract-title scroll bytes
    // here, so run the clear unconditionally to reach the same state.
    for (int i = 0; i <= 4; i++) mem[0x0600 + i] = 0x00;   // $6391 score BCD field
    for (int i = 0; i <= 5; i++) mem[0x32C5 + i] = 0x00;   // $639B on-screen cells

    mem[0x0627] = 0x00;

    // (display_setup also calls compute_gauge_geometry_from_006D / input_init /
    // vobj_draw_dispatch here; those drive the gauge PMG and belong to effect 2,
    // so they are added when the gauge fill is ported.)
    render_bcd_counter();               // "     0" -> $32C5-$32CA
}

// ---- effect 2: throttle gauge fill (vobj vertical object) -------------------
// The throttle gauge is the Atari "vertical object": a player-1 P/M strip at
// $0D98 that fills downward as the object steps to the bottom ($DC).  Each step
// (vobj_step_down $41E8 / vobj_erase_row $4207) advances the vert position $062F
// by +4 and writes $F0 (a 4px-wide bar segment) into the strip row indexed by
// ($DC - $062F) >> 2, with a per-step frame wait.  On the frame-driven Amiga we
// run ONE step per call (the 6502's wait becomes the caller's frame cadence) and
// report when the bar has reached the bottom.
//
// The fill writes $0D99+index (index 0..55, 56 rows) plus $0D98 on the wrap, so
// the strip spans $0D98..$0DD0 (the door savestate shows 57 contiguous $F0).
// $062F runs 0 -> $DC (step +4); index = ($DC - $062F) >> 2 goes 55 -> 0, i.e.
// the bar fills bottom (index 55) to top (index 0).  The colour for each segment
// is $4DEA[index>>3] (the "shape" table is actually a colour ramp), ending at
// $4DEA[0]=$D6 (#560) when full — see vobj_erase_row ($4207) / vobj_advance.

static const uint8_t kVobjBottom = 0xDC;   // $00DC bottom limit

extern "C" void launch_gauge_init_native(void)
{
    mem[0x062F] = 0x00;                      // vert position at the top of travel
    for (uint16_t i = 0; i < 57; i++)        // clear the strip ($0D98..$0DD0)
        mem[0x0D98 + i] = 0x00;
    mem[0x00DE] = mem[0x4DEA + 6];           // initial (bottom) segment colour
}

// One vobj_step_down ($41E8) + vobj_erase_row ($4207) iteration, minus the 6502
// 1-frame wait (the caller paces it).  Returns 1 while filling, 0 once full.
extern "C" uint8_t launch_gauge_step_native(void)
{
    uint16_t pos = (uint16_t)mem[0x062F] + 4u;   // $41ED: LDA #4 / ADC $062F
    uint8_t done = 0;
    if (pos >= kVobjBottom) {                    // $41F4: CMP $DC, reached bottom
        mem[0x0D98] = 0xF0;                      // $41FA
        mem[0x062F] = kVobjBottom;               // $4201
        done = 1;
    } else {
        mem[0x062F] = (uint8_t)pos;              // $4201
    }
    // vobj_erase_row: index Y = ($DC - $062F) >> 2 ; $0D99+Y = $F0 ; colour ramp
    uint8_t y = (uint8_t)((kVobjBottom - mem[0x062F]) >> 2);
    mem[0x0D99 + y] = 0xF0;
    mem[0x00DE] = mem[0x4DEA + (y >> 3)];        // $421C: $00DE = $4DEA[Y>>3]
    return done ? 0u : 1u;
}

// ---- effects 3 & 4: left indicator-light column -----------------------------
// The left lights are the lower half (rows 8..15) of the cockpit dial-bar drawn
// by game_sub_4447 -> setup_dial_bar_draw -> draw_object_column ($43E8): for each
// row X (=$00BD, $0F..$08) it writes cell $4581[X] with bit7 = lit/unlit set by
// X >= $00BF (lit) vs X < $00BF (unlit).  $00BF = A+8.  Lit = $34/$37/$38 (COLPF2),
// unlit = $B4/$B7/$B8 (COLPF3).  The existing render() draws these cockpit cells.

// Effect 3: when the doors start, light the bottom-left indicator only.
//   display_setup $63FD: LDA #$07 / game_sub_4447  ($00BF=$0F -> only row 15 = $34E5 lit)
extern "C" void launch_light_doorstart_native(void)
{
    cpu.A = 0x07;
    game_sub_4447();
}

// Effect 4: before the tunnel animates, light the whole left column.
//   display_setup $6482: LDA #$00 / game_sub_4447  ($00BF=$08 -> rows 8..15 all lit)
extern "C" void launch_light_all_native(void)
{
    cpu.A = 0x00;
    game_sub_4447();
}

// ---- stars / space phase (display_setup $64C8-$6552 setup) ------------------
// Once the tunnel ring auto-clears $0088 (advance_message_column, NativeHandlers),
// the cinematic switches the viewport to ANTIC mode-D from $1000 (VDSLST $6CC2),
// pre-fills the fractal terrain-column buffers, and scrolls them while the planet
// builds.  We port the mem[]-state subset of $64C8-$6552 (HW register writes —
// SIZEM/HPOSP/DLIST — are the Amiga's copper/sprite job) and call the genuine
// transpiled setup routines.  build_line_addr_table_1000 points the draw row-addr
// table $073D/$0793 at $1000 stride $30 (=48, wide playfield) so the planet's
// draw_vline_pair writes land in the displayed buffer.
extern "C" void launch_stars_init_native(void)
{
    clear_scroll_accum();                       // $6506 ($6B71)
    mem[0x00B5] = 0x62;                          // $64F8

    // Clear the mode-D viewport buffer $1000..$1810 (43 rows * 48 bytes).
    for (uint16_t i = 0; i < 43u * 48u; i++) mem[0x1000 + i] = 0u;   // $650B loop equiv
    copy_192_to_1800();                          // $6521 ($75A5): seed lower bank $1810

    mem[0x00DC] = 0x00;                          // $6524: viewport COLBK source = black
    mem[0x0071] = 0x00;                          // $6528
    mem[0x3157] = 0x10; mem[0x3158] = 0x18;      // $652A: DL LMS bytes (-> $1810; copper ignores)
    mem[0x0200] = 0xC2; mem[0x0201] = 0x6C;      // $6537: VDSLST = $6CC2 (stars/planet DLI)

    init_object_positions();                     // $654B ($6B85): seed planet object table
    mem[0x0089] = 0x7F;                          // $654E: arm scroll_terrain_columns
    fill_terrain_columns();                      // $6552 ($6AE5): fill height buffers

    cpu.A = 0x30;                                // $6569: stride $30 (48, wide)
    build_line_addr_table_1000_stride();         // $65D2 -> $073D/$0793 point at $1000
}

// One stars-scroll step (the $6557-$656E warm-up + spin, frame-driven): the VBI
// dispatcher's $0089 branch is JMP $6AEE with A = $0089, so set A then call it.
// scroll_terrain_columns sets $0089 = 2 when the scroll accumulator reaches its
// mark; display_setup leaves the stars phase once $0089 < 4 (i.e. == 2).  Returns
// 1 while still scrolling, 0 once $0089 has dropped below 4 (advance to planet).
extern "C" uint8_t launch_stars_step_native(void)
{
    cpu.A = mem[0x0089];
    scroll_terrain_columns();                    // $6AEE
    return (mem[0x0089] >= 4u) ? 1u : 0u;        // $656E: CMP #4 / BPL spin
}

// ---- planet phase (display_setup $6574-$6594) -------------------------------
// The planet "zooms up" as advance_object_positions ($6BA8) steps its scaled
// object table and draw_vline_pair writes the growing sphere into $1000, plus
// draw_player3_object ($42A7) renders it as a scaled Player-3 sprite.  The loop
// runs every other frame ($6578: $0014 >= 2) until $1002 == $FF, then $0089 = 0
// and display_setup returns to flight.  Returns 1 while zooming, 0 when done.
extern "C" uint8_t launch_planet_step_native(void)
{
    advance_object_positions();                  // $6BA8 -> update_object_distance -> draw_vline_pair
    // draw_player3_object ($42A7) renders the planet as Player-3 — a heavy TRANSPILED
    // 6502 routine.  We don't render P3 as an Amiga sprite (sprite 3 stays null), so
    // its output is invisible; skipping it removes the dominant per-step cost.  The
    // visible sphere comes entirely from advance_object_positions -> draw_vline_pair
    // into $1000, and the $1002==$FF "risen" mark is set there, not by P3.
    if (mem[0x1002] == 0xFFu) {                   // $6585: planet fully risen
        mem[0x0089] = 0x00;                       // $658C
        return 0u;
    }
    return 1u;
}
