/* Окно-терминал: бэкенд Windows (чистый Win32 + GDI).
 * Моноширинный системный шрифт (Cascadia Mono, иначе Consolas, иначе Courier New), размер по DPI
 * монитора (per-monitor DPI v2). Рисование — в WM_PAINT через ExtTextOutW по строкам сетки
 * (gui_grid.c), прогонами одинаковых атрибутов и цветов: только строки из области перерисовки,
 * без буфера на всё окно. Цвета по умолчанию — как у Discord (тёмная тема #313338/#dbdee1,
 * светлая — #ffffff/#313338, по теме системы); SGR-цвета — из палитры сетки.
 * Ожидание без опроса: term_waitables отдаёт признак W_WINMSG, plat_wait ждёт объекты и очередь
 * окна одним MsgWaitForMultipleObjectsEx.
 * Снимок (--shot, только с --demo): окно не показывается вовсе, сетка рисуется в DIB и пишется в BMP. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui.h"

int plat_win_map_vk(WORD vk);                   /* plat_win.c: виртуальная клавиша -> K_* */

static HWND hwnd;
static bool hidden;
static HFONT f_norm, f_bold;
static HFONT f_ital;                            /* курсив (SGR 3), при первой нужде */
static HFONT f_wide;                            /* запасной шрифт для символов, которых нет в основном (CJK и пр.) */
static bool f_wide_tried;
static HFONT f_small[5];                        /* мелкая сетка картинок: высота клетки / K, K = 2..4 */
static const WCHAR *face_used = L"Consolas";
static int cw = 8, chh = 16, asc = 12;          /* клетка и базовая линия, пикселы */
static UINT dpi = 96;
static int last_mx = -1, last_my = -1, wheel_acc;
static bool drag_on;                            /* левая зажата в окне (для WM_CAPTURECHANGED) */
#define WM_TRAY (WM_APP + 1)
static NOTIFYICONDATAW nid;                     /* №124: значок в трее (появляется при первом уведомлении или --tray) */
static bool tray_added;
static int last_vk;                             /* №141: VK последнего нажатия в окне (выбор клавиши рации) */
static WCHAR hi_sur;

#define COLS0 110                               /* начальный размер окна в клетках */
#define ROWS0 34
#define PT 11                                   /* кегль шрифта, пункты */
static int zoom = 100;                          /* №15: масштаб шрифта, % */
static int font_h(void) { return -MulDiv(PT * zoom, (int)dpi, 7200); }

static COLORREF cref(u32 rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

/* Тема: HKCU\...\Themes\Personalize\AppsUseLightTheme (нет значения — светлая). */
static void theme_load(void) {
    DWORD v = 1, sz = sizeof v;
    bool light = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS && v != 0;
    G.def_bg = light ? 0xFFFFFF : 0x313338;
    G.def_fg = light ? 0x313338 : 0xDBDEE1;
    if (hwnd) {
        BOOL dark = !light;                      /* тёмный заголовок окна (DWMWA_USE_IMMERSIVE_DARK_MODE) */
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);
    }
}

static HFONT font_try(const WCHAR *face, int h, int weight, HDC dc) {
    LOGFONTW lf = {0};
    lf.lfHeight = h;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) return NULL;
    WCHAR got[LF_FACESIZE];
    HGDIOBJ old = SelectObject(dc, f);
    int k = GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, old);
    if (k <= 0 || _wcsicmp(got, face)) { DeleteObject(f); return NULL; }   /* подменён другим шрифтом */
    return f;
}

static void fonts_make(void) {
    HDC dc = GetDC(hwnd);
    int h = font_h();
    static const WCHAR *const faces[] = { L"Cascadia Mono", L"Consolas", L"Courier New" };
    HFONT n = NULL, b = NULL;
    for (int i = 0; i < 3 && !n; i++) {
        n = font_try(faces[i], h, FW_NORMAL, dc);
        if (n) { b = font_try(faces[i], h, FW_BOLD, dc); face_used = faces[i]; }
    }
    if (!n) {                                    /* последний запас: системный моноширинный */
        LOGFONTW lf = {0};
        lf.lfHeight = h;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        n = CreateFontIndirectW(&lf);
    }
    if (!b) b = n;
    if (f_bold && f_bold != f_norm) DeleteObject(f_bold);
    if (f_norm) DeleteObject(f_norm);
    if (f_wide) DeleteObject(f_wide);
    f_wide = NULL;
    if (f_ital) DeleteObject(f_ital);
    f_ital = NULL;
    f_wide_tried = false;
    f_norm = n;
    f_bold = b;
    HGDIOBJ old = SelectObject(dc, f_norm);
    TEXTMETRICW tm;
    SIZE s;
    if (GetTextMetricsW(dc, &tm)) { chh = tm.tmHeight; asc = tm.tmAscent; }
    if (GetTextExtentPoint32W(dc, L"M", 1, &s) && s.cx > 0) cw = s.cx;
    if (chh < 4) chh = 4;
    if (cw < 2) cw = 2;
    SelectObject(dc, old);
    /* мелкие шрифты: высота ячейки (положительная lfHeight) = высота клетки / K, не меньше 3 px */
    for (int k = 2; k <= 4; k++) {
        if (f_small[k]) DeleteObject(f_small[k]);
        f_small[k] = NULL;
        if (chh / k < 3) continue;
        LOGFONTW lf = {0};
        lf.lfHeight = chh / k;
        lf.lfWidth = cw / k > 0 ? cw / k : 1;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfQuality = ANTIALIASED_QUALITY;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcsncpy(lf.lfFaceName, face_used, LF_FACESIZE - 1);
        f_small[k] = CreateFontIndirectW(&lf);
    }
    ReleaseDC(hwnd, dc);
}
int gb_fine_scale(void) {
    for (int k = 4; k >= 2; k--) if (f_small[k]) return k;
    return 0;
}
static void on_size(void);
/* №15: шаг 10 %, шрифт 10..28 px (при 96 dpi; на других — пропорционально); окно — прежнего размера.
 * GDI-шрифты пересоздаются (старые удаляются в fonts_make) — память не растёт. */
