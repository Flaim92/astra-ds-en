/* golive.c — «Astra DS Video»: трансляция экрана в голосовой канал (Discord Go Live). Только при -DASTRA_VIDEO.
 *
 * Шаг 1 (этот файл): шлюз и состояние потока, RTP-пакетизация H.264 и вход кадров кодера.
 *   Шлюз: op 18 STREAM_CREATE {type:"guild", guild_id, channel_id, preferred_region:null} -> событие STREAM_CREATE
 *   {stream_key, rtc_server_id} и STREAM_SERVER_UPDATE {stream_key, endpoint, token}; op 22 STREAM_SET_PAUSED
 *   {stream_key, paused}; op 19 STREAM_DELETE {stream_key}; событие STREAM_DELETE {stream_key, reason}.
 *   stream_key: "guild:<gid>:<cid>:<uid>" (личный звонок — "call:<cid>:<uid>").
 *   H.264 (RFC 6184): NAL <= MTU — одиночный пакет; больше — FU-A (индикатор F|NRI|28, заголовок S|E|тип);
 *   метка (marker) — на последнем пакете кадра. Отметка времени — 90 кГц.
 * Зритель (этап 2): сборка кадра из RTP (h264_asm_*), см. golive.h; приём, DAVE, поток декодера — voice_stream.c; декодеры — viddec.c.
 * Шаг 2 (дальше): отдельное голосовое соединение для потока (server_id = rtc_server_id, тот же session_id,
 *   op 12 с видеопотоком screen, кодек H264 PT 101 / RTX 102), DAVE для видео (libdave, кодек H264: заголовки NAL
 *   открыты), отправка пакетов из golive_frame. Живая проверка — только через координатора. */
#ifdef ASTRA_VIDEO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "plat.h"
#include "golive.h"
#include "share.h"

static GoLiveState GL;
static GoWatchState GW;                         /* просмотр чужой трансляции */
static char gw_key[96], gw_rs[32];
static u64 gw_guild, gw_chan;                   /* что смотрим — для переподключения с широким набором кодеков */
static bool gw_wide;                            /* этот просмотр — с широким набором */
static char gw_wide_key[96];                    /* ключ, для которого откат уже был: снова не откатываемся, сразу широкий */
static PlatMutex *gl_mu;
static void gl_lock(void) { if (!gl_mu) gl_mu = mutex_new(); mutex_lock(gl_mu); }
static void gl_unlock(void) { mutex_unlock(gl_mu); }

void golive_state(GoLiveState *out) { gl_lock(); *out = GL; gl_unlock(); }

static bool key_ok(const char *k) {             /* только наш формат: буквы, цифры, двоеточия */
    if (!k || !*k || strlen(k) >= sizeof GL.key) return false;
    for (const char *p = k; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || *p == ':')) return false;
    return true;
}

bool golive_start(u64 guild_id, u64 channel_id, char *err, size_t errn) {
    if (!channel_id) { snprintf(err, errn, "%s", ui_tr("трансляция: сначала войдите в голосовой канал")); return false; }
    gl_lock();
    bool busy = GL.phase != GL_OFF && GL.phase != GL_FAILED;
    gl_unlock();
    if (busy) { snprintf(err, errn, "%s", ui_tr("трансляция уже идёт")); return false; }
    char b[256];
    if (guild_id) snprintf(b, sizeof b, "{\"op\":18,\"d\":{\"type\":\"guild\",\"guild_id\":\"%llu\",\"channel_id\":\"%llu\",\"preferred_region\":null}}",
                           (unsigned long long)guild_id, (unsigned long long)channel_id);
    else snprintf(b, sizeof b, "{\"op\":18,\"d\":{\"type\":\"call\",\"channel_id\":\"%llu\",\"preferred_region\":null}}", (unsigned long long)channel_id);
    gl_lock();
    memset(&GL, 0, sizeof GL);
    GL.phase = GL_CREATING;
    GL.guild_id = guild_id;
    GL.channel_id = channel_id;
    if (guild_id) snprintf(GL.key, sizeof GL.key, "guild:%llu:%llu:%llu", (unsigned long long)guild_id, (unsigned long long)channel_id, (unsigned long long)S.me_id);
    else snprintf(GL.key, sizeof GL.key, "call:%llu:%llu", (unsigned long long)channel_id, (unsigned long long)S.me_id);
    gl_unlock();
    if (!O.demo) gw_send(b);
    return true;
}

void golive_stop(void) {
    char b[160] = "";
    gl_lock();
    if (GL.phase != GL_OFF && key_ok(GL.key)) snprintf(b, sizeof b, "{\"op\":19,\"d\":{\"stream_key\":\"%s\"}}", GL.key);
    wipe(&GL, sizeof GL);                       /* там токен сервера трансляции */
    gl_unlock();
    voice_stream_stop();
    if (b[0] && !O.demo) gw_send(b);
}

void golive_pause(bool paused) {
    char b[160] = "";
    gl_lock();
    if (GL.phase != GL_OFF && key_ok(GL.key)) {
        snprintf(b, sizeof b, "{\"op\":22,\"d\":{\"stream_key\":\"%s\",\"paused\":%s}}", GL.key, paused ? "true" : "false");
        GL.paused = paused;
    }
    gl_unlock();
    if (b[0] && !O.demo) gw_send(b);
}

