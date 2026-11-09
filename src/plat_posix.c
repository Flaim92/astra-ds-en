/* Платформенный слой: Linux (POSIX). */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <malloc.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "plat.h"
#include "gui.h"

extern char **environ;

/* ---------- ожидание ---------- */
void plat_wait(const Waitable *w, int n, int timeout_ms) {
    struct pollfd p[32];
    int k = 0;
    for (int i = 0; i < n && k < 32; i++) if (w[i].h >= 0) p[k++] = (struct pollfd){ (int)w[i].h, w[i].ev ? w[i].ev : POLLIN, 0 };
    int r;
    do r = poll(p, (nfds_t)k, timeout_ms); while (r < 0 && errno == EINTR && 0);
}

struct PlatWake { int fd; };
PlatWake *wake_new(void) {
    PlatWake *w = calloc(1, sizeof *w);
    if (w) w->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return w;
}
void wake_signal(PlatWake *w) { if (w) { uint64_t one = 1; ssize_t r = write(w->fd, &one, 8); (void)r; } }
void wake_clear(PlatWake *w) { if (w) { uint64_t v; ssize_t r = read(w->fd, &v, 8); (void)r; } }
Waitable wake_waitable(PlatWake *w) { return (Waitable){ w ? w->fd : -1, POLLIN }; }
void wake_free(PlatWake *w) { if (w) { close(w->fd); free(w); } }

struct SockWait { int fd; };
SockWait *sockwait_new(intptr_t sock) { SockWait *s = calloc(1, sizeof *s); if (s) s->fd = (int)sock; return s; }
Waitable sockwait_get(SockWait *s) { return (Waitable){ s ? s->fd : -1, POLLIN }; }
void sockwait_clear(SockWait *s) { (void)s; }   /* poll по уровню — сбрасывать нечего */
void sockwait_free(SockWait *s) { free(s); }

/* ---------- время ---------- */
int64_t plat_mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int64_t plat_wall_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int64_t plat_timegm(struct tm *t) { return (int64_t)timegm(t); }
void plat_localtime(int64_t s, struct tm *out) { time_t t = (time_t)s; localtime_r(&t, out); }
void plat_sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

/* ---------- память ---------- */
void *plat_pages(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void plat_pages_free(void *p, size_t n) { if (p) munmap(p, n); }
void plat_lock(void *p, size_t n) { mlock(p, n); madvise(p, n, MADV_DONTDUMP); }
void plat_unlock(void *p, size_t n) { munlock(p, n); }
void plat_wipe(void *p, size_t n) { if (p && n) explicit_bzero(p, n); }
void plat_trim(void) { malloc_trim(0); }
int64_t plat_cpu_ms(void) {
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (int64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000;
}
size_t plat_private_bytes(void) {
    /* RssAnon — анонимная (частная) резидентная память */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[128];
    size_t kb = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "RssAnon: %zu", &kb) == 1) break;
    fclose(f);
    return kb * 1024;
}

/* ---------- процесс ---------- */
static void (*g_on_crash)(void);
static void crash_handler(int sig) {
    if (g_on_crash) g_on_crash();
    _exit(128 + sig);
}
void plat_harden(void (*on_crash)(void)) {
    /* Аллокатор glibc: одна арена на процесс (у голосового потока не будет своей на 64 МБ
     * адресов с хвостами), блоки от 64 КБ — отдельными отображениями, которые сразу возвращаются
     * системе при free, а свободный хвост кучи отдаётся при остатке от 128 КБ. */
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    g_on_crash = on_crash;
    /* Нет дампа памяти и нет ptrace от процессов того же пользователя. */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    struct rlimit rl = { 0, 0 };
    setrlimit(RLIMIT_CORE, &rl);
    signal(SIGPIPE, SIG_IGN);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) signal(sigs[i], crash_handler);
}
void plat_net_init(void) {}
const char *plat_ca_file(void) {
    static const char *const paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",                 /* Debian, Ubuntu, Arch */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  /* Fedora, RHEL */
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",                             /* openSUSE */
        "/etc/ssl/cert.pem",                                  /* Alpine */
    };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) if (access(paths[i], R_OK) == 0) return paths[i];
    return NULL;
}

