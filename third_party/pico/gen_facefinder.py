# Генератор third_party/pico/facefinder.h из исходного двоичного каскада pico (rnt/cascades/facefinder).
# Запуск: python gen_facefinder.py facefinder facefinder.h
import struct, sys, hashlib
b = open(sys.argv[1], 'rb').read()
d, n = struct.unpack('<2i', b[8:16])
assert d == 6
nodes, leaves = (1 << d) - 1, 1 << d
off = nodes * 4 + leaves * 4 + 4
assert 16 + n * off == len(b)
o = []
o.append('/* facefinder.h — каскад детектора лица pico (Nenad Markus, MIT, см. LICENSE и SOURCE.txt).')
o.append(' * Сгенерирован gen_facefinder.py из rnt/cascades/facefinder (SHA-256 %s)' % hashlib.sha256(b).hexdigest())
o.append(' * без изменения значений: сравнения пикселей (int8), выходы листьев и пороги (float, точные шестнадцатеричные). */')
o.append('#define PICO_TDEPTH %d' % d)
o.append('#define PICO_NTREES %d' % n)
tc, lut, thr = [], [], []
for t in range(n):
    p = 16 + t * off
    tc.append(struct.unpack('<%db' % (nodes * 4), b[p:p + nodes * 4]))
    lut.append(struct.unpack('<%df' % leaves, b[p + nodes * 4:p + nodes * 4 + leaves * 4]))
    thr.append(struct.unpack('<f', b[p + off - 4:p + off])[0])
def fl(x):
    return float(x).hex() + 'f' if x != 0 else '0.0f'
o.append('static const int8_t PICO_TC[PICO_NTREES][%d] = {' % (nodes * 4))
for t in tc: o.append('{' + ','.join(str(v) for v in t) + '},')
o.append('};')
o.append('static const float PICO_LUT[PICO_NTREES][%d] = {' % leaves)
for t in lut: o.append('{' + ','.join(fl(v) for v in t) + '},')
o.append('};')
o.append('static const float PICO_THR[PICO_NTREES] = {')
for i in range(0, n, 8): o.append(','.join(fl(v) for v in thr[i:i + 8]) + ',')
o.append('};')
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write('\n'.join(o) + '\n')
