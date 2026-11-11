/* Окно-терминал: бэкенд macOS (Cocoa + CoreGraphics + CoreText). Собирать с -fobjc-arc (ARC).
 * Повторяет поведение gui_x11.c: сетка G, рисование только строк с dirty, слои мелкой сетки, мышь (K_MOUSE,
 * наведение, протяжка, колесо, боковые кнопки), буфер обмена (вставка через kq_paste_utf8, OSC 52 через
 * gb_clipboard), масштаб gb_zoom, полный экран, снимок в BMP (--shot), перетаскивание файла, видео (ASTRA_VIDEO).
 *
 * ---------- цикл событий: почему так ----------
 * Главный поток программы ждёт в plat_wait (poll по дескрипторам), а события Cocoa приходят в главный поток
 * через его CFRunLoop. Открытого API, который дал бы дескриптор «у окна есть события», нет: порт событий
 * WindowServer и порт пробуждения ран-лупа закрыты, CFRunLoopObserver срабатывает только пока ран-луп крутится
 * (а пока поток в poll, он стоит), CGEventTap требует разрешения «Мониторинг ввода». Узнать о событии окна
 * может только сам главный поток, сидя в ран-лупе. Поэтому ждать надо в Cocoa, а не в poll:
 *
 *   gb_wait(w, n, timeout) (вызывает plat_wait на macOS, см. отчёт координатору) кладёт дескрипторы из w в
 *   один kqueue (EV_ONESHOT), сам kqueue обёрнут в CFFileDescriptor, источник которого стоит в главном
 *   ран-лупе (общие режимы). Дальше поток ждёт в [NSApp nextEventMatchingMask:untilDate:...] до timeout.
 *   Пробуждения: событие окна (возвращается само), готовый дескриптор (обработчик CFFileDescriptor кладёт в
 *   очередь пустое событие NSEventTypeApplicationDefined, тот же приём, что glfwPostEmptyEvent), таймаут.
 *   После пробуждения все регистрации в kqueue снимаются: между ожиданиями он пуст и не будит зря.
 *   В простое поток спит в mach_msg ран-лупа: 0 % процессора. События не теряются: пока поток вне ожидания,
 *   они копятся в очереди AppKit и разбираются следующим gb_wait или gb_pump.
 *
 * Без вызова gb_wait из plat_wait (договор не поменян) окно всё равно работает, но хуже: gb_waitables отдаёт
 * канал kick, и в нём включается запасной таймер (20 мс), который будит poll, чтобы gb_pump разобрал
 * очередь Cocoa. Это не 0 % процессора, поэтому запасной путь включается, только если gb_wait ни разу не
 * позвали (проверка со второго вызова gb_waitables).
 *
 * Видео: gb_video_inval зовут из других потоков. Прямоугольник копится под os_unfair_lock, перерисовка
 * передаётся в главный поток через dispatch_async_f; дополнительно пишется байт в kick, чтобы poll проснулся,
 * если главный поток сейчас не в ран-лупе. Рисует только главный поток в drawRect: (CGImage из BGRA).
 *
 * Рисование: NSView (перевёрнутая система координат, y вниз), drawRect: рисует строки, задетые грязным
 * прямоугольником; gb_flush помечает setNeedsDisplayInRect: только строки с dirty и сразу рисует
 * (displayIfNeeded + CATransaction flush), потому что ран-луп между кадрами может не крутиться.
 * Размеры клетки в точках (points); Retina даёт backingScaleFactor, глифы и растр рисуются в полном разрешении.
 * Снимок (--shot): окно не создаётся вовсе (нужен только CoreText), сетка рисуется в CGBitmapContext -> BMP. */
#ifdef __APPLE__
#if !__has_feature(objc_arc)
#error "gui_mac.m собирается с -fobjc-arc"
#endif
#ifndef __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0   /* AssertMacros.h: без макросов check/verify/require */
#endif
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreText/CoreText.h>
#include <dispatch/dispatch.h>
#include <os/lock.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/event.h>
#include "gui.h"
#include "astra_icon.h"

/* Левая/правая Option в modifierFlags (зависимые от устройства биты, IOKit/hidsystem/IOLLEvent.h) */
#ifndef NX_DEVICELALTKEYMASK
#define NX_DEVICELALTKEYMASK 0x00000020
#endif
#ifndef NX_DEVICERALTKEYMASK
#define NX_DEVICERALTKEYMASK 0x00000040
#endif

#define COLS0 110
#define ROWS0 34
#define FONT_PT 13.0                            /* Menlo 13 pt при масштабе 100 % */
#define WAKE_SUBTYPE 0x4153                     /* пустое событие-пробуждение: subtype и data1 */
#define WAKE_DATA 0x41535452

/* объявления заранее: точечная запись свойств на классе из @class не компилируется */
@interface AstraView : NSView <NSTextInputClient>
@end
@interface AstraDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

static bool g_open, g_hidden, g_hooked;
static NSWindow *g_win;
static AstraView *g_view;
static AstraDelegate *g_delegate;
static id<NSObject> g_activity;                 /* без App Nap: голос и шлюз не должны засыпать */
static CGColorSpaceRef srgb;
static CTFontRef f_st[4];                       /* 0 обычный, 1 жирный, 2 курсив, 3 жирный курсив */
static CGGlyph gl_st[4][128];                   /* глифы ASCII каждого начертания */
static CTFontRef f_small[5];                    /* мелкая сетка: шрифт под клетку / K */
static CGFloat asc_small[5];
static CGGlyph gl_small[5][128];
static int cw = 8, chh = 16, asc = 13;
static int zoom = 100;                          /* №15: масштаб шрифта, % */
static CGFloat draw_scale = 1;                  /* пикселей на точку при текущем рисовании */
static int last_mx = -1, last_my = -1;
static double wheel_acc;
static bool g_marked;                           /* метод ввода (IME) сейчас набирает */
static int kick_r = -1, kick_w = -1;            /* канал: будит poll (kq_pending, видео, запасной таймер) */
static int g_kq = -1;                           /* kqueue ожидания gb_wait */
static CFFileDescriptorRef kq_cf;
static int wait_depth, waitables_calls;
static dispatch_source_t g_tick;                /* запасной таймер (нет gb_wait в plat_wait) */
static atomic_bool g_visible;                   /* окно видно: не свёрнуто, не закрыто другими */

static void push(Key k) { kq_push(&k); }
static void kick(void) { if (kick_w >= 0) { char one = 1; ssize_t r = write(kick_w, &one, 1); (void)r; } }
static void kick_drain(void) { if (kick_r >= 0) { char b[64]; while (read(kick_r, b, sizeof b) > 0) {} } }

