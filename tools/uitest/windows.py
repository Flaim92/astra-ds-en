# -*- coding: utf-8 -*-
"""Матрица окон клиента в настоящем окне (скрыто, --demo --shot). Запуск:
    python tools/uitest/windows.py [фильтр]        — все окна (или имена с подстрокой фильтра)
Итог: таблица ok/FAIL по окнам, out/report.txt, снимки out/<окно>.png.
Координаты щелчков ищутся распознаванием текста на снимке «до щелчка» (at()), поэтому сценарии не
ломаются от сдвига раскладки. Проверки каждого окна: ожидаемые надписи, мышь дошла (WM_* -> K_MOUSE),
память процесса (<= 10 МБ, рост от интерфейса <= 1 МБ), снимок не пустой, ровная сетка (нет
клеток-обрубков по краям), размеры 50x12..200x60."""
import os, subprocess, sys, time
import numpy as np
import shot

BASE_KB = None                                  # частная память процесса на главном экране 120x34
LIMIT_KB = 10 * 1024
UI_LIMIT_KB = 1024
RES = []                                        # (окно, проверка, ok, подробность, снимок)
_cache = {}


def at(prefix, text, size='120x34', dx=0, dy=0, last=False, extra=None, tol=None):
    """Клетка надписи text на экране после шагов prefix: 'X:Y' для click:/hover:."""
    key = (prefix, text, size, last, tuple(extra or ()), tol)
    if key not in _cache:
        s = shot.run(prefix, 'at', size, extra=extra)
        p = None
        if last:
            for y in range(s.rows - 1, -1, -1):
                ln = shot.norm(s.text()[y])
                k = ln.find(shot.norm(text))
                if k >= 0:
                    p = (k, y)
                    break
        if p is None:
            p = s.find(text, tol)
        if p is None:
            raise LookupError('не найдено «%s» после [%s] %s' % (text, prefix, size))
        _cache[key] = p
    x, y = _cache[key]
    return '%d:%d' % (x + dx, y + dy)


_srv_cache = {}


def server_xy(name, size='120x34'):
    """Шаги до значка сервера по его названию: значки без подписей, часть — в папках («[ N ]», щелчок раскрывает).
    Щёлкаем по каждому значку, читаем шапку колонки каналов; папки раскрываем и ищем внутри. Возвращает «X:Y» или
    «X:Y,wait:50,click:X2:Y2» (раскрыть папку, затем сервер). Кэш на прогон."""
    key = (name, size)
    if key in _srv_cache:
        return _srv_cache[key]

    def mids(t):
        return [y for y in range(4, len(t) - 3) if t[y][1:7].strip() and t[y - 1][1:7].strip() and t[y + 1][1:7].strip()]

    def scan(prefix, depth):
        t = shot.run(prefix + 'wait:50', 'srv-поиск', size).text()
        for y in mids(t):
            step = '3:%d' % y
            r = shot.run(prefix + 'click:' + step + ',wait:50', 'srv-поиск', size).text()
            if name in r[0][9:45]:
                return step
            if depth == 0 and r[0][9:45] == t[0][9:45] and [l[:9] for l in r] != [l[:9] for l in t]:   # раскрылась папка
                inner = scan(prefix + 'click:' + step + ',wait:50,', 1)
                if inner:
                    return step + ',wait:50,click:' + inner
        return None
    found = scan('', 0)
    if not found:
        raise LookupError('сервер «%s» не найден в колонке серверов' % name)
    _srv_cache[key] = found
    return found


def check(win, what, ok, detail='', snap=''):
    RES.append((win, what, bool(ok), detail, snap))
    print('  [%s] %s: %s%s' % ('ok' if ok else 'FAIL', win, what, (' - ' + detail) if detail and not ok else ''))


def grid_even(s):
    """Ровная сетка: размер картинки — точно cols x rows клеток."""
    H, W, _ = s.img.shape
    return W == s.cols * s.cw and H == s.rows * s.ch


def common(win, s, mouse_steps=0):
    """Общие проверки любого снимка."""
    name = os.path.basename(s.png())
    check(win, 'снимок не пустой, сетка %dx%d ровная' % (s.cols, s.rows), s.nonblank() and grid_even(s), snap=name)
    if mouse_steps:
        check(win, 'мышь: %d событий окна дошло до интерфейса' % s.info['mouse'], s.info['mouse'] >= mouse_steps,
              'ожидалось >= %d' % mouse_steps, name)
    kb = s.info['mem_kb']
    # Рост интерфейса <= 1 МБ над главным экраном — для всех окон. В голосе/звонке из роста вычитается память ядра
    # звука (WASAPI, Opus, поток), измеренная в этом же прогоне: экран канала после входа минус до входа (core_kb()).
    in_voice = any(t in win for t in ('голос-подключён', 'голос-микрофон-выкл', 'голос-звук-выкл', 'голос-смена', 'звонок'))
    core = core_kb() if in_voice else 0
    ui = None if BASE_KB is None else kb - BASE_KB - core
    ok = kb <= LIMIT_KB and (ui is None or ui <= UI_LIMIT_KB)
    check(win, 'память %d КБ (интерфейс +%s КБ%s)' % (kb, ui, ', ядро звука %d КБ вычтено' % core if core else ''), ok,
          'потолок 10 МБ, рост интерфейса над главным экраном <= 1 МБ (база %s КБ)' % BASE_KB, name)
    return name


CORE_KB = None


def core_kb():
    """Память ядра звука: экран голосового канала ОБЩИЙ после «Присоединиться» минус до него (один процесс на каждый)."""
    global CORE_KB
    if CORE_KB is None:
        pre = 'click:3:7,wait:50'
        v = pre + ',click:' + at(pre, '<) ОБЩИЙ', dx=3) + ',wait:100'
        a = shot.run(v, 'core_a', '120x34').info['mem_kb']
        b = shot.run(v + ',click:' + at(v, 'Присоединиться') + ',wait:600', 'core_b', '120x34').info['mem_kb']
        CORE_KB = max(0, b - a)
        print('  ядро звука: %d КБ (экран канала %d -> в голосе %d)' % (CORE_KB, a, b))
    return CORE_KB


def expect(win, s, texts, absent=(), name=''):
    miss = [t for t in texts if not s.has(t)]
    extra = [t for t in absent if s.has(t, tol=0)]       # «лишнее» — только точное совпадение (иначе «не заданы» ~ «не задан»)
    check(win, 'надписи: ' + ', '.join(texts[:4]) + ('...' if len(texts) > 4 else ''), not miss and not extra,
          ('нет: ' + ', '.join(miss) if miss else '') + (' лишние: ' + ', '.join(extra) if extra else ''), name)


def nmouse(keys):
    import re
    return len(re.findall(r'(?:^|,)(?:click|rclick|mclick|hover|wheelup|wheeldown):', keys))


def win(name, keys, texts, size='120x34', absent=(), mouse=True, extra=None):
    """Один экран: прогон, общие проверки, ожидаемые надписи. Возвращает Shot."""
    s = shot.run(keys, name, size, extra=extra)
    nm = common(name, s, nmouse(keys) if mouse else 0)
    expect(name, s, texts, absent, nm)
    return s


# ---------------------------------------------------------------- сценарии
SRV = 'click:3:7,wait:50'                       # первый сервер «Уютный уголок»


def t_home():
    global BASE_KB
    s = win('главная-друзья', 'wait:50', ['Друзья', 'В сети', 'Все', 'Добавить в друзья', 'Написать', 'Личные сообщения'], mouse=False)
    BASE_KB = s.info['mem_kb']
    for tab, want in (('Все', 'ВСЕ'), ('Ожид.', 'ОЖИДАНИЕ'), ('Блок.', 'ЗАБЛОКИРОВАННЫЕ')):
        win('друзья-вкладка-' + tab.strip('.'), 'click:' + at('wait:50', tab) + ',wait:50', [want])
    win('друзья-добавить', 'click:' + at('wait:50', 'Добавить в друзья') + ',wait:50', ['Добавить в друзья'])
    win('друзья-наведение-Написать', 'hover:' + at('wait:50', 'Написать') + ',wait:50', ['Написать'])


def t_friends():
    H = 'wait:50'
    dots = at(H, '...', dx=1)
    m = H + ',click:' + dots + ',wait:50'
    win('друзья-меню-...', m, ['Написать', 'Удалить из друзей'])
    win('друзья-окно-удалить', m + ',click:' + at(m, 'Удалить из друзей') + ',wait:50',
        ['Удалить из друзей', 'Отмена', 'Удалить друга'])
    d = m + ',click:' + at(m, 'Удалить из друзей') + ',wait:50'
    win('друзья-удалить-отмена', d + ',click:' + at(d, 'Отмена') + ',wait:50', ['В СЕТИ'], absent=['Удалить друга'])
    win('друзья-написать', H + ',click:' + at(H, 'Написать') + ',wait:100', ['Написать @Алиса'])
    win('друзья-щелчок-строки', H + ',click:' + at(H, 'Сонный Кот') + ',wait:100', ['Сонный Кот'])
    o = H + ',click:' + at(H, 'Ожид.') + ',wait:50'
    win('друзья-ожидание', o, ['ОЖИДАНИЕ', 'Принять', 'Отклонить', 'Отменить', 'Входящий запрос дружбы'])
    win('друзья-принять', o + ',click:' + at(o, 'Принять') + ',wait:100', [])
    win('друзья-отклонить', o + ',click:' + at(o, 'Отклонить') + ',wait:100', [])
    b = H + ',click:' + at(H, 'Блок.') + ',wait:50'
    win('друзья-заблокированные', b, ['ЗАБЛОКИРОВАНО', 'Разблокировать'])
    # клавиатура: Tab Tab — фокус в списке, Left/Right вкладки, Up/Down, Del, '.'
    win('друзья-клав-right', 'tab,tab,right,wait:50', ['ВСЕ ДРУЗЬЯ'], mouse=False)
    win('друзья-клав-right3', 'tab,tab,right,right,right,wait:50', ['ЗАБЛОКИРОВАНО'], mouse=False)
    win('друзья-клав-точка', 'tab,tab,down,t:.,wait:50', ['Удалить из друзей'], mouse=False)
    win('друзья-клав-del', 'tab,tab,del,wait:50', ['Удалить друга'], mouse=False)
    win('друзья-клав-enter', 'tab,tab,enter,wait:100', ['Написать @'], mouse=False)
    for sz in ('50x12', '80x24', '100x30', '150x45', '200x60'):
        win('друзья-' + sz, 'wait:50', ['Друзья' if sz != '50x12' else '< (В сети) >'], size=sz, mouse=False)   # 50x12: переключатель вкладок


def t_newdm():
    H = 'wait:50'
    s = shot.run(H, 'at', '120x34')
    p = s.find('Личные сообщения')
    ln = s.text()[p[1]]
    plus = '%d:%d' % (ln.rindex('+'), p[1])
    n = H + ',click:' + plus + ',wait:50'
    win('новая-лс-выберите-друзей', n, ['Выберите друзей', 'Создать ЛС'])
    win('новая-лс-фильтр', n + ',t:али,wait:50', ['Алиса'], absent=['Ночной Филин'])
    win('новая-лс-фильтр-пусто', n + ',t:zzzz,wait:50', ['Выберите друзей'])
    win('новая-лс-enter', n + ',down,enter,wait:100', [])
    win('новая-лс-esc', n + ',esc,wait:50', ['Друзья'], absent=['Выберите друзей'])
    win('новая-лс-щелчок-мимо', n + ',click:100:30,wait:50', [], absent=['Выберите друзей'])
    win('новая-лс-найти-или-начать', 'click:' + at(H, 'Найти или начать беседу') + ',wait:50', ['Выберите друзей'])
    for sz in ('60x16', '200x60'):
        s = shot.run(H, 'at', sz)
        p = s.find('Личные')
        if not p:
            check('новая-лс-' + sz, 'есть «Личные сообщения»', False)
            continue
        win('новая-лс-' + sz, H + ',click:%d:%d,wait:50' % (s.text()[p[1]].rindex('+'), p[1]), ['Выберите'], size=sz)


def t_search():
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    win('поиск-ctrl+f', ch + ',ctrl+f,wait:50', ['Enter искать', 'Esc закрыть'], mouse=True)
    win('поиск-результаты', ch + ',ctrl+f,t:привет,enter,wait:300', ['результат'])
    r = ch + ',ctrl+f,t:привет,enter,wait:300'
    win('поиск-ничего', ch + ',ctrl+f,t:qqqzzz,enter,wait:300', ['Ничего не найдено'])
    win('поиск-щелчок', ch + ',click:' + at(ch, 'Поиск') + ',t:Боб,enter,wait:300', ['результат'])
    win('поиск-down-enter', r + ',down,enter,wait:100', ['Написать #general'], absent=['результат'])
    win('поиск-esc', r + ',esc,wait:50', ['general'], absent=['результат'])
    win('поиск-узко-70x24', SRV + ',click:20:6,wait:100,ctrl+f,t:привет,enter,wait:300', ['результат'], size='70x24')
    win('поиск-широко-200x60', SRV + ',click:20:6,wait:100,ctrl+f,t:привет,enter,wait:300', ['результат'], size='200x60')
    win('поиск-лс', 'click:' + at('wait:50', 'Алиса', last=True) + ',wait:100,ctrl+f,t:созвон,enter,wait:300', ['результат'])
    win('поиск-смена-сервера', r + ',click:' + server_xy('Другой сервер') + ',wait:50', [], absent=['результат'])
    win('поиск-друзья-закрывает', r + ',click:3:1,wait:50', ['Друзья'], absent=['результат'])


def t_file():
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    inp = at(ch, 'Написать #general')
    win('файл-ctrl+o', ch + ',ctrl+o,wait:50', ['Отправка файла', 'Esc'])
    win('файл-плюс', ch + ',click:' + at(ch, 'Написать #general', dx=-5) + ',wait:50', ['Отправка файла'])
    win('файл-второй-плюс', ch + ',click:' + at(ch, 'Написать #general', dx=-2) + ',wait:50', [])
    win('файл-нет-такого', ch + ',ctrl+o,t:Z:/нет/такого.bin,enter,wait:100', ['Файл не отправлен'])
    big = os.path.join(shot.OUT, 'big11mb.bin')
    if not os.path.exists(big):
        with open(big, 'wb') as f:
            f.truncate(11 * 1024 * 1024)
    win('файл-больше-10мб', ch + ',ctrl+o,t:' + big.replace(',', '') + ',enter,wait:100', ['Файл не отправлен'])
    small = os.path.join(shot.OUT, 'small.txt')
    with open(small, 'w') as f:
        f.write('test\n' * 1000)
    win('файл-отправка', ch + ',click:' + inp + ',t:подпись,ctrl+o,t:"' + small + '",enter,wait:300', [])
    win('файл-esc-черновик', ch + ',click:' + inp + ',t:черновик,ctrl+o,esc,wait:50', ['черновик'], absent=['Отправка файла'])
    win('файл-щелчок-мимо', ch + ',click:' + inp + ',t:черновик,ctrl+o,click:' + at(ch, 'всем привет :wave:') + ',wait:50', ['черновик'], absent=['Отправка файла'])
    win('файл-узко-50x12', 'click:3:7,wait:50,click:15:6,wait:100,ctrl+o,wait:50', ['Отпра', 'путь'], size='50x12')


