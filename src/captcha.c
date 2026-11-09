/* Проверка hCaptcha при входе: системный webview только на время проверки. См. captcha.h.
 *
 * Windows, WebView2. Заголовок — официальный WebView2.h из пакета NuGet Microsoft.Web.WebView2 1.0.4258.31
 * (third_party/webview2, BSD-3; SHA-512 пакета сверен с каталогом NuGet; SHA-256 заголовка
 * dff1e3181ec7ec203a34ef6efa966590e0ef0ba1a5c3fe3b69da6508c2f8a02e). Загрузчик WebView2Loader.dll не нужен:
 * как открытые загрузчики (OpenWebView2Loader), берём путь установленной среды из реестра EdgeUpdate
 * (ClientState\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}, значение EBWebView; сначала HKLM, затем HKCU),
 * проверяем подпись EmbeddedBrowserWebView.dll (WinVerifyTrust) и зовём её экспорт
 * CreateWebViewEnvironmentWithOptionsInternal. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "plat.h"
#include "captcha.h"

#ifndef _WIN32
/* Linux: WebKitGTK в статическую сборку не входит — проверку показать нечем (интерфейс предложит QR). */
bool captcha_supported(void) { return false; }
bool captcha_open(const char *sitekey, const char *rqdata, char *out, size_t cap, bool test, char *err, size_t errn) {
    (void)sitekey; (void)rqdata; (void)out; (void)cap; (void)test;
    snprintf(err, errn, "проверка hCaptcha в Astra DS на Linux пока не поддерживается: войдите по QR-коду");
    return false;
}
int captcha_result(char *err, size_t errn) { if (errn) err[0] = 0; return -1; }
void captcha_close(void) {}
bool captcha_busy(void) { return false; }
void captcha_bridge_stats(int *used, int *refused) { *used = *refused = 0; }
#else

#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <WebView2.h>                           /* -isystem third_party/webview2 */

#define PAGE_URL L"https://discord.com/astra-captcha"
#define TEST_SITEKEY "10000000-ffff-ffff-ffff-000000000001"   /* открытый тестовый ключ hCaptcha */
#define WM_MC_CLOSE (WM_APP + 1)

enum { CS_IDLE, CS_OPEN, CS_OK, CS_FAIL };

static struct {
    HANDLE th;
    volatile LONG state;
    char *out;
    size_t cap;
    char err[200];
    char page[8192];                            /* HTML страницы (ключ сайта и rqdata проверены по набору символов) */
    size_t page_len;
    HWND hwnd;
    DWORD tid;
    WCHAR dll[MAX_PATH + 64];
    WCHAR udf[MAX_PATH];                        /* временный профиль WebView2 */
    /* WebView2 (только в потоке окна) */
    ICoreWebView2Environment *env;
    ICoreWebView2Controller *ctrl;
    ICoreWebView2 *wv;
    /* мост CONNECT */
    SOCKET lsn;
    int port;
    volatile LONG stop, conns, used, refused;
    bool via;                                   /* через SOCKS5 туннеля */
    char sip[64], suser[128], spass[128];
    int sport;
} C = { .lsn = INVALID_SOCKET };

