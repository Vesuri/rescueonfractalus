// rof_native_amiga.cpp — Amiga-only native (68000) reimplementations.
//
// These are the Amiga twin functions: hand-written 68000-native bodies that
// REPLACE the 6502-transpiled versions (rof_gen.c) on the hot/per-frame path,
// where the per-instruction CPU+flag emulation runs ~60x too slow on a 7 MHz
// 68000.  Unlike the shared, validated twins in src/gen/rof_native.c (which are
// bit-for-bit equivalent to their __t6502 oracle and link into BOTH backends),
// these are deliberately "lossy": they drop the Atari hardware-register writes
// the Amiga handles via the Copper / VBI ISR, and route audio through
// platform_hw_write -> Paula.  They are therefore Amiga-only and NOT run through
// `make validate`.
//
// Consolidated 2026-06-17 from four single-purpose files (history under those
// names): SfxPlayer.cpp, station_native.cpp, NativeHandlers.cpp, flight_native.cpp.
// Kept as one file so the Amiga implementations are easy to find — the matching
// validated twins likewise live in one file (src/gen/rof_native.c).  The original
// per-file banner comments are preserved as the section headers below.

#include "PlatformAmiga.h"          // PlatformAmiga::pokeyRandom / noiseTick + uint types
#include "../../gen/mem.h"                     // MEM_<name> named Atari mem[] offsets
#include "FlightProf.h"             // per-frame VBI-count profiler (g_flightProf)
#include "../../gen/rof_native.h"   // typed C cores (clear_terrain_column_core, ...)

extern "C" volatile uint8_t mem[65536];


// ============================================================================
// SFX engine  (was SfxPlayer.cpp)
// ============================================================================

// The attract/standby-theme SFX sequencer sfx_voice_tick ($70F9) + sfx_seq_step
// ($7148) now live as byte-identical VALIDATED twins in src/gen/rof_native.c
// (see VALIDATE_FUNCS), linked into both backends.  They route POKEY AUDF/AUDC
// through bus_write -> platform_hw_write -> the Paula want[] table, so the Amiga
// no longer needs a separate lossy copy here.  PlatformAmiga's CIA-B tick calls
// sfx_voice_tick() directly.  Likewise sfx_engine_reset ($5433) is the validated
// twin in rof_native.c (its former sfx_engine_reset_native duplicate was dead and
// has been removed).
//
// flush_paula (PlatformAmiga.cpp) applies the frame's batched POKEY->Paula
// register changes; game_vbi_isr (below) calls it once per frame.  The SFX ticks
// only RECORD their writes into want[] via platform_hw_write; flushing in one
// place keeps the DMA-restart sequence on a single interrupt level.
extern "C" void flush_paula(void);

// ============================================================================
// Station / attract mode  (was station_native.cpp)
// ============================================================================

// Native 68000 reimplementations of the attract-mode 6502-transpiled routines.
//
// The rof_gen.c versions emulate 6502 CPU registers and flags on every
// "instruction", running ~60x too slowly on a 7 MHz 68000.  These replacements
// do the same mem[] mutations in plain C with no emulation overhead.
//
// Functions dropped entirely (Amiga doesn't need them):
//   pmg_update_station  — only modifies PMG RAM ($B82C area), not displayed
//   pmg_colors_station  — only writes GTIA player-colour registers (all stubbed)
//
// station_audio is NOT replaced here; it is complex and already working via
// the 6502 transpile + POKEY→Paula backend.



// ---- station_poll_start_native ----------------------------------------------
// Faithful port of the CONSOL (53279 / $D01F) read in station_init's attract
// loop.  The original idles the register with a write of $08 ($19FC: LDA #$08 /
// STA CONSOL), then on every pass round the loop reads it back and breaks out to
// the launch path when it reads exactly $06 — START down (bit0=0) with SELECT
// and OPTION up (bits 1-2 = 1):
//   1A0E:  LDA CONSOL      ; $D01F
//   1A11:  CMP #$06
//   1A13:  BEQ launch
// On the Amiga the keyboard ISR maintains CONSOL (mem[$D01F]) from the RETURN
// key (see Keyboard.cpp), so this reads identically to the 6502 original.
// Returns true when START is pressed.
extern "C" bool station_poll_start_native(void)
{
    return mem[0xD01F] == 0x06u;   // LDA CONSOL / CMP #$06 / BEQ
}

// ---- display_scroll ---------------------------------------------------------
// Advances the title-text scroll pointer (mem[$1C39/$1C3A]) and increments the
// global phase counter (mem[$008B]).  Called by station_anim_frame_native.
// Atari DLIST LMS writes and character-screen updates are skipped; the Amiga
// renders from mem[] state (mem[$008B], mem[$1C39/$1C3A]) directly.

static void display_scroll_native(void)
{
    if (mem[0x1C39] == 0 && mem[0x1C3A] == 0xB8) return;

    mem[0x008B]++;

    uint16_t ptr = (uint16_t)mem[0x1C39] | ((uint16_t)mem[0x1C3A] << 8);
    ptr -= 3;
    mem[0x1C39] = (uint8_t)ptr;
    mem[0x1C3A] = (uint8_t)(ptr >> 8);

    // Atari: if ptr < $B9BC copy 3 bytes to DLIST (LMS update) — not needed on Amiga.
    // Atari: update Mode-6/4 character screen RAM at $3300/$3600 — not needed on Amiga.
}

// ---- station_anim_frame_native ----------------------------------------------
// Title-text scroll state machine.  Manages countdown timers (mem[$008A]),
// phase index (mem[$0089]), and global phase counter (mem[$008B]).
// Table at $1DE2+X holds hold-times for each animation phase (0x12 entries).

extern "C" void station_anim_frame_native(void)
{
    uint8_t phase = mem[0x008B];
    if (phase == 0x94) return;          // animation complete, idle

    uint8_t timer = mem[0x008A];

    if (phase >= 0x82) {
        // Phase >= $82: reverse-scroll region.
        if (phase == 0x82) mem[0x0089] = 0x11;   // enter reverse: reset index

        // Count down; when timer hits 0 fire a scroll step and reload.
        if (timer != 0) { mem[0x008A] = timer - 1; return; }
        uint8_t x = mem[0x0089];
        mem[0x008A] = mem[0x1DE2 + x];
        if (x != 0) mem[0x0089] = x - 1;   // decrement index (stop at 0)
        display_scroll_native();
        return;
    }

    // Phase < $82: forward-scroll region.
    uint8_t x = mem[0x0089];
    if (x < 0x12) {
        // Still within phase table: load hold-time from table, advance index.
        if (timer != 0) { mem[0x008A] = timer - 1; return; }
        mem[0x008A] = mem[0x1DE2 + x];
        mem[0x0089] = x + 1;
        display_scroll_native();
    } else {
        // Past end of phase table: fixed 1-tick hold.
        if (timer != 0) { mem[0x008A] = timer - 1; return; }
        mem[0x008A] = 1;
        display_scroll_native();
    }
}