/* события шлюза (store.c, только под ASTRA_VIDEO); чужие ключи — мимо */
void golive_on_event(const char *t, const cJSON *d) {
    const char *k = jstr(d, "stream_key");
    if (!key_ok(k)) return;
    bool connect = false, drop = false;
    char c_ep[128] = "", c_tok[64] = "", c_rs[32] = "";
    u64 ch = 0;
    gl_lock();
    if (gw_key[0] && !strcmp(k, gw_key)) {      /* трансляция, которую смотрим */
        bool wc = false, wd = false;
        char ep[128] = "", tk[64] = "", rs[32] = "";
        u64 author = GW.user;
        bool wide = gw_wide;
        if (!strcmp(t, "STREAM_CREATE")) {
            const char *r = jstr(d, "rtc_server_id");
            if (r && strlen(r) < sizeof gw_rs) snprintf(gw_rs, sizeof gw_rs, "%s", r);
        } else if (!strcmp(t, "STREAM_SERVER_UPDATE")) {
            const char *e = jstr(d, "endpoint"), *o = jstr(d, "token");
            if (e && o && strlen(e) < sizeof ep && strlen(o) < sizeof tk && gw_rs[0]) {
                snprintf(ep, sizeof ep, "%s", e); snprintf(tk, sizeof tk, "%s", o); snprintf(rs, sizeof rs, "%s", gw_rs);
                GW.phase = GW_CONNECTING;
                wc = true;
            }
        } else if (!strcmp(t, "STREAM_DELETE")) {
            const char *r = jstr(d, "reason");   /* причины Discord: stream_ended, unauthorized, stream_full */
            GW.phase = GW_FAILED;
            snprintf(GW.err, sizeof GW.err, "%s", r && !strcmp(r, "unauthorized") ? ui_tr("нет доступа к трансляции")
                     : r && !strcmp(r, "stream_full") ? ui_tr("трансляция переполнена") : ui_tr("трансляция завершена"));
            wd = true;
        }
        gl_unlock();
        if (wd) voice_watch_stop();
        if (wc) {
            char err[128] = "";
            if (!voice_watch_start(ep, tk, rs, author, wide, err, sizeof err) && err[0]) golive_wstatus(VS_FAILED, err);
            wipe(tk, sizeof tk);
        }
        ui_dirty();
        return;
    }
    bool mine = !strcmp(k, GL.key);
    if (mine && !strcmp(t, "STREAM_CREATE")) {
        const char *rs = jstr(d, "rtc_server_id");
        if (rs && strlen(rs) < sizeof GL.rtc_server_id) snprintf(GL.rtc_server_id, sizeof GL.rtc_server_id, "%s", rs);
        if (GL.phase == GL_CREATING) GL.phase = GL_WAIT_SERVER;
    } else if (mine && !strcmp(t, "STREAM_SERVER_UPDATE")) {
        const char *ep = jstr(d, "endpoint"), *tok = jstr(d, "token");
        if (ep && tok && strlen(ep) < sizeof GL.endpoint && strlen(tok) < sizeof GL.token) {
            snprintf(GL.endpoint, sizeof GL.endpoint, "%s", ep);
            snprintf(GL.token, sizeof GL.token, "%s", tok);
            GL.phase = GL_CONNECTING;           /* шаг 2: голосовое соединение потока */
            connect = true;
        }
    } else if (mine && !strcmp(t, "STREAM_DELETE")) {
        const char *r = jstr(d, "reason");
        GL.phase = GL_FAILED;
        snprintf(GL.err, sizeof GL.err, "%s%s", ui_tr("трансляция остановлена"), r && !strcmp(r, "user_requested") ? "" : ui_tr(" сервером"));
        drop = true;
    }
    if (connect) {
        snprintf(c_ep, sizeof c_ep, "%s", GL.endpoint);
        snprintf(c_tok, sizeof c_tok, "%s", GL.token);
        snprintf(c_rs, sizeof c_rs, "%s", GL.rtc_server_id);
        ch = GL.channel_id;
    }
    gl_unlock();
    if (drop) voice_stream_stop();
    if (connect) {
        char err[128] = "";
        if (!voice_stream_start(c_ep, c_tok, c_rs, ch, err, sizeof err) && err[0]) golive_vstatus(VS_FAILED, err);
        wipe(c_tok, sizeof c_tok);
    }
    if (mine) ui_dirty();
}
/* ---------- просмотр чужой трансляции ---------- */
static void gw_make_key(char *k, size_t n, u64 guild_id, u64 channel_id, u64 user_id) {
    if (guild_id) snprintf(k, n, "guild:%llu:%llu:%llu", (unsigned long long)guild_id, (unsigned long long)channel_id, (unsigned long long)user_id);
    else snprintf(k, n, "call:%llu:%llu", (unsigned long long)channel_id, (unsigned long long)user_id);
}
static bool gw_watch(u64 guild_id, u64 channel_id, u64 user_id, bool wide, char *err, size_t errn) {
    if (!channel_id || !user_id) { snprintf(err, errn, "%s", ui_tr("нечего смотреть")); return false; }
    if (user_id == S.me_id) { snprintf(err, errn, "%s", ui_tr("это ваша трансляция")); return false; }
    golive_unwatch();
    char k[96], b[160];
    gw_make_key(k, sizeof k, guild_id, channel_id, user_id);
    snprintf(b, sizeof b, "{\"op\":20,\"d\":{\"stream_key\":\"%s\"}}", k);
    gl_lock();
    memset(&GW, 0, sizeof GW);
    GW.phase = GW_WAIT;
    GW.user = user_id;
    snprintf(gw_key, sizeof gw_key, "%s", k);
    gw_rs[0] = 0;
    gw_guild = guild_id; gw_chan = channel_id;
    gw_wide = wide || !strcmp(k, gw_wide_key);   /* для этой трансляции откат уже был — сразу широкий */
    gl_unlock();
    if (!O.demo) gw_send(b);
    return true;
}
/* смотреть: дешёвый набор кодеков (другой автор — снова дешёвый; та же трансляция после отката — широкий) */
bool golive_watch(u64 guild_id, u64 channel_id, u64 user_id, char *err, size_t errn) { return gw_watch(guild_id, channel_id, user_id, false, err, errn); }
void golive_unwatch(void) {
    char b[160] = "";
    gl_lock();
    if (GW.phase != GW_OFF && key_ok(gw_key)) snprintf(b, sizeof b, "{\"op\":19,\"d\":{\"stream_key\":\"%s\"}}", gw_key);
    memset(&GW, 0, sizeof GW);
    gw_key[0] = gw_rs[0] = 0;
    gl_unlock();
    voice_watch_stop();
    if (b[0] && !O.demo) gw_send(b);
}
void golive_wstate(GoWatchState *out) {
    if (voice_watch_take_wide()) {               /* поток зрителя: в дешёвом наборе видео нет — один раз переподключиться */
        gl_lock();
        u64 g = gw_guild, c = gw_chan, u = GW.user;
        bool can = GW.phase != GW_OFF && !gw_wide && u;
        if (can) snprintf(gw_wide_key, sizeof gw_wide_key, "%s", gw_key);
        gl_unlock();
        char e[96];
        if (can) gw_watch(g, c, u, true, e, sizeof e);   /* op 19 + op 20 тем же ключом; панель — «подключение...» */
    }
    gl_lock(); *out = GW; gl_unlock();
    voice_watch_counts(&out->apkts, &out->vpkts);
    if (out->phase == GW_OFF) return;
    voice_watch_vstats(&out->v);
    if (out->v.unsupported && out->phase == GW_LIVE && !out->err[0])
        snprintf(out->err, sizeof out->err, "%s", ui_tr("формат трансляции не поддерживается (нужен H.264 Baseline)"));
}
void golive_wstatus(int ph, const char *err) {
    gl_lock();
    if (GW.phase != GW_OFF && GW.phase != GW_FAILED) {
        if (ph == VS_CONNECTED) GW.phase = GW_LIVE;
        else if (ph == VS_FAILED) { GW.phase = GW_FAILED; snprintf(GW.err, sizeof GW.err, "%s", err ? err : ui_tr("просмотр не удался")); }
    }
    gl_unlock();
    if (g_wake) wake_signal(g_wake);             /* зовётся и из потока зрителя: U.dirty не трогаем, ui_tick сверит фазу */
}
/* состояние соединения трансляции (из его потока) */
void golive_vstatus(int ph, const char *err) {
    gl_lock();
    if (GL.phase != GL_OFF) {
        if (ph == VS_CONNECTED) { GL.phase = GL_LIVE; GL.waiting = false; share_request_idr(); }   /* зрителю — сразу ключевой кадр */
        else if (ph == VS_DAVE && GL.phase == GL_CONNECTING) GL.waiting = true;   /* группа DAVE создаётся с первым зрителем */
        else if (ph == VS_FAILED) { GL.phase = GL_FAILED; snprintf(GL.err, sizeof GL.err, "%s", err ? err : ui_tr("трансляция не удалась")); }
    }
    gl_unlock();
    ui_dirty();
}
bool golive_paused(void) { gl_lock(); bool p = GL.paused; gl_unlock(); return p; }

