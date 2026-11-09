/* Окно-терминал, общая часть для Windows и X11: сетка символов, разбор потока VT (тот же, что
 * ui.c пишет в консоль), очередь ввода и построчный вход (токен, ссылка VLESS) внутри окна.
 * Понимает: CUP/CUU/CUD/CUF/CUB/CHA/VPA, ED, EL, SGR 0/1/4/7/22/24/27, цвета 30-37/40-47/90-97/100-107,
 * 39/49, 38;5;n/48;5;n, 38;2;r;g;b/48;2;r;g;b; ?2026 (синхронный вывод), ?1049 (как очистка), BEL, CR, LF, BS, TAB.
 * Память: cols*rows*(4+1+1+1) байт ячеек + байт на строку + палитра 1 КБ. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui.h"

Grid G;

/* ---------- палитра ---------- */
static u8 pal_idx(u32 rgb) {
    rgb &= 0xFFFFFF;
    for (int i = 1; i <= G.npal; i++) if (G.pal[i] == rgb) return (u8)i;
    if (G.npal < 255) { G.pal[++G.npal] = rgb; return (u8)G.npal; }
    /* палитра полна: ближайший известный цвет */
    int best = 1;
    long bd = -1;
    for (int i = 1; i <= G.npal; i++) {
        long dr = (long)((G.pal[i] >> 16) & 255) - (long)((rgb >> 16) & 255);
        long dg = (long)((G.pal[i] >> 8) & 255) - (long)((rgb >> 8) & 255);
        long db = (long)(G.pal[i] & 255) - (long)(rgb & 255);
        long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return (u8)best;
}
/* 16 цветов терминала (как в Windows Terminal) и куб 6x6x6 + серые для 38;5;n */
static u32 xterm_rgb(int n) {
    static const u32 base[16] = { 0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
                                  0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2 };
    if (n < 16) return base[n < 0 ? 0 : n];
    if (n < 232) {
        n -= 16;
        static const u8 lv[6] = { 0, 95, 135, 175, 215, 255 };
        return (u32)lv[n / 36] << 16 | (u32)lv[(n / 6) % 6] << 8 | lv[n % 6];
    }
    u32 g = (u32)(8 + (n > 255 ? 23 : n - 232) * 10);
    return g << 16 | g << 8 | g;
}
void grid_colors(u8 at, u8 fg, u8 bg, u32 *rf, u32 *rb) {
    u32 f = fg ? G.pal[fg] : G.def_fg, b = bg ? G.pal[bg] : G.def_bg;
    if (at & A_INV) { u32 t = f; f = b; b = t; }
    *rf = f;
    *rb = b;
}

static void layers_clear(void);

/* ---------- сетка ---------- */
void grid_mark_all(void) { if (G.dirty) memset(G.dirty, 1, (size_t)G.rows); }

void grid_resize(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > 2000) cols = 2000;
    if (rows > 1000) rows = 1000;
    if (G.ch && cols == G.cols && rows == G.rows) return;
    layers_clear();                              /* до освобождения старых массивов: он помечает строки */
    size_t n = (size_t)cols * (size_t)rows;
    u32 *ch = xmalloc(n * sizeof *ch);
    u8 *at = xcalloc(n, 1), *fg = xcalloc(n, 1), *bg = xcalloc(n, 1);
    u8 *dirty = xmalloc((size_t)rows);
    for (size_t i = 0; i < n; i++) ch[i] = ' ';
    if (G.ch) {
        /* построчный вход держит низ экрана: при уменьшении высоты срезаются верхние строки */
        int sy = !G.ui && G.rows > rows ? G.rows - rows : 0;
        int cw = cols < G.cols ? cols : G.cols;
        for (int y = 0; y < rows && y + sy < G.rows; y++) {
            size_t d = (size_t)y * cols, s = (size_t)(y + sy) * G.cols;
            memcpy(ch + d, G.ch + s, (size_t)cw * sizeof *ch);
            memcpy(at + d, G.at + s, (size_t)cw);
            memcpy(fg + d, G.fg + s, (size_t)cw);
            memcpy(bg + d, G.bg + s, (size_t)cw);
        }
        G.cy -= sy;
        free(G.ch); free(G.at); free(G.fg); free(G.bg); free(G.dirty);
    }
    G.ch = ch; G.at = at; G.fg = fg; G.bg = bg; G.dirty = dirty;
    G.cols = cols; G.rows = rows;
    if (G.cx > cols) G.cx = cols;
    if (G.cy >= rows) G.cy = rows - 1;
    if (G.cy < 0) G.cy = 0;
    grid_mark_all();
}

