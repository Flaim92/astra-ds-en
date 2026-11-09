/* Консольный (построчный) интерфейс --cli, как у IRC-клиента.
 * Вывод: новые сообщения открытого канала строками, уведомления и ошибки строкой "-!- ...".
 * Ввод: строка "/команда ..." или текст, который уходит в открытый канал.
 * Терминал не переводится ни в сырой режим, ни в альтернативный экран.
 *
 * Как читается ввод (без опроса в цикле):
 *   Linux   — fd 0 стоит в общем poll; read() только после poll(0) "готово", флаги fd не меняются.
 *             Строку редактирует сам терминал (канонический режим), эхо — его же.
 *   Windows — консоль: хэндл ввода в общем ожидании; события забираются ReadConsoleInputW, который
 *             не блокирует, если события есть (мышь/фокус/отпускание клавиш выбрасываются, так что
 *             хэндл не остаётся сигнальным). Строку собираем и показываем сами (эхо WriteConsoleW).
 *             Канал или файл: анонимный канал нельзя ждать в WaitForMultipleObjects, поэтому — один
 *             поток-читатель с ограниченным буфером и событием (только в этом случае).
 * Ничего не пишется на диск; вывод — только в stdout. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include "mc.h"
#include "plat.h"
#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#endif

#define MAP_CAP 256                              /* номера сообщений из последнего вывода */
#define PEND_MAX 65536                           /* необработанный ввод (байт) */
#define IN_MAX 8000                              /* длина строки ввода (байт) */
#define SHOW_ON_OPEN 20                          /* сколько сообщений показать при открытии */

typedef struct { u64 id; u32 hc, hr; } Num;      /* id и отпечатки текста/реакций (для правок) */

static struct {
    Num map[MAP_CAP]; int nmap, base;            /* номер = base + индекс + 1 */
    u64 last_id;                                 /* последнее показанное сообщение открытого канала */
    u64 *list; int nlist, caplist;               /* каналы из последнего /channels или /dms */
    bool list_guilds;                            /* последним был /servers: /open N - сервер N */
    u64 vlist[32]; int nvlist;                   /* участники голоса из последнего /status */
    u64 guild;                                   /* текущий сервер */
    bool loading, dirty, ringing_was, ptt_on;
    int want;                                    /* сколько показать после подгрузки старых */
    i64 ack_at;
    VoicePhase vphase; bool vdave;
    u64 seen[32]; int nseen;                     /* каналы, о новом в которых уже сказали */
    Buf in, out;                                 /* необработанный ввод; накопленный вывод */
    bool eof, tty;                               /* tty: вывод в терминал (для звонка \a) */
    bool await_link;                             /* после /vless set: следующая строка — ссылка (скрытый ввод) */
    VlessState vl;                               /* последнее показанное состояние туннеля */
    bool search_wait, search_idx_said;           /* ждём ответ /search; «индекс строится» уже сказали */
} C;
static volatile sig_atomic_t c_stop;             /* Ctrl+C */
static volatile sig_atomic_t c_panic;            /* тройной Ctrl+\ (Linux): паника в главном потоке */

static void out_raw(const char *s, size_t n);
static void ed_show(void);
static void ed_hide(void);
static void echo_set(bool on);                   /* эхо ввода (скрытая ссылка VLESS) */
#ifdef _WIN32
static bool input_is_console(void);
#endif

/* ======================= вывод ======================= */
#define P(...) buf_printf(&C.out, __VA_ARGS__)