/* ---------- H.264 -> RTP (RFC 6184) ---------- */
/* следующий NAL в Annex B: старт-код 00 00 01 или 00 00 00 01; nal и nn — без старт-кода */
static bool next_nal(const u8 *b, size_t n, size_t *pos, const u8 **nal, size_t *nn) {
    size_t i = *pos;
    while (i + 3 <= n && !(b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)) i++;
    if (i + 3 > n) return false;
    size_t s = i + 3, e = s;
    while (e + 3 <= n && !(b[e] == 0 && b[e + 1] == 0 && (b[e + 2] == 1 || (b[e + 2] == 0 && e + 3 < n && b[e + 3] == 1)))) e++;
    if (e + 3 > n) e = n;
    *nal = b + s;
    *nn = e - s;
    *pos = e;
    return *nn > 0;
}
int h264_packetize(const u8 *annexb, size_t n, size_t mtu, void (*emit)(void *u, const u8 *p, size_t len, bool marker), void *u) {
    if (mtu < 16 || mtu > 1400) return -1;
    u8 pkt[1400];
    const u8 *nal, *nals[64];
    size_t nn, lens[64], pos = 0;
    int cnt = 0, out = 0;
    while (cnt < 64 && next_nal(annexb, n, &pos, &nal, &nn)) { nals[cnt] = nal; lens[cnt] = nn; cnt++; }
    for (int i = 0; i < cnt; i++) {
        /* STAP-A (тип 24), как у WebRTC-отправителя в packetization-mode 1: подряд идущие малые NAL (SPS, PPS, ...)
         * одним пакетом, пока помещаются: [F|NRI|24] + {размер 2 байта + NAL}... */
        int j = i;
        size_t agg = 1;
        u8 nri = 0;
        while (j < cnt && agg + 2 + lens[j] <= mtu) { agg += 2 + lens[j]; if ((nals[j][0] & 0x60) > nri) nri = nals[j][0] & 0x60; j++; }
        if (j - i >= 2) {
            size_t k = 0;
            pkt[k++] = (u8)(nri | 24);
            for (int q = i; q < j; q++) {
                pkt[k++] = (u8)(lens[q] >> 8); pkt[k++] = (u8)lens[q];
                memcpy(pkt + k, nals[q], lens[q]);
                k += lens[q];
            }
            emit(u, pkt, k, j == cnt);
            out++;
            i = j - 1;
            continue;
        }
        bool last = i == cnt - 1;
        nal = nals[i];
        nn = lens[i];
        if (nn <= mtu) { emit(u, nal, nn, last); out++; continue; }
        u8 hdr = nal[0];
        size_t off = 1;                         /* байт заголовка NAL переходит в индикатор/заголовок FU */
        while (off < nn) {
            size_t chunk = nn - off < mtu - 2 ? nn - off : mtu - 2;
            pkt[0] = (u8)((hdr & 0xE0) | 28);
            pkt[1] = (u8)((off == 1 ? 0x80 : 0) | (off + chunk == nn ? 0x40 : 0) | (hdr & 0x1F));
            memcpy(pkt + 2, nal + off, chunk);
            off += chunk;
            emit(u, pkt, chunk + 2, last && off == nn);
            out++;
        }
    }
    return out;
}

/* кадр кодера (Annex B) — от дизайнера (share_*): пока поток не подключён, только счёт */
void golive_frame(const u8 *annexb, size_t n, bool keyframe, u32 ts90k) {
    gl_lock();
    bool on = GL.phase == GL_LIVE && !GL.paused;
    if (on) { GL.frames++; GL.bytes += (long long)n; if (keyframe) GL.keyframes++; }
    else GL.dropped++;
    gl_unlock();
    if (on) voice_stream_frame(annexb, n, keyframe, ts90k);
}

/* ---------- RTP -> H.264 (RFC 6184): сборка кадра у зрителя ---------- */
#define ASM_MAXPK 4096                          /* пакетов в кадре (IDR 1080p ~ 500 КБ / 1100 Б ~ 450) */
typedef struct { u16 seq; u8 fl; u32 off, len; } AsmPk;   /* fl: 1 — начало кадра, 2 — ключевой (VP8/VP9) */
struct H264Asm {
    size_t max;                                 /* предел кадра, Б */
    u8 *pk; size_t pkn, pkcap;                  /* нагрузки пакетов текущего кадра подряд */
    AsmPk *ix; int nix, ixcap;
    bool cur, broken, have_mark; u32 ts; u16 mark_seq;
    bool have_end; u16 end_seq;                 /* последний номер прошлого кадра (граница) */
    u8 *out; size_t outn, outcap; bool idr, sps;
    int codec;                                  /* VC_* */
    u8 *tmp; size_t tmpcap;                     /* AV1: склейка частей OBU */
};
H264Asm *vid_asm_new(int codec, size_t max_frame) {
    if (codec < 0 || codec >= VC_N) return NULL;
    H264Asm *a = calloc(1, sizeof *a);
    if (a) { a->max = max_frame ? max_frame : (2u << 20); a->codec = codec; }
    return a;
}
H264Asm *h264_asm_new(size_t max_frame) { return vid_asm_new(VC_H264, max_frame); }
int vid_asm_codec(const H264Asm *a) { return a ? a->codec : -1; }
static void asm_wipe_free(void *p, size_t n) { if (p) { wipe(p, n); free(p); } }
void h264_asm_free(H264Asm *a) {
    if (!a) return;
    asm_wipe_free(a->pk, a->pkcap);
    asm_wipe_free(a->ix, (size_t)a->ixcap * sizeof *a->ix);
    asm_wipe_free(a->out, a->outcap);
    asm_wipe_free(a->tmp, a->tmpcap);
    wipe(a, sizeof *a);
    free(a);
}
void h264_asm_reset(H264Asm *a) { if (a) { a->cur = a->broken = a->have_mark = a->have_end = false; a->nix = 0; a->pkn = a->outn = 0; } }
size_t h264_asm_bytes(const H264Asm *a) { return a ? sizeof *a + a->pkcap + (size_t)a->ixcap * sizeof *a->ix + a->outcap + a->tmpcap : 0; }
const u8 *h264_asm_frame(const H264Asm *a, size_t *n, bool *idr, bool *sps) {
    *n = a->outn;
    if (idr) *idr = a->idr;
    if (sps) *sps = a->sps;
    return a->out;
}
/* рост буфера без realloc: старое содержимое затирается (это чужой экран) */
static bool asm_grow(u8 **b, size_t *cap, size_t need, size_t lim) {
    if (need <= *cap) return true;
    if (need > lim) return false;
    size_t nc = *cap ? *cap : 65536;
    while (nc < need) nc *= 2;
    if (nc > lim) nc = lim;
    u8 *nb = malloc(nc);
    if (!nb) return false;
    if (*b) { memcpy(nb, *b, *cap); wipe(*b, *cap); free(*b); }
    *b = nb;
    *cap = nc;
    return true;
}
static bool asm_put(H264Asm *a, const u8 *p, size_t n) {
    if (!asm_grow(&a->out, &a->outcap, a->outn + n, a->max + a->max / 2 + 4096)) return false;
    memcpy(a->out + a->outn, p, n);
    a->outn += n;
    return true;
}
static bool asm_nal(H264Asm *a, const u8 *p, size_t n) {   /* старт-код + NAL целиком */
    if (!n) return false;
    if (a->codec == VC_H265) {                  /* IRAP (16..23) — ключевой; VPS/SPS (32, 33) — параметры */
        int t = (p[0] >> 1) & 0x3F;
        if (t >= 16 && t <= 23) a->idr = true;
        if (t == 32 || t == 33) a->sps = true;
        return asm_put(a, (const u8 *)"\0\0\0\1", 4) && asm_put(a, p, n);
    }
    int t = p[0] & 31;
    if (t == 5) a->idr = true;
    if (t == 7) a->sps = true;
    return asm_put(a, (const u8 *)"\0\0\0\1", 4) && asm_put(a, p, n);
}
/* пакеты first..mark_seq (все на месте) -> Annex B; false — кадр битый */
/* ---------- кодеки зрителя ---------- */
static const struct { const char *name; int pt; } VC[VC_N] = { { "H264", 105 }, { "VP8", 107 }, { "H265", 103 }, { "VP9", 109 }, { "AV1", 101 } };
int vc_pt(int c) { return c >= 0 && c < VC_N ? VC[c].pt : -1; }
int vc_of_pt(int pt) { for (int i = 0; i < VC_N; i++) if (VC[i].pt == pt) return i; return -1; }
int vc_of_name(const char *n) { for (int i = 0; n && i < VC_N; i++) if (!strcmp(VC[i].name, n)) return i; return -1; }
const char *vc_name(int c) { return c >= 0 && c < VC_N ? VC[c].name : "?"; }

/* VP8 (RFC 7741 4.2): X R N S R PID | [I L T K] | [M PictureID 7/15] | [TL0PICIDX] | [TID Y KEYIDX].
 * Возврат — смещение нагрузки, -1 — битый; *fl: бит 0 — начало кадра (S=1, PID=0), бит 1 — ключевой. */
