#!/usr/bin/env python3
"""Parse an atari-dl-analyzer capture (DLIST + per-scanline GTIA sweep) into a
clear, region-by-region description of what an Atari ANTIC display list does:
the scanline ranges, the screen mode + screen-RAM (LMS) of each region, and the
live GTIA colour / player / missile / priority state at those scanlines (i.e. the
*result* of the DLIs).  Optionally disassembles a DLI handler to show which memory
each colour write is sourced from (immediate vs mem[zp]) — the bit you need to
decide whether an Amiga copper slot is a baked constant or a per-frame poke.

This output is NOT a copper list; it is a faithful behavioural spec of the DL that
you then translate into the appropriate Amiga CopperList / poke code.

Usage:
  dl_report.py <capture.log>                      # DLIST structure + GTIA-sweep regions
  dl_report.py <capture.log> --ram <ram.bin>      # also resolve mem[] values cited by DLIs
  dl_report.py --ram <ram.bin> --dl <hexaddr>     # STATIC: walk the DL bytes (no emulator)
  dl_report.py --ram <ram.bin> --dli <hexaddr>    # disassemble one DLI handler -> reg writes

See SKILL.md for the capture step (dl_capture.sh).
"""
import re, sys

# ANTIC mode -> scanlines per row.  0 = blank/jump (handled separately).
MODE_H = {0x2:8,0x3:10,0x4:8,0x5:16,0x6:8,0x7:16,
          0x8:8,0x9:4,0xA:4,0xB:2,0xC:1,0xD:2,0xE:1,0xF:1}

# Atari hue nibble -> rough name, for human-readable colour annotation.
HUE = {0x0:"grey",0x1:"gold",0x2:"orange",0x3:"red-orange",0x4:"pink",0x5:"purple",
       0x6:"blue-purple",0x7:"blue",0x8:"blue",0x9:"blue",0xA:"turquoise",0xB:"cyan",
       0xC:"green",0xD:"yellow-green",0xE:"orange-green",0xF:"orange"}

def colname(v):
    return f"${v:02X}({HUE.get(v>>4,'?')}{'' if (v&0xf) else ' dark'})"

# ---- GTIA register names we surface, in report order ----
GTIA_REGS = ["COLBK","COLPF0","COLPF1","COLPF2","COLPF3",
             "COLPM0","COLPM1","COLPM2","COLPM3",
             "GRAFM","PRIOR","HPOSM0","HPOSM1","HPOSM2","HPOSM3","HPOSP0","HPOSP1"]
# Registers whose change starts a new "region" in the collapsed sweep.
SIG_REGS = ["COLBK","COLPF0","COLPF1","COLPF2","COLPF3",
            "COLPM0","COLPM1","COLPM2","COLPM3","GRAFM","PRIOR"]


def parse_gtia_sweep(text):
    """Return {scanline:int -> {REG:val}} from the ==Lnnn== / GTIA blocks."""
    out = {}
    cur = None
    for line in text.splitlines():
        m = re.search(r"==L(\d+)==", line)
        if m:
            cur = int(m.group(1)); out.setdefault(cur, {})
            continue
        if cur is None:
            continue
        for k, v in re.findall(r"([A-Z]+\d?)=\s*([0-9A-Fa-f]{2})", line):
            out[cur][k] = int(v, 16)
    return {k: v for k, v in out.items() if v}


def collapse(sweep):
    """Collapse consecutive scanlines with identical SIG_REGS into regions."""
    regions = []
    for y in sorted(sweep):
        sig = tuple(sweep[y].get(r) for r in SIG_REGS)
        if regions and regions[-1][2] == sig and y == regions[-1][1] + 1:
            regions[-1][1] = y
        else:
            regions.append([y, y, sig, sweep[y]])
    return regions


