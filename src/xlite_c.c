/* xlite_c.c — туннель VLESS на чистом C (основная сборка, VLESS_IMPL=c; вариант на Go — tools/xlite).
 * Контракт для vless.c:
 *   int  XliteStart(char *cfg, int n);   0 — работает; 1 — неверные параметры; 2 — не создан
 *                                         (в т.ч. имя сервера не разрешилось); 3 — порт не открылся
 *   void XliteStop(void);                 остановить поток, затереть всё
 *   void XliteWipe(void);                 паника: затереть параметры ссылки и ключи сессий, не дожидаясь потока дольше 0,3 с
 *   int  XliteFailed(void);               1 — поток туннеля умер (порт уже закрыт), нужен перезапуск
 * cfg — строки "ключ=значение" (см. build_config в vless.c).
 *
 * Что внутри:
 *   - вход: SOCKS5 на 127.0.0.1:port, только логин/пароль (RFC 1928/1929); CONNECT (домен уходит в
 *     VLESS доменом — DNS на стороне сервера) и UDP ASSOCIATE (реле UDP на 127.0.0.1, голос). Реле
 *     принимает датаграммы только от UDP-сокета этого же процесса (Linux — /proc/net/udp и
 *     /proc/self/fd, Windows — GetExtendedUdpTable) и, если клиент указал в запросе свой адрес, —
 *     только с него; первый принятый адрес закрепляется;
 *   - выход: VLESS версии 0 (UUID, без дополнений, flow пустой), TCP = команда 1, UDP = команда 2 с
 *     пакетами «длина(2) + данные», одно соединение VLESS на пару «ассоциация — назначение»;
 *   - транспорт TCP или WebSocket (RFC 6455, клиентские кадры маскируются);
 *   - защита: Reality или TLS 1.3 — своё рукопожатие (tls13.c) с ClientHello как у Chrome на обеих ОС.
 * Имя сервера разрешается один раз в XliteStart (до запуска потока): поток никогда не ждёт DNS,
 * XliteStop не висит; новые адреса — при следующем XliteStart (переподключение в vless.c).
 *
 * Один поток туннеля. Без него нельзя, не меняя устройство vless.c: контракт — только Start/Stop, а
 * главный поток (curl) и голосовой поток сами блокируются в ожидании ответа от этого SOCKS5, так что
 * обслуживать его должен кто-то третий. Поток спит в poll/WSAPoll на всех сокетах сразу; таймеры —
 * через срок ожидания poll; пробуждение для остановки — датаграмма на свой UDP-сокет. Опроса нет.
 * Журналов нет, на диск ничего; UUID, пароль SOCKS, ключи сессий затираются; конфигурация, ключи и
 * рабочие буферы — в закреплённых страницах; буферы данных — через распределитель OpenSSL (в mincord
 * он затирает освобождаемую память). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
typedef SOCKET sock_t;
typedef WSAPOLLFD pfd_t;
typedef int slen_t;
#define SBAD INVALID_SOCKET
#define sclose closesocket
#define spoll(f, n, t) WSAPoll((f), (ULONG)(n), (t))
static bool again(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR; }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
typedef int sock_t;
typedef struct pollfd pfd_t;
typedef socklen_t slen_t;
#define SBAD (-1)
#define sclose close
#define spoll(f, n, t) poll((f), (nfds_t)(n), (t))
static bool again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
#endif
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "plat.h"
#include "tls13.h"

int XliteStart(char *cfg, int n);
void XliteStop(void);
void XliteWipe(void);
int XliteFailed(void);

typedef unsigned char u8;
typedef int64_t i64;

#define MAX_CLI 256                             /* соединений SOCKS одновременно */
#define MAX_OUT 512                             /* соединений VLESS одновременно (всего) */
#define MAX_UDP_DST 32                          /* назначений UDP на одну ассоциацию */
#define MAX_ADDR 8                              /* адресов сервера из DNS */
#define CHUNK 16000                             /* полезная нагрузка на запись TLS / кадр WS */
#define HIWAT (64 * 1024)                       /* выше — не читать источник (обратное давление) */
#define UDP_PEND_MAX (64 * 1024)                /* UDP до готовности туннеля — дальше отбрасывается */
#define T_SOCKS 10000                           /* рукопожатие SOCKS */
#define T_HS 15000                              /* подключение + TLS + WebSocket */
#define T_IDLE 300000                           /* тишина соединения (как connIdle Xray) */
#define T_LINGER 2000                           /* клиент закрыл свою сторону — ждём хвост ответа */
#define T_FLUSH 5000                            /* дописать клиенту остаток перед закрытием */
#define SIN_SZ 65536
#define SOUT_SZ (65536 + 512)
#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36"

/* ======================= конфигурация (секреты, закреплённая память) ======================= */
static struct {
    int port, rport;
    char user[256], pass[256];
    size_t ul, pl;
    u8 uuid[16];
    char host[256], sni[256];
    bool ws;
    char wshost[256], wspath[1024];
    u8 pqv[1952];
    TlsCfg tc;
} K;

/* ======================= состояние потока ======================= */
enum { CS_GREET, CS_AUTH, CS_REQ, CS_TCP, CS_UDP };
enum { OS_CONN, OS_TLS, OS_WS, OS_UP };

typedef struct Cli Cli;
typedef struct Out Out;
struct Cli {                                    /* соединение SOCKS от программы */
    Cli *next;
    sock_t fd;
    int st;
    bool dead, eof, closing;
    i64 deadline;
    Bb rin, wout;
    Out *out;                                   /* CS_TCP: его выход VLESS */
    sock_t ufd;                                 /* CS_UDP: реле */
    struct sockaddr_in peer;                    /* CS_UDP: закреплённый отправитель */
    bool have_peer;
    uint32_t want_ip;                           /* CS_UDP: адрес из запроса (0 — любой 127/8) */
    uint16_t want_port;                         /* CS_UDP: порт из запроса (0 — не указан) */
    uint16_t bad_port;                          /* последний отвергнутый порт (не проверять снова) */
    int checks;                                 /* проверок владельца в текущую секунду */
    i64 checks_t;
    int nudp;
};
struct Out {                                    /* соединение VLESS к серверу */
    Out *next;
    sock_t fd;
    int st;
    bool dead, udp;
    i64 deadline, last;
    Tls *tls;
    Bb rin, wout, plain, pend, ubuf;
    int vl;                                     /* ответ VLESS: 0 версия, 1 длина дополнений, 2 пропуск, 3 готово */
    size_t vskip;
    char wsacc[32];                             /* ожидаемый Sec-WebSocket-Accept */
    Cli *cli;
    int ai;                                     /* какой адрес сервера пробуем */
    u8 dst[1 + 1 + 255 + 2];                    /* назначение в виде SOCKS: тип, адрес, порт */
    size_t dstn;
};