static int vp8_desc(const u8 *p, size_t n, u8 *fl) {
    if (n < 1) return -1;
    size_t k = 1;
    if (p[0] & 0x80) {
        if (n < 2) return -1;
        u8 x = p[1];
        k = 2;
        if (x & 0x80) { if (k >= n) return -1; k += (p[k] & 0x80) ? 2 : 1; }
        if (x & 0x40) k++;
        if (x & 0x30) k++;
    }
    if (k >= n) return -1;
    bool start = (p[0] & 0x10) && !(p[0] & 0x07);
    *fl = (u8)((start ? 1 : 0) | (start && !(p[k] & 1) ? 2 : 0));   /* P (инверсный ключ) — бит 0 первого байта кадра */
    return (int)k;
}
/* VP9 (RFC 9628 4.2): I P L F B E V Z | [M PictureID] | [TID U SID D] [TL0PICIDX если !F] | [P_DIFF..3] | [SS] */
static int vp9_desc(const u8 *p, size_t n, u8 *fl) {
    if (n < 1) return -1;
    u8 b = p[0];
    size_t k = 1;
    if (b & 0x80) { if (k >= n) return -1; k += (p[k] & 0x80) ? 2 : 1; }
    if (b & 0x20) { k++; if (!(b & 0x10)) k++; }
    if ((b & 0x10) && (b & 0x40)) {
        for (int i = 0; i < 3; i++) { if (k >= n) return -1; u8 d = p[k++]; if (!(d & 1)) break; }
    }
    if (b & 0x02) {                              /* scalability structure */
        if (k >= n) return -1;
        u8 ss = p[k++];
        int ns = (ss >> 5) + 1;
        if (ss & 0x10) k += (size_t)ns * 4;
        if (ss & 0x08) {
            if (k >= n) return -1;
            int ng = p[k++];
            for (int i = 0; i < ng; i++) { if (k >= n) return -1; u8 t = p[k++]; k += (t >> 2) & 3; }
        }
    }
    if (k >= n) return -1;
    bool start = b & 0x08;
    *fl = (u8)((start ? 1 : 0) | (start && !(b & 0x40) ? 2 : 0));   /* P = 0 — без межкадрового предсказания */
    return (int)k;
}
/* H265: AP (48) {размер 2 Б, NAL}..., FU (49) [заголовок 2 Б][S E тип] */
static bool asm_build_h265(H264Asm *a) {
    bool in_fu = false;
    int fu_type = 0;
    for (int i = 0; i < a->nix; i++) {
        const u8 *p = a->pk + a->ix[i].off;
        size_t n = a->ix[i].len;
        if (!n) continue;
        if (n < 3) return false;
        int t = (p[0] >> 1) & 0x3F;
        if (t < 48) {
            if (in_fu || !asm_nal(a, p, n)) return false;
        } else if (t == 48) {
            if (in_fu) return false;
            size_t k = 2;
            while (k < n) {
                if (k + 2 > n) return false;
                size_t sz = (size_t)p[k] << 8 | p[k + 1];
                k += 2;
                if (sz < 2 || k + sz > n || !asm_nal(a, p + k, sz)) return false;
                k += sz;
            }
        } else if (t == 49) {
            bool st = p[2] & 0x80, en = p[2] & 0x40;
            int ft = p[2] & 0x3F;
            if (st) {
                if (in_fu || ft >= 48) return false;
                u8 h[2] = { (u8)((p[0] & 0x81) | (ft << 1)), p[1] };
                if (!asm_nal(a, h, 2) || !asm_put(a, p + 3, n - 3)) return false;
                in_fu = true;
                fu_type = ft;
            } else if (!in_fu || ft != fu_type || !asm_put(a, p + 3, n - 3)) return false;
            if (en) in_fu = false;
        } else return false;                    /* PACI (50) и прочее */
    }
    return !in_fu && a->outn > 0;
}
/* VP8/VP9: нагрузки подряд (описатели уже сняты); первый пакет обязан начинать кадр */
static bool asm_build_vpx(H264Asm *a) {
    if (!a->nix || !(a->ix[0].fl & 1)) return false;
    a->idr = a->sps = (a->ix[0].fl & 2) != 0;
    for (int i = 0; i < a->nix; i++) if (a->ix[i].len && !asm_put(a, a->pk + a->ix[i].off, a->ix[i].len)) return false;
    return a->outn > 0;
}
static size_t leb128_put(u8 *o, size_t v) { size_t k = 0; do { u8 b = v & 0x7F; v >>= 7; o[k++] = (u8)(b | (v ? 0x80 : 0)); } while (v); return k; }
static bool leb128_get(const u8 *p, size_t n, size_t *pos, size_t *v) {
    size_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (*pos >= n) return false;
        u8 b = p[(*pos)++];
        r |= (size_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *v = r; return true; }
    }
    return false;
}
/* AV1: [Z Y W W N - - -] + элементы OBU (W = 0 — у каждого leb128-длина; иначе W элементов, у последнего — без).
 * Части OBU склеиваются в tmp, затем OBU пишутся с полем размера (кроме последнего), TD/tile list/padding — мимо. */
