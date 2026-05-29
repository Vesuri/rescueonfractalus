#!/usr/bin/env python3
"""Build the post-load 64K RAM image from a XEX, mirroring Atari load order.

Applies every segment's bytes to a flat 64K image in file order (later
segments overwrite earlier ones, matching real loader behaviour, e.g. the
$3CDE overlap). Records the final INITAD/RUNAD vectors and a block manifest
of the RAM ranges actually touched (for Ghidra memory-block creation).

Outputs:
  disasm/rof_mem.bin        flat 64K image
  disasm/rof_blocks.txt     "start end" hex ranges of touched RAM (merged)
  prints entry vector(s)
"""
import sys, os

def main(path, outdir):
    data = open(path, "rb").read()
    n = len(data)
    u16 = lambda i: data[i] | (data[i+1] << 8)
    assert u16(0) == 0xFFFF, "not a XEX"

    mem = bytearray(64 * 1024)
    touched = []          # list of (start, end) inclusive, RAM only
    initad = runad = None

    p = 2
    while p + 4 <= n:
        if u16(p) == 0xFFFF:
            p += 2
            if p + 4 > n: break
        start, end = u16(p), u16(p + 2); p += 4
        length = end - start + 1
        body = data[p:p + length]; p += length
        # Track run/init vectors (consumed by loader, not game memory).
        if start <= 0x02E2 <= end or start <= 0x02E3 <= end:
            initad = body[-2] | (body[-1] << 8) if len(body) >= 2 else initad
        if start <= 0x02E0 <= end or start <= 0x02E1 <= end:
            runad = body[0] | (body[1] << 8) if len(body) >= 2 else runad
        # Apply to image (everything below the hardware page; harmless above).
        for i, b in enumerate(body):
            a = start + i
            if 0 <= a < 0x10000:
                mem[a] = b
        if start < 0xD000:            # treat as real RAM content for Ghidra
            touched.append((start, end))

    # Merge touched ranges.
    touched.sort()
    merged = []
    for s, e in touched:
        if merged and s <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], e))
        else:
            merged.append((s, e))

    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, "rof_mem.bin"), "wb").write(mem)
    with open(os.path.join(outdir, "rof_blocks.txt"), "w") as f:
        for s, e in merged:
            f.write(f"{s:04X} {e:04X}\n")

    print(f"INITAD (entry) = ${initad:04X}" if initad else "INITAD = (none)")
    print(f"RUNAD          = ${runad:04X}" if runad else "RUNAD  = (none)")
    print(f"wrote {outdir}/rof_mem.bin (64K) and rof_blocks.txt "
          f"({len(merged)} merged RAM blocks):")
    for s, e in merged:
        print(f"  ${s:04X}-${e:04X}  ({e - s + 1} bytes)")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rof.xex",
         sys.argv[2] if len(sys.argv) > 2 else "disasm")
