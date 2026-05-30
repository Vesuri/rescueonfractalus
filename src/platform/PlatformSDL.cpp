#include "PlatformSDL.h"
#include <cstdio>
#include <cstring>
#include <cmath>

extern volatile uint8_t mem[65536];

/* POKEY base clock (NTSC, ~1.79 MHz) used for audio frequency calculation. */
static const double POKEY_CLOCK = 1789772.5;
static const double POKEY_DIV   = 28.0;   /* ÷28 = ~63.9 kHz channel clock */

/* ------------------------------------------------------------------ */
/* Constructor / Destructor                                            */
/* ------------------------------------------------------------------ */

PlatformSDL::PlatformSDL(const char* imagePath) :
    window(nullptr), windowSurface(nullptr), bufferSurface(nullptr),
    audioSpec{}, audioDeviceID(0),
    interruptFn(nullptr),
    vbiTableCount(0),
    audctl(0),
    displayListPtr(0), chbase(0xE0), dmactl(0x22), hscrol(0), vscrol(0), gractl(0), pmbase(0),
    hposP{}, hposM{}, sizePM{},
    framesPerSecond_(50), vcountReg(0), colHW{},
    samplesSinceInterrupt(0), interruptIntervalInSamples(0),
    lastVBITicks(0), renderNeeded(false)
{
    memset(channels, 0, sizeof(channels));
    memset(vbiTable, 0, sizeof(vbiTable));

    /* SDL_INIT_JOYSTICK is required even if we don't use joystick input:
       on macOS, it triggers a CoreFoundation run-loop init that CoreAudio
       needs before it can create its HAL thread.  Without it,
       SDL_OpenAudioDevice → COREAUDIO_OpenDevice → SDL_SYS_CreateThread
       calls dlsym() which crashes with an EXC_BAD_ACCESS PAC failure in
       dyld4::RuntimeLocks::withLoadersReadLock.                          */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        quit = true;
        return;
    }

    /* Build Atari NTSC palette (hue/luma → RGB via YIQ). */
    buildPalette();

    /* Audio: mono 16-bit, 44100 Hz, callback-based.  The callback generates
       POKEY samples only — VBI is fired on the main thread by tickVBI().  */
    SDL_AudioSpec want{};
    want.freq     = 44100;
    want.format   = AUDIO_S16LSB;
    want.channels = 1;
    want.samples  = 512;
    want.callback = audioCallback;
    want.userdata = this;
    audioDeviceID = SDL_OpenAudioDevice(nullptr, 0, &want, &audioSpec,
                                        SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (!audioDeviceID) {
        fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
        audioSpec.freq = 44100;
    }
    interruptIntervalInSamples = audioSpec.freq / framesPerSecond_;
    samplesSinceInterrupt      = interruptIntervalInSamples; /* fire on first fill */

    /* Window + surfaces (same dual-surface pattern as PETSCIIRobots-SDL). */
    window = SDL_CreateWindow("Rescue on Fractalus!",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              ROF_WINDOW_W, ROF_WINDOW_H, 0);
    if (!window) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        quit = true;
        return;
    }
    windowSurface = SDL_GetWindowSurface(window);
    bufferSurface = SDL_CreateRGBSurface(0, ROF_NATIVE_W, ROF_NATIVE_H, 32,
                                          0x000000ff, 0x0000ff00,
                                          0x00ff0000, 0xff000000);

    /* Load memory image. */
    if (loadImage(imagePath) != 0) {
        quit = true;
        return;
    }

    /* Seed VBI wall-clock timer now that SDL_GetTicks() is available.    */
    lastVBITicks = SDL_GetTicks();

    /* Start audio (unpaused — same idiom as PETSCII Robots). */
    SDL_PauseAudioDevice(audioDeviceID, 0);

    platform = this;

    /* Pump events and show the window immediately.  On macOS, Cocoa
       requires at least one event-loop iteration before the window
       becomes visible, even though it was just created.  Without this,
       the window stays invisible until renderFrame() is first called
       (which may be hundreds of VBI ticks into game_entry()).          */
    SDL_ShowWindow(window);
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {}  /* drain any queued events */
    }
    SDL_UpdateWindowSurface(window);    /* present the (black) buffer    */
}

PlatformSDL::~PlatformSDL() {
    SDL_CloseAudioDevice(audioDeviceID);
    if (bufferSurface) SDL_FreeSurface(bufferSurface);
    if (window)        SDL_DestroyWindow(window);
    SDL_Quit();
}

/* ------------------------------------------------------------------ */
/* Platform virtual methods                                            */
/* ------------------------------------------------------------------ */