def t_dm():
    p = at('wait:50', 'Алиса', last=True)
    win('лс-алиса', 'click:' + p + ',wait:100', ['Алиса', 'созвонимся', 'Написать @Алиса'])
    win('лс-группа', 'click:' + at('wait:50', 'Алиса, Боб') + ',wait:100', ['Алиса, Боб'])
    win('лс-звонок-F4', 'click:' + p + ',wait:100,f4,wait:300', ['Голос'])


def t_server():
    win('сервер-выберите-канал', SRV, ['Уютный уголок', 'Выберите канал слева', 'Текстовые каналы', 'Голосовые каналы'])
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    s = win('канал-лента', ch, ['general', 'Алиса', 'всем привет', 'В ЧАТЕ', 'Написать #general'])
    check('канал-лента', 'мелкая сетка: аватар ленты — слой картинки (много цветов)',
          s.fine_cells(*_cellrect(s, 'всем привет', -6, -1, 4, 2)) > 20)
    win('категория-свернуть', SRV + ',click:' + at(SRV, 'Текстовые каналы v') + ',wait:50', ['Текстовые каналы >'], absent=['новости'])
    win('канал-наведение', ch + ',hover:' + at(ch, 'новости') + ',wait:50', ['новости'])
    win('канал-подсказка-шестерёнка', ch + ',hover:' + at(ch, '(*)', dx=1) + ',wait:50', ['Настройки пользователя'])
    win('канал-подсказка-справка', ch + ',hover:' + at(ch, '(?)', dx=1) + ',wait:50', ['Справка (F1)'])
    win('канал-колесо-ленты', ch + ',wheelup:60:10,wheelup:60:10,wheelup:60:10,wheelup:60:10,wait:50', ['ниже ещё'])
    win('канал-поиск', ch + ',click:' + at(ch, 'Поиск') + ',wait:50,t:привет,enter,wait:100', ['привет'])
    return ch


def _cellrect(s, text, dx, dy, w, h):
    p = s.find(text)
    if not p:
        return (0, 0, 1, 1)
    return (max(0, p[0] + dx), max(0, p[1] + dy), w, h)


def t_messages(ch):
    msg = at(ch, 'привет! в #новости')
    win('меню-сообщения', ch + ',rclick:' + msg + ',wait:50', ['Ответить', 'Добавить реакцию', 'Копировать текст'],
        absent=['Удалить сообщение'])
    menu = ch + ',rclick:' + msg + ',wait:50'
    win('ответ', menu + ',click:' + at(menu, 'Ответить') + ',wait:50', ['Ответ пользователю Боб'])
    win('реакция-режим', menu + ',click:' + at(menu, 'Добавить реакцию') + ',wait:50', ['Реакция'])
    win('реакция-щелчок', ch + ',click:' + at(ch, ':pepe:') + ',wait:50', [':pepe:'])
    # своё сообщение: отправить, затем правка (Up в пустом поле) и удаление (d)
    inp = at(ch, 'Написать #general')
    mine = ch + ',click:' + inp + ',t:моё тестовое,enter,wait:100'
    win('отправка', mine, ['моё тестовое'])
    win('правка-своего', mine + ',up,wait:50', ['моё тестовое'])
    win('удаление-подтверждение', mine + ',btab,t:d,wait:50', ['Удалить сообщение', 'Отмена', 'Удалить'])
    win('поле-ввода-длинное', ch + ',click:' + inp + ',t:' + 'абвгд ' * 60 + ',wait:50', ['абвгд'])
    win('поле-ввода-плюс-файл', ch + ',click:' + inp + ',click:' + at(ch, 'Написать #general', dx=-5) + ',wait:50',
        ['Отправка файла'])


def t_voice():
    pre = SRV
    v = pre + ',click:' + at(pre, '<) Игры', dx=3) + ',wait:100'
    win('голос-экран', v, ['Игры', 'Присоединиться к голосовому каналу', 'никого нет', 'Написать'])
    j = v + ',click:' + at(v, 'Присоединиться к голосовому каналу') + ',wait:600'
    s = win('голос-подключён', j, ['Отключиться', 'Голос', 'Выбрать статус канала'])
    win('голос-микрофон-выкл', j + ',f5,wait:50', ['(/)'])
    win('голос-звук-выкл', j + ',f6,wait:50', ['/x\\'])
    win('голос-смена-F11', j + ',f11,wait:50', ['смена голоса'])
    s = win('голос-отключиться-F7', j + ',f7,wait:100', ['Присоединиться'], absent=['Отключиться'])
    # ядро звука под контролем (координатор 09.11): само ядро <= 1,5 МБ; после F7 — не выше главного экрана + 0,6 МБ
    c = core_kb()
    check('голос-ядро', 'ядро звука %d КБ <= 1536 КБ' % c, c <= 1536)
    kb = s.info['mem_kb']
    # +0,8 МБ: ~0,5 МБ держат системные DLL звука Windows (9 DLL звука/COM не выгружаются), разбор разработчика 09.11
    check('голос-отключиться-F7', 'после F7 память %d КБ <= главный экран %s + 819 КБ' % (kb, BASE_KB),
          BASE_KB is None or kb <= BASE_KB + 819)
    # настоящая утечка: повторный звонок в том же процессе добавляет <= 64 КБ
    jb = at(v, 'Присоединиться к голосовому каналу')
    r = shot.run(v + ',click:' + jb + ',wait:600,f7,wait:300,snap,click:' + jb + ',wait:600,f7,wait:300,snap,'
                 'click:' + jb + ',wait:600,f7,wait:300,snap', 'голос-повторный-звонок', '120x34')
    sk = r.info['snap_kb']
    check('голос-повторный-звонок', 'после 1/2/3 звонка: %s КБ; рост 2-го и 3-го <= 64 КБ' % sk,
          len(sk) == 3 and sk[1] - sk[0] <= 64 and sk[2] - sk[1] <= 64)
    win('голос-чат-скрыть', v + ',click:' + at(v, 'x', last=False, dx=0) + ',wait:50', ['Игры'])
    win('голос-общий-с-участниками', pre + ',click:' + at(pre, 'ОБЩИЙ') + ',wait:100', ['ОБЩИЙ'])


def t_settings():
    g = 'click:' + at('wait:50', '(*)', dx=1) + ',wait:50'
    win('настройки-учётная-запись', g, ['НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ', 'Моя учётная запись', 'ТОКЕН', 'ESC'])
    for sec, want in (('Голос и видео', 'СМЕНА ГОЛОСА'), ('Конфиденциальность', 'ПАНИК-КЕЙ'), ('Горячие клавиши', 'справка')):
        win('настройки-' + sec, g + ',click:' + at(g, sec) + ',wait:50', [want])
    pc = g + ',click:' + at(g, 'Конфиденциальность') + ',wait:50'
    win('vless-сменить-ссылку', pc + ',click:' + at(pc, 'Сменить ссылку') + ',wait:50', ['vless://', 'Проверить'])
    win('vless-ссылка-ввод-главная', pc + ',click:' + at(pc, 'Сменить ссылку') + ',wait:50,t:vless://abc,wait:50', ['vless://***'])
    ch = SRV + ',click:20:6,wait:100'
    g2 = ch + ',click:' + at(ch, '(*)', dx=1) + ',wait:50'
    p2 = g2 + ',click:' + at(g2, 'Конфиденциальность') + ',wait:50'
    win('vless-ссылка-ввод-канал', p2 + ',click:' + at(p2, 'Сменить ссылку') + ',wait:50,t:vless://abc,wait:50', ['vless://***', 'VLESS'])
    win('vless-ссылка-esc', pc + ',click:' + at(pc, 'Сменить ссылку') + ',wait:50,esc,wait:50', [], absent=['Проверить'])
    win('vless-ссылка-неверная', pc + ',click:' + at(pc, 'Сменить ссылку') + ',wait:50,t:http://x,enter,wait:300', ['vless://'])
    win('настройки-esc', g + ',esc,wait:50', ['Друзья'], absent=['НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ'])
    win('настройки-выйти', g + ',click:' + at(g, 'Выйти из ') + ',wait:50', [])


def t_help():
    win('справка-F1', 'f1,wait:50', ['справка', 'МЫШЬ', 'СООБЩЕНИЯ', 'Ctrl+Q'], mouse=False)
    win('справка-60x20', 'f1,wait:50', ['справка', 'ниже ещё'], size='60x20', mouse=False)
    win('справка-колесо', 'f1,wait:50,wheeldown:30:10,wheeldown:30:10,wait:50', ['справка'], size='60x20')
    win('справка-x', 'f1,wait:50,click:' + at('f1,wait:50', '[x]', dx=1) + ',wait:50', ['Друзья'], absent=['МЫШЬ'])


def t_login():
    L = 'ctrl+l,wait:50'
    win('вход-vless', L, ['Подключение', 'vless://', 'Подключить', 'Без VLESS'], mouse=False)
    nv = L + ',click:' + at(L, 'Без VLESS') + ',wait:50'
    win('вход-форма', nv, ['С возвращением', 'Пароль', 'Вход', 'QR'])
    em = at(nv, 'Адрес электронной почты', dy=1, dx=2)
    pw = at(nv, 'Пароль', dy=1, dx=2)
    vh = at(nv, 'Вход', last=True)

    def login(user):
        return nv + ',click:' + em + ',t:' + user + ',click:' + pw + ',t:secret,click:' + vh + ',wait:100'
    win('вход-ошибка', login('bad'), ['Неверный'])
    win('вход-капча', login('captcha'), ['проверку'])
    win('вход-почта', login('mail'), ['почту'])
    win('вход-2fa', login('user'), ['Многофакторная', 'Подтвердить'])
    win('вход-sms', login('phone'), ['SMS'])
    win('вход-отключён', login('undel'), ['Восстановить'])
    win('вход-2fa-код', login('user') + ',t:123456,enter,wait:200', [], mouse=True)
    win('вход-qr-сканирован', nv + ',click:' + at(nv, 'Войти с помощью QR-кода', dy=-4) + ',wait:100', ['Проверьте телефон'])
    L8 = L + ',click:' + at(L, 'Без VLESS', size='80x24') + ',wait:50'
    win('вход-форма-80x24', L8, ['С возвращением', 'QR'], size='80x24')
    win('вход-qr-экран-80x24', L8 + ',click:' + at(L8, 'QR', size='80x24') + ',wait:100', ['QR'], size='80x24')
    win('вход-esc', L + ',esc,wait:50', [], mouse=False)
    win('вход-vless-пустая-enter', L + ',enter,wait:50', ['vless://'], mouse=False)


def t_sizes():
    for sz in ('50x12', '60x20', '80x24', '100x30', '160x50', '200x60'):
        win('размер-' + sz, 'wait:50', ['Друзья' if sz != '50x12' else '< (В сети) >'], size=sz, mouse=False)   # 50x12: переключатель вкладок
        win('размер-канал-' + sz, 'click:3:7,wait:50,tab,enter,wait:50', [], size=sz, mouse=True)
    win('слишком-маленькое-49x11', 'wait:50', ['слишком маленькое', '50x12', 'Ctrl+Q'], size='49x11', mouse=False)
    win('ресайз-200x60-50x12-120x34', 'size:200x60,wait:50,size:50x12,wait:50,size:120x34,wait:50', ['Друзья'], mouse=False)
    win('ресайз-канал', SRV + ',click:20:6,wait:50,size:60x16,wait:50,size:160x45,wait:50', ['general'])


def t_fine():
    s = win('мелкая-сетка-тест', 'wait:50,fine:60:10:20:8', [], mouse=False)
    check('мелкая-сетка-тест', 'слой fine: градиент нарисован (>500 цветов в прямоугольнике)', s.fine_cells(60, 10, 20, 8) > 500,
          str(s.fine_cells(60, 10, 20, 8)))
    s = win('мелкая-сетка-вне-сетки', 'wait:50,fine:110:30:20:8', [], mouse=False)
    check('мелкая-сетка-вне-сетки', 'слой за краем сетки отброшен', s.fine_cells(100, 26, 20, 8) < 200, str(s.fine_cells(100, 26, 20, 8)))


def t_wave1():
    """Волна 1 larperCord (дизайнер, 09.11): панель действий, меню+, профиль, закреплённые, входящие, разметка,
    спойлер, статус, черновики, переход к ответу; исправления: VLESS-окно, 50x12, плейсхолдер поиска."""
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    msg = at(ch, 'привет! в #новости', dx=20)
    win('в1-панель-действий-наведение', ch + ',hover:' + msg + ',wait:50', ['...'])
    m = ch + ',rclick:' + msg + ',wait:50'
    win('в1-меню-сообщения-расширенное', m, ['Закрепить', 'Отметить непрочитанным', 'Копировать ссылку', 'Копировать ID'])
    p = ch + ',click:' + at(ch, '<Алиса>', dx=1) + ',wait:50'
    win('в1-профиль-по-нику', p, ['Алиса', 'О СЕБЕ'])
    win('в1-профиль-esc', p + ',esc,wait:50', ['general'], absent=['О СЕБЕ'])
    win('в1-профиль-щелчок-мимо', p + ',click:100:30,wait:50', [], absent=['О СЕБЕ'])
    win('в1-профиль-по-упоминанию', ch + ',click:' + at(ch, '@Вы держи', dx=1) + ',wait:50', ['В DISCORD С'])
    win('в1-закреплённые', ch + ',click:' + at(ch, '-o', dx=1) + ',wait:100', ['Закреп'])
    win('в1-входящие', ch + ',click:' + at(ch, '[_]', dx=1) + ',wait:100', ['Непрочитанное', 'Упоминания'])
    win('в1-входящие-ctrl+i-right', ch + ',ctrl+i,wait:50,right,wait:50', ['Упоминания'], mouse=True)
    mm = ch + ',click:' + at(ch, '#новости смешное', dx=1) + ',wait:100'
    s = win('в1-плашка-канала-открывает', mm, ['новости'])
    win('в1-спойлер-щелчок', mm + ',click:' + at(mm, 'СПОЙЛЕР', dx=2) + ',wait:100', [])
    win('в1-ответ-переход', ch + ',click:' + at(ch, '@Алиса всем привет', dx=3) + ',wait:100', ['всем привет'])
    win('в1-статус-меню', ch + ',click:' + at(ch, '(В сети)', dx=-3) + ',wait:50', ['Не беспокоить', 'Невидимый'])
    st = ch + ',click:' + at(ch, '(В сети)', dx=-3) + ',wait:50'
    win('в1-статус-не-беспокоить', st + ',click:' + at(st, 'Не беспокоить') + ',wait:50', ['Не беспокоить'])
    inp = at(ch, 'Написать #general')
    d = ch + ',click:' + inp + ',t:черновик1,click:' + at(ch, 'новости') + ',wait:50,click:' + at(SRV, 'general') + ',wait:50'
    win('в1-черновики-по-каналам', d, ['черновик1'])
    win('в1-разметка-отправка', ch + ',click:' + inp + ',t:**жир** __под__ `код` ||спой|| > цит,enter,wait:100', ['жир', 'код'])
    # исправления после 1-го прогона
    win('в1-друзья-50x12-имена', 'wait:50', ['Алиса'], size='50x12', mouse=False)
    win('в1-поиск-плейсхолдер', ch + ',ctrl+f,wait:50', ['Поиск'])


