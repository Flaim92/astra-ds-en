/* voice_stream.c — голосовое соединение трансляции экрана («Astra DS Video», Go Live, шаг 2).
 * Включается в voice.c (#include, как socks.c) только при -DASTRA_VIDEO: отдельно не компилируется.
 *
 * Второй VW (stream = true) со своим потоком: тот же голосовой шлюз (identify с server_id = rtc_server_id потока,
 * тот же session_id, video:true, streams [screen]), IP discovery, выбор протокола с кодеками opus/H264,
 * описание сессии (op 4) -> op 12 VIDEO {audio_ssrc, video_ssrc, rtx_ssrc, streams[screen]}; DAVE — тем же
 * кодом, что у звонка, но шифратор назначен на video_ssrc с кодеком H264 (libdave оставляет заголовки NAL
 * открытыми). Кадры Annex B от кодера (golive_frame -> voice_stream_frame) -> DAVE -> RTP (PT 101, 90 кГц,
 * FU-A через h264_packetize) -> AES-GCM rtpsize, как голос. Звук — Opus на audio_ssrc (vs_send_audio); входящее — только RTCP.
 * Обычный звонок не затронут: общие функции отличают поток по w->stream / t_stream. Живая проверка — только через
 * координатора (тестовый аккаунт). */
#include "golive.h"

#define VS_PT_H264 105
#define VS_PT_RTX 106
#define VS_MTU 1100
#define VS_QMAX 4

static struct {
    PlatThread *thr;
    atomic_bool stop;
    PlatWake *cmd;
    PlatMutex *mu;                               /* очередь кадров */
    struct { u8 *p; size_t n; bool key; u32 ts; } q[VS_QMAX];
    int qn;
    VW *w;
} VSt;

/* разбор составного RTCP (уже расшифрованного): PT 206 FMT 1 (PLI) / FMT 4 (FIR) -> ключевой кадр;
 * PT 205 FMT 1 (generic NACK: PID + BLP) -> повтор по RTX. Только для своего video_ssrc. */
static bool vs_rtx(VW *w, u16 seq);
static int vs_rtcp_parse(VW *w, const u8 *p, int n) {
    int got = 0;
    for (int off = 0; off + 12 <= n; ) {
        int fmt = p[off] & 31, pt = p[off + 1], len = ((p[off + 2] << 8) | p[off + 3]) * 4 + 4;
        if (len < 12 || off + len > n) break;
        u32 media = (u32)p[off + 8] << 24 | (u32)p[off + 9] << 16 | (u32)p[off + 10] << 8 | p[off + 11];
        if (pt == 206 && (fmt == 1 || fmt == 4) && (media == w->vssrc || fmt == 4)) { w->vpli++; got++; share_request_idr(); }
        else if (pt == 205 && fmt == 1 && media == w->vssrc) {
            for (int q = off + 12; q + 4 <= off + len; q += 4) {
                u16 pid = (u16)(p[q] << 8 | p[q + 1]), blp = (u16)(p[q + 2] << 8 | p[q + 3]);
                w->vnack++; got++;
                vs_rtx(w, pid);
                for (int bit = 0; bit < 16; bit++) if (blp & (1 << bit)) { w->vnack++; vs_rtx(w, (u16)(pid + bit + 1)); }
            }
        }
        off += len;
    }
    return got;
}
/* RTP-пакет трансляции (rtpsize AES-GCM): заголовок 12 Б + расширение one-byte 0xBEDE/1 слово открыты (AAD),
 * данные расширения (id 6 playout-delay = 0) и полезная нагрузка шифруются. Возврат — длина пакета, 0 — ошибка. */
