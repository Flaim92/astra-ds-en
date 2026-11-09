/* tools/sec/ntpath.c — стенд: какие формы пути доходят до файла через _wstat64/_wfopen (как open_file в
 * net.c). Проверяет, обходит ли NT-префикс \??\ запрет UNC (\\ в начале) в send_file. Только локальные
 * пути (сеть не трогается): если \??\C:\... открывается, то \??\UNC\хост\... тоже дойдёт до SMB. */
#include <stdio.h>
#include <sys/stat.h>
#include <wchar.h>
int main(void) {
    const wchar_t *P[] = { L"C:\\Windows\\win.ini", L"\\??\\C:\\Windows\\win.ini", L"\\\\?\\C:\\Windows\\win.ini",
                           L"\\\\.\\C:\\Windows\\win.ini", L"\\GLOBAL??\\C:\\Windows\\win.ini" };
    for (int i = 0; i < 5; i++) {
        struct _stat64 st;
        int r = _wstat64(P[i], &st);
        FILE *f = _wfopen(P[i], L"rb");
        printf("%ls: stat=%d fopen=%s\n", P[i], r, f ? "OK" : "no");
        if (f) fclose(f);
    }
    return 0;
}