// ---- station_sub_1EB4_native -------------------------------------------------
// Every 3rd frame: copies a 102-byte column-stripe of bitmap data from one of
// 8 ROM source buffers (addressed via tables at $2313/$231B) into the attract
// screen RAM at $077A or $077C, with a +40-byte stride (one column per ANTIC row).
// Alternates between forward ($077A, index $009F) and reverse ($077C, $00A0)
// on each firing.

extern "C" void station_sub_1EB4_native(void)
{
    if (mem[0x009D] != 0) { mem[0x009D]--; return; }
    mem[0x009D] = 2;                       // reload: fires every 3rd frame

    mem[0x009E] ^= 0x80;                   // toggle direction bit

    uint8_t  idx;
    uint16_t dest;
    if (mem[0x009E] & 0x80) {
        // Reverse direction: decrement index (wraps 0-7).
        mem[0x00A0] = (uint8_t)((mem[0x00A0] - 1) & 0x07);
        idx  = mem[0x00A0];
        dest = 0x077C;
    } else {
        // Forward direction: increment index (wraps 0-7).
        mem[0x009F] = (uint8_t)((mem[0x009F] + 1) & 0x07);
        idx  = mem[0x009F];
        dest = 0x077A;
    }

    // Source address: lo byte from table $231B, hi byte from table $2313.
    uint16_t src = (uint16_t)mem[0x231B + idx] | ((uint16_t)mem[0x2313 + idx] << 8);

    // Copy 102 bytes to consecutive rows (stride +40 per row).
    for (int i = 0; i < 0x66; i++, dest += 0x28) {
        mem[dest] = mem[src + i];
    }
}

// ---- station_sub_1f51_native -------------------------------------------------
// Processes one animation channel rooted at offset x in the channel table at
// $2603.  Each channel has a countdown timer, a frame-cycle counter, source and
// destination pointers, and a row-copy size.  When the timer fires it copies
// rowSize bytes × numRows rows from src to dest (dest stride = 40).
//
// Channel struct offsets (all relative to x):
//   $2603+x : timer reload value
//   $2604+x : countdown timer
//   $2605+x : animation cycle length (frames)
//   $2606+x : current frame within cycle
//   $2607+x : bytes per row (copy width)
//   $2608+x : number of rows to copy
//   $2609+x : next channel link (0 = end of chain)
//   $260A+x : source advance per frame (added to src each tick)
//   $260B+x : cycle-end timer reload (negative → use POKEY RANDOM)
//   $260C+x : source lo reset value
//   $260D+x : source hi reset value
//   $260E+x : current source lo
//   $260F+x : current source hi
//   $2610+x : destination lo (fixed)
//   $2611+x : destination hi (fixed)

static uint8_t station_sub_1f51_native(uint8_t x)
{
    // Decrement countdown; if not yet zero, skip to tail (return next link).
    if (mem[0x2604 + x] != 0) {
        mem[0x2604 + x]--;
        return mem[0x2609 + x];   // L_1fdf: return next channel
    }

    // Timer fired: reload from the constant reload field.
    mem[0x2604 + x] = mem[0x2603 + x];

    // Advance frame counter and source position.
    mem[0x2606 + x]++;
    uint16_t src = (uint16_t)mem[0x260E + x] | ((uint16_t)mem[0x260F + x] << 8);
    src += mem[0x260A + x];
    mem[0x260E + x] = (uint8_t)src;
    mem[0x260F + x] = (uint8_t)(src >> 8);

    // If frame cycle complete, reset source and set a new cycle-end timer.
    if (mem[0x2606 + x] == mem[0x2605 + x]) {
        mem[0x2606 + x] = 0;
        mem[0x260E + x] = mem[0x260C + x];
        mem[0x260F + x] = mem[0x260D + x];
        src = (uint16_t)mem[0x260E + x] | ((uint16_t)mem[0x260F + x] << 8);
        uint8_t reload = mem[0x260B + x];
        if ((int8_t)reload < 0) reload = PlatformAmiga::pokeyRandom();  // RANDOM path
        mem[0x2604 + x] = reload;
    }

    // Copy rowCount rows of rowSize bytes each from src to dest (dest stride 40).
    uint16_t dest    = (uint16_t)mem[0x2610 + x] | ((uint16_t)mem[0x2611 + x] << 8);
    uint8_t  rowSize = mem[0x2607 + x];
    uint8_t  rowCount = mem[0x2608 + x];

    for (uint8_t row = 0; row < rowCount; row++) {
        for (uint8_t b = 0; b < rowSize; b++) {
            mem[dest + b] = mem[src + b];
        }
        src  += rowSize;
        dest += 0x28;   // +40 bytes per row
    }

    return mem[0x2609 + x];   // return next channel link
}

// ---- station_sub_1F48_native ------------------------------------------------
// Walks the linked list of animation channels starting at x=0, calling
// station_sub_1f51_native for each until the chain terminates (next link = 0).
// Iteration cap prevents infinite loops on uninitialised/malformed channel data.

extern "C" void station_sub_1F48_native(void)
{
    uint8_t x = 0;
    for (int guard = 0; guard < 64; guard++) {
        x = station_sub_1f51_native(x);
        if (x == 0) break;
    }
}

// ---- station_setup -----------------------------------------------------
// Mirrors the one-time initialisation that station_init performs before
// entering its attract loop.  Called once from RescueOnFractalus::initialize().
// Hardware register writes (bus_write calls) are no-ops on the Amiga; we only
// care about the mem[] state mutations and the two data-setup functions.

extern "C" void station_init_small(void);
extern "C" void display_list_build(void);
extern "C" void rle_decompress(void);

extern "C" void station_setup(void)
{
    // Zero RTCLOK timers (mem[$0012-$0014]) and misc flag.
    mem[0x0012] = 0;
    mem[0x0013] = 0;
    mem[0x0014] = 0;
    mem[0x00B7] = 0;

    // station_init_small: sets mem[$009A]=$00, mem[$0098]=$00,
    // mem[$009B]=mem[$276D+0].
    station_init_small();

    // Initial timer for station_sub_1EB4: $64 frames before first fire.
    mem[0x009D] = 0x64;

    // Frame counters used by station_anim_frame.
    mem[0x0088] = 2;
    mem[0x0087] = 0;

    // Build the attract display list at $B800 (Mode F rows, data at $0600).
    // Writes to mem[$B800+] via bus_write (RAM enabled under OS ROM on Atari).
    display_list_build();

    // Decompress the attract image/data from $1BF4 into $283E.
    // Source pointer: mem[$BB/$BC] = $1BF4.
    // Dest pointer:   mem[$BD/$BE] = $283E.
    mem[0x00BB] = 0xF4;
    mem[0x00BC] = 0x1B;
    mem[0x00BD] = 0x3E;
    mem[0x00BE] = 0x28;
    rle_decompress();

    // Source tables for station_sub_1EB4: both must be all $88, pointing every
    // entry to address $8888 which holds the real attract door-animation bitmap
    // data in ROM.  In rof_mem.bin (game_entry state) these tables have wrong
    // values; game_main_loop's init functions fix them before station_init
    // runs.  We do it here instead of running game_main_loop.
    for (int i = 0; i < 8; i++) {
        mem[0x2313 + i] = 0x88;   // source lo bytes → $8888
        mem[0x231B + i] = 0x88;   // source hi bytes → $8888
    }

    // Reset frame tick (VBI will set it non-zero; skip the Atari spin-wait).
    mem[0x0080] = 0;
}

