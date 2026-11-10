/* share_pw.c — приём кадров PipeWire (демонстрация экрана в Wayland), только «Astra DS Video» на Linux. См. share_pw.h.
 *
 * libpipewire-0.3 — через dlopen (в зависимости сборки не входит, заголовки PipeWire/SPA не нужны): ниже объявлен
 * минимум устойчивого ABI 0.3 (spa_hook, spa_buffer/spa_data/spa_chunk/spa_meta, pw_buffer, pw_stream_events,
 * номера типов и ключей SPA). Параметры потока (SPA pod) собираются и разбираются вручную, с проверкой границ.
 *
 * Поток: pw_thread_loop (свой поток цикла) -> pw_context_connect_fd(fd портала) -> pw_stream на узел портала.
 * Предлагаемые форматы: BGRx, RGBx, BGRA, RGBA, любой размер, частота источника до 30 к/с (maxFramerate — меньше
 * работы и источнику), 2..4 буфера, метаданные VideoCrop (окно в GNOME отдаётся обрезкой кадра монитора).
 * Два варианта по порядку: (1) общая память MemPtr/MemFd без модификатора; (2) DMA-BUF только с модификатором
 * DRM_FORMAT_MOD_LINEAR (niri отдаёт запись только так; mutter/KWin тоже предлагают DMA-BUF). Линейный DMA-BUF
 * отображается в память (mmap только для чтения, один раз на буфер, снимается при remove_buffer) и читается под
 * DMA_BUF_IOCTL_SYNC: без EGL/GPU. Такое отображение некэшируемое (write-combined): обычное чтение по 4 байта —
 * ~280 мс на кадр 1920x1080 (замер на AMD Vega), поэтому каждая нужная строка сначала копируется потоковыми
 * загрузками SSE4.1 (MOVNTDQA, ~2 мс на кадр) в один кэшируемый буфер строки (shpw_row), конверсия — из него.
 * Плиточные (нелинейные) модификаторы не берём: для них нужен GPU — тогда понятная ошибка.
 * Обработка (process) идёт в потоке цикла под его замком: берём самый свежий буфер, проверяем размеры, stride и
 * границы, держим его (предыдущий — сразу источнику). Поток захвата берёт тот же замок и переводит картинку прямо из
 * этого буфера в YUV — без копии кадра. Пропавший буфер (remove_buffer, смена формата) забывается сразу. */
#if defined(ASTRA_VIDEO) && !defined(_WIN32)
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#if defined(__x86_64__) || defined(__i386__)
#include <smmintrin.h>
#define SHPW_X86 1
#else
#define SHPW_X86 0
#endif
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#endif
#ifndef DMA_BUF_IOCTL_SYNC                       /* linux/dma-buf.h (ядро 4.6+) */
struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)
#endif
#include "share_pw.h"
#include "share_conv.h"

/* ---------- минимум ABI PipeWire 0.3 / SPA 0.2 (проверено по заголовкам 0.3.65) ---------- */
typedef struct PwList { struct PwList *next, *prev; } PwList;
typedef struct { PwList link; struct { const void *funcs; void *data; } cb; void (*removed)(void *hook); void *priv; } PwHook;   /* spa_hook */
typedef struct { uint32_t offset, size; int32_t stride, flags; } PwChunk;                     /* spa_chunk */
typedef struct { uint32_t type, flags; int64_t fd; uint32_t mapoffset, maxsize; void *data; PwChunk *chunk; } PwData;   /* spa_data */
typedef struct { uint32_t type, size; void *data; } PwMeta;                                  /* spa_meta */
typedef struct { uint32_t n_metas, n_datas; PwMeta *metas; PwData *datas; } PwSpaBuf;        /* spa_buffer */
typedef struct { PwSpaBuf *buffer; void *user_data; uint64_t size; } PwBuffer;               /* pw_buffer (читаем только buffer) */
typedef struct { int32_t x, y; uint32_t w, h; } PwRegion;                                    /* spa_meta_region */
typedef struct {                                 /* pw_stream_events, версия 2 */
    uint32_t version;
    void (*destroy)(void *data);
    void (*state_changed)(void *data, int old, int state, const char *error);
    void (*control_info)(void *data, uint32_t id, const void *control);
    void (*io_changed)(void *data, uint32_t id, void *area, uint32_t size);
    void (*param_changed)(void *data, uint32_t id, const void *param);
    void (*add_buffer)(void *data, PwBuffer *buffer);
    void (*remove_buffer)(void *data, PwBuffer *buffer);
    void (*process)(void *data);
    void (*drained)(void *data);
    void (*command)(void *data, const void *command);
    void (*trigger_done)(void *data);
} PwStreamEvents;
enum {                                           /* spa/utils/type.h */
    T_ID = 3, T_INT = 4, T_LONG = 5, T_RECT = 10, T_FRAC = 11, T_OBJECT = 15, T_CHOICE = 19,
    T_OBJ_FORMAT = 0x40003, T_OBJ_BUFFERS = 0x40004, T_OBJ_META = 0x40005,
};
enum { P_ENUMFORMAT = 3, P_FORMAT = 4, P_BUFFERS = 5, P_META = 6 };   /* spa/param/param.h */
enum {                                           /* spa/param/format.h, video/raw.h, buffers.h, meta.h, buffer.h */
    K_MEDIATYPE = 1, K_MEDIASUBTYPE = 2, K_VFORMAT = 0x20001, K_VMODIFIER = 0x20002, K_VSIZE = 0x20003,
    K_VFRAMERATE = 0x20004, K_VMAXFRAMERATE = 0x20005,
    MT_VIDEO = 2, MST_RAW = 1,
    VF_RGBX = 7, VF_BGRX = 8, VF_RGBA = 11, VF_BGRA = 12,
    KB_BUFFERS = 1, KB_DATATYPE = 6, KM_TYPE = 1, KM_SIZE = 2,
    D_MEMPTR = 1, D_MEMFD = 2, D_DMABUF = 3,
    PROP_MANDATORY = 1 << 3, PROP_DONT_FIXATE = 1 << 4,   /* spa/pod/pod.h: SPA_POD_PROP_FLAG_* */
    META_VIDEOCROP = 2,
    CH_NONE = 0, CH_RANGE = 1, CH_ENUM = 3, CH_FLAGS = 4,
    CHUNK_CORRUPTED = 1,
};
enum { ST_ERROR = -1, ST_UNCONNECTED = 0, ST_CONNECTING = 1, ST_PAUSED = 2, ST_STREAMING = 3 };   /* pw_stream_state */
#define F_AUTOCONNECT (1u << 0)                  /* pw_stream_flags */
#define F_MAP_BUFFERS (1u << 2)
#define SHPW_MAXDIM 16384
#define SHPW_MAPS 8                              /* отображённых DMA-BUF (буферов потока не больше 4) */
#define SHPW_NOFMT_MS 10000                      /* после подключения формат так и не согласован — форматы не сошлись */
#define SHPW_LOST_MS 5000                        /* поток был, а источник отцеплен (PAUSED) дольше этого — источник пропал */

