#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <hardware/cia.h>
#include <hardware/blit.h>
#include <hardware/custom.h>
#include <graphics/display.h>
#include "AmigaHardware.h"
#include "CopperList.h"
#include "Sprite.h"
#include "Palette.h"
#ifndef ASSEMBLER
#include <proto/exec.h>   // Supervisor(), for getVBR() (GCC build only)
#endif

uint16_t AmigaHardware::octants[4] = {
    OCTANT2 | LINEMODE,
    OCTANT1 | LINEMODE,
    OCTANT3 | LINEMODE,
    OCTANT4 | LINEMODE
};

uint16_t AmigaHardware::blitterQueueBuffer[BLITTER_QUEUE_SIZE + 1];
uint16_t* AmigaHardware::blitterQueueBufferEnd = blitterQueueBuffer + BLITTER_QUEUE_SIZE;
uint16_t* AmigaHardware::blitterQueueToBeBlitted = blitterQueueBuffer;
uint16_t* AmigaHardware::blitterQueueAddPosition = blitterQueueBuffer;

bool AmigaHardware::hasAGAChipSet = false;
volatile bool AmigaHardware::hasQueuedBlits = false;

void AmigaHardware::setCopperList(const CopperList& copperList, bool immediate)
{
    *cop1lcPointer = copperList.data();
    if (immediate) {
        *copjmp1Pointer = 0;
    }
}

void AmigaHardware::setPalette(uint16_t colorIndex, const Palette& palette)
{
    uint16_t* colorRegister = (uint16_t*)color00Pointer + colorIndex;
    for (uint32_t i = 0; i < palette.colorCount(); i++) {
        *colorRegister++ = palette[i];
    }
}

void AmigaHardware::setColor(uint16_t colorIndex, uint16_t color)
{
    uint16_t* colorRegister = (uint16_t*)color00Pointer + colorIndex;
    *colorRegister = color;
}

void AmigaHardware::setPlayfield(uint16_t width, uint16_t height, uint8_t bitplaneCount, bool interleaved, bool hires, bool interlace, bool dualPlayfield, bool holdAndModify, uint16_t centerY)
{
    uint16_t halfHeight = height >> 1;
    uint16_t bitplaneWidth = width >> 3;
    uint16_t alignedWidth = hasAGAChipSet ? (bitplaneWidth & 0xfffc) : bitplaneWidth;
    *fmodePointer = (uint16_t)(hasAGAChipSet ? 3 : 0);
    *bplcon3Pointer = 0x0c00 | BPLCON3_BRDNBLNK | BPLCON3_BRDNTRAN;
    *bplcon2Pointer = 0x0024;
    *bplcon1Pointer = 0;
    *bplcon0Pointer = (uint16_t)((bitplaneCount << PLNCNTSHFT) | (hires ? MODE_640 : 0) | (dualPlayfield ? DBLPF : 0) | (holdAndModify ? HOLDNMODIFY : 0) | USE_BPLCON3);
    // DIW bounds the 320px lores fetch (DDFSTRT=0x38/DDFSTOP=0xD0 → hpos
    // 0x81..0x1C1); matched so BPLCON3 BRDNBLNK can blank both borders.
    *diwstrtPointer = (uint16_t)(((centerY - halfHeight) << 8) | 0x81);
    *diwstopPointer = (uint16_t)(((centerY + halfHeight) << 8) | 0xc1);
    *diwhighPointer = 0x2100;
    if (hasAGAChipSet) {
        if (hires) {
            *ddfstrtPointer = (uint16_t)(0x88 - alignedWidth);
            *ddfstopPointer = (uint16_t)(0x94 + (alignedWidth >> 1));
        } else {
            *ddfstrtPointer = (uint16_t)(0x88 - (alignedWidth << 1));
            *ddfstopPointer = (uint16_t)(0x90 + alignedWidth);
        }
    } else {
        if (hires) {
            *ddfstrtPointer = (uint16_t)(0x88 - bitplaneWidth);
            *ddfstopPointer = (uint16_t)(0x80 + bitplaneWidth);
        } else {
            *ddfstrtPointer = (uint16_t)(0x88 - (bitplaneWidth << 1));
            *ddfstopPointer = (uint16_t)(0x80 + (bitplaneWidth << 1));
        }
    }
    *bpl1modPointer = (uint16_t)(interleaved ? (bitplaneCount * bitplaneWidth - alignedWidth) : 0);
    *bpl2modPointer = (uint16_t)(interleaved ? (bitplaneCount * bitplaneWidth - alignedWidth) : 0);
}

void AmigaHardware::setSpritesEnabled(bool enabled)
{
    if (enabled) {
        *dmaconPointer = DMAF_SETCLR | DMAF_SPRITE;
    } else {
        *dmaconPointer = DMAF_SPRITE;
    }
}

void AmigaHardware::setBlitterNasty(bool enabled)
{
    if (enabled) {
        *dmaconPointer = DMAF_SETCLR | DMAF_BLITHOG;
    } else {
        *dmaconPointer = DMAF_BLITHOG;
    }
}

