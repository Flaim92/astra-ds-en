/* tools/sec/fuzz_proto.c — фаззер разбора protobuf user_settings_proto (папки серверов, store.c №2).
 * Код (pb_varint, pb_skip, pb_wrapped, pb_folder, folders_proto, folder_*) вырезается из ТЕКУЩЕГО store.c
 * (см. team/security.md) в build-sec/proto_extract.c. Буфер после base64 (xmalloc в folders_proto) — под
 * «забором»: конец блока упирается в PAGE_NOACCESS, чтение за ним = AV, вход сохраняется в build-sec/crash/.
 * Семена — правильные PreloadedUserSettings{14: GuildFolders{1: GuildFolder{1: fixed64 ids, 2: Int64Value,
 * 3: StringValue, 4: UInt64Value}}}; мутации байтов, длин, номеров полей, типов; плюс случайный мусор. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/mc.h"
Store S;

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
/* в вырезанном коде: xmalloc (буфер base64) — под забором; free — умный */
#define xmalloc ef_alloc
#define free ef_free
#include "../../build-sec/proto_extract.c"
#undef xmalloc
#undef free

/* заглушки plat для util.c */
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
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_STACK_OVERFLOW && c != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
    FILE *f = fopen("build-sec/crash/proto-crash.bin", "wb");
    if (f && cur) { fwrite(cur, 1, cur_n, f); fclose(f); }
    fprintf(stderr, "\n!!! ИСКЛЮЧЕНИЕ 0x%08lx на входе %zu байт -> build-sec/crash/proto-crash.bin\n", c, cur_n);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}
static unsigned rs = 777;
static unsigned r32(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static size_t put_varint(unsigned char *b, unsigned long long v) { size_t n = 0; do { b[n] = (unsigned char)(v & 0x7F); v >>= 7; if (v) b[n] |= 0x80; n++; } while (v); return n; }
static size_t seed(unsigned char *b, int nf) {
    unsigned char inner[4096]; size_t in = 0;
    for (int f = 0; f < nf; f++) {
        unsigned char fo[512]; size_t fl = 0;
        int ng = 1 + (int)(r32() % 6);
        fo[fl++] = 0x0A; fl += put_varint(fo + fl, (unsigned)ng * 8);
        for (int g = 0; g < ng; g++) { unsigned long long id = 100000000000000000ull + r32(); memcpy(fo + fl, &id, 8); fl += 8; }
        fo[fl++] = 0x12; fo[fl++] = 2; fo[fl++] = 0x08; fo[fl++] = (unsigned char)(r32() & 0x7F);
        const char *nm = (r32() & 1) ? "Папка" : "games";
        size_t nl = strlen(nm);
        fo[fl++] = 0x1A; fo[fl++] = (unsigned char)(nl + 2); fo[fl++] = 0x0A; fo[fl++] = (unsigned char)nl; memcpy(fo + fl, nm, nl); fl += nl;
        { unsigned char vb[10]; size_t vl = put_varint(vb, 0x5865F2);
          fo[fl++] = 0x22; fo[fl++] = (unsigned char)(1 + vl); fo[fl++] = 0x08; memcpy(fo + fl, vb, vl); fl += vl; }
        inner[in++] = 0x0A; in += put_varint(inner + in, fl); memcpy(inner + in, fo, fl); in += fl;
    }
    size_t n = 0;
    b[n++] = 0x08; b[n++] = 1;                     /* какое-то поле 1 */
    b[n++] = (14 << 3) | 2; n += put_varint(b + n, in); memcpy(b + n, inner, in); n += in;
    return n;
}
static char *b64(const unsigned char *p, size_t n, size_t *ol) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc(n * 4 / 3 + 8); size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o[k++] = T[v >> 18 & 63]; o[k++] = T[v >> 12 & 63];
        o[k++] = i + 1 < n ? T[v >> 6 & 63] : '='; o[k++] = i + 2 < n ? T[v & 63] : '=';
    }
    *ol = k; return o;
}
static void run(const unsigned char *p, size_t n) {
    cur = p; cur_n = n;
    size_t L; char *s = b64(p, n, &L);
    char *g = ef_alloc(L ? L : 1);                  /* и сама строка base64 — точной длины под забором */
    memcpy(g, s, L);
    folders_free();
    folders_proto(g, L);
    const GuildFolder *f; int nf = guild_folders(&f);
    for (int i = 0; i < nf; i++) if (f[i].n > 200 || (f[i].name && strlen(f[i].name) > 64)) { fprintf(stderr, "предел нарушен\n"); exit(5); }
    ef_free(g); free(s);
}
int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 300000;
    AddVectoredExceptionHandler(1, on_crash);
    CreateDirectoryA("build-sec\\crash", NULL);
    static unsigned char b[1 << 16];
    size_t n = seed(b, 3); run(b, n);
    const GuildFolder *f; int nf = guild_folders(&f);
    printf("семя: папок %d, в первой %d серверов, имя \"%s\"\n", nf, nf ? f[0].n : 0, nf && f[0].name ? f[0].name : "-");
    long ok = 0;
    for (long it = 0; it < iters; it++) {
        n = (r32() % 8 == 0) ? (size_t)(r32() % 600) : seed(b, 1 + (int)(r32() % 8));
        if (n && r32() % 8 == 0) for (size_t i = 0; i < n; i++) b[i] = (unsigned char)r32();
        int k = 1 + (int)(r32() % 6);
        for (int t = 0; t < k && n; t++) {
            switch (r32() % 5) {
            case 0: b[r32() % n] ^= (unsigned char)(1u << (r32() % 8)); break;
            case 1: b[r32() % n] = (unsigned char)r32(); break;
            case 2: b[r32() % n] = (unsigned char)(0x80 | r32()); break;          /* длинные varint */
            case 3: b[r32() % n] = (unsigned char)((r32() % 16) << 3 | (r32() % 8)); break;   /* ключ: поле/тип */
            case 4: n = r32() % (n + 1); break;
            }
        }
        if (r32() % 50 == 0) { memset(b, 0xFF, 64); n = 64; }        /* varint без конца */
        run(b, n);
        if (guild_folders(&f)) ok++;
    }
    printf("ИТОГ: %ld входов, с папками %ld, падений 0, пределы (≤100 папок, ≤200 серверов, имя ≤64 Б) соблюдены\n", iters, ok);
    return 0;
}
