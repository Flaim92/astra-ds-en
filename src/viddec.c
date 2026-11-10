/* viddec.c — «Astra DS Video»: видео-декодер просмотра чужой трансляции (только -DASTRA_VIDEO).
 * H.264 (все профили 4:2:0 8/10 бит, с B-кадрами), H.265, VP8, VP9 — FFmpeg libavcodec минимальной сборки
 * (deps/build-deps.sh build_ffmpeg); AV1 — dav1d. Однопоточно (libavcodec thread_count = 1, dav1d n_threads = 1,
 * max_frame_delay = 1): меньше памяти и задержки, поток — у зрителя свой (voice_stream.c). Кадры декодера
 * (самая большая память) выделяются здесь же (get_buffer2 / Dav1dPicAllocator): учёт байт и затирание при
 * освобождении (в них чужой экран). Журнал av_log выключен. Объявления — golive.h. */
#ifdef ASTRA_VIDEO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <dav1d/dav1d.h>
#include "mc.h"
#include "plat.h"
#include "golive.h"

#define VD_MAX_PIX (2560 * 1600)                /* больше — «не поддерживается»: память опорных кадров */
#define VD_ALIGN 64

/* ---------- память кадров: пул, учёт и затирание ----------
 * Освобождённый кадр не отдаётся системе, а ложится в пул (до VD_POOL блоков) и берётся следующим кадром того же
 * размера: без пула каждый кадр — новое выделение 1,4 МБ (~350 ошибок страниц, 1,1 -> 0,7 мс/кадр по замеру
 * tools/perf/watch/decbench). Затирается и освобождается пул при vdec_free и при смене размера кадра.
 * Декодер в каждый момент один (поток зрителя), пул общий. vd_pic_bytes — в работе + в пуле. */
#define VD_POOL 1                               /* живой замер: в установившемся режиме нужен один свободный блок; 4 держали до 4 лишних кадров по 1,4 МБ */
static atomic_size_t vd_pic_bytes;
static atomic_int vd_frames;                    /* кадров декодера сейчас (в работе + в пуле) — для журнала/индикатора */
static struct { u8 *p; size_t n; } vd_pool[VD_POOL];
static int vd_npool;
/* пул общий для декодера зрителя трансляции и декодера камер (разные потоки): короткая блокировка */
static atomic_flag vd_lk = ATOMIC_FLAG_INIT;
static void vd_lock(void) { while (atomic_flag_test_and_set_explicit(&vd_lk, memory_order_acquire)) {} }
static void vd_unlock(void) { atomic_flag_clear_explicit(&vd_lk, memory_order_release); }
static void vd_os_free(u8 *p, size_t n) {       /* p — данные (за заголовком) */
    wipe(p, n);
    atomic_fetch_sub(&vd_pic_bytes, n);
    atomic_fetch_sub(&vd_frames, 1);
#ifdef _WIN32
    _aligned_free(p - VD_ALIGN);
#else
    free(p - VD_ALIGN);
#endif
}
static void vd_pool_drain(void) { vd_lock(); while (vd_npool) { vd_npool--; vd_os_free(vd_pool[vd_npool].p, vd_pool[vd_npool].n); } vd_unlock(); }
static void *vd_alloc(size_t n) {               /* выравнивание 64, перед блоком — его размер */
    if (n > (256u << 20)) return NULL;
    vd_lock();
    for (int i = 0; i < vd_npool; i++) if (vd_pool[i].n == n) { u8 *p = vd_pool[i].p; vd_pool[i] = vd_pool[--vd_npool]; vd_unlock(); return p; }
    for (int i = vd_npool - 1; i >= 0; i--) { vd_os_free(vd_pool[i].p, vd_pool[i].n); vd_pool[i] = vd_pool[--vd_npool]; }   /* другой размер — старое мимо */
    vd_unlock();
    u8 *b;
#ifdef _WIN32
    b = _aligned_malloc(n + VD_ALIGN, VD_ALIGN);
#else
    void *q = NULL;
    b = posix_memalign(&q, VD_ALIGN, n + VD_ALIGN) ? NULL : q;
#endif
    if (!b) return NULL;
    memcpy(b, &n, sizeof n);
    atomic_fetch_add(&vd_pic_bytes, n);
    atomic_fetch_add(&vd_frames, 1);
    return b + VD_ALIGN;
}
static void vd_release(void *p) {
    if (!p) return;
    size_t n;
    memcpy(&n, (u8 *)p - VD_ALIGN, sizeof n);
    vd_lock();
    if (vd_npool < VD_POOL) { vd_pool[vd_npool].p = p; vd_pool[vd_npool].n = n; vd_npool++; vd_unlock(); return; }
    vd_unlock();
    vd_os_free(p, n);
}
size_t vdec_bytes(void) { return atomic_load(&vd_pic_bytes); }
int vdec_frames(void) { return atomic_load(&vd_frames); }

