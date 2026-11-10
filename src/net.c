/* Сеть: REST (curl_multi, без блокировки цикла) и gateway (WebSocket + zlib-stream).
 * Хосты: только discord.com и *.discord.gg; только https/wss; без перенаправлений. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <zlib.h>
#include <stdint.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif
#include "mc.h"
#include "captcha.h"
#include "plat.h"

#define API_BASE "https://discord.com/api/v9"
#define GW_QUERY "/?v=9&encoding=json&compress=zlib-stream"
#define GW_DEFAULT "wss://gateway.discord.gg"
#define MAX_FRAME (32u << 20)
#define MAX_PAYLOAD (64u << 20)

static const char UA[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36";
/* Свойства клиента (X-Super-Properties и IDENTIFY — одна строка, согласованы). Как у веб-клиента (снято 10.11.2026):
 * язык системы, идентификатор запуска и «подпись запуска» (launch_signature) — случайные на каждый запуск.
 * launch_signature — UUIDv4, в котором биты-признаки модов клиента (BetterDiscord, Vencord и др.) нулевые:
 * модов у Astra DS нет. Без этого поля Discord отвергал верный код 2FA (60008), issue #1. Собирается в net_init. */
static char PROPS[900];
static char h_locale[48], h_lang[96];
/* Случайные байты системы (не RAND_bytes: генератор OpenSSL при первом вызове поднимает свой DRBG
 * и держит ~0,4 МБ — ради двух идентификаторов на старте это дорого). */
