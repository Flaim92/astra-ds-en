/* Звук macOS: Audio Queue (AudioToolbox). 48 кГц, s16: вывод стерео, микрофон моно — частоту устройства
 * система пересчитывает сама. Очереди зовут свои обработчики в собственном потоке; обмен с голосовым
 * потоком — через два кольца под мьютексом, а пробуждение его poll() — байтом в неблокирующий канал
 * (audio_waitables). Вывод: кольцо на 200 мс + 3 буфера очереди по 10 мс; не успели дописать — тишина. */
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "mc.h"
#include "plat.h"

#define RATE 48000
#define OUT_BUFS 3
#define OUT_BUF_FR 480                          /* 10 мс */
#define OUT_RING (RATE / 5)                     /* 200 мс стерео-кадров */
#define IN_BUFS 3
#define IN_BUF_FR 960                           /* 20 мс */
#define IN_RING RATE                            /* 1 с моно */

struct Audio {
    AudioQueueRef oq, iq;
    AudioQueueBufferRef ob[OUT_BUFS], ib[IN_BUFS];
    pthread_mutex_t mu;
    int16_t *oring;                             /* стерео, OUT_RING кадров */
    int orh, orn;
    int16_t *iring;                             /* моно, IN_RING сэмплов */
    int irh, irn;
    int pipe_r, pipe_w;
    volatile bool lost, stopping;
};

static void poke(Audio *a) { char c = 1; ssize_t r = write(a->pipe_w, &c, 1); (void)r; }
static void drain(Audio *a) { char b[64]; while (read(a->pipe_r, b, sizeof b) == (ssize_t)sizeof b) {} }

static void out_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef b) {
    Audio *a = ud;
    if (a->stopping) return;
    int16_t *d = b->mAudioData;
    pthread_mutex_lock(&a->mu);
    int take = a->orn < OUT_BUF_FR ? a->orn : OUT_BUF_FR;
    for (int i = 0; i < take; i++) {
        int k = (a->orh + i) % OUT_RING;
        d[i * 2] = a->oring[k * 2];
        d[i * 2 + 1] = a->oring[k * 2 + 1];
    }
    a->orh = (a->orh + take) % OUT_RING;
    a->orn -= take;
    pthread_mutex_unlock(&a->mu);
    if (take < OUT_BUF_FR) memset(d + take * 2, 0, (size_t)(OUT_BUF_FR - take) * 2 * sizeof(int16_t));
    b->mAudioDataByteSize = OUT_BUF_FR * 2 * sizeof(int16_t);
    if (AudioQueueEnqueueBuffer(q, b, 0, NULL) != noErr) a->lost = true;
    poke(a);
}

static void in_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *t, UInt32 np, const AudioStreamPacketDescription *pd) {
    (void)t; (void)np; (void)pd;
    Audio *a = ud;
    if (a->stopping) return;
    const int16_t *s = b->mAudioData;
    int n = (int)(b->mAudioDataByteSize / sizeof(int16_t));
    pthread_mutex_lock(&a->mu);
    for (int i = 0; i < n; i++) {
        if (a->irn == IN_RING) { a->irh = (a->irh + 1) % IN_RING; a->irn--; }   /* переполнение — выбросить старое */
        a->iring[(a->irh + a->irn) % IN_RING] = s[i];
        a->irn++;
    }
    pthread_mutex_unlock(&a->mu);
    memset(b->mAudioData, 0, b->mAudioDataBytesCapacity);   /* голос не лежит в буфере очереди дольше нужного */
    if (AudioQueueEnqueueBuffer(q, b, 0, NULL) != noErr) a->lost = true;
    poke(a);
}

static AudioStreamBasicDescription fmt(unsigned ch) {
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = ch;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = f.mBytesPerPacket = 2 * ch;
    f.mFramesPerPacket = 1;
    return f;
}

