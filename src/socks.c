/* SOCKS5 UDP ASSOCIATE (RFC 1928) с логином и паролем (RFC 1929): голосовой UDP через локальный
 * SOCKS5 туннеля VLESS (xlite_c.c; вариант на Go — Xray).
 *
 * ВНИМАНИЕ: файл не компилируется отдельно — он включается в voice.c (#include "socks.c"),
 * все функции static. Так сборка (build.sh) не меняется.
 *
 * Как устроено:
 *   1. TCP-соединение управления к 127.0.0.1:порт туннеля, приветствие (только метод 0x02 — логин и
 *      пароль), аутентификация, команда UDP ASSOCIATE. В ответе — адрес реле UDP.
 *   2. Каждая датаграмма уходит на реле с заголовком: RSV(2)=0, FRAG=0, ATYP=1, IPv4, порт, данные.
 *      Ответы реле приходят с тем же заголовком: принимаются только FRAG=0 и только от голосового
 *      сервера (адрес и порт сверяются), остальное отбрасывается.
 *   3. Пока живо TCP-соединение управления, живо и сопоставление UDP. Его закрытие или ошибка —
 *      туннель для UDP умер: голос завершается, напрямую ничего не отправляется.
 * Рукопожатие неблокирующее, с общим сроком и проверкой флага остановки; дальше сокет управления
 * ждётся в общем цикле голосового потока вместе с WebSocket. Все ответы прокси проверяются строго:
 * это данные извне. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sk_t;
#define SK_BAD INVALID_SOCKET
#define sk_close closesocket
static bool sk_again(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR; }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
typedef int sk_t;
#define SK_BAD (-1)
#define sk_close close
static bool sk_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
#endif

#define SOCKS_HDR 10                             /* RSV RSV FRAG ATYP=1 IPv4(4) порт(2) */

typedef struct {
    sk_t tcp;                                    /* соединение управления; SK_BAD — нет */
    unsigned char relay[16];                     /* sockaddr_in реле UDP (для udp_send) */
    unsigned char dst_ip[4];                     /* голосовой сервер: куда идут наши датаграммы */
    unsigned char dst_port[2];
    bool ready;
} Socks;

static void socks_reset(Socks *s) { memset(s, 0, sizeof *s); s->tcp = SK_BAD; }

/* Ждать готовности сокета: 1 — готов, 0 — срок вышел, -1 — ошибка. */
static int sk_wait(sk_t t, bool wr, int ms) {
#ifdef _WIN32
    fd_set f, x;
    FD_ZERO(&f); FD_ZERO(&x);
    FD_SET(t, &f); FD_SET(t, &x);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select(0, wr ? NULL : &f, wr ? &f : NULL, &x, &tv);
    if (r < 0) return -1;
    if (r > 0 && FD_ISSET(t, &x)) return -1;     /* неудачный connect на Windows — в exceptfds */
    return r > 0;
#else
    struct pollfd p = { t, (short)(wr ? POLLOUT : POLLIN), 0 };
    int r = poll(&p, 1, ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r > 0 && (p.revents & (POLLERR | POLLNVAL))) return -1;
    return r > 0;
#endif
}

/* Отправить/принять ровно n байт до срока; stop — прервать по выходу из звонка. */
static bool sk_io(sk_t t, bool wr, unsigned char *b, size_t n, i64 deadline, const atomic_bool *stop) {
    size_t off = 0;
    while (off < n) {
        if (atomic_load(stop)) return false;
        i64 left = deadline - now_ms();
        if (left <= 0) return false;
        int w = sk_wait(t, wr, left > 100 ? 100 : (int)left);
        if (w < 0) return false;
        if (w == 0) continue;
#ifdef _WIN32
        int r = wr ? send(t, (const char *)b + off, (int)(n - off), 0) : recv(t, (char *)b + off, (int)(n - off), 0);
#else
        ssize_t r = wr ? send(t, b + off, n - off, MSG_NOSIGNAL) : recv(t, b + off, n - off, 0);
#endif
        if (r == 0) return false;                /* прокси закрыл соединение */
        if (r < 0) { if (sk_again()) continue; return false; }
        off += (size_t)r;
    }
    return true;
}