static bool os_random(void *p, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    return getrandom(p, n, 0) == (ssize_t)n;
#endif
}
static void uuid4_mask(char out[37], bool launch_sig) {
    unsigned char r[16];
    if (!os_random(r, sizeof r)) { u64 t = (u64)now_ms(); memcpy(r, &t, 8); memcpy(r + 8, &t, 8); }
    r[6] = (unsigned char)((r[6] & 0x0F) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3F) | 0x80);
    if (launch_sig) {                           /* биты модов (от младшего бита 128-битного числа) — в ноль */
        static const unsigned char bits[] = { 119, 108, 100, 91, 84, 75, 61, 55, 48, 38, 24, 11 };
        for (size_t i = 0; i < sizeof bits; i++) r[15 - bits[i] / 8] &= (unsigned char)~(1u << (bits[i] % 8));
    }
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}
static void uuid4(char out[37]) { uuid4_mask(out, false); }
/* Язык системы: "ru-RU" -> X-Discord-Locale "ru" (у Discord полные коды только en-US/en-GB/es-ES/pt-BR/zh-CN/zh-TW/sv-SE). */
static void locale_detect(char *sys, size_t sn, char *dl, size_t dn) {
    snprintf(sys, sn, "en-US");
#ifdef _WIN32
    wchar_t w[85];
    if (GetUserDefaultLocaleName(w, 85) > 0) {
        char t[16];
        int i = 0;
        for (; w[i] && i < 15; i++) t[i] = w[i] < 0x80 ? (char)w[i] : '?';
        t[i] = 0;
        snprintf(sys, sn, "%s", t);
    }
#else
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LANG");
    if (l && strlen(l) >= 2) {
        char t[16];
        size_t i = 0;
        for (; l[i] && l[i] != '.' && l[i] != '@' && i < 15; i++) t[i] = l[i] == '_' ? '-' : l[i];
        t[i] = 0;
        if (strcmp(t, "C") && strcmp(t, "POSIX")) snprintf(sys, sn, "%s", t);
    }
#endif
    bool ok = strlen(sys) >= 2;
    for (const char *p = sys; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-';
    if (!ok) snprintf(sys, sn, "en-US");
    static const char *const FULL[] = { "en-US", "en-GB", "es-ES", "pt-BR", "zh-CN", "zh-TW", "sv-SE" };
    snprintf(dl, dn, "%.2s", sys);
    for (size_t i = 0; i < sizeof FULL / sizeof *FULL; i++) if (!strcmp(sys, FULL[i])) snprintf(dl, dn, "%s", sys);
    if (!strcmp(dl, "en")) snprintf(dl, dn, "en-US");
}
static void props_build(void) {
    char sys[16], dl[16], launch[37], sig[37];
    locale_detect(sys, sizeof sys, dl, sizeof dl);
    uuid4(launch);
    uuid4_mask(sig, true);
    snprintf(PROPS, sizeof PROPS,
        "{\"os\":\"Windows\",\"browser\":\"Chrome\",\"device\":\"\",\"device_model_name\":\"\",\"system_locale\":\"%s\",\"has_client_mods\":false,"
        "\"browser_user_agent\":\"%s\",\"browser_version\":\"152.0.0.0\",\"os_version\":\"10\",\"referrer\":\"\","
        "\"referring_domain\":\"\",\"referrer_current\":\"\",\"referring_domain_current\":\"\",\"release_channel\":\"stable\","
        "\"client_build_number\":668923,\"client_event_source\":null,\"client_launch_id\":\"%s\","
        "\"launch_signature\":\"%s\",\"client_app_state\":\"focused\"}", sys, UA, launch, sig);
    snprintf(h_locale, sizeof h_locale, "X-Discord-Locale: %s", dl);
    if (!strncmp(dl, "en", 2)) snprintf(h_lang, sizeof h_lang, "Accept-Language: en-US,en;q=0.9");
    else snprintf(h_lang, sizeof h_lang, "Accept-Language: %s,%.2s;q=0.9,en-US;q=0.8,en;q=0.7", sys, sys);
}

bool g_auth_failed;
int g_gw_ping = -1;

/* Заголовки: узлы curl_slist собраны вручную, чтобы токен не копировался в кучу curl. */
static char h_super[1400];
static struct curl_slist n_auth, n_super, n_ct, n_origin, n_ref, n_locale, n_lang, n_debug, n_tz;
static char h_tz[96];                           /* X-Discord-Timezone: Европа/... (как шлёт любой клиент), "" — не определён */
/* Часовой пояс IANA: Windows — ICU из системы (icu.dll, Windows 10 1903+), Linux — /etc/localtime -> zoneinfo/.
 * Только символы [A-Za-z0-9_/+-]; не определили — заголовок не шлём. */
static void tz_detect(void) {
    char z[64] = "";
#ifdef _WIN32
    /* Без ICU (icu.dll оставлял ~0,5 МБ своих кэшей ради одного заголовка): имя пояса Windows ->
     * IANA по таблице CLDR (основные пояса). Нет в таблице — заголовок не шлём. */
    static const char *const MAP[][2] = {
        {"Russian Standard Time","Europe/Moscow"},{"Kaliningrad Standard Time","Europe/Kaliningrad"},
        {"Russia Time Zone 3","Europe/Samara"},{"Ekaterinburg Standard Time","Asia/Yekaterinburg"},
        {"Omsk Standard Time","Asia/Omsk"},{"N. Central Asia Standard Time","Asia/Novosibirsk"},
        {"North Asia Standard Time","Asia/Krasnoyarsk"},{"North Asia East Standard Time","Asia/Irkutsk"},
        {"Yakutsk Standard Time","Asia/Yakutsk"},{"Vladivostok Standard Time","Asia/Vladivostok"},
        {"Magadan Standard Time","Asia/Magadan"},{"Russia Time Zone 10","Asia/Srednekolymsk"},{"Russia Time Zone 11","Asia/Kamchatka"},
        {"Astrakhan Standard Time","Europe/Astrakhan"},{"Saratov Standard Time","Europe/Saratov"},{"Volgograd Standard Time","Europe/Volgograd"},
        {"Altai Standard Time","Asia/Barnaul"},{"Tomsk Standard Time","Asia/Tomsk"},{"Transbaikal Standard Time","Asia/Chita"},
        {"Sakhalin Standard Time","Asia/Sakhalin"},
        {"Central Asia Standard Time","Asia/Bishkek"},{"West Asia Standard Time","Asia/Tashkent"},{"Qyzylorda Standard Time","Asia/Qyzylorda"},
        {"Bangladesh Standard Time","Asia/Dhaka"},{"Caucasus Standard Time","Asia/Yerevan"},{"Georgian Standard Time","Asia/Tbilisi"},
        {"Azerbaijan Standard Time","Asia/Baku"},{"Belarus Standard Time","Europe/Minsk"},{"FLE Standard Time","Europe/Kiev"},
        {"E. Europe Standard Time","Europe/Chisinau"},{"GTB Standard Time","Europe/Bucharest"},{"Turkey Standard Time","Europe/Istanbul"},
        {"W. Europe Standard Time","Europe/Berlin"},{"Central Europe Standard Time","Europe/Budapest"},
        {"Central European Standard Time","Europe/Warsaw"},{"Romance Standard Time","Europe/Paris"},{"GMT Standard Time","Europe/London"},
        {"Greenwich Standard Time","Atlantic/Reykjavik"},{"UTC","Etc/UTC"},{"Israel Standard Time","Asia/Jerusalem"},
        {"Arabian Standard Time","Asia/Dubai"},{"Arab Standard Time","Asia/Riyadh"},{"Iran Standard Time","Asia/Tehran"},
        {"Pakistan Standard Time","Asia/Karachi"},{"India Standard Time","Asia/Calcutta"},{"SE Asia Standard Time","Asia/Bangkok"},
        {"China Standard Time","Asia/Shanghai"},{"Singapore Standard Time","Asia/Singapore"},{"Tokyo Standard Time","Asia/Tokyo"},
        {"Korea Standard Time","Asia/Seoul"},{"Taipei Standard Time","Asia/Taipei"},{"AUS Eastern Standard Time","Australia/Sydney"},
        {"New Zealand Standard Time","Pacific/Auckland"},{"Mongolia Standard Time","Asia/Ulaanbaatar"},{"Afghanistan Standard Time","Asia/Kabul"},
        {"Eastern Standard Time","America/New_York"},{"Central Standard Time","America/Chicago"},{"Mountain Standard Time","America/Denver"},
        {"Pacific Standard Time","America/Los_Angeles"},{"Alaskan Standard Time","America/Anchorage"},{"Hawaiian Standard Time","Pacific/Honolulu"},
        {"E. South America Standard Time","America/Sao_Paulo"},{"Argentina Standard Time","America/Buenos_Aires"},
        {"Canada Central Standard Time","America/Regina"},{"Atlantic Standard Time","America/Halifax"},{"Egypt Standard Time","Africa/Cairo"},
        {"South Africa Standard Time","Africa/Johannesburg"},{"E. Africa Standard Time","Africa/Nairobi"},{"W. Central Africa Standard Time","Africa/Lagos"},
    };
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    if (GetDynamicTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID) {
        char k[128];
        int i = 0;
        for (; tz.TimeZoneKeyName[i] && i < 127; i++) k[i] = tz.TimeZoneKeyName[i] < 0x80 ? (char)tz.TimeZoneKeyName[i] : '?';
        k[i] = 0;
        for (size_t m = 0; m < sizeof MAP / sizeof *MAP; m++) if (!strcmp(k, MAP[m][0])) { snprintf(z, sizeof z, "%s", MAP[m][1]); break; }
    }
#else
    char lnk[256];
    ssize_t n = readlink("/etc/localtime", lnk, sizeof lnk - 1);
    if (n > 0) { lnk[n] = 0; const char *p = strstr(lnk, "zoneinfo/"); if (p) snprintf(z, sizeof z, "%s", p + 9); }
#endif
    bool ok = z[0] != 0;
    for (const char *p = z; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("_/+-", *p);
    if (ok) snprintf(h_tz, sizeof h_tz, "X-Discord-Timezone: %s", z);
}
static struct curl_slist *hdr_json, *hdr_plain;

/* Временно здесь, до переноса в mc.h координатором: */
void net_curl_common(CURL *e);                  /* общие настройки — и для проверки туннеля в vless.c */

/* Маршрут хэндла. VLESS выключен — без прокси; включён и работает (или проверяется) — только через
 * локальный SOCKS5 Xray с удалённым DNS; включён, но не работает — false: запрос ждёт, напрямую
 * не идём. Переменные окружения (http_proxy, all_proxy, no_proxy) не действуют ни в каком случае:
 * CURLOPT_PROXY и CURLOPT_NOPROXY заданы явно ("" — без прокси и без исключений). */
static bool route_ok(void) { return vless_state() == VL_OFF || vless_proxy_url() != NULL; }
static bool set_route(CURL *e, bool *via) {
    const char *p = vless_state() == VL_OFF ? "" : vless_proxy_url();
    if (!p) return false;
    curl_easy_setopt(e, CURLOPT_PROXY, p);
    curl_easy_setopt(e, CURLOPT_NOPROXY, "");
    if (p[0]) curl_easy_setopt(e, CURLOPT_SOCKS5_AUTH, (long)CURLAUTH_BASIC);
    if (via) *via = p[0] != 0;
    return true;
}
/* Ошибки, по которым видно, что не идёт сам путь (а не ответ Discord). */
static bool path_error(CURLcode rc) {
    return rc == CURLE_COULDNT_CONNECT || rc == CURLE_PROXY || rc == CURLE_OPERATION_TIMEDOUT ||
           rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
           rc == CURLE_GOT_NOTHING || rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_RESOLVE_PROXY;
}

/* login.c: свои хэндлы (без Authorization), но маршрут и X-Super-Properties — те же, что у всего REST. */
bool net_route(void *e, bool *via) { return set_route((CURL *)e, via); }
const char *net_super_header(void) { return h_super; }
const char *net_user_agent(void) { return UA; }
bool net_path_error(int rc) { return path_error((CURLcode)rc); }

void net_curl_common(CURL *e) {
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "https,wss");
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(e, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
    net_set_ca(e);
}

/* Корни доверия: Linux — встроенный набор из 18 центров (roots.h) вместо ~150 системных:
 * меньше тех, кто может подделать сертификат, и ~0,8 МБ памяти экономии на хранилище.
 * Windows — системное хранилище Schannel. */
#ifndef _WIN32
#include "roots.h"
#endif
void net_set_ca(void *h) {
#ifdef _WIN32
    (void)h;
#else
    struct curl_blob b = { (void *)ROOTS_PEM, sizeof ROOTS_PEM - 1, CURL_BLOB_NOCOPY };
    curl_easy_setopt((CURL *)h, CURLOPT_CAINFO_BLOB, &b);
#endif
}

/* Распределитель памяти для curl и OpenSSL: каждый блок затирается при освобождении и при
 * переносе (realloc). curl собирает HTTP-запрос с заголовком Authorization и кадры IDENTIFY/RESUME
 * в своей куче, OpenSSL — открытый текст TLS и ключи DAVE. Без затирания копии токена и ключей
 * оставались бы в освобождённой памяти и могли попасть в файл подкачки. */
typedef struct { size_t n, pad; } SaHdr;                  /* 16 байт: выравнивание данных сохраняется */
static void *sa_malloc(size_t n) {
    if (n > SIZE_MAX - sizeof(SaHdr)) return NULL;
    SaHdr *h = malloc(sizeof(SaHdr) + n);
    if (!h) return NULL;
    h->n = n;
    return h + 1;
}
static void sa_free(void *p) {
    if (!p) return;
    SaHdr *h = (SaHdr *)p - 1;
    plat_wipe(p, h->n);
    free(h);
}
static void *sa_realloc(void *p, size_t n) {
    if (!p) return sa_malloc(n);
    if (!n) { sa_free(p); return NULL; }
    SaHdr *h = (SaHdr *)p - 1;
    void *q = sa_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->n < n ? h->n : n);
    sa_free(p);
    return q;
}
static char *sa_strdup(const char *str) {
    size_t n = strlen(str) + 1;
    char *q = sa_malloc(n);
    if (q) memcpy(q, str, n);
    return q;
}
static void *sa_calloc(size_t a, size_t b) {
    if (b && a > SIZE_MAX / b) return NULL;
    void *q = sa_malloc(a * b);
    if (q) memset(q, 0, a * b);
    return q;
}
static void *ossl_malloc(size_t n, const char *f, int l) { (void)f; (void)l; return sa_malloc(n); }
static void *ossl_realloc(void *p, size_t n, const char *f, int l) { (void)f; (void)l; return sa_realloc(p, n); }
static void ossl_free(void *p, const char *f, int l) { (void)f; (void)l; sa_free(p); }

bool net_init(void) {
    /* до первого выделения памяти в OpenSSL и curl */
    CRYPTO_set_mem_functions(ossl_malloc, ossl_realloc, ossl_free);
    if (curl_global_init_mem(CURL_GLOBAL_DEFAULT, sa_malloc, sa_free, sa_realloc, sa_strdup, sa_calloc) != CURLE_OK) return false;
    props_build();
    Buf b = {0};
    base64_enc(PROPS, strlen(PROPS), &b);
    snprintf(h_super, sizeof h_super, "X-Super-Properties: %s", b.p);
    buf_free(&b);
    /* Порядок: Authorization, X-Super-Properties, [Content-Type], Origin, ... */
    n_auth.data = (char *)secret_auth_header();
    n_super.data = h_super;
    n_ct.data = "Content-Type: application/json";
    n_origin.data = "Origin: https://discord.com";
    n_ref.data = "Referer: https://discord.com/channels/@me";
    n_locale.data = h_locale;
    n_lang.data = h_lang;
    n_debug.data = "X-Debug-Options: bugReporterEnabled";
    n_auth.next = &n_super;
    n_ct.next = &n_origin;
    n_origin.next = &n_ref;
    n_ref.next = &n_locale;
    tz_detect();
    n_tz.data = h_tz;
    n_tz.next = &n_lang;
    n_locale.next = h_tz[0] ? &n_tz : &n_lang;
    n_lang.next = &n_debug;
    n_debug.next = NULL;
    hdr_json = &n_auth;
    hdr_plain = &n_auth;
    return true;
}

/* ======================= REST ======================= */
typedef struct Req {
    struct Req *next;
    char method[8];
    char *path;
    char *body;
    HttpCb cb;
    void *ctx;
    int attempts;
    i64 not_before;
    bool overtake;
    int cap_round;                              /* сколько раз уже повторяли с ответом капчи */
    char *xhdr;                                 /* доп. заголовок запроса (X-Context-Properties), NULL — нет */
    CURL *e;
    bool via_vless;
    Resp r;
    /* отправка файла: тело multipart = head + файл (потоком, кусками) + tail */
    FILE *fp;
    i64 fsize, fsent;
    Buf head, tail;
    size_t hoff, toff;
} Req;

static CURLM *g_multi;
static Req *q_head, *q_tail, *q_active;
static i64 g_rate_until;

bool g_readonly;
/* Только для --livetest (в рабочем режиме NULL): сторож записей и журнал ответов. */
bool (*g_http_guard)(const char *method, const char *path);
void (*g_http_trace)(const char *method, const char *path, long status);
long g_http_fake_status;                        /* selftest: отказ сторожа сразу отвечает этим кодом */
static Req *req_new(const char *method, const char *path, const char *body, HttpCb cb, void *ctx) {
    if (O.demo || !path || path[0] != '/') return NULL;
    if (g_readonly && strcmp(method, "GET")) return NULL;   /* снимок живого аккаунта: ничего не меняем */
    if (g_http_guard && !g_http_guard(method, path)) {
        if (g_http_fake_status && cb) { Resp fr; memset(&fr, 0, sizeof fr); fr.status = g_http_fake_status; cb(&fr, ctx); }
        return NULL;
    }
    Req *r = xcalloc(1, sizeof *r);
    snprintf(r->method, sizeof r->method, "%s", method);
    r->path = xstrdup(path);
    r->body = body ? xstrdup(body) : NULL;
    r->cb = cb;
    r->ctx = ctx;
    return r;
}
static struct curl_slist n_xh;                  /* доп. заголовок текущего запроса — перед Origin */
static void req_queue(Req *r) {
    if (q_tail) q_tail->next = r; else q_head = r;
    q_tail = r;
}
void http_async(const char *method, const char *path, const char *body, HttpCb cb, void *ctx) {
    Req *r = req_new(method, path, body, cb, ctx);
    if (r) req_queue(r);
}
/* С доп. заголовком (готовая строка «Имя: значение», только [ -~]): так официальный клиент шлёт
 * X-Context-Properties при вступлении на сервер и т. п. */
void http_async_hdr(const char *method, const char *path, const char *body, const char *hdr, HttpCb cb, void *ctx) {
    /* ревью 16: неподходящий заголовок — отказ запроса с ошибкой, а не отправка без заголовка (причина бана и т. п.) */
    if (hdr) {
        bool ok = strlen(hdr) < 8192 && strchr(hdr, ':');
        for (const char *p = hdr; ok && *p; p++) ok = *p >= 0x20 && *p <= 0x7E;
        if (!ok) { ui_notice(ui_tr("запрос не отправлен: неверный заголовок")); return; }
    }
    Req *r = req_new(method, path, body, cb, ctx);
    if (!r) return;
    if (hdr) r->xhdr = xstrdup(hdr);
    req_queue(r);
}
void http_async_delayed(const char *method, const char *path, const char *body, HttpCb cb, void *ctx, int delay_ms) {
    Req *r = req_new(method, path, body, cb, ctx);
    if (!r) return;
    r->not_before = now_ms() + (delay_ms > 0 ? delay_ms : 0);
    r->overtake = true;                         /* отложенный запрос не держит очередь: остальные идут вперёд */
    req_queue(r);
}
/* Первый запрос, который можно запускать сейчас: идём с головы; ждущий 429 держит очередь
 * (порядок сообщений), отложенный (overtake) — пропускается. *wake — когда проснуться, если никого. */
static bool is_send(const Req *r);
static bool cap_holds_sends(void);
static Req **q_pick(i64 now, i64 *wake) {
    *wake = INT64_MAX;
    for (Req **pp = &q_head; *pp; pp = &(*pp)->next) {
        Req *r = *pp;
        if (cap_holds_sends() && is_send(r)) continue;   /* ревью 6 М2: сообщение за капчей — следующие ждут за ним */
        i64 t = r->not_before > g_rate_until ? r->not_before : g_rate_until;
        if (now >= t) return pp;
        if (t < *wake) *wake = t;
        if (!r->overtake) return NULL;
    }
    return NULL;
}
bool http_busy(void) { return q_active || q_head; }

static size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
    Buf *b = ud;
    size_t t = sz * n;
    if (b->len + t > (8u << 20)) return 0;      /* ответ REST больше 8 МБ — не наш случай */
    buf_put(b, p, t);
    return t;
}

