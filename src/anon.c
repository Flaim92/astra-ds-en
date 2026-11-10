/* anon.c — анонимизация своей камеры: скрытие глаз в реальном времени (договор — anon.h).
 *
 * Устройство (fail-closed):
 *   1. Каждый кадр: уменьшенная копия Y (ширина ~320, усреднение f x f) и признак изменений — блоки 8 x 8 против
 *      прошлого кадра. На копии — детектор лица pico (Nenad Markus, «Pixel Intensity Comparisons Organized in
 *      Decision Trees», MIT: third_party/pico; первые деревья — по таблице смещений на масштаб, тот же результат).
 *   2. Известное лицо: неподвижное и подтверждённое не позже 3 кадров / 120 мс назад — без детектора (прямоугольник
 *      ставится по последнему положению); иначе — поиск в окрестности (+-0,6 стороны, 5 масштабов), при потере —
 *      ещё под углами +-11 градусов. Новые лица: окна с центром в изменившихся блоках (грубая сетка) + полоса 1/10
 *      высоты мелкой сеткой (весь кадр за 10 кадров — для неподвижных); первый кадр после включения — весь кадр.
 *   3. Кадр уходит с закрытыми глазами (ANON_SEND), только если все лица подтверждены ANON_CONF раз подряд без
 *      скачков, лиц не больше ANON_MAXFACE, нет резкой смены освещения/сцены, подтверждённое лицо не пропадало
 *      посреди кадра последнюю секунду и нет движения вне зон известных лиц (лицо + плечи и руки ниже: там может
 *      входить в кадр лицо, которого детектор ещё не видит). Иначе — ANON_LOGO: буфер целиком перезаписан
 *      логотипом (ни одного пикселя камеры). Любое новое срабатывание (и слабое) — кандидат: закрывается сразу,
 *      кадры — логотип, пока его не подтвердит локальный поиск.
 *   4. Область глаз — с большим запасом от геометрии лица: вся полоса глаз квадрата лица с полями (на размеченном
 *      наборе во всех ушедших кадрах центры глаз лежат в строках -0,27..+0,08 и до +-0,42 стороны от центра
 *      квадрата, у обрезанного краем лица — до -0,57; полоса — -0,80..+0,30 и +-0,60), объединённая с областями 3 прошлых кадров и продлённая по скорости на кадр вперёд;
 *      при повороте — описанный прямоугольник. Пропавшее лицо закрывается «призраком» ещё ~2 с.
 *   5. Закрытие необратимое: огрублённый средний цвет области + шум + случайные символы своего шрифта 5x7
 *      (символы и сдвиг сетки меняются каждый кадр); исходные пиксели в области не остаются. Рисуется только в
 *      области; логотип — заливка + ~15 глифов (дешевле копирования готового кадра, без буфера на кадр).
 * Память: две уменьшенные копии Y (до 320 x 240), карты блоков, строка сумм, таблица смещений (~16 КБ) — до
 * ~180 КБ на контекст; кадр целиком не копируется. */
#ifdef ASTRA_VIDEO
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "anon.h"
#include "mc.h"
#include "plat.h"
#include "../third_party/pico/facefinder.h"
#include "anon_font.h"

/* ---------- настройки ---------- */
#define SMALL_W      320         /* ширина уменьшенной копии (не больше) */
/* параметры поиска (подобраны стендом --anontest, см. отчёт) */
static const float MIN_FACE = 20;      /* наименьшее лицо на уменьшенной копии, px (1280x720 -> 80 px) */
static const float SCALE_STEP = 1.15f; /* шаг масштабов поиска (стенд: 1.15 + 0.12 — меньше логотипов и вдвое быстрее, чем 1.1 + 0.1) */
static const float STRIDE = 0.12f;     /* шаг окна относительно размера */
static const float FINE_STEP = 1.1f, FINE_STRIDE = 0.1f;   /* мелкая сетка: полосы и весь кадр (неподвижные лица) */
static const float Q_TRACK = 5.0f;     /* уверенность (сумма q по кластеру) полного поиска — заводит трек */
static const float Q_LOCAL = 3.0f;     /* уверенность локального поиска (меньше окон в кластере) — подтверждает трек */
#define Q_MASK       1.0f        /* слабее — не лицо; от Q_MASK до порога — закрыть, но кадр не считать надёжным */
#define MAXDET       1024        /* сырых срабатываний за проход */
#define ANON_MAXFACE 4           /* больше лиц — логотип */
#define ANON_CONF    3           /* подтверждений подряд для уверенного трека */
#define CONFIRM_N    3           /* неподвижное лицо подтверждается не реже чем раз в 3 кадра ... */
#define CONFIRM_US   120000      /* ... и не реже чем раз в 120 мс; иначе — поиск (не удалось — логотип) */
#define STRIPES      10          /* весь кадр просматривается полосами: 1/10 высоты за кадр (весь — за 10 кадров) */
#define MOVE_HOLD    4           /* движение вне зон известных лиц: столько кадров — логотип */
#define MOVE_MIN     2           /* ... от стольких изменившихся блоков (8x8 уменьшенной копии) */
#define HIST         3           /* объединение с областями последних кадров */
#define LOST_HOLD    30          /* трек пропал посреди кадра: столько кадров — логотип */
#define GHOST        60          /* ... и столько кадров закрывается его последняя область */
#define BLK          8           /* блок признака изменений на уменьшенной копии (8 px = 32 px при 1280) */
#define CHG_SAD      6           /* средняя разность пикселя блока с прошлым кадром — блок «изменился» */
#define LIGHT_DY     14          /* смена средней яркости кадра за кадр (0..255) — «смена освещения» */
#define SCENE_FRAC   60          /* изменилось больше 60 % блоков — рывок камеры/сцены */
#define BUDGET_US    60000       /* кадр дольше 60 мс — ANON_DROP (он и так опоздал) */
/* полоса глаз относительно квадрата лица pico (центр r,c, сторона s): строки r+EY0*s .. r+EY1*s,
 * столбцы c-EX*s .. c+EX*s (в системе лица; при повороте — описанный прямоугольник). Калибровка — стенд
 * --anontest (см. отчёт): с запасом к худшему случаю. */
static const float EY0 = -0.80f, EY1 = 0.30f, EX = 0.60f;   /* верх с запасом: у лица, обрезанного краем кадра, квадрат pico съезжает вниз */

typedef struct { int x0, y0, x1, y1; } Rect;   /* полуинтервалы, полное разрешение */
typedef struct {
    float r, c, s, q;            /* уменьшенная копия */
    int a;                       /* поворот в плоскости: 0, +-1, +-2 шага по 1/32 оборота (11,25 град) */
    float vr, vc;                /* скорость центра, px полного разрешения за кадр */
    int streak;                  /* подтверждений подряд */
    bool confirmed;              /* хоть раз был уверенным */
    bool strong;                 /* хоть раз найден с уверенностью полного поиска (Q_TRACK) — это «лицо» */
    int since;                   /* кадров с последнего подтверждения */
    long long t_conf;            /* время последнего подтверждения, мкс */
    bool hit;
    Rect hist[HIST];
    int nh;
} Track;
/* «призрак» пропавшего лица: его область ещё закрывается; hold — подтверждённое лицо пропало посреди кадра без
 * движения в его области (скорее всего, оно на месте, детектор его просто не видит): логотип, пока лицо не
 * найдётся снова или его область не сдвинется и снова не успокоится */
typedef struct { Rect r; int left; bool hold, moved; int calm; float gr, gc, gs; } Ghost;
enum { M_REUSE, M_LOCAL, M_PART, M_FULL, M_N };   /* самый дорогой проход кадра (для замеров) */

struct AnonCtx {
    int w, h, f, sw, sh, bw, bh;
    bool on;
    unsigned gen;
    uint8_t *small, *prev;       /* sw x sh: текущая и прошлая уменьшенные копии */
    uint8_t *chg, *chg2;         /* bw x bh: изменившиеся блоки и они же, расширенные на 2 блока */
    uint32_t *acc;               /* w сумм строк */
    float det[MAXDET * 4];
    int fast[32][63][2];         /* смещения сравнений первых деревьев для текущего масштаба (run_fast) */
    int clab[MAXDET], cstack[MAXDET];
    Track tr[ANON_MAXFACE];
    int ntr;
    Ghost gh[ANON_MAXFACE];
    int lost_hold, unstable, stripe, moving;
    bool have_prev;
    int state, faces;
    uint64_t rng;
    Rect last[ANON_MAXFACE * 4];  /* закрытые прямоугольники последнего кадра (стенд, selftest) */
    int nlast;
    long long us_last, us_down, us_det;
    int mode;                     /* M_* последнего кадра */
};

static atomic_bool g_def = ANON_DEFAULT_ON;
static atomic_uint g_gen;
static atomic_int g_status;

/* ---------- случайные числа (xorshift64*, не криптостойкий: только шум и символы) ---------- */
static uint64_t rnd(AnonCtx *c) {
    uint64_t x = c->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    c->rng = x;
    return x * 2685821657736338717ull;
}
static void rnd_mix(AnonCtx *c, uint64_t v) {
    c->rng ^= v * 0x9E3779B97F4A7C15ull;
    if (!c->rng) c->rng = 0x853c49e6748fea9bull;
    rnd(c);
}

/* ---------- детектор pico (переписан по rnt/picornt.c, MIT; каскад — константные массивы) ---------- */
static int run_cascade(float *o, int r, int c, int s, const uint8_t *px, int nr, int nc, int ld) {
    r *= 256; c *= 256;
    if ((r + 128 * s) / 256 >= nr || (r - 128 * s) / 256 < 0 || (c + 128 * s) / 256 >= nc || (c - 128 * s) / 256 < 0) return -1;
    float acc = 0;
    for (int i = 0; i < PICO_NTREES; i++) {
        const int8_t *t0 = PICO_TC[i];
        int idx = 1;
        for (int j = 0; j < PICO_TDEPTH; j++) {
            const int8_t *t = t0 + 4 * (idx - 1);
            idx = 2 * idx + (px[(r + t[0] * s) / 256 * ld + (c + t[1] * s) / 256] <= px[(r + t[2] * s) / 256 * ld + (c + t[3] * s) / 256]);
        }
        acc += PICO_LUT[i][idx - (1 << PICO_TDEPTH)];
        if (acc <= PICO_THR[i]) return -1;
    }
    *o = acc - PICO_THR[PICO_NTREES - 1];
    return 1;
}
/* поворот в плоскости (run_rotated_cascade из pico): ai — 1/32 оборота, таблицы cos/sin * 256 */
static const int QCOS[33] = { 256, 251, 236, 212, 181, 142, 97, 49, 0, -49, -97, -142, -181, -212, -236, -251, -256, -251, -236, -212, -181, -142, -97, -49, 0, 49, 97, 142, 181, 212, 236, 251, 256 };
static const int QSIN[33] = { 0, 49, 97, 142, 181, 212, 236, 251, 256, 251, 236, 212, 181, 142, 97, 49, 0, -49, -97, -142, -181, -212, -236, -251, -256, -251, -236, -212, -181, -142, -97, -49, 0 };
static int run_rot(float *o, int r, int c, int s, int ai, const uint8_t *px, int nr, int nc, int ld) {
    int qs = s * QSIN[ai], qc = s * QCOS[ai];
    r *= 65536; c *= 65536;
    if ((r + 46341 * s) / 65536 >= nr || (r - 46341 * s) / 65536 < 0 || (c + 46341 * s) / 65536 >= nc || (c - 46341 * s) / 65536 < 0) return -1;
    float acc = 0;
    for (int i = 0; i < PICO_NTREES; i++) {
        const int8_t *t0 = PICO_TC[i];
        int idx = 1;
        for (int j = 0; j < PICO_TDEPTH; j++) {
            const int8_t *t = t0 + 4 * (idx - 1);
            int r1 = (r + qc * t[0] - qs * t[1]) / 65536, c1 = (c + qs * t[0] + qc * t[1]) / 65536;
            int r2 = (r + qc * t[2] - qs * t[3]) / 65536, c2 = (c + qs * t[2] + qc * t[3]) / 65536;
            idx = 2 * idx + (px[r1 * ld + c1] <= px[r2 * ld + c2]);
        }
        acc += PICO_LUT[i][idx - (1 << PICO_TDEPTH)];
        if (acc <= PICO_THR[i]) return -1;
    }
    *o = acc - PICO_THR[PICO_NTREES - 1];
    return 1;
}
/* окна по сетке pico в области [r0,r1] x [c0,c1] уменьшенной копии, масштабы smin..smax, поворот a (шаги
 * по 1/32), mask — только окна с центром в отмеченном блоке (NULL — все). Возврат — число срабатываний;
 * maxn — переполнение (вызывающий уходит в логотип). */