static size_t vs_rtp_build(VW *w, u8 pt, u32 ssrc, u16 seq, u32 ts, bool marker, const u8 *p, size_t len, u8 *pkt) {
    if (len > 1300) return 0;
    unsigned char plain[4 + 1400];
    pkt[0] = 0x90;                              /* V=2, X=1 */
    pkt[1] = (unsigned char)((marker ? 0x80 : 0) | pt);
    pkt[2] = (unsigned char)(seq >> 8); pkt[3] = (unsigned char)seq;
    pkt[4] = (unsigned char)(ts >> 24); pkt[5] = (unsigned char)(ts >> 16);
    pkt[6] = (unsigned char)(ts >> 8); pkt[7] = (unsigned char)ts;
    pkt[8] = (unsigned char)(ssrc >> 24); pkt[9] = (unsigned char)(ssrc >> 16);
    pkt[10] = (unsigned char)(ssrc >> 8); pkt[11] = (unsigned char)ssrc;
    pkt[12] = 0xBE; pkt[13] = 0xDE; pkt[14] = 0; pkt[15] = 1;
    plain[0] = (6 << 4) | (3 - 1);              /* id 6 playout-delay (у Discord id 5 — transport-wide-cc), 3 байта = 0 */
    plain[1] = plain[2] = plain[3] = 0;
    memcpy(plain + 4, p, len);
    unsigned char iv[12] = {0};
    w->nonce++;
    iv[0] = (unsigned char)(w->nonce >> 24); iv[1] = (unsigned char)(w->nonce >> 16);
    iv[2] = (unsigned char)(w->nonce >> 8); iv[3] = (unsigned char)w->nonce;
    unsigned char tag[16];
    int pn = (int)len + 4;
    if (!gcm(w->enc, true, w->key, iv, pkt, 16, plain, pn, pkt + 16, tag)) return 0;
    memcpy(pkt + 16 + pn, tag, 16);
    memcpy(pkt + 16 + pn + 16, iv, 4);
    return (size_t)(16 + pn + 20);
}
/* история отправленного видео для NACK -> RTX (~1 с при 30 к/с) */
#define VS_HIST 256
static struct { u16 seq; u32 ts; bool marker, used; u16 len; u8 p[1300]; } *vs_hist;
static void vs_rtp(void *u, const u8 *p, size_t len, bool marker) {
    VW *w = u;
    if (w->udp < 0 || !w->session_ready || len > 1300) return;
    unsigned char pkt[16 + 4 + 1400 + 20];
    size_t k = vs_rtp_build(w, VS_PT_H264, w->vssrc, w->vseq, w->vts, marker, p, len, pkt);
    if (!k) return;
    vudp_send(w, pkt, k);
    if (!vs_hist) vs_hist = calloc(VS_HIST, sizeof *vs_hist);
    if (vs_hist) {
        int i = w->vseq % VS_HIST;
        vs_hist[i].seq = w->vseq; vs_hist[i].ts = w->vts; vs_hist[i].marker = marker; vs_hist[i].used = true;
        vs_hist[i].len = (u16)len; memcpy(vs_hist[i].p, p, len);
    }
    w->vseq++;
    w->vsent++;
    w->vbytes += k;
}
/* RTX (RFC 4588): PT 106, ssrc rtx, свой номер; полезная нагрузка = исходный номер (2 Б) + исходная нагрузка */
static bool vs_rtx(VW *w, u16 seq) {
    if (!vs_hist || !w->vrtx) return false;
    int i = seq % VS_HIST;
    if (!vs_hist[i].used || vs_hist[i].seq != seq) return false;
    u8 pl[2 + 1300], pkt[16 + 4 + 1400 + 20];
    pl[0] = (u8)(seq >> 8); pl[1] = (u8)seq;
    memcpy(pl + 2, vs_hist[i].p, vs_hist[i].len);
    size_t k = vs_rtp_build(w, VS_PT_RTX, w->vrtx, w->vrtx_seq, vs_hist[i].ts, vs_hist[i].marker, pl, (size_t)vs_hist[i].len + 2, pkt);
    if (!k) return false;
    vudp_send(w, pkt, k);
    w->vrtx_seq++;
    w->vrtx_sent++;
    return true;
}
/* Входящий RTCP (rtpsize: первые 8 Б открыты, остальное — AES-GCM, суффикс nonce 4 Б): PLI/FIR -> ключевой кадр,
 * NACK -> повтор по RTX. Возврат: сколько обратной связи разобрано. */