/* Тело multipart отдаётся curl кусками: заголовок части, файл (fread прямо в буфер curl), хвост.
 * В памяти — только эти два коротких куска; сам файл не загружается целиком. */
static size_t upload_read(char *out, size_t sz, size_t n, void *ud) {
    Req *r = ud;
    size_t cap = sz * n, got = 0;
    if (r->hoff < r->head.len) {
        got = r->head.len - r->hoff < cap ? r->head.len - r->hoff : cap;
        memcpy(out, r->head.p + r->hoff, got);
        r->hoff += got;
        return got;
    }
    if (r->fsent < r->fsize) {
        size_t want = (size_t)(r->fsize - r->fsent) < cap ? (size_t)(r->fsize - r->fsent) : cap;
        got = fread(out, 1, want, r->fp);
        if (got == 0) return CURL_READFUNC_ABORT;    /* файл укоротили или ошибка чтения */
        r->fsent += (i64)got;
        return got;
    }
    if (r->toff < r->tail.len) {
        got = r->tail.len - r->toff < cap ? r->tail.len - r->toff : cap;
        memcpy(out, r->tail.p + r->toff, got);
        r->toff += got;
    }
    return got;
}
/* Отправка файла идёт параллельно очереди REST, поэтому у неё своя голова цепочки заголовков
 * (Authorization, X-Super-Properties, Content-Type multipart) и общий неизменный хвост (Origin...). */
/* ======================= капча на любом REST-запросе =======================
 * Ответ 400 с captcha_key/captcha_sitekey: запрос не теряется — ждёт, пока пользователь пройдёт проверку
 * в окне captcha.c (WebView2; кнопку/клавишу показывает интерфейс), затем уходит заново с заголовками
 * X-Captcha-Key / X-Captcha-Rqtoken / X-Captcha-Session-Id, как у официального клиента. Отказ — ответ
 * 400 уходит вызывающему, пользователь видит причину. Автоматического решения нет. Данные проверки
 * (до ~22 КБ) живут в куче только пока она идёт, затираются при освобождении. Одновременно — одна. */
static void free_req(Req *r);
static CaptchaState CS;
static struct {
    Req *r;                                     /* отложенный запрос */
    char svc[24], site[64], sess[128];
    char *rqdata, *rqtoken, *key;               /* 4 КБ, 2 КБ, 16 КБ */
    char *h_key, *h_rqt, *h_sess;               /* готовые заголовки для повтора */
} CP;
#define CAP_KEY 16384
static struct curl_slist n_ck, n_crt, n_cses;
const CaptchaState *net_captcha(void) { return &CS; }
static bool cap_charset(const char *s, const char *extra) {
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr(extra, *s))) return false;
    return true;
}
static void cap_field(char *dst, size_t cap, const char *v, const char *extra) {
    dst[0] = 0;
    if (v && strlen(v) < cap && cap_charset(v, extra)) snprintf(dst, cap, "%s", v);
}
static void cap_wipe_free(char **p) { if (*p) { wipe(*p, strlen(*p)); free(*p); *p = NULL; } }
/* SECURITY К1: ответ hCaptcha (ключ) — в страницах от системы, закреплённых (plat_lock: не в подкачку, не в дампы) */
static char *cap_key_alloc(void) {
    char *k = plat_pages(CAP_KEY);
    if (!k) return NULL;
    plat_lock(k, CAP_KEY);
    memset(k, 0, CAP_KEY);
    return k;
}
static void cap_key_free(char *k) { wipe(k, CAP_KEY); plat_unlock(k, CAP_KEY); plat_pages_free(k, CAP_KEY); }
/* Ревью 8 М2: заголовок «X-Captcha-Key: <ответ>» собирается на месте, в тех же закреплённых страницах
 * (ответ сдвигается вправо), — копии ответа в обычной куче нет. */
