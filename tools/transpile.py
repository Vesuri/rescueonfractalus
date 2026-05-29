#!/usr/bin/env python3
"""Transpile the Ghidra 6502 disassembly listing to C.

Reads  disasm/listing.txt  +  disasm/symbols.csv
Writes src/gen/rof_gen.c       (one C function per 6502 routine)
       src/gen/rof_decl.h      (forward declarations)
       src/gen/rof_manual.c    (hand-written replacements for SMC routines)

Design
------
* Each 6502 routine becomes a void C function.
* JSR  → direct function call (always static; no indirect JMPs exist).
* RTS  → return;
* RTI  → platform_rti(); return;
* Branches (BEQ etc.) → if (flag) goto L_xxxx;
* JMP  within same function → goto L_xxxx;
* JMP  to a different function → callee(); return;   (tail call)
* Stack, flags, registers modelled via cpu.h macros.
* Hardware addresses ($D000-$D7FF) → bus_read/bus_write.
* All other addresses → mem[] direct.
* ZP-indexed wraps using (uint8_t) cast.
* Known self-modifying function screen_page_swap ($1A62) is skipped
  and provided hand-written in rof_manual.c.
"""
import re
import sys
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).parent.parent
LISTING  = ROOT / "disasm/listing.txt"
SYM_CSV  = ROOT / "disasm/symbols.csv"
OUT_C    = ROOT / "src/gen/rof_gen.c"
OUT_H    = ROOT / "src/gen/rof_decl.h"
OUT_MAN  = ROOT / "src/gen/rof_manual.c"

# Self-modifying functions: skip in generated code, provide manual impl.
MANUAL_FUNCS = {0x1a62}   # screen_page_swap

HW_BASE, HW_END = 0xD000, 0xD800   # bus_read/bus_write range

# ---------------------------------------------------------------------------
# Parse symbols.csv → addr_int → name
# ---------------------------------------------------------------------------
def load_symbols(path):
    sym = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'): continue
        parts = line.split(',', 4)
        if len(parts) < 2: continue
        addr_s = parts[0].strip().lstrip('$')
        name   = parts[1].strip()
        try: sym[int(addr_s, 16)] = name
        except ValueError: pass
    return sym

# ---------------------------------------------------------------------------
# Parse listing into a list of functions, each with instructions.
# Returns:
#   funcs: list of {start, end, name, insns: [{addr, bytes, mnem, op, raw}]}
#   func_by_addr: addr → function index
# ---------------------------------------------------------------------------
def parse_listing(path, symbols):
    funcs = []
    func_ranges = []   # (start, end) from function-summary header

    # Pass 1: collect function ranges from the header summary.
    with open(path) as f:
        for line in f:
            m = re.match(r'^; FUNC (\S+)\s+([0-9a-f]+) - ([0-9a-f]+)', line)
            if m:
                name  = m.group(1)
                start = int(m.group(2), 16)
                end   = int(m.group(3), 16)
                func_ranges.append((start, end, name))

    # Build addr → (start, end, name) lookup.
    func_ranges.sort()
    func_start_set = {r[0] for r in func_ranges}
    func_info = {r[0]: r for r in func_ranges}

    # Pass 2: parse instructions, grouped into functions by address range.
    insn_re = re.compile(
        r'^([0-9a-f]{4})\s+((?:[0-9A-F]{2} )*[0-9A-F]{2})\s+'
        r'([A-Z]{2,3})\s*(.*?)\s*$'
    )

    current_func = None
    func_insns   = {}   # start_addr → list of insn dicts

    with open(path) as f:
        for line in f:
            m = insn_re.match(line)
            if not m: continue
            addr  = int(m.group(1), 16)
            bs    = bytes(int(x, 16) for x in m.group(2).split())
            mnem  = m.group(3)
            op    = m.group(4).strip()

            # Determine enclosing function by address range.
            fn_start = None
            for (s, e, n) in func_ranges:
                if s <= addr <= e:
                    fn_start = s
                    break
            if fn_start is None:
                continue

            if fn_start not in func_insns:
                func_insns[fn_start] = []
            func_insns[fn_start].append({
                'addr': addr, 'bytes': bs,
                'mnem': mnem, 'op': op,
            })

    # Assemble final list in address order.
    for (start, end, name) in func_ranges:
        insns = func_insns.get(start, [])
        if not insns: continue
        # Override name from symbols if present.
        final_name = symbols.get(start, name)
        funcs.append({'start': start, 'end': end,
                      'name': final_name, 'insns': insns})

    # func_by_addr: addr → function dict
    func_by_addr = {}
    for f in funcs:
        for ins in f['insns']:
            func_by_addr[ins['addr']] = f

    return funcs, func_by_addr

