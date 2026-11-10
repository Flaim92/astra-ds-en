/* voice_cam.c — камера в ОСНОВНОМ голосовом соединении («Astra DS Video»). Включается в voice.c (#include, как
 * voice_stream.c) только при -DASTRA_VIDEO: отдельно не компилируется. Договор с интерфейсом — cam.h.
 *
 * Отправка (как «Включить камеру» у официального клиента, не Go Live): identify основного соединения — video:true,
 * streams [video rid 100]; op 2 READY даёт ssrc/rtx_ssrc видео; op 1 — opus 120 + H264 105/106 (кодируем и
 * декодируем) + VP8 107/108 (только приём); op 4 video_codec должен быть H264. Камера включена — op 12
 * {audio_ssrc, video_ssrc, rtx_ssrc, streams[{video, rid 100, active, quality 100, max_bitrate, max_framerate,
 * max_resolution fixed}]}, выключена — тот же op 12 с video_ssrc 0 и active:false; на основном шлюзе — op 4
 * self_video. Кадр кодера (cam.c) -> очередь -> поток звонка: длинные старт-коды + SPS с VUI bitstream_restriction
 * (vs_long_startcodes, «Прогон 11») -> DAVE (шифратор звонка, video_ssrc назначен на H264) -> FU-A/STAP-A
 * (h264_packetize, MTU 1100) -> RTP PT 105 с расширением playout-delay -> AES-GCM rtpsize. PLI/FIR -> ключевой кадр,
 * NACK -> RTX (PT 106, своя история 256 пакетов). RTCP SR раз в секунду. Без DAVE с ключами видео не уходит.
 *
 * Приём: сервер шлёт op 12 с видео каждого участника (user_id, video_ssrc, streams[] — слои rid/quality/ssrc).
 * Что присылать, решает op 15 media sink wants: {ssrc: качество 0..100}; видимая плитка (g_cam_area) — выбранный
 * слой 100 (маленькая плитка и есть слой с quality <= 50 — он), остальные 0. Невидимые не собираются и не
 * декодируются (память до нуля). Видимая: RTP -> сборщик своего кодека (golive.c) -> DAVE (дешифратор участника)
 * -> очередь -> один поток декодера на все камеры (viddec.c) -> gcam_put (gui_grid.c: кадр сразу уменьшается до
 * размера плитки). */
#include "cam.h"

bool g_gw_self_video;                            /* net.c gw_voice_state: self_video в op 4 основного шлюза */
void cam_request_idr(void);                      /* cam.c */
bool gcam_want(u64 user, int *pw, int *ph);      /* gui_grid.c */
bool gcam_put(u64 user, const u8 *y, const u8 *u, const u8 *v, int ys, int uvs, int w, int h, int bits);
void gcam_drop(u64 user);
bool gcam_shown(u64 user);

/* ======================= отправка ======================= */
#define VC_QMAX 4
static struct {
    PlatMutex *mu;                               /* очередь и параметры */
    struct { u8 *p; size_t n; bool key; u32 ts; } q[VC_QMAX];
    int qn;
    int w, h, fps, kbps_max;
    atomic_bool on;                              /* камера даёт кадры (cam.c) */
    atomic_int gen;                              /* смена on/параметров: поток звонка шлёт op 12 заново */
    atomic_bool ready;                           /* эфир возможен прямо сейчас */
    atomic_bool gw_dirty;                        /* главный поток: op 4 на основном шлюзе (self_video) */
    atomic_bool codec_bad;                       /* сервер выбрал для видео не H264 */
} VC;
static void vc_init(void) { if (!VC.mu) VC.mu = mutex_new(); }
static void vc_drop_queue(void) {                /* под VC.mu */
    for (int i = 0; i < VC.qn; i++) { wipe(VC.q[i].p, VC.q[i].n); free(VC.q[i].p); }
    VC.qn = 0;
}
bool voice_cam_ready(void) { return atomic_load(&VC.ready); }
/* звонок есть: голосовой поток работает и не упал */
bool voice_cam_call_ok(char *err, size_t n) {
    if (!V.mu) { snprintf(err, n, "%s", ui_tr("камера: сначала войдите в голосовой канал или звонок")); return false; }
    mutex_lock(V.mu);
    VoicePhase ph = V.st.phase;
    mutex_unlock(V.mu);
    if (ph == VS_IDLE || !V.thr) { snprintf(err, n, "%s", ui_tr("камера: сначала войдите в голосовой канал или звонок")); return false; }
    if (ph == VS_FAILED) { snprintf(err, n, "%s", ui_tr("камера: звонок не подключён")); return false; }
    return true;
}
void voice_cam_enable(bool on, int w, int h, int fps, int kbps_max) {
    vc_init();
    mutex_lock(VC.mu);
    if (on) { VC.w = w; VC.h = h; VC.fps = fps; VC.kbps_max = kbps_max; }
    else vc_drop_queue();
    mutex_unlock(VC.mu);
    atomic_store(&VC.on, on);
    if (!on) atomic_store(&VC.ready, false);
    atomic_fetch_add(&VC.gen, 1);
    atomic_store(&VC.gw_dirty, true);
    if (V.cmd) wake_signal(V.cmd);
    wake_signal(g_wake);
}
void voice_cam_frame(const u8 *annexb, size_t n, bool keyframe, u32 ts90k) {
    if (!n || n > (8u << 20) || !atomic_load(&VC.ready)) return;
    u8 *c = malloc(n);
    if (!c) return;
    memcpy(c, annexb, n);
    mutex_lock(VC.mu);
    if (VC.qn == VC_QMAX) {                      /* поток звонка не успевает: выбросить старый неключевой */
        int drop = 0;
        for (int i = 0; i < VC.qn; i++) if (!VC.q[i].key) { drop = i; break; }
        wipe(VC.q[drop].p, VC.q[drop].n); free(VC.q[drop].p);
        memmove(VC.q + drop, VC.q + drop + 1, (size_t)(VC.qn - drop - 1) * sizeof VC.q[0]);
        VC.qn--;
        cam_request_idr();
    }
    VC.q[VC.qn].p = c; VC.q[VC.qn].n = n; VC.q[VC.qn].key = keyframe; VC.q[VC.qn].ts = ts90k;
    VC.qn++;
    mutex_unlock(VC.mu);
    if (V.cmd) wake_signal(V.cmd);
}
/* главный поток (voice_main_chores): self_video на основном шлюзе */
static void cam_main_chores(void) {
    if (!atomic_exchange(&VC.gw_dirty, false)) return;
    bool on = atomic_load(&VC.on);
    if (on == g_gw_self_video) return;
    g_gw_self_video = on;
    resend_state();
}