/* ---------- потоки ---------- */
struct PlatThread { pthread_t t; int (*fn)(void *); void *arg; };
static void *tramp(void *p) { PlatThread *t = p; return (void *)(intptr_t)t->fn(t->arg); }
PlatThread *thread_start(int (*fn)(void *), void *arg) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    if (pthread_create(&t->t, &a, tramp, t)) { pthread_attr_destroy(&a); free(t); return NULL; }
    pthread_attr_destroy(&a);
    return t;
}
void thread_join(PlatThread *t) { if (t) { pthread_join(t->t, NULL); free(t); } }
void plat_timer_hires(bool on) { (void)on; }
int plat_ui_lang(void) {                        /* LC_ALL / LC_MESSAGES / LANG: ru/uk/be -> русский; C/POSIX/пусто — русский (как раньше) */
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    if (!l || !*l || !strcmp(l, "C") || !strncmp(l, "C.", 2) || !strcmp(l, "POSIX")) return 0;
    return strncmp(l, "ru", 2) && strncmp(l, "uk", 2) && strncmp(l, "be", 2) ? 1 : 0;
}
void thread_audio_priority(void) {
    struct sched_param sp = { .sched_priority = 10 };
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) != 0) {
        int r = setpriority(PRIO_PROCESS, 0, -5);
        (void)r;
    }
}
struct PlatMutex { pthread_mutex_t m; };
PlatMutex *mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); if (m) pthread_mutex_init(&m->m, NULL); return m; }
void mutex_lock(PlatMutex *m) { pthread_mutex_lock(&m->m); }
void mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(&m->m); }
void mutex_free(PlatMutex *m) { if (m) { pthread_mutex_destroy(&m->m); free(m); } }

/* ---------- UDP ---------- */
intptr_t udp_open(void) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET };
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    return s;
}
bool udp_resolve(const char *ip, uint16_t port, void *addr16) {
    struct sockaddr_in *a = addr16;
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    return inet_pton(AF_INET, ip, &a->sin_addr) == 1;
}
int udp_send(intptr_t s, const void *buf, size_t n, const void *addr16) {
    return (int)sendto((int)s, buf, n, MSG_NOSIGNAL, (const struct sockaddr *)addr16, sizeof(struct sockaddr_in));
}
int udp_recv(intptr_t s, void *buf, size_t n, void *from16) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    socklen_t al = sizeof a;
    ssize_t r = recvfrom((int)s, buf, n, 0, (struct sockaddr *)&a, &al);
    if (r < 0) return -1;
    if (from16) memcpy(from16, &a, sizeof a);
    return (int)r;
}
bool udp_same(const void *a16, const void *b16) {
    const struct sockaddr_in *a = a16, *b = b16;
    return a->sin_family == AF_INET && b->sin_family == AF_INET && a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}
void udp_close(intptr_t s) { if (s >= 0) close((int)s); }

/* ---------- хранилище секретов: Secret Service через secret-tool ----------
 * Токен идёт через stdin дочернего процесса, а не в аргументах (их видно в /proc). */
/* Самопроверка пишет в отдельные записи и не трогает сохранённые токен и ссылку пользователя:
 * account selftest (токен) и selftest-<entry> (прочее). */
static bool kr_test;
void keyring_test_mode(void) { kr_test = true; }
static char *kr_account(const char *entry, char *buf, size_t n) {
    if (kr_test) snprintf(buf, n, strcmp(entry, "token") ? "selftest-%s" : "selftest", entry);
    else snprintf(buf, n, "%s", entry);
    return buf;
}

/* Программа, которой отдаётся токен, ищется НЕ по PATH: подложенный ~/.local/bin/secret-tool
 * получил бы его. Только системные каталоги, файл root и не доступен на запись другим. */
static const char *tool_path(void) {
    static const char *const cands[] = { "/usr/bin/secret-tool", "/bin/secret-tool", "/usr/local/bin/secret-tool" };
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        struct stat st;
        if (stat(cands[i], &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH)))
            return cands[i];
    }
    return NULL;
}
/* Окружение для secret-tool — только то, что нужно, чтобы найти сеанс D-Bus. */
static char **tool_env(char store[4][512]) {
    static char *env[5];
    static const char *const keep[] = { "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR", "HOME", "LANG" };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const char *v = getenv(keep[i]);
        if (!v) continue;
        snprintf(store[n], 512, "%s=%s", keep[i], v);
        env[n] = store[n];
        n++;
    }
    env[n] = NULL;
    return env;
}

