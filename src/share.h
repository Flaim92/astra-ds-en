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
    int w, h, fps;                               /* размер кадра и целевые к/с */
    double fps_out, mbps;                        /* отправлено за последнюю секунду */
    int frames, sent, dropped;
    long long bytes;
    char reason[96];
    char cover[64];
    char pause_why[80];                          /* при паузе: перекрыто / за краем / сдвинули во время захвата */                              /* при паузе: кто перекрыл — «программа.exe (класс окна)» */                             /* почему остановилась (пусто — работает) */
} ShareState;
int share_windows(char (*titles)[80], uintptr_t *ids, int max);   /* окна-источники (видимые, с заголовком, не свои) */
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n);  /* всегда 1280x720@30; mode 0 — 1,8 Мбит/с, 1 — 1 Мбит/с; hwnd 0 — весь экран */
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
