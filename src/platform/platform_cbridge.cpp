/* C bridge — implements the C-callable interface declared in platform_c.h
   by forwarding to the Platform C++ singleton.  Compiled as C++ so it can
   see Platform.h; the symbols are exported with C linkage so the generated
   C translation units can link against them without name-mangling. */

#include "Platform.h"
#include "platform_c.h"

extern "C" {

uint8_t platform_hw_read(uint16_t addr) {
    return platform ? platform->hwRead(addr) : 0;
}

void platform_hw_write(uint16_t addr, uint8_t val) {
    if (platform) platform->hwWrite(addr, val);
}

void platform_shadow_write(uint16_t addr, uint8_t val) {
    if (platform) platform->shadowWrite(addr, val);
}

int platform_load_image(const char* path) {
    return platform ? platform->loadImage(path) : -1;
}

void platform_register_vbi(uint16_t addr, void (*fn)(void)) {
    if (platform) platform->registerVBI(addr, fn);
}

void platform_indirect_jmp(uint16_t addr) {
    if (platform) platform->indirectJmp(addr);
}

void platform_render_frame(void) {
    if (platform) platform->renderFrame();
}

void platform_tick_vbi(void) {
    if (platform) platform->tickVBI();
}

void platform_poll_events(void) {
    if (platform) platform->pollEvents();
}

/* SDL renders mem[] directly, so the tunnel rings need no bitplane conversion. */
void platform_tunnel_rings_drawn(void) {}

} /* extern "C" */

/* ---------------------------------------------------------------------------
   Test-only headless platform for the native-reimplementation validation
   harness (tools/validate_native.c).  The harness links the platform objects
   but never creates a window; it needs a Platform whose hwRead routes the
   POKEY RANDOM register ($D20A) to the LFSR so RANDOM-reading terrain routines
   can be diffed, with a seedable LFSR so both runs share an identical stream.
   None of this is referenced by the real game build.
   --------------------------------------------------------------------------- */
namespace {
struct HeadlessPlatform : Platform {
    void    setInterrupt(void (*)(void)) override {}
    int     framesPerSecond() override { return 50; }
    void    renderFrame() override {}
    int     loadImage(const char*) override { return -1; }
    uint8_t hwRead(uint16_t addr) override {
        return addr == 0xD20A ? pokeyRandomStep() : 0x00;
    }
};
} /* namespace */

extern "C" {

void platform_test_init_headless(void) {
    if (!platform) platform = new HeadlessPlatform();
}

void     platform_test_seed_rng(uint32_t s) { if (platform) platform->rngSeed(s); }
uint32_t platform_test_get_rng(void)        { return platform ? platform->rngGet() : 0; }

} /* extern "C" */
