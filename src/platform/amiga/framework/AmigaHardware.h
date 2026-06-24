#ifndef _AMIGAHARDWARE_H
#define _AMIGAHARDWARE_H

#include "Util.h"

#define INTF_ALL ((uint16_t)~INTF_SETCLR)
#define BLITTER_QUEUE_SIZE 12000

class CopperList;
class Palette;

class AmigaHardware {
private:
    static uint16_t octants[4];
    static uint16_t blitterQueueBuffer[BLITTER_QUEUE_SIZE + 1];
    static uint16_t* blitterQueueBufferEnd;
    static uint16_t* blitterQueueToBeBlitted;
    static uint16_t* blitterQueueAddPosition;
    static bool modifyingBlitterQueue;

public:
    static bool hasAGAChipSet;
    static volatile bool hasQueuedBlits;
#if defined(ASSEMBLER) && defined(__SASC)
    __asm static void* getVBR(void);
#else
    static void* getVBR(void);
#endif
    static void setCopperList(const CopperList& copperList, bool immediate = false);
    static void setPalette(uint16_t colorIndex, const Palette& palette);
    static void setColor(uint16_t colorIndex, uint16_t color);
    static void setPlayfield(uint16_t width, uint16_t height, uint8_t bitplaneCount, bool interleaved, bool hires = false, bool interlace = false, bool dualPlayfield = false, bool holdAndModify = false, uint16_t centerY = 0xa8);
    static void setSpritesEnabled(bool enabled);
    static void setBlitterNasty(bool enabled);
    static void setDMAChannels(uint16_t dmaChannels, bool enabled);
    static void setInterrupts(uint16_t interrupts, bool enabled);
    __inline static uint16_t enabledDMAChannels();
    __inline static uint16_t enabledInterrupts();
    __inline static void clearInterruptRequests(uint16_t interrupts);
    __inline static bool isLeftMouseButtonPressed();
    __inline static bool isRightMouseButtonPressed();
    // Busy-wait until the raster beam reaches (>=) the given PAL scanline, by
    // polling VPOSR/VHPOSR (the real beam-position registers).  This is the
    // matching Amiga construct for the Atari's VCOUNT polls (wait_vcount_eq /
    // wait_vcount_ge_7a, $D40B): a genuine hardware raster sync, not a frame wait.
    static void waitBeamLine(uint16_t line);
#if defined(ASSEMBLER) && defined(__SASC)
    __asm static bool isLongFrame();
    __asm static bool isBlitterBusy();
    __asm static void blitterWait();
#else
    static bool isLongFrame();
    static bool isBlitterBusy();
    static void blitterWait();
#endif
#if defined(ASSEMBLER) && defined(__SASC)
    __asm static void blitterClear(register __d2 uint16_t* data, register __d0 uint16_t width, register __d1 uint16_t height, register __d3 int16_t modulo);
    __asm static void blitterFill(register __d2 uint16_t* data, register __d0 uint16_t width, register __d1 uint16_t height, register __d3 int16_t modulo);
    __asm static void blitterCopy(register __d2 uint16_t* source, register __d3 uint16_t* destination, register __d0 uint16_t width, register __d1 uint16_t height, register __a1 int16_t sourceModulo, register __a2 int16_t destinationModulo, register __d4 int16_t shift, register __d5 uint16_t firstWordMask, register __d6 uint16_t lastWordMask, register __d7 uint16_t mask);
    __asm static void blitterCopyWithMask(register __d2 uint16_t* source, register __d3 uint16_t* destination, register __d4 uint16_t* mask, register __d0 uint16_t width, register __d1 uint16_t height, register __a1 int16_t sourceModulo, register __a2 int16_t destinationModulo, register __a3 int16_t maskModulo, register __d5 int16_t sourceShift, register __d6 int16_t maskShift, register __a5 uint16_t firstWordMask, register __a6 uint16_t lastWordMask, register __d7 bool clearMasked);
    __asm static void blitterLine(register __d4 uint16_t* data, register __d0 uint16_t x1, register __d1 uint16_t y1, register __d2 uint16_t x2, register __d3 uint16_t y2, register __d5 uint16_t bytesPerRow, register __d6 bool singleBitPerRow);
    __asm static void processBlitterQueue();
#else
    static void blitterClear(uint16_t* data, uint16_t width, uint16_t height, int16_t modulo);
    static void blitterFill(uint16_t* data, uint16_t width, uint16_t height, int16_t modulo);
    static void blitterCopy(uint16_t* source, uint16_t* destination, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t shift, uint16_t firstWordMask, uint16_t lastWordMask, uint16_t mask);
    static void blitterCopyWithMask(uint16_t* source, uint16_t* destination, uint16_t* mask, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t maskModulo, int16_t sourceShift, int16_t maskShift, uint16_t firstWordMask, uint16_t lastWordMask, bool clearMasked);
    static void blitterLine(uint16_t* data, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t bytesPerRow, bool singleBitPerRow);
    // Single-pass vertical fill-UP (flight terrain sky): one descending (BLITREVERSE) blit,
    // D = A|C with A one row below D, so each just-written row feeds the next row up.  Fills
    // `height` rows above the seed.  modulo = bytes between rows of the target plane.
    static void blitterFillUp(uint16_t* dest, uint16_t width, uint16_t height, int16_t modulo);
    static void processBlitterQueue();
#endif
    // Masked compositing — portable C++ on both compilers (no hand-written asm
    // counterpart), so unconditional. blitterCombineWithMask draws source through
    // mask over background; blitterPatternWithMask fills destination with a 16-bit
    // pattern through mask. (Imported from DanceDiverse3.)
    static void blitterCombineWithMask(uint16_t* background, uint16_t* source, uint16_t* destination, uint16_t* mask, uint16_t width, uint16_t height, int16_t backgroundModulo, int16_t sourceModulo, int16_t destinationModulo, int16_t maskModulo, int16_t sourceShift, int16_t maskShift, uint16_t firstWordMask, uint16_t lastWordMask, bool clearMasked);
    static void blitterPatternWithMask(uint16_t pattern, uint16_t* destination, uint16_t* mask, uint16_t width, uint16_t height, int16_t sourceModulo, int16_t destinationModulo, int16_t maskModulo, int16_t sourceShift, int16_t maskShift, uint16_t firstWordMask, uint16_t lastWordMask, bool clearMasked);
};