/* быстрый путь без поворота: (256r + t*s)/256 == r + ((t*s) >> 8) (числитель неотрицателен — проверка границ),
 * значит, смещения пикселей сравнения зависят только от масштаба — таблица на первые FAST_T деревьев (почти все
 * окна отсеиваются на них); дальше — обычный расчёт. Результат тот же, что у run_cascade. */
#define FAST_T 32
static int run_fast(float *o, int r, int c, int s, const uint8_t *px, int nr, int nc, int ld, const int (*off)[63][2]) {
    if ((c * 256 + 128 * s) / 256 >= nc || (c * 256 - 128 * s) / 256 < 0 || (r * 256 + 128 * s) / 256 >= nr || (r * 256 - 128 * s) / 256 < 0) return -1;
    const uint8_t *base = px + (size_t)r * ld + c;
    float acc = 0;
    for (int i = 0; i < FAST_T; i++) {
        int idx = 1;
        for (int j = 0; j < PICO_TDEPTH; j++) {
            const int *q = off[i][idx - 1];
            idx = 2 * idx + (base[q[0]] <= base[q[1]]);
        }
        acc += PICO_LUT[i][idx - (1 << PICO_TDEPTH)];
        if (acc <= PICO_THR[i]) return -1;
    }
    int R = r * 256, C = c * 256;
    for (int i = FAST_T; i < PICO_NTREES; i++) {
        const int8_t *t0 = PICO_TC[i];
        int idx = 1;
        for (int j = 0; j < PICO_TDEPTH; j++) {
            const int8_t *t = t0 + 4 * (idx - 1);
            idx = 2 * idx + (px[(R + t[0] * s) / 256 * ld + (C + t[1] * s) / 256] <= px[(R + t[2] * s) / 256 * ld + (C + t[3] * s) / 256]);
        }
        acc += PICO_LUT[i][idx - (1 << PICO_TDEPTH)];
        if (acc <= PICO_THR[i]) return -1;
    }
    *o = acc - PICO_THR[PICO_NTREES - 1];
    return 1;
}
static void fast_table(int (*off)[63][2], int s, int ld) {
    for (int i = 0; i < FAST_T; i++)
        for (int k = 0; k < 63; k++) {
            const int8_t *t = PICO_TC[i] + 4 * k;
            off[i][k][0] = ((t[0] * s) >> 8) * ld + ((t[1] * s) >> 8);
            off[i][k][1] = ((t[2] * s) >> 8) * ld + ((t[3] * s) >> 8);
        }
}
static int scan(AnonCtx *x, float *out, int maxn, float r0, float r1, float c0, float c1, float smin, float smax, int a, const uint8_t *mask, float step, float stride) {
    int n = 0, nr = x->sh, nc = x->sw, ai = (a % 32 + 32) % 32;
    for (float s = smin; s <= smax * 1.0001f; s *= step) {
        float d = stride * s > 1.0f ? stride * s : 1.0f;
        if (!ai) fast_table(x->fast, (int)s, nc);
        float ra = fmaxf(r0, s / 2 + 1), rb = fminf(r1, nr - s / 2 - 1);
        float ca = fmaxf(c0, s / 2 + 1), cb = fminf(c1, nc - s / 2 - 1);
        for (float r = ra; r <= rb; r += d)
            for (float c = ca; c <= cb; c += d) {
                if (mask && !mask[((int)r / BLK) * x->bw + (int)c / BLK]) continue;
                float q;
                int t = ai ? run_rot(&q, (int)r, (int)c, (int)s, ai, x->small, nr, nc, nc) : run_fast(&q, (int)r, (int)c, (int)s, x->small, nr, nc, nc, x->fast);
                if (t != 1) continue;
                if (n >= maxn) return maxn;
                out[4 * n] = r; out[4 * n + 1] = c; out[4 * n + 2] = s; out[4 * n + 3] = q;
                n++;
            }
    }
    return n;
}
static float overlap(const float *a, const float *b) {
    float h1 = a[2] / 2, h2 = b[2] / 2;
    float orr = fminf(a[0] + h1, b[0] + h2) - fmaxf(a[0] - h1, b[0] - h2);
    float oc = fminf(a[1] + h1, b[1] + h2) - fmaxf(a[1] - h1, b[1] - h2);
    if (orr <= 0 || oc <= 0) return 0;
    return orr * oc / (a[2] * a[2] + b[2] * b[2] - orr * oc);
}
/* кластеры по перекрытию > 0.3 (как cluster_detections в pico, без рекурсии): среднее положение, сумма q */
static int cluster(AnonCtx *x, int n) {
    float *d = x->det;
    int *lab = x->clab, *st = x->cstack, ncc = 0;
    for (int i = 0; i < n; i++) lab[i] = 0;
    for (int i = 0; i < n; i++) {
        if (lab[i]) continue;
        lab[i] = ++ncc;
        int sp = 0;
        st[sp++] = i;
        while (sp) {
            int k = st[--sp];
            for (int j = 0; j < n; j++)
                if (!lab[j] && overlap(d + 4 * k, d + 4 * j) > 0.3f) { lab[j] = ncc; st[sp++] = j; }
        }
    }
    /* результат — в начало d (кластер cc пишется в позицию cc-1 <= первого его элемента: сначала суммы) */
    float acc[64 * 4];
    int cnt[64];
    int m = ncc < 64 ? ncc : 64;
    memset(acc, 0, sizeof acc);
    memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < n; i++) {
        int k = lab[i] - 1;
        if (k >= m) continue;
        acc[4 * k] += d[4 * i]; acc[4 * k + 1] += d[4 * i + 1]; acc[4 * k + 2] += d[4 * i + 2]; acc[4 * k + 3] += d[4 * i + 3];
        cnt[k]++;
    }
    for (int k = 0; k < m; k++) {
        d[4 * k] = acc[4 * k] / (float)cnt[k];
        d[4 * k + 1] = acc[4 * k + 1] / (float)cnt[k];
        d[4 * k + 2] = acc[4 * k + 2] / (float)cnt[k];
        d[4 * k + 3] = acc[4 * k + 3];
    }
    return ncc;   /* > 64 — слишком много: вызывающий уходит в логотип */
}

/* ---------- уменьшенная копия Y и миниатюра ---------- */
static void downscale(AnonCtx *c, const uint8_t *y, int ys) {
    int f = c->f, w = c->w, sw = c->sw;
    uint32_t mul = (uint32_t)(65536 / (f * f));
    for (int sy = 0; sy < c->sh; sy++) {
        uint32_t *a = c->acc;
        const uint8_t *p = y + (size_t)sy * f * ys;
        for (int i = 0; i < sw * f; i++) a[i] = p[i];
        for (int k = 1; k < f; k++) {
            p += ys;
            for (int i = 0; i < sw * f; i++) a[i] += p[i];
        }
        uint8_t *o = c->small + (size_t)sy * sw;
        for (int sx = 0; sx < sw; sx++) {
            uint32_t s = 0;
            for (int k = 0; k < f; k++) s += a[sx * f + k];
            uint32_t v = (s * mul + 32768) >> 16;
            o[sx] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    (void)w;
}
/* признак изменений: блоки BLK x BLK уменьшенной копии против прошлого кадра (средняя разность пикселя).
 * chg — изменившиеся блоки, chg2 — они же, расширенные на 2 блока (окно лица с центром рядом). *light — сдвиг
 * средней яркости кадра, *scene — изменилась большая часть кадра. Первый кадр — «изменилось всё». */
static int blocks(AnonCtx *c, bool *light, bool *scene) {
    int bw = c->bw, bh = c->bh, nb = bw * bh, nch = 0;
    *light = *scene = false;
    if (!c->have_prev) {
        memset(c->chg, 1, (size_t)nb);
        memset(c->chg2, 1, (size_t)nb);
        return nb;
    }
    long long sdiff = 0;
    for (int by = 0; by < bh; by++) {
        int y0 = by * BLK, y1 = y0 + BLK < c->sh ? y0 + BLK : c->sh;
        for (int bx = 0; bx < bw; bx++) {
            int x0 = bx * BLK, x1 = x0 + BLK < c->sw ? x0 + BLK : c->sw;
            int sad = 0, sd = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *a = c->small + (size_t)yy * c->sw, *b = c->prev + (size_t)yy * c->sw;
                for (int xx = x0; xx < x1; xx++) { int d = (int)a[xx] - (int)b[xx]; sd += d; sad += d < 0 ? -d : d; }
            }
            int npx = (y1 - y0) * (x1 - x0);
            bool ch = npx && sad > CHG_SAD * npx;
            c->chg[by * bw + bx] = ch;
            nch += ch;
            sdiff += sd;
        }
    }
    long long npix = (long long)c->sw * c->sh;
    *light = llabs(sdiff) > (long long)LIGHT_DY * npix;
    *scene = nch * 100 > SCENE_FRAC * nb;
    memset(c->chg2, 0, (size_t)nb);
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            if (!c->chg[by * bw + bx]) continue;
            for (int yy = by - 2; yy <= by + 2; yy++)
                for (int xx = bx - 2; xx <= bx + 2; xx++)
                    if (yy >= 0 && yy < bh && xx >= 0 && xx < bw) c->chg2[yy * bw + xx] = 1;
        }
    return nch;
}
/* есть ли изменившийся блок в квадрате (r, c, половина m) уменьшенной копии */
static bool area_changed(const AnonCtx *c, float r, float cc, float m) {
    int by0 = (int)((r - m) / BLK), by1 = (int)((r + m) / BLK), bx0 = (int)((cc - m) / BLK), bx1 = (int)((cc + m) / BLK);
    if (by0 < 0) by0 = 0;
    if (bx0 < 0) bx0 = 0;
    if (by1 >= c->bh) by1 = c->bh - 1;
    if (bx1 >= c->bw) bx1 = c->bw - 1;
    for (int by = by0; by <= by1; by++)
        for (int bx = bx0; bx <= bx1; bx++)
            if (c->chg[by * c->bw + bx]) return true;
    return false;
}

