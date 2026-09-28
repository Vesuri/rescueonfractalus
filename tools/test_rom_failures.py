#!/usr/bin/env python3
"""Check ROM startup failures under AmigaDOS using local ROM/Workbench inputs.

Source amiga/env.sh first and build the default release executable.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workbench', type=Path, required=True)
    args = parser.parse_args()
    (ROOT / 'build').mkdir(exist_ok=True)
    base = Path(tempfile.mkdtemp(prefix='rom-startup-test-', dir=ROOT / 'build'))
    print('Fixture:', base, flush=True)
    boot, game = base / 'boot', base / 'game'
    (boot / 's').mkdir(parents=True)
    rom = (ROOT / 'rof.rom').read_bytes()
    cases = {
        'missing': None,
        'short': rom[:-1],
        'oversized': rom + b'\0',
        'corrupt': bytes([rom[0] ^ 1]) + rom[1:],
    }
    commands = [
        'DF0:C/Assign C: DF0:C', 'DF0:C/Assign LIBS: DF0:Libs',
        'Stack 16384', 'FailAt 999',
    ]
    for name, data in cases.items():
        folder = game / name
        (folder / 'data').mkdir(parents=True)
        (folder / 'RoF').write_bytes((ROOT / 'amiga/out/RoF').read_bytes())
        if data is not None:
            (folder / 'data/rof.rom').write_bytes(data)
        commands += [
            f'CD DH1:{name}', f'RoF >DH0:{name}.log', 'If FAIL',
            f'Echo rejected >DH0:{name}.result', 'Else',
            f'Echo accepted >DH0:{name}.result', 'EndIf',
        ]
    commands += ['Echo done >DH0:done']
    (boot / 's/startup-sequence').write_text('\n'.join(commands) + '\n')
    with (base / 'emulator.log').open('w') as log:
        emu = subprocess.Popen([
            'fs-uae', '--amiga_model=A500+', '--chip_memory=1024',
            '--fast_memory=8192', '--kickstart_file=' + os.environ['KICKSTART'],
            '--hard_drive_0_priority=10', '--hard_drive_0=' + str(boot),
            '--hard_drive_1=' + str(game), '--floppy_drive_0=' + str(args.workbench),
            '--warp_mode=1', '--fullscreen=0', '--state_dir=' + str(base / 'state'),
        ], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 50
            while not (boot / 'done').exists() and time.monotonic() < deadline:
                if emu.poll() is not None:
                    raise RuntimeError(f'Emulator exited unexpectedly: {base}')
                time.sleep(.25)
            assert (boot / 'done').exists(), f'Test timed out: {base}'
            for name in cases:
                assert (boot / f'{name}.result').read_text().strip() == 'rejected', name
                assert 'cannot load cartridge data' in (boot / f'{name}.log').read_text(), name
                print(f'PASS: {name} ROM exits with FAIL and a diagnostic', flush=True)
        finally:
            emu.terminate()
            try:
                emu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                emu.kill()
                emu.wait()


if __name__ == '__main__':
    main()
