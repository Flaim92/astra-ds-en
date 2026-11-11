/* share_pw.h — приём кадров PipeWire для демонстрации экрана в Wayland (share_pw.c), только «Astra DS Video» на Linux.
 * Поток выдаёт xdg-desktop-portal (portal_wl.c): fd удалённого PipeWire и номер узла. libpipewire-0.3 подгружается
 * через dlopen; без неё — понятная ошибка. Кадры — 32 бита на точку (BGRx/BGRA/RGBx/RGBA): общая память
 * (MemPtr/MemFd) или линейный DMA-BUF (модификатор LINEAR, mmap только для чтения, без GPU). Кадр не копируется:
 * читается прямо из буфера PipeWire, пока держится замок shpw_lock(). Ни файлов, ни вывода. */
#ifndef SHARE_PW_H
#define SHARE_PW_H
#if defined(ASTRA_VIDEO) && !defined(_WIN32) && !defined(__APPLE__)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SHPW_NPAR 5                              /* EnumFormat (общая память), EnumFormat (LINEAR DMA-BUF), Buffers x2, Meta */

typedef struct {
    const uint8_t *data;                         /* первая точка картинки (уже со сдвигом обрезки) */
    int w, h;                                    /* размер картинки (после обрезки VideoCrop) */
    size_t stride;                               /* байт на строку */
    int rgb;                                     /* 1 — в памяти R,G,B,x; 0 — B,G,R,x */
    int uncached;                                /* DMA-BUF: отображение некэшируемое (write-combined) — читать
                                                  * строки только через shpw_row() */
} ShPwFrame;

bool shpw_load(char *err, size_t n);             /* libpipewire-0.3 есть и в ней все нужные функции */
/* Подключиться к PipeWire портала (fd забирается всегда: закрывается здесь или самим PipeWire) и к узлу node. */
bool shpw_open(int fd, uint32_t node, char *err, size_t n);
/* Замок цикла PipeWire: пока он взят, буфер кадра не вернётся источнику. После shpw_lock — всегда shpw_unlock.
 * Вернуть: 1 — кадр есть (*f, *seq — номер кадра, растёт с каждым новым), 0 — кадра ещё нет,
 * -1 — поток закончился (источник закрыт, отказ PipeWire), -2 — формат картинки не поддерживается,
 * -3 — источник отдаёт только буферы видеокарты в нелинейном формате (без GPU не прочитать). */
int shpw_lock(ShPwFrame *f, uint32_t *seq);
void shpw_unlock(void);
void shpw_close(void);                           /* закрыть поток и соединение; можно звать и без shpw_open */
/* Строка некэшируемого кадра (uncached): байты [a,b) строки src (src + b — внутри кадра) копируются потоковыми
 * загрузками (SSE4.1 MOVNTDQA; без него — memcpy) в свой кэшируемый буфер строки. Вернуть указатель r: r[i] == src[i]
 * для i из [a,b), выравнивание r по 4 — как у src. Только под shpw_lock; нет памяти — вернуть src (медленно, но верно). */
const uint8_t *shpw_row(const uint8_t *src, size_t a, size_t b);

/* ---- для самопроверки (без PipeWire) ---- */
/* Параметры для pw_stream_connect / update_params (SHPW_NPAR штук, порядок — как у SHPW_NPAR) — в buf. */
int shpw_build_params(uint8_t *buf, size_t cap, const void *out[SHPW_NPAR], size_t *len);
/* Разбор согласованного SPA_PARAM_Format: формат точки, размер, *dma — линейный DMA-BUF (модификатор LINEAR).
 * 1 — подходит; 0 — не видео/raw, не 32 бита, мусор; -1 — модификатор нелинейный (нужен GPU). */
int shpw_parse_format(const void *pod, size_t n, uint32_t *w, uint32_t *h, int *rgb, int *dma);
/* Проверка буфера от источника: тип памяти, обрезка, stride, размеры и границы. true — *f готов к чтению. */
bool shpw_check_frame(uint32_t fw, uint32_t fh, int rgb, uint32_t dtype, const void *ptr, uint32_t maxsize,
                      uint32_t off, uint32_t size, int32_t stride, int32_t flags,
                      bool has_crop, int32_t cx, int32_t cy, uint32_t cw, uint32_t chh, ShPwFrame *f);
/* То же в заданный буфер dst16 (выровнен по 16, не меньше b + 32 байт); nt — потоковые загрузки, если есть SSE4.1. */
const uint8_t *shpw_row_copy(uint8_t *dst16, const uint8_t *src, size_t a, size_t b, int nt);
int share_pw_selftest(void (*check)(bool ok, const char *what));
#endif
#endif
