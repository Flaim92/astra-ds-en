/* Окно-терминал: общая часть (gui_grid.c) и платформенные бэкенды (gui_win.c — Win32+GDI,
 * gui_x11.c — Xlib). Окно — сетка символов: тот же поток VT, что ui.c пишет в консоль
 * (позиционирование, SGR, очистка, ?2026), разбирается в ячейки; бэкенд рисует только
 * изменившиеся строки. Ввод — те же Key, что у консоли (plat.h), плюс мышь (K_MOUSE). */
#ifndef GUI_H
#define GUI_H

#include "mc.h"

/* Windows: Waitable{0, W_WINMSG} — «и очередь сообщений окна» для plat_wait (MsgWaitForMultipleObjectsEx).
 * TODO координатору: перенести в plat.h рядом с W_IN/W_OUT. */
#define W_WINMSG 0x4000

/* Атрибуты ячейки */
#define A_BOLD 1
#define A_UL   2
#define A_INV  4
#define A_ITAL 8                                /* SGR 3 — курсив (№45) */
#define A_STRIKE 16                             /* SGR 9 — зачёркнутый (№45) */

/* ---------- мелкая сетка картинок (формат задаёт ui.c, см. там FINE_K) ----------
 *   ESC _ mincord-img ; X ; Y ; W ; H ; K ; DATA ESC \
 * X, Y — левая верхняя клетка (с 0), W x H — прямоугольник в обычных клетках, K — масштаб (2..4).
 * DATA — (K*W) x (K*H) мелких клеток по строкам, ровно 13 байт каждая: символ 0x20..0x7E, цвет
 * символа RRGGBB и цвет фона RRGGBB (hex). Проверки: прямоугольник целиком внутри сетки, K не больше
 * того, что окно умеет нарисовать (gb_fine_scale), длина DATA — точно, байты — только допустимые;
 * любое нарушение — слой отбрасывается целиком. Разбор потоковый: сырые данные не копятся.
 * Слой рисуется поверх клеток прямоугольника мелким шрифтом (высота клетки / K). Снимается, когда в
 * его прямоугольник приходит обычная запись символа (ESC[2J, ESC[K тоже) или меняется размер сетки;
 * если до конца кадра (ESC[?2026l) пришёл такой же слой — снятие отменяется без перерисовки.
 * Новый слой с теми же X, Y, W заменяет старый; пересекающийся — тоже. Слоёв не больше LAYER_MAX,
 * всего не больше LAYER_BYTES (7 байт на мелкую клетку), старые вытесняются. */
#define LAYER_MAX 6
#define LAYER_BYTES (192u << 10)                 /* 6 превью 32x8 при K=4 = 6 x 28 КБ */
typedef struct {
    int x, y, w, h, k;
    u32 hash;                   /* подпись содержимого: тот же слой повторно — без перерисовки */
    bool dead;                  /* в прямоугольник писали: снять в конце кадра, если не пришёл снова */
    u64 stamp;                  /* для вытеснения */
    u8 *c;                      /* (k*w)*(k*h) клеток по 7 байт: символ, fg r g b, bg r g b */
} Layer;

/* Цвет ячейки — индекс в палитре окна (до 256 цветов, заполняется по мере появления в потоке):
 * 0 — цвет темы по умолчанию (текст или фон), 1..255 — G.pal[i] = 0xRRGGBB. Палитра полна —
 * берётся ближайший уже известный цвет. */
typedef struct {
    int cols, rows;
    u32 *ch;                    /* символ ячейки; 0 — правая половина широкого символа слева */
    u8 *at;                     /* атрибуты A_* */
    u8 *fg, *bg;                /* индексы палитры */
    u8 *dirty;                  /* строка изменилась с прошлой отрисовки */
    int cx, cy;                 /* курсор вывода; cx == cols — «отложенный перенос» */
    u8 cur_at, cur_fg, cur_bg;  /* текущие SGR */
    bool sync;                  /* внутри ?2026h..l: рисовать только в конце кадра */
    bool ui;                    /* после term_init (полноэкранный интерфейс); иначе — построчный вход */
    bool tail;                  /* после последнего ввода в построчном режиме что-то напечатано */
    bool closed;                /* окно закрыто пользователем */
    u32 pal[256];
    int npal;                   /* занято pal[1..npal] */
    u32 def_fg, def_bg;         /* цвета темы (ставит бэкенд) */
    Layer lay[LAYER_MAX];
    int nlay;
    /* разбор потока */
    int st, np;
    int par[16];
    char priv;
    u32 u;
    int uneed;
} Grid;
extern Grid G;

void grid_resize(int cols, int rows);           /* сохраняет пересечение старой и новой сетки */
void grid_write(const char *s, size_t n);       /* разбор VT; рисует (gb_flush) вне ?2026 */
void grid_mark_all(void);                       /* перерисовать всё (смена темы, шрифта) */
/* Итоговые цвета ячейки (с учётом инверсии), 0xRRGGBB */
void grid_colors(u8 at, u8 fg, u8 bg, u32 *rf, u32 *rb);

