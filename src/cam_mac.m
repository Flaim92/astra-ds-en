/* cam_mac.m - веб-камера «Astra DS Video» на macOS: AVFoundation (AVCaptureSession + AVCaptureVideoDataOutput).
 * Договор с cam.c - cam_mac.h. Собирается только вместе с -DASTRA_CAM_MAC и с -fobjc-arc.
 *
 * Режим камеры: формат устройства с >= 24 к/с, вписывающийся в наибольший кадр эфира (720p/480p/360p - выбор
 * качества в cam.c), из таких - самый крупный; иначе самый мелкий из крупных. Выход всегда NV12 (420v), уже
 * вписанный в размер эфира: масштабирует сам AVFoundation, cam.c только переводит NV12 -> I420 один к одному.
 * Темп - не выше 30 к/с.
 *
 * Кадры приходят в последовательную очередь dispatch; хранится только самый свежий (буфер AVFoundation удержан),
 * поток камеры cam.c забирает его через cammac_read. Плоскости Y и UV отдаются прямо из буфера, если лежат подряд
 * с одинаковым шагом; иначе копируются в свой буфер, который затирается при закрытии. Ни файлов, ни вывода.
 *
 * Разрешение на камеру: первый раз система спрашивает пользователя (ждём ответа в потоке камеры). Для собранного
 * .app в Info.plist обязателен NSCameraUsageDescription, иначе система завершает процесс при обращении к камере. */
#if defined(ASTRA_VIDEO) && defined(__APPLE__)
#if !__has_feature(objc_arc)
#error "cam_mac.m собирается с -fobjc-arc"
#endif
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "cam_mac.h"

#define CM_FIRST_FRAME_MS 5000                   /* после startRunning: первый кадр - не позже */

static void cm_wipe(void *p, size_t n) {         /* затирание, которое компилятор не выбросит */
    volatile unsigned char *b = p;
    while (n--) *b++ = 0;
}

@interface AstraCamGrab : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    pthread_mutex_t mu_;
    pthread_cond_t cv_;
    CVPixelBufferRef next_;                      /* свежий кадр (под mu_), удержан */
    CVPixelBufferRef cur_;                       /* выдан cam.c, база заблокирована; только поток камеры */
    int gone_;                                   /* камера отключена / сессия оборвалась (под mu_) */
    int w_, h_;                                  /* размер кадра (по первому кадру) */
    uint8_t *copy_;                              /* NV12 одним куском, если плоскости не подряд */
    size_t copy_n_;
    AVCaptureSession *session_;
    AVCaptureDevice *device_;
    AVCaptureVideoDataOutput *output_;
    dispatch_queue_t queue_;
    NSMutableArray *observers_;
}
@end

@implementation AstraCamGrab
- (instancetype)init {
    if ((self = [super init])) {
        pthread_mutex_init(&mu_, NULL);
        pthread_cond_init(&cv_, NULL);
        observers_ = [NSMutableArray array];
    }
    return self;
}
- (void)dealloc {
    if (next_) CVPixelBufferRelease(next_);
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}
- (void)captureOutput:(AVCaptureOutput *)out didOutputSampleBuffer:(CMSampleBufferRef)sb fromConnection:(AVCaptureConnection *)conn {
    (void)out; (void)conn;
    CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb);
    if (!ib) return;
    CVPixelBufferRetain(ib);
    pthread_mutex_lock(&mu_);
    CVPixelBufferRef old = next_;
    next_ = ib;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
    if (old) CVPixelBufferRelease(old);          /* не успели забрать - старый кадр мимо */
}
- (void)markGone {
    pthread_mutex_lock(&mu_);
    gone_ = 1;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}
@end

/* все камеры видео (встроенные, внешние USB, Continuity) */
static NSArray<AVCaptureDevice *> *cm_devices(void) {
    if (@available(macOS 10.15, *)) {
        NSMutableArray<AVCaptureDeviceType> *types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
        if (@available(macOS 14.0, *)) {
            [types addObject:AVCaptureDeviceTypeExternal];
            [types addObject:AVCaptureDeviceTypeContinuityCamera];
        } else
#endif
        {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
        }
        AVCaptureDeviceDiscoverySession *ds = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                                                     mediaType:AVMediaTypeVideo
                                                                                                      position:AVCaptureDevicePositionUnspecified];
        return ds.devices ? ds.devices : @[];
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray<AVCaptureDevice *> *all = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
    return all ? all : @[];
}

