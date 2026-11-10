/* portal_wl.h - демонстрация экрана под Wayland: xdg-desktop-portal ScreenCast (только Linux, «Astra DS Video»).
 * libdbus-1 грузится через dlopen: без неё бинарник работает, portal_wl_available() == false.
 * Порядок (поток захвата): portal_wl_open -> pw_context_connect_fd(pw_fd) + узел node -> ... ->
 * portal_wl_close (ровно один раз в конце, в том числе после неудачного portal_wl_open).
 * pw_fd принадлежит вызывающему (PipeWire закроет его сам); portal_wl_close его не трогает.
 * Сессия портала живёт, пока открыто соединение D-Bus, то есть до portal_wl_close. */
#ifndef PORTAL_WL_H
#define PORTAL_WL_H
#if defined(ASTRA_VIDEO) && !defined(_WIN32)
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {
    int pw_fd;                                   /* сокет PipeWire от OpenPipeWireRemote (-1 - нет) */
    uint32_t node;                               /* узел PipeWire выбранного источника */
    int w, h;                                    /* размер из ответа портала; 0 - не сообщён (брать из формата потока) */
    int source_type;                             /* 1 - монитор, 2 - окно, 4 - виртуальный; 0 - не сообщён */
} PortalStream;
/* Дёшево, для потока интерфейса: сеанс Wayland (WAYLAND_DISPLAY или XDG_SESSION_TYPE=wayland) и загрузилась
 * libdbus-1. Наличие портала на шине здесь не проверяется: его нет -> portal_wl_open вернёт понятную ошибку. */
bool portal_wl_available(void);
/* Блокирующий: системное окно выбора ждём до 10 мин; *stop опрашивается каждые ~100 мс
 * (*stop != 0 -> false, «трансляция остановлена»). Отмена в окне -> false, ровно «выбор отменён».
 * Отказ бэкенда до окна выбора (перезапуск после падения) - один повтор всего запроса через ~1,5 с. */
bool portal_wl_open(PortalStream *out, volatile int *stop, char *err, size_t n);
bool portal_wl_closed(void);                     /* неблокирующе: компоновщик закрыл сессию (Session::Closed) или шина оборвалась */
void portal_wl_close(void);                      /* Session.Close и закрыть соединение; идемпотентно */
int portal_wl_selftest(void (*check)(bool ok, const char *what));   /* --selftest: чистые функции и разбор ответов */
#endif
#endif