// ============================================================================
// Standby VBI + door/tunnel cinematic drivers  (was NativeHandlers.cpp)
// ============================================================================

// Native 68000 translations of Atari VBI handler fragments used by RescueOnFractalus.
//
// The full vbi_handler_standby ($52D7) and vbi_handler_flight ($4FF5) do many things:
// hardware register writes (DMACTL, CHBASE, COLPF0-3, HPOSP, PRIOR), display-list
// management, sound dispatch, and object animation.  On the Amiga:
//   • Copper handles all hardware register writes.
//   • main.cpp VBI interrupt server handles RTCLOK ($0014/$0013/$0080).
//   • the CIA-B tick handles the audio (sfx_voice_tick, rof_native.c).
// Only the pure mem[]-state fragments below remain.


// Cockpit per-instrument dirty flags (defined in RescueOnFractalus.cpp): a writer raises the
// flag for the instrument it changed and render() decodes only that instrument's cells (no scan).
extern "C" volatile unsigned char g_ckDigits;   // score/kills/quota digits + DL-stride
extern "C" volatile unsigned char g_ckLockon;   // lock-on indicator $3491-$3497

// vbi_attract_timer_native: fragment of vbi_handler_standby @ $52D7 relevant to
// Standby.  The full handler also writes DMACTL/CHBASE/colour/HPOS registers
// (handled by Copper) and increments $0014 (handled by main.cpp VBI server).
// This fragment is lines $5335-$533A:
//   INC_M($062D)           — attract timer sub-counter
//   if Z: INC_M($00E2)     — cascade to attract_timer on 256-wrap
extern "C" void vbi_attract_timer_native(void)
{
    mem[MEM_attract_timer_sub]++;
    if (mem[MEM_attract_timer_sub] == 0) mem[MEM_attract_timer]++;
}

// The Standby title banner ($782A copy_title_text_block_to_screen) is NOT reimplemented
// here: the genuine transpiled standby loop ($62FB) calls it directly (native twin in
// src/gen/rof_native.c), copying the SFX-selected block ($0091 → $5A9F/$5AB3) into screen
// RAM $32B7-$32CA every frame.  render() picks up the alternation by shadow-comparing $32B7.

// startup_init_native: direct translation of startup_init @ $3FFA.
// Updates three cockpit digit displays based on mem[$0642], mem[$0641], mem[$0628].
// Each digit is a 2×2 block of mode-4 chars from the $4AE3 glyph table (10 entries,
// 4 bytes each: top-left, top-right, bottom-left, bottom-right).  Writes go to
// (dest+0), (dest+1), (dest+$30), (dest+$31) — the +$30 skips one mode-4 row
// (40 bytes) plus 8 column positions.  Only called when mem[$004A] != 0 (active game).
//
// Digit 1: mem[$0642] lower nibble   → dest = $33B4
// Digit 2: mem[$0641] upper nibble   → dest = $3413 (tens)
//           mem[$0641] lower nibble  → dest = $3445 (units)
// Digit 3: mem[$0628] upper nibble   → dest = $3472 (tens)
//           mem[$0628] lower nibble  → dest = $34A4 (units)
//           mem[$0628] is OR'd with $80 when mem[$062B]!=0 and !(0x0C&mem[$004B])
//
// Additionally writes mem[$33DF]/$33E0 (display-list stride control) as $9E/$9D
// when mem[$0642] is 1 or 2 and mem[$004B] passes the BIT test; otherwise $1E/$1D.
extern "C" void startup_init_native(void)
{
    // helper: write a 2×2 digit block from table $4AE3[idx*4] to dest, OR'ing flag
    auto writeDigit = [](uint16_t dest, uint8_t idx, uint8_t flag) {
        uint16_t t = (uint16_t)(0x4AE3u + (uint16_t)(idx << 2u));
        mem[dest + 0u]     = mem[t + 0u] | flag;
        mem[dest + 1u]     = mem[t + 1u] | flag;
        mem[dest + 0x30u]  = mem[t + 2u] | flag;
        mem[dest + 0x31u]  = mem[t + 3u] | flag;
        g_ckDigits = 1u;   // a digit 2×2 block changed → render decodes the digit instruments
    };

    mem[MEM_bar_col_threshold] = 0u;
    uint8_t y = 0x1Eu;
    uint8_t a = mem[MEM_game_phase_flag];

    if (a >= 1u && a < 3u && (a & mem[MEM_collision_flags]) == 0u) {
        if (mem[MEM_player_lives] != 0u) {
            // ring_push_marked(X=$14): push (a|$80) into altitude ring buffer at $0719
            uint8_t ptr = mem[MEM_alt_ring_head];
            if (ptr >= 0x20u) ptr = 0x1Fu;
            mem[0x0719u + ptr] = a | 0x80u;
            mem[MEM_alt_ring_head] = (ptr == 0u) ? 0x1Fu : (uint8_t)(ptr - 1u);
        }
        y = 0x9Eu;
    }
    // $33DF/$33E0 are in the scanned mode-4 region; only dirty when the value actually
    // changes (this runs every call, so an unconditional dirty would defeat the skip).
    if (mem[0x33DFu] != y) { mem[0x33DFu] = y; mem[0x33E0u] = (uint8_t)(y - 1u); g_ckDigits = 1u; }

    // Digit 1: lower nibble of mem[$0642], change-detected against mem[$0647]
    if (a != mem[MEM_digit_cache_647]) {
        mem[MEM_digit_cache_647] = a;
        writeDigit(0x33B4u, (uint8_t)(a & 0x0Fu), 0u);
    }

    // Digit 2: BCD byte mem[$0641], upper nibble → $3413, lower nibble → $3445
    a = mem[MEM_placed_item_count_bcd];
    if (a != mem[MEM_shield_or_damage]) {
        mem[MEM_shield_or_damage] = a;
        uint8_t hi = (uint8_t)(a >> 4u);  // upper nibble (BCD tens)
        uint8_t lo = (uint8_t)(a & 0x0Fu);                   // lower nibble (BCD units)
        writeDigit(0x3413u, hi, 0u);
        writeDigit(0x3445u, lo, 0u);
    }

    // Digit 3: BCD byte mem[$0628] with optional $80 flag
    uint8_t bf = 0u;
    if (mem[0x062Bu] != 0u && (0x0Cu & mem[MEM_collision_flags]) == 0u)
        bf = 0x80u;
    mem[MEM_bar_col_threshold] = bf;
    a = mem[MEM_score_bcd];
    if ((a | bf) != mem[MEM_digit_cache_646]) {
        mem[MEM_digit_cache_646] = a | bf;
        uint8_t hi = (uint8_t)(((a >> 2u) & 0x3Cu) >> 2u);
        uint8_t lo = (uint8_t)(a & 0x0Fu);
        writeDigit(0x3472u, hi, bf);
        writeDigit(0x34A4u, lo, bf);
    }
}

