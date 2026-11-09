/* tls13.c — клиент TLS 1.3 для туннеля на C: ClientHello как у Chrome 133 (отпечаток uTLS
 * HelloChrome_Auto, которым пользуется Xray при fp=chrome), Reality (XTLS) и обычный TLS.
 *
 * Сверено с эталоном: utls v1.8.3-0.20260301010127 (u_parrots.go HelloChrome_133,
 * ApplyPreset, u_ech.go BoringGREASEECH, u_tls_extensions.go), xtls/reality v0.0.0-20261014101318
 * (tls.go: проверка ClientHello на сервере), Xray-core v26.10.31 transport/internet/reality (UClient).
 * Проверено на tls.peet.ws: JA4, ja4_r и peetprint совпадают с uTLS.
 *
 * Reality, клиентская часть (как UClient):
 *   - ClientHello строится целиком, legacy_session_id = 32 нуля;
 *   - AuthKey = HKDF-SHA256(X25519(наш ключ из доли X25519, pbk), соль = random[0:20], "REALITY", 32);
 *   - session_id = AES-256-GCM(AuthKey, nonce = random[20:32], {26,10,31,0, время(4), shortId(8)},
 *     aad = всё сообщение ClientHello с нулевым session_id) — 16 байт шифротекста + 16 метки;
 *   - сервер, узнавший нас, отвечает «временным» сертификатом ed25519, подпись которого равна
 *     HMAC-SHA512(AuthKey, открытый ключ); иначе он проксирует к настоящему сайту и сертификат будет
 *     настоящий — тогда соединение рвём (Xray в этом случае ещё «паучит» сайт для правдоподобия —
 *     здесь этого нет). С pqv дополнительно проверяется ML-DSA-65 из первого расширения сертификата.
 *   - CertificateVerify (только ed25519) и Finished проверяются всегда, как в Go.
 * Обычный TLS: цепочка проверяется по встроенным корням (roots.h, 18 центров) и имени/адресу.
 * Только TLS 1.3 (сервер TLS 1.2 — ошибка). HelloRetryRequest — по RFC 8446 §4.1.4 (один раз, доля
 * P-256/P-384, cookie; CCS для совместимости перед вторым ClientHello, как Chrome). Сжатый
 * сертификат (RFC 8879) — brotli, распакованный размер не больше 64 КБ, память декодера не больше
 * 1 МБ. Без возобновления сессий и 0-RTT. Ключи, доли и промежуточные секреты затираются; само
 * состояние соединения лежит в закреплённых страницах (не в подкачку). */
#include <string.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/crypto.h>
#include <brotli/decode.h>
#include "plat.h"
#include "tls13.h"
#include "roots.h"

typedef unsigned char u8;

/* ======================= буферы ======================= */
bool bb_reserve(Bb *b, size_t n) {
    if (b->cap - b->len >= n) return true;
    if (b->off) {                               /* сдвинуть данные в начало */
        memmove(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
        if (b->cap - b->len >= n) return true;
    }
    size_t need = b->len + n;
    if (need > BB_MAX) return false;
    size_t nc = b->cap ? b->cap : 512;
    while (nc < need) nc *= 2;
    u8 *q = OPENSSL_malloc(nc);
    if (!q) return false;
    if (b->len) memcpy(q, b->p, b->len);
    if (b->p) OPENSSL_clear_free(b->p, b->cap);
    b->p = q;
    b->cap = nc;
    return true;
}
bool bb_put(Bb *b, const void *d, size_t n) {
    if (!n) return true;
    if (!bb_reserve(b, n)) return false;
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return true;
}
void bb_eat(Bb *b, size_t n) {
    b->off += n;
    if (b->off >= b->len) {
        b->off = b->len = 0;
        if (b->cap > 32768) bb_free(b);         /* после всплеска вернуть память */
    }
}
void bb_free(Bb *b) {
    if (b->p) OPENSSL_clear_free(b->p, b->cap);
    b->p = NULL;
    b->off = b->len = b->cap = 0;
}

/* ======================= общее ======================= */
static EVP_KDF *g_hkdf;
static EVP_MAC *g_hmac;
static X509_STORE *g_store;                     /* корни для обычного TLS, загружаются при первой нужде */

bool t13_global_init(void) {
    if (!g_hkdf) g_hkdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    if (!g_hmac) g_hmac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    return g_hkdf && g_hmac;
}
void t13_global_free(void) {
    EVP_KDF_free(g_hkdf);
    EVP_MAC_free(g_hmac);
    X509_STORE_free(g_store);
    g_hkdf = NULL;
    g_hmac = NULL;
    g_store = NULL;
}

static X509_STORE *roots(void) {
    if (g_store) return g_store;
    X509_STORE *s = X509_STORE_new();
    BIO *b = s ? BIO_new_mem_buf(ROOTS_PEM, (int)sizeof ROOTS_PEM - 1) : NULL;
    int n = 0;
    if (b) {
        X509 *x;
        while ((x = PEM_read_bio_X509(b, NULL, NULL, NULL))) {
            if (X509_STORE_add_cert(s, x) == 1) n++;
            X509_free(x);
        }
        BIO_free(b);
    }
    if (!n) { X509_STORE_free(s); return NULL; }
    g_store = s;
    return s;
}

static void p16(u8 *p, size_t v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void p24(u8 *p, size_t v) { p[0] = (u8)(v >> 16); p[1] = (u8)(v >> 8); p[2] = (u8)v; }
static size_t g16(const u8 *p) { return (size_t)p[0] << 8 | p[1]; }
static size_t g24(const u8 *p) { return (size_t)p[0] << 16 | (size_t)p[1] << 8 | p[2]; }

/* HKDF: mode — EVP_KDF_HKDF_MODE_*; extract — выход длиной хэша. */
static bool hkdf(int mode, const EVP_MD *md, const u8 *salt, size_t sl, const u8 *key, size_t kl,
                 const u8 *info, size_t il, u8 *out, size_t ol) {
    EVP_KDF_CTX *c = EVP_KDF_CTX_new(g_hkdf);
    if (!c) return false;
    OSSL_PARAM p[6], *q = p;
    *q++ = OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode);
    *q++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)EVP_MD_get0_name(md), 0);
    *q++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)key, kl);
    if (sl) *q++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, sl);
    if (il) *q++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, il);
    *q = OSSL_PARAM_construct_end();
    bool ok = EVP_KDF_derive(c, out, ol, p) == 1;
    EVP_KDF_CTX_free(c);
    return ok;
}

/* HMAC по нескольким частям (a, b, c; пустые пропускаются). */
static bool hmac3(const char *md, const u8 *key, size_t kl, const u8 *a, size_t an, const u8 *b, size_t bn,
                  const u8 *c, size_t cn, u8 *out, size_t cap) {
    EVP_MAC_CTX *m = EVP_MAC_CTX_new(g_hmac);
    if (!m) return false;
    OSSL_PARAM p[2] = { OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, (char *)md, 0), OSSL_PARAM_construct_end() };
    size_t ol = 0;
    bool ok = EVP_MAC_init(m, key, kl, p) == 1 && (!an || EVP_MAC_update(m, a, an) == 1) &&
              (!bn || EVP_MAC_update(m, b, bn) == 1) && (!cn || EVP_MAC_update(m, c, cn) == 1) &&
              EVP_MAC_final(m, out, &ol, cap) == 1;
    EVP_MAC_CTX_free(m);
    return ok;
}

static bool x25519(EVP_PKEY *priv, const u8 pub[32], u8 out[32]) {
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, pub, 32);
    EVP_PKEY_CTX *c = peer ? EVP_PKEY_CTX_new_from_pkey(NULL, priv, NULL) : NULL;
    size_t n = 32;
    bool ok = c && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, peer) == 1 &&
              EVP_PKEY_derive(c, out, &n) == 1 && n == 32;
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(peer);
    return ok;
}

