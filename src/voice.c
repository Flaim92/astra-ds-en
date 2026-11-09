/* Голос: голосовой gateway v8, UDP (IP discovery, RTP), транспорт aead_aes256_gcm_rtpsize,
 * сквозное шифрование DAVE (libdave), Opus, микшер. Всё живёт в одном потоке и только во
 * время звонка. Правило безопасности: без установленного DAVE свой звук не отправляется. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <curl/curl.h>
#include <opus/opus.h>
#include <dave/dave.h>
#include <openssl/evp.h>
#include "mc.h"
#include "plat.h"
#include "socks.c"                               /* SOCKS5 UDP для VLESS: static, отдельно не компилируется */

PlatWake *g_wake;

#define FRAME 960                                /* 20 мс при 48 кГц */
#define MAX_SRC 24
#define FIFO_FRAMES (48000 / 4)                  /* до 250 мс на говорящего */
#define PREBUF (48000 * 40 / 1000)               /* 40 мс перед началом воспроизведения */

static const unsigned char SILENCE[3] = { 0xF8, 0xFF, 0xFE };

/* ======================= общее состояние (под мьютексом) ======================= */
typedef struct { u64 user; float vol; } Vol;
static struct {
    PlatMutex *mu;
    VoiceStatus st;
    char session[96];
    char token[128];
    char endpoint[160];
    bool have_session, have_server;
    PlatThread *thr;
    PlatWake *cmd;
    atomic_bool stop;
    atomic_bool mute, deaf, ptt;
    atomic_int vad_db;
    atomic_bool need_leave;                     /* поток упал: главный поток скажет Discord, что мы вышли */
    atomic_bool need_rejoin;                    /* RESUME не принят: войти заново через gateway (op 4) */
    atomic_bool vl_fail;                        /* голос не прошёл через туннель: сообщить vless.c из главного потока */
    bool via_vless;                             /* звонок поднят через туннель */
    int rejoins;                                /* повторных входов подряд без успеха */
    char gw_sess[96];                           /* сессия gateway, в которой отправлен op 4 */
    Vol vols[64]; int nvols;
    char dev[2][96];                            /* №140: выбранные устройства (0 — микрофон, 1 — вывод); "" — как в O */
    atomic_int dev_gen;                         /* смена устройства: поток звонка переоткрывает звук */
    atomic_int call_level;                      /* уровень микрофона в звонке (0..100) — для теста микрофона */
    atomic_int ptt_vk;                          /* №141: клавиша рации (0 — по-старому, F8 переключает) */
    atomic_bool hpf_off;                        /* №143: фильтр 90 Гц выключен */
    atomic_bool vad_auto;                       /* №142: авточувствительность */
    atomic_int vad_auto_db;
} V;
static void mt_stop_thread(void);
/* selftest: по умолчанию звук не открывается вовсе (индикатор микрофона не загорается);
 * настоящие устройства — только при ST_REALAUDIO=1 (g_audio_fake ставит selftest). */
bool g_audio_fake;
static Audio *vopen(const char *mic, const char *out, char *err, size_t errn) {
    if (g_audio_fake) { snprintf(err, errn, "фиктивное устройство (selftest без ST_REALAUDIO=1)"); return NULL; }
    return audio_open(mic, out, err, errn);
}
/* имена для audio_open: выбранные в настройках, иначе из командной строки (--mic/--out) */
static void dev_hints(char *mic, char *out) {
    mutex_lock(V.mu);
    snprintf(mic, 96, "%s", V.dev[0][0] ? V.dev[0] : O.mic ? O.mic : "");
    snprintf(out, 96, "%s", V.dev[1][0] ? V.dev[1] : O.out ? O.out : "");
    mutex_unlock(V.mu);
}

/* Нулевое логирование: без своего приёмника libdave пишет журнал MLS в std::cout. */
static void dave_log_noop(DAVELoggingSeverity sev, const char *file, int line, const char *msg) {
    (void)sev; (void)file; (void)line; (void)msg;
}
void voice_init(void) {
    V.mu = mutex_new();
    V.cmd = wake_new();
    atomic_store(&V.vad_db, -45);
    daveSetLogSinkCallback(dave_log_noop);
}

static void status_set(VoicePhase ph, const char *err) {
    mutex_lock(V.mu);
    V.st.phase = ph;
    if (err) snprintf(V.st.error, sizeof V.st.error, "%s", err);
    else if (ph != VS_FAILED) V.st.error[0] = 0;
    mutex_unlock(V.mu);
    wake_signal(g_wake);
}
static void maybe_start(void);
static void voice_rejoin(void);
/* Служебные дела главного потока по сигналам голосового (голосовой поток не трогает net.c и vless.c). */
static void voice_main_chores(void) {
    if (atomic_exchange(&V.vl_fail, false)) vless_report_failure("голос");
    /* голосовой поток упал: сообщаем Discord о выходе, иначе для других мы «висим» в канале */
    if (atomic_exchange(&V.need_leave, false) && !O.demo) gw_voice_state(V.st.guild_id, 0, false, false);
    if (O.demo || V.st.phase == VS_IDLE) return;
    if (atomic_exchange(&V.need_rejoin, false)) { voice_rejoin(); return; }
    /* gateway переподключился с НОВОЙ сессией (обрыв сети, не удался его RESUME): прежний op 4 мог уйти
     * в мёртвое соединение, а голосовая сессия привязана к старой — входим в канал заново */
    if (V.st.phase != VS_FAILED && S.ready && S.session_id && V.gw_sess[0] && strcmp(S.session_id, V.gw_sess)) {
        V.rejoins = 0;
        ui_notice("связь с Discord восстановлена: голос входит в канал заново");
        voice_rejoin();
        return;
    }
    VlessState vl = vless_state();
    /* ждали подъёма туннеля — пробуем снова */
    if (V.st.phase == VS_WAITING && !V.thr && vl != VL_STARTING) { maybe_start(); return; }
    /* туннель включили или пользователь явно выключил его посреди звонка: голос перезаходит по-новому,
     * чтобы не остаться напрямую при включённом VLESS */
    if (V.thr && V.st.phase != VS_FAILED && ((vl == VL_UP && !V.via_vless) || (vl == VL_OFF && V.via_vless))) {
        ui_notice(vl == VL_UP ? "голос переподключается через VLESS" : "голос переподключается напрямую");
        V.rejoins = 0;
        voice_rejoin();
    }
}
void voice_status(VoiceStatus *out) {
    if (!V.mu) { memset(out, 0, sizeof *out); return; }
    static bool busy;                           /* voice_rejoin -> voice_join может снова спросить статус */
    if (!busy) { busy = true; voice_main_chores(); busy = false; }
    if (V.st.phase == VS_CONNECTED) V.rejoins = 0;
    mutex_lock(V.mu);
    *out = V.st;
    out->self_mute = atomic_load(&V.mute);
    out->self_deaf = atomic_load(&V.deaf);
    mutex_unlock(V.mu);
}
float voice_get_volume(u64 user) {
    float v = 1.0f;
    if (!V.mu) return v;
    mutex_lock(V.mu);
    for (int i = 0; i < V.nvols; i++) if (V.vols[i].user == user) v = V.vols[i].vol;
    mutex_unlock(V.mu);
    return v;
}
void voice_set_volume(u64 user, float vol) {
    if (vol < 0) vol = 0;
    if (vol > 2.0f) vol = 2.0f;
    mutex_lock(V.mu);
    int i;
    for (i = 0; i < V.nvols && V.vols[i].user != user; i++) {}
    if (i == V.nvols && V.nvols < 64) V.nvols++;
    if (i < 64) V.vols[i] = (Vol){ user, vol };
    mutex_unlock(V.mu);
    ui_notice("громкость %s: %d%%", user_name(user), (int)(vol * 100 + 0.5f));
}
void voice_vad_adjust(int d) {
    int v = atomic_load(&V.vad_db) + d;
    if (v < -70) v = -70;
    if (v > -10) v = -10;
    atomic_store(&V.vad_db, v);
}
int voice_vad_db(void) { return atomic_load(&V.vad_auto) ? atomic_load(&V.vad_auto_db) : atomic_load(&V.vad_db); }
void voice_vad_auto(bool on) { if (on && !atomic_load(&V.vad_auto)) atomic_store(&V.vad_auto_db, atomic_load(&V.vad_db)); atomic_store(&V.vad_auto, on); }
bool voice_vad_is_auto(void) { return atomic_load(&V.vad_auto); }
void voice_ptt_bind(int vk) { atomic_store(&V.ptt_vk, vk > 0 && vk < 256 ? vk : 0); }
int voice_ptt_bound(void) { return atomic_load(&V.ptt_vk); }
void voice_ptt(bool down) { atomic_store(&V.ptt, down); }

/* ======================= голосовой поток ======================= */
typedef struct {
    u32 ssrc;
    u64 user;
    OpusDecoder *dec;
    DAVEDecryptorHandle dd;
    int16_t *fifo;                               /* стерео, кольцо FIFO_FRAMES кадров */
    int rd, n;
    bool playing;
    u16 last_seq;
    bool have_seq;
    i64 last_pkt, last_voice;
} Src;

typedef struct {
    /* параметры */
    u64 server_id, channel_id, me;
    char session[96], token[128], host[128];
    bool demo;
    /* соединения */
    CURL *ws;
    SockWait *ws_wait;
    intptr_t udp;
    SockWait *udp_wait;
    unsigned char addr[16];
    Audio *au;
    /* протокол */
    i64 hb_interval, next_hb, hb_sent;
    bool hb_acked;
    int last_seq;
    u32 ssrc;
    bool session_ready;
    unsigned char key[32];
    EVP_CIPHER_CTX *enc, *dec;
    u32 nonce;
    u16 rtp_seq;
    u32 rtp_ts;
    Buf in;
    /* DAVE */
    DAVESessionHandle dave;
    DAVEEncryptorHandle denc;
    u16 dave_ver, dave_pending;
    bool dave_ready, dave_on;
    double vf_cur, vf_floor; int vf_n; bool vf_init;   /* №142: авточувствительность — состояние звонка (ревью 14 М2) */
    double hp_x1, hp_x2, hp_y1, hp_y2;          /* №143: состояние фильтра 90 Гц */
    u64 users[64]; int nusers;
    /* звук */
    OpusEncoder *encd;
    Src src[MAX_SRC]; int nsrc;
    struct { u32 ssrc; u64 user; } smap[64]; int nsmap;   /* SSRC -> участник (op 5), живёт дольше слота */
    i64 next_gc;
    u32 pub_hash;
    bool talking;
    int trailing;
    i64 talk_until;
    bool marker;
    i64 next_frame;
    i64 next_status, next_ka;
    float level;
    /* темп звука по реальному времени: защита от устройств, которые отдают/берут данные без меры */
    i64 t_audio;
    i64 cap_frames, out_frames;
    bool throttled;
    /* демо */
    double phase;
    /* туннель VLESS: копии, сделанные главным потоком при старте (vless.c не зовём из этого потока) */
    bool via_socks;
    char proxy[300];                             /* CURLOPT_PROXY: "" — без прокси (и без переменных окружения) */
    char sx_ip[64], sx_user[160], sx_pass[160];
    int sx_port;
    Socks sx;
    SockWait *sx_wait;
    /* переподключение WebSocket (op 7 RESUME) */
    bool ws_dead;                                /* соединение потеряно, разбор — в цикле */
    int ws_close;                                /* код закрытия; 0 — сетевой обрыв */
    bool resuming;                               /* ждём Hello, чтобы послать RESUME */
    int rc_try;
    i64 rc_at, rc_start, rc_conn;
    /* буферы микшера и приёма WebSocket (121 КБ): живут только в звонке (раньше — статические, всегда) */
    int32_t *mix_acc;                            /* 4800 стерео-кадров */
    int16_t *mix_out;
    char *ws_buf;                                /* VW_WSBUF байт */
} VW;
#define VW_WSBUF 65536

static void vw_error(VW *w, const char *msg) {
    (void)w;
    status_set(VS_FAILED, msg);
    atomic_store(&V.need_leave, true);
    atomic_store(&V.stop, true);
}
/* Сбой, который при VLESS означает «через туннель не идёт»: сообщить vless.c (из главного потока). */
static void vw_net_error(VW *w, const char *direct, const char *tunnel) {
    if (w->via_socks) atomic_store(&V.vl_fail, true);
    vw_error(w, w->via_socks ? tunnel : direct);
}

/* ---------- UDP: напрямую или через SOCKS5 туннеля ---------- */
static void vudp_send(VW *w, const void *p, size_t n) {
    if (w->udp < 0) return;
    if (!w->via_socks) { udp_send(w->udp, p, n, w->addr); return; }
    /* при VLESS — только на локальное реле; без живого UDP ASSOCIATE не отправляется ничего */
    unsigned char b[1600];
    int k = socks_wrap(&w->sx, p, n, b, sizeof b);
    if (k > 0) udp_send(w->udp, b, (size_t)k, w->sx.relay);
}
/* Принять датаграмму; данные — в начале buf. -1 — больше нет. 0 — отброшена, читать дальше. */
static int vudp_recv(VW *w, unsigned char *buf, int cap) {
    int k = udp_recv(w->udp, buf, (size_t)cap);
    if (k < 0 || !w->via_socks) return k;
    int off = socks_unwrap(&w->sx, buf, k);
    if (off < 0) return 0;
    memmove(buf, buf + off, (size_t)(k - off));
    return k - off;
}

