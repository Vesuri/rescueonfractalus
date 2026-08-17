# Rescue on Fractalus — Amiga cross-build (m68k-amiga-elf-gcc)
#
# Usage (from amiga/ with toolchain on PATH via `. env.sh`):
#   make          — build out/RoF
#   make clean    — remove obj/ and out/
#   ./run.sh      — boot in FS-UAE (OCS A500)
#   ./debug.sh    — source-level debug via FS-UAE GDB stub
#
# The framework's hand-written m68k *Assembler.s routines are vendored
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
# The AmigaOS executable itself is extension-less (`out/RoF`); OUT is only the stem
# for the build by-products that sit next to it (.elf/.map/.s).
EXE      = $(OUT)
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
    $(AMIGA_SRC)/Gtia9CopperList.cpp \
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
    $(REPO)/src/rof_boot.c \
    $(REPO)/src/rof_logo.c \
    $(REPO)/src/gen/rof_gen.c \
    $(REPO)/src/gen/rof_native.c \
    $(REPO)/src/gen/rof_manual.c

# Support runtime (freestanding CRT: program entry, memset…) + incbin'd assets + the
# AmigaOS $VER: string.  These need bespoke flags, so they keep explicit rules below
# rather than the CXX/CORE pattern rules.
SUPPORT_C_SRCS := $(SUPPORT)/gcc8_c_support.c
ASM_SRCS       := $(SUPPORT)/gcc8_a_support.s $(AMIGA_SRC)/incbin.s \
                  $(AMIGA_SRC)/version.s

# Framework hand-written m68k asm (Motorola syntax, assembled by vasm -> ELF).  These are
# the framework's *Assembler.s routines, vendored with
# their dotted-in-middle local labels sanitised — `sed -E 's/([A-Za-z0-9])\.([A-Za-z_][A-Za-z0-9_]+)/\1_\2/g'`,
# e.g. cl.cpu->cl_cpu — because vasm mot reads a leading `.` as a local-label marker.
# Assembled -m68010, not -m68000, for ONE instruction: `movec vbr,d0` in Util's getVBR.
# The matching GCC register-marshalling bridges live under
# `#if defined(ASSEMBLER) && !defined(__SASC)` in the corresponding framework .cpp;
# AmigaHardware.h aliases the blitter-queue statics to their SAS/C mangled names
# (ROF_SASC_ALIAS) so the asm's xrefs resolve.  Palette/Sprite have no asm twin.
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
# The subdivide asm hands the running cursor to the rasterizer through a PRIVATE register ABI
# (terrain_column_rasterize_span) instead of mem[$82/$84/$86].  That entry only exists when the
# rasterizer asm is built, and must NOT be used under the raster differential (there every call
# has to go through the C dispatcher or raster_verify.gdb sees none of them).  Cleared below in
# that case; the subdivide .s then keeps its old stack+mem[] call.
RASTER_SPAN_ABI := 1
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
RASTER_SPAN_ABI :=
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
# terrain_draw_objects reaches the subdivide through its OBJECT-INDEXED entry
# (terrain_subdivide_column_obj), which loads the running span from the object arrays instead of
# having the caller copy it through mem[$82-$86].  That entry only exists in the asm; under
# VERIFY the C wrapper owns the name and the asm is reached as *_obj_asm, so this define is
# harmless there (the #if in rof_native.c prefers the VERIFY branch).
EXTRA_DEFINES += -DROF_SUBDIV_OBJ_ABI
ifdef VERIFY
EXTRA_DEFINES += -DROF_SUBDIV_VERIFY
obj/TerrainSubdivideAssembler.o : VASMFLAGS += -DROF_SUBDIV_VERIFY
endif
# The exit residue ($82-$86 span, $8D-$91 midpoint) is published ONCE PER DRAW PASS instead of
# on each of the ~68 subdivide calls: the twin parks it in g_sdResidue with a single MOVEM.W
# (28 cycles against 100 for the byte-swapped span flush, and 28 against 132 for the mid), and
# terrain_draw_objects seeds the scratch before its loop and writes it through afterwards.
# Sound because NOTHING inside that loop, and nothing in the flight VBI, reads those cells --
# reader survey over the linked image, see the DEFER note in TerrainSubdivideAssembler.s.
# `make SUBDIV_EAGER=1` restores the per-call flush (for an A/B or if a new reader appears).
ifndef SUBDIV_EAGER
EXTRA_DEFINES += -DROF_SUBDIV_DEFER_RESIDUE
obj/TerrainSubdivideAssembler.o : VASMFLAGS += -DROF_SUBDIV_DEFER_RESIDUE
endif
# Use the rasterizer's register-ABI entry when it exists (see RASTER_SPAN_ABI above).  To put
# the fast path under a differential, build `make VERIFY=1 NO_RASTER_VERIFY=1` — that keeps
# this on while subdiv_verify.gdb compares the asm subdivide (register handoff) against the C
# oracle (mem[] handoff) call-for-call.
ifdef RASTER_SPAN_ABI
obj/TerrainSubdivideAssembler.o : VASMFLAGS += -DROF_RASTER_SPAN_ABI
# ...and once rasterEntryDepth is dead (that is exactly what RASTER_SPAN_ABI means), the object
# entry's only LIVE argument is obj0: startDepth is the literal 0 at its one call site.  So the
# shipping ABI passes just obj0 — the caller drops a `move.l`+`clr.l -(sp)` pair (32 cycles) and
# the callee drops the arg decode (32).  Tied to RASTER_SPAN_ABI because a plain `make VERIFY=1`
# build routes the rasterize through the C dispatcher, where rasterEntryDepth is still passed.
EXTRA_DEFINES += -DROF_SUBDIV_OBJ1ARG
obj/TerrainSubdivideAssembler.o : VASMFLAGS += -DROF_SUBDIV_OBJ1ARG
# ...and the same argument one level up: terrain_draw_objects used to copy obj1's projected vector
# into SubPt slot 0 on EVERY visible pair (5 memory-to-memory MOVE.B, 20 cycles each, 68.1 pairs
# an iteration = 1.2% of wall) purely so the twin could read it back.  Under OBJ_SEED0 the twin
# takes obj1 as a second argument, its depth-0 arm reads the far endpoint out of the object arrays
# directly, and slot 0 is materialised only when the recursion descends past depth 0 (~0.21 pushes
# a call) plus once per pass by the caller.  Sound because NOTHING else in the linked image reads
# those five cells — reader survey in docs/flight-perf-log.md §24.2 and the SEED note in the .s.
# Tied to OBJ1ARG because the depth-0 arm assumes startDepth == 0, which is what OBJ1ARG means.
# `make SUBDIV_EAGER0=1` restores the per-pair seed (for an A/B or if a new reader appears).
ifndef SUBDIV_EAGER0
EXTRA_DEFINES += -DROF_SUBDIV_OBJ_SEED0
obj/TerrainSubdivideAssembler.o : VASMFLAGS += -DROF_SUBDIV_OBJ_SEED0
# `make SEED0_VERIFY=1 PROBES=1` + amiga/seed0_verify.gdb: shadows obj1's vector at each visible
# pair and checks the once-per-pass publish against it.  The ONLY instrument that can see a wrong
# publish — nothing reads those cells, so no differential and no cross-build run can.
ifdef SEED0_VERIFY
EXTRA_DEFINES += -DROF_SEED0_VERIFY
endif
endif
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

