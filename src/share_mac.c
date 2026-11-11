/* share_mac.c - демонстрация экрана «Astra DS Video» на macOS. Интерфейс - share.h, тот же, что у streamtest.c
 * (Windows) и share_x11.c (Linux). Только при -DASTRA_VIDEO и только на macOS.
 *
 * Этап 1 переноса: захвата экрана на macOS пока нет (будет ScreenCaptureKit). share_start честно отказывает
 * понятным текстом тем же путём, каким Linux сообщает об ошибке запуска (false + err; ui.c переводит его tr()
 * и показывает ui_notice). Ничего не захватывается, потоков нет, памяти под кадры нет.
 *
 * Здесь же реализация кодера minih264: на Linux она в share_x11.c, на Windows в streamtest.c, а камера (cam.c)
 * и самопроверка декодера (viddec.c) берут только объявления. */
#if defined(ASTRA_VIDEO) && defined(__APPLE__)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "share.h"

#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#define MINIH264_IMPLEMENTATION
/* сторонний код: предупреждения выключены, -Werror к нему не относится (clang Apple - целиком, gcc - как в share_x11.c) */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
#include "../third_party/minih264/minih264e.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define SHM_UNSUPPORTED "демонстрация экрана на macOS пока не поддерживается"

static ShareState sh_last;                       /* только причина отказа: захват не запускается никогда */

bool share_wayland(void) { return false; }       /* выбор источника через портал - только Linux/Wayland */
int share_windows(char (*titles)[80], uintptr_t *ids, int max) {
    (void)titles; (void)ids; (void)max;
    return 0;                                    /* окон-источников нет: в списке останется «Весь экран» */
}
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n) {
    (void)mode; (void)hwnd;
    memset(&sh_last, 0, sizeof sh_last);
    snprintf(sh_last.reason, sizeof sh_last.reason, "%s", SHM_UNSUPPORTED);
    if (err && n) snprintf(err, n, "%s", SHM_UNSUPPORTED);
    return false;
}
void share_stop(void) {}
void share_stop_nowait(void) {}
void share_request_idr(void) {}
void share_audio_enable(bool on) { (void)on; }
void share_self_excluded(bool ok) { (void)ok; }
void share_state(ShareState *out) { if (out) *out = sh_last; }   /* active всегда false */
#endif