/* ECDH на P-256/P-384 (доля после HelloRetryRequest): точка сервера проверяется OpenSSL. */
static bool ecdh(EVP_PKEY *priv, const u8 *pub, size_t pn, u8 *out, size_t *on) {
    EVP_PKEY *peer = EVP_PKEY_new();
    EVP_PKEY_CTX *c = NULL;
    bool ok = peer && EVP_PKEY_copy_parameters(peer, priv) == 1 && EVP_PKEY_set1_encoded_public_key(peer, pub, pn) == 1 &&
              (c = EVP_PKEY_CTX_new_from_pkey(NULL, priv, NULL)) && EVP_PKEY_derive_init(c) == 1 &&
              EVP_PKEY_derive_set_peer(c, peer) == 1 && EVP_PKEY_derive(c, out, on) == 1;
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(peer);
    return ok;
}

static bool rnd(void *p, size_t n) { return RAND_bytes(p, (int)n) == 1; }
static bool rnd_below(unsigned n, unsigned *out) {      /* равномерно в [0, n) */
    unsigned lim = 0xFFFFFFFFu - 0xFFFFFFFFu % n, v;
    do { if (!rnd(&v, sizeof v)) return false; } while (v >= lim);
    *out = v % n;
    return true;
}

/* ======================= состояние ======================= */
enum { ST_SH, ST_EE, ST_CERT, ST_CV, ST_FIN, ST_APP, ST_FAIL };
enum { X_GREASE1, X_SNI, X_EMS, X_RENEG, X_GROUPS, X_POINTS, X_TICKET, X_ALPN, X_STATUS, X_SIGALGS,
       X_SCT, X_KEYSHARE, X_PSKMODES, X_VERSIONS, X_COMPRESS, X_ALPS, X_ECH, X_GREASE2, X_N };
typedef struct { EVP_CIPHER_CTX *c; u8 iv[12]; uint64_t seq; bool on; } Aead;

struct Tls {
    const TlsCfg *cfg;
    int st;
    const char *why;
    EVP_PKEY *kx, *kxh, *kml;                   /* доли: X25519, X25519 гибрида, ML-KEM-768 */
    EVP_PKEY *kec;                              /* доля P-256/P-384 после HelloRetryRequest */
    u8 *ch; size_t chn;                         /* наш ClientHello (сообщение с заголовком) */
    u8 *sh; size_t shn;                         /* ServerHello (для ML-DSA Reality) */
    u8 random[32], sid[32];
    u8 auth[32];                                /* Reality AuthKey */
    uint16_t gs[5];                             /* GREASE: шифр, группа, расширение 1 и 2, версия */
    int ord[X_N];                               /* порядок расширений (тот же во втором ClientHello) */
    unsigned ech_cfg, ech_len;
    u8 ech_enc[32], ech_pay[240];
    bool hrr, ccs_sent;
    bool alps;                                  /* сервер согласовал ALPS: ответить своими (пустыми) настройками */
    int nccs;                                   /* CCS от сервера: не больше двух (после HRR и после SH) */
    unsigned hrr_group, hrr_suite;
    u8 *cookie; size_t cookien;
    u8 *thpre; size_t thpren;                   /* message_hash(CH1) || HRR — начало стенограммы */
    const EVP_MD *md; size_t hl;
    const EVP_CIPHER *ciph; size_t kl;
    EVP_MD_CTX *th;                             /* хэш стенограммы */
    u8 c_hs[48], s_hs[48], hsec[48];            /* секреты рукопожатия */
    u8 c_ap[48], s_ap[48];                      /* секреты приложения (для KeyUpdate) */
    Aead rd, wr;
    Bb hs;                                      /* сборка сообщений рукопожатия */
    X509 *leaf;
    STACK_OF(X509) *chain;
};

const char *t13_why(const Tls *t) { return t->why ? t->why : ""; }
bool t13_ready(const Tls *t) { return t->st == ST_APP; }

static void freebuf(u8 **p, size_t *n) { if (*p) OPENSSL_clear_free(*p, *n); *p = NULL; *n = 0; }

static void drop_hs_state(Tls *t) {
    EVP_PKEY_free(t->kx); EVP_PKEY_free(t->kxh); EVP_PKEY_free(t->kml); EVP_PKEY_free(t->kec);
    t->kx = t->kxh = t->kml = t->kec = NULL;
    freebuf(&t->ch, &t->chn);
    freebuf(&t->sh, &t->shn);
    freebuf(&t->cookie, &t->cookien);
    freebuf(&t->thpre, &t->thpren);
    EVP_MD_CTX_free(t->th);
    t->th = NULL;
    X509_free(t->leaf);
    t->leaf = NULL;
    if (t->chain) sk_X509_pop_free(t->chain, X509_free);
    t->chain = NULL;
    OPENSSL_cleanse(t->c_hs, sizeof t->c_hs);
    OPENSSL_cleanse(t->s_hs, sizeof t->s_hs);
    OPENSSL_cleanse(t->hsec, sizeof t->hsec);
    OPENSSL_cleanse(t->auth, sizeof t->auth);
}

void t13_free(Tls *t) {
    if (!t) return;
    drop_hs_state(t);
    EVP_CIPHER_CTX_free(t->rd.c);
    EVP_CIPHER_CTX_free(t->wr.c);
    bb_free(&t->hs);
    OPENSSL_cleanse(t, sizeof *t);
    plat_unlock(t, sizeof *t);
    plat_pages_free(t, sizeof *t);
}

static int fail(Tls *t, const char *why) { t->st = ST_FAIL; if (!t->why) t->why = why; return -1; }

/* ======================= ClientHello как у Chrome 133 =======================
 * Порядок расширений в спецификации uTLS; GREASE первым и последним, остальные перемешиваются
 * так же, как ShuffleChromeTLSExtensions (Fisher–Yates math/rand, обмены с GREASE пропускаются). */
typedef struct { u8 *p; size_t n, cap; bool bad; } W;
static void wb(W *w, const void *d, size_t n) {
    if (w->bad || w->cap - w->n < n) { w->bad = true; return; }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}
static void w8(W *w, unsigned v) { u8 b = (u8)v; wb(w, &b, 1); }
static void w16(W *w, unsigned v) { u8 b[2] = { (u8)(v >> 8), (u8)v }; wb(w, b, 2); }
static size_t wopen(W *w, int lb) { size_t at = w->n; u8 z[3] = {0}; wb(w, z, (size_t)lb); return at; }
static void wclose(W *w, size_t at, int lb) {
    if (w->bad) return;
    size_t v = w->n - at - (size_t)lb;
    if (lb == 1) { if (v > 255) w->bad = true; else w->p[at] = (u8)v; }
    else if (lb == 2) { if (v > 65535) w->bad = true; else p16(w->p + at, v); }
    else p24(w->p + at, v);
}
static unsigned grease(uint16_t seed) { unsigned r = (seed & 0xf0) | 0x0a; return r | r << 8; }

static bool pub_raw(EVP_PKEY *k, u8 *out, size_t want) {
    size_t n = want;
    if (EVP_PKEY_get_raw_public_key(k, out, &n) == 1 && n == want) return true;
    n = 0;
    return EVP_PKEY_get_octet_string_param(k, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, out, want, &n) == 1 && n == want;
}

static bool is_ip_literal(const char *s) {
    if (!*s) return true;
    if (strchr(s, ':')) return true;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || *s == '.')) return false;
    return true;
}

typedef struct { const u8 *mlkem, *xh, *x, *ec; size_t ecn; } Shares;

