/* Звук Linux: ALSA, неблокирующий режим, дескрипторы ждутся в общем poll() голосового потока.
 * Устройство "default" идёт через PipeWire/PulseAudio, если они есть, иначе напрямую. */
#ifndef _WIN32
#include <alsa/asoundlib.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "plat.h"

struct Audio {
    snd_pcm_t *pb, *cap;
    int16_t *acc;
    int accn, acccap;
    bool lost;
};

static snd_pcm_t *open_pcm(const char *name, snd_pcm_stream_t dir, unsigned ch) {
    snd_pcm_t *p = NULL;
    if (snd_pcm_open(&p, name && *name ? name : "default", dir, SND_PCM_NONBLOCK) < 0) return NULL;
    /* 48 кГц, s16; при другой частоте устройства ALSA пересчитает сама (soft_resample=1). */
    if (snd_pcm_set_params(p, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, ch, 48000, 1, 60000) < 0) {
        snd_pcm_close(p);
        return NULL;
    }
    return p;
}

Audio *audio_open(const char *mic_hint, const char *out_hint, char *err, size_t errn) {
    Audio *a = xcalloc(1, sizeof *a);
    a->pb = open_pcm(out_hint, SND_PCM_STREAM_PLAYBACK, 2);
    if (!a->pb) { snprintf(err, errn, "не открылось устройство вывода ALSA"); free(a); return NULL; }
    a->cap = open_pcm(mic_hint, SND_PCM_STREAM_CAPTURE, 1);
    if (a->cap) snd_pcm_start(a->cap);
    else snprintf(err, errn, "микрофон не найден: только прослушивание");
    a->acccap = 48000;
    a->acc = xmalloc((size_t)a->acccap * sizeof(int16_t));
    return a;
}

/* №69: только вывод (голосовые сообщения) */
Audio *audio_open_out(const char *out_hint, char *err, size_t errn) {
    Audio *a = xcalloc(1, sizeof *a);
    a->pb = open_pcm(out_hint, SND_PCM_STREAM_PLAYBACK, 2);
    if (!a->pb) { snprintf(err, errn, "не открылось устройство вывода ALSA"); free(a); return NULL; }
    return a;
}

void audio_close(Audio *a) {
    if (!a) return;
    if (a->pb) { snd_pcm_drop(a->pb); snd_pcm_close(a->pb); }
    if (a->cap) { snd_pcm_drop(a->cap); snd_pcm_close(a->cap); }
    if (a->acc) { plat_wipe(a->acc, (size_t)a->acccap * sizeof(int16_t)); free(a->acc); }
    free(a);
}

int audio_waitables(Audio *a, Waitable *out, int max) {
    int n = 0;
    snd_pcm_t *pcms[2] = { a->pb, a->cap };
    for (int k = 0; k < 2; k++) {
        if (!pcms[k]) continue;
        struct pollfd pf[4];
        int c = snd_pcm_poll_descriptors(pcms[k], pf, 4);
        for (int i = 0; i < c && n < max; i++) out[n++] = (Waitable){ pf[i].fd, pf[i].events };
    }
    return n;
}

static bool recover(Audio *a, snd_pcm_t *p, long rc) {
    if (rc == -ENODEV) { a->lost = true; return false; }
    if (rc == -EPIPE || rc == -ESTRPIPE) {
        if (snd_pcm_recover(p, (int)rc, 1) < 0) { a->lost = true; return false; }
        if (p == a->cap) snd_pcm_start(p);
        return true;
    }
    return false;
}

bool audio_read_frame(Audio *a, int16_t *pcm960) {
    if (a->cap && a->accn < 960) {
        /* одно чтение за вызов и не больше, чем устройство реально накопило */
        for (int attempt = 0; attempt < 2; attempt++) {
            snd_pcm_sframes_t av = snd_pcm_avail_update(a->cap);
            if (av < 0) { if (recover(a, a->cap, av)) continue; break; }
            int room = a->acccap - a->accn;
            if (av > room) av = room;
            if (av <= 0) break;
            snd_pcm_sframes_t r = snd_pcm_readi(a->cap, a->acc + a->accn, (snd_pcm_uframes_t)av);
            if (r == -EAGAIN) break;
            if (r < 0) { if (recover(a, a->cap, r)) continue; break; }
            a->accn += (int)r;
            break;
        }
    }
    if (a->accn < 960) return false;
    memcpy(pcm960, a->acc, 960 * sizeof(int16_t));
    memmove(a->acc, a->acc + 960, (size_t)(a->accn - 960) * sizeof(int16_t));
    a->accn -= 960;
    return true;
}

int audio_render_space(Audio *a) {
    snd_pcm_sframes_t av = snd_pcm_avail_update(a->pb);
    if (av < 0) { if (recover(a, a->pb, av)) av = snd_pcm_avail_update(a->pb); else return 0; }
    return av < 0 ? 0 : (int)av;
}

void audio_render_write(Audio *a, const int16_t *stereo, int frames) {
    while (frames > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(a->pb, stereo, (snd_pcm_uframes_t)frames);
        if (w == -EAGAIN) return;
        if (w < 0) { if (!recover(a, a->pb, w)) return; continue; }
        stereo += w * 2;
        frames -= (int)w;
    }
}

bool audio_lost(Audio *a) { return a->lost; }

/* №140: имена PCM ALSA (NAME) нужного направления — для выбора в настройках */
int audio_devices(int out, char (*names)[96], int max) {
    void **hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return -1;
    int k = 0;
    for (void **h = hints; *h && k < max; h++) {
        char *name = snd_device_name_get_hint(*h, "NAME");
        char *io = snd_device_name_get_hint(*h, "IOID");
        bool ok = name && strlen(name) < 96 && (!io || !strcmp(io, out ? "Output" : "Input"));
        if (ok) { char *t = san(name, false); snprintf(names[k++], 96, "%s", t); free(t); }
        free(name);
        free(io);
    }
    snd_device_name_free_hint(hints);
    return k;
}
void audio_list_devices(void) {
    void **hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) { term_puts("ALSA: список устройств недоступен\n"); return; }
    term_puts("Устройства ALSA (--out=ИМЯ / --mic=ИМЯ, по умолчанию \"default\"):\n");
    for (void **h = hints; *h; h++) {
        char *name = snd_device_name_get_hint(*h, "NAME");
        char *desc = snd_device_name_get_hint(*h, "DESC");
        char *io = snd_device_name_get_hint(*h, "IOID");
        if (name) {
            term_puts("  ");
            term_puts(name);
            term_puts(io ? (strcmp(io, "Input") ? "  [вывод]" : "  [микрофон]") : "  [вывод+микрофон]");
            if (desc) {
                for (char *c = desc; *c; c++) if (*c == '\n') *c = ' ';
                term_puts(" - ");
                term_puts(desc);
            }
            term_puts("\n");
        }
        free(name);
        free(desc);
        free(io);
    }
    snd_device_name_free_hint(hints);
}
#endif
