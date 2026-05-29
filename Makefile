CC      = clang
CFLAGS  = -std=c11 -O2 -Wall -Wno-unused-label \
          -Isrc -Isrc/cpu -Isrc/platform -Isrc/gen

SRCS    = src/main.c \
          src/cpu/cpu.c \
          src/platform/stub/stub.c \
          src/gen/rof_gen.c \
          src/gen/rof_manual.c

TARGET  = build/rof_stub

.PHONY: all clean gen

all: $(TARGET)

$(TARGET): $(SRCS) src/gen/rof_decl.h | build
	$(CC) $(CFLAGS) $(SRCS) -o $@

build:
	mkdir -p build

# Re-run transpiler (requires listing.txt to be up to date)
gen:
	python3 tools/transpile.py

clean:
	rm -rf build