/* ---------- SPA pod: сборка ---------- */
typedef struct { uint8_t *b; size_t cap, n; bool bad; } PodB;
static void pb_put(PodB *p, const void *v, size_t k) {
    if (p->bad || k > p->cap - p->n) { p->bad = true; return; }
    memcpy(p->b + p->n, v, k);
    p->n += k;
}
static void pb_u32(PodB *p, uint32_t v) { pb_put(p, &v, 4); }
static void pb_pad(PodB *p) { static const uint8_t z[8]; size_t r = p->n & 7; if (r) pb_put(p, z, 8 - r); }
static void pb_size(PodB *p, size_t at) {        /* дописать размер тела pod, начатого с at (заголовок 8 байт) */
    if (p->bad) return;
    uint32_t sz = (uint32_t)(p->n - at - 8);
    memcpy(p->b + at, &sz, 4);
}
static size_t pb_obj_begin(PodB *p, uint32_t type, uint32_t id) {
    size_t at = p->n;
    pb_u32(p, 0); pb_u32(p, T_OBJECT); pb_u32(p, type); pb_u32(p, id);
    return at;
}
static void pb_propf(PodB *p, uint32_t key, uint32_t flags) { pb_u32(p, key); pb_u32(p, flags); }
static void pb_prop(PodB *p, uint32_t key) { pb_propf(p, key, 0); }
static void pb_id(PodB *p, uint32_t key, uint32_t v) { pb_prop(p, key); pb_u32(p, 4); pb_u32(p, T_ID); pb_u32(p, v); pb_pad(p); }
/* Choice: n значений по k байт типа t */
static void pb_choicef(PodB *p, uint32_t key, uint32_t pflags, uint32_t ctype, uint32_t t, uint32_t k, const uint32_t *vals, int nv) {
    pb_propf(p, key, pflags);
    size_t at = p->n;
    pb_u32(p, 0); pb_u32(p, T_CHOICE); pb_u32(p, ctype); pb_u32(p, 0); pb_u32(p, k); pb_u32(p, t);
    for (int i = 0; i < nv; i++) pb_put(p, vals + i * (int)(k / 4), k);
    pb_size(p, at);
    pb_pad(p);
}
static void pb_choice(PodB *p, uint32_t key, uint32_t ctype, uint32_t t, uint32_t k, const uint32_t *vals, int nv) {
    pb_choicef(p, key, 0, ctype, t, k, vals, nv);
}
static size_t pb_enumformat(PodB *p, bool linear) {
    /* видео raw, 32 бита на точку, любой размер, частота источника до 30 к/с; linear — DMA-BUF с модификатором LINEAR */
    size_t at = pb_obj_begin(p, T_OBJ_FORMAT, P_ENUMFORMAT);
    pb_id(p, K_MEDIATYPE, MT_VIDEO);
    pb_id(p, K_MEDIASUBTYPE, MST_RAW);
    static const uint32_t fm[] = { VF_BGRX, VF_BGRX, VF_RGBX, VF_BGRA, VF_RGBA };   /* первое — по умолчанию */
    pb_choice(p, K_VFORMAT, CH_ENUM, T_ID, 4, fm, 5);
    if (linear) {                                /* Choice Enum из одного LINEAR (0): по умолчанию и единственный */
        static const uint32_t md[] = { 0, 0, 0, 0 };
        pb_choicef(p, K_VMODIFIER, PROP_MANDATORY | PROP_DONT_FIXATE, CH_ENUM, T_LONG, 8, md, 2);
    }
    static const uint32_t sz[] = { 1920, 1080, 1, 1, 8192, 8192 };
    pb_choice(p, K_VSIZE, CH_RANGE, T_RECT, 8, sz, 3);
    static const uint32_t fr[] = { 30, 1, 0, 1, 360, 1 };
    pb_choice(p, K_VFRAMERATE, CH_RANGE, T_FRAC, 8, fr, 3);
    static const uint32_t mfr[] = { 30, 1, 1, 1, 30, 1 };
    pb_choice(p, K_VMAXFRAMERATE, CH_RANGE, T_FRAC, 8, mfr, 3);
    pb_size(p, at);
    return at;
}
static size_t pb_buffers(PodB *p, uint32_t types) {   /* 3 буфера (2..4) нужного типа памяти */
    size_t at = pb_obj_begin(p, T_OBJ_BUFFERS, P_BUFFERS);
    static const uint32_t nb[] = { 3, 2, 4 };
    pb_choice(p, KB_BUFFERS, CH_RANGE, T_INT, 4, nb, 3);
    pb_choice(p, KB_DATATYPE, CH_FLAGS, T_INT, 4, &types, 1);
    pb_size(p, at);
    return at;
}
int shpw_build_params(uint8_t *buf, size_t cap, const void *out[SHPW_NPAR], size_t *len) {
    PodB p = { buf, cap, 0, false };
    size_t o[SHPW_NPAR];
    o[0] = pb_enumformat(&p, false);             /* сначала общая память */
    o[1] = pb_enumformat(&p, true);              /* потом линейный DMA-BUF */
    o[2] = pb_buffers(&p, (1u << D_MEMPTR) | (1u << D_MEMFD));
    o[3] = pb_buffers(&p, 1u << D_DMABUF);
    o[4] = pb_obj_begin(&p, T_OBJ_META, P_META); /* Meta: обрезка кадра (VideoCrop) */
    pb_id(&p, KM_TYPE, META_VIDEOCROP);
    pb_prop(&p, KM_SIZE); pb_u32(&p, 4); pb_u32(&p, T_INT); pb_u32(&p, (uint32_t)sizeof(PwRegion)); pb_pad(&p);
    pb_size(&p, o[4]);
    if (p.bad) return -1;
    for (int i = 0; i < SHPW_NPAR; i++) out[i] = buf + o[i];
    *len = p.n;
    return SHPW_NPAR;
}

