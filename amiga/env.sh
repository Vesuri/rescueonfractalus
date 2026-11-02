#!/usr/bin/env bash
# Source this for the shared Amiga environment that AmigaXDev installs (~/.local/share/amiga):
# the toolchain and FS-UAE on PATH, Kickstarts, WHDLoad and the FS-UAE helpers.  Every variable
# can be overridden from the environment.
#   . amiga/env.sh
# or from the amiga/ directory:
#   . env.sh
# The slave and its tests use the shared WHDLoad (20.1).
export WHDLOAD="${WHDLOAD:-${AMIGA_SHARE:-$HOME/.local/share/amiga}/WHDLoad}"
. "${AMIGA_ENV:-$HOME/.local/share/amiga/env.sh}"

# Which FS-UAE every run/debug/diag/probe script uses (fsuae_common.sh reads $FSUAE).
# The native arm64 build is several times faster on long diagnostics and 68030 runs,
# and it carries the cached-MMU write-attribute correction that the x86_64 build on
# PATH lacks -- without it a cached 040 can read stale DMA pixels.  Nothing here needs
# the x86_64 build (only host lldb attach does, which this tree never uses).
# Memory timing differs between binaries: never compare measurements across them.
export FSUAE="${FSUAE:-$FSUAE_ARM}"
