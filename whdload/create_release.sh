#!/usr/bin/env bash
# Assemble the WHDLoad install archive in whdload/dist/.
#
# Runs on the HOST.  It collects three things that are built elsewhere:
#   * the game executable   ../amiga/out/RoF.exe   (`cd ../amiga && . env.sh && make`)
#   * the slave             ./RoF.slave            (`make` on the Amiga -- see makefile)
#   * the install package   ./install/             (in the repo)
#
# Result: whdload/dist/RoF.lha, the archive to hand to a player, plus the unpacked
# tree beside it so you can inspect or test it without unpacking.
set -euo pipefail
cd "$(dirname "$0")"

DIST=dist
PKG="RoF Install"
EXE=../amiga/out/RoF.exe
SLAVE=RoF.slave

[ -f "$EXE" ] || {
  echo "error: $EXE not found."
  echo "       Build the game first:  cd ../amiga && . env.sh && make"
  exit 1
}
[ -f "$SLAVE" ] || {
  echo "error: $SLAVE not found."
  echo "       The slave is assembled with basm ON THE AMIGA -- see whdload/makefile."
  exit 1
}

rm -rf "$DIST"
mkdir -p "$DIST/$PKG"

# The install package as checked in (Install, its icon, the ReadMe, the game icon).
cp -p "install/$PKG"/* "$DIST/$PKG/"
cp -p "install/$PKG.info" "$DIST/"

# ...plus the two build artifacts.  The Install script copies "RoF" into <dest>/data,
# which is what the slave's ws_CurrentDir points at.
cp -p "$EXE"   "$DIST/$PKG/RoF"
cp -p "$SLAVE" "$DIST/$PKG/$SLAVE"

# The player-facing docs from the repo root, so the archive stands on its own.
cp -p ../docs/manual.md "$DIST/$PKG/Manual"

if command -v lha >/dev/null 2>&1; then
  ( cd "$DIST" && lha a RoF.lha "$PKG" "$PKG.info" >/dev/null )
  echo "built $DIST/RoF.lha"
else
  echo "note: 'lha' not installed -- the unpacked tree is in $DIST/, archive it on the Amiga"
fi

echo "contents:"
find "$DIST" -type f | sort | sed 's/^/  /'