bool gb_zoom(int step) {
    int z = step ? zoom + 10 * step : 100;
    int px = MulDiv(PT * z, 96, 7200);          /* высота шрифта без учёта dpi монитора */
    if (step > 0 && px > 28) return false;
    if (step < 0 && px < 10) return false;
    if (z == zoom) return false;
    zoom = z;
    fonts_make();
    if (!hidden) on_size();                     /* число клеток по прежнему окну; K_RESIZE — если сменилось */
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
    return true;
}
int gb_zoom_pct(void) { return zoom; }
static void tray_add(void) {
    if (tray_added || hidden || !hwnd) return;
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wcscpy(nid.szTip, L"Astra DS");
    tray_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}
static void tray_del(void) { if (tray_added) { Shell_NotifyIconW(NIM_DELETE, &nid); tray_added = false; } }
/* №123: всплывающее уведомление (текст уже очищен store.c); строки в nid затираются после показа */
/* UTF-8 -> WCHAR не длиннее cap-1, по символам (не оборвать суррогатную пару) */
static void wcopy(WCHAR *dst, int cap, const char *s) {
    WCHAR tmp[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, tmp, 512);
    if (n <= 0) { dst[0] = 0; return; }
    n--;                                        /* без завершающего 0 */
    if (n > cap - 1) { n = cap - 1; if (n > 0 && tmp[n - 1] >= 0xD800 && tmp[n - 1] <= 0xDBFF) n--; }
    memcpy(dst, tmp, (size_t)n * sizeof(WCHAR));
    dst[n] = 0;
    SecureZeroMemory(tmp, sizeof tmp);
}
void gb_notify(const char *title, const char *text) {
    if (hidden || !hwnd) return;
    static ULONGLONG last;
    static int skipped;
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 2000) { skipped++; return; }   /* У-3: не чаще раза в 2 с, остальное — счётчиком */
    last = now;
    tray_add();
    if (!tray_added) return;
    nid.uFlags = NIF_INFO;
    wcopy(nid.szInfoTitle, 64, title ? title : "Astra DS");
    if (skipped) {
        char t[300];
        snprintf(t, sizeof t, "%s (и ещё %d)", text ? text : "", skipped);
        wcopy(nid.szInfo, 256, t);
        SecureZeroMemory(t, sizeof t);
        skipped = 0;
    } else wcopy(nid.szInfo, 256, text);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    SecureZeroMemory(nid.szInfo, sizeof nid.szInfo);
    SecureZeroMemory(nid.szInfoTitle, sizeof nid.szInfoTitle);
}
int gb_last_vk(void) { return last_vk; }
/* №16: полный экран — окно без рамки на весь монитор; обратно — прежние стиль и место */
int gb_fullscreen(int mode) {
    static WINDOWPLACEMENT prev;
    static LONG style;
    static bool on;
    if (!hwnd || hidden) return on;
    bool want = mode < 0 ? !on : mode != 0;
    if (want == on) return on;
    if (want) {
        style = GetWindowLongW(hwnd, GWL_STYLE);
        prev.length = sizeof prev;
        GetWindowPlacement(hwnd, &prev);
        MONITORINFO mi = { .cbSize = sizeof mi };
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return on;
        SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style);
        SetWindowPlacement(hwnd, &prev);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    on = want;
    return on;
}

/* ---------- рисование ---------- */
static void fill(HDC dc, const RECT *r, u32 rgb) {
    SetBkColor(dc, cref(rgb));
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}
/* Запасной шрифт той же высоты: подстановка шрифтов GDI (font linking) для CJK срабатывает не всегда —
 * тот же «日» то рисовался, то выходил квадратиком. Создаётся один раз, при первой нужде. */
