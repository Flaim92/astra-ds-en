/* Звук Windows: WASAPI, общий режим, по событиям. Захват 48 кГц моно, вывод 48 кГц стерео;
 * частоту и формат устройства преобразует сама система (AUTOCONVERTPCM). */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "plat.h"

DEFINE_GUID(MC_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(MC_IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(MC_IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(MC_IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(MC_IID_IAudioCaptureClient, 0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
static const PROPERTYKEY MC_PKEY_FriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

struct Audio {
    IMMDeviceEnumerator *en;
    IAudioClient *rc, *cc;
    IAudioRenderClient *render;
    IAudioCaptureClient *cap;
    HANDLE rev, cev;
    UINT32 rbuf;
    int16_t *acc;                                /* накопитель микрофона, моно */
    int accn, acccap;
    bool lost;
    bool com;
};

static void wide_lower(WCHAR *s) { CharLowerW(s); }

static IMMDevice *pick(IMMDeviceEnumerator *en, EDataFlow flow, const char *hint) {
    IMMDevice *dev = NULL;
    if (hint && *hint) {
        WCHAR wh[128];
        MultiByteToWideChar(CP_UTF8, 0, hint, -1, wh, 128);
        wide_lower(wh);
        IMMDeviceCollection *col = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, flow, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0;
            IMMDeviceCollection_GetCount(col, &n);
            for (UINT i = 0; i < n && !dev; i++) {
                IMMDevice *d = NULL;
                if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
                IPropertyStore *ps = NULL;
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && SUCCEEDED(IPropertyStore_GetValue(ps, &MC_PKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                    WCHAR name[256];
                    lstrcpynW(name, v.pwszVal, 256);
                    wide_lower(name);
                    if (wcsstr(name, wh)) { dev = d; d = NULL; }
                }
                PropVariantClear(&v);
                if (ps) IPropertyStore_Release(ps);
                if (d) IMMDevice_Release(d);
            }
            IMMDeviceCollection_Release(col);
        }
    }
    if (!dev) IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, eCommunications, &dev);
    return dev;
}

static IAudioClient *open_client(IMMDevice *dev, int channels, REFERENCE_TIME buf, HANDLE ev) {
    IAudioClient *c = NULL;
    if (FAILED(IMMDevice_Activate(dev, &MC_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&c))) return NULL;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = 48000 * wf.nBlockAlign;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(IAudioClient_Initialize(c, AUDCLNT_SHAREMODE_SHARED, flags, buf, 0, &wf, NULL)) ||
        FAILED(IAudioClient_SetEventHandle(c, ev))) {
        IAudioClient_Release(c);
        return NULL;
    }
    return c;
}

Audio *audio_open(const char *mic_hint, const char *out_hint, char *err, size_t errn) {
    Audio *a = xcalloc(1, sizeof *a);
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    a->com = SUCCEEDED(hr);
    if (FAILED(CoCreateInstance(&MC_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MC_IID_IMMDeviceEnumerator, (void **)&a->en))) {
        snprintf(err, errn, "WASAPI недоступен");
        audio_close(a);
        return NULL;
    }
    a->rev = CreateEventW(NULL, FALSE, FALSE, NULL);
    a->cev = CreateEventW(NULL, FALSE, FALSE, NULL);
    IMMDevice *od = pick(a->en, eRender, out_hint);
    if (od) {
        a->rc = open_client(od, 2, 600000, a->rev);    /* 60 мс буфер вывода: такт 20 мс + запас */
        IMMDevice_Release(od);
    }
    if (!a->rc || FAILED(IAudioClient_GetService(a->rc, &MC_IID_IAudioRenderClient, (void **)&a->render))) {
        snprintf(err, errn, "не открылось устройство вывода");
        audio_close(a);
        return NULL;
    }
    IAudioClient_GetBufferSize(a->rc, &a->rbuf);
    IMMDevice *id = pick(a->en, eCapture, mic_hint);
    if (id) {
        a->cc = open_client(id, 1, 600000, a->cev);
        IMMDevice_Release(id);
        if (a->cc && FAILED(IAudioClient_GetService(a->cc, &MC_IID_IAudioCaptureClient, (void **)&a->cap))) {
            IAudioClient_Release(a->cc);
            a->cc = NULL;
        }
    }
    a->acccap = 48000;
    a->acc = xmalloc((size_t)a->acccap * sizeof(int16_t));
    IAudioClient_Start(a->rc);
    if (a->cc) IAudioClient_Start(a->cc);
    else snprintf(err, errn, "микрофон не найден: только прослушивание");
    return a;
}

/* №69: только вывод — для голосовых сообщений (микрофон не трогаем: индикатор не загорается) */
Audio *audio_open_out(const char *out_hint, char *err, size_t errn) {
    Audio *a = xcalloc(1, sizeof *a);
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    a->com = SUCCEEDED(hr);
    if (FAILED(CoCreateInstance(&MC_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MC_IID_IMMDeviceEnumerator, (void **)&a->en))) {
        snprintf(err, errn, "WASAPI недоступен");
        audio_close(a);
        return NULL;
    }
    a->rev = CreateEventW(NULL, FALSE, FALSE, NULL);
    a->cev = CreateEventW(NULL, FALSE, FALSE, NULL);
    IMMDevice *od = pick(a->en, eRender, out_hint);
    if (od) {
        a->rc = open_client(od, 2, 600000, a->rev);
        IMMDevice_Release(od);
    }
    if (!a->rc || FAILED(IAudioClient_GetService(a->rc, &MC_IID_IAudioRenderClient, (void **)&a->render))) {
        snprintf(err, errn, "не открылось устройство вывода");
        audio_close(a);
        return NULL;
    }
    IAudioClient_GetBufferSize(a->rc, &a->rbuf);
    IAudioClient_Start(a->rc);
    return a;
}

void audio_close(Audio *a) {
    if (!a) return;
    if (a->rc) IAudioClient_Stop(a->rc);
    if (a->cc) IAudioClient_Stop(a->cc);
    if (a->render) IAudioRenderClient_Release(a->render);
    if (a->cap) IAudioCaptureClient_Release(a->cap);
    if (a->rc) IAudioClient_Release(a->rc);
    if (a->cc) IAudioClient_Release(a->cc);
    if (a->en) IMMDeviceEnumerator_Release(a->en);
    if (a->rev) CloseHandle(a->rev);
    if (a->cev) CloseHandle(a->cev);
    if (a->acc) { plat_wipe(a->acc, (size_t)a->acccap * sizeof(int16_t)); free(a->acc); }
    /* после звонка: выгрузить COM-библиотеки звука, которые уже можно выгрузить (AudioSes и т. п.),
     * и вернуть системе освободившиеся страницы кучи — иначе после первого звонка остаётся ~0,5 МБ */
    if (a->com) { CoFreeUnusedLibrariesEx(INFINITE, 0); CoUninitialize(); }
    free(a);
    HeapCompact(GetProcessHeap(), 0);
}

int audio_waitables(Audio *a, Waitable *out, int max) {
    int n = 0;
    if (n < max && a->rev) out[n++] = (Waitable){ (intptr_t)a->rev, W_IN };
    if (n < max && a->cc) out[n++] = (Waitable){ (intptr_t)a->cev, W_IN };
    return n;
}

static void pull_capture(Audio *a) {
    if (!a->cap) return;
    UINT32 pkt = 0;
    for (;;) {
        HRESULT hr = IAudioCaptureClient_GetNextPacketSize(a->cap, &pkt);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { a->lost = true; return; }
        if (FAILED(hr) || pkt == 0) return;
        BYTE *data;
        UINT32 frames;
        DWORD flags;
        if (FAILED(IAudioCaptureClient_GetBuffer(a->cap, &data, &frames, &flags, NULL, NULL))) return;
        if (a->accn + (int)frames > a->acccap) a->accn = 0;   /* не успеваем — сбрасываем старое */
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(a->acc + a->accn, 0, frames * sizeof(int16_t));
        else memcpy(a->acc + a->accn, data, frames * sizeof(int16_t));
        a->accn += (int)frames;
        IAudioCaptureClient_ReleaseBuffer(a->cap, frames);
    }
}

bool audio_read_frame(Audio *a, int16_t *pcm960) {
    pull_capture(a);
    if (a->accn < 960) return false;
    memcpy(pcm960, a->acc, 960 * sizeof(int16_t));
    memmove(a->acc, a->acc + 960, (size_t)(a->accn - 960) * sizeof(int16_t));
    a->accn -= 960;
    return true;
}

int audio_render_space(Audio *a) {
    UINT32 pad = 0;
    HRESULT hr = IAudioClient_GetCurrentPadding(a->rc, &pad);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { a->lost = true; return 0; }
    if (FAILED(hr)) return 0;
    return (int)(a->rbuf - pad);
}

void audio_render_write(Audio *a, const int16_t *stereo, int frames) {
    BYTE *data;
    if (frames <= 0) return;
    HRESULT hr = IAudioRenderClient_GetBuffer(a->render, (UINT32)frames, &data);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { a->lost = true; return; }
    if (FAILED(hr)) return;
    memcpy(data, stereo, (size_t)frames * 4);
    IAudioRenderClient_ReleaseBuffer(a->render, (UINT32)frames, 0);
}

bool audio_lost(Audio *a) { return a->lost; }

/* №140: имена активных устройств (FriendlyName, UTF-8) — для выбора в настройках */
int audio_devices(int out, char (*names)[96], int max) {
    HRESULT ci = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    int k = -1;
    if (SUCCEEDED(CoCreateInstance(&MC_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MC_IID_IMMDeviceEnumerator, (void **)&en))) {
        IMMDeviceCollection *col = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, out ? eRender : eCapture, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0;
            k = 0;
            IMMDeviceCollection_GetCount(col, &n);
            for (UINT i = 0; i < n && k < max; i++) {
                IMMDevice *d = NULL;
                if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
                IPropertyStore *ps = NULL;
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && SUCCEEDED(IPropertyStore_GetValue(ps, &MC_PKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                    char nm[96];
                    int len = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nm, sizeof nm, NULL, NULL);
                    if (len <= 0) {              /* длинное имя: обрезать по символам, а не посреди буквы */
                        WCHAR wb[40];
                        lstrcpynW(wb, v.pwszVal, 40);
                        len = WideCharToMultiByte(CP_UTF8, 0, wb, -1, nm, sizeof nm, NULL, NULL);
                    }
                    if (len > 0) { char *t = san(nm, false); snprintf(names[k++], 96, "%s", t); free(t); }
                }
                PropVariantClear(&v);
                if (ps) IPropertyStore_Release(ps);
                IMMDevice_Release(d);
            }
            IMMDeviceCollection_Release(col);
        }
        IMMDeviceEnumerator_Release(en);
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return k;
}
void audio_list_devices(void) {
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return;
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&MC_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MC_IID_IMMDeviceEnumerator, (void **)&en))) { CoUninitialize(); return; }
    const char *titles[2] = { "Устройства вывода (--out=...):\n", "Микрофоны (--mic=...):\n" };
    EDataFlow flows[2] = { eRender, eCapture };
    for (int f = 0; f < 2; f++) {
        term_puts(titles[f]);
        IMMDeviceCollection *col = NULL;
        if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, flows[f], DEVICE_STATE_ACTIVE, &col))) continue;
        UINT n = 0;
        IMMDeviceCollection_GetCount(col, &n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice *d = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            IPropertyStore *ps = NULL;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && SUCCEEDED(IPropertyStore_GetValue(ps, &MC_PKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                char name[512];
                WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof name, NULL, NULL);
                term_puts("  ");
                term_puts(name);
                term_puts("\n");
            }
            PropVariantClear(&v);
            if (ps) IPropertyStore_Release(ps);
            IMMDevice_Release(d);
        }
        IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
    CoUninitialize();
}
#endif
