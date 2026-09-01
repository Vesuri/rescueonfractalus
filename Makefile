## Rescue on Fractalus! — build system
## C and C++ sources are compiled separately, then linked with SDL2 via pkg-config.

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

# `make validate FN=flight_control_integrate FCIBASE=1` — the HOST arm of the Amiga mem[] base
# fold (rof_native.c ROF_FCI_BASE, docs/asm-migration-plan.md §Phase 12).  The fold itself is
# Amiga-only, so make validate cannot normally see it at all; this compiles the SAME source
# transformation here (portable "r" constraint instead of m68k's "a") so the oracle can prove the
# macro rescan is byte-identical.  ⚠ TEST ONLY — it casts volatile off mem[], which is sound in
# the single-threaded validate harness and NOT in the threaded SDL game.
ifdef FCIBASE
  EXTRA_DEFINES += -DROF_FCI_BASE_HOSTTEST
endif

# `make validate FN=terrain_draw_frame MEMVIEW=1` — the HOST arm of the Amiga local-mem-view
# de-volatiling (rof_native.c ROF_MEM_VIEW).  Same idea and the same ⚠ as FCIBASE above: the
# change only exists on the Amiga, so this compiles its shape here to prove it byte-identical.
ifdef MEMVIEW
  EXTRA_DEFINES += -DROF_MEM_VIEW_HOSTTEST
endif

# `make validate MEMBASE=1` — the HOST arm of the GENERALISED mem[] base fold (rof_native.c
# ROF_MEMBASE, applied to a dozen flight routines).  FCIBASE covers only flight_control_integrate;
# this one turns the fold on everywhere it is used, which is what proves the `#define mem`
# rescan reached every access in each body.  Same ⚠ as above — TEST ONLY.
ifdef MEMBASE
  EXTRA_DEFINES += -DROF_MEMBASE_HOSTTEST
endif

BUILD_FLAGS := $(OPT) $(EXTRA_DEFINES)

CFLAGS   := -std=c11   -g $(OPT) -Wall -Wno-unused-label -fsigned-char \
             $(EXTRA_DEFINES) \
             -Isrc -Isrc/cpu -Isrc/platform -Isrc/gen
CXXFLAGS := -std=c++11 -g $(OPT) -Wall -Wno-reorder -fsigned-char \
             -Isrc -Isrc/cpu -Isrc/platform -Isrc/gen \
             $(SDL_CFLAGS) $(PNG_CFLAGS)
LDFLAGS  := $(SDL_LDFLAGS) $(PNG_LDFLAGS)

# C sources (generated 6502 transliteration + CPU model)
C_SRCS := \
    src/cpu/cpu.c \
    src/rof_boot.c \
    src/rof_hiscore.c \
    src/rof_logo.c \
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

