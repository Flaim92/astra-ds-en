# -*- coding: utf-8 -*-
"""Английский интерфейс (LANG=en_US): непереведённые строки и обрезанные слова.
Проход по экранам (шаги ищут места по демо-данным и ASCII-значкам, а не по переводимым подписям), размеры
50x12 / 120x34 / 200x60. Кириллица на экране, которой нет в демо-данных (слова из строк src/demo.c), —
непереведённое; источник ищется в src/*.c (ui.c — дизайнер; store/net/main/login/... — разработчик).
Обрезанное слово: латинское слово экрана (от 3 букв) — строгое начало слова из английской таблицы (src/ui_en.h),
само словом не являющееся, без «...» после.
    python tools/uitest/lang_scan.py"""
import glob, os, re, sys
os.environ['UITEST_LANG'] = 'en_US.UTF-8'
import shot
import windows as W

ROOT = shot.ROOT
SRC = os.path.join(ROOT, 'src')
CYR = re.compile(r'[А-Яа-яЁё][А-Яа-яЁё\-]*')


def demo_words():
    s = open(os.path.join(SRC, 'demo.c'), encoding='utf-8').read()
    s = s.encode('utf-8').decode('unicode_escape').encode('latin-1', 'ignore').decode('utf-8', 'ignore') if '\\u' in s else s
    ws = set(w.lower() for w in CYR.findall(s))
    # демо-данные вне demo.c: посты форума (store.c, массив nm[] около «Как настроить звук?»)
    st = open(os.path.join(SRC, 'store.c'), encoding='utf-8').read().splitlines()
    for i, l in enumerate(st):
        if 'Как настроить звук?' in l:
            for x in st[max(0, i - 3):i + 6]:
                ws.update(w.lower() for w in CYR.findall(x))
    return ws


def en_words():
    s = open(os.path.join(SRC, 'ui_en.h'), encoding='utf-8').read()
    ws = set()
    for m in re.finditer(r'\{\s*"(?:[^"\\]|\\.)*"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', s):
        ws.update(w.lower() for w in re.findall(r'[A-Za-z]{2,}', m.group(1)))
    return ws


def src_of(word):
    """Файлы src, где слово встречается в строковом литерале."""
    hits = []
    for f in sorted(glob.glob(os.path.join(SRC, '*.c'))):
        if os.path.basename(f) in ('demo.c', 'selftest.c'):
            continue
        t = open(f, encoding='utf-8', errors='replace').read()
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', t):
            if word in lit:
                hits.append(os.path.basename(f))
                break
    return hits