static void put_ext(Tls *t, W *w, int id, const Shares *k) {
    const char *sni = t->cfg->sni;
    const uint16_t *gs = t->gs;
    size_t a, b;
    switch (id) {
    case X_GREASE1: w16(w, grease(gs[2])); w16(w, 0); break;
    case X_GREASE2: w16(w, grease(gs[3])); w16(w, 1); w8(w, 0); break;
    case X_SNI:
        if (is_ip_literal(sni)) break;          /* uTLS (как Chrome) не шлёт SNI для адресов */
        w16(w, 0x0000); a = wopen(w, 2); b = wopen(w, 2);
        w8(w, 0); w16(w, (unsigned)strlen(sni)); wb(w, sni, strlen(sni));
        wclose(w, b, 2); wclose(w, a, 2);
        break;
    case X_EMS: w16(w, 0x0017); w16(w, 0); break;
    case X_RENEG: w16(w, 0xff01); w16(w, 1); w8(w, 0); break;
    case X_GROUPS:
        w16(w, 0x000a); w16(w, 12); w16(w, 10);
        w16(w, grease(gs[1])); w16(w, 0x11ec); w16(w, 0x001d); w16(w, 0x0017); w16(w, 0x0018);
        break;
    case X_POINTS: w16(w, 0x000b); w16(w, 2); w8(w, 1); w8(w, 0); break;
    case X_TICKET: w16(w, 0x0023); w16(w, 0); break;
    case X_ALPN:
        w16(w, 0x0010); a = wopen(w, 2); b = wopen(w, 2);
        if (t->cfg->alpn == TLS_ALPN_HTTP11) { w8(w, 8); wb(w, "http/1.1", 8); }
        else { w8(w, 2); wb(w, "h2", 2); w8(w, 8); wb(w, "http/1.1", 8); }
        wclose(w, b, 2); wclose(w, a, 2);
        break;
    case X_STATUS: w16(w, 0x0005); w16(w, 5); w8(w, 1); w16(w, 0); w16(w, 0); break;
    case X_SIGALGS: {
        static const uint16_t sa[] = { 0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601 };
        w16(w, 0x000d); w16(w, 2 + 2 * 8); w16(w, 2 * 8);
        for (int i = 0; i < 8; i++) w16(w, sa[i]);
        break;
    }
    case X_SCT: w16(w, 0x0012); w16(w, 0); break;
    case X_KEYSHARE:
        w16(w, 0x0033); a = wopen(w, 2); b = wopen(w, 2);
        if (!t->hrr) {
            w16(w, grease(gs[1])); w16(w, 1); w8(w, 0);
            w16(w, 0x11ec); w16(w, 1184 + 32); wb(w, k->mlkem, 1184); wb(w, k->xh, 32);
            w16(w, 0x001d); w16(w, 32); wb(w, k->x, 32);
        } else {                                /* после HRR — только запрошенная группа (как BoringSSL) */
            w16(w, t->hrr_group); w16(w, (unsigned)k->ecn); wb(w, k->ec, k->ecn);
        }
        wclose(w, b, 2); wclose(w, a, 2);
        if (t->cookie) {                        /* cookie из HRR — сразу за key_share */
            w16(w, 0x002c); w16(w, (unsigned)(t->cookien + 2)); w16(w, (unsigned)t->cookien); wb(w, t->cookie, t->cookien);
        }
        break;
    case X_PSKMODES: w16(w, 0x002d); w16(w, 2); w8(w, 1); w8(w, 1); break;
    case X_VERSIONS: w16(w, 0x002b); w16(w, 7); w8(w, 6); w16(w, grease(gs[4])); w16(w, 0x0304); w16(w, 0x0303); break;
    case X_COMPRESS: w16(w, 0x001b); w16(w, 3); w8(w, 2); w16(w, 0x0002); break;
    case X_ALPS: w16(w, 0x44cd); w16(w, 5); w16(w, 3); w8(w, 2); wb(w, "h2", 2); break;
    case X_ECH:
        /* GREASE ECH (BoringGREASEECH): outer, HKDF-SHA256 + AES-128-GCM, случайный config_id,
         * enc — настоящий открытый ключ X25519, полезная нагрузка случайная 128/160/192/224 + 16.
         * Во втором ClientHello (HRR) — то же расширение целиком (draft ECH §6.2). */
        w16(w, 0xfe0d); a = wopen(w, 2);
        w8(w, 0); w16(w, 0x0001); w16(w, 0x0001); w8(w, t->ech_cfg);
        w16(w, 32); wb(w, t->ech_enc, 32);
        w16(w, t->ech_len); wb(w, t->ech_pay, t->ech_len);
        wclose(w, a, 2);
        break;
    }
}

/* Собрать ClientHello из состояния в t->ch (первый или второй — после HRR). */
static bool write_hello(Tls *t) {
    u8 mlkem_pub[1184], xh_pub[32], x_pub[32], ec_pub[100];
    Shares k = { mlkem_pub, xh_pub, x_pub, ec_pub, 0 };
    if (!t->hrr) {
        if (!pub_raw(t->kml, mlkem_pub, sizeof mlkem_pub) || !pub_raw(t->kxh, xh_pub, 32) || !pub_raw(t->kx, x_pub, 32)) return false;
    } else if (EVP_PKEY_get_octet_string_param(t->kec, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, ec_pub, sizeof ec_pub, &k.ecn) != 1) {
        return false;
    }
    freebuf(&t->ch, &t->chn);
    size_t cap = 4096 + t->cookien;
    t->ch = OPENSSL_malloc(cap);
    if (!t->ch) return false;
    W w = { t->ch, 0, cap, false };
    w8(&w, 1);
    size_t m = wopen(&w, 3);
    w16(&w, 0x0303);
    wb(&w, t->random, 32);
    w8(&w, 32); wb(&w, t->sid, 32);
    static const uint16_t cs[] = { 0x1301, 0x1302, 0x1303, 0xc02b, 0xc02f, 0xc02c, 0xc030, 0xcca9, 0xcca8,
                                   0xc013, 0xc014, 0x009c, 0x009d, 0x002f, 0x0035 };
    w16(&w, 2 * 16);
    w16(&w, grease(t->gs[0]));
    for (int i = 0; i < 15; i++) w16(&w, cs[i]);
    w8(&w, 1); w8(&w, 0);
    size_t e = wopen(&w, 2);
    for (int i = 0; i < X_N; i++) put_ext(t, &w, t->ord[i], &k);
    wclose(&w, e, 2);
    wclose(&w, m, 3);
    if (w.bad) return false;
    t->chn = w.n;
    return true;
}

static EVP_PKEY *keygen(const char *alg) { return EVP_PKEY_Q_keygen(NULL, NULL, alg); }

/* Первый ClientHello: случайные значения, доли, Reality. */
static bool build_hello(Tls *t) {
    EVP_PKEY *ech = NULL;
    bool ok = false;
    t->kml = keygen("ML-KEM-768");
    t->kxh = keygen("X25519");
    t->kx = keygen("X25519");
    ech = keygen("X25519");
    unsigned li;
    static const unsigned ech_lens[4] = { 144, 176, 208, 240 };
    if (!t->kml || !t->kxh || !t->kx || !ech || !pub_raw(ech, t->ech_enc, 32)) goto out;
    if (!rnd(t->gs, sizeof t->gs) || !rnd(t->random, 32) || !rnd_below(256, &t->ech_cfg) || !rnd_below(4, &li)) goto out;
    t->ech_len = ech_lens[li];
    if (!rnd(t->ech_pay, t->ech_len)) goto out;
    if (grease(t->gs[2]) == grease(t->gs[3])) t->gs[3] ^= 0x1010;
    if (t->cfg->reality) memset(t->sid, 0, 32);
    else if (!rnd(t->sid, 32)) goto out;
    for (int i = 0; i < X_N; i++) t->ord[i] = i;
    for (int i = X_N - 1; i > 0; i--) {
        unsigned j;
        if (!rnd_below((unsigned)i + 1, &j)) goto out;
        if (t->ord[i] == X_GREASE1 || t->ord[i] == X_GREASE2 || t->ord[j] == X_GREASE1 || t->ord[j] == X_GREASE2) continue;
        int tmp = t->ord[i]; t->ord[i] = t->ord[(int)j]; t->ord[(int)j] = tmp;
    }
    if (!write_hello(t)) goto out;

    if (t->cfg->reality) {
        /* session_id = AES-GCM(AuthKey) — как Xray UClient */
        u8 shared[32], plain[16];
        if (!x25519(t->kx, t->cfg->pbk, shared)) goto out;
        bool k = hkdf(EVP_KDF_HKDF_MODE_EXTRACT_AND_EXPAND, EVP_sha256(), t->random, 20, shared, 32,
                      (const u8 *)"REALITY", 7, t->auth, 32);
        OPENSSL_cleanse(shared, 32);
        if (!k) goto out;
        plain[0] = 26; plain[1] = 10; plain[2] = 31; plain[3] = 0;   /* версия Xray-core 26.10.31 */
        uint32_t now = (uint32_t)time(NULL);
        plain[4] = (u8)(now >> 24); plain[5] = (u8)(now >> 16); plain[6] = (u8)(now >> 8); plain[7] = (u8)now;
        memcpy(plain + 8, t->cfg->sid, 8);
        EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
        int l1 = 0, l2 = 0;
        u8 ct[16], tag[16];
        k = c && EVP_EncryptInit_ex2(c, EVP_aes_256_gcm(), t->auth, t->random + 20, NULL) == 1 &&
            EVP_EncryptUpdate(c, NULL, &l1, t->ch, (int)t->chn) == 1 &&
            EVP_EncryptUpdate(c, ct, &l1, plain, 16) == 1 && EVP_EncryptFinal_ex(c, ct + l1, &l2) == 1 &&
            l1 + l2 == 16 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1;
        EVP_CIPHER_CTX_free(c);
        OPENSSL_cleanse(plain, sizeof plain);
        if (!k) goto out;
        memcpy(t->sid, ct, 16);
        memcpy(t->sid + 16, tag, 16);
        memcpy(t->ch + 39, t->sid, 32);         /* 4 заголовок + 2 версия + 32 random + 1 длина */
    }
    ok = true;
out:
    EVP_PKEY_free(ech);
    return ok;
}

