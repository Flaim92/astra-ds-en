/* tools/sec/fuzz_scan.c — фаззер самописного сканера JSON из src/store.c (ready_slim, obj_spans,
 * arr_next, event_wanted, presence_is_friend). Код сканера НЕ копируется руками: run_scan.sh
 * вырезает его из store.c (по маркерам) в build-sec/scan_extract.c, этот файл его включает.
 *
 * Буфер текста — как у G.text в net.c: len байт + завершающий 0 (buf_put), сразу за ним страница
 * PAGE_NOACCESS: любая запись или чтение за пределом даёт AV, вход сохраняется в build-sec/crash/.
 * Проверки: ready_slim не пишет за len (w <= len), не возвращает > len; если вернул длину, то
 * результат — валидный JSON (cJSON_ParseWithLength) при валидном входе; obj_spans/arr_next — границы
 * внутри буфера. Входы: валидные READY/READY_SUPPLEMENTAL/события + мутации (скобки, кавычки,
 * обратные слэши в конце, пропуск запятых, глубина 10^5, огромные строки, обрезка). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/mc.h"

/* заглушки того, что использует вырезанный код */
static struct { int nrels; } S_ = { 1 };
#define S S_
static const Relation FRIEND = { .id = 42, .type = REL_FRIEND };
const Relation *rel_find(u64 uid) { return uid == 42 ? &FRIEND : NULL; }

#include "../../build-sec/scan_extract.c"
#undef S

#define PG 4096u
static uint8_t *g_base; static size_t g_pages;
static char *gbuf(size_t n) {              /* n байт, конец упирается в запрещённую страницу */
    size_t dp = (n + PG - 1) / PG; if (!dp) dp = 1;
    g_base = VirtualAlloc(NULL, (dp + 1) * PG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD o; VirtualProtect(g_base + dp * PG, PG, PAGE_NOACCESS, &o);
    g_pages = dp + 1;
    return (char *)g_base + dp * PG - n;
}
static void gfree(void) { VirtualFree(g_base, 0, MEM_RELEASE); }

static const char *cur; static size_t cur_n;
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_STACK_OVERFLOW && c != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
    char p[256]; snprintf(p, sizeof p, "build-sec/crash/scan-%lu.json", (unsigned long)GetTickCount());
    FILE *f = fopen(p, "wb"); if (f && cur) { fwrite(cur, 1, cur_n, f); fclose(f); }
    fprintf(stderr, "\n!!! ИСКЛЮЧЕНИЕ 0x%08lx (адрес %p) на входе %zu байт -> %s\n", c,
            ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL, cur_n, p);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* сторож: один вход дольше 5 с — зависание; сохранить вход и выйти (rc=4) */
static volatile LONG64 run_started;
static DWORD WINAPI watchdog(LPVOID a) {
    (void)a;
    for (;;) {
        Sleep(1000);
        LONG64 t = run_started;
        if (t && (LONG64)GetTickCount64() - t > 5000) {
            char p[256]; snprintf(p, sizeof p, "build-sec/crash/scan-hang-%lu.json", (unsigned long)GetTickCount());
            FILE *f = fopen(p, "wb"); if (f && cur) { fwrite(cur, 1, cur_n, f); fclose(f); }
            fprintf(stderr, "\n!!! ЗАВИСАНИЕ > 5 с на входе %zu байт -> %s\n", cur_n, p);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 4);
        }
    }
    return 0;
}

