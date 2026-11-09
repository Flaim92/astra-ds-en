/* tls13.h — клиент TLS 1.3 с ClientHello как у Chrome (uTLS HelloChrome_Auto = Chrome 133) и Reality.
 * Только для туннеля на C (xlite_c.c, сборка VLESS_IMPL=c). Без сокетов: байты из сети и в сеть
 * передаются буферами, вызывающий сам ждёт событий. Вся криптография — примитивы OpenSSL,
 * распаковка сжатых сертификатов — декодер google/brotli. */
#ifndef TLS13_H
#define TLS13_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Буфер байт: данные — p[off..len). Память — через OpenSSL (затирается при освобождении). */
typedef struct { unsigned char *p; size_t off, len, cap; } Bb;
#define BB_MAX (1u << 20)                       /* больше 1 МБ в одном буфере — ошибка */
bool bb_reserve(Bb *b, size_t n);               /* свободно не меньше n байт в конце */
bool bb_put(Bb *b, const void *d, size_t n);
void bb_eat(Bb *b, size_t n);                   /* снять n байт с начала */
void bb_free(Bb *b);                            /* затереть и освободить */
static inline unsigned char *bb_ptr(const Bb *b) { return b->p + b->off; }
static inline size_t bb_n(const Bb *b) { return b->len - b->off; }

enum { TLS_ALPN_CHROME = 0, TLS_ALPN_HTTP11 = 1 };
typedef struct {
    const char *sni;                            /* "" — без SNI (адрес-литерал) */
    const char *verify_ip;                      /* обычный TLS без SNI: сверять сертификат с этим IP */
    bool reality;
    unsigned char pbk[32];                      /* Reality: открытый ключ X25519 сервера */
    unsigned char sid[8];                       /* Reality: shortId */
    const unsigned char *pqv;                   /* Reality: ключ ML-DSA-65 (1952 байта) или NULL */
    int alpn;                                   /* что объявлять в ClientHello */
    bool http11_only;                           /* WebSocket: сервер не должен выбрать h2 */
} TlsCfg;

typedef struct Tls Tls;
bool t13_global_init(void);
void t13_global_free(void);
/* Новый клиент: ClientHello дописывается в net. NULL — ошибка. cfg должен жить, пока жив Tls. */
Tls *t13_new(const TlsCfg *cfg, Bb *net);
/* Разобрать целые записи из in (съедаются): данные приложения — в app, ответы рукопожатия — в net.
 * 0 — ок (возможно, ждём ещё байт), 1 — сервер закрыл (close_notify), -1 — ошибка. */
int t13_input(Tls *t, Bb *in, Bb *app, Bb *net);
bool t13_ready(const Tls *t);
bool t13_seal(Tls *t, const void *p, size_t n, Bb *net);   /* данные приложения в записи */
const char *t13_why(const Tls *t);              /* причина ошибки (для проверок), без секретов */
void t13_free(Tls *t);

#endif
