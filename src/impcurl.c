/* Загрузчик libcurl-impersonate. См. impcurl.h.
 *
 * Windows: FindResource/LoadResource/LockResource -> запись во временный файл
 * с общим чтением и FILE_FLAG_DELETE_ON_CLOSE -> LoadLibrary. Файл удаляется
 * ядром при закрытии HANDLE, который мы держим до impcurl_shutdown().
 *
 * Linux: заглушка (вктабл не поднимается), пока нет .so импера. */
#include "impcurl.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#ifndef _WIN32
#include <errno.h>
#endif

static ImpCurl VT;
static bool tried, ready;
bool impcurl_verbose;                               /* --imptest: печатать, где падаем */
#define DBG(...) do { if (impcurl_verbose) { printf("impcurl: " __VA_ARGS__); printf("\n"); } } while (0)

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <zlib.h>

#define IMPCURL_RCDATA_ID 10

static HMODULE DLL_H;
static HANDLE DLL_FILE;
static wchar_t DLL_PATH[MAX_PATH];

/* Подмести распакованные DLL прошлых запусков (их уже никто не держит). */
static void sweep_old(const wchar_t *dir) {
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%sastra-ds-*.dll", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t full[MAX_PATH];
        _snwprintf(full, MAX_PATH, L"%s%s", dir, fd.cFileName);
        DeleteFileW(full);                           /* смапленную не удалит — и ладно */
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static bool extract_and_load(void) {
    HMODULE self = GetModuleHandleW(NULL);
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(IMPCURL_RCDATA_ID), RT_RCDATA);
    if (!r) { DBG("FindResource id=%d RT_RCDATA не найден, GetLastError=%lu", IMPCURL_RCDATA_ID, GetLastError()); return false; }
    HGLOBAL g = LoadResource(self, r);
    DWORD sz = SizeofResource(self, r);
    const void *data = g ? LockResource(g) : NULL;
    if (!data || !sz) { DBG("LoadResource/LockResource пусто (sz=%lu)", sz); return false; }
    DBG("ресурс найден, %lu байт", sz);
    /* ресурс — gzip (build.sh): распаковать в кучу на время записи файла. Размер — из хвоста gzip (ISIZE). */
    if (sz < 18) return false;
    const unsigned char *gz = data;
    uLong full = (uLong)gz[sz - 4] | (uLong)gz[sz - 3] << 8 | (uLong)gz[sz - 2] << 16 | (uLong)gz[sz - 1] << 24;
    if (full < 4096 || full > (64u << 20)) return false;
    unsigned char *dll = malloc(full);
    if (!dll) return false;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    bool zok = inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK;
    if (zok) {
        zs.next_in = (Bytef *)gz; zs.avail_in = sz;
        zs.next_out = dll; zs.avail_out = (uInt)full;
        zok = inflate(&zs, Z_FINISH) == Z_STREAM_END && zs.total_out == full;
        inflateEnd(&zs);
    }
    if (!zok) { free(dll); DBG("распаковка DLL не удалась"); return false; }
    data = dll;
    sz = (DWORD)full;

    wchar_t base[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, base)) { DBG("GetTempPath failed %lu", GetLastError()); free(dll); return false; }
    sweep_old(base);                                 /* убрать остатки прошлых запусков */
    /* случайная часть имени: 8 hex от QueryPerformanceCounter */
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    _snwprintf(DLL_PATH, MAX_PATH, L"%sastra-ds-%08x%08x.dll", base,
               (unsigned)GetCurrentProcessId(), (unsigned)c.LowPart);

    /* Шаг 1: записать DLL и ЗАКРЫТЬ ручку — иначе LoadLibrary не смапит (sharing violation). */
    HANDLE wf = CreateFileW(DLL_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (wf == INVALID_HANDLE_VALUE) { DBG("CreateFile(write) failed %lu", GetLastError()); free(dll); return false; }
    DWORD w = 0;
    if (!WriteFile(wf, data, sz, &w, NULL) || w != sz) {
        DBG("WriteFile failed %lu (w=%lu)", GetLastError(), w);
        CloseHandle(wf); DeleteFileW(DLL_PATH); free(dll); return false;
    }
    CloseHandle(wf);
    free(dll);

    /* Шаг 2: загрузить. */
    DLL_H = LoadLibraryExW(DLL_PATH, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!DLL_H) { DBG("LoadLibrary failed %lu", GetLastError()); DeleteFileW(DLL_PATH); return false; }
    DBG("LoadLibrary ок");

    /* Шаг 3: ручка-«сторож» с DELETE_ON_CLOSE. Файл реально удалится, когда закроются
     * и она, и секция загрузчика — то есть при выходе процесса. */
    DLL_FILE = CreateFileW(DLL_PATH, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (DLL_FILE == INVALID_HANDLE_VALUE) { DLL_FILE = NULL; DBG("сторож не открылся %lu (не критично)", GetLastError()); }
    return true;
}

static void *sym(const char *name) { return (void *)GetProcAddress(DLL_H, name); }

static void unload(void) {
    if (DLL_H) { FreeLibrary(DLL_H); DLL_H = NULL; }   /* распаковка файла отменяется только после unmap */
    if (DLL_FILE) { CloseHandle(DLL_FILE); DLL_FILE = NULL; }
    if (DLL_PATH[0]) {
        if (!DeleteFileW(DLL_PATH))                   /* ещё смаплена — удалить при перезагрузке + подметём на старте */
            MoveFileExW(DLL_PATH, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        DLL_PATH[0] = 0;
    }
}
#elif defined(__APPLE__)
/* macOS: своей сборки libcurl-impersonate пока нет — вход идёт через обычный libcurl (как без библиотеки). */
static bool extract_and_load(void) { DBG("macOS: libcurl-impersonate не встроена"); return false; }
static void *sym(const char *name) { (void)name; return NULL; }
static void unload(void) {}
#else
/* POSIX (Linux): .so запечена в ELF (build.sh: ld -r -b binary). Грузим без диска —
 * memfd_create + dlopen(/proc/self/fd/N). Файл нигде на диске не появляется. */
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>

extern const unsigned char _binary_libcurl_impersonate_so_start[];
extern const unsigned char _binary_libcurl_impersonate_so_end[];

static void *DL;
static int MEMFD = -1;

static bool extract_and_load(void) {
    const unsigned char *s = _binary_libcurl_impersonate_so_start;
    size_t n = (size_t)(_binary_libcurl_impersonate_so_end - _binary_libcurl_impersonate_so_start);
    if (!n) { DBG("blob пуст"); return false; }
    int fd = memfd_create("astra-ds", MFD_CLOEXEC);
    if (fd < 0) { DBG("memfd_create failed %d", errno); return false; }
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, s + off, n - off);
        if (w <= 0) { DBG("write memfd failed %d", errno); close(fd); return false; }
        off += (size_t)w;
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    DL = dlopen(path, RTLD_NOW | RTLD_LOCAL);   /* RTLD_LOCAL: символы curl не видны снаружи, не конфликтуют с нашим libcurl */
    if (!DL) { DBG("dlopen: %s", dlerror()); close(fd); return false; }
    MEMFD = fd;                                 /* держим до выгрузки */
    DBG("dlopen из memfd ок");
    return true;
}

static void *sym(const char *name) { return dlsym(DL, name); }

static void unload(void) {
    if (DL) { dlclose(DL); DL = NULL; }
    if (MEMFD >= 0) { close(MEMFD); MEMFD = -1; }
}
#endif

static bool resolve_all(void) {
    void *p;
#define R(field, sym_name) do { \
        p = sym(sym_name); \
        if (!p) { DBG("символ %s не найден", sym_name); return false; } \
        memcpy(&VT.field, &p, sizeof p); \
    } while (0)
    R(easy_init,          "curl_easy_init");
    R(easy_cleanup,       "curl_easy_cleanup");
    R(easy_setopt,        "curl_easy_setopt");
    R(easy_getinfo,       "curl_easy_getinfo");
    R(multi_init,         "curl_multi_init");
    R(multi_cleanup,      "curl_multi_cleanup");
    R(multi_add_handle,   "curl_multi_add_handle");
    R(multi_remove_handle,"curl_multi_remove_handle");
    R(multi_perform,      "curl_multi_perform");
    R(multi_info_read,    "curl_multi_info_read");
    R(slist_append,       "curl_slist_append");
    R(slist_free_all,     "curl_slist_free_all");
    R(easy_strerror,      "curl_easy_strerror");
    R(global_init,        "curl_global_init");
    R(global_cleanup,     "curl_global_cleanup");
#undef R
    return true;
}

