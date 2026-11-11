/* share_x11.c — демонстрация экрана «Astra DS Video» на Linux (X11). Интерфейс — share.h, тот же, что у streamtest.c
 * (Windows). Только при -DASTRA_VIDEO и не на Windows.
 *
 * Захват — со своего соединения с X-сервером в потоке трансляции (соединение окна интерфейса не трогаем).
 * Кадр берётся с корневого окна: весь экран или прямоугольник окна-источника. MIT-SHM (libXext подгружается
 * через dlopen, в зависимости сборки не входит); нет расширения — обычный XGetImage. Выход всегда 1280x720@30
 * с полями, кодер minih264, кадры -> golive_frame.
 *
 * Окно-источник снимается с экрана, поэтому чужое окно поверх него попало бы в эфир. Как и в Windows-версии,
 * это не допускается: пока источник перекрыт другим окном (по _NET_CLIENT_LIST_STACKING), свёрнут или на другом
 * рабочем столе — зрители видят последний чистый кадр (пауза с пояснением), после — сразу ключевой кадр.
 * Весь экран: видно всё, включая окно Astra DS (в X11 окно нельзя исключить из захвата).
 *
 * Wayland (niri, GNOME, KDE, sway): X11 чужих окон не видит, поэтому источник выбирает сам компоновщик в системном
 * окне xdg-desktop-portal (portal_wl.c, в потоке трансляции — окно Astra DS не замирает), кадры приходят потоком
 * PipeWire в общей памяти или линейным DMA-BUF без GPU (share_pw.c) и переводятся в YUV прямо из его буфера, без копии. Разрешение на захват
 * не сохраняется (persist_mode 0): системное окно — при каждом запуске. Кадр, уже переведённый целиком, повторно
 * не переводится (источник шлёт новый кадр только при изменениях). Окно Astra DS исключить нельзя (портал этого не
 * умеет): при показе экрана оно видно.
 * Звук трансляции — через PulseAudio/PipeWire (см. ниже). Буферы с содержимым экрана затираются перед освобождением. */
#if defined(ASTRA_VIDEO) && !defined(_WIN32) && !defined(__APPLE__)   /* macOS: share_mac.c */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "mc.h"
#include "share.h"
#include "golive.h"
#include <stdint.h>
#include "share_conv.h"
#include "share_pw.h"
#include "portal_wl.h"
#include <opus/opus.h>
#if __has_include(<pulse/pulseaudio.h>)
#include <pulse/pulseaudio.h>
#endif

#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#define MINIH264_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "../third_party/minih264/minih264e.h"
#pragma GCC diagnostic pop

#define SH_OW 1280
#define SH_OH 720
#define SH_FPS 30
#define SH_GOP_MS 4000.0

static pthread_mutex_t sh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t sh_thr;
static bool sh_thr_on;
static volatile int sh_stop, sh_want_idr, sh_done;
static ShareState sh;
static Window sh_win;
static int sh_mode;
static bool sa_want = true;
static bool sh_wl;                               /* этот запуск — через портал Wayland */
static char sh_wl_err[96];
static struct { Window w; long pid; char t[80]; } sh_known[24];
static int sh_nknown;

static double now_msf(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static void wipe_mem(void *p, size_t n) { if (p && n) explicit_bzero(p, n); }
static int sh_xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }   /* окно могли закрыть между запросами */
static void sh_finish(const char *why) {
    pthread_mutex_lock(&sh_mu);
    sh.active = false;
    sh.choosing = false;
    snprintf(sh.reason, sizeof sh.reason, "%s", why);
    pthread_mutex_unlock(&sh_mu);
}

/* ---------- свойства окон ---------- */
static unsigned char *prop_get(Display *d, Window w, const char *name, Atom type, unsigned long *n) {
    Atom a = XInternAtom(d, name, True), rt;
    int fmt;
    unsigned long left;
    unsigned char *p = NULL;
    *n = 0;
    if (a == None) return NULL;
    if (XGetWindowProperty(d, w, a, 0, 4096, False, type, &rt, &fmt, n, &left, &p) != Success || !p) { *n = 0; return NULL; }
    if (!*n) { XFree(p); return NULL; }
    return p;
}
static long prop_long(Display *d, Window w, const char *name, long def) {
    unsigned long n;
    unsigned char *p = prop_get(d, w, name, AnyPropertyType, &n);
    if (!p) return def;
    long v = (long)*(unsigned long *)(void *)p;
    XFree(p);
    return v;
}
static void win_title(Display *d, Window w, char *out, size_t cap) {
    unsigned long n;
    out[0] = 0;
    unsigned char *p = prop_get(d, w, "_NET_WM_NAME", XInternAtom(d, "UTF8_STRING", False), &n);
    if (!p) p = prop_get(d, w, "WM_NAME", XA_STRING, &n);
    if (!p) return;
    size_t k = n < cap - 1 ? n : cap - 1;
    while (k && k < n && ((unsigned char)p[k] & 0xC0) == 0x80) k--;   /* не резать посреди символа UTF-8 */
    memcpy(out, p, k);
    out[k] = 0;
    XFree(p);
}
static bool win_has_state(Display *d, Window w, const char *state) {
    unsigned long n;
    Atom want = XInternAtom(d, state, True);
    if (want == None) return false;
    unsigned char *p = prop_get(d, w, "_NET_WM_STATE", XA_ATOM, &n);
    bool hit = false;
    for (unsigned long i = 0; p && i < n; i++) if (((Atom *)(void *)p)[i] == want) hit = true;
    if (p) XFree(p);
    return hit;
}
/* Прямоугольник окна в координатах экрана; false — окна нет или оно не отображено. */
static bool win_rect(Display *d, Window w, int *x, int *y, int *ww, int *hh) {
    XWindowAttributes wa;
    Window child;
    if (!XGetWindowAttributes(d, w, &wa) || wa.map_state != IsViewable) return false;
    if (!XTranslateCoordinates(d, w, wa.root, 0, 0, x, y, &child)) return false;
    *ww = wa.width; *hh = wa.height;
    return true;
}
static bool win_exists(Display *d, Window w) { XWindowAttributes wa; return XGetWindowAttributes(d, w, &wa) != 0; }
static Window *client_list(Display *d, const char *name, unsigned long *n) {
    return (Window *)(void *)prop_get(d, DefaultRootWindow(d), name, XA_WINDOW, n);
}