# Hand-written m68k twin of build_view_transform_matrix (four signed_mul_8x16 -> mulu.w each;
# retires the bit-serial-multiply TODO).  ON by default; `make BUILDVIEW_C=1` falls back to the
# clean-C oracle.  Verify via amiga/buildview_verify.gdb (setup path, not raster-diluted).
ifndef BUILDVIEW_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/BuildViewAssembler.s
EXTRA_DEFINES += -DROF_BUILDVIEW_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_BUILDVIEW_VERIFY
obj/BuildViewAssembler.o : VASMFLAGS += -DROF_BUILDVIEW_VERIFY
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

# Hand-written m68k twin of fill_vertical_span_core (the standby->doors door-frame draw's
# dominant cost).  ON by default; `make FRAMEDRAW_C=1` falls back to the clean-C oracle.
# Verify via amiga/framedraw_verify.gdb (in-process differential over the $1000 door field).
ifndef FRAMEDRAW_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/FrameDrawAssembler.s
EXTRA_DEFINES += -DROF_FRAMEDRAW_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_FRAMEDRAW_VERIFY
obj/FrameDrawAssembler.o : VASMFLAGS += -DROF_FRAMEDRAW_VERIFY
endif
endif

# Hand-written m68k twin of the g_mulTable build (the one-shot 64KB mul_u8 lookup-table fill,
# run once at startup via rof_mul_table_init so it's NEVER built lazily inside the flight VBI
# ISR — that first-flight build froze the display ~3.6s at flight entry).  Uses mul_u8's
# symmetry (triangle + mirror) so it's ~2x fewer bit-serial passes than the C.  ON by default;
# `make MULTABLE_C=1` falls back to the clean-C oracle.  Verify (one-shot, memcmp vs the full
# C oracle -> g_mulTableMismatch) via `make VERIFY=1` + amiga/multable_verify.gdb.
ifndef MULTABLE_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/MulTableAssembler.s
EXTRA_DEFINES += -DROF_MULTABLE_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_MULTABLE_VERIFY
endif
endif

# Hand-written m68k twin of the SFX voice-priority MIXER chain (sfx_reorder_voice_slot $5614
# plus its three leaves $5673/$568A/$56AF, all inlined into one routine).  Not a rendering
# path: it is the biggest single item inside the 50 Hz flight VBI, which fires 50x/s regardless
# of frame rate, so it taxes ALL wall clock (measured 22.4 t/firing = ~7% of it in combat).
# ON by default; `make SFXMIX_C=1` falls back to the clean-C twin.  Same VERIFY
# pattern as the others: `make VERIFY=1 PROBES=1` runs the asm + the C twin back-to-back on
# identical state each call (rof_native.c, ROF_SFXMIX_VERIFY), read via amiga/sfxmix_verify.gdb.
ifndef SFXMIX_C
FRAMEWORK_ASM_SRCS += $(AMIGA_SRC)/SfxMixerAssembler.s
EXTRA_DEFINES += -DROF_SFXMIX_ASM
ifdef VERIFY
EXTRA_DEFINES += -DROF_SFXMIX_VERIFY
obj/SfxMixerAssembler.o : VASMFLAGS += -DROF_SFXMIX_VERIFY
endif
# `make FUZZ=1` — ONE-SHOT on-target fuzz of the asm twin vs its C oracle at boot (20000 random
# mem[] states, every path, incl. the ones flight rarely reaches), read via amiga/sfxmix_fuzz.gdb.
# This is the check VERIFY cannot make: sfxmix_verify only ever sees states real flight produces,
# and `make validate` can only exercise the C twin on the host.  Costs a few seconds at startup.
ifdef FUZZ
EXTRA_DEFINES += -DROF_SFXMIX_FUZZ
endif
endif

# A/B baseline: `make BLEND_LOOP=1` restores terr_blend's ORIGINAL 8-iteration bit-serial loop
# in place of the two 4 KB nibble tables (rof_native.c), so the win can be re-measured in one
# tree: GDBSCRIPT=isr_ab.gdb on this build vs the default.  The metric is the WHOLE-VBI
# t/firing, which is a legitimate cross-build number because the ISR fires 50x/s regardless of
# frame rate.  Both forms are byte-identical (tools/terr_blend_table_test.c, all 2^24 inputs).
ifdef BLEND_LOOP
EXTRA_DEFINES += -DROF_BLEND_LOOP
endif

# Correctness aid: `make BAND_VERIFY=1 PROBES=1` + amiga/band_verify.gdb — in-process differential
# for the CACHED windscreen-band composite and the box-narrowed object overlay.  Rendering cannot be
# judged from a headless run (the remote debugger greys the display), so instead the frame is
# composited BOTH ways and byte-compared (g_bandMismatch must be 0), and the object scratch is swept
# for any nonzero byte left outside the tracked dirty box (g_objLeak must be 0).
ifdef BAND_VERIFY
EXTRA_DEFINES += -DROF_BAND_VERIFY
endif


