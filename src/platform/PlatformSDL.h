#ifndef PLATFORMSDL_H
#define PLATFORMSDL_H

/* Following the PETSCIIRobots-SDL convention: define PlatformClass so that
   main.cpp can instantiate PlatformClass() without knowing the concrete type. */
#define PlatformClass PlatformSDL

#include <SDL.h>
#include "Platform.h"

/* Screen geometry.
   ROF_NATIVE_W = 384: the full WIDE (DMACTL bits=11) ANTIC playfield output.
   Screenshots crop to ROF_SCREENSHOT_W = 336px (centre of the 384px frame,
   trimming 24px per side) to match atari800's F10 capture format. */
#define ROF_NATIVE_W      384
#define ROF_NATIVE_H      240
#define ROF_WINDOW_W      768   /* ROF_NATIVE_W * 2 */
#define ROF_WINDOW_H      480
#define ROF_SCREENSHOT_W  336   /* centre crop: x=24..359 of the 384px buffer */
#define ROF_SCREENSHOT_X0  24   /* first column of the screenshot crop */

/* Number of entries in the VBI address→function dispatch table. */
#define VBI_TABLE_SIZE 32

struct PokeyChannel {
    float   phase;      /* oscillator phase accumulator (in samples) */
    float   period;     /* half-period in samples (0 = silent) */
    int16_t amplitude;  /* output amplitude per sample */
    int     sign;       /* current output sign (+1/-1) */
};

class PlatformSDL : public Platform {
public:
    explicit PlatformSDL(const char* imagePath);
    virtual ~PlatformSDL();

    virtual void    setInterrupt(void (*fn)(void)) override;
    virtual int     framesPerSecond()              override;
    virtual void    renderFrame()                  override;
    virtual void    pollEvents()                   override;
    virtual void    tickVBI()                      override;
    virtual uint8_t hwRead(uint16_t addr)          override;
    virtual void    hwWrite(uint16_t addr, uint8_t val) override;
    virtual void    shadowWrite(uint16_t addr, uint8_t val) override;
    virtual int     loadImage(const char* path)    override;

    virtual void registerVBI(uint16_t addr, void (*fn)(void)) override;
    virtual void indirectJmp(uint16_t addr) override;

    /* Audio sample counter — incremented by the audio callback,
       read to decide when to fire the VBI.  Public so the static
       callback can reach it via the Platform* pointer.            */
    int samplesSinceInterrupt;
    int interruptIntervalInSamples;

private:
    static void audioCallback(void* userdata, uint8_t* stream, int bytes);
    void buildPalette();
    void renderAtariDisplay();
    void renderPMGraphicsRange(int fromY, int toY);
    SDL_Color atariColor(uint8_t c) const;
    void saveScreenshot();

    /* POKEY audio helpers (called under audio lock). */
    void    updateChannelFreq(int ch, uint8_t audf);
    void    updateChannelCtrl(int ch, uint8_t audc);
    uint8_t audctlAutf(int ch);

    /* Input helpers. */
    uint8_t readJoystick();
    uint8_t readTrigger(int stick);
    uint8_t readConsol();

    /* SDL objects */
    SDL_Window*   window;
    SDL_Surface*  windowSurface;
    SDL_Surface*  bufferSurface;
    SDL_AudioSpec audioSpec;
    SDL_AudioDeviceID audioDeviceID;

    /* VBI interrupt */
    void (*interruptFn)(void);

    /* VBI address→function dispatch table */
    struct VBIEntry { uint16_t addr; void (*fn)(void); };
    VBIEntry vbiTable[VBI_TABLE_SIZE];
    int      vbiTableCount;

    /* POKEY audio — 4 channels updated under audio lock */
    PokeyChannel channels[4];
    uint8_t      audctl;

    /* Atari NTSC palette (256 entries; bit 0 of colour byte is ignored) */
    SDL_Color palette[256];

    /* Hardware colour registers — updated by hwWrite for $D012-$D01A.
       Index: 0=COLPM0 1=COLPM1 2=COLPM2 3=COLPM3
              4=COLPF0 5=COLPF1 6=COLPF2 7=COLPF3 8=COLBK
       Initialised from OS shadows at the start of each renderAtariDisplay().
       DLI handlers update these mid-scan via hwWrite so subsequent rows
       in the renderer pick up the new values.                            */
    uint8_t colHW[9];

    /* Display list pointer cached from the most recent DLISTL/H write */
    uint16_t displayListPtr;
    /* Character set base page from CHBASE ($02F4 shadow / $D209 write).
       Charset address = chbase << 8.  Game sets this to $04 in init_B800. */
    uint8_t  chbase;
    /* DMACTL ($D400 / $022F shadow): controls ANTIC DMA.
       Bit 5 = DMA enable; bits 2-0 = playfield width.
       If bit 5 clear, renderer skips the display list (blank screen). */
    uint8_t  dmactl;
    /* Smooth scroll offsets (0-15 for fine scroll) */
    uint8_t  hscrol, vscrol;
    /* GRACTL: bit 0 = missile enable, bit 1 = player enable, bit 2 = trigger latch */
    uint8_t  gractl;
    /* PMBASE shadow (page number for PM bitmaps) */
    uint8_t  pmbase;
    /* Player/Missile horizontal position shadows ($D000-$D007) */
    uint8_t  hposP[4];   /* HPOSP0-3 */
    uint8_t  hposM[4];   /* HPOSM0-3 */
    /* Player/Missile size register ($D008-$D00B) and missile size ($D00C) */
    uint8_t  sizePM[4];  /* SIZEP0-3 */
    uint8_t  sizeM;      /* SIZEM: bits 7:6=M3, 5:4=M2, 3:2=M1, 1:0=M0; 00=normal,01=double,11=quad */
    /* GTIA PRIOR ($D01B / $026F shadow): bits 7:6 select GTIA display mode.
       00=normal, 01=mode9 (16 luma), 10=mode10 (9 colours), 11=mode11.
       Updated by hwWrite($D01B) and shadowWrite($026F); seeded from
       mem[$026F] in tickVBI() before the VBI handler runs.              */
    uint8_t  gprior;
    int      framesPerSecond_;
    uint8_t  vcountReg;     /* virtual VCOUNT — cycles 0-127 per frame */

    /* Wall-clock timestamp of the last VBI tick (SDL_GetTicks units, ms).
       tickVBI() uses this to fire at exactly framesPerSecond_ Hz without
       any dependency on the audio device being open.                    */
    uint32_t lastVBITicks;

    /* Set by tickVBI() after firing the VBI; consumed by renderFrame(). */
    volatile bool renderNeeded;
    int screenshotIndex;
};

#endif /* PLATFORMSDL_H */
