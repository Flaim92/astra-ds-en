/* Платформенный слой: Windows. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincred.h>
#include <shellapi.h>
#include <werapi.h>
#include <avrt.h>
#include <process.h>
#include <psapi.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "gui.h"

/* ---------- ожидание ---------- */
/* Waitable{0, W_WINMSG} (от term_waitables в режиме окна) — ждать ещё и очередь сообщений окна:
 * MsgWaitForMultipleObjectsEx просыпается и от объектов, и от ввода/перерисовки окна. */
void plat_wait(const Waitable *w, int n, int timeout_ms) {
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    int k = 0;
    bool msgs = false;
    for (int i = 0; i < n; i++) {
        if (!w[i].h && (w[i].ev & W_WINMSG)) msgs = true;
        else if (w[i].h && k < MAXIMUM_WAIT_OBJECTS - 1) hs[k++] = (HANDLE)w[i].h;
    }
    DWORD t = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    if (msgs) { MsgWaitForMultipleObjectsEx((DWORD)k, hs, t, QS_ALLINPUT, MWMO_INPUTAVAILABLE); return; }
    if (k == 0) { Sleep(t == INFINITE ? 1000 : t); return; }
    WaitForMultipleObjects((DWORD)k, hs, FALSE, t);
}

struct PlatWake { HANDLE ev; };
PlatWake *wake_new(void) {
    PlatWake *w = calloc(1, sizeof *w);
    if (w) w->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    return w;
}
void wake_signal(PlatWake *w) { if (w) SetEvent(w->ev); }
void wake_clear(PlatWake *w) { if (w) ResetEvent(w->ev); }
Waitable wake_waitable(PlatWake *w) { return (Waitable){ w ? (intptr_t)w->ev : 0, W_IN }; }
void wake_free(PlatWake *w) { if (w) { CloseHandle(w->ev); free(w); } }

struct SockWait { SOCKET s; WSAEVENT ev; };
SockWait *sockwait_new(intptr_t sock) {
    SockWait *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->s = (SOCKET)sock;
    s->ev = WSACreateEvent();
    WSAEventSelect(s->s, s->ev, FD_READ | FD_CLOSE);
    SetEvent(s->ev);                            /* данные могли прийти раньше привязки */
    return s;
}
Waitable sockwait_get(SockWait *s) { return (Waitable){ s ? (intptr_t)s->ev : 0, W_IN }; }
void sockwait_clear(SockWait *s) { if (s) WSAResetEvent(s->ev); }
void sockwait_free(SockWait *s) {
    if (!s) return;
    WSAEventSelect(s->s, NULL, 0);
    WSACloseEvent(s->ev);
    free(s);
}

/* ---------- время ---------- */
int64_t plat_mono_ms(void) { return (int64_t)GetTickCount64(); }
int64_t plat_wall_ms(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u = { .LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime };
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}
int64_t plat_timegm(struct tm *t) { return (int64_t)_mkgmtime(t); }
void plat_localtime(int64_t s, struct tm *out) { time_t t = (time_t)s; localtime_s(out, &t); }
void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }

/* ---------- память ---------- */
void *plat_pages(size_t n) { return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
void plat_pages_free(void *p, size_t n) { (void)n; if (p) VirtualFree(p, 0, MEM_RELEASE); }
void plat_lock(void *p, size_t n) {
    VirtualLock(p, n);
    WerRegisterExcludedMemoryBlock(p, (DWORD)n);   /* закреплённые страницы (токен, ключи) — не в дамп */
}
void plat_unlock(void *p, size_t n) { WerUnregisterExcludedMemoryBlock(p); VirtualUnlock(p, n); }
void plat_wipe(void *p, size_t n) { if (p && n) SecureZeroMemory(p, n); }
void plat_trim(void) { _heapmin(); }
int64_t plat_cpu_ms(void) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER kk = { .LowPart = k.dwLowDateTime, .HighPart = k.dwHighDateTime }, uu = { .LowPart = u.dwLowDateTime, .HighPart = u.dwHighDateTime };
    return (int64_t)((kk.QuadPart + uu.QuadPart) / 10000ULL);
}
size_t plat_private_bytes(void) {
    PROCESS_MEMORY_COUNTERS_EX m = { .cb = sizeof m };
    return K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m) ? m.PrivateUsage : 0;
}

/* ---------- процесс ---------- */
static void (*g_on_crash)(void);
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    (void)e;
    /* Сначала затираем токен и ключи, затем умираем сами — до того, как WER снимет дамп. */
    if (g_on_crash) g_on_crash();
    TerminateProcess(GetCurrentProcess(), 0xDEAD);
    return EXCEPTION_EXECUTE_HANDLER;
}