static int vs_rtcp_in(VW *w, const u8 *b, int n) {
    if (n < 8 + 16 + 4 || (b[0] >> 6) != 2 || b[1] < 200 || b[1] > 206) return 0;
    int cn = n - 8 - 16 - 4;
    u8 full[1500];
    if (cn <= 0 || 8 + cn > (int)sizeof full) return 0;
    unsigned char iv[12] = {0}, tag[16];
    memcpy(iv, b + n - 4, 4);
    memcpy(tag, b + n - 20, 16);
    memcpy(full, b, 8);
    if (!gcm(w->dec, false, w->key, iv, b, 8, b + 8, cn, full + 8, tag)) return 0;
    return vs_rtcp_parse(w, full, 8 + cn);
}
/* RTCP SR (RFC 3550) по видео-ssrc: заголовок 8 байт открыт (AAD), тело 20 байт шифруется, как у них */
static void vs_rtcp_sr(VW *w) {
    if (w->udp < 0 || !w->session_ready || !w->vssrc || !w->vsent) return;
    unsigned char pkt[8 + 20 + 20], body[20];
    pkt[0] = 0x80; pkt[1] = 0xC8; pkt[2] = 0x00; pkt[3] = 0x06;
    pkt[4] = (unsigned char)(w->vssrc >> 24); pkt[5] = (unsigned char)(w->vssrc >> 16);
    pkt[6] = (unsigned char)(w->vssrc >> 8); pkt[7] = (unsigned char)w->vssrc;
    i64 ms = wall_ms();
    u32 msw = (u32)(ms / 1000 + 2208988800LL), lsw = (u32)((double)(ms % 1000) / 1000.0 * 4294967296.0);
    u32 v[5] = { msw, lsw, w->vts, (u32)w->vsent, (u32)w->vbytes };
    for (int i = 0; i < 5; i++) { body[i * 4] = (unsigned char)(v[i] >> 24); body[i * 4 + 1] = (unsigned char)(v[i] >> 16); body[i * 4 + 2] = (unsigned char)(v[i] >> 8); body[i * 4 + 3] = (unsigned char)v[i]; }
    unsigned char iv[12] = {0};
    w->nonce++;
    iv[0] = (unsigned char)(w->nonce >> 24); iv[1] = (unsigned char)(w->nonce >> 16);
    iv[2] = (unsigned char)(w->nonce >> 8); iv[3] = (unsigned char)w->nonce;
    unsigned char tag[16];
    if (!gcm(w->enc, true, w->key, iv, pkt, 8, body, 20, pkt + 8, tag)) return;
    memcpy(pkt + 28, tag, 16);
    memcpy(pkt + 44, iv, 4);
    vudp_send(w, pkt, 48);
}
/* Прогон 11: P-кадры у зрителя расшифровываются, ключевые (SPS+PPS+IDR) — нет. Причина: депакетизатор WebRTC
 * зрителя (video_rtp_depacketizer_h264 -> SpsVuiRewriter) ПЕРЕПИСЫВАЕТ SPS без VUI/bitstream_restriction ещё до
 * расшифровки, и байты кадра расходятся с зашифрованными. Отправитель WebRTC делает это до шифрования. Делаем так же:
 * SPS без VUI получает VUI только с bitstream_restriction (max_num_reorder_frames = 0, max_dec_frame_buffering =
 * max_num_ref_frames) — ровно то, что зритель считает «VUI в порядке» и уже не трогает. */