/* Открытое сообщение рукопожатия в записи(ях) не длиннее 2^14 (RFC 8446 §5.1). */
static bool put_record(Bb *net, unsigned ver_lo, const u8 *p, size_t n) {
    while (n) {
        size_t k = n > 16384 ? 16384 : n;
        if (!bb_reserve(net, 5 + k)) return false;
        u8 *r = net->p + net->len;
        r[0] = 22; r[1] = 3; r[2] = (u8)ver_lo; p16(r + 3, k);
        memcpy(r + 5, p, k);
        net->len += 5 + k;
        p += k;
        n -= k;
    }
    return true;
}

Tls *t13_new(const TlsCfg *cfg, Bb *net) {
    Tls *t = plat_pages(sizeof *t);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    plat_lock(t, sizeof *t);                    /* ключи сессии — не в подкачку */
    t->cfg = cfg;
    t->st = ST_SH;
    /* первая запись — версия 0x0301, как у Go/Chrome до согласования */
    if (!build_hello(t) || !put_record(net, 1, t->ch, t->chn)) { t13_free(t); return NULL; }
    return t;
}

/* ======================= ключевое расписание ======================= */
static bool expand_label(Tls *t, const u8 *secret, const char *label, const u8 *ctx, size_t cl, u8 *out, size_t ol) {
    u8 info[2 + 1 + 6 + 32 + 1 + 64];
    size_t ll = strlen(label);
    if (ll > 32 || cl > 64) return false;
    p16(info, ol);
    info[2] = (u8)(6 + ll);
    memcpy(info + 3, "tls13 ", 6);
    memcpy(info + 9, label, ll);
    info[9 + ll] = (u8)cl;
    if (cl) memcpy(info + 10 + ll, ctx, cl);
    return hkdf(EVP_KDF_HKDF_MODE_EXPAND_ONLY, t->md, NULL, 0, secret, t->hl, info, 10 + ll + cl, out, ol);
}

static bool th_hash(Tls *t, u8 *out) {
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    unsigned n = 0;
    bool ok = c && EVP_MD_CTX_copy_ex(c, t->th) == 1 && EVP_DigestFinal_ex(c, out, &n) == 1 && n == t->hl;
    EVP_MD_CTX_free(c);
    return ok;
}

static bool aead_set(Tls *t, Aead *a, const u8 *secret, bool enc) {
    u8 key[32];
    bool ok = expand_label(t, secret, "key", NULL, 0, key, t->kl) && expand_label(t, secret, "iv", NULL, 0, a->iv, 12);
    if (ok && !a->c) a->c = EVP_CIPHER_CTX_new();
    ok = ok && a->c && (enc ? EVP_EncryptInit_ex2(a->c, t->ciph, key, NULL, NULL) : EVP_DecryptInit_ex2(a->c, t->ciph, key, NULL, NULL)) == 1;
    OPENSSL_cleanse(key, sizeof key);
    a->seq = 0;
    a->on = ok;
    return ok;
}

static void nonce(const Aead *a, u8 n[12]) {
    memcpy(n, a->iv, 12);
    for (int i = 0; i < 8; i++) n[4 + i] ^= (u8)(a->seq >> (56 - 8 * i));
}

static bool seal_rec(Tls *t, u8 type, const u8 *p, size_t n, Bb *out) {
    if (n > 16384 || !bb_reserve(out, 5 + n + 1 + 16)) return false;
    u8 *r = out->p + out->len, nn[12];
    r[0] = 23; r[1] = 3; r[2] = 3; p16(r + 3, n + 1 + 16);
    memmove(r + 5, p, n);
    r[5 + n] = type;
    nonce(&t->wr, nn);
    int l = 0, l2 = 0;
    EVP_CIPHER_CTX *c = t->wr.c;
    bool ok = EVP_EncryptInit_ex2(c, NULL, NULL, nn, NULL) == 1 && EVP_EncryptUpdate(c, NULL, &l, r, 5) == 1 &&
              EVP_EncryptUpdate(c, r + 5, &l, r + 5, (int)n + 1) == 1 && EVP_EncryptFinal_ex(c, r + 5 + l, &l2) == 1 &&
              (size_t)(l + l2) == n + 1 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, r + 6 + n) == 1;
    if (!ok) { OPENSSL_cleanse(r, 5 + n + 17); return false; }
    t->wr.seq++;
    out->len += 5 + n + 17;
    return true;
}

bool t13_seal(Tls *t, const void *p, size_t n, Bb *net) {
    if (t->st != ST_APP) return false;
    const u8 *s = p;
    while (n) {
        size_t k = n > 16384 ? 16384 : n;
        if (!seal_rec(t, 23, s, k, net)) return false;
        s += k;
        n -= k;
    }
    return true;
}

/* Расшифровать запись на месте: rec — заголовок, тело L байт. Возвращает длину содержимого или -1;
 * *type — внутренний тип. Открытый текст не больше 2^14 + 1 (RFC 8446 §5.4). */
static long open_rec(Tls *t, u8 *rec, size_t L, u8 *type) {
    if (L < 17 || L - 16 > 16385) return -1;
    u8 nn[12];
    nonce(&t->rd, nn);
    EVP_CIPHER_CTX *c = t->rd.c;
    int l = 0, l2 = 0;
    bool ok = EVP_DecryptInit_ex2(c, NULL, NULL, nn, NULL) == 1 && EVP_DecryptUpdate(c, NULL, &l, rec, 5) == 1 &&
              EVP_DecryptUpdate(c, rec + 5, &l, rec + 5, (int)(L - 16)) == 1 &&
              EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, rec + 5 + L - 16) == 1 &&
              EVP_DecryptFinal_ex(c, rec + 5 + l, &l2) == 1;
    if (!ok) return -1;
    t->rd.seq++;
    long n = (long)(L - 16);
    while (n > 0 && rec[5 + n - 1] == 0) n--;
    if (n <= 0) return -1;
    *type = rec[5 + n - 1];
    return n - 1;
}

/* ======================= сообщения сервера ======================= */
static bool set_suite(Tls *t, unsigned suite) {
    switch (suite) {
    case 0x1301: t->md = EVP_sha256(); t->ciph = EVP_aes_128_gcm(); t->kl = 16; break;
    case 0x1302: t->md = EVP_sha384(); t->ciph = EVP_aes_256_gcm(); t->kl = 32; break;
    case 0x1303: t->md = EVP_sha256(); t->ciph = EVP_chacha20_poly1305(); t->kl = 32; break;
    default: return false;
    }
    t->hl = (size_t)EVP_MD_get_size(t->md);
    return true;
}