/* ---------- слои мелкой сетки (картинки), формат — в gui.h ---------- */
static u64 lay_tick;
static size_t lay_bytes(const Layer *l) { return (size_t)(l->k * l->w) * (size_t)(l->k * l->h) * 7; }
static void lay_mark(const Layer *l) {
    for (int y = l->y; y < l->y + l->h && y < G.rows; y++) if (G.dirty) G.dirty[y] = 1;
}
static void lay_drop(int i) {
    lay_mark(&G.lay[i]);
    free(G.lay[i].c);
    memmove(&G.lay[i], &G.lay[i + 1], (size_t)(G.nlay - i - 1) * sizeof *G.lay);
    G.nlay--;
}
static void layers_clear(void) { while (G.nlay) lay_drop(G.nlay - 1); }
/* обычная запись в клетку: слой под ней — снять в конце кадра (если не придёт такой же) */
static void layers_touch(int x, int y) {
    for (int i = 0; i < G.nlay; i++) {
        Layer *l = &G.lay[i];
        if (x >= l->x && x < l->x + l->w && y >= l->y && y < l->y + l->h) l->dead = true;
    }
}
static void layers_reap(void) {
    for (int i = G.nlay - 1; i >= 0; i--) if (G.lay[i].dead) lay_drop(i);
}
static bool lay_overlap(const Layer *a, int x, int y, int w, int h) {
    return a->x < x + w && x < a->x + a->w && a->y < y + h && y < a->y + a->h;
}
static void lay_commit(int x, int y, int w, int h, int k, u8 *c, u32 hash) {
    for (int i = 0; i < G.nlay; i++) {
        Layer *l = &G.lay[i];
        if (l->x == x && l->y == y && l->w == w && l->h == h && l->k == k && l->hash == hash) {
            l->dead = false;                     /* тот же слой: ничего не перерисовывать */
            l->stamp = ++lay_tick;
            free(c);
            return;
        }
    }
    for (int i = G.nlay - 1; i >= 0; i--)
        if ((G.lay[i].x == x && G.lay[i].y == y && G.lay[i].w == w) || lay_overlap(&G.lay[i], x, y, w, h)) lay_drop(i);
    Layer nl = { x, y, w, h, k, hash, false, ++lay_tick, c };
    size_t total = lay_bytes(&nl);
    for (int i = 0; i < G.nlay; i++) total += lay_bytes(&G.lay[i]);
    while (G.nlay && (G.nlay >= LAYER_MAX || total > LAYER_BYTES)) {
        int old = 0;
        for (int i = 1; i < G.nlay; i++) if (G.lay[i].stamp < G.lay[old].stamp) old = i;
        total -= lay_bytes(&G.lay[old]);
        lay_drop(old);
    }
    G.lay[G.nlay++] = nl;
    lay_mark(&nl);
}