// The lock-on indicator animation ($4229 and its cluster) is now a validated native twin
// in rof_native.c (lock_on_indicator_dispatch / lock_on_indicator_tick / _step / _write_cell /
// _phase_advance / lock_on_indicator_fill_cells), byte-identical to the 6502 oracle and driving
// platform_lockon_changed() at each glyph write.  The old hand-written twin here was an
// unvalidated approximation (wrong ring-push id + wrong cell index on the step/reverse-fill
// paths); standby now calls the shared native lock_on_indicator_tick() below.
extern "C" void lock_on_indicator_tick(void);   // $4229 (rof_native.c)

// update_indicator_blink_native: direct translation of update_blink_timer_006e
// @ $4131, called via vbi_handler_flight ($4FF5) during Standby.
// Counts down mem[$006E]; on expiry reloads to $0F and sets mem[$00DE]=$4E (ON);
// when counter drops below $0A sets mem[$00DE]=$46 (OFF).
// Tail calls to vobj_* (in-game object animation) are skipped for Standby.
extern "C" void update_indicator_blink_native(void)
{
    if (mem[MEM_blink_timer] < 1) return;   // CMP #1 / BCC: skip if already 0
    mem[MEM_blink_timer]--;
    if (mem[MEM_blink_timer] == 0) {
        mem[MEM_blink_timer] = 0x0F;        // reload timer
        mem[MEM_indicator_light_state] = 0x4E;        // lights ON
    } else if (mem[MEM_blink_timer] < 0x0A) {
        mem[MEM_indicator_light_state] = 0x46;        // lights OFF (last 9 ticks of cycle)
    }
}

// --- Tunnel-ring cycle: faithful ports of the $5367 dispatcher's $0088 branch ---
//
// The original per-frame driver is launch_anim_dispatch ($5367), a strict
// priority dispatcher that runs exactly ONE action per frame:
//     if   $008D != 0  -> step_accum_sub_7e   (ring reverse — NOT ported)
//     elif $0088 != 0  -> step_accum_add_75   (tunnel ring cycle — ported below)
//     elif $0089 != 0  -> scroll_field_columns
//     elif $008B != 0  -> dl_index_dec
//     else (via $008F toggle) $008A != 0 -> scroll_terrain_dl  (door scroll)
// Only the $0088 ring branch is ported here; the lower-priority branches drive
// the ANTIC display-list machinery and are deferred to the (not-yet-done) DL pass.

// add_multibyte_a1 @ $6AB5: A (carry clear) += $00A1, carry chaining up through
// $00A2,$00A3,$00A4; returns the top byte (the caller stores it to $00A4).
// NOTE: the $A3 step reuses the freshly-written $A2 as its operand (no LDA #0
// between them) — a quirk of the original that is reproduced verbatim.
static uint8_t add_multibyte_a1(uint8_t a)
{
    uint16_t r = (uint16_t)a + mem[MEM_scroll_accum_b0];                 // CLC; ADC $A1
    mem[MEM_scroll_accum_b0] = (uint8_t)r;
    r = (uint16_t)mem[MEM_scroll_accum_b1] + (r >> 8);                   // LDA #0; ADC $A2
    mem[MEM_scroll_accum_b1] = (uint8_t)r;
    r = (uint16_t)mem[MEM_scroll_accum_b1] + mem[MEM_scroll_accum_b2] + (r >> 8);     // ADC $A3 (A = new $A2!)
    mem[MEM_scroll_accum_b2] = (uint8_t)r;
    r = (uint16_t)mem[MEM_scroll_accum_b3] + (r >> 8);                   // LDA #0; ADC $A4
    return (uint8_t)r;                                      // top byte
}

// advance_history_6a4d @ $6A4D: rotate the 6-byte colour ring $08D4-$08D9 up one
// slot (old $08D9 wraps back into $08D4 — feeds COLOR01-06); if $008D is negative
// copy $08D8 -> $0071; bump $0679[$0C] by $06CC, saturating to $FF on wrap-to-0.
// The tail-call to reorder_sprite_slot ($5629, PMG slot reordering) is irrelevant
// to the Standby tunnel and is omitted (same convention as the handlers above).
static void advance_history_6a4d(void)
{
    uint8_t top = mem[MEM_color_ring + 5];
    mem[MEM_color_ring + 5] = mem[MEM_color_ring + 4];
    mem[MEM_color_ring + 4] = mem[MEM_color_ring + 3];
    mem[MEM_color_ring + 3] = mem[MEM_color_ring + 2];
    mem[MEM_color_ring + 2] = mem[MEM_color_ring + 1];
    mem[MEM_color_ring + 1] = mem[MEM_color_ring];
    mem[MEM_color_ring] = top;
    if ((int8_t)mem[MEM_step_mode_flag] < 0) mem[MEM_display_flags] = mem[MEM_color_ring + 4];     // $008D < 0
    uint8_t s = (uint8_t)(mem[0x0679 + 0x0C] + mem[MEM_history_ring_step]);    // CLC; ADC $06CC
    mem[0x0679 + 0x0C] = s ? s : 0xFF;                          // BNE keep; else LDA #$FF
}

// draw_ring_frame_step @ $670D: the tunnel ring's "top byte >= $90" branch.
// It is the FAITHFUL tunnel→stars trigger: each invocation steps the column
// index $00A0 down by one and sets $0088 = $00A0 + 1, so once $00A0 wraps from
// $00 to $FF the gate $0088 becomes 0 and the ring stops cycling — display_setup
// then advances to the stars/space phase.  init_row_coords_9c seeds $00A0 = $13
// (=19), so the tunnel runs for 20 threshold crossings before it ends.
//
//   $670D  LDY $00A0
//   $670F  CPY #$06 / BMI $671E   ; $00A0 < 6 (signed) -> clear $08D8, else DRAW
//   $6713  LDA $6E0F,Y / STA $0096 / JSR draw_symmetric_span_loop ($6642)
//   $671E  (else) LDA #$00 / STA $08D8
//   $6723  DEC $00A0 / CLC / LDA $00A0 / ADC #$01 / STA $0088
//
// So the GEOMETRIC clear (expanding black frames from the centre) runs for the FIRST
// 14 crossings ($00A0 19->6); the $08D8 palette touch is only the last 6 ($00A0 5->0).
// The draw writes black (pen $0094=0) into the GTIA field at $2000, which the Amiga
// re-decodes (g_tunnelFieldDirty) so the rings visibly clear from the middle out.
static void draw_symmetric_span_loop(void);   // fwd decl (defined below)

