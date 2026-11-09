# -*- coding: utf-8 -*-
"""Серия для правки рендера звука: N прогонов audio_check (по S секунд) на сборке, сводка; плюс CPU простоя без звонка.
    python tools/uitest/audio_series.py [N=5] [S=15]
Сборка — MC_BIN (по умолчанию build-test2/astra-ds.exe; база для сравнения — build-test2/astra-ds-base.exe).
AUDIO_MIC=1 — с петлёй микрофона (как в разговоре). Звук только в память, файлов нет."""
import os, statistics, subprocess, sys, threading, time
import numpy as np
import shot
import audio_check as A
import windows as W


def one(secs):
    """Один прогон: (пропуски, щелчки, секунд тона, CPU процесса %)."""
    S = 'click:3:7,wait:50'
    v = S + ',click:' + W.at(S, '<) ОБЩИЙ', dx=3) + ',wait:300'
    mic = os.environ.get('AUDIO_MIC') == '1'
    j = v + ',click:' + W.at(v, 'Присоединиться') + ',wait:800' + ('' if mic else ',f5') + ',wait:%d' % (secs * 1000 + 2000)
    sc = os.path.join(shot.OUT, 'audio_keys.txt')
    with open(sc, 'w', encoding='utf-8') as f:
        f.write('size:120x34,' + j)
    out, stop = {}, threading.Event()
    th = threading.Thread(target=A.loopback, args=(secs + 2, stop, out))
    th.start()
    p = subprocess.Popen([shot.EXE, '--demo'] + ([] if mic else ['--ptt']) + ['--shot=' + os.path.join(shot.OUT, 'audio.bmp'),
                         '--shot-keys=@' + sc], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=shot.child_env())
    time.sleep(3)
    t1, c1 = time.time(), A.thread_times(p.pid)
    time.sleep(5)
    t2, c2 = time.time(), A.thread_times(p.pid)
    p.wait(timeout=secs + 30)
    th.join()
    cpu = sum(max(0, c2[k] - c1.get(k, 0)) for k in c2) / ((t2 - t1) * 1e7) * 100
    a = A.analyze(out.get('frames', np.zeros(0)), out.get('rate', 48000)) or {}
    if not a.get('tone'):
        return None, None, 0, cpu
    return a['gaps'], a['clicks'], a['sec'], cpu


def idle_cpu(secs=10):
    """CPU процесса без звонка (главный экран, простой): ожидается 0 %."""
    sc = os.path.join(shot.OUT, 'idle_keys.txt')
    with open(sc, 'w', encoding='utf-8') as f:
        f.write('size:120x34,wait:%d' % (secs * 1000 + 3000))
    p = subprocess.Popen([shot.EXE, '--demo', '--shot=' + os.path.join(shot.OUT, 'idle.bmp'), '--shot-keys=@' + sc],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=shot.child_env())
    time.sleep(2)
    t1, c1 = time.time(), A.thread_times(p.pid)
    time.sleep(secs)
    t2, c2 = time.time(), A.thread_times(p.pid)
    p.wait(timeout=secs + 30)
    return sum(max(0, c2[k] - c1.get(k, 0)) for k in c2) / ((t2 - t1) * 1e7) * 100


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    print('сборка: %s; режим: %s; %d x %d с' % (shot.EXE, 'с микрофоном' if os.environ.get('AUDIO_MIC') == '1' else 'только тон (--ptt)', n, secs))
    rows = []
    for i in range(n):
        g, c, sec, cpu = one(secs)
        rows.append((g, c, sec, cpu))
        print('  прогон %d: пропусков %s, щелчков %s, тон %.1f с, CPU %.2f %%' % (i + 1, g, c, sec, cpu))
    ok = [r for r in rows if r[0] is not None]
    if ok:
        tot = sum(r[2] for r in ok)
        print('  ИТОГО: пропусков %d, щелчков %d за %.0f с (%.3f щелчка/с), CPU медиана %.2f %%'
              % (sum(r[0] for r in ok), sum(r[1] for r in ok), tot, sum(r[1] for r in ok) / tot, statistics.median(r[3] for r in rows)))
    print('  CPU простоя без звонка (10 с): %.2f %%' % idle_cpu())
    return 0


if __name__ == '__main__':
    sys.exit(main())
