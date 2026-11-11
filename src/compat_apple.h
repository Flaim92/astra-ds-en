/* macOS: недостающие в Darwin вызовы Linux, которыми пользуется переносимый код.
 * Подключается к каждому файлу ключом -include (build.sh, только на macOS); на Windows и Linux не участвует.
 * Сначала системные заголовки, потом обёртки: макросы ниже не задевают объявления в самих заголовках. */
#ifndef COMPAT_APPLE_H
#define COMPAT_APPLE_H
#ifdef __APPLE__
#define __STDC_WANT_LIB_EXT1__ 1                /* memset_s */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <mach-o/dyld.h>

/* pipe2: pipe + флаги на оба конца */
static inline int astra_pipe2(int fd[2], int flags) {
    if (pipe(fd)) return -1;
    for (int i = 0; i < 2; i++) {
        if ((flags & O_CLOEXEC) && fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
        if ((flags & O_NONBLOCK) && fcntl(fd[i], F_SETFL, fcntl(fd[i], F_GETFL) | O_NONBLOCK) < 0) goto fail;
    }
    return 0;
fail:
    close(fd[0]);
    close(fd[1]);
    return -1;
}
#define pipe2(fd, fl) astra_pipe2(fd, fl)

/* socket(): флаги типа Linux SOCK_CLOEXEC / SOCK_NONBLOCK; потоковым сокетам — без SIGPIPE */
#define SOCK_CLOEXEC 0x10000000
#define SOCK_NONBLOCK 0x20000000
#ifndef MSG_NOSIGNAL                           /* новые SDK его уже определяют */
#define MSG_NOSIGNAL 0
#endif
static inline int astra_socket(int dom, int type, int proto) {
    int fl = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    int s = socket(dom, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), proto);
    if (s < 0) return -1;
    if ((fl & SOCK_CLOEXEC) && fcntl(s, F_SETFD, FD_CLOEXEC) < 0) { close(s); return -1; }
    if ((fl & SOCK_NONBLOCK) && fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) < 0) { close(s); return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return s;
}
#define socket(d, t, p) astra_socket(d, t, p)

/* accept4: accept + флаги */
static inline int astra_accept4(int ls, struct sockaddr *a, socklen_t *al, int fl) {
    int s = accept(ls, a, al);
    if (s < 0) return -1;
    if ((fl & SOCK_CLOEXEC) && fcntl(s, F_SETFD, FD_CLOEXEC) < 0) { close(s); return -1; }
    if ((fl & SOCK_NONBLOCK) && fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) < 0) { close(s); return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return s;
}
#define accept4(l, a, al, f) astra_accept4(l, a, al, f)

/* getrandom: getentropy отдаёт не больше 256 байт за вызов */
#define GRND_NONBLOCK 1
static inline ssize_t astra_getrandom(void *p, size_t n, unsigned flags) {
    (void)flags;
    unsigned char *b = p;
    for (size_t off = 0; off < n;) {
        size_t k = n - off > 256 ? 256 : n - off;
        if (getentropy(b + off, k)) return -1;
        off += k;
    }
    return (ssize_t)n;
}
#define getrandom(p, n, f) astra_getrandom(p, n, f)

/* explicit_bzero: memset_s компилятор не выбрасывает */
static inline void astra_explicit_bzero(void *p, size_t n) { if (p && n) memset_s(p, n, 0, n); }
#define explicit_bzero(p, n) astra_explicit_bzero(p, n)

/* readlink("/proc/self/exe"): путь программы — у Darwin через dyld (без завершающего нуля, как readlink) */
static inline ssize_t astra_readlink(const char *path, char *buf, size_t n) {
    if (strcmp(path, "/proc/self/exe")) return readlink(path, buf, n);
    char raw[PATH_MAX], real[PATH_MAX];
    uint32_t rn = sizeof raw;
    if (_NSGetExecutablePath(raw, &rn) != 0 || !realpath(raw, real)) { errno = ENOENT; return -1; }
    size_t k = strlen(real);
    if (k > n) k = n;
    memcpy(buf, real, k);
    return (ssize_t)k;
}
#define readlink(p, b, n) astra_readlink(p, b, n)

#endif
#endif