def t_wave1b():
    """Волна 1, вторая часть (дизайнер ~08:10): Ctrl+K, пикер эмодзи, поле до 10 строк, автодополнение,
    «НОВОЕ», переход к сообщению и «Вернуться к настоящему», перепроверка профиля и подсказок."""
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    inp = at(ch, 'Написать #general')
    k = ch + ',ctrl+k,wait:50'
    win('в1б-ctrl+k', k, ['Алиса, Боб', 'новости', 'Esc закрыть'])
    win('в1б-ctrl+k-фильтр-другой-сервер', k + ',t:чат,wait:50', ['чат', 'Другой сервер'])
    win('в1б-ctrl+k-фильтр-кириллица-регистр', k + ',t:НОВ,wait:50', ['новости'])
    win('в1б-ctrl+k-enter', k + ',t:новости,enter,wait:100', ['Написать #новости'])
    win('в1б-ctrl+k-down-enter', k + ',down,down,enter,wait:100', [], absent=['Esc закрыть'])
    win('в1б-ctrl+k-esc', k + ',esc,wait:50', ['Написать #general'], absent=['Esc закрыть'])
    km = k + ',t:муз,wait:50'                    # пустой список — сначала беседы; ищем набором
    win('в1б-ctrl+k-щелчок', km + ',click:' + at(km, '# музыка', tol=0, dx=3) + ',wait:100', ['Написать #музыка'])
    win('в1б-ctrl+k-щелчок-мимо', k + ',click:5:30,wait:50', [], absent=['Up/Down выбор  Enter открыть'])
    win('в1б-ctrl+k-ничего', k + ',t:qqqzzz,wait:50', [])
    win('в1б-ctrl+k-50x12', 'click:3:7,wait:50,ctrl+k,wait:50', [], size='50x12')
    e = ch + ',click:' + at(ch, ':)', last=True) + ',wait:50'
    win('в1б-эмодзи-пикер', e, ['Эмодзи', 'найти'])
    win('в1б-эмодзи-фильтр-огонь', e + ',t:огонь,wait:50', ['🔥'])
    win('в1б-эмодзи-фильтр-fire', e + ',t:fire,wait:50', ['🔥'])
    s = win('в1б-эмодзи-enter-вставка', e + ',right,down,enter,wait:50', [], absent=['найти:', 'Написать #general'])   # что-то вставлено (набор эмодзи меняется)
    win('в1б-эмодзи-esc', e + ',esc,wait:50', [], absent=['найти:'])
    win('в1б-эмодзи-x', e + ',click:' + at(e, 'x', last=False, dx=0) + ',wait:50', [])
    msg = at(ch, 'привет! в #новости', dx=20)
    hv = ch + ',hover:' + msg + ',wait:50'
    win('в1б-реакция-+:)', hv + ',click:' + at(hv, '+:)', dx=1) + ',wait:50', ['Реакция'])
    win('в1б-поле-shift-enter-строки', ch + ',click:' + inp + ',t:' + 'строка длинная текст ' * 30 + ',wait:50',
        ['строка длинная'])
    s = shot.run(ch + ',click:' + inp + ',t:' + 'слово ' * 400 + ',wait:50', 'в1б-поле-10-строк', '120x34')
    t = s.text()
    rows = [y for y, l in enumerate(t) if 'слово' in l]
    check('в1б-поле-10-строк', 'поле не больше 10 строк текста (%d), лента >= 6 строк' % len(rows), 1 < len(rows) <= 10
          and min(rows) >= 7, 'строки %s' % rows, os.path.basename(s.png()))
    common('в1б-поле-10-строк', s, 1)
    s50 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'general', size='50x12') + ',wait:50'
    win('в1б-поле-10-строк-50x12', s50 + ',click:' + at(s50, 'Н...', size='50x12') + ',t:' + 'слово ' * 100 + ',wait:50',
        ['сло'], size='50x12')
    a = ch + ',click:' + inp
    win('в1б-авто-@', a + ',t:@ал,wait:50', ['Алиса'])
    win('в1б-авто-@-tab', a + ',t:@ал,tab,wait:50', ['@Алиса'], absent=['УЧАСТНИКИ'])
    win('в1б-авто-#', a + ',t:#фл,wait:50', ['флуд-2', 'флуд'])
    win('в1б-авто-#-enter', a + ',t:#фл,down,enter,wait:50', ['#флуд'])
    win('в1б-авто-:', a + ',t::fi,wait:50', ['fire'])
    win('в1б-авто-esc', a + ',t:@ал,esc,wait:50', [], absent=['УЧАСТНИКИ'])
    win('в1б-авто-отправка-@', a + ',t:@ал,tab,t:привет,enter,wait:100', ['Алиса привет'])
    mm = SRV + ',click:' + at(SRV, 'новости') + ',wait:100'
    win('в1б-новые-сообщения', mm, ['НОВОЕ'])
    pin = ch + ',click:' + at(ch, '-o', dx=1) + ',wait:100'
    win('в1б-закреплённые-подсказка', pin, ['Enter перейти'], absent=['закрыть поиск'])   # хвост подсказки может съесть показатель памяти
    w = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'general', size='200x30') + ',wait:100'
    wp = w + ',click:' + at(w, '-o', size='200x30', dx=1) + ',wait:100'
    s = win('в1б-закреплённые-переход-200x30', wp + ',down,enter,wait:100', ['всем привет'], size='200x30')
    # на 120x34 панель на весь центр: после Enter лента не видна (ошибка-кандидат: панель не закрывается)
    s = win('в1б-закреплённые-переход-120x34', pin + ',down,enter,wait:100', [])
    check('в1б-закреплённые-переход-120x34', 'после перехода видна лента (панель на весь центр закрыта)',
          s.has('Написать #general'), 'панель «Закреплённые» осталась на весь центр, лента скрыта')
    ib = ch + ',click:' + at(ch, '[_]', dx=1) + ',wait:100'
    win('в1б-входящие-подсказка', ib, ['Left/Right', 'Esc закрыть'], absent=['закрыть поиск'])
    win('в1б-входящие-упоминания', ib + ',right,wait:50', ['Упоминания'])
    win('в1б-входящие-enter', ib + ',enter,wait:100', [])
    pr = ch + ',click:' + at(ch, '<Алиса>', dx=1) + ',wait:50'
    win('в1б-профиль-строки', pr, ['В DISCORD С', 'НА СЕРВЕРЕ С', 'РОЛИ'])
    # исправления после прогона 08:40 и ревью 5
    s = win('в1в-авто-@-без-блок-и-заявок', a + ',t:@а,wait:50', ['Алиса'])
    d = s.dump()
    check('в1в-авто-@-без-блок-и-заявок', 'нет заблокированного и заявок (точно: Спамер, Незнакомец, Ваня)',
          not any(w in d for w in ('Спамер', 'Незнакомец', 'Ваня')))
    check('в1в-авто-@-без-запросов-и-спама', 'нет запросов на переписку и спама (Незнакомка, Бесплатный Нитро)',
          not any(w in d for w in ('Незнакомка', 'Бесплатный Нитро')))
    win('в1в-файл-щелчок-по-дате', ch + ',click:' + inp + ',t:черновик,ctrl+o,click:' + at(ch, 'ноября 2026') + ',wait:50',
        ['черновик'], absent=['Отправка файла'])
    ib2 = ib + ',enter,wait:100'
    win('в1в-входящие-переход-120x34', ib2, ['Написать'], absent=['Непрочитанное'])
    sr = ch + ',ctrl+f,t:привет,enter,wait:300,down,enter,wait:100'
    win('в1в-поиск-переход-120x34', sr, ['Написать #general'], absent=['результат'])
    snd = a + ',t:@ал,tab,t:проверка правки,enter,wait:100'
    s = win('в1в-правка-raw', snd + ',up,wait:50', ['проверка правки'])
    check('в1в-правка-raw', 'правка своего с упоминанием: в поле исходный текст (<@id> или @Алиса), без «[файл»',
          not s.has('[файл') and (s.has('<@') or s.has('@Алиса')), '', '')
    win('в1в-упоминание-только-целым-именем', a + ',t:@Алисаxyz тест,enter,wait:100', ['@Алисаxyz тест'])
    win('в1в-авто-everyone-первым', a + ',t:@e,wait:50', ['everyone'])
    m2 = ch + ',rclick:' + msg + ',wait:50'
    pinned = m2 + ',click:' + at(m2, 'Закрепить') + ',wait:100,rclick:' + msg + ',wait:50'
    win('в1в-закрепить-затем-открепить', pinned, ['Открепить'])
    win('в1в-up-после-перехода-к-старым', ch + ',wheelup:60:10,wheelup:60:10,wheelup:60:10,click:' + inp + ',up,wait:50', [])


