# -*- coding: utf-8 -*-
"""Длинный прогон в ОДНОМ процессе: N шагов одного вида, snap каждые EVERY шагов, частная память на каждом
snap (--shot-keys=@файл). Вывод: кривая и вердикт «плато» (рост последней половины < 64 КБ) или «рост».
    python tools/uitest/leak.py [N] [EVERY]"""
import os, re, subprocess, sys
import shot

N = int(sys.argv[1]) if len(sys.argv) > 1 else 12000
EVERY = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
PRE = 'size:120x34,click:3:7,wait:50,click:20:6,wait:100'
KINDS = {
    'наведение в одну клетку': ['hover:60:12'],
    'наведение 2 клетки': ['hover:60:12', 'hover:61:12'],
    'Up/Down в каналах': ['up', 'down'],
    'щелчки в фон шапки': ['click:70:2', 'click:71:2'],
    'переключение каналов': ['click:20:6', 'click:20:8'],
}


def one(name, cyc):
    steps = PRE.split(',')
    for i in range(N):
        steps.append(cyc[i % len(cyc)])
        if (i + 1) % EVERY == 0:
            steps.append('snap')
    sc = os.path.join(shot.OUT, 'leak_keys.txt')
    with open(sc, 'w', encoding='utf-8') as f:
        f.write('\n'.join(steps))
    path = os.path.join(shot.OUT, 'leak.bmp')
    r = subprocess.run([shot.EXE, '--demo', '--shot=' + path, '--shot-keys=@' + sc], capture_output=True, timeout=1800)
    out = r.stdout.decode('utf-8', 'replace')
    mem = [int(m) for m in re.findall(r'частная память (\d+) КБ', out)]
    for f in os.listdir(shot.OUT):
        if f.startswith('leak') and (f.endswith('.bmp') or f.endswith('.txt')):
            os.remove(os.path.join(shot.OUT, f))
    snaps = mem[:-1]
    half = snaps[len(snaps) // 2:]
    tail = half[-1] - half[0] if len(half) > 1 else 0
    verdict = 'плато' if tail < 64 else 'РОСТ'
    print('%-26s %s; вторая половина %+d КБ -> %s' % (name, ' '.join(str(m) for m in snaps), tail, verdict))
    return verdict


if __name__ == '__main__':
    print('шагов %d, snap каждые %d, частная память КБ на каждом snap' % (N, EVERY))
    for k, c in KINDS.items():
        one(k, c)
