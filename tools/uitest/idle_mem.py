# -*- coding: utf-8 -*-
"""Единый замер памяти простоя (координатор, ревьюер, разработчик, 09.11): 5 прогонов
--demo --shot --shot-keys="size:140x40,...,wait:2000"; медиана и разброс. Порог: ±30 КБ — шум, рост < 40 КБ
к опорному — не сигнал. Канал открывается ПО ИМЕНИ (строки списка сдвигаются при новых каналах и ветках).
    python tools/uitest/idle_mem.py              — простой + «с картинками» (#музыка, по имени) против опорных REF
    python tools/uitest/idle_mem.py каналы       — сравнить каналы между собой (general, новости, музыка, игры)"""
import statistics, sys
import shot
import windows as W

SIZE = '140x40'
S = 'click:3:7,wait:50'
# опорные (build-test2 09.11 ~18:50, 5 прогонов): простой — главный экран 2864; «с картинками» — #музыка 2944 (аватары,
# бот, эфемерное). #general не годится: двухуровневый (≈3090 / ≈3290 КБ, превью кот.png) — разброс 200+ КБ.
REF = {'простой': 2864, 'с картинками (#музыка)': 2944}


def keys_for(channel):
    return S + ',click:' + W.at(S, channel, size=SIZE, tol=0) + ',wait:2000'


def measure(keys, n=5):
    v = [shot.run(keys, 'idle', SIZE).info['mem_kb'] for _ in range(n)]
    return v, statistics.median(v)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'каналы':
        base = None
        for ch in ('general', 'новости', 'музыка', 'игры'):
            v, med = measure(keys_for(ch))
            base = base or med
            print('  #%-8s %s КБ; медиана %d, разброс %d; к #general %+d КБ' % (ch, v, med, max(v) - min(v), med - base))
        return 0
    bad = 0
    for name, keys in (('простой', 'wait:2000'), ('с картинками (#музыка)', keys_for('музыка'))):
        v, med = measure(keys)
        ref = REF.get(name)
        if ref is None:
            print('  %-24s %s КБ; медиана %d, разброс %d; опорного нет — записать в REF' % (name, v, med, max(v) - min(v)))
            continue
        d = med - ref
        sig = d >= 40
        bad += sig
        print('  %-24s %s КБ; медиана %d, разброс %d; к опорному %d: %+d КБ -> %s'
              % (name, v, med, max(v) - min(v), ref, d, 'РОСТ (сигнал)' if sig else 'норма'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
