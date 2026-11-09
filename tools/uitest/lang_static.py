# -*- coding: utf-8 -*-
"""Статическая проверка перевода: строки TR("...") в ui.c без пары в ui_en.h и кириллические литералы ui.c,
не обёрнутые в TR() (кандидаты «забыли обернуть»; комментарии и строки рядом с O.demo/demo не считаются).
Строки других файлов (store/net/main/login/...) с кириллицей — список для разработчика (ui_tr у него).
    python tools/uitest/lang_static.py"""
import os, re, sys

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src')
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CYR = re.compile('[А-Яа-яЁё]')


def main():
    ui = open(os.path.join(SRC, 'ui.c'), encoding='utf-8').read()
    en = open(os.path.join(SRC, 'ui_en.h'), encoding='utf-8').read()
    keys = set(m.group(1) for m in re.finditer(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"(?:[^"\\]|\\.)*"\s*\}', en))
    used = re.findall(r'TR\(\s*"((?:[^"\\]|\\.)*)"', ui)
    miss = sorted(set(u for u in used if u not in keys))
    print('TR() в ui.c: %d разных строк, без перевода в ui_en.h: %d' % (len(set(used)), len(miss)))
    for m in miss:
        print('   ' + m)
    cand = []
    for i, l in enumerate(ui.split('\n'), 1):
        st = l.strip()
        if st.startswith(('/*', '*', '//')):
            continue
        code = l.split('/*')[0].split('//')[0]
        for lit in LIT.finditer(code):
            t = lit.group(1)
            if not CYR.search(t):
                continue
            pre = code[:lit.start()]
            if re.search(r'(TR|ui_tr|tr)\(\s*$', pre) or 'demo' in code.lower() or 'selftest' in code:
                continue
            cand.append('ui.c:%d  %s' % (i, t[:70]))
    print('кириллица в ui.c вне TR(): %d (проверить: забыто или так задумано)' % len(cand))
    for c in cand:
        print('   ' + c)
    print('кириллица в строках других файлов (разработчик; показываются пользователю через ui_notice/ошибки):')
    for f in ('store.c', 'net.c', 'main.c', 'login.c', 'vless.c', 'voice.c', 'img.c', 'captcha.c'):
        p = os.path.join(SRC, f)
        if not os.path.exists(p):
            continue
        n = 0
        for i, l in enumerate(open(p, encoding='utf-8', errors='replace').read().split('\n'), 1):
            code = l.split('/*')[0].split('//')[0]
            for lit in LIT.finditer(code):
                if CYR.search(lit.group(1)) and not re.search(r'ui_tr\(\s*$', code[:lit.start()]) and 'demo' not in code.lower():
                    n += 1
        print('   %-10s %d' % (f, n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