static void cap_key_header(void) {
    static const char H[] = "X-Captcha-Key: ";
    size_t kl = strnlen(CP.key, CAP_KEY - sizeof H);
    memmove(CP.key + sizeof H - 1, CP.key, kl);
    memcpy(CP.key, H, sizeof H - 1);
    CP.key[sizeof H - 1 + kl] = 0;
    CP.h_key = CP.key;
}
static void cap_clear(void) {
    if (CS.open || captcha_busy()) captcha_close();
    /* SECURITY К2: поток окна не завершился (captcha_close не дождался) — он ещё может писать в key:
     * буфер не освобождаем (оставляем потоку; утечка 16 КБ лучше записи в освобождённую память). */
    if (CP.key && captcha_busy()) CP.key = NULL;
    if (CP.key) { cap_key_free(CP.key); CP.key = NULL; }
    cap_wipe_free(&CP.rqdata);
    cap_wipe_free(&CP.rqtoken);
    CP.h_key = NULL;                            /* живёт внутри CP.key (закреплённые страницы) */
    cap_wipe_free(&CP.h_rqt);
    cap_wipe_free(&CP.h_sess);
    CP.r = NULL;
    CP.svc[0] = CP.site[0] = CP.sess[0] = 0;
    memset(&CS, 0, sizeof CS);
}
/* Заголовки повтора: только для запроса, который ждал капчу, и только когда ответ уже есть. */
static bool cap_headers(Req *r, struct curl_slist *tail) {
    if (!r->cap_round || !CP.h_key || CP.r != r) return false;
    n_ck = (struct curl_slist){ CP.h_key, CP.h_rqt ? &n_crt : CP.h_sess ? &n_cses : tail };
    n_crt = (struct curl_slist){ CP.h_rqt, CP.h_sess ? &n_cses : tail };
    n_cses = (struct curl_slist){ CP.h_sess, tail };
    return true;
}
static bool is_send(const Req *r) {
    return !strcmp(r->method, "POST") && !strncmp(r->path, "/channels/", 10) && strstr(r->path, "/messages") &&
           !strstr(r->path, "/messages/");
}
/* капча держит отправку сообщения (не другой запрос) */
static bool cap_holds_sends(void) { return CS.pending && CP.r && is_send(CP.r); }
static const char *cap_what(const Req *r) {
    if (!strcmp(r->method, "POST") && !strcmp(r->path, "/users/@me/relationships")) return ui_tr("заявка в друзья");
    if (!strcmp(r->method, "POST") && !strcmp(r->path, "/users/@me/channels")) return ui_tr("новая беседа");
    if (!strcmp(r->method, "POST") && strstr(r->path, "/messages") && !strstr(r->path, "/ack") && !strstr(r->path, "/messages/")) return ui_tr("отправка сообщения");
    if (!strcmp(r->method, "PUT") && strstr(r->path, "/reactions/")) return ui_tr("реакция");
    return ui_tr("запрос к Discord");
}
/* Ответ — капча? Тогда запрос остаётся у нас (true), иначе — обычный путь (false). */
static bool cap_take(Req *r) {
    if (r->r.status != 400 || !r->r.body.p || r->fp) return false;
    if (!strncmp(r->path, "/invites/", 9)) return false;   /* вступление: капча -> «вступите через браузер» (store.c) */
    if (!strstr(r->r.body.p, "captcha_key") && !strstr(r->r.body.p, "captcha_sitekey")) return false;
    if (CP.r && CP.r != r) { ui_notice(ui_tr("Discord просит ещё одну проверку - сначала завершите текущую")); return false; }
    if (r->cap_round >= 3) return false;        /* трижды не принято — отдаём ошибку */
    cJSON *j = json_parse(r->r.body.p, r->r.body.len);
    char svc[24], site[64];
    cap_field(svc, sizeof svc, jstr(j, "captcha_service"), "");
    cap_field(site, sizeof site, jstr(j, "captcha_sitekey"), "-");
    if (!svc[0]) snprintf(svc, sizeof svc, "hcaptcha");
    if (strcmp(svc, "hcaptcha") || !site[0]) { json_done(); return false; }   /* другой сервис — показать нечем */
    bool again = CP.r == r;
    if (!again) cap_clear();
    else {                                      /* Discord не принял ответ: новая проверка тем же запросом */
        if (CP.key) wipe(CP.key, CAP_KEY);
        cap_wipe_free(&CP.rqdata); cap_wipe_free(&CP.rqtoken);
        CP.h_key = NULL; cap_wipe_free(&CP.h_rqt); cap_wipe_free(&CP.h_sess);
    }
    CP.r = r;
    snprintf(CP.svc, sizeof CP.svc, "%s", svc);
    snprintf(CP.site, sizeof CP.site, "%s", site);
    cap_field(CP.sess, sizeof CP.sess, jstr(j, "captcha_session_id"), "-_.");
    char *tmp = xmalloc(4096);
    cap_field(tmp, 4096, jstr(j, "captcha_rqdata"), "+/=._-");
    CP.rqdata = tmp;
    tmp = xmalloc(2048);
    cap_field(tmp, 2048, jstr(j, "captcha_rqtoken"), "+/=._-");
    CP.rqtoken = tmp;
    json_done();
    if (!CP.key) CP.key = cap_key_alloc();
    if (!CP.key) { cap_clear(); return false; }   /* нет памяти — обычная ошибка 400 */
    CS.pending = true;
    CS.open = false;
    CS.outcome = CAP_NONE;
    CS.supported = captcha_supported();
    snprintf(CS.what, sizeof CS.what, "%s", cap_what(r));
    snprintf(CS.error, sizeof CS.error, "%s", again ? ui_tr("Discord не принял проверку - пройдите её ещё раз") : "");
    ui_notice(ui_tr("\a Discord просит проверку (капча): %s. Откройте окно проверки или отмените"), CS.what);
    ui_dirty();
    return true;
}
void net_captcha_open(void) {
    if (!CS.pending || CS.open) return;
    if (O.demo) {                               /* демо: окна и сети нет — «пройдено», запрос «выполнен» */
        Req *r = CP.r;
        char what[64];
        snprintf(what, sizeof what, "%s", CS.what);
        cap_clear();
        CS.outcome = CAP_PASSED;
        snprintf(CS.what, sizeof CS.what, "%s", what);
        if (r) free_req(r);
        ui_notice(ui_tr("проверка пройдена (демо): %s"), what);
        ui_dirty();
        return;
    }
    if (!captcha_supported()) {
        snprintf(CS.error, sizeof CS.error, ui_tr("показать проверку нечем: нужен Edge WebView2 (в Linux - пока нет)"));
        ui_dirty();
        return;
    }
    char err[160] = "";
    wipe(CP.key, CAP_KEY);
    if (captcha_open(CP.site, CP.rqdata ? CP.rqdata : "", CP.key, CAP_KEY, false, err, sizeof err)) { CS.open = true; CS.error[0] = 0; }
    else snprintf(CS.error, sizeof CS.error, ui_tr("окно проверки не открылось: %s"), err);
    ui_dirty();
}
/* Отказ: вызывающий получает исходный ответ 400, пользователь — причину. */
void net_captcha_cancel(void) {
    if (!CS.pending) return;
    Req *r = CP.r;
    char what[64];
    snprintf(what, sizeof what, "%s", CS.what);
    cap_clear();
    CS.outcome = CAP_CANCELLED;                 /* интерфейс отличает отмену от «пройдено» */
    snprintf(CS.what, sizeof CS.what, "%s", what);
    if (r) {
        if (g_http_trace) g_http_trace(r->method, r->path, r->r.status);
        if (r->cb) r->cb(&r->r, r->ctx);
        free_req(r);
    }
    ui_notice(ui_tr("%s: отменено - проверка не пройдена"), what);
    ui_dirty();
}
/* Окно закрыто без ответа: запрос ещё ждёт (можно открыть снова или отменить). */
static void cap_failed(const char *err) {
    snprintf(CS.error, sizeof CS.error, ui_tr("проверка не пройдена: %s"), err && err[0] ? err : ui_tr("окно закрыто"));
    CS.outcome = CAP_FAILED;
    ui_dirty();
}
void net_captcha_test_closed(void) { if (CS.pending) { CS.open = false; cap_failed(NULL); } }   /* --selftest */
/* Главный цикл: окно проверки открыто — забрать результат (готовность будит g_wake, опроса нет). */
static void cap_service(void) {
    if (!CS.open) return;
    char err[160] = "";
    int rr = captcha_result(err, sizeof err);
    if (rr == 0) return;
    captcha_close();
    CS.open = false;
    if (rr < 0 || !CP.key[0]) { cap_failed(err); return; }
    /* ответ — в заголовки повтора; сам ответ сразу затираем */
    cap_key_header();
    if (CP.rqtoken && CP.rqtoken[0]) { size_t n = strlen(CP.rqtoken) + 24; CP.h_rqt = xmalloc(n); snprintf(CP.h_rqt, n, "X-Captcha-Rqtoken: %s", CP.rqtoken); }
    if (CP.sess[0]) { size_t n = strlen(CP.sess) + 28; CP.h_sess = xmalloc(n); snprintf(CP.h_sess, n, "X-Captcha-Session-Id: %s", CP.sess); }
    Req *r = CP.r;
    r->cap_round++;
    r->attempts = 0;
    r->not_before = 0;
    r->next = q_head;                            /* в начало очереди */
    q_head = r;
    if (!q_tail) q_tail = r;
    CS.pending = false;
    CS.error[0] = 0;
    CS.outcome = CAP_PASSED;
    ui_notice(ui_tr("проверка пройдена - повторяю: %s"), CS.what);
    ui_dirty();
}
/* После ответа на повтор: данные проверки больше не нужны (если это не новая капча). */
static void cap_done(Req *r) { if (CP.r == r && !CS.pending) cap_clear(); }
/* --selftest: подставной ответ 400 с капчей на запрос (без сети); cb получит ответ при отмене. */
void net_captcha_test(const char *method, const char *path, const char *resp, HttpCb cb, void *ctx) {
    Req *r = xcalloc(1, sizeof *r);
    snprintf(r->method, sizeof r->method, "%s", method);
    r->path = xstrdup(path);
    r->cb = cb;
    r->ctx = ctx;
    r->r.status = 400;
    buf_puts(&r->r.body, resp);
    if (!cap_take(r)) { if (r->cb) r->cb(&r->r, r->ctx); free_req(r); }
}
/* --selftest: «пройти» проверку подставным ответом (без окна) и вернуть заголовки повтора. */
bool net_captcha_test_solve(const char *answer, char *hdrs, size_t n) {
    if (!CS.pending || !CP.key) return false;
    snprintf(CP.key, CAP_KEY, "%s", answer);
    CS.open = true;
    /* как cap_service при rr == 1, но без окна */
    CS.open = false;
    cap_key_header();
    if (CP.rqtoken && CP.rqtoken[0]) { size_t m = strlen(CP.rqtoken) + 24; CP.h_rqt = xmalloc(m); snprintf(CP.h_rqt, m, "X-Captcha-Rqtoken: %s", CP.rqtoken); }
    if (CP.sess[0]) { size_t m = strlen(CP.sess) + 28; CP.h_sess = xmalloc(m); snprintf(CP.h_sess, m, "X-Captcha-Session-Id: %s", CP.sess); }
    CP.r->cap_round++;
    bool ok = cap_headers(CP.r, &n_origin);
    snprintf(hdrs, n, "%s|%s|%s", n_ck.data, CP.h_rqt ? CP.h_rqt : "", CP.h_sess ? CP.h_sess : "");
    Req *r = CP.r;
    cap_clear();
    free_req(r);
    return ok;
}

static char h_mp[96];                           /* Content-Type: multipart/form-data; boundary=... */
static struct curl_slist n_auth_u, n_super_u, n_mp;

static void start_req(Req *r, CURLM *multi) {
    /* Цепочка заголовков: с телом нужен Content-Type между X-Super-Properties и Origin. */
    if (!r->fp) {
        struct curl_slist *tail = &n_origin;
        if (r->xhdr) { n_xh = (struct curl_slist){ r->xhdr, &n_origin }; tail = &n_xh; }
        struct curl_slist *after = cap_headers(r, tail) ? &n_ck : tail;
        n_ct.next = after;
        n_super.next = r->body ? &n_ct : after;
    }
    n_auth_u = (struct curl_slist){ (char *)secret_auth_header(), &n_super_u };
    n_super_u = (struct curl_slist){ h_super, &n_mp };
    n_mp = (struct curl_slist){ h_mp, &n_origin };
    CURL *e = curl_easy_init();
    if (!e) { r->e = NULL; return; }
    char url[512];
    snprintf(url, sizeof url, API_BASE "%s", r->path);
    net_curl_common(e);
    if (!set_route(e, &r->via_vless)) { curl_easy_cleanup(e); r->e = NULL; return; }
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, r->body ? hdr_json : hdr_plain);
    curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, r->method);
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &r->r.body);
    if (r->fp) {
        r->hoff = r->toff = 0;
        r->fsent = 0;
        fseek(r->fp, 0, SEEK_SET);
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, &n_auth_u);
        curl_easy_setopt(e, CURLOPT_POST, 1L);
        curl_easy_setopt(e, CURLOPT_READFUNCTION, upload_read);
        curl_easy_setopt(e, CURLOPT_READDATA, r);
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)(r->head.len + (size_t)r->fsize + r->tail.len));
        /* медленная связь: без общего лимита 20 с, но обрыв, если меньше 1 КБ/с дольше 30 с */
        curl_easy_setopt(e, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 30L);
    } else if (r->body) {
        curl_easy_setopt(e, CURLOPT_POSTFIELDS, r->body);
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)strlen(r->body));
    } else if (!strcmp(r->method, "POST") || !strcmp(r->method, "PUT")) {
        curl_easy_setopt(e, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, 0L);
    }
    r->e = e;
    curl_multi_add_handle(multi, e);
}

static void free_req(Req *r) {
    free(r->path);
    free(r->body);
    free(r->xhdr);
    buf_free(&r->r.body);
    if (r->fp) fclose(r->fp);
    buf_free(&r->head);
    buf_free(&r->tail);
    free(r);
}

/* ---------- отправка файла ---------- */
/* Открыть файл по пути UTF-8 (на Windows — через широкие символы: кириллица в пути). 1 — нет файла, 2 — папка, 3 — не открыть. */
/* Только локальный файл (SECURITY Н2): иначе Windows откроет SMB-соединение мимо VLESS и отдаст чужому
 * серверу хэш NTLM. Отвергаются любые пути, начинающиеся с \ или / (UNC \\хост, \\?\UNC\, \??\UNC\, \\.\,
 * \GLOBAL??\ ...); полный путь (GetFullPathNameW — относительный путь при сетевом текущем каталоге тоже
 * станет UNC) обязан иметь вид X:\..., а диск X: — не сетевой (GetDriveTypeW: не DRIVE_REMOTE). 0 — ок. */