/* ---------- SPA pod: разбор согласованного формата ---------- */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
/* Значение свойства: сам pod или первое значение Choice. Вернуть тип, *v — тело, *vn — его длина. */
static uint32_t pod_value(const uint8_t *pod, uint32_t size, uint32_t type, const uint8_t **v, uint32_t *vn) {
    if (type != T_CHOICE) { *v = pod; *vn = size; return type; }
    if (size < 16) return 0;
    uint32_t k = rd32(pod + 8), t = rd32(pod + 12);
    if (!k || k > size - 16) return 0;
    *v = pod + 16; *vn = k;
    return t;
}
/* Модификатор: Long или Choice из Long; все значения должны быть LINEAR (0). 1 — да, 0 — мусор, -1 — нелинейный. */
static int pod_linear(const uint8_t *pod, uint32_t size, uint32_t type) {
    if (type == T_LONG) { if (size < 8) return 0; uint64_t m; memcpy(&m, pod, 8); return m ? -1 : 1; }
    if (type != T_CHOICE || size < 16 || rd32(pod + 8) != 8 || rd32(pod + 12) != T_LONG) return 0;
    uint32_t nv = (size - 16) / 8;
    if (!nv) return 0;
    for (uint32_t i = 0; i < nv; i++) { uint64_t m; memcpy(&m, pod + 16 + i * 8, 8); if (m) return -1; }
    return 1;
}
int shpw_parse_format(const void *pod, size_t n, uint32_t *w, uint32_t *h, int *rgb, int *dma) {
    const uint8_t *b = pod;
    if (!pod || n < 16) return 0;
    uint32_t size = rd32(b), type = rd32(b + 4);
    if (type != T_OBJECT || size < 8 || size > n - 8) return 0;
    if (rd32(b + 8) != T_OBJ_FORMAT) return 0;
    const uint8_t *p = b + 16, *end = b + 8 + size;
    bool mt_ok = false, st_ok = false, fmt_ok = false, sz_ok = false;
    *dma = 0;
    while ((size_t)(end - p) >= 16) {
        uint32_t key = rd32(p), vs = rd32(p + 8), vt = rd32(p + 12);
        if (vs > (size_t)(end - p) - 16) return 0;   /* свойство за концом объекта */
        const uint8_t *v = NULL;
        uint32_t vn = 0, t = pod_value(p + 16, vs, vt, &v, &vn);
        if (key == K_VMODIFIER) {                        /* модификатор — DMA-BUF: только линейный (mmap без GPU) */
            int lin = pod_linear(p + 16, vs, vt);
            if (lin <= 0) return lin;
            *dma = 1;
        }
        if (key == K_MEDIATYPE || key == K_MEDIASUBTYPE || key == K_VFORMAT) {
            if (t != T_ID || vn < 4) return 0;
            uint32_t id = rd32(v);
            if (key == K_MEDIATYPE) { if (id != MT_VIDEO) return 0; mt_ok = true; }
            else if (key == K_MEDIASUBTYPE) { if (id != MST_RAW) return 0; st_ok = true; }
            else {
                if (id == VF_BGRX || id == VF_BGRA) *rgb = 0;
                else if (id == VF_RGBX || id == VF_RGBA) *rgb = 1;
                else return 0;
                fmt_ok = true;
            }
        } else if (key == K_VSIZE) {
            if (t != T_RECT || vn < 8) return 0;
            *w = rd32(v); *h = rd32(v + 4);
            if (*w < 1 || *h < 1 || *w > SHPW_MAXDIM || *h > SHPW_MAXDIM) return 0;
            sz_ok = true;
        }
        size_t adv = 16 + (((size_t)vs + 7) & ~(size_t)7);
        if (adv > (size_t)(end - p)) break;
        p += adv;
    }
    return mt_ok && st_ok && fmt_ok && sz_ok ? 1 : 0;
}

/* ---------- проверка буфера от источника ---------- */
bool shpw_check_frame(uint32_t fw, uint32_t fh, int rgb, uint32_t dtype, const void *ptr, uint32_t maxsize,
                      uint32_t off, uint32_t size, int32_t stride, int32_t flags,
                      bool has_crop, int32_t cx, int32_t cy, uint32_t cw, uint32_t chh, ShPwFrame *f) {
    if (fw < 1 || fh < 1 || fw > SHPW_MAXDIM || fh > SHPW_MAXDIM) return false;
    if (dtype != D_MEMPTR && dtype != D_MEMFD && dtype != D_DMABUF) return false;   /* память, отображённая для чтения */
    if (!ptr || (flags & CHUNK_CORRUPTED) || !size) return false;   /* size 0 — новой картинки нет (только курсор/метаданные) */
    uint64_t row = (uint64_t)fw * 4, st = stride ? (uint64_t)(int64_t)stride : row;
    if (stride < 0 || st < row || st > (uint64_t)SHPW_MAXDIM * 8 || (st & 3)) return false;
    if (off > maxsize || size > maxsize - off) return false;
    if ((uint64_t)(fh - 1) * st + row > size) return false;
    const uint8_t *base = (const uint8_t *)ptr + off;
    if ((uintptr_t)base & 3) return false;       /* строки читаются по 32 бита */
    uint32_t x = 0, y = 0, w = fw, h = fh;
    if (has_crop && cw && chh && cx >= 0 && cy >= 0 && (uint64_t)(uint32_t)cx + cw <= fw && (uint64_t)(uint32_t)cy + chh <= fh) {
        x = (uint32_t)cx; y = (uint32_t)cy; w = cw; h = chh;
    }                                            /* обрезка вне кадра — не верим ей, берём кадр целиком */
    f->data = base + (size_t)y * (size_t)st + (size_t)x * 4;
    f->w = (int)w; f->h = (int)h; f->stride = (size_t)st; f->rgb = rgb;
    return true;
}