static HFONT wide_font(HDC dc) {
    if (f_wide_tried) return f_wide;
    f_wide_tried = true;
    static const WCHAR *const faces[] = { L"MS Gothic", L"Yu Gothic", L"Microsoft YaHei", L"SimSun", L"Malgun Gothic", L"Segoe UI Symbol" };
    int h = font_h();
    for (size_t i = 0; i < sizeof faces / sizeof *faces && !f_wide; i++) f_wide = font_try(faces[i], h, FW_NORMAL, dc);
    return f_wide;
}
/* Курсив (SGR 3): тот же шрифт с наклоном, создаётся при первой нужде. */
static HFONT ital_font(HDC dc) {
    if (f_ital) return f_ital;
    LOGFONTW lf = {0};
    if (!GetObjectW(f_norm, sizeof lf, &lf)) return f_norm;
    lf.lfItalic = TRUE;
    f_ital = CreateFontIndirectW(&lf);
    (void)dc;
    return f_ital ? f_ital : f_norm;
}
static void draw_text(HDC dc, size_t ci, const RECT *r, const WCHAR *s, int n, const INT *dx) {
    u8 a = G.at[ci];
    u32 fg, bg;
    grid_colors(a, G.fg[ci], G.bg[ci], &fg, &bg);
    SetTextColor(dc, cref(fg));
    SetBkColor(dc, cref(bg));
    SelectObject(dc, (a & A_ITAL) ? ital_font(dc) : (a & A_BOLD) ? f_bold : f_norm);
    if (!dx && n >= 1 && s[0] > 0x7F) {          /* одиночный символ (широкий / вне BMP): есть ли он в основном шрифте */
        WORD gi[2] = { 0, 0 };
        if (GetGlyphIndicesW(dc, s, n, gi, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && gi[0] == 0xFFFF) {
            HFONT wf = wide_font(dc);
            if (wf) SelectObject(dc, wf);
        }
    }
    ExtTextOutW(dc, r->left, r->top, ETO_OPAQUE | ETO_CLIPPED, r, s, (UINT)n, dx);
    if (a & A_UL) {
        int t = (int)dpi / 96;
        RECT u = { r->left, r->top + asc + 1, r->right, r->top + asc + 1 + (t < 1 ? 1 : t) };
        if (u.bottom > r->bottom) { u.bottom = r->bottom; u.top = u.bottom - 1; }
        fill(dc, &u, fg);
    }
    if (a & A_STRIKE) {                         /* SGR 9: черта посередине клетки */
        int t = (int)dpi / 96;
        int my = r->top + (r->bottom - r->top) / 2;
        RECT u = { r->left, my, r->right, my + (t < 1 ? 1 : t) };
        fill(dc, &u, fg);
    }
}
static void draw_row(HDC dc, int y) {
    static WCHAR wb[2000];
    static INT dx[2000];
    size_t row = (size_t)y * G.cols;
    const u32 *ch = G.ch + row;
    int x = 0;
    while (x < G.cols) {
        u32 c = ch[x];
        bool wide = x + 1 < G.cols && ch[x + 1] == 0;
        if (c == 0 || wide || c > 0xFFFF) {
            /* широкий символ, символ вне BMP или осиротевшая половина — по одному, в своих клетках */
            int w = wide ? 2 : 1;
            RECT r = { x * cw, y * chh, (x + w) * cw, (y + 1) * chh };
            WCHAR s[2];
            int n = 1;
            if (c == 0) s[0] = L' ';
            else if (c > 0xFFFF) { u32 v = c - 0x10000; s[0] = (WCHAR)(0xD800 + (v >> 10)); s[1] = (WCHAR)(0xDC00 + (v & 0x3FF)); n = 2; }
            else s[0] = (WCHAR)c;
            draw_text(dc, row + (size_t)x, &r, s, n, NULL);
            x += w;
            continue;
        }
        /* прогон: одинаковые атрибуты и цвета, узкие символы BMP */
        int n = 0, x0 = x;
        size_t i0 = row + (size_t)x0;
        while (x < G.cols && ch[x] != 0 && ch[x] <= 0xFFFF && !(x + 1 < G.cols && ch[x + 1] == 0) &&
               G.at[row + x] == G.at[i0] && G.fg[row + x] == G.fg[i0] && G.bg[row + x] == G.bg[i0]) {
            wb[n] = (WCHAR)ch[x];
            dx[n] = cw;
            n++;
            x++;
        }
        RECT r = { x0 * cw, y * chh, x * cw, (y + 1) * chh };
        draw_text(dc, i0, &r, wb, n, dx);
    }
}
/* Слои мелкой сетки поверх строки y: мелкие клетки, у которых верх внутри этой строки. Координаты
 * мелких клеток — пропорциональные (клетка не делится на K нацело), прогоны одинаковых цветов. */
static void draw_layers(HDC dc, int y) {
    static WCHAR wb[2000];
    static INT dx[2000];
    for (int li = 0; li < G.nlay; li++) {
        const Layer *l = &G.lay[li];
        if (y < l->y || y >= l->y + l->h || !f_small[l->k]) continue;
        int fw = l->k * l->w, fh = l->k * l->h, X0 = l->x * cw, Y0 = l->y * chh, PW = l->w * cw, PH = l->h * chh;
        SelectObject(dc, f_small[l->k]);
        for (int fy = (y - l->y) * l->k; fy < (y - l->y + 1) * l->k && fy < fh; fy++) {
            int top = Y0 + fy * PH / fh, bot = Y0 + (fy + 1) * PH / fh;
            const u8 *c = l->c + (size_t)fy * fw * 7;
            for (int fx = 0; fx < fw; ) {
                int f0 = fx, n = 0;
                const u8 *a = c + (size_t)fx * 7;
                while (fx < fw && n < 2000 && !memcmp(c + (size_t)fx * 7 + 1, a + 1, 6)) {
                    wb[n] = (WCHAR)c[(size_t)fx * 7];
                    dx[n] = (X0 + (fx + 1) * PW / fw) - (X0 + fx * PW / fw);
                    n++;
                    fx++;
                }
                RECT r = { X0 + f0 * PW / fw, top, X0 + fx * PW / fw, bot };
                SetTextColor(dc, RGB(a[1], a[2], a[3]));
                SetBkColor(dc, RGB(a[4], a[5], a[6]));
                ExtTextOutW(dc, r.left, r.top, ETO_OPAQUE | ETO_CLIPPED, &r, wb, (UINT)n, dx);
            }
        }
        SelectObject(dc, f_norm);
    }
}
static void paint(HDC dc, const RECT *rc, int cl_right) {
    SetBkMode(dc, OPAQUE);
    SetTextAlign(dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
    HGDIOBJ old = SelectObject(dc, f_norm);
    int y0 = rc->top / chh, y1 = (rc->bottom + chh - 1) / chh;
    if (y1 > G.rows) y1 = G.rows;
    for (int y = y0 < 0 ? 0 : y0; y < y1; y++) { draw_row(dc, y); if (G.nlay) draw_layers(dc, y); }
    /* поля справа и снизу (доли клетки) — цветом фона */
    RECT m1 = { G.cols * cw, rc->top, cl_right, rc->bottom };
    if (m1.left < m1.right) fill(dc, &m1, G.def_bg);
    RECT m2 = { rc->left, G.rows * chh, cl_right, rc->bottom };
    if (m2.top < m2.bottom) fill(dc, &m2, G.def_bg);
    SelectObject(dc, old);
}

void gb_flush(void) {
    if (!hwnd || !G.dirty) return;
    bool any = false;
    for (int y = 0; y < G.rows; ) {
        if (!G.dirty[y]) { y++; continue; }
        int y0 = y;
        while (y < G.rows && G.dirty[y]) G.dirty[y++] = 0;
        if (hidden) continue;                    /* снимок: рисуется целиком в gb_snapshot */
        RECT r = { 0, y0 * chh, G.cols * cw, y * chh };
        InvalidateRect(hwnd, &r, FALSE);
        any = true;
    }
    if (any) UpdateWindow(hwnd);                 /* кадр сразу, не дожидаясь разбора очереди */
}

/* Окно активно: отметки о прочтении уходят только тогда (без окна — консоль — считаем активным). */
bool win_focused(void) { return !hwnd || hidden || GetForegroundWindow() == hwnd; }

/* Один экземпляр на пользователя Windows: именованный мьютекс сеанса (без файлов). Уже запущен —
 * поднять его окно и выйти. Описатель держится до выхода процесса (система закроет сама). */
/* SECURITY Н3: имя мьютекса — с SID пользователя (другой пользователь того же сеанса не займёт имя);
 * окно поднимается, только если оно принадлежит процессу с тем же exe, что и наш. */
static bool same_exe(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    WCHAR a[MAX_PATH], b[MAX_PATH];
    DWORD an = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(p, 0, a, &an) && GetModuleFileNameW(NULL, b, MAX_PATH) && !_wcsicmp(a, b);
    CloseHandle(p);
    return ok;
}
#ifdef ASTRA_VIDEO
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
static bool cap_excl;                           /* скрывать окна из захвата экрана */
static bool cap_apply(HWND w) {
    if (!w) return true;
    if (!cap_excl) return SetWindowDisplayAffinity(w, WDA_NONE) != 0;
    return SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE) || SetWindowDisplayAffinity(w, WDA_MONITOR);
}
bool term_capture_exclude(bool on) { cap_excl = on; return cap_apply(hwnd); }
void capture_exclude_window(void *w) { if (cap_excl) cap_apply((HWND)w); }
#endif
static HANDLE g_single;                         /* мьютекс единственного экземпляра (держим до выхода) */
void single_instance_release(void) { if (g_single) { CloseHandle(g_single); g_single = NULL; } }
bool single_instance(void) {
    WCHAR name[160] = L"Local\\astra-ds-single-instance";
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BYTE buf[128];
        DWORD n = 0;
        LPWSTR sid = NULL;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n) && ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid)) {
            _snwprintf(name, sizeof name / sizeof *name - 1, L"Local\\astra-ds-single-%ls", sid);
            LocalFree(sid);
        }
        CloseHandle(tok);
    }
    /* ревью 8 М4: запущен старый mincord.exe — тоже «уже запущен» (мьютекс прежнего имени только открываем) */
    WCHAR oldn[160];
    _snwprintf(oldn, sizeof oldn / sizeof *oldn - 1, L"%ls", name);
    oldn[sizeof oldn / sizeof *oldn - 1] = 0;
    WCHAR *pfx = wcsstr(oldn, L"astra-ds-single");
    if (pfx) {
        WCHAR rest[140];
        _snwprintf(rest, 139, L"%ls", pfx + 15);
        rest[139] = 0;
        _snwprintf(pfx, (size_t)(oldn + 159 - pfx), L"mincord-single%ls", rest);
        HANDLE om = OpenMutexW(SYNCHRONIZE, FALSE, oldn);
        if (om) { CloseHandle(om); return false; }
    }
    HANDLE m = CreateMutexW(NULL, FALSE, name);
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(L"Astra DS", NULL);
        if (w && same_exe(w)) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        CloseHandle(m);
        return false;
    }
    if (m && !g_single) g_single = m;
    return true;
}