/* op 12 своей камеры: on — эфир, иначе выключение (video_ssrc 0, active false). Возврат — длина или -1. */
static int cam_op12_json(char *b, size_t n, u32 assrc, u32 vssrc, u32 rtx, bool on, int w, int h, int fps, int maxbr) {
    int k = snprintf(b, n, "{\"op\":12,\"d\":{\"audio_ssrc\":%u,\"video_ssrc\":%u,\"rtx_ssrc\":%u,\"streams\":[{\"type\":\"video\","
                     "\"rid\":\"100\",\"ssrc\":%u,\"active\":%s,\"quality\":100,\"rtx_ssrc\":%u,\"max_bitrate\":%d,"
                     "\"max_framerate\":%d,\"max_resolution\":{\"type\":\"fixed\",\"width\":%d,\"height\":%d}}]}}",
                     (unsigned)assrc, on ? (unsigned)vssrc : 0u, on ? (unsigned)rtx : 0u, (unsigned)vssrc, on ? "true" : "false",
                     (unsigned)rtx, maxbr, fps, w, h);
    return k > 0 && (size_t)k < n ? k : -1;
}
static void cam_send_op12(VW *w) {
    mutex_lock(VC.mu);
    int vw = VC.w ? VC.w : 1280, vh = VC.h ? VC.h : 720, vf = VC.fps ? VC.fps : 30, br = VC.kbps_max ? VC.kbps_max * 1000 : 2500000;
    mutex_unlock(VC.mu);
    bool on = atomic_load(&VC.on);
    char b[640];
    if (cam_op12_json(b, sizeof b, w->ssrc, w->vssrc, w->vrtx, on, vw, vh, vf, br) > 0) ws_text(w, b);
    w->cam_op12 = on;
}

/* RTP своей камеры: своя история для NACK -> RTX (у трансляции — своя, vs_hist) */
#define CAM_HIST 256
typedef struct { u16 seq; u32 ts; bool marker, used; u16 len; u8 p[1300]; } CamHist;
static void cam_rtp(void *u, const u8 *p, size_t len, bool marker) {
    VW *w = u;
    if (w->udp < 0 || !w->session_ready || len > 1300) return;
    unsigned char pkt[16 + 4 + 1400 + 20];
    size_t k = vs_rtp_build(w, VS_PT_H264, w->vssrc, w->vseq, w->vts, marker, p, len, pkt);
    if (!k) return;
    vudp_send(w, pkt, k);
    if (!w->chist) w->chist = calloc(CAM_HIST, sizeof(CamHist));
    CamHist *hs = w->chist;
    if (hs) {
        int i = w->vseq % CAM_HIST;
        hs[i].seq = w->vseq; hs[i].ts = w->vts; hs[i].marker = marker; hs[i].used = true;
        hs[i].len = (u16)len; memcpy(hs[i].p, p, len);
    }
    w->vseq++;
    w->vsent++;
    w->vbytes += k;
}
static bool cam_rtx(VW *w, u16 seq) {
    CamHist *hs = w->chist;
    if (!hs || !w->vrtx) return false;
    int i = seq % CAM_HIST;
    if (!hs[i].used || hs[i].seq != seq) return false;
    u8 pl[2 + 1300], pkt[16 + 4 + 1400 + 20];
    pl[0] = (u8)(seq >> 8); pl[1] = (u8)seq;
    memcpy(pl + 2, hs[i].p, hs[i].len);
    size_t k = vs_rtp_build(w, VS_PT_RTX, w->vrtx, w->vrtx_seq, hs[i].ts, hs[i].marker, pl, (size_t)hs[i].len + 2, pkt);
    if (!k) return false;
    vudp_send(w, pkt, k);
    w->vrtx_seq++;
    w->vrtx_sent++;
    return true;
}
/* входящий RTCP основного соединения: PLI/FIR по нашему video_ssrc -> ключевой кадр, NACK -> RTX */
static void cam_rtcp_in(VW *w, const u8 *b, int n) {
    if (!w->vssrc || !atomic_load(&VC.on) || n < 8 + 16 + 4) return;
    int cn = n - 8 - 16 - 4;
    u8 full[1500];
    if (cn <= 0 || 8 + cn > (int)sizeof full) return;
    unsigned char iv[12] = {0}, tag[16];
    memcpy(iv, b + n - 4, 4);
    memcpy(tag, b + n - 20, 16);
    memcpy(full, b, 8);
    if (!gcm(w->dec, false, w->key, iv, b, 8, b + 8, cn, full + 8, tag)) return;
    const u8 *p = full;
    int len_all = 8 + cn;
    for (int off = 0; off + 12 <= len_all; ) {
        int fmt = p[off] & 31, pt = p[off + 1], len = ((p[off + 2] << 8) | p[off + 3]) * 4 + 4;
        if (len < 12 || off + len > len_all) break;
        u32 media = (u32)p[off + 8] << 24 | (u32)p[off + 9] << 16 | (u32)p[off + 10] << 8 | p[off + 11];
        if (pt == 206 && (fmt == 1 || fmt == 4) && (media == w->vssrc || fmt == 4)) { w->vpli++; cam_request_idr(); }
        else if (pt == 205 && fmt == 1 && media == w->vssrc) {
            for (int q = off + 12; q + 4 <= off + len; q += 4) {
                u16 pid = (u16)(p[q] << 8 | p[q + 1]), blp = (u16)(p[q + 2] << 8 | p[q + 3]);
                w->vnack++;
                cam_rtx(w, pid);
                for (int bit = 0; bit < 16; bit++) if (blp & (1 << bit)) { w->vnack++; cam_rtx(w, (u16)(pid + bit + 1)); }
            }
        }
        off += len;
    }
}
/* кадр -> длинные старт-коды и VUI в SPS -> DAVE -> RTP (только под DAVE, как Go Live: GL-1) */
static void cam_send_frame(VW *w, const u8 *f0, size_t n0, u32 ts) {
    if (!w->session_ready || !w->vssrc || !w->dave || !w->dave_on) return;
    size_t n = 0;
    u8 *norm = vs_long_startcodes(f0, n0, &n);
    if (!norm || !n) { free(norm); return; }
    size_t cap = daveEncryptorGetMaxCiphertextByteSize(w->denc, DAVE_MEDIA_TYPE_VIDEO, n);
    u8 *enc = malloc(cap);
    size_t wr = 0;
    int rc = enc ? (int)daveEncryptorEncrypt(w->denc, DAVE_MEDIA_TYPE_VIDEO, w->vssrc, norm, n, enc, cap, &wr) : -1;
    if (rc == DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) {
        w->vdave_ok++;
        w->vts = ts;
        h264_packetize(enc, wr, VS_MTU, cam_rtp, w);
    } else { w->vdave_fail++; cam_request_idr(); }
    if (enc) { wipe(enc, cap); free(enc); }
    wipe(norm, n);
    free(norm);
}

