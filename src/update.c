/* Автообновление (SECURITY.md «Автообновление», п. 1–14).
 * Проверка: GET api.github.com/repos/Flaim92/astra-ds/releases/latest (только описание для показа, решений по нему нет),
 * затем из служебного релиза UP_TAG («updates», не «последний»: в обычных релизах только архивы и суммы, с 0.2.6)
 * подписанный манифест manifest-<продукт>-<платформа>.json и его подпись .sig (ed25519, 64 Б) — проверка подписи
 * встроенным открытым ключом ДО использования полей; версия строго больше текущей; продукт и платформа совпадают.
 * Загрузка и замена — только после явного «Обновить» (update_apply): временный файл в каталоге exe (CREATE_NEW /
 * O_EXCL|O_NOFOLLOW, 0700), SHA-256 по ходу записи и повторно из того же дескриптора, затем замена (Windows —
 * exe -> exe.old и переименование открытого временного файла через его дескриптор; Linux — fchmod, fsync,
 * rename, fsync каталога). Сеть — только https, маршрут как у всего клиента (VLESS), хосты — белый список,
 * перенаправления вручную (<= 2), UA «Astra-DS», без идентификаторов. Дата последней проверки — в хранилище
 * секретов (не файлом). Ничего не пишется в журналы. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include "mc.h"
#include "plat.h"
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>
#else
#include <sys/random.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
extern char **environ;
#endif

void net_curl_common(CURL *e);
static bool up_random(void *p, size_t n) {     /* системный генератор (как в net.c: без DRBG OpenSSL) */
#ifdef _WIN32
    return BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    return getrandom(p, n, 0) == (ssize_t)n;
#endif
}
static void up_trunc(char *s, size_t max) {    /* по границе символа UTF-8 */
    size_t n = strlen(s);
    if (n <= max) return;
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    s[max] = 0;
}

#ifdef _WIN32
#define UP_PLATFORM "win-x64"
#else
#define UP_PLATFORM "linux-x64"
#endif
/* Два продукта выходят параллельно: обычный и «Astra DS Video» (-DASTRA_VIDEO). Каждый обновляется только своим
 * продуктом и своей платформой: файлы релиза manifest-<продукт>-<платформа>.json(.sig), поле product подписано. */
#ifdef ASTRA_VIDEO
#define UP_PRODUCT "astra-ds-video"
#else
#define UP_PRODUCT "astra-ds"
#endif
/* текст предупреждения «общий каталог» (безопасник: конкретно, с путём; без длинного тире — ASCII-оформление) */
#define UP_WARN_SHARED ui_tr("Каталог %s открыт на запись другим: они могут подменить программу и без обновления. " \
                             "Лучше перенести её в свою папку. Обновить всё равно?")
#define UP_JSON_MAX (1u << 20)
#define UP_MAN_MAX 4096
#define UP_EXE_MAX (64u << 20)
#define UP_TMP_PREFIX ".astra-upd-"

/* Открытые ключи подписи релизов (ed25519, сырые 32 Б). Второй — запасной (план смены ключа). Закрытые ключи —
 * вне репозитория и CI (E:\projects\astra-ds-signing\), подпись — tools/sign_release.sh. */
static const unsigned char UP_KEYS[2][32] = {
    { 0x04, 0xc4, 0x6a, 0xd8, 0xf2, 0x00, 0xe6, 0x82, 0x9b, 0x39, 0xa9, 0xf9, 0xd5, 0x1a, 0xdf, 0xe8,
      0x88, 0xc6, 0xe8, 0x4b, 0xb2, 0x37, 0x13, 0xd0, 0x2e, 0x5b, 0x82, 0xd4, 0xde, 0x13, 0xdd, 0x8a },
    { 0x2a, 0x0b, 0xb8, 0x39, 0x8e, 0x51, 0x6f, 0x42, 0x7a, 0x35, 0xee, 0x60, 0x14, 0xed, 0xdc, 0x69,
      0x4a, 0x00, 0xb3, 0x2d, 0xa8, 0x3e, 0xed, 0xe2, 0xc3, 0x60, 0x6a, 0x9a, 0x03, 0x4d, 0xad, 0x17 },
};
static unsigned char up_test_key[32];           /* selftest: третий ключ (только в selftest) */
static bool up_test_key_on;

/* ---------- состояние ---------- */
static UpdateState UST;                         /* под up_mu */
static PlatMutex *up_mu;
static PlatThread *up_thr;
static atomic_bool up_done;
static bool up_auto = true;
static char up_proxy[300];
#define UP_TAG "updates"                         /* служебный релиз с манифестами, подписями и файлами без архива */
static char up_tag[48], up_file[72], up_sha[65];
static long long up_size;

const UpdateState *update_state(void) {
    static UpdateState out;
    if (!up_mu) { memset(&out, 0, sizeof out); return &out; }
    mutex_lock(up_mu);
    out = UST;
    mutex_unlock(up_mu);
    return &out;
}
/* --demo-update: подставное состояние для снимков окна обновления (только --demo; сети, загрузки и перезапуска нет) */
bool update_demo(const char *phase) {
    static const char *const names[] = { "available", "downloading", "ready", "failed", "confirm" };
    static const int ph[] = { UP_AVAILABLE, UP_DOWNLOADING, UP_READY, UP_FAILED, UP_AVAILABLE };
    if (!O.demo || !phase) return false;
    for (int i = 0; i < 5; i++) if (!strcmp(phase, names[i])) {
        if (!up_mu) up_mu = mutex_new();
        mutex_lock(up_mu);
        memset(&UST, 0, sizeof UST);
        UST.phase = ph[i];
        snprintf(UST.version, sizeof UST.version, "9.9.0");
        snprintf(UST.notes, sizeof UST.notes, ui_tr("- Демонстрация экрана и камера (отдельная сборка Astra DS Video)\n"
                 "- Подавление шума микрофона: SpeexDSP или RNNoise\n- Исправлен треск при приёме голоса"));
        UST.size = 10639360;
        UST.got = ph[i] == UP_DOWNLOADING ? UST.size * 42 / 100 : ph[i] == UP_READY ? UST.size : 0;
        if (ph[i] == UP_FAILED) snprintf(UST.err, sizeof UST.err, ui_tr("хэш файла не совпал с подписанным"));
        UST.checked_at = wall_ms() / 1000;
        UST.shared_dir = i == 4;
        if (UST.shared_dir) snprintf(UST.warn, sizeof UST.warn, UP_WARN_SHARED, "E:\\Games\\Astra DS");
        mutex_unlock(up_mu);
        return true;
    }
    return false;
}
static void up_set(int phase, const char *err) {
    mutex_lock(up_mu);
    UST.phase = phase;
    if (err) snprintf(UST.err, sizeof UST.err, "%s", err);
    else if (phase != UP_FAILED) UST.err[0] = 0;
    mutex_unlock(up_mu);
    wake_signal(g_wake);
}
void update_auto_set(bool on) { up_auto = on; }
bool update_auto(void) { return up_auto && !O.no_update; }

