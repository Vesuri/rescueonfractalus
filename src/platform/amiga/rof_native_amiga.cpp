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
#ifdef ROF_FLIGHT_PROBE
// How many of the five 2×2 digit blocks actually change per g_ckDigits fire, and how often the
// $33DF/$33E0 stride pair alone raises the flag.  Measured 2026-08-09: exactly ONE block per fire,
// zero stride flips — which is why render() now decodes per block instead of all five.
extern "C" volatile unsigned long g_ckWdigCalls = 0, g_ckStrideFlips = 0, g_ckSiNative = 0;
#endif
// Which of the six digit groups changed (0-4 = the 2×2 blocks $33B4/$3413/$3445/$3472/$34A4,
// 5 = the $33DF/$33E0 DL-stride pair).  Defined in RescueOnFractalus.cpp next to the decoder.
extern "C" void rof_cockpit_digit_dirty(unsigned char slot);

extern "C" void startup_init_native(void)
{
#ifdef ROF_FLIGHT_PROBE
    g_ckSiNative++;
#endif
    // helper: write a 2×2 digit block from table $4AE3[idx*4] to dest, OR'ing flag.  `slot` is the
    // block's index in the decoder's registry — only the block that changed gets re-decoded.
    auto writeDigit = [](uint16_t dest, uint8_t idx, uint8_t flag, uint8_t slot) {
#ifdef ROF_FLIGHT_PROBE
        g_ckWdigCalls++;
#endif
        uint16_t t = (uint16_t)(0x4AE3u + (uint16_t)(idx << 2u));
        mem[dest + 0u]     = mem[t + 0u] | flag;
        mem[dest + 1u]     = mem[t + 1u] | flag;
        mem[dest + 0x30u]  = mem[t + 2u] | flag;
        mem[dest + 0x31u]  = mem[t + 3u] | flag;
        rof_cockpit_digit_dirty(slot);   // this block changed → render decodes its 4 cells
    };

    mem[MEM_bar_col_threshold] = 0u;
    uint8_t y = 0x1Eu;
    uint8_t a = mem[MEM_range_to_pilot];

    if (a >= 1u && a < 3u && (a & mem[MEM_collision_flags]) == 0u) {
        if (mem[MEM_flight_mode_state] != 0u) {
            // ring_push_marked(X=$14): push (a|$80) into altitude ring buffer at $0719
            uint8_t ptr = mem[MEM_alt_ring_head];
            if (ptr >= 0x20u) ptr = 0x1Fu;
            // $4016 LDX #$14; $4018 JSR $5815 (ring_push_marked): push X|$80 = $94 = event
            // $14 (range-to-pilot beep) — a CONSTANT, NOT the range value `a`.  The old
            // `a | 0x80` used the $0642 range digit as the event id, so range 1 pushed $81
            // (event $01, poly4 = the "wrong sound") and range 2 pushed $82.
            mem[0x0719u + ptr] = 0x14u | 0x80u;
            mem[MEM_alt_ring_head] = (ptr == 0u) ? 0x1Fu : (uint8_t)(ptr - 1u);
        }
        y = 0x9Eu;
    }
    // $33DF/$33E0 are in the scanned mode-4 region; only dirty when the value actually
    // changes (this runs every call, so an unconditional dirty would defeat the skip).
    if (mem[0x33DFu] != y) {
        mem[0x33DFu] = y; mem[0x33E0u] = (uint8_t)(y - 1u); rof_cockpit_digit_dirty(5u);
#ifdef ROF_FLIGHT_PROBE
        g_ckStrideFlips++;
#endif
    }

    // Digit 1: lower nibble of mem[$0642], change-detected against mem[$0647]
    if (a != mem[MEM_digit_cache_647]) {
        mem[MEM_digit_cache_647] = a;
        writeDigit(0x33B4u, (uint8_t)(a & 0x0Fu), 0u, 0u);
    }

    // Digit 2: BCD byte mem[$0641], upper nibble → $3413, lower nibble → $3445
    a = mem[MEM_placed_item_count_bcd];
    if (a != mem[MEM_shield_or_damage]) {
        mem[MEM_shield_or_damage] = a;
        uint8_t hi = (uint8_t)(a >> 4u);  // upper nibble (BCD tens)
        uint8_t lo = (uint8_t)(a & 0x0Fu);                   // lower nibble (BCD units)
        writeDigit(0x3413u, hi, 0u, 1u);
        writeDigit(0x3445u, lo, 0u, 2u);
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
        writeDigit(0x3472u, hi, bf, 3u);
        writeDigit(0x34A4u, lo, bf, 4u);
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

// --- Tunnel-ring cycle + door scroll: the $5367 dispatcher body -------------
//
// launch_anim_dispatch ($5367) is a strict priority dispatcher that runs exactly
// ONE action per frame:
//     if   $008D != 0  -> step_accum_sub_7e   (DL-construction step)
//     elif $0088 != 0  -> step_accum_add_75   (tunnel ring cycle)
//     elif $0089 != 0  -> scroll_field_columns (stars/planet column scroll)
//     elif $008B != 0  -> dl_index_dec         (unused in Standby)
//     else (via $008F toggle) $008C reveal / $008A != 0 -> scroll_terrain_dl (doors)
// launch_anim_dispatch_native (below) is the Amiga per-frame dispatch entry.
//
// The helper 6502 routines it drives (step_accum_add_75/$6A38, draw_ring_frame_step/
// $670D, add_multibyte_a1/$6AB5, advance_history_6a4d/$6A4D, scroll_terrain_dl/$6953,
// dl_lms_*) are pure mem[] 6502 logic, not Amiga-specific, so they now live as faithful
// native twins in src/gen/rof_native.c (VALIDATE_FUNCS).  draw_ring_frame_step's Amiga
// tunnel dirty-band publish (the g_tun* globals, formerly defined here) is guarded there
// under #ifdef ROF_PLATFORM_AMIGA; advance_history_6a4d skips its sfx_reorder_voice_slot tail
// on Amiga.  The g_tun* globals are now defined by their writer's TU (rof_native.c).

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
extern "C" void step_accum_add_75(void);      // $6A38 native twin (rof_native.c) — tunnel ring cycle
extern "C" void scroll_terrain_dl(void);      // $6953 native twin (rof_native.c) — one door-open step
extern "C" void dl_index_dec(void);           // $69E3 native twin (rof_native.c) — level-select DL scroll step
extern "C" void launch_anim_dispatch_native(void)
{
    // $008D (step_mode_flag): the DL-CONSTRUCTION step, NOT a "reverse ring" — the Atari
    // $5367 does `if ($008D != 0) JMP $6A8F` (step_accum_sub_7e), which subtracts $7E from the
    // scroll accumulator, arms the next DL row (INC $008E) and counts $008D down to 0.  The
    // standby REBUILD path in boot_standby_launch_driver (the $006C!=0 branch, taken when re-entering Standby
    // from the Title/level-select) spins on this handshake (`while(step_mode_flag) ds_frame()`),
    // so stubbing it as a bare return hung the rebuild at black doors (no green fade).  Drive it.
    if (mem[MEM_step_mode_flag]) { step_accum_sub_7e(); return; }   // $008D: JMP $6A8F
    if (mem[MEM_vbi_flags]) { step_accum_add_75(); return; }  // $0088: tunnel ring cycle
    if (uint8_t g = mem[MEM_terrain_state]) {            // $0089: scroll star/planet columns
        cpu.A = g; scroll_field_columns();                 // $6AEE: $0089 gate in A (>=4 advances accum)
        return;
    }
    // $008B (dl_src_index): the level-select door "elevator" scroll.  The Atari $5367 does
    // `if ($008B) JMP $69E3` (dl_index_dec) — run EVERY frame, ahead of the $008F gate — which
    // DECs $008B and rebuilds the whole per-scanline DL LMS window (a uniform vertical scroll of
    // the $2000 door field).  The post-mother-ship SELECT in-place level cycle
    // (boot_standby_launch_driver L_622d..L_628f) busy-spins waiting for this ISR-driven decrement
    // (`while ($008B != $0F)`); stubbing the branch inert froze the 2nd SELECT.  Drive it — the
    // real INTB_VERTB ISR decrements $008B during the main-thread spin, and the render side
    // (RescueOnFractalus door-scroll copper) repoints the viewport BPLxPT from $008B each frame.
    if (mem[MEM_dl_src_index]) {
        dl_index_dec();                                      // $008B: JMP $69E3
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned short g_dlScrollCount; g_dlScrollCount++; }   // liveness (scroll active)
#endif
        return;
    }
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

// ---- procedural tunnel rings ------------------------------------------------
// The concentric-tunnel-ring drawing primitives (plot_masked_pixel $66DE,
// plot_pixel_masked $66D5, fill_horizontal_span $665D, fill_vertical_span $669C,
// draw_symmetric_span_loop $6642, draw_frame_guide_columns $6620, init_row_coords_9c
// $6DDF) and the one-shot ring drawer (draw_frame_pattern_seq $65FB) all live as
// faithful shared native twins in src/gen/rof_native.c (declared in rof_decl.h).
// They are pure mem[] 6502 logic — not Amiga-specific — so the duplicate copies
// that used to live here were removed.  The Amiga standby builds the rings once via
// rof_native.c's boot_standby_launch_driver (the g_tunnelPrebuilt path); draw_ring_frame_step
// (above) drives the per-frame ring-clear and calls draw_symmetric_span_loop from
// rof_native.c.  The former Amiga-only entry points draw_tunnel_rings_native and
// tunnel_ring_arm_native were dead (no callers) and are gone with them.

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
//   $534D  sfx_voice_tick ($70F9)         -> ported below, gated $00E7 & BIT $062D (25 Hz)
//   $5359  music_player_tick ($7253)      -> ported below, gated on $0655 (see note)
//          sfx_voice_envelope_tick ($548D) -> ported below, gated on $060B
extern "C" void sfx_voice_tick(void);            // $70F9: attract/standby-theme SFX sequencer (validated twin)
extern "C" void sfx_voice_envelope_tick(void);   // $548D: SFX voice engine + $0719 ring drain
extern "C" void music_player_tick(void);         // $7253: note-stream music player (transpiled)
extern "C" uint8_t platform_hw_read(uint16_t addr);  // $D01F/$D010/$D300 live input (hwRead)

// vbi_attract_poll: faithful port of the $5398 console/attract poll shared by BOTH
// standby-family VBI handlers ($52D7 calls it at $533C; $53CC at $5400).  Reads the
// console keys ($D01F), fire trigger ($D010), and joystick ($D300) — all maintained on
// the Amiga now (the keyboard ISR keeps mem[$D01F]; hwRead maps $D010/$D300 to
// s_trig0State/s_portaState), so route them through platform_hw_read exactly as bus_read
// would.  On ANY input it resets the attract-mode timeout ($0049/$0002/$00E2/$0003).
//
// ⭐ $5398 OPENS A KEYBOARD WINDOW, and dropping it caused a soft hang.  The 6502:
//     5398  LDX #$FF
//     539A  CLI / SEI        <- one-instruction window; a PENDING POKEY keyboard IRQ lands here
//     539C  CPX #$80 / JMP $52BE      BREAK -> the restart trampoline
//     53A3  TXA / BMI $53B4           X still $FF (no key) -> fall through to the console poll
//     53A6  STX $0049 ... / BNE $53CB a REAL key: reset the attract timeout and RETURN
// The game runs with IRQs masked ($3D27 SEI) and POKEY's keyboard IRQ armed (IRQEN=$C0 at
// $3DA1), so a keypress stays PENDING until a CLI window opens and irq_handler ($462A) drops
// KBCODE&$3F (or $80 for BREAK) into X.  There are only two such windows: this one, and the
// flight VBI's $519C.  Note that NEITHER standby-family path routes the code to
// event_sequence_dispatcher ($4644) — out of flight a command key does nothing but reset the
// attract timeout.
//
// This port previously omitted the window on the reasoning that "X is loaded #$FF and never
// becomes $80".  That is only true if no IRQ can fire in the window, which is exactly backwards:
// the window exists to let one fire.  The consequence was that a command key pressed OUTSIDE
// flight stayed latched in s_pendingFlightKey across the whole launch cinematic and was consumed
// by the FIRST flight VBI $519C window instead — so pressing ESC on the Standby screen
// dispatched the freeze takeover ($0043) the instant flight began, parking the main loop in its
// `while (event_active_flag)` spin: measured as VVBLKI=$4FF5 with $0043=01 and the painted-frame
// counter frozen (amiga/esc_standby.gdb).  Consuming the key here is what the Atari does.
extern "C" uint8_t rof_attract_poll_key(void);   // PlatformAmiga.cpp — leaves a pending $80

static void vbi_attract_poll(void)
{
    const uint8_t x = rof_attract_poll_key();   // $5398 LDX #$FF + the $539A CLI/SEI window
    // $539C-$53A0: BREAK -> JMP $52BE.  rof_check_restart drives the restart from main-loop
    // context (it re-reads the same pending code), so leave it alone and stop here.
    if (x == 0x80u) return;
    if (!(x & 0x80u)) {                 // $53A3/$53A4: TXA; BMI -> only a REAL key comes here
        mem[MEM_joystick_raw]  = x;     // $53A6 STX $0049 (the keycode itself)
        mem[MEM_rtclok_frac]   = 0x00u; // $53A8
        mem[MEM_attract_timer] = 0x64u; // $53AC
        mem[MEM_zp_flag_03]    = 0x64u; // $53B0
        return;                         // $53B2 BNE $53CB (A=$64, always taken)
    }

    // $53B4-$53C9: input iff a console key is down, OR fire is pressed, OR the stick is up/down.
    bool input = ((platform_hw_read(0xD01Fu) & 0x07u) != 0x07u)     // CONSOL START/SELECT/OPTION
              || (platform_hw_read(0xD010u) == 0x00u)               // TRIG0 fire
              || ((platform_hw_read(0xD300u) & 0x03u) != 0x03u);    // PORTA joystick up/down
    if (input) {                                    // $53A6-$53B0: reset the attract timeout
        mem[MEM_joystick_raw]  = 0xFFu;   // $0049 = X ($FF)
        mem[MEM_rtclok_frac]   = 0x00u;   // $0002 = 0
        mem[MEM_attract_timer] = 0x64u;   // $00E2 = 100
        mem[MEM_zp_flag_03]    = 0x64u;   // $0003 = 100
    }
}

// vbi_shared_tail: the $534D deferred-VBI audio tail shared by both standby-family
// handlers ($52D7 falls into it; $53CC does JMP $534D at $5403).  Faithful order:
//   $534D  LDA $00E7; BEQ; BIT $062D; BNE; JSR $70F9   sfx_voice_tick (every other frame)
//   $5359  LDA $0655; BEQ; JSR $7253                   music_player_tick (jingle-gated)
//   $5361  JSR $548D                                   sfx_voice_envelope_tick (UNCONDITIONAL)
// POKEY writes route via bus_write -> Paula want[] (flushed once per frame in game_vbi_isr).
static void vbi_shared_tail(void)
{
    // $534D: run only when $00E7!=0 AND ($00E7 & $062D)==0 (25 Hz, since vbi_attract_timer_native
    // just bumped $062D).  The attract/standby-theme SFX sequencer.
    if (mem[0x00E7] && (mem[0x00E7] & mem[MEM_attract_timer_sub]) == 0) sfx_voice_tick();
    // $5359: the note-stream tune player (level-start, game-over/results jingles), gated on
    // the music-active flag $0655 (set by music_init_state $7238) so the game-over tune plays.
    if (mem[0x0655]) music_player_tick();
    // $5361: SFX voice engine + $0719 ring drain.  UNCONDITIONAL on the Atari (2026-07-24:
    // made faithful; the old `if (mem[$060B])` Amiga gate was removed).
    sfx_voice_envelope_tick();
}

// standby_vbi_native: faithful $52D7 vbi_handler_game (standby screen + launch cinematic).
// HW-display writes ($52D7-$5332: DMACTL, CHBASE, PMG/colour/HPOS registers) are SKIPPED —
// the Amiga copper owns the display.  mem[] order mirrors $52DF onward:
//   $52DF  STA $00C7                 dli_dispatch_index = 0   (DLI chain index reset)
//   $5333  INC $0014 (RTCLOK)        -> done by the ISR before this call
//   $5335  attract-timer cascade     -> vbi_attract_timer_native ($062D -> $00E2)
//   $533C  JSR $5398                 -> vbi_attract_poll
//   $533F  JSR $5367                 -> launch_anim_dispatch_native (door/tunnel/scroll)
//   $5342  lock-on parity           -> LSR $0643 / BCS skip / lock_on_indicator_tick / INC
//   $534D  shared audio tail        -> vbi_shared_tail
extern "C" void standby_vbi_native(void)
{
    mem[MEM_dli_dispatch_index] = 0u;        // $52DF STA $00C7 (DLI chain index reset)
    vbi_attract_timer_native();              // $5335 (INC $062D; 256-ovf -> INC $00E2)
    vbi_attract_poll();                      // $533C JSR $5398
    launch_anim_dispatch_native();           // $533F JSR $5367 (launch cinematic driver;
                                             //   self-gated on $0088/$0089/$008A/$008B/$008D)
    uint8_t g = mem[MEM_lock_on_indicator_tick_parity];   // $5342 LSR $0643
    mem[MEM_lock_on_indicator_tick_parity] = (uint8_t)(g >> 1);
    if (!(g & 1u)) {                         // carry clear -> run, then INC
        lock_on_indicator_tick();                  // $4229 (native twin, rof_native.c)
        mem[MEM_lock_on_indicator_tick_parity]++;
    }
    vbi_shared_tail();                       // $534D
}

// vbi_handler_1_native: faithful $53CC vbi_handler_1 (attract / Title Screen / game-over
// card).  This is the SEPARATE VBI body the Atari installs for the static cards — it does
// NOT run the launch cinematic ($5367) or the lock-on tick, and maintains a RICHER attract
// cascade than $52D7 (this is what the committed band-aid approximated by gating
// launch_anim inside standby_vbi_native; now split out properly).  HW writes ($53CC STA
// $D400; the $53ED colour-cycle STA $D016,X) are SKIPPED — but the pen cycle's INPUTS
// ($0002/$0013) are the mem[] state updateTitleScreenCopper reads to reproduce the pen
// cycle on the Amiga, so the cascade + clamps are kept exactly.  mem[] order:
//   $53D2  INC $0014 (RTCLOK)        -> done by the ISR before this call
//   $53D4  INC $062D; 256-ovf -> INC $0002, INC $0013, INC $00E2 (clamp $00E2 -> $80 if neg)
//   $53E5  LDY $0002; if neg clamp $0002 -> $80
//   $53ED  colour-cycle STA $D016,X  -> HW only, SKIPPED (see updateTitleScreenCopper)
//   $5400  JSR $5398                 -> vbi_attract_poll
//   $5403  JMP $534D                 -> vbi_shared_tail
extern "C" void vbi_handler_1_native(void)
{
    mem[MEM_attract_timer_sub]++;                          // $53D4 INC $062D
    if (mem[MEM_attract_timer_sub] == 0) {                 // $53D7 256-wrap cascade
        mem[MEM_rtclok_frac]++;                            // $53D9 INC $0002
        mem[MEM_RTCLOK_MID]++;                             // $53DB INC $0013
        mem[MEM_attract_timer]++;                          // $53DD INC $00E2
        if (mem[MEM_attract_timer] & 0x80u)               // $53DF BPL / LDA #$80 / STA $00E2
            mem[MEM_attract_timer] = 0x80u;
    }
    if (mem[MEM_rtclok_frac] & 0x80u)                     // $53E5 LDY $0002; BPL / LDY #$80 / STY $0002
        mem[MEM_rtclok_frac] = 0x80u;
    vbi_attract_poll();                                   // $5400 JSR $5398
    vbi_shared_tail();                                    // $5403 JMP $534D
}

// ============================================================================
// In-game flight  (was flight_native.cpp)
// ============================================================================

// Native in-game FLIGHT ports — the continuation of the launch cinematic.
//
// On the Atari the whole game is one program: boot_standby_launch_driver ($5F1D) runs the
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
#ifdef ROF_RAS_SHAPE
// Rasterizer SHAPE probe (`make RASTER_C=1 RAS_SHAPE=1 PROBES=1`, read via amiga/ras_shape.gdb)
// — the structural counters that sized the 2026-08-05 phase-2 restructure.  See the header
// comment on ROF_RAS_SHAPE in rof_native.c for why this is off even under PROBES.
// Buckets: [1..8] exact span, 9=9-12, 10=13-16, 11=17-24, 12=25-32, 13=33-64, 14=65-128, 15=129+.
extern "C" volatile unsigned long g_rasSpanHist[16] = { 0 };
extern "C" volatile unsigned long g_rasFarHist[16] = { 0 };
extern "C" volatile unsigned long g_rasFe = 0, g_rasFf = 0, g_rasPh1Adv = 0,
                                 g_rasPh1Push = 0, g_rasSat = 0, g_rasBail = 0;
// Draws that actually WROTE a plane2 dot — strictly fewer than the accepted draws (g_tdPlots),
// because the dot is plotted at the column's PREVIOUS top and the per-frame $6B reset floor
// lands on the one excluded scanline.  This is the denominator for any per-plotted-dot estimate.
extern "C" volatile unsigned long g_rasDots = 0;
// Whole-subtree occlusion-culling sizing (span-3 / span-4 straight-line DRAW groups): groups
// seen, groups where every draw is EXACTLY rejected, groups the asm truncates at the right
// edge, and the cheap bound-vs-COL_MAX test's early-out profile (Cons[k] = k columns passed
// before the first failure; Cons[N] = the cheap test culls it).  See ras_occl_probe().
extern "C" volatile unsigned long g_rasSp3Grp = 0, g_rasSp3Occl = 0, g_rasSp3Edge = 0;
extern "C" volatile unsigned long g_rasSp4Grp = 0, g_rasSp4Occl = 0, g_rasSp4Edge = 0;
extern "C" volatile unsigned long g_rasSp3Cons[4] = { 0 };
extern "C" volatile unsigned long g_rasSp4Cons[5] = { 0 };
// terrain_subdivide_column shape: the per-call helper mix that sizes TerrainSubdivideAssembler.s.
// FarKnown/P2Known count the far-endpoint reloads that are provably redundant (the slot was just
// written from `mid`, which is still in registers) — the twin's 2x `lsl.w #8` per load is 44
// cycles before the four byte loads.
extern "C" volatile unsigned long g_sdCalls = 0, g_sdBail = 0, g_sdP2Adopt = 0,
                                  g_sdP2Push = 0, g_sdP2Known = 0;
extern "C" volatile unsigned long g_sdInner = 0, g_sdInnerFarKnown = 0, g_sdFarEsc = 0,
                                  g_sdSteep = 0;
extern "C" volatile unsigned long g_sdRas = 0, g_sdSkip = 0, g_sdPop = 0, g_sdMid = 0,
                                  g_sdRough = 0;
extern "C" volatile unsigned long g_sdDepthHist[16] = { 0 };
// Per-SEGMENT occlusion-cull sizing (one subdivide call from the object draw-order loop):
// NoDraw = the ceiling (segments that accepted no draw at all), Sound = the derived
// max(ends)+W/2 bound's hit rate, Naive = the leaf version's unsound max(ends) bound,
// *Bad = a bound that culled a segment which DID draw (Sound must stay 0), Scan* = the
// test's own cost in COL_MAX compares.  See seg_occl_pre() in rof_native.c for the bound.
extern "C" volatile unsigned long g_segCalls = 0, g_segNoDraw = 0, g_segOffscr = 0,
                                  g_segSound = 0, g_segSoundBad = 0, g_segNaive = 0,
                                  g_segNaiveBad = 0, g_segScanCull = 0, g_segScanMiss = 0,
                                  g_segMisses = 0;
extern "C" volatile unsigned long g_segWidthHist[16] = { 0 };
extern "C" volatile unsigned long g_segDrawsCull = 0, g_segRasCull = 0;
// Same occlusion test one level down: cull a whole terrain_column_rasterize_core CALL.
extern "C" volatile unsigned long g_rcCalls = 0, g_rcNoAccept = 0, g_rcSound = 0,
                                  g_rcSoundBad = 0, g_rcScanHit = 0, g_rcScanMiss = 0,
                                  g_rcMisses = 0, g_rcDrawsCull = 0;
#endif
// object draw-order loop shape (-DROF_TDRAW_PROF): total pairs scanned, pairs culled at the
// primary gate (cheap skip), visible pairs reaching the companion/subdivide path, and total
// project_terrain_points calls.  Divide by g_tdFrames.  Tells whether the loop cost is the
// cull scan (many culled pairs) or the visible-pair work.
extern "C" unsigned long g_tdPairs = 0, g_tdCulled = 0, g_tdVisPairs = 0, g_tdProjCount = 0;
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
void sfx_reorder_voice_slot(void);          // $5614: voice-priority mixer — assigns a POKEY channel ($0705) to slot cpu.Y
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
#if defined(ROF_COMBAT_LOAD) && defined(ROF_FLIGHT_PROBE)
    // Split the REAL handler cost by combat state.  This is the measurement that finished the
    // combat attribution: the flight VBI carries the faithful 50 Hz sim + all of the audio, it
    // fires 50x/second no matter how slow the frame is, and its cost is NOT constant — an
    // explosion floods the SFX ring, so the handler gets more expensive exactly when combat is
    // heaviest.  Must use dHandler, NOT g_isrBeamLines: the latter deliberately includes the
    // 2x256-iteration ZP write-set audit above (~70 lines/firing of pure probe overhead), which
    // would swamp the real signal.
    {   extern volatile unsigned long g_clIsr[2]; extern volatile unsigned long g_clIsrN[2];
        const int st = (mem[0x0041u] != 0) ? 0 : 1;
        g_clIsr[st] += dHandler; g_clIsrN[st]++; }
#endif
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

// standby_vbi_native: the faithful $52D7 per-frame body (defined above).
extern "C" void standby_vbi_native(void);
// vbi_handler_1_native: the faithful $53CC per-frame body (attract/Title/game-over card).
extern "C" void vbi_handler_1_native(void);
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
#ifdef ROF_FLIGHT_PROBE
// Precise flight-VBI cost measurement (VPOSR/VHPOSR beam-line deltas).  Split into the
// three components game_vbi_isr runs for the $4FF5 flight frame, so we can attribute the
// per-firing scanline cost.  All three EXCLUDE the ZP-write-set audit inside flight_vbi_native
// (that audit is bracketed out separately there) — these are clean, real-VBI spans:
//   g_vbiHandlerLines = vbi_handler_flight (== g_flightProf.isrLines, mirrored here for the sum)
//   g_vbiSpriteLines  = flightShotTick + flightScannerTick (laser + LR-scanner blink)
//   g_vbiAudioLines   = flush_paula + noiseTick (Paula channel flush + noise-sample refresh)
// Per-firing scanlines = lines/calls; time = scanlines * 63.56 us; PAL frame = 313 lines.
extern "C" volatile unsigned long g_vbiSpriteLines = 0, g_vbiAudioLines = 0, g_vbiFullCalls = 0;
#endif

extern "C" void game_vbi_isr(void)
{
    if (!g_activeVbi) return;                        // scene still initialising — stay inert
    // Dispatch on the LIVE VVBLKI vector ($0222/$0223), exactly as the Atari OS VBLANK
    // jumps through it.  The genuine station_init/boot_standby_launch_driver/game_main_loop install
    // $1B30 (attract) / $52D7 (standby+cinematic) / $4FF5 (flight) in turn, so the right
    // body runs automatically — including across the internal cinematic->flight switch
    // inside game_main_loop.  An unknown or half-written vector falls back to standby
    // (harmless for the odd frame during a two-byte vector update).
    uint16_t vbi = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
    Cpu6502 saved = cpu;                            // == OS VBLANK PHA;TXA;PHA;TYA;PHA
    if      (vbi == 0x4FF5) { flight_vbi_native();    // $4FF5 in-flight VBI
#ifdef ROF_FLIGHT_PROBE
                              unsigned short _s0 = rof_beam_line();
#endif
                              PlatformAmiga::flightShotTick();      // laser sprite @ 50Hz (VBI, faithful)
                              PlatformAmiga::flightScannerTick();   // LR-scanner close-range blink @ 50Hz
#ifdef ROF_FLIGHT_PROBE
                              { unsigned short _s1 = rof_beam_line();
                                g_vbiSpriteLines += (_s1 >= _s0) ? (unsigned short)(_s1 - _s0)
                                                                 : (unsigned short)(_s1 + 313 - _s0);
                                g_vbiFullCalls++; }
#endif
                            }
    else if (vbi == 0x1B30) vbi_handler_station();   // $1B30 attract VBI (sets $0080 + RTCLOK)
    else if (vbi == 0x53CC) vbi_handler_1_native();  // $53CC attract/Title/game-over card VBI
    else                    standby_vbi_native();    // $52D7 standby/launch VBI (and fallback)
    cpu = saved;                                    // == XITVBV PLA;TAY;PLA;TAX;PLA
    // Apply this frame's batched POKEY→Paula writes — from the CIA-B music tick
    // (sfx_voice_tick) AND the in-game SFX engine (sfx_voice_envelope_tick, run in the VBI
    // bodies above).  One flush per frame: it silences released channels (vol=0) and
    // starts new notes/SFX, so without it stuck notes never stop and SFX never sound.
#ifdef ROF_FLIGHT_PROBE
    unsigned short _a0 = (vbi == 0x4FF5) ? rof_beam_line() : 0;
#endif
    flush_paula();
    PlatformAmiga::noiseTick();                     // refresh a slice of the noise sample (cheap)
#ifdef ROF_FLIGHT_PROBE
    if (vbi == 0x4FF5) { unsigned short _a1 = rof_beam_line();
        g_vbiAudioLines += (_a1 >= _a0) ? (unsigned short)(_a1 - _a0)
                                        : (unsigned short)(_a1 + 313 - _a0); }
#endif
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
// via sfx_reorder_voice_slot ($5614).  We install the launch end-state directly here, then let
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
    cpu.X = 0x00; cpu.Y = 0x0C; sfx_reorder_voice_slot();   // slot 12 -> a channel (noise body)
    cpu.X = 0x00; cpu.Y = 0x0D; sfx_reorder_voice_slot();   // slot 13 -> a channel
    cpu.X = 0x00; cpu.Y = 0x0E; sfx_reorder_voice_slot();   // slot 14 -> a channel
}
