/* portal_wl.c - xdg-desktop-portal ScreenCast для демонстрации экрана под Wayland (Linux, «Astra DS Video»).
 * libdbus-1.so.3 - только через dlopen: заголовки dbus в сборку не нужны, типы и функции объявлены здесь
 * вручную (минимум). Ничего не пишется на диск (restore token не запрашивается: persist_mode 0), ничего
 * не выводится. Протокол:
 *   CreateSession -> Response(session_handle) -> SelectSources(types, cursor_mode) -> Response ->
 *   Start -> Response(streams a(ua{sv})) -> OpenPipeWireRemote -> fd (h).
 * Сигнал Response приходит на путь запроса, который предсказуем по имени соединения и handle_token:
 * подписка на него оформляется ДО вызова метода, иначе быстрый ответ можно пропустить. */
#if defined(ASTRA_VIDEO) && !defined(_WIN32) && !defined(__APPLE__)   /* xdg-desktop-portal есть только в Linux */
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
#include "mc.h"
#include "portal_wl.h"

/* ---- минимальное объявление libdbus-1 (ABI 1.x) ---- */
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef uint32_t dbus_bool_t;
/* как в dbus-errors.h: два указателя, пять однобитовых полей в unsigned int, указатель */
typedef struct { const char *name, *message; unsigned int dummy; void *padding1; } DBusError;
/* dbus-message.h: непрозрачная структура на стеке вызывающего (72 байта на 64 бит, 56 на 32 бит);
 * здесь с запасом и выравниванием указателя: libdbus пишет не больше своего размера. */
typedef struct { void *pad[32]; } DBusMessageIter;
_Static_assert(sizeof(DBusMessageIter) >= 2 * sizeof(void *) + 10 * sizeof(int) + 2 * sizeof(void *), "DBusMessageIter");

enum { T_INVALID = 0, T_BOOL = 'b', T_U32 = 'u', T_I32 = 'i', T_STR = 's', T_PATH = 'o', T_VAR = 'v',
       T_ARRAY = 'a', T_STRUCT = 'r', T_DICT = 'e', T_FD = 'h' };
enum { BUS_SESSION = 0 };

static struct {
    void (*error_init)(DBusError *);
    void (*error_free)(DBusError *);
    DBusConnection *(*bus_get_private)(int, DBusError *);
    const char *(*bus_unique)(DBusConnection *);
    void (*add_match)(DBusConnection *, const char *, DBusError *);
    void (*remove_match)(DBusConnection *, const char *, DBusError *);
    void (*set_exit)(DBusConnection *, dbus_bool_t);
    void (*conn_close)(DBusConnection *);
    void (*conn_unref)(DBusConnection *);
    dbus_bool_t (*read_write)(DBusConnection *, int);
    DBusMessage *(*pop)(DBusConnection *);
    DBusMessage *(*send_block)(DBusConnection *, DBusMessage *, int, DBusError *);
    dbus_bool_t (*send)(DBusConnection *, DBusMessage *, uint32_t *);
    void (*flush)(DBusConnection *);
    dbus_bool_t (*can_send)(DBusConnection *, int);
    dbus_bool_t (*connected)(DBusConnection *);
    DBusMessage *(*new_call)(const char *, const char *, const char *, const char *);
    DBusMessage *(*new_signal)(const char *, const char *, const char *);
    void (*msg_unref)(DBusMessage *);
    dbus_bool_t (*is_signal)(DBusMessage *, const char *, const char *);
    const char *(*get_path)(DBusMessage *);
    const char *(*get_sender)(DBusMessage *);
    dbus_bool_t (*has_sig)(DBusMessage *, const char *);
    dbus_bool_t (*it_init)(DBusMessage *, DBusMessageIter *);
    void (*it_init_append)(DBusMessage *, DBusMessageIter *);
    dbus_bool_t (*it_append)(DBusMessageIter *, int, const void *);
    dbus_bool_t (*it_open)(DBusMessageIter *, int, const char *, DBusMessageIter *);
    dbus_bool_t (*it_close)(DBusMessageIter *, DBusMessageIter *);
    int (*it_type)(DBusMessageIter *);
    void (*it_get)(DBusMessageIter *, void *);
    void (*it_recurse)(DBusMessageIter *, DBusMessageIter *);
    dbus_bool_t (*it_next)(DBusMessageIter *);
} D;
static bool d_loaded;
static pthread_once_t d_once = PTHREAD_ONCE_INIT;