/* ---------- чистые проверки (selftest) ---------- */
/* «x.y.z» -> число; -1 — не версия */
static long long up_ver(const char *s) {
    if (!s) return -1;
    long long v = 0;
    for (int part = 0; part < 3; part++) {
        if (*s < '0' || *s > '9') return -1;
        long n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); if (n > 99999) return -1; s++; }
        v = v * 100000 + n;
        if (part < 2) { if (*s != '.') return -1; s++; }
    }
    return *s ? -1 : v;
}
static bool up_name_ok(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > max) return false;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-')) return false;
    return s[0] != '.' && !strstr(s, "..");
}
/* хост URL по белому списку: api — только api.github.com; загрузка — github.com и два хоста объектов */
static bool up_url_ok(const char *u, bool api) {
    if (!u || strncmp(u, "https://", 8)) return false;
    const char *h = u + 8, *e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    size_t n = (size_t)(e - h);
    if (*e != '/') return false;
    for (const char *p = h; p < e; p++) if (*p == '@' || *p == ':' || *p == '\\') return false;   /* без логина и порта */
    static const char *const API[] = { "api.github.com" };
    static const char *const DL[] = { "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com" };
    const char *const *L = api ? API : DL;
    int nl = api ? 1 : 3;
    for (int i = 0; i < nl; i++) if (strlen(L[i]) == n && !memcmp(h, L[i], n)) return true;
    return false;
}
static bool up_sig_ok(const unsigned char *msg, size_t n, const unsigned char sig[64]) {
    int nk = up_test_key_on ? 3 : 2;
    for (int k = 0; k < nk; k++) {
        const unsigned char *key = k < 2 ? UP_KEYS[k] : up_test_key;
        EVP_PKEY *pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, key, 32);
        if (!pk) continue;
        EVP_MD_CTX *c = EVP_MD_CTX_new();
        bool ok = c && EVP_DigestVerifyInit(c, NULL, NULL, NULL, pk) == 1 && EVP_DigestVerify(c, sig, 64, msg, n) == 1;
        EVP_MD_CTX_free(c);
        EVP_PKEY_free(pk);
        if (ok) return true;
    }
    return false;
}
/* подписанный манифест -> поля; false — отказ (err). Поля проверяются только после подписи. */
static bool up_manifest(const char *man, size_t n, const unsigned char sig[64], const char *cur, char *ver, size_t vn,
                        char *file, size_t fn, long long *size, char *sha, char *err, size_t errn) {
    if (n == 0 || n > UP_MAN_MAX) { snprintf(err, errn, ui_tr("манифест: неверный размер")); return false; }
    if (!up_sig_ok((const unsigned char *)man, n, sig)) { snprintf(err, errn, ui_tr("подпись обновления не прошла проверку")); return false; }
    cJSON *j = json_parse(man, n);
    bool ok = false;
    const char *pr = jstr(j, "product"), *pl = jstr(j, "platform"), *v = jstr(j, "version"), *f = jstr(j, "file"), *h = jstr(j, "sha256");
    const cJSON *sz = jget(j, "size");
    if (!pr || strcmp(pr, UP_PRODUCT)) snprintf(err, errn, ui_tr("манифест для другого продукта"));
    else if (!pl || strcmp(pl, UP_PLATFORM)) snprintf(err, errn, ui_tr("обновление для другой платформы"));
    else if (up_ver(v) < 0) snprintf(err, errn, ui_tr("неверная версия в манифесте"));
    else if (up_ver(v) <= up_ver(cur)) snprintf(err, errn, ui_tr("установлена та же или более новая версия"));
    else if (!up_name_ok(f, 64)) snprintf(err, errn, ui_tr("неверное имя файла в манифесте"));
    else if (!cJSON_IsNumber(sz) || sz->valuedouble < 1 || sz->valuedouble > UP_EXE_MAX) snprintf(err, errn, ui_tr("неверный размер в манифесте"));
    else if (!h || strlen(h) != 64 || strspn(h, "0123456789abcdef") != 64) snprintf(err, errn, ui_tr("неверный хэш в манифесте"));
    else {
        snprintf(ver, vn, "%s", v);
        snprintf(file, fn, "%s", f);
        snprintf(sha, 65, "%s", h);
        *size = (long long)sz->valuedouble;
        ok = true;
    }
    json_done();
    return ok;
}

/* ---------- сеть: только https, вручную по перенаправлениям, белый список хостов ---------- */
typedef struct { Buf *mem; size_t max; bool over; EVP_MD_CTX *sha;
#ifdef _WIN32
    HANDLE fh;
#else
    int fd;
#endif
    long long got, cap; } Sink;