/* ======================= приём ======================= */
#define CRX_MAX 16
#define CRX_L 3                                  /* слоёв (simulcast) на участника */
#define CRX_Q 2                                  /* кадров в очереди декодера на участника */
typedef struct {
    u64 user;
    int nl;
    struct { u32 ssrc; int q; bool act; int w, h; int sent; } l[CRX_L];   /* sent — качество в последнем op 15 */
    /* поток звонка */
    u32 cur;                                     /* слой, который собираем */
    H264Asm *as;
    int codec;
    DAVEDecryptorHandle dd;
    u8 *pl; size_t plcap;
    bool need_idr, unsup, wanted;
    int px;                                      /* площадь, которую рисуем (pixelCounts в op 15), точек */
    i64 next_pli;
    /* под CR.mu: счётчики, очередь, декодер (поток декодера) */
    long long frames, lost;
    int fw, fh;
    bool e2ee, dead, drop_dec;
    struct { u8 *p; size_t n; bool key; int codec; } q[CRX_Q];
    int qn;
    VidDec *dec;
    int dec_codec;
    H4St *h4;
    atomic_int fb;                               /* от декодера: 1 — нужен ключевой, 2 — не поддерживается */
} CamRx;
static struct {
    PlatMutex *mu;                               /* список, очереди, счётчики */
    PlatThread *thr;
    PlatWake *wk;
    atomic_bool stop;
    CamRx *s[CRX_MAX];
    int n;
    CamRx *busy;                                 /* поток декодера сейчас работает с этим (под mu) */
    int rr;                                      /* по кругу */
    atomic_size_t bytes;                         /* сборка + очереди (декодер — vdec_bytes, растр — gcam_bytes) */
    bool wants_dirty;                            /* поток звонка: пора слать op 15 */
} CR;
static void crx_init(void) { if (!CR.mu) CR.mu = mutex_new(); }
static void crx_q_drop(CamRx *c) {               /* под mu */
    for (int i = 0; i < c->qn; i++) { wipe(c->q[i].p, c->q[i].n); free(c->q[i].p); }
    c->qn = 0;
}
static void crx_dec_free(CamRx *c) {             /* под mu, когда поток декодера с ним не работает */
    vdec_free(c->dec); c->dec = NULL; c->dec_codec = -1;
    if (c->h4) { free(c->h4->buf); free(c->h4); c->h4 = NULL; }
    c->drop_dec = false;
}
static void crx_free_rx(CamRx *c) {              /* поток звонка: сборка и буфер DAVE — до нуля */
    h264_asm_free(c->as); c->as = NULL;
    if (c->pl) { wipe(c->pl, c->plcap); free(c->pl); c->pl = NULL; c->plcap = 0; }
    c->cur = 0;
    c->need_idr = true;
}
static void crx_slot_free(CamRx *c) {            /* под mu */
    crx_free_rx(c);
    crx_q_drop(c);
    if (CR.busy == c) { c->dead = true; return; }   /* поток декодера доделает и освободит сам */
    crx_dec_free(c);
    if (c->dd) daveDecryptorDestroy(c->dd);
    u64 u = c->user;
    wipe(c, sizeof *c);
    free(c);
    gcam_drop(u);
}
static CamRx *crx_find(u64 user) { for (int i = 0; i < CR.n; i++) if (CR.s[i]->user == user) return CR.s[i]; return NULL; }
static void crx_remove_user(VW *w, u64 user) {
    (void)w;
    if (!CR.mu) return;
    mutex_lock(CR.mu);
    for (int i = 0; i < CR.n; i++) if (CR.s[i]->user == user) { crx_slot_free(CR.s[i]); CR.s[i] = CR.s[--CR.n]; CR.wants_dirty = true; break; }
    mutex_unlock(CR.mu);
}
static void crx_ratchet(VW *w, CamRx *c) {
    if (!w->dave || !c->user) return;
    if (!c->dd) c->dd = daveDecryptorCreate();
    if (!c->dd) return;
    char u[24];
    snprintf(u, sizeof u, "%llu", (unsigned long long)c->user);
    DAVEKeyRatchetHandle r = daveSessionGetKeyRatchet(w->dave, u);
    if (r) { daveDecryptorTransitionToKeyRatchet(c->dd, r); daveKeyRatchetDestroy(r); }
}
static void crx_ratchet_all(VW *w) { if (!CR.mu) return; for (int i = 0; i < CR.n; i++) crx_ratchet(w, CR.s[i]); }
static void crx_pass(VW *w, bool on) { (void)w; if (!CR.mu) return; for (int i = 0; i < CR.n; i++) if (CR.s[i]->dd) daveDecryptorTransitionToPassthroughMode(CR.s[i]->dd, on); }