/* ---------- поиск среды WebView2 ---------- */
static bool reg_path(HKEY root, WCHAR *out, DWORD cap) {
    HKEY k;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\EdgeUpdate\\ClientState\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", 0,
                      KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, n = cap * sizeof(WCHAR);
    LONG rc = RegQueryValueExW(k, L"EBWebView", NULL, &type, (BYTE *)out, &n);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_SZ || n < 4 || n >= cap * sizeof(WCHAR)) return false;
    out[n / sizeof(WCHAR)] = 0;
    return true;
}
static bool signed_ok(const WCHAR *path) {
    WINTRUST_FILE_INFO fi = { .cbStruct = sizeof fi, .pcwszFilePath = path };
    GUID act = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = { .cbStruct = sizeof wd, .dwUIChoice = WTD_UI_NONE, .fdwRevocationChecks = WTD_REVOKE_NONE,
                         .dwUnionChoice = WTD_CHOICE_FILE, .pFile = &fi, .dwStateAction = WTD_STATEACTION_VERIFY,
                         .dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL };
    LONG rc = WinVerifyTrust(NULL, &act, &wd);
    /* SECURITY М3: подпись не просто действительна, а от Microsoft — имя подписанта (лист цепочки,
     * проверенной WinVerifyTrust до доверенного корня) должно быть «Microsoft Corporation». */
    bool ms = false;
    if (rc == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR *sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
        if (sg && sg->csCertChain > 0 && sg->pasCertChain && sg->pasCertChain[0].pCert) {
            WCHAR who[128];
            if (CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, who, 128) > 1)
                ms = !wcscmp(who, L"Microsoft Corporation");
            /* Р4: и цепочка ведёт к корню Microsoft (а не к любому доверенному центру) */
            if (ms && sg->pChainContext) {
                /* корни продуктов Microsoft и прикладной «Microsoft Root Certificate Authority 2011» (им подписан WebView2) */
#ifndef MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG
#define MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG 0x00020000
#endif
                CERT_CHAIN_POLICY_PARA pp = { .cbSize = sizeof pp, .dwFlags = MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG };
                CERT_CHAIN_POLICY_STATUS ps = { .cbSize = sizeof ps };
                ms = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_MICROSOFT_ROOT, sg->pChainContext, &pp, &ps) && ps.dwError == 0;
            } else ms = false;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &act, &wd);
    return rc == ERROR_SUCCESS && ms;
}
static bool find_dll(void) {
    static const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2; i++) {
        WCHAR base[MAX_PATH];
        if (!reg_path(roots[i], base, MAX_PATH)) continue;
        int n = _snwprintf(C.dll, sizeof C.dll / sizeof *C.dll, L"%ls\\EBWebView\\x64\\EmbeddedBrowserWebView.dll", base);
        if (n <= 0 || n >= (int)(sizeof C.dll / sizeof *C.dll)) continue;
        if (GetFileAttributesW(C.dll) != INVALID_FILE_ATTRIBUTES) return true;
    }
    C.dll[0] = 0;
    return false;
}
bool captcha_supported(void) { return C.dll[0] || find_dll(); }

static void fail(const char *text, const char *tail) {
    if (InterlockedCompareExchange(&C.state, CS_FAIL, CS_OPEN) == CS_OPEN) snprintf(C.err, sizeof C.err, "%s%s", text, tail ? tail : "");
}