void plat_harden(void (*on_crash)(void)) {
    g_on_crash = on_crash;
    /* DLL только из System32: подложенная рядом библиотека не загрузится. */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    /* Дамп WER без кучи (NOHEAP), страницы секретов исключены из дампа (plat_lock). Раньше здесь был
     * WerAddExcludedApplication — он навсегда оставлял в реестре HKCU след «mincord.exe = 1» и не
     * спасал при fail-fast (SECURITY.md М1). Старый след убираем. */
    WerSetFlags(WER_FAULT_REPORTING_NO_UI | WER_FAULT_REPORTING_FLAG_DISABLE_THREAD_SUSPENSION | WER_FAULT_REPORTING_FLAG_NOHEAP);
    SetUnhandledExceptionFilter(crash_filter);
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n && n < MAX_PATH) {
        WCHAR *base = wcsrchr(path, L'\\');
        WerRemoveExcludedApplication(base ? base + 1 : path, FALSE);
        WerRemoveExcludedApplication(L"mincord.exe", FALSE);         /* следы прежнего имени в HKCU */
        WerRemoveExcludedApplication(L"mincord-dbg.exe", FALSE);
    }
    /* Политики смягчения: строгая проверка дескрипторов, запрет AppInit/старых точек
     * внедрения, запрет загрузки образов с сетевых путей и с низкой меткой целостности. */
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY sh = {0};
    sh.RaiseExceptionOnInvalidHandleReference = 1;
    sh.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &sh, sizeof sh);
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ep = {0};
    ep.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ep, sizeof ep);
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY il = {0};
    il.NoRemoteImages = 1;
    il.NoLowMandatoryLabelImages = 1;
    il.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &il, sizeof il);
}

void plat_net_init(void) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
const char *plat_ca_file(void) { return NULL; }

/* ---------- потоки ---------- */
struct PlatThread { HANDLE h; int (*fn)(void *); void *arg; };
static unsigned __stdcall thread_tramp(void *p) { PlatThread *t = p; return (unsigned)t->fn(t->arg); }
PlatThread *thread_start(int (*fn)(void *), void *arg) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = (HANDLE)_beginthreadex(NULL, 256 * 1024, thread_tramp, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}
void thread_join(PlatThread *t) {
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}
int plat_ui_lang(void) {                        /* язык интерфейса Windows: ru/uk/be -> русский */
    LANGID l = GetUserDefaultUILanguage();
    WORD p = PRIMARYLANGID(l);
    return p == LANG_RUSSIAN || p == LANG_UKRAINIAN || p == LANG_BELARUSIAN ? 0 : 1;
}
/* такт звонка — точнее 15,6 мс. Ревью 18 С1: winmm.dll не в статическом импорте (+~200 КБ всегда) — грузится
 * только на время звонка и выгружается после (вызовы — из одного потока звонка) */
static HMODULE tm_dll;
static UINT (WINAPI *tm_begin)(UINT), (WINAPI *tm_end)(UINT);
void plat_timer_hires(bool on) {
    if (on && !tm_dll) {
        tm_dll = LoadLibraryExW(L"winmm.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!tm_dll) return;
        tm_begin = (UINT (WINAPI *)(UINT))(void *)GetProcAddress(tm_dll, "timeBeginPeriod");
        tm_end = (UINT (WINAPI *)(UINT))(void *)GetProcAddress(tm_dll, "timeEndPeriod");
        if (!tm_begin || !tm_end) { FreeLibrary(tm_dll); tm_dll = NULL; tm_begin = tm_end = NULL; return; }
        tm_begin(1);
    } else if (!on && tm_dll) {
        tm_end(1);
        FreeLibrary(tm_dll);
        tm_dll = NULL;
        tm_begin = tm_end = NULL;
    }
}
void thread_audio_priority(void) {
    DWORD idx = 0;
    if (!AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}
struct PlatMutex { CRITICAL_SECTION cs; };
PlatMutex *mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); if (m) InitializeCriticalSection(&m->cs); return m; }
void mutex_lock(PlatMutex *m) { EnterCriticalSection(&m->cs); }
void mutex_unlock(PlatMutex *m) { LeaveCriticalSection(&m->cs); }
void mutex_free(PlatMutex *m) { if (m) { DeleteCriticalSection(&m->cs); free(m); } }

/* ---------- UDP ---------- */
intptr_t udp_open(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return -1;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in a = { .sin_family = AF_INET };
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return -1; }
    return (intptr_t)s;
}
bool udp_resolve(const char *ip, uint16_t port, void *addr16) {
    struct sockaddr_in *a = addr16;
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    return inet_pton(AF_INET, ip, &a->sin_addr) == 1;
}
int udp_send(intptr_t s, const void *buf, size_t n, const void *addr16) {
    return sendto((SOCKET)s, buf, (int)n, 0, (const struct sockaddr *)addr16, sizeof(struct sockaddr_in));
}
int udp_recv(intptr_t s, void *buf, size_t n, void *from16) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    int al = (int)sizeof a;
    int r = recvfrom((SOCKET)s, buf, (int)n, 0, (struct sockaddr *)&a, &al);
    if (r < 0) return -1;
    if (from16) memcpy(from16, &a, sizeof a);
    return (int)r;
}
bool udp_same(const void *a16, const void *b16) {
    const struct sockaddr_in *a = a16, *b = b16;
    return a->sin_family == AF_INET && b->sin_family == AF_INET && a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}
