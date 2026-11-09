/* tools/sec/leak_buf.c — стенд: остаются ли секреты в освобождённой куче после Buf (util.c).
 *
 * Повторяет ровно то, что делает код продукта:
 *   1) ui.c login_key(): ссылка vless:// и пароль набираются в Buf LG.link / LG.pass по одному
 *      символу (buf_put), затем buf_wipe() — как lg_go(B_LG_VCHECK) / lg_go(B_LG_LOGIN);
 *   2) vless.c build_config(): конфиг туннеля (uuid, pass, pbk, sid) собирается buf_puts() в Buf,
 *      затем buf_wipe().
 * buf_put растит буфер через realloc (64 -> 128 -> 256 ...), старый блок освобождается без затирания;
 * buf_wipe затирает только последний блок. После этого стенд ищет фрагменты секретов в записываемой
 * памяти процесса (тот же обход VirtualQuery, что mem_count() в selftest.c).
 * Секреты — выдуманные (не тестовая ссылка владельца). Только стенд; в продукт не входит. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../src/mc.h"

/* заглушки plat.h, нужные util.c */
void plat_wipe(void *p, size_t n) { SecureZeroMemory(p, n); }
#include <time.h>
int64_t plat_mono_ms(void) { return (int64_t)GetTickCount64(); }
int64_t plat_wall_ms(void) { return 0; }
int64_t plat_timegm(struct tm *t) { (void)t; return 0; }
void plat_localtime(int64_t unix_s, struct tm *out) { (void)unix_s; memset(out, 0, sizeof *out); }
void *plat_pages(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void plat_pages_free(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }

static int mem_count(const char *needle, size_t n) {
    int hits = 0;
    MEMORY_BASIC_INFORMATION mi;
    for (char *a = NULL; VirtualQuery(a, &mi, sizeof mi) == sizeof mi; a = (char *)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE) continue;
        if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        if (!(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) continue;
        const char *b = mi.BaseAddress, *e = b + mi.RegionSize;
        for (const char *p = b; p + n <= e; p++) {
            p = memchr(p, needle[0], (size_t)(e - p));
            if (!p || p + n > e) break;
            if (p != needle && !memcmp(p, needle, n)) hits++;
        }
        if ((char *)mi.BaseAddress + mi.RegionSize < (char *)mi.BaseAddress) break;
    }
    return hits;
}

/* Иголку храним «перевёрнутой», чтобы её саму не находить: разворачиваем во временный буфер на стеке
 * и считаем вхождения кроме него (mem_count пропускает адрес needle). */
static int count_frag(const char *rev, size_t n) {
    char nd[64];
    for (size_t i = 0; i < n; i++) nd[i] = rev[n - 1 - i];
    int h = mem_count(nd, n);
    SecureZeroMemory(nd, sizeof nd);
    return h;
}
static void rev(char *d, const char *s) { size_t n = strlen(s); for (size_t i = 0; i < n; i++) d[i] = s[n - 1 - i]; d[n] = 0; }

int main(void) {
    /* выдуманная ссылка (~230 байт, как у Reality) и пароль 100 символов */
    char link[300], pass[128], R1[64], R2[64], R3[64], R4[64];
    /* фрагменты — в перевёрнутом виде; сами строки строим из частей, чтобы в .rdata не было целиком */
    rev(R1, "7f3c9a12-5e4b-4d6a-9c1e-0b8d2f6a4e91");                         /* uuid — в начале ссылки (до 64 байт) */
    rev(R2, "pbk=Zq8xV3mN0pL7rT2wY5uI9oK4jH6gF1dS3aQ");                       /* середина ссылки (64..128) */
    rev(R3, "sid=6b1f0c9e2d7a");                                               /* хвост ссылки (128..256) */
    rev(R4, "Pa55-w0rd-very-long-0123456789-abcdefghij-0123456789-ABCDEFGHIJ"); /* пароль: первые 63 байта */
    char u[64], p1[64], s1[64], pw[64];
    rev(u, R1); rev(p1, R2); rev(s1, R3); rev(pw, R4);
    snprintf(link, sizeof link, "vless://%s@vpn.example.org:443?encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.example.com&fp=chrome&%s&%s&spx=%%2F&type=tcp#test-server-name-x", u, p1, s1);
    snprintf(pass, sizeof pass, "%s-tail-of-password-0123456789-0123456789", pw);

    /* 1) ui.c: посимвольный набор в Buf и buf_wipe */
    Buf L = {0}, P = {0};
    for (const char *c = link; *c; c++) buf_put(&L, c, 1);
    for (const char *c = pass; *c; c++) buf_put(&P, c, 1);
    buf_wipe(&L);
    buf_wipe(&P);
    /* 2) vless.c build_config: buf_puts строк "ключ=значение" */
    Buf C = {0};
    buf_puts(&C, "port=10808\nuser=0123456789abcdef0123456789abcdef\npass=fedcba9876543210fedcba9876543210\n");
    buf_puts(&C, "uuid="); buf_puts(&C, u); buf_puts(&C, "\nhost=vpn.example.org\nrport=443\nenc=none\nflow=xtls-rprx-vision\n");
    buf_puts(&C, "type=tcp\nsecurity=reality\nfp=chrome\nsni=www.example.com\n"); buf_puts(&C, p1); buf_puts(&C, "\n"); buf_puts(&C, s1); buf_puts(&C, "\n\n");
    buf_wipe(&C);
    SecureZeroMemory(link, sizeof link); SecureZeroMemory(pass, sizeof pass);
    SecureZeroMemory(u, sizeof u); SecureZeroMemory(p1, sizeof p1); SecureZeroMemory(s1, sizeof s1); SecureZeroMemory(pw, sizeof pw);

    /* фрагменты по 16 байт: из разных «поколений» буфера */
    int h1 = count_frag(R1 + strlen(R1) - 16, 16);   /* начало uuid */
    int h2 = count_frag(R2 + strlen(R2) - 20, 16);   /* pbk */
    int h3 = count_frag(R3, 12);                     /* хвост sid */
    int h4 = count_frag(R4 + strlen(R4) - 24, 16);   /* пароль */
    printf("после buf_wipe() в куче осталось копий: uuid %d, pbk %d, sid %d, пароль %d\n", h1, h2, h3, h4);
    printf("%s\n", h1 + h2 + h3 + h4 ? "ПОДТВЕРЖДЕНО: realloc в buf_put оставляет секреты в освобождённой куче"
                                     : "копий не найдено");
    return 0;
}