static struct {
    PlatThread *thr;
    Cli *clis;
    Out *outs;
    int ncli, nout;
    i64 lsn_pause;                              /* accept упал не с EAGAIN (нет дескрипторов): не слушать до */
    struct sockaddr_storage sa[MAX_ADDR];       /* адреса сервера (разрешены в XliteStart) */
    slen_t salen[MAX_ADDR];
    int nsa, cur;
    u8 *sin, *sout;                             /* рабочие буферы потока (закреплённые страницы) */
} G;
static atomic_bool g_stop, g_exited, g_failed;
static sock_t g_lsn = SBAD, g_wrx = SBAD, g_wtx = SBAD;

static i64 now(void) { return plat_mono_ms(); }

/* ======================= сокеты ======================= */
static void set_nb(sock_t s) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}
static void set_nodelay(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}
/* Неблокирующий, не наследуемый дочерними процессами. */
static sock_t mksock(int fam, int type) {
#ifdef _WIN32
    sock_t s = WSASocketW(fam, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
    sock_t s = socket(fam, type | SOCK_CLOEXEC, 0);
#endif
    if (s != SBAD) set_nb(s);
    return s;
}
static void exclusive(sock_t s) {               /* Windows: чужой процесс не сядет на тот же адрес */
#ifdef _WIN32
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    (void)s;
#endif
}
static long s_recv(sock_t s, void *b, size_t n) {
#ifdef _WIN32
    return recv(s, (char *)b, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
#else
    return (long)recv(s, b, n, 0);
#endif
}
static long s_send(sock_t s, const void *b, size_t n) {
#ifdef _WIN32
    return send(s, (const char *)b, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
#else
    return (long)send(s, b, n, MSG_NOSIGNAL);
#endif
}
/* Отправить сколько возьмёт сокет. 0 — ок (возможно, частично), -1 — ошибка. */
static int flush(sock_t s, Bb *b) {
    while (bb_n(b)) {
        long r = s_send(s, bb_ptr(b), bb_n(b));
        if (r > 0) { bb_eat(b, (size_t)r); continue; }
        if (r < 0 && again()) return 0;
        return -1;
    }
    return 0;
}
static bool udp_loopback(sock_t *s, struct sockaddr_in *a) {
    *s = mksock(AF_INET, SOCK_DGRAM);
    if (*s == SBAD) return false;
    exclusive(*s);
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    slen_t l = sizeof *a;
    if (bind(*s, (struct sockaddr *)a, sizeof *a) || getsockname(*s, (struct sockaddr *)a, &l)) {
        sclose(*s);
        *s = SBAD;
        return false;
    }
    return true;
}

/* UDP-порт port (IPv4) принадлежит сокету этого процесса? Так реле отличает голосовой поток mincord
 * от любой другой локальной программы, приславшей датаграмму первой. */
static bool udp_port_ours(unsigned port) {
#ifdef _WIN32
    bool ok = false;
    DWORD sz = 0, me = GetCurrentProcessId();
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        MIB_UDPTABLE_OWNER_PID *t = sz ? malloc(sz) : NULL;
        DWORD r = GetExtendedUdpTable(t, &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (r == NO_ERROR && t) {
            for (DWORD i = 0; i < t->dwNumEntries; i++)
                if (ntohs((u_short)t->table[i].dwLocalPort) == port && t->table[i].dwOwningPid == me) { ok = true; break; }
            free(t);
            break;
        }
        free(t);
        if (r != ERROR_INSUFFICIENT_BUFFER) break;
    }
    return ok;
#else
    unsigned long ino[8];
    int ni = 0;
    char line[256];
    FILE *f = fopen("/proc/net/udp", "re");
    if (!f) return false;
    while (ni < 8 && fgets(line, sizeof line, f)) {
        unsigned lip, lport, rip, rport, st;
        unsigned long in = 0;
        int sl;
        if (sscanf(line, " %d: %x:%x %x:%x %x %*x:%*x %*x:%*x %*x %*u %*u %lu", &sl, &lip, &lport, &rip, &rport, &st, &in) == 7 &&
            lport == port && in)
            ino[ni++] = in;
    }
    fclose(f);
    if (!ni) return false;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return false;
    bool ok = false;
    struct dirent *e;
    while (!ok && (e = readdir(d))) {
        char path[64], tgt[64];
        snprintf(path, sizeof path, "/proc/self/fd/%.20s", e->d_name);
        ssize_t l = readlink(path, tgt, sizeof tgt - 1);
        if (l <= 0) continue;
        tgt[l] = 0;
        unsigned long in;
        if (sscanf(tgt, "socket:[%lu]", &in) == 1)
            for (int i = 0; i < ni; i++) if (ino[i] == in) ok = true;
    }
    closedir(d);
    return ok;
#endif
}

/* ======================= разбор конфигурации ======================= */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool parse_uuid(const char *s, u8 out[16]) {
    size_t n = strlen(s), o = 0;
    if (n != 36 && n != 32) return false;
    for (size_t i = 0; i < n;) {
        if (n == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) { if (s[i] != '-') return false; i++; continue; }
        int a = hexv(s[i]), b = i + 1 < n ? hexv(s[i + 1]) : -1;
        if (a < 0 || b < 0 || o >= 16) return false;
        out[o++] = (u8)(a << 4 | b);
        i += 2;
    }
    return o == 16;
}
static long b64url(const char *s, u8 *out, size_t cap) {      /* base64 без '=' (RawURLEncoding) */
    size_t n = strlen(s), o = 0;
    unsigned acc = 0;
    int bits = 0;
    if (n % 4 == 1) return -1;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-') v = 62;
        else if (c == '_') v = 63;
        else return -1;
        acc = (acc << 6 | (unsigned)v) & 0xffffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (u8)(acc >> bits);
        }
    }
    return (long)o;
}
static int parse_port(const char *s) {
    long v = 0;
    if (!*s || strlen(s) > 5) return -1;
    for (; *s; s++) { if (*s < '0' || *s > '9') return -1; v = v * 10 + (*s - '0'); }
    return v >= 1 && v <= 65535 ? (int)v : -1;
}
static bool is_ip(const char *s) {
    if (strchr(s, ':')) return true;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || *s == '.')) return false;
    return true;
}