# ---------------------------------------------------------------------------
# Operand → C expression
# ---------------------------------------------------------------------------
def is_hw(addr):
    return HW_BASE <= addr < HW_END

def addr_read(addr):
    if is_hw(addr):
        return f'bus_read(0x{addr:04X})'
    return f'mem[0x{addr:04X}]'

def addr_write(addr, val):
    if is_hw(addr):
        return f'bus_write(0x{addr:04X}, {val})'
    return f'mem[0x{addr:04X}] = {val}'

def parse_operand(op, nbytes, symbols):
    """Return (mode, addr_or_imm, index) where mode is one of:
       imm, zp, abs, zpx, zpy, absx, absy, indy, indx, acc, impl
    """
    op = op.strip()
    if not op:
        return ('impl', 0, None)

    # Immediate: #0x12
    m = re.match(r'^#0x([0-9a-fA-F]+)$', op)
    if m:
        return ('imm', int(m.group(1), 16), None)

    # (zp),Y
    m = re.match(r'^\(0x([0-9a-fA-F]+)\),Y$', op)
    if m:
        return ('indy', int(m.group(1), 16), 'Y')

    # (zp,X)
    m = re.match(r'^\(0x([0-9a-fA-F]+),X\)$', op)
    if m:
        return ('indx', int(m.group(1), 16), 'X')

    # abs,X or abs,Y or zp,X or zp,Y
    m = re.match(r'^0x([0-9a-fA-F]+),([XY])$', op)
    if m:
        addr = int(m.group(1), 16)
        idx  = m.group(2)
        mode = ('zpx' if nbytes == 2 and idx=='X' else
                'zpy' if nbytes == 2 and idx=='Y' else
                'absx' if idx=='X' else 'absy')
        return (mode, addr, idx)

    # abs or zp (bare address)
    m = re.match(r'^0x([0-9a-fA-F]+)$', op)
    if m:
        addr = int(m.group(1), 16)
        mode = 'zp' if nbytes == 2 else 'abs'
        return (mode, addr, None)

    # Accumulator (e.g. "ASL A" in some disassemblers)
    if op == 'A':
        return ('acc', 0, None)

    return ('impl', 0, None)

def operand_read(mode, addr, idx):
    """C expression that reads the source value."""
    if mode == 'imm':   return f'0x{addr:02X}'
    if mode in ('zp','abs'):
        return addr_read(addr)
    if mode == 'zpx':   return f'mem[(uint8_t)(0x{addr:02X}+cpu.X)]'
    if mode == 'zpy':   return f'mem[(uint8_t)(0x{addr:02X}+cpu.Y)]'
    if mode == 'absx':  return addr_read(addr) if not is_hw(addr) else f'bus_read(0x{addr:04X}+cpu.X)'
    # For abs,X with non-HW: mem[0xXXXX + cpu.X] — no wrapping for abs indexed
    if mode == 'absx':  return f'mem[0x{addr:04X}+cpu.X]'
    if mode == 'absy':  return f'mem[0x{addr:04X}+cpu.Y]'
    if mode == 'indy':  return f'bus_read(ZP_IND_Y(0x{addr:02X}))'
    if mode == 'indx':  return f'bus_read(ZP_IND_X(0x{addr:02X}))'
    return '0'

def operand_read_fixed(mode, addr, idx):
    """Like operand_read but non-HW abs uses mem[] directly.
    Parenthesise the hex constant to avoid the C preprocessor E+/P+
    tokenisation quirk (pp-numbers include 'E+' and 'E-' sequences)."""
    if mode == 'absx':  return f'mem[(0x{addr:04X})+cpu.X]'
    if mode == 'absy':  return f'mem[(0x{addr:04X})+cpu.Y]'
    return operand_read(mode, addr, idx)

