/* Самопроверка без терминала и без сети: mincord --selftest
 * Демо-данные идут настоящим путём (JSON -> store -> ui); проверяются очистка вредного текста,
 * права, отсев событий, раскладка (ровная сетка, ASCII-оформление, линии колонок), фокус и клавиши,
 * мышь (щелчки, правый щелчок, колесо, наведение) по таблице кликов, экраны входа, вредный ввод,
 * утечки при долгой навигации, возврат памяти после больших JSON.
 *
 * Снимок кадра (ui_snapshot) разбирается в сетку клеток: символ, цвет текста и фона, атрибуты
 * (жирный, подчёркнутый, инверсия) — так проверяется то, что без цвета не видно (фокус, курсор).
 *
 * [FAIL]    — интерфейс ведёт себя не так, как описано в DESIGN.md (регрессия);
 * [ДЕФЕКТ]  — найденная ошибка в чужом файле, уже описанная в отчёте тестировщика; в FAIL не
 *             считается, чтобы не прятать регрессии за известным. Исправят — строка станет [ ok ]. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "plat.h"
#include "img.h"
#include "login.h"
#include "golive.h"

static int fails, passes, defects;
/* FAIL печатает строку; с ST_DUMP=1 ещё и экран 120x34 в момент проверки (без цветов) — для разбора. */
static void check(bool ok, const char *what) {
    char b[320];
    snprintf(b, sizeof b, "  [%s] %s\n", ok ? " ok " : "FAIL", what);
    term_puts(b);
    if (ok) passes++; else fails++;
    if (!ok && getenv("ST_DUMP")) {
        Buf r = {0}, o = {0};
        ui_snapshot(120, 34, &r);
        for (size_t i = 0; i < r.len; i++) {
            if (r.p[i] == 0x1b) { while (i < r.len && r.p[i] != 'm') i++; continue; }
            buf_put(&o, r.p + i, 1);
        }
        buf_put(&o, "", 1);
        term_puts(o.p);
        buf_free(&r); buf_free(&o);
    }
}
/* Известный дефект в чужом файле: печатается, но в FAIL не идёт. */
__attribute__((unused)) static void defect(bool ok, const char *what) {
    char b[320];
    snprintf(b, sizeof b, "  [%s] %s\n", ok ? " ok " : "ДЕФЕКТ", what);
    term_puts(b);
    if (ok) passes++; else defects++;
}
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    term_puts(b);
}

/* Есть ли в строке то, что могло бы управлять терминалом или подменять порядок текста. */
static bool dirty(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        /* А1: маркер настоящего вложения — только в начале строки и только перед «[файл: » */
        if (*p == (unsigned char)ATT_MARK && (p == (const unsigned char *)s || p[-1] == '\n') &&
            !strncmp((const char *)p + 1, "[файл: ", strlen("[файл: "))) continue;
        if ((*p < 0x20 && *p != '\n') || *p == 0x7F) return true;
        if (p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) return true;                 /* C1 */
        if (p[0] == 0xE2 && p[1] == 0x80 && p[2] >= 0xAA && p[2] <= 0xAE) return true; /* LRE..RLO */
        if (p[0] == 0xE2 && p[1] == 0x81 && p[2] >= 0xA6 && p[2] <= 0xA9) return true; /* изоляты */
    }
    return false;
}

static void key(int code, u32 ch) { Key k = { .code = code, .ch = ch }; ui_feed_key(&k); }
static void key_ctrl(u32 ch) { Key k = { .code = K_CHAR, .ch = ch, .ctrl = true }; ui_feed_key(&k); }
static void key_alt(u32 ch) { Key k = { .code = K_CHAR, .ch = ch, .alt = true }; ui_feed_key(&k); }
static void type(const char *s) { while (*s) { u32 c; s += utf8_dec(s, &c); key(K_CHAR, c); } }
static void mouse_at(int b, int x, int y) { Key k = { .code = K_MOUSE, .x = x, .y = y, .button = b }; ui_feed_key(&k); }
static void hover_off(void) { mouse_at(0, -1, -1); }

/* ======================= снимок: сетка клеток ======================= */
#define SAT_B 1
#define SAT_U 2
#define SAT_I 4
#define SCR_MAXH 200
typedef struct { u32 cp, fg, bg; u8 at; } SC;
typedef struct {
    int w, h, n;
    SC *c;                                       /* w*h; cp = 0 — правая половина широкого символа */
    Buf txt;                                     /* строки без SGR, через '\0' */
    char *ln[SCR_MAXH];
    int lw[SCR_MAXH];                            /* ширина строки в клетках */
    char err[240];                               /* первое нарушение потока */
} Scr;

/* Символы, которых не должно быть в выводе ни при каких данных. */
static bool forbidden_cp(u32 c) {
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F)) return true;
    if (c == 0x200E || c == 0x200F || c == 0x061C || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)) return true;
    if (c == 0x2028 || c == 0x2029 || c == 0xFEFF || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return true;
    return false;
}
static void scr_err(Scr *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void scr_err(Scr *s, const char *fmt, ...) {
    if (s->err[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof s->err, fmt, ap);
    va_end(ap);
}
static void scr_take(Scr *s, int w, int h) {
    memset(s, 0, sizeof *s);
    s->w = w;
    s->h = h;
    s->c = xcalloc((size_t)w * (size_t)h, sizeof(SC));
    Buf raw = {0};
    ui_snapshot(w, h, &raw);
    const char *p = raw.p ? raw.p : "", *end = p + raw.len;
    int y = 0, x = 0;
    u8 at = 0;
    u32 fg = 0, bg = 0;
    size_t lstart = 0;
    while (p < end) {
        unsigned char b = (unsigned char)*p;
        if (b == '\n') {
            buf_put(&s->txt, "", 1);
            if (y < SCR_MAXH) { s->ln[y] = (char *)(uintptr_t)lstart; s->lw[y] = x; }
            lstart = s->txt.len;
            y++; x = 0; p++;
            continue;
        }
        if (b == 0x1b) {
            if (p + 1 >= end || p[1] != '[') { scr_err(s, "строка %d: ESC без '[' (0x%02x)", y, p + 1 < end ? (unsigned char)p[1] : 0); p++; continue; }
            const char *q = p + 2;
            int par[24], np = 0, cur = 0;
            bool have = false;
            while (q < end && ((*q >= '0' && *q <= '9') || *q == ';')) {
                if (*q == ';') { if (np < 24) par[np++] = cur; cur = 0; have = false; }
                else { cur = cur * 10 + (*q - '0'); have = true; }
                q++;
            }
            if (np < 24 && (have || np)) par[np++] = cur;
            if (np == 0) par[np++] = 0;                 /* ESC[m без параметров = ESC[0m: сброс */
            if (q >= end || *q != 'm') { scr_err(s, "строка %d: последовательность ESC[...%c не SGR", y, q < end ? *q : '?'); p = q < end ? q + 1 : end; continue; }
            for (int i = 0; i < np; i++) {
                int v = par[i];
                if (v == 0) { at = 0; fg = bg = 0; }
                else if (v == 1) at |= SAT_B;
                else if (v == 4) at |= SAT_U;
                else if (v == 7) at |= SAT_I;
                else if ((v == 38 || v == 48) && i + 4 < np && par[i + 1] == 2) {
                    u32 c = (u32)par[i + 2] << 16 | (u32)par[i + 3] << 8 | (u32)par[i + 4];
                    if (v == 38) fg = c; else bg = c;
                    i += 4;
                } else scr_err(s, "строка %d: неожиданный SGR %d", y, v);
            }
            p = q + 1;
            continue;
        }
        u32 c;
        int k = utf8_dec(p, &c);
        if (k < 1) k = 1;
        if (c == 0xFFFD && !(k == 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBF && (unsigned char)p[2] == 0xBD))
            scr_err(s, "строка %d, клетка %d: битый UTF-8 в выводе", y, x);
        if (forbidden_cp(c)) scr_err(s, "строка %d, клетка %d: запрещённый символ U+%04X в выводе", y, x, (unsigned)c);
        int cw = cp_width(c);
        if (cw < 1) { scr_err(s, "строка %d, клетка %d: символ нулевой ширины U+%04X в выводе", y, x, (unsigned)c); cw = 0; }
        if (cw && y < h && x + cw <= w) {
            SC *e = &s->c[(size_t)y * (size_t)w + (size_t)x];
            *e = (SC){ c, fg, bg, at };
            if (cw == 2) s->c[(size_t)y * (size_t)w + (size_t)x + 1] = (SC){ 0, fg, bg, at };
        }
        buf_put(&s->txt, p, (size_t)k);
        x += cw;
        p += k;
    }
    if (x || lstart < s->txt.len) {              /* последняя строка без '\n' */
        buf_put(&s->txt, "", 1);
        if (y < SCR_MAXH) { s->ln[y] = (char *)(uintptr_t)lstart; s->lw[y] = x; }
        y++;
    }
    s->n = y;
    buf_put(&s->txt, "", 1);
    for (int i = 0; i < s->n && i < SCR_MAXH; i++) s->ln[i] = s->txt.p + (uintptr_t)s->ln[i];
    buf_free(&raw);
}
static void scr_free(Scr *s) { buf_free(&s->txt); free(s->c); s->c = NULL; }
static const char *scr_line(const Scr *s, int y) { return y >= 0 && y < s->n && y < SCR_MAXH ? s->ln[y] : ""; }
static const SC *scr_cell(const Scr *s, int x, int y) {
    static const SC none = { ' ', 0, 0, 0 };
    return x >= 0 && y >= 0 && x < s->w && y < s->h ? &s->c[(size_t)y * (size_t)s->w + (size_t)x] : &none;
}
static bool scr_has(const Scr *s, const char *needle) {
    for (int y = 0; y < s->n && y < SCR_MAXH; y++) if (strstr(s->ln[y], needle)) return true;
    return false;
}
/* Клетка начала подписи: сверху вниз (rev = false) или снизу вверх. */
static bool scr_find_ex(const Scr *s, const char *needle, int *x, int *y, bool rev) {
    int n = s->n < SCR_MAXH ? s->n : SCR_MAXH;
    for (int k = 0; k < n; k++) {
        int i = rev ? n - 1 - k : k;
        char *p = strstr(s->ln[i], needle);
        if (!p) continue;
        char save = *p;
        *p = 0;
        *x = str_width(s->ln[i]);
        *p = save;
        *y = i;
        return true;
    }
    return false;
}
static bool scr_find(const Scr *s, const char *needle, int *x, int *y) { return scr_find_ex(s, needle, x, y, false); }
static void scr_print(const Scr *s) {
    for (int y = 0; y < s->n && y < SCR_MAXH; y++) { term_puts("    |"); term_puts(s->ln[y]); term_puts("\n"); }
}
/* Есть ли атрибут в прямоугольнике клеток. */
static bool scr_attr(const Scr *s, int x0, int y0, int x1, int y1, u8 a) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (scr_cell(s, x, y)->at & a) return true;
    return false;
}
static bool scr_bg_row(const Scr *s, int x0, int x1, int y, u32 bg) {
    for (int x = x0; x <= x1; x++) if (scr_cell(s, x, y)->bg == bg) return true;
    return false;
}

/* Сетка: строк ровно H, каждая ровно W клеток, последняя W-1; в потоке только SGR и печатные символы. */
static bool scr_grid_ok(const Scr *s, const char *ctx) {
    bool ok = s->n == s->h;
    if (!ok) say("    %s %dx%d: строк %d вместо %d\n", ctx, s->w, s->h, s->n, s->h);
    for (int y = 0; y < s->n && y < SCR_MAXH; y++) {
        int want = y == s->h - 1 ? s->w - 1 : s->w;
        if (s->lw[y] != want) {
            if (ok) say("    %s %dx%d, строка %d: ширина %d вместо %d: %s\n", ctx, s->w, s->h, y, s->lw[y], want, s->ln[y]);
            ok = false;
        }
    }
    if (s->err[0]) { say("    %s %dx%d: %s\n", ctx, s->w, s->h, s->err); ok = false; }
    return ok;
}

/* ---- оформление: ASCII 0x20-0x7E и кириллица; прочее допустимо, только если это пришло в данных ---- */
static u8 data_bmp[0x10000 / 8];
static u32 data_astral[512];
static int ndata_astral;
static void data_add(const char *s) {
    while (s && *s) {
        u32 c;
        s += utf8_dec(s, &c);
        if (c < 0x10000) data_bmp[c >> 3] |= (u8)(1u << (c & 7));
        else {
            bool have = false;
            for (int i = 0; i < ndata_astral; i++) if (data_astral[i] == c) have = true;
            if (!have && ndata_astral < (int)(sizeof data_astral / sizeof *data_astral)) data_astral[ndata_astral++] = c;
        }
    }
}
static bool data_has(u32 c) {
    if (c < 0x10000) return data_bmp[c >> 3] >> (c & 7) & 1;
    for (int i = 0; i < ndata_astral; i++) if (data_astral[i] == c) return true;
    return false;
}
static void data_build(void) {
    for (int i = 0; i < S.nusers; i++) data_add(S.users[i].name);
    for (int i = 0; i < S.nguilds; i++) data_add(S.guilds[i].name);
    for (int i = 0; i < S.nchans; i++) data_add(S.chans[i].name);
    for (int i = 0; i < S.nmsgs; i++) {
        const Message *m = &S.msgs[i];
        data_add(m->author); data_add(m->content); data_add(m->ref_author); data_add(m->ref_text);
        for (int r = 0; r < m->nreacts; r++) data_add(m->reacts[r].emoji);
    }
    data_add(S.me_name);
    data_add(S.typing_name);
}
static bool deco_cp_ok(u32 c) { return (c >= 0x20 && c <= 0x7E) || (c >= 0x400 && c <= 0x45F); }
/* Строка-образец без данных: проверка самой проверки. */
static bool deco_line_ok(const char *s, u32 *bad) {
    while (s && *s) {
        u32 c;
        s += utf8_dec(s, &c);
        if (!deco_cp_ok(c)) { if (bad) *bad = c; return false; }
    }
    return true;
}
static int deco_quotes;                          /* «» в подписях интерфейса (дефект, не FAIL) */
static bool scr_deco_ok(const Scr *s, const char *ctx) {
    data_build();
    bool ok = true;
    for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
        u32 c = scr_cell(s, x, y)->cp;
        if (!c || deco_cp_ok(c) || data_has(c)) continue;
        if (c == 0xAB || c == 0xBB) { deco_quotes++; continue; }
        if (ok) say("    %s %dx%d, строка %d, клетка %d: символ U+%04X в оформлении: %s\n", ctx, s->w, s->h, y, x, (unsigned)c, scr_line(s, y));
        ok = false;
    }
    return ok;
}

/* Раскладка главного экрана (DESIGN.md раздел 4): линия рельса в столбце 9 и линия колонки каналов —
 * во всех строках; у текстового канала — рамка поля ввода и (от 100 клеток) линия колонки участников. */
static int side_w(int W) { int sw = W / 4 + 3; return sw < 20 ? 20 : sw > 34 ? 34 : sw; }
static bool scr_layout_ok(const Scr *s, bool text_ch, bool members, const char *ctx) {
    int W = s->w, H = s->h, sw = side_w(W), cx = 10 + sw, cw = W - cx;
    bool ok = true;
    for (int y = 0; y < H; y++) {
        int lim = y == H - 1 ? W - 1 : W;
        if (scr_cell(s, 9, y)->cp != '|') { if (ok) say("    %s %dx%d: строка %d: нет линии рельса в столбце 9: %s\n", ctx, W, H, y, scr_line(s, y)); ok = false; }
        if (cx - 1 < lim && scr_cell(s, cx - 1, y)->cp != '|') { if (ok) say("    %s %dx%d: строка %d: нет линии колонки каналов в столбце %d: %s\n", ctx, W, H, y, cx - 1, scr_line(s, y)); ok = false; }
    }
    if (text_ch) {
        int memw = members ? (W * 18 / 100 > 28 ? 28 : W * 18 / 100) : 0, fw = cw - memw;
        if (memw) for (int y = 1; y < H; y++)
            if (scr_cell(s, cx + fw, y)->cp != '|') { if (ok) say("    %s %dx%d: строка %d: нет линии колонки участников в столбце %d\n", ctx, W, H, y, cx + fw); ok = false; }
        /* Рамка поля ввода: низ — строка H-1 «+..+», выше — «|..|» (поле растёт до 10 строк текста),
         * верх «+..+» — не выше H-3-ex, ex = min(9, H-12). Ищем верх снизу вверх. */
        int l = cx + 1, r = cx + fw - 2, ex = H - 12 < 9 ? H - 12 : 9;
        if (ex < 0) ex = 0;
        int top = -1;
        for (int y = H - 1; y >= H - 3 - ex && y >= 0 && top < 0; y--) {
            u32 a = scr_cell(s, l, y)->cp, b = scr_cell(s, r, y)->cp;
            if (r >= (y == H - 1 ? W - 1 : W)) b = a == '+' || a == '|' ? a : b;
            bool corner = a == '+' && b == '+', side = a == '|' && b == '|';
            if (y == H - 1 ? corner : y <= H - 3 && corner) { if (y != H - 1) top = y; continue; }
            if (y != H - 1 && side) continue;
            if (ok) say("    %s %dx%d: рамка поля ввода, строка %d: '%c'..'%c': %s\n", ctx, W, H, y,
                        a < 0x7F && a >= 0x20 ? (char)a : '?', b < 0x7F && b >= 0x20 ? (char)b : '?', scr_line(s, y));
            ok = false;
            break;
        }
        if (ok && top < 0) { say("    %s %dx%d: рамка поля ввода: верх «+..+» не найден в строках %d..%d\n", ctx, W, H, H - 3 - ex, H - 3); ok = false; }
    }
    return ok;
}
/* Всё сразу для одного снимка: сетка + оформление (+ раскладка, если layout). */
__attribute__((unused)) static bool scr_all_ok(const Scr *s, const char *ctx) { return scr_grid_ok(s, ctx) & scr_deco_ok(s, ctx); }

/* ======================= мышь по подписям ======================= */
static int CW = 120, CH = 34;
static Scr cur;                                  /* последний снимок для кликов */
static void cur_take(void) { scr_free(&cur); scr_take(&cur, CW, CH); }
static bool on_screen(const char *s) { cur_take(); return scr_has(&cur, s); }
/* Щелчок по первой клетке подписи (+dx); rev — искать снизу вверх. Снимок берётся заново: таблица кликов — от него. */
static bool click_ex(const char *label, int button, int dx, bool rev) {
    cur_take();
    int x, y;
    if (!scr_find_ex(&cur, label, &x, &y, rev)) { say("    не найдено на экране %dx%d: %s\n", CW, CH, label); return false; }
    mouse_at(button, x + dx, y);
    return true;
}
static bool click(const char *label) { return click_ex(label, 1, 0, false); }
static bool rclick(const char *label) { return click_ex(label, 3, 0, false); }
static bool click_low(const char *label, int dx) { return click_ex(label, 1, dx, true); }
static bool hover(const char *label, int dx, bool rev) { return click_ex(label, 0, dx, rev); }
static void click_xy(int x, int y) { cur_take(); mouse_at(1, x, y); }

/* ======================= поиск секрета в памяти процесса ======================= */
/* Сколько раз needle встречается в записываемой памяти процесса (кроме самого needle). */
__attribute__((unused)) static int mem_count(const char *needle, size_t n) {
    int hits = 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi;
    for (char *a = NULL; VirtualQuery(a, &mi, sizeof mi) == sizeof mi; a = (char *)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE) continue;
        if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        if (!(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) continue;
        const char *b = mi.BaseAddress, *e = b + mi.RegionSize;
        for (const char *p = b; p + n <= e; p++) {
            p = memchr(p, needle[0], (size_t)(e - p));
            if (!p || p + n > e) break;
            if (p != needle && !memcmp(p, needle, n)) hits++;
        }
        if ((char *)mi.BaseAddress + mi.RegionSize < (char *)mi.BaseAddress) break;
    }
#else
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char ln[512];
    while (fgets(ln, sizeof ln, f)) {
        unsigned long lo, hi;
        char perm[8] = "";
        if (sscanf(ln, "%lx-%lx %7s", &lo, &hi, perm) != 3 || perm[0] != 'r' || perm[1] != 'w') continue;
        if (strstr(ln, "[vvar") || strstr(ln, "[vsyscall")) continue;
        const char *b = (const char *)lo, *e = (const char *)hi;
        for (const char *p = b; p + n <= e; p++) {
            p = memchr(p, needle[0], (size_t)(e - p));
            if (!p || p + n > e) break;
            if (p != needle && !memcmp(p, needle, n)) hits++;
        }
    }
    fclose(f);
#endif
    return hits;
}

/* ======================= разделы проверки ======================= */
static void t_sizes(void) {
    static const char *const bad_deco[] = {
        "+-[ серверы ]\xE2\x94\x80+",           /* ─ U+2500 */
        "+-[ ввод ]---\xE2\x80\xA6+",            /* … */
        " )) канал \xE2\x80\x94 ошибка",          /* — */
        " )) канал \xE2\x80\x93 ошибка",          /* – */
        " \xE2\x84\x96 1",                        /* № */
        "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90",   /* ┌─┐ */
        "+=[ \xE2\x86\x90 назад ]=+",             /* ← */
    };
    bool catches = true;
    for (size_t i = 0; i < sizeof bad_deco / sizeof *bad_deco; i++) if (deco_line_ok(bad_deco[i], NULL)) catches = false;
    catches = catches && deco_line_ok("+-----+ | /Ал\\ <Алиса> 08.11 16:01 (Ёж) [#] GIF [/] :) ( Поиск ) .:| ~v~ <)) ((o", NULL);
    check(catches, "оформление: проверка ловит ─ … — – № ┌┐ ← и пропускает ASCII с кириллицей");

    static const int sizes[][2] = { { 50, 12 }, { 51, 13 }, { 64, 16 }, { 80, 24 }, { 99, 30 }, { 100, 30 }, { 120, 34 },
                                    { 200, 60 }, { 333, 100 }, { 49, 12 }, { 50, 11 }, { 49, 11 }, { 30, 4 }, { 10, 3 }, { 1, 1 } };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int w = sizes[i][0], h = sizes[i][1];
        Scr s;
        scr_take(&s, w, h);
        bool small = w < 50 || h < 12;
        char what[200];
        bool ok = scr_grid_ok(&s, "снимок") && scr_deco_ok(&s, "снимок");
        if (small) {
            char want[64];
            snprintf(want, sizeof want, "сейчас %dx%d", w, h);
            int n0 = h == 1 ? w - 1 : w;             /* последняя клетка экрана не пишется (прокрутка) */
            if (n0 > 7) n0 = 7;
            ok = ok && (h < 1 || n0 <= 0 || !strncmp(scr_line(&s, 0), "Astra DS", (size_t)n0)) && (h < 2 || strstr(scr_line(&s, 1), want) || w < 30)
                 && (h < 3 || w < 50 || strstr(scr_line(&s, 2), "Ctrl+Q - выход"));
            snprintf(what, sizeof what, "снимок %dx%d: ровная сетка, «окно слишком маленькое» с размером и Ctrl+Q", w, h);
        } else {
            ok = ok && scr_layout_ok(&s, true, w >= 100, "снимок");
            snprintf(what, sizeof what, "снимок %dx%d: ровная сетка, ASCII-оформление, линии колонок, рамка ввода%s", w, h,
                     w >= 100 ? ", колонка участников" : "");
        }
        if (!ok) scr_print(&s);
        check(ok, what);
        scr_free(&s);
    }
}

/* Фокус (DESIGN.md раздел 13): подчёркнута подпись выбранного значка / шапка сервера / имя канала,
 * у сообщения фон C_SEL, в поле ввода — курсор-инверсия; подсказка внизу — по колонке. */
static void t_focus(void) {
    const int W = 120, H = 34, sw = side_w(W), cx = 10 + sw;
    static const char *const hint[4] = { "Up/Down сервер", "Enter открыть/свернуть", "r ответ", "Enter отправить" };
    static const char *const name[4] = { "серверы", "каналы", "сообщения", "ввод" };
    bool all = true;
    for (int step = 0; step < 4; step++) {
        key(K_TAB, 0);                           /* ввод -> серверы -> каналы -> сообщения -> ввод */
        Scr s;
        scr_take(&s, W, H);
        bool rail = scr_attr(&s, 1, 0, 6, H - 1, SAT_U);
        bool head = scr_attr(&s, 10, 0, cx - 2, 0, SAT_U);
        bool title = scr_attr(&s, cx, 0, W - 1, 0, SAT_U);
        bool selmsg = false;
        for (int y = 1; y < H - 3; y++) if (scr_bg_row(&s, cx + 1, cx + 40, y, 0x35383cu)) selmsg = true;
        bool caret = scr_attr(&s, cx, H - 2, W - 1, H - 2, SAT_I);
        bool want[4][5] = { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 1, 0 }, { 0, 0, 0, 0, 1 } };
        bool got[5] = { rail, head, title, selmsg, caret };
        bool ok = scr_grid_ok(&s, "фокус") && strstr(scr_line(&s, H - 1), hint[step]) != NULL;
        for (int i = 0; i < 5; i++) if (got[i] != want[step][i]) ok = false;
        if (!ok) {
            say("    Tab %d (%s): подчёркнут значок %d, шапка %d, канал %d; выбрано сообщение %d; курсор %d; подсказка: %s\n",
                step + 1, name[step], rail, head, title, selmsg, caret, scr_line(&s, H - 1));
            all = false;
        }
        scr_free(&s);
    }
    check(all, "Tab: фокус виден (подчёркнут значок / шапка сервера / имя канала, сообщение на C_SEL, курсор в поле) и подсказка по колонке");
    /* Shift+Tab — обратно */
    key(K_BTAB, 0);
    Scr s;
    scr_take(&s, W, H);
    bool ok = strstr(scr_line(&s, H - 1), hint[2]) != NULL;
    scr_free(&s);
    key(K_TAB, 0);
    check(ok, "Shift+Tab: из поля ввода назад к сообщениям");
}

static void t_scroll(void) {
    key(K_BTAB, 0);                              /* ввод -> сообщения (выбрано последнее) */
    const int sizes[][2] = { { 120, 20 }, { 70, 12 } };
    for (int i = 0; i < 2; i++) {
        int W = sizes[i][0], H = sizes[i][1];
        key(K_END, 0);
        key(K_PGUP, 0);
        Scr s;
        scr_take(&s, W, H);
        bool ok = scr_has(&s, "ниже ещё") && scr_grid_ok(&s, "PgUp") && scr_deco_ok(&s, "PgUp") && scr_layout_ok(&s, true, W >= 100, "PgUp");
        ok = ok && scr_has(&s, "PgDn");
        if (W >= 100) ok = ok && scr_has(&s, "Перейти к новым");
        if (!ok) scr_print(&s);
        scr_free(&s);
        char what[160];
        snprintf(what, sizeof what, "сообщения + PgUp (%dx%d): плашка «...ниже ещё N строк: PgDn%s», сетка цела", W, H, W >= 100 ? " ... Перейти к новым" : "");
        check(ok, what);
        for (int q = 0; q < 20; q++) {           /* страница — высота ленты: на 70x12 нужно несколько PgDn */
            scr_take(&s, W, H);
            ok = !scr_has(&s, "ниже ещё");
            scr_free(&s);
            if (ok) break;
            key(K_PGDN, 0);
        }
        scr_take(&s, W, H);
        ok = !scr_has(&s, "ниже ещё");
        scr_free(&s);
        snprintf(what, sizeof what, "сообщения + PgDn (%dx%d): вернулись вниз, плашка пропала", W, H);
        check(ok, what);
    }
    /* выбор Up по сообщениям держит выбранное видимым */
    bool vis = true;
    for (int i = 0; i < S.nmsgs + 2; i++) {
        key(K_UP, 0);
        Scr s;
        scr_take(&s, 70, 12);
        bool sel = false;
        for (int y = 1; y < 8; y++) if (scr_bg_row(&s, 31, 60, y, 0x35383cu)) sel = true;
        if (!sel) vis = false;
        scr_free(&s);
    }
    check(vis, "сообщения + Up до самого верха (70x12): выбранное сообщение всегда на экране");
    key(K_END, 0);
    Scr s;
    scr_take(&s, 70, 12);
    bool ok = !scr_has(&s, "ниже ещё");
    scr_free(&s);
    check(ok, "сообщения + End: вниз, «ниже ещё» нет");
    key(K_ESC, 0);                               /* сообщения -> ввод */
}

static void t_help(void) {
    key(K_F1, 0);
    Scr s;
    scr_take(&s, 110, 30);
    bool ok = scr_grid_ok(&s, "справка") && scr_deco_ok(&s, "справка") && strstr(scr_line(&s, 0), "справка:")
              && strstr(scr_line(&s, 0), "[x]") && scr_has(&s, "МЫШЬ") && scr_has(&s, "ПАНЕЛИ") && scr_has(&s, "СООБЩЕНИЯ")
              && scr_has(&s, "ГОЛОС И ЗВОНКИ") && scr_has(&s, "Ctrl+Q");
    if (!ok) scr_print(&s);
    scr_free(&s);
    check(ok, "справка F1 110x30: «справка:» и [x] вверху, разделы МЫШЬ/ПАНЕЛИ/СООБЩЕНИЯ/ГОЛОС, сетка и ASCII");
    scr_take(&s, 60, 20);
    ok = scr_grid_ok(&s, "справка") && scr_deco_ok(&s, "справка") && strstr(scr_line(&s, 0), "справка")
         && scr_has(&s, "ниже ещё: Down / PgDn");
    if (!ok) scr_print(&s);
    scr_free(&s);
    check(ok, "справка F1 60x20: одна колонка, внизу плашка «ниже ещё: Down / PgDn»");
    for (int i = 0; i < 200; i++) key(K_DOWN, 0);
    scr_take(&s, 60, 20);
    ok = scr_grid_ok(&s, "справка") && !scr_has(&s, "ниже ещё") && scr_has(&s, "АНОНИМНОСТЬ");
    if (!ok) scr_print(&s);
    scr_free(&s);
    check(ok, "справка 60x20: Down до конца — «ниже ещё» пропало, последний раздел виден");
    key(K_PGUP, 0); key(K_PGUP, 0); key(K_PGUP, 0); key(K_PGUP, 0); key(K_PGUP, 0);
    scr_take(&s, 60, 20);
    ok = scr_has(&s, "МЫШЬ");
    scr_free(&s);
    check(ok, "справка: PgUp возвращает к началу");
    key(K_ESC, 0);
    scr_take(&s, 110, 30);
    ok = !scr_has(&s, "справка:") && strstr(scr_line(&s, 29), "Enter отправить");
    scr_free(&s);
    check(ok, "справка: Esc закрывает, фокус снова в поле ввода");
    key(K_F1, 0); key(K_F1, 0);
    scr_take(&s, 110, 30);
    ok = !scr_has(&s, "справка:");
    scr_free(&s);
    check(ok, "справка: F1 повторно закрывает");
}

static void t_typing_call(void) {
    snprintf(S.typing_name, sizeof S.typing_name, "Алиса");
    S.typing_until = now_ms() + 60000;
    S.call.ringing = true;
    S.call.channel_id = 4000;                    /* ЛС с Алисой */
    S.call.caller = 1001;
    Scr s;
    scr_take(&s, 160, 40);
    const char *st = scr_line(&s, 39);
    bool ok = strstr(st, "~ Алиса печатает...") && strstr(st, "ЗВОНОК: Алиса") && strstr(st, "F2 принять") && strstr(st, "F3 отклонить")
              && scr_has(&s, "Входящий звонок") && scr_has(&s, "звонит: Алиса") && scr_has(&s, "Отклонить (F3)") && scr_has(&s, "Принять (F2)")
              && scr_grid_ok(&s, "звонок") && scr_deco_ok(&s, "звонок") && scr_layout_ok(&s, true, true, "звонок");
    if (!ok) scr_print(&s);
    scr_free(&s);
    check(ok, "160x40: внизу рамки ввода «ЗВОНОК: Алиса F2 принять F3 отклонить» и «~ Алиса печатает...», окно «Входящий звонок»");
    scr_take(&s, 100, 30);
    ok = strstr(scr_line(&s, 29), "ЗВОНОК: Алиса") && scr_grid_ok(&s, "звонок") && scr_layout_ok(&s, true, true, "звонок");
    if (!ok) scr_print(&s);
    scr_free(&s);
    check(ok, "100x30: звонок в нижней строке виден, сетка цела");
    scr_take(&s, 50, 12);
    ok = scr_grid_ok(&s, "звонок") && scr_deco_ok(&s, "звонок") && strstr(scr_line(&s, 11), "ЗВОНОК");
    if (!ok) scr_print(&s);
    scr_free(&s);
    check(ok, "50x12: сетка цела, «ЗВОНОК» в нижней строке виден");
    S.call.ringing = false;
    S.call.channel_id = S.call.caller = 0;
    S.typing_until = 0;
    S.typing_name[0] = 0;
}

static void t_before_ready(void) {
    u64 keep_ch = S.open_ch;
    S.ready = false;
    S.open_ch = 0;
    O.demo = false;
    Scr s;
    scr_take(&s, 100, 30);
    bool ok = scr_grid_ok(&s, "до READY") && scr_deco_ok(&s, "до READY") && scr_has(&s, "Astra DS") && scr_has(&s, "Ctrl+Q - выход")
              && (scr_has(&s, "подключение...") || gw_status_text()[0]);
    if (!ok) scr_print(&s);
    scr_free(&s);
    O.demo = true;
    S.ready = true;
    S.open_ch = keep_ch;
    check(ok, "до READY: карточка «Astra DS / подключение... / Ctrl+Q - выход», сетка и ASCII");
}

