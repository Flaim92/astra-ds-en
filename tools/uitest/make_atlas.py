# -*- coding: utf-8 -*-
"""Атлас глифов для распознавания текста снимков окна: символы рисуются тем же GDI-шрифтом, что окно
клиента в --shot (gui_win.c: Cascadia Mono -> Consolas -> Courier New, кегль 11 пт при 96 dpi, ClearType,
обычный и FW_BOLD), в клетку того же размера; маска «чернил» — тем же порогом, что Shot.cell.
Проверка: печатается доля узнанных клеток на снимке настоящего окна. Перезапускать при смене шрифта."""
import ctypes as C
from ctypes import wintypes as W
import numpy as np
import shot

gdi = C.windll.gdi32
PT, DPI = 11, 96
CHARS = [chr(c) for c in range(0x21, 0x7F)] + [chr(c) for c in range(0x410, 0x450)] + ['ё', 'Ё']


class LOGFONTW(C.Structure):
    _fields_ = [('lfHeight', W.LONG), ('lfWidth', W.LONG), ('lfEscapement', W.LONG), ('lfOrientation', W.LONG),
                ('lfWeight', W.LONG), ('lfItalic', W.BYTE), ('lfUnderline', W.BYTE), ('lfStrikeOut', W.BYTE),
                ('lfCharSet', W.BYTE), ('lfOutPrecision', W.BYTE), ('lfClipPrecision', W.BYTE),
                ('lfQuality', W.BYTE), ('lfPitchAndFamily', W.BYTE), ('lfFaceName', W.WCHAR * 32)]


class BITMAPINFOHEADER(C.Structure):
    _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG), ('biPlanes', W.WORD),
                ('biBitCount', W.WORD), ('biCompression', W.DWORD), ('biSizeImage', W.DWORD),
                ('biXPelsPerMeter', W.LONG), ('biYPelsPerMeter', W.LONG), ('biClrUsed', W.DWORD),
                ('biClrImportant', W.DWORD)]


gdi.CreateFontIndirectW.restype = W.HFONT
gdi.CreateCompatibleDC.restype = W.HDC
gdi.CreateCompatibleDC.argtypes = [W.HDC]
gdi.CreateDIBSection.restype = W.HBITMAP
gdi.CreateDIBSection.argtypes = [W.HDC, C.c_void_p, W.UINT, C.POINTER(C.c_void_p), W.HANDLE, W.DWORD]
gdi.SelectObject.restype = W.HGDIOBJ
gdi.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
gdi.ExtTextOutW.argtypes = [W.HDC, C.c_int, C.c_int, W.UINT, C.c_void_p, W.LPCWSTR, W.UINT, C.c_void_p]
gdi.SetTextColor.argtypes = [W.HDC, W.DWORD]
gdi.SetBkColor.argtypes = [W.HDC, W.DWORD]
gdi.GetTextFaceW.argtypes = [W.HDC, C.c_int, W.LPWSTR]


def font(face, weight):
    lf = LOGFONTW()
    lf.lfHeight = -((PT * DPI + 36) // 72)      # -MulDiv(PT, dpi, 72)
    lf.lfWeight = weight
    lf.lfCharSet = 1                            # DEFAULT_CHARSET
    lf.lfOutPrecision = 4                       # OUT_TT_PRECIS
    lf.lfQuality = 5                            # CLEARTYPE_QUALITY
    lf.lfPitchAndFamily = 1 | 0x30              # FIXED_PITCH | FF_MODERN
    lf.lfFaceName = face
    return gdi.CreateFontIndirectW(C.byref(lf))


def render(cw, ch, fg=0xDBDEE1, bg=0x313338):
    dc = gdi.CreateCompatibleDC(None)
    face = 'Courier New'
    for f in ('Cascadia Mono', 'Consolas', 'Courier New'):
        gdi.SelectObject(dc, font(f, 400))
        b = C.create_unicode_buffer(32)
        gdi.GetTextFaceW(dc, 32, b)
        if b.value.lower() == f.lower():
            face = f
            break
    n = len(CHARS)
    bi = BITMAPINFOHEADER(C.sizeof(BITMAPINFOHEADER), cw * n, -ch * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    bits = C.c_void_p()
    bm = gdi.CreateDIBSection(dc, C.byref(bi), 0, C.byref(bits), None, 0)
    gdi.SelectObject(dc, bm)
    cref = lambda c: ((c & 255) << 16) | (c & 0xFF00) | ((c >> 16) & 255)
    gdi.SetTextColor(dc, cref(fg))
    gdi.SetBkColor(dc, cref(bg))
    s = ''.join(CHARS)
    dx = (C.c_int * n)(*([cw] * n))
    for row, wt in enumerate((400, 700)):
        gdi.SelectObject(dc, font(face, wt))
        rc = (C.c_long * 4)(0, row * ch, cw * n, (row + 1) * ch)
        gdi.ExtTextOutW(dc, 0, row * ch, 2 | 4, rc, s, n, dx)   # ETO_OPAQUE | ETO_CLIPPED
    gdi.GdiFlush()
    buf = C.string_at(bits, cw * n * ch * 2 * 4)
    a = np.frombuffer(buf, np.uint8).reshape(ch * 2, cw * n, 4)[:, :, 2::-1].astype(np.int32)
    return face, a, bg


def main():
    ref = shot.run('wait:50', 'atlas_ref', '120x34')
    cw, ch = ref.cw, ref.ch
    face, a, bg = render(cw, ch)
    bgc = np.array([(bg >> 16) & 255, (bg >> 8) & 255, bg & 255])
    masks, chars = [], []
    for row in range(2):
        for i, c in enumerate(CHARS):
            p = a[row * ch:(row + 1) * ch, i * cw:(i + 1) * cw].reshape(-1, 3)
            d = np.abs(p - bgc).sum(1)
            if d.max() < 60:
                continue
            masks.append(d > d.max() * 0.45)
            chars.append(ord(c))
    np.savez_compressed(shot.ATLAS, masks=np.array(masks, dtype=np.uint8), chars=np.array(chars, dtype=np.int32),
                        bold=np.array([1], dtype=np.uint8))
    shot._atlas = None
    t = ref.text()
    cells = sum(1 for l in t for c in l if c != ' ')
    unk = sum(l.count('?') for l in t)
    print('шрифт %s, клетка %dx%d, глифов %d; узнано клеток с чернилами %d из %d (картинки — «?»)'
          % (face, cw, ch, len(chars), cells - unk, cells))
    print('\n'.join(t[:6]))


if __name__ == '__main__':
    main()
