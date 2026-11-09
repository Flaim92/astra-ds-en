/* VLESS: разбор ссылки, запуск туннеля внутри mincord, локальный SOCKS5, проверка туннеля.
 *
 * Как устроено:
 * - Ссылка (в ней UUID — ключ доступа к серверу) и всё, что из неё выведено, живут в закреплённой
 *   области secret.c; в системном хранилище — отдельная запись "vless" (с --no-save — не пишется).
 * - Туннель вкомпонован в mincord: по умолчанию свой на C (src/xlite_c.c + src/tls13.c, сборка
 *   VLESS_IMPL=c), вариант — Xray-core на Go (tools/xlite, VLESS_IMPL=go). Контракт один:
 *   XliteStart/XliteStop; конфигурация — строки "ключ=значение" в памяти, на диск ничего, журналов нет.
 * - Вход туннеля — SOCKS5 на 127.0.0.1 со случайными логином и паролем на каждый запуск: чужие
 *   программы на этом компьютере туннелем не воспользуются. UDP включён (голос).
 * - Проверка: GET https://discord.com/api/v9/gateway через туннель, дважды по одному соединению;
 *   задержка — время второго запроса (рукопожатия уже позади, это чистый путь до Discord и назад).
 * - Состояния: OFF (VLESS не используется) -> STARTING (туннель поднят, идёт проверка) -> UP;
 *   DOWN — не работает: сеть (net.c) тогда ждёт и напрямую не ходит. В DOWN модуль сам пробует
 *   восстановиться с нарастающим интервалом (перезапуск туннеля, проверка), пока пользователь не
 *   выберет другое (vless_restart / vless_set_link / vless_disable). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "mc.h"
#include "plat.h"

/* Временно здесь, до переноса в mc.h координатором: */
void *secret_vless_area(size_t *cap);           /* secret.c: закреплённая область для VLESS */
void net_curl_common(CURL *e);                  /* net.c: общие настройки TLS/протоколов для хэндла */
i64 vless_next_wake(void);                      /* главный цикл: когда разбудить vless_service */
int vless_waitables(Waitable *out, int max);    /* главный цикл: события туннеля (сейчас нет) */
long vless_xray_rss_kb(void);                   /* --vlesstest: память отдельного процесса туннеля (-1: его нет) */

/* ---------- туннель ---------- */
/* VLESS_C (VLESS_IMPL=c, по умолчанию): свой туннель на C (src/xlite_c.c, src/tls13.c) — SOCKS5-вход
 * и VLESS-выход (Reality/TLS 1.3 с ClientHello как у Chrome, TCP/WS), один поток, примитивы OpenSSL.
 * Иначе (VLESS_IMPL=go): xlite — официальные исходники Xray-core v26.10.31 (коммит a4a3dff),
 * go build -buildmode=c-archive (tools/xlite/main.go). Оба вкомпонованы в mincord: отдельного
 * исполняемого файла, его поиска и проверки хэша нет. */
#ifdef VLESS_C
#define XRAY_VERSION "туннель на C (VLESS, Reality/TLS 1.3)"
void XliteWipe(void);                           /* паника: затереть ссылку и ключи сессий в туннеле */
int XliteFailed(void);                          /* поток туннеля умер */
#else
#define XRAY_VERSION "xlite (Xray-core v26.10.31)"
static void XliteWipe(void) {}                  /* Go: память среды Go отсюда не затереть */
static int XliteFailed(void) { return 0; }      /* Go: «умереть сам» не может (паника Go — весь mincord) */
#endif
int XliteStart(char *cfg, int n);               /* 0 — работает; 1 — параметры; 2 — не создан (DNS?); 3 — порт */
void XliteStop(void);

#define LINK_MAX 4000
#define PROBE_URL "https://discord.com/api/v9/gateway"
#define FAILS_TO_DOWN 3                         /* столько сбоев подряд от сети/голоса — туннель не работает */

enum { SEC_TLS = 1, SEC_REALITY = 2 };
enum { NET_TCP = 1, NET_WS = 2 };

/* Всё секретное — в закреплённой области (secret_vless_area). */
typedef struct {
    char link[LINK_MAX + 1];                    /* ссылка без #имени */
    char q[LINK_MAX + 1];                       /* разбор: раскодированные значения параметров */
    char uuid[37];
    char host[256];
    bool host_v6;
    int port;
    int sec, net;
    char enc[256];
    char flow[32];
    char sni[256];
    char fp[16];
    char pbk[48];
    char sid[20];
    char spx[256];
    char pqv[3072];
    char alpn[64];
    char ws_host[256];
    char ws_path[512];
    char user[33], pass[33];
    char proxy[128];                            /* socks5h://логин:пароль@127.0.0.1:порт */
} VSec;

static VSec *K;

static struct {
    VlessState st;
    bool have;                                  /* в K есть разобранная ссылка */
    bool fatal;                                 /* ссылка не подходит туннелю: сами не повторяем */
    char err[512];
    int latency;
    int fails;                                  /* сбоев подряд от сети/голоса */
    bool running;                               /* туннель xlite запущен в этом процессе */
    int port;
    /* восстановление в DOWN */
    i64 next_try;                               /* 0 — не пробовать самим */
    int backoff;
    i64 deaths_since;
    int deaths;
} V = { .latency = -1 };