/* ---------- рисование в I420 ---------- */
static int glyph_of(unsigned cp) {
    if (cp >= 'a' && cp <= 'z') cp -= 32;
    if (cp >= 0x20 && cp < 0x61) return (int)(cp - 0x20);
    if (cp >= 0x7b && cp <= 0x7e) return (int)(cp - 0x20 - 26);
    if (cp >= 0x430 && cp <= 0x44f) cp -= 0x20;
    if (cp >= 0x410 && cp <= 0x42f) return AF_NASC + (int)(cp - 0x410);
    if (cp == 0x401 || cp == 0x451) return AF_NASC + 32;
    return '?' - 0x20;
}
/* глиф g масштаба sc в плоскость p (шаг ps, размеры pw x ph) в точку (x, y): пиксели «1» = val */
static void draw_glyph(uint8_t *p, int ps, int pw, int ph, int x, int y, int sc, int g, uint8_t val) {
    if (sc < 1) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = AF_GLYPH[g][row];
        if (!bits) continue;
        int y0 = y + row * sc, y1 = y0 + sc;
        if (y0 < 0) y0 = 0;
        if (y1 > ph) y1 = ph;
        for (int col = 0; col < 5; col++) {
            if (!(bits & (0x10 >> col))) continue;
            int x0 = x + col * sc, x1 = x0 + sc;
            if (x0 < 0) x0 = 0;
            if (x1 > pw) x1 = pw;
            for (int yy = y0; yy < y1; yy++)
                if (x1 > x0) memset(p + (size_t)yy * ps + x0, val, (size_t)(x1 - x0));
        }
    }
}
static void fill_plane(uint8_t *p, int ps, int w, int h, uint8_t v) {
    for (int j = 0; j < h; j++) memset(p + (size_t)j * ps, v, (size_t)w);
}
typedef struct { uint8_t *y, *u, *v; int ys, cs, w, h; } Pic;
/* строка (UTF-8) крупно: глиф sc в Y, в U/V (если заданы) — sc/2 тем же цветом */
static void draw_text(Pic *P, const char *s, int x, int y, int sc, uint8_t cy, int cu, int cv) {
    const unsigned char *q = (const unsigned char *)s;
    while (*q) {
        unsigned cp = *q++;
        if (cp >= 0xc0 && (*q & 0xc0) == 0x80) {
            if (cp < 0xe0) cp = ((cp & 0x1f) << 6) | (*q++ & 0x3f);
            else { cp = 0xfffd; while ((*q & 0xc0) == 0x80) q++; }
        } else if (cp >= 0x80) cp = '?';
        int g = glyph_of(cp);
        draw_glyph(P->y, P->ys, P->w, P->h, x, y, sc, g, cy);
        if (cu >= 0 && sc >= 2) {
            draw_glyph(P->u, P->cs, P->w / 2, P->h / 2, x / 2, y / 2, sc / 2, g, (uint8_t)cu);
            draw_glyph(P->v, P->cs, P->w / 2, P->h / 2, x / 2, y / 2, sc / 2, g, (uint8_t)cv);
        }
        x += 6 * sc;
    }
}
static int utf8_len(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

/* логотип Astra DS (кошка из левого верхнего угла интерфейса) на тёмном фоне — без единого пикселя камеры */
static void draw_logo(Pic *P) {
    fill_plane(P->y, P->ys, P->w, P->h, 20);
    fill_plane(P->u, P->cs, P->w / 2, P->h / 2, 128);
    fill_plane(P->v, P->cs, P->w / 2, P->h / 2, 128);
    static const char *const cat[3] = { "/\\_/\\", "(=.=)", " >^< " };
    int sc = P->w * 55 / 100 / 30, sc2 = P->h * 50 / 100 / 24;
    if (sc2 < sc) sc = sc2;
    sc &= ~1;
    if (sc < 2) sc = 2;
    int bw = 30 * sc - sc, bh = 3 * 8 * sc - sc;
    int x = (P->w - bw) / 2 & ~1, y = (P->h - bh) / 2 - P->h / 20;
    y &= ~1;
    for (int r = 0; r < 3; r++) draw_text(P, cat[r], x, y + r * 8 * sc, sc, 150, 182, 118);   /* акцент #5865f2 светлее */
    int ts = sc / 4 < 1 ? 1 : sc / 4;
    const char *nm = "ASTRA DS";
    draw_text(P, nm, (P->w - (8 * 6 * ts - ts)) / 2 & ~1, (y + bh + 2 * sc) & ~1, ts, 120, -1, -1);
}

/* предупреждение для своего предпросмотра — в стиле логотипа: зачёркнутое лицо из символов шрифта 5x7 и
 * надпись тем же шрифтом, палитра кота-логотипа; без единого пикселя камеры */
static void draw_warning(Pic *P) {
    int w = P->w, h = P->h;
    fill_plane(P->y, P->ys, w, h, 20);
    fill_plane(P->u, P->cs, w / 2, h / 2, 128);
    fill_plane(P->v, P->cs, w / 2, h / 2, 128);
    /* лицо 7 x 5 клеток (как кот 5 x 3); крест поверх — по диагоналям: \ сверху слева, / сверху справа, X в центре */
    static const char *const face[5] = {
        " .---. ",
        "| o o |",
        "|     |",
        "| \\_/ |",
        " '---' ",
    };
    enum { FW = 7, FH = 5 };
    int sc = w * 50 / 100 / (FW * 6), sc2 = h * 50 / 100 / (FH * 8);
    if (sc2 < sc) sc = sc2;
    sc &= ~1;
    if (sc < 2) sc = 2;
    int bw = FW * 6 * sc - sc, bh = FH * 8 * sc - sc;
    int x0 = (w - bw) / 2 & ~1, y0 = (h - bh) / 2 - h / 10;
    if (y0 < 0) y0 = 0;
    y0 &= ~1;
    for (int r = 0; r < FH; r++) {
        static const int dg[FH] = { 0, 1, 3, 5, 6 };            /* столбец диагонали в строке r (симметрично) */
        char cross[FW + 1];
        memset(cross, ' ', FW);
        cross[FW] = 0;
        if (dg[r] == FW - 1 - dg[r]) cross[dg[r]] = 'X';
        else { cross[dg[r]] = '\\'; cross[FW - 1 - dg[r]] = '/'; }
        draw_text(P, face[r], x0, y0 + r * 8 * sc, sc, 105, 156, 124);   /* лицо — приглушённый сиреневый */
        draw_text(P, cross, x0, y0 + r * 8 * sc, sc, 150, 182, 118);     /* крест поверх — сиреневый логотипа */
    }
    /* надпись одной строкой тем же шрифтом (как «ASTRA DS» под логотипом), не шире кадра */
    const char *msg = ui_tr("Лицо не найдено");
    int n = utf8_len(msg), ts = sc / 3 < 2 ? 2 : sc / 3;
    while (ts > 1 && n * 6 * ts - ts > w - 8) ts--;
    int lw = n * 6 * ts - ts, ly = y0 + bh + 2 * sc;
    if (ly + 7 * ts <= h) draw_text(P, msg, (w - lw) / 2 > 0 ? (w - lw) / 2 & ~1 : 0, ly & ~1, ts, 150, ts >= 2 ? 182 : -1, 118);
}

/* закрытие прямоугольника: средний цвет + шум + случайные символы (необратимо) */
static void mask_rect(AnonCtx *c, Pic *P, Rect r) {
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    /* средний цвет по всем исходным пикселям области (не зависит от их расположения) */
    unsigned long long sy = 0, su = 0, sv = 0, ny = 0, nc = 0;
    for (int yy = r.y0; yy < r.y1; yy++) {
        const uint8_t *p = P->y + (size_t)yy * P->ys;
        for (int xx = r.x0; xx < r.x1; xx++) sy += p[xx];
        ny += (unsigned)(r.x1 - r.x0);
    }
    for (int yy = r.y0 / 2; yy < r.y1 / 2; yy++) {
        const uint8_t *pu = P->u + (size_t)yy * P->cs, *pv = P->v + (size_t)yy * P->cs;
        for (int xx = r.x0 / 2; xx < r.x1 / 2; xx++) { su += pu[xx]; sv += pv[xx]; }
        nc += (unsigned)(r.x1 / 2 - r.x0 / 2);
    }
    int my = ny ? (int)(sy / ny) : 128, mu = nc ? (int)(su / nc) : 128, mv = nc ? (int)(sv / nc) : 128;
    /* грубое квантование среднего — меньше сведений об исходнике */
    my = (my & ~7) + 4; mu = (mu & ~7) + 4; mv = (mv & ~7) + 4;
    if (my < 40) my = 40;
    if (my > 215) my = 215;
    for (int yy = r.y0; yy < r.y1; yy++) {
        uint8_t *py = P->y + (size_t)yy * P->ys;
        int xx = r.x0;
        while (xx < r.x1) {
            uint64_t b = rnd(c);
            for (int k = 0; k < 8 && xx < r.x1; k++, xx++, b >>= 8) py[xx] = (uint8_t)(my - 24 + (int)((b & 0xff) * 49 >> 8));
        }
    }
    for (int yy = r.y0 / 2; yy < r.y1 / 2; yy++) {
        memset(P->u + (size_t)yy * P->cs + r.x0 / 2, mu, (size_t)(r.x1 / 2 - r.x0 / 2));
        memset(P->v + (size_t)yy * P->cs + r.x0 / 2, mv, (size_t)(r.x1 / 2 - r.x0 / 2));
    }
    /* символы: сетка со случайным сдвигом, случайный символ в клетке (иногда пусто) */
    int rh = r.y1 - r.y0, rw = r.x1 - r.x0;
    int sc = rh / 20;
    if (sc < 1) sc = 1;
    if (sc * 6 > rw) sc = rw / 6 > 0 ? rw / 6 : 1;
    int cw = 6 * sc, ch = 8 * sc;
    uint8_t col = (uint8_t)(my < 128 ? my + 90 : my - 90);
    int ox = (int)(rnd(c) % (unsigned)cw), oy = (int)(rnd(c) % (unsigned)ch);
    Pic sub = { P->y + (size_t)r.y0 * P->ys + r.x0, NULL, NULL, P->ys, 0, rw, rh };
    for (int gy = -oy; gy < rh; gy += ch)
        for (int gx = -ox; gx < rw; gx += cw) {
            uint64_t b = rnd(c);
            if ((b & 7) < 2) continue;
            int g = 1 + (int)((b >> 8) % (AF_NASC - 1));
            draw_glyph(sub.y, sub.ys, sub.w, sub.h, gx + (int)((b >> 20) % (unsigned)sc), gy + (int)((b >> 28) % (unsigned)sc), sc, g, col);
        }
}

/* полоса глаз трека в полном разрешении (с поворотом — описанный прямоугольник) */
static Rect clip_even(const AnonCtx *c, float x0, float y0, float x1, float y1) {
    Rect r = { (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1) };
    r.x0 &= ~1; r.y0 &= ~1; r.x1 = (r.x1 + 1) & ~1; r.y1 = (r.y1 + 1) & ~1;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > c->w) r.x1 = c->w;
    if (r.y1 > c->h) r.y1 = c->h;
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}
static Rect eye_band(const AnonCtx *c, float r, float cc, float s, int a) {
    float f = (float)c->f, off = (f - 1) / 2;
    float R = r * f + off, C = cc * f + off, Sz = s * f;
    if (!a) return clip_even(c, C - EX * Sz, R + EY0 * Sz, C + EX * Sz, R + EY1 * Sz);
    /* как в run_rot: смещение (dr, dc) в системе лица -> (cos*dr - sin*dc, sin*dr + cos*dc) */
    float th = (float)a * 6.2831853f / 32, cs = cosf(th), sn = sinf(th);
    float rr[4] = { EY0, EY0, EY1, EY1 }, ccs[4] = { -EX, EX, -EX, EX };
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float dr = rr[k] * Sz, dc = ccs[k] * Sz;
        float py = R + cs * dr - sn * dc, px = C + sn * dr + cs * dc;
        x0 = fminf(x0, px); x1 = fmaxf(x1, px); y0 = fminf(y0, py); y1 = fmaxf(y1, py);
    }
    return clip_even(c, x0, y0, x1, y1);
}
static Rect runion(Rect a, Rect b) {
    if (a.x1 <= a.x0) return b;
    if (b.x1 <= b.x0) return a;
    Rect r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}