/* ---------- WebSocket ---------- */
static bool ws_send(VW *w, unsigned flags, const void *data, size_t n) {
    if (!w->ws || w->ws_dead) return false;      /* переподключаемся: служебное сообщение не нужно */
    size_t off = 0;
    int spins = 0;
    while (off < n) {
        size_t sent = 0;
        CURLcode rc = curl_ws_send(w->ws, (const char *)data + off, n - off, &sent, 0, off ? CURLWS_CONT | (flags & ~(unsigned)(CURLWS_TEXT | CURLWS_BINARY)) : flags);
        if (rc == CURLE_AGAIN) { if (++spins > 100) break; plat_sleep_ms(2); continue; }
        if (rc != CURLE_OK) break;
        off += sent;
    }
    if (off < n) { w->ws_dead = true; w->ws_close = 0; return false; }   /* разберётся цикл: переподключение */
    return true;
}
static void ws_text(VW *w, const char *s) { ws_send(w, CURLWS_TEXT, s, strlen(s)); }
static void ws_bin(VW *w, u8 op, const void *p, size_t n) {
    unsigned char *b = xmalloc(n + 1);
    b[0] = op;
    memcpy(b + 1, p, n);
    ws_send(w, CURLWS_BINARY, b, n + 1);
    free(b);
}

/* ---------- DAVE ---------- */
static void dave_fail(const char *src, const char *reason, void *ud) { (void)src; (void)reason; (void)ud; }
static void me_str(VW *w, char *b, size_t n) { snprintf(b, n, "%llu", (unsigned long long)w->me); }
static void dave_add_user(VW *w, u64 u) {
    for (int i = 0; i < w->nusers; i++) if (w->users[i] == u) return;
    if (w->nusers < 64) w->users[w->nusers++] = u;
}
static void dave_init(VW *w, u16 ver) {
    if (!w->dave) w->dave = daveSessionCreate(NULL, "", dave_fail, NULL);
    if (!w->denc) w->denc = daveEncryptorCreate();
    if (!w->dave || !w->denc) return;
    char me[24];
    me_str(w, me, sizeof me);
    daveSessionReset(w->dave);
    daveSessionInit(w->dave, ver, w->channel_id, me);
    w->dave_ver = w->dave_pending = ver;
    w->dave_ready = false;
    dave_add_user(w, w->me);
}
static void dave_send_kp(VW *w) {
    uint8_t *kp = NULL;
    size_t n = 0;
    daveSessionGetMarshalledKeyPackage(w->dave, &kp, &n);
    if (kp && n) ws_bin(w, 26, kp, n);
    if (kp) daveFree(kp);
}
static int dave_ids(VW *w, char ids[][24], const char **ptrs) {
    for (int i = 0; i < w->nusers; i++) {
        snprintf(ids[i], 24, "%llu", (unsigned long long)w->users[i]);
        ptrs[i] = ids[i];
    }
    return w->nusers;
}
static void dave_set_ratchet(VW *w, Src *s) {
    if (!w->dave || !s->user) return;
    if (!s->dd) s->dd = daveDecryptorCreate();
    if (!s->dd) return;
    char u[24];
    snprintf(u, sizeof u, "%llu", (unsigned long long)s->user);
    DAVEKeyRatchetHandle r = daveSessionGetKeyRatchet(w->dave, u);
    if (r) { daveDecryptorTransitionToKeyRatchet(s->dd, r); daveKeyRatchetDestroy(r); }
}
static void dave_apply_receive(VW *w) { for (int i = 0; i < w->nsrc; i++) dave_set_ratchet(w, &w->src[i]); }
static void dave_execute(VW *w) {
    w->dave_ver = w->dave_pending;
    if (w->dave_ver == 0) { w->dave_on = false; w->dave_ready = false; return; }
    if (!w->dave_ready) return;
    w->dave_ready = false;
    char me[24];
    me_str(w, me, sizeof me);
    DAVEKeyRatchetHandle r = daveSessionGetKeyRatchet(w->dave, me);
    if (r) {
        daveEncryptorSetKeyRatchet(w->denc, r);
        daveEncryptorAssignSsrcToCodec(w->denc, w->ssrc, DAVE_CODEC_OPUS);
        daveKeyRatchetDestroy(r);
    }
    w->dave_on = daveEncryptorHasKeyRatchet(w->denc);
    /* вернулись на DAVE: переходный режим «без шифрования» у дешифраторов выключается */
    for (int i = 0; i < w->nsrc; i++) if (w->src[i].dd) daveDecryptorTransitionToPassthroughMode(w->src[i].dd, false);
}
static void send_tid(VW *w, int op, int tid) {
    char b[80];
    snprintf(b, sizeof b, "{\"op\":%d,\"d\":{\"transition_id\":%d}}", op, tid);
    ws_text(w, b);
}

/* ---------- источники звука ---------- */
static void src_free(Src *s);
static Src *src_get(VW *w, u32 ssrc, u64 user) {
    for (int i = 0; i < w->nsrc; i++) if (w->src[i].ssrc == ssrc) {
        if (user && w->src[i].user != user) { w->src[i].user = user; dave_set_ratchet(w, &w->src[i]); }
        return &w->src[i];
    }
    if (w->nsrc == MAX_SRC) {
        /* все слоты заняты: вытесняем того, кто молчит дольше всех (сначала безымянные) */
        int v = 0;
        for (int i = 1; i < w->nsrc; i++) {
            bool a = w->src[i].user == 0, b = w->src[v].user == 0;
            if (a != b ? a : w->src[i].last_pkt < w->src[v].last_pkt) v = i;
        }
        src_free(&w->src[v]);
        w->src[v] = w->src[--w->nsrc];
    }
    int err;
    OpusDecoder *d = opus_decoder_create(48000, 2, &err);
    if (err != OPUS_OK) return NULL;
    Src *s = &w->src[w->nsrc++];
    memset(s, 0, sizeof *s);
    s->ssrc = ssrc;
    s->user = user;
    if (!user) for (int i = 0; i < w->nsmap; i++) if (w->smap[i].ssrc == ssrc) s->user = w->smap[i].user;
    s->dec = d;
    s->fifo = xmalloc(FIFO_FRAMES * 2 * sizeof(int16_t));
    s->last_pkt = now_ms();
    dave_set_ratchet(w, s);
    return s;
}
static void src_free(Src *s) {
    if (s->dec) opus_decoder_destroy(s->dec);
    if (s->dd) daveDecryptorDestroy(s->dd);
    free(s->fifo);
    memset(s, 0, sizeof *s);
}
static void src_remove_user(VW *w, u64 user) {
    for (int i = w->nsmap - 1; i >= 0; i--) if (w->smap[i].user == user) w->smap[i] = w->smap[--w->nsmap];
    for (int i = w->nsrc - 1; i >= 0; i--) if (w->src[i].user == user) { src_free(&w->src[i]); w->src[i] = w->src[--w->nsrc]; }
    for (int i = 0; i < w->nusers; i++) if (w->users[i] == user) { w->users[i] = w->users[--w->nusers]; break; }
}
static void fifo_push(Src *s, const int16_t *pcm, int frames) {
    for (int i = 0; i < frames; i++) {
        if (s->n == FIFO_FRAMES) { s->rd = (s->rd + 1) % FIFO_FRAMES; s->n--; }   /* выкидываем старое: не копим задержку */
        int wi = (s->rd + s->n) % FIFO_FRAMES;
        s->fifo[wi * 2] = pcm[i * 2];
        s->fifo[wi * 2 + 1] = pcm[i * 2 + 1];
        s->n++;
    }
}

/* ---------- RTP ---------- */
static bool gcm(EVP_CIPHER_CTX *c, bool enc, const unsigned char *key, const unsigned char iv[12],
                const unsigned char *aad, int aadn, const unsigned char *in, int n, unsigned char *out, unsigned char tag[16]) {
    int l = 0;
    if (enc) {
        if (!EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, iv)) return false;
        if (!EVP_EncryptUpdate(c, NULL, &l, aad, aadn)) return false;
        if (n && !EVP_EncryptUpdate(c, out, &l, in, n)) return false;
        if (!EVP_EncryptFinal_ex(c, out + l, &l)) return false;
        return EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    }
    if (!EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, iv)) return false;
    if (!EVP_DecryptUpdate(c, NULL, &l, aad, aadn)) return false;
    if (n && !EVP_DecryptUpdate(c, out, &l, in, n)) return false;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) != 1) return false;
    return EVP_DecryptFinal_ex(c, out + l, &l) > 0;
}
static void rtp_send(VW *w, const unsigned char *payload, int n) {
    if (w->udp < 0 || !w->session_ready || n <= 0 || n > 1300) return;
    unsigned char pkt[12 + 1400 + 20];
    pkt[0] = 0x80;
    pkt[1] = w->marker ? 0xF8 : 0x78;
    w->marker = false;
    pkt[2] = (unsigned char)(w->rtp_seq >> 8); pkt[3] = (unsigned char)w->rtp_seq;
    pkt[4] = (unsigned char)(w->rtp_ts >> 24); pkt[5] = (unsigned char)(w->rtp_ts >> 16);
    pkt[6] = (unsigned char)(w->rtp_ts >> 8); pkt[7] = (unsigned char)w->rtp_ts;
    pkt[8] = (unsigned char)(w->ssrc >> 24); pkt[9] = (unsigned char)(w->ssrc >> 16);
    pkt[10] = (unsigned char)(w->ssrc >> 8); pkt[11] = (unsigned char)w->ssrc;
    unsigned char iv[12] = {0};
    w->nonce++;
    iv[0] = (unsigned char)(w->nonce >> 24); iv[1] = (unsigned char)(w->nonce >> 16);
    iv[2] = (unsigned char)(w->nonce >> 8); iv[3] = (unsigned char)w->nonce;
    unsigned char tag[16];
    if (!gcm(w->enc, true, w->key, iv, pkt, 12, payload, n, pkt + 12, tag)) return;
    memcpy(pkt + 12 + n, tag, 16);
    memcpy(pkt + 12 + n + 16, iv, 4);
    vudp_send(w, pkt, (size_t)(12 + n + 20));
    w->rtp_seq++;
}

static void send_opus(VW *w, const unsigned char *frame, int n) {
    /* Свой звук уходит только под сквозным шифрованием. Тишина — служебный кадр, не голос. */
    bool silence = n == 3 && !memcmp(frame, SILENCE, 3);
    if (silence) { rtp_send(w, frame, n); return; }
    if (!w->dave_on) return;
    unsigned char out[1500];
    size_t cap = daveEncryptorGetMaxCiphertextByteSize(w->denc, DAVE_MEDIA_TYPE_AUDIO, (size_t)n);
    if (cap > sizeof out) return;
    size_t wr = 0;
    if (daveEncryptorEncrypt(w->denc, DAVE_MEDIA_TYPE_AUDIO, w->ssrc, frame, (size_t)n, out, sizeof out, &wr) != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) return;
    rtp_send(w, out, (int)wr);
}

static void on_rtp(VW *w, const unsigned char *b, int len) {
    if (len < 12 + 20 || !w->session_ready) return;
    if ((b[0] >> 6) != 2 || (b[1] & 0x7F) != 120) return;   /* только Opus; RTCP мимо */
    u32 ssrc = (u32)b[8] << 24 | (u32)b[9] << 16 | (u32)b[10] << 8 | b[11];
    if (ssrc == w->ssrc) return;
    int aad = 12 + (b[0] & 0x0F) * 4, ext_words = 0;
    if (b[0] & 0x10) {
        if (aad + 4 > len) return;
        ext_words = b[aad + 2] << 8 | b[aad + 3];
        aad += 4;
    }
    int clen = len - aad - 20;
    if (clen <= 0 || clen > 1500) return;
    unsigned char iv[12] = {0}, tag[16], plain[1500];
    memcpy(iv, b + len - 4, 4);
    memcpy(tag, b + len - 20, 16);
    if (!gcm(w->dec, false, w->key, iv, b, aad, b + aad, clen, plain, tag)) return;
    int end = clen;
    if (b[0] & 0x20) { if (!end || !plain[end - 1] || plain[end - 1] > end) return; end -= plain[end - 1]; }
    if (ext_words * 4 > end) return;
    const unsigned char *frame = plain + ext_words * 4;
    int fn = end - ext_words * 4;
    if (fn <= 0) return;
    Src *s = src_get(w, ssrc, 0);
    if (!s) return;
    u16 seq = (u16)(b[2] << 8 | b[3]);
    i64 now = now_ms();
    s->last_pkt = now;
    /* Номер пакета учитываем всегда (даже без звука и при ошибке DAVE), иначе после долгого
     * разрыва все следующие пакеты казались бы «старыми» и собеседник замолкал бы надолго. */
    int gap = 0;
    if (s->have_seq) {
        int d = (u16)(seq - s->last_seq);
        if (d == 0 || d > 32768) return;         /* дубль или запоздавший */
        gap = d - 1;
    }
    s->have_seq = true;
    s->last_seq = seq;
    if (gap > 1000) { opus_decoder_ctl(s->dec, OPUS_RESET_STATE); gap = 0; }   /* пересинхронизация */
    if (atomic_load(&V.deaf)) return;            /* без звука: не расшифровываем и не декодируем */
    int16_t pcm[5760 * 2];
    if (fn == 3 && !memcmp(frame, SILENCE, 3)) return;
    unsigned char dout[1500];
    if (w->dave) {
        /* При сессии DAVE через дешифратор идёт КАЖДЫЙ кадр: открытый звук принимается, только если
         * libdave сама разрешила переходный режим. Иначе сервер мог бы подсунуть голос от чужого имени. */
        if (!s->dd) { dave_set_ratchet(w, s); if (!s->dd) return; }
        size_t wr = 0;
        if (daveDecryptorDecrypt(s->dd, DAVE_MEDIA_TYPE_AUDIO, frame, (size_t)fn, dout, sizeof dout, &wr) != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) return;
        frame = dout;
        fn = (int)wr;
    }
    /* потерянные кадры: восстанавливаем силами Opus (PLC), не больше 5 */
    for (int g = 0; g < gap && g < 5; g++) {
        int k = opus_decode(s->dec, NULL, 0, pcm, FRAME, 0);
        if (k > 0) fifo_push(s, pcm, k);
    }
    int k = opus_decode(s->dec, frame, fn, pcm, 5760, 0);
    if (k > 0) { fifo_push(s, pcm, k); s->last_voice = now; }
}