/* ---------- libpipewire через dlopen ---------- */
static struct {
    void *lib;
    bool inited;
    void (*init)(int *, char ***);
    void *(*tl_new)(const char *, const void *);
    void (*tl_destroy)(void *);
    int (*tl_start)(void *);
    void (*tl_stop)(void *);
    void (*tl_lock)(void *);
    void (*tl_unlock)(void *);
    void *(*tl_loop)(void *);
    void *(*ctx_new)(void *, void *, size_t);
    void (*ctx_destroy)(void *);
    void *(*connect_fd)(void *, int, void *, size_t);
    int (*core_disconnect)(void *);
    void *(*props_new)(const char *, ...);
    void *(*st_new)(void *, const char *, void *);
    void (*st_destroy)(void *);
    void (*st_listen)(void *, PwHook *, const PwStreamEvents *, void *);
    int (*st_connect)(void *, int, uint32_t, uint32_t, const void **, uint32_t);
    PwBuffer *(*st_dequeue)(void *);
    int (*st_queue)(void *, PwBuffer *);
    int (*st_update)(void *, const void **, uint32_t);
    /* поток */
    void *tl, *ctx, *core, *st;
    PwHook hook;
    uint8_t parbuf[1536];
    const void *par[SHPW_NPAR];
    /* под замком цикла */
    uint32_t fw, fh;                             /* согласованный размер; 0 — формата нет */
    int rgb, bad_fmt;                            /* bad_fmt: 1 — формат не подходит, 2 — только нелинейный DMA-BUF */
    bool dma, ended, nofmt, locked, streamed, synced;
    double open_at;                              /* мс: когда подключились к узлу */
    double paused_at;                            /* мс CLOCK_MONOTONIC: с какого момента PAUSED после STREAMING (0 — нет) */
    PwBuffer *held;                              /* самый свежий проверенный буфер, держим у себя */
    int held_fd;                                 /* его DMA-BUF (для DMA_BUF_IOCTL_SYNC), -1 — общая память */
    struct { PwBuffer *b; void *m; size_t n; } maps[SHPW_MAPS];   /* отображённые DMA-BUF по буферам потока */
    uint8_t *rowbuf;                             /* буфер строки для некэшируемых кадров (выровнен по 64) */
    size_t rowcap;
    ShPwFrame cur;
    uint32_t seq;
} P;

bool shpw_load(char *err, size_t n) {
    if (P.lib) return true;
    void *l = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!l) { snprintf(err, n, "%s", "не найдена библиотека PipeWire (libpipewire-0.3)"); return false; }
    bool ok = true;
#define SY(f, name) do { *(void **)(&P.f) = dlsym(l, name); if (!P.f) ok = false; } while (0)
    SY(init, "pw_init"); SY(tl_new, "pw_thread_loop_new"); SY(tl_destroy, "pw_thread_loop_destroy");
    SY(tl_start, "pw_thread_loop_start"); SY(tl_stop, "pw_thread_loop_stop"); SY(tl_lock, "pw_thread_loop_lock");
    SY(tl_unlock, "pw_thread_loop_unlock"); SY(tl_loop, "pw_thread_loop_get_loop"); SY(ctx_new, "pw_context_new");
    SY(ctx_destroy, "pw_context_destroy"); SY(connect_fd, "pw_context_connect_fd"); SY(core_disconnect, "pw_core_disconnect");
    SY(props_new, "pw_properties_new"); SY(st_new, "pw_stream_new"); SY(st_destroy, "pw_stream_destroy");
    SY(st_listen, "pw_stream_add_listener"); SY(st_connect, "pw_stream_connect"); SY(st_dequeue, "pw_stream_dequeue_buffer");
    SY(st_queue, "pw_stream_queue_buffer"); SY(st_update, "pw_stream_update_params");
#undef SY
    if (!ok) { dlclose(l); snprintf(err, n, "%s", "библиотека PipeWire слишком старая"); return false; }
    P.lib = l;                                   /* не выгружается до выхода: у libpipewire общее состояние (pw_init) */
    return true;
}

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6 + 1.0;   /* +1: ноль — «нет отметки» */
}
/* Источник исчез (окно закрыто, бэкенд портала упал, выход отключён): узел источника удаляется, и наш поток
 * не обрывается, а уходит в PAUSED. Короткая пауза — пересогласование формата; долгая — конец (см. shpw_lock). */