void AmigaHardware::setDMAChannels(uint16_t dmaChannels, bool enabled)
{
    if (enabled) {
        *dmaconPointer = (uint16_t)(dmaChannels | DMAF_SETCLR);
    } else {
        *dmaconPointer = dmaChannels;
    }
}

void AmigaHardware::setInterrupts(uint16_t interrupts, bool enabled)
{
    if (enabled) {
        *intenaPointer = (uint16_t)(interrupts | INTF_SETCLR);
    } else {
        *intenaPointer = interrupts;
    }
}

uint16_t AmigaHardware::enabledDMAChannels()
{
    return *dmaconrPointer;
}

uint16_t AmigaHardware::enabledInterrupts()
{
    return *intenarPointer;
}

void AmigaHardware::clearInterruptRequests(uint16_t interrupts)
{
    *intreqPointer = interrupts;
    *intreqPointer = interrupts;
}

bool AmigaHardware::isLeftMouseButtonPressed()
{
    return !(*ciaapraPointer & CIAF_GAMEPORT0);
}

bool AmigaHardware::isRightMouseButtonPressed()
{
    return !(*potinpPointer & (1 << 10));
}

void AmigaHardware::waitBeamLine(uint16_t line)
{
    // Vertical beam position = VPOSR bit0 (V8) << 8 | VHPOSR high byte (V0..7).
    // Spin until it reaches the target line.  If we're already past it this frame,
    // first wait for the beam to wrap (back above the line) so the sync lands on
    // the upcoming line, not returns immediately mid-frame.
    line &= 0x1ffu;
    auto beamY = []() -> uint16_t {
        return (uint16_t)(((*vposrPointer & 1u) << 8) | (*vhposrPointer >> 8));
    };
    if (beamY() >= line)
        while (beamY() >= line) { /* wait for vertical wrap */ }
    while (beamY() < line) { /* wait for the target line */ }
}

#ifndef ASSEMBLER
void AmigaHardware::blitterClear(uint16_t* data, uint16_t width, uint16_t height, int16_t modulo)
{
    AmigaHardware::setInterrupts(INTF_BLIT, false);
    if (!isBlitterBusy() && !hasQueuedBlits) {
        *bltawmPointer = 0xffffffff;
        *bltcon1Pointer = 0;
        *bltcon0Pointer = BC0F_DEST;
        *bltdmodPointer = modulo;
        *bltdptPointer = data;
        *bltsizePointer = (uint16_t)((height << 6) | width);
#ifdef DEBUG
        *color00Pointer = 0xf00;
#endif
    } else {
        *blitterQueueAddPosition++ = 8;
        if (blitterQueueAddPosition + 16 >= blitterQueueBufferEnd) {
            blitterQueueAddPosition = blitterQueueBuffer;
        }
        *blitterQueueAddPosition++ = bltafwm;
        *blitterQueueAddPosition++ = 0xffff;
        *blitterQueueAddPosition++ = bltalwm;
        *blitterQueueAddPosition++ = 0xffff;
        *blitterQueueAddPosition++ = bltcon1;
        *blitterQueueAddPosition++ = 0;
        *blitterQueueAddPosition++ = bltcon0;
        *blitterQueueAddPosition++ = BC0F_DEST;
        *blitterQueueAddPosition++ = bltdmod;
        *blitterQueueAddPosition++ = modulo;
        *blitterQueueAddPosition++ = bltdpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(data) >> 16);
        *blitterQueueAddPosition++ = bltdptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)data);
        *blitterQueueAddPosition++ = bltsize;
        *blitterQueueAddPosition++ = (uint16_t)((height << 6) | width);
        hasQueuedBlits = true;
        processBlitterQueue();
    }
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}

void AmigaHardware::blitterCopy(uint16_t* source, uint16_t* destination, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t shift, uint16_t firstWordMask, uint16_t lastWordMask, uint16_t mask)
{
    AmigaHardware::setInterrupts(INTF_BLIT, false);
    if (!isBlitterBusy() && !hasQueuedBlits) {
        *bltafwmPointer = firstWordMask;
        *bltalwmPointer = lastWordMask;
        *bltcon1Pointer = (uint16_t)(shift < 0 ? BLITREVERSE : 0);
        *bltcon0Pointer = (uint16_t)(BC0F_SRCA | BC0F_DEST | ABC | ABNC | ((shift < 0 ? -shift : shift) << 12));
        *bltamodPointer = sourceModulo;
        *bltdmodPointer = destinationModulo;
        *bltaptPointer = source;
        *bltbdatPointer = mask;
        *bltdptPointer = destination;
        *bltsizePointer = (uint16_t)((height << 6) | width);
#ifdef DEBUG
        *color00Pointer = 0xf00;
#endif
    } else {
        *blitterQueueAddPosition++ = 12;
        if (blitterQueueAddPosition + 24 >= blitterQueueBufferEnd) {
            blitterQueueAddPosition = blitterQueueBuffer;
        }
        *blitterQueueAddPosition++ = bltafwm;
        *blitterQueueAddPosition++ = firstWordMask;
        *blitterQueueAddPosition++ = bltalwm;
        *blitterQueueAddPosition++ = lastWordMask;
        *blitterQueueAddPosition++ = bltcon1;
        *blitterQueueAddPosition++ = (uint16_t)(shift < 0 ? BLITREVERSE : 0);
        *blitterQueueAddPosition++ = bltcon0;
        *blitterQueueAddPosition++ = (uint16_t)(BC0F_SRCA | BC0F_DEST | ABC | ABNC | ((shift < 0 ? -shift : shift) << 12));
        *blitterQueueAddPosition++ = bltamod;
        *blitterQueueAddPosition++ = sourceModulo;
        *blitterQueueAddPosition++ = bltdmod;
        *blitterQueueAddPosition++ = destinationModulo;
        *blitterQueueAddPosition++ = bltapth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(source) >> 16);
        *blitterQueueAddPosition++ = bltaptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)source);
        *blitterQueueAddPosition++ = bltbdat;
        *blitterQueueAddPosition++ = mask;
        *blitterQueueAddPosition++ = bltdpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(destination) >> 16);
        *blitterQueueAddPosition++ = bltdptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)destination);
        *blitterQueueAddPosition++ = bltsize;
        *blitterQueueAddPosition++ = (uint16_t)((height << 6) | width);
        hasQueuedBlits = true;
        processBlitterQueue();
    }
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}

