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
#define COBJMACROS
#include <windows.h>
#include <psapi.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <opus/opus.h>
#include "golive.h"
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
    if (e->ExceptionRecord->ExceptionInformation[0] > 1) return EXCEPTION_CONTINUE_SEARCH;   /* 0 чтение, 1 запись; выполнение (8) — не наше */
    uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    if (!lz_base || a < (uintptr_t)lz_base || a >= (uintptr_t)lz_base + lz_size) return EXCEPTION_CONTINUE_SEARCH;
    MEMORY_BASIC_INFORMATION mi;                 /* только ещё не выделенная страница: иначе это настоящая ошибка */
    if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualAlloc((void *)(a & ~(uintptr_t)4095), 4096, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void *lz_alloc(size_t n) {
    if (lz_base) return NULL;                    /* одна область за раз (одна трансляция или замер) */
    n = (n + 4095) & ~(size_t)4095;
    void *p = VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return NULL;
    lz_h = AddVectoredExceptionHandler(0, lz_veh);   /* последним: чужие обработчики (панель паники) — раньше */
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
/* Большие буферы движка — прямо у системы (VirtualAlloc, обнулены): куча CRT держала после остановки ~1,4 МБ. */
static void *vm_alloc(size_t n) { return n ? VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL; }
static void vm_free(void *p, size_t n) { if (p) { SecureZeroMemory(p, n); VirtualFree(p, 0, MEM_RELEASE); } }
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
    HWND pw;                                     /* источник «окно»: снимается, только пока его ничто не перекрывает */
    int stretch, bandw, band_oy;                 /* stretch: StretchBlt сразу в ширину кадра — полоса в ow, не в sw */                      /* полос по 16 строк кадра на один BitBlt; SRCCOPY [| CAPTUREBLT] */
    int *xmap;
    uint8_t *y, *u, *v;
    uint32_t *mbsig;                             /* отпечаток каждого блока 16x16 прошлого кадра (яркость) */
    int dx, dy, dw, dh;                          /* место картинки в кадре ow x oh (вокруг — чёрные поля); dw 0 — весь кадр */
    int fit_w, fit_h, band_sh;                   /* для какого размера источника посчитаны dx..dh и полоса */
    HDC pdc; HBITMAP pbm; HGDIOBJ pold; uint32_t *ppix;   /* PrintWindow: DIB размером с окно (GetWindowRect), 32 бит */
    int pdw, pdh, pcx, pcy;                      /* размер DIB и сдвиг видимой части окна (без невидимой рамки) в нём */
    bool from_dib;                               /* этот кадр — из DIB окна, а не из полосы экрана */
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

/* Видимые границы окна (DWM без невидимой рамки и тени), обрезанные по экрану. */
static bool win_rect(HWND h, RECT *r) {
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof *r)) && !GetWindowRect(h, r)) return false;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);   /* все мониторы */
    int vr = vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vb = vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (r->left < vx) r->left = vx;
    if (r->top < vy) r->top = vy;
    if (r->right > vr) r->right = vr;
    if (r->bottom > vb) r->bottom = vb;
    return r->right - r->left >= 16 && r->bottom - r->top >= 16;
}
/* Перекрыто ли окно чем-то выше по Z-порядку (уведомление, другое окно, всплывающее окно расширения...).
 * Не считаются: невидимые и «спрятанные» DWM (другой рабочий стол), свои окна (исключены из захвата WDA),
 * меню (#32768) того же процесса. Всё остальное, что пересекает окно, — пауза. */
static HWND cov_last;                            /* кто перекрыл в последний раз (отчёт замера, индикатор паузы) */
static volatile LONG sh_self_ok;                 /* term_capture_exclude сработал: свои окна в захват не попадают */
void share_self_excluded(bool ok) { InterlockedExchange(&sh_self_ok, ok ? 1 : 0); }
/* «кто перекрыл»: имя программы и класс окна (для индикатора паузы) */
static void cover_name(HWND h, char *out, size_t n) {
    DWORD pid = 0;
    char exe[64] = "?", cls[40] = "";
    GetWindowThreadProcessId(h, &pid);
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (ph) {
        wchar_t w[MAX_PATH];
        DWORD wn = MAX_PATH;
        if (QueryFullProcessImageNameW(ph, 0, w, &wn)) {
            const wchar_t *b = wcsrchr(w, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : w, -1, exe, sizeof exe, NULL, NULL);
            exe[sizeof exe - 1] = 0;
        }
        CloseHandle(ph);
    }
    GetClassNameA(h, cls, sizeof cls);
    snprintf(out, n, "%s (%s)", exe, cls);
}
/* Настоящее перекрытие: общая площадь не меньше 4x4 точек (касание краёв на соседнем мониторе — не перекрытие). */
static bool overlaps(const RECT *a, const RECT *b) {
    RECT x;
    return IntersectRect(&x, a, b) && x.right - x.left >= 4 && x.bottom - x.top >= 4;
}
static bool win_covered(HWND w, const RECT *wr) {
    DWORD self = GetCurrentProcessId(), wpid = 0;
    GetWindowThreadProcessId(w, &wpid);
    /* второй слой: окно на переднем плане. Пуск, центр уведомлений, панель задач, Alt+Tab — пауза всегда;
     * другое окно не-источника — пауза, если пересекается с источником (на случай отдельных полос Z-порядка Windows 11) */
    HWND fg = GetForegroundWindow();
    if (fg && fg != w && GetAncestor(fg, GA_ROOTOWNER) != w) {
        DWORD fpid = 0;
        GetWindowThreadProcessId(fg, &fpid);
        if (!(fpid == self && sh_self_ok)) {
            static const char *const shell[] = { "Windows.UI.Core.CoreWindow", "Shell_TrayWnd", "Shell_SecondaryTrayWnd",
                                                 "XamlExplorerHostIslandWindow", "MultitaskingViewFrame", "TaskSwitcherWnd",
                                                 "ForegroundStaging", "NotifyIconOverflowWindow", "TopLevelWindowForOverflowXamlIsland" };
            char c[64] = "";
            GetClassNameA(fg, c, sizeof c);
            /* оболочка (Пуск, уведомления, Alt+Tab) — только если она на мониторе источника */
            HMONITOR sm = MonitorFromRect(wr, MONITOR_DEFAULTTONEAREST);
            for (size_t i = 0; i < sizeof shell / sizeof *shell; i++)
                if (!strcmp(c, shell[i]) && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == sm) { cov_last = fg; return true; }
            RECT r;
            bool menu = fpid == wpid && !strcmp(c, "#32768");
            if (!menu && (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) || GetWindowRect(fg, &r))
                && overlaps(&r, wr)) { cov_last = fg; return true; }
        }
    }
    int guard = 0;
    for (HWND h = GetWindow(w, GW_HWNDPREV); h && guard < 4096; h = GetWindow(h, GW_HWNDPREV), guard++) {
        if (!IsWindowVisible(h) || IsIconic(h)) continue;
        DWORD cl = 0, pid = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof cl)) && cl) continue;
        GetWindowThreadProcessId(h, &pid);
        if (pid == self && sh_self_ok) continue;
        RECT r;
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && !GetWindowRect(h, &r)) continue;
        if (!overlaps(&r, wr)) continue;
        if (pid == wpid) {
            char c[16];
            if (GetClassNameA(h, c, sizeof c) && !strcmp(c, "#32768")) continue;
        }
        LONG hx = GetWindowLongW(h, GWL_EXSTYLE);
        /* оверлеи игр (Steam, NVIDIA, Game Bar, счётчики к/с): прозрачный для мыши слой поверх — не перекрытие,
         * иначе на DirectX-игре PrintWindow чёрный и эфир стоит на последнем кадре; сам оверлей попадёт в кадр */
        if ((hx & WS_EX_LAYERED) && (hx & WS_EX_TRANSPARENT)) continue;
        if (hx & WS_EX_LAYERED) {                /* полностью прозрачный слой (alpha 0) не виден */
            BYTE a = 255; DWORD fl = 0;
            if (GetLayeredWindowAttributes(h, NULL, &a, &fl) && (fl & LWA_ALPHA) && a == 0) continue;
        }
        cov_last = h;
        return true;
    }
    return guard >= 4096;
}