/* разбор op 12 участника (без сети, проверяется selftest) */
typedef struct { u64 user; u32 assrc, vssrc; int nl; struct { u32 ssrc; int q; bool act; int w, h; } l[CRX_L]; bool video; } CamOp12;
static bool cam_parse_op12(const cJSON *d, CamOp12 *o) {
    memset(o, 0, sizeof *o);
    o->user = jid(d, "user_id");
    if (!o->user) return false;
    o->assrc = (u32)jint(d, "audio_ssrc", 0);
    o->vssrc = (u32)jint(d, "video_ssrc", 0);
    const cJSON *s;
    cJSON_ArrayForEach(s, jget(d, "streams")) {
        if (o->nl >= CRX_L) break;
        u32 ss = (u32)jint(s, "ssrc", 0);
        if (!ss) continue;
        const char *ty = jstr(s, "type");
        if (ty && strcmp(ty, "video") && strcmp(ty, "screen")) continue;
        o->l[o->nl].ssrc = ss;
        o->l[o->nl].q = (int)jint(s, "quality", 100);
        o->l[o->nl].act = jbool(s, "active");
        const cJSON *mr = jget(s, "max_resolution");
        o->l[o->nl].w = (int)jint(mr, "width", 0);
        o->l[o->nl].h = (int)jint(mr, "height", 0);
        o->nl++;
    }
    if (!o->nl && o->vssrc) { o->l[0].ssrc = o->vssrc; o->l[0].q = 100; o->l[0].act = true; o->nl = 1; }   /* старый вид: без streams */
    bool any = false;
    for (int i = 0; i < o->nl; i++) if (o->l[i].act) any = true;
    o->video = o->vssrc != 0 && any;
    return true;
}
static void crx_op12(VW *w, const cJSON *d) {
    CamOp12 o;
    if (!cam_parse_op12(d, &o) || o.user == w->me) return;
    crx_init();
    mutex_lock(CR.mu);
    CamRx *c = crx_find(o.user);
    if (!o.video) {                              /* камеру выключили — всё до нуля */
        if (c) { for (int i = 0; i < CR.n; i++) if (CR.s[i] == c) { crx_slot_free(c); CR.s[i] = CR.s[--CR.n]; break; } CR.wants_dirty = true; }
        mutex_unlock(CR.mu);
        wake_signal(g_wake);
        return;
    }
    if (!c && CR.n < CRX_MAX) {
        c = calloc(1, sizeof *c);
        if (c) { c->user = o.user; c->codec = -1; c->dec_codec = -1; c->need_idr = true; CR.s[CR.n++] = c; }
    }
    if (c) {
        int old[CRX_L] = { 0 };
        u32 olds[CRX_L] = { 0 };
        for (int i = 0; i < c->nl; i++) { old[i] = c->l[i].sent; olds[i] = c->l[i].ssrc; }
        c->nl = o.nl;
        for (int i = 0; i < o.nl; i++) {
            c->l[i].ssrc = o.l[i].ssrc; c->l[i].q = o.l[i].q; c->l[i].act = o.l[i].act; c->l[i].w = o.l[i].w; c->l[i].h = o.l[i].h;
            c->l[i].sent = -1;
            for (int j = 0; j < CRX_L; j++) if (olds[j] == o.l[i].ssrc) c->l[i].sent = old[j];
        }
        bool cur_ok = false;
        for (int i = 0; i < c->nl; i++) if (c->l[i].ssrc == c->cur && c->l[i].act) cur_ok = true;
        if (!cur_ok && c->cur) crx_free_rx(c);
        if (!c->dd) crx_ratchet(w, c);
        dave_add_user(w, o.user);
        CR.wants_dirty = true;
    }
    mutex_unlock(CR.mu);
    wake_signal(g_wake);
}
/* какой слой брать под плитку высотой ph пикселей: самый маленький, чей размер не меньше плитки; размеров нет —
 * по качеству (маленькая плитка < 300 точек — слой quality <= 50, если он есть) */
static int crx_pick(const CamRx *c, int pw, int ph) {
    int best = -1;
    for (int i = 0; i < c->nl; i++) {
        if (!c->l[i].act) continue;
        if (best < 0) { best = i; continue; }
        const int bi = best;
        bool sized = c->l[i].h > 0 && c->l[bi].h > 0;
        if (sized) {
            bool ifit = c->l[i].h >= ph || c->l[i].w >= pw, bfit = c->l[bi].h >= ph || c->l[bi].w >= pw;
            if (ifit && (!bfit || c->l[i].h < c->l[bi].h)) best = i;
            else if (!ifit && !bfit && c->l[i].h > c->l[bi].h) best = i;
        } else {
            bool small = ph < 300;
            if (small ? c->l[i].q < c->l[bi].q : c->l[i].q > c->l[bi].q) best = i;
        }
    }
    return best;
}
/* op 15 по всем известным видеослоям: выбранный — 100, остальные — 0. Без сети (selftest). Длина или -1. */
static int cam_wants_json(char *b, size_t n, CamRx *const *s, int ns) {
    int k = snprintf(b, n, "{\"op\":15,\"d\":{");
    bool first = true;
    for (int i = 0; i < ns && k > 0 && (size_t)k < n; i++)
        for (int j = 0; j < s[i]->nl; j++) {
            int m = snprintf(b + k, n - (size_t)k, "%s\"%u\":%d", first ? "" : ",", (unsigned)s[i]->l[j].ssrc, s[i]->l[j].sent > 0 ? s[i]->l[j].sent : 0);
            if (m < 0 || (size_t)m >= n - (size_t)k) return -1;
            k += m;
            first = false;
        }
    /* pixelCounts — сколько точек этого слоя реально рисуем (как официальный клиент: площадь плитки) */
    int m = k > 0 && (size_t)k < n ? snprintf(b + k, n - (size_t)k, "%s\"pixelCounts\":{", first ? "" : ",") : -1;
    if (m < 0 || (size_t)m >= n - (size_t)k) return -1;
    k += m;
    first = true;
    for (int i = 0; i < ns; i++)
        for (int j = 0; j < s[i]->nl; j++) {
            if (s[i]->l[j].sent <= 0) continue;
            m = snprintf(b + k, n - (size_t)k, "%s\"%u\":%d", first ? "" : ",", (unsigned)s[i]->l[j].ssrc, s[i]->px > 0 ? s[i]->px : 0);
            if (m < 0 || (size_t)m >= n - (size_t)k) return -1;
            k += m;
            first = false;
        }
    m = snprintf(b + k, n - (size_t)k, "}}}");
    return m > 0 && (size_t)(k + m) < n ? k + m : -1;
}
static void crx_pli(VW *w, CamRx *c) {
    i64 now = now_ms();
    if (!c->cur || w->udp < 0 || !w->session_ready || now < c->next_pli) return;
    if (w->dave && !w->dave_on && w->nusers > 1) return;   /* ключей ещё нет — ключевой всё равно не расшифровать */
    c->next_pli = now + 500;
    unsigned char pkt[8 + 4 + 20], body[4];
    pkt[0] = 0x81; pkt[1] = 206; pkt[2] = 0; pkt[3] = 2;
    pkt[4] = (unsigned char)(w->ssrc >> 24); pkt[5] = (unsigned char)(w->ssrc >> 16);
    pkt[6] = (unsigned char)(w->ssrc >> 8); pkt[7] = (unsigned char)w->ssrc;
    body[0] = (unsigned char)(c->cur >> 24); body[1] = (unsigned char)(c->cur >> 16);
    body[2] = (unsigned char)(c->cur >> 8); body[3] = (unsigned char)c->cur;
    unsigned char iv[12] = {0}, tag[16];
    w->nonce++;
    iv[0] = (unsigned char)(w->nonce >> 24); iv[1] = (unsigned char)(w->nonce >> 16);
    iv[2] = (unsigned char)(w->nonce >> 8); iv[3] = (unsigned char)w->nonce;
    if (!gcm(w->enc, true, w->key, iv, pkt, 8, body, 4, pkt + 8, tag)) return;
    memcpy(pkt + 12, tag, 16);
    memcpy(pkt + 28, iv, 4);
    vudp_send(w, pkt, sizeof pkt);
}