/* Очередь ввода окна. Вставка из буфера обмена хранится отдельно и раздаётся по символу. */
void kq_push(const Key *k);
extern int gui_mouse_seen;
bool kq_pending(void);
void kq_paste_utf8(const char *s, size_t n);    /* \r\n и \r -> '\n'; управляющие — выбросить */
void kq_paste_utf16(const uint16_t *s, size_t n);
int kq_pop(Key *out, int max);
void kq_wipe(void);

/* Общие реализации term_* для окна (plat_*.c вызывает их, когда окно открыто). */
void gui_puts(const char *s);
bool gui_read_line(const char *prompt, char *out, size_t cap, bool hidden);
bool gui_init(void);
void gui_shutdown(void);
void gui_size(int *w, int *h);
void gui_write(const char *s, size_t n);
int gui_read(Key *out, int max);
void gui_atexit(void);                          /* текст ошибки после выхода — дождаться клавиши */
void gui_resize(int cols, int rows);            /* снимок: задать размер сетки (+ K_RESIZE) */

/* ---------- бэкенд (gui_win.c / gui_x11.c) ---------- */
bool gb_open(bool hidden);                      /* hidden — окно не показывается (снимок --shot) */
void gb_flush(void);                            /* нарисовать строки с dirty и сбросить флаги */
void gb_pump(int timeout_ms);                   /* разобрать события окна; ждать до timeout (-1 — без предела) */
void gb_bell(void);
int gb_waitables(Waitable *out, int max);
void gb_inject(const Key *k);                   /* снимок: мышь — настоящими событиями окна, прочее — в очередь */
bool gb_snapshot(const char *path);              /* снимок: вся сетка -> BMP (только по явному --shot) */
void gb_clipboard(const char *utf8, size_t n);  /* OSC 52: положить текст в буфер обмена */
int gb_fine_scale(void);                        /* наибольший K мелкой сетки, 0 — не умеет */
bool gb_zoom(int step);
int gb_fullscreen(int mode);                    /* №16: -1 переключить, 0/1; возврат — состояние */
#ifdef _WIN32
int gb_last_vk(void);                           /* №141: VK последнего нажатия клавиши/боковой кнопки в окне */
void gb_notify(const char *title, const char *text);   /* №123: всплывашка у значка в трее */
#endif
#ifdef _WIN32
bool gb_vk_key(unsigned vk, bool ctrl, bool alt, Key *out);           /* Ctrl+клавиша окна -> Key (selftest) */
#else
bool gb_ks_key(unsigned long base, unsigned long ks, bool alt, Key *out);   /* то же для X11 keysym (с Ctrl) */
#endif                         /* №15: масштаб шрифта, шаг 10 %; 0 — сброс; false — без изменений */
int gb_zoom_pct(void);
#ifdef ASTRA_VIDEO
/* ---------- видео просмотра трансляции (gui_grid.c; договор g_video_area/g_video_supported — golive.h) ---------- */
typedef struct { int x, y, w, h; const u32 *buf; int bw, bh; } GvPaint;   /* область, пиксели; картинка bw x bh (BGRA) */
void gvid_frame_begin(void);                    /* gui_size: начало render() */
void gvid_commit(void);                         /* ESC[?2026l: зафиксировать прямоугольник */
bool gvid_paint_lock(GvPaint *o);               /* бэкенд при рисовании: true — кадр есть, держит мьютекс */
void gvid_paint_unlock(void);
int gcam_paint_lock(GvPaint *o, int max);       /* плитки камер с кадром (cam.h); мьютекс — до gcam_paint_unlock всегда */
void gcam_paint_unlock(void);
/* бэкенд */
bool gb_video_ok(void);                         /* окно с растром (не --tui/--cli) */
void gb_cell_px(int *cw, int *ch);              /* размер клетки, пиксели */
void gb_video_inval(int x, int y, int w, int h);   /* перерисовать область (пиксели); из любого потока */
bool gb_video_shown(void);                      /* окно видно (не свёрнуто/не скрыто); из любого потока */
void *gb_vbuf_alloc(size_t n);                  /* растр кадра (X11: сегмент MIT-SHM, если можно); из любого потока */
void gb_vbuf_free(void *p, size_t n);
#endif

/* Для plat.h (координатору): 0 — терминал или окно не умеет; иначе наибольший масштаб K. */
int term_subcell_scale(void);
bool term_fine_images(void);                    /* ui.c: true — окно рисует мелкую сетку с K=4 */
/* №15: масштаб окна (Ctrl+= / Ctrl+- / Ctrl+0): step +1/-1 — на 10 %, 0 — 100 %. Шрифт 10..28 px, окно
 * своего размера не меняет (меняется число клеток, придёт K_RESIZE). false — консоль или упёрлись в предел. */
bool term_zoom(int step);
int term_zoom_pct(void);                        /* текущий масштаб, % (консоль — 100) */

#endif