/* ---------- устройства: имя -> UID (для выбора в настройках, --mic= / --out=) ---------- */
static int dev_list(int out, AudioDeviceID *ids, char (*names)[96], int max) {
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &sz) != noErr) return -1;
    int n = (int)(sz / sizeof(AudioDeviceID));
    if (n <= 0) return 0;
    AudioDeviceID *all = malloc(sz);
    if (!all) return -1;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, all) != noErr) { free(all); return -1; }
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        AudioObjectPropertyAddress sp = { kAudioDevicePropertyStreams, out ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain };
        UInt32 ss = 0;
        if (AudioObjectGetPropertyDataSize(all[i], &sp, 0, NULL, &ss) != noErr || ss == 0) continue;
        AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef nm = NULL;
        UInt32 ns = sizeof nm;
        if (AudioObjectGetPropertyData(all[i], &np, 0, NULL, &ns, &nm) != noErr || !nm) continue;
        char buf[96] = "";
        CFStringGetCString(nm, buf, sizeof buf, kCFStringEncodingUTF8);
        CFRelease(nm);
        if (!buf[0]) continue;
        if (ids) ids[k] = all[i];
        if (names) { char *t = san(buf, false); snprintf(names[k], 96, "%s", t); free(t); }
        k++;
    }
    free(all);
    return k;
}
/* Указанное устройство (по имени) — очереди; нет такого или пусто — устройство системы по умолчанию. */
static void pick_dev(AudioQueueRef q, int out, const char *hint) {
    if (!hint || !*hint || !strcmp(hint, "default")) return;
    AudioDeviceID ids[32];
    char nm[32][96];
    int n = dev_list(out, ids, nm, 32);
    for (int i = 0; i < n; i++) {
        if (strcmp(nm[i], hint)) continue;
        AudioObjectPropertyAddress up = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef uid = NULL;
        UInt32 us = sizeof uid;
        if (AudioObjectGetPropertyData(ids[i], &up, 0, NULL, &us, &uid) == noErr && uid) {
            AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &uid, sizeof uid);
            CFRelease(uid);
        }
        return;
    }
}

static Audio *a_new(void) {
    Audio *a = xcalloc(1, sizeof *a);
    pthread_mutex_init(&a->mu, NULL);
    int p[2];
    if (pipe(p)) { free(a); return NULL; }
    for (int i = 0; i < 2; i++) { fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK); fcntl(p[i], F_SETFD, FD_CLOEXEC); }
    a->pipe_r = p[0];
    a->pipe_w = p[1];
    a->oring = xcalloc(OUT_RING * 2, sizeof(int16_t));
    return a;
}

static bool open_out(Audio *a, const char *hint) {
    AudioStreamBasicDescription f = fmt(2);
    if (AudioQueueNewOutput(&f, out_cb, a, NULL, NULL, 0, &a->oq) != noErr) { a->oq = NULL; return false; }
    pick_dev(a->oq, 1, hint);
    for (int i = 0; i < OUT_BUFS; i++) {
        if (AudioQueueAllocateBuffer(a->oq, OUT_BUF_FR * 4, &a->ob[i]) != noErr) return false;
        memset(a->ob[i]->mAudioData, 0, OUT_BUF_FR * 4);
        a->ob[i]->mAudioDataByteSize = OUT_BUF_FR * 4;
        AudioQueueEnqueueBuffer(a->oq, a->ob[i], 0, NULL);
    }
    return AudioQueueStart(a->oq, NULL) == noErr;
}
static bool open_in(Audio *a, const char *hint) {
    AudioStreamBasicDescription f = fmt(1);
    if (AudioQueueNewInput(&f, in_cb, a, NULL, NULL, 0, &a->iq) != noErr) { a->iq = NULL; return false; }
    pick_dev(a->iq, 0, hint);
    a->iring = xcalloc(IN_RING, sizeof(int16_t));
    for (int i = 0; i < IN_BUFS; i++) {
        if (AudioQueueAllocateBuffer(a->iq, IN_BUF_FR * 2, &a->ib[i]) != noErr) return false;
        AudioQueueEnqueueBuffer(a->iq, a->ib[i], 0, NULL);
    }
    return AudioQueueStart(a->iq, NULL) == noErr;
}