/* Одна строка картинки (dw точек, источник по xmap) -> строка oy кадра YUV420 BT.601, со сдвигом dx (чётный). */
static void conv_row(ST *st, const uint32_t *s32, const uint16_t *s16, int oy) {
    uint8_t *yrow = st->y + (size_t)oy * st->ow + st->dx;
    bool crow = !(oy & 1);
    uint8_t *ur = st->u + (size_t)(oy / 2) * (st->ow / 2) + st->dx / 2, *vr = st->v + (size_t)(oy / 2) * (st->ow / 2) + st->dx / 2;
    for (int i = 0; i < st->dw; i++) {
        int R, G, B;
        if (s16) {                               /* 5-5-5: вдвое меньше памяти полосы */
            unsigned px = s16[st->xmap[i]];
            R = (int)(px >> 7 & 0xF8); G = (int)(px >> 2 & 0xF8); B = (int)(px << 3 & 0xF8);
        } else {
            uint32_t px = s32[st->xmap[i]];
            R = px >> 16 & 255; G = px >> 8 & 255; B = px & 255;
        }
        yrow[i] = (uint8_t)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
        if (crow && !(i & 1)) {
            ur[i / 2] = (uint8_t)(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
            vr[i / 2] = (uint8_t)(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
        }
    }
}
/* Захват (полосами с экрана или из готового DIB окна) + масштаб (ближайший) + YUV420 в место dx,dy,dw,dh кадра;
 * вернуть число изменившихся блоков 16x16 всего кадра. */
static int grab_convert(ST *st, double *t_cap, double *t_conv) {
    int changed = 0, mbw = st->ow / 16, r0 = 0, bh = 0;
    if (!st->dw) { st->dx = st->dy = 0; st->dw = st->ow; st->dh = st->oh; }
    if (!st->bandw) st->bandw = st->stretch ? st->ow : st->sw;
    double t0 = qpc_ms(), cap = 0;
    if (st->from_dib) {                          /* окно уже целиком в памяти (PrintWindow) */
        for (int i = 0; i < st->dh; i++)
            conv_row(st, st->ppix + (size_t)(st->pcy + i * st->sh / st->dh) * st->pdw + st->pcx, NULL, st->dy + i);
    } else {
        int rows = 16 * st->kstrips;
        for (int i = 0; i < st->dh; i++) {
            if (i % rows == 0) {                 /* новая полоса: rows строк картинки одним BitBlt */
                double a = qpc_ms();
                int ie = i + rows > st->dh ? st->dh : i + rows;
                r0 = i * st->sh / st->dh;
                bh = (ie - 1) * st->sh / st->dh - r0 + 1;
                if (r0 + bh > st->sh) bh = st->sh - r0;
                if (bh > st->bandh) bh = st->bandh;
                if (st->stretch) {               /* масштаб делает GDI: в полосе уже строки кадра */
                    int r1 = ie * st->sh / st->dh;
                    StretchBlt(st->bdc, 0, 0, st->dw, ie - i, st->src, st->sx, st->sy + r0, st->sw, r1 - r0, st->rop);
                    st->band_oy = i;
                } else BitBlt(st->bdc, 0, 0, st->sw, bh, st->src, st->sx, st->sy + r0, st->rop);
                cap += qpc_ms() - a;
            }
            int sy = st->stretch ? i - st->band_oy : i * st->sh / st->dh - r0;
            if (!st->stretch && sy >= bh) sy = bh - 1;
            if (st->bpp == 16) conv_row(st, NULL, (const uint16_t *)st->band + (size_t)sy * (size_t)((st->bandw + 1) & ~1), st->dy + i);
            else conv_row(st, st->band + (size_t)sy * st->bandw, NULL, st->dy + i);
        }
    }
    /* блоки 16x16: изменились ли (отпечаток яркости FNV-1a по 4 байта); поля не меняются — дёшево */
    for (int s = 0; s < st->oh / 16; s++)
        for (int mx = 0; mx < mbw; mx++) {
            uint32_t h = 2166136261u;
            for (int yy = 0; yy < 16; yy++) {
                const uint8_t *p = st->y + (size_t)(s * 16 + yy) * st->ow + (size_t)mx * 16;
                for (int k = 0; k < 16; k += 4) { uint32_t w4; memcpy(&w4, p + k, 4); h = (h ^ w4) * 16777619u; }
            }
            uint32_t *sig = &st->mbsig[(size_t)s * mbw + mx];
            if (*sig != h) { changed++; *sig = h; }
        }
    *t_cap += cap;
    *t_conv += qpc_ms() - t0 - cap;
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
static int share_loop(int n, int mode, int secs);
int streamtest_run(int secs, int ow, int oh, int fps, int scene, int kbps) {
    if (getenv("ST_SHARELOOP")) return share_loop(atoi(getenv("ST_SHARELOOP")), ow < 1280 ? 1 : 0, secs);
    ST st;
    memset(&st, 0, sizeof st);
    st.ow = ow; st.oh = oh; st.fps = fps; st.scene = scene; st.kbps = kbps;
    size_t mem0 = priv_bytes();
    HDC screen = getenv("ST_DISPLAYDC") ? CreateDCA("DISPLAY", NULL, NULL, NULL) : GetDC(NULL);
    st.sw = GetSystemMetrics(SM_CXSCREEN); st.sh = GetSystemMetrics(SM_CYSCREEN);
    if (!scene && getenv("ST_WINDOW")) {         /* замер источника «окно» (PrintWindow) */
        char t[24][80]; uintptr_t id[24];
        int n = share_windows(t, id, 24);
        SetProcessDPIAware();
        for (int i = 0; i < n && !st.pw; i++) if (strstr(t[i], getenv("ST_WINDOW"))) {
            RECT r;
            if (win_rect((HWND)id[i], &r)) {
                st.pw = (HWND)id[i]; st.sx = r.left; st.sy = r.top; st.sw = r.right - r.left; st.sh = r.bottom - r.top;
                printf("  окно: %s (%dx%d в %d,%d)\n", t[i], st.sw, st.sh, st.sx, st.sy);
            }
        }
        if (!st.pw) {
            printf("streamtest: окно '%s' не найдено; есть:\n", getenv("ST_WINDOW"));
            for (int i = 0; i < n; i++) printf("  %s\n", t[i]);
            return 1;
        }
    }
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

    int frames = 0, coded = 0, skipped = 0, dropped = 0, blank = 0, nf = secs * fps;
    double t_cov = 0;
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
        RECT wr = { st.sx, st.sy, st.sx + st.sw, st.sy + st.sh };
        double k0 = qpc_ms();
        bool cov = st.pw && win_covered(st.pw, &wr);
        t_cov += qpc_ms() - k0;
        int ch = grab_convert(&st, &t_cap, &t_conv);
        k0 = qpc_ms();
        if (st.pw && (cov || win_covered(st.pw, &wr))) blank++;
        t_cov += qpc_ms() - k0;
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
    if (st.pw && getenv("ST_ZCHAIN")) {
        int k = 0;
        for (HWND h = GetWindow(st.pw, GW_HWNDPREV); h && k < 4096; h = GetWindow(h, GW_HWNDPREV), k++) {
            char nm[128], t[64] = "";
            DWORD cl = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof cl);
            cover_name(h, nm, sizeof nm);
            GetWindowTextA(h, t, sizeof t);
            if (strstr(nm, "Experience") || strstr(nm, "CoreWindow") || strstr(nm, "Xaml") || strstr(nm, "Multitasking") || strstr(nm, "Shell_") || IsWindowVisible(h))
                printf("  выше: %-60s видимо %d, cloaked %lu, '%s'\n", nm, IsWindowVisible(h) ? 1 : 0, (unsigned long)cl, t);
        }
        printf("  окон выше источника: %d\n", k);
    }
    if (st.pw && cov_last) {
        char t[80] = "", c[64] = "";
        DWORD pid = 0;
        GetWindowTextA(cov_last, t, sizeof t); GetClassNameA(cov_last, c, sizeof c); GetWindowThreadProcessId(cov_last, &pid);
        printf("  перекрывало: класс '%s', заголовок '%s', PID %lu, стиль 0x%lx\n", c, t, (unsigned long)pid, (unsigned long)GetWindowLongW(cov_last, GWL_EXSTYLE));
    }
    if (st.pw) printf("  окно перекрыто (кадр был бы на паузе): %d из %d; проверка перекрытия %.3f мс/кадр\n", blank, frames, t_cov / (frames ? frames : 1));
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
static struct { HWND h; DWORD pid; wchar_t t[80]; } sh_known[24];   /* последний список окон: при старте — то же ли окно */
static int sh_nknown;
static volatile LONG sh_want_idr;               /* Go Live: следующий кадр — ключевой */
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
    if (e->n < 24) { sh_known[e->n].h = h; sh_known[e->n].pid = pid; memcpy(sh_known[e->n].t, w, sizeof w); }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, e->t[e->n], 80, NULL, NULL);
    e->t[e->n][79] = 0;
    e->id[e->n++] = (uintptr_t)h;
    return TRUE;
}
int share_windows(char (*titles)[80], uintptr_t *ids, int max) {
    EnumCtx e = { titles, ids, 0, max, GetCurrentProcessId() };
    memset(sh_known, 0, sizeof sh_known);
    EnumWindows(sh_enum, (LPARAM)&e);
    sh_nknown = e.n < 24 ? e.n : 24;
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
static int sh_band_rows = 1088;                   /* полоса: строк источника на один BitBlt (ST_BANDROWS — замер) */
static int sh_speed = 8, sh_gop_ms = 4000, sh_nodrop = 1, sh_hrt = 1;   /* замер: ST_SPEED, ST_GOP, ST_NODROP, ST_HRT */                        /* замер (share_loop, ST_NOPW=1): окно только BitBlt, как при чёрном PrintWindow */
/* ======================= Звук трансляции (как в Discord) =======================
 * Окно — звук процесса окна и всех его дочерних; весь экран — весь звук системы, кроме нашего процесса (голоса
 * звонка в эфир не попадают, эха нет). WASAPI process loopback (Windows 10 2004+, ActivateAudioInterfaceAsync);
 * нет API — эфир без звука с пояснением (обычный loopback взял бы и звонок). Opus 48 кГц стерео, 20 мс, 128 кбит/с,
 * application AUDIO -> voice_stream_audio. Поток звука живёт только во время эфира, буферы затираются. */
typedef struct { DWORD TargetProcessId; int ProcessLoopbackMode; } SA_LOOPBACK_PARAMS;
typedef struct { int ActivationType; SA_LOOPBACK_PARAMS lp; } SA_ACTIVATION_PARAMS;
enum { SA_ACT_PROCESS_LOOPBACK = 1, SA_MODE_INCLUDE_TREE = 0, SA_MODE_EXCLUDE_TREE = 1 };
static const GUID SA_IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID SA_IID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const GUID SA_IID_Handler = { 0x41d949ab, 0x9862, 0x444a, { 0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb } };
static const GUID SA_IID_IAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
static const GUID SA_IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
typedef HRESULT (WINAPI *SaActivateFn)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);
/* обработчик завершения активации: COM-объект на стеке потока звука (агентный, без счёта ссылок) */
typedef struct { IActivateAudioInterfaceCompletionHandler h; HANDLE done; } SaHandler;
static HRESULT STDMETHODCALLTYPE sa_qi(IActivateAudioInterfaceCompletionHandler *t, REFIID r, void **pp) {
    if (IsEqualGUID(r, &SA_IID_IUnknown) || IsEqualGUID(r, &SA_IID_Handler) || IsEqualGUID(r, &SA_IID_IAgileObject)) { *pp = t; return S_OK; }
    *pp = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sa_ref(IActivateAudioInterfaceCompletionHandler *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE sa_done(IActivateAudioInterfaceCompletionHandler *t, IActivateAudioInterfaceAsyncOperation *op) {
    (void)op;
    SetEvent(((SaHandler *)t)->done);
    return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl sa_vtbl = { sa_qi, sa_ref, sa_ref, sa_done };
static volatile LONG sa_stop;                    /* поток звука: стоп (ставит поток видео перед выходом) */
static volatile LONG sa_want = 1;                /* «Транслировать звук» в форме (по умолчанию вкл) */
void share_audio_enable(bool on) { InterlockedExchange(&sa_want, on ? 1 : 0); }
static struct { double cpu_ms, wall_ms; long long frames, bytes, silent; int peak; } sa_stat;   /* замер */

static void sa_status(bool on, const char *err) {
    sh_lock();
    sh.audio = on;
    snprintf(sh.audio_err, sizeof sh.audio_err, "%s", err ? err : "");
    sh_unlock();
}
/* процесс окна (вкл. дочерние) или всё, кроме нас; false — API нет или не вышло */
static IAudioClient *sa_open(DWORD pid, bool include, HANDLE ev) {
    HMODULE m = LoadLibraryW(L"Mmdevapi.dll");
    SaActivateFn act = m ? (SaActivateFn)(void *)GetProcAddress(m, "ActivateAudioInterfaceAsync") : NULL;
    if (!act) return NULL;
    SA_ACTIVATION_PARAMS ap;
    memset(&ap, 0, sizeof ap);
    ap.ActivationType = SA_ACT_PROCESS_LOOPBACK;
    ap.lp.TargetProcessId = pid;
    ap.lp.ProcessLoopbackMode = include ? SA_MODE_INCLUDE_TREE : SA_MODE_EXCLUDE_TREE;
    PROPVARIANT pv;
    memset(&pv, 0, sizeof pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof ap;
    pv.blob.pBlobData = (BYTE *)&ap;
    SaHandler hd;
    hd.h.lpVtbl = &sa_vtbl;
    hd.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    IAudioClient *c = NULL;
    if (hd.done && SUCCEEDED(act(L"VAD\\Process_Loopback", &SA_IID_IAudioClient, &pv, &hd.h, &op)) && op
        && WaitForSingleObject(hd.done, 5000) == WAIT_OBJECT_0) {
        HRESULT hr = E_FAIL;
        IUnknown *u = NULL;
        if (SUCCEEDED(IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &hr, &u)) && SUCCEEDED(hr) && u) {
            IUnknown_QueryInterface(u, &SA_IID_IAudioClient, (void **)&c);
            IUnknown_Release(u);
        }
    }
    if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
    if (hd.done) CloseHandle(hd.done);
    if (!c) return NULL;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 48000 * 4;
    DWORD fl = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (FAILED(IAudioClient_Initialize(c, AUDCLNT_SHAREMODE_SHARED, fl, 200000, 0, &wf, NULL))
        && FAILED(IAudioClient_Initialize(c, AUDCLNT_SHAREMODE_SHARED, fl | 0x80000000 /* AUTOCONVERTPCM */ | 0x08000000, 200000, 0, &wf, NULL))) {
        IAudioClient_Release(c);
        return NULL;
    }
    if (FAILED(IAudioClient_SetEventHandle(c, ev))) { IAudioClient_Release(c); return NULL; }
    return c;
}
static DWORD WINAPI sa_main(LPVOID arg) {
    (void)arg;
    double cpu0 = thread_cpu_ms(), w0 = qpc_ms();
    memset(&sa_stat, 0, sizeof sa_stat);
    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    DWORD pid = 0;
    if (sh_hwnd) GetWindowThreadProcessId(sh_hwnd, &pid);
    else pid = GetCurrentProcessId();
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient *c = ev ? sa_open(pid, sh_hwnd != NULL, ev) : NULL;
    IAudioCaptureClient *cc = NULL;
    int esz = opus_encoder_get_size(2);
    OpusEncoder *enc = c ? vm_alloc((size_t)esz) : NULL;
    int16_t *pcm = c ? vm_alloc(960 * 2 * sizeof(int16_t)) : NULL;   /* кадр 20 мс стерео */
    uint8_t *pkt = c ? vm_alloc(1500) : NULL;
    bool ok = c && enc && pcm && pkt && SUCCEEDED(IAudioClient_GetService(c, &SA_IID_IAudioCaptureClient, (void **)&cc))
              && opus_encoder_init(enc, 48000, 2, OPUS_APPLICATION_AUDIO) == OPUS_OK;
    if (ok) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));   /* ~вдвое дешевле 10, для музыки/игры разницы почти нет */
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        ok = SUCCEEDED(IAudioClient_Start(c));
    }
    if (!ok) sa_status(false, c ? "звук трансляции не запустился" : "звук недоступен: нужна Windows 10 2004 или новее");
    else {
        sa_status(true, NULL);
        int fill = 0, loud = 0;                  /* сколько отсчётов (на канал) уже в кадре; макс. уровень кадра */
        u32 ts = 0;
        double t0 = qpc_ms(), last = t0;
        while (!sa_stop) {
            WaitForSingleObject(ev, 100);
            UINT32 np = 0;
            while (!sa_stop && SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &np)) && np) {
                BYTE *d = NULL;
                UINT32 nf = 0;
                DWORD fl = 0;
                if (FAILED(IAudioCaptureClient_GetBuffer(cc, &d, &nf, &fl, NULL, NULL))) break;
                double now = qpc_ms();
                if (now - last > 60) {           /* была тишина (пакетов нет) — отметка времени догоняет часы */
                    u32 tn = (u32)((now - t0) * 48.0);
                    if ((int32_t)(tn - ts) > 0) ts = tn - (tn % 960);
                }
                last = now;
                for (UINT32 i = 0; i < nf; i++) {
                    if ((fl & AUDCLNT_BUFFERFLAGS_SILENT) || !d) { pcm[fill * 2] = 0; pcm[fill * 2 + 1] = 0; }
                    else {
                        memcpy(&pcm[fill * 2], d + (size_t)i * 4, 4);
                        int a = abs(pcm[fill * 2]), b = abs(pcm[fill * 2 + 1]);
                        if (a > loud) loud = a;
                        if (b > loud) loud = b;
                    }
                    if (++fill == 960) {         /* цифровая тишина — не кодировать и не слать (как голос без речи) */
                        int n = loud > 2 ? opus_encode(enc, pcm, 960, pkt, 1500) : 0;
                        if (loud <= 2) sa_stat.silent++;
                        if (loud > sa_stat.peak) sa_stat.peak = loud;
                        loud = 0;
                        if (n > 0) { voice_stream_audio(pkt, (size_t)n, ts); sa_stat.frames++; sa_stat.bytes += n; }
                        ts += 960;
                        fill = 0;
                    }
                }
                IAudioCaptureClient_ReleaseBuffer(cc, nf);
            }
        }
        IAudioClient_Stop(c);
    }
    sa_stat.cpu_ms = thread_cpu_ms() - cpu0; sa_stat.wall_ms = qpc_ms() - w0;
    if (cc) IAudioCaptureClient_Release(cc);
    if (c) IAudioClient_Release(c);
    vm_free(enc, (size_t)esz); vm_free(pcm, 960 * 2 * sizeof(int16_t)); vm_free(pkt, 1500);   /* затирают */
    if (ev) CloseHandle(ev);
    if (com) CoUninitialize();
    return 0;
}
/* Выход всегда 1280x720 @ 30 к/с, как у Discord: картинка вписана с сохранением пропорций, без апскейла, поля чёрные.
 * Кодер получает один и тот же размер весь эфир — смена размера окна не трогает поток Go Live. */
