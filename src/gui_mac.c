/* Окно macOS: пока не сделано (шаг 2 переноса — Cocoa). Бэкенд-заглушка: gb_open отказывает, и программа
 * работает в терминале (--tui по умолчанию, --cli построчно) — как Linux без $DISPLAY. */
#ifdef __APPLE__
#include "gui.h"

bool gb_open(bool hidden) { (void)hidden; return false; }
void gb_flush(void) {}
void gb_pump(int timeout_ms) { (void)timeout_ms; }
void gb_bell(void) {}
int gb_waitables(Waitable *out, int max) { (void)out; (void)max; return 0; }
void gb_inject(const Key *k) { (void)k; }
bool gb_snapshot(const char *path) { (void)path; return false; }
void gb_clipboard(const char *utf8, size_t n) { (void)utf8; (void)n; }
int gb_fine_scale(void) { return 0; }
bool gb_zoom(int step) { (void)step; return false; }
int gb_fullscreen(int mode) { (void)mode; return -1; }
int gb_zoom_pct(void) { return 100; }
bool gb_wait(const Waitable *w, int n, int timeout_ms) { (void)w; (void)n; (void)timeout_ms; return false; }   /* окна нет: обычный poll */
#ifdef ASTRA_VIDEO
bool gb_video_ok(void) { return false; }
void gb_cell_px(int *cw, int *ch) { *cw = 8; *ch = 16; }
void gb_video_inval(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
bool gb_video_shown(void) { return false; }
void *gb_vbuf_alloc(size_t n) { return calloc(1, n); }
void gb_vbuf_free(void *p, size_t n) { (void)n; free(p); }
#endif
#endif
