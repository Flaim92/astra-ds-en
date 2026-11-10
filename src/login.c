/* Вход в аккаунт Discord: движок без интерфейса (экраны — ui.c, по login_view()). См. login.h.
 *
 * Парольная ветка (REST, по одному запросу за раз, curl_multi без блокировки цикла):
 *   [GET /experiments -> X-Fingerprint] -> POST /auth/login {login, password, undelete, login_source, gift_code_sku_id}
 *     token                         -> готово
 *     mfa + ticket (+ totp/sms/backup/webauthn) -> код: POST /auth/mfa/{totp|sms|backup} {code, ticket, login_instance_id, ...}
 *                                      SMS: POST /auth/mfa/sms/send {ticket} -> {phone}
 *     400 captcha_key/captcha_sitekey -> капча: решить нельзя, предлагаем QR (он запускается сам)
 *     400 ACCOUNT_LOGIN_VERIFICATION_EMAIL -> «проверьте почту»: подтвердить письмом и повторить вход
 *         (или вставить ссылку из письма: POST /auth/authorize-ip {token} -> повтор входа)
 *     400 code 70007                -> код из SMS: POST /phone-verifications/verify {phone, code} -> authorize-ip -> повтор
 *     code 20013/20011, ACCOUNT_DISABLED/SCHEDULED_FOR_DELETION -> предложить undelete:true
 *     INVALID_LOGIN / PASSWORD_DOES_NOT_MATCH и прочие ошибки полей -> ошибка у поля
 *     403 suspended_user_token      -> аккаунт заблокирован (токен ограниченный, им не входим)
 *     429 retry_after               -> «подождите N с», без автоповтора (защита от перебора на стороне Discord)
 * QR-ветка (remote auth v2): wss://remote-auth-gateway.discord.gg/?v=2, Origin https://discord.com
 *   hello{heartbeat_interval, timeout_ms} -> init{encoded_public_key = base64(SPKI DER RSA-2048)}
 *   nonce_proof{encrypted_nonce} -> nonce_proof{nonce = base64url(RSA-OAEP-SHA256^-1(nonce)) без '='}
 *   pending_remote_init{fingerprint = base64url(SHA256(SPKI))} -> ссылка https://discord.com/ra/<fingerprint>
 *     (QR из неё строит кодер в ui.c)
 *   pending_ticket{encrypted_user_payload} -> "id:discriminator:avatar:username" -> «Проверьте телефон»
 *   pending_login{ticket} -> POST /users/@me/remote-auth/login {ticket} -> encrypted_token -> расшифровать
 *   cancel -> новый QR; закрытие 4003 (сессия истекла) -> новый QR (до 3 раз подряд), затем «устарел». */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>
#include "mc.h"
#include "plat.h"
#include "login.h"
#include "captcha.h"
#include "impcurl.h"

/* Временно здесь, до переноса в mc.h координатором (раздел net.c): */
void net_curl_common(CURL *e);

/* libcurl-impersonate: выбранный на этот вход curl для auth-эндпоинтов. NULL — обычный curl. */
static const ImpCurl *VT;

#define API "https://discord.com/api/v9"
#define RA_URL "wss://remote-auth-gateway.discord.gg/?v=2"
#define RA_PREFIX "https://discord.com/ra/"
#define QR_AUTO_RENEW 3                         /* истёкший QR обновляется сам не больше 3 раз подряд */

/* ---------- закреплённая область: всё секретное ---------- */
#define CAP_LOGIN 320
#define CAP_PASS 320
#define CAP_CODE 1024                           /* код 2FA/SMS или ссылка из письма */
#define CAP_TOKEN 256
typedef struct {
    char login[CAP_LOGIN];
    char pass[CAP_PASS];
    char code[CAP_CODE];
    char token[CAP_TOKEN];
    char ticket[1024];                          /* тикет MFA */
    char lii[128];                              /* login_instance_id */
    char ra_ticket[2048];                       /* тикет remote auth (pending_login) */
    char ipt[1024];                             /* токен authorize-ip (из письма или после проверки телефона) */
    char body[6144];                            /* тело запроса (с паролем/кодом/тикетом) */
    char resp[16384];                           /* ответ REST (с токеном) */
    char ws[8192];                              /* кадр шлюза QR (тикет) */
    char ckey[16384];                           /* ответ hCaptcha (h-captcha-response) до повтора запроса */
    unsigned char bin[512];                     /* шифротекст RSA */
    unsigned char pt[512];                      /* расшифровка RSA (токен, nonce, данные пользователя) */
} Locked;

enum { R_NONE = 0, R_RA = 1, R_FP = 2, R_LOGIN = 4, R_MFA = 8, R_SMS = 16, R_AUTHIP = 32, R_PHONEV = 64, R_TOKEN = 128 };

static struct {
    bool on, selftest;
    Locked *k;
    size_t ksz;
    bool sheap;                                 /* защищённая куча OpenSSL поднята нами */
    LoginStage stage, back;                     /* back — куда вернуться при ошибке запроса */
    char err[256], notice[512], busy[64], phone[48], csvc[24];
    int err_field;
    bool err_both;                              /* INVALID_LOGIN: ошибка и у логина, и у пароля */
    i64 wait_until;
    bool route_wait;
    unsigned mfa_methods, mfa_method;
    bool sms_sent, undelete;
    char fp[80];                                /* X-Fingerprint (не секрет) */
    bool fp_tried;
    char cookie[4][2][256];                     /* куки Discord из ответов (__dcfduid, __sdcfduid, __cfruid, _cfuvid), только в памяти.
                                                   256: _cfuvid/__sdcfduid длиннее 96 — иначе отбрасывались и MFA видел «нового посетителя» CF */
    /* капча: что повторить после проверки */
    int cap_kind;                               /* R_* запроса, на который пришла капча; 0 — нет */
    bool cap_key, cap_tried, cap_open;
    char csite[64], csess[128], crqdata[4096], crqtoken[2048];
    /* REST */
    unsigned want;
    CURLM *m;
    CURL *e;
    int kind;
    bool via;
    struct curl_slist *hdr, n_auth;
    size_t rlen;
    bool rover;
    LoginView v;
} L;

static struct {
    QrState st;
    bool need;                                  /* подключиться при первой возможности */
    CURLM *wm;
    CURL *ws;
    bool live, via, drain, got_login;
    SockWait *sw;
    EVP_PKEY *key;
    char fpr[48];
    char url[96];                               /* https://discord.com/ra/<отпечаток> */
    i64 hb_iv, next_hb, expires;
    bool acked;
    size_t flen;
    int renew;
    char text[256], uname[96], discrim[8], avatar[40];
    u64 uid;
} Q;

/* ---------- мелочи ---------- */
static i64 now(void) { return now_ms(); }
static bool route_ok(void) { return vless_state() == VL_OFF || vless_proxy_url() != NULL; }
static void kick(void) { if (g_wake) wake_signal(g_wake); ui_dirty(); }

static void set_err(int field, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_err(int field, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(L.err, sizeof L.err, fmt, ap);
    va_end(ap);
    L.err_field = field;
    L.err_both = false;
}
static void clear_err(void) { L.err[0] = 0; L.err_field = LF_NONE; L.err_both = false; }

/* Строка из сети -> san() -> копия с обрезкой по границе UTF-8. */
static void copy_san(char *dst, size_t cap, const char *src) {
    char *s = san(src ? src : "", false);
    size_t n = strlen(s);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, s, n);
    dst[n] = 0;
    free(s);
}

static bool charset_ok(const char *s, size_t n, const char *extra) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (extra && c && strchr(extra, c))))
            return false;
    }
    return true;
}

/* Затереть все строки разобранного JSON (арена освобождается без затирания). */
static void jwipe(cJSON *j) {
    for (; j; j = j->next) {
        if (j->valuestring) wipe(j->valuestring, strlen(j->valuestring));
        if (j->string) wipe(j->string, strlen(j->string));
        if (j->child) jwipe(j->child);
    }
}
static void jdone(cJSON *j) { jwipe(j); json_done(); }

/* Тело JSON прямо в закреплённом буфере. */
typedef struct { char *p; size_t cap, n; bool bad; } Out;
static void o_raw(Out *o, const char *s) {
    size_t n = strlen(s);
    if (o->bad || o->n + n >= o->cap) { o->bad = true; return; }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}
static void o_str(Out *o, const char *s) {
    o_raw(o, "\"");
    for (; *s && !o->bad; s++) {
        unsigned char c = (unsigned char)*s;
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; e[2] = 0; }
        else if (c < 0x20) snprintf(e, sizeof e, "\\u%04x", c);
        else { e[0] = (char)c; e[1] = 0; }
        o_raw(o, e);
        wipe(e, sizeof e);
    }
    o_raw(o, "\"");
}
static void o_kv(Out *o, const char *k, const char *v) { o_raw(o, "\""); o_raw(o, k); o_raw(o, "\":"); if (v) o_str(o, v); else o_raw(o, "null"); }

/* ---------- base64 и RSA ---------- */
static size_t b64_enc(const unsigned char *in, size_t n, char *out, size_t cap, bool url) {
    if (cap < 4 * ((n + 2) / 3) + 1) return 0;
    size_t m = (size_t)EVP_EncodeBlock((unsigned char *)out, in, (int)n);
    if (url) {
        while (m && out[m - 1] == '=') out[--m] = 0;
        for (size_t i = 0; i < m; i++) { if (out[i] == '+') out[i] = '-'; else if (out[i] == '/') out[i] = '_'; }
    }
    return m;
}
static int b64_dec(const char *in, unsigned char *out, size_t cap) {
    size_t n = in ? strlen(in) : 0;
    if (!n || n % 4 || n / 4 * 3 > cap || !charset_ok(in, n, "+/=")) return -1;
    int m = EVP_DecodeBlock(out, (const unsigned char *)in, (int)n);
    if (m < 0) return -1;
    if (in[n - 1] == '=') m--;
    if (in[n - 2] == '=') m--;
    return m;
}

static EVP_PKEY *rsa_new(void) { return EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048); }

/* RSA-OAEP, SHA-256 и MGF1-SHA-256, без метки. Возврат: длина в out или -1. */
static int rsa_crypt(EVP_PKEY *key, bool enc, const unsigned char *in, size_t n, unsigned char *out, size_t cap) {
    EVP_PKEY_CTX *c = EVP_PKEY_CTX_new(key, NULL);
    size_t ol = cap;
    int ok = c && (enc ? EVP_PKEY_encrypt_init(c) : EVP_PKEY_decrypt_init(c)) > 0 &&
             EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_OAEP_PADDING) > 0 &&
             EVP_PKEY_CTX_set_rsa_oaep_md(c, EVP_sha256()) > 0 &&
             EVP_PKEY_CTX_set_rsa_mgf1_md(c, EVP_sha256()) > 0 &&
             (enc ? EVP_PKEY_encrypt(c, out, &ol, in, n) : EVP_PKEY_decrypt(c, out, &ol, in, n)) > 0;
    EVP_PKEY_CTX_free(c);
    return ok && ol < cap ? (int)ol : -1;
}

/* base64 шифротекст -> расшифровка в L.k->pt (с нулём в конце). Длина или -1. */
static int rsa_dec_b64(EVP_PKEY *key, const char *b64) {
    int n = b64_dec(b64, L.k->bin, sizeof L.k->bin);
    int m = n > 0 && key ? rsa_crypt(key, false, L.k->bin, (size_t)n, L.k->pt, sizeof L.k->pt - 1) : -1;
    wipe(L.k->bin, sizeof L.k->bin);
    if (m < 0) { wipe(L.k->pt, sizeof L.k->pt); return -1; }
    L.k->pt[m] = 0;
    return m;
}

/* SPKI DER открытого ключа: base64 (для init) и отпечаток base64url(SHA-256). */
static bool key_spki(EVP_PKEY *key, char *b64, size_t b64cap, char *fpr, size_t fprcap) {
    unsigned char *der = NULL;
    int n = i2d_PUBKEY(key, &der);
    if (n <= 0) return false;
    unsigned char md[32];
    unsigned int ml = 0;
    bool ok = EVP_Digest(der, (size_t)n, md, &ml, EVP_sha256(), NULL) && ml == 32 &&
              b64_enc(der, (size_t)n, b64, b64cap, false) && b64_enc(md, 32, fpr, fprcap, true) == 43;
    OPENSSL_free(der);
    return ok;
}