static void on_state(void *d, int old, int state, const char *error) {
    (void)d; (void)old;
    if (state == ST_ERROR || state == ST_UNCONNECTED) P.ended = true;
    /* ошибка до согласования формата — форматы не сошлись (у источника только нелинейный DMA-BUF и т.п.) */
    if (state == ST_ERROR && !P.fw && !P.streamed && error && strstr(error, "format")) P.nofmt = true;
    if (state == ST_STREAMING) { P.streamed = true; P.paused_at = 0; }
    else if (state == ST_PAUSED && P.streamed && !P.paused_at) P.paused_at = mono_ms();
}
static void forget_held(void) {
    if (P.held && P.st) P.st_queue(P.st, P.held);
    P.held = NULL; P.held_fd = -1;
    memset(&P.cur, 0, sizeof P.cur);
}
static void unmap_buf(PwBuffer *b) {             /* b == NULL — все */
    for (int i = 0; i < SHPW_MAPS; i++)
        if (P.maps[i].m && (!b || P.maps[i].b == b)) { munmap(P.maps[i].m, P.maps[i].n); P.maps[i].m = NULL; P.maps[i].b = NULL; P.maps[i].n = 0; }
}
/* DMA-BUF буфера b: отображение только для чтения (один раз на буфер). *n — длина. NULL — не вышло. */
static void *map_buf(PwBuffer *b, int fd, size_t *n) {
    int fr = -1;
    for (int i = 0; i < SHPW_MAPS; i++) {
        if (P.maps[i].m && P.maps[i].b == b) { *n = P.maps[i].n; return P.maps[i].m; }
        if (!P.maps[i].m && fr < 0) fr = i;
    }
    if (fr < 0 || fd < 0) return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);           /* размер DMA-BUF */
    if (sz <= 0 || (uint64_t)sz > 0xFFFFFFFFull) return NULL;
    void *m = mmap(NULL, (size_t)sz, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return NULL;
    P.maps[fr].b = b; P.maps[fr].m = m; P.maps[fr].n = (size_t)sz;
    *n = (size_t)sz;
    return m;
}
static void on_param(void *d, uint32_t id, const void *param) {
    (void)d;
    if (id != P_FORMAT) return;
    forget_held();
    P.fw = P.fh = 0;
    if (!param) return;                          /* формат снят (пересогласование) */
    uint32_t w = 0, h = 0, sz = 0;
    int rgb = 0, dma = 0;
    memcpy(&sz, param, 4);
    int r = shpw_parse_format(param, (size_t)sz + 8, &w, &h, &rgb, &dma);
    if (r <= 0) { P.bad_fmt = r < 0 ? 2 : 1; return; }
    P.fw = w; P.fh = h; P.rgb = rgb; P.dma = dma != 0; P.bad_fmt = 0;
    const void *up[2] = { P.par[P.dma ? 3 : 2], P.par[4] };   /* Buffers (общая память или DMA-BUF) и Meta (VideoCrop) */
    P.st_update(P.st, up, 2);
}
static void on_remove(void *d, PwBuffer *b) {
    (void)d;
    if (b == P.held) { P.held = NULL; P.held_fd = -1; memset(&P.cur, 0, sizeof P.cur); }   /* буфер уходит: не трогать его больше */
    unmap_buf(b);
}
static void on_process(void *d) {
    (void)d;
    PwBuffer *b, *last = NULL;
    while ((b = P.st_dequeue(P.st))) { if (last) P.st_queue(P.st, last); last = b; }   /* нужен только самый свежий */
    if (!last) return;
    ShPwFrame f;
    const PwSpaBuf *sb = last->buffer;
    bool ok = P.fw && sb && sb->n_datas >= 1 && sb->datas && sb->datas[0].chunk;
    if (ok) {
        const PwData *dd = &sb->datas[0];
        const PwChunk *c = dd->chunk;
        const PwRegion *crop = NULL;
        for (uint32_t i = 0; sb->metas && i < sb->n_metas; i++)
            if (sb->metas[i].type == META_VIDEOCROP && sb->metas[i].data && sb->metas[i].size >= sizeof(PwRegion)) crop = sb->metas[i].data;
        const void *ptr = dd->data;
        uint32_t maxsize = dd->maxsize;
        int dfd = -1;
        if (dd->type == D_DMABUF) {              /* только при согласованном линейном модификаторе */
            ok = P.dma && dd->fd >= 0 && dd->fd <= 0x7FFFFFFF;
            if (ok) dfd = (int)dd->fd;
            if (ok && !ptr) {                    /* PipeWire не отобразил сам — отображаем (один раз на буфер) */
                size_t mn = 0;
                ptr = map_buf(last, dfd, &mn);
                maxsize = (uint32_t)mn;
            }
        } else if (P.dma) ok = false;            /* договорились о DMA-BUF, а пришло другое — не верим */
        ok = ok && shpw_check_frame(P.fw, P.fh, P.rgb, dd->type, ptr, maxsize, c->offset, c->size, c->stride, c->flags,
                                    crop != NULL, crop ? crop->x : 0, crop ? crop->y : 0, crop ? crop->w : 0, crop ? crop->h : 0, &f);
        if (ok) { f.uncached = dd->type == D_DMABUF; if (P.held) P.st_queue(P.st, P.held); P.held = last; P.held_fd = dfd; P.cur = f; P.seq++; return; }
    }
    P.st_queue(P.st, last);                      /* пустой или негодный буфер — прежняя картинка остаётся */
}

bool shpw_open(int fd, uint32_t node, char *err, size_t n) {
    if (!shpw_load(err, n)) { if (fd >= 0) close(fd); return false; }
    if (!P.inited) { P.init(NULL, NULL); P.inited = true; }
    static const PwStreamEvents ev = { .version = 2, .state_changed = on_state, .param_changed = on_param,
                                       .remove_buffer = on_remove, .process = on_process };
    size_t len = 0;
    P.fw = P.fh = 0; P.bad_fmt = 0; P.dma = P.ended = P.nofmt = P.streamed = P.synced = false; P.paused_at = 0; P.held = NULL; P.held_fd = -1; P.seq = 0;
    memset(P.maps, 0, sizeof P.maps);
    P.open_at = mono_ms();
    memset(&P.cur, 0, sizeof P.cur);
    memset(&P.hook, 0, sizeof P.hook);
    if (shpw_build_params(P.parbuf, sizeof P.parbuf, P.par, &len) != SHPW_NPAR) { close(fd); snprintf(err, n, "%s", "не удалось подключиться к PipeWire"); return false; }
    P.tl = P.tl_new("astra-share", NULL);
    if (!P.tl) { close(fd); snprintf(err, n, "%s", "не удалось подключиться к PipeWire"); return false; }
    P.ctx = P.ctx_new(P.tl_loop(P.tl), NULL, 0);
    if (!P.ctx) { close(fd); shpw_close(); snprintf(err, n, "%s", "не удалось подключиться к PipeWire"); return false; }
    if (P.tl_start(P.tl) < 0) { close(fd); shpw_close(); snprintf(err, n, "%s", "не удалось подключиться к PipeWire"); return false; }
    P.tl_lock(P.tl);
    P.core = P.connect_fd(P.ctx, fd, NULL, 0); /* fd теперь у PipeWire */
    bool ok = P.core != NULL;
    if (ok) {
        void *props = P.props_new("media.type", "Video", "media.category", "Capture", "media.role", "Screen", (const char *)NULL);
        P.st = P.st_new(P.core, "astra-share", props);
        ok = P.st != NULL;
    }
    if (ok) {
        P.st_listen(P.st, &P.hook, &ev, NULL);
        ok = P.st_connect(P.st, 0 /* SPA_DIRECTION_INPUT */, node, F_AUTOCONNECT | F_MAP_BUFFERS, P.par, 2) >= 0;   /* EnumFormat: общая память, линейный DMA-BUF */
    }
    P.tl_unlock(P.tl);
    if (!ok) { shpw_close(); snprintf(err, n, "%s", "не удалось подключиться к PipeWire"); return false; }
    return true;
}
int shpw_lock(ShPwFrame *f, uint32_t *seq) {
    if (!P.tl) return -1;
    P.tl_lock(P.tl);
    P.locked = true;
    /* форматы не сошлись: PipeWire может не перевести поток в ERROR, а просто не дать формат */
    if (P.nofmt || P.bad_fmt == 2 || (!P.fw && !P.bad_fmt && !P.streamed && !P.ended && mono_ms() - P.open_at > SHPW_NOFMT_MS)) return -3;
    if (P.ended || (P.paused_at && mono_ms() - P.paused_at > SHPW_LOST_MS)) return -1;
    if (P.bad_fmt) return -2;
    if (!P.held || !P.cur.data) return 0;
    if (P.held_fd >= 0) {                        /* DMA-BUF: начало чтения процессором (кэши, ожидание записи источника) */
        struct dma_buf_sync sy = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        P.synced = ioctl(P.held_fd, DMA_BUF_IOCTL_SYNC, &sy) == 0;
    }
    *f = P.cur; *seq = P.seq;
    return 1;
}
void shpw_unlock(void) {
    if (!P.tl || !P.locked) return;
    if (P.synced && P.held_fd >= 0) {            /* конец чтения: до возврата буфера источнику (буфер ещё наш) */
        struct dma_buf_sync sy = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        ioctl(P.held_fd, DMA_BUF_IOCTL_SYNC, &sy);
    }
    P.synced = false;
    P.locked = false;
    P.tl_unlock(P.tl);
}
void shpw_close(void) {
    if (P.rowbuf) { explicit_bzero(P.rowbuf, P.rowcap); free(P.rowbuf); P.rowbuf = NULL; P.rowcap = 0; }   /* там строка экрана */
    if (!P.lib) return;
    if (P.tl) P.tl_stop(P.tl);                   /* цикл остановлен: обратных вызовов больше нет */
    if (P.st) P.st_destroy(P.st);                /* буферы (и удерживаемый) уходят вместе с потоком */
    if (P.core) P.core_disconnect(P.core);
    if (P.ctx) P.ctx_destroy(P.ctx);
    if (P.tl) P.tl_destroy(P.tl);
    unmap_buf(NULL);                             /* что не сняли в remove_buffer */
    P.st = P.core = P.ctx = P.tl = NULL;
    P.held = NULL; P.held_fd = -1; P.dma = false;
    memset(&P.cur, 0, sizeof P.cur);
    P.fw = P.fh = 0; P.seq = 0; P.locked = false; P.streamed = false; P.paused_at = 0;
}