/* Свои подписи из других модулей могут содержать "…", "—" и т. п.: в оформлении — только ASCII. */
static void put_ascii(Buf *b, const char *s) {
    while (*s) {
        u32 c;
        int k = utf8_dec(s, &c);
        const char *r = NULL;
        switch (c) {
        case 0x2026: r = "..."; break;
        case 0x2013: case 0x2014: case 0x2212: r = "-"; break;
        case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: r = "\""; break;
        case 0x2190: r = "<-"; break;
        case 0x2192: r = "->"; break;
        case 0x2116: r = "N"; break;
        case '\a': r = ""; break;
        default: break;
        }
        if (r) buf_puts(b, r); else buf_put(b, s, (size_t)k);
        s += k;
    }
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    char b[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    buf_puts(&C.out, "-!- ");
    put_ascii(&C.out, b);
    buf_puts(&C.out, "\n");
}

static void bell(void) { if (C.tty) buf_puts(&C.out, "\a"); }

static void flush(void) {
    if (!C.out.len) { ed_show(); return; }
    ed_hide();
    out_raw(C.out.p, C.out.len);
    buf_wipe(&C.out);
    ed_show();
}

/* ======================= имена ======================= */
static bool is_voice(const Channel *c) { return c->type == CH_VOICE || c->type == CH_STAGE; }
static bool is_dm(const Channel *c) { return !c->guild_id && (c->type == CH_DM || c->type == CH_GROUP); }

static void chan_title(const Channel *c, char *out, size_t n) {
    char l[160];
    chan_label(c, l, sizeof l);
    snprintf(out, n, "%s%s", !c->guild_id ? "@" : is_voice(c) ? "~" : "#", l);
}
static const char *guild_name(u64 gid) {
    Guild *g = guild_find(gid);
    return g && g->name ? g->name : NULL;
}
static const char *vs_name(const VoiceState *v) { return v->name ? v->name : user_name(v->user_id); }

/* Сравнение без учёта регистра (латиница и кириллица); prefix — q лишь начало name. */
static u32 fold(u32 c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
static bool name_eq(const char *name, const char *q, bool prefix) {
    while (*q) {
        if (!*name) return false;
        u32 a, b;
        name += utf8_dec(name, &a);
        q += utf8_dec(q, &b);
        if (fold(a) != fold(b)) return false;
    }
    return prefix || !*name;
}

/* Число в начале аргумента (за ним пробел или конец); -1 — не число. rest — остаток после пробелов. */
static int num_arg(const char *s, const char **rest) {
    if (!s || *s < '0' || *s > '9') return -1;
    long v = 0;
    while (*s >= '0' && *s <= '9') { if (v < 1000000) v = v * 10 + (*s - '0'); s++; }
    if (*s && *s != ' ') return -1;
    while (*s == ' ') s++;
    if (rest) *rest = s;
    return (int)v;
}

/* ======================= сообщения и их номера ======================= */
static u32 fnv(u32 h, const char *s) { for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; } return h; }
static u32 hash_content(const Message *m) { return fnv(2166136261u, m->content) ^ (m->edited ? 0x9e3779b9u : 0); }
static u32 hash_reacts(const Message *m) {
    u32 h = 2166136261u;
    for (int i = 0; i < m->nreacts; i++) {
        h = fnv(h, m->reacts[i].emoji);
        h ^= (u32)m->reacts[i].count * 31u + (m->reacts[i].me ? 7u : 0u);
        h *= 16777619u;
    }
    return h;
}

static void num_reset(void) { C.nmap = 0; C.base = 0; }
static int num_add(const Message *m) {
    if (C.nmap == MAP_CAP) {                     /* старшая половина остаётся, номера не меняются */
        memmove(C.map, C.map + MAP_CAP / 2, (MAP_CAP / 2) * sizeof(Num));
        C.nmap -= MAP_CAP / 2;
        C.base += MAP_CAP / 2;
    }
    C.map[C.nmap] = (Num){ m->id, hash_content(m), hash_reacts(m) };
    return C.base + ++C.nmap;
}
static Message *num_msg(int n) {
    int i = n - C.base - 1;
    if (i < 0 || i >= C.nmap || !C.map[i].id) return NULL;
    return msg_find(C.map[i].id, NULL);
}

static void put_reacts(Buf *b, const Message *m) {
    for (int i = 0; i < m->nreacts; i++) {
        const char *e = m->reacts[i].emoji;
        buf_printf(b, "%s[%d] ", i ? "  " : "", i + 1);
        if (e[0] == ':') {                       /* своя эмодзи сервера: ":имя:id" -> ":имя:" */
            const char *z = strchr(e + 1, ':');
            buf_put(b, e, z ? (size_t)(z - e + 1) : strlen(e));
        } else buf_puts(b, e);
        buf_printf(b, " %d%s", m->reacts[i].count, m->reacts[i].me ? "*" : "");
    }
}

static void put_msg(const Message *m, int num, const char *where) {
    /* ответ — своя строка с начала строки и с маркером ",--": не выглядит продолжением предыдущего
     * сообщения (продолжения и реакции идут с отступом 6 пробелов) */
    if (m->ref_id) buf_printf(&C.out, ",-- (ответ) %s: %s\n", m->ref_author ? m->ref_author : "?", m->ref_text ? m->ref_text : "");
    char t[32], nb[16] = "";
    fmt_time(m->ts, t, sizeof t);
    if (num > 0) snprintf(nb, sizeof nb, "(%d) ", num);
    P("%s[%s] %s %s: ", nb, t, where, m->author ? m->author : "?");
    const char *s = m->content ? m->content : "";
    if (!*s) buf_puts(&C.out, "(пусто)");
    for (; *s; s++) {                            /* переносы внутри сообщения — продолжение с отступом */
        if (*s == '\n') buf_puts(&C.out, "\n      ");
        else if (*s != ATT_MARK) buf_put(&C.out, s, 1);   /* маркер вложения не печатать */
    }
    if (m->edited) buf_puts(&C.out, " (ред.)");
    for (int i = 0; i < m->nemb; i++) {         /* №53: эмбед — одной строкой (заголовок - начало описания) */
        const char *et = embed_str(&m->emb[i], EM_TITLE), *d = embed_str(&m->emb[i], EM_DESC);
        if (!*et && !*d) et = embed_str(&m->emb[i], EM_AUTHOR);
        if (!*et && !*d) continue;
        buf_puts(&C.out, "\n      [встройка] ");
        buf_puts(&C.out, et);
        if (*et && *d) buf_puts(&C.out, " - ");
        for (int k = 0; d[k] && k < 200; k++) buf_put(&C.out, d[k] == '\n' ? " " : d + k, 1);
    }
    buf_puts(&C.out, "\n");
    if (m->nreacts) { buf_puts(&C.out, "      + "); put_reacts(&C.out, m); buf_puts(&C.out, "\n"); }
}

static bool open_title(char *t, size_t n) {
    Channel *c = chan_find(S.open_ch);
    if (!c) return false;
    chan_title(c, t, n);
    return true;
}

static void show_history(int count) {
    char t[200];
    if (!open_title(t, sizeof t)) { note("канал не открыт: /open N|имя"); return; }
    if (!S.nmsgs) { note("в %s пока нет сообщений", t); return; }
    int from = count > 0 && count < S.nmsgs ? S.nmsgs - count : 0;
    num_reset();
    P("--- %s: %d из %d загруженных%s ---\n", t, S.nmsgs - from, S.nmsgs,
      !from && S.history_end ? " (начало истории)" : "");
    for (int i = from; i < S.nmsgs; i++) put_msg(&S.msgs[i], num_add(&S.msgs[i]), t);
    if (S.msgs[S.nmsgs - 1].id > C.last_id) C.last_id = S.msgs[S.nmsgs - 1].id;
    if (from || !S.history_end) P("--- /history - все загруженные, /history N - последние N (подгрузит старее) ---\n");
}

static void print_new(void) {
    char t[200];
    if (!open_title(t, sizeof t)) return;
    for (int i = 0; i < S.nmsgs; i++) {
        if (S.msgs[i].id <= C.last_id) continue;
        put_msg(&S.msgs[i], num_add(&S.msgs[i]), t);
        C.last_id = S.msgs[i].id;
    }
}

/* Правки, удаления и реакции приходят как "что-то изменилось": сверяем с отпечатками показанного. */
static void check_changes(void) {
    char t[200];
    if (C.loading || !S.nmsgs || !open_title(t, sizeof t)) return;
    for (int i = 0; i < C.nmap; i++) {
        Num *e = &C.map[i];
        if (!e->id) continue;
        int num = C.base + i + 1;
        Message *m = msg_find(e->id, NULL);
        if (!m) {
            if (e->id > S.msgs[0].id) note("(%d) сообщение удалено", num);
            e->id = 0;                           /* старое выпало из загруженных — молчим */
            continue;
        }
        u32 hc = hash_content(m), hr = hash_reacts(m);
        if (hc != e->hc) { note("(%d) изменено:", num); put_msg(m, num, t); }
        else if (hr != e->hr) {
            P("-!- (%d) реакции: ", num);
            if (m->nreacts) put_reacts(&C.out, m); else buf_puts(&C.out, "нет");
            buf_puts(&C.out, "\n");
        }
        e->hc = hc;
        e->hr = hr;
    }
}

/* ======================= сеть: действия (логика как в ui.c) ======================= */
static void notice_http(const char *what, Resp *r) {
    const char *m = NULL;
    cJSON *j = r->body.p ? json_parse(r->body.p, r->body.len) : NULL;
    if (j) m = jstr(j, "message");
    char *safe = san(m ? m : "", false);
    if (r->status == 0) note("%s: нет связи", what);
    else if (*safe) note("%s: %s", what, safe);
    else note("%s (HTTP %ld)", what, r->status);
    free(safe);
    json_done();
}

static void after_load(bool older, u64 old_first) {
    if (!older) {
        if (C.last_id) print_new();              /* после переподключения — только пропущенное */
        else if (!S.nmsgs) { char t[200]; if (open_title(t, sizeof t)) note("в %s пока нет сообщений", t); }
        else show_history(SHOW_ON_OPEN);
    } else {
        int n = C.want;
        C.want = 0;
        if (!S.nmsgs || S.msgs[0].id >= old_first) note("более старых сообщений нет");
        show_history(n);
    }
    C.ack_at = now_ms() + 300;
}

static void on_messages(Resp *r, void *ctx) {
    u64 cid = (u64)(uintptr_t)ctx;
    bool older = (cid & 1ULL << 63) != 0;
    cid &= ~(1ULL << 63);
    C.loading = false;
    if (cid != S.open_ch) return;
    if (r->status != 200) { C.want = 0; notice_http("история не загрузилась", r); return; }
    u64 first = S.nmsgs ? S.msgs[0].id : 0;
    cJSON *j = json_parse(r->body.p, r->body.len);
    bool ok = cJSON_IsArray(j);
    if (ok) store_set_messages(j, older);
    json_done();
    if (!ok) { note("история: неожиданный ответ"); return; }
    after_load(older, first);
}

static void fetch_messages(bool older) {
    if (!S.open_ch || C.loading) return;
    if (older && (S.history_end || !S.nmsgs)) return;
    char path[128];
    if (older) snprintf(path, sizeof path, "/channels/%llu/messages?before=%llu&limit=50",
                        (unsigned long long)S.open_ch, (unsigned long long)S.msgs[0].id);
    else snprintf(path, sizeof path, "/channels/%llu/messages?limit=50", (unsigned long long)S.open_ch);
    C.loading = true;
    http_async("GET", path, NULL, on_messages, (void *)(uintptr_t)(S.open_ch | (older ? 1ULL << 63 : 0)));
}

static void send_ack(void) {
    Channel *c = chan_find(S.open_ch);
    if (!c || O.no_ack || O.demo) return;
    if (c->last_msg <= c->last_read && !c->mentions) return;
    u64 target = S.nmsgs ? S.msgs[S.nmsgs - 1].id : c->last_msg;
    if (target < c->last_msg) target = c->last_msg;
    char path[128], body[64];
    snprintf(path, sizeof path, "/channels/%llu/messages/%llu/ack", (unsigned long long)c->id, (unsigned long long)target);
    i64 days = (wall_ms() - DISCORD_EPOCH_MS + 86399999) / 86400000;
    snprintf(body, sizeof body, "{\"token\":null,\"last_viewed\":%lld}", (long long)days);
    http_async("POST", path, body, NULL, NULL);
    c->last_read = target;
    c->mentions = 0;
}

static void seen_forget(u64 id) {
    for (int i = 0; i < C.nseen; i++) if (C.seen[i] == id) { C.seen[i] = C.seen[--C.nseen]; return; }
}
static bool seen_add(u64 id) {                   /* true — впервые */
    for (int i = 0; i < C.nseen; i++) if (C.seen[i] == id) return false;
    if (C.nseen == 32) memmove(C.seen, C.seen + 1, 31 * sizeof(u64)), C.nseen--;
    C.seen[C.nseen++] = id;
    return true;
}

/* keep: переоткрытие после переподключения — номера и "показано до" сохраняются */
static void open_chan(Channel *c, bool keep) {
    if (c->type == CH_CATEGORY) return;
    char t[200];
    chan_title(c, t, sizeof t);
    if (is_voice(c)) { note("%s - голосовой канал: /join", t); return; }
    if (c->guild_id) C.guild = c->guild_id;
    seen_forget(c->id);
    if (S.open_ch == c->id && !keep) { show_history(SHOW_ON_OPEN); return; }
    S.open_ch = c->id;
    msgs_clear();
    S.typing_until = 0;
    C.loading = false;
    C.want = 0;
    if (!keep) {
        num_reset();
        C.last_id = 0;
        const char *g = guild_name(c->guild_id);
        P("-!- открыт %s%s%s%s\n", t, g ? " [" : "", g ? g : "", g ? "]" : "");
    }
    if (O.demo) { demo_send(c->id, NULL); after_load(false, 0); }
    else {
        fetch_messages(false);
        if (c->guild_id) gw_subscribe(c->guild_id, c->id);
    }
}

static void on_sent(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status >= 300 || r->status == 0) notice_http("не отправлено", r);
}

static void send_text(const char *text, u64 reply_to, u64 edit_id) {
    if (!S.open_ch) { note("сначала откройте канал: /servers, /channels N, /open N"); return; }
    Channel *c = chan_find(S.open_ch);
    if (c && !c->can_send) { note("в этом канале писать нельзя"); return; }
    if (O.demo) {
        if (edit_id) { note("в демо-режиме правка не отправляется"); return; }
        demo_send(S.open_ch, text);
        return;
    }
    Buf b = {0};
    char path[96];
    if (edit_id) {
        snprintf(path, sizeof path, "/channels/%llu/messages/%llu", (unsigned long long)S.open_ch, (unsigned long long)edit_id);
        buf_puts(&b, "{\"content\":");
        buf_json_str(&b, text);
        buf_puts(&b, "}");
        http_async("PATCH", path, b.p, on_sent, NULL);
    } else {
        snprintf(path, sizeof path, "/channels/%llu/messages", (unsigned long long)S.open_ch);
        buf_puts(&b, "{\"content\":");
        buf_json_str(&b, text);
        buf_printf(&b, ",\"nonce\":\"%llu\",\"tts\":false,\"flags\":0", (unsigned long long)snowflake_now());
        if (reply_to) {
            buf_printf(&b, ",\"message_reference\":{\"channel_id\":\"%llu\",\"message_id\":\"%llu\"",
                       (unsigned long long)S.open_ch, (unsigned long long)reply_to);
            if (c && c->guild_id) buf_printf(&b, ",\"guild_id\":\"%llu\"", (unsigned long long)c->guild_id);
            buf_puts(&b, "}");
        }
        buf_puts(&b, "}");
        http_async("POST", path, b.p, on_sent, NULL);
    }
    buf_wipe(&b);
}

static void delete_message(u64 id) {
    if (O.demo) { note("в демо-режиме удаление не отправляется"); return; }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/messages/%llu", (unsigned long long)S.open_ch, (unsigned long long)id);
    http_async("DELETE", path, NULL, on_sent, NULL);
}

