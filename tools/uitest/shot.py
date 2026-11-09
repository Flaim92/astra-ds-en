# -*- coding: utf-8 -*-
"""Стенд окон клиента (Astra DS, бывш. mincord): скрытый снимок настоящего окна (--demo --shot) и разбор BMP по клеткам.

Запуск клиента — только --demo и только --shot (окно не показывается). По одному процессу.
  run(keys, name)      -> Shot: картинка, сетка клеток, память, событий мыши
  Shot.text()          -> строки экрана (распознавание глифов по атласу atlas.npz, см. make_atlas.py)
  Shot.has(s)          -> есть ли подстрока (с допуском на 1 нераспознанную клетку на 6 символов)
  Shot.find(s)         -> (x, y) клетки начала
  Shot.cell(x, y)      -> (fg, bg, mask)
"""
import os, re, subprocess, sys
import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
# бинарник: astra-ds.exe (после переименования) или mincord.exe; MC_BIN — явный путь
# только astra-ds: старый mincord.exe в каталоге — устаревшая сборка; после неудачной линковки astra-ds.exe нет,
# и молчаливый откат на старое имя проверял бы старый интерфейс (случай 09.11) — лучше явная ошибка запуска
EXE = os.environ.get('MC_BIN') or next((p for p in (os.path.join(ROOT, 'build-test2', n) for n in
      ('astra-ds.exe', 'astra-ds')) if os.path.exists(p)), os.path.join(ROOT, 'build-test2', 'astra-ds.exe'))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')
ATLAS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'atlas.npz')
os.makedirs(OUT, exist_ok=True)

_atlas = None


# Одинаковые на вид глифы (кириллица/латиница и пр.): сравнение строк — после сведения к одному виду.
_HOMO = str.maketrans('аеорсухАВЕКМНОРСТХіІ|ё', 'aeopcyxABEKMHOPCTXiIlе')


def norm(s):
    return s.translate(_HOMO)


def _load_atlas():
    global _atlas
    if _atlas is None and os.path.exists(ATLAS):
        z = np.load(ATLAS, allow_pickle=False)
        m = z['masks'].astype(bool)             # обычный и жирный — отрисованы make_atlas.py шрифтом окна
        _atlas = (m.astype(np.float32), [chr(c) for c in z['chars']])
    return _atlas


class Shot:
    def __init__(self, path, cols, rows, info):
        self.path = path
        self.cols, self.rows = cols, rows
        self.info = info
        im = Image.open(path).convert('RGB')
        self.img = np.asarray(im).astype(np.int32)
        H, W, _ = self.img.shape
        self.cw, self.ch = W // cols, H // rows
        self._text = None

    # ---------- клетки ----------
    def cell_px(self, x, y):
        return self.img[y * self.ch:(y + 1) * self.ch, x * self.cw:(x + 1) * self.cw]

    def cell(self, x, y):
        """fg, bg (int RGB), маска «чернил» cw*ch bool."""
        p = self.cell_px(x, y).reshape(-1, 3)
        key = (p[:, 0] << 16) | (p[:, 1] << 8) | p[:, 2]
        vals, cnt = np.unique(key, return_counts=True)
        bg = int(vals[cnt.argmax()])
        bgc = np.array([(bg >> 16) & 255, (bg >> 8) & 255, bg & 255])
        d = np.abs(p - bgc).sum(1)
        if d.max() < 60:
            return None, bg, np.zeros(self.ch * self.cw, bool)
        far = p[d.argmax()]
        fg = int((far[0] << 16) | (far[1] << 8) | far[2])
        mask = d > d.max() * 0.45
        return fg, bg, mask

    def bg(self, x, y):
        return self.cell(x, y)[1]

    # ---------- текст ----------
    def text(self):
        """Строки экрана: из --shot-text (ui_snapshot, точно) или распознаванием клеток (UITEST_OCR=1 или нет .txt).
        Широкий символ в .txt дополняется пробелом — номера столбцов совпадают с клетками окна."""
        if self._text is not None:
            return self._text
        txt = self.path[:-4] + '.txt'
        if os.path.exists(txt) and not os.environ.get('UITEST_OCR'):
            import unicodedata
            lines = []
            for ln in open(txt, encoding='utf-8', errors='replace').read().split('\n')[:self.rows]:
                s = ''.join(c + (' ' if unicodedata.east_asian_width(c) in 'WF' else '') for c in ln)
                lines.append(s.ljust(self.cols)[:self.cols])
            while len(lines) < self.rows:
                lines.append(' ' * self.cols)
            self._text = lines
            return lines
        at = _load_atlas()
        lines = []
        for y in range(self.rows):
            s = []
            for x in range(self.cols):
                fg, bg, m = self.cell(x, y)
                if fg is None or m.sum() == 0:
                    s.append(' ')
                    continue
                if at is None:
                    s.append('?')
                    continue
                M, C = at
                if M.shape[1] != m.size:
                    s.append('?')
                    continue
                mf = m.astype(np.float32)
                # расстояние Хэмминга, нормированное на «чернила» обоих
                diff = np.abs(M - mf).sum(1)
                ink = M.sum(1) + mf.sum() + 1
                score = diff / ink
                i = int(score.argmin())
                s.append(C[i] if score[i] < 0.42 else '?')
            lines.append(''.join(s))
        self._text = lines
        return lines

    def dump(self):
        return '\n'.join(self.text())

    def find(self, needle, tol=None):
        n = len(needle)
        if tol is None:
            tol = n // 6 if n < 4 else max(1, n // 5)
        needle = norm(needle)
        for y, ln in enumerate(self.text()):
            ln = norm(ln)
            for x in range(0, len(ln) - n + 1):
                bad = 0
                for a, b in zip(ln[x:x + n], needle):
                    if a != b:
                        bad += 1
                        if bad > tol:
                            break
                if bad <= tol:
                    return x, y
        return None

    def has(self, needle, tol=None):
        return self.find(needle, tol) is not None

    # ---------- общие проверки ----------
    def colors(self):
        p = self.img.reshape(-1, 3)
        key = (p[:, 0] << 16) | (p[:, 1] << 8) | p[:, 2]
        vals, cnt = np.unique(key, return_counts=True)
        return dict(zip(vals.tolist(), cnt.tolist()))

    def nonblank(self):
        c = self.colors()
        top = max(c.values())
        return len(c) > 8 and top < self.img.shape[0] * self.img.shape[1] * 0.97

    def fine_cells(self, x0, y0, w, h):
        """Сколько различных цветов в прямоугольнике клеток: мелкая сетка даёт десятки-сотни."""
        p = self.img[y0 * self.ch:(y0 + h) * self.ch, x0 * self.cw:(x0 + w) * self.cw].reshape(-1, 3)
        key = (p[:, 0] << 16) | (p[:, 1] << 8) | p[:, 2]
        return len(np.unique(key))

    def png(self):
        p = self.path[:-4] + '.png'
        Image.open(self.path).save(p)
        return p