static bool args_ok(const AnonCtx *c, const uint8_t *y, const uint8_t *u, const uint8_t *v, int ys, int cs, int w, int h) {
    return c && y && u && v && w == c->w && h == c->h && ys >= w && cs >= w / 2;
}
static void reset_track(AnonCtx *c) {
    c->ntr = 0;
    c->lost_hold = 0;
    c->unstable = ANON_CONF;
    c->have_prev = false;
    c->stripe = 0;
    c->moving = 0;
    memset(c->gh, 0, sizeof c->gh);
}

/* ---------- API ---------- */
AnonCtx *anon_new(int w, int h) {
    if (w < 32 || h < 32 || w > 4096 || h > 4096 || (w & 1) || (h & 1)) return NULL;
    AnonCtx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->w = w; c->h = h;
    c->f = (w + SMALL_W - 1) / SMALL_W;
    if (c->f < 1) c->f = 1;
    c->sw = w / c->f; c->sh = h / c->f;
    c->bw = (c->sw + BLK - 1) / BLK; c->bh = (c->sh + BLK - 1) / BLK;
    c->small = malloc((size_t)c->sw * c->sh);
    c->prev = malloc((size_t)c->sw * c->sh);
    c->chg = malloc((size_t)c->bw * c->bh);
    c->chg2 = malloc((size_t)c->bw * c->bh);
    c->acc = malloc(sizeof(uint32_t) * (size_t)w);
    if (!c->small || !c->prev || !c->chg || !c->chg2 || !c->acc) { anon_free(c); return NULL; }
    c->gen = atomic_load(&g_gen);
    c->on = atomic_load(&g_def);
    c->rng = 0x2545F4914F6CDD1Dull;
    rnd_mix(c, (uint64_t)plat_now_us() ^ (uint64_t)(uintptr_t)c);
    reset_track(c);
    c->state = c->on ? ANON_NOFACE : ANON_OFF;
    return c;
}
void anon_free(AnonCtx *c) {
    if (!c) return;
    free(c->small); free(c->prev); free(c->chg); free(c->chg2); free(c->acc);
    free(c);
}
void anon_enable(AnonCtx *c, bool on) {
    if (!c) return;
    if (on && !c->on) reset_track(c);
    c->on = on;
}
bool anon_enabled(const AnonCtx *c) { return c && c->on; }
int anon_state(const AnonCtx *c) { return c ? c->state : ANON_OFF; }
int anon_status(void) { return atomic_load(&g_status); }
int anon_faces(const AnonCtx *c) { return c ? c->faces : 0; }
void anon_set_default(bool on) { atomic_store(&g_def, on); atomic_fetch_add(&g_gen, 1); }
bool anon_default(void) { return atomic_load(&g_def); }
bool anon_small_size(const AnonCtx *c, int *sw, int *sh, int *f) {
    if (!c) return false;
    if (sw) *sw = c->sw;
    if (sh) *sh = c->sh;
    if (f) *f = c->f;
    return true;
}

bool anon_preview_warning(AnonCtx *c, uint8_t *y, uint8_t *u, uint8_t *v, int ys, int cs, int w, int h) {
    if (!args_ok(c, y, u, v, ys, cs, w, h)) return false;
    Pic P = { y, u, v, ys, cs, w, h };
    draw_warning(&P);
    return true;
}

static int finish(AnonCtx *c, int st, long long t0, int ret) {
    c->state = st;
    atomic_store(&g_status, st);
    c->us_last = plat_now_us() - t0;
    if (c->us_last > BUDGET_US) return ANON_DROP;
    return ret;
}

/* локальный поиск около трека с поворотом a: лучший кластер рядом (d — r, c, s, q). false — ничего */
static bool local_at(AnonCtx *c, const Track *t, int a, float d[4]) {
    float f = (float)c->f;
    float m = 0.6f * t->s + (fabsf(t->vr) + fabsf(t->vc)) / f;
    float s2 = SCALE_STEP * SCALE_STEP;
    int n = scan(c, c->det, MAXDET, t->r - m, t->r + m, t->c - m, t->c + m, t->s / s2, t->s * s2, a, NULL, SCALE_STEP, STRIDE);
    if (n <= 0 || n >= MAXDET) return false;
    int ncl = cluster(c, n);
    if (ncl > 64) ncl = 64;
    int best = -1;
    for (int k = 0; k < ncl; k++) {
        float *e = c->det + 4 * k, dr = e[0] - t->r, dc = e[1] - t->c;
        if (sqrtf(dr * dr + dc * dc) > 0.6f * t->s) continue;
        if (best < 0 || e[3] > c->det[4 * best + 3]) best = k;
    }
    if (best < 0) return false;
    memcpy(d, c->det + 4 * best, 4 * sizeof(float));
    return true;
}
/* подтверждение трека найденным положением (q — сила, strong — выше порога) */
static void track_update(AnonCtx *c, Track *t, const float d[4], int a, bool strong, long long now) {
    float f = (float)c->f, ratio = d[2] / t->s;
    float dr = d[0] - t->r, dc = d[1] - t->c;
    bool jump = sqrtf(dr * dr + dc * dc) > 0.25f * t->s || ratio > 1.2f || ratio < 1 / 1.2f;   /* резкое движение */
    int k = t->since + 1;
    t->vr = dr * f / (float)k; t->vc = dc * f / (float)k;
    t->r = d[0]; t->c = d[1]; t->s = d[2]; t->q = d[3]; t->a = a;
    if (d[3] >= Q_TRACK) t->strong = true;
    if (strong) t->streak = jump ? 1 : t->streak + 1;
    else t->streak = t->streak < ANON_CONF - 1 ? t->streak : ANON_CONF - 1;   /* слабое: этот кадр — логотип */
    t->since = 0;
    t->t_conf = now;
    t->hit = true;
}