void AmigaHardware::blitterCopyWithMask(uint16_t* source, uint16_t* destination, uint16_t* mask, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t maskModulo, int16_t sourceShift, int16_t maskShift, uint16_t firstWordMask, uint16_t lastWordMask, bool clearMasked)
{
    AmigaHardware::setInterrupts(INTF_BLIT, false);
    if (!isBlitterBusy() && !hasQueuedBlits) {
        *bltafwmPointer = firstWordMask;
        *bltalwmPointer = lastWordMask;
        *bltcon1Pointer = (uint16_t)((sourceShift < 0 ? BLITREVERSE : 0) | ((maskShift < 0 ? -maskShift : maskShift) << 12));
        *bltcon0Pointer = (uint16_t)(BC0F_SRCA | BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC | (clearMasked ? 0 : (NANBC | ANBC)) | ((sourceShift < 0 ? -sourceShift : sourceShift) << 12));
        *bltamodPointer = sourceModulo;
        *bltbmodPointer = maskModulo;
        *bltcmodPointer = destinationModulo;
        *bltdmodPointer = destinationModulo;
        *bltaptPointer = source;
        *bltbptPointer = mask;
        *bltcptPointer = destination;
        *bltdptPointer = destination;
        *bltsizePointer = (uint16_t)((height << 6) | width);
#ifdef DEBUG
        *color00Pointer = 0xf00;
#endif
    } else {
        *blitterQueueAddPosition++ = 17;
        if (blitterQueueAddPosition + 34 >= blitterQueueBufferEnd) {
            blitterQueueAddPosition = blitterQueueBuffer;
        }
        *blitterQueueAddPosition++ = bltafwm;
        *blitterQueueAddPosition++ = firstWordMask;
        *blitterQueueAddPosition++ = bltalwm;
        *blitterQueueAddPosition++ = lastWordMask;
        *blitterQueueAddPosition++ = bltcon1;
        *blitterQueueAddPosition++ = (uint16_t)((sourceShift < 0 ? BLITREVERSE : 0) | ((maskShift < 0 ? -maskShift : maskShift) << 12));
        *blitterQueueAddPosition++ = bltcon0;
        *blitterQueueAddPosition++ = (uint16_t)(BC0F_SRCA | BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC | (clearMasked ? 0 : (NANBC | ANBC)) | ((sourceShift < 0 ? -sourceShift : sourceShift) << 12));
        *blitterQueueAddPosition++ = bltamod;
        *blitterQueueAddPosition++ = sourceModulo;
        *blitterQueueAddPosition++ = bltbmod;
        *blitterQueueAddPosition++ = maskModulo;
        *blitterQueueAddPosition++ = bltcmod;
        *blitterQueueAddPosition++ = destinationModulo;
        *blitterQueueAddPosition++ = bltdmod;
        *blitterQueueAddPosition++ = destinationModulo;
        *blitterQueueAddPosition++ = bltapth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(source) >> 16);
        *blitterQueueAddPosition++ = bltaptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)source);
        *blitterQueueAddPosition++ = bltbpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(mask) >> 16);
        *blitterQueueAddPosition++ = bltbptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)mask);
        *blitterQueueAddPosition++ = bltcpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(destination) >> 16);
        *blitterQueueAddPosition++ = bltcptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)destination);
        *blitterQueueAddPosition++ = bltdpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(destination) >> 16);
        *blitterQueueAddPosition++ = bltdptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)destination);
        *blitterQueueAddPosition++ = bltsize;
        *blitterQueueAddPosition++ = (uint16_t)((height << 6) | width);
        hasQueuedBlits = true;
        processBlitterQueue();
    }
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}