/* libavcodec: свой get_buffer2 — как avcodec_default_get_buffer2, но одним блоком из vd_alloc */
static void av_buf_free(void *opaque, uint8_t *data) { (void)opaque; vd_release(data); }
static int av_get_buf(AVCodecContext *c, AVFrame *f, int flags) {
    if (!(c->codec->capabilities & AV_CODEC_CAP_DR1)) return avcodec_default_get_buffer2(c, f, flags);
    int w = f->width, h = f->height, la[AV_NUM_DATA_POINTERS];
    avcodec_align_dimensions2(c, &w, &h, la);
    int ls[4] = { 0 };
    if (av_image_fill_linesizes(ls, f->format, w) < 0) return AVERROR(EINVAL);
    ptrdiff_t pls[4];
    for (int i = 0; i < 4; i++) { ls[i] = (ls[i] + VD_ALIGN - 1) & ~(VD_ALIGN - 1); pls[i] = ls[i]; }
    size_t sz[4] = { 0 };
    if (av_image_fill_plane_sizes(sz, f->format, h, pls) < 0) return AVERROR(EINVAL);
    size_t off[4], tot = 0;
    for (int i = 0; i < 4; i++) { off[i] = tot; if (sz[i]) tot += (sz[i] + 16 + VD_ALIGN * 2 - 1) & ~(size_t)(VD_ALIGN - 1); }
    u8 *b = vd_alloc(tot);
    if (!b) return AVERROR(ENOMEM);
    f->buf[0] = av_buffer_create(b, (size_t)tot, av_buf_free, NULL, 0);
    if (!f->buf[0]) { vd_release(b); return AVERROR(ENOMEM); }
    for (int i = 0; i < 4; i++) { f->data[i] = sz[i] ? b + off[i] : NULL; f->linesize[i] = sz[i] ? ls[i] : 0; }
    f->extended_data = f->data;
    return 0;
}
/* dav1d: свой распределитель кадров — раскладка как у dav1d_default_picture_alloc */
static int d1_alloc(Dav1dPicture *p, void *cookie) {
    (void)cookie;
    int hbd = p->p.bpc > 8, aw = (p->p.w + 127) & ~127, ah = (p->p.h + 127) & ~127;
    int chroma = p->p.layout != DAV1D_PIXEL_LAYOUT_I400, ssv = p->p.layout == DAV1D_PIXEL_LAYOUT_I420, ssh = p->p.layout != DAV1D_PIXEL_LAYOUT_I444;
    ptrdiff_t ys = (ptrdiff_t)aw << hbd, uvs = chroma ? ys >> ssh : 0;
    if (!(ys & 1023)) ys += DAV1D_PICTURE_ALIGNMENT;
    if (chroma && !(uvs & 1023)) uvs += DAV1D_PICTURE_ALIGNMENT;
    size_t ysz = (size_t)ys * ah, uvsz = (size_t)uvs * (ah >> ssv);
    u8 *b = vd_alloc(ysz + 2 * uvsz + DAV1D_PICTURE_ALIGNMENT);
    if (!b) return DAV1D_ERR(ENOMEM);
    p->stride[0] = ys; p->stride[1] = uvs;
    p->data[0] = b; p->data[1] = chroma ? b + ysz : NULL; p->data[2] = chroma ? b + ysz + uvsz : NULL;
    p->allocator_data = b;
    return 0;
}
static void d1_free(Dav1dPicture *p, void *cookie) { (void)cookie; vd_release(p->allocator_data); }

struct VidDec {
    int codec;
    bool lowdelay;                               /* H264: AV_CODEC_FLAG_LOW_DELAY, пока не встретился B-кадр */
    AVCodecContext *av;
    AVPacket *pk;
    AVFrame *fr;
    Dav1dContext *d1;
    Dav1dPicture pic;
    bool have_pic;
};
static void av_quiet(void *a, int l, const char *f, va_list v) { (void)a; (void)l; (void)f; (void)v; }
/* Контекст libavcodec. H264 без B-кадров (WebRTC) — с AV_CODEC_FLAG_LOW_DELAY: кадр выходит сразу, без +1 кадра
 * задержки (has_b_frames), даже если в SPS нет VUI bitstream_restriction. Пришёл B-кадр — контекст пересоздаётся
 * без флага (vdec_frame), до ключевого кадра. Зерно плёнки не накладываем (экспорт — и не используем). */