/* Потоковый разбор ESC _ mincord-img ; X ; Y ; W ; H ; K ; DATA ESC \ — без накопления сырых данных. */
static struct {
    bool on, bad, data;
    char hdr[64];
    int hn, semi, x, y, w, h, k, rn;
    size_t need, got;
    unsigned char rec[13];
    u8 *c;
    u32 hash;
} A;
static void apc_begin(void) {
    free(A.c);
    memset(&A, 0, sizeof A);
    A.on = true;
    A.hash = 2166136261u;
}
static int hexv(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void apc_header(void) {
    A.hdr[A.hn] = 0;
    int x, y, w, h, k, n = 0;
    if (sscanf(A.hdr, "astra-img;%d;%d;%d;%d;%d;%n", &x, &y, &w, &h, &k, &n) != 5 || n != A.hn) { A.bad = true; return; }
    int kmax = gb_fine_scale();
    if (k < 2 || k > 4 || k > kmax || w < 1 || h < 1 || x < 0 || y < 0 || w > G.cols || h > G.rows ||
        x > G.cols - w || y > G.rows - h) { A.bad = true; return; }
    size_t cells = (size_t)(k * w) * (size_t)(k * h);
    if (cells * 7 > LAYER_BYTES) { A.bad = true; return; }
    A.c = malloc(cells * 7);
    if (!A.c) { A.bad = true; return; }
    A.x = x; A.y = y; A.w = w; A.h = h; A.k = k;
    A.need = cells;
    A.data = true;
}
static void apc_byte(unsigned char c) {
    if (!A.on || A.bad) return;
    if (!A.data) {
        if (A.hn >= (int)sizeof A.hdr - 1) { A.bad = true; return; }
        A.hdr[A.hn++] = (char)c;
        if (c == ';' && ++A.semi == 6) apc_header();
        return;
    }
    A.rec[A.rn++] = c;
    if (A.rn < 13) return;
    A.rn = 0;
    if (A.got >= A.need || A.rec[0] < 0x20 || A.rec[0] > 0x7E) { A.bad = true; return; }
    u8 *o = A.c + A.got * 7;
    o[0] = A.rec[0];
    for (int i = 0; i < 6; i++) {
        int hi = hexv(A.rec[1 + 2 * i]), lo = hexv(A.rec[2 + 2 * i]);
        if (hi < 0 || lo < 0) { A.bad = true; return; }
        o[1 + i] = (u8)(hi << 4 | lo);
    }
    for (int i = 0; i < 7; i++) A.hash = (A.hash ^ o[i]) * 16777619u;
    A.got++;
}
static void apc_end(void) {
    if (A.on && !A.bad && A.data && A.got == A.need && A.rn == 0) {
        u32 h = A.hash;
        int hv[5] = { A.x, A.y, A.w, A.h, A.k };
        for (int i = 0; i < 5; i++) h = (h ^ (u32)hv[i]) * 16777619u;
        lay_commit(A.x, A.y, A.w, A.h, A.k, A.c, h);
        A.c = NULL;
    }
    free(A.c);
    memset(&A, 0, sizeof A);
}
static void apc_abort(void) { if (A.on) A.bad = true; }

static void put_cell(int x, int y, u32 c, u8 a, u8 f, u8 b) {
    size_t i = (size_t)y * (size_t)G.cols + (size_t)x;
    if (G.nlay) layers_touch(x, y);
    if (G.ch[i] != c || G.at[i] != a || G.fg[i] != f || G.bg[i] != b) {
        G.ch[i] = c; G.at[i] = a; G.fg[i] = f; G.bg[i] = b;
        G.dirty[y] = 1;
    }
}
/* Стирание — текущим цветом фона (как в xterm): так интерфейс заливает панели через ESC[K. */
static void erase(int y, int x0, int x1) {          /* [x0, x1) строки y */
    if (x0 < 0) x0 = 0;
    if (x1 > G.cols) x1 = G.cols;
    if (x0 > 0 && x0 < G.cols && G.ch[(size_t)y * G.cols + x0] == 0) x0--;   /* не резать широкий символ */
    if (x1 < G.cols && G.ch[(size_t)y * G.cols + x1] == 0) put_cell(x1, y, ' ', 0, 0, G.cur_bg);
    for (int x = x0; x < x1; x++) put_cell(x, y, ' ', 0, 0, G.cur_bg);
}
static void erase_all(void) { for (int y = 0; y < G.rows; y++) erase(y, 0, G.cols); }
static void sgr_reset(void) { G.cur_at = 0; G.cur_fg = 0; G.cur_bg = 0; }

static void scroll_up(void) {
    layers_clear();
    size_t row = (size_t)G.cols, keep = (size_t)(G.rows - 1) * row;
    memmove(G.ch, G.ch + row, keep * sizeof *G.ch);
    memmove(G.at, G.at + row, keep);
    memmove(G.fg, G.fg + row, keep);
    memmove(G.bg, G.bg + row, keep);
    for (size_t i = keep; i < keep + row; i++) { G.ch[i] = ' '; G.at[i] = 0; G.fg[i] = 0; G.bg[i] = G.cur_bg; }
    grid_mark_all();
}
static void newline(void) {
    G.cx = 0;
    if (G.cy + 1 >= G.rows) scroll_up(); else G.cy++;
}

static void print_cp(u32 c) {
    int w = cp_width(c);
    if (w <= 0) return;                          /* нулевой ширины (комбинирующие) не рисуем */
    if (G.cx + w > G.cols) {
        if (G.ui) { G.cx = G.cols; return; }     /* интерфейс сам укладывает строку: обрезать */
        newline();
        if (w > G.cols) return;
    }
    int x = G.cx, y = G.cy;
    size_t i = (size_t)y * G.cols + x;
    if (G.ch[i] == 0 && x > 0) put_cell(x - 1, y, ' ', G.at[i - 1], G.fg[i - 1], G.bg[i - 1]);   /* затёрта правая половина */
    int end = x + w;                             /* после символа: хвост чужого широкого символа */
    if (end < G.cols && G.ch[i + (size_t)w] == 0) put_cell(end, y, ' ', G.at[i + (size_t)w], G.fg[i + (size_t)w], G.bg[i + (size_t)w]);
    put_cell(x, y, c, G.cur_at, G.cur_fg, G.cur_bg);
    if (w == 2) put_cell(x + 1, y, 0, G.cur_at, G.cur_fg, G.cur_bg);
    G.cx += w;
}

static void sgr(void) {
    if (G.np == 0) { sgr_reset(); return; }
    for (int i = 0; i < G.np; i++) {
        int p = G.par[i];
        if (p >= 30 && p <= 37) G.cur_fg = pal_idx(xterm_rgb(p - 30));
        else if (p >= 90 && p <= 97) G.cur_fg = pal_idx(xterm_rgb(p - 90 + 8));
        else if (p >= 40 && p <= 47) G.cur_bg = pal_idx(xterm_rgb(p - 40));
        else if (p >= 100 && p <= 107) G.cur_bg = pal_idx(xterm_rgb(p - 100 + 8));
        else switch (p) {
        case 0: sgr_reset(); break;
        case 1: G.cur_at |= A_BOLD; break;
        case 4: G.cur_at |= A_UL; break;
        case 7: G.cur_at |= A_INV; break;
        case 3: G.cur_at |= A_ITAL; break;
        case 9: G.cur_at |= A_STRIKE; break;
        case 23: G.cur_at &= (u8)~A_ITAL; break;
        case 29: G.cur_at &= (u8)~A_STRIKE; break;
        case 22: G.cur_at &= (u8)~A_BOLD; break;
        case 24: G.cur_at &= (u8)~A_UL; break;
        case 27: G.cur_at &= (u8)~A_INV; break;
        case 39: G.cur_fg = 0; break;
        case 49: G.cur_bg = 0; break;
        case 38: case 48: case 58: {             /* 38;5;n или 38;2;r;g;b (58 — цвет подчёркивания: пропустить) */
            u32 rgb = 0;
            bool ok = false;
            if (i + 2 < G.np && G.par[i + 1] == 5) { rgb = xterm_rgb(G.par[i + 2] & 255); ok = true; i += 2; }
            else if (i + 4 < G.np && G.par[i + 1] == 2) {
                rgb = (u32)(G.par[i + 2] & 255) << 16 | (u32)(G.par[i + 3] & 255) << 8 | (u32)(G.par[i + 4] & 255);
                ok = true;
                i += 4;
            } else i = G.np;                     /* оборванная запись — остаток не разбирать */
            if (ok && p == 38) G.cur_fg = pal_idx(rgb);
            else if (ok && p == 48) G.cur_bg = pal_idx(rgb);
            break;
        }
        default: break;                          /* курсив, тусклый, мигание — не рисуем */
        }
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void csi(char f) {
    int p0 = G.np > 0 ? G.par[0] : 0, p1 = G.np > 1 ? G.par[1] : 0;
    if (G.priv == '?') {
        if (f != 'h' && f != 'l') return;
        for (int i = 0; i < G.np; i++) {
            if (G.par[i] == 2026) { G.sync = f == 'h'; if (!G.sync) { layers_reap(); gb_flush(); } }
            else if (G.par[i] == 1049) { sgr_reset(); erase_all(); G.cx = G.cy = 0; }
        }
        return;
    }
    if (G.priv) return;
    int cx = G.cx >= G.cols ? G.cols - 1 : G.cx;
    switch (f) {
    case 'H': case 'f': G.cy = clampi((p0 ? p0 : 1) - 1, 0, G.rows - 1); G.cx = clampi((p1 ? p1 : 1) - 1, 0, G.cols - 1); break;
    case 'A': G.cy = clampi(G.cy - (p0 ? p0 : 1), 0, G.rows - 1); G.cx = cx; break;
    case 'B': G.cy = clampi(G.cy + (p0 ? p0 : 1), 0, G.rows - 1); G.cx = cx; break;
    case 'C': G.cx = clampi(cx + (p0 ? p0 : 1), 0, G.cols - 1); break;
    case 'D': G.cx = clampi(cx - (p0 ? p0 : 1), 0, G.cols - 1); break;
    case 'G': G.cx = clampi((p0 ? p0 : 1) - 1, 0, G.cols - 1); break;
    case 'd': G.cy = clampi((p0 ? p0 : 1) - 1, 0, G.rows - 1); break;
    case 'J':
        if (p0 == 2 || p0 == 3) erase_all();
        else if (p0 == 1) { for (int y = 0; y < G.cy; y++) erase(y, 0, G.cols); erase(G.cy, 0, cx + 1); }
        else { erase(G.cy, G.cx, G.cols); for (int y = G.cy + 1; y < G.rows; y++) erase(y, 0, G.cols); }
        break;
    case 'K':
        if (p0 == 2) erase(G.cy, 0, G.cols);
        else if (p0 == 1) erase(G.cy, 0, cx + 1);
        else erase(G.cy, G.cx, G.cols);
        break;
    case 'm': sgr(); break;
    default: break;
    }
}

enum { S_GROUND, S_ESC, S_CSI, S_STR, S_STR_ESC, S_SKIP1 };

/* OSC 52 (ESC ] 52 ; c ; base64 BEL / ESC \): «копировать в буфер обмена». Текст — до 64 КБ;
 * временные буферы затираются сразу после передачи бэкенду. Прочие OSC/DCS пропускаются. */
#define OSC_TEXT_MAX (64u << 10)
#define OSC_RAW_MAX (OSC_TEXT_MAX / 3 * 4 + 64)
static char *osc;
static size_t osc_n;
static bool osc_on, osc_over;
static void osc_begin(bool is_osc) {
    osc_on = is_osc;
    osc_over = false;
    osc_n = 0;
}
static void osc_byte(unsigned char c) {
    if (!osc_on || osc_over) return;
    if (osc_n >= OSC_RAW_MAX) { osc_over = true; return; }
    if (!osc) osc = xmalloc(OSC_RAW_MAX);
    osc[osc_n++] = (char)c;
}
static int b64v(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static void osc_end(void) {
    if (osc_on && !osc_over && osc && osc_n > 3 && !memcmp(osc, "52;", 3)) {
        const char *d = memchr(osc + 3, ';', osc_n - 3);    /* после выбора буфера (c, p, s...) */
        if (d) {
            size_t i = (size_t)(d + 1 - osc), o = 0;
            char *txt = xmalloc(OSC_TEXT_MAX + 4);
            u32 acc = 0;
            int bits = 0;
            for (; i < osc_n && o < OSC_TEXT_MAX; i++) {
                int v = b64v((unsigned char)osc[i]);
                if (v < 0) continue;             /* '=' и мусор */
                acc = (acc << 6) | (u32)v;
                bits += 6;
                if (bits >= 8) { bits -= 8; txt[o++] = (char)((acc >> bits) & 0xFF); }
            }
            /* "?" вместо данных — запрос чтения буфера: не поддерживается (буфер не отдаём) */
            if (o > 0) gb_clipboard(txt, o);
            wipe(txt, OSC_TEXT_MAX + 4);
            free(txt);
            acc = 0;
        }
    }
    if (osc) { wipe(osc, osc_n); free(osc); osc = NULL; }
    osc_n = 0;
    osc_on = false;
}

void grid_write(const char *s, size_t n) {
    if (!G.ch) return;
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        switch (G.st) {
        case S_GROUND:
            if (G.uneed) {
                if ((c & 0xC0) == 0x80) {
                    G.u = (G.u << 6) | (c & 0x3F);
                    if (--G.uneed == 0) print_cp(G.u);
                    continue;
                }
                G.uneed = 0;
                print_cp('?');                   /* оборванная последовательность UTF-8 */
            }
            if (c >= 0x80) {
                if ((c & 0xE0) == 0xC0) { G.u = c & 0x1F; G.uneed = 1; }
                else if ((c & 0xF0) == 0xE0) { G.u = c & 0x0F; G.uneed = 2; }
                else if ((c & 0xF8) == 0xF0) { G.u = c & 0x07; G.uneed = 3; }
                else print_cp('?');
            } else if (c >= 0x20 && c != 0x7F) print_cp(c);
            else if (c == 0x1B) G.st = S_ESC;
            else if (c == '\n') newline();
            else if (c == '\r') G.cx = 0;
            else if (c == '\b') { if (G.cx > 0) G.cx--; }
            else if (c == '\t') G.cx = clampi((G.cx / 8 + 1) * 8, 0, G.cols - 1);
            else if (c == '\a') gb_bell();
            break;
        case S_ESC:
            if (c == '[') { G.st = S_CSI; G.np = 0; G.par[0] = 0; G.priv = 0; }
            else if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') { G.st = S_STR; osc_begin(c == ']'); if (c == '_') apc_begin(); }
            else if (c >= 0x20 && c <= 0x2F) G.st = S_SKIP1;
            else if (c == 'c') { sgr_reset(); erase_all(); G.cx = G.cy = 0; G.st = S_GROUND; }
            else G.st = S_GROUND;
            break;
        case S_CSI:
            if (c >= '0' && c <= '9') {
                if (G.np == 0) { G.np = 1; G.par[0] = 0; }
                int *p = &G.par[G.np - 1];
                if (*p < 100000) *p = *p * 10 + (c - '0');
            } else if (c == ';' || c == ':') {
                if (G.np == 0) { G.np = 1; G.par[0] = 0; }
                if (G.np < 16) G.par[G.np++] = 0;
            } else if (c >= 0x3C && c <= 0x3F) {
                if (G.np == 0) G.priv = (char)c;
            } else if (c >= 0x40 && c <= 0x7E) {
                csi((char)c);
                G.st = S_GROUND;
            } else if (c == 0x1B) G.st = S_ESC;
            else if (c < 0x20 || c > 0x7E) G.st = S_GROUND;   /* мусор: бросить последовательность */
            break;
        case S_STR:                              /* OSC/DCS и т. п.: до BEL или ESC \ */
            if (c == 0x07) { osc_end(); apc_abort(); apc_end(); G.st = S_GROUND; }
            else if (c == 0x1B) G.st = S_STR_ESC;
            else { osc_byte(c); apc_byte(c); }
            break;
        case S_STR_ESC:
            if (c == '\\') { osc_end(); apc_end(); G.st = S_GROUND; }
            else { osc_byte(0x1B); osc_byte(c); apc_abort(); G.st = S_STR; }
            break;
        case S_SKIP1: G.st = S_GROUND; break;
        }
    }
    if (!G.sync) { layers_reap(); gb_flush(); }
}

/* ---------- очередь ввода ---------- */
#define KQ_N 256
static Key kq[KQ_N];
static int kq_r, kq_w;
static u32 *paste;
static size_t paste_n, paste_i, paste_cap;

int gui_mouse_seen;                             /* --shot: сколько K_MOUSE пришло из событий окна */
void kq_push(const Key *k) {
    int nw = (kq_w + 1) % KQ_N;
    if (nw == kq_r) return;                      /* переполнение: лишнее отбросить */
    if (k->code == K_MOUSE) gui_mouse_seen++;
    kq[kq_w] = *k;
    kq_w = nw;
}
bool kq_pending(void) { return kq_r != kq_w || paste_i < paste_n; }

static void paste_add(u32 c) {
    static bool cr;
    if (c == '\n' && cr) { cr = false; return; }  /* \r\n -> один '\n' */
    cr = c == '\r';
    if (c == '\r') c = '\n';
    if (c == '\t') c = ' ';
    if ((c < 0x20 && c != '\n') || c == 0x7F || (c >= 0x80 && c < 0xA0)) return;
    if (paste_n >= (1u << 20)) return;           /* больше миллиона символов — хватит */
    if (paste_n == paste_cap) {
        size_t nc = paste_cap ? paste_cap * 2 : 256;
        u32 *np = xmalloc(nc * sizeof *np);
        if (paste_n) { memcpy(np, paste, paste_n * sizeof *np); wipe(paste, paste_cap * sizeof *paste); }
        free(paste);
        paste = np;
        paste_cap = nc;
    }
    paste[paste_n++] = c;
}
void kq_paste_utf8(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) {
        u32 c;
        int k = utf8_dec(s + i, &c);
        if (i + (size_t)k > n) break;
        paste_add(c);
        i += (size_t)k;
    }
}
void kq_paste_utf16(const uint16_t *s, size_t n) {
    for (size_t i = 0; i < n && s[i]; i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00u);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) c = '?';
        paste_add(c);
    }
}
void kq_wipe(void) {
    wipe(kq, sizeof kq);
    kq_r = kq_w = 0;
    if (paste) { wipe(paste, paste_cap * sizeof *paste); free(paste); }
    paste = NULL;
    paste_n = paste_i = paste_cap = 0;
}
int kq_pop(Key *out, int max) {
    int n = 0;
    while (n < max && kq_r != kq_w) {
        out[n++] = kq[kq_r];
        wipe(&kq[kq_r], sizeof kq[kq_r]);
        kq_r = (kq_r + 1) % KQ_N;
    }
    while (n < max && paste_i < paste_n) out[n++] = (Key){ .code = K_CHAR, .ch = paste[paste_i++] };
    if (paste && paste_i >= paste_n) {           /* вставка роздана: затереть (там мог быть токен) */
        wipe(paste, paste_cap * sizeof *paste);
        free(paste);
        paste = NULL;
        paste_n = paste_i = paste_cap = 0;
    }
    return n;
}