void AmigaHardware::blitterLine(uint16_t* data, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t bytesPerRow, bool singleBitPerRow)
{
    if (singleBitPerRow && y1 == y2) {
        return;
    }

    // Make sure the line is drawn from top to bottom
    if (y2 < y1) {
        uint16_t temp = x2;
        x2 = x1;
        x1 = temp;

        temp = y2;
        y2 = y1;
        y1 = temp;
    }

    uint16_t octant = 0;
    int16_t dx = x2 - x1;
    int16_t dy = y2 - y1;
    if (dx == 0 && dy == 0) {
        return;
    }
    if (dx < 0) {
        octant += 2;
        dx = (int16_t)-dx;
    }
    if (dx >= (dy + dy)) {
        dy--;
    }

    uint16_t* firstWord = data + y1 * (bytesPerRow >> 1) + (x1 >> 4);
    if (dy < dx) {
        int16_t signedTemp = dy;
        dy = dx;
        dx = signedTemp;
        octant++;
    }
    uint16_t bltcon0Value = ((x1 & 15) << 12) | BC0F_SRCA | BC0F_SRCC | BC0F_DEST;
    uint16_t bltcon1Value = octants[octant];
    if (singleBitPerRow) {
        bltcon0Value |= A_XOR_C;
        bltcon1Value |= ONEDOT;
    } else {
        bltcon0Value |= A_OR_C;
    }
    int16_t v = dx + dx;

    AmigaHardware::setInterrupts(INTF_BLIT, false);
    if (!isBlitterBusy() && !hasQueuedBlits) {
        *bltawmPointer = 0xffffffff;
        *bltadatPointer = 0x8000;
        *bltbdatPointer = 0xffff;
        *bltcmodPointer = (int16_t)bytesPerRow;
        *bltdmodPointer = (int16_t)bytesPerRow;
        *bltbmodPointer = v;
        v -= dy;
        if (v < 0) {
            bltcon1Value |= SIGNFLAG;
        }
        *bltaptlPointer = v;
        v -= dy;
        *bltamodPointer = v;
        *bltcon0Pointer = bltcon0Value;
        *bltcon1Pointer = bltcon1Value;
        *bltcptPointer = firstWord;
        *bltdptPointer = firstWord;
        *bltsizePointer = (uint16_t)((dy << 6) | 2);
#ifdef DEBUG
        *color00Pointer = 0xf00;
#endif
    } else {
        *blitterQueueAddPosition++ = 16;
        if (blitterQueueAddPosition + 32 >= blitterQueueBufferEnd) {
            blitterQueueAddPosition = blitterQueueBuffer;
        }
        *blitterQueueAddPosition++ = bltafwm;
        *blitterQueueAddPosition++ = 0xffff;
        *blitterQueueAddPosition++ = bltalwm;
        *blitterQueueAddPosition++ = 0xffff;
        *blitterQueueAddPosition++ = bltadat;
        *blitterQueueAddPosition++ = 0x8000;
        *blitterQueueAddPosition++ = bltbdat;
        *blitterQueueAddPosition++ = 0xffff;
        *blitterQueueAddPosition++ = bltcmod;
        *blitterQueueAddPosition++ = (int16_t)bytesPerRow;
        *blitterQueueAddPosition++ = bltdmod;
        *blitterQueueAddPosition++ = (int16_t)bytesPerRow;
        *blitterQueueAddPosition++ = bltbmod;
        *blitterQueueAddPosition++ = v;
        v -= dy;
        if (v < 0) {
            bltcon1Value |= SIGNFLAG;
        }
        *blitterQueueAddPosition++ = bltaptl;
        *blitterQueueAddPosition++ = v;
        v -= dy;
        *blitterQueueAddPosition++ = bltamod;
        *blitterQueueAddPosition++ = v;
        *blitterQueueAddPosition++ = bltcon0;
        *blitterQueueAddPosition++ = bltcon0Value;
        *blitterQueueAddPosition++ = bltcon1;
        *blitterQueueAddPosition++ = bltcon1Value;
        *blitterQueueAddPosition++ = bltcpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(firstWord) >> 16);
        *blitterQueueAddPosition++ = bltcptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)firstWord);
        *blitterQueueAddPosition++ = bltdpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(firstWord) >> 16);
        *blitterQueueAddPosition++ = bltdptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)firstWord);
        *blitterQueueAddPosition++ = bltsize;
        *blitterQueueAddPosition++ = (uint16_t)((dy << 6) | 2);
        hasQueuedBlits = true;
        processBlitterQueue();
    }
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}

