/* cam_mac.h - захват веб-камеры на macOS через AVFoundation (cam_mac.m), только «Astra DS Video».
 * Договор между cam.c (переносимый C) и cam_mac.m (Objective-C, ARC): здесь только типы C, без mc.h и Foundation,
 * чтобы заголовок безопасно подключался с обеих сторон. Строк для человека тут нет: cam_mac.m отдаёт коды
 * CAMMAC_E_*, а текст (переведённый ui_tr) выбирает cam.c.
 *
 * cam.c пользуется этим только при -DASTRA_CAM_MAC (build.sh добавляет его вместе с src/cam_mac.m);
 * без него камера на macOS честно отказывает: «камера на macOS пока не поддерживается».
 *
 * Все функции, кроме cammac_list, - из одного потока (поток камеры cam.c). Кадры всегда NV12 (420v, видеодиапазон):
 * плоскость Y, на uv_off байт дальше - UV вперемежку с тем же шагом stride (как ждёт cvt_nv12 в cam.c). */
#ifndef CAM_MAC_H
#define CAM_MAC_H
#if defined(ASTRA_VIDEO) && defined(__APPLE__)
#include <stddef.h>
#include <stdint.h>

enum {
    CAMMAC_OK = 0,
    CAMMAC_E_OTHER,                              /* прочий сбой: подробность - в *code (NSError.code) */
    CAMMAC_E_DENIED,                             /* нет разрешения (Конфиденциальность и безопасность -> Камера) */
    CAMMAC_E_BUSY,                               /* камера занята другой программой */
    CAMMAC_E_GONE,                               /* камера отключена (или сессия захвата оборвалась) */
    CAMMAC_E_NOTFOUND,                           /* устройства с таким id нет */
    CAMMAC_E_FORMAT,                             /* камера не отдаёт кадры NV12 */
    CAMMAC_E_NOFRAMES,                           /* открылась, но кадров нет (5 с) */
};

typedef struct CamMac CamMac;                    /* непрозрачный: объект Objective-C за CFBridgingRetain */
typedef struct { int w, h; double fps; } CamMacMode;
typedef struct {
    const uint8_t *p;                            /* начало плоскости Y */
    size_t n;                                    /* байт, доступных от p (до конца UV) */
    int stride;                                  /* шаг строки Y и UV, байт */
    size_t uv_off;                               /* смещение плоскости UV от p */
    int w, h;                                    /* размер кадра */
} CamMacFrame;

/* Перечень камер: add(u, id, name) для каждой (id - AVCaptureDevice.uniqueID, name - localizedName, UTF-8).
 * Разрешения не требует, камеру не включает. Вернуть число камер. */
int cammac_list(void (*add)(void *u, const char *id, const char *name), void *u);
/* Открыть камеру id и запустить захват. maxw x maxh - наибольший кадр эфира (cam.c CAM_MAXW/CAM_MAXH): кадр
 * сразу приходит вписанным в него (масштабирует AVFoundation). Ждёт первый кадр (до 5 с) и сообщает его настоящий
 * размер в *m. Первый раз может спросить разрешение у системы (ждём ответа до 2 мин). NULL - *err = CAMMAC_E_*,
 * *code - код NSError (0 - нет). */
CamMac *cammac_open(const char *id, int maxw, int maxh, CamMacMode *m, int *err, int *code);
/* Следующий кадр, ждать не дольше timeout_ms. 1 - кадр в *f (действует до cammac_done или следующего cammac_read),
 * 0 - пока нет, -1 - сбой (*err = CAMMAC_E_*). */
int cammac_read(CamMac *c, CamMacFrame *f, int timeout_ms, int *err);
void cammac_done(CamMac *c);                     /* кадр больше не нужен (вернуть буфер AVFoundation) */
void cammac_close(CamMac *c);                    /* остановить захват, затереть свои буферы; NULL - можно */
#endif
#endif