def parse_dlist(text):
    """Parse atari800 'DLIST' output -> list of region dicts with relative scanlines."""
    rows = []
    y = 0
    started = False
    for line in text.splitlines():
        m = re.match(r"^\s*([0-9A-Fa-f]{4}):\s+(.*)$", line)
        if not m:
            continue
        addr = int(m.group(1), 16); body = m.group(2).strip()
        rep = 1
        rm = re.match(r"(\d+)x\s+(.*)", body)
        if rm:
            rep = int(rm.group(1)); body = rm.group(2)
        dli = "DLI" in body
        lms = None
        lm = re.search(r"LMS\s+([0-9A-Fa-f]{4})", body)
        if lm:
            lms = int(lm.group(1), 16)
        if "JVB" in body or ("JMP" in body):
            jm = re.search(r"(?:JVB|JMP)\s+([0-9A-Fa-f]{4})", body)
            rows.append(dict(addr=addr, kind="JUMP", target=int(jm.group(1),16) if jm else None,
                             y0=y, lines=0, dli=dli, lms=lms))
            started = True
            continue
        bm = re.search(r"(\d+)\s+BLANK", body)
        if bm:
            h = int(bm.group(1)) * rep
            rows.append(dict(addr=addr, kind="BLANK", y0=y, lines=h, dli=dli, lms=None))
            y += h; started = True
            continue
        mm = re.search(r"MODE\s+([0-9A-Fa-f])", body)
        if mm:
            mode = int(mm.group(1), 16); h = MODE_H.get(mode, 1) * rep
            rows.append(dict(addr=addr, kind="MODE", mode=mode, rows=rep, y0=y,
                             lines=h, dli=dli, lms=lms))
            y += h; started = True
    return rows


# ---- minimal 6502 disassembler for DLI handlers ----
_OPLEN = {0xA9:2,0xA5:2,0xB5:2,0xAD:3,0xBD:3,0xB9:3,0xA2:2,0xA0:2,0xA6:2,0xA4:2,0xAE:3,0xAC:3,
          0x8D:3,0x9D:3,0x99:3,0x85:2,0x95:2,0x86:2,0x96:2,0x84:2,0x94:2,0x8E:3,0x8C:3,
          0x48:1,0x68:1,0x40:1,0x60:1,0xEA:1,0x18:1,0x38:1,0x78:1,0x58:1,0xAA:1,0xA8:1,0x8A:1,
          0x98:1,0xBA:1,0x9A:1,0xC9:2,0xC5:2,0xE0:2,0xC0:2,0xF0:2,0xD0:2,0xB0:2,0x90:2,0x10:2,
          0x30:2,0x50:2,0x70:2,0x4C:3,0x20:3,0x6C:3,0x29:2,0x09:2,0x49:2,0x69:2,0xE9:2,0x0A:1,
          0x4A:1,0x2A:1,0x6A:1,0xCA:1,0x88:1,0xE8:1,0xC8:1,0x24:2,0x2C:3,0x46:2,0x06:2,0x4E:3}
_NM = {0xA9:"LDA#",0xA5:"LDA",0xB5:"LDA",0xAD:"LDA",0xBD:"LDA,X",0xB9:"LDA,Y",0xA2:"LDX#",
       0xA0:"LDY#",0xA6:"LDX",0xA4:"LDY",0xAE:"LDX",0xAC:"LDY",0x8D:"STA",0x9D:"STA,X",
       0x99:"STA,Y",0x85:"STA",0x95:"STA,X",0x86:"STX",0x84:"STY",0x8E:"STX",0x8C:"STY",
       0x48:"PHA",0x68:"PLA",0x40:"RTI",0x60:"RTS",0x4C:"JMP",0x20:"JSR",0x6C:"JMP()",
       0x29:"AND#",0x09:"ORA#",0x49:"EOR#",0xAA:"TAX",0xA8:"TAY",0x8A:"TXA",0x98:"TYA"}
_GTIA = {0x12:"COLPM0",0x13:"COLPM1",0x14:"COLPM2",0x15:"COLPM3",0x16:"COLPF0",0x17:"COLPF1",
         0x18:"COLPF2",0x19:"COLPF3",0x1A:"COLBK",0x00:"HPOSP0",0x01:"HPOSP1",0x02:"HPOSP2",
         0x03:"HPOSP3",0x04:"HPOSM0",0x05:"HPOSM1",0x06:"HPOSM2",0x07:"HPOSM3",0x08:"SIZEP0",
         0x0C:"SIZEM",0x0D:"GRAFP0",0x11:"GRAFM",0x1B:"PRIOR",0x1D:"GRACTL"}