void PlatformSDL::setInterrupt(void (*fn)(void)) {
    interruptFn = fn;
}

int PlatformSDL::framesPerSecond() {
    return framesPerSecond_;
}

int PlatformSDL::loadImage(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    memset((uint8_t*)mem, 0, 65536);
    size_t n = fread((uint8_t*)mem, 1, 65536, f);
    fclose(f);
    printf("[rof] loaded %zu bytes from %s\n", n, path);
    /* Sync cached registers from OS shadow values in the loaded image. */
    displayListPtr = mem[0x0230] | (mem[0x0231] << 8);
    dmactl  = mem[0x022F];  /* SDMCTL shadow */
    chbase  = mem[0x02F4];  /* CHBAS shadow  */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hardware bus                                                        */
/* ------------------------------------------------------------------ */

uint8_t PlatformSDL::hwRead(uint16_t addr) {
    if (addr == 0xD20A) return pokeyRandomStep();   /* POKEY RANDOM */
    if (addr == 0xD300) return readJoystick();       /* PORTA */
    if (addr == 0xD01F) return readConsol();         /* CONSOL */
    if (addr == 0xD40B) {
        /* VCOUNT: advance by 1 each read, cycling 0-127.
           FUN_3c75 does CMP(VCOUNT); BNE loop — waits for VCOUNT == cpu.A.
           FUN_3c7b does LDA VCOUNT; CMP #$7A; BCC loop — waits for >= $7A.
           An auto-incrementing register resolves both waits in at most
           128 loop iterations instead of spinning forever.              */
        vcountReg = (vcountReg + 1) & 0x7F;
        return vcountReg;
    }
    if (addr >= 0xD000 && addr <= 0xD00F) return 0x00; /* collision regs: no collision */
    if (addr == 0xD010) return readTrigger(0);     /* TRIG0: joystick 0 fire */
    if (addr == 0xD011) return readTrigger(1);     /* TRIG1: joystick 1 fire */
    if (addr == 0xD012) return 0x01;               /* TRIG2 (paddle): not pressed */
    if (addr == 0xD013) return 0x01;               /* TRIG3 */
    if (addr == 0xD20F) return 0xFF;               /* SKSTAT: no key, no framing errors */
    if (addr == 0xD20E) return 0xC0;               /* IRQST: timer 1+2 expired (N=1 V=1 for BIT) */
    return 0x00;
}

void PlatformSDL::hwWrite(uint16_t addr, uint8_t val) {
    /* POKEY audio registers — update under audio lock so the callback
       thread does not observe half-written channel state.             */
    if (addr >= 0xD200 && addr <= 0xD208) {
        SDL_LockAudioDevice(audioDeviceID);
        int ch = (addr - 0xD200) >> 1;   /* 0-3 */
        if ((addr & 1) == 0) {             /* AUDFn */
            if (ch < 4) updateChannelFreq(ch, val);
        } else if (addr == 0xD208) {       /* AUDCTL */
            audctl = val;
            for (int i = 0; i < 4; i++) updateChannelFreq(i, audctlAutf(i));
        } else {                            /* AUDCn */
            if (ch < 4) updateChannelCtrl(ch, val);
        }
        SDL_UnlockAudioDevice(audioDeviceID);
        return;
    }
    /* ANTIC display-list pointer — cache it for the renderer. */
    if (addr == 0xD402) { displayListPtr = (displayListPtr & 0xFF00) | val; return; }
    if (addr == 0xD403) { displayListPtr = (displayListPtr & 0x00FF) | ((uint16_t)val << 8); return; }
    /* Player/Missile H-positions and sizes (write-only hardware regs). */
    if (addr >= 0xD000 && addr <= 0xD003) { hposP[addr - 0xD000] = val; return; }
    if (addr >= 0xD004 && addr <= 0xD007) { hposM[addr - 0xD004] = val; return; }
    if (addr >= 0xD008 && addr <= 0xD00B) { sizePM[addr - 0xD008] = val; return; }

    /* GTIA colour registers — keep colHW[] in sync for the renderer.
       Index mapping: COLPM0-3 = D012-D015 → [0-3], COLPF0-3 = D016-D019 → [4-7], COLBK = D01A → [8] */
    if (addr >= 0xD012 && addr <= 0xD01A) {
        colHW[addr - 0xD012] = val;
        return;
    }

    /* DMACTL — ANTIC DMA control.  Also update displayListPtr width bits. */
    if (addr == 0xD400) { dmactl = val; return; }

    /* PMBASE — player/missile bitmap base page ($D406; $D407 aliases it). */
    if (addr == 0xD406 || addr == 0xD407) { pmbase = val; return; }
    /* CHBASE — character set page ($D409 ANTIC, not $D209 POKEY STIMER). */
    if (addr == 0xD409) { chbase = val; return; }
    /* WSYNC — CPU stall until end of horizontal line; just ignore.       */
    if (addr == 0xD40A) { return; }
    /* Smooth scroll registers. */
    if (addr == 0xD404) { hscrol = val & 0x0F; return; }
    if (addr == 0xD405) { vscrol = val & 0x0F; return; }
    /* GRACTL — graphics control (player/missile enable, trigger latch).  */
    if (addr == 0xD01D) { gractl = val; return; }
}

void PlatformSDL::shadowWrite(uint16_t addr, uint8_t val) {
    /* Track VVBLKI changes and update the VBI dispatch pointer. */
    if (addr == 0x0222 || addr == 0x0223) {
        uint16_t vbi_addr = mem[0x0222] | (mem[0x0223] << 8);
        for (int i = 0; i < vbiTableCount; i++) {
            if (vbiTable[i].addr == vbi_addr) {
                interruptFn = vbiTable[i].fn;
                break;
            }
        }
    }
    /* Track SDLSTL/H for display-list pointer. */
    if (addr == 0x0230) displayListPtr = (displayListPtr & 0xFF00) | val;
    if (addr == 0x0231) displayListPtr = (displayListPtr & 0x00FF) | ((uint16_t)val << 8);
    /* CHBASE shadow ($02F4). */
    if (addr == 0x02F4) chbase = val;
    /* SDMCTL shadow ($022F = DMACTL shadow). */
    if (addr == 0x022F) dmactl = val;
}

/* ------------------------------------------------------------------ */
/* VBI dispatch table registration                                     */
/* ------------------------------------------------------------------ */

void PlatformSDL::registerVBI(uint16_t addr, void (*fn)(void)) {
    if (vbiTableCount < VBI_TABLE_SIZE) {
        vbiTable[vbiTableCount++] = { addr, fn };
    }
}

void PlatformSDL::indirectJmp(uint16_t addr) {
    if (addr == 0) return;  /* null / unset DLI chain — do nothing */
    for (int i = 0; i < vbiTableCount; i++) {
        if (vbiTable[i].addr == addr) {
            vbiTable[i].fn();
            return;
        }
    }
    /* Unknown address — silently ignore; Phase 4 will add missing entries. */
}

/* ------------------------------------------------------------------ */
/* Audio callback — sample synthesis only, no VBI                    */
/* ------------------------------------------------------------------ */

void PlatformSDL::audioCallback(void* userdata, uint8_t* stream, int bytes) {
    PlatformSDL* p = static_cast<PlatformSDL*>(userdata);
    int words     = bytes >> 1;
    int16_t* out  = reinterpret_cast<int16_t*>(stream);

    for (int i = 0; i < words; i++) {
        int32_t mix = 0;
        for (int ch = 0; ch < 4; ch++) {
            PokeyChannel& c = p->channels[ch];
            if (c.period > 0.0f) {
                c.phase += 1.0f;
                if (c.phase >= c.period) {
                    c.phase -= c.period;
                    c.sign ^= 1;
                }
                mix += c.sign ? c.amplitude : -c.amplitude;
            }
        }
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out[i] = (int16_t)mix;
    }
    /* VBI is fired on the main thread by tickVBI() — nothing to do here. */
}

/* Fire one VBI tick on the main game thread if enough wall-clock time has
   elapsed since the last tick.  Using SDL_GetTicks makes VBI timing fully
   independent of whether the audio device is open.                       */
void PlatformSDL::tickVBI() {
    uint32_t now = SDL_GetTicks();
    uint32_t msPerFrame = 1000u / (uint32_t)framesPerSecond_;   /* 20 ms */
    if (now - lastVBITicks < msPerFrame) return;

    /* Advance by exactly one frame step to avoid drift.
       Cap catch-up to 4 frames so a debugger pause doesn't flood.       */
    lastVBITicks += msPerFrame;
    if (now - lastVBITicks > msPerFrame * 4) lastVBITicks = now - msPerFrame;

    vcountReg = 0;

    /* Do NOT increment RTCLOK here — the game's own VBI handler does it at
       $5333 (INC $0014).  On real hardware the OS VVBLKD would also tick it,
       but our FUN_e462 is a stub, so the game handler is the only source.
       Adding a second increment here makes $0014 advance by +2/frame and
       skip odd $004C targets in FUN_3cb1, causing an infinite spin.       */
    if (interruptFn) (*interruptFn)();
    renderNeeded = true;
}

/* ------------------------------------------------------------------ */
/* POKEY channel helpers (called under audio lock)                    */
/* ------------------------------------------------------------------ */

void PlatformSDL::updateChannelFreq(int ch, uint8_t audf) {
    /* Cache AUDF in mem[] for audctlAutf() to read back. */
    mem[0xD200 + ch * 2] = audf;  /* not real HW mem, but convenient */

    /* AUDCTL clock source per channel (Atari Hardware Reference Manual):
       Bit 5 ($20): CH1 (ch0) → 1.79 MHz.  Bit 6 ($40): CH3 (ch2) → 1.79 MHz.
       Bit 3 ($08): CH1+CH2 (ch0+ch1) 16-bit counter.
       Bit 4 ($10): CH3+CH4 (ch2+ch3) 16-bit counter.
       Bit 0 ($01): base clock = 15 kHz (÷114) instead of 64 kHz (÷28).    */
    bool use179MHz = (ch == 0 && (audctl & 0x20)) ||
                     (ch == 2 && (audctl & 0x40));
    double baseDiv  = (audctl & 0x01) ? 114.0 : POKEY_DIV;

    /* 16-bit chaining: ch0+ch1 linked when AUDCTL bit 3 set.
       ch2+ch3 linked when AUDCTL bit 4 set.
       The "high" channel (ch1 or ch3) becomes the MSB of the divider and
       is silenced; the "low" channel plays the combined frequency.         */
    bool chain01 = (audctl & 0x08) != 0;  /* ch0 (low) + ch1 (high) */
    bool chain23 = (audctl & 0x10) != 0;  /* ch2 (low) + ch3 (high) */

    if ((ch == 1 && chain01) || (ch == 3 && chain23)) {
        /* This is the MSB channel — silent in 16-bit mode. */
        channels[ch].period = 0.0f;
        return;
    }

    double clockHz = use179MHz ? POKEY_CLOCK : POKEY_CLOCK / baseDiv;
    double divider;
    if (ch == 0 && chain01) {
        /* 16-bit: divider = AUDF0 + AUDF1*256 + 1 */
        divider = (double)audf + (double)audctlAutf(1) * 256.0 + 1.0;
    } else if (ch == 2 && chain23) {
        /* 16-bit: divider = AUDF2 + AUDF3*256 + 1 */
        divider = (double)audf + (double)audctlAutf(3) * 256.0 + 1.0;
    } else {
        divider = (double)audf + 1.0;
    }

    double freqHz = clockHz / divider;
    if (freqHz <= 0.0 || freqHz > audioSpec.freq / 2.0) {
        channels[ch].period = 0.0f;
        return;
    }
    /* Half-period in samples: when phase reaches period, toggle sign. */
    channels[ch].period = (float)(audioSpec.freq / (2.0 * freqHz));
}

void PlatformSDL::updateChannelCtrl(int ch, uint8_t audc) {
    mem[0xD201 + ch * 2] = audc;
    int volume = audc & 0x0F;
    bool mute  = (audc & 0x10) != 0;  /* volume-only mode = DC (no audio) */
    /* Amplitude: scale volume 0–15 to a quarter of int16 max per channel. */
    channels[ch].amplitude = mute ? 0 : (int16_t)(volume * 512);
    if (volume == 0) channels[ch].period = 0.0f;
}

uint8_t PlatformSDL::audctlAutf(int ch) {
    return mem[0xD200 + ch * 2];
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

uint8_t PlatformSDL::readJoystick() {
    /* PORTA: each nibble = one joystick; active LOW.
       Upper nibble = stick 1, lower = stick 0.
       Bits: 0=up 1=down 2=left 3=right (0=pressed).            */
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    uint8_t port = 0xFF;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) port &= ~0x01;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) port &= ~0x02;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) port &= ~0x04;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) port &= ~0x08;
    return port;
}

