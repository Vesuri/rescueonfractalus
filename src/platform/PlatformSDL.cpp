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
    displayListPtr(0), framesPerSecond_(50), vcountReg(0),
    samplesSinceInterrupt(0), interruptIntervalInSamples(0)
{
    memset(channels, 0, sizeof(channels));
    memset(vbiTable, 0, sizeof(vbiTable));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        quit = true;
        return;
    }

    /* Build Atari NTSC palette (hue/luma → RGB via YIQ). */
    buildPalette();

    /* Audio: mono 16-bit, 44100 Hz, 512-sample callback — mirrors
       PETSCIIRobots-SDL's setup exactly. */
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
    /* Pick up initial display-list pointer from OS shadow. */
    displayListPtr = mem[0x0230] | (mem[0x0231] << 8);
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
    if (addr == 0xD010) return 0x00;               /* GRAFP3 collision */
    if (addr == 0xD20F) return 0x00;               /* SKSTAT */
    return 0x00;
}

void PlatformSDL::hwWrite(uint16_t addr, uint8_t val) {
    /* POKEY audio registers — update under audio lock so the callback
       does not observe half-written channel state. */
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
/* Audio callback (fires VBI — same design as PETSCIIRobots-SDL)      */
/* ------------------------------------------------------------------ */

void PlatformSDL::audioCallback(void* userdata, uint8_t* stream, int bytes) {
    PlatformSDL* p = static_cast<PlatformSDL*>(userdata);
    int words  = bytes >> 1;
    int16_t* out = reinterpret_cast<int16_t*>(stream);

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
        /* Clamp to int16 range. */
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out[i] = (int16_t)mix;
    }

    p->samplesSinceInterrupt += words;
    while (p->samplesSinceInterrupt >= p->interruptIntervalInSamples) {
        /* Reset VCOUNT at the start of each frame so scanline-wait loops
           see a naturally cycling value across frames.                  */
        p->vcountReg = 0;

        /* Increment the OS jiffy clock (RTCLOK $0012-$0014).
           On real hardware this is done by the OS deferred VBI (VVBLKD).
           The game overrides only VVBLKI; VVBLKD would still run in the
           OS and tick the clock.  We simulate it here so that timing
           loops like "wait for RTCLOK to reach N" terminate.
           $0014 = fast byte (every frame), $0013/$0012 = overflow bytes. */
        if (++mem[0x0014] == 0)
            if (++mem[0x0013] == 0)
                ++mem[0x0012];

        if (p->interruptFn) (*p->interruptFn)();
        p->samplesSinceInterrupt -= p->interruptIntervalInSamples;
    }
}

/* ------------------------------------------------------------------ */
/* POKEY channel helpers (called under audio lock)                    */
/* ------------------------------------------------------------------ */