static uint32_t rs = 0x1234567u;
static uint32_t r32(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static size_t rn(size_t n) { return n ? r32() % n : 0; }

static long n_runs, n_slim_ok, n_slim_bad, n_overlen, n_invalid_out;

/* Один вход: ready_slim + обход как в store_ready_text (без cJSON-модели) + event_wanted. */
static void run(const char *in, size_t n) {
    cur = in; cur_n = n;
    run_started = (LONG64)GetTickCount64();
    char *s = gbuf(n + 1);
    memcpy(s, in, n); s[n] = 0;
    n_runs++;
    (void)event_wanted(s, n);
    bool valid_in = false;
    { cJSON *j = cJSON_ParseWithLength(s, n); valid_in = j != NULL; cJSON_Delete(j); }
    size_t m = ready_slim(s, n);
    if (m > n) n_overlen++;
    if (!m) { n_slim_bad++; gfree(); return; }
    n_slim_ok++;
    if (valid_in && m != n) { cJSON *j = cJSON_ParseWithLength(s, m); if (!j) { n_invalid_out++; if (n_invalid_out < 4) fprintf(stderr, "невалидный результат: %.200s\n", in); } cJSON_Delete(j); }
    /* обход, как store_ready_text */
    Slim x = { s, 0, 0, m, false };
    KSpan top[8], ks[48];
    sl_ws(&x);
    if (x.r < m && s[x.r] == '{') {
        int nt = obj_spans(&x, x.r, top, 8);
        const KSpan *d = NULL;
        for (int i = 0; i < nt; i++) if (key_is(&x, &top[i], "d")) d = &top[i];
        if (!x.bad && d && s[d->v0] == '{') {
            int nk = obj_spans(&x, d->v0, ks, 48);
            for (int i = 0; !x.bad && i < nk; i++) {
                if (ks[i].v1 > m || ks[i].k1 > m) { fprintf(stderr, "граница за концом\n"); n_overlen++; }
                if (s[ks[i].v0] != '[') continue;
                size_t pos = ks[i].v0, e0, e1;
                while (arr_next(&x, &pos, ks[i].v1, &e0, &e1)) if (e1 > m) { n_overlen++; break; }
            }
        }
    }
    gfree();
}

/* валидные семена */
static const char *SEEDS[] = {
    "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"me\",\"x\":[1,2,{\"a\":\"\\\"}\"}]},"
    "\"guilds\":[{\"id\":\"10\",\"name\":\"g\",\"channels\":[{\"id\":\"11\",\"type\":0,\"name\":\"c\\\\\",\"junk\":{\"a\":[[[]]]}}],"
    "\"roles\":[{\"id\":\"10\",\"permissions\":\"8\"}],\"voice_states\":[],\"emojis\":[{\"x\":\"]}\"}]},{\"id\":\"20\",\"unavailable\":true}],"
    "\"merged_members\":[[{\"user_id\":\"1\",\"nick\":\"n\",\"roles\":[\"10\"]}],[]],\"private_channels\":[{\"id\":\"5\",\"type\":1,\"recipient_ids\":[\"2\"]}],"
    "\"read_state\":{\"entries\":[{\"id\":\"11\",\"last_message_id\":\"0\"}]},\"session_id\":\"abc\",\"resume_gateway_url\":\"wss://x.discord.gg\"}}",
    "{\"t\":\"READY_SUPPLEMENTAL\",\"s\":2,\"op\":0,\"d\":{\"guilds\":[{\"id\":\"10\",\"voice_states\":[{\"user_id\":\"1\",\"channel_id\":\"11\"}]}],"
    "\"merged_members\":[[{\"user_id\":\"1\",\"user\":{\"id\":\"1\",\"username\":\"u\"}}]],\"merged_presences\":{\"friends\":[]}}}",
    "{\"t\":\"PRESENCE_UPDATE\",\"s\":3,\"op\":0,\"d\":{\"user\":{\"id\":\"42\"},\"status\":\"online\",\"activities\":[{\"name\":\"\\\"}\"}]}}",
    "{\"t\":\"PRESENCE_UPDATE\",\"s\":3,\"op\":0,\"d\":{\"user\":{\"username\":\"\\\"id\\\":\\\"42\\\"\",\"id\":\"7\"},\"status\":\"online\"}}",
    "{\"t\":\"MESSAGE_CREATE\",\"s\":4,\"op\":0,\"d\":{\"content\":\"\\\"t\\\":\\\"READY\\\"\"}}",
    "{\"t\":null,\"s\":null,\"op\":10,\"d\":{\"heartbeat_interval\":41250}}",
};

static size_t mutate(char *b, size_t n, size_t cap) {
    static const char TOK[] = "{}[]\",:\\ 0a\n";
    int k = 1 + (int)rn(5);
    for (int t = 0; t < k; t++) {
        switch (rn(9)) {
        case 0: if (n) b[rn(n)] = TOK[rn(sizeof TOK - 1)]; break;
        case 1: if (n) b[rn(n)] = (char)r32(); break;
        case 2: if (n) { size_t at = rn(n); memmove(b + at, b + at + 1, n - at - 1); n--; } break;   /* удалить символ (запятую, кавычку) */
        case 3: if (n + 1 < cap) { size_t at = rn(n + 1); memmove(b + at + 1, b + at, n - at); b[at] = TOK[rn(sizeof TOK - 1)]; n++; } break;
        case 4: n = rn(n + 1); break;                                                           /* обрезка */
        case 5: { size_t from = rn(n), len = 1 + rn(200), at = rn(n + 1); if (from + len > n) len = n - from;    /* дубль куска */
                  if (n + len >= cap) break; char *tmp = malloc(len); memcpy(tmp, b + from, len);
                  memmove(b + at + len, b + at, n - at); memcpy(b + at, tmp, len); free(tmp); n += len; break; }
        case 6: { char *p = memchr(b, ',', n); if (p && rn(2)) { size_t at = (size_t)(p - b); memmove(p, p + 1, n - at - 1); n--; } break; }  /* пропуск запятой */
        case 7: if (n) { size_t at = n - 1 - rn(n < 8 ? n : 8); b[at] = '\\'; } break;                      /* \ у конца */
        case 8: { char *p = memchr(b, '[', n); if (p && n + 64 < cap) { size_t at = (size_t)(p - b) + 1, len = 1 + rn(60);   /* элементы без запятых */
                  memmove(b + at + len, b + at, n - at); for (size_t i = 0; i < len; i++) b[at + i] = i & 1 ? '}' : '{'; n += len; } break; }
        }
    }
    return n;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    AddVectoredExceptionHandler(1, on_crash);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    setvbuf(stdout, NULL, _IONBF, 0);
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    size_t ns = sizeof SEEDS / sizeof *SEEDS;
    for (size_t i = 0; i < ns; i++) run(SEEDS[i], strlen(SEEDS[i]));
    printf("семена: %ld, ready_slim ok %ld\n", n_runs, n_slim_ok);
    /* крайние: глубина 100000, строка 1 МБ, массив без запятых */
    size_t cap = 4u << 20;
    char *b = malloc(cap);
    {   size_t n = (size_t)snprintf(b, cap, "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"user\":");
        for (int i = 0; i < 100000; i++) b[n++] = '['; for (int i = 0; i < 100000; i++) b[n++] = ']';
        n += (size_t)snprintf(b + n, cap - n, "}}"); run(b, n); }
    {   size_t n = (size_t)snprintf(b, cap, "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"session_id\":\"");
        memset(b + n, 'A', 1u << 20); n += 1u << 20; for (int i = 0; i < 1000; i++) { b[n++] = '\\'; b[n++] = '"'; }
        n += (size_t)snprintf(b + n, cap - n, "\"}}"); run(b, n); }
    {   size_t n = (size_t)snprintf(b, cap, "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"guilds\":[");
        for (int i = 0; i < 100000; i++) { b[n++] = '{'; b[n++] = '}'; }
        n += (size_t)snprintf(b + n, cap - n, "]}}"); run(b, n); }
    printf("крайние: прогнаны (глубина 1e5, строка 1 МБ, 1e5 элементов без запятых)\n");
    for (long it = 0; it < iters; it++) {
        const char *sd = SEEDS[rn(ns)];
        size_t n = strlen(sd);
        memcpy(b, sd, n);
        n = mutate(b, n, 65536);
        run(b, n);
    }
    printf("ИТОГ: прогонов %ld, ready_slim ok %ld / отказ %ld, длина > входа %ld, невалидный JSON на выходе %ld, падений 0\n",
           n_runs, n_slim_ok, n_slim_bad, n_overlen, n_invalid_out);
    return 0;
}