def operand_addr_expr(mode, addr, idx):
    """C expression giving the effective address (for write targets).
    Parenthesise hex constants to avoid the C preprocessor E+/P+ quirk."""
    if mode in ('zp','abs'):  return f'0x{addr:04X}'
    if mode == 'zpx':  return f'(uint8_t)((0x{addr:02X})+cpu.X)'
    if mode == 'zpy':  return f'(uint8_t)((0x{addr:02X})+cpu.Y)'
    if mode == 'absx': return f'(0x{addr:04X})+cpu.X'
    if mode == 'absy': return f'(0x{addr:04X})+cpu.Y'
    if mode == 'indy': return f'ZP_IND_Y(0x{addr:02X})'
    if mode == 'indx': return f'ZP_IND_X(0x{addr:02X})'
    return '0'

def write_expr(mode, addr, idx, val_expr):
    ea = operand_addr_expr(mode, addr, idx)
    if mode in ('zp','abs') and is_hw(addr):
        return f'bus_write(0x{addr:04X}, {val_expr})'
    if mode in ('zp','abs'):
        return f'mem[0x{addr:04X}] = {val_expr}'
    if mode in ('absx','absy','zpx','zpy'):
        return f'mem[{ea}] = {val_expr}'
    # indirect modes
    return f'bus_write({ea}, {val_expr})'

# ---------------------------------------------------------------------------
# Translate one instruction to C statement(s)
# ---------------------------------------------------------------------------
BRANCH_FLAGS = {
    'BEQ': 'cpu.Z', 'BNE': '!cpu.Z',
    'BCS': 'cpu.C', 'BCC': '!cpu.C',
    'BMI': 'cpu.N', 'BPL': '!cpu.N',
    'BVS': 'cpu.V', 'BVC': '!cpu.V',
}

