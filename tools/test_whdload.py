#!/usr/bin/env python3
"""Run isolated WHDLoad tests and retain its own core dumps under build/.

Source amiga/env.sh first. ROMs and original data are local inputs, never shipped.
The quit mode requires a FORCE_QUIT=<vbl> executable; timed mode runs production.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', choices=('quit', 'timed'), default='timed')
    p.add_argument('--whdload', type=Path, default=os.environ.get('WHDLOAD_BINARY')
                   or Path.home()/'.local/share/amiga/WHDLoad/C/WHDLoad')
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--rtb', type=Path, required=True)
    p.add_argument('--logo-phase', choices=('initial', 'games'),
                   help='require the enhanced logo bitmap in the chip-memory dump')
    p.add_argument('--elf', type=Path, help='matching ELF for runtime enhancement checks')
    p.add_argument('--exe', type=Path, default=ROOT/'amiga/out/RoF')
    p.add_argument('--seconds', type=int, default=90, help='host safety ceiling')
    p.add_argument('--ticks', type=int, help='WHDLoad timeout in PAL fields (timed: 1500; quit: disabled)')
    p.add_argument('--cpu', default='68020')
    p.add_argument('--workbench', type=Path, required=True)
    p.add_argument('--cartridge', type=Path, default=ROOT/'rof.rom')
    p.add_argument('--no-preload', action='store_true')
    for option in (2, 3, 4):
        p.add_argument(f'--custom{option}', type=int, choices=(0, 1), default=0)
    args = p.parse_args()
    if args.logo_phase and not args.custom4:
        p.error('--logo-phase requires --custom4 1')
    if args.ticks is None:
        args.ticks = 1500 if args.mode == 'timed' else 0
    (ROOT/'build').mkdir(exist_ok=True)
    base = Path(tempfile.mkdtemp(prefix='whdload-test-', dir=ROOT/'build'))
    print('Fixture:', base, flush=True)
    boot, game = base/'boot', base/'game'
    for d in (boot/'s', boot/'devs/Kickstarts', game/'data', base/'state'):
        d.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.whdload, game/'WHDLoad')
    shutil.copyfile(ROOT/'whdload/RoF.slave', game/'RoF.slave')
    shutil.copyfile(args.rom, boot/'devs/Kickstarts/kick34005.A500')
    shutil.copyfile(args.rtb, boot/'devs/Kickstarts/kick34005.A500.RTB')
    shutil.copyfile(args.exe, game/'data/RoF')
    shutil.copyfile(args.cartridge, game/'data/rof.rom')
    (boot/'s/WHDLoad.prefs').write_text('Expert\nReadDelay=0\n')
    preload = '' if args.no_preload else 'PRELOAD '
    custom = f'CUSTOM2={args.custom2} CUSTOM3={args.custom3} CUSTOM4={args.custom4} '
    (boot/'s/startup-sequence').write_text(
        'DF0:C/Assign C: DF0:C\nDF0:C/Assign LIBS: DF0:Libs\n'
        'DF0:C/Assign DEVS: DH0:devs\nStack 16384\nFailAt 999\n'
        f'CD DH1:\nWHDLoad RoF.slave {preload}{custom}SPLASHDELAY=0 NOREQ COREDUMP FILELOG TIMEOUT={args.ticks} >DH0:result\n'
        'If WARN\nEcho failed >DH0:failed\nElse\nEcho passed >DH0:passed\nEndIf\n')
    with (base/'emulator.log').open('w') as log:
        emu = subprocess.Popen(['fs-uae', '--amiga_model=A1200', '--cpu='+args.cpu,
            '--uae_cpu_model='+args.cpu, '--uae_cpu_24bit_addressing=false',
            '--jit_compiler=0', '--chip_memory=2048', '--fast_memory=8192',
            '--kickstart_file='+os.environ['KICKSTART'],
            '--hard_drive_0='+str(boot), '--hard_drive_0_priority=10', '--hard_drive_1='+str(game),
            '--floppy_drive_0='+str(args.workbench),
            '--joystick_port_0=mouse', '--joystick_port_1=nothing', '--warp_mode=1', '--fullscreen=0',
            '--window_width=720', '--window_height=568', '--state_dir='+str(base/'state')], stdout=log, stderr=log)
        try:
            deadline = time.monotonic()+args.seconds
            while time.monotonic()<deadline and not any((boot/n).exists() for n in ('passed','failed')):
                if emu.poll() is not None:
                    raise RuntimeError('FS-UAE exited unexpectedly')
                time.sleep(.25)
            output = (boot/'result').read_text(errors='replace') if (boot/'result').exists() else ''
            report = (game/'.whdl_register').read_text(encoding='latin1') if (game/'.whdl_register').exists() else ''
            assert report, f'No WHDLoad core dump: {base}\n{output}'
            if args.mode == 'timed':
                assert 'DEBUG caused.' in report, report + output
                files = (game/'.whdl_log').read_text(encoding='latin1')
                reads = [line for line in files.splitlines() if '[ReadOff]' in line and 'name=rof.rom ' in line]
                assert len(reads) == 3, files
                memory = (game/'.whdl_expmem').read_bytes()
                descriptor = memory.find(b'RoF!DATA')
                assert descriptor >= 0 and memory[descriptor+12:descriptor+16] == b'RDF!', 'Game data was not marked ready'
                for magic, wanted in ((b'RoF!TERR', args.custom2),
                                      (b'RoF!EPAL', args.custom3),
                                      (b'RoF!EGRF', args.custom4)):
                    offset = memory.find(magic)
                    assert offset >= 0, f'Missing configuration block: {magic}'
                    assert int.from_bytes(memory[offset+8:offset+10], 'big') == wanted, magic
                if args.elf:
                    from audit_release_data import elf_symbols
                    from verify_rom_recipe import COCKPIT_SHA256
                    symbols, _ = elf_symbols(args.elf)
                    expmem_base = int(re.search(r'ExpMem\s+([0-9A-F]+)', report)[1], 16)
                    package = struct.unpack_from('>I', memory, descriptor + 16)[0]

                    def runtime_offset(name):
                        section, address = symbols[name]
                        assert section == '.bss', (name, section)
                        return package - expmem_base + address - symbols['rof_game_data'][1]

                    for name, wanted in (('g_flightEnhancedTerrain', args.custom2),
                                         ('g_enhancedPalette', args.custom3),
                                         ('g_enhancedGraphics', args.custom4)):
                        if name not in symbols and wanted == 0:
                            continue  # graphics may be compiled out of the default build
                        value = struct.unpack_from('>I', memory, runtime_offset(name))[0]
                        assert value == wanted, (name, value, wanted)
                    if 'rof_cockpit_planar' in symbols:
                        offset = runtime_offset('rof_cockpit_planar')
                        atlas = memory[offset:offset + 17952]
                        assert len(atlas) == 17952, 'Cockpit atlas outside memory dump'
                        if args.custom4:
                            assert hashlib.sha256(atlas).hexdigest() == COCKPIT_SHA256
                        else:
                            assert not any(atlas), 'Disabled cockpit atlas was built'
                    print('PASS: runtime enhancement settings and cockpit atlas')
                assert "Prg 'RoF'" in report, 'Timeout did not reach the game: ' + report
                if args.logo_phase:
                    expected = bytearray((ROOT/'whdload/assets/enhanced_logo.bin').read_bytes())
                    if args.logo_phase == 'games':
                        overlay = (ROOT/'whdload/assets/enhanced_games.bin').read_bytes()
                        for span in range(120):
                            offset = 65 * 160 + 15 + span * 40
                            expected[offset:offset + 11] = overlay[span * 11:(span + 1) * 11]
                    chip = (game/'.whdl_memory').read_bytes()
                    assert expected in chip, f'Enhanced {args.logo_phase} logo not rendered'
                    print(f'PASS: rendered enhanced {args.logo_phase} logo matches all 16000 bytes')
                print('PASS: three cartridge ranges loaded, data validated, execution reached RoF; core retained')
            else:
                assert (boot/'passed').exists() and 'Return OK.' in report, report + output
                print(f'PASS: {args.mode} slave returned normally; WHDLoad core saved')
        finally:
            emu.terminate()
            try: emu.wait(timeout=5)
            except subprocess.TimeoutExpired: emu.kill(); emu.wait()

if __name__ == '__main__':
    main()