/* Короткие коды для реакций (как в ui.c); остальное — вставить эмодзи напрямую. */
static const char *const SHORTCODES[][2] = {
    {"thumbsup", "\xF0\x9F\x91\x8D"}, {"+1", "\xF0\x9F\x91\x8D"}, {"thumbsdown", "\xF0\x9F\x91\x8E"},
    {"heart", "\xE2\x9D\xA4\xEF\xB8\x8F"}, {"joy", "\xF0\x9F\x98\x82"}, {"sob", "\xF0\x9F\x98\xAD"},
    {"fire", "\xF0\x9F\x94\xA5"}, {"eyes", "\xF0\x9F\x91\x80"}, {"ok_hand", "\xF0\x9F\x91\x8C"},
    {"pray", "\xF0\x9F\x99\x8F"}, {"clap", "\xF0\x9F\x91\x8F"}, {"tada", "\xF0\x9F\x8E\x89"},
    {"thinking", "\xF0\x9F\xA4\x94"}, {"skull", "\xF0\x9F\x92\x80"}, {"smile", "\xF0\x9F\x98\x84"},
    {"rofl", "\xF0\x9F\xA4\xA3"}, {"100", "\xF0\x9F\x92\xAF"}, {"white_check_mark", "\xE2\x9C\x85"},
    {"x", "\xE2\x9D\x8C"}, {"wave", "\xF0\x9F\x91\x8B"}, {"rage", "\xF0\x9F\x98\xA1"},
};
static void react(const Message *m, const char *emoji_in) {
    char emoji[160];
    snprintf(emoji, sizeof emoji, "%s", emoji_in);
    size_t el = strlen(emoji);
    if (num_arg(emoji, NULL) > 0) {              /* номер существующей реакции */
        int k = num_arg(emoji, NULL) - 1;
        if (k >= m->nreacts) { note("у сообщения нет реакции [%d]", k + 1); return; }
        snprintf(emoji, sizeof emoji, "%s", m->reacts[k].emoji);
    } else if (el > 2 && emoji[0] == ':' && emoji[el - 1] == ':') {
        emoji[el - 1] = 0;
        const char *name = emoji + 1;
        const char *hit = NULL;
        for (size_t i = 0; i < sizeof SHORTCODES / sizeof *SHORTCODES; i++) if (!strcmp(SHORTCODES[i][0], name)) hit = SHORTCODES[i][1];
        char tmp[160];                           /* своя эмодзи сервера, если она уже стоит на сообщении */
        size_t nl = strlen(name);
        if (!hit) for (int i = 0; i < m->nreacts; i++) {
            const char *e = m->reacts[i].emoji;
            if (e[0] == ':' && !strncmp(e + 1, name, nl) && e[nl + 1] == ':') { snprintf(tmp, sizeof tmp, "%s", e); hit = tmp; }
        }
        if (!hit) { note("неизвестный код :%s: - вставьте эмодзи напрямую или номер реакции", name); return; }
        snprintf(emoji, sizeof emoji, "%s", hit);
    }
    if (O.demo) { note("в демо-режиме реакции не отправляются"); return; }
    bool mine = false;
    for (int i = 0; i < m->nreacts; i++) if (!strcmp(m->reacts[i].emoji, emoji)) mine = m->reacts[i].me;
    Buf e = {0}, path = {0};                     /* unicode url-кодирован, своя эмодзи — name:id */
    url_encode(emoji[0] == ':' ? emoji + 1 : emoji, &e);
    buf_printf(&path, "/channels/%llu/messages/%llu/reactions/%s/@me?location=Message&type=0",
               (unsigned long long)S.open_ch, (unsigned long long)m->id, e.p ? e.p : "");
    http_async(mine ? "DELETE" : "PUT", path.p, NULL, on_sent, NULL);
    buf_free(&e);
    buf_free(&path);
}

static void call_decline(u64 ch) {
    char path[96], body[64];
    snprintf(path, sizeof path, "/channels/%llu/call/stop-ringing", (unsigned long long)ch);
    snprintf(body, sizeof body, "{\"recipients\":[\"%llu\"]}", (unsigned long long)S.me_id);
    http_async("POST", path, body, NULL, NULL);
}

static void call_note(void) {
    Channel *c = chan_find(S.call.channel_id);
    char t[200] = "?";
    if (c) chan_title(c, t, sizeof t);
    bell();
    note("входящий звонок от %s (%s): /accept - принять, /decline - отклонить",
         S.call.caller ? user_name(S.call.caller) : "?", t);
    C.ringing_was = true;
}

/* ======================= списки ======================= */
static void list_reset(void) {
    if (C.caplist < S.nchans + 1) { C.caplist = S.nchans + 1; C.list = xrealloc(C.list, (size_t)C.caplist * sizeof(u64)); }
    C.nlist = 0;
    C.list_guilds = false;
}
static int list_add(u64 id) { C.list[C.nlist++] = id; return C.nlist; }

static int cmp_chan(const void *a, const void *b) {
    const Channel *x = chan_find(*(const u64 *)a), *y = chan_find(*(const u64 *)b);
    if (!x || !y) return 0;
    bool vx = is_voice(x), vy = is_voice(y);     /* голос после текста, как в официальном клиенте */
    if (vx != vy) return vx ? 1 : -1;
    if (x->position != y->position) return x->position - y->position;
    return x->id < y->id ? -1 : x->id > y->id;
}
static bool listed(const Channel *c) {
    return c->visible && (c->type == CH_TEXT || c->type == CH_NEWS || is_voice(c));
}

static void put_members(u64 cid) {
    const VoiceState *v[32];
    int k = voice_members(cid, v, 32);
    for (int j = 0; j < k; j++)
        P("           - %s%s%s\n", vs_name(v[j]), v[j]->self_mute || v[j]->mute ? " (мик. выкл)" : "",
          v[j]->self_deaf || v[j]->deaf ? " (звук выкл)" : "");
}

static void put_chan(const Channel *c, const VoiceStatus *vs) {
    char t[200];
    chan_title(c, t, sizeof t);
    bool here = c->id == S.open_ch || (vs->phase != VS_IDLE && vs->channel_id == c->id);
    P("  %c%3d  %s", here ? '>' : ' ', list_add(c->id), t);
    if (!is_voice(c) && chan_unread(c)) P(" *");
    if (c->mentions) P(" @%d", c->mentions);
    if (c->muted) P(" (заглушён)");
    if (!is_voice(c) && !c->can_send) P(" (только чтение)");
    if (is_voice(c) && !c->can_connect) P(" (нет доступа)");
    P("\n");
    if (is_voice(c) || is_dm(c)) put_members(c->id);
}

static void show_guild(const Guild *g) {
    VoiceStatus vs;
    voice_status(&vs);
    C.guild = g->id;
    list_reset();
    P("-!- %s: каналы (/open N - открыть, /join N - голос; * непрочитано, @N упоминания, > открыт):\n", g->name ? g->name : "?");
    u64 *tmp = xmalloc((size_t)(S.nchans + 1) * sizeof(u64)), *cats = xmalloc((size_t)(S.nchans + 1) * sizeof(u64));
    int n = 0, nc = 0;
    for (int i = 0; i < S.nchans; i++) {
        Channel *c = &S.chans[i];
        if (c->guild_id != g->id) continue;
        if (c->type == CH_CATEGORY) cats[nc++] = c->id;
        else if (!c->parent_id && listed(c)) tmp[n++] = c->id;
    }
    qsort(tmp, (size_t)n, sizeof(u64), cmp_chan);
    for (int i = 0; i < n; i++) put_chan(chan_find(tmp[i]), &vs);
    qsort(cats, (size_t)nc, sizeof(u64), cmp_chan);
    for (int k = 0; k < nc; k++) {
        n = 0;
        for (int i = 0; i < S.nchans; i++) {
            Channel *c = &S.chans[i];
            if (c->guild_id == g->id && c->parent_id == cats[k] && listed(c)) tmp[n++] = c->id;
        }
        if (!n) continue;
        Channel *cat = chan_find(cats[k]);
        P("  [%s]\n", cat && cat->name ? cat->name : "?");
        qsort(tmp, (size_t)n, sizeof(u64), cmp_chan);
        for (int i = 0; i < n; i++) put_chan(chan_find(tmp[i]), &vs);
    }
    free(tmp);
    free(cats);
    if (!C.nlist) note("видимых каналов нет");
}

/* Канал по номеру из последнего списка или по имени: сначала текущий сервер, потом ЛС, потом все.
 * voice: искать голосовые (для /join), иначе текстовые и ЛС. */
static Channel *pick_channel(const char *arg, bool voice) {
    int n = num_arg(arg, NULL);
    if (n >= 0) {
        if (n < 1 || n > C.nlist) { note("нет номера %d в последнем списке (/channels, /dms)", n); return NULL; }
        return chan_find(C.list[n - 1]);
    }
    const char *q = arg;
    if (*q == '#' || *q == '@' || *q == '~') q++;
    if (!*q) return NULL;
    for (int pass = 0; pass < 2; pass++)
        for (int scope = 0; scope < 3; scope++)
            for (int i = 0; i < S.nchans; i++) {
                Channel *c = &S.chans[i];
                bool dm = is_dm(c);
                if (scope == 0 && (!c->guild_id || c->guild_id != C.guild)) continue;
                if (scope == 1 && !dm) continue;
                if (scope == 2 && (!c->guild_id || c->guild_id == C.guild)) continue;
                if (!dm && (!listed(c) || is_voice(c) != voice)) continue;
                char l[160];
                chan_label(c, l, sizeof l);
                if (name_eq(l, q, pass == 1)) return c;
            }
    note("канал \"%s\" не найден; номера: /channels, /dms", arg);
    return NULL;
}

static Message *pick_msg(const char *a, const char **rest, const char *usage) {
    int n = num_arg(a, rest);
    if (n < 1) { note("использование: %s", usage); return NULL; }
    Message *m = num_msg(n);
    if (!m) note("нет сообщения (%d) в последнем выводе; /history - показать заново", n);
    return m;
}