static bool sym(void *h, const char *name, void *slot) {
    void *p = dlsym(h, name);
    if (!p) return false;
    memcpy(slot, &p, sizeof p);                  /* указатель на объект -> указатель на функцию без предупреждений */
    return true;
}
static void d_load_once(void) {
    void *h = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    bool ok = sym(h, "dbus_error_init", &D.error_init) && sym(h, "dbus_error_free", &D.error_free)
        && sym(h, "dbus_bus_get_private", &D.bus_get_private) && sym(h, "dbus_bus_get_unique_name", &D.bus_unique)
        && sym(h, "dbus_bus_add_match", &D.add_match) && sym(h, "dbus_bus_remove_match", &D.remove_match)
        && sym(h, "dbus_connection_set_exit_on_disconnect", &D.set_exit) && sym(h, "dbus_connection_close", &D.conn_close)
        && sym(h, "dbus_connection_unref", &D.conn_unref) && sym(h, "dbus_connection_read_write", &D.read_write)
        && sym(h, "dbus_connection_pop_message", &D.pop) && sym(h, "dbus_connection_send_with_reply_and_block", &D.send_block)
        && sym(h, "dbus_connection_send", &D.send) && sym(h, "dbus_connection_flush", &D.flush)
        && sym(h, "dbus_connection_can_send_type", &D.can_send) && sym(h, "dbus_connection_get_is_connected", &D.connected)
        && sym(h, "dbus_message_new_method_call", &D.new_call) && sym(h, "dbus_message_new_signal", &D.new_signal)
        && sym(h, "dbus_message_unref", &D.msg_unref) && sym(h, "dbus_message_is_signal", &D.is_signal)
        && sym(h, "dbus_message_get_path", &D.get_path) && sym(h, "dbus_message_get_sender", &D.get_sender)
        && sym(h, "dbus_message_has_signature", &D.has_sig) && sym(h, "dbus_message_iter_init", &D.it_init)
        && sym(h, "dbus_message_iter_init_append", &D.it_init_append) && sym(h, "dbus_message_iter_append_basic", &D.it_append)
        && sym(h, "dbus_message_iter_open_container", &D.it_open) && sym(h, "dbus_message_iter_close_container", &D.it_close)
        && sym(h, "dbus_message_iter_get_arg_type", &D.it_type) && sym(h, "dbus_message_iter_get_basic", &D.it_get)
        && sym(h, "dbus_message_iter_recurse", &D.it_recurse) && sym(h, "dbus_message_iter_next", &D.it_next);
    if (!ok) { memset(&D, 0, sizeof D); dlclose(h); return; }
    dbus_bool_t (*thr)(void) = NULL;             /* libdbus < 1.7: потоки включаются явно; новее - сами */
    if (sym(h, "dbus_threads_init_default", &thr)) thr();
    d_loaded = true;                             /* библиотека остаётся загруженной до выхода: выгружать libdbus нельзя */
}
static bool d_load(void) { pthread_once(&d_once, d_load_once); return d_loaded; }

/* ---- чистые функции (проверяются в --selftest без шины) ---- */
#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_OBJ "/org/freedesktop/portal/desktop"
#define IF_SC "org.freedesktop.portal.ScreenCast"
#define IF_REQ "org.freedesktop.portal.Request"
#define IF_SES "org.freedesktop.portal.Session"
#define REQ_PREFIX "/org/freedesktop/portal/desktop/request/"
#define SES_PREFIX "/org/freedesktop/portal/desktop/session/"