def t_wave2():
    """Волна 2 (дизайнер ~09:30, MC_WAVE2): окна-формы — добавить/создать/вступить на сервер, меню сервера,
    пригласить, создать канал, меню канала, изменить канал, права, удалить, переставить, добавить в друзья, капча."""
    H = 'wait:50'
    add = 'click:' + at(H, '[ +  ]', tol=0) + ',wait:50'          # точно: иначе совпадает с «[    ]»
    win('в2-добавить-сервер', add, ['Добавить сервер', 'Отмена', 'Присоединиться', 'Создать свой'])
    win('в2-добавить-сервер-esc', add + ',esc,wait:50', ['Друзья'], absent=['Добавить сервер'])
    win('в2-добавить-сервер-x', add + ',click:' + at(add, 'x', last=False) + ',wait:50', [])
    cr = add + ',click:' + at(add, 'Создать свой') + ',wait:50'
    win('в2-создать-сервер', cr, ['Создайте свой сервер'])
    win('в2-создать-сервер-ввод', cr + ',t:Мой тест,tab,t:Z:/нет.png,wait:50', ['Мой тест'])
    win('в2-создать-сервер-enter', cr + ',t:Мой тест,enter,wait:200', ['сервер создан'])
    jn = add + ',click:' + at(add, 'Присоединиться') + ',wait:50'
    win('в2-присоединиться', jn, ['Присоединиться к серверу'])
    win('в2-приглашение-карточка', jn + ',t:discord.gg/abc,enter,wait:300', ['Вас пригласили', 'Котики 2', 'Вступить на сервер'])
    inv = jn + ',t:discord.gg/abc,enter,wait:300'
    j1 = inv + ',click:' + at(inv, 'Вступить на сервер') + ',wait:300'
    win('в2-приглашение-вступить-риск', j1, ['Понятно, вступить'])
    j2 = j1 + ',click:' + at(j1, 'Понятно, вступить') + ',wait:300'
    win('в2-приглашение-вступили', j2, ['Вы на этом сервере', 'Готово'])
    win('в2-приглашение-готово', j2 + ',click:' + at(j2, 'Готово') + ',wait:100', [], absent=['Вас пригласили'])
    jc = jn + ',t:captcha,enter,wait:300'
    jc1 = jc + ',click:' + at(jc, 'Вступить на сервер') + ',wait:300'
    jc2 = jc1 + ',click:' + at(jc1, 'Понятно, вступить') + ',wait:300'
    win('в2-приглашение-нужен-браузер', jc2, ['через браузер', 'Открыть в браузере'])
    win('в2-приглашение-enter-не-вступает', inv + ',enter,wait:300,enter,wait:300,enter,wait:300', [],
        absent=['Вы на этом сервере', 'вступили'])
    win('в2-приглашение-tab-пробел', inv + ',tab,wait:50,t: ,wait:300', ['Понятно, вступить'])
    win('в2-приглашение-enter-на-вступить', inv + ',tab,wait:50,enter,wait:300', ['щелчком'], absent=['Понятно, вступить'])
    win('в2-приглашение-ошибка-xyz', jn + ',t:xyz,enter,wait:300', ['недействительно'])
    win('в2-приглашение-ошибка-429', jn + ',t:429,enter,wait:300', ['слишком часто'])
    gm = SRV + ',click:' + at(SRV, 'Уютный уголок v') + ',wait:50'
    win('в2-меню-сервера', gm, ['Пригласить людей', 'Настройки сервера', 'Создать канал', 'Создать категорию',
                                 'Переставить каналы', 'Покинуть сервер'])
    win('в2-меню-сервера-esc', gm + ',esc,wait:50', [], absent=['Переставить каналы'])
    win('в2-настройки-сервера', gm + ',click:' + at(gm, 'Настройки сервера') + ',wait:50', [])
    win('в2-создать-канал-из-меню', gm + ',click:' + at(gm, 'Создать канал') + ',wait:50', ['Создать канал'])
    win('в2-создать-категорию', gm + ',click:' + at(gm, 'Создать категорию') + ',wait:50', ['категори'])
    win('в2-покинуть-сервер', gm + ',click:' + at(gm, 'Покинуть сервер') + ',wait:50', ['Покинуть'])
    rr = gm + ',click:' + at(gm, 'Переставить каналы') + ',wait:50'
    win('в2-переставить', rr, [])
    def order(s):
        names = ['general', 'музыка', 'новости', 'флуд-2', 'рисунки', 'флуд ', 'игры']
        pos = []
        for n in names:
            p = s.find(n, tol=0)
            pos.append(p[1] if p else -1)
        return pos
    base = order(shot.run(rr, 'at', '120x34'))
    # под general теперь ветка «обсуждение котов» (её не двигают): первый Down — на неё, второй — на #музыка
    for keys, nm in (('down,down,down,down,down,alt+up', 'alt+up'), ('down,down,alt+down', 'alt+down')):
        s = win('в2-переставить-' + nm, rr + ',' + keys + ',wait:50', [])
        o = order(s)
        check('в2-переставить-' + nm, 'порядок каналов изменился (%s -> %s)' % (base, o), o != base and -1 not in o)
        s2 = win('в2-переставить-' + nm + '-esc', rr + ',' + keys + ',wait:50,esc,wait:50', [])
        check('в2-переставить-' + nm + '-esc', 'после Esc (готово) новый порядок сохранён', order(s2) == o)
    win('в2-переставить-esc', rr + ',esc,wait:50', [])
    win('в2-пригласить-o+', SRV + ',click:' + at(SRV, 'o+') + ',wait:50', ['Пригласить друзей', 'Создать ссылку'])
    ip = SRV + ',click:' + at(SRV, 'o+') + ',wait:50'
    win('в2-пригласить-создать-ссылку', ip + ',click:' + at(ip, 'Создать ссылку') + ',wait:200', ['Копировать'])
    o2 = 'click:' + server_xy('Другой сервер') + ',wait:50'
    s = shot.run(o2, 'at', '120x34')
    p = s.find(' v')
    if p:
        g2 = o2 + ',click:%d:0,wait:50' % (p[0] - 3)
        win('в2-меню-другого-сервера', g2, ['Пригласить', 'Покинуть'], absent=['Создать канал', 'Переставить'])
    s = shot.run(SRV, 'at', '120x34')
    p = s.find('Текстовые каналы v')
    plus = '%d:%d' % (s.text()[p[1]].rindex('+'), p[1])
    cc = SRV + ',click:' + plus + ',wait:50'
    win('в2-создать-канал-в-категории', cc, ['Создать канал'])
    win('в2-создать-канал-тип-left-right', cc + ',right,wait:50', [])
    win('в2-создать-канал-ввод-enter', cc + ',tab,t:новый-канал,enter,wait:200', [])
    cm = SRV + ',rclick:' + at(SRV, 'рисунки') + ',wait:50'
    win('в2-меню-канала', cm, ['Изменить канал', 'Права канала', 'Пригласить', 'Копировать ID', 'Удалить канал'])
    ed = cm + ',click:' + at(cm, 'Изменить канал') + ',wait:50'
    win('в2-изменить-канал', ed, ['Сохранить'])
    win('в2-изменить-канал-tab-пробел', ed + ',tab,tab,tab,t: ,wait:50', [])
    win('в2-права-канала', cm + ',click:' + at(cm, 'Права канала') + ',wait:50', ['ДЛЯ КОГО'])
    pc = cm + ',click:' + at(cm, 'Права канала') + ',wait:50'
    win('в2-права-left-right', pc + ',right,down,right,wait:50', ['ДЛЯ КОГО'])
    win('в2-удалить-канал-подтверждение', cm + ',click:' + at(cm, 'Удалить канал') + ',wait:50', ['Удалить'])
    win('в2-копировать-id-канала', cm + ',click:' + at(cm, 'Копировать ID') + ',wait:50', [])
    # ревью 6: открытый канал — тема дозаполнена; смена только названия не стирает тему
    row = at(SRV, 'general')
    och = SRV + ',click:' + row + ',wait:100,rclick:' + row + ',wait:50'
    oed = och + ',click:' + at(och, 'Изменить канал') + ',wait:200'
    win('в2-изменить-канал-тема-дозаполнена', oed, ['Уютное общение'])
    sv = oed + ',end,t:X,enter,wait:300'
    win('в2-изменить-только-название', sv, ['generalX', 'канал изменён'])
    re2 = sv + ',rclick:' + row + ',wait:50'
    win('в2-изменить-только-название-тема-цела', re2 + ',click:' + at(re2, 'Изменить канал') + ',wait:200',
        ['generalX', 'Уютное общение'])
    vm = SRV + ',rclick:' + at(SRV, '<) Игры', dx=3) + ',wait:50'
    ve = vm + ',click:' + at(vm, 'Изменить канал') + ',wait:50'
    win('в2-изменить-голосовой', ve, ['БИТРЕЙТ', 'ЛИМИТ ПОЛЬЗОВАТЕЛЕЙ'])
    win('в2-меню-категории', SRV + ',rclick:' + at(SRV, 'Текстовые каналы v') + ',wait:50', [])
    af = 'click:' + at(H, 'Добавить в друзья') + ',wait:50'
    win('в2-добавить-в-друзья', af, ['Добавить в друзья'])
    win('в2-добавить-в-друзья-ввод', af + ',t:someone,enter,wait:200', [])
    cp = af + ',t:captcha,enter,wait:200'
    win('в2-капча', cp, ['человек'])
    win('в2-капча-esc', cp + ',esc,wait:200', ['не пройдена'], absent=['Проверка пройдена'])
    win('в2-капча-enter', cp + ',enter,wait:300', [])
    for sz in ('50x12', '80x24', '200x60'):
        win('в2-добавить-сервер-' + sz, 'click:3:%d,wait:50' % 0 + ',wait:10', [], size=sz)
    # исправления: @everyone, правка с упоминанием
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    inp = at(ch, 'Написать #general')
    win('в2-авто-@e', ch + ',click:' + inp + ',t:@e,wait:50', ['@everyone', '@here'])
    s = win('в2-правка-с-упоминанием', ch + ',click:' + inp + ',t:@ал,tab,t:тест правки,enter,wait:100,up,wait:50',
            ['@Алиса тест правки'], absent=['<@1001>'])


def t_txt():
    """Превью текстового вложения. В демо вложение — только у отправленного файла (Ctrl+O, demo_file); текст
    «[файл: x] url» в сообщении вложением НЕ становится (защита от подделки, 09.11). Содержимое — синтетика ui.c."""
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    inp = at(ch, 'Написать #general')

    def post(name, base=ch):
        path = os.path.join(shot.OUT, 'txtdemo', name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8') as f:
            f.write('строка 1\nстрока 2\n')
        return base + ',ctrl+o,t:' + path.replace(os.sep, '/') + ',enter,wait:500'
    t = post('zametka.txt')
    win('txt-превью', t, ['zametka.txt', 'строка текста номер 1', 'Развернуть', 'Скачать'])
    for ext in ('log', 'md', 'csv', 'json', 'c', 'py'):
        win('txt-превью-' + ext, post('file.' + ext), ['file.' + ext, 'Развернуть'])
    win('txt-не-текст-zip-без-превью', post('cat.zip'), ['cat.zip'], absent=['Развернуть'])
    win('txt-спойлер-не-грузится', post('SPOILER_secret.txt'), [], absent=['строка текста номер 1'])
    # подделка: текст «[файл: ...] url» — обычный текст, без рамки вложения и без «Скачать»
    fk = ch + ',click:' + inp + ',t:[файл: x.png] https://evil.example/a,enter,wait:400'
    win('txt-поддельное-вложение-текстом', fk, ['[файл: x.png] https://evil.example/a'], absent=['Скачать', 'Развернуть'])
    fk2 = ch + ',click:' + inp + ',t:[файл: x.txt] https://cdn.discordapp.com/attachments/1/2/x.txt,enter,wait:400'
    win('txt-поддельное-txt-текстом', fk2, ['[файл: x.txt]'], absent=['Скачать', 'Развернуть', 'строка текста номер 1'])
    e = t + ',click:' + at(t, 'Развернуть') + ',wait:300'
    win('txt-развернуть', e, ['строка текста номер 1'])
    win('txt-развернуть-колесо', e + ',wheeldown:' + at(e, 'строка текста номер 5') + ',wheeldown:' +
        at(e, 'строка текста номер 5') + ',wait:50', ['строка текста номер'])
    win('txt-скачать', t + ',click:' + at(t, 'Скачать') + ',wait:50', [])
    k = t + ',btab,wait:50'
    win('txt-клав-v', k + ',t:v,wait:100', ['строка текста номер'])
    win('txt-клав-v-]', k + ',t:v,wait:100,t:],t:],t:],wait:50', ['строка текста номер'])
    win('txt-клав-v-[-v', k + ',t:v,wait:100,t:],t:[,t:v,wait:50', ['Развернуть'])
    for sz in ('50x12', '80x24', '200x60'):
        s50 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'general', size=sz) + ',wait:100'
        win('txt-превью-' + sz, post('a.txt', s50), [], size=sz)


def t_call():
    """Исходящий звонок в ЛС (демо: петля, без сети). Живой звонок — только через координатора."""
    p = at('wait:50', 'Алиса', last=True)
    dm = 'click:' + p + ',wait:100'
    c = dm + ',click:' + at(dm, '(~)', dx=1) + ',wait:600'
    win('звонок-лс-кнопка', c, ['Голос подключён', 'Алиса', 'петля'])
    win('звонок-лс-F4', dm + ',f4,wait:600', ['Голос подключён'])
    win('звонок-лс-отбой-F7', c + ',f7,wait:200', ['(~)'], absent=['Голос подключён'])
    win('звонок-лс-отбой-кнопкой', c + ',click:' + at(c, ' \\_/ ', dx=2, last=True) + ',wait:200', [], absent=['Голос подключён'])
    win('звонок-лс-мик-звук', c + ',f5,f6,wait:50', ['(/)', '/x\\'])
    win('звонок-лс-смена-голоса', c + ',f11,wait:50', ['смена голоса'])
    win('звонок-лс-повторно', c + ',f7,wait:200,f4,wait:600', ['Голос подключён'])
    g = 'click:' + at('wait:50', 'Алиса, Боб') + ',wait:100'
    win('звонок-группа-F4', g + ',f4,wait:600', ['Голос подключён'])
    win('звонок-с-сервера-F4', SRV + ',click:' + at(SRV, 'general') + ',wait:100,f4,wait:200', ['из личной переписки'])
    for sz in ('50x12', '200x60'):
        s0 = shot.run('wait:50', 'at', sz)
        q = s0.find('Алиса')
        if q:
            win('звонок-лс-' + sz, 'click:%d:%d,wait:100,f4,wait:600' % q, [], size=sz)


def red_px(s):
    """Число красных пикселей (ошибки C_RED/C_DANGER) в окне снимка."""
    p = s.img
    return int(((p[:, :, 0] > 180) & (p[:, :, 1] < 100) & (p[:, :, 2] < 100)).sum())


def t_vstart():
    """Стартовый экран Astra DS — шаг VLESS (демо: --demo-start вопрос, --demo-start=err ошибка туннеля)."""
    Q, E = ['--demo-start'], ['--demo-start=err']
    s0 = win('vs-вопрос', 'wait:100', ['Astra DS', 'Подключение', 'Ссылка vless://', 'Больше не спрашивать при запуске',
                                      'Подключить', 'Без VLESS', 'Tab'], absent=['должна', 'не работает'], mouse=False, extra=Q)
    r0 = red_px(s0)
    con = at('wait:100', 'Подключить', extra=Q, dx=3)
    s1 = win('vs-подключить-пусто', 'wait:100,click:' + con + ',wait:300', ['vless://'], extra=Q)
    r1 = red_px(s1)
    check('vs-вопрос', 'красной ошибки поля нет до первой попытки (красных пикселей %d, после попытки %d)' % (r0, r1),
          r1 > r0 + 100 and r0 < 300, 'до попытки красного почти столько же, сколько после' if r1 <= r0 + 100 else '')
    fld = at('wait:100', 'vless://...', extra=Q, dx=2)
    win('vs-подключить-неверная', 'wait:100,click:' + fld + ',t:http://x,click:' + con + ',wait:300', ['vless://'], extra=Q)
    win('vs-ввод-звёздочки', 'wait:100,click:' + fld + ',t:vless://секрет@host:443,wait:50', ['vless://'],
        absent=['секрет'], extra=Q)
    win('vs-подключить-верная', 'wait:100,click:' + fld + ',t:vless://u@h:443?security=reality,click:' + con + ',wait:600',
        [], extra=Q)
    cb = at('wait:100', '[ ] Больше', extra=Q, dx=1)
    win('vs-галочка-щелчок', 'wait:100,click:' + cb + ',wait:50', ['[x] Больше не спрашивать'], extra=Q)
    win('vs-галочка-щелчок-дважды', 'wait:100,click:' + cb + ',click:' + cb + ',wait:50', ['[ ] Больше не спрашивать'], extra=Q)
    # Tab по полям: поле -> галочка -> Подключить -> Без VLESS; пробел на галочке ставит её
    tabs = []
    for n in range(1, 5):
        st = shot.run('wait:100' + ',tab' * n + ',wait:50', 'vs-tab-%d' % n, '120x34', extra=Q)
        tabs.append(st.img.tobytes())
    check('vs-tab', 'каждый Tab меняет фокус (4 разных кадра)', len(set(tabs)) == 4)
    win('vs-tab-пробел-галочка', 'wait:100,tab,t: ,wait:50', ['[x] Больше'], mouse=False, extra=Q)
    nv = at('wait:100', 'Без VLESS )', extra=Q, dx=2)
    win('vs-без-vless', 'wait:100,click:' + nv + ',wait:300', [], absent=['Подключение'], extra=Q)
    win('vs-без-vless-с-галочкой', 'wait:100,click:' + cb + ',click:' + nv + ',wait:300', [], absent=['Подключение'], extra=Q)
    # ошибка туннеля
    win('vs-ошибка', 'wait:100', ['VLESS не работает', 'Повторить', 'Выход', 'Другая ссылка', 'Подключить', 'Без VLESS'],
        mouse=False, extra=E)
    rp = at('wait:100', 'Повторить', extra=E)
    win('vs-ошибка-повторить', 'wait:100,click:' + rp + ',wait:600', [], extra=E)
    win('vs-ошибка-enter-повторить', 'wait:100,enter,wait:600', [], mouse=False, extra=E)
    win('vs-ошибка-выход', 'wait:100,click:' + at('wait:100', 'Выход', extra=E, dx=1) + ',wait:200', [], extra=E)
    of = at('wait:100', 'vless://...', extra=E, dx=2)
    oc = at('wait:100', '( Подключить )', extra=E, dx=3)
    win('vs-ошибка-другая-ссылка', 'wait:100,click:' + of + ',t:vless://u@h2:443,click:' + oc + ',wait:600', [], extra=E)
    win('vs-ошибка-другая-пусто', 'wait:100,click:' + oc + ',wait:300', [], extra=E)
    win('vs-ошибка-без-vless', 'wait:100,click:' + at('wait:100', 'Без VLESS', extra=E, dx=2) + ',wait:300', [],
        absent=['VLESS не работает'], extra=E)
    # пункт «Спрашивать про VLESS при запуске» (Настройки > Конфиденциальность); в демо хранилище не трогается (ui.c O.demo)
    g = 'click:' + at('wait:50', '(*)', dx=1) + ',wait:50'
    pc = g + ',click:' + at(g, 'Конфиденциальность') + ',wait:50'
    win('vs-настройки-пункт', pc, ['ПРИ ЗАПУСКЕ', '[x] Спрашивать про VLESS при запуске'])
    sa = at(pc, 'Спрашивать про VLESS', dx=2)
    win('vs-настройки-снять', pc + ',click:' + sa + ',wait:50', ['[ ] Спрашивать про VLESS при запуске'])
    win('vs-настройки-вернуть', pc + ',click:' + sa + ',click:' + sa + ',wait:50', ['[x] Спрашивать про VLESS при запуске'])
    # «Больше не спрашивать» + «Без VLESS» на стартовой карточке -> в настройках вопрос выключен (та же настройка)
    qg = 'wait:100,click:' + cb + ',click:' + nv + ',wait:300'
    g2 = qg + ',click:' + at(qg, '(*)', dx=1, extra=Q) + ',wait:50'
    p2 = g2 + ',click:' + at(g2, 'Конфиденциальность', extra=Q) + ',wait:50'
    win('vs-галочка-видна-в-настройках', p2, ['[ ] Спрашивать про VLESS при запуске'], extra=Q)
    # --vless (вернуть вопрос) со --shot не сочетается — отказ с подсказкой, без окна
    r = subprocess.run([shot.EXE, '--demo', '--vless', '--shot=' + os.path.join(shot.OUT, 'vs-flag.bmp')], capture_output=True, timeout=30)
    check('vs-флаг-vless', '--vless со --shot отклонён (код 2, подсказка)', r.returncode == 2 and '--vless' in r.stdout.decode('utf-8', 'replace'))
    h = subprocess.run([shot.EXE, '--help'], capture_output=True, timeout=30).stdout.decode('utf-8', 'replace')
    check('vs-флаг-vless', '--help описывает --vless', '--vless ' in h)
    # на любом допустимом размере (от 50x12) видны поле ссылки и кнопки — иначе выбрать нечего
    for sz in ('50x12', '50x16', '80x24', '200x60'):
        s = win('vs-вопрос-' + sz, 'wait:100', ['vless://...', 'Подключить', 'Без VLESS'], size=sz, mouse=False, extra=Q)
        check('vs-вопрос-' + sz, 'красной ошибки нет до попытки', red_px(s) < 300, str(red_px(s)))
        win('vs-ошибка-' + sz, 'wait:100', ['Повторить', 'Выход', 'Подключить', 'Без VLESS'], size=sz, mouse=False, extra=E)
    x = at('wait:100', 'Выход', extra=E, dx=1)
    win('vs-ошибка-выход-завершает', 'wait:100,click:' + x + ',wait:200,click:' + at('wait:100', 'Без VLESS', extra=E, dx=2) +
        ',wait:300', ['VLESS не работает'], absent=['Друзья'], mouse=False, extra=E)   # после выхода щелчки не доходят


