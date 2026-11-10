/* golive.c — «Astra DS Video»: трансляция экрана в голосовой канал (Discord Go Live). Только при -DASTRA_VIDEO.
 *
 * Шаг 1 (этот файл): шлюз и состояние потока, RTP-пакетизация H.264 и вход кадров кодера.
 *   Шлюз: op 18 STREAM_CREATE {type:"guild", guild_id, channel_id, preferred_region:null} -> событие STREAM_CREATE
 *   {stream_key, rtc_server_id} и STREAM_SERVER_UPDATE {stream_key, endpoint, token}; op 22 STREAM_SET_PAUSED
 *   {stream_key, paused}; op 19 STREAM_DELETE {stream_key}; событие STREAM_DELETE {stream_key, reason}.
 *   stream_key: "guild:<gid>:<cid>:<uid>" (личный звонок — "call:<cid>:<uid>").
 *   H.264 (RFC 6184): NAL <= MTU — одиночный пакет; больше — FU-A (индикатор F|NRI|28, заголовок S|E|тип);
 *   метка (marker) — на последнем пакете кадра. Отметка времени — 90 кГц.
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
/* состояние соединения трансляции (из его потока) */
void golive_vstatus(int ph, const char *err) {
    gl_lock();
    if (GL.phase != GL_OFF) {
        if (ph == VS_CONNECTED) { GL.phase = GL_LIVE; share_request_idr(); }   /* зрителю — сразу ключевой кадр */
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
    return 0;
}
#endif
