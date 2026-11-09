/* tools/sec/wpath_test.c — стенд для Н2: local_wpath() вырезается из src/net.c (run: см. team/security.md)
 * в build-sec/wpath_extract.c и прогоняется на путях-обходах. 0 — принят как локальный, 1 — не найден,
 * 4 — отвергнут как сетевой. Сеть не трогается: функция только разбирает путь и спрашивает тип диска. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../../build-sec/wpath_extract.c"
int main(void) {
    static const char *P[] = {
        "C:\\Windows\\win.ini", "C:/Windows/win.ini", "C:Windows\\win.ini", "win.ini",
        "\\??\\UNC\\127.0.0.1\\c$\\x", "\\\\?\\UNC\\127.0.0.1\\c$\\x", "\\\\.\\C:\\x", "\\??\\C:\\x", "\\GLOBAL??\\C:\\x",
        "//?/UNC/h/s/x", "\\\\h\\s\\x", "C:\\x\\CON", "C:\\x\\..\\..\\..\\Windows\\win.ini", "C:\\x\\\\\\\\h\\s",
        "C:\\Windows\\win.ini:stream", "Q:\\nope\\x", "NUL", "CON", "COM1", "C:\\x\\aux.txt",
    };
    for (size_t i = 0; i < sizeof P / sizeof *P; i++) {
        wchar_t full[1024];
        int r = local_wpath(P[i], full, 1024);
        printf("%-42s -> %d%s%ls\n", P[i], r, r == 0 ? "  " : "", r == 0 ? full : L"");
    }
    /* все буквы дисков машины: тип и решение */
    DWORD m = GetLogicalDrives();
    for (int d = 0; d < 26; d++) if (m & (1u << d)) {
        char p[8] = { (char)('A' + d), ':', '\\', 'x', 0 };
        wchar_t root[4] = { (wchar_t)(L'A' + d), L':', L'\\', 0 }, full[1024];
        printf("диск %c: тип %u -> %d\n", 'A' + d, GetDriveTypeW(root), local_wpath(p, full, 1024));
    }
    return 0;
}
