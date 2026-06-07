#!/usr/bin/env bash
# Source this to put the ~/.local Amiga toolchain on PATH.
#   . amiga/env.sh
# or from the amiga/ directory:
#   . env.sh
TC="$HOME/.local"
export PATH="$TC/opt/bin:$TC:$TC/fs-uae:$PATH"
export KICKSTART="${KICKSTART:-$HOME/Documents/RetroPie/BIOS/kick31.rom}"