def fg_bright(s, text):
    """Средняя яркость «чернил» надписи (для проверки «тусклый»): сумма RGB цвета символов клеток надписи."""
    p = s.find(text, tol=0)
    if not p:
        return -1
    v = []
    for i in range(len(text)):
        fg, bg, m = s.cell(p[0] + i, p[1])
        if fg is not None:
            v.append(((fg >> 16) & 255) + ((fg >> 8) & 255) + (fg & 255))
    return sum(v) / len(v) if v else -1


def t_packA():
    """Пакет А (дизайнер ~14:10): полная дата у времени, Переслать, Просмотр реакций, «Не отправлено» (!fail),
    превью ссылок, заглушить сервер/канал/беседу и уровень уведомлений, подсказка и страницы поиска, нет прав писать."""
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:100'
    inp = at(ch, 'Написать #general')
    tm = at(ch, '<Боб> 08.11 16:05', dx=13)
    win('пА-время-наведение', ch + ',hover:' + tm + ',wait:100', ['воскресенье, 8 ноября 2026 г. в 16:05'])
    win('пА-время-щелчок', ch + ',click:' + tm + ',wait:50', ['воскресенье, 8 ноября 2026 г. в 16:05'])
    rm = at(ch, 'держи ссылку', dx=3)
    m = ch + ',rclick:' + rm + ',wait:50'
    win('пА-меню-сообщения', m, ['Переслать', 'Просмотр реакций', 'Закрепить сообщение', 'Копировать ID сообщения'])
    mn = ch + ',rclick:' + at(ch, 'привет! в #новости', dx=20) + ',wait:50'
    win('пА-меню-без-реакций', mn, ['Переслать'], absent=['Просмотр реакций'])
    f = m + ',click:' + at(m, 'Переслать') + ',wait:50'
    win('пА-переслать-окно', f, ['Переслать в', 'Алиса, Боб', 'новости', 'Enter переслать'], absent=['Enter открыть'])
    # «видно только вам» (flags 64): демо — #музыка, MemeBot
    mu = SRV + ',click:' + at(SRV, 'музыка') + ',wait:150'
    win('пА-видно-только-вам', mu, ['(i) Видно только вам - Скрыть'])
    win('пА-видно-только-вам-скрыть', mu + ',click:' + at(mu, 'Скрыть', last=True, dx=2) + ',wait:100', [],
        absent=['Видно только вам'])
    # «!slow» — ожидание отправки 3 с: текст серым, потом обычным
    sl = ch + ',click:' + inp + ',t:!slow проверка,enter,wait:300,snap,wait:3500'
    s7 = shot.run(sl, 'пА-ожидание-отправки', '120x34')
    common('пА-ожидание-отправки', s7, 1)
    if s7.snaps:
        bw, bd = fg_bright(s7.snaps[0], '!slow проверка'), fg_bright(s7, '!slow проверка')
        check('пА-ожидание-отправки', 'во время ожидания текст серее (%d), после отправки ярче (%d)' % (bw, bd),
              0 < bw < bd - 30)
    else:
        check('пА-ожидание-отправки', 'промежуточный снимок есть', False)
    win('пА-переслать-в-канал', f + ',t:новости,enter,wait:200', ['переслано'], absent=['Переслать в'])
    win('пА-переслать-на-сервер-отказ', f + ',t:Другой сервер,wait:50,enter,wait:200', [])
    win('пА-переслать-esc', f + ',esc,wait:50', [], absent=['Переслать в'])
    rv = m + ',click:' + at(m, 'Просмотр реакций') + ',wait:50'
    win('пА-реакции-окно', rv, ['Алиса', 'Боб'])
    win('пА-реакции-down', rv + ',down,wait:50', [])
    win('пА-реакции-esc', rv + ',esc,wait:50', [], absent=['Просмотр реакций'])
    win('пА-реакции-щелчок-мимо', rv + ',click:100:30,wait:50', [], absent=['Реакции  '])
    # store-версия (дизайнер ~14:50): Down/щелчок по реакции меняют список справа; x закрывает; смена канала закрывает
    a0 = shot.run(rv, 'пА-реакции-a', '120x34').text()
    a1 = shot.run(rv + ',down,wait:50', 'пА-реакции-b', '120x34').text()
    check('пА-реакции-down-меняет-список', 'Down: справа другой список (кто поставил :pepe:)', a0 != a1)
    pe = at(rv, ':pepe: 1', dx=1)        # первое сверху — список в окне (ниже — плашка в ленте)
    a2 = shot.run(rv + ',click:' + pe + ',wait:50', 'пА-реакции-c', '120x34').text()
    check('пА-реакции-щелчок-по-реакции', 'щелчок по :pepe: в списке = как Down', a2 == a1)
    win('пА-реакции-x', rv + ',click:' + at(rv, 'Реакции', dx=46) + ',wait:50', [], absent=['Реакции  '])
    # смена канала при открытом окне: Alt+Down окно перехватывает (по замыслу); закрытие при смене — в selftest
    win('пА-переслать-в-голосовой-отказ', f + ',t:Отдых,wait:50,enter,wait:200', ['переслать можно в текстовый канал или беседу'])
    fl = ch + ',click:' + inp + ',t:!fail тест,enter,wait:300'
    win('пА-не-отправлено', fl, ['!fail тест', 'Не отправлено. Повторить - Удалить'])
    win('пА-не-отправлено-удалить', fl + ',click:' + at(fl, 'Удалить', last=True, dx=2) + ',wait:100', [],
        absent=['Не отправлено'])
    win('пА-не-отправлено-повторить', fl + ',click:' + at(fl, 'Повторить', last=True, dx=2) + ',wait:300', ['!fail тест'])
    lk = ch + ',click:' + inp + ',t:смотри https://example.org,wait:50'
    win('пА-превью-ссылок', lk, ['[x] превью ссылок'])
    win('пА-превью-ссылок-снять', lk + ',click:' + at(lk, '[x] превью', dx=1) + ',wait:50', ['[ ] превью ссылок'])
    gm = SRV + ',click:' + at(SRV, 'Уютный уголок v') + ',wait:50'
    win('пА-меню-сервера', gm, ['Заглушить сервер', 'Уведомления: по умолчанию'])
    win('пА-уведомления-по-кругу', gm + ',click:' + at(gm, 'Уведомления:') + ',wait:50,click:' + at(SRV, 'Уютный уголок v') +
        ',wait:50', ['Уведомления:'], absent=['Уведомления: по умолчанию'])
    win('пА-заглушить-сервер', gm + ',click:' + at(gm, 'Заглушить сервер') + ',wait:50,click:' + at(SRV, 'Уютный уголок v') +
        ',wait:50', ['Включить уведомления сервера'])
    cm = SRV + ',rclick:' + at(SRV, 'рисунки') + ',wait:50'
    win('пА-меню-канала-заглушить', cm, ['Заглушить канал', 'Уведомления:'])
    s0 = shot.run(SRV, 'пА-канал-до-заглушения', '120x34')
    s1 = win('пА-канал-заглушён-тусклый', cm + ',click:' + at(cm, 'Заглушить канал') + ',wait:50', ['рисунки'])
    b0, b1 = fg_bright(s0, 'рисунки'), fg_bright(s1, 'рисунки')
    check('пА-канал-заглушён-тусклый', 'имя заглушённого канала тусклее (%d -> %d)' % (b0, b1), 0 < b1 < b0 - 30)
    dm = 'rclick:' + at('wait:50', 'Алиса, Боб') + ',wait:50'
    win('пА-меню-беседы-заглушить', dm, ['Заглушить беседу'])          # у беседы уровня уведомлений нет (как в Discord)
    # подсказка операторов — на широкой плашке (от 160 колонок; на 120 плашка узкая — по замыслу)
    c16 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'general', size='160x40') + ',wait:100'
    win('пА-поиск-подсказка-160x40', c16 + ',ctrl+f,wait:50', ['from: in: has:'], absent=['befor '], size='160x40', mouse=True)
    c20 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'general', size='200x60') + ',wait:100'
    win('пА-поиск-подсказка-200x60', c20 + ',ctrl+f,wait:50', ['from: in: has:'], absent=['befor '], size='200x60', mouse=True)
    # замок голосового: в демо — «Права канала» (@everyone: запретить «Подключаться») -> «<!»
    vm = SRV + ',rclick:' + at(SRV, '<) Игры', dx=3) + ',wait:50'
    vp = vm + ',click:' + at(vm, 'Права канала') + ',wait:50'
    s6 = shot.run(vp, 'пА-права-голосового', '120x34')
    q = s6.find('Подключаться')
    if q:
        xl = s6.text()[q[1]].find('x', q[0] + 20)
        win('пА-замок-голосового', vp + ',click:%d:%d,wait:50,click:' % (xl, q[1]) + at(vp, 'Сохранить', dx=2) + ',wait:100',
            ['<! Игры'])
    else:
        check('пА-замок-голосового', 'в «Правах» голосового есть «Подключаться»', False)
    # страницы поиска: 150 сообщений «флуд номер N» в #флуд
    for sz in ('160x40', '120x34'):
        s5 = shot.run(SRV, 'пА-поиск-каналы-' + sz, sz)
        y = [i for i, l in enumerate(s5.text()) if '#  флуд ' in l][0]
        sp = SRV + ',click:20:%d,wait:150,ctrl+f,t:флуд,enter,wait:400' % y
        s2 = win('пА-поиск-страницы-' + sz, sp, ['стр. 1 из'], size=sz)
        p = s2.find('стр. 1 из')
        if p:
            gt = s2.text()[p[1]].find('>', p[0])
            win('пА-поиск-страница-вперёд-' + sz, sp + ',click:%d:%d,wait:300' % (gt, p[1]), ['стр. 2 из'], size=sz)
    # нет права писать: в демо — через «Права канала» (@everyone: запретить «Отправлять сообщения»)
    pc = SRV + ',rclick:' + at(SRV, 'рисунки') + ',wait:50'
    pp = pc + ',click:' + at(pc, 'Права канала') + ',wait:50'
    s3 = shot.run(pp, 'at', '120x34')
    q = s3.find('Отправлять сообщения')
    if q:
        xl = s3.text()[q[1]].find('x', q[0] + 20)
        sv = pp + ',click:%d:%d,wait:50,click:' % (xl, q[1]) + at(pp, 'Сохранить', dx=2) + ',wait:100,click:' + \
            at(SRV, 'рисунки') + ',wait:150'
        win('пА-нет-прав-писать', sv, ['нет прав на отправку'])


