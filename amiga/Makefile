# Rescue on Fractalus — Amiga cross-build (m68k-amiga-elf-gcc)
#
# Usage (from amiga/ with toolchain on PATH via `. env.sh`):
#   make          — build out/RoF.exe
#   make clean    — remove obj/ and out/
#   ./run.sh      — boot in FS-UAE (OCS A500)
#   ./debug.sh    — source-level debug via FS-UAE GDB stub
#
# The dA JoRMaS framework's hand-written m68k *Assembler.s routines are vendored
# (framework/*Assembler.s, vasm-assembled) and reached from GCC via register-
# marshalling bridges (the `#if defined(ASSEMBLER) && !defined(__SASC)` sections in
# the framework .cpp).  ASSEMBLER is enabled by default (Util.h); pass -DNO_ASSEMBLER
# to fall back to the portable C++ bodies.  See docs/asm-migration-plan.md.
#
# Structure mirrors the repo-root SDL Makefile: explicit CXX_SRCS / CORE_C_SRCS
# lists mapped to objects, compiled by per-extension pattern rules.  The objects
# live in a flat obj/ (keyed by basename — there are no basename collisions across
# the source trees) and the sources are located via vpath.

OUT      = out/RoF
CC       = m68k-amiga-elf-gcc
AS       = m68k-amiga-elf-as
VASM     = vasmm68k_mot
OBJDUMP  = m68k-amiga-elf-objdump
SDKDIR   = $(abspath $(dir $(shell which $(CC)))../m68k-amiga-elf/sys-include)
SUPPORT  = $(abspath $(dir $(shell which $(CC)))../../support)

# Repository root (parent of amiga/) and the unified Amiga source tree.  The code now
# lives under $(REPO)/src/platform/amiga (and .../amiga/framework); this amiga/ directory
# holds only build infrastructure (this Makefile, env.sh, run.sh, assets/, diag scripts).
REPO      = ..
AMIGA_SRC = $(REPO)/src/platform/amiga

$(shell mkdir -p obj out)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# C++ sources: framework hw classes + RoF application + the shared platform layer
# (the abstract Platform base, the C bridge dispatching the transpiled code's
# platform_* calls to the Platform singleton, and the common entry point).
CXX_SRCS := \
    $(AMIGA_SRC)/framework/AmigaHardware.cpp \
    $(AMIGA_SRC)/framework/Bitmap.cpp \
    $(AMIGA_SRC)/framework/CopperList.cpp \
    $(AMIGA_SRC)/framework/Palette.cpp \
    $(AMIGA_SRC)/framework/Sprite.cpp \
    $(AMIGA_SRC)/framework/Util.cpp \
    $(AMIGA_SRC)/DoorsCopperList.cpp \
    $(AMIGA_SRC)/EmptyCopperList.cpp \
    $(AMIGA_SRC)/FlightCopperList.cpp \
    $(AMIGA_SRC)/GCCRuntime.cpp \
    $(AMIGA_SRC)/PlanetCopperList.cpp \
    $(AMIGA_SRC)/PlatformAmiga.cpp \
    $(AMIGA_SRC)/RescueOnFractalus.cpp \
    $(AMIGA_SRC)/StandbyCopperList.cpp \
    $(AMIGA_SRC)/TitleScreenCopperList.cpp \
    $(AMIGA_SRC)/TunnelCopperList.cpp \
    $(AMIGA_SRC)/XexImage.cpp \
    $(AMIGA_SRC)/rof_native_amiga.cpp \
    $(REPO)/src/platform/Platform.cpp \
    $(REPO)/src/platform/platform_cbridge.cpp \
    $(REPO)/src/main.cpp

# C sources: the transpiled 6502 core — cpu/bus emulation + generated 6502->C
# translation (rof_gen) + hand-written native twins (rof_native) + manual stubs
# for self-modifying routines (rof_manual).  --gc-sections drops the unused funcs.
CORE_C_SRCS := \
    $(REPO)/src/cpu/cpu.c \
    $(REPO)/src/gen/rof_gen.c \
    $(REPO)/src/gen/rof_native.c \
    $(REPO)/src/gen/rof_manual.c

# Support runtime (freestanding CRT: program entry, memset…) + incbin'd assets.
# These need bespoke flags, so they keep explicit rules below rather than the
# CXX/CORE pattern rules.
SUPPORT_C_SRCS := $(SUPPORT)/gcc8_c_support.c
ASM_SRCS       := $(SUPPORT)/gcc8_a_support.s $(AMIGA_SRC)/incbin.s