#ifdef _WIN32
static int local_wpath(const char *path, wchar_t *full, DWORD n) {
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, (int)(sizeof w / sizeof *w))) return 1;
    if (w[0] == L'\\' || w[0] == L'/') return 4;
    DWORD k = GetFullPathNameW(w, n, full, NULL);
    if (!k || k >= n) return 1;
    if (!((full[0] >= L'A' && full[0] <= L'Z') || (full[0] >= L'a' && full[0] <= L'z')) || full[1] != L':' || full[2] != L'\\') return 4;
    if (wcsstr(full, L"\\??\\") || wcsstr(full + 2, L"\\\\")) return 4;
    wchar_t root[4] = { full[0], L':', L'\\', 0 };
    UINT t = GetDriveTypeW(root);
    if (t == DRIVE_NO_ROOT_DIR) return 1;      /* нет такого диска — «не найден» */
    if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE && t != DRIVE_CDROM && t != DRIVE_RAMDISK) return 4;   /* REMOTE, NO_ROOT, UNKNOWN */
    return 0;
}
#endif
/* Размер локального файла по пути UTF-8 (кавычки по краям допустимы); -1 — нет, не обычный файл или путь
 * не локальный (сетевые пути и диски не трогаются вовсе: никакого SMB ради показа размера). */
long long plat_file_size(const char *utf8_path) {
    if (!utf8_path || !*utf8_path) return -1;
    char p[1024];
    size_t pl = strlen(utf8_path);
    if (pl >= sizeof p) return -1;
    memcpy(p, utf8_path, pl + 1);
    if (pl >= 2 && p[0] == '"' && p[pl - 1] == '"') { memmove(p, p + 1, pl - 2); p[pl - 2] = 0; }
    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) return -1;
#ifdef _WIN32
    wchar_t w[1024];
    if (local_wpath(p, w, (DWORD)(sizeof w / sizeof *w))) return -1;
    struct _stat64 st;
    if (_wstat64(w, &st) != 0 || !(st.st_mode & _S_IFREG)) return -1;
#else
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
#endif
    return (long long)st.st_size;
}
static int open_file(const char *path, FILE **fp, i64 *size) {
#ifdef _WIN32
    wchar_t w[1024];
    int lp = local_wpath(path, w, (DWORD)(sizeof w / sizeof *w));
    if (lp) return lp;
    struct _stat64 st;
    if (_wstat64(w, &st) != 0) return 1;
    if (st.st_mode & _S_IFDIR) return 2;
    if (!(st.st_mode & _S_IFREG)) return 3;
    *fp = _wfopen(w, L"rb");
#else
    struct stat st;
    if (stat(path, &st) != 0) return 1;
    if (S_ISDIR(st.st_mode)) return 2;
    if (!S_ISREG(st.st_mode)) return 3;
    *fp = fopen(path, "rb");
#endif
    if (!*fp) return 3;
    *size = (i64)st.st_size;
    return 0;
}
/* Локальный файл на чтение по пути UTF-8 с проверкой «не сетевой» (значок, баннер). NULL — err заполнен. */
FILE *fopen_local(const char *path, char *err, size_t errn) {
    FILE *fp = NULL;
    i64 size = 0;
    int rc = open_file(path, &fp, &size);
    if (rc == 0) return fp;
    snprintf(err, errn, "%s", rc == 4 ? ui_tr("сетевые пути и диски не поддерживаются: только локальный файл") :
             rc == 2 ? ui_tr("это папка, нужен файл") : ui_tr("файл не открывается"));
    return NULL;
}
/* fopen по пути UTF-8 (на Windows — широкими символами, иначе кириллица в пути искажается). */
FILE *fopen_u8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t w[1024], m[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, m, 8)) return NULL;
    return _wfopen(w, m);
#else
    return fopen(path, mode);
#endif
}
static void on_file_sent(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status == 200 || r->status == 201) ui_notice(ui_tr("файл отправлен"));
    else if (r->status == 413) ui_notice(ui_tr("файл не отправлен: Discord не принял размер (413)"));
    else if (r->status == 0) ui_notice(ui_tr("файл не отправлен: нет связи или файл изменился во время отправки"));
    else ui_notice(ui_tr("файл не отправлен: Discord ответил %ld"), r->status);
}
/* Отправка файла — отдельный хэндл в своём curl_multi, вне очереди REST: сообщения и история не
 * ждут загрузку. Без опроса: главный цикл ждёт сокеты, которые назвал curl (curl_multi_waitfds),
 * и таймер curl. Windows: на сокет — событие WSAEventSelect (чтение/запись/закрытие/подключение);
 * FD_WRITE срабатывает по фронту, поэтому ожидание ограничено 1 с (страховка, только пока идёт отправка). */
#define UP_FDS 4
static struct {
    CURLM *m;
    Req *r;
    int nfd;
    struct { curl_socket_t s; short ev; void *wev; } fd[UP_FDS];
} UP;
static void up_fd_drop(int i) {
#ifdef _WIN32
    WSAEventSelect(UP.fd[i].s, NULL, 0);
    WSACloseEvent((WSAEVENT)UP.fd[i].wev);
#endif
    UP.fd[i] = UP.fd[--UP.nfd];
}
/* Обновить набор ожидаемых сокетов по тому, чего сейчас ждёт curl. */
static void up_fds_refresh(void) {
    struct curl_waitfd w[UP_FDS];
    unsigned n = 0;
    if (curl_multi_waitfds(UP.m, w, UP_FDS, &n) != CURLM_OK || n > UP_FDS) n = 0;
    for (int i = UP.nfd - 1; i >= 0; i--) {
        bool keep = false;
        for (unsigned k = 0; k < n; k++) if (w[k].fd == UP.fd[i].s) keep = true;
        if (!keep) up_fd_drop(i);
    }
    for (unsigned k = 0; k < n; k++) {
        int i;
        for (i = 0; i < UP.nfd && UP.fd[i].s != w[k].fd; i++) {}
        if (i == UP.nfd) {
            if (UP.nfd == UP_FDS) continue;
            UP.fd[i].s = w[k].fd;
            UP.fd[i].wev = NULL;
#ifdef _WIN32
            WSAEVENT ev = WSACreateEvent();
            if (ev == WSA_INVALID_EVENT) continue;
            WSAEventSelect(w[k].fd, ev, FD_READ | FD_WRITE | FD_CLOSE | FD_CONNECT);
            UP.fd[i].wev = ev;
#endif
            UP.nfd++;
        }
#ifdef _WIN32
        /* Каждый раз — по маске curl: номер сокета мог достаться новому сокету (закрыт и открыт заново).
         * FD_WRITE — по фронту: повторная регистрация сразу отмечает событие, если сокет готов к записи;
         * поэтому FD_WRITE — только пока curl ждёт записи (иначе пустые пробуждения). */
        else WSAEventSelect(w[k].fd, (WSAEVENT)UP.fd[i].wev,
                            FD_READ | FD_CLOSE | (w[k].events & CURL_WAIT_POLLOUT ? FD_WRITE | FD_CONNECT : 0));
#endif
        UP.fd[i].ev = (short)((w[k].events & CURL_WAIT_POLLIN ? W_IN : 0) | (w[k].events & CURL_WAIT_POLLOUT ? W_OUT : 0));
    }
}
static void up_finish(long status) {
    Req *r = UP.r;
    while (UP.nfd) up_fd_drop(UP.nfd - 1);
    if (r->e) { curl_multi_remove_handle(UP.m, r->e); curl_easy_cleanup(r->e); r->e = NULL; }
    UP.r = NULL;
    r->r.status = status;
    if (g_http_trace) g_http_trace(r->method, r->path, status);
    if (r->cb) r->cb(&r->r, r->ctx);
    free_req(r);
    ui_dirty();
}
static void up_service(void) {
    if (!UP.r) return;
#ifdef _WIN32
    for (int i = 0; i < UP.nfd; i++) WSAResetEvent((WSAEVENT)UP.fd[i].wev);
#endif
    int running = 0;
    curl_multi_perform(UP.m, &running);
    CURLMsg *m;
    int left;
    while ((m = curl_multi_info_read(UP.m, &left))) {
        if (m->msg != CURLMSG_DONE || m->easy_handle != UP.r->e) continue;
        long status = 0;
        if (m->data.result == CURLE_OK) curl_easy_getinfo(UP.r->e, CURLINFO_RESPONSE_CODE, &status);
        if (UP.r->via_vless) vless_report_failure(status > 0 ? NULL : path_error(m->data.result) ? "REST" : NULL);
        if (status == 401) g_auth_failed = true;
        up_finish(status);
        return;
    }
    up_fds_refresh();
}
static i64 up_next_wake(void) {
    if (!UP.r) return INT64_MAX;
    long ms = -1;
    curl_multi_timeout(UP.m, &ms);
    if (ms < 0 || ms > 1000) ms = 1000;
    return now_ms() + ms;
}
int http_waitables(Waitable *out, int max) {
    int n = 0;
    for (int i = 0; i < UP.nfd && n < max; i++) {
#ifdef _WIN32
        out[n++] = (Waitable){ (intptr_t)UP.fd[i].wev, W_IN };
#else
        out[n++] = (Waitable){ (intptr_t)UP.fd[i].s, UP.fd[i].ev };
#endif
    }
    return n;
}
bool file_sending(int *percent) {
    Req *r = UP.r;
    if (percent) *percent = r && r->fsize > 0 ? (int)(r->fsent * 100 / r->fsize) : 0;
    return r != NULL;
}
bool send_file(u64 channel_id, const char *path, const char *text, char *err, size_t errn) {
    return send_file_ex(channel_id, path, text, false, NULL, err, errn);
}
/* №65/67: spoiler — имя с SPOILER_ (картинка скрыта до щелчка); alt — описание вложения (до 1024 символов) */
bool send_file_ex(u64 channel_id, const char *path, const char *text, bool spoiler, const char *alt, char *err, size_t errn) {
    if (!err || !errn) return false;
    err[0] = 0;
    Channel *c = chan_find(channel_id);
    if (!c) { snprintf(err, errn, ui_tr("сначала откройте канал")); return false; }
    if (!c->visible || !c->can_send || c->type == CH_CATEGORY || c->type == CH_VOICE || c->type == CH_STAGE) {
        snprintf(err, errn, ui_tr("в этом канале писать нельзя"));
        return false;
    }
    if (!path || !*path) { snprintf(err, errn, ui_tr("укажите путь к файлу")); return false; }
    if (UP.r) { snprintf(err, errn, ui_tr("уже идёт отправка файла")); return false; }
    if (!O.demo && !route_ok()) { snprintf(err, errn, ui_tr("VLESS не работает: напрямую не отправляю")); return false; }
    /* путь можно вставить в кавычках (как копирует Проводник) */
    char p[1024];
    size_t pl = strlen(path);
    if (pl >= sizeof p) { snprintf(err, errn, ui_tr("слишком длинный путь")); return false; }
    memcpy(p, path, pl + 1);
    if (pl >= 2 && p[0] == '"' && p[pl - 1] == '"') { memmove(p, p + 1, pl - 2); p[pl - 2] = 0; }
    /* Сетевые пути (UNC \\сервер\папка, //сервер/папка, \\?\UNC\...) не принимаем: Windows открыла бы
     * SMB-соединение мимо VLESS и отдала бы чужому серверу хэш NTLM. Только локальные файлы. */
    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
        snprintf(err, errn, ui_tr("сетевые пути не поддерживаются: только локальный файл"));
        return false;
    }
    FILE *fp = NULL;
    i64 size = 0;
    int rc = open_file(p, &fp, &size);
    if (rc == 4) { snprintf(err, errn, ui_tr("сетевые пути и диски не поддерживаются: только локальный файл")); return false; }
    if (rc == 1) { snprintf(err, errn, ui_tr("файл не найден")); return false; }
    if (rc == 2) { snprintf(err, errn, ui_tr("это папка, нужен файл")); return false; }
    if (rc) { snprintf(err, errn, ui_tr("файл не открывается")); return false; }
    if (size > (i64)FILE_MAX) { fclose(fp); snprintf(err, errn, ui_tr("больше 10 МБ (%.1f МБ)"), size / 1048576.0); return false; }
    /* имя файла для Discord: только последняя часть пути, очищенная, без кавычек и обратных слэшей */
    const char *base = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    char *name = san(*base ? base : "file", false);
    for (char *q = name; *q; q++) if (*q == '"' || *q == '\\' || *q == ';') *q = '_';
    if (spoiler && strncmp(name, "SPOILER_", 8)) {   /* №65 */
        char *sp = xmalloc(strlen(name) + 9);
        memcpy(sp, "SPOILER_", 8);
        memcpy(sp + 8, name, strlen(name) + 1);
        free(name);
        name = sp;
    }
    char *desc = NULL;                          /* №67: описание (alt) — очищенное, до 1024 символов */
    if (alt && *alt) {
        desc = san(alt, false);
        size_t k = 0, ch = 0;
        while (desc[k] && ch < 1024) { k++; while (((unsigned char)desc[k] & 0xC0) == 0x80) k++; ch++; }
        desc[k] = 0;
    }
    if (O.demo) {
        fclose(fp);
        demo_file(channel_id, name, (long long)size, text);
        free(name);
        free(desc);
        return true;
    }
    if (g_readonly) { fclose(fp); free(name); free(desc); snprintf(err, errn, ui_tr("режим только чтения")); return false; }
    char path_api[64];
    snprintf(path_api, sizeof path_api, "/channels/%llu/messages", (unsigned long long)channel_id);
    Req *r = req_new("POST", path_api, NULL, on_file_sent, NULL);
    if (!r) { fclose(fp); free(name); free(desc); snprintf(err, errn, ui_tr("не отправлено")); return false; }
    unsigned char rnd[12];
    if (!os_random(rnd, sizeof rnd)) { u64 t = (u64)now_ms(); memcpy(rnd, &t, 8); memcpy(rnd + 8, &r, 4); }
    char bnd[48] = "astrads";
    for (int i = 0; i < 12; i++) snprintf(bnd + 7 + i * 2, 3, "%02x", rnd[i]);
    snprintf(h_mp, sizeof h_mp, "Content-Type: multipart/form-data; boundary=%s", bnd);
    buf_printf(&r->head, "--%s\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n", bnd);
    buf_puts(&r->head, "{\"content\":");
    buf_json_str(&r->head, text ? text : "");
    buf_puts(&r->head, ",\"attachments\":[{\"id\":0,\"filename\":");
    buf_json_str(&r->head, name);
    if (desc) { buf_puts(&r->head, ",\"description\":"); buf_json_str(&r->head, desc); free(desc); desc = NULL; }
    buf_printf(&r->head, "}]}\r\n--%s\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"%s\"\r\n"
               "Content-Type: application/octet-stream\r\n\r\n", bnd, name);
    buf_printf(&r->tail, "\r\n--%s--\r\n", bnd);
    free(name);
    r->fp = fp;
    r->fsize = size;
    if (!UP.m) UP.m = curl_multi_init();
    if (UP.m) start_req(r, UP.m);
    if (!r->e) { free_req(r); snprintf(err, errn, ui_tr("не отправлено: нет памяти или маршрута")); return false; }
    UP.r = r;
    up_service();                                /* первый шаг сразу: curl назовёт сокеты */
    return true;
}

