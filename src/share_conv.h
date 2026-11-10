/* share_conv.h — общее для отдачи трансляции (streamtest.c — Windows, share_x11.c — Linux), только -DASTRA_VIDEO:
 *  1) перевод строки картинки (5-5-5 16 бит или BGRX/RGBX 32 бита, выбор точек по xmap — масштаб «ближайший») в YUV420
 *     BT.601: SSE2 по 16 точек, скалярный запасной путь даёт побитно тот же результат;
 *  2) отпечатки блоков 16x16 (Y + U + V, четыре независимые цепочки FNV-1a по 4 байта: изменение одного слова
 *     меняет отпечаток всегда) и «возраст» блока — сколько закодированных кадров он не менялся;
 *  3) карта для кодера: блок не менялся с прошлого закодированного кадра — без поиска движения и intra (P_Skip,
 *     если проходит обычная проверка пропуска, иначе P16x16 с вектором 0 и остатком — качество продолжает
 *     доводиться); все блоки стоят >= SHC_AGE_ALL кадров — кадр из одних пропусков;
 *  4) темп: 30 к/с при заметном движении, 20 — при малом, 10 Гц такт (захват 10/5 Гц) при неподвижной картинке;
 *     захват целиком не реже раза в 100 мс (200 мс после 5 с покоя), между ними — только область изменений.
 * Только вычисления в памяти; ни файлов, ни вывода. */
#ifndef SHARE_CONV_H
#define SHARE_CONV_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#if defined(__SSE2__) && !defined(SHC_NO_SIMD)
#include <emmintrin.h>
#define SHC_SSE2 1
#else
#define SHC_SSE2 0
#endif