# Framework hand-written m68k asm (Motorola syntax, assembled by vasm -> ELF).  These
# are the dA JoRMaS framework's *Assembler.s routines, vendored + dotted-label-sanitised
# (see framework/UPSTREAM.md).  The matching GCC register-marshalling bridges live under
# `#if defined(ASSEMBLER) && !defined(__SASC)` in the corresponding framework .cpp.
FRAMEWORK_ASM_SRCS := \
    $(AMIGA_SRC)/framework/UtilAssembler.s \
    $(AMIGA_SRC)/framework/AmigaHardwareAssembler.s \
    $(AMIGA_SRC)/framework/BitmapAssembler.s \
    $(AMIGA_SRC)/framework/CopperListAssembler.s

# Hand-written m68k twin of the flight terrain rasterizer (asm-migration-plan Phase 2).
# ON by default; build with `make RASTER_C=1` to fall back to the clean-C oracle
# (terrain_column_rasterize_core_c) — used for the headless render-diff A/B.
ifndef RASTER_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/TerrainRasterizeAssembler.s
EXTRA_DEFINES += -DROF_RASTERIZE_ASM
# `make VERIFY=1` (implies PROBES): on-target differential check of the asm twin vs
# the C oracle each call (rof_native.c), read via amiga/raster_verify.gdb.  The define
# goes to BOTH the C (the diff wrapper) and the raster .s (drops its plain xdef so the
# wrapper owns terrain_column_rasterize_core and the asm is reached as *_asm).
# NO_RASTER_VERIFY=1 keeps the rasterizer as plain asm even under VERIFY (so another
# twin's differential — e.g. subdiv — runs against a fast deterministic rasterizer).
ifdef VERIFY
ifndef NO_RASTER_VERIFY
EXTRA_DEFINES += -DROF_RASTERIZE_VERIFY
obj/TerrainRasterizeAssembler.o : VASMFLAGS += -DROF_RASTERIZE_VERIFY
endif
endif
endif

# Hand-written m68k twin of project_terrain_points (asm-migration-plan Phase 3).
# ON by default; `make PROJECT_C=1` falls back to the clean-C oracle.  Same VERIFY
# mechanism as the rasterizer: the on-target differential vs the C oracle each call
# (rof_native.c, ROF_PROJECT_VERIFY), read via amiga/project_verify.gdb.
ifndef PROJECT_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/ProjectTerrainAssembler.s
EXTRA_DEFINES += -DROF_PROJECT_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_PROJECT_VERIFY
obj/ProjectTerrainAssembler.o : VASMFLAGS += -DROF_PROJECT_VERIFY
endif
endif

# Hand-written m68k twin of terrain_subdivide_column (asm-migration-plan Phase 3).
# ON by default; `make SUBDIV_C=1` falls back to the clean-C oracle.  Same VERIFY
# mechanism; read the on-target differential via amiga/subdiv_verify.gdb.
ifndef SUBDIV_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/TerrainSubdivideAssembler.s
EXTRA_DEFINES += -DROF_SUBDIV_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_SUBDIV_VERIFY
obj/TerrainSubdivideAssembler.o : VASMFLAGS += -DROF_SUBDIV_VERIFY
endif
endif

# Hand-written m68k twin of terrain_frame_setup's two transform loops (Phase 3).
# ON by default; `make TFSETUP_C=1` falls back to the clean-C oracle.  Verify via
# amiga/tfsetup_verify.gdb (not raster-diluted: no rasterizer call in this function).
ifndef TFSETUP_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/TerrainFrameSetupAssembler.s
EXTRA_DEFINES += -DROF_TFSETUP_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_TFSETUP_VERIFY
obj/TerrainFrameSetupAssembler.o : VASMFLAGS += -DROF_TFSETUP_VERIFY
endif
endif

# Hand-written m68k twin of fill_terrain_columns (the one-shot stars/planet field build).
# ON by default; `make FILLTERR_C=1` falls back to the clean-C oracle.  Verify via
# amiga/fillterr_verify.gdb (fires once — fill_terrain_columns is one-shot).
ifndef FILLTERR_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/FillTerrainAssembler.s
EXTRA_DEFINES += -DROF_FILLTERR_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_FILLTERR_VERIFY
obj/FillTerrainAssembler.o : VASMFLAGS += -DROF_FILLTERR_VERIFY
endif
endif

# Objects (flattened into obj/ by basename).
CXX_OBJS       := $(addprefix obj/,$(notdir $(CXX_SRCS:.cpp=.o)))
CORE_C_OBJS    := $(addprefix obj/,$(notdir $(CORE_C_SRCS:.c=.o)))
SUPPORT_C_OBJS := $(addprefix obj/,$(notdir $(SUPPORT_C_SRCS:.c=.o)))
ASM_OBJS       := $(addprefix obj/,$(notdir $(ASM_SRCS:.s=.o)))
FRAMEWORK_ASM_OBJS := $(addprefix obj/,$(notdir $(FRAMEWORK_ASM_SRCS:.s=.o)))

