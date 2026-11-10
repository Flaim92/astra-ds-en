/* impcurl: libcurl-impersonate (Chrome 152 TLS fingerprint) для auth-эндпоинтов Discord.
 * Используется ТОЛЬКО из login.c; обычный libcurl проекта остаётся нетронутым.
 *
 * DLL запекается в exe как ресурс RCDATA id=10 (Windows, build.sh). При первом
 * обращении распаковывается во временный путь со случайным именем с
 * FILE_FLAG_DELETE_ON_CLOSE и исчезает при выходе процесса. Linux пока без
 * импера: impcurl_vt() вернёт NULL, вызывающий код падёт обратно на обычный curl. */
#ifndef IMPCURL_H
#define IMPCURL_H
#include <curl/curl.h>

/* Опция libcurl-impersonate: выбор профиля ClientHello.
 * Не объявлена в стоковых заголовках libcurl 8.23 — задаётся по номеру из curl_cffi. */
#ifndef CURLOPT_IMPERSONATE
#define CURLOPT_IMPERSONATE ((CURLoption)(CURLOPTTYPE_OBJECTPOINT + 999))
#endif

typedef struct {
    CURL *(*easy_init)(void);
    void  (*easy_cleanup)(CURL *);
    CURLcode (*easy_setopt)(CURL *, CURLoption, ...);
    CURLcode (*easy_getinfo)(CURL *, CURLINFO, ...);
    CURLM *(*multi_init)(void);
    CURLMcode (*multi_cleanup)(CURLM *);
    CURLMcode (*multi_add_handle)(CURLM *, CURL *);
    CURLMcode (*multi_remove_handle)(CURLM *, CURL *);
    CURLMcode (*multi_perform)(CURLM *, int *);
    CURLMsg *(*multi_info_read)(CURLM *, int *);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void  (*slist_free_all)(struct curl_slist *);
    const char *(*easy_strerror)(CURLcode);
    CURLcode (*global_init)(long);
    void  (*global_cleanup)(void);
} ImpCurl;

/* Указатель на вктабл или NULL, если impersonate недоступен.
 * Безопасно вызывать многократно: загрузка ленивая и один раз. */
const ImpCurl *impcurl_vt(void);

/* Явно выгрузить DLL и удалить распакованный файл (на выходе процесса). */
void impcurl_shutdown(void);

/* Диагностика (--imptest): грузит DLL, делает GET /experiments. 0 — порядок. */
int impcurl_selftest(void);

#endif