/* 0 — ок, иначе 1 (как Go: неверные параметры). */
static int parse_cfg(const char *s, size_t n) {
    char port[8] = "", rport[8] = "", uuid[40] = "", flow[48] = "", enc[64] = "", type[8] = "", sec[16] = "",
         fp[32] = "", pbk[64] = "", sid[24] = "", spx[256] = "", alpn[64] = "";
    static char pqv[2700];
    pqv[0] = 0;
    struct { const char *k; char *v; size_t cap; } keys[] = {
        { "port", port, sizeof port }, { "user", K.user, sizeof K.user }, { "pass", K.pass, sizeof K.pass },
        { "uuid", uuid, sizeof uuid }, { "host", K.host, sizeof K.host }, { "rport", rport, sizeof rport },
        { "flow", flow, sizeof flow }, { "enc", enc, sizeof enc }, { "type", type, sizeof type },
        { "security", sec, sizeof sec }, { "fp", fp, sizeof fp }, { "sni", K.sni, sizeof K.sni },
        { "pbk", pbk, sizeof pbk }, { "sid", sid, sizeof sid }, { "spx", spx, sizeof spx },
        { "pqv", pqv, sizeof pqv }, { "alpn", alpn, sizeof alpn }, { "wspath", K.wspath, sizeof K.wspath },
        { "wshost", K.wshost, sizeof K.wshost },
    };
    int rc = 1;
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && s[e] != '\n') e++;
        size_t ln = e - i;
        if (ln) {
            const char *eq = memchr(s + i, '=', ln);
            if (!eq || eq == s + i) goto out;
            size_t kn = (size_t)(eq - (s + i)), vn = ln - kn - 1;
            for (size_t j = 0; j < sizeof keys / sizeof *keys; j++) {
                if (strlen(keys[j].k) != kn || memcmp(keys[j].k, s + i, kn)) continue;
                if (vn >= keys[j].cap || memchr(eq + 1, 0, vn) || memchr(eq + 1, '\r', vn)) goto out;
                memcpy(keys[j].v, eq + 1, vn);
                keys[j].v[vn] = 0;
                break;
            }                                   /* незнакомые ключи пропускаются, как в Go */
        }
        i = e + 1;
    }
    if ((K.port = parse_port(port)) < 0 || (K.rport = parse_port(rport)) < 0 || !parse_uuid(uuid, K.uuid)) goto out;
    if (!K.host[0] || !K.user[0] || !K.pass[0]) goto out;
    K.ul = strlen(K.user);
    K.pl = strlen(K.pass);
    if (flow[0]) goto out;                      /* xtls-rprx-vision этой сборкой не поддерживается */
    if (enc[0] && strcmp(enc, "none")) goto out;
    if (!type[0] || !strcmp(type, "tcp")) K.ws = false;
    else if (!strcmp(type, "ws")) K.ws = true;
    else goto out;
    if (fp[0]) {                                /* все отпечатки сводятся к Chrome 133 */
        static const char *const ok[] = { "chrome", "firefox", "safari", "ios", "android", "edge", "360", "qq", "random", "randomized" };
        bool f = false;
        for (size_t j = 0; j < sizeof ok / sizeof *ok; j++) if (!strcmp(fp, ok[j])) f = true;
        if (!f) goto out;
    }
    if (!K.sni[0]) snprintf(K.sni, sizeof K.sni, "%s", K.host);
    memset(&K.tc, 0, sizeof K.tc);
    K.tc.sni = K.sni;
    K.tc.verify_ip = is_ip(K.sni) ? K.sni : NULL;
    K.tc.http11_only = K.ws;
    if (!strcmp(sec, "reality")) {
        if (K.ws) goto out;                     /* Reality только поверх TCP */
        K.tc.reality = true;
        if (b64url(pbk, K.tc.pbk, sizeof K.tc.pbk) != 32) goto out;
        size_t sl = strlen(sid);
        if (sl > 16 || sl % 2) goto out;
        for (size_t j = 0; j < sl; j += 2) {
            int a = hexv(sid[j]), b = hexv(sid[j + 1]);
            if (a < 0 || b < 0) goto out;
            K.tc.sid[j / 2] = (u8)(a << 4 | b);
        }
        if (pqv[0]) {
            if (b64url(pqv, K.pqv, sizeof K.pqv) != 1952) goto out;
            K.tc.pqv = K.pqv;
        }
        if (spx[0] && spx[0] != '/') goto out;  /* spiderX — только при отказе сервера; здесь не используется */
        K.tc.alpn = TLS_ALPN_CHROME;
    } else if (!strcmp(sec, "tls")) {
        /* как Xray: для TCP ALPN берётся из отпечатка (h2, http/1.1); для WebSocket — http/1.1,
         * если в ссылке не задано ровно "h2,http/1.1" (тогда сервер, выбравший h2, — ошибка) */
        K.tc.alpn = K.ws && strcmp(alpn, "h2,http/1.1") ? TLS_ALPN_HTTP11 : TLS_ALPN_CHROME;
    } else goto out;                            /* без TLS VLESS — открытый текст */
    if (K.ws && !K.wspath[0]) snprintf(K.wspath, sizeof K.wspath, "/");
    if (K.ws && K.wspath[0] != '/') goto out;
    rc = 0;
out:
    OPENSSL_cleanse(uuid, sizeof uuid);
    OPENSSL_cleanse(pbk, sizeof pbk);
    OPENSSL_cleanse(sid, sizeof sid);
    OPENSSL_cleanse(spx, sizeof spx);
    OPENSSL_cleanse(pqv, sizeof pqv);
    return rc;
}

/* Адреса сервера — один раз, до запуска потока (DNS может ждать секунды). */
static bool resolve(void) {
    char port[8];
    snprintf(port, sizeof port, "%d", K.rport);
    struct addrinfo h, *res = NULL;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_UNSPEC;
    h.ai_socktype = SOCK_STREAM;
    h.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(K.host, port, &h, &res) || !res) return false;
    G.nsa = 0;
    for (struct addrinfo *a = res; a && G.nsa < MAX_ADDR; a = a->ai_next) {
        if (a->ai_addrlen > sizeof G.sa[0] || (a->ai_family != AF_INET && a->ai_family != AF_INET6)) continue;
        memcpy(&G.sa[G.nsa], a->ai_addr, a->ai_addrlen);
        G.salen[G.nsa] = (slen_t)a->ai_addrlen;
        G.nsa++;
    }
    freeaddrinfo(res);
    G.cur = 0;
    return G.nsa > 0;
}

/* ======================= выход VLESS ======================= */
static void out_kill(Out *o);
static void cli_kill(Cli *c) {
    if (c->dead) return;
    c->dead = true;
    if (c->out) out_kill(c->out);
    if (c->st == CS_UDP)
        for (Out *o = G.outs; o; o = o->next) if (o->udp && o->cli == c) out_kill(o);
}
static void out_kill(Out *o) {
    if (o->dead) return;
    o->dead = true;
    Cli *c = o->cli;
    if (!o->udp && c && !c->dead) {             /* дописать клиенту остаток и закрыть */
        if (!bb_n(&c->wout)) cli_kill(c);
        else { c->closing = true; c->deadline = now() + T_FLUSH; }
    }
}
static void cli_flush(Cli *c) {
    if (flush(c->fd, &c->wout) < 0) { cli_kill(c); return; }
    if (c->closing && !bb_n(&c->wout)) cli_kill(c);
}

