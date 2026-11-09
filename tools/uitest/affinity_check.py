# -*- coding: utf-8 -*-
"""Исключается ли окно с SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) из захвата GDI BitBlt экранного DC.
ВИДИМОЕ окно (разрешено владельцем 09.11 один раз, до минуты): красный квадрат 200x200 поверх всех на ~3 с,
без клиента. Снимок области окна — BitBlt в память (на диск ничего), счёт красных пикселей: без affinity,
с WDA_EXCLUDEFROMCAPTURE (0x11), снова без (контроль). Печатает только цифры."""
import ctypes as C
from ctypes import wintypes as W
import time

u32, g32, k32 = C.WinDLL('user32', use_last_error=True), C.WinDLL('gdi32'), C.WinDLL('kernel32')
WNDPROC = C.WINFUNCTYPE(C.c_ssize_t, W.HWND, W.UINT, W.WPARAM, W.LPARAM)
u32.DefWindowProcW.restype = C.c_ssize_t
u32.DefWindowProcW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]


class WNDCLASSW(C.Structure):
    _fields_ = [('style', W.UINT), ('lpfnWndProc', WNDPROC), ('cbClsExtra', C.c_int), ('cbWndExtra', C.c_int),
                ('hInstance', W.HINSTANCE), ('hIcon', W.HICON), ('hCursor', W.HANDLE), ('hbrBackground', W.HBRUSH),
                ('lpszMenuName', W.LPCWSTR), ('lpszClassName', W.LPCWSTR)]


class BITMAPINFOHEADER(C.Structure):
    _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG), ('biPlanes', W.WORD), ('biBitCount', W.WORD),
                ('biCompression', W.DWORD), ('biSizeImage', W.DWORD), ('biXPelsPerMeter', W.LONG), ('biYPelsPerMeter', W.LONG),
                ('biClrUsed', W.DWORD), ('biClrImportant', W.DWORD)]


@WNDPROC
def proc(h, m, w, l):
    return u32.DefWindowProcW(h, m, w, l)


def pump(t):
    msg = W.MSG()
    end = time.time() + t
    while time.time() < end:
        while u32.PeekMessageW(C.byref(msg), None, 0, 0, 1):
            u32.TranslateMessage(C.byref(msg)); u32.DispatchMessageW(C.byref(msg))
        time.sleep(0.02)


def red_count(x, y, w, h):
    g32.CreateCompatibleDC.restype = W.HDC
    g32.CreateCompatibleDC.argtypes = [W.HDC]
    g32.CreateCompatibleBitmap.restype = W.HBITMAP
    g32.CreateCompatibleBitmap.argtypes = [W.HDC, C.c_int, C.c_int]
    g32.SelectObject.restype = W.HGDIOBJ
    g32.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
    g32.BitBlt.argtypes = [W.HDC, C.c_int, C.c_int, C.c_int, C.c_int, W.HDC, C.c_int, C.c_int, W.DWORD]
    g32.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, C.c_void_p, C.c_void_p, W.UINT]
    g32.DeleteObject.argtypes = [W.HGDIOBJ]
    g32.DeleteDC.argtypes = [W.HDC]
    u32.GetDC.restype = W.HDC
    u32.GetDC.argtypes = [W.HWND]
    u32.ReleaseDC.argtypes = [W.HWND, W.HDC]
    sdc = u32.GetDC(None)
    mdc = g32.CreateCompatibleDC(sdc)
    bm = g32.CreateCompatibleBitmap(sdc, w, h)
    g32.SelectObject(mdc, bm)
    g32.BitBlt(mdc, 0, 0, w, h, sdc, x, y, 0x00CC0020)          # SRCCOPY
    bi = BITMAPINFOHEADER(C.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = (C.c_ubyte * (w * h * 4))()
    g32.GetDIBits(mdc, bm, 0, h, buf, C.byref(bi), 0)
    g32.DeleteObject(bm); g32.DeleteDC(mdc); u32.ReleaseDC(None, sdc)
    n = 0
    for i in range(0, w * h * 4, 4):
        b, g, r = buf[i], buf[i + 1], buf[i + 2]
        if r > 200 and g < 60 and b < 60:
            n += 1
    return n


def main():
    k32.GetModuleHandleW.restype = W.HMODULE
    hinst = k32.GetModuleHandleW(None)
    g32.CreateSolidBrush.restype = W.HBRUSH
    wc = WNDCLASSW(0, proc, 0, 0, hinst, None, None, g32.CreateSolidBrush(0x000000FF), None, 'AffinityRedTest')
    u32.RegisterClassW(C.byref(wc))
    u32.CreateWindowExW.restype = W.HWND
    u32.CreateWindowExW.argtypes = [W.DWORD, W.LPCWSTR, W.LPCWSTR, W.DWORD, C.c_int, C.c_int, C.c_int, C.c_int, W.HWND, W.HMENU, W.HINSTANCE, W.LPVOID]
    u32.SetWindowDisplayAffinity.argtypes = [W.HWND, W.DWORD]
    u32.DestroyWindow.argtypes = [W.HWND]
    X, Y, S = 120, 120, 200
    hwnd = u32.CreateWindowExW(0x00000008 | 0x00000080, 'AffinityRedTest', 'affinity test', 0x80000000 | 0x10000000,
                               X, Y, S, S, None, None, hinst, None)   # WS_EX_TOPMOST|TOOLWINDOW, WS_POPUP|WS_VISIBLE
    try:
        pump(0.6)
        a = red_count(X, Y, S, S)
        okset = u32.SetWindowDisplayAffinity(hwnd, 0x11)               # WDA_EXCLUDEFROMCAPTURE
        err = C.get_last_error()
        pump(0.6)
        b = red_count(X, Y, S, S)
        u32.SetWindowDisplayAffinity(hwnd, 0)                          # WDA_NONE
        pump(0.6)
        c = red_count(X, Y, S, S)
    finally:
        u32.DestroyWindow(hwnd)
        pump(0.1)
    tot = S * S
    print('без affinity: красных %d из %d' % (a, tot))
    print('WDA_EXCLUDEFROMCAPTURE (SetWindowDisplayAffinity=%s, ошибка %d): красных %d из %d' % (bool(okset), err, b, tot))
    print('снова без (контроль): красных %d из %d' % (c, tot))
    print('вывод: %s' % ('окно ИСКЛЮЧАЕТСЯ из BitBlt' if a > tot * 0.9 and b < tot * 0.01 and c > tot * 0.9 else
                         'окно НЕ исключается из BitBlt' if a > tot * 0.9 and b > tot * 0.9 else 'неоднозначно (см. цифры)'))


if __name__ == '__main__':
    main()