int cammac_list(void (*add)(void *u, const char *id, const char *name), void *u) {
    int n = 0;
    @autoreleasepool {
        for (AVCaptureDevice *d in cm_devices()) {
            const char *id = d.uniqueID.UTF8String, *name = d.localizedName.UTF8String;
            if (!id || !*id) continue;
            if (add) add(u, id, name ? name : "");
            n++;
        }
    }
    return n;
}

/* разрешение на камеру; первый раз - системный вопрос пользователю */
static int cm_access(void) {
    AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (st == AVAuthorizationStatusAuthorized) return CAMMAC_OK;
    if (st != AVAuthorizationStatusNotDetermined) return CAMMAC_E_DENIED;   /* запрещено или ограничено */
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL granted = NO;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL ok) {
        granted = ok;
        dispatch_semaphore_signal(sem);
    }];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)120 * NSEC_PER_SEC))) return CAMMAC_E_DENIED;
    return granted ? CAMMAC_OK : CAMMAC_E_DENIED;
}

static int cm_nserr(NSError *e) {
    if (!e) return CAMMAC_E_OTHER;
    if (![e.domain isEqualToString:AVFoundationErrorDomain]) return CAMMAC_E_OTHER;
    switch (e.code) {
    case AVErrorApplicationIsNotAuthorizedToUseDevice: return CAMMAC_E_DENIED;
    case AVErrorDeviceInUseByAnotherApplication: return CAMMAC_E_BUSY;
    case AVErrorDeviceNotConnected: case AVErrorDeviceWasDisconnected: return CAMMAC_E_GONE;
    default: return CAMMAC_E_OTHER;
    }
}

/* размер эфира: как у камеры, если влезает, иначе вписать с пропорциями; чётный (как cam_out_size в cam.c) */
static void cm_fit(int sw, int sh, int maxw, int maxh, int *ow, int *oh) {
    int w = sw, h = sh;
    if (w > maxw || h > maxh) {
        if ((long long)w * maxh > (long long)h * maxw) { h = (int)((long long)h * maxw / w); w = maxw; }
        else { w = (int)((long long)w * maxh / h); h = maxh; }
    }
    *ow = w & ~1; *oh = h & ~1;
}