OBJECTS := $(CXX_OBJS) $(CORE_C_OBJS) $(SUPPORT_C_OBJS) $(ASM_OBJS) $(FRAMEWORK_ASM_OBJS)

# Locate sources for the pattern rules.  (gcc8_c_support.c is deliberately NOT on
# vpath %.c — it has its own explicit rule so it builds with CCFLAGS, not CORE_CFLAGS.)
vpath %.cpp $(AMIGA_SRC)/framework $(AMIGA_SRC) $(REPO)/src/platform $(REPO)/src
vpath %.c   $(REPO)/src/cpu $(REPO)/src/gen
vpath %.s   $(AMIGA_SRC)/framework $(AMIGA_SRC)

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
WARN      = -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
# -m68000: target is A500 68000. -msoft-float: no FPU.
# Core C flags: same base but with repo src/ include paths for cpu.h/bus.h/rof_gen.c
CORE_CINCLUDES = -I"$(AMIGA_SRC)/framework/compat-include" \
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
# On the Amiga, bus_write to hardware ($D000-$D7FF) is a no-op for everything except the
# POKEY audio range $D200-$D20F (PlatformAmiga::hwWrite ignores GTIA/ANTIC writes).  Tell
# bus_write to skip the (virtual) platform call for those dead ranges — the per-frame VBI
# alone pokes ~20 GTIA/ANTIC registers that did nothing but cost an indirect call.
EXTRA_DEFINES += -DROF_HW_WRITE_POKEY_ONLY
ifdef PROBES
EXTRA_DEFINES += -DROF_FLIGHT_PROBE -DROF_TDRAW_PROF
endif
# `make PROBES=1 TITLE=1`: force-enter the Title Screen (inject its screen RAM + palette and
# pin rsTitle) instead of auto-launching to flight — a visual test path for the Title Screen
# render before its real attract-mode entry is wired.  Requires PROBES.
ifdef TITLE
EXTRA_DEFINES += -DROF_FORCE_TITLE
endif
CORE_CFLAGS   = -g -MP -MMD -m68000 -msoft-float $(OPT) -nostdlib \
                -fno-tree-loop-distribution -ffunction-sections -fdata-sections \
                -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
                -Wno-implicit-function-declaration -Wno-int-conversion \
                -DROF_PLATFORM_AMIGA \
                $(CORE_CINCLUDES) $(EXTRA_DEFINES)
CCFLAGS   = -g -MP -MMD -m68000 -msoft-float $(OPT) -nostdlib \
            -fno-tree-loop-distribution -ffunction-sections -fdata-sections \
            $(WARN) $(EXTRA_DEFINES)
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
# vasm (Motorola syntax) for the framework *Assembler.s.  -m68010: the only >68000
# instruction is `movec vbr,d0` in getVBR (the C++ getVBR already emits movec via raw
# bytes through Supervisor(), so this is behaviour-neutral on the A500 68000 target).
VASMFLAGS = -m68010 -Felf -quiet -x -no-opt -nowarn=62
LDFLAGS   = -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(OUT).map

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------
all: $(OUT).exe

$(OUT).exe: $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(OUT).exe

$(OUT).elf: $(OBJECTS)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(OBJECTS) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins -S $@ > $(OUT).s 2>/dev/null || true

# C++ (framework + RoF app + shared platform layer): all built with CPPFLAGS.
obj/%.o : %.cpp
	$(info C++  $<)
	@$(CC) $(CPPFLAGS) -c -o $@ "$(abspath $<)"

# Transpiled 6502 core C: built with CORE_CFLAGS (repo src/ include paths).
# rof_native.c (hand-written native twins) additionally gets NATIVE_OPT.
obj/%.o : %.c
	$(info C    $< [core])
	@$(CC) $(CORE_CFLAGS) -c -o $@ "$(abspath $<)"
obj/rof_native.o : CORE_CFLAGS += $(NATIVE_OPT)

# Support runtime: gcc8_c_support.c needs CCFLAGS (no core include paths), so it
# gets an explicit rule rather than the obj/%.o:%.c pattern above.
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

# Framework hand-written asm (vasm, Motorola syntax -> ELF).  Static pattern rule so
# it doesn't collide with the gas-assembled gcc8_a_support.s / incbin.s (explicit rules).
$(FRAMEWORK_ASM_OBJS) : obj/%.o : %.s
	$(info VASM $<)
	@$(VASM) $(VASMFLAGS) -o $@ "$(abspath $<)"

clean:
	@$(RM) -r obj/ out/

-include $(CXX_OBJS:.o=.d) $(CORE_C_OBJS:.o=.d) $(SUPPORT_C_OBJS:.o=.d)