uint8_t PlatformSDL::readTrigger(int stick) {
    /* TRIG0/1: active LOW — 0 = fire pressed, 1 = released.
       Stick 0 = space/Z; stick 1 = unused for now.          */
    if (stick != 0) return 0x01;
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    return (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_Z]) ? 0x00 : 0x01;
}

uint8_t PlatformSDL::readConsol() {
    /* CONSOL: bits 2=OPTION 1=SELECT 0=START (0=pressed); 3=speaker.
       Reading $D01F: START=bit0, SELECT=bit1, OPTION=bit2 (active low). */
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    uint8_t c = 0x0F;  /* all buttons released + speaker bit */
    if (keys[SDL_SCANCODE_RETURN]) c &= ~0x01;  /* START  */
    if (keys[SDL_SCANCODE_F2])     c &= ~0x02;  /* SELECT */
    if (keys[SDL_SCANCODE_F3])     c &= ~0x04;  /* OPTION */
    return c;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/* Convert Atari colour byte (hue/luma) to SDL_Color via YIQ.
   Hue = bits 7:4 (0=grey), luma = bits 3:1 (0-7), bit 0 ignored. */
void PlatformSDL::buildPalette() {
    /* Approximate NTSC hue angles in degrees (hue 0 = achromatic). */
    static const double HUE_ANGLES[16] = {
         0,  27,  50,  77, 105, 138, 166, 193,
       215, 232, 248, 265, 283, 310, 333, 355
    };
    for (int c = 0; c < 256; c++) {
        int h     = (c >> 4) & 0xF;
        int lbits = (c >> 1) & 0x7;
        float Y   = 18.0f + lbits * 30.0f;    /* 18…228 */
        float r, g, b;
        if (h == 0) {
            r = g = b = Y;
        } else {
            double angle = HUE_ANGLES[h] * M_PI / 180.0;
            float  chroma = 60.0f;
            float  I = chroma * (float)cos(angle);
            float  Q = chroma * (float)sin(angle);
            r = Y + 0.956f * I + 0.621f * Q;
            g = Y - 0.272f * I - 0.647f * Q;
            b = Y - 1.106f * I + 1.703f * Q;
        }
        auto clamp8 = [](float v) -> uint8_t {
            return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
        };
        palette[c] = { clamp8(r), clamp8(g), clamp8(b), 255 };
    }
}

SDL_Color PlatformSDL::atariColor(uint8_t c) const {
    return palette[c & 0xFE];  /* bit 0 is unused in Atari colour bytes */
}

/* Render the Atari display list into bufferSurface.
   Supports ANTIC modes 0 (blank), 1 (JMP), E (4-colour bitmap), F (mono).
   Other modes render as background fill; refined in Phase 4.            */
void PlatformSDL::renderAtariDisplay() {
    /* If DMA is disabled (DMACTL bit 5 clear), ANTIC outputs a blank screen. */
    if (!(dmactl & 0x20)) {
        SDL_FillRect(bufferSurface, nullptr, 0);
        return;
    }

    uint16_t dlPtr = (displayListPtr != 0)
                   ? displayListPtr
                   : (uint16_t)(mem[0x0230] | (mem[0x0231] << 8));
    if (dlPtr == 0) return;

    /* Sync hardware colour registers from OS shadows at frame start.
       The VBI writes hardware regs FROM OS shadows (so the shadows reflect
       the frame-start state).  DLI handlers then update colHW[] mid-scan via
       hwWrite; the renderer reads colHW[] each row so DLI colour changes
       take effect on the correct scanline.                              */
    colHW[0] = mem[0x02C0]; /* COLPM0 shadow */
    colHW[1] = mem[0x02C1]; /* COLPM1 */
    colHW[2] = mem[0x02C2]; /* COLPM2 */
    colHW[3] = mem[0x02C3]; /* COLPM3 */
    colHW[4] = mem[0x02C4]; /* COLPF0 */
    colHW[5] = mem[0x02C5]; /* COLPF1 */
    colHW[6] = mem[0x02C6]; /* COLPF2 */
    colHW[7] = mem[0x02C7]; /* COLPF3 */
    colHW[8] = mem[0x02C8]; /* COLBK  */

    /* Initial background fill from COLBK (updated per-entry by DLI). */
    {
        SDL_Color bg = atariColor(colHW[8]);
        SDL_FillRect(bufferSurface, nullptr,
                     SDL_MapRGB(bufferSurface->format, bg.r, bg.g, bg.b));
    }

    /* DLI handler address from VDSLST ($0200/$0201). */
    uint16_t dliAddr = mem[0x0200] | ((uint16_t)mem[0x0201] << 8);

    int scanY   = 0;
    uint16_t dp = dlPtr;
    int  guard  = 0;
    /* ANTIC data address counter — persists across display-list entries.
       Only reset on an LMS instruction; otherwise auto-advances after each
       rendered row (same semantics as the real ANTIC data counter).       */
    uint16_t dataAddr = 0;

    while (scanY < ROF_NATIVE_H && guard++ < 1024) {
        uint8_t instr = mem[dp++];
        uint8_t mode  = instr & 0x0F;
        bool    lms   = (instr & 0x40) != 0;

        if (mode == 0) {
            /* Blank lines: count = bits 6:4 + 1. */
            int count = ((instr >> 4) & 0x7) + 1;
            scanY += count;
            continue;
        }
        if (mode == 1) {
            /* JMP: follow 16-bit address in next 2 bytes. */
            uint16_t dest = mem[dp] | ((uint16_t)mem[dp + 1] << 8);
            if (dest == dlPtr) break;  /* looping back to start = done */
            dp = dest;
            continue;
        }

        /* Data mode: LMS sets a new data address; without LMS the counter
           continues from where the previous entry left off.               */
        if (lms) {
            dataAddr = mem[dp] | ((uint16_t)mem[dp + 1] << 8);
            dp += 2;
        }

        /* DLI dispatch: bit 7 of the instruction fires the DLI handler BEFORE
           rendering this row so the new colours apply to the correct scanlines.
           On real hardware it fires at the END of the last scanline of the entry,
           but firing before the next entry's render is functionally equivalent. */
        if ((instr & 0x80) && dliAddr != 0)
            indirectJmp(dliAddr);

        /* Read current colours from colHW[] — updated by DLI above.     */
        SDL_Color bgClr  = atariColor(colHW[8]);
        SDL_Color pf0Clr = atariColor(colHW[4]);
        SDL_Color pf1Clr = atariColor(colHW[5]);
        SDL_Color pf2Clr = atariColor(colHW[6]);
        uint32_t  bgPx   = SDL_MapRGB(bufferSurface->format,
                                      bgClr.r, bgClr.g, bgClr.b);

        /* Scanlines per display-list entry and bytes per screen row. */
        int scans = 1, rowBytes = 40;
        switch (mode) {
        case 0x2: scans = 8;  rowBytes = 40; break;
        case 0x3: scans = 10; rowBytes = 40; break;
        case 0x4: scans = 8;  rowBytes = 40; break;
        case 0x5: scans = 16; rowBytes = 40; break;
        case 0x6: scans = 8;  rowBytes = 20; break;
        case 0x7: scans = 16; rowBytes = 20; break;
        case 0x8: scans = 8;  rowBytes = 8;  break;
        case 0x9: scans = 4;  rowBytes = 8;  break;
        case 0xA: scans = 4;  rowBytes = 16; break;
        case 0xB: scans = 2;  rowBytes = 16; break;
        case 0xC: scans = 1;  rowBytes = 16; break;
        case 0xD: scans = 2;  rowBytes = 40; break;
        case 0xE: scans = 1;  rowBytes = 40; break;
        case 0xF: scans = 1;  rowBytes = 40; break;
        }

        if (dataAddr == 0) { scanY += scans; continue; }

        if (mode == 0xF) {
            /* ANTIC mode F: 320 pixels, 1 bpp, COLBK / COLPF2.
               HSCROL shifts the bitmap right by hscrol colour clocks (pixels). */
            uint32_t c0 = bgPx;
            uint32_t c1 = SDL_MapRGB(bufferSurface->format,
                                      pf2Clr.r, pf2Clr.g, pf2Clr.b);
            /* If HSCROL is in effect (display list instruction has bit 4 set),
               use an extra byte at the start of the row data.                 */
            bool hscrolEn = (instr & 0x10) != 0;
            int  extraBytes = hscrolEn ? 2 : 0; /* read 1–2 extra bytes at left */
            int  shift      = hscrolEn ? hscrol : 0; /* pixel offset 0–15 */
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                /* Build a 48-byte shifted view: normal rowBytes + extraBytes  */
                int totalBytes = rowBytes + extraBytes;
                for (int b = 0; b < totalBytes; b++) {
                    uint8_t byte = mem[(dataAddr + b) & 0xFFFF];
                    for (int bit = 7; bit >= 0; bit--) {
                        int px = b * 8 + (7 - bit) - shift;
                        if (px >= 0 && px < ROF_NATIVE_W)
                            row[px] = (byte >> bit) & 1 ? c1 : c0;
                    }
                }
                dataAddr = (dataAddr + rowBytes) & 0xFFFF;
            }
        } else if (mode == 0xE || mode == 0xD) {
            /* ANTIC mode E/D: 160 pixels, 2 bpp, 4 colours. */
            SDL_Color mc[4] = { bgClr, pf0Clr, pf1Clr, pf2Clr };
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                for (int b = 0; b < rowBytes; b++) {
                    uint8_t byte = mem[(dataAddr + b) & 0xFFFF];
                    for (int pp = 0; pp < 4; pp++) {
                        int ci = (byte >> (6 - pp * 2)) & 3;
                        int px = (b * 4 + pp) * 2; /* 2× stretch to 320 */
                        if (px < ROF_NATIVE_W - 1) {
                            uint32_t col = SDL_MapRGB(bufferSurface->format,
                                mc[ci].r, mc[ci].g, mc[ci].b);
                            row[px] = row[px + 1] = col;
                        }
                    }
                }
                dataAddr = (dataAddr + rowBytes) & 0xFFFF;
            }
        } else if (mode == 0x2 || mode == 0x3 || mode == 0x4 || mode == 0x5) {
            /* ANTIC modes 2-5: 40-column character modes, 1-pixel-per-bit,
               320 display pixels wide (8 bits per char * 40 chars).
               Mode 2: 40 chars, 8 scans. Mode 3: 10 scans. Mode 4: 8 scans, 2 colors.
               Mode 5: 16 scans.  All use CHBASE charset.                  */
            uint16_t csBase = (uint16_t)chbase << 8;
            int charsPerRow = 40;
            /* Scan count within a character tile (not the DL entry scans). */
            int scansPerChar = (mode == 3) ? 10 : (mode == 5) ? 16 : 8;
            int charScan = 0; /* which row within the current char tile */
            uint16_t rowAddr = dataAddr;
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                /* For mode 3 we clamp bitmap row to 8 (last 2 scans repeat row 7). */
                int bitmapRow = (scansPerChar > 8) ?
                    (charScan < 8 ? charScan : 7) : charScan;
                for (int c = 0; c < charsPerRow; c++) {
                    uint8_t chByte = mem[(rowAddr + c) & 0xFFFF];
                    bool inv = (chByte & 0x80) != 0;
                    uint8_t ci  = chByte & 0x7F;
                    uint8_t bits = mem[(csBase + ci * 8 + bitmapRow) & 0xFFFF];
                    /* Select fg color: modes 2/3 use COLPF2; modes 4/5 use
                       bits[7:6] of char byte to select COLPF0-COLPF3.     */
                    SDL_Color fgClr = pf2Clr;
                    if (mode == 4 || mode == 5) {
                        static const SDL_Color* pfTab[4] = {
                            &pf0Clr, &pf1Clr, &pf2Clr, nullptr };
                        int sel = (chByte >> 6) & 3;
                        fgClr = pfTab[sel < 3 ? sel : 0][0];
                    }
                    uint32_t fgPx = SDL_MapRGB(bufferSurface->format,
                                               fgClr.r, fgClr.g, fgClr.b);
                    for (int bit = 7; bit >= 0; bit--) {
                        bool set = ((bits >> bit) & 1) != 0;
                        if (inv) set = !set;
                        int px = c * 8 + (7 - bit);
                        if (px < ROF_NATIVE_W)
                            row[px] = set ? fgPx : bgPx;
                    }
                }
                if (++charScan >= scansPerChar) {
                    charScan = 0;
                    rowAddr = (rowAddr + charsPerRow) & 0xFFFF;
                }
            }
            dataAddr = (rowAddr + charsPerRow) & 0xFFFF;
        } else if (mode == 0x6 || mode == 0x7) {
            /* ANTIC modes 6/7: 20-column colour-character modes.
               Mode 6: 8 scans/char, mode 7: 16 scans/char.
               Each byte: bits[5:0]=char index (0-63), bits[7:6]=color register.
               Each bit displayed 2× wide → 20*16=320 display pixels.     */
            uint16_t csBase = (uint16_t)chbase << 8;
            int charsPerRow = 20;
            int scansPerChar = (mode == 7) ? 16 : 8;
            int charScan = 0;
            uint16_t rowAddr = dataAddr;
            SDL_Color colPFn[4] = { pf0Clr, pf1Clr, pf2Clr, pf2Clr };
            colPFn[3] = atariColor(mem[0x02C7]); /* COLPF3 */
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                int bitmapRow = charScan < 8 ? charScan : 7;
                for (int c = 0; c < charsPerRow; c++) {
                    uint8_t chByte = mem[(rowAddr + c) & 0xFFFF];
                    int colorSel = (chByte >> 6) & 3;
                    uint8_t ci   = chByte & 0x3F;
                    uint8_t bits = mem[(csBase + ci * 8 + bitmapRow) & 0xFFFF];
                    SDL_Color fgClr = colPFn[colorSel];
                    uint32_t fgPx = SDL_MapRGB(bufferSurface->format,
                                               fgClr.r, fgClr.g, fgClr.b);
                    for (int bit = 7; bit >= 0; bit--) {
                        bool set = ((bits >> bit) & 1) != 0;
                        int px = (c * 8 + (7 - bit)) * 2; /* 2× stretch */
                        if (px < ROF_NATIVE_W - 1)
                            row[px] = row[px + 1] = set ? fgPx : bgPx;
                    }
                }
                if (++charScan >= scansPerChar) {
                    charScan = 0;
                    rowAddr = (rowAddr + charsPerRow) & 0xFFFF;
                }
            }
            dataAddr = (rowAddr + charsPerRow) & 0xFFFF;
        } else {
            /* Modes 8–C not yet implemented; fill with background. */
            scanY += scans;
        }
    }

    /* Player/Missile graphics overlay — use current COLBK for transparency. */
    {
        SDL_Color bg = atariColor(colHW[8]);
        renderPMGraphics(SDL_MapRGB(bufferSurface->format, bg.r, bg.g, bg.b));
    }
}

