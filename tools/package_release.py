#!/usr/bin/env python3
"""Create the two deterministic WHDLoad install archives (LH5) in dist/.

    python3 tools/package_release.py amiga/out/RoF whdload/RoF.slave dist

  dist/RoF-<version>.lha          the full release: GitHub, with the RoF executable beside it
  dist/RoF-<version>-whdload.lha  for whdload.de: no RoF executable and no Manual; the
                                  Install script asks for the executable instead

Version and date come from VERSION and must agree across the executable, slave, ReadMe and
Install script (release_version below); the archive date is the release date.
"""
import argparse
import hashlib
import re
from pathlib import Path
import _amigaxdev  # noqa: F401
from amigaxdev import release

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / 'whdload/RoF Install'
PREFIX = 'RoF Install'
DRAWER_ICON = ROOT / 'whdload/RoF Install.info'
# The XEGS v5.0 cartridge (README.md). The local Atari binaries are added when present.
ROM_SHA256 = '1eb18e2a1ff31f43871a5e4e21e73f61b7659177d33d2cdbb29f36530b9f4d68'
CHECKED_IN = ('Install', 'Install.info', 'Manual', 'Manual.info', 'ReadMe', 'ReadMe.info', 'RoF.inf')
WHDLOAD_DE_OMITS = ('RoF', 'Manual', 'Manual.info')


def original_hashes():
    hashes = {ROM_SHA256}
    for name in ('rof.rom', 'rof.xex'):
        if (ROOT / name).is_file():
            hashes.add(hashlib.sha256((ROOT / name).read_bytes()).hexdigest())
    return hashes


def release_version(executable, slave, readme, install):
    """Return (version, (day, month, year)) after checking every place that states it."""
    version = (ROOT / 'VERSION').read_text().strip()
    found = re.search(rb'\$VER: Rescue on Fractalus! (\S+) \((\d\d)\.(\d\d)\.(\d{4})\)', executable)
    if not found or found[1].decode() != version:
        raise SystemExit(f'executable $VER: does not state version {version}')
    date = found[2].decode(), found[3].decode(), found[4].decode()
    stated = f'{version} ({".".join(date)})'.encode()
    for what, data, pattern in (
            ('slave $VER:', slave, b'$VER: RoF.slave ' + stated + b'\0'),
            ('slave info', slave, b'version ' + stated + b'\xff'),
            ('ReadMe history', readme, b'\n version ' + stated + b':\n')):
        if pattern not in data:
            raise SystemExit(f'{what} does not state {stated.decode()}')
    if readme.index(b'\n version ') != readme.index(b'\n version ' + stated):
        raise SystemExit('the ReadMe history must start with ' + stated.decode())
    major, minor = version.split('.')
    for name, value in (('exe-version', major), ('exe-revision', minor)):
        if not re.search(rf'^\(set #{name} {value}\)'.encode(), install, re.M):
            raise SystemExit(f'Install #{name} is not {value}')
    return version, date


def members(executable, slave):
    files = {name: (PACKAGE / name).read_bytes() for name in CHECKED_IN}
    files['RoF'] = executable
    files['RoF.slave'] = slave
    return files


def release_notes(readme):
    """The ReadMe's newest History entry as Markdown bullets, for the GitHub release."""
    text = readme.decode('latin-1')
    entry = text[text.index('\n version ') + 1:].split('\n\n', 1)[0].splitlines()[1:]
    notes = []
    for line in entry:
        if line.startswith(' - '):
            notes.append('- ' + line[3:])
        else:
            notes[-1] += ' ' + line.strip()
    return '\n'.join(notes) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('executable', type=Path)
    parser.add_argument('slave', type=Path)
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    files = members(args.executable.read_bytes(), args.slave.read_bytes())
    version, (day, month, year) = release_version(files['RoF'], files['RoF.slave'],
                                                  files['ReadMe'], files['Install'])
    release.refuse_originals(files, original_hashes())
    stamp = release.dos_stamp(int(year), int(month), int(day))
    drawer = DRAWER_ICON.read_bytes()
    for name, contents in ((f'RoF-{version}.lha', files),
                           (f'RoF-{version}-whdload.lha',
                            {k: v for k, v in files.items() if k not in WHDLOAD_DE_OMITS})):
        archive = release.write(args.output_directory / name,
                                release.archive(PREFIX, contents, drawer, stamp))
        print(f'{archive}: {len(contents)} drawer files plus drawer icon, {archive.stat().st_size} bytes')


if __name__ == '__main__':
    main()