/* основная обработка; sy — готовая уменьшенная Y (sw x sh, шаг sys) или NULL */
static int process(AnonCtx *c, uint8_t *y, uint8_t *u, uint8_t *v, int ys, int cs, const uint8_t *sy, int sys) {
    unsigned g = atomic_load(&g_gen);
    if (g != c->gen) { c->gen = g; anon_enable(c, atomic_load(&g_def)); }
    c->nlast = 0;
    c->faces = 0;
    if (!c->on) { c->state = ANON_OFF; atomic_store(&g_status, ANON_OFF); return ANON_SEND; }
    long long t0 = plat_now_us();
    rnd_mix(c, (uint64_t)t0);
    Pic P = { y, u, v, ys, cs, c->w, c->h };

    uint8_t *sp = c->prev; c->prev = c->small; c->small = sp;   /* прошлая копия — в prev */
    if (sy) for (int j = 0; j < c->sh; j++) memcpy(c->small + (size_t)j * c->sw, sy + (size_t)j * sys, (size_t)c->sw);
    else downscale(c, y, ys);
    bool light, scene;
    blocks(c, &light, &scene);
    c->have_prev = true;
    long long t1 = plat_now_us();
    c->us_down += t1 - t0;
    if (light || scene) {
        c->unstable = ANON_CONF;
        for (int i = 0; i < c->ntr; i++) c->tr[i].streak = 0;
    }
    bool fail = false;
    int mode = M_REUSE;

    /* 1. известные лица: неподвижное и недавно подтверждённое — без детектора; иначе — поиск рядом */
    for (int i = 0; i < c->ntr; i++) {
        Track *t = &c->tr[i];
        t->hit = false;
        bool due = t->streak < ANON_CONF || t->since + 1 >= CONFIRM_N || t0 - t->t_conf >= CONFIRM_US || area_changed(c, t->r, t->c, 0.75f * t->s);
        if (!due) { t->hit = true; t->since++; t->vr = t->vc = 0; continue; }
        if (mode < M_LOCAL) mode = M_LOCAL;
        float d[4], bd[4];
        int ba = 0;
        bool got = false;
        /* сначала прежний угол; не нашли уверенно — соседние (+-11 град), только при потере */
        int tries[3] = { t->a, t->a - 1, t->a + 1 };
        for (int k = 0; k < 3; k++) {
            int a = tries[k];
            if (a < -2 || a > 2) continue;
            if (local_at(c, t, a, d) && d[3] >= Q_MASK && (!got || d[3] > bd[3])) { memcpy(bd, d, sizeof bd); ba = a; got = true; }
            if (got && bd[3] >= Q_LOCAL) break;
        }
        if (got) track_update(c, t, bd, ba, bd[3] >= Q_LOCAL, t0);
        else t->since++;
    }

    /* 2. поиск новых лиц: окна с центром в изменившихся блоках (вне найденных лиц) + полоса 1/STRIPES высоты
     *    (весь кадр — за STRIPES кадров, для неподвижных лиц); первый кадр после включения — весь кадр */
    for (int i = 0; i < c->ntr; i++) {   /* внутри найденного лица новых лиц не ищем */
        const Track *t = &c->tr[i];
        if (!t->hit) continue;
        int by0 = (int)ceilf((t->r - 0.4f * t->s) / BLK), by1 = (int)floorf((t->r + 0.4f * t->s) / BLK) - 1;
        int bx0 = (int)ceilf((t->c - 0.4f * t->s) / BLK), bx1 = (int)floorf((t->c + 0.4f * t->s) / BLK) - 1;
        for (int by = by0 < 0 ? 0 : by0; by <= by1 && by < c->bh; by++)
            for (int bx = bx0 < 0 ? 0 : bx0; bx <= bx1 && bx < c->bw; bx++) c->chg2[by * c->bw + bx] = 0;
    }
    float b0 = (float)c->sh * (float)c->stripe / STRIPES, b1 = (float)c->sh * (float)(c->stripe + 1) / STRIPES;
    if (++c->stripe >= STRIPES) c->stripe = 0;
    const uint8_t *mask = c->chg2;
    bool full = true, any = false;
    for (int i = 0; i < c->bw * c->bh; i++) { full = full && c->chg2[i]; any = any || c->chg2[i]; }
    if (full) mask = NULL;
    int nnew = 0;
    {
        if (mode < (full ? M_FULL : M_PART)) mode = full ? M_FULL : M_PART;
        int mn = c->sw < c->sh ? c->sw : c->sh, nd;
        if (full) nd = scan(c, c->det, MAXDET, 0, (float)c->sh, 0, (float)c->sw, MIN_FACE, (float)mn, 0, NULL, FINE_STEP, FINE_STRIDE);
        else {
            /* полоса — мелкой сеткой (неподвижные лица, слабые для грубой), изменения — грубой (быстро) */
            nd = scan(c, c->det, MAXDET, b0, b1, 0, (float)c->sw, MIN_FACE, (float)mn, 0, NULL, FINE_STEP, FINE_STRIDE);
            if (any && nd < MAXDET) nd += scan(c, c->det + 4 * nd, MAXDET - nd, 0, (float)c->sh, 0, (float)c->sw, MIN_FACE, (float)mn, 0, mask, SCALE_STEP, STRIDE);
        }
        if (nd >= MAXDET) fail = true;
        int ncl = nd > 0 ? cluster(c, nd) : 0;
        if (ncl > 64) { fail = true; ncl = 64; }
        for (int k = 0; k < ncl && !fail; k++) {
            float *d = c->det + 4 * k;
            if (d[3] < Q_MASK) continue;
            int best = -1;
            float bdist = 1e9f;
            for (int i = 0; i < c->ntr; i++) {
                Track *t = &c->tr[i];
                float dr = d[0] - t->r, dc = d[1] - t->c, dist = sqrtf(dr * dr + dc * dc);
                float ratio = d[2] / t->s;
                /* то же лицо: рядом и того же размера (мелкое срабатывание внутри крупного лица — не оно) */
                if (dist < 0.6f * fmaxf(t->s, d[2]) && ratio < 1.5f && ratio > 1 / 1.5f && dist < bdist) { bdist = dist; best = i; }
            }
            if (best >= 0) {
                Track *t = &c->tr[best];
                if (!t->hit) track_update(c, t, d, 0, d[3] >= Q_TRACK, t0);   /* ушёл из окрестности — нашёлся полным */
                continue;
            }
            /* новое: трек-кандидат (и слабое срабатывание — вдруг лицо): закрывается сразу, кадр — логотип, пока
             * локальный поиск не подтвердит его ANON_CONF раз подряд; не подтвердился — тихо исчезает */
            if (c->ntr >= ANON_MAXFACE) { fail = true; continue; }
            Track *t = &c->tr[c->ntr++];
            memset(t, 0, sizeof *t);
            t->r = d[0]; t->c = d[1]; t->s = d[2]; t->q = d[3];
            t->streak = d[3] >= Q_TRACK ? 1 : 0;
            t->strong = d[3] >= Q_TRACK;
            t->t_conf = t0;
            t->hit = true;
            nnew++;
        }
    }
    c->us_det += plat_now_us() - t1;
    c->mode = mode;

    /* 3. пропавшие лица */
    for (int i = 0; i < c->ntr; i++) {
        Track *t = &c->tr[i];
        if (t->hit) continue;
        /* у края кадра — вероятно, ушло; посреди кадра — логотип на LOST_HOLD кадров и «призрак» области */
        float m = t->s * 0.6f;
        bool edge = t->r < m || t->c < m || t->r > c->sh - m || t->c > c->sw - m;
        /* в этом же кадре появилось новое лицо — скорее всего, то же, рывком (новый трек и так ждёт подтверждений) */
        /* неподтверждённый кандидат исчезает без удержания (его область ещё закрывается «призраком») */
        bool hold = false;
        if (!edge && nnew > 0) nnew--;
        else {
            if (!edge && t->confirmed) c->lost_hold = LOST_HOLD;
            /* и у края: лицо, обрезанное краем, детектор теряет, хотя оно на месте; ушедшее — сдвинет область */
            hold = t->confirmed || t->strong;   /* и кандидат, хоть раз найденный уверенно, — скорее лицо */
        }
        int slot = -1;
        for (int j = 0; j < ANON_MAXFACE; j++)   /* свободный или самый старый без удержания */
            if (!c->gh[j].hold && (slot < 0 || c->gh[j].left < c->gh[slot].left)) slot = j;
        if (slot < 0) fail = true;               /* все заняты удерживаемыми — логотип */
        else {
            Ghost *gp = &c->gh[slot];
            Rect gr = eye_band(c, t->r, t->c, t->s, t->a);
            for (int q = 0; q < t->nh; q++) gr = runion(gr, t->hist[q]);
            memset(gp, 0, sizeof *gp);
            gp->r = gr; gp->left = GHOST; gp->hold = hold;
            gp->gr = t->r; gp->gc = t->c; gp->gs = t->s;
        }
        c->tr[i] = c->tr[--c->ntr];
        i--;
    }

    /* 4. движение вне зон известных лиц (лицо + плечи и руки ниже): там может быть лицо, которое детектор ещё
     *    не видит (входит в кадр, видно наполовину, поворачивается) — логотип, пока не уляжется */
    {
        int unexpl = 0;
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++) {
                if (!c->chg[by * c->bw + bx]) continue;
                float r = (by + 0.5f) * BLK, cc = (bx + 0.5f) * BLK;
                bool in = false;
                for (int i = 0; i < c->ntr && !in; i++) {
                    const Track *t = &c->tr[i];
                    in = r >= t->r - 0.9f * t->s && fabsf(cc - t->c) <= 1.6f * t->s;
                }
                unexpl += !in;
            }
        if (unexpl >= MOVE_MIN) c->moving = MOVE_HOLD;
    }

    /* 5. решение: в эфир с закрытыми глазами — только если все лица уверенные и всё спокойно */
    bool send = !fail && c->ntr > 0 && c->unstable == 0 && c->lost_hold == 0 && c->moving == 0;
    /* слабый трек (ни разу не набрал Q_TRACK) закрывается, но лицом не считается: ложное срабатывание на фоне
     * не должно открывать эфир, пока настоящее лицо не найдено */
    int nface = 0;
    for (int i = 0; i < c->ntr; i++) {
        if (c->tr[i].streak < ANON_CONF) send = false;
        else c->tr[i].confirmed = true;
        nface += c->tr[i].strong;
    }
    if (!nface) send = false;
    /* удерживаемые призраки: лицо нашлось снова — отпустить; область сдвинулась и успокоилась — отпустить */
    for (int j = 0; j < ANON_MAXFACE; j++) {
        Ghost *gp = &c->gh[j];
        if (!gp->hold) continue;
        bool found = false;
        for (int i = 0; i < c->ntr && !found; i++) {
            const Track *t = &c->tr[i];
            float dr = t->r - gp->gr, dc = t->c - gp->gc, ratio = t->s / gp->gs;
            found = t->strong && sqrtf(dr * dr + dc * dc) < 0.6f * fmaxf(t->s, gp->gs) && ratio < 1.5f && ratio > 1 / 1.5f;
        }
        if (area_changed(c, gp->gr, gp->gc, 0.75f * gp->gs)) { gp->moved = true; gp->calm = 0; }
        else if (gp->moved) gp->calm++;
        if (found) { gp->hold = false; if (gp->left > HIST) gp->left = HIST; }
        else if (gp->moved && gp->calm >= MOVE_HOLD) gp->hold = false;
        else { send = false; if (gp->left < 2) gp->left = 2; }
    }

    /* области: текущая полоса + последние HIST + прогноз на кадр вперёд (история — и в кадрах логотипа) */
    Rect rects[ANON_MAXFACE * 4];
    int nr = 0;
    for (int i = 0; i < c->ntr; i++) {
        Track *t = &c->tr[i];
        Rect cur = eye_band(c, t->r, t->c, t->s, t->a);
        Rect u2 = cur;
        for (int q = 0; q < t->nh; q++) u2 = runion(u2, t->hist[q]);
        float pr = t->vr * 1.5f, pc = t->vc * 1.5f;
        Rect pred = clip_even(c, (float)cur.x0 + fminf(pc, 0), (float)cur.y0 + fminf(pr, 0), (float)cur.x1 + fmaxf(pc, 0), (float)cur.y1 + fmaxf(pr, 0));
        u2 = runion(u2, pred);
        memmove(t->hist + 1, t->hist, sizeof(Rect) * (HIST - 1));
        t->hist[0] = cur;
        if (t->nh < HIST) t->nh++;
        rects[nr++] = u2;
    }
    for (int j = 0; j < ANON_MAXFACE; j++)
        if (c->gh[j].left > 0) {
            c->gh[j].left--;
            if (nr < (int)(sizeof rects / sizeof *rects)) rects[nr++] = c->gh[j].r;
            else send = false;
        }
    if (c->unstable > 0) c->unstable--;
    if (c->lost_hold > 0) c->lost_hold--;
    if (c->moving > 0) c->moving--;

    if (!send) {
        draw_logo(&P);
        return finish(c, ANON_NOFACE, t0, ANON_LOGO);
    }
    for (int i = 0; i < nr; i++) { mask_rect(c, &P, rects[i]); c->last[c->nlast++] = rects[i]; }
    c->faces = c->ntr;
    return finish(c, ANON_TRACKING, t0, ANON_SEND);
}

int anon_frame(AnonCtx *c, uint8_t *y, uint8_t *u, uint8_t *v, int ys, int cs, int w, int h) {
    if (!args_ok(c, y, u, v, ys, cs, w, h)) return ANON_DROP;
    return process(c, y, u, v, ys, cs, NULL, 0);
}
int anon_frame_small(AnonCtx *c, uint8_t *y, uint8_t *u, uint8_t *v, int ys, int cs, int w, int h, const uint8_t *sy, int sys) {
    if (!args_ok(c, y, u, v, ys, cs, w, h)) return ANON_DROP;
    if (sy && sys < c->sw) return ANON_DROP;
    return process(c, y, u, v, ys, cs, sy, sys);
}

/* ======================================================================================================
 * Проверки (selftest) и стенд точности (--anontest). Только числа; изображения не сохраняются и не показываются.
 * ====================================================================================================== */
static void st_seed(AnonCtx *c, uint64_t s) { c->rng = s ? s : 1; }

/* синтетическое «лицо»: тёмные глазницы, брови, нос, рот на овале — pico его, скорее всего, не найдёт; для
 * проверок fail-closed этого и надо. Для пути ANON_SEND selftest подкладывает треки напрямую. */
static void st_frame(uint8_t *y, uint8_t *u, uint8_t *v, int w, int h, int ys, int cs, unsigned seed) {
    uint32_t s = seed * 2654435761u + 1;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) { s = s * 1664525u + 1013904223u; y[(size_t)j * ys + i] = (uint8_t)(s >> 24); }
    for (int j = 0; j < h / 2; j++)
        for (int i = 0; i < w / 2; i++) { s = s * 1664525u + 1013904223u; u[(size_t)j * cs + i] = (uint8_t)(s >> 24); v[(size_t)j * cs + i] = (uint8_t)(s >> 16); }
}