static bool name_ch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
/* синтаксис объектного пути D-Bus: "/" или /эл/эл, элементы [A-Za-z0-9_]+ */
static bool objpath_ok(const char *p) {
    if (!p || p[0] != '/') return false;
    size_t n = strlen(p);
    if (n > 255) return false;
    if (n == 1) return true;
    if (p[n - 1] == '/') return false;
    for (size_t i = 1; i < n; i++) {
        if (p[i] == '/') { if (p[i - 1] == '/') return false; }
        else if (!name_ch(p[i])) return false;
    }
    return true;
}
/* путь запроса: REQ_PREFIX + имя соединения без ':' и с '.'->'_' + '/' + токен (как в xdg-desktop-portal) */
static bool req_path(const char *uniq, const char *token, char *out, size_t n) {
    if (!uniq || uniq[0] != ':' || !uniq[1] || !token || !token[0] || strlen(token) > 64 || strlen(uniq) > 64) return false;
    for (const char *t = token; *t; t++) if (!name_ch(*t)) return false;
    char s[72];
    size_t j = 0;
    for (const char *u = uniq + 1; *u; u++) {
        if (*u == '.') s[j++] = '_';
        else if (name_ch(*u)) s[j++] = *u;
        else return false;                       /* '-' и прочее в путь не ложится */
    }
    s[j] = 0;
    int k = snprintf(out, n, REQ_PREFIX "%s/%s", s, token);
    return k > 0 && (size_t)k < n && objpath_ok(out);
}
static void make_token(char *out, size_t n) {
    static unsigned cnt;
    unsigned char r[8];
    if (getrandom(r, sizeof r, GRND_NONBLOCK) != (ssize_t)sizeof r) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned long long v = (unsigned long long)ts.tv_nsec ^ ((unsigned long long)ts.tv_sec << 30) ^ ((unsigned long long)getpid() << 16)
                               ^ __atomic_add_fetch(&cnt, 1, __ATOMIC_RELAXED);
        memcpy(r, &v, sizeof r);
    }
    snprintf(out, n, "astra_%02x%02x%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}
static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- разбор ответов (строгие типы) ---- */
/* Response(u код, a{sv} результаты): -1 - не та подпись; иначе код (0 ок, 1 отмена, 2 прочее) */
static int resp_code(DBusMessage *m, DBusMessageIter *res) {
    DBusMessageIter it;
    uint32_t code = 0;
    if (!D.has_sig(m, "ua{sv}") || !D.it_init(m, &it) || D.it_type(&it) != T_U32) return -1;
    D.it_get(&it, &code);
    if (!D.it_next(&it) || D.it_type(&it) != T_ARRAY) return -1;
    D.it_recurse(&it, res);
    return code > 2 ? 2 : (int)code;
}
/* a{sv}: значение ключа (содержимое variant) */
static bool dict_find(const DBusMessageIter *arr, const char *key, DBusMessageIter *val) {
    DBusMessageIter a = *arr;
    for (int i = 0; i < 256 && D.it_type(&a) == T_DICT; i++) {
        DBusMessageIter e;
        D.it_recurse(&a, &e);
        const char *k = NULL;
        if (D.it_type(&e) == T_STR) {
            D.it_get(&e, &k);
            if (k && !strcmp(k, key) && D.it_next(&e) && D.it_type(&e) == T_VAR) { D.it_recurse(&e, val); return true; }
        }
        if (!D.it_next(&a)) break;
    }
    return false;
}
static bool dict_u32(const DBusMessageIter *arr, const char *key, uint32_t *v) {
    DBusMessageIter x;
    if (!dict_find(arr, key, &x) || D.it_type(&x) != T_U32) return false;
    D.it_get(&x, v);
    return true;
}
/* session_handle: строка (по спецификации s; некоторые порталы шлют o), путь сессии портала */
static bool parse_session(const DBusMessageIter *res, char *out, size_t n) {
    DBusMessageIter x;
    const char *s = NULL;
    if (!dict_find(res, "session_handle", &x) || (D.it_type(&x) != T_STR && D.it_type(&x) != T_PATH)) return false;
    D.it_get(&x, &s);
    if (!objpath_ok(s) || strncmp(s, SES_PREFIX, sizeof SES_PREFIX - 1) || strlen(s) >= n) return false;
    memcpy(out, s, strlen(s) + 1);
    return true;
}
/* streams a(ua{sv}): первый поток - узел, size (ii), source_type u. Размер вне 1..16384 - «не сообщён». */
static bool parse_streams(const DBusMessageIter *res, PortalStream *ps) {
    DBusMessageIter x, a, st, props;
    if (!dict_find(res, "streams", &x) || D.it_type(&x) != T_ARRAY) return false;
    D.it_recurse(&x, &a);
    if (D.it_type(&a) != T_STRUCT) return false;
    D.it_recurse(&a, &st);
    uint32_t node = 0;
    if (D.it_type(&st) != T_U32) return false;
    D.it_get(&st, &node);
    if (node == 0 || node == 0xFFFFFFFFu) return false;
    ps->node = node;
    ps->w = ps->h = ps->source_type = 0;
    if (!D.it_next(&st) || D.it_type(&st) != T_ARRAY) return true;   /* без свойств - только узел */
    D.it_recurse(&st, &props);
    DBusMessageIter sz, f;
    if (dict_find(&props, "size", &sz) && D.it_type(&sz) == T_STRUCT) {
        int32_t w = 0, h = 0;
        D.it_recurse(&sz, &f);
        if (D.it_type(&f) == T_I32) {
            D.it_get(&f, &w);
            if (D.it_next(&f) && D.it_type(&f) == T_I32) {
                D.it_get(&f, &h);
                if (w > 0 && h > 0 && w <= 16384 && h <= 16384) { ps->w = w; ps->h = h; }
            }
        }
    }
    uint32_t t = 0;
    if (dict_u32(&props, "source_type", &t) && (t == 1 || t == 2 || t == 4)) ps->source_type = (int)t;
    return true;
}
/* ответ OpenPipeWireRemote: ровно (h); libdbus отдаёт свою копию дескриптора - она наша */
static int parse_fd(DBusMessage *m) {
    DBusMessageIter it;
    int fd = -1;
    if (!D.has_sig(m, "h") || !D.it_init(m, &it) || D.it_type(&it) != T_FD) return -1;
    D.it_get(&it, &fd);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFD);
    if (fl < 0) return -1;
    fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    return fd;
}
/* ответ Properties.Get: ровно (v) с u внутри */
static bool parse_prop_u32(DBusMessage *m, uint32_t *v) {
    DBusMessageIter it, x;
    if (!D.has_sig(m, "v") || !D.it_init(m, &it)) return false;
    D.it_recurse(&it, &x);
    if (D.it_type(&x) != T_U32) return false;
    D.it_get(&x, v);
    return true;
}

/* ---- построение a{sv} ---- */
static bool dict_put(DBusMessageIter *arr, const char *key, int type, const void *v) {
    char sig[2] = { (char)type, 0 };
    DBusMessageIter e, var;
    if (!D.it_open(arr, T_DICT, NULL, &e)) return false;
    bool ok = D.it_append(&e, T_STR, &key);
    if (ok && (ok = D.it_open(&e, T_VAR, sig, &var))) {
        ok = D.it_append(&var, type, v);
        ok = D.it_close(&e, &var) && ok;
    }
    return D.it_close(arr, &e) && ok;
}

/* ---- сессия ---- */
static DBusConnection *g_conn;
static char g_owner[128];                        /* уникальное имя портала: сигналы принимаются только от него */
static char g_session[256];
static char g_closed_rule[512];
static bool g_closed;

static DBusConnection *bus_open(void) {
    DBusError e;
    D.error_init(&e);
    DBusConnection *c = D.bus_get_private(BUS_SESSION, &e);
    D.error_free(&e);
    if (c) D.set_exit(c, 0);                     /* по умолчанию libdbus делает _exit при обрыве шины */
    return c;
}
static void bus_drop(DBusConnection *c) { D.conn_close(c); D.conn_unref(c); }

/* Ошибка D-Bus, после которой стоит один раз повторить весь запрос: бэкенд портала только что упал
 * и перезапускается (xdg-desktop-portal-gnome 46 падает, если сессию закрыть при открытом окне выбора;
 * первые секунды после этого - «No such interface ...ScreenCast» и подобные). */
static bool err_retryable(const char *name) {
    static const char *const R[] = { "UnknownInterface", "UnknownMethod", "UnknownObject", "ServiceUnknown", "NoReply", "NameHasNoOwner" };
    static const char pfx[] = "org.freedesktop.DBus.Error.";
    if (!name || strncmp(name, pfx, sizeof pfx - 1)) return false;
    for (size_t i = 0; i < sizeof R / sizeof *R; i++) if (!strcmp(name + sizeof pfx - 1, R[i])) return true;
    return false;
}
static char g_call_err[96];                      /* имя ошибки последнего call() (пусто - ответ пришёл) */
static bool g_retry;                             /* portal_wl_open: отказ похож на перезапуск бэкенда - повторить */
static long long g_resp_ms;                      /* request(): сколько ждали Response после вызова метода */

