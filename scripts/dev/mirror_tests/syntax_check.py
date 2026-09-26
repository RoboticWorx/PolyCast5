"""Run the real ESP-IDF cross-compiler over changed sources with -fsyntax-only.

Uses each file's own command line out of build/compile_commands.json, so the include
paths, defines and sdkconfig are exactly what a real build would use. Produces no
artifacts and does not touch the build tree.
"""

import io
import json
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..')).replace(chr(92), '/')
SP = os.environ.get('MIRROR_TEST_OUT') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '.out')
os.makedirs(SP, exist_ok=True)
DB = os.path.join(REPO, 'build', 'compile_commands.json')

TARGETS = [
    'components/mirror/src/mirror_capture.c',
    'components/mirror/src/mirror_encode.c',
    'components/mirror/src/mirror_task.c',
    'components/mirror/src/mirror_quality.c',
    'components/lcd/src/lcd_mirror_page.c',
    'components/lcd/src/lcd_utils.c',
    'components/lcd/src/lcd_task.c',
    'components/lcd/src/lcd_text_input.c',
    'components/lcd/src/lcd_wifi.c',
    'components/gpio/src/gpio_utils.c',
    'components/lcd/src/lcd_ir_exp.c',
    'components/lcd/src/lcd_ir_exp_render.c',
    'components/mirror/src/mirror_thermal.c',
    'components/gpio/src/gpio_remote.c',
]

# Repo-relative paths on the command line replace the default list
if len(sys.argv) > 1:
    TARGETS = [a.replace(chr(92), '/') for a in sys.argv[1:]]

if not os.path.exists(DB):
    print('skipped: build/compile_commands.json not found (configure the project first)')
    sys.exit(0)

db = json.load(io.open(DB, encoding='utf-8'))
index = {}
for e in db:
    index[e['file'].replace(chr(92), '/').lower()] = e

fails = 0
missing = []
borrowed = []


def sibling(key):
    # A source newer than the configure. Components glob src/*.c and compile every file
    # in one with the same flags, so a neighbour's command line stands in for its own
    d = key.rsplit('/', 1)[0] + '/'
    for k in sorted(index):
        if k.startswith(d) and k.endswith('.c') and '/' not in k[len(d):]:
            return index[k]
    return None


for t in TARGETS:
    key = (REPO + '/' + t).lower()
    e = index.get(key)
    via = None

    if e is None:
        e = sibling(key)
        if e is None:
            missing.append(t)
            continue
        via = e['file']
        borrowed.append(t)

    cmd = e['command']
    if via is not None:
        src = (REPO + '/' + t).replace('/', chr(92))
        cmd = re.sub(re.escape(via), lambda m: src, cmd, flags=re.IGNORECASE)
    cmd = re.sub(r'\s-o\s+\S+', ' ', cmd)
    cmd = re.sub(r'\s-c\s+', ' -fsyntax-only ', cmd)
    # The feature ships switched off, which would compile every guarded source to nothing
    cmd = cmd.replace(' -fsyntax-only ', ' -DPOLYCAST5_EN_SCREEN_MIRROR=1 -fsyntax-only ', 1)

    bat = os.path.join(SP, '_sc.bat')
    io.open(bat, 'w', encoding='utf-8', newline='\r\n').write(
        '@echo off\r\ncd /d ' + e['directory'].replace('/', chr(92)) + '\r\n' + cmd + '\r\n'
    )

    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    out = (p.stdout + p.stderr)
    out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out)
    out = '\n'.join(l for l in out.splitlines()
                    if 'volume label' not in l and l.strip())

    if p.returncode != 0:
        fails += 1
        print('FAIL  ' + t)
        print('\n'.join('      ' + l for l in out.splitlines()[:14]))
    else:
        warn = [l for l in out.splitlines() if 'warning:' in l]
        print(('WARN  ' if warn else 'ok    ') + t + (
            ' (%d warning%s)' % (len(warn), '' if len(warn) == 1 else 's') if warn else '') + (
            ' [flags of %s]' % os.path.basename(via.replace(chr(92), '/')) if via else ''))
        for l in warn[:6]:
            print('      ' + l)

if borrowed:
    print('\nNot in compile_commands.json yet, so checked with a neighbour\'s flags. A CMake')
    print('re-run (idf.py reconfigure, or any build) is also what adds them to the firmware:')
    for t in borrowed:
        print('      ' + t)

if missing:
    print('\nNOT CHECKED - not in compile_commands.json, the configure predates them:')
    for t in missing:
        print('      ' + t)
    print('    Run a build to reconfigure, then re-run this. A skipped file is not a passing one.')

if fails:
    print('\n%d FILE(S) FAILED' % fails)
elif missing:
    print('\nINCOMPLETE: %d checked, %d not checked' % (len(TARGETS) - len(missing), len(missing)))
else:
    print('\nALL FILES COMPILE')

sys.exit(1 if (fails or missing) else 0)
