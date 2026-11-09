/* Общие функции: буферы, UTF-8, очистка текста, JSON в одноразовой куче, время. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "mc.h"
#include "plat.h"

/* ---------- память ---------- */
static void oom(void) {
    /* Без памяти работать нельзя; без вывода в файл — просто завершаемся. */
    abort();
}
void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) oom(); return p; }
void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); if (!p) oom(); return p; }
void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) oom(); return q; }
char *xstrdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}
void wipe(void *p, size_t n) { plat_wipe(p, n); }

/* ---------- Buf ---------- */
void buf_put(Buf *b, const void *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 64;
        while (c < b->len + n + 1) c *= 2;
        /* не realloc: старый блок мог хранить секрет (пароль, ссылку vless, конфиг туннеля) —
         * копируем, затираем и только потом освобождаем (SECURITY.md Б1) */
        char *q = xmalloc(c);
        if (b->p) {
            memcpy(q, b->p, b->len + 1);
            wipe(b->p, b->cap);
            free(b->p);
        }
        b->p = q;
        b->cap = c;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_put(b, tmp, (size_t)n); wipe(tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_put(b, big, (size_t)n);
    wipe(big, (size_t)n);                        /* промежуточная копия могла содержать секрет */
    free(big);
}
void buf_json_str(Buf *b, const char *s) {
    buf_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': buf_put(b, "\\\"", 2); break;
        case '\\': buf_put(b, "\\\\", 2); break;
        case '\n': buf_put(b, "\\n", 2); break;
        case '\r': buf_put(b, "\\r", 2); break;
        case '\t': buf_put(b, "\\t", 2); break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else buf_put(b, p, 1);
        }
    }
    buf_put(b, "\"", 1);
}
void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }
void buf_wipe(Buf *b) { wipe(b->p, b->cap); buf_free(b); }

/* ---------- время ---------- */
i64 g_clock_skew;                               /* selftest: сдвиг монотонных часов (имитация времени) */
i64 now_ms(void) { return plat_mono_ms() + g_clock_skew; }
i64 wall_ms(void) { return plat_wall_ms(); }
u64 snowflake_now(void) { return (u64)(wall_ms() - DISCORD_EPOCH_MS) << 22; }

i64 iso_to_unix(const char *iso) {
    if (!iso) return 0;
    int Y, M, D, h, m, s, used = 0;
    /* %n — реальная длина разобранного: строка от сервера может быть короче 19 символов */
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d%n", &Y, &M, &D, &h, &m, &s, &used) != 6 || used <= 0) return 0;
    struct tm t = { .tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = m, .tm_sec = s };
    i64 r = plat_timegm(&t);
    /* смещение часового пояса вида +03:00 после секунд и долей */
    const char *z = iso + used;
    while (*z == '.' || (*z >= '0' && *z <= '9')) z++;
    if ((*z == '+' || *z == '-') && z[1] >= '0' && z[1] <= '9' && z[2] >= '0' && z[2] <= '9' && z[3] == ':' &&
        z[4] >= '0' && z[4] <= '9' && z[5] >= '0' && z[5] <= '9') {
        int oh = (z[1] - '0') * 10 + (z[2] - '0'), om = (z[4] - '0') * 10 + (z[5] - '0');
        i64 off = (i64)(oh * 3600 + om * 60);
        r += (*z == '+') ? -off : off;
    }
    return r < 0 ? 0 : r;
}
void fmt_time(i64 unix_s, char *out, size_t n) {
    struct tm lt, ln;
    plat_localtime(unix_s, &lt);
    plat_localtime(wall_ms() / 1000, &ln);
    if (lt.tm_year == ln.tm_year && lt.tm_yday == ln.tm_yday)
        snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    else
        snprintf(out, n, "%02d.%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
}

/* ---------- UTF-8 ---------- */
int utf8_dec(const char *s, u32 *cp) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    int n; u32 c, min;
    if ((p[0] & 0xE0) == 0xC0) { n = 2; c = p[0] & 0x1F; min = 0x80; }
    else if ((p[0] & 0xF0) == 0xE0) { n = 3; c = p[0] & 0x0F; min = 0x800; }
    else if ((p[0] & 0xF8) == 0xF0) { n = 4; c = p[0] & 0x07; min = 0x10000; }
    else { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    *cp = c;
    return n;
}
int utf8_enc(u32 c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}
/* Ширина по Unicode East Asian Width (W/F), как у wcwidth в терминалах.
 * Таблицы отсортированы: поиск двоичный. */
typedef struct { u32 a, b; } Range;
static const Range ZERO[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
    {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
    {0x200B, 0x200F}, {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0x1F3FB, 0x1F3FF}, {0xE0000, 0xE007F}, {0xE0100, 0xE01EF},
};
static const Range WIDE[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250, 0x1F251},
    {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};
static bool in_ranges(const Range *r, int n, u32 c) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c < r[mid].a) hi = mid - 1;
        else if (c > r[mid].b) lo = mid + 1;
        else return true;
    }
    return false;
}
int cp_width(u32 c) {
    if (c == 0 || c == 0x200D) return 0;
    if (c < 0x300) return 1;
    if (in_ranges(ZERO, (int)(sizeof ZERO / sizeof *ZERO), c)) return 0;
    if (in_ranges(WIDE, (int)(sizeof WIDE / sizeof *WIDE), c)) return 2;
    return 1;
}
int str_width(const char *s) {
    int w = 0;
    while (*s) { u32 c; s += utf8_dec(s, &c); w += cp_width(c); }
    return w;
}

