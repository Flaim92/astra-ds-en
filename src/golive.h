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
void golive_vstatus(int voice_phase, const char *err);   /* состояние соединения трансляции (VS_*) -> GL_*/
#endif
#endif