void udp_close(intptr_t s) { if (s != -1) closesocket((SOCKET)s); }

/* ---------- хранилище секретов: Диспетчер учётных данных ---------- */
/* Самопроверка пишет в отдельные записи и не трогает сохранённые токен и ссылку пользователя. */
static bool kr_test;
void keyring_test_mode(void) { kr_test = true; }
#define KR_NEW "astra-ds"
#define KR_OLD "mincord"
/* <svc>/<entry>; в самопроверке <svc>/selftest (токен) и <svc>/selftest-<entry> (прочее) */
static bool cred_target(const char *svc, const char *entry, WCHAR *out, size_t cap) {
    char a[64];
    if (kr_test) snprintf(a, sizeof a, strcmp(entry, "token") ? "%s/selftest-%s" : "%s/selftest", svc, entry);
    else snprintf(a, sizeof a, "%s/%s", svc, entry);
    size_t n = strlen(a);
    if (n + 1 > cap) return false;
    for (size_t i = 0; i <= n; i++) out[i] = (WCHAR)(unsigned char)a[i];
    return true;
}
static int kr_load_svc(const char *svc, const char *entry, char *out, size_t cap) {
    WCHAR target[64];
    if (!cred_target(svc, entry, target, 64)) return -1;
    PCREDENTIALW c = NULL;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &c)) return GetLastError() == ERROR_NOT_FOUND ? -1 : -2;   /* -2 — ошибка чтения */
    int n = -1;
    if (c->CredentialBlobSize > 0 && c->CredentialBlobSize < cap) {
        memcpy(out, c->CredentialBlob, c->CredentialBlobSize);
        n = (int)c->CredentialBlobSize;
    }
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return n;
}
static bool kr_save_svc(const char *svc, const char *entry, const char *tok, size_t n) {
    WCHAR target[64];
    if (!cred_target(svc, entry, target, 64) || n > 5 * 512) return false;   /* предел Windows: 2560 байт */
    CREDENTIALW c = {0};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target;
    c.CredentialBlobSize = (DWORD)n;
    c.CredentialBlob = (LPBYTE)tok;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;     /* только этот компьютер, без роуминга профиля */
    c.UserName = L"astra-ds";
    return CredWriteW(&c, 0) != 0;
}
static void kr_delete_svc(const char *svc, const char *entry) {
    WCHAR target[64];
    if (cred_target(svc, entry, target, 64)) CredDeleteW(target, CRED_TYPE_GENERIC, 0);
}
/* Переименование mincord -> Astra DS (SECURITY П1): записи хранилища под новым именем. Если под новым пусто,
 * а под старым есть — прочитать старое, записать новое, ПЕРЕЧИТАТЬ и сравнить, и только тогда удалить
 * старое; при любом сбое старое не трогаем (повтор при следующем запуске). Удаление — под обоими именами.
 * Временный буфер сверки — закреплённые страницы, затираются. Самопроверка — свои записи, без миграции. */
static bool kr_ro;                              /* --no-save: миграцию не записывать (только читать старое) */
void keyring_readonly(bool on) { kr_ro = on; }
int keyring_load(const char *entry, char *out, size_t cap) {
    int n = kr_load_svc(KR_NEW, entry, out, cap);
    if (n >= 0) { if (!kr_ro) kr_delete_svc(KR_OLD, entry); return n; }   /* П1-а: старый след убрать тихо */
    if (n == -2) return -2;                     /* ревью 8 М3: ошибка чтения — не мигрировать, не «нет записи» */
    int m = kr_load_svc(KR_OLD, entry, out, cap);
    if (m < 0) return m;
    if (kr_ro || !kr_save_svc(KR_NEW, entry, out, (size_t)m)) return m;   /* П1-б: --no-save — не писать */
    char *chk = plat_pages(cap);
    if (!chk) return m;
    plat_lock(chk, cap);
    int k = kr_load_svc(KR_NEW, entry, chk, cap);
    bool same = k == m && !memcmp(chk, out, (size_t)m);
    plat_wipe(chk, cap);
    plat_unlock(chk, cap);
    plat_pages_free(chk, cap);
    if (same) kr_delete_svc(KR_OLD, entry);                               /* перенесено и сверено */
    return m;
}
bool keyring_save(const char *entry, const char *data, size_t n) { return kr_save_svc(KR_NEW, entry, data, n); }
void keyring_delete(const char *entry) {
    kr_delete_svc(KR_NEW, entry);
    kr_delete_svc(KR_OLD, entry);                                         /* --logout, паника: и под старым именем */
}
/* --keyringtest: записать под СТАРЫМ именем (проверка миграции; только в режиме самопроверки) */
bool keyring_save_old(const char *entry, const char *data, size_t n) { return kr_test && kr_save_svc(KR_OLD, entry, data, n); }
int keyring_load_raw(const char *entry, bool old, char *out, size_t cap) { return kr_test ? kr_load_svc(old ? KR_OLD : KR_NEW, entry, out, cap) : -1; }

