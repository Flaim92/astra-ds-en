/* share.h — демонстрация экрана «Astra DS Video» (streamtest.c): только при -DASTRA_VIDEO. */
#ifndef SHARE_H
#define SHARE_H
#ifdef ASTRA_VIDEO
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {
    bool active;                                 /* поток захвата работает */
    bool paused;                                 /* нового кадра нет (свёрнуто, или перекрыто при BitBlt): зрители видят последний */
    bool minimized;                              /* окно-источник свёрнуто: поток идёт (повтор последнего кадра) */
    bool dib;                                    /* окно снимается PrintWindow (видно и под другими окнами) */
    bool audio;                                  /* звук транслируется (процесс окна / всё, кроме нас) */
    bool choosing;                               /* Wayland: открыто системное окно выбора источника (эфир ещё не начат) */
    char audio_err[64];                          /* почему без звука (пусто — со звуком или ещё запускается) */
    int w, h, fps;                               /* размер кадра и целевые к/с (известны с share_start) */
    int kbps_max;                                /* пик битрейта, кбит/с: max_bitrate в op 12 трансляции */
    double fps_out, mbps;                        /* отправлено за последнюю секунду */
    int frames, sent, dropped;
    long long bytes;
    char reason[96];
    char cover[64];
    char pause_why[80];                          /* при паузе: перекрыто / за краем / сдвинули во время захвата */                              /* при паузе: кто перекрыл — «программа.exe (класс окна)» */                             /* почему остановилась (пусто — работает) */
} ShareState;
/* Качество эфира (поле «КАЧЕСТВО» окна запуска, mode в share_start). Full HD - только с Nitro, как в Discord:
 * premium_type 1 (Nitro Classic) и 2 (Nitro); 3 (Nitro Basic) и 0 - до 720p. 60 к/с нет: кодер программный
 * (minih264, один поток) и такт захвата рассчитан на 30 к/с (share_conv.h); замеры - в streamtest.c, комментарий над sh_fit.
 * Битрейт 1080p: 1,8 Мбит/с x 2,25 (во столько больше точек) = 4 Мбит/с при тех же битах на точку; пик 6 Мбит/с
 * (запас ведра 2 Мбит против 1,7 у 720p: ключевой кадр 1080p крупнее). */
enum { SHARE_Q_720 = 0, SHARE_Q_720_ECO = 1, SHARE_Q_1080 = 2, SHARE_Q_N };
typedef struct { int w, h, fps, kbps, kbps_max; } ShareQuality;
static inline ShareQuality share_quality(int mode) {
    switch (mode) {
    case SHARE_Q_720_ECO: return (ShareQuality){ 1280, 720, 30, 1000, 2000 };
    case SHARE_Q_1080: return (ShareQuality){ 1920, 1080, 30, 4000, 6000 };
    default: return (ShareQuality){ 1280, 720, 30, 1800, 3500 };
    }
}
static inline bool share_quality_nitro(int premium_type) { return premium_type == 1 || premium_type == 2; }
/* режим доступен этому пользователю (premium_type из READY) */
static inline bool share_quality_allowed(int mode, int premium_type) {
    if (mode < 0 || mode >= SHARE_Q_N) return false;
    return mode != SHARE_Q_1080 || share_quality_nitro(premium_type);
}
int share_windows(char (*titles)[80], uintptr_t *ids, int max);   /* окна-источники (видимые, с заголовком, не свои) */
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n);  /* mode - SHARE_Q_* (Nitro проверяет вызывающий); hwnd 0 - весь экран */
void share_stop(void);                           /* ждёт поток захвата до конца (буферы затёрты) */
void share_stop_nowait(void);                    /* паника: флаг + ожидание не дольше 150 мс, без освобождения */
void share_request_idr(void);
void share_audio_enable(bool on);                /* до share_start: «Транслировать звук» (по умолчанию вкл) */                   /* Go Live: следующий кадр — ключевой (SPS+PPS+IDR) */
void share_self_excluded(bool ok);               /* term_capture_exclude сработал: свои окна — не перекрытие */
void share_state(ShareState *out);
#ifndef _WIN32
bool share_wayland(void);                        /* сеанс Wayland: источник выбирается в системном окне (xdg-desktop-portal) */
#endif
#endif
#endif