/* SECURITY М4: профиль WebView2 капчи (%TEMP%\mincord-wv2-*) удаляется при закрытии окна, но после паники
 * или сбоя мог остаться. При старте удаляем такие каталоги старше 10 минут (свежие — возможно, капча в
 * другом процессе, например в параллельной проверке). Каталоги — только с нашим префиксом. */
void wv2_cleanup(void) {
    WCHAR tmp[MAX_PATH], pat[MAX_PATH + 32];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH - 1) return;
    /* старое имя (mincord-wv2-*) и нейтральное (wv-XXXXXXXXXXXXXXXX: ровно 16 hex — чужие каталоги не трогаем) */
    for (int pass = 0; pass < 2; pass++) {
    _snwprintf(pat, sizeof pat / sizeof *pat - 1, pass ? L"%lsastra-wv-????????????????" : L"%lsmincord-wv2-*", tmp);
    pat[sizeof pat / sizeof *pat - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t0 = { .LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime };
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        ULARGE_INTEGER t = { .LowPart = fd.ftLastWriteTime.dwLowDateTime, .HighPart = fd.ftLastWriteTime.dwHighDateTime };
        if (t0.QuadPart < t.QuadPart || t0.QuadPart - t.QuadPart < 10ULL * 60 * 10000000ULL) continue;
        WCHAR from[MAX_PATH + 64];
        int k = _snwprintf(from, MAX_PATH + 60, L"%ls%ls", tmp, fd.cFileName);
        if (k <= 0 || k >= MAX_PATH + 60) continue;
        from[k] = 0;
        from[k + 1] = 0;                         /* SHFileOperation: список, оканчивающийся двумя нулями */
        SHFILEOPSTRUCTW op = { 0 };
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMATION;
        if (pass) {                              /* «astra-wv-» + 16 шестнадцатеричных и внутри EBWebView — только наш профиль */
            bool hex = wcslen(fd.cFileName) == 25;
            for (int i = 9; hex && i < 25; i++) hex = iswxdigit(fd.cFileName[i]) != 0;
            if (!hex) continue;
            WCHAR eb[MAX_PATH + 80];
            _snwprintf(eb, MAX_PATH + 79, L"%ls%ls\\EBWebView", tmp, fd.cFileName);
            eb[MAX_PATH + 79] = 0;
            DWORD at = GetFileAttributesW(eb);
            if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) continue;
        }
        SHFileOperationW(&op);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    }
}

