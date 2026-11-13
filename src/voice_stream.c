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
/* op 12 трансляции: max_resolution / max_framerate / max_bitrate - по качеству эфира (share_quality: 720p30 или
 * 1080p30 с Nitro). Нули (качество ещё неизвестно) - как у официального клиента без Nitro: 720p30, 3,5 Мбит/с.
 * Возврат - длина или -1 (буфер мал). */
int voice_stream_op12_json(char *b, size_t n, unsigned assrc, unsigned vssrc, unsigned rtx, int w, int h, int fps, int max_bps) {
    ShareQuality d = share_quality(SHARE_Q_720);
    if (w <= 0 || h <= 0) { w = d.w; h = d.h; }
    if (fps <= 0) fps = d.fps;
    if (max_bps <= 0) max_bps = d.kbps_max * 1000;
    int k = snprintf(b, n, "{\"op\":12,\"d\":{\"audio_ssrc\":%u,\"video_ssrc\":%u,\"rtx_ssrc\":%u,\"streams\":[{\"type\":\"video\","
                     "\"rid\":\"100\",\"ssrc\":%u,\"active\":true,\"quality\":100,\"rtx_ssrc\":%u,\"max_bitrate\":%d,"
                     "\"max_framerate\":%d,\"max_resolution\":{\"type\":\"fixed\",\"width\":%d,\"height\":%d}}]}}",
                     assrc, vssrc, rtx, vssrc, rtx, max_bps, fps, w, h);
    return k > 0 && (size_t)k < n ? k : -1;
}
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
    bool live = false, waiting = false;
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
        else if (!live && !waiting && w->session_ready && w->dave && !w->dave_on) { waiting = true; vs_status(VS_DAVE, NULL); }   /* зрителей ещё нет */
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
/* ======================= зритель чужой трансляции =======================
 * Тот же голосовой шлюз сервера трансляции (identify с server_id = rtc_server_id, video:true), DAVE в группе
 * rtc_server_id - 1, op 15 any:100. Сервер присылает op 12 со SSRC звука и видео автора. Звук (Opus, PT 120)
 * идёт тем же путём, что голос звонка: on_rtp -> DAVE -> декодер источника -> микшер -> своё устройство вывода.
 * Видео (этап 2): ниже, wv_* — сборка кадра, DAVE, декодер, растр окна. Всё живёт, пока открыт просмотр; при
 * закрытии — затирается и освобождается. */
static struct {
    PlatThread *thr;
    atomic_bool stop;
    PlatWake *cmd;
    VW *w;
    atomic_llong apk, vpk;
    PlatMutex *smu;                              /* счётчики видео (vs) */
    GoVideoStats vs;
    atomic_size_t lbytes;                        /* сборщик + буфер кадра после DAVE */
    atomic_bool want_wide;                       /* откат: главный поток переподключит с широким набором кодеков */
    atomic_bool outside;                         /* пришёл видеопоток с PT вне объявленного набора */
} VWa;
/* счётчики видео: 0 собран, 1 выброшен, 2 показан, 3 DAVE, 4 декодер, 5 PLI, 6 «не поддерживается» */
static void vw_stat(VW *w, int k) {
    if (!VWa.smu) return;
    mutex_lock(VWa.smu);
    GoVideoStats *v = &VWa.vs;
    switch (k) {
    case 0: v->frames++; break;
    case 1: v->lost++; break;
    case 2: v->shown++; break;
    case 3: v->dave_fail++; break;
    case 4: v->dec_err++; break;
    case 5: v->pli++; break;
    case 6: v->unsupported = true; break;
    default: break;
    }
    if (w) v->e2ee = w->dave != NULL;           /* NULL — из потока декодера */
    mutex_unlock(VWa.smu);
}
static void vw_size(int fw, int fh) {
    if (!VWa.smu) return;
    mutex_lock(VWa.smu);
    VWa.vs.w = fw; VWa.vs.h = fh; VWa.vs.unsupported = false;
    mutex_unlock(VWa.smu);
}
/* ---------- видео зрителя ----------
 * RTP видео (PT по кодеку: H264 105, VP8 107, H265 103, VP9 109, AV1 101; RTX = PT + 1 — мимо) по SSRC автора ->
 * транспортный AES-GCM rtpsize -> сборщик кадра своего кодека (golive.c) -> DAVE (дешифратор автора, VIDEO; при
 * сессии E2EE кадр без ключа не показывается) -> очередь (до VQ кадров) -> ПОТОК ДЕКОДЕРА (wv_dec_thread:
 * libavcodec/dav1d, однопоточно) -> gvid_put_pic (растр окна, <= 15 к/с). watch_thread (и микшер звука) декодер
 * не ждёт. Отстаём: в очереди > 1 — неопорные кадры пропускаются (AVDISCARD_NONREF); переполнение — очередь
 * сбрасывается, PLI, ждём ключевой. Кодек сменился (другой PT) — новый сборщик и декодер. Область видео скрыта
 * или окна нет — поток декодера остановлен, декодер и буферы освобождены (до нуля), кадры только считаются. */
