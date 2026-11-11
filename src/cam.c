/* cam.c — веб-камера «Astra DS Video» (только -DASTRA_VIDEO): перечень устройств, захват, перевод в I420,
 * предпросмотр своей камеры, кодер H.264 (minih264) -> voice_cam_frame (voice_cam.c: DAVE -> RTP основного
 * голосового соединения). Договор с интерфейсом — cam.h.
 *
 * Захват — в своём потоке, синхронно (кадр за кадром, темп задаёт камера):
 *   Windows — Media Foundation Source Reader. DLL (mfplat, mf, mfreadwrite) подгружаются только при первом
 *     обращении к камере (LoadLibraryEx из System32), в зависимости exe не входят; без D3D-менеджера, без
 *     аппаратных преобразователей (MF_READWRITE_DISABLE_CONVERTERS): берём родной формат камеры NV12/YUY2/MJPG;
 *     MJPG разбирает libjpeg-turbo (уже в сборке). После выключения — MFShutdown.
 *   Linux — V4L2 напрямую (ioctl, mmap, 3 буфера), YUYV/NV12/MJPG.
 *   macOS: AVFoundation (src/cam_mac.m, -DASTRA_CAM_MAC), кадры NV12 уже вписаны в размер эфира; без cam_mac.m
 *     камера честно отказывает: «камера на macOS пока не поддерживается».
 * Режим: самый крупный не выше 1280x720 при >= 24 к/с; камера даёт только больше — уменьшаем до 1280x720.
 * Пока видео в эфир не идёт (ключей DAVE нет) и предпросмотр не виден — кадры даже не переводятся.
 * Кадры камеры (её содержимое) нигде не сохраняются: буферы затираются перед освобождением; ни файлов, ни вывода. */
#ifdef ASTRA_VIDEO
#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#elif defined(__APPLE__)
#include <errno.h>
#ifdef ASTRA_CAM_MAC
#include "cam_mac.h"
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
/* MJPG — своя копия libjpeg-turbo с SIMD (deps build_jpegcam, имена camj_*); картинки Discord (img.c) идут через
 * обычную копию без SIMD. Нет копии — обычная (медленнее, тогда работает свой предел памяти потока в img.c). */
#if __has_include(<jpegcam/camj.h>)
#include <jpegcam/camj.h>
#define CAM_JPEG_SIMD 1
#else
#include <jpeglib.h>
#include <jerror.h>
#define CAM_JPEG_SIMD 0
#endif
#include "mc.h"
#include "plat.h"
#include "cam.h"
#include "golive.h"

/* кодер — реализация minih264 уже в сборке (streamtest.c на Windows, share_x11.c на Linux): здесь только объявления */
#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "../third_party/minih264/minih264e.h"
#pragma GCC diagnostic pop

enum { CF_NV12, CF_YUY2, CF_MJPG };
static const char *const CF_NAME[] = { "NV12", "YUY2", "MJPG" };
static int CAM_MAXW = 1280, CAM_MAXH = 720;    /* наибольший кадр эфира (качество: cam_quality_set) */
#define CAM_GOP_MS 4000                          /* ключевой кадр не реже раза в 4 с (и по PLI зрителя) */

void img_jpeg_thread_limit(size_t max);
/* анонимизация (src/anon.c, отдельный разработчик): кадр I420 размера эфира — до предпросмотра и до кодера */
#ifdef ASTRA_ANON                               /* build.sh: есть src/anon.c */
#include "anon.h"
#define CAM_ANON 1
#else
#define CAM_ANON 0
#endif         /* img.c: свой предел памяти libjpeg для этого потока */

/* ======================= состояние ======================= */
static struct {
    PlatMutex *mu;
    PlatThread *thr;
    atomic_bool stop;
    atomic_bool idr;                             /* следующий кадр — ключевой (PLI зрителя / начало эфира) */
    atomic_bool running;                         /* поток захвата жив */
    atomic_bool ended;                           /* поток кончился сам (ошибка): главный поток соберёт его */
    CamState st;                                 /* под mu */
    char sel[256];                               /* выбранное устройство ("" — первое) */
    atomic_size_t bytes;                         /* буферы захвата и кодера */
    int quality;                                 /* CAM_Q_* */
    _Atomic u64 full;                            /* крупный вид (cam_want CAM_FULL) */
} C;
static void cam_lock(void) { if (!C.mu) C.mu = mutex_new(); mutex_lock(C.mu); }
static void cam_unlock(void) { mutex_unlock(C.mu); }

/* имя/ид устройства из системы: только печатные символы и UTF-8, без управляющих */
static void cam_clean(char *s) {
    for (unsigned char *p = (unsigned char *)s; *p; p++) if (*p < 0x20 || *p == 0x7F) *p = ' ';
}

/* ======================= перевод кадра в I420 =======================
 * xm/ym — номер точки/строки источника для каждой точки/строки результата (масштаб «ближайший»), NULL — один к
 * одному. Цветность результата (cx, cy) берётся у точки источника xm[2cx] (чётной) и среднего двух строк
 * ym[2cy], ym[2cy+1]: при одинаковом размере это обычное прореживание 4:2:2 -> 4:2:0 со средним по вертикали. */
typedef struct { u8 *y, *u, *v; int ys, uvs, w, h; } I420;
static inline int mx(const int *m, int i) { return m ? m[i] : i; }
static void cvt_yuy2(const u8 *s, int ss, const int *xm, const int *ym, I420 *d) {
    for (int dy = 0; dy < d->h; dy++) {
        const u8 *r = s + (size_t)mx(ym, dy) * ss;
        u8 *o = d->y + (size_t)dy * d->ys;
        if (!xm) for (int dx = 0; dx < d->w; dx++) o[dx] = r[2 * dx];
        else for (int dx = 0; dx < d->w; dx++) o[dx] = r[2 * xm[dx]];
    }
    for (int cy = 0; cy < d->h / 2; cy++) {
        const u8 *r0 = s + (size_t)mx(ym, 2 * cy) * ss, *r1 = s + (size_t)mx(ym, 2 * cy + 1) * ss;
        u8 *ou = d->u + (size_t)cy * d->uvs, *ov = d->v + (size_t)cy * d->uvs;
        for (int cx = 0; cx < d->w / 2; cx++) {
            int sx = (mx(xm, 2 * cx) & ~1) * 2;      /* байт пары Y0 U Y1 V */
            ou[cx] = (u8)((r0[sx + 1] + r1[sx + 1] + 1) >> 1);
            ov[cx] = (u8)((r0[sx + 3] + r1[sx + 3] + 1) >> 1);
        }
    }
}
/* NV12: плоскость Y (шаг ss), за ней на uv_off байт — UV вперемежку (шаг ss), высота вдвое меньше */
static void cvt_nv12(const u8 *s, int ss, size_t uv_off, const int *xm, const int *ym, I420 *d) {
    for (int dy = 0; dy < d->h; dy++) {
        const u8 *r = s + (size_t)mx(ym, dy) * ss;
        u8 *o = d->y + (size_t)dy * d->ys;
        if (!xm) memcpy(o, r, (size_t)d->w);
        else for (int dx = 0; dx < d->w; dx++) o[dx] = r[xm[dx]];
    }
    const u8 *uv = s + uv_off;
    for (int cy = 0; cy < d->h / 2; cy++) {
        const u8 *r = uv + (size_t)(mx(ym, 2 * cy) >> 1) * ss;
        u8 *ou = d->u + (size_t)cy * d->uvs, *ov = d->v + (size_t)cy * d->uvs;
        for (int cx = 0; cx < d->w / 2; cx++) { int sx = mx(xm, 2 * cx) & ~1; ou[cx] = r[sx]; ov[cx] = r[sx + 1]; }
    }
}
/* I420 -> I420 с масштабом (после разбора MJPG во временный кадр) */
static void cvt_i420(const I420 *s, const int *xm, const int *ym, I420 *d) {
    for (int dy = 0; dy < d->h; dy++) {
        const u8 *r = s->y + (size_t)mx(ym, dy) * s->ys;
        u8 *o = d->y + (size_t)dy * d->ys;
        if (!xm) memcpy(o, r, (size_t)d->w);
        else for (int dx = 0; dx < d->w; dx++) o[dx] = r[xm[dx]];
    }
    for (int cy = 0; cy < d->h / 2; cy++) {
        int sy = mx(ym, 2 * cy) >> 1;
        const u8 *ru = s->u + (size_t)sy * s->uvs, *rv = s->v + (size_t)sy * s->uvs;
        u8 *ou = d->u + (size_t)cy * d->uvs, *ov = d->v + (size_t)cy * d->uvs;
        for (int cx = 0; cx < d->w / 2; cx++) { int sx = mx(xm, 2 * cx) >> 1; ou[cx] = ru[sx]; ov[cx] = rv[sx]; }
    }
}
static void cam_map(int *m, int sn, int dn) {   /* центр точки результата -> точка источника */
    for (int i = 0; i < dn; i++) { long long v = ((long long)i * 2 + 1) * sn / (2LL * dn); m[i] = (int)(v < sn ? v : sn - 1); }
}
/* размер результата: как у камеры, если влезает в 1280x720, иначе вписать с пропорциями; чётный */
static void cam_out_size(int sw, int sh, int *ow, int *oh) {
    int w = sw, h = sh;
    if (w > CAM_MAXW || h > CAM_MAXH) {
        if ((long long)w * CAM_MAXH > (long long)h * CAM_MAXW) { h = (int)((long long)h * CAM_MAXW / w); w = CAM_MAXW; }
        else { w = (int)((long long)w * CAM_MAXH / h); h = CAM_MAXH; }
    }
    *ow = w & ~1; *oh = h & ~1;
}
/* выбор режима камеры: сначала плавность (>= 24 к/с), затем «влезает в 720p» и крупнее, затем дешевле формат */
static long long cam_score(int fmt, int w, int h, double fps) {
    if (w < 160 || h < 120 || w > 4096 || h > 4096 || (w & 1) || (h & 1) || fps < 1) return -1;
    bool fit = w <= CAM_MAXW && h <= CAM_MAXH;
    /* несжатый поток больше ~30 МБ/с (720p30 YUY2/NV12 = 55 МБ/с) USB 2 не тянет: камера на деле даёт 8-12 к/с */
    double rate = fmt == CF_MJPG ? 0 : (double)w * h * (fmt == CF_YUY2 ? 2.0 : 1.5) * (fps > 30 ? 30 : fps);
    double efps = rate > 30e6 ? 10 : fps;
    long long s = efps >= 24 ? 3 : efps >= 15 ? 2 : 1;
    s = s << 1 | (fit ? 1 : 0);
    s = s << 26 | (fit ? (long long)w * h : 4096LL * 4096 - (long long)w * h);
    s = s << 1 | (fps <= 31 ? 1 : 0);            /* 60 к/с не нужно: лишняя нагрузка на USB и разбор */
    s = s << 2 | (fmt == CF_NV12 ? 3 : fmt == CF_YUY2 ? 2 : 1);
    return s;
}

