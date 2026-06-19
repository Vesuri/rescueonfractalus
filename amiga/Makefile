# Rescue on Fractalus — Amiga cross-build (m68k-amiga-elf-gcc, NO_ASSEMBLER)
#
# Usage (from amiga/ with toolchain on PATH via `. env.sh`):
#   make          — build out/RoF.exe
#   make clean    — remove obj/ and out/
#   ./run.sh      — boot in FS-UAE (OCS A500)
#   ./debug.sh    — source-level debug via FS-UAE GDB stub
#
# NO_ASSEMBLER is the only build mode for this plan: the dA JoRMaS framework
# *Assembler.s are not vendored; we compile the portable C++ bodies throughout.
# See amiga/ARCH.md and amiga/framework/UPSTREAM.md for rationale.

OUT      = out/RoF
CC       = m68k-amiga-elf-gcc
AS       = m68k-amiga-elf-as
OBJDUMP  = m68k-amiga-elf-objdump
SDKDIR   = $(abspath $(dir $(shell which $(CC)))../m68k-amiga-elf/sys-include)
SUPPORT  = $(abspath $(dir $(shell which $(CC)))../../support)

# Repository root (parent of amiga/)
REPO     = ..

$(shell mkdir -p obj/framework obj/src obj/platform obj/audio out)

