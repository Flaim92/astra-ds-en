/* streamtest.c — прототип трансляции экрана БЕЗ Discord, только замеры (скрытый флаг --streamtest=N).
 * Захват экрана GDI BitBlt полосами (не DXGI: драйвер видеокарты добавил бы десятки МБ), сразу масштабирование
 * до размера кадра и перевод в YUV420 во входной буфер кодера; кодер — minih264 (H.264 baseline, один поток).
 * Блоки 16x16 без изменений находятся сравнением с прошлым кадром; кадр без изменений не кодируется.
 * Вывод — только счётчик байт: ни файлов, ни сети. Печатает память (пик/средняя), CPU потока, к/с, Мбит/с,
 * время на кадр по этапам. Сцены: desk — настоящий экран; scroll и video — синтетические (в памяти, на экран
 * ничего не выводится), чтобы замер не зависел от того, что сейчас открыто. Linux (XShm) — позже. */
/* Только в «Astra DS Video» (build.sh VIDEO=1 -> -DASTRA_VIDEO); в обычной сборке файла как будто нет. */
#ifdef ASTRA_VIDEO
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "share.h"

#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#define MINIH264_IMPLEMENTATION
#if defined(__GNUC__)
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
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
#include "../third_party/minih264/minih264e.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* Рабочая память кодера (scratch) — по требованию: адреса резервируются, страница выделяется при первом обращении
 * (обработчик исключений). Из ~2 МБ scratch большую часть занимает буфер выхода «на худший случай», а кадр при
 * 1-2 Мбит/с пишет в него десятки КБ — частная память меньше на ~1,5-1,8 МБ. Только свой диапазон адресов. */
static uint8_t *lz_base;
static size_t lz_size;
static PVOID lz_h;
static LONG CALLBACK lz_veh(PEXCEPTION_POINTERS e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    if (!lz_base || a < (uintptr_t)lz_base || a >= (uintptr_t)lz_base + lz_size) return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualAlloc((void *)(a & ~(uintptr_t)4095), 4096, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void *lz_alloc(size_t n) {
    if (lz_base) return NULL;                    /* одна область за раз (одна трансляция или замер) */
    n = (n + 4095) & ~(size_t)4095;
    void *p = VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return NULL;
    lz_h = AddVectoredExceptionHandler(1, lz_veh);
    if (!lz_h) { VirtualFree(p, 0, MEM_RELEASE); return NULL; }
    lz_size = n;
    lz_base = p;
    return p;
}
static size_t lz_committed(void) {               /* сколько страниц реально выделено (для отчёта) */
    size_t c = 0;
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *q = lz_base; lz_base && q < lz_base + lz_size; q += mi.RegionSize) {
        if (!VirtualQuery(q, &mi, sizeof mi)) break;
        if (mi.State == MEM_COMMIT) c += mi.RegionSize;
    }
    return c;
}
static void lz_free(void *p) {
    if (!p || p != lz_base) return;
    MEMORY_BASIC_INFORMATION mi;                 /* затереть только выделенные страницы (невыделенные не трогать) */
    for (uint8_t *q = lz_base; q < lz_base + lz_size; q += mi.RegionSize) {
        if (!VirtualQuery(q, &mi, sizeof mi)) break;
        if (mi.State == MEM_COMMIT) SecureZeroMemory(q, mi.RegionSize);
    }
    lz_base = NULL;
    lz_size = 0;
    VirtualFree(p, 0, MEM_RELEASE);
    if (lz_h) { RemoveVectoredExceptionHandler(lz_h); lz_h = NULL; }
}
static double qpc_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static size_t priv_bytes(void) {
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm)) return 0;
    return pm.PrivateUsage;
}
static double thread_cpu_ms(void) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a, b;
    a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return (double)(a.QuadPart + b.QuadPart) / 10000.0;
}

