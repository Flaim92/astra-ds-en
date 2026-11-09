# -*- coding: utf-8 -*-
"""Контроль audio_check.py: тот же анализ на эталонном тоне 440 Гц, сыгранном из памяти через winsound (WinMM),
без клиента. Если и здесь «пропуски/щелчки» — виноват захват/система, а не клиент."""
import io, struct, sys, threading, time, wave, winsound
import numpy as np
import audio_check as A

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 10
rate = 48000
t = np.arange(rate * secs) / rate
pcm = (np.sin(2 * np.pi * 440 * t) * 2500).astype(np.int16)
buf = io.BytesIO()
with wave.open(buf, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(pcm.tobytes())
out, stop = {}, threading.Event()
th = threading.Thread(target=A.loopback, args=(secs + 2, stop, out))
th.start()
time.sleep(0.5)
winsound.PlaySound(buf.getvalue(), winsound.SND_MEMORY)      # синхронно, только в память
th.join()
a = A.analyze(out['frames'], out['rate'])
print('  контроль: флагов разрыва %d; %s' % (out['glitch'], {k: a[k] for k in ('sec', 'amp', 'gaps', 'clicks', 'click_rate')} if a and a.get('tone') else a))