/* ---------- шрифт ---------- */
static void glyphs_ascii(CTFontRef f, CGGlyph *out) {
    UniChar u[128];
    for (int i = 0; i < 128; i++) u[i] = (UniChar)(i >= 0x20 && i < 0x7F ? i : ' ');
    memset(out, 0, 128 * sizeof *out);
    CTFontGetGlyphsForCharacters(f, u, out, 128);  /* false, если чего-то нет: найденные всё равно заполнены */
}
static CTFontRef font_trait(CTFontRef base, CTFontSymbolicTraits t) {
    CTFontRef f = CTFontCreateCopyWithSymbolicTraits(base, 0, NULL, t, t);
    if (!f) { CFRetain(base); f = base; }       /* нет такого начертания: обычное */
    return f;
}
/* Запасные шрифты для символов, которых нет в Menlo (CJK, эмодзи): маленький кэш по коду символа. */
static struct { u32 cp; CTFontRef f; } fbc[64];
static void fallback_clear(void) {
    for (int i = 0; i < 64; i++) { if (fbc[i].f) CFRelease(fbc[i].f); fbc[i].f = NULL; fbc[i].cp = 0; }
}
static CTFontRef fallback_font(u32 cp, const UniChar *u, int nu) {
    int s = (int)(cp & 63);
    if (fbc[s].f && fbc[s].cp == cp) return fbc[s].f;
    CFStringRef str = CFStringCreateWithCharacters(kCFAllocatorDefault, u, nu);
    if (!str) return NULL;
    CTFontRef f = CTFontCreateForString(f_st[0], str, CFRangeMake(0, nu));
    CFRelease(str);
    if (fbc[s].f) CFRelease(fbc[s].f);
    fbc[s].cp = cp;
    fbc[s].f = f;
    return f;
}
static void fonts_free(void) {                  /* №15: перед новым размером выгрузить прежние */
    for (int i = 0; i < 4; i++) { if (f_st[i]) CFRelease(f_st[i]); f_st[i] = NULL; }
    for (int k = 0; k < 5; k++) { if (f_small[k]) CFRelease(f_small[k]); f_small[k] = NULL; asc_small[k] = 0; }
    fallback_clear();
}
static void fonts_make(void) {
    CGFloat size = FONT_PT * zoom / 100.0;
    f_st[0] = CTFontCreateWithName(CFSTR("Menlo"), size, NULL);   /* нет Menlo: CoreText подставит системный */
    f_st[1] = font_trait(f_st[0], kCTFontBoldTrait);
    f_st[2] = font_trait(f_st[0], kCTFontItalicTrait);
    f_st[3] = font_trait(f_st[0], kCTFontBoldTrait | kCTFontItalicTrait);
    for (int i = 0; i < 4; i++) glyphs_ascii(f_st[i], gl_st[i]);
    UniChar m = 'M';
    CGGlyph gm = 0;
    CGSize adv = { 0, 0 };
    CTFontGetGlyphsForCharacters(f_st[0], &m, &gm, 1);
    CTFontGetAdvancesForGlyphs(f_st[0], kCTFontOrientationHorizontal, &gm, &adv, 1);
    CGFloat a = CTFontGetAscent(f_st[0]), d = CTFontGetDescent(f_st[0]), l = CTFontGetLeading(f_st[0]);
    if (adv.width < 1) adv.width = size * 0.6;
    cw = (int)ceil(adv.width - 0.01);
    asc = (int)ceil(a);
    chh = (int)ceil(a + d + l);
    if (cw < 4) cw = 4;
    if (chh < asc + 2) chh = asc + 2;
    /* Мелкая сетка: Menlo такого размера, чтобы глиф влез в клетку/K по высоте и ширине */
    for (int k = 2; k <= 4; k++) {
        double hk = (double)chh / k, wk = (double)cw / k;
        double s1 = hk / ((a + d) / size), s2 = wk / (adv.width / size);
        double s = s1 < s2 ? s1 : s2;
        if (s < 1) s = 1;
        f_small[k] = CTFontCreateWithName(CFSTR("Menlo"), (CGFloat)s, NULL);
        asc_small[k] = CTFontGetAscent(f_small[k]);
        glyphs_ascii(f_small[k], gl_small[k]);
    }
}
int gb_fine_scale(void) { return chh / 4 >= 3 ? 4 : chh / 3 >= 3 ? 3 : chh / 2 >= 3 ? 2 : 0; }