const ImpCurl *impcurl_vt(void) {
    if (tried) return ready ? &VT : NULL;
    tried = true;
    if (!extract_and_load()) { unload(); return NULL; }
    if (!resolve_all())      { unload(); return NULL; }
    /* Инициализация отдельной копии libcurl (у неё своё глобальное состояние,
     * не мешает основному curl 8.23 в net.c). */
    if (VT.global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { DBG("global_init != OK"); unload(); return NULL; }
    DBG("global_init ок, вктабл готов");
    ready = true;
    static bool reg;                                 /* после выгрузки (login_finish) DLL может грузиться снова */
    if (!reg) { reg = true; atexit(impcurl_shutdown); }   /* распакованный файл удалить при выходе */
    return &VT;
}

void impcurl_shutdown(void) {
    if (ready && VT.global_cleanup) VT.global_cleanup();
    ready = tried = false;
    unload();
}

/* ---------- диагностика ---------- */
static size_t sink(char *p, size_t sz, size_t n, void *ud) {
    (void)p; (void)ud; return sz * n;
}

int impcurl_selftest(void) {
    impcurl_verbose = true;
    printf("impcurl: загрузка DLL из ресурса...\n");
    const ImpCurl *v = impcurl_vt();
    if (!v) {
        printf("impcurl: НЕ загрузился (impcurl_vt() == NULL) -> вход пойдёт обычным curl\n");
        return 1;
    }
    printf("impcurl: DLL загружена, функции найдены\n");
    {   /* IMPTEST_LOAD=1: только выгрузка и повторная загрузка (как после входа и при повторном входе), без сети */
        const char *lo = getenv("IMPTEST_LOAD");
        if (lo && !strcmp(lo, "1")) {
            impcurl_shutdown();
            const ImpCurl *v2 = impcurl_vt();
            bool again = v2 && v2->easy_init;
            impcurl_shutdown();
            printf("impcurl: выгрузка и повторная загрузка: %s\n", again ? "ок" : "НЕ ВЫШЛО");
            return again ? 0 : 1;
        }
    }
    CURL *e = v->easy_init();
    if (!e) { printf("impcurl: easy_init() == NULL\n"); return 1; }
    v->easy_setopt(e, CURLOPT_IMPERSONATE, "chrome152");
    v->easy_setopt(e, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    v->easy_setopt(e, CURLOPT_URL, "https://discord.com/api/v9/experiments?with_guild_experiments=false");
    v->easy_setopt(e, CURLOPT_WRITEFUNCTION, sink);
    v->easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    v->easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    v->easy_setopt(e, CURLOPT_TIMEOUT, 20L);
    v->easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    CURLM *m = v->multi_init();
    v->multi_add_handle(m, e);
    int running = 1;
    for (int i = 0; i < 2000 && running; i++) { v->multi_perform(m, &running); plat_sleep_ms(5); }
    long status = 0, httpver = 0;
    CURLcode rc = CURLE_OK;
    CURLMsg *msg; int left;
    while ((msg = v->multi_info_read(m, &left))) if (msg->msg == CURLMSG_DONE) rc = msg->data.result;
    v->easy_getinfo(e, CURLINFO_RESPONSE_CODE, &status);
    v->easy_getinfo(e, CURLINFO_HTTP_VERSION, &httpver);
    printf("impcurl: GET /experiments -> curl rc=%d (%s), HTTP %ld, статус %ld\n",
           rc, v->easy_strerror(rc), status, httpver);
    v->multi_remove_handle(m, e);
    v->easy_cleanup(e);
    v->multi_cleanup(m);
    if (status == 200) { printf("impcurl: OK — рукопожатие Chrome принято Discord\n"); return 0; }
    printf("impcurl: запрос не дал 200 (см. rc/статус выше)\n");
    return 1;
}