typedef struct { const u8 *b; size_t n, pos; bool bad; } VBr;
typedef struct { u8 *b; size_t cap, pos; bool bad; } VBw;
static unsigned vbr_bit(VBr *r) { if (r->pos >= r->n * 8) { r->bad = true; return 0; } unsigned v = (r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1; r->pos++; return v; }
static unsigned vbr_bits(VBr *r, int k) { unsigned v = 0; while (k--) v = v << 1 | vbr_bit(r); return v; }
static unsigned vbr_ue(VBr *r) { int z = 0; while (!vbr_bit(r) && !r->bad && z < 32) z++; return z >= 32 ? (r->bad = true, 0) : ((1u << z) - 1 + vbr_bits(r, z)); }
static void vbw_bit(VBw *w, unsigned v) {
    if (w->pos >= w->cap * 8) { w->bad = true; return; }
    if (!(w->pos & 7)) w->b[w->pos >> 3] = 0;
    if (v) w->b[w->pos >> 3] |= (u8)(0x80 >> (w->pos & 7));
    w->pos++;
}
static void vbw_bits(VBw *w, unsigned v, int k) { while (k--) vbw_bit(w, (v >> k) & 1); }
static void vbw_ue(VBw *w, unsigned v) { unsigned x = v + 1; int k = 0; while ((x >> k) > 1) k++; vbw_bits(w, 0, k); vbw_bits(w, x, k + 1); }
static unsigned vcp_bits(VBr *r, VBw *w, int k) { unsigned v = vbr_bits(r, k); vbw_bits(w, v, k); return v; }
static unsigned vcp_ue(VBr *r, VBw *w) { unsigned v = vbr_ue(r); vbw_ue(w, v); return v; }
/* nal — SPS с заголовком (0x67), без старт-кода; out — новый NAL (с заголовком). 0 — оставить как есть. */
static size_t vs_sps_vui(const u8 *nal, size_t n, u8 *out, size_t cap) {
    if (n < 4 || n > 256 || (nal[0] & 31) != 7) return 0;
    u8 rb[256];
    size_t rn = 0;
    for (size_t i = 1; i < n; i++) {           /* RBSP: убрать 0x03 после 00 00 */
        if (i >= 3 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0 && rn >= 2 && rb[rn - 1] == 0 && rb[rn - 2] == 0) continue;
        rb[rn++] = nal[i];
    }
    u8 wb[300];
    VBr r = { rb, rn, 0, false };
    VBw w = { wb, sizeof wb, 0, false };
    unsigned prof = vcp_bits(&r, &w, 8);
    vcp_bits(&r, &w, 16);                       /* constraint flags + level */
    vcp_ue(&r, &w);                             /* sps id */
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 || prof == 118 || prof == 128 || prof == 138 || prof == 139 || prof == 134 || prof == 135) {
        if (vcp_ue(&r, &w) == 3) vcp_bits(&r, &w, 1);
        vcp_ue(&r, &w); vcp_ue(&r, &w); vcp_bits(&r, &w, 1);
        if (vcp_bits(&r, &w, 1)) return 0;     /* матрицы квантования — не разбираем */
    }
    vcp_ue(&r, &w);                             /* log2_max_frame_num_minus4 */
    unsigned poc = vcp_ue(&r, &w);
    if (poc == 0) vcp_ue(&r, &w);
    else if (poc == 1) {
        vcp_bits(&r, &w, 1); vcp_ue(&r, &w); vcp_ue(&r, &w);   /* se — тем же кодом ue */
        unsigned c = vcp_ue(&r, &w);
        if (c > 255) return 0;
        for (unsigned i = 0; i < c; i++) vcp_ue(&r, &w);
    }
    unsigned refs = vcp_ue(&r, &w);             /* max_num_ref_frames */
    vcp_bits(&r, &w, 1);                        /* gaps */
    vcp_ue(&r, &w); vcp_ue(&r, &w);             /* размеры в макроблоках */
    if (!vcp_bits(&r, &w, 1)) vcp_bits(&r, &w, 1);   /* frame_mbs_only / mb_adaptive */
    vcp_bits(&r, &w, 1);                        /* direct_8x8 */
    if (vcp_bits(&r, &w, 1)) { vcp_ue(&r, &w); vcp_ue(&r, &w); vcp_ue(&r, &w); vcp_ue(&r, &w); }   /* обрезка */
    if (r.bad || w.bad) return 0;
    if (vbr_bit(&r)) return 0;                  /* VUI уже есть — не трогаем */
    vbw_bit(&w, 1);                             /* vui_parameters_present_flag */
    vbw_bits(&w, 0, 5);                         /* aspect_ratio, overscan, video_signal, chroma_loc, timing: нет */
    vbw_bits(&w, 0, 3);                         /* nal_hrd, vcl_hrd, pic_struct: нет */
    vbw_bit(&w, 1);                             /* bitstream_restriction_flag */
    vbw_bit(&w, 1);                             /* motion_vectors_over_pic_boundaries */
    vbw_ue(&w, 2); vbw_ue(&w, 1);               /* max_bytes_per_pic_denom, max_bits_per_mb_denom (как WebRTC) */
    vbw_ue(&w, 16); vbw_ue(&w, 16);             /* log2_max_mv_length h/v */
    vbw_ue(&w, 0);                              /* max_num_reorder_frames */
    vbw_ue(&w, refs);                           /* max_dec_frame_buffering */
    vbw_bit(&w, 1);                             /* rbsp_stop_one_bit */
    while (w.pos & 7) vbw_bit(&w, 0);
    if (w.bad) return 0;
    size_t wn = w.pos / 8, k = 0;
    if (cap < 1 + wn * 3 / 2 + 2) return 0;
    out[k++] = nal[0];
    int z = 0;
    for (size_t i = 0; i < wn; i++) {           /* обратно в NAL: 0x03 после 00 00 перед байтом <= 3 */
        if (z >= 2 && wb[i] <= 3) { out[k++] = 3; z = 0; }
        out[k++] = wb[i];
        z = wb[i] == 0 ? z + 1 : 0;
    }
    return k;
}
/* Прогон 9: у зрителя «Decrypt failed, no valid cryptor found» при ключе нашего поколения 0. Зритель (WebRTC)
 * собирает кадр из RTP с длинными старт-кодами 00 00 00 01 перед каждым NAL — DAVE проверяет тег по ЭТИМ байтам.
 * Кодер даёт и короткие 00 00 01: шифруем кадр уже в том виде, в каком его соберёт зритель (все старт-коды — 4 байта). */