// Set by draw_ring_frame_step when it draws a black ring-clear frame into the GTIA field at
// $1000, so RescueOnFractalus re-decodes ONLY the band it just wrote — not the whole 86-row
// field (> 1 PAL frame on the 68000, freezes the ring cycle) and not a shadow scan of the
// cumulative extent.  draw_symmetric_span_loop expands the rectangle [$009C..$009F] outward
// by $0096 on each side; the newly-written cells are exactly the picture-frame band between
// the rectangle BEFORE the loop (inner = the previous outer) and AFTER (outer).  Publish both:
//   g_tunRowLo/Hi     = outer rows ($009F..$009E after)   [also reused as the reveal full extent]
//   g_tunInRowLo/Hi   = inner rows ($009F..$009E before)
//   g_tunColLpx/Rpx   = outer left/right PIXEL cols ($009C/$009D after)
//   g_tunInColLpx/Rpx = inner left/right PIXEL cols ($009C/$009D before)
//   g_tunBandMode     = 1 → decode the band (exit clear); 0 → decode the full extent (reveal)
// RescueOnFractalus::decodeTunnelBand turns these into four thin decode strips.  Cleared by
// RescueOnFractalus after it re-decodes.
extern "C" volatile uint8_t g_tunnelFieldDirty = 0;
extern "C" volatile uint8_t g_tunRowLo = 0, g_tunRowHi = 0;       // outer rows $009F .. $009E after
extern "C" volatile uint8_t g_tunInRowLo = 0, g_tunInRowHi = 0;   // inner rows $009F .. $009E before
extern "C" volatile uint8_t g_tunColLpx = 0, g_tunColRpx = 0;     // outer $009C / $009D after
extern "C" volatile uint8_t g_tunInColLpx = 0, g_tunInColRpx = 0; // inner $009C / $009D before
extern "C" volatile uint8_t g_tunBandMode = 0;                    // 1 = band decode, 0 = full extent

static void draw_ring_frame_step(void)
{
    uint8_t a0 = mem[0x00A0];
    if ((int8_t)a0 >= 6) {                          // CPY #$06; BMI -> $00A0 >= 6 = DRAW
        // Snapshot the rectangle BEFORE the loop expands it (= this band's inner edge).
        uint8_t inTop = mem[0x009F], inBot = mem[0x009E];
        uint8_t inL   = mem[0x009C], inR   = mem[0x009D];
        mem[0x0096] = mem[0x6E0F + a0];             // LDA $6E0F,Y; STA $0096
        draw_symmetric_span_loop();                 // JSR $6642 (steps $009C--/$009D++/$009E++/$009F--)
        g_tunRowLo = mem[0x009F];                   // outer rows (top .. bottom) after
        g_tunRowHi = mem[0x009E];
        g_tunInRowLo = inTop; g_tunInRowHi = inBot; // inner rows (before)
        g_tunColLpx = mem[0x009C]; g_tunColRpx = mem[0x009D];   // outer cols after
        g_tunInColLpx = inL;       g_tunInColRpx = inR;         // inner cols before
        g_tunBandMode = 1;
        g_tunnelFieldDirty = 1;
    } else {
        mem[0x08D8] = 0u;                           // $671E: LDA #$00; STA $08D8
    }
    mem[0x00A0]--;                                  // DEC $00A0
    mem[MEM_vbi_flags] = (uint8_t)(mem[0x00A0] + 1u);   // CLC; LDA $00A0; ADC #$01; STA $0088
}

// step_accum_add_75 @ $6A38: add $75 into the accumulator; if the resulting top byte
// ($A4) is unchanged, do nothing; otherwise store it, and — when the top byte >= $90
// — step the message column (draw_ring_frame_step, the tunnel-exit clear + the
// stars trigger), then ALWAYS rotate the ring (advance_history_6a4d).  Per the real
// $6A38: `CMP #$90 / BCC $6A4D / JSR $670D` falls THROUGH into the rotation at $6A4D
// — the two are additive, NOT exclusive, so the palette keeps cycling while the
// tunnel clears.  (The earlier if/else port froze the cycle during the clear.)
static void step_accum_add_75(void)
{
    uint8_t a = add_multibyte_a1(0x75);
    mem[MEM_scroll_accum_b3] = a;
    if (a == mem[MEM_scroll_accum_prev]) return;     // CMP $A5; BEQ -> top byte unchanged
    mem[MEM_scroll_accum_prev] = a;
    if (a >= 0x90u) draw_ring_frame_step();      // CMP #$90; BCS -> JSR $670D
    advance_history_6a4d();                        // $6A4D: ring rotation, ALWAYS runs
}

// ---- door display-list scroll: native ports of the $6953 family -------------
// The "doors" open by scrolling the ANTIC display list at $3000: each scanline's
// 3-byte LMS entry (mode byte + 16-bit source address) is shifted one slot, then a
// fresh leading row pointer is pushed at each edge.  These run on the live DL in
// mem[$3000+]; the door-open progress is the $008A counter they decrement.
// (Raw $00xx / $30xx literals kept here to mirror the 6502 DL layout 1:1.)

// dl_lms_scroll_up @ $69A9: shift top-half LMS entries from $300C,X up to $3009,X
// (3-byte stride) until X reaches the top index $0097.
static void dl_lms_scroll_up(void)
{
    uint8_t x = 1, top = mem[0x0097];
    while (x != top) {
        mem[0x3009 + x] = mem[0x300C + x]; x++;
        mem[0x3009 + x] = mem[0x300C + x]; x += 2;
    }
}

// dl_lms_scroll_down @ $69C3: shift bottom-half LMS entries from $3087,Y down to
// $308A,Y until Y reaches the bottom index $0098.
static void dl_lms_scroll_down(void)
{
    uint8_t y = 0x80, bot = mem[0x0098];
    while (y != bot) {
        mem[0x308A + y] = mem[0x3087 + y]; y--;
        mem[0x308A + y] = mem[0x3087 + y]; y -= 2;
    }
}

// dl_lms_push_top @ $6973: write the 16-bit top push pointer $0080/$0081 into the
// top LMS entry at $300A,X (net X -= 3), then advance the pointer up one row (-$2E).
static uint8_t dl_lms_push_top(uint8_t x)
{
    mem[0x300A + x] = mem[0x0081]; x--;
    mem[0x300A + x] = mem[0x0080];
    uint16_t p = (uint16_t)(mem[0x0080] | (mem[0x0081] << 8));
    p = (uint16_t)(p - 0x2E);
    mem[0x0080] = (uint8_t)p; mem[0x0081] = (uint8_t)(p >> 8);
    return (uint8_t)(x - 2);
}

// dl_lms_push_bottom @ $698E: write the 16-bit bottom push pointer $0082/$0083 into
// the bottom LMS entry at $3089,Y (net Y += 3), then advance the pointer down (+$2E).
static uint8_t dl_lms_push_bottom(uint8_t y)
{
    mem[0x3089 + y] = mem[0x0082]; y++;
    mem[0x3089 + y] = mem[0x0083];
    uint16_t p = (uint16_t)(mem[0x0082] | (mem[0x0083] << 8));
    p = (uint16_t)(p + 0x2E);
    mem[0x0082] = (uint8_t)p; mem[0x0083] = (uint8_t)(p >> 8);
    return (uint8_t)(y + 2);
}

// scroll_terrain_dl @ $6953: one door-open step.  Decrement $008A; while it is
// still non-zero scroll both DL halves apart; on the step that reaches 0 set the
// reload $008C=8 instead.  Then push the leading row into each half.
static void scroll_terrain_dl(void)
{
    if (--mem[MEM_terrain_scroll_counter] != 0) {     // DEC $008A; BNE
        dl_lms_scroll_down();
        dl_lms_scroll_up();
    } else {
        mem[MEM_terrain_scroll_reload] = 8;            // $008C = 8
    }
    mem[0x0098] = dl_lms_push_bottom(mem[0x0098]);   // LDY $0098 / push / STY $0098
    mem[0x0097] = dl_lms_push_top(mem[0x0097]);      // LDX $0097 / push / STX $0097
}

