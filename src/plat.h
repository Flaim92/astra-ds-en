/* Платформенный слой: всё, что отличается между Windows и Linux. Остальной код — переносимый C. */
#ifndef PLAT_H
#define PLAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ---------- ожидание событий ----------
 * Windows: h — HANDLE (ev не используется). Linux: h — дескриптор, ev — маска poll. */
typedef struct { intptr_t h; short ev; } Waitable;
#define W_IN 0x001
#define W_OUT 0x004
void plat_wait(const Waitable *w, int n, int timeout_ms);

typedef struct PlatWake PlatWake;                /* пробуждение из другого потока */
PlatWake *wake_new(void);
void wake_signal(PlatWake *w);
void wake_clear(PlatWake *w);
Waitable wake_waitable(PlatWake *w);
void wake_free(PlatWake *w);

typedef struct SockWait SockWait;                /* сокет как ожидаемый объект */
SockWait *sockwait_new(intptr_t sock);
Waitable sockwait_get(SockWait *s);
void sockwait_clear(SockWait *s);               /* перед чтением до EAGAIN */
void sockwait_free(SockWait *s);

/* ---------- время ---------- */
int64_t plat_mono_ms(void);
int64_t plat_wall_ms(void);
int64_t plat_timegm(struct tm *t);
void plat_localtime(int64_t unix_s, struct tm *out);
void plat_sleep_ms(int ms);

/* ---------- память ---------- */
void *plat_pages(size_t n);                     /* страницы прямо от системы */
void plat_pages_free(void *p, size_t n);
void plat_lock(void *p, size_t n);              /* не в подкачку и не в дампы */
void plat_unlock(void *p, size_t n);
void plat_wipe(void *p, size_t n);
size_t plat_private_bytes(void);               /* частная память процесса */
int64_t plat_cpu_ms(void);
void plat_trim(void);                           /* вернуть системе свободную память кучи */                      /* процессорное время процесса */

/* ---------- процесс ---------- */
void plat_harden(void (*on_crash)(void));       /* без дампов, без чужих DLL; on_crash затирает секреты */
void plat_net_init(void);
const char *plat_ca_file(void);                 /* NULL — системное хранилище (Windows) */

/* ---------- потоки ---------- */
typedef struct PlatThread PlatThread;
PlatThread *thread_start(int (*fn)(void *), void *arg);
void thread_join(PlatThread *t);
void thread_audio_priority(void);
typedef struct PlatMutex PlatMutex;
PlatMutex *mutex_new(void);
void mutex_lock(PlatMutex *m);
void mutex_unlock(PlatMutex *m);
void mutex_free(PlatMutex *m);

/* ---------- UDP ---------- */
intptr_t udp_open(void);                        /* -1 при ошибке; неблокирующий */
bool udp_resolve(const char *ip, uint16_t port, void *addr16);  /* только IPv4-литерал */
int udp_send(intptr_t s, const void *buf, size_t n, const void *addr16);
int udp_recv(intptr_t s, void *buf, size_t n);  /* -1 — данных нет */
void udp_close(intptr_t s);

/* ---------- хранилище секретов ----------
 * entry — имя записи: "token" (токен) или "vless" (ссылка VLESS).
 * Windows: Диспетчер учётных данных, цель mincord/<entry>; Linux: secret-tool, account <entry>. */
int keyring_load(const char *entry, char *out, size_t cap);      /* длина; -1 — записи нет; -2 — ошибка чтения */
void keyring_readonly(bool on);                 /* --no-save: миграцию старых записей не записывать */
bool keyring_save(const char *entry, const char *data, size_t n);
void keyring_delete(const char *entry);
void keyring_test_mode(void);                  /* отдельные записи для самопроверки */
bool keyring_save_old(const char *entry, const char *data, size_t n);         /* самопроверка: запись под старым именем mincord */
int keyring_load_raw(const char *entry, bool old, char *out, size_t cap);     /* самопроверка: чтение без миграции */
const char *keyring_name(void);

/* ---------- дочерняя программа (Xray) ----------
 * Файл программы открывается так, чтобы проверенное содержимое нельзя было подменить до запуска:
 * Windows — с запретом записи и удаления, пока открыт; Linux — запуск через fexecve по тому же fd. */
typedef struct PlatFile PlatFile;
PlatFile *exe_open(const char *name, char *path, size_t pathn);  /* name — в каталоге mincord; NULL — нет файла */
long exe_read(PlatFile *f, void *buf, size_t n);                 /* -1 ошибка, 0 конец */
void exe_close(PlatFile *f);
typedef struct PlatProc PlatProc;
/* Запустить проверенный файл: argv[0] — имя, далее аргументы. stdin получает in[0..n) и
 * закрывается, stdout/stderr — в никуда, окружение пустое. Процесс умирает вместе с mincord
 * даже при его сбое (Windows — Job Object, Linux — PR_SET_PDEATHSIG). */
PlatProc *proc_spawn(PlatFile *f, const char *const *argv, const void *in, size_t n);
bool proc_alive(PlatProc *p);
Waitable proc_waitable(PlatProc *p);           /* срабатывает, когда процесс завершился */
void proc_kill(PlatProc *p);                   /* убить, дождаться и освободить */
long proc_rss_kb(PlatProc *p);                 /* резидентная память процесса, -1 нет данных */
int tcp_free_port(void);                       /* свободный порт на 127.0.0.1, -1 ошибка */
int tcp_local_ready(int port, int timeout_ms); /* 127.0.0.1:port принимает? 1 да, 0 нет, -1 ошибка */

/* ---------- терминал ---------- */
enum {
    K_NONE, K_CHAR, K_ENTER, K_TAB, K_BTAB, K_BS, K_DEL, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_ESC, K_INS, K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8,
    K_F9, K_F10, K_F11, K_F12, K_RESIZE,
    K_MOUSE                                     /* мышь: x, y — клетка (с 0), button — см. ниже */
};
/* Мышь (K_MOUSE): button 1 — левая, 2 — средняя, 3 — правая (нажатие); 4/5 — колесо вверх/вниз;
 * 0 — перемещение (наведение; окно шлёт только при смене клетки, терминал — может не слать). */
typedef struct { int code; uint32_t ch; bool ctrl, alt, shift; int x, y, button; } Key;
/* До term_init: скрытый ввод строки (без эха и истории). Пробелы/кавычки/табуляции выбрасываются,
 * пустая строка — true и "". false — нет консоли, конец ввода или строка не влезла в cap. */
bool term_read_secret(const char *prompt, char *out, size_t cap);
bool term_interactive(void);                    /* stdin — консоль (можно спросить пользователя) */
void term_puts(const char *s);                  /* обычный вывод до/после интерфейса */
bool term_init(void);
void term_shutdown(void);
void term_size(int *w, int *h);
void term_write(const char *s, size_t n);
int term_waitables(Waitable *out, int max);
int term_read(Key *out, int max);               /* неблокирующее */

#endif