# Correctness aid: `make SCAN_VERIFY=1 PROBES=1` + amiga/scan_verify.gdb — in-process differential
# for the long-striding PMG run scans (pmgScanFirst / pmgScanBounds) that the scope-P3, viewport-P3
# and scanner-dot sprite mirrors use to locate their object.  Those are Amiga-only render mirrors,
# so `make validate` cannot see them; instead every call re-derives the run with the ORIGINAL
# per-byte loop and compares (g_scanBad must be 0, g_scanHit > 0 or the run never had an object to
# find and the test proved nothing).
ifdef SCAN_VERIFY
EXTRA_DEFINES += -DROF_SCAN_VERIFY
endif

# Correctness aid: `make CK_VERIFY=1 PROBES=1` + amiga/ck_verify.gdb — coverage check for the
# PER-CELL cockpit registries (lock-on strip $3491-$3497, digit blocks).  Those decoders now
# re-render only the cells a writer flagged, so the risk is not arithmetic but a MISSED write
# leaving a stale cell — which no in-process arithmetic differential can see.  Instead, right
# after the targeted decode, the whole group is decoded again and the destination bytes are
# compared: if the targeted decode was complete, the full decode is a no-op (g_ckVerBad must be 0).
# The source cells are written by the VBI ISR, so the full decode runs TWICE and any call whose two
# full passes disagree is counted as `raced` and discarded (the sandwich rule in
# flight-measurement-rules) rather than reported as a mismatch.
ifdef CK_VERIFY
EXTRA_DEFINES += -DROF_CK_VERIFY
endif

# Correctness aid: `make SHOT_VERIFY=1 PROBES=1` + amiga/shot_verify.gdb — in-process differential
# for buildShotSprite's strip extent.  It now READS the extent from mem[$2865]/mem[$2866] (which
# build_player2_sprite $8C58 maintains so it can erase the strip next frame) instead of re-deriving
# it with a 94-byte volatile scan.  That is an Amiga-only render mirror, so `make validate` cannot
# check it — this runs the old scan alongside the new derivation every call and compares
# (g_svBad must stay 0).  Off by default: it reinstates exactly the scan the change removed.
ifdef SHOT_VERIFY
EXTRA_DEFINES += -DROF_SHOT_VERIFY
endif

# A/B baseline: `make SHOT_SCAN=1` restores buildShotSprite's ORIGINAL body — the 94-byte volatile
# scan for the strip extent plus the unconditional 128-byte sprite clear — so the win can be
# re-measured in one tree (GDBSCRIPT=isr_ab.gdb on this build vs the default; the metric is the
# WHOLE-VBI t/firing, which is a legitimate cross-build number because the ISR fires 50x/s
# regardless of frame rate).  Do not combine with SHOT_VERIFY.
ifdef SHOT_SCAN
EXTRA_DEFINES += -DROF_SHOT_SCAN
endif

# Benchmark determinism: `make FIXED_RNG=1` re-pins the POKEY LFSR ($D20A) at two points —
# immediately before the fresh-start level seeding (rof_native.c intro_random_setup) and on the
# flight rising edge (RescueOnFractalus.cpp deriveRenderSignals).  Our $D20A is read-clocked
# from a fixed power-on seed, not free-running like the Atari's, so WHICH LEVEL you get is
# decided by how many RANDOM reads the attract sequence happened to make first.  Any code change
# shifts the free-running main loop against the real-time 50Hz VBI, shifts that read count, and
# hands the next build a different level — the reason cross-build beam-tick numbers have never
# been comparable and only the in-process differential was trusted.  With this on, every build
# flies the same level.  OFF by default: it deliberately removes real gameplay variety, so never
# judge RENDERING or gameplay from a FIXED_RNG build.
ifdef FIXED_RNG
EXTRA_DEFINES += -DROF_FIXED_RNG
endif

# `make MEMNVC=1` — A/B fallback: keep the `volatile` qualifier on the 6502 RAM image mem[].
# The Amiga build drops it by DEFAULT (src/cpu/cpu.h), which is what lets GCC fold
# `mem[CONST + var]` into a base pointer + 16-bit displacement (12 cycles) instead of the
# volatile form's move/add-immediate/indexed-load chain (34 cycles), and lets a cell live in a
# register across statements.  volatile is still the default OFF-Amiga: the SDL host runs the
# VBI on a real audio thread.  See the comment in src/cpu/cpu.h.
ifndef MEMNVC
EXTRA_DEFINES += -DROF_MEM_NONVOLATILE
endif

# A/B fallback: `make VERTB_SERVER=1` goes back to hanging the game VBI off exec's VERTB
# server CHAIN (AddIntServer) instead of taking the whole VERTB IntVector over.  The default
# (takeover) drops graphics.library / gameport.device / timer.device off the vblank, which
# measured ~780us per 20ms frame = ~3.9% of all wall clock (amiga/irq_probe.gdb, g_irqLat*).
# Use this flag if an interrupt-delivery regression needs bisecting against the old behaviour.
ifdef VERTB_SERVER
EXTRA_DEFINES += -DROF_VERTB_SERVER
endif

# A/B fallback: `make BLIT_IRQ=1` goes back to ARMING the blit-done interrupt after every blit
# (INTENA bit 6), as the framework originally did for its blit-done queue ISR.  This port has no
# such ISR — blitterWait() polls DMACONR BLTBUSY and blitterDrain() spin-drains the queue — so by
# default blit-done stays masked: every armed one was a level-3 autovector dispatch into
# graphics.library's queue handler ($F901C0) doing nothing for us, measured at 6 per flight
# iteration x ~52us (amiga/int_probe.gdb).  Needs to reach the vasm side too (BLIT_IRQ_ARM in
# AmigaHardwareAssembler.s is the copy that runs while ASSEMBLER is on).
ifdef BLIT_IRQ
EXTRA_DEFINES  += -DROF_BLIT_IRQ
EXTRA_VASMDEFS += -DROF_BLIT_IRQ
endif

# Experiment, OFF by default: `make PORTS_TAKEOVER=1` takes over the level-2 PORTS IntVector
# (our own CIA-A ICR read + keyboard dispatch) instead of reaching the keyboard through
# ciaa.resource's ICR dispatcher on exec's PORTS server chain.  That chain measures ~11
# firings/s x ~250us = ~0.3% of wall clock in flight with our own handler running ZERO times
# (amiga/int_probe.gdb) — but it is ciaa.resource + FS-UAE's "UAE fs" host-filesystem/clipboard
# trap server, i.e. an emulator artifact that a bare A500 does not have, AND starving that trap
# server makes FS-UAE reset the machine (bisected: session reboots before flight, 2/2 runs).
# So: kept for the record, not enabled.  See the portsHandler comment in PlatformAmiga.cpp.
ifdef PORTS_TAKEOVER
EXTRA_DEFINES += -DROF_PORTS_TAKEOVER
endif