static size_t up_write(char *p, size_t sz, size_t nm, void *u) {
    Sink *s = u;
    size_t n = sz * nm;
    if (s->mem) {
        if (s->mem->len + n > s->max) { s->over = true; return 0; }
        buf_put(s->mem, p, n);
        return n;
    }
    if (s->got + (long long)n > s->cap) { s->over = true; return 0; }   /* больше подписанного размера — отказ */
#ifdef _WIN32
    DWORD w = 0;
    if (!WriteFile(s->fh, p, (DWORD)n, &w, NULL) || w != n) return 0;
#else
    size_t off = 0;
    while (off < n) { ssize_t w = write(s->fd, p + off, n - off); if (w <= 0) { if (errno == EINTR) continue; return 0; } off += (size_t)w; }
#endif
    EVP_DigestUpdate(s->sha, p, n);
    s->got += (long long)n;
    if (up_mu) { mutex_lock(up_mu); UST.got = s->got; mutex_unlock(up_mu); }
    return n;
}
/* АО-2: тело ответа-перенаправления (3xx) не должно попасть ни в манифест, ни в exe — приёмник к нулю */
static bool up_sink_reset(Sink *s) {
    s->over = false;
    if (s->mem) { s->mem->len = 0; return true; }
    s->got = 0;
    if (s->sha) EVP_DigestInit_ex(s->sha, EVP_sha256(), NULL);
#ifdef _WIN32
    LARGE_INTEGER z = {0};
    return SetFilePointerEx(s->fh, z, NULL, FILE_BEGIN) && SetEndOfFile(s->fh);
#else
    return ftruncate(s->fd, 0) == 0 && lseek(s->fd, 0, SEEK_SET) == 0;
#endif
}
/* GET url (api — белый список метаданных) в sink; возврат — HTTP-статус, 0 — сеть/отказ (err) */
static long up_get(const char *url0, bool api, Sink *sk, char *err, size_t errn) {
    char url[1024];
    snprintf(url, sizeof url, "%s", url0);
    for (int hop = 0; hop < 3; hop++) {
        if (!up_url_ok(url, api)) { snprintf(err, errn, ui_tr("адрес обновления вне разрешённых хостов")); return 0; }
        CURL *e = curl_easy_init();
        if (!e) { snprintf(err, errn, ui_tr("нет памяти")); return 0; }
        net_curl_common(e);
        curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(e, CURLOPT_USERAGENT, "Astra-DS");
        curl_easy_setopt(e, CURLOPT_PROXY, up_proxy);
        curl_easy_setopt(e, CURLOPT_NOPROXY, "");
        if (up_proxy[0]) curl_easy_setopt(e, CURLOPT_SOCKS5_AUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(e, CURLOPT_URL, url);
        curl_easy_setopt(e, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(e, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(sk->mem ? sk->max : (size_t)sk->cap));
        static struct curl_slist acc = { (char *)"Accept: application/vnd.github+json", NULL };
        if (api) curl_easy_setopt(e, CURLOPT_HTTPHEADER, &acc);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, up_write);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, sk);
        CURLcode rc = curl_easy_perform(e);
        long st = 0;
        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &st);
        char next[1024] = "";
        if (st >= 300 && st < 400) {
            char *loc = NULL;
            curl_easy_getinfo(e, CURLINFO_REDIRECT_URL, &loc);
            if (loc && strlen(loc) < sizeof next) snprintf(next, sizeof next, "%s", loc);
        }
        curl_easy_cleanup(e);
        if (sk->over) { snprintf(err, errn, ui_tr("ответ больше разрешённого размера")); return 0; }
        if (st >= 300 && st < 400) {
            if (!next[0]) { snprintf(err, errn, ui_tr("перенаправление без адреса")); return 0; }
            if (!up_url_ok(next, false)) { snprintf(err, errn, ui_tr("перенаправление на чужой хост — отказ")); return 0; }
            if (!up_sink_reset(sk)) { snprintf(err, errn, ui_tr("временный файл не очищен")); return 0; }
            api = false;                        /* дальше — хосты загрузки */
            snprintf(url, sizeof url, "%s", next);
            continue;
        }
        if (rc != CURLE_OK && !st) { snprintf(err, errn, ui_tr("нет связи с GitHub")); return 0; }
        return st;
    }
    snprintf(err, errn, ui_tr("слишком много перенаправлений"));
    return 0;
}

/* ---------- проверка ---------- */
static int up_check_thread(void *arg) {
    (void)arg;
    char err[160] = "";
    Buf js = {0};
    Sink sk = { .mem = &js, .max = UP_JSON_MAX };
    long st = up_get("https://api.github.com/repos/Flaim92/astra-ds/releases/latest", true, &sk, err, sizeof err);
    if (st != 200) { if (!err[0]) snprintf(err, sizeof err, ui_tr("GitHub ответил %ld"), st); buf_free(&js); up_set(UP_FAILED, err); goto done; }
    {
        cJSON *j = json_parse(js.p ? js.p : "", js.len);
        const char *tag = jstr(j, "tag_name"), *body = jstr(j, "body");
        bool tag_ok = tag && up_name_ok(tag, 40);
        if (tag_ok) snprintf(up_tag, sizeof up_tag, "%s", tag);
        char *notes = san(body ? body : "", true);   /* только для показа */
        up_trunc(notes, 4096);
        mutex_lock(up_mu);
        snprintf(UST.notes, sizeof UST.notes, "%s", notes);
        mutex_unlock(up_mu);
        free(notes);
        json_done();
        buf_free(&js);
        if (!tag_ok) { up_set(UP_FAILED, ui_tr("неверный ответ GitHub")); goto done; }
        snprintf(up_tag, sizeof up_tag, "%s", UP_TAG);   /* файлы обновления — только из служебного релиза */
    }
    {
        char url[256];
        Buf man = {0}, sig = {0};
        Sink s1 = { .mem = &man, .max = UP_MAN_MAX }, s2 = { .mem = &sig, .max = 64 };
        snprintf(url, sizeof url, "https://github.com/Flaim92/astra-ds/releases/download/%s/manifest-" UP_PRODUCT "-" UP_PLATFORM ".json", up_tag);
        long a = up_get(url, false, &s1, err, sizeof err);
        snprintf(url, sizeof url, "https://github.com/Flaim92/astra-ds/releases/download/%s/manifest-" UP_PRODUCT "-" UP_PLATFORM ".json.sig", up_tag);
        long b = a == 200 ? up_get(url, false, &s2, err, sizeof err) : 0;
        if (a == 404) { buf_free(&man); buf_free(&sig); up_set(UP_NONE, NULL); goto done; }   /* в релизе нет сборки под платформу */
        if (a != 200 || b != 200 || sig.len != 64) {
            if (!err[0]) snprintf(err, sizeof err, ui_tr("манифест обновления не загрузился"));
            buf_free(&man); buf_free(&sig);
            up_set(UP_FAILED, err);
            goto done;
        }
        char ver[32];
        bool ok = up_manifest(man.p, man.len, (const unsigned char *)sig.p, ASTRA_VERSION, ver, sizeof ver, up_file, sizeof up_file,
                              &up_size, up_sha, err, sizeof err);
        buf_free(&man); buf_free(&sig);
        if (!ok) {
            if (strstr(err, "та же или более новая")) up_set(UP_NONE, NULL);
            else up_set(UP_FAILED, err);
            goto done;
        }
        mutex_lock(up_mu);
        snprintf(UST.version, sizeof UST.version, "%s", ver);
        UST.size = up_size;
        mutex_unlock(up_mu);
        up_set(UP_AVAILABLE, NULL);
    }
done:
    atomic_store(&up_done, true);
    wake_signal(g_wake);
    return 0;
}

/* ---------- установка ---------- */
#ifdef _WIN32
static bool up_exe_path(WCHAR *p, DWORD n) { DWORD k = GetModuleFileNameW(NULL, p, n); return k > 0 && k < n; }
/* АО-3: в DACL право записи (данные, добавление, удаление, смена прав и владельца) — только у нас, SYSTEM и
 * Администраторов. NULL-DACL, нечитаемый DACL, чужой разрешающий ACE с записью или ACE незнакомого вида — отказ.
 * Запрещающие ACE проверку не ослабляют (пропускаются); наследуемые-только (INHERIT_ONLY) к самому объекту не относятся. */