void AmigaHardware::blitterFill(uint16_t* data, uint16_t width, uint16_t height, int16_t modulo)
{
    AmigaHardware::setInterrupts(INTF_BLIT, false);
    if (!isBlitterBusy() && !hasQueuedBlits) {
        *bltawmPointer = 0xffffffff;
        *bltcon0Pointer = BC0F_SRCA | BC0F_DEST | A_TO_D;
        *bltcon1Pointer = BLITREVERSE | FILL_OR;
        *bltamodPointer = modulo;
        *bltdmodPointer = modulo;
        *bltaptPointer = data;
        *bltdptPointer = data;
        *bltsizePointer = (uint16_t)((height << 6) | width);
#ifdef DEBUG
        *color00Pointer = 0xf00;
#endif
    } else {
        *blitterQueueAddPosition++ = 11;
        if (blitterQueueAddPosition + 22 >= blitterQueueBufferEnd) {
            blitterQueueAddPosition = blitterQueueBuffer;
        }
        *blitterQueueAddPosition++ = bltafwm;
        *blitterQueueAddPosition++ = 0xffff;
        *blitterQueueAddPosition++ = bltalwm;
        *blitterQueueAddPosition++ = 0xffff;
        *blitterQueueAddPosition++ = bltcon0;
        *blitterQueueAddPosition++ = BC0F_SRCA | BC0F_DEST | A_TO_D;
        *blitterQueueAddPosition++ = bltcon1;
        *blitterQueueAddPosition++ = BLITREVERSE | FILL_OR;
        *blitterQueueAddPosition++ = bltamod;
        *blitterQueueAddPosition++ = modulo;
        *blitterQueueAddPosition++ = bltdmod;
        *blitterQueueAddPosition++ = modulo;
        *blitterQueueAddPosition++ = bltapth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(data) >> 16);
        *blitterQueueAddPosition++ = bltaptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)data);
        *blitterQueueAddPosition++ = bltdpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(data) >> 16);
        *blitterQueueAddPosition++ = bltdptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)data);
        *blitterQueueAddPosition++ = bltsize;
        *blitterQueueAddPosition++ = (uint16_t)((height << 6) | width);
        hasQueuedBlits = true;
        processBlitterQueue();
    }
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}

void AmigaHardware::processBlitterQueue()
{
    if (isBlitterBusy() || !hasQueuedBlits) {
        return;
    }

    uint16_t registerCount = *blitterQueueToBeBlitted++;
    if (blitterQueueToBeBlitted + registerCount + registerCount >= blitterQueueBufferEnd) {
        blitterQueueToBeBlitted = blitterQueueBuffer;
    }
    for (uint16_t i = 0; i < registerCount; i++) {
        uint32_t destination = 0xdff000 + *blitterQueueToBeBlitted++;
        *((volatile uint16_t*)destination) = *blitterQueueToBeBlitted++;
    }
    hasQueuedBlits = (bool)(blitterQueueToBeBlitted != blitterQueueAddPosition ? true : false);
#ifdef DEBUG
    *color00Pointer = 0xf00;
#endif
}

// getVBR had no SAS/C C++ body — it was asm-only in AmigaHardwareAssembler.s.
// Provided here as a portable C++ body for the !ASSEMBLER builds; the ASSEMBLER
// builds reach the asm version (SAS/C directly, GCC via the wrapper below).
static __attribute__((interrupt)) void getVBRSupervisor()
{
    __asm__ volatile(".short 0x4e7a, 0x0801"); // movec.l vbr,d0 (privileged)
}

void* AmigaHardware::getVBR()
{
    return (void*)Supervisor((ULONG (*)())getVBRSupervisor);
}

bool AmigaHardware::isLongFrame()
{
    return (bool)((*vposrPointer & 0x8000) ? true : false);   // VPOSR bit 15 = LOF
}

bool AmigaHardware::isBlitterBusy()
{
    (void)*(volatile uint16_t*)dmaconrPointer;             // A1000 compatibility read
    return (bool)((*(volatile uint16_t*)dmaconrPointer & (1 << 14)) ? true : false);
}

void AmigaHardware::blitterWait()
{
    while (isBlitterBusy())
        ;
}
#endif

// GCC + ASSEMBLER: reach the hand-tuned AmigaHardwareAssembler.s routines via
// register-marshalling wrappers (GCC has no SAS/C register-parameter syntax).
// The blitter routines reference the C++ statics (hasQueuedBlits, octants, the
// queue) — their SAS/C-mangled names are bridged to the GCC ones in the Makefile.
// SAS/C+ASSEMBLER links the asm directly; the C++ bodies above serve !ASSEMBLER.
#if defined(ASSEMBLER) && !defined(__SASC)
void* AmigaHardware::getVBR()
{
    register void* ret __asm("d0");
    __asm volatile("jsr _getVBR__13AmigaHardwareFv"
                   : "=r"(ret) : : "cc", "memory", "d1", "a0", "a1");
    return ret;
}

bool AmigaHardware::isLongFrame()
{
    register bool ret __asm("d0");
    __asm volatile("jsr _isLongFrame__13AmigaHardwareFv"
                   : "=r"(ret) : : "cc", "memory");
    return ret;
}

bool AmigaHardware::isBlitterBusy()
{
    register bool ret __asm("d0");
    __asm volatile("jsr _isBlitterBusy__13AmigaHardwareFv"
                   : "=r"(ret) : : "cc", "memory");
    return ret;
}

void AmigaHardware::blitterWait()
{
    __asm volatile("jsr _blitterWait__13AmigaHardwareFv" : : : "cc", "memory");
}

void AmigaHardware::blitterClear(uint16_t* data, uint16_t width, uint16_t height, int16_t modulo)
{
    register uint16_t* a __asm("d2") = data;     // data   in d2
    register uint16_t  b __asm("d0") = width;    // width  in d0
    register uint16_t  c __asm("d1") = height;   // height in d1
    register int16_t   d __asm("d3") = modulo;   // modulo in d3
    __asm volatile("jsr _blitterClear__13AmigaHardwareFPUsUsUss"
                   : "+r"(a), "+r"(b), "+r"(c), "+r"(d)
                   :
                   : "cc", "memory", "a0", "a1");
}