/* Клавиатура (DESIGN.md раздел 13) — по сообщениям, F-клавиши, Home/End, русская раскладка. */
static void t_keys(void) {
    const int W = 120, H = 34;
    Scr s;
    /* сообщения: r / e / d / + / 1-9, к / у / в */
    key(K_BTAB, 0);                              /* ввод -> сообщения */
    key(K_END, 0);                               /* последнее — своё (отправлено выше) */
    key(K_CHAR, 'r');
    ui_notice_clear();
    scr_take(&s, W, H);
    bool ok = scr_has(&s, "Ответ пользователю Вы") && scr_has(&s, "(x)") && strstr(scr_line(&s, H - 1), "Esc отмена ответа");
    scr_free(&s);
    key(K_ESC, 0);
    scr_take(&s, W, H);
    ok = ok && !scr_has(&s, "Ответ пользователю");
    scr_free(&s);
    check(ok, "r: режим ответа («Ответ пользователю Вы», (x), подсказка Esc), Esc отменяет");
    key(K_BTAB, 0);
    key(K_END, 0);
    key(K_CHAR, 'e');
    scr_take(&s, W, H);
    ok = scr_has(&s, "Изменение сообщения") && scr_has(&s, "ünïcödé");
    scr_free(&s);
    key(K_ESC, 0);
    scr_take(&s, W, H);
    ok = ok && !scr_has(&s, "Изменение сообщения") && scr_has(&s, "Написать #general");
    scr_free(&s);
    check(ok, "e: правка своего (текст в поле), Esc отменяет и очищает поле");
    key(K_BTAB, 0);
    key(K_END, 0);
    key(K_UP, 0);                                /* сообщение Боба */
    key(K_CHAR, 'e');
    scr_take(&s, W, H);
    ok = scr_has(&s, "изменять можно только свои") && !scr_has(&s, "Изменение сообщения");
    scr_free(&s);
    check(ok, "e на чужом: уведомление «изменять можно только свои», режим не включается");
    key(K_CHAR, 'd');
    scr_take(&s, W, H);
    ok = scr_has(&s, "удалять можно только свои");
    scr_free(&s);
    check(ok, "d на чужом: «удалять можно только свои»");
    key(K_END, 0);
    key(K_CHAR, 'd');
    scr_take(&s, W, H);
    ok = scr_has(&s, "Удалить сообщение") && scr_has(&s, "Вы точно хотите удалить") && scr_has(&s, "Удалить (y)") && scr_has(&s, "Отмена")
         && scr_grid_ok(&s, "удаление") && scr_deco_ok(&s, "удаление");
    scr_free(&s);
    key(K_CHAR, 'n');
    scr_take(&s, W, H);
    ok = ok && !scr_has(&s, "Вы точно хотите");
    scr_free(&s);
    check(ok, "d на своём: окно «Удалить сообщение» с (Отмена) (Удалить (y)); другая клавиша — отмена");
    key(K_CHAR, 0x432);                          /* в — d в русской раскладке */
    key(K_CHAR, 'y');
    scr_take(&s, W, H);
    ok = scr_has(&s, "в демо-режиме удаление не отправляется");
    scr_free(&s);
    check(ok, "в (рус. d) + y: удаление подтверждено (в демо — уведомление, запрос не уходит)");
    key(K_CHAR, '+');
    scr_take(&s, W, H);
    ok = scr_has(&s, "Реакция: эмодзи") && scr_has(&s, "эмодзи, :код: или номер");
    scr_free(&s);
    type(":fire:");
    key(K_ENTER, 0);
    ui_notice_clear();                           /* уведомление прошлого шага закрыло бы подсказку */
    scr_take(&s, W, H);
    ok = ok && !scr_has(&s, "Реакция: эмодзи") && strstr(scr_line(&s, H - 1), "r ответ");
    scr_free(&s);
    check(ok, "+: режим реакции, :fire: Enter — реакция, фокус снова на сообщениях");
    key(K_CHAR, '+');
    type(":nosuchcode:");
    key(K_ENTER, 0);
    ok = on_screen("неизвестный код :nosuchcode:");
    check(ok, "+ :nosuchcode: — уведомление «неизвестный код»");
    key(K_CHAR, '9');
    ok = on_screen("нет реакции с номером 9");
    check(ok, "9 без девятой реакции — «нет реакции с номером 9»");
    key(K_CHAR, 0x43A);                          /* к — r */
    ok = on_screen("Ответ пользователю");
    key(K_ESC, 0);
    check(ok, "к (рус. r): ответ");
    /* поле ввода: Up в пустом — правка последнего своего, Ctrl+U, Ctrl+W, Shift+Enter не нужен — '\n' */
    /* ответ переводит фокус в поле ввода, Esc выше отменил ответ — фокус остался в поле
     * (ещё один Esc увёл бы его в каналы: ui.c editor_key) */
    ui_notice_clear();
    scr_take(&s, W, H);
    bool in_focus = strstr(scr_line(&s, H - 1), "Enter отправить") != NULL;
    scr_free(&s);
    key(K_UP, 0);
    scr_take(&s, W, H);
    ok = in_focus && scr_has(&s, "Изменение сообщения") && scr_has(&s, "ünïcödé");
    scr_free(&s);
    type(" (правка)");
    key(K_ESC, 0);
    check(ok, "поле ввода: Up в пустой строке открывает последнее своё на правку, Esc отменяет");
    type("одно два три");
    key_ctrl('w');
    ok = on_screen("одно два ") && !scr_has(&cur, "три");
    key_ctrl('u');
    ok = ok && !on_screen("одно два");
    check(ok, "поле ввода: Ctrl+W стирает слово, Ctrl+U — строку");
    type("а");
    key(K_LEFT, 0); key(K_HOME, 0); type("б"); key(K_END, 0); type("в"); key(K_LEFT, 0); key(K_BS, 0); key(K_HOME, 0); key(K_DEL, 0);
    ok = on_screen("+ + в") || on_screen(" в ");
    key_ctrl('u');
    check(ok, "поле ввода: Left/Home/End/Backspace/Delete правят по символам");
    {
        /* 2000 символов — предел; 2001-й не принимается */
        for (int i = 0; i < 2005; i++) key(K_CHAR, 'x');
        ok = on_screen("не больше 2000 символов");
        Scr t;
        scr_take(&t, 64, 16);
        ok = ok && scr_grid_ok(&t, "2000 x") && scr_layout_ok(&t, true, false, "2000 x");
        scr_free(&t);
        key_ctrl('u');
        check(ok, "поле ввода: 2000 символов — предел с уведомлением, длинная строка не ломает рамку (64x16)");
    }
    /* F-клавиши */
    VoiceStatus vs;
    voice_status(&vs);
    bool m0 = vs.self_mute, d0 = vs.self_deaf;
    key(K_F5, 0);
    voice_status(&vs);
    ok = vs.self_mute != m0;
    key(K_F5, 0);
    key(K_F6, 0);
    voice_status(&vs);
    ok = ok && vs.self_deaf != d0;
    key(K_F6, 0);
    check(ok, "F5 / F6: микрофон и звук переключаются");
    int db0 = voice_vad_db();
    key(K_F9, 0);
    ok = voice_vad_db() == db0 - 3 && on_screen("порог голоса:");
    key(K_F10, 0);
    ok = ok && voice_vad_db() == db0;
    check(ok, "F9 / F10: порог -3 / +3 дБ с уведомлением");
    key(K_F8, 0);
    ok = on_screen("рация включается флагом --ptt");
    check(ok, "F8 без --ptt: подсказка «рация включается флагом --ptt»");
    VoiceFx fx0 = voicefx_get();
    key(K_F11, 0);
    ok = voicefx_get() != fx0 && on_screen("смена голоса:");
    key_alt('v');
    key_alt(0x43C);                              /* Alt+м */
    ok = ok && voicefx_get() == fx0;
    check(ok, "F11, Alt+V, Alt+м: смена голоса по кругу выкл -> ниже -> выше -> выкл, уведомление");
    key(K_F4, 0);
    ok = on_screen("звонок - из личной переписки");
    check(ok, "F4 на сервере: «звонок - из личной переписки»");
    key(K_F2, 0); key(K_F3, 0); key(K_F7, 0);   /* без звонка — ничего */
    voice_status(&vs);
    check(vs.phase == VS_IDLE, "F2 / F3 / F7 без звонка ничего не делают");
    /* серверы: Home / End / Up / Down */
    key(K_TAB, 0);                               /* ввод -> серверы */
    key(K_END, 0);
    scr_take(&s, W, H);
    ok = !scr_has(&s, "Уютный уголок v");
    scr_free(&s);
    key(K_HOME, 0);
    scr_take(&s, W, H);
    ok = ok && scr_has(&s, "Найти или начать беседу") && scr_has(&s, "Друзья");
    scr_free(&s);
    key(K_DOWN, 0);
    scr_take(&s, W, H);
    ok = ok && scr_has(&s, "Уютный уголок v");
    scr_free(&s);
    check(ok, "серверы: End — последний, Home — личные сообщения, Down — «Уютный уголок»");
    /* каналы: Esc -> серверы, Right -> каналы */
    key(K_RIGHT, 0);
    key(K_ESC, 0);
    ui_notice_clear();                           /* «звонок - из личной переписки» от F4 закрыл бы подсказку */
    scr_take(&s, W, H);
    ok = strstr(scr_line(&s, H - 1), "Up/Down сервер") != NULL;
    scr_free(&s);
    key(K_ENTER, 0);
    scr_take(&s, W, H);
    ok = ok && strstr(scr_line(&s, H - 1), "Enter открыть/свернуть");
    scr_free(&s);
    check(ok, "Esc в каналах — к серверам, Enter на сервере — к каналам");
    /* Ctrl+Q */
    key_ctrl('q');
    ok = g_quit;
    g_quit = false;
    check(ok, "Ctrl+Q: выход");
    /* меню настроек (Esc / Up / Down) — экран настроек */
    key(K_TAB, 0); key(K_TAB, 0);                /* -> ввод */
}

/* Мышь: щелчки, правый щелчок, колесо, наведение — по таблице кликов (DESIGN.md раздел 12). */
static void t_mouse(void) {
    CW = 120; CH = 34;
    int x, y;
    hover_off();
    click_xy(3, 7);                              /* первый сервер под «домой» и чертой */
    check(on_screen("Уютный уголок v"), "щелчок по значку сервера открывает «Уютный уголок»");
    /* «#  новости» — строка колонки каналов; просто «мемы» могло бы попасть в ссылку «#новости» в ленте */
    check(click("#  новости") && S.open_ch == 3002, "щелчок по #новости открывает канал");
    check(click("general") && S.open_ch == 3001, "щелчок по #general открывает канал");
    check(click("Текстовые каналы v") && on_screen("Текстовые каналы >") && !scr_has(&cur, "#  новости") && scr_has(&cur, "general"),
          "щелчок по категории сворачивает её (виден только открытый канал)");
    check(click("Текстовые каналы >") && on_screen("#  новости"), "повторный щелчок разворачивает категорию");
    {
        Buf a = {0}, b = {0};
        ui_snapshot(CW, CH, &a);
        cur_take();
        x = y = -1;
        scr_find(&cur, "новости", &x, &y);
        mouse_at(0, x, y);
        ui_snapshot(CW, CH, &b);
        check(a.len && b.len && strcmp(a.p, b.p) != 0, "наведение на #новости подсвечивает строку");
        buf_free(&a); buf_free(&b);
        hover_off();
    }
    hover("(*)", 1, true);
    check(on_screen("Настройки пользователя"), "наведение на (*) — подсказка словом «Настройки пользователя»");
    hover("(?)", 1, false);
    check(on_screen("Справка (F1)"), "наведение на (?) — подсказка «Справка (F1)»");
    hover("GIF", 1, true);
    check(on_screen("GIF: нет в Astra DS"), "наведение на GIF — «GIF: нет в Astra DS»");
    hover_off();
    check(click(":pepe: 1"), "щелчок по реакции :pepe: (в демо запрос не уходит, проверяется путь)");
    check(rclick("держи ссылку"), "правый щелчок по сообщению Боба");
    check(on_screen("Ответить") && scr_has(&cur, "Копировать текст") && scr_has(&cur, "Добавить реакцию") && !scr_has(&cur, "Удалить сообщение")
          && scr_grid_ok(&cur, "меню") && scr_deco_ok(&cur, "меню"),
          "меню чужого сообщения: ответить, реакция, копировать, без удаления; сетка и ASCII");
    click("Ответить");
    check(on_screen("Ответ пользователю Боб"), "пункт «Ответить» включает ответ");
    check(click("(x)") && !on_screen("Ответ пользователю"), "(x) отменяет ответ");
    /* меню с клавиатуры */
    rclick("держи ссылку");
    key(K_DOWN, 0);
    key(K_ENTER, 0);
    check(on_screen("Реакция: эмодзи"), "меню: Down, Enter — второй пункт «Добавить реакцию»");
    key(K_ESC, 0);
    rclick("держи ссылку");
    key(K_ESC, 0);
    check(!on_screen("Копировать текст"), "меню: Esc закрывает");
    rclick("держи ссылку");
    click_xy(CW - 2, 2);                          /* щелчок мимо */
    check(!on_screen("Копировать текст"), "меню: щелчок мимо закрывает");
    /* колесо */
    cur_take();
    for (int i = 0; i < 4; i++) mouse_at(4, 10 + side_w(CW) + 10, 10);
    check(on_screen("ниже ещё"), "колесо вверх по ленте: плашка «ниже ещё N строк»");
    check(click("Перейти к новым") && !on_screen("ниже ещё"), "щелчок по плашке — снова вниз");
    for (int i = 0; i < 4; i++) mouse_at(4, 10 + side_w(CW) + 10, 10);
    for (int i = 0; i < 6; i++) mouse_at(5, 10 + side_w(CW) + 10, 10);
    check(!on_screen("ниже ещё"), "колесо вниз возвращает ленту");
    for (int i = 0; i < 10; i++) mouse_at(5, 15, 10);
    for (int i = 0; i < 10; i++) mouse_at(4, 15, 10);
    check(on_screen("general") && scr_grid_ok(&cur, "колесо"), "колесо по колонке каналов листает и возвращает список");
    for (int i = 0; i < 5; i++) mouse_at(5, 3, 10);
    for (int i = 0; i < 5; i++) mouse_at(4, 3, 10);
    check(scr_grid_ok(&cur, "колесо"), "колесо по колонке серверов");
    /* «нет в mincord» — подсказка уведомлением */
    {   /* волна 2: «o+» — окно приглашения (модальное, Esc закрывает) */
        bool ok = click("o+") && on_screen("Пригласить друзей на") && scr_has(&cur, "Создать ссылку");
        key(K_ESC, 0);
        check(ok && !on_screen("Пригласить друзей на"), "o+ — окно «Пригласить друзей на ...» с «Создать ссылку», Esc закрывает");
    }
    {   /* поиск: щелчок по «Поиск» в шапке -> строка поиска; текст, Enter -> панель результатов; Esc -> search_close */
        bool ok = click("Поиск");
        type("привет");
        key(K_ENTER, 0);
        const SearchState *ss = search_state();
        ok = ok && ss->nhits == 2 && on_screen("результат") && scr_grid_ok(&cur, "поиск") && scr_deco_ok(&cur, "поиск");
        key(K_ESC, 0);
        check(ok && !search_state()->nhits, "Поиск: строка поиска, «привет» Enter -> панель результатов (2), Esc закрывает и освобождает");
    }
    check(click("Нужно бустов") && on_screen("Бусты сервера: нет в Astra DS"), "Нужно бустов — «нет в Astra DS»");
    click("Текстовые каналы v");               /* свернуть: «+» голосовой категории окажется выше */
    click("Текстовые каналы >");
    {
        cur_take();
        bool f = scr_find(&cur, "Голосовые каналы v", &x, &y);
        if (f) {
            int sw = side_w(CW);
            mouse_at(1, 10 + sw - 3, y);
        }
        bool ok = f && on_screen("Создать канал") && scr_has(&cur, "НАЗВАНИЕ КАНАЛА");
        key(K_ESC, 0);
        check(ok && !on_screen("НАЗВАНИЕ КАНАЛА"), "+ у категории — окно «Создать канал» (тип, название), Esc закрывает");
    }
    {
        cur_take();
        bool f = scr_find_ex(&cur, "Написать #general", &x, &y, true);
        if (f) mouse_at(1, 10 + side_w(CW) + 3, y);
        bool ok = f && on_screen("Отправка файла (до 10 МБ)");
        type("Z:/нет/такого/файла.png");
        key(K_ENTER, 0);
        ok = ok && on_screen("Файл не отправлен") && scr_has(&cur, "не найден");
        key(K_ESC, 0);
        check(ok, "+ в поле ввода — режим «Отправка файла (до 10 МБ)», несуществующий путь -> «Файл не отправлен: файл не найден»");
    }
    /* голосовой канал: экран, «Присоединиться», панель голоса */
    voice_toggle_deaf();                          /* без тестового тона в динамиках */
    check(click("Игры") && S.open_ch == 3012, "щелчок по голосовому каналу Игры открывает его экран");
    check(on_screen("Присоединиться к голосовому каналу") && scr_has(&cur, "Сейчас в голосовом чате никого нет")
          && scr_grid_ok(&cur, "экран голоса") && scr_deco_ok(&cur, "экран голоса") && scr_layout_ok(&cur, false, false, "экран голоса"),
          "экран голосового канала: «никого нет», кнопка «Присоединиться к голосовому каналу», сетка и ASCII");
    /* чат справа узкий: приветствие переносится, «Написать Игры» сокращается */
    check(on_screen("Добро пожаловать на канал") && scr_has(&cur, "Написать Игр"), "справа чат канала: приветствие и «Написать Игры»");
    /* «x» в шапке чата — в первой строке «___x__»; поиск просто «x» снизу попал бы в /x\ панели пользователя */
    click_ex("_x_", 1, 1, false);
    check(!on_screen("Добро пожаловать на канал") && scr_has(&cur, "[#]"), "x в шапке чата скрывает чат, в шапке экрана появляется [#]");
    check(click("[#]") && on_screen("Добро пожаловать на канал"), "[#] показывает чат снова");
    click("Присоединиться");
    VoiceStatus vs;
    voice_status(&vs);
    check(vs.phase != VS_IDLE && vs.channel_id == 3012, "кнопка «Присоединиться» входит в голос");
    check(on_screen("Голос") || scr_has(&cur, "Подключение"), "внизу колонки каналов — панель голоса");
    check(on_screen("Отключиться") && scr_has(&cur, "Выбрать статус канала"), "на экране канала — «Отключиться», под каналом «Выбрать статус канала»");
    check(scr_grid_ok(&cur, "в голосе") && scr_deco_ok(&cur, "в голосе") && scr_layout_ok(&cur, false, false, "в голосе"), "в голосе: сетка, ASCII, линии колонок");
    bool mute0 = vs.self_mute;
    click_low("(|)", 1);
    voice_status(&vs);
    check(vs.self_mute != mute0 && on_screen("(/)"), "пиктограмма (|) выключает микрофон, видно (/)");
    click_low("(/)", 1);
    voice_status(&vs);
    check(vs.self_mute == mute0, "(/) включает обратно");
    VoiceFx fx0 = voicefx_get();
    click_low("~v~", 0);
    check(voicefx_get() != fx0, "кнопка ~v~ в панели голоса — смена голоса");
    while (voicefx_get() != VFX_OFF) voicefx_cycle();
    click("general");
    click_low("Игры /", 0);
    check(S.open_ch == 3012, "подпись «канал / сервер» в панели голоса открывает экран канала");
    click("general");
    /* настройки */
    click_low("(*)", 1);
    check(on_screen("НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ") && scr_has(&cur, "Моя учётная запись") && scr_has(&cur, "ТОКЕН") && scr_has(&cur, "ESC")
          && scr_grid_ok(&cur, "настройки") && scr_deco_ok(&cur, "настройки"), "шестерёнка: экран настроек (учётная запись, токен, ESC)");
    click("Голос и видео");
    check(on_screen("ПОРОГ АКТИВАЦИИ") && scr_has(&cur, "СМЕНА ГОЛОСА") && scr_grid_ok(&cur, "настройки"), "раздел «Голос и видео»");
    int db0 = voice_vad_db();
    click("-3 дБ (F9)");
    check(voice_vad_db() == db0 - 3, "кнопка «-3 дБ (F9)» меняет порог");
    click("+3 дБ (F10)");
    click("Голос: как есть");
    check(voicefx_get() == VFX_LOW, "кнопка «Голос: как есть (F11)» — смена голоса");
    while (voicefx_get() != VFX_OFF) voicefx_cycle();
    click("Конфиденциальность");
    check(on_screen("VLESS") && scr_has(&cur, "ПАНИК-КЕЙ") && scr_has(&cur, "Сменить ссылку (C)"), "раздел «Конфиденциальность»: VLESS, паник-кей");
    key(K_UP, 0);                                /* порядок как в Discord: учётная запись, конфиденциальность, вид, голос, уведомления, клавиши */
    check(on_screen("ТОКЕН"), "настройки: Up — предыдущий раздел");
    for (int i = 0; i < 5; i++) key(K_DOWN, 0);  /* «Горячие клавиши» — шестой раздел */
    click("Горячие клавиши");
    /* справка теперь — раздел настроек (как в Discord), а не отдельное окно с [x] */
    check(on_screen("НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ") && scr_has(&cur, "МЫШЬ") && scr_has(&cur, "ПАНЕЛИ") && scr_has(&cur, "СООБЩЕНИЯ")
          && scr_grid_ok(&cur, "горячие клавиши") && scr_deco_ok(&cur, "горячие клавиши"),
          "раздел «Горячие клавиши»: справка внутри настроек (МЫШЬ, ПАНЕЛИ, СООБЩЕНИЯ), сетка и ASCII");
    key(K_ESC, 0);                               /* настройки ещё открыты (раздел справки) */
    check(!on_screen("НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ"), "настройки: Esc закрывает");
    click_low("(*)", 1);
    cur_take();
    check(scr_find(&cur, "ESC", &x, &y) && (mouse_at(1, x + 1, y - 1), !on_screen("НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ")), "настройки: круглая кнопка x закрывает");
    click_low("\\_/", 1);
    voice_status(&vs);
    check(vs.phase == VS_IDLE, "значок \\_/ в панели голоса отключает");
    /* своё сообщение: меню, удаление */
    type("моё тестовое");
    key(K_ENTER, 0);
    rclick("моё тестовое");
    check(on_screen("Изменить") && scr_has(&cur, "Удалить сообщение"), "меню своего сообщения: «Изменить» и «Удалить сообщение»");
    click("Удалить сообщение");
    check(on_screen("Вы точно хотите удалить") && scr_has(&cur, "моё тестовое"), "окно подтверждения удаления с текстом сообщения");
    check(click("Отмена") && !on_screen("Вы точно хотите"), "«Отмена» закрывает окно");
    rclick("моё тестовое");
    click("Удалить сообщение");
    click("Удалить (y)");
    check(on_screen("в демо-режиме удаление не отправляется"), "«Удалить (y)» подтверждает");
    rclick("моё тестовое");
    click("Удалить сообщение");
    click_xy(CW - 3, 3);
    check(!on_screen("Вы точно хотите"), "щелчок мимо окна удаления — отмена");
    rclick("моё тестовое");
    click("Изменить");
    check(on_screen("Изменение сообщения") && scr_has(&cur, "моё тестовое"), "пункт «Изменить» — правка в поле");
    key(K_ESC, 0);
    rclick("моё тестовое");
    click("Копировать текст");
    check(on_screen("текст скопирован"), "пункт «Копировать текст» (OSC 52)");
    /* колонка участников */
    check(on_screen("В ЧАТЕ - "), "справа колонка участников «В ЧАТЕ - N»");
    check(click("oo") && !on_screen("В ЧАТЕ - ") && scr_layout_ok(&cur, true, false, "без участников"), "oo в шапке скрывает колонку");
    check(click("oo") && on_screen("В ЧАТЕ - "), "oo снова показывает");
    /* клик в поле ввода — фокус */
    key(K_TAB, 0);
    cur_take();
    scr_find_ex(&cur, "Написать #general", &x, &y, true);
    mouse_at(1, x + 3, y);
    ui_notice_clear();                           /* «текст скопирован» закрыл бы подсказку */
    check(on_screen("Enter отправить"), "щелчок в поле ввода — фокус ввода");
    /* (?) — справка */
    check(click("(?)") && on_screen("справка:"), "(?) открывает справку");
    click("[x]");
    /* ЛС: «домой», «Друзья», беседа, звонок */
    click_xy(3, 1);
    check(on_screen("Найти или начать беседу") && scr_has(&cur, "Друзья") && scr_has(&cur, "Nitro"), "кнопка «домой»: личные сообщения");
    click("Друзья");
    check(on_screen("Добавить в друзья") && scr_has(&cur, "В СЕТИ - ") && scr_has(&cur, "Алиса") && !scr_has(&cur, "Спамер")
          && scr_grid_ok(&cur, "друзья") && scr_deco_ok(&cur, "друзья"),
          "экран «Друзья»: вкладки, «Добавить в друзья», «В СЕТИ - N» с друзьями, без заблокированных, сетка и ASCII");
    check(click("Ожид.") && on_screen("ОЖИДАНИЕ - 2") && scr_has(&cur, "Незнакомец") && scr_has(&cur, "Ваня"),
          "вкладка «Ожидание»: входящая и исходящая заявки");
    {
        int nch = S.nchans;
        click("В сети");
        bool ok = click("Ночной Филин");
        Channel *c = chan_find(S.open_ch);
        check(ok && S.nchans == nch + 1 && c && c->type == CH_DM && c->nrecip == 1 && c->recipients[0] == 1004,
              "щелчок по другу -> dm_open: новая ЛС создана (демо) и открыта");
    }
    check(click("Алиса   ") && S.open_ch == 4000 && on_screen("(~)"), "ЛС с Алисой: в шапке (~) позвонить");
    check(on_screen("созвонимся?") && scr_grid_ok(&cur, "ЛС") && scr_deco_ok(&cur, "ЛС") && scr_layout_ok(&cur, true, false, "ЛС"),
          "ЛС: лента, сетка, ASCII, рамка ввода без колонки участников");
    click("(~)");
    voice_status(&vs);
    check(vs.phase != VS_IDLE && vs.channel_id == 4000, "(~) звонит в ЛС (в демо — вход в голос)");
    key(K_F7, 0);
    voice_status(&vs);
    check(vs.phase == VS_IDLE, "F7 отключает");
    /* входящий звонок: щелчок «Принять (F2)» */
    S.call.ringing = true; S.call.channel_id = 4000; S.call.caller = 1001;
    check(click("Принять (F2)") && !S.call.ringing, "окно «Входящий звонок»: «Принять (F2)»");
    voice_status(&vs);
    check(vs.phase != VS_IDLE && vs.channel_id == 4000, "принятый звонок — в голосе");
    key(K_F7, 0);
    S.call.ringing = true; S.call.channel_id = 4000; S.call.caller = 1001;
    check(click("Отклонить (F3)") && !S.call.ringing, "«Отклонить (F3)»");
    voice_toggle_deaf();
    voice_status(&vs);
    check(vs.phase == VS_IDLE && !vs.self_deaf, "после проверок голос выключен, звук включён как был");
    /* правый щелчок и колесо при открытом меню / щелчки по пустому месту — без падений */
    for (int i = 0; i < 40; i++) {
        mouse_at(i % 2 ? 3 : 1, (i * 37) % CW, (i * 11) % CH);
        mouse_at(4 + i % 2, (i * 53) % CW, (i * 7) % CH);
        mouse_at(0, (i * 29) % CW, (i * 13) % CH);
        mouse_at(2, (i * 31) % CW, (i * 5) % CH);
    }
    key(K_ESC, 0);
    S.call.ringing = false;
    g_quit = false;
    cur_take();
    check(scr_grid_ok(&cur, "случайные щелчки"), "160 случайных щелчков/колёс/наведений: интерфейс жив, сетка цела");
    click_xy(3, 7);
    click("general");
}

/* Вредный ввод (DESIGN.md: ни одной поломки сетки, ни одного управляющего символа на выходе):
 * сообщения, имена и название канала с ESC/OSC, C1, bidi, огромные строки, ZWJ-эмодзи, пустые,
 * битый UTF-8 — настоящим путём событий (JSON -> store -> ui). */
static void evil_msg(u64 id, u64 uid, const char *uname, const char *text) {
    Buf b = {0};
    buf_printf(&b, "{\"id\":\"%llu\",\"channel_id\":\"3001\",\"author\":{\"id\":\"%llu\",\"username\":\"evil\",\"global_name\":",
               (unsigned long long)id, (unsigned long long)uid);
    buf_json_str(&b, uname);
    buf_puts(&b, "},\"content\":");
    buf_json_str(&b, text);
    buf_puts(&b, ",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    cJSON *j = json_parse(b.p, b.len);
    if (j) store_event("MESSAGE_CREATE", j);
    json_done();
    buf_free(&b);
}
static void t_evil(void) {
    static char huge[100001], hugename[1201];
    memset(huge, 'A', sizeof huge - 1);
    memset(hugename, 0, sizeof hugename);
    for (int i = 0; i < 400; i++) memcpy(hugename + i * 3, "\xe6\x97\xa5", 3);          /* 400 x 日 */
    static const char *const texts[] = {
        "\x1b[2J\x1b]0;взлом\x07\x1b[31m красный? \x1b[?1049h",
        "\xc2\x9b" "31m C1 CSI \xc2\x9d" "0;title\xc2\x9c",
        "\xe2\x80\xae" "задом наперёд \xe2\x81\xa6" "изолят\xe2\x81\xa9 \xe2\x80\x8f RLM \xd8\x9c ALM",
        "👨\u200d👩\u200d👧\u200d👦 семья 🏳️\u200d🌈 флаг 🇷🇺🇺🇦 👍🏽 é (e+акут) ﷽",
        "",
        "\t табуляция\r возврат каретки\n\n\n\n\n\n\n\n\n\n много строк \x7f DEL \x08 BS",
        "\xff\xfe\xc0\xaf битый UTF-8 \xed\xa0\x80 суррогат",
        "\u2028 LS \u2029 PS \ufeff BOM \u200b ZWSP \u00ad мягкий перенос",
    };
    u64 id = 77000;
    int n0 = S.nmsgs;
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++) evil_msg(id++, 1003, "\x1b[31mЗлой\xe2\x80\xae", texts[i]);
    evil_msg(id++, 1003, hugename, huge);
    evil_msg(id++, 1003, "", "пустое имя");
    {
        Buf b = {0};
        buf_puts(&b, "{\"id\":\"3099\",\"type\":0,\"guild_id\":\"2000\",\"parent_id\":\"3000\",\"position\":9,\"name\":");
        buf_json_str(&b, "\x1b[31mзлой-канал\xe2\x80\xae\xc2\x9b" "2J" "\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5");
        buf_puts(&b, "}");
        cJSON *j = json_parse(b.p, b.len);
        if (j) store_event("CHANNEL_CREATE", j);
        json_done();
        buf_free(&b);
    }
    check(S.nmsgs == n0 + (int)(sizeof texts / sizeof *texts) + 2, "вредный ввод: все сообщения приняты (не потеряны и не уронили разбор)");
    bool clean = true;
    for (int i = 0; i < S.nmsgs; i++) if (dirty(S.msgs[i].content) || dirty(S.msgs[i].author)) clean = false;
    Channel *ec = chan_find(3099);
    {   /* последовательности терминала вырезаются целиком (имя, текст), а не только байт ESC */
        char *a1 = san("\x1b[31mМэллори\xe2\x80\xae", false), *a2 = san("x\x1b]0;взлом\x07y\xc2\x9b" "2Jz\x1b", false);
        check(!strcmp(a1, "Мэллори") && !strcmp(a2, "xyz"), "вредный ввод: CSI/OSC вырезаются целиком («\\e[31mМэллори» -> «Мэллори»)");
        free(a1); free(a2);
    }
    check(clean && ec && !dirty(ec->name), "вредный ввод: в модели нет ESC, C1, bidi (сообщения, имена, канал)");
    static const int sizes[][2] = { { 50, 12 }, { 64, 16 }, { 80, 24 }, { 120, 34 }, { 200, 60 } };
    bool all = true;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        for (int pg = 0; pg < 3; pg++) {
            Scr s;
            scr_take(&s, sizes[i][0], sizes[i][1]);
            bool ok = scr_grid_ok(&s, "вредный ввод") && scr_deco_ok(&s, "вредный ввод");
            if (!ok) { say("    %dx%d, страница %d:\n", sizes[i][0], sizes[i][1], pg); scr_print(&s); }
            all = all && ok;
            scr_free(&s);
            key(K_BTAB, 0);                      /* к сообщениям: листать вверх по вредным */
            key(K_PGUP, 0);
            key(K_ESC, 0);
        }
        key(K_BTAB, 0); key(K_END, 0); key(K_ESC, 0);
    }
    check(all, "вредный ввод на 50x12..200x60 (с прокруткой): сетка ровная, на выходе только SGR, без запрещённых символов");
}

/* Утечки: 1000 переключений каналов/экранов/меню с отрисовкой — частная память не растёт. */
static void t_leak(void) {
    Buf r = {0};
    for (int warm = 0; warm < 50; warm++) { key(K_F1, 0); key(K_F1, 0); ui_snapshot(120, 34, &r); buf_free(&r); }
    /* 30000 оборотов, порог 64 КБ (ревью 3): первые 10000 — прогрев (кэши интерфейса; разовые ступени кучи
     * ~256 КБ приходят в разное время), затем 20000 — роста быть не должно (утечка 3 байта/оборот уже видна) */
    size_t m0 = 0;
    i64 t0 = now_ms();
    int iters = getenv("ST_LEAK_N") ? atoi(getenv("ST_LEAK_N")) : 30000;   /* для разбора: ST_LEAK_N=50000 */
    if (iters < 11000) iters = 11000;
    for (int i = 0; i < iters; i++) {
        if (i == 10000) m0 = plat_private_bytes();
        if (i % 1000 == 0 && getenv("ST_LEAK")) say("    %5d: %llu КБ, пользователей %d, сообщений %d\n", i, (unsigned long long)(plat_private_bytes() / 1024), S.nusers, S.nmsgs);
        switch (i % 8) {
        case 0: key(K_TAB, 0); break;                        /* серверы */
        case 1: key(K_DOWN, 0); break;
        case 2: key(K_TAB, 0); key(K_DOWN, 0); key(K_ENTER, 0); break;   /* канал */
        case 3: key(K_F1, 0); break;                         /* справка */
        case 4: key(K_F1, 0); break;
        case 5: key(K_BTAB, 0); key(K_PGUP, 0); break;       /* сообщения, листать */
        case 6: key(K_END, 0); key(K_ESC, 0); break;
        case 7: mouse_at(4 + (i / 8) % 2, 60, 15); break;   /* колесо */
        }
        ui_snapshot(i % 3 ? 120 : 64, i % 3 ? 34 : 16, &r);
        buf_free(&r);
    }
    size_t m1 = plat_private_bytes();
    char w[220];
    snprintf(w, sizeof w, "утечки: 30000 переключений каналов/справки/прокрутки с отрисовкой за %lld мс; память %.0f -> %.0f КБ (рост после 10000)",
             (long long)(now_ms() - t0), m0 / 1024.0, m1 / 1024.0);
    check(m1 < m0 + (64u << 10), w);
}

/* ======================= запуск ======================= */
/* Данные и разбор: права, голосовые состояния, отсев событий, очистка, разметка, отправка. */
static void t_data(void) {
    check(S.ready && S.nguilds >= 1, "демо READY разобран");
    Channel *sec = chan_find(3003), *gen = chan_find(3001), *voice = chan_find(3011);
    check(sec && !sec->visible, "права: канал с запретом VIEW для @everyone скрыт");
    check(gen && gen->visible && gen->can_send, "права: обычный канал виден, писать можно");
    check(voice && voice->can_connect, "права: в голосовой канал можно войти");
    const VoiceState *vs[8];
    check(voice_members(3011, vs, 8) >= 1, "голосовые состояния: в «ОБЩИЙ» есть участники");

    const char *pres = "{\"t\":\"PRESENCE_UPDATE\",\"s\":7,\"op\":0,\"d\":{}}";
    const char *msg = "{\"t\":\"MESSAGE_CREATE\",\"s\":8,\"op\":0,\"d\":{}}";
    const char *hello = "{\"t\":null,\"s\":null,\"op\":10,\"d\":{}}";
    check(!event_wanted(pres, strlen(pres)), "отсев: PRESENCE_UPDATE отброшен без разбора");
    check(event_wanted(msg, strlen(msg)), "отсев: MESSAGE_CREATE принят");
    check(event_wanted(hello, strlen(hello)), "отсев: служебный Hello принят");

    bool clean = true;
    for (int i = 0; i < S.nusers; i++) if (dirty(S.users[i].name)) clean = false;
    check(clean, "очистка: имена без ESC, C1 и смены направления");
    clean = true;
    for (int i = 0; i < S.nmsgs; i++) if (dirty(S.msgs[i].content) || dirty(S.msgs[i].author)) clean = false;
    check(S.open_ch == 3001 && S.nmsgs > 0, "история #general загружена");
    check(clean, "очистка: в сообщениях нет управляющих последовательностей");
    Message *m1 = msg_find(9001, NULL), *m5 = msg_find(9005, NULL), *m2 = msg_find(9002, NULL);
    check(m1 && strstr(m1->content, ":wave:") && !strstr(m1->content, "<:"), "разметка: своя эмодзи показана как :wave:");
    check(m5 && strstr(m5->content, "@Вы"), "разметка: упоминание показано именем");
    check(m5 && m5->nreacts == 2 && m5->reacts[0].me, "реакции: две, своя отмечена");
    check(m2 && m2->ref_id == 9001, "ответ разобран");

    int n0 = S.nmsgs;
    type("тест ✓ ünïcödé 日本 ✅❤ 👍🏽");
    key(K_ENTER, 0);
    check(S.nmsgs == n0 + 1 && !strcmp(S.msgs[S.nmsgs - 1].content, "тест ✓ ünïcödé 日本 ✅❤ 👍🏽"),
          "отправка: сообщение с юникодом добавлено");
    Scr s;
    scr_take(&s, 110, 30);
    bool ok = scr_grid_ok(&s, "юникод") && scr_deco_ok(&s, "юникод");
    if (!ok) scr_print(&s);
    scr_free(&s);
    check(ok, "ширина: сетка 110x30 ровная после сообщения с ✓ ✅ ❤ 日本 и тоном кожи");
}

/* ---------- фаза 4: друзья, новая ЛС, поиск, файл (модель, без сети) ---------- */
const char *search_test_path(void);             /* store.c, только для проверок */
void search_test_resp(long status, const char *body);
static void ev(const char *t, const char *json) {
    cJSON *j = json_parse(json, strlen(json));
    if (j) store_event(t, j);
    json_done();
}
static bool wanted(const char *raw) { return event_wanted(raw, strlen(raw)); }
static u8 rel_type(u64 id) { const Relation *r = rel_find(id); return r ? r->type : 0; }
static u8 rel_st(u64 id) { const Relation *r = rel_find(id); return r ? r->status : 255; }