static bool up_acl_ok(const WCHAR *path) {
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS) return false;
    bool ok = dacl != NULL;
    BYTE ub[256], sys[SECURITY_MAX_SID_SIZE], adm[SECURITY_MAX_SID_SIZE];
    DWORD un = 0, sn = sizeof sys, an = sizeof adm;
    PSID me = NULL;
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenUser, ub, sizeof ub, &un)) me = ((TOKEN_USER *)ub)->User.Sid;
        CloseHandle(tok);
    }
    if (!me || !CreateWellKnownSid(WinLocalSystemSid, NULL, sys, &sn) || !CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, adm, &an)) ok = false;
    const DWORD W = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD i = 0; ok && i < dacl->AceCount; i++) {
        ACE_HEADER *ah = NULL;
        if (!GetAce(dacl, i, (void **)&ah)) { ok = false; break; }
        if (ah->AceFlags & INHERIT_ONLY_ACE) continue;
        if (ah->AceType == ACCESS_DENIED_ACE_TYPE || ah->AceType == ACCESS_DENIED_OBJECT_ACE_TYPE || ah->AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE) continue;
        if (ah->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
        ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)ah;
        if (!(a->Mask & W)) continue;
        PSID s = (PSID)&a->SidStart;
        if (!EqualSid(s, me) && !EqualSid(s, (PSID)sys) && !EqualSid(s, (PSID)adm)) ok = false;
    }
    LocalFree(sd);
    return ok;
}
static bool up_long(const WCHAR *in, WCHAR *out, DWORD n) { DWORD k = GetLongPathNameW(in, out, n); return k && k < n; }
/* Где лежит exe (п. 8, АО-3). UP_PLACE_SYS — системный или общий каталог (%PUBLIC%, %TEMP%, ProgramData, Program
 * Files, Windows): не обновляем никогда. UP_PLACE_SHARED — DACL каталога или exe даёт запись не только нам/SYSTEM/
 * Администраторам (обычно для не-системных дисков): обновление — только после «обновить всё равно?» (решение
 * координатора: отказ тут не защищает — кто пишет в каталог, подменит exe и без нас). Пути — полные (8.3 иначе
 * обходит сравнение префиксов). */
enum { UP_PLACE_OK, UP_PLACE_SHARED, UP_PLACE_SYS };
static int up_dir_check(const WCHAR *exe0) {
    WCHAR exe[MAX_PATH], dir[MAX_PATH];
    if (!up_long(exe0, exe, MAX_PATH)) return UP_PLACE_SYS;
    static const WCHAR *const env[] = { L"PUBLIC", L"TEMP", L"TMP", L"ProgramData", L"ProgramFiles", L"ProgramFiles(x86)", L"SystemRoot" };
    for (size_t i = 0; i < sizeof env / sizeof *env; i++) {
        WCHAR v[MAX_PATH], lv[MAX_PATH];
        DWORD k = GetEnvironmentVariableW(env[i], v, MAX_PATH);
        if (!k || k >= MAX_PATH) continue;
        const WCHAR *c = up_long(v, lv, MAX_PATH) ? lv : v;
        size_t cn = wcslen(c);
        while (cn > 3 && c[cn - 1] == L'\\') cn--;
        if (!_wcsnicmp(exe, c, cn) && (exe[cn] == L'\\' || !exe[cn])) return UP_PLACE_SYS;
    }
    WCHAR *bs = wcsrchr(exe, L'\\');
    if (!bs) return UP_PLACE_SYS;
    size_t dn = (size_t)(bs - exe) + (bs - exe == 2 ? 1 : 0);   /* "E:\\" — корень диска с косой чертой */
    memcpy(dir, exe, dn * sizeof(WCHAR));
    dir[dn] = 0;
    return up_acl_ok(dir) && up_acl_ok(exe) ? UP_PLACE_OK : UP_PLACE_SHARED;
}
#else
enum { UP_PLACE_OK, UP_PLACE_SHARED, UP_PLACE_SYS };
/* Linux: каталог или exe не наш (например, /usr/bin у root) — не обновляем; наш, но открыт на запись группе или
 * всем — только после подтверждения */
static int up_dir_check_posix(const char *exe, const char *dir) {
    struct stat ds, es;
    if (stat(dir, &ds) || stat(exe, &es) || ds.st_uid != getuid() || es.st_uid != getuid()) return UP_PLACE_SYS;
    if (ds.st_mode & S_ISVTX) return UP_PLACE_SYS;   /* sticky (как /tmp) — общий каталог: не обновляем */
    if ((ds.st_mode & (S_IWGRP | S_IWOTH)) || (es.st_mode & (S_IWGRP | S_IWOTH))) return UP_PLACE_SHARED;
    return UP_PLACE_OK;
}
#endif
static bool up_force;                           /* «обновить всё равно» подтверждено для этой загрузки */
/* место exe этой программы (для update_apply в главном потоке); dir — каталог программы (UTF-8) */
static int up_place(char *dirout, size_t dn) {
    dirout[0] = 0;
#ifdef _WIN32
    WCHAR exe[MAX_PATH], lx[MAX_PATH];
    if (!up_exe_path(exe, MAX_PATH)) return UP_PLACE_SYS;
    const WCHAR *ex = up_long(exe, lx, MAX_PATH) ? lx : exe;
    const WCHAR *bs = wcsrchr(ex, L'\\');
    if (bs) {
        int k = WideCharToMultiByte(CP_UTF8, 0, ex, (int)(bs - ex), dirout, (int)dn - 1, NULL, NULL);
        dirout[k > 0 ? k : 0] = 0;
    }
    return up_dir_check(exe);
#else
    char exe[4096], dir[4096];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return UP_PLACE_SYS;
    exe[k] = 0;
    char *sl = strrchr(exe, '/');
    if (!sl) return UP_PLACE_SYS;
    snprintf(dir, sizeof dir, "%.*s", (int)(sl - exe) ? (int)(sl - exe) : 1, exe);
    size_t l = strlen(dir);
    if (l >= dn) l = dn - 1;                    /* длинный путь — только для показа, обрезать */
    memcpy(dirout, dir, l);
    dirout[l] = 0;
    return up_dir_check_posix(exe, dir);
#endif
}
/* Скачать в новый временный файл каталога exe, проверить размер и SHA-256 (по ходу и повторно из того же
 * дескриптора), заменить exe. Возврат: true — заменено; err — почему нет. (Используется и selftest с exe_override.) */
