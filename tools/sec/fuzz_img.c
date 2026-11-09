/* tools/sec/fuzz_img.c — фаззер декодера картинок src/img.c (libFuzzer/ASan под MinGW нет).
 *
 * Как устроено: img.c включается целиком (#include), а malloc/calloc/realloc/free внутри него
 * подменены «электрическим забором»: каждый блок — в своих страницах VirtualAlloc, конец блока
 * упирается в страницу PAGE_NOACCESS, освобождённые страницы снимаются (VirtualFree) — выход за
 * конец блока и обращение после free дают AV сразу. Вся память libjpeg идёт через jm_get() ->
 * malloc img.c, т.е. тоже под забором (кроме выходов внутри одного пула alloc_small).
 * При AV векторный обработчик сохраняет текущий вход в <out>/crash-<n>.jpg.
 *
 * Проверки на каждом входе: нет падения; s_jmem == 0 после разбора (всё освобождено); клетки
 * результата — только 0x20..0x7E; пик памяти libjpeg <= DEC_MEM_MAX; время разбора (худшее).
 * Мутации: биты, байты, вставка/удаление/дублирование кусков, правка полей SOF (размеры, число
 * компонент, дискретизация, точность), SOS (Ss/Se/Ah/Al), DRI, DHT/DQT, усечение.
 *
 * Запуск: tools/sec/run_fuzz.sh. Только стенд безопасности; в продукт не входит. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

/* ---------- электрический забор ---------- */
#define PG 4096u
static size_t g_live;                           /* живых блоков */
static void *ef_alloc(size_t n) {
    size_t rn = (n + 15) & ~(size_t)15;
    if (rn < n) return NULL;
    size_t data_pages = (rn + 16 + PG - 1) / PG;
    uint8_t *base = VirtualAlloc(NULL, (data_pages + 1) * PG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) return NULL;
    DWORD old;
    VirtualProtect(base + data_pages * PG, PG, PAGE_NOACCESS, &old);
    uint8_t *p = base + data_pages * PG - rn;
    ((size_t *)p)[-2] = n;
    ((size_t *)p)[-1] = (size_t)base;
    memset(p, 0xA5, rn);                        /* «мусор»: чтение неинициализированного видно по результату */
    g_live++;
    return p;
}
static void ef_free(void *v) {
    if (!v) return;
    uint8_t *p = v;
    uint8_t *base = (uint8_t *)((size_t *)p)[-1];
    g_live--;
    VirtualFree(base, 0, MEM_RELEASE);
}
static void *ef_calloc(size_t a, size_t b) {
    if (b && a > SIZE_MAX / b) return NULL;
    void *p = ef_alloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}
static void *ef_realloc(void *v, size_t n) {
    if (!v) return ef_alloc(n);
    size_t o = ((size_t *)v)[-2];
    void *q = ef_alloc(n);
    if (!q) return NULL;
    memcpy(q, v, o < n ? o : n);
    ef_free(v);
    return q;
}

#define malloc ef_alloc
#define calloc ef_calloc
#define realloc ef_realloc
#define free ef_free
#include "../../src/img.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

/* ---------- заглушки остального mincord ---------- */
Opts O;
void wipe(void *p, size_t n) { volatile unsigned char *c = p; while (n--) *c++ = 0; }
void plat_wipe(void *p, size_t n) { wipe(p, n); }
i64 now_ms(void) { return (i64)GetTickCount64(); }
char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *p = ef_alloc(n); memcpy(p, s, n); return p; }
void ui_dirty(void) {}
VlessState vless_state(void) { return VL_OFF; }
const char *vless_proxy_url(void) { return ""; }
void net_curl_common(CURL *e) { (void)e; }

/* ---------- текущий вход и сохранение при падении ---------- */
static const uint8_t *cur;
static size_t cur_n;
static int cur_w, cur_h;
static const char *outdir = "build-sec/crash";
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */) return EXCEPTION_CONTINUE_SEARCH;
    char p[1024];
    snprintf(p, sizeof p, "%s/crash-%lu-%dx%d.jpg", outdir, (unsigned long)GetTickCount(), cur_w, cur_h);
    FILE *f = fopen(p, "wb");
    if (f && cur) { fwrite(cur, 1, cur_n, f); fclose(f); }
    fprintf(stderr, "\n!!! ИСКЛЮЧЕНИЕ 0x%08lx по адресу %p (чтение/запись %p) на входе %zu байт, клетки %dx%d -> %s\n",
            code, ep->ExceptionRecord->ExceptionAddress,
            ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL,
            cur_n, cur_w, cur_h, p);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------- статистика ---------- */