/* ---------- MJPG: libjpeg-turbo, «сырые» плоскости YCbCr без перевода цвета ---------- */
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } CJErr;
static void cj_exit(j_common_ptr c) { longjmp(((CJErr *)c->err)->jb, 1); }
static void cj_out(j_common_ptr c) { (void)c; }
static void cj_emit(j_common_ptr c, int l) { (void)c; (void)l; }
/* кадр sw x sh -> d (размер d = размер JPEG / den; ширина строк d — с запасом до блока). den 1, 2, 4, 8 — уменьшение
 * прямо в обратном DCT (libjpeg scale_denom): 1280x720 MJPG -> 640x360 почти даром. false — битый кадр (пропустить). */
static bool mjpg_decode_into(const u8 *p, size_t n, I420 *d, u8 *dummy, int sw, int sh, int den) {
    struct jpeg_decompress_struct ci;
    CJErr je;
    memset(&ci, 0, sizeof ci);
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = cj_exit; je.pub.output_message = cj_out; je.pub.emit_message = cj_emit;
    if (setjmp(je.jb)) { jpeg_destroy_decompress(&ci); return false; }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, p, (unsigned long)n);
    if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK || (int)ci.image_width != sw || (int)ci.image_height != sh || ci.num_components != 3
        || ci.comp_info[0].h_samp_factor != 2 || (ci.comp_info[0].v_samp_factor != 1 && ci.comp_info[0].v_samp_factor != 2)
        || ci.comp_info[1].h_samp_factor != 1 || ci.comp_info[1].v_samp_factor != 1 || ci.comp_info[2].h_samp_factor != 1 || ci.comp_info[2].v_samp_factor != 1) {
        jpeg_destroy_decompress(&ci);
        return false;                            /* не 4:2:0 / 4:2:2 — у веб-камер не бывает */
    }
    ci.raw_data_out = TRUE;
    ci.dct_method = JDCT_IFAST;
    ci.out_color_space = JCS_YCbCr;
    ci.scale_num = 1;
    ci.scale_denom = (unsigned)(den == 2 || den == 4 || den == 8 ? den : 1);
    jpeg_start_decompress(&ci);
    int k = ci.min_DCT_scaled_size;              /* строк блока после масштаба: 8, 4, 2, 1 */
    int vs = ci.comp_info[0].v_samp_factor, lines = vs * k;   /* строк яркости за проход */
    if (k < 1 || k > DCTSIZE || (int)ci.output_width > d->ys || (int)ci.output_height > d->h + 16) { jpeg_abort_decompress(&ci); jpeg_destroy_decompress(&ci); return false; }
    JSAMPROW ry[16], ru[8], rv[8];
    JSAMPARRAY pl[3] = { ry, ru, rv };
    while (ci.output_scanline < ci.output_height) {
        int y0 = (int)ci.output_scanline;
        for (int i = 0; i < lines; i++) ry[i] = y0 + i < d->h ? d->y + (size_t)(y0 + i) * d->ys : dummy;
        for (int i = 0; i < k; i++) {
            /* 4:2:0 — строка цветности на две строки яркости; 4:2:2 — на каждую: берём чётные */
            int cr = vs == 2 ? y0 / 2 + i : ((y0 + i) & 1 ? -1 : (y0 + i) / 2);
            ru[i] = cr >= 0 && cr < d->h / 2 ? d->u + (size_t)cr * d->uvs : dummy;
            rv[i] = cr >= 0 && cr < d->h / 2 ? d->v + (size_t)cr * d->uvs : dummy;
        }
        if (!jpeg_read_raw_data(&ci, pl, (JDIMENSION)lines)) break;
    }
    jpeg_abort_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    return true;
}

/* ======================= кодер ======================= */
typedef struct {
    H264E_persist_t *enc;
    H264E_scratch_t *scr;
    size_t pn, sn;
    H264E_run_param_t rp;
    int w, h, fps, kbps;
    i64 last_idr;
} CamEnc;
static void enc_free(CamEnc *e) {
    if (e->enc) { wipe(e->enc, e->pn); free(e->enc); }
    if (e->scr) { wipe(e->scr, e->sn); free(e->scr); }
    memset(e, 0, sizeof *e);
}
static bool enc_make(CamEnc *e, int w, int h, int fps, int kbps, int kbps_max) {
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = w; cp.height = h;
    cp.gop = fps * 8;                            /* ключевые — по таймеру CAM_GOP_MS и по PLI */
    cp.vbv_size_bytes = kbps_max * 1000 / 8;
    cp.const_input_flag = ((w | h) & 15) ? 1 : 0;   /* не кратно 16 (640x360): кодер держит свою копию входа */
    int szp = 0, szs = 0;
    if (H264E_sizeof(&cp, &szp, &szs)) return false;
    e->enc = malloc((size_t)szp); e->scr = malloc((size_t)szs);
    e->pn = (size_t)szp; e->sn = (size_t)szs;
    if (!e->enc || !e->scr || H264E_init(e->enc, &cp)) { enc_free(e); return false; }
    memset(&e->rp, 0, sizeof e->rp);
    e->rp.encode_speed = 10;                     /* камера: движение по всему кадру — самый быстрый режим */
    e->rp.qp_min = 20; e->rp.qp_max = 51;
    e->rp.desired_frame_bytes = kbps * 1000 / 8 / fps;
    e->w = w; e->h = h; e->fps = fps; e->kbps = kbps;
    e->last_idr = -1000000;
    return true;
}

/* ======================= источники ======================= */
typedef struct { int w, h, fmt; double fps; char name[96]; } CamMode;
typedef struct { const u8 *p; size_t n; int stride; size_t uv_off; } CamRaw;
typedef struct CamSrc CamSrc;
static int src_list(CamDevice *out, int max);
static CamSrc *src_open(const char *id, CamMode *m, char *err, size_t errn);
static int src_read(CamSrc *s, CamRaw *r, char *err, size_t errn);   /* 1 — кадр, 0 — пока нет, -1 — сбой */
static void src_done(CamSrc *s);                 /* кадр больше не нужен */
static void src_close(CamSrc *s);

