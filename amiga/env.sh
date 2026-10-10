#!/usr/bin/env bash
# Source this for the shared Amiga environment that AmigaXDev installs (~/.local/share/amiga):
# the toolchain and FS-UAE on PATH, Kickstarts, WHDLoad and the FS-UAE helpers.  Every variable
# can be overridden from the environment.
#   . amiga/env.sh
# or from the amiga/ directory:
#   . env.sh
# The slave and its tests use WHDLoad 19.2 (the shared default is 20.0).
export WHDLOAD="${WHDLOAD:-${AMIGA_SHARE:-$HOME/.local/share/amiga}/WHDLoad}"
. "${AMIGA_ENV:-$HOME/.local/share/amiga/env.sh}"