# Debug test aid: `make FORCE_ALIEN=1` forces EVERY rescued figure to be the alien
# jump-scare (pilot_render marker override, rof_native.c g_forceAlienRescue).  For testing
# the alien-on-windshield animation without waiting for the level-scaled random roll.  OFF
# by default; can also be toggled at runtime by poking g_forceAlienRescue=1.
ifdef FORCE_ALIEN
EXTRA_DEFINES += -DROF_FORCE_ALIEN_RESCUE
endif

# Debug test aid: `make FORCE_AIRLOCK=1` auto-opens the airlock at the phase-4 knock
# (rof_native.c g_forceAirlockOpen) so a HEADLESS run reaches the alien-attack reveal
# without an interactive A-key press.  Combine with FORCE_ALIEN=1 to run the whole
# jump-scare test headlessly (FORCE_ALIEN designates the alien; FORCE_AIRLOCK opens the
# airlock that gates the reveal).  OFF by default; also runtime-pokable.
ifdef FORCE_AIRLOCK
EXTRA_DEFINES += -DROF_FORCE_AIRLOCK_OPEN
endif

# Profiling aid: `make AUTO_FIRE=1` (implies PROBES) holds the trigger down in flight
# (PlatformAmiga.cpp ROF_AUTO_FIRE) so the player laser fires continuously — used to
# profile the viewport-PMG sprite path (shot/explosion build) under load.  OFF by default.
ifdef AUTO_FIRE
EXTRA_DEFINES += -DROF_AUTO_FIRE
endif

# `make FLUSHWAIT_OLD=1` — A/B control for flush_paula's DMA-restart wait (PlatformAmiga.cpp
# ROF_FLUSHWAIT_OLD).  Default OFF: the restart drops each channel to the Paula minimum period
# BEFORE the off-window, so "hold off >2 sample periods" is ~1.1 rasterlines and the wait is the
# constant 7-line floor.  This flag restores sizing the wait from the OUTGOING note's period, i.e.
# the 7..110 line wait that made this the most expensive thing in the flight VBI while a laser or
# explosion swept its frequency (docs/flight-perf-log.md §22/§25).  Read the A/B with
# `GDBSCRIPT=fire_once.gdb` — its audio row is exactly this wait.
ifdef FLUSHWAIT_OLD
EXTRA_DEFINES += -DROF_FLUSHWAIT_OLD
endif

# `make FCI_NOBASE=1` — A/B control for the mem[] base-register fold in
# flight_control_integrate_impl (rof_native.c ROF_FCI_BASE).  Default ON: an empty asm launders
# mem's address through an address register so GCC stops emitting 16-cycle absolute-long operands
# for every access.  This flag restores the plain absolute addressing so the two can be compared
# (statically with objdump, or on target with amiga/isr_full.gdb's integ row).
ifdef FCI_NOBASE
EXTRA_DEFINES += -DROF_FCI_NOBASE
endif

# Profiling aid: `make FORCE_SAUCER=1` (implies PROBES) injects a static flying-saucer P3
# object into the PMG buffer each flight VBI frame (rof_native.c ROF_FORCE_SAUCER) so the
# Amiga per-frame P3 sprite mirror (buildViewportP3Sprite/buildScopeP3Sprite) runs under
# load headlessly — used to profile the saucer sprite path.  OFF by default.
ifdef FORCE_SAUCER
EXTRA_DEFINES += -DROF_FORCE_SAUCER
endif

# ===========================================================================================
# `make COMBAT=1` — THE COMBAT-LOAD BENCHMARK.  Headless flight meets almost no enemies, and
# firing into a near-empty world measures nothing (2026-07-31: AUTO_FIRE alone read IDENTICAL
# FPS, because the shot hit nothing — no impact, no explosion).  The reason is the LEVEL: the
# boot init ($3D1D) sets level_stage $006D = 4, and compute_stage_display_geometry ($75F2)
# derives all three combat knobs from it:
#     $0623 emplacement density  = (lvl==1) ? 0 : min(lvl,$2B)<<2   (RANDOM gate, /256)
#     $0621 saucer spawn period  = (lvl< 4) ? 0 : $58 - 2*min(lvl,$22)     (0 = never)
#     $0624 enemy-fire delay mask= ($2C - min(lvl,$28)) >> 1               (smaller = more)
# At the default level 4 that is $10 / $50 / $14 = only 6.2% of terrain peaks carry a gun
# emplacement, a saucer spawn OPPORTUNITY every 80 terrain-draw passes (then half of those are
# thrown away by a RANDOM gate and more by the "must sit above the terrain" gate), and the
# enemy-fire pacing mask is 10x slower than at level 40.  Sparse, not absent — which is exactly
# why a short AUTO_FIRE window saw nothing.
# COMBAT=1 pins the starting level to COMBAT_LEVEL (default 40), so the whole combat cascade
# comes out of the FAITHFUL binary logic: 63% of terrain peaks carry a jaggi gun emplacement,
# a saucer spawns every 20 draw passes, and the emplacements shoot back almost continuously.
# It also implies AUTO_FIRE (hold the trigger, so the emplacements get destroyed and explode)
# and adds a flight-VBI top-up that re-seeds emplacement cells whose destroyed marker ($FA-$FF)
# has aged back to 0, so a long profiling run does not slowly drain the map.
# Pair with FIXED_RNG=1 so every build flies the same level.
#   make clean && make -j4 COMBAT=1 PROBES=1 FIXED_RNG=1     # verify the load (g_cl* counters)
#   make clean && make -j4 COMBAT=1 FPSCOUNT=1 FIXED_RNG=1   # honest combat framerate
#   ./prof_flight.sh --build ...  with COMBAT=1               # combat PC profile
# ⚠ Deliberately NOT faithful — never judge rendering or gameplay from such a build.
# COMBAT_SAUCER overrides the saucer spawn period $0621 (default 6).  The level's own floor is
# $14 and it counts down per terrain_draw_frame pass, which at flight's real rate measured ZERO
# saucers in a 30 s window — the saucer sprite path never entered the profile.  Set it to 20 to
# get the faithful level-$22+ value back.
COMBAT_LEVEL  ?= 40
COMBAT_SAUCER ?= 6
ifdef COMBAT
EXTRA_DEFINES += -DROF_COMBAT_LOAD -DROF_COMBAT_LEVEL=$(COMBAT_LEVEL) \
                 -DROF_COMBAT_SAUCER=$(COMBAT_SAUCER) -DROF_INVULNERABLE