static pid_t spawn_tool(char *const argv[], int *to_child, int *from_child) {
    const char *path = tool_path();
    if (!path) return -1;
    int in[2] = { -1, -1 }, out[2] = { -1, -1 };
    if (to_child && pipe2(in, O_CLOEXEC)) return -1;
    if (from_child && pipe2(out, O_CLOEXEC)) { if (to_child) { close(in[0]); close(in[1]); } return -1; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (to_child) posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    if (from_child) posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (!from_child) posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (!to_child) posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    pid_t pid = -1;
    char envs[4][512];
    if (posix_spawn(&pid, path, &fa, NULL, argv, tool_env(envs)) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    if (to_child) { close(in[0]); if (pid < 0) close(in[1]); else *to_child = in[1]; }
    if (from_child) { close(out[1]); if (pid < 0) close(out[0]); else *from_child = out[0]; }
    return pid;
}
static int wait_tool(pid_t pid) {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#define KR_NEW "astra-ds"
#define KR_OLD "mincord"
static int kr_load_svc(const char *svc, const char *entry, char *out, size_t cap) {
    char acc[64];
    char *argv[] = { "secret-tool", "lookup", "service", (char *)svc, "account", kr_account(entry, acc, sizeof acc), NULL };
    int fd = -1;
    pid_t pid = spawn_tool(argv, NULL, &fd);
    if (pid < 0) return -1;
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, out + n, cap - 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
        if (n >= cap - 1) break;
    }
    close(fd);
    int rc = wait_tool(pid);
    /* secret-tool lookup: код 1 без вывода — записи нет (-1); прочее — ошибка чтения (-2, ревью 8 М3) */
    if (rc != 0 || n == 0) { explicit_bzero(out, cap); return rc == 1 && n == 0 ? -1 : -2; }
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    out[n] = 0;
    return (int)n;
}
static bool kr_save_svc(const char *svc, const char *entry, const char *tok, size_t n) {
    char acc[64];
    char *argv[] = { "secret-tool", "store", "--label=Astra DS", "service", (char *)svc, "account", kr_account(entry, acc, sizeof acc), NULL };
    int fd = -1;
    pid_t pid = spawn_tool(argv, &fd, NULL);
    if (pid < 0) return false;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, tok + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
    return wait_tool(pid) == 0 && off == n;
}
static void kr_delete_svc(const char *svc, const char *entry) {
    char acc[64];
    char *argv[] = { "secret-tool", "clear", "service", (char *)svc, "account", kr_account(entry, acc, sizeof acc), NULL };
    pid_t pid = spawn_tool(argv, NULL, NULL);
    if (pid > 0) wait_tool(pid);
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

const char *keyring_name(void) { return "Secret Service (secret-tool)"; }

/* Открыть ссылку-приглашение в браузере (SECURITY С3-а): только https://discord.gg/КОД и
 * https://discord.com/invite/КОД (КОД [A-Za-z0-9-]{2,32}); xdg-open — абсолютный путь из системных
 * каталогов (не PATH), окружение — только для сеанса рабочего стола, стандартные потоки — /dev/null. */
static void *url_reap(void *a) {               /* забрать завершившийся xdg-open (без зомби) */
    pid_t p = (pid_t)(intptr_t)a;
    while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {}
    return NULL;
}
/* Запуск системной программы по абсолютному пути (root, не правится группой/всеми), чистое окружение
 * сеанса, потоки — /dev/null, ожидание — в отдельном потоке. */
static bool spawn_sys(const char *const *cands, int nc, char *const argv[]) {
    const char *path = NULL;
    for (int i = 0; i < nc && !path; i++) {
        struct stat st;
        if (stat(cands[i], &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH))) path = cands[i];
    }
    if (!path) return false;
    static const char *const keep[] = { "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "HOME" };
    char store[6][512];
    char *env[7];
    int k = 0;
    for (int i = 0; i < 5; i++) { const char *v = getenv(keep[i]); if (v && strlen(v) < 480) { snprintf(store[k], 512, "%s=%s", keep[i], v); env[k] = store[k]; k++; } }
    snprintf(store[k], 512, "PATH=/usr/bin:/bin");
    env[k] = store[k]; k++;
    env[k] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    bool ok = posix_spawn(&pid, path, &fa, NULL, argv, env) == 0;
    posix_spawn_file_actions_destroy(&fa);
    if (ok) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 64 << 10);
        if (pthread_create(&t, &at, url_reap, (void *)(intptr_t)pid) != 0) waitpid(pid, NULL, WNOHANG);
        pthread_attr_destroy(&at);
    }
    return ok;
}
/* №123: уведомление через notify-send, только если он есть; не чаще раза в 2 с */
void plat_notify(const char *title, const char *text) {
    static i64 last;
    i64 now = now_ms();
    if (now - last < 2000) return;
    last = now;
    static const char *const cands[] = { "/usr/bin/notify-send", "/bin/notify-send" };
    /* У-1: тело notify-send — разметка (<img src=...> пошёл бы в сеть мимо VLESS): & < > экранировать */
    char t1[400], t2[900];
    const char *src[2] = { title ? title : "Astra DS", text ? text : "" };
    char *dst[2] = { t1, t2 };
    size_t cap[2] = { sizeof t1, sizeof t2 };
    for (int i = 0; i < 2; i++) {
        size_t k = 0;
        for (const char *p = src[i]; *p && k + 6 < cap[i]; p++) {
            const char *rep = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : NULL;
            if (rep) { memcpy(dst[i] + k, rep, strlen(rep)); k += strlen(rep); }
            else dst[i][k++] = *p;
        }
        while (k && ((unsigned char)dst[i][k - 1] & 0xC0) == 0x80 && k > 0) {   /* не оборвать букву UTF-8 */
            size_t s0 = k - 1;
            while (s0 && ((unsigned char)dst[i][s0] & 0xC0) == 0x80) s0--;
            unsigned char lead = (unsigned char)dst[i][s0];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (k - s0 >= need) break;
            k = s0;
        }
        dst[i][k] = 0;
    }
    char *argv[] = { (char *)"notify-send", (char *)"-a", (char *)"Astra DS", (char *)"--", t1, t2, NULL };   /* «--»: не ключи */
    spawn_sys(cands, 2, argv);
}
bool open_url_browser(const char *url) {
    static const char *const PRE[] = { "https://discord.gg/", "https://discord.com/invite/" };
    const char *code = NULL;
    for (int i = 0; i < 2; i++) if (!strncmp(url, PRE[i], strlen(PRE[i]))) code = url + strlen(PRE[i]);
    if (!code) return false;
    size_t n = strlen(code);
    if (n < 2 || n > 32) return false;
    for (size_t i = 0; i < n; i++) if (!((code[i] >= 'a' && code[i] <= 'z') || (code[i] >= 'A' && code[i] <= 'Z') || (code[i] >= '0' && code[i] <= '9') || code[i] == '-')) return false;
    static const char *const cands[] = { "/usr/bin/xdg-open", "/bin/xdg-open", "/usr/local/bin/xdg-open" };
    const char *path = NULL;
    for (size_t i = 0; i < sizeof cands / sizeof *cands && !path; i++) {
        struct stat st;
        if (stat(cands[i], &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH))) path = cands[i];
    }
    if (!path) return false;
    static const char *const keep[] = { "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "HOME" };
    char store[6][512];
    char *env[7];
    int k = 0;
    for (int i = 0; i < 5; i++) { const char *v = getenv(keep[i]); if (v && strlen(v) < 480) { snprintf(store[k], 512, "%s=%s", keep[i], v); env[k] = store[k]; k++; } }
    snprintf(store[k], 512, "PATH=/usr/bin:/bin");
    env[k] = store[k]; k++;
    env[k] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = { (char *)"xdg-open", (char *)url, NULL };
    pid_t pid = -1;
    bool ok = posix_spawn(&pid, path, &fa, NULL, argv, env) == 0;   /* дескрипторы программы — O_CLOEXEC */
    posix_spawn_file_actions_destroy(&fa);
    if (ok) {                                   /* главный поток не ждёт xdg-open: ждёт отдельный короткий поток */
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 64 << 10);
        if (pthread_create(&t, &at, url_reap, (void *)(intptr_t)pid) != 0) waitpid(pid, NULL, WNOHANG);
        pthread_attr_destroy(&at);
    }
    return ok;
}