# C++ sources: framework hw classes + RoF application.  The code now lives in the
# unified source tree under $(REPO)/src/platform/amiga (and .../amiga/framework); this
# amiga/ directory holds only build infrastructure (this Makefile, env.sh, run.sh,
# assets/, the diag scripts).
AMIGA_SRC := $(REPO)/src/platform/amiga
FW_CPPS  := $(wildcard $(AMIGA_SRC)/framework/*.cpp)
SRC_CPPS := $(wildcard $(AMIGA_SRC)/*.cpp)
FW_OBJS  := $(patsubst $(AMIGA_SRC)/framework/%.cpp,obj/framework/%.o,$(FW_CPPS))
SRC_OBJS := $(patsubst $(AMIGA_SRC)/%.cpp,obj/src/%.o,$(SRC_CPPS))

# Shared platform layer (repo src/): the abstract Platform base, the C bridge that
# dispatches the transpiled code's platform_* calls to the Platform singleton, and the
# common entry point.  Built with the SAME C++ flags + -DROF_PLATFORM_AMIGA (so main.cpp
# selects PlatformAmiga) and -I src/platform (CPPFLAGS below).
PLATFORM_CPPS := $(REPO)/src/platform/Platform.cpp $(REPO)/src/platform/platform_cbridge.cpp $(REPO)/src/main.cpp
PLATFORM_OBJS := obj/platform/Platform.o obj/platform/platform_cbridge.o obj/platform/main.o

CPP_OBJS := $(FW_OBJS) $(SRC_OBJS) $(PLATFORM_OBJS)

# M5 audio slice: the 6502-converted cpu/bus core + generated audio functions.
# Compiled as plain C (not C++), with include paths into the repo src tree.
# --gc-sections keeps only what station_audio transitively calls.
AUDIO_C_SRCS := $(REPO)/src/cpu/cpu.c $(REPO)/src/gen/rof_gen.c $(REPO)/src/gen/rof_native.c $(REPO)/src/gen/rof_manual.c
AUDIO_C_OBJS := obj/audio/cpu.o obj/audio/rof_gen.o obj/audio/rof_native.o obj/audio/rof_manual.o

# Support runtime (freestanding CRT: program entry, memset…)
S_OBJS   := obj/gcc8_a_support.o obj/incbin.o
C_OBJS   := obj/gcc8_c_support.o

OBJECTS  := $(CPP_OBJS) $(C_OBJS) $(S_OBJS) $(AUDIO_C_OBJS)

WARN      = -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
# -m68000: target is A500 68000. -msoft-float: no FPU.
# -DNO_ASSEMBLER: use the portable C++ bodies; no *Assembler.s or vasm step.
# Audio C flags: same base but with repo src/ include paths for cpu.h/bus.h/rof_gen.c
AUDIO_CINCLUDES = -I"$(AMIGA_SRC)/framework/compat-include" \
                  -I"$(REPO)/src/cpu" \
                  -I"$(REPO)/src/gen" \
                  -I"$(REPO)/src/platform"
# Optimization level — overridable for debug builds, e.g.
#   make OPT='-O0' NATIVE_OPT='-O0'   (also drops -fomit-frame-pointer for clean backtraces)
OPT       ?= -O2 -fomit-frame-pointer
NATIVE_OPT ?= -O3 -funroll-loops
# Debug probes: headless auto-launch + flight/init sub-frame timing accumulators (read via
# the gdb stub, e.g. amiga/diag_run.sh + diag_timing.gdb).  Off by default; enable with
# `make PROBES=1`.  Turns on ROF_FLIGHT_PROBE (PlatformAmiga.cpp + rof_native.c FP_* macros)
# and ROF_TDRAW_PROF (terrain_draw_frame sub-phase split).
ifdef PROBES
EXTRA_DEFINES += -DROF_FLIGHT_PROBE -DROF_TDRAW_PROF
endif
AUDIO_CFLAGS  = -g -MP -MMD -m68000 -msoft-float $(OPT) -nostdlib \
                -fno-tree-loop-distribution -ffunction-sections -fdata-sections \
                -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
                -Wno-implicit-function-declaration -Wno-int-conversion \
                $(AUDIO_CINCLUDES) $(EXTRA_DEFINES)
CCFLAGS   = -g -MP -MMD -m68000 -msoft-float $(OPT) -nostdlib \
            -fno-tree-loop-distribution -ffunction-sections -fdata-sections \
            -DNO_ASSEMBLER $(WARN) $(EXTRA_DEFINES)
CPPFLAGS  = $(CCFLAGS) -fno-rtti -fno-exceptions -fno-use-cxa-atexit -std=gnu++17 \
            -fpermissive -Wno-write-strings -Wno-pointer-arith \
            -DROF_PLATFORM_AMIGA \
            -include "$(AMIGA_SRC)/framework/SASCCompat.h" \
            -I"$(AMIGA_SRC)/framework" \
            -I"$(AMIGA_SRC)/framework/compat-include" \
            -I"$(AMIGA_SRC)" \
            -I"$(REPO)/src/platform" \
            -I"$(CURDIR)"
ASFLAGS   = -mcpu=68000 -g --register-prefix-optional -I"$(SDKDIR)" -I"$(CURDIR)"
LDFLAGS   = -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(OUT).map

all: $(OUT).exe

$(OUT).exe: $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(OUT).exe

$(OUT).elf: $(OBJECTS)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(OBJECTS) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins -S $@ > $(OUT).s 2>/dev/null || true

obj/framework/%.o : $(AMIGA_SRC)/framework/%.cpp
	$(info C++  $<)
	@$(CC) $(CPPFLAGS) -c -o $@ "$(abspath $<)"

obj/src/%.o : $(AMIGA_SRC)/%.cpp
	$(info C++  $<)
	@$(CC) $(CPPFLAGS) -c -o $@ "$(abspath $<)"

# Shared platform layer (repo src/ + src/platform/): same C++ flags as the Amiga src.
obj/platform/Platform.o : $(REPO)/src/platform/Platform.cpp
	$(info C++  $< [platform])
	@$(CC) $(CPPFLAGS) -c -o $@ "$(abspath $<)"
obj/platform/platform_cbridge.o : $(REPO)/src/platform/platform_cbridge.cpp
	$(info C++  $< [platform])
	@$(CC) $(CPPFLAGS) -c -o $@ "$(abspath $<)"
obj/platform/main.o : $(REPO)/src/main.cpp
	$(info C++  $< [platform])
	@$(CC) $(CPPFLAGS) -c -o $@ "$(abspath $<)"

obj/gcc8_c_support.o : $(SUPPORT)/gcc8_c_support.c
	$(info C    $<)
	@$(CC) $(CCFLAGS) -c -o $@ "$<"

obj/gcc8_a_support.o : $(SUPPORT)/gcc8_a_support.s
	$(info ASM  $<)
	@$(AS) $(ASFLAGS) -o $@ "$<"

# NOTE: .incbin dependencies are invisible to gcc -MMD, so list them explicitly.
# Without this, editing/regenerating an embedded dump leaves a stale blob in the
# binary while the on-disk file looks correct (this exact trap shifted the SFX
# music + title data by one byte and cost a long debugging session).
# incbin.s lives in the unified tree ($(AMIGA_SRC)) but its .incbin paths are resolved
# relative to this amiga/ build dir (where assets/ and ../rof.xex live) via ASFLAGS
# -I"$(CURDIR)"; INCBIN_DEPS are likewise listed relative to amiga/.
INCBIN_DEPS := assets/cockpit.raw ../disasm/screen3_mem.bin ../rof.xex assets/atari_osrom.bin
obj/incbin.o : $(AMIGA_SRC)/incbin.s $(INCBIN_DEPS)
	$(info ASM  $<)
	@$(AS) $(ASFLAGS) -o $@ "$(abspath $(AMIGA_SRC)/incbin.s)"

# Audio C translation units (rof_gen.c is large; GC sections keep only the audio slice)
obj/audio/cpu.o : $(REPO)/src/cpu/cpu.c
	$(info C    $< [audio])
	@$(CC) $(AUDIO_CFLAGS) -c -o $@ "$(abspath $<)"

obj/audio/rof_gen.o : $(REPO)/src/gen/rof_gen.c
	$(info C    $< [audio — may take ~30 s])
	@$(CC) $(AUDIO_CFLAGS) -c -o $@ "$(abspath $<)"

# rof_native.c: hand-written native twins (divide_16x16, …) the transpile leaves
# unimplemented (VALIDATE_FUNCS seam) — required once flight code references them.
obj/audio/rof_native.o : $(REPO)/src/gen/rof_native.c
	$(info C    $< [audio, native])
	@$(CC) $(AUDIO_CFLAGS) $(NATIVE_OPT) -c -o $@ "$(abspath $<)"

# rof_manual.c: hand-written replacements for self-modifying routines (screen_page_swap,
# the DLI handlers).  Needed now that the genuine station_init/game_main_loop chain is
# linked (station_init tail-calls screen_page_swap).  GC-sections keeps only what's used.
obj/audio/rof_manual.o : $(REPO)/src/gen/rof_manual.c
	$(info C    $< [audio])
	@$(CC) $(AUDIO_CFLAGS) -c -o $@ "$(abspath $<)"

clean:
	@$(RM) -r obj/ out/

-include $(CPP_OBJS:.o=.d) $(C_OBJS:.o=.d) $(AUDIO_C_OBJS:.o=.d)