/* Начать подключение к серверу по адресу G.cur. false — сразу не вышло. */
static bool out_dial(Out *o) {
    if (o->fd != SBAD) sclose(o->fd);
    o->ai = G.cur;
    o->fd = mksock(G.sa[o->ai].ss_family, SOCK_STREAM);
    if (o->fd == SBAD) return false;
    set_nodelay(o->fd);
    if (connect(o->fd, (struct sockaddr *)&G.sa[o->ai], G.salen[o->ai]) != 0 && !again()) {
        G.cur = (o->ai + 1) % G.nsa;            /* следующий раз — другой адрес */
        return false;
    }
    return true;
}

/* Новый выход: подключение к серверу; заголовок запроса VLESS ждёт в pend. dst — адрес SOCKS. */
static Out *out_new(Cli *c, bool udp, const u8 *dst, size_t dstn) {
    if (dstn > sizeof ((Out *)0)->dst || G.nout >= MAX_OUT || !G.nsa || atomic_load(&g_stop)) return NULL;
    Out *o = OPENSSL_zalloc(sizeof *o);
    if (!o) return NULL;
    o->fd = SBAD;
    bool ok = out_dial(o) || (G.nsa > 1 && out_dial(o));
    if (!ok) { if (o->fd != SBAD) sclose(o->fd); OPENSSL_free(o); return NULL; }
    o->st = OS_CONN;
    o->deadline = now() + T_HS;
    o->last = now();
    o->udp = udp;
    o->cli = c;
    memcpy(o->dst, dst, dstn);
    o->dstn = dstn;
    /* запрос: версия 0, UUID, дополнения 0, команда, порт, тип адреса (1 IPv4, 2 домен, 3 IPv6), адрес */
    u8 h[1 + 16 + 1 + 1 + 2 + 1 + 1 + 255];
    size_t n = 0;
    h[n++] = 0;
    memcpy(h + n, K.uuid, 16); n += 16;
    h[n++] = 0;
    h[n++] = udp ? 2 : 1;
    memcpy(h + n, dst + dstn - 2, 2); n += 2;
    if (dst[0] == 1) { h[n++] = 1; memcpy(h + n, dst + 1, 4); n += 4; }
    else if (dst[0] == 4) { h[n++] = 3; memcpy(h + n, dst + 1, 16); n += 16; }
    else { h[n++] = 2; memcpy(h + n, dst + 1, 1 + (size_t)dst[1]); n += 1 + (size_t)dst[1]; }
    ok = bb_put(&o->pend, h, n);
    OPENSSL_cleanse(h, sizeof h);
    if (!ok) { sclose(o->fd); bb_free(&o->pend); OPENSSL_free(o); return NULL; }
    o->next = G.outs;
    G.outs = o;
    G.nout++;
    return o;
}

static void out_free(Out *o) {
    if (o->fd != SBAD) sclose(o->fd);
    t13_free(o->tls);
    bb_free(&o->rin); bb_free(&o->wout); bb_free(&o->plain); bb_free(&o->pend); bb_free(&o->ubuf);
    OPENSSL_clear_free(o, sizeof *o);
    G.nout--;
}

/* Кадр WebSocket от клиента (маскированный) в запись TLS. */
static bool ws_send(Out *o, unsigned op, const u8 *p, size_t n) {
    u8 *f = G.sout, mask[4];
    size_t h = 0;
    f[h++] = (u8)(0x80 | op);
    if (n < 126) f[h++] = (u8)(0x80 | n);
    else if (n < 65536) { f[h++] = 0x80 | 126; f[h++] = (u8)(n >> 8); f[h++] = (u8)n; }
    else return false;
    if (RAND_bytes(mask, 4) != 1) return false;
    memcpy(f + h, mask, 4);
    h += 4;
    for (size_t i = 0; i < n; i++) f[h + i] = p[i] ^ mask[i & 3];
    bool ok = t13_seal(o->tls, f, h + n, &o->wout);
    OPENSSL_cleanse(f, h + n);
    return ok;
}

/* Поток VLESS к серверу: до готовности — в pend. */
static bool out_send(Out *o, const u8 *p, size_t n) {
    if (o->st != OS_UP) return bb_put(&o->pend, p, n);
    while (n) {
        size_t k = n > CHUNK ? CHUNK : n;
        if (K.ws ? !ws_send(o, 2, p, k) : !t13_seal(o->tls, p, k, &o->wout)) return false;
        p += k;
        n -= k;
    }
    return true;
}

static bool out_up(Out *o) {
    o->st = OS_UP;
    o->deadline = 0;
    Bb t = o->pend;
    memset(&o->pend, 0, sizeof o->pend);
    bool ok = out_send(o, bb_ptr(&t), bb_n(&t));
    bb_free(&t);
    return ok;
}

static bool b64(const u8 *in, size_t n, char *out, size_t cap) {
    if (cap < 4 * ((n + 2) / 3) + 1) return false;
    EVP_EncodeBlock((unsigned char *)out, in, (int)n);
    return true;
}

/* TLS готов: WebSocket — запрос Upgrade, иначе сразу данные. */
static bool out_tls_up(Out *o) {
    if (!K.ws) return out_up(o);
    u8 key[16], dg[20];
    char k64[32], req[1600];
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned dl = 0;
    if (RAND_bytes(key, 16) != 1 || !b64(key, 16, k64, sizeof k64)) return false;
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    bool ok = m && EVP_DigestInit_ex(m, EVP_sha1(), NULL) == 1 && EVP_DigestUpdate(m, k64, strlen(k64)) == 1 &&
              EVP_DigestUpdate(m, guid, sizeof guid - 1) == 1 && EVP_DigestFinal_ex(m, dg, &dl) == 1 &&
              b64(dg, 20, o->wsacc, sizeof o->wsacc);
    EVP_MD_CTX_free(m);
    if (!ok) return false;
    const char *host = K.wshost[0] ? K.wshost : K.sni;
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " UA "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", K.wspath, host, k64);
    if (n <= 0 || (size_t)n >= sizeof req || !t13_seal(o->tls, req, (size_t)n, &o->wout)) return false;
    o->st = OS_WS;
    return true;
}

static void udp_reply(Out *o, const u8 *p, size_t n) {
    Cli *c = o->cli;
    if (!c || c->dead || !c->have_peer || n > 65535) return;
    u8 *b = G.sout;
    b[0] = b[1] = b[2] = 0;
    memcpy(b + 3, o->dst, o->dstn);
    memcpy(b + 3 + o->dstn, p, n);
#ifdef _WIN32
    sendto(c->ufd, (const char *)b, (int)(3 + o->dstn + n), 0, (struct sockaddr *)&c->peer, sizeof c->peer);
#else
    sendto(c->ufd, b, 3 + o->dstn + n, 0, (struct sockaddr *)&c->peer, sizeof c->peer);
#endif
}