/* вызов с ответом; msg освобождается здесь */
static DBusMessage *call(DBusMessage *m, int ms) {
    g_call_err[0] = 0;
    if (!m) return NULL;
    DBusError e;
    D.error_init(&e);
    /* plat_posix.c делает процесс недампящимся (PR_SET_DUMPABLE 0): тогда /proc/<pid>/... принадлежит root,
     * и xdg-desktop-portal не может определить приложение вызывающего (root, cgroup) - вызов падает.
     * Включаем 1 только на время синхронного вызова метода (портал определяет приложение при вызове),
     * не во время ожидания Response и системного окна: в памяти токен, ptrace того же пользователя. */
    int was = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    if (was == 0) prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    DBusMessage *r = D.send_block(g_conn, m, ms, &e);
    if (was == 0) prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    if (!r && e.name) snprintf(g_call_err, sizeof g_call_err, "%s", e.name);
    D.msg_unref(m);
    D.error_free(&e);
    return r;
}
static bool get_prop(const char *name, uint32_t *v) {
    DBusMessage *m = D.new_call(PORTAL_BUS, PORTAL_OBJ, "org.freedesktop.DBus.Properties", "Get");
    if (!m) return false;
    const char *ifc = IF_SC;
    DBusMessageIter it;
    D.it_init_append(m, &it);
    if (!D.it_append(&it, T_STR, &ifc) || !D.it_append(&it, T_STR, &name)) { D.msg_unref(m); return false; }
    DBusMessage *r = call(m, 25000);             /* первый вызов может запустить портал (активация по шине) */
    if (!r) return false;
    bool ok = parse_prop_u32(r, v);
    D.msg_unref(r);
    return ok;
}
static bool get_owner(void) {
    DBusMessage *m = D.new_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
    if (!m) return false;
    const char *nm = PORTAL_BUS;
    DBusMessageIter it;
    D.it_init_append(m, &it);
    if (!D.it_append(&it, T_STR, &nm)) { D.msg_unref(m); return false; }
    DBusMessage *r = call(m, 5000);
    if (!r) return false;
    const char *s = NULL;
    bool ok = D.has_sig(r, "s") && D.it_init(r, &it) && D.it_type(&it) == T_STR;
    if (ok) D.it_get(&it, &s);
    ok = ok && s && s[0] == ':' && strlen(s) < sizeof g_owner;
    if (ok) memcpy(g_owner, s, strlen(s) + 1);
    D.msg_unref(r);
    return ok;
}
static void send_noreply(const char *path, const char *ifc, const char *meth) {
    if (!g_conn || !objpath_ok(path)) return;
    DBusMessage *m = D.new_call(PORTAL_BUS, path, ifc, meth);
    if (!m) return;
    D.send(g_conn, m, NULL);
    D.msg_unref(m);
    D.flush(g_conn);
}
/* сообщение из очереди: Session::Closed отмечаем, остальное (кроме искомого Response) - выбросить */
static bool is_closed_sig(DBusMessage *m) {
    const char *p = D.get_path(m), *s = D.get_sender(m);
    return g_session[0] && D.is_signal(m, IF_SES, "Closed") && p && !strcmp(p, g_session) && s && !strcmp(s, g_owner);
}

enum { WR_OK, WR_STOP, WR_TIMEOUT, WR_LOST };
static int wait_response(const char *p1, const char *p2, volatile int *stop, int timeout_ms, DBusMessage **out) {
    long long end = mono_ms() + timeout_ms;
    for (;;) {
        DBusMessage *m;
        while ((m = D.pop(g_conn))) {
            const char *p = D.get_path(m), *s = D.get_sender(m);
            if (D.is_signal(m, IF_REQ, "Response") && p && (!strcmp(p, p1) || (p2[0] && !strcmp(p, p2))) && s && !strcmp(s, g_owner)) {
                *out = m;
                return WR_OK;
            }
            if (is_closed_sig(m)) g_closed = true;
            D.msg_unref(m);
        }
        if (stop && *stop) return WR_STOP;
        if (g_closed) return WR_LOST;             /* компоновщик закрыл сессию посреди выбора */
        if (mono_ms() >= end) return WR_TIMEOUT;
        if (!D.read_write(g_conn, 100)) return WR_LOST;
    }
}
static void match_rule(char *out, size_t n, const char *ifc, const char *member, const char *path) {
    snprintf(out, n, "type='signal',sender='%s',interface='%s',member='%s',path='%s'", g_owner, ifc, member, path);
}
static void err_set(char *err, size_t n, const char *s) { if (err && n) snprintf(err, n, "%s", s); }