/* ServerHello / HelloRetryRequest: строгий разбор (RFC 8446 §4.1.3–4.1.4). */
typedef struct { unsigned suite, ver, group; const u8 *key, *cookie; size_t keyn, cookien; bool sid_ok; } ShInfo;
static const char *parse_sh(Tls *t, const u8 *m, size_t n, bool hrr, ShInfo *s) {
    memset(s, 0, sizeof *s);
    size_t p = 34, sl = m[p++];
    if (sl > 32 || n < p + sl + 3) return "ServerHello: коротко";
    s->sid_ok = sl == 32 && !memcmp(m + p, t->sid, 32);
    p += sl;
    s->suite = (unsigned)g16(m + p);
    p += 2;
    if (m[p++] != 0) return "ServerHello: сжатие";
    if (n < p + 2) return "сервер не поддерживает TLS 1.3";
    size_t el = g16(m + p);
    p += 2;
    if (p + el != n) return "ServerHello: длина расширений";
    bool seen_v = false, seen_k = false, seen_c = false, alien = false;
    while (p < n) {
        if (p + 4 > n) return "ServerHello: расширение";
        unsigned et = (unsigned)g16(m + p);
        size_t ln = g16(m + p + 2);
        p += 4;
        if (p + ln > n) return "ServerHello: расширение";
        const u8 *v = m + p;
        if (et == 0x002b) {
            if (seen_v || ln != 2) return "ServerHello: supported_versions";
            seen_v = true;
            s->ver = (unsigned)g16(v);
        } else if (et == 0x0033) {
            if (seen_k) return "ServerHello: key_share повторяется";
            seen_k = true;
            if (hrr) {
                if (ln != 2) return "HelloRetryRequest: key_share";
                s->group = (unsigned)g16(v);
            } else {
                if (ln < 4 || g16(v + 2) + 4 != ln) return "ServerHello: key_share";
                s->group = (unsigned)g16(v);
                s->key = v + 4;
                s->keyn = ln - 4;
            }
        } else if (et == 0x002c && hrr) {
            if (seen_c || ln < 3 || g16(v) + 2 != ln || ln > 4096) return "HelloRetryRequest: cookie";
            seen_c = true;
            s->cookie = v + 2;
            s->cookien = ln - 2;
        } else alien = true;                    /* решение — после версии: у сервера TLS 1.2 свои расширения */
        p += ln;
    }
    if (s->ver != 0x0304) return "сервер не поддерживает TLS 1.3";
    if (alien) return "ServerHello: непредложенное расширение";
    if (!s->sid_ok) return "ServerHello: session_id не наш";
    if (!seen_k) return "ServerHello: нет key_share";
    return NULL;
}

/* HelloRetryRequest: один раз, только для группы, предложенной без доли (P-256/P-384). */
static int on_hrr(Tls *t, const u8 *m, size_t n, const u8 *raw, size_t rawn, Bb *net) {
    if (t->cfg->reality) return fail(t, "Reality: сервер прислал HelloRetryRequest");
    if (t->hrr) return fail(t, "второй HelloRetryRequest");
    ShInfo s;
    const char *e = parse_sh(t, m, n, true, &s);
    if (e) return fail(t, e);
    if (!set_suite(t, s.suite)) return fail(t, "HelloRetryRequest: набор шифров");
    if (s.group != 0x0017 && s.group != 0x0018) return fail(t, "HelloRetryRequest: группа");
    /* стенограмма: message_hash(Hash(CH1)) || HRR (RFC 8446 §4.4.1) */
    t->thpren = 4 + t->hl + rawn;
    t->thpre = OPENSSL_malloc(t->thpren);
    unsigned hn = 0;
    if (!t->thpre || EVP_Digest(t->ch, t->chn, t->thpre + 4, &hn, t->md, NULL) != 1 || hn != t->hl)
        return fail(t, "HelloRetryRequest: хэш");
    t->thpre[0] = 254; t->thpre[1] = 0; t->thpre[2] = 0; t->thpre[3] = (u8)t->hl;
    memcpy(t->thpre + 4 + t->hl, raw, rawn);
    if (s.cookie) {
        t->cookie = OPENSSL_memdup(s.cookie, s.cookien);
        if (!t->cookie) return fail(t, "память");
        t->cookien = s.cookien;
    }
    EVP_PKEY_free(t->kml); EVP_PKEY_free(t->kxh); EVP_PKEY_free(t->kx);
    t->kml = t->kxh = t->kx = NULL;
    t->kec = EVP_PKEY_Q_keygen(NULL, NULL, "EC", s.group == 0x0017 ? "P-256" : "P-384");
    t->hrr = true;
    t->hrr_group = s.group;
    t->hrr_suite = s.suite;
    static const u8 ccs[6] = { 20, 3, 3, 0, 1, 1 };
    if (!t->kec || !write_hello(t) || !bb_put(net, ccs, 6) || !put_record(net, 3, t->ch, t->chn))
        return fail(t, "второй ClientHello");
    t->ccs_sent = true;
    return 0;
}

static int on_server_hello(Tls *t, const u8 *m, size_t n, const u8 *raw, size_t rawn, Bb *net) {
    static const u8 hrr[32] = { 0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
                                0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };
    if (n < 2 + 32 + 1) return fail(t, "ServerHello: коротко");
    if (g16(m) != 0x0303) return fail(t, "ServerHello: версия");
    if (!memcmp(m + 2, hrr, 32)) return on_hrr(t, m, n, raw, rawn, net);
    ShInfo s;
    const char *e = parse_sh(t, m, n, false, &s);
    if (e) return fail(t, e);
    if (t->hrr ? s.suite != t->hrr_suite : !set_suite(t, s.suite)) return fail(t, "ServerHello: набор шифров");
    u8 shared[64];
    size_t sn = 0;
    if (t->hrr) {
        size_t want = t->hrr_group == 0x0017 ? 65 : 97;
        sn = sizeof shared;
        if (s.group != t->hrr_group || s.keyn != want || !ecdh(t->kec, s.key, s.keyn, shared, &sn))
            return fail(t, "ServerHello: доля после HRR");
    } else if (s.group == 0x001d && s.keyn == 32) {
        if (!x25519(t->kx, s.key, shared)) return fail(t, "X25519");
        sn = 32;
    } else if (s.group == 0x11ec && s.keyn == 1088 + 32) {
        EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_pkey(NULL, t->kml, NULL);
        size_t ml = 32;
        bool ok = c && EVP_PKEY_decapsulate_init(c, NULL) == 1 && EVP_PKEY_decapsulate(c, shared, &ml, s.key, 1088) == 1 && ml == 32;
        EVP_PKEY_CTX_free(c);
        if (!ok || !x25519(t->kxh, s.key + 1088, shared + 32)) { OPENSSL_cleanse(shared, sizeof shared); return fail(t, "X25519MLKEM768"); }
        sn = 64;
    } else return fail(t, "ServerHello: группа ключей");

    t->sh = OPENSSL_memdup(raw, rawn);
    t->shn = rawn;
    t->th = EVP_MD_CTX_new();
    u8 zero[48] = {0}, early[48], derived[48], h[48];
    unsigned hn = 0;
    bool ok = t->sh && t->th && EVP_DigestInit_ex(t->th, t->md, NULL) == 1 &&
              (!t->thpre || EVP_DigestUpdate(t->th, t->thpre, t->thpren) == 1) &&
              EVP_DigestUpdate(t->th, t->ch, t->chn) == 1 && EVP_DigestUpdate(t->th, raw, rawn) == 1 &&
              hkdf(EVP_KDF_HKDF_MODE_EXTRACT_ONLY, t->md, zero, t->hl, zero, t->hl, NULL, 0, early, t->hl) &&
              EVP_Digest("", 0, h, &hn, t->md, NULL) == 1 &&
              expand_label(t, early, "derived", h, t->hl, derived, t->hl) &&
              hkdf(EVP_KDF_HKDF_MODE_EXTRACT_ONLY, t->md, derived, t->hl, shared, sn, NULL, 0, t->hsec, t->hl) &&
              th_hash(t, h) &&
              expand_label(t, t->hsec, "c hs traffic", h, t->hl, t->c_hs, t->hl) &&
              expand_label(t, t->hsec, "s hs traffic", h, t->hl, t->s_hs, t->hl) &&
              aead_set(t, &t->rd, t->s_hs, false) && aead_set(t, &t->wr, t->c_hs, true);
    OPENSSL_cleanse(shared, sizeof shared);
    OPENSSL_cleanse(early, sizeof early);
    OPENSSL_cleanse(derived, sizeof derived);
    if (!ok) return fail(t, "ключи рукопожатия");
    EVP_PKEY_free(t->kml); EVP_PKEY_free(t->kxh); EVP_PKEY_free(t->kx); EVP_PKEY_free(t->kec);
    t->kml = t->kxh = t->kx = t->kec = NULL;    /* доли больше не нужны */
    t->st = ST_EE;
    return 0;
}