/* ---------- дочерняя программа ---------- */
struct PlatFile { int fd; };
PlatFile *exe_open(const char *name, char *path, size_t pathn) {
    char p[4096];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) return NULL;
    p[n] = 0;
    char *slash = strrchr(p, '/');
    if (!slash || (size_t)(slash + 1 - p) + strlen(name) + 1 > sizeof p) return NULL;
    snprintf(slash + 1, sizeof p - (size_t)(slash + 1 - p), "%s", name);
    if (path && pathn) snprintf(path, pathn, "%s", p);
    /* Хэш считается по этому fd, и запускается тоже он (fexecve): подмена файла по пути после
     * проверки ничего не даёт. O_NOFOLLOW — не идти по символической ссылке на месте программы. */
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    PlatFile *f = calloc(1, sizeof *f);
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}
long exe_read(PlatFile *f, void *buf, size_t n) {
    for (;;) {
        ssize_t r = read(f->fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return (long)r;
    }
}
void exe_close(PlatFile *f) { if (f) { close(f->fd); free(f); } }

struct PlatProc { pid_t pid; int pidfd; bool dead; };
PlatProc *proc_spawn(PlatFile *f, const char *const *argv, const void *in, size_t n) {
    int pp[2];
    if (pipe2(pp, O_CLOEXEC)) return NULL;
    int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nul < 0) { close(pp[0]); close(pp[1]); return NULL; }
    pid_t parent = getpid();
    /* Среда Go у Xray настроена на минимум ресурсов: один рабочий поток, чаще сборка мусора,
     * мягкий потолок кучи. Больше ничего из окружения mincord не передаётся. */
    char *const envp[] = { "GOMAXPROCS=1", "GOGC=50", "GOMEMLIMIT=12MiB", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        /* Дочерний процесс: только безопасные после fork вызовы. Умереть вместе с mincord
         * (сигнал придёт при смерти потока-родителя — это главный поток, он живёт до конца). */
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        if (getppid() != parent) _exit(127);   /* родитель умер раньше prctl */
        if (dup2(pp[0], 0) < 0 || dup2(nul, 1) < 0 || dup2(nul, 2) < 0) _exit(127);
        /* Ни одного чужого дескриптора (сокеты curl, хранилище): всё от 3 — закрыть при exec */
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3U, ~0U, 4U /* CLOSE_RANGE_CLOEXEC */) != 0)
#endif
        for (int i = 3; i < 4096; i++) if (i != f->fd) close(i);
        signal(SIGPIPE, SIG_DFL);              /* SIG_IGN наследуется через exec */
        fexecve(f->fd, (char *const *)argv, envp);
        _exit(127);
    }
    close(pp[0]);
    close(nul);
    if (pid < 0) { close(pp[1]); return NULL; }
    /* Конфиг: канал вмещает 64 КБ, процесс читает его при старте. SIGPIPE игнорируется. */
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(pp[1], (const char *)in + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(pp[1]);
    PlatProc *p = calloc(1, sizeof *p);
    if (!p) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return NULL; }
    p->pid = pid;
