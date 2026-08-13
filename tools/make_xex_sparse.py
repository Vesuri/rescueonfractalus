#!/usr/bin/env python3
"""make_xex_sparse.py — build the SPARSE boot-image asset that replaces rof.xex.

    tools/make_xex_sparse.py [out.bin]

rof.xex is 43,066 B of which ~23.4 KB is 6502 instruction bytes the port never reads (it
transliterates the code to C; only DATA reads still need the image).  This emits the same
boot image with those bytes omitted, in a format the loader can stream:

    chunk  := u16 load_addr, u16 len, len bytes          (big-endian, 68000-native)
    stream := chunk*

There is deliberately NO stage marker: a stage ends at the chunk covering INITAD
($02E2-$02E3), exactly the rule xex_parse_stage already uses for segments, so
xex_sparse_stage stays structurally identical to it and rof_boot.c needs no change.

The emit rule is a SIMULATION, not a filter, and that is the whole correctness argument:

    state[] starts as 64 KB of zeros (what rof_load_stage_reset leaves)
    for each segment, in file order, for each byte:  emit iff state[addr] != value

Emitting only where the simulated state disagrees reproduces the zeroed asset's mem[]
byte-for-byte at every stage boundary — which is what the A/B actually validated.  A naive
"drop the dead bytes" filter would be WRONG: segment 16 overwrites the logo staged at $5000
and the station image staged at $4000, so a dead byte there must still be written (as 0) to
CLOBBER the earlier segment's data.  Dropping it would leave the station image visible to
the game stage.  The simulation catches those 2,160 addresses automatically.

$02E2/$02E3 are force-emitted even when they match the simulated state: INITAD $5000 is the
bytes "00 50", and that leading zero would otherwise be omitted, splitting the chunk that
the loader detects a stage boundary on.

Self-checks before writing: replay the emitted stream and assert it reproduces the zeroed
image at every stage boundary, and assert the stage count and INITAD sequence are unchanged.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_deadset import load, parse_segments      # noqa: E402  (one mask, shared)

INITAD_LO, INITAD_HI = 0x02E2, 0x02E3


# Bridge a gap of at most this many unneeded bytes rather than starting a new chunk.  A
# chunk header costs 4 bytes, so 4 is the break-even and measurably the optimum (gap 0 ->
# 22,789 B in 1,361 chunks; gap 4 -> 20,051 B in 265; gap 8 -> 20,292 B).  Bridging is free
# correctness-wise: need[j]==0 means the simulated state ALREADY equals that payload byte,
# so writing it is a semantic no-op.
MERGE_GAP = 4


def build(raw, zeroed):
    """-> (blob, chunks, stage_initads)"""
    segs = parse_segments(zeroed)
    state = bytearray(65536)                     # post-reset mem[]
    out = bytearray()
    chunks = 0
    stage_initads = []

    for (off, s, cnt) in segs:
        payload = zeroed[off:off + cnt]
        # which bytes of this segment must actually be written?
        need = bytearray(cnt)
        for j in range(cnt):
            a = (s + j) & 0xFFFF
            if state[a] != payload[j] or a in (INITAD_LO, INITAD_HI):
                need[j] = 1
        # emit runs of needed bytes, bridging gaps up to MERGE_GAP
        j = 0
        while j < cnt:
            if not need[j]:
                j += 1
                continue
            k = j
            while k < cnt:
                end = k
                while end < cnt and need[end]:
                    end += 1
                nxt = end
                while nxt < cnt and not need[nxt]:
                    nxt += 1
                if end < cnt and nxt < cnt and (nxt - end) <= MERGE_GAP:
                    k = nxt                       # bridge the gap, keep going
                else:
                    k = end
                    break
            addr = (s + j) & 0xFFFF
            run = payload[j:k]
            out += struct.pack('>HH', addr, len(run)) + run
            chunks += 1
            j = k
        # advance the simulated state over the WHOLE segment (emitted or not: an omitted
        # byte is omitted precisely because the state already equals it)
        for j in range(cnt):
            state[(s + j) & 0xFFFF] = payload[j]
        if s <= INITAD_HI and s + cnt - 1 >= INITAD_LO:
            stage_initads.append((state[INITAD_LO] | (state[INITAD_HI] << 8)))
    return bytes(out), chunks, stage_initads


def replay(blob):
    """Stream the sparse blob the way the C loader will; -> (final_mem, per_stage_snapshots)"""
    mem = bytearray(65536)
    snaps = []
    i = 0
    while i + 4 <= len(blob):
        a, n = struct.unpack_from('>HH', blob, i)
        i += 4
        for j in range(n):
            mem[(a + j) & 0xFFFF] = blob[i + j]
        i += n
        if a <= INITAD_HI and a + n - 1 >= INITAD_LO:
            snaps.append(bytes(mem))
    return bytes(mem), snaps


def reference(zeroed):
    """The same snapshots produced by the EXISTING segment loader on the zeroed asset."""
    mem = bytearray(65536)
    snaps = []
    for (off, s, cnt) in parse_segments(zeroed):
        for j in range(cnt):
            mem[(s + j) & 0xFFFF] = zeroed[off + j]
        if s <= INITAD_HI and s + cnt - 1 >= INITAD_LO:
            snaps.append(bytes(mem))
    return bytes(mem), snaps


def main():
    raw, (mask, zeroed) = load()
    blob, chunks, initads = build(raw, zeroed)

    got_final, got_snaps = replay(blob)
    ref_final, ref_snaps = reference(zeroed)

    ok = True
    if len(got_snaps) != len(ref_snaps):
        print(f"FAIL: stage count {len(got_snaps)} != {len(ref_snaps)}")
        ok = False
    for n, (g, r) in enumerate(zip(got_snaps, ref_snaps)):
        if g != r:
            bad = [k for k in range(65536) if g[k] != r[k]]
            print(f"FAIL: stage {n + 1} snapshot differs at {len(bad)} bytes, "
                  f"first ${bad[0]:04X}")
            ok = False
    if got_final != ref_final:
        print("FAIL: final image differs")
        ok = False
    if not ok:
        return 1

    print(f"sparse blob: {len(blob):,} B in {chunks:,} chunks "
          f"({chunks * 4:,} B of headers)")
    print(f"  vs rof.xex {len(raw):,} B  =>  saves {len(raw) - len(blob):,} B")
    print(f"  candidate-dead bytes omitted: {sum(mask):,}")
    print(f"  stages: {len(initads)}  INITAD chain: " +
          " -> ".join(f"${a:04X}" for a in initads))
    print(f"  self-check: replay reproduces the zeroed image at all "
          f"{len(ref_snaps)} stage boundaries AND at the end")

    dest = sys.argv[1] if len(sys.argv) > 1 else '/tmp/rof_sparse.bin'
    open(dest, 'wb').write(blob)
    print(f"  wrote {dest}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