static const char *up_exe_override;              /* selftest: «exe» — тестовый файл, загрузка — из src_override */
static const char *up_src_override;
static bool up_install(char *err, size_t errn) {
    unsigned char rnd[8];
    if (!up_random(rnd, sizeof rnd)) { snprintf(err, errn, ui_tr("нет случайных чисел")); return false; }
    char rh[17];
    for (int i = 0; i < 8; i++) snprintf(rh + i * 2, 3, "%02x", rnd[i]);
    unsigned char md[32];
    unsigned int mdn = 0;
    EVP_MD_CTX *sha = EVP_MD_CTX_new();
    if (!sha) { snprintf(err, errn, ui_tr("нет памяти")); return false; }
    EVP_DigestInit_ex(sha, EVP_sha256(), NULL);
    Sink sk = { .sha = sha, .cap = up_size };
    bool ok = false;
#ifdef _WIN32
    WCHAR exe[MAX_PATH], tmp[MAX_PATH + 40], old[MAX_PATH + 8];
    if (up_exe_override) MultiByteToWideChar(CP_UTF8, 0, up_exe_override, -1, exe, MAX_PATH);
    else if (!up_exe_path(exe, MAX_PATH)) { snprintf(err, errn, ui_tr("путь программы не определён")); EVP_MD_CTX_free(sha); return false; }
    int pc = up_exe_override ? UP_PLACE_OK : up_dir_check(exe);
    if (pc == UP_PLACE_SYS) { snprintf(err, errn, ui_tr("программа в системном или общем каталоге — обновите вручную")); EVP_MD_CTX_free(sha); return false; }
    if (pc == UP_PLACE_SHARED && !up_force) { snprintf(err, errn, ui_tr("каталог программы доступен на запись другим — нужно подтверждение")); EVP_MD_CTX_free(sha); return false; }
    WCHAR *bs = wcsrchr(exe, L'\\');
    if (!bs) { snprintf(err, errn, ui_tr("путь программы не определён")); EVP_MD_CTX_free(sha); return false; }
    swprintf(tmp, MAX_PATH + 40, L"%.*ls\\" L".astra-upd-%hs.tmp", (int)(bs - exe), exe, rh);
    swprintf(old, MAX_PATH + 8, L"%ls.old", exe);
    HANDLE h = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { snprintf(err, errn, ui_tr("временный файл не создан")); EVP_MD_CTX_free(sha); return false; }
    sk.fh = h;
#else
    char exe[4096], tmpn[64], old[4200];
    if (up_exe_override) snprintf(exe, sizeof exe, "%s", up_exe_override);
    else { ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1); if (k <= 0) { snprintf(err, errn, ui_tr("путь программы не определён")); EVP_MD_CTX_free(sha); return false; } exe[k] = 0; }
    char *sl = strrchr(exe, '/');
    if (!sl) { snprintf(err, errn, ui_tr("путь программы не определён")); EVP_MD_CTX_free(sha); return false; }
    char dir[4096];
    snprintf(dir, sizeof dir, "%.*s", (int)(sl - exe) ? (int)(sl - exe) : 1, exe);
    int pc = up_dir_check_posix(exe, dir);
    if (pc == UP_PLACE_SYS) { snprintf(err, errn, ui_tr("каталог программы не ваш или общий (sticky) — обновите вручную")); EVP_MD_CTX_free(sha); return false; }
    if (pc == UP_PLACE_SHARED && !up_force) { snprintf(err, errn, ui_tr("каталог программы доступен на запись другим — нужно подтверждение")); EVP_MD_CTX_free(sha); return false; }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    snprintf(tmpn, sizeof tmpn, UP_TMP_PREFIX "%s.tmp", rh);
    snprintf(old, sizeof old, "%s.old", exe);
    int fd = dfd >= 0 ? openat(dfd, tmpn, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700) : -1;
    if (fd < 0) { if (dfd >= 0) close(dfd); snprintf(err, errn, ui_tr("временный файл не создан")); EVP_MD_CTX_free(sha); return false; }
    sk.fd = fd;
#endif
    /* загрузка (или для selftest — копия из файла) */
    if (up_src_override) {
        FILE *f = fopen(up_src_override, "rb");
        char b[16384];
        size_t r;
        bool w = f != NULL;
        while (f && w && (r = fread(b, 1, sizeof b, f)) > 0) w = up_write(b, 1, r, &sk) == r;
        if (f) fclose(f);
        if (!w) snprintf(err, errn, sk.over ? ui_tr("файл больше подписанного размера") : ui_tr("не записан"));
        else ok = true;
    } else {
        char url[256];
        snprintf(url, sizeof url, "https://github.com/Flaim92/astra-ds/releases/download/%s/%s", up_tag, up_file);
        long st = up_get(url, false, &sk, err, errn);
        ok = st == 200;
        if (!ok && !err[0]) snprintf(err, errn, ui_tr("загрузка: GitHub ответил %ld"), st);
    }
    char hex[65] = "";
    if (ok) {
        EVP_DigestFinal_ex(sha, md, &mdn);
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", md[i]);
        if (sk.got != up_size) { snprintf(err, errn, ui_tr("размер не совпал с подписанным")); ok = false; }
        else if (strcmp(hex, up_sha)) { snprintf(err, errn, ui_tr("хэш файла не совпал с подписанным")); ok = false; }
    }
    if (ok) {                                   /* повторно: читаем из того же дескриптора (никто не подменил) */
        EVP_DigestInit_ex(sha, EVP_sha256(), NULL);
        char b[16384];
        long long tot = 0;
#ifdef _WIN32
        LARGE_INTEGER z = {0};
        FlushFileBuffers(h);
        SetFilePointerEx(h, z, NULL, FILE_BEGIN);
        DWORD r;
        while (ReadFile(h, b, sizeof b, &r, NULL) && r > 0) { EVP_DigestUpdate(sha, b, r); tot += r; }
#else
        fsync(fd);
        ssize_t r;
        off_t off = 0;
        while ((r = pread(fd, b, sizeof b, off)) > 0) { EVP_DigestUpdate(sha, b, (size_t)r); tot += r; off += r; }
#endif
        EVP_DigestFinal_ex(sha, md, &mdn);
        char hex2[65];
        for (int i = 0; i < 32; i++) snprintf(hex2 + i * 2, 3, "%02x", md[i]);
        if (tot != up_size || strcmp(hex2, up_sha)) { snprintf(err, errn, ui_tr("файл изменился после загрузки — отказ")); ok = false; }
    }
    EVP_MD_CTX_free(sha);
#ifdef _WIN32
    if (ok) {                                   /* замена: exe -> .old, затем открытый временный -> exe по дескриптору */
        if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) { snprintf(err, errn, ui_tr("программа занята — не заменить")); ok = false; }
        else {
            size_t nl = wcslen(exe);
            size_t sz = sizeof(FILE_RENAME_INFO) + (nl + 1) * sizeof(WCHAR);
            FILE_RENAME_INFO *ri = xcalloc(1, sz);
            ri->ReplaceIfExists = TRUE;
            ri->FileNameLength = (DWORD)(nl * sizeof(WCHAR));
            memcpy(ri->FileName, exe, (nl + 1) * sizeof(WCHAR));
            if (!SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD)sz)) {
                MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);   /* вернуть как было */
                snprintf(err, errn, ui_tr("новая версия не встала на место — оставлена прежняя"));
                ok = false;
            }
            free(ri);
        }
    }
    if (!ok) {                                  /* удалить временный (по дескриптору) */
        FILE_DISPOSITION_INFO di = { TRUE };
        SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di);
    }
    CloseHandle(h);