static void t_friends(void) {
    size_t m0 = plat_private_bytes();
    const Relation *rs;
    int n = rel_list(&rs);
    bool sorted = true;
    for (int i = 1; i < n; i++) if (rs[i - 1].id >= rs[i].id) sorted = false;
    check(n == 8 && sorted && sizeof(Relation) == 16, "READY relationships: 8 записей (тип 5 не хранится), по id, 16 байт на запись");
    check(rel_type(1001) == REL_FRIEND && rel_type(1007) == REL_IN && rel_type(1008) == REL_OUT && rel_type(1009) == REL_BLOCKED && !rel_find(1003),
          "типы: друг / входящая / исходящая / заблокирован");
    check(!strcmp(user_name(1007), "Незнакомец"), "имя из объекта user в записи отношения");
    check(rel_st(1001) == PS_ONLINE && rel_st(1002) == PS_IDLE && rel_st(1004) == PS_DND && rel_st(1005) == PS_OFFLINE,
          "статусы друзей из merged_presences.friends: в сети / не активен / не беспокоить / не в сети");
    check(rel_st(1009) == PS_OFFLINE && rel_st(1007) == PS_OFFLINE, "статус не друга не хранится (заблокированный «в сети» -> не в сети)");
    check(!strcmp(presence_name(PS_DND), "не беспокоить") && !strcmp(presence_name(PS_OFFLINE), "не в сети"), "presence_name");

    /* отсев PRESENCE_UPDATE до разбора: только друзья */
    check(wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user\":{\"id\":\"1005\"},\"status\":\"online\"}}"),
          "отсев: присутствие друга проходит");
    check(!wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user\":{\"id\":\"1003\"},\"status\":\"online\"}}") &&
          !wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user\":{\"id\":\"1007\"},\"status\":\"online\"}}") &&
          !wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"guild_id\":\"2000\",\"user\":{\"id\":\"99999\"},\"status\":\"dnd\"}}"),
          "отсев: чужие присутствия, заявки и сервера отброшены без разбора");
    check(wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user\":{\"avatar_decoration_data\":{\"id\":\"7\",\"sku_id\":\"8\"},\"username\":\"a\\\"}\",\"id\":\"1002\"},\"status\":\"idle\"}}") &&
          !wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user\":{\"x\":{\"id\":\"1001\"},\"id\":\"5\"}}}") &&
          !wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user\":{\"id\":\"10011") &&
          !wanted("{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"status\":\"online\"}}"),
          "отсев: id только первого уровня user (вложенные и строки с \\\" не путают), обрезанный пакет отброшен");
    ev("PRESENCE_UPDATE", "{\"user\":{\"id\":\"1005\"},\"status\":\"online\"}");
    ev("PRESENCE_UPDATE", "{\"user\":{\"id\":\"1009\"},\"status\":\"online\"}");
    check(rel_st(1005) == PS_ONLINE && rel_st(1009) == PS_OFFLINE, "PRESENCE_UPDATE: друг стал «в сети», заблокированный — нет");
    {
        const char *raw = "{\"t\":\"PRESENCE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user\":{\"id\":\"88888888888888888\"},\"status\":\"online\",\"client_status\":{\"desktop\":\"online\"},\"activities\":[{\"name\":\"Game\",\"type\":0}]}}";
        i64 t0 = now_ms();
        int kept = 0;
        for (int i = 0; i < 100000; i++) kept += event_wanted(raw, strlen(raw));
        char b[160];
        snprintf(b, sizeof b, "отсев: 100000 чужих присутствий за %lld мс, ни одно не разобрано", (long long)(now_ms() - t0));
        check(kept == 0 && now_ms() - t0 < 1000, b);
    }

    /* события отношений */
    check(wanted("{\"t\":\"RELATIONSHIP_ADD\",\"s\":1}") && wanted("{\"t\":\"RELATIONSHIP_REMOVE\",\"s\":1}") && wanted("{\"t\":\"RELATIONSHIP_UPDATE\",\"s\":1}"),
          "отсев: RELATIONSHIP_* нужны");
    ev("RELATIONSHIP_ADD", "{\"id\":\"1003\",\"type\":3,\"user\":{\"id\":\"1003\",\"username\":\"m\",\"global_name\":\"\\u001b[31mЗлой\\u202e\"}}");
    check(rel_type(1003) == REL_IN && !strchr(user_name(1003), 0x1b) && !strstr(user_name(1003), "\xe2\x80\xae"),
          "RELATIONSHIP_ADD: входящая заявка, имя очищено san()");
    ev("RELATIONSHIP_ADD", "{\"id\":\"1003\",\"type\":1}");
    check(rel_type(1003) == REL_FRIEND && rel_st(1003) == PS_OFFLINE, "заявка принята (ADD type 1): друг, статус «не в сети» до присутствия");
    ev("RELATIONSHIP_REMOVE", "{\"id\":\"1003\",\"type\":1}");
    check(!rel_find(1003) && rel_list(NULL) == 8, "RELATIONSHIP_REMOVE: запись удалена");
    ev("RELATIONSHIP_ADD", "{\"id\":\"0\",\"type\":1}");
    ev("RELATIONSHIP_ADD", "{\"id\":\"77\",\"type\":9}");
    check(rel_list(NULL) == 8, "мусорные отношения (id 0, тип 9) не хранятся");
    /* вредный ввод в хэшах аватаров (хэш идёт в адрес CDN) */
    ev("RELATIONSHIP_ADD", "{\"id\":\"1201\",\"type\":3,\"user\":{\"id\":\"1201\",\"username\":\"a\",\"avatar\":\"../../../evil?x=0123456789abcdef0\"}}");
    ev("RELATIONSHIP_ADD", "{\"id\":\"1202\",\"type\":3,\"user\":{\"id\":\"1202\",\"username\":\"b\",\"avatar\":\"0123456789ABCDEF0123456789ABCDEF\"}}");
    ev("RELATIONSHIP_ADD", "{\"id\":\"1203\",\"type\":3,\"user\":{\"id\":\"1203\",\"username\":\"c\",\"avatar\":\"a_0123456789abcdef0123456789abcdef\"}}");
    ev("RELATIONSHIP_ADD", "{\"id\":\"1204\",\"type\":3,\"user\":{\"id\":\"1204\",\"username\":\"d\",\"avatar\":\"0123456789abcdef0123456789abcde\\u0000f\"}}");
    check(!user_avatar(1201) && !user_avatar(1202) && !user_avatar(1204) && user_avatar(1203) && !strcmp(user_avatar(1203), "a_0123456789abcdef0123456789abcdef"),
          "аватар: путь/параметры, верхний регистр, NUL внутри — отвергнуты; a_+32 hex принят");
    ev("RELATIONSHIP_UPDATE", "{\"id\":\"1203\",\"type\":3,\"user\":{\"id\":\"1203\",\"username\":\"c2\"}}");
    check(user_avatar(1203) != NULL, "аватар: частичный user без ключа avatar не стирает известный хэш");
    ev("RELATIONSHIP_UPDATE", "{\"id\":\"1203\",\"type\":3,\"user\":{\"id\":\"1203\",\"username\":\"c2\",\"avatar\":null}}");
    check(user_avatar(1203) == NULL, "аватар: avatar:null — хэш снят");
    for (u64 i = 1201; i <= 1204; i++) { char b[64]; snprintf(b, sizeof b, "{\"id\":\"%llu\"}", (unsigned long long)i); ev("RELATIONSHIP_REMOVE", b); }
    rel_accept(1007);
    check(rel_type(1007) == REL_FRIEND, "rel_accept (демо): входящая -> друг, запрос не уходит");
    rel_remove(1008);
    check(!rel_find(1008), "rel_remove (демо): исходящая заявка отменена");

    /* память: 1000 друзей */
    size_t a = plat_private_bytes();
    for (int i = 0; i < 1000; i++) {
        char b[200];
        snprintf(b, sizeof b, "{\"id\":\"%d\",\"type\":1,\"user\":{\"id\":\"%d\",\"username\":\"friend%d\"}}", 500000 + i, 500000 + i, i);
        ev("RELATIONSHIP_ADD", b);
    }
    size_t b2 = plat_private_bytes();
    for (int i = 0; i < 1000; i++) { char b[64]; snprintf(b, sizeof b, "{\"id\":\"%d\"}", 500000 + i); ev("RELATIONSHIP_REMOVE", b); }
    say("  память: друзья в демо %+.1f КБ; 1000 друзей (с именами) %+.1f КБ\n",
        ((double)a - (double)m0) / 1024.0, ((double)b2 - (double)a) / 1024.0);
    check(rel_list(NULL) == 7 && b2 < a + (1u << 20), "1000 друзей: < 1 МБ (16 байт запись + пользователь с именем), удалены");
}

/* ---------- волна 1: модель (демо и подставные ответы, без сети) ---------- */
static void t_wave1(void) {
    size_t m0 = plat_private_bytes();
    u64 was = S.open_ch;
    /* тема — только у открытого канала */
    S.open_ch = 3001; demo_send(3001, NULL);
    Channel *c1 = chan_find(3001);
    bool t1 = c1 && c1->topic && strstr(c1->topic, "Уютное общение");
    S.open_ch = 3002; demo_send(3002, NULL);
    check(t1 && !c1->topic, "тема: у открытого канала есть, при переходе в другой — освобождена");
    /* бот, вебхук, пересланное, тихое, ответ на сообщение без текста, спойлер */
    Message *bot = msg_find(9100, NULL), *fwd = msg_find(9099, NULL), *wh = msg_find(9097, NULL), *sp = msg_find(9098, NULL);
    check(bot && bot->bot && bot->flags == 4096 && bot->ref_text && !strcmp(bot->ref_text, "[файл]"),
          "сообщение бота: bot, flags 4096 (тихое), ответ на сообщение без текста — «[файл]»");
    check(fwd && fwd->forwarded && strstr(fwd->content, "переслано из другого чата") && !fwd->ref_id,
          "пересланное: текст оригинала из message_snapshots, без «(сообщение недоступно)»");
    check(wh && wh->webhook && sp && strstr(sp->content, "[файл: SPOILER_a.png]"), "вебхук помечен; спойлер-вложение — «[файл: SPOILER_имя]»");
    /* переход к сообщению и «Вернуться к настоящему» */
    msgs_jump(3001, 9003);
    check(S.open_ch == 3001 && S.jump_id == 9003 && msg_find(9003, NULL) && !S.detached, "msgs_jump (демо): канал открыт, сообщение в ленте, jump_id");
    S.detached = true;
    int nm = S.nmsgs;
    ev("MESSAGE_CREATE", "{\"id\":\"99006\",\"channel_id\":\"3001\",\"author\":{\"id\":\"1002\",\"username\":\"bob\"},\"content\":\"новое\",\"timestamp\":\"2026-11-08T10:06:00+00:00\"}");
    Channel *c = chan_find(3001);
    check(S.nmsgs == nm && !msg_find(99006, NULL) && c && c->last_msg == 99006, "отрыв (detached): новое сообщение не дописано в ленту (разрыв), last_msg обновлён");
    msgs_present();
    check(!S.detached && !S.jump_id && S.nmsgs > 0, "msgs_present: снова последние, отрыва нет");
    /* исходный текст своих сообщений для правки (ревью 5 С1) */
    demo_send(3001, "<@1001> глянь <#3002> :fire:");
    const Message *own = S.nmsgs ? &S.msgs[S.nmsgs - 1] : NULL;
    const Message *other = msg_find(9003, NULL);
    check(own && own->raw && !strcmp(own->raw, "<@1001> глянь <#3002> :fire:") && strstr(own->content, "@Алиса") && other && !other->raw,
          "правка: у своих — исходный текст (<@id> <#id> как набраны), в ленте — раскрытый; у чужих raw нет");
    /* закреплённые */
    pins_open(3001);
    const PinsState *ps = pins_state();
    check(!ps->busy && ps->n == 2 && ps->channel_id == 3001 && !ps->more, "закреплённые (демо): 2 в #general");
    msg_pin(3001, 9005, true);
    bool pin3 = ps->n == 3 && msg_find(9005, NULL) && msg_find(9005, NULL)->pinned;
    msg_pin(3001, 9001, false);
    check(pin3 && ps->n == 2, "закрепить / открепить: список перечитан (CHANNEL_PINS_UPDATE)");
    {
        static const char P1[] = "{\"items\":[{\"pinned_at\":\"2026&before=x\",\"message\":{\"id\":\"77\",\"content\":\"\\u001b[31mevil\"}}],\"has_more\":true}";
        static const char P2[] = "[{\"id\":\"78\",\"content\":\"старый формат\"}]";
        cJSON *j = json_parse(P1, sizeof P1 - 1);
        pins_apply(j);
        json_done();
        check(ps->n == 3 && !ps->more && !strchr(ps->m[2].content, 0x1b), "закреплённые: pinned_at с & не попадает в адрес «ещё», текст очищен");
        j = json_parse(P2, sizeof P2 - 1);
        pins_apply(j);
        json_done();
        check(ps->n == 4 && !strcmp(ps->m[3].content, "старый формат"), "закреплённые: старый формат ответа (массив) тоже разбирается");
    }
    pins_close();
    check(!ps->n && !ps->m, "pins_close: всё освобождено");
    /* упоминания */
    mentions_open();
    const MentionsState *ms = mentions_state();
    check(!ms->busy && ms->n == 3 && ms->hits[0].channel_id == 3001 && !strchr(ms->hits[2].m.content, 0x1b) && ms->hits[2].m.bot,
          "упоминания (демо): 3, канал у каждого, текст очищен, бот помечен");
    mentions_close();
    check(!ms->n && !ms->hits, "mentions_close: освобождено");
    /* прочитано */
    Channel *c5 = chan_find(3005);
    bool had = c5 && c5->mentions == 2;
    ack_channel(3005);
    check(had && !c5->mentions && c5->last_read == c5->last_msg, "ack_channel: упоминания сброшены, прочитано до последнего");
    mark_unread(3001, 9003);
    check(c && c->last_read == 9002 && chan_unread(c), "mark_unread: канал непрочитан с выбранного сообщения");
    ack_guild(2000);
    int gm = 0;
    check(!guild_unread(2000, &gm) && !gm, "ack_guild: на сервере всё прочитано");
    mark_unread(3001, 9003);
    ack_all();
    bool any = false;
    for (int i = 0; i < S.nchans; i++) if (chan_unread(&S.chans[i]) || S.chans[i].mentions) any = true;
    check(!any, "ack_all: прочитано всё (серверы и ЛС)");
    /* сеть заглушена g_readonly (запросы не ставятся), gateway не подключён (op 3 никуда не уходит) */
    O.demo = false; g_readonly = true;
    /* статус: op 3 не чаще 5 раз за 20 с */
    i64 w_before = store_next_wake();
    for (int i = 0; i < 7; i++) presence_set(i % 2 ? PS_IDLE : PS_DND);
    i64 w2 = store_next_wake();
    check(w_before == INT64_MAX && presence_me() == PS_DND && w2 > now_ms() + 10000,
          "статус: 7 смен подряд — 5 ушли, остальное отложено на ~20 с, побеждает последнее");
    /* частота отметок: не чаще раза в 3 с на канал */
    c->last_read = 0; ack_channel(3001);
    i64 w0 = store_next_wake();
    c->last_read = 0; ack_channel(3001);
    i64 w1 = store_next_wake(), now = now_ms();
    O.demo = true; g_readonly = false;
    check(w0 == w2 && w1 > now && w1 <= now + 3000, "отметка о прочтении: вторая за 3 с отложена (store_next_wake), не отправлена сразу");
    presence_set(PS_ONLINE);
    /* меню человека */
    rel_block(1002);
    rel_add(1003);
    check(rel_type(1002) == REL_BLOCKED && rel_type(1003) == REL_OUT, "rel_block / rel_add (демо): заблокирован, заявка отправлена");
    rel_remove(1002); rel_remove(1003);
    /* профиль */
    profile_open(1001, 2000);
    const Profile *pf = profile_state();
    check(!pf->busy && !strcmp(pf->name, "Алиса") && !strcmp(pf->pronouns, "она/её") && strchr(pf->bio, '\n') &&
          pf->member_since_unix > 0 && pf->nroles == 1 && pf->roles[0] == 2001 && pf->accent == 15548997,
          "профиль (демо): имя, местоимения, о себе (многострочно), на сервере с, роли, цвет");
    profile_close();
    check(!pf->id && !pf->roles, "profile_close: освобождено, без кэша");
    /* один экземпляр */
    if (!single_instance()) say("  (один экземпляр: уже запущен другой Astra DS — проверка пропущена)\n");
    else check(!single_instance(), "один экземпляр: второй запуск видит первый (мьютекс/абстрактный сокет, без файлов)");
    S.open_ch = was;
    if (was) demo_send(was, NULL);
    size_t m1 = plat_private_bytes();
    say("  память: волна 1 после всех проверок и закрытий %+.1f КБ (бюджет постоянной ~90 КБ)\n", ((double)m1 - (double)m0) / 1024.0);
    check(m1 < m0 + (128u << 10), "волна 1: после закрытия панелей память не растёт (< 128 КБ)");
}

/* Таблица пользователей не растёт с историей каналов (вытеснение тех, на кого никто не ссылается). */
static void t_users_gc(void) {
    u64 was = S.open_ch;
    int n0 = S.nusers, peak = 0;
    size_t m0 = plat_private_bytes();
    for (int round = 0; round < 60; round++) {
        Buf b = {0};
        buf_puts(&b, "[");
        for (int k = 0; k < 50; k++)
            buf_printf(&b, "%s{\"id\":\"%d\",\"channel_id\":\"3006\",\"author\":{\"id\":\"%d\",\"username\":\"автор_%d_%d\"},\"content\":\"x\",\"timestamp\":\"2026-11-08T10:00:00+00:00\"}",
                       k ? "," : "", 600000 + round * 100 + k, 700000 + round * 100 + k, round, k);
        buf_puts(&b, "]");
        S.open_ch = 3006;
        cJSON *j = json_parse(b.p, b.len);
        store_set_messages(j, false);
        json_done();
        buf_free(&b);
        if (S.nusers > peak) peak = S.nusers;
    }
    size_t m1 = plat_private_bytes();
    const Message *last = S.nmsgs ? &S.msgs[S.nmsgs - 1] : NULL;
    bool names_ok = last && !strcmp(user_name(last->author_id), "автор_59_49") && !strcmp(user_name(1001), "Алиса") &&
                    !strcmp(user_name(1004), "Ночной Филин") && strcmp(user_name(700000), "автор_0_0");
    char w[400];
    snprintf(w, sizeof w, "пользователи: 60 каналов по 50 новых авторов (3000) — в таблице не больше %d (было %d), память %+.0f КБ",
             peak, n0, ((double)m1 - (double)m0) / 1024.0);
    check(peak < 600 && names_ok, w);
    check(names_ok, "пользователи: авторы открытой ленты, собеседники ЛС и друзья остались, старые авторы вытеснены");
    S.open_ch = was;
    if (was) demo_send(was, NULL);
}

/* Капча на REST-запросе: запрос ждёт, отказ отдаёт 400 вызывающему, повтор — с X-Captcha-* (без окна и сети). */
bool net_captcha_test_solve(const char *answer, char *hdrs, size_t n);
void net_captcha_test_closed(void);
static long cap_cb_status = -1;
static void cap_cb(Resp *r, void *ctx) { (void)ctx; cap_cb_status = r->status; }
static void t_captcha(void) {
    static const char RESP[] = "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":\"a9b5fb07-92ff-493f-86fe-352a2803b3df\","
        "\"captcha_service\":\"hcaptcha\",\"captcha_session_id\":\"sess-1\",\"captcha_rqdata\":\"rq+data/1=\",\"captcha_rqtoken\":\"rqtok.1\"}";
    size_t m0 = plat_private_bytes();
    net_captcha_test("POST", "/users/@me/relationships", RESP, cap_cb, NULL);
    const CaptchaState *cs = net_captcha();
    check(cs->pending && !cs->open && !strcmp(cs->what, "заявка в друзья") && cap_cb_status == -1,
          "капча: 400 с captcha_* — запрос не потерян, ждёт проверки («заявка в друзья»), вызывающему пока ничего");
    net_captcha_cancel();
    check(!cs->pending && cap_cb_status == 400 && cs->outcome == CAP_CANCELLED, "капча: отказ — вызывающий получил 400, исход «отменено» (не «пройдено»)");
    cap_cb_status = -1;
    net_captcha_test("POST", "/users/@me/relationships", RESP, cap_cb, NULL);
    net_captcha_test_closed();
    check(cs->pending && cs->outcome == CAP_FAILED && strstr(cs->error, "окно закрыто") && cap_cb_status == -1,
          "капча: окно закрыто без ответа — «не пройдена», запрос ещё ждёт (можно открыть снова)");
    net_captcha_cancel();
    cap_cb_status = -1;
    net_captcha_test("POST", "/channels/3001/messages", RESP, cap_cb, NULL);
    char h[512] = "";
    bool ok = net_captcha_test_solve("P1_eyJ0eXAiOiJKV1QifQ.answer", h, sizeof h);
    check(ok && !strcmp(h, "X-Captcha-Key: P1_eyJ0eXAiOiJKV1QifQ.answer|X-Captcha-Rqtoken: rqtok.1|X-Captcha-Session-Id: sess-1") && !cs->pending,
          "капча: после прохождения повтор с X-Captcha-Key / X-Captcha-Rqtoken / X-Captcha-Session-Id");
    static const char EVIL[] = "{\"captcha_key\":[\"x\"],\"captcha_sitekey\":\"a9b5\\r\\nX-Evil: 1\",\"captcha_rqtoken\":\"t\\r\\nX: y\",\"captcha_session_id\":\"s\\u0000\"}";
    cap_cb_status = -1;
    net_captcha_test("POST", "/channels/3001/messages", EVIL, cap_cb, NULL);
    check(!cs->pending && cap_cb_status == 400, "капча: sitekey с переводом строки (подмена заголовков) отвергнут — обычная ошибка 400");
    static const char OTHER[] = "{\"captcha_key\":[\"x\"],\"captcha_service\":\"recaptcha\",\"captcha_sitekey\":\"abc\"}";
    cap_cb_status = -1;
    net_captcha_test("POST", "/channels/3001/messages", OTHER, cap_cb, NULL);
    check(!cs->pending && cap_cb_status == 400, "капча: не hCaptcha — показать нечем, обычная ошибка 400");
    size_t m1 = plat_private_bytes();
    char w[160];
    snprintf(w, sizeof w, "капча: данные проверки освобождены (память %+.0f КБ)", ((double)m1 - (double)m0) / 1024.0);
    check(m1 < m0 + (64u << 10), w);
}

/* ---------- волна 2: серверы, каналы, приглашения (демо, без сети) ---------- */
static void t_wave2(void) {
    size_t m0 = plat_private_bytes();
    char err[160], code[40];
    /* права */
    check(can_manage_channels(2000) && can_manage_guild(2000) && can_manage_roles(2000) && !guild_is_owner(2000) && can_create_invite(3001),
          "права: роль «Управляющий» — управлять каналами/сервером/ролями, приглашения; не владелец");
    check(!can_manage_channels(2100) && !can_manage_guild(2100) && !chan_create(2100, CHK_TEXT, "x", 0, err, sizeof err) && strstr(err, "нет права"),
          "права: на «Другом сервере» прав нет — создание канала отказано до сети");
    Channel *sec = chan_find(3003);
    check(sec && !sec->visible, "права: «Управляющий» не видит скрытый канал (не администратор)");
    /* приглашения: разбор */
    bool p1 = invite_parse("https://discord.gg/AbcDef123", code, sizeof code) && !strcmp(code, "AbcDef123");
    bool p2 = invite_parse("discord.com/invite/abc-DEF?event=1", code, sizeof code) && !strcmp(code, "abc-DEF");
    bool p3 = invite_parse("  <https://www.discordapp.com/invite/Zz9>", code, sizeof code) && !strcmp(code, "Zz9");
    bool p4 = invite_parse("xyz123", code, sizeof code) && !strcmp(code, "xyz123");
    bool p5 = !invite_parse("https://evil.example/../x", code, sizeof code) && !invite_parse("discord.gg/a%2Fb", code, sizeof code) &&
              !invite_parse("", code, sizeof code) && !invite_parse("discord.gg/../../users/@me", code, sizeof code);
    check(p1 && p2 && p3 && p4 && p5, "приглашения: discord.gg/КОД, discord.com/invite/КОД, discordapp, КОД; мусор и пути — отказ");
    void invite_test_reset(void);
    invite_test_reset();
    invite_open("https://discord.gg/abc");
    const InviteState *iv = invite_state();
    check(!iv->busy && !iv->error[0] && !strcmp(iv->guild_name, "Котики 2") && iv->members == 1234 && iv->online == 321 && !iv->joined,
          "приглашение (демо): карточка «Котики 2», 1234 участника / 321 в сети");
    invite_join();
    check(iv->risk_warning && !iv->joined, "вступление: сначала разовое предупреждение о риске, запрос не уходит");
    invite_risk_ack();
    invite_join();
    check(iv->joined && guild_find(2300) != NULL, "вступление (демо): после согласия, сервер появился (GUILD_CREATE)");
    guild_leave(2300);
    invite_open("https://discord.gg/captcha");
    invite_join();
    check(!iv->joined && !iv->need_browser && strstr(iv->error, "30 с"), "вступление: не чаще раза в 30 с");
    invite_test_reset();
    invite_risk_ack();
    invite_open("https://discord.gg/captcha");
    invite_join();
    check(iv->need_browser && strstr(iv->error, "браузер") && invite_open_browser(), "вступление: Discord просит капчу -> «вступить через браузер», ссылка discord.gg/КОД");
    check(!open_url_browser("https://evil.example/x") && !open_url_browser("https://discord.gg/a b") && !open_url_browser("https://discord.gg/../../x") &&
          !open_url_browser("file:///C:/Windows/notepad.exe") && !open_url_browser("https://discord.gg/"), "открыть в браузере: только https://discord.gg/КОД и discord.com/invite/КОД");
    {   /* сервер 2300 уже покинут выше: вернуть для проверки «вы уже на сервере» */
        invite_test_reset(); invite_risk_ack();
        invite_open("abc"); invite_join();
    }
    invite_open("abc");
    check(iv->joined, "приглашение на сервер, где вы уже есть — «вы уже на сервере»");
    invite_open("xyz");
    check(iv->error[0] && strstr(iv->error, "недействительно"), "приглашение «xyz» — «недействительно или истекло»");
    invite_open("429");
    check(strstr(iv->error, "слишком часто") != NULL, "приглашение «429» — «слишком часто»");
    invite_close();
    invite_create(3001, 86400, 5);
    check(invite_created() && !strncmp(invite_created(), "https://discord.gg/", 19), "создать приглашение (демо): ссылка https://discord.gg/...");
    /* сервер: создать, изменить, описание, удалить, покинуть */
    int ng = S.nguilds;
    bool c1 = guild_create("mincord-temp", "", err, sizeof err);
    Guild *ng_ = c1 && S.nguilds == ng + 1 ? &S.guilds[S.nguilds - 1] : NULL;
    check(ng_ && ng_->owner_id == S.me_id && guild_is_owner(ng_->id) && !guild_create("", "", err, sizeof err) &&
          !guild_create("a\x1b[31m", "", err, sizeof err), "сервер: создан (демо), вы владелец; пустое имя и ESC в имени — отказ");
    u64 tg = ng_ ? ng_->id : 0;
    check(tg && guild_edit(tg, "mincord-temp2", "описание", NULL, NULL, err, sizeof err) && !strcmp(guild_find(tg)->name, "mincord-temp2") &&
          !strcmp(guild_description(tg), "описание"), "сервер: изменены имя и описание");
    check(!guild_edit(tg, NULL, NULL, "Z:/нет.png", NULL, err, sizeof err) && strstr(err, "не открывается"), "значок: нет файла — отказ до сети");
    const char *d = guild_description(2000);
    check(d && strstr(d, "Уютный"), "описание сервера (демо)");
    check(!guild_delete(tg, "mincord-temp", err, sizeof err) && strstr(err, "точно") && guild_find(tg), "удаление: имя введено неточно — отказ");
    check(!guild_delete(2000, "Уютный уголок", err, sizeof err) && strstr(err, "владелец"), "удаление чужого сервера — отказ (только владелец)");
    check(guild_delete(tg, "mincord-temp2", err, sizeof err) && !guild_find(tg), "удаление своего сервера по точному имени (демо)");
    guild_leave(2300);
    check(!guild_find(2300), "покинуть сервер (демо)");
    invite_test_reset();
    /* роли с именами */
    const RoleName *rn;
    int nr = guild_roles(2000, &rn);
    check(nr == 4 && role_name(2000, 2001) && !strcmp(role_name(2000, 2001), "Админ"), "роли сервера с именами (для окна «Права»): 4, «Админ»");
    /* каналы */
    int nc = S.nchans;
    bool cc = chan_create(2000, CHK_TEXT, "новый-канал", 3000, err, sizeof err);
    Channel *nch = cc && S.nchans == nc + 1 ? &S.chans[S.nchans - 1] : NULL;
    check(nch && nch->guild_id == 2000 && nch->parent_id == 3000 && nch->type == CH_TEXT && nch->visible, "канал: создан в категории (демо)");
    check(!chan_create(2000, CHK_TEXT, "x", 3001, err, sizeof err) && strstr(err, "категория"), "канал: родитель не категория — отказ");
    u64 nid = nch ? nch->id : 0;
    ChanEdit ce = { "переименован", NULL, -1, -1, -1, -1 };
    check(nid && chan_edit(nid, &ce, err, sizeof err) && !strcmp(chan_find(nid)->name, "переименован"), "канал: переименован");
    ChanEdit bad = { NULL, NULL, 99999, -1, -1, -1 };
    check(!chan_edit(nid, &bad, err, sizeof err) && strstr(err, "6 часов"), "канал: медленный режим больше 6 часов — отказ");
    ChanEdit info;
    check(chan_info(3005, &info) && info.slowmode == 10 && info.bitrate == 64000, "настройки канала для «Изменить канал» (демо): медленный режим 10 с");
    chan_perm_set(nid, 2000, OW_ROLE, 0, P_VIEW_CHANNEL);
    bool hid = !chan_find(nid)->visible;
    chan_perm_clear(nid, 2000);
    check(hid && chan_find(nid)->visible, "права канала: запрет VIEW для @everyone скрывает канал, снятие — возвращает");
    chan_move(nid, 0, 7);
    check(chan_find(nid)->parent_id == 0 && chan_find(nid)->position == 7, "канал: перемещён (вне категории, позиция 7)");
    {   /* перестановка соседей (Alt+Up/Down): обмен позициями внутри категории, порядок меняется */
        Channel *a = chan_find(3004), *b2 = chan_find(3002);   /* музыка (1), между ними скрытый «секретный» (2), новости (3) */
        int pb = b2 ? b2->position : -1;
        chan_swap(3004, 1);                      /* Alt+Down: сосед — ближайший ВИДИМЫЙ (новости), скрытый не в счёт */
        bool moved = a && chan_find(3004)->position > chan_find(3002)->position;
        chan_swap(3004, -1);
        check(moved && chan_find(3004)->position < chan_find(3002)->position && pb >= 0, "перестановка каналов: обмен с соседом (нормализация позиций), обратно");
    }
    {   /* лимиты в символах (кириллица — 2 байта): тема 1024 символа и имя 100 символов принимаются */
        Buf t = {0};
        for (int i = 0; i < 1024; i++) buf_puts(&t, "я");
        ChanEdit te = { NULL, t.p, -1, -1, -1, -1 };
        bool ok1024 = chan_edit(nid, &te, err, sizeof err);
        buf_puts(&t, "я");
        te.topic = t.p;
        bool no1025 = !chan_edit(nid, &te, err, sizeof err) && strstr(err, "1024");
        buf_free(&t);
        check(ok1024 && no1025, "тема канала: 1024 символа кириллицей (2048 байт) принимается, 1025 — отказ");
    }
    {   /* права по маске: биты вне формы сохраняются (allow 1<<15, deny «упоминать @everyone» 1<<17) */
        chan_perm_set(nid, 2000, OW_ROLE, P_SEND_MESSAGES | (1ULL << 15), 1ULL << 17);
        chan_perm_edit(nid, 2000, OW_ROLE, P_VIEW_CHANNEL, 0, P_VIEW_CHANNEL);
        u64 al = 0, de = 0;
        check(chan_overwrite(nid, 2000, &al, &de, NULL) && (al & (1ULL << 15)) && (al & P_SEND_MESSAGES) && (de & P_VIEW_CHANNEL) && (de & (1ULL << 17)),
              "права канала по маске: изменён только бит формы, остальные биты перезаписи сохранены");
        chan_perm_clear(nid, 2000);
    }
    chan_delete(nid);
    check(!chan_find(nid) && S.nchans == nc, "канал: удалён (демо)");
    size_t m1 = plat_private_bytes();
    char w[200];
    snprintf(w, sizeof w, "волна 2: память после проверок %+.0f КБ (всё по запросу)", ((double)m1 - (double)m0) / 1024.0);
    check(m1 < m0 + (128u << 10), w);
}

/* Исходящий звонок: ring уходит только после CALL_CREATE (или через 5 с), не сразу за op 4. */
static int ring_seen;
static bool ring_guard(const char *method, const char *path) {
    if (!strcmp(method, "POST") && strstr(path, "/call/ring")) ring_seen++;
    return false;                               /* в сеть ничего не уходит */
}
static void t_call_ring(void) {
    O.demo = false;
    g_http_guard = ring_guard;
    ring_seen = 0;
    call_start(4000);                           /* ЛС с Алисой; gateway не подключён — op 4 никуда не уходит */
    bool waits = call_ring_pending() == 4000 && ring_seen == 0;
    ev("CALL_CREATE", "{\"channel_id\":\"4001\",\"ringing\":[\"1002\"],\"voice_states\":[]}");   /* чужая ЛС — не наше */
    bool other = ring_seen == 0 && call_ring_pending() == 4000;
    ev("CALL_CREATE", "{\"channel_id\":\"4000\",\"ringing\":[\"1001\"],\"voice_states\":[]}");
    bool rang = ring_seen == 1 && call_ring_pending() == 0;
    voice_leave();
    check(waits && other && rang, "исходящий звонок: ring не сразу за op 4, а по CALL_CREATE своей ЛС (раньше ring мог опередить создание звонка)");
    /* положили трубку до CALL_CREATE — звонка нет ни по событию, ни по таймеру */
    ring_seen = 0;
    call_start(4000);
    voice_leave();
    store_tick();
    ev("CALL_CREATE", "{\"channel_id\":\"4000\",\"ringing\":[\"1001\"],\"voice_states\":[]}");
    check(ring_seen == 0 && call_ring_pending() == 0, "исходящий звонок: трубку положили до CALL_CREATE — ring не уходит (ни по событию, ни через 5 с)");
    g_http_guard = NULL;
    O.demo = true;
}

/* ---------- пакет А: заглушить/уведомления, «ожидает» и повтор, фильтры поиска, ссылки CDN ---------- */
static int pa_patch, pa_post;
static char pa_body[600];
static bool pa_guard(const char *method, const char *path) {
    if (!strcmp(method, "PATCH") && strstr(path, "/settings")) pa_patch++;
    if (!strcmp(method, "POST") && strstr(path, "/messages")) pa_post++;
    (void)pa_body;
    return false;                               /* в сеть ничего не уходит */
}
/* Ревью 9 К1 / А1: 150 сообщений со старыми ссылками, 100 кадров (с прокруткой) — не больше 3 запросов
 * refresh-urls; подделка «[файл: x] url» в тексте — не вложение. Демо имитирует сеть (ответ через 400 мс). */