static u8 *vs_long_startcodes(const u8 *f, size_t n, size_t *on) {
    size_t ocap = n + n / 3 + 16 + 512;
    u8 *o = malloc(ocap);
    if (!o) return NULL;
    size_t k = 0, i = 0;
    while (i + 3 <= n) {
        size_t s;
        if (f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 1) s = i + 3;
        else if (i + 4 <= n && f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 0 && f[i + 3] == 1) s = i + 4;
        else { i++; continue; }
        size_t e = s;
        while (e + 3 <= n && !(f[e] == 0 && f[e + 1] == 0 && (f[e + 2] == 1 || (f[e + 2] == 0 && e + 3 < n && f[e + 3] == 1)))) e++;
        if (e + 3 > n) e = n;
        o[k++] = 0; o[k++] = 0; o[k++] = 0; o[k++] = 1;
        size_t sv = (f[s] & 31) == 7 ? vs_sps_vui(f + s, e - s, o + k, ocap - k) : 0;
        if (sv) k += sv;
        else { if (k + (e - s) > ocap) { free(o); return NULL; } memcpy(o + k, f + s, e - s); k += e - s; }
        i = e;
    }
    *on = k;
    return o;
}
/* кадр -> длинные старт-коды -> DAVE (если сессия E2EE) -> RTP */
static void vs_send_frame(VW *w, const u8 *f0, size_t n0, u32 ts) {
    if (!w->session_ready || !w->vssrc) return;
    if (!w->dave || !w->dave_on) return;        /* GL-1: только под DAVE; без E2EE видео не уходит никогда */
    size_t n = 0;
    u8 *norm = vs_long_startcodes(f0, n0, &n);
    if (!norm || !n) { free(norm); return; }
    const u8 *f = norm;
    const u8 *out = f;
    size_t on = n;
    u8 *enc = NULL;
    {
        size_t cap = daveEncryptorGetMaxCiphertextByteSize(w->denc, DAVE_MEDIA_TYPE_VIDEO, n);
        enc = malloc(cap);
        size_t wr = 0;
        int rc = enc ? (int)daveEncryptorEncrypt(w->denc, DAVE_MEDIA_TYPE_VIDEO, w->vssrc, f, n, enc, cap, &wr) : -1;
        if (rc != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) { w->vdave_fail++; free(enc); free(norm); return; }
        w->vdave_ok++;
        out = enc;
        on = wr;
    }
    w->vts = ts;
    h264_packetize(out, on, VS_MTU, vs_rtp, w);
    free(enc);
    free(norm);
}

/* Звук трансляции (дизайнер, 10.11): Opus 48 кГц стерео 20 мс от захвата звука (streamtest.c) -> очередь ->
 * поток трансляции: DAVE (ssrc звука назначен на OPUS) -> RTP PT 120 на audio_ssrc, как голос звонка (rtp_send).
 * Видео-путь не затронут. Только под DAVE (send_opus сам не шлёт без dave_on). */
#define VS_AQ 16
static struct { u8 p[1276]; u16 n; u32 ts; } vs_aq[VS_AQ];   /* 1276 — наибольший кадр Opus */
static int vs_aqn;
static long long vs_asent;
static void vs_send_audio(VW *w, const u8 *p, int n, u32 ts) {
    if (!w->session_ready || !w->ssrc || !w->dave || !w->dave_on) return;
    w->rtp_ts = ts;
    send_opus(w, p, n);
    vs_asent++;
}
void voice_stream_audio(const u8 *opus, size_t n, u32 ts48k) {
    if (!VSt.thr || !n || n > sizeof vs_aq[0].p) return;
    mutex_lock(VSt.mu);
    if (vs_aqn == VS_AQ) {                       /* поток трансляции не успевает — старый кадр звука выбросить */
        memmove(vs_aq, vs_aq + 1, (size_t)(VS_AQ - 1) * sizeof vs_aq[0]);
        vs_aqn--;
    }
    memcpy(vs_aq[vs_aqn].p, opus, n);
    vs_aq[vs_aqn].n = (u16)n;
    vs_aq[vs_aqn].ts = ts48k;
    vs_aqn++;
    mutex_unlock(VSt.mu);
    wake_signal(VSt.cmd);
}

static void vs_status(VoicePhase ph, const char *err) { golive_vstatus((int)ph, err); }