#else
    if (ok) {
        if (fchmod(fd, 0755) || fsync(fd) || renameat(dfd, tmpn, dfd, sl + 1)) { snprintf(err, errn, ui_tr("новая версия не встала на место")); ok = false; }
        else fsync(dfd);
    }
    if (!ok) unlinkat(dfd, tmpn, 0);
    close(fd);
    close(dfd);
    (void)old;
#endif
    return ok;
}
static int up_apply_thread(void *arg) {
    (void)arg;
    char err[160] = "";
    if (up_install(err, sizeof err)) up_set(UP_READY, NULL);
    else up_set(UP_FAILED, err);
    up_force = false;                           /* подтверждение — на одну загрузку, не запоминается */
    atomic_store(&up_done, true);
    wake_signal(g_wake);
    return 0;
}

/* ---------- внешнее API ---------- */
static bool up_start(int (*fn)(void *), int phase) {
    if (up_thr) return false;
    const char *p = vless_state() == VL_OFF ? "" : vless_proxy_url();
    if (!p) { up_set(UP_FAILED, ui_tr("VLESS включён, но не работает — проверка отложена")); return false; }
    snprintf(up_proxy, sizeof up_proxy, "%s", p);
    atomic_store(&up_done, false);
    up_set(phase, NULL);
    up_thr = thread_start(fn, NULL);
    if (!up_thr) { wipe(up_proxy, sizeof up_proxy); up_set(UP_FAILED, ui_tr("не удалось запустить проверку")); return false; }
    return true;
}
void update_check(bool manual) {
    if (!up_mu) up_mu = mutex_new();
    if (O.demo || (!manual && !update_auto())) return;
    if (!manual) {                              /* не чаще раза в сутки: дата — в хранилище секретов, не файлом */
        char b[32] = "";
        int n = keyring_load("update-checked", b, sizeof b - 1);
        if (n > 0) { b[n] = 0; i64 t = strtoll(b, NULL, 10); if (t > 0 && wall_ms() / 1000 - t < 86400) return; }
    }
    if (up_start(up_check_thread, UP_CHECKING)) {
        mutex_lock(up_mu);
        UST.checked_at = wall_ms() / 1000;
        mutex_unlock(up_mu);
    }
}
static void up_apply_go(bool force) {
    if (!up_mu || update_state()->phase != UP_AVAILABLE || O.demo) return;
    char dir[300];
    int pc = up_place(dir, sizeof dir);
    char *sd = san(dir, false);                 /* путь — в интерфейс: без управляющих последовательностей */
    snprintf(dir, sizeof dir, "%s", sd);
    free(sd);
    mutex_lock(up_mu);
    UST.got = 0;
    UST.shared_dir = pc == UP_PLACE_SHARED && !force;
    if (UST.shared_dir) snprintf(UST.warn, sizeof UST.warn, UP_WARN_SHARED, dir);
    else UST.warn[0] = 0;
    mutex_unlock(up_mu);
    if (pc == UP_PLACE_SYS) { up_set(UP_FAILED, ui_tr("программа в системном или общем каталоге — обновите вручную")); return; }
    if (pc == UP_PLACE_SHARED && !force) { ui_dirty(); return; }   /* окно обновления спросит «обновить всё равно?» */
    up_force = force;
    up_start(up_apply_thread, UP_DOWNLOADING);
}
void update_apply(void) { up_apply_go(false); }
void update_apply_anyway(void) { if (update_state()->shared_dir) up_apply_go(true); }
void update_apply_cancel(void) {
    if (!up_mu) return;
    mutex_lock(up_mu);
    UST.shared_dir = false;
    UST.warn[0] = 0;
    mutex_unlock(up_mu);
    ui_dirty();
}
/* главный цикл: закончившийся поток — забрать; дату проверки — в хранилище */
void update_service(void) {
    if (!up_thr || !atomic_load(&up_done)) return;
    thread_join(up_thr);
    up_thr = NULL;
    wipe(up_proxy, sizeof up_proxy);            /* ревью 18 М4: там логин и пароль SOCKS туннеля */
    UpdateState s = *update_state();
    if (s.phase == UP_AVAILABLE || s.phase == UP_NONE) {
        char b[32];
        int n = snprintf(b, sizeof b, "%lld", (long long)s.checked_at);
        keyring_save("update-checked", b, (size_t)n);
    }
    if (s.phase == UP_AVAILABLE) ui_notice(ui_tr("доступна новая версия Astra DS %s — обновить можно в настройках"), s.version);
    if (s.phase == UP_READY) ui_notice(ui_tr("Astra DS обновлена до %s — перезапустите"), s.version);
    ui_dirty();
}
bool update_restart(void) {
    if (O.demo || update_state()->phase != UP_READY) return false;
#ifdef _WIN32
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 4];
    if (!up_exe_path(exe, MAX_PATH)) return false;
    swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);   /* без аргументов: секретов в командной строке нет */
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    single_instance_release();                  /* АО-4: иначе новый процесс увидит нас и сразу выйдет */
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { single_instance(); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    char exe[4096];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return false;
    exe[k] = 0;
    char *argv[] = { exe, NULL };
    pid_t pid;
    single_instance_release();                  /* АО-4: освободить имя единственного экземпляра до запуска */
    if (posix_spawn(&pid, exe, NULL, NULL, argv, environ) != 0) { single_instance(); return false; }
#endif
    g_quit = true;
    return true;
}
/* при старте: убрать .old и брошенные временные файлы (только наши имена в каталоге exe, не ссылки) */
void update_init(void) {
    if (!up_mu) up_mu = mutex_new();
#ifdef _WIN32
    WCHAR exe[MAX_PATH], p[MAX_PATH + 8];
    if (!up_exe_path(exe, MAX_PATH)) return;
    swprintf(p, MAX_PATH + 8, L"%ls.old", exe);
    DWORD a = GetFileAttributesW(p);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) DeleteFileW(p);
    WCHAR *bs = wcsrchr(exe, L'\\');
    if (!bs) return;
    WCHAR pat[MAX_PATH + 32];
    swprintf(pat, MAX_PATH + 32, L"%.*ls\\.astra-upd-*.tmp", (int)(bs - exe), exe);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) continue;
            swprintf(p, MAX_PATH + 8, L"%.*ls\\%ls", (int)(bs - exe), exe, fd.cFileName);
            DeleteFileW(p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
#else
    char exe[4096];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return;
    exe[k] = 0;
    char p[4096 + 256 + 8];                     /* каталог exe + «/» + имя из readdir (до 255) */
    struct stat st;
    snprintf(p, sizeof p, "%s.old", exe);
    if (!lstat(p, &st) && S_ISREG(st.st_mode)) unlink(p);
    char *sl = strrchr(exe, '/');
    if (!sl) return;
    *sl = 0;
    DIR *d = opendir(exe[0] ? exe : "/");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (strncmp(de->d_name, UP_TMP_PREFIX, strlen(UP_TMP_PREFIX)) || n < 5 || strcmp(de->d_name + n - 4, ".tmp")) continue;
        snprintf(p, sizeof p, "%s/%s", exe, de->d_name);
        if (!lstat(p, &st) && S_ISREG(st.st_mode)) unlink(p);
    }
    closedir(d);
