/* Окно-терминал: бэкенд Linux (Xlib, без Xft/fontconfig/freetype — ради памяти).
 * Шрифт — core-шрифт X-сервера, моноширинный с ISO10646 (misc-fixed), размер по DPI экрана;
 * метрики берутся через XListFontsWithInfo — без таблицы метрик на каждый из 65536 символов
 * (XLoadQueryFont для iso10646 держал бы ~0,8 МБ). Рисование — XDrawImageString16 (UCS-2;
 * символы вне BMP — '?') прогонами одинаковых атрибутов и цветов, только изменившиеся строки
 * (и области Expose). Ожидание без опроса: дескриптор соединения ConnectionNumber — в общий poll;
 * события, уже прочитанные Xlib в свою очередь, будят poll через eventfd.
 * Снимок (--shot, только с --demo): окно не отображается (не map), сетка рисуется в Pixmap -> BMP. */
#ifndef _WIN32
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include "gui.h"
#include "astra_icon.h"

static Display *dpy;
static Window win;
static GC gc;
static Font f_norm, f_bold;                     /* f_bold = 0 — жирный двойной печатью */
static Font f_ital;                             /* наклонный (SGR 3); 0 — нет: курсив приглушённым цветом */
static Font f_small[5];                         /* мелкая сетка: core-шрифт не выше клетки/K; 0 — плотностью */
static int asc_small[5];
static int cw = 9, chh = 18, asc = 14;
static bool hidden, truecolor;
static int rsh, gsh, bsh, rbits, gbits, bbits;  /* раскладка пикселя TrueColor */
static unsigned long rmask, gmask, bmask;
static XIM xim;
static XIC xic;
static Atom a_del, a_proto, a_clip, a_utf8, a_sel;
static Atom x_aware, x_enter, x_pos, x_status, x_drop, x_finished, x_sel, x_copy, x_urilist, x_prop;   /* XDND */
static Window x_src;                            /* окно-источник текущего перетаскивания */
static int kick = -1;                           /* eventfd: «в очереди Xlib уже есть события» */
static int last_mx = -1, last_my = -1;

#define COLS0 110
#define ROWS0 34
#define PT 11
static int zoom = 100;                          /* №15: масштаб шрифта, % */

/* ---------- цвета ---------- */
static int low_bit(unsigned long m) { int s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; }
static int nbits(unsigned long m) { int n = 0; while (m) { n += (int)(m & 1); m >>= 1; } return n; }
static unsigned long pix(u32 rgb) {
    unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (truecolor)
        return ((unsigned long)(r >> (8 - rbits)) << rsh) | ((unsigned long)(g >> (8 - gbits)) << gsh) |
               ((unsigned long)(b >> (8 - bbits)) << bsh);
    /* не TrueColor (редкость): чёрный или белый по яркости */
    int scr = DefaultScreen(dpy);
    return r * 3 + g * 6 + b > 1280 ? WhitePixel(dpy, scr) : BlackPixel(dpy, scr);
}
static u32 rgb_of(unsigned long p) {
    if (!truecolor) return p == WhitePixel(dpy, DefaultScreen(dpy)) ? 0xFFFFFF : 0;
    u32 r = (u32)((p & rmask) >> rsh) << (8 - rbits), g = (u32)((p & gmask) >> gsh) << (8 - gbits), b = (u32)((p & bmask) >> bsh) << (8 - bbits);
    return r << 16 | g << 8 | b;
}