/* Данные из туннеля (после WebSocket): снять ответ VLESS и отдать программе. */
static bool vless_in(Out *o, const u8 *p, size_t n) {
    while (n && o->vl < 3) {
        if (o->vl == 0) { if (*p != 0) return false; o->vl = 1; }
        else if (o->vl == 1) { o->vskip = *p; o->vl = o->vskip ? 2 : 3; }
        else { if (!--o->vskip) o->vl = 3; }
        p++;
        n--;
    }
    if (!n) return true;
    if (!o->udp) {
        Cli *c = o->cli;
        if (!c || c->dead || !bb_put(&c->wout, p, n)) return false;
        if (c->eof) c->deadline = now() + T_LINGER;   /* клиент закрыл свою сторону: пока идёт ответ — ждём */
        cli_flush(c);
        return true;
    }
    if (!bb_put(&o->ubuf, p, n)) return false;
    while (bb_n(&o->ubuf) >= 2) {
        u8 *q = bb_ptr(&o->ubuf);
        size_t l = (size_t)q[0] << 8 | q[1];
        if (bb_n(&o->ubuf) < 2 + l) break;
        udp_reply(o, q + 2, l);
        bb_eat(&o->ubuf, 2 + l);
    }
    return true;
}

static const u8 *find(const u8 *h, size_t hn, const char *nd) {
    size_t nn = strlen(nd);
    for (size_t i = 0; i + nn <= hn; i++) if (!memcmp(h + i, nd, nn)) return h + i;
    return NULL;
}
static char lc(u8 c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c; }

/* Ответ на Upgrade: строка статуса 101 и заголовок Sec-WebSocket-Accept с ожидаемым значением. */
static bool ws_accept_ok(const Out *o, const u8 *p, size_t hl) {
    static const char sl[] = "HTTP/1.1 101", name[] = "sec-websocket-accept:";
    if (hl < sizeof sl || memcmp(p, sl, sizeof sl - 1) || (p[sizeof sl - 1] != ' ' && p[sizeof sl - 1] != '\r')) return false;
    size_t al = strlen(o->wsacc), nl = sizeof name - 1;
    for (size_t i = 0; i + 2 < hl; i++) {
        if (p[i] != '\r' || p[i + 1] != '\n') continue;
        size_t s = i + 2, k = 0;
        while (k < nl && s + k < hl && lc(p[s + k]) == name[k]) k++;
        if (k != nl) continue;
        s += nl;
        while (s < hl && (p[s] == ' ' || p[s] == '\t')) s++;
        if (s + al + 2 <= hl && !memcmp(p + s, o->wsacc, al) && p[s + al] == '\r' && p[s + al + 1] == '\n') return true;
        return false;                           /* заголовок есть, значение не то */
    }
    return false;
}

static bool out_deliver(Out *o) {
    if (o->st == OS_WS) {
        u8 *p = bb_ptr(&o->plain);
        size_t n = bb_n(&o->plain);
        const u8 *e = find(p, n, "\r\n\r\n");
        if (!e) return n < 8192;
        size_t hl = (size_t)(e - p) + 4;
        if (!ws_accept_ok(o, p, hl)) return false;
        bb_eat(&o->plain, hl);
        if (!out_up(o)) return false;
    }
    if (o->st != OS_UP) return !bb_n(&o->plain);
    for (;;) {
        u8 *p = bb_ptr(&o->plain);
        size_t n = bb_n(&o->plain), used, pn;
        const u8 *pay;
        if (!n) return true;
        if (K.ws) {
            if (n < 2) return true;
            unsigned op = p[0] & 0x0f;
            if ((p[0] & 0x70) || (p[1] & 0x80)) return false;   /* RSV или маска от сервера — ошибка (RFC 6455 §5.1-5.2) */
            if (op >= 8 && !(p[0] & 0x80)) return false;         /* управляющий кадр не может быть фрагментом (§5.5) */
            uint64_t len = p[1] & 0x7f;
            size_t h = 2;
            if (len == 126) { if (n < 4) return true; len = (uint64_t)p[2] << 8 | p[3]; h = 4; }
            else if (len == 127) {
                if (n < 10) return true;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
                h = 10;
            }
            if (len > BB_MAX / 2) return false;
            if (n < h + len) return true;
            u8 *pl = p + h;
            used = h + (size_t)len;
            if (op == 0x8) {                    /* закрытие: ответить Close и закрыть */
                ws_send(o, 0x8, pl, len >= 2 && len <= 125 ? 2 : 0);
                flush(o->fd, &o->wout);
                return false;
            }
            if (op == 0x9) {                    /* ping -> pong */
                if (len > 125 || !ws_send(o, 0xA, pl, (size_t)len)) return false;
                bb_eat(&o->plain, used);
                continue;
            }
            if (op == 0xA) { bb_eat(&o->plain, used); continue; }
            if (op > 2) return false;
            pay = pl;
            pn = (size_t)len;
        } else {
            pay = p;
            pn = used = n;
        }
        if (!vless_in(o, pay, pn)) return false;
        bb_eat(&o->plain, used);
    }
}

static void out_connected(Out *o) {
    int err = 0;
    slen_t el = sizeof err;
    if (getsockopt(o->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el) || err) {
        G.cur = (o->ai + 1) % G.nsa;            /* адрес не отвечает: этот и следующие — другой */
        if (G.nsa > 1 && now() < o->deadline && out_dial(o)) return;
        out_kill(o);
        return;
    }
    o->tls = t13_new(&K.tc, &o->wout);
    if (!o->tls) { out_kill(o); return; }
    o->st = OS_TLS;
    if (flush(o->fd, &o->wout) < 0) out_kill(o);
}

static void out_read(Out *o) {
    if (!bb_reserve(&o->rin, 16384 + 512)) { out_kill(o); return; }
    long r = s_recv(o->fd, o->rin.p + o->rin.len, o->rin.cap - o->rin.len);
    if (r == 0) { out_kill(o); return; }
    if (r < 0) { if (!again()) out_kill(o); return; }
    o->rin.len += (size_t)r;
    o->last = now();
    int k = t13_input(o->tls, &o->rin, &o->plain, &o->wout);
    if (k < 0) { out_kill(o); return; }
    if (o->st == OS_TLS && t13_ready(o->tls) && !out_tls_up(o)) { out_kill(o); return; }
    if (!out_deliver(o) || k == 1) { out_kill(o); return; }
    if (flush(o->fd, &o->wout) < 0) out_kill(o);
}

/* ======================= вход SOCKS5 ======================= */
static void cli_reply(Cli *c, const u8 *p, size_t n, bool close_after) {
    if (!bb_put(&c->wout, p, n)) { cli_kill(c); return; }
    if (close_after) { c->closing = true; c->deadline = now() + T_FLUSH; }
    cli_flush(c);
}