/* ---- поток декодера (один на все камеры) ---- */
static int crx_dec_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&CR.stop)) {
        Waitable wt = wake_waitable(CR.wk);
        plat_wait(&wt, 1, 1000);
        wake_clear(CR.wk);
        for (;;) {
            mutex_lock(CR.mu);
            for (int i = 0; i < CR.n; i++) if (CR.s[i]->drop_dec) crx_dec_free(CR.s[i]);   /* плитку скрыли */
            CamRx *c = NULL;
            for (int k = 0; k < CR.n && !c; k++) { CamRx *x = CR.s[(CR.rr + k) % CR.n]; if (x->qn) { c = x; CR.rr = (CR.rr + k + 1) % CR.n; } }
            if (!c || atomic_load(&CR.stop)) { mutex_unlock(CR.mu); break; }
            u8 *f = c->q[0].p;
            size_t n = c->q[0].n;
            bool key = c->q[0].key;
            int codec = c->q[0].codec;
            memmove(c->q, c->q + 1, (size_t)(c->qn - 1) * sizeof c->q[0]);
            c->qn--;
            int behind = c->qn;
            atomic_fetch_sub(&CR.bytes, n);
            CR.busy = c;
            u64 user = c->user;
            mutex_unlock(CR.mu);
            if (codec != c->dec_codec || !c->dec) {
                vdec_free(c->dec);
                c->dec = vdec_new(codec); c->dec_codec = codec;
                if (c->h4) { free(c->h4->buf); free(c->h4); }
                c->h4 = codec == VC_H264 ? calloc(1, sizeof *c->h4) : NULL;
            }
            const u8 *df = f;
            size_t dn = n;
            bool skip = false;
            H4St *h4 = c->h4;
            if (codec == VC_H264 && h4) {        /* «1 опорный кадр», как у зрителя Go Live (wv_dec_thread) */
                bool sok = true, rw = false;
                df = h4_frame(h4, f, n, &dn, &sok, &rw);
                int mref = 0;
                for (int i = 0; i < 4; i++) if (h4->sps[i].ok && h4->sps[i].refs > mref) mref = h4->sps[i].refs;
                if (!sok && h4->mode != H4_OFF) {
                    bool was_on = h4->mode == H4_ON;
                    h4->mode = H4_OFF;
                    if (was_on) { vdec_free(c->dec); c->dec = vdec_new(codec); atomic_fetch_or(&c->fb, 1); skip = true; }
                    df = f; dn = n;
                } else if (h4->mode == H4_OBSERVE && sok) {
                    if (++h4->good >= 60 && mref > 1) { h4->mode = H4_ARMED; atomic_fetch_or(&c->fb, 1); }
                } else if (h4->mode == H4_ARMED && rw) h4->mode = H4_ON;
            }
            VidPic pic = { 0 };
            int r = skip ? VD_NONE : c->dec ? vdec_frame(c->dec, df, dn, behind > 0 && !key, &pic) : VD_ERR;
            if (!skip && codec == VC_H264 && h4 && h4->mode == H4_ON && (r == VD_ERR || (r == VD_PIC && pic.corrupt))) {
                h4->mode = H4_OFF;
                vdec_done(c->dec); vdec_free(c->dec); c->dec = vdec_new(codec);
                atomic_fetch_or(&c->fb, 1);
                r = VD_NONE;
            }
            wipe(f, n);
            free(f);
            if (r == VD_UNSUPPORTED) { atomic_fetch_or(&c->fb, 2); vdec_free(c->dec); c->dec = NULL; }
            else if (r == VD_ERR) atomic_fetch_or(&c->fb, 1);
            else if (r == VD_PIC) {
                if (!behind) {
                    gcam_put(user, pic.p[0], pic.p[1], pic.p[2], pic.s[0], pic.s[1], pic.w, pic.h, pic.bits);
                    if (cam_full_user() == user) gvid_put_pic(&pic);   /* крупный вид — путь трансляции */
                }
                vdec_done(c->dec);
            }
            mutex_lock(CR.mu);
            if (r == VD_PIC) { c->fw = pic.w; c->fh = pic.h; }
            CR.busy = NULL;
            if (c->dead) {                       /* участника убрали, пока декодировали */
                crx_dec_free(c);
                if (c->dd) daveDecryptorDestroy(c->dd);
                wipe(c, sizeof *c);
                free(c);
                gcam_drop(user);
            }
            mutex_unlock(CR.mu);
        }
    }
    mutex_lock(CR.mu);
    for (int i = 0; i < CR.n; i++) crx_dec_free(CR.s[i]);
    mutex_unlock(CR.mu);
    return 0;
}
static bool crx_enqueue(CamRx *c, const u8 *f, size_t n, bool key, int codec) {   /* под mu */
    if (!CR.thr) {
        if (!CR.wk) CR.wk = wake_new();
        atomic_store(&CR.stop, false);
        if (!CR.wk || !(CR.thr = thread_start(crx_dec_thread, NULL))) return false;
    }
    u8 *b = malloc(n + 64);
    if (!b) return false;
    memcpy(b, f, n);
    memset(b + n, 0, 64);
    bool ok = true;
    if (c->qn == CRX_Q) { crx_q_drop(c); ok = key; }   /* декодер не успевает: старое мимо */
    if (!ok) { wipe(b, n); free(b); return false; }
    c->q[c->qn].p = b; c->q[c->qn].n = n; c->q[c->qn].key = key; c->q[c->qn].codec = codec;
    c->qn++;
    atomic_fetch_add(&CR.bytes, n);
    wake_signal(CR.wk);
    return true;
}
/* видеопакет основного соединения */
static void cam_rx_rtp(VW *w, const u8 *b, int len) {
    if (!CR.mu || len < 12 + 20 || !w->session_ready) return;
    u32 ssrc = (u32)b[8] << 24 | (u32)b[9] << 16 | (u32)b[10] << 8 | b[11];
    int codec = vc_of_pt(b[1] & 0x7F);
    if (codec < 0) return;
    mutex_lock(CR.mu);
    CamRx *c = NULL;
    for (int i = 0; i < CR.n && !c; i++) for (int j = 0; j < CR.s[i]->nl; j++) if (CR.s[i]->l[j].ssrc == ssrc) { c = CR.s[i]; break; }
    if (!c || !c->wanted) { mutex_unlock(CR.mu); return; }   /* неизвестный или невидимый — мимо, без расшифровки */
    int aad = 12 + (b[0] & 0x0F) * 4, ext_words = 0;
    if (b[0] & 0x10) {
        if (aad + 4 > len) { mutex_unlock(CR.mu); return; }
        ext_words = b[aad + 2] << 8 | b[aad + 3];
        aad += 4;
    }
    int clen = len - aad - 20;
    unsigned char iv[12] = {0}, tag[16], plain[1500];
    if (clen <= 0 || clen > 1500) { mutex_unlock(CR.mu); return; }
    memcpy(iv, b + len - 4, 4);
    memcpy(tag, b + len - 20, 16);
    if (!gcm(w->dec, false, w->key, iv, b, aad, b + aad, clen, plain, tag)) { mutex_unlock(CR.mu); return; }
    int end = clen;
    if (b[0] & 0x20) { if (!end || !plain[end - 1] || plain[end - 1] > end) { mutex_unlock(CR.mu); return; } end -= plain[end - 1]; }
    if (ext_words * 4 > end) { mutex_unlock(CR.mu); return; }
    const u8 *p = plain + ext_words * 4;
    int pn = end - ext_words * 4;
    int fb = atomic_exchange(&c->fb, 0);
    if (fb & 2) c->unsup = true;
    if (fb & 1) { c->need_idr = true; crx_pli(w, c); }
    if (!c->as || c->cur != ssrc || c->codec != codec) {
        if (c->codec != codec) { h264_asm_free(c->as); c->as = NULL; c->unsup = false; }
        if (!c->as) c->as = vid_asm_new(codec, 0);
        else h264_asm_reset(c->as);
        c->cur = ssrc; c->codec = codec; c->need_idr = true;
        crx_pli(w, c);
    }
    if (!c->as) { mutex_unlock(CR.mu); return; }
    int r = h264_asm_push(c->as, (u16)(b[2] << 8 | b[3]), (u32)b[4] << 24 | (u32)b[5] << 16 | (u32)b[6] << 8 | b[7], (b[1] & 0x80) != 0, p, (size_t)pn);
    if (r & H264A_LOST) { c->lost++; c->need_idr = true; crx_pli(w, c); }
    if (r & H264A_FRAME) {
        size_t fn = 0;
        bool idr = false, sps = false;
        const u8 *f = h264_asm_frame(c->as, &fn, &idr, &sps);
        c->frames++;
        if (c->unsup && !sps) { mutex_unlock(CR.mu); return; }
        if (c->need_idr && !idr) { crx_pli(w, c); mutex_unlock(CR.mu); return; }
        const u8 *out = f;
        size_t on = fn;
        c->e2ee = w->dave != NULL;
        if (w->dave) {                           /* сессия E2EE: кадр — только через дешифратор участника */
            if (!c->dd) crx_ratchet(w, c);
            size_t cap = c->dd ? daveDecryptorGetMaxPlaintextByteSize(c->dd, DAVE_MEDIA_TYPE_VIDEO, fn) : fn;
            if (cap < fn) cap = fn;
            if (cap > c->plcap) {
                u8 *nb = malloc(cap);
                if (!nb) { mutex_unlock(CR.mu); return; }
                if (c->pl) { wipe(c->pl, c->plcap); free(c->pl); }
                c->pl = nb; c->plcap = cap;
            }
            size_t wr = 0;
            int drc = c->dd ? (int)daveDecryptorDecrypt(c->dd, DAVE_MEDIA_TYPE_VIDEO, f, fn, c->pl, c->plcap, &wr) : -99;
            if (drc != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) { c->lost++; c->need_idr = true; crx_pli(w, c); mutex_unlock(CR.mu); return; }
            out = c->pl; on = wr;
        }
        if (!crx_enqueue(c, out, on, idr, codec)) { c->need_idr = true; crx_pli(w, c); }
        else if (idr) { c->need_idr = false; c->unsup = false; }
    }
    mutex_unlock(CR.mu);
}
/* поток звонка, каждые ~200 мс: видимость плиток -> op 15, невидимые — до нуля */
static void crx_tick(VW *w) {
    if (!CR.mu) return;
    mutex_lock(CR.mu);
    bool dirty = CR.wants_dirty;
    CR.wants_dirty = false;
    for (int i = 0; i < CR.n; i++) {
        CamRx *c = CR.s[i];
        int pw = 0, ph = 0;
        bool want = gcam_want(c->user, &pw, &ph);
        int px = want ? pw * ph : 0;
        if (cam_full_user() == c->user && gvid_wanted()) {   /* крупный вид — самый крупный слой */
            int fw = 0, fh = 0;
            gvid_area_px(&fw, &fh);
            if (fw * fh > px) px = fw * fh;
            want = true; pw = ph = 4096;
        }
        if (px != c->px) { c->px = px; dirty = true; }
        int pick = want ? crx_pick(c, pw, ph) : -1;
        for (int j = 0; j < c->nl; j++) {
            int q = j == pick ? 100 : 0;
            if (c->l[j].sent != q) { c->l[j].sent = q; dirty = true; }
        }
        if (want != c->wanted) {
            c->wanted = want;
            if (!want) {                         /* скрыли: сборка, очередь, декодер — до нуля */
                crx_free_rx(c);
                crx_q_drop(c);
                if (CR.busy == c) c->drop_dec = true; else crx_dec_free(c);
            }
        }
        if (want && pick >= 0 && c->cur && c->cur != c->l[pick].ssrc) { crx_free_rx(c); }   /* сменили слой: ждём ключевой нового */
    }
    size_t lb = 0;
    for (int i = 0; i < CR.n; i++) { CamRx *c = CR.s[i]; lb += sizeof *c + h264_asm_bytes(c->as) + c->plcap; for (int k = 0; k < c->qn; k++) lb += c->q[k].n; }
    atomic_store(&CR.bytes, lb);
    char b[1400];
    int k = dirty && w->session_ready ? cam_wants_json(b, sizeof b, CR.s, CR.n) : -1;
    mutex_unlock(CR.mu);
    if (k > 0) ws_text(w, b);
}
static void crx_end(void) {                      /* конец звонка: всё до нуля */
    if (!CR.mu) return;
    if (CR.thr) { atomic_store(&CR.stop, true); wake_signal(CR.wk); thread_join(CR.thr); CR.thr = NULL; }
    mutex_lock(CR.mu);
    for (int i = 0; i < CR.n; i++) crx_slot_free(CR.s[i]);
    CR.n = 0;
    CR.busy = NULL;
    atomic_store(&CR.bytes, 0);
    mutex_unlock(CR.mu);
}