const char *keyring_name(void) { return "Диспетчер учётных данных Windows"; }

/* ---------- дочерняя программа ---------- */
struct PlatFile { HANDLE h; WCHAR path[MAX_PATH]; };
PlatFile *exe_open(const char *name, char *path, size_t pathn) {
    PlatFile *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    DWORD n = GetModuleFileNameW(NULL, f->path, MAX_PATH);
    WCHAR *base = n && n < MAX_PATH ? wcsrchr(f->path, L'\\') : NULL;
    size_t k = strlen(name);
    if (!base || (size_t)(base + 1 - f->path) + k + 1 > MAX_PATH) { free(f); return NULL; }
    base++;
    for (size_t i = 0; i <= k; i++) base[i] = (WCHAR)(unsigned char)name[i];
    if (path && pathn && !WideCharToMultiByte(CP_UTF8, 0, f->path, -1, path, (int)pathn, NULL, NULL)) path[0] = 0;
    /* Запрет записи и удаления (а с ним и переименования — файла и каталогов над ним), пока хэндл
     * открыт: проверенный файл и есть тот, что запустит CreateProcess. */
    f->h = CreateFileW(f->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f->h == INVALID_HANDLE_VALUE) { free(f); return NULL; }
    return f;
}
long exe_read(PlatFile *f, void *buf, size_t n) {
    DWORD got = 0;
    if (!ReadFile(f->h, buf, n > 0x40000000 ? 0x40000000 : (DWORD)n, &got, NULL)) return -1;
    return (long)got;
}
void exe_close(PlatFile *f) { if (f) { CloseHandle(f->h); free(f); } }

struct PlatProc { HANDLE h; DWORD pid; };
static HANDLE g_job;
PlatProc *proc_spawn(PlatFile *f, const char *const *argv, const void *in, size_t n) {
    if (!g_job) {
        /* Задание убивает все свои процессы, когда закрывается его последний хэндл — то есть
         * когда завершается mincord, как бы он ни завершился. */
        g_job = CreateJobObjectW(NULL, NULL);
        if (!g_job) return NULL;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li)) {
            CloseHandle(g_job); g_job = NULL; return NULL;
        }
    }
    /* Командная строка: "путь" и аргументы (простые, без кавычек и пробелов). */
    WCHAR cmd[MAX_PATH + 256];
    int pos = swprintf(cmd, MAX_PATH + 256, L"\"%ls\"", f->path);
    for (int i = 1; argv[i] && pos > 0; i++) {
        size_t k = strlen(argv[i]);
        if ((size_t)pos + k + 2 >= MAX_PATH + 256) return NULL;
        cmd[pos++] = L' ';
        for (size_t j = 0; j < k; j++) cmd[pos++] = (WCHAR)(unsigned char)argv[i][j];
        cmd[pos] = 0;
    }
    if (pos <= 0) return NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, (DWORD)(n + 4096))) return NULL;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    PlatProc *p = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST al = NULL;
    if (nul == INVALID_HANDLE_VALUE) goto out;
    /* Наследуются только эти два хэндла, а не всё наследуемое, что есть в процессе. */
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    al = malloc(asz);
    if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &asz)) { free(al); al = NULL; goto out; }
    HANDLE inh[2] = { rd, nul };
    if (!UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inh, sizeof inh, NULL, NULL)) goto out;
    STARTUPINFOEXW si = {0};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = rd;
    si.StartupInfo.hStdOutput = nul;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = al;
    /* Окружение: SystemRoot (без него Go не загрузит системные библиотеки) и настройки среды Go
     * на минимум ресурсов: один рабочий поток, чаще сборка мусора, мягкий потолок кучи. */
    static const WCHAR GOENV[] = L"GOMAXPROCS=1\0GOGC=50\0GOMEMLIMIT=12MiB\0";
    WCHAR env[MAX_PATH + 16 + sizeof GOENV / sizeof(WCHAR)] = L"SystemRoot=";
    UINT sl = GetWindowsDirectoryW(env + 11, MAX_PATH);
    if (!sl || sl >= MAX_PATH) goto out;
    env[11 + sl] = 0;
    memcpy(env + 11 + sl + 1, GOENV, sizeof GOENV);   /* \0 в конце GOENV и \0 строки — конец блока окружения */
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(f->path, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        env, NULL, &si.StartupInfo, &pi)) goto out;
    if (!AssignProcessToJobObject(g_job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        goto out;
    }
    p = calloc(1, sizeof *p);
    if (!p) { TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); goto out; }
    p->h = pi.hProcess;
    p->pid = pi.dwProcessId;
    CloseHandle(rd); rd = NULL;
    /* Конфиг целиком помещается в буфер канала: запись не блокируется и до старта процесса. */
    DWORD w;
    size_t off = 0;
    while (off < n && WriteFile(wr, (const char *)in + off, (DWORD)(n - off), &w, NULL) && w) off += w;
    CloseHandle(wr); wr = NULL;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