#define SH_OW 1280
#define SH_OH 720
#define SH_FPS 30
/* PrintWindow пишет только в 32-битный DIB размером со всё окно: так снимаем окна до 1920x1080 (DIB <= 7,9 МБ) —
 * их видно и под другими окнами. Больше (2K/4K) — BitBlt видимой части полосами (~1-3 МБ), перекрыли — последний кадр. */
#define SH_PW_MAXPIX (1920 * 1080)
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
/* Место картинки в кадре для источника sw x sh; при смене — кадр залить чёрным (Y 16, U/V 128), отпечатки сбросить. */
static void sh_fit(ST *st) {
    if (st->fit_w == st->sw && st->fit_h == st->sh) return;
    double kx = (double)st->ow / st->sw, ky = (double)st->oh / st->sh, sc = kx < ky ? kx : ky;
    if (sc > 1) sc = 1;
    st->dw = (int)(st->sw * sc + 0.5) & ~1; st->dh = (int)(st->sh * sc + 0.5) & ~1;
    if (st->dw < 2) st->dw = 2;
    if (st->dh < 2) st->dh = 2;
    if (st->dw > st->ow) st->dw = st->ow;
    if (st->dh > st->oh) st->dh = st->oh;
    st->dx = ((st->ow - st->dw) / 2) & ~1; st->dy = ((st->oh - st->dh) / 2) & ~1;
    size_t n = (size_t)st->ow * st->oh;
    memset(st->y, 16, n); memset(st->u, 128, n / 4); memset(st->v, 128, n / 4);
    memset(st->mbsig, 0, sizeof(uint32_t) * (size_t)(st->ow / 16) * (st->oh / 16));
    for (int x = 0; x < st->dw; x++) st->xmap[x] = x * st->sw / st->dw;
    st->fit_w = st->sw; st->fit_h = st->sh;
}
static HGDIOBJ sh_bold;                          /* исходный 1x1 в DC полосы — вернуть перед DeleteObject */
static void sh_band_free(ST *st) {               /* полоса экрана: затереть и освободить */
    if (!st->bbm) return;
    SecureZeroMemory(st->band, (size_t)((st->bandw + 1) & ~1) * (size_t)st->bandh * 2);
    SelectObject(st->bdc, sh_bold);
    DeleteObject(st->bbm); st->bbm = NULL; st->band = NULL; st->band_sh = 0;
}
static bool sh_band(ST *st, HDC screen) {
    if (st->bbm && st->bandw == st->sw && st->band_sh == st->sh) return true;
    sh_band_free(st);
    int rows = sh_band_rows, lim = (int)((size_t)(4400 * 1024) / ((size_t)((st->sw + 1) & ~1) * 2));   /* полоса <= ~4,3 МБ */
    if (rows > lim) rows = lim;
    st->kstrips = rows * st->dh / (st->sh * 16);   /* строк источника на один BitBlt: каждый вызов ~5-8 мс ожидания DWM */
    if (st->kstrips < 1) st->kstrips = 1;
    st->bandh = 16 * st->kstrips * st->sh / st->dh + 2;
    st->bandw = st->sw; st->band_sh = st->sh;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biWidth = st->bandw; bi.bmiHeader.biHeight = -st->bandh; bi.bmiHeader.biBitCount = 16;
    if (!st->bdc) st->bdc = CreateCompatibleDC(screen);
    st->bbm = CreateDIBSection(st->bdc, &bi, DIB_RGB_COLORS, (void **)&st->band, NULL, 0);
    if (!st->bbm || !st->band) { if (st->bbm) DeleteObject(st->bbm); st->bbm = NULL; st->band = NULL; st->band_sh = 0; return false; }
    HGDIOBJ o = SelectObject(st->bdc, st->bbm);
    if (!sh_bold) sh_bold = o;
    return true;
}
static void sh_pdib_free(ST *st) {               /* DIB окна: затереть и освободить */
    if (!st->pbm) return;
    SecureZeroMemory(st->ppix, (size_t)st->pdw * st->pdh * 4);
    SelectObject(st->pdc, st->pold);
    DeleteObject(st->pbm);
    st->pbm = NULL; st->ppix = NULL; st->pdw = st->pdh = 0;
}
static bool sh_pdib(ST *st, int w, int h, HDC screen) {
    if (st->pbm && st->pdw == w && st->pdh == h) return true;
    sh_pdib_free(st);
    if (w < 16 || h < 16 || (size_t)w * (size_t)h > SH_PW_MAXPIX) return false;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biBitCount = 32;
    if (!st->pdc) st->pdc = CreateCompatibleDC(screen);
    st->pbm = CreateDIBSection(st->pdc, &bi, DIB_RGB_COLORS, (void **)&st->ppix, NULL, 0);
    if (!st->pbm || !st->ppix) { if (st->pbm) DeleteObject(st->pbm); st->pbm = NULL; st->ppix = NULL; return false; }
    st->pold = SelectObject(st->pdc, st->pbm);
    st->pdw = w; st->pdh = h;
    return true;
}
/* Видимая часть DIB целиком чёрная (выборка ~40x30 точек): так PrintWindow отвечает на часть DirectX-окон. */
static bool dib_black(const ST *st) {
    int stx = st->sw / 40 + 1, sty = st->sh / 30 + 1;
    for (int y = 0; y < st->sh; y += sty) {
        const uint32_t *r = st->ppix + (size_t)(st->pcy + y) * st->pdw + st->pcx;
        for (int x = 0; x < st->sw; x += stx) if (r[x] & 0xFFFFFF) return false;
    }
    return true;
}
/* Окно целиком через PrintWindow (видно и под другими окнами, и за краем экрана). false — велико, не вышло
 * или чёрное: тогда BitBlt с экрана. */