/* ======================= поток звонка ======================= */
/* каждый проход цикла голосового потока (основное соединение) */
static void cam_tick(VW *w) {
    vc_init();
    i64 now = now_ms();
    if (w->session_ready && w->vssrc && atomic_load(&VC.gen) != w->cam_gen) {
        w->cam_gen = atomic_load(&VC.gen);
        if (atomic_load(&VC.on) || w->cam_op12) cam_send_op12(w);
        if (atomic_load(&VC.on)) cam_request_idr();
    }
    bool ready = atomic_load(&VC.on) && w->session_ready && w->vssrc && w->cam_op12 && w->vcodec_ok && w->dave && w->dave_on;
    if (ready != atomic_load(&VC.ready)) { atomic_store(&VC.ready, ready); if (ready) cam_request_idr(); }
    atomic_store(&VC.codec_bad, !w->vcodec_ok);
    for (;;) {
        mutex_lock(VC.mu);
        if (!VC.qn) { mutex_unlock(VC.mu); break; }
        u8 *p = VC.q[0].p;
        size_t pn = VC.q[0].n;
        u32 ts = VC.q[0].ts;
        memmove(VC.q, VC.q + 1, (size_t)(VC.qn - 1) * sizeof VC.q[0]);
        VC.qn--;
        mutex_unlock(VC.mu);
        if (ready) cam_send_frame(w, p, pn, ts);
        wipe(p, pn);
        free(p);
    }
    if (w->cam_op12 && w->vsent && now >= w->next_sr) { vs_rtcp_sr(w); w->next_sr = now + 1000; }
    if (now >= w->cam_next_rx) { w->cam_next_rx = now + 200; crx_tick(w); }
}
/* конец соединения звонка (выход из voice_thread) */
static void cam_vw_end(VW *w) {
    atomic_store(&VC.ready, false);
    if (w->chist) { wipe(w->chist, CAM_HIST * sizeof(CamHist)); free(w->chist); w->chist = NULL; }
    crx_end();
}
/* разбор входящей датаграммы основного соединения: RTCP — свой видеопоток, видео участников, иначе звук */
static void vrx(VW *w, const unsigned char *b, int k) {
    if (k >= 12 && (b[0] >> 6) == 2 && !w->demo) {
        if (b[1] >= 200 && b[1] <= 206) { cam_rtcp_in(w, b, k); return; }
        if (vc_of_pt(b[1] & 0x7F) >= 0) { cam_rx_rtp(w, b, k); return; }
    }
    on_rtp(w, b, k);
}

