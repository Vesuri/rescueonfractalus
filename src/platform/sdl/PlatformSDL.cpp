#include "PlatformSDL.h"
#include "../../cpu/cpu.h"
#include "../../xex_load.h"   /* shared XEX-format walk (xex_parse / xex_overlay_osrom) */
#include "../../rof_boot.h"   /* staged INITAD boot chain (Logo / Station) */
#include <cstdio>
#include <cstdlib>      /* getenv, atoi */
#include <csignal>      /* signal, SIGINT, SIGTERM (Ctrl-C handling in run()) */
#include <cstring>
#include <strings.h>   /* strcasecmp */
#include <cmath>
#include <png.h>

extern volatile uint8_t mem[65536];

/* POKEY base clock (NTSC, ~1.79 MHz) used for audio frequency calculation. */
static const double POKEY_CLOCK = 1789772.5;
static const double POKEY_DIV   = 28.0;   /* ÷28 = ~63.9 kHz channel clock */

/* ------------------------------------------------------------------ */
/* Launch stage selection (ROF_START)                                  */
/* ------------------------------------------------------------------ */

/* Stages the boot chain can be entered at, IN THE ORDER the game flows through
   them — so the ordering comparisons below (`> ROF_STAGE_STANDBY` = "auto-press
   START") stay meaningful as stages are added.

   The first two are the boot INITAD scenes rof.xex's own loader runs BEFORE
   game_entry() ($5000 = the Lucasfilm logo, $1A97 = the space station).  They are
   reachable now that the loader walk is staged (rof_boot.c) instead of loading the
   whole image at once; before that this port jumped straight to game_entry() and
   they had no code to jump to.  standby STAYS THE DEFAULT, so the boot scenes are
   opt-in and no existing SDL workflow changes.                                    */
enum RofStage {
    ROF_STAGE_LOGO = 0,      /* Lucasfilm logo -> station -> standby              */
    ROF_STAGE_STATION,       /* space-station cinematic -> standby                */
    ROF_STAGE_STANDBY,       /* cockpit "STAND BY" + title (default)              */
    ROF_STAGE_LAUNCH,        /* launch/descent sequence (after START)             */
    ROF_STAGE_FLIGHT       /* terrain flight — fast-forward past the launch       */
};

/* Parse ROF_START once and cache it. Canonical values (case-insensitive):
   logo | station | standby | launch | flight. Legacy aliases kept so nothing
   breaks: attract->standby, tunnel->launch, gameplay/game->flight. The legacy
   ROF_AUTOSTART=1 toggle still works as an alias for flight.
   - logo    : the whole faithful boot chain — Lucasfilm logo, then the station
               cinematic, then Standby (~9 s before Standby appears).
   - station : skip the logo; play the station cinematic, then Standby.
   - standby (default) : skip both boot scenes; no input injected; stays on the
                         Standby screen (cockpit "STAND BY" + title).
   - launch  : auto-presses START and runs at normal speed, so the ~30s
               Launch sequence (Doors -> Tunnel -> Planet) plays out visibly.
   - flight  : auto-presses START and FAST-FORWARDS (unthrottled) through the
               Launch sequence, dropping straight into terrain Flight (~1s).
   (docs/logo-station-plan.md covers the two boot scenes; CLAUDE.md has the
    7-scene vocab: Logo, Station, Standby, Doors, Tunnel, Planet, Flight.)         */
static RofStage rofStartStage() {
    static int cached = -1;
    if (cached < 0) {
        cached = ROF_STAGE_STANDBY;
        const char* s = getenv("ROF_START");
        if (s && *s) {
            if      (!strcasecmp(s, "logo"))     cached = ROF_STAGE_LOGO;
            else if (!strcasecmp(s, "station"))  cached = ROF_STAGE_STATION;
            else if (!strcasecmp(s, "standby") ||
                     !strcasecmp(s, "attract"))  cached = ROF_STAGE_STANDBY;
            else if (!strcasecmp(s, "launch")  ||
                     !strcasecmp(s, "tunnel"))   cached = ROF_STAGE_LAUNCH;
            else if (!strcasecmp(s, "flight")   ||
                     !strcasecmp(s, "gameplay") ||
                     !strcasecmp(s, "game"))      cached = ROF_STAGE_FLIGHT;
            else fprintf(stderr, "[rof] ROF_START='%s' unrecognised; using standby "
                         "(valid: logo, station, standby, launch, flight)\n", s);
        } else if (getenv("ROF_AUTOSTART")) {
            cached = ROF_STAGE_FLIGHT;   /* legacy alias */
        }
        const char* names[] = { "logo", "station", "standby", "launch", "flight" };
        fprintf(stderr, "[rof] launch stage: %s\n", names[cached]);
    }
    return (RofStage)cached;
}

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
    hposP{}, hposM{}, sizePM{}, sizeM(0),
    gprior(0),
    framesPerSecond_(50), vcountReg(0), colHW{},
    samplesSinceInterrupt(0), interruptIntervalInSamples(0),
    lastVBITicks(0), renderNeeded(false), screenshotIndex(0),
    reachedFlight_(false), lastFFRender_(0), inInterrupt_(false)
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
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

extern "C" {
    void rof_register_vbi_handlers(void);
    void game_entry(void);
}

/* SDL may swallow SIGINT on macOS; reinstall a plain exit handler so Ctrl-C
   always works, even when the game is stuck in a spin-wait loop before the SDL
   event loop runs.  (SDL_Init in our ctor installs its own SIGINT/SIGTERM
   handlers, so we re-arm ours here, after SDL is up.) */
static void handleSigInt(int) { exit(0); }

