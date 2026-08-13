#!/usr/bin/env python3
"""xex_deadset.py — the candidate-DEAD byte set of rof.xex, in one place.

Groundwork for dropping rof.xex from the Amiga incbin (docs/asset-extraction.md).
The port transliterates the 6502 to C, so the instruction bytes of the loaded image are
never executed FROM mem[]; only data reads can still need them.  This module computes the
set of instruction bytes that are therefore *candidates* for removal, and can emit a
same-length xex with exactly those bytes zeroed.

⚠ Both the asset generator and the offline diff import THIS module, deliberately.  If the
two ever computed the mask separately they could drift, and a mask that is wider than the
bytes actually zeroed would silently swallow a real difference — the diff would report
"0 unexpected" while hiding a regression.  One definition, imported twice.

Two traps this encodes:

1. listing.txt describes the FLATTENED image, but rof.xex is loaded in STAGES and later
   segments overwrite earlier ones.  Segment 16 ($3CDE-$B7FF) buries both the Lucasfilm
   logo staged at $5000 and the station image staged at $4000, so the disassembly labels
   those addresses as code even though the bytes the boot scenes need there are DATA from
   an earlier segment.  A byte is therefore only a candidate if its segment is the LAST
   WRITER of that address — the one whose content the disassembly actually describes.

2. Zeroing is not an approximation of removal, it is exactly equivalent.  A dropped byte
   would read as 0 at runtime, because rof_load_stage_reset() clears all 64 KB of mem[]
   before placing any segment.  So a zeroed-in-place asset of the SAME LENGTH is a
   faithful model of the sparse asset, and keeping the length means the binary is
   byte-identical in size — which is what makes the A/B legitimate at all (a shorter
   binary shifts main-loop iterations per vblank and diverges the read-clocked $D20A LFSR;
   see the feedback-same-length-asset-ab memory).
"""
import os
import re
import sys

# The region under test.  Segment 16, the main code+data blob; the logo/station/charset
# segments are left alone for now (their code is a separate question).
REGION_LO = 0x3CDE
REGION_HI = 0xB7FF


def parse_segments(raw):
    """[(file_offset, load_addr, count)] for each XEX segment, in file order."""
    segs = []
    i = 2 if raw[:2] == b'\xff\xff' else 0
    while i + 4 <= len(raw):
        if raw[i] == 0xFF and raw[i + 1] == 0xFF:
            i += 2
            continue
        s = raw[i] | (raw[i + 1] << 8)
        e = raw[i + 2] | (raw[i + 3] << 8)
        i += 4
        cnt = min(e - s + 1, len(raw) - i)
        segs.append((i, s, cnt))
        i += cnt
    return segs


def instruction_map(listing_path):
    """bytearray(65536): 1 where the disassembly consumed the byte as part of an opcode."""
    m = bytearray(65536)
    line_re = re.compile(r'^([0-9a-f]{4})  ((?:[0-9A-F]{2} )+)\s')
    with open(listing_path, errors='replace') as f:
        for ln in f:
            g = line_re.match(ln)
            if not g:
                continue
            a = int(g.group(1), 16)
            nb = len(g.group(2).split())
            for k in range(a, min(a + nb, 65536)):
                m[k] = 1
    return m


_READ_OPS = {'LDA', 'LDX', 'LDY', 'CMP', 'CPX', 'CPY', 'ADC', 'SBC', 'AND', 'ORA', 'EOR',
             'BIT', 'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'}     # RMW reads too
_LINE = re.compile(r'^([0-9a-f]{4})  ((?:[0-9A-F]{2} )+)\s+([A-Z]{3})\s*(.*)$')
_INDEX_WINDOW = 256          # abs,X / abs,Y can reach base+0..255