static void t_cdn(void) {
    bool was_demo = O.demo;
    O.demo = true;
    u64 was = S.open_ch;
    ui_open_channel(3001);
    Buf b = {0};
    buf_puts(&b, "[");
    for (int i = 149; i >= 0; i--)
        buf_printf(&b, "%s{\"id\":\"%d\",\"channel_id\":\"3001\",\"author\":{\"id\":\"1002\",\"username\":\"bob\"},\"content\":\"\","
                   "\"attachments\":[{\"filename\":\"f%d.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/%d/f.png?ex=5f000000&is=1&hm=ab\"}],"
                   "\"timestamp\":\"2026-11-09T10:00:00+00:00\"}", i == 149 ? "" : ",", 880000 + i, i, i);
    buf_puts(&b, "]");
    cJSON *j = json_parse(b.p, b.len);
    if (cJSON_IsArray(j)) store_set_messages(j, false);
    json_done();
    buf_free(&b);
    bool marked = S.nmsgs >= 100 && S.msgs[S.nmsgs - 1].content[0] == ATT_MARK;   /* лента держит 100 последних */
    int r0 = cdn_requests;
    i64 skew0 = g_clock_skew;
    Buf r = {0};
    for (int f = 0; f < 100; f++) {             /* 100 кадров без прокрутки = 10 с «времени» */
        ui_snapshot(120, 34, &r); buf_free(&r);
        store_tick();
        g_clock_skew += 100;
    }
    int sent = cdn_requests - r0;
    r0 = cdn_requests;
    for (int f = 0; f < 100; f++) {             /* ещё 100 кадров с прокруткой (PgUp раз в секунду) */
        ui_snapshot(120, 34, &r); buf_free(&r);
        store_tick();
        g_clock_skew += 100;
        if (f % 10 == 9) key(K_PGUP, 0);
    }
    int sent2 = cdn_requests - r0;
    const char *u0 = "https://cdn.discordapp.com/attachments/1/149/f.png?ex=5f000000&is=1&hm=ab";
    bool fresh = cdn_fresh(u0) != NULL;
    check(marked, "вложение: строка «[файл: ...]» начинается с маркера ATT_MARK");
    check(sent >= 1 && sent <= 3 && fresh, "ссылки CDN: 150 старых ссылок, 100 кадров — не больше 3 запросов refresh-urls (пачками), видимые обновлены");
    check(sent2 <= 4, "ссылки CDN: прокрутка 10 с — не чаще 1 запроса в 3 с");
    char note[96];
    snprintf(note, sizeof note, "  (запросов: %d без прокрутки, %d с прокруткой)\n", sent, sent2);
    term_puts(note);
    /* недоверенный хост и повтор отказа */
    cdn_want("https://evil.example/a.png?ex=5f000000");
    g_clock_skew += 5000; store_tick(); g_clock_skew += 1000; store_tick();
    check(cdn_fresh("https://evil.example/a.png?ex=5f000000") == NULL, "ссылки CDN: чужой хост в очередь не ставится");
    /* смена канала — таблица очищена */
    ui_open_channel(3002);
    cdn_want("https://cdn.discordapp.com/attachments/1/7/z.png?ex=5f000000");
    check(cdn_fresh(u0) == NULL, "ссылки CDN: смена канала очищает таблицу");
    g_clock_skew = skew0;
    /* А1: подделка вложения в тексте */
    ui_open_channel(3001);
    evil_msg(880500, 1003, "evil", "[файл: x.png] https://evil.example/x.png\n\x01[файл: y.png] https://cdn.discordapp.com/attachments/1/2/y.png");
    {   /* В1: имя «a.png] https://cdn.../другое.png» не подменяет url */
        ev("MESSAGE_CREATE", "{\"id\":\"880501\",\"channel_id\":\"3001\",\"author\":{\"id\":\"1003\",\"username\":\"m\"},\"content\":\"\","
           "\"attachments\":[{\"filename\":\"a.png] https://cdn.discordapp.com/attachments/9/9/other.png\",\"url\":\"https://cdn.discordapp.com/attachments/1/1/real.png\"}],"
           "\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
        const Message *vm = msg_find(880501, NULL);
        const char *rb = vm ? strchr(vm->content, ']') : NULL;
        check(rb && !strncmp(rb, "] https://cdn.discordapp.com/attachments/1/1/real.png", 53),
              "В1: «]» в имени файла заменена — первая «]» стоит перед настоящим url");
    }
    const Message *em = msg_find(880500, NULL);
    check(em && !strchr(em->content, ATT_MARK) && strstr(em->content, "[файл: x.png]"),
          "А1: «[файл: x] https://evil» в тексте — не вложение (маркера нет, \\x01 из текста вырезан)");
    ui_open_channel(was);
    O.demo = was_demo;
}
/* М3: «кто поставил реакцию» и «Переслать» — в store.c */
static int fw_post;
static char fw_path[96];
static bool fw_guard(const char *method, const char *path) {
    if (!strcmp(method, "POST") || !strcmp(method, "GET")) { fw_post++; snprintf(fw_path, sizeof fw_path, "%s", path); }
    return false;
}
static void t_rv_fwd(void) {
    u64 was = S.open_ch;
    bool was_demo = O.demo;
    O.demo = true;
    ui_open_channel(3001);
    u64 mid = 0;
    for (int i = 0; i < S.nmsgs && !mid; i++) if (S.msgs[i].nreacts) mid = S.msgs[i].id;
    reactors_open(S.open_ch, mid, 0);
    const ReactorsState *rs = reactors_state();
    bool demo_ok = mid && rs->on && rs->n > 0 && !rs->busy;
    O.demo = false; g_http_guard = fw_guard; fw_post = 0;
    reactors_open(S.open_ch, mid, 0);
    bool get_ok = fw_post == 1 && strstr(fw_path, "/reactions/") && strstr(fw_path, "limit=25") && rs->busy;
    reactors_close();
    fw_post = 0;
    msg_forward(3002, 3001, mid);
    bool fw_ok = fw_post == 1 && !strcmp(fw_path, "/channels/3002/messages");
    g_http_guard = NULL;
    O.demo = was_demo;
    check(demo_ok && get_ok && !rs->on, "М3: reactors_open (демо — имена, сеть — GET .../reactions/..?limit=25), reactors_close");
    check(fw_ok, "М3: msg_forward — POST /channels/{цель}/messages");
    ui_open_channel(was);
}
void chan_remove_test(u64 id);
/* Очередь 1: статус со сроком, текстовый статус, игнор, заметка, правила сервера (демо) */
static void t_queue1(void) {
    bool was_demo = O.demo;
    O.demo = true;
    char err[160] = "";
    /* №103/104 */
    presence_set_for(PS_DND, 60);
    bool a1 = presence_until() > wall_ms() / 1000 + 59 * 60;
    bool a2 = custom_status_set("на \x1b[31mпаре", 30, err, sizeof err) && !strcmp(custom_status(), "на паре") && custom_status_until() > 0;
    char lng[700]; memset(lng, 'x', sizeof lng - 1); lng[sizeof lng - 1] = 0;
    bool a3 = !custom_status_set(lng, 0, err, sizeof err);
    g_clock_skew += 0;
    custom_status_set("", 0, err, sizeof err);
    presence_set_for(PS_ONLINE, 0);
    check(a1 && a2 && a3 && !*custom_status() && !presence_until(), "№103/104: статус на срок, текстовый статус (очищен, <= 128), снятие");
    /* №108 игнор */
    ignore_set(1002, true);
    int m0 = chan_find(4001) ? chan_find(4001)->mentions : 0;
    ev("MESSAGE_CREATE", "{\"id\":\"885000\",\"channel_id\":\"4001\",\"author\":{\"id\":\"1002\",\"username\":\"bob\"},\"content\":\"эй\",\"timestamp\":\"2026-11-10T10:00:00+00:00\"}");
    bool g1 = ignored(1002) && (!chan_find(4001) || chan_find(4001)->mentions == m0);
    ignore_set(1002, false);
    check(g1 && !ignored(1002), "№108 игнор: только у себя, упоминания игнорируемого не считаются, снятие");
    /* №100 заметка */
    note_open(1001);
    bool n1 = strstr(note_state()->text, "котов") != NULL;
    bool n2 = note_set(1001, "новая \x1b[2Jзаметка", err, sizeof err) && !strcmp(note_state()->text, "новая заметка") && !note_set(S.me_id, "x", err, sizeof err);
    char n300[400]; memset(n300, 'z', 300); n300[300] = 0;
    bool n3 = !note_set(1001, n300, err, sizeof err);
    note_close();
    check(n1 && n2 && n3, "№100 заметка: чтение, запись (очищена, <= 256), не о себе");
    /* №128 правила */
    Guild *pg = guild_find(2500);
    bool r1 = pg && pg->pending;
    u64 was = S.open_ch;
    ui_open_channel(3500);
    int nb = S.nmsgs;
    msg_send(3500, "привет", 0, 0);
    bool r2 = S.nmsgs == nb;                    /* не ушло: правила не приняты */
    guild_rules_open(2500);
    bool r3 = guild_rules_state()->n == 3 && guild_rules_state()->version && guild_rules_state()->version[0];
    bool r4 = guild_rules_accept(2500, err, sizeof err) && !guild_find(2500)->pending;
    guild_rules_close();
    guild_find(2500)->pending = true;           /* вернуть демо как было (для снимков) */
    ui_open_channel(was);
    check(r1 && r2 && r3 && r4, "№128 правила сервера: pending из READY, писать нельзя, правила (TERMS), принять — снимает");
    O.demo = was_demo;
}
/* №65/67 отправка файла со спойлером и описанием (демо: в ленту) */
static void t_send_ex(void) {
    bool was_demo = O.demo;
    u64 was = S.open_ch;
    O.demo = true;
    ui_open_channel(3001);
    char path[600], err[160] = "";
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    snprintf(path, sizeof path, "%s/astra-spoiler.png", tmp);
    FILE *f = fopen(path, "wb");
    if (f) { fputs("x", f); fclose(f); }
    bool fs = plat_file_size(path) == 1 && plat_file_size(tmp) == -1 && plat_file_size("//evil.example/share/x.png") == -1 && plat_file_size("Z:/нет/x") == -1;
    bool ok = fs && send_file_ex(3001, path, "подпись", true, "описание \x1b[2Jкартинки", err, sizeof err);
    const Message *lm = S.nmsgs ? &S.msgs[S.nmsgs - 1] : NULL;
    bool sp = ok && lm && strstr(lm->content, "[файл: SPOILER_");
    remove(path);
    sp = sp && text_unsafe("C:/aâ®b.png") && text_unsafe("/tmp/ab") && !text_unsafe("/tmp/фото 1.png");   /* D-4 */
    check(sp, "№65/67: send_file_ex — SPOILER_, описание (alt) очищается; путь броска с управляющими/BiDi — отказ");
    ui_open_channel(was);
    O.demo = was_demo;
}
/* №57 слеш-команды и №133/117/147 модерация */
void cmds_test_resp(long status, const char *body);
static char sl_body_path[160];
static int sl_n;
static bool sl_guard(const char *method, const char *path) { sl_n++; snprintf(sl_body_path, sizeof sl_body_path, "%s %s", method, path); return false; }
static void t_slash_mod(void) {
    bool was_demo = O.demo;
    u64 was = S.open_ch;
    O.demo = true;
    ui_open_channel(3004);
    cmds_open(3004);
    const SlashIndex *cx = cmds_state();
    int m[8];
    int nm = cmds_match("q", m, 8);
    bool s1 = cx->n == 5 && nm == 2 && !strcmp(cmds_str(cx->c[m[0]].name), "queue show") && !strcmp(cmds_str(cx->c[0].app), "MemeBot");
    char err[160] = "";
    int ip = -1, iav = -1, imeme = -1;
    for (int i = 0; i < cx->n; i++) {
        if (!strcmp(cmds_str(cx->c[i].name), "play")) ip = i;
        if (!strcmp(cmds_str(cx->c[i].name), "avatar")) iav = i;
        if (!strcmp(cmds_str(cx->c[i].name), "meme")) imeme = i;
    }
    const char *v1[] = { "", "50", "да" };
    const char *v2[] = { "lofi", "много", "" };
    const char *v3[] = { "lofi", "70", "может" };
    const char *v4[] = { "Код" };
    const char *v5[] = { "Нет Такого" };
    bool s2 = ip >= 0 && !cmd_run(ip, v1, 3, err, sizeof err) && strstr(err, "обязательно") &&
              !cmd_run(ip, v2, 3, err, sizeof err) && strstr(err, "число") && !cmd_run(ip, v3, 3, err, sizeof err) && strstr(err, "да или нет");
    bool s3 = imeme >= 0 && cmd_run(imeme, v4, 1, err, sizeof err) && iav >= 0 && !cmd_run(iav, v5, 1, err, sizeof err);
    /* сеть: подставной индекс с подкомандой в группе и лишним (вложение) */
    O.demo = false; g_http_guard = sl_guard; sl_n = 0;
    cmds_close();
    cmds_open(3004);
    bool s4 = sl_n == 1 && strstr(sl_body_path, "/guilds/2000/application-command-index");
    cmds_test_resp(200, "{\"applications\":[{\"id\":\"9\",\"name\":\"Bot\\u001b[2J\",\"icon\":\"x\",\"bot\":{\"big\":1}}],\"version\":\"1\",\"application_commands\":["
        "{\"id\":\"1\",\"application_id\":\"9\",\"version\":\"2\",\"type\":1,\"name\":\"cfg\",\"description\":\"d\",\"dm_permission\":true,\"options\":["
        "{\"type\":2,\"name\":\"set\",\"description\":\"g\",\"options\":[{\"type\":1,\"name\":\"color\",\"description\":\"s\",\"options\":[{\"type\":3,\"name\":\"value\",\"description\":\"v\",\"required\":true}]}]}]},"
        "{\"id\":\"2\",\"application_id\":\"9\",\"version\":\"3\",\"type\":1,\"name\":\"up\",\"description\":\"d\",\"options\":[{\"type\":11,\"name\":\"file\",\"description\":\"f\",\"required\":true}]},"
        "{\"id\":\"3\",\"application_id\":\"9\",\"version\":\"4\",\"type\":2,\"name\":\"ctx\"}]}");
    cx = cmds_state();
    bool s5 = cx->n == 2 && !strcmp(cmds_str(cx->c[0].name), "cfg set color") && !strcmp(cmds_str(cx->c[0].app), "Bot") &&
              !cx->c[1].usable && cx->c[0].usable;
    const char *v6[] = { "красный" };
    sl_n = 0;
    bool s6 = cmd_run(0, v6, 1, err, sizeof err) && sl_n == 1 && strstr(sl_body_path, "POST /interactions") && !cmd_run(1, v6, 1, err, sizeof err);
    /* С-2: числа — только десятичные, конечные, |d| <= 2^53 */
    O.demo = true;
    cmds_close();
    cmds_open(3004);
    int ipl = -1;
    for (int i = 0; i < cmds_state()->n; i++) if (!strcmp(cmds_str(cmds_state()->c[i].name), "play")) ipl = i;
    const char *b1[] = { "x", "inf", "" }, *b2[] = { "x", "0x10", "" }, *b3[] = { "x", "1e300", "" }, *b4[] = { "x", "nan", "" }, *b5[] = { "x", "42", "нет" };
    bool s8 = ipl >= 0 && !cmd_run(ipl, b1, 3, err, sizeof err) && !cmd_run(ipl, b2, 3, err, sizeof err) && !cmd_run(ipl, b3, 3, err, sizeof err) &&
              !cmd_run(ipl, b4, 3, err, sizeof err) && cmd_run(ipl, b5, 3, err, sizeof err) && !strcmp(cmds_str(cmds_state()->c[ipl].base), "play");
    O.demo = false;
    cmds_close();
    bool s7 = s8 && !cmds_state()->n && !cmds_state()->s;
    /* модерация: права, ограничения, запросы */
    sl_n = 0;
    bool m1 = mod_can(2000, MOD_KICK) == ((guild_find(2000)->owner_id == S.me_id) || mod_can(2000, MOD_KICK));
    bool m2 = !mod_kick(2000, guild_find(2000)->owner_id, "x", err, sizeof err) && !mod_ban(2000, S.me_id, NULL, 0, err, sizeof err);
    bool m3 = !mod_timeout(2000, 1002, 29 * 24 * 60, NULL, err, sizeof err);
    char rs[600]; memset(rs, 'a', sizeof rs - 1); rs[sizeof rs - 1] = 0;
    bool m4 = !mod_kick(2000, 1002, rs, err, sizeof err);
    bool can_k = mod_can(2000, MOD_KICK), can_r = mod_can(2000, MOD_ROLES);
    sl_n = 0;
    bool m5 = !can_k || (mod_kick(2000, 1002, "спам", err, sizeof err) && sl_n == 1 && strstr(sl_body_path, "DELETE /guilds/2000/members/1002"));
    sl_n = 0;
    bool m6 = !can_r || (mod_role(2000, 1002, 2001, true, err, sizeof err) && strstr(sl_body_path, "PUT /guilds/2000/members/1002/roles/2001") &&
                         !mod_role(2000, 1002, 2004, true, err, sizeof err));   /* 2001 (поз. 3) ниже моей 2004 (поз. 4); свою — нельзя */
    bool m7 = !mod_voice_move(2000, 1002, 3001, err, sizeof err);   /* не голосовой */
    {   /* М-1: русская причина в 512 символов доходит заголовком (после url_encode ~ 9 КБ не бывает: 512×2×3 < 8 КБ) */
        char rr[1100] = "";
        for (int i = 0; i < 500; i++) strcat(rr, "ж");
        sl_n = 0;
        bool hdr_ok = !can_k || (mod_kick(2000, 1002, rr, err, sizeof err) && sl_n == 1);
        m7 = m7 && hdr_ok;
    }
    g_http_guard = NULL; O.demo = true;
    char d[200];
    snprintf(d, sizeof d, "  (слеш: %d %d %d %d %d %d %d; мод: %d %d %d %d %d %d %d; права кик=%d роли=%d)\n", s1, s2, s3, s4, s5, s6, s7, m1, m2, m3, m4, m5, m6, m7, can_k, can_r);
    term_puts(d);
    check(s1 && s2 && s3 && s4 && s5 && s6 && s7, "№57 слеш-команды: индекс по требованию (отсев по схеме), подкоманды, проверка опций, неподдерживаемые — нельзя, POST /interactions, закрытие освобождает");
    check(m1 && m2 && m3 && m4 && m5 && m6 && m7, "№133/117/147 модерация: права, не к владельцу/себе, пределы, причина <= 512, запрос с причиной, роли не выше своей, перенос только в голосовой");
    ui_open_channel(was);
    O.demo = was_demo;
}
/* Пакет В, часть 2: уведомления, ветки (архив/правка/пост/участники), группа и свой профиль, демо-ЛС */
static char nt_title[160], nt_text[260];
static int nt_n;
static void nt_hook(const char *t, const char *x) { snprintf(nt_title, sizeof nt_title, "%s", t); snprintf(nt_text, sizeof nt_text, "%s", x); nt_n++; }
static void t_packv2(void) {
    bool was_demo = O.demo;
    u64 was = S.open_ch;
    O.demo = true;
    char err[160] = "";
    /* №123/124 */
    g_notify_hook = nt_hook; nt_n = 0;
    notify_set(NOTIFY_PLAIN);
    ev("MESSAGE_CREATE", "{\"id\":\"884000\",\"channel_id\":\"4000\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"секретный текст\",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    bool n1 = nt_n == 1 && !strcmp(nt_title, "Astra DS") && !strcmp(nt_text, "новое сообщение");   /* без имён (диск) */
    notify_set(NOTIFY_TEXT);
    ev("MESSAGE_CREATE", "{\"id\":\"884001\",\"channel_id\":\"4000\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"текст \\u001b[31mвидно\",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    bool n2 = nt_n == 2 && !strcmp(nt_text, "текст видно") && strstr(nt_title, "Алиса");
    streamer_set(true);
    ev("MESSAGE_CREATE", "{\"id\":\"884002\",\"channel_id\":\"4000\",\"author\":{\"id\":\"1001\",\"username\":\"alice\",\"global_name\":\"Алиса\"},\"content\":\"при стриме\",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    bool n3 = nt_n == 3 && !strstr(nt_text, "стриме") && !strstr(nt_title, "Алиса");
    streamer_set(false);
    ev("MESSAGE_CREATE", "{\"id\":\"884003\",\"channel_id\":\"3002\",\"guild_id\":\"2000\",\"author\":{\"id\":\"1001\",\"username\":\"alice\"},\"content\":\"просто так\",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    bool n4 = nt_n == 3;                        /* не упоминание — без уведомления */
    notify_set(NOTIFY_OFF);
    ev("MESSAGE_CREATE", "{\"id\":\"884004\",\"channel_id\":\"4000\",\"author\":{\"id\":\"1001\",\"username\":\"alice\"},\"content\":\"выкл\",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    bool n5 = nt_n == 3;
    notify_set(NOTIFY_PLAIN);
    g_notify_hook = NULL;
    check(n1 && n2 && n3 && n4 && n5, "№123 уведомления: по умолчанию «Astra DS: новое сообщение» без имён, с текстом (очищен) — по выбору, стример — без имени, не упоминание и «выкл» — молча");
    /* №93/94/96/97 */
    threads_open(0);
    threads_open(2000);
    threads_archived_open(3001);
    bool t1 = forum_state()->n == 1 && forum_state()->p[0].archived;
    forum_close();
    bool t2 = thread_edit(3101, "коты и кошки", -1, 1, err, sizeof err) && !strcmp(chan_find(3101)->name, "коты и кошки") && (chan_find(3101)->tflags & TF_LOCKED);
    thread_edit(3101, NULL, -1, 0, err, sizeof err);
    bool t3 = !thread_edit(3101, "", -1, -1, err, sizeof err);
    bool t4 = forum_post_create(3020, "Новый вопрос", "Как включить рацию?", err, sizeof err) && !forum_post_create(3001, "x", "y", err, sizeof err) &&
              !forum_post_create(3020, "x", "", err, sizeof err);
    thread_members_open(3101);
    bool t5 = thread_members_state()->n == 3;
    thread_members_close();
    check(t1 && t2 && t3 && t4 && t5 && !thread_members_state()->n, "№93/94/96/97 ветки: архив, правка (имя, закрыть), новый пост форума, участники");
    /* №112/113 */
    bool g1 = gdm_rename(4004, "Поход в горы 2", err, sizeof err) && !strcmp(chan_find(4004)->name, "Поход в горы 2") && !gdm_rename(4000, "x", err, sizeof err);
    char *old = xstrdup(S.me_name ? S.me_name : "");
    bool g2 = profile_me_set("Новое Имя", "Люблю котов", err, sizeof err) && !strcmp(S.me_name, "Новое Имя") &&
              !profile_me_set("", NULL, err, sizeof err) && !profile_me_set("a\x1b[2J", NULL, err, sizeof err);
    profile_me_set(old, NULL, err, sizeof err);
    free(old);
    check(g1 && g2, "№112/113: имя группы ЛС, свой профиль (имя 1..32 без управляющих, «о себе»)");
    /* демо-ЛС для №109/111 */
    bool d1 = chan_find(4002) && (chan_find(4002)->dmflags & DMF_REQUEST) && chan_find(4003) && (chan_find(4003)->dmflags & DMF_SPAM) &&
              gdm_owner(4004) == S.me_id && gdm_owner(4005) != S.me_id && chan_find(4004)->nrecip == 3;
    check(d1, "демо: запрос на переписку, спам, группа (я владелец, 4 человека) и группа, где не владелец");
    ui_open_channel(was);
    O.demo = was_demo;
}
/* Пакет В: фильтр слов, фокус, стример, статус голосового, баннер, рация, авточувствительность, удаление ветки (демо) */
static void t_packv(void) {
    bool was_demo = O.demo;
    u64 was = S.open_ch;
    O.demo = true;
    filter_words_set("спойлер, финал");
    Message fm = {0};
    fm.author_id = 1001;
    fm.content = (char *)"Кто видел ФИНАЛ?";
    bool f1 = msg_filtered(&fm);
    fm.content = (char *)"полуфинал был ок";
    bool f2 = !msg_filtered(&fm);
    filter_words_set("");
    check(f1 && f2 && !*filter_words(), "№120 фильтр слов: целое слово без учёта регистра, выключение");
    /* №121 фокус: слово-тревога не считается упоминанием, ЛС — да */
    alert_words_set("релиз");
    focus_set(30);
    int m0 = chan_find(3006)->mentions;
    ev("MESSAGE_CREATE", "{\"id\":\"883000\",\"channel_id\":\"3006\",\"guild_id\":\"2000\",\"author\":{\"id\":\"1002\",\"username\":\"bob\"},\"content\":\"релиз завтра\",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    bool q1 = chan_find(3006)->mentions == m0 && focus_until() > wall_ms() / 1000 + 29 * 60;
    g_clock_skew += 31 * 60 * 1000;             /* монотонные часы; фокус — по настенным: проверяем выключение */
    focus_set(0);
    g_clock_skew -= 31 * 60 * 1000;
    alert_words_set("");
    check(q1 && !focus_until(), "№121 режим фокуса: слова-тревоги не уведомляют, выключение");
    streamer_set(true);
    bool s1 = streamer_on();
    streamer_set(false);
    check(s1 && !streamer_on(), "№125 режим стримера: флаг");
    /* №151 статус голосового */
    char err[120] = "";
    bool v1 = voice_channel_status_set(3011, "играем \x1b[31mв шахматы", err, sizeof err) && !strcmp(voice_channel_status(3011), "играем в шахматы");
    bool v2 = !voice_channel_status_set(3001, "x", err, sizeof err);
    ev("VOICE_CHANNEL_STATUS_UPDATE", "{\"id\":\"3012\",\"guild_id\":\"2000\",\"status\":\"стрим\"}");
    ev("CHANNEL_INFO", "{\"guild_id\":\"2000\",\"channels\":[{\"id\":\"3011\",\"status\":\"\"}]}");
    bool v3 = !strcmp(voice_channel_status(3012), "стрим") && !*voice_channel_status(3011);
    check(v1 && v2 && v3, "№151 статус голосового канала: задать (демо), только голосовой, события VOICE_CHANNEL_STATUS_UPDATE / CHANNEL_INFO");
    /* №4 баннер — цветом */
    ev("GUILD_UPDATE", "{\"id\":\"2000\",\"banner\":\"0123456789abcdef0123456789abcdef\"}");
    bool b1 = guild_find(2000)->banner_color != 0;
    ev("GUILD_UPDATE", "{\"id\":\"2000\",\"banner\":null}");
    check(b1 && !guild_find(2000)->banner_color, "№4 баннер сервера: полоса цвета по хэшу (картинка не грузится), снятие");
    /* №141/142 */
    voice_ptt_bind(0x05);
    char kn[64];
    plat_key_name(0x05, kn, sizeof kn);
    bool p1 = voice_ptt_bound() == 5;
    voice_ptt_bind(0);
    voice_vad_auto(true);
    bool a1 = voice_vad_is_auto() && voice_vad_db() >= -70;
    voice_vad_auto(false);
#ifdef _WIN32
    p1 = p1 && strstr(kn, "кнопка 4");
#endif
    check(p1 && a1 && !voice_vad_is_auto() && term_fullscreen(-1) == -1, "№141/142/16: клавиша рации (имя), авточувствительность вкл/выкл, полный экран без окна — -1");
    /* демо «!delthread»: ветка удаляется через 3 с, открытая — помечена */
    threads_open(0);
    threads_open(2000);
    ui_open_channel(3101);
    msg_send(3101, "проверка !delthread", 0, 0);
    g_clock_skew += 3100;
    store_tick();
    g_clock_skew -= 3100;
    Channel *dt = chan_find(3101);
    check(dt && (dt->tflags & TF_DELETED) && !dt->can_send, "демо «!delthread»: открытая ветка через 3 с помечена удалённой, ввод закрыт");
    ui_open_channel(was);
    O.demo = was_demo;
}
/* №138/149 групповой звонок и переход между каналами; №69 голосовые (демо) */
static void t_calls_vm(void) {
    bool was_demo = O.demo;
    u64 was = S.open_ch;
    O.demo = true;
    /* №149: переход в другой голосовой канал того же сервера — без выхода */
    voice_join(2000, 3011);
    int rs0 = voice_restarts;
    VoiceStatus v1, v2;
    for (int i = 0; i < 50; i++) { voice_status(&v1); if (v1.phase == VS_CONNECTED) break; Waitable w = wake_waitable(g_wake); plat_wait(&w, 1, 20); wake_clear(g_wake); }
    voice_join(2000, 3012);
    voice_status(&v2);
    /* ревью 13 С1: смена канала — полный выход и вход: новое соединение, E2EE заново (до нового commit/welcome
     * dave=false; в живом звонке mic_live требует dave_on, так что голос под старым ключом не уходит) */
    bool mv = v1.phase == VS_CONNECTED && v2.channel_id == 3012 && v2.guild_id == 2000 && voice_restarts == rs0 + 1 && !v2.dave;
    voice_leave();
    check(mv, "№149 (ревью 13): переход между голосовыми — полный выход и вход, E2EE не переносится со старого канала");
    /* №138: кому звонят, позвонить ещё раз, перестать */
    ev("CHANNEL_CREATE", "{\"id\":\"3703\",\"type\":3,\"name\":\"созвон\",\"owner_id\":\"1000\",\"recipients\":[{\"id\":\"1001\",\"username\":\"alice\"},{\"id\":\"1002\",\"username\":\"bob\"}]}");
    voice_join(0, 3703);                        /* управлять звонком — только находясь в нём (ревью 13 М4) */
    ev("CALL_CREATE", "{\"channel_id\":\"3703\",\"ringing\":[\"1001\",\"1002\"],\"voice_states\":[]}");
    u64 rg[10];
    bool r1 = call_ringing(3703, rg, 10) == 2;
    call_stop_ring(3703, 1001);
    bool r2 = call_ringing(3703, rg, 10) == 1 && rg[0] == 1002;
    call_ring_user(3703, 1001);
    bool r3 = call_ringing(3703, rg, 10) == 2;
    voice_leave();
    ev("CALL_DELETE", "{\"channel_id\":\"3703\"}");
    bool r4 = call_ringing(3703, rg, 10) == 0;
    chan_remove_test(3703);
    check(r1 && r2 && r3 && r4, "№138 групповой звонок: кому звонят (CALL_*), позвонить одному, перестать звонить, конец звонка");
    /* №69 голосовое (демо: по часам, без сети и звука) */
    ui_open_channel(3008);
    Message *vm = msg_find(9906, NULL);
    bool g1 = vm && vm->voice_secs == 7 && (vm->flags & 8192);
    bool g2 = vmsg_play(9906) && vmsg_state()->active && vmsg_state()->dur_ms == 7000;
    g_clock_skew += 3000;
    int pos = vmsg_state()->pos_ms;
    g_clock_skew += 5000;
    bool g3 = pos >= 2900 && pos <= 3500 && !vmsg_state()->active && !vmsg_state()->loading && !vmsg_state()->loading;
    g_clock_skew -= 8000;
    bool g4 = !vmsg_play(9905) && vmsg_state()->err[0];             /* не голосовое */
    vmsg_stop();
    char d[120];
    snprintf(d, sizeof d, "  (голосовое: позиция через 3 с — %d мс)\n", pos);
    term_puts(d);
    check(g1 && g2 && g3 && g4, "№69 голосовые: длина из вложения, воспроизведение (демо по часам), конец, не голосовое — отказ");
    ui_open_channel(was);
    O.demo = was_demo;
}
/* Пакет Б, часть 3 и ревью 11: кнопки ботов, полный профиль, запросы на переписку, группы, папки, слова-тревоги */
void folders_test_proto(const char *b64);
bool ready_slim_check(const char *s, size_t n, char *why, size_t wn);
void chan_remove_test(u64 id);
static int ix_post;
static char ix_body_path[64];
static bool ix_guard(const char *method, const char *path) {
    if (!strcmp(method, "POST") && !strcmp(path, "/interactions")) ix_post++;
    snprintf(ix_body_path, sizeof ix_body_path, "%s", path);
    return false;
}
static void pb_vint(Buf *b, u64 v) { do { u8 c = (u8)(v & 0x7F); v >>= 7; if (v) c |= 0x80; buf_put(b, (const char *)&c, 1); } while (v); }
static void pb_len(Buf *b, int field, const Buf *in) { pb_vint(b, (u64)field << 3 | 2); pb_vint(b, in->len); buf_put(b, in->p, in->len); }
static void t_packb3(void) {
    bool was_demo = O.demo;
    u64 was = S.open_ch;
    O.demo = true;
    ui_open_channel(3004);
    /* ревью 11 С1: чужая ветка не делает сервер «непрочитанным» */
    {
        ev("THREAD_CREATE", "{\"id\":\"3150\",\"type\":11,\"name\":\"чужая\",\"parent_id\":\"3001\",\"guild_id\":\"2000\",\"last_message_id\":\"99999999\",\"thread_metadata\":{},\"member\":{}}");
        Channel *tc = chan_find(3150);
        if (tc) tc->tflags &= (u8)~TF_JOINED;
        bool r1 = tc && !chan_unread(tc);
        if (tc) { tc->tflags |= TF_JOINED; r1 = r1 && chan_unread(tc); tc->tflags &= (u8)~TF_JOINED; }
        S.open_ch = 3150;                       /* ревью 11 М5: удаление открытой ветки — пометка, не исчезновение */
        ev("THREAD_DELETE", "{\"id\":\"3150\",\"guild_id\":\"2000\",\"parent_id\":\"3001\",\"type\":11}");
        tc = chan_find(3150);
        bool r2 = tc && (tc->tflags & TF_DELETED) && !tc->can_send;
        S.open_ch = 3004;
        if (tc) chan_remove_test(3150);
        check(r1 && r2, "ревью 11: ветка без меня — не «непрочитана»; удалённая открытая ветка — помечена, ввод закрыт");
    }
    /* №55 кнопки бота */
    ev("MESSAGE_CREATE", "{\"id\":\"882000\",\"channel_id\":\"3004\",\"application_id\":\"1100\",\"author\":{\"id\":\"1100\",\"username\":\"MemeBot\",\"bot\":true},\"content\":\"очередь\","
       "\"components\":[{\"type\":1,\"components\":[{\"type\":2,\"style\":1,\"label\":\"Пауза\\u001b\",\"custom_id\":\"pause\"},"
       "{\"type\":2,\"style\":5,\"label\":\"Сайт\",\"url\":\"https://example.com\"},{\"type\":2,\"style\":5,\"label\":\"Плохо\",\"url\":\"javascript:1\"},"
       "{\"type\":2,\"style\":2,\"label\":\"Нет\",\"custom_id\":\"x\",\"disabled\":true}]},"
       "{\"type\":1,\"components\":[{\"type\":3,\"custom_id\":\"vol\",\"placeholder\":\"Громкость\",\"min_values\":1,\"max_values\":1,"
       "\"options\":[{\"label\":\"Тихо\",\"value\":\"1\"},{\"label\":\"Громко\",\"value\":\"9\"}]}]}],\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    Message *bm = msg_find(882000, NULL);
    const char *ol = "", *ov = "";
    bool c1 = bm && bm->comp && bm->comp->n == 4 && !strcmp(comp_str(bm, 0, CS_LABEL), "Пауза") && !strcmp(comp_str(bm, 1, CS_URL), "https://example.com") &&
              bm->comp->c[3].type == CT_SELECT && comp_option(bm, 3, 1, &ol, &ov) && !strcmp(ol, "Громко") && !strcmp(ov, "9");
    char err[160] = "";
    bool c2 = comp_click(3004, 882000, 0, err, sizeof err);            /* демо */
    bool c3 = !comp_click(3004, 882000, 2, err, sizeof err) && strstr(err, "недоступна");
    int two[2] = { 0, 1 }, one = 1;
    bool c4 = !comp_select(3004, 882000, 3, two, 2, err, sizeof err);
    O.demo = false; g_http_guard = ix_guard; ix_post = 0;
    bool c5 = comp_select(3004, 882000, 3, &one, 1, err, sizeof err) && ix_post == 1;
    bool c6 = !comp_click(3004, 882000, 0, err, sizeof err) && strstr(err, "ждём");     /* одно нажатие за раз */
    g_clock_skew += 6000; store_tick(); g_clock_skew -= 6000;          /* 5 с без ответа — «бот не ответил» */
    bool c7 = comp_click(3004, 882000, 0, err, sizeof err) && ix_post == 2;
    g_clock_skew += 6000; store_tick(); g_clock_skew -= 6000;
    g_http_guard = NULL; O.demo = true;
    check(c1 && c2 && c3 && c4 && c5 && c6 && c7, "№55 кнопки ботов: разбор (ссылки только https), недоступная, выбор по min/max, POST /interactions, одно нажатие за раз, тайм-аут 5 с");
    ev("MESSAGE_DELETE", "{\"id\":\"882000\",\"channel_id\":\"3004\"}");
    /* №99 полный профиль */
    profile_open_full(1001, 2000);
    const Profile *pf = profile_state();
    bool p1 = pf->full && pf->nmutual == 1 && pf->mutual[0] == 2000 && pf->nfriends == 1 && pf->friends[0] == 1002 && pf->nconn == 1 && strstr(pf->conn[0], "github");
    profile_close();
    profile_open(1001, 2000);
    bool p2 = !profile_state()->full && !profile_state()->nmutual;
    profile_close();
    check(p1 && p2, "№99 полный профиль: общие серверы и друзья, привязки — только по profile_open_full, закрытие освобождает");
    /* №109/111 запросы и группы */
    ev("CHANNEL_CREATE", "{\"id\":\"3700\",\"type\":1,\"recipients\":[{\"id\":\"1007\",\"username\":\"stranger\"}],\"is_message_request\":true,\"last_message_id\":\"9999\"}");
    ev("CHANNEL_CREATE", "{\"id\":\"3701\",\"type\":1,\"recipients\":[{\"id\":\"1008\",\"username\":\"spammer\"}],\"is_spam\":true}");
    ev("CHANNEL_CREATE", "{\"id\":\"3702\",\"type\":3,\"name\":\"компания\",\"owner_id\":\"1000\",\"recipients\":[{\"id\":\"1001\",\"username\":\"alice\"}]}");
    Channel *rq = chan_find(3700), *sp = chan_find(3701), *gd = chan_find(3702);
    bool q1 = rq && (rq->dmflags & DMF_REQUEST) && sp && (sp->dmflags & DMF_SPAM) && gd && gdm_owner(3702) == S.me_id;
    bool q2 = dm_request_accept(3700, err, sizeof err) && !chan_find(3700)->dmflags && dm_request_reject(3701, err, sizeof err) && !chan_find(3701);
    u64 fr = 0;                                 /* друг, которого нет в группе */
    { const Relation *rl; int nr = rel_list(&rl); for (int i = 0; i < nr && !fr; i++) if (rl[i].type == REL_FRIEND && rl[i].id != 1001) fr = rl[i].id; }
    bool q3a = !gdm_add(3702, 99999, err, sizeof err), q3b = fr && gdm_add(3702, fr, err, sizeof err), q3c = chan_find(3702) && chan_find(3702)->nrecip == 2;
    bool q3d = gdm_remove(3702, fr, err, sizeof err), q3e = chan_find(3702) && chan_find(3702)->nrecip == 1, q3f = gdm_leave(3702, err, sizeof err) && !chan_find(3702);
    bool q3 = q3a && q3b && q3c && q3d && q3e && q3f;
    if (chan_find(3700)) chan_remove_test(3700);
    check(q1 && q2 && q3, "№109/111 запросы на переписку и спам (флаги, принять/отклонить), группа ЛС: владелец, добавить друга, исключить, выйти");
    /* №2 папки (protobuf как в user_settings_proto) */
    Buf ids = {0}, f = {0}, w = {0}, fs = {0}, top = {0}, b64 = {0};
    u64 g1 = 2000, g2 = 2100;
    for (int k = 0; k < 8; k++) buf_put(&ids, (const char *)&((u8 *)&g1)[k], 1);
    for (int k = 0; k < 8; k++) buf_put(&ids, (const char *)&((u8 *)&g2)[k], 1);
    pb_len(&f, 1, &ids);
    pb_vint(&w, 1 << 3 | 0); pb_vint(&w, 77); pb_len(&f, 2, &w); w.len = 0;
    { Buf s = {0}; buf_puts(&s, "Игры"); pb_vint(&w, 1 << 3 | 2); pb_vint(&w, s.len); buf_put(&w, s.p, s.len); buf_free(&s); }
    pb_len(&f, 3, &w); w.len = 0;
    pb_vint(&w, 1 << 3 | 0); pb_vint(&w, 0x00FF00); pb_len(&f, 4, &w);
    pb_len(&fs, 1, &f);
    pb_vint(&top, 3 << 3 | 0); pb_vint(&top, 5);   /* чужое поле перед папками */
    pb_len(&top, 14, &fs);
    base64_enc(top.p, top.len, &b64);
    buf_put(&b64, "", 1);
    folders_test_proto(b64.p);
    const GuildFolder *gf;
    int nf = guild_folders(&gf);
    bool f1 = nf == 1 && gf[0].id == 77 && gf[0].name && !strcmp(gf[0].name, "Игры") && gf[0].has_color && gf[0].color == 0x00FF00 &&
              gf[0].n == 2 && gf[0].gids[0] == 2000 && gf[0].gids[1] == 2100;
    {   /* ревью 15 К1: папка с 200 одинаковыми gid и сервер в двух папках — каждый gid один раз на все папки */
        Buf t2 = {0}, f2 = {0}, ids2 = {0}, b2 = {0};
        u64 g7 = 2100;
        for (int r = 0; r < 200; r++) for (int k = 0; k < 8; k++) buf_put(&ids2, (const char *)&((u8 *)&g7)[k], 1);
        pb_len(&f2, 1, &ids2);
        Buf fs2 = {0};
        pb_len(&fs2, 1, &f2);
        pb_len(&fs2, 1, &f2);                   /* та же папка ещё раз — тот же сервер во второй */
        pb_len(&t2, 14, &fs2);
        base64_enc(t2.p, t2.len, &b2);
        buf_put(&b2, "", 1);
        folders_test_proto(b2.p);
        const GuildFolder *gfd;
        int nf2 = guild_folders(&gfd), total = 0;
        for (int i = 0; i < nf2; i++) total += gfd[i].n;
        check(total == 1, "ревью 15 К1: повторы gid в папке и в двух папках отброшены (сервер — ровно один раз)");
        buf_free(&t2); buf_free(&f2); buf_free(&ids2); buf_free(&b2); buf_free(&fs2);
    }
    folders_test_proto("!!!испорчено");
    bool f2 = guild_folders(&gf) == 0;
    buf_free(&ids); buf_free(&f); buf_free(&w); buf_free(&fs); buf_free(&top); buf_free(&b64);
    check(f1 && f2, "№2 папки серверов: protobuf user_settings_proto (id, имя, цвет, серверы); испорченный — без папок");

    /* №119 слова-тревоги */
    alert_words_set("Котик, релиз ,  ");
    Message am = {0};
    am.author_id = 1001;
    am.content = (char *)"Новый РЕЛИЗ вышел";
    bool a1 = msg_alert(&am);
    am.content = (char *)"предрелизный билд";
    bool a2 = !msg_alert(&am);
    am.content = (char *)"смотри, котик!";
    bool a3 = msg_alert(&am) && !strncmp(alert_words(), "Котик, релиз", strlen("Котик, релиз"));
    int m0 = chan_find(3006)->mentions;
    ev("MESSAGE_CREATE", "{\"id\":\"882100\",\"channel_id\":\"3006\",\"guild_id\":\"2000\",\"author\":{\"id\":\"1002\",\"username\":\"bob\"},\"content\":\"когда релиз?\",\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    bool a4 = chan_find(3006)->mentions == m0 + 1;
    chan_find(3006)->mentions = m0;
    alert_words_set("");
    am.content = (char *)"релиз";
    bool a5 = !msg_alert(&am);
    check(a1 && a2 && a3 && a4 && a5, "№119 слова-тревоги: регистр (и кириллица), целое слово, считаются как упоминание, выключение");
    ui_open_channel(was);
    O.demo = was_demo;
}
/* №15: перевод клавиш окна с Ctrl (чистые функции gui_win.c / gui_x11.c) */
#ifdef _WIN32
bool gb_vk_key(unsigned vk, bool ctrl, bool alt, Key *out);
#else
#include <X11/keysym.h>
bool gb_ks_key(unsigned long base, unsigned long ks, bool alt, Key *out);
#endif
static bool kk(bool got, const Key *k, u32 ch) { return got && k->code == K_CHAR && k->ch == ch && k->ctrl && !k->alt; }
static void t_zoom_keys(void) {
    Key k;
    bool ok = true;
#ifdef _WIN32
    ok &= kk(gb_vk_key(VK_OEM_PLUS, true, false, &k), &k, '=');
    ok &= kk(gb_vk_key(VK_ADD, true, false, &k), &k, '=');
    ok &= kk(gb_vk_key(VK_OEM_MINUS, true, false, &k), &k, '-');
    ok &= kk(gb_vk_key(VK_SUBTRACT, true, false, &k), &k, '-');
    ok &= kk(gb_vk_key('0', true, false, &k), &k, '0');
    ok &= kk(gb_vk_key(VK_NUMPAD0, true, false, &k), &k, '0');
    ok &= kk(gb_vk_key('K', true, false, &k), &k, 'k');
    ok &= !gb_vk_key(VK_OEM_PLUS, false, false, &k) && !gb_vk_key('0', false, false, &k) && !gb_vk_key(VK_SUBTRACT, false, false, &k);
    ok &= !gb_vk_key(VK_OEM_PLUS, true, true, &k) && !gb_vk_key('1', true, false, &k);   /* AltGr и прочие — нет */
    const char *where = "окно Windows (VK)";
#else
    ok &= kk(gb_ks_key(XK_equal, XK_equal, false, &k), &k, '=');
    ok &= kk(gb_ks_key(XK_equal, XK_plus, false, &k), &k, '=');
    ok &= kk(gb_ks_key(XK_KP_Add, XK_KP_Add, false, &k), &k, '=');
    ok &= kk(gb_ks_key(XK_minus, XK_minus, false, &k), &k, '-');
    ok &= kk(gb_ks_key(XK_KP_Subtract, XK_KP_Subtract, false, &k), &k, '-');
    ok &= kk(gb_ks_key(XK_0, XK_0, false, &k), &k, '0');
    ok &= kk(gb_ks_key(XK_KP_0, XK_KP_0, false, &k), &k, '0');
    ok &= kk(gb_ks_key(XK_k, XK_k, false, &k), &k, 'k');
    ok &= !gb_ks_key(XK_equal, XK_equal, true, &k) && !gb_ks_key(XK_1, XK_1, false, &k);
    const char *where = "окно X11 (keysym)";
#endif
    char w[300];
    snprintf(w, sizeof w, "№15: %s — Ctrl+= / Ctrl+- / Ctrl+0 и цифровой блок дают клавиши масштаба, без Ctrl и с AltGr — нет", where);
    check(ok, w);
    check(!term_zoom(1) && term_zoom_pct() == 100, "№15: без окна (selftest) term_zoom ничего не меняет, масштаб 100 %");
}
/* Пакет Б: эмодзи серверов, разметка, опросы, эмбеды, ветки, форумы (демо и подставные ответы, без сети) */
void emoji_test_resp(long status, const char *body);
void forum_test_resp(long status, const char *body);
void chan_remove_test(u64 id);
static bool md_has(const char *s, const MdSpan *sp, int n, const char *text, u8 attr) {
    for (int i = 0; i < n; i++) if (sp[i].attr == attr && sp[i].len == strlen(text) && !strncmp(s + sp[i].off, text, sp[i].len)) return true;
    return false;
}
static void t_packb(void) {
    bool was_demo = O.demo;
    u64 was = S.open_ch;
    size_t m0 = plat_private_bytes();
    O.demo = true;
    /* №32 эмодзи */
    emoji_open(2000);
    const EmojiState *es = emoji_state();
    char code[80] = "";
    if (es->n) emoji_code(&es->e[1], code, sizeof code);
    bool e_ok = es->n == 4 && !strcmp(code, "<a:party:7002>") && !es->e[3].available &&
                (S.me_premium || (!emoji_need_nitro(&es->e[0], 2000, 3001) && emoji_need_nitro(&es->e[1], 2000, 3001) && emoji_need_nitro(&es->e[0], 2000, 0)));
    emoji_close();
    O.demo = false; g_http_guard = pa_guard;
    emoji_open(2000);
    emoji_test_resp(200, "[{\"id\":\"1\",\"name\":\"a\\u001b[2Jb\"},{\"id\":\"2\",\"name\":\"x\"},{\"name\":\"noid\"},{\"id\":\"3\",\"name\":\"ok_1\"}]");
    bool e_san = emoji_state()->n == 2 && !strcmp(emoji_state()->e[0].name, "a2Jb") && !strcmp(emoji_state()->e[1].name, "ok_1");
    emoji_close();
    g_http_guard = NULL; O.demo = true;
    check(e_ok && e_san, "№32 эмодзи: по запросу (демо и ответ), имена [A-Za-z0-9_], <a:имя:id>, «нужен Nitro», закрытие освобождает");
    /* №45/46 разметка */
    MdLine ln; MdSpan sp[32];
    const char *l1 = "**жирный** и *курсив* и __подч__ ~~зач~~ ||спойлер|| `код **не**` [сайт](https://example.com) 2*3*4";
    int n1 = md_line(l1, strlen(l1), &ln, sp, 32);
    bool md1 = ln.kind == ML_TEXT && md_has(l1, sp, n1, "жирный", MD_B) && md_has(l1, sp, n1, "курсив", MD_I) &&
               md_has(l1, sp, n1, "подч", MD_U) && md_has(l1, sp, n1, "зач", MD_S) && md_has(l1, sp, n1, "спойлер", MD_SPOIL) &&
               md_has(l1, sp, n1, "код **не**", MD_CODE) && md_has(l1, sp, n1, "сайт", MD_LINK);
    for (int i = 0; i < n1; i++) if (sp[i].attr & MD_LINK) md1 = md1 && sp[i].url_len == 19 && !strncmp(l1 + sp[i].url_off, "https://example.com", 19);
    const char *l2 = "## Заголовок *наклон*", *l3 = "-# мелкий", *l4 = "  12. пункт", *l5 = "[x](javascript:alert(1)) \\*не курсив\\*", *l6 = "***оба***";
    MdLine a2, a3, a4, a5;
    int n2 = md_line(l2, strlen(l2), &a2, sp, 32);
    bool md2 = a2.kind == ML_H2 && md_has(l2, sp, n2, "наклон", MD_I);
    md_line(l3, strlen(l3), &a3, sp, 32);
    md_line(l4, strlen(l4), &a4, sp, 32);
    int n5 = md_line(l5, strlen(l5), &a5, sp, 32);
    bool md3 = a3.kind == ML_SUB && a4.kind == ML_OLIST && a4.indent == 1 && a4.num_len == 3;
    bool md4 = true;
    for (int i = 0; i < n5; i++) if (sp[i].attr) md4 = false;   /* не http — не ссылка; \* — буквально */
    int n6 = md_line(l6, strlen(l6), NULL, sp, 32);
    bool md5 = n6 == 1 && sp[0].attr == (MD_B | MD_I);
    Buf cb = {0};
    bool md6 = md_code_block("текст\n```c\nint x;\nint y;\n```\nещё ```второй```", 1, &cb) && cb.len == strlen("второй") && !memcmp(cb.p, "второй", cb.len);
    buf_free(&cb);
    md6 = md6 && md_code_block("```c\nint x;\n```", 0, &cb) && cb.len == 6 && !memcmp(cb.p, "int x;", 6);
    buf_free(&cb);
    char nd[200]; snprintf(nd, sizeof nd, "  (разметка: %d %d %d %d %d %d)\n", md1, md2, md3, md4, md5, md6); term_puts(nd);
    check(md1 && md2 && md3 && md4 && md5 && md6, "№45/46 разметка: жирный, курсив, подчёркнутый, зачёркнутый, спойлер, код, ссылка (только http/s), заголовки, -#, списки, \\, блок кода для «Копировать»");
    /* №41 опрос и №53 эмбед */
    ui_open_channel(3001);
    ev("MESSAGE_CREATE", "{\"id\":\"881000\",\"channel_id\":\"3001\",\"author\":{\"id\":\"1001\",\"username\":\"alice\"},\"content\":\"\","
       "\"poll\":{\"question\":{\"text\":\"Чай или кофе?\"},\"answers\":[{\"answer_id\":1,\"poll_media\":{\"text\":\"чай\",\"emoji\":{\"name\":\"\\ud83c\\udf75\"}}},"
       "{\"answer_id\":2,\"poll_media\":{\"text\":\"кофе\"}}],\"expiry\":\"2099-01-01T00:00:00+00:00\",\"allow_multiselect\":false,"
       "\"results\":{\"is_finalized\":false,\"answer_counts\":[{\"id\":2,\"count\":3,\"me_voted\":false}]}},\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    Message *pm = msg_find(881000, NULL);
    Poll *p = pm ? pm->poll : NULL;
    char perr[120] = "";
    u32 v1 = 1, two[2] = { 1, 2 };
    bool p1 = p && p->n == 2 && !strcmp(p->question, "Чай или кофе?") && p->a[1].count == 3 && strstr(p->a[0].text, "чай");
    bool p2 = !poll_vote(3001, 881000, two, 2, perr, sizeof perr) && strstr(perr, "один");
    bool p3 = poll_vote(3001, 881000, &v1, 1, perr, sizeof perr) && p->a[0].count == 1 && p->a[0].me;
    poll_vote(3001, 881000, NULL, 0, perr, sizeof perr);
    bool p4 = p->a[0].count == 0 && !p->a[0].me;
    ev("MESSAGE_POLL_VOTE_ADD", "{\"user_id\":\"1002\",\"channel_id\":\"3001\",\"message_id\":\"881000\",\"answer_id\":2}");
    bool p5 = p->a[1].count == 4 && !poll_end(3001, 881000, perr, sizeof perr);   /* не автор */
    check(p1 && p2 && p3 && p4 && p5, "№41 опросы: разбор, один ответ без multiselect, голос/снятие (демо), событие VOTE_ADD, завершить — только автор");
    Buf eb = {0};
    buf_puts(&eb, "{\"id\":\"881001\",\"channel_id\":\"3001\",\"author\":{\"id\":\"1100\",\"username\":\"bot\",\"bot\":true},\"content\":\"\",\"embeds\":[");
    buf_puts(&eb, "{\"title\":\"Заголовок\\u001b[2J\",\"color\":16711680,\"url\":\"javascript:x\",\"description\":\"");
    for (int i = 0; i < 300; i++) buf_puts(&eb, "длинно ");
    buf_puts(&eb, "\",\"image\":{\"url\":\"https://evil.example/x.png\"},\"thumbnail\":{\"url\":\"https://x/y.png\",\"proxy_url\":\"https://media.discordapp.net/external/a/b.png\"},");
    buf_puts(&eb, "\"fields\":[{\"name\":\"поле\",\"value\":\"значение\",\"inline\":true}],\"footer\":{\"text\":\"подвал\"}},{\"title\":\"2\"},{\"title\":\"3\"},{\"title\":\"4\"}],");
    buf_puts(&eb, "\"timestamp\":\"2026-11-09T10:00:00+00:00\"}");
    ev("MESSAGE_CREATE", eb.p);
    buf_free(&eb);
    Message *em = msg_find(881001, NULL);
    const Embed *e0 = em && em->nemb ? &em->emb[0] : NULL;
    size_t total = 0;
    if (e0) { const char *q = e0->s; size_t len = 0; for (int k = 0; k < EM_NSTR; k++) if (e0->o[k] != 0xFFFF && e0->o[k] + strlen(q + e0->o[k]) + 1 > len) len = e0->o[k] + strlen(q + e0->o[k]) + 1; total = len; }
    const char *fn = NULL, *fv = NULL; bool finl = false;
    bool em1 = e0 && em->nemb == EMB_MAX && e0->has_color && e0->color == 0xFF0000 && !strcmp(embed_str(e0, EM_TITLE), "Заголовок") &&
               !*embed_str(e0, EM_URL) && !*embed_str(e0, EM_IMAGE) && strstr(embed_str(e0, EM_THUMB), "media.discordapp.net") &&
               !strcmp(embed_str(e0, EM_FOOTER), "подвал") && total <= EMB_BYTES && strlen(embed_str(e0, EM_DESC)) > 500;
    bool em2 = e0 && e0->nf == 1 && embed_field(e0, 0, &fn, &fv, &finl) && finl && !strcmp(fn, "поле") && !strcmp(fv, "значение");
    char ed[160]; snprintf(ed, sizeof ed, "  (эмбед: %zu Б, полей %d)\n", total, e0 ? e0->nf : -1); term_puts(ed);
    check(em1 && em2, "№53 эмбеды: до 3 на сообщение, <= 1 КБ (обрезка), цвет, без управляющих, ссылка только https, картинки только с CDN/медиа Discord");
    ev("MESSAGE_DELETE", "{\"id\":\"881000\",\"channel_id\":\"3001\"}");
    ev("MESSAGE_DELETE", "{\"id\":\"881001\",\"channel_id\":\"3001\"}");
    /* №90–92 ветки */
    threads_open(2000);
    const Channel *th[8];
    int nt = chan_threads(3001, th, 8);
    bool t1 = nt == 2 && th[0]->id == 3101 && (th[0]->tflags & TF_JOINED) && (th[1]->tflags & TF_LOCKED) && chan_is_thread(th[0]);
    char terr[120] = "";
    bool t2 = thread_create(3001, 0, "новая ветка", terr, sizeof terr) && chan_threads(3001, th, 8) == 3;
    bool t3 = !thread_create(3011, 0, "x", terr, sizeof terr);   /* голосовой — нельзя */
    bool t4 = thread_member(3102, true, terr, sizeof terr) && (chan_find(3102)->tflags & TF_JOINED) &&
              thread_member(3102, false, terr, sizeof terr) && !(chan_find(3102)->tflags & TF_JOINED);
    ev("THREAD_DELETE", "{\"id\":\"3102\",\"guild_id\":\"2000\",\"parent_id\":\"3001\",\"type\":11}");
    bool t5 = !chan_find(3102) && chan_find(3101)->visible;
    threads_open(2100);                         /* другой сервер: не мои ветки 2000 убраны, свои — остались */
    bool t6 = chan_find(3101) != NULL;
    for (int i = S.nchans - 1; i >= 0; i--) if (chan_is_thread(&S.chans[i])) chan_remove_test(S.chans[i].id);
    check(t1 && t2 && t3 && t4 && t5 && t6, "№90–92 ветки: список под каналом (свежие сверху), права от родителя, создать, вступить/выйти, удаление, чистка при смене сервера");
    /* №95 форум: ответ сервера (подставной) */
    O.demo = false; g_http_guard = pa_guard;
    Channel *fc = chan_find(3001);
    int ft = fc->type;
    fc->type = CH_FORUM;
    forum_open(3001, 1, 0);
    forum_test_resp(200, "{\"total_results\":30,\"has_more\":true,\"threads\":[{\"id\":\"4001\",\"type\":11,\"name\":\"пост\\u202e\",\"owner_id\":\"1001\",\"message_count\":7,\"flags\":2},"
                         "{\"id\":\"4002\",\"type\":11,\"name\":\"второй\",\"thread_metadata\":{\"archived\":true}}],\"members\":[{\"id\":\"4002\",\"user_id\":\"1000\"}],"
                         "\"first_messages\":[{\"id\":\"4001\",\"channel_id\":\"4001\",\"author\":{\"id\":\"1001\",\"username\":\"alice\"},\"content\":\"первое сообщение\"}]}");
    const ForumState *fs = forum_state();
    bool f1 = fs->n == 2 && fs->total == 30 && fs->more && fs->page == 1 && !strcmp(fs->p[0].name, "пост") && fs->p[0].pinned &&
              fs->p[0].msg_count == 7 && fs->p[0].preview && fs->p[1].archived && fs->p[1].joined;
    u64 pc = forum_post_open(0);
    bool f2 = pc == 4001 && chan_find(4001) && chan_is_thread(chan_find(4001)) && !strcmp(chan_find(4001)->name, "пост");
    forum_close();
    chan_remove_test(4001);
    fc->type = ft;
    g_http_guard = NULL; O.demo = true;
    check(f1 && f2 && !forum_state()->n, "№95 форумы: страница постов (имя, превью, закреплён, архив, мой), пост открывается как ветка, закрытие освобождает");
    ui_open_channel(was);
    O.demo = was_demo;
    char mm[120];
    snprintf(mm, sizeof mm, "  память пакета Б после закрытия: %+.1f КБ\n", ((double)plat_private_bytes() - (double)m0) / 1024);
    term_puts(mm);
}
/* №140/146: список устройств, выбор (без записи на диск), тест микрофона открывает и закрывает звук */
static void t_audio_dev(void) {
    static char nm[16][96];
    int no = audio_devices(1, nm, 16), ok_names = 1;
    for (int i = 0; i < no; i++) if (dirty(nm[i]) || !nm[i][0]) ok_names = 0;
    int ni = audio_devices(0, nm, 16);
    for (int i = 0; i < ni; i++) if (dirty(nm[i]) || !nm[i][0]) ok_names = 0;
    char d[96];
    snprintf(d, sizeof d, "  (вывод: %d, микрофонов: %d)\n", no, ni);
    term_puts(d);
    check(no >= -1 && ni >= -1 && ok_names, "№140: audio_devices — имена без управляющих символов (или -1 без звука)");
    voice_set_device(0, "Тестовый микрофон");
    bool set_ok = !strcmp(voice_device(0), "Тестовый микрофон") && !strcmp(voice_device(1), "");
    voice_set_device(0, "");
    check(set_ok && !strcmp(voice_device(0), ""), "№146: voice_set_device / voice_device — выбор и сброс на «по умолчанию»");
    voice_mic_test(true);
    i64 t0 = now_ms();
    while (now_ms() - t0 < 300) { Waitable w = wake_waitable(g_wake); plat_wait(&w, 1, 50); wake_clear(g_wake); }
    int lv = voice_mic_level();
    voice_mic_test(false);
    check(lv >= -1 && lv <= 100 && voice_mic_level() == -1, "№146: тест микрофона — уровень 0..100 (или -1 без микрофона), закрытие освобождает звук");
}
static void t_packa(void) {
    /* заглушить и уровни: модель сразу, запрос PATCH .../settings */
    O.demo = false; g_http_guard = pa_guard; pa_patch = 0;
    mute_guild(2100, true);
    mute_channel(3002, true);
    notify_level(2000, 3001, NOTIFY_NONE);
    bool m = guild_find(2100)->muted && chan_find(3002)->muted && chan_find(3001)->notify == NOTIFY_NONE && pa_patch == 3;
    mute_guild(2100, false); mute_channel(3002, false); notify_level(2000, 3001, NOTIFY_DEFAULT);
    check(m && !guild_find(2100)->muted, "заглушить сервер/канал и уровень уведомлений: модель сразу, PATCH /users/@me/guilds/.../settings");
    g_http_fake_status = 500;                   /* М5: ошибка ответа — откат модели */
    mute_guild(2100, true);
    mute_channel(3002, true);
    notify_level(2000, 3001, NOTIFY_NONE);
    notify_level(2000, 0, NOTIFY_NONE);
    g_http_fake_status = 0;
    check(!guild_find(2100)->muted && !chan_find(3002)->muted && chan_find(3001)->notify == NOTIFY_DEFAULT && guild_find(2000)->notify != NOTIFY_NONE,
          "М5: ошибка PATCH настроек — заглушение и уровни откатываются");
    /* «ничего» — упоминания не считаются */
    notify_level(2000, 3006, NOTIFY_NONE);
    int before = chan_find(3006)->mentions;
    ev("MESSAGE_CREATE", "{\"id\":\"99100\",\"channel_id\":\"3006\",\"author\":{\"id\":\"1002\",\"username\":\"bob\"},\"content\":\"<@1000>\",\"mentions\":[{\"id\":\"1000\"}],\"timestamp\":\"2026-11-08T10:00:00+00:00\"}");
    check(chan_find(3006)->mentions == before, "уровень «ничего»: упоминание в канале не добавляет счётчик");
    notify_level(2000, 3006, NOTIFY_DEFAULT);
    /* «ожидает отправки» -> настоящее по nonce; ошибка -> «не отправлено», повтор, удаление */
    u64 was = S.open_ch;
    S.open_ch = 3001;
    pa_post = 0;
    u64 n1 = msg_send(3001, "ожидающее сообщение", 0, SEND_NO_EMBEDS | SEND_NO_MENTIONS);
    Message *pm = msg_find(n1, NULL);
    bool pending = n1 && pm && pm->state == MS_PENDING && pm->author_id == S.me_id && pa_post == 1;
    char mc[300];
    snprintf(mc, sizeof mc, "{\"id\":\"99101\",\"channel_id\":\"3001\",\"nonce\":\"%llu\",\"author\":{\"id\":\"1000\",\"username\":\"you\"},\"content\":\"ожидающее сообщение\",\"timestamp\":\"2026-11-08T10:00:00+00:00\"}",
             (unsigned long long)n1);
    /* А2: чужое сообщение с тем же nonce (или другой канал) «ожидающее» не снимает */
    char fk[300];
    snprintf(fk, sizeof fk, "{\"id\":\"99102\",\"channel_id\":\"3001\",\"nonce\":\"%llu\",\"author\":{\"id\":\"1003\",\"username\":\"mallory\"},\"content\":\"x\",\"timestamp\":\"2026-11-08T10:00:00+00:00\"}",
             (unsigned long long)n1);
    ev("MESSAGE_CREATE", fk);
    bool kept = msg_find(n1, NULL) && msg_find(n1, NULL)->state == MS_PENDING;
    check(kept, "А2: чужой MESSAGE_CREATE с подобранным nonce не снимает «ожидающее»");
    /* С1: «прочитано» не берёт nonce ожидающего как id */
    ack_channel(3001);
    check(chan_find(3001)->last_read != n1, "С1: отметка прочитанного — по последнему отправленному, не по nonce ожидающего");
    ev("MESSAGE_DELETE", "{\"id\":\"99102\",\"channel_id\":\"3001\"}");
    ev("MESSAGE_CREATE", mc);
    check(pending && !msg_find(n1, NULL) && msg_find(99101, NULL), "отправка: сразу в ленте «ожидает», MESSAGE_CREATE с тем же nonce заменяет его настоящим");
    ev("MESSAGE_DELETE", "{\"id\":\"99101\",\"channel_id\":\"3001\"}");   /* не мешать следующим проверкам ленты */
    /* ревью 10 М2: 200 без тела — «отправлено» (не «ожидает»), потом MESSAGE_CREATE заменяет */
    g_http_fake_status = 200;
    u64 n3 = msg_send(3001, "ответ без тела", 0, 0);
    g_http_fake_status = 0;
    Message *m3 = msg_find(n3, NULL);
    bool sent3 = m3 && m3->state == MS_SENT;
    ack_channel(3001);
    bool noack = chan_find(3001)->last_read != n3;
    snprintf(mc, sizeof mc, "{\"id\":\"99103\",\"channel_id\":\"3001\",\"nonce\":\"%llu\",\"author\":{\"id\":\"1000\",\"username\":\"you\"},\"content\":\"ответ без тела\",\"timestamp\":\"2026-11-08T10:00:00+00:00\"}",
             (unsigned long long)n3);
    ev("MESSAGE_CREATE", mc);
    check(sent3 && noack && !msg_find(n3, NULL) && msg_find(99103, NULL), "ревью 10 М2: 200 без тела — MS_SENT, MESSAGE_CREATE заменяет; прочитанное не по nonce");
    ev("MESSAGE_DELETE", "{\"id\":\"99103\",\"channel_id\":\"3001\"}");
    g_http_guard = NULL; O.demo = true;
    u64 n2 = msg_send(3001, "это !fail", 0, 0);           /* демо: «не отправлено» */
    Message *fm = msg_find(n2, NULL);
    bool failed = fm && fm->state == MS_FAILED;
    msg_retry(n2);
    bool still = msg_find(n2, NULL) && msg_find(n2, NULL)->state == MS_FAILED;
    msg_discard(n2);
    check(failed && still && !msg_find(n2, NULL), "не отправлено: MS_FAILED, повтор (в демо снова отказ), удаление убирает из ленты");
    /* С1: полная таблица вытесняет старое «не отправлено»; демо «!slow» — 3 с «ожидает», потом доставлено */
    u64 first = 0, last = 0;
    for (int k = 0; k < 33; k++) { u64 x = msg_send(3001, "!fail вытеснение", 0, 0); if (!k) first = x; last = x; g_clock_skew += 1; }
    bool evicted = last && !msg_find(first, NULL) && msg_find(last, NULL);
    for (int k = S.nmsgs - 1; k >= 0; k--) if (S.msgs[k].state == MS_FAILED) msg_discard(S.msgs[k].id);
    check(evicted && first != last, "С1: 17-е и далее неотправленные вытесняют самое старое «не отправлено» (без отказа); nonce разные в одну мс");
    /* ревью 10 М1: «не отправлено» при смене канала — в черновик своего канала */
    u64 nb = msg_send(3001, "вернись в черновик !fail", 0, 0);
    ui_open_channel(3002);
    ui_open_channel(3001);
    Buf sh = {0};
    ui_snapshot(120, 34, &sh);
    bool back = nb && !msg_find(nb, NULL) && sh.p && strstr(sh.p, "вернись в черновик");
    buf_free(&sh);
    for (int k = 0; k < 40; k++) key(K_BS, 0);   /* поле ввода — снова пустое */
    check(back, "ревью 10 М1: «не отправлено» при уходе из канала — текст в черновике канала, не потерян");
    /* №127: медленный режим (демо: #флуд-2 — 10 с) и тайм-аут */
    {
        SendLimit l0, l1, l2;
        bool ex0 = chan_find(3005)->slow_exempt;
        chan_find(3005)->slow_exempt = false;     /* в демо я с правом «управлять каналами» — медленный режим не действует */
        send_limit(3005, &l0);
        msg_send(3005, "первое в медленном", 0, 0);
        send_limit(3005, &l1);
        int nb0 = S.nmsgs;
        u64 r2 = msg_send(3005, "второе сразу", 0, 0);
        i64 now = wall_ms() / 1000;
        bool ex_ok = ex0;
        chan_find(3005)->slow_exempt = ex0;
        SendLimit l9;
        send_limit(3009, &l9);
        ex_ok = ex_ok && chan_find(3009) && !chan_find(3009)->slow_exempt && l9.slowmode == 15;   /* демо #медленный — действует на меня */
        bool slow_ok = ex_ok && chan_find(3005)->slowmode == 10 && l0.slowmode == 10 && !l0.slow_until &&
                       l1.slow_until >= now + 9 && l1.slow_until <= now + 11 && !r2 && S.nmsgs == nb0;
        Guild *g0 = guild_find(2000);
        char ev1[600], rl[400] = "";
        for (int k = 0; k < g0->nmy && strlen(rl) < 360; k++)
            snprintf(rl + strlen(rl), sizeof rl - strlen(rl), "%s\"%llu\"", k ? "," : "", (unsigned long long)g0->my_roles[k]);
        snprintf(ev1, sizeof ev1, "{\"guild_id\":\"2000\",\"user\":{\"id\":\"1000\"},\"roles\":[%s],\"communication_disabled_until\":\"2099-01-01T00:00:00+00:00\"}", rl);
        ev("GUILD_MEMBER_UPDATE", ev1);
        send_limit(3001, &l2);
        int nb1 = S.nmsgs;
        msg_send(3001, "под тайм-аутом", 0, 0);
        bool to_ok = g0->timeout_until > now && (l2.timeout_until > now || g0->owner_id == S.me_id) && S.nmsgs == nb1;
        snprintf(ev1, sizeof ev1, "{\"guild_id\":\"2000\",\"user\":{\"id\":\"1000\"},\"roles\":[%s]}", rl);
        ev("GUILD_MEMBER_UPDATE", ev1);
        check(slow_ok, "№127: медленный режим — rate_limit_per_user из READY, после своего сообщения отсчёт, второе не уходит");
        check(to_ok && !guild_find(2000)->timeout_until, "№127: тайм-аут (communication_disabled_until) — send_limit, msg_send не шлёт; снятие");
        char d[160];
        snprintf(d, sizeof d, "  (slowmode=%d slow_until-now=%lld, timeout l2=%lld)\n", l1.slowmode, (long long)(l1.slow_until - now), (long long)l2.timeout_until);
        term_puts(d);
    }
    u64 ns = msg_send(3001, "медленно !slow", 0, 0);
    bool slw = ns && msg_find(ns, NULL) && msg_find(ns, NULL)->state == MS_PENDING;
    store_tick();
    bool sp2 = msg_find(ns, NULL) != NULL;
    g_clock_skew += 3100;
    store_tick();
    g_clock_skew -= 3100 + 33;
    bool delivered = !msg_find(ns, NULL) && S.nmsgs && strstr(S.msgs[S.nmsgs - 1].content, "медленно !slow");
    check(slw && sp2 && delivered, "демо «!slow»: 3 с «ожидает отправки», потом доставлено");
    S.open_ch = was;
    /* фильтры поиска */
    search_start(2000, 0, "from:Алиса привет has:link before:2026-11-09");
    const char *sp = search_test_path();
    bool f1 = strstr(sp, "content=%D0%BF%D1%80%D0%B8%D0%B2%D0%B5%D1%82") && strstr(sp, "&author_id=1001") && strstr(sp, "&has=link") && strstr(sp, "&max_id=");
    search_start(2000, 0, "in:#новости смешное");
    bool f2 = strstr(search_test_path(), "&channel_id=3002") != NULL;
    search_start(2000, 0, "from:НетТакого");
    bool f3 = strstr(search_state()->error, "не найден") != NULL;
    search_start(2000, 0, "from:\"Нет Такого Человека Совсем Длинное Имя Ёжик\" привет");
    const char *e4 = search_state()->error;
    bool m4 = strstr(e4, "Нет Такого") != NULL && !strstr(e4, "\xef\xbf\xbd");   /* имя с пробелом целиком, без «битой» буквы */
    for (const unsigned char *p = (const unsigned char *)e4; *p && m4; p++) if (*p == '"' && p[-1] >= 0x80 && (p[-1] & 0xC0) == 0xC0) m4 = false;
    search_start(2000, 0, "from:\"Алиса\" привет");
    m4 = m4 && strstr(search_test_path(), "&author_id=1001") != NULL;
    check(m4, "М4: from:\"имя с пробелами\" — одно значение, в ошибке обрезка по границе UTF-8");
    search_start(2000, 0, "before:2026-13-40");
    bool f4 = strstr(search_state()->error, "ГГГГ-ММ-ДД") != NULL;
    search_start(2000, 0, "привет");
    search_test_resp(200, "{\"total_results\":60,\"messages\":[]}");
    search_page(2);
    bool f5 = strstr(search_test_path(), "&offset=50") && search_state()->page == 2;
    search_close();
    check(f1 && f2 && f3 && f4 && f5, "поиск: from:/in:/has:/before: -> author_id/channel_id/has/max_id, ошибки словами, страницы (offset=50)");
    /* ссылки CDN */
    check(cdn_expired("https://cdn.discordapp.com/attachments/1/2/a.png?ex=5f000000&is=1&hm=x") &&
          !cdn_expired("https://cdn.discordapp.com/attachments/1/2/a.png?ex=7fffffff&is=1&hm=x") && !cdn_expired("https://cdn.discordapp.com/a.png") &&
          cdn_host_ok("https://media.discordapp.net/attachments/1/2/a.png") && !cdn_host_ok("https://evil.example/a.png") &&
          !cdn_host_ok("https://cdn.discordapp.com.evil.example/a.png"),
          "ссылки CDN: истёкший ex= распознан, хосты — только cdn.discordapp.com / media.discordapp.net");
}

static void t_dm_open(void) {
    u64 was = S.open_ch;
    int nch = S.nchans;
    dm_open(1001);
    check(S.nchans == nch && S.open_ch == 4000, "dm_open: беседа с Алисой уже есть — открыта она, новая не создана");
    dm_open(1002);
    Channel *c = chan_find(S.open_ch);
    check(c && S.nchans == nch + 1 && c->type == CH_DM && !c->guild_id && c->nrecip == 1 && c->recipients[0] == 1002 && c->can_send,
          "dm_open (демо): создана ЛС с Бобом, добавлена в список ЛС и открыта");
    u64 id = c ? c->id : 0;
    dm_open(1002);
    check(S.open_ch == id && S.nchans == nch + 1, "dm_open повторно — та же беседа");
    dm_open(S.me_id);
    check(S.nchans == nch + 1, "dm_open с самим собой — ничего");
    S.open_ch = was;
}

static void t_search(void) {
    size_t m0 = plat_private_bytes();
    search_start(2000, 0, "привет");
    const SearchState *s = search_state();
    bool ch_ok = s->nhits == 2;
    for (int i = 0; i < s->nhits; i++) if (s->hits[i].channel_id != 3001 || !strstr(s->hits[i].m.content, "привет")) ch_ok = false;
    check(!s->busy && s->total == 2 && ch_ok, "поиск (демо) по серверу: 2 результата из #general, разбор как у ответа Discord");
    check(!strcmp(search_test_path(), "/guilds/2000/messages/search?content=%D0%BF%D1%80%D0%B8%D0%B2%D0%B5%D1%82"),
          "запрос: /guilds/{id}/messages/search?content= в URL-кодировке");
    search_start(2000, 0, "очистка");
    check(s->nhits == 1 && !strchr(s->hits[0].m.content, 0x1b) && !strstr(s->hits[0].m.content, "\xe2\x80\xae"),
          "результат с ESC и сменой направления — очищен san()");
    search_start(2000, 3001, "a&channel_id=1#x y");
    check(!strcmp(search_test_path(), "/guilds/2000/messages/search?content=a%26channel_id%3D1%23x%20y&channel_id=3001"),
          "запрос: & = # пробел закодированы (подмена параметров невозможна), канал — отдельным параметром");
    search_start(0, 4000, "\x1b" "созвон\x07");
    check(!strcmp(search_test_path(), "/channels/4000/messages/search?content=%D1%81%D0%BE%D0%B7%D0%B2%D0%BE%D0%BD") && s->nhits == 1,
          "поиск в ЛС: /channels/{id}/messages/search, ESC из запроса вырезан");
    search_start(2000, 0, "");
    check(!s->busy && !s->nhits && !search_test_path()[0], "пустой запрос — ничего");
    /* ответы Discord (подставные) */
    search_start(2000, 0, "zzz");
    search_test_resp(202, "{\"message\":\"Index not yet available\",\"code\":110000,\"retry_after\":3}");
    check(s->busy && s->indexing && !s->error[0], "202: «индекс строится», повтор запланирован сам");
    for (int i = 0; i < 6; i++) search_test_resp(202, "{\"retry_after\":1}");
    check(!s->busy && strstr(s->error, "индекс"), "202 много раз: ошибка «индекс ... строится» пользователю");
    search_start(2000, 0, "zzz");
    search_test_resp(429, "{\"message\":\"You are being rate limited.\",\"retry_after\":4.2,\"global\":false}");
    check(!s->busy && strstr(s->error, "слишком часто") && strstr(s->error, "5 с"), "429: «слишком часто, повтор через 5 с»");
    search_start(2000, 0, "zzz");
    search_test_resp(200, "{\"total_results\":1000,\"messages\":[[{\"id\":\"1\",\"channel_id\":\"3001\",\"content\":\"\\u001b]0;x\\u0007hi\",\"author\":{\"id\":\"1003\",\"username\":\"m\"},\"timestamp\":\"2026-01-01T00:00:00+00:00\"}],"
                     "[{\"id\":\"2\",\"content\":\"ctx\"},{\"id\":\"3\",\"channel_id\":\"3002\",\"content\":\"hit\",\"hit\":true}],\"мусор\",[]]}");
    check(s->total == 1000 && s->nhits == 2 && s->hits[1].m.id == 3 && s->hits[1].channel_id == 3002 && !strchr(s->hits[0].m.content, 0x1b),
          "200: группы с hit, мусор пропущен, total из ответа, текст очищен");
    {
        Buf big = {0};
        buf_puts(&big, "{\"total_results\":40,\"messages\":[");
        for (int i = 0; i < 40; i++) buf_printf(&big, "%s[{\"id\":\"%d\",\"channel_id\":\"3001\",\"content\":\"m%d\"}]", i ? "," : "", 100 + i, i);
        buf_puts(&big, "]}");
        search_test_resp(200, big.p);
        buf_free(&big);
        check(s->nhits == SEARCH_PAGE && s->total == 40, "больше 25 в ответе — хранится одна страница (25)");
    }
    search_close();
    search_test_resp(200, "{\"total_results\":1,\"messages\":[[{\"id\":\"5\",\"content\":\"late\"}]]}");
    check(!s->nhits && !s->busy, "ответ после закрытия поиска отброшен");
    size_t a = plat_private_bytes();
    for (int i = 0; i < 300; i++) { search_start(2000, 0, "привет"); search_close(); }
    size_t b = plat_private_bytes();
    say("  память: поиск %+.1f КБ после проверок; 300 циклов поиск/закрытие %+.1f КБ\n",
        ((double)a - (double)m0) / 1024.0, ((double)b - (double)a) / 1024.0);
    check(b <= a + (64u << 10), "300 циклов поиск/закрытие: память не растёт (< 64 КБ)");
}

static void t_file(void) {
    char err[160];
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    char small[600], big[600];
    snprintf(small, sizeof small, "%s/mincord-тест-файл.txt", tmp);
    snprintf(big, sizeof big, "%s/mincord-big.bin", tmp);
    check(!send_file(3001, "Z:/нет/такого/файла.png", NULL, err, sizeof err) && strstr(err, "не найден"), "файл: нет файла — «файл не найден»");
    check(!send_file(3001, tmp, NULL, err, sizeof err) && strstr(err, "папка"), "файл: папка — «это папка»");
    check(!send_file(3003, small, NULL, err, sizeof err) && strstr(err, "нельзя"), "файл: канал без права писать — отказ");
    check(!send_file(0, small, NULL, err, sizeof err) && strstr(err, "канал"), "файл: канал не открыт — отказ");
    check(!send_file(3001, "\\\\evil.example\\share\\x.png", NULL, err, sizeof err) && strstr(err, "сетевые") &&
          !send_file(3001, "//evil.example/share/x.png", NULL, err, sizeof err) &&
          !send_file(3001, "\"\\\\?\\UNC\\evil\\s\\x\"", NULL, err, sizeof err) && strstr(err, "сетевые"),
          "файл: сетевые пути (UNC) отвергнуты до обращения к диску — SMB мимо VLESS и NTLM невозможны");
#ifdef _WIN32
    {   /* SECURITY Н2: NT-префиксы и пути устройств тоже отвергаются (проверка по полному пути) */
        static const char *const NT[] = { "\\??\\UNC\\evil.example\\s\\x.png", "\\\\?\\UNC\\evil.example\\s\\x.png",
                                          "\\\\.\\C:\\Windows\\win.ini", "\\??\\C:\\Windows\\win.ini", "\\GLOBAL??\\C:\\Windows\\win.ini",
                                          "/?" "?/UNC/evil/s/x" };   /* разрезано: иначе триграф ??/ */
        bool all = true;
        for (size_t i = 0; i < sizeof NT / sizeof *NT; i++)
            if (send_file(3001, NT[i], NULL, err, sizeof err) || !strstr(err, "сетевые")) { all = false; say("    пропущен: %s (%s)\n", NT[i], err); }
        check(all, "файл: \\??\\UNC\\, \\\\?\\UNC\\, \\\\.\\, \\??\\C:, \\GLOBAL??\\ — отвергнуты (SECURITY Н2)");
        char e2[160];
        check(!fopen_local("\\??\\UNC\\evil.example\\s\\i.png", e2, sizeof e2) && strstr(e2, "сетевые"), "значок/баннер: та же проверка локального пути");
    }
#endif
#ifdef _WIN32
    wchar_t w[700];
    MultiByteToWideChar(CP_UTF8, 0, small, -1, w, 700);
    FILE *f = _wfopen(w, L"wb");
#else
    FILE *f = fopen(small, "wb");
#endif
    if (f) { for (int i = 0; i < 1000; i++) fputc('a' + i % 26, f); fclose(f); }
    u64 was = S.open_ch;
    S.open_ch = 3001;
    int nm = S.nmsgs;
    bool sent = f && send_file(3001, small, "подпись", err, sizeof err);
    bool shown = S.nmsgs == nm + 1 || (S.nmsgs == nm && nm == 100);
    const Message *last = S.nmsgs ? &S.msgs[S.nmsgs - 1] : NULL;
    check(sent && shown && last && strstr(last->content, "[файл: mincord-тест-файл.txt]") && strstr(last->content, "подпись"),
          "файл (демо): кириллица в пути, «[файл: имя]» с подписью в ленте, в сеть ничего");
    char q[640];
    snprintf(q, sizeof q, "\"%s\"", small);
    check(send_file(3001, q, NULL, err, sizeof err), "файл: путь в кавычках (как копирует Проводник)");
    S.open_ch = was;
#ifdef _WIN32
    _wremove(w);
#else
    remove(small);
#endif
    f = fopen(big, "wb");
    if (f) { fseek(f, (long)FILE_MAX, SEEK_SET); fputc(1, f); fclose(f); }
    check(f && !send_file(3001, big, NULL, err, sizeof err) && strstr(err, "больше 10 МБ"), "файл: 10 МБ + 1 байт — «больше 10 МБ»");
    remove(big);
    check(!file_sending(NULL), "файл: в демо очередь отправки пуста");
}

/* ---- тестировщик: таблица перевода ui_en.h (предложение ревьюера). В каждой паре одинаковые printf-спецификаторы
 * (иначе английская строка с другим %-набором — неопределённое поведение snprintf), таблица строго по strcmp
 * (иначе двоичный поиск в tr() не находит пару — строка остаётся русской), и каждая пара находится через ui_tr. ---- */
#include "ui_en.h"
extern bool g_lang_en;
/* Следующий спецификатор после p: в buf длина+тип («lld», «s», «.1f» -> «f»), возврат — позиция после него или NULL. */
static const char *tr_next_conv(const char *p, char *buf, size_t n) {
    for (; (p = strchr(p, '%')); ) {
        p++;
        if (*p == '%') { p++; continue; }
        while (*p && strchr("-+ #0", *p)) p++;
        if (*p == '*') p++; else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; if (*p == '*') p++; else while (*p >= '0' && *p <= '9') p++; }
        size_t k = 0;
        while (*p && strchr("hlzjt", *p) && k + 2 < n) buf[k++] = *p++;
        if (*p && strchr("diouxXcspfFeEgG", *p)) { buf[k++] = *p++; buf[k] = 0; return p; }
        buf[k] = 0; return p;                                  /* неизвестный тип — сравнится как есть */
    }
    return NULL;
}
static bool tr_same_convs(const char *a, const char *b) {
    char ca[8], cb[8];
    for (;;) {
        a = tr_next_conv(a, ca, sizeof ca);
        b = tr_next_conv(b, cb, sizeof cb);
        if (!a || !b) return !a && !b;
        if (strcmp(ca, cb)) return false;
    }
}
static void t_tr_table(void) {
    size_t n = sizeof TR_EN / sizeof *TR_EN, fmt = 0, ord = 0, miss = 0;
    bool e = g_lang_en;
    g_lang_en = true;
    for (size_t i = 0; i < n; i++) {
        if (!tr_same_convs(TR_EN[i].ru, TR_EN[i].en)) {
            if (fmt++ < 3) { char m[160]; snprintf(m, sizeof m, "    формат различается: %.60s\n", TR_EN[i].ru); term_puts(m); }
        }
        if (i && strcmp(TR_EN[i - 1].ru, TR_EN[i].ru) >= 0) {
            if (ord++ < 3) { char m[160]; snprintf(m, sizeof m, "    порядок: %.60s\n", TR_EN[i].ru); term_puts(m); }
        }
        /* по содержимому: ui_tr отдаёт указатель из таблицы ui.c, а здесь своя копия ui_en.h */
        if (strcmp(ui_tr(TR_EN[i].ru), TR_EN[i].en)) {
            if (miss++ < 3) { char m[160]; snprintf(m, sizeof m, "    не находится: %.60s\n", TR_EN[i].ru); term_puts(m); }
        }
    }
    g_lang_en = e;
    check(n > 100, "ui_en.h: таблица перевода не пуста");
    check(!fmt, "ui_en.h: у каждой пары одинаковые printf-спецификаторы");
    check(!ord, "ui_en.h: таблица строго по strcmp (двоичный поиск)");
    check(!miss, "ui_en.h: каждая пара находится через ui_tr в EN");
}

/* ---- тестировщик: щелчок по КАЖДОЙ кнопке экрана входа и стартового VLESS (ошибка 09.11: номера кнопок B_LG_*
 * совпали с B_LANG — щелчок по галочке переключал язык). Каждая кнопка делает своё, язык меняет только RU|EN. ---- */
extern bool g_lang_en;
void ui_vless_begin(const char *err, bool only);   /* ui.c (объявлены в main.c, не в mc.h) */
void ui_startup_direct_set(bool on);
static void t_login_buttons(void) {
    int cw0 = CW, ch0 = CH;
    CW = 120; CH = 34;
    /* стартовый экран, вопрос */
    ui_vless_begin(NULL, true);
    bool ok = on_screen("Подключение") && scr_has(&cur, "[ ] Больше не спрашивать");
    check(ok, "старт VLESS: экран вопроса, галочка снята");
    click("[ ] Больше не спрашивать");
    check(on_screen("[x] Больше не спрашивать") && !g_lang_en, "старт: щелчок по галочке — ставит её, язык не меняется");
    click("[x] Больше не спрашивать");
    check(on_screen("[ ] Больше не спрашивать") && !g_lang_en, "старт: повторный щелчок — снимает, язык не меняется");
    click_ex("RU|EN", 1, 3, false);
    check(g_lang_en, "старт: RU|EN переключает на английский");
    click_ex("RU|EN", 1, 1, false);
    check(!g_lang_en && on_screen("Подключение"), "старт: RU|EN обратно на русский");
    click("Подключить");
    check(ui_login_active() && on_screen("ссылка должна") && !g_lang_en, "старт: «Подключить» с пустой ссылкой — ошибка у поля, окно не закрыто");
    click("Без VLESS");
    check(!ui_login_active() && !g_lang_en, "старт: «Без VLESS» закрывает экран (дальше клиент), язык не тронут");
    ui_startup_direct_set(false);
    /* стартовый экран, ошибка туннеля */
    ui_vless_begin("туннель не отвечает (проверка)", true);
    check(on_screen("VLESS не работает") && scr_has(&cur, "Повторить") && scr_has(&cur, "Выход"), "старт-ошибка: экран с Повторить / Выход");
    click("Повторить");
    check(ui_login_active() && !g_lang_en && on_screen("VLESS не работает"), "старт-ошибка: «Повторить» — остаётся на экране, язык не тронут");
    click("( Подключить");
    check(ui_login_active() && on_screen("ссылка должна"), "старт-ошибка: «Подключить» с пустой другой ссылкой — ошибка у поля");
    click("Без VLESS");
    check(!ui_login_active() && !g_lang_en, "старт-ошибка: «Без VLESS» закрывает экран");
    ui_vless_begin("туннель не отвечает (проверка)", true);
    click("Выход");
    check(g_quit && !g_lang_en, "старт-ошибка: «Выход» — выход из программы");
    g_quit = false;
    /* окно входа: шаг VLESS -> форма; кнопки формы делают своё, язык не меняется. only=false — после VLESS форма
     * (ui_login_begin не сбрасывает LG.vonly от прошлого ui_vless_begin(..., true)) */
    ui_vless_begin(NULL, false);
    click("Без VLESS");
    check(ui_login_active() && on_screen("С возвращением") && !g_lang_en, "вход: «Без VLESS» — форма входа");
    click("Вход");
    check(ui_login_active() && on_screen("С возвращением") && !g_lang_en, "вход: «Вход» с пустыми полями — остаётся на форме");
    click_ex("RU|EN", 1, 3, false);
    check(g_lang_en, "вход: RU|EN на форме переключает язык");
    click_ex("RU|EN", 1, 1, false);
    check(!g_lang_en, "вход: RU|EN обратно");
    ui_vless_begin(NULL, true);                  /* закрыть окно входа тем же путём, что пользователь */
    click("Без VLESS");
    ui_startup_direct_set(false);
    check(!ui_login_active(), "вход: окно закрыто после проверок");
    ui_lang_init(0);
    CW = cw0; CH = ch0;
}

static void t_voice_life(void) {
    /* жизненный цикл звонка (демо, без звука в динамиках): вход, работа, выход */
    size_t mv0 = plat_private_bytes();
    voice_toggle_deaf();
    voice_join(2000, 3011);
    VoiceStatus st;
    i64 until = now_ms() + 3000, c0 = plat_cpu_ms(), t0 = now_ms();
    size_t mv1 = 0;
    while (now_ms() < until) { plat_sleep_ms(100); size_t m = plat_private_bytes(); if (m > mv1) mv1 = m; }
    double cpu = 100.0 * (double)(plat_cpu_ms() - c0) / (double)(now_ms() - t0);
    voice_status(&st);
    bool running = st.phase == VS_CONNECTED || st.phase == VS_FAILED;
    voice_leave();
    voice_status(&st);
    voice_toggle_deaf();
    plat_sleep_ms(100);
    size_t mv2 = plat_private_bytes();
    char vb[220];
    snprintf(vb, sizeof vb, "звонок (демо): поток запущен и остановлен; память %.1f -> %.1f -> %.1f МБ; процессор в звонке %.2f%%",
             mv0 / 1048576.0, mv1 / 1048576.0, mv2 / 1048576.0, cpu);
    check(running && st.phase == VS_IDLE && mv2 < mv0 + (2u << 20), vb);
}

static void t_arena(void) {
    /* память: большие JSON разбираются в арене и возвращаются системе */
    Buf big = {0};
    buf_puts(&big, "[");
    for (int i = 0; i < 60000; i++) buf_printf(&big, "%s{\"id\":\"%d\",\"name\":\"канал номер %d\",\"x\":[1,2,3]}", i ? "," : "", i, i);
    buf_puts(&big, "]");
    size_t before = plat_private_bytes(), peak = before;
    for (int r = 0; r < 10; r++) {
        cJSON *j = json_parse(big.p, big.len);
        size_t now = plat_private_bytes();
        if (now > peak) peak = now;
        if (!j) break;
        json_done();
    }
    size_t after = plat_private_bytes();
    say("\n  JSON %.1f МБ x10: до %.1f МБ, пик %.1f МБ, после %.1f МБ\n", big.len / 1048576.0, before / 1048576.0,
        peak / 1048576.0, after / 1048576.0);
    check(after < before + (1u << 20), "арена: после разбора память вернулась системе");
    buf_free(&big);
}

/* ---------- READY: отсев полей до cJSON (ревью 3, П1) ---------- */
static u64 sig_mix(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h; }
static u64 sig_str(u64 h, const char *s) { while (s && *s) h = sig_mix(h, (unsigned char)*s++); return sig_mix(h, 0xFF); }
/* Отпечаток модели: всё, что store берёт из READY. */
static u64 store_sig(void) {
    u64 h = sig_mix(S.me_id, (u64)S.nguilds);
    h = sig_str(h, S.me_name);
    h = sig_str(h, S.session_id);
    for (int i = 0; i < S.nusers; i++) { h = sig_mix(h, S.users[i].id); h = sig_str(h, S.users[i].name); h = sig_str(h, S.users[i].avatar); }
    for (int i = 0; i < S.nguilds; i++) {
        Guild *g = &S.guilds[i];
        h = sig_mix(sig_mix(h, g->id), g->owner_id ^ (u64)g->nroles ^ ((u64)g->nmy << 20) ^ ((u64)g->muted << 40));
        h = sig_str(sig_str(h, g->name), g->icon);
        for (int k = 0; k < g->nroles; k++) h = sig_mix(h, g->roles[k].perms ^ g->roles[k].color ^ ((u64)g->roles[k].pos << 32));
    }
    for (int i = 0; i < S.nchans; i++) {
        Channel *c = &S.chans[i];
        h = sig_mix(h, c->id ^ c->parent_id ^ c->last_msg ^ c->last_read);
        h = sig_mix(h, (u64)c->type | (u64)c->position << 8 | (u64)c->mentions << 24 | (u64)c->visible << 40 | (u64)c->can_send << 41 |
                       (u64)c->can_connect << 42 | (u64)c->muted << 43 | (u64)c->nrecip << 48);
        h = sig_str(h, c->name);
    }
    for (int i = 0; i < S.nvs; i++) h = sig_mix(sig_str(h, S.vs[i].name), S.vs[i].user_id ^ S.vs[i].channel_id ^ (u64)S.vs[i].self_mute << 63);
    for (int i = 0; i < S.nrels; i++) h = sig_mix(h, S.rels[i].id ^ (u64)S.rels[i].type << 56 ^ (u64)S.rels[i].status << 60);
    return h;
}
static size_t g_ballast;                        /* make_ready: байты, которые отсев обязан выбросить */
static void junk(Buf *b, int i) {
    size_t s0 = b->len;
    buf_printf(b, "\"flags\":%d,\"x_\\\"q\":\"a\\\\\\\"b}]{[,\",\"nested\":{\"a\":[1,[2,{\"id\":\"999\"}],null,true],\"s\":\"\\u0441\"},\"n\":-1.5e3", i);
    g_ballast += b->len - s0;
}
static void make_ready(Buf *b, int ng) {
    buf_puts(b, "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"me\",\"global_name\":\"Я\",\"email\":\"x@y\",\"phone\":\"+7\",");
    junk(b, 0);
    buf_puts(b, "},\"session_id\":\"abc\",");
    size_t s0 = b->len;
    buf_puts(b, "\"experiments\":[");
    for (int i = 0; i < 2000; i++) buf_printf(b, "%s[%d,0,1,-1,0,%d,0,0]", i ? "," : "", 100000 + i, i);
    buf_puts(b, "],\"guild_experiments\":[");
    for (int i = 0; i < 500; i++) buf_printf(b, "%s[%d,null,0,[[-1,[{\"s\":0,\"e\":10000}]]],[],[[[[1604612045,[\"a\",\"b\"]]]]]]", i ? "," : "", i);
    buf_puts(b, "],\"user_settings_proto\":\"");
    for (int i = 0; i < 40000; i++) buf_put(b, "QUJD", 4);
    buf_puts(b, "\",");
    g_ballast += b->len - s0;
    buf_puts(b, "\"users\":[");
    for (int i = 0; i < 400; i++) {
        buf_printf(b, "%s{\"id\":\"%d\",\"username\":\"u%d\",\"global_name\":\"Имя %d\",\"avatar\":\"%032x\",\"discriminator\":\"0\",\"avatar_decoration_data\":{\"asset\":\"a\",\"sku_id\":\"5\"},\"clan\":null,",
                   i ? "," : "", 10 + i, i, i, i);
        junk(b, i);
        buf_puts(b, "}");
    }
    buf_puts(b, "],\"guilds\":[");
    for (int g = 0; g < ng; g++) {
        int gid = 100000 + g * 1000;
        buf_printf(b, "%s{\"id\":\"%d\",\"properties\":{\"name\":\"Сервер %d\",\"owner_id\":\"%d\",\"icon\":null,\"description\":\"длинное описание\",\"features\":[\"A\",\"B\",\"C\"],\"banner\":null},",
                   g ? "," : "", gid, g, 10 + g);
        size_t e0 = b->len;
        buf_puts(b, "\"emojis\":[");
        for (int e = 0; e < 150; e++) buf_printf(b, "%s{\"id\":\"%d\",\"name\":\"emoji_%d\",\"roles\":[],\"require_colons\":true,\"managed\":false,\"animated\":false,\"available\":true}", e ? "," : "", 900000 + e, e);
        buf_puts(b, "],\"stickers\":[");
        for (int e = 0; e < 30; e++) buf_printf(b, "%s{\"id\":\"%d\",\"name\":\"st%d\",\"tags\":\"x\",\"type\":2,\"format_type\":1,\"description\":\"описание стикера\"}", e ? "," : "", 800000 + e, e);
        buf_puts(b, "],");
        g_ballast += b->len - e0;
        buf_puts(b, "\"roles\":[");
        for (int r = 0; r < 40; r++) {
            buf_printf(b, "%s{\"id\":\"%d\",\"name\":\"роль %d\",\"permissions\":\"%d\",\"color\":%d,\"position\":%d,\"hoist\":false,\"icon\":null,\"tags\":{\"bot_id\":\"5\"},",
                       r ? "," : "", r ? gid + 500 + r : gid, r, r ? 0 : 3263553, r * 1000, r);
            junk(b, r);
            buf_puts(b, "}");
        }
        buf_puts(b, "],\"channels\":[");
        for (int c = 0; c < 80; c++) {
            buf_printf(b, "%s{\"id\":\"%d\",\"type\":%d,\"name\":\"канал-%d\",\"position\":%d,\"topic\":\"тема канала с \\\"кавычками\\\" и \\\\\",\"rate_limit_per_user\":0,"
                       "\"nsfw\":false,\"last_message_id\":\"%d\",\"permission_overwrites\":[{\"id\":\"%d\",\"type\":0,\"allow\":\"0\",\"deny\":\"%d\"}],",
                       c ? "," : "", gid + 1 + c, c % 10 == 9 ? 2 : (c % 10 == 0 ? 4 : 0), c, c, 5000000 + c, gid, c % 7 == 3 ? 1024 : 0);
            junk(b, c);
            buf_puts(b, "}");
        }
        buf_printf(b, "],\"voice_states\":[{\"user_id\":\"%d\",\"channel_id\":\"%d\",\"self_mute\":true,\"self_deaf\":false,\"session_id\":\"s\",\"self_video\":false}],"
                   "\"threads\":[],\"guild_scheduled_events\":[],\"member_count\":12345,\"lazy\":true,\"premium_subscription_count\":3}", 10 + g, gid + 10);
    }
    buf_puts(b, "],\"merged_members\":[");
    for (int g = 0; g < ng; g++) buf_printf(b, "%s[{\"user_id\":\"1\",\"roles\":[\"%d\"],\"nick\":null,\"joined_at\":\"2020\",\"flags\":0}]", g ? "," : "", 100000 + g * 1000 + 503);
    buf_puts(b, "],\"private_channels\":[");
    for (int i = 0; i < 60; i++) buf_printf(b, "%s{\"id\":\"%d\",\"type\":1,\"recipient_ids\":[\"%d\"],\"last_message_id\":\"%d\",\"flags\":0,\"is_spam\":false}", i ? "," : "", 7000000 + i, 10 + i, 7100000 + i);
    buf_puts(b, "],\"read_state\":{\"version\":5,\"partial\":false,\"entries\":[");
    for (int i = 0; i < 1500; i++) buf_printf(b, "%s{\"id\":\"%d\",\"last_message_id\":\"%d\",\"mention_count\":%d,\"last_pin_timestamp\":\"1970\",\"flags\":0}",
                                              i ? "," : "", i < 60 ? 7000000 + i : 100001 + (i - 60), 5000000, i % 3);
    buf_puts(b, "]},\"user_guild_settings\":{\"version\":1,\"entries\":[");
    for (int g = 0; g < ng; g++) buf_printf(b, "%s{\"guild_id\":\"%d\",\"muted\":%s,\"channel_overrides\":[{\"channel_id\":\"%d\",\"muted\":true,\"message_notifications\":3}],\"hide_muted_channels\":false}",
                                            g ? "," : "", 100000 + g * 1000, g % 4 ? "false" : "true", 100000 + g * 1000 + 2);
    buf_puts(b, "]},\"relationships\":[");
    for (int i = 0; i < 150; i++) buf_printf(b, "%s{\"id\":\"%d\",\"type\":%d,\"user_id\":\"%d\",\"nickname\":null,\"since\":\"2021\"}", i ? "," : "", 10 + i, 1 + i % 4, 10 + i);
    buf_puts(b, "],\"merged_presences\":{\"friends\":[");
    for (int i = 0; i < 150; i++) buf_printf(b, "%s{\"user_id\":\"%d\",\"status\":\"%s\",\"client_status\":{\"web\":\"online\"},\"activities\":[{\"name\":\"Игра\",\"type\":0,\"assets\":{\"large_text\":\"x\"}}]}",
                                             i ? "," : "", 10 + i, i % 3 ? "online" : "dnd");
    buf_puts(b, "],\"guilds\":[");
    for (int g = 0; g < ng; g++) {
        buf_puts(b, g ? ",[" : "[");
        for (int i = 0; i < 100; i++) buf_printf(b, "%s{\"user_id\":\"%d\",\"status\":\"online\",\"activities\":[{\"name\":\"Spotify\",\"details\":\"песня\",\"timestamps\":{\"start\":1}}]}", i ? "," : "", 50000 + i);
        buf_puts(b, "]");
    }
    buf_puts(b, "]},\"sessions\":[{\"session_id\":\"x\",\"client_info\":{\"os\":\"w\"}}],\"connected_accounts\":[],\"tutorial\":null,\"country_code\":\"RU\",\"resume_gateway_url\":\"wss://g.discord.gg\"}}");
}
/* READY в живом формате пользовательского аккаунта (дедупликация: relationships с user_id, имена — в users;
 * статусы — в READY_SUPPLEMENTAL.merged_presences.friends) через полный путь: ready_slim + store_ready_text. */
static void t_ready_live_friends(void) {
    static const char R[] = "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"me\",\"global_name\":null},"
        "\"session_id\":\"s1\",\"users\":[{\"id\":\"300000000000000001\",\"username\":\"friend_one\",\"global_name\":\"Друг\",\"avatar\":null,\"discriminator\":\"0\"},"
        "{\"id\":\"5\",\"username\":\"x\",\"global_name\":null}],"
        "\"relationships\":[{\"user_id\":\"300000000000000001\",\"type\":1,\"nickname\":null,\"since\":\"2026-11-09T01:00:00+00:00\",\"id\":\"300000000000000001\",\"is_spam_request\":false},"
        "{\"user_id\":\"5\",\"type\":3,\"id\":\"5\"}],"
        "\"guilds\":[],\"merged_members\":[],\"private_channels\":[],\"read_state\":{\"entries\":[],\"version\":1},"
        "\"user_guild_settings\":{\"entries\":[]},\"merged_presences\":{\"friends\":[],\"guilds\":[]}}}";
    char *t = xmalloc(sizeof R);
    memcpy(t, R, sizeof R);
    size_t n = ready_slim(t, sizeof R - 1);
    char ru[64];
    bool ok = n && store_ready_text(t, n, ru, sizeof ru);
    free(t);
    const Relation *f = rel_find(300000000000000001ULL);
    check(ok && rel_list(NULL) == 2 && f && f->type == REL_FRIEND && !strcmp(user_name(300000000000000001ULL), "Друг"),
          "READY живого формата: relationships (user_id) + users -> друг с именем, входящая заявка");
    static const char SUP[] = "{\"t\":\"READY_SUPPLEMENTAL\",\"s\":2,\"op\":0,\"d\":{\"guilds\":[],\"merged_members\":[],"
        "\"merged_presences\":{\"friends\":[{\"user_id\":\"300000000000000001\",\"status\":\"online\",\"client_status\":{\"desktop\":\"online\"},\"activities\":[]}],\"guilds\":[]},"
        "\"lazy_private_channels\":[],\"game_invites\":[]}}";
    t = xmalloc(sizeof SUP);
    memcpy(t, SUP, sizeof SUP);
    n = ready_slim(t, sizeof SUP - 1);
    cJSON *j = n ? json_parse(t, n) : NULL;
    if (j) store_event("READY_SUPPLEMENTAL", jget(j, "d"));
    json_done();
    free(t);
    f = rel_find(300000000000000001ULL);
    check(f && f->status == PS_ONLINE, "READY_SUPPLEMENTAL: статус друга (merged_presences.friends) не вырезан отсевом и применён — «в сети»");
    S.open_ch = 0;
    users_gc(true);
    check(!strcmp(user_name(300000000000000001ULL), "Друг"), "чистка таблицы пользователей не трогает друзей");
    demo_load();
}

static void t_ready_slim(void) {
    Buf full = {0};
    g_ballast = 0;
    make_ready(&full, 60);
    char *copy = xmalloc(full.len + 1);
    memcpy(copy, full.p, full.len + 1);
    /* без отсева */
    size_t m0 = plat_private_bytes();
    cJSON *j = json_parse(full.p, full.len);
    size_t p_full = plat_private_bytes();
    if (j) store_ready(jget(j, "d"));
    json_done();
    u64 sig_a = store_sig();
    int ng = S.nguilds, nc = S.nchans, nr = S.nrels, nu = S.nusers, nv = S.nvs;
    /* с отсевом: на месте */
    size_t m1 = plat_private_bytes();
    i64 t0 = now_ms();
    size_t n = ready_slim(copy, full.len);
    i64 dt = now_ms() - t0;
    j = n ? json_parse(copy, n) : NULL;
    size_t p_slim = plat_private_bytes();
    if (j) store_ready(jget(j, "d"));
    json_done();
    u64 sig_b = store_sig();
    if (sig_a != sig_b) say("  отпечатки: серверы %d/%d каналы %d/%d друзья %d/%d пользователи %d/%d голос %d/%d\n", ng, S.nguilds, nc, S.nchans, nr, S.nrels, nu, S.nusers, nv, S.nvs);
    say("  READY %.1f МБ -> после отсева %.2f МБ (%lld мс); пик разбора cJSON: без отсева %+.1f МБ, с отсевом %+.1f МБ\n",
        full.len / 1048576.0, n / 1048576.0, (long long)dt, ((double)p_full - (double)m0) / 1048576.0, ((double)p_slim - (double)m1) / 1048576.0);
    /* ревью 14: свойства вместо доли — в ужатом только ключи схемы и ни одного null; балласт выброшен целиком */
    char why[80] = "";
    bool shape = n > 0 && ready_slim_check(copy, n, why, sizeof why);
    bool size_ok = n > 0 && n <= full.len - g_ballast + full.len / 100;
    char rs[220];
    snprintf(rs, sizeof rs, "READY: отсев — только ключи схемы, без null%s%s; балласт %.2f МБ выброшен (осталось %.2f из %.2f МБ)",
             why[0] ? ": " : "", why, g_ballast / 1048576.0, n / 1048576.0, full.len / 1048576.0);
    check(shape && size_ok, rs);
    check(sig_a == sig_b && ng == 60 && nc > 60 * 70 && nr == 150, "READY: модель после отсева та же (серверы, роли, каналы, права, прочитанное, ЛС, друзья, статусы, голос)");
    check(p_slim - m1 < (p_full - m0) / 4, "READY: пик памяти разбора меньше в 4+ раза");
    {   /* по одному серверу: отсев + разбор частями (как в net.c) */
        extern size_t g_ready_peak;
        Buf b3 = {0};
        make_ready(&b3, 60);
        size_t m2 = plat_private_bytes();
        g_ready_peak = m2;
        size_t n3 = ready_slim(b3.p, b3.len);
        char ru[256] = "";
        bool ok3 = n3 && store_ready_text(b3.p, n3, ru, sizeof ru);
        size_t p3 = g_ready_peak;
        g_ready_peak = 0;
        u64 sig_c = store_sig();
        say("  READY по одному серверу: пик %+.2f МБ (было %+.1f МБ целиком после отсева)\n",
            ((double)p3 - (double)m2) / 1048576.0, ((double)p_slim - (double)m1) / 1048576.0);
        check(ok3 && sig_c == sig_a && !strcmp(ru, "wss://g.discord.gg"), "READY по одному серверу: модель та же, resume_gateway_url найден");
        check(p3 - m2 < (p_slim - m1) / 3, "READY по одному серверу: пик в 3+ раза меньше, чем дерево всего пакета");
        static const char *const BAD2[] = {
            "{\"t\":\"READY\",\"d\":{\"guilds\":[{\"id\":\"1\"},",
            "{\"t\":\"READY\",\"d\":[1]}",
            "{\"t\":\"READY\"}",
        };
        bool bad_ok = true;
        for (size_t i = 0; i < 3; i++) {
            size_t bl = strlen(BAD2[i]);
            char *bb = xmalloc(bl + 1);
            memcpy(bb, BAD2[i], bl + 1);
            if (store_ready_text(bb, bl, ru, sizeof ru)) bad_ok = false;
            free(bb);
        }
        check(bad_ok, "READY по одному серверу: обрезанный / без d / d не объект — отказ, без выхода за буфер");
        buf_free(&b3);
    }
    /* служебное и сломанное */
    char other[] = "{\"t\":\"MESSAGE_CREATE\",\"s\":2,\"op\":0,\"d\":{\"experiments\":[1]}}";
    check(ready_slim(other, strlen(other)) == strlen(other) && strstr(other, "experiments"), "прочие события не трогаются");
    char sup[] = "{\"t\":\"READY_SUPPLEMENTAL\",\"s\":2,\"op\":0,\"d\":{\"guilds\":[{\"id\":\"5\",\"voice_states\":[{\"user_id\":\"7\",\"channel_id\":\"8\"}],\"embedded_activities\":[1,2]}],"
                 "\"merged_presences\":{\"guilds\":[[{\"user_id\":\"9\"}]]},\"merged_members\":[[{\"user_id\":\"7\",\"nick\":\"Ник\",\"joined_at\":\"x\"}]]}}";
    size_t sn = ready_slim(sup, strlen(sup));
    check(sn && !strstr(sup, "embedded") && !strstr(sup, "\"user_id\":\"9\"") && strstr(sup, "\"nick\":\"Ник\"") && strstr(sup, "\"voice_states\""),
          "READY_SUPPLEMENTAL: голосовые состояния и ники оставлены, присутствия и активности вырезаны");
    static const char *const BAD[] = {
        "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"users\":[{\"id\":\"1\",\"x\":\"не закрыта",
        "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"users\":[{\"id\" \"1\"}]}}",
        "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"x\":\"\\",
        "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"guilds\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[",
        "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"users\":[1,2,",
    };
    bool all = true;
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        size_t bl = strlen(BAD[i]);
        char *bb = xmalloc(bl + 1);
        memcpy(bb, BAD[i], bl + 1);
        if (ready_slim(bb, bl) != 0) all = false;
        free(bb);
    }
    check(all, "READY сломанный (незакрытые строки/массивы, нет двоеточия, глубина) — 0, без выхода за буфер");
    free(copy);
    buf_free(&full);
    demo_load();                                 /* вернуть демо-модель */
}

/* Фаззинг фильтра READY: испорченные пакеты в буфере, за концом которого — недоступная страница
 * (любое чтение/запись за len — падение процесса, а не тихая порча). */
static unsigned fz_seed = 12345;
static unsigned fz_rand(void) { fz_seed = fz_seed * 1103515245u + 12345u; return fz_seed >> 8; }
static char *guard_alloc(size_t cap, void **base, size_t *basesz) {
    size_t pg = 4096, data = (cap + pg - 1) / pg * pg;
#ifdef _WIN32
    char *b = VirtualAlloc(NULL, data + pg, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    if (!b || !VirtualProtect(b + data, pg, PAGE_NOACCESS, &old)) return NULL;
#else
    char *b = mmap(NULL, data + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED || mprotect(b + data, pg, PROT_NONE)) return NULL;
#endif
    *base = b;
    *basesz = data + pg;
    return b + data;                            /* конец доступной области: данные кладём вплотную к нему */
}
static void guard_free(void *base, size_t sz) {
#ifdef _WIN32
    (void)sz;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, sz);
#endif
}
static void t_slim_fuzz(void) {
    static const char REPRO[] = "{\"t\":\"READY\"\"\":,\"\":,\"d\":{\"\":{\"\"\"\"[]}\"guilds\":[{\"\":\"\"\"roles\":[\"\"\"\"\"\"\"\"\"\"}]\"channels\":[{\"\"\"\"}]}";
    /* затравка — компактный READY со всеми ветками схемы (серверы, роли, каналы, участники, ЛС, прочитанное) */
    Buf seed = {0};
    buf_puts(&seed, "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"user\":{\"id\":\"1\",\"username\":\"me\",\"x\":[1,{\"a\":\"}\"}]},\"session_id\":\"s\","
                    "\"experiments\":[[1,2,[3]],{\"k\":\"v\\\"]\"}],\"users\":[{\"id\":\"2\",\"username\":\"u\",\"avatar\":null,\"clan\":{\"t\":1}}],\"guilds\":[");
    for (int g = 0; g < 3; g++)
        buf_printf(&seed, "%s{\"id\":\"%d\",\"properties\":{\"name\":\"G%d\",\"owner_id\":\"1\",\"icon\":null,\"f\":[\"A\"]},\"emojis\":[{\"id\":\"5\",\"n\":\"e\"}],"
                   "\"roles\":[{\"id\":\"%d\",\"permissions\":\"8\",\"color\":1,\"position\":0,\"tags\":{}}],\"channels\":[{\"id\":\"%d1\",\"type\":0,\"name\":\"c\","
                   "\"permission_overwrites\":[{\"id\":\"1\",\"type\":1,\"allow\":\"0\",\"deny\":\"0\"}],\"topic\":\"t\"},{\"id\":\"%d2\",\"type\":2,\"name\":\"v\"}],"
                   "\"voice_states\":[{\"user_id\":\"2\",\"channel_id\":\"%d2\",\"member\":{\"nick\":\"n\",\"user\":{\"id\":\"2\"}}}]}",
                   g ? "," : "", 100 + g, g, 100 + g, 100 + g, 100 + g, 100 + g);
    buf_puts(&seed, "],\"merged_members\":[[{\"user_id\":\"1\",\"roles\":[\"100\"]}],[],[{\"user_id\":\"1\",\"roles\":[]}]],"
                    "\"private_channels\":[{\"id\":\"9\",\"type\":1,\"recipient_ids\":[\"2\"]}],\"read_state\":{\"entries\":[{\"id\":\"1001\",\"last_message_id\":\"5\"}]},"
                    "\"user_guild_settings\":{\"entries\":[{\"guild_id\":\"100\",\"muted\":true,\"channel_overrides\":[]}]},"
                    "\"relationships\":[{\"id\":\"2\",\"type\":1}],\"merged_presences\":{\"friends\":[{\"user_id\":\"2\",\"status\":\"idle\"}],\"guilds\":[[]]}}}");
    size_t base_len = seed.len;
    void *gb; size_t gsz;
    char *gend = guard_alloc(8192, &gb, &gsz);
    if (!gend) { check(false, "фаззинг: охранная страница не выделена"); buf_free(&seed); return; }
    int runs = 0, slimmed = 0, streamed = 0;
    i64 t0 = now_ms();
    for (int it = 0; it < 20000; it++) {
        char tmp[8192];
        size_t n;
        if (it == 0) { n = sizeof REPRO - 1; memcpy(tmp, REPRO, n); }
        else {
            n = it % 3 == 0 ? base_len : 40 + fz_rand() % (base_len - 40);
            memcpy(tmp, seed.p, n);
            int muts = 1 + (int)(fz_rand() % 8);
            static const char AL[] = "{}[]\",:\\ a0-tnul";
            for (int m = 0; m < muts; m++) {
                size_t at = 24 + fz_rand() % (n - 24);       /* начало "t":"READY" не трогаем — иначе фильтр не включится */
                unsigned op = fz_rand() % 3;
                if (op == 0) tmp[at] = AL[fz_rand() % (sizeof AL - 1)];
                else if (op == 1 && n > 30) { memmove(tmp + at, tmp + at + 1, n - at - 1); n--; }
                else if (n + 1 < sizeof tmp) { memmove(tmp + at + 1, tmp + at, n - at); tmp[at] = AL[fz_rand() % (sizeof AL - 1)]; n++; }
            }
        }
        char *buf = gend - n;
        memcpy(buf, tmp, n);
        size_t r = ready_slim(buf, n);
        runs++;
        if (r && r != n) {
            slimmed++;
            char *b2 = gend - r;
            memmove(b2, buf, r);
            char ru[64];
            if (store_ready_text(b2, r, ru, sizeof ru)) streamed++;
        }
    }
    char w[400];
    snprintf(w, sizeof w, "фаззинг READY: %d пакетов (и 93 Б ревьюера), %lld мс, у недоступной страницы; ужато %d, разобрано %d",
             runs, (long long)(now_ms() - t0), slimmed, streamed);
    check(true, w);
    guard_free(gb, gsz);
    buf_free(&seed);
    demo_load();
}

int selftest(void) {
    term_puts("Astra DS --selftest\n");
    ui_lang_init(0);                            /* проверки — по русскому интерфейсу, независимо от языка системы */
    { const char *ra = getenv("ST_REALAUDIO"); g_audio_fake = !(ra && !strcmp(ra, "1")); }   /* микрофон не открывать без явной просьбы */
    size_t m0 = plat_private_bytes();
    O.demo = true;
    ui_init();
    demo_load();
    ui_on_ready();
    {   /* №2: демо-READY — две папки из user_settings_proto (с именем и цветом; без имени) */
        const GuildFolder *gf;
        int nd = guild_folders(&gf);
        bool f3 = nd == 2 && gf[0].name && !strcmp(gf[0].name, "Игры") && gf[0].color == 0x3BA55C && gf[0].n == 2 && gf[0].gids[1] == 2500 &&
                  !gf[1].name && !gf[1].has_color && gf[1].n == 2 && gf[1].gids[0] == 2600;
        check(f3, "№2 демо: две папки из user_settings_proto демо-READY");
    }
    key(K_DOWN, 0);                              /* «Уютный уголок» */
    key(K_TAB, 0);                               /* каналы */
    key(K_ENTER, 0);                             /* первый — #general, фокус уходит в поле ввода */

    term_puts("\n  данные и разбор:\n");     t_data();
    term_puts("\n  размеры экрана:\n");      t_sizes();
    term_puts("\n  фокус:\n");               t_focus();
    term_puts("\n  прокрутка:\n");           t_scroll();
    term_puts("\n  справка:\n");             t_help();
    term_puts("\n  печатает и звонок:\n");   t_typing_call();
    term_puts("\n  до READY:\n");            t_before_ready();
    term_puts("\n  клавиши:\n");             t_keys();
    term_puts("\n  мышь:\n");                t_mouse();
    term_puts("\n  вредный ввод:\n");        t_evil();
    term_puts("\n  утечки:\n");              t_leak();
    term_puts("\n  друзья:\n");              t_friends();
    term_puts("\n  новая ЛС:\n");            t_dm_open();
    term_puts("\n  волна 1:\n");             t_wave1();
    term_puts("\n  таблица пользователей:\n"); t_users_gc();
    term_puts("\n  капча на запросах:\n");   t_captcha();
    term_puts("\n  волна 2:\n");             t_wave2();
    term_puts("\n  звонки:\n");              t_call_ring();
    term_puts("\n  пакет А:\n");             t_packa();
    term_puts("\n  ссылки CDN и вложения:\n"); t_cdn();
    term_puts("\n  устройства звука:\n");   t_audio_dev();
    term_puts("\n  пакет Б:\n");             t_packb();
    term_puts("\n  клавиши масштаба:\n");   t_zoom_keys();
    term_puts("\n  пакет Б (часть 3):\n");   t_packb3();
    term_puts("\n  звонки в группах, голосовые:\n"); t_calls_vm();
    term_puts("\n  пакет В:\n");             t_packv();
    term_puts("\n  пакет В (часть 2):\n");   t_packv2();
    term_puts("\n  слеш-команды и модерация:\n"); t_slash_mod();
    term_puts("\n  файл со спойлером:\n");  t_send_ex();
    term_puts("\n  автообновление:\n");     update_selftest(check);
    demo_en_selftest(check);
#ifdef ASTRA_VIDEO
    golive_selftest(check);
#endif
    term_puts("\n  очередь 1:\n");          t_queue1();
    term_puts("\n  реакции и пересылка (store):\n"); t_rv_fwd();
    term_puts("\n  поиск:\n");               t_search();
    term_puts("\n  отправка файла:\n");      t_file();
    term_puts("\n  кнопки входа и старта (тестировщик):\n"); t_login_buttons();
    term_puts("\n  таблица перевода ui_en.h (тестировщик):\n"); t_tr_table();

    term_puts("\n  голосовой тракт:\n");
    voice_selftest(check);
    img_selftest(check);
    term_puts("\n  вход (login.c, записанные ответы, без сети):\n");
    login_selftest(check);
    t_voice_life();
    t_arena();
    term_puts("\n  READY: отсев полей:\n");  t_ready_slim();
    t_ready_live_friends();
    t_slim_fuzz();

    say("  частная память процесса: старт %.1f МБ, сейчас %.1f МБ\n", m0 / 1048576.0, plat_private_bytes() / 1048576.0);
    if (deco_quotes) say("  [ДЕФЕКТ] «» в подписях интерфейса: %d раз (оформление должно быть ASCII)\n", deco_quotes);
    say("\nИтог: %d ok, %d FAIL, %d ДЕФЕКТ\n", passes, fails, defects);
    ui_shutdown();
    store_reset();
    return fails ? 1 : 0;
}

/* Проверка протокола на живом Discord без риска для аккаунта: заведомо поддельный токен
 * (зашит здесь, токен пользователя не читается). Ожидается: TLS, WebSocket, zlib, Hello,
 * IDENTIFY, затем отказ с кодом 4004; REST /users/@me — 401. */
/* Этап 1: хранилище секретов и защищённая память, поддельный токен в отдельной записи. */
int selftest_keyring(void) {
    char b[200];
    snprintf(b, sizeof b, "Astra DS --keyringtest: %s (запись Astra DS/selftest)\n", keyring_name());
    term_puts(b);
    keyring_test_mode();
    if (!secret_init()) { check(false, "защищённая память выделена"); return 1; }
    check(true, "защищённая память выделена и закреплена");
    static const char FAKE[] = "MTAwMDAwMDAwMDAwMDAwMDAw.AAAAAA.mincord-keyring-test-token-123";
    secret_set("bad token with spaces", 21);
    check(secret_token() == NULL, "токен с недопустимыми символами отвергнут");
    secret_set(FAKE, sizeof FAKE - 1);
    check(secret_token() && !strcmp(secret_token(), FAKE), "токен принят в защищённую память");
    check(!strncmp(secret_auth_header(), "Authorization: ", 15), "заголовок Authorization собран там же");
    cred_delete();
    check(cred_save(), "сохранение в хранилище");
    secret_set("", 0);
    check(secret_token() == NULL, "память очищена перед чтением");
    check(cred_load() && secret_token() && !strcmp(secret_token(), FAKE), "чтение из хранилища: тот же токен");
    cred_delete();
    secret_set("", 0);
    check(!cred_load(), "после удаления токена в хранилище нет");
    {   /* переименование mincord -> Astra DS: запись под старым именем переносится в новое и старая удаляется */
        static const char V[] = "vless://migration-test-not-a-real-link";
        char buf[128];
        keyring_delete("vless");
        bool w = keyring_save_old("vless", V, sizeof V - 1);
        int n = keyring_load("vless", buf, sizeof buf);
        int nn = keyring_load_raw("vless", false, buf + 64, 64);
        int no = keyring_load_raw("vless", true, buf + 64, 64);
        check(w && n == (int)sizeof V - 1 && nn == n && no < 0, "миграция хранилища: старое имя -> новое (перечитано и сверено), старое удалено");
        keyring_save_old("vless", V, sizeof V - 1);
        keyring_delete("vless");
        check(keyring_load_raw("vless", false, buf, 64) < 0 && keyring_load_raw("vless", true, buf, 64) < 0,
              "удаление (--logout, паника) стирает запись под обоими именами");
        wipe(buf, sizeof buf);
    }
    secret_destroy();
    check(secret_token() == NULL, "защищённая память освобождена");
    snprintf(b, sizeof b, "\nИтог: %d ok, %d FAIL\n", passes, fails);
    term_puts(b);
    return fails ? 1 : 0;
}

extern bool g_headless;
static long rest_status = -1;
static void on_me(Resp *r, void *ctx) { (void)ctx; rest_status = r->status; }

int selftest_gateway(void) {
    term_puts("Astra DS --gwtest: живой Discord, поддельный токен\n");
    g_headless = true;
    static const char FAKE[] = "MTAwMDAwMDAwMDAwMDAwMDAw.AAAAAA.mincord-selftest-not-a-real-token-xx";
    if (!secret_init()) return 1;
    secret_set(FAKE, sizeof FAKE - 1);
    plat_net_init();
    if (!net_init()) { term_puts("сеть не запустилась\n"); return 1; }
    ui_init();
    /* Этап 1: только gateway, ожидание как в главном цикле main.c — просыпаемся лишь по сокету
     * gateway или по его таймеру (gw_next_wake), без частого опроса. Так ловится Hello, застрявший
     * в буфере curl после рукопожатия WebSocket: сокет больше не «готов», и клиент ждёт таймер 30 с. */
    gw_start();
    char last[128] = "";
    i64 t0 = now_ms(), deadline = t0 + 45000, t_login = -1;
    while (now_ms() < deadline) {
        const char *st = gw_status_text();
        if (strcmp(st, last)) {
            snprintf(last, sizeof last, "%s", st);
            if (t_login < 0 && strstr(st, "вход")) t_login = now_ms() - t0;
            char b[200];
            snprintf(b, sizeof b, "  [%5lld мс] gateway: %s\n", (long long)(now_ms() - t0), st);
            term_puts(b);
        }
        if (g_auth_failed && strstr(st, "токен")) break;
        Waitable w[4];
        int n = gw_waitables(w, 4);
        i64 t = gw_next_wake(), now = now_ms();
        if (t > deadline) t = deadline;
        plat_wait(w, n, t > now ? (int)(t - now) : 0);
        gw_service();
    }
    {
        char b[200];
        snprintf(b, sizeof b, "gateway: Hello обработан сразу после подключения (%lld мс, без ожидания таймера 30 с)", (long long)t_login);
        check(t_login >= 0 && t_login < 5000, b);
    }
    /* Этап 2: REST, с частым опросом (у REST в этом цикле нет своих ожидаемых объектов) */
    http_async("GET", "/users/@me", NULL, on_me, NULL);
    for (i64 rd = now_ms() + 20000; rest_status < 0 && now_ms() < rd; ) {
        i64 t = http_next_wake(), now = now_ms();
        plat_wait(NULL, 0, t == INT64_MAX ? 200 : (int)(t > now ? (t - now > 200 ? 200 : t - now) : 0));
        http_service();
    }
    char b[160];
    snprintf(b, sizeof b, "  REST /users/@me: HTTP %ld\n", rest_status);
    term_puts(b);
    check(rest_status == 401, "REST: TLS до discord.com, поддельный токен отвергнут (401)");
    check(g_auth_failed && strstr(gw_status_text(), "токен"), "gateway: TLS+WebSocket+zlib, Hello, IDENTIFY, отказ 4004 распознан");
    /* Этап 3: отправка файла (multipart потоком, 2 МБ) с поддельным токеном — ожидается 401.
     * Цикл как в main.c: ждём только сокеты отправки и таймер curl; считаем пробуждения. */
    {
        static const char RD[] = "{\"user\":{\"id\":\"1\",\"username\":\"t\"},\"private_channels\":[{\"id\":\"5\",\"type\":1,\"recipient_ids\":[\"2\"]}]}";
        cJSON *j = json_parse(RD, sizeof RD - 1);
        store_ready(j);
        json_done();
        const char *tmp = getenv("TEMP");
        if (!tmp) tmp = getenv("TMPDIR");
        if (!tmp) tmp = "/tmp";
        char fp[600], err[160] = "";
        snprintf(fp, sizeof fp, "%s/mincord-up-test.bin", tmp);
        FILE *f = fopen(fp, "wb");
        if (f) { for (int i = 0; i < (2 << 20); i++) fputc(i & 0xff, f); fclose(f); }
        size_t m0 = plat_private_bytes(), peak = m0;
        bool started = f && send_file(5, fp, "test", err, sizeof err);
        int wakes = 0, pct = 0, maxpct = 0;
        i64 u0 = now_ms();
        while (started && file_sending(&pct) && now_ms() - u0 < 60000) {
            if (pct > maxpct) maxpct = pct;
            Waitable w[4];
            int n = http_waitables(w, 4);
            i64 t = http_next_wake(), now = now_ms();
            plat_wait(w, n, t == INT64_MAX ? 1000 : (int)(t > now ? t - now : 0));
            http_service();
            wakes++;
            size_t m = plat_private_bytes();
            if (m > peak) peak = m;
        }
        remove(fp);
        i64 dt = now_ms() - u0;
        char ub[320];
        snprintf(ub, sizeof ub, "  отправка 2 МБ (поддельный токен): %lld мс, пробуждений %d (%.0f/с), отправлено до %d%%, память %+.0f КБ пик\n",
                 (long long)dt, wakes, dt > 0 ? wakes * 1000.0 / dt : 0.0, maxpct, ((double)peak - (double)m0) / 1024.0);
        term_puts(ub);
        if (!started) { term_puts("  не началась: "); term_puts(err); term_puts("\n"); }
        check(started && !file_sending(NULL), "отправка файла: завершилась (ответ Discord получен), очередь REST свободна");
        check(peak < m0 + (1u << 20), "отправка файла: пик памяти < 1 МБ при файле 2 МБ (читается потоком)");
    }
    login_qr_probe(check);
    login_captcha_probe(check);                 /* окно капчи: WebView2, тестовый ключ hCaptcha */                      /* шлюз QR-входа: рукопожатие без входа, ссылка не выводится */
    snprintf(b, sizeof b, "  частная память: %.1f МБ\n\nИтог: %d ok, %d FAIL\n", plat_private_bytes() / 1048576.0, passes, fails);
    term_puts(b);
    net_cleanup();
    secret_destroy();
    return fails ? 1 : 0;
}


/* ======================= --livetest: записи на живом аккаунте =======================
 * Только свой тестовый сервер «mincord-test» (создаётся, если нет) и ЛС с другом MC_LIVE_FRIEND
 * (--livetest=dm). Чужие серверы — только чтение: сторож g_http_guard
 * не пропускает ни одной записи вне тестового сервера/ЛС. Паузы между записями — как у человека.
 * Вывод — только сюда (это проверка, не рабочий режим): метод, путь, код ответа; токен не печатается. */
static u64 LV_G, LV_C, LV_DM, LV_U;             /* тестовый сервер, канал; ЛС; друг (владелец) */
static u64 LV_G2;                               /* временный сервер (создать и удалить) */
static bool lv_allow_newguild;                  /* разрешено создать временный сервер */
static char lv_join_code[40];                   /* разрешено вступить только по этому коду */
static int lv_writes, lv_errors, lv_blocked;
static bool lv_path_in(const char *path, u64 id, const char *pre) {
    char b[48];
    snprintf(b, sizeof b, "%s%llu", pre, (unsigned long long)id);
    size_t n = strlen(b);
    return id && !strncmp(path, b, n) && (path[n] == 0 || path[n] == '/' || path[n] == '?');
}
/* /channels/<id>... — канал тестового или временного сервера (созданные в прогоне — тоже) */
static bool lv_test_channel(const char *path) {
    if (strncmp(path, "/channels/", 10)) return false;
    u64 id = strtoull(path + 10, NULL, 10);
    Channel *c = chan_find(id);
    return c && c->guild_id && (c->guild_id == LV_G || (LV_G2 && c->guild_id == LV_G2));
}
static bool lv_guard(const char *method, const char *path) {
    if (!strcmp(method, "GET")) return true;
    char jp[64];
    snprintf(jp, sizeof jp, "/invites/%s", lv_join_code);
    if (lv_join_code[0] && !strcmp(method, "POST") && !strcmp(path, jp)) return true;
    if (lv_allow_newguild && !strcmp(method, "POST") && !strcmp(path, "/guilds")) return true;
    if (lv_test_channel(path) || lv_path_in(path, LV_G2, "/guilds/")) return true;
    bool ok = (!LV_G && !strcmp(method, "POST") && !strcmp(path, "/guilds")) ||
              lv_path_in(path, LV_G, "/guilds/") || lv_path_in(path, LV_C, "/channels/") || lv_path_in(path, LV_DM, "/channels/") ||
              (!strcmp(method, "POST") && (!strcmp(path, "/users/@me/channels") || !strcmp(path, "/users/@me/relationships"))) ||
              (!strcmp(method, "PUT") && lv_path_in(path, LV_U, "/users/@me/relationships/"));
    if (!ok) { lv_blocked++; say("  [СТОРОЖ] не пропущено: %s %s\n", method, path); }
    return ok;
}
static void lv_trace(const char *method, const char *path, long status) {
    if (strcmp(method, "GET")) lv_writes++;
    bool bad = status == 0 || status >= 400;
    if (bad) lv_errors++;
    say("    -> %s %s : %ld%s\n", method, path, status, bad ? "  <- ОШИБКА" : "");
}
static void lv_pump(int ms) {
    i64 end = now_ms() + ms;
    do {
        Waitable w[8];
        int n = gw_waitables(w, 8);
        n += http_waitables(w + n, 8 - n);
        n += vless_waitables(w + n, 8 - n);
        i64 t = gw_next_wake(), h = http_next_wake(), v = vless_next_wake(), st = store_next_wake(), now = now_ms();
        if (h < t) t = h;
        if (v < t) t = v;
        if (st < t) t = st;
        if (t > end) t = end;
        plat_wait(w, n, t > now ? (int)(t - now) : 0);
        vless_service();
        gw_service();
        http_service();
        store_tick();
    } while (now_ms() < end);
}
#define LV_WAIT(cond, ms) do { for (i64 _d = now_ms() + (ms); !(cond) && now_ms() < _d; ) lv_pump(100); } while (0)
static void lv_pause(void) { lv_pump(4000 + (int)(now_ms() % 3000)); }   /* как у человека: 4..7 с */
typedef struct { bool done; long st; u64 id; bool captcha; char body[1200]; } LvR;
static void lv_cb(Resp *r, void *ctx) {
    LvR *x = ctx;
    x->st = r->status;
    if (r->status >= 400 && r->body.p) {        /* тело ошибки (code, message, errors) — для разбора причины */
        char *t = san(r->body.p, false);
        snprintf(x->body, sizeof x->body, "%s", t);
        free(t);
    }
    if (r->body.p && strstr(r->body.p, "captcha")) x->captcha = true;
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    x->id = jid(j, "id");
    json_done();
    x->done = true;
}
static LvR lv_rest(const char *method, const char *path, const char *body) {
    LvR x = {0};
    http_async(method, path, body, lv_cb, &x);
    LV_WAIT(x.done || net_captcha()->pending, 30000);
    if (!x.done && net_captcha()->pending) { x.captcha = true; net_captcha_cancel(); }   /* ответ капчи придёт в x сейчас, пока x жив */
    return x;
}
static bool lv_hist_done;
static void lv_hist(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p, r->body.len); if (cJSON_IsArray(j)) store_set_messages(j, false); json_done(); }
    lv_hist_done = true;
}
static void lv_open(u64 ch) {
    S.open_ch = ch;
    char p[96];
    snprintf(p, sizeof p, "/channels/%llu/messages?limit=50", (unsigned long long)ch);
    lv_hist_done = false;
    http_async("GET", p, NULL, lv_hist, NULL);
    LV_WAIT(lv_hist_done, 20000);
}
/* Отправить сообщение (тело — как у ui.c). extra — дополнительные поля JSON (",\"flags\":4096" ...). */
static u64 lv_send(u64 ch, const char *text, const char *extra) {
    Buf b = {0};
    buf_puts(&b, "{\"content\":");
    buf_json_str(&b, text);
    buf_printf(&b, ",\"nonce\":\"%llu\",\"tts\":false%s}", (unsigned long long)snowflake_now(), extra ? extra : ",\"flags\":0");
    char p[64];
    snprintf(p, sizeof p, "/channels/%llu/messages", (unsigned long long)ch);
    LvR r = lv_rest("POST", p, b.p);
    if (r.st != 200) say("  тело запроса: %s\n  ответ %ld: %s%s\n", b.p, r.st, r.body, net_captcha()->pending ? " (капча: запрос ждёт проверки)" : "");
    buf_free(&b);
    if (r.st == 200 && r.id) LV_WAIT(msg_find(r.id, NULL) != NULL, 15000);
    return r.st == 200 ? r.id : 0;
}
static const Message *lv_last_with(const char *needle) {
    for (int i = S.nmsgs - 1; i >= 0; i--) if (S.msgs[i].author_id == S.me_id && strstr(S.msgs[i].content, needle)) return &S.msgs[i];
    return NULL;
}
/* Общие проверки сообщений в канале ch (сервер или ЛС). guild — для поиска (0 — ЛС). */
static void lv_messages(u64 ch, u64 guild, const char *where) {
    char w[400];
    lv_open(ch);
    snprintf(w, sizeof w, "%s: история загружена, сообщения в ленте", where);
    check(lv_hist_done, w);
    lv_pause();
    u64 m1 = lv_send(ch, "проверка Astra DS: отправка", NULL);
    snprintf(w, sizeof w, "%s: отправка — сообщение пришло по gateway (MESSAGE_CREATE)", where);
    check(m1 && msg_find(m1, NULL), w);
    if (!m1) { term_puts("  отправка не прошла — остальные проверки этого канала пропущены\n"); return; }
    lv_pause();
    char p[128];
    snprintf(p, sizeof p, "/channels/%llu/messages/%llu", (unsigned long long)ch, (unsigned long long)m1);
    LvR r = lv_rest("PATCH", p, "{\"content\":\"проверка Astra DS: отправка (правка)\"}");
    LV_WAIT(msg_find(m1, NULL) && msg_find(m1, NULL)->edited, 15000);
    snprintf(w, sizeof w, "%s: правка — (ред.) по MESSAGE_UPDATE", where);
    check(r.st == 200 && msg_find(m1, NULL) && msg_find(m1, NULL)->edited, w);
    lv_pause();
    snprintf(p, sizeof p, "/channels/%llu/messages/%llu/reactions/%%F0%%9F%%91%%8D/@me?location=Message&type=0", (unsigned long long)ch, (unsigned long long)m1);
    r = lv_rest("PUT", p, NULL);
    LV_WAIT(msg_find(m1, NULL) && msg_find(m1, NULL)->nreacts > 0, 15000);
    snprintf(w, sizeof w, "%s: реакция — пришла по MESSAGE_REACTION_ADD", where);
    check(r.st == 204 && msg_find(m1, NULL) && msg_find(m1, NULL)->nreacts > 0 && msg_find(m1, NULL)->reacts[0].me, w);
    lv_pause();
    char ex[200];
    snprintf(ex, sizeof ex, ",\"flags\":0,\"message_reference\":{\"message_id\":\"%llu\"},\"allowed_mentions\":{\"parse\":[\"users\",\"roles\",\"everyone\"],\"replied_user\":false}",
             (unsigned long long)m1);
    u64 m2 = lv_send(ch, "проверка Astra DS: ответ с @ ВЫКЛ", ex);
    snprintf(w, sizeof w, "%s: ответ с «@ ВЫКЛ» — ссылка на исходное", where);
    check(m2 && msg_find(m2, NULL) && msg_find(m2, NULL)->ref_id == m1, w);
    lv_pause();
    u64 m3 = lv_send(ch, "проверка Astra DS: тихое сообщение", ",\"flags\":4096");
    snprintf(w, sizeof w, "%s: тихое — flags 4096", where);
    check(m3 && msg_find(m3, NULL) && (msg_find(m3, NULL)->flags & 4096), w);
    lv_pause();
    /* файл */
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    char fp[600], err[160] = "";
    snprintf(fp, sizeof fp, "%s/mincord-проверка.txt", tmp);
#ifdef _WIN32
    wchar_t wp[700];
    MultiByteToWideChar(CP_UTF8, 0, fp, -1, wp, 700);
    FILE *f = _wfopen(wp, L"wb");
#else
    FILE *f = fopen(fp, "wb");
#endif
    if (f) { fputs("проверка Astra DS: маленький файл\n", f); fclose(f); }
    bool fs = f && send_file(ch, fp, "проверка Astra DS: файл", err, sizeof err);
    LV_WAIT(!file_sending(NULL), 60000);
    LV_WAIT(lv_last_with("[файл: Astra DS") != NULL, 15000);
    const Message *fm = lv_last_with("[файл: Astra DS");
    if (fm) say("  файл в ленте: %.120s\n", fm->content);
    snprintf(w, sizeof w, "%s: отправка файла (кириллица в имени) — «[файл: ...]» в ленте", where);
    check(fs && fm, w);
#ifdef _WIN32
    _wremove(wp);
#else
    remove(fp);
#endif
    char big[600];
    snprintf(big, sizeof big, "%s/mincord-big-live.bin", tmp);
    f = fopen(big, "wb");
    if (f) { fseek(f, (long)FILE_MAX, SEEK_SET); fputc(1, f); fclose(f); }
    int w0 = lv_writes;
    snprintf(w, sizeof w, "%s: файл > 10 МБ — отказ до сети", where);
    check(f && !send_file(ch, big, NULL, err, sizeof err) && strstr(err, "10 МБ") && lv_writes == w0, w);
    remove(big);
    lv_pause();
    /* закрепить / открепить */
    msg_pin(ch, m1, true);
    pins_open(ch);
    LV_WAIT(!pins_state()->busy && pins_state()->n > 0, 20000);
    bool pinned = false;
    for (int i = 0; i < pins_state()->n; i++) if (pins_state()->m[i].id == m1) pinned = true;
    snprintf(w, sizeof w, "%s: закрепить — сообщение в закреплённых (новый API pins)", where);
    check(pinned && !pins_state()->error[0], w);
    lv_pause();
    msg_pin(ch, m1, false);
    lv_pump(3000);
    pins_open(ch);
    LV_WAIT(!pins_state()->busy, 20000);
    pinned = false;
    for (int i = 0; i < pins_state()->n; i++) if (pins_state()->m[i].id == m1) pinned = true;
    snprintf(w, sizeof w, "%s: открепить — сообщения в закреплённых нет", where);
    check(!pinned && !pins_state()->error[0], w);
    pins_close();
    lv_pause();
    /* непрочитанное / прочитано */
    int e0 = lv_errors;
    Channel *c = chan_find(ch);
    mark_unread(ch, m2);
    ack_seen(ch);                               /* авто-отметка не должна снять ручное «непрочитанное» */
    lv_pump(2500);
    snprintf(w, sizeof w, "%s: «отметить непрочитанным» — Discord принял, канал непрочитан", where);
    check(c && c->last_read == m2 - 1 && lv_errors == e0, w);
    lv_pause();
    ack_channel(ch);
    lv_pump(4000);
    snprintf(w, sizeof w, "%s: прочитано (ack) — Discord принял", where);
    check(c && c->last_read >= m3 && lv_errors == e0, w);
    /* поиск (новый индекс может строиться — 202, модуль повторяет сам) */
    search_start(guild, guild ? ch : ch, "Astra DS");
    LV_WAIT(!search_state()->busy, 120000);
    snprintf(w, sizeof w, "%s: поиск «Astra DS» — %d найдено%s%s", where, search_state()->nhits, search_state()->error[0] ? ", " : "", search_state()->error);
    check(!search_state()->busy && (search_state()->nhits > 0 || strstr(search_state()->error, "индекс")), w);
    search_close();
    /* переход к сообщению (?around) */
    msgs_clear();
    msgs_jump(ch, m1);
    LV_WAIT(S.jump_id == m1 && msg_find(m1, NULL), 20000);
    snprintf(w, sizeof w, "%s: переход к сообщению (around) — лента вокруг него", where);
    check(S.jump_id == m1 && msg_find(m1, NULL) && !S.detached, w);
    lv_pause();
    /* удаление (тихое) */
    snprintf(p, sizeof p, "/channels/%llu/messages/%llu", (unsigned long long)ch, (unsigned long long)m3);
    r = lv_rest("DELETE", p, NULL);
    LV_WAIT(!msg_find(m3, NULL), 15000);
    snprintf(w, sizeof w, "%s: удаление — ушло из ленты по MESSAGE_DELETE", where);
    check(r.st == 204 && !msg_find(m3, NULL), w);
}

static int live_guild(void) {
    term_puts("\n  тестовый сервер «mincord-test»:\n");
    for (int i = 0; i < S.nguilds; i++) if (S.guilds[i].name && !strcmp(S.guilds[i].name, "mincord-test")) LV_G = S.guilds[i].id;
    if (!LV_G) {
        LvR r = lv_rest("POST", "/guilds", "{\"name\":\"mincord-test\"}");
        if (r.captcha) { term_puts("  Discord требует капчу при создании сервера — остановка (не обходим)\n"); return 1; }
        check((r.st == 200 || r.st == 201) && r.id, "сервер создан (POST /guilds)");
        if (!r.id) return 1;
        LV_G = r.id;
        LV_WAIT(guild_find(LV_G) != NULL, 20000);
        lv_pause();
    } else say("  сервер уже есть: %llu\n", (unsigned long long)LV_G);
    check(guild_find(LV_G) != NULL, "сервер в модели (GUILD_CREATE / READY)");
    for (int i = 0; i < S.nchans; i++)
        if (S.chans[i].guild_id == LV_G && S.chans[i].type == CH_TEXT && S.chans[i].can_send) { LV_C = S.chans[i].id; break; }
    if (!LV_C) {
        char p[64];
        snprintf(p, sizeof p, "/guilds/%llu/channels", (unsigned long long)LV_G);
        LvR r = lv_rest("POST", p, "{\"name\":\"Astra DS\",\"type\":0}");
        LV_C = r.id;
        LV_WAIT(chan_find(LV_C) != NULL, 15000);
        lv_pause();
    }
    check(LV_C && chan_find(LV_C), "текстовый канал на тестовом сервере");
    if (!LV_C) return 1;
    lv_messages(LV_C, LV_G, "сервер");
    /* тема канала: PATCH, затем открыть канал заново — тема приходит GET /channels/{id} */
    lv_pause();
    char p[64];
    snprintf(p, sizeof p, "/channels/%llu", (unsigned long long)LV_C);
    LvR r = lv_rest("PATCH", p, "{\"topic\":\"проверка Astra DS: тема канала\"}");
    S.open_ch = 0;
    lv_open(LV_C);
    LV_WAIT(chan_find(LV_C)->topic != NULL, 15000);
    check(r.st == 200 && chan_find(LV_C)->topic && strstr(chan_find(LV_C)->topic, "тема канала"), "тема канала: задана и пришла при открытии канала");
    /* упоминания, профиль — чтение */
    mentions_open();
    LV_WAIT(!mentions_state()->busy, 20000);
    char w[400];
    snprintf(w, sizeof w, "упоминания: ответ разобран (%d)%s%s", mentions_state()->n, mentions_state()->error[0] ? " - " : "", mentions_state()->error);
    check(!mentions_state()->busy && !mentions_state()->error[0], w);
    mentions_close();
    profile_open(S.me_id, LV_G);
    LV_WAIT(!profile_state()->busy, 20000);
    snprintf(w, sizeof w, "профиль (свой, на тестовом сервере): «%s», на сервере с %lld%s%s", profile_state()->name,
             (long long)profile_state()->member_since_unix, profile_state()->error[0] ? " - " : "", profile_state()->error);
    check(!profile_state()->busy && !profile_state()->error[0] && profile_state()->member_since_unix > 0, w);
    profile_close();
    /* статус: отошёл / не беспокоить / невидимка / в сети (op 3), между — паузы */
    static const u8 ST[] = { PS_IDLE, PS_DND, PS_INVISIBLE, PS_ONLINE };
    for (int i = 0; i < 4; i++) { presence_set(ST[i]); lv_pump(5000); }
    check(presence_me() == PS_ONLINE && !strcmp(gw_status_text(), "в сети"), "статус: отошёл -> не беспокоить -> невидимка -> в сети, gateway жив");
    return 0;
}

/* ЛС с другом MC_LIVE_FRIEND (имя пользователя): принять заявку, если входящая; дальше — dm_open и те же проверки. */
static const char *lv_friend(void) { const char *f = getenv("MC_LIVE_FRIEND"); return f && *f ? f : NULL; }
static Buf lv_body;
static bool lv_body_done;
static void lv_body_cb(Resp *r, void *ctx) {
    (void)ctx;
    buf_free(&lv_body);
    if (r->status == 200 && r->body.p) buf_put(&lv_body, r->body.p, r->body.len);
    lv_body_done = true;
}
/* id и тип отношения с пользователем username (GET /users/@me/relationships), 0 — нет */
static u64 lv_find_user(const char *username, int *type) {
    lv_body_done = false;
    http_async("GET", "/users/@me/relationships", NULL, lv_body_cb, NULL);
    LV_WAIT(lv_body_done, 20000);
    u64 id = 0;
    *type = 0;
    cJSON *j = json_parse(lv_body.p ? lv_body.p : "", lv_body.len);
    const cJSON *e;
    cJSON_ArrayForEach(e, j) {
        const char *un = jstr(jget(e, "user"), "username");
        if (un && !strcmp(un, username)) { id = jid(e, "id"); *type = (int)jint(e, "type", 0); break; }
    }
    json_done();
    buf_free(&lv_body);
    return id;
}
static int live_dm(void) {
    if (!lv_friend()) { term_puts("  задайте MC_LIVE_FRIEND=имя_пользователя друга\n"); return 1; }
    term_puts("\n  ЛС с другом:\n");
    int type = 0;
    u64 uid = lv_find_user(lv_friend(), &type);
    if (type != REL_FRIEND) {
        /* заявку шлёт владелец (наша по имени — 400); принимаем входящую через rel_accept (PUT) */
        if (type != REL_IN || !rel_find(uid) || rel_find(uid)->type != REL_IN) { say("  нет входящей заявки от друга (тип %d) — стоп\n", type); return 2; }
        LV_U = uid;
        lv_pause();
        rel_accept(uid);
        LV_WAIT(rel_find(uid) && rel_find(uid)->type == REL_FRIEND, 30000);
        check(rel_find(uid) && rel_find(uid)->type == REL_FRIEND, "rel_accept: входящая заявка принята (PUT), RELATIONSHIP_ADD type 1");
        if (!rel_find(uid) || rel_find(uid)->type != REL_FRIEND) return 2;
        lv_pause();
    }
    check(rel_find(uid) && rel_find(uid)->type == REL_FRIEND, "друг (RELATIONSHIP_ADD type 1)");
    say("  статус друга: %s\n", presence_name(rel_find(uid)->status));
    /* новая ЛС через dm_open (есть беседа — та же, нет — POST /users/@me/channels) */
    u64 was = S.open_ch;
    S.open_ch = 0;
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].type == CH_DM && S.chans[i].nrecip == 1 && S.chans[i].recipients[0] == uid) LV_DM = S.chans[i].id;
    bool existed = LV_DM != 0;
    if (!existed) {
        /* id беседы ещё не знаем — сторож пропускает POST /users/@me/channels; дальше канал по ответу */
        dm_open(uid);
        LV_WAIT(S.open_ch != 0, 20000);
        LV_DM = S.open_ch;
    } else dm_open(uid);
    Channel *c = chan_find(LV_DM);
    check(c && c->type == CH_DM && c->recipients[0] == uid, existed ? "dm_open: беседа уже была — открыта она" : "dm_open: беседа создана (POST /users/@me/channels) и в списке ЛС");
    (void)was;
    if (!c) return 1;
    lv_messages(LV_DM, 0, "ЛС");
    return 0;
}

/* Одна отправка в ЛС с другом и одна заявка по имени — с полным телом ответа (разбор причины 400). */
static int live_probe(void) {
    term_puts("\n  проба: ЛС и заявка по имени (по одному запросу):\n");
    int type = 0;
    if (!lv_friend()) { term_puts("  задайте MC_LIVE_FRIEND\n"); return 1; }
    u64 uid = lv_find_user(lv_friend(), &type);
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].type == CH_DM && S.chans[i].nrecip == 1 && S.chans[i].recipients[0] == uid) LV_DM = S.chans[i].id;
    say("  друг: отношение %d, ЛС %s\n", type, LV_DM ? "есть" : "нет");
    if (LV_DM) {
        lv_open(LV_DM);
        lv_pause();
        u64 m = lv_send(LV_DM, "проверка Astra DS", NULL);
        say("  ЛС: %s\n", m ? "отправлено (200)" : "не отправлено");
        if (net_captcha()->pending) net_captcha_cancel();
    }
    lv_pause();
    char rb[128];
    snprintf(rb, sizeof rb, "{\"username\":\"%s\",\"discriminator\":null}", lv_friend());
    LvR r = lv_rest("POST", "/users/@me/relationships", rb);
    say("  заявка по имени: %ld %s%s\n", r.st, r.body, net_captcha()->pending ? " (капча: запрос ждёт проверки)" : "");
    if (net_captcha()->pending) net_captcha_cancel();
    return 0;
}

/* Капча вживую: заявка по имени -> 400 капча -> окно проверки ВИДИМО владельцу -> повтор с ключом.
 * Ответ складывается в статическую LvR: колбэк приходит после проверки, когда стек давно другой. */
static LvR lv_capr;
static int live_captcha(void) {
    if (!lv_friend()) { term_puts("  задайте MC_LIVE_FRIEND\n"); return 1; }
    term_puts("\n  капча вживую: заявка в друзья по имени\n");
    memset(&lv_capr, 0, sizeof lv_capr);
    char rb[128];
    snprintf(rb, sizeof rb, "{\"username\":\"%s\",\"discriminator\":null}", lv_friend());
    http_async("POST", "/users/@me/relationships", rb, lv_cb, &lv_capr);
    LV_WAIT(lv_capr.done || net_captcha()->pending, 30000);
    if (lv_capr.done) { say("  Discord ответил без капчи: %ld %s\n", lv_capr.st, lv_capr.body); return 0; }
    const CaptchaState *cs = net_captcha();
    if (!cs->pending) { term_puts("  ни ответа, ни капчи за 30 с\n"); return 1; }
    say("  капча: «%s», окно поддерживается: %s — открываю окно проверки\n", cs->what, cs->supported ? "да" : "нет");
    net_captcha_open();
    if (!cs->open) { say("  окно не открылось: %s\n", cs->error); net_captcha_cancel(); return 1; }
    term_puts("  окно проверки открыто — ждём прохождения (до 3 минут)...\n");
    LV_WAIT(!cs->open, 180000);
    if (cs->open) { term_puts("  не пройдено за 3 минуты — отмена\n"); net_captcha_cancel(); return 1; }
    if (cs->pending) { say("  окно закрыто без ответа: %s — отмена\n", cs->error); net_captcha_cancel(); LV_WAIT(lv_capr.done, 5000); return 1; }
    term_puts("  проверка пройдена — запрос повторён с X-Captcha-Key, ждём ответ Discord\n");
    LV_WAIT(lv_capr.done || cs->pending, 30000);
    if (cs->pending) { say("  Discord снова просит проверку (ответ не принят): %s — отмена\n", cs->error); net_captcha_cancel(); LV_WAIT(lv_capr.done, 5000); }
    say("  ответ Discord после повтора: %ld %s\n", lv_capr.st, lv_capr.body[0] ? lv_capr.body : "(пусто — успех без тела)");
    check(lv_capr.done && lv_capr.st != 0, "капча вживую: окно, прохождение, повтор с ключом — Discord ответил");
    return 0;
}

/* ---------- волна 2 вживую: только на «mincord-test» и временном сервере ---------- */
static const Channel *lv_chan_named(u64 gid, const char *name) {
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].guild_id == gid && S.chans[i].name && !strcmp(S.chans[i].name, name)) return &S.chans[i];
    return NULL;
}
static int live_wave2(void) {
    term_puts("\n  волна 2 на «mincord-test»:\n");
    for (int i = 0; i < S.nguilds; i++) if (S.guilds[i].name && !strcmp(S.guilds[i].name, "mincord-test")) LV_G = S.guilds[i].id;
    if (!LV_G) { term_puts("  нет сервера mincord-test — стоп\n"); return 1; }
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].guild_id == LV_G && S.chans[i].type == CH_TEXT) { LV_C = S.chans[i].id; break; }
    char err[160], w[300];
    check(guild_is_owner(LV_G) && can_manage_channels(LV_G) && can_manage_guild(LV_G) && can_create_invite(LV_C), "права на своём сервере: владелец, всё доступно");
    /* категория и канал в ней */
    lv_pause();
    check(chan_create(LV_G, CHK_CATEGORY, "mincord-категория", 0, err, sizeof err), "создать категорию: запрос ушёл");
    LV_WAIT(lv_chan_named(LV_G, "mincord-категория") != NULL, 15000);
    const Channel *cat = lv_chan_named(LV_G, "mincord-категория");
    check(cat && cat->type == CH_CATEGORY, "категория создана (CHANNEL_CREATE)");
    u64 cat_id = cat ? cat->id : 0;
    lv_pause();
    check(cat_id && chan_create(LV_G, CHK_TEXT, "mincord-канал", cat_id, err, sizeof err), "создать канал в категории: запрос ушёл");
    LV_WAIT(lv_chan_named(LV_G, "mincord-канал") != NULL, 15000);
    const Channel *nc = lv_chan_named(LV_G, "mincord-канал");
    check(nc && nc->parent_id == cat_id, "канал создан в категории");
    u64 nid = nc ? nc->id : 0;
    if (!nid) return 1;
    lv_pause();
    ChanEdit ce = { "mincord-канал-2", "проверка Astra DS: тема", 10, 0, -1, -1 };
    check(chan_edit(nid, &ce, err, sizeof err), "изменить канал (имя, тема, медленный режим): запрос ушёл");
    LV_WAIT(lv_chan_named(LV_G, "mincord-канал-2") != NULL, 15000);
    ChanEdit info;
    bool ci = false;
    for (i64 d = now_ms() + 15000; !(ci = chan_info(nid, &info)) && now_ms() < d; ) lv_pump(100);
    snprintf(w, sizeof w, "канал изменён: имя по CHANNEL_UPDATE, настройки (GET) — медленный режим %d, тема «%s»", ci ? info.slowmode : -1, ci ? info.topic : "?");
    check(lv_chan_named(LV_G, "mincord-канал-2") && ci && info.slowmode == 10 && strstr(info.topic, "тема"), w);
    lv_pause();
    chan_perm_set(nid, LV_G, OW_ROLE, 0, P_SEND_MESSAGES);                /* @everyone: запретить писать */
    lv_pump(3000);
    lv_pause();
    chan_perm_clear(nid, LV_G);
    lv_pump(3000);
    lv_pause();
    chan_move(nid, 0, 5);                                        /* из категории наружу */
    LV_WAIT(chan_find(nid) && chan_find(nid)->parent_id == 0, 15000);
    check(chan_find(nid) && chan_find(nid)->parent_id == 0, "переместить канал из категории (PATCH /guilds/{id}/channels)");
    /* роли, описание, значок сервера */
    const RoleName *rn;
    int nr = -1;
    for (i64 d = now_ms() + 15000; (nr = guild_roles(LV_G, &rn)) < 0 && now_ms() < d; ) lv_pump(100);
    snprintf(w, sizeof w, "роли сервера с именами (GET): %d, первая «%s»", nr, nr > 0 ? rn[0].name : "?");
    check(nr >= 1, w);
    lv_pause();
    static const unsigned char PNG[] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,'I','H','D','R',0,0,0,1,0,0,0,1,8,2,0,0,0,0x90,0x77,0x53,0xDE,
        0,0,0,0x0C,'I','D','A','T',0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,0x00,0x03,0x01,0x01,0x00,0x18,0xDD,0x8D,0xB0,0,0,0,0,'I','E','N','D',0xAE,0x42,0x60,0x82 };
    const char *tmp = getenv("TEMP");
    char ip[600];
    snprintf(ip, sizeof ip, "%s\\mincord-icon.png", tmp ? tmp : ".");
    FILE *f = fopen_u8(ip, "wb");
    if (f) { fwrite(PNG, 1, sizeof PNG, f); fclose(f); }
    check(guild_edit(LV_G, NULL, "проверка Astra DS: описание", ip, NULL, err, sizeof err), "изменить сервер (описание, значок PNG): запрос ушёл");
    remove(ip);
    LV_WAIT(guild_find(LV_G)->icon[0], 15000);
    check(guild_find(LV_G)->icon[0] != 0, "значок сервера установлен (GUILD_UPDATE)");
    const char *desc = NULL;
    for (i64 d = now_ms() + 15000; !(desc = guild_description(LV_G)) && now_ms() < d; ) lv_pump(100);
    check(desc && strstr(desc, "описание"), "описание сервера (GET /guilds/{id})");
    /* приглашение на свой сервер */
    lv_pause();
    invite_create(LV_C, 3600, 1);
    LV_WAIT(invite_created() != NULL, 15000);
    snprintf(w, sizeof w, "создать приглашение: %s", invite_created() ? invite_created() : "нет");
    check(invite_created() != NULL, w);
    if (invite_created()) {
        invite_open(invite_created());
        LV_WAIT(!invite_state()->busy, 15000);
        check(invite_state()->guild_id == LV_G && invite_state()->joined && invite_state()->members >= 1, "карточка своего приглашения: «вы уже на сервере», число участников");
        invite_close();
    }
    /* удалить канал и категорию */
    lv_pause();
    chan_delete(nid);
    LV_WAIT(!chan_find(nid), 15000);
    lv_pause();
    chan_delete(cat_id);
    LV_WAIT(!chan_find(cat_id), 15000);
    check(!chan_find(nid) && !chan_find(cat_id), "удалить канал и категорию (CHANNEL_DELETE)");
    /* временный сервер: создать и удалить */
    lv_pause();
    lv_allow_newguild = true;
    int ng = S.nguilds;
    check(guild_create("mincord-temp", "", err, sizeof err), "создать временный сервер: запрос ушёл");
    LV_WAIT(S.nguilds > ng, 20000);
    lv_allow_newguild = false;
    for (int i = 0; i < S.nguilds; i++) if (S.guilds[i].name && !strcmp(S.guilds[i].name, "mincord-temp")) LV_G2 = S.guilds[i].id;
    check(LV_G2 && guild_is_owner(LV_G2), "временный сервер создан, вы владелец");
    if (LV_G2) {
        lv_pause();
        check(!guild_delete(LV_G2, "mincord-tem", err, sizeof err), "удаление: неточное имя — отказ до сети");
        check(guild_delete(LV_G2, "mincord-temp", err, sizeof err), "удалить временный сервер по точному имени: запрос ушёл");
        LV_WAIT(!guild_find(LV_G2), 20000);
        check(!guild_find(LV_G2), "временный сервер удалён (GUILD_DELETE)");
    }
    return 0;
}
/* Перестановка соседей вживую: два канала в категории, обмен (PATCH массивом), обратно, удаление. */
static int live_swap(void) {
    term_puts("\n  перестановка каналов на «mincord-test»:\n");
    for (int i = 0; i < S.nguilds; i++) if (S.guilds[i].name && !strcmp(S.guilds[i].name, "mincord-test")) LV_G = S.guilds[i].id;
    if (!LV_G) return 1;
    char err[160];
    chan_create(LV_G, CHK_TEXT, "mincord-a", 0, err, sizeof err);
    LV_WAIT(lv_chan_named(LV_G, "mincord-a") != NULL, 15000);
    lv_pause();
    chan_create(LV_G, CHK_TEXT, "mincord-b", 0, err, sizeof err);
    LV_WAIT(lv_chan_named(LV_G, "mincord-b") != NULL, 15000);
    const Channel *a = lv_chan_named(LV_G, "mincord-a"), *b = lv_chan_named(LV_G, "mincord-b");
    if (!a || !b) { term_puts("  каналы не создались\n"); return 1; }
    u64 ida = a->id, idb = b->id;
    lv_pause();
    bool below0 = chan_find(idb)->position > chan_find(ida)->position;
    chan_swap(idb, -1);                         /* b выше a */
    LV_WAIT(false, 4000);
    bool swapped = chan_find(idb)->position < chan_find(ida)->position;
    lv_pause();
    chan_swap(idb, 1);                          /* обратно */
    LV_WAIT(false, 4000);
    bool back = chan_find(idb)->position > chan_find(ida)->position;
    char w[200];
    snprintf(w, sizeof w, "перестановка вживую: b ниже a -> b выше a -> обратно (%d/%d/%d)", below0, swapped, back);
    check(below0 && swapped && back, w);
    lv_pause();
    chan_delete(ida);
    LV_WAIT(!chan_find(ida), 15000);
    lv_pause();
    chan_delete(idb);
    LV_WAIT(!chan_find(idb), 15000);
    check(!chan_find(ida) && !chan_find(idb), "временные каналы удалены");
    return 0;
}
/* Друзья вживую — ТОЛЬКО ЧТЕНИЕ (g_readonly): сколько отношений по типам, есть ли имена, статусы после
 * READY_SUPPLEMENTAL, что увидит экран «Друзья». Без имён и id в выводе. */