/* Метод с Request: подписка на Response до вызова, вызов, ожидание. Возврат - Response или NULL (err заполнен). */
static DBusMessage *request(DBusMessage *m, const char *token, volatile int *stop, int wait_ms, char *err, size_t n) {
    char path[256], handle[256] = "", rule[512], rule2[512] = "";
    if (!m) { err_set(err, n, ui_tr("Wayland: неверный ответ портала")); return NULL; }
    if (!req_path(D.bus_unique(g_conn), token, path, sizeof path)) {
        D.msg_unref(m);
        err_set(err, n, ui_tr("Wayland: неверный ответ портала"));
        return NULL;
    }
    match_rule(rule, sizeof rule, IF_REQ, "Response", path);
    DBusError e;
    D.error_init(&e);
    D.add_match(g_conn, rule, &e);
    bool bad = e.name != NULL;
    D.error_free(&e);
    if (bad) { D.msg_unref(m); err_set(err, n, ui_tr("Wayland: нет портала демонстрации экрана (xdg-desktop-portal)")); return NULL; }
    long long t0 = mono_ms();
    DBusMessage *r = call(m, 30000);
    DBusMessage *resp = NULL;
    if (!r) {
        err_set(err, n, ui_tr("Wayland: портал отказал в демонстрации экрана"));
        if (err_retryable(g_call_err)) g_retry = true;
    } else {
        DBusMessageIter it;
        const char *h = NULL;
        if (D.has_sig(r, "o") && D.it_init(r, &it) && D.it_type(&it) == T_PATH) D.it_get(&it, &h);
        if (h && objpath_ok(h) && !strncmp(h, REQ_PREFIX, sizeof REQ_PREFIX - 1) && strlen(h) < sizeof handle) memcpy(handle, h, strlen(h) + 1);
        D.msg_unref(r);
        if (!handle[0]) err_set(err, n, ui_tr("Wayland: неверный ответ портала"));
        else {
            if (strcmp(handle, path)) {          /* старые порталы (< 0.9): путь не по токену - ловим и его */
                match_rule(rule2, sizeof rule2, IF_REQ, "Response", handle);
                D.add_match(g_conn, rule2, NULL);
            } else handle[0] = 0;
            int w = wait_response(path, handle, stop, wait_ms, &resp);
            g_resp_ms = mono_ms() - t0;
            if (w != WR_OK) {
                send_noreply(handle[0] ? handle : path, IF_REQ, "Close");   /* закрыть системное окно выбора */
                err_set(err, n, w == WR_STOP ? ui_tr("трансляция остановлена")
                              : w == WR_TIMEOUT ? ui_tr("Wayland: портал не ответил вовремя")
                              : ui_tr("Wayland: соединение с порталом оборвалось"));
            }
        }
    }
    D.remove_match(g_conn, rule, NULL);
    if (rule2[0]) D.remove_match(g_conn, rule2, NULL);
    return resp;
}
/* новый вызов ScreenCast: (o сессия)? + a{sv} с handle_token и опциями; it_opts - открытый словарь */
static DBusMessage *sc_call(const char *meth, bool with_session, const char *parent, char *token, size_t tn,
                            DBusMessageIter *it, DBusMessageIter *opts) {
    DBusMessage *m = D.new_call(PORTAL_BUS, PORTAL_OBJ, IF_SC, meth);
    if (!m) return NULL;
    D.it_init_append(m, it);
    const char *ses = g_session;
    bool ok = !with_session || D.it_append(it, T_PATH, &ses);
    if (ok && parent) ok = D.it_append(it, T_STR, &parent);
    if (ok) ok = D.it_open(it, T_ARRAY, "{sv}", opts);
    if (ok && token) {
        make_token(token, tn);
        const char *t = token;
        if (!dict_put(opts, "handle_token", T_STR, &t)) { D.it_close(it, opts); ok = false; }
    }
    if (!ok) { D.msg_unref(m); return NULL; }
    return m;
}
static bool sc_end(DBusMessage *m, DBusMessageIter *it, DBusMessageIter *opts, bool ok) {
    ok = D.it_close(it, opts) && ok;
    if (!ok) D.msg_unref(m);
    return ok;
}
/* Response обработан: код 0 - ок; иначе err и false. Код 2 быстрее 2 с - отказ до показа окна выбора
 * (фронтенд так отвечает, когда вызов бэкенда не прошёл): повторяемо. Код 1 (отмена) - нет. */
static bool resp_ok(DBusMessage *r, DBusMessageIter *res, char *err, size_t n) {
    int c = resp_code(r, res);
    if (c == 0) return true;
    if (c == 2 && g_resp_ms < 2000) g_retry = true;
    err_set(err, n, c == 1 ? ui_tr("выбор отменён") : c == 2 ? ui_tr("системное окно выбора не сработало") : ui_tr("Wayland: неверный ответ портала"));
    return false;
}

bool portal_wl_available(void) {
    const char *wd = getenv("WAYLAND_DISPLAY"), *st = getenv("XDG_SESSION_TYPE");
    if (!((wd && wd[0]) || (st && !strcmp(st, "wayland")))) return false;
    return d_load();
}

bool portal_wl_closed(void) {
    if (!g_conn || g_closed) return true;
    if (!D.read_write(g_conn, 0) || !D.connected(g_conn)) { g_closed = true; return true; }
    DBusMessage *m;
    while ((m = D.pop(g_conn))) {
        if (is_closed_sig(m)) g_closed = true;
        D.msg_unref(m);
    }
    return g_closed;
}

void portal_wl_close(void) {
    if (!g_conn) { g_session[0] = 0; return; }
    if (g_session[0] && !g_closed) send_noreply(g_session, IF_SES, "Close");
    if (g_closed_rule[0]) D.remove_match(g_conn, g_closed_rule, NULL);
    bus_drop(g_conn);
    g_conn = NULL;
    g_session[0] = g_owner[0] = g_closed_rule[0] = 0;
    g_closed = false;
}

