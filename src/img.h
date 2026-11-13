/* img.c — аватарки и картинки Discord как цветной ASCII в сетке символов.
 *
 * Поток: CDN Discord (только https://cdn.discordapp.com и https://media.discordapp.net, тот же маршрут,
 * что и весь трафик, — напрямую или через VLESS) -> JPEG не меньше 128 px (аватар/значок size=128..512,
 * превью — меньшая сторона 256, не больше исходника) -> libjpeg-turbo с DCT-масштабированием до 2x4
 * пикселя на клетку, построчно, в RGB -> клетки {символ, цвет символа, цвет фона}: символ подбирается по
 * закраске шрифта, цвета — так, чтобы смесь повторяла картинку. Картинки не хранятся: скачанные байты и
 * пиксели затираются сразу после перекодировки, готовые клетки живут, только пока картинка на экране
 * (жёсткий предел 512 КБ на кадр); на диск — никогда. Не больше одной загрузки одновременно; загрузка
 * начинается, если картинка видна не меньше 150 мс (прокрутка не долбит CDN). */
#ifndef IMG_H
#define IMG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "plat.h"

/* ch — 0x20..0x7E; после круглой маски ещё и 0 — «прозрачная клетка: не рисовать, оставить фон колонки» */
typedef struct { char ch; uint8_t fg[3], bg[3]; } ImgCell;

#define IMG_MAX_W 256                           /* наибольшая картинка в клетках (просмотр 192x48) */
#define IMG_MAX_H 64

/* Включить модуль (main.c: выключен при --no-images и в --cli). Выключенный модуль ничего не
 * скачивает и не держит, все запросы возвращают false. */
void img_enable(bool on);

/* Начало кадра отрисовки (ui.c, ОБЯЗАТЕЛЬНО в каждом кадре, до запросов картинок): всё, что не
 * запрашивали в прошлом кадре, ушло с экрана — клетки затираются и освобождаются, загрузка
 * прерывается. Вернулось на экран — будет скачано и перекодировано заново. */
void img_frame(void);

/* Аватар w x h клеток (для квадрата w = 2*h; мелкая сетка — 12x6, 16x8). avatar_hash — поле avatar пользователя ("a_" + 32 hex
 * или 32 hex); NULL/"" — стандартный аватар Discord (рисуется на месте, без сети).
 * true — out[w*h] заполнен; false — ещё не готов (поставлен в очередь) или не получился: рисуйте
 * инициалы. Готовность сообщается через ui_dirty(). out каждый раз копируется заново (w*h*7 байт). */
bool img_avatar(uint64_t user_id, const char *avatar_hash, int w, int h, ImgCell *out);

/* Значок сервера (Guild.icon, тот же формат хэша): cdn.discordapp.com/icons/{id}/{hash}.jpg, тот же
 * порядок «только видимое». Мелкая сетка — 18x9, 24x12. NULL/"" — значка нет: false (рисуйте сокращение названия, как Discord). */
bool img_icon(uint64_t guild_id, const char *icon_hash, int w, int h, ImgCell *out);
/* Мини-значок w x h (плитка свёрнутой папки): уменьшается из уже готовых клеток этого значка любого размера
 * не меньше w x h, без новой загрузки. Готовых нет — значок заказывается в размере bw x bh (каким его рисует
 * полоса серверов: тогда раскрытая папка берёт его же), false. */
bool img_icon_mini(uint64_t guild_id, const char *icon_hash, int bw, int bh, int w, int h, ImgCell *out);
/* Уменьшить готовые клетки sw x sh до dw x dh (dw <= sw, dh <= sh): средний видимый цвет частей, подбор символа
 * тем же способом, что при разборе картинки. */
void img_shrink(const ImgCell *src, int sw, int sh, int dw, int dh, ImgCell *dst);

/* Круглая маска (как кружки Discord): эллипс, вписанный в w x h клеток с учётом того, что клетка
 * вдвое выше ширины (w = 2h — круг: 2x1 ... 48x24). Клетки вне круга -> ch = 0 (прозрачные, fg = bg =
 * colbg); граничные -> символ дуги . , ' ` " _ - ( ) / \ | [ ] < > ^ ~ : ; — один из двух цветов
 * клетки ровно colbg (фон колонки, RGB; NULL — чёрный), другой — картинка, подобраны по доле площади
 * (тонкая дуга без ступенек); внутренние — без изменений. */
void img_round(ImgCell *c, int w, int h, const uint8_t colbg[3]);
bool img_avatar_round(uint64_t user_id, const char *avatar_hash, int w, int h, const uint8_t colbg[3], ImgCell *out);
bool img_icon_round(uint64_t guild_id, const char *icon_hash, int w, int h, const uint8_t colbg[3], ImgCell *out);

/* Превью вложения. url — url или proxy_url вложения (https://cdn.discordapp.com/attachments/... или
 * https://media.discordapp.net/attachments/...; подписи ex/is/hm сохраняются, остальное
 * отбрасывается). src_w/src_h — width/height вложения из сообщения (0 — неизвестно). Картинка
 * вписывается в maxw x maxh клеток с сохранением пропорций (превью до 128x32, просмотр 192x48);
 * out — буфер на maxw*maxh клеток.
 * true — готово, *outw x *outh клеток (строки подряд, шаг *outw). */
bool img_attachment(const char *url, int src_w, int src_h, int maxw, int maxh, ImgCell *out, int *outw, int *outh);
/* То же без копии: указатель на готовые клетки в кэше (*outw x *outh, шаг *outw) или NULL (не готово —
 * поставлено в очередь). Действителен только до конца текущего кадра (до img_frame/img_service/img_forget):
 * рисовать сразу, не хранить. Экономит буфер maxw*maxh клеток у вызывающего. */
const ImgCell *img_attachment_peek(const char *url, int src_w, int src_h, int maxw, int maxh, int *outw, int *outh);

/* Главный цикл: ведёт загрузку (curl_multi), разбирает готовое. Опрос — только пока идёт загрузка. */
void img_service(void);
int img_waitables(Waitable *out, int max);      /* пока всегда 0: загрузка опрашивается, как REST */
int64_t img_next_wake(void);

void img_forget(void);                          /* паника/выход: прервать загрузку, затереть всё */
size_t img_cache_bytes(void);                   /* сколько занимают клетки видимых картинок (замеры) */

/* Разбор JPEG в клетки w x h (без сети и без кэша). false — не JPEG, битый, больше 512x512,
 * не 8 бит, не RGB/серый, слишком много сканов или памяти. Ничего не выводит. */
bool img_decode(const uint8_t *jpg, size_t n, int w, int h, ImgCell *out);
size_t img_decode_peak(void);                   /* пик памяти libjpeg в последнем разборе, байт */

int img_selftest(void (*check)(bool ok, const char *what));

#endif