// launch_anim_dispatch @ $5367 (Standby subset): the per-frame priority dispatcher
// that runs exactly ONE action.  $0088 (ring) outranks $0089 (column scroll) outranks
// $008A (door scroll), so they are sequential — the doors scroll with a static tunnel
// while $0088==0, the ring animates once $0088 is armed (after the doors finish
// opening; the hand-off lives in RescueOnFractalus::update), and once the tunnel ends
// ($0088 wraps to 0) the $0089 column scroll drives the stars/planet phase.
// The $008C/clear_slot branch drives the doors/tunnel windscreen-corner reveal (the
// green canopy-post wedge receding top-down); the $008B branch is inert in Standby.
// The transpile's 6502 register file (src/cpu/cpu.h: `Cpu6502 cpu`) — mirrored
// as a POD (see launch_native.cpp for why we don't #include cpu.h).
extern "C" { typedef struct { uint8_t A, X, Y, S, N, V, Z, C, I, D; } Cpu6502; extern Cpu6502 cpu; }
extern "C" void scroll_field_columns(void);  // $6AEE transpiled (entered with A = $0089)
extern "C" void step_accum_sub_7e(void);      // $6A8F transpiled — the $008D DL-construction step
extern "C" void launch_anim_dispatch_native(void)
{
    // $008D (step_mode_flag): the DL-CONSTRUCTION step, NOT a "reverse ring" — the Atari
    // $5367 does `if ($008D != 0) JMP $6A8F` (step_accum_sub_7e), which subtracts $7E from the
    // scroll accumulator, arms the next DL row (INC $008E) and counts $008D down to 0.  The
    // standby REBUILD path in display_setup (the $006C!=0 branch, taken when re-entering Standby
    // from the Title/level-select) spins on this handshake (`while(step_mode_flag) ds_frame()`),
    // so stubbing it as a bare return hung the rebuild at black doors (no green fade).  Drive it.
    if (mem[MEM_step_mode_flag]) { step_accum_sub_7e(); return; }   // $008D: JMP $6A8F
    if (mem[MEM_vbi_flags]) { step_accum_add_75(); return; }  // $0088: tunnel ring cycle
    if (uint8_t g = mem[MEM_terrain_state]) {            // $0089: scroll star/planet columns
        cpu.A = g; scroll_field_columns();                 // $6AEE: $0089 gate in A (>=4 advances accum)
        return;
    }
    if (mem[MEM_dl_src_index]) return;                        // $008B: dl_index_dec (unused)
    uint8_t ph = mem[MEM_sfx_toggle_8F];                // $008F every-other-frame toggle
    mem[MEM_sfx_toggle_8F] = (uint8_t)(ph >> 1);        // LSR $008F
    if (ph & 1u) return;                                     // carry set -> skip this frame
    mem[MEM_sfx_toggle_8F]++;                           // INC $008F
    // $008C: windscreen-corner reveal.  clear_slot_0c87_0d87 ($6A27) recedes the green
    // canopy-post quad-player wedge one scanline per (every-other) frame so the tunnel
    // shows through top-down.  scroll_terrain_dl arms $008C=8 when the doors finish; this
    // branch was missing from the native dispatch, so the wedge never receded on the Amiga.
    if (mem[MEM_terrain_scroll_reload]) {                      // LDA $008C; BEQ skip
        uint8_t h = --mem[MEM_terrain_scroll_reload];          // DEC $008C (8 -> 0)
        uint8_t y = (uint8_t)(8 - h);                        // index = 8 - $008C
        mem[0x0C87 + y] = 0;                                 // clear left canopy-post player line ($0C88..)
        mem[0x0D87 + y] = 0;                                 // clear right canopy-post player line ($0D88..)
    }
    if (mem[MEM_terrain_scroll_counter] == 0) return;          // $008A: doors already fully open
    scroll_terrain_dl();
}

// ---- procedural tunnel rings: native port of draw_frame_pattern_seq ($65FB) ---
// The concentric tunnel rings are DRAWN (not pre-baked): nested rectangle frames
// are filled into the GTIA-mode-10 screen at $2000 (per-row base address table
// $073D lo / $0793 hi, 46-byte stride), with the pen cycling 1..6.  Run once at
// scene init; the $2000 image is then decoded to the tunnel bitmap, the same path
// the terrain uses.  All scratch is in zero page ($0080-84/$0092/$0094/$0096/
// $009C-A0/$00B7-B9/$00DF) — re-seeded by the genuine display_setup launch path
// before the door scroll runs.

// plot_masked_pixel @ $66DE: OR then AND a pen into the screen byte at
// ($0080)+yByte using the mask tables $66E9 (set) / $66FB (clear), indexed by x.
static void plot_masked_pixel(uint8_t yByte, uint8_t x)
{
    uint16_t a = (uint16_t)((mem[0x0080] | (mem[0x0081] << 8)) + yByte);
    mem[a] = (uint8_t)((mem[a] | mem[0x66E9 + x]) & mem[0x66FB + x]);
}

// plot_pixel_masked @ $66D5: plot pen $0094 into pixel column `col` of the row
// pointer $0080/$0081 — high nibble for even columns, low nibble for odd.  (The
// 6502 LSR leaves carry = col bit0, so the odd-column "ADC #8" actually adds 9.)
static uint8_t plot_pixel_masked(uint8_t col)
{
    uint8_t x = (col & 1u) ? (uint8_t)(mem[0x0094] + 9u) : mem[0x0094];
    plot_masked_pixel((uint8_t)(col >> 1), x);
    return x;
}

// fill_horizontal_span @ $665D: solid-fill the pattern byte $00B9 across the byte
// range [$009C>>1 .. $009D>>1] in both the top ($009E) and bottom ($009F) rows.
static void fill_horizontal_span(void)
{
    uint8_t et = mem[0x009E], eb = mem[0x009F];
    uint16_t topp = (uint16_t)(mem[0x073D + et] | (mem[0x0793 + et] << 8));
    uint16_t botp = (uint16_t)(mem[0x073D + eb] | (mem[0x0793 + eb] << 8));
    uint8_t b0 = (uint8_t)(mem[0x009C] >> 1);            // $0082
    uint8_t b1 = (uint8_t)(mem[0x009D] >> 1);            // $0083
    if (mem[0x009D] & 1u) b0++; else b1--;               // trim one edge byte by parity
    uint8_t fill = mem[0x00B9];
    int cnt = (int)b1 - (int)b0;                         // $00DF
    uint8_t y = b1;
    do { mem[topp + y] = fill; mem[botp + y] = fill; y--; } while (--cnt >= 0);
}

// fill_vertical_span @ $669C: plot the left ($009C) and right ($009D) edge pixels
// down every row from $009F to $009E (the right pixel reuses the left's mask).
static void fill_vertical_span(void)
{
    mem[0x0084] = mem[0x009F];
    int cnt = (uint8_t)(mem[0x009E] - mem[0x009F]);      // $00DF
    do {
        uint8_t y = mem[0x0084];
        mem[0x0080] = mem[0x073D + y];
        mem[0x0081] = mem[0x0793 + y];
        uint8_t x = plot_pixel_masked(mem[0x009C]);                 // left edge
        plot_masked_pixel((uint8_t)(mem[0x009D] >> 1), x);    // right edge, same mask
        mem[0x0084]++;
    } while (--cnt >= 0);
}

