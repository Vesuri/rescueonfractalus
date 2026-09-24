#!/usr/bin/env bash
# Assemble the WHDLoad install archive in whdload/dist/.
#
# Runs on the HOST.  It builds the slave and collects:
#   * the game executable   ../amiga/out/RoF       (`cd ../amiga && . env.sh && make`)
#   * the slave             ./RoF.slave            (`make` on this host)
#   * the install package   ./RoF Install/         (in the repo)
#
# Result: whdload/dist/RescueOnFractalus.lha, the archive to hand to a player, plus the unpacked
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
# Rebuild the default slave even if the previous build was enhanced or diagnostic.
make RoF.slave VASMFLAGS=

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

# ...plus the two build artifacts.  The Install script copies "RoF" into <dest>/data,
# which is what the slave's ws_CurrentDir points at.
cp -p "$EXE"   "$DIST/$PKG/RoF"
cp -p "$SLAVE" "$DIST/$PKG/$SLAVE"

# Native LH5 compressor; LHA can override the installed compressor path.
python3 ../tools/package_whdload.py "$DIST"

echo "contents:"
find "$DIST" -type f | sort | sed 's/^/  /'