void PlatformSDL::run() {
    signal(SIGINT,  handleSigInt);
    signal(SIGTERM, handleSigInt);

    /* Populate the VBI address -> C-function dispatch table so the audio callback
       can fire the right handler when the game installs it. */
    rof_register_vbi_handlers();

    /* ROF_START=logo|station: replay the genuine staged INITAD boot chain (rof_boot.c),
       which reloads the XEX one stage at a time so the Logo and Station see the memory
       they were written for, and plays them.  Every other stage (standby, the default,
       and launch/flight) keeps the one-shot full image loadImage() already placed and
       drops straight into game_entry, byte-for-byte as before. */
    if (rofStartStage() <= ROF_STAGE_STATION)
        rof_boot_chain(rofStartStage() == ROF_STAGE_LOGO ? ROF_BOOT_LOGO : ROF_BOOT_STATION);

    /* Run the game — loops forever (or until ESC / window close). */
    game_entry();
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

/* SDL mem[] writer for the shared XEX parser: a plain byte copy (the host is fast
   and the Amiga's endian-specific 32-bit block store would byte-swap here). */
static void sdl_mem_write(uint16_t s, const uint8_t* src, uint32_t cnt) {
    for (uint32_t k = 0; k < cnt; k++) mem[(uint16_t)(s + k)] = src[k];
}

/* The loaded image + OS ROM, kept for the STAGED boot walk (rof_boot.c) — which redoes
   the load one INITAD stage at a time so the Logo and Station see the memory they were
   written for (see xex_load.h xex_parse_stage).  File-static rather than local to
   loadImage() for exactly that reason. */
static uint8_t  s_xex[65536];
static uint32_t s_xexLen = 0;
static uint8_t  s_rom[0x3800];
static uint32_t s_romLen = 0;
/* true = s_rom holds the lean 1 KB charset (place at $E000, what the Amiga ships);
   false = a full 14 KB OS ROM image (place via the wider two-range overlay). */
static bool     s_romIsCharset = false;

/* rof_overlay_rom(): place whichever ROM asset was loaded.  One helper so the two call
   sites (power-on reset + the one-shot full load) cannot drift apart. */
static void rof_overlay_rom(void) {
    if (!s_romLen) return;
    if (s_romIsCharset) xex_overlay_charset(s_rom, s_romLen, sdl_mem_write);
    else                xex_overlay_osrom(s_rom, s_romLen, sdl_mem_write);
}

/* rof_load_stage_reset(): power-on RAM — zero mem[], then overlay the charset/OS ROM.
   Mirrors the Amiga XexImage.cpp pair of the same names; rof_boot.c calls both. */
extern "C" void rof_load_stage_reset(void) {
    memset((uint8_t*)mem, 0, 65536);
    rof_overlay_rom();
}

/* rof_load_stage(): place the next stage's segments (up to and including the one that
   sets INITAD) and return the offset to resume from; == len when the file is placed. */
extern "C" uint32_t rof_load_stage(uint32_t from) {
    return xex_parse_stage(s_xex, s_xexLen, from, sdl_mem_write);
}

int PlatformSDL::loadImage(const char* path) {
    /* Pristine boot: load the genuine Atari segmented load file (.xex) the SAME way
       the Amiga does (XexImage.cpp load_xex_image) — zero RAM, place each XEX segment
       at its load address, then overlay the Atari OS ROM — so the SDL build runs the
       identical pristine game_entry code path as the Amiga.  (The legacy flat 64 KB
       RAM-snapshot boot was retired; SDL now boots only .xex, like the Amiga.) */
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    s_xexLen = (uint32_t)fread(s_xex, 1, sizeof(s_xex), f);
    fclose(f);

    /* Read the Atari charset — the same 1 KB asset the Amiga embeds, placed at $E000 by
       the shared xex_overlay_charset.  It is the only ROM data the port reads (see
       xex_load.h).  A full 14 KB atari_osrom.bin is still accepted if one is lying around,
       via the wider xex_overlay_osrom ([0..$1000)->$C000, [$1000..$3800)->$D800, skipping
       the $D000-$D7FF hardware range so hwRead/hwWrite stay authoritative): it places the
       ranges the charset-only asset cannot, which keeps it usable as a divergence detector
       if a build is ever suspected of reading ROM outside the charset.  Charset first, so
       the lean asset is what a normal dev tree exercises — matching the Amiga. */
    const char* csPaths[]  = { "amiga/assets/atari_charset.bin", "assets/atari_charset.bin" };
    const char* romPaths[] = { "amiga/assets/atari_osrom.bin",   "assets/atari_osrom.bin"   };
    FILE* r = 0;
    for (size_t p = 0; p < sizeof(csPaths)/sizeof(csPaths[0]) && !r; p++)
        r = fopen(csPaths[p], "rb");
    s_romIsCharset = (r != 0);
    for (size_t p = 0; p < sizeof(romPaths)/sizeof(romPaths[0]) && !r; p++)
        r = fopen(romPaths[p], "rb");
    if (r) {
        s_romLen = (uint32_t)fread(s_rom, 1, sizeof(s_rom), r);
        fclose(r);
    } else {
        s_romLen = 0;
        fprintf(stderr, "[rof] WARNING: neither atari_charset.bin nor atari_osrom.bin found; "
                        "$E000 charset (LEVEL text) will be blank\n");
    }

    /* Zero RAM + OS ROM, then place every segment (the one-shot full load).  ROF_START=
       logo|station redoes this in stages from run(); every other stage runs on this image,
       byte-for-byte as before. */
    rof_load_stage_reset();
    xex_parse(s_xex, s_xexLen, sdl_mem_write);
    rof_overlay_rom();
    printf("[rof] loaded pristine XEX %s (%u bytes) + %s (%u bytes)\n",
           path, (unsigned)s_xexLen,
           s_romIsCharset ? "charset @ $E000" : "full OS ROM", (unsigned)s_romLen);

    /* Sync cached registers from OS shadow values in the loaded image (0 on a
       pristine XEX — game_entry sets them as it runs, re-read per frame). */
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
    if (addr == 0xD00C) { sizeM = val; return; }   /* SIZEM */

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
    /* PRIOR (GTIA mode / PM priority) — track for renderer.              */
    if (addr == 0xD01B) { gprior = val; return; }
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
    /* GPRIOR shadow ($026F). */
    if (addr == 0x026F) gprior = val;
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
    /* Re-entrancy guard: the VBI handler's own spin-waits call
       platform_tick_vbi() again.  The 20ms gate below used to absorb those,
       but fast-forward drops the gate, so guard explicitly or the handler
       recurses into itself and overflows the stack.                         */
    if (inInterrupt_) return;

    uint32_t now = SDL_GetTicks();
    uint32_t msPerFrame = 1000u / (uint32_t)framesPerSecond_;   /* 20 ms */

    /* ROF_START=gameplay fast-forward: until terrain flight begins
       (reachedFlight_), shrink the per-frame interval to 1ms (~1000 Hz) so the
       ~30s launch/landing sequence is compressed to ~1.5s.  This keeps the
       one-VBI-per-frame cadence intact (just runs the clock ~20x faster) —
       firing a VBI on *every* tick_vbi call instead would desync the VBI from
       main-thread progress and corrupt game state.                          */
    bool fastForward = (rofStartStage() == ROF_STAGE_FLIGHT) && !reachedFlight_;
    if (fastForward) msPerFrame = 1;

    if (now - lastVBITicks < msPerFrame) return;
    /* Advance by exactly one frame step to avoid drift.
       Cap catch-up to 4 frames so a debugger pause doesn't flood.       */
    lastVBITicks += msPerFrame;
    if (now - lastVBITicks > msPerFrame * 4) lastVBITicks = now - msPerFrame;

    vcountReg = 0;
    static int vbiCount = 0;
    ++vbiCount;

    /* Latch "terrain flight has begun" for the ROF_START=gameplay fast-forward.
       At mission start the game switches to the flight display list ($316B,
       the "MANUAL" cockpit) and sets the flight sub-state mem[$72]≠0; both are
       0 / a standby DL ($3000/$3120) throughout boot, title and descent.      */
    if (!reachedFlight_ && (displayListPtr == 0x316B || mem[0x72] != 0))
        reachedFlight_ = true;
    /* Screenshot timing. Default: one shot at attract frame 50. Override with
       ROF_SHOT_FIRST=N (first frame) and ROF_SHOT_EVERY=M (then every M frames)
       to capture a series — e.g. ROF_AUTOSTART=1 ROF_SHOT_FIRST=120 ROF_SHOT_EVERY=20
       grabs the launch/tunnel sequence so we can pick the frame to compare.    */
    {
        static int first = -1, every = 0;
        if (first < 0) {
            const char* f = getenv("ROF_SHOT_FIRST");
            const char* e = getenv("ROF_SHOT_EVERY");
            first = f ? atoi(f) : 50;
            every = e ? atoi(e) : 0;
        }
        if (vbiCount == first || (every > 0 && vbiCount > first && (vbiCount - first) % every == 0))
            saveScreenshot();
    }

    /* Seed colHW[] from OS shadows BEFORE calling the VBI handler.
       The handler reads the shadow registers, applies transforms (e.g.
       attract-mode EOR/AND in vbi_handler_1), and writes the result to
       the hardware registers via bus_write → hwWrite → colHW[].
       Doing this AFTER the handler (in renderAtariDisplay) would clobber
       exactly those transforms and cause the 50 Hz blue/black flash.

       ...EXCEPT during the two boot scenes.  This re-seed stands in for the Atari OS
       vblank's stage-2 shadow→hardware copy — and the Logo ($51EF) and Station ($1B30)
       handlers REPLACE the OS vblank wholesale: VVBLKI is called before any OS stage-1/2
       processing, and both return with an RTI-equivalent rather than chaining onward
       (which is exactly why each increments RTCLOK itself).  So no shadow copy happens on
       real hardware there, and both scenes program GTIA directly.  Re-seeding would undo
       every PRIOR/COLPF/COLPM they write: with GPRIOR forced back to 0 the station's
       PRIOR=$71 GTIA-mode-9 field was reinterpreted as mono hi-res ANTIC F against an
       all-zero palette — a completely BLACK screen.                                   */
    if (!g_bootScene) {
        colHW[0] = mem[0x02C0];  /* COLPM0 */
        colHW[1] = mem[0x02C1];  /* COLPM1 */
        colHW[2] = mem[0x02C2];  /* COLPM2 */
        colHW[3] = mem[0x02C3];  /* COLPM3 */
        colHW[4] = mem[0x02C4];  /* COLPF0 */
        colHW[5] = mem[0x02C5];  /* COLPF1 */
        colHW[6] = mem[0x02C6];  /* COLPF2 */
        colHW[7] = mem[0x02C7];  /* COLPF3 */
        colHW[8] = mem[0x02C8];  /* COLBK  */
        gprior   = mem[0x026F];  /* GPRIOR shadow → seed PRIOR for this frame */
    }

    /* Do NOT increment RTCLOK here — the game's own VBI handler does it.
       Save/restore CPU registers across the VBI call: on real hardware the
       6502 VBI is an interrupt that pushes/pops the processor state (RTI).
       Without this, spin-waits that load a register THEN call platform_tick_vbi
       find their register clobbered by the handler on return.               */
    if (interruptFn) {
        Cpu6502 saved = cpu;
        inInterrupt_ = true;
        (*interruptFn)();
        inInterrupt_ = false;
        cpu = saved;
    }
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

    // POKEY's output flip-flop halves the counted rate: f = clock / (2 * divider).
    // Without this ÷2 every voice is an octave too high (see PlatformAmiga pokey_period).
    double freqHz = clockHz / (2.0 * divider);
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
    /* ROF_START=tunnel|gameplay (or legacy ROF_AUTOSTART=1) auto-presses START
       (=Enter) over a brief wall-clock window ~0.8-1.2s after launch, so the
       game leaves the attract screen with no keypress and begins the mission:
       it flies the tunnel and then transitions into gameplay on its own. The
       window releases START before gameplay so it isn't held down. The upper
       bound is configurable via ROF_AUTOSTART_HI=<ms>. ROF_START=attract (the
       default) injects nothing and the game stays on the attract screen.
       ORDER-BASED test: only the stages AFTER standby want START injected — the
       two boot stages (logo/station) must not, or the injected START would exit
       the station cinematic the moment it appeared.                            */
    if (rofStartStage() > ROF_STAGE_STANDBY) {
        uint32_t t = SDL_GetTicks();
        uint32_t lo = 800, hi = 1200;
        if (getenv("ROF_AUTOSTART_HI")) hi = (uint32_t)atoi(getenv("ROF_AUTOSTART_HI"));
        if (t > lo && t < hi) c &= ~0x01;
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/* Convert Atari colour byte (hue/luma) to SDL_Color via YIQ.
   Hue = bits 7:4 (0=grey), luma = bits 3:1 (0-7), bit 0 ignored. */
void PlatformSDL::buildPalette() {
    /* Hue angles calibrated against atari800 5.2.0 default NTSC palette.
       Derived from known reference points: hue 2 ≈10.5°, hue 7 ≈143°,
       hue 9 ≈196°, hue 12 ≈276°; step ≈26.5°/hue, base at hue 1 ≈344°.
       Hue 0 = achromatic (no angle used).                                */
    static const double HUE_ANGLES[16] = {
          0,  344.0,  10.5,  37.0,  63.5,  90.0, 116.5, 143.0,
        169.5, 196.0, 222.5, 249.0, 275.5, 302.0, 328.5, 355.0
    };
    for (int c = 0; c < 256; c++) {
        int h     = (c >> 4) & 0xF;
        int lbits = (c >> 1) & 0x7;
        /* Quadratic luma curve fitted to atari800 reference grey levels:
           $00→4, $04→51, $06→79  (vs old linear 18+30k which was too bright). */
        float Y = 1.5f * lbits * lbits + 20.5f * lbits + 4.0f;
        float r, g, b;
        if (h == 0) {
            r = g = b = Y;
        } else {
            double angle = HUE_ANGLES[h] * M_PI / 180.0;
            float  chroma = 49.0f;   /* reduced from 60 to match reference saturation */
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

    /* colHW[] was seeded from OS shadows in tickVBI() before the VBI
       handler ran.  The handler may have updated them (e.g. attract-mode
       EOR/AND in vbi_handler_1); DLI handlers update them further mid-scan.
       Do NOT re-seed here — that would undo the VBI's colour transforms.  */

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
    /* DLI timing: a DLI tagged on a DL entry takes effect (its register writes
       become visible) at display scan line  entryStartY + max(scans, 2).
       For multi-scan entries this is the last scanline + 1 (the classic "fire at
       the start of the next entry"); for single-scan entries (mode F/E/C) it is
       one line further, because the handler's GTIA writes don't complete before
       the immediately-following 1-scan line is drawn.  We schedule the fire by
       scanline rather than "next entry" so both cases fall out of one rule.
       Verified against atari000.png: terrain GTIA-10 covers rows 42-127, the
       cockpit-frame HPOS step has no spike, and the title/instrument colours
       (all multi-scan entries) are unchanged from the previous behaviour.       */
    /* DLI fire scheduling with dense-region latency (verified against atari800
       per-scanline $C7 traces of BOTH the attract and the in-game cockpit, which
       are identical). All DLIs dispatch through one handler ($6CAD) that walks a
       slot table indexed by $C7 (incremented per DLI). Real hardware services
       EVERY DLI — none are missed — but in the dense cockpit region (a run of
       short <=2-scanline entries) the main 6502 loop is busy running game logic,
       so the DLI NMIs are serviced LATE by a roughly constant ~8 scanlines. The
       well-spaced terrain/title DLIs fire promptly (CPU idle on WSYNC).
       Ground truth (effect scanlines): slot6 COLBK:=$00 lands at ~136 (BELOW the
       canopy rows 128-135, keeping its frame edges green) and slot8 HPOSP1:=$94
       (the throttle reposition) lands at ~144. Our naive fire (entryStartY +
       max(scans,2)) puts slot6 at 128 and slot8 at 136 — exactly 8 early — so we
       add DENSE_DELAY in the dense region. Because up to ~5 cockpit DLIs are then
       in flight within that window, pending fires are held in a small FIFO and
       serviced in order (each indirectJmp advances $C7). This replaces the old
       "dead-window" miss model, which wrongly DROPPED slots 7/8 and hid the
       throttle.                                                                  */
    const int DLI_SERVICE = 8;       /* per-DLI service cost (lines) for serializing  */
    int lastDliFireY = -1000;        /* fire scanline of the most recently SCHEDULED DLI */
    int pendFireY[16];               /* FIFO of pending DLI fire scanlines           */
    int pendN = 0;

    while (scanY < ROF_NATIVE_H && guard++ < 1024) {
        int entryStartY = scanY;
        uint8_t instr = mem[dp++];
        uint8_t mode  = instr & 0x0F;
        bool    lms   = (instr & 0x40) != 0;

        /* Service any pending DLIs whose (possibly delayed) fire scanline has
           arrived, in FIFO order.  They run before this entry renders, so their
           register writes apply to this entry's playfield and PM.  Each fire
           advances $C7 via the dispatch handler.                                */
        while (pendN > 0 && entryStartY >= pendFireY[0] && dliAddr != 0) {
            indirectJmp(dliAddr);
            for (int i = 1; i < pendN; i++) pendFireY[i - 1] = pendFireY[i];
            pendN--;
        }

        if (mode == 0) {
            /* Blank lines: count = bits 6:4 + 1. */
            int count = ((instr >> 4) & 0x7) + 1;
            scanY += count;
            if (instr & 0x80) {                    /* blank can have DLI too */
                int fireY = entryStartY + (count >= 2 ? count : 2);
                if (fireY < lastDliFireY + DLI_SERVICE) fireY = lastDliFireY + DLI_SERVICE;
                lastDliFireY = fireY;
                if (pendN < (int)(sizeof pendFireY / sizeof *pendFireY))
                    pendFireY[pendN++] = fireY;
            }
            renderPMGraphicsRange(entryStartY, scanY - 1);
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

        /* This entry's DLI (if any) is scheduled after `scans` is known, below. */

        /* Bits 7:6 of PRIOR select the GTIA display mode for this row:
             00 = normal GTIA (standard ANTIC mode interpretation)
             01 = GTIA mode 9  (80 px, 4bpp, luma only from COLBK hue)
             10 = GTIA mode 10 (80 px, 4bpp, nibble → 9 color registers)
             11 = GTIA mode 11 (not used in RoF, fall through to normal) */
        uint8_t rowGprior = gprior;

        /* Read current colours from colHW[] — updated by DLI above.     */
        SDL_Color bgClr  = atariColor(colHW[8]);
        SDL_Color pf0Clr = atariColor(colHW[4]);
        SDL_Color pf1Clr = atariColor(colHW[5]);
        SDL_Color pf2Clr = atariColor(colHW[6]);
        uint32_t  bgPx   = SDL_MapRGB(bufferSurface->format,
                                      bgClr.r, bgClr.g, bgClr.b);

        /* Scanlines per display-list entry and NORMAL-mode bytes per screen row.
           rowBytes is the NORMAL-mode byte count. The ANTIC memory-scan counter
           advances by the number of bytes ACTUALLY fetched per line, which scales
           with playfield width (NARROW/NORMAL/WIDE) — i.e. by renderCount, not
           rowBytes. (In WIDE, mode D/E/F fetch 48 bytes/line, not 40; advancing
           by 40 shears the bitmap 8 bytes/row — the canopy-frame skew.)        */
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

        /* Schedule this entry's DLI now that the scan count is known.
           Base effect line = entryStartY + max(scans, 2), but a DLI cannot be
           serviced until DLI_SERVICE lines after the previous one was (the 6502
           handler chain occupies the CPU). Isolated DLIs (terrain/title, far
           apart) thus keep their natural timing, while the clustered cockpit
           DLIs serialize and pile up — reproducing the measured real-HW effect
           scanlines (slot6 COLBK:=$00 at ~136, below the green canopy; slot8
           HPOSP1:=$94 throttle reposition at ~144).                              */
        if (instr & 0x80) {
            int fireY = entryStartY + (scans >= 2 ? scans : 2);
            if (fireY < lastDliFireY + DLI_SERVICE) fireY = lastDliFireY + DLI_SERVICE;
            lastDliFireY = fireY;
            if (pendN < (int)(sizeof pendFireY / sizeof *pendFireY))
                pendFireY[pendN++] = fireY;
        }

        /* Playfield width and horizontal offset.
           The 384-px buffer represents the full WIDE (192 colour-clock) output.
           NORMAL (160cc = 320px) is centred at x=32; NARROW (128cc = 256px) at x=64.
             WIDE  (bits=11): playfieldPx=384, xOff=  0, renderCount = rowBytes*6/5
             NORMAL(bits=10): playfieldPx=320, xOff= 32, renderCount = rowBytes
             NARROW(bits=01): playfieldPx=256, xOff= 64, renderCount = rowBytes*4/5  */
        int pfBits = dmactl & 0x03;
        int xOff = (pfBits == 3) ?  0 : (pfBits == 2) ? 32 : 64;
        int renderCount = (pfBits == 3) ? rowBytes * 6 / 5 :
                          (pfBits == 1) ? rowBytes * 4 / 5 : rowBytes;

        if (dataAddr == 0) { scanY += scans; renderPMGraphicsRange(entryStartY, scanY - 1); continue; }

        /* GTIA modes 9 and 10 reinterpret ANTIC's serial output as 4-bit pixels.
           Our GTIA path below reads each data byte as two nibbles, which is the
           correct interpretation only for the 1-bpp hi-res stream of ANTIC mode F
           (the terrain). The 4-colour 2-bpp modes (D/E) send a different AN-signal
           stream, so feeding their raw bytes through the nibble path is wrong —
           e.g. the cockpit canopy (mode D) came out green in the middle instead of
           grey. Render those as their normal ANTIC mode instead; with COLBK left
           green by the cockpit DLI timing, mode D then yields green frame edges
           (colour index 0 = COLBK) over a grey body (COLPF) — matching atari800.
           (Only mode D is excluded — the confirmed cockpit-canopy case; mode E is
           left on the GTIA path since no observed screen needs it as plain E.)   */
        uint8_t gtiaMode = (rowGprior >> 6) & 0x03;
        if ((gtiaMode == 1 || gtiaMode == 2) && mode != 0xD) {
            /* Pre-build a 9-entry colour table for mode 10 (reused each row). */
            uint32_t creg10[9];
            if (gtiaMode == 2) {
                for (int i = 0; i < 9; i++) {
                    SDL_Color c = atariColor(colHW[i]);
                    creg10[i] = SDL_MapRGB(bufferSurface->format,
                                           c.r, c.g, c.b);
                }
            }
            uint8_t hue9 = colHW[8] & 0xF0; /* COLBK hue for mode 9 */
            /* GTIA modes 9/10/11 are shifted RIGHT by one colour clock relative
               to the ANTIC modes (GTIA delays the playfield one CC in these
               modes). Our buffer is 2 px per colour clock, so add 2 px. Without
               this the terrain (and its black marker dots) sits 2 px left of
               atari800 — e.g. leftmost dot column at screenshot x60 instead of 62. */
            const int GTIA_CC_SHIFT = 2;
            /* All scan lines of a DL entry read from the SAME 40 bytes.
               In GTIA mode, ANTIC fetches one batch; dataAddr advances once. */
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                for (int b = 0; b < renderCount; b++) {
                    uint8_t byte = mem[(dataAddr + b) & 0xFFFF];
                    uint8_t nib[2] = { (uint8_t)(byte >> 4), (uint8_t)(byte & 0x0F) };
                    for (int n = 0; n < 2; n++) {
                        uint32_t px;
                        if (gtiaMode == 1) {
                            SDL_Color c = atariColor(hue9 | nib[n]);
                            px = SDL_MapRGB(bufferSurface->format,
                                            c.r, c.g, c.b);
                        } else {
                            px = creg10[(nib[n] <= 8) ? nib[n] : 8];
                        }
                        int x0 = (b * 2 + n) * 4 + xOff + GTIA_CC_SHIFT;
                        for (int x = x0; x < x0 + 4 && x < ROF_NATIVE_W; x++)
                            if (x >= 0) row[x] = px;
                    }
                }
                /* dataAddr does NOT advance per scan; advance once after entry */
            }
            dataAddr = (dataAddr + renderCount) & 0xFFFF;
            renderPMGraphicsRange(entryStartY, scanY - 1);
            continue;
        }

        if (mode == 0xF) {
            /* ANTIC mode F: 320 pixels, 1 bpp, COLBK / COLPF2.
               HSCROL shifts the bitmap right by hscrol colour clocks (pixels). */
            uint32_t c0 = bgPx;
            uint32_t c1 = SDL_MapRGB(bufferSurface->format,
                                      pf2Clr.r, pf2Clr.g, pf2Clr.b);
            /* If HSCROL is in effect (display list instruction has bit 4 set),
               use an extra byte at the start of the row data.                 */
            bool hscrolEn = (instr & 0x10) != 0;
            int  extraBytes = hscrolEn ? 2 : 0;
            int  shift      = hscrolEn ? hscrol : 0;
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                int totalBytes = renderCount + extraBytes;
                for (int b = 0; b < totalBytes; b++) {
                    uint8_t byte = mem[(dataAddr + b) & 0xFFFF];
                    for (int bit = 7; bit >= 0; bit--) {
                        int px = b * 8 + (7 - bit) - shift + xOff;
                        if (px >= 0 && px < ROF_NATIVE_W)
                            row[px] = (byte >> bit) & 1 ? c1 : c0;
                    }
                }
                dataAddr = (dataAddr + renderCount) & 0xFFFF;
            }
        } else if (mode == 0xE || mode == 0xD) {
            /* ANTIC mode E/D: 160 pixels, 2 bpp, 4 colours.
               All scan lines of a DL entry share the SAME 40-byte row;
               dataAddr advances once per entry (not once per scan).   */
            SDL_Color mc[4] = { bgClr, pf0Clr, pf1Clr, pf2Clr };
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                for (int b = 0; b < renderCount; b++) {
                    uint8_t byte = mem[(dataAddr + b) & 0xFFFF];
                    for (int pp = 0; pp < 4; pp++) {
                        int ci = (byte >> (6 - pp * 2)) & 3;
                        int px = (b * 4 + pp) * 2 + xOff;
                        if (px >= 0 && px < ROF_NATIVE_W - 1) {
                            uint32_t col = SDL_MapRGB(bufferSurface->format,
                                mc[ci].r, mc[ci].g, mc[ci].b);
                            row[px] = row[px + 1] = col;
                        }
                    }
                }
                /* dataAddr does NOT advance per scan */
            }
            dataAddr = (dataAddr + renderCount) & 0xFFFF;
        } else if (mode == 0x2 || mode == 0x3 || mode == 0x4 || mode == 0x5) {
            /* ANTIC character modes, 40 chars/line (WIDE 48, NARROW 24).
               Modes 2/3: hi-res 1-bit-per-pixel, 8 px/char, 2 colours (COLPF2 fg
                 on COLBK), with NTSC artifact blending of adjacent differing bits.
               Modes 4/5: 4-colour, 2 BITS per pixel → 4 px/char (each 2 buffer px).
                 2-bit value → 00=COLBK, 01=COLPF0, 10=COLPF1, 11=COLPF2 (char bit7
                 =0) / COLPF3 (bit7=1). Glyph index = char & 0x7F. (Matches atari800
                 draw_antic_4: lookup2 over chdata&0xC0/0x30/0x0C/0x03.)             */
            bool twoBpp = (mode == 4 || mode == 5);
            uint16_t csBase = (uint16_t)chbase << 8;
            int charsPerRow = renderCount;   /* WIDE=48, NORMAL=40, NARROW=24 */
            int scansPerChar = (mode == 3) ? 10 : (mode == 5) ? 16 : 8;
            int charScan = 0;
            uint16_t rowAddr = dataAddr;
            uint32_t pf0Px = SDL_MapRGB(bufferSurface->format, pf0Clr.r, pf0Clr.g, pf0Clr.b);
            uint32_t pf1Px = SDL_MapRGB(bufferSurface->format, pf1Clr.r, pf1Clr.g, pf1Clr.b);
            uint32_t pf2Px = SDL_MapRGB(bufferSurface->format, pf2Clr.r, pf2Clr.g, pf2Clr.b);
            SDL_Color pf3Clr = atariColor(colHW[7]);
            uint32_t pf3Px = SDL_MapRGB(bufferSurface->format, pf3Clr.r, pf3Clr.g, pf3Clr.b);
            for (int s = 0; s < scans && scanY < ROF_NATIVE_H; s++, scanY++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanY * bufferSurface->pitch);
                int bitmapRow = (scansPerChar > 8) ?
                    (charScan < 8 ? charScan : 7) : charScan;
                for (int c = 0; c < charsPerRow; c++) {
                    uint8_t chByte = mem[(rowAddr + c) & 0xFFFF];
                    if (twoBpp) {
                        uint8_t bits = mem[(csBase + (chByte & 0x7F) * 8 + bitmapRow) & 0xFFFF];
                        uint32_t pf3orPf2 = (chByte & 0x80) ? pf3Px : pf2Px;
                        uint32_t tab[4] = { bgPx, pf0Px, pf1Px, pf3orPf2 };
                        for (int pix = 0; pix < 4; pix++) {
                            uint32_t px = tab[(bits >> (6 - pix * 2)) & 3];
                            int x = c * 8 + pix * 2 + xOff;
                            if (x >= 0 && x < ROF_NATIVE_W)     row[x]     = px;
                            if (x+1 >= 0 && x+1 < ROF_NATIVE_W) row[x + 1] = px;
                        }
                        continue;
                    }
                    /* modes 2/3: 1 bpp + NTSC artifact blend */
                    bool inv = (chByte & 0x80) != 0;
                    uint8_t bits = mem[(csBase + (chByte & 0x7F) * 8 + bitmapRow) & 0xFFFF];
                    auto blended = [&](uint32_t a, uint32_t b) -> uint32_t {
                        return SDL_MapRGB(bufferSurface->format,
                            (((a)&0xFF) + ((b)&0xFF)) >> 1,
                            ((((a)>>8)&0xFF) + (((b)>>8)&0xFF)) >> 1,
                            ((((a)>>16)&0xFF) + (((b)>>16)&0xFF)) >> 1);
                    };
                    for (int pair = 0; pair < 4; pair++) {
                        int hibit = 7 - pair * 2, lobit = hibit - 1;
                        bool hi = (((bits >> hibit) & 1) != 0) != inv;
                        bool lo = (((bits >> lobit) & 1) != 0) != inv;
                        uint32_t hiPx = hi ? pf2Px : bgPx;
                        uint32_t loPx = lo ? pf2Px : bgPx;
                        uint32_t mix  = (hi == lo) ? hiPx : blended(hiPx, loPx);
                        int px = c * 8 + pair * 2 + xOff;
                        if (px >= 0 && px < ROF_NATIVE_W)     row[px]     = mix;
                        if (px+1 >= 0 && px+1 < ROF_NATIVE_W) row[px + 1] = mix;
                    }
                }
                if (++charScan >= scansPerChar) {
                    charScan = 0;
                    rowAddr = (rowAddr + charsPerRow) & 0xFFFF;
                }
            }
            /* rowAddr already advanced past this entry's char row(s) during the
               loop; the ANTIC memory counter ends there. (Adding charsPerRow again
               here double-counted, leaving a no-LMS following entry — e.g. the
               modeD canopy rows 40-41 — reading 48 bytes too far → garbage.) */
            dataAddr = rowAddr;
        } else if (mode == 0x6 || mode == 0x7) {
            /* ANTIC modes 6/7: 20-column colour-character modes.
               Mode 6: 8 scans/char, mode 7: 16 scans/char.
               Each byte: bits[5:0]=char index (0-63), bits[7:6]=color register.
               Each bit displayed 2× wide → 20*16=320 display pixels.     */
            uint16_t csBase = (uint16_t)chbase << 8;
            int charsPerRow = renderCount;   /* WIDE=24, NORMAL=20, NARROW=12 */
            int scansPerChar = (mode == 7) ? 16 : 8;
            int charScan = 0;
            uint16_t rowAddr = dataAddr;
            SDL_Color colPFn[4] = { pf0Clr, pf1Clr, pf2Clr, pf2Clr };
            colPFn[3] = atariColor(mem[0x02C7]);
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
                        int px = (c * 8 + (7 - bit)) * 2 + xOff;
                        if (px >= 0 && px < ROF_NATIVE_W - 1)
                            row[px] = row[px + 1] = set ? fgPx : bgPx;
                    }
                }
                if (++charScan >= scansPerChar) {
                    charScan = 0;
                    rowAddr = (rowAddr + charsPerRow) & 0xFFFF;
                }
            }
            dataAddr = rowAddr;   /* rowAddr already advanced past this row (see mode 2-5) */
        } else {
            /* Modes 8–C not yet implemented; fill with background. */
            scanY += scans;
        }
        /* PM overlay for this entry's scan lines, with the HPOSM/HPOSP values
           that were active when this entry started (set by the previous DLI). */
        renderPMGraphicsRange(entryStartY, scanY - 1);
    }

    /* Below the last display-list mode line, ANTIC outputs the lower border =
       current COLBK until the frame ends. Re-fill those rows with the LIVE
       colHW[8] (post-DLI), not the frame-start value the initial fill used —
       otherwise they keep a stale colour. (atari800 shows black here because its
       COLBK is $00 at frame end; ours tracks COLBK too, so it matches once the
       cockpit COLBK is correct.) */
    if (scanY < ROF_NATIVE_H) {
        SDL_Rect r = { 0, scanY, ROF_NATIVE_W, ROF_NATIVE_H - scanY };
        SDL_Color bg = atariColor(colHW[8]);
        SDL_FillRect(bufferSurface, &r,
                     SDL_MapRGB(bufferSurface->format, bg.r, bg.g, bg.b));
    }
}