ifdef COMBAT_QUIET
# `make COMBAT=1 COMBAT_QUIET=1` — the CONTROL build for the combat benchmark.  Same forced
# level (so the same terrain and the same sim tuning), same everything else, but $0623/$0621
# are zeroed and the trigger is not held: no emplacements, no saucers, no firing.  Quote the
# combat cost as COMBAT vs COMBAT_QUIET, never as COMBAT vs a plain build — a plain build flies
# LEVEL 1, so that delta is mostly "level 40 has rougher terrain", not combat.
EXTRA_DEFINES += -DROF_COMBAT_QUIET
else
EXTRA_DEFINES += -DROF_AUTO_FIRE
endif
# An enemy bolt impact calls jitter_roll_pitch, which banks the ship — and with the auto-fire
# autopilot that bank steers it into a mountain within seconds, so the benchmark window was
# spent shooting terrain instead of emplacements.  Hold the attitude accumulators still under
# COMBAT so the ship flies straight; the RANDOM read and the $002E decay are kept, so only the
# steering is suppressed.  `make COMBAT=1 COMBAT_JITTER=1` restores the faithful bank.
ifndef COMBAT_JITTER
EXTRA_DEFINES += -DROF_COMBAT_NO_JITTER
endif
endif

# ===========================================================================================
# `make COMBAT=1 PROBES=1 OBJ_SHAPE=1` + amiga/obj_shape.gdb — SHAPE-PROBE THE OBJECT PLOTTER.
# The combat attribution (d05a72e) put 69% of the combat frame-time delta in DRAW and reasoned
# that ~25 occupied map cells per iteration enter terrain_plot_object while only ONE reaches
# raster_scaled_object, so the cost must be the per-visit plotter overhead.  That was an
# INFERENCE from a count ("25 visits x ~50 ticks"), never a measurement — and 50 ticks is
# ~22000 cycles, far more than the handful of table loads a bailing visit executes.  This probe
# brackets the plotter chain and raster_scaled_object separately and counts the shape of both
# (which early-out fires, the scaled-blit's cell/plot/row counts, the entry-depth histogram) so
# the split is measured instead of assumed.  Counters are flushed once per call from locals, so
# the per-cell loops carry no volatile traffic.
ifdef OBJ_SHAPE
EXTRA_DEFINES += -DROF_OBJ_SHAPE
endif

# Debug / benchmark toggle: `make INVULNERABLE=1` — never die in flight.  Tops the energy bar
# (life_counter $062F) up before it can reach 0 and clears the death-cinematic trigger $063D,
# so a headless run keeps flying instead of ending in the game-over sequence.  Implied by
# COMBAT=1, which would otherwise be shot down in ~30 s of emulated time.  Also useful on its
# own for any long headless flight measurement.  OFF by default.
ifdef INVULNERABLE
EXTRA_DEFINES += -DROF_INVULNERABLE
endif