void http_service(void) {
    if (!g_multi) g_multi = curl_multi_init();
    up_service();
    cap_service();
    i64 now = now_ms();
    /* VLESS включён, но не работает: очередь ждёт (интерфейс предложит выбор), напрямую не идём */
    i64 wk;
    Req **pp = !q_active && q_head && route_ok() ? q_pick(now, &wk) : NULL;
    if (pp) {
        q_active = *pp;
        *pp = q_active->next;
        if (q_tail == q_active) {                /* хвост — последний оставшийся */
            q_tail = q_head;
            while (q_tail && q_tail->next) q_tail = q_tail->next;
        }
        q_active->next = NULL;
        buf_free(&q_active->r.body);
        q_active->attempts++;
        start_req(q_active, g_multi);
        if (!q_active->e) {                      /* curl не создал запрос: ответ «нет связи», очередь идёт дальше */
            Req *r = q_active;
            q_active = NULL;
            r->r.status = 0;
            if (r->cb) r->cb(&r->r, r->ctx);
            free_req(r);
            return;
        }
    }
    if (!q_active) return;
    int running = 0;
    curl_multi_perform(g_multi, &running);
    CURLMsg *m;
    int left;
    while ((m = curl_multi_info_read(g_multi, &left))) {
        if (m->msg != CURLMSG_DONE || !q_active || m->easy_handle != q_active->e) continue;
        Req *r = q_active;
        long status = 0;
        if (m->data.result == CURLE_OK) curl_easy_getinfo(r->e, CURLINFO_RESPONSE_CODE, &status);
        if (r->via_vless) vless_report_failure(status > 0 ? NULL : path_error(m->data.result) ? "REST" : NULL);
        curl_multi_remove_handle(g_multi, r->e);
        curl_easy_cleanup(r->e);
        r->e = NULL;
        q_active = NULL;
        r->r.status = status;
        if (status == 429 && r->attempts < 3) {
            double after = 1.0;
            cJSON *j = json_parse(r->r.body.p ? r->r.body.p : "", r->r.body.len);
            const cJSON *ra = jget(j, "retry_after");
            if (cJSON_IsNumber(ra)) after = ra->valuedouble;
            bool global = jbool(j, "global");
            bool slowmode = jint(j, "code", 0) == 20016;
            json_done();
            if (slowmode) goto deliver;          /* №127: медленный режим канала — не повторять, ответ отправителю */
            if (after > 60) after = 60;
            i64 until = now_ms() + (i64)(after * 1000) + 100;
            if (global) g_rate_until = until;
            r->not_before = until;
            r->next = q_head;                    /* в начало очереди: порядок сохраняется */
            q_head = r;
            if (!q_tail) q_tail = r;
            ui_notice(ui_tr("лимит запросов Discord: повтор через %.1f с"), after);
            continue;
        }
    deliver:
        if (status == 401) g_auth_failed = true;
        if (cap_take(r)) continue;               /* капча: запрос ждёт проверки пользователя */
        if (g_http_trace) g_http_trace(r->method, r->path, status);
        cap_done(r);
        if (r->cb) r->cb(&r->r, r->ctx);
        free_req(r);
    }
}

static i64 q_next_wake(void) {
    if (q_active) return now_ms() + 10;         /* опрос только пока идёт запрос */
    if (q_head) {
        if (!route_ok()) return INT64_MAX;       /* разбудит vless (wake_signal) при смене состояния */
        i64 now = now_ms(), wk;
        if (q_pick(now, &wk)) return now;
        return wk;
    }
    return INT64_MAX;
}
i64 http_next_wake(void) {
    i64 a = q_next_wake(), b = up_next_wake();
    return a < b ? a : b;
}

/* ======================= Gateway ======================= */
/* GW_CONNECTING: TCP, TLS и Upgrade идут в curl_multi по шагам из главного цикла — ввод,
 * паник-кей и выход работают и во время подключения (раньше curl_easy_perform держал цикл до 20 с). */
typedef enum { GW_OFF, GW_WAIT, GW_CONNECTING, GW_LIVE } GwState;
static struct {
    GwState st;
    CURLM *wm;                                  /* свой multi: хэндл WebSocket живёт в нём всю сессию */
    CURL *ws;
    bool via_vless;
    bool vwait;                                 /* ждём VLESS: напрямую не подключаемся */
    SockWait *sw;
    z_stream z;
    bool z_ok;
    Buf frame;                                  /* сжатые байты текущего сообщения */
    Buf text;                                   /* распакованный JSON */
    i64 seq;                                    /* -1 = нет */
    char session[96];
    char resume_url[160];
    i64 hb_interval, next_hb, hb_sent;
    bool acked;
    i64 retry_at;
    int backoff;
    char status[96];
    bool want_identify_delay;
    bool drain;                                 /* после рукопожатия: прочитать то, что curl уже принял */
} G = { .seq = -1 };

/* Потолок 15 с: после обрыва сети (без события «сеть вернулась») связь восстанавливается не позже
 * чем через 15 с; попытка подключения в это время почти ничего не стоит. */