static int live_friends(void) {
    term_puts("\n  друзья (только чтение):\n");
    lv_pump(6000);                              /* READY_SUPPLEMENTAL со статусами приходит после READY */
    const Relation *rs;
    int n = rel_list(&rs), by[6] = {0}, named = 0, online = 0, friends = 0;
    for (int i = 0; i < n; i++) {
        if (rs[i].type < 6) by[rs[i].type]++;
        if (strcmp(user_name(rs[i].id), "?")) named++;
        if (rs[i].type == REL_FRIEND) { friends++; if (rs[i].status != PS_OFFLINE) online++; }
    }
    say("  отношений: %d (друзей %d, заблокировано %d, входящих %d, исходящих %d); с именем в таблице: %d\n",
        n, by[REL_FRIEND], by[REL_BLOCKED], by[REL_IN], by[REL_OUT], named);
    say("  экран «Друзья»: «В сети» — %d, «Все» — %d, «Ожидание» — %d\n", online, friends, by[REL_IN] + by[REL_OUT]);
    check(n > 0 && named == n, "друзья вживую: отношения есть, у всех есть имена");
    return 0;
}
/* Вступление по ссылке владельца: карточка, затем ОДИН POST; капча — видимое окно до 3 минут. */
static int live_join(void) {
    const char *link = getenv("MC_LIVE_INVITE");   /* ссылка-приглашение для проверки вступления */
    if (!link || !*link) { term_puts("  задайте MC_LIVE_INVITE=https://discord.gg/КОД\n"); return 1; }
    term_puts("\n  вступление по приглашению владельца:\n");
    invite_open(link);
    LV_WAIT(!invite_state()->busy, 20000);
    const InviteState *iv = invite_state();
    say("  карточка: «%s», канал #%s, участников %d, в сети %d, уже там: %s%s%s\n", iv->guild_name ? iv->guild_name : "?", iv->channel_name ? iv->channel_name : "?",
        iv->members, iv->online, iv->joined ? "да" : "нет", iv->error[0] ? ", ошибка: " : "", iv->error);
    check(!iv->error[0] && iv->guild_id, "карточка приглашения (GET /invites/{code}?with_counts=true)");
    if (iv->error[0] || iv->joined) return iv->error[0] ? 1 : 0;
    snprintf(lv_join_code, sizeof lv_join_code, "%s", iv->code);
    lv_pause();
    invite_join();
    LV_WAIT(!invite_state()->busy || net_captcha()->pending, 30000);
    if (net_captcha()->pending) {
        term_puts("  Discord просит капчу — открываю окно (до 3 минут)\n");
        net_captcha_open();
        LV_WAIT(!net_captcha()->open, 180000);
        if (net_captcha()->pending) { say("  капча не пройдена: %s — отмена\n", net_captcha()->error); net_captcha_cancel(); }
        LV_WAIT(!invite_state()->busy, 30000);
    }
    LV_WAIT(guild_find(iv->guild_id) != NULL, 20000);
    say("  итог: %s%s%s\n", iv->joined ? "вступили" : "не вступили", iv->error[0] ? " — " : "", iv->error);
    check(iv->joined && guild_find(iv->guild_id), "вступление по ссылке (POST /invites/{code}), сервер в модели (GUILD_CREATE)");
    lv_join_code[0] = 0;
    return 0;
}

