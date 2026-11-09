# -*- coding: utf-8 -*-
"""Память простоя ПАРОЙ с эталоном (правило 09.11): эталон build-ref/astra-ds.exe (не пересобирается, кладёт
ревьюер) и проверяемая сборка (MC_BIN или build-test2) — в одном прогоне, замеры чередуются (эталон, сборка, ...),
по 5 каждого; size:140x40, wait:2000; канал — ПО ИМЕНИ. Отчёт — разница медиан к эталону.
Порог: ±30 КБ шум, рост < 40 КБ — не сигнал. Абсолютные числа меняются с системой (09.11 вся линейка +300 КБ
после смены системных DLL), поэтому опорных абсолютных значений больше нет.
    python tools/uitest/idle_mem.py              — простой и «с картинками» (#музыка)
    python tools/uitest/idle_mem.py каналы       — каналы general/новости/музыка/игры, пара с эталоном
Эталон: MC_REF (по умолчанию E:/projects/mincord/build-ref/astra-ds.exe)."""
import os, statistics, sys
import shot
import windows as W

SIZE = '140x40'
S = 'click:3:7,wait:50'
REF = os.environ.get('MC_REF', os.path.join(shot.ROOT, 'build-ref', 'astra-ds.exe'))
CUR = shot.EXE


def use(exe):
    shot.EXE = exe


def keys_for(channel):
    use(CUR)                                     # координаты — по проверяемой сборке (раскладка одна)
    return S + ',click:' + W.at(S, channel, size=SIZE, tol=0) + ',wait:2000'


def measure(keys, n=5):
    """Совместимость (одна сборка): n замеров текущей shot.EXE."""
    v = [shot.run(keys, 'idle', SIZE).info['mem_kb'] for _ in range(n)]
    return v, statistics.median(v)


def pair(keys, n=5):
    r, c = [], []
    for _ in range(n):
        use(REF); r.append(shot.run(keys, 'idle', SIZE).info['mem_kb'])
        use(CUR); c.append(shot.run(keys, 'idle', SIZE).info['mem_kb'])
    use(CUR)
    return r, c


def report(name, keys):
    r, c = pair(keys)
    mr, mc = statistics.median(r), statistics.median(c)
    d = mc - mr
    sig = d >= 40
    print('  %-24s эталон %s (медиана %d) | сборка %s (медиана %d) | разница %+d КБ -> %s'
          % (name, r, mr, c, mc, d, 'РОСТ (сигнал)' if sig else 'норма'))
    return sig


def main():
    if not os.path.exists(REF):
        print('нет эталона %s (кладёт ревьюер)' % REF)
        return 2
    print('эталон: %s\nсборка: %s' % (REF, CUR))
    bad = 0
    if len(sys.argv) > 1 and sys.argv[1] == 'каналы':
        for ch in ('general', 'новости', 'музыка', 'игры'):
            bad += report('#' + ch, keys_for(ch))
    else:
        bad += report('простой', 'wait:2000')
        bad += report('с картинками (#музыка)', keys_for('музыка'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