/* ---------- мост CONNECT -> (SOCKS5 туннеля | напрямую), только hcaptcha.com:443 ---------- */
static bool host_ok(const char *h) {
    size_t n = strlen(h);
    if (!n || n > 100) return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    return !strcmp(h, "hcaptcha.com") || (n > 13 && !strcmp(h + n - 13, ".hcaptcha.com"));
}
static bool send_all(SOCKET s, const void *p, int n) {
    const char *b = p;
    while (n > 0) { int k = send(s, b, n, 0); if (k <= 0) return false; b += k; n -= k; }
    return true;
}
static bool recv_n(SOCKET s, void *p, int n) {
    char *b = p;
    while (n > 0) { int k = recv(s, b, n, 0); if (k <= 0) return false; b += k; n -= k; }
    return true;
}
static void timeouts(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
}
static SOCKET upstream(const char *host) {
    SOCKET u = INVALID_SOCKET;
    if (!C.via) {
        struct addrinfo hint = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai = NULL;
        if (getaddrinfo(host, "443", &hint, &ai)) return INVALID_SOCKET;
        for (struct addrinfo *a = ai; a && u == INVALID_SOCKET; a = a->ai_next) {
            u = socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
            if (u == INVALID_SOCKET) continue;
            timeouts(u, 15000);
            if (connect(u, a->ai_addr, (int)a->ai_addrlen)) { closesocket(u); u = INVALID_SOCKET; }
        }
        freeaddrinfo(ai);
        return u;
    }
    /* SOCKS5 с логином и паролем (RFC 1928/1929), DNS — на стороне сервера */
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((u_short)C.sport) };
    if (inet_pton(AF_INET, C.sip, &sa.sin_addr) != 1) return INVALID_SOCKET;
    u = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (u == INVALID_SOCKET) return u;
    timeouts(u, 15000);
    unsigned char b[600];
    size_t ul = strlen(C.suser), pl = strlen(C.spass), hl = strlen(host);
    bool ok = !connect(u, (struct sockaddr *)&sa, sizeof sa) && ul < 256 && pl < 256 &&
              send_all(u, "\x05\x01\x02", 3) && recv_n(u, b, 2) && b[0] == 5 && b[1] == 2;
    if (ok) {
        size_t n = 0;
        b[n++] = 1; b[n++] = (unsigned char)ul; memcpy(b + n, C.suser, ul); n += ul;
        b[n++] = (unsigned char)pl; memcpy(b + n, C.spass, pl); n += pl;
        ok = send_all(u, b, (int)n) && recv_n(u, b, 2) && b[1] == 0;
        wipe(b, sizeof b);
    }
    if (ok) {
        size_t n = 0;
        b[n++] = 5; b[n++] = 1; b[n++] = 0; b[n++] = 3; b[n++] = (unsigned char)hl;
        memcpy(b + n, host, hl); n += hl;
        b[n++] = 443 >> 8; b[n++] = 443 & 255;
        ok = send_all(u, b, (int)n) && recv_n(u, b, 4) && b[0] == 5 && b[1] == 0;
        int skip = !ok ? 0 : b[3] == 1 ? 6 : b[3] == 4 ? 18 : -1;
        if (ok && b[3] == 3) { ok = recv_n(u, b, 1); skip = b[0] + 2; }
        ok = ok && skip > 0 && recv_n(u, b, skip);
    }
    if (!ok) { closesocket(u); u = INVALID_SOCKET; }
    return u;
}
static DWORD WINAPI bridge_conn(LPVOID arg) {
    SOCKET c = (SOCKET)(uintptr_t)arg, u = INVALID_SOCKET;
    timeouts(c, 15000);
    char h[4096];
    int n = 0;
    while (n < (int)sizeof h - 1) {
        int k = recv(c, h + n, (int)sizeof h - 1 - n, 0);
        if (k <= 0) break;
        n += k;
        h[n] = 0;
        if (strstr(h, "\r\n\r\n")) break;
    }
    h[n] = 0;
    char host[128] = "";
    int port = 0;
    char *end = strstr(h, "\r\n\r\n");
    if (end && !strncmp(h, "CONNECT ", 8) && sscanf(h + 8, "%127[^: ]:%d HTTP/1.", host, &port) == 2) {
        for (char *p = host; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    }
    if (port != 443 || !host_ok(host) || !(u = upstream(host), u != INVALID_SOCKET)) {
        InterlockedIncrement(&C.refused);
        send_all(c, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", 45);
        goto out;
    }
    InterlockedIncrement(&C.used);
    if (!send_all(c, "HTTP/1.1 200 Connection Established\r\n\r\n", 39)) goto out;
    int extra = n - (int)(end + 4 - h);
    if (extra > 0 && !send_all(u, end + 4, extra)) goto out;
    timeouts(c, 0);
    timeouts(u, 0);
    char buf[16384];
    while (!C.stop) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(c, &rs);
        FD_SET(u, &rs);
        struct timeval tv = { 0, 300000 };
        int r = select(0, &rs, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue;
        if (FD_ISSET(c, &rs)) { int k = recv(c, buf, sizeof buf, 0); if (k <= 0 || !send_all(u, buf, k)) break; }
        if (FD_ISSET(u, &rs)) { int k = recv(u, buf, sizeof buf, 0); if (k <= 0 || !send_all(c, buf, k)) break; }
    }
out:
    if (u != INVALID_SOCKET) closesocket(u);
    closesocket(c);
    InterlockedDecrement(&C.conns);
    return 0;
}
static DWORD WINAPI bridge_accept(LPVOID arg) {
    (void)arg;
    for (;;) {
        SOCKET c = accept(C.lsn, NULL, NULL);
        if (c == INVALID_SOCKET) break;          /* слушатель закрыт — конец */
        if (C.stop) { closesocket(c); break; }
        InterlockedIncrement(&C.conns);
        HANDLE t = CreateThread(NULL, 64 * 1024, bridge_conn, (LPVOID)(uintptr_t)c, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (t) CloseHandle(t); else { closesocket(c); InterlockedDecrement(&C.conns); }
    }
    InterlockedDecrement(&C.conns);
    return 0;
}
static bool bridge_start(void) {
    C.stop = 0;
    C.used = C.refused = 0;
    C.lsn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (C.lsn == INVALID_SOCKET) return false;
    BOOL ex = TRUE;
    setsockopt(C.lsn, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&ex, sizeof ex);
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    if (bind(C.lsn, (struct sockaddr *)&a, sizeof a) || listen(C.lsn, 16) || getsockname(C.lsn, (struct sockaddr *)&a, &len)) {
        closesocket(C.lsn); C.lsn = INVALID_SOCKET; return false;
    }
    C.port = ntohs(a.sin_port);
    C.conns = 1;                                /* поток приёма */
    HANDLE t = CreateThread(NULL, 64 * 1024, bridge_accept, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { closesocket(C.lsn); C.lsn = INVALID_SOCKET; C.conns = 0; return false; }
    CloseHandle(t);
    return true;
}
static void bridge_stop(void) {
    C.stop = 1;
    if (C.lsn != INVALID_SOCKET) { closesocket(C.lsn); C.lsn = INVALID_SOCKET; }
    for (int i = 0; i < 100 && C.conns > 0; i++) Sleep(20);   /* соединения видят stop за 0,3 с */
    wipe(C.suser, sizeof C.suser);
    wipe(C.spass, sizeof C.spass);
}

/* ---------- COM-объекты обработчиков (в C: таблица методов + счётчик ссылок) ---------- */
typedef struct { void *vt; LONG ref; } Obj;
static HRESULT STDMETHODCALLTYPE o_qi(void *This, REFIID riid, void **pp) { (void)riid; *pp = This; ((Obj *)This)->ref++; return S_OK; }
static ULONG STDMETHODCALLTYPE o_addref(void *This) { return (ULONG)++((Obj *)This)->ref; }
static ULONG STDMETHODCALLTYPE o_release(void *This) { return (ULONG)--((Obj *)This)->ref; }   /* объекты статические */

/* Параметры среды: прокси — наш мост; всё лишнее у Chromium выключено. */
static WCHAR g_args[256];
static HRESULT STDMETHODCALLTYPE opt_dup(const WCHAR *s, LPWSTR *v) {
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    *v = CoTaskMemAlloc(n);
    if (!*v) return E_OUTOFMEMORY;
    memcpy(*v, s, n);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE opt_qi(ICoreWebView2EnvironmentOptions *This, REFIID riid, void **pp) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ICoreWebView2EnvironmentOptions)) { *pp = This; return S_OK; }
    *pp = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE opt_ar(ICoreWebView2EnvironmentOptions *This) { (void)This; return 2; }
static HRESULT STDMETHODCALLTYPE opt_get_args(ICoreWebView2EnvironmentOptions *This, LPWSTR *v) { (void)This; return opt_dup(g_args, v); }
static HRESULT STDMETHODCALLTYPE opt_put_s(ICoreWebView2EnvironmentOptions *This, LPCWSTR v) { (void)This; (void)v; return S_OK; }
static HRESULT STDMETHODCALLTYPE opt_get_lang(ICoreWebView2EnvironmentOptions *This, LPWSTR *v) { (void)This; return opt_dup(L"ru-RU", v); }
static HRESULT STDMETHODCALLTYPE opt_get_ver(ICoreWebView2EnvironmentOptions *This, LPWSTR *v) { (void)This; return opt_dup(L"154.0.4258.31", v); }
static HRESULT STDMETHODCALLTYPE opt_get_sso(ICoreWebView2EnvironmentOptions *This, BOOL *v) { (void)This; *v = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE opt_put_sso(ICoreWebView2EnvironmentOptions *This, BOOL v) { (void)This; (void)v; return S_OK; }
static ICoreWebView2EnvironmentOptionsVtbl opt_vt = { opt_qi, opt_ar, opt_ar, opt_get_args, opt_put_s, opt_get_lang, opt_put_s,
                                                      opt_get_ver, opt_put_s, opt_get_sso, opt_put_sso };
static ICoreWebView2EnvironmentOptions opt_obj = { &opt_vt };

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler h_ctrl;
static ICoreWebView2WebResourceRequestedEventHandler h_res;
static ICoreWebView2WebMessageReceivedEventHandler h_msg;
static ICoreWebView2NavigationStartingEventHandler h_nav;
static ICoreWebView2NewWindowRequestedEventHandler h_new;

static HRESULT STDMETHODCALLTYPE env_done(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT hr, ICoreWebView2Environment *env) {
    (void)This;
    if (FAILED(hr) || !env) { fail("WebView2 не запустился", ""); PostMessageW(C.hwnd, WM_MC_CLOSE, 0, 0); return S_OK; }
    C.env = env;
    ICoreWebView2Environment_AddRef(env);
    if (FAILED(ICoreWebView2Environment_CreateCoreWebView2Controller(env, C.hwnd, &h_ctrl))) {
        fail("WebView2: окно не создалось", ""); PostMessageW(C.hwnd, WM_MC_CLOSE, 0, 0);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ctrl_done(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT hr, ICoreWebView2Controller *ctrl) {
    (void)This;
    if (FAILED(hr) || !ctrl) { fail("WebView2: окно не создалось", ""); PostMessageW(C.hwnd, WM_MC_CLOSE, 0, 0); return S_OK; }
    C.ctrl = ctrl;
    ICoreWebView2Controller_AddRef(ctrl);
    RECT r;
    GetClientRect(C.hwnd, &r);
    ICoreWebView2Controller_put_Bounds(ctrl, r);
    if (FAILED(ICoreWebView2Controller_get_CoreWebView2(ctrl, &C.wv)) || !C.wv) { fail("WebView2 не отвечает", ""); PostMessageW(C.hwnd, WM_MC_CLOSE, 0, 0); return S_OK; }
    ICoreWebView2Settings *st = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(C.wv, &st)) && st) {
        ICoreWebView2Settings_put_AreDevToolsEnabled(st, FALSE);
        ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsStatusBarEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsZoomControlEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsWebMessageEnabled(st, TRUE);
        ICoreWebView2Settings_put_IsScriptEnabled(st, TRUE);
        ICoreWebView2Settings_Release(st);
    }
    EventRegistrationToken tk;
    ICoreWebView2_AddWebResourceRequestedFilter(C.wv, PAGE_URL L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    ICoreWebView2_add_WebResourceRequested(C.wv, &h_res, &tk);
    ICoreWebView2_add_WebMessageReceived(C.wv, &h_msg, &tk);
    ICoreWebView2_add_NavigationStarting(C.wv, &h_nav, &tk);
    ICoreWebView2_add_NewWindowRequested(C.wv, &h_new, &tk);
    ICoreWebView2_Navigate(C.wv, PAGE_URL);
    return S_OK;
}
/* Наша страница — из памяти, под адресом discord.com: запрос в сеть не уходит. */
static HRESULT STDMETHODCALLTYPE res_req(ICoreWebView2WebResourceRequestedEventHandler *This, ICoreWebView2 *s, ICoreWebView2WebResourceRequestedEventArgs *a) {
    (void)This; (void)s;
    ICoreWebView2WebResourceRequest *rq = NULL;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2WebResourceRequestedEventArgs_get_Request(a, &rq)) && rq && SUCCEEDED(ICoreWebView2WebResourceRequest_get_Uri(rq, &uri)) && uri) {
        bool page = !wcscmp(uri, PAGE_URL);
        IStream *body = SHCreateMemStream((const BYTE *)(page ? C.page : ""), page ? (UINT)C.page_len : 0);
        ICoreWebView2WebResourceResponse *resp = NULL;
        if (body && SUCCEEDED(ICoreWebView2Environment_CreateWebResourceResponse(C.env, body, page ? 200 : 404, page ? L"OK" : L"Not Found",
                L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store", &resp)) && resp) {
            ICoreWebView2WebResourceRequestedEventArgs_put_Response(a, resp);
            ICoreWebView2WebResourceResponse_Release(resp);
        }
        if (body) IStream_Release(body);
    }
    if (uri) CoTaskMemFree(uri);
    if (rq) ICoreWebView2WebResourceRequest_Release(rq);
    return S_OK;
}
/* Сообщение страницы: "ok:<ответ>" или "err:<код>". */
static HRESULT STDMETHODCALLTYPE msg_got(ICoreWebView2WebMessageReceivedEventHandler *This, ICoreWebView2 *s, ICoreWebView2WebMessageReceivedEventArgs *a) {
    (void)This; (void)s;
    /* ревью 6 М5: сообщение принимается только от нашей страницы (не от фрейма или чужого документа) */
    LPWSTR src = NULL;
    bool ours = SUCCEEDED(ICoreWebView2WebMessageReceivedEventArgs_get_Source(a, &src)) && src && !wcscmp(src, PAGE_URL);
    if (src) CoTaskMemFree(src);
    if (!ours) return S_OK;
    LPWSTR m = NULL;
    if (FAILED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(a, &m)) || !m) return S_OK;
    size_t wn = wcslen(m);
    if (wn > 3 && !wcsncmp(m, L"ok:", 3)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, m + 3, (int)(wn - 3), C.out, (int)C.cap - 1, NULL, NULL);
        bool ok = n > 20 && (size_t)n < C.cap;
        for (int i = 0; ok && i < n; i++) { char c = C.out[i]; ok = c > 0x20 && c < 0x7f && c != '"' && c != '\\'; }
        if (ok) { C.out[n] = 0; InterlockedCompareExchange(&C.state, CS_OK, CS_OPEN); }
        else { wipe(C.out, C.cap); fail("hCaptcha вернула непонятный ответ", ""); }
        PostMessageW(C.hwnd, WM_MC_CLOSE, 0, 0);
    } else if (!wcscmp(m, L"err:load")) {
        fail("hCaptcha не загрузилась: нет связи с hcaptcha.com", C.via ? " через VLESS" : "");
        PostMessageW(C.hwnd, WM_MC_CLOSE, 0, 0);
    }
    wipe(m, wn * sizeof(WCHAR));
    CoTaskMemFree(m);
    return S_OK;
}
/* Никуда, кроме своей страницы, окно не переходит; новые окна не открываются. */
static HRESULT STDMETHODCALLTYPE nav_start(ICoreWebView2NavigationStartingEventHandler *This, ICoreWebView2 *s, ICoreWebView2NavigationStartingEventArgs *a) {
    (void)This; (void)s;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_get_Uri(a, &uri)) && uri) {
        if (wcscmp(uri, PAGE_URL)) ICoreWebView2NavigationStartingEventArgs_put_Cancel(a, TRUE);
        CoTaskMemFree(uri);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE new_win(ICoreWebView2NewWindowRequestedEventHandler *This, ICoreWebView2 *s, ICoreWebView2NewWindowRequestedEventArgs *a) {
    (void)This; (void)s;
    ICoreWebView2NewWindowRequestedEventArgs_put_Handled(a, TRUE);
    return S_OK;
}

#define VT(T, inv) static T##Vtbl T##_vt = { (void *)o_qi, (void *)o_addref, (void *)o_release, inv }
VT(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, env_done);
VT(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, ctrl_done);
VT(ICoreWebView2WebResourceRequestedEventHandler, res_req);
VT(ICoreWebView2WebMessageReceivedEventHandler, msg_got);
VT(ICoreWebView2NavigationStartingEventHandler, nav_start);
VT(ICoreWebView2NewWindowRequestedEventHandler, new_win);
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler h_env = { &ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_vt };
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler h_ctrl = { &ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_vt };
static ICoreWebView2WebResourceRequestedEventHandler h_res = { &ICoreWebView2WebResourceRequestedEventHandler_vt };
static ICoreWebView2WebMessageReceivedEventHandler h_msg = { &ICoreWebView2WebMessageReceivedEventHandler_vt };
static ICoreWebView2NavigationStartingEventHandler h_nav = { &ICoreWebView2NavigationStartingEventHandler_vt };
static ICoreWebView2NewWindowRequestedEventHandler h_new = { &ICoreWebView2NewWindowRequestedEventHandler_vt };

/* ---------- окно и поток ---------- */
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (C.ctrl) { RECT r; GetClientRect(h, &r); ICoreWebView2Controller_put_Bounds(C.ctrl, r); }
        return 0;
    case WM_MC_CLOSE: case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(h, m, w, l);
    }
}

static void rm_tree(const WCHAR *dir) {
    WCHAR pat[MAX_PATH * 2];
    _snwprintf(pat, sizeof pat / sizeof *pat, L"%ls\\*", dir);
    pat[sizeof pat / sizeof *pat - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            WCHAR p[MAX_PATH * 2];
            _snwprintf(p, sizeof p / sizeof *p, L"%ls\\%ls", dir, fd.cFileName);
            p[sizeof p / sizeof *p - 1] = 0;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) rm_tree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir);
}

