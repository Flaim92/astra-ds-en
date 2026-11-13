/* golive.h — трансляция экрана в голосовой канал (Discord Go Live), только «Astra DS Video» (-DASTRA_VIDEO). */
#ifndef GOLIVE_H
#define GOLIVE_H
#ifdef ASTRA_VIDEO
#include <stdbool.h>
#include <stddef.h>
#include "mc.h"
enum { GL_OFF, GL_CREATING, GL_WAIT_SERVER, GL_CONNECTING, GL_LIVE, GL_FAILED };
typedef struct {
    int phase;                                  /* GL_* */
    bool paused;
    bool waiting;                               /* соединение есть, ключи DAVE ждут первого зрителя */
    u64 guild_id, channel_id;
    char key[96];                               /* stream_key */
    char rtc_server_id[32];
    char endpoint[128], token[64];              /* STREAM_SERVER_UPDATE (токен стирается при остановке) */
    long long frames, keyframes, bytes, dropped;   /* кадры кодера: отправлено / отброшено (не в эфире) */
    char err[128];
} GoLiveState;
bool golive_start(u64 guild_id, u64 channel_id, char *err, size_t errn);   /* op 18; guild_id 0 — личный звонок */
void golive_stop(void);                         /* op 19 */
void golive_pause(bool paused);                 /* op 22 */
bool golive_paused(void);
void golive_state(GoLiveState *out);
void golive_on_event(const char *t, const cJSON *d);   /* STREAM_CREATE / STREAM_SERVER_UPDATE / STREAM_DELETE */
/* кадр кодера в формате Annex B (share_* дизайнера): ts90k — отметка 90 кГц */
void golive_frame(const u8 *annexb, size_t n, bool keyframe, u32 ts90k);
/* RFC 6184: NAL <= mtu — одиночный, иначе FU-A; marker — последний пакет кадра. Возврат — число пакетов, -1 — mtu */
int h264_packetize(const u8 *annexb, size_t n, size_t mtu, void (*emit)(void *u, const u8 *p, size_t len, bool marker), void *u);
int golive_selftest(void (*check)(bool ok, const char *what));
/* voice.c (voice_stream.c): голосовое соединение трансляции */
bool voice_stream_start(const char *endpoint, const char *token, const char *server_id, u64 channel_id, char *err, size_t errn);
void voice_stream_stop(void);
typedef struct { bool running, session_ready, have_vssrc, dave, dave_on, udp; int dave_ver, queued; long long packets; unsigned ops[40];
                 char host[128]; int port, curl_rc, ws_close; long long dave_ok, dave_fail, pli, nack, rtx; } VStreamInfo;
void voice_stream_info(VStreamInfo *o);         /* --livetest: состояние соединения трансляции (без секретов) */
void voice_stream_frame(const u8 *annexb, size_t n, bool keyframe, u32 ts90k);
void voice_stream_audio(const u8 *opus, size_t n, u32 ts48k);   /* звук трансляции: кадр Opus 20 мс, отметка 48 кГц */
int voice_stream_selftest(void (*check)(bool ok, const char *what));
/* op 12 трансляции (ssrc и качество эфира: размер, к/с, max_bitrate в бит/с; нули - 720p30): длина или -1 */
int voice_stream_op12_json(char *b, size_t n, unsigned audio_ssrc, unsigned video_ssrc, unsigned rtx_ssrc, int w, int h, int fps, int max_bps);
void golive_vstatus(int voice_phase, const char *err);   /* состояние соединения трансляции (VS_*) -> GL_*/
/* Просмотр чужой трансляции (зритель): op 20 STREAM_WATCH -> STREAM_CREATE/SERVER_UPDATE -> своё соединение. */
enum { GW_OFF, GW_WAIT, GW_CONNECTING, GW_LIVE, GW_FAILED };
/* Видео зрителя (этап 2): счётчики и память — без журнала, только для индикатора. */
typedef struct {
    int w, h;                                   /* размер кадра после обрезки (0 — ещё не было SPS) */
    bool e2ee;                                  /* кадры проходят через DAVE (сессия E2EE есть) */
    bool unsupported;                           /* поток не Constrained Baseline: «формат не поддерживается» */
    long long frames, shown, lost, dave_fail, dec_err, pli;   /* собрано / показано / выброшено / DAVE / декодер / запросы PLI */
    size_t bytes;                               /* память видео сейчас: декодер + буферы сборки + растр экрана, Б */
} GoVideoStats;
typedef struct { int phase; u64 user; char err[128]; long long apkts, vpkts; GoVideoStats v; } GoWatchState;
bool golive_watch(u64 guild_id, u64 channel_id, u64 user_id, char *err, size_t errn);
void golive_unwatch(void);
void golive_wstate(GoWatchState *out);
void golive_wstatus(int voice_phase, const char *err);
/* author — автор трансляции (из ключа): только его SSRC (op 12) и его ключ DAVE принимаются для видео */
bool voice_watch_start(const char *endpoint, const char *token, const char *server_id, u64 author, bool wide, char *err, size_t errn);
/* Набор кодеков зрителя в op 1. По умолчанию — только самые дешёвые (H264, VP8: официальный автор иначе сам выбирает
 * AV1 — дороже); широкий (все 5) — откат, если в дешёвом наборе просмотр не работает (golive_fallback). */