/* ---------- рисование (общая часть окна и снимка; контекст перевёрнут: y вниз) ---------- */
static void set_fill(CGContextRef c, u32 rgb) {
    CGFloat k[4] = { (rgb >> 16 & 255) / 255.0, (rgb >> 8 & 255) / 255.0, (rgb & 255) / 255.0, 1 };
    CGContextSetFillColor(c, k);                /* цветовое пространство заливки: sRGB (ставится заранее) */
}
static void fill(CGContextRef c, CGFloat x, CGFloat y, CGFloat w, CGFloat h, u32 rgb) {
    set_fill(c, rgb);
    CGContextFillRect(c, CGRectMake(x, y, w, h));
}
static int enc16(u32 v, UniChar *u) {
    if (v > 0xFFFF && v <= 0x10FFFF) {
        v -= 0x10000;
        u[0] = (UniChar)(0xD800 + (v >> 10));
        u[1] = (UniChar)(0xDC00 + (v & 0x3FF));
        return 2;
    }
    u[0] = (UniChar)(v > 0xFFFF ? '?' : v);
    return 1;
}
#define RUN_MAX 256
/* Прогон одинаковых атрибутов: фон на cells клеток, глифы по клеткам (моноширинно), затем подчёркивание. */
static void draw_run(CGContextRef c, size_t ci, int x, int y, const u32 *cp, int n, int cells) {
    u8 a = G.at[ci];
    u32 fg, bg;
    grid_colors(a, G.fg[ci], G.bg[ci], &fg, &bg);
    CGFloat px = (CGFloat)x * cw, py = (CGFloat)y * chh;
    fill(c, px, py, (CGFloat)cells * cw, chh, bg);
    int st = ((a & A_BOLD) ? 1 : 0) | ((a & A_ITAL) ? 2 : 0);
    CTFontRef f = f_st[st];
    CGGlyph g[RUN_MAX];
    CGPoint p[RUN_MAX];
    int ng = 0;
    set_fill(c, fg);
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, px, py + asc);     /* начало строки на базовой линии; локально y вверх */
    CGContextScaleCTM(c, 1, -1);
    CGContextSetTextMatrix(c, CGAffineTransformIdentity);
    for (int i = 0; i < n; i++) {
        u32 v = cp[i];
        if (v == ' ' || v == 0) continue;
        CGFloat gx = (CGFloat)i * cw;
        CGGlyph gg = v < 128 ? gl_st[st][v] : 0;
        if (!gg) {
            UniChar u[2];
            CGGlyph t[2] = { 0, 0 };
            int nu = enc16(v, u);
            CTFontGetGlyphsForCharacters(f, u, t, nu);
            gg = t[0];
            if (!gg) {                           /* нет в Menlo: запасной шрифт, обрезка по своим клеткам */
                if (ng) { CTFontDrawGlyphs(f, g, p, (size_t)ng, c); ng = 0; }
                CTFontRef fb = fallback_font(v, u, nu);
                CGGlyph t2[2] = { 0, 0 };
                if (fb && CTFontGetGlyphsForCharacters(fb, u, t2, nu) && t2[0]) {
                    CGContextSaveGState(c);
                    CGContextClipToRect(c, CGRectMake(gx, -(CGFloat)(chh - asc), (CGFloat)(cells - i) * cw, chh));
                    CGPoint pp = CGPointMake(gx, 0);
                    CTFontDrawGlyphs(fb, t2, &pp, 1, c);
                    CGContextRestoreGState(c);
                }
                continue;
            }
        }
        if (ng == RUN_MAX) { CTFontDrawGlyphs(f, g, p, (size_t)ng, c); ng = 0; }
        g[ng] = gg;
        p[ng] = CGPointMake(gx, 0);
        ng++;
    }
    if (ng) CTFontDrawGlyphs(f, g, p, (size_t)ng, c);
    CGContextRestoreGState(c);
    if (a & A_UL) fill(c, px, py + asc + 1 < py + chh ? py + asc + 1 : py + chh - 1, (CGFloat)cells * cw, 1, fg);
    if (a & A_STRIKE) fill(c, px, py + chh / 2, (CGFloat)cells * cw, 1, fg);   /* SGR 9 */
}
static void draw_row(CGContextRef c, int y) {
    size_t row = (size_t)y * (size_t)G.cols;
    const u32 *ch = G.ch + row;
    int x = 0;
    while (x < G.cols) {
        u32 v = ch[x];
        bool wide = x + 1 < G.cols && ch[x + 1] == 0;
        if (v == 0 || wide) {                   /* широкий символ: фон на две клетки, глиф слева */
            u32 s = v ? v : ' ';
            draw_run(c, row + (size_t)x, x, y, &s, 1, wide ? 2 : 1);
            x += wide ? 2 : 1;
            continue;
        }
        int x0 = x;
        size_t i0 = row + (size_t)x0;
        while (x < G.cols && ch[x] != 0 && !(x + 1 < G.cols && ch[x + 1] == 0) && G.at[row + (size_t)x] == G.at[i0] &&
               G.fg[row + (size_t)x] == G.fg[i0] && G.bg[row + (size_t)x] == G.bg[i0])
            x++;
        draw_run(c, i0, x0, y, ch + x0, x - x0, x - x0);
    }
}
/* «Вес» символа ASCII для рисования плотностью (как gui_x11.c): доля закрашенной площади, 0..8 восьмых. */
static int ink(unsigned char c) {
    if (c == ' ') return 0;
    if (strchr(".,'`", c)) return 1;
    if (strchr("-_:;\"~^", c)) return 2;
    if (strchr("!|/\\()[]{}<>il1rtcvx=+*?", c)) return 3;
    if (strchr("#@%&MWBNQ08$", c)) return 6;
    return 4;
}
static u32 rgb3(const u8 *q) { return (u32)q[0] << 16 | (u32)q[1] << 8 | q[2]; }
/* Слои мелкой сетки поверх строки y (см. gui.h). Мелкая клетка от 6 пикселей экрана: глиф Menlo, иначе плотность. */
static void draw_layers(CGContextRef c, int y) {
    for (int li = 0; li < G.nlay; li++) {
        const Layer *l = &G.lay[li];
        if (y < l->y || y >= l->y + l->h || l->k < 2 || l->k > 4) continue;
        int k = l->k, fw = k * l->w, fh = k * l->h, X0 = l->x * cw, Y0 = l->y * chh, PW = l->w * cw, PH = l->h * chh;
        bool text = f_small[k] && (double)chh / k * draw_scale >= 6.0;
        CGContextSaveGState(c);
        CGContextClipToRect(c, CGRectMake(X0, (CGFloat)y * chh, PW, chh));
        for (int fy = (y - l->y) * k; fy < (y - l->y + 1) * k && fy < fh; fy++) {
            int top = Y0 + fy * PH / fh, bot = Y0 + (fy + 1) * PH / fh;
            const u8 *row = l->c + (size_t)fy * (size_t)fw * 7;
            for (int fx = 0; fx < fw;) {        /* фон: подряд одинаковые: одним прямоугольником */
                u32 bg = rgb3(row + (size_t)fx * 7 + 4);
                int fx1 = fx + 1;
                while (fx1 < fw && rgb3(row + (size_t)fx1 * 7 + 4) == bg) fx1++;
                int x0 = X0 + fx * PW / fw, x1 = X0 + fx1 * PW / fw;
                fill(c, x0, top, x1 - x0, bot - top, bg);
                fx = fx1;
            }
            if (text) {
                CGGlyph g[64];
                CGPoint p[64];
                int ng = 0;
                u32 cur = 0;
                CGContextSaveGState(c);
                CGContextTranslateCTM(c, 0, top + asc_small[k]);
                CGContextScaleCTM(c, 1, -1);
                CGContextSetTextMatrix(c, CGAffineTransformIdentity);
                for (int fx = 0; fx < fw; fx++) {
                    const u8 *q = row + (size_t)fx * 7;
                    if (q[0] <= ' ' || q[0] >= 0x7F) continue;
                    u32 fg = rgb3(q + 1);
                    if (ng && (fg != cur || ng == 64)) { set_fill(c, cur); CTFontDrawGlyphs(f_small[k], g, p, (size_t)ng, c); ng = 0; }
                    cur = fg;
                    g[ng] = gl_small[k][q[0]];
                    p[ng] = CGPointMake(X0 + fx * PW / fw, 0);
                    ng++;
                }
                if (ng) { set_fill(c, cur); CTFontDrawGlyphs(f_small[k], g, p, (size_t)ng, c); }
                CGContextRestoreGState(c);
            } else {
                for (int fx = 0; fx < fw; fx++) {
                    const u8 *q = row + (size_t)fx * 7;
                    if (q[0] == ' ') continue;
                    int x0 = X0 + fx * PW / fw, x1 = X0 + (fx + 1) * PW / fw;
                    int w = x1 - x0, h = bot - top, v = ink(q[0]);
                    int iw = (w * v + 7) / 8, ih = (h * v + 7) / 8;
                    if (iw < 1) iw = 1;
                    if (ih < 1) ih = 1;
                    fill(c, x0 + (w - iw) / 2, top + (h - ih) / 2, iw, ih, rgb3(q + 1));
                }
            }
        }
        CGContextRestoreGState(c);
    }
}
static void draw_line(CGContextRef c, int y) { draw_row(c, y); if (G.nlay) draw_layers(c, y); }

#ifdef ASTRA_VIDEO
/* ---------- видео просмотра трансляции и плитки камер: CGImage кадра поверх клеток ----------
 * Растр BGRA (0x00RRGGBB, младший байт первым) = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst.
 * Кадр копируется в CFData под мьютексом: CGImage может жить в CoreGraphics дольше, чем мы держим буфер. */
static os_unfair_lock vlock = OS_UNFAIR_LOCK_INIT;
static CGRect vrect;                            /* накопленная область перерисовки (точки) */
static bool vrect_set;
static atomic_bool v_posted;                    /* блок перерисовки уже стоит в главной очереди */
static bool cap_excl;                           /* term_capture_exclude: окно не попадает в захват экрана */

static void vid_put(CGContextRef c, const GvPaint *g) {   /* картинка по центру области, поля чёрные */
    if (g->bw <= 0 || g->bh <= 0) return;
    int dx = g->x + (g->w - g->bw) / 2, dy = g->y + (g->h - g->bh) / 2;
    CGContextSaveGState(c);
    CGContextClipToRect(c, CGRectMake(g->x, g->y, g->w, g->h));
    if (dy > g->y) fill(c, g->x, g->y, g->w, dy - g->y, 0);
    if (dy + g->bh < g->y + g->h) fill(c, g->x, dy + g->bh, g->w, g->y + g->h - dy - g->bh, 0);
    if (dx > g->x) fill(c, g->x, dy, dx - g->x, g->bh, 0);
    if (dx + g->bw < g->x + g->w) fill(c, dx + g->bw, dy, g->x + g->w - dx - g->bw, g->bh, 0);
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)g->buf, (CFIndex)g->bw * g->bh * 4);
    CGDataProviderRef pr = d ? CGDataProviderCreateWithCFData(d) : NULL;
    CGImageRef im = pr ? CGImageCreate((size_t)g->bw, (size_t)g->bh, 8, 32, (size_t)g->bw * 4, srgb,
                                       (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, pr, NULL, false,
                                       kCGRenderingIntentDefault) : NULL;
    if (im) {
        CGContextTranslateCTM(c, dx, dy + g->bh);   /* CGContextDrawImage рисует снизу вверх: перевернуть локально */
        CGContextScaleCTM(c, 1, -1);
        CGContextSetInterpolationQuality(c, kCGInterpolationLow);
        CGContextDrawImage(c, CGRectMake(0, 0, g->bw, g->bh), im);
        CGImageRelease(im);
    }
    if (pr) CGDataProviderRelease(pr);
    if (d) CFRelease(d);
    CGContextRestoreGState(c);
}
static void vid_draw(CGContextRef c, NSRect dirty) {
    GvPaint g;
    if (gvid_paint_lock(&g)) {
        if (NSIntersectsRect(dirty, NSMakeRect(g.x, g.y, g.w, g.h))) vid_put(c, &g);
        gvid_paint_unlock();
    }
    GvPaint t[8];                               /* CAM_TILES */
    int k = gcam_paint_lock(t, 8);
    for (int i = 0; i < k; i++)
        if (NSIntersectsRect(dirty, NSMakeRect(t[i].x, t[i].y, t[i].w, t[i].h))) vid_put(c, &t[i]);
    gcam_paint_unlock();
}
#endif