#define bltddat 0x000
#define dmaconr 0x002
#define vposr 0x004
#define vhposr 0x006
#define dskdatr 0x008
#define joy0dat 0x00A
#define joy1dat 0x00C
#define clxdat 0x00E
#define adkconr 0x010
#define pot0dat 0x012
#define pot1dat 0x014
#define potinp 0x016
#define serdatr 0x018
#define dskbytr 0x01A
#define intenar 0x01C
#define intreqr 0x01E
#define dskpt 0x020
#define dsklen 0x024
#define dskdat 0x026
#define refptr 0x028
#define vposw 0x02A
#define vhposw 0x02C
#define copcon 0x02E
#define serdat 0x030
#define serper 0x032
#define potgo 0x034
#define joytest 0x036
#define strequ 0x038
#define strvbl 0x03A
#define strhor 0x03C
#define strlong 0x03E
#define bltcon0 0x040
#define bltcon1 0x042
#define bltafwm 0x044
#define bltalwm 0x046
#define bltcpt 0x048
#define bltcpth 0x048
#define bltcptl 0x04A
#define bltbpt 0x04C
#define bltbpth 0x04C
#define bltbptl 0x04E
#define bltapt 0x050
#define bltapth 0x050
#define bltaptl 0x052
#define bltdpt 0x054
#define bltdpth 0x054
#define bltdptl 0x056
#define bltsize 0x058
#define bltcon0l 0x05B // note: byte access only
#define bltsizv 0x05C
#define bltsizh 0x05E
#define bltcmod 0x060
#define bltbmod 0x062
#define bltamod 0x064
#define bltdmod 0x066
#define bltcdat 0x070
#define bltbdat 0x072
#define bltadat 0x074
#define deniseid 0x07C
#define dsksync 0x07E
#define cop1lc 0x080
#define cop2lc 0x084
#define copjmp1 0x088
#define copjmp2 0x08A
#define copins 0x08C
#define diwstrt 0x08E
#define diwstop 0x090
#define ddfstrt 0x092
#define ddfstop 0x094
#define dmacon 0x096
#define clxcon 0x098
#define intena 0x09A
#define intreq 0x09C
#define adkcon 0x09E
#define aud0ptr 0x0a0
#define aud0len 0x0a4
#define aud0per 0x0a6
#define aud0vol 0x0a8
#define aud0dat 0x0aa
#define aud1ptr 0x0b0
#define aud1len 0x0b4
#define aud1per 0x0b6
#define aud1vol 0x0b8
#define aud1dat 0x0ba
#define aud2ptr 0x0c0
#define aud2len 0x0c4
#define aud2per 0x0c6
#define aud2vol 0x0c8
#define aud2dat 0x0ca
#define aud3ptr 0x0d0
#define aud3len 0x0d4
#define aud3per 0x0d6
#define aud3vol 0x0d8
#define aud3dat 0x0da
#define bpl1pth 0x0e0
#define bpl1ptl 0x0e2
#define bpl2pth 0x0e4
#define bpl2ptl 0x0e6
#define bpl3pth 0x0e8
#define bpl3ptl 0x0ea
#define bpl4pth 0x0ec
#define bpl4ptl 0x0ee
#define bpl5pth 0x0f0
#define bpl5ptl 0x0f2
#define bpl6pth 0x0f4
#define bpl6ptl 0x0f6
#define bplcon0 0x100
#define bplcon1 0x102
#define bplcon2 0x104
#define bplcon3 0x106
#define bpl1mod 0x108
#define bpl2mod 0x10A
#define bplcon4 0x10C
#define clxcon2 0x10E
#define bpl1dat 0x110
#define bpl2dat 0x112
#define bpl3dat 0x114
#define bpl4dat 0x116
#define bpl5dat 0x118
#define bpl6dat 0x11a
#define spr1pth 0x120
#define spr1ptl 0x122
#define spr2pth 0x124
#define spr2ptl 0x126
#define spr3pth 0x128
#define spr3ptl 0x12a
#define spr4pth 0x12c
#define spr4ptl 0x12e
#define spr5pth 0x130
#define spr5ptl 0x132
#define spr6pth 0x134
#define spr6ptl 0x136
#define spr7pth 0x138
#define spr7ptl 0x13a
#define spr8pth 0x13c
#define spr8ptl 0x13e
#define spr1pos 0x140
#define spr1ctl 0x142
#define spr1data 0x144
#define spr1datb 0x146
#define spr2pos 0x148
#define spr2ctl 0x14a
#define spr2data 0x14c
#define spr2datb 0x14e
#define spr3pos 0x150
#define spr3ctl 0x152
#define spr3data 0x154
#define spr3datb 0x156
#define spr4pos 0x158
#define spr4ctl 0x15a
#define spr4data 0x15c
#define spr4datb 0x15e
#define spr5pos 0x160
#define spr5ctl 0x162
#define spr5data 0x164
#define spr5datb 0x166
#define spr6pos 0x168
#define spr6ctl 0x16a
#define spr6data 0x16c
#define spr6datb 0x16e
#define spr7pos 0x170
#define spr7ctl 0x172
#define spr7data 0x174
#define spr7datb 0x176
#define spr8pos 0x178
#define spr8ctl 0x17a
#define spr8data 0x17c
#define spr8datb 0x17e
#define color00 0x180
#define color01 0x182
#define color02 0x184
#define color03 0x186
#define color04 0x188
#define color05 0x18a
#define color06 0x18c
#define color07 0x18e
#define color08 0x190
#define color09 0x192
#define color10 0x194
#define color11 0x196
#define color12 0x198
#define color13 0x19a
#define color14 0x19c
#define color15 0x19e
#define color16 0x1a0
#define color17 0x1a2
#define color18 0x1a4
#define color19 0x1a6
#define color20 0x1a8
#define color21 0x1aa
#define color22 0x1ac
#define color23 0x1ae
#define color24 0x1b0
#define color25 0x1b2
#define color26 0x1b4
#define color27 0x1b6
#define color28 0x1b8
#define color29 0x1ba
#define color30 0x1bc
#define color31 0x1be
#define htotal 0x1c0
#define hsstop 0x1c2
#define hbstrt 0x1c4
#define hbstop 0x1c6
#define vtotal 0x1c8
#define vsstop 0x1ca
#define vbstrt 0x1cc
#define vbstop 0x1ce
#define sprhstrt 0x1d0
#define sprhstop 0x1d2
#define bplhstrt 0x1d4
#define bplhstop 0x1d6
#define hhposw 0x1d8
#define hhposr 0x1da
#define beamcon0 0x1dc
#define hsstrt 0x1de
#define vsstrt 0x1e0
#define hcenter 0x1e2
#define diwhigh 0x1e4
#define fmode 0x1fc