static const int BACKOFF[] = { 1000, 2000, 5000, 10000, 15000 };

const char *gw_status_text(void) { return G.status; }
bool gw_online(void) { return G.st == GW_LIVE && G.ws != NULL; }
int gw_waitables(Waitable *out, int max) {
    if (G.st != GW_LIVE || !G.sw || max < 1) return 0;
    out[0] = sockwait_get(G.sw);
    return 1;
}

static bool host_allowed_gw(const char *url) {
    /* wss://<что-то>.discord.gg без порта, пути, логина и прочего */
    if (strncmp(url, "wss://", 6)) return false;
    const char *h = url + 6;
    size_t n = strcspn(h, "/?#");
    if (n == 0 || n > 100) return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    return n > 11 && !strncmp(h + n - 11, ".discord.gg", 11);
}

static void gw_close(void) {
    if (G.sw) { sockwait_free(G.sw); G.sw = NULL; }
    if (G.ws) {
        if (G.wm) curl_multi_remove_handle(G.wm, G.ws);
        curl_easy_cleanup(G.ws);
        G.ws = NULL;
    }
    if (G.z_ok) { inflateEnd(&G.z); G.z_ok = false; }
    buf_free(&G.frame);
    buf_free(&G.text);
    G.next_hb = 0;
    g_gw_ping = -1;
}

static void schedule_retry(const char *why) {
    gw_close();
    if (g_auth_failed) { G.st = GW_OFF; snprintf(G.status, sizeof G.status, ui_tr("токен не принят")); return; }
    int d = BACKOFF[G.backoff < 4 ? G.backoff : 4];
    d += (int)(now_ms() % 500);
    if (G.backoff < 4) G.backoff++;
    G.retry_at = now_ms() + d;
    G.st = GW_WAIT;
    snprintf(G.status, sizeof G.status, ui_tr("%s; повтор через %d с"), why, (d + 999) / 1000);
    ui_dirty();
}

static bool ws_send_raw(const char *data, size_t n) {
    if (!G.ws) return false;
    size_t off = 0;
    int spins = 0;
    while (off < n) {
        size_t sent = 0;
        CURLcode rc = curl_ws_send(G.ws, data + off, n - off, &sent, 0, CURLWS_TEXT);
        if (rc == CURLE_AGAIN) { if (++spins > 200) return false; plat_sleep_ms(5); continue; }
        if (rc != CURLE_OK) return false;
        off += sent;
    }
    return true;
}

void gw_send(const char *json) {
    if (G.st == GW_LIVE && !ws_send_raw(json, strlen(json))) schedule_retry(ui_tr("связь потеряна"));
}

static void send_heartbeat(void) {
    char b[64];
    if (G.seq >= 0) snprintf(b, sizeof b, "{\"op\":1,\"d\":%lld}", (long long)G.seq);
    else snprintf(b, sizeof b, "{\"op\":1,\"d\":null}");
    G.acked = false;
    G.hb_sent = now_ms();
    gw_send(b);
}

/* IDENTIFY и RESUME собираются в защищённой памяти и затираются сразу после отправки. */
static void send_identify(void) {
    size_t cap;
    char *s = secret_scratch(&cap);
    int n = snprintf(s, cap,
        "{\"op\":2,\"d\":{\"token\":\"%s\",\"capabilities\":4605,\"properties\":%s,"
        "\"presence\":{\"activities\":[],\"status\":\"%s\",\"afk\":false,\"since\":0},\"compress\":false,"
        "\"client_state\":{\"guild_hashes\":{},\"highest_last_message_id\":\"0\",\"read_state_version\":0,"
        "\"user_guild_settings_version\":-1,\"user_settings_version\":-1}}}",
        secret_token(), PROPS, presence_word());
    if (n > 0 && (size_t)n < cap) ws_send_raw(s, (size_t)n);
    wipe(s, cap);
    G.seq = -1;
}
static void send_resume(void) {
    size_t cap;
    char *s = secret_scratch(&cap);
    int n = snprintf(s, cap, "{\"op\":6,\"d\":{\"token\":\"%s\",\"session_id\":\"%s\",\"seq\":%lld}}",
                     secret_token(), G.session, (long long)G.seq);
    if (n > 0 && (size_t)n < cap) ws_send_raw(s, (size_t)n);
    wipe(s, cap);
}