/* Render ANTIC Player/Missile graphics over the current bufferSurface.
   Players are 8-pixel-wide single-line bitmaps.  Phase 4 refines size/
   double-line resolution handling; this is the Phase 3 baseline.      */
void PlatformSDL::renderPMGraphics(uint32_t /*bgPx*/) {
    if (pmbase == 0) return;
    if (!(gractl & 0x02)) return;  /* GRACTL bit 1: player/missile enable */

    /* Single-line resolution (GRACTL bit 0 = 0): bitmap base = pmbase*256+512.
       Double-line (GRACTL bit 0 = 1): bitmap base = pmbase*256+256, stride=128. */
    bool doubleLine  = (gractl & 0x01) != 0;
    int  playerStride = doubleLine ? 128 : 256;
    int  pmBaseAddr   = (int)pmbase << 8;

    for (int p = 0; p < 4; p++) {
        /* Use tracked H-position + colHW for DLI-updated colours.         */
        uint8_t hpos  = hposP[p];
        SDL_Color col = atariColor(colHW[p]);  /* COLPM0-3 from colHW[0-3] */
        uint32_t pCol = SDL_MapRGB(bufferSurface->format,
                                   col.r, col.g, col.b);

        int bitmapBase = pmBaseAddr + 512 + p * playerStride;
        int maxScan    = SDL_min(doubleLine ? 128 : 256, ROF_NATIVE_H);
        for (int y = 0; y < maxScan; y++) {
            uint8_t bits = mem[(bitmapBase + y) & 0xFFFF];
            if (!bits) continue;
            /* For double-line mode, each bitmap row covers 2 display rows. */
            int dy = doubleLine ? y * 2 : y;
            for (int rep = 0; rep <= (doubleLine ? 1 : 0); rep++) {
                int scanRow = dy + rep;
                if (scanRow >= ROF_NATIVE_H) break;
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanRow * bufferSurface->pitch);
                /* hpos: Atari colour-clock → display pixel.
                   hpos 0 = leftmost colour clock; each colour clock = 1 pixel
                   in mode F (320-wide).  Subtract 48 for left-border offset. */
                int x0 = (int)hpos - 48;
                for (int bit = 7; bit >= 0; bit--) {
                    if ((bits >> bit) & 1) {
                        int px = x0 + (7 - bit);
                        if (px >= 0 && px < ROF_NATIVE_W)
                            row[px] = pCol;
                    }
                }
            }
        }
    }
}

