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


def cpu_tail(keys, tail=5.0):
    """CPU клиента (% одного ядра) за последние tail секунд прогона: опрос дочернего процесса через psutil."""
    import threading, time, psutil
    smp, done = [], threading.Event()

    def poll():
        me = psutil.Process()
        while not done.is_set():
            for ch in me.children():
                try:
                    ct = ch.cpu_times()
                    smp.append((time.perf_counter(), ct.user + ct.system))
                except psutil.Error:
                    pass
            time.sleep(0.2)
    th = threading.Thread(target=poll)
    th.start()
    try:
        snap(keys)
    finally:
        done.set()
        th.join()
    if len(smp) < 3:
        return float('nan')
    end = smp[-1][0] - 0.6                         # без снимка и выхода в самом конце
    w = [s for s in smp if end - tail <= s[0] <= end]
    return 100.0 * (w[-1][1] - w[0][1]) / max(w[-1][0] - w[0][0], 0.1)


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
    # CPU в эфире (по просьбе дизайнера): последние 5 с из 7-секундного ожидания, медиана 3 прогонов
    c = {}
    for name, keys in (('звонок без эфира', j + ',wait:7000'), ('эфир 720p', live + ',wait:7000'),
                       ('эфир 480p', l480 + ',wait:7000')):
        c[name] = statistics.median(cpu_tail(keys) for _ in range(3))
    print('  CPU (%% одного ядра, медиана 3): ' + ', '.join('%s %.1f %%' % kv for kv in c.items()))
    ok('CPU эфира 720p разумный (<= 25 %% ядра; %.1f %%)' % c['эфир 720p'], c['эфир 720p'] <= 25)
    # 10 запусков/остановок подряд: эфир снова поднимается, память возвращается, без падения
    stop = ',click:%d:%d,wait:500' % ind if ind else ''
    again = ',click:%d:%d,wait:300,click:%d:%d,wait:1500' % (x, row, btn[0] + 2, btn[1])
    cyc = live + (stop + again) * 9
    r5 = [snap(cyc) for _ in range(3)]               # сбой был плавающим (~1 из 3) — смотрим все три прогона
    up = sum(has(t, 'ЭФИР') for t, _ in r5)
    ok('10-й запуск эфира подряд работает (%d из 3 прогонов)' % up, up == 3)
    m5 = statistics.median(i['mem_kb'] for _, i in r5)
    print('  память после 10 запусков: %d КБ (один запуск %d КБ, разница %+d)' % (m5, m['эфир 720p'], m5 - m['эфир 720p']))
    ok('память после 10 запусков вернулась (разница <= 256 КБ)', m5 - m['эфир 720p'] <= 256)
    r6 = [snap(cyc + stop) for _ in range(3)]
    down = sum(not has(t, 'ЭФИР') for t, _ in r6)
    m6 = statistics.median(i['mem_kb'] for _, i in r6)
    print('  после 10-й остановки: %d КБ, по прогонам %s (звонок без эфира %d КБ, остаток %+d)' %
          (m6, [i['mem_kb'] for _, i in r6], m['звонок без эфира'], m6 - m['звонок без эфира']))
    ok('после 10-й остановки эфира нет, клиент жив (%d из 3)' % down, down == 3)
    ok('остаток памяти после стопа <= 256 КБ к звонку (%+d)' % (m6 - m['звонок без эфира']), m6 - m['звонок без эфира'] <= 256)
    print('Итог: %d ok, %d FAIL' % (sum(res), len(res) - sum(res)))
    return 0 if all(res) else 1


if __name__ == '__main__':
    sys.exit(main())