/* ---------- строка некэшируемого DMA-BUF -> кэшируемый буфер ---------- */
#if SHPW_X86
/* Блоки по 16 байт [s0, s1) (оба выровнены по 16) -> d (выровнен по 16); 4 загрузки подряд — одна строка WC-буфера */
__attribute__((target("sse4.1"))) static void nt_copy(uint8_t *d, const uint8_t *s0, const uint8_t *s1) {
    const __m128i *s = (const __m128i *)(const void *)s0, *e = (const __m128i *)(const void *)s1;
    __m128i *o = (__m128i *)(void *)d;
    for (; s + 4 <= e; s += 4, o += 4) {
        __m128i x0 = _mm_stream_load_si128((__m128i *)(uintptr_t)s), x1 = _mm_stream_load_si128((__m128i *)(uintptr_t)(s + 1));
        __m128i x2 = _mm_stream_load_si128((__m128i *)(uintptr_t)(s + 2)), x3 = _mm_stream_load_si128((__m128i *)(uintptr_t)(s + 3));
        _mm_store_si128(o, x0); _mm_store_si128(o + 1, x1); _mm_store_si128(o + 2, x2); _mm_store_si128(o + 3, x3);
    }
    for (; s < e; s++, o++) _mm_store_si128(o, _mm_stream_load_si128((__m128i *)(uintptr_t)s));
}
static int have_sse41(void) {                    /* SSE4.1 не входит в базовый x86-64: проверить один раз */
    static int v = -1;
    if (v < 0) { __builtin_cpu_init(); v = __builtin_cpu_supports("sse4.1") ? 1 : 0; }
    return v;
}
#endif
const uint8_t *shpw_row_copy(uint8_t *dst16, const uint8_t *src, size_t a, size_t b, int nt) {
    size_t mis = (uintptr_t)src & 15;
    uint8_t *r = dst16 + mis;                    /* r[i] <-> src[i]; адрес src, выровненный по 16, ложится на выровненный */
    if (b <= a) return r;
#if SHPW_X86
    if (nt && have_sse41()) {
        /* границы — по 16 байт наружу: лишнее не выходит за 16-байтовый блок (и страницу) строки */
        const uint8_t *s0 = (const uint8_t *)((uintptr_t)(src + a) & ~(uintptr_t)15);
        const uint8_t *s1 = (const uint8_t *)(((uintptr_t)(src + b) + 15) & ~(uintptr_t)15);
        nt_copy(r + (s0 - src), s0, s1);
        return r;
    }
#else
    (void)nt;
#endif
    memcpy(r + a, src + a, b - a);
    return r;
}
const uint8_t *shpw_row(const uint8_t *src, size_t a, size_t b) {
    size_t need = b + 64;
    if (need > P.rowcap) {
        size_t cap = (need + 63) & ~(size_t)63;
        uint8_t *nb = aligned_alloc(64, cap);
        if (!nb) return src;
        if (P.rowbuf) { explicit_bzero(P.rowbuf, P.rowcap); free(P.rowbuf); }
        P.rowbuf = nb; P.rowcap = cap;
    }
    return shpw_row_copy(P.rowbuf, src, a, b, 1);
}

