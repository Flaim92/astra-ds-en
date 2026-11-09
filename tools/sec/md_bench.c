/* tools/sec/md_bench.c — стенд для md_line (store.c, пакет Б): время разбора на враждебных строках и
 * случайный фаззинг с проверкой границ спанов. Код md_* вырезается из текущего store.c (см. team/security.md)
 * в build-sec/md_extract.c. Строка — в буфере точной длины перед запрещённой страницей (чтение за концом = AV). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/mc.h"
#include "../../build-sec/md_extract.c"
static char *gbuf(size_t n, void **base) {
    size_t dp = (n + 4095) / 4096; if (!dp) dp = 1;
    char *b = VirtualAlloc(NULL, (dp + 1) * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD o; VirtualProtect(b + dp * 4096, 4096, PAGE_NOACCESS, &o);
    *base = b;
    return b + dp * 4096 - n;
}
static double run(const char *s, size_t n, int *bad) {
    void *base;
    char *g = gbuf(n, &base);
    memcpy(g, s, n);
    MdLine ln; MdSpan sp[128];
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    int k = md_line(g, n, &ln, sp, 128);
    QueryPerformanceCounter(&b);
    for (int i = 0; i < k; i++) {
        if ((size_t)sp[i].off + sp[i].len > n) (*bad)++;
        if ((sp[i].attr & MD_LINK) && (size_t)sp[i].url_off + sp[i].url_len > n) (*bad)++;
    }
    VirtualFree(base, 0, MEM_RELEASE);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}
int main(void) {
    static char s[4000];
    const char *pats[] = { "*", "**", "*a", "_a", "~~", "||", "[", "[a](", "`", "\\*", "***", "[a](https://x", "*__~~||" };
    int bad = 0;
    for (size_t p = 0; p < sizeof pats / sizeof *pats; p++) {
        size_t n = 0, pl = strlen(pats[p]);
        while (n + pl < 2000) { memcpy(s + n, pats[p], pl); n += pl; }
        printf("2000 Б из \"%s\": %.2f мс\n", pats[p], run(s, n, &bad));
    }
    unsigned r = 12345;
    double worst = 0;
    static const char AL[] = "*_~|`[]()\\ ahttps:/#-.1";
    for (int it = 0; it < 200000; it++) {
        size_t n = 1 + (r = r * 1103515245 + 12345) % 400;
        for (size_t i = 0; i < n; i++) { r = r * 1103515245 + 12345; s[i] = AL[(r >> 16) % (sizeof AL - 1)]; }
        double t = run(s, n, &bad);
        if (t > worst) worst = t;
    }
    printf("фаззинг 200000 строк до 400 Б: выходов спанов за строку %d, худшее %.2f мс, падений 0\n", bad, worst);
    return 0;
}