def translate_insn(insn, func, all_funcs_by_start, symbols, local_targets,
                   external_entries=None, wrapper_names=None):
    """external_entries: {addr → container_func} from main pass-1 analysis.
    wrapper_names:      {addr → C name} for mid-function entry wrappers."""
    if external_entries is None:  external_entries = {}
    if wrapper_names   is None:  wrapper_names    = {}

    addr  = insn['addr']
    mnem  = insn['mnem']
    op    = insn['op']
    nbytes = len(insn['bytes'])
    mode, val, idx = parse_operand(op, nbytes, symbols)

    lines = [f'    /* {addr:04x} */']

    def resolve_target_name(target):
        """Return the C name to call for a branch/JMP to target.
        Checks: local function start, wrapper, symbol, fallback."""
        if target in wrapper_names:
            return wrapper_names[target]
        if target in all_funcs_by_start:
            return all_funcs_by_start[target]['name']
        return symbols.get(target, f'FUN_{target:04x}')

    # --- Branches ---
    if mnem in BRANCH_FLAGS:
        target = val
        flag   = BRANCH_FLAGS[mnem]
        if target in local_targets:
            lines.append(f'    if ({flag}) goto L_{target:04x};')
        else:
            # Cross-function branch → conditional tail call.
            name = resolve_target_name(target)
            lines.append(f'    if ({flag}) {{ {name}(); return; }}')
        return lines

    # --- JMP ---
    if mnem == 'JMP':
        target = val
        if target in local_targets:
            lines.append(f'    goto L_{target:04x};')
        else:
            name = resolve_target_name(target)
            lines.append(f'    {name}(); return;')
        return lines

    # --- JSR ---
    if mnem == 'JSR':
        target = val
        name = resolve_target_name(target)
        lines.append(f'    {name}();')
        return lines

    # --- RTS / RTI ---
    if mnem == 'RTS':
        lines.append('    return;')
        return lines
    if mnem == 'RTI':
        lines.append('    PLP(); return;')
        return lines

    # --- NOP / BRK ---
    if mnem == 'NOP':
        lines.append('    NOP();')
        return lines
    if mnem == 'BRK':
        lines.append('    /* BRK: software interrupt — ignored in C translation */;')
        return lines

    # --- Flag ops ---
    simple_flag = {'CLC':'CLC()','SEC':'SEC()','CLI':'CLI()','SEI':'SEI()',
                   'CLD':'CLD()','SED':'SED()','CLV':'CLV()'}
    if mnem in simple_flag:
        lines.append(f'    {simple_flag[mnem]};')
        return lines

    # --- Stack ---
    if mnem == 'PHA': lines.append('    PHA();'); return lines
    if mnem == 'PLA': lines.append('    PLA();'); return lines
    if mnem == 'PHP': lines.append('    PHP();'); return lines
    if mnem == 'PLP': lines.append('    PLP();'); return lines

    # --- Transfer ---
    for mn, mac in [('TAX','TAX()'),('TAY','TAY()'),('TXA','TXA()'),('TYA','TYA()'),
                    ('TSX','TSX()'),('TXS','TXS()'),
                    ('INX','INX()'),('INY','INY()'),('DEX','DEX()'),('DEY','DEY()')]:
        if mnem == mn:
            lines.append(f'    {mac};')
            return lines

    # --- Load ---
    if mnem in ('LDA','LDX','LDY'):
        reg = mnem[2]  # A, X, or Y
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    LD{reg}({src});')
        return lines

    # --- Store ---
    if mnem in ('STA','STX','STY'):
        reg = mnem[2]
        stmt = write_expr(mode, val, idx, f'cpu.{reg}')
        lines.append(f'    {stmt};')
        return lines

    # --- ADC / SBC ---
    if mnem == 'ADC':
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    ADC({src});')
        return lines
    if mnem == 'SBC':
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    SBC({src});')
        return lines

    # --- Compare ---
    cmp_map = {'CMP': 'CMP', 'CPX': 'CPX', 'CPY': 'CPY'}
    if mnem in cmp_map:
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    {cmp_map[mnem]}({src});')
        return lines

    # --- Logical ---
    for mn, mac in [('AND','AND'),('ORA','ORA'),('EOR','EOR')]:
        if mnem == mn:
            src = operand_read_fixed(mode, val, idx)
            lines.append(f'    {mac}({src});')
            return lines

    # --- BIT ---
    if mnem == 'BIT':
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    BIT({src});')
        return lines

    # --- INC / DEC memory ---
    if mnem == 'INC':
        ea = operand_addr_expr(mode, val, idx)
        lines.append(f'    INC_M({ea});')
        return lines
    if mnem == 'DEC':
        ea = operand_addr_expr(mode, val, idx)
        lines.append(f'    DEC_M({ea});')
        return lines

    # --- Shift / Rotate ---
    if mnem == 'ASL':
        if mode == 'impl' or mode == 'acc':
            lines.append('    ASL_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    ASL_M({ea});')
        return lines
    if mnem == 'LSR':
        if mode == 'impl' or mode == 'acc':
            lines.append('    LSR_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    LSR_M({ea});')
        return lines
    if mnem == 'ROL':
        if mode == 'impl' or mode == 'acc':
            lines.append('    ROL_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    ROL_M({ea});')
        return lines
    if mnem == 'ROR':
        if mode == 'impl' or mode == 'acc':
            lines.append('    ROR_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    ROR_M({ea});')
        return lines

    # --- Unknown ---
    lines.append(f'    /* TODO: {mnem} {op} */')
    return lines