/* Render Player/Missile graphics for display scan lines [fromY, toY] using the
   HPOSM/HPOSP values current at the time of the call.  Called once per DL entry,
   after that entry's scheduled DLI (if any) has fired, so each segment uses the
   register values that were live for those scan lines.
   WIDE mode: 1 colour-clock = 2 buffer pixels → x0 = (hpos−32)×2, each bit = 2px.

   PM bitmap layout (single-line, DMACTL bit4=1):
     pmBase×256 + 0x300: missiles  M3=bits7:6, M2=5:4, M1=3:2, M0=1:0
     pmBase×256 + 0x400/0x500/0x600/0x700: players 0-3
   Double-line (DMACTL bit4=0): +0x180 missiles, +0x200/0x280/0x300/0x380 players. */
void PlatformSDL::renderPMGraphicsRange(int fromY, int toY) {
    if (pmbase == 0) return;
    if (fromY > toY) return;
    toY = SDL_min(toY, ROF_NATIVE_H - 1);

    bool doubleLine   = !(dmactl & 0x10);
    int  playerStride = doubleLine ? 128 : 256;
    int  pmBaseAddr   = (int)pmbase << 8;
    int  maxScan      = doubleLine ? 128 : 256;
    /* On hardware, PM/missile DMA fetches bitmap byte index = ANTIC_ypos (the
       absolute scan-line counter), and the playfield for that same line is also
       produced at ANTIC_ypos. ANTIC starts the display list at ypos 8 (the 8-line
       vertical-blank overscan; see atari800 antic.c "do {OVERSCREEN_LINE} while
       (ypos < 8)"). Our scanY=0 is the first DL instruction = ypos 8, so missile
       byte N must render at scanY = N - 8. Hence the offset is 8. Verified against
       atari000.png: this reproduces the frame line spanning rows 42-127 with the
       slant stepping at rows 50/64/78/92/106/120, exactly matching the reference. */
    const int PM_DL_OFFSET = 8;

    /* GTIA priority select (PRIOR/$D01B bits 0-3) decides, per player/missile,
       whether the playfield draws in front of it. We approximate "playfield in
       front" by occluding a PM pixel wherever the playfield already drew a
       non-background colour there (every mode path writes its background pixels
       as COLBK, so "== bakPx" reliably marks background). The four documented
       orderings (De Re Atari):
         $01: P0-3 > PF0-3 > BAK            (all players in front)
         $02: P0,P1 > PF0-3 > P2,P3 > BAK   (P2,P3 behind; P0,P1 in front)
         $04: PF0-3 > P0-3 > BAK            (all players behind)
         $08: PF0,PF1 > P0-3 > PF2,PF3 > BAK
       RoF uses $11/$94 (players in front / all behind) for terrain and $14 for
       the attract cockpit, but $02 for the in-flight instrument panel: the gauge
       contents are P2/P3 sitting BEHIND the dashboard playfield, so the bezel and
       crosshair (non-background playfield pixels) mask them, while P0/P1 (e.g. the
       centre crosshair) stay in front. We approximate "behind" as "occlude this
       PM pixel wherever the playfield already drew a non-COLBK colour there".
       $08 (PF0/PF1 vs PF2/PF3 split) isn't used by RoF; treat it as all-behind. */
    uint8_t priSel = gprior & 0x0F;
    auto pfOverPlayer = [priSel](int idx) -> bool {
        if (priSel & 0x04) return true;        /* $04: all players behind PF      */
        if (priSel & 0x08) return true;        /* $08 (unused): approximate as behind */
        if (priSel & 0x02) return idx >= 2;    /* $02: only P2,P3 behind PF        */
        return false;                          /* $01/none: players in front of PF */
    };
    SDL_Color bakC = atariColor(colHW[8]);
    uint32_t  bakPx = SDL_MapRGB(bufferSurface->format, bakC.r, bakC.g, bakC.b);

    auto drawPMPixels = [&](uint32_t* scanRow, int px, int pixPerBit, uint32_t col,
                            bool occlude) {
        for (int w = 0; w < pixPerBit; w++) {
            int p = px + w;
            if (p < 0 || p >= ROF_NATIVE_W) continue;
            if (occlude && scanRow[p] != bakPx) continue;  /* playfield wins */
            scanRow[p] = col;
        }
    };

    /* ---- Players (GRACTL bit 1) ---- */
    if (gractl & 0x02) {
        int playerBase = doubleLine ? 0x200 : 0x400;
        for (int p = 0; p < 4; p++) {
            uint8_t hpos  = hposP[p];
            SDL_Color col = atariColor(colHW[p]);
            uint32_t pCol = SDL_MapRGB(bufferSurface->format, col.r, col.g, col.b);
            int szBits    = sizePM[p] & 0x03;
            int pixPerBit = (szBits == 1) ? 4 : (szBits == 3) ? 8 : 2;
            int bitmapBase = pmBaseAddr + playerBase + p * playerStride;
            int x0 = ((int)hpos - 32) * 2;
            bool occP = pfOverPlayer(p);

            for (int y = 0; y < maxScan; y++) {
                int dy = (doubleLine ? y * 2 : y) - PM_DL_OFFSET;
                if (doubleLine ? (dy + 1 < fromY || dy > toY) : (dy < fromY || dy > toY)) continue;
                uint8_t bits = mem[(bitmapBase + y) & 0xFFFF];
                if (!bits) continue;
                for (int rep = 0; rep <= (doubleLine ? 1 : 0); rep++) {
                    int scanRow = dy + rep;
                    if (scanRow < fromY || scanRow > toY) continue;
                    if (scanRow < 0 || scanRow >= ROF_NATIVE_H) break;
                    uint32_t* row = reinterpret_cast<uint32_t*>(
                        static_cast<uint8_t*>(bufferSurface->pixels) +
                        scanRow * bufferSurface->pitch);
                    for (int bit = 7; bit >= 0; bit--) {
                        if ((bits >> bit) & 1)
                            drawPMPixels(row, x0 + (7 - bit) * pixPerBit, pixPerBit, pCol, occP);
                    }
                }
            }
        }
    }

    /* ---- Missiles (GRACTL bit 0) ----
       Fifth-player mode: when GPRIOR bit 4 ($10) is set, all four missiles are
       drawn in COLPF3 (colHW[7]) and act as a single 5th player, instead of each
       missile taking its matching player colour COLPM0-3. RoF's cockpit window
       frame uses this: during the terrain rows the DLI sets PRIOR=$94 (bit 4 set)
       and COLPF3=$06 (grey), so the missile frame renders grey — not the black of
       COLPM0-3=$00. (Verified against atari000.png.)                            */
    if (gractl & 0x01) {
        bool fifthPlayer = (gprior & 0x10) != 0;
        int missileBase = pmBaseAddr + (doubleLine ? 0x180 : 0x300);
        for (int y = 0; y < maxScan; y++) {
            int dy = (doubleLine ? y * 2 : y) - PM_DL_OFFSET;
            if (doubleLine ? (dy + 1 < fromY || dy > toY) : (dy < fromY || dy > toY)) continue;
            uint8_t mbyte = mem[(missileBase + y) & 0xFFFF];
            if (!mbyte) continue;
            for (int rep = 0; rep <= (doubleLine ? 1 : 0); rep++) {
                int scanRow = dy + rep;
                if (scanRow < fromY || scanRow > toY) continue;
                if (scanRow < 0 || scanRow >= ROF_NATIVE_H) break;
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(bufferSurface->pixels) +
                    scanRow * bufferSurface->pitch);
                for (int m = 0; m < 4; m++) {
                    int szBits    = (sizeM >> (m * 2)) & 0x03;
                    int pixPerBit = (szBits == 1) ? 4 : (szBits == 3) ? 8 : 2;
                    SDL_Color col = atariColor(fifthPlayer ? colHW[7] : colHW[m]);
                    uint32_t mCol = SDL_MapRGB(bufferSurface->format, col.r, col.g, col.b);
                    int x0      = ((int)hposM[m] - 32) * 2;
                    int hiShift = m * 2 + 1;
                    int loShift = m * 2;
                    /* 5th-player missiles act as playfield (PF3), so they are not
                       occluded by playfield; normal missiles share their matching
                       player's priority (M0/M1 with P0/P1, M2/M3 with P2/P3).     */
                    bool occ = !fifthPlayer && pfOverPlayer(m);
                    if ((mbyte >> hiShift) & 1)
                        drawPMPixels(row, x0,             pixPerBit, mCol, occ);
                    if ((mbyte >> loShift) & 1)
                        drawPMPixels(row, x0 + pixPerBit, pixPerBit, mCol, occ);
                }
            }
        }
    }
}