/* Pump the SDL event queue without ticking the VBI.
   Rate-limited to 100 Hz so the joystick run-loop (reinstated via
   SDL_INIT_JOYSTICK) cannot block the main thread when called from
   tight spin-wait loops.                                             */
void PlatformSDL::pollEvents() {
    uint32_t now = SDL_GetTicks();
    static uint32_t lastPoll = 0;
    if (now - lastPoll < 10u) return;   /* max ~100 Hz */
    lastPoll = now;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) quit = true;
        if (ev.type == SDL_KEYDOWN &&
            ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            quit = true;
    }
}

/* Fire the VBI and blit the display if a new frame is pending.
   Call from spin-waits that own a full VBI tick (FUN_3cb1, L_656e).
   Fires tickVBI() first so both VBI and render happen in one call.
   SDL_UpdateWindowSurface is only called once per VBI (50 Hz),
   so Metal's nextDrawable never accumulates pressure.               */
void PlatformSDL::renderFrame() {
    tickVBI();
    if (!renderNeeded) return;
    renderNeeded = false;

    pollEvents();

    renderAtariDisplay();

    SDL_Rect src  = { 0, 0, ROF_NATIVE_W, ROF_NATIVE_H };
    SDL_Rect dst  = { 0, 0, ROF_WINDOW_W, ROF_WINDOW_H };
    SDL_BlitScaled(bufferSurface, &src, windowSurface, &dst);
    SDL_UpdateWindowSurface(window);
}