# ---------------------------------------------------------------------------
# Translate one function
# ---------------------------------------------------------------------------
def translate_func(func, all_funcs_by_start, symbols,
                   external_entry_labels=None,
                   external_entries=None, wrapper_names=None):
    """external_entry_labels: addresses within this function needing L_ labels.
    external_entries / wrapper_names: passed through to translate_insn."""
    start = func['start']
    name  = func['name']
    insns = func['insns']
    if external_entry_labels is None: external_entry_labels = set()
    if external_entries       is None: external_entries      = {}
    if wrapper_names           is None: wrapper_names         = {}

    if start in MANUAL_FUNCS:
        return [f'/* {name} @ ${start:04X}: manual implementation in rof_manual.c */']

    func_end = func['end']
    local_targets = set()
    for insn in insns:
        mnem, op, nbytes = insn['mnem'], insn['op'], len(insn['bytes'])
        if mnem in BRANCH_FLAGS or mnem == 'JMP':
            mode, val, idx = parse_operand(op, nbytes, symbols)
            if start <= val <= func_end:
                local_targets.add(val)

    lines = [f'void {name}(void) {{']
    for insn in insns:
        addr = insn['addr']
        if addr in local_targets or addr in external_entry_labels:
            lines.append(f'L_{addr:04x}:;')
        stmt_lines = translate_insn(insn, func, all_funcs_by_start, symbols,
                                    local_targets, external_entries, wrapper_names)
        lines.extend(stmt_lines)
    lines.append('}')
    lines.append('')
    return lines

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def find_containing_func(addr, funcs):
    """Return the function whose address range contains addr, or None."""
    for f in funcs:
        if f['start'] <= addr <= f['end']:
            return f
    return None