void AmigaHardware::blitterFill(uint16_t* data, uint16_t width, uint16_t height, int16_t modulo)
{
    register uint16_t* a __asm("d2") = data;     // data   in d2
    register uint16_t  b __asm("d0") = width;    // width  in d0
    register uint16_t  c __asm("d1") = height;   // height in d1
    register int16_t   d __asm("d3") = modulo;   // modulo in d3
    __asm volatile("jsr _blitterFill__13AmigaHardwareFPUsUsUss"
                   : "+r"(a), "+r"(b), "+r"(c), "+r"(d)
                   :
                   : "cc", "memory", "a0", "a1");
}

void AmigaHardware::blitterCopy(uint16_t* source, uint16_t* destination, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t shift, uint16_t firstWordMask, uint16_t lastWordMask, uint16_t mask)
{
    register uint16_t* src  __asm("d2") = source;             // source            in d2
    register uint16_t* dst  __asm("d3") = destination;        // destination       in d3
    register uint16_t  w    __asm("d0") = width;              // width             in d0
    register uint16_t  h    __asm("d1") = height;             // height            in d1
    register int16_t   smod __asm("a1") = sourceModulo;       // sourceModulo      in a1
    register int16_t   dmod __asm("a2") = destinationModulo;  // destinationModulo in a2
    register int16_t   sh   __asm("d4") = shift;              // shift             in d4
    register uint16_t  fwm  __asm("d5") = firstWordMask;      // firstWordMask     in d5
    register uint16_t  lwm  __asm("d6") = lastWordMask;       // lastWordMask      in d6
    register uint16_t  msk  __asm("d7") = mask;               // mask              in d7
    __asm volatile("jsr _blitterCopy__13AmigaHardwareFPUsPUsUsUssssUsUsUs"
                   : "+r"(src), "+r"(dst), "+r"(w), "+r"(h), "+r"(smod), "+r"(dmod),
                     "+r"(sh), "+r"(fwm), "+r"(lwm), "+r"(msk)
                   :
                   : "cc", "memory", "a0");
}

void AmigaHardware::blitterCopyWithMask(uint16_t* source, uint16_t* destination, uint16_t* mask, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t maskModulo, int16_t sourceShift, int16_t maskShift, uint16_t firstWordMask, uint16_t lastWordMask, bool clearMasked)
{
    register uint16_t* src  __asm("d2") = source;             // source            in d2
    register uint16_t* dst  __asm("d3") = destination;        // destination       in d3
    register uint16_t* msk  __asm("d4") = mask;               // mask              in d4
    register uint16_t  w    __asm("d0") = width;              // width             in d0
    register uint16_t  h    __asm("d1") = height;             // height            in d1
    register int16_t   smod __asm("a1") = sourceModulo;       // sourceModulo      in a1
    register int16_t   dmod __asm("a2") = destinationModulo;  // destinationModulo in a2
    register int16_t   mmod __asm("a3") = maskModulo;         // maskModulo        in a3
    register int16_t   ssh  __asm("d5") = sourceShift;        // sourceShift       in d5
    register int16_t   msh  __asm("d6") = maskShift;          // maskShift         in d6
    register uint16_t  fwm  __asm("a5") = firstWordMask;      // firstWordMask     in a5
    register uint16_t  lwm  __asm("a6") = lastWordMask;       // lastWordMask      in a6
    register uint16_t  clm  __asm("d7") = clearMasked;        // clearMasked       in d7
    __asm volatile("jsr _blitterCopyWithMask__13AmigaHardwareFPUsPUsPUsUsUssssssUsUsUc"
                   : "+r"(src), "+r"(dst), "+r"(msk), "+r"(w), "+r"(h), "+r"(smod),
                     "+r"(dmod), "+r"(mmod), "+r"(ssh), "+r"(msh), "+r"(fwm), "+r"(lwm), "+r"(clm)
                   :
                   : "cc", "memory", "a0");
}

void AmigaHardware::blitterLine(uint16_t* data, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t bytesPerRow, bool singleBitPerRow)
{
    register uint16_t* d   __asm("d4") = data;              // data            in d4
    register uint16_t  a   __asm("d0") = x1;                // x1              in d0
    register uint16_t  b   __asm("d1") = y1;                // y1              in d1
    register uint16_t  c   __asm("d2") = x2;                // x2              in d2
    register uint16_t  e   __asm("d3") = y2;                // y2              in d3
    register uint16_t  bpr __asm("d5") = bytesPerRow;       // bytesPerRow     in d5
    register bool      sbp __asm("d6") = singleBitPerRow;   // singleBitPerRow in d6
    __asm volatile("jsr _blitterLine__13AmigaHardwareFPUsUsUsUsUsUsUc"
                   : "+r"(d), "+r"(a), "+r"(b), "+r"(c), "+r"(e), "+r"(bpr), "+r"(sbp)
                   :
                   : "cc", "memory", "a0", "a1");
}

