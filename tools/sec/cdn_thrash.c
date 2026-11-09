/* tools/sec/cdn_thrash.c — стенд: сколько POST /attachments/refresh-urls даёт код обновления ссылок CDN из
 * store.c, если на экране много просроченных ссылок. Код cdn_* вырезается из ТЕКУЩЕГО store.c (см.
 * team/security.md) в build-sec/cdn_extract.c; http_async — счётчик, ответ сразу (200 без ссылок или 500).
 * Имитация: N разных ссылок с ex=1 видны на экране, кадр каждые 16 мс, 60 с; для каждой ссылки на каждом
 * кадре — то, что делает ui.c att_url: cdn_want (видимая) + cdn_fresh; затем store_tick -> cdn_flush.
 * Плюс переключение каналов туда-обратно каждые 2 с (сброс таблицы). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/mc.h"
Opts O;
Store S;
static int posts;
static long reply_status = 200;
void http_async(const char *method, const char *path, const char *body, HttpCb cb, void *ctx) {
    (void)method; (void)path; (void)body;
    posts++;
    Resp r;
    memset(&r, 0, sizeof r);
    r.status = reply_status;
    buf_puts(&r.body, "{\"refreshed_urls\":[]}");
    if (cb) cb(&r, ctx);
    buf_free(&r.body);
}
void ui_dirty(void) {}
void plat_wipe(void *p, size_t n) { SecureZeroMemory(p, n); }
int64_t plat_mono_ms(void) { return 0; }
int64_t plat_wall_ms(void) { return 1700000000000LL; }
int64_t plat_timegm(struct tm *t) { (void)t; return 0; }
void plat_localtime(int64_t s, struct tm *o) { (void)s; memset(o, 0, sizeof *o); }
void *plat_pages(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void plat_pages_free(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }
static void cdn_flush(i64 now);
#include "../../build-sec/cdn_extract.c"
static i64 base = 1000000;
static int run(int n, bool switching, long status) {
    posts = 0;
    base += 1000000;                            /* cdn_last статичен: каждый прогон — с нового времени */
    reply_status = status;
    u64 ch0 = (u64)(base / 1000000) * 2; S.open_ch = ch0;   /* свой канал на каждый прогон: таблица чистая */
    for (i64 t = 0; t < 60000; t += 16) {
        if (switching && t % 2000 == 0) S.open_ch = S.open_ch == ch0 ? ch0 + 1 : ch0;
        for (int i = 0; i < n; i++) {
            char u[160];
            snprintf(u, sizeof u, "https://cdn.discordapp.com/attachments/%llu/%d/a.png?ex=1&is=1&hm=00", (unsigned long long)S.open_ch, i + 1);
            cdn_want(u);
            cdn_fresh(u);
        }
        cdn_flush(base + t);
    }
    return posts;
}
int main(void) {
    static const int N[] = { 8, 16, 24, 200 };
    for (int k = 0; k < 4; k++) {

        int a = run(N[k], false, 200);

        int b = run(N[k], false, 500);
        int c = run(N[k], true, 200);
        printf("ссылок на экране %3d, 60 с, кадр 16 мс: POST = %3d (ответ 200), %3d (ответ 500), %3d (смена канала каждые 2 с)\n", N[k], a, b, c);
    }
    return 0;
}