#ifdef _WIN32
/* ---------- Windows: Media Foundation ---------- */
typedef HRESULT (WINAPI *PMFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PMFShutdown)(void);
typedef HRESULT (WINAPI *PMFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *PMFEnumDeviceSources)(IMFAttributes *, IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *PMFCreateDeviceSource)(IMFAttributes *, IMFMediaSource **);
typedef HRESULT (WINAPI *PMFCreateSourceReaderFromMediaSource)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **);
static struct {
    bool tried, ok;
    PMFStartup startup; PMFShutdown shutdown; PMFCreateAttributes attrs;
    PMFEnumDeviceSources enumd; PMFCreateDeviceSource create; PMFCreateSourceReaderFromMediaSource reader;
    HMODULE a, b, c;
} MFL;
static bool mf_load(void) {
    if (MFL.tried) return MFL.ok;
    MFL.tried = true;
    HMODULE a = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE b = LoadLibraryExW(L"mf.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE c = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    MFL.a = a; MFL.b = b; MFL.c = c;
    if (!a || !b || !c) return false;
    MFL.startup = (PMFStartup)(void *)GetProcAddress(a, "MFStartup");
    MFL.shutdown = (PMFShutdown)(void *)GetProcAddress(a, "MFShutdown");
    MFL.attrs = (PMFCreateAttributes)(void *)GetProcAddress(a, "MFCreateAttributes");
    MFL.enumd = (PMFEnumDeviceSources)(void *)GetProcAddress(b, "MFEnumDeviceSources");
    MFL.create = (PMFCreateDeviceSource)(void *)GetProcAddress(b, "MFCreateDeviceSource");
    MFL.reader = (PMFCreateSourceReaderFromMediaSource)(void *)GetProcAddress(c, "MFCreateSourceReaderFromMediaSource");
    MFL.ok = MFL.startup && MFL.shutdown && MFL.attrs && MFL.enumd && MFL.create && MFL.reader;
    return MFL.ok;
}
static void mf_err(HRESULT hr, char *err, size_t n) {
    if (hr == E_ACCESSDENIED)
        snprintf(err, n, "%s", ui_tr("нет доступа к камере: Параметры Windows -> Конфиденциальность -> Камера"));
    else if (hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)
             || hr == HRESULT_FROM_WIN32(ERROR_BUSY))
        snprintf(err, n, "%s", ui_tr("камера занята другой программой"));
    else if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) || hr == MF_E_SHUTDOWN)
        snprintf(err, n, "%s", ui_tr("камера отключена"));
    else if (hr == MF_E_NO_CAPTURE_DEVICES_AVAILABLE)
        snprintf(err, n, "%s", ui_tr("камера не найдена"));
    else snprintf(err, n, ui_tr("камера не открылась (код 0x%08lX)"), (unsigned long)hr);
}
static void w2u(const WCHAR *w, char *o, size_t n) {
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, o, (int)n, NULL, NULL);
    if (k <= 0) o[0] = 0;
    o[n - 1] = 0;
    cam_clean(o);
}
struct CamSrc { IMFMediaSource *ms; IMFSourceReader *rd; IMFSample *smp; IMFMediaBuffer *buf; IMF2DBuffer *b2; bool com; int w, h, fmt; };
static int mf_list(CamDevice *out, int max) {   /* поток с MTA: COM и MF только на время перечня */
    if (!mf_load()) return 0;
    HRESULT ci = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFL.startup(MF_VERSION, MFSTARTUP_LITE))) { if (SUCCEEDED(ci)) CoUninitialize(); return 0; }
    IMFAttributes *a = NULL;
    IMFActivate **acts = NULL;
    UINT32 cnt = 0;
    int n = 0;
    if (SUCCEEDED(MFL.attrs(&a, 1)) && SUCCEEDED(IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
        && SUCCEEDED(MFL.enumd(a, &acts, &cnt))) {
        for (UINT32 i = 0; i < cnt; i++) {
            WCHAR *nm = NULL, *sl = NULL;
            UINT32 len = 0;
            if (n < max && SUCCEEDED(IMFActivate_GetAllocatedString(acts[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &nm, &len))
                && SUCCEEDED(IMFActivate_GetAllocatedString(acts[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sl, &len))) {
                w2u(nm, out[n].name, sizeof out[n].name);
                w2u(sl, out[n].id, sizeof out[n].id);
                if (out[n].id[0]) n++;
            }
            if (nm) CoTaskMemFree(nm);
            if (sl) CoTaskMemFree(sl);
            IMFActivate_Release(acts[i]);
        }
        CoTaskMemFree(acts);
    }
    if (a) IMFAttributes_Release(a);
    MFL.shutdown();
    if (SUCCEEDED(ci)) CoUninitialize();
    return n;
}
static void mf_unload(void);
typedef struct { CamDevice *out; int max, n; } MfListArg;
static int mf_list_thread(void *p) { MfListArg *a = p; a->n = mf_list(a->out, a->max); if (!atomic_load(&C.running)) mf_unload(); return 0; }
static int src_list(CamDevice *out, int max) {  /* главный поток: своё COM-состояние не трогаем — отдельный поток */
    MfListArg a = { out, max, 0 };
    PlatThread *t = thread_start(mf_list_thread, &a);
    if (!t) return 0;
    thread_join(t);
    return a.n;
}
static int mf_fmt(const GUID *g) {
    if (IsEqualGUID(g, &MFVideoFormat_NV12)) return CF_NV12;
    if (IsEqualGUID(g, &MFVideoFormat_YUY2)) return CF_YUY2;
    if (IsEqualGUID(g, &MFVideoFormat_MJPG)) return CF_MJPG;
    return -1;
}
static void mf_unload(void) {                    /* камера выключена: DLL Media Foundation — выгрузить */
    if (MFL.c) FreeLibrary(MFL.c);
    if (MFL.b) FreeLibrary(MFL.b);
    if (MFL.a) FreeLibrary(MFL.a);
    memset(&MFL, 0, sizeof MFL);
}
static void src_close(CamSrc *s) {
    if (!s) return;
    src_done(s);
    if (s->rd) IMFSourceReader_Release(s->rd);
    if (s->ms) { IMFMediaSource_Shutdown(s->ms); IMFMediaSource_Release(s->ms); }
    MFL.shutdown();
    if (s->com) CoUninitialize();
    free(s);
}
static CamSrc *src_open(const char *id, CamMode *m, char *err, size_t errn) {
    if (!mf_load()) { snprintf(err, errn, "%s", ui_tr("Media Foundation недоступна (Windows N без пакета мультимедиа?)")); return NULL; }
    CamSrc *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    s->com = SUCCEEDED(hr);
    if (FAILED(hr = MFL.startup(MF_VERSION, MFSTARTUP_LITE))) { mf_err(hr, err, errn); if (s->com) CoUninitialize(); free(s); return NULL; }
    char first[256] = "";
    if (!id || !*id) {                           /* по умолчанию — первая камера */
        CamDevice d[1];
        if (mf_list(d, 1) < 1) { snprintf(err, errn, "%s", ui_tr("камера не найдена")); src_close(s); return NULL; }
        snprintf(first, sizeof first, "%s", d[0].id);
        snprintf(m->name, sizeof m->name, "%s", d[0].name);
        id = first;
        MFL.startup(MF_VERSION, MFSTARTUP_LITE);  /* mf_list сделал свой MFShutdown */
    } else {
        CamDevice d[16];
        int k = mf_list(d, 16);
        MFL.startup(MF_VERSION, MFSTARTUP_LITE);
        m->name[0] = 0;
        for (int i = 0; i < k; i++) if (!strcmp(d[i].id, id)) snprintf(m->name, sizeof m->name, "%s", d[i].name);
        if (!m->name[0]) { snprintf(err, errn, "%s", ui_tr("выбранная камера отключена")); src_close(s); return NULL; }
    }
    WCHAR wid[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, id, -1, wid, 256)) { snprintf(err, errn, "%s", ui_tr("камера не найдена")); src_close(s); return NULL; }
    IMFAttributes *a = NULL;
    hr = MFL.attrs(&a, 2);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetString(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wid);
    if (SUCCEEDED(hr)) hr = MFL.create(a, &s->ms);
    if (a) { IMFAttributes_Release(a); a = NULL; }
    if (FAILED(hr)) { mf_err(hr, err, errn); src_close(s); return NULL; }
    hr = MFL.attrs(&a, 1);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(a, &MF_READWRITE_DISABLE_CONVERTERS, TRUE);   /* только родные форматы, без MFT */
    if (SUCCEEDED(hr)) hr = MFL.reader(s->ms, a, &s->rd);
    if (a) IMFAttributes_Release(a);
    if (FAILED(hr)) { mf_err(hr, err, errn); src_close(s); return NULL; }
    /* режимы камеры: родные типы первого видеопотока */
    long long best = -1;
    DWORD bi = 0;
    for (DWORD i = 0; i < 512; i++) {
        IMFMediaType *mt = NULL;
        if (FAILED(IMFSourceReader_GetNativeMediaType(s->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &mt))) break;
        GUID sub;
        UINT64 fs = 0, fr = 0;
        if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &sub)) && SUCCEEDED(IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_SIZE, &fs))
            && SUCCEEDED(IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_RATE, &fr))) {
            int f = mf_fmt(&sub), w = (int)(fs >> 32), h = (int)(UINT32)fs;
            UINT32 num = (UINT32)(fr >> 32), den = (UINT32)fr;
            double fps = den ? (double)num / den : 0;
            long long sc = f >= 0 ? cam_score(f, w, h, fps) : -1;
            if (sc > best) { best = sc; bi = i; m->w = w; m->h = h; m->fmt = f; m->fps = fps; }
        }
        IMFMediaType_Release(mt);
    }
    if (best < 0) { snprintf(err, errn, "%s", ui_tr("камера не даёт NV12, YUY2 или MJPG")); src_close(s); return NULL; }
    IMFMediaType *mt = NULL;
    hr = IMFSourceReader_GetNativeMediaType(s->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, bi, &mt);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetStreamSelection(s->rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetStreamSelection(s->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetCurrentMediaType(s->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt);
    if (mt) IMFMediaType_Release(mt);
    if (FAILED(hr)) { mf_err(hr, err, errn); src_close(s); return NULL; }
    s->w = m->w; s->h = m->h; s->fmt = m->fmt;
    return s;
}
static void src_done(CamSrc *s) {
    if (s->b2) { IMF2DBuffer_Unlock2D(s->b2); IMF2DBuffer_Release(s->b2); s->b2 = NULL; }
    else if (s->buf) IMFMediaBuffer_Unlock(s->buf);
    if (s->buf) { IMFMediaBuffer_Release(s->buf); s->buf = NULL; }
    if (s->smp) { IMFSample_Release(s->smp); s->smp = NULL; }
}
static int src_read(CamSrc *s, CamRaw *r, char *err, size_t errn) {
    DWORD idx = 0, fl = 0;
    LONGLONG ts = 0;
    HRESULT hr = IMFSourceReader_ReadSample(s->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &fl, &ts, &s->smp);
    if (FAILED(hr)) { mf_err(hr, err, errn); return -1; }
    if (fl & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) { snprintf(err, errn, "%s", ui_tr("камера отключена")); return -1; }
    if (!s->smp) return 0;                       /* метка потока без кадра */
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s->smp, &s->buf))) { src_done(s); return 0; }
    BYTE *p = NULL;
    DWORD len = 0;
    LONG pitch = 0;
    if (s->fmt != CF_MJPG && SUCCEEDED(IMFMediaBuffer_QueryInterface(s->buf, &IID_IMF2DBuffer, (void **)&s->b2))) {
        if (FAILED(IMF2DBuffer_Lock2D(s->b2, &p, &pitch)) || pitch <= 0) { IMF2DBuffer_Release(s->b2); s->b2 = NULL; p = NULL; }
        else IMFMediaBuffer_GetCurrentLength(s->buf, &len);
    }
    if (!p) {
        if (FAILED(IMFMediaBuffer_Lock(s->buf, &p, NULL, &len))) { IMFMediaBuffer_Release(s->buf); s->buf = NULL; src_done(s); return 0; }
        pitch = s->fmt == CF_YUY2 ? s->w * 2 : s->w;
    }
    r->p = p; r->n = len; r->stride = (int)pitch;
    r->uv_off = (size_t)pitch * s->h;
    if (s->fmt == CF_NV12 && len >= (size_t)pitch * s->h * 3 / 2) {
        size_t ha = (size_t)len * 2 / 3 / (size_t)pitch;   /* высота с выравниванием (1080 -> 1088 у части камер) */
        if (ha >= (size_t)s->h && ha <= (size_t)s->h + 32) r->uv_off = (size_t)pitch * ha;
    }
    return 1;
}
#elif defined(__APPLE__) && defined(ASTRA_CAM_MAC)
/* ---------- macOS: AVFoundation (cam_mac.m) ---------- */
struct CamSrc { CamMac *h; int w, h_; };
typedef struct { CamDevice *out; int n, max; } MacList;
static void mac_add(void *u, const char *id, const char *name) {
    MacList *l = u;
    if (l->n >= l->max || !id || !*id) return;
    snprintf(l->out[l->n].id, sizeof l->out[l->n].id, "%s", id);
    snprintf(l->out[l->n].name, sizeof l->out[l->n].name, "%s", name && *name ? name : ui_tr("камера"));
    cam_clean(l->out[l->n].name);
    l->n++;
}
static int src_list(CamDevice *out, int max) {
    MacList l = { out, 0, max };
    cammac_list(mac_add, &l);
    return l.n;
}
static void mac_err(int e, int code, char *err, size_t n) {
    switch (e) {
    case CAMMAC_E_DENIED: snprintf(err, n, "%s", ui_tr("нет доступа к камере: Системные настройки -> Конфиденциальность и безопасность -> Камера")); break;
    case CAMMAC_E_BUSY: snprintf(err, n, "%s", ui_tr("камера занята другой программой")); break;
    case CAMMAC_E_GONE: case CAMMAC_E_NOTFOUND: snprintf(err, n, "%s", ui_tr("камера отключена")); break;
    case CAMMAC_E_FORMAT: snprintf(err, n, "%s", ui_tr("камера не даёт NV12, YUY2 или MJPG")); break;
    case CAMMAC_E_NOFRAMES: snprintf(err, n, "%s", ui_tr("камера не отдаёт кадры")); break;
    default: snprintf(err, n, ui_tr("камера не открылась (ошибка %d)"), code); break;
    }
}
static void src_close(CamSrc *s) {
    if (!s) return;
    cammac_close(s->h);
    free(s);
}
static CamSrc *src_open(const char *id, CamMode *m, char *err, size_t errn) {
    CamDevice d[16];
    int k = src_list(d, 16);
    int pick = -1;
    for (int i = 0; i < k; i++) if (!id || !*id || !strcmp(d[i].id, id)) { pick = i; break; }
    if (pick < 0) { snprintf(err, errn, "%s", k ? ui_tr("выбранная камера отключена") : ui_tr("камера не найдена")); return NULL; }
    CamSrc *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    CamMacMode mm;
    memset(&mm, 0, sizeof mm);
    int e = 0, code = 0;
    s->h = cammac_open(d[pick].id, CAM_MAXW, CAM_MAXH, &mm, &e, &code);
    if (!s->h) { mac_err(e ? e : CAMMAC_E_OTHER, code, err, errn); free(s); return NULL; }
    snprintf(m->name, sizeof m->name, "%s", d[pick].name);
    m->w = mm.w; m->h = mm.h; m->fps = mm.fps > 0 ? mm.fps : 30; m->fmt = CF_NV12;
    if (m->w < 160 || m->h < 120 || m->w > 4096 || m->h > 4096 || (m->w & 1) || (m->h & 1)) {
        snprintf(err, errn, "%s", ui_tr("камера не даёт NV12, YUY2 или MJPG"));
        src_close(s);
        return NULL;
    }
    s->w = m->w; s->h_ = m->h;
    return s;
}
static void src_done(CamSrc *s) { cammac_done(s->h); }
static int src_read(CamSrc *s, CamRaw *r, char *err, size_t errn) {
    CamMacFrame f;
    int e = 0;
    int k = cammac_read(s->h, &f, 1000, &e);
    if (k < 0) { mac_err(e ? e : CAMMAC_E_GONE, 0, err, errn); return -1; }
    if (k == 0) return 0;
    if (f.w != s->w || f.h != s->h_) { cammac_done(s->h); return 0; }   /* размер сменился на ходу: кадр мимо */
    r->p = f.p; r->n = f.n; r->stride = f.stride; r->uv_off = f.uv_off;
    return 1;
}
#elif defined(__APPLE__)
/* ---------- macOS без cam_mac.m: честный отказ ---------- */
#define CAM_MAC_UNSUPPORTED "камера на macOS пока не поддерживается"
struct CamSrc { int unused; };
static int src_list(CamDevice *out, int max) { (void)out; (void)max; (void)cam_clean; return 0; }
static CamSrc *src_open(const char *id, CamMode *m, char *err, size_t errn) {
    (void)id; (void)m;
    snprintf(err, errn, "%s", ui_tr(CAM_MAC_UNSUPPORTED));
    return NULL;
}
static int src_read(CamSrc *s, CamRaw *r, char *err, size_t errn) {
    (void)s; (void)r;
    snprintf(err, errn, "%s", ui_tr(CAM_MAC_UNSUPPORTED));
    return -1;
}
static void src_done(CamSrc *s) { (void)s; }
static void src_close(CamSrc *s) { (void)s; }
#else
/* ---------- Linux: V4L2 ---------- */
#define V4_NBUF 3
struct CamSrc { int fd; struct { void *p; size_t n; } b[V4_NBUF]; int nb; int cur; bool on; int w, h, fmt, stride; };
static int v4_ioctl(int fd, unsigned long rq, void *a) { int r; do r = ioctl(fd, rq, a); while (r < 0 && errno == EINTR); return r; }
static int v4_fmt(unsigned pf) {
    if (pf == V4L2_PIX_FMT_NV12) return CF_NV12;
    if (pf == V4L2_PIX_FMT_YUYV) return CF_YUY2;
    if (pf == V4L2_PIX_FMT_MJPEG) return CF_MJPG;
    return -1;
}
static const unsigned V4_PF[3] = { V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG };
static bool v4_capture_dev(int fd, char *card, size_t n) {   /* устройство захвата с поддерживаемым форматом */
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (v4_ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) return false;
    unsigned c = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if (!(c & V4L2_CAP_VIDEO_CAPTURE) || !(c & V4L2_CAP_STREAMING)) return false;
    bool ok = false;
    for (unsigned i = 0; i < 64 && !ok; i++) {
        struct v4l2_fmtdesc fd2;
        memset(&fd2, 0, sizeof fd2);
        fd2.index = i; fd2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (v4_ioctl(fd, VIDIOC_ENUM_FMT, &fd2) < 0) break;
        if (v4_fmt(fd2.pixelformat) >= 0) ok = true;
    }
    if (ok && card) { snprintf(card, n, "%.*s", (int)sizeof cap.card, (const char *)cap.card); cam_clean(card); }
    return ok;
}
static int src_list(CamDevice *out, int max) {
    int n = 0;
    for (int i = 0; i < 64 && n < max; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (v4_capture_dev(fd, out[n].name, sizeof out[n].name)) { snprintf(out[n].id, sizeof out[n].id, "%s", p); n++; }
        close(fd);
    }
    return n;
}
static void v4_err(int e, char *err, size_t n) {
    if (e == EBUSY) snprintf(err, n, "%s", ui_tr("камера занята другой программой"));
    else if (e == EACCES || e == EPERM) snprintf(err, n, "%s", ui_tr("нет доступа к камере (пользователь не в группе video?)"));
    else if (e == ENODEV || e == ENXIO || e == ENOENT) snprintf(err, n, "%s", ui_tr("камера отключена"));
    else snprintf(err, n, ui_tr("камера не открылась (ошибка %d)"), e);
}
static void src_close(CamSrc *s) {
    if (!s) return;
    if (s->on) { int t = V4L2_BUF_TYPE_VIDEO_CAPTURE; v4_ioctl(s->fd, VIDIOC_STREAMOFF, &t); }
    for (int i = 0; i < s->nb; i++) if (s->b[i].p && s->b[i].p != MAP_FAILED) munmap(s->b[i].p, s->b[i].n);
    if (s->fd >= 0) {
        struct v4l2_requestbuffers rb;
        memset(&rb, 0, sizeof rb);
        rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP; rb.count = 0;
        v4_ioctl(s->fd, VIDIOC_REQBUFS, &rb);
        close(s->fd);
    }
    free(s);
}
static CamSrc *src_open(const char *id, CamMode *m, char *err, size_t errn) {
    CamDevice d[16];
    int k = src_list(d, 16);
    int pick = -1;
    for (int i = 0; i < k; i++) if (!id || !*id || !strcmp(d[i].id, id)) { pick = i; break; }
    if (pick < 0) { snprintf(err, errn, "%s", k ? ui_tr("выбранная камера отключена") : ui_tr("камера не найдена")); return NULL; }
    CamSrc *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->fd = open(d[pick].id, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0) { v4_err(errno, err, errn); free(s); return NULL; }
    snprintf(m->name, sizeof m->name, "%s", d[pick].name);
    long long best = -1;
    unsigned bpf = 0;
    for (int f = 0; f < 3; f++) {
        for (unsigned i = 0; i < 128; i++) {
            struct v4l2_frmsizeenum fs;
            memset(&fs, 0, sizeof fs);
            fs.index = i; fs.pixel_format = V4_PF[f];
            if (v4_ioctl(s->fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0 || fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                if (i == 0 && fs.type != V4L2_FRMSIZE_TYPE_DISCRETE && v4_ioctl(s->fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
                    /* ступенчатые размеры: просим 1280x720, драйвер подберёт ближайший */
                    long long sc = cam_score(f, 1280, 720, 30);
                    if (sc > best) { best = sc; bpf = V4_PF[f]; m->w = 1280; m->h = 720; m->fps = 30; m->fmt = f; }
                }
                break;
            }
            double fbest = 0;
            for (unsigned j = 0; j < 64; j++) {
                struct v4l2_frmivalenum fi;
                memset(&fi, 0, sizeof fi);
                fi.index = j; fi.pixel_format = V4_PF[f]; fi.width = fs.discrete.width; fi.height = fs.discrete.height;
                if (v4_ioctl(s->fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) < 0) break;
                double fps = fi.type == V4L2_FRMIVAL_TYPE_DISCRETE && fi.discrete.numerator ? (double)fi.discrete.denominator / fi.discrete.numerator : 30;
                if (fps > 31 && fbest >= 24) continue;
                if (fps <= 31 ? fps > fbest || fbest > 31 : fbest == 0) fbest = fps;
            }
            if (fbest <= 0) fbest = 30;
            long long sc = cam_score(f, (int)fs.discrete.width, (int)fs.discrete.height, fbest);
            if (sc > best) { best = sc; bpf = V4_PF[f]; m->w = (int)fs.discrete.width; m->h = (int)fs.discrete.height; m->fps = fbest; m->fmt = f; }
        }
    }
    if (best < 0) { snprintf(err, errn, "%s", ui_tr("камера не даёт NV12, YUY2 или MJPG")); src_close(s); return NULL; }
    struct v4l2_format fm;
    memset(&fm, 0, sizeof fm);
    fm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fm.fmt.pix.width = (unsigned)m->w; fm.fmt.pix.height = (unsigned)m->h;
    fm.fmt.pix.pixelformat = bpf; fm.fmt.pix.field = V4L2_FIELD_NONE;
    if (v4_ioctl(s->fd, VIDIOC_S_FMT, &fm) < 0) { v4_err(errno, err, errn); src_close(s); return NULL; }
    m->fmt = v4_fmt(fm.fmt.pix.pixelformat);
    m->w = (int)fm.fmt.pix.width; m->h = (int)fm.fmt.pix.height;
    if (m->fmt < 0 || m->w < 160 || m->h < 120 || m->w > 4096 || m->h > 4096 || (m->w & 1) || (m->h & 1)) { snprintf(err, errn, "%s", ui_tr("камера не даёт NV12, YUY2 или MJPG")); src_close(s); return NULL; }
    s->w = m->w; s->h = m->h; s->fmt = m->fmt;
    s->cur = -1;
    s->stride = (int)fm.fmt.pix.bytesperline;   /* шаг строки (MJPG — не нужен) */
    if (m->fmt != CF_MJPG && s->stride < (m->fmt == CF_YUY2 ? m->w * 2 : m->w)) s->stride = m->fmt == CF_YUY2 ? m->w * 2 : m->w;
    struct v4l2_streamparm sp;
    memset(&sp, 0, sizeof sp);
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = (unsigned)(m->fps > 30 ? 30 : m->fps < 1 ? 30 : m->fps + 0.5);
    v4_ioctl(s->fd, VIDIOC_S_PARM, &sp);         /* не вышло — темп камеры по умолчанию */
    if (v4_ioctl(s->fd, VIDIOC_G_PARM, &sp) == 0 && sp.parm.capture.timeperframe.numerator)
        m->fps = (double)sp.parm.capture.timeperframe.denominator / sp.parm.capture.timeperframe.numerator;
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = V4_NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (v4_ioctl(s->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) { v4_err(errno ? errno : ENOMEM, err, errn); src_close(s); return NULL; }
    s->nb = (int)(rb.count > V4_NBUF ? V4_NBUF : rb.count);
    for (int i = 0; i < s->nb; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = (unsigned)i;
        if (v4_ioctl(s->fd, VIDIOC_QUERYBUF, &b) < 0) { v4_err(errno, err, errn); src_close(s); return NULL; }
        s->b[i].n = b.length;
        s->b[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, b.m.offset);
        if (s->b[i].p == MAP_FAILED) { s->b[i].p = NULL; v4_err(errno, err, errn); src_close(s); return NULL; }
        if (v4_ioctl(s->fd, VIDIOC_QBUF, &b) < 0) { v4_err(errno, err, errn); src_close(s); return NULL; }
    }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4_ioctl(s->fd, VIDIOC_STREAMON, &t) < 0) { v4_err(errno, err, errn); src_close(s); return NULL; }
    s->on = true;
    return s;
}
static void src_done(CamSrc *s) {
    if (s->cur < 0) return;
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = (unsigned)s->cur;
    v4_ioctl(s->fd, VIDIOC_QBUF, &b);
    s->cur = -1;
}
static int src_read(CamSrc *s, CamRaw *r, char *err, size_t errn) {
    struct pollfd pf = { s->fd, POLLIN, 0 };
    int pr = poll(&pf, 1, 1000);
    if (pr == 0) return 0;
    if (pr < 0) return errno == EINTR ? 0 : -1;
    if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) { snprintf(err, errn, "%s", ui_tr("камера отключена")); return -1; }
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
    if (v4_ioctl(s->fd, VIDIOC_DQBUF, &b) < 0) {
        if (errno == EAGAIN) return 0;
        v4_err(errno, err, errn);
        return -1;
    }
    if (b.index >= (unsigned)s->nb || (b.flags & V4L2_BUF_FLAG_ERROR) || !b.bytesused) { v4_ioctl(s->fd, VIDIOC_QBUF, &b); return 0; }
    s->cur = (int)b.index;
    r->p = s->b[b.index].p;
    r->n = b.bytesused;
    r->stride = s->stride;
    r->uv_off = (size_t)r->stride * s->h;
    return 1;
}
#endif

/* ======================= поток камеры ======================= */
bool voice_cam_ready(void);                     /* voice_cam.c: эфир возможен (сессия, video_ssrc, DAVE с ключами, op 12) */
void voice_cam_enable(bool on, int w, int h, int fps, int kbps_max);   /* voice_cam.c: op 12 и self_video */
void voice_cam_frame(const u8 *annexb, size_t n, bool keyframe, u32 ts90k);
bool voice_cam_call_ok(char *err, size_t n);
bool gcam_want(u64 user, int *pw, int *ph);     /* gui_grid.c: плитка видна, её размер в пикселях */
bool gcam_put(u64 user, const u8 *y, const u8 *u, const u8 *v, int ys, int uvs, int w, int h, int bits);
void gcam_drop(u64 user);
size_t gcam_bytes(void);
size_t voice_cam_rx_bytes(void);

static void cam_fail(const char *e) {
    cam_lock();
    C.st.phase = CAM_FAILED;
    snprintf(C.st.err, sizeof C.st.err, "%s", e);
    cam_unlock();
    wake_signal(g_wake);
}
/* средняя яркость Y (для --camtest: «не чёрный») */
static int y_mean(const I420 *d) {
    unsigned long long s = 0;
    for (int y = 0; y < d->h; y += 8) for (int x = 0; x < d->w; x += 8) s += d->y[(size_t)y * d->ys + x];
    long long n = (long long)((d->h + 7) / 8) * ((d->w + 7) / 8);
    return n ? (int)(s / (unsigned long long)n) : 0;
}
typedef struct { int secs; bool test; i64 first_ms, last_ms; long long frames, enc_frames, bytes; int ymean; double enc_ms; size_t mem_peak; CamMode m; int ow, oh; char err[128]; } CamTest;
static CamTest *t_cam;                           /* только --camtest */

static int cam_thread(void *arg) {
    (void)arg;
    char err[128] = "";
    CamMode m;
    memset(&m, 0, sizeof m);
    char id[256];
    cam_lock(); snprintf(id, sizeof id, "%s", C.sel); int q = C.quality; cam_unlock();
    static const int QW[CAM_Q_N] = { 640, 1280, 854, 640 }, QH[CAM_Q_N] = { 360, 720, 480, 360 };   /* авто — 360p: плитки у собеседников маленькие */
    CAM_MAXW = QW[q >= 0 && q < CAM_Q_N ? q : 0]; CAM_MAXH = QH[q >= 0 && q < CAM_Q_N ? q : 0];
    if (!CAM_JPEG_SIMD) img_jpeg_thread_limit(4u << 20);   /* обычная копия: MJPG 1080p — рабочая память < 1 МБ, предел с запасом */
    CamSrc *s = src_open(id, &m, err, sizeof err);
    if (!s) { cam_fail(err[0] ? err : ui_tr("камера не открылась")); atomic_store(&C.ended, true); atomic_store(&C.running, false); return 0; }
    int ow, oh;
    cam_out_size(m.w, m.h, &ow, &oh);
    bool scale = ow != m.w || oh != m.h;
    int fps = m.fps >= 29 ? 30 : m.fps >= 1 ? (int)(m.fps + 0.5) : 30;
    long long px = (long long)ow * oh;
    int kbps = px >= 1280LL * 720 * 9 / 10 ? 1800 : px >= 640LL * 360 ? 900 : 500;
    int kbps_max = px >= 1280LL * 720 * 9 / 10 ? 2500 : px >= 640LL * 360 ? 1500 : 1000;
    if (fps < 30) kbps = kbps * fps / 30 + 100;
    /* кадр результата (вход кодера): плоскости с запасом 16 по краям для кодера не нужны — minih264 читает в пределах */
    size_t ysz = (size_t)ow * oh, csz = ysz / 4;
    u8 *fb = malloc(ysz + 2 * csz);
    int *xm = scale ? malloc(sizeof(int) * (size_t)ow) : NULL, *ym = scale ? malloc(sizeof(int) * (size_t)oh) : NULL;
    /* MJPG: при масштабе или ширине не кратной 16 — разбор во временный кадр источника */
    /* MJPG ровно вдвое/вчетверо больше нужного (кратно блокам) — уменьшает сам декодер, сразу в кадр результата */
    int mj_den = 1;
    for (int dd = 2; m.fmt == CF_MJPG && scale && dd <= 8 && mj_den == 1; dd *= 2)
        if (m.w == ow * dd && m.h == oh * dd && !(m.w % (16 * dd)) && !(m.h % (8 * dd))) mj_den = dd;
    if (mj_den > 1) scale = false;
    bool mj_tmp = m.fmt == CF_MJPG && (scale || (mj_den == 1 && ((m.w & 15) || (m.h & 15))));
    int tw = (m.w + 15) & ~15, th = (m.h + 15) & ~15;
    size_t tsz = mj_tmp ? (size_t)tw * th * 3 / 2 : 0;
    u8 *tb = mj_tmp ? malloc(tsz) : NULL;
    size_t dsz = (size_t)(tw > ow ? tw : ow) + 64;
    u8 *dummy = m.fmt == CF_MJPG ? malloc(dsz) : NULL;
    if (!fb || (scale && (!xm || !ym)) || (mj_tmp && !tb) || (m.fmt == CF_MJPG && !dummy)) {
        free(fb); free(xm); free(ym); free(tb); free(dummy); src_close(s);
        cam_fail(ui_tr("не хватило памяти для камеры"));
        atomic_store(&C.ended, true); atomic_store(&C.running, false);
        return 0;
    }
    if (scale) { cam_map(xm, m.w, ow); cam_map(ym, m.h, oh); }
    memset(fb, 16, ysz); memset(fb + ysz, 128, 2 * csz);
    I420 d = { fb, fb + ysz, fb + ysz + csz, ow, ow / 2, ow, oh };
    I420 tmp = { tb, tb ? tb + (size_t)tw * th : NULL, tb ? tb + (size_t)tw * th * 5 / 4 : NULL, tw, tw / 2, m.w, m.h };
    atomic_store(&C.bytes, ysz + 2 * csz + tsz + dsz);
    cam_lock();
    C.st.phase = CAM_WAIT_E2EE;
    C.st.w = ow; C.st.h = oh; C.st.fps = fps; C.st.cam_w = m.w; C.st.cam_h = m.h;
    snprintf(C.st.fmt, sizeof C.st.fmt, "%s", CF_NAME[m.fmt]);
    snprintf(C.st.dev, sizeof C.st.dev, "%s", m.name);
    C.st.err[0] = 0;
    cam_unlock();
    wake_signal(g_wake);
    if (!t_cam) voice_cam_enable(true, ow, oh, fps, kbps_max);
    if (t_cam) { t_cam->m = m; t_cam->ow = ow; t_cam->oh = oh; }
    CamEnc e;
    memset(&e, 0, sizeof e);
#if CAM_ANON
    AnonCtx *an = anon_new(ow, oh);             /* флаг «Скрывать глаза» — глобальный (anon_set_default), контекст берёт его сам */
    if (!an) { cam_fail(ui_tr("анонимизация не запустилась: камера выключена")); src_close(s); s = NULL; }
#endif
    i64 t0 = now_ms(), win = t0, last_enc = -1000;
    long long win_bytes = 0, win_sent = 0, frames = 0, sent = 0, dropped = 0;
    bool was_ready = false;
    u64 me = S.me_id;
    while (s && !atomic_load(&C.stop)) {
        CamRaw r;
        int k = src_read(s, &r, err, sizeof err);
        if (k < 0) { cam_fail(err[0] ? err : ui_tr("камера отключена")); break; }
        if (k == 0) continue;
        frames++;
        i64 now = now_ms();
        if (t_cam) { if (!t_cam->first_ms) t_cam->first_ms = now; t_cam->last_ms = now; }
        int pw = 0, ph = 0;
        bool full = !t_cam && atomic_load(&C.full) == me && gvid_wanted();
        bool prev = !t_cam && (gcam_want(me, &pw, &ph) || full);
        bool ready = t_cam ? true : voice_cam_ready();
        if (ready && !was_ready) atomic_store(&C.idr, true);   /* начало эфира — сразу ключевой */
        was_ready = ready;
        /* темп эфира не выше fps (камера на 60 к/с — каждый второй) */
        bool enc_now = ready && now - last_enc >= 1000 / fps - 4;
        bool ok = true;
        if (prev || enc_now) {
            if (m.fmt == CF_YUY2) { if (r.n >= (size_t)r.stride * (m.h - 1) + (size_t)m.w * 2) cvt_yuy2(r.p, r.stride, xm, ym, &d); else ok = false; }
            else if (m.fmt == CF_NV12) { if (r.n >= r.uv_off + (size_t)r.stride * (m.h / 2 - 1) + (size_t)m.w) cvt_nv12(r.p, r.stride, r.uv_off, xm, ym, &d); else ok = false; }
            else if (mj_tmp) { ok = mjpg_decode_into(r.p, r.n, &tmp, dummy, m.w, m.h, 1); if (ok) cvt_i420(&tmp, xm, ym, &d); }
            else ok = mjpg_decode_into(r.p, r.n, &d, dummy, m.w, m.h, mj_den);
        }
        src_done(s);
        if (!ok) { dropped++; continue; }
        bool logo = false;
#if CAM_ANON
        if (prev || enc_now) {                   /* ровно то, что уйдёт в эфир, и то же — в предпросмотр */
            /* ANON_SEND — глаза закрыты (или анонимизация выключена); ANON_LOGO — кадр целиком заменён логотипом (без
             * пикселей камеры): кодируем и шлём, а себе — предупреждение anon_preview_warning (после кодера);
             * ANON_DROP — ничего не шлём и не показываем */
            int ar = anon_frame(an, d.y, d.u, d.v, d.ys, d.uvs, d.w, d.h);
            if (ar != ANON_SEND && ar != ANON_LOGO) { dropped++; continue; }
            logo = ar == ANON_LOGO;
        }
#endif
        if (t_cam && frames % 8 == 1) t_cam->ymean = y_mean(&d);
        if (prev && !logo) {                     /* до кодера: кодер портит вход */
            if (pw) gcam_put(me, d.y, d.u, d.v, d.ys, d.uvs, d.w, d.h, 8);
            if (full) gvid_put_i420(d.y, d.u, d.v, d.ys, d.uvs, d.w, d.h);
        }
        if (enc_now) {
            if (!e.enc && !enc_make(&e, ow, oh, fps, kbps, kbps_max)) { cam_fail(ui_tr("кодер видео не создан")); break; }
            atomic_store(&C.bytes, ysz + 2 * csz + tsz + dsz + e.pn + e.sn);
            bool key = atomic_exchange(&C.idr, false) || now - e.last_idr >= CAM_GOP_MS;
            e.rp.frame_type = key ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
            if (key) e.last_idr = now;
            H264E_io_yuv_t io;
            io.yuv[0] = d.y; io.yuv[1] = d.u; io.yuv[2] = d.v;
            io.stride[0] = d.ys; io.stride[1] = d.uvs; io.stride[2] = d.uvs;
            unsigned char *out = NULL;
            int n = 0;
            long long c0 = plat_now_us();
            int er = H264E_encode(e.enc, e.scr, &e.rp, &io, &out, &n);
            if (t_cam) { t_cam->enc_ms += (double)(plat_now_us() - c0) / 1000.0; t_cam->enc_frames++; }
            last_enc = now;
            if (!er && n > 0) {
                if (!t_cam) voice_cam_frame(out, (size_t)n, key, (u32)((now - t0) * 90));
                else t_cam->bytes += n;
                sent++; win_sent++; win_bytes += n;
            }
        }
#if CAM_ANON
        if (prev && logo) {                      /* себе — предупреждение (без пикселей камеры), не логотип */
            if (anon_preview_warning(an, d.y, d.u, d.v, d.ys, d.uvs, d.w, d.h)) {
                if (pw) gcam_put(me, d.y, d.u, d.v, d.ys, d.uvs, d.w, d.h, 8);
                if (full) gvid_put_i420(d.y, d.u, d.v, d.ys, d.uvs, d.w, d.h);
            } else gcam_drop(me);
        }
#endif
        if (now - win >= 1000) {
            cam_lock();
            C.st.fps_out = (double)win_sent * 1000.0 / (double)(now - win);
            C.st.kbps = (double)win_bytes * 8.0 / (double)(now - win);
            C.st.frames = frames; C.st.sent = sent; C.st.dropped = dropped;
            if (C.st.phase == CAM_WAIT_E2EE || C.st.phase == CAM_LIVE) C.st.phase = ready ? CAM_LIVE : CAM_WAIT_E2EE;
            cam_unlock();
            win = now; win_bytes = win_sent = 0;
            wake_signal(g_wake);
            if (t_cam) { size_t pm = plat_private_bytes(); if (pm > t_cam->mem_peak) t_cam->mem_peak = pm; }
        } else if (ready != (C.st.phase == CAM_LIVE)) {   /* смена фазы — сразу (чтение без блокировки: только для решения) */
            cam_lock();
            if (C.st.phase == CAM_WAIT_E2EE || C.st.phase == CAM_LIVE) C.st.phase = ready ? CAM_LIVE : CAM_WAIT_E2EE;
            cam_unlock();
            wake_signal(g_wake);
        }
        if (t_cam && now - t0 >= t_cam->secs * 1000) break;
    }
    if (t_cam) { t_cam->frames = frames; snprintf(t_cam->err, sizeof t_cam->err, "%s", C.st.phase == CAM_FAILED ? C.st.err : ""); }
    src_close(s);                                /* камера закрыта до затирания буферов */
#ifdef _WIN32
    mf_unload();                                 /* выключенная камера — ни потоков, ни DLL Media Foundation */
#endif
#if CAM_ANON
    if (an) anon_free(an);
#endif
    enc_free(&e);
    wipe(fb, ysz + 2 * csz); free(fb);
    if (tb) { wipe(tb, tsz); free(tb); }
    if (dummy) { wipe(dummy, dsz); free(dummy); }
    free(xm); free(ym);
    atomic_store(&C.bytes, 0);
    if (!t_cam) { voice_cam_enable(false, 0, 0, 0, 0); gcam_drop(me); }
    atomic_store(&C.ended, true);
    atomic_store(&C.running, false);
    wake_signal(g_wake);
    return 0;
}

/* ======================= API (cam.h) ======================= */
int cam_list(CamDevice *out, int max) { return max > 0 && out ? src_list(out, max) : 0; }
void cam_select(const char *id) {
    cam_lock();
    bool changed = strcmp(C.sel, id ? id : "") != 0;
    snprintf(C.sel, sizeof C.sel, "%s", id ? id : "");
    cam_unlock();
    if (changed && atomic_load(&C.running)) {   /* включена — переключить на новую */
        char e[128];
        cam_stop();
        cam_start(e, sizeof e);
    }
}
const char *cam_selected(void) { return C.sel; }
const char *cam_quality_name(int q) {
    switch (q) {
    case CAM_Q_AUTO: return ui_tr("Авто (360p)");
    case CAM_Q_720: return "720p";
    case CAM_Q_480: return "480p";
    case CAM_Q_360: return "360p";
    default: return "";
    }
}
int cam_quality(void) { return C.quality; }
void cam_quality_set(int q) {
    if (q < 0 || q >= CAM_Q_N || q == C.quality) return;
    C.quality = q;
    if (atomic_load(&C.running)) { char e[128]; cam_stop(); cam_start(e, sizeof e); }   /* включена — с новым качеством */
}
void cam_mode_set(int q) { cam_quality_set(q); }
u64 cam_full_user(void) { return atomic_load(&C.full); }
void cam_want(u64 user, int mode) {
    if (!user) return;
    if (mode == CAM_FULL) {
        if (atomic_load(&C.full) != user) { gvid_open(); atomic_store(&C.full, user); cam_request_idr(); }
    } else if (atomic_load(&C.full) == user) {
        atomic_store(&C.full, 0);
        gvid_close();                            /* растр крупного вида — затереть (трансляция, если смотрят, заполнит снова) */
    }
}
#ifdef _WIN32
/* Параметры -> Конфиденциальность -> Камера: доступ запрещён целиком или классическим программам — сразу понятная
 * ошибка, камеру даже не открываем (иначе Media Foundation отказывает уже в потоке захвата) */
static bool win_cam_denied(void) {
    static const WCHAR *const K[3] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam\\NonPackaged" };
    static const HKEY H[3] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, HKEY_CURRENT_USER };
    for (int i = 0; i < 3; i++) {
        WCHAR v[16];
        DWORD n = sizeof v, t = 0;
        if (RegGetValueW(H[i], K[i], L"Value", RRF_RT_REG_SZ, &t, v, &n) == ERROR_SUCCESS && !wcscmp(v, L"Deny")) return true;
    }
    return false;
}
#endif
static void cam_join(void) {
    if (!C.thr) return;
    atomic_store(&C.stop, true);
    thread_join(C.thr);
    C.thr = NULL;
    atomic_store(&C.ended, false);
}
bool cam_start(char *err, size_t errn) {
    if (atomic_load(&C.running)) return true;
    cam_join();
    if (O.demo) { snprintf(err, errn, "%s", ui_tr("камера недоступна в демо")); return false; }
    if (!voice_cam_call_ok(err, errn)) return false;
#ifdef _WIN32
    if (win_cam_denied()) { snprintf(err, errn, "%s", ui_tr("нет доступа к камере: Параметры Windows -> Конфиденциальность -> Камера")); return false; }
#endif
#if defined(__APPLE__) && !defined(ASTRA_CAM_MAC)
    snprintf(err, errn, "%s", ui_tr(CAM_MAC_UNSUPPORTED));   /* поток даже не запускаем */
    return false;
#endif
    cam_lock();
    memset(&C.st, 0, sizeof C.st);
    C.st.phase = CAM_STARTING;
    cam_unlock();
    atomic_store(&C.stop, false);
    atomic_store(&C.ended, false);
    atomic_store(&C.idr, true);
    atomic_store(&C.running, true);
    C.thr = thread_start(cam_thread, NULL);
    if (!C.thr) {
        atomic_store(&C.running, false);
        cam_lock(); C.st.phase = CAM_OFF; cam_unlock();
        snprintf(err, errn, "%s", ui_tr("не удалось запустить поток"));
        return false;
    }
    /* камера открывается быстро (< 1 с): ошибку открытия (занята, нет доступа, отключена) — сразу ответом */
    for (int i = 0; i < 75; i++) {
        cam_lock();
        int ph = C.st.phase;
        cam_unlock();
        if (ph != CAM_STARTING) break;
        plat_sleep_ms(20);
    }
    cam_lock();
    bool failed = C.st.phase == CAM_FAILED;
    if (failed) snprintf(err, errn, "%s", C.st.err);
    cam_unlock();
    if (failed) { cam_join(); cam_lock(); memset(&C.st, 0, sizeof C.st); cam_unlock(); return false; }
    return true;
}
void cam_stop(void) {
    cam_join();
    cam_lock();
    memset(&C.st, 0, sizeof C.st);
    cam_unlock();
}
bool cam_active(void) {
    if (atomic_load(&C.ended)) cam_join();       /* поток кончился сам (ошибка): собрать, фаза FAILED остаётся */
    return atomic_load(&C.running);
}
void cam_state(CamState *out) {
    if (atomic_load(&C.ended)) cam_join();
    cam_lock();
    *out = C.st;
    cam_unlock();
}
void cam_request_idr(void) { atomic_store(&C.idr, true); }
void cam_on_voice_end(void) { if (C.thr) cam_stop(); }
void cam_stop_nowait(void) { atomic_store(&C.stop, true); }   /* паника: процесс сейчас завершится */
size_t cam_bytes(void) { return atomic_load(&C.bytes) + gcam_bytes() + voice_cam_rx_bytes(); }