#define bltddatPointer ((volatile uint16_t*)0xdff000)
#define dmaconrPointer ((volatile uint16_t*)0xdff002)
#define vposrPointer ((volatile uint16_t*)0xdff004)
#define vhposrPointer ((volatile uint16_t*)0xdff006)
#define dskdatrPointer ((volatile uint16_t*)0xdff008)
#define joy0datPointer ((volatile uint16_t*)0xdff00A)
#define joy1datPointer ((volatile uint16_t*)0xdff00C)
#define clxdatPointer ((volatile uint16_t*)0xdff00E)
#define adkconrPointer ((volatile uint16_t*)0xdff010)
#define pot0datPointer ((volatile uint16_t*)0xdff012)
#define pot1datPointer ((volatile uint16_t*)0xdff014)
#define potinpPointer ((volatile uint16_t*)0xdff016)
#define serdatrPointer ((volatile uint16_t*)0xdff018)
#define dskbytrPointer ((volatile uint16_t*)0xdff01A)
#define intenarPointer ((volatile uint16_t*)0xdff01C)
#define intreqrPointer ((volatile uint16_t*)0xdff01E)
#define dskptPointer ((volatile uint16_t*)0xdff020)
#define dsklenPointer ((volatile uint16_t*)0xdff024)
#define dskdatPointer ((volatile uint16_t*)0xdff026)
#define refptrPointer ((volatile uint16_t*)0xdff028)
#define vposwPointer ((volatile uint16_t*)0xdff02A)
#define vhposwPointer ((volatile uint16_t*)0xdff02C)
#define copconPointer ((volatile uint16_t*)0xdff02E)
#define serdatPointer ((volatile uint16_t*)0xdff030)
#define serperPointer ((volatile uint16_t*)0xdff032)
#define potgoPointer ((volatile uint16_t*)0xdff034)
#define joytestPointer ((volatile uint16_t*)0xdff036)
#define strequPointer ((volatile uint16_t*)0xdff038)
#define strvblPointer ((volatile uint16_t*)0xdff03A)
#define strhorPointer ((volatile uint16_t*)0xdff03C)
#define strlongPointer ((volatile uint16_t*)0xdff03E)
#define bltcon0Pointer ((volatile uint16_t*)0xdff040)
#define bltcon1Pointer ((volatile uint16_t*)0xdff042)
#define bltawmPointer ((volatile uint32_t*)0xdff044)
#define bltafwmPointer ((volatile uint16_t*)0xdff044)
#define bltalwmPointer ((volatile uint16_t*)0xdff046)
#define bltcptPointer ((uint16_t* volatile *)0xdff048)
#define bltbptPointer ((uint16_t* volatile *)0xdff04C)
#define bltaptPointer ((uint16_t* volatile *)0xdff050)
#define bltaptlPointer ((volatile uint16_t*)0xdff052)
#define bltdptPointer ((uint16_t* volatile *)0xdff054)
#define bltsizePointer ((volatile uint16_t*)0xdff058)
#define bltcon0lPointer ((volatile uint16_t*)0xdff05B) // note: byte access only
#define bltsizvPointer ((volatile uint16_t*)0xdff05C)
#define bltsizhPointer ((volatile uint16_t*)0xdff05E)
#define bltcmodPointer ((volatile int16_t*)0xdff060)
#define bltbmodPointer ((volatile int16_t*)0xdff062)
#define bltamodPointer ((volatile int16_t*)0xdff064)
#define bltdmodPointer ((volatile int16_t*)0xdff066)
#define bltcdatPointer ((volatile uint16_t*)0xdff070)
#define bltbdatPointer ((volatile uint16_t*)0xdff072)
#define bltadatPointer ((volatile uint16_t*)0xdff074)
#define deniseidPointer ((volatile uint16_t*)0xdff07C)
#define dsksyncPointer ((volatile uint16_t*)0xdff07E)
#define cop1lcPointer ((uint32_t* volatile *)0xdff080)
#define cop2lcPointer ((uint32_t* volatile *)0xdff084)
#define copjmp1Pointer ((volatile uint16_t*)0xdff088)
#define copjmp2Pointer ((volatile uint16_t*)0xdff08A)
#define copinsPointer ((volatile uint16_t*)0xdff08C)
#define diwstrtPointer ((volatile uint16_t*)0xdff08E)
#define diwstopPointer ((volatile uint16_t*)0xdff090)
#define ddfstrtPointer ((volatile uint16_t*)0xdff092)
#define ddfstopPointer ((volatile uint16_t*)0xdff094)
#define dmaconPointer ((volatile uint16_t*)0xdff096)
#define clxconPointer ((volatile uint16_t*)0xdff098)
#define intenaPointer ((volatile uint16_t*)0xdff09A)
#define intreqPointer ((volatile uint16_t*)0xdff09C)
#define adkconPointer ((volatile uint16_t*)0xdff09E)
#define aud0ptrPointer ((volatile uint16_t*)0xdff0a0)
#define aud0lenPointer ((volatile uint16_t*)0xdff0a4)
#define aud0perPointer ((volatile uint16_t*)0xdff0a6)
#define aud0volPointer ((volatile uint16_t*)0xdff0a8)
#define aud0datPointer ((volatile uint16_t*)0xdff0aa)
#define aud1ptrPointer ((volatile uint16_t*)0xdff0b0)
#define aud1lenPointer ((volatile uint16_t*)0xdff0b4)
#define aud1perPointer ((volatile uint16_t*)0xdff0b6)
#define aud1volPointer ((volatile uint16_t*)0xdff0b8)
#define aud1datPointer ((volatile uint16_t*)0xdff0ba)
#define aud2ptrPointer ((volatile uint16_t*)0xdff0c0)
#define aud2lenPointer ((volatile uint16_t*)0xdff0c4)
#define aud2perPointer ((volatile uint16_t*)0xdff0c6)
#define aud2volPointer ((volatile uint16_t*)0xdff0c8)
#define aud2datPointer ((volatile uint16_t*)0xdff0ca)
#define aud3ptrPointer ((volatile uint16_t*)0xdff0d0)
#define aud3lenPointer ((volatile uint16_t*)0xdff0d4)
#define aud3perPointer ((volatile uint16_t*)0xdff0d6)
#define aud3volPointer ((volatile uint16_t*)0xdff0d8)
#define aud3datPointer ((volatile uint16_t*)0xdff0da)
#define bpl1pthPointer ((volatile uint16_t*)0xdff0e0)
#define bpl1ptlPointer ((volatile uint16_t*)0xdff0e2)
#define bpl2pthPointer ((volatile uint16_t*)0xdff0e4)
#define bpl2ptlPointer ((volatile uint16_t*)0xdff0e6)
#define bpl3pthPointer ((volatile uint16_t*)0xdff0e8)
#define bpl3ptlPointer ((volatile uint16_t*)0xdff0ea)
#define bpl4pthPointer ((volatile uint16_t*)0xdff0ec)
#define bpl4ptlPointer ((volatile uint16_t*)0xdff0ee)
#define bpl5pthPointer ((volatile uint16_t*)0xdff0f0)
#define bpl5ptlPointer ((volatile uint16_t*)0xdff0f2)
#define bpl6pthPointer ((volatile uint16_t*)0xdff0f4)
#define bpl6ptlPointer ((volatile uint16_t*)0xdff0f6)
#define bpl1ptPointer ((volatile uint32_t*)0xdff0e0)
#define bpl2ptPointer ((volatile uint32_t*)0xdff0e4)
#define bpl3ptPointer ((volatile uint32_t*)0xdff0e8)
#define bpl4ptPointer ((volatile uint32_t*)0xdff0ec)
#define bpl5ptPointer ((volatile uint32_t*)0xdff0f0)
#define bpl6ptPointer ((volatile uint32_t*)0xdff0f4)
#define bplcon0Pointer ((volatile uint16_t*)0xdff100)
#define bplcon1Pointer ((volatile uint16_t*)0xdff102)
#define bplcon2Pointer ((volatile uint16_t*)0xdff104)
#define bplcon3Pointer ((volatile uint16_t*)0xdff106)
#define bpl1modPointer ((volatile uint16_t*)0xdff108)
#define bpl2modPointer ((volatile uint16_t*)0xdff10A)
#define bplcon4Pointer ((volatile uint16_t*)0xdff10C)
#define clxcon2Pointer ((volatile uint16_t*)0xdff10E)
#define bpl1datPointer ((volatile uint16_t*)0xdff110)
#define bpl2datPointer ((volatile uint16_t*)0xdff112)
#define bpl3datPointer ((volatile uint16_t*)0xdff114)
#define bpl4datPointer ((volatile uint16_t*)0xdff116)
#define bpl5datPointer ((volatile uint16_t*)0xdff118)
#define bpl6datPointer ((volatile uint16_t*)0xdff11a)
#define spr1pthPointer ((volatile uint16_t*)0xdff120)
#define spr1ptlPointer ((volatile uint16_t*)0xdff122)
#define spr2pthPointer ((volatile uint16_t*)0xdff124)
#define spr2ptlPointer ((volatile uint16_t*)0xdff126)
#define spr3pthPointer ((volatile uint16_t*)0xdff128)
#define spr3ptlPointer ((volatile uint16_t*)0xdff12a)
#define spr4pthPointer ((volatile uint16_t*)0xdff12c)
#define spr4ptlPointer ((volatile uint16_t*)0xdff12e)
#define spr5pthPointer ((volatile uint16_t*)0xdff130)
#define spr5ptlPointer ((volatile uint16_t*)0xdff132)
#define spr6pthPointer ((volatile uint16_t*)0xdff134)
#define spr6ptlPointer ((volatile uint16_t*)0xdff136)
#define spr7pthPointer ((volatile uint16_t*)0xdff138)
#define spr7ptlPointer ((volatile uint16_t*)0xdff13a)
#define spr8pthPointer ((volatile uint16_t*)0xdff13c)
#define spr8ptlPointer ((volatile uint16_t*)0xdff13e)
#define spr1ptPointer ((volatile uint32_t*)0xdff120)
#define spr2ptPointer ((volatile uint32_t*)0xdff124)
#define spr3ptPointer ((volatile uint32_t*)0xdff128)
#define spr4ptPointer ((volatile uint32_t*)0xdff12c)
#define spr5ptPointer ((volatile uint32_t*)0xdff130)
#define spr6ptPointer ((volatile uint32_t*)0xdff134)
#define spr7ptPointer ((volatile uint32_t*)0xdff138)
#define spr8ptPointer ((volatile uint32_t*)0xdff13c)
#define spr1posPointer ((volatile uint16_t*)0xdff140)
#define spr1ctlPointer ((volatile uint16_t*)0xdff142)
#define spr1dataPointer ((volatile uint16_t*)0xdff144)
#define spr1datbPointer ((volatile uint16_t*)0xdff146)
#define spr2posPointer ((volatile uint16_t*)0xdff148)
#define spr2ctlPointer ((volatile uint16_t*)0xdff14a)
#define spr2dataPointer ((volatile uint16_t*)0xdff14c)
#define spr2datbPointer ((volatile uint16_t*)0xdff14e)
#define spr3posPointer ((volatile uint16_t*)0xdff150)
#define spr3ctlPointer ((volatile uint16_t*)0xdff152)
#define spr3dataPointer ((volatile uint16_t*)0xdff154)
#define spr3datbPointer ((volatile uint16_t*)0xdff156)
#define spr4posPointer ((volatile uint16_t*)0xdff158)
#define spr4ctlPointer ((volatile uint16_t*)0xdff15a)
#define spr4dataPointer ((volatile uint16_t*)0xdff15c)
#define spr4datbPointer ((volatile uint16_t*)0xdff15e)
#define spr5posPointer ((volatile uint16_t*)0xdff160)
#define spr5ctlPointer ((volatile uint16_t*)0xdff162)
#define spr5dataPointer ((volatile uint16_t*)0xdff164)
#define spr5datbPointer ((volatile uint16_t*)0xdff166)
#define spr6posPointer ((volatile uint16_t*)0xdff168)
#define spr6ctlPointer ((volatile uint16_t*)0xdff16a)
#define spr6dataPointer ((volatile uint16_t*)0xdff16c)
#define spr6datbPointer ((volatile uint16_t*)0xdff16e)
#define spr7posPointer ((volatile uint16_t*)0xdff170)
#define spr7ctlPointer ((volatile uint16_t*)0xdff172)
#define spr7dataPointer ((volatile uint16_t*)0xdff174)
#define spr7datbPointer ((volatile uint16_t*)0xdff176)
#define spr8posPointer ((volatile uint16_t*)0xdff178)
#define spr8ctlPointer ((volatile uint16_t*)0xdff17a)
#define spr8dataPointer ((volatile uint16_t*)0xdff17c)
#define spr8datbPointer ((volatile uint16_t*)0xdff17e)
#define color00Pointer ((volatile uint16_t*)0xdff180)
#define color01Pointer ((volatile uint16_t*)0xdff182)
#define color02Pointer ((volatile uint16_t*)0xdff184)
#define color03Pointer ((volatile uint16_t*)0xdff186)
#define color04Pointer ((volatile uint16_t*)0xdff188)
#define color05Pointer ((volatile uint16_t*)0xdff18a)
#define color06Pointer ((volatile uint16_t*)0xdff18c)
#define color07Pointer ((volatile uint16_t*)0xdff18e)
#define color08Pointer ((volatile uint16_t*)0xdff190)
#define color09Pointer ((volatile uint16_t*)0xdff192)
#define color10Pointer ((volatile uint16_t*)0xdff194)
#define color11Pointer ((volatile uint16_t*)0xdff196)
#define color12Pointer ((volatile uint16_t*)0xdff198)
#define color13Pointer ((volatile uint16_t*)0xdff19a)
#define color14Pointer ((volatile uint16_t*)0xdff19c)
#define color15Pointer ((volatile uint16_t*)0xdff19e)
#define color16Pointer ((volatile uint16_t*)0xdff1a0)
#define color17Pointer ((volatile uint16_t*)0xdff1a2)
#define color18Pointer ((volatile uint16_t*)0xdff1a4)
#define color19Pointer ((volatile uint16_t*)0xdff1a6)
#define color20Pointer ((volatile uint16_t*)0xdff1a8)
#define color21Pointer ((volatile uint16_t*)0xdff1aa)
#define color22Pointer ((volatile uint16_t*)0xdff1ac)
#define color23Pointer ((volatile uint16_t*)0xdff1ae)
#define color24Pointer ((volatile uint16_t*)0xdff1b0)
#define color25Pointer ((volatile uint16_t*)0xdff1b2)
#define color26Pointer ((volatile uint16_t*)0xdff1b4)
#define color27Pointer ((volatile uint16_t*)0xdff1b6)
#define color28Pointer ((volatile uint16_t*)0xdff1b8)
#define color29Pointer ((volatile uint16_t*)0xdff1ba)
#define color30Pointer ((volatile uint16_t*)0xdff1bc)
#define color31Pointer ((volatile uint16_t*)0xdff1be)
#define htotalPointer ((volatile uint16_t*)0xdff1c0)
#define hsstopPointer ((volatile uint16_t*)0xdff1c2)
#define hbstrtPointer ((volatile uint16_t*)0xdff1c4)
#define hbstopPointer ((volatile uint16_t*)0xdff1c6)
#define vtotalPointer ((volatile uint16_t*)0xdff1c8)
#define vsstopPointer ((volatile uint16_t*)0xdff1ca)
#define vbstrtPointer ((volatile uint16_t*)0xdff1cc)
#define vbstopPointer ((volatile uint16_t*)0xdff1ce)
#define sprhstrtPointer ((volatile uint16_t*)0xdff1d0)
#define sprhstopPointer ((volatile uint16_t*)0xdff1d2)
#define bplhstrtPointer ((volatile uint16_t*)0xdff1d4)
#define bplhstopPointer ((volatile uint16_t*)0xdff1d6)
#define hhposwPointer ((volatile uint16_t*)0xdff1d8)
#define hhposrPointer ((volatile uint16_t*)0xdff1da)
#define beamcon0Pointer ((volatile uint16_t*)0xdff1dc)
#define hsstrtPointer ((volatile uint16_t*)0xdff1de)
#define vsstrtPointer ((volatile uint16_t*)0xdff1e0)
#define hcenterPointer ((volatile uint16_t*)0xdff1e2)
#define diwhighPointer ((volatile uint16_t*)0xdff1e4)
#define fmodePointer ((volatile uint16_t*)0xdff1fc)

