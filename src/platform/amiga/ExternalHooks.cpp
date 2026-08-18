/* ExternalHooks.cpp — the one instance of the patch block.  See ExternalHooks.h.
 *
 * It lives in .data (not .rodata: the patcher writes it; not .bss: the magic has to be
 * IN the file for the scan to find it) and is longword-aligned so the scan can read it
 * with a 68000 long compare. */
#include "ExternalHooks.h"

extern "C" {

struct RofExternalHooks g_rofExternalHooks __attribute__((aligned(4))) = {
    ROF_HOOKS_MAGIC0,
    ROF_HOOKS_MAGIC1,
    ROF_HOOKS_VERSION,
    (unsigned short)sizeof(struct RofExternalHooks),
    0,
    0
};

}
