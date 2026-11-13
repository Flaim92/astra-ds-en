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
#include <math.h>
#include "share.h"
#include "share_conv.h"

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

static double psnr_y(const uint8_t *a, const uint8_t *b, size_t n);
typedef struct {
    int ow, oh, fps, scene, kbps;                /* scene: 0 desk, 1 scroll, 2 video */
    int ah;                                      /* строк в буфере Y: oh до целых блоков 16 (1080 -> 1088, кодер пишет туда) */
    int sw, sh, sx, sy;                          /* источник: размер и левый верхний угол на экране */
    HDC src;                                     /* экран или синтетический «экран» в памяти */
    HDC vdc; HBITMAP vbm; uint32_t *vpix;        /* синтетика */
    HDC bdc; HBITMAP bbm; uint32_t *band; int bandh;   /* полоса захвата: sw x bandh, 32 бит */
    int kstrips, bpp; DWORD rop;
    HWND pw;                                     /* источник «окно»: снимается, только пока его ничто не перекрывает */
    int stretch, bandw, band_oy;                 /* stretch: StretchBlt сразу в ширину кадра — полоса в ow, не в sw */                      /* полос по 16 строк кадра на один BitBlt; SRCCOPY [| CAPTUREBLT] */
    int *xmap;
    bool xident;                                 /* xmap[x] == x: источник без масштаба — точки подряд */
    bool refit;                                  /* место картинки изменилось: захват целиком, блоки «неизвестны» */
    uint8_t *y, *u, *v;
    ShMb mb;                                     /* отпечатки (Y+U+V) и возраст блоков 16x16, карта для кодера */
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
    } else if (st->scene == 3) {                 /* панорама: картинка st->ppix сдвигается на (3,2) за кадр */
        int ox = frame * 3, oy = frame * 2;
        for (int yy = 0; yy < st->sh; yy++) {
            const uint32_t *srow = st->ppix + (size_t)((yy + oy) % st->sh) * st->sw;
            uint32_t *row = st->vpix + (size_t)yy * st->sw;
            int k = ox % st->sw;
            memcpy(row, srow + k, (size_t)(st->sw - k) * 4);
            memcpy(row + (st->sw - k), srow, (size_t)k * 4);
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

/* Строка i картинки, точки [j0,j1) (j0 чётный): источник по xmap (или подряд) -> YUV420 BT.601 в кадр со сдвигом dx,dy. */
static bool sh_test_scalar;                      /* замер: скалярная конверсия вместо SSE2 (ST_SCALAR) */
static void conv_row(ST *st, const uint32_t *s32, const uint16_t *s16, int i, int j0, int j1) {
    int oy = st->dy + i;
    uint8_t *yrow = st->y + (size_t)oy * st->ow + st->dx;
    uint8_t *ur = NULL, *vr = NULL;
    if (!(oy & 1)) { ur = st->u + (size_t)(oy / 2) * (st->ow / 2) + st->dx / 2; vr = st->v + (size_t)(oy / 2) * (st->ow / 2) + st->dx / 2; }
    const int *xm = st->xident ? NULL : st->xmap;
    if (sh_test_scalar) { if (s16) shc_row16_c(s16, xm, j0, j1, yrow, ur, vr); else shc_row32_c(s32, xm, j0, j1, yrow, ur, vr, 0); }
    else if (s16) shc_row16(s16, xm, j0, j1, yrow, ur, vr);
    else shc_row32(s32, xm, j0, j1, yrow, ur, vr);
}
static void st_xmap(ST *st, int n) {             /* тождественный ли выбор точек (тогда без косвенной адресации) */
    st->xident = true;
    for (int x = 0; x < n && st->xident; x++) st->xident = st->xmap[x] == x;
}
/* Захват (полосами с экрана или из готового DIB окна) + масштаб (ближайший) + YUV420 части кадра: блоки
 * [mx0,mx1) x [my0,my1) сетки кадра (весь кадр — 0,0,mbw,mbh); с экрана берётся только прямоугольник источника под
 * этими блоками. Отпечатки снятых блоков обновляются в том же проходе (пока строки в кэше); вернуть число изменившихся. */
static int grab_convert(ST *st, int mx0, int my0, int mx1, int my1, double *t_cap, double *t_conv) {
    if (!st->dw) { st->dx = st->dy = 0; st->dw = st->ow; st->dh = st->oh; }
    if (!st->bandw) st->bandw = st->stretch ? st->ow : st->sw;
    if (st->refit) { mx0 = my0 = 0; mx1 = st->mb.mbw; my1 = st->mb.mbh; st->refit = false; }
    shc_scan_begin(&st->mb);
    int ox0 = mx0 * 16 > st->dx ? mx0 * 16 : st->dx, ox1 = mx1 * 16 < st->dx + st->dw ? mx1 * 16 : st->dx + st->dw;
    int oy0 = my0 * 16 > st->dy ? my0 * 16 : st->dy, oy1 = my1 * 16 < st->dy + st->dh ? my1 * 16 : st->dy + st->dh;
    if (ox0 >= ox1 || oy0 >= oy1) return 0;
    int j0 = ox0 - st->dx, j1 = ox1 - st->dx, i0 = oy0 - st->dy, i1 = oy1 - st->dy, hx0 = ox0 >> 4, hx1 = (ox1 + 15) >> 4;
    int changed = 0;
    double t0 = qpc_ms(), cap = 0;
#define ST_HASH(k) do { int oy_ = st->dy + (k); if ((oy_ & 15) == 15 || (k) == i1 - 1) \
        changed += shc_hash_row(&st->mb, st->y, st->u, st->v, st->ow, st->ow / 2, oy_ >> 4, hx0, hx1, ox0, ox1, oy0, oy1); } while (0)
    if (st->from_dib) {                          /* окно уже целиком в памяти (PrintWindow) */
        for (int i = i0; i < i1; i++) {
            conv_row(st, st->ppix + (size_t)(st->pcy + i * st->sh / st->dh) * st->pdw + st->pcx, NULL, i, j0, j1);
            ST_HASH(i);
        }
    } else {
        int rows = 16 * st->kstrips;
        size_t bstride = st->bpp == 16 ? (size_t)((st->bandw + 1) & ~1) : (size_t)st->bandw;
        for (int i = i0; i < i1;) {              /* полоса: rows строк картинки одним BitBlt, только нужные столбцы */
            int ie = i + rows > i1 ? i1 : i + rows, r0 = i * st->sh / st->dh, bh;
            double a = qpc_ms();
            if (st->stretch) {                   /* масштаб делает GDI: в полосе уже строки кадра (только весь кадр) */
                int r1 = ie * st->sh / st->dh;
                StretchBlt(st->bdc, 0, 0, st->dw, ie - i, st->src, st->sx, st->sy + r0, st->sw, r1 - r0, st->rop);
                bh = ie - i;
            } else {
                bh = (ie - 1) * st->sh / st->dh - r0 + 1;
                if (r0 + bh > st->sh) bh = st->sh - r0;
                if (bh > st->bandh) bh = st->bandh;
                int xs0 = st->xmap[j0], xs1 = st->xmap[j1 - 1] + 1;   /* столбцы источника; в полосе — на своих местах */
                BitBlt(st->bdc, xs0, 0, xs1 - xs0, bh, st->src, st->sx + xs0, st->sy + r0, st->rop);
            }
            cap += qpc_ms() - a;
            for (int k = i; k < ie; k++) {
                int sy = st->stretch ? k - i : k * st->sh / st->dh - r0;
                if (sy >= bh) sy = bh - 1;
                if (st->bpp == 16) conv_row(st, NULL, (const uint16_t *)st->band + (size_t)sy * bstride, k, j0, j1);
                else conv_row(st, st->band + (size_t)sy * bstride, NULL, k, j0, j1);
                ST_HASH(k);
            }
            i = ie;
        }
    }
#undef ST_HASH
    *t_cap += cap;
    *t_conv += qpc_ms() - t0 - cap;
    return changed;
}

/* Кадр и полоса захвата — затереть до освобождения: на экране мог быть пароль. */
static void st_wipe(ST *st) {
    size_t n = (size_t)st->ow * (st->ah ? st->ah : st->oh);
    if (st->y) SecureZeroMemory(st->y, n);
    if (st->u) SecureZeroMemory(st->u, n / 4);
    if (st->v) SecureZeroMemory(st->v, n / 4);
    if (st->band) SecureZeroMemory(st->band, (size_t)(st->bandw ? st->bandw : st->sw) * st->bandh * (size_t)(st->bpp / 8));
}
static int share_loop(int n, int mode, int secs);
int streamtest_run(int secs, int ow, int oh, int fps, int scene, int kbps) {
    if (getenv("ST_SHARELOOP")) return share_loop(atoi(getenv("ST_SHARELOOP")), ow >= 1920 ? SHARE_Q_1080 : ow < 1280 ? SHARE_Q_720_ECO : SHARE_Q_720, secs);
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
        if (scene == 2 && getenv("ST_PAN")) {    /* панорама по снимку экрана (копия в памяти; только замер) */
            st.ppix = malloc((size_t)st.sw * st.sh * 4);
            if (st.ppix) {
                /* картинка без зависимости от экрана: плавные пятна + цветные надписи + рамки (как сцена игры/сайта) */
                for (int yy = 0; yy < st.sh; yy++) for (int xx = 0; xx < st.sw; xx++) {
                    int cr = (int)(128 + 100 * sin(xx * 0.011 + yy * 0.004)), cg = (int)(128 + 90 * sin(yy * 0.013 - xx * 0.002)), cb = (int)(128 + 80 * cos((xx + yy) * 0.007));
                    st.vpix[(size_t)yy * st.sw + xx] = (uint32_t)(cr << 16 | cg << 8 | cb);
                }
                SetBkMode(st.vdc, TRANSPARENT);
                for (int k = 0; k < 400; k++) {
                    char t[64];
                    int tn = snprintf(t, sizeof t, "Astra %d score %05d HP %d", k, k * 37 % 99999, k % 100);
                    SetTextColor(st.vdc, RGB(k * 53 % 256, k * 97 % 256, k * 31 % 256));
                    TextOutA(st.vdc, (k * 211) % st.sw, (k * 137) % st.sh, t, tn);
                    if (k % 4 == 0) { RECT q = { (k * 89) % st.sw, (k * 59) % st.sh, (k * 89) % st.sw + 40 + k % 90, (k * 59) % st.sh + 20 + k % 60 }; FrameRect(st.vdc, &q, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
                }
                GdiFlush();
                memcpy(st.ppix, st.vpix, (size_t)st.sw * st.sh * 4);
                st.scene = 3;
            }
        }
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
    st_xmap(&st, ow);
    st.ah = (oh + 15) & ~15;                     /* 1080 -> 1088: кодер пишет восстановленный кадр целыми блоками */
    st.y = calloc((size_t)ow * st.ah, 1);
    st.mb.mbw = (ow + 15) / 16; st.mb.mbh = st.ah / 16;   /* отпечаток 0 - «ещё не было»: первый кадр весь новый */
    st.mb.sig = calloc((size_t)st.mb.mbw * st.mb.mbh, sizeof(uint32_t));
    st.mb.age = calloc((size_t)st.mb.mbw * st.mb.mbh, 1); st.mb.map = calloc((size_t)st.mb.mbw * st.mb.mbh, 1);
    st.u = calloc((size_t)ow * st.ah / 4, 1); st.v = calloc((size_t)ow * st.ah / 4, 1);

    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = ow; cp.height = oh; cp.gop = fps * 4;
    cp.vbv_size_bytes = kbps * 1000 / 8;         /* буфер ~1 с */
    if (getenv("ST_FAST")) { cp.gop = fps * 2; cp.vbv_size_bytes = (kbps * 35 / 18) * 1000 / 8; }
    cp.fine_rate_control_flag = 0;
    cp.const_input_flag = getenv("ST_CONSTIN") ? 1 : 0;   /* 0: кодер пишет во вход (экономит кадр); сравнение — по своей копии Y */
    cp.padded_input_flag = !cp.const_input_flag; /* буфер - до целых блоков: 1080p без лишнего опорного кадра */
    int szp = 0, szs = 0;
    if (H264E_sizeof(&cp, &szp, &szs)) { printf("streamtest: кодер не принял параметры\n"); return 1; }
    H264E_persist_t *enc = malloc((size_t)szp);
    H264E_scratch_t *scr = getenv("ST_NOLAZY") ? malloc((size_t)szs) : lz_alloc((size_t)szs);
    bool lazy = scr && scr == (void *)lz_base;
    if (!enc || !scr || H264E_init(enc, &cp)) { printf("streamtest: кодер не создан\n"); return 1; }
    size_t mem_init = priv_bytes();

    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    rp.encode_speed = getenv("ST_SPEED") ? atoi(getenv("ST_SPEED")) : 8;
    /* ST_FAST: без пауз, кадр = шаг сцены, ведро и кодер — как в эфире (sh_main): детерминированное сравнение качества */
    bool fast = getenv("ST_FAST") != NULL, noskip = getenv("ST_NOSKIP") != NULL;
    uint8_t *pbuf = fast ? malloc((size_t)ow * oh) : NULL;
    double psum = 0; int pn = 0, keys = 0;
    (void)noskip;
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
    double t_cap = 0, t_conv = 0, t_enc = 0, t_gen = 0, max_frame = 0, t_enc_only = 0;
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
        int ch = grab_convert(&st, 0, 0, st.mb.mbw, st.mb.mbh, &t_cap, &t_conv);
        k0 = qpc_ms();
        if (st.pw && (cov || win_covered(st.pw, &wr))) blank++;
        t_cov += qpc_ms() - k0;
        double e0 = qpc_ms();
        bucket += per_frame;
        if (bucket > cap) bucket = cap;
        if (ch || pending) {
            if (bucket < (fast ? -2 * cap : 0)) { dropped++; pending = true; }   /* поток впереди бюджета — этот кадр не кодируем */
            else {
                unsigned char *out; int n = 0;
                rp.desired_frame_bytes = (int)(per_frame + bucket / 4);
                if (fast) {                      /* как sh_main: постоянная цель, долг -> нижняя граница qp; IDR раз в 4 с */
                    double debt = bucket < 0 ? -bucket / per_frame : 0;
                    rp.qp_min = 18 + (debt > 33 ? 33 : (int)debt);
                    rp.desired_frame_bytes = (int)per_frame;
                    rp.frame_type = f % (fps * 4) == 0 ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
                }
                bool allst = false;
                if (fast && !noskip && rp.frame_type != H264E_FRAME_TYPE_KEY) { allst = shc_build_map(&st.mb); rp.mb_static = st.mb.map; rp.skip_frame = allst; }
                else { rp.mb_static = NULL; rp.skip_frame = 0; }
                if (pbuf) memcpy(pbuf, st.y, (size_t)ow * oh);
                double ee0 = qpc_ms();
                if (!H264E_encode(enc, scr, &rp, &io, &out, &n)) {
                    bytes += n; bucket -= n;
                    bool key = false;
                    for (int i = 0; i + 3 < n && !key; i++) if (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 1 && (out[i + 3] & 31) == 5) key = true;
                    keys += key;
                    if (fast) shc_encoded(&st.mb, key);
                    if (pbuf && !allst) { psum += psnr_y(pbuf, st.y, (size_t)ow * oh); pn++; }
                }
                t_enc_only += qpc_ms() - ee0;
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
        if (fast) next = now;
        else if (next > now) Sleep((DWORD)(next - now));
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
    if (fast) printf("  ST_FAST: PSNR Y %.2f дБ (%d кадров), ключевых %d, кодер %.2f мс/кадр, поток %.2f Мбит/с при 30 к/с\n", pn ? psum / pn : 0.0, pn, keys,
                     coded ? t_enc_only / coded : 0.0, bytes * 8.0 / (frames / (double)fps) / 1e6);
    printf("  CPU потока %.1f%% одного ядра (%.0f мс за %.0f мс)\n", cpu * 100.0 / wall, cpu, wall);
    printf("  частная память: до %.2f МБ, после создания %.2f МБ, пик %.2f МБ, средняя %.2f МБ\n",
           mem0 / 1048576.0, mem_init / 1048576.0, mem_peak / 1048576.0, mem_sum / frames / 1048576.0);
    printf("  кодер: persist %.2f МБ, scratch %.2f МБ (выделено %.2f МБ%s); буфер YUV %.2f МБ (+отпечатки %.0f КБ), полоса %.2f МБ\n",
           szp / 1048576.0, szs / 1048576.0, (lazy ? lz_committed() : (size_t)szs) / 1048576.0, lazy ? ", по требованию" : "", ow * oh * 1.5 / 1048576.0, (ow / 16) * (oh / 16) * 4 / 1024.0, (double)st.bandw * st.bandh * st.bpp / 8 / 1048576.0);
    st_wipe(&st);
    free(pbuf); free(st.ppix); st.ppix = NULL;
    if (lazy) lz_free(scr); else free(scr);
    free(enc); free(st.xmap); free(st.y); free(st.mb.sig); free(st.mb.age); free(st.mb.map); free(st.u); free(st.v);
    DeleteObject(st.bbm); DeleteDC(st.bdc);
    if (st.vdc) { DeleteObject(st.vbm); DeleteDC(st.vdc); }
    ReleaseDC(NULL, screen);
    return 0;
}

/* ======================= Демонстрация экрана (видео-версия): поток захвата и кодирования =======================
 * share_start - источник (окно или весь экран) и качество (SHARE_Q_*, share.h); всё - в своём потоке,
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
static int sh_speed = 10, sh_gop_ms = 4000, sh_nodrop = 1, sh_hrt = 1;   /* замер: ST_SPEED, ST_GOP, ST_NODROP, ST_HRT */                        /* замер (share_loop, ST_NOPW=1): окно только BitBlt, как при чёрном PrintWindow */
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
/* Выход - по качеству (share_quality: 1280x720 или, с Nitro, 1920x1080; 30 к/с), как у Discord: картинка вписана
 * с сохранением пропорций, без апскейла, поля чёрные. Кодер получает один и тот же размер весь эфир - смена размера
 * окна не трогает поток Go Live.
 * Замер 13.11.2026 (ST_SHARELOOP=1 ST_NOAUDIO=1, 10 с, Ryzen 7 5700G, экран 1920x1080, сцена на весь экран), поток
 * трансляции, % одного ядра 720p -> 1080p: static 7,0 -> 7,6; scroll 22,9 -> 30,8 (кодер 2,2 -> 5,0 мс/кадр, оба
 * 30 к/с); video (шум, упор в битрейт) 23,0 -> 26,6 (кодер 3,2 -> 6,6 мс). BitBlt - 12-16 мс/кадр в обоих (ожидание
 * DWM, не CPU). Пик памяти +7,1 -> +10,7 МБ. 60 к/с не включаем: кодер без ограничения битрейта на шуме - 6,9 мс
 * (720p) и 11,1 мс (1080p) на кадр, 1080p60 - ~0,7 ядра на один кодер, а такт захвата и темп (share_conv.h) - под 30. */
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
    shc_reset(&st->mb);
    for (int x = 0; x < st->dw; x++) st->xmap[x] = x * st->sw / st->dw;
    st_xmap(st, st->dw);
    st->fit_w = st->sw; st->fit_h = st->sh;
    st->refit = true;
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
    double thr_cpu, thr_cyc, thr_wall;           /* поток трансляции целиком: CPU (мс), циклы, стена (мс) */
    double psnr_sum, psnr_cyc; int psnr_n;                /* ST_PSNR: средний PSNR яркости закодированных кадров */
    int nfull, nroi, nskipf, nskipenc;           /* такты: захват целиком / только область / без захвата; кадров из одних пропусков */
    long long chsum;                             /* изменившихся блоков всего */
    int nhi, nlo, nstill;                        /* такты 30 / 20 к/с / 10 Гц */
} sh_stat;
static bool sh_test_nopw, sh_test_psnr, sh_test_noroi, sh_test_noskip, sh_test_noadapt;
static double thr_cycles(void) { ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return (double)c; }
/* PSNR яркости: копия входа до кодирования против восстановленного кадра (кодер пишет его во вход) — только замер */
static double psnr_y(const uint8_t *a, const uint8_t *b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; i++) { int d = (int)a[i] - (int)b[i]; se += (double)(d * d); }
    if (se <= 0) return 99.0;
    double mse = se / (double)n;
    return 10.0 * log10(255.0 * 255.0 / mse);
}
static DWORD WINAPI sh_main(LPVOID arg) {
    (void)arg;
    ST st;
    memset(&st, 0, sizeof st);
    /* физические точки: иначе при масштабе Windows 125-150% GetWindowRect и рамки DWM в разных единицах */
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   /* игра грузит ядра: такт эфира не должен ждать */
    double thr_c0 = thread_cpu_ms(), thr_y0 = thr_cycles(), thr_w0 = qpc_ms();   /* замер (share_loop) */
    HDC screen = GetDC(NULL);
    if (sh_hwnd && !IsWindow(sh_hwnd)) { ReleaseDC(NULL, screen); sh_finish("окно источника закрыто"); return 0; }
    st.pw = sh_hwnd;
    st.sw = GetSystemMetrics(SM_CXSCREEN); st.sh = GetSystemMetrics(SM_CYSCREEN);   /* весь экран (основной монитор) */
    if (st.sw < 16 || st.sh < 16 || st.sw > 16384 || st.sh > 16384) { ReleaseDC(NULL, screen); sh_finish("источник слишком мал"); return 0; }
    ShareQuality q = share_quality(sh_mode);
    int fps = q.fps, kbps = q.kbps;
    int kbps_max = q.kbps_max;                   /* пик за любую секунду: цель + запас ведра */
    st.ow = q.w; st.oh = q.h; st.ah = (q.h + 15) & ~15;   /* 1080 -> 1088 строк: кодер пишет восстановленный кадр целыми блоками */
    st.fps = fps; st.kbps = kbps; st.src = screen; st.bpp = 16; st.rop = SRCCOPY;
    st.stretch = 0;                              /* BitBlt: StretchBlt экономит полосу, но CPU 60-73 % и 14-17 к/с */
    st.xmap = malloc(sizeof(int) * (size_t)st.ow);
    size_t ysz = (size_t)st.ow * st.ah, csz = ysz / 4;
    st.y = vm_alloc(ysz);
    st.u = vm_alloc(csz); st.v = vm_alloc(csz);
    st.mb.mbw = (st.ow + 15) / 16; st.mb.mbh = st.ah / 16;   /* сетка кодера: карта mb_static - на все его блоки */
    size_t nmb = (size_t)st.mb.mbw * st.mb.mbh;
    st.mb.sig = calloc(nmb, sizeof(uint32_t)); st.mb.age = calloc(nmb, 1); st.mb.map = calloc(nmb, 1);
    if (st.y && st.u && st.v) { memset(st.y, 16, ysz); memset(st.u, 128, csz); memset(st.v, 128, csz); }   /* чёрный, не зелёный */
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = st.ow; cp.height = st.oh; cp.gop = fps * 8;   /* ключевые — только по таймеру sh_gop_ms и по PLI зрителя: два несогласованных источника давали IDR раз в ~2 с */ cp.vbv_size_bytes = kbps_max * 1000 / 8;
    cp.padded_input_flag = 1;                    /* буфер до целых блоков: 1080p - обрезка в SPS, без лишнего опорного кадра */
    int szp = 0, szs = 0;
    H264E_persist_t *enc = NULL;
    H264E_scratch_t *scr = NULL;
    bool ok = st.xmap && st.y && st.u && st.v && st.mb.sig && st.mb.age && st.mb.map && !H264E_sizeof(&cp, &szp, &szs);
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
        sh_lock(); sh.w = st.ow; sh.h = st.oh; sh.fps = fps; sh.kbps_max = kbps_max; sh_unlock();
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
        uint8_t *psnr_buf = sh_test_psnr ? malloc(ysz) : NULL;   /* только замер ST_PSNR */
        double last_sent = 0;
        double last_change = t0;                 /* когда в кадре последний раз что-то менялось */
        /* темп и захват (share_conv.h): 30 к/с при движении, 20 при малом, 10 Гц при покое; целиком не реже 100 мс,
         * между — только область изменений; блоки без изменений кодер не ищет, кадр без изменений — одни пропуски */
        ShPace pace;
        memset(&pace, 0, sizeof pace);
        pace.force_full = true; pace.last_change = t0; pace.last_full = -1e9; pace.hi_until = t0 + 1000;
        double tick_ms = 1000.0 / fps, last_chk = -1e9;
        int enc_since_key = 0;
        while (!sh_stop) {
            { double lt = qpc_ms() - next; if (frames && lt > sh_stat.late_max) sh_stat.late_max = lt; if (frames && lt > 5) sh_stat.late5++; }
            double c_cap = tcap, c_conv = tconv;
            if (qpc_ms() - last_chk >= 1000) {   /* раз в секунду: окно живо? экран не заблокирован? */
                last_chk = qpc_ms();
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
            /* что снимать: ключевой кадр скоро (по времени, по запросу, по GOP кодера) — целиком */
            bool key_due = sh_want_idr || tq - last_idr >= sh_gop_ms - 2 * tick_ms || enc_since_key >= cp.gop - 1;
            int kind = sh_test_noroi ? 2 : shc_grab_kind(&pace, &st.mb, tq, key_due);
            int gx0 = 0, gy0 = 0, gx1 = st.mb.mbw, gy1 = st.mb.mbh;
            if (kind == 1) {                     /* область изменений прошлого захвата и 2 блока вокруг */
                gx0 = st.mb.cx0 - 2 < 0 ? 0 : st.mb.cx0 - 2; gy0 = st.mb.cy0 - 2 < 0 ? 0 : st.mb.cy0 - 2;
                gx1 = st.mb.cx1 + 2 > st.mb.mbw ? st.mb.mbw : st.mb.cx1 + 2; gy1 = st.mb.cy1 + 2 > st.mb.mbh ? st.mb.mbh : st.mb.cy1 + 2;
            }
            if (st.pw && IsIconic(st.pw)) { blank = mini = true; pwhy = "окно свёрнуто"; }
            else if (st.pw && kind) {
                RECT ext;
                bool onscr = win_rect(st.pw, &wr) && SUCCEEDED(DwmGetWindowAttribute(st.pw, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof ext))
                             && EqualRect(&ext, &wr), done = false;
                if (onscr && !win_covered(st.pw, &wr)) {
                    st.sx = wr.left; st.sy = wr.top;
                    st.sw = wr.right - wr.left; st.sh = wr.bottom - wr.top;
                    sh_fit(&st);
                    if (!sh_band(&st, screen)) { why = "не хватило памяти под новый размер окна"; break; }
                    ch = grab_convert(&st, gx0, gy0, gx1, gy1, &tcap, &tconv);
                    RECT w2;                     /* после захвата: перекрыли или сдвинули — в кадре может быть чужое */
                    if (win_covered(st.pw, &wr)) { pwhy = "перекрыто во время захвата"; sh_stat.rpost++; }
                    else if (!win_rect(st.pw, &w2) || !EqualRect(&w2, &wr)) { pwhy = "окно сдвинули во время захвата"; sh_stat.rmove++; }
                    else { done = true; last_band = tq; sh_stat.nband++; }
                } else { pwhy = onscr ? "перекрыто" : "окно за краем экрана"; if (onscr) sh_stat.rcov++; else sh_stat.roff++; }
                if (!done && !sh_test_nopw && tq >= pw_retry) {
                    if (sh_printwin(&st, screen, &tcap)) { ch = grab_convert(&st, gx0, gy0, gx1, gy1, &tcap, &tconv); done = true; pwhy = ""; last_pw = tq; sh_stat.npw++; }
                    else pw_retry = tq + 2000;   /* велико / чёрное / не вышло: снова через 2 с */
                }
                if (!done) blank = true;
                /* буфер, которым не пользовались 3 с, — затереть и вернуть (оба сразу держим, только пока окно то
                 * перекрывают, то открывают) */
                if (st.pbm && tq - last_pw > 3000) sh_pdib_free(&st);
                if (st.bbm && tq - last_band > 3000) sh_band_free(&st);
            } else if (!st.pw && kind) {         /* весь экран */
                sh_fit(&st);
                if (!sh_band(&st, screen)) { why = "не хватило памяти под полосу захвата"; break; }
                ch = grab_convert(&st, gx0, gy0, gx1, gy1, &tcap, &tconv);
            }                                    /* kind 0: ничего не менялось — не снимаем, повтор кадра ниже */
            if (kind == 2) sh_stat.nfull++; else if (kind) sh_stat.nroi++; else sh_stat.nskipf++;
            sh_stat.chsum += ch;
            /* пауза или кадр с чужим содержимым: следующий захват — только целиком (иначе чужое останется вне области) */
            if (blank) pace.force_full = true;
            else if (kind == 2) { pace.force_full = false; pace.last_full = tq; }
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
            if (ch) last_change = tq;
            if (blank != paused) { paused = blank; sh_lock(); sh.paused = blank; sh_unlock(); }
            double fr = tick_ms * fps / 1000.0;  /* доля «кадра 30 к/с» в этом такте: бюджет по времени, не по кадрам */
            bucket += per * fr;
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
                    rp.desired_frame_bytes = sh_nodrop ? (int)(per * fr) : (int)(per * fr + bucket / 4);
                    /* блоки без изменений — без поиска; кадр целиком без изменений — одни пропуски (почти даром) */
                    bool allst = !want_key && !sh_test_noskip && shc_build_map(&st.mb);
                    rp.mb_static = want_key || sh_test_noskip ? NULL : st.mb.map;
                    rp.skip_frame = allst;
                    if (allst) sh_stat.nskipenc++;
                    rp.frame_type = want_key ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
                    if (want_key) last_idr = tn;
                    last_out = tn;
                    double py0 = psnr_buf ? thr_cycles() : 0;
                    if (psnr_buf) memcpy(psnr_buf, st.y, ysz);
                    if (psnr_buf) sh_stat.psnr_cyc += thr_cycles() - py0;
                    double e0 = qpc_ms();
                    int er = H264E_encode(enc, scr, &rp, &io, &out, &n);
                    double e1 = qpc_ms();
                    if (psnr_buf && !er && !allst) { py0 = thr_cycles(); sh_stat.psnr_sum += psnr_y(psnr_buf, st.y, ysz); sh_stat.psnr_n++; sh_stat.psnr_cyc += thr_cycles() - py0; }
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
                        shc_encoded(&st.mb, key);
                        enc_since_key = key ? 0 : enc_since_key + 1;
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
            /* картинка стоит больше 1 с — захват 10 Гц (столько же, сколько повтор кадра для зрителей), первое
             * изменение — снова fps: экран без движения не стоит 30 захватов BitBlt в секунду */
            int pmb = ((st.dx + st.dw + 15) / 16 - st.dx / 16) * ((st.dy + st.dh + 15) / 16 - st.dy / 16);
            tick_ms = shc_tick(&pace, tq, ch, pmb > 0 ? pmb : 1, pending || blank);
            if (sh_test_noadapt) tick_ms = !pending && !blank && now - last_change > 1000.0 ? 100.0 : 1000.0 / fps;   /* как было */
            if (tick_ms < 40) sh_stat.nhi++; else if (tick_ms < 80) sh_stat.nlo++; else sh_stat.nstill++;
            next += tick_ms;
            now = qpc_ms();
            if (next > now) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((next - now) * 10000.0);
                if (tmr && SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(tmr, 1000);
                else Sleep((DWORD)(next - now));
            } else if (now - next > 1000.0) next = now;
        }
        if (tmr) CloseHandle(tmr);
        if (psnr_buf) { SecureZeroMemory(psnr_buf, ysz); free(psnr_buf); }
        sh_stat.thr_cpu = thread_cpu_ms() - thr_c0; sh_stat.thr_cyc = thr_cycles() - thr_y0; sh_stat.thr_wall = qpc_ms() - thr_w0;
        if (sh_stat.thr_cyc > sh_stat.psnr_cyc && sh_stat.psnr_cyc > 0) {   /* замер качества — не в счёт CPU */
            sh_stat.thr_cpu *= 1.0 - sh_stat.psnr_cyc / sh_stat.thr_cyc;
            sh_stat.thr_cyc -= sh_stat.psnr_cyc;
        }
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
    free(st.xmap); free(st.mb.sig); free(st.mb.age); free(st.mb.map);
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
    ShareQuality q = share_quality(mode);
    sh_lock();
    memset(&sh, 0, sizeof sh);
    sh.active = true;
    sh.w = q.w; sh.h = q.h; sh.fps = q.fps; sh.kbps_max = q.kbps_max;   /* сразу: op 12 трансляции может уйти раньше первого кадра */
    sh_unlock();
    sh_hwnd = (HWND)hwnd; sh_mode = mode >= 0 && mode < SHARE_Q_N ? mode : SHARE_Q_720;
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
/* Стенд share_loop: тестовое окно со сценой (ST_SCENE=static|scroll|video), рисуется в главном потоке ~30 к/с —
 * одинаковая картинка для замеров «до/после». ST_SRC=win — источник это окно (1280x900), иначе весь экран, а окно
 * поверх всего на весь основной монитор. Только замер: ничего не пишется на диск. */
static struct { HWND w; HDC dc; HBITMAP bm; HGDIOBJ old; uint32_t *pix; int w_, h_, scene, f; double next, per; HANDLE tmr; } tw;
static LRESULT CALLBACK tw_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC d = BeginPaint(h, &ps);
        if (tw.dc) BitBlt(d, 0, 0, tw.w_, tw.h_, tw.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void tw_text(int y0, int line) {          /* строка «документа»: тёмный текст на светлом */
    char t[200];
    int n = snprintf(t, sizeof t, "%05d  The quick brown fox jumps over the lazy dog; %d  съешь же ещё этих мягких булок, да выпей чаю  %08x", line, line * 7, (unsigned)line * 2654435761u);
    RECT r = { 0, y0, tw.w_, y0 + 20 };
    FillRect(tw.dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SetBkMode(tw.dc, TRANSPARENT);
    TextOutA(tw.dc, 24 + (line % 5) * 8, y0 + 2, t, n);
}
static void tw_step(void) {
    int f = tw.f++;
    if (tw.scene == 0 && f) return;              /* статика: нарисовали один раз */
    if (tw.scene == 3 && f) {                    /* набор текста: буква каждые 4 кадра 30 Гц, 3 с набора — 2 с пауза */
        if (f % 150 >= 90 || f % 4) return;
        int k = f / 4, line = 30 + (k / 60) % 20, col = k % 60;
        char c = (char)('a' + k % 26);
        SetBkMode(tw.dc, OPAQUE);
        TextOutA(tw.dc, 24 + col * 8, line * 20 + 2, &c, 1);
    } else if (tw.scene == 0 || tw.scene == 3 || (tw.scene == 1 && !f)) {
        RECT r = { 0, 0, tw.w_, tw.h_ };
        FillRect(tw.dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        for (int y = 0, l = 0; y < tw.h_; y += 20, l++) tw_text(y, l);
    } else if (tw.scene == 1) {                  /* прокрутка: 3 точки за кадр 60 Гц, внизу новые строки */
        BitBlt(tw.dc, 0, 0, tw.w_, tw.h_ - 3, tw.dc, 0, 3, SRCCOPY);
        int sc = f * 3, line = (tw.h_ + sc) / 20;
        tw_text(tw.h_ - ((tw.h_ + sc) % 20) - 20, line);
        tw_text(tw.h_ - ((tw.h_ + sc) % 20), line + 1);
    } else {                                     /* «видео»: движение и шум по всему окну */
        GdiFlush();
        uint32_t seed = (uint32_t)f * 2654435761u;
        for (int yy = 0; yy < tw.h_; yy++) {
            uint32_t *row = tw.pix + (size_t)yy * tw.w_;
            for (int xx = 0; xx < tw.w_; xx++) {
                seed = seed * 1103515245u + 12345u;
                int r = (xx + f * 6) & 255, g = (yy + f * 3) & 255, b = ((xx ^ yy) + f * 4) & 255, nse = (int)(seed >> 28);
                row[xx] = (uint32_t)(((r + nse) & 255) << 16 | ((g + nse) & 255) << 8 | ((b + nse) & 255));
            }
        }
    }
    HDC d = GetDC(tw.w);
    BitBlt(d, 0, 0, tw.w_, tw.h_, tw.dc, 0, 0, SRCCOPY);
    ReleaseDC(tw.w, d);
}
static HWND tw_open(const char *scene, bool win) {
    tw.scene = !strcmp(scene, "scroll") ? 1 : !strcmp(scene, "video") ? 2 : !strcmp(scene, "type") ? 3 : 0;
    int sw = GetSystemMetrics(SM_CXSCREEN), shh = GetSystemMetrics(SM_CYSCREEN);
    tw.w_ = win ? 1280 : sw; tw.h_ = win ? 900 : shh;
    if (tw.w_ > sw) tw.w_ = sw;
    if (tw.h_ > shh) tw.h_ = shh;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = tw_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"AstraStTest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    tw.w = CreateWindowExW(WS_EX_TOPMOST, L"AstraStTest", L"Astra ST test", WS_POPUP, win ? (sw - tw.w_) / 2 : 0, win ? (shh - tw.h_) / 2 : 0,
                           tw.w_, tw.h_, NULL, NULL, wc.hInstance, NULL);
    if (!tw.w) return NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biWidth = tw.w_; bi.bmiHeader.biHeight = -tw.h_; bi.bmiHeader.biBitCount = 32;
    HDC sd = GetDC(NULL);
    tw.dc = CreateCompatibleDC(sd);
    ReleaseDC(NULL, sd);
    tw.bm = CreateDIBSection(tw.dc, &bi, DIB_RGB_COLORS, (void **)&tw.pix, NULL, 0);
    tw.old = SelectObject(tw.dc, tw.bm);
    SelectObject(tw.dc, GetStockObject(ANSI_FIXED_FONT));
    tw.f = 0;
    tw_step();
    ShowWindow(tw.w, SW_SHOW);
    SetForegroundWindow(tw.w);                   /* источник «окно» снимается BitBlt, только если он на переднем плане */
    UpdateWindow(tw.w);
    tw.next = qpc_ms();
    tw.per = tw.scene == 1 ? 1000.0 / 60 : 1000.0 / 30;
    tw.tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    return tw.w;
}
static void tw_close(void) {
    if (!tw.w) return;
    DestroyWindow(tw.w);
    SelectObject(tw.dc, tw.old); DeleteObject(tw.bm); DeleteDC(tw.dc);
    if (tw.tmr) CloseHandle(tw.tmr);
    memset(&tw, 0, sizeof tw);
}
/* ждать ms, перерисовывая сцену ~30 к/с и разбирая сообщения окна */
static void tw_pump(double ms) {
    double end = qpc_ms() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        double now = qpc_ms();
        if (now >= end) return;
        if (tw.w && now >= tw.next) { tw_step(); tw.next += tw.per; if (now - tw.next > 200) tw.next = now; }
        double wake = tw.w && tw.next < end ? tw.next : end;
        wake -= qpc_ms();
        if (wake <= 0) continue;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(wake * 10000.0);
        if (tw.tmr && SetWaitableTimer(tw.tmr, &due, 0, NULL, NULL, FALSE)) MsgWaitForMultipleObjects(1, &tw.tmr, FALSE, 1000, QS_ALLINPUT);
        else MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)wake + 1, QS_ALLINPUT);
    }
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
    sh_test_psnr = getenv("ST_PSNR") != NULL;
    sh_test_noroi = getenv("ST_NOROI") != NULL;      /* замер по пунктам: без области изменений (всегда целиком) */
    sh_test_noskip = getenv("ST_NOSKIP") != NULL;    /* без пропуска блоков без изменений в кодере */
    sh_test_noadapt = getenv("ST_NOADAPT") != NULL;  /* темп как раньше: 30 к/с, 10 Гц при покое > 1 с */
    sh_test_scalar = getenv("ST_SCALAR") != NULL;    /* скалярная конверсия */
    bool twin = getenv("ST_SRC") && !strcmp(getenv("ST_SRC"), "win");
    if (getenv("ST_SCENE")) {
        SetProcessDPIAware();
        if (!tw_open(getenv("ST_SCENE"), twin)) { printf("  тестовое окно не создано\n"); return 1; }
        tw_pump(500);
    }
    m0 = priv_bytes();                           /* без DIB тестового окна */
    for (int i = 1; i <= n; i++) {
        char err[128] = "";
        uintptr_t src = 0;                       /* ST_WINDOW — источник «окно» по части заголовка */
        if (tw.w && twin) {                      /* своё тестовое окно: в списке источников его нет — добавить вручную */
            sh_known[0].h = tw.w; GetWindowThreadProcessId(tw.w, &sh_known[0].pid);
            GetWindowTextW(tw.w, sh_known[0].t, 80);
            sh_nknown = 1;
            src = (uintptr_t)tw.w;
        } else if (getenv("ST_WINDOW")) {
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
            tw_pump(100);
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
        if (sh_stat.thr_wall > 0) printf("  %d: поток трансляции: CPU %.1f%% ядра (%.2f мс/кадр), циклов %.1f М/с (%.2f М/кадр); тактов %d: захват целиком %d, по области %d, без захвата %d; кадров из пропусков %d; изменений %.0f блоков/такт; PSNR Y %.2f дБ (%d кадров); в среднем %.1f к/с, %.2f Мбит/с; такты 30/20/10 Гц: %d/%d/%d\n", i,
               sh_stat.thr_cpu * 100.0 / sh_stat.thr_wall, sh_stat.thr_cpu / (sh_stat.n ? sh_stat.n : 1), sh_stat.thr_cyc / sh_stat.thr_wall / 1000.0,
               sh_stat.thr_cyc / 1e6 / (sh_stat.n ? sh_stat.n : 1), sh_stat.n, sh_stat.nfull, sh_stat.nroi, sh_stat.nskipf, sh_stat.nskipenc,
               (double)sh_stat.chsum / (sh_stat.n ? sh_stat.n : 1), sh_stat.psnr_n ? sh_stat.psnr_sum / sh_stat.psnr_n : 0.0, sh_stat.psnr_n,
               ss.sent * 1000.0 / sh_stat.thr_wall, ss.bytes * 8.0 / sh_stat.thr_wall / 1000.0, sh_stat.nhi, sh_stat.nlo, sh_stat.nstill);
        if (sa_stat.wall_ms > 0) printf("  %d: звук [%s%s]: кадров Opus %lld (тишина %lld), пик %d, %.0f кбит/с, CPU потока звука %.1f%% ядра\n", i, ss.audio ? "идёт" : "нет", ss.audio_err,
               sa_stat.frames, sa_stat.silent, sa_stat.peak, sa_stat.bytes * 8.0 / sa_stat.wall_ms, sa_stat.cpu_ms * 100.0 / sa_stat.wall_ms);
        ShareState s2;
        share_state(&s2);
        printf("  %d: шёл %s, отправлено %d, в эфире %.2f МБ, после стопа %.2f МБ (%+.0f КБ к началу)%s\n", i, ss.active ? "да" : "НЕТ",
               ss.sent, mr / 1048576.0, priv_bytes() / 1048576.0, ((double)priv_bytes() - (double)m0) / 1024.0, s2.active ? " — НЕ ОСТАНОВЛЕН" : "");
        if (!ss.active || s2.active) bad++;
    }
    tw_close();
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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "share.h"
/* Linux: стенд ST_SHARELOOP=N --streamtest=СЕКУНД — N запусков настоящего share_start (X11: весь экран; Wayland —
 * через портал и PipeWire) по СЕКУНД, без Discord и сети: Go Live не запущен, кадры кодера только считаются
 * (golive_frame). ST_NOAUDIO=1 — без звука; ST_STOPCHOOSE=МС — стоп, пока открыто окно выбора. Вывод — только в консоль и только по этому явному ключу; файлов нет. */
static double st_clock(clockid_t id) { struct timespec t; clock_gettime(id, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }
static long st_status_kb(const char *key) {     /* поле /proc/self/status в КБ (чтение; -1 — нет) */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char ln[128];
    long v = -1;
    size_t k = strlen(key);
    while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, key, k) && ln[k] == ':') { v = atol(ln + k + 1); break; }
    fclose(f);
    return v;
}
static void st_sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
int streamtest_run(int secs, int ow, int oh, int fps, int scene, int kbps) {
    (void)oh; (void)fps; (void)scene; (void)kbps;
    const char *lp = getenv("ST_SHARELOOP");
    if (!lp) { printf("streamtest: на Linux — только стенд ST_SHARELOOP=N (настоящий захват, без Discord)\n"); return 1; }
    int n = atoi(lp) < 1 ? 1 : atoi(lp), bad = 0;
    if (getenv("ST_NOAUDIO")) share_audio_enable(false);
    printf("share_loop (Linux, %s): %d циклов по %d с\n", share_wayland() ? "Wayland, портал + PipeWire" : "X11, весь экран", n, secs);
    for (int i = 1; i <= n; i++) {
        char err[160] = "";
        long a0 = st_status_kb("RssAnon");
        if (!share_start(ow >= 1920 ? SHARE_Q_1080 : ow < 1280 ? SHARE_Q_720_ECO : SHARE_Q_720, 0, err, sizeof err)) { printf("  %d: старт не удался: %s\n", i, err); bad++; continue; }
        ShareState q;
        double c0 = st_clock(CLOCK_MONOTONIC);
        /* ST_STOPCHOOSE=МС: share_stop, пока открыто системное окно выбора, — проверка, что ожидание прерывается */
        int stop_ms = getenv("ST_STOPCHOOSE") ? atoi(getenv("ST_STOPCHOOSE")) : 0;
        do { st_sleep_ms(50); share_state(&q); } while (q.active && q.choosing && st_clock(CLOCK_MONOTONIC) - c0 < (stop_ms > 0 ? stop_ms : 120000));
        if (stop_ms > 0) {
            bool was = q.active && q.choosing;
            double t1 = st_clock(CLOCK_MONOTONIC);
            share_stop();
            share_state(&q);
            printf("  %d: стоп во время выбора (окно было открыто: %s): share_stop за %.0f мс, идёт: %s, причина: %s\n", i, was ? "да" : "нет",
                   st_clock(CLOCK_MONOTONIC) - t1, q.active ? "ДА" : "нет", q.reason);
            if (!was || q.active) bad++;
            continue;
        }
        if (!q.active || q.choosing) { printf("  %d: выбор источника: %s\n", i, q.active ? "нет ответа 120 с" : q.reason); share_stop(); bad++; continue; }
        printf("  %d: источник выбран за %.0f мс\n", i, st_clock(CLOCK_MONOTONIC) - c0);
        double w0 = st_clock(CLOCK_MONOTONIC), p0 = st_clock(CLOCK_PROCESS_CPUTIME_ID);
        long rss_pk = 0, anon_pk = 0, shm_pk = 0;
        int npause = 0, nt = 0;
        for (int t = 0; t < secs * 10 && q.active; t++) {
            st_sleep_ms(100);
            share_state(&q);
            long r = st_status_kb("VmRSS"), an = st_status_kb("RssAnon"), sm = st_status_kb("RssShmem");
            if (r > rss_pk) rss_pk = r;
            if (an > anon_pk) anon_pk = an;
            if (sm > shm_pk) shm_pk = sm;
            nt++; npause += q.paused;
            if (t % 10 == 9) printf("  %d: %3d с: %.1f к/с, %.2f Мбит/с%s%s\n", i, (t + 1) / 10, q.fps_out, q.mbps, q.paused ? ", пауза: " : "", q.paused ? q.pause_why : "");
        }
        double cpu = st_clock(CLOCK_PROCESS_CPUTIME_ID) - p0, wall = st_clock(CLOCK_MONOTONIC) - w0;
        ShareState ss;
        share_state(&ss);
        printf("  %d: %dx%d, тактов %d, отправлено %d (%.2f МБ), пауза %d%% времени; CPU процесса %.1f%% ядра; VmRSS пик %ld КБ, RssAnon пик %ld КБ (%+ld к началу), RssShmem пик %ld КБ; звук: %s%s\n",
               i, ss.w, ss.h, ss.frames, ss.sent, (double)ss.bytes / 1048576.0, npause * 100 / (nt ? nt : 1), cpu * 100.0 / (wall > 0 ? wall : 1),
               rss_pk, anon_pk, anon_pk - a0, shm_pk, ss.audio ? "идёт" : "нет ", ss.audio_err);
        if (!ss.active) printf("  %d: остановилась сама: %s\n", i, ss.reason);
        share_stop();
        ShareState s2;
        share_state(&s2);
        printf("  %d: после стопа RssAnon %ld КБ%s\n", i, st_status_kb("RssAnon"), s2.active ? " — НЕ ОСТАНОВЛЕН" : "");
        if (!ss.active || s2.active) bad++;
    }
    printf("share_loop: %d циклов, сбоев %d\n", n, bad);
    return bad ? 1 : 0;
}
/* share_* для Linux — в share_x11.c (захват X11; Wayland — portal_wl.c + share_pw.c) */
#endif
#endif /* ASTRA_VIDEO */