#define VQ 3
struct WVid {
    H264Asm *as;
    int codec;                                   /* кодек сборщика (VC_*) */
    DAVEDecryptorHandle dd;
    u64 dd_user;                                 /* для кого поставлен ключ */
    u8 *pl; size_t plcap;                        /* кадр после DAVE */
    u32 ssrc;
    bool need_idr, unsup;
    i64 next_pli;
    /* поток декодера */
    PlatThread *thr;
    PlatMutex *mu;
    PlatWake *wk;
    atomic_bool stop;
    struct { u8 *p; size_t n; bool key; int codec; } q[VQ];
    int qn;
    atomic_int fb;                               /* от декодера: 1 — ошибка (нужен ключевой), 2 — не поддерживается */
    atomic_size_t qbytes;
};
static void wv_pli(VW *w) {
    struct WVid *v = w->wv;
    i64 now = now_ms();
    if (!v || !w->wvssrc || w->udp < 0 || !w->session_ready || now < v->next_pli) return;
    if (w->dave && !w->dave_on) return;            /* ключей группы ещё нет — ключевой кадр всё равно не расшифровать */
    v->next_pli = now + 500;
    /* RTCP PSFB (PT 206) FMT 1 PLI: заголовок 8 Б (V=2|FMT, PT, длина 2, SSRC отправителя) открыт (AAD),
     * SSRC источника 4 Б шифруется; хвост — тег 16 Б и суффикс nonce 4 Б, как vs_rtcp_sr у автора */
    unsigned char pkt[8 + 4 + 20], body[4];
    pkt[0] = 0x81; pkt[1] = 206; pkt[2] = 0; pkt[3] = 2;
    pkt[4] = (unsigned char)(w->ssrc >> 24); pkt[5] = (unsigned char)(w->ssrc >> 16);
    pkt[6] = (unsigned char)(w->ssrc >> 8); pkt[7] = (unsigned char)w->ssrc;
    body[0] = (unsigned char)(w->wvssrc >> 24); body[1] = (unsigned char)(w->wvssrc >> 16);
    body[2] = (unsigned char)(w->wvssrc >> 8); body[3] = (unsigned char)w->wvssrc;
    unsigned char iv[12] = {0}, tag[16];
    w->nonce++;
    iv[0] = (unsigned char)(w->nonce >> 24); iv[1] = (unsigned char)(w->nonce >> 16);
    iv[2] = (unsigned char)(w->nonce >> 8); iv[3] = (unsigned char)w->nonce;
    if (!gcm(w->enc, true, w->key, iv, pkt, 8, body, 4, pkt + 8, tag)) return;
    memcpy(pkt + 12, tag, 16);
    memcpy(pkt + 28, iv, 4);
    vudp_send(w, pkt, sizeof pkt);
    vw_stat(w, 5);
}
static void wv_ratchet(VW *w) {
    struct WVid *v = w->wv;
    if (!v || !w->dave || !w->wuser) return;
    if (!v->dd) v->dd = daveDecryptorCreate();
    if (!v->dd) return;
    char u[24];
    snprintf(u, sizeof u, "%llu", (unsigned long long)w->wuser);
    DAVEKeyRatchetHandle r = daveSessionGetKeyRatchet(w->dave, u);
    if (r) { daveDecryptorTransitionToKeyRatchet(v->dd, r); daveKeyRatchetDestroy(r); v->dd_user = w->wuser; }
}
static void wv_pass(VW *w, bool on) { if (w->wv && w->wv->dd) daveDecryptorTransitionToPassthroughMode(w->wv->dd, on); }

/* ---- поток декодера ---- */
static void wq_drop_all(struct WVid *v) {        /* под v->mu */
    for (int i = 0; i < v->qn; i++) { wipe(v->q[i].p, v->q[i].n); free(v->q[i].p); }
    v->qn = 0;
    atomic_store(&v->qbytes, 0);
}
/* ---------- H.264 зрителя: разбор SPS/PPS/заголовков срезов и «1 опорный кадр» ----------
 * Живой замер: FFmpeg держит 6 кадров 720p (8,6 МБ), потому что SPS автора объявляет max_num_ref_frames > 1, хотя
 * WebRTC обычно ссылается только на предыдущий кадр. Наблюдаем поток (поток декодера, до libavcodec): P/I-срезы,
 * num_ref_idx_l0_active = 1, без ref_pic_list_modification, без MMCO (adaptive_ref_pic_marking), без взвешенного
 * предсказания и B-срезов. 60 таких кадров подряд — «взвод»: просим ключевой кадр (PLI) и в его SPS ставим
 * max_num_ref_frames = 1 (список 0 при одном активном опорном и без перестановок — всегда последний опорный кадр,
 * поэтому картинка та же). Любое нарушение условий, ошибка декодера или повреждённый кадр — навсегда для этого
 * потока исходные SPS, новый декодер, PLI.  */