#ifdef SYS_pidfd_open
    p->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);   /* читаемым становится при завершении */
#else
    p->pidfd = -1;
#endif
    return p;
}
bool proc_alive(PlatProc *p) {
    if (!p || p->dead) return false;
    int st;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid || (r < 0 && errno == ECHILD)) p->dead = true;
    return !p->dead;
}
Waitable proc_waitable(PlatProc *p) { return (Waitable){ p && !p->dead ? p->pidfd : -1, POLLIN }; }
void proc_kill(PlatProc *p) {
    if (!p) return;
    if (!p->dead) {
        kill(p->pid, SIGKILL);
        while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (p->pidfd >= 0) close(p->pidfd);
    free(p);
}
long proc_rss_kb(PlatProc *p) {
    if (!p || p->dead) return -1;
    char path[64], line[128];
    snprintf(path, sizeof path, "/proc/%d/status", (int)p->pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long kb = -1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "VmRSS: %ld", &kb) == 1) break;
    fclose(f);
    return kb;
}
int tcp_free_port(void) {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof a;
    int port = -1;
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr *)&a, &len) == 0) port = ntohs(a.sin_port);
    close(s);
    return port;
}
int tcp_local_ready(int port, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int r = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) r = 1;
    else if (errno == EINPROGRESS) {
        struct pollfd pf = { s, POLLOUT, 0 };
        int e = 0;
        socklen_t el = sizeof e;
        if (poll(&pf, 1, timeout_ms) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e == 0) r = 1;
    }
    close(s);
    return r;
}

