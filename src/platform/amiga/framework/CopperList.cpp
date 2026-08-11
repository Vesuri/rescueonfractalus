#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "Copperlist.h"
#include "Palette.h"
#include "AmigaHardware.h"
#include "Sprite.h"
#include "Bitmap.h"

CopperList::CopperList() :
    data_(0),
    length(0),
    owner(false)
{
}

CopperList::CopperList(uint32_t* data, uint32_t length, bool takeOwnership) :
    data_(data),
    length(length),
    owner(takeOwnership)
{
    if (length > 0) {
        // Pre-fill every slot with a copper NOP (MOVE #0 -> $1FE, the standard dummy register).
        // These are FIXED-LAYOUT lists: buildLayout writes ~40 scattered INDEX_* slots, and a slot
        // that no writer ever reaches would otherwise keep the allocation's MEMF_CLEAR zeros —
        // and an all-zero copper word is MOVE #0 -> register $000, which is below the CDANG
        // threshold and HALTS THE COPPER for the rest of the frame.  The whole display below that
        // point then freezes on whatever pointers/modulos were last set (symptom: the entire
        // screen renders with the top bar's bitplane parameters).  Cost is one-time; the payoff is
        // that forgetting a slot degrades to a wasted copper cycle instead of a dead display.
        for (uint32_t i = 0; i < length; i++) data_[i] = copperMove(0x1FE, 0);
        data_[0] = copperWait(16, 0);
        data_[length - 1] = copperWait(255, 254);
    }
}

CopperList::~CopperList()
{
    if (owner) {
        FreeMem(data_, length << 2);
    }
}

uint32_t* CopperList::data() const
{
    return data_;
}

CopperList* CopperList::allocate(uint32_t length)
{
    uint32_t* data = (uint32_t*)AllocMem(length << 2, MEMF_CHIP | MEMF_CLEAR);
    return data ? new CopperList(data, length, true) : 0;
}

// GCC + ASSEMBLER: reach CopperListAssembler.s via register-marshalling wrappers
// (this in a0, args in the SAS/C-annotated registers). SAS/C+ASSEMBLER links the
// asm directly; the #ifndef ASSEMBLER C++ bodies below serve the !ASSEMBLER builds.
#if defined(ASSEMBLER) && !defined(__SASC)
void CopperList::showSprite(uint32_t listIndex, uint16_t spriteNumber, const Sprite& sprite)
{
    register CopperList* self __asm("a0") = this;        // this         in a0
    register uint32_t a __asm("d0") = listIndex;         // listIndex    in d0
    register uint16_t b __asm("d1") = spriteNumber;      // spriteNumber in d1
    register const Sprite* s __asm("a1") = &sprite;      // sprite       in a1
    __asm volatile("jsr _showSprite__10CopperListFUlUsRC6Sprite"
                   : "+r"(self), "+r"(a), "+r"(b), "+r"(s)
                   :
                   : "cc", "memory");
}

void CopperList::showBitmap(uint32_t listIndex, const Bitmap& bitmap, uint16_t firstBitplane, uint16_t bitplaneNumberDelta, int16_t xOffset, int16_t yOffset, uint16_t bitplaneCount)
{
    register CopperList* self __asm("a0") = this;            // this                in a0
    register uint32_t a __asm("d0") = listIndex;             // listIndex           in d0
    register const Bitmap* bm __asm("a1") = &bitmap;         // bitmap              in a1
    register uint16_t b __asm("d1") = firstBitplane;         // firstBitplane       in d1
    register uint16_t c __asm("d2") = bitplaneNumberDelta;   // bitplaneNumberDelta in d2
    register int16_t  d __asm("d3") = xOffset;               // xOffset             in d3
    register int16_t  e __asm("d4") = yOffset;               // yOffset             in d4
    register uint16_t f __asm("d5") = bitplaneCount;         // bitplaneCount       in d5
    __asm volatile("jsr _showBitmap__10CopperListFUlRC6BitmapUsUsssUs"
                   : "+r"(self), "+r"(a), "+r"(bm), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f)
                   :
                   : "cc", "memory");
}
#elif !defined(ASSEMBLER)
void CopperList::showSprite(uint32_t listIndex, uint16_t spriteNumber, const Sprite& sprite)
{
    uint32_t spriteData = (uint32_t)sprite.data();
    data_[listIndex++] = copperMove(spr1pth + (spriteNumber << 2), (uint16_t)(spriteData >> 16));
    data_[listIndex] = copperMove(spr1ptl + (spriteNumber << 2), (uint16_t)spriteData);
}