/* ---------- окно: вид ---------- */
static void mouse_px(CGFloat fx, CGFloat fy, int button, NSEventModifierFlags f) {
    if (!G.ui || G.cols < 1 || G.rows < 1) return;
    int x = (int)floor(fx / cw), y = (int)floor(fy / chh);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= G.cols) x = G.cols - 1;
    if (y >= G.rows) y = G.rows - 1;
    if ((button == 0 || button == 6) && x == last_mx && y == last_my) return;   /* наведение/протяжка: при смене клетки */
    last_mx = x;
    last_my = y;
    push((Key){ .code = K_MOUSE, .x = x, .y = y, .button = button, .ctrl = (f & NSEventModifierFlagControl) != 0,
                .shift = (f & NSEventModifierFlagShift) != 0, .alt = (f & NSEventModifierFlagOption) != 0 });
}
static void mouse_ev(NSView *v, NSEvent *e, int button) {
    NSPoint p = [v convertPoint:e.locationInWindow fromView:nil];
    mouse_px(p.x, p.y, button, e.modifierFlags);
}
static void do_paste(void) {
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!s) return;
    NSUInteger n = [s lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    if (!n || n > (16u << 20)) return;
    char *b = malloc(n + 1);
    if (!b) return;
    if ([s getCString:b maxLength:n + 1 encoding:NSUTF8StringEncoding]) kq_paste_utf8(b, n);
    wipe(b, n + 1);
    free(b);
}
/* Текст из метода ввода или клавиши: по символам Unicode (пары суррогатов склеиваются). */
static void push_text(NSString *s, bool alt) {
    NSUInteger n = s.length;
    for (NSUInteger i = 0; i < n; i++) {
        u32 c = [s characterAtIndex:i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            u32 d = [s characterAtIndex:i + 1];
            if (d >= 0xDC00 && d <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
        }
        if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c <= 0xDFFF) || (c >= 0xF700 && c <= 0xF8FF)) continue;
        push((Key){ .code = K_CHAR, .ch = c, .alt = alt });
    }
}
/* Латинская буква физической клавиши (раскладка ANSI, коды kVK_ANSI_* из HIToolbox/Events.h): для Ctrl+буква
 * в нелатинской раскладке (кириллица), как X11 берёт keysym группы 0. */
static u32 keycode_latin(unsigned kc) {
    static const char t[47] = { 'a', 's', 'd', 'f', 'h', 'g', 'z', 'x', 'c', 'v', 0, 'b', 'q', 'w', 'e', 'r',
                                'y', 't', '1', '2', '3', '4', '6', '5', '=', '9', '7', '-', '8', '0', ']', 'o',
                                'u', '[', 'i', 'p', 0, 'l', 'j', '\'', 'k', ';', '\\', ',', '/', 'n', 'm' };
    if (kc < sizeof t) return (u32)(unsigned char)t[kc];
    if (kc == 0x45) return '+';                 /* цифровой блок: + - 0 */
    if (kc == 0x4E) return '-';
    if (kc == 0x52) return '0';
    return 0;
}
/* Ctrl+клавиша -> Key интерфейса (чистая функция, как gb_ks_key у X11): ch: символ клавиши без модификаторов
 * (charactersIgnoringModifiers), keycode: код физической клавиши. Ctrl+буква в любой раскладке;
 * №15 масштаб: Ctrl+= / Ctrl+- / Ctrl+0 и цифровой блок. */
bool gb_mac_key(u32 ch, unsigned keycode, bool alt, Key *out) {
    u32 b = ch;
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (b < 0x21 || b > 0x7E) b = keycode_latin(keycode);   /* не ASCII (кириллица и т. п.): по физической клавише */
    if (b >= 'a' && b <= 'z') { *out = (Key){ .code = K_CHAR, .ch = b, .ctrl = true, .alt = alt }; return true; }
    if (alt) return false;
    u32 r = 0;
    if (b == '=' || b == '+') r = '=';
    else if (b == '-') r = '-';
    else if (b == '0') r = '0';
    if (!r) return false;
    *out = (Key){ .code = K_CHAR, .ch = r, .ctrl = true };
    return true;
}
static void zoom_cmd(int step) {               /* меню «Вид»: как Ctrl+= / Ctrl+- / Ctrl+0 */
    if (G.ui) push((Key){ .code = K_CHAR, .ch = step > 0 ? '=' : step < 0 ? '-' : '0', .ctrl = true });
    else gb_zoom(step);
}
static void vis_update(void) {
    bool v = g_win && g_win.visible && !g_win.miniaturized && (g_win.occlusionState & NSWindowOcclusionStateVisible);
    atomic_store(&g_visible, v);
}
static void fit_grid(void) {
    if (!g_view || g_hidden) return;
    NSSize s = g_view.bounds.size;
    int cols = (int)(s.width / cw), rows = (int)(s.height / chh);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols != G.cols || rows != G.rows) {
        grid_resize(cols, rows);
        if (G.ui) push((Key){ .code = K_RESIZE });
    }
    [g_view setNeedsDisplay:YES];
}

@implementation AstraView
- (instancetype)initWithFrame:(NSRect)r {
    self = [super initWithFrame:r];
    if (self) {
        NSTrackingArea *ta = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                          options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                            owner:self userInfo:nil];
        [self addTrackingArea:ta];
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    }
    return self;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self setNeedsDisplay:YES]; }

- (void)drawRect:(NSRect)dirty {
    CGContextRef c = [NSGraphicsContext currentContext].CGContext;
    if (!c) return;
    CGContextSetFillColorSpace(c, srgb);
    draw_scale = self.window ? self.window.backingScaleFactor : 1;
    NSRect b = self.bounds;
    if (!G.ch) { fill(c, 0, 0, b.size.width, b.size.height, G.def_bg); return; }
    int y0 = (int)floor(NSMinY(dirty) / chh), y1 = (int)ceil(NSMaxY(dirty) / chh);
    if (y0 < 0) y0 = 0;
    if (y1 > G.rows) y1 = G.rows;
    for (int y = y0; y < y1; y++) draw_line(c, y);
    CGFloat gw = (CGFloat)G.cols * cw, gh = (CGFloat)G.rows * chh;
    if (b.size.width > gw) fill(c, gw, 0, b.size.width - gw, b.size.height, G.def_bg);   /* поля за сеткой */
    if (b.size.height > gh) fill(c, 0, gh, b.size.width, b.size.height - gh, G.def_bg);
#ifdef ASTRA_VIDEO
    vid_draw(c, dirty);
#endif
}

/* ---------- клавиатура ----------
 * Левая Option: Alt (сочетания интерфейса: Alt+буква), правая Option: набор символов раскладки (@, {, ё и
 * мёртвые клавиши), как AltGr. Cmd: только меню (Cmd+Q, Cmd+C, Cmd+V, масштаб). Обычный текст и IME -
 * через interpretKeyEvents (NSTextInputClient). */