/* ======================= cam.h: участники ======================= */
size_t voice_cam_rx_bytes(void) { return CR.mu ? atomic_load(&CR.bytes) + (CR.n ? vdec_bytes() : 0) : 0; }
int cam_users(u64 *out, int max) {
    if (!CR.mu) return 0;
    int n = 0;
    mutex_lock(CR.mu);
    for (int i = 0; i < CR.n && n < max; i++) out[n++] = CR.s[i]->user;
    mutex_unlock(CR.mu);
    return n;
}
bool cam_tile(u64 user, CamTile *o) {
    memset(o, 0, sizeof *o);
    if (user == S.me_id) {
        CamState cs;
        cam_state(&cs);
        o->video = cs.phase == CAM_STARTING || cs.phase == CAM_WAIT_E2EE || cs.phase == CAM_LIVE;
        o->shown = o->video && gcam_shown(user);
        o->e2ee = cs.phase == CAM_LIVE;
        o->w = cs.w; o->h = cs.h;
        o->frames = cs.frames;
        return o->video;
    }
    if (!CR.mu) return false;
    mutex_lock(CR.mu);
    CamRx *c = crx_find(user);
    if (c) {
        o->video = true; o->e2ee = c->e2ee; o->unsupported = c->unsup;
        o->w = c->fw; o->h = c->fh; o->frames = c->frames; o->lost = c->lost;
    }
    mutex_unlock(CR.mu);
    if (c) o->shown = gcam_shown(user);
    return o->video;
}

