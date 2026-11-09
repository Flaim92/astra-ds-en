# -*- coding: utf-8 -*-
"""Стенд окон не открывает микрофон без нужды. Windows записывает каждое обращение программы к микрофону в
HKCU\\...\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged\\<путь exe через #>\\LastUsedTimeStart.
Прогоны без голоса не должны менять отметку; вход в голосовой канал (контроль) — должен.
    python tools/uitest/mic_check.py"""
import os, sys, time, winreg
import shot

KEY = r'Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone\NonPackaged'
SUB = os.path.abspath(shot.EXE).replace('\\', '#')


def stamp():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY + '\\' + SUB) as k:
            return winreg.QueryValueEx(k, 'LastUsedTimeStart')[0]
    except OSError:
        return None                                 # ни разу не открывал микрофон


def stamps():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY + '\\' + SUB) as k:
            return winreg.QueryValueEx(k, 'LastUsedTimeStart')[0], winreg.QueryValueEx(k, 'LastUsedTimeStop')[0]
    except OSError:
        return None, None


def watch(keys):
    """Прогон --shot с опросом реестра, пока процесс жив: (был ли микрофон занят, освободился ли до выхода)."""
    import subprocess
    sc = os.path.join(shot.OUT, 'mic_keys.txt')
    with open(sc, 'w', encoding='utf-8') as f:
        f.write(keys)
    p = subprocess.Popen([shot.EXE, '--demo', '--shot=' + os.path.join(shot.OUT, 'mic.bmp'), '--shot-keys=@' + sc],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    a0, _ = stamps()
    busy = freed = False
    while p.poll() is None:
        a, z = stamps()
        if a is not None and a != a0:
            if z is not None and z < a:
                busy = True                         # Start новее Stop — микрофон занят
            elif busy and z is not None and z >= a:
                freed = True                        # освобождён, процесс ещё жив
        time.sleep(0.1)
    return busy, freed


S = 'click:3:7,wait:50'
import windows as W
VC = S + ',click:' + W.at(S, '<) ОБЩИЙ', dx=3) + ',wait:300'   # экран голосового канала (по имени: строки сдвигаются)
QUIET = [                                           # (имя, шаги, доп. флаги) — микрофон не нужен
    ('главная', 'wait:200', None),
    ('канал', S + ',click:15:6,wait:200', None),
    ('экран голосового до входа', VC, None),
    ('справка F1', 'f1,wait:100,f1,wait:50', None),
    ('вход', 'ctrl+l,wait:200', None),
    ('стартовый экран VLESS', 'wait:200', ['--demo-start']),
    ('смена голоса вне звонка F11', 'f11,wait:200', None),
]
res = []
for name, keys, extra in QUIET:
    a = stamp()
    shot.run(keys, 'mic', '120x34', extra=extra)
    time.sleep(0.5)
    b = stamp()
    ok = a == b
    res.append(ok)
    print('  [%s] %s: микрофон не открывался' % ('ok' if ok else 'FAIL', name))
# Настройки > Голос и видео: шестерёнка, раздел — без нажатия «тест микрофона» устройство не открывается
g = shot.run('wait:50', 'mic', '120x34')
p = g.find('(*)')
if p:
    gk = 'click:%d:%d,wait:50' % (p[0] + 1, p[1])
    s2 = shot.run(gk, 'mic', '120x34')
    q = s2.find('Голос и видео')
    if q:
        a = stamp()
        shot.run(gk + ',click:%d:%d,wait:300' % q, 'mic', '120x34')
        time.sleep(0.5)
        ok = stamp() == a
        res.append(ok)
        print('  [%s] Настройки > Голос и видео: микрофон не открывался' % ('ok' if ok else 'FAIL'))
# «Проверить микрофон» открывает микрофон; уход из раздела закрывает (процесс при этом ещё жив)
import windows as W
gk = 'click:' + W.at('wait:50', '(*)', dx=1) + ',wait:50'
vc = gk + ',click:' + W.at(gk, 'Голос и видео') + ',wait:100'
tb = W.at(vc, 'Проверить микрофон', dx=2)
look = W.at(gk, 'Внешний вид')
busy, freed = watch('size:120x34,' + vc + ',click:' + tb + ',wait:1500,click:' + look + ',wait:3000')
res += [busy, freed]
print('  [%s] «Проверить микрофон» открывает микрофон' % ('ok' if busy else 'FAIL'))
print('  [%s] уход из раздела «Голос и видео» закрывает микрофон (до выхода из программы)' % ('ok' if freed else 'FAIL'))
busy2, freed2 = watch('size:120x34,' + vc + ',click:' + tb + ',wait:1500,click:' + tb + ',wait:3000')
res += [busy2 and freed2]
print('  [%s] «Остановить» закрывает микрофон' % ('ok' if busy2 and freed2 else 'FAIL'))
# контроль: вход в голосовой канал открывает микрофон (иначе проверка ничего не ловит)
a = stamp()
v = shot.run(VC, 'mic', '120x34')
j = v.find('Присоединиться')
if j:
    shot.run(VC + ',click:%d:%d,wait:1500' % j, 'mic', '120x34')
    time.sleep(0.5)
    ok = stamp() != a
    res.append(ok)
    print('  [%s] контроль: вход в голос открыл микрофон (отметка сменилась)' % ('ok' if ok else 'FAIL'))
else:
    print('  [FAIL] контроль: кнопка «Присоединиться» не найдена')
    res.append(False)
print('Итог: %d ok, %d FAIL' % (sum(res), len(res) - sum(res)))
sys.exit(0 if all(res) else 1)