/* ---------- проверка туннеля (curl_multi, без блокировки главного цикла) ---------- */
enum { PR_IDLE, PR_GET1, PR_GET2, PR_DIAG };
static struct {
    CURLM *m;
    CURL *e;
    int stage;
    Buf body;
    i64 t1_us;                                  /* время первого запроса */
    char fail[512];
    bool ok;
} P;

static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(V.err, sizeof V.err, fmt, ap);
    va_end(ap);
}

static bool area(void) {
    if (K) return true;
    size_t cap = 0;
    void *p = secret_vless_area(&cap);
    if (!p || cap < sizeof(VSec)) return false;
    K = p;
    return true;
}

static void wake_all(void) {
    if (g_wake) wake_signal(g_wake);
    ui_dirty();
}

/* ======================= разбор ссылки ======================= */
static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hexv(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool valid_uuid(const char *s, size_t n) {
    if (n != 36) return false;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!is_hex(s[i])) return false;
    }
    return true;
}

static bool valid_ipv4(const char *s) {
    int parts = 0;
    while (*s) {
        int v = 0, d = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (++d > 3 || v > 255) return false; s++; }
        if (!d || (d > 1 && s[-d] == '0')) return false;           /* без ведущих нулей (восьмеричное) */
        parts++;
        if (*s == '.') { s++; if (!*s) return false; }
        else if (*s) return false;
    }
    return parts == 4;
}

static bool valid_domain(const char *s) {
    size_t n = strlen(s), lab = 0;
    if (n == 0 || n > 253) return false;
    bool alpha = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            if (lab == 0 || s[i - 1] == '-') return false;
            lab = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        if (c == '-' && lab == 0) return false;
        if (c >= 'a' && c <= 'z') alpha = true;
        if (++lab > 63) return false;
    }
    if (lab == 0 || s[n - 1] == '-') return false;
    return alpha;                                /* "1.2.3.4" с ошибкой — не домен */
}

static bool valid_ipv6(const char *s) {
    size_t n = strlen(s);
    int colons = 0;
    if (n < 2 || n > 45) return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ':') colons++;
        else if (!is_hex(s[i]) && s[i] != '.') return false;
    }
    return colons >= 2 && colons <= 7 && !strstr(s, ":::");
}

/* Значение параметра: только печатный ASCII без кавычек, обратной косой черты и пробела. */
static bool printable(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < 0x21 || (unsigned char)*s > 0x7E || *s == '"' || *s == '\\') return false;
    return true;
}
static bool charset(const char *s, const char *extra) {
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(extra, c))) return false;
    }
    return true;
}

static bool pct_decode(const char *s, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%') {
            if (i + 2 >= n || !is_hex(s[i + 1]) || !is_hex(s[i + 2])) return false;
            c = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) return false;   /* управляющие — нет */
            i += 2;
        }
        if (k + 1 >= cap) return false;
        out[k++] = c;
    }
    out[k] = 0;
    return true;
}

typedef struct { const char *k; size_t kn; char *v; } Param;

static bool cpy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