/* --camtest=СЕКУНД: короткая проверка камеры без сети (только числа; кадры не сохраняются и не показываются) */
int cam_test(int secs, const char *id) {
    char line[320];
#if CAM_ANON
    snprintf(line, sizeof line, "camtest: анонимизация %s\n", anon_default() ? "включена" : "выключена");
    term_puts(line);
#endif
    CamDevice d[16];
    int n = cam_list(d, 16);
    snprintf(line, sizeof line, "camtest: камер %d\n", n);
    term_puts(line);
#if defined(__APPLE__) && !defined(ASTRA_CAM_MAC)
    term_puts("camtest: " CAM_MAC_UNSUPPORTED "\n");
#endif
    for (int i = 0; i < n; i++) { snprintf(line, sizeof line, "  %d: %s\n", i, d[i].name); term_puts(line); }
    if (!n) return 1;
    if (id && *id) cam_select(id);
    static CamTest t;
    memset(&t, 0, sizeof t);
    t.secs = secs < 1 ? 1 : secs > 10 ? 10 : secs;
    t_cam = &t;
    size_t m0 = plat_private_bytes();
    long long cpu0 = plat_thread_cpu_us();
    (void)cpu0;
    i64 w0 = now_ms();
    atomic_store(&C.stop, false);
    atomic_store(&C.running, true);
    long long pc0 = 0;
#ifdef _WIN32
    FILETIME a, b, k0, u0;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k0, &u0);
    pc0 = (long long)(((ULONGLONG)k0.dwHighDateTime << 32 | k0.dwLowDateTime) + ((ULONGLONG)u0.dwHighDateTime << 32 | u0.dwLowDateTime)) / 10;
