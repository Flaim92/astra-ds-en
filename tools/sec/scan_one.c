/* tools/sec/scan_one.c — прогнать ready_slim (вырезанный из store.c) на одном файле и показать результат.
 * Сборка: как fuzz_scan (run_scan.sh создаёт build-sec/scan_extract.c). Буфер — len+1 с запрещённой
 * страницей сразу за ним (как G.text). Только стенд. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/mc.h"
static struct { int nrels; } S_ = { 1 };
#define S S_
static const Relation FRIEND = { .id = 42, .type = REL_FRIEND };
const Relation *rel_find(u64 uid) { return uid == 42 ? &FRIEND : NULL; }
#include "../../build-sec/scan_extract.c"
#undef S
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static char in[1 << 22];
    size_t n = fread(in, 1, sizeof in, f);
    fclose(f);
    size_t dp = (n + 1 + 4095) / 4096;
    char *base = VirtualAlloc(NULL, (dp + 1) * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD o; VirtualProtect(base + dp * 4096, 4096, PAGE_NOACCESS, &o);
    char *s = base + dp * 4096 - (n + 1);
    memcpy(s, in, n); s[n] = 0;
    printf("вход %zu байт\n", n); fflush(stdout);
    size_t m = ready_slim(s, n);
    printf("ready_slim -> %zu\n", m);
    if (m && m <= n) printf("выход: %.*s\n", (int)(m < 400 ? m : 400), s);
    return 0;
}