int anon_selftest(void (*check)(bool ok, const char *what)) {
    int w = 640, h = 360, ys = 704, cs = 352;
    uint8_t *y = malloc((size_t)ys * h), *u = malloc((size_t)cs * h / 2), *v = malloc((size_t)cs * h / 2);
    uint8_t *y0 = malloc((size_t)ys * h), *u0 = malloc((size_t)cs * h / 2), *v0 = malloc((size_t)cs * h / 2);
    if (!y || !u || !v || !y0 || !u0 || !v0) { check(false, "anon: память под проверки"); free(y); free(u); free(v); free(y0); free(u0); free(v0); return 1; }
    char b[256];

    /* 1. размеры и аргументы */
    check(anon_new(31, 32) == NULL && anon_new(33, 32) == NULL && anon_new(64, 5000) == NULL, "anon: неверные размеры кадра -> NULL");
    AnonCtx *c = anon_new(w, h);
    check(c != NULL, "anon: контекст 640x360");
    if (!c) { free(y); free(u); free(v); free(y0); free(u0); free(v0); return 1; }
    anon_enable(c, true);
    check(anon_frame(c, y, u, v, w - 2, cs, w, h) == ANON_DROP, "anon: шаг строки Y меньше ширины -> ANON_DROP");
    check(anon_frame(c, y, u, v, ys, cs, w + 2, h) == ANON_DROP, "anon: размер не совпадает с контекстом -> ANON_DROP");
    check(anon_frame(c, NULL, u, v, ys, cs, w, h) == ANON_DROP && anon_frame(NULL, y, u, v, ys, cs, w, h) == ANON_DROP, "anon: NULL -> ANON_DROP");

    /* 2. нет лица -> логотип, буфер целиком перезаписан (ни одного значения, зависящего от исходника) */
    st_frame(y, u, v, w, h, ys, cs, 1);
    memset(y0, 0x5a, (size_t)ys * h);   /* поля за шириной — не трогаются */
    for (int j = 0; j < h; j++) memcpy(y0 + (size_t)j * ys + w, y + (size_t)j * ys + w, (size_t)(ys - w));
    int r1 = anon_frame(c, y, u, v, ys, cs, w, h);
    /* тот же логотип из другого исходного кадра должен совпасть байт в байт (в пределах w x h) */
    memcpy(y0, y, (size_t)ys * h); memcpy(u0, u, (size_t)cs * h / 2); memcpy(v0, v, (size_t)cs * h / 2);
    st_frame(y, u, v, w, h, ys, cs, 2);
    int r2 = anon_frame(c, y, u, v, ys, cs, w, h);
    bool same = true;
    for (int j = 0; j < h && same; j++) same = !memcmp(y + (size_t)j * ys, y0 + (size_t)j * ys, (size_t)w);
    for (int j = 0; j < h / 2 && same; j++) same = !memcmp(u + (size_t)j * cs, u0 + (size_t)j * cs, (size_t)w / 2) && !memcmp(v + (size_t)j * cs, v0 + (size_t)j * cs, (size_t)w / 2);
    check(r1 == ANON_LOGO && r2 == ANON_LOGO && anon_state(c) == ANON_NOFACE, "anon: лица нет -> ANON_LOGO, состояние ANON_NOFACE");
    check(same, "anon: логотип не зависит от кадра камеры (два разных кадра -> одинаковый буфер)");
    {
        int hist[256] = { 0 };
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) hist[y[(size_t)j * ys + i]]++;
        int distinct = 0;
        for (int i = 0; i < 256; i++) distinct += hist[i] > 0;
        snprintf(b, sizeof b, "anon: логотип — фон и рисунок (%d значений Y), без остатков шума камеры", distinct);
        check(distinct <= 4, b);
    }
    /* предупреждение для предпросмотра — тоже без пикселей камеры */
    st_frame(y, u, v, w, h, ys, cs, 3);
    check(anon_preview_warning(c, y, u, v, ys, cs, w, h), "anon: предупреждение нарисовано");
    memcpy(y0, y, (size_t)ys * h);
    st_frame(y, u, v, w, h, ys, cs, 4);
    anon_preview_warning(c, y, u, v, ys, cs, w, h);
    same = true;
    for (int j = 0; j < h && same; j++) same = !memcmp(y + (size_t)j * ys, y0 + (size_t)j * ys, (size_t)w);
    check(same, "anon: предупреждение не зависит от кадра камеры");
    check(!anon_preview_warning(c, y, u, v, ys, cs, w, h - 2), "anon: предупреждение с неверным размером -> false");

    /* 3. выключено -> кадр не тронут, ANON_SEND; включение -> с первого кадра логотип */
    anon_enable(c, false);
    st_frame(y, u, v, w, h, ys, cs, 5);
    memcpy(y0, y, (size_t)ys * h);
    check(anon_frame(c, y, u, v, ys, cs, w, h) == ANON_SEND && !memcmp(y, y0, (size_t)ys * h) && anon_state(c) == ANON_OFF, "anon: выключено -> кадр без изменений");
    anon_enable(c, true);
    check(anon_frame(c, y, u, v, ys, cs, w, h) == ANON_LOGO, "anon: включение действует с первого кадра (логотип)");

    /* 4. закрытие глаз: трек подложен напрямую (уверенный), проверяем необратимость и границы */
    {
        Pic P = { y, u, v, ys, cs, w, h };
        Rect rr[3] = { { 100, 60, 300, 140 }, { 0, 0, 64, 32 }, { w - 50, h - 20, w, h } };   /* с краями кадра */
        bool irrev = true, bounds = true;
        for (int k = 0; k < 3; k++) {
            st_frame(y, u, v, w, h, ys, cs, 10 + (unsigned)k);
            memcpy(y0, y, (size_t)ys * h); memcpy(u0, u, (size_t)cs * h / 2);
            st_seed(c, 77);
            mask_rect(c, &P, rr[k]);
            /* за пределами прямоугольника ничего не изменилось */
            for (int j = 0; j < h; j++)
                for (int i = 0; i < ys; i++) {
                    bool in = i >= rr[k].x0 && i < rr[k].x1 && j >= rr[k].y0 && j < rr[k].y1;
                    if (!in && y[(size_t)j * ys + i] != y0[(size_t)j * ys + i]) bounds = false;
                }
            /* другая картинка с тем же средним внутри (перестановка пикселей) + та же случайность -> тот же результат:
             * значит, кроме (огрублённого) среднего, ничего из исходника в области нет */
            uint8_t *keep = malloc((size_t)ys * h);
            if (!keep) { irrev = false; continue; }
            memcpy(keep, y, (size_t)ys * h);
            memcpy(y, y0, (size_t)ys * h); memcpy(u, u0, (size_t)cs * h / 2);
            for (int j = rr[k].y0; j < rr[k].y1; j++) {   /* разворот строки внутри области: среднее то же */
                uint8_t *p = y + (size_t)j * ys;
                for (int a = rr[k].x0, z = rr[k].x1 - 1; a < z; a++, z--) { uint8_t t = p[a]; p[a] = p[z]; p[z] = t; }
            }
            for (int j = rr[k].y0 / 2, z = rr[k].y1 / 2 - 1; j < z; j++, z--)   /* строки цветности — переставить */
                for (int i = rr[k].x0 / 2; i < rr[k].x1 / 2; i++) { uint8_t t = u[(size_t)j * cs + i]; u[(size_t)j * cs + i] = u[(size_t)z * cs + i]; u[(size_t)z * cs + i] = t; }
            st_seed(c, 77);
            mask_rect(c, &P, rr[k]);
            for (int j = rr[k].y0; j < rr[k].y1; j++)
                if (memcmp(y + (size_t)j * ys + rr[k].x0, keep + (size_t)j * ys + rr[k].x0, (size_t)(rr[k].x1 - rr[k].x0))) irrev = false;
            /* и доля совпадений с исходными значениями — на уровне случайности */
            long eq = 0, tot = 0;
            for (int j = rr[k].y0; j < rr[k].y1; j++)
                for (int i = rr[k].x0; i < rr[k].x1; i++, tot++) eq += keep[(size_t)j * ys + i] == y0[(size_t)j * ys + i];
            if (tot && eq * 100 / tot > 5) irrev = false;
            free(keep);
        }
        check(irrev, "anon: закрытие необратимо (в области только огрублённое среднее + шум + символы)");
        check(bounds, "anon: закрытие не выходит за прямоугольник, края кадра и шаг строки соблюдены");
    }
    /* 5. путь ANON_SEND: подложенный уверенный трек на «пустом» кадре пропадает -> логотип, а не открытый кадр */
    {
        anon_enable(c, false); anon_enable(c, true);
        st_frame(y, u, v, w, h, ys, cs, 20);
        anon_frame(c, y, u, v, ys, cs, w, h);      /* прошлый кадр — тот же (неподвижная сцена) */
        st_frame(y, u, v, w, h, ys, cs, 20);
        c->ntr = 1;
        memset(&c->tr[0], 0, sizeof c->tr[0]);
        c->tr[0].r = (float)c->sh / 2; c->tr[0].c = (float)c->sw / 2; c->tr[0].s = 60; c->tr[0].streak = 10; c->tr[0].confirmed = true; c->tr[0].strong = true;
        c->unstable = 0; c->tr[0].t_conf = plat_now_us(); c->tr[0].since = CONFIRM_N;   /* пора подтверждать */
        int r = anon_frame(c, y, u, v, ys, cs, w, h);
        check(r == ANON_LOGO && c->lost_hold > 0, "anon: трек пропал посреди кадра -> логотип и удержание");
        /* «призрак» области ещё закрывается */
        check(c->gh[0].left > 0 && c->gh[0].r.x1 > c->gh[0].r.x0 && c->gh[0].hold, "anon: область пропавшего лица закрывается, логотип держится, пока лицо не найдётся или область не сдвинется");
        /* та же неподвижная картинка ещё 60 кадров — удержание не отпускается само по времени */
        for (int k = 0; k < 60; k++) { st_frame(y, u, v, w, h, ys, cs, 20); anon_frame(c, y, u, v, ys, cs, w, h); }
        check(c->gh[0].hold && anon_state(c) == ANON_NOFACE, "anon: пропавшее лицо без движения в его области — логотип и через 60 кадров");
    }
    /* 5а. быстрый путь каскада даёт ровно то же, что исходный run_cascade (все окна на картинке-шуме и градиенте) */
    {
        st_frame(y, u, v, w, h, ys, cs, 50);
        downscale(c, y, ys);
        for (int j = 0; j < c->sh; j++) for (int i = 0; i < c->sw / 2; i++) c->small[j * c->sw + i] = (uint8_t)(i * 2 + j);
        long nw = 0, diff = 0, pos = 0;
        for (int sz = 20; sz < c->sh; sz = sz * 5 / 4) {
            fast_table(c->fast, sz, c->sw);
            for (int r = 0; r < c->sh; r += 3)
                for (int cc = 0; cc < c->sw; cc += 3) {
                    float q1 = 0, q2 = 0;
                    int a1 = run_cascade(&q1, r, cc, sz, c->small, c->sh, c->sw, c->sw), a2 = run_fast(&q2, r, cc, sz, c->small, c->sh, c->sw, c->sw, c->fast);
                    nw++;
                    pos += a1 == 1;
                    if (a1 != a2 || (a1 == 1 && q1 != q2)) diff++;
                }
        }
        snprintf(b, sizeof b, "anon: быстрый путь детектора совпадает с исходным pico (%ld окон, прошли каскад %ld, расхождений %ld)", nw, pos, diff);
        check(diff == 0 && nw > 1000, b);
    }
    /* 6. время на кадр 1280x720 (шум — худший случай для детектора) */
    {
        AnonCtx *c7 = anon_new(1280, 720);
        uint8_t *Y = malloc(1280 * 720), *U = malloc(640 * 360), *V = malloc(640 * 360);
        if (c7 && Y && U && V) {
            anon_enable(c7, true);
            long long best = 1LL << 60;
            for (int k = 0; k < 5; k++) {
                st_frame(Y, U, V, 1280, 720, 1280, 640, 30 + (unsigned)k);
                long long t = plat_now_us();
                anon_frame(c7, Y, U, V, 1280, 640, 1280, 720);
                t = plat_now_us() - t;
                if (t < best) best = t;
            }
            snprintf(b, sizeof b, "anon: 1280x720 на шуме — %.2f мс/кадр (порог 15 мс на selftest)", best / 1000.0);
            check(best < 15000, b);
            size_t mem = sizeof *c7 + 2u * (size_t)c7->sw * c7->sh + 2u * (size_t)c7->bw * c7->bh + 4u * 1280;
            snprintf(b, sizeof b, "anon: память контекста 1280x720 — %zu КБ (кадр не копируется)", mem / 1024);
            check(mem < 200 * 1024, b);
        } else check(false, "anon: память под замер");
        anon_free(c7); free(Y); free(U); free(V);
    }
    /* 6а. anon_frame_small (готовая уменьшенная Y) даёт тот же результат, что anon_frame */
    {
        AnonCtx *c1 = anon_new(w, h), *c2 = anon_new(w, h);
        bool ok = c1 && c2;
        int sw = 0, sh = 0, f = 0;
        ok = ok && anon_small_size(c1, &sw, &sh, &f) && sw == w / f && sh == h / f;
        uint8_t *sm = malloc((size_t)(sw + 8) * (sh > 0 ? sh : 1));
        if (ok && sm) {
            anon_enable(c1, true); anon_enable(c2, true);
            for (int k = 0; k < 3 && ok; k++) {
                st_frame(y, u, v, w, h, ys, cs, 60 + (unsigned)k);
                memcpy(y0, y, (size_t)ys * h); memcpy(u0, u, (size_t)cs * h / 2); memcpy(v0, v, (size_t)cs * h / 2);
                for (int j = 0; j < sh; j++)        /* среднее блоков f x f — как у модуля */
                    for (int i = 0; i < sw; i++) {
                        unsigned sum = 0;
                        for (int a = 0; a < f; a++) for (int z = 0; z < f; z++) sum += y[(size_t)(j * f + a) * ys + i * f + z];
                        sm[(size_t)j * (sw + 8) + i] = (uint8_t)((sum * (unsigned)(65536 / (f * f)) + 32768) >> 16);
                    }
                st_seed(c1, 99); st_seed(c2, 99);
                int q1 = anon_frame(c1, y, u, v, ys, cs, w, h);
                int q2 = anon_frame_small(c2, y0, u0, v0, ys, cs, w, h, sm, sw + 8);
                ok = q1 == q2 && q1 != ANON_DROP && !memcmp(y, y0, (size_t)ys * h) && !memcmp(u, u0, (size_t)cs * h / 2);
            }
            ok = ok && anon_frame_small(c2, y, u, v, ys, cs, w, h, sm, sw - 1) == ANON_DROP;
        } else ok = false;
        check(ok, "anon: вариант с готовой уменьшенной Y совпадает с обычным; узкий шаг -> ANON_DROP");
        free(sm); anon_free(c1); anon_free(c2);
    }
    /* 7. глобальный флаг: выключение действует на открытый контекст с ближайшего кадра */
    {
        bool was = anon_default();
        anon_set_default(false);
        st_frame(y, u, v, w, h, ys, cs, 40);
        memcpy(y0, y, (size_t)ys * h);
        int r = anon_frame(c, y, u, v, ys, cs, w, h);
        bool ok = r == ANON_SEND && !memcmp(y, y0, (size_t)ys * h) && anon_status() == ANON_OFF;
        anon_set_default(true);
        r = anon_frame(c, y, u, v, ys, cs, w, h);
        ok = ok && r == ANON_LOGO;
        anon_set_default(was);
        check(ok, "anon: глобальный флаг «Скрывать глаза» — сразу на открытый контекст");
    }
    anon_free(c);
    free(y); free(u); free(v); free(y0); free(u0); free(v0);
    return 0;
}

