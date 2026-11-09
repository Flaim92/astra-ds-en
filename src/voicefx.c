/* Смена голоса: сдвиг высоты тона на линии задержки (классический «двухголовочный» сдвигатель).
 *
 * Кольцевой буфер ~43 мс. Две головки чтения отстают от записи на переменную задержку
 * d = D0 + ph*W, где фаза ph равномерно растёт (голос ниже) или убывает (выше): скорость
 * изменения задержки (1 - r) даёт на выходе частоту r*f. Когда задержка у головки «перескакивает»
 * (ph проходит через 0/1), её вес равен нулю: вторая головка сдвинута на полпериода и в этот
 * момент звучит в полную силу. Веса g и 1-g (сглаженный треугольник) в сумме всегда 1, поэтому
 * выход не выходит за пределы входа.
 *
 * Стоимость: две выборки с линейной интерполяцией и десяток умножений на отсчёт, без БПФ, без
 * выделений памяти. Состояние статическое и принадлежит голосовому потоку; режим читается
 * атомарно один раз на кадр. Переход выкл <-> вкл плавный (перекрёстное затухание на кадр),
 * ниже <-> выше — без перескоков: фаза головок непрерывна. */
#include <stdatomic.h>
#include <string.h>
#include "mc.h"

#define FX_N 2048                                /* буфер, степень двойки (42,7 мс при 48 кГц) */
#define FX_M (FX_N - 1)
#define FX_W 1440.0f                             /* окно головки: 30 мс */
#define FX_D0 4.0f                               /* минимальная задержка: головка не догоняет запись */

static atomic_int g_mode;

void voicefx_set(VoiceFx m) {
    if ((int)m < VFX_OFF || (int)m > VFX_HIGH) m = VFX_OFF;
    atomic_store(&g_mode, (int)m);
}
VoiceFx voicefx_get(void) { return (VoiceFx)atomic_load(&g_mode); }
void voicefx_cycle(void) { voicefx_set((VoiceFx)((voicefx_get() + 1) % 3)); }

/* ---------- состояние голосового потока ---------- */
static float fx_buf[FX_N];
static unsigned fx_w;                            /* индекс записи */
static float fx_ph;                              /* фаза первой головки, [0, 1) */
static int fx_cur;                               /* режим, которым обработан прошлый кадр */

static float rate_of(int mode) {
    /* шаг фазы на отсчёт: (1 - r) / W */
    if (mode == VFX_LOW) return (1.0f - 0.8f) / FX_W;
    if (mode == VFX_HIGH) return (1.0f - 1.25f) / FX_W;
    return 0.0f;
}

/* Отсчёт x(t - d) с линейной интерполяцией; текущий x(t) уже записан по индексу fx_w.
 * Целая и дробная части задержки считаются отдельно: индекс не проходит через float. */
static inline float tap(float d) {
    unsigned di = (unsigned)d;                   /* d >= FX_D0 > 0 */
    float f = d - (float)di;
    float a = fx_buf[(fx_w - di) & FX_M], b = fx_buf[(fx_w - di - 1) & FX_M];
    return a + (b - a) * f;
}

/* Один отсчёт сдвигателя (вход уже записан в буфер). */
static inline float shift_one(float dp) {
    float p1 = fx_ph, p2 = fx_ph + 0.5f;
    if (p2 >= 1.0f) p2 -= 1.0f;
    float t = p1 < 0.5f ? p1 * 2.0f : (1.0f - p1) * 2.0f;   /* треугольник: 0 на перескоке, 1 посередине */
    float g = t * t * (3.0f - 2.0f * t);                    /* сглаживание; g(p) + g(p+1/2) = 1 */
    float y = g * tap(FX_D0 + p1 * FX_W) + (1.0f - g) * tap(FX_D0 + p2 * FX_W);
    fx_ph += dp;
    if (fx_ph >= 1.0f) fx_ph -= 1.0f;
    else if (fx_ph < 0.0f) fx_ph += 1.0f;
    return y;
}

static inline int16_t clip16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

void voicefx_process(int16_t *mono, int n) {
    int mode = atomic_load(&g_mode);
    int prev = fx_cur;
    fx_cur = mode;
    if (mode == VFX_OFF && prev == VFX_OFF) {
        /* выключено: только держим буфер свежим (копия кадра), чтобы включение было без мусора */
        for (int i = 0; i < n; i++) fx_buf[(fx_w + (unsigned)i) & FX_M] = (float)mono[i];
        fx_w += (unsigned)n;
        return;
    }
    if (prev == VFX_OFF) fx_ph = 0.0f;           /* старт: первая головка в нуле веса */
    /* выкл -> вкл: от сухого к обработанному; вкл -> выкл: обратно, со старым шагом фазы */
    int fade = (mode == VFX_OFF) != (prev == VFX_OFF) ? (mode == VFX_OFF ? -1 : 1) : 0;
    float dp = rate_of(mode == VFX_OFF ? prev : mode);
    float inv = n > 0 ? 1.0f / (float)n : 0.0f;
    for (int i = 0; i < n; i++) {
        float x = (float)mono[i];
        fx_buf[fx_w & FX_M] = x;
        float y = shift_one(dp);
        fx_w++;
        if (fade) {
            float a = (float)i * inv;            /* 0 -> 1 за кадр */
            if (fade < 0) a = 1.0f - a;
            y = x + (y - x) * a;
        }
        mono[i] = clip16(y);
    }
}