static int stream_thread(void *arg) {
    VW *w = arg;
    t_stream = true;
    thread_audio_priority();
    w->udp = -1;
    socks_reset(&w->sx);
    w->enc = EVP_CIPHER_CTX_new();
    w->dec = EVP_CIPHER_CTX_new();
    bool proxy_bad = false;
    if (!ws_connect(w, 10, &proxy_bad)) {
        if (!atomic_load(&VSt.stop)) vw_net_error(w, ui_tr("сервер трансляции недоступен"), ui_tr("VLESS: сервер трансляции недоступен через туннель"));
        goto out;
    }
    bool live = false;
    while (!atomic_load(&VSt.stop)) {
        Waitable wt[4];
        int n = 0;
        if (w->ws_wait) wt[n++] = sockwait_get(w->ws_wait);
        if (w->sx_wait) wt[n++] = sockwait_get(w->sx_wait);
        if (w->udp_wait && w->session_ready) wt[n++] = sockwait_get(w->udp_wait);
        wt[n++] = wake_waitable(VSt.cmd);
        i64 now = now_ms();
        i64 t = w->next_hb ? w->next_hb : now + 1000;
        plat_wait(wt, n, (int)(t > now ? (t - now < 1000 ? t - now : 1000) : 0));
        wake_clear(VSt.cmd);
        if (w->sx_wait) {
            sockwait_clear(w->sx_wait);
            if (!socks_alive(&w->sx)) { vw_net_error(w, "", ui_tr("VLESS: туннель для трансляции оборвался")); break; }
        }
        if (w->ws_wait) { sockwait_clear(w->ws_wait); vw_read_ws(w); }
        if (atomic_load(&VSt.stop)) break;
        if (w->ws_dead) { vs_status(VS_FAILED, ui_tr("соединение трансляции оборвалось")); break; }
        if (w->udp_wait) {                       /* входящее нам не нужно — вычерпать */
            sockwait_clear(w->udp_wait);
            unsigned char pk[2048];
            int k;
            for (int g = 0; g < 128 && (k = vudp_recv(w, pk, sizeof pk)) >= 0; g++) if (k > 0) vs_rtcp_in(w, pk, k);   /* PLI/FIR/NACK */
        }
        now = now_ms();
        if (w->next_hb && now >= w->next_hb) {
            if (!w->hb_acked && w->hb_interval) { vs_status(VS_FAILED, ui_tr("сервер трансляции не отвечает")); break; }
            vw_heartbeat(w);
            w->next_hb = now + (w->hb_interval ? w->hb_interval : 15000);
        }
        if (w->session_ready && w->udp >= 0 && now >= w->next_ka) {
            unsigned char ka[2] = { 0x13, 0x37 };
            vudp_send(w, ka, 2);
            w->next_ka = now + 5000;
        }
        if (live && now >= w->next_sr) { vs_rtcp_sr(w); w->next_sr = now + 1000; }
        if (w->session_ready && !w->dave) { vs_status(VS_FAILED, ui_tr("трансляция без сквозного шифрования невозможна")); break; }   /* GL-1 */
        bool ready = w->session_ready && w->vssrc && w->dave && w->dave_on;
        if (ready && !live) { live = true; vs_status(VS_CONNECTED, NULL); }
        /* кадры из очереди */
        for (;;) {
            mutex_lock(VSt.mu);
            if (!VSt.qn) { mutex_unlock(VSt.mu); break; }
            u8 *p = VSt.q[0].p;
            size_t pn = VSt.q[0].n;
            u32 ts = VSt.q[0].ts;
            memmove(VSt.q, VSt.q + 1, (size_t)(VSt.qn - 1) * sizeof VSt.q[0]);
            VSt.qn--;
            mutex_unlock(VSt.mu);
            if (live) vs_send_frame(w, p, pn, ts);
            free(p);
        }
        for (;;) {                               /* звук трансляции */
            u8 ap[sizeof vs_aq[0].p];
            int an;
            u32 ats;
            mutex_lock(VSt.mu);
            if (!vs_aqn) { mutex_unlock(VSt.mu); break; }
            an = vs_aq[0].n; ats = vs_aq[0].ts;
            memcpy(ap, vs_aq[0].p, (size_t)an);
            memmove(vs_aq, vs_aq + 1, (size_t)(vs_aqn - 1) * sizeof vs_aq[0]);
            vs_aqn--;
            mutex_unlock(VSt.mu);
            if (live) vs_send_audio(w, ap, an, ats);
            wipe(ap, sizeof ap);
        }
    }
out:
    if (w->denc) daveEncryptorDestroy(w->denc);
    if (w->dave) daveSessionDestroy(w->dave);
    if (w->enc) EVP_CIPHER_CTX_free(w->enc);
    if (w->dec) EVP_CIPHER_CTX_free(w->dec);
    if (w->ws_wait) sockwait_free(w->ws_wait);
    if (w->udp_wait) sockwait_free(w->udp_wait);
    if (w->sx_wait) sockwait_free(w->sx_wait);
    if (w->ws) curl_easy_cleanup(w->ws);
    udp_close(w->udp);
    socks_close(&w->sx);
    buf_free(&w->in);
    if (w->ws_buf) { wipe(w->ws_buf, VW_WSBUF); free(w->ws_buf); }
    wipe(w->key, sizeof w->key);
    wipe(w->token, sizeof w->token);
    return 0;
}