/* ---------- стенд точности: --anontest=КАТАЛОГ (BioID: BioID_NNNN.pgm + .eye) ---------- */
typedef struct { int w, h; uint8_t *p; float lx, ly, rx, ry; } Img;
static bool load_pgm(const char *path, Img *im) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char m[3] = { 0 };
    int w, h, mx;
    bool ok = fscanf(f, "%2s %d %d %d", m, &w, &h, &mx) == 4 && !strcmp(m, "P5") && w > 0 && h > 0 && w < 10000 && h < 10000 && mx == 255;
    if (ok) {
        fgetc(f);
        im->p = malloc((size_t)w * h);
        ok = im->p && fread(im->p, 1, (size_t)w * h, f) == (size_t)w * h;
        im->w = w; im->h = h;
    }
    fclose(f);
    return ok;
}
static bool load_eye(const char *path, Img *im) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[128];
    bool ok = fgets(line, sizeof line, f) && fscanf(f, "%f %f %f %f", &im->lx, &im->ly, &im->rx, &im->ry) == 4;
    fclose(f);
    return ok;
}
/* в область W x H плоскости Y (шаг ys): изображение с масштабом sc, поворотом a (рад) вокруг центра
 * изображения, сдвигом (tx,ty), яркость gain/bias, шум сенсора +-noise; края — продолжение крайних пикселей.
 * eye — координаты глаз в области. */
typedef struct { float sc, a, tx, ty, gain, bias; int noise; } Xf;
static void render(const Img *im, const Xf *x, uint8_t *Y, int ys, int W, int H, float eye[4], uint32_t *rs) {
    float ca = cosf(x->a), sa = sinf(x->a), icx = im->w / 2.0f, icy = im->h / 2.0f;
    float ocx = W / 2.0f + x->tx, ocy = H / 2.0f + x->ty;
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            float dx = (i - ocx) / x->sc, dy = (j - ocy) / x->sc;
            float sx = ca * dx + sa * dy + icx, sy = -sa * dx + ca * dy + icy;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float fx = sx - x0, fy = sy - y0;
            int xa = x0 < 0 ? 0 : x0 >= im->w ? im->w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= im->w ? im->w - 1 : x0 + 1;
            int ya = y0 < 0 ? 0 : y0 >= im->h ? im->h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= im->h ? im->h - 1 : y0 + 1;
            float p = (im->p[ya * im->w + xa] * (1 - fx) + im->p[ya * im->w + xb] * fx) * (1 - fy) + (im->p[yb * im->w + xa] * (1 - fx) + im->p[yb * im->w + xb] * fx) * fy;
            p = p * x->gain + x->bias;
            if (x->noise) { *rs = *rs * 1664525u + 1013904223u; p += (float)((int)(*rs >> 24) % (2 * x->noise + 1) - x->noise); }
            Y[(size_t)j * ys + i] = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
        }
    float e[4] = { im->lx, im->ly, im->rx, im->ry };
    for (int k = 0; k < 2; k++) {
        float dx = e[2 * k] - icx, dy = e[2 * k + 1] - icy;
        eye[2 * k] = (ca * dx - sa * dy) * x->sc + ocx;
        eye[2 * k + 1] = (sa * dx + ca * dy) * x->sc + ocy;
    }
}
static bool in_rects(const AnonCtx *c, float x, float y) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return true;   /* вне кадра — не видно */
    for (int i = 0; i < c->nlast; i++)
        if (x >= c->last[i].x0 && x < c->last[i].x1 && y >= c->last[i].y0 && y < c->last[i].y1) return true;
    return false;
}
/* глаза «закрыты»: центр и окружность радиусом 0.3 межзрачкового расстояния целиком внутри закрытых областей.
 * Глаз, у которого хоть одна точка видна в кадре, обязан быть закрыт. e — пары глаз (ne пар). */
static bool eyes_covered(const AnonCtx *c, const float *e, int ne) {
    for (int p = 0; p < ne; p++) {
        const float *q = e + 4 * p;
        float iod = hypotf(q[2] - q[0], q[3] - q[1]), rad = 0.3f * iod;
        for (int k = 0; k < 2; k++) {
            if (!in_rects(c, q[2 * k], q[2 * k + 1])) return false;
            for (int a = 0; a < 16; a++) {
                float an = a * 6.2831853f / 16;
                if (!in_rects(c, q[2 * k] + rad * cosf(an), q[2 * k + 1] + rad * sinf(an))) return false;
            }
        }
    }
    return true;
}
typedef struct { long frames, sent, logo, drop, leak, warm, warm_logo; double us; long long us_max; } Stat;
/* время по режимам (M_*) и итогу (0 — с закрытыми глазами, 1 — логотип) для каждого размера кадра */
static long g_mn[3][M_N][2];
static double g_mus[3][M_N][2];
static long long g_mmax[3][M_N][2];
static void st_print(const char *name, const Stat *s) {
    char b[320];
    snprintf(b, sizeof b, "anontest: %-28s кадров %6ld  с закрытыми глазами %6ld  логотип %5.1f %% (после разгона %5.1f %%)  ОТКРЫТЫХ ГЛАЗ %ld  drop %ld  %.2f мс/кадр (макс %.2f)\n",
             name, s->frames, s->sent, s->frames ? 100.0 * s->logo / s->frames : 0, s->warm ? 100.0 * s->warm_logo / s->warm : 0, s->leak, s->drop,
             s->frames ? s->us / s->frames / 1000.0 : 0, s->us_max / 1000.0);
    term_puts(b);
}
/* калибровка по всем сценариям: положение глаз в системе квадрата ближайшего трека (с учётом его угла) —
 * [0] без поворота, [1] с поворотом: мин/макс строки, макс |столбца| */
