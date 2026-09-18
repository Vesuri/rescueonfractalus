# WHDLoad slave for the Amiga port of Rescue on Fractalus!
#
# ⚠ This makefile runs ON THE AMIGA (or in an emulated Amiga), not on the host: the
# slave is assembled with basm, and it INCLUDEs WHDLoad's own kick13.s, which is
# written for BASM / ASM-One / Asm-Pro / PhxAss.  The host cross-build in ../amiga
# builds the game; this builds only the slave.  See ../docs/whdload-slave.md.
#
# Assigns it expects:
#   Includes:  the Amiga asm includes + WHDLoad's Include drawer (whdload.i,
#              whdmacros.i) and lvo/dos.i
#   Sources:   WHDLoad's Src/sources drawer (so whdload/kick13.s resolves)
#
#   make                  -> RoF.slave        the release slave (no Enhanced Graphics)
#   make ENHANCED_GRAPHICS=1                  bundle the enhanced logo/games artwork
#                                             and offer the Custom4 Enhanced Graphics
#                                             toggle; plain `make` leaves them out
#   make RoFTune.slave    -> RoFTune.slave    same, plus DEBUG and MEMFREE, for
#                                             re-tuning CHIPMEMSIZE/FASTMEMSIZE
#   make clean

BASENAME     = RoF
SLAVE        = $(BASENAME).slave
TUNE_SLAVE   = $(BASENAME)Tune.slave
SLAVE_SOURCE = $(BASENAME)Slave.s

#   O+      enable optimizer
#   OG+     enable global forward-reference optimizing
#   ODd-    disable mulu optimizing
#   ODe-    disable muls optimizing
#   w4-     disable 64k-access warnings
#   wo-     disable optimizing warnings
#   ws-     disable supervisor warnings
COMMON_OPTS = -ws-
SLAVE_OPTS  = -O+ -OG+ -ODd- -ODe- -w4- -wo- $(COMMON_OPTS)

# Enhanced Graphics is opt-in at build time: `make ENHANCED_GRAPHICS=1` defines
# ROF_ENHANCED_GRAPHICS so the slave bundles the enhanced artwork and the Custom4
# hook.  A plain `make` assembles none of it and the enhanced_logo assets below are
# not even a prerequisite.
ifneq ($(ENHANCED_GRAPHICS),)
EG_DEF      = -dROF_ENHANCED_GRAPHICS=1
LOGO_ASSETS = assets/enhanced_logo.bin assets/enhanced_games.bin assets/enhanced_logo.pal
else
EG_DEF      =
LOGO_ASSETS =
endif

all : $(SLAVE)

$(SLAVE) : $(SLAVE_SOURCE) rof_data_layout.i $(LOGO_ASSETS)
	basm $(SLAVE_OPTS) $(EG_DEF) -o$(SLAVE) $(SLAVE_SOURCE)

$(TUNE_SLAVE) : $(SLAVE_SOURCE) rof_data_layout.i $(LOGO_ASSETS)
	basm $(SLAVE_OPTS) $(EG_DEF) -dTUNE=1 -o$(TUNE_SLAVE) $(SLAVE_SOURCE)

clean:
	delete $(SLAVE) $(TUNE_SLAVE)