/* снимок для --livetest: без токенов и ключей */
void voice_stream_info(VStreamInfo *o) {
    memset(o, 0, sizeof *o);
    for (int i = 0; i < 40; i++) o->ops[i] = atomic_load(&vs_ops[i]);
    VW *w = VSt.w;
    o->running = VSt.thr != NULL;
    if (!w || !VSt.thr) return;
    o->session_ready = w->session_ready;
    o->have_vssrc = w->vssrc != 0;
    o->dave = w->dave != NULL;
    o->dave_on = w->dave_on;
    o->dave_ver = w->dave_ver;
    o->udp = w->udp >= 0;
    o->packets = w->vsent;
    o->dave_ok = w->vdave_ok;
    o->pli = w->vpli;
    o->nack = w->vnack;
    o->rtx = w->vrtx_sent;
    o->dave_fail = w->vdave_fail;
    snprintf(o->host, sizeof o->host, "%s", w->host);
    o->port = w->vport;
    o->curl_rc = w->last_curl;
    o->ws_close = w->ws_close;
    mutex_lock(VSt.mu);
    o->queued = VSt.qn;
    mutex_unlock(VSt.mu);
}
void voice_stream_stop(void) {
    if (!VSt.thr) return;
    atomic_store(&VSt.stop, true);
    wake_signal(VSt.cmd);
    thread_join(VSt.thr);
    VSt.thr = NULL;
    if (VSt.w) { wipe(VSt.w, sizeof *VSt.w); free(VSt.w); VSt.w = NULL; }
    mutex_lock(VSt.mu);
    for (int i = 0; i < VSt.qn; i++) free(VSt.q[i].p);
    VSt.qn = 0;
    wipe(vs_aq, sizeof vs_aq);                   /* звук трансляции — затереть */
    vs_aqn = 0;
    mutex_unlock(VSt.mu);
}

/* STREAM_SERVER_UPDATE: подключиться к серверу трансляции (главный поток) */
bool voice_stream_start(const char *endpoint, const char *token, const char *server_id, u64 channel_id, char *err, size_t errn) {
    if (O.demo) return false;                   /* демо и selftest: сети нет */
    voice_stream_stop();
    if (!VSt.mu) { VSt.mu = mutex_new(); VSt.cmd = wake_new(); }
    VW *w = xcalloc(1, sizeof *w);
    w->stream = true;
    w->stopf = &VSt.stop;
    char *e = NULL;
    unsigned long long sid = server_id ? strtoull(server_id, &e, 10) : 0;
    bool safe = sid && e && !*e && token && *token && strlen(token) < sizeof w->token && V.session[0];
    for (const char *c = token; safe && *c; c++) safe = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '_' || *c == '-';
    if (!safe || !host_ok(endpoint, w->host, sizeof w->host)) {
        free(w);
        snprintf(err, errn, "%s", ui_tr("неверные данные сервера трансляции"));
        return false;
    }
    /* VLESS — как у звонка: только через туннель */
    VlessState vl = vless_state();
    if (vl != VL_OFF) {
        const char *u = vless_proxy_url();
        int sport = 0;
        if (vl != VL_UP || !u || strlen(u) >= sizeof w->proxy ||
            !vless_socks(w->sx_ip, sizeof w->sx_ip, &sport, w->sx_user, sizeof w->sx_user, w->sx_pass, sizeof w->sx_pass)) {
            wipe(w, sizeof *w);
            free(w);
            snprintf(err, errn, "%s", ui_tr("VLESS: туннель не работает - трансляция не подключена"));
            return false;
        }
        snprintf(w->proxy, sizeof w->proxy, "%s", u);
        w->sx_port = sport;
        w->via_socks = true;
    }
    {   /* порт из endpoint ("host:port"); у трансляции он может быть не 443 */
        const char *pc = strchr(endpoint, ':');
        int port = pc ? atoi(pc + 1) : 0;
        w->vport = port > 0 && port < 65536 ? port : 0;
    }
    w->server_id = sid;
    /* группа DAVE трансляции — не канал звонка, а rtc_server_id - 1 (так у Discord-RE/Discord-video-stream:
     * StreamConnection.daveChannelId = BigInt(serverId) - 1n); живой прогон 2: с каналом звонка welcome не применялся */
    (void)channel_id;
    w->channel_id = (u64)sid - 1;
    w->me = S.me_id;
    snprintf(w->session, sizeof w->session, "%s", V.session);
    snprintf(w->token, sizeof w->token, "%s", token);
    atomic_store(&VSt.stop, false);
    VSt.w = w;
    VSt.thr = thread_start(stream_thread, w);
    if (!VSt.thr) { wipe(w, sizeof *w); free(w); VSt.w = NULL; snprintf(err, errn, "%s", ui_tr("не удалось запустить поток")); return false; }
    return true;
}

