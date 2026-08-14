# WHDLoad slave for the Amiga port of Rescue on Fractalus!
#
# ⚠ This makefile runs ON THE AMIGA (or in an emulated Amiga), not on the host: the
# slave is assembled with basm, and it INCLUDEs WHDLoad's own kick13.s, which is
# written for BASM / ASM-One / Asm-Pro / PhxAss.  Same arrangement as the Stunt Car
# Racer slave.  The host cross-build in ../amiga builds the game; this builds only
# the slave.  See ../docs/whdload-slave.md.
#
# Assigns it expects:
#   Includes:  the Amiga asm includes + WHDLoad's Include drawer (whdload.i,
#              whdmacros.i) and lvo/dos.i
#   Sources:   WHDLoad's Src/sources drawer (so whdload/kick13.s resolves)
#
#   make                  -> RoF.slave        the release slave
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

all : $(SLAVE)

$(SLAVE) : $(SLAVE_SOURCE)
	basm $(SLAVE_OPTS) -o$(SLAVE) $(SLAVE_SOURCE)

$(TUNE_SLAVE) : $(SLAVE_SOURCE)
	basm $(SLAVE_OPTS) -dTUNE=1 -o$(TUNE_SLAVE) $(SLAVE_SOURCE)

clean:
	delete $(SLAVE) $(TUNE_SLAVE)