#define WAIT_QUICK 30000                         /* CreateSession: окна нет */
#define WAIT_USER  600000                        /* SelectSources/Start: пользователь выбирает в системном окне */
static bool open_once(PortalStream *out, volatile int *stop, char *err, size_t n) {
    out->pw_fd = -1;
    out->node = 0;
    out->w = out->h = out->source_type = 0;
    if (!d_load()) { err_set(err, n, ui_tr("Wayland: нет библиотеки libdbus-1 (пакет libdbus-1-3)")); return false; }
    portal_wl_close();                           /* прежняя сессия, если её не закрыли */
    if (!(g_conn = bus_open())) { err_set(err, n, ui_tr("Wayland: нет связи с сеансовой шиной D-Bus")); return false; }
    if (!D.can_send(g_conn, T_FD)) { err_set(err, n, ui_tr("Wayland: шина D-Bus не передаёт дескрипторы файлов")); goto fail; }
    uint32_t types = 0, cursors = 0;
    if (!get_prop("AvailableSourceTypes", &types) || !get_owner()) {
        err_set(err, n, ui_tr("Wayland: нет портала демонстрации экрана (xdg-desktop-portal)"));
        goto fail;
    }
    bool have_cursors = get_prop("AvailableCursorModes", &cursors);   /* с версии 2 интерфейса */
    if (stop && *stop) { err_set(err, n, ui_tr("трансляция остановлена")); goto fail; }

    char token[32], stoken[32];
    DBusMessageIter it, opts, res;
    /* 1. CreateSession */
    DBusMessage *m = sc_call("CreateSession", false, NULL, token, sizeof token, &it, &opts);
    if (m) {
        make_token(stoken, sizeof stoken);
        const char *st = stoken;
        if (!sc_end(m, &it, &opts, dict_put(&opts, "session_handle_token", T_STR, &st))) m = NULL;
    }
    DBusMessage *r = request(m, token, stop, WAIT_QUICK, err, n);
    if (!r) goto fail;
    bool ok = resp_ok(r, &res, err, n);
    if (ok && !(ok = parse_session(&res, g_session, sizeof g_session))) err_set(err, n, ui_tr("Wayland: неверный ответ портала"));
    D.msg_unref(r);
    if (!ok) { g_session[0] = 0; goto fail; }
    match_rule(g_closed_rule, sizeof g_closed_rule, IF_SES, "Closed", g_session);
    D.add_match(g_conn, g_closed_rule, NULL);

    /* 2. SelectSources: монитор и окно (что есть), один источник, курсор в кадре, без сохранения выбора */
    uint32_t want = types & 3u;
    if (!want) want = 1;
    uint32_t cmode = (cursors & 2u) ? 2u : 1u, persist = 0;
    dbus_bool_t multi = 0;
    m = sc_call("SelectSources", true, NULL, token, sizeof token, &it, &opts);
    if (m) {
        ok = dict_put(&opts, "types", T_U32, &want) && dict_put(&opts, "multiple", T_BOOL, &multi)
             && (!have_cursors || dict_put(&opts, "cursor_mode", T_U32, &cmode)) && dict_put(&opts, "persist_mode", T_U32, &persist);
        if (!sc_end(m, &it, &opts, ok)) m = NULL;
    }
    if (!(r = request(m, token, stop, WAIT_USER, err, n))) goto fail;
    ok = resp_ok(r, &res, err, n);
    D.msg_unref(r);
    if (!ok) goto fail;

    /* 3. Start: здесь GNOME/KDE показывают окно выбора */
    m = sc_call("Start", true, "", token, sizeof token, &it, &opts);
    if (m && !sc_end(m, &it, &opts, true)) m = NULL;
    if (!(r = request(m, token, stop, WAIT_USER, err, n))) goto fail;
    ok = resp_ok(r, &res, err, n);
    if (ok && !(ok = parse_streams(&res, out))) err_set(err, n, ui_tr("Wayland: портал не дал поток экрана"));
    D.msg_unref(r);
    if (!ok) goto fail;

    /* 4. OpenPipeWireRemote -> fd */
    m = sc_call("OpenPipeWireRemote", true, NULL, NULL, 0, &it, &opts);
    if (m && !sc_end(m, &it, &opts, true)) m = NULL;
    if (!(r = call(m, 30000))) { err_set(err, n, ui_tr("Wayland: портал не дал доступ к PipeWire")); goto fail; }
    out->pw_fd = parse_fd(r);
    D.msg_unref(r);                              /* закрывает копию libdbus; наша (dup) остаётся */
    if (out->pw_fd < 0) { err_set(err, n, ui_tr("Wayland: портал не дал доступ к PipeWire")); goto fail; }
    if (stop && *stop) { close(out->pw_fd); out->pw_fd = -1; err_set(err, n, ui_tr("трансляция остановлена")); goto fail; }
    return true;
fail:
    out->node = 0;
    out->w = out->h = out->source_type = 0;
    portal_wl_close();
    return false;
}
bool portal_wl_open(PortalStream *out, volatile int *stop, char *err, size_t n) {
    g_retry = false;
    if (open_once(out, stop, err, n)) return true;
    if (!g_retry || (stop && *stop)) return false;
    /* один повтор через ~1,5 с: бэкенд портала перезапускается после падения */
    for (int i = 0; i < 15; i++) {
        if (stop && *stop) { err_set(err, n, ui_tr("трансляция остановлена")); return false; }
        struct timespec ts = { 0, 100 * 1000000L };
        nanosleep(&ts, NULL);
    }
    if (stop && *stop) { err_set(err, n, ui_tr("трансляция остановлена")); return false; }
    g_retry = false;
    return open_once(out, stop, err, n);
}