/* Разобрать ссылку в K. false — err по-русски. Ссылка строго проверяется: всё неизвестное — отказ. */
static bool parse_link(const char *link, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); return false; } while (0)
    size_t full = strlen(link);
    size_t n = strcspn(link, "#");               /* #имя — подпись для людей, отбрасываем */
    if (n < 8 || strncmp(link, "vless://", 8)) {
        if (n >= 8 && !strncmp(link, "VLESS://", 8)) {} else FAIL(ui_tr("это не ссылка vless:// (должна начинаться с vless://)"));
    }
    if (n > LINK_MAX || full > LINK_MAX * 2) FAIL(ui_tr("ссылка слишком длинная (больше %d символов)"), LINK_MAX);
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)link[i] < 0x21 || (unsigned char)link[i] > 0x7E) FAIL(ui_tr("в ссылке недопустимый символ (позиция %zu)"), i + 1);
    VSec *k = K;
    /* сначала копия: link может указывать в саму область (загрузка из хранилища) */
    memmove(k->link, link, n);
    k->link[n] = 0;
    memcpy(k->link, "vless://", 8);
    const char *p = k->link + 8;
    /* UUID@ */
    const char *at = strchr(p, '@');
    if (!at) FAIL(ui_tr("в ссылке нет UUID@сервер"));
    if (!valid_uuid(p, (size_t)(at - p))) FAIL(ui_tr("UUID в ссылке неверного формата (нужно 8-4-4-4-12 шестнадцатеричных цифр)"));
    for (int i = 0; i < 36; i++) k->uuid[i] = lower(p[i]);
    k->uuid[36] = 0;
    p = at + 1;
    /* сервер:порт */
    size_t hl;
    k->host_v6 = false;
    if (*p == '[') {
        const char *e = strchr(p, ']');
        if (!e || (size_t)(e - p - 1) >= sizeof k->host) FAIL(ui_tr("адрес IPv6 в ссылке не закрыт ']'"));
        hl = (size_t)(e - p - 1);
        memcpy(k->host, p + 1, hl);
        k->host[hl] = 0;
        for (size_t i = 0; i < hl; i++) k->host[i] = lower(k->host[i]);
        if (!valid_ipv6(k->host)) FAIL(ui_tr("неверный адрес IPv6 сервера"));
        k->host_v6 = true;
        p = e + 1;
    } else {
        hl = strcspn(p, ":/?");
        if (hl == 0 || hl >= sizeof k->host) FAIL(ui_tr("в ссылке нет адреса сервера"));
        for (size_t i = 0; i < hl; i++) k->host[i] = lower(p[i]);
        k->host[hl] = 0;
        if (!valid_ipv4(k->host) && !valid_domain(k->host)) FAIL(ui_tr("адрес сервера должен быть доменом, IPv4 или [IPv6]"));
        p += hl;
    }
    if (*p != ':') FAIL(ui_tr("в ссылке нет порта сервера"));
    p++;
    long port = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); if (++digits > 5) break; p++; }
    if (!digits || digits > 5 || port < 1 || port > 65535) FAIL(ui_tr("порт сервера должен быть числом 1-65535"));
    k->port = (int)port;
    if (*p == '/') p++;
    if (*p && *p != '?') FAIL(ui_tr("после порта ожидаются параметры ?..."));
    if (*p == '?') p++;
    /* параметры */
    Param ps[32];
    int np = 0;
    size_t qo = 0;
    while (*p) {
        size_t len = strcspn(p, "&");
        if (len) {
            const char *eq = memchr(p, '=', len);
            if (!eq) FAIL(ui_tr("параметр без значения: %.*s"), (int)(len > 40 ? 40 : len), p);
            if (np == 32) FAIL(ui_tr("слишком много параметров в ссылке"));
            size_t kn = (size_t)(eq - p);
            for (int i = 0; i < np; i++)
                if (ps[i].kn == kn && !memcmp(ps[i].k, p, kn)) FAIL(ui_tr("параметр повторяется: %.*s"), (int)(kn > 40 ? 40 : kn), p);
            ps[np].k = p;
            ps[np].kn = kn;
            ps[np].v = k->q + qo;
            if (!pct_decode(eq + 1, len - kn - 1, k->q + qo, sizeof k->q - qo))
                FAIL(ui_tr("неверная %%-кодировка в параметре %.*s"), (int)(kn > 40 ? 40 : kn), p);
            qo += strlen(k->q + qo) + 1;
            np++;
        }
        p += len;
        if (*p == '&') p++;
    }
    static const char *const known[] = {
        "type", "security", "encryption", "flow", "sni", "fp", "pbk", "sid", "spx", "pqv",
        "support-x25519mlkem768", "alpn", "host", "path", "headerType", "allowInsecure", "insecure", "packetEncoding",
    };
    const char *val[sizeof known / sizeof *known] = {0};
    for (int i = 0; i < np; i++) {
        size_t j = 0;
        for (; j < sizeof known / sizeof *known; j++) if (strlen(known[j]) == ps[i].kn && !memcmp(known[j], ps[i].k, ps[i].kn)) break;
        if (j == sizeof known / sizeof *known) FAIL(ui_tr("неизвестный параметр ссылки: %.*s"), (int)(ps[i].kn > 40 ? 40 : ps[i].kn), ps[i].k);
        val[j] = ps[i].v[0] ? ps[i].v : NULL;   /* пустое значение = параметра нет */
    }
    const char *type = val[0], *security = val[1], *encryption = val[2], *flow = val[3], *sni = val[4], *fp = val[5],
               *pbk = val[6], *sid = val[7], *spx = val[8], *pqv = val[9], *mlkem = val[10], *alpn = val[11],
               *host = val[12], *path = val[13], *header = val[14], *insec = val[15], *insec2 = val[16], *pe = val[17];
    /* транспорт */
    if (!type || !strcmp(type, "tcp") || !strcmp(type, "raw")) k->net = NET_TCP;
    else if (!strcmp(type, "ws")) k->net = NET_WS;
    else FAIL(ui_tr("транспорт type=%.24s не поддерживается (только tcp и ws)"), type);
    /* защита */
    if (!security || !strcmp(security, "none"))
        FAIL(ui_tr("security=none: VLESS без TLS передаёт всё открытым текстом - такая ссылка не принимается"));
    if (!strcmp(security, "reality")) k->sec = SEC_REALITY;
    else if (!strcmp(security, "tls")) k->sec = SEC_TLS;
    else FAIL(ui_tr("security=%.24s не поддерживается (только reality и tls)"), security);
    if (k->sec == SEC_REALITY && k->net != NET_TCP) FAIL(ui_tr("Reality работает только с type=tcp"));
    if ((insec && strcmp(insec, "0") && strcmp(insec, "false")) || (insec2 && strcmp(insec2, "0") && strcmp(insec2, "false")))
        FAIL(ui_tr("allowInsecure=1 отключает проверку сертификата сервера - такая ссылка не принимается"));
    if (header && strcmp(header, "none")) FAIL(ui_tr("headerType=%.24s не поддерживается"), header);
    if (pe && strcmp(pe, "xudp") && strcmp(pe, "none")) FAIL(ui_tr("packetEncoding=%.24s не поддерживается"), pe);
    if (mlkem && strcmp(mlkem, "true") && strcmp(mlkem, "false") && strcmp(mlkem, "1") && strcmp(mlkem, "0"))
        FAIL(ui_tr("support-x25519mlkem768 должен быть true или false"));
    /* шифрование VLESS: none или собственное шифрование Xray (mlkem768x25519plus...) */
    if (!encryption) encryption = "none";
    if (!charset(encryption, "._-") || !cpy(k->enc, sizeof k->enc, encryption)) FAIL(ui_tr("неверный параметр encryption"));
    /* flow */
    k->flow[0] = 0;
    if (flow) {
        if (strcmp(flow, "xtls-rprx-vision") && strcmp(flow, "xtls-rprx-vision-udp443")) FAIL(ui_tr("flow=%.32s не поддерживается"), flow);
        if (k->net != NET_TCP) FAIL(ui_tr("flow=xtls-rprx-vision работает только с type=tcp"));
        cpy(k->flow, sizeof k->flow, flow);
    }
    /* SNI */
    k->sni[0] = 0;
    if (sni) {
        char t[256];
        size_t sl = strlen(sni);
        if (sl >= sizeof t) FAIL(ui_tr("sni слишком длинный"));
        for (size_t i = 0; i <= sl; i++) t[i] = lower(sni[i]);
        if (!valid_domain(t)) FAIL(ui_tr("sni должен быть доменным именем"));
        cpy(k->sni, sizeof k->sni, t);
    } else if (k->sec == SEC_REALITY) FAIL(ui_tr("для Reality в ссылке нужен sni"));
    else if (valid_domain(k->host)) cpy(k->sni, sizeof k->sni, k->host);
    /* отпечаток TLS-клиента */
    static const char *const fps[] = { "chrome", "firefox", "safari", "ios", "android", "edge", "360", "qq", "random", "randomized" };
    if (!fp) fp = "chrome";
    size_t fi = 0;
    for (; fi < sizeof fps / sizeof *fps; fi++) if (!strcmp(fp, fps[fi])) break;
    if (fi == sizeof fps / sizeof *fps) FAIL(ui_tr("fp=%.24s не поддерживается"), fp);
    cpy(k->fp, sizeof k->fp, fp);
    /* Reality */
    k->pbk[0] = k->sid[0] = k->spx[0] = k->pqv[0] = 0;
    if (k->sec == SEC_REALITY) {
        if (!pbk || strlen(pbk) != 43 || !charset(pbk, "-_")) FAIL(ui_tr("pbk (открытый ключ Reality) неверного формата"));
        cpy(k->pbk, sizeof k->pbk, pbk);
        if (sid) {
            size_t sl = strlen(sid);
            if (sl > 16 || sl % 2) FAIL(ui_tr("sid (shortId Reality) - до 16 шестнадцатеричных цифр, чётное число"));
            for (size_t i = 0; i < sl; i++) if (!is_hex(sid[i])) FAIL(ui_tr("sid (shortId Reality) - только шестнадцатеричные цифры"));
            cpy(k->sid, sizeof k->sid, sid);
        }
        if (spx && (spx[0] != '/' || !printable(spx) || !cpy(k->spx, sizeof k->spx, spx))) FAIL(ui_tr("spx (spiderX) должен быть путём вида /..."));
        if (pqv && (!charset(pqv, "-_") || !cpy(k->pqv, sizeof k->pqv, pqv))) FAIL(ui_tr("pqv (ключ ML-DSA-65) неверного формата"));
    } else if (pbk || sid || spx || pqv) FAIL(ui_tr("pbk/sid/spx/pqv бывают только при security=reality"));
    /* ALPN (только TLS; для Reality Xray его не использует) */
    k->alpn[0] = 0;
    if (alpn && k->sec == SEC_TLS) {
        if (strlen(alpn) >= sizeof k->alpn || !charset(alpn, ",/.")) FAIL(ui_tr("alpn неверного формата"));
        char t[64];
        cpy(t, sizeof t, alpn);
        for (char *s = t, *e; *s; s = e + (*e != 0)) {
            e = s + strcspn(s, ",");
            size_t l = (size_t)(e - s);
            if (!((l == 2 && !memcmp(s, "h2", 2)) || (l == 8 && !memcmp(s, "http/1.1", 8))))
                FAIL(ui_tr("alpn: допустимы только h2 и http/1.1"));
            if (!*e) break;
        }
        cpy(k->alpn, sizeof k->alpn, alpn);
    }
    /* WebSocket */
    k->ws_host[0] = k->ws_path[0] = 0;
    if (k->net == NET_WS) {
        if (host) {
            char t[256];
            size_t hl2 = strlen(host);
            if (hl2 >= sizeof t) FAIL(ui_tr("host (WebSocket) слишком длинный"));
            for (size_t i = 0; i <= hl2; i++) t[i] = lower(host[i]);
            if (!valid_domain(t)) FAIL(ui_tr("host (WebSocket) должен быть доменным именем"));
            cpy(k->ws_host, sizeof k->ws_host, t);
        }
        if (path && (path[0] != '/' || !printable(path) || !cpy(k->ws_path, sizeof k->ws_path, path)))
            FAIL(ui_tr("path (WebSocket) должен быть путём вида /..."));
    } else if ((host && strcmp(host, k->host)) || (path && strcmp(path, "/"))) {
        FAIL(ui_tr("host/path бывают только при type=ws"));
    }
    plat_wipe(k->q, sizeof k->q);
    return true;