static long n_runs, n_ok, n_leak, n_badcell;
static double worst_ms;
static size_t worst_peak;
static uint8_t *worst_in;
static size_t worst_n;
static int worst_w, worst_h;
static LARGE_INTEGER qf;

static void run(const uint8_t *in, size_t n, int w, int h) {
    /* и вход, и выход — точно своего размера под забором: чтение за концом JPEG и запись за w*h клеток видны */
    ImgCell *cells = ef_alloc(sizeof(ImgCell) * (size_t)w * (size_t)h);
    uint8_t *gin = ef_alloc(n ? n : 1);
    memcpy(gin, in, n);
    const uint8_t *orig = in;
    in = gin;
    cur = orig; cur_n = n; cur_w = w; cur_h = h;
    size_t live0 = g_live;
    memset(cells, 0x7f, sizeof(ImgCell) * (size_t)w * (size_t)h);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    bool ok = img_decode(in, n, w, h, cells);
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)qf.QuadPart;
    n_runs++;
    if (ok) {
        n_ok++;
        for (int i = 0; i < w * h; i++) if (cells[i].ch < 0x20 || cells[i].ch > 0x7E) { n_badcell++; break; }
    }
    if (s_jmem != 0 || g_live != live0) {
        n_leak++;
        fprintf(stderr, "утечка: s_jmem=%zu, блоков %zu -> %zu\n", s_jmem, live0, g_live);
        s_jmem = 0;
    }
    if (img_decode_peak() > worst_peak) worst_peak = img_decode_peak();
    if (ms > worst_ms) {
        worst_ms = ms;
        free(worst_in);
        worst_in = malloc(n);
        memcpy(worst_in, in, n);
        worst_n = n; worst_w = w; worst_h = h;
    }
    ef_free(gin);
    ef_free(cells);
}