/* ---------- шрифт ---------- */
/* Шаблон XLFD: -ФИРМА-fixed-НАСЫЩЕННОСТЬ-r-*--РАЗМЕР-*-*-*-c-*-КОДИРОВКА */
static void xlfd(char *out, size_t n, const char *foundry, const char *weight, int size, const char *reg) {
    snprintf(out, n, "-%s-fixed-%s-r-*--%d-*-*-*-c-*-%s", foundry, weight, size, reg);
}
/* Метрики шрифта по имени — без таблицы символов. 0 — нет такого. */
static Font font_load(const char *pat, int *w, int *a, int *h) {
    int n = 0;
    XFontStruct *info = NULL;
    char **names = XListFontsWithInfo(dpy, pat, 1, &n, &info);
    if (!names || n < 1) return 0;
    Font f = XLoadFont(dpy, names[0]);
    *w = info[0].max_bounds.width;
    *a = info[0].ascent;
    *h = info[0].ascent + info[0].descent;
    XFreeFontInfo(names, info, n);
    return f;
}
/* Ближайший к нужной высоте размер, который есть на X-сервере. */
static int font_size(const char *foundry, const char *reg, int want) {
    static const int sizes[] = { 10, 11, 12, 13, 14, 15, 16, 18, 20, 24, 26, 28 };
    int best = -1, bd = 1000;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int d = abs(sizes[i] - want);
        if (d >= bd) continue;
        char pat[160];
        xlfd(pat, sizeof pat, foundry, "medium", sizes[i], reg);
        int n = 0;
        char **l = XListFonts(dpy, pat, 1, &n);
        if (l) XFreeFontNames(l);
        if (n > 0) { bd = d; best = sizes[i]; }
    }
    return best;
}
static void fonts_free(void) {                  /* №15: перед новым размером — выгрузить прежние */
    if (f_bold && f_bold != f_norm) XUnloadFont(dpy, f_bold);
    if (f_ital) XUnloadFont(dpy, f_ital);
    f_ital = 0;
    if (f_norm) XUnloadFont(dpy, f_norm);
    f_norm = f_bold = 0;
    for (int k = 0; k < 5; k++) { if (f_small[k]) XUnloadFont(dpy, f_small[k]); f_small[k] = 0; asc_small[k] = 0; }
}
static void fonts_make(void) {
    int scr = DefaultScreen(dpy);
    int mm = DisplayHeightMM(dpy, scr);
    double dpi = mm > 0 ? DisplayHeight(dpy, scr) * 25.4 / mm : 96;
    if (dpi < 72 || dpi > 400) dpi = 96;
    int want = (int)(PT * dpi / 72 * zoom / 100 + 0.5);
    if (want < 10) want = 10;
    if (want > 28) want = 28;
    int w = 0, a = 0, h = 0;
    char pat[160];
    const char *fd = "misc", *rg = "iso10646-1";
    int sz = font_size(fd, rg, want);
    if (sz < 0) { fd = "*"; rg = "*-*"; sz = font_size(fd, rg, want); }
    if (sz > 0) {
        xlfd(pat, sizeof pat, fd, "medium", sz, rg);
        f_norm = font_load(pat, &w, &a, &h);
        /* жирный — только с теми же метриками, иначе двойная печать */
        int w2 = 0, a2 = 0, h2 = 0;
        xlfd(pat, sizeof pat, fd, "bold", sz, rg);
        Font b = f_norm ? font_load(pat, &w2, &a2, &h2) : 0;
        if (b && w2 == w && a2 == a && h2 == h) f_bold = b;
        else if (b) XUnloadFont(dpy, b);
        int w3 = 0, a3 = 0, h3 = 0;              /* наклонный (o) — тоже только с теми же метриками */
        snprintf(pat, sizeof pat, "-%s-fixed-medium-o-*--%d-*-*-*-c-*-%s", fd, sz, rg);
        Font it = f_norm ? font_load(pat, &w3, &a3, &h3) : 0;
        if (it && w3 == w && a3 == a && h3 == h) f_ital = it;
        else if (it) XUnloadFont(dpy, it);
    }
    if (!f_norm) f_norm = font_load("fixed", &w, &a, &h);
    if (w > 1) cw = w;
    if (h > 3) { chh = h; asc = a; }
    /* Мелкая сетка: самый крупный символьный core-шрифт не выше клетки/K (ASCII — хватает iso8859-1).
     * Нет такого (обычно для K=3..4: меньше 5x7 на X-сервере не бывает) — мелкая клетка рисуется
     * плотностью: фон и прямоугольник цвета символа, площадь — по «весу» символа. */
    for (int k = 2; k <= 4; k++) {
        for (int fsz = chh / k; fsz >= 5 && !f_small[k]; fsz--) {
            for (int pi = 0; pi < 2 && !f_small[k]; pi++) {
                char fpat[96];
                if (pi == 0) snprintf(fpat, sizeof fpat, "-*-*-medium-r-*--%d-*-*-*-c-*-iso8859-1", fsz);
                else snprintf(fpat, sizeof fpat, "-*-*-medium-r-*--%d-*-*-*-m-*-iso8859-1", fsz);
                int w3, a3, h3;
                Font f = font_load(fpat, &w3, &a3, &h3);
                if (f && h3 <= chh / k + 1 && w3 <= cw / k + 2) { f_small[k] = f; asc_small[k] = a3; }
                else if (f) XUnloadFont(dpy, f);
            }
        }
    }
}
int gb_fine_scale(void) { return chh / 4 >= 3 ? 4 : chh / 3 >= 3 ? 3 : chh / 2 >= 3 ? 2 : 0; }