static void socks_req(Cli *c) {
    u8 *p = bb_ptr(&c->rin);
    size_t n = bb_n(&c->rin), al;
    if (n < 5) return;
    if (p[0] != 5 || p[2] != 0) { cli_kill(c); return; }
    switch (p[3]) {
    case 1: al = 4; break;
    case 4: al = 16; break;
    case 3: if (!p[4]) { cli_kill(c); return; } al = 1 + (size_t)p[4]; break;
    default: { static const u8 r[10] = { 5, 8, 0, 1 }; cli_reply(c, r, 10, true); return; }
    }
    if (n < 4 + al + 2) return;
    const u8 *dst = p + 3;
    size_t dn = 1 + al + 2;
    if (p[1] == 1) {                            /* CONNECT */
        Out *o = out_new(c, false, dst, dn);
        if (!o) { static const u8 r[10] = { 5, 1, 0, 1 }; cli_reply(c, r, 10, true); return; }
        c->out = o;
        c->st = CS_TCP;
        c->deadline = 0;
        bb_eat(&c->rin, 4 + al + 2);
        if (bb_n(&c->rin) && !out_send(o, bb_ptr(&c->rin), bb_n(&c->rin))) { out_kill(o); return; }
        bb_free(&c->rin);
        static const u8 r[10] = { 5, 0, 0, 1 };
        cli_reply(c, r, 10, false);
    } else if (p[1] == 3) {                     /* UDP ASSOCIATE */
        /* DST — откуда клиент будет слать: IPv4 0.0.0.0/127.x и порт (0 — не указан). Иное — отказ. */
        uint32_t ip = 0;
        uint16_t port = (uint16_t)(dst[dn - 2] << 8 | dst[dn - 1]);
        if (p[3] == 1) memcpy(&ip, p + 4, 4);
        bool good = p[3] == 1 && (!ip || (ntohl(ip) >> 24) == 127);
        struct sockaddr_in a;
        if (!good || !udp_loopback(&c->ufd, &a)) { static const u8 r[10] = { 5, 1, 0, 1 }; cli_reply(c, r, 10, true); return; }
        c->want_ip = ip;
        c->want_port = port;
        c->st = CS_UDP;
        c->deadline = 0;
        bb_free(&c->rin);
        u8 r[10] = { 5, 0, 0, 1, 127, 0, 0, 1 };
        memcpy(r + 8, &a.sin_port, 2);
        cli_reply(c, r, 10, false);
    } else {
        static const u8 r[10] = { 5, 7, 0, 1 };
        cli_reply(c, r, 10, true);
    }
}

static void socks_step(Cli *c) {
    while (!c->dead && !c->closing) {
        u8 *p = bb_ptr(&c->rin);
        size_t n = bb_n(&c->rin);
        if (c->st == CS_GREET) {
            if (n < 2) return;
            if (p[0] != 5) { cli_kill(c); return; }
            size_t m = p[1];
            if (n < 2 + m) return;
            bool pw = m && memchr(p + 2, 2, m);
            bb_eat(&c->rin, 2 + m);
            u8 r[2] = { 5, pw ? 2 : 0xff };
            c->st = CS_AUTH;
            cli_reply(c, r, 2, !pw);
        } else if (c->st == CS_AUTH) {
            if (n < 2) return;
            if (p[0] != 1) { cli_kill(c); return; }
            size_t ul = p[1];
            if (n < 3 + ul) return;
            size_t pl = p[2 + ul];
            if (n < 3 + ul + pl) return;
            bool ok = ul == K.ul && pl == K.pl;
            ok &= !CRYPTO_memcmp(p + 2, K.user, ul == K.ul ? ul : 0);
            ok &= !CRYPTO_memcmp(p + 3 + ul, K.pass, pl == K.pl ? pl : 0);
            ok &= K.ul > 0;                     /* после XliteWipe — никого */
            OPENSSL_cleanse(p, 3 + ul + pl);
            bb_eat(&c->rin, 3 + ul + pl);
            u8 r[2] = { 1, ok ? 0 : 1 };
            c->st = CS_REQ;
            cli_reply(c, r, 2, !ok);
        } else if (c->st == CS_REQ) {
            socks_req(c);
            return;
        } else return;
    }
}

static void cli_read(Cli *c) {
    if (c->st == CS_TCP) {
        Out *o = c->out;
        if (!o || o->dead) { cli_kill(c); return; }
        long r = s_recv(c->fd, G.sin, CHUNK);
        if (r == 0) { c->eof = true; c->deadline = now() + T_LINGER; return; }
        if (r < 0) { if (!again()) cli_kill(c); return; }
        o->last = now();
        if (!out_send(o, G.sin, (size_t)r)) { OPENSSL_cleanse(G.sin, (size_t)r); out_kill(o); return; }
        OPENSSL_cleanse(G.sin, (size_t)r);
        if (o->st == OS_UP && flush(o->fd, &o->wout) < 0) out_kill(o);
        return;
    }
    if (c->st == CS_UDP) {                      /* управление: данных быть не должно, закрытие — конец ассоциации */
        long r = s_recv(c->fd, G.sin, 512);
        if (r == 0 || (r < 0 && !again())) cli_kill(c);
        return;
    }
    if (!bb_reserve(&c->rin, 600)) { cli_kill(c); return; }
    long r = s_recv(c->fd, c->rin.p + c->rin.len, 600);
    if (r == 0 || (r < 0 && !again())) { cli_kill(c); return; }
    if (r < 0) return;
    c->rin.len += (size_t)r;
    if (bb_n(&c->rin) > 2048) { cli_kill(c); return; }
    socks_step(c);
}

/* Отправитель датаграммы годится в клиенты ассоциации? Первый подходящий закрепляется. */
static bool udp_peer_ok(Cli *c, const struct sockaddr_in *from) {
    if (from->sin_family != AF_INET || (ntohl(from->sin_addr.s_addr) >> 24) != 127) return false;
    if (c->have_peer) return from->sin_addr.s_addr == c->peer.sin_addr.s_addr && from->sin_port == c->peer.sin_port;
    unsigned port = ntohs(from->sin_port);
    if (c->want_ip && from->sin_addr.s_addr != c->want_ip) return false;
    if (c->want_port && port != c->want_port) return false;
    if (port == c->bad_port) return false;
    i64 t = now();                              /* проверка владельца — не чаще 20 раз в секунду */
    if (t - c->checks_t >= 1000) { c->checks_t = t; c->checks = 0; }
    if (++c->checks > 20) return false;
    if (!udp_port_ours(port)) { c->bad_port = (uint16_t)port; return false; }
    c->peer = *from;
    c->have_peer = true;
    return true;
}