/* ---------- term_* поверх окна ---------- */
void gui_puts(const char *s) {
    grid_write(s, strlen(s));
    if (!G.ui) G.tail = true;
}

/* Поле ввода с позиции fx,fy до конца строки: скрытое — счётчик символов, обычное — хвост текста. */
static void draw_field(int fx, int fy, const char *s, size_t n, bool hidden, bool caret, bool bad) {
    Buf b = {0};
    buf_printf(&b, "\x1b[%d;%dH\x1b[0m", fy + 1, fx + 1);
    int room = G.cols - fx - 1;
    if (hidden) {
        size_t cps = 0;
        for (size_t i = 0; i < n; i++) cps += ((unsigned char)s[i] & 0xC0) != 0x80;
        char t[96];
        snprintf(t, sizeof t, "[ввод скрыт: %zu симв.]%s", cps, bad ? " СЛИШКОМ ДЛИННО (Esc - стереть)" : "");
        buf_puts(&b, t);
    } else {
        /* показать хвост, умещающийся в строку */
        size_t st = 0;
        int w = 0;
        for (size_t i = n; i > 0; ) {
            size_t j = i - 1;
            while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
            u32 c;
            utf8_dec(s + j, &c);
            int cw = cp_width(c);
            if (w + cw > room) { st = i; break; }
            w += cw;
            i = j;
        }
        buf_put(&b, s + st, n - st);
    }
    if (caret) buf_puts(&b, "\x1b[7m \x1b[0m");
    buf_puts(&b, "\x1b[K");
    grid_write(b.p, b.len);
    buf_wipe(&b);
}