def reachable_map(listing_path):
    """bytearray(65536): 1 where SOME instruction could read the byte as data.

    Deliberately pessimistic, and it is what gates the removal — path coverage cannot,
    because the combat path is not run-to-run deterministic (two runs of ONE binary differ
    in 1024 bytes), so no A/B can clear it.  Three sources:

      * exact absolute reads             `LDA $1234`      -> that byte
      * indexed absolute reads           `LDA $1234,X`    -> base..base+255 (whole window;
                                          real tables are usually far shorter, so this
                                          over-keeps on purpose)
      * indirect reads through a ZP pointer built from an immediate pair
                                         `LDA #lo/STA $zp` + `LDA #hi/STA $zp+1`
                                          -> ptr..ptr+255
      * `JMP ($1234)` reads its 2-byte vector

    Not covered: a pointer computed at runtime from a table.  That residual is what the
    empirical A/B on the two deterministic paths (Standby, quiet flight) is for.
    """
    insns = []
    with open(listing_path, errors='replace') as f:
        for ln in f:
            m = _LINE.match(ln)
            if m:
                insns.append((int(m.group(1), 16), m.group(3), m.group(4).strip()))

    reach = bytearray(65536)

    def mark(base, n=1):
        for k in range(base, min(base + n, 65536)):
            reach[k] = 1

    for a, mn, op in insns:
        if mn == 'JMP' and op.startswith('('):
            g = re.match(r'\(0x([0-9a-f]{4})\)', op)
            if g:
                mark(int(g.group(1), 16), 2)
            continue
        if mn not in _READ_OPS:
            continue
        g = re.match(r'0x([0-9a-f]{4})(,([XY]))?$', op)
        if g:
            mark(int(g.group(1), 16), _INDEX_WINDOW if g.group(3) else 1)

    # ZP pointers built from a nearby pair of immediates
    def store_after(i):
        for j in range(i + 1, min(i + 4, len(insns))):
            if insns[j][1] in ('STA', 'STX', 'STY'):
                g = re.match(r'0x00([0-9a-f]{2})$', insns[j][2])
                if g:
                    return int(g.group(1), 16)
        return None

    for i, (a, mn, op) in enumerate(insns):
        if mn not in ('LDA', 'LDX', 'LDY'):
            continue
        g = re.match(r'#0x([0-9a-f]{1,2})$', op)
        if not g:
            continue
        lo = int(g.group(1), 16)
        z = store_after(i)
        if z is None:
            continue
        for j in range(max(0, i - 10), min(i + 10, len(insns))):
            if insns[j][1] not in ('LDA', 'LDX', 'LDY'):
                continue
            g2 = re.match(r'#0x([0-9a-f]{1,2})$', insns[j][2])
            if not g2:
                continue
            v = int(g2.group(1), 16)
            z2 = store_after(j)
            if z2 == z + 1:
                mark(((v << 8) | lo), _INDEX_WINDOW)
            elif z2 == z - 1:
                mark(((lo << 8) | v), _INDEX_WINDOW)
    return reach


def dead_set(raw, isinsn, lo=REGION_LO, hi=REGION_HI, reach=None):
    """(mask, zeroed_raw): mask[addr]=1 for each candidate-dead ADDRESS, plus a
    same-length copy of `raw` with those bytes zeroed."""
    segs = parse_segments(raw)
    last = {}
    for n, (off, s, cnt) in enumerate(segs):
        for k in range(s, s + cnt):
            last[k] = n                       # trap 1: who really owns this address
    mask = bytearray(65536)
    out = bytearray(raw)
    for n, (off, s, cnt) in enumerate(segs):
        for j in range(cnt):
            a = s + j
            if not (lo <= a <= hi):
                continue
            if last[a] != n:
                continue
            if not isinsn[a]:
                continue
            if reach is not None and reach[a]:
                continue                      # some instruction could read it: KEEP
            if out[off + j] == 0:
                continue                      # already zero: nothing to prove
            out[off + j] = 0
            mask[a] = 1
    return mask, bytes(out)


# Resolve assets against the REPO ROOT, not the cwd — the shell harness runs from amiga/.
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(xex_path=None, listing_path=None, conservative=True):
    """(raw, (mask, zeroed)).  conservative=True (the shipping setting) also keeps every byte
    reachable_map() says an instruction could read; False gives the wider instruction-bytes
    set, which only the two deterministic paths can vouch for."""
    xex_path = xex_path or os.path.join(_ROOT, 'rof.xex')
    listing_path = listing_path or os.path.join(_ROOT, 'disasm', 'listing.txt')
    raw = open(xex_path, 'rb').read()
    reach = reachable_map(listing_path) if conservative else None
    return raw, dead_set(raw, instruction_map(listing_path), reach=reach)


if __name__ == '__main__':
    # xex_deadset.py <out.xex>   — write the zeroed same-length asset
    raw, (mask, zeroed) = load()
    dest = sys.argv[1] if len(sys.argv) > 1 else '/tmp/rof_codezero.xex'
    open(dest, 'wb').write(zeroed)
    print(f"{dest}: {len(zeroed)} B (same length as rof.xex), "
          f"{sum(mask)} candidate-dead instruction bytes zeroed")
