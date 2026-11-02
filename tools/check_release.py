#!/usr/bin/env python3
"""Audit both release archives independently of the packager's encoder.

    python3 tools/check_release.py [dist]
    python3 tools/check_release.py --notes     # print the GitHub release notes

Each member is decompressed with Lhasa (extract-only, so independent of the LHa for UNIX that
wrote it), its CRC and size checked against the header, and its bytes compared with the
source it came from. The full archive must carry RoF, the whdload.de one must not carry RoF
or the Manual, and neither may carry original game data.
"""
import argparse
import hashlib
import shutil
import struct
import subprocess
from pathlib import Path
import _amigaxdev  # noqa: F401
from amigaxdev.release import crc16, dos_stamp
from package_release import (CHECKED_IN, DRAWER_ICON, PACKAGE, PREFIX, ROOT, WHDLOAD_DE_OMITS,
                             original_hashes, release_notes, release_version)

EXECUTABLE = ROOT / 'amiga/out/RoF'
SLAVE = ROOT / 'whdload/RoF.slave'


def read_archive(path, lhasa):
    raw = path.read_bytes()
    pos, payloads, stamps = 0, {}, set()
    while pos < len(raw) and raw[pos]:
        size = raw[pos]
        header = raw[pos + 2:pos + 2 + size]
        assert len(header) == size and sum(header) & 255 == raw[pos + 1], 'bad header checksum'
        assert header[:5] == b'-lh5-' and header[18] == 0, 'expected level-zero LH5'
        packed, unpacked = struct.unpack_from('<II', header, 5)
        stamps.add(struct.unpack_from('<I', header, 13)[0])
        n = header[19]
        assert size == 22 + n
        name = header[20:20 + n].decode('ascii').replace('\\', '/')
        assert name == PREFIX + '.info' or name.startswith(PREFIX + '/'), 'wrong drawer: ' + name
        assert name not in payloads, 'duplicate member: ' + name
        pos += size + 2
        assert len(raw[pos:pos + packed]) == packed, 'truncated payload'
        data = subprocess.run([lhasa, 'pq', str(path.resolve()), name],
                              check=True, capture_output=True).stdout
        assert len(data) == unpacked and crc16(data) == struct.unpack_from('<H', header, 20 + n)[0], \
            'bad payload CRC: ' + name
        payloads[name] = data
        pos += packed
    assert raw[pos:] == b'\0', 'trailing bytes after the archive end'
    return payloads, stamps


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dist', type=Path, nargs='?', default=ROOT / 'dist')
    parser.add_argument('--notes', action='store_true')
    args = parser.parse_args()
    sources = {name: (PACKAGE / name).read_bytes() for name in CHECKED_IN}
    sources['RoF'] = EXECUTABLE.read_bytes()
    sources['RoF.slave'] = SLAVE.read_bytes()
    version, (day, month, year) = release_version(sources['RoF'], sources['RoF.slave'],
                                                  sources['ReadMe'], sources['Install'])
    if args.notes:
        print(release_notes(sources['ReadMe']), end='')
        return
    lhasa = shutil.which('lha')
    assert lhasa and b'Lhasa' in subprocess.run([lhasa], capture_output=True).stdout, \
        'the independent check needs Lhasa as `lha` (brew install lhasa)'
    hashes = original_hashes()
    for name, omitted in ((f'RoF-{version}.lha', ()), (f'RoF-{version}-whdload.lha', WHDLOAD_DE_OMITS)):
        payloads, stamps = read_archive(args.dist / name, lhasa)
        assert stamps == {dos_stamp(int(year), int(month), int(day))}, f'{name}: not dated {day}.{month}.{year}'
        expected = {PREFIX + '.info': DRAWER_ICON.read_bytes()}
        expected.update({PREFIX + '/' + k: v for k, v in sources.items() if k not in omitted})
        assert set(payloads) == set(expected), \
            f'{name}: missing {set(expected) - set(payloads)}, unexpected {set(payloads) - set(expected)}'
        for member, data in payloads.items():
            assert data == expected[member], f'{name}: {member} differs from its source'
            assert hashlib.sha256(data).hexdigest() not in hashes, f'{name}: {member} is original data'
        for member in ('RoF', 'RoF.slave'):
            if PREFIX + '/' + member in payloads:
                assert payloads[PREFIX + '/' + member][:4] == b'\0\0\3\xf3', member + ' is not a HUNK file'
        print(f'PASS: {name}: {len(payloads)} members, Lhasa decompression, CRCs, sources and version {version}')


if __name__ == '__main__':
    main()