# ⚠⚠ THE FLAG GUARD — do not remove, and do not "simplify" it into a stamp file.
# FCIBASE/MEMVIEW/MEMBASE/RELEASE change only CFLAGS, and `%.o: %.c` has no dependency on them, so
# `make validate MEMBASE=1` straight after a plain `make validate` RELINKED stale objects and re-ran
# the whole suite against the code with the fold switched OFF — a vacuous green indistinguishable
# from a real pass (caught 2026-08-14: rof_native.o was 15 minutes older than the validate_native it
# was linked into).  Same class as the Amiga Makefile's `make clean` before PROBES=1 (CLAUDE.md).
#
# This runs at PARSE time, before make builds its dependency graph: if the recorded flag set differs
# it deletes the affected objects, so make simply sees them as missing.  No timestamps, no stamp
# prerequisite, no pattern-rule interaction.
# ⚠ Two tidier-looking versions were built and BOTH silently failed on 3 of 10 flag transitions,
# because Apple ships GNU Make 3.81 (2006): (a) one fixed stamp file compared by CONTENTS under a
# FORCE rule — make caches its stat of the stamp, so rewriting it in its own recipe does not reliably
# mark dependents out of date; (b) the flag set hashed into the stamp's FILENAME, as a prerequisite
# of `%.o` and then of the concrete object list — still stale, the failing transitions merely moved.
# If you change this, re-run the 10-step transition matrix (docs/transpiler.md §flag guard); a
# half-working guard is worse than none, because it makes a vacuous green look verified.
FLAGS_FILE := build/.flags
FLAG_GUARD := $(shell mkdir -p build; \
    if [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" != "$(BUILD_FLAGS)" ]; then \
        rm -f $(OBJS) tools/validate_native.o; \
        printf '%s' "$(BUILD_FLAGS)" > $(FLAGS_FILE); \
        echo "flags-changed"; \
    fi)
ifeq ($(FLAG_GUARD),flags-changed)
  $(info FLAGS [$(BUILD_FLAGS)] changed — objects dropped, rebuilding)
endif

.PHONY: all clean gen validate hostproof

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

# ---------------------------------------------------------------------------
# `make hostproof` — the host-side equivalence proofs (tools/*_test.c)
#
# WHY THIS TARGET EXISTS.  `make validate` diffs a native twin against its 6502
# oracle through the `mem[]` contract, so it only reaches code that BOTH backends
# link (rof_native.c).  It is structurally blind to:
#   * Amiga-only code (`rof_native_amiga.cpp`, `RescueOnFractalus.cpp` — ~516
#     mem[]-touching lines), which has no 6502 oracle to diff against;
#   * pure REORDERINGS and table-folds inside a routine, whose 6502 oracle was
#     shed long ago;
#   * transformations that only exist under an `#ifdef ROF_PLATFORM_AMIGA`.
# For those, the proof is: compile the OLD and NEW bodies side by side on the host
# and diff them over the whole input domain (or a large randomized sample).  That
# is seconds of work and it is the only check that reaches this code at all.
# These proofs were hand-compiled one-offs for years; this target makes them a habit.
#
# `make hostproof` runs all of them; `make hostproof FN=<substr>` runs the subset
# whose name contains <substr>.  Each proof exits non-zero on any mismatch.
#
# ⚠ THE CAVEAT THAT MATTERS: each proof carries a VERBATIM SNAPSHOT of the routine
# it was written against.  So a green run proves "the transformation is sound", NOT
# "the shipping source still matches the snapshot" — the two drift apart silently as
# the real routine is edited further.  Re-read the snapshot against its source before
# leaning on an old proof, and prefer adding a NEW proof to amending an old one.
#
# ⚠ NOT included: tools/xorshift_triple_test.c.  It is a design SEARCH (which shift
# triple is full-period and cheapest on the 68000), not an equivalence proof — it
# does not check the triple the code actually ships.
HOSTPROOF_DIR    := build/hostproof
HOSTPROOF_CFLAGS := -std=c11 -O2 -Wall -fsigned-char -Isrc -Isrc/cpu -Isrc/gen

# Self-contained proofs: no arguments, exit non-zero on mismatch.
HOSTPROOF_SELF := \
    alien_mirror_test \
    dot_table_test \
    tunnel_batch_test \
    ras_fused_midpoint_test \
    hiscore_block_test \
    pokey_divider_test \
    poly_dist_test \
    ras_restructure_test \
    terr_blend_table_test \
    terr_blend_test
# Needs the two boot-image assets built first (see the rules below).
HOSTPROOF_ASSET := test_xex_sparse

# The same-length zeroed xex and the sparse boot image, regenerated here rather than
# borrowed from amiga/assets so the proof never depends on the Amiga build's state.
# Both derive from the disassembly, hence the listing.txt dependency.
$(HOSTPROOF_DIR)/zeroed.xex: rof.xex disasm/listing.txt tools/xex_deadset.py | $(HOSTPROOF_DIR)
	$(info GEN  $@)
	@python3 -c "import sys; sys.path.insert(0, 'tools'); import xex_deadset; \
	             raw, (mask, z) = xex_deadset.load(); open('$@', 'wb').write(z)"

$(HOSTPROOF_DIR)/sparse.bin: rof.xex disasm/listing.txt tools/make_xex_sparse.py \
                             tools/xex_deadset.py | $(HOSTPROOF_DIR)
	$(info GEN  $@)
	@python3 tools/make_xex_sparse.py $@ > $(HOSTPROOF_DIR)/sparse.log

hostproof: $(HOSTPROOF_DIR)/zeroed.xex $(HOSTPROOF_DIR)/sparse.bin | $(HOSTPROOF_DIR)
	@fail=0; ran=0; \
	for t in $(HOSTPROOF_SELF) $(HOSTPROOF_ASSET); do \
	  case "$$t" in *$(FN)*) ;; *) continue ;; esac; \
	  case "$$t" in \
	    $(HOSTPROOF_ASSET)) a="$(HOSTPROOF_DIR)/zeroed.xex $(HOSTPROOF_DIR)/sparse.bin" ;; \
	    *)                  a="" ;; \
	  esac; \
	  ran=$$((ran+1)); \
	  if ! $(CC) $(HOSTPROOF_CFLAGS) -o $(HOSTPROOF_DIR)/$$t tools/$$t.c; then \
	    printf '%-24s COMPILE FAIL\n' "$$t"; fail=$$((fail+1)); continue; \
	  fi; \
	  if out=$$($(HOSTPROOF_DIR)/$$t $$a 2>&1); then \
	    printf '%-24s PASS  %s\n' "$$t" "$$(printf '%s' "$$out" | tail -1)"; \
	  else \
	    printf '%-24s FAIL\n' "$$t"; printf '%s\n' "$$out" | sed 's/^/    /'; \
	    fail=$$((fail+1)); \
	  fi; \
	done; \
	if [ $$ran -eq 0 ]; then \
	  echo "hostproof: no proof matches FN='$(FN)'"; exit 1; \
	fi; \
	echo "hostproof: $$ran run, $$fail failed"; \
	[ $$fail -eq 0 ]

$(HOSTPROOF_DIR):
	mkdir -p $(HOSTPROOF_DIR)

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
	rm -f $(FLAGS_FILE)
	rm -rf $(HOSTPROOF_DIR)
