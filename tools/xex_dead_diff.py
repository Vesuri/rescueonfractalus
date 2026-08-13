#!/usr/bin/env python3
"""xex_dead_diff.py — compare two frame-locked mem[] dumps with the candidate-dead set masked out.

    tools/xex_dead_diff.py <intact.bin> <zeroed.bin> [label]

The two dumps come from amiga/mem_dump*.gdb on binaries that differ ONLY in the xex asset
(same length, so the same code and the same timing).  Every byte inside the candidate-dead
set is expected to differ — that is the change under test.  Anything ELSE that differs is a
byte of downstream state that moved, i.e. proof that something read a supposedly-dead byte.

Why a checksum probe cannot do this job: the blocks it would sum CONTAIN the bytes under
test, so they differ trivially and the result is vacuous.  Hence raw dumps + this mask.

Exit status 0 = no unexpected differences.
"""
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from xex_deadset import load                                   # noqa: E402  (one mask, shared)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a = open(sys.argv[1], 'rb').read()
    b = open(sys.argv[2], 'rb').read()
    label = sys.argv[3] if len(sys.argv) > 3 else ''
    if len(a) != 65536 or len(b) != 65536:
        print(f"expected two 65536-byte dumps, got {len(a)} and {len(b)}")
        return 2

    _raw, (mask, _zeroed) = load()
    diff = [k for k in range(65536) if a[k] != b[k]]
    unexpected = [k for k in diff if not mask[k]]

    print(f"=== xex dead-set diff {label} ===")
    print(f"  candidate-dead addresses : {sum(mask)}")
    print(f"  differing bytes          : {len(diff)}")
    print(f"    expected (in the set)  : {len(diff) - len(unexpected)}")
    print(f"    UNEXPECTED             : {len(unexpected)}")
    if not unexpected:
        print("  => nothing outside the candidate-dead set moved: those bytes are unread "
              "on this path")
        return 0

    runs = []
    s = p = unexpected[0]
    for k in unexpected[1:]:
        if k == p + 1:
            p = k
            continue
        runs.append((s, p))
        s = p = k
    runs.append((s, p))
    print(f"  => LIVE bytes found, in {len(runs)} runs (these must stay in the asset):")
    for s, e in runs[:60]:
        print(f"     ${s:04X}-${e:04X} ({e - s + 1:5d} B)  intact={a[s]:02x} zeroed={b[s]:02x}")
    if len(runs) > 60:
        print(f"     ... +{len(runs) - 60} more runs")
    return 1


if __name__ == '__main__':
    sys.exit(main())