/* ======================= selftest ======================= */
int voice_cam_selftest(void (*check)(bool ok, const char *what)) {
    char b[700], w[400];
    /* op 12 своей камеры: включение и выключение */
    int k1 = cam_op12_json(b, sizeof b, 11, 22, 23, true, 1280, 720, 30, 2500000);
    bool on = k1 > 0 && strstr(b, "\"op\":12") && strstr(b, "\"audio_ssrc\":11,\"video_ssrc\":22,\"rtx_ssrc\":23") && strstr(b, "\"type\":\"video\"")
              && strstr(b, "\"rid\":\"100\"") && strstr(b, "\"ssrc\":22,\"active\":true,\"quality\":100") && strstr(b, "\"max_bitrate\":2500000")
              && strstr(b, "\"max_framerate\":30") && strstr(b, "\"max_resolution\":{\"type\":\"fixed\",\"width\":1280,\"height\":720}");
    cJSON *j = on ? cJSON_Parse(b) : NULL;
    bool valid = j != NULL;
    cJSON_Delete(j);
    int k2 = cam_op12_json(b, sizeof b, 11, 22, 23, false, 1280, 720, 30, 2500000);
    bool off = k2 > 0 && strstr(b, "\"video_ssrc\":0,\"rtx_ssrc\":0") && strstr(b, "\"active\":false");
    volatile size_t tn = 64;                     /* размер не виден компилятору: обрезка — нарочно (gcc 12 -Wformat-truncation) */
    char *tiny = malloc(tn);
    bool small_ok = tiny && cam_op12_json(tiny, tn, 1, 2, 3, true, 640, 360, 30, 1) == -1;
    free(tiny);
    check(on && valid && off && small_ok,
          "камера: op 12 своей камеры (video rid 100, active, max_resolution fixed 1280x720, 30 к/с), выключение — video_ssrc 0, active false; тесный буфер — отказ");
    /* op 12 участника: два слоя simulcast; без streams; выключено */
    {
        const char *a = "{\"user_id\":\"777\",\"audio_ssrc\":5,\"video_ssrc\":6,\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"ssrc\":6,\"rtx_ssrc\":7,\"quality\":100,\"active\":true,"
                        "\"max_resolution\":{\"type\":\"fixed\",\"width\":1280,\"height\":720}},{\"type\":\"video\",\"rid\":\"50\",\"ssrc\":8,\"rtx_ssrc\":9,\"quality\":50,\"active\":true,"
                        "\"max_resolution\":{\"type\":\"fixed\",\"width\":640,\"height\":360}}]}";
        const char *bb = "{\"user_id\":\"778\",\"audio_ssrc\":15,\"video_ssrc\":16}";
        const char *cc = "{\"user_id\":\"779\",\"audio_ssrc\":25,\"video_ssrc\":0,\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"ssrc\":26,\"active\":false,\"quality\":100}]}";
        cJSON *ja = cJSON_Parse(a), *jb = cJSON_Parse(bb), *jc = cJSON_Parse(cc);
        CamOp12 oa, ob, oc;
        bool pa = ja && cam_parse_op12(ja, &oa), pb = jb && cam_parse_op12(jb, &ob), pc = jc && cam_parse_op12(jc, &oc);
        cJSON_Delete(ja); cJSON_Delete(jb); cJSON_Delete(jc);
        bool ok = pa && oa.user == 777 && oa.video && oa.nl == 2 && oa.l[0].ssrc == 6 && oa.l[1].ssrc == 8 && oa.l[1].q == 50 && oa.l[1].h == 360
                  && pb && ob.video && ob.nl == 1 && ob.l[0].ssrc == 16 && pc && !oc.video;
        check(ok, "камера: op 12 участника — два слоя simulcast (100/50, размеры), старый вид без streams, выключенная камера (video_ssrc 0)");
        /* выбор слоя под плитку и op 15 */
        CamRx *r = calloc(1, sizeof *r), *r2 = calloc(1, sizeof *r2);
        if (r && r2 && pa) {
            r->nl = oa.nl;
            for (int i = 0; i < oa.nl; i++) { r->l[i].ssrc = oa.l[i].ssrc; r->l[i].q = oa.l[i].q; r->l[i].act = oa.l[i].act; r->l[i].w = oa.l[i].w; r->l[i].h = oa.l[i].h; }
            int big = crx_pick(r, 1280, 720), small = crx_pick(r, 320, 180), mid = crx_pick(r, 800, 450);
            r->l[0].h = r->l[1].h = 0;           /* размеров нет — по качеству */
            int qs = crx_pick(r, 320, 180), qb = crx_pick(r, 960, 540);
            r->l[0].sent = 100; r->l[1].sent = 0; r->px = 320 * 180;
            r2->nl = 1; r2->l[0].ssrc = 16; r2->l[0].sent = -1;
            CamRx *arr[2] = { r, r2 };
            int k = cam_wants_json(b, sizeof b, arr, 2);
            cJSON *jw = k > 0 ? cJSON_Parse(b) : NULL;
            bool wj = jw && jint(jw, "op", 0) == 15 && jint(jget(jw, "d"), "6", -1) == 100 && jint(jget(jw, "d"), "8", -1) == 0 && jint(jget(jw, "d"), "16", -1) == 0
                      && jint(jget(jget(jw, "d"), "pixelCounts"), "6", -1) == 57600 && !jget(jget(jget(jw, "d"), "pixelCounts"), "8");
            cJSON_Delete(jw);
            snprintf(w, sizeof w, "камера: слой под плитку 1280x720 -> %d, 320x180 -> %d, 800x450 -> %d; без размеров 320x180 -> %d, 960x540 -> %d; op 15 {ssrc: 100/0}",
                     big, small, mid, qs, qb);
            strncat(w, ", pixelCounts по площади плитки", sizeof w - strlen(w) - 1);
            check(big == 0 && small == 1 && mid == 0 && qs == 1 && qb == 0 && wj, w);
        }
        free(r); free(r2);
    }
    /* приём без звонка: невидимая плитка не собирается; кадр в эфир без готовности не принимается */
    {
        bool rx0 = voice_cam_rx_bytes() == 0;
        voice_cam_frame((const u8 *)"\0\0\0\1\x65", 5, true, 0);
        bool q0 = !VC.mu || VC.qn == 0;
        int pw = 0, ph = 0;
        bool nw = !gcam_want(12345, &pw, &ph);
        check(rx0 && q0 && nw && !voice_cam_ready(), "камера: без звонка — ни очереди кадров, ни памяти приёма, плитки не видны");
    }
    return 0;
}