typedef struct {
    bool ok, scal;                               /* разобран; есть матрицы квантования (переписать не берёмся) */
    int id, prof, cons, level, chroma, sep, log2_mfn, poc_type, log2_poc, dpoaz, refs, fmo, vui, br, reorder, maxdec;
    size_t refs_bit, refs_len;                   /* где в RBSP поле max_num_ref_frames (для переписи) */
} H4Sps;
typedef struct { bool ok; int id, sps, cabac, bfpo, nsg, l0def, l1def, wp, wbi, redund; } H4Pps;
enum { H4_OBSERVE, H4_ARMED, H4_ON, H4_OFF };
typedef struct {
    H4Sps sps[4];
    H4Pps pps[8];
    int mode, good;
    u8 *buf; size_t cap;                         /* кадр с переписанным SPS */
} H4St;
static int vbr_se(VBr *r) { unsigned k = vbr_ue(r); return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2); }
static size_t h4_rbsp(const u8 *nal, size_t n, u8 *out, size_t cap) {   /* без заголовка NAL, без 0x03 */
    size_t k = 0;
    int z = 0;
    for (size_t i = 1; i < n && k < cap; i++) {
        u8 b = nal[i];
        if (z >= 2 && b == 3) { z = 0; continue; }
        out[k++] = b;
        z = b == 0 ? z + 1 : 0;
    }
    return k;
}
static void h4_hrd(VBr *r) {
    unsigned c = vbr_ue(r);
    vbr_bits(r, 8);
    for (unsigned i = 0; i <= c && i < 32 && !r->bad; i++) { vbr_ue(r); vbr_ue(r); vbr_bit(r); }
    vbr_bits(r, 20);
}
static bool h4_parse_sps(const u8 *nal, size_t n, H4Sps *s) {
    u8 rb[256];
    size_t rn = h4_rbsp(nal, n < 256 ? n : 256, rb, sizeof rb);
    VBr r = { rb, rn, 0, false };
    memset(s, 0, sizeof *s);
    s->prof = (int)vbr_bits(&r, 8); s->cons = (int)vbr_bits(&r, 8); s->level = (int)vbr_bits(&r, 8);
    s->id = (int)vbr_ue(&r);
    s->chroma = 1;
    int p = s->prof;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        s->chroma = (int)vbr_ue(&r);
        if (s->chroma == 3) s->sep = (int)vbr_bit(&r);
        vbr_ue(&r); vbr_ue(&r); vbr_bit(&r);
        if (vbr_bit(&r)) s->scal = true;         /* матрицы: дальше не разбираем */
    }
    if (s->scal || r.bad || s->id > 3) return false;
    s->log2_mfn = (int)vbr_ue(&r) + 4;
    s->poc_type = (int)vbr_ue(&r);
    if (s->poc_type == 0) s->log2_poc = (int)vbr_ue(&r) + 4;
    else if (s->poc_type == 1) {
        s->dpoaz = (int)vbr_bit(&r); vbr_se(&r); vbr_se(&r);
        unsigned c = vbr_ue(&r);
        for (unsigned i = 0; i < c && i < 256 && !r.bad; i++) vbr_se(&r);
    }
    s->refs_bit = r.pos;
    s->refs = (int)vbr_ue(&r);
    s->refs_len = r.pos - s->refs_bit;
    vbr_bit(&r); vbr_ue(&r); vbr_ue(&r);
    s->fmo = (int)vbr_bit(&r);
    if (!s->fmo) vbr_bit(&r);
    vbr_bit(&r);
    if (vbr_bit(&r)) { vbr_ue(&r); vbr_ue(&r); vbr_ue(&r); vbr_ue(&r); }
    s->reorder = s->maxdec = -1;
    s->vui = (int)vbr_bit(&r);
    if (s->vui) {
        if (vbr_bit(&r)) { if (vbr_bits(&r, 8) == 255) vbr_bits(&r, 32); }
        if (vbr_bit(&r)) vbr_bit(&r);
        if (vbr_bit(&r)) { vbr_bits(&r, 4); if (vbr_bit(&r)) vbr_bits(&r, 24); }
        if (vbr_bit(&r)) { vbr_ue(&r); vbr_ue(&r); }
        if (vbr_bit(&r)) { vbr_bits(&r, 32); vbr_bits(&r, 32); vbr_bit(&r); }
        int nh = (int)vbr_bit(&r); if (nh) h4_hrd(&r);
        int vh = (int)vbr_bit(&r); if (vh) h4_hrd(&r);
        if (nh || vh) vbr_bit(&r);
        vbr_bit(&r);
        s->br = (int)vbr_bit(&r);
        if (s->br) { vbr_bit(&r); vbr_ue(&r); vbr_ue(&r); vbr_ue(&r); vbr_ue(&r); s->reorder = (int)vbr_ue(&r); s->maxdec = (int)vbr_ue(&r); }
    }
    s->ok = !r.bad;
    return s->ok;
}
static bool h4_parse_pps(const u8 *nal, size_t n, H4Pps *p) {
    u8 rb[64];
    size_t rn = h4_rbsp(nal, n < 64 ? n : 64, rb, sizeof rb);
    VBr r = { rb, rn, 0, false };
    memset(p, 0, sizeof *p);
    p->id = (int)vbr_ue(&r); p->sps = (int)vbr_ue(&r);
    p->cabac = (int)vbr_bit(&r); p->bfpo = (int)vbr_bit(&r);
    p->nsg = (int)vbr_ue(&r);
    if (p->nsg) return false;                    /* FMO — не разбираем дальше */
    p->l0def = (int)vbr_ue(&r); p->l1def = (int)vbr_ue(&r);
    p->wp = (int)vbr_bit(&r); p->wbi = (int)vbr_bits(&r, 2);
    vbr_se(&r); vbr_se(&r); vbr_se(&r);          /* pic_init_qp/qs, chroma_qp_index_offset */
    vbr_bit(&r); vbr_bit(&r);
    p->redund = (int)vbr_bit(&r);
    p->ok = !r.bad && p->id < 8 && p->sps < 4;
    return p->ok;
}
/* заголовок среза: true — подходит под «1 опорный кадр» */
static bool h4_slice(H4St *h, const u8 *nal, size_t n) {
    u8 rb[48];
    size_t rn = h4_rbsp(nal, n < 48 ? n : 48, rb, sizeof rb);
    VBr r = { rb, rn, 0, false };
    int nri = (nal[0] >> 5) & 3, idr = (nal[0] & 31) == 5;
    vbr_ue(&r);
    int st = (int)(vbr_ue(&r) % 5);
    unsigned ppsid = vbr_ue(&r);
    if (ppsid >= 8 || !h->pps[ppsid].ok || !h->sps[h->pps[ppsid].sps].ok) return false;
    const H4Pps *p = &h->pps[ppsid];
    const H4Sps *s = &h->sps[p->sps];
    if (s->sep) vbr_bits(&r, 2);
    vbr_bits(&r, s->log2_mfn);
    if (!s->fmo && vbr_bit(&r)) vbr_bit(&r);     /* field_pic_flag / bottom */
    if (idr) vbr_ue(&r);
    if (s->poc_type == 0) { vbr_bits(&r, s->log2_poc); if (p->bfpo) vbr_se(&r); }
    else if (s->poc_type == 1 && !s->dpoaz) { vbr_se(&r); if (p->bfpo) vbr_se(&r); }
    if (p->redund) vbr_ue(&r);
    if (st == 1) vbr_bit(&r);
    int ovr = 0, l0 = st == 2 ? 0 : p->l0def + 1, mod = 0, mmco = 0;
    if (st == 0 || st == 1 || st == 3) {
        ovr = (int)vbr_bit(&r);
        if (ovr) { l0 = (int)vbr_ue(&r) + 1; if (st == 1) vbr_ue(&r); }
    }
    if (st != 2 && st != 4) {                    /* ref_pic_list_modification l0 (и l1 у B) */
        for (int l = 0; l < (st == 1 ? 2 : 1); l++)
            if (vbr_bit(&r)) { mod = 1; for (int g = 0; g < 32 && !r.bad; g++) { unsigned idc = vbr_ue(&r); if (idc == 3) break; vbr_ue(&r); } }
    }
    bool wp = (p->wp && (st == 0 || st == 3)) || (p->wbi == 1 && st == 1);
    if (!wp && nri) {                            /* dec_ref_pic_marking */
        if (idr) { vbr_bit(&r); mmco = (int)vbr_bit(&r); }   /* long_term_reference_flag */
        else mmco = (int)vbr_bit(&r);            /* adaptive_ref_pic_marking_mode_flag */
    }
    
    return !r.bad && (st == 2 || st == 4 || (st == 0 && l0 == 1)) && !mod && !wp && !mmco;
}
/* SPS с max_num_ref_frames = 1 (остальное — бит в бит). 0 — не переписать. */
static size_t h4_sps_refs1(const u8 *nal, size_t n, const H4Sps *s, u8 *out, size_t cap) {
    u8 rb[256], wb[260];
    size_t rn = h4_rbsp(nal, n < 256 ? n : 256, rb, sizeof rb);
    if (!rn) return 0;
    size_t last = rn * 8;                        /* стоп-бит RBSP — последняя единица */
    while (last > 0 && !((rb[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1)) last--;
    if (!last || s->refs_bit + s->refs_len >= last) return 0;
    last--;
    VBr r = { rb, rn, 0, false };
    VBw w = { wb, sizeof wb, 0, false };
    while (r.pos < s->refs_bit) vbw_bit(&w, vbr_bit(&r));
    vbr_bits(&r, (int)s->refs_len);
    vbw_ue(&w, 1);
    while (r.pos < last) vbw_bit(&w, vbr_bit(&r));
    vbw_bit(&w, 1);
    while (w.pos & 7) vbw_bit(&w, 0);
    if (r.bad || w.bad) return 0;
    size_t wn = w.pos / 8, k = 0;
    if (cap < 1 + wn * 3 / 2 + 2) return 0;
    out[k++] = nal[0];
    int z = 0;
    for (size_t i = 0; i < wn; i++) {
        if (z >= 2 && wb[i] <= 3) { out[k++] = 3; z = 0; }
        out[k++] = wb[i];
        z = wb[i] == 0 ? z + 1 : 0;
    }
    return k;
}
/* кадр Annex B (старт-коды 00 00 00 01 от сборщика): разбор, журнал, при H4_ON/H4_ARMED — SPS с 1 опорным.
 * *ok — все срезы подходят; возврат — кадр для декодера (f или h->buf). */
static const u8 *h4_frame(H4St *h, const u8 *f, size_t n, size_t *on, bool *ok, bool *rewrote) {
    *ok = true; *rewrote = false; *on = n;
    bool rw = h->mode == H4_ARMED || h->mode == H4_ON;
    size_t need = n + 64 + 64;
    if (rw && need > h->cap) { free(h->buf); h->buf = malloc(need); h->cap = h->buf ? need : 0; }
    size_t k = 0;
    bool any_sps = false;
    for (size_t i = 0; i + 4 <= n; ) {
        if (f[i] || f[i + 1] || f[i + 2] || f[i + 3] != 1) { i++; continue; }
        size_t s = i + 4, e = s;
        while (e + 4 <= n && !(f[e] == 0 && f[e + 1] == 0 && f[e + 2] == 0 && f[e + 3] == 1)) e++;
        if (e + 4 > n) e = n;
        const u8 *nal = f + s;
        size_t nn = e - s;
        int t = nn ? nal[0] & 31 : 0;
        const u8 *put = nal;
        size_t putn = nn;
        u8 tmp[512];
        if (t == 7 && nn > 4) {
            H4Sps sp;
            bool pk = h4_parse_sps(nal, nn, &sp);
            if (pk) h->sps[sp.id] = sp;
            any_sps = true;
            if (rw && pk && sp.refs > 1 && (!sp.br || sp.maxdec >= 1)) {
                size_t m = h4_sps_refs1(nal, nn, &sp, tmp, sizeof tmp);
                if (m) { put = tmp; putn = m; *rewrote = true; }
            }
        } else if (t == 8 && nn > 1) {
            H4Pps pp;
            bool pk = h4_parse_pps(nal, nn, &pp);
            if (pk) h->pps[pp.id] = pp;
        } else if ((t == 1 || t == 5) && nn > 1) {
            if (!h4_slice(h, nal, nn)) *ok = false;
        }
        if (rw && h->buf && k + 4 + putn <= h->cap) { memcpy(h->buf + k, "\0\0\0\1", 4); memcpy(h->buf + k + 4, put, putn); k += 4 + putn; }
        i = e;
    }
    (void)any_sps;
    if (!rw || !h->buf || !*rewrote) return f;   /* без переписанного SPS кадр как есть */
    memset(h->buf + k, 0, 64);                  /* запас libavcodec */
    *on = k;
    return h->buf;
}
static int wv_dec_thread(void *arg) {
    struct WVid *v = arg;
    VidDec *d = NULL;
    int dc = -1;
    H4St *h4 = calloc(1, sizeof *h4);           /* H.264: разбор параметров и «1 опорный кадр» */
    while (!atomic_load(&v->stop)) {
        Waitable wt = wake_waitable(v->wk);
        plat_wait(&wt, 1, 1000);
        wake_clear(v->wk);
        for (;;) {
            mutex_lock(v->mu);
            if (atomic_load(&v->stop) || !v->qn) { mutex_unlock(v->mu); break; }
            u8 *f = v->q[0].p;
            size_t n = v->q[0].n;
            bool key = v->q[0].key;
            int codec = v->q[0].codec;
            memmove(v->q, v->q + 1, (size_t)(v->qn - 1) * sizeof v->q[0]);
            v->qn--;
            int behind = v->qn;
            atomic_fetch_sub(&v->qbytes, n);
            mutex_unlock(v->mu);
            if (codec != dc || !d) {
                if (codec != dc && h4) { free(h4->buf); memset(h4, 0, sizeof *h4); }   /* новый кодек — наблюдение заново */
                vdec_free(d); d = vdec_new(codec); dc = codec;
            }
            const u8 *df = f;
            size_t dn = n;
            if (codec == VC_H264 && h4) {
                bool sok = true, rw = false;
                df = h4_frame(h4, f, n, &dn, &sok, &rw);
                int mref = 0;
                for (int i = 0; i < 4; i++) if (h4->sps[i].ok && h4->sps[i].refs > mref) mref = h4->sps[i].refs;
                if (!sok && h4->mode != H4_OFF) {   /* поток ссылается шире одного кадра — навсегда как есть */
                    bool was_on = h4->mode == H4_ON;
                    
                    h4->mode = H4_OFF;
                    if (was_on) { vdec_free(d); d = vdec_new(codec); atomic_fetch_or(&v->fb, 1); wipe(f, n); free(f); continue; }
                    df = f; dn = n;
                } else if (h4->mode == H4_OBSERVE && sok) {
                    if (++h4->good >= 60 && mref > 1) { h4->mode = H4_ARMED; atomic_fetch_or(&v->fb, 1);  }
                } else if (h4->mode == H4_ARMED && rw) { h4->mode = H4_ON;  }
            }
            VidPic pic = { 0 };
            int r = d ? vdec_frame(d, df, dn, behind > 0 && !key, &pic) : VD_ERR;
            if (codec == VC_H264 && h4 && h4->mode == H4_ON && (r == VD_ERR || (r == VD_PIC && pic.corrupt))) {
                
                h4->mode = H4_OFF;
                vdec_done(d); vdec_free(d); d = vdec_new(codec);
                atomic_fetch_or(&v->fb, 1);
                wipe(f, n); free(f);
                continue;
            }
            wipe(f, n);
            free(f);
            if (r == VD_UNSUPPORTED) { atomic_fetch_or(&v->fb, 2); vdec_free(d); d = NULL; continue; }
            if (r == VD_ERR) { vw_stat(NULL, 4); atomic_fetch_or(&v->fb, 1); continue; }
            if (r != VD_PIC) continue;
            vw_size(pic.w, pic.h);
            bool put = !behind && !cam_full_user() && gvid_put_pic(&pic);   /* показываем только самый свежий; панель занята камерой — нет */
            vdec_done(d);                        /* своя ссылка на кадр больше не нужна */
            if (put) vw_stat(NULL, 2);
        }
    }
    vdec_free(d);
    if (h4) { free(h4->buf); free(h4); }
    return 0;
}
static void wv_dec_stop(struct WVid *v) {
    if (!v->thr) return;
    atomic_store(&v->stop, true);
    wake_signal(v->wk);
    thread_join(v->thr);
    v->thr = NULL;
    mutex_lock(v->mu);
    wq_drop_all(v);
    mutex_unlock(v->mu);
    mutex_free(v->mu); v->mu = NULL;
    wake_free(v->wk); v->wk = NULL;
}
/* кадр -> очередь декодера; false — очередь переполнена и сброшена (нужен ключевой) */
static bool wv_enqueue(struct WVid *v, const u8 *f, size_t n, bool key, int codec) {
    if (!v->thr) {
        v->mu = mutex_new();
        v->wk = wake_new();
        atomic_store(&v->stop, false);
        if (!v->mu || !v->wk || !(v->thr = thread_start(wv_dec_thread, v))) {
            if (v->mu) mutex_free(v->mu);
            if (v->wk) wake_free(v->wk);
            v->mu = NULL; v->wk = NULL;
            return false;
        }
    }
    u8 *c = malloc(n + 64);                      /* + AV_INPUT_BUFFER_PADDING_SIZE нулей для libavcodec */
    if (!c) return false;
    memcpy(c, f, n);
    memset(c + n, 0, 64);
    bool ok = true;
    mutex_lock(v->mu);
    if (v->qn == VQ) { wq_drop_all(v); ok = key; }   /* не успеваем: всё старое — мимо */
    if (ok) {
        v->q[v->qn].p = c; v->q[v->qn].key = key; v->q[v->qn].codec = codec;
        v->q[v->qn].n = n;
        v->qn++;
        atomic_fetch_add(&v->qbytes, n);
    }
    mutex_unlock(v->mu);
    if (!ok) { wipe(c, n); free(c); return false; }
    wake_signal(v->wk);
    return true;
}
static void wv_drop_decoder(struct WVid *v) {   /* поток декодера, декодер и буферы — до нуля */
    wv_dec_stop(v);
    if (v->pl) { wipe(v->pl, v->plcap); free(v->pl); v->pl = NULL; v->plcap = 0; }
}
static void wv_free(VW *w) {
    struct WVid *v = w->wv;
    if (!v) return;
    wv_drop_decoder(v);
    h264_asm_free(v->as);
    if (v->dd) daveDecryptorDestroy(v->dd);
    wipe(v, sizeof *v);
    free(v);
    w->wv = NULL;
}
static void wv_feedback(VW *w) {                 /* итоги потока декодера */
    struct WVid *v = w->wv;
    int fb = atomic_exchange(&v->fb, 0);
    if (fb & 2) { v->unsup = true; vw_stat(w, 6); }
    if (fb & 1) { v->need_idr = true; wv_pli(w); }
}
static void wv_frame(VW *w, const u8 *f, size_t n, bool idr, bool sps) {
    struct WVid *v = w->wv;
    wv_feedback(w);
    if (v->unsup && !sps) return;
    if (v->need_idr && !idr) { wv_pli(w); return; }
    size_t cap = w->dave && v->dd ? daveDecryptorGetMaxPlaintextByteSize(v->dd, DAVE_MEDIA_TYPE_VIDEO, n) : n;
    if (cap < n) cap = n;
    if (cap > v->plcap) {
        u8 *nb = malloc(cap);
        if (!nb) return;
        if (v->pl) { wipe(v->pl, v->plcap); free(v->pl); }
        v->pl = nb;
        v->plcap = cap;
    }
    const u8 *out = f;
    size_t pn = n;
    if (w->dave) {
        /* сессия E2EE есть: каждый кадр — через дешифратор автора (libdave сам читает открытые диапазоны из
         * хвоста кадра, кодек ему не нужен); открытый кадр примет только сама libdave (переходный режим). */
        if (!v->dd || v->dd_user != w->wuser) wv_ratchet(w);
        size_t wr = 0;
        int drc = v->dd ? (int)daveDecryptorDecrypt(v->dd, DAVE_MEDIA_TYPE_VIDEO, f, n, v->pl, v->plcap, &wr) : -99;
        if (drc != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) {
            vw_stat(w, 3);
            v->need_idr = true;
            wv_pli(w);
            return;
        }
        out = v->pl;
        pn = wr;
    }
    if (!wv_enqueue(v, out, pn, idr, v->codec)) { v->need_idr = true; wv_pli(w); return; }
    if (idr) { v->need_idr = false; v->unsup = false; }
}
static void wv_rtp(VW *w, const unsigned char *b, int len) {
    if (len < 12 + 20 || !w->session_ready) return;
    u32 ssrc = (u32)b[8] << 24 | (u32)b[9] << 16 | (u32)b[10] << 8 | b[11];
    int codec = vc_of_pt(b[1] & 0x7F);
    if (codec < 0) return;
    if (!(watch_codec_set(w->wwide) >> codec & 1)) atomic_store(&VWa.outside, true);   /* не объявляли — признак отката */
    if (!w->wvssrc) w->wvssrc = ssrc;            /* op 12 ещё не пришёл: первый видеопоток сервера трансляции */
    if (ssrc != w->wvssrc) return;
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
    const unsigned char *p = plain + ext_words * 4;
    int pn = end - ext_words * 4;

    if (!w->wv) {
        w->wv = calloc(1, sizeof *w->wv);
        if (!w->wv) return;
        w->wv->need_idr = true;
        w->wv->codec = -1;
    }
    struct WVid *v = w->wv;
    /* видео не видно: не собираем и не декодируем, память — до нуля. Кроме «не поддерживается»: ui тогда растр
     * не заявляет (пишет текст), а ключевые кадры с параметрами всё равно пробуем — вдруг автор сменил формат */
    if ((!gvid_wanted() || cam_full_user()) && !v->unsup) {   /* в панели крупно камера (cam_want CAM_FULL) — как скрыто */
        if (v->thr || v->as || v->pl) { wv_drop_decoder(v); h264_asm_free(v->as); v->as = NULL; }
        v->need_idr = true;
        return;
    }
    if (!v->as || v->ssrc != ssrc || v->codec != codec) {
        if (v->codec != codec) { h264_asm_free(v->as); v->as = NULL; v->unsup = false; vw_size(0, 0); }   /* автор сменил кодек */
        if (!v->as) v->as = vid_asm_new(codec, 0);
        else h264_asm_reset(v->as);
        v->ssrc = ssrc;
        v->codec = codec;
        v->need_idr = true;
        
        wv_pli(w);                               /* начали смотреть (или снова видно, или новый кодек) — ключевой сразу */
    }
    if (!v->as) return;
    int r = h264_asm_push(v->as, (u16)(b[2] << 8 | b[3]), (u32)b[4] << 24 | (u32)b[5] << 16 | (u32)b[6] << 8 | b[7],
                          (b[1] & 0x80) != 0, p, (size_t)pn);
    if (r & H264A_LOST) {  vw_stat(w, 1); v->need_idr = true; wv_pli(w); }
    if (r & H264A_FRAME) {
        size_t fn = 0;
        bool idr = false, sps = false;
        const u8 *f = h264_asm_frame(v->as, &fn, &idr, &sps);
        vw_stat(w, 0);
        wv_frame(w, f, fn, idr, sps);
    }
}

static int watch_thread(void *arg) {
    VW *w = arg;
    t_stream = true;
    t_watch = true;
    thread_audio_priority();
    w->udp = -1;
    socks_reset(&w->sx);
    w->enc = EVP_CIPHER_CTX_new();
    w->dec = EVP_CIPHER_CTX_new();
    char err[128] = "", hm[96], ho[96];
    dev_hints(hm, ho);
    w->au = g_audio_fake ? NULL : audio_open_out(ho[0] ? ho : NULL, err, sizeof err);
    bool proxy_bad = false;
    if (!ws_connect(w, 10, &proxy_bad)) {
        if (!atomic_load(&VWa.stop)) vw_net_error(w, ui_tr("сервер трансляции недоступен"), ui_tr("VLESS: сервер трансляции недоступен через туннель"));
        goto out;
    }
    i64 next_mix = now_ms(), next_gc = 0;
    bool live = false;
    i64 fb_t = 0;                                /* откат набора кодеков (golive_fallback) */
    long long fb_wanted = 0, fb_first = -1;
    int fb_res = 0;
    while (!atomic_load(&VWa.stop)) {
        Waitable wt[4];
        int n = 0;
        if (w->ws_wait) wt[n++] = sockwait_get(w->ws_wait);
        if (w->sx_wait) wt[n++] = sockwait_get(w->sx_wait);
        if (w->udp_wait && w->session_ready) wt[n++] = sockwait_get(w->udp_wait);
        wt[n++] = wake_waitable(VWa.cmd);
        i64 now = now_ms(), t = next_mix;
        if (w->next_hb && w->next_hb < t) t = w->next_hb;
        plat_wait(wt, n, (int)(t > now ? t - now : 0));
        wake_clear(VWa.cmd);
        if (w->sx_wait) {
            sockwait_clear(w->sx_wait);
            if (!socks_alive(&w->sx)) { vw_net_error(w, "", ui_tr("VLESS: туннель для трансляции оборвался")); break; }
        }
        if (w->ws_wait) { sockwait_clear(w->ws_wait); vw_read_ws(w); }
        if (atomic_load(&VWa.stop)) break;
        if (w->ws_dead) { status_set(VS_FAILED, ui_tr("соединение трансляции оборвалось")); break; }
        if (w->udp_wait) {
            sockwait_clear(w->udp_wait);
            unsigned char pk[2048];
            int k;
            for (int g = 0; g < 256 && (k = vudp_recv(w, pk, sizeof pk)) >= 0; g++) {
                if (k < 12 || (pk[0] >> 6) != 2) continue;
                int pt = pk[1] & 0x7F;
                if (pt == 120) { on_rtp(w, pk, k); atomic_fetch_add(&VWa.apk, 1); }
                else if (vc_of_pt(pt) >= 0) { atomic_fetch_add(&VWa.vpk, 1); wv_rtp(w, pk, k); }   /* RTX (PT + 1) — мимо */
            }
            struct WVid *v = w->wv;
            atomic_store(&VWa.lbytes, v ? sizeof *v + h264_asm_bytes(v->as) + v->plcap + atomic_load(&v->qbytes) : 0);
        }
        now = now_ms();
        if (!live && w->session_ready && (!w->dave || w->dave_on)) { live = true; status_set(VS_CONNECTED, NULL); fb_t = now; }   /* ключи DAVE согласованы */
        if (live && fb_res == 0 && now - fb_t >= 500) {   /* откат набора кодеков: считаем только пока видео видно */
            if (gvid_wanted()) fb_wanted += now - fb_t;
            fb_t = now;
            GoVideoStats vs;
            voice_watch_vstats(&vs);
            if (vs.frames > 0 && fb_first < 0) fb_first = fb_wanted;
            WatchFbIn fi = { w->wwide, atomic_load(&VWa.outside) && fb_wanted > 0, atomic_load(&VWa.vpk), vs.frames, vs.shown, fb_wanted, fb_first };
            fb_res = golive_fallback(&fi);
            if (fb_res == 1) {  atomic_store(&VWa.want_wide, true); wake_signal(g_wake); }
            if (fb_res == 2) {                   /* и широкий набор не дал картинки: понятная ошибка, звук идёт */
                
                vw_stat(w, 6);
            }
        }
        /* такт микшера: 10 мс, пока есть источники звука; без них — 40 мс (устройство держит ~60 мс тишины), без
         * устройства — раз в секунду. Пришёл первый пакет звука — поток и так проснётся по UDP, такт догонит */
        if (now >= next_mix) { mix_out(w); next_mix = now + (!w->au ? 1000 : w->nsrc ? 10 : 40); }
        if (now >= next_gc) {
            next_gc = now + 1000;
            for (int i = w->nsrc - 1; i >= 0; i--)
                if (now - w->src[i].last_pkt > 30000) { src_free(&w->src[i]); w->src[i] = w->src[--w->nsrc]; }
        }
        if (w->next_hb && now >= w->next_hb) {
            if (!w->hb_acked && w->hb_interval) { status_set(VS_FAILED, ui_tr("сервер трансляции не отвечает")); break; }
            vw_heartbeat(w);
            w->next_hb = now + (w->hb_interval ? w->hb_interval : 15000);
        }
        if (w->session_ready && w->udp >= 0 && now >= w->next_ka) {
            unsigned char ka[2] = { 0x13, 0x37 };
            vudp_send(w, ka, 2);
            w->next_ka = now + 5000;
        }
    }
out:
    for (int i = 0; i < w->nsrc; i++) src_free(&w->src[i]);
    wv_free(w);
    atomic_store(&VWa.lbytes, 0);
    if (w->dave) daveSessionDestroy(w->dave);
    if (w->enc) EVP_CIPHER_CTX_free(w->enc);
    if (w->dec) EVP_CIPHER_CTX_free(w->dec);
    if (w->ws_wait) sockwait_free(w->ws_wait);
    if (w->udp_wait) sockwait_free(w->udp_wait);
    if (w->sx_wait) sockwait_free(w->sx_wait);
    if (w->ws) curl_easy_cleanup(w->ws);
    udp_close(w->udp);
    socks_close(&w->sx);
    if (w->au) audio_close(w->au);
    buf_free(&w->in);
    if (w->mix_acc) { wipe(w->mix_acc, 4800 * 2 * sizeof(int32_t)); free(w->mix_acc); }
    if (w->mix_out) { wipe(w->mix_out, 4800 * 2 * sizeof(int16_t)); free(w->mix_out); }
    if (w->pcm) { wipe(w->pcm, 5760 * 2 * sizeof(int16_t)); free(w->pcm); }
    if (w->ws_buf) { wipe(w->ws_buf, VW_WSBUF); free(w->ws_buf); }
    wipe(w->key, sizeof w->key);
    wipe(w->token, sizeof w->token);
    json_done();
    return 0;
}

void voice_watch_counts(long long *apkts, long long *vpkts) { *apkts = atomic_load(&VWa.apk); *vpkts = atomic_load(&VWa.vpk); }
void voice_watch_vstats(GoVideoStats *o) {
    memset(o, 0, sizeof *o);
    if (VWa.smu) { mutex_lock(VWa.smu); *o = VWa.vs; mutex_unlock(VWa.smu); }
    o->bytes = atomic_load(&VWa.lbytes) + vdec_bytes() + gvid_bytes();
}

void voice_watch_stop(void) {
    if (!VWa.thr) return;
    atomic_store(&VWa.stop, true);
    wake_signal(VWa.cmd);
    thread_join(VWa.thr);
    VWa.thr = NULL;
    if (VWa.w) { wipe(VWa.w, sizeof *VWa.w); free(VWa.w); VWa.w = NULL; }
    gvid_close();                                /* растр окна — затереть и освободить */
}

/* STREAM_SERVER_UPDATE для трансляции, которую смотрим (главный поток) */
bool voice_watch_take_wide(void) { return atomic_exchange(&VWa.want_wide, false); }
bool voice_watch_start(const char *endpoint, const char *token, const char *server_id, u64 author, bool wide, char *err, size_t errn) {
    if (O.demo) return false;
    voice_watch_stop();
    if (!VWa.cmd) VWa.cmd = wake_new();
    if (!VWa.smu) VWa.smu = mutex_new();
    mutex_lock(VWa.smu);
    memset(&VWa.vs, 0, sizeof VWa.vs);
    mutex_unlock(VWa.smu);
    gvid_open();
    VW *w = xcalloc(1, sizeof *w);
    w->stream = w->watch = true;
    w->stopf = &VWa.stop;
    w->wauthor = w->wuser = author;              /* ключ DAVE автора можно ставить и до op 12 */
    w->wwide = wide;
    atomic_store(&VWa.want_wide, false);
    atomic_store(&VWa.outside, false);
    char *e = NULL;
    unsigned long long sid = server_id ? strtoull(server_id, &e, 10) : 0;
    bool safe = sid && e && !*e && token && *token && strlen(token) < sizeof w->token && V.session[0];
    for (const char *c = token; safe && *c; c++) safe = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '_' || *c == '-';
    if (!safe || !host_ok(endpoint, w->host, sizeof w->host)) {
        free(w);
        snprintf(err, errn, "%s", ui_tr("неверные данные сервера трансляции"));
        return false;
    }
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
    {
        const char *pc = strchr(endpoint, ':');
        int port = pc ? atoi(pc + 1) : 0;
        w->vport = port > 0 && port < 65536 ? port : 0;
    }
    w->server_id = sid;
    w->channel_id = (u64)sid - 1;               /* группа DAVE трансляции, как у автора */
    w->me = S.me_id;
    snprintf(w->session, sizeof w->session, "%s", V.session);
    snprintf(w->token, sizeof w->token, "%s", token);
    atomic_store(&VWa.stop, false);
    atomic_store(&VWa.apk, 0);
    atomic_store(&VWa.vpk, 0);
    VWa.w = w;
    VWa.thr = thread_start(watch_thread, w);
    if (!VWa.thr) { wipe(w, sizeof *w); free(w); VWa.w = NULL; snprintf(err, errn, "%s", ui_tr("не удалось запустить поток")); return false; }
    return true;
}

int voice_stream_selftest(void (*check)(bool ok, const char *what)) {
    {   /* H.264 зрителя: SPS High с 4 опорными и VUI (bitstream_restriction) -> max_num_ref_frames = 1, прочее то же */
        u8 wb[64], nal[96];
        VBw w = { wb, sizeof wb, 0, false };
        vbw_bits(&w, 100, 8); vbw_bits(&w, 0, 8); vbw_bits(&w, 31, 8); vbw_ue(&w, 0);
        vbw_ue(&w, 1); vbw_ue(&w, 0); vbw_ue(&w, 0); vbw_bit(&w, 0); vbw_bit(&w, 0);   /* chroma 4:2:0, 8 бит, без матриц */
        vbw_ue(&w, 0); vbw_ue(&w, 2); vbw_ue(&w, 4); vbw_bit(&w, 0);                  /* log2_mfn 4, poc 2, refs 4 */
        vbw_ue(&w, 79); vbw_ue(&w, 44); vbw_bit(&w, 1); vbw_bit(&w, 1); vbw_bit(&w, 0); /* 1280x720, frame_mbs_only */
        vbw_bit(&w, 1); vbw_bits(&w, 0, 8);                                            /* VUI: всё нет, кроме br */
        vbw_bit(&w, 1); vbw_bit(&w, 1); vbw_ue(&w, 2); vbw_ue(&w, 1); vbw_ue(&w, 16); vbw_ue(&w, 16); vbw_ue(&w, 0); vbw_ue(&w, 4);
        vbw_bit(&w, 1);
        while (w.pos & 7) vbw_bit(&w, 0);
        size_t k = 0;
        nal[k++] = 0x67;
        for (size_t i = 0; i < w.pos / 8; i++) nal[k++] = wb[i];
        H4Sps a, b;
        u8 out[128];
        bool pa = h4_parse_sps(nal, k, &a);
        size_t m = pa ? h4_sps_refs1(nal, k, &a, out, sizeof out) : 0;
        bool pb = m && h4_parse_sps(out, m, &b);
        check(pa && a.refs == 4 && a.br && a.maxdec == 4 && pb && b.refs == 1 && b.prof == 100 && b.level == 31 && b.log2_mfn == 4
              && b.poc_type == 2 && b.br && b.reorder == 0 && b.maxdec == 4 && b.vui,
              "Go Live зритель: SPS H.264 High, 4 опорных -> max_num_ref_frames 1, остальные поля (VUI, bitstream_restriction) те же");
    }
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