/* ---------- строка картинки -> Y (+ U, V на чётных строках: точка с чётным индексом, как раньше) ---------- */
static inline void shc_px(unsigned R, unsigned G, unsigned B, int i, uint8_t *y, uint8_t *u, uint8_t *v) {
    int r = (int)R, g = (int)G, b = (int)B;
    y[i] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    if (u && !(i & 1)) {
        u[i / 2] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
        v[i / 2] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    }
}
/* скалярно: точки [i0, n) строки; xm — номер точки источника для каждой точки картинки (NULL — один к одному) */
static inline void shc_row16_c(const uint16_t *s, const int *xm, int i0, int n, uint8_t *y, uint8_t *u, uint8_t *v) {
    for (int i = i0; i < n; i++) {
        unsigned px = s[xm ? xm[i] : i];
        shc_px(px >> 7 & 0xF8, px >> 2 & 0xF8, px << 3 & 0xF8, i, y, u, v);
    }
}
/* rgb: порядок байт в памяти R,G,B,x (RGBx/RGBA PipeWire) вместо B,G,R,x — меняются местами R и B */
static inline void shc_row32_c(const uint32_t *s, const int *xm, int i0, int n, uint8_t *y, uint8_t *u, uint8_t *v, int rgb) {
    for (int i = i0; i < n; i++) {
        uint32_t px = s[xm ? xm[i] : i];
        if (rgb) shc_px(px & 255, px >> 8 & 255, px >> 16 & 255, i, y, u, v);
        else shc_px(px >> 16 & 255, px >> 8 & 255, px & 255, i, y, u, v);
    }
}
#if SHC_SSE2
/* 8 точек R,G,B (16-битные дорожки, 0..255) -> Y в 16-битных дорожках; формула и округление — как в shc_px */
static inline __m128i shc_y8(__m128i R, __m128i G, __m128i B) {
    __m128i t = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(R, _mm_set1_epi16(66)), _mm_mullo_epi16(G, _mm_set1_epi16(129))),
                              _mm_add_epi16(_mm_mullo_epi16(B, _mm_set1_epi16(25)), _mm_set1_epi16(128)));
    return _mm_add_epi16(_mm_srli_epi16(t, 8), _mm_set1_epi16(16));   /* сумма < 65536: без переноса */
}
static inline void shc_uv8(__m128i R, __m128i G, __m128i B, uint8_t *u, uint8_t *v) {   /* |сумма| <= 28688: в int16 */
    __m128i tu = _mm_add_epi16(_mm_sub_epi16(_mm_mullo_epi16(B, _mm_set1_epi16(112)), _mm_add_epi16(_mm_mullo_epi16(R, _mm_set1_epi16(38)), _mm_mullo_epi16(G, _mm_set1_epi16(74)))), _mm_set1_epi16(128));
    __m128i tv = _mm_add_epi16(_mm_sub_epi16(_mm_mullo_epi16(R, _mm_set1_epi16(112)), _mm_add_epi16(_mm_mullo_epi16(G, _mm_set1_epi16(94)), _mm_mullo_epi16(B, _mm_set1_epi16(18)))), _mm_set1_epi16(128));
    tu = _mm_add_epi16(_mm_srai_epi16(tu, 8), _mm_set1_epi16(128));
    tv = _mm_add_epi16(_mm_srai_epi16(tv, 8), _mm_set1_epi16(128));
    _mm_storel_epi64((__m128i *)(void *)u, _mm_packus_epi16(tu, tu));
    _mm_storel_epi64((__m128i *)(void *)v, _mm_packus_epi16(tv, tv));
}
static inline void shc_rgb555(__m128i p, __m128i *R, __m128i *G, __m128i *B) {
    __m128i m = _mm_set1_epi16(0xF8);
    *R = _mm_and_si128(_mm_srli_epi16(p, 7), m);
    *G = _mm_and_si128(_mm_srli_epi16(p, 2), m);
    *B = _mm_and_si128(_mm_slli_epi16(p, 3), m);
}
/* 4 точки BGRX (32-битные дорожки) x2 -> R,G,B в 16-битных дорожках (8 точек) */
static inline void shc_rgb32(__m128i a, __m128i b, __m128i *R, __m128i *G, __m128i *B) {
    __m128i m = _mm_set1_epi32(255);
    *R = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 16), m), _mm_and_si128(_mm_srli_epi32(b, 16), m));
    *G = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 8), m), _mm_and_si128(_mm_srli_epi32(b, 8), m));
    *B = _mm_packs_epi32(_mm_and_si128(a, m), _mm_and_si128(b, m));
}
#endif
/* i0 чётный (начало блока относительно картинки) */
static inline void shc_row16(const uint16_t *s, const int *xm, int i0, int n, uint8_t *y, uint8_t *u, uint8_t *v) {
    int i = i0;
#if SHC_SSE2
    for (; i + 16 <= n; i += 16) {
        __m128i p0, p1;
        if (xm) {                                /* выбор точек: обычные загрузки, дальше — векторно */
            uint16_t t[16];
            const int *q = xm + i;
            for (int k = 0; k < 16; k++) t[k] = s[q[k]];
            p0 = _mm_loadu_si128((const __m128i *)(const void *)t);
            p1 = _mm_loadu_si128((const __m128i *)(const void *)(t + 8));
        } else {
            p0 = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
            p1 = _mm_loadu_si128((const __m128i *)(const void *)(s + i + 8));
        }
        p0 = _mm_and_si128(p0, _mm_set1_epi16(0x7FFF));   /* старший бит 5-5-5 не используется */
        p1 = _mm_and_si128(p1, _mm_set1_epi16(0x7FFF));
        __m128i R, G, B, y0, y1;
        shc_rgb555(p0, &R, &G, &B); y0 = shc_y8(R, G, B);
        shc_rgb555(p1, &R, &G, &B); y1 = shc_y8(R, G, B);
        _mm_storeu_si128((__m128i *)(void *)(y + i), _mm_packus_epi16(y0, y1));
        if (u) {                                 /* чётные точки: младшие 16 бит каждой 32-битной дорожки */
            __m128i lo = _mm_set1_epi32(0xFFFF);
            __m128i e = _mm_packs_epi32(_mm_and_si128(p0, lo), _mm_and_si128(p1, lo));
            shc_rgb555(e, &R, &G, &B);
            shc_uv8(R, G, B, u + i / 2, v + i / 2);
        }
    }
#endif
    shc_row16_c(s, xm, i, n, y, u, v);
}
static inline void shc_row32x(const uint32_t *s, const int *xm, int i0, int n, uint8_t *y, uint8_t *u, uint8_t *v, int rgb) {
    int i = i0;
#if SHC_SSE2
    for (; i + 16 <= n; i += 16) {
        __m128i q0, q1, q2, q3;
        if (xm) {
            uint32_t t[16];
            const int *q = xm + i;
            for (int k = 0; k < 16; k++) t[k] = s[q[k]];
            q0 = _mm_loadu_si128((const __m128i *)(const void *)t); q1 = _mm_loadu_si128((const __m128i *)(const void *)(t + 4));
            q2 = _mm_loadu_si128((const __m128i *)(const void *)(t + 8)); q3 = _mm_loadu_si128((const __m128i *)(const void *)(t + 12));
        } else {
            const uint32_t *p = s + i;
            q0 = _mm_loadu_si128((const __m128i *)(const void *)p); q1 = _mm_loadu_si128((const __m128i *)(const void *)(p + 4));
            q2 = _mm_loadu_si128((const __m128i *)(const void *)(p + 8)); q3 = _mm_loadu_si128((const __m128i *)(const void *)(p + 12));
        }
        __m128i R, G, B, y0, y1;
        if (rgb) {                               /* R и B местами: в дорожках «R» лежит синий */
            shc_rgb32(q0, q1, &B, &G, &R); y0 = shc_y8(R, G, B);
            shc_rgb32(q2, q3, &B, &G, &R); y1 = shc_y8(R, G, B);
        } else {
            shc_rgb32(q0, q1, &R, &G, &B); y0 = shc_y8(R, G, B);
            shc_rgb32(q2, q3, &R, &G, &B); y1 = shc_y8(R, G, B);
        }
        _mm_storeu_si128((__m128i *)(void *)(y + i), _mm_packus_epi16(y0, y1));
        if (u) {                                 /* точки 0,2,4..14 */
            __m128i e0 = _mm_unpacklo_epi64(_mm_shuffle_epi32(q0, 0x08), _mm_shuffle_epi32(q1, 0x08));
            __m128i e1 = _mm_unpacklo_epi64(_mm_shuffle_epi32(q2, 0x08), _mm_shuffle_epi32(q3, 0x08));
            if (rgb) shc_rgb32(e0, e1, &B, &G, &R);
            else shc_rgb32(e0, e1, &R, &G, &B);
            shc_uv8(R, G, B, u + i / 2, v + i / 2);
        }
    }
#endif
    shc_row32_c(s, xm, i, n, y, u, v, rgb);
}
/* BGRX (как раньше: GDI, X11) */
static inline void shc_row32(const uint32_t *s, const int *xm, int i0, int n, uint8_t *y, uint8_t *u, uint8_t *v) {
    shc_row32x(s, xm, i0, n, y, u, v, 0);
}