#ifdef ASTRA_VIDEO
#include "share.h"
/* Окно-источник для трансляции: своё, крупный текст и секунды (не исключается из захвата — это и есть картинка). */
#ifdef _WIN32
static HWND lvs_hwnd;
static volatile LONG lvs_quit;
static i64 lvs_t0;
static LRESULT CALLBACK lvs_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_TIMER) { if (now_ms() - lvs_t0 > 200000) PostQuitMessage(0); InvalidateRect(h, NULL, TRUE); return 0; }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(0x20, 0x40, 0x80));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HFONT f = CreateFontW(72, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        WCHAR t[96];
        long s = (long)((now_ms() - lvs_t0) / 1000);
        _snwprintf(t, 95, L"Astra DS stream test\n%02ld:%02ld", s / 60, s % 60);
        t[95] = 0;
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(dc, of);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static int lvs_thread(void *arg) {
    (void)arg;
    lvs_quit = 0;
    WNDCLASSW wc = { .lpfnWndProc = lvs_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"AstraStreamTest",
                     .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW) };
    RegisterClassW(&wc);
    lvs_t0 = now_ms();
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Astra DS stream test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    SetTimer(h, 1, 500, NULL);
    if (h) SetForegroundWindow(h);              /* захват дизайнера ставит на паузу, когда источник не на переднем плане */
    lvs_hwnd = h;
    MSG m;
    while (!lvs_quit && GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (h) DestroyWindow(h);
    lvs_hwnd = NULL;
    return 0;
}
/* --streamsrc (только Video, скрытый): окно-источник для MC_LIVE=golive, живёт до 200 с или до завершения */
int stream_source_main(void) {
    lvs_thread(NULL);
    return 0;
}
#endif
/* MC_LIVE_SYNTH=1: синтетические кадры 1280x720 24 к/с прямо в кодер (без захвата окон и пауз) — проверка ровно
 * пути кодер -> golive_frame -> DAVE -> RTP. Кодер — тот же minih264 (реализация собрана в streamtest.c). */
#ifdef _WIN32
#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif
#include "../third_party/minih264/minih264e.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static volatile LONG syn_stop;
static volatile LONG syn_frames, syn_bytes_kb;
/* цифра 0-9 семисегментно, сегменты — прямоугольники в Y */
static void syn_rect(u8 *y, int x0, int y0, int w, int h, u8 v) {
    for (int r = y0; r < y0 + h && r < 720; r++) if (r >= 0) for (int c = x0; c < x0 + w && c < 1280; c++) if (c >= 0) y[r * 1280 + c] = v;
}
static void syn_digit(u8 *y, int x, int yy, int d) {
    static const u8 seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int s = seg[d % 10], W = 90, H = 160, T = 18;
    if (s & 1) syn_rect(y, x, yy, W, T, 235);
    if (s & 2) syn_rect(y, x + W - T, yy, T, H / 2, 235);
    if (s & 4) syn_rect(y, x + W - T, yy + H / 2, T, H / 2, 235);
    if (s & 8) syn_rect(y, x, yy + H - T, W, T, 235);
    if (s & 16) syn_rect(y, x, yy + H / 2, T, H / 2, 235);
    if (s & 32) syn_rect(y, x, yy, T, H / 2, 235);
    if (s & 64) syn_rect(y, x, yy + H / 2 - T / 2, W, T, 235);
}
static int syn_thread(void *arg) {
    (void)arg;
    const int W = 1280, Hh = 720, FPS = 30;
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = W; cp.height = Hh; cp.gop = FPS * 2; cp.vbv_size_bytes = 2000 * 1000 / 8;
    int szp = 0, szs = 0;
    if (H264E_sizeof(&cp, &szp, &szs)) return 1;
    H264E_persist_t *enc = malloc((size_t)szp);
    H264E_scratch_t *scr = malloc((size_t)szs);
    u8 *yb = malloc((size_t)W * Hh), *ub = malloc((size_t)W * Hh / 4), *vb = malloc((size_t)W * Hh / 4);
    if (!enc || !scr || !yb || !ub || !vb || H264E_init(enc, &cp)) { free(enc); free(scr); free(yb); free(ub); free(vb); return 1; }
    memset(ub, 128, (size_t)W * Hh / 4);
    memset(vb, 90, (size_t)W * Hh / 4);
    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    rp.encode_speed = 8; rp.qp_min = 18; rp.qp_max = 51;
    rp.desired_frame_bytes = 2000 * 1000 / 8 / FPS;
    H264E_io_yuv_t io;
    io.yuv[0] = yb; io.yuv[1] = ub; io.yuv[2] = vb;
    io.stride[0] = W; io.stride[1] = W / 2; io.stride[2] = W / 2;
    i64 t0 = now_ms();
    for (int f = 0; !syn_stop; f++) {
        memset(yb, 60, (size_t)W * Hh);
        syn_rect(yb, (f * 12) % (W - 80), 0, 80, Hh, 200);          /* бегущая полоса */
        int s = (int)((now_ms() - t0) / 1000);
        syn_digit(yb, 480, 280, s / 100); syn_digit(yb, 590, 280, s / 10); syn_digit(yb, 700, 280, s);
        rp.frame_type = f % FPS == 0 ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;   /* ключевой раз в секунду */
        unsigned char *out = NULL;
        int n = 0;
        if (!H264E_encode(enc, scr, &rp, &io, &out, &n) && n > 0) {
            if (f == 0) {
                char nl[300] = "";
                size_t o = 0;
                for (int i = 0; i + 3 < n && o < sizeof nl - 32; i++)
                    if (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 1) {
                        int sc = i > 0 && out[i - 1] == 0 ? 4 : 3;
                        o += (size_t)snprintf(nl + o, sizeof nl - o, " [%d@%d sc%d]", out[i + 3] & 31, i, sc);
                    }
                say("  синтетика: ключевой кадр %d Б, NAL (тип@смещение, старт-код):%s; хвост %02x %02x\n", n, nl, out[n - 2], out[n - 1]);
            }
            golive_frame(out, (size_t)n, rp.frame_type == H264E_FRAME_TYPE_KEY, (u32)((now_ms() - t0) * 90));
            InterlockedIncrement(&syn_frames);
            InterlockedExchangeAdd(&syn_bytes_kb, n / 1024);
        }
        i64 due = t0 + (i64)(f + 1) * 1000 / FPS, nw = now_ms();
        if (due > nw) Sleep((DWORD)(due - nw));
    }
    free(enc); free(scr); free(yb); free(ub); free(vb);
    return 0;
}
#endif
static const char *const gl_phase_name[] = { "OFF", "CREATING", "WAIT_SERVER", "CONNECTING", "LIVE", "FAILED" };
static void lv_golive_print(const char *tag) {
    GoLiveState g;
    golive_state(&g);
    VStreamInfo vi;
    voice_stream_info(&vi);
    ShareState sh;
    share_state(&sh);
    char ops[400] = "";
    size_t o = 0;
    for (int i = 0; i < 40; i++) if (vi.ops[i] && o < sizeof ops - 16) o += (size_t)snprintf(ops + o, sizeof ops - o, " %d:%u", i, vi.ops[i]);
    say("  [%s] golive %s%s | соединение: %s сессия=%d ssrc_видео=%d udp=%d DAVE=%s(v%d) пакетов=%lld очередь=%d | ops:%s\n",
        tag, gl_phase_name[g.phase >= 0 && g.phase <= 5 ? g.phase : 0], g.paused ? " (пауза)" : "",
        vi.running ? "идёт" : "нет", vi.session_ready, vi.have_vssrc, vi.udp, vi.dave ? (vi.dave_on ? "готов" : "ждём") : "нет", vi.dave_ver,
        vi.packets, vi.queued, ops[0] ? ops : " -");
    say("        кадры: в эфир %lld (ключевых %lld, %lld КБ), отброшено %lld | захват: %s %dx%d@%d, %.1f к/с, %.2f Мбит/с%s%s%s\n",
        g.frames, g.keyframes, g.bytes / 1024, g.dropped, sh.active ? "идёт" : "нет", sh.w, sh.h, sh.fps, sh.fps_out, sh.mbps,
        sh.reason[0] ? " (" : "", sh.reason, sh.reason[0] ? ")" : "");
    if (sh.paused) say("        пауза захвата: %s%s%s\n", sh.pause_why[0] ? sh.pause_why : "?", sh.cover[0] ? " — перекрыло: " : "", sh.cover);
    say("        сервер трансляции: %s:%d, curl %d, закрытие WS %d; DAVE видео: зашифровано %lld, ошибок %lld; PLI %lld, NACK %lld, RTX %lld\n", vi.host[0] ? vi.host : "-", vi.port,
        vi.curl_rc, vi.ws_close, vi.dave_ok, vi.dave_fail, vi.pli, vi.nack, vi.rtx);
    if (g.err[0]) say("        ошибка: %s\n", g.err);
}
/* MC_LIVE=golive: звонок другу в ЛС -> он принимает -> трансляция своего окна 720p24 90 с -> стоп, выход */
static int live_golive(void) {
    if (!lv_friend()) { term_puts("  задайте MC_LIVE_FRIEND\n"); return 1; }
    term_puts("\n  Go Live: звонок в ЛС, трансляция своего окна 90 с\n");
    int type = 0;
    u64 uid = lv_find_user(lv_friend(), &type);
    if (type != REL_FRIEND) { say("  %s — не друг (тип %d) — стоп\n", lv_friend(), type); return 2; }
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].type == CH_DM && S.chans[i].nrecip == 1 && S.chans[i].recipients[0] == uid) LV_DM = S.chans[i].id;
    if (!LV_DM) { dm_open(uid); LV_WAIT(S.open_ch != 0, 20000); LV_DM = S.open_ch; }
    if (!LV_DM) { term_puts("  ЛС не открылась — стоп\n"); return 2; }
    say("  звоню (ЛС %llu), жду до 60 с, пока примут...\n", (unsigned long long)LV_DM);
    call_start(LV_DM);
    VoiceStatus vs;
    i64 end = now_ms() + 60000;
    int last_ph = -1;
    do {
        lv_pump(200);
        voice_status(&vs);
        if ((int)vs.phase != last_ph) {
            last_ph = (int)vs.phase;
            char vh[128];
            int vp, vc, vw;
            voice_diag(vh, sizeof vh, &vp, &vc, &vw);
            say("  голос: фаза %d%s%s | сервер %s:%d, curl %d, закрытие WS %d\n", (int)vs.phase, vs.error[0] ? " — " : "", vs.error,
                vh[0] ? vh : "-", vp, vc, vw);
        }
    } while (vs.phase != VS_CONNECTED && vs.phase != VS_FAILED && now_ms() < end);
    if (vs.phase != VS_CONNECTED) { say("  звонок не соединился (фаза %d) — выход\n", (int)vs.phase); voice_leave(); lv_pump(2000); return 1; }
    say("  звонок соединён (DAVE %s)\n", vs.dave ? "да" : "нет");
    uintptr_t src = 0;
    const char *synth_env = getenv("MC_LIVE_SYNTH");
    bool synth = synth_env && *synth_env == '1';