bool gui_read_line(const char *prompt, char *out, size_t cap, bool hidden) {
    if (!cap) return false;
    out[0] = 0;
    gui_puts(prompt);
    if (G.cx > G.cols - 12) grid_write("\n", 1);
    int fx = G.cx, fy = G.cy;
    size_t n = 0;
    bool bad = false, ok = false, done = false;
    draw_field(fx, fy, out, n, hidden, true, bad);
    while (!done && !G.closed) {
        gb_pump(kq_pending() ? 0 : -1);
        Key k[16];
        int m;
        while (!done && (m = kq_pop(k, 16)) > 0) {
            for (int i = 0; i < m && !done; i++) {
                const Key *e = &k[i];
                if (e->code == K_ENTER) { done = true; ok = !bad; }
                else if (e->code == K_CHAR && e->ctrl && (e->ch == 'q' || e->ch == 'c')) { done = true; ok = false; }
                else if (e->code == K_ESC) { wipe(out, n); n = 0; bad = false; }
                else if (e->code == K_BS && n > 0 && !bad) {
                    while (n > 0) {             /* стереть последний символ UTF-8 целиком */
                        unsigned char b = (unsigned char)out[--n];
                        out[n] = 0;
                        if ((b & 0xC0) != 0x80) break;
                    }
                } else if (e->code == K_CHAR && !e->ctrl && !e->alt && e->ch >= 0x20 && e->ch != '\n') {
                    if (hidden && (e->ch == ' ' || e->ch == '"' || e->ch == '\t')) continue;
                    char u[4];
                    int l = utf8_enc(e->ch, u);
                    if (n + (size_t)l >= cap) bad = true;
                    else { memcpy(out + n, u, (size_t)l); n += (size_t)l; out[n] = 0; }
                    wipe(u, sizeof u);
                }
            }
            wipe(k, sizeof k);
        }
        draw_field(fx, fy, out, n, hidden, !done, bad);
    }
    if (!ok) { wipe(out, cap); out[0] = 0; }
    else wipe(out + n, cap - n);
    grid_write("\x1b[0m\n", 5);
    G.tail = false;
    return ok;
}