/* ======================= команды ======================= */
static void cmd_help(const char *a) {
    (void)a;
    P("-!- команды (N сообщения - номер в скобках из последнего вывода):\n"
      "  /servers                 список серверов\n"
      "  /channels [N|имя]        каналы сервера; номера для /open и /join\n"
      "  /dms                     личные сообщения\n"
      "  /open N|имя              открыть канал или ЛС и показать последние сообщения\n"
      "  /history [N]             показать загруженные; N - последние N (подгрузит старее)\n"
      "  /friends [online|pending|blocked]  друзья и заявки (номера для /dm, /friend)\n"
      "  /dm N|имя                открыть или создать личную беседу с другом\n"
      "  /friend accept N, /friend remove N yes   принять заявку; удалить/отклонить/разблокировать\n"
      "  /search текст, /search close   поиск по серверу открытого канала или по ЛС (25 результатов)\n"
      "  /file путь, /file \"путь\" подпись   отправить файл в открытый канал (до 10 МБ)\n"
      "  /captcha, /captcha cancel  Discord просит проверку: открыть окно капчи / отказаться\n"
      "  /reply N текст           ответить на сообщение\n"
      "  /edit N текст            править своё\n"
      "  /del N yes               удалить своё (без yes - только показать)\n"
      "  /react N эмодзи|:код:|номер   поставить/снять реакцию\n"
      "  /join N|имя, /leave      войти в голосовой канал, выйти\n"
      "  /mute, /deaf             микрофон, звук вкл/выкл\n"
      "  /call, /accept, /decline позвонить в открытый ЛС, принять, отклонить\n"
      "  /vol имя|N процент       громкость участника (номера из /status), 0..200\n"
      "  /vad +|-|дБ              порог активации голосом\n"
      "  /ptt                     рация вкл/выкл (с флагом --ptt)\n"
      "  /voice [off|low|high]    смена голоса: выкл / ниже / выше (без аргумента - по кругу)\n"
      "  /vless                   туннель VLESS: состояние и задержка\n"
      "  /vless retry|set|off     переподключить / сменить ссылку (ввод скрыт) / отключить (напрямую)\n"
      "  /status                  связь, открытый канал, голос\n"
      "  /panic                   паника: стереть токен и ссылку VLESS, очистить экран, выйти\n"
#ifdef _WIN32
      "                           (также тройное нажатие клавиши --panic)\n"
#else
      "                           (также тройной Ctrl+\\ за 1,5 с при заданном --panic)\n"
#endif
      "  /quit                    выход (также Ctrl+C)\n"
      "  текст                    отправить в открытый канал; //текст - текст с / в начале\n"
      "  в выводе: (N) номер, ,-- (ответ) на сообщение, + реакции ([номер] эмодзи количество, * - ваша),\n"
      "            (ред.) - сообщение отредактировано\n");
}

static void cmd_servers(const char *a) {
    (void)a;
    if (!S.nguilds) note("серверов нет");
    else P("-!- серверы (/channels N - каналы; * непрочитано, @N упоминания, > текущий):\n");
    for (int i = 0; i < S.nguilds; i++) {
        Guild *g = &S.guilds[i];
        int ment = 0;
        bool un = guild_unread(g->id, &ment);
        P("  %c%3d  %s%s", g->id == C.guild ? '>' : ' ', i + 1, g->name ? g->name : "?", un ? " *" : "");
        if (ment) P(" @%d", ment);
        if (g->muted) P(" (заглушён)");
        P("\n");
    }
    C.list_guilds = true;
    int dun = 0;
    for (int i = 0; i < S.nchans; i++) if (is_dm(&S.chans[i]) && (chan_unread(&S.chans[i]) || S.chans[i].mentions)) dun++;
    P("        личные сообщения: /dms%s\n", dun ? " *" : "");
}

static void cmd_channels(const char *a) {
    const Guild *g = NULL;
    int n = num_arg(a, NULL);
    if (n >= 1 && n <= S.nguilds) g = &S.guilds[n - 1];
    else if (n >= 0) { note("нет сервера %d; список: /servers", n); return; }
    else if (*a) {
        for (int pass = 0; pass < 2 && !g; pass++)
            for (int i = 0; i < S.nguilds && !g; i++) if (S.guilds[i].name && name_eq(S.guilds[i].name, a, pass == 1)) g = &S.guilds[i];
        if (!g) { note("сервер \"%s\" не найден; список: /servers", a); return; }
    } else {
        g = guild_find(C.guild);
        if (!g) { note("укажите сервер: /channels N (номера в /servers)"); return; }
    }
    show_guild(g);
}

static void cmd_dms(const char *a) {
    (void)a;
    VoiceStatus vs;
    voice_status(&vs);
    list_reset();
    P("-!- личные сообщения (/open N; * непрочитано, @N упоминания, > открыт):\n");
    for (int i = 0; i < S.nchans; i++) if (is_dm(&S.chans[i])) put_chan(&S.chans[i], &vs);
    if (!C.nlist) note("личных бесед нет");
}

static void cmd_open(const char *a) {
    if (!*a) {
        char t[200];
        if (open_title(t, sizeof t)) note("открыт %s; другой: /open N|имя", t);
        else note("использование: /open N|имя (номера: /channels, /dms)");
        return;
    }
    if (C.list_guilds && num_arg(a, NULL) >= 0) { cmd_channels(a); return; }
    Channel *c = pick_channel(a, false);
    if (c) open_chan(c, false);
}

static void cmd_history(const char *a) {
    if (!S.open_ch) { note("канал не открыт: /open N|имя"); return; }
    if (C.loading) { note("история ещё загружается"); return; }
    int n = num_arg(a, NULL);
    if (*a && n < 1) { note("использование: /history [N]"); return; }
    if (n > S.nmsgs && !S.history_end && !O.demo) {
        C.want = n;
        note("подгружаю более старые сообщения...");
        fetch_messages(true);
        return;
    }
    show_history(n > 0 ? n : 0);
}

static void cmd_reply(const char *a) {
    const char *t = NULL;
    Message *m = pick_msg(a, &t, "/reply N текст");
    if (!m) return;
    if (!*t) { note("использование: /reply N текст"); return; }
    send_text(t, m->id, 0);
}

static void cmd_edit(const char *a) {
    const char *t = NULL;
    Message *m = pick_msg(a, &t, "/edit N текст");
    if (!m) return;
    if (m->author_id != S.me_id) { note("править можно только свои сообщения"); return; }
    if (!*t) { note("использование: /edit N текст"); return; }
    send_text(t, 0, m->id);
}

static void cmd_del(const char *a) {
    const char *t = NULL;
    Message *m = pick_msg(a, &t, "/del N yes");
    if (!m) return;
    if (m->author_id != S.me_id) { note("удалять можно только свои сообщения"); return; }
    if (!strcmp(t, "yes") || !strcmp(t, "y") || !strcmp(t, "да")) { delete_message(m->id); return; }
    char tt[200] = "?";
    open_title(tt, sizeof tt);
    note("удалить это сообщение? подтвердите: /del %s yes", a);
    put_msg(m, 0, tt);
}

static void cmd_react(const char *a) {
    const char *t = NULL;
    Message *m = pick_msg(a, &t, "/react N эмодзи|:код:|номер");
    if (!m) return;
    if (!*t) { note("использование: /react N эмодзи|:код:|номер"); return; }
    react(m, t);
}

static void cmd_join(const char *a) {
    if (!*a) { note("использование: /join N|имя (номера: /channels)"); return; }
    Channel *c = pick_channel(a, true);
    if (!c) return;
    char t[200];
    chan_title(c, t, sizeof t);
    if (is_dm(c)) { note("звонок в ЛС: /open %s, затем /call", t); return; }
    if (!is_voice(c)) { note("%s - текстовый канал: /open", t); return; }
    VoiceStatus vs;
    voice_status(&vs);
    if (vs.phase != VS_IDLE && vs.channel_id == c->id) { note("вы уже в %s; /leave - выйти", t); return; }
    if (!c->can_connect) { note("нет права подключаться к %s", t); return; }
    note("подключаюсь к %s", t);
    voice_join(c->guild_id, c->id);
    voice_status(&vs);
    C.vphase = vs.phase;                         /* "ожидание сервера" уже сказано этой строкой */
}

static void cmd_leave(const char *a) {
    (void)a;
    VoiceStatus vs;
    voice_status(&vs);
    if (vs.phase == VS_IDLE) { note("вы не в голосовом канале"); return; }
    voice_leave();
    note("голос: отключено");
    C.vphase = VS_IDLE;
}

static void cmd_mute(const char *a) {
    (void)a;
    voice_toggle_mute();
    VoiceStatus vs;
    voice_status(&vs);
    note("микрофон %s", vs.self_mute ? "выключен" : "включён");
}

static void cmd_deaf(const char *a) {
    (void)a;
    voice_toggle_deaf();
    VoiceStatus vs;
    voice_status(&vs);
    note("звук %s", vs.self_deaf ? "выключен" : "включён");
}

static void cmd_call(const char *a) {
    if (*a) { note("/call - без аргументов: звонок в открытую личную беседу (/dms, /open N, /call)"); return; }
    Channel *c = chan_find(S.open_ch);
    if (!c || !is_dm(c)) { note("звонок - из открытой личной беседы: /dms, /open N, /call"); return; }
    char t[200];
    chan_title(c, t, sizeof t);
    note("звоню %s", t);
    call_start(c->id);                          /* op 4, звонок собеседнику — после CALL_CREATE */
    VoiceStatus vs;
    voice_status(&vs);
    C.vphase = vs.phase;
}

static void cmd_accept(const char *a) {
    (void)a;
    if (!S.call.ringing) { note("входящего звонка нет"); return; }
    note("принимаю звонок");
    voice_join(0, S.call.channel_id);
    S.call.ringing = false;
    VoiceStatus vs;
    voice_status(&vs);
    C.vphase = vs.phase;
}

static void cmd_decline(const char *a) {
    (void)a;
    if (!S.call.ringing) { note("входящего звонка нет"); return; }
    if (!O.demo) call_decline(S.call.channel_id);
    S.call.ringing = false;
    note("звонок отклонён");
}