/* Открыть ссылку-приглашение в системном браузере: только https://discord.gg/КОД и https://discord.com/invite/КОД,
 * КОД — [A-Za-z0-9-]{2,32}; ничего другого ShellExecute не получает. */
bool open_url_browser(const char *url) {
    static const char *const PRE[] = { "https://discord.gg/", "https://discord.com/invite/" };
    const char *code = NULL;
    for (int i = 0; i < 2; i++) if (!strncmp(url, PRE[i], strlen(PRE[i]))) code = url + strlen(PRE[i]);
    if (!code) return false;
    size_t n = strlen(code);
    if (n < 2 || n > 32) return false;
    for (size_t i = 0; i < n; i++) if (!((code[i] >= 'a' && code[i] <= 'z') || (code[i] >= 'A' && code[i] <= 'Z') || (code[i] >= '0' && code[i] <= '9') || code[i] == '-')) return false;
    WCHAR w[96];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 96)) return false;
    return (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
}

void gb_bell(void) {
    if (!hwnd || hidden) return;
    if (GetForegroundWindow() != hwnd) {
        FLASHWINFO fi = { sizeof fi, hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&fi);
    } else MessageBeep(MB_OK);
}

/* ---------- снимок (--shot) ---------- */
bool gb_snapshot(const char *path) {
    if (!hwnd || !G.ch) return false;
    int w = G.cols * cw, h = G.rows * chh;
    HDC sdc = GetDC(hwnd), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                  /* сверху вниз */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    bool ok = false;
    if (bm) {
        HGDIOBJ o = SelectObject(mdc, bm);
        RECT all = { 0, 0, w, h };
        paint(mdc, &all, w);
        GdiFlush();
        SelectObject(mdc, o);
        WCHAR wp[MAX_PATH];
        FILE *f = NULL;
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH)) f = _wfopen(wp, L"wb");
        if (f) {
            BITMAPFILEHEADER fh = {0};
            DWORD sz = (DWORD)w * (DWORD)h * 4;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
            fh.bfSize = fh.bfOffBits + sz;
            ok = fwrite(&fh, sizeof fh, 1, f) == 1 && fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f) == 1 &&
                 fwrite(bits, 1, sz, f) == sz;
            ok = fclose(f) == 0 && ok;
        }
        DeleteObject(bm);
    }
    DeleteDC(mdc);
    ReleaseDC(hwnd, sdc);
    return ok;
}