/* ---------- терминал ----------
 * Два исполнения term_*: терминал (--tui, ниже) и окно X11 (по умолчанию, gui_x11.c + gui_grid.c).
 * Выбор — term_open_window() до первого вывода: дальше все term_* идут в окно. */
static struct termios t_old;
static bool t_active, t_gui;
static int winch_pipe[2] = { -1, -1 };

static void on_winch(int s) { (void)s; char c = 1; ssize_t r = write(winch_pipe[1], &c, 1); (void)r; }

/* Открыть окно; false — нет $DISPLAY / X-сервера, остаётся терминал. */
bool term_open_window(bool hidden) {
    if (t_gui) return true;
    if (!gb_open(hidden)) return false;
    t_gui = true;
    return true;
}
bool term_windowed(void) { return t_gui; }
void term_inject(const Key *k) { if (t_gui) gb_inject(k); }
void term_resize(int w, int h) { if (t_gui) gui_resize(w, h); }
bool term_snapshot(const char *path) { return t_gui && gb_snapshot(path); }
int term_mouse_seen(void) { return gui_mouse_seen; }
/* Мелкая сетка картинок (слои ESC _ mincord-img, см. gui.h): только окно. Сильный символ —
 * заменяет слабую заглушку ui.c. */
int term_subcell_scale(void) { return t_gui ? gb_fine_scale() : 0; }
int term_fullscreen(int mode) { return t_gui ? gb_fullscreen(mode) : -1; }
/* №141: на Linux глобальный опрос клавиш не делаем (X11 из другого потока небезопасен) — рация по F8 */
bool plat_key_down(int vk) { (void)vk; return false; }

void plat_key_name(int vk, char *out, size_t n) { (void)vk; if (n) out[0] = 0; }
bool term_fine_images(void) { return term_subcell_scale() >= 4; }
bool term_zoom(int step) { return t_gui && gb_zoom(step); }
int term_zoom_pct(void) { return t_gui ? gb_zoom_pct() : 100; }

void term_console_puts(const char *s) { ssize_t r = write(1, s, strlen(s)); (void)r; }
void term_puts(const char *s) { if (t_gui) gui_puts(s); else term_console_puts(s); }

bool term_interactive(void) { return t_gui || isatty(0) == 1; }

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
    term_puts(prompt);
    struct termios o, t;
    if (!cap || tcgetattr(0, &o)) return false;
    out[0] = 0;
    t = o;
    t.c_lflag &= (tcflag_t)~(ECHO | ECHONL);
    t.c_lflag |= ICANON;
    tcsetattr(0, TCSAFLUSH, &t);
    /* Пробелы, кавычки и табуляции (прилипают при копировании) выбрасываются. Проверку формата
     * делает получатель (токен — только ASCII, ссылка vless:// — свои правила). Строка длиннее
     * cap — ошибка; пустая строка (просто Enter) — true и "". Канонический режим терминала
     * ограничивает строку 4095 байтами — ссылкам vless:// этого хватает. */
    size_t k = 0;
    bool bad = false, eol = false;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (c == '\n' || c == '\r') { eol = true; break; }
        if (c == ' ' || c == '"' || c == '\t') continue;
        if (k + 1 >= cap) bad = true;
        if (!bad) out[k++] = c;
    }
    tcsetattr(0, TCSAFLUSH, &o);
    term_puts("\n");
    if (bad || !eol) { k = 0; bad = true; }      /* конец ввода без перевода строки — тоже отказ */
    explicit_bzero(out + k, cap - k);
    return !bad;
}