static bool asm_build_av1(H264Asm *a) {
    size_t tn = 0;
    int nobu = 0;
    size_t obu_off[256], obu_len[256];
    bool open = false;
    for (int i = 0; i < a->nix; i++) {
        const u8 *p = a->pk + a->ix[i].off;
        size_t n = a->ix[i].len;
        if (!n) continue;
        u8 ah = p[0];
        bool z = ah & 0x80, y = ah & 0x40;
        int wc = (ah >> 4) & 3;
        if (ah & 0x08) a->sps = true;            /* N: начало новой видеопоследовательности */
        if (z != open) return false;             /* продолжение без начала или начало без конца */
        size_t pos = 1;
        for (int e = 0; pos < n; e++) {
            size_t len;
            if (wc == 0 || e + 1 < wc) { if (!leb128_get(p, n, &pos, &len) || len > n - pos) return false; }
            else len = n - pos;
            bool cont = e == 0 && z;
            if (!cont) {
                if (nobu == 256) return false;
                obu_off[nobu] = tn; obu_len[nobu] = 0; nobu++;
            }
            if (!asm_grow(&a->tmp, &a->tmpcap, tn + len, a->max)) return false;
            memcpy(a->tmp + tn, p + pos, len);
            tn += len;
            obu_len[nobu - 1] += len;
            pos += len;
            if (wc && e + 1 == wc) break;
        }
        open = y;
    }
    if (open || !nobu) return false;
    int last = -1;
    for (int i = 0; i < nobu; i++) {
        if (!obu_len[i]) return false;
        int t = (a->tmp[obu_off[i]] >> 3) & 15;
        if (t != 2 && t != 8 && t != 15) last = i;
    }
    for (int i = 0; i < nobu; i++) {
        const u8 *o = a->tmp + obu_off[i];
        size_t on = obu_len[i];
        u8 h = o[0];
        int t = (h >> 3) & 15;
        if (t == 2 || t == 8 || t == 15) continue;   /* temporal delimiter, tile list, padding */
        if (t == 1) a->idr = a->sps = true;      /* заголовок последовательности — у ключевых кадров */
        size_t hl = (h & 4) ? 2 : 1;
        if (on < hl) return false;
        size_t pl = on - hl, ps = hl;
        if (h & 2) { if (!leb128_get(o, on, &ps, &pl) || pl > on - ps) return false; }   /* размер уже был */
        u8 hb[2] = { (u8)(i == last ? h & ~2 : h | 2), hl == 2 ? o[1] : 0 }, lb[10];
        if (!asm_put(a, hb, hl)) return false;
        if (i != last && !asm_put(a, lb, leb128_put(lb, pl))) return false;
        if (!asm_put(a, o + ps, pl)) return false;
    }
    return a->outn > 0;
}
static bool asm_build_h264(H264Asm *a) {
    bool in_fu = false;
    int fu_type = 0;
    for (int i = 0; i < a->nix; i++) {
        const u8 *p = a->pk + a->ix[i].off;
        size_t n = a->ix[i].len;
        if (!n) continue;                       /* пустой (заполнитель номера) */
        int t = p[0] & 31;
        if (t >= 1 && t <= 23) {
            if (in_fu || !asm_nal(a, p, n)) return false;
        } else if (t == 24) {                   /* STAP-A: [F|NRI|24] {размер 2 Б, NAL}... */
            if (in_fu) return false;
            size_t k = 1;
            while (k < n) {
                if (k + 2 > n) return false;
                size_t sz = (size_t)p[k] << 8 | p[k + 1];
                k += 2;
                if (!sz || k + sz > n || !asm_nal(a, p + k, sz)) return false;
                k += sz;
            }
        } else if (t == 28) {                   /* FU-A: [F|NRI|28] [S|E|R|тип] данные */
            if (n < 2) return false;
            bool st = p[1] & 0x80, en = p[1] & 0x40;
            int ft = p[1] & 31;
            if (st) {
                if (in_fu || ft == 0 || ft > 23) return false;
                u8 h = (u8)((p[0] & 0xE0) | ft);
                if (!asm_nal(a, &h, 1) || !asm_put(a, p + 2, n - 2)) return false;
                in_fu = true;
                fu_type = ft;
            } else {
                if (!in_fu || ft != fu_type || !asm_put(a, p + 2, n - 2)) return false;
            }
            if (en) in_fu = false;
        } else return false;                    /* STAP-B, MTAP, FU-B, 0, 30, 31 — не поддерживаем */
    }
    return !in_fu && a->outn > 0;
}
static bool asm_build(H264Asm *a, u16 first) {
    for (int i = 1; i < a->nix; i++) {          /* по номерам; почти всегда уже по порядку */
        AsmPk k = a->ix[i];
        int j = i;
        while (j > 0 && (u16)(a->ix[j - 1].seq - first) > (u16)(k.seq - first)) { a->ix[j] = a->ix[j - 1]; j--; }
        a->ix[j] = k;
    }
    a->outn = 0;
    a->idr = a->sps = false;
    switch (a->codec) {
    case VC_H264: return asm_build_h264(a);
    case VC_H265: return asm_build_h265(a);
    case VC_VP8: case VC_VP9: return asm_build_vpx(a);
    case VC_AV1: return asm_build_av1(a);
    default: return false;
    }
}
static void asm_close(H264Asm *a) {             /* кадр закончен (собран или выброшен) */
    if (a->have_mark) { a->end_seq = a->mark_seq; a->have_end = true; }
    else a->have_end = false;
    a->cur = a->broken = a->have_mark = false;
    a->nix = 0;
    a->pkn = 0;
}
int h264_asm_push(H264Asm *a, u16 seq, u32 ts, bool marker, const u8 *p, size_t n) {
    if (!a) return H264A_NONE;
    int r = H264A_NONE;
    if (a->have_end && (int16_t)(u16)(seq - a->end_seq) <= 0) return r;   /* запоздавший или дубль */
    if (!n && !a->cur) {                        /* пустой пакет между кадрами: только номер */
        if (a->have_end && (u16)(seq - a->end_seq) == 1) a->end_seq = seq;
        return r;
    }
    if (a->cur && ts != a->ts) {
        if ((int32_t)(ts - a->ts) < 0) return r;   /* хвост уже выброшенного кадра */
        if (!a->broken) r |= H264A_LOST;        /* начался следующий, а текущий не собран */
        asm_close(a);
    }
    if (!a->cur) { a->cur = true; a->ts = ts; a->broken = a->have_mark = false; a->nix = 0; a->pkn = 0; }
    if (a->broken) { if (marker) { a->have_mark = true; a->mark_seq = seq; asm_close(a); } return r; }
    if (a->have_mark && (int16_t)(u16)(seq - a->mark_seq) > 0) return r;   /* после marker с той же отметкой — мусор */
    for (int i = 0; i < a->nix; i++) if (a->ix[i].seq == seq) return r;   /* дубль */
    u8 fl = 0;
    if (n && (a->codec == VC_VP8 || a->codec == VC_VP9)) {   /* описатель нагрузки снимается сразу */
        int off = a->codec == VC_VP8 ? vp8_desc(p, n, &fl) : vp9_desc(p, n, &fl);
        if (off < 0) {
            a->broken = true;
            r |= H264A_LOST;
            if (marker) { a->have_mark = true; a->mark_seq = seq; asm_close(a); }
            return r;
        }
        p += off;
        n -= (size_t)off;
    }
    if (a->nix == ASM_MAXPK || a->pkn + n > a->max) {
        a->broken = true;
        r |= H264A_LOST;
        if (marker) { a->have_mark = true; a->mark_seq = seq; asm_close(a); }
        return r;
    }
    if (a->nix == a->ixcap) {
        int nc = a->ixcap ? a->ixcap * 2 : 128;
        AsmPk *ni = malloc((size_t)nc * sizeof *ni);
        if (!ni) { a->broken = true; return r | H264A_LOST; }
        if (a->ix) { memcpy(ni, a->ix, (size_t)a->ixcap * sizeof *ni); free(a->ix); }
        a->ix = ni;
        a->ixcap = nc;
    }
    if (n && !asm_grow(&a->pk, &a->pkcap, a->pkn + n, a->max)) { a->broken = true; return r | H264A_LOST; }
    if (n) memcpy(a->pk + a->pkn, p, n);
    a->ix[a->nix++] = (AsmPk){ seq, fl, (u32)a->pkn, (u32)n };
    a->pkn += n;
    if (marker) { a->have_mark = true; a->mark_seq = seq; }
    if (!a->have_mark) return r;
    /* все ли на месте: от границы прошлого кадра (или самого раннего пакета, если границы нет) до marker */
    u16 first;
    if (a->have_end) first = (u16)(a->end_seq + 1);
    else {
        u16 far = 0;
        for (int i = 0; i < a->nix; i++) { u16 dd = (u16)(a->mark_seq - a->ix[i].seq); if (dd > far) far = dd; }
        first = (u16)(a->mark_seq - far);
    }
    if ((int)(u16)(a->mark_seq - first) + 1 != a->nix) return r;   /* ждём недостающие (или следующий кадр -> LOST) */
    bool ok = asm_build(a, first);
    asm_close(a);
    if (!ok) { a->outn = 0; return r | H264A_LOST; }
    return r | H264A_FRAME;
}

