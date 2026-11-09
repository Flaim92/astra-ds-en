/* tools/sec/fuzz_ogg.c — фаззер потокового разбора Ogg/Opus голосовых сообщений (voice.c №69).
 * Код (VmDec, vm_packet, vm_pages, vm_feed, vm_decode, vm_dec_free) вырезается из ТЕКУЩЕГО voice.c
 * (см. team/security.md) в build-sec/ogg_extract.c. Буферы og (70 КБ) и pq (64 КБ) — под «забором»
 * (конец упирается в PAGE_NOACCESS). Поток подаётся кусками случайной длины (как curl), между кусками —
 * vm_decode (настоящий libopus). Семена — правильные Ogg-страницы: OpusHead, OpusTags, пакеты
 * (в т.ч. через несколько страниц, 255-сегменты, гигантские пакеты > 8 КБ, page_segments = 255).
 * Мутации: байты, длины сегментов, число сегментов, усечение, «OggS» в середине. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus/opus.h>
#include "../../src/mc.h"

#define PG 4096u
#define EF_MAGIC 0x5EC5EC5EC5EC5EC5ull
static void *ef_alloc(size_t n) {
    size_t rn = (n + 15) & ~(size_t)15, dp = (rn + 32 + PG - 1) / PG;
    unsigned char *b = VirtualAlloc(NULL, (dp + 1) * PG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD o; VirtualProtect(b + dp * PG, PG, PAGE_NOACCESS, &o);
    unsigned char *p = b + dp * PG - rn;
    ((size_t *)p)[-1] = (size_t)b; ((unsigned long long *)p)[-2] = EF_MAGIC;
    return p;
}
static void ef_free(void *v) {
    if (!v) return;
    /* свой блок или из обычной кучи? Смотрим, что за память по адресу (а не по выравниванию: блок
     * кратного странице размера начинается ровно с начала страницы) */
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(v, &mi, sizeof mi) == sizeof mi && mi.Type == MEM_PRIVATE && mi.AllocationBase == (void *)((size_t *)v)[-1] &&
        ((unsigned long long *)v)[-2] == EF_MAGIC) { VirtualFree(mi.AllocationBase, 0, MEM_RELEASE); return; }
    free(v);
}
#define xmalloc ef_alloc
#define free ef_free
#include "../../build-sec/ogg_extract.c"
#undef xmalloc
#undef free

void plat_wipe(void *p, size_t n) { SecureZeroMemory(p, n); }
int64_t plat_mono_ms(void) { return 0; }
int64_t plat_wall_ms(void) { return 0; }
int64_t plat_timegm(struct tm *t) { (void)t; return 0; }
void plat_localtime(int64_t s, struct tm *o) { (void)s; memset(o, 0, sizeof *o); }
void *plat_pages(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void plat_pages_free(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }

static const unsigned char *cur; static size_t cur_n;
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_STACK_OVERFLOW && c != 0xC0000409 && c != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    FILE *f = fopen("build-sec/crash/ogg-crash.bin", "wb");
    if (f && cur) { fwrite(cur, 1, cur_n, f); fclose(f); }
    fprintf(stderr, "\n!!! ИСКЛЮЧЕНИЕ 0x%08lx на входе %zu байт -> build-sec/crash/ogg-crash.bin\n", c, cur_n);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}