/* ---------- рисование ---------- */
static void fill(Drawable d, int x, int y, int w, int h, u32 rgb) {
    XSetForeground(dpy, gc, pix(rgb));
    XFillRectangle(dpy, d, gc, x, y, (unsigned)w, (unsigned)h);
}
static void draw_run(Drawable d, size_t ci, int x, int y, const XChar2b *s, int n, int cells) {
    u8 a = G.at[ci];
    u32 fg, bg;
    grid_colors(a, G.fg[ci], G.bg[ci], &fg, &bg);
    int px = x * cw, py = y * chh;
    Font fn = (a & A_ITAL) && f_ital ? f_ital : (a & A_BOLD) && f_bold ? f_bold : f_norm;
    if ((a & A_ITAL) && !f_ital)                 /* нет наклонного шрифта — приглушить цвет (3/4 к фону) */
        fg = (((fg >> 16 & 255) * 3 + (bg >> 16 & 255)) / 4) << 16 | (((fg >> 8 & 255) * 3 + (bg >> 8 & 255)) / 4) << 8 | ((fg & 255) * 3 + (bg & 255)) / 4;
    if (cells != n) {                            /* широкий символ: фон на обе клетки, глиф слева */
        fill(d, px, py, cells * cw, chh, bg);
        XSetForeground(dpy, gc, pix(fg));
        XSetFont(dpy, gc, fn);
        XDrawString16(dpy, d, gc, px, py + asc, s, n);
    } else {
        XSetForeground(dpy, gc, pix(fg));
        XSetBackground(dpy, gc, pix(bg));
        XSetFont(dpy, gc, fn);
        XDrawImageString16(dpy, d, gc, px, py + asc, s, n);
    }
    if ((a & A_BOLD) && !f_bold) XDrawString16(dpy, d, gc, px + 1, py + asc, s, n);
    if (a & A_UL) fill(d, px, py + asc + 1 < py + chh ? py + asc + 1 : py + chh - 1, cells * cw, 1, fg);
    if (a & A_STRIKE) fill(d, px, py + chh / 2, cells * cw, 1, fg);   /* SGR 9 */
}
static void draw_row(Drawable d, int y) {
    static XChar2b buf[2000];
    size_t row = (size_t)y * G.cols;
    const u32 *ch = G.ch + row;
    int x = 0;
    while (x < G.cols) {
        u32 c = ch[x];
        bool wide = x + 1 < G.cols && ch[x + 1] == 0;
        if (c == 0 || wide) {
            u32 v = c == 0 ? ' ' : c > 0xFFFF ? '?' : c;
            XChar2b s = { (unsigned char)(v >> 8), (unsigned char)(v & 255) };
            draw_run(d, row + (size_t)x, x, y, &s, 1, wide ? 2 : 1);
            x += wide ? 2 : 1;
            continue;
        }
        int n = 0, x0 = x;
        size_t i0 = row + (size_t)x0;
        while (x < G.cols && ch[x] != 0 && !(x + 1 < G.cols && ch[x + 1] == 0) &&
               G.at[row + x] == G.at[i0] && G.fg[row + x] == G.fg[i0] && G.bg[row + x] == G.bg[i0]) {
            u32 v = ch[x] > 0xFFFF ? '?' : ch[x];
            buf[n].byte1 = (unsigned char)(v >> 8);
            buf[n].byte2 = (unsigned char)(v & 255);
            n++;
            x++;
        }
        draw_run(d, i0, x0, y, buf, n, n);
    }
}
/* «Вес» символа ASCII для рисования плотностью: доля закрашенной площади, 0..8 восьмых. */
static int ink(unsigned char c) {
    if (c == ' ') return 0;
    if (strchr(".,'`", c)) return 1;
    if (strchr("-_:;\"~^", c)) return 2;
    if (strchr("!|/\\()[]{}<>il1rtcvx=+*?", c)) return 3;
    if (strchr("#@%&MWBNQ08$", c)) return 6;
    return 4;
}
/* Слои мелкой сетки поверх строки y (см. gui.h). */
static void draw_layers(Drawable d, int y) {
    for (int li = 0; li < G.nlay; li++) {
        const Layer *l = &G.lay[li];
        if (y < l->y || y >= l->y + l->h) continue;
        int fw = l->k * l->w, fh = l->k * l->h, X0 = l->x * cw, Y0 = l->y * chh, PW = l->w * cw, PH = l->h * chh;
        XRectangle clip = { (short)X0, (short)(y * chh), (unsigned short)PW, (unsigned short)chh };
        XSetClipRectangles(dpy, gc, 0, 0, &clip, 1, Unsorted);
        Font sf = f_small[l->k];
        if (sf) XSetFont(dpy, gc, sf);
        for (int fy = (y - l->y) * l->k; fy < (y - l->y + 1) * l->k && fy < fh; fy++) {
            int top = Y0 + fy * PH / fh, bot = Y0 + (fy + 1) * PH / fh;
            const u8 *c = l->c + (size_t)fy * fw * 7;
            for (int fx = 0; fx < fw; fx++) {
                const u8 *q = c + (size_t)fx * 7;
                int x0 = X0 + fx * PW / fw, x1 = X0 + (fx + 1) * PW / fw;
                u32 fg = (u32)q[1] << 16 | (u32)q[2] << 8 | q[3], bg = (u32)q[4] << 16 | (u32)q[5] << 8 | q[6];
                fill(d, x0, top, x1 - x0, bot - top, bg);
                if (q[0] == ' ') continue;
                XSetForeground(dpy, gc, pix(fg));
                if (sf) {
                    char ch = (char)q[0];
                    XDrawString(dpy, d, gc, x0, top + asc_small[l->k], &ch, 1);
                } else {
                    int w = x1 - x0, h = bot - top, v = ink(q[0]);
                    int iw = (w * v + 7) / 8, ih = (h * v + 7) / 8;
                    if (iw < 1) iw = 1;
                    if (ih < 1) ih = 1;
                    XFillRectangle(dpy, d, gc, x0 + (w - iw) / 2, top + (h - ih) / 2, (unsigned)iw, (unsigned)ih);
                }
            }
        }
        XSetClipMask(dpy, gc, None);
    }
}
static void draw_line(Drawable d, int y) { draw_row(d, y); if (G.nlay) draw_layers(d, y); }

static void draw_margins(Drawable d, int W, int H, int y0, int y1) {
    if (W > G.cols * cw) fill(d, G.cols * cw, y0, W - G.cols * cw, y1 - y0, G.def_bg);
    if (H > G.rows * chh && y1 > G.rows * chh) fill(d, 0, G.rows * chh, W, H - G.rows * chh, G.def_bg);
}
static int win_w, win_h;
static void expose(int ey, int eh) {
    int y0 = ey / chh, y1 = (ey + eh + chh - 1) / chh;
    if (y1 > G.rows) y1 = G.rows;
    for (int y = y0 < 0 ? 0 : y0; y < y1; y++) draw_line(win, y);
    draw_margins(win, win_w, win_h, ey, ey + eh);
}