static bool av_open(VidDec *d, enum AVCodecID id) {
    const AVCodec *c = avcodec_find_decoder(id);
    if (!c || !(d->av = avcodec_alloc_context3(c))) return false;
    d->av->thread_count = 1;
    d->av->thread_type = 0;
    d->av->flags2 |= AV_CODEC_FLAG2_FAST;
    if (d->lowdelay) d->av->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->av->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
    d->av->max_pixels = VD_MAX_PIX;
    d->av->get_buffer2 = av_get_buf;
    if (avcodec_open2(d->av, c, NULL) < 0) { avcodec_free_context(&d->av); return false; }
    return true;
}

VidDec *vdec_new(int codec) {
    VidDec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->codec = codec;
    if (codec == VC_AV1) {
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = 1;
        s.max_frame_delay = 1;
        s.apply_grain = 0;                       /* зерно плёнки — лишняя нагрузка для экрана */
        s.frame_size_limit = VD_MAX_PIX;
        s.all_layers = 0;
        s.output_invisible_frames = 0;
        s.logger.callback = NULL;
        s.allocator.cookie = NULL;
        s.allocator.alloc_picture_callback = d1_alloc;
        s.allocator.release_picture_callback = d1_free;
        if (dav1d_open(&d->d1, &s) < 0) { free(d); return NULL; }
        return d;
    }
    av_log_set_level(AV_LOG_QUIET);              /* нулевое логирование */
    av_log_set_callback(av_quiet);
    enum AVCodecID id = codec == VC_H264 ? AV_CODEC_ID_H264 : codec == VC_H265 ? AV_CODEC_ID_HEVC
                      : codec == VC_VP8 ? AV_CODEC_ID_VP8 : codec == VC_VP9 ? AV_CODEC_ID_VP9 : AV_CODEC_ID_NONE;
    d->lowdelay = codec == VC_H264;
    d->pk = av_packet_alloc();
    d->fr = av_frame_alloc();
    if (!d->pk || !d->fr || !av_open(d, id)) { vdec_free(d); return NULL; }
    return d;
}
void vdec_free(VidDec *d) {
    if (!d) return;
    if (d->have_pic) dav1d_picture_unref(&d->pic);
    if (d->d1) dav1d_close(&d->d1);
    av_frame_free(&d->fr);
    av_packet_free(&d->pk);
    avcodec_free_context(&d->av);
    free(d);
    vd_pool_drain();                             /* все кадры вернулись в пул — затереть и отдать системе */
}

