# -*- coding: utf-8 -*-
"""Автообновление без сети: --version, --no-update, отказ манифесту с чужим ключом / испорченной подписью
(ST_UPD_MANIFEST -> строка selftest «манифест из sign_release.sh проходит встроенным ключом» должна быть FAIL),
и что в --demo процесс не открывает сетевых соединений. Настоящий ключ подписи не используется — свой ed25519
во временном каталоге стенда (out/upd, удаляется). Ветка «версия не новее» с ВЕРНОЙ подписью — только в selftest
(тестовый ключ, проверка t4), т. к. настоящим ключом стенд не подписывает.
    python tools/uitest/upd_check.py"""
import os, re, shutil, subprocess, sys, time
import shot

OUT = os.path.join(shot.OUT, 'upd')
res = []


def ok(what, cond, detail=''):
    res.append(bool(cond))
    print('  [%s] %s%s' % ('ok' if cond else 'FAIL', what, (' - ' + detail) if detail and not cond else ''))


def run(args, env=None, timeout=120):
    r = subprocess.run([shot.EXE] + args, capture_output=True, timeout=timeout, env=env)
    return r.returncode, r.stdout.decode('utf-8', 'replace')


def selftest_line(manifest):
    e = dict(os.environ, ST_UPD_MANIFEST=manifest)
    _, out = run(['--selftest'], env=e, timeout=300)
    m = [l for l in out.splitlines() if 'манифест из sign_release.sh' in l]
    return m[0] if m else ''


def sign(path, key):
    subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', key, '-in', path, '-out', path + '.sig'], check=True)


def main():
    rc, out = run(['--version'])
    ok('--version: «Astra DS x.y.z», код 0 (%s)' % out.strip(), rc == 0 and re.match(r'Astra DS \d+\.\d+\.\d+', out.strip()))
    _, out = run(['--help'])
    ok('--help описывает --no-update и --version', '--no-update' in out and '--version' in out)
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    try:
        key = os.path.join(OUT, 'foreign.pem')
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'ed25519', '-out', key], check=True, capture_output=True)
        f = os.path.join(OUT, 'astra-ds.bin')
        with open(f, 'wb') as h:
            h.write(b'hello')
        man = os.path.join(OUT, 'manifest-win-x64.json')
        with open(man, 'w') as h:
            h.write('{"product":"astra-ds","platform":"win-x64","version":"99.0.0","file":"astra-ds.bin","size":5,'
                    '"sha256":"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"}')
        sign(man, key)
        l1 = selftest_line(man)
        ok('чужой ключ (версия 99.0.0): отказ, причина — подпись', '[FAIL]' in l1 and 'подпись' in l1, l1.strip())
        with open(man + '.sig', 'r+b') as h:            # подпись на 63 байта
            h.truncate(63)
        l2 = selftest_line(man)
        ok('подпись короче 64 байт: отказ', '[FAIL]' in l2, l2.strip())
        os.remove(man + '.sig')
        l3 = selftest_line(man)
        ok('нет файла подписи: отказ', '[FAIL]' in l3, l3.strip())
    finally:
        shutil.rmtree(OUT, ignore_errors=True)
    # демо: сетевых соединений нет (netstat по PID), с --no-update и без
    for extra in ([], ['--no-update']):
        p = subprocess.Popen([shot.EXE, '--demo'] + extra + ['--shot=' + os.path.join(shot.OUT, 'upd.bmp'),
                             '--shot-keys=size:120x34,wait:6000'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        conns = set()
        t = time.time()
        while p.poll() is None and time.time() - t < 15:
            ns = subprocess.run(['netstat', '-ano', '-p', 'TCP'], capture_output=True).stdout.decode('cp866', 'replace')
            conns.update(l.split()[2] for l in ns.splitlines() if l.strip().endswith(' %d' % p.pid) and len(l.split()) >= 5)
            time.sleep(0.5)
        p.wait()
        ok('--demo %s: TCP-соединений нет (%s)' % (' '.join(extra) or '(без флагов)', ', '.join(sorted(conns)) or 'нет'), not conns)
    print('Итог: %d ok, %d FAIL' % (sum(res), len(res) - sum(res)))
    return 0 if all(res) else 1


if __name__ == '__main__':
    sys.exit(main())