#endif
}

/* ---------- selftest (п. 14) ---------- */
int update_selftest(void (*check)(bool ok, const char *what)) {
    if (!up_mu) up_mu = mutex_new();
    /* тестовый ключ — только здесь; закрытого ключа релизов в коде нет */
    EVP_PKEY *tk = NULL;
    EVP_PKEY_CTX *kc = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!kc || EVP_PKEY_keygen_init(kc) != 1 || EVP_PKEY_keygen(kc, &tk) != 1) { check(false, "обновление: тестовый ключ не создан"); EVP_PKEY_CTX_free(kc); return 1; }
    EVP_PKEY_CTX_free(kc);
    size_t pl = 32;
    EVP_PKEY_get_raw_public_key(tk, up_test_key, &pl);
    unsigned char sig[64];
    #define SIGN(msg) do { EVP_MD_CTX *c = EVP_MD_CTX_new(); size_t sl2 = 64; EVP_DigestSignInit(c, NULL, NULL, NULL, tk); \
        EVP_DigestSign(c, sig, &sl2, (const unsigned char *)(msg), strlen(msg)); EVP_MD_CTX_free(c); } while (0)
    char v[32], f[72], h[65], err[160];
    long long sz;
    const char *good = "{\"product\":\"" UP_PRODUCT "\",\"platform\":\"" UP_PLATFORM "\",\"version\":\"9.0.0\",\"file\":\"astra-ds.bin\",\"size\":5,"
                       "\"sha256\":\"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\"}";
    SIGN(good);
    up_test_key_on = false;                     /* ключ не наш (как «чужой ключ»): отказ */
    bool t1 = !up_manifest(good, strlen(good), sig, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err) && strstr(err, "подпись");
    up_test_key_on = true;
    bool t2 = up_manifest(good, strlen(good), sig, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err) && !strcmp(v, "9.0.0") && sz == 5;
    char bad[400];
    memcpy(bad, good, strlen(good) + 1);
    bad[60] ^= 1;                               /* изменён байт после подписи */
    bool t3 = !up_manifest(bad, strlen(bad), sig, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err);
    char m[400];
    snprintf(m, sizeof m, "{\"product\":\"" UP_PRODUCT "\",\"platform\":\"" UP_PLATFORM "\",\"version\":\"%s\",\"file\":\"a.bin\",\"size\":5,\"sha256\":\"%064d\"}", ASTRA_VERSION, 0);
    SIGN(m);
    bool t4 = !up_manifest(m, strlen(m), sig, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err) && strstr(err, "более новая");
    snprintf(m, sizeof m, "{\"product\":\"" UP_PRODUCT "\",\"platform\":\"%s\",\"version\":\"9.0.0\",\"file\":\"a.bin\",\"size\":5,\"sha256\":\"%064d\"}",
             strcmp(UP_PLATFORM, "win-x64") ? "win-x64" : "linux-x64", 0);
    SIGN(m);
    bool t5 = !up_manifest(m, strlen(m), sig, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err) && strstr(err, "платформ");
    snprintf(m, sizeof m, "{\"product\":\"" UP_PRODUCT "\",\"platform\":\"" UP_PLATFORM "\",\"version\":\"9.0.0\",\"file\":\"../evil\",\"size\":5,\"sha256\":\"%064d\"}", 0);
    SIGN(m);
    bool t6 = !up_manifest(m, strlen(m), sig, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err);
    bool t7 = up_url_ok("https://objects.githubusercontent.com/x", false) && !up_url_ok("https://evil.example/x", false) &&
              !up_url_ok("https://github.com.evil.example/x", false) && !up_url_ok("http://github.com/x", false) &&
              !up_url_ok("https://user@github.com/x", false) && !up_url_ok("https://github.com:444/x", false) &&
              !up_url_ok("https://github.com/x", true) && up_url_ok("https://api.github.com/repos/x", true);
    bool t8 = up_ver("0.1.10") > up_ver("0.1.9") && up_ver("1.0.0") > up_ver("0.99.99") && up_ver("1.0") < 0 && up_ver("1.0.0x") < 0;
    check(t1 && t2 && t3, "обновление: подпись чужим ключом и изменённый манифест — отказ, верная — принята");
    /* другой продукт (обычный <-> Video) тем же ключом — отказ */
    snprintf(m, sizeof m, "{\"product\":\"%s\",\"platform\":\"" UP_PLATFORM "\",\"version\":\"9.0.0\",\"file\":\"a.bin\",\"size\":5,\"sha256\":\"%064d\"}",
             strcmp(UP_PRODUCT, "astra-ds") ? "astra-ds" : "astra-ds-video", 0);
    SIGN(m);
    bool t6b = !up_manifest(m, strlen(m), sig, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err) && strstr(err, "продукт");
    check(t4 && t5 && t6 && t6b, "обновление: версия не новее — отказ (без отката), другая платформа и другой продукт (обычный/Video) — отказ, «..» в имени — отказ");
    check(t7 && t8, "обновление: хосты — точный белый список (https, без логина/порта), версии сравниваются числами");
    {   /* АО-2: после 3xx приёмник пуст — тело перенаправления не попадает в манифест */
        Buf rb = {0};
        Sink rs = { .mem = &rb, .max = 64 };
        up_write((char *)"<html>moved</html>", 1, 18, &rs);
        bool r1 = rb.len == 18 && up_sink_reset(&rs) && rb.len == 0 && !rs.over;
        up_write((char *)"{}", 1, 2, &rs);
        r1 = r1 && rb.len == 2 && !memcmp(rb.p, "{}", 2);
        free(rb.p);
        check(r1, "обновление: тело перенаправления (3xx) сбрасывается до следующего перехода");
    }
#ifdef _WIN32
    {   /* АО-3: системные/общие каталоги (Windows, Public), и 8.3-путь к ним — отказ; DACL с записью для других — «общий» */
        WCHAR sr[MAX_PATH], pb[MAX_PATH], pe[MAX_PATH + 16], sh[MAX_PATH];
        DWORD a1 = GetEnvironmentVariableW(L"SystemRoot", sr, MAX_PATH), a2 = GetEnvironmentVariableW(L"PUBLIC", pb, MAX_PATH);
        bool d1 = a1 && a1 < MAX_PATH && !up_acl_ok(sr);
        swprintf(pe, MAX_PATH + 16, L"%ls\\x.exe", sr);
        bool d2 = a1 && a1 < MAX_PATH && up_dir_check(pe) == UP_PLACE_SYS;
        bool d3 = !a2 || a2 >= MAX_PATH || !up_acl_ok(pb);
        bool d4 = true;                         /* короткое имя (если 8.3 включены на томе) раскрывается и тоже отвергается */
        WCHAR pf[MAX_PATH];
        DWORD a3 = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
        if (a3 && a3 < MAX_PATH) {
            DWORD k = GetShortPathNameW(pf, sh, MAX_PATH);
            if (k && k < MAX_PATH - 8) { wcscat(sh, L"\\x.exe"); d4 = up_dir_check(sh) == UP_PLACE_SYS; }
        }
        check(d1 && d2 && d3 && d4, "обновление: системные и общие каталоги (и 8.3-путь в Program Files) — отказ, DACL Windows/Public — запись для других");
    }