bool gui_init(void) {
    G.ui = true;
    G.tail = false;
    sgr_reset();
    G.sync = false;
    erase_all();
    G.cx = G.cy = 0;
    gb_flush();
    return true;
}
void gui_shutdown(void) {
    if (!G.ui) return;
    G.ui = false;
    G.sync = false;
    sgr_reset();
    erase_all();
    G.cx = G.cy = 0;
    kq_wipe();
    gb_flush();
}
void gui_size(int *w, int *h) { *w = G.cols; *h = G.rows; }
void gui_resize(int cols, int rows) {
    grid_resize(cols, rows);
    if (G.ui) kq_push(&(Key){ .code = K_RESIZE });
}
void gui_write(const char *s, size_t n) { grid_write(s, n); }
int gui_read(Key *out, int max) {
    gb_pump(0);
    return kq_pop(out, max);
}

/* Окно исчезнет вместе с процессом: если напоследок напечатана ошибка (вход не удался, токен не
 * принят), дать её прочитать — до клавиши или закрытия окна. */
void gui_atexit(void) {
    if (!G.ch || G.closed || !G.tail || G.ui) return;
    gui_puts("\n\x1b[1m[любая клавиша или закрыть окно - выход]\x1b[0m");
    kq_wipe();
    while (!G.closed) {
        gb_pump(-1);
        Key k[8];
        int m = kq_pop(k, 8);
        bool any = false;
        for (int i = 0; i < m; i++) if (k[i].code != K_RESIZE && k[i].code != K_MOUSE) any = true;
        if (any) break;
    }
}