# `make COMBAT=1 FORCE_EMPLACEMENTS=1` — saturated worst case on top of COMBAT: pin $0623 to
# $FF so EVERY terrain peak carries a gun emplacement instead of the level's 63%.  Peaks are
# the cells terrain_plot_object routes to the A822 plotter, i.e. the ones that shoot back.
ifdef FORCE_EMPLACEMENTS
EXTRA_DEFINES += -DROF_FORCE_EMPLACEMENTS
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
vpath %.c   $(REPO)/src/cpu $(REPO)/src/gen $(REPO)/src
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
EXTRA_DEFINES += -DROF_FLIGHT_PROBE
ifndef NO_TDRAW_PROF
EXTRA_DEFINES += -DROF_TDRAW_PROF
endif
endif
# `make PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1`: same idea as PROFILE_NORING, for the OTHER
# probe that distorts a PC-sample profile.  ROF_TDRAW_PROF instruments terrain_draw_frame_core's
# object-pair loop itself — TDPAIR counters per pair plus PB/PE brackets that each read the live
# beam (VPOSR/VHPOSR) around every project/plot/subdivide call, ~6 chip-register reads per visible
# pair.  That cost lands ON the object-loop source lines, inflating the `game_main_loop` bucket.
# Use this flag when profiling the object loop; drop it when you actually want g_td* read out
# (the g_td*/g_tdSubdiv/g_tdProjPlot globals lose their only writer and get DCE'd, so a gdb
# script that prints them will abort with `Missing ELF symbol` — that is expected).
# `make PROBES=1 PROFILE_NORING=1`: compile out the ROF_FLIGHT_PROBE rescue-debug ring
# (rfPlaneSum — 4x full-plane 47x120 byte-sum walks EVERY frame; it only freezes after a
# rescue, so in steady flight it runs forever and eats ~half the PC-profile samples).
# Use this for any flight PC-sampling profile (diag_sample.sh) so the ring doesn't swamp it.
ifdef PROFILE_NORING
EXTRA_DEFINES += -DROF_PROFILE_NORING
endif
# `make COMBAT=1 PROBES=1 CALIBRATE=1` (+ the QUIET control) + amiga/combat_probe.gdb —
# PRICE THE MACHINE, NOT THE WORK.  Every phase of a combat iteration measures slower than the
# quiet control, including terrain_frame_setup, whose 45-cell loop cannot know about combat.  A
# phase bracket cannot tell computation from DMA stolen by sprites/audio/blitter, so this adds two
# fixed-trip, data-independent loops (scattered mem[] byte reads; a register-only LFSR) once per
# flight iteration.  Their delta between the two builds IS the machine slowdown; subtract it
# before attributing any phase delta to extra work.  Costs ~160 t/it, so never leave it on for a
# framerate or profile run, and expect the budget check's `covered` to fall below 100% (the
# calibration deliberately sits outside every phase bracket).
ifdef CALIBRATE
EXTRA_DEFINES += -DROF_CALIBRATE
endif
# `make FPSCOUNT=1` — the HONEST framerate build.  Adds exactly two things to an otherwise
# shipping binary: the headless auto-launch (so flight is reachable with no keypress) and ONE
# increment per painted terrain frame (g_fpsFrames).  Deliberately does NOT imply PROBES:
# ROF_FLIGHT_PROBE's FP_TIME brackets read two CHIP custom registers plus a 16x16 multiply
# several times per iteration, and ROF_TDRAW_PROF adds ~6 more per visible object pair — that
# instrumentation is what this build exists to measure against.  g_vbiCount is bumped by the
# real VERTB handler in every build, so:  FPS = 50 * g_fpsFrames / g_vbiCount.
# Run: . ./env.sh && GDBSCRIPT=fps_seg.gdb ./diag_run.sh 200
ifdef FPSCOUNT
EXTRA_DEFINES += -DROF_FPSCOUNT
endif
# `make SKIPBOOT=1` — SKIP THE TWO BOOT INITAD SCENES (Logo $5000 + Station $1A97) and enter
# straight at game_entry ($3CDE), i.e. byte-for-byte the behaviour the port had before those
# scenes existed.  Not a faithfulness switch — a MEASUREMENT switch.  Every probe/harness in
# amiga/ is calibrated in wall-clock seconds (`./diag_run.sh [delay]`, `GDBSCRIPT=fps_seg.gdb
# ./diag_run.sh 400`), and the two scenes add ~4-5 s of Logo plus up to ~4 s of Station scroll
# before Standby is reached — enough to silently shift every calibrated delay and every
# docs/flight-perf-log.md comparison, whose first symptom is a mystery perf "regression".
#
# So PROBES=1 and FPSCOUNT=1 IMPLY SKIPBOOT (user decision, 2026-08-12).  Pass SKIPBOOT=0
# explicitly to run a probe build THROUGH the boot scenes (e.g. to instrument them) — and then
# re-derive the delays, don't compare them to anything.
# ⚠ This changes a -D flag, which the Makefile does not track: `make clean` when toggling it.
SKIPBOOT_IMPLIED :=
ifdef PROBES
SKIPBOOT_IMPLIED := 1
endif
ifdef FPSCOUNT
SKIPBOOT_IMPLIED := 1
endif
SKIPBOOT ?= $(SKIPBOOT_IMPLIED)
ifneq ($(SKIPBOOT),)
ifneq ($(SKIPBOOT),0)
EXTRA_DEFINES += -DROF_SKIP_BOOT_SCENES
endif
endif
# LOGO_START — let START (F1) *or the joystick FIRE button* abort the Lucasfilm logo, the way both
# already exit the Station.  A DELIBERATE DIVERGENCE: the Atari's $5000 logo is an unconditional
# blocking routine with no exit check anywhere.
# ⚠ **ON by default since 2026-08-14** (user decision — it was behind an opt-in flag from
# 2026-08-12 to keep the shipped binary faithful; the quality-of-life win won).  Pass
# `make LOGO_START=0` to restore the faithful unconditional logo.
# ⚠ This changes a -D flag, which the Makefile does not track: `make clean` when toggling it.
LOGO_START ?= 1
ifneq ($(LOGO_START),0)
EXTRA_DEFINES += -DROF_LOGO_START_ABORT
endif
# `make PROBES=1 AUTOFIRE=1`: hold the trigger once in flight so the player laser fires
# continuously — a capture path for the shot PMG (dump mem[$0E00 page] + shadows via gdb).
ifdef AUTOFIRE
EXTRA_DEFINES += -DROF_AUTO_FIRE
endif
# `make PROBES=1 FORCE_DEATH=1`: once flight has been running a while, arm the energy-out
# death cinematic (event_trigger $063D) once — headless verification of the
# flash→salmon→black→title sequence + g_flightBlank.  Requires PROBES.
ifdef FORCE_DEATH
EXTRA_DEFINES += -DROF_FORCE_DEATH
endif
# `make NAME_ENTRY=1`: reach the HIGH-SCORE INITIALS entry (name_entry_loop $5B6C) — the only way
# in is to DIE, so ~4 s into flight this arms the energy-out death cinematic ($063D) and seeds a
# current score that beats the (zero) high score.  The faithful binary then runs the death
# teardown, the results card, the game-over tune, and the initials entry.  Needs NO PROBES:
# builds a normal playable out/RoF, so the entry can be driven by hand (also on real hardware).
# Headless: add PROBES=1 and read it with `GDBSCRIPT=name_entry.gdb ./diag_run.sh 90`.
ifdef NAME_ENTRY
EXTRA_DEFINES += -DROF_NAME_ENTRY
endif
# `make PROBES=1 TUNDIFF=1`: run the tunnel direct-painter fidelity differential — decode the live
# $1000 GTIA field through the owner's LUT and diff it against what the painter left in
# tunnelBitmap (docs/boost-tunnel-direct-handoff.md §3, read with amiga/boost_probe.gdb).
# ⚠ OFF by default even under PROBES, and it MUST stay that way: the scan is ~10k volatile mem[]
# reads a frame, which pushes render past one vblank.  That is enough to break the launch
# cinematic's `while (mem[$0684] != $64)` style polls (they step OVER the target value and spin
# forever), and it inflates the tunnel->stars gap by ~60ms.  A run with this on measures FIDELITY,
# never TIMING.
ifdef TUNDIFF
EXTRA_DEFINES += -DROF_TUNNEL_DIFF
endif
# `make PROBES=1 FORCE_RETURN=1`: once flight has been running a while, replicate the B/boosters
# handler (level-clear gate $003A + player_lives $0072=2) once — headless verification of the
# return-to-mother-ship ascent + flight→launch-cinematic handoff.  Requires PROBES.
# Also knocks energy $062F down to half at the boost: the forced return fires before the ship has
# taken any damage, and a FULL energy bar exactly fills its dial, which hides every gauge-vs-
# cockpit rendering bug (user, 2026-08-11).
ifdef FORCE_RETURN
EXTRA_DEFINES += -DROF_FORCE_RETURN
endif
# `make PROBES=1 FORCE_RETURN=1 LEVEL_DONE=1`: make that forced return a COMPLETED level.  The
# rescued-pilot count $0629 is pinned to the level quota $062A when the mother ship arrives, which
# is the condition boot_standby_launch_driver tests at L_615b ($00E3 = $0629-$062A): quota met runs
# the level-clear sequence — per-pilot bonus scoring, the jingle, the count-up-to-level readout —
# and then the LIFT (L_622d/L_6268: the elevator scrolling the LEVEL doors from level+1 to
# min(level+4,$63), stopped early by SELECT).  Plain FORCE_RETURN leaves $0629 at 0, so it takes
# the not-completed branch and NEVER reaches the lift; this flag is the only headless path to it.
# The phase-3 SELECT pulse is suppressed here so the lift runs its full range.
# Readout: amiga/level_done.gdb.  Requires PROBES + FORCE_RETURN.
# LEVEL_DONE_LEVEL=n also pins the flown level (default 1), to reach the lift from level n.
LEVEL_DONE_LEVEL ?= 1
ifdef LEVEL_DONE
EXTRA_DEFINES += -DROF_FORCE_LEVEL_DONE -DROF_LEVEL_DONE_LEVEL=$(LEVEL_DONE_LEVEL)
endif
# `make PROBES=1 SKIPBOOT=0 FORCE_BOOT_FIRE=1`: hold the joystick FIRE button over vbi 100..160 —
# mid-Logo and on into the Station — to verify headlessly that fire advances Logo -> Station ->
# Standby (the 2026-08-14 QoL divergence) with no physical joystick, then release so Standby is
# reached rather than launched through.  ⚠ SKIPBOOT=0 is REQUIRED: PROBES implies SKIPBOOT, which
# skips the very scenes under test.  Readout: amiga/boot_fire.gdb.
ifdef FORCE_BOOT_FIRE
EXTRA_DEFINES += -DROF_FORCE_BOOT_FIRE
endif
# `make FORCE_MOTHERSHIP=1`: INTERACTIVE — makes pressing BOOSTERS (B) in flight force the mother
# ship "present" ($003A=$FF + HUD indicator) at that instant, so B always triggers the return-to-
# mother-ship cinematic WITHOUT having rescued the pilot quota.  Lets you test the return + the
# post-mother-ship Standby level select by hand.  Normal flight is untouched until you press B.
# Needs NO PROBES/auto-launch — builds a normal playable out/RoF; boot with run.sh, press START to
# launch, fly a moment, then press B to return.  B also halves energy $062F, for the same reason
# FORCE_RETURN does: a full bar fills its dial exactly and hides any gauge overflow/priority bug.
ifdef FORCE_MOTHERSHIP
EXTRA_DEFINES += -DROF_FORCE_MOTHERSHIP
endif
# `make PROBES=1 TITLE=1`: force-enter the Title Screen (inject its screen RAM + palette and
# pin rsTitle) instead of auto-launching to flight — a visual test path for the Title Screen
# render before its real attract-mode entry is wired.  Requires PROBES.
ifdef TITLE
EXTRA_DEFINES += -DROF_FORCE_TITLE
endif
# `make PROBES=1 NOAUTO=1`: disable the auto-launch START injection so the player presses
# START manually (measure the REAL manual-launch timing under debug.sh, not the auto path).
ifdef NOAUTO
EXTRA_DEFINES += -DROF_NO_AUTOLAUNCH
endif
# `make PROBES=1 FORCE_SELECT=1 NOAUTO=1`: in the initial Standby, inject a Del/SELECT press
# (CONSOL bit1) instead of START, to verify the wired SELECT drives the transition to the
# separate level-selector card (VVBLKI $53CC).  Requires PROBES; pair with NOAUTO.
ifdef FORCE_SELECT
EXTRA_DEFINES += -DROF_FORCE_SELECT
endif
# `make PROBES=1 FORCE_BREAK=1`: seed a high score, then inject BREAK ($80) during flight to verify
# the game_loop_reset restart recovers (VVBLKI leaves $52B4) and the high score survives.
ifdef FORCE_BREAK
EXTRA_DEFINES += -DROF_FORCE_BREAK
endif
# `make PROBES=1 FORCE_BREAK_EARLY=1`: inject BREAK ($80) during the Standby/launch cinematic
# (before flight) to verify BREAK now restarts from a non-flight scene (bug 1) -> $53CC card.
ifdef FORCE_BREAK_EARLY
EXTRA_DEFINES += -DROF_FORCE_BREAK_EARLY
endif
# `make PROBES=1 FORCE_ESC_STANDBY=1`: inject ESC ($1c) during the Standby cockpit (before the
# auto-START) to verify a command key pressed OUTSIDE flight no longer survives into flight as
# the freeze takeover ($0043) — the "ESC in Standby soft-hangs on the way into flight" bug.
ifdef FORCE_ESC_STANDBY
EXTRA_DEFINES += -DROF_FORCE_ESC_STANDBY
endif
# `make PAUSE_NOW=1`: INTERACTIVE — skip the ~11-minute event_pending_flag ($063B 1->$80) warmup that
# gates the ESC-pause colour cycle, and clock the $5039 strobe every 8 frames instead of every 256, so
# pressing ESC in flight starts the "screensaver" cycle within a second instead of after ~11 min.  ESC
# is still yours to press.  The strobe block itself is untouched/genuine.  Use with ./run.sh to eyeball
# that the cycle reaches the top bar, the canopy pillars and the dashboard body — not just the terrain.
ifdef PAUSE_NOW
EXTRA_DEFINES += -DROF_PAUSE_NOW
endif
# `make PROBES=1 FORCE_PAUSE=1`: the headless half of the same harness — also injects ESC ($1c) during
# flight (retrying until $0043 latches) so no keypress is needed.  Pair with
# `GDBSCRIPT=pause_cycle.gdb ./diag_run.sh 45`, which diffs the strobed $00CF-$00DD bytes against the
# live FlightCopperList colour MOVEs.
ifdef FORCE_PAUSE
EXTRA_DEFINES += -DROF_FORCE_PAUSE
endif
# `make PROBES=1 FORCE_BREAK_CARD=1` (WITHOUT NOAUTO): break during the stars cinematic so the
# restart lands on the $53CC card, then auto-launch drives START -> the card->doors transition
# (bug 3 repro).
ifdef FORCE_BREAK_CARD
EXTRA_DEFINES += -DROF_FORCE_BREAK_CARD
endif
# `make PROBES=1 FORCE_DEMO=1 NOAUTO=1`: launch DEMO DROID (OPTION) then BREAK, to verify the
# restart does NOT re-run the demo (bug: level_or_state $0004 left non-zero).
ifdef FORCE_DEMO
EXTRA_DEFINES += -DROF_FORCE_DEMO
endif
# `make ATTRACT_NOW=1|2`: shorten the Standby attract timeout so the Title Screen (scene 3b)
# appears a few seconds after the Standby settles instead of after ~7000 idle frames (~2.5 min:
# $062D wraps 256 frames per attract_timer $00E2 tick, $64 -> $80).  ONE-SHOT — it sets $00E2 bit7
# once, so the Title reached this way behaves like the genuine timeout and the Standby that
# follows it does NOT immediately bounce back to the Title.
#   ATTRACT_NOW=1  any Standby (use for the INITIAL-standby A/B)
#   ATTRACT_NOW=2  only the POST-MOTHER-SHIP Standby ($003A==$FF) — the mothership-title repro
# Works in a plain interactive build too (`make FORCE_MOTHERSHIP=1 ATTRACT_NOW=2` + ./run.sh).
ifdef ATTRACT_NOW
EXTRA_DEFINES += -DROF_ATTRACT_NOW=$(ATTRACT_NOW)
endif
# `make TITLE_START=1`: once the Title Screen has been up ~2.4 s, HOLD START (CONSOL bit0) so the
# card exits and the Standby behind it launches — the headless equivalent of the user pressing F1
# on the Title.  Released once flight ($4FF5) is reached.  Pair with ATTRACT_NOW.
ifdef TITLE_START
EXTRA_DEFINES += -DROF_TITLE_START
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
# `make FULLXEX=1` embeds the original ../rof.xex instead of the sparse boot image and
# switches XexImage.cpp back to the segment walk — the A/B for suspecting the sparse image.
# The define has to reach BOTH the assembler (.ifdef in incbin.s) and the C++ side.
ifdef FULLXEX
ASFLAGS   += --defsym ROF_FULL_XEX=1
EXTRA_DEFINES += -DROF_FULL_XEX
endif
# vasm (Motorola syntax) for the framework *Assembler.s.  -m68010: the only >68000
# instruction is `movec vbr,d0` in getVBR (the C++ getVBR already emits movec via raw
# bytes through Supervisor(), so this is behaviour-neutral on the A500 68000 target).
VASMFLAGS = -m68010 -Felf -quiet -x -no-opt -nowarn=62 $(EXTRA_VASMDEFS)
# memf_chip_bss.ld: link `.MEMF_CHIP` as an uninitialised (chip BSS) hunk rather than a data
# hunk — those 14,654 bytes were 100.0% zeros in the file (4.8% of out/RoF).  It SUPPLEMENTS
# the default script (INSERT AFTER), so -Ttext=0 still applies.  Read that file before
# touching anything `__chip`: it makes a static initialiser on a __chip variable a silent
# zero, which is why CHIP_BSS_CHECK below gates the link.
CHIP_BSS_LD    = memf_chip_bss.ld
CHIP_BSS_CHECK = $(REPO)/tools/check_chip_bss.py
LDFLAGS   = -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(OUT).map -Wl,-T,$(CHIP_BSS_LD)

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------
all: $(EXE)