#endif
    {   /* ST_UPD_MANIFEST=путь: проверить настоящий манифест из tools/sign_release.sh встроенными ключами */
        const char *mp = getenv("ST_UPD_MANIFEST");
        if (mp) {
            char sp[600], mb[UP_MAN_MAX + 1];
            unsigned char sg[64];
            snprintf(sp, sizeof sp, "%s.sig", mp);
            FILE *a = fopen(mp, "rb"), *b = fopen(sp, "rb");
            size_t mn = a ? fread(mb, 1, UP_MAN_MAX, a) : 0, sn = b ? fread(sg, 1, 64, b) : 0;
            if (a) fclose(a);
            if (b) fclose(b);
            up_test_key_on = false;
            bool real = sn == 64 && up_manifest(mb, mn, sg, ASTRA_VERSION, v, sizeof v, f, sizeof f, &sz, h, err, sizeof err);
            up_test_key_on = true;
            char w2[300];
            snprintf(w2, sizeof w2, "обновление: манифест из sign_release.sh проходит встроенным ключом (%.60s %.40s)", real ? v : err, real ? f : "");
            check(real, w2);
        }
    }
    /* установка: файл с неверным хэшем и больше подписанного — exe не тронут, временный удалён */
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    char exe[600], src[600], tdir[560];
    snprintf(tdir, sizeof tdir, "%s/astra-upd-test", tmp);   /* свой каталог (общий /tmp обновление отвергает — так и задумано) */
#ifdef _WIN32
    { WCHAR wd[600]; MultiByteToWideChar(CP_UTF8, 0, tdir, -1, wd, 600); CreateDirectoryW(wd, NULL); }
#else
    mkdir(tdir, 0700);
#endif
    snprintf(exe, sizeof exe, "%s/astra-upd-test.bin", tdir);
    snprintf(src, sizeof src, "%s/astra-upd-src.bin", tdir);
    FILE *fe = fopen(exe, "wb"); if (fe) { fputs("old!!", fe); fclose(fe); }
#ifndef _WIN32
    chmod(exe, 0700);                           /* при umask 002 (Ubuntu) файл вышел бы с записью для группы — «общий каталог» */
#endif
    FILE *fs = fopen(src, "wb"); if (fs) { fputs("hello", fs); fclose(fs); }
    up_exe_override = exe;
    up_src_override = src;
    up_size = 5;
    snprintf(up_sha, sizeof up_sha, "%064d", 0);   /* хэш не совпадёт */
    bool i1 = !up_install(err, sizeof err) && strstr(err, "хэш");
    char rd[16] = "";
    fe = fopen(exe, "rb"); if (fe) { size_t r = fread(rd, 1, 15, fe); rd[r] = 0; fclose(fe); }
    bool i2 = !strcmp(rd, "old!!");
    up_size = 3;                                /* файл больше подписанного размера */
    bool i3 = !up_install(err, sizeof err) && strstr(err, "размера");
    up_size = 5;
    snprintf(up_sha, sizeof up_sha, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");   /* sha256("hello") */
    bool i4 = up_install(err, sizeof err);
    fe = fopen(exe, "rb"); if (fe) { size_t r = fread(rd, 1, 15, fe); rd[r] = 0; fclose(fe); }
    bool i5 = i4 && !strcmp(rd, "hello");
    /* АО-3 (мягко): каталог открыт на запись другим — без подтверждения отказ, с «обновить всё равно» — замена */
    bool c1 = true, c2 = true;
#ifndef _WIN32
    chmod(tdir, 0777);
    fe = fopen(exe, "wb"); if (fe) { fputs("old!!", fe); fclose(fe); }
    up_force = false;
    c1 = !up_install(err, sizeof err) && strstr(err, "подтвержд");
    up_force = true;
    c2 = up_install(err, sizeof err);
    chmod(tdir, 01777);                         /* sticky (как /tmp) — отказ даже с подтверждением */
    c2 = c2 && !up_install(err, sizeof err) && strstr(err, "sticky");
    up_force = false;
    chmod(tdir, 0700);
#else
    {
        WCHAR wd[600];
        MultiByteToWideChar(CP_UTF8, 0, tdir, -1, wd, 600);
        PACL old = NULL, nacl = NULL;
        PSECURITY_DESCRIPTOR sd = NULL;
        EXPLICIT_ACCESSW ea = { .grfAccessPermissions = FILE_GENERIC_WRITE, .grfAccessMode = GRANT_ACCESS, .grfInheritance = NO_INHERITANCE };
        BYTE ev[SECURITY_MAX_SID_SIZE];
        DWORD en = sizeof ev;
        c1 = c2 = false;
        if (CreateWellKnownSid(WinWorldSid, NULL, ev, &en) &&
            GetNamedSecurityInfoW(wd, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL, &sd) == ERROR_SUCCESS) {
            ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
            ea.Trustee.ptstrName = (LPWSTR)ev;
            if (SetEntriesInAclW(1, &ea, old, &nacl) == ERROR_SUCCESS &&
                SetNamedSecurityInfoW(wd, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, nacl, NULL) == ERROR_SUCCESS) {
                c1 = !up_acl_ok(wd);            /* «Все: запись» -> общий */
                c2 = true;
                SetNamedSecurityInfoW(wd, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, old, NULL);
            }
            if (nacl) LocalFree(nacl);
            LocalFree(sd);
        }
    }
#endif
    check(c1 && c2, "обновление: каталог с записью для других — без подтверждения отказ, «обновить всё равно» — разрешено");
    char oldp[620];
    snprintf(oldp, sizeof oldp, "%s.old", exe);
    remove(exe); remove(src); remove(oldp);
#ifdef _WIN32
    { WCHAR wd[600]; MultiByteToWideChar(CP_UTF8, 0, tdir, -1, wd, 600); RemoveDirectoryW(wd); }
#else
    rmdir(tdir);
#endif
    up_exe_override = up_src_override = NULL;
    up_test_key_on = false;
    EVP_PKEY_free(tk);
    char w[300];
    snprintf(w, sizeof w, "обновление: хэш не совпал и размер больше подписанного — отказ, прежний файл цел; верный — заменён (%.15s)", i5 ? "ok" : err);
    check(i1 && i2 && i3 && i5, w);
    return 0;
    #undef SIGN
}