/* ---------- блоки 16x16: отпечатки и возраст ---------- */
#define SHC_AGE_ALL 6                            /* все блоки не менялись столько закодированных кадров — кадр из одних
                                                  * пропусков (до того кодер доводит качество неподвижной картинки) */
typedef struct {
    int mbw, mbh;                                /* кадр в блоках */
    uint32_t *sig;                               /* отпечаток последнего снятого содержимого (0 — неизвестно) */
    uint8_t *age;                                /* закодированных кадров без изменений (насыщение 255) */
    uint8_t *map;                                /* для кодера: 1 — блок без изменений с прошлого закодированного кадра */
    int cx0, cy0, cx1, cy1;                      /* область изменений последнего захвата (блоки; пусто — cx0 >= cx1) */
} ShMb;
static inline uint32_t shc_fnv(uint32_t h, const uint8_t *p, int n) {   /* n кратно 4 */
    for (int k = 0; k < n; k += 4) { uint32_t w; memcpy(&w, p + k, 4); h = (h ^ w) * 16777619u; }
    return h;
}
static inline uint32_t shc_fnvb(uint32_t h, const uint8_t *p, int n) { for (int k = 0; k < n; k++) h = (h ^ p[k]) * 16777619u; return h; }
/* Отпечаток части блока, занятой картинкой: точки [x0,x1) x [y0,y1) кадра (Y, шаг ys) и их U/V (шаг cs). */
static inline uint32_t shc_mbhash(const uint8_t *Y, const uint8_t *U, const uint8_t *V, int ys, int cs, int x0, int x1, int y0, int y1) {
    uint32_t h[4] = { 2166136261u, 2166136261u ^ 0x9E3779B9u, 2166136261u ^ 0x7F4A7C15u, 2166136261u ^ 0x85EBCA6Bu };
    int w = x1 - x0, full = w == 16 && !(x0 & 3);
    for (int yy = y0; yy < y1; yy++) {
        const uint8_t *p = Y + (size_t)yy * ys + x0;
        h[yy & 3] = full ? shc_fnv(h[yy & 3], p, 16) : shc_fnvb(h[yy & 3], p, w);
    }
    int c0 = x0 / 2, cw = (x1 + 1) / 2 - c0;
    for (int yy = (y0 + 1) / 2; yy < (y1 + 1) / 2; yy++) {
        const uint8_t *pu = U + (size_t)yy * cs + c0, *pv = V + (size_t)yy * cs + c0;
        if (full) { h[yy & 3] = shc_fnv(h[yy & 3], pu, 8); h[(yy + 2) & 3] = shc_fnv(h[(yy + 2) & 3], pv, 8); }
        else { h[yy & 3] = shc_fnvb(h[yy & 3], pu, cw); h[(yy + 2) & 3] = shc_fnvb(h[(yy + 2) & 3], pv, cw); }
    }
    uint32_t r = h[0] ^ (h[1] << 8 | h[1] >> 24) ^ (h[2] << 16 | h[2] >> 16) ^ (h[3] << 24 | h[3] >> 8);
    return r ? r : 1;
}
static inline void shc_reset(ShMb *m) {          /* новый размер/место картинки: всё «неизвестно» */
    int n = m->mbw * m->mbh;
    memset(m->sig, 0, sizeof(uint32_t) * (size_t)n);
    memset(m->age, 0, (size_t)n);
}
static inline void shc_scan_begin(ShMb *m) { m->cx0 = m->cy0 = 1 << 20; m->cx1 = m->cy1 = -1; }
/* Ряд блоков mby, столбцы [mx0,mx1): отпечатки частей внутри картинки px0..px1 x py0..py1 (точки кадра). */
static inline int shc_hash_row(ShMb *m, const uint8_t *Y, const uint8_t *U, const uint8_t *V, int ys, int cs, int mby, int mx0, int mx1,
                               int px0, int px1, int py0, int py1) {
    int ch = 0, y0 = mby * 16 > py0 ? mby * 16 : py0, y1 = mby * 16 + 16 < py1 ? mby * 16 + 16 : py1;
    if (y0 >= y1) return 0;
    for (int mx = mx0; mx < mx1; mx++) {
        int x0 = mx * 16 > px0 ? mx * 16 : px0, x1 = mx * 16 + 16 < px1 ? mx * 16 + 16 : px1;
        if (x0 >= x1) continue;
        uint32_t h = shc_mbhash(Y, U, V, ys, cs, x0, x1, y0, y1);
        size_t k = (size_t)mby * m->mbw + mx;
        if (m->sig[k] != h) {
            m->sig[k] = h; m->age[k] = 0; ch++;
            if (mx < m->cx0) m->cx0 = mx;
            if (mx >= m->cx1) m->cx1 = mx + 1;
            if (mby < m->cy0) m->cy0 = mby;
            if (mby >= m->cy1) m->cy1 = mby + 1;
        }
    }
    return ch;
}
/* Перед кодированием P-кадра: карта блоков без изменений; true — всё стоит давно (кадр из одних пропусков). */
static inline bool shc_build_map(ShMb *m) {
    int n = m->mbw * m->mbh;
    bool all = true;
    for (int k = 0; k < n; k++) { m->map[k] = m->age[k] >= 1; all &= m->age[k] >= SHC_AGE_ALL; }
    return all;
}
static inline void shc_encoded(ShMb *m, bool key) {   /* кадр закодирован; после ключевого — снова кадры доводки */
    int n = m->mbw * m->mbh;
    for (int k = 0; k < n; k++) m->age[k] = key ? 1 : (uint8_t)(m->age[k] + (m->age[k] < 255));
}