void PlatformSDL::updateChannelFreq(int ch, uint8_t audf) {
    /* Cache AUDF in mem[] for audctlAutf() to read back. */
    mem[0xD200 + ch * 2] = audf;  /* not real HW mem, but convenient */
    double clockHz = (audctl & (ch >= 2 ? 0x40 : 0x10))
                      ? POKEY_CLOCK : POKEY_CLOCK / POKEY_DIV;
    double freqHz  = clockHz / (audf + 1);
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
    uint16_t dlPtr = (displayListPtr != 0)
                   ? displayListPtr
                   : (uint16_t)(mem[0x0230] | (mem[0x0231] << 8));
    if (dlPtr == 0) return;

    /* Color registers from OS shadows (updated by VBI handler). */
    SDL_Color bgClr  = atariColor(mem[0x02C8]);  /* COLBK  / COLOR4 */
    SDL_Color pf0Clr = atariColor(mem[0x02C4]);  /* COLPF0 / COLOR0 */
    SDL_Color pf1Clr = atariColor(mem[0x02C5]);  /* COLPF1 / COLOR1 */
    SDL_Color pf2Clr = atariColor(mem[0x02C6]);  /* COLPF2 / COLOR2 */
    /* pf3Clr unused in current renderer; available for Phase 4 */

    uint32_t bgPx  = SDL_MapRGB(bufferSurface->format, bgClr.r,  bgClr.g,  bgClr.b);
    SDL_FillRect(bufferSurface, nullptr, bgPx);

    int scanY   = 0;
    uint16_t dp = dlPtr;
    int  guard  = 0;

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

        /* Data mode: optional LMS address prefix. */
        uint16_t dataAddr = 0;
        if (lms) {
            dataAddr = mem[dp] | ((uint16_t)mem[dp + 1] << 8);
            dp += 2;
        }

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
            /* ANTIC mode F: 320 pixels, 1 bpp, COLBK / COLPF2. */
            uint32_t c0 = bgPx;
            uint32_t c1 = SDL_MapRGB(bufferSurface->format,
                                      pf2Clr.r, pf2Clr.g, pf2Clr.b);
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                for (int b = 0; b < rowBytes; b++) {
                    uint8_t byte = mem[(dataAddr + b) & 0xFFFF];
                    for (int bit = 7; bit >= 0; bit--) {
                        int px = b * 8 + (7 - bit);
                        if (px < ROF_NATIVE_W)
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
        } else {
            /* All other modes: background fill for now (Phase 4 refines). */
            scanY += scans;
        }
    }

    /* Player/Missile graphics overlay. */
    renderPMGraphics(bgPx);
}

/* Render ANTIC Player/Missile graphics over the current bufferSurface.
   Players are 8-pixel-wide single-line bitmaps.  Phase 4 refines size/
   double-line resolution handling; this is the Phase 3 baseline.      */
void PlatformSDL::renderPMGraphics(uint32_t /*bgPx*/) {
    uint8_t pmbase = mem[0xD407];     /* PMBASE register value */
    if (pmbase == 0) return;

    /* Single-line resolution: player N bitmap at pmbase*256 + 512 + N*256.
       Double-line: pmbase*256 + 256 + N*128.  Assume single for now.  */
    bool doubleLine = false;  /* TODO: read GRACTL bit for resolution */
    int playerStride = doubleLine ? 128 : 256;
    int pmBaseAddr   = pmbase << 8;

    for (int p = 0; p < 4; p++) {
        uint8_t hpos  = mem[0xD000 + p];  /* HPOSP0-3 */
        SDL_Color col = atariColor(mem[0x02C0 + p]); /* PCOLR0-3 shadow */
        uint32_t pCol = SDL_MapRGB(bufferSurface->format,
                                   col.r, col.g, col.b);

        int bitmapBase = pmBaseAddr + 512 + p * playerStride;
        /* PM bitmap height matches display: scan through up to 256 rows. */
        int maxScan = SDL_min(256, ROF_NATIVE_H);
        for (int y = 0; y < maxScan; y++) {
            uint8_t bits = mem[(bitmapBase + y) & 0xFFFF];
            if (!bits) continue;
            uint32_t* row = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(bufferSurface->pixels) +
                y * bufferSurface->pitch);
            /* Horizontal position: hpos maps to pixel 0..319 approximately.
               Atari PM hpos 0 = left edge of colour clock 0;
               subtract 128 to centre the player in the visible area.   */
            int x0 = hpos - 48;  /* rough centering for single-width */
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

/* Blit buffer → window with 2× scale, process SDL quit event. */
void PlatformSDL::renderFrame() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) quit = true;
        if (ev.type == SDL_KEYDOWN &&
            ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            quit = true;
    }

    renderAtariDisplay();

    SDL_Rect src  = { 0, 0, ROF_NATIVE_W, ROF_NATIVE_H };
    SDL_Rect dst  = { 0, 0, ROF_WINDOW_W, ROF_WINDOW_H };
    SDL_BlitScaled(bufferSurface, &src, windowSurface, &dst);
    SDL_UpdateWindowSurface(window);
}
