## Rescue on Fractalus! — build system
## Mirrors PETSCIIRobots-SDL/Makefile structure: C and C++ sources compiled
## separately, linked with SDL2 via pkg-config.

CC      := clang
CXX     := clang++

SDL_CFLAGS  := $(shell pkg-config --cflags sdl2)
SDL_LDFLAGS := $(shell pkg-config --libs   sdl2)

CFLAGS   := -std=c11   -g -O2 -Wall -Wno-unused-label -fsigned-char \
             -Isrc -Isrc/cpu -Isrc/platform -Isrc/gen
CXXFLAGS := -std=c++11 -g -O2 -Wall -Wno-reorder -fsigned-char \
             -Isrc -Isrc/cpu -Isrc/platform -Isrc/gen \
             $(SDL_CFLAGS)
LDFLAGS  := $(SDL_LDFLAGS)

# C sources (generated 6502 transliteration + CPU model)
C_SRCS := \
    src/cpu/cpu.c \
    src/gen/rof_gen.c \
    src/gen/rof_manual.c \
    src/gen/rof_vbi.c

# C++ sources (platform layer + entry point)
CXX_SRCS := \
    src/platform/Platform.cpp \
    src/platform/PlatformSDL.cpp \
    src/platform/platform_cbridge.cpp \
    src/main.cpp

C_OBJS   := $(C_SRCS:.c=.o)
CXX_OBJS := $(CXX_SRCS:.cpp=.o)
OBJS     := $(C_OBJS) $(CXX_OBJS)
TARGET   := build/rof

.PHONY: all clean gen

all: $(TARGET)

$(TARGET): $(OBJS) | build
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

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
	rm -f $(OBJS) $(TARGET)
