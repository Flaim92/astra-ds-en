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
 * Звук трансляции на Linux пока не передаётся. Буферы с содержимым экрана затираются перед освобождением. */
#if defined(ASTRA_VIDEO) && !defined(_WIN32)
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
#define SH_GOP_MS 4000

static pthread_mutex_t sh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t sh_thr;
static bool sh_thr_on;
static volatile int sh_stop, sh_want_idr, sh_done;
static ShareState sh;
static Window sh_win;
static int sh_mode;
static bool sa_want = true;
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

int share_windows(char (*titles)[80], uintptr_t *ids, int max) {
    int cnt = 0;
    memset(sh_known, 0, sizeof sh_known);
    sh_nknown = 0;
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
    u8 *y, *u, *v;
    u32 *mbsig;
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
    memset(c->mbsig, 0, sizeof(u32) * (size_t)(SH_OW / 16) * (SH_OH / 16));
    for (int x = 0; x < c->dw; x++) c->xmap[x] = x * c->sw / c->dw;
    c->fit_w = c->sw; c->fit_h = c->sh;
}
/* c->img (BGRX, 32 бита) -> YUV420 BT.601 в место dx,dy,dw,dh; вернуть число изменившихся блоков 16x16. */
static int cap_convert(Cap *c) {
    const XImage *im = c->img;
    for (int i = 0; i < c->dh; i++) {
        int syy = i * c->sh / c->dh;
        if (syy >= im->height) syy = im->height - 1;
        const u8 *row = (const u8 *)im->data + (size_t)syy * (size_t)im->bytes_per_line;
        int oy = c->dy + i;
        u8 *yr = c->y + (size_t)oy * SH_OW + c->dx;
        bool crow = !(oy & 1);
        u8 *ur = c->u + (size_t)(oy / 2) * (SH_OW / 2) + c->dx / 2, *vr = c->v + (size_t)(oy / 2) * (SH_OW / 2) + c->dx / 2;
        for (int k = 0; k < c->dw; k++) {
            u32 px;
            memcpy(&px, row + (size_t)c->xmap[k] * 4, 4);
            int R = (int)(px >> 16 & 255), G = (int)(px >> 8 & 255), B = (int)(px & 255);
            yr[k] = (u8)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
            if (crow && !(k & 1)) {
                ur[k / 2] = (u8)(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
                vr[k / 2] = (u8)(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
            }
        }
    }
    int changed = 0, mbw = SH_OW / 16;
    for (int s = 0; s < SH_OH / 16; s++)
        for (int mx = 0; mx < mbw; mx++) {
            u32 h = 2166136261u;
            for (int yy = 0; yy < 16; yy++) {
                const u8 *p = c->y + (size_t)(s * 16 + yy) * SH_OW + (size_t)mx * 16;
                for (int k = 0; k < 16; k += 4) { u32 w4; memcpy(&w4, p + k, 4); h = (h ^ w4) * 16777619u; }
            }
            u32 *sig = &c->mbsig[(size_t)s * mbw + mx];
            if (*sig != h) { changed++; *sig = h; }
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

static void *sh_main(void *arg) {
    (void)arg;
    Cap c;
    memset(&c, 0, sizeof c);
    const char *why = "трансляция остановлена";
    H264E_persist_t *enc = NULL;
    H264E_scratch_t *scr = NULL;
    int szp = 0, szs = 0;
    size_t ysz = (size_t)SH_OW * SH_OH, csz = ysz / 4;
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
    int fps = SH_FPS, kbps = sh_mode ? 1000 : 1800, kbps_max = sh_mode ? 2000 : 3500;
    c.xmap = malloc(sizeof(int) * SH_OW);
    c.y = malloc(ysz); c.u = malloc(csz); c.v = malloc(csz);
    c.mbsig = calloc((size_t)(SH_OW / 16) * (SH_OH / 16), sizeof(u32));
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = SH_OW; cp.height = SH_OH; cp.gop = fps * 2; cp.vbv_size_bytes = kbps_max * 1000 / 8;
    bool ok = c.xmap && c.y && c.u && c.v && c.mbsig && !H264E_sizeof(&cp, &szp, &szs);
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
    snprintf(sh.audio_err, sizeof sh.audio_err, "%s", sa_want ? "звук на Linux пока не передаётся" : "без звука");
    pthread_mutex_unlock(&sh_mu);

    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    rp.encode_speed = 8; rp.qp_min = 18; rp.qp_max = 51;
    H264E_io_yuv_t io;
    io.yuv[0] = c.y; io.yuv[1] = c.u; io.yuv[2] = c.v;
    io.stride[0] = SH_OW; io.stride[1] = SH_OW / 2; io.stride[2] = SH_OW / 2;
    double per = kbps * 1000.0 / 8 / fps, cap = (kbps_max - kbps) * 1000.0 / 8, bucket = 0;
    double t0 = now_msf(), next = t0, win0 = t0, last_idr = -1e9, last_out = -1e9;
    long long bytes = 0, win_bytes = 0;
    int sent = 0, frames = 0, win_sent = 0;
    bool paused = false, was_mini = false, have_clean = false;
    while (!sh_stop) {
        bool blank = false, mini = false;
        const char *pwhy = "";
        char cover[64] = "";
        int ch = 0;
        if (sh_win) {
            int x, y, w, h;
            if (frames % fps == 0 && !win_exists(c.d, sh_win)) { why = "окно источника закрыто"; break; }
            if (win_has_state(c.d, sh_win, "_NET_WM_STATE_HIDDEN") || !win_rect(c.d, sh_win, &x, &y, &w, &h)) {
                blank = mini = true; pwhy = "окно свёрнуто";
            } else if (x < 0 || y < 0 || x + w > c.scr_w || y + h > c.scr_h) {
                blank = true; pwhy = "окно за краем экрана";
            } else if (w < 16 || h < 16) {
                blank = true; pwhy = "окно слишком мало";
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
                        memset(c.mbsig, 0, sizeof(u32) * (size_t)(SH_OW / 16) * (SH_OH / 16));
                    } else if (win_covered(c.d, sh_win, x, y, w, h, cv2, sizeof cv2)) {
                        blank = true; pwhy = "перекрыто во время захвата"; snprintf(cover, sizeof cover, "%s", cv2);
                    } else { ch = cap_convert(&c); have_clean = true; }
                }
            }
        } else {                                 /* весь экран */
            c.sw = c.scr_w; c.sh = c.scr_h;
            cap_fit(&c);
            if (!cap_grab(&c, 0, 0, c.scr_w, c.scr_h)) { why = "захват экрана не удался"; break; }
            ch = cap_convert(&c);
            have_clean = true;
        }
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
        bucket += per;
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
            rp.desired_frame_bytes = (int)per;
            rp.frame_type = want_key ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
            if (want_key) last_idr = tn;
            last_out = tn;
            unsigned char *out = NULL;
            int n = 0;
            if (!H264E_encode(enc, scr, &rp, &io, &out, &n) && n > 0) {
                bytes += n; win_bytes += n; bucket -= n;
                bool key = false;
                for (int i = 0; i + 3 < n && !key; i++)
                    if (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 1 && (out[i + 3] & 31) == 5) key = true;
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
        next += 1000.0 / fps;
        now = now_msf();
        if (next > now) {
            struct timespec ts = { (time_t)((next - now) / 1000.0), (long)(((next - now) - (double)(long)((next - now) / 1000.0) * 1000.0) * 1e6) };
            nanosleep(&ts, NULL);
        } else if (now - next > 1000.0) next = now;
    }
    if (sh_stop) why = "трансляция остановлена";
out:
    cap_img_free(&c);
    wipe_mem(c.y, ysz); wipe_mem(c.u, csz); wipe_mem(c.v, csz);
    if (enc) wipe_mem(enc, (size_t)szp);
    if (scr) wipe_mem(scr, (size_t)szs);         /* в scratch кодера — куски кадра */
    free(enc); free(scr);
    free(c.y); free(c.u); free(c.v); free(c.xmap); free(c.mbsig);
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
    if (!getenv("DISPLAY")) { snprintf(err, n, "демонстрация экрана на Linux работает только в сеансе X11"); return false; }
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
    pthread_mutex_unlock(&sh_mu);
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