static unsigned rs = 4242;
static unsigned r32(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* Ogg-страница с пакетами (без CRC — разборщик её не проверяет) */
static size_t page(unsigned char *o, const unsigned char *const *pk, const size_t *pl, int np, bool cont_last) {
    unsigned char seg[255]; int ns = 0; size_t body = 0;
    for (int i = 0; i < np; i++) {
        size_t l = pl[i];
        while (l >= 255 && ns < 255) { seg[ns++] = 255; l -= 255; }
        if (ns < 255 && !(cont_last && i == np - 1)) seg[ns++] = (unsigned char)l;
        body += pl[i];
    }
    memcpy(o, "OggS", 4); o[4] = 0; o[5] = 0; memset(o + 6, 0, 20); o[26] = (unsigned char)ns;
    memcpy(o + 27, seg, (size_t)ns);
    size_t k = 27 + (size_t)ns;
    for (int i = 0; i < np; i++) { memcpy(o + k, pk[i], pl[i]); k += pl[i]; }
    (void)body;
    return k;
}
static size_t make_stream(unsigned char *o, size_t cap) {
    size_t n = 0;
    unsigned char head[19] = { 'O','p','u','s','H','e','a','d', 1, (unsigned char)(1 + r32() % 2), 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0 };
    const unsigned char *p1[1] = { head }; size_t l1[1] = { 19 };
    n += page(o + n, p1, l1, 1, false);
    static unsigned char tags[20000];
    memcpy(tags, "OpusTags", 8);
    size_t tl = (r32() % 4 == 0) ? 9000 + r32() % 9000 : 16;      /* иногда OpusTags > 8 КБ (обложка) */
    const unsigned char *p2[1] = { tags }; size_t l2[1] = { tl };
    if (tl < 255 * 254) n += page(o + n, p2, l2, 1, false);
    /* звук: тишина Opus (TOC 0xF8 + 0xFF 0xFE) и случайные пакеты, иногда гигантские */
    static unsigned char pk[70000];
    int np = 3 + (int)(r32() % 40);
    for (int i = 0; i < np && n + 70000 < cap; i++) {
        size_t l = (r32() % 10 == 0) ? 8000 + r32() % 60000 : 3 + r32() % 300;
        for (size_t k = 0; k < l; k++) pk[k] = (unsigned char)r32();
        pk[0] = 0xF8;
        const unsigned char *pp[1] = { pk }; size_t ll[1] = { l > 65024 ? 65024 : l };
        n += page(o + n, pp, ll, 1, false);
    }
    return n;
}
static long runs, bad_runs, max_pqn;
static void run(const unsigned char *s, size_t n) {
    cur = s; cur_n = n;
    VmDec *d = ef_alloc(sizeof *d);
    memset(d, 0, sizeof *d);
    size_t off = 0;
    bool stall = r32() % 4 == 0;                  /* вывод стоит: очередь пакетов растёт до предела */
    while (off < n) {
        size_t k = 1 + r32() % 20000;                /* куски как у curl (до 16 КБ) и мельче */
        if (k > n - off) k = n - off;
        if (!vm_feed(d, s + off, k)) break;
        if ((long)d->pqn > max_pqn) max_pqn = (long)d->pqn;
        if (!stall) vm_decode(d);
        if (d->rn > VM_RING) { fprintf(stderr, "кольцо переполнено\n"); exit(5); }
        if (d->rn > VM_RING / 2) { d->rh = (d->rh + d->rn) % VM_RING; d->rn = 0; }    /* «вывод» */
        off += k;
    }
    runs++;
    if (d->bad) bad_runs++;
    vm_dec_free(d);
    ef_free(d);
}
int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    AddVectoredExceptionHandler(1, on_crash);
    CreateDirectoryA("build-sec\\crash", NULL);
    static unsigned char s[4 << 20];
    for (long it = 0; it < iters; it++) {
        size_t n = make_stream(s, sizeof s);
        int muts = (int)(r32() % 8);
        for (int t = 0; t < muts && n; t++) {
            switch (r32() % 6) {
            case 0: s[r32() % n] = (unsigned char)r32(); break;
            case 1: s[r32() % n] ^= (unsigned char)(1u << (r32() % 8)); break;
            case 2: { size_t at = r32() % n; if (at + 27 < n) s[at] = 255; } break;      /* «длина сегмента» */
            case 3: n = r32() % n; break;                                                 /* неполная страница */
            case 4: { size_t at = r32() % n; if (at + 30 < n) { memcpy(s + at, "OggS\0", 5); s[at + 26] = (unsigned char)r32(); } } break;
            case 5: { char *q = memchr(s + 1, 'O', n - 1); if (q && (size_t)((unsigned char *)q - s) + 27 < n) ((unsigned char *)q)[26] = 255; } break;
            }
        }
        run(s, n);
        if ((it + 1) % 5000 == 0) { printf("  %ld: испорчено (отвергнуто) %ld, пик очереди %ld Б\n", it + 1, bad_runs, max_pqn); fflush(stdout); }
    }
    printf("ИТОГ: потоков %ld, отвергнуто как испорченные %ld, падений 0, пик очереди пакетов %ld Б (предел %u)\n", runs, bad_runs, max_pqn, VM_PQ_CAP);
    return 0;
}
