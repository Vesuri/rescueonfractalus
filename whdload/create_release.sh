#!/usr/bin/env bash
# Assemble the WHDLoad install archive in whdload/dist/.
#
# Runs on the HOST.  It collects three things that are built elsewhere:
#   * the game executable   ../amiga/out/RoF       (`cd ../amiga && . env.sh && make`)
#   * the slave             ./RoF.slave            (`make` on the Amiga -- see makefile)
#   * the install package   ./RoF Install/         (in the repo)
#
# Result: whdload/dist/RoF.lha, the archive to hand to a player, plus the unpacked
# tree beside it so you can inspect or test it without unpacking.
set -euo pipefail
cd "$(dirname "$0")"

DIST=dist
PKG="RoF Install"
EXE=../amiga/out/RoF
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

# The public package must use the external-data build.  Check its ELF sections and descriptor
# rather than guessing from printable strings (the Atari text is encoded).
python3 ../tools/audit_release_data.py ../amiga/out/RoF.elf "$EXE" --repo ..

rm -rf "$DIST"
mkdir -p "$DIST/$PKG"

# The install package as checked in (Install, the ReadMe, the Manual, their icons,
# and the game icon).  The Manual is generated from docs/manual.md by
# tools/make_whdload_manual.py and checked in -- not rendered here.
cp -p "$PKG"/* "$DIST/$PKG/"
cp -p "$PKG.info" "$DIST/"

# The ReadMe keeps the Enhanced Graphics (Custom4) paragraph inside
# @@ENHANCED_GRAPHICS markers.  Custom4 is a build-time option the default slave
# omits, so drop that block unless ENHANCED_GRAPHICS=1 selects an enhanced
# release; the markers themselves never ship either way.  (Portable sed: no -i.)
README="$DIST/$PKG/ReadMe"
if [ -n "${ENHANCED_GRAPHICS:-}" ]; then
  sed -e '/^@@ENHANCED_GRAPHICS_BEGIN$/d' -e '/^@@ENHANCED_GRAPHICS_END$/d' "$README" > "$README.tmp"
else
  sed '/^@@ENHANCED_GRAPHICS_BEGIN$/,/^@@ENHANCED_GRAPHICS_END$/d' "$README" > "$README.tmp"
fi
mv "$README.tmp" "$README"

# ...plus the two build artifacts.  The Install script copies "RoF" into <dest>/data,
# which is what the slave's ws_CurrentDir points at.
cp -p "$EXE"   "$DIST/$PKG/RoF"
cp -p "$SLAVE" "$DIST/$PKG/$SLAVE"

if command -v lha >/dev/null 2>&1; then
  ( cd "$DIST" && lha a RoF.lha "$PKG" "$PKG.info" >/dev/null )
  echo "built $DIST/RoF.lha"
else
  echo "note: 'lha' not installed -- the unpacked tree is in $DIST/, archive it on the Amiga"
fi

echo "contents:"
find "$DIST" -type f | sort | sed 's/^/  /'