/* Save a 336×240 PNG screenshot matching atari800's F10 capture format.
   Our 384-px buffer holds the full WIDE playfield; we crop to the centre
   336px (x=ROF_SCREENSHOT_X0 .. x=ROF_SCREENSHOT_X0+ROF_SCREENSHOT_W-1)
   which corresponds to the area atari800 captures.
   Files are named rof000.png, rof001.png, … in the working directory.  */
void PlatformSDL::saveScreenshot() {
    char filename[32];
    snprintf(filename, sizeof(filename), "rof%03d.png", screenshotIndex);

    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "[rof] screenshot: cannot open %s\n", filename);
        return;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); return; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); fclose(fp); return; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        fprintf(stderr, "[rof] screenshot: libpng error\n");
        return;
    }

    png_init_io(png, fp);

    png_set_IHDR(png, info, ROF_SCREENSHOT_W, ROF_NATIVE_H, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    static uint8_t row[ROF_SCREENSHOT_W * 3];

    SDL_LockSurface(bufferSurface);
    for (int y = 0; y < ROF_NATIVE_H; y++) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(bufferSurface->pixels) +
            y * bufferSurface->pitch) + ROF_SCREENSHOT_X0;
        uint8_t* dst = row;
        for (int x = 0; x < ROF_SCREENSHOT_W; x++) {
            uint32_t px = src[x];
            *dst++ = (uint8_t)(px & 0xFF);
            *dst++ = (uint8_t)((px >> 8) & 0xFF);
            *dst++ = (uint8_t)((px >> 16) & 0xFF);
        }
        png_write_row(png, row);
    }
    SDL_UnlockSurface(bufferSurface);

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);

    screenshotIndex++;
    printf("[rof] screenshot → %s\n", filename);
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
        // Act on quit immediately: the boot runs in transpiled spin-waits that never
        // return to a top-level loop, so a `quit` flag would never be checked.  exit(0)
        // here makes window-close, ESC, and Ctrl-C (SDL posts SDL_QUIT for SIGINT) all
        // terminate the process.  renderFrame() calls pollEvents() every frame, so this
        // is reached each frame during the build.
        if (ev.type == SDL_QUIT) { quit = true; exit(0); }
        if (ev.type == SDL_KEYDOWN) {
            switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE: quit = true; exit(0); break;
            case SDL_SCANCODE_F10:   saveScreenshot(); break;
            default: break;
            }
        }
    }
}