/* ---------- selftest сборщика: packetize -> перемешать -> собрать ---------- */
typedef struct { u8 p[64][1400]; size_t n[64]; bool m[64]; int k; } DpT;
static void dp_emit(void *u, const u8 *p, size_t len, bool marker) {
    DpT *t = u;
    if (t->k < 64 && len <= sizeof t->p[0]) { memcpy(t->p[t->k], p, len); t->n[t->k] = len; t->m[t->k] = marker; t->k++; }
}
/* кадр Annex B из NAL: старт-коды 3 и 4 байта вперемешку; norm — то же с длинными старт-кодами */
static size_t dp_frame(u8 *f, u8 *norm, size_t *nn, const u8 *const *nal, const size_t *len, int cnt) {
    size_t n = 0, k = 0;
    for (int i = 0; i < cnt; i++) {
        if (i & 1) { memcpy(f + n, "\0\0\1", 3); n += 3; } else { memcpy(f + n, "\0\0\0\1", 4); n += 4; }
        memcpy(f + n, nal[i], len[i]); n += len[i];
        memcpy(norm + k, "\0\0\0\1", 4); k += 4;
        memcpy(norm + k, nal[i], len[i]); k += len[i];
    }
    *nn = k;
    return n;
}
int h264_depacketize_selftest(void (*check)(bool ok, const char *what)) {
    static u8 f[12000], norm[12000], idr[5000], pf[3000];
    static const u8 sps[] = { 0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8, 0x40 }, pps[] = { 0x68, 0xCE, 0x3C, 0x80 };
    idr[0] = 0x65; for (size_t i = 1; i < sizeof idr; i++) idr[i] = (u8)((i * 7 + 3) | 1);
    pf[0] = 0x41; for (size_t i = 1; i < sizeof pf; i++) pf[i] = (u8)((i * 13 + 5) | 1);
    DpT *t = calloc(1, sizeof *t);
    H264Asm *a = h264_asm_new(0);
    if (!t || !a) { free(t); h264_asm_free(a); check(false, "Go Live: сборщик кадра — нет памяти"); return 1; }
    const u8 *n1[] = { sps, pps, idr }; size_t l1[] = { sizeof sps, sizeof pps, sizeof idr };
    size_t nn = 0, fn = dp_frame(f, norm, &nn, n1, l1, 3);
    h264_packetize(f, fn, 1100, dp_emit, t);   /* STAP-A (SPS+PPS) + 5 FU-A */
    int np = t->k;
    /* 1) кадр 1: пакеты не по порядку (обмен 2<->4, последний — раньше предпоследнего), дубль */
    int ord[64];
    for (int i = 0; i < np; i++) ord[i] = i;
    if (np >= 6) { int x = ord[2]; ord[2] = ord[4]; ord[4] = x; x = ord[np - 1]; ord[np - 1] = ord[np - 2]; ord[np - 2] = x; }
    int r = 0, got = 0;
    u16 seq0 = 65530;                           /* переход номера через 0 */
    for (int i = 0; i < np; i++) {
        int q = ord[i];
        r = h264_asm_push(a, (u16)(seq0 + q), 90000, t->m[q], t->p[q], t->n[q]);
        if (r & H264A_FRAME) got++;
        if (i == 1) h264_asm_push(a, (u16)(seq0 + q), 90000, t->m[q], t->p[q], t->n[q]);   /* дубль */
    }
    size_t on = 0;
    bool isidr = false, hassps = false;
    const u8 *o = h264_asm_frame(a, &on, &isidr, &hassps);
    bool ok1 = np == 6 && got == 1 && on == nn && !memcmp(o, norm, nn) && isidr && hassps;
    char w[200];
    snprintf(w, sizeof w, "Go Live зритель: RTP -> кадр (STAP-A + FU-A, %d пакетов не по порядку, дубль, номер через 0) — байты как у WebRTC", np);
    check(ok1, w);
    /* 2) кадр 2 (P, 3 FU-A) с потерянным средним пакетом -> ничего; кадр 3 -> LOST + FRAME */
    const u8 *n2[] = { pf }; size_t l2[] = { sizeof pf };
    t->k = 0;
    fn = dp_frame(f, norm, &nn, n2, l2, 1);
    h264_packetize(f, fn, 1100, dp_emit, t);
    int np2 = t->k, r2 = 0;
    u16 s2 = (u16)(seq0 + np);
    for (int i = 0; i < np2; i++) if (i != 1) r2 |= h264_asm_push(a, (u16)(s2 + i), 93000, t->m[i], t->p[i], t->n[i]);
    int r3 = 0;
    u16 s3 = (u16)(s2 + np2);
    for (int i = 0; i < np2; i++) r3 |= h264_asm_push(a, (u16)(s3 + i), 96000, t->m[i], t->p[i], t->n[i]);
    o = h264_asm_frame(a, &on, &isidr, NULL);
    bool ok2 = np2 == 3 && r2 == H264A_NONE && (r3 & H264A_LOST) && (r3 & H264A_FRAME) && on == nn && !memcmp(o, norm, nn) && !isidr;
    /* 3) целиком потерянный кадр между двумя: граница кадров ловит дыру (LOST), следующий за ним — снова собирается */
    u16 s4 = (u16)(s3 + np2 + np2);             /* кадр с отметкой 99000 пропал весь */
    int r4 = 0;
    for (int i = 0; i < np2; i++) r4 |= h264_asm_push(a, (u16)(s4 + i), 102000, t->m[i], t->p[i], t->n[i]);
    int r5 = 0;
    u16 s5 = (u16)(s4 + np2);
    for (int i = 0; i < np2; i++) r5 |= h264_asm_push(a, (u16)(s5 + i), 105000, t->m[i], t->p[i], t->n[i]);
    bool ok3 = r4 == H264A_NONE && (r5 & H264A_LOST) && (r5 & H264A_FRAME);
    check(ok2 && ok3, "Go Live зритель: дыра в номерах — кадр выброшен (LOST), пропавший целиком кадр тоже ловится, следующий собирается");
    /* 4) битые пакеты: FU-A без начала, STAP-A с размером за край, FU-B — LOST, без выхода за буфер */
    h264_asm_reset(a);
    static const u8 fu_mid[] = { 0x7C, 0x05, 1, 2, 3 }, stap_bad[] = { 0x78, 0x00, 0x40, 0x67 }, fub[] = { 0x7D, 0x85, 1, 2 };
    int b1 = h264_asm_push(a, 100, 1000, true, fu_mid, sizeof fu_mid);
    int b2 = h264_asm_push(a, 101, 2000, true, stap_bad, sizeof stap_bad);
    int b3 = h264_asm_push(a, 102, 3000, true, fub, sizeof fub);
    int b4 = h264_asm_push(a, 103, 4000, true, sps, sizeof sps);   /* одиночный NAL — кадр */
    o = h264_asm_frame(a, &on, NULL, &hassps);
    check(b1 == H264A_LOST && b2 == H264A_LOST && b3 == H264A_LOST && b4 == H264A_FRAME && on == 4 + sizeof sps && hassps,
          "Go Live зритель: FU-A без начала, STAP-A за край, FU-B — кадр выброшен; одиночный NAL — кадр");
    /* 5) предел размера кадра: переполнение — LOST, память не растёт выше предела */
    H264Asm *sm = h264_asm_new(2000);
    int b5 = 0;
    for (int i = 0; i < 5 && sm; i++) b5 |= h264_asm_push(sm, (u16)(200 + i), 5000, i == 4, pf, 1000);
    bool ok5 = sm && (b5 & H264A_LOST) && !(b5 & H264A_FRAME) && h264_asm_bytes(sm) < 200000;
    h264_asm_free(sm);
    check(ok5, "Go Live зритель: кадр больше предела — выброшен, буфер не растёт");
    h264_asm_free(a);
    free(t);
    return 0;
}