/* EncryptedExtensions: только предложенные расширения, без повторов; ALPN — из предложенных. */
static int on_ee(Tls *t, const u8 *m, size_t n) {
    if (n < 2 || g16(m) + 2 != n) return fail(t, "EncryptedExtensions: длина");
    unsigned seen[8];
    int ns = 0;
    for (size_t p = 2; p < n;) {
        if (p + 4 > n) return fail(t, "EncryptedExtensions: расширение");
        unsigned et = (unsigned)g16(m + p);
        size_t ln = g16(m + p + 2);
        p += 4;
        if (p + ln > n) return fail(t, "EncryptedExtensions: расширение");
        for (int i = 0; i < ns; i++) if (seen[i] == et) return fail(t, "EncryptedExtensions: повтор");
        if (ns == 8) return fail(t, "EncryptedExtensions: слишком много");
        seen[ns++] = et;
        const u8 *v = m + p;
        switch (et) {
        case 0x0000: if (ln) return fail(t, "EncryptedExtensions: server_name"); break;
        case 0x0010: {
            if (ln < 3 || g16(v) + 2 != ln || (size_t)v[2] + 3 != ln) return fail(t, "EncryptedExtensions: ALPN");
            bool h2 = v[2] == 2 && !memcmp(v + 3, "h2", 2), h11 = v[2] == 8 && !memcmp(v + 3, "http/1.1", 8);
            if (!h11 && !(h2 && t->cfg->alpn == TLS_ALPN_CHROME)) return fail(t, "сервер выбрал непредложенный ALPN");
            if (h2 && t->cfg->http11_only) return fail(t, "сервер выбрал h2 - WebSocket невозможен (уберите h2 из alpn)");
            break;
        }
        case 0x44cd: t->alps = true; break;    /* ALPS (настройки HTTP/2 сервера) — нам не нужны, ответить надо */
        case 0x000a: case 0xfe0d: break;        /* группы сервера, ECH retry_configs */
        default: return fail(t, "EncryptedExtensions: непредложенное расширение");
        }
        p += ln;
    }
    t->st = ST_CERT;
    return 0;
}

/* DER: прочитать TLV в [p, e): тег, длина (до 4 байт), начало значения. false — не DER. */
static bool der(const u8 **p, const u8 *e, unsigned *tag, const u8 **v, size_t *vl) {
    const u8 *q = *p;
    if (e - q < 2) return false;
    *tag = *q++;
    size_t l = *q++;
    if (l & 0x80) {
        int k = (int)(l & 0x7f);
        if (k < 1 || k > 4 || e - q < k) return false;
        for (l = 0; k--; ) l = l << 8 | *q++;
    }
    if ((size_t)(e - q) < l) return false;
    *v = q;
    *vl = l;
    *p = q + l;
    return true;
}
/* Алгоритм открытого ключа сертификата — из известных (RSA, RSA-PSS, EC, Ed25519)? Проверяется до
 * d2i_X509: иначе OpenSSL на каждый новый OID заводит запись в кэше декодеров (рост памяти от
 * подставного сервера ещё до проверки подписи). */
static bool spki_known(const u8 *c, size_t n) {
    static const u8 rsa[] = { 6, 9, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 1, 1 },
                    pss[] = { 6, 9, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 1, 10 },
                    ec[] = { 6, 7, 0x2a, 0x86, 0x48, 0xce, 0x3d, 2, 1 },
                    ed[] = { 6, 3, 0x2b, 0x65, 0x70 };
    const u8 *p = c, *e = c + n, *v, *tbs, *te;
    size_t vl;
    unsigned tag;
    if (!der(&p, e, &tag, &v, &vl) || tag != 0x30) return false;        /* Certificate */
    p = v; e = v + vl;
    if (!der(&p, e, &tag, &tbs, &vl) || tag != 0x30) return false;      /* tbsCertificate */
    te = tbs + vl;
    p = tbs;
    if (!der(&p, te, &tag, &v, &vl)) return false;
    if (tag == 0xa0 && !der(&p, te, &tag, &v, &vl)) return false;       /* [0] version -> serial */
    for (int i = 0; i < 4; i++) if (!der(&p, te, &tag, &v, &vl)) return false;   /* signature, issuer, validity, subject */
    if (!der(&p, te, &tag, &v, &vl) || tag != 0x30) return false;       /* subjectPublicKeyInfo */
    const u8 *s = v, *se = v + vl, *alg;
    if (!der(&s, se, &tag, &alg, &vl) || tag != 0x30) return false;     /* AlgorithmIdentifier */
    const u8 *a = alg, *oid;
    size_t ol;
    if (!der(&a, alg + vl, &tag, &oid, &ol) || tag != 6) return false;
    size_t full = (size_t)(oid - alg) + ol;                             /* OID с тегом и длиной */
#define IS(x) (full == sizeof x && !memcmp(alg, x, sizeof x))
    return IS(rsa) || IS(pss) || IS(ec) || IS(ed);
#undef IS
}