/* Fire the VBI and blit the display if a new frame is pending.
   Call from spin-waits that own a full VBI tick (FUN_3cb1, L_656e).
   Fires tickVBI() first so both VBI and render happen in one call.
   SDL_UpdateWindowSurface is only called once per VBI (50 Hz),
   so Metal's nextDrawable never accumulates pressure.               */
void PlatformSDL::renderFrame() {
    /* Do NOT call tickVBI() here: all spin-wait hooks call platform_tick_vbi()
       first, then platform_render_frame().  Calling tickVBI() again inside
       renderFrame() would double-fire the VBI if rendering takes ≥20 ms,
       causing RTCLOK ($14) to increment twice per loop iteration and making
       spin-waits overshoot their target (e.g. always missing value 1).       */
    if (!renderNeeded) return;
    renderNeeded = false;

    /* During the ROF_START=gameplay fast-forward, the VBI fires every spin-loop
       iteration (thousands/sec).  Blitting each one would bottleneck on the GPU
       and defeat the speed-up, so pace the actual blit to ~30Hz wall-clock —
       the launch still plays visibly, just sped up, and reaches flight in ~1s. */
    if ((rofStartStage() == ROF_STAGE_FLIGHT) && !reachedFlight_) {
        uint32_t now = SDL_GetTicks();
        if (now - lastFFRender_ < 33u) return;
        lastFFRender_ = now;
    }

    pollEvents();

    renderAtariDisplay();

    SDL_Rect src  = { 0, 0, ROF_NATIVE_W, ROF_NATIVE_H };
    SDL_Rect dst  = { 0, 0, ROF_WINDOW_W, ROF_WINDOW_H };
    SDL_BlitScaled(bufferSurface, &src, windowSurface, &dst);
    SDL_UpdateWindowSurface(window);
}