#else
    pc0 = (long long)plat_cpu_ms() * 1000;
#endif
    C.thr = thread_start(cam_thread, NULL);
    if (C.thr) { thread_join(C.thr); C.thr = NULL; }
    long long pc1;
#ifdef _WIN32
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k0, &u0);
    pc1 = (long long)(((ULONGLONG)k0.dwHighDateTime << 32 | k0.dwLowDateTime) + ((ULONGLONG)u0.dwHighDateTime << 32 | u0.dwLowDateTime)) / 10;
#else
    pc1 = (long long)plat_cpu_ms() * 1000;
#endif
    i64 wall = now_ms() - w0;
    plat_trim();
    size_t m1 = plat_private_bytes();
    if (t.err[0]) { snprintf(line, sizeof line, "camtest: ошибка: %s\n", t.err); term_puts(line); }
    snprintf(line, sizeof line, "camtest: режим камеры %s %dx%d %.2f к/с -> кадр %dx%d\n", t.m.fmt >= 0 && t.m.fmt < 3 ? CF_NAME[t.m.fmt] : "?", t.m.w, t.m.h, t.m.fps, t.ow, t.oh);
    term_puts(line);
    double fs = wall > 0 ? (double)t.frames * 1000.0 / (double)wall : 0;
    snprintf(line, sizeof line, "camtest: %lld кадров за %lld мс (%.1f к/с), средняя яркость Y %d (%s)\n", t.frames, (long long)wall, fs, t.ymean, t.ymean > 20 ? "не чёрный" : "тёмный/чёрный");
    term_puts(line);
    if (t.frames > 1 && t.last_ms > t.first_ms) {
        snprintf(line, sizeof line, "camtest: открытие %lld мс, темп после первого кадра %.1f к/с\n", (long long)(t.first_ms - w0), (double)(t.frames - 1) * 1000.0 / (double)(t.last_ms - t.first_ms));
        term_puts(line);
    }
    snprintf(line, sizeof line, "camtest: кодер %lld кадров, %.2f мс/кадр, %.0f кбит/с\n", t.enc_frames, t.enc_frames ? t.enc_ms / (double)t.enc_frames : 0,
             wall > 0 ? (double)t.bytes * 8.0 / (double)wall : 0);
    term_puts(line);
    snprintf(line, sizeof line, "camtest: частная память до %.1f МБ, пик %.1f МБ (+%.1f), после %.1f МБ; CPU процесса %.1f %% ядра\n",
             (double)m0 / 1048576.0, (double)t.mem_peak / 1048576.0, t.mem_peak > m0 ? (double)(t.mem_peak - m0) / 1048576.0 : 0.0,
             (double)m1 / 1048576.0, wall > 0 ? (double)(pc1 - pc0) / 10.0 / (double)wall : 0);
    term_puts(line);
    t_cam = NULL;
    atomic_store(&C.ended, false);
    return t.frames > 0 && !t.err[0] ? 0 : 1;
}

