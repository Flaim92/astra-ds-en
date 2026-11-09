# -*- coding: utf-8 -*-
"""Разрывы и щелчки в выводе голоса + CPU потоков (демо, скрытое окно).
Демо-голос играет тестовый тон 440 Гц (voice.c, render: «тестовый тон 440 Гц, тихо»). Стенд снимает вывод
системы через WASAPI loopback (ctypes, только в память, файлов нет) и ищет на тоне:
  * пропуски — 5-мс окна без тона посреди звучания (амплитуда на 440 Гц < 30 % медианы);
  * щелчки — остаток предсказания синуса y[n]-2cos(w)y[n-1]+y[n-2] выше 8 медиан;
  * флаги WASAPI DATA_DISCONTINUITY.
Микрофон в звонке выключается (F5), чтобы в выводе был только тон. Внимание: loopback слышит ВСЁ, что играет
система, — посторонний звук даст ложные «щелчки»; при сомнении повторить в тишине.
CPU: GetThreadTimes по каждому потоку процесса в середине звонка (окно 5 с).
    python tools/uitest/audio_check.py [секунд]"""
import ctypes as C
import os, sys, threading, time, subprocess
from ctypes import wintypes as W
import numpy as np
import shot
import windows as WN

ole = C.OleDLL('ole32')
k32 = C.WinDLL('kernel32', use_last_error=True)


class GUID(C.Structure):
    _fields_ = [('a', C.c_uint32), ('b', C.c_uint16), ('c', C.c_uint16), ('d', C.c_ubyte * 8)]

    def __init__(self, s):
        p = s.split('-')
        tail = bytes.fromhex(p[3] + p[4])
        super().__init__(int(p[0], 16), int(p[1], 16), int(p[2], 16), (C.c_ubyte * 8)(*tail))


CLSID_MMDeviceEnumerator = GUID('BCDE0395-E52F-467C-8E3D-C4579291692E')
IID_IMMDeviceEnumerator = GUID('A95664D2-9614-4F35-A746-DE8DB63617E6')
IID_IAudioClient = GUID('1CB9AD4C-DBFA-4C32-B178-C2F568A703B2')
IID_IAudioCaptureClient = GUID('C8ADBD64-E71E-48A0-A4DE-185C395CD317')


def vcall(obj, idx, restype, *args):
    """Вызов метода COM по номеру в таблице (obj — c_void_p)."""
    vt = C.cast(obj, C.POINTER(C.POINTER(C.c_void_p)))[0]
    conv = [C.c_void_p(C.addressof(a._obj)) if hasattr(a, '_obj') else a for a in args]   # byref(x) -> адрес x
    fn = C.WINFUNCTYPE(restype, C.c_void_p, *[type(a) for a in conv])(vt[idx])
    return fn(obj, *conv)


class WAVEFORMATEX(C.Structure):
    _fields_ = [('wFormatTag', W.WORD), ('nChannels', W.WORD), ('nSamplesPerSec', W.DWORD), ('nAvgBytesPerSec', W.DWORD),
                ('nBlockAlign', W.WORD), ('wBitsPerSample', W.WORD), ('cbSize', W.WORD)]