static bool sh_printwin(ST *st, HDC screen, double *tcap) {
    RECT wr, ext, v;
    if (!GetWindowRect(st->pw, &wr)) return false;
    if (FAILED(DwmGetWindowAttribute(st->pw, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof ext))) ext = wr;
    if (!IntersectRect(&v, &ext, &wr) || v.right - v.left < 16 || v.bottom - v.top < 16) return false;
    if (!sh_pdib(st, wr.right - wr.left, wr.bottom - wr.top, screen)) return false;
    st->pcx = v.left - wr.left; st->pcy = v.top - wr.top;
    st->sw = v.right - v.left; st->sh = v.bottom - v.top;
    double a = qpc_ms();
    BOOL ok = PrintWindow(st->pw, st->pdc, PW_RENDERFULLCONTENT);
    GdiFlush();
    *tcap += qpc_ms() - a;
    if (!ok || dib_black(st)) return false;
    sh_fit(st);
    st->from_dib = true;
    return true;
}
static struct {                                  /* замер (share_loop): время этапов, выбросы ведра, провалы и джиттер такта */
    double cap, conv, enc, enc_max, gap_max, late_max, key_bytes;
    int n, coded, keys, drop, drop_idr, gaps40, late5, np, pmax, frozen, nband, npw, rcov, rpost, rmove, roff;
    double pbytes;
} sh_stat;
static bool sh_test_nopw;
static DWORD WINAPI sh_main(LPVOID arg) {
    (void)arg;
    ST st;
    memset(&st, 0, sizeof st);
    /* физические точки: иначе при масштабе Windows 125-150% GetWindowRect и рамки DWM в разных единицах */
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   /* игра грузит ядра: такт эфира не должен ждать */
    HDC screen = GetDC(NULL);
    if (sh_hwnd && !IsWindow(sh_hwnd)) { ReleaseDC(NULL, screen); sh_finish("окно источника закрыто"); return 0; }
    st.pw = sh_hwnd;
    st.sw = GetSystemMetrics(SM_CXSCREEN); st.sh = GetSystemMetrics(SM_CYSCREEN);   /* весь экран (основной монитор) */
    if (st.sw < 16 || st.sh < 16 || st.sw > 16384 || st.sh > 16384) { ReleaseDC(NULL, screen); sh_finish("источник слишком мал"); return 0; }
    int fps = SH_FPS, kbps = sh_mode ? 1000 : 1800;
    int kbps_max = sh_mode ? 2000 : 3500;        /* пик за любую секунду: цель + запас ведра */
    st.ow = SH_OW; st.oh = SH_OH;
    st.fps = fps; st.kbps = kbps; st.src = screen; st.bpp = 16; st.rop = SRCCOPY;
    st.stretch = 0;                              /* BitBlt: StretchBlt экономит полосу, но CPU 60-73 % и 14-17 к/с */
    st.xmap = malloc(sizeof(int) * (size_t)st.ow);
    size_t ysz = (size_t)st.ow * st.oh, csz = ysz / 4;
    st.y = vm_alloc(ysz);
    st.u = vm_alloc(csz); st.v = vm_alloc(csz);
    st.mbsig = calloc((size_t)(st.ow / 16) * (st.oh / 16), sizeof(uint32_t));
    if (st.y && st.u && st.v) { memset(st.y, 16, ysz); memset(st.u, 128, csz); memset(st.v, 128, csz); }   /* чёрный, не зелёный */
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = st.ow; cp.height = st.oh; cp.gop = fps * 2; cp.vbv_size_bytes = kbps_max * 1000 / 8;
    int szp = 0, szs = 0;
    H264E_persist_t *enc = NULL;
    H264E_scratch_t *scr = NULL;
    bool ok = st.xmap && st.y && st.u && st.v && st.mbsig && !H264E_sizeof(&cp, &szp, &szs);
    bool lazy = false;
    if (ok) {
        enc = vm_alloc((size_t)szp);
        scr = lz_alloc((size_t)szs);             /* scratch — по требованию (−1,5 МБ); не вышло — обычный */
        lazy = scr != NULL;
        if (!scr) scr = malloc((size_t)szs);
        ok = enc && scr && !H264E_init(enc, &cp);
    }
    HANDLE sa_thr = NULL;
    if (ok) {
        sh_lock(); sh.w = st.ow; sh.h = st.oh; sh.fps = fps; sh_unlock();
        InterlockedExchange(&sa_stop, 0);
        if (sa_want) sa_thr = CreateThread(NULL, 256 * 1024, sa_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        else sa_status(false, "без звука");
        H264E_run_param_t rp;
        memset(&rp, 0, sizeof rp);
        rp.encode_speed = sh_speed; rp.qp_min = 18; rp.qp_max = 51;
        int qpm = 18;                            /* нижняя граница qp: растёт при долге ведра (качество вместо выброса) */
        HANDLE tmr = sh_hrt ? CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS) : NULL;
        H264E_io_yuv_t io;
        io.yuv[0] = st.y; io.yuv[1] = st.u; io.yuv[2] = st.v;
        io.stride[0] = st.ow; io.stride[1] = st.ow / 2; io.stride[2] = st.ow / 2;
        double per = kbps * 1000.0 / 8 / fps, cap = (kbps_max - kbps) * 1000.0 / 8, bucket = 0, tcap = 0, tconv = 0;
        double t0 = qpc_ms(), next = t0, win0 = t0, pw_retry = 0, last_pw = 0, last_band = 0;
        long long bytes = 0, win_bytes = 0;
        int sent = 0, frames = 0, dropped = 0, win_sent = 0;
        bool pending = false, paused = false, tainted = false, was_mini = false;
        double last_idr = -1e9, last_out = -1e9;
        const char *why = "трансляция остановлена";
        memset(&sh_stat, 0, sizeof sh_stat);
        double last_sent = 0;
        while (!sh_stop) {
            { double lt = qpc_ms() - next; if (frames && lt > sh_stat.late_max) sh_stat.late_max = lt; if (frames && lt > 5) sh_stat.late5++; }
            double c_cap = tcap, c_conv = tconv;
            if (frames % fps == 0) {             /* раз в секунду: окно живо? экран не заблокирован? */
                if (sh_hwnd && !IsWindow(sh_hwnd)) { why = "окно источника закрыто"; break; }
                if (desk_locked()) { why = "экран заблокирован"; break; }
            }
            /* окно, видимое и ничем не перекрытое, — BitBlt с экрана (1080p одним вызовом ~16 мс, PrintWindow ~28 мс);
             * перекрыто, за краем или сдвинули во время захвата — PrintWindow (окно <= 1920x1080 и не чёрное);
             * не вышло или свёрнуто — зрители видят последний кадр */
            bool blank = false, mini = false;
            const char *pwhy = "";
            RECT wr;
            int ch = 0;
            double tq = qpc_ms();
            st.from_dib = false;
            if (st.pw && IsIconic(st.pw)) { blank = mini = true; pwhy = "окно свёрнуто"; }
            else if (st.pw) {
                RECT ext;
                bool onscr = win_rect(st.pw, &wr) && SUCCEEDED(DwmGetWindowAttribute(st.pw, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof ext))
                             && EqualRect(&ext, &wr), done = false;
                if (onscr && !win_covered(st.pw, &wr)) {
                    st.sx = wr.left; st.sy = wr.top;
                    st.sw = wr.right - wr.left; st.sh = wr.bottom - wr.top;
                    sh_fit(&st);
                    if (!sh_band(&st, screen)) { why = "не хватило памяти под новый размер окна"; break; }
                    ch = grab_convert(&st, &tcap, &tconv);
                    RECT w2;                     /* после захвата: перекрыли или сдвинули — в кадре может быть чужое */
                    if (win_covered(st.pw, &wr)) { pwhy = "перекрыто во время захвата"; sh_stat.rpost++; }
                    else if (!win_rect(st.pw, &w2) || !EqualRect(&w2, &wr)) { pwhy = "окно сдвинули во время захвата"; sh_stat.rmove++; }
                    else { done = true; last_band = tq; sh_stat.nband++; }
                } else { pwhy = onscr ? "перекрыто" : "окно за краем экрана"; if (onscr) sh_stat.rcov++; else sh_stat.roff++; }
                if (!done && !sh_test_nopw && tq >= pw_retry) {
                    if (sh_printwin(&st, screen, &tcap)) { ch = grab_convert(&st, &tcap, &tconv); done = true; pwhy = ""; last_pw = tq; sh_stat.npw++; }
                    else pw_retry = tq + 2000;   /* велико / чёрное / не вышло: снова через 2 с */
                }
                if (!done) blank = true;
                /* буфер, которым не пользовались 3 с, — затереть и вернуть (оба сразу держим, только пока окно то
                 * перекрывают, то открывают) */
                if (st.pbm && tq - last_pw > 3000) sh_pdib_free(&st);
                if (st.bbm && tq - last_band > 3000) sh_band_free(&st);
            } else {                             /* весь экран */
                sh_fit(&st);
                if (!sh_band(&st, screen)) { why = "не хватило памяти под полосу захвата"; break; }
                ch = grab_convert(&st, &tcap, &tconv);
            }
            if (blank && st.pw) {                /* кто перекрыл — для индикатора */
                char nm[64] = "";
                if (strstr(pwhy, "перекрыто")) { if (cov_last && IsWindow(cov_last)) cover_name(cov_last, nm, sizeof nm); }
                DWORD cvp = 0;
                if (cov_last) GetWindowThreadProcessId(cov_last, &cvp);
                sh_lock();
                snprintf(sh.cover, sizeof sh.cover, "%s", nm);
                snprintf(sh.pause_why, sizeof sh.pause_why, "%s%s", pwhy, !mini && cvp == GetCurrentProcessId() && !sh_self_ok ? " (своё окно, не исключено из захвата)" : "");
                sh_unlock();
            }
            if (mini != was_mini) {              /* свернули — последний кадр; развернули — сразу ключевой */
                was_mini = mini;
                sh_lock(); sh.minimized = mini; sh_unlock();
                if (!mini) InterlockedExchange(&sh_want_idr, 1);
            }
            if (blank && pwhy[0] && strstr(pwhy, "во время захвата")) tainted = true;
            else if (!blank) tainted = false;
            if (blank) { ch = 0; pending = false; }
            if (blank != paused) { paused = blank; sh_lock(); sh.paused = blank; sh_unlock(); }
            bucket += per;
            if (bucket > cap) bucket = cap;
            double tn = qpc_ms();
            bool want_key = InterlockedExchange(&sh_want_idr, 0) || tn - last_idr >= sh_gop_ms;
            /* пауза: поток не прерывается — повтор последнего чистого кадра (P-skip) не реже 10 к/с и IDR по запросу;
             * если буфер испорчен перекрытием во время захвата — молчим до следующего чистого кадра */
            bool keepalive = !tainted && tn - last_out >= 100;
            if (tainted && want_key) InterlockedExchange(&sh_want_idr, 1);   /* ключевой — с первым чистым кадром */
            if (ch || pending || (!tainted && want_key) || keepalive) {
                if (sh_nodrop) {                 /* долг ведра — хуже качество следующих кадров, а не выброс:
                                                  * +1 к нижней границе qp за каждый кадр бюджета долга (плавно, без качелей) */
                    double debt = bucket < 0 ? -bucket / per : 0;
                    qpm = 18 + (debt > 33 ? 33 : (int)debt);
                    rp.qp_min = qpm;
                }
                if (bucket < (sh_nodrop ? -2 * cap : 0) && !want_key) { dropped++; pending = true; sh_stat.drop++; if (tn - last_idr < 500) sh_stat.drop_idr++; }
                else {
                    unsigned char *out; int n = 0;
                    /* постоянная цель: у minih264 своя модель буфера (учитывает и ключевые); качели цели сбивали её */
                    rp.desired_frame_bytes = sh_nodrop ? (int)per : (int)(per + bucket / 4);
                    rp.frame_type = want_key ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
                    if (want_key) last_idr = tn;
                    last_out = tn;
                    double e0 = qpc_ms();
                    int er = H264E_encode(enc, scr, &rp, &io, &out, &n);
                    double e1 = qpc_ms();
                    sh_stat.enc += e1 - e0; sh_stat.coded++;
                    if (e1 - e0 > sh_stat.enc_max) sh_stat.enc_max = e1 - e0;
                    if (last_sent > 0 && !blank) { double g = e1 - last_sent; if (g > sh_stat.gap_max) sh_stat.gap_max = g; if (g > 40) sh_stat.gaps40++; }
                    last_sent = e1;
                    if (want_key) { sh_stat.keys++; sh_stat.key_bytes += n; }
                    else if (ch) { sh_stat.np++; sh_stat.pbytes += n; if (n > sh_stat.pmax) sh_stat.pmax = n; if (n < 300) sh_stat.frozen++; }
                    if (!er && n > 0) {
                        bytes += n; win_bytes += n; bucket -= n;
                        bool key = false;            /* ключевой кадр: есть NAL IDR (тип 5) */
                        for (int i = 0; i + 3 < n && !key; i++)
                            if (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 1 && (out[i + 3] & 31) == 5) key = true;
                        golive_frame(out, (size_t)n, key, (u32)((qpc_ms() - t0) * 90.0));
                    }
                    sent++; win_sent++;
                    pending = false;
                }
            }
            frames++;
            sh_stat.n++; sh_stat.cap += tcap - c_cap; sh_stat.conv += tconv - c_conv;
            double now = qpc_ms();
            if (now - win0 >= 1000) {            /* показатели за последнюю секунду */
                sh_lock();
                sh.fps_out = win_sent * 1000.0 / (now - win0);
                sh.mbps = win_bytes * 8.0 / ((now - win0) / 1000.0) / 1e6;
                sh.frames = frames; sh.sent = sent; sh.dropped = dropped; sh.bytes = bytes;
                sh.dib = st.pbm != NULL && st.from_dib;
                sh_unlock();
                win0 = now; win_sent = 0; win_bytes = 0;
            }
            next += 1000.0 / fps;
            now = qpc_ms();
            if (next > now) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((next - now) * 10000.0);
                if (tmr && SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(tmr, 1000);
                else Sleep((DWORD)(next - now));
            } else if (now - next > 1000.0) next = now;
        }
        if (tmr) CloseHandle(tmr);
        InterlockedExchange(&sa_stop, 1);        /* звук: до конца, его буферы затираются в его потоке */
        if (sa_thr) { WaitForSingleObject(sa_thr, INFINITE); CloseHandle(sa_thr); }
        sh_finish(sh_stop ? "трансляция остановлена" : why);
    } else sh_finish("не удалось запустить кодер");
    sh_band_free(&st);
    sh_pdib_free(&st);
    st_wipe(&st);
    if (enc) SecureZeroMemory(enc, (size_t)szp);
    if (lazy) lz_free(scr);                      /* затирает выделенные страницы сам */
    else { if (scr) SecureZeroMemory(scr, (size_t)szs); free(scr); }   /* в scratch кодера — куски кадра */
    vm_free(enc, (size_t)szp); vm_free(st.y, ysz); vm_free(st.u, csz); vm_free(st.v, csz);
    st.y = st.u = st.v = NULL;
    free(st.xmap); free(st.mbsig);
    if (st.bdc) DeleteDC(st.bdc);
    if (st.pdc) DeleteDC(st.pdc);
    sh_bold = NULL;
    ReleaseDC(NULL, screen);
    return 0;
}
/* Go Live (прогон 3): зритель ждёт ключевой кадр. golive просит IDR (SPS+PPS+IDR) когда эфир готов; кроме того —
 * IDR не реже раза в 2 с по времени и повтор кадра (P-skip) не реже 10 к/с, если картинка не меняется. */