static void cmd_vol(const char *a) {
    VoiceStatus vs;
    voice_status(&vs);
    char who[128];
    size_t k = strcspn(a, " ");
    if (!k || k >= sizeof who) { note("использование: /vol имя|N процент (номера из /status)"); return; }
    memcpy(who, a, k);
    who[k] = 0;
    const char *pct = a + k;
    while (*pct == ' ') pct++;
    u64 uid = 0;
    int n = num_arg(who, NULL);
    if (n >= 1 && n <= C.nvlist) uid = C.vlist[n - 1];
    else if (n >= 0) { note("нет участника %d; номера: /status", n); return; }
    else {
        const VoiceState *v[32];
        int m = vs.phase != VS_IDLE ? voice_members(vs.channel_id, v, 32) : 0;
        for (int pass = 0; pass < 2 && !uid; pass++)
            for (int i = 0; i < m && !uid; i++) if (name_eq(vs_name(v[i]), who, pass == 1)) uid = v[i]->user_id;
        if (!uid) { note("в вашем голосовом канале нет \"%s\"; номера: /status", who); return; }
    }
    if (!*pct) { note("громкость %s: %d%%", user_name(uid), (int)(voice_get_volume(uid) * 100 + 0.5f)); return; }
    int p = num_arg(pct, NULL);
    if (p < 0 || p > 200) { note("громкость - число 0..200 (процентов)"); return; }
    voice_set_volume(uid, (float)p / 100.0f);    /* сам скажет "громкость ...: N%" */
}

static void cmd_vad(const char *a) {
    if (!strcmp(a, "+")) voice_vad_adjust(3);
    else if (!strcmp(a, "-")) voice_vad_adjust(-3);
    else if (*a) {
        char *e = NULL;
        long v = strtol(a, &e, 10);
        if (!e || *e || v < -70 || v > -10) { note("использование: /vad +|-|дБ (от -70 до -10)"); return; }
        voice_vad_adjust((int)v - voice_vad_db());
    }
    note("порог активации голосом: %d дБ%s", voice_vad_db(), O.ptt ? " (сейчас рация, --ptt)" : "");
}

static void cmd_ptt(const char *a) {
    (void)a;
    if (!O.ptt) { note("рация включается флагом --ptt"); return; }
    C.ptt_on = !C.ptt_on;
    voice_ptt(C.ptt_on);
    note("%s", C.ptt_on ? "рация: передача" : "рация: молчание");
}

static const char *phase_text(const VoiceStatus *vs) {
    switch (vs->phase) {
    case VS_WAITING: return "ожидание сервера";
    case VS_CONNECTING: return "подключение";
    case VS_DAVE: return "согласование шифрования DAVE";
    case VS_CONNECTED: return vs->dave ? "подключено, сквозное шифрование DAVE" : "подключено без DAVE: свой звук не отправляется";
    case VS_FAILED: return "ошибка";
    default: return "не в голосе";
    }
}

static void cmd_status(const char *a) {
    (void)a;
    char t[200];
    note("вы: %s | связь: %s%s", S.me_name ? S.me_name : "?", gw_status_text(), O.demo ? " (демо)" : "");
    if (open_title(t, sizeof t)) {
        const char *g = guild_name(S.open_ch ? chan_find(S.open_ch)->guild_id : 0);
        P("-!- открыт: %s%s%s%s, загружено сообщений: %d\n", t, g ? " [" : "", g ? g : "", g ? "]" : "", S.nmsgs);
    } else note("канал не открыт");
    VoiceStatus vs;
    voice_status(&vs);
    C.nvlist = 0;
    if (vs.phase == VS_IDLE) note("голос: не в голосовом канале; микрофон %s, звук %s", vs.self_mute ? "выкл" : "вкл", vs.self_deaf ? "выкл" : "вкл");
    else {
        Channel *c = chan_find(vs.channel_id);
        if (c) chan_title(c, t, sizeof t); else snprintf(t, sizeof t, "?");
        note("голос: %s - %s%s%s, пинг %d мс", t, phase_text(&vs), vs.error[0] ? ": " : "", vs.error, vs.ping);
        note("микрофон %s%s, звук %s, порог %d дБ%s", vs.self_mute ? "выкл" : "вкл", vs.mic_live ? " (идёт передача)" : "",
             vs.self_deaf ? "выкл" : "вкл", voice_vad_db(), O.ptt ? ", рация" : "");
        const VoiceState *v[32];
        int k = voice_members(vs.channel_id, v, 32);
        if (k) P("-!- участники (/vol N процент):\n");
        for (int i = 0; i < k; i++) {
            bool sp = false;
            for (int j = 0; j < vs.nspeaking; j++) if (vs.speaking[j] == v[i]->user_id) sp = true;
            C.vlist[C.nvlist++] = v[i]->user_id;
            P("  %3d  %s%s%s%s %d%%\n", C.nvlist, vs_name(v[i]), sp ? " (говорит)" : "",
              v[i]->self_mute || v[i]->mute ? " (мик. выкл)" : "", v[i]->self_deaf || v[i]->deaf ? " (звук выкл)" : "",
              (int)(voice_get_volume(v[i]->user_id) * 100 + 0.5f));
        }
    }
    if (S.call.ringing) call_note();
}

static void cmd_quit(const char *a) { (void)a; g_quit = true; }

/* ---------- смена голоса, паника, VLESS ---------- */
static const char *fx_name(VoiceFx m) { return m == VFX_LOW ? "ниже" : m == VFX_HIGH ? "выше" : "обычный (выкл)"; }

static void cmd_voice(const char *a) {
    if (!*a) voicefx_cycle();
    else if (!strcmp(a, "off") || !strcmp(a, "выкл")) voicefx_set(VFX_OFF);
    else if (!strcmp(a, "low") || !strcmp(a, "ниже")) voicefx_set(VFX_LOW);
    else if (!strcmp(a, "high") || !strcmp(a, "выше")) voicefx_set(VFX_HIGH);
    else { note("использование: /voice [off|low|high] (без аргумента - по кругу)"); return; }
    note("голос: %s", fx_name(voicefx_get()));
}

static void cmd_panic(const char *a) {
    (void)a;
    flush();
    panic_now();                                 /* не возвращается */
}

static const char *vl_text(VlessState s) {
    switch (s) {
    case VL_STARTING: return "запускается";
    case VL_UP: return "работает";
    case VL_DOWN: return "НЕ работает";
    default: return "выключен (трафик напрямую)";
    }
}

static void vless_hint(void) {
    const char *e = vless_error();
    note("VLESS: туннель не работает%s%s. Напрямую клиент не ходит. /vless retry - переподключить, "
         "/vless set - сменить ссылку, /vless off - отключить VLESS (напрямую)", e && *e ? ": " : "", e ? e : "");
}

static void vless_done(bool ok, const char *err, int ms) {
    if (ok) note("VLESS: туннель работает, задержка до Discord %d мс", ms);
    else note("VLESS: не работает: %s", err && *err ? err : "?");
}

static void cmd_vless(const char *a) {
    VlessState st = vless_state();
    if (!*a) {
        int ms = vless_latency_ms();
        const char *e = vless_error();
        if (ms >= 0 && st == VL_UP) note("VLESS: %s, задержка %d мс", vl_text(st), ms);
        else note("VLESS: %s%s%s", vl_text(st), st == VL_DOWN && e && *e ? ": " : "", st == VL_DOWN && e ? e : "");
        note("/vless retry - переподключить, /vless set - сменить ссылку, /vless off - отключить");
        return;
    }
    if (!strcmp(a, "retry")) {
        note("VLESS: перезапуск и проверка...");
        flush();
        bool ok = vless_restart();
        vless_done(ok, vless_error(), vless_latency_ms());
        C.vl = vless_state();
        return;
    }
    if (!strcmp(a, "set")) {
        C.await_link = true;
        note("вставьте ссылку vless:// и нажмите Enter (ввод скрыт; пустая строка - отмена)");
        flush();
        echo_set(false);
        return;
    }
    if (!strcmp(a, "off")) {
        if (st == VL_OFF) { note("VLESS уже выключен: трафик идёт напрямую"); return; }
        note("ВНИМАНИЕ: без VLESS трафик пойдёт напрямую, ваш IP увидит Discord. Подтвердите: /vless off yes");
        return;
    }
    if (!strcmp(a, "off yes") || !strcmp(a, "off да")) {
        vless_disable();
        C.vl = vless_state();
        note("VLESS выключен по вашему выбору: трафик идёт напрямую");
        return;
    }
    note("использование: /vless [retry|set|off]");
}

/* Строка после /vless set: ссылка. Не проходит через разбор команд, нигде не остаётся. */
static void take_link(char *line) {
    C.await_link = false;
    echo_set(true);
    char *p = line, *e = line + strlen(line);
    while (*p == ' ' || *p == '\t') p++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    if (!*p) { note("смена ссылки VLESS отменена"); return; }
    note("VLESS: проверяю ссылку (пробный запрос к Discord через туннель)...");
    flush();
    char err[256] = "";
    int ms = -1;
    bool ok = vless_set_link(p, err, sizeof err, &ms);
    char *safe = san(err, false);
    vless_done(ok, safe, ms);
    free(safe);
    C.vl = vless_state();
}