def t_look_voice_slow():
    """Настройки (порядок как в Discord): Внешний вид (масштаб), Голос и видео (устройства, тест микрофона);
    медленный режим (#медленный, 15 с): Enter не отправляет, текст остаётся, отсчёт в рамке."""
    g = 'click:' + at('wait:50', '(*)', dx=1) + ',wait:50'
    s = shot.run(g, 'нс-порядок', '120x34')
    order = [s.find(n) for n in ('Моя учётная запись', 'Конфиденциальность', 'Внешний вид', 'Голос и видео', 'Горячие клавиши')]
    check('нс-порядок-разделов', 'разделы как в Discord: учётная запись, конфиденциальность, вид, голос, клавиши',
          all(order) and [p[1] for p in order] == sorted(p[1] for p in order))
    lk = g + ',click:' + at(g, 'Внешний вид') + ',wait:100'
    win('нс-внешний-вид', lk, ['ТЕМА', 'МАСШТАБ: 100 %', '(Ctrl+-)', '(Ctrl+0)', '(Ctrl+=)'])
    s1 = win('нс-масштаб-ctrl+=', lk + ',ctrl+=,wait:200', ['МАСШТАБ: 110 %'])
    win('нс-масштаб-ctrl+=-ctrl+0', lk + ',ctrl+=,ctrl+=,wait:100,ctrl+0,wait:200', ['МАСШТАБ: 100 %'])
    win('нс-масштаб-ctrl+-', lk + ',ctrl+-,wait:200', ['МАСШТАБ: 90 %'])
    win('нс-масштаб-кнопка-+', lk + ',click:' + at(lk, '( + (Ctrl+=)', dx=2, tol=0) + ',wait:200', ['МАСШТАБ: 110 %'])
    s0 = shot.run(lk, 'нс-масштаб-база', '120x34')
    check('нс-масштаб-ctrl+=', 'клетка окна крупнее (%dx%d -> %dx%d px)' % (s0.cw, s0.ch, s1.cw, s1.ch),
          s1.cw * s1.ch > s0.cw * s0.ch)
    vc = g + ',click:' + at(g, 'Голос и видео') + ',wait:100'
    win('нс-голос', vc, ['УСТРОЙСТВО ВВОДА', 'УСТРОЙСТВО ВЫВОДА', 'По умолчанию', 'Проверить микрофон', 'ПОРОГ АКТИВАЦИИ'])
    nx = at(vc, 'По умолчанию', dx=28)
    win('нс-голос-устройство-следующее', vc + ',click:' + nx + ',wait:100', ['УСТРОЙСТВО ВВОДА'])
    win('нс-голос-тест', vc + ',click:' + at(vc, 'Проверить микрофон', dx=2) + ',wait:600', ['Остановить'])
    win('нс-голос-тест-остановить', vc + ',click:' + at(vc, 'Проверить микрофон', dx=2) + ',wait:600,click:' +
        at(vc, 'Проверить микрофон', dx=2) + ',wait:200', ['Проверить микрофон'])
    win('нс-голос-тест-уход-из-раздела', vc + ',click:' + at(vc, 'Проверить микрофон', dx=2) + ',wait:600,click:' +
        at(vc, 'Внешний вид') + ',wait:200,click:' + at(g, 'Голос и видео') + ',wait:100', ['Проверить микрофон'],
        absent=['Остановить'])
    win('нс-голос-50x12', 'wait:50', [], size='50x12', mouse=False)
    # медленный режим
    md = SRV + ',click:' + at(SRV, 'медленный') + ',wait:150'
    inp = at(md, 'Написать #медленный')
    sm = md + ',click:' + inp + ',t:раз,enter,wait:200,t:два,enter,wait:300'
    win('мр-второе-не-отправлено', sm, ['раз', 'Медленный режим', 'текст оста', '+ два'])   # хвост режет показатель памяти
    s2 = shot.run(sm + ',snap,wait:2200', 'мр-отсчёт', '120x34')
    import re as _re
    a = _re.search(r'Медленный режим: (\d+) с', s2.snaps[0].dump()) if s2.snaps else None
    b = _re.search(r'Медленный режим: (\d+) с', s2.dump())
    check('мр-отсчёт', 'отсчёт идёт (%s -> %s с)' % (a and a.group(1), b and b.group(1)),
          bool(a and b) and int(b.group(1)) < int(a.group(1)))
    common('мр-отсчёт', s2, 1)


def t_viewer():
    """№61 просмотр картинки на весь экран (щелчок по превью; Left/Right; Esc / x / щелчок по фону; спойлеры
    пропускаются; память до/после) и №46 «[копировать]» у блока кода."""
    import re as _re

    def preview_xy(keys, size='120x34'):
        t = shot.run(keys, 'пк-поиск-превью', size).text()
        for y, l in enumerate(t):
            for pat in ('======', '++++++', '******', ';;;;;;', '%%%%%%', '######', '@@@@@@'):
                i = l.find(pat, 40)
                if i >= 0:
                    return '%d:%d' % (i + 3, y)
        return None
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:150'
    p = preview_xy(ch)
    v = ch + ',click:' + p + ',wait:300'
    win('пк-просмотр', v, ['кот.png', '1 из 1', 'Esc - закрыть'])
    win('пк-просмотр-esc', v + ',esc,wait:100', ['Написать #general'], absent=['Esc - закрыть'])
    win('пк-просмотр-x', v + ',click:' + at(v, 'x', last=False, dx=0, tol=0) + ',wait:100', [], absent=['Esc - закрыть'])
    sv = shot.run(v, 'пк-просмотр-x-поиск', '120x34').text()
    xr = len(sv[0].rstrip()) - 1
    win('пк-просмотр-x-угол', v + ',click:%d:0,wait:100' % xr, ['Написать #general'], absent=['Esc - закрыть'])
    win('пк-просмотр-щелчок-по-фону', v + ',click:3:15,wait:100', ['Написать #general'], absent=['Esc - закрыть'])
    # память: открыть/закрыть в одном процессе 5 раз — буфер освобождается, роста нет
    r = shot.run(ch + ',snap' + (',click:' + p + ',wait:300,esc,wait:100,snap') * 5, 'пк-память', '120x34')
    sk = r.info['snap_kb']
    check('пк-память', 'после 5 открытий/закрытий: %s КБ; рост от первого закрытия до последнего <= 64 КБ' % sk,
          len(sk) == 6 and sk[5] - sk[1] <= 64)
    # несколько картинок: чат голосового «Отдых» (3018: meme/face/icon), 200x60; Right/Left, без зацикливания
    o = SRV + ',click:' + at(SRV, '<) Отдых', dx=3, size='200x60') + ',wait:200'
    t = shot.run(o, 'пк-отдых', '200x60').text()
    bar = t[5].rfind('|')
    ys = [y for y, l in enumerate(t) if any(q in l[bar + 1:] for q in ('++++', '****', '====')) and y > 20]
    v2 = o + ',click:%d:%d,wait:300' % (bar + 12, ys[0]) if ys else None
    if v2:
        names = []
        for n, k in enumerate(('', ',right,wait:300', ',right,wait:200,right,wait:300', ',right,wait:200,right,wait:200,right,wait:300',
                               ',left,wait:300')):
            s = win('пк-отдых-%d' % n, v2 + k, [' из 3'], size='200x60')
            m = _re.search(r'(\S+)\s+(\d) из 3', s.text()[0])
            names.append(m.group(2) if m else '?')
        check('пк-листание', 'Right/Right/Right/Left: %s (ожидается 1 2 3 3 1)' % ' '.join(names), names == ['1', '2', '3', '3', '1'])
    else:
        check('пк-отдых', 'в чате «Отдых» найдено превью', False)
    # спойлер: закрытый SPOILER_ в листание не входит — в демо одной ленты с картинкой и спойлером нет (selftest)
    # №46 блок кода: «[копировать]» -> уведомление
    inp = at(ch, 'Написать #general')
    # Многострочный блок набрать нельзя: --shot-keys «shift+enter» шлёт K_ENTER+shift мимо gui_win (там Shift+Enter ->
    # '\n'), ui.c отправляет. Ждём от разработчика шаг перевода строки в --shot-keys (просьба 09.11), до тех пор — ⏳.
    if os.environ.get('UITEST_SHIFT_ENTER_OK'):
        cb = ch + ',click:' + inp + ',t:```,shift+enter,t:print(1),shift+enter,t:```,enter,wait:300'
        win('пк-блок-кода', cb, ['print(1)', '[копировать]'])
        win('пк-блок-кода-копировать', cb + ',click:' + at(cb, '[копировать]', dx=2, last=True) + ',wait:100', ['скопирован'])
    win('пк-строка-без-кода-без-кнопки', ch + ',click:' + inp + ',t:просто `код` в строке,enter,wait:300', ['код'],
        absent=['[копировать]'])


def sel_px(s):
    """Пиксели выделения текста (синяя плашка, как на снимках №38): r<90, g<110, b>120."""
    p = s.img
    return int(((p[:, :, 0] < 90) & (p[:, :, 1] < 110) & (p[:, :, 2] > 120)).sum())


def t_select_threads():
    """№38 выделение текста (drag, shift+click, Ctrl+C), пропуск спойлера в просмотре (#рисунки 3006: 3 картинки,
    средняя SPOILER_), ветки (3101 «обсуждение котов», запись) и форум #вопросы (3020)."""
    import re as _re
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:150'
    a, b = at(ch, 'Очень длинное'), at(ch, 'fox jumps over', dx=9)
    s0 = shot.run(ch, 'выд-нет', '120x34')
    s1 = win('выд-drag', ch + ',drag:' + a + ':' + b + ',wait:100', [])
    check('выд-drag', 'перетаскивание выделяет (синих пикселей %d -> %d)' % (sel_px(s0), sel_px(s1)), sel_px(s1) > sel_px(s0) + 3000)
    s2 = win('выд-shift-click', ch + ',click:' + a + ',shift+click:' + b + ',wait:100', [])
    check('выд-shift-click', 'Shift+щелчок выделяет то же, что перетаскивание', abs(sel_px(s2) - sel_px(s1)) < 500)
    win('выд-ctrl+c', ch + ',drag:' + a + ':' + b + ',ctrl+c,wait:50', ['выделенный текст скопирован'])
    s3 = win('выд-щелчок-снимает', ch + ',drag:' + a + ':' + b + ',wait:50,click:' + at(ch, 'всем привет :wave:') + ',wait:50', [])
    check('выд-щелчок-снимает', 'обычный щелчок снимает выделение', sel_px(s3) < sel_px(s0) + 500)
    win('выд-ctrl+click-меню-нет', ch + ',ctrl+click:' + a + ',wait:50', [])
    # просмотр: закрытый спойлер пропускается, открытый — входит в листание
    ri = SRV + ',click:' + at(SRV, 'рисунки') + ',wait:200'
    t = shot.run(ri, 'выд-рисунки', '120x34').text()
    ys = [y for y, l in enumerate(t) if any(q in l[40:96] for q in ('++++', '****', '====')) and 'СПОЙЛЕР' not in l]
    if ys:
        x = max(t[ys[0]].find(q, 40) for q in ('++++', '****', '====')) + 2
        v = ri + ',click:%d:%d,wait:300' % (x, ys[0])
        s = win('пк-рисунки-просмотр', v, [' из 2'], absent=['SPOILER_'])
        s4 = win('пк-рисунки-right', v + ',right,wait:300,right,wait:300', [' из 2'], absent=['SPOILER_'])
        sp = at(ri, 'СПОЙЛЕР', dx=2)
        v3 = ri + ',click:' + sp + ',wait:200,click:%d:%d,wait:300' % (x, ys[0])
        win('пк-рисунки-спойлер-открыт', v3, [' из 3'])
    else:
        check('пк-рисунки-просмотр', 'в #рисунки найдено превью', False)
    # ветки
    kt = SRV + ',click:' + at(SRV, 'обсуждение котов') + ',wait:300'
    win('ветка-лента', kt, ['обсуждение котов', 'Это начало ветки в #general', 'а мой кот спит на клавиатуре',
                            'Написать #обсуждение'])
    inp = at(kt, 'Написать #обсуждение')
    win('ветка-запись', kt + ',click:' + inp + ',t:мой тоже,enter,wait:200', ['мой тоже'])
    # форум
    fo = SRV + ',click:' + at(SRV, 'вопросы') + ',wait:200'
    s5 = win('форум-список', fo, ['Сортировка', 'Как настроить звук?', 'Покажите свои рабочие места', '[закреплено] Правила раздела',
                                  'сообщ.'])
    check('форум-дата-последнего', 'дата последнего сообщения не «01.01.2015» (начало эпохи Discord = нет данных)',
          '01.01.2015' not in s5.dump())
    win('форум-сортировка', fo + ',click:' + at(fo, 'Сортировка', dx=2) + ',wait:100', ['Сортировка'], absent=['по активности'])
    ft = fo + ',click:' + at(fo, 'Как настроить звук?') + ',wait:600'
    win('форум-ветка', ft, ['Как настроить звук?', 'Написать #Как'])
    win('форум-ветка-есть-сообщения', ft, ['Микрофон тихий'])
    for sz in ('80x24', '200x60'):              # на 50x12 #вопросы ниже видимой части списка
        f2 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'вопросы', size=sz) + ',wait:200'
        win('форум-' + sz, f2, ['вопросы'], size=sz)


def gold_px(s, x0=40, x1=96):
    """Золотые пиксели (полоска упоминания C_GOLD / предупреждения) в столбцах ленты."""
    p = s.img[:, x0 * s.cw:x1 * s.cw]
    return int(((p[:, :, 0] > 180) & (p[:, :, 1] > 120) & (p[:, :, 1] < 190) & (p[:, :, 2] < 100)).sum())


def t_alert_links():
    """№119 слова-тревоги (Настройки > Уведомления; подсветка как упоминание) и Б-1 безопасность ссылок
    (замаскированная — виден настоящий адрес; нелатинские буквы в домене — «(!)» и предупреждение)."""
    g = 'click:' + at('wait:50', '(*)', dx=1) + ',wait:50'
    n = g + ',click:' + at(g, 'Уведомления') + ',wait:100'
    win('ст-раздел', n, ['СЛОВА-ТРЕВОГИ', 'не заданы', 'Изменить слова'])
    e = n + ',click:' + at(n, 'Изменить слова', dx=2) + ',wait:100'
    win('ст-форма', e, ['Слова-тревоги', 'Через запятую', 'Сохранить'])
    sv = e + ',t:булок,click:' + at(e, 'Сохранить', dx=2, last=True) + ',wait:100'
    win('ст-сохранено', sv + ',click:' + at('wait:50', '(*)', dx=1) + ',wait:50,click:' + at(g, 'Уведомления') + ',wait:100',
        ['булок'], absent=['не заданы'])
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:150'
    s0 = shot.run(ch, 'ст-лента-до', '120x34')
    s1 = win('ст-подсветка', sv + ',' + ch, ['булок'])
    check('ст-подсветка', 'сообщение со словом подсвечено как упоминание (золото %d -> %d)' % (gold_px(s0), gold_px(s1)),
          gold_px(s1) > gold_px(s0) + 100)
    s2 = win('ст-целое-слово', e + ',t:була,click:' + at(e, 'Сохранить', dx=2, last=True) + ',wait:100,' + ch, [])
    check('ст-целое-слово', 'часть слова («була») не подсвечивает', gold_px(s2) <= gold_px(s0) + 100)
    s3 = win('ст-регистр', e + ',t:БУЛОК,click:' + at(e, 'Сохранить', dx=2, last=True) + ',wait:100,' + ch, [])
    check('ст-регистр', 'регистр не важен («БУЛОК»)', gold_px(s3) > gold_px(s0) + 100)
    # Б-1 ссылки
    inp = at(ch, 'Написать #general')
    lk = ch + ',click:' + inp + ',t:[discord.com](https://evil.com) и https://disсord.com/x,enter,wait:300'
    win('б1-маскированная', lk, ['discord.com -> evil.com'])
    win('б1-нелатинские', lk, ['https://disсord.com/x (!)'])
    hv = lk + ',click:' + at(lk, 'disсord.com/x', dx=3, last=True) + ',wait:100'      # предупреждение — по щелчку
    win('б1-предупреждение', hv, ['ОСТОРОЖНО'])
    win('б1-щелчок-маскированной', lk + ',click:' + at(lk, '-> evil.com', dx=5, last=True) + ',wait:100', ['evil.com'])
    win('б1-обычная-без-метки', ch + ',click:' + inp + ',t:https://discord.com/channels,enter,wait:300',
        ['https://discord.com/channels'], absent=['channels (!)', 'ОСТОРОЖНО'])