void gb_flush(void) {
    if (!dpy || !G.dirty) return;
    for (int y = 0; y < G.rows; y++) {
        if (!G.dirty[y]) continue;
        G.dirty[y] = 0;
        if (!hidden) draw_line(win, y);
    }
    XFlush(dpy);
}
void gb_bell(void) { if (dpy && !hidden) { XBell(dpy, 0); XFlush(dpy); } }

/* ---------- снимок ---------- */
bool gb_snapshot(const char *path) {
    if (!dpy || !G.ch) return false;
    int W = G.cols * cw, H = G.rows * chh;
    Pixmap pm = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H, (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)));
    for (int y = 0; y < G.rows; y++) draw_line(pm, y);
    XImage *im = XGetImage(dpy, pm, 0, 0, (unsigned)W, (unsigned)H, AllPlanes, ZPixmap);
    XFreePixmap(dpy, pm);
    if (!im) return false;
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t sz = (uint32_t)W * (uint32_t)H * 4, off = 14 + 40;
        unsigned char h[54] = { 'B', 'M' };
        uint32_t v[] = { off + sz, 0, off, 40, (uint32_t)W, (uint32_t)-H };
        memcpy(h + 2, &v[0], 4); memcpy(h + 10, &v[2], 4); memcpy(h + 14, &v[3], 4);
        memcpy(h + 18, &v[4], 4); memcpy(h + 22, &v[5], 4);
        h[26] = 1; h[28] = 32;
        memcpy(h + 34, &sz, 4);
        ok = fwrite(h, 1, sizeof h, f) == sizeof h;
        unsigned char *line = malloc((size_t)W * 4);
        for (int y = 0; ok && line && y < H; y++) {
            for (int x = 0; x < W; x++) {
                u32 c = rgb_of(XGetPixel(im, x, y));
                line[x * 4] = (unsigned char)(c & 255);
                line[x * 4 + 1] = (unsigned char)((c >> 8) & 255);
                line[x * 4 + 2] = (unsigned char)((c >> 16) & 255);
                line[x * 4 + 3] = 255;
            }
            ok = fwrite(line, 1, (size_t)W * 4, f) == (size_t)W * 4;
        }
        free(line);
        ok = fclose(f) == 0 && ok;
    }
    XDestroyImage(im);
    return ok;
}

/* ---------- ввод ---------- */
static void push(Key k) { kq_push(&k); }

static void mouse(int px, int py, int button, unsigned state) {
    if (!G.ui) return;
    int x = px / cw, y = py / chh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= G.cols) x = G.cols - 1;
    if (y >= G.rows) y = G.rows - 1;
    if ((button == 0 || button == 6) && x == last_mx && y == last_my) return;   /* наведение/протяжка — при смене клетки */
    last_mx = x;
    last_my = y;
    push((Key){ .code = K_MOUSE, .x = x, .y = y, .button = button,
                .ctrl = (state & ControlMask) != 0, .shift = (state & ShiftMask) != 0, .alt = (state & Mod1Mask) != 0 });
}

void gb_inject(const Key *k) {
    if (k->code != K_MOUSE || !dpy) { kq_push(k); return; }
    /* настоящим событием через X-сервер: проверяется весь путь до K_MOUSE */
    XEvent e = {0};
    int px = k->x * cw + cw / 2, py = k->y * chh + chh / 2;
    unsigned st = (k->ctrl ? ControlMask : 0) | (k->shift ? ShiftMask : 0) | (k->alt ? Mod1Mask : 0);
    if (k->button == 0 || k->button == 6) {     /* 6 — протяжка: движение с зажатой левой */
        if (k->button == 6) st |= Button1Mask;
        e.xmotion = (XMotionEvent){ .type = MotionNotify, .display = dpy, .window = win, .x = px, .y = py, .state = st, .same_screen = True };
        XSendEvent(dpy, win, False, PointerMotionMask, &e);
    } else if (k->button == 7) {                /* отпускание левой */
        e.xbutton = (XButtonEvent){ .type = ButtonRelease, .display = dpy, .window = win, .x = px, .y = py,
                                    .button = 1, .state = st | Button1Mask, .same_screen = True };
        XSendEvent(dpy, win, False, ButtonReleaseMask, &e);
    } else {
        e.xbutton = (XButtonEvent){ .type = ButtonPress, .display = dpy, .window = win, .x = px, .y = py,
                                    .button = k->button == 99 ? 1u : k->button == 8 ? 8u : k->button == 9 ? 9u : (unsigned)k->button, .state = st, .same_screen = True };
        XSendEvent(dpy, win, False, ButtonPressMask, &e);
    }
    XFlush(dpy);
}