/* ======================= фаза 4: друзья, новая ЛС, поиск, файл ======================= */
static const char *const REL_WORD[] = { "", "друг", "заблокирован", "входящая заявка", "исходящая заявка" };
/* Номер N — позиция в /friends (массив отношений по id), иначе имя. */
static const Relation *pick_rel(const char *a, const char **rest) {
    const Relation *rs;
    int n = rel_list(&rs);
    int k = num_arg(a, rest);
    if (k >= 1 && k <= n) return &rs[k - 1];
    if (k >= 0) { note("нет записи %d в /friends", k); return NULL; }
    if (rest) *rest = "";
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) if (name_eq(user_name(rs[i].id), a, pass == 1)) return &rs[i];
    note("\"%s\" нет в /friends", a);
    return NULL;
}
static void cmd_friends(const char *a) {
    const Relation *rs;
    int n = rel_list(&rs);
    bool online = !strcmp(a, "online"), pend = !strcmp(a, "pending"), blk = !strcmp(a, "blocked");
    if (*a && !online && !pend && !blk && strcmp(a, "all")) { note("использование: /friends [all|online|pending|blocked]"); return; }
    P("-!- друзья (/dm N - написать; /friend accept N, /friend remove N yes):\n");
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const Relation *r = &rs[i];
        if (online && (r->type != REL_FRIEND || r->status == PS_OFFLINE)) continue;
        if (pend && r->type != REL_IN && r->type != REL_OUT) continue;
        if (blk && r->type != REL_BLOCKED) continue;
        if (!*a && r->type == REL_BLOCKED) continue;          /* как в Discord: заблокированные — отдельно */
        P("  %3d  %s - %s\n", i + 1, user_name(r->id), r->type == REL_FRIEND ? presence_name(r->status) : REL_WORD[r->type]);
        shown++;
    }
    if (!shown) note("пусто");
}
static void cmd_friend(const char *a) {
    bool acc = !strncmp(a, "accept ", 7), rem = !strncmp(a, "remove ", 7);
    if (!acc && !rem) { note("использование: /friend accept N | /friend remove N yes"); return; }
    const char *rest = "";
    const Relation *r = pick_rel(a + 7, &rest);
    if (!r) return;
    u64 id = r->id;
    if (acc) {
        if (r->type != REL_IN) { note("%s - %s, принимать нечего", user_name(id), REL_WORD[r->type]); return; }
        rel_accept(id);
        if (O.demo) note("заявка принята (демо)");
        return;
    }
    while (*rest == ' ') rest++;
    if (strcmp(rest, "yes")) { note("%s - %s; подтвердите: /friend remove N yes", user_name(id), REL_WORD[r->type]); return; }
    rel_remove(id);
    if (O.demo) note("удалено (демо)");
}
static void cmd_dm(const char *a) {
    if (!*a) { note("использование: /dm N|имя (номера из /friends)"); return; }
    const Relation *r = pick_rel(a, NULL);
    if (!r) return;
    if (r->type == REL_BLOCKED) { note("%s заблокирован", user_name(r->id)); return; }
    dm_open(r->id);
}
void cli_open_dm(u64 channel_id) {
    Channel *c = chan_find(channel_id);
    if (c) open_chan(c, false);
}
static void cmd_search(const char *a) {
    if (!strcmp(a, "close")) { search_close(); C.search_wait = false; note("поиск закрыт"); return; }
    Channel *c = chan_find(S.open_ch);
    if (!*a || !c) { note("использование: /search текст (в открытом канале: по серверу или по ЛС), /search close"); return; }
    C.search_wait = true;
    C.search_idx_said = false;
    search_start(c->guild_id, c->guild_id ? 0 : c->id, a);
    note("поиск \"%s\"...", a);
}
static void search_report(void) {
    const SearchState *s = search_state();
    if (s->indexing && !C.search_idx_said) { C.search_idx_said = true; note("поиск: индекс строится, повторю сам"); }
    if (!C.search_wait || s->busy) return;
    C.search_wait = false;
    if (s->error[0]) { note("поиск: %s", s->error); return; }
    P("-!- найдено %d%s:\n", s->total, s->total > s->nhits ? " (показана первая страница)" : "");
    for (int i = 0; i < s->nhits; i++) {
        char w[200] = "?";
        Channel *ch = chan_find(s->hits[i].channel_id);
        if (ch) chan_title(ch, w, sizeof w);
        put_msg(&s->hits[i].m, 0, w);
    }
}
static void cmd_file(const char *a) {
    char path[1024];
    const char *text = "";
    if (*a == '"') {
        const char *e = strchr(a + 1, '"');
        if (!e) { note("использование: /file \"путь\" [подпись]"); return; }
        snprintf(path, sizeof path, "%.*s", (int)(e - a - 1), a + 1);
        text = e + 1;
        while (*text == ' ') text++;
    } else snprintf(path, sizeof path, "%s", a);
    if (!path[0]) { note("использование: /file путь  или  /file \"путь с пробелами\" подпись (до 10 МБ)"); return; }
    char err[160];
    if (!send_file(S.open_ch, path, text, err, sizeof err)) { note("файл: %s", err); return; }
    if (!O.demo) note("файл: отправка...");
}

/* Капча на запросе (net.c): /captcha — открыть окно проверки, /captcha cancel — отказаться. */
static void cmd_captcha(const char *a) {
    const CaptchaState *cs = net_captcha();
    if (!cs->pending) { note("проверка (капча) сейчас не нужна"); return; }
    if (!strcmp(a, "cancel")) { net_captcha_cancel(); return; }
    if (cs->open) { note("окно проверки уже открыто - пройдите её там"); return; }
    net_captcha_open();
    if (cs->error[0]) note("капча: %s", cs->error);
    else if (cs->open) note("окно проверки открыто: пройдите её, запрос (%s) повторится сам", cs->what);
}

typedef struct { const char *name; void (*fn)(const char *); } Cmd;
static const Cmd CMDS[] = {
    {"friends", cmd_friends}, {"friend", cmd_friend}, {"dm", cmd_dm}, {"search", cmd_search}, {"file", cmd_file},
    {"captcha", cmd_captcha},
    {"help", cmd_help}, {"servers", cmd_servers}, {"channels", cmd_channels}, {"dms", cmd_dms},
    {"open", cmd_open}, {"history", cmd_history}, {"reply", cmd_reply}, {"edit", cmd_edit},
    {"del", cmd_del}, {"react", cmd_react}, {"join", cmd_join}, {"leave", cmd_leave},
    {"mute", cmd_mute}, {"deaf", cmd_deaf}, {"call", cmd_call}, {"accept", cmd_accept},
    {"decline", cmd_decline}, {"vol", cmd_vol}, {"vad", cmd_vad}, {"ptt", cmd_ptt},
    {"status", cmd_status}, {"quit", cmd_quit}, {"exit", cmd_quit},
    {"voice", cmd_voice}, {"panic", cmd_panic}, {"vless", cmd_vless},
};

/* ======================= строки ввода ======================= */
#ifdef _WIN32
/* Из канала строка может прийти не в UTF-8 (cmd пишет в кодовой странице консоли). */
static char *line_utf8(const char *s) {
    if (input_is_console() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) > 0) return xstrdup(s);
    UINT cp = GetConsoleCP();
    if (!cp || cp == CP_UTF8) cp = CP_ACP;
    int wn = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
    if (wn <= 0) return xstrdup(s);
    WCHAR *w = xmalloc((size_t)wn * sizeof(WCHAR));
    MultiByteToWideChar(cp, 0, s, -1, w, wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    char *u = xmalloc(un > 0 ? (size_t)un : 1);
    if (un <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, wn, u, un, NULL, NULL)) u[0] = 0;
    wipe(w, (size_t)wn * sizeof(WCHAR));
    free(w);
    return u;
}
#else
static char *line_utf8(const char *s) { return xstrdup(s); }
#endif

