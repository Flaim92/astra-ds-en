# -*- coding: utf-8 -*-
"""--tui в псевдоконсоли Windows (ConPTY, без видимого окна): клиент --demo --tui рисует в поток VT,
стенд читает поток, шлёт клавиши (байты VT), в конце Ctrl+Q. Проверка: интерфейс запустился,
в потоке ожидаемые надписи, выход по Ctrl+Q с кодом 0.
    python tools/uitest/tui_conpty.py [ШxВ]"""
import ctypes as C
from ctypes import wintypes as W
import os, re, sys, threading, time

k32 = C.WinDLL('kernel32', use_last_error=True)
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
# бинарник: astra-ds.exe (после переименования) или mincord.exe; MC_BIN — явный путь
# только astra-ds: старый mincord.exe в каталоге — устаревшая сборка; после неудачной линковки astra-ds.exe нет,
# и молчаливый откат на старое имя проверял бы старый интерфейс (случай 09.11) — лучше явная ошибка запуска
EXE = os.environ.get('MC_BIN') or next((p for p in (os.path.join(ROOT, 'build-test2', n) for n in
      ('astra-ds.exe', 'astra-ds')) if os.path.exists(p)), os.path.join(ROOT, 'build-test2', 'astra-ds.exe'))


class COORD(C.Structure):
    _fields_ = [('X', W.SHORT), ('Y', W.SHORT)]


class STARTUPINFOW(C.Structure):
    _fields_ = [('cb', W.DWORD), ('lpReserved', W.LPWSTR), ('lpDesktop', W.LPWSTR), ('lpTitle', W.LPWSTR),
                ('dwX', W.DWORD), ('dwY', W.DWORD), ('dwXSize', W.DWORD), ('dwYSize', W.DWORD),
                ('dwXCountChars', W.DWORD), ('dwYCountChars', W.DWORD), ('dwFillAttribute', W.DWORD),
                ('dwFlags', W.DWORD), ('wShowWindow', W.WORD), ('cbReserved2', W.WORD),
                ('lpReserved2', C.c_void_p), ('hStdInput', W.HANDLE), ('hStdOutput', W.HANDLE), ('hStdError', W.HANDLE)]


class STARTUPINFOEXW(C.Structure):
    _fields_ = [('StartupInfo', STARTUPINFOW), ('lpAttributeList', C.c_void_p)]


class PROCESS_INFORMATION(C.Structure):
    _fields_ = [('hProcess', W.HANDLE), ('hThread', W.HANDLE), ('dwProcessId', W.DWORD), ('dwThreadId', W.DWORD)]


k32.CreatePseudoConsole.argtypes = [COORD, W.HANDLE, W.HANDLE, W.DWORD, C.POINTER(C.c_void_p)]
k32.InitializeProcThreadAttributeList.argtypes = [C.c_void_p, W.DWORD, W.DWORD, C.POINTER(C.c_size_t)]
k32.UpdateProcThreadAttribute.argtypes = [C.c_void_p, W.DWORD, C.c_size_t, C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p]
k32.CreateProcessW.argtypes = [W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p, W.BOOL, W.DWORD, C.c_void_p, W.LPCWSTR,
                               C.c_void_p, C.c_void_p]
k32.ClosePseudoConsole.argtypes = [C.c_void_p]
PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
EXTENDED_STARTUPINFO_PRESENT = 0x00080000


def pipe():
    r, w = W.HANDLE(), W.HANDLE()
    assert k32.CreatePipe(C.byref(r), C.byref(w), None, 0)
    return r, w