out:
    if (al) { DeleteProcThreadAttributeList(al); free(al); }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return p;
}
bool proc_alive(PlatProc *p) { return p && WaitForSingleObject(p->h, 0) == WAIT_TIMEOUT; }
Waitable proc_waitable(PlatProc *p) { return (Waitable){ p ? (intptr_t)p->h : 0, W_IN }; }
void proc_kill(PlatProc *p) {
    if (!p) return;
    TerminateProcess(p->h, 1);
    WaitForSingleObject(p->h, 2000);
    CloseHandle(p->h);
    free(p);
}
long proc_rss_kb(PlatProc *p) {
    PROCESS_MEMORY_COUNTERS_EX m = { .cb = sizeof m };
    if (!p || !K32GetProcessMemoryInfo(p->h, (PROCESS_MEMORY_COUNTERS *)&m, sizeof m)) return -1;
    return (long)(m.WorkingSetSize / 1024);
}
int tcp_free_port(void) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a, port = -1;
    /* Без SO_REUSEADDR и с монопольным захватом: чужой процесс не «делит» найденный порт */
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr *)&a, &len) == 0) port = ntohs(a.sin_port);
    closesocket(s);
    return port;
}
int tcp_local_ready(int port, int timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((u_short)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) r = 1;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        /* Windows повторяет SYN на закрытый порт ~2 с: ждём не дольше timeout_ms */
        fd_set wf, ef;
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(s, &wf); FD_SET(s, &ef);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(0, NULL, &wf, &ef, &tv) > 0 && FD_ISSET(s, &wf)) r = 1;
    }
    closesocket(s);
    return r;
}

/* ---------- терминал ----------
 * Два исполнения term_*: консоль (--tui, ниже) и окно (по умолчанию, gui_win.c + gui_grid.c).
 * Выбор — term_open_window() до первого вывода: дальше все term_* идут в окно. */
static HANDLE t_in, t_out;
static DWORD t_in_mode, t_out_mode;
static UINT t_cp_out, t_cp_in;
static bool t_active, t_gui;
void term_console_puts(const char *s);

/* Открыть окно; false — окна не будет, остаётся консоль. Если консоль создана только для нас
 * (запуск двойным щелчком: в ней один процесс — мы), она убирается: работаем без консоли.
 * Запуск из терминала оставляет его как есть (оболочка ждёт завершения, --tui/--cli там же). */
bool term_open_window(bool hidden) {
    if (t_gui) return true;
    if (!gb_open(hidden)) return false;
    DWORD pl[4];
    if (!hidden && GetConsoleProcessList(pl, 4) == 1) FreeConsole();
    t_gui = true;
    return true;
}
bool term_windowed(void) { return t_gui; }
/* Снимок (--shot): ввод по сценарию, размер сетки, запись BMP. */
void term_inject(const Key *k) { if (t_gui) gb_inject(k); }
void term_resize(int w, int h) { if (t_gui) gui_resize(w, h); }
bool term_snapshot(const char *path) { return t_gui && gb_snapshot(path); }
int term_mouse_seen(void) { return gui_mouse_seen; }
/* Мелкая сетка картинок (слои ESC _ mincord-img, см. gui.h): только окно. Сильный символ —
 * заменяет слабую заглушку ui.c. */