/* Всё, что пришло из сети, проходит здесь до хранения. Удаляются:
 * C0 (кроме \n при multiline; \t -> пробел), DEL, C1 (там ESC-последовательности
 * 8-битного терминала), управление направлением текста (подмена видимого порядка),
 * разделители строк Unicode. Битый UTF-8 становится U+FFFD. */
static bool dangerous(u32 c) {
    if (c < 0x20 || c == 0x7F) return true;
    if (c >= 0x80 && c <= 0x9F) return true;
    if (c >= 0x202A && c <= 0x202E) return true;
    if (c >= 0x2066 && c <= 0x2069) return true;
    if (c == 0x200E || c == 0x200F || c == 0x061C) return true;
    if (c == 0x2028 || c == 0x2029) return true;
    if (c == 0xFEFF) return true;
    return false;
}
/* D-4: в строке есть управляющие (C0/C1), DEL или смена направления — путь такой не принимаем */
bool text_unsafe(const char *s) {
    if (!s) return true;
    while (*s) {
        u32 c;
        int n = utf8_dec(s, &c);
        if (dangerous(c) || c == 0xFFFD) return true;
        s += n;
    }
    return false;
}
char *san(const char *s, bool multiline) {
    if (!s) return xstrdup("");
    Buf b = {0};
    while (*s) {
        u32 c;
        int n = utf8_dec(s, &c);
        if (c == 0x1B || c == 0x9B || c == 0x9D) {   /* последовательность терминала целиком, не только ESC */
            const char *p = s + n;
            if (c == 0x1B && *p == '[') p++;
            if (c == 0x9B || p > s + n) {             /* CSI: параметры 0x20..0x3F, конец — 0x40..0x7E */
                int k = 0;
                while (*p && (unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x3F && k < 32) { p++; k++; }
                if ((unsigned char)*p >= 0x40 && (unsigned char)*p <= 0x7E) p++;
                s = p;
                continue;
            }
            if (c == 0x9D || *p == ']') {             /* OSC: до BEL или ESC \ (не дальше 256 байт) */
                if (c == 0x1B) p++;
                int k = 0;
                while (*p && *p != 0x07 && *p != 0x1B && k < 256) { p++; k++; }
                if (*p == 0x07) p++;
                else if (*p == 0x1B && p[1] == '\\') p += 2;
                s = p;
                continue;
            }
            s += n;                                   /* одиночный ESC */
            continue;
        }
        if (c == '\n' && multiline) buf_put(&b, "\n", 1);
        else if (c == '\t' || (c == '\n' && !multiline)) buf_put(&b, " ", 1);
        else if (c == '\r') { /* пропуск */ }
        else if (dangerous(c)) { /* пропуск */ }
        else if (c == 0xFFFD) buf_put(&b, "\xEF\xBF\xBD", 3);
        else buf_put(&b, s, (size_t)n);
        s += n;
    }
    if (!b.p) return xstrdup("");
    return b.p;
}

/* ---------- JSON в одноразовой арене ----------
 * Свой у каждого потока. Память берётся страницами прямо у системы и после json_done()
 * возвращается ей целиком: пиковый READY в десятки мегабайт не оседает в куче процесса. */
typedef struct Chunk { struct Chunk *next; size_t size, used; } Chunk;
static __thread Chunk *t_arena;
static __thread bool t_in_arena;

static void *jmalloc(size_t n) {
    if (!t_in_arena) return malloc(n);
    n = (n + 15) & ~(size_t)15;
    Chunk *c = t_arena;
    if (!c || c->used + n > c->size) {
        size_t sz = (size_t)256 << 10;
        while (sz < n + sizeof(Chunk) + 16) sz *= 2;
        Chunk *nc = plat_pages(sz);
        if (!nc) return NULL;
        nc->next = c;
        nc->size = sz;
        nc->used = (sizeof(Chunk) + 15) & ~(size_t)15;
        t_arena = c = nc;
    }
    void *p = (char *)c + c->used;
    c->used += n;
    return p;
}
static void jfree(void *p) {
    if (!t_in_arena) free(p);                    /* в арене освобождение — разом в json_done */
}

cJSON *json_parse(const char *text, size_t len) {
    static bool hooks_set;
    if (!hooks_set) {
        cJSON_Hooks h = { jmalloc, jfree };
        cJSON_InitHooks(&h);
        hooks_set = true;
    }
    json_done();
    size_t en_n = 0;
    char *en = O.demo ? demo_en_json(text, len, &en_n) : NULL;   /* английский демо-набор (только --demo) */
    t_in_arena = true;
    if (en) {
        cJSON *r = cJSON_ParseWithLength(en, en_n);   /* строки копируются в арену — текст больше не нужен */
        free(en);
        return r;
    }
    return cJSON_ParseWithLength(text, len);
}
void json_done(void) {
    while (t_arena) {
        Chunk *n = t_arena->next;
        plat_pages_free(t_arena, t_arena->size);
        t_arena = n;
    }
    t_in_arena = false;
}
const cJSON *jget(const cJSON *o, const char *key) {
    return o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}
const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = jget(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
u64 id_of(const cJSON *v) {
    if (cJSON_IsString(v) && v->valuestring) return strtoull(v->valuestring, NULL, 10);
    if (cJSON_IsNumber(v) && v->valuedouble > 0) return (u64)v->valuedouble;
    return 0;
}
u64 jid(const cJSON *o, const char *key) { return id_of(jget(o, key)); }
i64 jint(const cJSON *o, const char *key, i64 def) {
    const cJSON *v = jget(o, key);
    if (cJSON_IsNumber(v)) return (i64)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return strtoll(v->valuestring, NULL, 10);
    return def;
}
bool jbool(const cJSON *o, const char *key) { return cJSON_IsTrue(jget(o, key)); }

/* ---------- кодирование ---------- */
void base64_enc(const void *in, size_t n, Buf *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = in;
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        char q[4] = { t[v >> 18 & 63], t[v >> 12 & 63], i + 1 < n ? t[v >> 6 & 63] : '=', i + 2 < n ? t[v & 63] : '=' };
        buf_put(out, q, 4);
    }
}
void url_encode(const char *s, Buf *out) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~')
            buf_put(out, p, 1);
        else
            buf_printf(out, "%%%02X", *p);
    }
}