/* ---------- ввод ---------- */
static void push(Key k) { kq_push(&k); }

static void mouse(int px, int py, int button, WPARAM wp) {
    if (!G.ui) return;
    int x = px / cw, y = py / chh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= G.cols) x = G.cols - 1;
    if (y >= G.rows) y = G.rows - 1;
    if ((button == 0 || button == 6) && x == last_mx && y == last_my) return;   /* наведение/протяжка — при смене клетки */
    last_mx = x;
    last_my = y;
    push((Key){ .code = K_MOUSE, .x = x, .y = y, .button = button,
                .ctrl = (wp & MK_CONTROL) != 0, .shift = (wp & MK_SHIFT) != 0, .alt = GetKeyState(VK_MENU) < 0 });
}

/* Снимок: мышь — настоящими сообщениями окна (проверяется весь путь WM_* -> K_MOUSE), остальное — в очередь. */
void gb_inject(const Key *k) {
    if (k->code != K_MOUSE || !hwnd) { kq_push(k); return; }
    int px = k->x * cw + cw / 2, py = k->y * chh + chh / 2;
    WPARAM mk = (k->ctrl ? MK_CONTROL : 0) | (k->shift ? MK_SHIFT : 0);
    switch (k->button) {
    case 1: PostMessageW(hwnd, WM_LBUTTONDOWN, mk | MK_LBUTTON, MAKELPARAM(px, py)); PostMessageW(hwnd, WM_LBUTTONUP, mk, MAKELPARAM(px, py)); break;
    case 99: PostMessageW(hwnd, WM_LBUTTONDOWN, mk | MK_LBUTTON, MAKELPARAM(px, py)); break;          /* снимок: только нажатие (протяжка) */
    case 8: case 9: PostMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(mk, k->button == 8 ? XBUTTON1 : XBUTTON2), MAKELPARAM(px, py)); break;
    case 6: PostMessageW(hwnd, WM_MOUSEMOVE, mk | MK_LBUTTON, MAKELPARAM(px, py)); break;
    case 7: PostMessageW(hwnd, WM_LBUTTONUP, mk, MAKELPARAM(px, py)); break;
    case 2: PostMessageW(hwnd, WM_MBUTTONDOWN, mk | MK_MBUTTON, MAKELPARAM(px, py)); PostMessageW(hwnd, WM_MBUTTONUP, mk, MAKELPARAM(px, py)); break;
    case 3: PostMessageW(hwnd, WM_RBUTTONDOWN, mk | MK_RBUTTON, MAKELPARAM(px, py)); PostMessageW(hwnd, WM_RBUTTONUP, mk, MAKELPARAM(px, py)); break;
    case 4: case 5: {
        POINT p = { px, py };
        ClientToScreen(hwnd, &p);
        PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(mk, k->button == 4 ? WHEEL_DELTA : -WHEEL_DELTA), MAKELPARAM(p.x, p.y));
        break;
    }
    default: PostMessageW(hwnd, WM_MOUSEMOVE, mk, MAKELPARAM(px, py)); break;
    }
}

/* OSC 52: текст UTF-8 -> CF_UNICODETEXT (переводы строк — \r\n). Промежуточная копия затирается. */
void gb_clipboard(const char *utf8, size_t n) {
    if (!hwnd || hidden || !n || n > (1u << 20)) return;   /* --shot не трогает буфер владельца */
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, NULL, 0);
    if (wn <= 0) return;
    WCHAR *w = malloc(((size_t)wn + 1) * sizeof *w);
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, w, wn);
    size_t lf = 0;
    for (int i = 0; i < wn; i++) lf += w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r');
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, ((size_t)wn + lf + 1) * sizeof(WCHAR));
    WCHAR *d = g ? GlobalLock(g) : NULL;
    if (d) {
        size_t o = 0;
        for (int i = 0; i < wn; i++) {
            if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) d[o++] = L'\r';
            d[o++] = w[i];
        }
        d[o] = 0;
        GlobalUnlock(g);
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, g)) g = NULL;    /* теперь им владеет система */
            /* не в журнал буфера (Win+V) и не в облачную синхронизацию (SECURITY.md М7) */
            static UINT f_hist, f_cloud;
            if (!f_hist) f_hist = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
            if (!f_cloud) f_cloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
            for (int k = 0; k < 2; k++) {
                UINT f = k ? f_cloud : f_hist;
                HGLOBAL z = f ? GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)) : NULL;
                DWORD *zp = z ? GlobalLock(z) : NULL;
                if (zp) { *zp = 0; GlobalUnlock(z); if (!SetClipboardData(f, z)) GlobalFree(z); }
                else if (z) GlobalFree(z);
            }
            CloseClipboard();
        }
    }
    if (g) GlobalFree(g);
    SecureZeroMemory(w, ((size_t)wn + 1) * sizeof *w);
    free(w);
}

