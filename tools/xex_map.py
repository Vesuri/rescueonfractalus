#!/usr/bin/env python3
"""Parse an Atari 8-bit XEX (binary load) file and print its segment map.

XEX format:
  - File begins with $FFFF magic (may repeat before any segment).
  - Each segment: start addr (2 bytes LE), end addr (2 bytes LE),
    then (end - start + 1) bytes of data loaded at [start..end].
  - A segment whose [start..end] points at hardware/OS addresses is a
    "poke" performed during load (e.g. $02E0/$02E2 RUN/INIT vectors,
    $D301 PIA PORTB bank switching, $0244 COLDST).
"""
import sys

# Known Atari 8-bit addresses worth flagging in the map.
NOTABLE = {
    0x02E0: "RUNAD  (run address vector)",
    0x02E2: "INITAD (init address vector, runs after this segment loads)",
    0x0244: "COLDST (coldstart flag)",
    0x03F8: "page-3 (cassette/loader area)",
    0xD301: "PIA PORTB (XL/XE bank switching / OS-ROM enable)",
}

def region(addr):
    if 0x0000 <= addr <= 0x00FF: return "zero page"
    if 0x0100 <= addr <= 0x01FF: return "stack"
    if 0x0200 <= addr <= 0x03FF: return "OS RAM / page 2-3"
    if 0xD000 <= addr <= 0xD0FF: return "GTIA"
    if 0xD200 <= addr <= 0xD2FF: return "POKEY"
    if 0xD300 <= addr <= 0xD3FF: return "PIA"
    if 0xD400 <= addr <= 0xD4FF: return "ANTIC"
    if 0xD500 <= addr <= 0xD7FF: return "cart/PBI"
    if 0xC000 <= addr <= 0xFFFF: return "OS ROM area / high RAM"
    return "RAM"

def main(path):
    data = open(path, "rb").read()
    n = len(data)
    p = 0
    def u16(i): return data[i] | (data[i+1] << 8)

    if n < 2 or u16(0) != 0xFFFF:
        print("Not a XEX (missing $FFFF magic)"); return
    print(f"File: {path}  ({n} bytes)\n")
    print(f"{'#':>3} {'start':>6} {'end':>6} {'len':>6}  region / note")
    print("-" * 64)

    p = 2
    seg = 0
    first_code_addr = None
    while p + 4 <= n:
        # Optional repeated $FFFF magic between segments.
        if u16(p) == 0xFFFF:
            p += 2
            if p + 4 > n: break
        start = u16(p); end = u16(p+2); p += 4
        length = end - start + 1
        body = data[p:p+length]; p += length
        note = region(start)
        for a in (start, end):
            if a in NOTABLE:
                note += f"  [{NOTABLE[a]}]"
        kind = "POKE" if length <= 2 and start >= 0x0200 and (start in NOTABLE or start >= 0xC000) else "DATA"
        # First sizeable RAM segment is almost certainly code/data payload.
        if first_code_addr is None and length > 2 and start < 0xC000:
            first_code_addr = start
        seg += 1
        preview = body[:6].hex(' ')
        print(f"{seg:>3} ${start:04X} ${end:04X} {length:>6}  {note:<32} {preview}")

    print("-" * 64)
    print(f"segments: {seg}   first payload load addr: "
          f"${first_code_addr:04X}" if first_code_addr else "none")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rof.xex")