void AmigaHardware::processBlitterQueue()
{
    __asm volatile("jsr _processBlitterQueue__13AmigaHardwareFv"
                   : : : "cc", "memory", "d0", "d1", "a0", "a1");
}
#endif

// Masked compositing routines (imported from DanceDiverse3). These have no
// hand-written asm counterpart, so they are unconditional C++ — used by all four
// ASSEMBLER x compiler combinations. They share the blitter queue with the asm
// blitter routines via the same statics/register macros.
void AmigaHardware::blitterFillUp(uint16_t* dest, uint16_t width, uint16_t height, int16_t modulo)
{
    // Single-pass vertical fill-UP: propagate every set bit upward (toward row 0) so the
    // region above each seed bit becomes set.  Done in ONE descending blit with D = A | C,
    // A offset one row BELOW D (C=D): in DESC mode the blitter writes row r then, processing
    // row r-1, reads that just-written row r as A — so the OR chains up the whole plane in a
    // single blit.  Writes `height` rows; the row just below them is the read-only seed.
    // Minterm 0xFA = ABC|ABNC|ANBC|ANBNC|NABC|NANBC = A|C.  `modulo` = bytes between rows of
    // this plane (e.g. interleaved plane stride - row bytes).
    const int32_t rowBytes = (int32_t)width * 2 + modulo;          // stride between plane rows
    uint8_t* dLast = (uint8_t*)dest + (int32_t)(height - 1) * rowBytes + (int32_t)(width - 1) * 2;
    uint8_t* aLast = dLast + rowBytes;                             // one row below
    AmigaHardware::setInterrupts(INTF_BLIT, false);
    while (hasQueuedBlits) processBlitterQueue();
    blitterWait();
    *bltafwmPointer = 0xffff;
    *bltalwmPointer = 0xffff;
    *bltcon1Pointer = BLITREVERSE;                                 // descending
    *bltcon0Pointer = (uint16_t)(BC0F_SRCA | BC0F_SRCC | BC0F_DEST |
                                 ABC | ABNC | ANBC | ANBNC | NABC | NANBC);
    *bltamodPointer = modulo;
    *bltcmodPointer = modulo;
    *bltdmodPointer = modulo;
    *bltaptPointer = (uint16_t*)aLast;
    *bltcptPointer = (uint16_t*)dLast;
    *bltdptPointer = (uint16_t*)dLast;
    *bltsizePointer = (uint16_t)((height << 6) | width);
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}

void AmigaHardware::blitterCombineWithMask(uint16_t* background, uint16_t* source, uint16_t* destination, uint16_t* mask, uint16_t width, uint16_t height, int16_t backgroundModulo, int16_t sourceModulo, int16_t destinationModulo, int16_t maskModulo, int16_t sourceShift, int16_t maskShift, uint16_t firstWordMask, uint16_t lastWordMask, bool clearMasked)
{
    AmigaHardware::setInterrupts(INTF_BLIT, false);
    if (!isBlitterBusy() && !hasQueuedBlits) {
        *bltafwmPointer = firstWordMask;
        *bltalwmPointer = lastWordMask;
        *bltcon1Pointer = (uint16_t)((sourceShift < 0 ? BLITREVERSE : 0) | ((maskShift < 0 ? -maskShift : maskShift) << 12));
        *bltcon0Pointer = (uint16_t)((source ? BC0F_SRCA : 0) | BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC | (clearMasked ? 0 : (NANBC | ANBC)) | ((sourceShift < 0 ? -sourceShift : sourceShift) << 12));
        *bltbmodPointer = maskModulo;
        *bltcmodPointer = backgroundModulo;
        *bltdmodPointer = destinationModulo;
        if (source) {
            *bltamodPointer = sourceModulo;
            *bltaptPointer = source;
        } else {
            *bltadatPointer = 0;
        }
        *bltbptPointer = mask;
        *bltcptPointer = background;
        *bltdptPointer = destination;
        *bltsizePointer = (uint16_t)((height << 6) | width);
#ifdef DEBUG
        *color00Pointer = 0xf00;
#endif
    } else {
        *blitterQueueAddPosition++ = (uint16_t)(source ? 17 : 15);
        if (blitterQueueAddPosition + (source ? 34 : 30) >= blitterQueueBufferEnd) {
            blitterQueueAddPosition = blitterQueueBuffer;
        }
        *blitterQueueAddPosition++ = bltafwm;
        *blitterQueueAddPosition++ = firstWordMask;
        *blitterQueueAddPosition++ = bltalwm;
        *blitterQueueAddPosition++ = lastWordMask;
        *blitterQueueAddPosition++ = bltcon1;
        *blitterQueueAddPosition++ = (uint16_t)((sourceShift < 0 ? BLITREVERSE : 0) | ((maskShift < 0 ? -maskShift : maskShift) << 12));
        *blitterQueueAddPosition++ = bltcon0;
        *blitterQueueAddPosition++ = (uint16_t)((source ? BC0F_SRCA : 0) | BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC | (clearMasked ? 0 : (NANBC | ANBC)) | ((sourceShift < 0 ? -sourceShift : sourceShift) << 12));
        *blitterQueueAddPosition++ = bltbmod;
        *blitterQueueAddPosition++ = maskModulo;
        *blitterQueueAddPosition++ = bltcmod;
        *blitterQueueAddPosition++ = backgroundModulo;
        *blitterQueueAddPosition++ = bltdmod;
        *blitterQueueAddPosition++ = destinationModulo;
        if (source) {
            *blitterQueueAddPosition++ = bltamod;
            *blitterQueueAddPosition++ = sourceModulo;
            *blitterQueueAddPosition++ = bltapth;
            *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(source) >> 16);
            *blitterQueueAddPosition++ = bltaptl;
            *blitterQueueAddPosition++ = (uint16_t)((uint32_t)source);
        } else {
            *blitterQueueAddPosition++ = bltadat;
            *blitterQueueAddPosition++ = 0;
        }
        *blitterQueueAddPosition++ = bltbpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(mask) >> 16);
        *blitterQueueAddPosition++ = bltbptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)mask);
        *blitterQueueAddPosition++ = bltcpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(background) >> 16);
        *blitterQueueAddPosition++ = bltcptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)background);
        *blitterQueueAddPosition++ = bltdpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(destination) >> 16);
        *blitterQueueAddPosition++ = bltdptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)destination);
        *blitterQueueAddPosition++ = bltsize;
        *blitterQueueAddPosition++ = (uint16_t)((height << 6) | width);
        hasQueuedBlits = true;
        processBlitterQueue();
    }
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}