void audio_close(Audio *a) {
    if (!a) return;
    a->stopping = true;
    if (a->oq) { AudioQueueStop(a->oq, true); AudioQueueDispose(a->oq, true); }
    if (a->iq) { AudioQueueStop(a->iq, true); AudioQueueDispose(a->iq, true); }
    if (a->iring) { plat_wipe(a->iring, IN_RING * sizeof(int16_t)); free(a->iring); }
    if (a->oring) { plat_wipe(a->oring, OUT_RING * 2 * sizeof(int16_t)); free(a->oring); }
    close(a->pipe_r);
    close(a->pipe_w);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

Audio *audio_open(const char *mic_hint, const char *out_hint, char *err, size_t errn) {
    Audio *a = a_new();
    if (!a || !open_out(a, out_hint)) { snprintf(err, errn, "%s", ui_tr("не открылось устройство вывода звука")); audio_close(a); return NULL; }
    if (!open_in(a, mic_hint)) {
        if (a->iq) { AudioQueueDispose(a->iq, true); a->iq = NULL; }
        snprintf(err, errn, "%s", ui_tr("микрофон не найден: только прослушивание"));
    }
    return a;
}
Audio *audio_open_out(const char *out_hint, char *err, size_t errn) {
    Audio *a = a_new();
    if (!a || !open_out(a, out_hint)) { snprintf(err, errn, "%s", ui_tr("не открылось устройство вывода звука")); audio_close(a); return NULL; }
    return a;
}

int audio_waitables(Audio *a, Waitable *out, int max) {
    if (max < 1) return 0;
    out[0] = (Waitable){ a->pipe_r, POLLIN };
    return 1;
}

bool audio_read_frame(Audio *a, int16_t *pcm960) {
    drain(a);
    if (!a->iq) return false;
    pthread_mutex_lock(&a->mu);
    bool ok = a->irn >= 960;
    if (ok) {
        for (int i = 0; i < 960; i++) pcm960[i] = a->iring[(a->irh + i) % IN_RING];
        a->irh = (a->irh + 960) % IN_RING;
        a->irn -= 960;
    }
    pthread_mutex_unlock(&a->mu);
    return ok;
}

int audio_render_queued(Audio *a) {
    pthread_mutex_lock(&a->mu);
    int n = a->orn;
    pthread_mutex_unlock(&a->mu);
    return n + OUT_BUFS * OUT_BUF_FR;           /* плюс буферы, уже отданные очереди */
}
int audio_render_space(Audio *a) {
    drain(a);
    pthread_mutex_lock(&a->mu);
    int n = OUT_RING - a->orn;
    pthread_mutex_unlock(&a->mu);
    return n;
}
void audio_render_write(Audio *a, const int16_t *stereo, int frames) {
    pthread_mutex_lock(&a->mu);
    for (int i = 0; i < frames && a->orn < OUT_RING; i++) {
        int k = (a->orh + a->orn) % OUT_RING;
        a->oring[k * 2] = stereo[i * 2];
        a->oring[k * 2 + 1] = stereo[i * 2 + 1];
        a->orn++;
    }
    pthread_mutex_unlock(&a->mu);
}

bool audio_lost(Audio *a) { return a->lost; }

int audio_devices(int out, char (*names)[96], int max) { return dev_list(out, NULL, names, max); }
void audio_list_devices(void) {
    char nm[32][96];
    term_puts(ui_tr("Устройства звука (--out=ИМЯ / --mic=ИМЯ, по умолчанию - системные):\n"));
    for (int o = 1; o >= 0; o--) {
        int n = dev_list(o, NULL, nm, 32);
        for (int i = 0; i < n; i++) {
            term_puts("  ");
            term_puts(nm[i]);
            term_puts(o ? ui_tr("  [вывод]\n") : ui_tr("  [микрофон]\n"));
        }
    }
}
#endif