/* Wayland: окна других программ X11 (Xwayland) не видит — источник выбирается в системном окне портала */
bool share_wayland(void) {
    const char *st = getenv("XDG_SESSION_TYPE"), *wd = getenv("WAYLAND_DISPLAY");
    return (wd && *wd) || (st && !strcmp(st, "wayland"));
}
int share_windows(char (*titles)[80], uintptr_t *ids, int max) {
    int cnt = 0;
    memset(sh_known, 0, sizeof sh_known);
    sh_nknown = 0;
    if (share_wayland()) return 0;               /* список X11 там пуст или чужой: интерфейс покажет выбор через портал */
    Display *d = XOpenDisplay(NULL);
    if (!d) return 0;
    XErrorHandler old = XSetErrorHandler(sh_xerr);
    unsigned long n;
    Window *l = client_list(d, "_NET_CLIENT_LIST", &n);
    long self = (long)getpid();
    for (unsigned long i = 0; l && i < n && cnt < max; i++) {
        int x, y, w, h;
        if (prop_long(d, l[i], "_NET_WM_PID", -1) == self) continue;       /* своё окно не предлагаем */
        if (win_has_state(d, l[i], "_NET_WM_STATE_HIDDEN") || win_has_state(d, l[i], "_NET_WM_STATE_SKIP_TASKBAR")) continue;
        if (!win_rect(d, l[i], &x, &y, &w, &h) || w < 64 || h < 64) continue;
        char t[80];
        win_title(d, l[i], t, sizeof t);
        if (!t[0]) continue;
        if (cnt < 24) { sh_known[cnt].w = l[i]; sh_known[cnt].pid = prop_long(d, l[i], "_NET_WM_PID", -1); memcpy(sh_known[cnt].t, t, sizeof t); }
        memcpy(titles[cnt], t, sizeof t);
        ids[cnt++] = (uintptr_t)l[i];
    }
    if (l) XFree(l);
    XSync(d, False);
    XSetErrorHandler(old);
    XCloseDisplay(d);
    sh_nknown = cnt < 24 ? cnt : 24;
    return cnt;
}

/* ---------- захват ---------- */
typedef struct {                                 /* XShmSegmentInfo (X11/extensions/XShm.h), без заголовка libXext */
    unsigned long shmseg;
    int shmid;
    char *shmaddr;
    Bool readOnly;
} ShmInfo;
typedef struct {
    Display *d;
    Window root;
    int scr_w, scr_h;
    void *xext;
    Bool (*q)(Display *);
    XImage *(*create)(Display *, Visual *, unsigned, int, char *, ShmInfo *, unsigned, unsigned);
    Bool (*attach)(Display *, ShmInfo *);
    Bool (*detach)(Display *, ShmInfo *);
    Bool (*get)(Display *, Drawable, XImage *, int, int, unsigned long);
    bool shm;
    ShmInfo si;
    XImage *img;                                 /* буфер захвата текущего размера */
    int iw, ih;
    /* кадр */
    int sw, sh, dx, dy, dw, dh, fit_w, fit_h;
    int *xmap;
    bool xident, refit;                          /* выбор точек один к одному; место картинки изменилось */
    u8 *y, *u, *v;
    ShMb mb;                                     /* отпечатки (Y+U+V) и возраст блоков 16x16, карта для кодера */
} Cap;

