# -*- coding: utf-8 -*-
"""Поиск старого названия в видимых строках клиента (после переименования mincord -> Astra DS).
Источники: тексты экранов последнего прогона windows.py (out/*.txt, --shot-text), вывод --help, --cli (демо,
набор команд), поток --tui (ConPTY). Печатает каждую уникальную строку с находкой и где она встретилась.
    python tools/uitest/brand_scan.py [слово]      (по умолчанию mincord, без учёта регистра)"""
import glob, os, re, subprocess, sys
import shot

WORD = sys.argv[1] if len(sys.argv) > 1 else 'mincord'
rx = re.compile(re.escape(WORD), re.I)
hits = {}


def scan(text, where):
    for ln in text.splitlines():
        if re.search(r'projects[\\/]mincord', ln, re.I):   # путь к каталогу проекта в набранном пути файла — не строка клиента
            continue
        for m in rx.finditer(ln):
            frag = ln[max(0, m.start() - 30):m.end() + 30].strip()
            hits.setdefault(frag, where)


# 1) экраны последнего прогона windows.py (свежие .txt, служебные файлы стенда не считаются)
skip = re.compile(r'^(report|full|at|_cur|e_.*|x\d*|core_.*|leak.*)\.txt$')
n = 0
rep = os.path.join(shot.OUT, 'report.txt')
fresh = os.path.getmtime(rep) - 3600 if os.path.exists(rep) else 0   # только экраны последнего прогона (не старше часа до отчёта)
for f in glob.glob(os.path.join(shot.OUT, '*.txt')):
    if skip.match(os.path.basename(f)) or os.path.getmtime(f) < fresh:
        continue
    n += 1
    scan(open(f, encoding='utf-8', errors='replace').read(), 'экран ' + os.path.basename(f)[:-4])
# 2) --help
r = subprocess.run([shot.EXE, '--help'], capture_output=True, timeout=30)
scan(r.stdout.decode('utf-8', 'replace'), '--help')
# 3) --cli (демо): приветствие, /help, списки, статус
cmds = '/help\n/servers\n/channels 1\n/open general\n/status\n/friends\n/dms\n/vless\n/quit\n'
r = subprocess.run([shot.EXE, '--demo', '--cli'], input=cmds.encode('utf-8'), capture_output=True, timeout=60)
scan(r.stdout.decode('utf-8', 'replace'), '--cli')
# 4) --tui (ConPTY): первый кадр, справка
try:
    import tui_conpty
    data, marks, _ = tui_conpty.run(120, 34, keys=[b'', b'\x1bOP', b'\x1b'])
    scan(tui_conpty.plain(data), '--tui')
except Exception as e:                      # нет ConPTY — не повод молчать
    print('  --tui не проверен:', e)

print('искали «%s»: экранов %d, --help, --cli, --tui; находок %d' % (WORD, n, len(hits)))
for frag, where in sorted(hits.items(), key=lambda x: x[1]):
    print('  [%s] %s' % (where, frag))
sys.exit(1 if hits else 0)