/* кадр кодера (поток захвата дизайнера): копия в очередь; переполнение — старый не ключевой кадр выбрасывается */
void voice_stream_frame(const u8 *annexb, size_t n, bool keyframe, u32 ts90k) {
    if (!VSt.thr || !n || n > (8u << 20)) return;
    u8 *c = malloc(n);
    if (!c) return;
    memcpy(c, annexb, n);
    mutex_lock(VSt.mu);
    if (VSt.qn == VS_QMAX) {
        int drop = 0;
        for (int i = 0; i < VSt.qn; i++) if (!VSt.q[i].key) { drop = i; break; }
        free(VSt.q[drop].p);
        memmove(VSt.q + drop, VSt.q + drop + 1, (size_t)(VSt.qn - drop - 1) * sizeof VSt.q[0]);
        VSt.qn--;
    }
    VSt.q[VSt.qn].p = c;
    VSt.q[VSt.qn].n = n;
    VSt.q[VSt.qn].key = keyframe;
    VSt.q[VSt.qn].ts = ts90k;
    VSt.qn++;
    mutex_unlock(VSt.mu);
    wake_signal(VSt.cmd);
}

/* selftest: SPS minih264 (без VUI) -> с VUI (bitstream_restriction); повторно — без изменений (VUI уже есть) */
int voice_stream_selftest(void (*check)(bool ok, const char *what)) {
    static const u8 sps[] = { 0x67, 0x42, 0x00, 0x1f, 0xa6, 0x80, 0x50, 0x05, 0xb9 };
    u8 a[400], b[400];
    size_t na = vs_sps_vui(sps, sizeof sps, a, sizeof a);
    size_t nb = na ? vs_sps_vui(a, na, b, sizeof b) : 1;
    bool same_head = na > sizeof sps - 1 && !memcmp(a, sps, 4);
    char w[200];
    snprintf(w, sizeof w, "Go Live: SPS без VUI -> VUI с bitstream_restriction (%zu -> %zu Б), повторно не меняется", sizeof sps, na);
    check(na && nb == 0 && same_head, w);
    /* звук трансляции (без сети и устройства): без потока очередь не растёт; без DAVE кадр не уходит;
     * Opus 48 кГц стерео AUDIO 128 кбит/с — кадр 20 мс помещается в ячейку очереди */
    {
        int q0 = vs_aqn;
        u8 fake[8] = { 1, 2, 3 };
        voice_stream_audio(fake, sizeof fake, 960);
        VW *vw = calloc(1, sizeof *vw);
        long long s0 = vs_asent;
        bool made = vw != NULL;
        if (vw) { vw->session_ready = true; vw->ssrc = 1; vs_send_audio(vw, fake, 3, 0); free(vw); }
        int e = 0, n = -1;
        OpusEncoder *oe = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &e);
        if (oe) {
            opus_encoder_ctl(oe, OPUS_SET_BITRATE(128000));
            int16_t pcm[960 * 2];
            for (int i = 0; i < 960; i++) pcm[2 * i] = pcm[2 * i + 1] = (int16_t)((i * 37) % 6000 - 3000);
            u8 pk[1500];
            n = opus_encode(oe, pcm, 960, pk, sizeof pk);
            opus_encoder_destroy(oe);
        }
        snprintf(w, sizeof w, "звук трансляции: без потока очередь пуста, без DAVE не шлётся, Opus 20 мс стерео = %d Б (<= %zu)", n, sizeof vs_aq[0].p);
        check(vs_aqn == q0 && vs_asent == s0 && made && n > 0 && n <= (int)sizeof vs_aq[0].p, w);
    }
    return 0;
}