#ifdef _WIN32
    PlatThread *synt = NULL;
    /* окно-источник — дочерним процессом этой же программы (--streamsrc): свои окна в список источников не входят */
    PROCESS_INFORMATION pi = {0};
    if (synth) {
        term_puts("  источник: синтетические кадры 1280x720 30 к/с (MC_LIVE_SYNTH=1), захват экрана не используется\n");
    } else {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(cmd, MAX_PATH + 31, L"\"%ls\" --streamsrc", exe);
        cmd[MAX_PATH + 31] = 0;
        STARTUPINFOW si = { .cb = sizeof si };
        if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) say("  окно-источник: процесс не запустился\n");
        else {                                  /* в Job Object с KILL_ON_JOB_CLOSE: окно не переживёт сценарий */
            static HANDLE job;
            if (!job) {
                job = CreateJobObjectW(NULL, NULL);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
                li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
            }
            if (job) AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
        }
    for (int i = 0; i < 60 && !src; i++) {
        lv_pump(100);
        char titles[24][80];
        uintptr_t ids[24];
        int nw = share_windows(titles, ids, 24);
        for (int k = 0; k < nw; k++) {
            DWORD wp = 0;
            GetWindowThreadProcessId((HWND)ids[k], &wp);
            if (pi.dwProcessId && wp == pi.dwProcessId) src = ids[k];
        }
    }
    say("  окно-источник: %s\n", src ? "найдено в списке источников" : "НЕТ (весь экран)");
    if (src) { AllowSetForegroundWindow(pi.dwProcessId); ShowWindow((HWND)src, SW_RESTORE); SetForegroundWindow((HWND)src); }
    }