/* ---------- selftest сборщика для H265 / VP8 / VP9 / AV1 (пакеты собраны вручную по RFC) ---------- */
static int vt_feed(H264Asm *a, u16 *seq, u32 ts, u8 (*pk)[1300], const size_t *pn, int np) {
    int r = 0;
    for (int i = 0; i < np; i++) r |= h264_asm_push(a, (*seq)++, ts, i == np - 1, pk[i], pn[i]);
    return r;
}
static bool vt_same(H264Asm *a, const u8 *want, size_t wn, bool key) {
    size_t n = 0;
    bool idr = false;
    const u8 *o = h264_asm_frame(a, &n, &idr, NULL);
    return n == wn && !memcmp(o, want, wn) && idr == key;
}
int vid_depacketize_selftest(void (*check)(bool ok, const char *what)) {
    static u8 pk[4][1300], want[4000], body[2600];
    size_t pn[4], wn;
    u16 seq = 40000;
    for (size_t i = 0; i < sizeof body; i++) body[i] = (u8)(i * 29 + 7);
    /* H265: AP(VPS, SPS) + FU(IDR_W_RADL, 3 части) */
    {
        static const u8 vps[] = { 0x40, 0x01, 0x0C, 0x01 }, sps[] = { 0x42, 0x01, 0x01, 0x60, 0x00 };
        H264Asm *a = vid_asm_new(VC_H265, 0);
        size_t k = 0;
        pk[0][k++] = 0x60; pk[0][k++] = 0x01;
        pk[0][k++] = 0; pk[0][k++] = sizeof vps; memcpy(pk[0] + k, vps, sizeof vps); k += sizeof vps;
        pk[0][k++] = 0; pk[0][k++] = sizeof sps; memcpy(pk[0] + k, sps, sizeof sps); k += sizeof sps;
        pn[0] = k;
        for (int i = 0; i < 3; i++) {
            pk[1 + i][0] = 0x62; pk[1 + i][1] = 0x01;
            pk[1 + i][2] = (u8)((i == 0 ? 0x80 : 0) | (i == 2 ? 0x40 : 0) | 19);
            memcpy(pk[1 + i] + 3, body + i * 800, 800);
            pn[1 + i] = 803;
        }
        wn = 0;
        memcpy(want + wn, "\0\0\0\1", 4); wn += 4; memcpy(want + wn, vps, sizeof vps); wn += sizeof vps;
        memcpy(want + wn, "\0\0\0\1", 4); wn += 4; memcpy(want + wn, sps, sizeof sps); wn += sizeof sps;
        memcpy(want + wn, "\0\0\0\1", 4); wn += 4; want[wn++] = 0x26; want[wn++] = 0x01;
        memcpy(want + wn, body, 2400); wn += 2400;
        int r = a ? vt_feed(a, &seq, 1000, pk, pn, 4) : 0;
        check(a && r == H264A_FRAME && vt_same(a, want, wn, true), "Go Live зритель: H265 (RFC 7798) AP + FU -> Annex B, ключевой (IRAP)");
        h264_asm_free(a);
    }
    /* VP8: описатель с X/I и 15-битным PictureID; S=1 PID=0 — начало; P=0 — ключевой */
    {
        H264Asm *a = vid_asm_new(VC_VP8, 0);
        static const u8 d1[] = { 0x90, 0x80, 0x81, 0x23 }, d2[] = { 0x80, 0x80, 0x81, 0x23 };
        memcpy(pk[0], d1, 4); pk[0][4] = 0x50; memcpy(pk[0] + 5, body, 900); pn[0] = 905;
        memcpy(pk[1], d2, 4); memcpy(pk[1] + 4, body + 900, 700); pn[1] = 704;
        wn = 0; want[wn++] = 0x50; memcpy(want + wn, body, 1600); wn += 1600;
        int r = a ? vt_feed(a, &seq, 2000, pk, pn, 2) : 0;
        bool ok1 = a && r == H264A_FRAME && vt_same(a, want, wn, true);
        pk[0][4] = 0x51;                         /* P = 1 — межкадровый */
        r = a ? vt_feed(a, &seq, 5000, pk, pn, 2) : 0;
        want[0] = 0x51;
        bool ok2 = a && r == H264A_FRAME && vt_same(a, want, wn, false);
        int r3 = a ? h264_asm_push(a, seq++, 8000, true, d2, 4) : 0;   /* без S и без нагрузки — битый */
        check(ok1 && ok2 && r3 == H264A_LOST, "Go Live зритель: VP8 (RFC 7741) описатель снят, ключевой/межкадровый по P, кадр без начала — LOST");
        h264_asm_free(a);
    }
    /* VP9: I + B (P=0) ... I + E; второй кадр с P=1 */
    {
        H264Asm *a = vid_asm_new(VC_VP9, 0);
        pk[0][0] = 0x88; pk[0][1] = 0x05; memcpy(pk[0] + 2, body, 1000); pn[0] = 1002;
        pk[1][0] = 0x84; pk[1][1] = 0x05; memcpy(pk[1] + 2, body + 1000, 500); pn[1] = 502;
        int r = a ? vt_feed(a, &seq, 9000, pk, pn, 2) : 0;
        bool ok1 = a && r == H264A_FRAME && vt_same(a, body, 1500, true);
        pk[0][0] = 0xC8; pk[1][0] = 0xC4;
        r = a ? vt_feed(a, &seq, 12000, pk, pn, 2) : 0;
        bool ok2 = a && r == H264A_FRAME && vt_same(a, body, 1500, false);
        check(ok1 && ok2, "Go Live зритель: VP9 описатель I/P/B/E снят, ключевой по P=0");
        h264_asm_free(a);
    }
    /* AV1: заголовок последовательности + кадр, разрезанный на 3 пакета (Z/Y/W/N); TD отбрасывается */
    {
        H264Asm *a = vid_asm_new(VC_AV1, 0);
        size_t k = 0;
        pk[0][k++] = 0x08 | 0x40 | 0x30;         /* N, Y, W=3 */
        pk[0][k++] = 1; pk[0][k++] = 0x10;       /* TD: длина 1, заголовок type 2 */
        pk[0][k++] = 11; pk[0][k++] = 0x08; memcpy(pk[0] + k, body, 10); k += 10;   /* seq header type 1 */
        pk[0][k++] = 0x30; memcpy(pk[0] + k, body + 10, 1000); k += 1000;            /* кадр type 6, начало */
        pn[0] = k;
        pk[1][0] = 0x80 | 0x40 | 0x10; memcpy(pk[1] + 1, body + 1010, 1000); pn[1] = 1001;   /* Z, Y, W=1 */
        pk[2][0] = 0x80 | 0x10; memcpy(pk[2] + 1, body + 2010, 500); pn[2] = 501;            /* Z, W=1 */
        wn = 0;
        want[wn++] = 0x0A; want[wn++] = 10; memcpy(want + wn, body, 10); wn += 10;
        want[wn++] = 0x30; memcpy(want + wn, body + 10, 2500); wn += 2500;
        int r = a ? vt_feed(a, &seq, 15000, pk, pn, 3) : 0;
        bool ok1 = a && r == H264A_FRAME && vt_same(a, want, wn, true);
        pk[1][0] = 0x10;                         /* продолжение потеряно: Z=0 при открытом OBU — битый */
        r = a ? vt_feed(a, &seq, 18000, pk, pn, 3) : 0;
        check(ok1 && (r & H264A_LOST) && !(r & H264A_FRAME), "Go Live зритель: AV1 OBU из 3 пакетов (Z/Y/W/N), размер у всех кроме последнего, TD мимо; разрыв OBU — LOST");
        h264_asm_free(a);
    }
    /* номера PT и имена */
    check(vc_of_pt(105) == VC_H264 && vc_of_pt(107) == VC_VP8 && vc_of_pt(103) == VC_H265 && vc_of_pt(109) == VC_VP9 && vc_of_pt(101) == VC_AV1
          && vc_of_pt(106) < 0 && vc_of_name("AV1") == VC_AV1 && vc_of_name("h264") < 0, "Go Live зритель: PT кодеков 105/107/103/109/101, RTX — не видео");
    return 0;
}