- (void)keyDown:(NSEvent *)e {
    if (g_marked) { [self interpretKeyEvents:@[ e ]]; return; }   /* идёт набор в IME: всё ему */
    NSEventModifierFlags f = e.modifierFlags;
    bool ctrl = (f & NSEventModifierFlagControl) != 0, shift = (f & NSEventModifierFlagShift) != 0;
    bool cmd = (f & NSEventModifierFlagCommand) != 0, opt = (f & NSEventModifierFlagOption) != 0;
    bool lalt = opt && !((f & NX_DEVICERALTKEYMASK) && !(f & NX_DEVICELALTKEYMASK));   /* сторона неизвестна: левая */
    NSString *ign = e.charactersIgnoringModifiers;
    u32 k0 = ign.length ? [ign characterAtIndex:0] : 0;
    int code = K_NONE;
    switch (k0) {
    case '\r': case 0x03:                       /* Return и Enter цифрового блока */
        push(shift ? (Key){ .code = K_CHAR, .ch = '\n' } : (Key){ .code = K_ENTER, .ctrl = ctrl, .alt = opt });
        return;
    case 0x19: push((Key){ .code = K_BTAB }); return;
    case '\t': push((Key){ .code = shift ? K_BTAB : K_TAB }); return;
    case 0x7F: case 0x08: code = K_BS; break;
    case NSDeleteFunctionKey: code = K_DEL; break;
    case NSUpArrowFunctionKey: code = K_UP; break;
    case NSDownArrowFunctionKey: code = K_DOWN; break;
    case NSLeftArrowFunctionKey: code = K_LEFT; break;
    case NSRightArrowFunctionKey: code = K_RIGHT; break;
    case NSHomeFunctionKey: code = K_HOME; break;
    case NSEndFunctionKey: code = K_END; break;
    case NSPageUpFunctionKey: code = K_PGUP; break;
    case NSPageDownFunctionKey: code = K_PGDN; break;
    case 0x1B: code = K_ESC; break;
    case NSInsertFunctionKey: case NSHelpFunctionKey:
        if (shift) { do_paste(); return; }
        code = K_INS;
        break;
    default:
        if (k0 >= NSF1FunctionKey && k0 <= NSF12FunctionKey) code = K_F1 + (int)(k0 - NSF1FunctionKey);
        break;
    }
    if (code != K_NONE) { push((Key){ .code = code, .ctrl = ctrl, .alt = opt, .shift = shift }); return; }
    if (cmd) return;                            /* Cmd+клавиша, которую не взяло меню: не наша */
    if (ctrl) {
        Key k;
        if (gb_mac_key(k0, e.keyCode, opt, &k)) {
            if (k.ch == 'v' && !k.alt) do_paste();   /* Ctrl+V: вставка, как в X11 */
            else push(k);
        }
        return;
    }
    if (lalt) { push_text(ign, true); return; }   /* левая Option: Alt+символ без модификатора */
    [self interpretKeyEvents:@[ e ]];
}
- (void)copy:(id)sender { (void)sender; if (G.ui) push((Key){ .code = K_CHAR, .ch = 'c', .ctrl = true }); }   /* Cmd+C: как Ctrl+C (копировать выделение) */
- (void)paste:(id)sender { (void)sender; do_paste(); }

/* NSTextInputClient: набор без показа предварительного текста (IME показывает своё окно у курсора). */
- (void)insertText:(id)string replacementRange:(NSRange)r {
    (void)r;
    g_marked = false;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    push_text(s, false);
}
- (void)doCommandBySelector:(SEL)s {            /* команды от IME вне набора; без super: не пищать */
    int code = K_NONE;
    if (s == @selector(insertNewline:)) code = K_ENTER;
    else if (s == @selector(deleteBackward:)) code = K_BS;
    else if (s == @selector(deleteForward:)) code = K_DEL;
    else if (s == @selector(cancelOperation:)) code = K_ESC;
    else if (s == @selector(insertTab:)) code = K_TAB;
    else if (s == @selector(insertBacktab:)) code = K_BTAB;
    else if (s == @selector(moveUp:)) code = K_UP;
    else if (s == @selector(moveDown:)) code = K_DOWN;
    else if (s == @selector(moveLeft:)) code = K_LEFT;
    else if (s == @selector(moveRight:)) code = K_RIGHT;
    if (code != K_NONE) push((Key){ .code = code });
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    (void)sel; (void)rep;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    g_marked = s.length > 0;
}
- (void)unmarkText { g_marked = false; }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return g_marked ? NSMakeRange(0, 1) : NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return g_marked; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)actual {
    (void)r;
    if (actual) *actual = NSMakeRange(NSNotFound, 0);
    return nil;
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)actual {
    (void)r;
    if (actual) *actual = NSMakeRange(NSNotFound, 0);
    NSRect cell = NSMakeRect((CGFloat)G.cx * cw, (CGFloat)G.cy * chh, cw, chh);
    if (!self.window) return cell;
    return [self.window convertRectToScreen:[self convertRect:cell toView:nil]];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)p { (void)p; return NSNotFound; }

/* ---------- мышь ---------- */
- (void)mouseDown:(NSEvent *)e { mouse_ev(self, e, 1); }
- (void)rightMouseDown:(NSEvent *)e { mouse_ev(self, e, 3); }
- (void)otherMouseDown:(NSEvent *)e {
    NSInteger b = e.buttonNumber;               /* 2: средняя, 3/4: боковые «назад»/«вперёд» */
    if (b == 2) mouse_ev(self, e, 2);
    else if (b == 3 || b == 4) { last_mx = -1; mouse_ev(self, e, b == 3 ? 8 : 9); }
}
- (void)mouseUp:(NSEvent *)e { last_mx = -1; mouse_ev(self, e, 7); }   /* №38: отпускание левой: button 7 */
- (void)mouseDragged:(NSEvent *)e { mouse_ev(self, e, 6); }
- (void)mouseMoved:(NSEvent *)e { mouse_ev(self, e, 0); }
- (void)rightMouseDragged:(NSEvent *)e { mouse_ev(self, e, 0); }
- (void)otherMouseDragged:(NSEvent *)e { mouse_ev(self, e, 0); }
- (void)scrollWheel:(NSEvent *)e {
    /* Тачпад и Magic Mouse дают точные смещения в точках (строка = высота клетки), колесо: в строках. */
    if (e.phase == NSEventPhaseBegan) wheel_acc = 0;
    double d = e.scrollingDeltaY;
    if (e.hasPreciseScrollingDeltas) d /= chh;
    wheel_acc += d;
    for (int i = 0; wheel_acc >= 1 && i < 10; i++) { wheel_acc -= 1; mouse_ev(self, e, 4); }
    for (int i = 0; wheel_acc <= -1 && i < 10; i++) { wheel_acc += 1; mouse_ev(self, e, 5); }
    if (wheel_acc > 1 || wheel_acc < -1) wheel_acc = 0;
}

