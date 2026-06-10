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

$(shell mkdir -p obj/framework obj/src obj/audio out)

# C++ sources: framework hw classes + RoF application
FW_CPPS  := $(wildcard framework/*.cpp)
SRC_CPPS := $(wildcard src/*.cpp)
FW_OBJS  := $(patsubst framework/%.cpp,obj/framework/%.o,$(FW_CPPS))
SRC_OBJS := $(patsubst src/%.cpp,obj/src/%.o,$(SRC_CPPS))
CPP_OBJS := $(FW_OBJS) $(SRC_OBJS)

# M5 audio slice: the 6502-converted cpu/bus core + generated audio functions.
# Compiled as plain C (not C++), with include paths into the repo src tree.
# --gc-sections keeps only what station_audio transitively calls.
AUDIO_C_SRCS := $(REPO)/src/cpu/cpu.c $(REPO)/src/gen/rof_gen.c $(REPO)/src/gen/rof_native.c
AUDIO_C_OBJS := obj/audio/cpu.o obj/audio/rof_gen.o obj/audio/rof_native.o

# Support runtime (freestanding CRT: program entry, memset…)
S_OBJS   := obj/gcc8_a_support.o obj/incbin.o
C_OBJS   := obj/gcc8_c_support.o

OBJECTS  := $(CPP_OBJS) $(C_OBJS) $(S_OBJS) $(AUDIO_C_OBJS)

WARN      = -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
# -m68000: target is A500 68000. -msoft-float: no FPU.
# -DNO_ASSEMBLER: use the portable C++ bodies; no *Assembler.s or vasm step.
# Audio C flags: same base but with repo src/ include paths for cpu.h/bus.h/rof_gen.c
AUDIO_CINCLUDES = -I"$(CURDIR)/framework/compat-include" \
                  -I"$(REPO)/src/cpu" \
                  -I"$(REPO)/src/gen" \
                  -I"$(REPO)/src/platform"
AUDIO_CFLAGS  = -g -MP -MMD -m68000 -msoft-float -O2 -nostdlib -fomit-frame-pointer \
                -fno-tree-loop-distribution -ffunction-sections -fdata-sections \
                -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
                -Wno-implicit-function-declaration -Wno-int-conversion \
                $(AUDIO_CINCLUDES)
CCFLAGS   = -g -MP -MMD -m68000 -msoft-float -O2 -nostdlib -fomit-frame-pointer \
            -fno-tree-loop-distribution -ffunction-sections -fdata-sections \
            -DNO_ASSEMBLER $(WARN)
CPPFLAGS  = $(CCFLAGS) -fno-rtti -fno-exceptions -fno-use-cxa-atexit -std=gnu++17 \
            -fpermissive -Wno-write-strings -Wno-pointer-arith \
            -include "$(CURDIR)/framework/SASCCompat.h" \
            -I"$(CURDIR)/framework" \
            -I"$(CURDIR)/framework/compat-include" \
            -I"$(CURDIR)/src"
ASFLAGS   = -mcpu=68000 -g --register-prefix-optional -I"$(SDKDIR)"
LDFLAGS   = -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(OUT).map

all: $(OUT).exe

$(OUT).exe: $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(OUT).exe

$(OUT).elf: $(OBJECTS)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(OBJECTS) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins -S $@ > $(OUT).s 2>/dev/null || true

obj/framework/%.o : framework/%.cpp
	$(info C++  $<)
	@$(CC) $(CPPFLAGS) -c -o $@ "$(CURDIR)/$<"

obj/src/%.o : src/%.cpp
	$(info C++  $<)
	@$(CC) $(CPPFLAGS) -c -o $@ "$(CURDIR)/$<"

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
INCBIN_DEPS := assets/cockpit.raw ../disasm/screen3_mem.bin
obj/incbin.o : src/incbin.s $(INCBIN_DEPS)
	$(info ASM  $<)
	@$(AS) $(ASFLAGS) -o $@ "$(CURDIR)/src/incbin.s"

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
	$(info C    $< [audio])
	@$(CC) $(AUDIO_CFLAGS) -c -o $@ "$(abspath $<)"

clean:
	@$(RM) -r obj/ out/

-include $(CPP_OBJS:.o=.d) $(C_OBJS:.o=.d) $(AUDIO_C_OBJS:.o=.d)