static void exec_line(const char *raw) {
    char *u = line_utf8(raw);
    char *s = san(u, false);                     /* свой ввод тоже без управляющих символов */
    wipe(u, strlen(u));
    free(u);
    char *p = s, *e = s + strlen(s);
    while (*p == ' ' || *p == '\t') p++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if (!*p) { free(s); return; }
    if (p[0] == '/' && p[1] != '/') {
        char *arg = p + 1 + strcspn(p + 1, " ");
        if (*arg) *arg++ = 0;
        while (*arg == ' ') arg++;
        for (char *q = p + 1; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
        const Cmd *hit = NULL;
        for (size_t i = 0; i < sizeof CMDS / sizeof *CMDS; i++) if (!strcmp(CMDS[i].name, p + 1)) hit = &CMDS[i];
        if (hit) hit->fn(arg);
        else note("неизвестная команда %s; /help - список команд", p);
    } else {
        if (!S.ready) note("нет связи с Discord, не отправлено");
        else send_text(p[0] == '/' ? p + 1 : p, 0, 0);
    }
    wipe(s, strlen(s));
    free(s);
}

/* Пока не готовы (нет READY, грузится история) строки ждут в буфере; /quit, /help, /status — сразу. */
static bool line_free(const char *p, size_t n) {
    /* /vless и /panic — без READY: при неработающем туннеле READY не придёт вовсе */
    static const char *const FREE[] = { "/quit", "/exit", "/help", "/status", "/panic", "/vless", "/voice" };
    while (n && (*p == ' ' || *p == '\t')) p++, n--;
    for (size_t i = 0; i < sizeof FREE / sizeof *FREE; i++) {
        size_t k = strlen(FREE[i]);
        if (n >= k && !strncmp(p, FREE[i], k) && (n == k || p[k] == ' ' || p[k] == '\r')) return true;
    }
    return false;
}

static void process_lines(void) {
    while (C.in.len && !g_quit) {
        char *nl = memchr(C.in.p, '\n', C.in.len);
        size_t n = nl ? (size_t)(nl - C.in.p) : C.in.len;
        if (!nl && !C.eof && C.in.len < PEND_MAX) break;   /* строка ещё не дописана */
        if (!C.await_link && (C.loading || !S.ready) && !line_free(C.in.p, n)) break;
        size_t take = nl ? n + 1 : n;
        if (n > IN_MAX) n = IN_MAX;
        char *line = xmalloc(n + 1);
        memcpy(line, C.in.p, n);
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        wipe(C.in.p, take);
        memmove(C.in.p, C.in.p + take, C.in.len - take);
        C.in.len -= take;
        C.in.p[C.in.len] = 0;
        if (C.await_link) take_link(line); else exec_line(line);
        wipe(line, n);
        free(line);
    }
    if (!C.in.len) buf_wipe(&C.in);
}

/* ======================= платформа: ввод и вывод ======================= */
#ifdef _WIN32
static HANDLE w_in = INVALID_HANDLE_VALUE, w_out = INVALID_HANDLE_VALUE, w_ev, rd_thr;
static bool w_con_in, w_con_out;
static UINT w_cp;
static Buf ed;                                   /* редактируемая строка (только консоль) */
static bool shown;                               /* приглашение и строка сейчас на экране */
static int shown_cells;
static WCHAR hi_sur;
/* поток-читатель для канала/файла */
static CRITICAL_SECTION rd_cs;
static CONDITION_VARIABLE rd_cv;
static char rd_buf[16384];
static size_t rd_len;
static bool rd_eof;

static bool input_is_console(void) { return w_con_in; }

static void con_write(const char *s, size_t n) {
    while (n) {
        size_t chunk = n > 16384 ? 16384 : n;
        while (chunk < n && ((unsigned char)s[chunk] & 0xC0) == 0x80) chunk--;   /* не резать символ */
        int wn = MultiByteToWideChar(CP_UTF8, 0, s, (int)chunk, NULL, 0);
        if (wn > 0) {
            WCHAR *w = xmalloc((size_t)wn * sizeof(WCHAR));
            MultiByteToWideChar(CP_UTF8, 0, s, (int)chunk, w, wn);
            DWORD done;
            WriteConsoleW(w_out, w, (DWORD)wn, &done, NULL);
            wipe(w, (size_t)wn * sizeof(WCHAR));
            free(w);
        }
        s += chunk;
        n -= chunk;
    }
}
static void out_raw(const char *s, size_t n) {
    if (w_con_out) { con_write(s, n); return; }
    DWORD w;
    while (n) {
        DWORD chunk = n > 65536 ? 65536 : (DWORD)n;
        if (!WriteFile(w_out, s, chunk, &w, NULL) || !w) return;
        s += w;
        n -= w;
    }
}

static void ed_show(void) {
    if (!w_con_in || !w_con_out || shown) return;
    char p[256], t[200];
    if (C.await_link) snprintf(p, sizeof p, "vless:// (скрыто) ");
    else if (open_title(t, sizeof t)) snprintf(p, sizeof p, "[%s] ", t); else snprintf(p, sizeof p, "> ");
    con_write(p, strlen(p));
    shown_cells = str_width(p);
    if (C.await_link) {                          /* маска: по звёздочке на символ */
        for (size_t i = 0; i < ed.len; i++) if (((unsigned char)ed.p[i] & 0xC0) != 0x80) { con_write("*", 1); shown_cells++; }
    } else {
        if (ed.len) con_write(ed.p, ed.len);
        shown_cells += ed.p ? str_width(ed.p) : 0;
    }
    shown = true;
}
/* Стереть приглашение и строку: от позиции курсора назад на shown_cells клеток. */
static void ed_hide(void) {
    if (!shown) return;
    shown = false;
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (!GetConsoleScreenBufferInfo(w_out, &bi) || bi.dwSize.X <= 0) return;
    long W = bi.dwSize.X;
    long lin = (long)bi.dwCursorPosition.Y * W + bi.dwCursorPosition.X - shown_cells;
    if (lin < 0) lin = 0;
    COORD st = { (SHORT)(lin % W), (SHORT)(lin / W) };
    DWORD done;
    FillConsoleOutputCharacterW(w_out, L' ', (DWORD)shown_cells + 1, st, &done);
    SetConsoleCursorPosition(w_out, st);
}

static void ed_char(u32 cp) {
    if (ed.len + 4 > IN_MAX) return;
    char u[4];
    int n = utf8_enc(cp, u);
    buf_put(&ed, u, (size_t)n);
    if (!shown) ed_show();
    else if (C.await_link) { con_write("*", 1); shown_cells++; }    /* ссылка VLESS: маска вместо эха */
    else { con_write(u, (size_t)n); shown_cells += cp_width(cp); }
    wipe(u, sizeof u);
}

static void ed_key(const KEY_EVENT_RECORD *k) {
    WCHAR wc = k->uChar.UnicodeChar;
    WORD vk = k->wVirtualKeyCode;
    DWORD st = k->dwControlKeyState;
    bool ctrl = st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED), alt = st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
    if (ctrl && alt && wc >= 0x20) ctrl = false;                /* AltGr — ввод символа */
    if (vk == VK_RETURN) {
        if (!shown) ed_show();
        con_write("\n", 1);                                     /* введённая строка остаётся на экране */
        shown = false;
        if (C.in.len + ed.len + 1 <= PEND_MAX) { buf_put(&C.in, ed.p ? ed.p : "", ed.len); buf_put(&C.in, "\n", 1); }
        buf_wipe(&ed);
        return;
    }
    if (vk == VK_BACK) {
        if (!ed.len) return;
        size_t i = ed.len;
        do i--; while (i && ((unsigned char)ed.p[i] & 0xC0) == 0x80);
        ed_hide();
        wipe(ed.p + i, ed.len - i);
        ed.len = i;
        ed.p[i] = 0;
        ed_show();
        return;
    }
    if (vk == VK_ESCAPE || (ctrl && vk == 'U')) { ed_hide(); buf_wipe(&ed); ed_show(); return; }
    if (ctrl && vk == 'C') { c_stop = 1; return; }
    if (ctrl && (vk == 'D' || vk == 'Z') && !ed.len) { c_stop = 1; return; }
    if (ctrl || !wc) return;
    if (wc >= 0xD800 && wc <= 0xDBFF) { hi_sur = wc; return; }
    if (wc >= 0xDC00 && wc <= 0xDFFF) {
        if (hi_sur) ed_char(0x10000 + (((u32)hi_sur - 0xD800) << 10) + ((u32)wc - 0xDC00));
        hi_sur = 0;
        return;
    }
    if (wc == '\t') wc = ' ';
    if (wc >= 0x20) ed_char(wc);
}

/* ---------- паник-клавиша в редакторе строки ----------
 * Каждое нажатие (не автоповтор и не одиночный модификатор) уходит в panic_key. Нажатие паник-клавиши
 * засчитывается в серию, а его обычное действие откладывается: если серия прервалась другой клавишей
 * или прошло 1,5 с — отложенные нажатия выполняются как обычно. */
static KEY_EVENT_RECORD pend_keys[2];
static int npend;
static i64 pend_until;
static WORD held_vk;                             /* зажатая клавиша: её автоповтор — не новые нажатия */

static void pend_flush(void) {
    int n = npend;
    npend = 0;
    pend_until = 0;
    for (int i = 0; i < n; i++) ed_key(&pend_keys[i]);
    SecureZeroMemory(pend_keys, sizeof pend_keys);
}

static bool is_modifier(WORD vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_CAPITAL ||
           vk == VK_LWIN || vk == VK_RWIN || vk == VK_NUMLOCK || vk == VK_SCROLL;
}

static Key key_of(const KEY_EVENT_RECORD *k) {
    WCHAR wc = k->uChar.UnicodeChar;
    WORD vk = k->wVirtualKeyCode;
    DWORD st = k->dwControlKeyState;
    bool ctrl = st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED), alt = st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
    if (ctrl && alt && wc >= 0x20) ctrl = alt = false;          /* AltGr */
    Key r = { .code = K_HOME, .ctrl = ctrl, .alt = alt, .shift = (st & SHIFT_PRESSED) != 0 };   /* K_HOME — «прочая клавиша» */
    if (vk >= VK_F1 && vk <= VK_F12) r.code = K_F1 + (vk - VK_F1);
    else if (vk == VK_ESCAPE) r.code = K_ESC;
    else if (vk == VK_TAB) r.code = K_TAB;
    else if (vk == VK_RETURN) r.code = K_ENTER;
    else if (ctrl && vk >= 'A' && vk <= 'Z') { r.code = K_CHAR; r.ch = (u32)(vk - 'A' + 'a'); }
    else if (wc >= 0x20 && (wc < 0xD800 || wc > 0xDFFF)) { r.code = K_CHAR; r.ch = wc; }
    return r;
}

static void con_key(const KEY_EVENT_RECORD *k, bool repeat) {
    if (!repeat && !is_modifier(k->wVirtualKeyCode)) {
        Key key = key_of(k);
        if (panic_key(&key)) {                   /* третье нажатие сюда не возвращается */
            if (npend < 2) pend_keys[npend++] = *k;
            pend_until = now_ms() + 1500;
            return;
        }
    }
    if (npend) pend_flush();
    ed_key(k);
}

static void con_read(void) {
    INPUT_RECORD rec[64];
    DWORD avail = 0, got = 0;
    while (C.in.len < PEND_MAX && GetNumberOfConsoleInputEvents(w_in, &avail) && avail) {
        if (!ReadConsoleInputW(w_in, rec, avail < 64 ? avail : 64, &got) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (rec[i].EventType != KEY_EVENT) continue;                /* мышь, фокус */
            const KEY_EVENT_RECORD *ke = &rec[i].Event.KeyEvent;
            if (!ke->bKeyDown) { if (ke->wVirtualKeyCode == held_vk) held_vk = 0; continue; }
            bool repeat = !is_modifier(ke->wVirtualKeyCode) && ke->wVirtualKeyCode == held_vk;
            if (!is_modifier(ke->wVirtualKeyCode)) held_vk = ke->wVirtualKeyCode;
            WORD rep = ke->wRepeatCount ? ke->wRepeatCount : 1;
            for (WORD r = 0; r < rep; r++) con_key(ke, repeat || r > 0);
        }
    }
    SecureZeroMemory(rec, sizeof rec);
}

static DWORD WINAPI rd_main(LPVOID arg) {
    (void)arg;
    char b[4096];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(w_in, b, sizeof b, &got, NULL);
        EnterCriticalSection(&rd_cs);
        if (!ok || !got) { rd_eof = true; SetEvent(w_ev); LeaveCriticalSection(&rd_cs); break; }
        while (rd_len + got > sizeof rd_buf) SleepConditionVariableCS(&rd_cv, &rd_cs, INFINITE);
        memcpy(rd_buf + rd_len, b, got);
        rd_len += got;
        SetEvent(w_ev);
        LeaveCriticalSection(&rd_cs);
    }
    SecureZeroMemory(b, sizeof b);
    return 0;
}
static void rd_take(void) {
    EnterCriticalSection(&rd_cs);
    size_t room = C.in.len < PEND_MAX ? PEND_MAX - C.in.len : 0, n = rd_len < room ? rd_len : room;
    if (n) {
        buf_put(&C.in, rd_buf, n);
        memmove(rd_buf, rd_buf + n, rd_len - n);
        rd_len -= n;
        WakeConditionVariable(&rd_cv);
    }
    if (!rd_len) { if (rd_eof) C.eof = true; else ResetEvent(w_ev); }
    LeaveCriticalSection(&rd_cs);
}