static void udp_read(Cli *c) {
    for (int guard = 0; guard < 64 && !c->dead; guard++) {
        struct sockaddr_in from;
        slen_t fl = sizeof from;
        u8 *p = G.sin;
#ifdef _WIN32
        long r = recvfrom(c->ufd, (char *)p, SIN_SZ - 1, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) { if (WSAGetLastError() == WSAECONNRESET) continue; return; }
#else
        long r = (long)recvfrom(c->ufd, p, SIN_SZ - 1, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) { if (errno == EINTR) continue; return; }
#endif
        size_t n = (size_t)r, al;
        if (fl < (slen_t)sizeof from || !udp_peer_ok(c, &from)) continue;
        if (n < 5 || p[0] || p[1] || p[2]) continue;   /* RSV, FRAG=0: фрагменты не собираем */
        switch (p[3]) {
        case 1: al = 4; break;
        case 4: al = 16; break;
        case 3: if (!p[4]) continue; al = 1 + (size_t)p[4]; break;
        default: continue;
        }
        size_t hl = 4 + al + 2;
        if (n <= hl) continue;
        const u8 *dst = p + 3;
        size_t dn = 1 + al + 2, pn = n - hl;
        Out *o = NULL;
        for (Out *x = G.outs; x; x = x->next)
            if (!x->dead && x->udp && x->cli == c && x->dstn == dn && !memcmp(x->dst, dst, dn)) { o = x; break; }
        if (!o) {
            if (c->nudp >= MAX_UDP_DST || !(o = out_new(c, true, dst, dn))) continue;
            c->nudp++;
        }
        if ((o->st != OS_UP && bb_n(&o->pend) > UDP_PEND_MAX) || bb_n(&o->wout) > 4 * HIWAT) continue;
        u8 *q = p + hl - 2;                     /* длина пакета VLESS — поверх уже разобранного порта */
        q[0] = (u8)(pn >> 8);
        q[1] = (u8)pn;
        if (!out_send(o, q, pn + 2)) { out_kill(o); continue; }
        o->last = now();
        if (o->st == OS_UP && flush(o->fd, &o->wout) < 0) out_kill(o);
    }
}

static void do_accept(void) {
    for (int i = 0; i < 16 && G.ncli < MAX_CLI; i++) {
#ifdef _WIN32
        sock_t s = accept(g_lsn, NULL, NULL);
        if (s != SBAD) SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
#else
        sock_t s = accept4(g_lsn, NULL, NULL, SOCK_CLOEXEC);
#endif
        if (s == SBAD) { if (!again()) G.lsn_pause = now() + 1000; return; }
        set_nb(s);
        set_nodelay(s);
        Cli *c = OPENSSL_zalloc(sizeof *c);
        if (!c) { sclose(s); return; }
        c->fd = s;
        c->ufd = SBAD;
        c->st = CS_GREET;
        c->deadline = now() + T_SOCKS;
        c->next = G.clis;
        G.clis = c;
        G.ncli++;
    }
}

static void sweep(void) {
    for (Out **pp = &G.outs; *pp;) {
        Out *o = *pp;
        if (!o->dead) { pp = &o->next; continue; }
        *pp = o->next;
        if (o->cli) {
            if (o->udp) o->cli->nudp--;
            else if (o->cli->out == o) o->cli->out = NULL;
        }
        out_free(o);
    }
    for (Cli **pp = &G.clis; *pp;) {
        Cli *c = *pp;
        if (!c->dead) { pp = &c->next; continue; }
        *pp = c->next;
        sclose(c->fd);
        if (c->ufd != SBAD) sclose(c->ufd);
        bb_free(&c->rin);
        bb_free(&c->wout);
        OPENSSL_clear_free(c, sizeof *c);
        G.ncli--;
    }
}

/* ======================= цикл потока ======================= */
enum { K_WAKE, K_LSN, K_CLI, K_UDP, K_OUT };
typedef struct { pfd_t *f; void **who; u8 *kind; size_t n, cap; } Set;

static bool set_add(Set *s, sock_t fd, short ev, int kind, void *who) {
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 32;
        pfd_t *f = realloc(s->f, nc * sizeof *f);
        if (f) s->f = f;
        void **w = realloc(s->who, nc * sizeof *w);
        if (w) s->who = w;
        u8 *k = realloc(s->kind, nc);
        if (k) s->kind = k;
        if (!f || !w || !k) return false;
        s->cap = nc;
    }
    s->f[s->n].fd = fd;
    s->f[s->n].events = ev;
    s->f[s->n].revents = 0;
    s->who[s->n] = who;
    s->kind[s->n] = (u8)kind;
    s->n++;
    return true;
}

static int run(void *arg) {
    (void)arg;
    Set s = {0};
    bool broken = false;
    while (!atomic_load(&g_stop)) {
        s.n = 0;
        i64 t = now(), wake = -1;
#define DUE(x) do { if ((x) > 0 && (wake < 0 || (x) < wake)) wake = (x); } while (0)
        bool ok = set_add(&s, g_wrx, POLLIN, K_WAKE, NULL);
        if (G.lsn_pause && t >= G.lsn_pause) G.lsn_pause = 0;
        if (G.ncli < MAX_CLI && !G.lsn_pause) ok &= set_add(&s, g_lsn, POLLIN, K_LSN, NULL);
        DUE(G.lsn_pause);
        for (Cli *c = G.clis; c; c = c->next) {
            if (c->dead) continue;
            short ev = 0;
            if (!c->closing && !c->eof) {
                if (c->st != CS_TCP) ev |= POLLIN;
                else if (c->out && !c->out->dead && bb_n(&c->out->pend) + bb_n(&c->out->wout) < HIWAT) ev |= POLLIN;
            }
            if (bb_n(&c->wout)) ev |= POLLOUT;
            if (ev) ok &= set_add(&s, c->fd, ev, K_CLI, c);
            if (c->st == CS_UDP) ok &= set_add(&s, c->ufd, POLLIN, K_UDP, c);
            DUE(c->deadline);
        }
        for (Out *o = G.outs; o; o = o->next) {
            if (o->dead) continue;
            short ev = 0;
            if (o->st == OS_CONN) ev = POLLOUT;
            else {
                if (o->udp || !o->cli || bb_n(&o->cli->wout) < HIWAT) ev |= POLLIN;
                if (bb_n(&o->wout)) ev |= POLLOUT;
            }
            if (ev) ok &= set_add(&s, o->fd, ev, K_OUT, o);
            DUE(o->deadline);
            DUE(o->last + T_IDLE);
        }
#undef DUE
        if (!ok) { broken = true; break; }
        int to = wake < 0 ? -1 : (wake <= t ? 0 : (int)(wake - t > 3600000 ? 3600000 : wake - t));
        int r = spoll(s.f, s.n, to);
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            broken = true;
            break;
        }
        for (size_t i = 0; i < s.n && r > 0; i++) {
            short re = s.f[i].revents;
            if (!re) continue;
            switch (s.kind[i]) {
            case K_WAKE: { char b[64]; while (s_recv(g_wrx, b, sizeof b) > 0) {} break; }
            case K_LSN: do_accept(); break;
            case K_CLI: {
                Cli *c = s.who[i];
                if (c->dead) break;
                if (re & (POLLIN | POLLHUP | POLLERR)) cli_read(c);
                if (!c->dead && (re & POLLOUT)) cli_flush(c);
                break;
            }
            case K_UDP: { Cli *c = s.who[i]; if (!c->dead) udp_read(c); break; }
            case K_OUT: {
                Out *o = s.who[i];
                if (o->dead) break;
                if (o->st == OS_CONN) { out_connected(o); break; }
                if (re & (POLLIN | POLLHUP | POLLERR)) out_read(o);
                if (!o->dead && (re & POLLOUT) && flush(o->fd, &o->wout) < 0) out_kill(o);
                break;
            }
            }
        }
        t = now();
        for (Cli *c = G.clis; c; c = c->next)
            if (!c->dead && c->deadline && t >= c->deadline) cli_kill(c);
        for (Out *o = G.outs; o; o = o->next)
            if (!o->dead && ((o->deadline && t >= o->deadline) || t - o->last >= T_IDLE)) out_kill(o);
        sweep();
    }
    if (broken) {                               /* сбой: порт закрыть сразу (клиенты получат отказ, а не тишину) */
        atomic_store(&g_failed, true);
        sclose(g_lsn);
        g_lsn = SBAD;
    }
    for (Cli *c = G.clis; c; c = c->next) cli_kill(c);
    for (Out *o = G.outs; o; o = o->next) out_kill(o);
    sweep();                                    /* ключи сессий затираются в t13_free */
    free(s.f);
    free(s.who);
    free(s.kind);
    atomic_store(&g_exited, true);
    return 0;
}