static void paste_request(void) {
    XConvertSelection(dpy, a_clip, a_utf8, a_sel, win, CurrentTime);
    XFlush(dpy);
}
/* ---------- XDND: перетаскивание файла (только первый, только file://) ---------- */
static void dnd_send(Atom type, long l1, long l2, long l4) {
    XEvent r = {0};
    r.xclient.type = ClientMessage;
    r.xclient.display = dpy;
    r.xclient.window = x_src;
    r.xclient.message_type = type;
    r.xclient.format = 32;
    r.xclient.data.l[0] = (long)win;
    r.xclient.data.l[1] = l1;
    r.xclient.data.l[2] = l2;
    r.xclient.data.l[4] = l4;
    XSendEvent(dpy, x_src, False, NoEventMask, &r);
    XFlush(dpy);
}
static bool dnd_message(const XClientMessageEvent *c) {
    if (c->message_type == x_enter) { x_src = (Window)c->data.l[0]; return true; }
    if (c->message_type == x_pos) {             /* принять (копирование) */
        x_src = (Window)c->data.l[0];
        dnd_send(x_status, 1, 0, (long)x_copy);
        return true;
    }
    if (c->message_type == x_drop) {
        x_src = (Window)c->data.l[0];
        XConvertSelection(dpy, x_sel, x_urilist, x_prop, win, (Time)c->data.l[2]);
        XFlush(dpy);
        return true;
    }
    return false;
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void dnd_receive(const XSelectionEvent *e) {
    Atom type;
    int fmt;
    unsigned long n = 0, left = 0;
    unsigned char *data = NULL;
    bool ok = false;
    if (e->property != None &&
        XGetWindowProperty(dpy, win, x_prop, 0, 16384, True, AnyPropertyType, &type, &fmt, &n, &left, &data) == Success && data) {
        const char *s = (const char *)data;
        /* D-3: только text/uri-list из 8-битных байт; D-2: только свой компьютер (file:/// или file://localhost/) */
        bool local = fmt == 8 && type == x_urilist && n > 8 &&
                     (!strncmp(s, "file:///", 8) || !strncmp(s, "file://localhost/", 17));
        if (local) {
            s += strncmp(s, "file:///", 8) ? 16 : 7;
            char path[4096];
            size_t k = 0;
            for (; *s && *s != '\r' && *s != '\n' && k + 1 < sizeof path; s++) {
                if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { path[k++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 2; }
                else path[k++] = *s;
            }
            path[k] = 0;
            if (k && path[0] == '/' && !memchr(path, 0, k) && !text_unsafe(path) && g_ui_drop) { g_ui_drop(path); ok = true; }   /* D-4 */
            wipe(path, sizeof path);
        }
        wipe(data, n);
        XFree(data);
    }
    if (x_src) dnd_send(x_finished, ok ? 1 : 0, ok ? (long)x_copy : 0, 0);
    x_src = 0;
}
static void paste_receive(const XSelectionEvent *e) {
    if (e->property == None) return;
    Atom type;
    int fmt;
    unsigned long n = 0, left = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, e->property, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &left, &data) == Success && data) {
        if (fmt == 8) kq_paste_utf8((const char *)data, n);
        wipe(data, n);
        XFree(data);
    }
}

/* OSC 52 -> CLIPBOARD. В X11 буфер обмена — это владение выделением: текст живёт у нас, пока его
 * не заменит другая программа (SelectionClear — затираем) или пока mincord не завершится. */
static char *clip;
static size_t clip_n;
static Atom a_targets;
static void clip_drop(void) { if (clip) { wipe(clip, clip_n); free(clip); clip = NULL; clip_n = 0; } }
void gb_clipboard(const char *utf8, size_t n) {
    if (!dpy || hidden || !n) return;             /* --shot не трогает буфер владельца */
    clip_drop();
    clip = xmalloc(n);
    memcpy(clip, utf8, n);
    clip_n = n;
    XSetSelectionOwner(dpy, a_clip, win, CurrentTime);
    if (XGetSelectionOwner(dpy, a_clip) != win) clip_drop();
    XFlush(dpy);
}
static void clip_request(const XSelectionRequestEvent *r) {
    XSelectionEvent ev = { .type = SelectionNotify, .display = dpy, .requestor = r->requestor,
                           .selection = r->selection, .target = r->target, .property = None, .time = r->time };
    Atom prop = r->property != None ? r->property : r->target;
    if (clip && r->selection == a_clip) {
        if (r->target == a_targets) {
            Atom t[3] = { a_targets, a_utf8, XA_STRING };
            XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
            ev.property = prop;
        } else if (r->target == a_utf8 || r->target == XA_STRING) {
            XChangeProperty(dpy, r->requestor, prop, r->target, 8, PropModeReplace, (unsigned char *)clip, (int)clip_n);
            ev.property = prop;
        }
    }
    XSendEvent(dpy, r->requestor, False, 0, (XEvent *)&ev);
    XFlush(dpy);
}

/* Раскладка без метода ввода: Latin-1, Unicode-keysym (0x01000000+cp) и кириллица. */
static u32 keysym_cp(KeySym ks) {
    static const uint16_t cyr[32] = { 0x44E, 0x430, 0x431, 0x446, 0x434, 0x435, 0x444, 0x433, 0x445, 0x438, 0x439,
                                      0x43A, 0x43B, 0x43C, 0x43D, 0x43E, 0x43F, 0x44F, 0x440, 0x441, 0x442, 0x443,
                                      0x436, 0x432, 0x44C, 0x44B, 0x437, 0x448, 0x44D, 0x449, 0x447, 0x44A };
    if ((ks >= 0x20 && ks <= 0x7E) || (ks >= 0xA0 && ks <= 0xFF)) return (u32)ks;
    if ((ks & 0xFF000000) == 0x01000000) return (u32)(ks & 0x00FFFFFF);
    if (ks >= 0x6C0 && ks <= 0x6DF) return cyr[ks - 0x6C0];
    if (ks >= 0x6E0 && ks <= 0x6FF) return cyr[ks - 0x6E0] - 0x20u;
    if (ks == 0x6A3) return 0x451;
    if (ks == 0x6B3) return 0x401;
    return 0;
}