#endif
    char err[160] = "";
    if (!synth && !share_start(0, src, err, sizeof err)) say("  share_start: %s\n", err);
    if (!golive_start(0, LV_DM, err, sizeof err)) say("  golive_start: %s\n", err);
#ifdef _WIN32
    if (synth) { syn_stop = 0; synt = thread_start(syn_thread, NULL); }
#endif
    i64 t0 = now_ms(), next = t0, not_conn_since = 0;
    int lastg = -1, lastv = (int)VS_CONNECTED;
    while (now_ms() - t0 < 90000) {
        lv_pump(200);
        voice_status(&vs);
        GoLiveState g;
        golive_state(&g);
        if (g.phase != lastg) { lastg = g.phase; lv_golive_print("фаза"); }
#ifdef _WIN32
        if (src && GetForegroundWindow() != (HWND)src && now_ms() % 3000 < 200) SetForegroundWindow((HWND)src);
#endif
        if (now_ms() >= next) { next += 5000; char tg[32]; snprintf(tg, sizeof tg, "%3lld с", (long long)((now_ms() - t0) / 1000)); lv_golive_print(tg); }
        /* прогон 6: при старте трансляции звонок на время пересогласования DAVE уходит в VS_DAVE (3) — это не обрыв.
         * Прерываем только на IDLE/FAILED или если звонок не CONNECTED дольше 15 с. */
        if ((int)vs.phase != lastv) {
            lastv = (int)vs.phase;
            char vh[128];
            int vp, vc, vw;
            voice_diag(vh, sizeof vh, &vp, &vc, &vw);
            say("  голос в эфире: фаза %d%s%s | сервер %s:%d, curl %d, закрытие WS %d\n", lastv, vs.error[0] ? " — " : "", vs.error,
                vh[0] ? vh : "-", vp, vc, vw);
        }
        if (vs.phase == VS_IDLE || vs.phase == VS_FAILED) { say("  звонок прервался (фаза %d %s)\n", (int)vs.phase, vs.error); break; }
        if (vs.phase != VS_CONNECTED) { if (!not_conn_since) not_conn_since = now_ms(); }
        else not_conn_since = 0;
        if (not_conn_since && now_ms() - not_conn_since > 15000) { say("  звонок не CONNECTED дольше 15 с (фаза %d) — стоп\n", (int)vs.phase); break; }
    }
    lv_golive_print("итог");
#ifdef _WIN32
    if (synt) { syn_stop = 1; thread_join(synt); say("  синтетика: кадров %ld, %ld КБ\n", (long)syn_frames, (long)syn_bytes_kb); }
#endif
    golive_stop();
    share_stop();
#ifdef _WIN32
    if (pi.hProcess) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
#endif
    lv_pump(1500);
    voice_leave();
    lv_pump(2000);
    term_puts("  трансляция остановлена, из звонка вышли\n");
    return 0;
}
#endif

int selftest_live(void) {
    term_puts("Astra DS --livetest: записи на живом аккаунте (свой тестовый сервер; MC_LIVE=dm — ЛС с MC_LIVE_FRIEND)\n");
    g_headless = true;
    if (!single_instance()) { term_puts("Astra DS уже запущен — сначала закройте его\n"); return 1; }
    if (!secret_init()) return 1;
    plat_net_init();
    if (!net_init()) { term_puts("сеть не запустилась\n"); return 1; }
    if (!cred_load()) { term_puts("нет сохранённого токена\n"); return 1; }
    /* VLESS: сохранённая ссылка — через туннель; не работает — стоп (напрямую молча не идём) */
    if (!vless_load_saved() && vless_state() != VL_OFF) { say("VLESS настроен, но не работает: %s — стоп\n", vless_error()); return 1; }
    say("  маршрут: %s\n", vless_state() == VL_OFF ? "напрямую (VLESS не настроен)" : "через VLESS");
    ui_init();
    g_http_guard = lv_guard;
    g_http_trace = lv_trace;
    gw_start();
    LV_WAIT(S.ready && !strcmp(gw_status_text(), "в сети"), 60000);
    check(S.ready, "READY (по одному серверу) и gateway «в сети»");
    if (!S.ready) return 1;
    say("  серверов %d, каналов %d, друзей/заявок %d\n", S.nguilds, S.nchans, S.nrels);
    int rc = 0;
    const char *mode = getenv("MC_LIVE");
    if (!mode || !strcmp(mode, "guild")) rc |= live_guild();
    if (mode && !strcmp(mode, "dm")) rc |= live_dm();
    if (mode && !strcmp(mode, "probe")) rc |= live_probe();
    if (mode && !strcmp(mode, "captcha")) rc |= live_captcha();
    if (mode && !strcmp(mode, "wave2")) rc |= live_wave2();
    if (mode && !strcmp(mode, "join")) rc |= live_join();
    if (mode && !strcmp(mode, "swap")) rc |= live_swap();
#ifdef ASTRA_VIDEO
    if (mode && !strcmp(mode, "golive")) rc |= live_golive();
#endif
    if (mode && !strcmp(mode, "friends")) { g_readonly = true; rc |= live_friends(); }   /* только чтение */
    say("\n  записей: %d, ошибок ответа: %d, остановлено сторожем: %d\n", lv_writes, lv_errors, lv_blocked);
    say("Итог: %d ok, %d FAIL\n", passes, fails);
    g_http_guard = NULL;
    g_http_trace = NULL;
    gw_stop();
    vless_stop();
    net_cleanup();
    secret_destroy();
    return fails || rc ? 1 : 0;
}