/* ======================= контракт ======================= */
static void bufs_free(void) {
    if (G.sin) { OPENSSL_cleanse(G.sin, SIN_SZ); plat_unlock(G.sin, SIN_SZ); plat_pages_free(G.sin, SIN_SZ); }
    if (G.sout) { OPENSSL_cleanse(G.sout, SOUT_SZ); plat_unlock(G.sout, SOUT_SZ); plat_pages_free(G.sout, SOUT_SZ); }
    G.sin = G.sout = NULL;
}

static void teardown(void) {
    if (g_lsn != SBAD) sclose(g_lsn);
    if (g_wrx != SBAD) sclose(g_wrx);
    if (g_wtx != SBAD) sclose(g_wtx);
    g_lsn = g_wrx = g_wtx = SBAD;
    bufs_free();
    OPENSSL_cleanse(G.sa, sizeof G.sa);
    memset(&G, 0, sizeof G);
    t13_global_free();
    OPENSSL_cleanse(&K, sizeof K);
}

int XliteStart(char *cfg, int n) {
    if (!cfg || n <= 0 || n > 65536) return 1;
    XliteStop();
    plat_lock(&K, sizeof K);                    /* UUID и пароль SOCKS — не в подкачку */
    if (parse_cfg(cfg, (size_t)n)) { teardown(); return 1; }
    G.sin = plat_pages(SIN_SZ);
    G.sout = plat_pages(SOUT_SZ);
    if (!G.sin || !G.sout || !t13_global_init()) { teardown(); return 2; }
    plat_lock(G.sin, SIN_SZ);
    plat_lock(G.sout, SOUT_SZ);
    if (!resolve()) { teardown(); return 2; }   /* до потока: поток DNS не ждёт никогда */
    /* вход: 127.0.0.1:port, исключительно (на Windows чужой процесс не сядет на тот же порт) */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)K.port);
    g_lsn = mksock(AF_INET, SOCK_STREAM);
    if (g_lsn == SBAD) { teardown(); return 2; }
    exclusive(g_lsn);
    if (bind(g_lsn, (struct sockaddr *)&a, sizeof a) || listen(g_lsn, 64)) { teardown(); return 3; }
    /* пробуждение: пара UDP на 127.0.0.1, связанная в обе стороны — чужие датаграммы ядро отбросит */
    struct sockaddr_in wa, ta;
    slen_t tl = sizeof ta;
    if (!udp_loopback(&g_wrx, &wa) || !udp_loopback(&g_wtx, &ta)) { teardown(); return 2; }
    if (connect(g_wtx, (struct sockaddr *)&wa, sizeof wa) || getsockname(g_wtx, (struct sockaddr *)&ta, &tl) ||
        connect(g_wrx, (struct sockaddr *)&ta, sizeof ta)) { teardown(); return 2; }
    atomic_store(&g_stop, false);
    atomic_store(&g_exited, false);
    atomic_store(&g_failed, false);
    G.thr = thread_start(run, NULL);
    if (!G.thr) { teardown(); return 2; }
    return 0;
}

void XliteStop(void) {
    if (G.thr) {
        atomic_store(&g_stop, true);
        s_send(g_wtx, "x", 1);
        thread_join(G.thr);
        G.thr = NULL;
    }
    teardown();
}

/* Паника: затереть параметры ссылки сразу (поток после этого видит пустые строки и нули — память
 * остаётся годной, указатели в K.tc указывают в те же массивы), велеть потоку закрыть всё (ключи
 * сессий затираются в t13_free) и подождать его не дольше 0,3 с. Join и освобождение — в XliteStop. */
void XliteWipe(void) {
    OPENSSL_cleanse(K.user, sizeof K.user);
    OPENSSL_cleanse(K.pass, sizeof K.pass);
    K.ul = K.pl = 0;
    OPENSSL_cleanse(K.uuid, sizeof K.uuid);
    OPENSSL_cleanse(K.host, sizeof K.host);
    OPENSSL_cleanse(K.sni, sizeof K.sni);
    OPENSSL_cleanse(K.wshost, sizeof K.wshost);
    OPENSSL_cleanse(K.wspath, sizeof K.wspath);
    OPENSSL_cleanse(K.pqv, sizeof K.pqv);
    OPENSSL_cleanse(K.tc.pbk, sizeof K.tc.pbk);
    OPENSSL_cleanse(K.tc.sid, sizeof K.tc.sid);
    if (!G.thr) return;
    atomic_store(&g_stop, true);
    s_send(g_wtx, "x", 1);
    for (int i = 0; i < 60 && !atomic_load(&g_exited); i++) plat_sleep_ms(5);
    if (atomic_load(&g_exited)) bufs_free();    /* поток вышел — его буферы больше не нужны */
}

int XliteFailed(void) { return G.thr && atomic_load(&g_failed); }