static void cap_img_free(Cap *c) {
    if (!c->img) return;
    if (c->shm) {
        wipe_mem(c->si.shmaddr, (size_t)c->img->bytes_per_line * (size_t)c->img->height);
        c->detach(c->d, &c->si);
        c->img->data = NULL;
        XDestroyImage(c->img);
        shmdt(c->si.shmaddr);
    } else {
        wipe_mem(c->img->data, (size_t)c->img->bytes_per_line * (size_t)c->img->height);
        XDestroyImage(c->img);                   /* освобождает и data */
    }
    c->img = NULL;
    c->iw = c->ih = 0;
}
/* Буфер SHM под w x h. false — нет расширения или памяти (тогда XGetImage). */
static bool cap_img_shm(Cap *c, int w, int h) {
    if (c->img && c->iw == w && c->ih == h) return true;
    cap_img_free(c);
    if (!c->shm) return false;
    memset(&c->si, 0, sizeof c->si);
    XImage *im = c->create(c->d, DefaultVisual(c->d, DefaultScreen(c->d)), (unsigned)DefaultDepth(c->d, DefaultScreen(c->d)),
                           ZPixmap, NULL, &c->si, (unsigned)w, (unsigned)h);
    if (!im) return false;
    c->si.shmid = shmget(IPC_PRIVATE, (size_t)im->bytes_per_line * (size_t)im->height, IPC_CREAT | 0600);
    if (c->si.shmid < 0) { XDestroyImage(im); return false; }
    c->si.shmaddr = im->data = shmat(c->si.shmid, NULL, 0);
    if (c->si.shmaddr == (char *)-1) { shmctl(c->si.shmid, IPC_RMID, NULL); im->data = NULL; XDestroyImage(im); return false; }
    c->si.readOnly = False;
    Bool ok = c->attach(c->d, &c->si);
    XSync(c->d, False);
    shmctl(c->si.shmid, IPC_RMID, NULL);         /* сегмент исчезнет сам после отсоединения обеих сторон */
    if (!ok) { im->data = NULL; XDestroyImage(im); shmdt(c->si.shmaddr); return false; }
    c->img = im; c->iw = w; c->ih = h;
    return true;
}
/* Снять прямоугольник экрана (уже обрезанный по границам) в c->img. */
static bool cap_grab(Cap *c, int x, int y, int w, int h) {
    if (c->shm && cap_img_shm(c, w, h)) {
        if (c->get(c->d, c->root, c->img, x, y, AllPlanes)) return true;
        c->shm = false;                          /* отказ SHM: дальше обычным способом */
        cap_img_free(c);
    }
    cap_img_free(c);
    c->img = XGetImage(c->d, c->root, x, y, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
    if (!c->img) return false;
    c->iw = w; c->ih = h;
    return true;
}
/* Место картинки sw x sh в кадре; при смене размера — поля чёрные, отпечатки блоков сброшены. */
static void cap_fit(Cap *c) {
    if (c->fit_w == c->sw && c->fit_h == c->sh) return;
    double kx = (double)SH_OW / c->sw, ky = (double)SH_OH / c->sh, sc = kx < ky ? kx : ky;
    if (sc > 1) sc = 1;
    c->dw = (int)(c->sw * sc + 0.5) & ~1; c->dh = (int)(c->sh * sc + 0.5) & ~1;
    if (c->dw < 2) c->dw = 2;
    if (c->dh < 2) c->dh = 2;
    if (c->dw > SH_OW) c->dw = SH_OW;
    if (c->dh > SH_OH) c->dh = SH_OH;
    c->dx = ((SH_OW - c->dw) / 2) & ~1; c->dy = ((SH_OH - c->dh) / 2) & ~1;
    size_t n = (size_t)SH_OW * SH_OH;
    memset(c->y, 16, n); memset(c->u, 128, n / 4); memset(c->v, 128, n / 4);
    shc_reset(&c->mb);
    c->xident = true;
    for (int x = 0; x < c->dw; x++) { c->xmap[x] = x * c->sw / c->dw; if (c->xmap[x] != x) c->xident = false; }
    c->fit_w = c->sw; c->fit_h = c->sh;
    c->refit = true;
}
/* Картинка sw x sh (32 бита: BGRX, rgb — RGBX; строки по bpl байт, не меньше sh строк) -> YUV420 BT.601 (SSE2 или
 * скалярно, share_conv.h) в место dx,dy,dw,dh — только блоки [mx0,mx1) x [my0,my1) сетки кадра (весь кадр —
 * 0,0,mbw,mbh); отпечатки снятых блоков — в том же проходе. Вернуть число изменившихся блоков 16x16. */
static int cap_convert(Cap *c, const u8 *data, size_t bpl, int height, int rgb, int unc, int mx0, int my0, int mx1, int my1) {
    if (c->refit) { mx0 = my0 = 0; mx1 = c->mb.mbw; my1 = c->mb.mbh; c->refit = false; }
    shc_scan_begin(&c->mb);
    int ox0 = mx0 * 16 > c->dx ? mx0 * 16 : c->dx, ox1 = mx1 * 16 < c->dx + c->dw ? mx1 * 16 : c->dx + c->dw;
    int oy0 = my0 * 16 > c->dy ? my0 * 16 : c->dy, oy1 = my1 * 16 < c->dy + c->dh ? my1 * 16 : c->dy + c->dh;
    if (ox0 >= ox1 || oy0 >= oy1) return 0;
    int j0 = ox0 - c->dx, j1 = ox1 - c->dx, i0 = oy0 - c->dy, i1 = oy1 - c->dy, hx0 = ox0 >> 4, hx1 = (ox1 + 15) >> 4, changed = 0;
    const int *xm = c->xident ? NULL : c->xmap;
    for (int i = i0; i < i1; i++) {
        int syy = i * c->sh / c->dh;
        if (syy >= height) syy = height - 1;
        const u8 *rp = data + (size_t)syy * bpl;
        /* DMA-BUF (некэшируемое отображение): нужные точки строки — сначала в кэшируемый буфер (share_pw.c) */
        if (unc) rp = shpw_row(rp, (size_t)(xm ? xm[j0] : j0) * 4, (size_t)((xm ? xm[j1 - 1] : j1 - 1) + 1) * 4);
        const u32 *row = (const u32 *)(const void *)rp;
        int oy = c->dy + i;
        u8 *yr = c->y + (size_t)oy * SH_OW + c->dx, *ur = NULL, *vr = NULL;
        if (!(oy & 1)) { ur = c->u + (size_t)(oy / 2) * (SH_OW / 2) + c->dx / 2; vr = c->v + (size_t)(oy / 2) * (SH_OW / 2) + c->dx / 2; }
        shc_row32x(row, xm, j0, j1, yr, ur, vr, rgb);
        if ((oy & 15) == 15 || i == i1 - 1)
            changed += shc_hash_row(&c->mb, c->y, c->u, c->v, SH_OW, SH_OW / 2, oy >> 4, hx0, hx1, ox0, ox1, oy0, oy1);
    }
    return changed;
}
/* Перекрыт ли источник окном, лежащим выше (и видимым на том же рабочем столе). name — заголовок перекрывшего. */
static bool win_covered(Display *d, Window w, int x, int y, int ww, int hh, char *name, size_t nn) {
    unsigned long n;
    Window *l = client_list(d, "_NET_CLIENT_LIST_STACKING", &n);   /* снизу вверх */
    bool above = false, hit = false;
    for (unsigned long i = 0; l && i < n && !hit; i++) {
        if (l[i] == w) { above = true; continue; }
        if (!above) continue;
        int ox, oy, ow, oh;
        if (win_has_state(d, l[i], "_NET_WM_STATE_HIDDEN") || !win_rect(d, l[i], &ox, &oy, &ow, &oh)) continue;
        if (ox < x + ww && ox + ow > x && oy < y + hh && oy + oh > y) { hit = true; win_title(d, l[i], name, nn); }
    }
    bool known = l != NULL && above;
    if (l) XFree(l);
    (void)known;
    return hit;
}

/* ======================= Звук трансляции (как в Windows-версии) =======================
 * Окно — звук процесса окна и его дочерних; весь экран — весь звук системы, кроме нашего процесса (голоса звонка
 * в эфир не попадают, эха нет). PulseAudio или PipeWire (pipewire-pulse): libpulse подгружается через dlopen на
 * время эфира, в зависимости сборки не входит. Для каждого подходящего потока воспроизведения (sink input)
 * открывается запись «монитора этого потока» (pa_stream_set_monitor_stream), потоки смешиваются.
 * Opus 48 кГц стерео, 20 мс, 128 кбит/с -> voice_stream_audio. Буферы затираются. */
static volatile int sa_stop;
static pthread_t sa_thr;
static bool sa_on;
static long sa_pid;                              /* процесс окна-источника; 0 — весь экран */
static void sa_status(bool on, const char *err) {
    pthread_mutex_lock(&sh_mu);
    sh.audio = on;
    snprintf(sh.audio_err, sizeof sh.audio_err, "%s", err);
    pthread_mutex_unlock(&sh_mu);
}
#if __has_include(<pulse/pulseaudio.h>)
#define SA_MAX 8
#define SA_FRAME 1920                            /* 20 мс: 960 отсчётов x 2 канала */
#define SA_RING (SA_FRAME * 25)                  /* 0,5 с */
typedef struct {
    pa_stream *s;
    uint32_t idx;
    bool used, seen;
    int16_t ring[SA_RING];
    size_t r, n;                                 /* начало и число отсчётов в кольце */
} SaSt;
static struct {
    void *lib;
    pa_threaded_mainloop *(*ml_new)(void);
    void (*ml_free)(pa_threaded_mainloop *);
    int (*ml_start)(pa_threaded_mainloop *);
    void (*ml_stop)(pa_threaded_mainloop *);
    void (*ml_lock)(pa_threaded_mainloop *);
    void (*ml_unlock)(pa_threaded_mainloop *);
    pa_mainloop_api *(*ml_api)(pa_threaded_mainloop *);
    pa_context *(*ctx_new)(pa_mainloop_api *, const char *);
    void (*ctx_unref)(pa_context *);
    int (*ctx_connect)(pa_context *, const char *, pa_context_flags_t, const pa_spawn_api *);
    void (*ctx_disconnect)(pa_context *);
    pa_context_state_t (*ctx_state)(const pa_context *);
    pa_operation *(*inputs)(pa_context *, pa_sink_input_info_cb_t, void *);
    pa_operation *(*sinks)(pa_context *, pa_sink_info_cb_t, void *);
    void (*op_unref)(pa_operation *);
    pa_stream *(*st_new)(pa_context *, const char *, const pa_sample_spec *, const pa_channel_map *);
    void (*st_unref)(pa_stream *);
    int (*st_record)(pa_stream *, const char *, const pa_buffer_attr *, pa_stream_flags_t);
    int (*st_disconnect)(pa_stream *);
    int (*st_monitor)(pa_stream *, uint32_t);
    void (*st_read_cb)(pa_stream *, pa_stream_request_cb_t, void *);
    int (*st_peek)(pa_stream *, const void **, size_t *);
    int (*st_drop)(pa_stream *);
    pa_stream_state_t (*st_state)(const pa_stream *);
    const char *(*pl_gets)(const pa_proplist *, const char *);
    pa_threaded_mainloop *ml;
    pa_context *ctx;
    pthread_mutex_t mu;                          /* кольца и результаты опроса */
    SaSt st[SA_MAX];
    struct { uint32_t idx, sink; long pid; } in[32];
    int nin;
    struct { uint32_t idx; char mon[192]; } sk[16];
    int nsk;
    bool eol_in, eol_sk;
} SA = { .mu = PTHREAD_MUTEX_INITIALIZER };

static bool sa_load(void) {
    SA.lib = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!SA.lib) return false;
    bool ok = true;
#define SY(f, name) do { *(void **)(&SA.f) = dlsym(SA.lib, name); if (!SA.f) ok = false; } while (0)
    SY(ml_new, "pa_threaded_mainloop_new"); SY(ml_free, "pa_threaded_mainloop_free"); SY(ml_start, "pa_threaded_mainloop_start");
    SY(ml_stop, "pa_threaded_mainloop_stop"); SY(ml_lock, "pa_threaded_mainloop_lock"); SY(ml_unlock, "pa_threaded_mainloop_unlock");
    SY(ml_api, "pa_threaded_mainloop_get_api"); SY(ctx_new, "pa_context_new"); SY(ctx_unref, "pa_context_unref");
    SY(ctx_connect, "pa_context_connect"); SY(ctx_disconnect, "pa_context_disconnect"); SY(ctx_state, "pa_context_get_state");
    SY(inputs, "pa_context_get_sink_input_info_list"); SY(sinks, "pa_context_get_sink_info_list"); SY(op_unref, "pa_operation_unref");
    SY(st_new, "pa_stream_new"); SY(st_unref, "pa_stream_unref"); SY(st_record, "pa_stream_connect_record");
    SY(st_disconnect, "pa_stream_disconnect"); SY(st_monitor, "pa_stream_set_monitor_stream"); SY(st_read_cb, "pa_stream_set_read_callback");
    SY(st_peek, "pa_stream_peek"); SY(st_drop, "pa_stream_drop"); SY(st_state, "pa_stream_get_state"); SY(pl_gets, "pa_proplist_gets");
#undef SY
    return ok;
}
/* pid — сам процесс root или его потомок (до 8 уровней): звук браузера и игр идёт из дочерних процессов. */
static bool sa_under(long pid, long root) {
    for (int lvl = 0; lvl < 8 && pid > 1; lvl++) {
        if (pid == root) return true;
        char p[64], b[512];
        snprintf(p, sizeof p, "/proc/%ld/stat", pid);
        FILE *f = fopen(p, "r");
        if (!f) return false;
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        b[n] = 0;
        const char *q = strrchr(b, ')');         /* имя процесса может содержать пробелы и скобки */
        long pp = 0;
        if (!q || sscanf(q + 1, " %*c %ld", &pp) != 1) return false;
        pid = pp;
    }
    return false;
}
static bool sa_wanted(long pid) {
    if (sa_pid < 0) return false;
    if (pid <= 0) return sa_pid == 0;            /* процесс неизвестен: только для «всего экрана» */
    if (sa_pid) return sa_under(pid, sa_pid);
    return pid != (long)getpid();
}
static void sa_in_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *u) {
    (void)c; (void)u;
    pthread_mutex_lock(&SA.mu);
    if (eol || !i) SA.eol_in = true;
    else if (SA.nin < 32) {
        const char *ps = i->proplist ? SA.pl_gets(i->proplist, "application.process.id") : NULL;
        SA.in[SA.nin].idx = i->index; SA.in[SA.nin].sink = i->sink; SA.in[SA.nin].pid = ps ? atol(ps) : 0;
        SA.nin++;
    }
    pthread_mutex_unlock(&SA.mu);
}
static void sa_sk_cb(pa_context *c, const pa_sink_info *i, int eol, void *u) {
    (void)c; (void)u;
    pthread_mutex_lock(&SA.mu);
    if (eol || !i) SA.eol_sk = true;
    else if (SA.nsk < 16 && i->monitor_source_name) {
        SA.sk[SA.nsk].idx = i->index;
        snprintf(SA.sk[SA.nsk].mon, sizeof SA.sk[SA.nsk].mon, "%s", i->monitor_source_name);
        SA.nsk++;
    }
    pthread_mutex_unlock(&SA.mu);
}
static void sa_read(pa_stream *s, size_t nbytes, void *u) {
    (void)nbytes;
    SaSt *t = u;
    const void *data = NULL;
    size_t n = 0;
    while (SA.st_peek(s, &data, &n) == 0 && n) {
        if (data) {
            pthread_mutex_lock(&SA.mu);
            const int16_t *p = data;
            for (size_t k = 0; k < n / 2; k++) {
                if (t->n == SA_RING) { t->r = (t->r + 1) % SA_RING; t->n--; }   /* переполнение: теряем самое старое */
                t->ring[(t->r + t->n) % SA_RING] = p[k];
                t->n++;
            }
            pthread_mutex_unlock(&SA.mu);
        }
        SA.st_drop(s);
    }
}
/* Под замком цикла PulseAudio: сверить список потоков воспроизведения с открытыми записями. */
static void sa_sync(void) {
    static const pa_sample_spec ss = { PA_SAMPLE_S16LE, 48000, 2 };
    pthread_mutex_lock(&SA.mu);
    for (int k = 0; k < SA_MAX; k++) SA.st[k].seen = false;
    for (int i = 0; i < SA.nin; i++) {
        if (!sa_wanted(SA.in[i].pid)) continue;
        int slot = -1, freeslot = -1;
        for (int k = 0; k < SA_MAX; k++) {
            if (SA.st[k].used && SA.st[k].idx == SA.in[i].idx) slot = k;
            if (!SA.st[k].used && freeslot < 0) freeslot = k;
        }
        if (slot >= 0) { SA.st[slot].seen = true; continue; }
        const char *mon = NULL;
        for (int j = 0; j < SA.nsk; j++) if (SA.sk[j].idx == SA.in[i].sink) mon = SA.sk[j].mon;
        if (freeslot < 0 || !mon) continue;
        SaSt *t = &SA.st[freeslot];
        t->r = t->n = 0;
        pa_stream *s = SA.st_new(SA.ctx, "astra-share", &ss, NULL);
        if (!s) continue;
        pa_buffer_attr at = { (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, SA_FRAME * 2 };
        SA.st_monitor(s, SA.in[i].idx);
        SA.st_read_cb(s, sa_read, t);
        if (SA.st_record(s, mon, &at, (pa_stream_flags_t)(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE)) < 0) { SA.st_unref(s); continue; }
        t->s = s; t->idx = SA.in[i].idx; t->used = t->seen = true;
    }
    for (int k = 0; k < SA_MAX; k++) {           /* поток воспроизведения исчез или запись оборвалась — закрыть */
        SaSt *t = &SA.st[k];
        if (!t->used) continue;
        pa_stream_state_t st = SA.st_state(t->s);
        if (t->seen && (st == PA_STREAM_READY || st == PA_STREAM_CREATING)) continue;
        pa_stream *s = t->s;
        t->used = false; t->s = NULL;
        pthread_mutex_unlock(&SA.mu);            /* обратный вызов чтения берёт тот же замок */
        SA.st_read_cb(s, NULL, NULL);
        SA.st_disconnect(s);
        SA.st_unref(s);
        pthread_mutex_lock(&SA.mu);
        wipe_mem(t->ring, sizeof t->ring);
        t->r = t->n = 0;
    }
    pthread_mutex_unlock(&SA.mu);
}
static void *sa_main(void *arg) {
    (void)arg;
    OpusEncoder *enc = NULL;
    bool started = false;
    if (!sa_load()) { sa_status(false, "нет PulseAudio/PipeWire (libpulse)"); goto done; }
    int oe = 0;
    enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &oe);
    if (!enc || oe) { sa_status(false, "кодер звука не запустился"); goto done; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
    SA.ml = SA.ml_new();
    if (!SA.ml) { sa_status(false, "звук: нет цикла PulseAudio"); goto done; }
    SA.ctx = SA.ctx_new(SA.ml_api(SA.ml), "Astra DS");
    if (!SA.ctx || SA.ctx_connect(SA.ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0 || SA.ml_start(SA.ml) < 0) {
        sa_status(false, "звуковой сервер недоступен");
        goto done;
    }
    started = true;
    for (int i = 0; i < 300 && !sa_stop; i++) {  /* до 3 с на соединение */
        SA.ml_lock(SA.ml);
        pa_context_state_t cs = SA.ctx_state(SA.ctx);
        SA.ml_unlock(SA.ml);
        if (cs == PA_CONTEXT_READY) break;
        if (cs == PA_CONTEXT_FAILED || cs == PA_CONTEXT_TERMINATED || i == 299) { sa_status(false, "звуковой сервер недоступен"); goto done; }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    sa_status(true, "");
    {
        double next = now_msf();
        int tick = 0;
        bool asked = false;
        u32 ts48 = 0;
        static int32_t mix[SA_FRAME];
        static int16_t pcm[SA_FRAME];
        static u8 pkt[1500];
        while (!sa_stop) {
            if (tick % 50 == 0) {                /* раз в секунду: какие программы сейчас играют звук */
                pthread_mutex_lock(&SA.mu);
                SA.nin = SA.nsk = 0; SA.eol_in = SA.eol_sk = false;
                pthread_mutex_unlock(&SA.mu);
                SA.ml_lock(SA.ml);
                pa_operation *o1 = SA.sinks(SA.ctx, sa_sk_cb, NULL), *o2 = SA.inputs(SA.ctx, sa_in_cb, NULL);
                if (o1) SA.op_unref(o1);
                if (o2) SA.op_unref(o2);
                SA.ml_unlock(SA.ml);
                asked = true;
            }
            if (asked) {
                pthread_mutex_lock(&SA.mu);
                bool ready = SA.eol_in && SA.eol_sk;
                pthread_mutex_unlock(&SA.mu);
                if (ready) { SA.ml_lock(SA.ml); sa_sync(); SA.ml_unlock(SA.ml); asked = false; }
            }
            bool any = false;
            memset(mix, 0, sizeof mix);
            pthread_mutex_lock(&SA.mu);
            for (int k = 0; k < SA_MAX; k++) {
                SaSt *t = &SA.st[k];
                if (!t->used || !t->n) continue;
                size_t take = t->n < SA_FRAME ? t->n & ~(size_t)1 : SA_FRAME;
                for (size_t q = 0; q < take; q++) mix[q] += t->ring[(t->r + q) % SA_RING];
                t->r = (t->r + take) % SA_RING; t->n -= take;
                if (t->n > SA_FRAME * 4) { size_t d = t->n - SA_FRAME * 2; t->r = (t->r + d) % SA_RING; t->n -= d; }   /* отставание: догнать */
                if (take) any = true;
            }
            pthread_mutex_unlock(&SA.mu);
            if (any) {
                for (int q = 0; q < SA_FRAME; q++) pcm[q] = (int16_t)(mix[q] > 32767 ? 32767 : mix[q] < -32768 ? -32768 : mix[q]);
                int n = opus_encode(enc, pcm, SA_FRAME / 2, pkt, (opus_int32)sizeof pkt);
                if (n > 2) voice_stream_audio(pkt, (size_t)n, ts48);
            }
            ts48 += SA_FRAME / 2;
            tick++;
            next += 20.0;
            double now = now_msf();
            if (next > now) { struct timespec ts = { 0, (long)((next - now) * 1e6) }; nanosleep(&ts, NULL); }
            else if (now - next > 500.0) next = now;
        }
        wipe_mem(mix, sizeof mix); wipe_mem(pcm, sizeof pcm); wipe_mem(pkt, sizeof pkt);
    }
done:
    if (started) {
        SA.ml_lock(SA.ml);
        for (int k = 0; k < SA_MAX; k++) if (SA.st[k].used) { SA.st_read_cb(SA.st[k].s, NULL, NULL); SA.st_disconnect(SA.st[k].s); SA.st_unref(SA.st[k].s); SA.st[k].used = false; SA.st[k].s = NULL; }
        SA.ctx_disconnect(SA.ctx);
        SA.ml_unlock(SA.ml);
        SA.ml_stop(SA.ml);
    }
    if (SA.ctx) { SA.ctx_unref(SA.ctx); SA.ctx = NULL; }
    if (SA.ml) { SA.ml_free(SA.ml); SA.ml = NULL; }
    for (int k = 0; k < SA_MAX; k++) { wipe_mem(SA.st[k].ring, sizeof SA.st[k].ring); SA.st[k].r = SA.st[k].n = 0; }
    if (enc) opus_encoder_destroy(enc);
    if (SA.lib) { dlclose(SA.lib); SA.lib = NULL; }
    pthread_mutex_lock(&sh_mu); sh.audio = false; pthread_mutex_unlock(&sh_mu);
    return NULL;
}
#else
static void *sa_main(void *arg) { (void)arg; sa_status(false, "собрано без libpulse: без звука"); return NULL; }   /* <= 63 байт (audio_err) */
#endif

static void *sh_main(void *arg) {
    (void)arg;
    Cap c;
    memset(&c, 0, sizeof c);
    sa_on = false;
    const char *why = "трансляция остановлена";
    H264E_persist_t *enc = NULL;
    H264E_scratch_t *scr = NULL;
    int szp = 0, szs = 0;
    size_t ysz = (size_t)SH_OW * SH_OH, csz = ysz / 4;
    const bool wl = sh_wl;
    bool wl_pw = false;
    int wl_src = 0;                              /* тип источника от портала: 1 — монитор, 2 — окно */
    u32 wl_full = 0;                             /* номер кадра PipeWire, уже переведённого целиком */
    if (wl) {
        /* системное окно выбора: ждём здесь, в потоке трансляции; share_stop (sh_stop) прерывает ожидание */
        PortalStream ps;
        char e[96] = "";
        memset(&ps, 0, sizeof ps);
        if (!portal_wl_open(&ps, &sh_stop, e, sizeof e)) {
            snprintf(sh_wl_err, sizeof sh_wl_err, "%s", !sh_stop && e[0] ? e : "трансляция остановлена");
            why = sh_wl_err;
            goto out;
        }
        wl_src = ps.source_type;
        if (sh_stop) { if (ps.pw_fd >= 0) close(ps.pw_fd); goto out; }
        if (!shpw_open(ps.pw_fd, ps.node, e, sizeof e)) { snprintf(sh_wl_err, sizeof sh_wl_err, "%s", e); why = sh_wl_err; goto out; }
        wl_pw = true;
        pthread_mutex_lock(&sh_mu); sh.choosing = false; pthread_mutex_unlock(&sh_mu);
    } else {
    c.d = XOpenDisplay(NULL);
    if (!c.d) { sh_finish("нет графического сеанса X11"); sh_done = 1; return NULL; }
    XSetErrorHandler(sh_xerr);
    c.root = DefaultRootWindow(c.d);
    c.scr_w = DisplayWidth(c.d, DefaultScreen(c.d)); c.scr_h = DisplayHeight(c.d, DefaultScreen(c.d));
    {   /* формат точки: только 32 бита на точку, младший байт — синий (обычный TrueColor 24/32) */
        XImage *probe = XGetImage(c.d, c.root, 0, 0, 1, 1, AllPlanes, ZPixmap);
        bool okf = probe && probe->bits_per_pixel == 32 && probe->byte_order == LSBFirst && probe->red_mask == 0xFF0000;
        if (probe) XDestroyImage(probe);
        if (!okf || c.scr_w < 16 || c.scr_h < 16) { why = "формат экрана не поддерживается"; goto out; }
    }
    c.xext = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
    if (c.xext) {
        *(void **)(&c.q) = dlsym(c.xext, "XShmQueryExtension");
        *(void **)(&c.create) = dlsym(c.xext, "XShmCreateImage");
        *(void **)(&c.attach) = dlsym(c.xext, "XShmAttach");
        *(void **)(&c.detach) = dlsym(c.xext, "XShmDetach");
        *(void **)(&c.get) = dlsym(c.xext, "XShmGetImage");
        c.shm = c.q && c.create && c.attach && c.detach && c.get && c.q(c.d);
    }
    }
    int fps = SH_FPS, kbps = sh_mode ? 1000 : 1800, kbps_max = sh_mode ? 2000 : 3500;
    c.xmap = malloc(sizeof(int) * SH_OW);
    c.y = malloc(ysz); c.u = malloc(csz); c.v = malloc(csz);
    c.mb.mbw = SH_OW / 16; c.mb.mbh = SH_OH / 16;
    c.mb.sig = calloc((size_t)c.mb.mbw * c.mb.mbh, sizeof(u32));
    c.mb.age = calloc((size_t)c.mb.mbw * c.mb.mbh, 1); c.mb.map = calloc((size_t)c.mb.mbw * c.mb.mbh, 1);
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = SH_OW; cp.height = SH_OH; cp.gop = fps * 8;   /* ключевые — по таймеру SH_GOP_MS и PLI */ cp.vbv_size_bytes = kbps_max * 1000 / 8;
    bool ok = c.xmap && c.y && c.u && c.v && c.mb.sig && c.mb.age && c.mb.map && !H264E_sizeof(&cp, &szp, &szs);
    if (ok) {
        memset(c.y, 16, ysz); memset(c.u, 128, csz); memset(c.v, 128, csz);
        enc = calloc(1, (size_t)szp);
        scr = malloc((size_t)szs);
        ok = enc && scr && !H264E_init(enc, &cp);
    }
    if (!ok) { why = "не удалось запустить кодер"; goto out; }
    pthread_mutex_lock(&sh_mu);
    sh.w = SH_OW; sh.h = SH_OH; sh.fps = fps;
    sh.audio = false;
    /* Wayland, окно: портал не говорит, чьё оно, — звук не берём (чужой звук в эфир не пускаем) */
    bool wl_win_mute = wl && wl_src != 1 && sa_want;
    snprintf(sh.audio_err, sizeof sh.audio_err, "%s", wl_win_mute ? "Wayland: звук окна недоступен" : sa_want ? "" : "без звука");
    pthread_mutex_unlock(&sh_mu);
    sa_stop = 0;
    sa_pid = wl ? 0 : sh_win ? prop_long(c.d, sh_win, "_NET_WM_PID", -1) : 0;   /* -1: процесс окна неизвестен — без звука, чужой не берём */
    if (sa_want && !wl_win_mute) sa_on = !pthread_create(&sa_thr, NULL, sa_main, NULL);

    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    /* 10: без четвертьпиксельного поиска — кодер на 11-24 % дешевле, PSNR -0,0..-0,13 дБ (замер 10.11, как в Windows) */
    rp.encode_speed = 10; rp.qp_min = 18; rp.qp_max = 51;
    H264E_io_yuv_t io;
    io.yuv[0] = c.y; io.yuv[1] = c.u; io.yuv[2] = c.v;
    io.stride[0] = SH_OW; io.stride[1] = SH_OW / 2; io.stride[2] = SH_OW / 2;
    double per = kbps * 1000.0 / 8 / fps, cap = (kbps_max - kbps) * 1000.0 / 8, bucket = 0;
    double t0 = now_msf(), next = t0, win0 = t0, last_idr = -1e9, last_out = -1e9, tick_ms = 1000.0 / fps, last_chk = -1e9;
    long long bytes = 0, win_bytes = 0;
    int sent = 0, frames = 0, win_sent = 0, enc_since_key = 0;
    bool paused = false, was_mini = false, have_clean = false;
    /* темп и захват (share_conv.h): 30 к/с при движении, 20 при малом, 10 Гц при покое; целиком не реже 100 мс,
     * между — только область изменений; блоки без изменений кодер не ищет, кадр без изменений — одни пропуски */
    ShPace pace;
    memset(&pace, 0, sizeof pace);
    pace.force_full = true; pace.last_change = t0; pace.last_full = -1e9; pace.hi_until = t0 + 1000;
    while (!sh_stop) {
        bool blank = false, mini = false;
        const char *pwhy = "";
        char cover[64] = "";
        int ch = 0;
        double tq = now_msf();
        bool key_due = sh_want_idr || tq - last_idr >= SH_GOP_MS - 2 * tick_ms || enc_since_key >= cp.gop - 1;
        int kind = shc_grab_kind(&pace, &c.mb, tq, key_due);   /* 2 целиком, 1 область изменений, 0 не снимать */
        int gx0 = 0, gy0 = 0, gx1 = c.mb.mbw, gy1 = c.mb.mbh;
        if (kind == 1) {                         /* область изменений прошлого захвата и 2 блока вокруг */
            gx0 = c.mb.cx0 - 2 < 0 ? 0 : c.mb.cx0 - 2; gy0 = c.mb.cy0 - 2 < 0 ? 0 : c.mb.cy0 - 2;
            gx1 = c.mb.cx1 + 2 > c.mb.mbw ? c.mb.mbw : c.mb.cx1 + 2; gy1 = c.mb.cy1 + 2 > c.mb.mbh ? c.mb.mbh : c.mb.cy1 + 2;
        }
        if (wl) {                                /* Wayland: кадр из буфера PipeWire, под замком его цикла */
            if (tq - last_chk >= 1000) { last_chk = tq; if (portal_wl_closed()) { why = "источник трансляции закрыт"; break; } }
            ShPwFrame pf;
            u32 seq = 0;
            int st = shpw_lock(&pf, &seq);
            if (st < 0) { shpw_unlock(); why = st == -3 ? "компоновщик даёт только буферы видеокарты" : st == -2 ? "формат картинки не поддерживается" : "источник трансляции закрыт"; break; }
            if (st == 0) { blank = true; pwhy = "ждём картинку"; }
            else if (pf.w < 16 || pf.h < 16) { blank = true; pwhy = "окно слишком мало"; }
            else if (kind) {
                c.sw = pf.w; c.sh = pf.h;
                cap_fit(&c);
                if (c.refit || seq != wl_full) {
                    bool full = c.refit || kind == 2;
                    ch = cap_convert(&c, pf.data, pf.stride, pf.h, pf.rgb, pf.uncached, gx0, gy0, gx1, gy1);
                    if (full) wl_full = seq;
                } else shc_scan_begin(&c.mb);    /* этот кадр уже переведён целиком: нового нет, ничего не менялось */
                have_clean = true;
            }
            shpw_unlock();
        } else if (sh_win) {
            int x, y, w, h;
            if (tq - last_chk >= 1000) { last_chk = tq; if (!win_exists(c.d, sh_win)) { why = "окно источника закрыто"; break; } }
            if (win_has_state(c.d, sh_win, "_NET_WM_STATE_HIDDEN") || !win_rect(c.d, sh_win, &x, &y, &w, &h)) {
                blank = mini = true; pwhy = "окно свёрнуто";
            } else if (x < 0 || y < 0 || x + w > c.scr_w || y + h > c.scr_h) {
                blank = true; pwhy = "окно за краем экрана";
            } else if (w < 16 || h < 16) {
                blank = true; pwhy = "окно слишком мало";
            } else if (!kind) {
                /* ничего не менялось и захват целиком ещё не нужен — не снимаем, ниже повтор кадра */
            } else if (win_covered(c.d, sh_win, x, y, w, h, cover, sizeof cover)) {
                blank = true; pwhy = "перекрыто";
            } else {
                c.sw = w; c.sh = h;
                cap_fit(&c);
                if (!cap_grab(&c, x, y, w, h)) { blank = true; pwhy = "кадр не получен"; }
                else {
                    /* после захвата: перекрыли или сдвинули — в кадре могло оказаться чужое, кадр не отправляем */
                    int x2, y2, w2, h2;
                    char cv2[64] = "";
                    if (!win_rect(c.d, sh_win, &x2, &y2, &w2, &h2) || x2 != x || y2 != y || w2 != w || h2 != h) {
                        blank = true; pwhy = "окно сдвинули во время захвата"; have_clean = false;
                        shc_reset(&c.mb);
                    } else if (win_covered(c.d, sh_win, x, y, w, h, cv2, sizeof cv2)) {
                        blank = true; pwhy = "перекрыто во время захвата"; snprintf(cover, sizeof cover, "%s", cv2);
                    } else { ch = cap_convert(&c, (const u8 *)c.img->data, (size_t)c.img->bytes_per_line, c.img->height, 0, 0, gx0, gy0, gx1, gy1); have_clean = true; }
                }
            }
        } else if (kind) {                       /* весь экран */
            c.sw = c.scr_w; c.sh = c.scr_h;
            cap_fit(&c);
            if (!cap_grab(&c, 0, 0, c.scr_w, c.scr_h)) { why = "захват экрана не удался"; break; }
            ch = cap_convert(&c, (const u8 *)c.img->data, (size_t)c.img->bytes_per_line, c.img->height, 0, 0, gx0, gy0, gx1, gy1);
            have_clean = true;
        }
        if (blank) pace.force_full = true;       /* после паузы — только целиком */
        else if (kind == 2) { pace.force_full = false; pace.last_full = tq; }
        if (mini != was_mini) {
            was_mini = mini;
            pthread_mutex_lock(&sh_mu); sh.minimized = mini; pthread_mutex_unlock(&sh_mu);
            if (!mini) sh_want_idr = 1;
        }
        if (blank != paused) {
            paused = blank;
            pthread_mutex_lock(&sh_mu);
            sh.paused = blank;
            snprintf(sh.pause_why, sizeof sh.pause_why, "%s", pwhy);
            snprintf(sh.cover, sizeof sh.cover, "%s", cover);
            pthread_mutex_unlock(&sh_mu);
            if (!blank) sh_want_idr = 1;         /* вернулись — сразу ключевой */
        }
        if (blank) ch = 0;
        double fr = tick_ms * fps / 1000.0;      /* доля «кадра 30 к/с» в этом такте: бюджет по времени */
        bucket += per * fr;
        if (bucket > cap) bucket = cap;
        double tn = now_msf();
        bool want_key = false;
        if (have_clean) {
            if (sh_want_idr) { sh_want_idr = 0; want_key = true; }
            if (tn - last_idr >= SH_GOP_MS) want_key = true;
        }
        /* пауза: поток не прерывается — повтор последнего чистого кадра не реже 10 к/с; чистого ещё не было — молчим */
        bool keepalive = have_clean && tn - last_out >= 100;
        if (have_clean && (ch || want_key || keepalive)) {
            double debt = bucket < 0 ? -bucket / per : 0;    /* долг битрейта — хуже качество, а не выброс кадра */
            rp.qp_min = 18 + (debt > 33 ? 33 : (int)debt);
            rp.desired_frame_bytes = (int)(per * fr);
            rp.frame_type = want_key ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
            /* блоки без изменений — без поиска; кадр, где всё давно стоит, — одни пропуски (почти даром) */
            rp.skip_frame = !want_key && shc_build_map(&c.mb);
            rp.mb_static = want_key ? NULL : c.mb.map;
            if (want_key) last_idr = tn;
            last_out = tn;
            unsigned char *out = NULL;
            int n = 0;
            if (!H264E_encode(enc, scr, &rp, &io, &out, &n) && n > 0) {
                bytes += n; win_bytes += n; bucket -= n;
                bool key = false;
                for (int i = 0; i + 3 < n && !key; i++)
                    if (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 1 && (out[i + 3] & 31) == 5) key = true;
                shc_encoded(&c.mb, key);
                enc_since_key = key ? 0 : enc_since_key + 1;
                golive_frame(out, (size_t)n, key, (u32)((now_msf() - t0) * 90.0));
            }
            sent++; win_sent++;
        }
        frames++;
        double now = now_msf();
        if (now - win0 >= 1000) {
            pthread_mutex_lock(&sh_mu);
            sh.fps_out = win_sent * 1000.0 / (now - win0);
            sh.mbps = (double)win_bytes * 8.0 / ((now - win0) / 1000.0) / 1e6;
            sh.frames = frames; sh.sent = sent; sh.dropped = 0; sh.bytes = bytes;
            pthread_mutex_unlock(&sh_mu);
            win0 = now; win_sent = 0; win_bytes = 0;
        }
        int pmb = ((c.dx + c.dw + 15) / 16 - c.dx / 16) * ((c.dy + c.dh + 15) / 16 - c.dy / 16);
        tick_ms = shc_tick(&pace, tq, ch, pmb > 0 ? pmb : 1, blank);
        next += tick_ms;
        now = now_msf();
        if (next > now) {
            struct timespec ts = { (time_t)((next - now) / 1000.0), (long)(((next - now) - (double)(long)((next - now) / 1000.0) * 1000.0) * 1e6) };
            nanosleep(&ts, NULL);
        } else if (now - next > 1000.0) next = now;
    }
    if (sh_stop) why = "трансляция остановлена";
out:
    if (sa_on) { sa_stop = 1; pthread_join(sa_thr, NULL); sa_on = false; }   /* звук: до конца, буферы затирает сам */
    if (wl_pw) shpw_close();                     /* сначала поток PipeWire, потом сессия портала */
    if (wl) portal_wl_close();
    cap_img_free(&c);
    wipe_mem(c.y, ysz); wipe_mem(c.u, csz); wipe_mem(c.v, csz);
    if (enc) wipe_mem(enc, (size_t)szp);
    if (scr) wipe_mem(scr, (size_t)szs);         /* в scratch кодера — куски кадра */
    free(enc); free(scr);
    free(c.y); free(c.u); free(c.v); free(c.xmap); free(c.mb.sig); free(c.mb.age); free(c.mb.map);
    if (c.d) XCloseDisplay(c.d);
    if (c.xext) dlclose(c.xext);
    sh_finish(why);
    sh_done = 1;
    return NULL;
}

void share_request_idr(void) { sh_want_idr = 1; }
void share_audio_enable(bool on) { sa_want = on; }
void share_self_excluded(bool ok) { (void)ok; }
static void thr_join(void) { if (sh_thr_on) { pthread_join(sh_thr, NULL); sh_thr_on = false; } }
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n) {
    ShareState cur;
    share_state(&cur);
    if (cur.active) { snprintf(err, n, "трансляция уже идёт"); return false; }
    thr_join();
    /* Wayland (niri, GNOME, KDE...): окна других программ X11 не видит, корень Xwayland снимается чёрным —
     * источник выбирается в системном окне xdg-desktop-portal, кадры — PipeWire */
    bool wl = share_wayland();
    if (wl) {
        if (!portal_wl_available()) { snprintf(err, n, "для демонстрации экрана в Wayland нужны libdbus-1 и xdg-desktop-portal"); return false; }
        char e2[96] = "";
        if (!shpw_load(e2, sizeof e2)) { snprintf(err, n, "%s", e2); return false; }
        hwnd = 0;
    } else if (!getenv("DISPLAY")) { snprintf(err, n, "демонстрация экрана на Linux работает только в сеансе X11"); return false; }
    if (hwnd) {                                  /* то же окно, что было в списке: тот же процесс и заголовок */
        int k = -1;
        for (int i = 0; i < sh_nknown; i++) if (sh_known[i].w == (Window)hwnd) k = i;
        if (k < 0) { snprintf(err, n, "этого окна уже нет"); return false; }
        Display *d = XOpenDisplay(NULL);
        if (!d) { snprintf(err, n, "нет графического сеанса X11"); return false; }
        XErrorHandler old = XSetErrorHandler(sh_xerr);
        char t[80];
        win_title(d, (Window)hwnd, t, sizeof t);
        bool same = win_exists(d, (Window)hwnd) && prop_long(d, (Window)hwnd, "_NET_WM_PID", -1) == sh_known[k].pid && !strcmp(t, sh_known[k].t);
        XSync(d, False);
        XSetErrorHandler(old);
        XCloseDisplay(d);
        if (!same) { snprintf(err, n, "окно изменилось - выберите источник заново"); return false; }
    }
    pthread_mutex_lock(&sh_mu);
    memset(&sh, 0, sizeof sh);
    sh.active = true;
    sh.choosing = wl;                            /* до ответа системного окна эфир не начинается (ui.c ждёт) */
    pthread_mutex_unlock(&sh_mu);
    sh_wl = wl;
    sh_win = (Window)hwnd; sh_mode = mode ? 1 : 0;
    sh_stop = 0; sh_done = 0; sh_want_idr = 1;
    if (pthread_create(&sh_thr, NULL, sh_main, NULL)) { sh_finish("поток не создан"); snprintf(err, n, "не удалось запустить захват"); return false; }
    sh_thr_on = true;
    return true;
}
void share_stop(void) { sh_stop = 1; thr_join(); }   /* до конца: буферы затёрты */
void share_stop_nowait(void) {                   /* паника: флаг + не дольше 150 мс; не вышел — ничего не освобождаем */
    sh_stop = 1;
    for (int i = 0; i < 15 && sh_thr_on && !sh_done; i++) { struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL); }
    if (sh_thr_on && sh_done) thr_join();
}
void share_state(ShareState *out) {
    pthread_mutex_lock(&sh_mu);
    *out = sh;
    pthread_mutex_unlock(&sh_mu);
}
#endif