typedef HRESULT (STDMETHODCALLTYPE *CreateEnvFn)(bool, int, PCWSTR, IUnknown *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static DWORD WINAPI ui_thread(LPVOID arg) {
    (void)arg;
    HMODULE dll = NULL;
    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    WNDCLASSW wc = { .lpfnWndProc = wnd_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"astra-captcha",
                     .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .hbrBackground = CreateSolidBrush(RGB(0x31, 0x33, 0x38)) };
    RegisterClassW(&wc);
    int w = 460, h = 660;
    C.hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Astra DS: проверка hCaptcha для Discord", WS_OVERLAPPEDWINDOW,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, NULL, NULL, wc.hInstance, NULL);
    if (!C.hwnd) { fail("окно проверки не создалось", ""); goto done; }
    ShowWindow(C.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(C.hwnd);
    dll = LoadLibraryExW(C.dll, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CreateEnvFn create = dll ? (CreateEnvFn)(void *)GetProcAddress(dll, "CreateWebViewEnvironmentWithOptionsInternal") : NULL;
    if (!create || FAILED(create(true, 0, C.udf, (IUnknown *)&opt_obj, &h_env))) {
        fail("WebView2 Runtime не запустился", "");
        DestroyWindow(C.hwnd);
    }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    fail("окно проверки закрыто", "");
done:
    C.hwnd = NULL;
    if (C.wv) { ICoreWebView2_Release(C.wv); C.wv = NULL; }
    if (C.ctrl) { ICoreWebView2Controller_Close(C.ctrl); ICoreWebView2Controller_Release(C.ctrl); C.ctrl = NULL; }
    if (C.env) { ICoreWebView2Environment_Release(C.env); C.env = NULL; }
    /* процессы Edge выходят сами в течение секунды-двух: ждём, пока профиль удастся удалить */
    for (int i = 0; i < 50 && C.udf[0]; i++) {
        MSG m2;
        while (PeekMessageW(&m2, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m2);
        rm_tree(C.udf);
        if (GetFileAttributesW(C.udf) == INVALID_FILE_ATTRIBUTES) break;
        Sleep(200);
    }
    if (dll) {
        HRESULT (STDMETHODCALLTYPE *can)(void) = (void *)GetProcAddress(dll, "DllCanUnloadNow");
        if (!can || can() == S_OK) FreeLibrary(dll);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (wc.hbrBackground) DeleteObject(wc.hbrBackground);
    if (com) CoUninitialize();
    bridge_stop();
    wipe(C.page, sizeof C.page);
    if (g_wake) wake_signal(g_wake);
    return 0;
}

static bool charset(const char *s, const char *extra, size_t max) {
    size_t n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(extra, c))) return false;
    }
    return true;
}

static void page_build(const char *sitekey, const char *rqdata, bool test) {
    Buf b = {0};
    buf_puts(&b,
        "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
        "<title>Astra DS</title><style>html,body{margin:0;height:100%;background:#313338;color:#dbdee1;font:15px 'Segoe UI',sans-serif}"
        "body{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:18px}"
        "#t{max-width:380px;text-align:center;line-height:1.4}</style><script>var SK=\"");
    buf_puts(&b, sitekey);
    buf_puts(&b, "\",RQ=\"");
    buf_puts(&b, rqdata);
    buf_puts(&b, test ? "\",AUTO=1;" : "\",AUTO=0;");
    buf_puts(&b,
        "function post(s){try{chrome.webview.postMessage(s)}catch(e){}}"
        "var loaded=0;function onl(){loaded=1;try{var id=hcaptcha.render('c',{sitekey:SK,theme:'dark',size:AUTO?'invisible':'normal',"
        "callback:function(t){post('ok:'+t)},'error-callback':function(e){try{hcaptcha.reset(id)}catch(x){}}});"
        "if(RQ)hcaptcha.setData(id,{rqdata:RQ});if(AUTO)hcaptcha.execute(id);}catch(e){post('err:load')}}"
        "setTimeout(function(){if(!loaded)post('err:load')},30000);</script>"
        "<script src=\"https://js.hcaptcha.com/1/api.js?render=explicit&onload=onl&hl=ru&recaptchacompat=off\" async defer></script>"
        "</head><body><div id=\"t\">Discord просит подтвердить, что вы человек.<br>Пройдите проверку - окно закроется само.</div>"
        "<div id=\"c\"></div></body></html>");
    C.page_len = b.len < sizeof C.page ? b.len : 0;
    if (C.page_len) memcpy(C.page, b.p, b.len);
    buf_wipe(&b);
}

bool captcha_open(const char *sitekey, const char *rqdata, char *out, size_t cap, bool test, char *err, size_t errn) {
    err[0] = 0;
    if (captcha_busy()) { snprintf(err, errn, "окно проверки уже открыто"); return false; }
    captcha_close();
    if (test) sitekey = TEST_SITEKEY;
    if (!sitekey || !rqdata || !charset(sitekey, "-", 64) || !charset(rqdata, "+/=._-", 4000) || !sitekey[0]) {
        snprintf(err, errn, "Discord прислал проверку непонятного вида");
        return false;
    }
    if (!captcha_supported()) { snprintf(err, errn, "нет Microsoft Edge WebView2 Runtime: проверку показать нечем, войдите по QR-коду"); return false; }
    if (!signed_ok(C.dll)) { snprintf(err, errn, "библиотека WebView2 не прошла проверку подписи"); return false; }
    C.via = vless_state() != VL_OFF;
    if (C.via && !vless_socks(C.sip, sizeof C.sip, &C.sport, C.suser, sizeof C.suser, C.spass, sizeof C.spass)) {
        snprintf(err, errn, "VLESS не работает: проверку открыть нельзя (напрямую не идём)");
        return false;
    }
    if (!bridge_start()) { bridge_stop(); snprintf(err, errn, "не удалось открыть локальный мост для проверки"); return false; }
    _snwprintf(g_args, sizeof g_args / sizeof *g_args,
               L"--proxy-server=http://127.0.0.1:%d --proxy-bypass-list=<-loopback> --disable-background-networking "
               L"--no-first-run --disable-sync --disable-features=msSmartScreenProtection "
               /* WebRTC не ходит мимо прокси (иначе реальный IP утекает в обход VLESS — SECURITY.md М2) */
               L"--force-webrtc-ip-handling-policy=disable_non_proxied_udp --webrtc-ip-handling-policy=disable_non_proxied_udp", C.port);
    g_args[sizeof g_args / sizeof *g_args - 1] = 0;
    WCHAR tmp[MAX_PATH];
    unsigned char r[8];
    if (BCryptGenRandom(NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) { bridge_stop(); snprintf(err, errn, "нет случайных чисел"); return false; }
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (!tn || tn > MAX_PATH - 40) { bridge_stop(); snprintf(err, errn, "нет временного каталога"); return false; }
    _snwprintf(C.udf, MAX_PATH, L"%lsastra-wv-%02x%02x%02x%02x%02x%02x%02x%02x", tmp, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    C.udf[MAX_PATH - 1] = 0;
    if (!CreateDirectoryW(C.udf, NULL)) { C.udf[0] = 0; bridge_stop(); snprintf(err, errn, "не создался временный профиль"); return false; }
    page_build(sitekey, rqdata, test);
    if (!C.page_len) { rm_tree(C.udf); C.udf[0] = 0; bridge_stop(); snprintf(err, errn, "rqdata слишком длинная"); return false; }
    C.out = out;
    C.cap = cap;
    wipe(out, cap);
    C.err[0] = 0;
    C.state = CS_OPEN;
    C.th = CreateThread(NULL, 0, ui_thread, NULL, 0, &C.tid);
    if (!C.th) { C.state = CS_IDLE; rm_tree(C.udf); C.udf[0] = 0; bridge_stop(); snprintf(err, errn, "поток проверки не запустился"); return false; }
    return true;
}

void captcha_bridge_stats(int *used, int *refused) { *used = (int)C.used; *refused = (int)C.refused; }
bool captcha_busy(void) { return C.th && WaitForSingleObject(C.th, 0) == WAIT_TIMEOUT; }

int captcha_result(char *err, size_t errn) {
    if (errn) err[0] = 0;
    if (C.state == CS_OPEN) return 0;
    if (captcha_busy()) return 0;               /* ответ есть, но окно ещё убирается: подождём поток */
    if (C.state == CS_OK) return 1;
    if (C.state == CS_FAIL) { snprintf(err, errn, "%s", C.err); return -1; }
    return -1;
}

void captcha_close(void) {
    if (C.th) {
        HWND h = C.hwnd;
        if (h) PostMessageW(h, WM_MC_CLOSE, 0, 0);
        /* SECURITY К2: поток не вышел за 20 с — он ещё может писать в C.out (буфер вызывающего).
         * Ничего не освобождаем и не сбрасываем: captcha_busy() останется true, вызывающий не освободит буфер. */
        if (WaitForSingleObject(C.th, 20000) == WAIT_TIMEOUT) return;
        CloseHandle(C.th);
        C.th = NULL;
    }
    if (C.udf[0]) { rm_tree(C.udf); C.udf[0] = 0; }
    C.state = CS_IDLE;
    C.out = NULL;
}
#endif