// draw_symmetric_span_loop @ $6642: draw $0096 nested rectangle frames, each one
// step larger ($009C--/$009D++/$009E++/$009F--), in the current pen $0094.
static void draw_symmetric_span_loop(void)
{
    mem[0x00B9] = (uint8_t)(mem[0x0094] | mem[0x66E9 + mem[0x0094]]);  // both pixels = pen
    do {
        fill_horizontal_span();
        fill_vertical_span();
        mem[0x009C]--; mem[0x009D]++; mem[0x009E]++; mem[0x009F]--;
    } while (--mem[0x0096] != 0);
}

// draw_frame_guide_columns @ $6620: three full-height vertical lines at columns $009C,
// $009D and $009D+1 (the tunnel's vanishing-point verticals), in pen $0094.
static void draw_frame_guide_columns(void)
{
    mem[0x0092] = 0x55;
    mem[0x00A0] = (uint8_t)(mem[0x009D] + 1);
    do {
        uint8_t y = mem[0x0092];
        mem[0x0080] = mem[0x073D + y];
        mem[0x0081] = mem[0x0793 + y];
        plot_pixel_masked(mem[0x009C]);
        plot_pixel_masked(mem[0x009D]);
        plot_pixel_masked(mem[0x00A0]);
    } while (((int8_t)--mem[0x0092]) >= 0);
}

// init_row_coords_9c @ $6DDF: seed the symmetric-span coordinates (centre of the
// terrain region: rows 42/43, columns 46/48) and the 20-frame outer count.
static void init_row_coords_9c(void)
{
    mem[0x009C] = 0x2E; mem[0x009D] = 0x30;
    mem[0x009E] = 0x2B; mem[0x009F] = 0x2A;
    mem[0x00A0] = 0x13;
}

// tunnel_ring_arm_native: reseed the message-column / span coordinates the way the
// Atari tunnel setup does at $647D-$6480 (JSR init_row_coords_9c; $0094=0) just
// before it arms $0088=1 at $64A8.  The Amiga ran init_row_coords_9c earlier (in
// decodeTunnelRings, which left $00A0=$FF), so we re-seed here when arming the ring
// — otherwise draw_ring_frame_step would start from $FF and never clear $0088 in
// the expected 20 crossings.
extern "C" void tunnel_ring_arm_native(void)
{
    mem[0x009C] = 0x2E; mem[0x009D] = 0x30;
    mem[0x009E] = 0x2B; mem[0x009F] = 0x2A;
    mem[0x00A0] = 0x13;     // 19 → 20 message-column crossings before $0088 clears
    mem[0x0094] = 0x00;     // pen index reset ($6480)
}

// draw_frame_pattern_seq @ $65FB: draw 20 concentric frame groups (thickness from
// the $6E0F table, pen cycling 1..6), then the vanishing-point verticals.
// draw_tunnel_rings_native clears the $2000 GTIA field first so the rings land on
// a black background (pen 0); RescueOnFractalus decodes the result into the tunnel bitmap.
extern "C" void draw_tunnel_rings_native(void)
{
    for (uint16_t i = 0; i < 86u * 46u; i++) mem[0x2000 + i] = 0u;   // clear screen field
    init_row_coords_9c();
    mem[0x0094] = 1u;
    do {
        mem[0x0096] = mem[0x6E0F + mem[0x00A0]];
        draw_symmetric_span_loop();
        if (++mem[0x0094] == 7u) mem[0x0094] = 1u;
    } while (((int8_t)--mem[0x00A0]) >= 0);
    mem[0x0094]--;
    draw_frame_guide_columns();
}

// standby_vbi_native: the faithful per-frame body of vbi_handler_standby ($52D7),
// run from the real INTB_VERTB interrupt via game_vbi_isr() during the Standby
// screen + the launch cinematic (the Atari ran $52D7 in the VBI throughout).
// HW-display writes ($52D7-$5332: PMG positions, colours, GRAFP, HPOS) are SKIPPED —
// the Amiga copper owns the display.  Order mirrors $5333 onward:
//   $5333  INC $0014 (RTCLOK)            -> done by the ISR before this call
//   $5335  attract-timer cascade         -> vbi_attract_timer_native
//   $5398  attract input poll            -> SKIPPED: Atari $D01F/$D010/$D300 reads
//          (resets the attract timeout on input) return neutral on the Amiga, where
//          input is the keyboard ISR, so it would only ever no-op.
//   $5367  launch_anim_dispatch          -> the door/tunnel/scroll cinematic driver
//          (self-gated on $0088/$0089/$008A/$008B/$008D — inert on the static screen)
//   $5342  lock_on_indicator_tick every other frame (LSR/INC $0643 gate)
//   $534D  SFX tick ($70F9)              -> runs on CIA-B Timer A instead (main.cpp)
//   $5359  music_player_tick ($7253)      -> ported below, gated on $0655 (see note)
//          sfx_voice_envelope_tick ($548D) -> ported below, gated on $060B
extern "C" void sfx_voice_envelope_tick(void);   // $548D: SFX voice engine + $0719 ring drain
extern "C" void music_player_tick(void);         // $7253: note-stream music player (transpiled)

extern "C" void standby_vbi_native(void)
{
    vbi_attract_timer_native();              // $5335
    launch_anim_dispatch_native();           // $5367
    // $5359  music_player_tick ($7253): the note-stream tune player (level-start,
    // game-over/results jingles).  Separate engine from the CIA-B SFX sequencer
    // ($70F9) that carries the standby/attract THEME — that one is gated on $00E7
    // and undisturbed by this.  The Atari ran $7253 in this VBI tail gated on the
    // music-active flag $0655 (set by music_init_state $7238); mirror that here so
    // the game-over tune actually plays.  Runs at 50 Hz (one VBI) like the Atari;
    // its own tempo counter ($0653) paces the notes.  POKEY AUDF/AUDC writes route
    // through platform_hw_write into the Paula want[] table and are applied by
    // flush_paula in game_vbi_isr (same path as the SFX envelope below).
    if (mem[0x0655]) music_player_tick();
    // $548D SFX voice engine — the Atari ran it in this VBI tail too.  Gate on
    // $060B (=$23 once the launch cinematic begins, 0 during pure attract) so the
    // attract music (CIA-B sequencer) is undisturbed but the START/doors/tunnel
    // launch effects get drained from the $0719 ring to POKEY -> Paula.
    if (mem[0x060B]) sfx_voice_envelope_tick();
    uint8_t g = mem[MEM_lock_on_indicator_tick_parity];   // $5342: LSR $0643 / BCS skip / ... / INC
    mem[MEM_lock_on_indicator_tick_parity] = (uint8_t)(g >> 1);
    if (!(g & 1u)) {                         // carry clear -> run, then INC
        lock_on_indicator_tick();                  // $4229 (native twin, rof_native.c)
        mem[MEM_lock_on_indicator_tick_parity]++;
    }
}

