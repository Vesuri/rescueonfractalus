// Boot memory image — load the pristine rof.xex into mem[].
//
// This is the faithful initial state: instead of booting from a hand-captured
// mid-Standby RAM snapshot (which baked in runtime state such as $00E7=1, the music
// gate), we reproduce what the Atari OS loader does — zero RAM, then place each XEX
// segment at its load address.  All runtime state then starts at genuine power-on
// values and the original setup code (the INITAD chain ending at game_entry $3CDE)
// establishes everything itself.
//
// rof.xex is embedded in .rodata by incbin.s (rof_xex / rof_xex_end).
//
// Integer types come from the project's force-included framework/SASCCompat.h
// (CPPFLAGS -include), like the other Amiga TUs — no <stdint.h> here.

extern "C" volatile uint8_t mem[65536];

// The original Atari 8-bit segmented load file, embedded by incbin.s.
extern "C" uint8_t rof_xex[];
extern "C" uint8_t rof_xex_end[];

// XEX format: optional leading $FFFF magic, then segments of
//   [startLo, startHi, endLo, endHi, data...]
// A repeated $FFFF before a segment is a (skippable) header marker.  The INITAD
// pseudo-segments ($02E2-$02E3) are loaded harmlessly; the entry point is invoked
// from C (RescueOnFractalus::run -> game_entry) rather than honoured here.
extern "C" void load_xex_image(void)
{
    for (uint32_t i = 0; i < 65536u; i++) mem[i] = 0u;

    const uint8_t* d   = rof_xex;
    const uint32_t len = (uint32_t)(rof_xex_end - rof_xex);
    uint32_t i = 0;
    if (i + 1 < len && d[i] == 0xFF && d[i + 1] == 0xFF) i += 2;   // skip $FFFF magic
    while (i + 4 <= len) {
        if (d[i] == 0xFF && d[i + 1] == 0xFF) { i += 2; continue; } // segment-header marker
        uint16_t s = (uint16_t)(d[i] | (d[i + 1] << 8));
        uint16_t e = (uint16_t)(d[i + 2] | (d[i + 3] << 8));
        i += 4;
        uint32_t seglen = (uint32_t)e - (uint32_t)s + 1u;
        for (uint32_t k = 0; k < seglen && i < len; k++, i++)
            mem[(uint16_t)(s + k)] = d[i];
    }
}
