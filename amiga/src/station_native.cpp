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

#include "PaulaAudio.h"

extern "C" uint8_t mem[65536];

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
        if ((int8_t)reload < 0) reload = paula_pokey_random();  // RANDOM path
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