void AmigaHardware::blitterPatternWithMask(uint16_t pattern, uint16_t* destination, uint16_t* mask, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t maskModulo, int16_t sourceShift, int16_t maskShift, uint16_t firstWordMask, uint16_t lastWordMask, bool clearMasked)
{
    AmigaHardware::setInterrupts(INTF_BLIT, false);
    if (!isBlitterBusy() && !hasQueuedBlits) {
        *bltafwmPointer = firstWordMask;
        *bltalwmPointer = lastWordMask;
        *bltcon1Pointer = (uint16_t)((sourceShift < 0 ? BLITREVERSE : 0) | ((maskShift < 0 ? -maskShift : maskShift) << 12));
        *bltcon0Pointer = (uint16_t)(BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC | (clearMasked ? 0 : (NANBC | ANBC)) | ((sourceShift < 0 ? -sourceShift : sourceShift) << 12));
        *bltbmodPointer = maskModulo;
        *bltcmodPointer = destinationModulo;
        *bltdmodPointer = destinationModulo;
        *bltadatPointer = pattern;
        *bltbptPointer = mask;
        *bltcptPointer = destination;
        *bltdptPointer = destination;
        *bltsizePointer = (uint16_t)((height << 6) | width);
#ifdef DEBUG
        *color00Pointer = 0xf00;
#endif
    } else {
        *blitterQueueAddPosition++ = 15;
        if (blitterQueueAddPosition + 30 >= blitterQueueBufferEnd) {
            blitterQueueAddPosition = blitterQueueBuffer;
        }
        *blitterQueueAddPosition++ = bltafwm;
        *blitterQueueAddPosition++ = firstWordMask;
        *blitterQueueAddPosition++ = bltalwm;
        *blitterQueueAddPosition++ = lastWordMask;
        *blitterQueueAddPosition++ = bltcon1;
        *blitterQueueAddPosition++ = (uint16_t)((sourceShift < 0 ? BLITREVERSE : 0) | ((maskShift < 0 ? -maskShift : maskShift) << 12));
        *blitterQueueAddPosition++ = bltcon0;
        *blitterQueueAddPosition++ = (uint16_t)(BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC | (clearMasked ? 0 : (NANBC | ANBC)) | ((sourceShift < 0 ? -sourceShift : sourceShift) << 12));
        *blitterQueueAddPosition++ = bltbmod;
        *blitterQueueAddPosition++ = maskModulo;
        *blitterQueueAddPosition++ = bltcmod;
        *blitterQueueAddPosition++ = destinationModulo;
        *blitterQueueAddPosition++ = bltdmod;
        *blitterQueueAddPosition++ = destinationModulo;
        *blitterQueueAddPosition++ = bltadat;
        *blitterQueueAddPosition++ = pattern;
        *blitterQueueAddPosition++ = bltbpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(mask) >> 16);
        *blitterQueueAddPosition++ = bltbptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)mask);
        *blitterQueueAddPosition++ = bltcpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(destination) >> 16);
        *blitterQueueAddPosition++ = bltcptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)destination);
        *blitterQueueAddPosition++ = bltdpth;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)(destination) >> 16);
        *blitterQueueAddPosition++ = bltdptl;
        *blitterQueueAddPosition++ = (uint16_t)((uint32_t)destination);
        *blitterQueueAddPosition++ = bltsize;
        *blitterQueueAddPosition++ = (uint16_t)((height << 6) | width);
        hasQueuedBlits = true;
        processBlitterQueue();
    }
    AmigaHardware::setInterrupts(INTF_BLIT, true);
}