# elf2hunk -s strips the HUNK_SYMBOL tables from the executable: 42,520 B of the 384,176 B
# PROBES build (11%) was a debug symbol table nothing reads.  Debugging is unaffected —
# debug.sh and every amiga/*.gdb script resolve symbols from $(OUT).elf, which still has
# them (plus the full .debug_* sections), and the FS-UAE stub is pointed at the ELF too.
# `make SYMS=1` keeps them in the executable if a tool ever needs them there.
ELF2HUNK_FLAGS := $(if $(SYMS),,-s)
$(EXE): $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(EXE) $(ELF2HUNK_FLAGS)
	@grep -qa '$$VER: Rescue on Fractalus! ' $(EXE) || { \
	  echo "error: the AmigaOS \$$VER: string is missing from $@."; \
	  echo "       Nothing references it, so --gc-sections/elf2hunk can drop it silently;"; \
	  echo "       see src/platform/amiga/version.s (SHF_GNU_RETAIN)."; \
	  $(RM) $@; exit 1; }

$(OUT).elf: $(OBJECTS) $(CHIP_BSS_LD) $(CHIP_BSS_CHECK)
	@python3 $(CHIP_BSS_CHECK) $(OBJECTS)
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

# The AmigaOS $VER: string.  Nothing references it (see version.s) — it survives
# --gc-sections only through its section's SHF_GNU_RETAIN flag, which is why the link
# rule below asserts it is still in the executable rather than trusting that.
obj/version.o : $(AMIGA_SRC)/version.s
	$(info ASM  $<)
	@$(AS) $(ASFLAGS) -o $@ "$(abspath $<)"

