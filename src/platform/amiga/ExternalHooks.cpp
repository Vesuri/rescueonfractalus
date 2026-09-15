/* ExternalHooks.cpp — the one instance of the patch block.  See ExternalHooks.h.
 *
 * It lives in .data (not .rodata: the patcher writes it; not .bss: the magic has to be
 * IN the file for the scan to find it) and is longword-aligned so the scan can read it
 * with a 68000 long compare. */
#include "ExternalHooks.h"
#include <stddef.h>

/* Keep the assembly contract in whdload/RoFSlave.s honest. */
static_assert(sizeof(struct RofLogoOverrideContext) == 40, "logo hook context ABI size");
static_assert(offsetof(struct RofLogoOverrideContext, bitmap) == 4, "logo hook bitmap ABI offset");
static_assert(offsetof(struct RofLogoOverrideContext, palette) == 28, "logo hook palette ABI offset");
static_assert(offsetof(struct RofLogoOverrideContext, phase) == 36, "logo hook phase ABI offset");
static_assert(sizeof(struct RofExternalHooks) == 24, "external hook block ABI size");
static_assert(offsetof(struct RofExternalHooks, logoOverride) == 20, "logo hook ABI offset");

extern "C" {

#ifdef ROF_ENHANCED_LOGO
extern const unsigned char rof_enhanced_logo[];
extern const unsigned char rof_enhanced_games[];
extern const unsigned char rof_enhanced_palette[];

/* Standalone test implementation of exactly the contract exercised by the slave.
 * The ordinary build remains free of both this routine and its optional artwork. */
static unsigned long rofBuiltInEnhancedLogo(struct RofLogoOverrideContext* context)
{
    if (!context || context->version != ROF_LOGO_OVERRIDE_VERSION ||
        context->size < sizeof(*context) ||
        context->format != ROF_LOGO_FORMAT_PLANAR4_INTERLEAVED ||
        context->width != 320 || context->height != 340 || context->bitplanes != 4 ||
        context->planeBytesPerRow != 40 || context->rowBytes != 160)
        return 0;

    if (context->phase == ROF_LOGO_PHASE_INITIAL) {
        if (!context->bitmap || context->bitmapBytes < 16000 || !context->palette ||
            context->paletteEntries < 16)
            return 0;
        for (unsigned long i = 0; i < 16000; ++i)
            context->bitmap[i] = rof_enhanced_logo[i];
        unsigned char* paletteBytes = (unsigned char*)context->palette;
        for (unsigned i = 0; i < 32; ++i)
            paletteBytes[i] = rof_enhanced_palette[i];
        context->visibleRows = 100;
        context->topBlankLines = 58;
        return ROF_LOGO_OVERRIDE_BITMAP | ROF_LOGO_OVERRIDE_PALETTE |
               ROF_LOGO_OVERRIDE_GEOMETRY;
    }

    if (context->phase == ROF_LOGO_PHASE_GAMES && context->bitmap) {
        const unsigned char* source = rof_enhanced_games;
        unsigned char* destination = context->bitmap + 65UL * 160UL + 15;
        for (unsigned span = 0; span < 30 * 4; ++span) {
            for (unsigned byte = 0; byte < 11; ++byte)
                *destination++ = *source++;
            destination += 29;       /* next 40-byte plane span at the same x */
        }
        return ROF_LOGO_OVERRIDE_BITMAP;
    }
    return 0;
}
#endif

struct RofExternalHooks g_rofExternalHooks __attribute__((aligned(4))) = {
    ROF_HOOKS_MAGIC0,
    ROF_HOOKS_MAGIC1,
    ROF_HOOKS_VERSION,
    (unsigned short)sizeof(struct RofExternalHooks),
    0,
    0,
#ifdef ROF_ENHANCED_LOGO
    rofBuiltInEnhancedLogo
#else
    0
#endif
};

}