int term_subcell_scale(void) { return t_gui ? gb_fine_scale() : 0; }
int term_fullscreen(int mode) { return t_gui ? gb_fullscreen(mode) : -1; }
/* №141: рация на выбранной клавише — опрос состояния ОДНОЙ клавиши (не перехват ввода) */
/* Вне фокуса окна — только непечатные клавиши (решение в PLAN.md, раздел «Голос»). */
static bool vk_global_ok(int vk) {
    /* Р-2: цифровой блок — не «глобальный» (с NumLock это цифры: PIN в другой программе включил бы микрофон) */
    return vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || (vk >= VK_F13 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL;
}
bool plat_key_down(int vk) {
    if (vk <= 0 || vk >= 256) return false;
    /* Р-1: «в фокусе» — окно (gui) или консоль этого процесса на переднем плане (--tui/--cli) */
    bool focused = t_gui ? win_focused() : (GetConsoleWindow() && GetConsoleWindow() == GetForegroundWindow());
    if (!vk_global_ok(vk) && !focused) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
void plat_notify(const char *title, const char *text) { if (t_gui) gb_notify(title, text); }   /* №123: только окно */
/* №141: точный VK события окна (для назначения рации): боковые кнопки — K_MOUSE 8/9, клавиши — последний WM_KEYDOWN */
int plat_key_vk(const Key *k) {
    if (!k) return 0;
    if (k->code == K_MOUSE) return k->button == 8 ? VK_XBUTTON1 : k->button == 9 ? VK_XBUTTON2 : 0;
    return t_gui ? gb_last_vk() : 0;
}
void plat_key_name(int vk, char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (vk == VK_XBUTTON1) { snprintf(out, n, "мышь: кнопка 4"); return; }
    if (vk == VK_XBUTTON2) { snprintf(out, n, "мышь: кнопка 5"); return; }
    if (vk == VK_MBUTTON) { snprintf(out, n, "мышь: колесо"); return; }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if (vk == VK_RCONTROL || vk == VK_RMENU || (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE) lp |= 1 << 24;
    WCHAR w[64];
    if (sc && GetKeyNameTextW(lp, w, 64) > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    else snprintf(out, n, "клавиша %d", vk);
}
bool term_fine_images(void) { return term_subcell_scale() >= 4; }
bool term_zoom(int step) { return t_gui && gb_zoom(step); }
int term_zoom_pct(void) { return t_gui ? gb_zoom_pct() : 100; }

/* Аргументы в UTF-8: argv от CRT — в кодовой странице ANSI, кириллица в --mic=, --shot-keys=t:... портится. */
char **plat_utf8_argv(int *argc) {
    int n = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w || n <= 0) return NULL;
    char **v = calloc((size_t)n + 1, sizeof *v);
    for (int i = 0; v && i < n; i++) {
        int k = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        v[i] = k > 0 ? malloc((size_t)k) : NULL;
        if (!v[i] || !WideCharToMultiByte(CP_UTF8, 0, w[i], -1, v[i], k, NULL, NULL)) { LocalFree(w); return NULL; }
    }
    LocalFree(w);
    if (v) *argc = n;
    return v;
}

void term_puts(const char *s) {
    if (t_gui) { gui_puts(s); return; }
    term_console_puts(s);
}
void term_console_puts(const char *s) {
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    UINT cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    DWORD w;
    WriteFile(o, s, (DWORD)strlen(s), &w, NULL);
    SetConsoleOutputCP(cp);
}

bool term_interactive(void) { DWORD m; return t_gui || GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m) != 0; }

/* Видимая строка (выбор в меню). false — конец ввода / окно закрыто. */
bool term_read_line(const char *prompt, char *out, size_t cap) {
    if (t_gui) return gui_read_line(prompt, out, cap, false);
    term_puts(prompt);
    if (!cap || !fgets(out, (int)cap, stdin)) return false;
    out[strcspn(out, "\r\n")] = 0;
    return true;
}

bool term_read_secret(const char *prompt, char *out, size_t cap) {
    if (t_gui) return gui_read_line(prompt, out, cap, true);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    term_puts(prompt);
    if (!cap || !GetConsoleMode(in, &mode)) return false;
    out[0] = 0;
    /* Без эха; строку читаем сами через ReadConsoleW — в историю консоли она не попадает.
     * Буфер — под длинные ссылки vless:// (сотни символов, с ключом ML-DSA — тысячи). */
    SetConsoleMode(in, (mode | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT) & ~ENABLE_ECHO_INPUT);
    static WCHAR wbuf[8192];
    DWORD got = 0;
    BOOL ok = ReadConsoleW(in, wbuf, 8191, &got, NULL);
    bool line_end = false;
    for (DWORD i = 0; ok && i < got; i++) if (wbuf[i] == L'\r' || wbuf[i] == L'\n') { line_end = true; break; }
    if (ok && !line_end) {
        /* строка длиннее буфера: дочитать хвост до конца строки и отбросить */
        WCHAR t[256];
        DWORD g2 = 0;
        while (ReadConsoleW(in, t, 256, &g2, NULL) && g2) {
            bool e = false;
            for (DWORD i = 0; i < g2; i++) if (t[i] == L'\n') e = true;
            SecureZeroMemory(t, sizeof t);
            if (e) break;
        }
    }
    SetConsoleMode(in, mode);
    term_puts("\r\n");
    /* Пробелы, кавычки и табуляции (часто прилипают при копировании) выбрасываются; остальное —
     * в UTF-8. Проверку формата делает получатель (токен — только ASCII, ссылка — свои правила). */
    size_t n = 0;
    bool bad = !ok || !line_end;
    for (DWORD i = 0; !bad && i < got; i++) {
        WCHAR c = wbuf[i];
        if (c == L'\r' || c == L'\n') break;
        if (c == L' ' || c == L'"' || c == L'\t') continue;
        char u[4];
        int k = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, sizeof u, NULL, NULL);
        if (k <= 0) { u[0] = '?'; k = 1; }      /* половинка суррогатной пары и т. п. */
        if (n + (size_t)k >= cap) { bad = true; break; }
        memcpy(out + n, u, (size_t)k);
        n += (size_t)k;
        SecureZeroMemory(u, sizeof u);
    }
    if (bad) n = 0;
    SecureZeroMemory(out + n, cap - n);
    SecureZeroMemory(wbuf, sizeof wbuf);
    return !bad;                                  /* пустая строка (просто Enter) — true и "" */
}

bool term_init(void) {
    if (t_gui) return gui_init();
    t_in = GetStdHandle(STD_INPUT_HANDLE);
    t_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(t_in, &t_in_mode) || !GetConsoleMode(t_out, &t_out_mode)) return false;
    t_cp_out = GetConsoleOutputCP();
    t_cp_in = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleMode(t_out, t_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | DISABLE_NEWLINE_AUTO_RETURN);
    /* Сырой ввод: без эха, без построчности, Ctrl+C — обычная клавиша; с событиями размера окна
     * и мыши. ENABLE_EXTENDED_FLAGS без ENABLE_QUICK_EDIT_MODE выключает выделение мышью
     * (QuickEdit) на время работы — иначе щелчок не дойдёт до программы; при выходе режим возвращается. */
    SetConsoleMode(t_in, ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS);
    t_active = true;
    /* Альтернативный экран: после выхода прокрутка не содержит переписки. */
    const char *s = "\x1b[?1049h\x1b[?25l\x1b[2J";
    term_write(s, strlen(s));
    return true;
}
void term_shutdown(void) {
    if (t_gui) { gui_shutdown(); return; }
    if (!t_active) return;
    const char *s = "\x1b[0m\x1b[2J\x1b[?25h\x1b[?1049l";
    term_write(s, strlen(s));
    /* QuickEdit возвращается только вместе с ENABLE_EXTENDED_FLAGS */
    SetConsoleMode(t_in, t_in_mode | (t_in_mode & ENABLE_QUICK_EDIT_MODE ? ENABLE_EXTENDED_FLAGS : 0));
    SetConsoleMode(t_out, t_out_mode);
    SetConsoleOutputCP(t_cp_out);
    SetConsoleCP(t_cp_in);
    t_active = false;
}
void term_size(int *w, int *h) {
    if (t_gui) { gui_size(w, h); return; }
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (GetConsoleScreenBufferInfo(t_out, &i)) {
        *w = i.srWindow.Right - i.srWindow.Left + 1;
        *h = i.srWindow.Bottom - i.srWindow.Top + 1;
    } else { *w = 80; *h = 25; }
}
void term_write(const char *s, size_t n) {
    if (t_gui) { gui_write(s, n); return; }
    DWORD w;
    while (n) {
        DWORD chunk = n > 65536 ? 65536 : (DWORD)n;
        if (!WriteFile(t_out, s, chunk, &w, NULL) || !w) return;
        s += w;
        n -= w;
    }
}
int term_waitables(Waitable *out, int max) {
    if (t_gui) return gb_waitables(out, max);
    if (max < 1) return 0;
    out[0] = (Waitable){ (intptr_t)t_in, W_IN };
    return 1;
}

