#!/usr/bin/env python3
"""Run the release Install script under the real Installer 43 in FS-UAE, never on user drawers.

    . amiga/env.sh && python3 tools/test_release_installer.py

Covers how the script finds the game executable: beside it (the full archive), asked for when
absent (the whdload.de archive), asked for again when the one beside it is the wrong version,
and a cancelled request. Wrong versions are a real 1.2 executable (taken from
dist/RoF-1.2.lha when present), a synthetic newer one and a file with no $VER: at all, so the
check is shown to reject older, newer and unversioned files.

Only the dialogues are replaced: each file request returns the next scripted answer and records
its prompt; the informational messages, the destination request and the ReadMe viewer are
skipped. Everything else, including every version check, is the shipped script.
"""
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
import _amigaxdev  # noqa: F401
from amigaxdev.env import path as shared_path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'whdload/RoF Install'
EXE = ROOT / 'amiga/out/RoF'
FSUAE = os.environ.get('FSUAE') or shared_path('FSUAE_ARM')
VERSION = (ROOT / 'VERSION').read_text().strip()


def replace_form(text, start, replacement):
    a = text.index(start)
    depth, quoted, i = 0, False, a
    while i < len(text):
        c = text[i]
        if quoted and c == '\\':
            i += 2
            continue
        if c == '"':
            quoted = not quoted
        elif not quoted:
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return text[:a] + replacement + text[i + 1:]
        i += 1
    raise ValueError('Unbalanced Installer form: ' + start)


def script(answers):
    s = (PACKAGE / 'Install').read_bytes().decode('latin-1')
    picks = ' '.join(f'"{a}"' for a in answers)
    # Installer's textfile hangs when its file exists, so every record gets its own file.
    s = replace_form(s, '(procedure P_SelectFile', f'''(procedure P_SelectFile
  (set #SF_n (+ #SF_n 1))
  (textfile (dest ("DH2:ask%ld" #SF_n)) (append #SF_prompt))
  (set #SF_filename (select (- #SF_n 1) {picks}))
)''')
    # Installer shows a startup requester unless the script mentions (welcome) somewhere.
    s = '(if 0 (welcome))\n(set #SF_n 0)\n' + s
    s = replace_form(s, '(if (< @installer-version', '(set #newstuff 0)')
    n = 0
    while '(message' in s:
        n += 1
        s = replace_form(s, '(message', f'(textfile (dest "DH2:message-{n}") (append "shown"))')
    s = replace_form(s, '(set #dest\n  (askdir', '(set #dest "DH2:out")')
    s = replace_form(s, '(abort "Installation cancelled: the Rescue on Fractalus! executable',
                     '((textfile (dest "DH2:cancelled") (append "yes")) (exit (quiet)))')
    s = s.replace('SYS:Utilities/Multiview', 'Execute DH0:viewer')
    s = replace_form(s, '(exit)', '(exit (quiet))')
    return s.encode('latin-1')


def fixtures(base):
    exe = EXE.read_bytes()
    marker = f'Fractalus! {VERSION} ('.encode()
    assert exe.count(marker) == 1, 'cannot find the executable $VER:'
    major, minor = VERSION.split('.')
    newer = f'Fractalus! {major}.{int(minor) + 1} ('.encode()
    files = {'good/RoF': exe, 'newer': exe.replace(marker, newer),
             'unversioned': (PACKAGE / 'ReadMe').read_bytes(), 'rom.bin': bytes(65536)}
    old = ROOT / 'dist/RoF-1.2.lha'
    if old.is_file():
        files['older'] = subprocess.run(['lha', 'pq', str(old), 'RoF Install/RoF'],
                                        check=True, capture_output=True).stdout
        assert b'Fractalus! 1.2 (' in files['older']
    else:
        files['older'] = exe.replace(marker, f'Fractalus! {major}.{int(minor) - 1} ('.encode())
    for name, data in files.items():
        (base / name).parent.mkdir(parents=True, exist_ok=True)
        (base / name).write_bytes(data)


CASES = {
    # name: (RoF beside the script, file answers, expected number of executable requests)
    'full': ('good', ['DH2:rom.bin'], 0),
    'whdload': (None, ['DH2:older', 'DH2:newer', 'DH2:unversioned', 'DH2:good/RoF', 'DH2:rom.bin'], 4),
    'stale': ('older', ['DH2:good/RoF', 'DH2:rom.bin'], 1),
    'cancel': (None, [''], 1),
}