#undef FAIL
}

/* ======================= туннель ======================= */
static void hex_rand(char *out, size_t bytes) {
    unsigned char r[32];
    if (bytes > sizeof r || RAND_bytes(r, (int)bytes) != 1) {
        /* без хорошего случайного источника туннель не поднимаем: пустой пароль отвергнет конфигурация */
        out[0] = 0;
        return;
    }
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) { out[2 * i] = hx[r[i] >> 4]; out[2 * i + 1] = hx[r[i] & 15]; }
    out[2 * bytes] = 0;
    plat_wipe(r, sizeof r);
}

/* Конфигурация для туннеля (xlite_c.c или tools/xlite): строки "ключ=значение", пустая
 * строка — конец. Значения уже проверены разбором ссылки; перевод строки в значении отвергается,
 * чтобы нельзя было подсунуть лишний ключ. Возвращает false, если значение недопустимо. */
static bool kv_line(Buf *b, const char *k, const char *v) {
    if (!v || strpbrk(v, "\r\n")) return false;
    buf_puts(b, k);
    buf_puts(b, "=");
    buf_puts(b, v);
    buf_puts(b, "\n");
    return true;
}

static bool build_config(Buf *b) {
    VSec *k = K;
    char num[16];
    bool ok = true;
    snprintf(num, sizeof num, "%d", V.port);
    ok &= kv_line(b, "port", num);
    ok &= kv_line(b, "user", k->user);
    ok &= kv_line(b, "pass", k->pass);
    ok &= kv_line(b, "uuid", k->uuid);
    ok &= kv_line(b, "host", k->host);
    snprintf(num, sizeof num, "%d", k->port);
    ok &= kv_line(b, "rport", num);
    ok &= kv_line(b, "enc", k->enc);
    if (k->flow[0]) ok &= kv_line(b, "flow", k->flow);
    ok &= kv_line(b, "type", k->net == NET_WS ? "ws" : "tcp");
    ok &= kv_line(b, "security", k->sec == SEC_REALITY ? "reality" : "tls");
    ok &= kv_line(b, "fp", k->fp);
    if (k->sni[0]) ok &= kv_line(b, "sni", k->sni);
    if (k->sec == SEC_REALITY) {
        ok &= kv_line(b, "pbk", k->pbk);
        ok &= kv_line(b, "sid", k->sid);
        if (k->spx[0]) ok &= kv_line(b, "spx", k->spx);
        if (k->pqv[0]) ok &= kv_line(b, "pqv", k->pqv);
    } else if (k->alpn[0]) {
        ok &= kv_line(b, "alpn", k->alpn);
    }
    if (k->net == NET_WS) {
        ok &= kv_line(b, "wspath", k->ws_path[0] ? k->ws_path : "/");
        if (k->ws_host[0]) ok &= kv_line(b, "wshost", k->ws_host);
    }
    buf_puts(b, "\n");
    return ok;
}