static int on_certificate(Tls *t, const u8 *m, size_t n) {
    if (n < 4 || m[0] != 0) return fail(t, "Certificate: контекст");
    size_t ln = g24(m + 1), p = 4;
    if (ln + 4 != n) return fail(t, "Certificate: длина");
    t->chain = sk_X509_new_null();
    if (!t->chain) return fail(t, "память");
    int cnt = 0;
    while (p < n) {
        if (p + 3 > n) return fail(t, "Certificate: запись");
        size_t cl = g24(m + p);
        p += 3;
        if (p + cl + 2 > n || !cl) return fail(t, "Certificate: запись");
        const u8 *q = m + p;
        if (!spki_known(q, cl)) return fail(t, "Certificate: неизвестный алгоритм ключа");
        X509 *x = d2i_X509(NULL, &q, (long)cl);
        if (!x || q != m + p + cl) { X509_free(x); return fail(t, "Certificate: не разобран"); }
        if (!t->leaf) t->leaf = x;
        else if (cnt > 10 || !sk_X509_push(t->chain, x)) { X509_free(x); return fail(t, "Certificate: цепочка"); }
        cnt++;
        p += cl;
        size_t xl = g16(m + p);
        p += 2 + xl;
        if (p > n) return fail(t, "Certificate: расширения");
    }
    if (!t->leaf) return fail(t, "Certificate: пусто");
    EVP_PKEY *pk = X509_get0_pubkey(t->leaf);
    if (!pk) return fail(t, "Certificate: ключ");

    if (t->cfg->reality) {
        u8 pub[32], mac[64];
        size_t pl = 32;
        const ASN1_BIT_STRING *sig = NULL;
        X509_get0_signature(&sig, NULL, t->leaf);
        if (!EVP_PKEY_is_a(pk, "ED25519") || EVP_PKEY_get_raw_public_key(pk, pub, &pl) != 1 || pl != 32 || !sig ||
            ASN1_STRING_length(sig) != 64 ||
            !hmac3("SHA512", t->auth, 32, pub, 32, NULL, 0, NULL, 0, mac, sizeof mac) ||
            CRYPTO_memcmp(mac, ASN1_STRING_get0_data(sig), 64)) {
            OPENSSL_cleanse(mac, sizeof mac);
            return fail(t, "Reality: сервер прислал настоящий сертификат (не узнал ключ/shortId или подмена)");
        }
        if (t->cfg->pqv) {
            /* HMAC-SHA512(AuthKey, pub || ClientHello || ServerHello), подпись — в первом расширении */
            X509_EXTENSION *ext = X509_get_ext(t->leaf, 0);
            ASN1_OCTET_STRING *v = ext ? X509_EXTENSION_get_data(ext) : NULL;
            EVP_PKEY *mk = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-DSA-65", NULL, t->cfg->pqv, 1952);
            EVP_MD_CTX *mc = EVP_MD_CTX_new();
            bool ok = v && mk && mc && hmac3("SHA512", t->auth, 32, pub, 32, t->ch, t->chn, t->sh, t->shn, mac, sizeof mac) &&
                      EVP_DigestVerifyInit_ex(mc, NULL, NULL, NULL, NULL, mk, NULL) == 1 &&
                      EVP_DigestVerify(mc, ASN1_STRING_get0_data(v), (size_t)ASN1_STRING_length(v), mac, 64) == 1;
            EVP_MD_CTX_free(mc);
            EVP_PKEY_free(mk);
            OPENSSL_cleanse(mac, sizeof mac);
            if (!ok) return fail(t, "Reality: подпись ML-DSA-65 не сошлась");
        }
        OPENSSL_cleanse(mac, sizeof mac);
    } else {
        X509_STORE *s = roots();
        X509_STORE_CTX *c = s ? X509_STORE_CTX_new() : NULL;
        bool ok = c && X509_STORE_CTX_init(c, s, t->leaf, t->chain) == 1;
        if (ok) {
            X509_VERIFY_PARAM *vp = X509_STORE_CTX_get0_param(c);
            X509_STORE_CTX_set_purpose(c, X509_PURPOSE_SSL_SERVER);
            X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (t->cfg->sni[0] && !is_ip_literal(t->cfg->sni)) ok = X509_VERIFY_PARAM_set1_host(vp, t->cfg->sni, 0) == 1;
            else if (t->cfg->verify_ip) ok = X509_VERIFY_PARAM_set1_ip_asc(vp, t->cfg->verify_ip) == 1;
            else ok = false;
            ok = ok && X509_verify_cert(c) == 1;
        }
        X509_STORE_CTX_free(c);
        if (!ok) return fail(t, "TLS: сертификат сервера не прошёл проверку");
    }
    t->st = ST_CV;
    return 0;
}

/* ---- сжатый сертификат (RFC 8879): brotli с ограничением памяти декодера ---- */
typedef struct { size_t used, cap; } BrBudget;
static void *br_alloc(void *o, size_t n) {
    BrBudget *b = o;
    if (n > b->cap - b->used) return NULL;
    size_t *p = OPENSSL_malloc(n + 16);
    if (!p) return NULL;
    p[0] = n;
    b->used += n;
    return (u8 *)p + 16;
}
static void br_free(void *o, void *a) {
    if (!a) return;
    BrBudget *b = o;
    size_t *p = (size_t *)(void *)((u8 *)a - 16);
    b->used -= p[0];
    OPENSSL_clear_free(p, p[0] + 16);
}
static int on_compressed_cert(Tls *t, const u8 *m, size_t n) {
    if (n < 8) return fail(t, "CompressedCertificate: коротко");
    unsigned alg = (unsigned)g16(m);
    size_t ulen = g24(m + 2), clen = g24(m + 5);
    if (alg != 2) return fail(t, "CompressedCertificate: непредложенный алгоритм");
    if (clen + 8 != n || !clen || !ulen || ulen > 65536) return fail(t, "CompressedCertificate: длина");
    BrBudget bud = { 0, 1u << 20 };
    BrotliDecoderState *s = BrotliDecoderCreateInstance(br_alloc, br_free, &bud);
    u8 *out = OPENSSL_malloc(ulen);
    size_t ai = clen, ao = ulen, total = 0;
    const uint8_t *ni = m + 8;
    uint8_t *no = out;
    bool ok = s && out && BrotliDecoderDecompressStream(s, &ai, &ni, &ao, &no, &total) == BROTLI_DECODER_RESULT_SUCCESS &&
              ai == 0 && total == ulen;
    if (s) BrotliDecoderDestroyInstance(s);
    int r = ok ? on_certificate(t, out, ulen) : fail(t, "CompressedCertificate: не распакован");
    if (out) OPENSSL_clear_free(out, ulen);
    return r;
}

static int on_cert_verify(Tls *t, const u8 *m, size_t n) {
    if (n < 4) return fail(t, "CertificateVerify: коротко");
    unsigned scheme = (unsigned)g16(m);
    size_t sl = g16(m + 2);
    if (sl + 4 != n) return fail(t, "CertificateVerify: длина");
    u8 content[64 + 34 + 48];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    if (!th_hash(t, content + 98)) return fail(t, "хэш");
    EVP_PKEY *pk = X509_get0_pubkey(t->leaf);
    const EVP_MD *md = NULL;
    const char *kind, *curve = NULL;
    bool pss = false;
    /* Reality — только ed25519; обычный TLS — только схемы из нашего signature_algorithms, годные
     * для TLS 1.3 (PKCS#1 v1.5 запрещён), ECDSA — с кривой своего хэша. */
    if (t->cfg->reality) {
        if (scheme != 0x0807) return fail(t, "Reality: подпись не ed25519");
        kind = "ED25519";
    } else switch (scheme) {
    case 0x0403: kind = "EC"; md = EVP_sha256(); curve = "prime256v1"; break;
    case 0x0503: kind = "EC"; md = EVP_sha384(); curve = "secp384r1"; break;
    case 0x0804: kind = "RSA"; md = EVP_sha256(); pss = true; break;
    case 0x0805: kind = "RSA"; md = EVP_sha384(); pss = true; break;
    case 0x0806: kind = "RSA"; md = EVP_sha512(); pss = true; break;
    default: return fail(t, "CertificateVerify: непредложенная схема подписи");
    }
    if (!pk || !EVP_PKEY_is_a(pk, kind)) return fail(t, "CertificateVerify: тип ключа");
    if (curve) {
        char g[32];
        size_t gl = 0;
        if (EVP_PKEY_get_group_name(pk, g, sizeof g, &gl) != 1 || strcmp(g, curve)) return fail(t, "CertificateVerify: кривая не та");
    }
    EVP_MD_CTX *mc = EVP_MD_CTX_new();
    EVP_PKEY_CTX *pc = NULL;
    bool ok = mc && EVP_DigestVerifyInit(mc, &pc, md, NULL, pk) == 1 &&
              (!pss || (EVP_PKEY_CTX_set_rsa_padding(pc, RSA_PKCS1_PSS_PADDING) == 1 &&
                        EVP_PKEY_CTX_set_rsa_pss_saltlen(pc, RSA_PSS_SALTLEN_DIGEST) == 1)) &&
              EVP_DigestVerify(mc, m + 4, sl, content, 98 + t->hl) == 1;
    EVP_MD_CTX_free(mc);
    if (!ok) return fail(t, "CertificateVerify: подпись не сошлась");
    t->st = ST_FIN;
    return 0;
}