/* ---- --selftest ---- */
static DBusMessage *st_resp(uint32_t code, DBusMessageIter *it, DBusMessageIter *arr) {
    DBusMessage *m = D.new_signal("/org/freedesktop/portal/desktop/request/1_5/t", IF_REQ, "Response");
    if (!m) return NULL;
    D.it_init_append(m, it);
    D.it_append(it, T_U32, &code);
    D.it_open(it, T_ARRAY, "{sv}", arr);
    return m;
}
/* streams: [(node, {size: (w,h), source_type: t})]; size_u - size не той формы (u) */
static DBusMessage *st_streams(uint32_t node, int32_t w, int32_t h, uint32_t t, bool size_u) {
    DBusMessageIter it, arr, e, v, a, s, p, pe, pv, sz;
    DBusMessage *m = st_resp(0, &it, &arr);
    if (!m) return NULL;
    const char *k = "streams", *ks = "size", *kt = "source_type";
    D.it_open(&arr, T_DICT, NULL, &e);
    D.it_append(&e, T_STR, &k);
    D.it_open(&e, T_VAR, "a(ua{sv})", &v);
    D.it_open(&v, T_ARRAY, "(ua{sv})", &a);
    D.it_open(&a, T_STRUCT, NULL, &s);
    D.it_append(&s, T_U32, &node);
    D.it_open(&s, T_ARRAY, "{sv}", &p);
    D.it_open(&p, T_DICT, NULL, &pe);
    D.it_append(&pe, T_STR, &ks);
    if (size_u) {
        uint32_t u = 640;
        D.it_open(&pe, T_VAR, "u", &pv);
        D.it_append(&pv, T_U32, &u);
    } else {
        D.it_open(&pe, T_VAR, "(ii)", &pv);
        D.it_open(&pv, T_STRUCT, NULL, &sz);
        D.it_append(&sz, T_I32, &w);
        D.it_append(&sz, T_I32, &h);
        D.it_close(&pv, &sz);
    }
    D.it_close(&pe, &pv);
    D.it_close(&p, &pe);
    dict_put(&p, kt, T_U32, &t);
    D.it_close(&s, &p);
    D.it_close(&a, &s);
    D.it_close(&v, &a);
    D.it_close(&e, &v);
    D.it_close(&arr, &e);
    D.it_close(&it, &arr);
    return m;
}
int portal_wl_selftest(void (*check)(bool ok, const char *what)) {
    char p[256];
    check(req_path(":1.42", "astra_00ff", p, sizeof p) && !strcmp(p, REQ_PREFIX "1_42/astra_00ff"),
          "портал: путь запроса из имени шины (:1.42 -> 1_42/токен)");
    check(req_path(":1.2.3", "t", p, sizeof p) && !strcmp(p, REQ_PREFIX "1_2_3/t"), "портал: путь запроса, несколько точек");
    check(!req_path("1.42", "t", p, sizeof p) && !req_path(":", "t", p, sizeof p) && !req_path(":1.4-2", "t", p, sizeof p)
          && !req_path(":1.42", "", p, sizeof p) && !req_path(":1.42", "a/b", p, sizeof p) && !req_path(":1.42", "a.b", p, sizeof p)
          && !req_path(NULL, "t", p, sizeof p) && !req_path(":1.42", "t", p, 20),
          "портал: путь запроса - отказ на имя без ':', '-', пустой/чужой токен, короткий буфер");
    check(objpath_ok("/") && objpath_ok("/a/b_1") && !objpath_ok("") && !objpath_ok("a") && !objpath_ok("/a/") && !objpath_ok("//a")
          && !objpath_ok("/a//b") && !objpath_ok("/a.b") && !objpath_ok("/a/../b") && !objpath_ok(NULL),
          "портал: синтаксис объектного пути");
    check(err_retryable("org.freedesktop.DBus.Error.UnknownInterface") && err_retryable("org.freedesktop.DBus.Error.UnknownMethod")
          && err_retryable("org.freedesktop.DBus.Error.ServiceUnknown") && err_retryable("org.freedesktop.DBus.Error.NoReply")
          && !err_retryable("org.freedesktop.DBus.Error.AccessDenied") && !err_retryable("org.freedesktop.portal.Error.Failed")
          && !err_retryable("org.freedesktop.DBus.Error.UnknownInterfaceX") && !err_retryable("UnknownMethod")
          && !err_retryable("org.freedesktop.DBus.Error.") && !err_retryable("") && !err_retryable(NULL),
          "портал: повтор запроса только на ошибки перезапуска бэкенда (UnknownInterface/Method, ServiceUnknown, NoReply)");
    char t1[32], t2[32];
    make_token(t1, sizeof t1);
    make_token(t2, sizeof t2);
    check(strlen(t1) == 22 && strcmp(t1, t2) && req_path(":1.1", t1, p, sizeof p), "портал: токены запроса разные и годятся для пути");
    {   /* available без Wayland - false при любом наличии libdbus */
        const char *wd = getenv("WAYLAND_DISPLAY"), *st = getenv("XDG_SESSION_TYPE");
        char *swd = wd ? strdup(wd) : NULL, *sst = st ? strdup(st) : NULL;
        unsetenv("WAYLAND_DISPLAY");
        setenv("XDG_SESSION_TYPE", "x11", 1);
        bool a = portal_wl_available();
        if (swd) setenv("WAYLAND_DISPLAY", swd, 1);
        if (sst) setenv("XDG_SESSION_TYPE", sst, 1); else unsetenv("XDG_SESSION_TYPE");
        free(swd); free(sst);
        check(!a, "портал: вне сеанса Wayland portal_wl_available() == false");
    }
    check(portal_wl_closed(), "портал: без сессии portal_wl_closed() == true");
    portal_wl_close();
    portal_wl_close();
    check(true, "портал: portal_wl_close без сессии, дважды - без сбоя");
    if (!d_load()) { term_puts("  (портал: libdbus-1 нет - разбор ответов не проверен)\n"); return 0; }

    DBusMessageIter it, arr, res;
    char ses[256];
    /* Response с session_handle */
    const char *sh = SES_PREFIX "1_42/astra_1", *bad = "/etc/passwd";
    DBusMessage *m = st_resp(0, &it, &arr);
    dict_put(&arr, "session_handle", T_STR, &sh);
    D.it_close(&it, &arr);
    check(m && resp_code(m, &res) == 0 && parse_session(&res, ses, sizeof ses) && !strcmp(ses, sh), "портал: Response 0 + session_handle (s)");
    check(m && resp_code(m, &res) == 0 && !parse_session(&res, ses, 10), "портал: session_handle длиннее буфера - отказ");
    if (m) D.msg_unref(m);
    m = st_resp(0, &it, &arr);
    dict_put(&arr, "session_handle", T_PATH, &bad);
    D.it_close(&it, &arr);
    check(m && resp_code(m, &res) == 0 && !parse_session(&res, ses, sizeof ses), "портал: session_handle не путь сессии портала - отказ");
    if (m) D.msg_unref(m);
    uint32_t one = 1;
    m = st_resp(1, &it, &arr);
    dict_put(&arr, "session_handle", T_U32, &one);
    D.it_close(&it, &arr);
    check(m && resp_code(m, &res) == 1 && !parse_session(&res, ses, sizeof ses), "портал: Response 1 (отмена); session_handle типа u - отказ");
    if (m) D.msg_unref(m);
    m = st_resp(77, &it, &arr);
    D.it_close(&it, &arr);
    check(m && resp_code(m, &res) == 2, "портал: неизвестный код ответа -> 2 (ошибка)");
    if (m) D.msg_unref(m);
    m = D.new_signal("/x", IF_REQ, "Response");
    if (m) { const char *s = "x"; D.it_init_append(m, &it); D.it_append(&it, T_STR, &s); }
    check(m && resp_code(m, &res) == -1, "портал: Response с чужой подписью (s) - отказ");
    if (m) D.msg_unref(m);
    /* streams */
    PortalStream ps;
    m = st_streams(57, 1920, 1080, 1, false);
    check(m && resp_code(m, &res) == 0 && parse_streams(&res, &ps) && ps.node == 57 && ps.w == 1920 && ps.h == 1080 && ps.source_type == 1,
          "портал: streams [(57, size 1920x1080, монитор)]");
    if (m) D.msg_unref(m);
    m = st_streams(58, -5, 100000, 9, false);
    check(m && resp_code(m, &res) == 0 && parse_streams(&res, &ps) && ps.node == 58 && !ps.w && !ps.h && !ps.source_type,
          "портал: размер вне 1..16384 и чужой source_type - «не сообщён»");
    if (m) D.msg_unref(m);
    m = st_streams(59, 0, 0, 2, true);
    check(m && resp_code(m, &res) == 0 && parse_streams(&res, &ps) && ps.node == 59 && !ps.w && ps.source_type == 2,
          "портал: size не (ii) - размер не берётся, узел и тип окна - да");
    if (m) D.msg_unref(m);
    m = st_streams(0, 640, 480, 1, false);
    check(m && resp_code(m, &res) == 0 && !parse_streams(&res, &ps), "портал: узел 0 - отказ");
    if (m) D.msg_unref(m);
    m = st_resp(0, &it, &arr);
    const char *ss = "streams";
    dict_put(&arr, "streams", T_STR, &ss);
    D.it_close(&it, &arr);
    check(m && resp_code(m, &res) == 0 && !parse_streams(&res, &ps), "портал: streams не массив - отказ");
    if (m) D.msg_unref(m);
    m = st_resp(0, &it, &arr);
    D.it_close(&it, &arr);
    check(m && resp_code(m, &res) == 0 && !parse_streams(&res, &ps), "портал: streams нет - отказ");
    if (m) D.msg_unref(m);
    /* fd: своя копия, закрывается; чужой тип - отказ без утечки */
    int pf[2];
    bool fdok = false;
    if (pipe(pf) == 0) {
        m = D.new_signal("/x", "x.y", "Z");
        if (m) {
            D.it_init_append(m, &it);
            D.it_append(&it, T_FD, &pf[0]);
            int fd = parse_fd(m);
            D.msg_unref(m);
            fdok = fd >= 0 && fd != pf[0] && fd != pf[1] && (fcntl(fd, F_GETFD) & FD_CLOEXEC);
            if (fd >= 0) close(fd);
        }
        close(pf[0]);
        close(pf[1]);
    }
    check(fdok, "портал: OpenPipeWireRemote (h) - своя копия fd с FD_CLOEXEC");
    m = D.new_signal("/x", "x.y", "Z");
    if (m) { D.it_init_append(m, &it); D.it_append(&it, T_U32, &one); }
    check(m && parse_fd(m) == -1, "портал: ответ OpenPipeWireRemote не (h) - отказ");
    if (m) D.msg_unref(m);
    uint32_t v = 0;
    m = D.new_signal("/x", "x.y", "Z");
    if (m) {
        DBusMessageIter var;
        D.it_init_append(m, &it);
        D.it_open(&it, T_VAR, "u", &var);
        D.it_append(&var, T_U32, &one);
        D.it_close(&it, &var);
    }
    check(m && parse_prop_u32(m, &v) && v == 1, "портал: свойство (v: u)");
    if (m) D.msg_unref(m);
    m = D.new_signal("/x", "x.y", "Z");
    if (m) { D.it_init_append(m, &it); D.it_append(&it, T_U32, &one); }
    check(m && !parse_prop_u32(m, &v), "портал: свойство не variant - отказ");
    if (m) D.msg_unref(m);
    return 0;
}
#endif