/* ---------- перетаскивание файла (только первый, только локальный путь) ---------- */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { (void)sender; return g_ui_drop ? NSDragOperationCopy : NSDragOperationNone; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray *urls = [sender.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                             options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    NSURL *u = urls.firstObject;
    if (!u || !g_ui_drop) return NO;
    const char *fs = u.fileSystemRepresentation;
    char path[4096];
    size_t k = fs ? strlen(fs) : 0;
    if (!k || k >= sizeof path) return NO;
    memcpy(path, fs, k + 1);
    bool ok = path[0] == '/' && !text_unsafe(path);   /* D-4 */
    if (ok) g_ui_drop(path);
    wipe(path, sizeof path);
    return ok;
}
@end

/* ---------- приложение и окно: делегат, меню ---------- */
@implementation AstraDelegate
/* Cmd+Q, выход из меню, завершение сеанса: как Ctrl+Q. Программа сама завершится и затрёт секреты;
 * [NSApp terminate:] позвал бы exit() в обход этого. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    push((Key){ .code = K_CHAR, .ch = 'q', .ctrl = true });
    return NSTerminateCancel;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return NO; }
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app { (void)app; return YES; }
- (BOOL)windowShouldClose:(NSWindow *)w {      /* закрытие окна = выход, как Ctrl+Q */
    (void)w;
    G.closed = true;
    [g_win orderOut:nil];
    vis_update();
    push((Key){ .code = K_CHAR, .ch = 'q', .ctrl = true });
    return NO;
}
- (void)windowDidResize:(NSNotification *)n { (void)n; fit_grid(); }
- (void)windowDidChangeOcclusionState:(NSNotification *)n { (void)n; vis_update(); }
- (void)windowDidMiniaturize:(NSNotification *)n { (void)n; vis_update(); }
- (void)windowDidDeminiaturize:(NSNotification *)n { (void)n; vis_update(); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n { (void)n; [g_view setNeedsDisplay:YES]; }
- (void)astraZoomIn:(id)sender { (void)sender; zoom_cmd(1); }
- (void)astraZoomOut:(id)sender { (void)sender; zoom_cmd(-1); }
- (void)astraZoomReset:(id)sender { (void)sender; zoom_cmd(0); }
@end

static NSMenuItem *menu_item(NSMenu *m, NSString *title, SEL act, NSString *key, NSEventModifierFlags mods, id target) {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:act keyEquivalent:key];
    it.keyEquivalentModifierMask = mods;
    it.target = target;                         /* nil: по цепочке ответчиков (вид: copy:/paste:) */
    [m addItem:it];
    return it;
}
static NSMenu *menu_sub(NSMenu *bar, NSString *title) {
    NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];
    top.submenu = m;
    [bar addItem:top];
    return m;
}
static void menu_make(void) {
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];
    NSEventModifierFlags C = NSEventModifierFlagCommand;
    NSMenu *app = menu_sub(bar, @"Astra DS");
    menu_item(app, @"Скрыть Astra DS", @selector(hide:), @"h", C, NSApp);
    [app addItem:[NSMenuItem separatorItem]];
    menu_item(app, @"Выйти из Astra DS", @selector(terminate:), @"q", C, NSApp);
    NSMenu *ed = menu_sub(bar, @"Правка");
    menu_item(ed, @"Копировать", @selector(copy:), @"c", C, nil);
    menu_item(ed, @"Вставить", @selector(paste:), @"v", C, nil);
    NSMenu *vw = menu_sub(bar, @"Вид");
    menu_item(vw, @"Крупнее", @selector(astraZoomIn:), @"=", C, g_delegate);
    menu_item(vw, @"Мельче", @selector(astraZoomOut:), @"-", C, g_delegate);
    menu_item(vw, @"Обычный размер", @selector(astraZoomReset:), @"0", C, g_delegate);
    [vw addItem:[NSMenuItem separatorItem]];
    menu_item(vw, @"Полный экран", @selector(toggleFullScreen:), @"f", C | NSEventModifierFlagControl, nil);
    NSApp.mainMenu = bar;
}
/* Значок в Dock: самый крупный из ASTRA_ICON (ARGB, формат _NET_WM_ICON: ширина, высота, точки). */
static void app_icon(void) {
    const unsigned long *p = ASTRA_ICON, *end = ASTRA_ICON + sizeof ASTRA_ICON / sizeof *ASTRA_ICON, *best = NULL;
    unsigned long bw = 0;
    while (p + 2 <= end) {
        unsigned long w = p[0], h = p[1];
        if (!w || !h || w > 1024 || h > 1024 || (unsigned long)(end - p - 2) < w * h) break;
        if (w > bw) { bw = w; best = p; }
        p += 2 + w * h;
    }
    if (!best) return;
    size_t w = best[0], h = best[1];
    u8 *px = malloc(w * h * 4);
    if (!px) return;
    for (size_t i = 0; i < w * h; i++) {        /* RGBA с предумноженной прозрачностью */
        unsigned long v = best[2 + i];
        unsigned a = (unsigned)(v >> 24 & 255);
        px[i * 4] = (u8)((v >> 16 & 255) * a / 255);
        px[i * 4 + 1] = (u8)((v >> 8 & 255) * a / 255);
        px[i * 4 + 2] = (u8)((v & 255) * a / 255);
        px[i * 4 + 3] = (u8)a;
    }
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, px, (CFIndex)(w * h * 4));
    free(px);
    if (!d) return;
    CGDataProviderRef pr = CGDataProviderCreateWithCFData(d);
    CFRelease(d);
    if (!pr) return;
    CGImageRef im = CGImageCreate(w, h, 8, 32, w * 4, srgb, (CGBitmapInfo)kCGImageAlphaPremultipliedLast, pr, NULL, true,
                                  kCGRenderingIntentDefault);
    CGDataProviderRelease(pr);
    if (!im) return;
    NSImage *img = [[NSImage alloc] initWithCGImage:im size:NSMakeSize(w, h)];
    CGImageRelease(im);
    NSApp.applicationIconImage = img;
}

/* ---------- ожидание и разбор событий ---------- */
static void post_wake(void) {
    @autoreleasepool {
        NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                       timestamp:0 windowNumber:0 context:nil subtype:WAKE_SUBTYPE data1:WAKE_DATA data2:0];
        if (e) [NSApp postEvent:e atStart:YES];
    }
}
static bool is_wake(NSEvent *e) {
    return e.type == NSEventTypeApplicationDefined && e.subtype == WAKE_SUBTYPE && e.data1 == WAKE_DATA;
}
static void kq_cb(CFFileDescriptorRef f, CFOptionFlags types, void *info) {
    (void)f; (void)types; (void)info;
    post_wake();                                /* обратные вызовы выключились сами: включает следующий gb_wait */
}
/* Разобрать события до until (первое ждём, остальные: без ожидания). Вызывающий держит @autoreleasepool. */
static void pump_until(NSDate *until) {
    NSDate *past = [NSDate distantPast];
    NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
    for (int i = 0; e; i++) {
        if (!is_wake(e)) [NSApp sendEvent:e];
        if (i >= 512) break;                    /* поток событий без конца: вернуть управление программе */
        e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:past inMode:NSDefaultRunLoopMode dequeue:YES];
    }
}
static NSDate *date_for(int timeout_ms) {
    if (timeout_ms == 0 || kq_pending()) return [NSDate distantPast];
    if (timeout_ms < 0) return [NSDate distantFuture];
    return [NSDate dateWithTimeIntervalSinceNow:timeout_ms / 1000.0];
}
#ifdef ASTRA_VIDEO
static void vid_service(void) {                 /* главный поток: накопленную область видео: перерисовать */
    os_unfair_lock_lock(&vlock);
    CGRect r = vrect;
    bool set = vrect_set;
    vrect_set = false;
    os_unfair_lock_unlock(&vlock);
    if (!set || !g_view || g_hidden) return;
    @autoreleasepool {
        [g_view setNeedsDisplayInRect:NSRectFromCGRect(r)];
        [g_view displayIfNeeded];
        [CATransaction flush];
    }
}
static void vid_main(void *u) { (void)u; atomic_store(&v_posted, false); vid_service(); }
#else
static void vid_service(void) {}
#endif

/* macOS: ожидание главного цикла в ран-лупе Cocoa вместо poll (см. начало файла). plat_wait зовёт его первым;
 * false: окна нет, это не главный поток или вложенный вызов: тогда обычный poll. */