/* Finished сервера: проверить, послать свой CCS (если ещё не был) + Finished, перейти на ключи приложения. */
static int on_finished(Tls *t, const u8 *m, size_t n, const u8 *raw, size_t rawn, Bb *net) {
    u8 fk[48], h[48], v[64], derived[48], master[48], zero[48] = {0}, fin[4 + 48];
    unsigned hn = 0;
    int r = 0;
    bool ok = n == t->hl && expand_label(t, t->s_hs, "finished", NULL, 0, fk, t->hl) && th_hash(t, h) &&
              hmac3(EVP_MD_get0_name(t->md), fk, t->hl, h, t->hl, NULL, 0, NULL, 0, v, sizeof v) &&
              !CRYPTO_memcmp(v, m, t->hl);
    if (!ok) { r = fail(t, "Finished сервера не сошёлся"); goto out; }
    ok = EVP_DigestUpdate(t->th, raw, rawn) == 1 && th_hash(t, h) &&
         EVP_Digest("", 0, derived, &hn, t->md, NULL) == 1 &&
         expand_label(t, t->hsec, "derived", derived, t->hl, derived, t->hl) &&
         hkdf(EVP_KDF_HKDF_MODE_EXTRACT_ONLY, t->md, derived, t->hl, zero, t->hl, NULL, 0, master, t->hl) &&
         expand_label(t, master, "c ap traffic", h, t->hl, t->c_ap, t->hl) &&
         expand_label(t, master, "s ap traffic", h, t->hl, t->s_ap, t->hl);
    if (!ok) { r = fail(t, "ключи приложения"); goto out; }
    /* ALPS: клиентские EncryptedExtensions (тип 8, расширение 0x44cd, пустые настройки — как uTLS
     * без ApplicationSettings) — после Finished сервера, до своего Finished, входят в стенограмму */
    static const u8 cee[10] = { 8, 0, 0, 6, 0, 4, 0x44, 0xcd, 0, 0 };
    if (t->alps && (EVP_DigestUpdate(t->th, cee, sizeof cee) != 1 || !th_hash(t, h))) { r = fail(t, "ALPS"); goto out; }
    ok = expand_label(t, t->c_hs, "finished", NULL, 0, fk, t->hl) &&
         hmac3(EVP_MD_get0_name(t->md), fk, t->hl, h, t->hl, NULL, 0, NULL, 0, v, sizeof v);
    if (!ok) { r = fail(t, "Finished клиента"); goto out; }
    /* CCS для совместимости (как Chrome/uTLS при непустом session_id) — один раз за рукопожатие */
    static const u8 ccs[6] = { 20, 3, 3, 0, 1, 1 };
    fin[0] = 20;
    p24(fin + 1, t->hl);
    memcpy(fin + 4, v, t->hl);
    ok = (t->ccs_sent || bb_put(net, ccs, 6)) && (!t->alps || seal_rec(t, 22, cee, sizeof cee, net)) &&
         seal_rec(t, 22, fin, 4 + t->hl, net) &&
         aead_set(t, &t->wr, t->c_ap, true) && aead_set(t, &t->rd, t->s_ap, false);
    if (!ok) { r = fail(t, "Finished клиента"); goto out; }
    t->ccs_sent = true;
    drop_hs_state(t);
    t->st = ST_APP;
out:
    OPENSSL_cleanse(fk, sizeof fk);
    OPENSSL_cleanse(v, sizeof v);
    OPENSSL_cleanse(derived, sizeof derived);
    OPENSSL_cleanse(master, sizeof master);
    OPENSSL_cleanse(fin, sizeof fin);
    return r;
}

static int on_post_handshake(Tls *t, unsigned typ, const u8 *m, size_t n, Bb *net) {
    if (typ == 4) return 0;                     /* NewSessionTicket — возобновление не используем */
    if (typ != 24 || n != 1 || m[0] > 1) return fail(t, "неожиданное сообщение после рукопожатия");
    /* KeyUpdate */
    if (!expand_label(t, t->s_ap, "traffic upd", NULL, 0, t->s_ap, t->hl) || !aead_set(t, &t->rd, t->s_ap, false))
        return fail(t, "KeyUpdate");
    if (m[0] == 1) {
        static const u8 ku[5] = { 24, 0, 0, 1, 0 };
        if (!seal_rec(t, 22, ku, 5, net) || !expand_label(t, t->c_ap, "traffic upd", NULL, 0, t->c_ap, t->hl) ||
            !aead_set(t, &t->wr, t->c_ap, true))
            return fail(t, "KeyUpdate");
    }
    return 0;
}

static int hs_process(Tls *t, Bb *net) {
    while (bb_n(&t->hs) >= 4) {
        u8 *raw = bb_ptr(&t->hs);
        size_t n = g24(raw + 1);
        if (n > 65536 + 8) return fail(t, "сообщение рукопожатия слишком большое");
        if (bb_n(&t->hs) < 4 + n) return 0;
        unsigned typ = raw[0];
        const u8 *m = raw + 4;
        int r = 0;
        bool keychange = false;                 /* после него запись должна кончиться (RFC 8446 §5.1) */
        switch (t->st) {
        case ST_SH:
            if (typ != 2) return fail(t, "ожидался ServerHello");
            r = on_server_hello(t, m, n, raw, 4 + n, net);
            keychange = true;
            break;
        case ST_EE:
            if (typ != 8) return fail(t, "ожидались EncryptedExtensions");
            r = on_ee(t, m, n);
            if (!r && EVP_DigestUpdate(t->th, raw, 4 + n) != 1) return fail(t, "хэш");
            break;
        case ST_CERT:
            if (typ == 13) return fail(t, "сервер требует сертификат клиента");
            if (typ != 11 && typ != 25) return fail(t, "ожидался Certificate");
            r = typ == 11 ? on_certificate(t, m, n) : on_compressed_cert(t, m, n);
            if (!r && EVP_DigestUpdate(t->th, raw, 4 + n) != 1) return fail(t, "хэш");
            break;
        case ST_CV:
            if (typ != 15) return fail(t, "ожидался CertificateVerify");
            r = on_cert_verify(t, m, n);
            if (!r && EVP_DigestUpdate(t->th, raw, 4 + n) != 1) return fail(t, "хэш");
            break;
        case ST_FIN:
            if (typ != 20) return fail(t, "ожидался Finished");
            r = on_finished(t, m, n, raw, 4 + n, net);
            keychange = true;
            break;
        case ST_APP:
            r = on_post_handshake(t, typ, m, n, net);
            keychange = typ == 24;
            break;
        default:
            return -1;
        }
        if (r) return r;
        bb_eat(&t->hs, 4 + n);
        if (keychange && bb_n(&t->hs)) return fail(t, "лишние данные на границе смены ключей");
    }
    return 0;
}

int t13_input(Tls *t, Bb *in, Bb *app, Bb *net) {
    if (t->st == ST_FAIL) return -1;
    while (bb_n(in) >= 5) {
        u8 *r = bb_ptr(in);
        size_t L = g16(r + 3);
        if (r[1] != 3 || L > 16384 + 256) return fail(t, "запись TLS: заголовок");
        if (bb_n(in) < 5 + L) return 0;
        u8 type = r[0];
        if (type == 20) {                       /* ChangeCipherSpec для совместимости: пропустить */
            bool allowed = t->st == ST_EE || t->st == ST_CERT || t->st == ST_CV || t->st == ST_FIN || (t->st == ST_SH && t->hrr);
            if (!allowed || L != 1 || r[5] != 1 || ++t->nccs > 2) return fail(t, "запись TLS: CCS");
        } else if (!t->rd.on) {
            if (type == 21) return fail(t, "сервер прервал рукопожатие (alert)");
            if (type != 22 || !L || L > 16384) return fail(t, "запись TLS: ожидалось рукопожатие");
            if (!bb_put(&t->hs, r + 5, L)) return fail(t, "память");
            int k = hs_process(t, net);
            if (k) return k;
        } else {
            if (type != 23) return fail(t, "запись TLS: тип");
            u8 it = 0;
            long n = open_rec(t, r, L, &it);
            if (n < 0) return fail(t, "запись TLS не расшифрована");
            if (it == 22) {
                if (!n || !bb_put(&t->hs, r + 5, (size_t)n)) return fail(t, "запись TLS: рукопожатие");
                int k = hs_process(t, net);
                OPENSSL_cleanse(r + 5, (size_t)n);
                if (k) return k;
            } else if (it == 21) {
                bool close = n == 2 && r[6] == 0;
                bb_eat(in, 5 + L);
                if (close) return 1;
                return fail(t, "сервер прислал alert");
            } else if (it == 23) {
                if (t->st != ST_APP) return fail(t, "данные до конца рукопожатия");
                if (!bb_put(app, r + 5, (size_t)n)) return fail(t, "память");
                OPENSSL_cleanse(r + 5, (size_t)n);
            } else return fail(t, "запись TLS: внутренний тип");
        }
        bb_eat(in, 5 + L);
    }
    return 0;
}