/* картинка показана — отпустить свою ссылку на кадр (опорные держит сам декодер, неопорный вернётся в пул сразу) */
void vdec_done(VidDec *d) {
    if (!d) return;
    if (d->have_pic) { dav1d_picture_unref(&d->pic); d->have_pic = false; }
    if (d->fr) av_frame_unref(d->fr);
}
int vdec_frame(VidDec *d, const u8 *f, size_t n, bool skip_nonref, VidPic *o) {
    if (!d || !n || n > (32u << 20)) return VD_ERR;
    memset(o, 0, sizeof *o);
    if (d->d1) {
        if (d->have_pic) { dav1d_picture_unref(&d->pic); d->have_pic = false; }
        Dav1dData dd = { 0 };
        u8 *b = dav1d_data_create(&dd, n);
        if (!b) return VD_ERR;
        memcpy(b, f, n);
        bool got = false, bad = false;
        for (int guard = 0; guard < 64 && (dd.sz > 0 || !got); guard++) {
            int r = dd.sz > 0 ? dav1d_send_data(d->d1, &dd) : 0;
            if (r < 0 && r != DAV1D_ERR(EAGAIN)) { bad = true; break; }
            Dav1dPicture p = { 0 };
            int g = dav1d_get_picture(d->d1, &p);
            if (g == 0) {
                if (d->have_pic) dav1d_picture_unref(&d->pic);
                d->pic = p; d->have_pic = true; got = true;
            } else if (g != DAV1D_ERR(EAGAIN)) { bad = true; break; }
            else if (dd.sz == 0) break;
        }
        if (dd.sz > 0) dav1d_data_unref(&dd);
        if (!got) return bad ? VD_ERR : VD_NONE;
        if (d->pic.p.layout != DAV1D_PIXEL_LAYOUT_I420 || (d->pic.p.bpc != 8 && d->pic.p.bpc != 10)) return VD_UNSUPPORTED;
        for (int i = 0; i < 3; i++) { o->p[i] = d->pic.data[i]; o->s[i] = (int)d->pic.stride[i ? 1 : 0]; }
        o->w = d->pic.p.w; o->h = d->pic.p.h; o->bits = d->pic.p.bpc;
        return VD_PIC;
    }
    if (!d->av) return VD_ERR;
    /* не успеваем (в очереди ещё кадры): неопорные мимо, у остальных — без фильтра блочности (у WebRTC почти все
     * кадры опорные, NONREF один не помогает); в норме (очередь пуста) — полное качество */
    d->av->skip_frame = skip_nonref ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    d->av->skip_loop_filter = skip_nonref ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
    av_frame_unref(d->fr);
    d->pk->data = (u8 *)f;                       /* не своё: libavcodec скопирует (f с запасом AV_INPUT_BUFFER_PADDING_SIZE) */
    d->pk->size = (int)n;
    int r = avcodec_send_packet(d->av, d->pk);
    d->pk->data = NULL; d->pk->size = 0;
    if (r < 0 && r != AVERROR(EAGAIN)) return r == AVERROR_PATCHWELCOME || r == AVERROR(ENOSYS) ? VD_UNSUPPORTED : VD_ERR;
    bool got = false;
    for (;;) {                                   /* без задержки вывода — обычно ровно один кадр */
        AVFrame *t = av_frame_alloc();
        if (!t) break;
        int g = avcodec_receive_frame(d->av, t);
        if (g < 0) { av_frame_free(&t); break; }
        av_frame_unref(d->fr);
        av_frame_move_ref(d->fr, t);
        av_frame_free(&t);
        got = true;
    }
    if (!got) return VD_NONE;
    if (d->lowdelay && d->fr->pict_type == AV_PICTURE_TYPE_B) {   /* поток с B-кадрами: без LOW_DELAY, заново с ключевого */
        av_frame_unref(d->fr);
        avcodec_free_context(&d->av);
        d->lowdelay = false;
        if (!av_open(d, AV_CODEC_ID_H264)) return VD_ERR;
        return VD_ERR;
    }
    int fmt = d->fr->format;
    int bits = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P ? 8 : fmt == AV_PIX_FMT_YUV420P10LE ? 10 : 0;
    if (!bits) return VD_UNSUPPORTED;            /* 4:2:2 / 4:4:4 / 12 бит — экрану Discord не нужны */
    for (int i = 0; i < 3; i++) { o->p[i] = d->fr->data[i]; o->s[i] = d->fr->linesize[i]; }
    o->w = d->fr->width; o->h = d->fr->height; o->bits = bits;
    o->corrupt = (d->fr->flags & AV_FRAME_FLAG_CORRUPT) || d->fr->decode_error_flags;
    return VD_PIC;
}