static void xray_kill(void) {
    if (V.running) { XliteStop(); V.running = false; }
    if (K) {
        plat_wipe(K->user, sizeof K->user);
        plat_wipe(K->pass, sizeof K->pass);
        plat_wipe(K->proxy, sizeof K->proxy);
    }
    V.port = 0;
}

/* Поднять туннель с текущей ссылкой. false — V.err. Обычно доли секунды. */
static bool xray_start(void) {
    xray_kill();
    V.fatal = true;
    if (!V.have) { set_err(ui_tr("ссылка VLESS не задана")); return false; }
    bool ok = false;
    V.fatal = false;
    /* Порт выбирает система (свободный на 127.0.0.1); если его успели занять — другой. */
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        V.port = tcp_free_port();
        hex_rand(K->user, 16);
        hex_rand(K->pass, 16);
        if (V.port <= 0 || !K->user[0] || !K->pass[0]) { set_err(ui_tr("нет свободного порта или случайных чисел")); break; }
        Buf cfg = {0};
        if (!build_config(&cfg)) { buf_wipe(&cfg); set_err(ui_tr("недопустимый символ в параметрах ссылки")); break; }
        int rc = XliteStart(cfg.p, (int)cfg.len);
        buf_wipe(&cfg);
        if (rc == 1) { V.fatal = true; set_err(ui_tr("ссылка не подходит туннелю (параметры Reality/TLS?)")); break; }
        if (rc == 2) { set_err(ui_tr("туннель не создался (адрес сервера не разрешился в DNS?)")); break; }
        if (rc == 3) { set_err(ui_tr("туннель не открыл порт 127.0.0.1:%d"), V.port); continue; }
        V.running = true;
        i64 deadline = now_ms() + 2000;
        while (now_ms() < deadline) {
            int r = tcp_local_ready(V.port, 100);
            if (r == 1) { ok = true; break; }
            if (r < 0) break;
            plat_sleep_ms(10);
        }
        if (!ok) { xray_kill(); set_err(ui_tr("туннель не отвечает на 127.0.0.1")); }
    }
    if (!ok) { xray_kill(); return false; }
    snprintf(K->proxy, sizeof K->proxy, "socks5h://%s:%s@127.0.0.1:%d", K->user, K->pass, V.port);
    return true;
}