static float g_cal[2][3] = { { 1e9f, -1e9f, 0 }, { 1e9f, -1e9f, 0 } };
static void st_calib(const AnonCtx *c, const float *eye, int ne) {
    for (int e = 0; e < 2 * ne; e++) {
        float ex = eye[2 * e], ey = eye[2 * e + 1];
        const Track *bt = NULL;
        float bd = 1e9f, f = (float)c->f, off = (f - 1) / 2;
        for (int i = 0; i < c->ntr; i++) {
            const Track *t = &c->tr[i];
            float d = hypotf(ex - (t->c * f + off), ey - (t->r * f + off));
            if (t->strong && d < 0.7f * t->s * f && d < bd + t->s * f && (!bt || t->s > bt->s)) { bd = d; bt = t; }   /* крупнейший уверенный: «владелец» лица */
        }
        if (!bt) continue;
        float R = bt->r * f + off, C = bt->c * f + off, Sz = bt->s * f, th = (float)bt->a * 6.2831853f / 32;
        float dy = ey - R, dx = ex - C;
        /* обратный поворот: (dr, dc) = R(-th) * (dy, dx) в обозначениях run_rot */
        float dr = (cosf(th) * dy + sinf(th) * dx) / Sz, dc = (-sinf(th) * dy + cosf(th) * dx) / Sz;
        float *g = g_cal[bt->a != 0];
        if (dr < g[0]) g[0] = dr;
        if (dr > g[1]) g[1] = dr;
        if (fabsf(dc) > g[2]) g[2] = fabsf(dc);
    }
}
static void st_run(AnonCtx *c, int res, Stat *s, uint8_t *Y, uint8_t *U, uint8_t *V, const float *eye, int ne, bool warm) {
    int r = anon_frame(c, Y, U, V, c->w, c->w / 2, c->w, c->h);
    if (r == ANON_SEND && ne == 1) st_calib(c, eye, ne);
    s->frames++;
    if (warm) s->warm++;
    s->us += (double)c->us_last;
    if (c->us_last > s->us_max) s->us_max = c->us_last;
    int lg = r != ANON_SEND;
    g_mn[res][c->mode][lg]++; g_mus[res][c->mode][lg] += (double)c->us_last;
    if (c->us_last > g_mmax[res][c->mode][lg]) g_mmax[res][c->mode][lg] = c->us_last;
    if (r == ANON_DROP) s->drop++;
    else if (r == ANON_LOGO) { s->logo++; if (warm) s->warm_logo++; }
    else {
        s->sent++;
        if (!eyes_covered(c, eye, ne)) {
            s->leak++;
        }
    }
}
static bool load_one(const char *dir, int idx, Img *im) {
    char path[600];
    memset(im, 0, sizeof *im);
    snprintf(path, sizeof path, "%s/BioID_%04d.pgm", dir, idx);
    if (!load_pgm(path, im)) { free(im->p); im->p = NULL; return false; }
    snprintf(path, sizeof path, "%s/BioID_%04d.eye", dir, idx);
    if (!load_eye(path, im)) { free(im->p); im->p = NULL; return false; }
    return true;
}
int anon_test(const char *dir) {
    char b[320];
    enum { SEQ_STATIC, SEQ_PAN, SEQ_ZOOM, SEQ_ROT, SEQ_LIGHT, SEQ_SHAKE, SEQ_ENTER, SEQ_360, SEQ_HD, SEQ_TWO, SEQ_N };
    static const char *const nm[SEQ_N] = { "статично 640x480", "движение 640x480", "приближение 640x480", "наклон +-25 град 640x480",
                                           "освещение 640x480", "тряска/рывки 640x480", "вход в кадр 640x480", "статично 640x360",
                                           "статично 1280x720", "два лица 1280x720" };
    Stat st[SEQ_N];
    memset(st, 0, sizeof st);
    memset(g_mn, 0, sizeof g_mn); memset(g_mus, 0, sizeof g_mus); memset(g_mmax, 0, sizeof g_mmax);
    AnonCtx *c = anon_new(640, 480), *c3 = anon_new(640, 360), *hd = anon_new(1280, 720);
    uint8_t *Y = malloc(1280 * 720), *U = malloc(640 * 360), *V = malloc(640 * 360);
    if (!c || !c3 || !hd || !Y || !U || !V) { term_puts("anontest: нет памяти\n"); return 1; }
    /* ANON_N / ANON_I0 — сколько изображений и с какого начать (для быстрых прогонов) */
    int lim = getenv("ANON_N") ? atoi(getenv("ANON_N")) : 100000, i0 = getenv("ANON_I0") ? atoi(getenv("ANON_I0")) : 0;
    snprintf(b, sizeof b, "anontest: мин. лицо %.0f px из %d, шаг масштаба %.2f, шаг окна %.2f (полосы %.2f/%.2f), полоса %.2f..%.2f +-%.2f, пороги %.1f/%.1f\n",
             MIN_FACE, SMALL_W, SCALE_STEP, STRIDE, FINE_STEP, FINE_STRIDE, EY0, EY1, EX, Q_TRACK, Q_LOCAL);
    term_puts(b);
    long nimg = 0;
    uint32_t rs = 12345;
    Img im, im2;
    for (int idx = i0; idx < i0 + lim; idx++) {
        if (!load_one(dir, idx, &im)) { if (idx > i0) break; continue; }
        nimg++;
        bool two = load_one(dir, (idx * 7 + 500) % 1521, &im2);
        float eye[8];
        float base = 640.0f / im.w * 0.95f;
        for (int sq = 0; sq < SEQ_N; sq++) {
            if (sq == SEQ_TWO && !two) continue;
            AnonCtx *x = sq == SEQ_HD || sq == SEQ_TWO ? hd : sq == SEQ_360 ? c3 : c;
            int res = x == hd ? 2 : x == c3 ? 1 : 0;
            int W = x->w, H = x->h, n = sq == SEQ_STATIC || sq == SEQ_360 || sq == SEQ_HD ? 30 : 40;
            anon_enable(x, false); anon_enable(x, true);
            for (int k = 0; k < n; k++) {
                Xf t = { base, 0, 0, 0, 1, 0, 3 };
                float ph = (float)k / (float)n;
                rs = rs * 1664525u + 1013904223u;
                int ne = 1;
                switch (sq) {
                case SEQ_360: t.sc = 360.0f / im.h * 0.95f; break;
                case SEQ_HD: t.sc = 720.0f / im.h * 0.9f; break;
                case SEQ_PAN: t.tx = 120 * sinf(6.2831853f * ph + idx); t.ty = 50 * cosf(6.2831853f * ph * 2); break;          /* до ~19 px/кадр */
                case SEQ_ZOOM: t.sc = base * (0.7f + 0.8f * ph); break;
                case SEQ_ROT: t.a = 0.436f * sinf(6.2831853f * ph); break;                                                         /* +-25 град */
                case SEQ_LIGHT: t.gain = 0.6f + 0.6f * ph; t.bias = (k / 10) % 2 ? 25 : -10; break;                              /* плавно + скачки */
                case SEQ_SHAKE: t.tx = (float)((int)(rs >> 24) % 41 - 20); t.ty = (float)((int)(rs >> 16 & 255) % 25 - 12); t.a = ((int)(rs >> 8 & 255) % 11 - 5) * 0.01f; break;
                case SEQ_ENTER: t.tx = (idx & 1 ? 1 : -1) * 640 * (1 - ph * 1.6f); if (ph > 0.625f) t.tx = 0; break;              /* въезжает сбоку ~26 px/кадр */
                default: break;
                }
                if (sq == SEQ_TWO) {
                    /* левая половина — лицо A неподвижно; правая — лицо B въезжает справа */
                    Xf a = { 720.0f / im.h * 0.8f, 0, 0, 0, 1, 0, 3 }, bb = a;
                    bb.tx = 640 * (1 - ph * 1.6f);
                    if (ph > 0.625f) bb.tx = 0;
                    render(&im, &a, Y, W, 640, H, eye, &rs);
                    render(&im2, &bb, Y + 640, W, 640, H, eye + 4, &rs);
                    eye[4] += 640; eye[6] += 640;
                    ne = 2;
                } else render(&im, &t, Y, W, W, H, eye, &rs);
                memset(U, 128, (size_t)W * H / 4);
                memset(V, 128, (size_t)W * H / 4);
                st_run(x, res, &st[sq], Y, U, V, eye, ne, k >= 2 * ANON_CONF);
            }
        }
        free(im.p);
        if (two) free(im2.p);
    }
    snprintf(b, sizeof b, "anontest: изображений %ld\n", nimg);
    term_puts(b);
    snprintf(b, sizeof b, "anontest: все ушедшие кадры: без поворота строка %.3f..%.3f, столбец до %.3f; с поворотом строка %.3f..%.3f, столбец до %.3f\n",
             g_cal[0][0], g_cal[0][1], g_cal[0][2], g_cal[1][0], g_cal[1][1], g_cal[1][2]);
    term_puts(b);
    Stat all = { 0 };
    for (int i = 0; i < SEQ_N; i++) {
        st_print(nm[i], &st[i]);
        all.frames += st[i].frames; all.sent += st[i].sent; all.logo += st[i].logo; all.drop += st[i].drop; all.leak += st[i].leak;
        all.warm += st[i].warm; all.warm_logo += st[i].warm_logo; all.us += st[i].us;
        if (st[i].us_max > all.us_max) all.us_max = st[i].us_max;
    }
    st_print("ВСЕГО", &all);
    static const char *const mname[M_N] = { "без детектора", "поиск у лица", "поиск по изменениям", "полный поиск" };
    static const char *const rname[3] = { "640x480", "640x360", "1280x720" };
    for (int r = 0; r < 3; r++)
        for (int m = 0; m < M_N; m++)
            for (int l = 0; l < 2; l++) {
                if (!g_mn[r][m][l]) continue;
                snprintf(b, sizeof b, "anontest: %-8s %-20s %-10s кадров %6ld  %.2f мс/кадр (макс %.2f)\n", rname[r], mname[m], l ? "логотип" : "глаза", g_mn[r][m][l],
                         g_mus[r][m][l] / (double)g_mn[r][m][l] / 1000.0, g_mmax[r][m][l] / 1000.0);
                term_puts(b);
            }
    long nf0 = st[SEQ_STATIC].frames + st[SEQ_PAN].frames + st[SEQ_ZOOM].frames + st[SEQ_ROT].frames + st[SEQ_LIGHT].frames + st[SEQ_SHAKE].frames + st[SEQ_ENTER].frames;
    long nfh = st[SEQ_HD].frames + st[SEQ_TWO].frames;
    snprintf(b, sizeof b, "anontest: доля времени: уменьшение+блоки 640x480 %.3f мс, 640x360 %.3f мс, 1280x720 %.3f мс на кадр\n",
             nf0 ? c->us_down / 1000.0 / (double)nf0 : 0, st[SEQ_360].frames ? c3->us_down / 1000.0 / (double)st[SEQ_360].frames : 0, nfh ? hd->us_down / 1000.0 / (double)nfh : 0);
    term_puts(b);
    snprintf(b, sizeof b, "anontest: память контекста: 640x480 %zu КБ, 640x360 %zu КБ, 1280x720 %zu КБ\n",
             (sizeof *c + 2u * (size_t)c->sw * c->sh + 2u * (size_t)c->bw * c->bh + 4u * (size_t)c->w) / 1024,
             (sizeof *c3 + 2u * (size_t)c3->sw * c3->sh + 2u * (size_t)c3->bw * c3->bh + 4u * (size_t)c3->w) / 1024,
             (sizeof *hd + 2u * (size_t)hd->sw * hd->sh + 2u * (size_t)hd->bw * hd->bh + 4u * (size_t)hd->w) / 1024);
    term_puts(b);
    /* ложные срабатывания: кадры без лица (шум, градиенты) */
    {
        long fp = 0, n = 0;
        anon_enable(c, false); anon_enable(c, true);
        for (int k = 0; k < 200; k++) {
            st_frame(Y, U, V, 640, 480, 640, 320, (unsigned)k);
            if (k % 2) for (int j = 0; j < 480; j++) for (int i = 0; i < 640; i++) Y[j * 640 + i] = (uint8_t)((i * 3 + j * (k % 7)) & 255);
            n++;
            if (anon_frame(c, Y, U, V, 640, 320, 640, 480) == ANON_SEND) fp++;
        }
        snprintf(b, sizeof b, "anontest: кадров без лица %ld, ушло как «лицо найдено» %ld\n", n, fp);
        term_puts(b);
    }
    anon_free(c); anon_free(c3); anon_free(hd);
    free(Y); free(U); free(V);
    return all.leak ? 2 : 0;
}
#endif
