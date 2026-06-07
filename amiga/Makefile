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

$(shell mkdir -p obj/framework obj/src out)

# C++ sources: framework hw classes + RoF application
FW_CPPS  := $(wildcard framework/*.cpp)
SRC_CPPS := $(wildcard src/*.cpp)
FW_OBJS  := $(patsubst framework/%.cpp,obj/framework/%.o,$(FW_CPPS))
SRC_OBJS := $(patsubst src/%.cpp,obj/src/%.o,$(SRC_CPPS))
CPP_OBJS := $(FW_OBJS) $(SRC_OBJS)

# Support runtime (freestanding CRT: program entry, memset…)
S_OBJS   := obj/gcc8_a_support.o obj/incbin.o
C_OBJS   := obj/gcc8_c_support.o

OBJECTS  := $(CPP_OBJS) $(C_OBJS) $(S_OBJS)

WARN      = -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
# -m68000: target is A500 68000. -msoft-float: no FPU.
# -DNO_ASSEMBLER: use the portable C++ bodies; no *Assembler.s or vasm step.
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

obj/incbin.o : src/incbin.s
	$(info ASM  $<)
	@$(AS) $(ASFLAGS) -o $@ "$(CURDIR)/src/incbin.s"

clean:
	@$(RM) -r obj/ out/

-include $(CPP_OBJS:.o=.d) $(C_OBJS:.o=.d)