int plat_win_map_vk(WORD vk) {
    switch (vk) {
    case VK_RETURN: return K_ENTER; case VK_TAB: return K_TAB; case VK_BACK: return K_BS;
    case VK_DELETE: return K_DEL; case VK_UP: return K_UP; case VK_DOWN: return K_DOWN;
    case VK_LEFT: return K_LEFT; case VK_RIGHT: return K_RIGHT; case VK_HOME: return K_HOME;
    case VK_END: return K_END; case VK_PRIOR: return K_PGUP; case VK_NEXT: return K_PGDN;
    case VK_ESCAPE: return K_ESC; case VK_INSERT: return K_INS;
    default:
        if (vk >= VK_F1 && vk <= VK_F12) return K_F1 + (vk - VK_F1);
        return K_NONE;
    }
}

/* Мышь консоли: нажатие (новые биты dwButtonState), колесо, перемещение со сменой клетки. */
static DWORD m_prev;
static int m_lx = -1, m_ly = -1;
static bool con_mouse(const MOUSE_EVENT_RECORD *m, Key *k) {
    CONSOLE_SCREEN_BUFFER_INFO bi;
    int ox = 0, oy = 0;
    if (GetConsoleScreenBufferInfo(t_out, &bi)) { ox = bi.srWindow.Left; oy = bi.srWindow.Top; }
    int x = m->dwMousePosition.X - ox, y = m->dwMousePosition.Y - oy, b;
    DWORD st = m->dwControlKeyState;
    if (m->dwEventFlags & MOUSE_WHEELED) b = (short)HIWORD(m->dwButtonState) > 0 ? 4 : 5;
    else if (m->dwEventFlags & MOUSE_HWHEELED) return false;
    else if (m->dwEventFlags & MOUSE_MOVED) {
        m_prev = m->dwButtonState & 0xFFFF;
        if (x == m_lx && y == m_ly) return false;
        b = 0;
    } else {                                     /* нажатие/отпускание (и второе нажатие двойного щелчка) */
        DWORD now = m->dwButtonState & 0xFFFF, down = now & ~m_prev;
        m_prev = now;
        if (down & FROM_LEFT_1ST_BUTTON_PRESSED) b = 1;
        else if (down & RIGHTMOST_BUTTON_PRESSED) b = 3;
        else if (down & FROM_LEFT_2ND_BUTTON_PRESSED) b = 2;
        else return false;
    }
    if (x < 0 || y < 0) return false;
    m_lx = x;
    m_ly = y;
    *k = (Key){ .code = K_MOUSE, .x = x, .y = y, .button = b,
                .ctrl = (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0,
                .alt = (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0, .shift = (st & SHIFT_PRESSED) != 0 };
    return true;
}

int term_read(Key *out, int max) {
    if (t_gui) return gui_read(out, max);
    DWORD avail = 0;
    if (!GetNumberOfConsoleInputEvents(t_in, &avail) || avail == 0) return 0;
    INPUT_RECORD rec[128];
    DWORD got = 0;
    if (!ReadConsoleInputW(t_in, rec, avail < 128 ? avail : 128, &got)) return 0;
    int n = 0;
    static WCHAR hi_surrogate;
    for (DWORD i = 0; i < got && n < max; i++) {
        if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) { out[n++] = (Key){ .code = K_RESIZE }; continue; }
        if (rec[i].EventType == MOUSE_EVENT) { if (con_mouse(&rec[i].Event.MouseEvent, &out[n])) n++; continue; }
        if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown) continue;
        KEY_EVENT_RECORD *k = &rec[i].Event.KeyEvent;
        DWORD st = k->dwControlKeyState;
        bool ctrl = st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
        bool alt = st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
        bool shift = st & SHIFT_PRESSED;
        /* AltGr = Ctrl+Alt: это ввод символа, а не сочетание */
        if (ctrl && alt && k->uChar.UnicodeChar >= 0x20) ctrl = alt = false;
        int code = plat_win_map_vk(k->wVirtualKeyCode);
        WCHAR wc = k->uChar.UnicodeChar;
        for (WORD r = 0; r < (k->wRepeatCount ? k->wRepeatCount : 1) && n < max; r++) {
            if (code == K_ENTER) {
                /* Вставка приходит пачкой: Enter посреди пачки — перевод строки, а не отправка. */
                bool more = false;
                for (DWORD j = i + 1; j < got; j++)
                    if (rec[j].EventType == KEY_EVENT && rec[j].Event.KeyEvent.bKeyDown) { more = true; break; }
                if (more || shift || ctrl || alt) out[n++] = (Key){ .code = K_CHAR, .ch = '\n' };   /* Shift/Ctrl/Alt+Enter — перенос */
                else out[n++] = (Key){ .code = K_ENTER, .ctrl = ctrl, .alt = alt };
            } else if (code == K_TAB && shift) {
                out[n++] = (Key){ .code = K_BTAB };
            } else if (code != K_NONE) {
                out[n++] = (Key){ .code = code, .ctrl = ctrl, .alt = alt, .shift = shift };
            } else if (wc >= 0xD800 && wc <= 0xDBFF) {
                hi_surrogate = wc;
            } else if (wc >= 0xDC00 && wc <= 0xDFFF && hi_surrogate) {
                uint32_t cp = 0x10000 + (((uint32_t)hi_surrogate - 0xD800) << 10) + (wc - 0xDC00);
                hi_surrogate = 0;
                out[n++] = (Key){ .code = K_CHAR, .ch = cp, .alt = alt };
            } else if (ctrl && k->wVirtualKeyCode >= 'A' && k->wVirtualKeyCode <= 'Z') {
                out[n++] = (Key){ .code = K_CHAR, .ch = (uint32_t)(k->wVirtualKeyCode - 'A' + 'a'), .ctrl = true, .alt = alt };
            } else if (wc >= 0x20) {
                out[n++] = (Key){ .code = K_CHAR, .ch = wc, .alt = alt };
            }
        }
    }
    return n;
}
#endif