static BOOL WINAPI on_ctrl(DWORD t) {
    if (t != CTRL_C_EVENT && t != CTRL_BREAK_EVENT) return FALSE;
    c_stop = 1;
    wake_signal(g_wake);
    return TRUE;
}

static void plat_cli_init(void) {
    DWORD m;
    w_in = GetStdHandle(STD_INPUT_HANDLE);
    w_out = GetStdHandle(STD_OUTPUT_HANDLE);
    w_con_out = w_out && w_out != INVALID_HANDLE_VALUE && GetConsoleMode(w_out, &m);
    C.tty = w_con_out;
    w_cp = GetConsoleOutputCP();
    if (w_cp) SetConsoleOutputCP(CP_UTF8);       /* на время работы; при выходе вернём */
    w_con_in = w_in && w_in != INVALID_HANDLE_VALUE && GetConsoleMode(w_in, &m);
    if (!w_con_in) {
        if (!w_in || w_in == INVALID_HANDLE_VALUE) C.eof = true;
        else {
            InitializeCriticalSection(&rd_cs);
            InitializeConditionVariable(&rd_cv);
            w_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
            rd_thr = w_ev ? CreateThread(NULL, 64 * 1024, rd_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
            if (!rd_thr) C.eof = true;
        }
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
}
static void plat_cli_shutdown(void) {
    if (rd_thr) { CancelSynchronousIo(rd_thr); CloseHandle(rd_thr); rd_thr = NULL; }   /* поток завершится с процессом */
    SetConsoleCtrlHandler(on_ctrl, FALSE);
    if (w_cp) SetConsoleOutputCP(w_cp);
    buf_wipe(&ed);
}
static void plat_read(void) {
    if (w_con_in) con_read();
    else if (w_ev && !C.eof) rd_take();
}
static int plat_waitable(Waitable *out) {
    if (w_con_in) { out[0] = (Waitable){ (intptr_t)w_in, W_IN }; return 1; }
    if (w_ev) { out[0] = (Waitable){ (intptr_t)w_ev, W_IN }; return 1; }
    return 0;
}
static void plat_tick(void) { if (npend && now_ms() >= pend_until) { pend_flush(); flush(); } }
static i64 plat_next_wake(void) { return npend ? pend_until : INT64_MAX; }
/* Windows: маску ссылки рисует свой редактор строки (ed_char/ed_show) */
static void echo_set(bool on) { (void)on; }

#else /* POSIX */

static void ed_show(void) {}
static void ed_hide(void) {}

static void out_raw(const char *s, size_t n) {
    while (n) {
        ssize_t w = write(1, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}
static void on_sigint(int sig) { (void)sig; c_stop = 1; wake_signal(g_wake); }   /* write() в eventfd безопасен */

/* Паника в каноническом режиме: отдельные клавиши не видны, поэтому — тройной Ctrl+\ (SIGQUIT) за 1,5 с.
 * В обработчике только async-signal-safe: clock_gettime, счётчик, флаг и write() в eventfd пробуждения.
 * Сама паника (затирание, хранилища, очистка экрана) — в главном потоке (cli_tick). Без --panic Ctrl+\
 * ничего не делает (и не завершает процесс без очистки). Любой введённый текст сбрасывает серию. */
static volatile sig_atomic_t q_count;
static struct timespec q_first;
static void on_sigquit(int sig) {
    (void)sig;
    int saved = errno;
    if (panic_key_name()[0]) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long ms = (long)(t.tv_sec - q_first.tv_sec) * 1000 + (t.tv_nsec - q_first.tv_nsec) / 1000000;
        if (!q_count || ms > 1500) { q_count = 1; q_first = t; }
        else if (++q_count >= 3) { q_count = 0; c_panic = 1; wake_signal(g_wake); }
    }
    errno = saved;
}

static struct termios echo_saved;
static bool echo_off;
static void echo_set(bool on) {
    if (!isatty(0)) return;
    if (!on && !echo_off) {
        struct termios t;
        if (tcgetattr(0, &echo_saved)) return;
        t = echo_saved;
        t.c_lflag &= (tcflag_t)~(ECHO | ECHOE | ECHOK);
        t.c_lflag |= ECHONL;                     /* перевод строки всё же видно */
        if (!tcsetattr(0, TCSANOW, &t)) echo_off = true;
    } else if (on && echo_off) {
        tcsetattr(0, TCSANOW, &echo_saved);
        echo_off = false;
    }
}

static void plat_cli_init(void) {
    C.tty = isatty(1);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_sigquit;
    sigaction(SIGQUIT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}
static void plat_cli_shutdown(void) { echo_set(true); }
static void plat_tick(void) {}
static i64 plat_next_wake(void) { return INT64_MAX; }
static void plat_read(void) {
    char b[4096];
    struct pollfd p = { 0, POLLIN, 0 };
    while (!C.eof && C.in.len < PEND_MAX && poll(&p, 1, 0) > 0) {
        if (p.revents & POLLNVAL) { C.eof = true; break; }
        size_t room = PEND_MAX - C.in.len;
        ssize_t r = read(0, b, room < sizeof b ? room : sizeof b);
        if (r > 0) q_count = 0;                  /* введён текст — серия Ctrl+\ прервана */
        if (r > 0) buf_put(&C.in, b, (size_t)r);
        else if (r == 0 || (errno != EINTR && errno != EAGAIN)) C.eof = true;
        else break;
    }
    wipe(b, sizeof b);
}
static int plat_waitable(Waitable *out) { out[0] = (Waitable){ 0, POLLIN }; return 1; }
#endif

/* ======================= интерфейс для main.c ======================= */
void cli_init(void) {
    plat_cli_init();
    C.vphase = VS_IDLE;
    note("Astra DS, консольный режим. /help - команды, /quit - выход");
    flush();
}

void cli_shutdown(void) {
    flush();
    ed_hide();
    plat_cli_shutdown();
    buf_wipe(&C.in);
    buf_wipe(&C.out);
    free(C.list);
    C.list = NULL;
    C.nlist = C.caplist = 0;
}

void cli_handle_input(void) {
    if (c_panic) panic_now();
    plat_read();
    process_lines();
    flush();
}

int cli_waitables(Waitable *out, int max) {
    if (max < 1 || C.eof || C.in.len >= PEND_MAX) return 0;
    return plat_waitable(out);
}

void cli_tick(void) {
    if (c_panic) panic_now();
    if (c_stop) g_quit = true;
    plat_tick();
    VlessState vl = vless_state();
    if (vl != C.vl) {
        if (vl == VL_DOWN) { bell(); vless_hint(); }
        else if (vl == VL_UP) note("VLESS: туннель работает, задержка до Discord %d мс", vless_latency_ms());
        else if (vl == VL_OFF && C.vl != VL_OFF) note("VLESS выключен: трафик идёт напрямую");
        C.vl = vl;
    }
    i64 now = now_ms();
    if (C.ack_at && now >= C.ack_at) { C.ack_at = 0; send_ack(); }
    process_lines();                             /* строки, ждавшие READY или истории */
    VoiceStatus vs;
    voice_status(&vs);
    if (vs.phase != C.vphase || (vs.phase == VS_CONNECTED && vs.dave != C.vdave)) {
        char t[200] = "?";
        Channel *c = chan_find(vs.channel_id);
        if (c) chan_title(c, t, sizeof t);
        if (vs.phase == VS_IDLE) note("голос: отключено");
        else if (vs.phase == VS_FAILED) note("голос: %s - ошибка: %s", t, vs.error[0] ? vs.error : "?");
        else note("голос: %s - %s", t, phase_text(&vs));
        C.vphase = vs.phase;
        C.vdave = vs.dave;
    }
    if (S.call.ringing && !C.ringing_was) call_note();
    C.ringing_was = S.call.ringing;
    if (C.dirty) { C.dirty = false; check_changes(); search_report(); }
    if (C.eof && !C.in.len && !C.loading && !http_busy()) g_quit = true;
    flush();
}

i64 cli_next_wake(void) {
    i64 t = plat_next_wake();
    return C.ack_at && C.ack_at < t ? C.ack_at : t;
}

void cli_dirty(void) { C.dirty = true; }

void cli_notice(const char *text) {
    if (text[0] == '\a') {
        if (S.call.ringing) { call_note(); return; }   /* своя строка с /accept вместо подсказок клавиш */
        bell();
        text++;
    }
    note("%s", text);
}

void cli_on_ready(void) {
    int dms = 0;
    for (int i = 0; i < S.nchans; i++) if (is_dm(&S.chans[i])) dms++;
    note("в сети как %s: серверов %d, личных бесед %d. /servers, /dms, /help", S.me_name ? S.me_name : "?", S.nguilds, dms);
    if (O.demo) note("демо-режим: данные ненастоящие, в сеть ничего не уходит");
    Channel *c = S.open_ch ? chan_find(S.open_ch) : NULL;
    if (c) { S.open_ch = 0; open_chan(c, true); }  /* переподключение: догрузить пропущенное */
    else S.open_ch = 0;
}

void cli_on_message(u64 channel_id, bool mention) {
    if (channel_id == S.open_ch) {
        print_new();
        C.ack_at = now_ms() + 1500;
        return;
    }
    Channel *c = chan_find(channel_id);
    if (!c || c->muted) return;
    Guild *g = c->guild_id ? guild_find(c->guild_id) : NULL;
    if (g && g->muted && !mention) return;
    char t[200];
    chan_title(c, t, sizeof t);
    if (!c->guild_id) {                          /* ЛС: одна строка, пока беседу не откроют */
        if (seen_add(c->id)) { bell(); note("личное сообщение в %s (/open %s)", t, t); }
    } else if (mention) {
        bell();
        note("упоминание в %s [%s]", t, g && g->name ? g->name : "?");
    } else if (c->guild_id == C.guild && seen_add(c->id)) {
        note("новое в %s", t);                   /* только текущий сервер и один раз до открытия */
    }
}
