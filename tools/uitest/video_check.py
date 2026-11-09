# -*- coding: utf-8 -*-
"""Видео-версия (astra-ds-video.exe): демонстрация экрана в демо-звонке, СКРЫТО (--demo --shot), без видимых окон.
Окно «Демонстрация экрана» показывает заголовки НАСТОЯЩИХ окон пользователя — поэтому снимки этого набора
удаляются сразу после разбора (BMP и текст), а в отчёт идут только числа (сколько строк-источников), не заголовки.
Проверки: форма (источник: окна и «Весь экран», качество 720p24 по умолчанию / 480p15), «Начать трансляцию» ->
«(o) ЭФИР» в строке состояния, режим стримера на время эфира, остановка щелчком, автостоп при выходе из звонка и
смене канала; память эфира 720p / 480p к эталону (build-ref, пара в одном прогоне).
    MC_BIN=.../astra-ds-video.exe python tools/uitest/video_check.py"""
import glob, os, re, statistics, sys
import shot
import windows as W

res = []


def ok(what, cond, detail=''):
    res.append(bool(cond))
    print('  [%s] %s%s' % ('ok' if cond else 'FAIL', what, (' - ' + detail) if detail and not cond else ''))


def snap(keys, size='120x34'):
    """Снимок, текст в память, файлы сразу удаляются (в них могут быть заголовки чужих окон)."""
    s = shot.run(keys, 'vid', size)
    t = list(s.text())
    info = dict(s.info)
    for f in glob.glob(os.path.join(shot.OUT, 'vid*')) + glob.glob(os.path.join(shot.OUT, '_cur*')):
        try:
            os.remove(f)
        except OSError:
            pass
    return t, info


def has(t, s):
    return any(s in l for l in t)


def main():
    S = 'click:3:7,wait:50'
    v = S + ',click:' + W.at(S, '<) ОБЩИЙ', dx=3) + ',wait:300'
    j = v + ',click:' + W.at(v, 'Присоединиться') + ',wait:800'
    t, _ = snap(j)
    row = next((y for y, l in enumerate(t) if '([^] )' in l), None)
    if row is None:
        ok('панель голоса с кнопкой [^]', False)
        return 1
    x = t[row].index('([^] )') + 2
    form = j + ',click:%d:%d,wait:300' % (x, row)
    t, _ = snap(form)
    t_form = t
    ok('форма «Демонстрация экрана»', has(t, 'Демонстрация экрана'))
    sy = next((y + 1 for y, l in enumerate(t) if 'ИСТОЧНИК' in l), None)
    qy = next((y + 1 for y, l in enumerate(t) if 'КАЧЕСТВО' in l), None)
    val = lambda tt, y: (re.search(r'<\s*(.*?)\s*>', tt[y]) or re.search('', '')).group(1) if y is not None else ''
    ok('качество по умолчанию 720p 24 к/с', '720p' in val(t, qy))
    t2, _ = snap(form + ',click:%d:%d,wait:100' % (t[qy].rindex('>'), qy))
    ok('второй вариант качества — 480p 15 к/с', '480p' in val(t2, qy))
    found = False
    for k in range(1, 15):                       # перебор источников стрелкой; значения (заголовки окон) не печатаются
        tk, _ = snap(form + (',click:%d:%d' % (t[sy].rindex('>'), sy)) * k + ',wait:100')
        if 'Весь экран' in val(tk, sy):
            found = True
            break
    ok('в источниках есть «Весь экран» (перебор стрелкой)', found)
    n_src = sum(1 for l in t if re.search(r'\(\s?[ *xo]\s?\)|\[[ x]\]', l))
    print('  строк-вариантов в форме: %d (заголовки не печатаются)' % n_src)
    btn = next(((l.index('Начать трансляцию'), y) for y, l in enumerate(t) if 'Начать трансляцию' in l), None)
    ok('кнопка «Начать трансляцию»', btn is not None)
    if not btn:
        return 1
    live = form + ',click:%d:%d,wait:1500' % (btn[0] + 2, btn[1])
    t, info = snap(live)
    ok('эфир: «(o) ЭФИР» в строке состояния', has(t, 'ЭФИР'))
    mem720 = info['mem_kb']
    # режим стримера на время эфира: слова-тревоги и т. п. скрыты — проверяем пометку в Конфиденциальности
    g = live + ',click:' + W.at('wait:50', '(*)', dx=1) + ',wait:50'
    tg, _ = snap(g)
    pc = next(((l.index('Конфиденциальность'), y) for y, l in enumerate(tg) if 'Конфиденциальность' in l), None)
    if pc:
        tp, _ = snap(g + ',click:%d:%d,wait:100' % pc)
        ok('во время эфира режим стримера включён ([x] Скрывать личное)', has(tp, '[x] Скрывать личное'))
    # остановка щелчком по индикатору
    ind = next(((l.index('ЭФИР'), y) for y, l in enumerate(t) if 'ЭФИР' in l), None)
    if ind:
        ts, _ = snap(live + ',click:%d:%d,wait:500' % ind)
        ok('щелчок по «ЭФИР» останавливает трансляцию', not has(ts, 'ЭФИР'))
    tf, _ = snap(live + ',f7,wait:500')
    ok('автостоп при выходе из звонка (F7)', not has(tf, 'ЭФИР'))
    tc, _ = snap(live + ',click:' + W.at(S, 'general') + ',wait:500')
    ok('при переходе в текстовый канал эфир продолжается (как в Discord)', has(tc, 'ЭФИР'))
    # 480p
    l480 = form + ',click:%d:%d,wait:100,click:%d:%d,wait:1500' % (t_form[qy].rindex('>'), qy, btn[0] + 2, btn[1])
    t4, _ = snap(l480)
    ok('эфир 480p 15 к/с запускается', has(t4, 'ЭФИР'))
    m = {}
    for name, keys in (('звонок без эфира', j + ',wait:1500'), ('эфир 720p', live), ('эфир 480p', l480)):
        m[name] = statistics.median(snap(keys)[1]['mem_kb'] for _ in range(3))
    print('  память (медиана 3 прогонов, частная): ' + ', '.join('%s %d КБ' % kv for kv in m.items()))
    ok('эфир 720p в пределах цели 9,6 МБ (%d КБ; потолок 10 МБ)' % m['эфир 720p'], m['эфир 720p'] <= 9830)
    ok('эфир 480p в пределах 10 МБ (%d КБ)' % m['эфир 480p'], m['эфир 480p'] <= 10240)
    print('Итог: %d ok, %d FAIL' % (sum(res), len(res) - sum(res)))
    return 0 if all(res) else 1


if __name__ == '__main__':
    sys.exit(main())