static void on_key(XKeyEvent *e) {
    char buf[64];
    KeySym ks = NoSymbol;
    Status st = 0;
    int n;
    if (xic) n = Xutf8LookupString(xic, e, buf, sizeof buf - 1, &ks, &st);
    else n = XLookupString(e, buf, sizeof buf - 1, &ks, NULL);
    if (n < 0) n = 0;
    buf[n] = 0;
    bool ctrl = e->state & ControlMask, alt = e->state & Mod1Mask, shift = e->state & ShiftMask;
    int code = K_NONE;
    switch (ks) {
    case XK_Return: case XK_KP_Enter:
        push(shift ? (Key){ .code = K_CHAR, .ch = '\n' } : (Key){ .code = K_ENTER, .ctrl = ctrl, .alt = alt });
        return;
    case XK_ISO_Left_Tab: push((Key){ .code = K_BTAB }); return;
    case XK_Tab: push((Key){ .code = shift ? K_BTAB : K_TAB }); return;
    case XK_BackSpace: code = K_BS; break;
    case XK_Delete: case XK_KP_Delete: code = K_DEL; break;
    case XK_Up: case XK_KP_Up: code = K_UP; break;
    case XK_Down: case XK_KP_Down: code = K_DOWN; break;
    case XK_Left: case XK_KP_Left: code = K_LEFT; break;
    case XK_Right: case XK_KP_Right: code = K_RIGHT; break;
    case XK_Home: case XK_KP_Home: code = K_HOME; break;
    case XK_End: case XK_KP_End: code = K_END; break;
    case XK_Prior: case XK_KP_Prior: code = K_PGUP; break;
    case XK_Next: case XK_KP_Next: code = K_PGDN; break;
    case XK_Escape: code = K_ESC; break;
    case XK_Insert: case XK_KP_Insert:
        if (shift) { paste_request(); return; }
        code = K_INS;
        break;
    default:
        if (ks >= XK_F1 && ks <= XK_F12) code = K_F1 + (int)(ks - XK_F1);
        break;
    }
    if (code != K_NONE) { push((Key){ .code = code, .ctrl = ctrl, .alt = alt, .shift = shift }); return; }
    if (ctrl) {
        /* Ctrl+буква — по латинской клавише (группа 0), в любой раскладке */
        KeySym base = XLookupKeysym(e, 0);
        if (base == XK_v && !alt) { paste_request(); return; }
        Key k;
        if (gb_ks_key((unsigned long)base, (unsigned long)ks, alt, &k)) push(k);
        return;
    }
    /* текст: UTF-8 из метода ввода или keysym */
    const char *s = buf;
    if (xic && n > 0 && (st == XLookupChars || st == XLookupBoth)) {
        while (*s) {
            u32 c;
            s += utf8_dec(s, &c);
            if (c >= 0x20 && c != 0x7F) push((Key){ .code = K_CHAR, .ch = c, .alt = alt });
        }
    } else {
        u32 c = keysym_cp(ks);
        if (!c && n == 1 && (unsigned char)buf[0] >= 0x20 && (unsigned char)buf[0] < 0x7F) c = (unsigned char)buf[0];
        if (c >= 0x20 && c != 0x7F) push((Key){ .code = K_CHAR, .ch = c, .alt = alt });
    }
    wipe(buf, sizeof buf);
}

/* №15: масштаб — ближайший core-шрифт 10..28 px; окно прежнего размера, меняется число клеток. */
bool gb_zoom(int step) {
    int z = step ? zoom + 10 * step : 100;
    int px = PT * 96 * z / 7200;
    if ((step > 0 && px > 28) || (step < 0 && px < 10) || z == zoom || !dpy) return false;
    int old_cw = cw, old_chh = chh;
    zoom = z;
    fonts_free();
    fonts_make();
    if (!hidden && win) {
        XSizeHints sh = {0};
        sh.flags = PMinSize | PResizeInc | PBaseSize;
        sh.min_width = 50 * cw; sh.min_height = 12 * chh; sh.width_inc = cw; sh.height_inc = chh;
        XSetWMNormalHints(dpy, win, &sh);
        int cols = win_w / cw, rows = win_h / chh;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (cols != G.cols || rows != G.rows) { grid_resize(cols, rows); if (G.ui) push((Key){ .code = K_RESIZE }); }
        else for (int y = 0; y < G.rows; y++) G.dirty[y] = 1;
        gb_flush();
        draw_margins(win, win_w, win_h, 0, win_h);
    }
    return cw != old_cw || chh != old_chh || true;
}
int gb_zoom_pct(void) { return zoom; }
/* №16: полный экран — просьба оконному менеджеру (_NET_WM_STATE_FULLSCREEN) */
int gb_fullscreen(int mode) {
    static bool on;
    if (!dpy || !win || hidden) return on;
    bool want = mode < 0 ? !on : mode != 0;
    if (want == on) return on;
    XEvent e = {0};
    e.xclient.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = want ? 1 : 0;
    e.xclient.data.l[1] = (long)XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    e.xclient.data.l[3] = 1;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(dpy);
    on = want;
    return on;
}
/* Ctrl+клавиша -> Key интерфейса (чистая функция, проверяется selftest): base — keysym группы 0 (латиница),
 * ks — keysym события. Ctrl+буква — в любой раскладке; №15 масштаб — Ctrl+= / Ctrl+- / Ctrl+0 и цифровой блок. */