def t_bots_misc():
    """№55 кнопки бота (#игры, сообщение 9907), голосовое сообщение, эмбед и опросы; М5 удалённая ветка
    («!delthread» — через 3 с); слова-тревоги списком («\\,» в --shot-keys); посты форума с сообщениями."""
    import re as _re
    ig = SRV + ',click:' + at(SRV, 'игры') + ',wait:200'
    for lab in ('Играть', 'Пауза', 'В избранное'):
        win('бот-кнопка-' + lab, ig + ',click:' + at(ig, lab, dx=1) + ',wait:200', ['бот получил «' + lab + '»'])
    win('бот-ссылка-копирует', ig + ',click:' + at(ig, 'Текст песни', dx=1) + ',wait:200', ['ссылка скопирована (example.com)'],
        absent=['бот получил'])
    gv = ig + ',click:' + at(ig, 'Громкость', dx=2) + ',wait:100'
    win('бот-список-1', gv, ['Тихо', 'Средне', 'Громко', 'Enter выбрать'])
    win('бот-список-1-enter', gv + ',down,enter,wait:200', ['бот получил'], absent=['Enter выбрать'])
    win('бот-список-1-esc', gv + ',esc,wait:100', [], absent=['Enter выбрать', 'бот получил'])
    gj = ig + ',click:' + at(ig, 'Жанры', dx=2) + ',wait:100'
    win('бот-список-3', gj, ['Рок', 'Джаз', 'Электроника', 'Классика', 'Готово'])
    gs = gj + ',t: ,down,t: ,wait:50'                      # после отметок «Готово» сдвигается — искать уже по отмеченному окну
    win('бот-список-3-пробел-готово', gs + ',click:' + at(gs, '( Готово', dx=3) + ',wait:200', ['бот получил'])
    win('бот-список-3-enter', gs + ',enter,wait:200', ['бот получил'])
    win('бот-список-3-больше-трёх', gj + ',t: ,down,t: ,down,t: ,down,t: ,wait:100', ['до 3'])
    # все 5 кнопок ряда видны и доступны (на 120x34 «Очистить очередь» и «Назад» не помещаются — ошибка)
    for sz in ('160x40', '120x34'):
        g2 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'игры', size=sz) + ',wait:200'
        win('бот-ряд-кнопок-' + sz, g2, ['Играть', 'Очистить очередь', 'Назад'], size=sz)
    g16 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'игры', size='160x40') + ',wait:200'
    win('бот-недоступная', g16 + ',click:' + at(g16, '( Назад )', dx=3, size='160x40', tol=0) + ',wait:200', ['кнопка недоступна'],
        size='160x40')
    # голосовое сообщение
    vm = at(ig, '[>]', dx=1)
    s = win('голосовое-воспроизведение', ig + ',click:' + vm + ',wait:1500', ['[#]'], absent=['[>] ---'])
    m = _re.search(r'(\d+):(\d\d) / 0:07', s.dump())
    check('голосовое-воспроизведение', 'время идёт (%s)' % (m and m.group(0)), bool(m) and m.group(0) != '0:00 / 0:07')
    win('голосовое-пауза', ig + ',click:' + vm + ',wait:800,click:' + vm + ',wait:200', ['[>]'])
    win('голосовое-до-конца', ig + ',click:' + vm + ',wait:9500', ['[>]'], absent=['загрузка'])   # после конца — снова [>] 0:00
    # эмбед и опросы
    i60 = 'click:3:7,wait:50,click:' + at('click:3:7,wait:50', 'игры', size='120x60') + ',wait:200'
    s = shot.run(i60, 'эмбед', '120x60')
    common('эмбед', s, 2)
    expect('эмбед', s, ['Размер', '1,2 ГБ', 'Платформы'])
    win('опрос-завершён', ig, ['Опрос завершён', 'Пятница', '9 голос(ов) - итог'])
    win('опрос-голос', ig + ',click:' + at(ig, 'Кооператив', dx=1) + ',wait:200', ['Кооператив'])
    win('опрос-убрать-голос', ig + ',click:' + at(ig, '[Убрать голос]', dx=2) + ',wait:200', ['Выберите один ответ'])
    # М5: ветка удалена, пока открыта
    kt = SRV + ',click:' + at(SRV, 'обсуждение котов') + ',wait:300'
    inp = at(kt, 'Написать #обсуждение')
    dl = kt + ',click:' + inp + ',t:!delthread,enter,wait:3500'
    win('м5-ветка-удалена', dl, ['эту ветку удалили'])
    win('м5-отправка-закрыта', dl + ',click:' + inp + ',t:ещё,enter,wait:200', ['эту ветку удалили'], absent=['____ ещё'])   # текст остаётся в поле, сообщением не уходит
    # слова-тревоги списком
    g = 'click:' + at('wait:50', '(*)', dx=1) + ',wait:50'
    n = g + ',click:' + at(g, 'Уведомления') + ',wait:100'
    e = n + ',click:' + at(n, 'Изменить слова', dx=2) + ',wait:100'
    sv = e + ',t:булок\\, чаю,click:' + at(e, 'Сохранить', dx=2, last=True) + ',wait:100'
    win('ст-список-сохранён', sv + ',click:' + at('wait:50', '(*)', dx=1) + ',wait:50,click:' + at(g, 'Уведомления') + ',wait:100',
        ['булок', 'чаю'])
    # посты форума с сообщениями
    fo = SRV + ',click:' + at(SRV, 'вопросы') + ',wait:200'
    for post in ('Как настроить звук?', 'Покажите свои рабочие места'):
        win('форум-пост-' + post[:12], fo + ',click:' + at(fo, post) + ',wait:600', [post[:12]])


def t_packV():
    """Пакет В (дизайнер ~19:00): вкладки, ширина колонок мышью, предпросмотр разметки, крупный аватар, уведомления
    системы, рация (x1click), авто-порог, архив/меню веток, новый пост, участники ветки, переименовать группу,
    «Изменить профиль», режим стримера, фокус, фильтр слов, статус голосового, полный экран."""
    Z = '120x50'
    g = 'click:' + at('wait:50', '(*)', dx=1, size=Z) + ',wait:50'
    sec = lambda name: g + ',click:' + at(g, name, size=Z) + ',wait:100'
    # --- настройки
    ac = sec('Моя учётная запись')
    win('пВ-изменить-профиль', ac + ',click:' + at(ac, 'Изменить профиль', dx=2, size=Z) + ',wait:100', ['Сохранить'], size=Z)
    pr = sec('Конфиденциальность')
    st = pr + ',click:' + at(pr, 'Скрывать личное на экране', size=Z) + ',wait:100'
    win('пВ-стример-вкл', st, ['[x] Скрывать личное'], size=Z)
    # личное в демо — слова-тревоги: заданы «булок», при режиме стримера в «Уведомлениях» не видны
    nt0 = sec('Уведомления')
    ew = nt0 + ',click:' + at(nt0, 'Изменить слова', dx=2, size=Z) + ',wait:100'
    aw = ew + ',t:булок,click:' + at(ew, 'Сохранить', dx=2, last=True, size=Z) + ',wait:100'
    win('пВ-слова-видны-без-стримера', aw + ',' + g + ',click:' + at(g, 'Уведомления', size=Z) + ',wait:100', ['булок'], size=Z)
    win('пВ-стример-скрывает-слова', aw + ',' + g + ',click:' + at(g, 'Конфиденциальность', size=Z) + ',wait:100,click:' +
        at(pr, 'Скрывать личное на экране', size=Z) + ',wait:100,click:' + at(g, 'Уведомления', size=Z) + ',wait:100',
        ['СЛОВА-ТРЕВОГИ'], absent=['булок'], size=Z)
    lk = sec('Внешний вид')
    win('пВ-полный-экран-кнопка', lk + ',click:' + at(lk, 'Во весь экран', dx=2, size=Z) + ',wait:100', ['Во весь экран'], size=Z)
    vo = sec('Голос и видео')
    pt = vo + ',click:' + at(vo, '( Назначить', dx=3, size=Z) + ',wait:100'
    win('пВ-рация-ожидание-клавиши', pt, ['КЛАВИША РАЦИИ'], size=Z)
    win('пВ-рация-x1click', pt + ',x1click:60:20,wait:100', ['мышь: кнопка 4', 'Сбросить'], absent=['F8 (с --ptt)'], size=Z)
    win('пВ-авто-порог', vo + ',click:' + at(vo, '[ ] Автоматически', dx=2, size=Z) + ',wait:100', ['[x] Автоматически'], size=Z)
    nt = sec('Уведомления')
    n1 = nt + ',click:' + at(nt, 'Показывать уведомления', dx=2, size=Z) + ',wait:100'
    s1 = win('пВ-уведомления-1', n1, ['Показывать уведомления'], size=Z)
    s2 = win('пВ-уведомления-2', n1 + ',click:' + at(nt, 'Показывать уведомления', dx=2, size=Z) + ',wait:100',
             ['Показывать уведомления'], size=Z)
    m1 = _first(s1.dump(), r'Показывать уведомления: ([^)]+?) *\)')
    m2 = _first(s2.dump(), r'Показывать уведомления: ([^)]+?) *\)')
    check('пВ-уведомления-по-кругу', 'без текста -> %s -> %s' % (m1, m2), len({'без текста', m1, m2}) == 3)
    check('пВ-уведомления-предупреждение', '«с текстом» — предупреждение про историю на диске',
          any('с текстом' in (m or '') for m in (m1, m2)) and (('истори' in s1.dump()) or ('истори' in s2.dump())))
    fe = nt + ',click:' + at(nt, 'Изменить фильтр', dx=2, size=Z) + ',wait:100'
    fs = fe + ',t:булок,click:' + at(fe, 'Сохранить', dx=2, last=True, size=Z) + ',wait:100'
    chz = 'esc,wait:50,' + SRV + ',click:' + at(SRV, 'general', size=Z) + ',wait:150'
    f2 = fs + ',' + chz
    win('пВ-фильтр-скрывает', f2, ['скрыто фильтром'], absent=['съешь же ещё этих мягких'], size=Z)
    win('пВ-фильтр-показать', f2 + ',click:' + at(f2, '[показать]', dx=2, size=Z) + ',wait:100', ['съешь же ещё этих мягких'],
        size=Z)
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:150'
    # --- вкладки: каждый открытый канал — вкладка (полоса при >= 2); Ctrl+Tab, Alt+1..9, Ctrl+W при пустом поле,
    # средний щелчок по вкладке — закрыть
    t2 = ch + ',click:' + at(SRV, 'новости') + ',wait:150'
    s9 = win('пВ-вкладки-две', t2, ['general', 'новости'])
    check('пВ-вкладки-полоса', 'в шапке полоса вкладок (general и новости в одной строке)',
          any('general' in l and 'новости' in l for l in s9.text()[:3]))
    win('пВ-вкладки-ctrl+tab', t2 + ',ctrl+tab,wait:100', [])
    r1 = shot.run(t2 + ',alt+1,wait:150', 'пВ-вкладки-alt1', '120x34')
    r2 = shot.run(t2 + ',alt+2,wait:150', 'пВ-вкладки-alt2', '120x34')
    same_hdr = (r1.img[:r1.ch] == r2.img[:r2.ch]).all()        # активная вкладка — только цветом шапки
    check('пВ-вкладки-alt+1/2', 'Alt+1 и Alt+2 открывают разные вкладки (лента разная)',
          r1.text()[5:30] != r2.text()[5:30] and not same_hdr)
    win('пВ-вкладки-ctrl+w', t2 + ',ctrl+w,wait:100', [])
    inp = at(ch, 'Написать #general')
    # Ctrl+W в непустом поле стирает слово (как раньше), вкладку не закрывает
    s3 = win('пВ-вкладки-ctrl+w-непустое-поле', t2 + ',click:' + inp + ',t:черновик,ctrl+w,wait:100', ['(#general', '(#новости'])
    # --- ширина колонки каналов: перетаскивание границы и двойной щелчок — сброс
    t0 = shot.run(ch, 'пВ-ширина-до', '120x34').text()
    bx = t0[10].index('|', 12)                    # граница колонки каналов (9 — граница колонки серверов)
    t1 = shot.run(ch + ',drag:%d:10:%d:10,wait:100' % (bx, bx + 8), 'пВ-ширина-шире', '120x34').text()
    bx1 = t1[10].index('|', 12)
    check('пВ-ширина-drag', 'граница колонки каналов сдвинулась (%d -> %d)' % (bx, bx1), bx1 > bx)
    t3 = shot.run(ch + ',drag:%d:10:%d:10,wait:100,click:%d:10,click:%d:10,wait:100' % (bx, bx + 8, bx1, bx1),
                  'пВ-ширина-сброс', '120x34').text()
    check('пВ-ширина-двойной-щелчок', 'двойной щелчок по границе — ширина по умолчанию (%d)' % t3[10].index('|', 12),
          t3[10].index('|', 12) == bx)
    # --- предпросмотр разметки над полем
    win('пВ-предпросмотр', ch + ',click:' + inp + ',t:**жир** __под__ `код`,wait:100', ['жир под код'])
    # --- крупный аватар из карточки профиля
    pc = ch + ',click:' + at(ch, '<Алиса>', dx=1) + ',wait:100'
    s4 = shot.run(pc, 'пВ-карточка', '120x34')
    av = next(((l.find('^<__>^'), y) for y, l in enumerate(s4.text()) if '^<__>^' in l), None)   # низ круглого аватара 6x3
    sb = shot.run(pc + ',click:%d:%d,wait:200' % (av[0] + 3, av[1] - 1), 'пВ-крупный-аватар-a', '120x34') if av else None
    check('пВ-крупный-аватар', 'щелчок по аватару карточки — крупный вид (экран сильно меняется)',
          bool(sb) and sum(a != b for a, b in zip(sb.text(), s4.text())) > 10)
    # --- ветки и форум
    tm = SRV + ',rclick:' + at(SRV, 'обсуждение котов') + ',wait:50'
    win('пВ-меню-ветки', tm, ['Переименовать ветку', 'В архив', 'Закрыть ветку', 'Участники ветки', 'Покинуть ветку'])
    win('пВ-участники-ветки', tm + ',click:' + at(tm, 'Участники ветки') + ',wait:100', ['Алиса', 'Боб'])
    win('пВ-переименовать-ветку', tm + ',click:' + at(tm, 'Переименовать ветку') + ',wait:100', ['Сохранить'])
    win('пВ-ветка-в-архив', tm + ',click:' + at(tm, 'В архив') + ',wait:200', [], absent=['обсуждение котов'])
    fo = SRV + ',click:' + at(SRV, 'вопросы') + ',wait:200'
    win('пВ-форум-новый-пост', fo + ',click:' + at(fo, 'Новый пост', dx=2) + ',wait:100', ['Новый пост в', 'Опубликовать'])
    win('пВ-форум-даты', fo, ['посл.: 08.11.2026', '2 сообщ.'], absent=['01.01.2015'])
    # --- беседы, голосовой, статус
    gm = 'rclick:' + at('wait:50', 'Алиса, Боб') + ',wait:50'
    win('пВ-переименовать-группу', gm + ',click:' + at(gm, 'Переименовать группу') + ',wait:100', ['Переименовать группу', 'Сохранить'])
    vm = SRV + ',rclick:' + at(SRV, '<) Игры', dx=3) + ',wait:50'
    win('пВ-статус-голосового', vm + ',click:' + at(vm, 'Изменить статус') + ',wait:100', ['Статус голосового канала', 'Сохранить'])
    sm = 'click:' + at('wait:50', '(В сети)', dx=-3) + ',wait:50'
    win('пВ-меню-статуса-фокус', sm, ['Не беспокоить'])