void CopperList::showBitmap(uint32_t listIndex, const Bitmap& bitmap, uint16_t firstBitplane, uint16_t bitplaneNumberDelta, int16_t xOffset, int16_t yOffset, uint16_t bitplaneCount)
{
    uint32_t bitplane = (uint32_t)bitmap.data;
    if (xOffset) {
        bitplane += xOffset >> 3;
    }
    if (yOffset) {
        bitplane += yOffset * bitmap.rowSizeInBytes;
    }
    if (bitplaneCount == 0) {
        bitplaneCount = bitmap.bitplanes;
    }
    uint16_t bitplanePointerRegister = bpl1pth + ((firstBitplane - 1) << 2);
    for (uint16_t i = 0; i < bitplaneCount; i++, bitplanePointerRegister += (bitplaneNumberDelta << 2)) {
        data_[listIndex++] = copperMove(bitplanePointerRegister, (uint16_t)(bitplane >> 16));
        data_[listIndex++] = copperMove(bitplanePointerRegister + 2, (uint16_t)bitplane);
        bitplane += bitmap.interleaved ? bitmap.widthInBytes : bitmap.bitplaneSizeInBytes;
    }
}
#endif

// Copper-driven palette/playfield (imported from DanceDiverse3) -- unconditional
// C++. setPalette writes the requested colour range as copper colour-register
// moves (DD3's AGA bplcon3 bank switching is dropped: this covers the 32 base
// registers; pull the 24-bit bundle for AGA banking). setPlayfield mirrors
// AmigaHardware::setPlayfield but emits copper moves so the mode can be set
// per-frame from the copper list.
uint32_t CopperList::setPalette(uint32_t listIndex, const Palette& palette, uint16_t colorIndex, int16_t fromIndex, int16_t toIndex)
{
    if (toIndex < 0) {
        toIndex = (int16_t)(palette.colorCount() - 1);
    }

    uint16_t colorRegister = (uint16_t)(color00 + (colorIndex << 1));
    for (int16_t i = fromIndex; i <= toIndex; i++, colorRegister += 2) {
        data_[listIndex++] = copperMove(colorRegister, palette[i]);
    }

    return listIndex;
}

void CopperList::setColor(uint32_t listIndex, uint16_t color, uint16_t count)
{
    uint16_t* wordData = (uint16_t*)data_;
    listIndex += listIndex + 1;

    for (uint16_t i = 0; i < count; i++, listIndex += 2) {
        wordData[listIndex] = color;
    }
}

// Emit ONLY the per-region-varying playfield registers: BPLCON0 (bitplane count /
// mode) and the interleaved-modulo pair (BPL1MOD/BPL2MOD).  These genuinely change
// down the screen — e.g. a 2-bitplane title band above a 3-bitplane terrain band —
// so each region re-emits them.  The rest of the playfield setup (FMODE, BPLCON3,
// BPLCON2, BPLCON1, DIWSTRT/STOP/HIGH, DDFSTRT/STOP) is CONSTANT for the whole frame;
// set it ONCE up front via AmigaHardware::setPlayfield() instead of re-running those
// nine MOVEs in the copper every frame.  (Width/height/centerY are accepted for a
// signature compatible with AmigaHardware::setPlayfield but only the bitplane geometry
// is used here.)
uint32_t CopperList::setPlayfield(uint32_t listIndex, uint16_t width, uint16_t height, uint8_t bitplaneCount, bool interleaved, bool hires, bool interlace, bool dualPlayfield, bool holdAndModify, uint16_t centerY)
{
    (void)height; (void)interlace; (void)centerY;
    uint16_t bitplaneWidth = width >> 3;
    uint16_t alignedWidth = AmigaHardware::hasAGAChipSet ? (bitplaneWidth & 0xfffc) : bitplaneWidth;
    data_[listIndex++] = copperMove(bplcon0, (uint16_t)((bitplaneCount << PLNCNTSHFT) | (hires ? MODE_640 : 0) | (dualPlayfield ? DBLPF : 0) | (holdAndModify ? HOLDNMODIFY : 0) | USE_BPLCON3));
    data_[listIndex++] = copperMove(bpl1mod, (uint16_t)(interleaved ? (bitplaneCount * bitplaneWidth - alignedWidth) : 0));
    data_[listIndex++] = copperMove(bpl2mod, (uint16_t)(interleaved ? (bitplaneCount * bitplaneWidth - alignedWidth) : 0));

    return listIndex;
}
