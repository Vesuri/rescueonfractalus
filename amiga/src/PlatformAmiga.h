#pragma once
/* PlatformAmiga — the Amiga-specific platform layer for Rescue on Fractalus.
 *
 * Owns everything Amiga-hardware-specific that sits below the RescueOnFractalus
 * scene: the display takeover (LoadView/DMACON/display window), the real
 * INTB_VERTB VBI server, the CIA-B Timer A music tick, the CIA-A serial-port
 * keyboard (RETURN -> Atari START switch), the POKEY->Paula audio backend, and
 * the launch-cinematic frame pump.  Static-method class (one platform per run),
 * matching the framework's AmigaHardware idiom.
 *
 * It also implements the platform_c.h bridge that the C-compiled 6502
 * transliteration calls (kept as extern "C" free functions; see PlatformAmiga.cpp).
 */
#include "../framework/Util.h"  // provides uint8_t, uint16_t, uint32_t

class RescueOnFractalus;

class PlatformAmiga {
public:
    // run(): the whole Amiga lifecycle.  Takes over the display, installs the VBI
    // server + CIA-B music tick + CIA-A keyboard, loads the boot image, runs the
    // scene (the genuine transpiled/native boot chain), then restores the system.
    // main() opens graphics.library, constructs the scene, and calls this; it
    // returns when the user quits (left mouse button).
    static void run(RescueOnFractalus& scene);

    // POKEY->Paula audio backend.  The 6502-converted station_audio writes POKEY
    // registers (via platform_hw_write below); these route them to Amiga Paula DMA.
    //   audioInit/audioShutdown : bracket Paula audio DMA.
    //   pokeyRandom             : advance + return one POKEY LFSR byte (read by the
    //                             attract/noise/star code, as the Atari read $D20A).
    //   noiseTick               : per-VBI refresh of the continuous poly17 noise sample.
    static void audioInit();
    static void audioShutdown();
    static uint8_t pokeyRandom();
    static void noiseTick();
};

#ifdef __cplusplus
extern "C" {
#endif

/* Platform bridge — C linkage so the bus.h inlines (compiled from C rof_gen.c)
 * can reach them.  Declared in platform_c.h; defined in PlatformAmiga.cpp.
 * (platform_hw_write is also called from the C++ SfxPlayer.) */
uint8_t platform_hw_read (uint16_t addr);
void    platform_hw_write(uint16_t addr, uint8_t val);
void    platform_shadow_write(uint16_t addr, uint8_t val);
void    platform_register_vbi(uint16_t addr, void (*fn)(void));
void    platform_indirect_jmp(uint16_t addr);
void    platform_render_frame(void);
void    platform_poll_events(void);
void    platform_tick_vbi(void);
int     platform_load_image(const char* path);

/* Gate the launch-cinematic frame pump (set by RescueOnFractalus::run): while on,
 * the transpiled frame-wait spin loops drive a real one-VBI repaint and RTCLOK is
 * advanced in lockstep by platform_tick_vbi instead of the free-running ISR. */
void rof_launch_blocking(uint8_t on);

#ifdef __cplusplus
}
#endif