/* ======================= проверка через туннель ======================= */
static size_t probe_write(char *p, size_t sz, size_t n, void *ud) {
    Buf *b = ud;
    size_t t = sz * n;
    if (b->len + t > 4096) return 0;
    buf_put(b, p, t);
    return t;
}

/* Проверка закончена или прервана: хэндлы и пул соединений (соединение с Discord через туннель
 * не висит без дела) освобождаются. */
static void probe_free(void) {
    if (P.e) { curl_multi_remove_handle(P.m, P.e); curl_easy_cleanup(P.e); P.e = NULL; }
    if (P.m) { curl_multi_cleanup(P.m); P.m = NULL; }
    buf_free(&P.body);
    P.stage = PR_IDLE;
}

static bool probe_add_get(void) {
    if (!P.e) {
        P.e = curl_easy_init();
        if (!P.e) return false;
        net_curl_common(P.e);
        curl_easy_setopt(P.e, CURLOPT_URL, PROBE_URL);
        curl_easy_setopt(P.e, CURLOPT_PROXY, K->proxy);
        curl_easy_setopt(P.e, CURLOPT_NOPROXY, "");
        curl_easy_setopt(P.e, CURLOPT_SOCKS5_AUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(P.e, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(P.e, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(P.e, CURLOPT_WRITEFUNCTION, probe_write);
        curl_easy_setopt(P.e, CURLOPT_WRITEDATA, &P.body);
    }
    P.body.len = 0;
    return curl_multi_add_handle(P.m, P.e) == CURLM_OK;
}

/* Прямое TCP-соединение с сервером VLESS (не с Discord): только чтобы отличить «сервер недоступен»
 * от «рукопожатие не прошло» в сообщении об ошибке. */
static bool probe_add_diag(void) {
    P.e = curl_easy_init();
    if (!P.e) return false;
    char url[300];
    snprintf(url, sizeof url, K->host_v6 ? "http://[%s]:%d/" : "http://%s:%d/", K->host, K->port);
    curl_easy_setopt(P.e, CURLOPT_URL, url);
    curl_easy_setopt(P.e, CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(P.e, CURLOPT_PROXY, "");
    curl_easy_setopt(P.e, CURLOPT_NOPROXY, "");
    curl_easy_setopt(P.e, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(P.e, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(P.e, CURLOPT_NOSIGNAL, 1L);
    return curl_multi_add_handle(P.m, P.e) == CURLM_OK;
}

static void probe_start(void) {
    probe_free();
    P.m = curl_multi_init();
    P.ok = false;
    P.fail[0] = 0;
    if (!P.m || !probe_add_get()) { snprintf(P.fail, sizeof P.fail, ui_tr("нет памяти для проверки")); probe_free(); return; }
    P.stage = PR_GET1;
}

static bool body_ok(void) {
    if (!P.body.len) return false;
    buf_put(&P.body, "", 1);
    return strstr(P.body.p, "\"url\"") && strstr(P.body.p, "wss://");
}

static void probe_done(bool ok);

/* Шаг проверки: вызывать, пока P.stage != PR_IDLE. */
static void probe_step(void) {
    if (P.stage == PR_IDLE) return;
    int running = 0;
    curl_multi_perform(P.m, &running);
    CURLMsg *msg;
    int left;
    while (P.stage != PR_IDLE && (msg = curl_multi_info_read(P.m, &left))) {
        if (msg->msg != CURLMSG_DONE || msg->easy_handle != P.e) continue;
        CURLcode rc = msg->data.result;
        long status = 0;
        curl_off_t t_us = 0;
        if (rc == CURLE_OK) {
            curl_easy_getinfo(P.e, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_getinfo(P.e, CURLINFO_TOTAL_TIME_T, &t_us);
        }
        curl_multi_remove_handle(P.m, P.e);
        if (P.stage == PR_GET1) {
            if (rc == CURLE_OK && status == 200 && body_ok()) {
                P.t1_us = (i64)t_us;
                P.stage = PR_GET2;
                if (!probe_add_get()) { V.latency = (int)(P.t1_us / 1000); probe_free(); probe_done(true); }
                return;
            }
            bool diag = false;
            if (rc == CURLE_OK) snprintf(P.fail, sizeof P.fail, ui_tr("Discord недоступен через туннель (HTTP %ld)"), status);
            else if (rc == CURLE_COULDNT_CONNECT) snprintf(P.fail, sizeof P.fail, ui_tr("туннель не принимает соединения"));
            else if (rc == CURLE_PEER_FAILED_VERIFICATION)
                snprintf(P.fail, sizeof P.fail, ui_tr("Discord недоступен через туннель: сертификат Discord не прошёл проверку (подмена на пути)"));
            else if (rc == CURLE_OPERATION_TIMEDOUT) {
                snprintf(P.fail, sizeof P.fail, ui_tr("сервер VLESS не отвечает (таймаут 10 с)"));
                diag = true;
            } else {
                snprintf(P.fail, sizeof P.fail, ui_tr("рукопожатие с сервером VLESS не прошло (ошибка curl %d)"), (int)rc);
                diag = true;
            }
            curl_easy_cleanup(P.e);
            P.e = NULL;
            if (diag && probe_add_diag()) { P.stage = PR_DIAG; return; }
            probe_free();
            probe_done(false);
            return;
        }
        if (P.stage == PR_GET2) {
            /* второй запрос по готовому соединению: чистое время пути до Discord и обратно */
            V.latency = (int)((rc == CURLE_OK && status == 200 ? (i64)t_us : P.t1_us) / 1000);
            probe_free();
            probe_done(true);
            return;
        }
        if (P.stage == PR_DIAG) {
            if (rc == CURLE_OK)
                snprintf(P.fail, sizeof P.fail, ui_tr("рукопожатие VLESS не прошло: сервер %.64s:%d доступен, но не принял ключ, UUID или SNI либо не пускает к Discord"),
                         K->host_v6 ? "[IPv6]" : K->host, K->port);
            else
                snprintf(P.fail, sizeof P.fail, ui_tr("сервер VLESS %.64s:%d не отвечает (нет TCP-соединения)"), K->host_v6 ? "[IPv6]" : K->host, K->port);
            probe_free();
            probe_done(false);
            return;
        }
    }
}

static const int RETRY_S[] = { 5, 10, 15, 15, 15 };   /* автопроверки в DOWN: не реже раза в 15 с */

static void schedule_retry(void) {
    int d = RETRY_S[V.backoff < 4 ? V.backoff : 4];
    if (V.backoff < 4) V.backoff++;
    V.next_try = now_ms() + d * 1000;
}

static void probe_done(bool ok) {
    if (V.st == VL_OFF) return;                 /* выключили, пока шла проверка */
    if (ok) {
        V.st = VL_UP;
        V.fails = 0;
        V.backoff = 0;
        V.next_try = 0;
        V.err[0] = 0;
    } else {
        V.st = VL_DOWN;
        set_err("%s", P.fail[0] ? P.fail : ui_tr("туннель не работает"));
        schedule_retry();
    }
    wake_all();
}

/* Перезапустить туннель и начать проверку (асинхронно). */
static bool restart_async(void) {
    probe_free();
    if (!xray_start()) {
        V.st = VL_DOWN;
        if (V.fatal) V.next_try = 0;            /* бинарник не тот / не найден — сами не повторяем */
        else schedule_retry();
        wake_all();
        return false;
    }
    V.st = VL_STARTING;
    V.fails = 0;
    probe_start();
    if (P.stage == PR_IDLE) { probe_done(false); return false; }
    wake_all();
    return true;
}

/* Дождаться конца проверки (вход в программу, до интерфейса). */
static void wait_probe(void) {
    while (P.stage != PR_IDLE) {
        curl_multi_poll(P.m, NULL, 0, 100, NULL);
        if (false) {
            probe_free();
            snprintf(P.fail, sizeof P.fail, ui_tr("туннель остановился во время проверки"));
            probe_done(false);
            return;
        }
        probe_step();
    }
}

/* ======================= интерфейс модуля ======================= */
VlessState vless_state(void) { return V.st; }
const char *vless_error(void) { return V.err; }
int vless_latency_ms(void) { return V.latency; }

const char *vless_proxy_url(void) {
    return (V.st == VL_UP || V.st == VL_STARTING) && K && K->proxy[0] ? K->proxy : NULL;
}

bool vless_socks(char *ip, size_t ipn, int *port, char *user, size_t usern, char *pass, size_t passn) {
    if (!vless_proxy_url()) return false;
    snprintf(ip, ipn, "127.0.0.1");
    *port = V.port;
    snprintf(user, usern, "%s", K->user);
    snprintf(pass, passn, "%s", K->pass);
    return true;
}

/* what == NULL — «через туннель прошло успешно»: счётчик сбоев подряд сбрасывается. */
void vless_report_failure(const char *what) {
    if (V.st != VL_UP && V.st != VL_STARTING) return;
    if (!what) { V.fails = 0; return; }
    if (++V.fails < FAILS_TO_DOWN) return;
    V.st = VL_DOWN;
    set_err(ui_tr("через туннель нет связи (%s)"), what);
    V.backoff = 0;
    V.next_try = now_ms() + 3000;
    wake_all();
}

/* Выбор пользователя «повторить»: синхронно, как vless_set_link (до ~15 с). Автоматические попытки
 * в VL_DOWN идут без блокировки — из vless_service. */
bool vless_restart(void) {
    if (!area() || !V.have) { set_err(ui_tr("ссылка VLESS не задана")); return false; }
    V.backoff = 0;
    V.deaths = 0;
    if (!restart_async()) return false;
    wait_probe();
    return V.st == VL_UP;
}

long vless_xray_rss_kb(void) { return -1; }      /* туннель внутри mincord: отдельной памяти нет */

bool vless_set_link(const char *link, char *err, size_t errn, int *latency_ms) {
    if (errn) err[0] = 0;
    if (latency_ms) *latency_ms = -1;
    if (!area()) { snprintf(err, errn, ui_tr("нет защищённой памяти")); return false; }
    probe_free();
    xray_kill();
    V.have = false;
    V.st = VL_OFF;
    if (!parse_link(link, err, errn)) {
        plat_wipe(K, sizeof *K);
        return false;
    }
    V.have = true;
    V.backoff = 0;
    V.deaths = 0;
    V.latency = -1;
    if (!restart_async()) { snprintf(err, errn, "%s", V.err); xray_kill(); return false; }
    wait_probe();
    if (V.st != VL_UP) {
        /* ссылку держим: «повторить» (vless_restart) снова поднимет туннель */
        snprintf(err, errn, "%s", V.err);
        xray_kill();
        V.next_try = 0;
        return false;
    }
    if (latency_ms) *latency_ms = V.latency;
    if (!O.no_save && !keyring_save("vless", K->link, strlen(K->link)))
        snprintf(err, errn, ui_tr("ссылка работает, но хранилище секретов недоступно: не сохранена"));
    return true;
}

bool vless_load_saved(void) {
    if (!area()) return false;
    V.err[0] = 0;
    size_t cap;
    char *buf = secret_scratch(&cap);
    int n = keyring_load("vless", buf, cap < LINK_MAX + 1 ? cap : LINK_MAX + 1);
    if (n == -2) {                              /* ревью 8 М3: хранилище не прочиталось — VLESS «не работает», а не «нет»: */
        plat_wipe(buf, cap);                    /* иначе клиент молча пошёл бы напрямую */
        set_err(ui_tr("хранилище секретов не прочиталось: ссылка VLESS недоступна"));
        V.st = VL_DOWN;
        return false;
    }
    if (n <= 0) { plat_wipe(buf, cap); return false; }
    char err[512];
    bool saved = O.no_save;
    O.no_save = true;                           /* уже в хранилище — не перезаписывать */
    bool ok = vless_set_link(buf, err, sizeof err, NULL);
    O.no_save = saved;
    plat_wipe(buf, cap);
    if (!ok) {
        if (!V.have) set_err(ui_tr("сохранённая ссылка VLESS повреждена: %s"), err);
        V.st = VL_DOWN;
    }
    return ok;
}

void vless_disable(void) {
    probe_free();
    xray_kill();
    if (K) plat_wipe(K, sizeof *K);
    V.have = false;
    V.st = VL_OFF;
    V.err[0] = 0;
    V.next_try = 0;
    V.latency = -1;
    wake_all();
}

/* Выход, паника, сбой: затереть ссылку и ключи (безопасно и из обработчика сбоя), затем освободить
 * проверку. Туннель на C: XliteWipe затирает его копию параметров ссылки и ключи сессий, ожидание
 * потока — не дольше 0,3 с (join не нужен: процесс сейчас завершится). Туннель на Go отсюда не
 * трогаем — его потоки завершатся вместе с процессом. */
void vless_stop(void) {
    if (V.running) XliteWipe();
    if (K) plat_wipe(K, sizeof *K);
    V.have = false;
    V.st = VL_OFF;
    V.next_try = 0;
    probe_free();
}

void vless_forget(void) { keyring_delete("vless"); }

void vless_service(void) {
    if (V.st == VL_OFF) return;
    /* поток туннеля на C упал (порт он уже закрыл): перезапуск, как при смерти процесса Xray раньше */
    if (V.running && XliteFailed()) {
        probe_free();
        xray_kill();
        i64 now = now_ms();
        if (now - V.deaths_since > 300000) { V.deaths_since = now; V.deaths = 0; }
        V.deaths++;
        V.st = VL_DOWN;
        if (V.deaths > 5) { set_err(ui_tr("туннель падает раз за разом - нужен выбор: повторить, другая ссылка или без VLESS")); V.next_try = 0; }
        else { set_err(ui_tr("туннель неожиданно остановился; перезапуск...")); V.next_try = now + 1000; }
        wake_all();
    }
    if (P.stage != PR_IDLE) probe_step();
    if (V.st == VL_DOWN && V.next_try && now_ms() >= V.next_try && P.stage == PR_IDLE) {
        V.next_try = 0;
        if (V.running) {
            V.st = VL_STARTING;                   /* туннель жив: только проверить путь */
            probe_start();
            if (P.stage == PR_IDLE) probe_done(false);
        } else {
            restart_async();                      /* при неудаче сама назначит следующую попытку */
        }
    }
    /* Сообщения о смене состояния показывает интерфейс (ui.c, cli.c) по vless_state()/vless_error(). */
}

i64 vless_next_wake(void) {
    if (P.stage != PR_IDLE) return now_ms() + 20;     /* проверка идёт: curl опрашивается часто, как REST */
    if (V.st == VL_DOWN && V.next_try) return V.next_try;
    return INT64_MAX;
}

int vless_waitables(Waitable *out, int max) {
    (void)out; (void)max;
    return 0;                                   /* процесса туннеля больше нет — ждать нечего */
}
