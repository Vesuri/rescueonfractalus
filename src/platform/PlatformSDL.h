#ifndef PLATFORMSDL_H
#define PLATFORMSDL_H

/* Following the PETSCIIRobots-SDL convention: define PlatformClass so that
   main.cpp can instantiate PlatformClass() without knowing the concrete type. */
#define PlatformClass PlatformSDL

#include <SDL.h>
#include "Platform.h"

/* Screen geometry.  The Atari runs ANTIC mode F at 320×(display list height)
   internally; we present at 2× scale in the SDL window. */
#define ROF_NATIVE_W  320
#define ROF_NATIVE_H  240
#define ROF_WINDOW_W  640
#define ROF_WINDOW_H  480

/* Number of entries in the VBI address→function dispatch table. */
#define VBI_TABLE_SIZE 16

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
    void renderPMGraphics(uint32_t bgPx);
    SDL_Color atariColor(uint8_t c) const;

    /* POKEY audio helpers (called under audio lock). */
    void    updateChannelFreq(int ch, uint8_t audf);
    void    updateChannelCtrl(int ch, uint8_t audc);
    uint8_t audctlAutf(int ch);

    /* Input helpers. */
    uint8_t readJoystick();
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

    /* Display list pointer cached from the most recent DLISTL/H write */
    uint16_t displayListPtr;

    int framesPerSecond_;
};

#endif /* PLATFORMSDL_H */