/* ======================= selftest ======================= */
int cam_selftest(void (*check)(bool ok, const char *what)) {
    char w[400];
    /* YUY2 8x4 -> I420 один к одному: Y — чётные байты, U/V — среднее двух строк */
    {
        u8 s[4][16], Y[32], U[8], V[8];
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            s[y][4 * x] = (u8)(10 * y + 2 * x); s[y][4 * x + 1] = (u8)(100 + y * 10 + x);
            s[y][4 * x + 2] = (u8)(10 * y + 2 * x + 1); s[y][4 * x + 3] = (u8)(200 - y * 10 - x);
        }
        I420 d = { Y, U, V, 8, 4, 8, 4 };
        cvt_yuy2(&s[0][0], 16, NULL, NULL, &d);
        bool ok = Y[0] == 0 && Y[1] == 1 && Y[7] == 7 && Y[8 * 3 + 5] == 35;
        ok = ok && U[0] == (u8)((100 + 110 + 1) / 2) && V[0] == (u8)((200 + 190 + 1) / 2) && U[4 + 3] == (u8)((123 + 133 + 1) / 2);
        check(ok, "камера: YUY2 -> I420 (яркость по точкам, цветность — среднее двух строк)");
    }
    /* NV12 с шагом больше ширины и выровненной высотой плоскости */
    {
        u8 s[16 * 6 + 16 * 3], Y[32], U[8], V[8];
        memset(s, 0xEE, sizeof s);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) s[y * 16 + x] = (u8)(y * 8 + x);
        size_t uo = 16 * 6;                      /* высота плоскости 6 (выравнивание), UV — после неё */
        for (int y = 0; y < 2; y++) for (int x = 0; x < 4; x++) { s[uo + y * 16 + 2 * x] = (u8)(50 + y * 4 + x); s[uo + y * 16 + 2 * x + 1] = (u8)(150 + y * 4 + x); }
        I420 d = { Y, U, V, 8, 4, 8, 4 };
        cvt_nv12(s, 16, uo, NULL, NULL, &d);
        bool ok = Y[0] == 0 && Y[31] == 31 && U[0] == 50 && V[0] == 150 && U[7] == 57 && V[5] == 155;
        check(ok, "камера: NV12 (шаг 16, высота плоскости 6) -> I420");
    }
    /* масштаб 1920x1080 -> 1280x720 (индексы в пределах), 640x480 — как есть, режимы */
    {
        int ow, oh, ow2, oh2, ow3, oh3;
        cam_out_size(1920, 1080, &ow, &oh); cam_out_size(640, 480, &ow2, &oh2); cam_out_size(1600, 1200, &ow3, &oh3);
        int xm[1280];
        cam_map(xm, 1920, 1280);
        bool inr = xm[0] == 0 && xm[1279] == 1919 && xm[640] >= 959 && xm[640] <= 961;
        long long a = cam_score(CF_MJPG, 1280, 720, 30), b = cam_score(CF_YUY2, 1280, 720, 10), c = cam_score(CF_YUY2, 640, 480, 30),
                  dd = cam_score(CF_MJPG, 1920, 1080, 30), e = cam_score(CF_NV12, 1280, 720, 30), f = cam_score(CF_MJPG, 1280, 720, 60),
                  g = cam_score(CF_NV12, 640, 480, 30), h = cam_score(CF_MJPG, 640, 480, 30);
        snprintf(w, sizeof w, "камера: 1920x1080 -> %dx%d, 1600x1200 -> %dx%d, 640x480 как есть; режим: MJPG 720p30 > несжатый 720p30 (USB 2) > 480p30 > 720p10, 1080p — если меньше нет", ow, oh, ow3, oh3);
        check(ow == 1280 && oh == 720 && ow2 == 640 && oh2 == 480 && ow3 == 960 && oh3 == 720 && inr && a > c && c > b && c > dd && a > e && a > f && g > h, w);
    }
    /* MJPG: синтетический JPEG 4:2:2 (libjpeg-сжатие в памяти) -> разбор в I420 -> средние совпадают */
    {
        int W = 64, H = 48;
        u8 *rgb = malloc((size_t)W * H * 3);
        unsigned char *jb = NULL;
        unsigned long jn = 0;
        bool made = false;
        if (rgb) {
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { u8 *p = rgb + ((size_t)y * W + x) * 3; p[0] = (u8)(x * 4); p[1] = (u8)(y * 5); p[2] = 128; }
            struct jpeg_compress_struct cc;
            CJErr je;
            memset(&cc, 0, sizeof cc);
            cc.err = jpeg_std_error(&je.pub);
            je.pub.error_exit = cj_exit; je.pub.output_message = cj_out;
            if (!setjmp(je.jb)) {
                jpeg_create_compress(&cc);
                jpeg_mem_dest(&cc, &jb, &jn);
                cc.image_width = (JDIMENSION)W; cc.image_height = (JDIMENSION)H; cc.input_components = 3; cc.in_color_space = JCS_RGB;
                jpeg_set_defaults(&cc);
                jpeg_set_quality(&cc, 95, TRUE);
                cc.comp_info[0].h_samp_factor = 2; cc.comp_info[0].v_samp_factor = 1;   /* 4:2:2, как у веб-камер */
                jpeg_start_compress(&cc, TRUE);
                while (cc.next_scanline < cc.image_height) { JSAMPROW row = rgb + (size_t)cc.next_scanline * W * 3; jpeg_write_scanlines(&cc, &row, 1); }
                jpeg_finish_compress(&cc);
                made = jb && jn > 0;
            }
            jpeg_destroy_compress(&cc);
        }
        u8 Y[64 * 48], U[32 * 24], V[32 * 24], dm[128];
        I420 d = { Y, U, V, 64, 32, 64, 48 };
        bool dec = made && mjpg_decode_into(jb, jn, &d, dm, W, H, 1);
        /* ожидание: Y = BT.601 полного диапазона JPEG (0..255) в точке (40, 20) */
        int ey = (int)(0.299 * 160 + 0.587 * 100 + 0.114 * 128 + 0.5), gy = Y[20 * 64 + 40];
        int eu = (int)(-0.168736 * 160 - 0.331264 * 100 + 0.5 * 128 + 128.5), gu = U[10 * 32 + 20];
        /* DCT-масштаб 1/2: 64x48 -> 32x24 прямо в плоскости; точка (20, 10) = исходная (40, 20) */
        u8 Y2[32 * 24], U2[16 * 12], V2[16 * 12];
        I420 d2 = { Y2, U2, V2, 32, 16, 32, 24 };
        bool dec2 = made && mjpg_decode_into(jb, jn, &d2, dm, W, H, 2);
        int gy2 = Y2[10 * 32 + 20];
        snprintf(w, sizeof w, "камера: MJPG 4:2:2 64x48 (%lu Б) -> I420: Y %d (ждём %d), U %d (ждём %d)", jn, gy, ey, gu, eu);
        check(dec && abs(gy - ey) <= 4 && abs(gu - eu) <= 4, w);
        snprintf(w, sizeof w, "камера: MJPG с уменьшением в DCT (1/2) 64x48 -> 32x24: Y %d (ждём %d)", gy2, ey);
        check(dec2 && abs(gy2 - ey) <= 6, w);
        bool trunc_ok = !made || (mjpg_decode_into(jb, jn / 2, &d, dm, W, H, 1), true);
        u8 junk[64] = { 0xFF, 0xD8, 0xFF, 0xC0, 0, 0 };
        check(trunc_ok && !mjpg_decode_into(junk, sizeof junk, &d, dm, W, H, 1), "камера: битый MJPG (обрезанный, мусор) — без падения, мусор отвергнут");
        free(jb);
        if (rgb) free(rgb);
    }
    /* без звонка камера не включается и ничего не открывает */
    {
        char e[128] = "";
        bool st = cam_start(e, sizeof e);
        CamState cs;
        cam_state(&cs);
        snprintf(w, sizeof w, "камера: без звонка cam_start отказывает (\"%s\"), камера не открыта", e);
        check(!st && e[0] && !cam_active() && cs.phase == CAM_OFF, w);
    }
    return 0;
}
#endif
