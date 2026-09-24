#!/usr/bin/env python3
"""Run isolated WHDLoad tests and retain its own core dumps under build/.

Source amiga/env.sh first. ROMs and original data are local inputs, never shipped.
The quit mode requires a FORCE_QUIT=<vbl> executable; timed mode runs production.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', choices=('quit', 'timed'), default='timed')
    p.add_argument('--whdload', type=Path, default=Path.home()/'.local/share/amiga/WHDLoad/C/WHDLoad')
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--rtb', type=Path, required=True)
    p.add_argument('--exe', type=Path, default=ROOT/'amiga/out/RoF')
    p.add_argument('--seconds', type=int, default=90, help='host safety ceiling')
    p.add_argument('--ticks', type=int, help='WHDLoad timeout in PAL fields (timed: 1500; quit: disabled)')
    p.add_argument('--cpu', default='68020')
    p.add_argument('--workbench', type=Path, required=True)
    p.add_argument('--cartridge', type=Path, default=ROOT/'rof.rom')
    p.add_argument('--no-preload', action='store_true')
    args = p.parse_args()
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
    (boot/'s/startup-sequence').write_text(
        'DF0:C/Assign C: DF0:C\nDF0:C/Assign LIBS: DF0:Libs\n'
        'DF0:C/Assign DEVS: DH0:devs\nStack 16384\nFailAt 999\n'
        f'CD DH1:\nWHDLoad RoF.slave {preload}SPLASHDELAY=0 NOREQ COREDUMP FILELOG TIMEOUT={args.ticks} >DH0:result\n'
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
                assert "Prg 'RoF'" in report, 'Timeout did not reach the game: ' + report
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