/* ---------- набор кодеков зрителя и откат ---------- */
unsigned watch_codec_set(bool wide) { return wide ? VC_ALL : VC_CHEAP; }
int watch_codecs_json(char *out, size_t cap, bool wide) {
    unsigned set = watch_codec_set(wide);
    size_t k = 0;
    if (cap) out[0] = 0;
    for (int vc = 0; vc < VC_N; vc++) {          /* приоритет — по цене декодирования (меньше — лучше) */
        if (!(set >> vc & 1)) continue;
        int m = snprintf(out + k, cap - k, ",{\"name\":\"%s\",\"type\":\"video\",\"priority\":%d,\"payload_type\":%d,\"rtx_payload_type\":%d,\"encode\":false,\"decode\":true}",
                         vc_name(vc), (vc + 1) * 1000, vc_pt(vc), vc_pt(vc) + 1);
        if (m < 0 || (size_t)m >= cap - k) return -1;
        k += (size_t)m;
    }
    return (int)k;
}
int golive_fallback(const WatchFbIn *s) {
    if (s->outside && !s->wide) return 1;       /* автор шлёт не то, что объявили: просим всё */
    bool nopkt = s->vpkts == 0 && s->wanted_ms >= 6000;   /* сессия готова, видно 6 с — ни одного видеопакета */
    bool nopic = s->frames > 0 && s->shown == 0 && s->wanted_at_first >= 0 && s->wanted_ms - s->wanted_at_first >= 6000;   /* кадры идут, картинки нет */
    if (nopkt || nopic) return s->wide ? 2 : 1;
    return 0;
}
int golive_fallback_selftest(void (*check)(bool ok, const char *what)) {
    char j[1200];
    int n0 = watch_codecs_json(j, sizeof j, false);
    bool cheap = n0 > 0 && strstr(j, "\"H264\"") && strstr(j, "\"VP8\"") && !strstr(j, "\"H265\"") && !strstr(j, "\"VP9\"") && !strstr(j, "\"AV1\"")
                 && strstr(j, "\"payload_type\":105") && strstr(j, "\"payload_type\":107");
    int n1 = watch_codecs_json(j, sizeof j, true);
    bool all = n1 > n0 && strstr(j, "\"H264\"") && strstr(j, "\"VP8\"") && strstr(j, "\"H265\"") && strstr(j, "\"VP9\"") && strstr(j, "\"AV1\"");
    char tiny[40];
    check(cheap && all && watch_codecs_json(tiny, sizeof tiny, true) == -1, "Go Live зритель: op 1 — по умолчанию только H264 + VP8, широкий набор — все 5 кодеков, тесный буфер — отказ");
    WatchFbIn a = { false, false, 0, 0, 0, 5000, -1 };
    bool r1 = golive_fallback(&a) == 0;          /* 5 с без пакетов — рано */
    a.wanted_ms = 6000; bool r2 = golive_fallback(&a) == 1;   /* 6 с видно, пакетов нет — откат */
    a.wide = true; bool r3 = golive_fallback(&a) == 2;        /* и широкий не помог — ошибка */
    WatchFbIn b = { false, false, 500, 40, 0, 9000, 4000 };
    bool r4 = golive_fallback(&b) == 0;          /* кадры идут 5 с видимости без картинки — ещё рано */
    b.wanted_ms = 10000; bool r5 = golive_fallback(&b) == 1;
    b.shown = 1; bool r6 = golive_fallback(&b) == 0;          /* хоть один показан — работает */
    WatchFbIn c = { false, true, 10, 0, 0, 0, -1 };
    bool r7 = golive_fallback(&c) == 1;          /* PT вне набора — сразу */
    c.wide = true; bool r8 = golive_fallback(&c) == 0;
    WatchFbIn d = { false, false, 0, 0, 0, 0, -1 };
    bool r9 = golive_fallback(&d) == 0;          /* панель скрыта (время видимости 0) — без ложного отката */
    check(r1 && r2 && r3 && r4 && r5 && r6 && r7 && r8 && r9, "Go Live зритель: откат на все кодеки — 6 с видимости без пакетов или без картинки, PT вне набора; широкий без картинки — ошибка; скрытая панель — без отката");
    return 0;
}

/* ---------- selftest ---------- */
typedef struct { u8 buf[16384]; size_t len; int pk, marks, big; bool bad_mark; size_t mtu; u8 cur[8192]; size_t curn; int nals; u8 nal_out[8][8192]; size_t nal_len[8]; } PkT;
static void pk_emit(void *u, const u8 *p, size_t len, bool marker) {
    PkT *t = u;
    t->pk++;
    if (len > t->mtu) t->big++;
    if (marker) t->marks++;
    int type = p[0] & 0x1F;
    if (type == 24) {                           /* STAP-A: размер + NAL ... */
        size_t k = 1;
        while (k + 2 <= len && t->nals < 8) {
            size_t sz = (size_t)p[k] << 8 | p[k + 1];
            k += 2;
            if (k + sz > len) break;
            memcpy(t->nal_out[t->nals], p + k, sz); t->nal_len[t->nals++] = sz;
            k += sz;
        }
        return;
    }
    if (type == 28) {                           /* FU-A: собрать NAL обратно */
        if (p[1] & 0x80) { t->curn = 0; t->cur[t->curn++] = (u8)((p[0] & 0xE0) | (p[1] & 0x1F)); }
        memcpy(t->cur + t->curn, p + 2, len - 2);
        t->curn += len - 2;
        if (p[1] & 0x40 && t->nals < 8) { memcpy(t->nal_out[t->nals], t->cur, t->curn); t->nal_len[t->nals++] = t->curn; }
    } else if (t->nals < 8) { memcpy(t->nal_out[t->nals], p, len); t->nal_len[t->nals++] = len; }
}
int golive_selftest(void (*check)(bool ok, const char *what)) {
    /* кадр: SPS (10), PPS (4), IDR (5000) — старт-коды 4 и 3 байта */
    static u8 f[6000];
    size_t n = 0;
    static const u8 sps[] = { 0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8, 0x40 }, pps[] = { 0x68, 0xCE, 0x3C, 0x80 };
    memcpy(f + n, "\0\0\0\1", 4); n += 4; memcpy(f + n, sps, sizeof sps); n += sizeof sps;
    memcpy(f + n, "\0\0\1", 3); n += 3; memcpy(f + n, pps, sizeof pps); n += sizeof pps;
    memcpy(f + n, "\0\0\0\1", 4); n += 4;
    size_t idr = n;
    f[n++] = 0x65;
    for (int i = 1; i < 5000; i++) f[n++] = (u8)((i * 7 + 3) | 1);   /* без 00 00: внутри NAL старт-кодов нет */
    PkT *t = calloc(1, sizeof *t);
    t->mtu = 1100;
    int np = h264_packetize(f, n, t->mtu, pk_emit, t);
    bool same = t->nals == 3 && t->nal_len[0] == sizeof sps && !memcmp(t->nal_out[0], sps, sizeof sps)
        && t->nal_len[1] == sizeof pps && !memcmp(t->nal_out[1], pps, sizeof pps)
        && t->nal_len[2] == 5000 && !memcmp(t->nal_out[2], f + idr, 5000);
    bool ok = np == t->pk && np == 1 + 5 && !t->big && t->marks == 1 && same;
    free(t);
    char w[200];
    snprintf(w, sizeof w, "Go Live: H.264 -> RTP (SPS+PPS — STAP-A, IDR 5000 Б — FU-A по 1100): пакетов %d, метка одна, сборка совпала", np);
    check(ok, w);
    /* состояние: ключ, события чужого потока — мимо, STREAM_DELETE */
    bool d = O.demo;
    O.demo = true;
    char err[96];
    u64 me = S.me_id;
    S.me_id = 1000;
    bool s1 = golive_start(2000, 3010, err, sizeof err);
    GoLiveState st;
    golive_state(&st);
    bool s2 = s1 && st.phase == GL_CREATING && !strcmp(st.key, "guild:2000:3010:1000");
    cJSON *ev = cJSON_Parse("{\"stream_key\":\"guild:2000:3010:1000\",\"rtc_server_id\":\"777\"}");
    cJSON *ev2 = cJSON_Parse("{\"stream_key\":\"guild:2000:3010:999\",\"endpoint\":\"evil.example:443\",\"token\":\"x\"}");
    cJSON *ev3 = cJSON_Parse("{\"stream_key\":\"guild:2000:3010:1000\",\"endpoint\":\"c-ams01.discord.media:443\",\"token\":\"tok\"}");
    golive_on_event("STREAM_CREATE", ev);
    golive_on_event("STREAM_SERVER_UPDATE", ev2);
    golive_state(&st);
    bool s3 = st.phase == GL_WAIT_SERVER && !strcmp(st.rtc_server_id, "777") && !st.endpoint[0];
    golive_on_event("STREAM_SERVER_UPDATE", ev3);
    golive_state(&st);
    bool s4 = st.phase == GL_CONNECTING && !strcmp(st.endpoint, "c-ams01.discord.media:443");
    golive_stop();
    golive_state(&st);
    bool s5 = st.phase == GL_OFF && !st.token[0];
    cJSON_Delete(ev); cJSON_Delete(ev2); cJSON_Delete(ev3);
    S.me_id = me;
    O.demo = d;
    check(s2 && s3 && s4 && s5, "Go Live: op 18 -> STREAM_CREATE -> STREAM_SERVER_UPDATE (чужой ключ — мимо), стоп стирает токен");
    voice_stream_selftest(check);
    h264_depacketize_selftest(check);
    vid_depacketize_selftest(check);
    vdec_selftest(check);
    gvid_selftest(check);
    golive_fallback_selftest(check);
    return 0;
}
#endif