static void sk_nonblock(sk_t t) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(t, FIONBIO, &nb);
#else
    int fl = fcntl(t, F_GETFL, 0);
    if (fl >= 0) fcntl(t, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Поднять UDP ASSOCIATE. dst_ip/dst_port — голосовой сервер (IPv4-литерал от Discord).
 * usock — свой UDP-сокет, с которого пойдут датаграммы (уже привязанный, udp_open), или -1: его порт
 * сообщается туннелю в запросе (RFC 1928 DST.PORT), и реле не примет датаграммы с другого порта.
 * err — по-русски, без учётных данных. */
static bool socks_open_udp(Socks *s, const char *ip, int port, const char *user, const char *pass,
                           const char *dst_ip, int dst_port, intptr_t usock, const atomic_bool *stop, char *err, size_t errn);
static bool socks_open_udp(Socks *s, const char *ip, int port, const char *user, const char *pass,
                           const char *dst_ip, int dst_port, intptr_t usock, const atomic_bool *stop, char *err, size_t errn) {
    socks_reset(s);
    size_t ul = strlen(user), pl = strlen(pass);
    struct sockaddr_in pa;
    if (!ul || ul > 255 || !pl || pl > 255 || port <= 0 || port > 65535 || dst_port <= 0 || dst_port > 65535 ||
        !udp_resolve(ip, (u16)port, &pa) || inet_pton(AF_INET, dst_ip, s->dst_ip) != 1) {
        snprintf(err, errn, "неверные параметры SOCKS5");
        return false;
    }
    s->dst_port[0] = (unsigned char)(dst_port >> 8);
    s->dst_port[1] = (unsigned char)dst_port;
    i64 deadline = now_ms() + 5000;
    unsigned char b[3 + 255 + 255 + 3];
    bool ok = false;
    const char *why = "локальный SOCKS5 туннеля не отвечает";

#ifdef _WIN32
    s->tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    s->tcp = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
    if (s->tcp == SK_BAD) goto done;
    sk_nonblock(s->tcp);
    {
        int one = 1;
        setsockopt(s->tcp, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
    if (connect(s->tcp, (struct sockaddr *)&pa, sizeof pa) != 0) {
        if (!sk_again()) goto done;
        int w;
        do w = sk_wait(s->tcp, true, 100); while (w == 0 && !atomic_load(stop) && now_ms() < deadline);
        if (w <= 0) goto done;
        int e = 0;
        socklen_t el = sizeof e;
        if (getsockopt(s->tcp, SOL_SOCKET, SO_ERROR, (char *)&e, &el) != 0 || e) goto done;
    }
    /* приветствие: версия 5, один метод — логин/пароль */
    b[0] = 5; b[1] = 1; b[2] = 2;
    if (!sk_io(s->tcp, true, b, 3, deadline, stop) || !sk_io(s->tcp, false, b, 2, deadline, stop)) goto done;
    if (b[0] != 5 || b[1] != 2) { why = "SOCKS5 не принял вход по паролю"; goto done; }
    /* RFC 1929: VER=1, ULEN, UNAME, PLEN, PASSWD */
    size_t n = 0;
    b[n++] = 1;
    b[n++] = (unsigned char)ul; memcpy(b + n, user, ul); n += ul;
    b[n++] = (unsigned char)pl; memcpy(b + n, pass, pl); n += pl;
    bool sent = sk_io(s->tcp, true, b, n, deadline, stop);
    wipe(b, sizeof b);                           /* пароль сессии не задерживается в стеке */
    if (!sent || !sk_io(s->tcp, false, b, 2, deadline, stop)) goto done;
    if (b[0] != 1 || b[1] != 0) { why = "SOCKS5 отверг логин/пароль туннеля"; goto done; }
    /* UDP ASSOCIATE: DST = 0.0.0.0:порт своего UDP-сокета (если известен), иначе 0.0.0.0:0 */
    unsigned char req[10] = { 5, 3, 0, 1, 0, 0, 0, 0, 0, 0 };
    if (usock >= 0) {
        struct sockaddr_in la;
        socklen_t ll = sizeof la;
        if (getsockname((sk_t)usock, (struct sockaddr *)&la, &ll) == 0 && la.sin_family == AF_INET) memcpy(req + 8, &la.sin_port, 2);
    }
    if (!sk_io(s->tcp, true, req, sizeof req, deadline, stop)) goto done;
    unsigned char h[4];
    if (!sk_io(s->tcp, false, h, 4, deadline, stop)) goto done;
    if (h[0] != 5 || h[2] != 0) { why = "SOCKS5: неверный ответ"; goto done; }
    if (h[1] != 0) { why = "SOCKS5 туннеля отказал в UDP"; goto done; }
    unsigned char a[18];
    char rip[64];
    if (h[3] == 1) {
        if (!sk_io(s->tcp, false, a, 6, deadline, stop)) goto done;
        if (!a[0] && !a[1] && !a[2] && !a[3]) snprintf(rip, sizeof rip, "%s", ip);   /* 0.0.0.0 — тот же адрес, что у прокси */
        else snprintf(rip, sizeof rip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
        if (!udp_resolve(rip, (u16)(a[4] << 8 | a[5]), s->relay)) goto done;
    } else if (h[3] == 4) {
        if (!sk_io(s->tcp, false, a, 18, deadline, stop)) goto done;
        bool zero = true;
        for (int i = 0; i < 16; i++) if (a[i]) zero = false;
        if (!zero) { why = "SOCKS5: реле UDP на IPv6 не поддерживается"; goto done; }
        if (!udp_resolve(ip, (u16)(a[16] << 8 | a[17]), s->relay)) goto done;
    } else { why = "SOCKS5: неверный тип адреса реле"; goto done; }
    struct sockaddr_in *ra = (struct sockaddr_in *)s->relay;
    if (!ra->sin_port) { why = "SOCKS5: нулевой порт реле"; goto done; }
    /* реле должно быть локальным: датаграммы голоса не уходят мимо туннеля */
    if ((ntohl(ra->sin_addr.s_addr) >> 24) != 127) { why = "SOCKS5: реле UDP не на 127.0.0.0/8"; goto done; }
    ok = true;
done:
    wipe(b, sizeof b);
    if (!ok) {
        if (s->tcp != SK_BAD) sk_close(s->tcp);
        socks_reset(s);
        snprintf(err, errn, "%s", atomic_load(stop) ? "прервано" : why);
        return false;
    }
    s->ready = true;
    return true;
}

/* Обернуть датаграмму для реле. Возвращает длину или -1. */
static int socks_wrap(const Socks *s, const void *p, size_t n, unsigned char *out, size_t cap) {
    if (!s->ready || n + SOCKS_HDR > cap) return -1;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
    memcpy(out + 4, s->dst_ip, 4);
    memcpy(out + 8, s->dst_port, 2);
    memcpy(out + SOCKS_HDR, p, n);
    return (int)(n + SOCKS_HDR);
}

/* Снять заголовок. Возвращает смещение полезных данных в pkt или -1 (отбросить). */
static int socks_unwrap(const Socks *s, const unsigned char *pkt, int n) {
    if (!s->ready || n < 4) return -1;
    if (pkt[0] || pkt[1]) return -1;             /* RSV */
    if (pkt[2]) return -1;                       /* фрагменты не собираем */
    int off;
    switch (pkt[3]) {
    case 1: off = 4 + 4 + 2; break;
    case 4: off = 4 + 16 + 2; break;
    case 3: if (n < 5) return -1; off = 4 + 1 + pkt[4] + 2; break;
    default: return -1;
    }
    if (n < off) return -1;
    /* только от голосового сервера: иначе любая локальная программа подсовывала бы пакеты */
    if (pkt[3] != 1 || memcmp(pkt + 4, s->dst_ip, 4) || memcmp(pkt + 8, s->dst_port, 2)) return -1;
    return off;
}

/* Соединение управления живо? Читает и отбрасывает всё, что пришло (по протоколу — ничего). */
static bool socks_alive(Socks *s) {
    if (!s->ready) return false;
    for (int guard = 0; guard < 16; guard++) {
        char b[64];
        int r = (int)recv(s->tcp, b, sizeof b, 0);
        if (r > 0) continue;
        if (r == 0) return false;
        return sk_again();
    }
    return true;
}

static void socks_close(Socks *s) {
    if (s->tcp != SK_BAD) sk_close(s->tcp);
    wipe(s, sizeof *s);
    socks_reset(s);
}