void share_request_idr(void) { InterlockedExchange(&sh_want_idr, 1); }
bool share_start(int mode, uintptr_t hwnd, char *err, size_t n) {
    ShareState cur;
    share_state(&cur);
    if (cur.active) { snprintf(err, n, "трансляция уже идёт"); return false; }
    if (sh_thr) { WaitForSingleObject(sh_thr, INFINITE); CloseHandle(sh_thr); sh_thr = NULL; }
    if (hwnd) {                                  /* то же окно, что было в списке: тот же процесс и заголовок */
        HWND h = (HWND)hwnd;
        int k = -1;
        for (int i = 0; i < sh_nknown; i++) if (sh_known[i].h == h) k = i;
        DWORD pid = 0;
        wchar_t w[80];
        memset(w, 0, sizeof w);
        if (!IsWindow(h) || k < 0) { snprintf(err, n, "этого окна уже нет"); return false; }
        GetWindowThreadProcessId(h, &pid);
        GetWindowTextW(h, w, 80);
        if (pid != sh_known[k].pid || wcscmp(w, sh_known[k].t)) { snprintf(err, n, "окно изменилось - выберите источник заново"); return false; }
    }
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
void share_stop_nowait(void) {                   /* паника: флаг + не дольше 150 мс; не вышел — ничего не освобождаем */
    InterlockedExchange(&sh_stop, 1);
    if (sh_thr && WaitForSingleObject(sh_thr, 150) == WAIT_OBJECT_0) { CloseHandle(sh_thr); sh_thr = NULL; }
}
void share_stop(void) {
    InterlockedExchange(&sh_stop, 1);
    if (sh_thr) { WaitForSingleObject(sh_thr, INFINITE); CloseHandle(sh_thr); sh_thr = NULL; }   /* до конца: буферы затёрты */
}
void share_state(ShareState *out) {
    sh_lock();
    *out = sh;
    sh_unlock();
}
/* Замер для тестировщика: N раз подряд старт/стоп движка трансляции (весь экран), память после каждой остановки. */
static int share_loop(int n, int mode, int secs) {
    size_t m0 = priv_bytes();
    int bad = 0;
    sh_test_nopw = getenv("ST_NOPW") != NULL;
    if (getenv("ST_NOAUDIO")) share_audio_enable(false);
    if (getenv("ST_BANDROWS")) sh_band_rows = atoi(getenv("ST_BANDROWS"));
    if (getenv("ST_SPEED")) sh_speed = atoi(getenv("ST_SPEED"));
    if (getenv("ST_GOP")) sh_gop_ms = atoi(getenv("ST_GOP"));
    if (getenv("ST_NODROP")) sh_nodrop = atoi(getenv("ST_NODROP"));
    if (getenv("ST_HRT")) sh_hrt = atoi(getenv("ST_HRT"));
    for (int i = 1; i <= n; i++) {
        char err[128] = "";
        uintptr_t src = 0;                       /* ST_WINDOW — источник «окно» по части заголовка */
        if (getenv("ST_WINDOW")) {
            char t[24][80]; uintptr_t id[24];
            int nw = share_windows(t, id, 24);
            for (int k = 0; k < nw && !src; k++) if (strstr(t[k], getenv("ST_WINDOW"))) src = id[k];
            if (!src) { printf("  окно не найдено\n"); return 1; }
        }
        if (!share_start(mode, src, err, sizeof err)) { printf("  %d: старт не удался: %s\n", i, err); bad++; continue; }
        size_t mpk = 0;                          /* пик частной памяти и CPU процесса за эфир (опрос раз в 100 мс) */
        FILETIME c0, e0, k0, u0, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
        double w0 = qpc_ms();
        int npause = 0, nmini = 0, ndib = 0, nt = 0;
        for (int t = 0; t < secs * 10; t++) {
            Sleep(100);
            size_t m = priv_bytes();
            if (m > mpk) mpk = m;
            ShareState q;
            share_state(&q);
            nt++; npause += q.paused; nmini += q.minimized; ndib += q.dib;
        }
        GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k1, &u1);
        ULARGE_INTEGER a0, a1, b0, b1;
        a0.LowPart = k0.dwLowDateTime; a0.HighPart = k0.dwHighDateTime; a1.LowPart = k1.dwLowDateTime; a1.HighPart = k1.dwHighDateTime;
        b0.LowPart = u0.dwLowDateTime; b0.HighPart = u0.dwHighDateTime; b1.LowPart = u1.dwLowDateTime; b1.HighPart = u1.dwHighDateTime;
        double cpu = (double)((a1.QuadPart - a0.QuadPart) + (b1.QuadPart - b0.QuadPart)) / 10000.0;
        ShareState ss;
        share_state(&ss);
        size_t mr = priv_bytes();
        printf("  %d: %dx%d, кадров %d, отправлено %d, пауза %d [%s] перекрыл [%s], %.1f к/с, %.2f Мбит/с\n", i, ss.w, ss.h, ss.frames, ss.sent, ss.paused ? 1 : 0, ss.pause_why, ss.cover, ss.fps_out, ss.mbps);
        if (sh_stat.n) printf("  %d: на кадр, мс: захват %.2f, конверсия %.2f, кодирование %.2f (макс %.1f); ключевых %d (средн. %.0f КБ); выброшено ведром %d (из них <0,5 с после IDR %d); провалов >40 мс %d, худший %.0f мс; опоздание такта >5 мс %d, макс %.1f мс\n", i,
               sh_stat.cap / sh_stat.n, sh_stat.conv / sh_stat.n, sh_stat.coded ? sh_stat.enc / sh_stat.coded : 0.0, sh_stat.enc_max, sh_stat.keys, sh_stat.keys ? sh_stat.key_bytes / sh_stat.keys / 1024.0 : 0.0,
               sh_stat.drop, sh_stat.drop_idr, sh_stat.gaps40, sh_stat.gap_max, sh_stat.late5, sh_stat.late_max);
        char cn[64] = "";
        if (cov_last && IsWindow(cov_last)) cover_name(cov_last, cn, sizeof cn);
        if (sh_stat.nband + sh_stat.npw) printf("  %d: окно: BitBlt %d, PrintWindow %d; причины PrintWindow: перекрыто %d, перекрыли во время %d, сдвинули %d, за краем %d; перекрыл [%s]\n", i,
               sh_stat.nband, sh_stat.npw, sh_stat.rcov, sh_stat.rpost, sh_stat.rmove, sh_stat.roff, cn);
        if (sh_stat.np) printf("  %d: P-кадры с изменениями: %d, средн. %.1f КБ, макс %.1f КБ, «замороженных» кодером (<300 байт) %d\n", i, sh_stat.np, sh_stat.pbytes / sh_stat.np / 1024.0, sh_stat.pmax / 1024.0, sh_stat.frozen);
        printf("  %d: пик памяти %.2f МБ (%+.0f КБ к началу), CPU процесса %.1f%% ядра; доля времени: PrintWindow %d%%, пауза %d%%, свёрнуто %d%%\n", i,
               mpk / 1048576.0, ((double)mpk - (double)m0) / 1024.0, cpu * 100.0 / (qpc_ms() - w0), ndib * 100 / (nt ? nt : 1), npause * 100 / (nt ? nt : 1), nmini * 100 / (nt ? nt : 1));
        share_stop();
        if (sa_stat.wall_ms > 0) printf("  %d: звук [%s%s]: кадров Opus %lld (тишина %lld), пик %d, %.0f кбит/с, CPU потока звука %.1f%% ядра\n", i, ss.audio ? "идёт" : "нет", ss.audio_err,
               sa_stat.frames, sa_stat.silent, sa_stat.peak, sa_stat.bytes * 8.0 / sa_stat.wall_ms, sa_stat.cpu_ms * 100.0 / sa_stat.wall_ms);
        ShareState s2;
        share_state(&s2);
        printf("  %d: шёл %s, отправлено %d, в эфире %.2f МБ, после стопа %.2f МБ (%+.0f КБ к началу)%s\n", i, ss.active ? "да" : "НЕТ",
               ss.sent, mr / 1048576.0, priv_bytes() / 1048576.0, ((double)priv_bytes() - (double)m0) / 1024.0, s2.active ? " — НЕ ОСТАНОВЛЕН" : "");
        if (!ss.active || s2.active) bad++;
    }
    printf("share_loop: %d циклов, сбоев %d\n", n, bad);
    return bad ? 1 : 0;
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
/* share_* для Linux — в share_x11.c (захват X11) */
#endif
#endif /* ASTRA_VIDEO */
