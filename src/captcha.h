/* captcha.c — проверка hCaptcha при входе: системный webview только на время проверки.
 *
 * Windows: WebView2 (Edge WebView2 Runtime, есть в Windows 11; библиотека среды проверяется по подписи
 * Authenticode перед загрузкой). Отдельный поток со своим окном и очередью сообщений; страница с виджетом
 * подаётся из памяти как https://discord.com/mincord-captcha (запрос перехватывается, в сеть не уходит),
 * так hCaptcha видит домен Discord. Ответ (h-captcha-response) приходит через chrome.webview.postMessage.
 * Сеть webview — только через свой локальный мост CONNECT (127.0.0.1, случайный порт, на время проверки),
 * пускающий лишь hcaptcha.com:443 и идущий тем же маршрутом, что весь трафик (VLESS или напрямую).
 * Профиль WebView2 — во временном каталоге, удаляется после закрытия. Linux: пока не поддерживается. */
#ifndef CAPTCHA_H
#define CAPTCHA_H

#include <stdbool.h>
#include <stddef.h>

bool captcha_supported(void);                   /* есть чем показать проверку (найдена среда WebView2) */
/* Открыть окно проверки. sitekey — из ответа Discord (captcha_sitekey), rqdata — captcha_rqdata или "".
 * Ответ запишется в out[cap] (закреплённая память вызывающего). test — тестовый режим (ключ hCaptcha
 * для проверок, решается сам). false — не открылось, err — причина по-русски. */
bool captcha_open(const char *sitekey, const char *rqdata, char *out, size_t cap, bool test, char *err, size_t errn);
/* 0 — окно открыто; 1 — решено (ответ в out); -1 — закрыто или ошибка (err). Готовность будит g_wake. */
int captcha_result(char *err, size_t errn);
void captcha_close(void);                       /* закрыть окно, дождаться потока, освободить и удалить профиль */
bool captcha_busy(void);                        /* окно открыто или поток ещё не завершён */
void captcha_bridge_stats(int *used, int *refused);
#ifndef _WIN32
int captcha_helper_main(int argc, char **argv);   /* Linux: процесс-помощник окна проверки (--captcha-helper) */
#endif   /* самопроверка: соединений через мост / отказов (не hcaptcha.com) */

#endif
