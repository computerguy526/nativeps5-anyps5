#!/usr/bin/env python3
"""Differential check for KK patch 0009 (no zero initializer on predeclared MSL SSA values).

  kk_msl_zero_init_check.py OLD.msl NEW.msl [--compile]

OLD is the MSL of KosmicKrisp without the patch (or KK_MSL_ZERO_INIT_SSA=1), NEW with it. The only allowed
difference: a line `<ws>T tN = T(0);` in OLD is `<ws>T tN;` in NEW (SSA temporaries; r-registers keep theirs).
With --compile both are built by `xcrun metal -c` and the times are printed. Exit 0 = identical modulo that.
"""
import re, subprocess, sys, time

INIT = re.compile(r'^(\s+[A-Za-z0-9_]+ t\d+) = [A-Za-z0-9_]+\(0\);$')

def normalise_old(text):
    return '\n'.join(INIT.sub(r'\1;', l) for l in text.split('\n'))

def check(old, new):
    o = normalise_old(old).split('\n')
    n = new.split('\n')
    if len(o) != len(n):
        return 'line count differs: %d vs %d' % (len(o), len(n))
    for i, (a, b) in enumerate(zip(o, n)):
        if a != b:
            return 'line %d differs:\n  old: %s\n  new: %s' % (i + 1, a, b)
    if any(INIT.match(l) for l in n):
        return 'NEW still has a zero initializer on an SSA temporary'
    return None

def compile_time(path):
    t = time.time()
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-x', 'metal', '-std=metal3.2', path, '-o', '/dev/null'], check=True, capture_output=True)
    return time.time() - t

def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    if len(args) != 2:
        print(__doc__); return 2
    old = open(args[0]).read(); new = open(args[1]).read()
    problem = check(old, new)
    if problem:
        print('FAIL', problem); return 1
    print('ok: NEW == OLD without SSA zero initializers (%d bytes -> %d)' % (len(old), len(new)))
    if '--compile' in argv:
        print('compile old %.1f s, new %.1f s' % (compile_time(args[0]), compile_time(args[1])))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