// ============================================================================
// In-game flight  (was flight_native.cpp)
// ============================================================================

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


// ---- per-frame profiler ------------------------------------------------------
// g_flightProf accumulates per-phase deltas; read it from the debugger.
volatile struct FlightProf g_flightProf = { 0 };
// terrain_draw_frame object-loop sub-phase probe (rof_native.c, -DROF_TDRAW_PROF):
// beam lines spent in the fractal subdivision vs the projection+object-plot.
extern "C" unsigned long g_tdSubdiv = 0, g_tdProjPlot = 0, g_tdFrames = 0;
// terrain-draw shape counters (-DROF_TDRAW_PROF): how many times the hot inner ops run
// per flight (cumulative; divide by g_tdFrames).  Tells where the subdiv cost actually is.
extern "C" unsigned long g_tdMidpoints = 0, g_tdPlots = 0, g_tdRasterCalls = 0, g_tdSubdivCalls = 0;
// beam ticks spent inside terrain_column_rasterize (a subset of g_tdSubdiv) — splits the
// fractal recursion cost from the leaf-rasterization cost.
extern "C" unsigned long g_tdRaster = 0;
// terrain_column_rasterize shape split: phase-2 bisect-push steps vs. DRAW() attempts.
extern "C" unsigned long g_tdRasBisect = 0, g_tdRasDraw = 0;
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

extern "C" {
// Flight init subroutines (game_entry $3E12-$3EA6) — genuine transpiled:
void clear_pm_state(void);            // $3FBF: zero player/missile state + PCOLR shadows
void wait_frames_1(void);              // $3CC3: zero colour shadows
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
void terrain_collision_and_silhouette(void);         // $AE53: terrain collision + final column fill (X)
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
void sfx_voice_envelope_tick(void);          // $548D: in-game SFX voice engine + ring drain (Atari VBI tail $534D)
// Full in-flight VBI ($4FF5).  Faithful transpiled handler; its calls to
// flight_control_integrate / update_terrain_scanline_proj resolve to the native twins
// (same symbol), so running it verbatim IS "full faithful VBI, native motion spliced in":
// PMG setup, instrument drawing (build_player2_sprite / draw_player3_object / the P3
// indicator stripe), the atmosphere colour ramp ($51C8: altitude→$00DA/$00DC/$00DB/$00DD),
// the cockpit message dispatch (MANUAL), the gauges and the score all run as on the Atari.
void vbi_handler_flight(void);           // $4FF5
void reorder_sprite_slot(void);          // $5614: voice-priority mixer — assigns a POKEY channel ($0705) to slot cpu.Y
}

// flight_vbi_native: one in-flight VBI frame ($4FF5).  Runs the FULL faithful transpiled
// handler — its flight_control_integrate / update_terrain_scanline_proj calls resolve to
// the native twins, so this is "full faithful VBI, native motion spliced in".  This draws
// the cockpit HUD (player gauges, P3 indicator, missiles/wing bars), the atmosphere colour
// ramp, the MANUAL message and the score, none of which the old curated subset did.  RTCLOK
// ($0014/$0012) is advanced here (as on the Atari); PlatformAmiga::renderFrame skips its
// own RTCLOK advance for $4FF5 so the clock is not double-counted.
#ifdef ROF_FLIGHT_PROBE
// Cumulative flight-VBI ISR beam-lines, subtracted by rof_native.c's FP_TIME so the main-loop
// phase buckets (clear/setup/collision/draw) exclude ISR firings that land in their windows.
extern "C" volatile unsigned long g_isrBeamLines = 0;
#endif
#ifdef ROF_FLIGHT_PROBE
// ZP write-set audit: which of $00-$FF does the flight VBI ($4FF5) change?  Snapshot ZP
// before each firing, OR the diff into g_vbiZpTouched so the gdb harness can read the union
// over the whole run.  (Catches net changes; a write-then-restore within one firing is
// invisible here — cross-check those statically.)  g_vbiZpFirings = firing count.
extern "C" volatile unsigned char g_vbiZpTouched[256] = {0};
extern "C" volatile unsigned long g_vbiZpFirings = 0;
#endif
extern "C" void flight_vbi_native(void)
{
    unsigned short a = beam_line();      // sub-frame profiler timer (FULL probe-ISR span)
#ifdef ROF_FLIGHT_PROBE
    // ZP write-set audit (rasterizer-alias safety, see CLAUDE.md): snapshot ZP before the
    // handler, diff after.  These two 256-iter loops are PROBE-ONLY overhead (~70 beam
    // lines/firing of volatile mem[] traffic) — absent from the real probe-off build.  So
    // bracket the HANDLER separately (a2..b2) and report THAT as isrLines (the real VBI cost),
    // while g_isrBeamLines keeps the FULL a..b span: the probe ISR really does steal that
    // wall-time during a probe run, so subtracting the full span is what makes the main-loop
    // FP_TIME buckets (draw/setup/clear) match the real game's pure main-loop compute.
    unsigned char zpSnap[256];
    for (int i = 0; i < 256; i++) zpSnap[i] = mem[i];
#endif
    unsigned short a2 = beam_line();     // HANDLER-only span start
    vbi_handler_flight();                // $4FF5 — the whole handler
    unsigned short b2 = beam_line();     // HANDLER-only span end
#ifdef ROF_FLIGHT_PROBE
    for (int i = 0; i < 256; i++) if (mem[i] != zpSnap[i]) g_vbiZpTouched[i] = 1;
    g_vbiZpFirings++;
#endif
    unsigned short b = beam_line();
    unsigned short dHandler = (b2 >= a2) ? (unsigned short)(b2 - a2)
                                         : (unsigned short)(b2 + 313 - a2);  // PAL wrap
    g_flightProf.isrLines += dHandler;   // report the real handler cost (excludes the ZP audit)
    g_flightProf.isrCalls++;
#ifdef ROF_FLIGHT_PROBE
    unsigned short dFull = (b >= a) ? (unsigned short)(b - a)
                                    : (unsigned short)(b + 313 - a);
    g_isrBeamLines += dFull;             // subtract FULL probe-ISR span from main-loop buckets
#endif
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
    if      (vbi == 0x4FF5) { flight_vbi_native();    // $4FF5 in-flight VBI
                              PlatformAmiga::flightShotTick(); }  // laser sprite @ 50Hz (VBI, faithful)
    else if (vbi == 0x1B30) vbi_handler_station();   // $1B30 attract VBI (sets $0080 + RTCLOK)
    else                    standby_vbi_native();    // $52D7 standby/launch VBI (and fallback)
    cpu = saved;                                    // == XITVBV PLA;TAY;PLA;TAX;PLA
    // Apply this frame's batched POKEY→Paula writes — from the CIA-B music tick
    // (sfx_voice_tick) AND the in-game SFX engine (sfx_voice_envelope_tick, run in the VBI
    // bodies above).  One flush per frame: it silences released channels (vol=0) and
    // starts new notes/SFX, so without it stuck notes never stop and SFX never sound.
    flush_paula();
    PlatformAmiga::noiseTick();                     // refresh a 128-byte slice of the noise sample (cheap)
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
// the mixer + flight_control_integrate + sfx_voice_envelope_tick (all ported) sustain it.
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