def loopback(seconds, stop_evt, out):
    """Захват вывода по умолчанию (loopback) в out['frames'] (float32, моно), out['glitch'] — флаги разрыва."""
    ole.CoInitializeEx(None, 0)
    en = C.c_void_p()
    ole.CoCreateInstance(C.byref(CLSID_MMDeviceEnumerator), None, 23, C.byref(IID_IMMDeviceEnumerator), C.byref(en))
    dev = C.c_void_p()
    vcall(en, 4, C.HRESULT, C.c_int(0), C.c_int(0), C.byref(dev))          # GetDefaultAudioEndpoint(eRender, eConsole)
    ac = C.c_void_p()
    vcall(dev, 3, C.HRESULT, C.byref(IID_IAudioClient), C.c_uint32(23), C.c_void_p(), C.byref(ac))   # Activate
    pwfx = C.c_void_p()
    vcall(ac, 8, C.HRESULT, C.byref(pwfx))                                 # GetMixFormat
    fmt = C.cast(pwfx, C.POINTER(WAVEFORMATEX)).contents
    ch, rate, bits = fmt.nChannels, fmt.nSamplesPerSec, fmt.wBitsPerSample
    out['rate'] = rate
    vcall(ac, 3, C.HRESULT, C.c_int(0), C.c_uint32(0x00020000), C.c_longlong(10_000_000), C.c_longlong(0), pwfx, C.c_void_p())
    cc = C.c_void_p()
    vcall(ac, 14, C.HRESULT, C.byref(IID_IAudioCaptureClient), C.byref(cc))   # GetService
    vcall(ac, 10, C.HRESULT)                                                   # Start
    frames, glitch = [], 0
    t_end = time.time() + seconds
    while time.time() < t_end and not stop_evt.is_set():
        n = C.c_uint32()
        vcall(cc, 5, C.HRESULT, C.byref(n))                                    # GetNextPacketSize
        while n.value:
            data = C.c_void_p(); nf = C.c_uint32(); flags = C.c_uint32()
            vcall(cc, 3, C.HRESULT, C.byref(data), C.byref(nf), C.byref(flags), C.c_void_p(), C.c_void_p())
            cnt = nf.value * ch
            if flags.value & 2 or not data.value:                              # SILENT
                a = np.zeros(nf.value, np.float32)
            elif bits == 32:
                a = np.ctypeslib.as_array(C.cast(data, C.POINTER(C.c_float)), (cnt,)).reshape(-1, ch).mean(1).astype(np.float32)
            else:
                a = np.ctypeslib.as_array(C.cast(data, C.POINTER(C.c_int16)), (cnt,)).reshape(-1, ch).mean(1) / 32768.0
            if flags.value & 1:
                glitch += 1                                                    # DATA_DISCONTINUITY
            frames.append(a.copy())
            vcall(cc, 4, C.HRESULT, nf)                                        # ReleaseBuffer
            vcall(cc, 5, C.HRESULT, C.byref(n))
        time.sleep(0.005)
    vcall(ac, 11, C.HRESULT)                                                   # Stop
    out['frames'] = np.concatenate(frames) if frames else np.zeros(0, np.float32)
    out['glitch'] = glitch


IID_IAudioSessionManager2 = GUID('77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F')
IID_IAudioSessionControl2 = GUID('BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D')


def render_session_pids():
    """PID процессов, у которых есть аудиосессия на устройстве вывода по умолчанию: {pid: состояние 0/1/2}."""
    ole.CoInitializeEx(None, 0)
    en = C.c_void_p()
    ole.CoCreateInstance(C.byref(CLSID_MMDeviceEnumerator), None, 23, C.byref(IID_IMMDeviceEnumerator), C.byref(en))
    dev = C.c_void_p()
    vcall(en, 4, C.HRESULT, C.c_int(0), C.c_int(0), C.byref(dev))
    sm = C.c_void_p()
    vcall(dev, 3, C.HRESULT, C.byref(IID_IAudioSessionManager2), C.c_uint32(23), C.c_void_p(), C.byref(sm))
    se = C.c_void_p()
    vcall(sm, 5, C.HRESULT, C.byref(se))                                       # GetSessionEnumerator
    n = C.c_int()
    vcall(se, 3, C.HRESULT, C.byref(n))
    res = {}
    for i in range(n.value):
        sc = C.c_void_p()
        vcall(se, 4, C.HRESULT, C.c_int(i), C.byref(sc))
        sc2 = C.c_void_p()
        vcall(sc, 0, C.HRESULT, C.byref(IID_IAudioSessionControl2), C.byref(sc2))   # QueryInterface
        pid, st = C.c_uint32(), C.c_int()
        try:
            vcall(sc2, 14, C.HRESULT, C.byref(pid))                            # GetProcessId
            vcall(sc2, 3, C.HRESULT, C.byref(st))                              # GetState
            res[pid.value] = st.value
        except OSError:
            pass
    return res


def analyze(x, rate, f=440.0):
    win = int(rate * 0.005)
    n = len(x) // win
    if n < 20:
        return None
    blocks = x[:n * win].reshape(n, win)
    t = np.arange(win) / rate
    c, s = np.cos(2 * np.pi * f * t), np.sin(2 * np.pi * f * t)
    amp = np.hypot(blocks @ c, blocks @ s) * 2 / win
    on = amp > amp.max() * 0.3
    if on.sum() < 20:
        return {'tone': False}
    i0, i1 = np.argmax(on), len(on) - np.argmax(on[::-1])                      # отрезок звучания
    seg = amp[i0:i1]
    med = np.median(seg)
    gaps = int((seg < med * 0.3).sum())
    y = x[i0 * win:i1 * win].astype(np.float64)
    r = y[2:] - 2 * np.cos(2 * np.pi * f / rate) * y[1:-1] + y[:-2]
    # щелчок — скачок формы не меньше 20 % амплитуды тона (у чистого синуса остаток ~0); соседние в 10 мс — одно событие
    idx = np.nonzero(np.abs(r) > med * 0.2)[0]
    ev = []
    for i in idx:
        if not ev or i - ev[-1] > rate // 100:
            ev.append(i)
        else:
            ev[-1] = i
    gap_t = [round((i0 + k) * 0.005, 2) for k in np.nonzero(seg < med * 0.3)[0]]
    return {'tone': True, 'sec': (i1 - i0) * 0.005, 'amp': float(med), 'gaps': gaps, 'gap_t': gap_t[:10],
            'clicks': len(ev), 'click_t': [round(i0 * 0.005 + i / rate, 3) for i in ev[:10]],
            'click_rate': len(ev) / max(1e-9, (i1 - i0) * 0.005)}


