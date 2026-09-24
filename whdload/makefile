# Host cross-build; the original BASM rules are in Makefile.amiga.
WHDLOAD ?= $(HOME)/.local/share/amiga/WHDLoad
NDK ?= $(HOME)/.local/opt/m68k-amiga-elf/sys-include
VASM ?= $(HOME)/.local/vasmm68k_mot
VASMFLAGS ?=
LHA ?= $(HOME)/.local/opt/lha/bin/lha
export LHA

.PHONY: all enhanced tune release dist clean FORCE
all: RoF.slave

# Always assemble: options and SDK includes must not leave a stale variant.
# Use the checked-in kick13/kickfs/segtracker sources (already use _lib.i names).
RoF.slave: RoFSlave.s rof_data_layout.i FORCE
	$(VASM) -m68000 -pic -x -devpac -Fhunkexe -nosym $(VASMFLAGS) -I"$(WHDLOAD)/Include" -I"$(NDK)" -o $@ $<

enhanced:
	$(MAKE) RoF.slave VASMFLAGS="$(VASMFLAGS) -DROF_ENHANCED_GRAPHICS=1"

tune: RoFTune.slave
RoFTune.slave: RoFSlave.s rof_data_layout.i FORCE
	$(VASM) -m68000 -pic -x -devpac -Fhunkexe -nosym $(VASMFLAGS) -DTUNE=1 -I"$(WHDLOAD)/Include" -I"$(NDK)" -o $@ $<

release: dist

dist:
	bash -c '. ../amiga/env.sh && $(MAKE) -C ../amiga clean && $(MAKE) -C ../amiga'
	./create_release.sh

clean:
	rm -f RoF.slave RoFTune.slave