def main():
    symbols = load_symbols(SYM_CSV)
    funcs, func_by_addr = parse_listing(LISTING, symbols)
    funcs_by_start = {f['start']: f for f in funcs}

    print(f'Parsed {len(funcs)} functions, '
          f'{sum(len(f["insns"]) for f in funcs)} instructions')

    # -----------------------------------------------------------------------
    # Pass 1: collect cross-function branch/JMP targets.
    # For each branch/JMP whose target T is NOT in the current function's
    # address range but IS within another function's range (and not at that
    # function's start), record T as an "external entry" needing a wrapper.
    # -----------------------------------------------------------------------
    # func_addr_ranges: list of (start, end) for range containment tests.
    func_addr_ranges = [(f['start'], f['end'], f) for f in funcs]

    # external_entries: target_addr → containing_func
    external_entries = {}
    # external_labels_for_func: func_start → set of addr needing L_xxxx labels
    external_labels_for_func = defaultdict(set)

    for func in funcs:
        for insn in func['insns']:
            mnem, op, nbytes = insn['mnem'], insn['op'], len(insn['bytes'])
            if mnem not in BRANCH_FLAGS and mnem not in ('JMP', 'JSR'):
                continue
            mode, val, _ = parse_operand(op, nbytes, symbols)
            if val == 0:
                continue
            # Is target within THIS function's range?
            if func['start'] <= val <= func['end']:
                continue
            # Target is outside. Find which function contains it.
            container = find_containing_func(val, funcs)
            if container is None:
                continue
            if val == container['start']:
                continue  # it's a normal tail call to another function's start
            # Mid-function entry: needs a wrapper and a label in the container.
            external_entries[val] = container
            external_labels_for_func[container['start']].add(val)

    wrapper_names = {}  # target_addr → wrapper function name
    for addr, container in external_entries.items():
        wname = symbols.get(addr, f'FUN_{addr:04x}')
        wrapper_names[addr] = wname

    # -----------------------------------------------------------------------
    # Collect all branch/JMP/JSR targets that fall in no function range.
    # Ghidra sometimes misses functions for gap addresses. Generate stubs.
    # -----------------------------------------------------------------------
    jsr_targets_unknown = set()
    all_call_mnems = set(BRANCH_FLAGS.keys()) | {'JMP', 'JSR'}
    for func in funcs:
        for insn in func['insns']:
            if insn['mnem'] not in all_call_mnems: continue
            _, val, _ = parse_operand(insn['op'], len(insn['bytes']), symbols)
            if val == 0: continue
            if val in funcs_by_start: continue
            if val in external_entries: continue
            if val in wrapper_names: continue
            # Check if it falls in any function range
            if find_containing_func(val, funcs) is not None: continue
            jsr_targets_unknown.add(val)

    # -----------------------------------------------------------------------
    # Forward declarations header — includes wrapper function names.
    # -----------------------------------------------------------------------
    decl_lines = [
        '#ifndef ROF_DECL_H', '#define ROF_DECL_H',
        '/* Auto-generated by tools/transpile.py — do not edit */',
        '#include <stdint.h>', '',
        '/* Forward declarations for all 6502 routines */',
    ]
    for f in funcs:
        decl_lines.append(f'void {f["name"]}(void);')
    # Wrappers for mid-function entry points.
    decl_lines.append('')
    decl_lines.append('/* Wrappers for cross-function branch/JMP entry points */')
    for addr, wname in sorted(wrapper_names.items()):
        decl_lines.append(f'void {wname}(void);')
    # Stubs for unlisted JSR targets.
    decl_lines.append('')
    decl_lines.append('/* Stubs for JSR targets without a Ghidra-detected function */')
    for addr in sorted(jsr_targets_unknown):
        name = symbols.get(addr, f'FUN_{addr:04x}')
        decl_lines.append(f'void {name}(void);')
    decl_lines += ['', '#endif /* ROF_DECL_H */']
    OUT_H.write_text('\n'.join(decl_lines) + '\n')
    print(f'Wrote {OUT_H}')

    # -----------------------------------------------------------------------
    # Pass 2: generate C.
    # -----------------------------------------------------------------------
    header = [
        '/* Auto-generated by tools/transpile.py — do not edit */',
        '#include "../cpu/cpu.h"',
        '#include "../cpu/bus.h"',
        '#include "rof_decl.h"',
        '',
    ]
    body = []
    for f in funcs:
        ext_labels = external_labels_for_func.get(f['start'], set())
        body.extend(translate_func(f, funcs_by_start, symbols,
                                   external_entry_labels=ext_labels,
                                   external_entries=external_entries,
                                   wrapper_names=wrapper_names))

    # Stubs for JSR targets Ghidra didn't create functions for.
    if jsr_targets_unknown:
        body.append('/* === Stubs for JSR targets without a known function === */')
        body.append('/* TODO: investigate each — may be data misidentified as code. */')
        for addr in sorted(jsr_targets_unknown):
            name = symbols.get(addr, f'FUN_{addr:04x}')
            body.append(f'void {name}(void) {{ /* stub: no instructions found at ${addr:04X} */ }}')
        body.append('')

    # Emit wrappers for mid-function entry points.
    body.append('/* === Wrappers for cross-function branch/JMP entry points === */')
    body.append('/* FIXME Phase3: these enter functions mid-body — split target functions. */')
    for addr in sorted(external_entries):
        container = external_entries[addr]
        wname     = wrapper_names[addr]
        cname     = container['name']
        body.append(f'void {wname}(void) {{')
        body.append(f'    /* TODO: should start execution at ${addr:04X} inside {cname}.')
        body.append(f'       Currently approximated as a full call from ${container["start"]:04X}. */')
        body.append(f'    {cname}();')
        body.append(f'}}')
        body.append('')

    OUT_C.write_text('\n'.join(header + body) + '\n')
    print(f'Wrote {OUT_C}  ({len(header)+len(body)} lines)')

    # -----------------------------------------------------------------------
    # Manual implementations stub (only written once).
    # -----------------------------------------------------------------------
    if not OUT_MAN.exists():
        manual = [
            '/* Hand-written implementations for self-modifying / special-case routines.',
            '   These are NOT auto-generated; edit this file freely. */',
            '#include "../cpu/cpu.h"',
            '#include "../cpu/bus.h"',
            '#include "rof_decl.h"',
            '#include <string.h>',
            '',
            '/* screen_page_swap ($1A62): swaps 5 x 256-byte pages between $40xx and $06xx.',
            '   The original code is self-modifying: it patches the high bytes of its own',
            '   LDA/STA instructions to cycle through pages $40-$44 and $06-$0A.',
            '   The semantics are straightforward so we translate the intent directly. */',
            'void screen_page_swap(void) {',
            '    int page;',
            '    for (page = 0; page < 5; page++) {',
            '        uint8_t *a = mem + ((0x40 + page) << 8);',
            '        uint8_t *b = mem + ((0x06 + page) << 8);',
            '        uint8_t tmp[256];',
            '        memcpy(tmp, a,   256);',
            '        memcpy(a,   b,   256);',
            '        memcpy(b,   tmp, 256);',
            '    }',
            '}',
        ]
        OUT_MAN.write_text('\n'.join(manual) + '\n')
        print(f'Wrote {OUT_MAN}  (manual stubs)')
    else:
        print(f'Skipped {OUT_MAN}  (already exists)')

if __name__ == '__main__':
    main()