def thread_times(pid):
    """{tid: (kernel+user) в 100 нс} всех потоков процесса."""
    class TE(C.Structure):
        _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD), ('th32OwnerProcessID', W.DWORD),
                    ('tpBasePri', C.c_long), ('tpDeltaPri', C.c_long), ('dwFlags', W.DWORD)]
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    te = TE(); te.dwSize = C.sizeof(TE)
    res = {}
    ok = k32.Thread32First(snap, C.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            h = k32.OpenThread(0x0800, False, te.th32ThreadID)                  # THREAD_QUERY_LIMITED_INFORMATION
            if h:
                a, b, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
                if k32.GetThreadTimes(h, C.byref(a), C.byref(b), C.byref(kt), C.byref(ut)):
                    res[te.th32ThreadID] = ((kt.dwHighDateTime << 32) | kt.dwLowDateTime) + ((ut.dwHighDateTime << 32) | ut.dwLowDateTime)
                k32.CloseHandle(h)
        ok = k32.Thread32Next(snap, C.byref(te))
    k32.CloseHandle(snap)
    return res


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    S = 'click:3:7,wait:50'
    v = S + ',click:' + WN.at(S, '<) ОБЩИЙ', dx=3) + ',wait:300'
    j = v + ',click:' + WN.at(v, 'Присоединиться') + ',wait:800' + (',f5' if os.environ.get('AUDIO_MIC') != '1' else '') + ',wait:%d' % (secs * 1000 + 2000)
    sc = os.path.join(shot.OUT, 'audio_keys.txt')
    with open(sc, 'w', encoding='utf-8') as f:
        f.write('size:120x34,' + j)
    out, stop = {}, threading.Event()
    th = threading.Thread(target=loopback, args=(secs + 2, stop, out))
    th.start()
    # --ptt: без F8 микрофон не передаёт — в выводе только тон (петля демо не подмешивает голос из комнаты)
    ptt = [] if os.environ.get('AUDIO_MIC') == '1' else ['--ptt']   # AUDIO_MIC=1 — петля микрофона работает (как в разговоре)
    p = subprocess.Popen([shot.EXE, '--demo'] + ptt + ['--shot=' + os.path.join(shot.OUT, 'audio.bmp'), '--shot-keys=@' + sc],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    t1, c1 = time.time(), thread_times(p.pid)
    time.sleep(5)
    t2, c2 = time.time(), thread_times(p.pid)
    p.wait(timeout=secs + 30)
    th.join()
    dt = (t2 - t1) * 1e7
    cpu = sorted(((c2[k] - c1.get(k, 0)) / dt * 100 for k in c2), reverse=True)
    print('  CPU потоков в звонке (окно 5 с, %% одного ядра): %s; процесс всего %.2f %%'
          % (', '.join('%.2f' % c for c in cpu[:5]), sum(cpu)))
    a = analyze(out.get('frames', np.zeros(0)), out.get('rate', 48000))
    print('  loopback: %.1f с звука, флагов разрыва WASAPI: %d' % (len(out.get('frames', [])) / out.get('rate', 48000), out.get('glitch', 0)))
    if not a or not a.get('tone'):
        print('  [FAIL] тон 440 Гц в выводе не найден (звук выключен? другое устройство вывода?)')
        return 1
    ok = a['gaps'] == 0 and a['click_rate'] < 1.0
    print('  тон %.1f с, амплитуда %.3f; пропусков (5 мс) %d %s; щелчков %d (%.2f/с) %s'
          % (a['sec'], a['amp'], a['gaps'], a['gap_t'], a['clicks'], a['click_rate'], a['click_t']))
    print('  [%s] вывод без разрывов и щелчков' % ('ok' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