#define VC_CHEAP ((1u << VC_H264) | (1u << VC_VP8))
#define VC_ALL ((1u << VC_N) - 1)
unsigned watch_codec_set(bool wide);
int watch_codecs_json(char *out, size_t cap, bool wide);   /* ",{кодек}..." для массива codecs; длина или -1 */
/* Решение об откате по счётчикам (без сети, проверяется selftest). Время — только пока видео видно (gvid_wanted)
 * и после готовности сессии с ключами DAVE. 0 — всё хорошо/рано; 1 — переподключиться с широким набором;
 * 2 — и широкий набор не дал картинки: понятная ошибка в панели, звук идёт. */
typedef struct {
    bool wide;                                  /* уже широкий набор */
    bool outside;                               /* пришёл видеопоток с PT вне объявленного набора */
    long long vpkts, frames, shown;             /* видеопакеты, собранные кадры, показанные */
    long long wanted_ms;                        /* сколько видео было видно после готовности сессии */
    long long wanted_at_first;                  /* wanted_ms в момент первого собранного кадра; -1 — кадров ещё нет */
} WatchFbIn;
int golive_fallback(const WatchFbIn *s);
bool voice_watch_take_wide(void);               /* главный поток: поток зрителя просит широкий набор (один раз) */
void voice_watch_stop(void);
void voice_watch_counts(long long *apkts, long long *vpkts);
void voice_watch_vstats(GoVideoStats *o);       /* снимок счётчиков видео (из любого потока) */

/* ---- Кодеки видео зрителя и их RTP PT (нумерация официального клиента; RTX = PT + 1) ----
 * Порядок перечисления = предпочтение в op 1: дешевле декодировать — выше. */
enum { VC_H264, VC_VP8, VC_H265, VC_VP9, VC_AV1, VC_N };
int vc_pt(int codec);                           /* H264 105, VP8 107, H265 103, VP9 109, AV1 101 */
int vc_of_pt(int pt);                           /* -1 — не видео (RTX и прочее) */
int vc_of_name(const char *name);               /* "H264"/"VP8"/"H265"/"VP9"/"AV1" -> VC_*, -1 — неизвестный */
const char *vc_name(int codec);

/* ---- Сборка кадра H.264 из RTP (RFC 6184), без сети: тестируется отдельно (selftest) ----
 * Вход — полезная нагрузка RTP после транспортной расшифровки и снятия расширения заголовка.
 * Одиночный NAL (1..23), STAP-A (24), FU-A (28); граница кадра — marker, кадр = все пакеты с одной
 * отметкой времени и непрерывными номерами от конца прошлого кадра до пакета с marker. Пакеты внутри
 * кадра могут приходить не по порядку. Дыра в номерах, пакет следующего кадра до окончания текущего,
 * STAP-B/MTAP/FU-B, битый FU-A, переполнение — кадр выбрасывается (H264A_LOST: зрителю — PLI и ждать IDR).
 * Выход — Annex B со старт-кодами 00 00 00 01 (так кадр собирает WebRTC; по этим байтам проверяет DAVE). */
enum { H264A_NONE = 0, H264A_LOST = 1, H264A_FRAME = 2 };   /* битовая маска: пакет мог и закрыть потерю, и собрать кадр */
typedef struct H264Asm H264Asm;
H264Asm *h264_asm_new(size_t max_frame);        /* max_frame — предел собранного кадра, Б (0 — 2 МБ) */
/* Тот же сборщик для любого кодека (граница кадра — marker, дыры/порядок — как у H.264):
 *   H265 — RFC 7798: одиночный NAL, AP (48), FU (49) -> Annex B со старт-кодами 00 00 00 01;
 *   VP8  — RFC 7741: снимается описатель (X/I/L/T/K, PictureID 7/15 бит), кадр — с S=1 PID=0;
 *   VP9  — RTP VP9: описатель I/P/L/F/B/E/V (+SS), кадр — от B до E;
 *   AV1  — RTP AV1: заголовок Z/Y/W/N, OBU собираются из частей; у всех OBU, кроме последнего, — поле размера
 *          (как кадр отдаёт шифратору DAVE отправитель; dav1d последний OBU без размера принимает).
 * h264_asm_frame: idr — ключевой кадр, sps — в кадре параметры потока (SPS/VPS, заголовок AV1, ключевой VP8/VP9). */
