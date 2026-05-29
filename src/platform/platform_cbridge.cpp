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

} /* extern "C" */