/* ---------- микшер ---------- */
static void mix_out(VW *w) {
    if (!w->au) return;
    int space = audio_render_space(w->au);
    if (space <= 0) return;
    if (space > 4800) space = 4800;
    /* не опережать реальное время больше чем на 60 мс */
    i64 budget = (now_ms() - w->t_audio) * 48 + 2880 - w->out_frames;
    if (budget <= 0) { w->throttled = true; return; }
    if (space > budget) { space = (int)budget; w->throttled = true; }
    w->out_frames += space;
    if (!w->mix_acc) w->mix_acc = xmalloc(4800 * 2 * sizeof(int32_t));
    if (!w->mix_out) w->mix_out = xmalloc(4800 * 2 * sizeof(int16_t));
    int32_t *acc = w->mix_acc;
    int16_t *out = w->mix_out;
    memset(acc, 0, (size_t)space * 2 * sizeof(int32_t));
    float vols[MAX_SRC];
    for (int i = 0; i < w->nsrc; i++) vols[i] = voice_get_volume(w->src[i].user);
    for (int i = 0; i < w->nsrc; i++) {
        Src *s = &w->src[i];
        if (!s->playing) { if (s->n >= PREBUF) s->playing = true; else continue; }
        int take = s->n < space ? s->n : space;
        int vq = (int)(vols[i] * 256);
        for (int f = 0; f < take; f++) {
            int ri = (s->rd + f) % FIFO_FRAMES;
            acc[f * 2] += s->fifo[ri * 2] * vq / 256;
            acc[f * 2 + 1] += s->fifo[ri * 2 + 1] * vq / 256;
        }
        s->rd = (s->rd + take) % FIFO_FRAMES;
        s->n -= take;
        if (s->n == 0) s->playing = false;       /* опустел — снова ждём запас */
    }
    if (w->demo && !atomic_load(&V.deaf)) {
        /* тестовый тон 440 Гц, тихо, чтобы проверить вывод без сети */
        for (int f = 0; f < space; f++) {
            int v = (int)(sin(w->phase) * 2500);
            w->phase += 2 * 3.14159265358979 * 440 / 48000;
            if (w->phase > 6.283185307) w->phase -= 6.283185307;
            acc[f * 2] += v;
            acc[f * 2 + 1] += v;
        }
    }
    for (int i = 0; i < space * 2; i++) out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    audio_render_write(w->au, out, space);
}