/* ---------- корпус ---------- */
typedef struct { uint8_t *p; size_t n; char name[260]; } Seed;
static Seed *seeds;
static int nseeds, capseeds;
static void add_file(const char *path, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (long)DL_MAX) { fclose(f); return; }
    if (nseeds == capseeds) { capseeds = capseeds ? capseeds * 2 : 64; seeds = realloc(seeds, sizeof *seeds * (size_t)capseeds); }
    Seed *s = &seeds[nseeds];
    s->p = malloc((size_t)n);
    s->n = fread(s->p, 1, (size_t)n, f);
    snprintf(s->name, sizeof s->name, "%s", name);
    fclose(f);
    nseeds++;
}
static void add_dir(const char *dir) {
    char pat[1024];
    snprintf(pat, sizeof pat, "%s/*.jpg", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, fd.cFileName);
        add_file(p, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static uint32_t rs = 0xC0FFEEu;
static uint32_t rnd32(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static size_t rndn(size_t n) { return n ? rnd32() % n : 0; }

/* найти маркер m (FF m) — смещение байта FF или -1 */
static long find_marker(const uint8_t *p, size_t n, int m, size_t from) {
    for (size_t i = from; i + 1 < n; i++) if (p[i] == 0xFF && p[i + 1] == m) return (long)i;
    return -1;
}
static long find_sof(const uint8_t *p, size_t n) {
    for (size_t i = 2; i + 1 < n; i++)
        if (p[i] == 0xFF && p[i + 1] >= 0xC0 && p[i + 1] <= 0xCF && p[i + 1] != 0xC4 && p[i + 1] != 0xC8 && p[i + 1] != 0xCC) return (long)i;
    return -1;
}

static size_t mutate(uint8_t *b, size_t n, size_t cap) {
    int k = 1 + (int)rndn(6);
    for (int t = 0; t < k; t++) {
        int op = (int)rndn(14);
        if (n < 8) op = 0;
        switch (op) {
        case 0: if (n > 2) b[2 + rndn(n - 2)] ^= (uint8_t)(1u << rndn(8)); break;
        case 1: if (n > 2) b[2 + rndn(n - 2)] = (uint8_t)rnd32(); break;
        case 2: { static const uint8_t I[] = { 0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x10, 0x20, 0x40, 8, 12, 16 };
                  if (n > 2) b[2 + rndn(n - 2)] = I[rndn(sizeof I)]; break; }
        case 3: { size_t at = 2 + rndn(n - 2), len = 1 + rndn(64); if (at + len > n) len = n - at;
                  memmove(b + at, b + at + len, n - at - len); n -= len; break; }
        case 4: { size_t at = 2 + rndn(n - 2), len = 1 + rndn(256); if (n + len > cap) break;
                  memmove(b + at + len, b + at, n - at); for (size_t i = 0; i < len; i++) b[at + i] = (uint8_t)rnd32(); n += len; break; }
        case 5: { size_t from = 2 + rndn(n - 2), len = 1 + rndn(4096), at = 2 + rndn(n - 2);   /* дублировать кусок (скан, DHT...) */
                  if (from + len > n) len = n - from; if (n + len > cap) break;
                  uint8_t *tmp = malloc(len); memcpy(tmp, b + from, len);
                  memmove(b + at + len, b + at, n - at); memcpy(b + at, tmp, len); free(tmp); n += len; break; }
        case 6: n = 2 + rndn(n - 2); break;                                   /* усечение */
        case 7: { long s = find_sof(b, n); if (s < 0 || (size_t)s + 9 > n) break;   /* размеры */
                  static const int D[] = { 0, 1, 2, 7, 8, 9, 16, 17, 255, 256, 511, 512, 513, 1024, 32767, 65535 };
                  int v = D[rndn(16)]; size_t off = (size_t)s + 5 + 2 * rndn(2); b[off] = (uint8_t)(v >> 8); b[off + 1] = (uint8_t)v; break; }
        case 8: { long s = find_sof(b, n); if (s < 0 || (size_t)s + 10 > n) break;  /* число компонент/точность */
                  if (rnd32() & 1) b[s + 9] = (uint8_t)rndn(6); else b[s + 4] = (uint8_t)(rnd32() & 1 ? 12 : rndn(17)); break; }
        case 9: { long s = find_sof(b, n); if (s < 0) break;                        /* дискретизация компоненты */
                  size_t off = (size_t)s + 11 + 3 * rndn(4); if (off < n) b[off] = (uint8_t)(((1 + rndn(4)) << 4) | (1 + rndn(4)));
                  if (rndn(8) == 0 && off < n) b[off] = (uint8_t)rnd32(); break; }
        case 10: { long s = find_sof(b, n); if (s < 0 || (size_t)s + 1 >= n) break;  /* тип кадра: базовый/прогр./lossless */
                   static const uint8_t T[] = { 0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC7, 0xC9, 0xCA, 0xCB };
                   b[s + 1] = T[rndn(sizeof T)]; break; }
        case 11: { long s = find_marker(b, n, 0xDA, 2 + rndn(n / 2)); if (s < 0) s = find_marker(b, n, 0xDA, 2); if (s < 0) break;
                   size_t ln = s + 4 < (long)n ? b[s + 4] : 0, off = (size_t)s + 5 + 2 * ln;      /* Ss Se AhAl */
                   if (off + 2 < n) b[off + rndn(3)] = (uint8_t)(rnd32() & 1 ? rndn(64) : rnd32()); break; }
        case 12: { long s = find_marker(b, n, 0xDD, 2); if (s >= 0 && (size_t)s + 6 <= n) { b[s + 4] = (uint8_t)rndn(3); b[s + 5] = (uint8_t)rnd32(); } break; }
        case 13: { long s = find_marker(b, n, rnd32() & 1 ? 0xC4 : 0xDB, 2 + rndn(n / 2)); if (s < 0) break;   /* DHT/DQT */
                   size_t off = (size_t)s + 4 + rndn(40); if (off < n) b[off] = (uint8_t)rnd32(); break; }
        }
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selfcheck")) {     /* забор работает? запись на 1 байт за конец -> AV */
        AddVectoredExceptionHandler(1, on_crash);
        char *p = ef_alloc(10);
        puts("selfcheck: пишу за конец блока..."); fflush(stdout);
        ((volatile char *)p)[16] = 1;
        puts("selfcheck: ЗАБОР НЕ СРАБОТАЛ");
        return 1;
    }
    if (argc > 2 && !strcmp(argv[1], "--url")) {     /* att_url: случайные адреса -> только CDN Discord и безопасные символы */
        static const char *const PRE[] = { "https://media.discordapp.net/attachments/", "https://cdn.discordapp.com/attachments/",
                                           "https://media.discordapp.net/", "https://cdn.discordapp.com/", "http://media.discordapp.net/attachments/",
                                           "https://media.discordapp.net.evil/attachments/", "" };
        static const char AL[] = "0123456789/abcdefxyzABZ._-%?&=#@:\\ \"'<>\r\n\t\x01\x7f\xc2\x85\xe2\x80\xae";
        long n = atol(argv[2]), okc = 0, bad = 0;
        char in[600], out[700];
        for (long it = 0; it < n; it++) {
            int k = snprintf(in, sizeof in, "%s", PRE[rndn(7)]);
            if (rnd32() & 1) k += snprintf(in + k, sizeof in - (size_t)k, "%u/%u/", rnd32() % 100000, rnd32());
            int len = (int)rndn(300);
            for (int i = 0; i < len && k < (int)sizeof in - 1; i++) in[k++] = AL[rndn(sizeof AL - 1)];
            in[k] = 0;
            u64 key;
            if (!att_url(in, 1 + (int)rndn(600), 1 + (int)rndn(600), out, sizeof out, &key)) continue;
            okc++;
            bool good = !strncmp(out, "https://media.discordapp.net/attachments/", 41);
            for (const char *p = out; *p && good; p++)
                good = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/:._-%?&=", *p);
            if (!good || strstr(out, "/../") || strstr(out, "@")) { bad++; if (bad < 5) printf("ПЛОХО: [%s] -> [%s]\n", in, out); }
        }
        printf("att_url: %ld адресов, принято %ld, опасных на выходе %ld\n", n, okc, bad);
        return bad != 0;
    }
    if (argc < 4) { fprintf(stderr, "fuzz_img <итераций> <каталог-выхода> <каталог-корпуса>...\n"); return 2; }
    long iters = atol(argv[1]);
    outdir = argv[2];
    QueryPerformanceFrequency(&qf);
    AddVectoredExceptionHandler(1, on_crash);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (int i = 3; i < argc; i++) add_dir(argv[i]);
    if (!nseeds) { fprintf(stderr, "корпус пуст\n"); return 2; }
    printf("корпус: %d файлов\n", nseeds);
    /* фаза 1: каждый файл корпуса на разных сетках (от 1x1 до 256x64) */
    static const int WH[][2] = { { 1, 1 }, { 2, 1 }, { 3, 7 }, { 16, 8 }, { 24, 12 }, { 128, 32 }, { 192, 48 }, { 256, 64 }, { 255, 63 } };
    long ok0 = 0;
    for (int i = 0; i < nseeds; i++)
        for (size_t k = 0; k < sizeof WH / sizeof *WH; k++) {
            long before = n_ok;
            run(seeds[i].p, seeds[i].n, WH[k][0], WH[k][1]);
            if (k == 3 && n_ok > before) ok0++;
        }
    printf("фаза 1: %ld разборов, удачных %ld (файлов корпуса, разобранных в 16x8: %ld из %d), утечек %ld, не-ASCII %ld\n",
           n_runs, n_ok, ok0, nseeds, n_leak, n_badcell);
    /* фаза 2: мутации */
    size_t cap = DL_MAX;
    uint8_t *b = malloc(cap);
    for (long it = 0; it < iters; it++) {
        const Seed *s = &seeds[rndn((size_t)nseeds)];
        memcpy(b, s->p, s->n);
        size_t n = mutate(b, s->n, cap);
        int w = 1 + (int)rndn(rnd32() & 3 ? 32 : IMG_MAX_W), h = 1 + (int)rndn(rnd32() & 3 ? 16 : IMG_MAX_H);
        run(b, n, w, h);
        if ((it + 1) % 20000 == 0) {
            printf("  %ld: удачных %ld, утечек %ld, не-ASCII %ld, худшее время %.1f мс, пик libjpeg %zu Б\n",
                   it + 1, n_ok, n_leak, n_badcell, worst_ms, worst_peak);
            fflush(stdout);
        }
    }
    printf("ИТОГ: разборов %ld, удачных %ld, падений 0, утечек %ld, клеток вне ASCII %ld, худшее время %.1f мс (%zu Б, %dx%d), пик libjpeg %zu Б (предел %u)\n",
           n_runs, n_ok, n_leak, n_badcell, worst_ms, worst_n, worst_w, worst_h, worst_peak, DEC_MEM_MAX);
    if (worst_in) {
        char p[1024];
        snprintf(p, sizeof p, "%s/slowest.jpg", outdir);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(worst_in, 1, worst_n, f); fclose(f); }
    }
    return 0;
}