# NOTE: .incbin dependencies are invisible to gcc -MMD, so list them explicitly.
# Without this, editing/regenerating an embedded dump leaves a stale blob in the
# binary while the on-disk file looks correct (this exact trap shifted the SFX
# music + title data by one byte and cost a long debugging session).
# incbin.s lives in the unified tree ($(AMIGA_SRC)) but its .incbin paths are resolved
# relative to this amiga/ build dir (where assets/ and ../rof.xex live) via ASFLAGS
# -I"$(CURDIR)"; INCBIN_DEPS are likewise listed relative to amiga/.
INCBIN_DEPS := assets/rof_boot_image.bin assets/atari_charset.bin
ifdef FULLXEX
INCBIN_DEPS := ../rof.xex assets/atari_charset.bin
endif

# assets/rof_boot_image.bin — the SPARSE boot image that replaced the embedded rof.xex
# (43,066 -> 27,872 B).  Regenerated whenever the xex, the disassembly it derives the
# instruction map from, or either tool changes.  docs/asset-extraction.md.
assets/rof_boot_image.bin: ../rof.xex ../disasm/listing.txt \
                           ../tools/make_xex_sparse.py ../tools/xex_deadset.py
	$(info GEN  $@)
	@python3 ../tools/make_xex_sparse.py $@
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