bool gb_ks_key(unsigned long base, unsigned long ks, bool alt, Key *out) {
    if (base >= XK_a && base <= XK_z) { *out = (Key){ .code = K_CHAR, .ch = (u32)(base - XK_a + 'a'), .ctrl = true, .alt = alt }; return true; }
    if (alt) return false;
    u32 ch = 0;
    if (base == XK_equal || base == XK_plus || ks == XK_KP_Add) ch = '=';
    else if (base == XK_minus || ks == XK_KP_Subtract) ch = '-';
    else if (base == XK_0 || ks == XK_KP_0) ch = '0';
    if (!ch) return false;
    *out = (Key){ .code = K_CHAR, .ch = ch, .ctrl = true };
    return true;
}
static void on_event(XEvent *e) {
    if (XFilterEvent(e, None)) return;
    switch (e->type) {
    case Expose: if (G.ch && !hidden) expose(e->xexpose.y, e->xexpose.height); break;
    case ConfigureNotify: {
        win_w = e->xconfigure.width;
        win_h = e->xconfigure.height;
        int cols = win_w / cw, rows = win_h / chh;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (!hidden && (cols != G.cols || rows != G.rows)) {
            grid_resize(cols, rows);
            if (G.ui) push((Key){ .code = K_RESIZE });
            gb_flush();
            draw_margins(win, win_w, win_h, 0, win_h);
        }
        break;
    }
    case KeyPress: on_key(&e->xkey); break;
    case ButtonPress:
        if (e->xbutton.button >= 1 && e->xbutton.button <= 5) mouse(e->xbutton.x, e->xbutton.y, (int)e->xbutton.button, e->xbutton.state);
        else if (e->xbutton.button == 8 || e->xbutton.button == 9) { last_mx = -1; mouse(e->xbutton.x, e->xbutton.y, (int)e->xbutton.button, e->xbutton.state); }   /* боковые */
        break;
    case ButtonRelease:                          /* №38: отпускание левой — button 7 */
        if (e->xbutton.button == 1) { last_mx = -1; mouse(e->xbutton.x, e->xbutton.y, 7, e->xbutton.state); }
        break;
    case MotionNotify: {
        XEvent last = *e;                        /* сжать серию перемещений до последнего */
        while (XCheckTypedWindowEvent(dpy, win, MotionNotify, &last)) {}
        mouse(last.xmotion.x, last.xmotion.y, (last.xmotion.state & Button1Mask) ? 6 : 0, last.xmotion.state);
        break;
    }
    case SelectionNotify:
        if (e->xselection.property == x_prop && x_prop) { dnd_receive(&e->xselection); break; }
        paste_receive(&e->xselection);
        break;
    case SelectionRequest: clip_request(&e->xselectionrequest); break;
    case SelectionClear: if (e->xselectionclear.selection == a_clip) clip_drop(); break;
    case DestroyNotify:
        /* окно уничтожено извне (не через WM_DELETE_WINDOW) — тоже выход */
        if (e->xdestroywindow.window == win && !G.closed) {
            G.closed = true;
            push((Key){ .code = K_CHAR, .ch = 'q', .ctrl = true });
        }
        break;
    case ClientMessage:
        if (x_enter && dnd_message(&e->xclient)) break;
        if (e->xclient.message_type == a_proto && (Atom)e->xclient.data.l[0] == a_del) {
            /* закрытие окна = выход, как Ctrl+Q */
            G.closed = true;
            XUnmapWindow(dpy, win);
            XFlush(dpy);
            push((Key){ .code = K_CHAR, .ch = 'q', .ctrl = true });
        }
        break;
    default: break;
    }
}

/* Ошибки протокола (окно уже уничтожено и т. п.) не роняют программу; потеря X-сервера — выход
 * без попыток рисовать дальше (Xlib после этого обработчика завершает процесс сам). */
static int x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
static int x_io_error(Display *d) { (void)d; G.closed = true; return 0; }