bool gb_wait(const Waitable *w, int n, int timeout_ms) {
    if (!g_open || g_hidden || g_kq < 0 || !kq_cf || wait_depth || !pthread_main_np()) return false;
    g_hooked = true;
    wait_depth++;
    struct kevent ch[64], rc[64];
    int nc = 0;
    for (int i = 0; i < n && nc + 2 <= 64; i++) {
        if (w[i].h < 0) continue;
        short ev = w[i].ev ? w[i].ev : POLLIN;
        uintptr_t fd = (uintptr_t)w[i].h;
        struct kevent *kp;                      /* EV_SET может вычислять аргумент несколько раз: без nc++ внутри */
        if (ev & POLLIN) { kp = &ch[nc++]; EV_SET(kp, fd, EVFILT_READ, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, NULL); }
        if (ev & POLLOUT) { kp = &ch[nc++]; EV_SET(kp, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, NULL); }
    }
    struct timespec z = { 0, 0 };
    bool now = false;
    if (nc) {
        int r = kevent(g_kq, ch, nc, rc, nc, &z);
        for (int i = 0; i < r; i++)
            if ((rc[i].flags & EV_ERROR) && rc[i].data != 0) now = true;   /* плохой дескриптор: как POLLNVAL у poll */
        struct kevent t;
        if (!now && kevent(g_kq, NULL, 0, &t, 1, &z) > 0) now = true;   /* уже готов: ждать нечего */
    }
    @autoreleasepool {
        if (!now && nc) CFFileDescriptorEnableCallBacks(kq_cf, kCFFileDescriptorReadCallBack);
        pump_until(now ? [NSDate distantPast] : date_for(timeout_ms));
    }
    if (nc) {                                   /* снять всё: между ожиданиями kqueue пуст */
        for (int i = 0; i < nc; i++) ch[i].flags = EV_DELETE | EV_RECEIPT;
        kevent(g_kq, ch, nc, rc, nc, &z);
    }
    vid_service();
    wait_depth--;
    return true;
}

void gb_pump(int timeout_ms) {
    if (!g_open) return;
    kick_drain();
    if (g_hidden) {                             /* снимок: событий окна нет, только очередь и kick */
        if (timeout_ms != 0 && !kq_pending()) {
            struct pollfd p = { kick_r, POLLIN, 0 };
            poll(&p, 1, timeout_ms);
            kick_drain();
        }
        return;
    }
    @autoreleasepool { pump_until(date_for(timeout_ms)); }
    vid_service();
}