def disasm_dli(ram, addr, limit=48):
    """Disassemble a DLI handler from `addr`; return (text, register-writes list).
    Tracks the source of the value in A (immediate or a mem load) so each STA to a
    GTIA register reports whether it is a hard constant (#$imm -> bake on the Amiga)
    or sourced from mem[] (-> per-frame copper poke; ramping fades come from here).
    Follows a single JMP (the $4A05-style dispatcher tail is reported, not chased)."""
    lines, writes = [], []
    p = addr; n = 0
    src = None  # ("imm",v) | ("mem",addr) — provenance of the accumulator
    LOAD_IMM = {0xA9}
    LOAD_MEM = {0xA5: 2, 0xAD: 3}  # LDA zp / LDA abs
    while n < limit:
        op = ram[p]; ln = _OPLEN.get(op, 1); b = ram[p:p+ln]
        operand = ""
        tgt = None
        if ln == 2:
            operand = f"${b[1]:02X}"
        if ln == 3:
            tgt = b[1] | (b[2] << 8); operand = f"${tgt:04X}"
        if op in LOAD_IMM:
            src = ("imm", b[1])
        elif op in LOAD_MEM:
            src = ("mem", b[1] if ln == 2 else tgt)
        note = ""
        if op in (0x8D, 0x9D, 0x99) and tgt is not None and 0xD000 <= tgt <= 0xD01F:
            reg = _GTIA.get(tgt & 0x1F, f"D0{tgt&0x1F:02X}")
            if src and src[0] == "imm":
                desc = f"#${src[1]:02X}"
            elif src and src[0] == "mem":
                cur = f" (=${ram[src[1]]:02X})" if ram is not None else ""
                desc = f"mem[${src[1]:04X}]{cur}"
            else:
                desc = "<reg/computed>"
            note = f"  <= {reg} = {desc}"
            writes.append((reg, desc))
        lines.append(f"  ${p:04X}: {_NM.get(op, f'?{op:02X}'):6} {operand:7}{note}")
        if op in (0x40, 0x60):  # RTI/RTS
            break
        if op == 0x4C:  # JMP — report and stop (dispatcher tail / chain)
            lines.append(f"  -> JMP ${tgt:04X} (dispatcher/chain; analyze separately if needed)")
            break
        p += ln; n += 1
    return "\n".join(lines), writes


def report_capture(log_path, ram=None):
    text = open(log_path, encoding="utf-8", errors="replace").read()
    dl = parse_dlist(text)
    sweep = parse_gtia_sweep(text)

    print("=" * 72)
    print("DISPLAY-LIST STRUCTURE (from DLIST; scanlines are RELATIVE to DL start)")
    print("=" * 72)
    if not dl:
        print("  (no DLIST block found in capture)")
    for r in dl:
        tag = "DLI " if r["dli"] else "    "
        if r["kind"] == "BLANK":
            print(f"  y+{r['y0']:>3}..{r['y0']+r['lines']-1:>3}  {tag}{r['lines']} blank")
        elif r["kind"] == "JUMP":
            print(f"  @${r['addr']:04X}      {tag}JUMP -> ${r['target']:04X}")
        else:
            lms = f" LMS ${r['lms']:04X}" if r["lms"] is not None else ""
            print(f"  y+{r['y0']:>3}..{r['y0']+r['lines']-1:>3}  {tag}MODE {r['mode']:X} x{r['rows']}{lms}")

    print()
    print("=" * 72)
    print("LIVE GTIA STATE BY SCANLINE (absolute; collapsed into constant-state runs)")
    print("  -> this is the RESULT of the DLIs: what colour/missile/priority is active")
    print("=" * 72)
    if not sweep:
        print("  (no GTIA sweep found in capture)")
    for y0, y1, _sig, regs in collapse(sweep):
        span = f"y{y0}" if y0 == y1 else f"y{y0}-{y1}"
        cols = " ".join(f"{r}={colname(regs[r])}" for r in ["COLBK","COLPF0","COLPF1","COLPF2","COLPF3"] if r in regs)
        print(f"\n  [{span}]  {cols}")
        pm = " ".join(f"{r}={colname(regs[r])}" for r in ["COLPM0","COLPM1","COLPM2","COLPM3"] if r in regs)
        extra = []
        if regs.get("GRAFM"): extra.append(f"GRAFM=${regs['GRAFM']:02X}(missiles on)")
        if "PRIOR" in regs:
            p = regs["PRIOR"]; extra.append(f"PRIOR=${p:02X}{' 5thPlayer' if p&0x10 else ''}")
        for h in ["HPOSM0","HPOSM1","HPOSM2","HPOSM3"]:
            if regs.get(h): extra.append(f"{h}=${regs[h]:02X}")
        if pm:    print(f"            players: {pm}")
        if extra: print(f"            {'  '.join(extra)}")