/* ---------- данные пользователя из pending_ticket: "id:discriminator:avatar:username" ---------- */
static bool parse_user(const char *s, u64 *id, char *discrim, size_t dn, char *avatar, size_t an, char *name, size_t nn) {
    const char *c1 = strchr(s, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL, *c3 = c2 ? strchr(c2 + 1, ':') : NULL;
    if (!c3 || c1 == s || c1 - s > 20 || !charset_ok(s, (size_t)(c1 - s), NULL)) return false;
    for (const char *p = s; p < c1; p++) if (*p < '0' || *p > '9') return false;
    *id = strtoull(s, NULL, 10);
    size_t dl = (size_t)(c2 - c1 - 1), al = (size_t)(c3 - c2 - 1);
    discrim[0] = avatar[0] = 0;
    if (dl && dl < dn && charset_ok(c1 + 1, dl, NULL)) { memcpy(discrim, c1 + 1, dl); discrim[dl] = 0; }
    bool hex = al == 32 || (al == 34 && c2[1] == 'a' && c2[2] == '_');
    for (size_t i = al == 34 ? 2 : 0; hex && i < al; i++) { char c = c2[1 + i]; hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
    if (hex && al < an) { memcpy(avatar, c2 + 1, al); avatar[al] = 0; }
    copy_san(name, nn, c3 + 1);
    return *id != 0;
}

/* Ссылка из письма «подтвердить вход» -> токен authorize-ip (параметр token=, или вставлен сам токен). */
static bool ip_token_from(const char *s, char *out, size_t cap) {
    while (*s == ' ') s++;
    const char *t = strstr(s, "token=");
    t = t ? t + 6 : s;
    size_t n = strcspn(t, "&# \r\n");
    if (n < 16 || n >= cap || !charset_ok(t, n, "._-%")) return false;
    memcpy(out, t, n);
    out[n] = 0;
    return true;
}

/* ======================= REST ======================= */
static size_t rest_write(char *p, size_t sz, size_t n, void *ud) {
    (void)ud;
    size_t t = sz * n;
    if (!L.k) return 0;
    if (L.rlen + t >= sizeof L.k->resp) {
        if (L.kind != R_FP) return 0;           /* ответы входа маленькие; большой — не наш */
        L.rover = true;                         /* /experiments: нужно только начало (fingerprint) */
        size_t room = sizeof L.k->resp - 1 - L.rlen;
        t = room < t ? room : t;
        memcpy(L.k->resp + L.rlen, p, t);
        L.rlen += t;
        return sz * n;
    }
    memcpy(L.k->resp + L.rlen, p, t);
    L.rlen += t;
    return t;
}

/* Куки Discord (как у браузера: их отсутствие — признак бота). curl собран без cookie-движка: берём
 * Set-Cookie вручную, только известные имена, только в памяти этого входа. */
static size_t rest_header(char *p, size_t sz, size_t n, void *ud) {
    (void)ud;
    size_t t = sz * n;
    static const char *const names[4] = { "__dcfduid", "__sdcfduid", "__cfruid", "_cfuvid" };
    if (t > 12 && t < 1024 && !strncasecmp(p, "set-cookie:", 11)) {
        const char *a = p + 11;
        while (a < p + t && *a == ' ') a++;
        const char *eq = memchr(a, '=', (size_t)(p + t - a));
        if (eq) {
            const char *v = eq + 1, *e = v;
            while (e < p + t && *e != ';' && *e != '\r' && *e != '\n') e++;
            for (int i = 0; i < 4; i++) {
                size_t nl = strlen(names[i]), vl = (size_t)(e - v);
                if ((size_t)(eq - a) == nl && !strncmp(a, names[i], nl) && vl && vl < sizeof L.cookie[i][1] && charset_ok(v, vl, "._-=%")) {
                    snprintf(L.cookie[i][0], sizeof L.cookie[i][0], "%s", names[i]);
                    memcpy(L.cookie[i][1], v, vl);
                    L.cookie[i][1][vl] = 0;
                }
            }
        }
    }
    return t;
}

static void rest_abort(void) {
    if (L.e) {
        if (L.m) (VT ? VT->multi_remove_handle : curl_multi_remove_handle)(L.m, L.e);
        (VT ? VT->easy_cleanup : curl_easy_cleanup)(L.e);
        L.e = NULL;
    }
    if (L.hdr) { curl_slist_free_all(L.hdr); L.hdr = NULL; }
    if (L.k) { wipe(L.k->body, sizeof L.k->body); wipe(L.k->resp, sizeof L.k->resp); }
    L.rlen = 0;
    L.kind = R_NONE;
}

static const char *mfa_name(unsigned m) { return m == LM_SMS ? "sms" : m == LM_BACKUP ? "backup" : "totp"; }

/* Тело и путь запроса в закреплённой памяти. false — не влезло. */
static bool build(int kind, char *path, size_t pn, const char **method) {
    Locked *k = L.k;
    Out o = { k->body, sizeof k->body, 0, false };
    k->body[0] = 0;
    *method = "POST";
    switch (kind) {
    case R_FP:
        snprintf(path, pn, "/experiments?with_guild_experiments=false");
        *method = "GET";
        return true;
    case R_TOKEN:
        snprintf(path, pn, "/users/@me");
        *method = "GET";
        return true;
    case R_LOGIN:
        snprintf(path, pn, "/auth/login");
        o_raw(&o, "{"); o_kv(&o, "login", k->login);
        o_raw(&o, ","); o_kv(&o, "password", k->pass);
        o_raw(&o, L.undelete ? ",\"undelete\":true," : ",\"undelete\":false,");
        o_kv(&o, "login_source", NULL); o_raw(&o, ","); o_kv(&o, "gift_code_sku_id", NULL); o_raw(&o, "}");
        break;
    case R_MFA:
        snprintf(path, pn, "/auth/mfa/%s", mfa_name(L.mfa_method));
        o_raw(&o, "{"); o_kv(&o, "code", k->code);
        o_raw(&o, ","); o_kv(&o, "ticket", k->ticket);
        if (k->lii[0]) { o_raw(&o, ","); o_kv(&o, "login_instance_id", k->lii); }
        o_raw(&o, ","); o_kv(&o, "login_source", NULL); o_raw(&o, ","); o_kv(&o, "gift_code_sku_id", NULL); o_raw(&o, "}");
        break;
    case R_SMS:
        snprintf(path, pn, "/auth/mfa/sms/send");
        o_raw(&o, "{"); o_kv(&o, "ticket", k->ticket); o_raw(&o, "}");
        break;
    case R_RA:
        snprintf(path, pn, "/users/@me/remote-auth/login");
        o_raw(&o, "{"); o_kv(&o, "ticket", k->ra_ticket); o_raw(&o, "}");
        break;
    case R_AUTHIP:
        snprintf(path, pn, "/auth/authorize-ip");
        o_raw(&o, "{"); o_kv(&o, "token", k->ipt); o_raw(&o, "}");
        break;
    case R_PHONEV:
        snprintf(path, pn, "/phone-verifications/verify");
        o_raw(&o, "{"); o_kv(&o, "phone", k->login); o_raw(&o, ","); o_kv(&o, "code", k->code); o_raw(&o, "}");
        break;
    default: return false;
    }
    return !o.bad;
}

static void on_resp(int kind, long status);

/* Заголовки как у веб-клиента на странице входа (без X-Debug-Options), плюс ответ капчи при повторе. */
static struct curl_slist *headers(int kind) {
    struct curl_slist *h = NULL;
    if (kind != R_FP && kind != R_TOKEN) h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, net_super_header());
    h = curl_slist_append(h, "Origin: https://discord.com");
    h = curl_slist_append(h, "Referer: https://discord.com/login");
    h = curl_slist_append(h, "X-Discord-Locale: en-US");
    h = curl_slist_append(h, "Accept-Language: en-US,en;q=0.9");
    if (L.fp[0] && kind != R_FP && kind != R_TOKEN) {
        char fh[112];
        snprintf(fh, sizeof fh, "X-Fingerprint: %s", L.fp);
        h = curl_slist_append(h, fh);
    }
    char ck[1024];
    size_t cn = 0;
    ck[0] = 0;
    for (int i = 0; i < 4; i++) if (L.cookie[i][0][0]) {
        int w = snprintf(ck + cn, sizeof ck - cn, "%s%s=%s", cn ? "; " : "Cookie: ", L.cookie[i][0], L.cookie[i][1]);
        if (w > 0 && (size_t)w < sizeof ck - cn) cn += (size_t)w;
    }
    if (cn) h = curl_slist_append(h, ck);
    if (L.cap_key && kind == L.cap_kind && L.k) {   /* повтор после проверки: ответ капчи — в заголовках (поля тела устарели) */
        size_t n = strlen(L.k->ckey) + 32;
        char *x = xmalloc(n);
        snprintf(x, n, "X-Captcha-Key: %s", L.k->ckey);
        h = curl_slist_append(h, x);
        wipe(x, n);
        free(x);
        char y[2200];
        if (L.crqtoken[0]) { snprintf(y, sizeof y, "X-Captcha-Rqtoken: %s", L.crqtoken); h = curl_slist_append(h, y); }
        if (L.csess[0]) { snprintf(y, sizeof y, "X-Captcha-Session-Id: %s", L.csess); h = curl_slist_append(h, y); }
        wipe(L.k->ckey, sizeof L.k->ckey);
        L.cap_key = false;
        L.cap_tried = true;
    }
    return h;
}

static void rest_start(int kind) {
    char path[96], url[160];
    const char *method;
    L.kind = kind;
    L.rlen = 0;
    L.rover = false;
    if (!build(kind, path, sizeof path, &method)) { rest_abort(); on_resp(kind, -1); return; }
    /* Выбор curl — один раз на весь вход. Если impercurl есть, идём через него
     * (Chrome 152 ClientHello + HTTP/1.1): так Discord принимает auth-запросы. */
    if (!L.m) VT = impcurl_vt();
    if (!L.m) L.m = VT ? VT->multi_init() : curl_multi_init();
    CURL *e = L.m ? (VT ? VT->easy_init() : curl_easy_init()) : NULL;
    if (!e) { rest_abort(); on_resp(kind, 0); return; }
    if (VT) {
        /* Импер-curl: TLS, cipher, ALPN, cert-compression — внутри DLL. Нам нужны только
         * маршрут (VLESS) и общие настройки curl. Явно HTTP/1.1 — как в mfa_h1.py. */
        VT->easy_setopt(e, CURLOPT_IMPERSONATE, "chrome152");
        VT->easy_setopt(e, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
        VT->easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
        VT->easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
        VT->easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        VT->easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
        VT->easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
        /* UA согласован с X-Super-Properties (net_super_header): Chrome 152. */
        extern const char *net_user_agent(void);
        VT->easy_setopt(e, CURLOPT_USERAGENT, net_user_agent());
        const char *proxy = vless_state() == VL_OFF ? "" : vless_proxy_url();
        if (!proxy) { VT->easy_cleanup(e); L.kind = R_NONE; L.want |= (unsigned)kind; L.route_wait = true; return; }
        VT->easy_setopt(e, CURLOPT_PROXY, proxy);
        VT->easy_setopt(e, CURLOPT_NOPROXY, "");
        if (proxy[0]) VT->easy_setopt(e, CURLOPT_SOCKS5_AUTH, (long)CURLAUTH_BASIC);
        L.via = proxy[0] != 0;
    } else {
        net_curl_common(e);
        if (!net_route(e, &L.via)) { curl_easy_cleanup(e); L.kind = R_NONE; L.want |= (unsigned)kind; L.route_wait = true; return; }
    }
    snprintf(url, sizeof url, API "%s", path);
    struct curl_slist *h = headers(kind);
    L.hdr = h;
    struct curl_slist *head = h;
    if (kind == R_TOKEN) {                      /* узел Authorization указывает прямо в secret.c */
        L.n_auth.data = (char *)secret_auth_header();
        L.n_auth.next = h;
        head = &L.n_auth;
    }
#define SO(opt, val) (VT ? VT->easy_setopt(e, (opt), (val)) : curl_easy_setopt(e, (opt), (val)))
    SO(CURLOPT_URL, url);
    SO(CURLOPT_HTTPHEADER, head);
    SO(CURLOPT_CUSTOMREQUEST, method);
    SO(CURLOPT_ACCEPT_ENCODING, "");
    SO(CURLOPT_TIMEOUT, 20L);
    SO(CURLOPT_WRITEFUNCTION, rest_write);
    SO(CURLOPT_WRITEDATA, NULL);
    SO(CURLOPT_HEADERFUNCTION, rest_header);
    if (!strcmp(method, "POST")) {               /* тело не копируется curl: отправляется из закреплённой памяти */
        SO(CURLOPT_POSTFIELDS, L.k->body);
        SO(CURLOPT_POSTFIELDSIZE, (long)strlen(L.k->body));
    }
#undef SO
    L.e = e;
    if ((VT ? VT->multi_add_handle(L.m, e) : curl_multi_add_handle(L.m, e)) != CURLM_OK) { rest_abort(); on_resp(kind, 0); }
}

static void rest_step(void) {
    int running = 0;
    (VT ? VT->multi_perform : curl_multi_perform)(L.m, &running);
    CURLMsg *msg;
    int left;
    while ((msg = (VT ? VT->multi_info_read : curl_multi_info_read)(L.m, &left))) {
        if (msg->msg != CURLMSG_DONE || msg->easy_handle != L.e) continue;
        long status = 0;
        CURLcode rc = msg->data.result;
        if (rc == CURLE_OK) (VT ? VT->easy_getinfo : curl_easy_getinfo)(L.e, CURLINFO_RESPONSE_CODE, &status);
        if (L.via) vless_report_failure(status > 0 ? NULL : net_path_error((int)rc) ? ui_tr("вход") : NULL);
        int kind = L.kind;
        (VT ? VT->multi_remove_handle : curl_multi_remove_handle)(L.m, L.e);
        (VT ? VT->easy_cleanup : curl_easy_cleanup)(L.e);
        L.e = NULL;
        curl_slist_free_all(L.hdr);
        L.hdr = NULL;
        wipe(L.k->body, sizeof L.k->body);
        L.k->resp[L.rlen < sizeof L.k->resp ? L.rlen : sizeof L.k->resp - 1] = 0;
        on_resp(kind, status);
        if (L.k) { wipe(L.k->resp, sizeof L.k->resp); L.rlen = 0; }
        L.kind = R_NONE;
        kick();
        return;
    }
}

/* ======================= разбор ответов ======================= */
static void qr_stop_all(void);

static void wipe_secrets(void) {
    if (!L.k) return;
    wipe(L.k->pass, sizeof L.k->pass);
    wipe(L.k->code, sizeof L.k->code);
    wipe(L.k->token, sizeof L.k->token);
    wipe(L.k->ticket, sizeof L.k->ticket);
    wipe(L.k->lii, sizeof L.k->lii);
    wipe(L.k->ra_ticket, sizeof L.k->ra_ticket);
    wipe(L.k->ipt, sizeof L.k->ipt);
    wipe(L.k->pt, sizeof L.k->pt);
    wipe(L.k->bin, sizeof L.k->bin);
}

/* Токен получен (из ответа входа или расшифрованный): в secret.c, в хранилище, конец входа. */
static bool accept_token(const char *t, size_t n) {
    secret_set(t, n);
    if (!secret_token()) { set_err(LF_NONE, ui_tr("Discord вернул токен непонятного вида")); return false; }
    L.notice[0] = 0;
    if (!O.no_save && !L.selftest && !cred_save())
        snprintf(L.notice, sizeof L.notice, ui_tr("Хранилище секретов недоступно: токен не сохранён, при следующем запуске войдите снова."));
    clear_err();
    L.stage = LG_DONE;
    L.want = 0;
    if (L.e) rest_abort();
    wipe_secrets();
    qr_stop_all();
    return true;
}

static void back_with(int field, const char *text) {
    L.stage = L.back;
    set_err(field, "%s", text);
}

/* Первая ошибка поля: errors.<поле>._errors[0].{code,message} (или вложенно на уровень глубже). */
static const cJSON *first_field_error(const cJSON *errs, const char **field) {
    for (const cJSON *f = errs ? errs->child : NULL; f; f = f->next) {
        const cJSON *ea = jget(f, "_errors");
        if (cJSON_IsArray(ea) && ea->child) { *field = f->string; return ea->child; }
        for (const cJSON *g = f->child; g; g = g->next) {
            ea = jget(g, "_errors");
            if (cJSON_IsArray(ea) && ea->child) { *field = f->string; return ea->child; }
        }
    }
    return NULL;
}
static int field_of(const char *f) {
    if (!f) return LF_NONE;
    if (!strcmp(f, "login") || !strcmp(f, "email") || !strcmp(f, "phone")) return LF_LOGIN;
    if (!strcmp(f, "password")) return LF_PASSWORD;
    if (!strcmp(f, "code") || !strcmp(f, "ticket") || !strcmp(f, "token")) return LF_CODE;
    return LF_NONE;
}

static void to_undelete(bool scheduled) {
    L.stage = LG_UNDELETE;
    clear_err();
    snprintf(L.notice, sizeof L.notice, scheduled
        ? ui_tr("Аккаунт помечен к удалению. Войти - значит отменить удаление и восстановить аккаунт.")
        : ui_tr("Аккаунт отключён. Войти - значит снова включить его."));
}
static void cap_field(char *dst, size_t cap, const char *v, const char *extra) {
    dst[0] = 0;
    size_t n = v ? strlen(v) : 0;
    if (n && n < cap && charset_ok(v, n, extra)) memcpy(dst, v, n + 1);
}
static void cap_reset(void) {
    if (L.cap_open) { captcha_close(); L.cap_open = false; }
    L.cap_kind = 0;
    L.cap_key = L.cap_tried = false;
    L.csite[0] = L.csess[0] = L.crqdata[0] = L.crqtoken[0] = 0;
    if (L.k) wipe(L.k->ckey, sizeof L.k->ckey);
}
static bool cap_usable(void) { return L.cap_kind && !strcmp(L.csvc, "hcaptcha") && L.csite[0]; }
/* Капча на запрос kind: запомнить данные проверки; повтор — после login_captcha_open(). */
static void to_captcha(const cJSON *j, int kind) {
    bool again = L.cap_tried && L.cap_kind == kind;  /* уже отправляли ответ — Discord не принял */
    copy_san(L.csvc, sizeof L.csvc, jstr(j, "captcha_service"));
    if (!L.csvc[0]) snprintf(L.csvc, sizeof L.csvc, "hcaptcha");
    cap_field(L.csite, sizeof L.csite, jstr(j, "captcha_sitekey"), "-");
    cap_field(L.csess, sizeof L.csess, jstr(j, "captcha_session_id"), "-_.");
    cap_field(L.crqdata, sizeof L.crqdata, jstr(j, "captcha_rqdata"), "+/=._-");
    cap_field(L.crqtoken, sizeof L.crqtoken, jstr(j, "captcha_rqtoken"), "+/=._-");
    L.cap_kind = kind;
    L.cap_key = L.cap_tried = false;
    if (L.k) wipe(L.k->ckey, sizeof L.k->ckey);
    bool can = cap_usable() && captcha_supported();
    if (kind == R_RA) {                         /* тикет и ключ RSA остаются: после проверки — повтор обмена */
        Q.st = QR_SCANNED;
        snprintf(Q.text, sizeof Q.text, "%s", again ? ui_tr("Discord не принял проверку: пройдите её ещё раз.")
                 : can ? ui_tr("Discord просит пройти проверку, чтобы завершить вход по QR.")
                       : ui_tr("Discord просит пройти проверку (hCaptcha), а показать её здесь нечем."));
        return;
    }
    L.stage = LG_CAPTCHA;
    clear_err();
    if (again) set_err(LF_NONE, ui_tr("Discord не принял проверку. Пройдите её ещё раз."));
    snprintf(L.notice, sizeof L.notice, "%s", can
        ? ui_tr("Discord просит подтвердить, что вы человек (hCaptcha). Проверка откроется в отдельном окне, после неё вход продолжится сам. Или войдите по QR-коду.")
        : ui_tr("Discord просит пройти капчу (hCaptcha), а показать её здесь нечем. Войдите по QR-коду: мобильное приложение Discord -> Настройки -> Сканировать QR-код."));
    if (Q.st == QR_OFF || Q.st == QR_EXPIRED || Q.st == QR_ERROR) login_qr_start();
}

/* Ответ входа / кода 2FA / токена — общий разбор. */
static void handle_auth(const cJSON *j, long status, int kind) {
    const char *tok = jstr(j, "token");
    if (status == 200 && tok && kind != R_PHONEV) { if (!accept_token(tok, strlen(tok))) L.stage = L.back; return; }
    if (status == 200 && jbool(j, "mfa")) {
        const char *t = jstr(j, "ticket"), *lii = jstr(j, "login_instance_id");
        if (!t || strlen(t) >= sizeof L.k->ticket) { back_with(LF_NONE, ui_tr("Discord прислал непонятный ответ на вход")); return; }
        snprintf(L.k->ticket, sizeof L.k->ticket, "%s", t);
        wipe(L.k->lii, sizeof L.k->lii);
        if (lii && strlen(lii) < sizeof L.k->lii && charset_ok(lii, strlen(lii), "-_.")) snprintf(L.k->lii, sizeof L.k->lii, "%s", lii);
        wipe(L.k->pass, sizeof L.k->pass);       /* дальше пароль не нужен */
        L.mfa_methods = (jbool(j, "totp") ? LM_TOTP : 0) | (jbool(j, "sms") ? LM_SMS : 0) | (jbool(j, "backup") ? LM_BACKUP : 0);
        const cJSON *wa = jget(j, "webauthn");
        if (cJSON_IsString(wa) && wa->valuestring[0]) L.mfa_methods |= LM_WEBAUTHN;
        L.mfa_method = L.mfa_methods & LM_TOTP ? LM_TOTP : L.mfa_methods & LM_SMS ? LM_SMS : LM_BACKUP;
        if (!(L.mfa_methods & (LM_TOTP | LM_SMS))) L.mfa_methods |= LM_BACKUP;   /* резервный код есть всегда */
        L.sms_sent = false;
        L.phone[0] = 0;
        L.stage = LG_MFA;
        clear_err();
        snprintf(L.notice, sizeof L.notice, "%s", L.mfa_methods == (LM_WEBAUTHN | LM_BACKUP)
            ? ui_tr("Ключи безопасности (WebAuthn) Astra DS не поддерживает: введите резервный код или войдите по QR-коду.")
            : ui_tr("Введите код из приложения-аутентификатора, из SMS или резервный код."));
        return;
    }
    if (status == 403 && jget(j, "suspended_user_token")) {
        L.stage = LG_FORM;
        set_err(LF_NONE, ui_tr("Аккаунт заблокирован Discord: войти нельзя (подробности - на discord.com)"));
        return;
    }
    i64 code = jint(j, "code", 0);
    const char *field = NULL;
    const cJSON *fe = first_field_error(jget(j, "errors"), &field);
    const char *ecode = fe ? jstr(fe, "code") : NULL, *emsg = fe ? jstr(fe, "message") : jstr(j, "message");
    if (!ecode) ecode = "";
    if (!strcmp(ecode, "ACCOUNT_LOGIN_VERIFICATION_EMAIL")) {
        L.stage = LG_EMAIL;
        clear_err();
        snprintf(L.notice, sizeof L.notice,
            "Discord увидел вход из нового места и отправил письмо на вашу почту. Откройте его, нажмите "
            "\"Verify Login\" (подтвердить вход), затем - \"Повторить вход\". Если ссылка открылась в приложении, "
            "скопируйте её сюда.");
        return;
    }
    if (code == 20013 || code == 20011 || !strcmp(ecode, "ACCOUNT_DISABLED") || !strcmp(ecode, "ACCOUNT_SCHEDULED_FOR_DELETION")) {
        if (L.undelete) { back_with(LF_NONE, ui_tr("Discord не дал восстановить аккаунт")); return; }
        to_undelete(code == 20011 || !strcmp(ecode, "ACCOUNT_SCHEDULED_FOR_DELETION"));
        return;
    }
    if (code == 70007) {
        L.stage = LG_PHONE;
        clear_err();
        snprintf(L.notice, sizeof L.notice, ui_tr("Discord увидел вход из нового места и отправил код в SMS на ваш телефон. Введите его."));
        return;
    }
    if (kind == R_MFA && (code == 60008 || status == 401 || field_of(field) == LF_CODE)) {
        wipe(L.k->code, sizeof L.k->code);
        L.stage = LG_MFA;
        if (code == 60008) { set_err(LF_CODE, "%s", ui_tr("Неверный код. Попробуйте ещё раз.")); return; }
        char m[160];                            /* иное: показать ответ Discord как есть (без секретов) */
        copy_san(m, sizeof m, emsg ? emsg : "");
        set_err(LF_CODE, ui_tr("Discord не принял код: %s (код %lld, HTTP %ld)"), m[0] ? m : "-", (long long)code, status);
        return;
    }
    if (!strcmp(ecode, "INVALID_LOGIN") || !strcmp(ecode, "PASSWORD_DOES_NOT_MATCH") || (code == 50035 && field_of(field) == LF_PASSWORD)) {
        wipe(L.k->pass, sizeof L.k->pass);
        L.stage = LG_FORM;
        bool pw = !strcmp(ecode, "PASSWORD_DOES_NOT_MATCH");
        set_err(pw ? LF_PASSWORD : field_of(field) == LF_NONE ? LF_LOGIN : field_of(field),
                "%s", pw ? ui_tr("Неверный пароль.") : ui_tr("Неверный email/телефон или пароль."));
        L.err_both = !pw && !strcmp(ecode, "INVALID_LOGIN");
        return;
    }
    if (!strcmp(ecode, "BASE_TYPE_REQUIRED")) { back_with(field_of(field), ui_tr("Это поле нужно заполнить.")); return; }
    /* Прочее — текст Discord (очищенный) у поля или общий. */
    char m[160];
    copy_san(m, sizeof m, emsg ? emsg : "");
    if (status == 401 && kind == R_TOKEN) snprintf(m, sizeof m, ui_tr("токен не принят"));
    if (!m[0]) snprintf(m, sizeof m, "HTTP %ld", status);
    char full[200];
    snprintf(full, sizeof full, "Discord: %s", m);
    back_with(field_of(field), full);
}

static void on_resp(int kind, long status) {
    L.route_wait = false;
    if (status <= 0) {                          /* сеть или не собрали запрос */
        if (kind == R_FP) return;               /* без отпечатка — тоже можно */
        if (kind == R_RA) { Q.st = QR_ERROR; snprintf(Q.text, sizeof Q.text, ui_tr("нет связи с Discord: войти по QR не удалось")); wipe(L.k->ra_ticket, sizeof L.k->ra_ticket); return; }
        if (kind == R_TOKEN) secret_set("", 0);
        back_with(LF_NONE, status < 0 ? ui_tr("Слишком длинные данные") : ui_tr("Нет связи с Discord. Проверьте сеть и повторите."));
        return;
    }
    const char *body = L.k->resp;
    if (kind == R_FP) {                         /* {"fingerprint":"<id>.<base64>", ...} — только начало ответа */
        const char *p = strstr(body, "\"fingerprint\":\"");
        if (status == 200 && p) {
            p += 15;
            size_t n = strcspn(p, "\"");
            if (n && n < sizeof L.fp && p[n] == '"' && charset_ok(p, n, "._-")) { memcpy(L.fp, p, n); L.fp[n] = 0; }
        }
        return;
    }
    cJSON *j = json_parse(body, strlen(body));
    if (status == 429) {
        const cJSON *ra = jget(j, "retry_after");
        double after = cJSON_IsNumber(ra) ? ra->valuedouble : 5;
        if (after < 1) after = 1;
        if (after > 3600) after = 3600;
        jdone(j);
        L.wait_until = now() + (i64)(after * 1000);
        if (kind == R_RA) { Q.st = QR_ERROR; snprintf(Q.text, sizeof Q.text, ui_tr("лимит запросов Discord: обновите QR через %.0f с"), after); return; }
        if (kind == R_TOKEN) secret_set("", 0);
        L.stage = L.back == LG_BUSY ? LG_FORM : L.back;
        set_err(LF_NONE, ui_tr("Слишком много попыток. Подождите %.0f с."), after);
        return;
    }
    if (status == 400 && (jget(j, "captcha_key") || jget(j, "captcha_sitekey"))) {
        jdone(j);
        cJSON *j2 = json_parse(body, strlen(body));
        to_captcha(j2, kind);
        jdone(j2);
        return;
    }
    if (kind == L.cap_kind) cap_reset();        /* ответ не капча: проверка своё сделала (или не нужна) */
    switch (kind) {
    case R_SMS: {
        if (status == 200) {
            copy_san(L.phone, sizeof L.phone, jstr(j, "phone"));
            L.sms_sent = true;
            L.stage = LG_MFA;
            clear_err();
            snprintf(L.notice, sizeof L.notice, ui_tr("Код отправлен в SMS%s%s."), L.phone[0] ? ui_tr(" на ") : "", L.phone);
        } else {
            handle_auth(j, status, kind);
            if (L.stage != LG_CAPTCHA) L.stage = LG_MFA;
        }
        break;
    }
    case R_RA: {
        const char *et = jstr(j, "encrypted_token");
        int n = status == 200 && et ? rsa_dec_b64(Q.key, et) : -1;
        wipe(L.k->ra_ticket, sizeof L.k->ra_ticket);
        if (n > 0 && accept_token((const char *)L.k->pt, (size_t)n)) { wipe(L.k->pt, sizeof L.k->pt); break; }
        if (L.k) wipe(L.k->pt, sizeof L.k->pt);
        char m[128];
        copy_san(m, sizeof m, jstr(j, "message"));
        Q.st = QR_ERROR;
        snprintf(Q.text, sizeof Q.text, ui_tr("Вход по QR не удался%s%s. Обновите QR-код."), m[0] ? ": " : "", m);
        if (Q.key) { EVP_PKEY_free(Q.key); Q.key = NULL; }
        break;
    }
    case R_AUTHIP:
        wipe(L.k->ipt, sizeof L.k->ipt);
        if (status == 204 || status == 200) {   /* место входа подтверждено: снова вход с тем же паролем */
            if (L.k->pass[0]) { L.want |= R_LOGIN; L.stage = LG_BUSY; snprintf(L.busy, sizeof L.busy, ui_tr("Вход...")); }
            else { L.stage = LG_FORM; set_err(LF_PASSWORD, ui_tr("Вход подтверждён. Введите пароль ещё раз.")); }
            clear_err();
        } else {
            wipe(L.k->code, sizeof L.k->code);
            back_with(LF_CODE, ui_tr("Ссылка не подошла (устарела или не та). Откройте её в браузере или повторите вход."));
        }
        break;
    case R_PHONEV: {
        const char *t = jstr(j, "token");
        wipe(L.k->code, sizeof L.k->code);
        if (status == 200 && t && strlen(t) < sizeof L.k->ipt) {
            snprintf(L.k->ipt, sizeof L.k->ipt, "%s", t);
            L.want |= R_AUTHIP;
            L.stage = LG_BUSY;
        } else back_with(LF_CODE, ui_tr("Код из SMS не подошёл."));
        break;
    }
    case R_TOKEN:
        if (status == 200) {
            if (secret_token()) {                            /* токен уже в secret.c: сохранить и закончить */
                L.notice[0] = 0;
                if (!O.no_save && !L.selftest && !cred_save())
                    snprintf(L.notice, sizeof L.notice, ui_tr("Хранилище секретов недоступно: токен не сохранён."));
                L.stage = LG_DONE;
                L.want = 0;
                clear_err();
                wipe_secrets();
                qr_stop_all();
            }
        } else {
            secret_set("", 0);
            L.stage = LG_TOKEN;
            set_err(LF_TOKEN, "%s", status == 401 ? ui_tr("Discord не принял токен.") : ui_tr("Токен не удалось проверить."));
        }
        break;
    default:                                    /* R_LOGIN, R_MFA */
        handle_auth(j, status, kind);
        break;
    }
    jdone(j);
}

/* ======================= QR: шлюз remote auth ======================= */
static void qr_close_ws(void) {
    if (Q.sw) { sockwait_free(Q.sw); Q.sw = NULL; }
    if (Q.ws) {
        if (Q.wm) curl_multi_remove_handle(Q.wm, Q.ws);
        curl_easy_cleanup(Q.ws);
        Q.ws = NULL;
    }
    Q.live = Q.drain = false;
    Q.next_hb = 0;
    Q.flen = 0;
    if (L.k) wipe(L.k->ws, sizeof L.k->ws);
}
static void qr_drop_key(void) { if (Q.key) { EVP_PKEY_free(Q.key); Q.key = NULL; } }
static void qr_stop_all(void) {
    qr_close_ws();
    if (!(L.want & R_RA) && L.kind != R_RA) qr_drop_key();
    if (Q.st != QR_OFF && L.stage == LG_DONE) Q.st = QR_OFF;
}

static void qr_fail(QrState st, const char *text) {
    qr_close_ws();
    qr_drop_key();
    Q.st = st;
    Q.url[0] = 0;
    snprintf(Q.text, sizeof Q.text, "%s", text);
    kick();
}

static void qr_renew(void) {                    /* новый ключ и новый QR */
    if (L.cap_kind == R_RA) cap_reset();
    if (L.k) wipe(L.k->ra_ticket, sizeof L.k->ra_ticket);
    L.want &= ~(unsigned)R_RA;
    qr_close_ws();
    qr_drop_key();
    Q.st = QR_CONNECTING;
    Q.need = true;
    Q.url[0] = 0;
    Q.uid = 0;
    Q.uname[0] = Q.avatar[0] = Q.discrim[0] = 0;
    Q.got_login = false;
    snprintf(Q.text, sizeof Q.text, ui_tr("Подготовка QR-кода..."));
    kick();
}

static bool ws_send(const char *s) {
    size_t n = strlen(s), off = 0;
    int spins = 0;
    while (off < n && Q.ws) {
        size_t sent = 0;
        CURLcode rc = curl_ws_send(Q.ws, s + off, n - off, &sent, 0, CURLWS_TEXT);
        if (rc == CURLE_AGAIN) { if (++spins > 200) return false; plat_sleep_ms(5); continue; }
        if (rc != CURLE_OK) return false;
        off += sent;
    }
    return off == n;
}

static void qr_connect(void) {
    Q.need = false;
    if (!route_ok()) { Q.need = true; return; }
    if (!Q.key) {
        Q.key = rsa_new();
        if (!Q.key) { qr_fail(QR_ERROR, ui_tr("не удалось создать ключ RSA")); return; }
    }
    if (!Q.wm) Q.wm = curl_multi_init();
    Q.ws = Q.wm ? curl_easy_init() : NULL;
    if (!Q.ws) { qr_fail(QR_ERROR, ui_tr("нет памяти")); return; }
    net_curl_common(Q.ws);
    if (!net_route(Q.ws, &Q.via)) { curl_easy_cleanup(Q.ws); Q.ws = NULL; Q.need = true; return; }
    static struct curl_slist o = { "Origin: https://discord.com", NULL };
    curl_easy_setopt(Q.ws, CURLOPT_URL, RA_URL);
    curl_easy_setopt(Q.ws, CURLOPT_HTTPHEADER, &o);
    curl_easy_setopt(Q.ws, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(Q.ws, CURLOPT_TIMEOUT, 20L);
    if (curl_multi_add_handle(Q.wm, Q.ws) != CURLM_OK) { qr_fail(QR_ERROR, ui_tr("нет памяти")); return; }
    snprintf(Q.text, sizeof Q.text, ui_tr("Подключение к Discord..."));
}

static void qr_connecting(void) {
    int running = 0;
    curl_multi_perform(Q.wm, &running);
    CURLMsg *m;
    int left;
    CURLcode rc = CURLE_AGAIN;
    while ((m = curl_multi_info_read(Q.wm, &left)))
        if (m->msg == CURLMSG_DONE && m->easy_handle == Q.ws) rc = m->data.result;
    if (rc == CURLE_AGAIN) return;
    if (rc != CURLE_OK) {
        if (Q.via && net_path_error((int)rc)) vless_report_failure("QR");
        qr_fail(QR_ERROR, ui_tr("Нет связи со шлюзом QR Discord."));
        return;
    }
    if (Q.via) vless_report_failure(NULL);
    curl_easy_setopt(Q.ws, CURLOPT_TIMEOUT, 0L);
    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(Q.ws, CURLINFO_ACTIVESOCKET, &sock);
    if (sock == CURL_SOCKET_BAD) { qr_fail(QR_ERROR, ui_tr("нет сокета")); return; }
    Q.sw = sockwait_new((intptr_t)sock);
    Q.live = true;
    Q.drain = true;                             /* hello мог прийти вместе с ответом на рукопожатие */
    Q.hb_iv = 0;
    Q.next_hb = now() + 30000;                  /* hello — не позже 30 с */
    Q.flen = 0;
}

static void qr_heartbeat(void) {
    Q.acked = false;
    if (!ws_send("{\"op\":\"heartbeat\"}")) qr_fail(QR_ERROR, ui_tr("Связь со шлюзом QR потеряна."));
}

/* Одно сообщение шлюза (текст JSON в L.k->ws). */
static void qr_message(const char *text, size_t len) {
    cJSON *j = json_parse(text, len);
    const char *op = jstr(j, "op");
    if (!op) { jdone(j); return; }
    if (!strcmp(op, "hello")) {
        Q.hb_iv = jint(j, "heartbeat_interval", 41250);
        if (Q.hb_iv < 1000) Q.hb_iv = 1000;
        i64 to = jint(j, "timeout_ms", 120000);
        Q.expires = now() + (to > 0 && to < 3600000 ? to : 120000);
        jdone(j);
        unsigned char r = 0;
        RAND_bytes(&r, 1);
        Q.next_hb = now() + Q.hb_iv * r / 256;
        Q.acked = true;
        char pub[600], fpr[48];
        Buf b = {0};
        if (!key_spki(Q.key, pub, sizeof pub, fpr, sizeof fpr)) { qr_fail(QR_ERROR, ui_tr("ключ RSA не выгрузился")); return; }
        memcpy(Q.fpr, fpr, sizeof fpr);
        buf_printf(&b, "{\"op\":\"init\",\"encoded_public_key\":\"%s\"}", pub);
        bool ok = ws_send(b.p);
        buf_free(&b);
        if (!ok) qr_fail(QR_ERROR, ui_tr("Связь со шлюзом QR потеряна (init)."));
        return;
    }
    if (!strcmp(op, "heartbeat_ack")) { Q.acked = true; jdone(j); return; }
    if (!strcmp(op, "nonce_proof")) {
        int n = rsa_dec_b64(Q.key, jstr(j, "encrypted_nonce"));
        jdone(j);
        if (n <= 0) { qr_fail(QR_ERROR, ui_tr("Шлюз QR прислал непонятные данные.")); return; }
        char msg[800], nb[700];                 /* nonce до 256 байт (ключ 2048 бит) */
        size_t m = b64_enc(L.k->pt, (size_t)n, nb, sizeof nb, true);
        wipe(L.k->pt, sizeof L.k->pt);
        if (!m) { qr_fail(QR_ERROR, ui_tr("Шлюз QR прислал слишком длинный nonce.")); return; }
        snprintf(msg, sizeof msg, "{\"op\":\"nonce_proof\",\"nonce\":\"%s\"}", nb);
        bool ok = ws_send(msg);
        wipe(nb, sizeof nb);
        wipe(msg, sizeof msg);
        if (!ok) qr_fail(QR_ERROR, ui_tr("Связь со шлюзом QR потеряна (nonce)."));
        return;
    }
    if (!strcmp(op, "pending_remote_init")) {
        const char *f = jstr(j, "fingerprint");
        bool same = f && !strcmp(f, Q.fpr);
        jdone(j);
        if (!same) { qr_fail(QR_ERROR, ui_tr("Отпечаток ключа не совпал: QR-код не показан.")); return; }
        snprintf(Q.url, sizeof Q.url, RA_PREFIX "%s", Q.fpr);
        Q.st = QR_SHOW;
        snprintf(Q.text, sizeof Q.text, ui_tr("Отсканируйте QR-код в мобильном приложении Discord, чтобы войти сразу."));
        kick();
        return;
    }
    if (!strcmp(op, "pending_ticket")) {
        int n = rsa_dec_b64(Q.key, jstr(j, "encrypted_user_payload"));
        jdone(j);
        if (n <= 0 || !parse_user((const char *)L.k->pt, &Q.uid, Q.discrim, sizeof Q.discrim, Q.avatar, sizeof Q.avatar, Q.uname, sizeof Q.uname)) {
            wipe(L.k->pt, sizeof L.k->pt);
            qr_fail(QR_ERROR, ui_tr("Шлюз QR прислал непонятные данные."));
            return;
        }
        wipe(L.k->pt, sizeof L.k->pt);
        Q.st = QR_SCANNED;
        snprintf(Q.text, sizeof Q.text, ui_tr("Подтвердите вход на телефоне."));
        kick();
        return;
    }
    if (!strcmp(op, "pending_login")) {
        const char *t = jstr(j, "ticket");
        bool ok = t && strlen(t) < sizeof L.k->ra_ticket && charset_ok(t, strlen(t), "._-+/=");
        if (ok) snprintf(L.k->ra_ticket, sizeof L.k->ra_ticket, "%s", t);
        jdone(j);
        if (!ok) { qr_fail(QR_ERROR, ui_tr("Шлюз QR прислал непонятный тикет.")); return; }
        Q.got_login = true;
        L.want |= R_RA;
        snprintf(Q.text, sizeof Q.text, ui_tr("Вход..."));
        qr_close_ws();                          /* шлюз больше не нужен, ключ ещё нужен для токена */
        kick();
        return;
    }
    if (!strcmp(op, "cancel")) { jdone(j); Q.renew = 0; qr_renew(); return; }
    jdone(j);
}

static void qr_closed(int code) {
    if (Q.got_login) { qr_close_ws(); return; }
    if (code == 4003 || code == 1000) {          /* сессия истекла: новый QR (сам — до 3 раз подряд) */
        if (++Q.renew <= QR_AUTO_RENEW) { qr_renew(); return; }
        qr_fail(QR_EXPIRED, ui_tr("QR-код устарел. Нажмите, чтобы получить новый."));
        return;
    }
    char t[160];
    snprintf(t, sizeof t, ui_tr("Шлюз QR закрыл соединение (%d). Обновите QR-код."), code);
    qr_fail(QR_ERROR, t);
}

static void qr_live(void) {
    sockwait_clear(Q.sw);
    Q.drain = false;
    Locked *k = L.k;
    for (int guard = 0; guard < 256 && Q.live; guard++) {
        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        char tmp[2048];
        CURLcode rc = curl_ws_recv(Q.ws, tmp, sizeof tmp, &got, &meta);
        if (rc == CURLE_AGAIN) { wipe(tmp, sizeof tmp); break; }
        if (rc != CURLE_OK || !meta) {
            wipe(tmp, sizeof tmp);
            if (Q.got_login) { qr_close_ws(); return; }
            char t[128];
            snprintf(t, sizeof t, ui_tr("Связь со шлюзом QR потеряна (%d)."), (int)rc);
            qr_fail(QR_ERROR, t);
            return;
        }
        if (meta->flags & CURLWS_CLOSE) {
            int code = got >= 2 ? ((unsigned char)tmp[0] << 8 | (unsigned char)tmp[1]) : 1000;
            wipe(tmp, sizeof tmp);
            qr_closed(code);
            return;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;
        if (Q.flen + got >= sizeof k->ws) { wipe(tmp, sizeof tmp); qr_fail(QR_ERROR, ui_tr("Шлюз QR прислал слишком большой кадр.")); return; }
        memcpy(k->ws + Q.flen, tmp, got);
        wipe(tmp, sizeof tmp);
        Q.flen += got;
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
            size_t n = Q.flen;
            k->ws[n] = 0;
            Q.flen = 0;
            qr_message(k->ws, n);
            if (L.k) wipe(L.k->ws, n + 1);
        }
    }
    if (!Q.live) return;
    i64 t = now();
    if (Q.next_hb && t >= Q.next_hb) {
        if (!Q.hb_iv) { qr_fail(QR_ERROR, ui_tr("Шлюз QR не ответил.")); return; }
        if (!Q.acked) { qr_fail(QR_ERROR, ui_tr("Шлюз QR не отвечает.")); return; }
        qr_heartbeat();
        Q.next_hb = t + Q.hb_iv;
    }
    if (Q.live && Q.expires && t > Q.expires + 10000) qr_closed(4003);   /* сервер не закрыл сам */
}

/* ======================= API ======================= */
bool login_init(void) {
    if (L.on) return true;
    memset(&L, 0, sizeof L);
    memset(&Q, 0, sizeof Q);
    L.ksz = (sizeof(Locked) + 4095) & ~(size_t)4095;
    L.k = plat_pages(L.ksz);
    if (!L.k) return false;
    plat_lock(L.k, L.ksz);
    memset(L.k, 0, L.ksz);
    /* Закрытый ключ RSA и временные числа расшифровки — в закреплённой защищённой куче OpenSSL. */
    if (!CRYPTO_secure_malloc_initialized()) L.sheap = CRYPTO_secure_malloc_init(65536, 32) == 1;
    L.on = true;
    L.stage = L.back = LG_FORM;
    L.err_field = LF_NONE;
    return true;
}

bool login_active(void) { return L.on; }

const LoginView *login_view(void) {
    LoginView *v = &L.v;
    memset(v, 0, sizeof *v);
    v->stage = L.stage;
    v->busy_text = L.busy;
    v->error = L.err;
    v->error_field = L.err_field;
    v->notice = L.notice;
    v->wait_until = L.wait_until > now() ? L.wait_until : 0;
    v->route_wait = L.route_wait || ((L.want || Q.need) && !route_ok());
    v->mfa_methods = L.mfa_methods;
    v->mfa_method = L.mfa_method;
    v->sms_sent = L.sms_sent;
    v->phone = L.phone;
    v->captcha_service = L.csvc;
    v->captcha = L.cap_kind != 0;
    v->captcha_qr = L.cap_kind == R_RA;
    v->captcha_open = L.cap_open;
    v->captcha_supported = cap_usable() && captcha_supported();
    v->err_login = L.err_field == LF_LOGIN ? L.err : "";
    v->err_pass = L.err_field == LF_PASSWORD || (L.err_both && L.err_field == LF_LOGIN) ? L.err : "";
    v->err_code = L.err_field == LF_CODE ? L.err : "";
    v->err_token = L.err_field == LF_TOKEN ? L.err : "";
    v->qr = Q.st;
    v->qr_url = Q.st == QR_SHOW ? Q.url : "";
    v->qr_text = Q.text;
    v->qr_expires = Q.st == QR_SHOW ? Q.expires : 0;
    v->qr_user_id = Q.uid;
    v->qr_user_name = Q.uname;
    v->qr_discrim = Q.discrim;
    v->qr_avatar = Q.avatar;
    return v;
}

char *login_field(LoginField f, size_t *cap) {
    if (!L.k) { if (cap) *cap = 0; return NULL; }
    switch (f) {
    case LF_LOGIN: if (cap) *cap = sizeof L.k->login; return L.k->login;
    case LF_PASSWORD: if (cap) *cap = sizeof L.k->pass; return L.k->pass;
    case LF_CODE: if (cap) *cap = sizeof L.k->code; return L.k->code;
    case LF_TOKEN: if (cap) *cap = sizeof L.k->token; return L.k->token;
    default: if (cap) *cap = 0; return NULL;
    }
}
void login_field_clear(LoginField f) {
    size_t cap;
    char *p = login_field(f, &cap);
    if (p) wipe(p, cap);
}

static void go_busy(int kind, const char *text) {
    clear_err();
    L.back = L.stage == LG_BUSY ? L.back : L.stage;
    L.stage = LG_BUSY;
    snprintf(L.busy, sizeof L.busy, "%s", text);
    L.want |= (unsigned)kind;
    kick();
}

/* Код: без пробелов (и без дефисов — у кодов из цифр и резервных xxxx-xxxx). */
static void norm_code(char *s) {
    size_t w = 0;
    for (size_t r = 0; s[r]; r++) if (s[r] != ' ' && s[r] != '-' && s[r] != '\t') s[w++] = s[r];
    wipe(s + w, strlen(s + w));
    s[w] = 0;
}

static bool can_send(void) {
    if (L.wait_until > now()) {
        set_err(LF_NONE, ui_tr("Слишком много попыток. Подождите %lld с."), (long long)((L.wait_until - now() + 999) / 1000));
        return false;
    }
    return L.stage != LG_BUSY && L.stage != LG_DONE && L.on;
}

static void start_login(void) {
    if (!L.k->login[0]) { L.stage = LG_FORM; set_err(LF_LOGIN, ui_tr("Введите email или номер телефона.")); return; }
    if (!L.k->pass[0]) { L.stage = LG_FORM; set_err(LF_PASSWORD, ui_tr("Введите пароль.")); return; }
    L.back = LG_FORM;
    L.stage = LG_FORM;
    go_busy(R_LOGIN, ui_tr("Вход..."));
}

void login_submit(void) {
    if (!can_send()) { kick(); return; }
    Locked *k = L.k;
    switch (L.stage) {
    case LG_FORM:
        L.undelete = false;
        start_login();
        break;
    case LG_MFA:
        norm_code(k->code);
        if (!k->code[0]) { set_err(LF_CODE, ui_tr("Введите код.")); break; }
        go_busy(R_MFA, ui_tr("Проверка кода..."));
        break;
    case LG_EMAIL:
        if (k->code[0]) {
            bool ok = ip_token_from(k->code, k->ipt, sizeof k->ipt);
            wipe(k->code, sizeof k->code);
            if (!ok) { set_err(LF_CODE, ui_tr("Это не похоже на ссылку из письма Discord.")); break; }
            go_busy(R_AUTHIP, ui_tr("Подтверждение входа..."));
        } else login_retry();
        break;
    case LG_PHONE:
        norm_code(k->code);
        if (!k->code[0]) { set_err(LF_CODE, ui_tr("Введите код из SMS.")); break; }
        go_busy(R_PHONEV, ui_tr("Проверка кода..."));
        break;
    case LG_TOKEN: {
        size_t n = strlen(k->token), a = 0;
        while (a < n && (k->token[a] == ' ' || k->token[a] == '"')) a++;
        while (n > a && (k->token[n - 1] == ' ' || k->token[n - 1] == '"' || k->token[n - 1] == '\r' || k->token[n - 1] == '\n')) n--;
        secret_set(k->token + a, n - a);
        wipe(k->token, sizeof k->token);
        if (!secret_token()) { set_err(LF_TOKEN, ui_tr("Токен не похож на настоящий.")); break; }
        go_busy(R_TOKEN, ui_tr("Проверка токена..."));
        break;
    }
    case LG_CAPTCHA: login_retry(); break;
    case LG_UNDELETE: login_undelete(); break;
    default: break;
    }
    kick();
}

static void put_field(LoginField f, const char *s) {
    size_t cap;
    char *p = login_field(f, &cap);
    if (!p) return;
    wipe(p, cap);
    size_t n = s ? strlen(s) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(p, s, n);
}
void login_start_password(const char *login, const char *pass) {
    if (!L.on || L.stage == LG_BUSY || L.stage == LG_DONE) return;
    put_field(LF_LOGIN, login);
    put_field(LF_PASSWORD, pass);
    L.stage = LG_FORM;
    login_submit();
}
void login_submit_code(const char *code, int kind) {
    if (!L.on) return;
    if (L.stage == LG_MFA) {
        unsigned m = kind == 1 ? LM_SMS : kind == 2 ? LM_BACKUP : LM_TOTP;
        if (L.mfa_methods & m) L.mfa_method = m;
    }
    put_field(L.stage == LG_TOKEN ? LF_TOKEN : LF_CODE, code);
    login_submit();
}
void login_email_retry(void) { login_retry(); }

void login_mfa_method(unsigned m) {
    if (L.stage != LG_MFA || !(L.mfa_methods & m) || (m & (m - 1))) return;
    L.mfa_method = m;
    clear_err();
    if (m == LM_SMS && !L.sms_sent) login_sms_send();
    kick();
}
void login_sms_send(void) {
    if (L.stage != LG_MFA || !(L.mfa_methods & LM_SMS) || !can_send()) { kick(); return; }
    L.mfa_method = LM_SMS;
    go_busy(R_SMS, ui_tr("Отправка SMS..."));
}
void login_retry(void) {
    if (!L.on || !can_send()) { kick(); return; }
    L.stage = LG_FORM;
    start_login();
    kick();
}
void login_undelete(void) {
    if (L.stage != LG_UNDELETE || !can_send()) { kick(); return; }
    L.undelete = true;
    L.stage = LG_FORM;
    start_login();
    kick();
}
void login_use_token(void) {
    if (!L.on || L.stage == LG_BUSY || L.stage == LG_DONE) return;
    L.stage = LG_TOKEN;
    clear_err();
    snprintf(L.notice, sizeof L.notice, ui_tr("Токен уходит только в Discord и хранится в системном хранилище секретов."));
    kick();
}
void login_captcha_open(void) {
    if (!L.on || !L.cap_kind || L.cap_open || L.stage == LG_DONE) return;
    char err[200];
    if (!cap_usable()) snprintf(err, sizeof err, ui_tr("эту проверку (%s) Astra DS показать не может"), L.csvc);
    else if (captcha_open(L.csite, L.crqdata, L.k->ckey, sizeof L.k->ckey, false, err, sizeof err)) { L.cap_open = true; clear_err(); kick(); return; }
    if (L.cap_kind == R_RA) snprintf(Q.text, sizeof Q.text, ui_tr("Проверка не открылась: %s"), err);
    else set_err(LF_NONE, ui_tr("Проверка не открылась: %s"), err);
    kick();
}
void login_captcha_cancel(void) {
    if (!L.cap_open) return;
    captcha_close();
    L.cap_open = false;
    if (L.k) wipe(L.k->ckey, sizeof L.k->ckey);
    kick();
}
/* Окно проверки закрылось: ответ есть — повторить тот же запрос с ним. */
static void captcha_step(void) {
    char err[200];
    int r = captcha_result(err, sizeof err);
    if (!r) return;
    captcha_close();
    L.cap_open = false;
    if (r == 1 && L.k->ckey[0]) {
        L.cap_key = true;
        L.want |= (unsigned)L.cap_kind;
        if (L.cap_kind == R_RA) snprintf(Q.text, sizeof Q.text, ui_tr("Вход..."));
        else { L.stage = LG_BUSY; snprintf(L.busy, sizeof L.busy, ui_tr("Вход...")); clear_err(); }
    } else {
        wipe(L.k->ckey, sizeof L.k->ckey);
        if (L.cap_kind == R_RA) snprintf(Q.text, sizeof Q.text, ui_tr("Проверка не пройдена: %s"), err[0] ? err : ui_tr("окно закрыто"));
        else set_err(LF_NONE, ui_tr("Проверка не пройдена: %s"), err[0] ? err : ui_tr("окно закрыто"));
    }
    kick();
}

void login_back(void) {
    if (!L.on || L.stage == LG_DONE) return;
    if (L.e && L.kind != R_RA) rest_abort();
    L.want &= R_RA;
    if (L.k) {
        wipe(L.k->pass, sizeof L.k->pass);
        wipe(L.k->code, sizeof L.k->code);
        wipe(L.k->token, sizeof L.k->token);
        wipe(L.k->ticket, sizeof L.k->ticket);
        wipe(L.k->lii, sizeof L.k->lii);
        wipe(L.k->ipt, sizeof L.k->ipt);
    }
    if (L.cap_kind != R_RA) cap_reset();
    L.stage = L.back = LG_FORM;
    L.mfa_methods = L.mfa_method = 0;
    L.sms_sent = L.undelete = false;
    L.phone[0] = L.notice[0] = L.csvc[0] = 0;
    clear_err();
    kick();
}

void login_qr_start(void) {
    if (!L.on || L.stage == LG_DONE) return;
    Q.renew = 0;
    qr_renew();
}
void login_qr_stop(void) {
    qr_close_ws();
    qr_drop_key();
    if (L.k) wipe(L.k->ra_ticket, sizeof L.k->ra_ticket);
    if (L.cap_kind == R_RA) cap_reset();
    L.want &= ~(unsigned)R_RA;
    Q.st = QR_OFF;
    Q.need = false;
    Q.url[0] = 0;
    kick();
}

void login_cancel(void) {
    if (!L.on) return;
    cap_reset();
    rest_abort();
    login_qr_stop();
    login_back();
    if (L.k) wipe(L.k->login, sizeof L.k->login);
    L.want = 0;
    L.wait_until = 0;
}

void login_finish(void) {
    if (!L.on) return;
    cap_reset();
    rest_abort();
    qr_close_ws();
    qr_drop_key();
    if (L.m) { (VT ? VT->multi_cleanup : curl_multi_cleanup)(L.m); L.m = NULL; }
    if (Q.wm) { curl_multi_cleanup(Q.wm); Q.wm = NULL; }
    VT = NULL;
    impcurl_shutdown();                          /* libcurl-impersonate нужна только для входа: выгрузить (повторный вход загрузит снова) */
    /* SECURITY К2: поток окна капчи не завершился (captcha_close не дождался за 20 с) — он ещё может писать
     * в L.k->ckey: страницы затираем, но не освобождаем (утечка закреплённых страниц лучше записи в чужую память). */
    if (L.k && captcha_busy()) { plat_wipe(L.k, L.ksz); L.k = NULL; }
    if (L.k) { plat_wipe(L.k, L.ksz); plat_unlock(L.k, L.ksz); plat_pages_free(L.k, L.ksz); L.k = NULL; }
    if (L.sheap) CRYPTO_secure_malloc_done();   /* 0 — что-то ещё занято: куча остаётся (закреплённой) */
    bool sh = L.sheap;
    memset(&Q, 0, sizeof Q);
    memset(&L, 0, sizeof L);
    L.sheap = sh && CRYPTO_secure_malloc_initialized();
    L.err_field = LF_NONE;
}

void login_service(void) {
    if (!L.on) return;
    if (L.cap_open) captcha_step();
    /* REST: один запрос за раз; QR-тикет — первым (он живёт недолго) */
    if (L.e) rest_step();
    if (L.on && !L.e && L.want && L.stage != LG_DONE) {
        if (!route_ok()) L.route_wait = true;
        else {
            L.route_wait = false;
            int kind = 0;
            /* отпечаток посетителя и куки — до первого запроса входа, в том числе до обмена QR: без них капча,
             * которую Discord просит на обмене, привязывалась к «никому» и её ответ отвергался (invalid-response) */
            if ((L.want & (R_LOGIN | R_RA)) && !L.fp_tried) { L.fp_tried = true; kind = R_FP; L.want |= R_FP; }
            else if (L.want & R_RA) kind = R_RA;
            else for (int b = 2; b <= R_TOKEN; b <<= 1) if (L.want & (unsigned)b) { kind = b; break; }
            if (kind) {
                L.want &= ~(unsigned)kind;
                rest_start(kind);
                kick();
            }
        }
    }
    /* QR */
    if (Q.need && L.stage != LG_DONE) qr_connect();
    if (Q.ws && !Q.live) qr_connecting();
    if (Q.live) qr_live();
}

/* Паника или сбой: без curl/OpenSSL (могут быть в неопределённом состоянии) — только затереть страницы. */
void login_panic(void) {
    if (L.k) plat_wipe(L.k, L.ksz);
    plat_wipe(L.err, sizeof L.err);
}

int login_waitables(Waitable *out, int max) {
    if (!L.on || !Q.live || !Q.sw || max < 1) return 0;
    out[0] = sockwait_get(Q.sw);
    return 1;
}

i64 login_next_wake(void) {
    if (!L.on) return INT64_MAX;
    i64 t = INT64_MAX, n = now();
    if (L.e) t = n + 10;                        /* опрос только пока идёт запрос, как у REST */
    else if (L.want && route_ok()) t = n;       /* иначе — разбудит vless при смене состояния */
    if (Q.need && route_ok()) t = n;
    if (Q.ws && !Q.live && n + 20 < t) t = n + 20;
    if (Q.live) {
        if (Q.drain) t = n;
        if (Q.next_hb && Q.next_hb < t) t = Q.next_hb;
        if (Q.expires && Q.expires + 10000 < t) t = Q.expires + 10000;
    }
    if (L.wait_until > n && L.wait_until < t) t = L.wait_until;   /* снять блокировку кнопки */
    return t;
}

/* ======================= самопроверка без сети ======================= */
static void feed(int kind, long status, const char *json) {
    snprintf(L.k->resp, sizeof L.k->resp, "%s", json);
    L.rlen = strlen(L.k->resp);
    L.stage = LG_BUSY;
    on_resp(kind, status);
    wipe(L.k->resp, sizeof L.k->resp);
}
static bool zero(const void *p, size_t n) { const unsigned char *c = p; for (size_t i = 0; i < n; i++) if (c[i]) return false; return true; }
static void put(LoginField f, const char *s) { put_field(f, s); }

int login_selftest(void (*check)(bool ok, const char *what)) {
    bool own_secret = !secret_vless_area(&(size_t){0}) && secret_init();   /* защищённая память secret.c ещё не выделена */
    bool save_ns = O.no_save;
    O.no_save = true;                           /* в хранилище ничего не пишем */
    check(login_init(), "вход: закреплённая память выделена");
    L.selftest = true;
    check(CRYPTO_secure_malloc_initialized(), "вход: защищённая куча OpenSSL поднята (закрытый ключ RSA не в обычной куче)");

    /* JSON тела: экранирование, пароль только в /auth/login */
    put(LF_LOGIN, "a@b.c");
    put(LF_PASSWORD, "p\"a\\s\x01s");
    char path[96];
    const char *meth;
    bool ok = build(R_LOGIN, path, sizeof path, &meth) && !strcmp(path, "/auth/login") &&
              !strcmp(L.k->body, "{\"login\":\"a@b.c\",\"password\":\"p\\\"a\\\\s\\u0001s\",\"undelete\":false,\"login_source\":null,\"gift_code_sku_id\":null}");
    check(ok, "вход: тело /auth/login (экранирование \" \\ и управляющих, undelete/login_source/gift_code_sku_id)");
    wipe(L.k->body, sizeof L.k->body);

    /* ветка: неверный пароль */
    feed(R_LOGIN, 400, "{\"code\":50035,\"errors\":{\"login\":{\"_errors\":[{\"code\":\"INVALID_LOGIN\",\"message\":\"Login or password is invalid.\"}]},"
                       "\"password\":{\"_errors\":[{\"code\":\"INVALID_LOGIN\",\"message\":\"Login or password is invalid.\"}]}},\"message\":\"Invalid Form Body\"}");
    check(L.stage == LG_FORM && L.err_field == LF_LOGIN && strstr(L.err, "Неверный") && zero(L.k->pass, sizeof L.k->pass),
          "вход: INVALID_LOGIN -> форма, ошибка у поля, пароль затёрт");

    /* ветка: 2FA */
    put(LF_PASSWORD, "secret-pass");
    feed(R_LOGIN, 200, "{\"user_id\":\"1\",\"mfa\":true,\"ticket\":\"WzEsMV0.tick-et\",\"login_instance_id\":\"abc-123\",\"sms\":true,\"totp\":true,\"backup\":true,\"webauthn\":null}");
    check(L.stage == LG_MFA && L.mfa_methods == (LM_TOTP | LM_SMS | LM_BACKUP) && L.mfa_method == LM_TOTP &&
          !strcmp(L.k->ticket, "WzEsMV0.tick-et") && !strcmp(L.k->lii, "abc-123") && zero(L.k->pass, sizeof L.k->pass),
          "вход: mfa+ticket -> экран кода (totp/sms/backup), тикет сохранён, пароль затёрт");
    put(LF_CODE, "123 456");
    login_submit();
    ok = L.stage == LG_BUSY && (L.want & R_MFA) && !strcmp(L.k->code, "123456") && build(R_MFA, path, sizeof path, &meth) &&
         !strcmp(path, "/auth/mfa/totp") && strstr(L.k->body, "\"code\":\"123456\"") && strstr(L.k->body, "\"ticket\":\"WzEsMV0.tick-et\"") &&
         strstr(L.k->body, "\"login_instance_id\":\"abc-123\"") && !strstr(L.k->body, "password");
    check(ok, "вход: код 2FA -> POST /auth/mfa/totp {code, ticket, login_instance_id}, без пароля");
    wipe(L.k->body, sizeof L.k->body);
    L.want = 0;
    feed(R_MFA, 400, "{\"message\":\"Invalid two-factor code\",\"code\":60008}");
    L.stage = LG_MFA;   /* feed ставит BUSY; back = MFA */
    feed(R_MFA, 400, "{\"message\":\"Invalid two-factor code\",\"code\":60008}");
    check(L.stage == LG_MFA && L.err_field == LF_CODE && zero(L.k->code, sizeof L.k->code), "вход: неверный код (60008) -> снова экран кода, поле очищено");
    L.stage = LG_MFA;
    login_mfa_method(LM_SMS);
    check(L.stage == LG_BUSY && (L.want & R_SMS) && build(R_SMS, path, sizeof path, &meth) && !strcmp(path, "/auth/mfa/sms/send"),
          "вход: выбор SMS -> POST /auth/mfa/sms/send {ticket}");
    wipe(L.k->body, sizeof L.k->body);
    L.want = 0;
    feed(R_SMS, 200, "{\"phone\":\"+*******0085\x1b[31m\"}");
    check(L.stage == LG_MFA && L.sms_sent && !strcmp(L.phone, "+*******0085"), "вход: SMS отправлено, номер очищен san()");

    /* ветка: токен в ответе 2FA */
    static const char TOK[] = "MTAwMDAwMDAwMDAwMDAwMDAw.AAAAAA.mincord-selftest-not-a-real-token";
    char tj[512];
    snprintf(tj, sizeof tj, "{\"token\":\"%s\",\"user_settings\":{\"locale\":\"ru\",\"theme\":\"dark\"}}", TOK);
    L.stage = LG_MFA;
    feed(R_MFA, 200, tj);
    check(L.stage == LG_DONE && secret_token() && !strcmp(secret_token(), TOK) && zero(L.k->ticket, sizeof L.k->ticket) &&
          zero(L.k->code, sizeof L.k->code) && zero(L.k->resp, sizeof L.k->resp),
          "вход: token -> готово, токен в secret.c, тикет/код/ответ затёрты");
    secret_set("", 0);
    login_back();
    L.stage = LG_FORM;

    /* ветка: капча -> предложен QR (без сети: только состояние) */
    put(LF_PASSWORD, "x");
    feed(R_LOGIN, 400, "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":\"a9b5fb07-92ff-493f-86fe-352a2803b3df\","
                       "\"captcha_service\":\"hcaptcha\",\"captcha_session_id\":\"s\",\"captcha_rqdata\":\"r\",\"captcha_rqtoken\":\"t\"}");
    check(L.stage == LG_CAPTCHA && !strcmp(L.csvc, "hcaptcha") && Q.st == QR_CONNECTING && Q.need && strstr(L.notice, "QR"),
          "вход: капча hCaptcha -> экран «капча», QR запускается сам");
    login_qr_stop();
    check(Q.st == QR_OFF && !Q.need && !Q.key, "вход: QR остановлен, ключа нет");

    /* ветка: новое место входа (почта) и ссылка из письма */
    feed(R_LOGIN, 400, "{\"code\":50035,\"errors\":{\"login\":{\"_errors\":[{\"code\":\"ACCOUNT_LOGIN_VERIFICATION_EMAIL\",\"message\":\"New login location detected, please check your e-mail.\"}]}},\"message\":\"Invalid Form Body\"}");
    check(L.stage == LG_EMAIL && L.k->pass[0], "вход: ACCOUNT_LOGIN_VERIFICATION_EMAIL -> «проверьте почту», пароль ждёт повтора");
    put(LF_CODE, "https://discord.com/authorize-ip?token=AbC.dEf_gh-123456789&utm=x");
    login_submit();
    check(L.stage == LG_BUSY && (L.want & R_AUTHIP) && !strcmp(L.k->ipt, "AbC.dEf_gh-123456789") && zero(L.k->code, sizeof L.k->code),
          "вход: ссылка из письма -> POST /auth/authorize-ip {token}");
    L.want = 0;
    feed(R_AUTHIP, 204, "");
    check(L.stage == LG_BUSY && (L.want & R_LOGIN) && zero(L.k->ipt, sizeof L.k->ipt), "вход: место подтверждено -> повтор входа тем же паролем");
    L.want = 0;
    L.back = LG_FORM;

    /* ветка: аккаунт отключён -> undelete */
    feed(R_LOGIN, 400, "{\"message\":\"This account is disabled\",\"code\":20013}");
    check(L.stage == LG_UNDELETE, "вход: 20013 (отключён) -> предложить восстановление");
    login_undelete();
    ok = L.undelete && (L.want & R_LOGIN) && build(R_LOGIN, path, sizeof path, &meth) && strstr(L.k->body, "\"undelete\":true");
    check(ok, "вход: восстановление -> /auth/login с undelete:true");
    wipe(L.k->body, sizeof L.k->body);
    L.want = 0;
    L.undelete = false;
    L.back = LG_FORM;

    /* ветка: телефон (70007), лимит, блокировка, чужие управляющие символы */
    feed(R_LOGIN, 400, "{\"message\":\"Phone verification required\",\"code\":70007}");
    check(L.stage == LG_PHONE, "вход: 70007 -> код из SMS для нового места входа");
    L.back = LG_FORM;
    feed(R_LOGIN, 429, "{\"message\":\"You are being rate limited.\",\"retry_after\":12.5,\"global\":false}");
    check(L.stage == LG_FORM && L.wait_until > now() + 11000 && L.wait_until < now() + 14000 && strstr(L.err, "Подождите"),
          "вход: 429 retry_after -> «подождите 13 с», кнопка заблокирована");
    login_submit();
    check(!(L.want & R_LOGIN), "вход: во время лимита повтор не уходит");
    L.wait_until = 0;
    feed(R_LOGIN, 403, "{\"user_id\":\"1\",\"suspended_user_token\":\"zzz\"}");
    check(L.stage == LG_FORM && strstr(L.err, "заблокирован"), "вход: 403 suspended_user_token -> «аккаунт заблокирован», токен не принят");
    feed(R_LOGIN, 400, "{\"code\":50035,\"errors\":{\"login\":{\"_errors\":[{\"code\":\"X\",\"message\":\"\x1b]0;evil\x07\xe2\x80\xaeok\"}]}}}");
    bool clean = true;
    for (const unsigned char *p = (const unsigned char *)L.err; *p; p++) if (*p < 0x20 || *p == 0x7f || (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0xAE)) clean = false;
    check(L.stage == LG_FORM && L.err_field == LF_LOGIN && clean && strstr(L.err, "Discord:"), "вход: текст ошибки Discord очищен san(), ошибка у поля");

    /* запасной путь: токен */
    login_use_token();
    put(LF_TOKEN, "  \"bad token\"  ");
    login_submit();
    check(L.stage == LG_TOKEN && L.err_field == LF_TOKEN && !secret_token() && zero(L.k->token, sizeof L.k->token), "вход: негодный токен отвергнут, поле затёрто");
    snprintf(tj, sizeof tj, " \"%s\" ", TOK);
    put(LF_TOKEN, tj);
    login_submit();
    check(L.stage == LG_BUSY && (L.want & R_TOKEN) && secret_token() && !strcmp(secret_token(), TOK), "вход: токен (с кавычками и пробелами) -> проверка GET /users/@me");
    L.want = 0;
    feed(R_TOKEN, 401, "{\"message\":\"401: Unauthorized\",\"code\":0}");
    check(L.stage == LG_TOKEN && !secret_token(), "вход: 401 на токен -> токен стёрт из secret.c");
    login_back();
    check(zero(L.k->pass, sizeof L.k->pass) && zero(L.k->ticket, sizeof L.k->ticket) && zero(L.k->code, sizeof L.k->code),
          "вход: «назад» затирает пароль, код и тикет");

    /* RSA-OAEP-SHA256 туда-обратно, отпечаток ключа */
    EVP_PKEY *key = rsa_new();
    char pub[600], fpr[48], ct[400];
    ok = key && key_spki(key, pub, sizeof pub, fpr, sizeof fpr) && strlen(fpr) == 43 && charset_ok(fpr, 43, "-_") && strlen(pub) > 380;
    check(ok, "QR: RSA-2048, SPKI base64 и отпечаток base64url(SHA-256) из 43 символов");
    unsigned char c[300];
    int cn = key ? rsa_crypt(key, true, (const unsigned char *)TOK, sizeof TOK - 1, c, sizeof c) : -1;
    ok = cn == 256 && b64_enc(c, 256, ct, sizeof ct, false) == 344 && rsa_dec_b64(key, ct) == (int)sizeof TOK - 1 && !strcmp((char *)L.k->pt, TOK);
    check(ok, "QR: RSA-OAEP-SHA256 шифрование -> base64 -> расшифровка совпала");
    wipe(L.k->pt, sizeof L.k->pt);
    ct[10] ^= 1;
    check(rsa_dec_b64(key, ct) < 0 && zero(L.k->pt, sizeof L.k->pt), "QR: испорченный шифротекст отвергнут, буфер чист");
    check(rsa_dec_b64(key, "@@@@") < 0 && rsa_dec_b64(key, "abc") < 0, "QR: не-base64 отвергнут");
    /* encrypted_token из /remote-auth/login -> токен */
    Q.key = key;
    cn = rsa_crypt(key, true, (const unsigned char *)TOK, sizeof TOK - 1, c, sizeof c);
    b64_enc(c, (size_t)cn, ct, sizeof ct, false);
    snprintf(tj, sizeof tj, "{\"encrypted_token\":\"%s\"}", ct);
    Q.st = QR_SCANNED;
    snprintf(L.k->ra_ticket, sizeof L.k->ra_ticket, "ticket");
    feed(R_RA, 200, tj);
    check(L.stage == LG_DONE && secret_token() && !strcmp(secret_token(), TOK) && zero(L.k->pt, sizeof L.k->pt) &&
          zero(L.k->ra_ticket, sizeof L.k->ra_ticket) && !Q.key && Q.st == QR_OFF,
          "QR: encrypted_token расшифрован -> готово, ключ и тикет затёрты");
    secret_set("", 0);

    /* данные пользователя из pending_ticket */
    u64 id = 0;
    char dsc[8], av[40], nm[96];
    ok = parse_user("852892297661906993:0:05145cc5646fbcba277b6d5ea2030610:dolfies", &id, dsc, sizeof dsc, av, sizeof av, nm, sizeof nm) &&
         id == 852892297661906993ULL && !strcmp(dsc, "0") && !strcmp(av, "05145cc5646fbcba277b6d5ea2030610") && !strcmp(nm, "dolfies");
    check(ok, "QR: данные пользователя id:discriminator:avatar:username разобраны");
    ok = parse_user("1:0:0:\x1b[2Jevil:name", &id, dsc, sizeof dsc, av, sizeof av, nm, sizeof nm) && !av[0] && !strchr(nm, 0x1b) &&
         !parse_user("x1:0:0:a", &id, dsc, sizeof dsc, av, sizeof av, nm, sizeof nm) && !parse_user("1:0", &id, dsc, sizeof dsc, av, sizeof av, nm, sizeof nm);
    check(ok, "QR: аватар \"0\" -> стандартный, ESC вырезан, мусор отвергнут");

    /* pending_remote_init: отпечаток сверяется, ссылка для QR */
    Q.key = rsa_new();
    ok = Q.key && key_spki(Q.key, pub, sizeof pub, fpr, sizeof fpr);
    memcpy(Q.fpr, fpr, sizeof fpr);
    char msg[160];
    snprintf(msg, sizeof msg, "{\"op\":\"pending_remote_init\",\"fingerprint\":\"%s\"}", fpr);
    Q.st = QR_CONNECTING;
    qr_message(msg, strlen(msg));
    const LoginView *lv = login_view();
    ok = ok && lv->qr == QR_SHOW && !strncmp(lv->qr_url, RA_PREFIX, sizeof RA_PREFIX - 1) && !strcmp(lv->qr_url + sizeof RA_PREFIX - 1, fpr) &&
         strlen(lv->qr_url) == 66;
    check(ok, "QR: pending_remote_init с нашим отпечатком -> ссылка https://discord.com/ra/<43 символа> для кодера");
    snprintf(msg, sizeof msg, "{\"op\":\"pending_remote_init\",\"fingerprint\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}");
    Q.st = QR_CONNECTING;
    qr_message(msg, strlen(msg));
    lv = login_view();
    check(lv->qr == QR_ERROR && !lv->qr_url[0] && !Q.key, "QR: чужой отпечаток -> ошибка, QR не показан, ключ уничтожен");
    /* капча на обмен тикета QR: тикет и ключ остаются, после проверки — повтор с заголовками */
    {
        Q.key = rsa_new();
        Q.st = QR_SCANNED;
        Q.got_login = true;
        snprintf(L.k->ra_ticket, sizeof L.k->ra_ticket, "ra-ticket-1");
        L.stage = LG_FORM;
        snprintf(L.k->resp, sizeof L.k->resp, "%s", "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":\"a9b5fb07-92ff-493f-86fe-352a2803b3df\","
                 "\"captcha_service\":\"hcaptcha\",\"captcha_session_id\":\"sess-1\",\"captcha_rqdata\":\"rq+data/1=\",\"captcha_rqtoken\":\"rqtok.1\"}");
        L.rlen = strlen(L.k->resp);
        on_resp(R_RA, 400);
        lv = login_view();
        ok = lv->stage == LG_FORM && lv->qr == QR_SCANNED && lv->captcha && lv->captcha_qr && !lv->captcha_open &&
             !strcmp(L.k->ra_ticket, "ra-ticket-1") && Q.key && !strcmp(L.csite, "a9b5fb07-92ff-493f-86fe-352a2803b3df") &&
             !strcmp(L.crqdata, "rq+data/1=") && !strcmp(L.crqtoken, "rqtok.1") && !strcmp(L.csess, "sess-1");
        check(ok, "капча: 400 на /remote-auth/login -> QR ждёт проверки, тикет и ключ RSA сохранены, sitekey/rqdata/rqtoken/session разобраны");
        snprintf(L.k->ckey, sizeof L.k->ckey, "P1_eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.test-answer");
        L.cap_key = true;
        struct curl_slist *h = headers(R_RA), *it;
        bool k1 = false, k2 = false, k3 = false, dbg = false;
        for (it = h; it; it = it->next) {
            k1 |= !strcmp(it->data, "X-Captcha-Key: P1_eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.test-answer");
            k2 |= !strcmp(it->data, "X-Captcha-Rqtoken: rqtok.1");
            k3 |= !strcmp(it->data, "X-Captcha-Session-Id: sess-1");
            dbg |= !strncmp(it->data, "X-Debug-Options", 15);
        }
        curl_slist_free_all(h);
        check(k1 && k2 && k3 && !dbg && zero(L.k->ckey, sizeof L.k->ckey) && L.cap_tried && !L.cap_key,
              "капча: повтор с X-Captcha-Key / X-Captcha-Rqtoken / X-Captcha-Session-Id, ответ затёрт, X-Debug-Options не шлём");
        on_resp(R_RA, 400);                     /* тот же ответ ещё раз: Discord не принял */
        check(L.cap_kind == R_RA && strstr(Q.text, "не принял") && !strcmp(L.k->ra_ticket, "ra-ticket-1"), "капча: ответ не принят -> пройти ещё раз, тикет цел");
        snprintf(L.k->resp, sizeof L.k->resp, "%s", "{\"message\":\"Invalid ticket\",\"code\":50035}");
        L.rlen = strlen(L.k->resp);
        on_resp(R_RA, 400);
        lv = login_view();
        check(!lv->captcha && lv->qr == QR_ERROR && !Q.key && zero(L.k->ra_ticket, sizeof L.k->ra_ticket), "капча: после ответа без капчи проверка сброшена, ключ и тикет затёрты");
        wipe(L.k->resp, sizeof L.k->resp);
        login_qr_stop();
    }
    /* куки Discord из ответа: только известные имена, в заголовок Cookie */
    {
        char hl[200];
        snprintf(hl, sizeof hl, "set-cookie: __dcfduid=abc123def; Expires=Tue, 01 Jan 2030; Path=/; HttpOnly\r\n");
        rest_header(hl, 1, strlen(hl), NULL);
        snprintf(hl, sizeof hl, "Set-Cookie: evil=1\r\n");
        rest_header(hl, 1, strlen(hl), NULL);
        snprintf(hl, sizeof hl, "Set-Cookie: __sdcfduid=x\x1by; Path=/\r\n");
        rest_header(hl, 1, strlen(hl), NULL);
        struct curl_slist *h = headers(R_LOGIN), *it;
        bool ck = false, bad = false;
        for (it = h; it; it = it->next) {
            if (!strncmp(it->data, "Cookie:", 7)) { ck = !strcmp(it->data, "Cookie: __dcfduid=abc123def"); bad = strstr(it->data, "evil") != NULL; }
        }
        curl_slist_free_all(h);
        check(ck && !bad, "куки: __dcfduid взят из Set-Cookie, чужие имена и управляющие символы отброшены");
        memset(L.cookie, 0, sizeof L.cookie);
    }
    /* ошибки по полям для ui.c */
    L.stage = LG_BUSY; L.back = LG_FORM;
    feed(R_LOGIN, 400, "{\"code\":50035,\"errors\":{\"login\":{\"_errors\":[{\"code\":\"INVALID_LOGIN\",\"message\":\"x\"}]}}}");
    lv = login_view();
    check(lv->err_login[0] && lv->err_pass[0] && !lv->err_code[0], "вход: INVALID_LOGIN -> ошибка и у логина, и у пароля (как в Discord)");
    login_start_password("", "pw");
    lv = login_view();
    check(lv->stage == LG_FORM && lv->err_login[0] && !lv->err_pass[0] && !(L.want & R_LOGIN), "вход: пустой логин -> ошибка у поля, запроса нет");
    login_start_password("me@example.com", "pw");
    check(L.stage == LG_BUSY && (L.want & R_LOGIN) && !strcmp(L.k->pass, "pw"), "вход: login_start_password -> запрос поставлен, пароль в закреплённой памяти");
    login_cancel();
    check(L.stage == LG_FORM && !L.want && zero(L.k, sizeof *L.k), "вход: отмена -> всё затёрто, запросов нет");

    login_finish();
    check(!login_active() && !L.k, "вход: login_finish освободил и затёр страницы");
    check(!CRYPTO_secure_malloc_initialized(), "вход: защищённая куча OpenSSL снята (ключи RSA освобождены, DAVE её не делит)");
    O.no_save = save_ns;
    if (own_secret) secret_destroy();
    return 0;
}

/* ======================= живая проверка окна капчи (тестовый ключ hCaptcha) ======================= */
int login_captcha_probe(void (*check)(bool ok, const char *what)) {
    if (!captcha_supported()) { check(true, "капча: WebView2 нет (Linux или не установлен) - проверка пропущена"); return 0; }
    if (!login_init()) { check(false, "капча: память"); return 1; }
    char err[200] = "";
    i64 t0 = now();
    bool ok = captcha_open(NULL, "", L.k->ckey, sizeof L.k->ckey, true, err, sizeof err);
    int r = 0;
    size_t pb0 = plat_private_bytes(), peak = pb0;
    while (ok && now() < t0 + 45000 && !(r = captcha_result(err, sizeof err))) {
        Waitable w[1];
        int n = 0;
        if (g_wake) w[n++] = wake_waitable(g_wake);
        plat_wait(w, n, 500);
        if (g_wake) wake_clear(g_wake);
        size_t pb = plat_private_bytes();
        if (pb > peak) peak = pb;
    }
    bool got = r == 1 && !strcmp(L.k->ckey, "10000000-aaaa-bbbb-cccc-000000000001");
    captcha_close();
    int used = 0, refused = 0;
    captcha_bridge_stats(&used, &refused);
    got = got && used > 0;
    char b[768];
    snprintf(b, sizeof b, "капча: WebView2 + страница под discord.com + hcaptcha.com через мост (%d соединений, отказано чужим: %d) -> "
             "ответ за %lld мс%s%s (память процесса: %zu -> пик %zu -> %zu КБ)", used, refused, (long long)(now() - t0),
             got ? "" : " - ", got ? "" : err, pb0 / 1024, peak / 1024, plat_private_bytes() / 1024);
    check(got, b);
    login_finish();
    return got ? 0 : 1;
}

/* ======================= живая проверка шлюза QR (без входа) ======================= */
int login_qr_probe(void (*check)(bool ok, const char *what)) {
    if (!login_init()) { check(false, "QR: память"); return 1; }
    L.selftest = true;
    login_qr_start();
    i64 t0 = now(), end = t0 + 25000;
    while (now() < end && (Q.st == QR_CONNECTING)) {
        Waitable w[2];
        int n = login_waitables(w, 2);
        i64 t = login_next_wake(), nw = now();
        if (t > end) t = end;
        plat_wait(w, n, t > nw ? (int)(t - nw > 200 ? 200 : t - nw) : 0);
        vless_service();
        login_service();
    }
    char b[512];
    snprintf(b, sizeof b, "QR: шлюз remote auth (живой): hello -> init -> nonce_proof -> pending_remote_init, отпечаток совпал (%lld мс)%s%s",
             (long long)(now() - t0), Q.st == QR_SHOW ? "" : " - ", Q.st == QR_SHOW ? "" : Q.text);
    bool ok = Q.st == QR_SHOW && strlen(Q.url) == sizeof RA_PREFIX - 1 + 43;
    check(ok, b);
    if (ok) {                                   /* до первого heartbeat и его подтверждения — без опроса */
        i64 hb = Q.next_hb, until = hb + 5000;
        int wakes = 0;
        while (now() < until && Q.st == QR_SHOW && (Q.next_hb == hb || !Q.acked)) {
            Waitable w[2];
            int n = login_waitables(w, 2);
            i64 t = login_next_wake(), nw = now();
            if (t > until) t = until;
            plat_wait(w, n, t > nw ? (int)(t - nw) : 0);
            wakes++;
            login_service();
        }
        snprintf(b, sizeof b, "QR: heartbeat через %lld мс подтверждён, QR жив; пробуждений цикла: %d", (long long)(now() - t0), wakes);
        ok = Q.st == QR_SHOW && Q.next_hb != hb && Q.acked && wakes < 20;
        check(ok, b);
    }
    login_qr_stop();
    login_finish();
    return ok ? 0 : 1;
}