typedef struct {
    int ow, oh, fps, scene, kbps;                /* scene: 0 desk, 1 scroll, 2 video */
    int sw, sh, sx, sy;                          /* источник: размер и левый верхний угол на экране */
    HDC src;                                     /* экран или синтетический «экран» в памяти */
    HDC vdc; HBITMAP vbm; uint32_t *vpix;        /* синтетика */
    HDC bdc; HBITMAP bbm; uint32_t *band; int bandh;   /* полоса захвата: sw x bandh, 32 бит */
    int kstrips, bpp; DWORD rop;
    int stretch, bandw, band_oy;                 /* stretch: StretchBlt сразу в ширину кадра — полоса в ow, не в sw */                      /* полос по 16 строк кадра на один BitBlt; SRCCOPY [| CAPTUREBLT] */
    int *xmap;
    uint8_t *y, *u, *v;
    uint32_t *mbsig;                             /* отпечаток каждого блока 16x16 прошлого кадра (яркость) */
} ST;

/* Синтетические сцены: прокрутка текста (сдвиг на 3 строки пикселей + новая строка) и «видео» (движение и шум). */
static void scene_step(ST *st, int frame) {
    if (st->scene == 1) {
        BitBlt(st->vdc, 0, 0, st->sw, st->sh - 3, st->vdc, 0, 3, SRCCOPY);
        RECT r = { 0, st->sh - 3, st->sw, st->sh };
        FillRect(st->vdc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (frame % 6 == 0) {
            char t[200];
            int n = snprintf(t, sizeof t, "%05d  The quick brown fox jumps over the lazy dog; съешь же ещё этих мягких булок %d", frame, frame * 7);
            RECT tr = { 20, st->sh - 18, st->sw, st->sh };
            FillRect(st->vdc, &tr, (HBRUSH)GetStockObject(WHITE_BRUSH));
            TextOutA(st->vdc, 20, st->sh - 18, t, n);
        }
    } else if (st->scene == 2) {
        uint32_t seed = (uint32_t)frame * 2654435761u;
        for (int yy = 0; yy < st->sh; yy++) {
            uint32_t *row = st->vpix + (size_t)yy * st->sw;
            for (int xx = 0; xx < st->sw; xx++) {
                seed = seed * 1103515245u + 12345u;
                int r = (xx + frame * 6) & 255, g = (yy + frame * 3) & 255, b = ((xx ^ yy) + frame * 4) & 255;
                int nse = (int)(seed >> 27);
                row[xx] = (uint32_t)(((r + nse) & 255) << 16 | ((g + nse) & 255) << 8 | ((b + nse) & 255));
            }
        }
    }
}

/* Захват полосами + масштаб (ближайший) + YUV420 BT.601; вернуть число изменившихся блоков 16x16. */
static int grab_convert(ST *st, double *t_cap, double *t_conv) {
    int changed = 0, mbw = st->ow / 16, r0 = 0, bh = 0;
    if (!st->bandw) st->bandw = st->stretch ? st->ow : st->sw;
    for (int s = 0; s < st->oh / 16; s++) {
        int oy0 = s * 16;
        double a = qpc_ms();
        if (s % st->kstrips == 0) {              /* новая полоса: kstrips полос кадра одним BitBlt */
            int oe = oy0 + 16 * st->kstrips;
            if (oe > st->oh) oe = st->oh;
            r0 = oy0 * st->sh / st->oh;
            bh = (oe - 1) * st->sh / st->oh - r0 + 1;
            if (r0 + bh > st->sh) bh = st->sh - r0;
            if (bh > st->bandh) bh = st->bandh;
            if (st->stretch) {                   /* масштаб делает GDI: в полосе уже строки кадра */
                int r1 = oe * st->sh / st->oh;
                StretchBlt(st->bdc, 0, 0, st->ow, oe - oy0, st->src, st->sx, st->sy + r0, st->sw, r1 - r0, st->rop);
                st->band_oy = oy0;
            } else BitBlt(st->bdc, 0, 0, st->sw, bh, st->src, st->sx, st->sy + r0, st->rop);
        }
        double b = qpc_ms();
        *t_cap += b - a;
        for (int yy = 0; yy < 16; yy++) {
            int oy = oy0 + yy, sy = st->stretch ? oy - st->band_oy : oy * st->sh / st->oh - r0;
            if (!st->stretch && sy >= bh) sy = bh - 1;
            const uint32_t *srow = st->band + (size_t)sy * st->bandw;
            const uint16_t *srow16 = (const uint16_t *)st->band + (size_t)sy * (size_t)((st->bandw + 1) & ~1);
            uint8_t *yrow = st->y + (size_t)oy * st->ow;
            for (int ox = 0; ox < st->ow; ox++) {
                int R, G, B;
                if (st->bpp == 16) {                 /* 5-5-5: вдвое меньше памяти полосы */
                    unsigned px = srow16[st->xmap[ox]];
                    R = (int)(px >> 7 & 0xF8); G = (int)(px >> 2 & 0xF8); B = (int)(px << 3 & 0xF8);
                } else {
                    uint32_t px = srow[st->xmap[ox]];
                    R = px >> 16 & 255; G = px >> 8 & 255; B = px & 255;
                }
                yrow[ox] = (uint8_t)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
                if (!(oy & 1) && !(ox & 1)) {
                    size_t ci = (size_t)(oy / 2) * (st->ow / 2) + ox / 2;
                    st->u[ci] = (uint8_t)(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
                    st->v[ci] = (uint8_t)(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
                }
            }
        }
        /* блоки 16x16 этой полосы: изменились ли (отпечаток яркости FNV-1a по 4 байта) */
        for (int mx = 0; mx < mbw; mx++) {
            uint32_t h = 2166136261u;
            for (int yy = 0; yy < 16; yy++) {
                const uint8_t *p = st->y + (size_t)(oy0 + yy) * st->ow + (size_t)mx * 16;
                for (int k = 0; k < 16; k += 4) { uint32_t w4; memcpy(&w4, p + k, 4); h = (h ^ w4) * 16777619u; }
            }
            uint32_t *sig = &st->mbsig[(size_t)s * mbw + mx];
            if (*sig != h) { changed++; *sig = h; }
        }
        *t_conv += qpc_ms() - b;
    }
    return changed;
}

/* Кадр и полоса захвата — затереть до освобождения: на экране мог быть пароль. */
static void st_wipe(ST *st) {
    size_t n = (size_t)st->ow * st->oh;
    if (st->y) SecureZeroMemory(st->y, n);
    if (st->u) SecureZeroMemory(st->u, n / 4);
    if (st->v) SecureZeroMemory(st->v, n / 4);
    if (st->band) SecureZeroMemory(st->band, (size_t)(st->bandw ? st->bandw : st->sw) * st->bandh * (size_t)(st->bpp / 8));
}
int streamtest_run(int secs, int ow, int oh, int fps, int scene, int kbps) {
    ST st;
    memset(&st, 0, sizeof st);
    st.ow = ow; st.oh = oh; st.fps = fps; st.scene = scene; st.kbps = kbps;
    size_t mem0 = priv_bytes();
    HDC screen = getenv("ST_DISPLAYDC") ? CreateDCA("DISPLAY", NULL, NULL, NULL) : GetDC(NULL);
    st.sw = GetSystemMetrics(SM_CXSCREEN); st.sh = GetSystemMetrics(SM_CYSCREEN);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    if (scene) {                                 /* синтетический «экран» в памяти */
        st.vdc = CreateCompatibleDC(screen);
        bi.bmiHeader.biWidth = st.sw; bi.bmiHeader.biHeight = -st.sh;
        st.vbm = CreateDIBSection(st.vdc, &bi, DIB_RGB_COLORS, (void **)&st.vpix, NULL, 0);
        SelectObject(st.vdc, st.vbm);
        RECT r = { 0, 0, st.sw, st.sh };
        FillRect(st.vdc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        st.src = st.vdc;
    } else st.src = screen;
    /* полоса ~360 строк источника: 3 BitBlt на кадр 1080p (каждый вызов ~5 мс накладных DWM), 1,33 МБ в 16 бит */
    st.kstrips = getenv("ST_BAND") ? atoi(getenv("ST_BAND")) : 360 * oh / (st.sh * 16);
    if (st.kstrips < 1) st.kstrips = 1;
    st.rop = getenv("ST_CAPTUREBLT") ? SRCCOPY | CAPTUREBLT : SRCCOPY;
    st.stretch = getenv("ST_STRETCH") != NULL;
    if (st.stretch) st.kstrips = getenv("ST_BAND") ? atoi(getenv("ST_BAND")) : 15;   /* 240 строк кадра на вызов */
    if (st.kstrips < 1) st.kstrips = 1;
    st.bandh = st.stretch ? 16 * st.kstrips : 16 * st.kstrips * st.sh / st.oh + 2;
    st.bpp = getenv("ST_BPP") ? atoi(getenv("ST_BPP")) : 16;
    if (st.bpp != 16) st.bpp = 32;
    st.bdc = CreateCompatibleDC(screen);
    st.bandw = st.stretch ? ow : st.sw;
    bi.bmiHeader.biWidth = st.bandw; bi.bmiHeader.biHeight = -st.bandh; bi.bmiHeader.biBitCount = (WORD)st.bpp;
    st.bbm = CreateDIBSection(st.bdc, &bi, DIB_RGB_COLORS, (void **)&st.band, NULL, 0);
    SelectObject(st.bdc, st.bbm);
    SetStretchBltMode(st.bdc, COLORONCOLOR);
    st.xmap = malloc(sizeof(int) * (size_t)ow);
    for (int x = 0; x < ow; x++) st.xmap[x] = st.stretch ? x : x * st.sw / ow;
    st.y = calloc((size_t)ow * oh, 1);
    st.mbsig = calloc((size_t)(ow / 16) * (oh / 16), sizeof(uint32_t));   /* 0 — «ещё не было»: первый кадр весь новый */
    st.u = calloc((size_t)ow * oh / 4, 1); st.v = calloc((size_t)ow * oh / 4, 1);

    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = ow; cp.height = oh; cp.gop = fps * 4;
    cp.vbv_size_bytes = kbps * 1000 / 8;         /* буфер ~1 с */
    cp.fine_rate_control_flag = 0;
    cp.const_input_flag = getenv("ST_CONSTIN") ? 1 : 0;   /* 0: кодер пишет во вход (экономит кадр); сравнение — по своей копии Y */
    int szp = 0, szs = 0;
    if (H264E_sizeof(&cp, &szp, &szs)) { printf("streamtest: кодер не принял параметры\n"); return 1; }
    H264E_persist_t *enc = malloc((size_t)szp);
    H264E_scratch_t *scr = getenv("ST_NOLAZY") ? malloc((size_t)szs) : lz_alloc((size_t)szs);
    bool lazy = scr && scr == (void *)lz_base;
    if (!enc || !scr || H264E_init(enc, &cp)) { printf("streamtest: кодер не создан\n"); return 1; }
    size_t mem_init = priv_bytes();

    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    rp.encode_speed = 8;
    rp.desired_frame_bytes = kbps * 1000 / 8 / fps;
    rp.qp_min = 18; rp.qp_max = 51;
    H264E_io_yuv_t io;
    io.yuv[0] = st.y; io.yuv[1] = st.u; io.yuv[2] = st.v;
    io.stride[0] = ow; io.stride[1] = ow / 2; io.stride[2] = ow / 2;

    int frames = 0, coded = 0, skipped = 0, dropped = 0, nf = secs * fps;
    double bucket = 0, per_frame = kbps * 1000.0 / 8 / fps, cap = kbps * 1000.0 / 8;   /* запас — не больше 1 с потока */
    bool pending = false;                        /* были изменения, но кадр пропущен по битрейту */
    long long bytes = 0;
    double t_cap = 0, t_conv = 0, t_enc = 0, t_gen = 0, max_frame = 0;
    size_t mem_peak = 0; double mem_sum = 0;
    double cpu0 = thread_cpu_ms(), w0 = qpc_ms(), next = w0;
    for (int f = 0; f < nf; f++) {
        double g0 = qpc_ms();
        scene_step(&st, f);
        double g1 = qpc_ms();
        t_gen += g1 - g0;
        double c0 = t_cap, v0 = t_conv;
        int ch = grab_convert(&st, &t_cap, &t_conv);
        double e0 = qpc_ms();
        bucket += per_frame;
        if (bucket > cap) bucket = cap;
        if (ch || pending) {
            if (bucket < 0) { dropped++; pending = true; }   /* поток впереди бюджета — этот кадр не кодируем */
            else {
                unsigned char *out; int n = 0;
                rp.desired_frame_bytes = (int)(per_frame + bucket / 4);
                if (!H264E_encode(enc, scr, &rp, &io, &out, &n)) { bytes += n; bucket -= n; }
                coded++;
                pending = false;
            }
        } else skipped++;
        double e1 = qpc_ms();
        t_enc += e1 - e0;
        double ft = (t_cap - c0) + (t_conv - v0) + (e1 - e0);
        if (ft > max_frame) max_frame = ft;
        frames++;
        size_t m = priv_bytes();
        if (m > mem_peak) mem_peak = m;
        mem_sum += (double)m;
        next += 1000.0 / fps;
        double now = qpc_ms();
        if (next > now) Sleep((DWORD)(next - now));
        else if (now - next > 1000.0) next = now;   /* сильно отстали — не догонять */
    }
    double wall = qpc_ms() - w0, cpu = thread_cpu_ms() - cpu0;
    static const char *const sn[] = { "desk (экран)", "scroll (синтетика)", "video (синтетика)" };
    printf("streamtest %dx%d@%d, %s, %d с, цель %d кбит/с, источник %dx%d, полос на BitBlt %d%s\n", ow, oh, fps, sn[scene], secs, kbps, st.sw, st.sh,
           st.kstrips, st.rop & CAPTUREBLT ? " +CAPTUREBLT" : "");
    if (st.bpp == 16) printf("  полоса 16 бит (5-5-5)\n");
    printf("  кадров %d: закодировано %d, без изменений %d, пропущено по битрейту %d; захват %.1f к/с, отправлено %.1f к/с\n",
           frames, coded, skipped, dropped, frames * 1000.0 / wall, coded * 1000.0 / wall);
    printf("  поток %.2f Мбит/с (%.1f КБ всего)\n", bytes * 8.0 / (wall / 1000.0) / 1e6, bytes / 1024.0);
    printf("  на кадр, мс: захват %.2f, конверсия %.2f, кодирование %.2f (на закодированный %.2f), худший кадр %.1f%s\n",
           t_cap / frames, t_conv / frames, t_enc / frames, coded ? t_enc / coded : 0.0, max_frame, scene ? "" : "");
    if (scene) printf("  (генерация синтетики %.2f мс/кадр — в замер не входит)\n", t_gen / frames);
    printf("  CPU потока %.1f%% одного ядра (%.0f мс за %.0f мс)\n", cpu * 100.0 / wall, cpu, wall);
    printf("  частная память: до %.2f МБ, после создания %.2f МБ, пик %.2f МБ, средняя %.2f МБ\n",
           mem0 / 1048576.0, mem_init / 1048576.0, mem_peak / 1048576.0, mem_sum / frames / 1048576.0);
    printf("  кодер: persist %.2f МБ, scratch %.2f МБ (выделено %.2f МБ%s); буфер YUV %.2f МБ (+отпечатки %.0f КБ), полоса %.2f МБ\n",
           szp / 1048576.0, szs / 1048576.0, (lazy ? lz_committed() : (size_t)szs) / 1048576.0, lazy ? ", по требованию" : "", ow * oh * 1.5 / 1048576.0, (ow / 16) * (oh / 16) * 4 / 1024.0, (double)st.bandw * st.bandh * st.bpp / 8 / 1048576.0);
    st_wipe(&st);
    if (lazy) lz_free(scr); else free(scr);
    free(enc); free(st.xmap); free(st.y); free(st.mbsig); free(st.u); free(st.v);
    DeleteObject(st.bbm); DeleteDC(st.bdc);
    if (st.vdc) { DeleteObject(st.vbm); DeleteDC(st.vdc); }
    ReleaseDC(NULL, screen);
    return 0;
}

/* ======================= Демонстрация экрана (видео-версия): поток захвата и кодирования =======================
 * share_start — источник (окно или весь экран) и режим (0 — 720p 24 к/с, 1 — 480p 15 к/с); всё — в своём потоке,
 * интерфейс только читает share_state. Кадры никуда не пишутся: отправка в Discord (Go Live) — позже, сейчас
 * считаются байты. Сам останавливается, если окно закрыли или экран заблокирован. */
static CRITICAL_SECTION sh_cs;
static bool sh_cs_init;
static HANDLE sh_thr;
static volatile LONG sh_stop;
static ShareState sh;
static HWND sh_hwnd;
static int sh_mode;
static void sh_lock(void) { if (!sh_cs_init) { InitializeCriticalSection(&sh_cs); sh_cs_init = true; } EnterCriticalSection(&sh_cs); }
static void sh_unlock(void) { LeaveCriticalSection(&sh_cs); }

typedef struct { char (*t)[80]; uintptr_t *id; int n, max; DWORD self; } EnumCtx;
static BOOL CALLBACK sh_enum(HWND h, LPARAM lp) {
    EnumCtx *e = (EnumCtx *)lp;
    if (e->n >= e->max || !IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == e->self) return TRUE;             /* своё окно не предлагаем */
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t w[80];
    int n = GetWindowTextW(h, w, 80);
    if (n <= 0) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, e->t[e->n], 80, NULL, NULL);
    e->t[e->n][79] = 0;
    e->id[e->n++] = (uintptr_t)h;
    return TRUE;
}
int share_windows(char (*titles)[80], uintptr_t *ids, int max) {
    EnumCtx e = { titles, ids, 0, max, GetCurrentProcessId() };
    EnumWindows(sh_enum, (LPARAM)&e);
    return e.n;
}
static bool desk_locked(void) {                  /* экран блокировки — другой рабочий стол: читать нельзя */
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) return true;
    CloseDesktop(d);
    return false;
}
static void sh_finish(const char *why) {
    sh_lock();
    sh.active = false;
    snprintf(sh.reason, sizeof sh.reason, "%s", why);
    sh_unlock();
}
static DWORD WINAPI sh_main(LPVOID arg) {
    (void)arg;
    ST st;
    memset(&st, 0, sizeof st);
    HDC screen = GetDC(NULL);
    RECT r = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (sh_hwnd && !GetWindowRect(sh_hwnd, &r)) { ReleaseDC(NULL, screen); sh_finish("окно источника закрыто"); return 0; }
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    st.sx = r.left; st.sy = r.top;
    st.sw = r.right - r.left; st.sh = r.bottom - r.top;
    if (st.sw > GetSystemMetrics(SM_CXSCREEN) - st.sx) st.sw = GetSystemMetrics(SM_CXSCREEN) - st.sx;
    if (st.sh > GetSystemMetrics(SM_CYSCREEN) - st.sy) st.sh = GetSystemMetrics(SM_CYSCREEN) - st.sy;
    int maxw = sh_mode ? 848 : 1280, maxh = sh_mode ? 480 : 720, fps = sh_mode ? 15 : 24, kbps = sh_mode ? 1000 : 2000;
    double sc = (double)maxw / st.sw < (double)maxh / st.sh ? (double)maxw / st.sw : (double)maxh / st.sh;
    if (sc > 1) sc = 1;
    st.ow = ((int)(st.sw * sc)) & ~15; st.oh = ((int)(st.sh * sc)) & ~15;
    if (st.ow < 16 || st.oh < 16) { ReleaseDC(NULL, screen); sh_finish("источник слишком мал"); return 0; }
    st.fps = fps; st.kbps = kbps; st.src = screen; st.bpp = 16; st.rop = SRCCOPY;
    st.stretch = 0;                              /* BitBlt: StretchBlt экономит полосу, но CPU 60-73 % и 14-17 к/с */
    st.kstrips = 360 * st.oh / (st.sh * 16);
    if (st.kstrips < 1) st.kstrips = 1;
    st.bandh = 16 * st.kstrips * st.sh / st.oh + 2;
    st.bandw = st.sw;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biWidth = st.bandw; bi.bmiHeader.biHeight = -st.bandh; bi.bmiHeader.biBitCount = 16;
    st.bdc = CreateCompatibleDC(screen);
    st.bbm = CreateDIBSection(st.bdc, &bi, DIB_RGB_COLORS, (void **)&st.band, NULL, 0);
    SelectObject(st.bdc, st.bbm);
    st.xmap = malloc(sizeof(int) * (size_t)st.ow);
    st.y = calloc((size_t)st.ow * st.oh, 1);
    st.u = calloc((size_t)st.ow * st.oh / 4, 1); st.v = calloc((size_t)st.ow * st.oh / 4, 1);
    st.mbsig = calloc((size_t)(st.ow / 16) * (st.oh / 16), sizeof(uint32_t));
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = st.ow; cp.height = st.oh; cp.gop = fps * 4; cp.vbv_size_bytes = kbps * 1000 / 8;
    int szp = 0, szs = 0;
    H264E_persist_t *enc = NULL;
    H264E_scratch_t *scr = NULL;
    bool ok = st.band && st.xmap && st.y && st.u && st.v && st.mbsig && !H264E_sizeof(&cp, &szp, &szs);
    bool lazy = false;
    if (ok) {
        enc = malloc((size_t)szp);
        scr = lz_alloc((size_t)szs);             /* scratch — по требованию (−1,5 МБ); не вышло — обычный */
        lazy = scr != NULL;
        if (!scr) scr = malloc((size_t)szs);
        ok = enc && scr && !H264E_init(enc, &cp);
    }
    if (ok) {
        for (int x = 0; x < st.ow; x++) st.xmap[x] = x * st.sw / st.ow;
        sh_lock(); sh.w = st.ow; sh.h = st.oh; sh.fps = fps; sh_unlock();
        H264E_run_param_t rp;
        memset(&rp, 0, sizeof rp);
        rp.encode_speed = 8; rp.qp_min = 18; rp.qp_max = 51;
        H264E_io_yuv_t io;
        io.yuv[0] = st.y; io.yuv[1] = st.u; io.yuv[2] = st.v;
        io.stride[0] = st.ow; io.stride[1] = st.ow / 2; io.stride[2] = st.ow / 2;
        double per = kbps * 1000.0 / 8 / fps, cap = kbps * 1000.0 / 8, bucket = 0, tcap = 0, tconv = 0;
        double t0 = qpc_ms(), next = t0, win0 = t0;
        long long bytes = 0, win_bytes = 0;
        int sent = 0, frames = 0, dropped = 0, win_sent = 0;
        bool pending = false;
        const char *why = "трансляция остановлена";
        while (!sh_stop) {
            if (frames % fps == 0) {             /* раз в секунду: окно живо? экран не заблокирован? */
                if (sh_hwnd && (!IsWindow(sh_hwnd) || IsIconic(sh_hwnd))) { why = "окно источника закрыто или свёрнуто"; break; }
                if (desk_locked()) { why = "экран заблокирован"; break; }
                if (sh_hwnd) {                   /* окно сдвинули — следим за ним (размер прежний) */
                    RECT r2;
                    if (GetWindowRect(sh_hwnd, &r2)) { st.sx = r2.left < 0 ? 0 : r2.left; st.sy = r2.top < 0 ? 0 : r2.top; }
                }
            }
            int ch = grab_convert(&st, &tcap, &tconv);
            bucket += per;
            if (bucket > cap) bucket = cap;
            if (ch || pending) {
                if (bucket < 0) { dropped++; pending = true; }
                else {
                    unsigned char *out; int n = 0;
                    rp.desired_frame_bytes = (int)(per + bucket / 4);
                    if (!H264E_encode(enc, scr, &rp, &io, &out, &n)) { bytes += n; win_bytes += n; bucket -= n; }
                    sent++; win_sent++;
                    pending = false;
                }
            }
            frames++;
            double now = qpc_ms();
            if (now - win0 >= 1000) {            /* показатели за последнюю секунду */
                sh_lock();
                sh.fps_out = win_sent * 1000.0 / (now - win0);
                sh.mbps = win_bytes * 8.0 / ((now - win0) / 1000.0) / 1e6;
                sh.frames = frames; sh.sent = sent; sh.dropped = dropped; sh.bytes = bytes;
                sh_unlock();
                win0 = now; win_sent = 0; win_bytes = 0;
            }
            next += 1000.0 / fps;
            now = qpc_ms();
            if (next > now) Sleep((DWORD)(next - now));
            else if (now - next > 1000.0) next = now;
        }
        sh_finish(sh_stop ? "трансляция остановлена" : why);
    } else sh_finish("не удалось запустить кодер");
    st_wipe(&st);
    if (enc) SecureZeroMemory(enc, (size_t)szp);
    if (lazy) lz_free(scr);                      /* затирает выделенные страницы сам */
    else { if (scr) SecureZeroMemory(scr, (size_t)szs); free(scr); }   /* в scratch кодера — куски кадра */
    free(enc); free(st.xmap); free(st.y); free(st.u); free(st.v); free(st.mbsig);
    if (st.bbm) DeleteObject(st.bbm);
    if (st.bdc) DeleteDC(st.bdc);
    ReleaseDC(NULL, screen);
    return 0;
}
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n) {
    ShareState cur;
    share_state(&cur);
    if (cur.active) { snprintf(err, n, "трансляция уже идёт"); return false; }
    if (sh_thr) { WaitForSingleObject(sh_thr, INFINITE); CloseHandle(sh_thr); sh_thr = NULL; }
    if (hwnd && !IsWindow((HWND)hwnd)) { snprintf(err, n, "этого окна уже нет"); return false; }
    sh_lock();
    memset(&sh, 0, sizeof sh);
    sh.active = true;
    sh_unlock();
    sh_hwnd = (HWND)hwnd; sh_mode = mode ? 1 : 0;
    InterlockedExchange(&sh_stop, 0);
    sh_thr = CreateThread(NULL, 256 * 1024, sh_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!sh_thr) { sh_finish("поток не создан"); snprintf(err, n, "не удалось запустить захват"); return false; }
    return true;
}
void share_stop(void) {
    InterlockedExchange(&sh_stop, 1);
    if (sh_thr) { WaitForSingleObject(sh_thr, 3000); CloseHandle(sh_thr); sh_thr = NULL; }
}
void share_state(ShareState *out) {
    sh_lock();
    *out = sh;
    sh_unlock();
}
#ifdef STREAMTEST_MAIN
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10, w = argc > 2 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
    int fps = argc > 4 ? atoi(argv[4]) : 24, scene = argc > 5 ? atoi(argv[5]) : 0, kbps = argc > 6 ? atoi(argv[6]) : 2000;
    SetProcessDPIAware();
    return streamtest_run(secs, w, h, fps, scene, kbps);
}
#endif
#else
#include <stdio.h>
#include <string.h>
#include "share.h"
int streamtest_run(int secs, int ow, int oh, int fps, int scene, int kbps) {
    (void)secs; (void)ow; (void)oh; (void)fps; (void)scene; (void)kbps;
    printf("streamtest: пока только Windows (Linux — XShmGetImage полосами, позже)\n");
    return 1;
}
int share_windows(char (*titles)[80], uintptr_t *ids, int max) { (void)titles; (void)ids; (void)max; return 0; }
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n) { (void)mode; (void)hwnd; snprintf(err, n, "демонстрация экрана пока только в Windows"); return false; }
void share_stop(void) {}
void share_state(ShareState *out) { memset(out, 0, sizeof *out); }
#endif
#endif /* ASTRA_VIDEO */