def report_static_dl(ram, addr):
    """Walk DL bytes directly from RAM (no emulator)."""
    print("=" * 72)
    print(f"STATIC DISPLAY-LIST WALK from ${addr:04X} (scanlines RELATIVE to DL start)")
    print("=" * 72)
    p = addr; y = 0; guard = 0
    while guard < 512:
        guard += 1
        op = ram[p]; mode = op & 0x0F; dli = "DLI " if (op & 0x80) else "    "
        if mode == 0:
            n = ((op >> 4) & 7) + 1
            print(f"  y+{y:>3}..{y+n-1:>3}  {dli}{n} blank")
            y += n; p += 1; continue
        if mode == 1:  # jump
            tgt = ram[p+1] | (ram[p+2] << 8)
            jvb = "JVB(wait vblank)" if (op & 0x40) else "JMP"
            print(f"  @${p:04X}      {dli}{jvb} -> ${tgt:04X}")
            if op & 0x40:
                break
            p = tgt; continue
        lms = ""
        adv = 1
        if op & 0x40:  # LMS
            tgt = ram[p+1] | (ram[p+2] << 8); lms = f" LMS ${tgt:04X}"; adv = 3
        h = MODE_H.get(mode, 1)
        flags = ("HSCROLL " if op & 0x10 else "") + ("VSCROLL " if op & 0x20 else "")
        print(f"  y+{y:>3}..{y+h-1:>3}  {dli}MODE {mode:X}{lms} {flags}")
        y += h; p += adv


def load_ram(path):
    raw = open(path, "rb").read()
    if path.endswith(".a8s"):
        import gzip
        if raw[:2] == b"\x1f\x8b":
            raw = gzip.decompress(raw)
        sig = bytes([0xAE,0x3C,0x07,0xE8,0xBD,0xDB,0x71]); pos = raw.find(sig)
        if pos < 0:
            raise SystemExit(f"{path}: ROM anchor not found")
        raw = raw[pos-0x7148:pos-0x7148+0x10000]
    return raw


def main(argv):
    args = {}
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--"):
            args[a[2:]] = argv[i+1]; i += 2
        else:
            pos.append(a); i += 1
    ram = load_ram(args["ram"]) if "ram" in args else None

    if "dl" in args:
        report_static_dl(ram, int(args["dl"], 16)); return
    if "dli" in args:
        txt, writes = disasm_dli(ram, int(args["dli"], 16))
        print(f"DLI handler ${int(args['dli'],16):04X}:\n{txt}")
        if writes:
            print("\n  register writes (constant #$.. = bake on Amiga; mem[..] = per-frame poke):")
            for reg, desc in writes:
                print(f"    {reg} = {desc}")
        return
    if not pos:
        raise SystemExit(__doc__)
    report_capture(pos[0], ram)


if __name__ == "__main__":
    main(sys.argv[1:])
