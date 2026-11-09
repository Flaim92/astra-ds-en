#!/usr/bin/env python3
"""Генератор src/astra_icon.h: значок Astra DS для X11 (_NET_WM_ICON) из res/astra-256.png.

Формат _NET_WM_ICON: [ширина, высота, ARGB * ширина*высота] для каждого размера подряд (тип CARDINAL —
в Xlib это long). Данные — константный массив в .rodata, без декодеров PNG в программе.
Запуск: python tools/gen_icon.py  (нужен Pillow; результат класть в репозиторий)."""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'res', 'astra-256.png')
DST = os.path.join(ROOT, 'src', 'astra_icon.h')

img = Image.open(SRC).convert('RGBA')
vals = []
for size in (32, 48):
    im = img.resize((size, size), Image.LANCZOS)
    vals += [size, size]
    for r, g, b, a in im.getdata():
        vals.append((a << 24) | (r << 16) | (g << 8) | b)

with open(DST, 'w', encoding='utf-8', newline='\n') as f:
    f.write('/* Значок Astra DS для X11 (_NET_WM_ICON): 32x32 и 48x48 ARGB. Создан tools/gen_icon.py из res/astra-256.png.\n')
    f.write(' * Не править вручную. */\n#ifndef ASTRA_ICON_H\n#define ASTRA_ICON_H\n')
    f.write('static const unsigned long ASTRA_ICON[] = {\n')
    for i in range(0, len(vals), 8):
        f.write('    ' + ', '.join('0x%08x' % v for v in vals[i:i + 8]) + ',\n')
    f.write('};\n#endif\n')
print(DST, len(vals))