def _first(text, rx):
    import re as _re
    m = _re.search(rx, text)
    return m.group(1).strip() if m else None


def t_queue1():
    """Очередь 1 (дизайнер ~21:00): статус на время и текст, игнор (свёрнутые сообщения «[показать]»), личная заметка,
    «Фильтр гула» и «Звуки звонка и событий» в «Голос и видео». Правила сервера (№128) — когда будет демо-сервер."""
    sm = 'click:' + at('wait:50', '(В сети)', dx=-3, last=True) + ',wait:50'
    win('о1-меню-статуса', sm, ['Статус на время и текст', 'Режим фокуса: 30 мин', 'Невидимый'])
    sf = sm + ',click:' + at(sm, 'Статус на время и текст') + ',wait:100'
    win('о1-форма-статуса', sf, ['Сохранить'])
    win('о1-форма-поля', sf, ['СТАТУС', 'НА СКОЛЬКО', 'ТЕКСТОВЫЙ СТАТУС', 'ТЕКСТ УБРАТЬ ЧЕРЕЗ'])
    win('о1-статус-текст', sf + ',tab,tab,t:на обеде,enter,wait:100', ['статус сохранён'])
    win('о1-статус-текст-виден', sf + ',tab,tab,t:на обеде,enter,wait:100,' + sm.split(',')[0] + ',wait:50,click:' +
        at(sm, 'Статус на время и текст') + ',wait:100', ['на обеде'])
    win('о1-фокус-30', sm + ',click:' + at(sm, 'Режим фокуса: 30 мин') + ',wait:100', ['фокус'])
    ch = SRV + ',click:' + at(SRV, 'general') + ',wait:150'
    pc = ch + ',click:' + at(ch, '<Боб>', dx=1) + ',wait:100'
    win('о1-карточка', pc, ['Личная заметка', '( Игнор )', '( Роли )'])
    ig = pc + ',click:' + at(pc, '( Игнор )', dx=2, tol=0) + ',wait:100,esc,wait:50'
    s = win('о1-игнор-свёрнуто', ig, ['[показать]'], absent=['привет! в #новости смешное'])
    win('о1-игнор-показать', ig + ',click:' + at(ig, '[показать]', dx=2) + ',wait:100', ['привет! в #новости смешное'])
    nt = pc + ',click:' + at(pc, 'Личная заметка', dx=2) + ',wait:100'
    win('о1-заметка-раскрыта', nt, ['ЗАМЕТКА', 'пусто'])
    ne = nt + ',click:' + at(nt, 'пусто', dx=1) + ',wait:100,t:знаком по работе,enter,wait:100'
    win('о1-заметка-сохранена', ne + ',esc,wait:50,click:' + at(ch, '<Боб>', dx=1) + ',wait:100,click:' + at(pc, 'Личная заметка', dx=2) +
        ',wait:100', ['знаком по работе'])
    Z = '120x50'
    g = 'click:' + at('wait:50', '(*)', dx=1, size=Z) + ',wait:50'
    vo = g + ',click:' + at(g, 'Голос и видео', size=Z) + ',wait:100'
    win('о1-фильтр-гула-снять', vo + ',click:' + at(vo, 'Фильтр гула', size=Z) + ',wait:100', ['[ ] Фильтр гула'], size=Z)
    win('о1-звуки-звонка-снять', vo + ',click:' + at(vo, 'Звуки звонка', size=Z) + ',wait:100', ['[ ] Звуки звонка'], size=Z)


def t_lang_switch():
    """Переключатель языка без перезапуска: «Внешний вид» (Русский / English), карточка входа и стартовый экран
    («RU|EN» в рамке). Остальная проверка английского — lang_scan.py / lang_static.py."""
    Z = '120x50'
    g = 'click:' + at('wait:50', '(*)', dx=1, size=Z) + ',wait:50'
    lk = g + ',click:' + at(g, 'Внешний вид', size=Z) + ',wait:100'
    win('язык-пункт', lk, ['ЯЗЫК / LANGUAGE', 'Русский', 'English'], size=Z)
    en = lk + ',click:' + at(lk, 'English', dx=2, size=Z) + ',wait:100'
    win('язык-en-сразу', en, ['Appearance', 'LANGUAGE'], absent=['Внешний вид'], size=Z)
    win('язык-en-главная', en + ',esc,wait:100', ['Friends'], absent=['Друзья'], size=Z)
    win('язык-en-ru-обратно', en + ',click:' + at(lk, 'Русский', dx=2, size=Z) + ',wait:100', ['Внешний вид'], size=Z)
    lg = 'ctrl+l,wait:100'
    s = shot.run(lg, 'язык-вход', '120x34')
    p = s.find('RU|EN', tol=0)
    if p:
        win('язык-вход-en', lg + ',click:%d:%d,wait:100' % (p[0] + 3, p[1]), ['RU|EN'], absent=['Подключение'])
        win('язык-вход-ru-обратно', lg + ',click:%d:%d,wait:100,click:%d:%d,wait:100' % (p[0] + 3, p[1], p[0] + 1, p[1]),
            ['Подключение'])
    else:
        check('язык-вход', 'на карточке входа есть RU|EN', False)
    s = shot.run('wait:100', 'язык-старт', '120x34', extra=['--demo-start'])
    p = s.find('RU|EN', tol=0)
    if p:
        win('язык-старт-en', 'wait:100,click:%d:%d,wait:100' % (p[0] + 3, p[1]), ['RU|EN'], absent=['Подключение'],
            extra=['--demo-start'])
    else:
        check('язык-старт', 'на стартовом экране есть RU|EN', False)


def t_updates_duck():
    """«Настройки > Обновления» (версия, «Проверять при запуске», «Проверить сейчас») и «Приглушать другие программы
    во время звонка» в «Голос и видео» — на русском и английском (--lang). Окно обновления — когда будет --demo-update."""
    Z = '120x50'
    for lang, T in (('ru', {'sec': 'Обновления', 'ver': 'ТЕКУЩАЯ ВЕРСИЯ', 'auto': 'Проверять обновления при запуске',
                            'now': 'Проверить сейчас', 'voice': 'Голос и видео', 'duck': 'Приглушать другие программы'}),
                    ('en', {'sec': 'Updates', 'ver': 'CURRENT VERSION', 'auto': 'Check for updates at startup',
                            'now': 'Check now', 'voice': 'Voice & Video', 'duck': 'Lower other apps'})):
        os.environ['UITEST_LANG'] = lang
        try:
            g = 'click:' + at('wait:50', '(*)', dx=1, size=Z) + ',wait:50'
            s = shot.run(g, 'обн-поиск-' + lang, Z)
            if not s.has(T['sec']):
                check('обн-%s' % lang, 'раздел «%s» в настройках' % T['sec'], False, s.dump()[:200])
                continue
            u = g + ',click:' + at(g, T['sec'], size=Z) + ',wait:100'
            win('обн-раздел-' + lang, u, [T['ver'], 'Astra DS 0.', '[x] ' + T['auto'], T['now']], size=Z)
            win('обн-снять-галочку-' + lang, u + ',click:' + at(u, T['auto'], size=Z) + ',wait:100', ['[ ] ' + T['auto']], size=Z)
            win('обн-проверить-сейчас-' + lang, u + ',click:' + at(u, T['now'], dx=2, size=Z) + ',wait:500', [T['now']], size=Z)
            v = g + ',click:' + at(g, T['voice'], size=Z) + ',wait:100'
            sv = shot.run(v, 'обн-голос-' + lang, Z)
            if sv.has(T['duck']):
                win('приглушать-по-умолч-' + lang, v, ['[ ] ' + T['duck']], size=Z)
                win('приглушать-вкл-' + lang, v + ',click:' + at(v, T['duck'], size=Z) + ',wait:100', ['[x] ' + T['duck']], size=Z)
            else:
                check('приглушать-' + lang, 'галочка «%s» в «%s»' % (T['duck'], T['voice']), False,
                      ' | '.join(l.strip()[:60] for l in sv.text() if '[' in l)[:300])
        finally:
            os.environ.pop('UITEST_LANG', None)


def t_update_window():
    """Окно обновления (--demo-update=available|downloading|ready|failed|confirm; без сети), ru и en, 50x12."""
    A = 'wait:300'
    U = lambda st: ['--demo-update=' + st]
    for lang, T in (('ru', {'av': 'Доступна версия 9.9.0', 'now': 'сейчас у вас 0.1.1', 'new': 'ЧТО НОВОГО', 'later': '( Позже',
                            'upd': '( Обновить', 'rdy': 'Версия 9.9.0 загружена', 'rst': 'Перезапустить', 'mb': '/ 10.1 МБ',
                            'dl': 'Загрузка версии 9.9.0'}),
                    ('en', {'av': '9.9.0 is available', 'now': '0.1.1 now', 'new': 'NEW', 'later': '( Later',
                            'upd': '( Update', 'rdy': '9.9.0 downloaded', 'rst': 'Restart', 'mb': '/ 10.1 MB',
                            'dl': 'Downloading version 9.9.0'})):
        os.environ['UITEST_LANG'] = lang
        try:
            win('ов-доступна-' + lang, A, [T['av'], T['now'], T['new'], T['later'], T['upd']], extra=U('available'), mouse=False)
            win('ов-позже-' + lang, A + ',click:' + at(A, T['later'], dx=3, extra=U('available')) + ',wait:200', [], absent=[T['av']],
                extra=U('available'))
            win('ов-esc-' + lang, A + ',esc,wait:200', [], absent=[T['av']], extra=U('available'), mouse=False)
            s = win('ов-обновить-' + lang, A + ',click:' + at(A, T['upd'], dx=3, extra=U('available')) + ',wait:800', [],
                    extra=U('available'))
            check('ов-обновить-' + lang, '«Обновить» в демо даёт отклик (переход к загрузке или сообщение)',
                  not s.has(T['av']) or 'демо' in s.text()[-1] or 'demo' in s.text()[-1])
            s = win('ов-загрузка-' + lang, A, [T['dl'], '####'], extra=U('downloading'), mouse=False)
            check('ов-загрузка-ед-' + lang, 'единица в полосе по языку интерфейса (%s)' % T['mb'], s.has(T['mb'], tol=0))
            win('ов-готово-' + lang, A, [T['rdy'], T['rst']], extra=U('ready'), mouse=False)
            win('ов-перезапуск-' + lang, A + ',click:' + at(A, T['rst'], dx=2, extra=U('ready')) + ',wait:300', [], extra=U('ready'))
        finally:
            os.environ.pop('UITEST_LANG', None)
    # ошибка: при запуске сразу в failed окна нет (показывается, если окно уже было открыто) — видна в «Обновлениях»
    Z = '120x50'
    g = 'click:' + at('wait:50', '(*)', dx=1, size=Z) + ',wait:50'
    u = g + ',click:' + at(g, 'Обновления', size=Z) + ',wait:100'
    win('ов-ошибка-в-настройках', A + ',' + u, ['хэш файла не совпал'], size=Z, extra=U('failed'))
    # общая папка (--demo-update=confirm): предупреждение сразу; Enter НЕ соглашается, Esc — отмена к обычному окну
    win('ов-общая-папка', A, ['открыт на запись другим', '( Отмена', 'Обновить всё равно'], extra=U('confirm'), mouse=False)
    win('ов-общая-папка-enter', A + ',enter,wait:500', ['открыт на запись другим', 'Обновить всё равно'], extra=U('confirm'),
        mouse=False)
    win('ов-общая-папка-esc', A + ',esc,wait:300', ['( Позже', '( Обновить'], absent=['открыт на запись другим'],
        extra=U('confirm'), mouse=False)
    s = shot.run(A, 'ов-общая-папка-текст', '120x34', extra=U('confirm'))
    check('ов-общая-папка-текст', 'предупреждение целиком (до «Обновить всё равно?») и без «—»',
          s.has('всё равно?', tol=0) and '—' not in s.dump())
    win('ов-доступна-50x12', A, ['9.9.0'], size='50x12', extra=U('available'), mouse=False)
    win('ов-доступна-200x60', A, ['Доступна версия 9.9.0', 'Обновить'], size='200x60', extra=U('available'), mouse=False)


ALL = [t_home, t_lang_switch, t_updates_duck, t_update_window, t_queue1, t_vstart, t_alert_links, t_bots_misc, t_packV, t_packA, t_look_voice_slow, t_viewer, t_select_threads, t_wave1, t_wave1b, t_wave2, t_txt, t_call, t_friends, t_newdm, t_dm, t_server, t_search, t_file, t_voice, t_settings, t_help, t_login, t_sizes, t_fine]


def main():
    flt = sys.argv[1] if len(sys.argv) > 1 else ''
    t0 = time.time()
    if flt and flt != 't_home':
        t_home()                                # база памяти
    for t in ALL:
        if flt and flt not in t.__name__:
            continue
        print('==', t.__name__)
        try:
            r = t()
            if t is t_server and (not flt or 'message' in flt or flt == 't_server'):
                t_messages(r)
        except Exception as e:                  # сценарий не дошёл: отметить и идти дальше
            check(t.__name__, 'сценарий выполнен', False, repr(e))
    bad = [r for r in RES if not r[2]]
    lines = ['%s %-34s %s%s' % ('ok  ' if r[2] else 'FAIL', r[0], r[1], (' - ' + r[3]) if r[3] and not r[2] else '') for r in RES]
    with open(os.path.join(shot.OUT, 'report.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    for f in os.listdir(shot.OUT):              # BMP ~3 МБ каждый: остаются только png
        if f.endswith('.bmp') or f in ('big11mb.bin', 'small.txt'):
            os.remove(os.path.join(shot.OUT, f))
    print('\nИтог: %d ok, %d FAIL, %.0f с' % (len(RES) - len(bad), len(bad), time.time() - t0))


if __name__ == '__main__':
    main()