static void gw_connect(void) {
    gw_close();
    G.vwait = false;
    if (!route_ok()) {
        /* VLESS включён, но не работает: ждём его (vless разбудит цикл), интерфейс предложит выбор */
        G.st = GW_WAIT;
        G.vwait = true;
        snprintf(G.status, sizeof G.status, ui_tr("ожидание VLESS"));
        ui_dirty();
        return;
    }
    char url[256];
    bool resume = G.session[0] && G.seq >= 0 && G.resume_url[0];
    snprintf(url, sizeof url, "%s" GW_QUERY, resume ? G.resume_url : GW_DEFAULT);
    snprintf(G.status, sizeof G.status, ui_tr("подключение..."));
    ui_dirty();
    if (!G.wm) G.wm = curl_multi_init();
    G.ws = G.wm ? curl_easy_init() : NULL;
    if (!G.ws) { schedule_retry(ui_tr("нет памяти")); return; }
    net_curl_common(G.ws);
    if (!set_route(G.ws, &G.via_vless)) { schedule_retry(ui_tr("нет маршрута")); return; }
    static struct curl_slist o = { "Origin: https://discord.com", NULL };
    curl_easy_setopt(G.ws, CURLOPT_URL, url);
    curl_easy_setopt(G.ws, CURLOPT_HTTPHEADER, &o);
    curl_easy_setopt(G.ws, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(G.ws, CURLOPT_TIMEOUT, 20L);
    if (curl_multi_add_handle(G.wm, G.ws) != CURLM_OK) { schedule_retry(ui_tr("нет памяти")); return; }
    G.st = GW_CONNECTING;
}

/* Шаг подключения: curl_multi продвигает TCP/SOCKS/TLS/Upgrade без ожидания. */
static void gw_connecting(void) {
    int running = 0;
    curl_multi_perform(G.wm, &running);
    CURLMsg *m;
    int left;
    CURLcode rc = CURLE_AGAIN;
    while ((m = curl_multi_info_read(G.wm, &left)))
        if (m->msg == CURLMSG_DONE && m->easy_handle == G.ws) rc = m->data.result;
    if (rc == CURLE_AGAIN) return;
    if (rc != CURLE_OK) {
        if (G.via_vless && path_error(rc)) vless_report_failure("gateway");
        schedule_retry(ui_tr("нет связи с Discord"));
        return;
    }
    if (G.via_vless) vless_report_failure(NULL);
    /* Хэндл остаётся в G.wm: так curl разрешает curl_ws_send/recv после CONNECT_ONLY в multi. */
    curl_easy_setopt(G.ws, CURLOPT_TIMEOUT, 0L);
    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(G.ws, CURLINFO_ACTIVESOCKET, &sock);
    if (sock == CURL_SOCKET_BAD) { schedule_retry(ui_tr("нет сокета")); return; }
    G.sw = sockwait_new((intptr_t)sock);
    memset(&G.z, 0, sizeof G.z);
    if (inflateInit(&G.z) != Z_OK) { schedule_retry("zlib"); return; }
    G.z_ok = true;
    G.st = GW_LIVE;
    G.hb_interval = 0;
    G.next_hb = now_ms() + 30000;               /* ждём Hello не дольше 30 с */
    /* Hello часто приходит вместе с ответом на рукопожатие и уже лежит в буфере curl: сокет
     * при этом не «готов к чтению», и без явного прохода цикл проспал бы до таймера (30 с). */
    G.drain = true;
}

void gw_start(void) {
    if (O.demo) { snprintf(G.status, sizeof G.status, ui_tr("демо-режим, без сети")); return; }
    G.backoff = 0;
    gw_connect();
}

void gw_stop(void) {
    if (G.ws && G.st == GW_LIVE) {
        size_t sent;
        curl_ws_send(G.ws, "", 0, &sent, 0, CURLWS_CLOSE);
    }
    gw_close();
    G.st = GW_OFF;
}

static void on_close_code(int code) {
    switch (code) {
    case 4004: g_auth_failed = true; schedule_retry(ui_tr("Discord отклонил токен")); return;
    case 4010: case 4011: case 4012: case 4013: case 4014:
        G.st = GW_OFF; gw_close(); snprintf(G.status, sizeof G.status, ui_tr("gateway закрыт (%d)"), code); ui_dirty(); return;
    case 4007: case 4009: G.session[0] = 0; G.seq = -1; break;
    default: break;
    }
    char why[48];
    snprintf(why, sizeof why, ui_tr("соединение закрыто (%d)"), code);
    schedule_retry(why);
}

static void handle_payload(const char *text, size_t len) {
    /* Отсев: dispatch-события, которые не нужны, отбрасываются по типу без разбора JSON. */
    if (!event_wanted(text, len)) {
        /* seq всё равно нужен для RESUME: он в начале или конце, ищем дёшево */
        const char *s = NULL;
        for (size_t i = 0; i + 4 < len && i < 64; i++) if (!memcmp(text + i, "\"s\":", 4)) { s = text + i + 4; break; }
        if (!s) goto full;                       /* в хвосте мог оказаться чужой текст — не гадаем */
        if (*s >= '0' && *s <= '9') G.seq = strtoll(s, NULL, 10);
        return;
    }
full:;
    cJSON *j = json_parse(text, len);
    if (!j) { json_done(); return; }
    int op = (int)jint(j, "op", -1);
    const cJSON *d = jget(j, "d");
    const cJSON *sv = jget(j, "s");
    if (cJSON_IsNumber(sv)) G.seq = (i64)sv->valuedouble;
    switch (op) {
    case 10: {
        G.hb_interval = jint(d, "heartbeat_interval", 41250);
        if (G.hb_interval < 1000) G.hb_interval = 1000;
        G.next_hb = now_ms() + (i64)(G.hb_interval * ((now_ms() % 1000) / 1000.0));
        G.acked = true;
        if (G.session[0] && G.seq >= 0) send_resume(); else send_identify();
        snprintf(G.status, sizeof G.status, ui_tr("вход..."));
        ui_dirty();
        break;
    }
    case 11:
        G.acked = true;
        g_gw_ping = (int)(now_ms() - G.hb_sent);
        break;
    case 1: send_heartbeat(); break;
    case 7: json_done(); schedule_retry(ui_tr("Discord попросил переподключиться")); return;
    case 9:
        if (!cJSON_IsTrue(d)) { G.session[0] = 0; G.seq = -1; }
        json_done();
        G.backoff = 1;
        schedule_retry(ui_tr("сессия недействительна"));
        return;
    case 0: {
        const char *t = jstr(j, "t");
        if (!t) break;
        if (!strcmp(t, "READY")) {
            const char *sid = jstr(d, "session_id");
            const char *ru = jstr(d, "resume_gateway_url");
            snprintf(G.session, sizeof G.session, "%s", sid ? sid : "");
            G.resume_url[0] = 0;
            if (ru && host_allowed_gw(ru) && strlen(ru) < sizeof G.resume_url) {
                size_t n = strcspn(ru + 6, "/?#") + 6;
                memcpy(G.resume_url, ru, n);
                G.resume_url[n] = 0;
            }
            store_ready(d);
            G.backoff = 0;
            snprintf(G.status, sizeof G.status, ui_tr("в сети"));
            ui_on_ready();
        } else if (!strcmp(t, "RESUMED")) {
            G.backoff = 0;
            snprintf(G.status, sizeof G.status, ui_tr("в сети"));
            ui_dirty();
        } else {
            store_event(t, d);
        }
        break;
    }
    default: break;
    }
    json_done();
}

/* {"t":"READY",...} — в первых байтах (Discord кладёт t первым) */
static bool is_ready(const char *t, size_t n) {
    size_t lim = n < 64 ? n : 64;
    for (size_t i = 0; i + 12 < lim; i++)
        if (!memcmp(t + i, "\"t\":\"READY\"", 11)) return true;
    return false;
}
static void handle_ready(char *text, size_t len) {
    for (size_t i = 0; i + 4 < len && i < 64; i++)
        if (!memcmp(text + i, "\"s\":", 4)) { if (text[i + 4] >= '0' && text[i + 4] <= '9') G.seq = strtoll(text + i + 4, NULL, 10); break; }
    char ru[256];
    if (!store_ready_text(text, len, ru, sizeof ru)) { schedule_retry(ui_tr("READY не разобран")); return; }
    snprintf(G.session, sizeof G.session, "%s", S.session_id ? S.session_id : "");
    G.resume_url[0] = 0;
    if (ru[0] && host_allowed_gw(ru) && strlen(ru) < sizeof G.resume_url) {
        size_t n = strcspn(ru + 6, "/?#") + 6;
        memcpy(G.resume_url, ru, n);
        G.resume_url[n] = 0;
    }
    G.backoff = 0;
    snprintf(G.status, sizeof G.status, ui_tr("в сети"));
    ui_on_ready();
}

static void inflate_frame(void) {
    /* Сообщение zlib-stream заканчивается на 00 00 FF FF; иначе ждём продолжения. */
    if (G.frame.len < 4 || memcmp(G.frame.p + G.frame.len - 4, "\x00\x00\xff\xff", 4)) return;
    G.text.len = 0;
    G.z.next_in = (Bytef *)G.frame.p;
    G.z.avail_in = (uInt)G.frame.len;
    char out[16384];
    for (;;) {
        G.z.next_out = (Bytef *)out;
        G.z.avail_out = sizeof out;
        int rc = inflate(&G.z, Z_SYNC_FLUSH);
        if (rc != Z_OK && rc != Z_BUF_ERROR) { schedule_retry(ui_tr("ошибка распаковки")); return; }
        size_t got = sizeof out - G.z.avail_out;
        if (got) buf_put(&G.text, out, got);
        if (G.text.len > MAX_PAYLOAD) { schedule_retry(ui_tr("слишком большой пакет")); return; }
        if (G.z.avail_in == 0 && G.z.avail_out != 0) break;
        if (rc == Z_BUF_ERROR && got == 0) break;
    }
    G.frame.len = 0;
    if (G.text.len) {
        /* READY / READY_SUPPLEMENTAL: ненужные поля вырезаются из текста до cJSON (пик памяти) */
        size_t n = ready_slim(G.text.p, G.text.len);
        if (!n) { schedule_retry(ui_tr("READY не разобран")); return; }
        G.text.len = n;
        if (is_ready(G.text.p, n)) handle_ready(G.text.p, n);   /* по одному серверу, без дерева всего пакета */
        else handle_payload(G.text.p, G.text.len);
    }
    /* После READY (мегабайты) буферы не держим на пике. */
    /* после крупного пакета (READY, список участников) буферы не держим: порог 64 КБ (было 1 МБ — до 2 МБ навсегда) */
    if (G.frame.cap > (64u << 10)) buf_free(&G.frame);
    if (G.text.cap > (64u << 10)) buf_free(&G.text);
}

void gw_service(void) {
    i64 now = now_ms();
    if (G.st == GW_WAIT && (G.vwait ? route_ok() : now >= G.retry_at)) { gw_connect(); return; }
    if (G.st == GW_CONNECTING) { gw_connecting(); if (G.st != GW_LIVE) return; }
    if (G.st != GW_LIVE) return;
    sockwait_clear(G.sw);
    G.drain = false;
    /* Приём прямо в хвост G.frame (без отдельного буфера 64 КБ): держим запас 16 КБ свободного места.
     * Управляющие кадры (ping/pong/close) пишутся туда же, но длина кадра не сдвигается. */
    enum { CHUNK = 16384 };
    for (int guard = 0; guard < 4096 && G.st == GW_LIVE; guard++) {
        if (G.frame.cap - G.frame.len < CHUNK + 1) {
            size_t c = G.frame.cap ? G.frame.cap : CHUNK * 2;
            while (c - G.frame.len < CHUNK + 1) c *= 2;
            char *q = xmalloc(c);
            if (G.frame.p) { memcpy(q, G.frame.p, G.frame.len); wipe(G.frame.p, G.frame.cap); free(G.frame.p); }
            G.frame.p = q;
            G.frame.cap = c;
        }
        char *dst = G.frame.p + G.frame.len;
        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode rc = curl_ws_recv(G.ws, dst, CHUNK, &got, &meta);
        if (rc == CURLE_AGAIN) break;
        if (rc != CURLE_OK || !meta) { schedule_retry(ui_tr("связь потеряна")); return; }
        if (meta->flags & CURLWS_CLOSE) {
            int code = got >= 2 ? ((unsigned char)dst[0] << 8 | (unsigned char)dst[1]) : 1000;
            on_close_code(code);
            return;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;
        if (got) {
            if (G.frame.len + got > MAX_FRAME) { schedule_retry(ui_tr("слишком большой кадр")); return; }
            G.frame.len += got;
            G.frame.p[G.frame.len] = 0;
        }
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
            inflate_frame();
            if (G.st != GW_LIVE) return;
        }
    }
    now = now_ms();
    if (G.next_hb && now >= G.next_hb) {
        if (!G.hb_interval) { schedule_retry(ui_tr("нет приветствия")); return; }
        if (!G.acked) { schedule_retry(ui_tr("сервер не отвечает")); return; }
        send_heartbeat();
        G.next_hb = now + G.hb_interval;
    }
}

i64 gw_next_wake(void) {
    if (G.st == GW_WAIT && G.vwait) return route_ok() ? now_ms() : INT64_MAX;
    if (G.st == GW_WAIT) return G.retry_at;
    if (G.st == GW_CONNECTING) return now_ms() + 20;    /* подключение: опрос curl, как у REST */
    if (G.st == GW_LIVE && G.drain) return now_ms();
    if (G.st == GW_LIVE && G.next_hb) return G.next_hb;
    return INT64_MAX;
}

void gw_subscribe(u64 guild, u64 channel) {
    if (!guild || !channel || G.st != GW_LIVE) return;
    char b[320];
    snprintf(b, sizeof b,
        "{\"op\":14,\"d\":{\"guild_id\":\"%llu\",\"typing\":true,\"threads\":false,\"activities\":false,"
        "\"members\":[],\"channels\":{\"%llu\":[[0,99]]},\"thread_member_lists\":[]}}",
        (unsigned long long)guild, (unsigned long long)channel);
    gw_send(b);
}

#ifdef ASTRA_VIDEO
extern bool g_gw_self_video;                    /* voice_cam.c: камера включена */
#endif
void gw_voice_state(u64 guild, u64 channel, bool mute, bool deaf) {
    if (G.st != GW_LIVE) return;
    char g[32], c[32], b[256];
    if (guild) snprintf(g, sizeof g, "\"%llu\"", (unsigned long long)guild); else snprintf(g, sizeof g, "null");
    if (channel) snprintf(c, sizeof c, "\"%llu\"", (unsigned long long)channel); else snprintf(c, sizeof c, "null");
#ifdef ASTRA_VIDEO
    bool video = channel && g_gw_self_video;
    snprintf(b, sizeof b,
        "{\"op\":4,\"d\":{\"guild_id\":%s,\"channel_id\":%s,\"self_mute\":%s,\"self_deaf\":%s,\"self_video\":%s,\"flags\":2}}",
        g, c, mute ? "true" : "false", deaf ? "true" : "false", video ? "true" : "false");
#else
    snprintf(b, sizeof b,
        "{\"op\":4,\"d\":{\"guild_id\":%s,\"channel_id\":%s,\"self_mute\":%s,\"self_deaf\":%s,\"self_video\":false,\"flags\":2}}",
        g, c, mute ? "true" : "false", deaf ? "true" : "false");
#endif
    gw_send(b);
}

void net_cleanup(void) {
    gw_stop();
    while (q_head) { Req *r = q_head; q_head = r->next; free_req(r); }
    if (q_active) { curl_multi_remove_handle(g_multi, q_active->e); curl_easy_cleanup(q_active->e); free_req(q_active); q_active = NULL; }
    if (UP.r) { UP.r->cb = NULL; up_finish(0); }   /* выход: без уведомлений */
    if (CP.r) { Req *r = CP.r; cap_clear(); free_req(r); }
    if (UP.m) { curl_multi_cleanup(UP.m); UP.m = NULL; }
    if (g_multi) curl_multi_cleanup(g_multi);
    if (G.wm) { curl_multi_cleanup(G.wm); G.wm = NULL; }
    curl_global_cleanup();
}