def screens():
    """(имя, шаги, размер, доп.флаги) — экраны для просмотра."""
    S = 'click:3:7,wait:50'
    Z = '120x34'
    out = [('главная', 'wait:50', Z, None), ('друзья-все', 'tab,tab,right,wait:50', Z, None),
           ('друзья-ожидание', 'tab,tab,right,right,wait:50', Z, None), ('друзья-блок', 'tab,tab,right,right,right,wait:50', Z, None),
           ('справка', 'f1,wait:50', Z, None), ('справка-60x20', 'f1,wait:50', '60x20', None),
           ('вход', 'ctrl+l,wait:50', Z, None), ('старт', 'wait:100', Z, ['--demo-start']), ('старт-ошибка', 'wait:100', Z, ['--demo-start=err']),
           ('мало-места', 'wait:50', '49x11', None), ('сервер', S, Z, None)]
    ch = S + ',click:' + W.at(S, 'general') + ',wait:150'
    inp = W.at(ch, '#general', last=True)
    out += [('канал', ch, Z, None), ('ctrl+k', ch + ',ctrl+k,wait:50', Z, None), ('поиск', ch + ',ctrl+f,t:привет,enter,wait:300', Z, None),
            ('файл', ch + ',ctrl+o,wait:50', Z, None), ('эмодзи', ch + ',click:' + W.at(ch, ':)', last=True) + ',wait:50', Z, None),
            ('меню-сообщения', ch + ',rclick:' + W.at(ch, 'привет! в #новости', dx=20) + ',wait:50', Z, None),
            ('карточка', ch + ',click:' + W.at(ch, '<Алиса>', dx=1) + ',wait:100', Z, None),
            ('закреплённые', ch + ',click:' + W.at(ch, '-o', dx=1) + ',wait:100', Z, None),
            ('входящие', ch + ',click:' + W.at(ch, '[_]', dx=1) + ',wait:100', Z, None),
            ('меню-сервера', S + ',click:12:0,wait:50', Z, None),
            ('меню-канала', S + ',rclick:' + W.at(S, 'general') + ',wait:50', Z, None),
            ('добавить-сервер', 'click:' + W.at('wait:50', '[ +  ]', tol=0) + ',wait:50', Z, None),
            ('статус', 'click:12:32,wait:50', Z, None),
            ('ЛС', 'click:' + W.at('wait:50', 'Алиса', last=True) + ',wait:150', Z, None),
            ('новая-ЛС', 'click:' + W.at('wait:50', 'Учёба', dx=0) .replace(':', ':', 1) + ',wait:50', Z, None)]
    for chn in ('игры', 'вопросы', 'медленный', 'музыка'):
        out.append(('#' + chn, S + ',click:' + W.at(S, chn, tol=0) + ',wait:200', Z, None))
    v = S + ',click:' + W.at(S, '<) ОБЩИЙ', dx=3) + ',wait:300'
    out.append(('голос-экран', v, Z, None))
    g = 'click:' + W.at('wait:50', '(*)', dx=1) + ',wait:50'
    for n in range(6):
        out.append(('настройки-%d' % n, g + ',down' * n + ',wait:100', '120x50', None))
    for sz in ('50x12', '200x60'):
        out.append(('главная-' + sz, 'wait:50', sz, None))
        out.append(('канал-' + sz, 'click:3:7,wait:50,click:' + W.at('click:3:7,wait:50', 'general', size=sz) + ',wait:150', sz, None))
    return out


def main():
    dw, ew = demo_words(), en_words()
    untr, trunc = {}, {}
    for name, keys, size, extra in screens():
        try:
            s = shot.run(keys, 'lang-' + name, size, extra=extra)
        except Exception as e:
            print('  экран %s не снят: %s' % (name, e))
            continue
        for ln in s.text():
            for w in CYR.findall(ln):
                wl = w.lower()
                # демо-данные и их обрезки (под окнами, сокращения серверов «[ Ку ]») — не интерфейс
                short_demo = len(w) <= 3 and any(wl in d for d in dw)          # обрезок демо-слова
                cut_demo = len(w) >= 4 and any(d.startswith(wl) for d in dw)   # начало демо-слова под окном
                if len(w) > 1 and wl not in dw and not short_demo and not cut_demo:
                    untr.setdefault(w, (name, ln.strip()[:90]))
            for m in re.finditer(r'[A-Za-z]{3,}', ln):
                if m.group(0).isupper():
                    continue                            # BOT, VLESS, ESC — метки, не слова текста
                w = m.group(0).lower()
                after = ln[m.end():m.end() + 3]
                if w not in ew and not after.startswith('...') and any(x.startswith(w) and x != w for x in ew):
                    trunc.setdefault(w, (name, size, ln.strip()[:90]))
    by_owner = {'ui.c (дизайнер)': [], 'другие файлы (разработчик)': [], 'не найдено в src': []}
    for w, (scr, ln) in sorted(untr.items()):
        files = src_of(w)
        key = 'не найдено в src' if not files else ('ui.c (дизайнер)' if files == ['ui.c'] or 'ui.c' in files and len(files) == 1 else
                                                    'другие файлы (разработчик)' if 'ui.c' not in files else 'ui.c (дизайнер)')
        by_owner[key].append('%s  [%s] %s  {%s}' % (w, scr, ln, ','.join(files)))
    print('НЕПЕРЕВЕДЁННОЕ (кириллица вне демо-данных): %d слов' % len(untr))
    for k, v in by_owner.items():
        print('== %s: %d' % (k, len(v)))
        for x in v:
            print('  ' + x)
    print('ОБРЕЗАННЫЕ СЛОВА: %d' % len(trunc))
    for w, (scr, sz, ln) in sorted(trunc.items()):
        print('  %s  [%s %s] %s' % (w, scr, sz, ln))
    return 0 if not untr and not trunc else 1


if __name__ == '__main__':
    sys.exit(main())