/* ---------- темп кадров и захвата ---------- */
#define SHC_HI_MS 33.333                         /* заметное движение: 30 к/с */
#define SHC_LO_MS 50.0                           /* малое движение (набор текста, мигание курсора): 20 к/с */
#define SHC_STILL_MS 100.0                       /* картинка стоит > 1 с: такт 10 Гц (повтор кадра зрителям) */
typedef struct {
    double last_change, hi_until, last_full;
    bool force_full;                             /* следующий захват — целиком (старт, смена размера, пауза, ключевой) */
    int rx0, ry0, rx1, ry1;                      /* последняя непустая область изменений (блоки) */
} ShPace;
/* Захват на этом такте: 2 — целиком, 1 — только область изменений прошлого захвата (+1 блок вокруг), 0 — не нужен. */
static inline int shc_grab_kind(ShPace *p, ShMb *m, double now, bool key_due) {
    double still = now - p->last_change, per = still > 5000 ? 200.0 : 100.0;
    if (p->force_full || key_due || now - p->last_full >= per - 1.0) return 2;
    if (m->cx0 < m->cx1) { p->rx0 = m->cx0; p->ry0 = m->cy0; p->rx1 = m->cx1; p->ry1 = m->cy1; return 1; }
    /* такт без изменений посреди движения (источник отдал прежний кадр: PrintWindow, видео 25 к/с) — снимать
     * ту же область ещё секунду, иначе до полного захвата пауза до 100-200 мс и картинка у зрителей дёргается */
    if (still <= 1000 && p->rx0 < p->rx1) {
        m->cx0 = p->rx0; m->cy0 = p->ry0;
        m->cx1 = p->rx1 < m->mbw ? p->rx1 : m->mbw; m->cy1 = p->ry1 < m->mbh ? p->ry1 : m->mbh;
        if (m->cx0 < m->cx1 && m->cy0 < m->cy1) return 1;
        shc_scan_begin(m);
    }
    return 0;
}
/* После захвата: ch — изменившихся блоков, pic — блоков картинки. Вернуть длину такта (мс). */
static inline double shc_tick(ShPace *p, double now, int ch, int pic, bool pending) {
    if (ch) p->last_change = now;
    if (ch * 50 >= pic && ch >= 8) p->hi_until = now + 1000;   /* >= 2 % картинки: 30 к/с ещё секунду */
    if (now < p->hi_until) return SHC_HI_MS;
    if (pending || now - p->last_change <= 1000) return SHC_LO_MS;
    return SHC_STILL_MS;
}
#endif