bool term_init(void) {
    if (t_gui) return gui_init();
    if (!isatty(0) || !isatty(1) || tcgetattr(0, &t_old)) return false;
    struct termios t = t_old;
    t.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_cflag |= CS8;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    if (pipe2(winch_pipe, O_NONBLOCK | O_CLOEXEC) == 0) signal(SIGWINCH, on_winch);
    t_active = true;
    /* Альтернативный экран, скрытый курсор, режим вставки со скобками; мышь: нажатия и колесо
     * (1000) в формате SGR (1006) — без отчётов о каждом перемещении (1003). */
    const char *s = "\x1b[?1049h\x1b[?25l\x1b[?2004h\x1b[?1000h\x1b[?1006h\x1b[2J";
    term_write(s, strlen(s));
    return true;
}
void term_shutdown(void) {
    if (t_gui) { gui_shutdown(); return; }
    if (!t_active) return;
    const char *s = "\x1b[0m\x1b[2J\x1b[?1006l\x1b[?1000l\x1b[?2004l\x1b[?25h\x1b[?1049l";
    term_write(s, strlen(s));
    tcsetattr(0, TCSAFLUSH, &t_old);
    t_active = false;
}
void term_size(int *w, int *h) {
    if (t_gui) { gui_size(w, h); return; }
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) { *w = ws.ws_col; *h = ws.ws_row; }
    else { *w = 80; *h = 24; }
}
void term_write(const char *s, size_t n) {
    if (t_gui) { gui_write(s, n); return; }
    while (n) {
        ssize_t w = write(1, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}
int term_waitables(Waitable *out, int max) {
    if (t_gui) return gb_waitables(out, max);
    int n = 0;
    if (n < max) out[n++] = (Waitable){ 0, POLLIN };
    if (n < max && winch_pipe[0] >= 0) out[n++] = (Waitable){ winch_pipe[0], POLLIN };
    return n;
}

/* Разбор ввода: UTF-8, управляющие байты и escape-последовательности VT/xterm. */
static bool in_paste;
static int csi_key(int num, char fin) {
    switch (fin) {
    case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME; case 'F': return K_END; case 'Z': return K_BTAB;
    case 'P': return K_F1; case 'Q': return K_F2; case 'R': return K_F3; case 'S': return K_F4;
    case '~':
        switch (num) {
        case 1: case 7: return K_HOME; case 4: case 8: return K_END; case 2: return K_INS; case 3: return K_DEL;
        case 5: return K_PGUP; case 6: return K_PGDN;
        case 11: return K_F1; case 12: return K_F2; case 13: return K_F3; case 14: return K_F4; case 15: return K_F5;
        case 17: return K_F6; case 18: return K_F7; case 19: return K_F8; case 20: return K_F9; case 21: return K_F10;
        case 23: return K_F11; case 24: return K_F12;
        }
    }
    return K_NONE;
}
/* Мышь xterm SGR: ESC [ < b ; x ; y M (нажатие) / m (отпускание). b: 0..2 — кнопка, +4 Shift,
 * +8 Alt, +16 Ctrl, +32 перемещение, 64/65 — колесо вверх/вниз. x, y — с 1. */
static bool sgr_mouse(int b, int x, int y, char fin, Key *k) {
    if (fin != 'M' || x < 1 || y < 1) return false;
    int btn;
    if (b & 64) btn = (b & 1) ? 5 : 4;
    else if (b & 32) btn = 0;
    else if ((b & 3) == 3) return false;
    else btn = (b & 3) == 0 ? 1 : (b & 3) == 1 ? 2 : 3;
    *k = (Key){ .code = K_MOUSE, .x = x - 1, .y = y - 1, .button = btn,
                .shift = (b & 4) != 0, .alt = (b & 8) != 0, .ctrl = (b & 16) != 0 };
    return true;
}
int term_read(Key *out, int max) {
    if (t_gui) return gui_read(out, max);
    int n = 0;
    if (winch_pipe[0] >= 0) {
        char d[16];
        if (read(winch_pipe[0], d, sizeof d) > 0 && n < max) out[n++] = (Key){ .code = K_RESIZE };
    }
    /* Хвост, не уместившийся в max клавиш или разрезанный на границе чтения, ждёт следующего вызова. */
    static unsigned char b[4096];
    static ssize_t have;
    if (have < (ssize_t)sizeof b) {
        ssize_t r = read(0, b + have, sizeof b - (size_t)have);
        if (r > 0) have += r;
    }
    ssize_t got = have;
    if (got <= 0) return n;
    ssize_t i = 0;
    while (i < got && n < max) {
        unsigned char c = b[i];
        if (c == 0x1b) {
            if (i + 1 >= got) { out[n++] = (Key){ .code = K_ESC }; i++; continue; }
            if (b[i + 1] == '[' && i + 2 < got && b[i + 2] == '<') {
                /* мышь SGR 1006 */
                ssize_t j = i + 3;
                int f[3] = { 0, 0, 0 }, field = 0;
                while (j < got && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) {
                    if (b[j] == ';') { if (++field > 2) field = 2; }
                    else if (f[field] < 100000) f[field] = f[field] * 10 + (b[j] - '0');
                    j++;
                }
                if (j >= got) break;             /* не дочитано — ждём остаток */
                i = j + 1;
                if (sgr_mouse(f[0], f[1], f[2], (char)b[j], &out[n])) n++;
                continue;
            }
            if (b[i + 1] == '[' || b[i + 1] == 'O') {
                ssize_t j = i + 2;
                int num = 0, mod = 0, field = 0;
                while (j < got && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) {
                    if (b[j] == ';') field++;
                    else if (field == 0) num = num * 10 + (b[j] - '0');
                    else mod = mod * 10 + (b[j] - '0');
                    j++;
                }
                if (j >= got) break;             /* последовательность не дочитана — ждём остаток */
                char fin = (char)b[j];
                i = j + 1;
                if (fin == '~' && num == 200) { in_paste = true; continue; }
                if (fin == '~' && num == 201) { in_paste = false; continue; }
                /* Shift+Enter у терминалов с расширенными клавишами (CSI 13;2u, kitty/xterm modifyOtherKeys) */
                if (fin == 'u' && num == 13 && mod >= 2) { out[n++] = (Key){ .code = K_CHAR, .ch = '\n' }; continue; }   /* Enter с любым модификатором — перенос */
                if (fin == 'u' && num == 13) { out[n++] = (Key){ .code = K_ENTER }; continue; }
                int k = csi_key(num, fin);
                if (k != K_NONE) {
                    bool sh = mod == 2 || mod == 4 || mod == 6 || mod == 8;
                    bool al = mod == 3 || mod == 4 || mod == 7 || mod == 8;
                    bool ct = mod >= 5;
                    out[n++] = (Key){ .code = k, .shift = sh, .alt = al, .ctrl = ct };
                }
                continue;
            }
            /* Alt+клавиша */
            i++;
            uint32_t cp = b[i];
            if (cp >= 0x80 && i + ((cp & 0xE0) == 0xC0 ? 2 : (cp & 0xF0) == 0xE0 ? 3 : 4) > got) { i--; break; }
            int len = 1;
            if (cp >= 0x80) {
                if ((cp & 0xE0) == 0xC0) len = 2; else if ((cp & 0xF0) == 0xE0) len = 3; else if ((cp & 0xF8) == 0xF0) len = 4;
                cp &= (uint32_t)(len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
                for (int q = 1; q < len && i + q < got; q++) cp = (cp << 6) | (b[i + q] & 0x3F);
            }
            i += len;
            if (cp == '\r' || cp == '\n') { out[n++] = (Key){ .code = K_CHAR, .ch = '\n' }; continue; }   /* Alt+Enter — перенос строки */
            out[n++] = (Key){ .code = K_CHAR, .ch = cp, .alt = true };
            continue;
        }
        /* Enter — '\r' (ICRNL выключен); Ctrl+J — '\n': перенос строки (терминал не отличает Shift+Enter от Enter) */
        if (c == '\r') { out[n++] = in_paste ? (Key){ .code = K_CHAR, .ch = '\n' } : (Key){ .code = K_ENTER }; i++; continue; }
        if (c == '\n') { out[n++] = (Key){ .code = K_CHAR, .ch = '\n' }; i++; continue; }
        if (c == '\t') { out[n++] = (Key){ .code = K_TAB }; i++; continue; }
        if (c == 0x7f || c == 0x08) { out[n++] = (Key){ .code = K_BS }; i++; continue; }
        if (c < 0x20) { out[n++] = (Key){ .code = K_CHAR, .ch = (uint32_t)(c + 'a' - 1), .ctrl = true }; i++; continue; }
        uint32_t cp = c;
        int len = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0) len = 2; else if ((c & 0xF0) == 0xE0) len = 3; else if ((c & 0xF8) == 0xF0) len = 4; else { i++; continue; }
            if (i + len > got) break;
            cp = c & (uint32_t)(len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (int q = 1; q < len; q++) cp = (cp << 6) | (b[i + q] & 0x3F);
        }
        i += len;
        out[n++] = (Key){ .code = K_CHAR, .ch = cp };
    }
    if (i > 0) { memmove(b, b + i, (size_t)(got - i)); have = got - i; }
    if (have == (ssize_t)sizeof b && n == 0) have = 0;   /* мусор без конца — сбросить */
    return n;
}
#endif