/* ---------- кадр микрофона ---------- */
/* №143: ФВЧ 2-го порядка (Баттерворт) 90 Гц при 48 кГц, коэффициенты — RBJ */
static void hpf_run(VW *w, int16_t *s, int n) {
    static double b0, b1, b2, a1, a2;
    if (b0 == 0) {
        double w0 = 2 * 3.14159265358979 * 90 / 48000, cw = cos(w0), al = sin(w0) / (2 * 0.70710678);
        double a0 = 1 + al;
        b0 = (1 + cw) / 2 / a0; b1 = -(1 + cw) / a0; b2 = (1 + cw) / 2 / a0; a1 = -2 * cw / a0; a2 = (1 - al) / a0;
    }
    for (int i = 0; i < n; i++) {
        double x = s[i], y = b0 * x + b1 * w->hp_x1 + b2 * w->hp_x2 - a1 * w->hp_y1 - a2 * w->hp_y2;
        w->hp_x2 = w->hp_x1; w->hp_x1 = x; w->hp_y2 = w->hp_y1; w->hp_y1 = y;
        s[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
}
void voice_hpf(bool on) { atomic_store(&V.hpf_off, !on); }
bool voice_hpf_on(void) { return !atomic_load(&V.hpf_off); }
static void capture_tick(VW *w) {
    if (!w->au) return;
    int16_t mono[FRAME];
    bool muted = atomic_load(&V.mute) || atomic_load(&V.deaf);
    for (;;) {
        /* не больше кадров, чем прошло времени (+2 кадра запаса) */
        if (w->cap_frames >= (now_ms() - w->t_audio) / 20 + 2) { w->throttled = true; break; }
        if (!audio_read_frame(w->au, mono)) break;
        w->cap_frames++;
        if (muted) { w->level = 0; continue; }
        if (atomic_load(&V.hpf_off) == false) hpf_run(w, mono, FRAME);   /* №143: срез ниже 90 Гц */
        voicefx_process(mono, FRAME);            /* смена голоса — до VAD и Opus (и в демо-петле) */
        double sum = 0;
        for (int i = 0; i < FRAME; i++) sum += (double)mono[i] * mono[i];
        double rms = sqrt(sum / FRAME) / 32768.0;
        w->level = (float)rms;
        double db = rms > 0 ? 20 * log10(rms) : -100;
        atomic_store(&V.call_level, db <= -60 ? 0 : db >= 0 ? 100 : (int)((db + 60) * 100 / 60));
        i64 now = now_ms();
        int pk = atomic_load(&V.ptt_vk);
        if (atomic_load(&V.vad_auto)) {         /* №142: шумовой фон — минимум за 1,5 с, сглаженный (свой у каждого звонка) */
            if (!w->vf_n || db < w->vf_cur) w->vf_cur = db;
            if (!w->vf_init) { w->vf_floor = -60; w->vf_init = true; }
            if (++w->vf_n >= 75) { w->vf_floor = w->vf_floor * 0.7 + w->vf_cur * 0.3; w->vf_n = 0; }
            int th = (int)(w->vf_floor + 9);
            atomic_store(&V.vad_auto_db, th < -60 ? -60 : th > -20 ? -20 : th);
        }
        int vth = atomic_load(&V.vad_auto) ? atomic_load(&V.vad_auto_db) : atomic_load(&V.vad_db);
        bool voice = pk ? plat_key_down(pk) : O.ptt ? atomic_load(&V.ptt) : db > vth;
        if (voice) w->talk_until = now + (O.ptt ? 0 : 300);     /* хвост 300 мс, чтобы не рвать слова */
        bool talking = voice || now < w->talk_until;
        if (talking && !w->talking) { w->marker = true; }
        if (!talking && w->talking) w->trailing = 5;
        w->talking = talking;
        w->rtp_ts += FRAME;
        if (w->demo) {
            /* петля: свой голос — в свой же динамик через кодек, как его услышат другие */
            if (talking && w->encd) {
                unsigned char pkt[1275];
                int16_t st[FRAME * 2];
                for (int i = 0; i < FRAME; i++) st[i * 2] = st[i * 2 + 1] = mono[i];
                int n = opus_encode(w->encd, st, FRAME, pkt, sizeof pkt);
                Src *s = src_get(w, 1, w->me);
                if (n > 0 && s) {
                    int16_t pcm[FRAME * 2];
                    int k = opus_decode(s->dec, pkt, n, pcm, FRAME, 0);
                    if (k > 0) { fifo_push(s, pcm, k); s->last_voice = now; }
                }
            }
            continue;
        }
        if (!talking) {
            if (w->trailing > 0) { w->trailing--; send_opus(w, SILENCE, 3); }
            continue;
        }
        if (!w->encd) continue;
        int16_t st[FRAME * 2];
        for (int i = 0; i < FRAME; i++) st[i * 2] = st[i * 2 + 1] = mono[i];
        unsigned char pkt[1275];
        int n = opus_encode(w->encd, st, FRAME, pkt, sizeof pkt);
        if (n > 0) send_opus(w, pkt, n);
    }
}

/* ---------- голосовой gateway ---------- */
static void vw_identify(VW *w) {
    char b[700];
    snprintf(b, sizeof b,
        "{\"op\":0,\"d\":{\"server_id\":\"%llu\",\"user_id\":\"%llu\",\"session_id\":\"%s\",\"token\":\"%s\","
        "\"video\":false,\"max_dave_protocol_version\":%u}}",
        (unsigned long long)w->server_id, (unsigned long long)w->me, w->session, w->token, (unsigned)daveMaxSupportedProtocolVersion());
    ws_text(w, b);
    wipe(b, sizeof b);
}
/* op 7 RESUME: та же сессия, UDP и ключи остаются; seq_ack — последний номер от сервера */
static void vw_resume(VW *w) {
    char b[600];
    snprintf(b, sizeof b, "{\"op\":7,\"d\":{\"server_id\":\"%llu\",\"session_id\":\"%s\",\"token\":\"%s\",\"seq_ack\":%d}}",
             (unsigned long long)w->server_id, w->session, w->token, w->last_seq);
    ws_text(w, b);
    wipe(b, sizeof b);
}
static void vw_heartbeat(VW *w) {
    char b[96];
    snprintf(b, sizeof b, "{\"op\":3,\"d\":{\"t\":%lld,\"seq_ack\":%d}}", (long long)wall_ms(), w->last_seq);
    w->hb_acked = false;
    w->hb_sent = now_ms();
    ws_text(w, b);
}
static bool ip_discovery(VW *w, const char *ip, int port) {
    if (!udp_resolve(ip, (u16)port, w->addr)) return false;
    w->udp = udp_open();                         /* до ASSOCIATE: туннель должен знать точный порт сокета */
    if (w->udp < 0) return false;
    if (w->via_socks) {
        /* при VLESS: сначала UDP ASSOCIATE; без него UDP не уходит вовсе */
        char e[96];
        if (!socks_open_udp(&w->sx, w->sx_ip, w->sx_port, w->sx_user, w->sx_pass, ip, port, w->udp, &V.stop, e, sizeof e)) {
            if (!atomic_load(&V.stop)) {
                char m[200];                     /* VoiceStatus.error обрежет до 127 байт сам */
                snprintf(m, sizeof m, "VLESS: UDP голоса не поднят (%s)", e);
                vw_net_error(w, m, m);
            }
            return true;                         /* ошибка уже выставлена */
        }
        w->sx_wait = sockwait_new((intptr_t)w->sx.tcp);
    }
    unsigned char req[74] = {0};
    req[1] = 1; req[3] = 70;
    req[4] = (unsigned char)(w->ssrc >> 24); req[5] = (unsigned char)(w->ssrc >> 16);
    req[6] = (unsigned char)(w->ssrc >> 8); req[7] = (unsigned char)w->ssrc;
    w->udp_wait = sockwait_new(w->udp);
    for (int attempt = 0; attempt < 5 && !atomic_load(&V.stop); attempt++) {
        vudp_send(w, req, sizeof req);
        i64 until = now_ms() + 1000;
        while (now_ms() < until && !atomic_load(&V.stop)) {
            Waitable wt = sockwait_get(w->udp_wait);
            plat_wait(&wt, 1, 100);
            sockwait_clear(w->udp_wait);
            unsigned char r[160];
            int n;
            while ((n = vudp_recv(w, r, sizeof r)) >= 0) {
                if (n < 74 || r[0] != 0 || r[1] != 2) continue;
                char myip[65];
                memcpy(myip, r + 8, 64);
                myip[64] = 0;
                for (int i = 0; myip[i]; i++) if (!((myip[i] >= '0' && myip[i] <= '9') || myip[i] == '.' || myip[i] == ':' || (myip[i] >= 'a' && myip[i] <= 'f'))) return false;
                int myport = r[72] << 8 | r[73];
                char b[256];
                snprintf(b, sizeof b, "{\"op\":1,\"d\":{\"protocol\":\"udp\",\"data\":{\"address\":\"%s\",\"port\":%d,\"mode\":\"aead_aes256_gcm_rtpsize\"}}}", myip, myport);
                ws_text(w, b);
                return true;
            }
        }
    }
    return false;
}

static void vw_text(VW *w, const char *text, size_t len) {
    cJSON *j = json_parse(text, len);
    if (!j) { json_done(); return; }
    int op = (int)jint(j, "op", -1);
    const cJSON *d = jget(j, "d");
    const cJSON *sq = jget(j, "seq");
    if (cJSON_IsNumber(sq)) w->last_seq = (int)sq->valuedouble;
    switch (op) {
    case 8:
        w->hb_interval = jint(d, "heartbeat_interval", 13750);
        if (w->hb_interval < 1000) w->hb_interval = 1000;
        w->next_hb = now_ms() + w->hb_interval / 4;
        w->hb_acked = true;
        if (w->resuming) vw_resume(w); else vw_identify(w);
        break;
    case 9:                                      /* RESUMED: звонок продолжается с теми же UDP и ключами */
        if (!w->resuming) break;
        w->resuming = false;
        w->rc_try = 0;
        w->rc_start = w->rc_at = 0;
        if (!w->session_ready) status_set(VS_CONNECTING, NULL);
        else if (w->dave) status_set(w->dave_on ? VS_CONNECTED : VS_DAVE, NULL);
        else status_set(VS_CONNECTED, "звонок без E2EE: микрофон не передаётся");
        break;
    case 6:
        w->hb_acked = true;
        mutex_lock(V.mu);
        V.st.ping = (int)(now_ms() - w->hb_sent);
        mutex_unlock(V.mu);
        break;
    case 2: {
        if (w->udp >= 0) break;                  /* повторный READY: сокет уже есть */
        w->ssrc = (u32)jint(d, "ssrc", 0);
        const char *ip = jstr(d, "ip");
        int port = (int)jint(d, "port", 0);
        bool mode_ok = false;
        const cJSON *m;
        cJSON_ArrayForEach(m, jget(d, "modes")) if (cJSON_IsString(m) && !strcmp(m->valuestring, "aead_aes256_gcm_rtpsize")) mode_ok = true;
        if (!mode_ok) { vw_error(w, "сервер не поддерживает AES-GCM"); atomic_store(&V.stop, true); break; }
        if (!ip || !port || !ip_discovery(w, ip, port))
            vw_net_error(w, "UDP не отвечает (файрвол/NAT?)", "VLESS: голосовой UDP не проходит через туннель (UDP на сервере выключен?)");
        break;
    }
    case 4: {
        const char *mode = jstr(d, "mode");
        const cJSON *k = jget(d, "secret_key");
        if (!mode || strcmp(mode, "aead_aes256_gcm_rtpsize") || cJSON_GetArraySize(k) != 32) {
            vw_error(w, "неверное описание сессии"); atomic_store(&V.stop, true); break;
        }
        int i = 0;
        const cJSON *x;
        cJSON_ArrayForEach(x, k) w->key[i++] = (unsigned char)x->valuedouble;
        w->session_ready = true;
        u16 dv = (u16)jint(d, "dave_protocol_version", 0);
        if (dv > 0) { dave_init(w, dv); dave_send_kp(w); status_set(VS_DAVE, NULL); }
        else status_set(VS_CONNECTED, "звонок без E2EE: микрофон не передаётся");
        char b[96];
        snprintf(b, sizeof b, "{\"op\":5,\"d\":{\"speaking\":1,\"delay\":0,\"ssrc\":%u}}", (unsigned)w->ssrc);
        ws_text(w, b);
        break;
    }
    case 5: {
        u64 uid = jid(d, "user_id");
        u32 ssrc = (u32)jint(d, "ssrc", 0);
        if (uid && ssrc) {
            int i;
            for (i = 0; i < w->nsmap && w->smap[i].ssrc != ssrc; i++) {}
            if (i == w->nsmap && w->nsmap < 64) w->nsmap++;
            if (i < 64) { w->smap[i].ssrc = ssrc; w->smap[i].user = uid; }
            dave_add_user(w, uid);
            src_get(w, ssrc, uid);
        }
        break;
    }
    case 11: {
        const cJSON *u;
        cJSON_ArrayForEach(u, jget(d, "user_ids")) dave_add_user(w, id_of(u));
        break;
    }
    case 18: case 20: { u64 uid = jid(d, "user_id"); if (uid) dave_add_user(w, uid); break; }
    case 13: { u64 uid = jid(d, "user_id"); if (uid) src_remove_user(w, uid); break; }
    case 21: {
        int ver = (int)jint(d, "protocol_version", 1), tid = (int)jint(d, "transition_id", 0);
        w->dave_pending = (u16)(ver < 0 ? 0 : ver);
        if (ver == 0) for (int s = 0; s < w->nsrc; s++) if (w->src[s].dd) daveDecryptorTransitionToPassthroughMode(w->src[s].dd, true);
        if (tid == 0) dave_execute(w); else send_tid(w, 23, tid);
        break;
    }
    case 22: dave_execute(w); break;
    case 24: {
        int epoch = (int)jint(d, "epoch", 0), ver = (int)jint(d, "protocol_version", 1);
        if (epoch == 1 && ver > 0) { dave_init(w, (u16)ver); dave_send_kp(w); }
        break;
    }
    default: break;
    }
    json_done();
}

static void vw_binary(VW *w, const unsigned char *b, size_t n) {
    if (n < 3) return;
    w->last_seq = b[0] << 8 | b[1];
    u8 op = b[2];
    const unsigned char *p = b + 3;
    size_t pn = n - 3;
    if (!w->dave) return;
    char ids[64][24];
    const char *ptrs[64];
    switch (op) {
    case 25: daveSessionSetExternalSender(w->dave, p, pn); break;
    case 27: {
        int k = dave_ids(w, ids, ptrs);
        uint8_t *out = NULL;
        size_t on = 0;
        daveSessionProcessProposals(w->dave, p, pn, ptrs, (size_t)k, &out, &on);
        if (out && on) ws_bin(w, 28, out, on);
        if (out) daveFree(out);
        break;
    }
    case 29: case 30: {
        if (pn < 2) break;
        int tid = p[0] << 8 | p[1];
        bool ok;
        if (op == 29) {
            DAVECommitResultHandle r = daveSessionProcessCommit(w->dave, p + 2, pn - 2);
            if (r && daveCommitResultIsIgnored(r)) { daveCommitResultDestroy(r); break; }
            ok = r && !daveCommitResultIsFailed(r);
            if (r) daveCommitResultDestroy(r);
        } else {
            int k = dave_ids(w, ids, ptrs);
            DAVEWelcomeResultHandle r = daveSessionProcessWelcome(w->dave, p + 2, pn - 2, ptrs, (size_t)k);
            ok = r != NULL;
            if (r) daveWelcomeResultDestroy(r);
        }
        if (ok) {
            w->dave_ready = true;
            dave_apply_receive(w);
            if (tid == 0) dave_execute(w); else send_tid(w, 23, tid);
        } else {
            send_tid(w, 31, tid);
            dave_init(w, w->dave_ver ? w->dave_ver : 1);
            dave_send_kp(w);
        }
        break;
    }
    default: break;
    }
}

static bool vw_read_ws(VW *w) {
    if (!w->ws_buf) w->ws_buf = xmalloc(VW_WSBUF);
    char *buf = w->ws_buf;
    for (int guard = 0; guard < 256; guard++) {
        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode rc = curl_ws_recv(w->ws, buf, VW_WSBUF, &got, &meta);
        if (rc == CURLE_AGAIN) return true;
        if (rc != CURLE_OK || !meta) { w->ws_dead = true; w->ws_close = 0; return false; }   /* сетевой обрыв */
        if (meta->flags & CURLWS_CLOSE) {
            w->ws_dead = true;
            w->ws_close = got >= 2 ? ((unsigned char)buf[0] << 8 | (unsigned char)buf[1]) : 1005;
            return false;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;
        if (got) {
            if (w->in.len + got > (4u << 20)) { vw_error(w, "голосовой сервер прислал слишком большое сообщение"); return false; }
            buf_put(&w->in, buf, got);
        }
        if (meta->bytesleft || (meta->flags & CURLWS_CONT)) continue;
        if (w->in.len) {
            if (meta->flags & CURLWS_BINARY) vw_binary(w, (unsigned char *)w->in.p, w->in.len);
            else vw_text(w, w->in.p, w->in.len);
        }
        w->in.len = 0;
    }
    return true;
}

static bool host_ok(const char *ep, char *host, size_t n) {
    /* endpoint вида "c-xxx.discord.media:443"; принимаем только *.discord.media */
    size_t len = strcspn(ep, ":/");
    if (len == 0 || len >= n) return false;
    for (size_t i = 0; i < len; i++) {
        char c = ep[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    if (len <= 14 || memcmp(ep + len - 14, ".discord.media", 14)) return false;
    memcpy(host, ep, len);
    host[len] = 0;
    return true;
}

static void publish(VW *w) {
    i64 now = now_ms();
    if (now < w->next_status) return;
    w->next_status = now + 200;
    mutex_lock(V.mu);
    V.st.nspeaking = 0;
    if (w->talking && !atomic_load(&V.mute) && V.st.nspeaking < 16) V.st.speaking[V.st.nspeaking++] = w->me;
    for (int i = 0; i < w->nsrc && V.st.nspeaking < 16; i++)
        if (w->src[i].user && now - w->src[i].last_voice < 300) V.st.speaking[V.st.nspeaking++] = w->src[i].user;
    V.st.mic_level = w->level;
    V.st.mic_live = w->au && !atomic_load(&V.mute) && !atomic_load(&V.deaf) && (w->dave_on || w->demo);
    V.st.dave = w->dave_on;
    if (w->dave_on && V.st.phase == VS_DAVE) V.st.phase = VS_CONNECTED;
    /* Один в канале: группе DAVE не с кем согласовывать ключи, Discord пришлёт их, когда кто-то зайдёт.
     * Считаем себя подключёнными (голос всё равно не уходит без dave_on), а при появлении собеседника
     * до установки шифрования снова показываем «Шифрование». */
    else if (w->dave && !w->dave_on && w->session_ready) {
        bool alone = w->nusers <= 1;
        if (alone && V.st.phase == VS_DAVE) V.st.phase = VS_CONNECTED;
        else if (!alone && V.st.phase == VS_CONNECTED) V.st.phase = VS_DAVE;
    }
    u32 h = (u32)V.st.phase * 31u + (u32)V.st.nspeaking * 7u + (V.st.mic_live ? 1u : 0u) + (V.st.dave ? 2u : 0u) + (u32)V.st.ping / 10u * 131u;
    for (int i = 0; i < V.st.nspeaking; i++) h = h * 33u + (u32)V.st.speaking[i];
    mutex_unlock(V.mu);
    if (h != w->pub_hash) { w->pub_hash = h; wake_signal(g_wake); }
}

static int abort_cb(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)p; (void)a; (void)b; (void)c; (void)d;
    return atomic_load(&V.stop) ? 1 : 0;          /* выход из звонка прерывает подключение */
}

/* Подключить голосовой WebSocket (TLS + Upgrade). Через туннель — прокси из vless.c (копия в w->proxy);
 * без туннеля — пустая строка: иначе curl взял бы прокси из переменных окружения.
 * *proxy_bad — при VLESS curl не принял прокси: напрямую не подключаемся. */
static bool ws_connect(VW *w, long connect_timeout, bool *proxy_bad) {
    char url[200];
    snprintf(url, sizeof url, "wss://%s/?v=8", w->host);
    *proxy_bad = false;
    w->ws = curl_easy_init();
    if (!w->ws) return false;
    CURLcode pc = curl_easy_setopt(w->ws, CURLOPT_PROXY, w->proxy);
    if (w->via_socks && (pc != CURLE_OK || !w->proxy[0])) {
        *proxy_bad = true;
        curl_easy_cleanup(w->ws);
        w->ws = NULL;
        return false;
    }
    curl_easy_setopt(w->ws, CURLOPT_URL, url);
    curl_easy_setopt(w->ws, CURLOPT_PROTOCOLS_STR, "wss");
    curl_easy_setopt(w->ws, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(w->ws, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(w->ws, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(w->ws, CURLOPT_CONNECTTIMEOUT, connect_timeout);
    curl_easy_setopt(w->ws, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(w->ws, CURLOPT_TIMEOUT, connect_timeout + 5);    /* TLS + Upgrade целиком */
    curl_easy_setopt(w->ws, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(w->ws, CURLOPT_XFERINFOFUNCTION, abort_cb);
    net_set_ca(w->ws);
    if (curl_easy_perform(w->ws) != CURLE_OK) {
        curl_easy_cleanup(w->ws);
        w->ws = NULL;
        return false;
    }
    curl_easy_setopt(w->ws, CURLOPT_TIMEOUT, 0L);
    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(w->ws, CURLINFO_ACTIVESOCKET, &sock);
    w->ws_wait = sockwait_new((intptr_t)sock);
    w->ws_dead = false;
    w->ws_close = 0;
    w->in.len = 0;
    w->hb_interval = 0;
    w->hb_acked = true;
    w->next_hb = now_ms() + 15000;
    return true;
}

static void ws_drop(VW *w) {
    if (w->ws_wait) { sockwait_free(w->ws_wait); w->ws_wait = NULL; }
    if (w->ws) { curl_easy_cleanup(w->ws); w->ws = NULL; }
    w->in.len = 0;
    w->next_hb = 0;
}

/* Следующая попытка: паузы 1 -> 2 -> 5 -> 5 ... с, не дольше 30 с от обрыва. */
static void rc_schedule(VW *w) {
    static const int DELAY[] = { 1000, 2000, 5000 };
    i64 now = now_ms();
    if (now - w->rc_start >= 30000) {
        vw_net_error(w, "голосовое соединение потеряно и не восстановилось за 30 с",
                     "VLESS: голосовое соединение через туннель не восстановилось за 30 с");
        return;
    }
    int d = DELAY[w->rc_try < 2 ? w->rc_try : 2];
    w->rc_try++;
    w->rc_at = now + d;
    char m[96];
    snprintf(m, sizeof m, "связь потеряна, переподключение через %d с", d / 1000);
    status_set(VS_CONNECTING, m);
}

/* Обрыв WebSocket: продолжить ту же сессию (RESUME), войти заново через gateway или завершить. */
static void ws_lost(VW *w) {
    int code = w->ws_close;
    bool was_resuming = w->resuming;
    ws_drop(w);
    w->ws_dead = false;
    w->resuming = false;
    char e[96];
    switch (code) {
    case 4014: vw_error(w, "вас отключили от канала"); return;              /* не переподключаемся */
    case 4021: vw_error(w, "голос отключён сервером: слишком много запросов"); return;
    case 4022: vw_error(w, "звонок завершён"); return;
    case 4016: case 4017:
        snprintf(e, sizeof e, "сквозное шифрование не согласовано (%d)", code);
        vw_error(w, e);
        return;
    case 0: case 1000: case 1001: case 1005: case 1006: case 4015:
        break;                                   /* сеть или сервер упали: сессию можно продолжить */
    default:
        if (code == 4006 || code == 4009 || (was_resuming && code >= 4000)) {
            /* RESUME не принят (сессия устарела): новый вход через gateway, op 4 — в главном потоке */
            status_set(VS_CONNECTING, "сессия голоса устарела: вход заново");
            atomic_store(&V.need_rejoin, true);
            atomic_store(&V.stop, true);
            wake_signal(g_wake);
            return;
        }
        snprintf(e, sizeof e, "голос закрыт (код %d)", code);
        vw_error(w, e);
        return;
    }
    if (!w->session_ready) {                     /* сессия не успела начаться: продолжать нечего */
        vw_net_error(w, "голосовое соединение потеряно", "VLESS: голосовое соединение через туннель потеряно");
        return;
    }
    if (!w->rc_start) { w->rc_start = now_ms(); w->rc_try = 0; }
    rc_schedule(w);
}

static int voice_thread(void *arg) {
    VW *w = arg;
    thread_audio_priority();
    char err[128] = "";
    w->udp = -1;
    socks_reset(&w->sx);
    char hm[96], ho[96];
    dev_hints(hm, ho);
    int gen = atomic_load(&V.dev_gen);
    w->au = vopen(hm[0] ? hm : NULL, ho[0] ? ho : NULL, err, sizeof err);
    if (!w->au) { status_set(VS_FAILED, err[0] ? err : "нет звукового устройства"); }
    int oe;
    w->encd = opus_encoder_create(48000, 2, OPUS_APPLICATION_VOIP, &oe);
    if (oe == OPUS_OK) {
        /* Профиль perf в живом звонке: ~60% процессора — кодировщик SILK, из них половина —
         * квантователь с отложенным решением, который включается со сложности 2. По умолчанию
         * сложность 1 (как в мобильных клиентах), --hq — 8. */
        opus_encoder_ctl(w->encd, OPUS_SET_BITRATE(O.hq ? 64000 : 48000));
        opus_encoder_ctl(w->encd, OPUS_SET_COMPLEXITY(O.hq ? 8 : 1));
        /* Полоса 8 кГц (WB, «HD-голос»): кодировщик работает в чистом SILK без CELT и части
         * пересчётов частоты — по замеру на треть дешевле; для речи разницы на слух нет. */
        if (!O.hq) opus_encoder_ctl(w->encd, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
        opus_encoder_ctl(w->encd, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(w->encd, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(w->encd, OPUS_SET_FORCE_CHANNELS(1));
        opus_encoder_ctl(w->encd, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    } else w->encd = NULL;
    w->enc = EVP_CIPHER_CTX_new();
    w->dec = EVP_CIPHER_CTX_new();

    if (w->demo) {
        status_set(VS_CONNECTED, NULL);
        mutex_lock(V.mu); V.st.dave = false; V.st.ping = 0; mutex_unlock(V.mu);
    } else {
        status_set(VS_CONNECTING, NULL);
        bool proxy_bad;
        if (!ws_connect(w, 10, &proxy_bad)) {
            if (proxy_bad) vw_net_error(w, "", "VLESS: голосовой WebSocket нельзя пустить через туннель (curl без прокси?)");
            else if (!atomic_load(&V.stop)) vw_net_error(w, "голосовой сервер недоступен", "VLESS: голосовой сервер недоступен через туннель");
            goto out;
        }
    }
    w->next_frame = now_ms();
    w->t_audio = now_ms();
    while (!atomic_load(&V.stop)) {
        /* Звук и UDP обслуживаются ровным тактом 20 мс (длина кадра Opus): одно пробуждение на кадр
         * вместо сотен в секунду от устройства и пакетов. Вне такта будят только WebSocket, сокет
         * управления SOCKS5 и команды. */
        Waitable wt[4];
        int n = 0;
        if (w->ws_wait) wt[n++] = sockwait_get(w->ws_wait);
        if (w->sx_wait) wt[n++] = sockwait_get(w->sx_wait);
        wt[n++] = wake_waitable(V.cmd);
        i64 now = now_ms();
        i64 t = w->next_frame;
        if (w->next_hb && w->next_hb < t) t = w->next_hb;
        plat_wait(wt, n, (int)(t > now ? t - now : 0));
        wake_clear(V.cmd);

        if (w->sx_wait) {
            /* TCP управления UDP ASSOCIATE закрылся: туннель для голоса умер, напрямую не идём */
            sockwait_clear(w->sx_wait);
            if (!socks_alive(&w->sx)) { vw_net_error(w, "", "VLESS: туннель для голоса оборвался (Xray остановлен?)"); break; }
        }
        if (w->ws_wait) { sockwait_clear(w->ws_wait); vw_read_ws(w); }
        if (atomic_load(&V.stop)) break;
        if (w->ws_dead) { ws_lost(w); if (atomic_load(&V.stop)) break; }
        now = now_ms();
        if (!w->ws && w->rc_at && now >= w->rc_at) {
            /* переподключение: TLS + Upgrade, затем Hello -> RESUME (op 7) */
            w->rc_at = 0;
            status_set(VS_CONNECTING, "переподключение голоса...");
            bool proxy_bad;
            if (ws_connect(w, 5, &proxy_bad)) { w->resuming = true; w->rc_conn = now_ms(); }
            else if (proxy_bad) { vw_net_error(w, "", "VLESS: голосовой WebSocket нельзя пустить через туннель"); break; }
            else if (!atomic_load(&V.stop)) rc_schedule(w);
            if (atomic_load(&V.stop)) break;
            now = now_ms();
        }
        if (w->ws && w->resuming && now - w->rc_conn > 10000) { w->ws_dead = true; w->ws_close = 0; continue; }   /* RESUMED не пришёл */
        if (now < w->next_frame) continue;
        w->next_frame += 20;
        if (now >= w->next_gc) {
            /* декодер и буфер — только пока человек говорит: молчащие дольше 30 с освобождаются */
            w->next_gc = now + 1000;
            for (int i = w->nsrc - 1; i >= 0; i--)
                if (now - w->src[i].last_pkt > 30000) { src_free(&w->src[i]); w->src[i] = w->src[--w->nsrc]; }
        }
        if (w->next_frame <= now) w->next_frame = now + 20;      /* проспали — не догоняем */
        if (w->udp >= 0) {
            unsigned char pkt[2048];
            int k;
            for (int g = 0; g < 128 && (k = vudp_recv(w, pkt, sizeof pkt)) >= 0; g++) if (k > 0) on_rtp(w, pkt, k);
        }
        if (w->au && (audio_lost(w->au) || gen != atomic_load(&V.dev_gen))) {   /* отключили или выбрали другое */
            gen = atomic_load(&V.dev_gen);
            audio_close(w->au);
            dev_hints(hm, ho);
            w->au = vopen(hm[0] ? hm : NULL, ho[0] ? ho : NULL, err, sizeof err);
            w->t_audio = now_ms();
            w->cap_frames = w->out_frames = 0;
            if (!w->au) status_set(VS_FAILED, "звуковое устройство отключено");
        }
        capture_tick(w);
        mix_out(w);
        now = now_ms();
        if (w->ws && w->next_hb && now >= w->next_hb) {
            /* сервер не ответил на прошлый heartbeat: соединение считаем потерянным -> RESUME */
            if (!w->hb_acked && w->hb_interval) { w->ws_dead = true; w->ws_close = 0; continue; }
            vw_heartbeat(w);
            w->next_hb = now + (w->hb_interval ? w->hb_interval : 15000);
        }
        if (w->session_ready && w->udp >= 0 && now >= w->next_ka) {
            /* keepalive UDP раз в 5 с, чтобы NAT не закрыл порт (и во время переподключения WebSocket) */
            unsigned char ka[2] = { 0x13, 0x37 };
            vudp_send(w, ka, 2);
            w->next_ka = now + 5000;
        }
        publish(w);
    }
out:
    for (int i = 0; i < w->nsrc; i++) src_free(&w->src[i]);
    if (w->denc) daveEncryptorDestroy(w->denc);
    if (w->dave) daveSessionDestroy(w->dave);
    if (w->encd) opus_encoder_destroy(w->encd);
    if (w->enc) EVP_CIPHER_CTX_free(w->enc);
    if (w->dec) EVP_CIPHER_CTX_free(w->dec);
    if (w->ws_wait) sockwait_free(w->ws_wait);
    if (w->udp_wait) sockwait_free(w->udp_wait);
    if (w->sx_wait) sockwait_free(w->sx_wait);
    if (w->ws) curl_easy_cleanup(w->ws);
    udp_close(w->udp);
    socks_close(&w->sx);                         /* закрытие TCP управления снимает сопоставление UDP в Xray */
    if (w->au) audio_close(w->au);
    buf_free(&w->in);
    free(w->mix_acc);
    free(w->mix_out);
    if (w->ws_buf) { wipe(w->ws_buf, VW_WSBUF); free(w->ws_buf); }   /* там были кадры голосового gateway */
    json_done();
    wipe(w, sizeof *w);
    free(w);
    mutex_lock(V.mu);
    V.st.nspeaking = 0;
    V.st.mic_live = false;
    V.st.dave = false;
    mutex_unlock(V.mu);
    wake_signal(g_wake);
    return 0;
}

/* ======================= управление из главного потока ======================= */
static void stop_thread(void) {
    if (!V.thr) return;
    atomic_store(&V.stop, true);
    wake_signal(V.cmd);
    thread_join(V.thr);
    V.thr = NULL;
    plat_trim();                                /* декодеры, буферы и сессия DAVE освобождены: вернуть память системе */
    atomic_store(&V.stop, false);
}

static void maybe_start(void) {
    mutex_lock(V.mu);
    bool go = V.st.phase == VS_WAITING && V.have_session && V.have_server;
    mutex_unlock(V.mu);
    if (!go) return;
    /* VLESS: голос (WebSocket и UDP) — только через туннель. Туннель поднимается — ждём (повтор из
     * voice_status); не работает — голос не подключается, напрямую не идём. */
    VlessState vl = vless_state();
    if (vl == VL_STARTING) { status_set(VS_WAITING, "ожидание туннеля VLESS"); return; }
    char px[300] = "", sip[64] = "", su[160] = "", sp[160] = "";
    int sport = 0;
    if (vl != VL_OFF) {
        const char *u = vless_proxy_url();
        bool ok = vl == VL_UP && u && strlen(u) < sizeof px &&
                  vless_socks(sip, sizeof sip, &sport, su, sizeof su, sp, sizeof sp);
        if (ok) snprintf(px, sizeof px, "%s", u);
        else {
            wipe(V.token, sizeof V.token);
            wipe(su, sizeof su);
            wipe(sp, sizeof sp);
            status_set(VS_FAILED, "VLESS: туннель не работает - голос не подключён");
            vless_report_failure("голос");
            atomic_store(&V.need_leave, true);   /* Discord уже ждёт нас в канале: сказать, что вышли */
            return;
        }
    }
    V.via_vless = vl != VL_OFF;
    VW *w = xcalloc(1, sizeof *w);
    w->via_socks = V.via_vless;
    memcpy(w->proxy, px, sizeof px);
    memcpy(w->sx_ip, sip, sizeof sip);
    memcpy(w->sx_user, su, sizeof su);
    memcpy(w->sx_pass, sp, sizeof sp);
    w->sx_port = sport;
    wipe(px, sizeof px);
    wipe(su, sizeof su);
    wipe(sp, sizeof sp);
    w->server_id = V.st.guild_id ? V.st.guild_id : V.st.channel_id;
    w->channel_id = V.st.channel_id;
    w->me = S.me_id;
    snprintf(w->session, sizeof w->session, "%s", V.session);
    snprintf(w->token, sizeof w->token, "%s", V.token);
    /* значения от сервера идут в JSON без экранирования: допускаем только безопасные символы */
    bool safe = w->session[0] && w->token[0];
    for (const char *c = w->session; *c && safe; c++) safe = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '_' || *c == '-';
    for (const char *c = w->token; *c && safe; c++) safe = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '_' || *c == '-';
    if (!safe || !host_ok(V.endpoint, w->host, sizeof w->host)) {
        wipe(w, sizeof *w);
        free(w);
        wipe(V.token, sizeof V.token);
        status_set(VS_FAILED, safe ? "голосовой сервер не из discord.media" : "неверные данные голосовой сессии");
        return;
    }
    wipe(V.token, sizeof V.token);
    status_set(VS_CONNECTING, NULL);
    mt_stop_thread();                           /* тест микрофона уступает устройство звонку */
    V.thr = thread_start(voice_thread, w);
    if (!V.thr) { wipe(w, sizeof *w); free(w); status_set(VS_FAILED, "не удалось запустить поток"); }   /* в w — токен голоса */
}

int voice_restarts;                             /* selftest: сколько раз вход начинался с полного выхода */
void voice_join(u64 guild_id, u64 channel_id) {
    /* №149 (ревью 13 С1): переход в другой канал — полный выход и обычный вход (~1 с). Переиспользовать
     * голосовое соединение нельзя: группа E2EE (DAVE/MLS) — по каналу, без нового commit/welcome голос
     * ушёл бы под ключом старой группы. Инвариант: без установленного E2EE звук не передаётся. */
    if (V.st.phase != VS_IDLE) { voice_leave(); voice_restarts++; }
    mutex_lock(V.mu);
    memset(&V.st, 0, sizeof V.st);
    V.st.phase = VS_WAITING;
    V.st.guild_id = guild_id;
    V.st.channel_id = channel_id;
    V.have_session = V.have_server = false;
    mutex_unlock(V.mu);
    if (O.demo) {
        VW *w = xcalloc(1, sizeof *w);
        w->demo = true;
        w->me = S.me_id;
        w->channel_id = channel_id;
        mt_stop_thread();                       /* тест микрофона уступает устройство звонку */
        V.thr = thread_start(voice_thread, w);
        store_vs_self(guild_id, channel_id, atomic_load(&V.mute), atomic_load(&V.deaf));   /* демо: вы в списке участников */
        return;
    }
    snprintf(V.gw_sess, sizeof V.gw_sess, "%s", S.session_id ? S.session_id : "");
    gw_voice_state(guild_id, channel_id, atomic_load(&V.mute), atomic_load(&V.deaf));
}

void voice_leave(void) {
    if (V.st.phase == VS_IDLE) return;
    u64 g = V.st.guild_id;
    if (!O.demo) gw_voice_state(g, 0, false, false);
    else store_vs_self(g, 0, false, false);
    stop_thread();
    mutex_lock(V.mu);
    memset(&V.st, 0, sizeof V.st);
    mutex_unlock(V.mu);
    wipe(V.token, sizeof V.token);
    V.have_session = V.have_server = false;
    V.gw_sess[0] = 0;
    ui_dirty();
}

/* Войти в тот же канал заново через gateway (op 4): новая голосовая сессия и сервер.
 * Не больше двух раз подряд без успешного подключения — дальше понятная ошибка. */
static void voice_rejoin(void) {
    u64 g = V.st.guild_id, c = V.st.channel_id;
    if (!c) return;
    if (++V.rejoins > 2) {
        stop_thread();
        if (!O.demo) gw_voice_state(g, 0, false, false);
        status_set(VS_FAILED, "голос не восстановился: войдите в канал заново");
        return;
    }
    voice_join(g, c);
}

/* Паника: остановить голосовой поток без ожидания (процесс сейчас завершится). Для panic.c. */
void voice_panic_stop(void);
void voice_panic_stop(void) {
    atomic_store(&V.mute, true);
    atomic_store(&V.stop, true);
    if (V.cmd) wake_signal(V.cmd);
}

static void resend_state(void) {
    if (V.st.phase == VS_IDLE || O.demo) return;
    gw_voice_state(V.st.guild_id, V.st.channel_id, atomic_load(&V.mute), atomic_load(&V.deaf));
}
void voice_toggle_mute(void) { atomic_store(&V.mute, !atomic_load(&V.mute)); resend_state(); ui_dirty(); }
void voice_toggle_deaf(void) { atomic_store(&V.deaf, !atomic_load(&V.deaf)); resend_state(); ui_dirty(); }

void voice_on_state(u64 guild, u64 channel, u64 user, const char *session_id) {
    if (user != S.me_id || V.st.phase == VS_IDLE) return;
    if (!channel) {
        /* нас отключили (или мы вышли с другого устройства) */
        stop_thread();
        mutex_lock(V.mu);
        memset(&V.st, 0, sizeof V.st);
        mutex_unlock(V.mu);
        wipe(V.token, sizeof V.token);
        wipe(V.session, sizeof V.session);
        V.have_session = V.have_server = false;
        V.gw_sess[0] = 0;
        ui_notice("вы отключены от голосового канала");
        return;
    }
    (void)guild;
    mutex_lock(V.mu);
    V.st.channel_id = channel;
    if (session_id) { snprintf(V.session, sizeof V.session, "%s", session_id); V.have_session = true; }
    mutex_unlock(V.mu);
    maybe_start();
}

void voice_on_server(u64 gid, const char *token, const char *endpoint) {
    if (V.st.phase == VS_IDLE) return;
    u64 want = V.st.guild_id ? V.st.guild_id : V.st.channel_id;
    if (gid != want) return;
    if (!endpoint || !token) { status_set(VS_WAITING, NULL); return; }
    bool restart = V.thr != NULL;
    if (restart) stop_thread();
    mutex_lock(V.mu);
    snprintf(V.token, sizeof V.token, "%s", token);
    snprintf(V.endpoint, sizeof V.endpoint, "%s", endpoint);
    V.have_server = true;
    V.st.phase = VS_WAITING;
    mutex_unlock(V.mu);
    maybe_start();
}

/* ---------- №140/146: устройства и тест микрофона ---------- */
void voice_set_device(int out, const char *name) {
    if (!V.mu || out < 0 || out > 1) return;
    mutex_lock(V.mu);
    snprintf(V.dev[out], sizeof V.dev[out], "%s", name ? name : "");
    mutex_unlock(V.mu);
    atomic_fetch_add(&V.dev_gen, 1);
    if (V.cmd) wake_signal(V.cmd);
}
const char *voice_device(int out) {
    static char b[96];
    if (!V.mu || out < 0 || out > 1) return "";
    mutex_lock(V.mu);
    snprintf(b, sizeof b, "%s", V.dev[out]);
    mutex_unlock(V.mu);
    return b;
}
static struct { PlatThread *thr; PlatWake *wk; atomic_bool stop; atomic_int level; bool on; } MT;
static int mic_thread(void *arg) {
    (void)arg;
    thread_audio_priority();
    char err[128] = "", hm[96], ho[96];
    dev_hints(hm, ho);
    int gen = atomic_load(&V.dev_gen);
    Audio *au = vopen(hm[0] ? hm : NULL, ho[0] ? ho : NULL, err, sizeof err);
    int16_t mono[FRAME];
    int shown = -1, peak = 0;
    i64 t_show = now_ms();
    while (au && !atomic_load(&MT.stop)) {
        Waitable wt = wake_waitable(MT.wk);
        plat_wait(&wt, 1, 20);                   /* такт 20 мс, как у звонка */
        wake_clear(MT.wk);
        if (audio_lost(au) || gen != atomic_load(&V.dev_gen)) {   /* выбрали другой микрофон — переоткрыть */
            gen = atomic_load(&V.dev_gen);
            audio_close(au);
            dev_hints(hm, ho);
            au = vopen(hm[0] ? hm : NULL, ho[0] ? ho : NULL, err, sizeof err);
            if (!au) break;
        }
        for (int g = 0; g < 8 && audio_read_frame(au, mono); g++) {
            double sum = 0;
            for (int i = 0; i < FRAME; i++) sum += (double)mono[i] * mono[i];
            double rms = sqrt(sum / FRAME) / 32768.0, db = rms > 0 ? 20 * log10(rms) : -100;
            int lv = db <= -60 ? 0 : db >= 0 ? 100 : (int)((db + 60) * 100 / 60);
            if (lv > peak) peak = lv;
        }
        memset(mono, 0, sizeof mono);            /* звук — только в памяти и тут же стирается */
        i64 now = now_ms();
        if (now - t_show >= 100) {               /* не чаще 10 раз в секунду */
            if (peak != shown) { atomic_store(&MT.level, peak); shown = peak; wake_signal(g_wake); }
            peak = 0;
            t_show = now;
        }
    }
    if (au) audio_close(au);
    if (!atomic_load(&MT.stop)) { atomic_store(&MT.level, -1); wake_signal(g_wake); }   /* не открылся / отключили */
    return 0;
}
static void mt_stop_thread(void) {
    if (!MT.thr) return;
    atomic_store(&MT.stop, true);
    wake_signal(MT.wk);
    thread_join(MT.thr);
    MT.thr = NULL;
}
void voice_mic_test(bool on) {
    if (!V.mu) return;
    if (on == MT.on) return;
    MT.on = on;
    if (on) {
        atomic_store(&MT.level, 0);
        if (V.thr) return;                      /* в звонке: уровень — из звонка, устройство второй раз не открываем */
        if (!MT.wk) MT.wk = wake_new();
        atomic_store(&MT.stop, false);
        MT.thr = thread_start(mic_thread, NULL);
        if (!MT.thr) atomic_store(&MT.level, -1);
        return;
    }
    mt_stop_thread();
    if (MT.wk) { wake_free(MT.wk); MT.wk = NULL; }
    atomic_store(&MT.level, -1);
}
int voice_mic_level(void) {
    if (!MT.on) return -1;
    if (V.thr && !MT.thr) return atomic_load(&V.call_level);
    return atomic_load(&MT.level);
}
/* ======================= №69 голосовые сообщения ======================= */
/* Поток: curl (через тот же маршрут, что картинки: VLESS или напрямую) -> разбор Ogg по страницам ->
 * очередь пакетов Opus (<= 64 КБ, при заполнении загрузка ставится на паузу) -> декодер -> кольцо PCM
 * 0,5 с -> вывод. Файл целиком не хранится; по окончании всё стирается и освобождается. */
void net_curl_common(CURL *e);
#define VM_OG_CAP (70u << 10)                   /* страница Ogg <= 65307 байт */
#define VM_PQ_CAP (136u << 10)                  /* ревью 13 М1: 64 КБ впрок + целая страница Ogg (<= 65307) */
#define VM_PQ_HI (64u << 10)
#define VM_RING 24000                           /* 0,5 с моно */
typedef struct {
    u8 *og; size_t ogn;                         /* недоразобранные байты Ogg */
    u8 pk[8192]; size_t pkn; bool pk_bad;       /* собираемый пакет (через страницы) */
    u8 *pq; size_t pqn;                         /* очередь пакетов: [u16 длина][байты] */
    int npk;                                    /* пакетов увидено (0 — OpusHead, 1 — OpusTags) */
    int ch, preskip;
    OpusDecoder *dec;
    int16_t ring[VM_RING]; int rh, rn;          /* кольцо PCM моно */
    int16_t pcm[5760 * 2];                      /* кадр декодера (ревью 13 М2: не static — освобождается) */
    bool bad;                                   /* поток испорчен */
} VmDec;
static void vm_dec_free(VmDec *d) {
    if (d->og) { wipe(d->og, VM_OG_CAP); free(d->og); }
    if (d->pq) { wipe(d->pq, VM_PQ_CAP); free(d->pq); }
    if (d->dec) opus_decoder_destroy(d->dec);
    wipe(d, sizeof *d);
}
static void vm_packet(VmDec *d, const u8 *p, size_t n) {
    if (d->npk == 0) {                          /* OpusHead */
        if (n < 19 || memcmp(p, "OpusHead", 8)) { d->bad = true; return; }
        d->ch = p[9] == 2 ? 2 : 1;
        d->preskip = p[10] | p[11] << 8;
        int e;
        d->dec = opus_decoder_create(48000, d->ch, &e);
        if (e != OPUS_OK) { d->dec = NULL; d->bad = true; }
    } else if (d->npk >= 2) {                   /* звук (1 — OpusTags) */
        if (n > 0xFFFF || d->pqn + 2 + n > VM_PQ_CAP) { d->bad = true; return; }
        d->pq[d->pqn] = (u8)(n & 255); d->pq[d->pqn + 1] = (u8)(n >> 8);
        memcpy(d->pq + d->pqn + 2, p, n);
        d->pqn += 2 + n;
    }
    d->npk++;
}
/* разобрать целые страницы из og */
static void vm_pages(VmDec *d) {
    size_t off = 0;
    while (!d->bad && d->ogn - off >= 27) {
        const u8 *h = d->og + off;
        if (memcmp(h, "OggS", 4) || h[4] != 0) { d->bad = true; break; }
        size_t ns = h[26];
        if (d->ogn - off < 27 + ns) break;
        size_t body = 0;
        for (size_t i = 0; i < ns; i++) body += h[27 + i];
        if (d->ogn - off < 27 + ns + body) break;
        const u8 *b = h + 27 + ns;
        for (size_t i = 0; i < ns && !d->bad; i++) {
            size_t l = h[27 + i];
            if (d->pkn + l > sizeof d->pk) d->pk_bad = true;
            else memcpy(d->pk + d->pkn, b, l);
            d->pkn += l;
            b += l;
            if (l < 255) {                      /* пакет закончен */
                if (!d->pk_bad) vm_packet(d, d->pk, d->pkn);
                else if (d->npk == 0) d->bad = true;   /* OpusHead не влез — поток испорчен */
                else d->npk++;                  /* длинный OpusTags (или слишком большой пакет) — пропустить, счёт не сбивать */
                d->pkn = 0;
                d->pk_bad = false;
            }
        }
        off += 27 + ns + body;
    }
    if (off) { memmove(d->og, d->og + off, d->ogn - off); d->ogn -= off; }
}
static bool vm_feed(VmDec *d, const void *p, size_t n) {   /* false — испорчено */
    if (!d->og) { d->og = xmalloc(VM_OG_CAP); d->pq = xmalloc(VM_PQ_CAP); }
    while (n && !d->bad) {
        size_t k = VM_OG_CAP - d->ogn < n ? VM_OG_CAP - d->ogn : n;
        if (!k) { d->bad = true; break; }
        memcpy(d->og + d->ogn, p, k);
        d->ogn += k;
        p = (const u8 *)p + k;
        n -= k;
        vm_pages(d);
    }
    return !d->bad;
}
/* декодировать пакеты, пока в кольце есть место на самый длинный кадр (120 мс) */
static void vm_decode(VmDec *d) {
    int16_t *pcm = d->pcm;
    while (d->dec && d->pqn >= 2 && VM_RING - d->rn >= 5760) {
        size_t n = (size_t)d->pq[0] | (size_t)d->pq[1] << 8;
        int got = opus_decode(d->dec, d->pq + 2, (opus_int32)n, pcm, 5760, 0);
        memmove(d->pq, d->pq + 2 + n, d->pqn - 2 - n);
        d->pqn -= 2 + n;
        if (got < 0) continue;                  /* битый пакет — пропустить */
        for (int i = 0; i < got; i++) {
            int16_t s = d->ch == 2 ? (int16_t)(((int)pcm[i * 2] + pcm[i * 2 + 1]) / 2) : pcm[i];
            if (d->preskip > 0) { d->preskip--; continue; }
            d->ring[(d->rh + d->rn) % VM_RING] = s;
            d->rn++;
        }
    }
}

static struct {
    PlatMutex *mu;
    PlatThread *thr;
    PlatWake *wk;
    atomic_bool stop, done;
    atomic_int pos_ms;
    char url[1100], proxy[300], out[96];
    VoiceMsgState st;                           /* под mu */
    i64 demo_t0;                                /* демо: воспроизведение по часам, без звука */
} VM;
static void vm_err(const char *e) {
    mutex_lock(VM.mu);
    snprintf(VM.st.err, sizeof VM.st.err, "%s", e);
    mutex_unlock(VM.mu);
}
typedef struct { VmDec d; CURL *e; bool paused; } VmCtx;
static size_t vm_write(char *p, size_t sz, size_t nm, void *u) {
    VmCtx *c = u;
    if (atomic_load(&VM.stop)) return 0;
    if (c->d.pqn + c->d.ogn > VM_PQ_HI) { c->paused = true; return CURL_WRITEFUNC_PAUSE; }   /* хватит впрок — пауза */
    return vm_feed(&c->d, p, sz * nm) ? sz * nm : 0;
}
static int vm_thread(void *arg) {
    (void)arg;
    thread_audio_priority();
    VmCtx *c = xcalloc(1, sizeof *c);
    char err[128] = "";
    Audio *au = g_audio_fake ? NULL : audio_open_out(VM.out[0] ? VM.out : NULL, err, sizeof err);
    CURLM *m = NULL;
    if (!au) { vm_err(g_audio_fake ? "звук выключен (selftest)" : err[0] ? err : "нет устройства вывода"); goto out; }
    c->e = curl_easy_init();
    m = curl_multi_init();
    if (!c->e || !m) { vm_err("не удалось начать загрузку"); goto out; }
    net_curl_common(c->e);
    curl_easy_setopt(c->e, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c->e, CURLOPT_PROXY, VM.proxy);
    curl_easy_setopt(c->e, CURLOPT_NOPROXY, "");
    if (VM.proxy[0]) curl_easy_setopt(c->e, CURLOPT_SOCKS5_AUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(c->e, CURLOPT_URL, VM.url);
    curl_easy_setopt(c->e, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c->e, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(25 << 20));
    curl_easy_setopt(c->e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c->e, CURLOPT_WRITEFUNCTION, vm_write);
    curl_easy_setopt(c->e, CURLOPT_WRITEDATA, c);
    curl_multi_add_handle(m, c->e);
    bool eof = false, started = false;
    long played = 0;                            /* сэмплов отдано на вывод */
    i64 last_pub = 0, idle_since = 0;
    int16_t st2[960 * 2];
    while (!atomic_load(&VM.stop)) {
        int running = 0;
        curl_multi_perform(m, &running);
        CURLMsg *msg;
        int q;
        while ((msg = curl_multi_info_read(m, &q))) {
            if (msg->msg != CURLMSG_DONE) continue;
            long status = 0;
            curl_easy_getinfo(c->e, CURLINFO_RESPONSE_CODE, &status);
            if (msg->data.result != CURLE_OK || status != 200) {
                char b[120];
                snprintf(b, sizeof b, status ? "голосовое не загрузилось: HTTP %ld" : "голосовое не загрузилось: нет связи", status);
                vm_err(b);
                atomic_store(&VM.stop, true);
            }
            eof = true;
        }
        if (c->d.bad) { vm_err("голосовое сообщение повреждено"); break; }
        vm_decode(&c->d);
        if (c->paused && c->d.pqn + c->d.ogn < VM_PQ_HI / 2) { c->paused = false; curl_easy_pause(c->e, CURLPAUSE_CONT); }
        if (!started && (c->d.rn >= 4800 || (eof && c->d.rn))) started = true;   /* 100 мс впрок — начинаем */
        if (started) {
            int space = audio_render_space(au);
            while (space >= 960 && c->d.rn > 0) {
                int k = c->d.rn < 960 ? c->d.rn : 960;
                for (int i = 0; i < k; i++) { int16_t s = c->d.ring[(c->d.rh + i) % VM_RING]; st2[i * 2] = st2[i * 2 + 1] = s; }
                audio_render_write(au, st2, k);
                c->d.rh = (c->d.rh + k) % VM_RING;
                c->d.rn -= k;
                played += k;
                space -= k;
            }
        }
        i64 now = now_ms();
        if (now - last_pub >= 250) { atomic_store(&VM.pos_ms, (int)(played / 48)); last_pub = now; wake_signal(g_wake); }
        if (eof && !c->d.rn && !c->d.pqn) {     /* всё отдано: дать устройству доиграть ~120 мс */
            if (!idle_since) idle_since = now;
            if (now - idle_since > 150) break;
        }
        if (audio_lost(au)) { vm_err("устройство вывода отключено"); break; }
        if (curl_multi_poll(m, NULL, 0, 20, NULL) != CURLM_OK) plat_wait(NULL, 0, 20);
    }
    atomic_store(&VM.pos_ms, (int)(played / 48));
out:
    if (m && c->e) curl_multi_remove_handle(m, c->e);
    if (c->e) curl_easy_cleanup(c->e);
    if (m) curl_multi_cleanup(m);
    if (au) audio_close(au);
    vm_dec_free(&c->d);
    free(c);
    atomic_store(&VM.done, true);
    wake_signal(g_wake);
    return 0;
}
static void vm_join(void) {
    if (!VM.thr) return;
    atomic_store(&VM.stop, true);
    thread_join(VM.thr);
    VM.thr = NULL;
    wipe(VM.url, sizeof VM.url);
    wipe(VM.proxy, sizeof VM.proxy);
}
void vmsg_stop(void) {
    vm_join();
    if (!VM.mu) return;
    mutex_lock(VM.mu);
    VM.st.active = VM.st.loading = false;
    mutex_unlock(VM.mu);
    VM.demo_t0 = 0;
    ui_dirty();
}
bool vmsg_play(u64 mid) {
    if (!VM.mu) VM.mu = mutex_new();
    vmsg_stop();
    Message *m = msg_find(mid, NULL);
    mutex_lock(VM.mu);
    memset(&VM.st, 0, sizeof VM.st);
    VM.st.mid = mid;
    mutex_unlock(VM.mu);
    if (!m || !m->voice_secs) { vm_err("это не голосовое сообщение"); return false; }
    /* ссылка — из строки вложения (маркер ATT_MARK), только CDN Discord; просроченная — обновлённая */
    const char *c = m->content ? strchr(m->content, ATT_MARK) : NULL;
    const char *u = c ? strstr(c, "] https://") : NULL;
    char url[1100] = "";
    if (u) { u += 2; size_t k = 0; while (u[k] && u[k] != '\n' && u[k] != ' ' && k + 1 < sizeof url) { url[k] = u[k]; k++; } url[k] = 0; }
    if (!url[0] || !cdn_host_ok(url)) { vm_err("нет ссылки на голосовое"); return false; }
    if (cdn_expired(url)) {
        const char *f = cdn_fresh(url);
        if (!f) { cdn_want(url); vm_err("ссылка устарела — обновляется, нажмите ещё раз через пару секунд"); return false; }
        snprintf(url, sizeof url, "%s", f);
    }
    mutex_lock(VM.mu);
    VM.st.active = VM.st.loading = true;
    VM.st.dur_ms = m->voice_secs * 1000;
    mutex_unlock(VM.mu);
    atomic_store(&VM.pos_ms, 0);
    if (O.demo) { VM.demo_t0 = now_ms(); ui_dirty(); return true; }   /* демо: по часам, без сети и звука */
    VlessState vs = vless_state();
    const char *px = vs == VL_OFF ? "" : vless_proxy_url();
    if (!px) { vm_err("VLESS ещё не готов"); vmsg_stop(); return false; }
    snprintf(VM.url, sizeof VM.url, "%s", url);
    snprintf(VM.proxy, sizeof VM.proxy, "%s", px);
    { char hm[96]; dev_hints(hm, VM.out); }
    atomic_store(&VM.stop, false);
    atomic_store(&VM.done, false);
    VM.thr = thread_start(vm_thread, NULL);
    if (!VM.thr) { vm_err("не удалось запустить воспроизведение"); vmsg_stop(); return false; }
    ui_dirty();
    return true;
}
const VoiceMsgState *vmsg_state(void) {
    static VoiceMsgState out;
    if (!VM.mu) { memset(&out, 0, sizeof out); return &out; }
    if (VM.thr && atomic_load(&VM.done)) {      /* поток закончил — забрать и освободить */
        vm_join();
        mutex_lock(VM.mu);
        VM.st.active = VM.st.loading = false;
        mutex_unlock(VM.mu);
    }
    mutex_lock(VM.mu);
    out = VM.st;
    mutex_unlock(VM.mu);
    if (VM.demo_t0) {
        out.pos_ms = (int)(now_ms() - VM.demo_t0);
        out.loading = false;
        if (out.pos_ms >= out.dur_ms) { out.pos_ms = out.dur_ms; out.active = false; VM.demo_t0 = 0; mutex_lock(VM.mu); VM.st.active = VM.st.loading = false; mutex_unlock(VM.mu); }
    } else {
        out.pos_ms = atomic_load(&VM.pos_ms);
        if (out.pos_ms > 0 || !out.active) out.loading = false;   /* закончилось/остановлено — не «загрузка» */
    }
    return &out;
}
/* selftest: разбор Ogg/Opus без сети и звука — свой поток Ogg из закодированного тона */
static void vm_put_page(Buf *b, const u8 *segs, int ns, const u8 *body, size_t bl, u8 type) {
    u8 h[27] = { 'O', 'g', 'g', 'S', 0, type };
    h[26] = (u8)ns;
    buf_put(b, (const char *)h, 27);
    buf_put(b, (const char *)segs, (size_t)ns);
    buf_put(b, (const char *)body, bl);
}
static bool vm_selftest(long *samples, double *rms) {
    int e;
    OpusEncoder *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &e);
    if (e != OPUS_OK) return false;
    Buf ogg = {0};
    u8 head[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0 };   /* 1 канал, preskip 312 */
    u8 s1 = 19;
    vm_put_page(&ogg, &s1, 1, head, 19, 2);
    {   /* OpusTags на 10 000 байт (больше буфера пакета): не должен «съесть» первый звуковой пакет */
        static u8 tags[10000];
        memcpy(tags, "OpusTags", 8);
        u8 segs[40];
        int ns = 0;
        size_t left = sizeof tags;
        while (left >= 255) { segs[ns++] = 255; left -= 255; }
        segs[ns++] = (u8)left;
        vm_put_page(&ogg, segs, ns, tags, sizeof tags, 0);
    }
    /* 50 кадров по 20 мс в страницах по 10 пакетов */
    for (int pg = 0; pg < 5; pg++) {
        u8 segs[255], body[10 * 1275];
        int ns = 0;
        size_t bl = 0;
        for (int f = 0; f < 10; f++) {
            int16_t pcm[960];
            for (int i = 0; i < 960; i++) pcm[i] = (int16_t)(8000 * sin(2 * 3.14159265 * 440 * ((pg * 10 + f) * 960 + i) / 48000.0));
            int n = opus_encode(enc, pcm, 960, body + bl, 1275);
            if (n < 0) { opus_encoder_destroy(enc); buf_free(&ogg); return false; }
            int left = n;
            while (left >= 255) { segs[ns++] = 255; left -= 255; }
            segs[ns++] = (u8)left;
            bl += (size_t)n;
        }
        vm_put_page(&ogg, segs, ns, body, bl, 0);
    }
    opus_encoder_destroy(enc);
    VmDec *d = xcalloc(1, sizeof *d);
    bool ok = true;
    for (size_t i = 0; i < ogg.len && ok; i += 777) ok = vm_feed(d, ogg.p + i, ogg.len - i < 777 ? ogg.len - i : 777);   /* кусками, как из сети */
    long got = 0;
    double sum = 0;
    while (ok && (d->pqn || d->rn)) {
        vm_decode(d);
        for (; d->rn; d->rn--, d->rh = (d->rh + 1) % VM_RING) { double s = d->ring[d->rh]; sum += s * s; got++; }
    }
    *samples = got;
    *rms = got ? sqrt(sum / (double)got) : 0;
    vm_dec_free(d);
    free(d);
    buf_free(&ogg);
    return ok;
}
/* ---------- №145 звуки звонка (синтез) ---------- */
/* тон частоты f, длительность ms, громкость a (0..1) с мягкими краями 5 мс; t — сдвиг записи */
static int snd_tone(int16_t *o, int max, int at, double f, int ms, double a) {
    int n = 48 * ms;
    for (int i = 0; i < n && at + i < max; i++) {
        double env = 1.0;
        if (i < 240) env = i / 240.0;
        if (n - i < 240) env = (n - i) / 240.0;
        o[at + i] = (int16_t)(sin(2 * 3.14159265358979 * f * i / 48000.0) * a * env * 32767);
    }
    return at + n < max ? at + n : max;
}
static int snd_gap(int16_t *o, int max, int at, int ms) {
    int n = 48 * ms;
    for (int i = 0; i < n && at + i < max; i++) o[at + i] = 0;
    return at + n < max ? at + n : max;
}
int sound_synth(int which, int16_t *o, int max) {
    int k = 0;
    switch (which) {
    case SOUND_RING: k = snd_tone(o, max, k, 440, 350, 0.25); k = snd_tone(o, max, k, 554, 350, 0.25); k = snd_gap(o, max, k, 1300); break;
    case SOUND_JOIN: k = snd_tone(o, max, k, 523, 90, 0.2); k = snd_tone(o, max, k, 784, 120, 0.2); break;
    case SOUND_LEAVE: k = snd_tone(o, max, k, 784, 90, 0.2); k = snd_tone(o, max, k, 523, 120, 0.2); break;
    case SOUND_MUTE: k = snd_tone(o, max, k, 330, 60, 0.18); break;
    case SOUND_UNMUTE: k = snd_tone(o, max, k, 660, 60, 0.18); break;
    case SOUND_MSG: k = snd_tone(o, max, k, 880, 70, 0.15); k = snd_gap(o, max, k, 30); k = snd_tone(o, max, k, 1175, 90, 0.15); break;
    default: break;
    }
    return k;
}
static struct { PlatThread *thr; PlatWake *wk; atomic_bool stop; int which; bool off; } SN;
static int snd_thread(void *arg) {
    (void)arg;
    thread_audio_priority();
    char err[128] = "", hm[96], ho[96];
    dev_hints(hm, ho);
    Audio *au = g_audio_fake ? NULL : audio_open_out(ho[0] ? ho : NULL, err, sizeof err);
    int16_t *pcm = xmalloc(48000 * 3 * sizeof(int16_t)), st[960 * 2];
    int n = sound_synth(SN.which, pcm, 48000 * 3), pos = 0, loops = 0;
    while (au && !atomic_load(&SN.stop)) {
        int space = audio_render_space(au);
        while (space >= 960 && pos < n) {
            int k = n - pos < 960 ? n - pos : 960;
            for (int i = 0; i < k; i++) st[i * 2] = st[i * 2 + 1] = pcm[pos + i];
            audio_render_write(au, st, k);
            pos += k;
            space -= k;
        }
        if (pos >= n) {
            if (SN.which == SOUND_RING && ++loops < 15) pos = 0;   /* звонок — до 15 повторов (~30 с) или sound_stop */
            else { Waitable wt = wake_waitable(SN.wk); plat_wait(&wt, 1, 150); break; }   /* дать доиграть буфер */
        }
        Waitable wt = wake_waitable(SN.wk);
        plat_wait(&wt, 1, 20);
        wake_clear(SN.wk);
    }
    if (au) audio_close(au);
    free(pcm);
    return 0;
}
void sound_stop(void) {
    if (!SN.thr) return;
    atomic_store(&SN.stop, true);
    if (SN.wk) wake_signal(SN.wk);
    thread_join(SN.thr);
    SN.thr = NULL;
}
void sound_play(int which) {
    if (SN.off || which < SOUND_RING || which > SOUND_MSG) return;
    sound_stop();
    if (!SN.wk) SN.wk = wake_new();
    SN.which = which;
    atomic_store(&SN.stop, false);
    SN.thr = thread_start(snd_thread, NULL);
}
void sounds_enable(bool on) { SN.off = !on; if (!on) sound_stop(); }
bool sounds_enabled(void) { return !SN.off; }

void voice_shutdown(void) {
    sound_stop();
    if (SN.wk) { wake_free(SN.wk); SN.wk = NULL; }
    voice_mic_test(false);
    vmsg_stop();
    voice_leave();
    stop_thread();
}

/* ======================= самопроверка голосового тракта (без сети) ======================= */
int panic_selftest(void (*check)(bool ok, const char *what));   /* panic.c */

/* Тон 440 Гц через смену голоса: основная частота по пересечениям нуля, отношение уровней (RMS),
 * отсутствие скачков сильнее, чем позволяет крутизна синуса. Первые 200 мс — переход, не считаются. */
static void fx_measure(VoiceFx m, double *freq, double *level, bool *jumps_ok) {
    voicefx_set(m);
    int16_t fr[FRAME];
    static double ph;                            /* тон непрерывен между вызовами: скачок входа не в счёт */
    static int last;
    double ein = 0, eout = 0;
    long cross = 0;
    int sign = 0;
    const double A = 30000;
    for (int f = 0; f < 110; f++) {
        for (int i = 0; i < FRAME; i++) {
            fr[i] = (int16_t)(A * sin(ph));
            ph += 2 * 3.14159265358979 * 440 / 48000;
            if (f >= 10) ein += (double)fr[i] * fr[i];
        }
        voicefx_process(fr, FRAME);
        for (int i = 0; i < FRAME; i++) {
            int y = fr[i];
            /* крутизна синуса 550 Гц амплитуды A — 0,072*A на отсчёт; вдвое больше — щелчок */
            if (abs(y - last) > (int)(0.15 * A)) *jumps_ok = false;
            last = y;
            if (f < 10) continue;
            eout += (double)y * y;
            int s = y > 200 ? 1 : y < -200 ? -1 : 0;           /* с гистерезисом: шум у нуля не считается */
            if (s && s != sign) { if (sign) cross++; sign = s; }
        }
    }
    *freq = cross / 2.0 / (100 * 0.020);
    *level = ein > 0 ? sqrt(eout / ein) : 0;
}
int voice_selftest(void (*check)(bool ok, const char *what)) {
    {   /* №143: ФВЧ 90 Гц — 30 Гц гасит, 1 кГц пропускает */
        VW t = {0};
        int16_t a[4800], b[4800];
        for (int i = 0; i < 4800; i++) { a[i] = (int16_t)(10000 * sin(2 * 3.14159265 * 30 * i / 48000.0)); b[i] = (int16_t)(10000 * sin(2 * 3.14159265 * 1000 * i / 48000.0)); }
        hpf_run(&t, a, 4800);
        VW t2 = {0};
        hpf_run(&t2, b, 4800);
        double ea = 0, eb = 0;
        for (int i = 2400; i < 4800; i++) { ea += (double)a[i] * a[i]; eb += (double)b[i] * b[i]; }
        double ra = sqrt(ea / 2400), rb = sqrt(eb / 2400);
        char w[120];
        snprintf(w, sizeof w, "№143 ФВЧ 90 Гц: 30 Гц -> %.0f (из 7071), 1 кГц -> %.0f", ra, rb);
        check(ra < 1500 && rb > 6500, w);
    }
    {   /* №145: звуки — синтез без файлов, длительность и уровень */
        static int16_t buf[48000 * 3];
        int n1 = sound_synth(SOUND_RING, buf, 48000 * 3), n2 = sound_synth(SOUND_JOIN, buf, 48000 * 3), n3 = sound_synth(SOUND_MSG, buf, 48000 * 3);
        int pk = 0;
        for (int i = 0; i < n3; i++) if (abs(buf[i]) > pk) pk = abs(buf[i]);
        sound_play(SOUND_JOIN);                   /* в selftest звук фиктивный — устройство не открывается */
        sound_stop();
        sounds_enable(false);
        sound_play(SOUND_RING);
        bool off = !SN.thr;
        sounds_enable(true);
        check(n1 == 48 * 2000 && n2 == 48 * 210 && n3 == 48 * 190 && pk > 3000 && pk < 6000 && off, "№145 звуки звонка: синтез (звонок 2 с, вход, сообщение), громкость умеренная, выключение");
    }
    {   /* №69: поток Ogg/Opus кусками -> PCM (без сети и звука) */
        long n = 0;
        double r = 0;
        bool ok = vm_selftest(&n, &r);
        char w[160];
        snprintf(w, sizeof w, "№69 голосовые: Ogg/Opus кусками по 777 байт -> %ld сэмплов (ждали %d), уровень %.0f", n, 50 * 960 - 312, r);
        check(ok && n == 50 * 960 - 312 && r > 1000, w);
    }
    /* Opus: кодирование и декодирование кадра тона */
    int e1, e2;
    OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_VOIP, &e1);
    OpusDecoder *dec = opus_decoder_create(48000, 2, &e2);
    bool opus_ok = false;
    if (e1 == OPUS_OK && e2 == OPUS_OK) {
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
        int16_t pcm[FRAME * 2], out[FRAME * 2];
        unsigned char pkt[1275];
        int got = 0;
        for (int f = 0; f < 5; f++) {
            for (int i = 0; i < FRAME; i++) pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(8000 * sin(2 * 3.14159265 * 440 * (f * FRAME + i) / 48000.0));
            int n = opus_encode(enc, pcm, FRAME, pkt, sizeof pkt);
            got = n > 0 ? opus_decode(dec, pkt, n, out, FRAME, 0) : -1;
        }
        double energy = 0;
        for (int i = 0; i < FRAME * 2; i++) energy += (double)out[i] * out[i];
        opus_ok = got == FRAME && energy > 1e9;
    }
    if (enc) opus_encoder_destroy(enc);
    if (dec) opus_decoder_destroy(dec);
    check(opus_ok, "Opus: кадр 20 мс закодирован и декодирован");

    /* AES-256-GCM: расшифровка и отказ на подделке */
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    unsigned char key[32], iv[12] = { 0, 0, 0, 7 }, aad[12] = { 0x80, 0x78 }, msg[40], ct[40], pt[40], tag[16];
    for (int i = 0; i < 32; i++) key[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < 40; i++) msg[i] = (unsigned char)i;
    bool enc_ok = gcm(c, true, key, iv, aad, 12, msg, 40, ct, tag);
    bool dec_ok = enc_ok && gcm(c, false, key, iv, aad, 12, ct, 40, pt, tag) && !memcmp(pt, msg, 40);
    ct[5] ^= 1;
    bool tamper = enc_ok && !gcm(c, false, key, iv, aad, 12, ct, 40, pt, tag);
    EVP_CIPHER_CTX_free(c);
    check(dec_ok, "AES-256-GCM: пакет расшифрован");
    check(tamper, "AES-256-GCM: подделанный пакет отвергнут");

    /* DAVE: сессия MLS и пакет ключей */
    daveSetLogSinkCallback(dave_log_noop);
    DAVESessionHandle s = daveSessionCreate(NULL, "", dave_fail, NULL);
    size_t kpn = 0;
    if (s) {
        daveSessionInit(s, daveMaxSupportedProtocolVersion(), 123456789, "1000");
        uint8_t *kp = NULL;
        daveSessionGetMarshalledKeyPackage(s, &kp, &kpn);
        if (kp) daveFree(kp);
        daveSessionDestroy(s);
    }
    char b[320];
    snprintf(b, sizeof b, "DAVE v%u: сессия MLS создана, пакет ключей %zu байт", (unsigned)daveMaxSupportedProtocolVersion(), kpn);
    check(s && kpn > 0, b);

    /* звук: открыть и закрыть устройства (без воспроизведения) */
    char err[128] = "";
    Audio *a = vopen(O.mic, O.out, err, sizeof err);
    if (a) {
        Waitable w[8];
        int n = audio_waitables(a, w, 8);
        snprintf(b, sizeof b, "звук: устройства открыты (%d ожид. объектов)%s%s", n, err[0] ? "; " : "", err);
        audio_close(a);
        check(true, b);
    } else {
        snprintf(b, sizeof b, "звук: %s (на сервере без звуковой карты это нормально)", err[0] ? err : "нет устройств");
        check(true, b);
    }

    /* смена голоса: тон 440 Гц -> ~352 (ниже) / ~550 Гц (выше), уровень, отсутствие щелчков */
    VoiceFx was = voicefx_get();
    double f_lo = 0, f_hi = 0, lv_lo = 0, lv_hi = 0;
    bool jumps_ok = true;
    fx_measure(VFX_LOW, &f_lo, &lv_lo, &jumps_ok);
    fx_measure(VFX_HIGH, &f_hi, &lv_hi, &jumps_ok);
    double f_off = 0, lv_off = 0;
    fx_measure(VFX_OFF, &f_off, &lv_off, &jumps_ok);
    snprintf(b, sizeof b, "смена голоса: 440 Гц -> ниже %.0f Гц, выше %.0f Гц, выкл %.0f Гц; уровень %.0f%% / %.0f%%",
             f_lo, f_hi, f_off, lv_lo * 100, lv_hi * 100);
    check(f_lo > 330 && f_lo < 375 && f_hi > 520 && f_hi < 580 && f_off > 435 && f_off < 445 &&
          lv_lo > 0.5 && lv_hi > 0.5 && lv_off > 0.99 && lv_off < 1.01, b);
    check(jumps_ok, "смена голоса: без выбросов и щелчков (в т. ч. при переключении режимов)");
    /* стоимость: 3000 кадров (60 с звука) в режиме «выше» */
    {
        int16_t fr[FRAME];
        for (int i = 0; i < FRAME; i++) fr[i] = (int16_t)(8000 * sin(i * 0.0576));
        voicefx_set(VFX_HIGH);
        i64 t0 = now_ms();
        for (int f = 0; f < 3000; f++) { voicefx_process(fr, FRAME); fr[f % FRAME] ^= 1; }
        i64 dt = now_ms() - t0;
        voicefx_set(VFX_OFF);
        voicefx_process(fr, FRAME);
        i64 t1 = now_ms();
        for (int f = 0; f < 3000; f++) voicefx_process(fr, FRAME);
        i64 dt0 = now_ms() - t1;
        snprintf(b, sizeof b, "смена голоса: %.1f мкс на кадр 20 мс (%.3f%% ядра), выкл %.1f мкс",
                 dt * 1000.0 / 3000, dt * 1000.0 / 3000 / 20000 * 100, dt0 * 1000.0 / 3000);
        check(dt < 3000, b);                     /* < 1 мс на кадр с огромным запасом */
    }
    voicefx_set(was);

    /* SOCKS5 UDP: заголовок датаграммы и строгий разбор ответа реле */
    {
        Socks sx;
        socks_reset(&sx);
        sx.ready = true;
        sx.dst_ip[0] = 1; sx.dst_ip[1] = 2; sx.dst_ip[2] = 3; sx.dst_ip[3] = 4;
        sx.dst_port[0] = 0xC3; sx.dst_port[1] = 0x50;          /* 50000 */
        unsigned char p[64], q[64];
        int k = socks_wrap(&sx, "abc", 3, p, sizeof p);
        bool ok = k == 13 && !p[0] && !p[1] && !p[2] && p[3] == 1 && p[4] == 1 && p[7] == 4 && p[8] == 0xC3 && !memcmp(p + 10, "abc", 3);
        ok = ok && socks_unwrap(&sx, p, k) == 10;
        memcpy(q, p, (size_t)k); q[2] = 1;                       /* фрагмент */
        ok = ok && socks_unwrap(&sx, q, k) < 0;
        memcpy(q, p, (size_t)k); q[9] ^= 1;                      /* чужой порт */
        ok = ok && socks_unwrap(&sx, q, k) < 0;
        memcpy(q, p, (size_t)k); q[6] ^= 1;                      /* чужой адрес */
        ok = ok && socks_unwrap(&sx, q, k) < 0;
        memcpy(q, p, (size_t)k); q[0] = 1;                       /* RSV */
        ok = ok && socks_unwrap(&sx, q, k) < 0;
        ok = ok && socks_unwrap(&sx, p, 9) < 0;                  /* обрезан */
        memcpy(q, p, (size_t)k); q[3] = 3; q[4] = 200;           /* домен длиннее пакета */
        ok = ok && socks_unwrap(&sx, q, k) < 0;
        memcpy(q, p, (size_t)k); q[3] = 9;                       /* неизвестный тип адреса */
        ok = ok && socks_unwrap(&sx, q, k) < 0;
        ok = ok && socks_wrap(&sx, p, 60, q, sizeof q) < 0;     /* не влезает */
        sx.ready = false;
        ok = ok && socks_wrap(&sx, "abc", 3, q, sizeof q) < 0 && socks_unwrap(&sx, p, k) < 0;
        check(ok, "SOCKS5 UDP: заголовок, отказ на фрагментах, чужом источнике и обрезанных пакетах");
    }

    panic_selftest(check);
    return 0;
}