#define ciaa 0xbfe001
#define ciab 0xbfd000

#define ciapra 0x000
#define ciaprb 0x100
#define ciaddra 0x200
#define ciaddrb 0x300
#define ciatalo 0x400
#define ciatahi 0x500
#define ciatblo 0x600
#define ciatbhi 0x700
#define ciatodlow 0x800
#define ciatodmid 0x900
#define ciatodhi 0xa00
#define ciasdr 0xc00
#define ciaicr 0xd00
#define ciacra 0xe00
#define ciacrb 0xf00

#define ciaapraPointer ((volatile uint8_t*)0xbfe001)
#define ciaaprbPointer ((volatile uint8_t*)0xbfe101)
#define ciaaddraPointer ((volatile uint8_t*)0xbfe201)
#define ciaaddrbPointer ((volatile uint8_t*)0xbfe301)
#define ciaataloPointer ((volatile uint8_t*)0xbfe401)
#define ciaatahiPointer ((volatile uint8_t*)0xbfe501)
#define ciaatbloPointer ((volatile uint8_t*)0xbfe601)
#define ciaatbhiPointer ((volatile uint8_t*)0xbfe701)
#define ciaatodlowPointer ((volatile uint8_t*)0xbfe801)
#define ciaatodmidPointer ((volatile uint8_t*)0xbfe901)
#define ciaatodhiPointer ((volatile uint8_t*)0xbfea01)
#define ciaasdrPointer ((volatile uint8_t*)0xbfec01)
#define ciaaicrPointer ((volatile uint8_t*)0xbfed01)
#define ciaacraPointer ((volatile uint8_t*)0xbfee01)
#define ciaacrbPointer ((volatile uint8_t*)0xbfef01)

#define ciabpraPointer ((volatile uint8_t*)0xbfd000)
#define ciabprbPointer ((volatile uint8_t*)0xbfd100)
#define ciabddraPointer ((volatile uint8_t*)0xbfd200)
#define ciabddrbPointer ((volatile uint8_t*)0xbfd300)
#define ciabtaloPointer ((volatile uint8_t*)0xbfd400)
#define ciabtahiPointer ((volatile uint8_t*)0xbfd500)
#define ciabtbloPointer ((volatile uint8_t*)0xbfd600)
#define ciabtbhiPointer ((volatile uint8_t*)0xbfd700)
#define ciabtodlowPointer ((volatile uint8_t*)0xbfd800)
#define ciabtodmidPointer ((volatile uint8_t*)0xbfd900)
#define ciabtodhiPointer ((volatile uint8_t*)0xbfda00)
#define ciabsdrPointer ((volatile uint8_t*)0xbfdc00)
#define ciabicrPointer ((volatile uint8_t*)0xbfdd00)
#define ciabcraPointer ((volatile uint8_t*)0xbfde00)
#define ciabcrbPointer ((volatile uint8_t*)0xbfdf00)

#endif