def run_case(name, beside, answers, exe_asks):
    (ROOT / 'tmp').mkdir(exist_ok=True)
    base = Path(tempfile.mkdtemp(prefix=f'installer-{name}-', dir=ROOT / 'tmp'))
    boot = base / 'boot'
    for p in (boot / 's', boot / 'devs/Kickstarts', base / 'state'):
        p.mkdir(parents=True, exist_ok=True)
    fixtures(base)
    for member in ('Install.info', 'ReadMe', 'ReadMe.info', 'RoF.inf'):
        shutil.copyfile(PACKAGE / member, boot / member)
    shutil.copyfile(ROOT / 'whdload/RoF.slave', boot / 'RoF.slave')
    shutil.copyfile(shared_path('INSTALLER43'), boot / 'Installer')
    shutil.copyfile(shared_path('WHDLOAD_BINARY'), boot / 'WHDLoad')
    if beside:
        shutil.copyfile(base / ('good/RoF' if beside == 'good' else beside), boot / 'RoF')
    (boot / 'devs/Kickstarts/kick34005.A500').touch()
    (boot / 'Install').write_bytes(script(answers))
    (boot / 'viewer').write_text('.key FILE/A\nQuit 0\n')
    (boot / 's/startup-sequence').write_text(
        # ENV:/ENVARC: must exist, or the script's getenv/setenv wait for the volume for ever.
        'DF0:C/Assign C: DF0:C\nDF0:C/Assign LIBS: DF0:Libs\nDF0:C/Assign DEVS: DH0:devs\n'
        'MakeDir RAM:ENV RAM:ENVARC\nAssign ENV: RAM:ENV\nAssign ENVARC: RAM:ENVARC\n'
        'CD DH0:\nStack 16384\nPath DH0: ADD\nC:LoadWB\n'
        'Installer SCRIPT DH0:Install APPNAME "Rescue on Fractalus!" MINUSER NOVICE DEFUSER NOVICE '
        'LOGFILE DH2:installer.log NOPRETEND >DH2:console.log\nEcho done >DH2:finished\n')
    with (base / 'emulator.log').open('w') as log:
        emu = subprocess.Popen([FSUAE, '--amiga_model=A1200', '--chip_memory=2048', '--fast_memory=8192',
                                '--kickstart_file=' + shared_path('KICKSTART'),
                                '--hard_drive_0=' + str(boot), '--hard_drive_0_priority=10',
                                '--hard_drive_2=' + str(base), '--floppy_drive_0=' + shared_path('WORKBENCH_ADF'),
                                '--warp_mode=1', '--fullscreen=0', '--state_dir=' + str(base / 'state')],
                               stdout=log, stderr=log, env=dict(os.environ, SDL_AUDIODRIVER='dummy'))
        try:
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and not (base / 'finished').exists():
                if emu.poll() is not None:
                    raise RuntimeError('emulator exited: ' + str(base))
                time.sleep(.25)
            assert (base / 'finished').exists(), f'Installer did not finish: {base}'
        finally:
            emu.terminate()
            try:
                emu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                emu.kill()
                emu.wait()
    prompts = [(base / f'ask{i}').read_text(errors='replace') for i in range(1, len(answers) + 1)
               if (base / f'ask{i}').exists()]
    exe_prompts = [p for p in prompts if 'executable' in p]
    assert len(exe_prompts) == exe_asks, f'{name}: {len(exe_prompts)} executable requests, expected {exe_asks}: {base}'
    data = base / 'out/RoF/data'
    if name == 'cancel':
        assert (base / 'cancelled').exists(), f'cancel was not taken: {base}'
        assert not (data / 'RoF').exists()
    else:
        assert len(prompts) == len(answers), f'{name}: {len(prompts)} requests, expected {len(answers)}: {base}'
        assert (data / 'RoF').read_bytes() == EXE.read_bytes(), f'{name}: wrong executable installed: {base}'
        assert (data / 'rof.rom').read_bytes() == bytes(65536)
        assert (base / 'out/RoF/RoF.slave').exists() and (base / 'out/RoF/RoF.info').exists()
    # The first request explains where the executable comes from; every later one (and a
    # stale executable beside the script) names the rejected file and the required version.
    if beside is None:
        assert 'It is not included in this install archive' in exe_prompts[0]
    for prompt in exe_prompts[1 if beside is None else 0:]:
        assert 'is not the Rescue on Fractalus! ' + VERSION in prompt, f'{name}: {prompt!r}'
    shutil.rmtree(base)
    print(f'PASS: Installer {name}: {len(exe_prompts)} executable request(s), '
          + ('cancelled cleanly' if name == 'cancel' else 'installed the matching executable'), flush=True)


def main():
    for name, case in CASES.items():
        run_case(name, *case)


if __name__ == '__main__':
    main()