/* ---------- самопроверка: без PipeWire и портала ---------- */
/* согласованный формат: fmt, размер, модификатор (mk: 0 — нет, 1 — Long m, 2 — Choice Enum из m и m2) */
static size_t st_format(uint8_t *fb, size_t cap, uint32_t fmt, uint32_t w, uint32_t h, int mk, uint64_t m, uint64_t m2) {
    PodB q = { fb, cap, 0, false };
    size_t at = pb_obj_begin(&q, T_OBJ_FORMAT, P_FORMAT);
    pb_id(&q, K_MEDIATYPE, MT_VIDEO); pb_id(&q, K_MEDIASUBTYPE, MST_RAW); pb_id(&q, K_VFORMAT, fmt);
    if (mk == 1) { pb_propf(&q, K_VMODIFIER, PROP_MANDATORY); pb_u32(&q, 8); pb_u32(&q, T_LONG); pb_put(&q, &m, 8); }
    if (mk == 2) { uint64_t mm[3] = { m, m, m2 }; pb_choicef(&q, K_VMODIFIER, PROP_MANDATORY | PROP_DONT_FIXATE, CH_ENUM, T_LONG, 8, (const uint32_t *)(const void *)mm, 3); }
    pb_prop(&q, K_VSIZE); pb_u32(&q, 8); pb_u32(&q, T_RECT); pb_u32(&q, w); pb_u32(&q, h);
    pb_size(&q, at);
    return q.bad ? 0 : q.n;
}
int share_pw_selftest(void (*check)(bool ok, const char *what)) {
    uint8_t buf[1536];
    const void *par[SHPW_NPAR] = { 0 };
    size_t len = 0;
    int np = shpw_build_params(buf, sizeof buf, par, &len);
    bool b1 = np == SHPW_NPAR && len > 0 && len % 8 == 0;
    for (int i = 0; b1 && i < SHPW_NPAR; i++) { uint32_t sz = rd32(par[i]); b1 = rd32((const uint8_t *)par[i] + 4) == T_OBJECT && (const uint8_t *)par[i] + 8 + sz <= buf + len; }
    uint8_t tiny[64];
    check(b1 && shpw_build_params(tiny, sizeof tiny, par, &len) < 0, "Wayland: параметры PipeWire (2 EnumFormat, 2 Buffers, Meta) собраны, мал буфер — отказ без записи за край");
    shpw_build_params(buf, sizeof buf, par, &len);
    uint32_t w = 0, h = 0;
    int rgb = -1, dma = -1;
    bool b2 = shpw_parse_format(par[0], len, &w, &h, &rgb, &dma) == 1 && w == 1920 && h == 1080 && rgb == 0 && dma == 0;
    bool b2d = shpw_parse_format(par[1], len, &w, &h, &rgb, &dma) == 1 && dma == 1;   /* свой EnumFormat с LINEAR */
    /* флаги свойства модификатора в своём EnumFormat: MANDATORY | DONT_FIXATE; Buffers DMA-BUF — только DmaBuf */
    bool b2f = false;
    {
        const uint8_t *pp = (const uint8_t *)par[1] + 16, *end = (const uint8_t *)par[1] + 8 + rd32(par[1]);
        while (end - pp >= 16) {
            if (rd32(pp) == K_VMODIFIER) { b2f = rd32(pp + 4) == (PROP_MANDATORY | PROP_DONT_FIXATE) && rd32(pp + 12) == T_CHOICE; break; }
            pp += 16 + ((rd32(pp + 8) + 7) & ~7u);
        }
    }
    uint8_t fb[256];
    size_t n1 = st_format(fb, sizeof fb, VF_RGBX, 1366, 768, 0, 0, 0);
    bool b3 = shpw_parse_format(fb, n1, &w, &h, &rgb, &dma) == 1 && w == 1366 && h == 768 && rgb == 1 && dma == 0;
    bool b4 = !shpw_parse_format(fb, n1 - 8, &w, &h, &rgb, &dma) && !shpw_parse_format(fb, 12, &w, &h, &rgb, &dma) && !shpw_parse_format(NULL, 64, &w, &h, &rgb, &dma);
    uint32_t big = 0x7FFFFFF0u;                  /* размер объекта больше данных */
    memcpy(fb, &big, 4);
    b4 = b4 && !shpw_parse_format(fb, n1, &w, &h, &rgb, &dma);
    size_t nq = st_format(fb, sizeof fb, 2 /* I420 */, 640, 480, 0, 0, 0);
    bool b5 = nq && shpw_parse_format(fb, nq, &w, &h, &rgb, &dma) == 0;
    nq = st_format(fb, sizeof fb, VF_BGRA, 0, 480, 0, 0, 0);
    b5 = b5 && nq && shpw_parse_format(fb, nq, &w, &h, &rgb, &dma) == 0;
    /* модификаторы: LINEAR (Long и Choice) — DMA-BUF с mmap; плиточный (Intel Y-tiled, AMD) — «нужен GPU» */
    nq = st_format(fb, sizeof fb, VF_BGRX, 1920, 1080, 1, 0, 0);
    bool m1 = nq && shpw_parse_format(fb, nq, &w, &h, &rgb, &dma) == 1 && dma == 1 && w == 1920;
    nq = st_format(fb, sizeof fb, VF_BGRA, 800, 600, 2, 0, 0);
    m1 = m1 && nq && shpw_parse_format(fb, nq, &w, &h, &rgb, &dma) == 1 && dma == 1 && w == 800;
    nq = st_format(fb, sizeof fb, VF_BGRX, 1920, 1080, 1, 0x0100000000000002ull, 0);
    bool m2 = nq && shpw_parse_format(fb, nq, &w, &h, &rgb, &dma) == -1;
    nq = st_format(fb, sizeof fb, VF_BGRX, 1920, 1080, 2, 0, 0x0200000018601b04ull);   /* LINEAR и нелинейный вместе */
    m2 = m2 && nq && shpw_parse_format(fb, nq, &w, &h, &rgb, &dma) == -1;
    PodB z = { fb, sizeof fb, 0, false };        /* модификатор не того типа (Int) — мусор */
    size_t at = pb_obj_begin(&z, T_OBJ_FORMAT, P_FORMAT);
    pb_id(&z, K_MEDIATYPE, MT_VIDEO); pb_id(&z, K_MEDIASUBTYPE, MST_RAW); pb_id(&z, K_VFORMAT, VF_BGRX);
    pb_prop(&z, K_VMODIFIER); pb_u32(&z, 4); pb_u32(&z, T_INT); pb_u32(&z, 0); pb_pad(&z);
    pb_prop(&z, K_VSIZE); pb_u32(&z, 8); pb_u32(&z, T_RECT); pb_u32(&z, 64); pb_u32(&z, 64);
    pb_size(&z, at);
    m2 = m2 && shpw_parse_format(fb, z.n, &w, &h, &rgb, &dma) == 0;
    check(b2 && b3, "Wayland: разбор формата PipeWire — BGRx (свой EnumFormat), RGBx 1366x768 (порядок R/B)");
    check(b4 && b5, "Wayland: формат — обрезанный, с размером за краем, I420, размер 0 — отказ");
    check(b2d && b2f && m1, "Wayland: DMA-BUF — свой EnumFormat с LINEAR (MANDATORY|DONT_FIXATE); согласованный LINEAR (Long и Choice) принят");
    check(m2, "Wayland: DMA-BUF — плиточный модификатор и смесь с LINEAR — «нужен GPU», модификатор не Long — отказ");

    /* буфер кадра: 64x32 BGRx, stride 300 (с запасом), смещение 16 */
    static uint32_t mem[(16 + 300 * 32) / 4 + 4];
    ShPwFrame f;
    const uint32_t MS = (uint32_t)sizeof mem;
    bool c1 = shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 16, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)
              && f.w == 64 && f.h == 32 && f.stride == 300 && f.data == (const uint8_t *)mem + 16;
    bool c2 = shpw_check_frame(64, 32, 1, D_MEMPTR, mem, MS, 0, 64 * 4 * 32, 0, 0, false, 0, 0, 0, 0, &f) && f.stride == 256 && f.rgb == 1;
    bool c3 = !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 200, 0, false, 0, 0, 0, 0, &f)   /* stride < ширины */
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, -300, 0, false, 0, 0, 0, 0, &f) /* отрицательный */
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 302, 0, false, 0, 0, 0, 0, &f)  /* не кратен 4 */
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 31, 300, 0, false, 0, 0, 0, 0, &f)  /* мало данных */
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, MS - 8, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)   /* за maxsize */
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0xFFFFFFF0u, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 2, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f);  /* не выровнено */
    bool c4 = !shpw_check_frame(64, 32, 0, 4 /* MemId */, mem, MS, 0, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)
              && !shpw_check_frame(64, 32, 0, 0, mem, MS, 0, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)
              && !shpw_check_frame(64, 32, 0, D_DMABUF, NULL, MS, 0, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)   /* не отображён */
              && shpw_check_frame(64, 32, 0, D_DMABUF, mem, MS, 16, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f) && f.stride == 300
              && !shpw_check_frame(64, 32, 0, D_DMABUF, mem, MS, 64, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)   /* за концом отображения */
              && !shpw_check_frame(64, 32, 0, D_MEMFD, NULL, MS, 0, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 300, CHUNK_CORRUPTED, false, 0, 0, 0, 0, &f)
              && !shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 0, 300, 0, false, 0, 0, 0, 0, &f)        /* пустой: только курсор */
              && !shpw_check_frame(0, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 300, 0, false, 0, 0, 0, 0, &f)
              && !shpw_check_frame(100000, 1, 0, D_MEMFD, mem, MS, 0, 300 * 32, 0, 0, false, 0, 0, 0, 0, &f);
    bool c5 = shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 300, 0, true, 8, 4, 40, 20, &f)
              && f.w == 40 && f.h == 20 && f.data == (const uint8_t *)mem + 4 * 300 + 8 * 4;
    bool c6 = shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 300, 0, true, 30, 4, 40, 20, &f) && f.w == 64 && f.data == (const uint8_t *)mem
              && shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 300, 0, true, -1, 0, 10, 10, &f) && f.w == 64
              && shpw_check_frame(64, 32, 0, D_MEMFD, mem, MS, 0, 300 * 32, 300, 0, true, 0, 0, 0, 0, &f) && f.h == 32;
    check(c1 && c2, "Wayland: кадр PipeWire — stride с запасом и смещение, stride 0 (плотные строки), RGBx");
    check(c3, "Wayland: кадр — stride меньше строки/отрицательный/не кратен 4, мало данных, смещение за краем, невыровненный — отказ");
    check(c4, "Wayland: кадр — чужой тип памяти, не отображён, испорчен (CORRUPTED), пустой (только курсор), размер 0 или огромный — отказ; линейный DMA-BUF в границах отображения — принят");
    check(c5 && c6, "Wayland: обрезка VideoCrop (окно в GNOME) применяется; за краем или отрицательная — кадр целиком");
    uint32_t sq = 0;
    int r = shpw_lock(&f, &sq);
    shpw_unlock();
    shpw_close();
    check(r == -1, "Wayland: без открытого потока shpw_lock — «поток закончился», shpw_close без open — безопасно");
    /* копирование строки некэшируемого кадра: все сдвиги начала строки (кратные 4) и границ, оба пути */
    {
        static uint8_t srcb[1024 + 64] __attribute__((aligned(64))), dst[1024 + 128] __attribute__((aligned(64)));
        for (int i = 0; i < (int)sizeof srcb; i++) srcb[i] = (uint8_t)(i * 37 + 11);
        bool cp = true;
        int cases = 0;
        for (int nt = 0; nt < 2; nt++)
            for (size_t m = 16; m < 32 && cp; m += 4)            /* сдвиг начала строки относительно 16 */
                for (size_t a = 0; a <= 72 && cp; a += 4)
                    for (size_t b = a; b <= a + 260 && cp; b += 4) {
                        const uint8_t *row = srcb + m;
                        memset(dst, 0xEE, sizeof dst);
                        const uint8_t *rr = shpw_row_copy(dst, row, a, b, nt);
                        cp = ((uintptr_t)rr & 15) == ((uintptr_t)row & 15) && rr >= dst && rr + b <= dst + sizeof dst
                             && (b == a || !memcmp(rr + a, row + a, b - a));
                        cases++;
                    }
        size_t bigw = 7680;                      /* строка 1920 точек через shpw_row (свой буфер, растёт) */
        static uint8_t bigsrc[7680 + 32] __attribute__((aligned(64)));
        for (size_t i = 0; i < sizeof bigsrc; i++) bigsrc[i] = (uint8_t)(i ^ (i >> 7));
        const uint8_t *rb = shpw_row(bigsrc + 4, 0, bigw);
        bool bg = rb != bigsrc + 4 && !memcmp(rb, bigsrc + 4, bigw) && P.rowbuf && P.rowcap >= bigw + 32 && !((uintptr_t)P.rowbuf & 63);
        shpw_close();                            /* буфер строки затирается и освобождается */
        check(cp && cases > 1000 && bg && !P.rowbuf,
              "Wayland: строка DMA-BUF -> кэшируемый буфер (MOVNTDQA и memcpy): все сдвиги и хвосты побайтно, буфер 64, затирается при закрытии");
    }
    /* перевод RGBx (порядок R,G,B в памяти) даёт ровно то же, что BGRx той же картинки; SSE2 и скалярный — побитно */
    uint32_t bg[48], rg[48];
    uint8_t y0[48], u0[24], v0[24], y1[48], u1[24], v1[24], y2[48], u2[24], v2[24];
    uint32_t x = 2463534242u;
    for (int i = 0; i < 48; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        bg[i] = x;
        rg[i] = (x & 0xFF00FF00u) | (x >> 16 & 255) | (x & 255) << 16;   /* B и R местами */
    }
    shc_row32x(bg, NULL, 0, 48, y0, u0, v0, 0);
    shc_row32x(rg, NULL, 0, 48, y1, u1, v1, 1);
    shc_row32_c(rg, NULL, 0, 48, y2, u2, v2, 1);
    check(!memcmp(y0, y1, 48) && !memcmp(u0, u1, 24) && !memcmp(v0, v1, 24) && !memcmp(y1, y2, 48) && !memcmp(u1, u2, 24) && !memcmp(v1, v2, 24),
          "Wayland: перевод RGBx в YUV = BGRx той же картинки, SSE2 и скалярный путь побитно равны");
    return 0;
}
#endif
