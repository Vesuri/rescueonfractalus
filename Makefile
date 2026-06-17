## Rescue on Fractalus! — build system
## Mirrors PETSCIIRobots-SDL/Makefile structure: C and C++ sources compiled
## separately, linked with SDL2 via pkg-config.

CC      := clang
CXX     := clang++

SDL_CFLAGS  := $(shell pkg-config --cflags sdl2)
SDL_LDFLAGS := $(shell pkg-config --libs   sdl2)
PNG_CFLAGS  := $(shell pkg-config --cflags libpng)
PNG_LDFLAGS := $(shell pkg-config --libs   libpng)

# Build mode: debug (default) or release.
# Usage: make          → debug (-O0 -g, full lldb variable support)
#        make RELEASE=1 → release (-O2 -g)
ifdef RELEASE
  OPT := -O2
else
  OPT := -O0
endif

CFLAGS   := -std=c11   -g $(OPT) -Wall -Wno-unused-label -fsigned-char \
             -Isrc -Isrc/cpu -Isrc/platform -Isrc/gen
CXXFLAGS := -std=c++11 -g $(OPT) -Wall -Wno-reorder -fsigned-char \
             -Isrc -Isrc/cpu -Isrc/platform -Isrc/gen \
             $(SDL_CFLAGS) $(PNG_CFLAGS)
LDFLAGS  := $(SDL_LDFLAGS) $(PNG_LDFLAGS)

# C sources (generated 6502 transliteration + CPU model)
C_SRCS := \
    src/cpu/cpu.c \
    src/gen/rof_gen.c \
    src/gen/rof_manual.c \
    src/gen/rof_vbi.c \
    src/gen/rof_native.c

# C++ sources (platform layer + entry point)
CXX_SRCS := \
    src/platform/Platform.cpp \
    src/platform/sdl/PlatformSDL.cpp \
    src/platform/platform_cbridge.cpp \
    src/main.cpp

C_OBJS   := $(C_SRCS:.c=.o)
CXX_OBJS := $(CXX_SRCS:.cpp=.o)
OBJS     := $(C_OBJS) $(CXX_OBJS)
TARGET   := build/rof

.PHONY: all clean gen validate

all: $(TARGET)

$(TARGET): $(OBJS) | build
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# Native-reimplementation validation harness.
# Links the full app object graph minus main.o (for the symbol environment),
# plus the harness, with its own main(). SDL is linked but never initialized.
VALIDATE_OBJS := $(filter-out src/main.o,$(OBJS)) tools/validate_native.o
# `make validate` runs the whole suite; `make validate FN="name ..."` runs only the
# tests whose name contains one of the given substrings (e.g. FN=mul_u8, FN=terrain).
validate: $(VALIDATE_OBJS) | build
	$(CXX) $(CXXFLAGS) -o build/validate_native $(VALIDATE_OBJS) $(LDFLAGS)
	./build/validate_native $(FN)

build:
	mkdir -p build

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Regenerate the transliterated C from the Ghidra listing.
# Requires: disasm/listing.txt to be current (run via Ghidra headless).
gen:
	python3 tools/transpile.py

clean:
	rm -f $(OBJS) $(TARGET) tools/validate_native.o build/validate_native