def run(cols=120, rows=34, keys=(b'', ), settle=1.5):
    in_r, in_w = pipe()
    out_r, out_w = pipe()
    hpc = C.c_void_p()
    hr = k32.CreatePseudoConsole(COORD(cols, rows), in_r, out_w, 0, C.byref(hpc))
    assert hr == 0, hr
    size = C.c_size_t()
    k32.InitializeProcThreadAttributeList(None, 1, 0, C.byref(size))
    al = C.create_string_buffer(size.value)
    assert k32.InitializeProcThreadAttributeList(al, 1, 0, C.byref(size))
    assert k32.UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, C.sizeof(C.c_void_p), None, None)
    si = STARTUPINFOEXW()
    si.StartupInfo.cb = C.sizeof(STARTUPINFOEXW)
    si.lpAttributeList = C.cast(al, C.c_void_p)
    si.StartupInfo.dwFlags = 0x100              # STARTF_USESTDHANDLES, пустые: иначе наследуются каналы Python
    pi = PROCESS_INFORMATION()
    cmd = C.create_unicode_buffer('"%s" --demo --tui' % EXE)
    ok = k32.CreateProcessW(None, cmd, None, None, False, EXTENDED_STARTUPINFO_PRESENT, None, ROOT, C.byref(si), C.byref(pi))
    assert ok, C.get_last_error()
    k32.CloseHandle(in_r)
    k32.CloseHandle(out_w)
    data = bytearray()

    def reader():
        buf = C.create_string_buffer(65536)
        n = W.DWORD()
        while k32.ReadFile(out_r, buf, 65536, C.byref(n), None) and n.value:
            data.extend(buf.raw[:n.value])
    th = threading.Thread(target=reader, daemon=True)
    th.start()
    time.sleep(settle)
    marks = []
    for k in keys:
        if k:
            n = W.DWORD()
            k32.WriteFile(in_w, k, len(k), C.byref(n), None)
            time.sleep(0.4)
        marks.append(len(data))
    t0 = time.time()
    n = W.DWORD()
    k32.WriteFile(in_w, b'\x11', 1, C.byref(n), None)        # Ctrl+Q
    rc = k32.WaitForSingleObject(pi.hProcess, 5000)
    code = W.DWORD()
    k32.GetExitCodeProcess(pi.hProcess, C.byref(code))
    if rc != 0:
        k32.TerminateProcess(pi.hProcess, 9)
    k32.ClosePseudoConsole(hpc)
    th.join(2)
    return bytes(data), marks, (rc == 0, code.value, time.time() - t0)


def plain(b):
    s = b.decode('utf-8', 'replace')
    s = re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', ' ', s)
    s = re.sub(r'\x1b\][^\x07\x1b]*(\x07|\x1b\\)', '', s)
    return re.sub(r'\x1b.', '', s)


if __name__ == '__main__':
    W_, H_ = (int(v) for v in (sys.argv[1] if len(sys.argv) > 1 else '120x34').split('x'))
    data, marks, (exited, code, dt) = run(W_, H_, keys=[b'', b'\x1bOP', b'\x1b', b'\t\t', b'\x1b[B\r', b'\x1b[<0;4;8M\x1b[<0;4;8m'])
    p = plain(data)
    res = []

    def ck(what, ok):
        res.append(ok)
        print('  [%s] %s' % ('ok' if ok else 'FAIL', what))
    ck('--tui запустился, кадр %d байт' % marks[0], marks[0] > 1000)
    ck('экран «Друзья» в потоке', 'Друзья' in plain(data[:marks[0]]))
    ck('F1 — справка', 'справка' in plain(data[marks[0]:marks[1]]))
    ck('Esc закрыл справку (кадр перерисован)', marks[2] > marks[1])
    ck('Tab Tab Down Enter — интерфейс жив', marks[4] > marks[3])
    ck('мышь терминала (SGR 1006): щелчок по значку сервера открыл «Уютный уголок»', 'Уютный' in plain(data[marks[4]:marks[5]]))
    ck('графика: только ASCII в оформлении (нет ┌─│…—)', not re.search('[─-╿…—]', p))
    ck('Ctrl+Q: выход за %.1f с, код %d' % (dt, code), exited and code == 0)
    # перенос строки в поле: Ctrl+J (0x0A) и Alt+Enter (ESC CR) — перенос, Enter (CR) — отправка
    if W_ >= 100 and H_ >= 30:
        def sgr(x, y):                              # щелчок SGR 1006, x/y — клетка с 0
            return b'\x1b[<0;%d;%dM\x1b[<0;%d;%dm' % (x + 1, y + 1, x + 1, y + 1)
        for name, nl in (('Ctrl+J', b'\n'), ('Alt+Enter', b'\x1b\r')):
            keys = [b'', sgr(3, 7), sgr(20, 6), sgr(60, H_ - 2), 'раз'.encode(), nl, 'два'.encode(), b'\r']
            d, mk, _ = run(W_, H_, keys=keys)
            before = plain(d[mk[3]:mk[6]])           # от начала набора до Enter
            after = plain(d[mk[6]:mk[7]])            # после Enter
            ck('%s — перенос: до Enter сообщение не ушло' % name, '<Вы>' not in before)
            ck('%s — Enter отправил сообщение' % name, '<Вы>' in after)
    print('Итог: %d ok, %d FAIL' % (sum(res), len(res) - sum(res)))