static void cm_teardown(AstraCamGrab *g) {
    if (g->session_.running) [g->session_ stopRunning];
    [g->output_ setSampleBufferDelegate:nil queue:NULL];
    for (id o in g->observers_) [[NSNotificationCenter defaultCenter] removeObserver:o];
    [g->observers_ removeAllObjects];
    if (g->queue_) dispatch_sync(g->queue_, ^{});   /* вызовы делегата, уже стоящие в очереди, - до конца */
    if (g->cur_) {
        CVPixelBufferUnlockBaseAddress(g->cur_, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(g->cur_);
        g->cur_ = NULL;
    }
    pthread_mutex_lock(&g->mu_);
    CVPixelBufferRef nx = g->next_;
    g->next_ = NULL;
    pthread_mutex_unlock(&g->mu_);
    if (nx) CVPixelBufferRelease(nx);
    if (g->copy_) { cm_wipe(g->copy_, g->copy_n_); free(g->copy_); g->copy_ = NULL; g->copy_n_ = 0; }
    g->session_ = nil; g->output_ = nil; g->device_ = nil; g->queue_ = nil;
}

CamMac *cammac_open(const char *id, int maxw, int maxh, CamMacMode *m, int *err, int *code) {
    *err = CAMMAC_OK; *code = 0;
    memset(m, 0, sizeof *m);
    if (maxw < 160 || maxh < 120) { maxw = 1280; maxh = 720; }
    @autoreleasepool {
        AVCaptureDevice *dev = nil;
        for (AVCaptureDevice *d in cm_devices()) {
            const char *u = d.uniqueID.UTF8String;
            if (!id || !*id || (u && !strcmp(u, id))) { dev = d; break; }
        }
        if (!dev) { *err = CAMMAC_E_NOTFOUND; return NULL; }
        int a = cm_access();
        if (a != CAMMAC_OK) { *err = a; return NULL; }

        /* формат устройства: плавность, затем «влезает в эфир» и крупнее, затем родной 4:2:0 (без перевода) */
        AVCaptureDeviceFormat *best = nil;
        long long bscore = -1;
        double bfps = 0;
        int bw = 0, bh = 0;
        for (AVCaptureDeviceFormat *f in dev.formats) {
            CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
            if (d.width < 160 || d.height < 120 || d.width > 4096 || d.height > 4096) continue;
            double fmax = 0;
            for (AVFrameRateRange *r in f.videoSupportedFrameRateRanges) if (r.maxFrameRate > fmax) fmax = r.maxFrameRate;
            if (fmax < 1) continue;
            FourCharCode st = CMFormatDescriptionGetMediaSubType(f.formatDescription);
            bool fit = d.width <= maxw && d.height <= maxh;
            long long area = (long long)d.width * d.height;
            long long s = fmax >= 24 ? 3 : fmax >= 15 ? 2 : 1;
            s = s << 1 | (fit ? 1 : 0);
            s = s << 26 | (fit ? area : 4096LL * 4096 - area);
            s = s << 1 | (st == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || st == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ? 1 : 0);
            if (s > bscore) { bscore = s; best = f; bfps = fmax; bw = d.width; bh = d.height; }
        }
        if (!best) { *err = CAMMAC_E_FORMAT; return NULL; }
        int ow, oh;
        cm_fit(bw, bh, maxw, maxh, &ow, &oh);
        if (ow < 2 || oh < 2) { *err = CAMMAC_E_FORMAT; return NULL; }

        NSError *ne = nil;
        AVCaptureDeviceInput *in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&ne];
        if (!in) { *err = cm_nserr(ne); *code = ne ? (int)ne.code : 0; return NULL; }

        AstraCamGrab *g = [[AstraCamGrab alloc] init];
        if (!g) { *err = CAMMAC_E_OTHER; return NULL; }
        g->device_ = dev;
        g->session_ = [[AVCaptureSession alloc] init];
        g->output_ = [[AVCaptureVideoDataOutput alloc] init];
        g->queue_ = dispatch_queue_create("astra.cam", DISPATCH_QUEUE_SERIAL);
        if (!g->session_ || !g->output_ || !g->queue_) { cm_teardown(g); *err = CAMMAC_E_OTHER; return NULL; }
        g->output_.alwaysDiscardsLateVideoFrames = YES;
        g->output_.videoSettings = @{
            (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (__bridge NSString *)kCVPixelBufferWidthKey: @(ow),
            (__bridge NSString *)kCVPixelBufferHeightKey: @(oh),
        };
        [g->output_ setSampleBufferDelegate:g queue:g->queue_];

        [g->session_ beginConfiguration];
        bool added = [g->session_ canAddInput:in];
        if (added) [g->session_ addInput:in];
        added = added && [g->session_ canAddOutput:g->output_];
        if (added) [g->session_ addOutput:g->output_];
        if (!added) { [g->session_ commitConfiguration]; cm_teardown(g); *err = CAMMAC_E_BUSY; return NULL; }
        /* формат и темп - под замком устройства, замок держим до startRunning (иначе сессия вернёт свой пресет) */
        bool locked = [dev lockForConfiguration:&ne];
        if (locked) {
            dev.activeFormat = best;
            double want = bfps > 30 ? 30 : bfps;
            for (AVFrameRateRange *r in best.videoSupportedFrameRateRanges) {
                if (want < r.minFrameRate - 0.01 || want > r.maxFrameRate + 0.01) continue;
                CMTime fd = CMTimeMake(1000, (int32_t)(want * 1000 + 0.5));
                if (CMTimeCompare(fd, r.minFrameDuration) < 0) fd = r.minFrameDuration;
                if (CMTimeCompare(fd, r.maxFrameDuration) > 0) fd = r.maxFrameDuration;
                dev.activeVideoMinFrameDuration = fd;
                break;
            }
        }
        [g->session_ commitConfiguration];

        __weak AstraCamGrab *wg = g;
        void (^gone)(NSNotification *) = ^(NSNotification *nt) { (void)nt; [wg markGone]; };
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [g->observers_ addObject:[nc addObserverForName:AVCaptureDeviceWasDisconnectedNotification object:dev queue:nil usingBlock:gone]];
        [g->observers_ addObject:[nc addObserverForName:AVCaptureSessionRuntimeErrorNotification object:g->session_ queue:nil usingBlock:gone]];

        [g->session_ startRunning];              /* блокирующий: мы в потоке камеры */
        if (locked) [dev unlockForConfiguration];
        if (!g->session_.running) { cm_teardown(g); *err = CAMMAC_E_BUSY; return NULL; }

        /* первый кадр: настоящий размер (если масштаб в videoSettings не сработал - cam.c уменьшит сам) */
        int fw = 0, fh = 0;
        pthread_mutex_lock(&g->mu_);
        for (int waited = 0; !g->next_ && !g->gone_ && waited < CM_FIRST_FRAME_MS; waited += 100) {
            struct timespec rel = { 0, 100 * 1000 * 1000 };
            pthread_cond_timedwait_relative_np(&g->cv_, &g->mu_, &rel);
        }
        if (g->next_) { fw = (int)CVPixelBufferGetWidth(g->next_); fh = (int)CVPixelBufferGetHeight(g->next_); }
        int gn = g->gone_;
        pthread_mutex_unlock(&g->mu_);
        if (!fw || !fh) { cm_teardown(g); *err = gn ? CAMMAC_E_GONE : CAMMAC_E_NOFRAMES; return NULL; }
        g->w_ = fw & ~1; g->h_ = fh & ~1;
        m->w = g->w_; m->h = g->h_;
        CMTime fd = dev.activeVideoMinFrameDuration;
        m->fps = fd.value > 0 && fd.timescale > 0 ? (double)fd.timescale / (double)fd.value : (bfps > 30 ? 30 : bfps);
        if (m->fps > 30) m->fps = 30;
        return (CamMac *)CFBridgingRetain(g);
    }
    return NULL;                                 /* сюда не доходит: все пути выше возвращают */
}

void cammac_done(CamMac *c) {
    if (!c) return;
    AstraCamGrab *g = (__bridge AstraCamGrab *)(void *)c;
    if (!g->cur_) return;
    CVPixelBufferUnlockBaseAddress(g->cur_, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(g->cur_);
    g->cur_ = NULL;
}

int cammac_read(CamMac *c, CamMacFrame *f, int timeout_ms, int *err) {
    *err = CAMMAC_OK;
    if (!c) { *err = CAMMAC_E_GONE; return -1; }
    AstraCamGrab *g = (__bridge AstraCamGrab *)(void *)c;
    cammac_done(c);
    pthread_mutex_lock(&g->mu_);
    if (!g->next_ && !g->gone_ && timeout_ms > 0) {
        struct timespec rel = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
        pthread_cond_timedwait_relative_np(&g->cv_, &g->mu_, &rel);
    }
    CVPixelBufferRef pb = g->next_;
    g->next_ = NULL;
    int gn = g->gone_;
    pthread_mutex_unlock(&g->mu_);
    if (!pb) { if (gn) { *err = CAMMAC_E_GONE; return -1; } return 0; }
    OSType pf = CVPixelBufferGetPixelFormatType(pb);
    if ((pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        || !CVPixelBufferIsPlanar(pb) || CVPixelBufferGetPlaneCount(pb) != 2) {
        CVPixelBufferRelease(pb);
        *err = CAMMAC_E_FORMAT;
        return -1;
    }
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) { CVPixelBufferRelease(pb); return 0; }
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    size_t uvh = CVPixelBufferGetHeightOfPlane(pb, 1);
    size_t fw = (size_t)g->w_, fh = (size_t)g->h_;
    if (!y || !uv || w < fw || h < fh || uvh < fh / 2 || ys < fw || uvs < fw || ys > 1u << 16 || uvs > 1u << 16) {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(pb);
        return 0;                                /* кадр не того размера: мимо */
    }
    uintptr_t ya = (uintptr_t)y, ua = (uintptr_t)uv;
    if (ys == uvs && ua >= ya + ys * fh && ua - ya <= ys * (fh + 64)) {
        /* как V4L2/Media Foundation: Y, за ним (с выравниванием высоты) UV тем же шагом - без копии */
        f->p = y; f->stride = (int)ys; f->uv_off = (size_t)(ua - ya);
        f->n = f->uv_off + uvs * (fh / 2);
    } else {
        size_t need = fw * fh + fw * (fh / 2);
        if (g->copy_n_ != need) {
            if (g->copy_) { cm_wipe(g->copy_, g->copy_n_); free(g->copy_); }
            g->copy_ = malloc(need);
            g->copy_n_ = g->copy_ ? need : 0;
        }
        if (!g->copy_) { CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(pb); return 0; }
        for (size_t r = 0; r < fh; r++) memcpy(g->copy_ + r * fw, y + r * ys, fw);
        for (size_t r = 0; r < fh / 2; r++) memcpy(g->copy_ + fw * fh + r * fw, uv + r * uvs, fw);
        f->p = g->copy_; f->stride = (int)fw; f->uv_off = fw * fh; f->n = need;
    }
    f->w = (int)fw; f->h = (int)fh;
    g->cur_ = pb;                                /* база заблокирована до cammac_done */
    return 1;
}

void cammac_close(CamMac *c) {
    if (!c) return;
    @autoreleasepool {
        AstraCamGrab *g = (AstraCamGrab *)CFBridgingRelease(c);
        cm_teardown(g);
    }
}
#endif
