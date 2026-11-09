/* share.h — демонстрация экрана «Astra DS Video» (streamtest.c): только при -DASTRA_VIDEO. */
#ifndef SHARE_H
#define SHARE_H
#ifdef ASTRA_VIDEO
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {
    bool active;                                 /* поток захвата работает */
    int w, h, fps;                               /* размер кадра и целевые к/с */
    double fps_out, mbps;                        /* отправлено за последнюю секунду */
    int frames, sent, dropped;
    long long bytes;
    char reason[96];                             /* почему остановилась (пусто — работает) */
} ShareState;
int share_windows(char (*titles)[80], uintptr_t *ids, int max);   /* окна-источники (видимые, с заголовком, не свои) */
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n);  /* mode 0 — 720p 24, 1 — 480p 15; hwnd 0 — весь экран */
void share_stop(void);
void share_state(ShareState *out);
#endif
#endif