static void paste_clipboard(void) {
    if (!OpenClipboard(hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const WCHAR *s = GlobalLock(h);
        if (s) { kq_paste_utf16((const uint16_t *)s, GlobalSize(h) / sizeof(WCHAR)); GlobalUnlock(h); }
    }
    CloseClipboard();
}

/* Ctrl+клавиша -> Key интерфейса (чистая функция, проверяется selftest): Ctrl+буква — по виртуальной клавише
 * (в любой раскладке); №15 масштаб — Ctrl+= / Ctrl+- / Ctrl+0 и на цифровом блоке (WM_CHAR с Ctrl не приходит).
 * Ctrl+Alt — это AltGr: символ придёт в WM_CHAR. */
bool gb_vk_key(unsigned vk, bool ctrl, bool alt, Key *out) {
    if (!ctrl || alt) return false;
    uint32_t ch = 0;
    if (vk >= 'A' && vk <= 'Z') ch = (uint32_t)(vk - 'A' + 'a');
    else if (vk == VK_OEM_PLUS || vk == VK_ADD) ch = '=';
    else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) ch = '-';
    else if (vk == '0' || vk == VK_NUMPAD0) ch = '0';
    if (!ch) return false;
    *out = (Key){ .code = K_CHAR, .ch = ch, .ctrl = true };
    return true;
}
static void on_key(WORD vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    int code = plat_win_map_vk(vk);
    if (code == K_ENTER) push(shift ? (Key){ .code = K_CHAR, .ch = '\n' } : (Key){ .code = K_ENTER, .ctrl = ctrl, .alt = alt });
    else if (code == K_TAB && shift) push((Key){ .code = K_BTAB });
    else if (code == K_INS && shift && !ctrl) paste_clipboard();
    else if (code != K_NONE) push((Key){ .code = code, .ctrl = ctrl, .alt = alt, .shift = shift });
    else if (ctrl && !alt && vk == 'V') paste_clipboard();
    /* Ctrl+буква (по виртуальной клавише — работает в любой раскладке); Ctrl+Alt — это AltGr: символ придёт в WM_CHAR */
    else { Key k; if (gb_vk_key(vk, ctrl, alt, &k)) push(k); }
}

static void on_char(WCHAR wc, bool alt) {
    uint32_t cp = wc;
    if (wc >= 0xD800 && wc <= 0xDBFF) { hi_sur = wc; return; }
    if (wc >= 0xDC00 && wc <= 0xDFFF) {
        if (!hi_sur) return;
        cp = 0x10000 + (((uint32_t)hi_sur - 0xD800) << 10) + (wc - 0xDC00u);
    }
    hi_sur = 0;
    if (cp < 0x20 || cp == 0x7F) return;         /* Enter, Tab, Backspace, Ctrl+буква — уже из WM_KEYDOWN */
    push((Key){ .code = K_CHAR, .ch = cp, .alt = alt });
}