H264Asm *vid_asm_new(int codec, size_t max_frame);
int vid_asm_codec(const H264Asm *a);
void h264_asm_free(H264Asm *a);                 /* затирает буферы */
void h264_asm_reset(H264Asm *a);                /* забыть текущий кадр и границу (после переподключения) */
int h264_asm_push(H264Asm *a, u16 seq, u32 ts, bool marker, const u8 *payload, size_t n);
/* последний собранный кадр (действителен до следующего push); idr — есть NAL 5, sps — есть NAL 7 */
const u8 *h264_asm_frame(const H264Asm *a, size_t *n, bool *idr, bool *sps);
size_t h264_asm_bytes(const H264Asm *a);        /* память сборщика, Б */
/* Проверка без сети: h264_packetize -> перемешанные пакеты -> h264_asm_push, потеря -> LOST. 0 — всё ок. */
int h264_depacketize_selftest(void (*check)(bool ok, const char *what));
int vid_depacketize_selftest(void (*check)(bool ok, const char *what));   /* H265, VP8, VP9, AV1 */
int golive_fallback_selftest(void (*check)(bool ok, const char *what));   /* набор кодеков в op 1 и решение об откате */

/* ---- Видео-декодер (src/viddec.c): FFmpeg libavcodec h264/hevc/vp8/vp9 + dav1d (AV1), однопоточный ---- */
typedef struct VidDec VidDec;
enum { VD_NONE, VD_PIC, VD_ERR, VD_UNSUPPORTED };
typedef struct { const u8 *p[3]; int s[3]; int w, h, bits; bool corrupt; } VidPic;   /* corrupt — декодер сообщил ошибку/скрытие в кадре */   /* 4:2:0, 8 или 10 бит (тогда u16 на отсчёт) */
VidDec *vdec_new(int codec);
void vdec_free(VidDec *d);                      /* кадры декодера затираются и освобождаются */
/* f — кадр (H26x — Annex B), за ним >= 64 Б запаса (нули); skip_nonref — не успеваем: неопорные пропустить.
 * Картинка в *o действительна до следующего вызова. */
int vdec_frame(VidDec *d, const u8 *f, size_t n, bool skip_nonref, VidPic *o);
size_t vdec_bytes(void);                        /* память кадров декодера сейчас, Б */
int vdec_frames(void);                          /* кадров декодера сейчас (в работе + пул) */
void vdec_done(VidDec *d);                      /* картинка больше не нужна — отпустить ссылку на кадр */
int vdec_selftest(void (*check)(bool ok, const char *what));

/* ---- Показ видео в окне (gui_grid.c + gui_win.c / gui_x11.c). ДОГОВОР с дизайнером (ui.c): ----
 * g_video_area — прямоугольник в клетках сетки; ui.c вызывает его в каждом render() (после term_size),
 * пока панель видео видна. Если в кадре вывода (до ESC[?2026l) не вызван — видео скрыто.
 * g_video_supported — true только в оконном режиме с растром (не --tui/--cli). */
void g_video_area(int x, int y, int w, int h);
bool g_video_supported(void);
/* поток зрителя: кадр I420 -> BGRA с масштабом (ближайший сосед, пропорции, поля — чёрные) в общий буфер
 * под мьютексом, перерисовка только области видео, не чаще 15 к/с. false — область скрыта (кадр не нужен). */
bool gvid_put_i420(const u8 *y, const u8 *u, const u8 *v, int ys, int uvs, int w, int h);
bool gvid_put_pic(const VidPic *p);             /* то же для VidPic (8 или 10 бит) */
bool gvid_wanted(void);                         /* область видна сейчас (иначе кадры можно не декодировать) */
void gvid_open(void);                           /* главный поток: начало просмотра */
void gvid_area_px(int *pw, int *ph);            /* размер области видео сейчас, пиксели (0 — скрыта) */
void gvid_close(void);                          /* главный поток: конец просмотра — растр затереть и освободить */
size_t gvid_bytes(void);
int gvid_selftest(void (*check)(bool ok, const char *what));   /* быстрая конверсия против точной */
#endif
#endif