_lang_ok = {}


def lang_flag():
    """Язык клиента явно: --lang=ru|en (с 09.11 язык — флаг, сохранённый выбор или язык системы; LANG на Windows
    не действует). UITEST_LANG=en... — английский, иначе русский (ожидания матрицы — русские). Старые сборки
    (до флага: база, 0.1.1) его не знают и завершаются с кодом 2 — для них флаг не передаётся."""
    if EXE not in _lang_ok:
        try:
            h = subprocess.run([EXE, '--help'], capture_output=True, timeout=20).stdout.decode('utf-8', 'replace')
        except Exception:
            h = ''
        _lang_ok[EXE] = '--lang' in h
    if not _lang_ok[EXE]:
        return []
    return ['--lang=en' if os.environ.get('UITEST_LANG', 'ru').lower().startswith('en') else '--lang=ru']


def child_env():
    """Окружение клиента (для прямых Popen): LANG тоже задаётся — на Linux он ещё может влиять."""
    e = dict(os.environ)
    e.pop('LC_ALL', None)
    e['LANG'] = 'en_US.UTF-8' if lang_flag() == ['--lang=en'] else 'ru_RU.UTF-8'
    return e


def run(keys, name, size=None, extra=None, timeout=60):
    """Один скрытый прогон. size='WxH' добавляется первым шагом. Возвращает Shot (итоговый снимок)."""
    # клиент пишет .txt через fopen (UTF-8 путь на Windows искажается): снимаем под ASCII-именем, потом переименовываем
    tmp = '_cur'
    name = re.sub(r'[<>:"/\\|?*]', '_', name)       # недопустимые в именах файлов Windows
    path = os.path.join(OUT, name + '.bmp')
    tpath = os.path.join(OUT, tmp + '.bmp')
    steps = []
    if size:
        steps.append('size:' + size)
    if keys:
        steps.append(keys)
    args = [EXE, '--demo'] + lang_flag() + ['--shot=' + tpath, '--shot-keys=' + ','.join(steps)] + \
        ([] if os.environ.get('UITEST_OCR') else ['--shot-text']) + (extra or [])
    for f in os.listdir(OUT):                       # старые снимки этого имени и временные
        if re.fullmatch('(' + re.escape(name) + '|' + tmp + r')(-\d+)?\.(bmp|png|txt)', f):
            os.remove(os.path.join(OUT, f))
    r = subprocess.run(args, capture_output=True, timeout=timeout, env=child_env())
    out = r.stdout.decode('utf-8', 'replace')
    m = re.search(r'\((\d+)x(\d+) клеток\); событий мыши дошло до интерфейса: (\d+); частная память (\d+) КБ', out)
    if r.returncode != 0 or not m:
        raise RuntimeError('клиент rc=%d: %s' % (r.returncode, out.strip()))
    for f in os.listdir(OUT):
        mm = re.fullmatch(tmp + r'((-\d+)?\.(bmp|txt))', f)
        if mm:
            os.replace(os.path.join(OUT, f), os.path.join(OUT, name + mm.group(1)))
    info = {'rc': r.returncode, 'mouse': int(m.group(3)), 'mem_kb': int(m.group(4)), 'out': out,
            'snap_kb': [int(v) for v in re.findall(r'снимок: [^\n(]*?; частная память (\d+) КБ', out)]}
    s = Shot(path, int(m.group(1)), int(m.group(2)), info)
    snaps = sorted([f for f in os.listdir(OUT) if re.fullmatch(re.escape(name) + r'-\d+\.bmp', f)],
                   key=lambda f: int(re.search(r'-(\d+)\.bmp$', f).group(1)))
    s.snaps = [Shot(os.path.join(OUT, f), s.cols, s.rows, info) for f in snaps]
    return s


if __name__ == '__main__':
    # ручной прогон: python shot.py ИМЯ "шаги" [WxH]
    s = run(sys.argv[2] if len(sys.argv) > 2 else '', sys.argv[1], sys.argv[3] if len(sys.argv) > 3 else None)
    print(s.info['out'].strip())
    print(s.png())
    print(s.dump())