static void on_size(void) {
    RECT c;
    GetClientRect(hwnd, &c);
    if (c.right <= 0 || c.bottom <= 0) return;   /* свёрнуто */
    int cols = c.right / cw, rows = c.bottom / chh;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols != G.cols || rows != G.rows) {
        grid_resize(cols, rows);
        if (G.ui) push((Key){ .code = K_RESIZE });
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cl;
        GetClientRect(h, &cl);
        if (G.ch) paint(dc, &ps.rcPaint, cl.right);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;               /* фон рисует WM_PAINT вместе с текстом: без мерцания */
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && O.tray && !hidden) { tray_add(); if (tray_added) ShowWindow(h, SW_HIDE); return 0; }   /* --tray */
        if (wp != SIZE_MINIMIZED && !hidden) on_size();
        return 0;
    case WM_DROPFILES: {                        /* перетащили файл в окно: первый — в отправку файла */
        HDROP hd = (HDROP)wp;
        WCHAR wp2[1024];
        UINT need = DragQueryFileW(hd, 0, NULL, 0);   /* D-1: длинный путь не обрезаем — отказ */
        if (need > 0 && need < 1023 && DragQueryFileW(hd, 0, wp2, 1024) == need) {
            char p8[3072];
            if (WideCharToMultiByte(CP_UTF8, 0, wp2, -1, p8, sizeof p8, NULL, NULL) > 0) {
                if (!text_unsafe(p8) && g_ui_drop) g_ui_drop(p8);   /* D-4: управляющие/BiDi в пути — отказ */
                SecureZeroMemory(p8, sizeof p8);
            }
            SecureZeroMemory(wp2, sizeof wp2);
        }
        DragFinish(hd);
        return 0;
    }
    case WM_TRAY:                               /* щелчок по значку или по уведомлению — окно на экран */
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_BALLOONUSERCLICK) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r = { 0, 0, 50 * cw, 12 * chh };    /* меньше 50x12 интерфейс не помещается */
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        MINMAXINFO *mi = (MINMAXINFO *)lp;
        mi->ptMinTrackSize.x = r.right - r.left;
        mi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        fonts_make();
        const RECT *s = (const RECT *)lp;
        SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        on_size();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const WCHAR *)lp, L"ImmersiveColorSet")) { theme_load(); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN: last_vk = (int)wp; on_key((WORD)wp); return 0;
    case WM_XBUTTONDOWN:                        /* боковые кнопки мыши: K_MOUSE button 8 / 9 (№141) */
        last_vk = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        last_mx = -1;
        mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 8 : 9, GET_KEYSTATE_WPARAM(wp));
        return TRUE;
    case WM_SYSKEYDOWN:
        last_vk = (int)wp;
        if (wp == VK_F4) break;                  /* Alt+F4 — закрыть, как обычно */
        on_key((WORD)wp);
        return 0;                                /* без меню окна по Alt/F10 */
    case WM_SYSKEYUP: if (wp == VK_F4) break; return 0;
    case WM_CHAR: on_char((WCHAR)wp, false); return 0;
    case WM_SYSCHAR: on_char((WCHAR)wp, true); return 0;     /* Alt+буква (Alt+V) — без писка */
    /* №38 выделение: левая зажата — движение приходит как button 6, отпускание — button 7 */
    case WM_LBUTTONDOWN: if (!hidden) SetCapture(h); drag_on = true; mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 1, wp); return 0;
    case WM_CAPTURECHANGED:                     /* ревью 12 М3: захват отобрали (Alt+Tab и т. п.) — отпускание */
        if ((HWND)lp != h && last_mx >= 0 && G.ui && (GetKeyState(VK_LBUTTON) < 0 || drag_on)) {
            drag_on = false;
            push((Key){ .code = K_MOUSE, .x = last_mx, .y = last_my, .button = 7 });
        }
        return 0;
    case WM_LBUTTONUP:
        drag_on = false;
        if (GetCapture() == h) ReleaseCapture();
        last_mx = -1;                           /* отпускание — всегда событие */
        mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 7, wp);
        return 0;
    case WM_MBUTTONDOWN: mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 2, wp); return 0;
    case WM_RBUTTONDOWN: mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 3, wp); return 0;
    case WM_MOUSEMOVE: mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_LBUTTON) ? 6 : 0, wp); return 0;
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        wheel_acc += GET_WHEEL_DELTA_WPARAM(wp);
        for (; wheel_acc >= WHEEL_DELTA; wheel_acc -= WHEEL_DELTA) mouse(p.x, p.y, 4, GET_KEYSTATE_WPARAM(wp));
        for (; wheel_acc <= -WHEEL_DELTA; wheel_acc += WHEEL_DELTA) mouse(p.x, p.y, 5, GET_KEYSTATE_WPARAM(wp));
        return 0;
    }
    case WM_CLOSE:
        /* Закрытие окна пользователем = выход, как Ctrl+Q: интерфейс завершится сам и вернёт всё на место. */
        tray_del();
        G.closed = true;
        ShowWindow(h, SW_HIDE);
        push((Key){ .code = K_CHAR, .ch = 'q', .ctrl = true });
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool gb_open(bool hide) {
    if (hwnd) return true;
    hidden = hide;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"Astra DS";
    /* значок Astra DS из ресурса 1 (src/astra.rc): большой — системного размера, маленький — для заголовка */
    wc.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!RegisterClassExW(&wc)) return false;
    theme_load();
    if (hidden) {
        /* снимок: окно никогда не показывается, не активируется и не попадает в панель задач */
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Astra DS", L"Astra DS", WS_POPUP,
                               -32000, -32000, 100, 100, NULL, NULL, hi, NULL);
        if (!hwnd) return false;
#ifdef ASTRA_VIDEO
        if (cap_excl) cap_apply(hwnd);
#endif
        dpi = 96;                                /* снимки одинаковы на любом мониторе */
        fonts_make();
        grid_resize(COLS0, ROWS0);
        return true;
    }
    hwnd = CreateWindowExW(0, L"Astra DS", L"Astra DS", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hi, NULL);
    if (!hwnd) return false;
#ifdef ASTRA_VIDEO
    if (cap_excl) cap_apply(hwnd);
#endif
    dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    DragAcceptFiles(hwnd, TRUE);                /* перетаскивание файла в окно */
    theme_load();
    fonts_make();
    /* начальный размер — COLS0 x ROWS0 клеток, но не больше рабочей области монитора */
    RECT r = { 0, 0, COLS0 * cw, ROWS0 * chh };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi = { sizeof mi };
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        int mw = mi.rcWork.right - mi.rcWork.left, mh = mi.rcWork.bottom - mi.rcWork.top;
        if (w > mw) w = mw;
        if (h > mh) h = mh;
        SetWindowPos(hwnd, NULL, mi.rcWork.left + (mw - w) / 2, mi.rcWork.top + (mh - h) / 2, w, h, SWP_NOZORDER);
    } else SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
    on_size();
    if (!G.ch) grid_resize(COLS0, ROWS0);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    atexit(gui_atexit);
    return true;
}

void gb_pump(int timeout_ms) {
    if (timeout_ms != 0)
        MsgWaitForMultipleObjectsEx(0, NULL, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

int gb_waitables(Waitable *out, int max) {
    if (max < 1) return 0;
    /* остаток очереди (длинная вставка) — разбудить ожидание сразу */
    if (kq_pending()) PostMessageW(hwnd, WM_NULL, 0, 0);
    out[0] = (Waitable){ 0, W_WINMSG };
    return 1;
}
#endif