static void tick_fn(void *u) { (void)u; kick(); }
static void fallback_start(void) {              /* запасной путь без gb_wait: будить poll каждые 20 мс */
    if (g_tick) return;
    g_tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
    if (!g_tick) return;
    dispatch_source_set_timer(g_tick, dispatch_time(DISPATCH_TIME_NOW, 0), 20 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler_f(g_tick, tick_fn);
    dispatch_resume(g_tick);
}
int gb_waitables(Waitable *out, int max) {
    if (!g_open || max < 1 || kick_r < 0) return 0;
    if (kq_pending()) kick();                   /* в очереди уже есть ввод: poll не должен засыпать */
    if (!g_hidden && !g_hooked && ++waitables_calls >= 2) fallback_start();
    out[0] = (Waitable){ kick_r, POLLIN };
    return 1;
}

/* ---------- договор бэкенда ---------- */
void gb_flush(void) {
    if (!g_open || !G.dirty) return;
    bool any = false;
    for (int y = 0; y < G.rows; y++) {
        if (!G.dirty[y]) continue;
        G.dirty[y] = 0;
        if (g_hidden || !g_view) continue;
        [g_view setNeedsDisplayInRect:NSMakeRect(0, (CGFloat)y * chh, g_view.bounds.size.width, chh)];
        any = true;
    }
    if (g_hidden || !g_view) return;
    if (any || g_view.needsDisplay) {
        @autoreleasepool {
            [g_view displayIfNeeded];           /* сразу: ран-луп до следующего ожидания может не крутиться */
            [CATransaction flush];
        }
    }
}
void gb_bell(void) { if (g_open && !g_hidden) NSBeep(); }

void gb_inject(const Key *k) {
    if (k->code != K_MOUSE || !g_open) { kq_push(k); return; }
    CGFloat px = k->x * cw + cw / 2.0, py = k->y * chh + chh / 2.0;
    NSEventModifierFlags f = (k->ctrl ? NSEventModifierFlagControl : 0) | (k->shift ? NSEventModifierFlagShift : 0) |
                             (k->alt ? NSEventModifierFlagOption : 0);
    int b = k->button == 99 ? 1 : k->button;
    NSEventType t;
    switch (b) {
    case 0: t = NSEventTypeMouseMoved; break;
    case 1: t = NSEventTypeLeftMouseDown; break;
    case 3: t = NSEventTypeRightMouseDown; break;
    case 6: t = NSEventTypeLeftMouseDragged; break;
    case 7: t = NSEventTypeLeftMouseUp; break;
    default: t = NSEventTypeApplicationDefined; break;   /* колесо, средняя и боковые: напрямую */
    }
    if (g_hidden || !g_view || t == NSEventTypeApplicationDefined) {
        if (b == 7 || b == 8 || b == 9) last_mx = -1;
        mouse_px(px, py, b, f);
        return;
    }
    /* настоящее NSEvent через обработчики вида: проверяется путь NSEvent -> K_MOUSE */
    @autoreleasepool {
        NSPoint wp = [g_view convertPoint:NSMakePoint(px, py) toView:nil];
        NSEvent *e = [NSEvent mouseEventWithType:t location:wp modifierFlags:f timestamp:[NSProcessInfo processInfo].systemUptime
                                    windowNumber:g_win.windowNumber context:nil eventNumber:0 clickCount:1 pressure:b == 7 ? 0 : 1];
        if (!e) return;
        switch (b) {
        case 0: [g_view mouseMoved:e]; break;
        case 1: [g_view mouseDown:e]; break;
        case 3: [g_view rightMouseDown:e]; break;
        case 6: [g_view mouseDragged:e]; break;
        default: [g_view mouseUp:e]; break;
        }
    }
}

/* ---------- снимок: вся сетка -> BMP (32 бита, сверху вниз) ---------- */
bool gb_snapshot(const char *path) {
    if (!g_open || !G.ch || !srgb) return false;
    int W = G.cols * cw, H = G.rows * chh;
    CGContextRef c = CGBitmapContextCreate(NULL, (size_t)W, (size_t)H, 8, (size_t)W * 4, srgb,
                                           (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (!c) return false;
    CGContextTranslateCTM(c, 0, H);             /* как в окне: y вниз */
    CGContextScaleCTM(c, 1, -1);
    CGContextSetFillColorSpace(c, srgb);
    CGFloat old = draw_scale;
    draw_scale = 1;
    for (int y = 0; y < G.rows; y++) draw_line(c, y);
    draw_scale = old;
    const u8 *data = CGBitmapContextGetData(c);   /* B, G, R, A в памяти; строка 0: верх картинки */
    size_t bpr = CGBitmapContextGetBytesPerRow(c);
    bool ok = false;
    FILE *fp = data ? fopen(path, "wb") : NULL;
    if (fp) {
        uint32_t sz = (uint32_t)W * (uint32_t)H * 4, off = 14 + 40;
        unsigned char h[54] = { 'B', 'M' };
        uint32_t v[] = { off + sz, 0, off, 40, (uint32_t)W, (uint32_t)-H };
        memcpy(h + 2, &v[0], 4); memcpy(h + 10, &v[2], 4); memcpy(h + 14, &v[3], 4);
        memcpy(h + 18, &v[4], 4); memcpy(h + 22, &v[5], 4);
        h[26] = 1; h[28] = 32;
        memcpy(h + 34, &sz, 4);
        ok = fwrite(h, 1, sizeof h, fp) == sizeof h;
        unsigned char *line = malloc((size_t)W * 4);
        if (!line) ok = false;
        for (int y = 0; ok && y < H; y++) {
            memcpy(line, data + (size_t)y * bpr, (size_t)W * 4);
            for (int x = 0; x < W; x++) line[x * 4 + 3] = 255;
            ok = fwrite(line, 1, (size_t)W * 4, fp) == (size_t)W * 4;
        }
        free(line);
        ok = fclose(fp) == 0 && ok;
    }
    CGContextRelease(c);
    return ok;
}

void gb_clipboard(const char *utf8, size_t n) {
    if (!g_open || g_hidden || !n) return;      /* --shot не трогает буфер владельца */
    @autoreleasepool {
        NSString *s = [[NSString alloc] initWithBytes:utf8 length:n encoding:NSUTF8StringEncoding];
        if (!s) return;
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:s forType:NSPasteboardTypeString];
    }
}

/* №15: масштаб: Menlo 9..28 pt; окно прежнего размера, меняется число клеток. */
bool gb_zoom(int step) {
    int z = step ? zoom + 10 * step : 100;
    double sz = FONT_PT * z / 100.0;
    if ((step > 0 && sz > 28) || (step < 0 && sz < 9) || z == zoom || !g_open) return false;
    zoom = z;
    @autoreleasepool {
        fonts_free();
        fonts_make();
        if (!g_hidden && g_win) {
            g_win.contentResizeIncrements = NSMakeSize(cw, chh);
            g_win.contentMinSize = NSMakeSize(50 * cw, 12 * chh);
            NSSize s = g_view.bounds.size;
            int cols = (int)(s.width / cw), rows = (int)(s.height / chh);
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            if (cols != G.cols || rows != G.rows) { grid_resize(cols, rows); if (G.ui) push((Key){ .code = K_RESIZE }); }
            else grid_mark_all();
            [g_view setNeedsDisplay:YES];
            gb_flush();
        }
    }
    return true;
}
int gb_zoom_pct(void) { return zoom; }
/* №16: полный экран: toggleFullScreen: (переход асинхронный: возвращается желаемое состояние) */
int gb_fullscreen(int mode) {
    static bool on;
    if (!g_win || g_hidden) return on;
    on = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    bool want = mode < 0 ? !on : mode != 0;
    if (want != on) [g_win toggleFullScreen:nil];
    on = want;
    return on;
}

static void activate_app(void) {
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
    if (@available(macOS 14.0, *)) { [NSApp activate]; return; }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
}

bool gb_open(bool hide) {
    if (g_open) return true;
    if (!pthread_main_np()) return false;       /* Cocoa: только главный поток */
    if (!hide) {                                /* нет графического сеанса (ssh, launchd): остаётся терминал */
        CFDictionaryRef sess = CGSessionCopyCurrentDictionary();
        if (!sess) return false;
        CFRelease(sess);
    }
    if (!srgb) srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!srgb) return false;
    g_hidden = hide;
    /* Цвета как у Discord (тёмная тема), как в X11 */
    G.def_bg = 0x313338;
    G.def_fg = 0xDBDEE1;
    int kp[2];
    if (pipe(kp) == 0) {
        for (int i = 0; i < 2; i++) {
            fcntl(kp[i], F_SETFD, FD_CLOEXEC);
            fcntl(kp[i], F_SETFL, fcntl(kp[i], F_GETFL) | O_NONBLOCK);
        }
        kick_r = kp[0];
        kick_w = kp[1];
    }
    @autoreleasepool {
        fonts_make();
        if (hide) {                             /* снимок: окно не нужно, только сетка и шрифты */
            grid_resize(COLS0, ROWS0);
            g_open = true;
            return true;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];   /* значок в Dock и меню, без .app */
        g_delegate = [[AstraDelegate alloc] init];
        NSApp.delegate = g_delegate;
        menu_make();
        app_icon();
        [NSApp finishLaunching];

        CGFloat W = (CGFloat)COLS0 * cw, H = (CGFloat)ROWS0 * chh;
        NSScreen *scr = [NSScreen mainScreen];
        if (scr) {
            NSRect vf = scr.visibleFrame;
            if (W > vf.size.width) W = floor(vf.size.width / cw) * cw;
            if (H > vf.size.height - 40) H = floor((vf.size.height - 40) / chh) * chh;
        }
        g_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered defer:NO];
        if (!g_win) { g_delegate = nil; return false; }
        g_win.releasedWhenClosed = NO;
        g_win.restorable = NO;
        g_win.title = @"Astra DS";
        g_win.delegate = g_delegate;
        g_win.backgroundColor = [NSColor colorWithSRGBRed:(G.def_bg >> 16 & 255) / 255.0 green:(G.def_bg >> 8 & 255) / 255.0
                                                     blue:(G.def_bg & 255) / 255.0 alpha:1];
        g_win.contentResizeIncrements = NSMakeSize(cw, chh);
        g_win.contentMinSize = NSMakeSize(50 * cw, 12 * chh);
        g_win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        g_win.tabbingMode = NSWindowTabbingModeDisallowed;
#ifdef ASTRA_VIDEO
        if (cap_excl) g_win.sharingType = NSWindowSharingNone;
#endif
        g_view = [[AstraView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
        g_win.contentView = g_view;
        [g_win makeFirstResponder:g_view];
        [g_win center];

        /* ожидание: kqueue в CFFileDescriptor, источник: в общих режимах главного ран-лупа */
        g_kq = kqueue();
        if (g_kq >= 0) {
            fcntl(g_kq, F_SETFD, FD_CLOEXEC);
            kq_cf = CFFileDescriptorCreate(kCFAllocatorDefault, g_kq, false, kq_cb, NULL);
            CFRunLoopSourceRef src = kq_cf ? CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, kq_cf, 0) : NULL;
            if (src) { CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes); CFRelease(src); }
            else if (kq_cf) { CFRelease(kq_cf); kq_cf = NULL; }
        }
        if (!g_activity) g_activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                                                                    reason:@"Astra DS: голос и соединение"];
        NSSize s = g_view.bounds.size;
        grid_resize((int)(s.width / cw), (int)(s.height / chh));
        [g_win makeKeyAndOrderFront:nil];
        activate_app();
        vis_update();
        atexit(gui_atexit);
    }
    g_open = true;
    return true;
}

#ifdef ASTRA_VIDEO
void *gb_vbuf_alloc(size_t n) { return malloc(n); }
void gb_vbuf_free(void *p, size_t n) { (void)n; free(p); }
bool gb_video_ok(void) { return g_open; }
bool gb_video_shown(void) { return g_open && (g_hidden || atomic_load(&g_visible)); }
void gb_cell_px(int *w, int *h) { *w = cw; *h = chh; }   /* в точках: растр видео строится в точках, Retina растягивает */
void gb_video_inval(int x, int y, int w, int h) {   /* из любого потока */
    if (!g_open || g_hidden || w <= 0 || h <= 0) return;
    CGRect r = CGRectMake(x, y, w, h);
    os_unfair_lock_lock(&vlock);
    bool was = vrect_set;
    vrect = was ? CGRectUnion(vrect, r) : r;
    vrect_set = true;
    os_unfair_lock_unlock(&vlock);
    if (!was) kick();                           /* главный поток в poll (не в ран-лупе): разбудить, gb_pump дорисует */
    if (!atomic_exchange(&v_posted, true)) dispatch_async_f(dispatch_get_main_queue(), NULL, vid_main);
}
/* Скрыть окно из захвата экрана: NSWindowSharingNone (CGWindowList, старый захват). */
bool term_capture_exclude(bool on) {
    cap_excl = on;
    if (!g_win) return false;
    g_win.sharingType = on ? NSWindowSharingNone : NSWindowSharingReadOnly;
    return true;
}
#endif
#endif