/* ---------- selftest: кодер minih264 -> RTP -> сборщик -> libavcodec ---------- */
#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "../third_party/minih264/minih264e.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
typedef struct { u8 p[2048][1000]; size_t n[2048]; bool m[2048]; int k; } DecPk;
static void dec_emit(void *u, const u8 *p, size_t len, bool marker) {
    DecPk *t = u;
    if (t->k < 2048 && len <= sizeof t->p[0]) { memcpy(t->p[t->k], p, len); t->n[t->k] = len; t->m[t->k] = marker; t->k++; }
}
int vdec_selftest(void (*check)(bool ok, const char *what)) {
    /* ST_VID720=1 — замер памяти на кадре 1280x720 (обычно 96x64, быстро) */
    const char *big = getenv("ST_VID720");
    bool b720 = big && !strcmp(big, "1");
    int W = b720 ? 1280 : 96, H = b720 ? 720 : 64, mtu = b720 ? 1000 : 300;
    u8 *Y = malloc((size_t)W * H), *U = malloc((size_t)W * H / 4), *V = malloc((size_t)W * H / 4);
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = W; cp.height = H; cp.gop = 30; cp.vbv_size_bytes = 200000; cp.const_input_flag = 1;
    int szp = 0, szs = 0;
    bool ok = Y && U && V && !H264E_sizeof(&cp, &szp, &szs);
    H264E_persist_t *enc = ok ? malloc((size_t)szp) : NULL;
    H264E_scratch_t *scr = ok ? malloc((size_t)szs) : NULL;
    ok = enc && scr && !H264E_init(enc, &cp);
    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    rp.encode_speed = 8; rp.desired_frame_bytes = b720 ? 60000 : 4000; rp.qp_min = 10; rp.qp_max = 24;
    H264E_io_yuv_t io;
    io.yuv[0] = Y; io.yuv[1] = U; io.yuv[2] = V;
    io.stride[0] = W; io.stride[1] = W / 2; io.stride[2] = W / 2;
    DecPk *pk = calloc(1, sizeof *pk);
    H264Asm *as = h264_asm_new(0);
    u8 *fc[3] = { NULL, NULL, NULL };            /* собранные кадры (кодер освобождается до замера декодера) */
    size_t fcn[3] = { 0, 0, 0 };
    int frames = 0;
    u16 seq = 1000;
    for (int f = 0; ok && pk && as && f < 3; f++) {
        for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) Y[yy * W + xx] = (u8)(40 + ((xx + f * 3) * 2 + yy * 2) % 160);
        for (int i = 0; i < W * H / 4; i++) { U[i] = (u8)(110 + i % 30); V[i] = (u8)(140 - i % 20); }
        unsigned char *cod = NULL;
        int cn = 0;
        if (H264E_encode(enc, scr, &rp, &io, &cod, &cn) || cn <= 0) { ok = false; break; }
        pk->k = 0;
        h264_packetize(cod, (size_t)cn, (size_t)mtu, dec_emit, pk);   /* с FU-A */
        int r = 0;
        for (int i = 0; i < pk->k; i++) r |= h264_asm_push(as, seq++, (u32)(f * 3000), pk->m[i], pk->p[i], pk->n[i]);
        if (!(r & H264A_FRAME)) { ok = false; break; }
        const u8 *fr = h264_asm_frame(as, &fcn[f], NULL, NULL);
        fc[f] = calloc(1, fcn[f] + 64);           /* + AV_INPUT_BUFFER_PADDING_SIZE нулей */
        if (!fc[f]) { ok = false; break; }
        memcpy(fc[f], fr, fcn[f]);
        frames++;
    }
    free(enc); free(scr); free(pk); free(Y); free(U); free(V);
    h264_asm_free(as);
    size_t priv0 = plat_private_bytes();
    VidDec *d = ok ? vdec_new(VC_H264) : NULL;
    size_t pic_peak = 0, priv_peak = 0;
    int pics = 0, worst = 0, gw = 0, gh = 0;
    for (int f = 0; ok && d && f < frames; f++) {
        VidPic pic;
        int dr = vdec_frame(d, fc[f], fcn[f], false, &pic);
        if (vdec_bytes() > pic_peak) pic_peak = vdec_bytes();
        size_t pv = plat_private_bytes();
        if (pv > priv0 && pv - priv0 > priv_peak) priv_peak = pv - priv0;
        if (dr != VD_PIC || pic.bits != 8) { ok = false; break; }
        pics++; gw = pic.w; gh = pic.h;
        long long sum = 0;
        for (int yy = 0; yy < H && yy < pic.h; yy++) for (int xx = 0; xx < W && xx < pic.w; xx++) {
            int e = pic.p[0][yy * pic.s[0] + xx] - (40 + ((xx + f * 3) * 2 + yy * 2) % 160);
            sum += e < 0 ? -e : e;
        }
        int mad = (int)(sum / ((long long)W * H));
        if (mad > worst) worst = mad;
    }
    vdec_free(d);
    for (int f = 0; f < 3; f++) free(fc[f]);
    size_t after = vdec_bytes();
    char m[400];
    snprintf(m, sizeof m, "Go Live зритель: minih264 -> RTP (FU-A) -> сборка -> libavcodec h264: %d/3 кадров %dx%d, ошибка яркости %d, кадры декодера пик %zu КБ (частная +%zu КБ) -> после %zu Б",
             pics, gw, gh, worst, pic_peak / 1024, priv_peak / 1024, after);
    check(ok && pics == 3 && frames == 3 && gw == W && gh == H && worst <= 6 && after == 0, m);
    /* все декодеры создаются однопоточными; мусор — без сбоя; память кадров после — 0 */
    bool all = true;
    static u8 junk[200];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (u8)(i * 37 + 11);
    junk[0] = 0; junk[1] = 0; junk[2] = 0; junk[3] = 1;
    for (int c = VC_H264; c <= VC_AV1; c++) {
        VidDec *x = vdec_new(c);
        if (!x) { all = false; continue; }
        u8 *jb = calloc(1, sizeof junk + 64);
        if (jb) { memcpy(jb, junk, sizeof junk); VidPic pp; int r = vdec_frame(x, jb, sizeof junk, false, &pp); if (r == VD_PIC) all = false; free(jb); }
        vdec_free(x);
    }
    check(all && vdec_bytes() == 0, "Go Live зритель: декодеры H264/H265/VP8/VP9/AV1 создаются, мусорный кадр — без картинки и без сбоя, память кадров 0");
    return 0;
}
#endif