bool gb_open(bool hide) {
    if (dpy) return true;
    const char *d = getenv("DISPLAY");
    if (!d || !*d) return false;
    XInitThreads();                              /* трансляция экрана (share_x11.c) работает с X из своего потока */
    dpy = XOpenDisplay(NULL);
    if (!dpy) return false;
    XSetErrorHandler(x_error);
    XSetIOErrorHandler(x_io_error);
    hidden = hide;
    int scr = DefaultScreen(dpy);
    Visual *vis = DefaultVisual(dpy, scr);
    truecolor = vis->class == TrueColor && DefaultDepth(dpy, scr) >= 15;
    if (truecolor) {
        rmask = vis->red_mask; gmask = vis->green_mask; bmask = vis->blue_mask;
        rsh = low_bit(rmask); gsh = low_bit(gmask); bsh = low_bit(bmask);
        rbits = nbits(rmask); gbits = nbits(gmask); bbits = nbits(bmask);
        if (rbits > 8) rbits = 8;
        if (gbits > 8) gbits = 8;
        if (bbits > 8) bbits = 8;
    }
    /* Цвета как у Discord (тёмная тема): в X11 нет общей настройки темы. */
    G.def_bg = 0x313338;
    G.def_fg = 0xDBDEE1;
    fonts_make();
    if (!f_norm) { XCloseDisplay(dpy); dpy = NULL; return false; }
    int W = COLS0 * cw, H = ROWS0 * chh;
    if (W > DisplayWidth(dpy, scr)) W = DisplayWidth(dpy, scr);
    if (H > DisplayHeight(dpy, scr) - 60) H = DisplayHeight(dpy, scr) - 60;
    XSetWindowAttributes wa = {0};
    wa.background_pixel = pix(G.def_bg);
    wa.bit_gravity = NorthWestGravity;
    wa.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | ExposureMask;
    win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)W, (unsigned)H, 0, CopyFromParent, InputOutput,
                        CopyFromParent, CWBackPixel | CWBitGravity | CWEventMask, &wa);
    win_w = W;
    win_h = H;
    gc = XCreateGC(dpy, win, 0, NULL);
    XStoreName(dpy, win, "Astra DS");
    {   /* значок окна: _NET_WM_ICON (32x32 и 48x48, данные в .rodata — src/astra_icon.h) */
        Atom icon = XInternAtom(dpy, "_NET_WM_ICON", False), card = XInternAtom(dpy, "CARDINAL", False);
        XChangeProperty(dpy, win, icon, card, 32, PropModeReplace, (const unsigned char *)ASTRA_ICON,
                        (int)(sizeof ASTRA_ICON / sizeof *ASTRA_ICON));
    }
    XClassHint ch = { (char *)"astra-ds", (char *)"astra-ds" };   /* WM_CLASS */
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh = {0};
    sh.flags = PMinSize | PResizeInc | PBaseSize;
    sh.min_width = 50 * cw;
    sh.min_height = 12 * chh;
    sh.width_inc = cw;
    sh.height_inc = chh;
    XSetWMNormalHints(dpy, win, &sh);
    a_proto = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_clip = XInternAtom(dpy, "CLIPBOARD", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_sel = XInternAtom(dpy, "MINCORD_PASTE", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    XSetWMProtocols(dpy, win, &a_del, 1);
    if (!hidden) {                              /* XDND v5: принимаем перетаскивание файла */
        x_aware = XInternAtom(dpy, "XdndAware", False);
        x_enter = XInternAtom(dpy, "XdndEnter", False);
        x_pos = XInternAtom(dpy, "XdndPosition", False);
        x_status = XInternAtom(dpy, "XdndStatus", False);
        x_drop = XInternAtom(dpy, "XdndDrop", False);
        x_finished = XInternAtom(dpy, "XdndFinished", False);
        x_sel = XInternAtom(dpy, "XdndSelection", False);
        x_copy = XInternAtom(dpy, "XdndActionCopy", False);
        x_urilist = XInternAtom(dpy, "text/uri-list", False);
        x_prop = XInternAtom(dpy, "ASTRA_DND", False);
        long ver = 5;
        XChangeProperty(dpy, win, x_aware, XA_ATOM, 32, PropModeReplace, (const unsigned char *)&ver, 1);
    }
    /* метод ввода (раскладки, IME) — при UTF-8 локали; без него — разбор keysym */
    if (!hidden) {
        const char *loc = setlocale(LC_CTYPE, "");
        if (!loc || !strcmp(loc, "C") || !strcmp(loc, "POSIX")) setlocale(LC_CTYPE, "C.UTF-8");
        if (XSupportsLocale()) {
            XSetLocaleModifiers("");
            xim = XOpenIM(dpy, NULL, NULL, NULL);
            if (xim) xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win, NULL);
        }
    }
    kick = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    grid_resize(W / cw, H / chh);
    if (!hidden) {
        XMapWindow(dpy, win);
        atexit(gui_atexit);
    }
    XFlush(dpy);
    return true;
}

void gb_pump(int timeout_ms) {
    if (!dpy) return;
    if (kick >= 0) { uint64_t v; ssize_t r = read(kick, &v, 8); (void)r; }
    if (timeout_ms != 0 && !XPending(dpy)) {
        struct pollfd p = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&p, 1, timeout_ms);
    }
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        on_event(&e);
    }
}

int gb_waitables(Waitable *out, int max) {
    if (!dpy || max < 1) return 0;
    /* Xlib мог уже прочитать события в свою очередь (при отрисовке, запросах) — тогда poll на сокете
     * не проснётся: будим его через eventfd. */
    if ((XEventsQueued(dpy, QueuedAfterFlush) > 0 || kq_pending()) && kick >= 0) {
        uint64_t one = 1;
        ssize_t r = write(kick, &one, 8);
        (void)r;
    }
    int n = 0;
    out[n++] = (Waitable){ ConnectionNumber(dpy), POLLIN };
    if (n < max && kick >= 0) out[n++] = (Waitable){ kick, POLLIN };
    return n;
}
#ifdef ASTRA_VIDEO
/* X11: исключить окно из захвата нельзя (у композитора нет такого запроса) */
bool term_capture_exclude(bool on) { (void)on; return false; }
#endif
#endif
