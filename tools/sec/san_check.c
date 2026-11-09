/* tools/sec/san_check.c — стенд: что san() (util.c) оставляет от враждебных имён (байты в hex).
 * Сборка: gcc <флаги> tools/sec/san_check.c src/util.c third_party/cJSON.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../src/mc.h"
void plat_wipe(void *p, size_t n) { SecureZeroMemory(p, n); }
int64_t plat_mono_ms(void) { return 0; }
int64_t plat_wall_ms(void) { return 0; }
int64_t plat_timegm(struct tm *t) { (void)t; return 0; }
void plat_localtime(int64_t s, struct tm *o) { (void)s; memset(o, 0, sizeof *o); }
void *plat_pages(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void plat_pages_free(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }
int main(void) {
    static const char *const in[] = {
        "\x1b[31m\xd0\x9c\xd1\x8d\xd0\xbb\xd0\xbb\xd0\xbe\xd1\x80\xd0\xb8\xe2\x80\xae",   /* ESC[31m Мэллори U+202E (как в демо) */
        "\xc2\x9b" "31m",                                        /* C1 CSI */
        "\x01[\xd1\x84]",                                        /* ATT_MARK */
        "a\xe2\x81\xa6" "b\xe2\x80\x8f" "c\xef\xbb\xbf" "d",     /* U+2066, U+200F, U+FEFF */
        "\xc0\x9b" "x",                                          /* overlong ESC */
    };
    for (size_t i = 0; i < sizeof in / sizeof *in; i++) {
        char *s = san(in[i], false);
        bool ctl = false;
        printf("%zu: ", i);
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) { printf("%02x ", *p); if (*p < 0x20 || *p == 0x7f) ctl = true; }
        printf(" -> \"%s\"%s\n", s, ctl ? "  !!! управляющий байт" : "");
        free(s);
    }
    return 0;
}
