/* Модель данных: только то, что нужно интерфейсу. Все строки из сети — через san(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mc.h"
#include "golive.h"                              /* только при ASTRA_VIDEO; иначе пусто */

Store S;
static int notify_of(const Channel *c);
static void pend_arrived(const cJSON *d);
static void cdn_flush(i64 now);
static void cdn_reset(void);
static i64 slow_next_wake(void);
static int pend_find(u64 nonce);
static void pend_on_clear(void);
static void demo_slow_tick(i64 now);
static u64 demo_slow;                           /* демо «!slow»: местное сообщение ждёт 3 с */
static i64 demo_slow_at;
static u64 demo_del_th;                         /* демо «!delthread» в ветке: через 3 с — THREAD_DELETE */
static i64 demo_del_at;
static i64 cdn_next_wake(void);

#define P_ADMIN   (1ULL << 3)
#define P_VIEW    (1ULL << 10)
#define P_SEND    (1ULL << 11)
#define P_HISTORY (1ULL << 16)
#define P_CONNECT (1ULL << 20)
#define P_SPEAK   (1ULL << 21)
#define MAX_MSGS  100

/* ---------- освобождение ---------- */
static void chan_free(Channel *c) { free(c->name); free(c->topic); free(c->recipients); free(c->ow); }
static void guild_free(Guild *g) { free(g->name); free(g->roles); free(g->my_roles); }
static void embeds_free(Message *m);
static void poll_free(Poll *p);
static void embeds_parse(Message *m, const cJSON *arr);
static Poll *poll_parse(const cJSON *po);
static void poll_vote_ev(const cJSON *d, int delta);
static void notify_msg(const cJSON *d, const Channel *c);
static i64 status_next_wake(void);
static void cst_from_proto(const u8 *p, size_t n);
bool ignored(u64 uid);
void vcs_clear_hook(void);
static void vcs_event(const char *t, const cJSON *d);
static void vcs_put(u64 cid, const char *s);
static i64 focus_next_wake(void);
i64 focus_until(void);
static u32 banner_color(const char *hash);
static void comps_free(Message *m);
static void comps_parse(Message *m, const cJSON *rows, u64 app);
static void dm_flags(Channel *c, const cJSON *o);
static void folders_ready(const cJSON *d);
static void folders_slim(const char *a, size_t n);
static void folders_event(const cJSON *d);
static void ix_event(const char *t, const cJSON *d);
static void ix_tick(i64 now);
static u64 ix_nonce;                            /* №55: ждём ответа бота (определение — ниже) */
static i64 ix_until;
static bool alert_text(const char *s);
static void thread_upsert(const cJSON *o, u64 gid);
static void thread_event(const char *t, const cJSON *d);
static void msg_free(Message *m) {
    free(m->author); free(m->content); free(m->ref_author); free(m->ref_text); free(m->raw);
    embeds_free(m);
    poll_free(m->poll);
    comps_free(m);
    for (int i = 0; i < m->nreacts; i++) free(m->reacts[i].emoji);
    free(m->reacts);
    memset(m, 0, sizeof *m);
}
void msgs_clear(void) {
    for (int i = 0; i < S.nmsgs; i++) msg_free(&S.msgs[i]);
    free(S.msgs);
    S.msgs = NULL;
    S.nmsgs = S.capmsgs = 0;
    S.history_end = false;
    S.detached = false;
    S.jump_id = 0;
    pend_on_clear();
    cdn_reset();                                /* ревью 10 М3: таблица CDN — только текущей ленты */
}
/* ревью 10 М1: ui.c кладёт текст «не отправлено» в черновик канала (если черновик пуст). Без ui — некуда. */
__attribute__((weak)) bool ui_draft_restore(u64 ch, const char *text) { (void)ch; (void)text; return false; }
void store_reset(void) {
    for (int i = 0; i < S.nusers; i++) free(S.users[i].name);
    for (int i = 0; i < S.nguilds; i++) guild_free(&S.guilds[i]);
    for (int i = 0; i < S.nchans; i++) chan_free(&S.chans[i]);
    for (int i = 0; i < S.nvs; i++) free(S.vs[i].name);
    free(S.users); free(S.guilds); free(S.chans); free(S.vs);
    free(S.me_name); free(S.session_id);
    free(S.rels);
    msgs_clear();
    search_close();
    pins_close();
    mentions_close();
    profile_close();
    vcs_clear_hook();
    memset(&S, 0, sizeof S);
}

/* ---------- пользователи ---------- */
static int user_cmp(const void *a, const void *b) {
    u64 x = ((const User *)a)->id, y = ((const User *)b)->id;
    return x < y ? -1 : x > y;
}
static User *user_find(u64 id) {
    User k = { .id = id };
    return S.nusers ? bsearch(&k, S.users, (size_t)S.nusers, sizeof(User), user_cmp) : NULL;
}
static const char *display_name(const cJSON *u) {
    const char *n = jstr(u, "global_name");
    if (!n || !*n) n = jstr(u, "username");
    return n ? n : "?";
}
static void user_put(u64 id, const char *name) {
    if (!id) return;
    User *u = user_find(id);
    if (u) { free(u->name); u->name = san(name, false); return; }
    if (S.nusers == S.capusers) {
        S.capusers = S.capusers ? S.capusers * 2 : 64;
        S.users = xrealloc(S.users, (size_t)S.capusers * sizeof(User));
    }
    int i = S.nusers++;
    while (i > 0 && S.users[i - 1].id > id) { S.users[i] = S.users[i - 1]; i--; }
    S.users[i] = (User){ .id = id, .name = san(name, false) };   /* avatar/color — нули, не мусор кучи */
}
/* Хэш картинки CDN: принимается только 32 hex или a_+32 hex, иначе out = "". */
static void hash_copy(char out[35], const char *h) {
    out[0] = 0;
    if (!h) return;
    size_t n = strlen(h), o = (n == 34 && h[0] == 'a' && h[1] == '_') ? 2 : 0;
    if (n != 32 + o) return;
    for (size_t i = o; i < n; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return;
    memcpy(out, h, n + 1);
}
static void user_avatar_set(u64 id, const cJSON *u) {
    User *x = user_find(id);
    if (x && jget(u, "avatar")) hash_copy(x->avatar, jstr(u, "avatar"));   /* частичный объект без ключа — хэш не трогаем */
}
static void user_put_obj(const cJSON *u) {
    u64 id = jid(u, "id");
    if (!id) return;
    user_put(id, display_name(u));
    user_avatar_set(id, u);
}
u32 user_color(u64 gid, u64 id) {
    User *u = user_find(id);
    return u && u->color_gid == gid ? u->color : 0;
}
const char *user_avatar(u64 id) {
    User *u = user_find(id);
    return u && u->avatar[0] ? u->avatar : NULL;
}
const char *user_name(u64 id) {
    if (id == S.me_id && S.me_name) return S.me_name;
    User *u = user_find(id);
    return u ? u->name : "?";
}

Guild *guild_find(u64 id) {
    for (int i = 0; i < S.nguilds; i++) if (S.guilds[i].id == id) return &S.guilds[i];
    return NULL;
}
Channel *chan_find(u64 id) {
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].id == id) return &S.chans[i];
    return NULL;
}

/* ---------- права ---------- */
static u64 perm_str(const cJSON *o, const char *key) {
    const cJSON *v = jget(o, key);
    if (cJSON_IsString(v)) return strtoull(v->valuestring, NULL, 10);
    if (cJSON_IsNumber(v)) return (u64)v->valuedouble;
    return 0;
}
static void compute_perms(Channel *c) {
    if (!c->guild_id) { c->visible = c->can_send = c->can_connect = c->can_speak = c->can_history = true; return; }
    Guild *g = guild_find(c->guild_id);
    if (!g) { c->visible = false; return; }
    Channel *th = NULL;                          /* №90: ветка — права родительского канала */
    if (chan_is_thread(c)) {
        Channel *par = chan_find(c->parent_id);
        if (!par) { c->visible = c->can_send = false; return; }
        th = c;
        c = par;
    }
    u64 p = 0;
    for (int i = 0; i < g->nroles; i++) if (g->roles[i].id == g->id) p |= g->roles[i].perms;
    for (int k = 0; k < g->nmy; k++)
        for (int i = 0; i < g->nroles; i++) if (g->roles[i].id == g->my_roles[k]) p |= g->roles[i].perms;
    if (g->owner_id == S.me_id || (p & P_ADMIN)) { p = ~0ULL; goto done; }
    for (int i = 0; i < c->now_; i++) if (c->ow[i].id == g->id) p = (p & ~c->ow[i].deny) | c->ow[i].allow;
    u64 allow = 0, deny = 0;
    for (int i = 0; i < c->now_; i++) {
        if (c->ow[i].type != 0 || c->ow[i].id == g->id) continue;
        for (int k = 0; k < g->nmy; k++) if (c->ow[i].id == g->my_roles[k]) { allow |= c->ow[i].allow; deny |= c->ow[i].deny; }
    }
    p = (p & ~deny) | allow;
    for (int i = 0; i < c->now_; i++) if (c->ow[i].type == 1 && c->ow[i].id == S.me_id) p = (p & ~c->ow[i].deny) | c->ow[i].allow;
done:
    if (th) {                                   /* №90: ветка — по правам родителя */
        bool mt = (p & (1ULL << 34)) != 0;      /* MANAGE_THREADS */
        th->visible = (p & P_VIEW) && (th->type != CH_PRIV_THREAD || (th->tflags & TF_JOINED) || mt);
        th->can_send = (p & (1ULL << 38)) && (!(th->tflags & TF_LOCKED) || mt);   /* SEND_MESSAGES_IN_THREADS */
        th->can_history = (p & P_HISTORY) != 0;
        th->can_connect = th->can_speak = false;
        th->slow_exempt = (p & ((1ULL << 13) | P_MANAGE_CHANNELS | (1ULL << 34))) != 0;
        return;
    }
    c->visible = (p & P_VIEW) != 0;
    c->can_send = (p & P_SEND) != 0;
    c->can_connect = (p & P_CONNECT) != 0;
    c->can_speak = (p & P_SPEAK) != 0;
    c->can_history = (p & P_HISTORY) != 0;
    c->slow_exempt = (p & ((1ULL << 13) | P_MANAGE_CHANNELS)) != 0;   /* MANAGE_MESSAGES / MANAGE_CHANNELS */
}
static void recompute_guild(u64 gid) {
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].guild_id == gid) compute_perms(&S.chans[i]);
}

/* ---------- каналы ---------- */
static bool chan_type_kept(int t) {
    return t == CH_TEXT || t == CH_DM || t == CH_VOICE || t == CH_GROUP || t == CH_CATEGORY || t == CH_NEWS || t == CH_STAGE ||
           t == CH_NEWS_THREAD || t == CH_THREAD || t == CH_PRIV_THREAD || t == CH_FORUM || t == CH_MEDIA;
}
static Channel *chan_alloc(void) {
    if (S.nchans == S.capchans) {
        S.capchans = S.capchans ? S.capchans * 2 : 128;
        S.chans = xrealloc(S.chans, (size_t)S.capchans * sizeof(Channel));
    }
    Channel *c = &S.chans[S.nchans++];
    memset(c, 0, sizeof *c);
    c->notify = NOTIFY_DEFAULT;
    return c;
}
/* Тема хранится только у открытого канала (topic_ch): при открытии — GET /channels/{id}, у прежнего
 * освобождается. Из READY темы не берутся (сотни КБ на больших аккаунтах). */
static u64 topic_ch;
static u64 unread_hold;                         /* канал, отмеченный непрочитанным вручную: авто-отметки не снимают */
/* Тема: одна строка (san без переводов), не длиннее TOPIC_MAX байт по границе символа; пустая — NULL. */
static char *topic_copy(const char *t) {
    if (!t || !*t) return NULL;
    char *s = san(t, false);
    size_t n = strlen(s);
    if (n > TOPIC_MAX) {
        n = TOPIC_MAX - 3;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        memcpy(s + n, "...", 4);
        char *q = xrealloc(s, n + 4);
        s = q;
    }
    if (!*s) { free(s); return NULL; }
    return s;
}
static void chan_fill(Channel *c, const cJSON *o, u64 gid) {
    free(c->name); free(c->topic); free(c->recipients); free(c->ow);
    c->name = NULL; c->topic = NULL; c->recipients = NULL; c->ow = NULL; c->nrecip = c->now_ = 0;
    c->id = jid(o, "id");
    c->guild_id = gid ? gid : jid(o, "guild_id");
    c->parent_id = jid(o, "parent_id");
    c->owner_id = jid(o, "owner_id");          /* №111: владелец группы ЛС (и ветки) */
    c->type = (int)jint(o, "type", 0);
    c->position = (int)jint(o, "position", 0);
    {
        i64 sm = jint(o, "rate_limit_per_user", 0);
        c->slowmode = (u16)(sm < 0 ? 0 : sm > 21600 ? 21600 : sm);
    }
    u64 lm = jid(o, "last_message_id");
    if (lm > c->last_msg) c->last_msg = lm;
    const char *n = jstr(o, "name");
    c->name = n ? san(n, false) : NULL;
    c->topic = c->id == topic_ch ? topic_copy(jstr(o, "topic")) : NULL;   /* тема — только у открытого канала */
    const cJSON *ow = jget(o, "permission_overwrites");
    int nw = cJSON_GetArraySize(ow);
    if (nw > 0) {
        c->ow = xcalloc((size_t)nw, sizeof(Overwrite));
        const cJSON *e;
        cJSON_ArrayForEach(e, ow) {
            Overwrite *w = &c->ow[c->now_++];
            w->id = jid(e, "id");
            w->type = (u8)jint(e, "type", 0);
            w->allow = perm_str(e, "allow");
            w->deny = perm_str(e, "deny");
        }
    }
    /* ЛС: получатели — либо id (дедупликация в READY), либо объекты */
    const cJSON *ids = jget(o, "recipient_ids"), *rec = jget(o, "recipients");
    int nr = cJSON_GetArraySize(ids ? ids : rec);
    if (nr > 0) {
        c->recipients = xcalloc((size_t)nr, sizeof(u64));
        const cJSON *e;
        if (ids) cJSON_ArrayForEach(e, ids) c->recipients[c->nrecip++] = id_of(e);
        else cJSON_ArrayForEach(e, rec) {
            u64 uid = jid(e, "id");
            user_put_obj(e);
            c->recipients[c->nrecip++] = uid;
        }
    }
}
static void chan_upsert(const cJSON *o, u64 gid) {
    int t = (int)jint(o, "type", -1);
    if (!chan_type_kept(t)) return;
    u64 id = jid(o, "id");
    Channel *c = chan_find(id);
    if (!c) c = chan_alloc();
    chan_fill(c, o, gid);
    dm_flags(c, o);                             /* №109 */
    if ((t == CH_VOICE || t == CH_STAGE) && jget(o, "status")) vcs_put(id, jstr(o, "status"));   /* №151 */
    compute_perms(c);
}
static void chan_remove(u64 id) {
    for (int i = 0; i < S.nchans; i++) if (S.chans[i].id == id) {
        chan_free(&S.chans[i]);
        S.chans[i] = S.chans[--S.nchans];
        return;
    }
}
void chan_remove_test(u64 id);
void chan_remove_test(u64 id) { chan_remove(id); }   /* selftest: убрать канал/ветку */

void chan_label(const Channel *c, char *out, size_t n) {
    if (c->name && *c->name) { snprintf(out, n, "%s", c->name); return; }
    out[0] = 0;
    size_t len = 0;
    for (int i = 0; i < c->nrecip && len + 4 < n; i++) {
        int w = snprintf(out + len, n - len, "%s%s", i ? ", " : "", user_name(c->recipients[i]));
        if (w < 0) break;
        len += (size_t)w;
    }
    if (!out[0]) snprintf(out, n, ui_tr("(пусто)"));
}
/* ревью 11 С1: в счёт непрочитанного идут только ветки, где я участник; форумы — никогда */
static bool chan_counts(const Channel *c) {
    if (c->type == CH_FORUM || c->type == CH_MEDIA) return false;
    if (chan_is_thread(c) && !(c->tflags & TF_JOINED)) return false;
    return true;
}
bool chan_unread(const Channel *c) {
    if (c->muted || !c->visible || !chan_counts(c)) return false;
    if (c->type == CH_VOICE || c->type == CH_CATEGORY || c->type == CH_STAGE) return false;
    return c->last_msg > c->last_read;
}
bool guild_unread(u64 gid, int *mentions) {
    bool any = false;
    int m = 0;
    Guild *g = guild_find(gid);
    for (int i = 0; i < S.nchans; i++) {
        Channel *c = &S.chans[i];
        if (c->guild_id != gid || !chan_counts(c)) continue;
        m += c->mentions;
        if (!(g && g->muted) && chan_unread(c)) any = true;
    }
    if (mentions) *mentions = m;
    return any || m > 0;
}

static int dm_cmp(const void *a, const void *b) {
    const Channel *x = a, *y = b;
    bool dx = !x->guild_id, dy = !y->guild_id;
    if (dx != dy) return dx ? -1 : 1;
    if (dx) return x->last_msg > y->last_msg ? -1 : x->last_msg < y->last_msg;
    return 0;
}
void store_sort_dms(void) {
    /* ЛС в начале массива по свежести; остальное после (порядок серверов строит интерфейс). */
    qsort(S.chans, (size_t)S.nchans, sizeof(Channel), dm_cmp);
}

/* ---------- голосовые состояния ---------- */
static void vs_set(u64 gid, u64 cid, u64 uid, const char *name, bool sm, bool sd, bool m, bool d) {
    for (int i = 0; i < S.nvs; i++) if (S.vs[i].user_id == uid && S.vs[i].guild_id == gid) {
        if (!cid) { free(S.vs[i].name); S.vs[i] = S.vs[--S.nvs]; return; }
        S.vs[i].channel_id = cid;
        S.vs[i].self_mute = sm; S.vs[i].self_deaf = sd; S.vs[i].mute = m; S.vs[i].deaf = d;
        if (name) { free(S.vs[i].name); S.vs[i].name = san(name, false); }
        return;
    }
    if (!cid) return;
    if (S.nvs == S.capvs) {
        S.capvs = S.capvs ? S.capvs * 2 : 32;
        S.vs = xrealloc(S.vs, (size_t)S.capvs * sizeof(VoiceState));
    }
    VoiceState *v = &S.vs[S.nvs++];
    *v = (VoiceState){ gid, cid, uid, san(name ? name : user_name(uid), false), sm, sd, m, d };
}
/* Демо: своё голосовое состояние (в сети его присылает Discord событием VOICE_STATE_UPDATE). */
void store_vs_self(u64 gid, u64 cid, bool mute, bool deaf) {
    vs_set(gid, cid, S.me_id, S.me_name ? S.me_name : "?", mute, deaf, false, false);
    ui_dirty();
}
static void vs_from_json(const cJSON *o, u64 gid) {
    u64 uid = jid(o, "user_id");
    const cJSON *member = jget(o, "member");
    const cJSON *user = jget(member, "user");
    const char *name = jstr(member, "nick");
    if (!name && user) name = display_name(user);
    if (user && jid(user, "id") == uid) user_put_obj(user);
    vs_set(gid ? gid : jid(o, "guild_id"), jid(o, "channel_id"), uid, name,
           jbool(o, "self_mute"), jbool(o, "self_deaf"), jbool(o, "mute"), jbool(o, "deaf"));
    for (int i = 0; i < S.nvs; i++) if (S.vs[i].user_id == uid) { S.vs[i].stream = jbool(o, "self_stream"); S.vs[i].video = jbool(o, "self_video"); }
}
int voice_members(u64 channel_id, const VoiceState **out, int max) {
    int n = 0;
    for (int i = 0; i < S.nvs && n < max; i++) if (S.vs[i].channel_id == channel_id) out[n++] = &S.vs[i];
    return n;
}

/* ---------- серверы ---------- */
static const cJSON *gprop(const cJSON *g, const char *key) {
    const cJSON *p = jget(jget(g, "properties"), key);
    return p ? p : jget(g, key);
}
static void guild_load(const cJSON *go, const cJSON *my_member) {
    if (jbool(go, "unavailable")) return;
    u64 gid = jid(go, "id");
    Guild *g = guild_find(gid);
    if (!g) {
        S.guilds = xrealloc(S.guilds, (size_t)(S.nguilds + 1) * sizeof(Guild));
        g = &S.guilds[S.nguilds++];
        memset(g, 0, sizeof *g);
        g->notify = NOTIFY_DEFAULT;
    } else {
        guild_free(g);
        bool m = g->muted;
        u8 nt = g->notify;
        memset(g, 0, sizeof *g);
        g->muted = m;
        g->notify = nt;
    }
    g->id = gid;
    const cJSON *nm = gprop(go, "name");
    g->name = san(cJSON_IsString(nm) ? nm->valuestring : "?", false);
    g->owner_id = id_of(gprop(go, "owner_id"));
    const cJSON *ic = gprop(go, "icon");
    hash_copy(g->icon, cJSON_IsString(ic) ? ic->valuestring : NULL);
    const cJSON *bn = gprop(go, "banner");
    g->banner_color = banner_color(cJSON_IsString(bn) ? bn->valuestring : NULL);   /* №4 */
    const cJSON *roles = jget(go, "roles");
    int nr = cJSON_GetArraySize(roles);
    if (nr > 0) {
        g->roles = xcalloc((size_t)nr, sizeof(Role));
        const cJSON *r;
        cJSON_ArrayForEach(r, roles) {
            g->roles[g->nroles].id = jid(r, "id");
            g->roles[g->nroles].perms = perm_str(r, "permissions");
            g->roles[g->nroles].color = (u32)jint(r, "color", 0) & 0xFFFFFFu;
            g->roles[g->nroles].pos = (int)jint(r, "position", 0);
            g->nroles++;
        }
    }
    g->timeout_until = iso_to_unix(jstr(my_member, "communication_disabled_until"));
    g->pending = jbool(my_member, "pending");   /* №128 */
    const cJSON *mr = jget(my_member, "roles");
    int nm2 = cJSON_GetArraySize(mr);
    if (nm2 > 0) {
        g->my_roles = xcalloc((size_t)nm2, sizeof(u64));
        const cJSON *r;
        cJSON_ArrayForEach(r, mr) g->my_roles[g->nmy++] = id_of(r);
    }
    const cJSON *c;
    cJSON_ArrayForEach(c, jget(go, "channels")) chan_upsert(c, gid);
    cJSON_ArrayForEach(c, jget(go, "threads")) if (jget(c, "member")) thread_upsert(c, gid);   /* №90: только мои; прочие — threads_open */
    const cJSON *v;
    cJSON_ArrayForEach(v, jget(go, "voice_states")) vs_from_json(v, gid);
    recompute_guild(gid);
}

static const cJSON *entries(const cJSON *v) {
    return cJSON_IsArray(v) ? v : jget(v, "entries");
}

static void apply_guild_settings(const cJSON *e) {
    u64 gid = jid(e, "guild_id");
    Guild *g = gid ? guild_find(gid) : NULL;
    if (g) { g->muted = jbool(e, "muted"); g->notify = (u8)jint(e, "message_notifications", NOTIFY_DEFAULT); }
    const cJSON *o;
    cJSON_ArrayForEach(o, jget(e, "channel_overrides")) {
        Channel *c = chan_find(jid(o, "channel_id"));
        if (c) { c->muted = jbool(o, "muted"); c->notify = (u8)jint(o, "message_notifications", NOTIFY_DEFAULT); }
    }
}

/* ---------- друзья и заявки ----------
 * Хранится только id + тип + байт статуса (16 байт на запись), массив по id — поиск двоичный.
 * Статус ведётся только для друзей (REL_FRIEND): чужие присутствия отсеиваются в event_wanted. */
static int rel_idx(u64 id, bool *found) {
    int lo = 0, hi = S.nrels;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (S.rels[mid].id < id) lo = mid + 1; else hi = mid;
    }
    *found = lo < S.nrels && S.rels[lo].id == id;
    return lo;
}
const Relation *rel_find(u64 uid) {
    bool f;
    int i = rel_idx(uid, &f);
    return f ? &S.rels[i] : NULL;
}
int rel_list(const Relation **out) { if (out) *out = S.rels; return S.nrels; }
static void rel_set(u64 id, int type) {
    if (!id || type < REL_FRIEND || type > REL_OUT) return;      /* 5/6 (неявные, «подписка») не храним */
    bool f;
    int i = rel_idx(id, &f);
    if (!f) {
        if (S.nrels == S.caprels) {
            S.caprels = S.caprels ? S.caprels * 2 : 32;
            S.rels = xrealloc(S.rels, (size_t)S.caprels * sizeof(Relation));
        }
        memmove(&S.rels[i + 1], &S.rels[i], (size_t)(S.nrels - i) * sizeof(Relation));
        S.nrels++;
        S.rels[i] = (Relation){ .id = id };
    }
    if (S.rels[i].type != type) S.rels[i].status = PS_OFFLINE;   /* статус есть только у друга */
    S.rels[i].type = (u8)type;
}
static void rel_del(u64 id) {
    bool f;
    int i = rel_idx(id, &f);
    if (!f) return;
    memmove(&S.rels[i], &S.rels[i + 1], (size_t)(S.nrels - i - 1) * sizeof(Relation));
    S.nrels--;
}
static void rel_from_json(const cJSON *e) {
    const cJSON *u = jget(e, "user");
    u64 id = jid(e, "id");
    if (!id) id = jid(e, "user_id");
    if (!id && u) id = jid(u, "id");
    if (u && jid(u, "id") == id) user_put_obj(u);
    rel_set(id, (int)jint(e, "type", 0));
}
static u8 presence_code(const char *s) {
    if (!s) return PS_OFFLINE;
    if (!strcmp(s, "online")) return PS_ONLINE;
    if (!strcmp(s, "idle")) return PS_IDLE;
    if (!strcmp(s, "dnd")) return PS_DND;
    return PS_OFFLINE;                                           /* offline, invisible, неизвестное */
}
const char *presence_name(u8 st) {
    switch (st) {
    case PS_ONLINE: return ui_tr("в сети");
    case PS_IDLE: return ui_tr("не активен");
    case PS_DND: return ui_tr("не беспокоить");
    default: return ui_tr("не в сети");
    }
}
/* {user_id | user:{id}, status} — только для друзей; возврат: изменилось ли */
static bool presence_apply(const cJSON *p) {
    u64 id = jid(p, "user_id");
    if (!id) id = jid(jget(p, "user"), "id");
    bool f;
    int i = rel_idx(id, &f);
    if (!f || S.rels[i].type != REL_FRIEND) return false;
    u8 st = presence_code(jstr(p, "status"));
    if (S.rels[i].status == st) return false;
    S.rels[i].status = st;
    return true;
}

/* Действия (записи): в демо — только локально, без сети. */
static void on_rel_done(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status < 200 || r->status >= 300) ui_notice(ui_tr("друзья: Discord ответил %ld"), r->status);
}
void rel_accept(u64 uid) {
    const Relation *r = rel_find(uid);
    if (!r || r->type != REL_IN) { ui_notice(ui_tr("нет входящей заявки")); return; }
    if (O.demo) { rel_set(uid, REL_FRIEND); ui_dirty(); return; }      /* по сети придёт RELATIONSHIP_ADD */
    char path[64];
    snprintf(path, sizeof path, "/users/@me/relationships/%llu", (unsigned long long)uid);
    http_async("PUT", path, "{}", on_rel_done, NULL);
}
void rel_remove(u64 uid) {
    if (!rel_find(uid)) return;
    if (O.demo) { rel_del(uid); ui_dirty(); return; }                  /* по сети придёт RELATIONSHIP_REMOVE */
    char path[64];
    snprintf(path, sizeof path, "/users/@me/relationships/%llu", (unsigned long long)uid);
    http_async("DELETE", path, NULL, on_rel_done, NULL);
}

/* ---------- новая ЛС ---------- */
/* ui.c определяет ui_open_dm; слабая заглушка — пока его нет (и для сборок без интерфейса). */
__attribute__((weak)) void ui_open_dm(u64 channel_id) {
    if (O.cli) { cli_open_dm(channel_id); return; }
    S.open_ch = channel_id;
    ui_dirty();
}
static Channel *dm_with(u64 uid) {
    for (int i = 0; i < S.nchans; i++) {
        Channel *c = &S.chans[i];
        if (c->type == CH_DM && !c->guild_id && c->nrecip == 1 && c->recipients[0] == uid) return c;
    }
    return NULL;
}
static void on_dm_created(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status != 200 && r->status != 201) { ui_notice(ui_tr("беседа не создана: Discord ответил %ld"), r->status); return; }
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    u64 id = jid(j, "id");
    int t = (int)jint(j, "type", -1);
    if (id && (t == CH_DM || t == CH_GROUP)) { chan_upsert(j, 0); store_sort_dms(); }
    json_done();
    if (!id || !chan_find(id)) { ui_notice(ui_tr("беседа: неожиданный ответ Discord")); return; }
    ui_open_dm(id);
}
void dm_open(u64 uid) {
    if (!uid || uid == S.me_id) return;
    Channel *c = dm_with(uid);
    if (c) { ui_open_dm(c->id); return; }
    if (O.demo) {
        char b[160];
        static u64 demo_dm_id = 4100;
        u64 id = demo_dm_id++;
        snprintf(b, sizeof b, "{\"id\":\"%llu\",\"type\":1,\"recipient_ids\":[\"%llu\"],\"last_message_id\":\"0\"}",
                 (unsigned long long)id, (unsigned long long)uid);
        cJSON *j = json_parse(b, strlen(b));
        if (j) { chan_upsert(j, 0); store_sort_dms(); }
        json_done();
        ui_open_dm(id);
        return;
    }
    char body[64];
    snprintf(body, sizeof body, "{\"recipient_id\":\"%llu\"}", (unsigned long long)uid);
    http_async("POST", "/users/@me/channels", body, on_dm_created, NULL);
}

/* READY по частям: голова (я, пользователи, друзья), серверы по одному, хвост (ЛС, прочитанное, настройки).
 * Так же устроен и потоковый разбор store_ready_text: в памяти дерево cJSON только одной части. */
static void ready_head(const cJSON *d) {
    u64 open = S.open_ch;
    store_reset();
    topic_ch = 0;                               /* каналы пересозданы: тему открытого запросить заново */
    S.open_ch = open;
    const cJSON *me = jget(d, "user");
    S.me_id = jid(me, "id");
    S.me_premium = (u8)jint(me, "premium_type", 0);
    S.me_name = san(display_name(me), false);
    user_put_obj(me);
    const char *sid = jstr(d, "session_id");
    S.session_id = xstrdup(sid ? sid : "");
    const cJSON *u, *e;
    cJSON_ArrayForEach(u, jget(d, "users")) user_put_obj(u);
    /* Друзья и заявки: id + тип; имя — из READY users (или из объекта user в записи). */
    cJSON_ArrayForEach(e, jget(d, "relationships")) rel_from_json(e);
    /* Статусы — только друзей: merged_presences.friends (новый READY) или presences (старый). */
    cJSON_ArrayForEach(e, jget(jget(d, "merged_presences"), "friends")) presence_apply(e);
    cJSON_ArrayForEach(e, jget(d, "presences")) presence_apply(e);
    folders_ready(d);                           /* №2 */
}
/* members — merged_members[i] (участники сервера guilds[i]); ищем себя */
static void ready_guild(const cJSON *go, const cJSON *members) {
    const cJSON *mine = NULL, *mm;
    cJSON_ArrayForEach(mm, members) if (jid(mm, "user_id") == S.me_id) { mine = mm; break; }
    guild_load(go, mine);
}
static void ready_tail(const cJSON *d) {
    const cJSON *pc, *e;
    cJSON_ArrayForEach(pc, jget(d, "private_channels")) chan_upsert(pc, 0);
    cJSON_ArrayForEach(e, entries(jget(d, "read_state"))) {
        if (jint(e, "read_state_type", 0) != 0) continue;
        Channel *c = chan_find(jid(e, "id"));
        if (!c) continue;
        c->last_read = jid(e, "last_message_id");
        c->mentions = (int)jint(e, "mention_count", 0);
    }
    cJSON_ArrayForEach(e, entries(jget(d, "user_guild_settings"))) apply_guild_settings(e);
    store_sort_dms();
    S.ready = true;
}
void store_ready(const cJSON *d) {
    ready_head(d);
    const cJSON *go, *merged = jget(d, "merged_members");
    int gi = 0;
    cJSON_ArrayForEach(go, jget(d, "guilds")) ready_guild(go, cJSON_GetArrayItem(merged, gi++));
    ready_tail(d);
}

/* ---------- сообщения ---------- */
static void append_mention(Buf *b, const char *p, size_t n, const cJSON *mentions) {
    /* p указывает на внутренность <...> длиной n */
    char inner[96];
    if (n >= sizeof inner) { buf_put(b, "<", 1); buf_put(b, p, n); buf_put(b, ">", 1); return; }
    memcpy(inner, p, n);
    inner[n] = 0;
    if (inner[0] == '@' && inner[1] != '&') {
        u64 id = strtoull(inner + (inner[1] == '!' ? 2 : 1), NULL, 10);
        const char *name = NULL;
        const cJSON *m;
        cJSON_ArrayForEach(m, mentions) if (jid(m, "id") == id) { name = display_name(m); break; }
        if (!name) name = user_name(id);
        char *s = san(name, false);
        buf_printf(b, "@%s", s);
        free(s);
    } else if (inner[0] == '@' && inner[1] == '&') {
        buf_puts(b, ui_tr("@роль"));
    } else if (inner[0] == '#') {
        Channel *c = chan_find(strtoull(inner + 1, NULL, 10));
        buf_printf(b, "#%s", c && c->name ? c->name : ui_tr("канал"));
    } else if (inner[0] == ':' || (inner[0] == 'a' && inner[1] == ':')) {
        const char *s = inner[0] == 'a' ? inner + 1 : inner;
        const char *e = strchr(s + 1, ':');
        if (e) { buf_put(b, s, (size_t)(e - s + 1)); } else { buf_put(b, "<", 1); buf_puts(b, inner); buf_put(b, ">", 1); }
    } else if (inner[0] == 't' && inner[1] == ':') {
        char tb[32];
        fmt_time(strtoll(inner + 2, NULL, 10), tb, sizeof tb);
        buf_puts(b, tb);
    } else {
        buf_put(b, "<", 1); buf_puts(b, inner); buf_put(b, ">", 1);
    }
}
static char *render_content(const char *raw, const cJSON *mentions) {
    Buf b = {0};
    for (const char *p = raw; *p; ) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (e && e - p < 90 && (p[1] == '@' || p[1] == '#' || p[1] == ':' || (p[1] == 'a' && p[2] == ':') || (p[1] == 't' && p[2] == ':'))) {
                append_mention(&b, p + 1, (size_t)(e - p - 1), mentions);
                p = e + 1;
                continue;
            }
        }
        buf_put(&b, p, 1);
        p++;
    }
    return b.p ? b.p : xstrdup("");
}
static const char *system_text(int type) {
    switch (type) {
    case 1: return ui_tr("[добавил участника]");
    case 2: return ui_tr("[убрал участника]");
    case 3: return ui_tr("[звонок]");
    case 4: return ui_tr("[изменил название]");
    case 6: return ui_tr("[закрепил сообщение]");
    case 7: return ui_tr("[присоединился к серверу]");
    case 8: return ui_tr("[буст сервера]");
    default: return NULL;
    }
}
static void reactions_from(Message *m, const cJSON *arr) {
    for (int i = 0; i < m->nreacts; i++) free(m->reacts[i].emoji);
    free(m->reacts);
    m->reacts = NULL;
    m->nreacts = 0;
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return;
    m->reacts = xcalloc((size_t)n, sizeof(Reaction));
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) {
        const cJSON *em = jget(r, "emoji");
        const char *name = jstr(em, "name");
        u64 eid = jid(em, "id");
        char buf[160];
        if (eid) snprintf(buf, sizeof buf, ":%s:%llu", name ? name : "?", (unsigned long long)eid);
        else snprintf(buf, sizeof buf, "%s", name ? name : "?");
        Reaction *x = &m->reacts[m->nreacts++];
        x->emoji = san(buf, false);
        x->count = (int)jint(r, "count", 1);
        x->me = jbool(r, "me");
    }
}
/* Цвет ника: цвет старшей (по position) роли участника, у которой цвет задан. */
static u32 member_color(const Guild *g, const cJSON *member) {
    u32 c = 0; int best = -1;
    const cJSON *r;
    cJSON_ArrayForEach(r, jget(member, "roles")) {
        u64 rid = id_of(r);
        for (int i = 0; i < g->nroles; i++)
            if (g->roles[i].id == rid && g->roles[i].color && g->roles[i].pos > best) { best = g->roles[i].pos; c = g->roles[i].color; }
    }
    return c;
}
/* Подпись ответа на сообщение без текста */
static const char *ref_placeholder(const cJSON *ref) {
    if (cJSON_GetArraySize(jget(ref, "attachments")) > 0) return jint(ref, "flags", 0) & 8192 ? ui_tr("[голосовое]") : ui_tr("[файл]");
    if (cJSON_GetArraySize(jget(ref, "sticker_items")) > 0) return ui_tr("[стикер]");
    if (jget(ref, "poll")) return ui_tr("[опрос]");
    if (cJSON_GetArraySize(jget(ref, "embeds")) > 0) return ui_tr("[встройка]");
    if (jget(ref, "message_snapshots")) return ui_tr("[пересланное]");
    return ui_tr("(без текста)");
}
static bool g_no_user_put;                       /* поиск: авторов в таблицу пользователей не добавлять */
static void message_fill(Message *m, const cJSON *o) {
    msg_free(m);
    m->id = jid(o, "id");
    const cJSON *a = jget(o, "author");
    m->author_id = jid(a, "id");
    if (m->author_id && !g_no_user_put) user_put_obj(a);
    {
        u64 gid = jid(o, "guild_id");
        if (!gid) { Channel *ch = chan_find(jid(o, "channel_id")); gid = ch ? ch->guild_id : 0; }
        Guild *g = gid ? guild_find(gid) : NULL;
        const cJSON *mem = jget(o, "member");
        User *u = m->author_id ? user_find(m->author_id) : NULL;
        if (g && mem && u) { u->color = member_color(g, mem); u->color_gid = gid; }
        m->color = g ? user_color(gid, m->author_id) : 0;
    }
    const char *nick = jstr(jget(o, "member"), "nick");
    m->author = san(nick ? nick : display_name(a), false);
    m->ts = iso_to_unix(jstr(o, "timestamp"));
    m->edited = jstr(o, "edited_timestamp") != NULL;
    m->bot = jbool(a, "bot");
    m->webhook = jid(o, "webhook_id") != 0;
    m->flags = (u32)jint(o, "flags", 0);
    if (m->flags & 8192) {                      /* №69: голосовое — длина из вложения */
        double ds = 0;
        const cJSON *va = cJSON_GetArrayItem(jget(o, "attachments"), 0), *dv = jget(va, "duration_secs");
        if (cJSON_IsNumber(dv)) ds = dv->valuedouble;
        m->voice_secs = (u16)(ds < 1 ? 1 : ds > 3600 ? 3600 : ds + 0.5);
    }
    m->pinned = jbool(o, "pinned");
    /* Пересланное (message_reference.type == 1): текст, вложения и встройки — из message_snapshots[0].message */
    const cJSON *mref = jget(o, "message_reference"), *src = o;
    if (mref && jint(mref, "type", 0) == 1) {
        const cJSON *snap = jget(cJSON_GetArrayItem(jget(o, "message_snapshots"), 0), "message");
        m->forwarded = true;
        if (cJSON_IsObject(snap)) src = snap;
    }
    Buf b = {0};
    const char *raw = jstr(src, "content");
    char *clean = san(raw ? raw : "", true);
    if (m->author_id && m->author_id == S.me_id && !m->forwarded) m->raw = xstrdup(clean);   /* для правки: как набрано */
    char *rendered = render_content(clean, jget(src, "mentions"));
    free(clean);
    buf_puts(&b, rendered);
    free(rendered);
    const char *sys = system_text((int)jint(o, "type", 0));
    if (sys && b.len == 0) buf_puts(&b, sys);
    const cJSON *x;
    cJSON_ArrayForEach(x, jget(src, "attachments")) {
        char *fn = san(jstr(x, "filename"), false), *url = san(jstr(x, "url"), false);
        for (char *p = fn; *p; p++) if (*p == ']') *p = ')';   /* В1: «]» в имени не подменяет url вложения */
        /* А1: маркер ATT_MARK (\x01) — только у настоящих вложений: san вырезает управляющие байты из
         * любого пользовательского текста, так что «[файл: x] url», набранное собеседником, вложением не станет */
        buf_printf(&b, "%s" ATT_MARK_S, b.len ? "\n" : "");
        buf_printf(&b, ui_tr("[файл: %s] %s"), fn, url);
        free(fn); free(url);
    }
    /* эмбеды — карточками в Message.emb (ревью 11 М2: без дубля «[встройка]» в content) */
    cJSON_ArrayForEach(x, jget(src, "sticker_items")) {
        char *n = san(jstr(x, "name"), false);
        buf_printf(&b, ui_tr("%s[стикер: %s]"), b.len ? "\n" : "", n);
        free(n);
    }
    m->content = b.p ? b.p : xstrdup("");
    embeds_parse(m, jget(src, "embeds"));      /* №53: карточки */
    m->poll = poll_parse(jget(src, "poll"));   /* №41 */
    {                                           /* №55: кнопки ботов (только у сообщений приложений) */
        u64 app = jid(o, "application_id");
        if (!app && m->bot) app = m->author_id;
        comps_parse(m, jget(o, "components"), app);
    }
    const cJSON *ref = jget(o, "referenced_message");
    if (cJSON_IsObject(ref)) {
        m->ref_id = jid(ref, "id");
        m->ref_author = san(display_name(jget(ref, "author")), false);
        const char *rc = jstr(ref, "content");
        char *t = san(rc ? rc : "", false);
        if (!*t) { free(t); t = xstrdup(ref_placeholder(ref)); }
        if (strlen(t) > 120) { int k = 117; while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--; memcpy(t + k, "...", 4); }
        m->ref_text = t;
    } else if (mref && !m->forwarded) {
        m->ref_id = jid(jget(o, "message_reference"), "message_id");
        m->ref_author = xstrdup("?");
        m->ref_text = xstrdup(ui_tr("(сообщение недоступно)"));
    }
    reactions_from(m, jget(o, "reactions"));
}

Message *msg_find(u64 id, int *idx) {
    for (int i = S.nmsgs - 1; i >= 0; i--) if (S.msgs[i].id == id) { if (idx) *idx = i; return &S.msgs[i]; }
    return NULL;
}
static Message *msg_insert(u64 id) {
    if (S.nmsgs == S.capmsgs) {
        S.capmsgs = S.capmsgs ? S.capmsgs * 2 : 64;
        S.msgs = xrealloc(S.msgs, (size_t)S.capmsgs * sizeof(Message));
    }
    int i = S.nmsgs++;
    while (i > 0 && S.msgs[i - 1].id > id) { S.msgs[i] = S.msgs[i - 1]; i--; }
    memset(&S.msgs[i], 0, sizeof(Message));
    return &S.msgs[i];
}
static void msgs_trim(bool keep_old) {
    /* Не больше MAX_MSGS: при новых сообщениях выкидываем старые, при подгрузке — новые */
    while (S.nmsgs > MAX_MSGS) {
        if (keep_old) { msg_free(&S.msgs[S.nmsgs - 1]); S.nmsgs--; }
        else { msg_free(&S.msgs[0]); memmove(S.msgs, S.msgs + 1, (size_t)(--S.nmsgs) * sizeof(Message)); S.history_end = false; }
    }
}
/* ---------- вытеснение пользователей ----------
 * Таблица S.users растёт с историей каналов (авторы сообщений). При смене канала, когда записей стало
 * вдвое больше, чем осталось после прошлой чистки (и не меньше USERS_GC_MIN), удаляются те, на кого никто
 * не ссылается: не я, не автор в открытой ленте, не участник голоса, не друг/заявка/блок, не собеседник ЛС,
 * не звонящий. Имена в результатах поиска, закреплённых, упоминаниях и профиле — свои копии, им таблица не нужна.
 * Указатели из user_name() годны только до следующего события store (ui.c их не хранит между кадрами). */
#define USERS_GC_MIN 256
static int users_gc_next = USERS_GC_MIN;
static void users_mark(u8 *keep, u64 id) {
    if (!id) return;
    User *u = user_find(id);
    if (u) keep[u - S.users] = 1;
}
int users_gc(bool force) {
    if (!force && (S.nusers < users_gc_next || S.nusers < USERS_GC_MIN)) return 0;
    if (!S.nusers) return 0;
    u8 *keep = xcalloc((size_t)S.nusers, 1);
    users_mark(keep, S.me_id);
    for (int i = 0; i < S.nmsgs; i++) { users_mark(keep, S.msgs[i].author_id); }
    for (int i = 0; i < S.nvs; i++) users_mark(keep, S.vs[i].user_id);
    for (int i = 0; i < S.nrels; i++) users_mark(keep, S.rels[i].id);
    for (int i = 0; i < S.nchans; i++)
        if (!S.chans[i].guild_id) for (int k = 0; k < S.chans[i].nrecip; k++) users_mark(keep, S.chans[i].recipients[k]);
    users_mark(keep, S.call.caller);
    int w = 0, dropped = 0;
    for (int i = 0; i < S.nusers; i++) {
        if (keep[i]) { S.users[w++] = S.users[i]; continue; }
        free(S.users[i].name);
        dropped++;
    }
    free(keep);
    S.nusers = w;
    users_gc_next = w * 2 > USERS_GC_MIN ? w * 2 : USERS_GC_MIN;
    if (S.capusers > 64 && S.capusers > S.nusers * 2) {    /* вернуть лишнее место массива */
        int cap = S.nusers < 32 ? 64 : S.nusers * 2;
        S.users = xrealloc(S.users, (size_t)cap * sizeof(User));
        S.capusers = cap;
    }
    return dropped;
}

static void topic_follow(void);
void store_set_messages(const cJSON *arr, bool older) {
    if (!older) { msgs_clear(); topic_follow(); }
    int n = cJSON_GetArraySize(arr);
    if (n < 50) S.history_end = true;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        u64 id = jid(o, "id");
        if (!id || msg_find(id, NULL)) continue;
        message_fill(msg_insert(id), o);
    }
    msgs_trim(older);
    if (!older) users_gc(false);                /* смена канала: авторы прежней ленты больше не нужны */
}

/* ---------- поиск сообщений ----------
 * Одна страница (до 25) во временном массиве; search_close() освобождает всё. Ответ, пришедший
 * после закрытия или нового запроса, отбрасывается по номеру поколения (ctx). */
static SearchState SR;
static unsigned search_gen;                     /* меняется при каждом start/close */
static int search_tries;                        /* повторы при 202 */
static char *search_path;                       /* путь запроса для повтора при 202 */
const SearchState *search_state(void) { return &SR; }
static void search_free_hits(void) {
    for (int i = 0; i < SR.nhits; i++) msg_free(&SR.hits[i].m);
    free(SR.hits);
    SR.hits = NULL;
    SR.nhits = SR.total = 0;
}
void search_close(void) {
    search_free_hits();
    free(search_path);
    search_path = NULL;
    memset(&SR, 0, sizeof SR);
    search_tries = 0;                           /* у нового поиска — свои повторы при 202 */
    search_gen++;
}
/* {"total_results":N,"messages":[[{..,"hit":true}],..]} -> SR.hits (не больше SEARCH_PAGE) */
void search_apply(const cJSON *j) {
    search_free_hits();
    SR.busy = SR.indexing = false;
    SR.total = (int)jint(j, "total_results", 0);
    if (SR.total < 0) SR.total = 0;
    const cJSON *msgs = jget(j, "messages");
    int n = cJSON_GetArraySize(msgs);
    if (n > SEARCH_PAGE) n = SEARCH_PAGE;
    if (n > 0) SR.hits = xcalloc((size_t)n, sizeof(SearchHit));
    const cJSON *grp;
    cJSON_ArrayForEach(grp, msgs) {
        if (SR.nhits >= n) break;
        const cJSON *m = cJSON_IsArray(grp) ? NULL : grp, *x;
        if (!m) {                               /* группа: найденное помечено hit, иначе первое */
            cJSON_ArrayForEach(x, grp) if (jbool(x, "hit")) { m = x; break; }
            if (!m) m = cJSON_GetArrayItem(grp, 0);
        }
        if (!cJSON_IsObject(m) || !jid(m, "id")) continue;
        SearchHit *h = &SR.hits[SR.nhits++];
        g_no_user_put = true;                   /* имя автора хранится в самом результате */
        message_fill(&h->m, m);
        g_no_user_put = false;
        h->channel_id = jid(m, "channel_id");
    }
    if (SR.total < SR.nhits) SR.total = SR.nhits;
    SR.page_count = (SR.total + SEARCH_PAGE - 1) / SEARCH_PAGE;
}
static void on_search(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != search_gen || !search_path) return;    /* поиск закрыт или заменён */
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    double ra = 0;
    const cJSON *rv = jget(j, "retry_after");
    if (cJSON_IsNumber(rv)) ra = rv->valuedouble;
    if (!(ra >= 0)) ra = 0;                     /* NaN, отрицательное */
    if (ra > 3600) ra = 3600;                   /* огромное значение: без переполнения при (int) */
    if (r->status == 200) {
        SR.busy = SR.indexing = false;
        search_apply(j);
    } else if (r->status == 202 && search_tries < 6) {
        /* индекс строится: повтор через retry_after (1..15 с), без опроса */
        int ms = ra > 0 ? (int)(ra * 1000) : 2000;
        if (ms < 1000) ms = 1000;
        if (ms > 15000) ms = 15000;
        search_tries++;
        SR.busy = SR.indexing = true;
        http_async_delayed("GET", search_path, NULL, on_search, ctx, ms);
    } else {
        SR.busy = SR.indexing = false;
        if (r->status == 202) snprintf(SR.error, sizeof SR.error, ui_tr("индекс поиска ещё строится, попробуйте позже"));
        else if (r->status == 429) snprintf(SR.error, sizeof SR.error, ui_tr("слишком часто, повтор через %d с"), (int)(ra + 0.999) > 0 ? (int)(ra + 0.999) : 1);
        else if (r->status == 403) snprintf(SR.error, sizeof SR.error, ui_tr("нет доступа к поиску здесь"));
        else if (r->status == 0) snprintf(SR.error, sizeof SR.error, ui_tr("нет связи с Discord"));
        else snprintf(SR.error, sizeof SR.error, ui_tr("поиск: Discord ответил %ld"), r->status);
    }
    json_done();
    ui_dirty();
}
/* только для --selftest: путь последнего запроса и подставной ответ Discord (без сети) */
const char *search_test_path(void) { return search_path ? search_path : ""; }
void search_test_resp(long status, const char *body) {
    Resp r = { .status = status };
    buf_puts(&r.body, body);
    on_search(&r, (void *)(uintptr_t)search_gen);
    buf_free(&r.body);
}
/* Дата ГГГГ-ММ-ДД (UTC, начало суток) -> snowflake; 0 — неверная дата. */
static u64 date_snowflake(const char *s) {
    int y, m, d;
    if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || y < 2015 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    y -= m <= 2;                                /* дни от 1970-01-01 (алгоритм Хиннанта) */
    i64 era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    i64 ms = days * 86400000LL - DISCORD_EPOCH_MS;
    return ms > 0 ? (u64)ms << 22 : 0;
}
static bool ascii_prefix_ci(const char *s, const char *p) {
    for (; *p; s++, p++) { char a = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s; if (a != *p) return false; }
    return true;
}
static u64 sr_gid, sr_cid;
static char sr_query[256];
static int sr_page;
/* Ревью 9 М4: обрезать строку до max байт по границе символа UTF-8 (не посреди буквы). */
static void utf8_trunc(char *s, size_t max) {
    size_t n = strlen(s);
    if (n <= max) return;
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    s[max] = 0;
}
/* Следующее слово запроса; значение фильтра в кавычках — с пробелами: from:"Иван Петров", in:"#мой канал".
 * Кавычки снимаются только у значения фильтра (слово с ':' перед '"'). */
static char *sr_tok(char **pp) {
    char *p = *pp;
    while (*p == ' ') p++;
    if (!*p) { *pp = p; return NULL; }
    char *t = p, *w = p;
    char *colon = NULL;
    while (*p && *p != ' ') {
        if (*p == ':' && !colon) colon = w + 1;
        if (*p == '"' && colon && w == colon) {   /* значение фильтра в кавычках */
            p++;
            while (*p && *p != '"') *w++ = *p++;
            if (*p == '"') p++;
            continue;
        }
        *w++ = *p++;
    }
    if (*p) p++;
    *w = 0;
    *pp = p;
    return t;
}
/* имя для сообщения об ошибке: до 40 байт по границе символа */
static const char *sr_short(const char *s, char *buf, size_t n) {
    snprintf(buf, n, "%s", s);
    utf8_trunc(buf, n - 1 < 40 ? n - 1 : 40);
    return buf;
}
/* Запрос с фильтрами (как в Discord): from:имя  in:#канал  has:link|file|image|video|embed|sound|sticker
 * before:ГГГГ-ММ-ДД  after:ГГГГ-ММ-ДД — остальное идёт в content. Ошибка разбора — SR.error. */
static void search_run(void) {
    char *q = san(sr_query, false);
    Buf content = {0}, extra = {0};
    char err[160] = "", sh[48];
    char *pp = q;
    for (char *tok = sr_tok(&pp); tok; tok = sr_tok(&pp)) {
        if (ascii_prefix_ci(tok, "from:")) {
            const char *nm = tok + 5;
            u64 uid = 0;
            for (int i = 0; i < S.nusers && !uid; i++) if (S.users[i].name && !strcmp(S.users[i].name, nm)) uid = S.users[i].id;
            if (S.me_name && !strcmp(nm, S.me_name)) uid = S.me_id;
            if (!uid) { snprintf(err, sizeof err, "from: пользователь \"%s\" не найден", sr_short(nm, sh, sizeof sh)); break; }
            buf_printf(&extra, "&author_id=%llu", (unsigned long long)uid);
        } else if (ascii_prefix_ci(tok, "in:")) {
            const char *nm = tok + 3 + (tok[3] == '#');
            u64 ch = 0;
            for (int i = 0; i < S.nchans && !ch; i++) if (S.chans[i].guild_id == sr_gid && S.chans[i].name && !strcmp(S.chans[i].name, nm)) ch = S.chans[i].id;
            if (!ch || !sr_gid) { snprintf(err, sizeof err, "in: канал \"%s\" не найден", sr_short(nm, sh, sizeof sh)); break; }
            buf_printf(&extra, "&channel_id=%llu", (unsigned long long)ch);
        } else if (ascii_prefix_ci(tok, "has:")) {
            static const char *const HAS[] = { "link", "file", "image", "video", "embed", "sound", "sticker", "poll" };
            bool ok = false;
            for (size_t i = 0; i < sizeof HAS / sizeof *HAS; i++) if (!strcmp(tok + 4, HAS[i])) { buf_printf(&extra, "&has=%s", HAS[i]); ok = true; }
            if (!ok) { snprintf(err, sizeof err, "has: link, file, image, video, embed, sound, sticker"); break; }
        } else if (ascii_prefix_ci(tok, "before:") || ascii_prefix_ci(tok, "after:")) {
            bool before = (tok[0] | 32) == 'b';
            u64 sf = date_snowflake(tok + (before ? 7 : 6));
            if (!sf) { snprintf(err, sizeof err, ui_tr("%s: дата в виде ГГГГ-ММ-ДД"), before ? "before" : "after"); break; }
            buf_printf(&extra, before ? "&max_id=%llu" : "&min_id=%llu", (unsigned long long)sf);
        } else {
            if (content.len) buf_put(&content, " ", 1);
            buf_puts(&content, tok);
        }
    }
    free(q);
    if (err[0] || (!content.len && !extra.len)) {
        snprintf(SR.error, sizeof SR.error, "%s", err[0] ? err : ui_tr("пустой запрос"));
        buf_free(&content); buf_free(&extra);
        if (!err[0]) SR.error[0] = 0;
        ui_dirty();
        return;
    }
    unsigned gen = search_gen;
    Buf p = {0};
    if (sr_gid) buf_printf(&p, "/guilds/%llu/messages/search?", (unsigned long long)sr_gid);
    else buf_printf(&p, "/channels/%llu/messages/search?", (unsigned long long)sr_cid);
    bool first = true;
    if (content.len) { buf_puts(&p, "content="); url_encode(content.p, &p); first = false; }
    if (sr_gid && sr_cid) { buf_printf(&p, "%schannel_id=%llu", first ? "" : "&", (unsigned long long)sr_cid); first = false; }
    if (extra.len) buf_puts(&p, first ? extra.p + 1 : extra.p);
    if (sr_page > 0) buf_printf(&p, "&offset=%d", sr_page * SEARCH_PAGE);
    free(search_path);
    search_path = p.p;
    SR.busy = true;
    SR.page = sr_page;
    if (O.demo) demo_search(sr_gid, sr_cid, content.len ? content.p : "");   /* фальшивые результаты, запрос не уходит */
    else http_async("GET", search_path, NULL, on_search, (void *)(uintptr_t)gen);
    buf_free(&content);
    buf_free(&extra);
    ui_dirty();
}
void search_start(u64 guild_id, u64 channel_id, const char *query) {
    search_close();
    if (!query || (!guild_id && !channel_id)) return;
    char *q = san(query, false);
    size_t ql = strlen(q);
    if (ql >= 120) { ql = 119; while (ql && ((unsigned char)q[ql] & 0xC0) == 0x80) ql--; q[ql] = 0; }
    bool empty = !q[0];
    snprintf(sr_query, sizeof sr_query, "%s", q);
    utf8_trunc(sr_query, sizeof sr_query - 1);
    free(q);
    if (empty) return;
    sr_gid = guild_id;
    sr_cid = channel_id;
    sr_page = 0;
    search_run();
}
void search_page(int page) {
    if (!sr_query[0] || page < 0 || (SR.page_count && page >= SR.page_count)) return;
    unsigned keep = search_gen;
    search_free_hits();
    SR.error[0] = 0;
    search_gen = keep + 1;                      /* ответ прежней страницы — отбросить */
    sr_page = page;
    search_run();
}

/* ======================= Волна 1 ======================= */
/* ---------- тема открытого канала ---------- */
static void on_topic(Resp *r, void *ctx) {
    u64 id = (u64)(uintptr_t)ctx;
    if (r->status != 200 || id != topic_ch) return;
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    if (jid(j, "id") == id) topic_set(id, jstr(j, "topic"));
    json_done();
}
/* Без разбора JSON: зовётся и изнутри store_set_messages (арена вызывающего ещё жива). */
void topic_set(u64 channel_id, const char *topic) {
    Channel *c = chan_find(channel_id);
    if (!c || c->id != topic_ch) return;
    free(c->topic);
    c->topic = topic_copy(topic);
    ui_dirty();
}
static void topic_follow(void) {
    if (S.open_ch == topic_ch) return;
    unread_hold = 0;                            /* ушли из канала — «непрочитанное» больше не держим */
    Channel *old = chan_find(topic_ch);
    if (old) { free(old->topic); old->topic = NULL; }
    topic_ch = S.open_ch;
    Channel *c = chan_find(topic_ch);
    if (!c || !c->guild_id || (c->type != CH_TEXT && c->type != CH_NEWS)) return;
    if (O.demo) { demo_topic(c->id); return; }
    char path[64];
    snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)c->id);
    http_async("GET", path, NULL, on_topic, (void *)(uintptr_t)c->id);
}

/* Список сообщений из ответа (массив объектов) -> SearchHit[] (не больше max), авторы в S.users не идут. */
static int hits_from(const cJSON *arr, SearchHit **out, int max) {
    int n = cJSON_GetArraySize(arr), k = 0;
    if (n > max) n = max;
    *out = n > 0 ? xcalloc((size_t)n, sizeof(SearchHit)) : NULL;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (k >= n) break;
        if (!cJSON_IsObject(o) || !jid(o, "id")) continue;
        g_no_user_put = true;
        message_fill(&(*out)[k].m, o);
        g_no_user_put = false;
        (*out)[k++].channel_id = jid(o, "channel_id");
    }
    return k;
}
static void hits_free(SearchHit **h, int *n) {
    for (int i = 0; i < *n; i++) msg_free(&(*h)[i].m);
    free(*h);
    *h = NULL;
    *n = 0;
}
static void http_err(char *out, size_t n, const char *what, long st) {
    if (st == 0) snprintf(out, n, ui_tr("%s: нет связи с Discord"), what);
    else if (st == 403) snprintf(out, n, ui_tr("%s: нет доступа"), what);
    else if (st == 404) snprintf(out, n, ui_tr("%s: не найдено"), what);
    else if (st == 429) snprintf(out, n, ui_tr("%s: слишком часто, попробуйте позже"), what);
    else snprintf(out, n, ui_tr("%s: Discord ответил %ld"), what, st);
}
static void on_write_done(Resp *r, void *ctx) {
    if (r->status < 200 || r->status >= 300) {
        char e[160];
        http_err(e, sizeof e, ctx ? (const char *)ctx : ui_tr("запрос"), r->status);
        ui_notice("%s", e);
    }
}

/* ---------- переход к сообщению и «Вернуться к настоящему» ---------- */
static unsigned jump_gen;
static u64 jump_cid, jump_mid;
static void on_jump(Resp *r, void *ctx) {
    bool present = jump_mid == 0;
    if ((unsigned)(uintptr_t)ctx != jump_gen || S.open_ch != jump_cid) return;
    if (r->status != 200) { char e[160]; http_err(e, sizeof e, present ? ui_tr("новые сообщения") : ui_tr("переход"), r->status); ui_notice("%s", e); return; }
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    if (cJSON_IsArray(j)) store_set_messages(j, false);           /* msgs_clear: detached/jump_id сброшены */
    json_done();
    if (present) { ui_dirty(); return; }
    Channel *c = chan_find(jump_cid);
    u64 newest = S.nmsgs ? S.msgs[S.nmsgs - 1].id : 0;
    S.history_end = false;                                         /* меньше 50 вокруг — не значит, что старее нет */
    S.detached = c && newest < c->last_msg;
    S.jump_id = jump_mid;
    if (!msg_find(jump_mid, NULL)) ui_notice(ui_tr("сообщение не найдено (удалено или нет доступа)"));
    ui_dirty();
}
void msgs_jump(u64 channel_id, u64 message_id) {
    if (!channel_id || !message_id) return;
    if (channel_id != S.open_ch) ui_open_channel(channel_id);
    if (channel_id != S.open_ch) { ui_notice(ui_tr("канал недоступен")); return; }
    jump_gen++;
    jump_cid = channel_id;
    jump_mid = message_id;
    if (msg_find(message_id, NULL)) { S.jump_id = message_id; ui_dirty(); return; }   /* уже в ленте */
    if (O.demo) {
        demo_around(channel_id, message_id);
        Channel *c = chan_find(channel_id);
        if (msg_find(message_id, NULL)) {
            S.jump_id = message_id;
            S.detached = c && S.nmsgs && S.msgs[S.nmsgs - 1].id < c->last_msg;
            S.history_end = false;
        } else ui_notice(ui_tr("сообщение не найдено (удалено или нет доступа)"));
        ui_dirty();
        return;
    }
    char path[128];
    snprintf(path, sizeof path, "/channels/%llu/messages?around=%llu&limit=50", (unsigned long long)channel_id, (unsigned long long)message_id);
    http_async("GET", path, NULL, on_jump, (void *)(uintptr_t)jump_gen);
}
void msgs_present(void) {
    if (!S.open_ch) return;
    jump_gen++;
    jump_cid = S.open_ch;
    jump_mid = 0;
    if (O.demo) { demo_send(S.open_ch, NULL); return; }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/messages?limit=50", (unsigned long long)S.open_ch);
    S.jump_id = 0;
    http_async("GET", path, NULL, on_jump, (void *)(uintptr_t)jump_gen);
}

/* ---------- закреплённые ---------- */
static PinsState PN;
static unsigned pins_gen;
static char pins_before[40];                    /* pinned_at последнего — для «ещё» */
const PinsState *pins_state(void) { return &PN; }
void pins_close(void) {
    for (int i = 0; i < PN.n; i++) msg_free(&PN.m[i]);
    free(PN.m);
    memset(&PN, 0, sizeof PN);
    pins_before[0] = 0;
    pins_gen++;
}
/* {"items":[{"pinned_at":..,"message":{..}}],"has_more":b} (новый API) или массив сообщений (старый) — дописать */
void pins_apply(const cJSON *j) {
    PN.busy = false;
    const cJSON *items = cJSON_IsArray(j) ? j : jget(j, "items");
    int add = cJSON_GetArraySize(items);
    if (add > PINS_MAX - PN.n) add = PINS_MAX - PN.n;
    if (add > 0) {
        Message *nm = xcalloc((size_t)(PN.n + add), sizeof(Message));
        if (PN.n) memcpy(nm, PN.m, (size_t)PN.n * sizeof(Message));
        free(PN.m);
        PN.m = nm;
    }
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (k >= add) break;
        const cJSON *m = cJSON_IsArray(j) ? it : jget(it, "message");
        if (!cJSON_IsObject(m) || !jid(m, "id")) continue;
        g_no_user_put = true;
        message_fill(&PN.m[PN.n + k], m);
        g_no_user_put = false;
        k++;
        const char *pa = jstr(it, "pinned_at");
        pins_before[0] = 0;                     /* «ещё» — только от последнего принятого значения */
        if (pa && strlen(pa) < sizeof pins_before) {
            size_t i = 0;                       /* метка времени: только [0-9T:.+-Z] — в адрес запроса */
            for (; pa[i]; i++) if (!strchr("0123456789T:.+-Z", pa[i])) break;
            if (!pa[i]) snprintf(pins_before, sizeof pins_before, "%s", pa);
        }
    }
    PN.n += k;
    PN.more = !cJSON_IsArray(j) && jbool(j, "has_more") && PN.n < PINS_MAX && pins_before[0];
    ui_dirty();
}
static void on_pins(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != pins_gen) return;
    PN.busy = false;
    if (r->status != 200) { http_err(PN.error, sizeof PN.error, ui_tr("закреплённые"), r->status); ui_dirty(); return; }
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    pins_apply(j);
    json_done();
}
static void pins_request(void) {
    PN.busy = true;
    PN.error[0] = 0;
    if (O.demo) { demo_pins(PN.channel_id); return; }
    Buf p = {0};
    buf_printf(&p, "/channels/%llu/messages/pins?limit=25", (unsigned long long)PN.channel_id);
    if (pins_before[0]) { buf_puts(&p, "&before="); url_encode(pins_before, &p); }
    http_async("GET", p.p, NULL, on_pins, (void *)(uintptr_t)pins_gen);
    buf_free(&p);
}
void pins_open(u64 channel_id) {
    pins_close();
    if (!channel_id) return;
    PN.channel_id = channel_id;
    pins_request();
    ui_dirty();
}
void pins_more(void) { if (PN.channel_id && PN.more && !PN.busy) pins_request(); }
static void pins_changed(u64 channel_id) {
    if (PN.channel_id && PN.channel_id == channel_id) pins_open(channel_id);
    ui_dirty();
}
/* Итог закрепления — уведомлением по ответу сервера; m->pinned — только при успехе. */
static void pin_result(u64 msg_id, bool pin, long status) {
    if (status >= 200 && status < 300) {
        Message *m = msg_find(msg_id, NULL);
        if (m) m->pinned = pin;
        ui_notice("%s", pin ? ui_tr("сообщение закреплено") : ui_tr("сообщение откреплено"));
    } else {
        char e[160];
        if (status == 403) snprintf(e, sizeof e, ui_tr("%s: нет прав (403)"), pin ? ui_tr("закрепить") : ui_tr("открепить"));
        else http_err(e, sizeof e, pin ? ui_tr("закрепить") : ui_tr("открепить"), status);
        ui_notice("%s", e);
    }
    ui_dirty();
}
static u64 pin_mid;                             /* закрепление — по одному: id и действие последнего запроса */
static bool pin_on;
static void on_pin(Resp *r, void *ctx) { (void)ctx; pin_result(pin_mid, pin_on, r->status); }
void msg_pin(u64 channel_id, u64 msg_id, bool pin) {
    if (!channel_id || !msg_id) return;
    if (O.demo) { demo_pin(channel_id, msg_id, pin); pin_result(msg_id, pin, 204); pins_changed(channel_id); return; }
    pin_mid = msg_id;
    pin_on = pin;
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/messages/pins/%llu", (unsigned long long)channel_id, (unsigned long long)msg_id);
    http_async(pin ? "PUT" : "DELETE", path, NULL, on_pin, NULL);
}

/* ---------- «Упоминания» ---------- */
static MentionsState MN;
static unsigned ment_gen;
const MentionsState *mentions_state(void) { return &MN; }
void mentions_close(void) { hits_free(&MN.hits, &MN.n); memset(&MN, 0, sizeof MN); ment_gen++; }
void mentions_apply(const cJSON *arr) {
    hits_free(&MN.hits, &MN.n);
    MN.busy = false;
    MN.n = hits_from(arr, &MN.hits, SEARCH_PAGE);
    ui_dirty();
}
static void on_mentions(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != ment_gen) return;
    MN.busy = false;
    if (r->status != 200) { http_err(MN.error, sizeof MN.error, ui_tr("упоминания"), r->status); ui_dirty(); return; }
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    mentions_apply(j);
    json_done();
}
void mentions_open(void) {
    mentions_close();
    MN.busy = true;
    if (O.demo) { demo_mentions(); return; }
    http_async("GET", "/users/@me/mentions?limit=25&roles=true&everyone=true", NULL, on_mentions, (void *)(uintptr_t)ment_gen);
    ui_dirty();
}

/* ---------- прочитано ---------- */
/* Недавние отметки по каналам: не чаще раза в 3 с на канал; что не ушло — pend, отправит store_tick. */
#define ACK_SLOTS 16
#define ACK_GAP 3000
static struct { u64 ch, pend; i64 at; } AK[ACK_SLOTS];
__attribute__((weak)) bool win_focused(void) { return true; }
static void ack_post(u64 ch, u64 msg, bool manual) {
    char path[112], body[96];
    snprintf(path, sizeof path, "/channels/%llu/messages/%llu/ack", (unsigned long long)ch, (unsigned long long)msg);
    if (manual) snprintf(body, sizeof body, "{\"manual\":true,\"mention_count\":0}");
    else {
        i64 days = (wall_ms() - DISCORD_EPOCH_MS + 86399999) / 86400000;
        snprintf(body, sizeof body, "{\"token\":null,\"last_viewed\":%lld}", (long long)days);
    }
    http_async("POST", path, body, NULL, NULL);
}
static void ack_queue(u64 ch, u64 msg, bool manual) {
    if (O.demo || O.no_ack || !msg) return;
    i64 now = now_ms();
    int slot = -1, old = 0;
    for (int i = 0; i < ACK_SLOTS; i++) {
        if (AK[i].ch == ch) { slot = i; break; }
        if (AK[i].at < AK[old].at && !AK[i].pend) old = i;
    }
    if (slot < 0) { slot = old; if (AK[slot].pend) ack_post(AK[slot].ch, AK[slot].pend, false); AK[slot] = (typeof(AK[0])){ ch, 0, 0 }; }
    if (manual) { ack_post(ch, msg, true); AK[slot].at = now; AK[slot].pend = 0; return; }   /* «непрочитанное» — сразу */
    if (now - AK[slot].at < ACK_GAP || !win_focused()) { AK[slot].pend = msg; return; }
    ack_post(ch, msg, false);
    AK[slot].at = now;
    AK[slot].pend = 0;
}
/* Авто-отметка при просмотре (ui.c: новое сообщение в открытом канале): не снимает ручное «непрочитанное». */
void ack_seen(u64 channel_id) { if (channel_id != unread_hold) ack_channel(channel_id); }
/* Явное «прочитано» (меню, клавиша): снимает и ручное «непрочитанное». */
void ack_channel(u64 channel_id) {
    Channel *c = chan_find(channel_id);
    if (!c) return;
    if (channel_id == unread_hold) unread_hold = 0;
    u64 target = c->last_msg;
    if (S.open_ch == channel_id)                /* С1: id «ожидающего» (nonce) — не настоящий, берём последнее отправленное */
        for (int k = S.nmsgs - 1; k >= 0; k--)
            if (S.msgs[k].state == MS_SENT && pend_find(S.msgs[k].id) < 0) { if (S.msgs[k].id > target) target = S.msgs[k].id; break; }
    if (target <= c->last_read && !c->mentions) return;
    if (target > c->last_read) c->last_read = target;
    c->mentions = 0;
    ack_queue(channel_id, target, false);
    ui_dirty();
}
/* Пачкой: POST /read-states/ack-bulk по 100 каналов. guild_id == ~0 — всё (серверы и ЛС). */
static void ack_bulk(u64 gid) {
    Buf b = {0};
    int n = 0;
    for (int i = 0; i <= S.nchans; i++) {
        Channel *c = i < S.nchans ? &S.chans[i] : NULL;
        if (c && (gid == ~0ULL || c->guild_id == gid) && c->visible && chan_counts(c) && c->type != CH_CATEGORY && c->type != CH_VOICE &&
            (c->last_msg > c->last_read || c->mentions)) {
            if (c->last_msg > c->last_read) c->last_read = c->last_msg;
            c->mentions = 0;
            if (!O.demo && !O.no_ack && c->last_msg) {
                buf_printf(&b, "%s{\"channel_id\":\"%llu\",\"message_id\":\"%llu\",\"read_state_type\":0}",
                           n ? "," : "{\"read_states\":[", (unsigned long long)c->id, (unsigned long long)c->last_msg);
                n++;
            }
        }
        if (n && (n == 100 || !c)) {
            buf_puts(&b, "]}");
            http_async("POST", "/read-states/ack-bulk", b.p, on_write_done, (void *)ui_tr("прочитано"));
            b.len = 0;
            n = 0;
        }
    }
    buf_free(&b);
    ui_dirty();
}
void ack_guild(u64 guild_id) { if (guild_id) ack_bulk(guild_id); }
void ack_all(void) { ack_bulk(~0ULL); }
void mark_unread(u64 channel_id, u64 msg_id) {
    Channel *c = chan_find(channel_id);
    if (!c || !msg_id) return;
    c->last_read = msg_id - 1;                                  /* линия «Новые» — по last_read (рисует ui.c) */
    unread_hold = channel_id;                                   /* до перехода в другой канал */
    for (int i = 0; i < ACK_SLOTS; i++) if (AK[i].ch == channel_id) AK[i].pend = 0;
    ack_queue(channel_id, msg_id - 1, true);
    ui_dirty();
}

/* ---------- свой статус ---------- */
static u8 my_ps = 255, ps_pend = 255;
static i64 ps_sent[5];                          /* время последних 5 отправок op 3 */
u8 presence_me(void) { if (my_ps == 255) my_ps = O.invisible ? PS_INVISIBLE : PS_ONLINE; return my_ps; }
static const char *ps_word(u8 st) {
    return st == PS_IDLE ? "idle" : st == PS_DND ? "dnd" : st == PS_INVISIBLE ? "invisible" : "online";
}
const char *presence_word(void) { return ps_word(presence_me()); }   /* net.c: статус в IDENTIFY */
static void ps_send(u8 st) {
    if (!gw_online()) return;                   /* не в сети: статус уйдёт в IDENTIFY при подключении, не засчитываем */
    Buf b = {0};
    buf_printf(&b, "{\"op\":3,\"d\":{\"status\":\"%s\",\"since\":0,\"activities\":[", ps_word(st));
    if (*custom_status()) {                     /* №104: текстовый статус — активность type 4 */
        buf_puts(&b, "{\"name\":\"Custom Status\",\"type\":4,\"state\":");
        buf_json_str(&b, custom_status());
        buf_puts(&b, "}");
    }
    buf_puts(&b, "],\"afk\":false}}");
    gw_send(b.p);
    buf_free(&b);
    memmove(ps_sent, ps_sent + 1, sizeof ps_sent - sizeof *ps_sent);
    ps_sent[4] = now_ms();
}
static bool ps_allowed(void) { return !ps_sent[0] || now_ms() - ps_sent[0] >= 20000; }
void presence_set(u8 st) {
    if (st != PS_ONLINE && st != PS_IDLE && st != PS_DND && st != PS_INVISIBLE) return;
    presence_me();
    my_ps = st;
    ui_dirty();
    if (O.demo) return;
    if (ps_allowed()) { ps_send(st); ps_pend = 255; } else ps_pend = st;   /* побеждает последнее */
}
/* ---------- исходящий звонок в ЛС ----------
 * Раньше: op 4 (вход в голос) и сразу POST /call/ring. REST мог прийти к Discord раньше, чем gateway обработал
 * op 4 и создал звонок, — ring уходил «в пустоту», собеседнику не звонило (входящий путь ring не требует,
 * поэтому там всё работало). Теперь как у официального клиента: op 4, звонок — по CALL_CREATE этой ЛС
 * (звонок уже существует); если CALL_CREATE не пришёл за 5 с — звоним всё равно. */
#define RING_WAIT_MS 5000
static u64 ring_ch;                             /* ЛС, где ждём CALL_CREATE, чтобы позвонить */
static i64 ring_at;                             /* крайний срок ожидания */
static void ring_send(u64 ch) {
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/call/ring", (unsigned long long)ch);
    http_async("POST", path, "{\"recipients\":null}", on_write_done, (void *)ui_tr("звонок"));
}
void call_start(u64 channel_id) {
    Channel *c = chan_find(channel_id);
    if (!c || c->guild_id) { ui_notice(ui_tr("звонок - из личной переписки")); return; }
    voice_join(0, channel_id);
    if (O.demo) return;
    ring_ch = channel_id;
    ring_at = now_ms() + RING_WAIT_MS;
}
u64 call_ring_pending(void) { return ring_ch; }
/* №138: кому звонят в звонке канала cid (по CALL_CREATE/UPDATE), до 10 */
static u64 rg_ch, rg_ids[10];
static int rg_n;
static void ringing_note(u64 cid, const cJSON *arr) {
    if (rg_ch && rg_ch != cid && rg_n && !arr) return;   /* чужой звонок без списка не сбрасывает текущий */
    rg_ch = cid;
    rg_n = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) if (rg_n < 10) rg_ids[rg_n++] = id_of(r);
}
int call_ringing(u64 cid, u64 *out, int max) {
    if (cid != rg_ch) return 0;
    int n = rg_n < max ? rg_n : max;
    memcpy(out, rg_ids, (size_t)n * sizeof(u64));
    return n;
}
static void ring_post(u64 cid, u64 uid, bool stop) {
    Channel *c = chan_find(cid);
    if (!c || c->guild_id) return;
    if (O.demo) {
        if (stop) { for (int i = 0; i < rg_n; i++) if (!uid || rg_ids[i] == uid) rg_ids[i--] = rg_ids[--rg_n]; }
        else if (uid && rg_n < 10) { rg_ch = cid; rg_ids[rg_n++] = uid; }
        ui_dirty();
        return;
    }
    char path[96], body[64];
    snprintf(path, sizeof path, "/channels/%llu/call/%s", (unsigned long long)cid, stop ? "stop-ringing" : "ring");
    if (uid) snprintf(body, sizeof body, "{\"recipients\":[\"%llu\"]}", (unsigned long long)uid);
    else snprintf(body, sizeof body, "{\"recipients\":null}");
    http_async("POST", path, body, on_write_done, (void *)(stop ? ui_tr("перестать звонить") : ui_tr("звонок")));
}
static bool ring_still_wanted(u64 ch);
/* ревью 13 М4: управлять звонком можно только в своём (я в голосе этой ЛС) */
static bool in_call(u64 cid) {                  /* я в голосе этой ЛС (в любой фазе, кроме «нет звонка») */
    VoiceStatus vs;
    voice_status(&vs);
    return vs.phase != VS_IDLE && vs.channel_id == cid;
}
void call_ring_user(u64 cid, u64 uid) { if (uid && uid != S.me_id && in_call(cid)) ring_post(cid, uid, false); }
void call_stop_ring(u64 cid, u64 uid) { if (in_call(cid)) ring_post(cid, uid, true); }
/* Звонить только пока мы действительно в голосе этой ЛС: положили трубку до CALL_CREATE — звонка нет. */
static bool ring_still_wanted(u64 ch) {
    VoiceStatus vs;
    voice_status(&vs);
    return vs.phase != VS_IDLE && vs.phase != VS_FAILED && vs.channel_id == ch;
}
static void call_created(u64 ch) {               /* CALL_CREATE: звонок есть — теперь звоним */
    if (!ring_ch || ch != ring_ch) return;
    ring_ch = 0;
    if (ring_still_wanted(ch)) ring_send(ch);
}
void store_tick(void) {
    i64 now = now_ms();
    ix_tick(now);
    cdn_flush(now);
    demo_slow_tick(now);
    if (demo_del_th && now >= demo_del_at) {   /* демо: «сервер удалил ветку» */
        char j[160];
        Channel *dc = chan_find(demo_del_th);
        snprintf(j, sizeof j, "{\"id\":\"%llu\",\"guild_id\":\"%llu\",\"parent_id\":\"%llu\",\"type\":11}", (unsigned long long)demo_del_th,
                 dc ? (unsigned long long)dc->guild_id : 0ULL, dc ? (unsigned long long)dc->parent_id : 0ULL);
        demo_del_th = 0;
        cJSON *o = json_parse(j, strlen(j));
        if (o) store_event("THREAD_DELETE", o);
        json_done();
    }
    if (ring_ch && !ring_still_wanted(ring_ch)) ring_ch = 0;                         /* трубку положили — не звоним */
    if (ring_ch && now >= ring_at) { u64 ch = ring_ch; ring_ch = 0; ring_send(ch); }   /* CALL_CREATE не пришёл */
    if (ps_pend != 255 && ps_allowed()) { ps_send(ps_pend); ps_pend = 255; }
    if (!win_focused()) return;
    for (int i = 0; i < ACK_SLOTS; i++)
        if (AK[i].pend && now - AK[i].at >= ACK_GAP) { ack_post(AK[i].ch, AK[i].pend, false); AK[i].at = now; AK[i].pend = 0; }
}
i64 store_next_wake(void) {
    i64 t = INT64_MAX;
    if (ring_ch) t = ring_at;
    { i64 c = cdn_next_wake(); if (c < t) t = c; }
    if (demo_slow && demo_slow_at < t) t = demo_slow_at;
    if (demo_del_th && demo_del_at < t) t = demo_del_at;
    if (ix_nonce && ix_until < t) t = ix_until;
    { i64 f = focus_next_wake(); if (f < t) t = f; }
    { i64 f = status_next_wake(); if (f < t) t = f; }
    { i64 s = slow_next_wake(); if (s < t) t = s; }
    if (ps_pend != 255 && ps_sent[0] + 20000 < t) t = ps_sent[0] + 20000;   /* ревью 8 М1: минимум, не затирать */
    if (win_focused())
        for (int i = 0; i < ACK_SLOTS; i++) if (AK[i].pend && AK[i].at + ACK_GAP < t) t = AK[i].at + ACK_GAP;
    return t;
}

/* ---------- меню человека ---------- */
void rel_block(u64 uid) {
    if (!uid || uid == S.me_id) return;
    if (O.demo) { rel_set(uid, REL_BLOCKED); ui_dirty(); return; }
    char path[64];
    snprintf(path, sizeof path, "/users/@me/relationships/%llu", (unsigned long long)uid);
    http_async("PUT", path, "{\"type\":2}", on_write_done, (void *)ui_tr("заблокировать"));
}
void rel_add(u64 uid) {
    if (!uid || uid == S.me_id) return;
    const Relation *r = rel_find(uid);
    if (r && (r->type == REL_FRIEND || r->type == REL_OUT)) return;
    if (r && r->type == REL_IN) { rel_accept(uid); return; }
    if (O.demo) { rel_set(uid, REL_OUT); ui_dirty(); return; }
    char path[64];
    snprintf(path, sizeof path, "/users/@me/relationships/%llu", (unsigned long long)uid);
    http_async("PUT", path, "{}", on_write_done, (void *)ui_tr("заявка в друзья"));
}

/* «Добавить в друзья» по имени пользователя (как кнопка Discord): POST /users/@me/relationships
 * {"username":..,"discriminator":null}. Имя — только [a-z0-9_.], 2..32 (правила имён Discord). */
static void on_rel_name(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status == 204 || r->status == 200) { ui_notice(ui_tr("заявка в друзья отправлена")); return; }
    if (r->status == 400) { ui_notice(ui_tr("заявка не отправлена: нет такого пользователя или он не принимает заявки")); return; }
    char e[160];
    http_err(e, sizeof e, ui_tr("заявка в друзья"), r->status);
    ui_notice("%s", e);
}
bool rel_add_name(const char *username, char *err, size_t errn) {
    size_t n = username ? strlen(username) : 0;
    if (n < 2 || n > 32) { snprintf(err, errn, ui_tr("имя пользователя: 2-32 символа")); return false; }
    for (size_t i = 0; i < n; i++) {
        char c = username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            snprintf(err, errn, ui_tr("имя пользователя: только латиница a-z, цифры, _ и ."));
            return false;
        }
    }
    if (O.demo) {
        if (!strcmp(username, "captcha")) {     /* демо-триггер окна капчи (для снимков дизайнера) */
            net_captcha_test("POST", "/users/@me/relationships", "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":"
                             "\"a9b5fb07-92ff-493f-86fe-352a2803b3df\",\"captcha_service\":\"hcaptcha\"}", NULL, NULL);
            return true;
        }
        ui_notice(ui_tr("заявка в друзья отправлена (демо)"));
        return true;
    }
    char body[96];
    snprintf(body, sizeof body, "{\"username\":\"%s\",\"discriminator\":null}", username);
    http_async("POST", "/users/@me/relationships", body, on_rel_name, NULL);
    return true;
}

/* ---------- карточка профиля ---------- */
static Profile PF;
static char *pf_s[4];                           /* name, username, pronouns, bio */
static u64 *pf_roles, *pf_mut, *pf_fr;          /* №99: общие серверы и друзья */
static char *pf_conn[8];
static bool pf_full;
static unsigned pf_gen;
const Profile *profile_state(void) { return &PF; }
void profile_close(void) {
    for (int i = 0; i < 4; i++) { free(pf_s[i]); pf_s[i] = NULL; }
    free(pf_roles);
    pf_roles = NULL;
    free(pf_mut); free(pf_fr);
    pf_mut = pf_fr = NULL;
    for (int i = 0; i < 8; i++) { free(pf_conn[i]); pf_conn[i] = NULL; }
    memset(&PF, 0, sizeof PF);
    pf_gen++;
}
static char *pf_str(const char *s, bool multiline, size_t max) {
    if (!s || !*s) return NULL;
    char *t = san(s, multiline);
    size_t n = strlen(t);
    if (n > max) { n = max - 3; while (n && ((unsigned char)t[n] & 0xC0) == 0x80) n--; memcpy(t + n, "...", 4); }
    return t;
}
void profile_apply(const cJSON *j) {
    u64 id = PF.id, gid = PF.guild_id;
    profile_close();
    PF.id = id;
    PF.guild_id = gid;
    const cJSON *u = jget(j, "user"), *up = jget(j, "user_profile"), *gm = jget(j, "guild_member"), *gp = jget(j, "guild_member_profile");
    const char *nick = jstr(gm, "nick");
    pf_s[0] = pf_str(nick ? nick : display_name(u), false, 64);
    pf_s[1] = pf_str(jstr(u, "username"), false, 64);
    const char *pr = jstr(gp, "pronouns");
    if (!pr || !*pr) pr = jstr(up, "pronouns");
    pf_s[2] = pf_str(pr, false, 40);
    const char *bio = jstr(gp, "bio");
    if (!bio || !*bio) bio = jstr(up, "bio");
    if (!bio || !*bio) bio = jstr(u, "bio");
    pf_s[3] = pf_str(bio, true, 400);
    PF.name = pf_s[0] ? pf_s[0] : "?";
    PF.username = pf_s[1] ? pf_s[1] : "";
    PF.pronouns = pf_s[2] ? pf_s[2] : "";
    PF.bio = pf_s[3] ? pf_s[3] : "";
    i64 ac = jint(up, "accent_color", -1);
    if (ac < 0) ac = jint(u, "accent_color", 0);
    PF.accent = (u32)ac & 0xFFFFFFu;
    u64 uid = jid(u, "id") ? jid(u, "id") : id;
    PF.since_unix = (i64)(((uid >> 22) + (u64)DISCORD_EPOCH_MS) / 1000);
    PF.member_since_unix = gm ? iso_to_unix(jstr(gm, "joined_at")) : 0;
    PF.bot = jbool(u, "bot");
    int nr = cJSON_GetArraySize(jget(gm, "roles"));
    if (nr > 64) nr = 64;
    if (nr > 0) {
        pf_roles = xcalloc((size_t)nr, sizeof(u64));
        const cJSON *r;
        cJSON_ArrayForEach(r, jget(gm, "roles")) { if (PF.nroles >= nr) break; pf_roles[PF.nroles++] = id_of(r); }
    }
    PF.roles = pf_roles;
    if (pf_full) {                              /* №99 */
        PF.full = true;
        int nm = cJSON_GetArraySize(jget(j, "mutual_guilds")), nf = cJSON_GetArraySize(jget(j, "mutual_friends"));
        if (nm > 100) nm = 100;
        if (nf > 100) nf = 100;
        const cJSON *x;
        if (nm > 0) { pf_mut = xcalloc((size_t)nm, sizeof(u64)); cJSON_ArrayForEach(x, jget(j, "mutual_guilds")) { if (PF.nmutual >= nm) break; if (jid(x, "id")) pf_mut[PF.nmutual++] = jid(x, "id"); } }
        if (nf > 0) {
            pf_fr = xcalloc((size_t)nf, sizeof(u64));
            cJSON_ArrayForEach(x, jget(j, "mutual_friends")) {
                if (PF.nfriends >= nf) break;
                const cJSON *fu = jget(x, "user") ? jget(x, "user") : x;
                if (!jid(fu, "id")) continue;
                user_put_obj(fu);
                pf_fr[PF.nfriends++] = jid(fu, "id");
            }
        }
        cJSON_ArrayForEach(x, jget(j, "connected_accounts")) {
            if (PF.nconn >= 8) break;
            const char *ty = jstr(x, "type"), *nmx = jstr(x, "name");
            if (!ty || !nmx) continue;
            char b[160];
            snprintf(b, sizeof b, "%.20s: %.100s", ty, nmx);
            pf_conn[PF.nconn] = pf_str(b, false, 120);
            if (pf_conn[PF.nconn]) { PF.conn[PF.nconn] = pf_conn[PF.nconn]; PF.nconn++; }
        }
        PF.mutual = pf_mut;
        PF.friends = pf_fr;
    }
    ui_dirty();
}
static void on_profile(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != pf_gen) return;
    PF.busy = false;
    if (r->status != 200) { http_err(PF.error, sizeof PF.error, ui_tr("профиль"), r->status); ui_dirty(); return; }
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    profile_apply(j);
    json_done();
}
static void profile_req(u64 uid, u64 guild_id, bool full);
void profile_open(u64 uid, u64 guild_id) { profile_req(uid, guild_id, false); }
void profile_open_full(u64 uid, u64 guild_id) { profile_req(uid, guild_id, true); }
static void profile_req(u64 uid, u64 guild_id, bool full) {
    profile_close();
    pf_full = full;
    if (!uid) return;
    PF.id = uid;
    PF.guild_id = guild_id;
    PF.busy = true;
    pf_s[0] = xstrdup(user_name(uid));          /* своя копия: указатель в S.users может освободиться */
    PF.name = pf_s[0];
    PF.username = PF.pronouns = PF.bio = "";
    if (O.demo) { demo_profile(uid, guild_id); return; }
    char path[160];
    const char *fl = full ? "true" : "false";
    if (guild_id) snprintf(path, sizeof path, "/users/%llu/profile?with_mutual_guilds=%s&with_mutual_friends=%s&guild_id=%llu",
                           (unsigned long long)uid, fl, fl, (unsigned long long)guild_id);
    else snprintf(path, sizeof path, "/users/%llu/profile?with_mutual_guilds=%s&with_mutual_friends=%s", (unsigned long long)uid, fl, fl);
    http_async("GET", path, NULL, on_profile, (void *)(uintptr_t)pf_gen);
    ui_dirty();
}

/* ---------- события ---------- */
static bool mentions_me(const cJSON *d) {
    const cJSON *m;
    cJSON_ArrayForEach(m, jget(d, "mentions")) if (jid(m, "id") == S.me_id) return true;
    return jbool(d, "mention_everyone");
}

static void react_change(const cJSON *d, int delta) {
    if (jid(d, "channel_id") != S.open_ch) return;
    Message *m = msg_find(jid(d, "message_id"), NULL);
    if (!m) return;
    const cJSON *em = jget(d, "emoji");
    const char *name = jstr(em, "name");
    u64 eid = jid(em, "id");
    char key[160];
    if (eid) snprintf(key, sizeof key, ":%s:%llu", name ? name : "?", (unsigned long long)eid);
    else snprintf(key, sizeof key, "%s", name ? name : "?");
    char *k = san(key, false);
    bool mine = jid(d, "user_id") == S.me_id;
    for (int i = 0; i < m->nreacts; i++) if (!strcmp(m->reacts[i].emoji, k)) {
        m->reacts[i].count += delta;
        if (mine) m->reacts[i].me = delta > 0;
        if (m->reacts[i].count <= 0) { free(m->reacts[i].emoji); m->reacts[i] = m->reacts[--m->nreacts]; }
        free(k);
        return;
    }
    if (delta > 0) {
        m->reacts = xrealloc(m->reacts, (size_t)(m->nreacts + 1) * sizeof(Reaction));
        m->reacts[m->nreacts++] = (Reaction){ k, 1, mine };
        return;
    }
    free(k);
}

void store_event(const char *t, const cJSON *d) {
    if (!S.ready) return;
    if (!strcmp(t, "MESSAGE_CREATE")) {
        u64 cid = jid(d, "channel_id"), id = jid(d, "id");
        Channel *c = chan_find(cid);
        if (c) {
            if (id > c->last_msg) c->last_msg = id;
            if (jid(jget(d, "author"), "id") == S.me_id) { c->last_read = id; c->mentions = 0; }
        }
        bool mention = jid(jget(d, "author"), "id") != S.me_id && !ignored(jid(jget(d, "author"), "id")) &&   /* №108 */
                       (mentions_me(d) || (c && !c->guild_id) || (!focus_until() && alert_text(jstr(d, "content"))));   /* №121: в фокусе — только ЛС и @меня */
        if (c && mention && cid != S.open_ch && !c->muted && (!c->guild_id || notify_of(c) != NOTIFY_NONE)) c->mentions++;
        if (c && mention && (!c->guild_id || notify_of(c) != NOTIFY_NONE)) notify_msg(d, c);   /* №123/124 */
        pend_arrived(d);                            /* своё «ожидающее» по nonce — заменить настоящим */
        if (cid == S.open_ch && !S.detached && !msg_find(id, NULL)) {   /* после перехода — разрыв: не дописываем */
            message_fill(msg_insert(id), d);
            msgs_trim(false);
            u64 uid = jid(jget(d, "author"), "id");
            if (uid && S.typing_until && !strcmp(S.typing_name, user_name(uid))) S.typing_until = 0;
        }
        if (c && !c->guild_id) store_sort_dms();
        ui_on_message(cid, mention);
    } else if (!strcmp(t, "MESSAGE_UPDATE")) {
        if (jid(d, "channel_id") != S.open_ch) return;
        Message *m = msg_find(jid(d, "id"), NULL);
        if (!m || !jstr(d, "content")) return;
        /* В обновлении может не быть автора; дописываем недостающее из старого */
        char *author = xstrdup(m->author);
        u64 aid = m->author_id;
        i64 ts = m->ts;
        message_fill(m, d);
        if (!jget(d, "author")) { free(m->author); m->author = author; m->author_id = aid; } else free(author);
        if (!m->ts) m->ts = ts;
        m->edited = true;
        ui_dirty();
    } else if (!strcmp(t, "MESSAGE_DELETE")) {
        if (jid(d, "channel_id") != S.open_ch) return;
        int i;
        if (msg_find(jid(d, "id"), &i)) {
            msg_free(&S.msgs[i]);
            memmove(&S.msgs[i], &S.msgs[i + 1], (size_t)(S.nmsgs - i - 1) * sizeof(Message));
            S.nmsgs--;
            ui_dirty();
        }
    } else if (!strcmp(t, "MESSAGE_DELETE_BULK")) {
        if (jid(d, "channel_id") != S.open_ch) return;
        const cJSON *e;
        cJSON_ArrayForEach(e, jget(d, "ids")) {
            int i;
            if (msg_find(id_of(e), &i)) {
                msg_free(&S.msgs[i]);
                memmove(&S.msgs[i], &S.msgs[i + 1], (size_t)(S.nmsgs - i - 1) * sizeof(Message));
                S.nmsgs--;
            }
        }
        ui_dirty();
    } else if (!strcmp(t, "MESSAGE_REACTION_ADD")) { react_change(d, 1); ui_dirty(); }
    else if (!strncmp(t, "THREAD_", 7)) thread_event(t, d);
    else if (!strcmp(t, "INTERACTION_SUCCESS") || !strcmp(t, "INTERACTION_FAILURE")) ix_event(t, d);
    else if (!strcmp(t, "USER_SETTINGS_PROTO_UPDATE")) folders_event(d);
    else if (!strcmp(t, "VOICE_CHANNEL_STATUS_UPDATE") || !strcmp(t, "CHANNEL_INFO")) vcs_event(t, d);
    else if (!strcmp(t, "MESSAGE_POLL_VOTE_ADD")) poll_vote_ev(d, 1);
    else if (!strcmp(t, "MESSAGE_POLL_VOTE_REMOVE")) poll_vote_ev(d, -1);
    else if (!strcmp(t, "MESSAGE_REACTION_REMOVE")) { react_change(d, -1); ui_dirty(); }
    else if (!strcmp(t, "MESSAGE_REACTION_REMOVE_ALL") || !strcmp(t, "MESSAGE_REACTION_REMOVE_EMOJI")) {
        if (jid(d, "channel_id") != S.open_ch) return;
        Message *m = msg_find(jid(d, "message_id"), NULL);
        if (m) { reactions_from(m, NULL); ui_dirty(); }
    } else if (!strcmp(t, "MESSAGE_ACK")) {
        Channel *c = chan_find(jid(d, "channel_id"));
        if (c) { u64 id = jid(d, "message_id"); if (id > c->last_read) c->last_read = id; c->mentions = (int)jint(d, "mention_count", 0); ui_dirty(); }
    } else if (!strcmp(t, "TYPING_START")) {
        u64 uid = jid(d, "user_id");
        if (jid(d, "channel_id") != S.open_ch || uid == S.me_id) return;
        const cJSON *mu = jget(jget(d, "member"), "user");
        const char *nick = jstr(jget(d, "member"), "nick");
        char *n = san(nick ? nick : mu ? display_name(mu) : user_name(uid), false);
        size_t tl = strlen(n);
        if (tl >= sizeof S.typing_name) { tl = sizeof S.typing_name - 1; while (tl && ((unsigned char)n[tl] & 0xC0) == 0x80) tl--; }
        memcpy(S.typing_name, n, tl);
        S.typing_name[tl] = 0;
        free(n);
        S.typing_until = now_ms() + 10000;
        ui_dirty();
    } else if (!strcmp(t, "CHANNEL_CREATE") || !strcmp(t, "CHANNEL_UPDATE")) {
        chan_upsert(d, 0);
        store_sort_dms();
        ui_dirty();
    } else if (!strcmp(t, "CHANNEL_DELETE")) {
        chan_remove(jid(d, "id"));
        ui_dirty();
    } else if (!strcmp(t, "CHANNEL_RECIPIENT_ADD") || !strcmp(t, "CHANNEL_RECIPIENT_REMOVE")) {
        Channel *c = chan_find(jid(d, "channel_id"));
        const cJSON *u = jget(d, "user");
        u64 uid = jid(u, "id");
        if (!c || !uid) return;
        user_put_obj(u);
        if (t[18] == 'A') {
            c->recipients = xrealloc(c->recipients, (size_t)(c->nrecip + 1) * sizeof(u64));
            c->recipients[c->nrecip++] = uid;
        } else {
            for (int i = 0; i < c->nrecip; i++) if (c->recipients[i] == uid) { c->recipients[i] = c->recipients[--c->nrecip]; break; }
        }
        ui_dirty();
    } else if (!strcmp(t, "GUILD_CREATE")) {
        const cJSON *mine = NULL, *mm;
        cJSON_ArrayForEach(mm, jget(d, "members")) if (jid(jget(mm, "user"), "id") == S.me_id) { mine = mm; break; }
        guild_load(d, mine);
        ui_dirty();
    } else if (!strcmp(t, "GUILD_UPDATE")) {
        Guild *g = guild_find(jid(d, "id"));
        if (!g) return;
        const char *n = jstr(d, "name");
        if (n) { free(g->name); g->name = san(n, false); }
        if (jget(d, "icon")) hash_copy(g->icon, jstr(d, "icon"));
        if (jget(d, "banner")) g->banner_color = banner_color(jstr(d, "banner"));
        g->owner_id = jid(d, "owner_id") ? jid(d, "owner_id") : g->owner_id;
        ui_dirty();
    } else if (!strcmp(t, "GUILD_DELETE")) {
        if (jbool(d, "unavailable")) return;
        u64 gid = jid(d, "id");
        for (int i = S.nchans - 1; i >= 0; i--) if (S.chans[i].guild_id == gid) { chan_free(&S.chans[i]); S.chans[i] = S.chans[--S.nchans]; }
        for (int i = 0; i < S.nguilds; i++) if (S.guilds[i].id == gid) { guild_free(&S.guilds[i]); memmove(&S.guilds[i], &S.guilds[i + 1], (size_t)(S.nguilds - i - 1) * sizeof(Guild)); S.nguilds--; break; }
        store_sort_dms();
        ui_dirty();
    } else if (!strncmp(t, "GUILD_ROLE_", 11)) {
        u64 gid = jid(d, "guild_id");
        Guild *g = guild_find(gid);
        if (!g) return;
        const cJSON *r = jget(d, "role");
        u64 rid = r ? jid(r, "id") : jid(d, "role_id");
        int i;
        for (i = 0; i < g->nroles && g->roles[i].id != rid; i++) {}
        if (!strcmp(t + 11, "DELETE")) {
            if (i < g->nroles) g->roles[i] = g->roles[--g->nroles];
        } else {
            if (i == g->nroles) { g->roles = xrealloc(g->roles, (size_t)(g->nroles + 1) * sizeof(Role)); g->nroles++; }
            g->roles[i].id = rid;
            g->roles[i].perms = perm_str(r, "permissions");
            g->roles[i].color = (u32)jint(r, "color", 0) & 0xFFFFFFu;
            g->roles[i].pos = (int)jint(r, "position", 0);
        }
        recompute_guild(gid);
        ui_dirty();
    } else if (!strcmp(t, "GUILD_MEMBER_UPDATE")) {
        if (jid(jget(d, "user"), "id") != S.me_id) return;
        Guild *g = guild_find(jid(d, "guild_id"));
        if (!g) return;
        g->timeout_until = iso_to_unix(jstr(d, "communication_disabled_until"));
        g->pending = jbool(d, "pending");
        free(g->my_roles);
        g->my_roles = NULL;
        g->nmy = 0;
        int n = cJSON_GetArraySize(jget(d, "roles"));
        if (n > 0) {
            g->my_roles = xcalloc((size_t)n, sizeof(u64));
            const cJSON *r;
            cJSON_ArrayForEach(r, jget(d, "roles")) g->my_roles[g->nmy++] = id_of(r);
        }
        recompute_guild(g->id);
        ui_dirty();
    } else if (!strcmp(t, "USER_GUILD_SETTINGS_UPDATE")) {
        apply_guild_settings(d);
        ui_dirty();
    } else if (!strcmp(t, "READY_SUPPLEMENTAL")) {
        /* Статусы друзей: у пользовательских аккаунтов — здесь (merged_presences.friends), а не в READY. */
        const cJSON *fp;
        cJSON_ArrayForEach(fp, jget(jget(d, "merged_presences"), "friends")) presence_apply(fp);
        /* Кто уже сидит в голосовых каналах, Discord сообщает здесь, а не в READY.
         * merged_members[i] — участники из voice_states сервера guilds[i] (ники). */
        const cJSON *g, *merged = jget(d, "merged_members");
        int gi = 0;
        cJSON_ArrayForEach(g, jget(d, "guilds")) {
            u64 gid = jid(g, "id");
            const cJSON *members = cJSON_GetArrayItem(merged, gi++);
            const cJSON *v;
            cJSON_ArrayForEach(v, jget(g, "voice_states")) {
                u64 uid = jid(v, "user_id");
                const char *nick = NULL;
                const cJSON *m;
                cJSON_ArrayForEach(m, members) if (jid(m, "user_id") == uid) {
                    nick = jstr(m, "nick");
                    if (!nick && jget(m, "user")) nick = display_name(jget(m, "user"));
                    if (jget(m, "user") && jid(jget(m, "user"), "id") == uid) user_put_obj(jget(m, "user"));
                    break;
                }
                vs_set(gid, jid(v, "channel_id"), uid, nick ? nick : user_name(uid), jbool(v, "self_mute"),
                       jbool(v, "self_deaf"), jbool(v, "mute"), jbool(v, "deaf"));
                for (int q = 0; q < S.nvs; q++) if (S.vs[q].user_id == uid && S.vs[q].guild_id == gid) { S.vs[q].stream = jbool(v, "self_stream"); S.vs[q].video = jbool(v, "self_video"); }
            }
        }
        ui_dirty();
    } else if (!strcmp(t, "VOICE_STATE_UPDATE")) {
        vs_from_json(d, 0);
        u64 uid = jid(d, "user_id");
        if (uid == S.me_id) voice_on_state(jid(d, "guild_id"), jid(d, "channel_id"), uid, jstr(d, "session_id"));
        ui_dirty();
#ifdef ASTRA_VIDEO
    } else if (!strcmp(t, "STREAM_CREATE") || !strcmp(t, "STREAM_SERVER_UPDATE") || !strcmp(t, "STREAM_DELETE")) {
        golive_on_event(t, d);                  /* «Astra DS Video»: трансляция экрана */
#endif
    } else if (!strcmp(t, "VOICE_SERVER_UPDATE")) {
        u64 gid = jid(d, "guild_id");
        if (!gid) gid = jid(d, "channel_id");
        voice_on_server(gid, jstr(d, "token"), jstr(d, "endpoint"));
    } else if (!strcmp(t, "CALL_CREATE") || !strcmp(t, "CALL_UPDATE")) {
        u64 cid = jid(d, "channel_id");
        if (t[5] == 'C') call_created(cid);         /* исходящий: звонок создан — звоним собеседнику */
        ringing_note(cid, jget(d, "ringing"));      /* №138: кому звонят */
        bool ringing = false;
        const cJSON *r;
        cJSON_ArrayForEach(r, jget(d, "ringing")) if (id_of(r) == S.me_id) ringing = true;
        const cJSON *v;
        cJSON_ArrayForEach(v, jget(d, "voice_states")) vs_from_json(v, 0);
        if (ringing) {
            S.call.channel_id = cid;
            S.call.ringing = true;
            const cJSON *vs0 = cJSON_GetArrayItem(jget(d, "voice_states"), 0);
            S.call.caller = vs0 ? jid(vs0, "user_id") : 0;
            ui_notice(ui_tr("\a входящий звонок: F2 - принять, F3 - отклонить"));
        } else if (S.call.channel_id == cid) {
            S.call.ringing = false;
        }
        ui_dirty();
    } else if (!strcmp(t, "RELATIONSHIP_ADD") || !strcmp(t, "RELATIONSHIP_UPDATE")) {
        rel_from_json(d);
        ui_dirty();
    } else if (!strcmp(t, "RELATIONSHIP_REMOVE")) {
        u64 id = jid(d, "id");
        if (!id) id = jid(d, "user_id");
        rel_del(id);
        ui_dirty();
    } else if (!strcmp(t, "CHANNEL_PINS_UPDATE")) {
        pins_changed(jid(d, "channel_id"));
    } else if (!strcmp(t, "PRESENCE_UPDATE")) {
        /* сюда доходят только друзья (event_wanted); всё равно проверяем по настоящему JSON */
        if (presence_apply(d)) ui_dirty();
    } else if (!strcmp(t, "CALL_DELETE")) {
        u64 cid = jid(d, "channel_id");
        if (S.call.channel_id == cid) memset(&S.call, 0, sizeof S.call);
        if (rg_ch == cid) rg_n = 0;
        for (int i = S.nvs - 1; i >= 0; i--) if (!S.vs[i].guild_id && S.vs[i].channel_id == cid) { free(S.vs[i].name); S.vs[i] = S.vs[--S.nvs]; }
        ui_dirty();
    }
}


/* Обрезать строку до max байт по границе символа UTF-8 (на месте). */
static void utf8_cut(char *s, size_t max) {
    size_t n = strlen(s);
    if (n <= max) return;
    while (max && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    s[max] = 0;
}
/* ======================= Волна 2: серверы, каналы, приглашения ======================= */
static struct { u64 gid; bool loaded, busy; char *desc; } GD;           /* описание одного сервера */
static struct { u64 cid; bool loaded, busy; long failed; char *name, *topic; int slowmode, nsfw, bitrate, user_limit; } CI;   /* настройки одного канала */
/* Права на уровне сервера: @everyone + мои роли; владелец и ADMINISTRATOR — всё. */
static u64 guild_base_perms(const Guild *g) {
    if (!g) return 0;
    if (g->owner_id == S.me_id) return ~0ULL;
    u64 p = 0;
    for (int i = 0; i < g->nroles; i++) if (g->roles[i].id == g->id) p |= g->roles[i].perms;
    for (int k = 0; k < g->nmy; k++)
        for (int i = 0; i < g->nroles; i++) if (g->roles[i].id == g->my_roles[k]) p |= g->roles[i].perms;
    return (p & P_ADMIN) ? ~0ULL : p;
}
bool guild_is_owner(u64 gid) { Guild *g = guild_find(gid); return g && g->owner_id == S.me_id; }
bool can_manage_guild(u64 gid) { return (guild_base_perms(guild_find(gid)) & P_MANAGE_GUILD) != 0; }
bool can_manage_channels(u64 gid) { return (guild_base_perms(guild_find(gid)) & P_MANAGE_CHANNELS) != 0; }
bool can_manage_roles(u64 gid) { return (guild_base_perms(guild_find(gid)) & P_MANAGE_ROLES) != 0; }
bool can_create_invite(u64 cid) {
    Channel *c = chan_find(cid);
    if (!c || !c->guild_id || !c->visible) return false;
    u64 p = guild_base_perms(guild_find(c->guild_id));
    return (p & P_CREATE_INVITE) != 0;
}
static void on_op(Resp *r, void *ctx) {
    const char *what = ctx ? (const char *)ctx : ui_tr("запрос");
    if (r->status >= 200 && r->status < 300) { ui_notice(ui_tr("%s: готово"), what); ui_dirty(); return; }
    char e[160];
    if (r->status == 403) snprintf(e, sizeof e, ui_tr("%s: нет прав (403)"), what);
    else if (r->status == 400) snprintf(e, sizeof e, ui_tr("%s: Discord не принял данные (400)"), what);
    else http_err(e, sizeof e, what, r->status);
    ui_notice("%s", e);
}
/* Длина строки UTF-8 в символах (лимиты Discord — в символах). */
static size_t u8len(const char *s) { size_t n = 0; for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80; return n; }
/* Имя сервера/канала: 1..max символов после san(), без переводов строк. */
static bool name_ok(const char *n, size_t maxc, char *err, size_t errn) {
    if (!n || !*n) { snprintf(err, errn, ui_tr("название не может быть пустым")); return false; }
    if (u8len(n) > maxc) { snprintf(err, errn, ui_tr("название: не больше %zu символов"), maxc); return false; }
    char *t = san(n, false);
    bool ok = !strcmp(t, n);
    free(t);
    if (!ok) snprintf(err, errn, ui_tr("в названии недопустимые символы"));
    return ok;
}
/* Картинка с диска -> "data:image/...;base64,..." (в куче только на время запроса). "" -> null. */
static bool image_json(const char *path, Buf *out, char *err, size_t errn) {
    if (!*path) { buf_puts(out, "null"); return true; }
    FILE *f = fopen_local(path, err, errn);    /* только локальный файл (не UNC/сетевой диск) */
    if (!f) return false;
    unsigned char *d = xmalloc(ICON_MAX + 1);
    size_t n = fread(d, 1, ICON_MAX + 1, f);
    fclose(f);
    const char *mime = NULL;
    if (n >= 8 && !memcmp(d, "\x89PNG", 4)) mime = "image/png";
    else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) mime = "image/jpeg";
    else if (n >= 6 && !memcmp(d, "GIF8", 4)) mime = "image/gif";
    bool ok = false;
    if (n > ICON_MAX) snprintf(err, errn, ui_tr("картинка больше 256 КБ"));
    else if (!mime) snprintf(err, errn, ui_tr("картинка: нужен PNG, JPEG или GIF"));
    else {
        buf_printf(out, "\"data:%s;base64,", mime);
        base64_enc(d, n, out);
        buf_put(out, "\"", 1);
        ok = true;
    }
    free(d);
    return ok;
}
bool guild_create(const char *name, const char *icon_path, char *err, size_t errn) {
    if (!name_ok(name, 100, err, errn)) return false;
    Buf b = {0};
    buf_puts(&b, "{\"name\":");
    buf_json_str(&b, name);
    if (icon_path && *icon_path) {
        buf_puts(&b, ",\"icon\":");
        if (!image_json(icon_path, &b, err, errn)) { buf_free(&b); return false; }
    }
    buf_puts(&b, ",\"channels\":[],\"system_channel_id\":null}");
    if (O.demo) {
        u64 id = demo_new_id(), ch = demo_new_id();
        Buf g = {0};
        buf_printf(&g, "{\"id\":\"%llu\",\"name\":", (unsigned long long)id);
        buf_json_str(&g, name);
        buf_printf(&g, ",\"owner_id\":\"%llu\",\"roles\":[{\"id\":\"%llu\",\"permissions\":\"3263553\"}],"
                   "\"channels\":[{\"id\":\"%llu\",\"type\":0,\"name\":\"general\",\"position\":0}],\"members\":[]}",
                   (unsigned long long)S.me_id, (unsigned long long)id, (unsigned long long)ch);
        cJSON *j = json_parse(g.p, g.len);
        if (j) store_event("GUILD_CREATE", j);
        json_done();
        buf_free(&g);
        buf_free(&b);
        ui_notice(ui_tr("сервер создан (демо)"));
        return true;
    }
    http_async("POST", "/guilds", b.p, on_op, (void *)ui_tr("создание сервера"));
    buf_free(&b);
    return true;
}
bool guild_edit(u64 gid, const char *name, const char *description, const char *icon_path, const char *banner_path, char *err, size_t errn) {
    Guild *g = guild_find(gid);
    if (!g) { snprintf(err, errn, ui_tr("сервер не найден")); return false; }
    if (!can_manage_guild(gid)) { snprintf(err, errn, "нет права \"Управлять сервером\""); return false; }
    if (name && !name_ok(name, 100, err, errn)) return false;
    if (description && u8len(description) > 120) { snprintf(err, errn, ui_tr("описание: не больше 120 символов")); return false; }
    Buf b = {0};
    buf_puts(&b, "{");
    bool any = false;
    if (name && g->name && !strcmp(name, g->name)) name = NULL;          /* имя не менялось */
    if (name) { buf_puts(&b, "\"name\":"); buf_json_str(&b, name); any = true; }
    /* ревью 6 С2: описание не трогали (равно загруженному) — не шлём */
    if (description && GD.gid == gid && GD.loaded && !strcmp(description, GD.desc ? GD.desc : "")) description = NULL;
    if (name && g->name && !strcmp(name, g->name)) name = NULL;
    if (description) { buf_puts(&b, any ? ",\"description\":" : "\"description\":"); if (*description) buf_json_str(&b, description); else buf_puts(&b, "null"); any = true; }
    if (icon_path) { buf_puts(&b, any ? ",\"icon\":" : "\"icon\":"); if (!image_json(icon_path, &b, err, errn)) { buf_free(&b); return false; } any = true; }
    if (banner_path) { buf_puts(&b, any ? ",\"banner\":" : "\"banner\":"); if (!image_json(banner_path, &b, err, errn)) { buf_free(&b); return false; } any = true; }
    buf_puts(&b, "}");
    if (!any) { buf_free(&b); snprintf(err, errn, ui_tr("нечего менять")); return false; }
    if (O.demo) {
        if (name) { free(g->name); g->name = san(name, false); }
        if (description) { free(GD.desc); GD.desc = *description ? san(description, true) : NULL; GD.gid = gid; GD.loaded = true; }
        buf_free(&b);
        ui_notice(ui_tr("сервер изменён (демо)"));
        ui_dirty();
        return true;
    }
    char path[64];
    snprintf(path, sizeof path, "/guilds/%llu", (unsigned long long)gid);
    if (GD.gid == gid) GD.loaded = false;       /* описание перечитать при следующем показе */
    http_async("PATCH", path, b.p, on_op, (void *)ui_tr("изменение сервера"));
    buf_free(&b);
    return true;
}
static void guild_drop_local(u64 gid) {
    char b[64];
    snprintf(b, sizeof b, "{\"id\":\"%llu\"}", (unsigned long long)gid);
    cJSON *j = json_parse(b, strlen(b));
    if (j) store_event("GUILD_DELETE", j);
    json_done();
}
void guild_leave(u64 gid) {
    if (!guild_find(gid)) return;
    if (guild_is_owner(gid)) { ui_notice(ui_tr("владелец не может покинуть свой сервер (только удалить)")); return; }
    if (O.demo) { guild_drop_local(gid); ui_notice(ui_tr("вы покинули сервер (демо)")); return; }
    char path[64];
    snprintf(path, sizeof path, "/users/@me/guilds/%llu", (unsigned long long)gid);
    http_async("DELETE", path, "{\"lurking\":false}", on_op, (void *)ui_tr("выход с сервера"));
}
bool guild_delete(u64 gid, const char *typed_name, char *err, size_t errn) {
    Guild *g = guild_find(gid);
    if (!g) { snprintf(err, errn, ui_tr("сервер не найден")); return false; }
    if (!guild_is_owner(gid)) { snprintf(err, errn, ui_tr("удалить сервер может только владелец")); return false; }
    if (!typed_name || !g->name || strcmp(typed_name, g->name)) { snprintf(err, errn, ui_tr("для подтверждения введите название сервера точно")); return false; }
    if (O.demo) { guild_drop_local(gid); ui_notice(ui_tr("сервер удалён (демо)")); return true; }
    char path[64];
    snprintf(path, sizeof path, "/guilds/%llu", (unsigned long long)gid);
    http_async("DELETE", path, NULL, on_op, (void *)ui_tr("удаление сервера"));
    return true;
}

/* ---------- описание сервера и роли с именами: по запросу, один сервер ---------- */
static void on_guild_desc(Resp *r, void *ctx) {
    u64 gid = (u64)(uintptr_t)ctx;
    if (gid != GD.gid) return;
    GD.busy = false;
    GD.loaded = true;
    free(GD.desc);
    GD.desc = NULL;
    if (r->status == 200) {
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        const char *d = jstr(j, "description");
        if (d && *d) { GD.desc = san(d, true); utf8_cut(GD.desc, 1024); }
        json_done();
    }
    ui_dirty();
}
const char *guild_description(u64 gid) {
    if (GD.gid == gid && GD.loaded) return GD.desc ? GD.desc : "";
    if (GD.gid == gid && GD.busy) return NULL;
    free(GD.desc);
    GD.desc = NULL;
    GD.gid = gid;
    GD.loaded = false;
    if (O.demo) { GD.desc = xstrdup(gid == 2000 ? ui_tr("Уютный сервер для своих. Без токсичности.") : ""); GD.loaded = true; return GD.desc; }
    GD.busy = true;
    char path[48];
    snprintf(path, sizeof path, "/guilds/%llu", (unsigned long long)gid);
    http_async("GET", path, NULL, on_guild_desc, (void *)(uintptr_t)gid);
    return NULL;
}
static struct { u64 gid; bool loaded, busy; RoleName *r; int n; } RL;
static void roles_free(void) {
    for (int i = 0; i < RL.n; i++) free(RL.r[i].name);
    free(RL.r);
    RL.r = NULL;
    RL.n = 0;
}
static void roles_fill(const cJSON *arr) {
    roles_free();
    int n = cJSON_GetArraySize(arr);
    if (n > 250) n = 250;
    if (n > 0) RL.r = xcalloc((size_t)n, sizeof(RoleName));
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (RL.n >= n) break;
        const char *nm = jstr(o, "name");
        RL.r[RL.n].id = jid(o, "id");
        RL.r[RL.n].name = san(nm ? nm : "?", false);
        RL.r[RL.n].color = (u32)jint(o, "color", 0) & 0xFFFFFFu;
        RL.r[RL.n].pos = (int)jint(o, "position", 0);
        RL.n++;
    }
    RL.loaded = true;
}
static void on_roles(Resp *r, void *ctx) {
    if ((u64)(uintptr_t)ctx != RL.gid) return;
    RL.busy = false;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len); roles_fill(j); json_done(); }
    else RL.loaded = true;                      /* пусто: ошибка — ui покажет «ролей нет» */
    ui_dirty();
}
int guild_roles(u64 gid, const RoleName **out) {
    if (out) *out = RL.r;
    if (RL.gid == gid && RL.loaded) return RL.n;
    if (RL.gid == gid && RL.busy) return -1;
    roles_free();
    RL.gid = gid;
    RL.loaded = false;
    if (O.demo) {
        static const char R[] = "[{\"id\":\"2000\",\"name\":\"@everyone\",\"position\":0},{\"id\":\"2001\",\"name\":\"Админ\",\"color\":15548997,\"position\":3},"
                                "{\"id\":\"2002\",\"name\":\"Модератор\",\"color\":3447003,\"position\":1},{\"id\":\"2003\",\"name\":\"Участник\",\"color\":5763719,\"position\":2}]";
        cJSON *j = json_parse(R, sizeof R - 1);
        if (gid == 2000) roles_fill(j); else RL.loaded = true;
        json_done();
        if (out) *out = RL.r;
        return RL.n;
    }
    RL.busy = true;
    char path[64];
    snprintf(path, sizeof path, "/guilds/%llu/roles", (unsigned long long)gid);
    http_async("GET", path, NULL, on_roles, (void *)(uintptr_t)gid);
    return -1;
}
/* Ревью 6 М3: закрыли формы — кэши ролей, описания и настроек канала не держим. */
void forms_closed(void) {
    roles_free();
    RL.gid = 0; RL.loaded = RL.busy = false;
    free(GD.desc); GD.desc = NULL; GD.gid = 0; GD.loaded = GD.busy = false;
    free(CI.name); free(CI.topic); CI.name = CI.topic = NULL;
    CI.cid = 0; CI.loaded = CI.busy = false; CI.failed = 0;
}
const char *role_name(u64 gid, u64 rid) {
    if (RL.gid != gid) return NULL;
    for (int i = 0; i < RL.n; i++) if (RL.r[i].id == rid) return RL.r[i].name;
    return NULL;
}

/* ---------- каналы ---------- */
static bool chan_manageable(u64 cid, Channel **out, char *err, size_t errn) {
    Channel *c = chan_find(cid);
    if (!c || !c->guild_id) { if (err) snprintf(err, errn, ui_tr("канал не найден")); return false; }
    if (!can_manage_channels(c->guild_id)) { if (err) snprintf(err, errn, "нет права \"Управлять каналами\""); return false; }
    *out = c;
    return true;
}
bool chan_create(u64 gid, int kind, const char *name, u64 parent_id, char *err, size_t errn) {
    if (!guild_find(gid)) { snprintf(err, errn, ui_tr("сервер не найден")); return false; }
    if (!can_manage_channels(gid)) { snprintf(err, errn, "нет права \"Управлять каналами\""); return false; }
    if (kind != CHK_TEXT && kind != CHK_VOICE && kind != CHK_CATEGORY) { snprintf(err, errn, ui_tr("неизвестный тип канала")); return false; }
    if (!name_ok(name, 100, err, errn)) return false;
    Channel *p = parent_id ? chan_find(parent_id) : NULL;
    if (parent_id && (!p || p->type != CH_CATEGORY || p->guild_id != gid || kind == CHK_CATEGORY)) { snprintf(err, errn, ui_tr("родитель - только категория этого сервера")); return false; }
    Buf b = {0};
    buf_puts(&b, "{\"name\":");
    buf_json_str(&b, name);
    buf_printf(&b, ",\"type\":%d", kind);
    if (parent_id) buf_printf(&b, ",\"parent_id\":\"%llu\"", (unsigned long long)parent_id);
    buf_puts(&b, "}");
    if (O.demo) {
        Buf c = {0};
        buf_printf(&c, "{\"id\":\"%llu\",\"guild_id\":\"%llu\",\"type\":%d,\"name\":", (unsigned long long)demo_new_id(), (unsigned long long)gid, kind);
        buf_json_str(&c, name);
        if (parent_id) buf_printf(&c, ",\"parent_id\":\"%llu\"", (unsigned long long)parent_id);
        buf_puts(&c, ",\"position\":99}");
        cJSON *j = json_parse(c.p, c.len);
        if (j) store_event("CHANNEL_CREATE", j);
        json_done();
        buf_free(&c);
        buf_free(&b);
        ui_notice(ui_tr("канал создан (демо)"));
        return true;
    }
    char path[64];
    snprintf(path, sizeof path, "/guilds/%llu/channels", (unsigned long long)gid);
    http_async("POST", path, b.p, on_op, (void *)ui_tr("создание канала"));
    buf_free(&b);
    return true;
}
bool chan_edit(u64 cid, const ChanEdit *e, char *err, size_t errn) {
    Channel *c;
    if (!e || !chan_manageable(cid, &c, err, errn)) return false;
    if (e->name && !name_ok(e->name, 100, err, errn)) return false;
    if (e->topic && u8len(e->topic) > 1024) { snprintf(err, errn, ui_tr("тема: не больше 1024 символов")); return false; }
    if (e->slowmode > 21600) { snprintf(err, errn, ui_tr("медленный режим: не больше 6 часов")); return false; }
    if (e->user_limit > 99) { snprintf(err, errn, ui_tr("лимит участников: 0..99")); return false; }
    /* ревью 6 С1: только изменённые поля — если исходные значения загружены (chan_info), равные им не шлём */
    bool known = CI.cid == cid && CI.loaded;
    const char *nm = e->name && !(known && !strcmp(e->name, CI.name)) ? e->name : NULL;
    const char *tp = e->topic && !(known && !strcmp(e->topic, CI.topic)) ? e->topic : NULL;
    int sm = e->slowmode >= 0 && !(known && e->slowmode == CI.slowmode) ? e->slowmode : -1;
    int nf = e->nsfw >= 0 && !(known && !!e->nsfw == !!CI.nsfw) ? e->nsfw : -1;
    int br = e->bitrate >= 0 && !(known && e->bitrate == CI.bitrate) ? e->bitrate : -1;
    int ul = e->user_limit >= 0 && !(known && e->user_limit == CI.user_limit) ? e->user_limit : -1;
    Buf b = {0};
    buf_puts(&b, "{");
    int n = 0;
#define SEP() buf_puts(&b, n++ ? "," : "")
    if (nm) { SEP(); buf_puts(&b, "\"name\":"); buf_json_str(&b, nm); }
    if (tp) { SEP(); buf_puts(&b, "\"topic\":"); buf_json_str(&b, tp); }
    if (sm >= 0) { SEP(); buf_printf(&b, "\"rate_limit_per_user\":%d", sm); }
    if (nf >= 0) { SEP(); buf_printf(&b, "\"nsfw\":%s", nf ? "true" : "false"); }
    if (br >= 0) { SEP(); buf_printf(&b, "\"bitrate\":%d", br); }
    if (ul >= 0) { SEP(); buf_printf(&b, "\"user_limit\":%d", ul); }
#undef SEP
    buf_puts(&b, "}");
    if (!n) { buf_free(&b); snprintf(err, errn, ui_tr("нечего менять")); return false; }
    if (!n) { buf_free(&b); snprintf(err, errn, ui_tr("ничего не изменено")); return false; }
    CI.loaded = false;                          /* значения перечитать при следующем показе */
    if (O.demo) {
        if (nm) { free(c->name); c->name = san(nm, false); }
        if (tp && cid == topic_ch) { free(c->topic); c->topic = topic_copy(tp); }
        buf_free(&b);
        ui_notice(ui_tr("канал изменён (демо)"));
        ui_dirty();
        return true;
    }
    char path[48];
    snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)cid);
    http_async("PATCH", path, b.p, on_op, (void *)ui_tr("изменение канала"));
    buf_free(&b);
    return true;
}
static void ci_fill(const cJSON *o) {
    free(CI.name); free(CI.topic);
    const char *n = jstr(o, "name"), *t = jstr(o, "topic");
    CI.name = san(n ? n : "", false);
    CI.topic = san(t ? t : "", true);
    CI.slowmode = (int)jint(o, "rate_limit_per_user", 0);
    CI.nsfw = jbool(o, "nsfw");
    CI.bitrate = (int)jint(o, "bitrate", 0);
    CI.user_limit = (int)jint(o, "user_limit", 0);
    CI.loaded = true;
}
static void on_chan_info(Resp *r, void *ctx) {
    if ((u64)(uintptr_t)ctx != CI.cid) return;
    CI.busy = false;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len); ci_fill(j); json_done(); }
    else { ui_notice(ui_tr("канал: настройки не загрузились (%ld)"), r->status); CI.failed = r->status ? r->status : -1; }   /* не повторять в каждом кадре */
    ui_dirty();
}
long chan_info_failed(u64 cid) { return CI.cid == cid ? CI.failed : 0; }
void chan_info_retry(void) { CI.failed = 0; CI.loaded = false; CI.busy = false; CI.cid = 0; }
bool chan_info(u64 cid, ChanEdit *out) {
    Channel *c = chan_find(cid);
    if (!c || !out) return false;
    if (CI.cid == cid && CI.failed) return false;   /* ошибка запомнена: повтор — chan_info_retry() */
    if (CI.cid != cid || (!CI.loaded && !CI.busy)) {
        CI.cid = cid;
        CI.loaded = false;
        CI.failed = 0;
        if (O.demo) {
            char b[200];
            snprintf(b, sizeof b, "{\"rate_limit_per_user\":%d,\"nsfw\":false,\"bitrate\":64000,\"user_limit\":0}", cid == 3005 ? 10 : 0);
            cJSON *j = json_parse(b, strlen(b));
            ci_fill(j);
            json_done();
            free(CI.name); CI.name = xstrdup(c->name ? c->name : "");
            free(CI.topic); CI.topic = xstrdup(c->topic ? c->topic : "");
        } else {
            CI.busy = true;
            char path[48];
            snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)cid);
            http_async("GET", path, NULL, on_chan_info, (void *)(uintptr_t)cid);
            return false;
        }
    }
    if (!CI.loaded) return false;
    *out = (ChanEdit){ CI.name, CI.topic, CI.slowmode, CI.nsfw, CI.bitrate, CI.user_limit };
    return true;
}
void chan_delete(u64 cid) {
    Channel *c;
    char err[96];
    if (!chan_manageable(cid, &c, err, sizeof err)) { ui_notice("%s", err); return; }
    if (O.demo) {
        char b[64];
        snprintf(b, sizeof b, "{\"id\":\"%llu\"}", (unsigned long long)cid);
        cJSON *j = json_parse(b, strlen(b));
        if (j) store_event("CHANNEL_DELETE", j);
        json_done();
        ui_notice(ui_tr("канал удалён (демо)"));
        return;
    }
    char path[48];
    snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)cid);
    http_async("DELETE", path, NULL, on_op, (void *)ui_tr("удаление канала"));
}
/* Группа сортировки Discord: категории, голосовые, остальные (текстовые) — позиции сравниваются внутри группы. */
static int chan_group(int t) { return t == CH_CATEGORY ? 2 : (t == CH_VOICE || t == CH_STAGE) ? 1 : 0; }
static int sib_cmp(const void *a, const void *b) {
    const Channel *x = *(Channel *const *)a, *y = *(Channel *const *)b;
    if (x->position != y->position) return x->position < y->position ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}
/* Поменять канал местами с соседом (dir -1 — выше, +1 — ниже) среди каналов того же родителя и группы.
 * Позиции нормализуются (0..n-1), уходит массив {id, position} всех соседей — без дубликатов позиций. */
void chan_swap(u64 cid, int dir) {
    Channel *c;
    char err[96];
    if (!chan_manageable(cid, &c, err, sizeof err)) { ui_notice("%s", err); return; }
    Channel *sib[512];
    int n = 0, me = -1;
    for (int i = 0; i < S.nchans && n < 512; i++) {
        Channel *x = &S.chans[i];
        if (x->guild_id == c->guild_id && x->parent_id == c->parent_id && chan_group(x->type) == chan_group(c->type)) sib[n++] = x;
    }
    qsort(sib, (size_t)n, sizeof *sib, sib_cmp);
    for (int i = 0; i < n; i++) if (sib[i] == c) me = i;
    /* сосед — ближайший ВИДИМЫЙ (скрытые от вас каналы не меняем местами, но позиции нормализуем всем) */
    int step = dir < 0 ? -1 : 1, other = me + step;
    while (other >= 0 && other < n && !sib[other]->visible) other += step;
    if (me < 0 || other < 0 || other >= n) return;  /* уже первый/последний среди видимых */
    Channel *t = sib[me]; sib[me] = sib[other]; sib[other] = t;
    for (int i = 0; i < n; i++) sib[i]->position = i;   /* модель — сразу (CHANNEL_UPDATE подтвердит) */
    ui_dirty();
    if (O.demo) return;
    Buf b = {0};
    buf_put(&b, "[", 1);
    for (int i = 0; i < n; i++) buf_printf(&b, "%s{\"id\":\"%llu\",\"position\":%d}", i ? "," : "", (unsigned long long)sib[i]->id, i);
    buf_put(&b, "]", 1);
    char path[48];
    snprintf(path, sizeof path, "/guilds/%llu/channels", (unsigned long long)c->guild_id);
    http_async("PATCH", path, b.p, on_op, (void *)ui_tr("перестановка каналов"));
    buf_free(&b);
}
void chan_move(u64 cid, u64 new_parent, int new_position) {
    Channel *c;
    char err[96];
    if (!chan_manageable(cid, &c, err, sizeof err)) { ui_notice("%s", err); return; }
    /* тот же родитель и сдвиг на ±1 — это перестановка с соседом (как Alt+Up/Down) */
    if (new_parent == c->parent_id && (new_position == c->position - 1 || new_position == c->position + 1)) {
        chan_swap(cid, new_position < c->position ? -1 : 1);
        return;
    }
    if (new_position < 0) new_position = 0;
    Channel *p = new_parent ? chan_find(new_parent) : NULL;
    if (new_parent && (!p || p->type != CH_CATEGORY || p->guild_id != c->guild_id || c->type == CH_CATEGORY)) { ui_notice(ui_tr("переместить можно только в категорию этого сервера")); return; }
    if (O.demo) { c->position = new_position; c->parent_id = new_parent; ui_dirty(); return; }
    char path[48], body[160];
    snprintf(path, sizeof path, "/guilds/%llu/channels", (unsigned long long)c->guild_id);
    if (c->type == CH_CATEGORY) snprintf(body, sizeof body, "[{\"id\":\"%llu\",\"position\":%d}]", (unsigned long long)cid, new_position);
    else if (new_parent) snprintf(body, sizeof body, "[{\"id\":\"%llu\",\"position\":%d,\"parent_id\":\"%llu\",\"lock_permissions\":false}]",
                                  (unsigned long long)cid, new_position, (unsigned long long)new_parent);
    else snprintf(body, sizeof body, "[{\"id\":\"%llu\",\"position\":%d,\"parent_id\":null}]", (unsigned long long)cid, new_position);
    http_async("PATCH", path, body, on_op, (void *)ui_tr("перемещение канала"));
}
void chan_perm_set(u64 cid, u64 target, int type, u64 allow, u64 deny) {
    Channel *c;
    char err[96];
    if (!chan_manageable(cid, &c, err, sizeof err) || !can_manage_roles(c->guild_id)) { ui_notice(ui_tr("нет прав менять права канала")); return; }
    if (!target || (type != OW_ROLE && type != OW_MEMBER)) return;
    allow &= ~deny;                             /* один бит не может быть и разрешён, и запрещён */
    if (O.demo) {
        int i;
        for (i = 0; i < c->now_ && c->ow[i].id != target; i++) {}
        if (i == c->now_) { c->ow = xrealloc(c->ow, (size_t)(c->now_ + 1) * sizeof(Overwrite)); c->now_++; }
        c->ow[i] = (Overwrite){ target, allow, deny, (u8)type };
        compute_perms(c);
        ui_dirty();
        return;
    }
    char path[96], body[128];
    snprintf(path, sizeof path, "/channels/%llu/permissions/%llu", (unsigned long long)cid, (unsigned long long)target);
    snprintf(body, sizeof body, "{\"type\":%d,\"allow\":\"%llu\",\"deny\":\"%llu\"}", type, (unsigned long long)allow, (unsigned long long)deny);
    http_async("PUT", path, body, on_op, (void *)ui_tr("права канала"));
}
/* Исходная перезапись канала для target (из модели): false — нет перезаписи. */
bool chan_overwrite(u64 cid, u64 target, u64 *allow, u64 *deny, int *type) {
    Channel *c = chan_find(cid);
    if (!c) return false;
    for (int i = 0; i < c->now_; i++) if (c->ow[i].id == target) {
        if (allow) *allow = c->ow[i].allow;
        if (deny) *deny = c->ow[i].deny;
        if (type) *type = c->ow[i].type;
        return true;
    }
    return false;
}
/* Ревью 6 С3: менять только биты mask (биты формы), остальные — как в исходной перезаписи. Пусто — удалить. */
void chan_perm_edit(u64 cid, u64 target, int type, u64 mask, u64 allow, u64 deny) {
    u64 a0 = 0, d0 = 0;
    int t0 = type;
    chan_overwrite(cid, target, &a0, &d0, &t0);
    u64 a = (a0 & ~mask) | (allow & mask), d = (d0 & ~mask) | (deny & mask);
    if (!a && !d) chan_perm_clear(cid, target);
    else chan_perm_set(cid, target, type, a, d);
}
void chan_perm_clear(u64 cid, u64 target) {
    Channel *c;
    char err[96];
    if (!chan_manageable(cid, &c, err, sizeof err) || !can_manage_roles(c->guild_id)) { ui_notice(ui_tr("нет прав менять права канала")); return; }
    if (O.demo) {
        for (int i = 0; i < c->now_; i++) if (c->ow[i].id == target) { c->ow[i] = c->ow[--c->now_]; break; }
        compute_perms(c);
        ui_dirty();
        return;
    }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/permissions/%llu", (unsigned long long)cid, (unsigned long long)target);
    http_async("DELETE", path, NULL, on_op, (void *)ui_tr("права канала"));
}

/* ---------- приглашения ---------- */
static bool invite_code_ok(const char *c, size_t n) {
    if (n < 2 || n > 32) return false;
    for (size_t i = 0; i < n; i++) if (!((c[i] >= 'a' && c[i] <= 'z') || (c[i] >= 'A' && c[i] <= 'Z') || (c[i] >= '0' && c[i] <= '9') || c[i] == '-')) return false;
    return true;
}
bool invite_parse(const char *text, char *code, size_t n) {
    if (!text || !code || n < 2) return false;
    code[0] = 0;
    while (*text == ' ' || *text == '\t' || *text == '<') text++;
    static const char *const PRE[] = { "https://", "http://" };
    for (size_t i = 0; i < 2; i++) if (!strncmp(text, PRE[i], strlen(PRE[i]))) { text += strlen(PRE[i]); break; }
    if (!strncmp(text, "www.", 4)) text += 4;
    static const char *const HOST[] = { "discord.gg/", "discord.com/invite/", "discordapp.com/invite/", "ptb.discord.com/invite/", "canary.discord.com/invite/" };
    for (size_t i = 0; i < sizeof HOST / sizeof *HOST; i++) if (!strncmp(text, HOST[i], strlen(HOST[i]))) { text += strlen(HOST[i]); break; }
    size_t len = strcspn(text, "?#/> \t\r\n");
    if (!invite_code_ok(text, len) || len >= n) return false;
    memcpy(code, text, len);
    code[len] = 0;
    return true;
}
static InviteState IV;
static bool iv_risk_ack;
static i64 iv_last_join;
static u64 iv_channel_id;                       /* канал приглашения — для X-Context-Properties */
static int iv_channel_type;
static char iv_gname[104], iv_cname[104], iv_inviter[72], iv_link[64];
static unsigned iv_gen;
const InviteState *invite_state(void) { return &IV; }
const char *invite_created(void) { return iv_link[0] ? iv_link : NULL; }
void invite_close(void) { memset(&IV, 0, sizeof IV); iv_gname[0] = iv_cname[0] = iv_inviter[0] = 0; iv_gen++; }
/* --selftest: сброс паузы и согласия (между проверками) */
void invite_test_reset(void) { iv_last_join = 0; iv_risk_ack = false; }
static void iv_copy(char *dst, size_t n, const char *s) {
    char *t = san(s ? s : "", false);
    size_t l = strlen(t);
    if (l >= n) { l = n - 1; while (l && ((unsigned char)t[l] & 0xC0) == 0x80) l--; }
    memcpy(dst, t, l);
    dst[l] = 0;
    free(t);
}
void invite_apply(const cJSON *j) {
    const cJSON *g = jget(j, "guild");
    IV.busy = false;
    IV.guild_id = jid(g, "id");
    iv_copy(iv_gname, sizeof iv_gname, jstr(g, "name"));
    iv_copy(iv_cname, sizeof iv_cname, jstr(jget(j, "channel"), "name"));
    iv_channel_id = jid(jget(j, "channel"), "id");
    iv_channel_type = (int)jint(jget(j, "channel"), "type", 0);
    const cJSON *inv = jget(j, "inviter");
    iv_copy(iv_inviter, sizeof iv_inviter, inv ? display_name(inv) : "");
    hash_copy(IV.icon, jstr(g, "icon"));
    IV.members = (int)jint(j, "approximate_member_count", 0);
    IV.online = (int)jint(j, "approximate_presence_count", 0);
    IV.guild_name = iv_gname;
    IV.channel_name = iv_cname;
    IV.inviter = iv_inviter;
    IV.joined = IV.guild_id && guild_find(IV.guild_id) != NULL;
    if (!IV.guild_id) snprintf(IV.error, sizeof IV.error, ui_tr("приглашение не на сервер (группа?) - не поддерживается"));
    ui_dirty();
}
static void on_invite(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != iv_gen) return;
    IV.busy = false;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len); invite_apply(j); json_done(); return; }
    if (r->status == 404 || r->status == 400) snprintf(IV.error, sizeof IV.error, ui_tr("приглашение недействительно или истекло"));
    else http_err(IV.error, sizeof IV.error, ui_tr("приглашение"), r->status);
    ui_dirty();
}
static char iv_input[200];                      /* что ввёл пользователь (для inputValue) */
void invite_open(const char *code_or_url) {
    invite_close();
    snprintf(iv_input, sizeof iv_input, "%s", code_or_url ? code_or_url : "");
    if (!invite_parse(code_or_url, IV.code, sizeof IV.code)) { snprintf(IV.error, sizeof IV.error, ui_tr("это не ссылка-приглашение Discord")); ui_dirty(); return; }
    if (O.demo) {
        if (!strcmp(IV.code, "xyz")) snprintf(IV.error, sizeof IV.error, ui_tr("приглашение недействительно или истекло"));
        else if (!strcmp(IV.code, "429")) snprintf(IV.error, sizeof IV.error, ui_tr("приглашение: слишком часто, попробуйте позже"));
        else {
            static const char D[] = "{\"code\":\"abc\",\"guild\":{\"id\":\"2300\",\"name\":\"Котики 2\",\"icon\":\"7123456789abcdef0123456789abcdef\"},"
                "\"channel\":{\"name\":\"общий\"},\"inviter\":{\"id\":\"1001\",\"global_name\":\"Алиса\"},\"approximate_member_count\":1234,\"approximate_presence_count\":321}";
            cJSON *j = json_parse(D, sizeof D - 1);
            invite_apply(j);
            json_done();
        }
        ui_dirty();
        return;
    }
    IV.busy = true;
    char path[400];
    /* как веб-клиент: inputValue — то, что ввёл пользователь (ссылка), + счётчики, срок, права */
    Buf pb = {0};
    buf_printf(&pb, "/invites/%s?inputValue=", IV.code);
    url_encode(iv_input, &pb);
    buf_puts(&pb, "&with_counts=true&with_expiration=true&with_permissions=true");
    snprintf(path, sizeof path, "%s", pb.p);
    buf_free(&pb);
    http_async("GET", path, NULL, on_invite, (void *)(uintptr_t)iv_gen);
    ui_dirty();
}
static void on_join(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != iv_gen) return;
    IV.busy = false;
    if (r->status == 200) { IV.joined = true; ui_notice("вы вступили на сервер \"%s\"", iv_gname); }
    else if ((r->status == 400 && r->body.p && strstr(r->body.p, "captcha")) || r->status == 403) {
        IV.need_browser = true;                     /* капча или отказ: вступить через браузер */
        snprintf(IV.error, sizeof IV.error, ui_tr("Discord требует вступить через браузер"));
    }
    else {
        /* причина от Discord (code + message): 403 бывает и «нужна подтверждённая почта/телефон», и «вас не пускают» */
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        const char *m = jstr(j, "message");
        long code = (long)jint(j, "code", 0);
        (void)m;
        if (r->status == 403 || r->status == 401)
            snprintf(IV.error, sizeof IV.error, ui_tr("Discord отклонил вступление (код %ld) - вступите из официального приложения, затем сервер будет доступен в Astra DS"), code ? code : r->status);
        else http_err(IV.error, sizeof IV.error, ui_tr("вступление"), r->status);
        utf8_cut(IV.error, sizeof IV.error - 1);
        json_done();
    }
    ui_dirty();
}
/* Только по явному подтверждению пользователя: ни повторов, ни автоматики. */
/* Как в larperCord: вступление из программы — только без капчи; капча или отказ после неё — человек вступает
 * в браузере (https://discord.gg/КОД), сервер приходит событием GUILD_CREATE. Пауза между вступлениями,
 * разовое предупреждение о риске (частые вступления из стороннего клиента Discord может наказать). */
const char *invite_risk_text(void) {
    return ui_tr("Вступление из стороннего клиента: Discord может попросить капчу или отказать, а частые вступления - "
           "ограничить аккаунт. Если Discord попросит проверку, Astra DS предложит вступить через браузер.");
}
void invite_risk_ack(void) { iv_risk_ack = true; IV.risk_warning = false; ui_dirty(); }
bool invite_open_browser(void) {
    if (!invite_code_ok(IV.code, strlen(IV.code))) return false;
    char url[64];
    snprintf(url, sizeof url, "https://discord.gg/%s", IV.code);
    if (O.demo) { ui_notice(ui_tr("демо: открыл бы %s в браузере"), url); return true; }
    bool ok = open_url_browser(url);
    ui_notice(ok ? ui_tr("открыто в браузере: вступите там - сервер появится в Astra DS сам") : ui_tr("браузер не открылся: откройте %s вручную"), url);
    return ok;
}
void invite_join(void) {
    if (!IV.code[0] || !IV.guild_id || IV.busy || IV.joined || IV.error[0]) return;
    if (!iv_risk_ack) { IV.risk_warning = true; ui_dirty(); return; }   /* сначала — разовое предупреждение */
    i64 now = now_ms();
    if (iv_last_join && now - iv_last_join < INVITE_GAP_MS) {
        snprintf(IV.error, sizeof IV.error, ui_tr("вступать можно не чаще раза в 30 с - подождите %lld с"), (long long)((INVITE_GAP_MS - (now - iv_last_join) + 999) / 1000));
        ui_dirty();
        return;
    }
    iv_last_join = now;
    if (O.demo && !strcmp(IV.code, "captcha")) {    /* демо: Discord «просит капчу» — путь через браузер */
        IV.need_browser = true;
        snprintf(IV.error, sizeof IV.error, ui_tr("Discord требует вступить через браузер"));
        ui_dirty();
        return;
    }
    if (O.demo) {
        char b[200];
        snprintf(b, sizeof b, "{\"id\":\"%llu\",\"name\":", (unsigned long long)IV.guild_id);
        Buf g = {0};
        buf_puts(&g, b);
        buf_json_str(&g, iv_gname);
        buf_printf(&g, ",\"owner_id\":\"1001\",\"roles\":[{\"id\":\"%llu\",\"permissions\":\"3263553\"}],\"channels\":[{\"id\":\"%llu\",\"type\":0,\"name\":\"общий\",\"position\":0}],"
                   "\"members\":[{\"user\":{\"id\":\"%llu\"},\"roles\":[]}]}", (unsigned long long)IV.guild_id, (unsigned long long)demo_new_id(), (unsigned long long)S.me_id);
        cJSON *j = json_parse(g.p, g.len);
        if (j) store_event("GUILD_CREATE", j);
        json_done();
        buf_free(&g);
        IV.joined = true;
        ui_notice("вы вступили на сервер \"%s\" (демо)", iv_gname);
        return;
    }
    IV.busy = true;
    char path[64];
    snprintf(path, sizeof path, "/invites/%s", IV.code);
    Buf b = {0};
    buf_puts(&b, "{\"session_id\":");
    buf_json_str(&b, S.session_id ? S.session_id : "");
    buf_puts(&b, "}");
    /* как официальный клиент: X-Context-Properties = base64({"location":"Join Guild", сервер, канал, тип канала}) */
    char ctxj[200];
    snprintf(ctxj, sizeof ctxj, "{\"location\":\"Join Guild\",\"location_guild_id\":\"%llu\",\"location_channel_id\":\"%llu\",\"location_channel_type\":%d}",
             (unsigned long long)IV.guild_id, (unsigned long long)iv_channel_id, iv_channel_type);
    Buf h = {0};
    buf_puts(&h, "X-Context-Properties: ");
    base64_enc(ctxj, strlen(ctxj), &h);
    http_async_hdr("POST", path, b.p, h.p, on_join, (void *)(uintptr_t)iv_gen);
    buf_free(&h);
    buf_free(&b);
    ui_dirty();
}
static void on_inv_create(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status == 200) {
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        const char *c = jstr(j, "code");
        if (c && invite_code_ok(c, strlen(c))) { snprintf(iv_link, sizeof iv_link, "https://discord.gg/%s", c); ui_notice(ui_tr("приглашение: %s"), iv_link); }
        json_done();
    } else { char e[160]; http_err(e, sizeof e, ui_tr("приглашение"), r->status); ui_notice("%s", e); }
    ui_dirty();
}
void invite_create(u64 cid, int max_age_s, int max_uses) {
    iv_link[0] = 0;
    if (!can_create_invite(cid)) { ui_notice(ui_tr("нет права создавать приглашения в этом канале")); return; }
    if (max_age_s < 0) max_age_s = 0;
    if (max_age_s > 604800) max_age_s = 604800;
    if (max_uses < 0) max_uses = 0;
    if (max_uses > 100) max_uses = 100;
    if (O.demo) { snprintf(iv_link, sizeof iv_link, "https://discord.gg/demo%llu", (unsigned long long)(cid % 1000)); ui_notice(ui_tr("приглашение: %s (демо)"), iv_link); ui_dirty(); return; }
    char path[64], body[96];
    snprintf(path, sizeof path, "/channels/%llu/invites", (unsigned long long)cid);
    snprintf(body, sizeof body, "{\"max_age\":%d,\"max_uses\":%d,\"temporary\":false}", max_age_s, max_uses);
    http_async("POST", path, body, on_inv_create, NULL);
}


/* ======================= Пакет А ======================= */
/* ---------- заглушить и уровень уведомлений (PATCH /users/@me/guilds/{g|@me}/settings) ---------- */
/* Ревью 9 М5: модель меняется сразу; при ошибке ответа — откат к прежнему значению (кольцо записей). */
enum { SR_GMUTE, SR_CMUTE, SR_GNOTIFY, SR_CNOTIFY };
#define STR_MAX 8
static struct { u64 id; u8 kind, old; const char *what; unsigned gen; } STR[STR_MAX];
static unsigned sr_gen;
static void on_settings(Resp *r, void *ctx) {
    unsigned g = (unsigned)(uintptr_t)ctx;
    int i = (int)(g % STR_MAX);
    if (!g || STR[i].gen != g) return;           /* запись уже занята более новым изменением */
    STR[i].gen = 0;
    if (r->status >= 200 && r->status < 300) return;
    Guild *gu = STR[i].kind == SR_GMUTE || STR[i].kind == SR_GNOTIFY ? guild_find(STR[i].id) : NULL;
    Channel *c = STR[i].kind == SR_CMUTE || STR[i].kind == SR_CNOTIFY ? chan_find(STR[i].id) : NULL;
    if (gu && STR[i].kind == SR_GMUTE) gu->muted = STR[i].old;
    if (gu && STR[i].kind == SR_GNOTIFY) gu->notify = STR[i].old;
    if (c && STR[i].kind == SR_CMUTE) c->muted = STR[i].old;
    if (c && STR[i].kind == SR_CNOTIFY) c->notify = STR[i].old;
    char e[160];
    http_err(e, sizeof e, STR[i].what, r->status);
    ui_notice("%s", e);
    ui_dirty();
}
static void settings_patch(u64 gid, const char *body, const char *what, u8 kind, u64 id, u8 old) {
    if (O.demo) { ui_dirty(); return; }
    char path[80];
    if (gid) snprintf(path, sizeof path, "/users/@me/guilds/%llu/settings", (unsigned long long)gid);
    else snprintf(path, sizeof path, "/users/@me/guilds/@me/settings");
    unsigned g = ++sr_gen;
    if (!g) g = ++sr_gen;
    int i = (int)(g % STR_MAX);
    STR[i].id = id; STR[i].kind = kind; STR[i].old = old; STR[i].what = what; STR[i].gen = g;
    http_async("PATCH", path, body, on_settings, (void *)(uintptr_t)g);
}
void mute_guild(u64 gid, bool mute) {
    Guild *g = guild_find(gid);
    if (!g) return;
    u8 old = g->muted;
    g->muted = mute;                            /* сразу в модели; USER_GUILD_SETTINGS_UPDATE подтвердит */
    settings_patch(gid, mute ? "{\"muted\":true,\"mute_config\":null}" : "{\"muted\":false}", mute ? ui_tr("заглушить сервер") : ui_tr("включить сервер"),
                   SR_GMUTE, gid, old);
    ui_dirty();
}
void mute_channel(u64 cid, bool mute) {
    Channel *c = chan_find(cid);
    if (!c) return;
    u8 old = c->muted;
    c->muted = mute;
    char b[160];
    snprintf(b, sizeof b, "{\"channel_overrides\":{\"%llu\":{\"muted\":%s%s}}}", (unsigned long long)cid,
             mute ? "true" : "false", mute ? ",\"mute_config\":null" : "");
    settings_patch(c->guild_id, b, mute ? ui_tr("заглушить канал") : ui_tr("включить канал"), SR_CMUTE, cid, old);
    ui_dirty();
}
void notify_level(u64 gid, u64 cid, int level) {
    if (level < NOTIFY_ALL || level > NOTIFY_DEFAULT) return;
    char b[160];
    if (cid) {
        Channel *c = chan_find(cid);
        if (!c) return;
        u8 old = c->notify;
        c->notify = (u8)level;
        snprintf(b, sizeof b, "{\"channel_overrides\":{\"%llu\":{\"message_notifications\":%d}}}", (unsigned long long)cid, level);
        settings_patch(c->guild_id, b, ui_tr("уведомления канала"), SR_CNOTIFY, cid, old);
    } else {
        Guild *g = guild_find(gid);
        if (!g || level == NOTIFY_DEFAULT) return;
        u8 old = g->notify;
        g->notify = (u8)level;
        snprintf(b, sizeof b, "{\"message_notifications\":%d}", level);
        settings_patch(gid, b, ui_tr("уведомления сервера"), SR_GNOTIFY, gid, old);
    }
    ui_dirty();
}
/* итоговый уровень канала: свой, иначе сервера (по умолчанию — только упоминания) */
static int notify_of(const Channel *c) {
    if (c->notify != NOTIFY_DEFAULT) return c->notify;
    Guild *g = c->guild_id ? guild_find(c->guild_id) : NULL;
    return g && g->notify != NOTIFY_DEFAULT ? g->notify : NOTIFY_MENTIONS;
}

/* ---------- №127: медленный режим и тайм-аут ---------- */
#define SLOW_MAX 8
static struct { u64 cid; i64 until; } SLW[SLOW_MAX];   /* до какого момента (unix-с) нельзя писать в канал */
static int slw_rr;
static bool slow_wake_dirty;                    /* при окончании отсчёта — перерисовать */
static void slow_set(u64 cid, i64 until) {
    int k = -1;
    for (int i = 0; i < SLOW_MAX; i++) if (SLW[i].cid == cid) k = i;
    if (k < 0) for (int i = 0; i < SLOW_MAX && k < 0; i++) if (!SLW[i].cid || SLW[i].until <= wall_ms() / 1000) k = i;
    if (k < 0) k = slw_rr++ % SLOW_MAX;
    SLW[k].cid = cid; SLW[k].until = until;
    slow_wake_dirty = true;
    ui_dirty();
}
void send_limit(u64 cid, SendLimit *out) {
    memset(out, 0, sizeof *out);
    Channel *c = chan_find(cid);
    if (!c) return;
    i64 now = wall_ms() / 1000;
    out->slowmode = c->guild_id && !c->slow_exempt ? c->slowmode : 0;
    for (int i = 0; i < SLOW_MAX; i++) if (SLW[i].cid == cid && SLW[i].until > now) out->slow_until = SLW[i].until;
    Guild *g = c->guild_id ? guild_find(c->guild_id) : NULL;
    if (g && g->timeout_until > now && !(guild_base_perms(g) & P_ADMIN) && g->owner_id != S.me_id) out->timeout_until = g->timeout_until;
}
/* после своего сообщения в канале с медленным режимом — отсчёт */
static void slow_after_send(u64 cid) {
    SendLimit l;
    send_limit(cid, &l);
    if (l.slowmode > 0) slow_set(cid, wall_ms() / 1000 + l.slowmode);
}
static void fmt_wait(i64 s, char *out, size_t n) {
    if (s >= 3600) snprintf(out, n, ui_tr("%lld ч %lld мин"), (long long)(s / 3600), (long long)(s / 60 % 60));
    else if (s >= 60) snprintf(out, n, ui_tr("%lld мин %lld с"), (long long)(s / 60), (long long)(s % 60));
    else snprintf(out, n, ui_tr("%lld с"), (long long)s);
}
static i64 slow_next_wake(void) {
    SendLimit l;
    if (!S.open_ch) return INT64_MAX;
    send_limit(S.open_ch, &l);
    i64 wall = wall_ms(), until = l.slow_until ? l.slow_until : 0;
    if (!until && slow_wake_dirty) { slow_wake_dirty = false; ui_dirty(); }   /* отсчёт закончился */
    if (until) {                                /* раз в секунду, на границе секунды */
        i64 next = (wall / 1000 + 1) * 1000;
        return now_ms() + (next - wall);
    }
    if (l.timeout_until) return now_ms() + (l.timeout_until * 1000 - wall) + 50;   /* одно пробуждение — конец тайм-аута */
    return INT64_MAX;
}

/* ---------- отправка с «ожидает» и повтором (по nonce) ---------- */
#define PEND_MAX 16
/* Ревью 9 С1: слот живёт, пока запрос в полёте или сообщение «не отправлено» (для повтора). Ответ 200
 * сразу заменяет местное сообщение настоящим (из тела ответа) и освобождает слот; при смене канала
 * слоты не в полёте освобождаются; полная таблица вытесняет самый старый «не отправлено». */
static struct { u64 nonce, cid, reply; int opts; char *text; bool fly; i64 at; bool sent; } PD[PEND_MAX];   /* sent: 200 без тела */
static int pend_find(u64 nonce) { for (int i = 0; i < PEND_MAX; i++) if (PD[i].nonce && PD[i].nonce == nonce) return i; return -1; }
static void pend_drop(int i) { if (i < 0) return; free(PD[i].text); memset(&PD[i], 0, sizeof PD[i]); }
static void pend_mark(u64 nonce, u8 st) { Message *m = msg_find(nonce, NULL); if (m && m->state != MS_SENT) { m->state = st; ui_dirty(); } }
/* убрать из ленты местное («ожидает»/«не отправлено»/«отправлено без ответа») сообщение с id == nonce */
static void local_remove(u64 nonce) {
    int k;
    Message *m = msg_find(nonce, &k);
    int i = pend_find(nonce);
    if (!m || (m->state == MS_SENT && (i < 0 || !PD[i].sent))) return;   /* настоящее сообщение не трогаем */
    msg_free(&S.msgs[k]);
    memmove(&S.msgs[k], &S.msgs[k + 1], (size_t)(S.nmsgs - k - 1) * sizeof(Message));
    S.nmsgs--;
}
/* msgs_clear: ленты больше нет — слоты не в полёте не нужны */
static void pend_on_clear(void) {
    bool back = false;
    for (int i = 0; i < PEND_MAX; i++) {
        if (!PD[i].nonce || PD[i].fly) continue;
        /* ревью 10 М1: «не отправлено» не пропадает молча — текст в черновик своего канала */
        if (!PD[i].sent && PD[i].text && ui_draft_restore(PD[i].cid, PD[i].text)) back = true;
        pend_drop(i);
    }
    demo_slow = 0;
    if (back) ui_notice(ui_tr("неотправленное сообщение возвращено в поле ввода"));
}
static void on_send(Resp *r, void *ctx) {
    u64 nonce = (u64)(uintptr_t)ctx;
    int i = pend_find(nonce);
    if (i >= 0) PD[i].fly = false;
    if (r->status == 200) {
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        u64 id = jid(j, "id"), cid = jid(j, "channel_id");
        if (id && cid && jid(jget(j, "author"), "id") == S.me_id && (i < 0 || cid == PD[i].cid)) {
            local_remove(nonce);
            if (cid == S.open_ch && !S.detached && !msg_find(id, NULL)) { message_fill(msg_insert(id), j); msgs_trim(false); }
            slow_after_send(cid);
            pend_drop(i);
        } else {                                /* ревью 10 М2: без пригодного тела — «отправлено», заменит MESSAGE_CREATE */
            if (i >= 0) { PD[i].sent = true; slow_after_send(PD[i].cid); }
            Message *m = msg_find(nonce, NULL);
            if (m) m->state = MS_SENT;
        }
        json_done();
        ui_dirty();
        return;
    }
    if (i >= 0) PD[i].at = now_ms();
    pend_mark(nonce, MS_FAILED);
    char e[160];
    if (r->status == 429) {                     /* №127: медленный режим (код 20016) — net.c не повторяет */
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        double ra = cJSON_IsNumber(jget(j, "retry_after")) ? jget(j, "retry_after")->valuedouble : 0;
        json_done();
        if (ra > 0 && ra <= 21600 && i >= 0) {
            slow_set(PD[i].cid, wall_ms() / 1000 + (i64)(ra + 0.999));
            char w[48];
            fmt_wait((i64)(ra + 0.999), w, sizeof w);
            ui_notice(ui_tr("медленный режим: можно писать через %s"), w);
            return;
        }
    }
    http_err(e, sizeof e, ui_tr("сообщение не отправлено"), r->status);
    ui_notice("%s", e);
}
static void send_post(int i) {
    Buf b = {0};
    buf_puts(&b, "{\"content\":");
    buf_json_str(&b, PD[i].text);
    int fl = (PD[i].opts & SEND_SILENT ? 4096 : 0) | (PD[i].opts & SEND_NO_EMBEDS ? 4 : 0);
    buf_printf(&b, ",\"nonce\":\"%llu\",\"enforce_nonce\":true,\"tts\":false,\"flags\":%d", (unsigned long long)PD[i].nonce, fl);
    if (PD[i].reply) buf_printf(&b, ",\"message_reference\":{\"message_id\":\"%llu\"}", (unsigned long long)PD[i].reply);
    if (PD[i].opts & SEND_NO_MENTIONS) buf_puts(&b, ",\"allowed_mentions\":{\"parse\":[],\"replied_user\":false}");
    else if (PD[i].reply && (PD[i].opts & SEND_NO_PING)) buf_puts(&b, ",\"allowed_mentions\":{\"parse\":[\"users\",\"roles\",\"everyone\"],\"replied_user\":false}");
    buf_puts(&b, "}");
    char path[64];
    snprintf(path, sizeof path, "/channels/%llu/messages", (unsigned long long)PD[i].cid);
    PD[i].fly = true;
    http_async("POST", path, b.p, on_send, (void *)(uintptr_t)PD[i].nonce);
    buf_free(&b);
}
u64 msg_send(u64 cid, const char *text, u64 reply_to, int opts) {
    Channel *c = chan_find(cid);
    if (!c || !text || !*text) return 0;
    {   /* №127: тайм-аут и медленный режим — не отправлять, сказать сколько ждать */
        SendLimit l;
        send_limit(cid, &l);
        i64 now = wall_ms() / 1000;
        char w[48];
        if (l.timeout_until) { fmt_wait(l.timeout_until - now, w, sizeof w); ui_notice(ui_tr("тайм-аут на сервере: писать можно через %s"), w); return 0; }
        Guild *pg = c->guild_id ? guild_find(c->guild_id) : NULL;
        if (pg && pg->pending) { ui_notice(ui_tr("сначала примите правила сервера")); return 0; }   /* №128 */
        if (l.slow_until) { fmt_wait(l.slow_until - now, w, sizeof w); ui_notice(ui_tr("медленный режим: можно писать через %s"), w); return 0; }
    }
    bool slow = O.demo && strstr(text, "!slow");
    if (O.demo && chan_is_thread(c) && strstr(text, "!delthread")) { demo_del_th = cid; demo_del_at = now_ms() + 3000; }
    if (O.demo && !slow && !strstr(text, "!fail")) { demo_send(cid, text); slow_after_send(cid); return 0; }   /* демо: сразу «доставлено» */
    if (slow && demo_slow) return 0;            /* демо: одно «медленное» за раз */
    int i;
    for (i = 0; i < PEND_MAX && PD[i].nonce; i++) {}
    if (i == PEND_MAX) {                        /* полна: вытеснить самый старый «не отправлено» */
        int old = -1;
        for (int k = 0; k < PEND_MAX; k++) if (!PD[k].fly && (old < 0 || PD[k].at < PD[old].at)) old = k;
        if (old < 0) { ui_notice(ui_tr("слишком много неотправленных сообщений")); return 0; }
        local_remove(PD[old].nonce);
        pend_drop(old);
        i = old;
    }
    static u64 last_nonce;                      /* два сообщения в одну миллисекунду — разные nonce */
    u64 nonce = snowflake_now();
    if (nonce <= last_nonce) nonce = last_nonce + 1;
    last_nonce = nonce;
    PD[i] = (typeof(PD[0])){ nonce, cid, reply_to, opts, xstrdup(text), false, now_ms() };
    if (cid == S.open_ch && !S.detached) {      /* в ленту сразу — «ожидает отправки» */
        Message *m = msg_insert(nonce);
        m->id = nonce;
        m->author_id = S.me_id;
        m->author = xstrdup(S.me_name ? S.me_name : "?");
        m->content = san(text, true);
        m->raw = xstrdup(m->content);
        m->ts = wall_ms() / 1000;
        m->state = MS_PENDING;
        msgs_trim(false);
    }
    if (slow) { demo_slow = nonce; demo_slow_at = now_ms() + 3000; ui_dirty(); return nonce; }   /* демо «!slow» */
    if (O.demo) { pend_mark(nonce, MS_FAILED); return nonce; }   /* демо «!fail»: не отправлено */
    send_post(i);
    ui_dirty();
    return nonce;
}
void msg_retry(u64 local_id) {
    int i = pend_find(local_id);
    if (i < 0) return;
    if (PD[i].fly || PD[i].sent) return;
    pend_mark(local_id, MS_PENDING);
    if (O.demo) { pend_mark(local_id, MS_FAILED); return; }
    send_post(i);
}
void msg_discard(u64 local_id) {
    int i = pend_find(local_id), k;
    pend_drop(i);
    if (msg_find(local_id, &k)) {
        msg_free(&S.msgs[k]);
        memmove(&S.msgs[k], &S.msgs[k + 1], (size_t)(S.nmsgs - k - 1) * sizeof(Message));
        S.nmsgs--;
    }
    ui_dirty();
}
/* MESSAGE_CREATE с нашим nonce: «ожидающее» заменяется настоящим */
static void pend_arrived(const cJSON *d) {
    const char *ns = jstr(d, "nonce");
    if (!ns) return;
    u64 nonce = strtoull(ns, NULL, 10);
    int i = pend_find(nonce);
    if (i < 0) return;
    /* А2: только своё и в том же канале — чужое сообщение с подобранным nonce ничего не снимает */
    if (jid(jget(d, "author"), "id") != S.me_id || jid(d, "channel_id") != PD[i].cid) return;
    local_remove(nonce);                        /* до pend_drop: «отправлено без ответа» узнаётся по слоту */
    pend_drop(i);
}
/* демо «!slow»: через 3 с — доставлено */
static void demo_slow_tick(i64 now) {
    if (!demo_slow || now < demo_slow_at) return;
    int i = pend_find(demo_slow);
    demo_slow = 0;
    if (i < 0) return;
    u64 cid = PD[i].cid;
    char *t = PD[i].text;
    PD[i].text = NULL;
    local_remove(PD[i].nonce);
    pend_drop(i);
    demo_send(cid, t);
    free(t);
    ui_dirty();
}

/* ======================= пакет Б ======================= */
/* ---------- №32 эмодзи серверов (по запросу пикера) ---------- */
static EmojiState EM;
const EmojiState *emoji_state(void) { return &EM; }
void emoji_close(void) { free(EM.e); memset(&EM, 0, sizeof EM); }
static void emoji_fill(const cJSON *j) {
    int n = cJSON_GetArraySize(j);
    free(EM.e);
    EM.e = n > 0 ? xcalloc((size_t)(n > 500 ? 500 : n), sizeof(GuildEmoji)) : NULL;
    EM.n = 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, j) {
        if (EM.n >= 500) break;
        const char *nm = jstr(x, "name");
        u64 id = jid(x, "id");
        if (!nm || !id) continue;
        GuildEmoji *g = &EM.e[EM.n];
        size_t k = 0;                           /* имя эмодзи: [A-Za-z0-9_], до 32 */
        for (const char *p = nm; *p && k < 32; p++)
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') g->name[k++] = *p;
        if (k < 2) continue;
        g->id = id;
        g->animated = jbool(x, "animated");
        g->available = !cJSON_IsFalse(jget(x, "available"));
        EM.n++;
    }
}
static void on_emojis(Resp *r, void *ctx) {
    if ((u64)(uintptr_t)ctx != EM.gid || !EM.busy) return;
    EM.busy = false;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len); emoji_fill(j); json_done(); }
    else http_err(EM.err, sizeof EM.err, ui_tr("эмодзи сервера"), r->status);
    ui_dirty();
}
void emoji_open(u64 gid) {
    if (EM.gid == gid && (EM.busy || EM.n)) return;
    emoji_close();
    if (!gid || !guild_find(gid)) return;
    EM.gid = gid;
    if (O.demo) {
        static const char *const dj = "[{\"id\":\"7001\",\"name\":\"kot\"},{\"id\":\"7002\",\"name\":\"party\",\"animated\":true},"
            "{\"id\":\"7003\",\"name\":\"ok_hand\"},{\"id\":\"7004\",\"name\":\"old\",\"available\":false}]";
        cJSON *j = json_parse(dj, strlen(dj));
        emoji_fill(j);
        json_done();
        return;
    }
    char path[64];
    snprintf(path, sizeof path, "/guilds/%llu/emojis", (unsigned long long)gid);
    EM.busy = true;
    http_async("GET", path, NULL, on_emojis, (void *)(uintptr_t)gid);
}
/* для selftest: подставной ответ на emoji_open */
void emoji_test_resp(long status, const char *body) {
    Resp r = { .status = status };
    buf_puts(&r.body, body);
    EM.busy = true;
    on_emojis(&r, (void *)(uintptr_t)EM.gid);
    buf_free(&r.body);
}
bool emoji_need_nitro(const GuildEmoji *e, u64 emoji_gid, u64 target_ch) {
    if (!e) return true;
    if (S.me_premium) return false;
    Channel *c = chan_find(target_ch);
    return e->animated || !c || c->guild_id != emoji_gid;
}
void emoji_code(const GuildEmoji *e, char *out, size_t n) {
    if (!e || !e->id) { if (n) out[0] = 0; return; }
    snprintf(out, n, "<%s:%s:%llu>", e->animated ? "a" : "", e->name, (unsigned long long)e->id);
}

/* ---------- №45/46 разметка строки ---------- */
static int md_put(MdSpan *sp, int k, int max, size_t off, size_t len, u8 attr) {
    if (!len || off > 0xFFFF || len > 0xFFFF) return k;
    if (k > 0 && sp[k - 1].attr == attr && !(attr & MD_LINK) && sp[k - 1].off + sp[k - 1].len == off) { sp[k - 1].len = (u16)(sp[k - 1].len + len); return k; }
    if (k >= max) return k;
    sp[k] = (MdSpan){ (u16)off, (u16)len, attr, 0, 0 };
    return k + 1;
}
static bool md_punct(char c) { return c && strchr("\\*_~`|[]()#->.!", c) != NULL; }
/* закрывающий маркер mk (длины ml) в [i, e); для одиночных * и _ — не удвоенный */
static size_t md_close(const char *s, size_t i, size_t e, const char *mk, size_t ml) {
    for (size_t j = i; j + ml <= e; j++) {
        if (s[j] == '\\') { j++; continue; }
        if (s[j] == '`') {                      /* внутри кода маркеры не действуют */
            const char *q = memchr(s + j + 1, '`', e - j - 1);
            if (q) { j = (size_t)(q - s); continue; }
        }
        if (memcmp(s + j, mk, ml)) continue;
        if (ml == 2) while (j + 2 < e && s[j + 2] == mk[0]) j++;   /* «***»: закрывают последние две */
        if (ml == 1 && ((j + 1 < e && s[j + 1] == mk[0]) || (j > i && s[j - 1] == mk[0]))) { if (j + 1 < e && s[j + 1] == mk[0]) j++; continue; }
        if (j == i) continue;                   /* пустое содержимое */
        if (s[j - 1] == ' ') continue;          /* «* текст *» — не разметка */
        return j;
    }
    return (size_t)-1;
}
static int md_inline(const char *s, size_t a, size_t e, u8 attr, MdSpan *sp, int k, int max, int depth) {
    size_t t = a;                               /* начало накопленного обычного текста */
    size_t i = a;
    while (i < e) {
        char c = s[i];
        if (c == '\\' && i + 1 < e && md_punct(s[i + 1])) {
            k = md_put(sp, k, max, t, i - t, attr);
            t = i + 1;
            i += 2;
            continue;
        }
        if (c == '`') {
            const char *q = i + 1 < e ? memchr(s + i + 1, '`', e - i - 1) : NULL;
            if (q && (size_t)(q - s) > i + 1) {
                k = md_put(sp, k, max, t, i - t, attr);
                k = md_put(sp, k, max, i + 1, (size_t)(q - s) - i - 1, (u8)(attr | MD_CODE));
                i = (size_t)(q - s) + 1;
                t = i;
                continue;
            }
        }
        static const struct { const char *mk; u8 at; } M2[] = { { "**", MD_B }, { "__", MD_U }, { "~~", MD_S }, { "||", MD_SPOIL } };
        bool done = false;
        if (depth < 6) for (size_t m = 0; m < 4 && !done; m++) {
            if (i + 2 > e || memcmp(s + i, M2[m].mk, 2) || i + 2 >= e || s[i + 2] == ' ') continue;
            size_t cl = md_close(s, i + 2, e, M2[m].mk, 2);
            if (cl == (size_t)-1) continue;
            k = md_put(sp, k, max, t, i - t, attr);
            k = md_inline(s, i + 2, cl, (u8)(attr | M2[m].at), sp, k, max, depth + 1);
            i = cl + 2;
            t = i;
            done = true;
        }
        if (done) continue;
        if ((c == '*' || c == '_') && depth < 6 && i + 1 < e && s[i + 1] != ' ' && s[i + 1] != c &&
            (c == '*' || i == a || !(((unsigned char)s[i - 1] | 32) >= 'a' && ((unsigned char)s[i - 1] | 32) <= 'z'))) {
            char mk[2] = { c, 0 };
            size_t cl = md_close(s, i + 1, e, mk, 1);
            if (cl != (size_t)-1) {
                k = md_put(sp, k, max, t, i - t, attr);
                k = md_inline(s, i + 1, cl, (u8)(attr | MD_I), sp, k, max, depth + 1);
                i = cl + 1;
                t = i;
                continue;
            }
        }
        if (c == '[' && depth < 6) {            /* [текст](https://...) */
            const char *rb = memchr(s + i, ']', e - i);
            if (rb && (size_t)(rb - s) + 1 < e && rb[1] == '(') {
                size_t u0 = (size_t)(rb - s) + 2;
                const char *rp = memchr(s + u0, ')', e - u0);
                bool web = rp && ((size_t)(rp - s) - u0 > 8) && (!strncmp(s + u0, "https://", 8) || !strncmp(s + u0, "http://", 7));
                for (size_t q = u0; web && q < (size_t)(rp - s); q++) if (s[q] == ' ') web = false;
                if (web && rb > s + i + 1) {
                    k = md_put(sp, k, max, t, i - t, attr);
                    int k0 = k;
                    k = md_inline(s, i + 1, (size_t)(rb - s), (u8)(attr | MD_LINK), sp, k, max, depth + 1);
                    for (int q = k0; q < k; q++) { sp[q].url_off = (u16)u0; sp[q].url_len = (u16)((size_t)(rp - s) - u0); }
                    i = (size_t)(rp - s) + 1;
                    t = i;
                    continue;
                }
            }
        }
        i++;
    }
    return md_put(sp, k, max, t, e - t, attr);
}
int md_line(const char *s, size_t n, MdLine *ln, MdSpan *sp, int max) {
    MdLine l = { ML_TEXT, 0, 0, 0 };
    size_t b = 0;
    if (!s) { if (ln) *ln = l; return 0; }
    if (n > 0xFFF0) n = 0xFFF0;
    if (n >= 2 && !strncmp(s, "# ", 2)) { l.kind = ML_H1; b = 2; }
    else if (n >= 3 && !strncmp(s, "## ", 3)) { l.kind = ML_H2; b = 3; }
    else if (n >= 4 && !strncmp(s, "### ", 4)) { l.kind = ML_H3; b = 4; }
    else if (n >= 3 && !strncmp(s, "-# ", 3)) { l.kind = ML_SUB; b = 3; }
    else {
        size_t sp0 = 0;
        while (sp0 < n && sp0 < 8 && s[sp0] == ' ') sp0++;
        if (sp0 + 1 < n && (s[sp0] == '-' || s[sp0] == '*') && s[sp0 + 1] == ' ') { l.kind = ML_LIST; l.indent = (u8)(sp0 / 2); b = sp0 + 2; }
        else {
            size_t d = sp0;
            while (d < n && d - sp0 < 9 && s[d] >= '0' && s[d] <= '9') d++;
            if (d > sp0 && d + 1 < n && s[d] == '.' && s[d + 1] == ' ') {
                l.kind = ML_OLIST; l.indent = (u8)(sp0 / 2); l.num_off = (u16)sp0; l.num_len = (u16)(d - sp0 + 1); b = d + 2;
            }
        }
    }
    if (ln) *ln = l;
    if (!sp || max <= 0) return 0;
    return md_inline(s, b, n, 0, sp, 0, max, 0);
}
bool md_code_block(const char *c, int k, Buf *out) {
    if (!c) return false;
    int idx = 0;
    const char *p = c;
    for (;;) {
        const char *a = strstr(p, "```");
        if (!a) return false;
        const char *body = a + 3, *nl = strchr(body, '\n'), *z = strstr(body, "```");
        if (!z) return false;
        if (nl && nl < z) {                     /* первая строка ограды — язык (без пробелов) */
            bool lang = true;
            for (const char *q = body; q < nl; q++) if (*q == ' ') lang = false;
            if (lang) body = nl + 1;
        }
        if (idx++ == k) {
            const char *e = z;
            if (e > body && e[-1] == '\n') e--;
            buf_put(out, body, (size_t)(e - body));
            return true;
        }
        p = z + 3;
    }
}

/* ---------- №41 опросы ---------- */
static void poll_free(Poll *p) {
    if (!p) return;
    free(p->question);
    for (int i = 0; i < p->n; i++) free(p->a[i].text);
    free(p);
}
static Poll *poll_parse(const cJSON *po) {
    if (!cJSON_IsObject(po)) return NULL;
    Poll *p = xcalloc(1, sizeof *p);
    char *q = san(jstr(jget(po, "question"), "text"), false);
    if (strlen(q) > 300) { size_t k = 300; while (k && ((unsigned char)q[k] & 0xC0) == 0x80) k--; q[k] = 0; }
    p->question = q;
    p->expiry = iso_to_unix(jstr(po, "expiry"));
    p->multi = jbool(po, "allow_multiselect");
    const cJSON *x;
    cJSON_ArrayForEach(x, jget(po, "answers")) {
        if (p->n >= 10) break;
        PollAnswer *a = &p->a[p->n];
        a->id = (u32)jint(x, "answer_id", 0);
        if (!a->id) continue;
        const cJSON *md = jget(x, "poll_media");
        const char *em = jstr(jget(md, "emoji"), "name"), *tx = jstr(md, "text");
        char buf[400];
        snprintf(buf, sizeof buf, "%.60s%s%.300s", em && !jid(jget(md, "emoji"), "id") ? em : "", em && tx ? " " : "", tx ? tx : "");
        a->text = san(buf, false);
        utf8_trunc(a->text, 199);               /* ревью 11 М3: по границе символа */
        p->n++;
    }
    const cJSON *res = jget(po, "results");
    p->final = jbool(res, "is_finalized");
    cJSON_ArrayForEach(x, jget(res, "answer_counts")) {
        u32 id = (u32)jint(x, "id", 0);
        for (int i = 0; i < p->n; i++) if (p->a[i].id == id) { p->a[i].count = (int)jint(x, "count", 0); p->a[i].me = jbool(x, "me_voted"); }
    }
    return p;
}
static void poll_vote_ev(const cJSON *d, int delta) {
    if (jid(d, "channel_id") != S.open_ch) return;
    Message *m = msg_find(jid(d, "message_id"), NULL);
    if (!m || !m->poll) return;
    u32 id = (u32)jint(d, "answer_id", 0);
    for (int i = 0; i < m->poll->n; i++) if (m->poll->a[i].id == id) {
        PollAnswer *a = &m->poll->a[i];
        a->count += delta;
        if (a->count < 0) a->count = 0;
        if (jid(d, "user_id") == S.me_id) a->me = delta > 0;
    }
    ui_dirty();
}
static void on_poll(Resp *r, void *ctx) {
    if (r->status >= 200 && r->status < 300) return;   /* голоса придут событиями MESSAGE_POLL_VOTE_* */
    char e[160];
    http_err(e, sizeof e, ctx ? (const char *)ctx : ui_tr("опрос"), r->status);
    ui_notice("%s", e);
}
bool poll_vote(u64 cid, u64 mid, const u32 *ids, int n, char *err, size_t errn) {
    Message *m = msg_find(mid, NULL);
    Poll *p = m ? m->poll : NULL;
    if (!p || cid != S.open_ch) { snprintf(err, errn, ui_tr("опрос не найден")); return false; }
    if (p->final || (p->expiry && p->expiry <= wall_ms() / 1000)) { snprintf(err, errn, ui_tr("опрос завершён")); return false; }
    if (n < 0 || n > p->n || (n > 1 && !p->multi)) { snprintf(err, errn, ui_tr("в этом опросе можно выбрать один ответ")); return false; }
    for (int k = 0; k < n; k++) {
        bool ok = false;
        for (int i = 0; i < p->n; i++) if (p->a[i].id == ids[k]) ok = true;
        if (!ok) { snprintf(err, errn, ui_tr("нет такого ответа")); return false; }
    }
    if (O.demo) {                               /* демо: голос сразу в модели */
        for (int i = 0; i < p->n; i++) {
            bool want = false;
            for (int k = 0; k < n; k++) if (p->a[i].id == ids[k]) want = true;
            if (p->a[i].me && !want) { p->a[i].me = false; p->a[i].count--; }
            else if (!p->a[i].me && want) { p->a[i].me = true; p->a[i].count++; }
        }
        ui_dirty();
        return true;
    }
    Buf b = {0};
    buf_puts(&b, "{\"answer_ids\":[");
    for (int k = 0; k < n; k++) buf_printf(&b, "%s\"%u\"", k ? "," : "", ids[k]);
    buf_puts(&b, "]}");
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/polls/%llu/answers/@me", (unsigned long long)cid, (unsigned long long)mid);
    http_async("PUT", path, b.p, on_poll, (void *)(n ? ui_tr("голос не принят") : ui_tr("голос не снят")));
    buf_free(&b);
    return true;
}
bool poll_end(u64 cid, u64 mid, char *err, size_t errn) {
    Message *m = msg_find(mid, NULL);
    if (!m || !m->poll || cid != S.open_ch) { snprintf(err, errn, ui_tr("опрос не найден")); return false; }
    if (m->author_id != S.me_id) { snprintf(err, errn, ui_tr("завершить опрос может только автор")); return false; }
    if (m->poll->final) { snprintf(err, errn, ui_tr("опрос уже завершён")); return false; }
    if (O.demo) { m->poll->final = true; ui_dirty(); return true; }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/polls/%llu/expire", (unsigned long long)cid, (unsigned long long)mid);
    http_async("POST", path, NULL, on_poll, (void *)ui_tr("опрос не завершён"));
    return true;
}

/* ---------- №53 эмбеды карточкой ---------- */
const char *embed_str(const Embed *e, int k) {
    if (!e || !e->s || k < 0 || k >= EM_NSTR || e->o[k] == 0xFFFF) return "";
    return e->s + e->o[k];
}
bool embed_field(const Embed *e, int i, const char **name, const char **value, bool *inl) {
    if (!e || !e->s || i < 0 || i >= e->nf) return false;
    const char *p = e->s + e->fo;
    for (int k = 0; k < i; k++) { p += strlen(p) + 1; p += strlen(p) + 1; }
    *name = p;
    *value = p + strlen(p) + 1;
    if (inl) *inl = (e->f_inline >> i) & 1;
    return true;
}
/* строку — в буфер карточки, не больше остатка бюджета (обрезка по символу UTF-8) */
static u16 emb_put(Buf *b, const char *s, bool multi, size_t cap) {
    if (!s || !*s || b->len >= EMB_BYTES - 1) return 0xFFFF;
    char *t = san(s, multi);
    size_t room = EMB_BYTES - 1 - b->len, n = strlen(t);
    if (room > cap) room = cap;
    if (n > room) { n = room; while (n && ((unsigned char)t[n] & 0xC0) == 0x80) n--; }
    u16 off = (u16)b->len;
    buf_put(b, t, n);
    buf_put(b, "", 1);
    free(t);
    return n ? off : 0xFFFF;
}
static bool emb_img_ok(const char *u) { return u && cdn_host_ok(u) && strlen(u) < 600; }
/* ревью 11 М1: адрес — целиком или никак (обрезанная ссылка хуже отсутствующей) */
static u16 emb_put_whole(Buf *b, const char *s) {
    if (!s || !*s) return 0xFFFF;
    size_t n = strlen(s);
    if (b->len + n + 1 > EMB_BYTES - 1) return 0xFFFF;
    for (const char *p = s; *p; p++) if ((unsigned char)*p < 0x21 || *p == 0x7F) return 0xFFFF;   /* в адресе — без пробелов и управляющих */
    u16 off = (u16)b->len;
    buf_put(b, s, n + 1);
    return off;
}
static void embeds_parse(Message *m, const cJSON *arr) {
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return;
    if (n > EMB_MAX) n = EMB_MAX;
    m->emb = xcalloc((size_t)n, sizeof(Embed));
    const cJSON *x;
    cJSON_ArrayForEach(x, arr) {
        if (m->nemb >= n) break;
        Embed *e = &m->emb[m->nemb];
        Buf b = {0};
        for (int k = 0; k < EM_NSTR; k++) e->o[k] = 0xFFFF;
        const cJSON *col = jget(x, "color");
        if (cJSON_IsNumber(col)) { e->color = (u32)col->valuedouble & 0xFFFFFFu; e->has_color = true; }
        const char *url = jstr(x, "url");
        const cJSON *img = jget(x, "image"), *th = jget(x, "thumbnail");
        const char *iu = jstr(img, "proxy_url") ? jstr(img, "proxy_url") : jstr(img, "url");
        const char *tu = jstr(th, "proxy_url") ? jstr(th, "proxy_url") : jstr(th, "url");
        /* сначала короткое и важное, описание и поля — в остаток бюджета */
        if (url && !strncmp(url, "https://", 8) && strlen(url) < 400) e->o[EM_URL] = emb_put_whole(&b, url);
        if (emb_img_ok(iu)) e->o[EM_IMAGE] = emb_put_whole(&b, iu);
        if (emb_img_ok(tu)) e->o[EM_THUMB] = emb_put_whole(&b, tu);
        e->o[EM_AUTHOR] = emb_put(&b, jstr(jget(x, "author"), "name"), false, (size_t)-1);
        e->o[EM_TITLE] = emb_put(&b, jstr(x, "title"), false, (size_t)-1);
        e->o[EM_PROVIDER] = emb_put(&b, jstr(jget(x, "provider"), "name"), false, (size_t)-1);
        e->o[EM_FOOTER] = emb_put(&b, jstr(jget(x, "footer"), "text"), false, (size_t)-1);
        size_t fres = cJSON_GetArraySize(jget(x, "fields")) > 0 ? 256 : 0;   /* полям — хотя бы 256 Б */
        e->o[EM_DESC] = emb_put(&b, jstr(x, "description"), true, b.len + fres < EMB_BYTES - 1 ? EMB_BYTES - 1 - b.len - fres : 0);
        e->fo = (u16)b.len;
        const cJSON *f;
        cJSON_ArrayForEach(f, jget(x, "fields")) {
            if (e->nf >= 25 || b.len >= EMB_BYTES - 8) break;
            size_t keep = b.len;
            u16 a = emb_put(&b, jstr(f, "name"), false, (size_t)-1);
            if (a == 0xFFFF) { b.len = keep; break; }
            u16 v = emb_put(&b, jstr(f, "value"), true, (size_t)-1);
            if (v == 0xFFFF) { if (b.len < EMB_BYTES) buf_put(&b, "", 1); else { b.len = keep; break; } }
            if (jbool(f, "inline")) e->f_inline |= 1u << e->nf;
            e->nf++;
        }
        bool any = b.len > 0;
        e->s = any ? b.p : NULL;
        if (!any) { buf_free(&b); continue; }
        m->nemb++;
    }
    if (!m->nemb) { free(m->emb); m->emb = NULL; }
}
/* ---------- №55 кнопки и меню ботов ---------- */
static void comps_free(Message *m) { if (m->comp) { free(m->comp->s); free(m->comp); m->comp = NULL; } }
/* Подпись (показ): san, до 100 символов. Ревью 12 М1: custom_id, value и url — сырые, целиком
 * (их только отправляют обратно через buf_json_str / копируют), до 400 байт, иначе — нет. */
static u16 cp_put(Buf *b, const char *s) {
    if (!s || !*s || b->len > 4000) return 0xFFFF;
    char *t = san(s, false);
    size_t n = 0, ch = 0;
    while (t[n] && ch < 100) { n++; while (((unsigned char)t[n] & 0xC0) == 0x80) n++; ch++; }
    u16 off = (u16)b->len;
    buf_put(b, t, n);
    buf_put(b, "", 1);
    free(t);
    return off;
}
static u16 cp_raw(Buf *b, const char *s) {
    if (!s || !*s || b->len > 4000) return 0xFFFF;
    size_t n = strlen(s);
    if (n > 400) return 0xFFFF;
    u16 off = (u16)b->len;
    buf_put(b, s, n + 1);
    return off;
}
static void comps_parse(Message *m, const cJSON *rows, u64 app) {
    if (!cJSON_GetArraySize(rows) || !app) return;
    Comp tmp[25];                               /* ревью 12 М2: в сообщении — только фактическое число */
    int nc = 0;
    Buf b = {0};
    buf_put(&b, "", 1);                         /* смещение 0 — пустая строка */
    int r = 0;
    const cJSON *row, *x;
    cJSON_ArrayForEach(row, rows) {
        if (r >= 5) break;
        cJSON_ArrayForEach(x, jget(row, "components")) {
            int t = (int)jint(x, "type", 0);
            if (nc >= 25 || (t != CT_BUTTON && t != CT_SELECT)) continue;
            Comp *k = &tmp[nc];
            memset(k, 0, sizeof *k);
            for (int i = 0; i < CS_N; i++) k->s[i] = 0xFFFF;
            k->type = (u8)t; k->row = (u8)r;
            k->style = (u8)jint(x, "style", 0);
            k->disabled = jbool(x, "disabled");
            const char *url = jstr(x, "url");
            bool url_ok = false;
            if (k->style == 5) {                /* ссылка — только https; ревью 13 М3: только печатный ASCII */
                url_ok = url && !strncmp(url, "https://", 8);
                for (const char *q = url; url_ok && *q; q++) if ((unsigned char)*q <= 0x20 || (unsigned char)*q >= 0x7F) url_ok = false;
                if (!url || strncmp(url, "https://", 8)) continue;
            }
            k->s[CS_LABEL] = cp_put(&b, jstr(x, "label"));
            k->s[CS_ID] = cp_raw(&b, jstr(x, "custom_id"));
            k->s[CS_URL] = k->style == 5 && url_ok ? cp_raw(&b, url) : 0xFFFF;
            const cJSON *em = jget(x, "emoji");
            k->s[CS_EMOJI] = jid(em, "id") ? 0xFFFF : cp_put(&b, jstr(em, "name"));
            if (t == CT_SELECT) {
                k->s[CS_PLACEHOLDER] = cp_put(&b, jstr(x, "placeholder"));
                k->min = (u8)jint(x, "min_values", 1);
                k->max = (u8)jint(x, "max_values", 1);
                k->opt = (u16)b.len;
                const cJSON *o;
                cJSON_ArrayForEach(o, jget(x, "options")) {
                    if (k->nopt >= 25 || b.len > 4000) break;
                    const char *v = jstr(o, "value");
                    if (!v || strlen(v) > 400) break;
                    if (cp_put(&b, jstr(o, "label")) == 0xFFFF) break;
                    buf_put(&b, v, strlen(v) + 1);   /* значение — сырое */
                    k->nopt++;
                }
            }
            if (k->s[CS_ID] == 0xFFFF && k->style != 5) continue;   /* без custom_id нажимать нечего */
            nc++;
        }
        r++;
    }
    if (!nc) { buf_free(&b); return; }
    Comps *c = xmalloc(sizeof *c + (size_t)nc * sizeof(Comp));
    c->app_id = app;
    c->s = b.p;
    c->n = (u8)nc;
    memcpy(c->c, tmp, (size_t)nc * sizeof(Comp));
    m->comp = c;
}
const char *comp_str(const Message *m, int i, int k) {
    if (!m || !m->comp || i < 0 || i >= m->comp->n || k < 0 || k >= CS_N || m->comp->c[i].s[k] == 0xFFFF) return "";
    return m->comp->s + m->comp->c[i].s[k];
}
bool comp_option(const Message *m, int i, int k, const char **label, const char **value) {
    if (!m || !m->comp || i < 0 || i >= m->comp->n || k < 0 || k >= m->comp->c[i].nopt) return false;
    const char *p = m->comp->s + m->comp->c[i].opt;
    for (int q = 0; q < k; q++) { p += strlen(p) + 1; p += strlen(p) + 1; }
    *label = p;
    *value = p + strlen(p) + 1;
    return true;
}
static u64 ix_nonce;                            /* ждём INTERACTION_SUCCESS/FAILURE с этим nonce */
static i64 ix_until;
static void on_ix(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status >= 200 && r->status < 300) return;
    ix_nonce = 0;
    char e[160];
    http_err(e, sizeof e, ui_tr("бот не принял нажатие"), r->status);
    ui_notice("%s", e);
}
static bool comp_send(u64 cid, u64 mid, int i, const int *opts, int n, char *err, size_t errn) {
    Message *m = msg_find(mid, NULL);
    Channel *c = chan_find(cid);
    if (!m || !m->comp || !c || cid != S.open_ch || i < 0 || i >= m->comp->n) { snprintf(err, errn, ui_tr("кнопка не найдена")); return false; }
    const Comp *k = &m->comp->c[i];
    if (k->disabled) { snprintf(err, errn, ui_tr("кнопка недоступна")); return false; }
    if (k->style == 5) {
        if (!*comp_str(m, i, CS_URL)) snprintf(err, errn, ui_tr("ссылка содержит недопустимые символы"));
        else snprintf(err, errn, ui_tr("это ссылка: %s"), comp_str(m, i, CS_URL));
        return false;
    }
    if (k->type == CT_SELECT && (n < k->min || n > k->max || n > k->nopt)) { snprintf(err, errn, ui_tr("выберите от %d до %d"), k->min, k->max); return false; }
    if (ix_nonce && now_ms() < ix_until) { snprintf(err, errn, ui_tr("ждём ответа бота на прошлое нажатие")); return false; }
    if (O.demo) { ui_notice(ui_tr("демо: бот получил «%s»"), k->type == CT_BUTTON ? comp_str(m, i, CS_LABEL) : ui_tr("выбор")); return true; }
    u64 nonce = snowflake_now();
    Buf b = {0};
    buf_printf(&b, "{\"type\":3,\"nonce\":\"%llu\",", (unsigned long long)nonce);
    if (c->guild_id) buf_printf(&b, "\"guild_id\":\"%llu\",", (unsigned long long)c->guild_id);
    buf_printf(&b, "\"channel_id\":\"%llu\",\"message_flags\":%u,\"message_id\":\"%llu\",\"application_id\":\"%llu\",\"session_id\":",
               (unsigned long long)cid, (unsigned)m->flags, (unsigned long long)mid, (unsigned long long)m->comp->app_id);
    buf_json_str(&b, S.session_id ? S.session_id : "");
    buf_printf(&b, ",\"data\":{\"component_type\":%d,\"custom_id\":", k->type);
    buf_json_str(&b, comp_str(m, i, CS_ID));
    if (k->type == CT_SELECT) {
        buf_puts(&b, ",\"type\":3,\"values\":[");
        for (int q = 0; q < n; q++) {
            const char *lb, *v;
            if (opts[q] < 0 || !comp_option(m, i, opts[q], &lb, &v)) { buf_free(&b); snprintf(err, errn, ui_tr("нет такого варианта")); return false; }
            if (q) buf_puts(&b, ",");
            buf_json_str(&b, v);
        }
        buf_puts(&b, "]");
    }
    buf_puts(&b, "}}");
    ix_nonce = nonce;
    ix_until = now_ms() + 5000;
    http_async("POST", "/interactions", b.p, on_ix, NULL);
    buf_free(&b);
    return true;
}
bool comp_click(u64 cid, u64 mid, int i, char *err, size_t errn) {
    Message *m = msg_find(mid, NULL);
    if (m && m->comp && i >= 0 && i < m->comp->n && m->comp->c[i].type != CT_BUTTON) { snprintf(err, errn, ui_tr("это список")); return false; }
    return comp_send(cid, mid, i, NULL, 0, err, errn);
}
bool comp_select(u64 cid, u64 mid, int i, const int *opts, int n, char *err, size_t errn) {
    Message *m = msg_find(mid, NULL);
    if (m && m->comp && i >= 0 && i < m->comp->n && m->comp->c[i].type != CT_SELECT) { snprintf(err, errn, ui_tr("это кнопка")); return false; }
    return comp_send(cid, mid, i, opts, n, err, errn);
}
static void ix_event(const char *t, const cJSON *d) {
    u64 n = strtoull(jstr(d, "nonce") ? jstr(d, "nonce") : "0", NULL, 10);
    if (!ix_nonce || n != ix_nonce) return;
    ix_nonce = 0;
    if (!strcmp(t, "INTERACTION_FAILURE")) ui_notice(ui_tr("бот не ответил на нажатие"));
}
static void ix_tick(i64 now) {
    if (ix_nonce && now >= ix_until) { ix_nonce = 0; ui_notice(ui_tr("бот не ответил на нажатие")); }
}

/* ---------- №109/111 запросы на переписку, группы ЛС ---------- */
u64 gdm_owner(u64 cid) { Channel *c = chan_find(cid); return c && c->type == CH_GROUP ? c->owner_id : 0; }
static void dm_flags(Channel *c, const cJSON *o) {
    if (c->guild_id) return;
    u8 f = 0;
    if (jbool(o, "is_message_request")) f |= DMF_REQUEST;
    if (jbool(o, "is_spam")) f |= DMF_SPAM;
    if (jget(o, "is_message_request") || jget(o, "is_spam")) c->dmflags = f;
}
static void on_dm_op(Resp *r, void *ctx) {
    const char *what = ctx ? (const char *)ctx : ui_tr("запрос");
    if (r->status >= 200 && r->status < 300) { ui_dirty(); return; }
    char e[160];
    http_err(e, sizeof e, what, r->status);
    ui_notice("%s", e);
}
static Channel *dm_chan(u64 cid, char *err, size_t errn, bool group) {
    Channel *c = chan_find(cid);
    if (!c || c->guild_id || (group && c->type != CH_GROUP) || (!group && c->type != CH_DM && c->type != CH_GROUP)) {
        snprintf(err, errn, group ? ui_tr("это не группа") : ui_tr("беседа не найдена"));
        return NULL;
    }
    return c;
}
bool dm_request_accept(u64 cid, char *err, size_t errn) {
    Channel *c = dm_chan(cid, err, errn, false);
    if (!c) return false;
    if (!(c->dmflags & (DMF_REQUEST | DMF_SPAM))) { snprintf(err, errn, ui_tr("это не запрос на переписку")); return false; }
    c->dmflags = 0;
    ui_dirty();
    if (O.demo) return true;
    char path[80];
    snprintf(path, sizeof path, "/channels/%llu/recipients/@me", (unsigned long long)cid);
    http_async("PUT", path, "{\"consent_status\":2}", on_dm_op, (void *)ui_tr("запрос не принят"));
    return true;
}
bool dm_request_reject(u64 cid, char *err, size_t errn) {
    Channel *c = dm_chan(cid, err, errn, false);
    if (!c) return false;
    if (!(c->dmflags & (DMF_REQUEST | DMF_SPAM))) { snprintf(err, errn, ui_tr("это не запрос на переписку")); return false; }
    if (O.demo) { chan_remove(cid); ui_dirty(); return true; }
    char path[64];
    snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)cid);
    http_async("DELETE", path, NULL, on_dm_op, (void *)ui_tr("запрос не отклонён"));   /* CHANNEL_DELETE уберёт беседу */
    return true;
}
bool gdm_add(u64 cid, u64 uid, char *err, size_t errn) {
    Channel *c = dm_chan(cid, err, errn, true);
    if (!c) return false;
    const Relation *r = rel_find(uid);
    if (!r || r->type != REL_FRIEND) { snprintf(err, errn, ui_tr("добавить в группу можно только друга")); return false; }
    for (int i = 0; i < c->nrecip; i++) if (c->recipients[i] == uid) { snprintf(err, errn, ui_tr("уже в группе")); return false; }
    if (c->nrecip >= 9) { snprintf(err, errn, ui_tr("в группе не больше 10 человек")); return false; }
    if (O.demo) {
        c->recipients = xrealloc(c->recipients, (size_t)(c->nrecip + 1) * sizeof(u64));
        c->recipients[c->nrecip++] = uid;
        ui_dirty();
        return true;
    }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/recipients/%llu", (unsigned long long)cid, (unsigned long long)uid);
    http_async("PUT", path, "{}", on_dm_op, (void *)ui_tr("не добавлен в группу"));   /* CHANNEL_RECIPIENT_ADD обновит */
    return true;
}
bool gdm_remove(u64 cid, u64 uid, char *err, size_t errn) {
    Channel *c = dm_chan(cid, err, errn, true);
    if (!c) return false;
    if (gdm_owner(cid) != S.me_id) { snprintf(err, errn, ui_tr("исключать может только владелец группы")); return false; }
    if (uid == S.me_id) { snprintf(err, errn, ui_tr("чтобы уйти, выберите 'Покинуть группу'")); return false; }
    if (O.demo) {
        for (int i = 0; i < c->nrecip; i++) if (c->recipients[i] == uid) { c->recipients[i] = c->recipients[--c->nrecip]; break; }
        ui_dirty();
        return true;
    }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/recipients/%llu", (unsigned long long)cid, (unsigned long long)uid);
    http_async("DELETE", path, NULL, on_dm_op, (void *)ui_tr("не исключён из группы"));
    return true;
}
bool gdm_leave(u64 cid, char *err, size_t errn) {
    if (!dm_chan(cid, err, errn, true)) return false;
    if (O.demo) { chan_remove(cid); ui_dirty(); return true; }
    char path[64];
    snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)cid);
    http_async("DELETE", path, NULL, on_dm_op, (void *)ui_tr("не удалось выйти из группы"));
    return true;
}

/* ---------- №2 папки серверов ---------- */
static GuildFolder *FD;
static int nfd;
static void fgs_free(void);
static void folders_free(void) {
    fgs_free();
    for (int i = 0; i < nfd; i++) { free(FD[i].name); free(FD[i].gids); }
    free(FD);
    FD = NULL;
    nfd = 0;
}
int guild_folders(const GuildFolder **out) { *out = FD; return nfd; }
static GuildFolder *folder_new(void) {
    if (nfd >= 100) return NULL;
    FD = xrealloc(FD, (size_t)(nfd + 1) * sizeof *FD);
    memset(&FD[nfd], 0, sizeof *FD);
    return &FD[nfd++];
}
/* ревью 15 К1 / 16 М3: множество уже добавленных gid (открытая адресация) — повтор отбрасывается за O(1) */
#define FGS_CAP 4096                            /* 32 КБ только на время разбора; до 2048 gid во всех папках */
static u64 *fgs;
static int fgs_n;
static bool fgs_add(u64 g) {
    if (!fgs) fgs = xcalloc(FGS_CAP, sizeof(u64));
    if (fgs_n >= FGS_CAP / 2) return false;
    u32 h = (u32)((g * 0x9E3779B97F4A7C15ULL) >> 49);
    for (;;) {
        u32 i = h & (FGS_CAP - 1);
        if (!fgs[i]) { fgs[i] = g; fgs_n++; return true; }
        if (fgs[i] == g) return false;
        h++;
    }
}
static void fgs_free(void) { free(fgs); fgs = NULL; fgs_n = 0; }
static void folder_add_gid(GuildFolder *f, u64 g) {
    if (!g || f->n >= 200) return;
    if (!fgs_add(g)) return;                    /* повтор в этой же или другой папке — отбросить */
    f->gids = xrealloc(f->gids, (size_t)(f->n + 1) * sizeof(u64));
    f->gids[f->n++] = g;
}
static void folders_json(const cJSON *arr) {
    const cJSON *x, *g;
    cJSON_ArrayForEach(x, arr) {
        GuildFolder *f = folder_new();
        if (!f) break;
        f->id = jint(x, "id", 0);
        const char *nm = jstr(x, "name");
        if (nm && *nm) { char *t = san(nm, false); if (strlen(t) > 64) { size_t k = 64; while (k && ((unsigned char)t[k] & 0xC0) == 0x80) k--; t[k] = 0; } f->name = t; }
        const cJSON *col = jget(x, "color");
        if (cJSON_IsNumber(col)) { f->color = (u32)col->valuedouble & 0xFFFFFFu; f->has_color = true; }
        cJSON_ArrayForEach(g, jget(x, "guild_ids")) folder_add_gid(f, id_of(g));
    }
    fgs_free();
}
/* protobuf: varint / длина; false — испорчено */
static bool pb_varint(const u8 *p, size_t n, size_t *i, u64 *v) {
    *v = 0;
    for (int s = 0; s < 64 && *i < n; s += 7) {
        u8 b = p[(*i)++];
        *v |= (u64)(b & 0x7F) << s;
        if (!(b & 0x80)) return true;
    }
    return false;
}
static bool pb_skip(const u8 *p, size_t n, size_t *i, int wt) {
    u64 v;
    switch (wt) {
    case 0: return pb_varint(p, n, i, &v);
    case 1: if (n - *i < 8) return false; *i += 8; return true;
    case 2: if (!pb_varint(p, n, i, &v) || v > n - *i) return false; *i += (size_t)v; return true;
    case 5: if (n - *i < 4) return false; *i += 4; return true;
    default: return false;
    }
}
/* обёртка google.protobuf.*Value: поле 1 */
static bool pb_wrapped(const u8 *p, size_t n, u64 *v, const u8 **s, size_t *sl) {
    size_t i = 0;
    while (i < n) {
        u64 key;
        if (!pb_varint(p, n, &i, &key)) return false;
        if ((key >> 3) == 1 && (key & 7) == 0 && v) return pb_varint(p, n, &i, v);
        if ((key >> 3) == 1 && (key & 7) == 2 && s) { u64 l; if (!pb_varint(p, n, &i, &l) || l > n - i) return false; *s = p + i; *sl = (size_t)l; return true; }
        if (!pb_skip(p, n, &i, (int)(key & 7))) return false;
    }
    return false;
}
static void pb_folder(const u8 *p, size_t n) {
    GuildFolder *f = folder_new();
    if (!f) return;
    size_t i = 0;
    while (i < n) {
        u64 key, l;
        if (!pb_varint(p, n, &i, &key)) return;
        int fn = (int)(key >> 3), wt = (int)(key & 7);
        if (fn == 1 && wt == 2) {               /* guild_ids: packed fixed64 */
            if (!pb_varint(p, n, &i, &l) || l > n - i) return;
            for (size_t k = 0; k + 8 <= l; k += 8) { u64 g = 0; for (int b = 7; b >= 0; b--) g = g << 8 | p[i + k + (size_t)b]; folder_add_gid(f, g); }
            i += (size_t)l;
        } else if (fn == 1 && wt == 1) {        /* не упакованный fixed64 */
            if (n - i < 8) return;
            u64 g = 0; for (int b = 7; b >= 0; b--) g = g << 8 | p[i + (size_t)b];
            folder_add_gid(f, g);
            i += 8;
        } else if ((fn == 2 || fn == 3 || fn == 4) && wt == 2) {
            if (!pb_varint(p, n, &i, &l) || l > n - i) return;
            u64 v = 0; const u8 *s = NULL; size_t sl = 0;
            if (fn == 2 && pb_wrapped(p + i, (size_t)l, &v, NULL, NULL)) f->id = (i64)v;
            if (fn == 3 && pb_wrapped(p + i, (size_t)l, NULL, &s, &sl) && sl && sl < 200) {
                char nm[201]; memcpy(nm, s, sl); nm[sl] = 0;
                char *t = san(nm, false);
                if (strlen(t) > 64) { size_t k = 64; while (k && ((unsigned char)t[k] & 0xC0) == 0x80) k--; t[k] = 0; }
                free(f->name); f->name = t;
            }
            if (fn == 4 && pb_wrapped(p + i, (size_t)l, &v, NULL, NULL)) { f->color = (u32)v & 0xFFFFFFu; f->has_color = true; }
            i += (size_t)l;
        } else if (!pb_skip(p, n, &i, wt)) return;
    }
}
static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
/* user_settings_proto (base64 PreloadedUserSettings): поле 14 guild_folders -> поле 1 folders */
static void folders_proto(const char *b64, size_t L) {
    if (!L || L > (512u << 10)) return;
    u8 *p = xmalloc(L * 3 / 4 + 4);
    size_t n = 0;
    u32 acc = 0; int bits = 0;
    for (size_t k = 0; k < L; k++) {
        int v = b64v(b64[k]);
        if (v < 0) continue;
        acc = acc << 6 | (u32)v; bits += 6;
        if (bits >= 8) { bits -= 8; p[n++] = (u8)(acc >> bits); }
    }
    size_t i = 0;
    while (i < n) {
        u64 key, l;
        if (!pb_varint(p, n, &i, &key)) break;
        if ((key >> 3) == 14 && (key & 7) == 2) {
            if (!pb_varint(p, n, &i, &l) || l > n - i) break;
            size_t j = i, e = i + (size_t)l;
            while (j < e) {
                u64 k2, l2;
                if (!pb_varint(p, e, &j, &k2)) break;
                if ((k2 >> 3) == 1 && (k2 & 7) == 2) {
                    if (!pb_varint(p, e, &j, &l2) || l2 > e - j) break;
                    pb_folder(p + j, (size_t)l2);
                    j += (size_t)l2;
                } else if (!pb_skip(p, e, &j, (int)(k2 & 7))) break;
            }
            i = e;
            continue;
        }
        if ((key >> 3) == 11 && (key & 7) == 2) {     /* №104: status -> custom_status */
            if (!pb_varint(p, n, &i, &l) || l > n - i) break;
            cst_from_proto(p + i, (size_t)l);
            i += (size_t)l;
            continue;
        }
        if (!pb_skip(p, n, &i, (int)(key & 7))) break;
    }
    free(p);
    fgs_free();                                 /* множество — только на время разбора */
}
static void folders_ready(const cJSON *d) {
    /* обычно папки уже разобраны в ready_slim (folders_slim: protobuf не доходит до cJSON) */
    const char *pr = jstr(d, "user_settings_proto");
    const cJSON *uf = jget(jget(d, "user_settings"), "guild_folders");
    if (pr) { folders_free(); folders_proto(pr, strlen(pr)); }
    if (!nfd && uf) { folders_free(); folders_json(uf); }
}
/* ready_slim: строка user_settings_proto [a, b) — разобрать папки и выбросить */
static void folders_slim(const char *a, size_t n) { folders_free(); folders_proto(a, n); }
static void folders_event(const cJSON *d) {   /* USER_SETTINGS_PROTO_UPDATE: settings {type 1, proto} */
    const cJSON *s = jget(d, "settings");
    if (jint(s, "type", 0) != 1 || !jstr(s, "proto")) return;
    folders_free();
    folders_proto(jstr(s, "proto"), strlen(jstr(s, "proto")));
    ui_dirty();
}
void folders_test_proto(const char *b64);
void folders_test_proto(const char *b64) { folders_free(); folders_proto(b64, strlen(b64)); }

/* ---------- №119 слова-тревоги ---------- */
static char aw_raw[16 * 33 + 16];               /* как ввели (для настроек) */
static char aw[16][33];                         /* в нижнем регистре */
static int naw;
static u32 aw_lower(u32 c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;      /* А-Я */
    if (c == 0x401) return 0x451;                      /* Ё */
    return c;
}
static void aw_fold(const char *s, size_t n, char *out, size_t on) {
    size_t k = 0;
    const char *e = s + n;
    while (s < e && *s) {
        u32 c;
        int l = utf8_dec(s, &c);
        s += l;
        char b[4];
        int w = utf8_enc(aw_lower(c), b);
        if (k + (size_t)w >= on) break;
        memcpy(out + k, b, (size_t)w);
        k += (size_t)w;
    }
    out[k] = 0;
}
void alert_words_set(const char *w) {
    naw = 0;
    aw_raw[0] = 0;
    if (!w) return;
    char *t = san(w, false);
    snprintf(aw_raw, sizeof aw_raw, "%s", t);
    for (char *p = t; *p && naw < 16; ) {
        while (*p == ',' || *p == ' ') p++;
        char *q = p;
        while (*q && *q != ',') q++;
        char *e = q;
        while (e > p && e[-1] == ' ') e--;
        if (e > p && (size_t)(e - p) <= 32) aw_fold(p, (size_t)(e - p), aw[naw++], sizeof aw[0]);
        p = q;
    }
    wipe(t, strlen(t));
    free(t);
}
const char *alert_words(void) { return aw_raw; }
static bool aw_wordch(u32 c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || (c >= 0x430 && c <= 0x44F) || c == 0x451; }
static bool alert_text(const char *s) {
    if (!naw || !s) return false;
    char low[4096];
    aw_fold(s, strlen(s), low, sizeof low);
    for (int i = 0; i < naw; i++) {
        size_t wl = strlen(aw[i]);
        for (const char *p = strstr(low, aw[i]); p; p = strstr(p + 1, aw[i])) {
            u32 a = 0, b = 0;
            if (p > low) { const char *q = p - 1; while (q > low && ((unsigned char)*q & 0xC0) == 0x80) q--; utf8_dec(q, &a); }
            if (p[wl]) utf8_dec(p + wl, &b);
            if (!aw_wordch(a) && !aw_wordch(b)) return true;   /* целое слово */
        }
    }
    return false;
}
bool msg_alert(const Message *m) { return m && m->author_id != S.me_id && alert_text(m->content); }

/* №4: баннер сервера не грузим (картинка — до сотен КБ): полоса цвета, стабильного для хэша баннера */
static u32 banner_color(const char *h) {
    if (!h || !*h) return 0;
    static const u32 P[] = { 0x5865F2, 0x3BA55C, 0xFAA61A, 0xED4245, 0xEB459E, 0x9B59B6, 0x1ABC9C, 0x607D8B };
    u32 x = 2166136261u;
    for (; *h; h++) x = (x ^ (u8)*h) * 16777619u;
    return P[x % 8];
}
/* ---------- №120 фильтр слов, №121 фокус, №125 стример ---------- */
static char fw_raw[16 * 33 + 16];
static char fw[16][33];
static int nfw;
void filter_words_set(const char *w) {
    nfw = 0;
    fw_raw[0] = 0;
    if (!w) return;
    char *t = san(w, false);
    snprintf(fw_raw, sizeof fw_raw, "%s", t);
    for (char *p = t; *p && nfw < 16; ) {
        while (*p == ',' || *p == ' ') p++;
        char *q = p;
        while (*q && *q != ',') q++;
        char *e = q;
        while (e > p && e[-1] == ' ') e--;
        if (e > p && (size_t)(e - p) <= 32) aw_fold(p, (size_t)(e - p), fw[nfw++], sizeof fw[0]);
        p = q;
    }
    wipe(t, strlen(t));
    free(t);
}
const char *filter_words(void) { return fw_raw; }
static bool words_in(const char *s, char (*ws)[33], int n) {
    if (!n || !s) return false;
    char low[4096];
    aw_fold(s, strlen(s), low, sizeof low);
    for (int i = 0; i < n; i++) {
        size_t wl = strlen(ws[i]);
        for (const char *p = strstr(low, ws[i]); p; p = strstr(p + 1, ws[i])) {
            u32 a = 0, b = 0;
            if (p > low) { const char *q = p - 1; while (q > low && ((unsigned char)*q & 0xC0) == 0x80) q--; utf8_dec(q, &a); }
            if (p[wl]) utf8_dec(p + wl, &b);
            if (!aw_wordch(a) && !aw_wordch(b)) return true;
        }
    }
    return false;
}
bool msg_filtered(const Message *m) { return m && m->author_id != S.me_id && words_in(m->content, fw, nfw); }
static i64 focus_end;
static bool focus_was;
void focus_set(int minutes) {
    focus_end = minutes > 0 ? wall_ms() / 1000 + (i64)(minutes > 24 * 60 ? 24 * 60 : minutes) * 60 : 0;
    focus_was = focus_end != 0;
    ui_dirty();
}
i64 focus_until(void) { return focus_end > wall_ms() / 1000 ? focus_end : 0; }
static i64 focus_next_wake(void) {
    if (!focus_was) return INT64_MAX;
    i64 w = wall_ms();
    if (focus_end * 1000 <= w) { focus_was = false; focus_end = 0; ui_dirty(); ui_notice(ui_tr("режим фокуса закончился")); return INT64_MAX; }
    return now_ms() + (focus_end * 1000 - w) + 50;
}
static bool streamer;
void streamer_set(bool on) { streamer = on; ui_dirty(); }
bool streamer_on(void) { return streamer; }

/* ---------- №151 статус голосового канала ---------- */
#define VCS_MAX 64
static struct { u64 cid; char *s; } VCS[VCS_MAX];
static void vcs_clear(void) {                  /* ревью 14 М3: READY / смена аккаунта */
    for (int i = 0; i < VCS_MAX; i++) { free(VCS[i].s); VCS[i].s = NULL; VCS[i].cid = 0; }
}
void vcs_clear_hook(void) { vcs_clear(); }
static void vcs_put(u64 cid, const char *s) {
    int k = -1, fr = -1;
    for (int i = 0; i < VCS_MAX; i++) { if (VCS[i].cid == cid) k = i; else if (!VCS[i].cid && fr < 0) fr = i; }
    if (!s || !*s) { if (k >= 0) { free(VCS[k].s); VCS[k].cid = 0; VCS[k].s = NULL; } return; }
    if (k < 0) k = fr >= 0 ? fr : (int)(cid % VCS_MAX);
    free(VCS[k].s);
    char *t = san(s, false);
    utf8_trunc(t, 2000);                        /* 500 символов Discord <= 2000 байт */
    VCS[k].cid = cid;
    VCS[k].s = t;
}
const char *voice_channel_status(u64 cid) {
    for (int i = 0; i < VCS_MAX; i++) if (VCS[i].cid == cid && VCS[i].s) return VCS[i].s;
    return "";
}
bool voice_channel_status_set(u64 cid, const char *text, char *err, size_t errn) {
    Channel *c = chan_find(cid);
    if (!c || !c->guild_id || (c->type != CH_VOICE && c->type != CH_STAGE)) { snprintf(err, errn, ui_tr("статус — только у голосового канала")); return false; }
    if (text && u8len(text) > 500) { snprintf(err, errn, ui_tr("статус: не больше 500 символов")); return false; }
    if (O.demo) { vcs_put(cid, text); ui_dirty(); return true; }
    Buf b = {0};
    buf_puts(&b, "{\"status\":");
    buf_json_str(&b, text ? text : "");
    buf_puts(&b, "}");
    char path[80];
    snprintf(path, sizeof path, "/channels/%llu/voice-status", (unsigned long long)cid);
    http_async("PUT", path, b.p, on_write_done, (void *)ui_tr("статус канала"));   /* модель — по VOICE_CHANNEL_STATUS_UPDATE */
    buf_free(&b);
    return true;
}
static void vcs_event(const char *t, const cJSON *d) {
    if (!strcmp(t, "VOICE_CHANNEL_STATUS_UPDATE")) vcs_put(jid(d, "id"), jstr(d, "status"));
    else {                                      /* CHANNEL_INFO: {guild_id, channels:[{id, status}]} */
        const cJSON *x;
        cJSON_ArrayForEach(x, jget(d, "channels")) vcs_put(jid(x, "id"), jstr(x, "status"));
    }
    ui_dirty();
}

/* ---------- №90–92 ветки ---------- */
bool chan_is_thread(const Channel *c) { return c && (c->type == CH_THREAD || c->type == CH_PRIV_THREAD || c->type == CH_NEWS_THREAD); }
static void thread_flags(Channel *c, const cJSON *o) {
    if (!chan_is_thread(c)) return;
    const cJSON *md = jget(o, "thread_metadata");
    u8 f = c->tflags & TF_JOINED;
    if (jget(o, "member")) f |= TF_JOINED;       /* объект участника — я в ветке */
    if (md) {
        f = (u8)(f & ~(TF_ARCHIVED | TF_LOCKED));
        if (jbool(md, "archived")) f |= TF_ARCHIVED;
        if (jbool(md, "locked")) f |= TF_LOCKED;
    } else f |= c->tflags & (TF_ARCHIVED | TF_LOCKED);
    c->tflags = f;
}
static void thread_upsert(const cJSON *o, u64 gid) {
    u64 id = jid(o, "id");
    Channel *c = chan_find(id);
    u8 keep = c ? c->tflags : 0;
    chan_upsert(o, gid);
    c = chan_find(id);
    if (!c) return;
    c->tflags = keep;
    thread_flags(c, o);
    compute_perms(c);
}
/* ветки других серверов, где я не участник, не держим (кроме открытой) */
static void threads_prune(u64 keep_gid) {
    for (int i = S.nchans - 1; i >= 0; i--) {
        Channel *c = &S.chans[i];
        if (chan_is_thread(c) && c->guild_id != keep_gid && !(c->tflags & TF_JOINED) && c->id != S.open_ch) chan_remove(c->id);
    }
}
static void threads_members(const cJSON *mem) {
    const cJSON *x;
    cJSON_ArrayForEach(x, mem) {
        if (jid(x, "user_id") != S.me_id) continue;
        Channel *c = chan_find(jid(x, "id"));
        if (c) c->tflags |= TF_JOINED;
    }
}
static u64 thr_gid;
static void on_threads(Resp *r, void *ctx) {
    u64 gid = (u64)(uintptr_t)ctx;
    if (gid != thr_gid || r->status != 200) return;
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
    const cJSON *x;
    int n = 0;
    cJSON_ArrayForEach(x, jget(j, "threads")) { if (n++ >= 200) break; thread_upsert(x, gid); }
    threads_members(jget(j, "members"));
    json_done();
    ui_dirty();
}
void threads_open(u64 gid) {
    threads_prune(gid);
    if (!gid || !guild_find(gid) || thr_gid == gid) return;
    thr_gid = gid;
    if (O.demo) {
        if (gid != 2000) return;
        static const char *const dj = "{\"threads\":["
            "{\"id\":\"3101\",\"type\":11,\"name\":\"обсуждение котов\",\"parent_id\":\"3001\",\"guild_id\":\"2000\",\"last_message_id\":\"9110\",\"message_count\":12,\"thread_metadata\":{\"archived\":false,\"locked\":false}},"
            "{\"id\":\"3102\",\"type\":11,\"name\":\"идеи для сервера\",\"parent_id\":\"3001\",\"guild_id\":\"2000\",\"last_message_id\":\"9105\",\"message_count\":3,\"thread_metadata\":{\"archived\":false,\"locked\":true}}],"
            "\"members\":[{\"id\":\"3101\",\"user_id\":\"1000\"}]}";
        Resp r = { .status = 200 };
        buf_puts(&r.body, dj);
        on_threads(&r, (void *)(uintptr_t)gid);
        buf_free(&r.body);
        return;
    }
    char path[64];
    snprintf(path, sizeof path, "/guilds/%llu/threads/active", (unsigned long long)gid);
    http_async("GET", path, NULL, on_threads, (void *)(uintptr_t)gid);
}
int chan_threads(u64 parent, const Channel **out, int max) {
    int n = 0;
    for (int i = 0; i < S.nchans && n < max; i++) {
        const Channel *c = &S.chans[i];
        if (!chan_is_thread(c) || c->parent_id != parent || !c->visible) continue;
        int k = n++;                            /* вставка: свежие (больший last_msg / id) сверху */
        u64 key = c->last_msg > c->id ? c->last_msg : c->id;
        while (k > 0) {
            u64 pk = out[k - 1]->last_msg > out[k - 1]->id ? out[k - 1]->last_msg : out[k - 1]->id;
            if (pk >= key) break;
            out[k] = out[k - 1];
            k--;
        }
        out[k] = c;
    }
    return n;
}
static void on_thread_made(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status >= 200 && r->status < 300) {
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        if (jid(j, "id")) { thread_upsert(j, 0); Channel *c = chan_find(jid(j, "id")); if (c) c->tflags |= TF_JOINED; }
        json_done();
        ui_notice(ui_tr("ветка создана"));
        ui_dirty();
        return;
    }
    char e[160];
    http_err(e, sizeof e, ui_tr("ветка не создана"), r->status);
    ui_notice("%s", e);
}
bool thread_create(u64 cid, u64 mid, const char *name, char *err, size_t errn) {
    Channel *c = chan_find(cid);
    if (!c || !c->guild_id || (c->type != CH_TEXT && c->type != CH_NEWS)) { snprintf(err, errn, ui_tr("ветку можно создать в текстовом канале сервера")); return false; }
    if (!name_ok(name, 100, err, errn)) return false;
    if (O.demo) {
        static u64 next = 3190;
        char j[400];
        Buf nb = {0};
        buf_json_str(&nb, name);
        snprintf(j, sizeof j, "{\"id\":\"%llu\",\"type\":11,\"name\":%s,\"parent_id\":\"%llu\",\"guild_id\":\"%llu\",\"thread_metadata\":{},\"member\":{}}",
                 (unsigned long long)next++, nb.p, (unsigned long long)cid, (unsigned long long)c->guild_id);
        buf_free(&nb);
        cJSON *o = json_parse(j, strlen(j));
        thread_upsert(o, c->guild_id);
        json_done();
        ui_dirty();
        return true;
    }
    Buf b = {0};
    buf_puts(&b, "{\"name\":");
    buf_json_str(&b, name);
    buf_puts(&b, mid ? ",\"auto_archive_duration\":1440}" : ",\"type\":11,\"auto_archive_duration\":1440}");
    char path[96];
    if (mid) snprintf(path, sizeof path, "/channels/%llu/messages/%llu/threads", (unsigned long long)cid, (unsigned long long)mid);
    else snprintf(path, sizeof path, "/channels/%llu/threads", (unsigned long long)cid);
    http_async("POST", path, b.p, on_thread_made, NULL);
    buf_free(&b);
    return true;
}
static void on_thread_join(Resp *r, void *ctx) {
    u64 v = (u64)(uintptr_t)ctx;
    if (r->status >= 200 && r->status < 300) return;   /* подтвердит THREAD_MEMBERS_UPDATE */
    Channel *c = chan_find(v >> 1);
    if (c) c->tflags ^= TF_JOINED;              /* откат */
    char e[160];
    http_err(e, sizeof e, (v & 1) ? ui_tr("не удалось вступить в ветку") : ui_tr("не удалось выйти из ветки"), r->status);
    ui_notice("%s", e);
    ui_dirty();
}
bool thread_member(u64 tid, bool join, char *err, size_t errn) {
    Channel *c = chan_find(tid);
    if (!chan_is_thread(c)) { snprintf(err, errn, ui_tr("ветка не найдена")); return false; }
    if (!!(c->tflags & TF_JOINED) == join) return true;
    c->tflags ^= TF_JOINED;                     /* сразу в модели */
    ui_dirty();
    if (O.demo) return true;
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/thread-members/@me", (unsigned long long)tid);
    http_async(join ? "PUT" : "DELETE", path, NULL, on_thread_join, (void *)(uintptr_t)((tid << 1) | (join ? 1 : 0)));
    return true;
}
static void thread_event(const char *t, const cJSON *d) {
    if (!strcmp(t, "THREAD_CREATE") || !strcmp(t, "THREAD_UPDATE")) {
        u64 gid = jid(d, "guild_id");
        Channel *ex = chan_find(jid(d, "id"));
        bool mine = jid(d, "owner_id") == S.me_id && jbool(d, "newly_created");
        if (ex || gid == thr_gid || mine || jget(d, "member")) { thread_upsert(d, gid); if (mine) { Channel *c = chan_find(jid(d, "id")); if (c) c->tflags |= TF_JOINED; } }
    } else if (!strcmp(t, "THREAD_DELETE")) {
        u64 id = jid(d, "id");
        Channel *c = chan_find(id);
        if (id != S.open_ch) chan_remove(id);
        else if (c) { c->tflags |= TF_DELETED; c->can_send = false; ui_notice(ui_tr("эту ветку удалили")); }   /* ревью 11 М5 */
    } else if (!strcmp(t, "THREAD_LIST_SYNC")) {
        u64 gid = jid(d, "guild_id");
        if (gid != thr_gid) return;
        const cJSON *x;
        cJSON_ArrayForEach(x, jget(d, "threads")) thread_upsert(x, gid);
        threads_members(jget(d, "members"));
    } else if (!strcmp(t, "THREAD_MEMBER_UPDATE")) {
        Channel *c = chan_find(jid(d, "id"));
        if (c && jid(d, "user_id") == S.me_id) c->tflags |= TF_JOINED;
    } else if (!strcmp(t, "THREAD_MEMBERS_UPDATE")) {
        Channel *c = chan_find(jid(d, "id"));
        if (!c) return;
        const cJSON *x;
        cJSON_ArrayForEach(x, jget(d, "added_members")) if (jid(x, "user_id") == S.me_id) c->tflags |= TF_JOINED;
        cJSON_ArrayForEach(x, jget(d, "removed_member_ids")) if (id_of(x) == S.me_id) c->tflags &= (u8)~TF_JOINED;
    }
    ui_dirty();
}

/* ---------- №95 форумы ---------- */
static ForumState FR;
static unsigned fr_gen;
const ForumState *forum_state(void) { return &FR; }
static void forum_posts_free(void) {
    for (int i = 0; i < FR.n; i++) { free(FR.p[i].name); free(FR.p[i].preview); }
    free(FR.p);
    FR.p = NULL;
    FR.n = 0;
}
void forum_close(void) { forum_posts_free(); memset(&FR, 0, sizeof FR); fr_gen++; }
static void forum_fill(const cJSON *j) {
    forum_posts_free();
    FR.total = (int)jint(j, "total_results", 0);
    FR.more = jbool(j, "has_more");
    int n = cJSON_GetArraySize(jget(j, "threads"));
    if (n > FORUM_PAGE) n = FORUM_PAGE;
    FR.p = n > 0 ? xcalloc((size_t)n, sizeof(ForumPost)) : NULL;
    const cJSON *x;
    cJSON_ArrayForEach(x, jget(j, "threads")) {
        if (FR.n >= n) break;
        ForumPost *p = &FR.p[FR.n];
        p->id = jid(x, "id");
        if (!p->id) continue;
        p->owner_id = jid(x, "owner_id");
        p->last_msg = jid(x, "last_message_id");
        p->msg_count = (int)jint(x, "message_count", 0);
        p->archived = jbool(jget(x, "thread_metadata"), "archived");
        p->pinned = ((u32)jint(x, "flags", 0) & 2) != 0;
        char *nm = san(jstr(x, "name"), false);
        if (strlen(nm) > 100) { size_t k = 100; while (k && ((unsigned char)nm[k] & 0xC0) == 0x80) k--; nm[k] = 0; }
        p->name = nm;
        FR.n++;
    }
    cJSON_ArrayForEach(x, jget(j, "members")) {
        if (jid(x, "user_id") != S.me_id) continue;
        for (int i = 0; i < FR.n; i++) if (FR.p[i].id == jid(x, "id")) FR.p[i].joined = true;
    }
    cJSON_ArrayForEach(x, jget(j, "first_messages")) {   /* превью: первое сообщение поста (id == id ветки) */
        u64 cid = jid(x, "channel_id");
        for (int i = 0; i < FR.n; i++) if (FR.p[i].id == cid && !FR.p[i].preview) {
            char *t = san(jstr(x, "content"), false);
            if (strlen(t) > 200) { size_t k = 200; while (k && ((unsigned char)t[k] & 0xC0) == 0x80) k--; t[k] = 0; }
            FR.p[i].preview = t;
            user_put_obj(jget(x, "author"));
        }
    }
}
static void on_forum(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != fr_gen) return;
    FR.busy = false;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len); forum_fill(j); json_done(); }
    else if (r->status == 202) snprintf(FR.err, sizeof FR.err, ui_tr("Discord готовит список — откройте ещё раз через пару секунд"));
    else http_err(FR.err, sizeof FR.err, ui_tr("посты форума"), r->status);
    ui_dirty();
}
void forum_open(u64 cid, int page, int sort) {
    Channel *c = chan_find(cid);
    forum_close();
    if (!c || (c->type != CH_FORUM && c->type != CH_MEDIA) || page < 0 || page > 400) return;
    FR.cid = cid; FR.page = page; FR.sort = sort ? 1 : 0;
    if (O.demo) {
        Buf b = {0};
        buf_puts(&b, "{\"total_results\":3,\"has_more\":false,\"threads\":[");
        static const char *const nm[] = { "Как настроить звук?", "Покажите свои рабочие места", "Правила раздела" };
        for (int i = 0; i < 3; i++)
            buf_printf(&b, "%s{\"id\":\"%d\",\"type\":11,\"parent_id\":\"%llu\",\"owner_id\":\"%d\",\"name\":\"%s\",\"message_count\":2,\"last_message_id\":\"%llu\",\"flags\":%d}",
                       i ? "," : "", 3201 + i, (unsigned long long)cid, 1001 + i, nm[i], 1568943164620800000ULL + (u64)i * 15099494400000ULL, i == 2 ? 2 : 0);   /* последнее — 8.11, 11:00 + i ч */
        buf_puts(&b, "],\"members\":[{\"id\":\"3201\",\"user_id\":\"1000\"}],\"first_messages\":[{\"id\":\"3201\",\"channel_id\":\"3201\",\"author\":{\"id\":\"1001\",\"username\":\"alice\"},\"content\":\"Микрофон тихий, что крутить?\"}]}");
        cJSON *j = json_parse(b.p, b.len);
        forum_fill(j);
        json_done();
        buf_free(&b);
        return;
    }
    char path[200];
    snprintf(path, sizeof path, "/channels/%llu/threads/search?sort_by=%s&sort_order=desc&limit=%d&offset=%d",
             (unsigned long long)cid, FR.sort ? "creation_time" : "last_message_time", FORUM_PAGE, page * FORUM_PAGE);
    FR.busy = true;
    http_async("GET", path, NULL, on_forum, (void *)(uintptr_t)fr_gen);
}
void forum_test_resp(long status, const char *body) {
    Resp r = { .status = status };
    buf_puts(&r.body, body);
    FR.busy = true;
    on_forum(&r, (void *)(uintptr_t)fr_gen);
    buf_free(&r.body);
}
u64 forum_post_open(int i) {
    if (i < 0 || i >= FR.n) return 0;
    Channel *f = chan_find(FR.cid);
    if (!f) return 0;
    const ForumPost *p = &FR.p[i];
    char j[300];
    snprintf(j, sizeof j, "{\"id\":\"%llu\",\"type\":11,\"parent_id\":\"%llu\",\"guild_id\":\"%llu\",\"last_message_id\":\"%llu\",\"thread_metadata\":{\"archived\":%s}}",
             (unsigned long long)p->id, (unsigned long long)FR.cid, (unsigned long long)f->guild_id, (unsigned long long)p->last_msg, p->archived ? "true" : "false");
    cJSON *o = json_parse(j, strlen(j));
    thread_upsert(o, f->guild_id);
    json_done();
    Channel *c = chan_find(p->id);
    if (!c) return 0;
    free(c->name);
    c->name = xstrdup(p->name ? p->name : ui_tr("пост"));
    if (p->joined) c->tflags |= TF_JOINED;
    return c->id;
}

static void embeds_free(Message *m) {
    for (int i = 0; i < m->nemb; i++) free(m->emb[i].s);
    free(m->emb);
    m->emb = NULL;
    m->nemb = 0;
}

/* ---------- №103/104 статус со сроком, текстовый статус ---------- */
static i64 ps_until;                            /* unix-с: вернуться в «в сети» */
static char cst_text[520];
static i64 cst_until;
void presence_set_for(u8 st, int minutes) {
    presence_set(st);
    ps_until = minutes > 0 ? wall_ms() / 1000 + (i64)(minutes > 7 * 24 * 60 ? 7 * 24 * 60 : minutes) * 60 : 0;
}
i64 presence_until(void) { return ps_until; }
const char *custom_status(void) { return cst_until && cst_until <= wall_ms() / 1000 ? "" : cst_text; }
i64 custom_status_until(void) { return *custom_status() ? cst_until : 0; }
static void iso_utc(i64 t, char *out, size_t n) {
    i64 days = t / 86400, sod = t % 86400, z = days + 719468, era = z / 146097, doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.000Z", (int)y, (int)m, (int)d, (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
}
bool custom_status_set(const char *text, int minutes, char *err, size_t errn) {
    if (text && u8len(text) > 128) { snprintf(err, errn, ui_tr("статус: не больше 128 символов")); return false; }
    char *t = san(text ? text : "", false);
    snprintf(cst_text, sizeof cst_text, "%s", t);
    free(t);
    cst_until = *cst_text && minutes > 0 ? wall_ms() / 1000 + (i64)(minutes > 7 * 24 * 60 ? 7 * 24 * 60 : minutes) * 60 : 0;
    ui_dirty();
    if (O.demo) return true;
    Buf b = {0};
    if (!*cst_text) buf_puts(&b, "{\"custom_status\":null}");
    else {
        buf_puts(&b, "{\"custom_status\":{\"text\":");
        buf_json_str(&b, cst_text);
        if (cst_until) { char iso[40]; iso_utc(cst_until, iso, sizeof iso); buf_printf(&b, ",\"expires_at\":\"%s\"", iso); }
        buf_puts(&b, "}}");
    }
    http_async("PATCH", "/users/@me/settings", b.p, on_op, (void *)ui_tr("текстовый статус"));
    buf_free(&b);
    if (ps_allowed()) { ps_send(my_ps ? my_ps : PS_ONLINE); ps_pend = 255; } else ps_pend = my_ps ? my_ps : PS_ONLINE;   /* сразу в присутствие */
    return true;
}
static i64 status_next_wake(void) {             /* сроки статуса и текстового статуса */
    i64 w = wall_ms() / 1000, t = INT64_MAX;
    if (ps_until) {
        if (ps_until <= w) { ps_until = 0; presence_set(PS_ONLINE); ui_notice(ui_tr("статус: срок истёк — снова «в сети»")); }
        else t = ps_until;
    }
    if (cst_until && *cst_text) {
        if (cst_until <= w) { cst_text[0] = 0; cst_until = 0; ui_dirty(); if (!O.demo) { if (ps_allowed()) { ps_send(my_ps); ps_pend = 255; } else ps_pend = my_ps; } }
        else if (cst_until < t) t = cst_until;
    }
    return t == INT64_MAX ? INT64_MAX : now_ms() + (t * 1000 - wall_ms()) + 50;
}
/* user_settings_proto: поле 11 (status) -> 2 (custom_status): 1 текст, 4 expires_at_ms (fixed64) */
static void cst_from_proto(const u8 *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        u64 key, l;
        if (!pb_varint(p, n, &i, &key)) return;
        if ((key >> 3) == 2 && (key & 7) == 2) {
            if (!pb_varint(p, n, &i, &l) || l > n - i) return;
            const u8 *c = p + i;
            size_t cn = (size_t)l, j = 0;
            char txt[520] = "";
            u64 exp = 0;
            while (j < cn) {
                u64 k2, l2;
                if (!pb_varint(c, cn, &j, &k2)) break;
                if ((k2 >> 3) == 1 && (k2 & 7) == 2) {
                    if (!pb_varint(c, cn, &j, &l2) || l2 > cn - j) break;
                    size_t m = l2 < sizeof txt - 1 ? (size_t)l2 : sizeof txt - 1;
                    memcpy(txt, c + j, m); txt[m] = 0;
                    j += (size_t)l2;
                } else if ((k2 >> 3) == 4 && (k2 & 7) == 1) {
                    if (cn - j < 8) break;
                    for (int b = 7; b >= 0; b--) exp = exp << 8 | c[j + (size_t)b];
                    j += 8;
                } else if (!pb_skip(c, cn, &j, (int)(k2 & 7))) break;
            }
            char *t = san(txt, false);
            utf8_trunc(t, 512);
            snprintf(cst_text, sizeof cst_text, "%s", t);
            free(t);
            cst_until = exp ? (i64)(exp / 1000) : 0;
            i += (size_t)l;
        } else if (!pb_skip(p, n, &i, (int)(key & 7))) return;
    }
}

/* ---------- №108 игнор (локально), №100 заметка ---------- */
static u64 IGN[64];
static int nign;
void ignore_set(u64 uid, bool on) {
    for (int i = 0; i < nign; i++) if (IGN[i] == uid) { if (!on) IGN[i] = IGN[--nign]; ui_dirty(); return; }
    if (on && uid && uid != S.me_id && nign < 64) IGN[nign++] = uid;
    ui_dirty();
}
bool ignored(u64 uid) { for (int i = 0; i < nign; i++) if (IGN[i] == uid) return true; return false; }
static NoteState NT;
static unsigned nt_gen;
const NoteState *note_state(void) { return &NT; }
void note_close(void) { wipe(&NT, sizeof NT); nt_gen++; }
static void on_note(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != nt_gen) return;
    NT.busy = false;
    if (r->status == 200) {
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        char *t = san(jstr(j, "note") ? jstr(j, "note") : "", true);
        snprintf(NT.text, sizeof NT.text, "%s", t);
        utf8_trunc(NT.text, sizeof NT.text - 1);
        free(t);
        json_done();
    } else if (r->status != 404) http_err(NT.err, sizeof NT.err, ui_tr("заметка"), r->status);   /* 404 — заметки нет */
    ui_dirty();
}
void note_open(u64 uid) {
    note_close();
    if (!uid || uid == S.me_id) return;
    NT.uid = uid;
    if (O.demo) { if (uid == 1001) snprintf(NT.text, sizeof NT.text, ui_tr("Знакомы по учёбе, любит котов")); return; }
    char path[80];
    snprintf(path, sizeof path, "/users/@me/notes/%llu", (unsigned long long)uid);
    NT.busy = true;
    http_async("GET", path, NULL, on_note, (void *)(uintptr_t)nt_gen);
}
bool note_set(u64 uid, const char *text, char *err, size_t errn) {
    if (!uid || uid == S.me_id) { snprintf(err, errn, ui_tr("заметка — о другом человеке")); return false; }
    if (text && u8len(text) > 256) { snprintf(err, errn, ui_tr("заметка: не больше 256 символов")); return false; }
    if (NT.uid == uid) { char *t = san(text ? text : "", true); snprintf(NT.text, sizeof NT.text, "%s", t); free(t); }
    ui_dirty();
    if (O.demo) return true;
    Buf b = {0};
    buf_puts(&b, "{\"note\":");
    buf_json_str(&b, text ? text : "");
    buf_puts(&b, "}");
    char path[80];
    snprintf(path, sizeof path, "/users/@me/notes/%llu", (unsigned long long)uid);
    http_async("PUT", path, b.p, on_op, (void *)ui_tr("заметка"));
    buf_free(&b);
    return true;
}

/* ---------- №128 правила сервера ---------- */
static RulesState RU;
static unsigned ru_gen;
const RulesState *guild_rules_state(void) { return &RU; }
void guild_rules_close(void) {
    free(RU.desc);
    for (int i = 0; i < RU.n; i++) free(RU.rules[i]);
    free(RU.rules);
    free(RU.version);
    memset(&RU, 0, sizeof RU);
    ru_gen++;
}
static void rules_fill(const cJSON *j) {
    RU.desc = san(jstr(j, "description") ? jstr(j, "description") : "", true);
    utf8_trunc(RU.desc, 1000);
    const char *ver = jstr(j, "version");
    RU.version = xstrdup(ver && strlen(ver) < 64 ? ver : "");
    const cJSON *f, *v;
    cJSON_ArrayForEach(f, jget(j, "form_fields")) {
        if (!jstr(f, "field_type") || strcmp(jstr(f, "field_type"), "TERMS")) continue;
        cJSON_ArrayForEach(v, jget(f, "values")) {
            if (RU.n >= 50 || !cJSON_IsString(v)) break;
            RU.rules = xrealloc(RU.rules, (size_t)(RU.n + 1) * sizeof(char *));
            char *t = san(v->valuestring, true);
            utf8_trunc(t, 600);
            RU.rules[RU.n++] = t;
        }
    }
}
static void on_rules(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != ru_gen) return;
    RU.busy = false;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len); rules_fill(j); json_done(); }
    else http_err(RU.err, sizeof RU.err, ui_tr("правила сервера"), r->status);
    ui_dirty();
}
void guild_rules_open(u64 gid) {
    guild_rules_close();
    if (!guild_find(gid)) return;
    RU.gid = gid;
    if (O.demo) {
        static const char *const dj = "{\"version\":\"2026-11-01T00:00:00.000000+00:00\",\"description\":\"Добро пожаловать в Уютный уголок!\","
            "\"form_fields\":[{\"field_type\":\"TERMS\",\"label\":\"Прочитайте правила\",\"values\":[\"Будьте вежливы\",\"Без спама и рекламы\",\"Спойлеры — под ||спойлер||\"]}]}";
        cJSON *j = json_parse(dj, strlen(dj));
        rules_fill(j);
        json_done();
        return;
    }
    char path[96];
    snprintf(path, sizeof path, "/guilds/%llu/member-verification?with_guild=false", (unsigned long long)gid);
    RU.busy = true;
    http_async("GET", path, NULL, on_rules, (void *)(uintptr_t)ru_gen);
}
static void on_rules_ok(Resp *r, void *ctx) {
    u64 gid = (u64)(uintptr_t)ctx;
    if (r->status >= 200 && r->status < 300) { Guild *g = guild_find(gid); if (g) g->pending = false; ui_notice(ui_tr("правила приняты")); ui_dirty(); return; }
    char e[160];
    http_err(e, sizeof e, ui_tr("правила не приняты"), r->status);
    ui_notice("%s", e);
}
bool guild_rules_accept(u64 gid, char *err, size_t errn) {
    if (RU.gid != gid || !RU.version) { snprintf(err, errn, ui_tr("сначала откройте правила")); return false; }
    if (O.demo) { Guild *g = guild_find(gid); if (g) g->pending = false; ui_dirty(); return true; }
    Buf b = {0};
    buf_puts(&b, "{\"version\":");
    buf_json_str(&b, RU.version);
    buf_puts(&b, ",\"form_fields\":[{\"field_type\":\"TERMS\",\"label\":\"\",\"values\":[");
    for (int i = 0; i < RU.n; i++) { if (i) buf_puts(&b, ","); buf_json_str(&b, RU.rules[i]); }
    buf_puts(&b, "],\"required\":true,\"response\":true}]}");
    char path[80];
    snprintf(path, sizeof path, "/guilds/%llu/requests/@me", (unsigned long long)gid);
    http_async("PUT", path, b.p, on_rules_ok, (void *)(uintptr_t)gid);
    buf_free(&b);
    return true;
}

/* ---------- №123/124 уведомления ---------- */
static int ntf_mode = NOTIFY_PLAIN;
void notify_set(int mode) { ntf_mode = mode < NOTIFY_OFF || mode > NOTIFY_TEXT ? NOTIFY_PLAIN : mode; }
int notify_mode(void) { return ntf_mode; }
__attribute__((weak)) void plat_notify(const char *title, const char *text) { (void)title; (void)text; }
void (*g_ui_drop)(const char *path);           /* ui.c ставит при старте: перетащили файл в окно */
void (*g_notify_hook)(const char *title, const char *text);   /* selftest: перехват уведомлений */
static void notify_msg(const cJSON *d, const Channel *c) {
    if (ntf_mode == NOTIFY_OFF || (O.demo && !g_notify_hook) || !c || c->muted) return;
    void (*show)(const char *, const char *) = g_notify_hook ? g_notify_hook : plat_notify;
    if (c->id == S.open_ch && win_focused()) return;   /* и так перед глазами */
    Guild *g = c->guild_id ? guild_find(c->guild_id) : NULL;
    if (g && g->muted) return;
    char title[160], who[80], where[96];
    const cJSON *a = jget(d, "author");
    char *an = san(display_name(a), false);
    snprintf(who, sizeof who, "%s", an);
    utf8_trunc(who, 60);
    free(an);
    chan_label(c, where, sizeof where);
    if (c->guild_id) snprintf(title, sizeof title, ui_tr("%s в #%s"), who, where);
    else snprintf(title, sizeof title, "%s", who);
    /* У-2/С1: Windows хранит уведомления на диске (wpndatabase.db), демоны Linux — историю. По умолчанию
     * (NOTIFY_PLAIN) — без имён, каналов и текста; имена и текст — только в NOTIFY_TEXT (с предупреждением в UI). */
    if (ntf_mode == NOTIFY_PLAIN || streamer_on()) {
        show("Astra DS", c->guild_id ? ui_tr("новое упоминание") : ui_tr("новое сообщение"));
        return;
    }
    if (ntf_mode == NOTIFY_TEXT) {
        char *t = san(jstr(d, "content") ? jstr(d, "content") : "", false);
        utf8_trunc(t, 200);
        show(title, *t ? t : ui_tr("вложение"));
        wipe(t, strlen(t));
        free(t);
    }
}

/* ---------- №93/94/96/97 ветки: архив, правка, новый пост форума, участники ---------- */
void threads_archived_open(u64 cid) {
    Channel *c = chan_find(cid);
    forum_close();
    if (!c || !c->guild_id) return;
    FR.cid = cid;
    if (O.demo) {
        static const char *const dj = "{\"has_more\":false,\"threads\":[{\"id\":\"3190\",\"type\":11,\"name\":\"старое обсуждение\","
            "\"owner_id\":\"1002\",\"message_count\":40,\"last_message_id\":\"9001\",\"thread_metadata\":{\"archived\":true}}],\"members\":[]}";
        cJSON *j = json_parse(dj, strlen(dj));
        forum_fill(j);
        json_done();
        return;
    }
    char path[120];
    snprintf(path, sizeof path, "/channels/%llu/threads/archived/public?limit=%d", (unsigned long long)cid, FORUM_PAGE);
    FR.busy = true;
    http_async("GET", path, NULL, on_forum, (void *)(uintptr_t)fr_gen);
}
bool thread_edit(u64 tid, const char *name, int archived, int locked, char *err, size_t errn) {
    Channel *c = chan_find(tid);
    if (!chan_is_thread(c)) { snprintf(err, errn, ui_tr("ветка не найдена")); return false; }
    if (name && !name_ok(name, 100, err, errn)) return false;
    if (!name && archived < 0 && locked < 0) return true;
    if (O.demo) {
        if (name) { free(c->name); c->name = san(name, false); }
        if (archived >= 0) c->tflags = (u8)(archived ? c->tflags | TF_ARCHIVED : c->tflags & ~TF_ARCHIVED);
        if (locked >= 0) c->tflags = (u8)(locked ? c->tflags | TF_LOCKED : c->tflags & ~TF_LOCKED);
        compute_perms(c);
        ui_dirty();
        return true;
    }
    Buf b = {0};
    buf_puts(&b, "{");
    bool sep = false;
    if (name) { buf_puts(&b, "\"name\":"); buf_json_str(&b, name); sep = true; }
    if (archived >= 0) { buf_printf(&b, "%s\"archived\":%s", sep ? "," : "", archived ? "true" : "false"); sep = true; }
    if (locked >= 0) buf_printf(&b, "%s\"locked\":%s", sep ? "," : "", locked ? "true" : "false");
    buf_puts(&b, "}");
    char path[64];
    snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)tid);
    http_async("PATCH", path, b.p, on_op, (void *)ui_tr("ветка"));     /* модель — по THREAD_UPDATE */
    buf_free(&b);
    return true;
}
bool forum_post_create(u64 cid, const char *title, const char *text, char *err, size_t errn) {
    Channel *c = chan_find(cid);
    if (!c || (c->type != CH_FORUM && c->type != CH_MEDIA)) { snprintf(err, errn, ui_tr("пост создаётся в форуме")); return false; }
    if (!name_ok(title, 100, err, errn)) return false;
    if (!text || !*text || u8len(text) > 2000) { snprintf(err, errn, ui_tr("первое сообщение: от 1 до 2000 символов")); return false; }
    if (O.demo) {
        static u64 next = 3290;
        char j[400];
        Buf nb = {0};
        buf_json_str(&nb, title);
        snprintf(j, sizeof j, "{\"id\":\"%llu\",\"type\":11,\"name\":%s,\"parent_id\":\"%llu\",\"guild_id\":\"%llu\",\"owner_id\":\"%llu\",\"thread_metadata\":{},\"member\":{}}",
                 (unsigned long long)next++, nb.p, (unsigned long long)cid, (unsigned long long)c->guild_id, (unsigned long long)S.me_id);
        buf_free(&nb);
        cJSON *o = json_parse(j, strlen(j));
        thread_upsert(o, c->guild_id);
        json_done();
        ui_notice(ui_tr("пост создан (демо)"));
        ui_dirty();
        return true;
    }
    Buf b = {0};
    buf_puts(&b, "{\"name\":");
    buf_json_str(&b, title);
    buf_puts(&b, ",\"auto_archive_duration\":4320,\"message\":{\"content\":");
    buf_json_str(&b, text);
    buf_puts(&b, "}}");
    char path[64];
    snprintf(path, sizeof path, "/channels/%llu/threads", (unsigned long long)cid);
    http_async("POST", path, b.p, on_thread_made, NULL);
    buf_free(&b);
    return true;
}
static ThreadMembers TM;
static unsigned tm_gen;
const ThreadMembers *thread_members_state(void) { return &TM; }
void thread_members_close(void) { free(TM.ids); memset(&TM, 0, sizeof TM); tm_gen++; }
static void tm_fill(const cJSON *j) {
    int n = cJSON_GetArraySize(j);
    if (n > 100) n = 100;
    free(TM.ids);
    TM.ids = n > 0 ? xcalloc((size_t)n, sizeof(u64)) : NULL;
    TM.n = 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, j) {
        if (TM.n >= n) break;
        u64 u = jid(x, "user_id");
        if (u) TM.ids[TM.n++] = u;
        if (jget(jget(x, "member"), "user")) user_put_obj(jget(jget(x, "member"), "user"));
    }
}
static void on_tmembers(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != tm_gen) return;
    TM.busy = false;
    if (r->status == 200) { cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len); tm_fill(j); json_done(); }
    else http_err(TM.err, sizeof TM.err, ui_tr("участники ветки"), r->status);
    ui_dirty();
}
void thread_members_open(u64 tid) {
    thread_members_close();
    if (!chan_is_thread(chan_find(tid))) return;
    TM.tid = tid;
    if (O.demo) {
        static const char *const dj = "[{\"id\":\"0\",\"user_id\":\"1000\"},{\"id\":\"0\",\"user_id\":\"1001\"},{\"id\":\"0\",\"user_id\":\"1002\"}]";
        cJSON *j = json_parse(dj, strlen(dj));
        tm_fill(j);
        json_done();
        return;
    }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/thread-members?with_member=true&limit=100", (unsigned long long)tid);
    TM.busy = true;
    http_async("GET", path, NULL, on_tmembers, (void *)(uintptr_t)tm_gen);
}

/* ---------- №112 имя группы ЛС, №113 свой профиль ---------- */
bool gdm_rename(u64 cid, const char *name, char *err, size_t errn) {
    Channel *c = chan_find(cid);
    if (!c || c->type != CH_GROUP) { snprintf(err, errn, ui_tr("это не группа")); return false; }
    if (name && *name && !name_ok(name, 100, err, errn)) return false;
    if (O.demo) { free(c->name); c->name = name && *name ? san(name, false) : NULL; ui_dirty(); return true; }
    Buf b = {0};
    buf_puts(&b, "{\"name\":");
    if (name && *name) buf_json_str(&b, name); else buf_puts(&b, "null");
    buf_puts(&b, "}");
    char path[64];
    snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)cid);
    http_async("PATCH", path, b.p, on_op, (void *)ui_tr("имя группы"));   /* CHANNEL_UPDATE обновит */
    buf_free(&b);
    return true;
}
bool profile_me_set(const char *dn, const char *bio, char *err, size_t errn) {
    if (dn && (u8len(dn) < 1 || u8len(dn) > 32)) { snprintf(err, errn, ui_tr("отображаемое имя: от 1 до 32 символов")); return false; }
    if (dn) { char *t = san(dn, false); bool ok = !strcmp(t, dn); free(t); if (!ok) { snprintf(err, errn, ui_tr("в имени недопустимые символы")); return false; } }
    if (bio && u8len(bio) > 190) { snprintf(err, errn, ui_tr("«о себе»: не больше 190 символов")); return false; }
    if (!dn && !bio) return true;
    if (O.demo) {
        if (dn) { free(S.me_name); S.me_name = san(dn, false); user_put(S.me_id, dn); }
        ui_notice(ui_tr("профиль обновлён (демо)"));
        ui_dirty();
        return true;
    }
    if (dn) {
        Buf b = {0};
        buf_puts(&b, "{\"global_name\":");
        buf_json_str(&b, dn);
        buf_puts(&b, "}");
        http_async("PATCH", "/users/@me", b.p, on_op, (void *)ui_tr("отображаемое имя"));   /* USER_UPDATE обновит */
        buf_free(&b);
    }
    if (bio) {
        Buf b = {0};
        buf_puts(&b, "{\"bio\":");
        buf_json_str(&b, bio);
        buf_puts(&b, "}");
        http_async("PATCH", "/users/@me/profile", b.p, on_op, (void *)ui_tr("«о себе»"));
        buf_free(&b);
    }
    return true;
}

/* ---------- «Просмотр реакций» и «Переслать» (ревью 9 М3: из ui.c) ---------- */
static ReactorsState RV;
const ReactorsState *reactors_state(void) { return &RV; }
void reactors_close(void) { memset(&RV, 0, sizeof RV); ui_dirty(); }
static void on_reactors(Resp *r, void *ctx) {
    u64 tag = (u64)(uintptr_t)ctx;
    if (!RV.on || tag != (RV.msg ^ (u64)RV.sel)) return;   /* окно закрыто или сменилась реакция */
    RV.busy = false;
    RV.n = 0;
    ui_dirty();
    if (r->status != 200) { snprintf(RV.err, sizeof RV.err, r->status ? ui_tr("не загрузилось: HTTP %ld") : ui_tr("не загрузилось: нет связи"), r->status); return; }
    cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len), *u;
    cJSON_ArrayForEach(u, j) {
        if (RV.n >= 25) break;
        const char *nm = jstr(u, "global_name");
        if (!nm || !*nm) nm = jstr(u, "username");
        char *sn = san(nm ? nm : "?", false);
        snprintf(RV.names[RV.n++], sizeof RV.names[0], "%s", sn);
        free(sn);
    }
    json_done();
}
void reactors_open(u64 ch, u64 msg, int sel) {
    Message *m = msg_find(msg, NULL);
    if (!m || !m->nreacts || ch != S.open_ch) { reactors_close(); ui_notice(ui_tr("у сообщения нет реакций")); return; }
    if (sel < 0 || sel >= m->nreacts) sel = 0;
    memset(&RV, 0, sizeof RV);
    RV.on = true; RV.ch = ch; RV.msg = msg; RV.sel = sel;
    ui_dirty();
    if (O.demo) {                               /* демо: без сети */
        static const char *const dn[] = { "Алиса", "Боб", "Ночной Филин", "mira", "Сонный Кот" };
        int k = m->reacts[sel].count < 5 ? m->reacts[sel].count : 5;
        for (int i = 0; i < k; i++) snprintf(RV.names[RV.n++], sizeof RV.names[0], "%s", dn[(i + sel) % 5]);
        return;
    }
    const char *e = m->reacts[sel].emoji ? m->reacts[sel].emoji : "";
    Buf enc = {0}, path = {0};
    url_encode(e[0] == ':' ? e + 1 : e, &enc);
    buf_printf(&path, "/channels/%llu/messages/%llu/reactions/%s?limit=25&type=0", (unsigned long long)ch, (unsigned long long)msg, enc.p ? enc.p : "");
    RV.busy = true;
    http_async("GET", path.p, NULL, on_reactors, (void *)(uintptr_t)(msg ^ (u64)sel));
    buf_free(&enc);
    buf_free(&path);
}
static void on_forwarded(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status >= 200 && r->status < 300) { ui_notice(ui_tr("сообщение переслано")); return; }
    char e[160];
    http_err(e, sizeof e, ui_tr("не переслано"), r->status);
    ui_notice("%s", e);
}
void msg_forward(u64 target, u64 src_ch, u64 msg) {
    Message *m = msg_find(msg, NULL);
    Channel *src = chan_find(src_ch), *dst = chan_find(target);
    if (!m || !src || !dst || dst->type == CH_VOICE || dst->type == CH_CATEGORY || m->state != MS_SENT) {
        ui_notice(ui_tr("переслать можно в текстовый канал или беседу"));
        return;
    }
    if (O.demo) {
        Buf b = {0};
        buf_printf(&b, ui_tr("[переслано] %s"), m->content ? m->content : "");
        demo_send(target, b.p);
        buf_free(&b);
        ui_notice(ui_tr("переслано (демо)"));
        return;
    }
    char path[96];
    Buf b = {0};
    snprintf(path, sizeof path, "/channels/%llu/messages", (unsigned long long)target);
    buf_printf(&b, "{\"message_reference\":{\"type\":1,\"message_id\":\"%llu\",\"channel_id\":\"%llu\"", (unsigned long long)m->id, (unsigned long long)src->id);
    if (src->guild_id) buf_printf(&b, ",\"guild_id\":\"%llu\"", (unsigned long long)src->guild_id);
    buf_printf(&b, "},\"nonce\":\"%llu\"}", (unsigned long long)snowflake_now());
    http_async("POST", path, b.p, on_forwarded, NULL);
    buf_free(&b);
}

/* ---------- ссылки CDN с истёкшим сроком (ex=) — POST /attachments/refresh-urls ---------- */
/* Ревью 9 К1: раньше cdn_fresh слал запрос сам на каждый вызов (а зовут его из разметки ленты каждый
 * кадр) — лавина POST. Теперь: cdn_fresh только смотрит таблицу; cdn_want (из отрисовки видимых строк)
 * ставит ссылку в очередь; store_tick шлёт пачку до CDN_BATCH ссылок, один запрос в полёте, не чаще
 * раза в CDN_GAP мс, не больше CDN_CAP запросов за сессию канала. Отказ/ошибка — отрицательный кэш до
 * смены канала (таблица чистится при смене открытого канала). */
#define CDN_MAX 128
#define CDN_BATCH 50
#define CDN_GAP 3000
#define CDN_CAP 20
enum { CDN_FREE, CDN_WANT, CDN_FLY, CDN_DONE, CDN_FAIL };
static struct { char *orig, *fresh; unsigned char st; } CDN[CDN_MAX];
static u64 cdn_ch;                              /* канал, к которому относится таблица */
static unsigned cdn_gen;                        /* поколение таблицы: ответ на старое — мимо */
static bool cdn_fly;
static int cdn_sent, cdn_nwant;                 /* запросов за сессию канала; ссылок в очереди */
static i64 cdn_last = -CDN_GAP, cdn_demo_at;    /* время последнего запроса; демо: когда «придёт ответ» */
int cdn_requests;                               /* всего запросов (для selftest) */
bool cdn_expired(const char *url) {
    const char *p = url ? strstr(url, "ex=") : NULL;
    if (!p || (p != url && p[-1] != '?' && p[-1] != '&')) return false;
    u64 ex = strtoull(p + 3, NULL, 16);
    return ex && (i64)ex < wall_ms() / 1000;
}
bool cdn_host_ok(const char *u) {
    return !strncmp(u, "https://cdn.discordapp.com/", 27) || !strncmp(u, "https://media.discordapp.net/", 29);
}
static void cdn_reset(void) {
    for (int i = 0; i < CDN_MAX; i++) { free(CDN[i].orig); free(CDN[i].fresh); }
    memset(CDN, 0, sizeof CDN);
    cdn_gen++;
    cdn_fly = false;
    cdn_sent = cdn_nwant = 0;
    cdn_demo_at = 0;
    cdn_ch = S.open_ch;
}
static void cdn_check_ch(void) { if (cdn_ch != S.open_ch) cdn_reset(); }
static void cdn_fail_flying(void) {
    for (int i = 0; i < CDN_MAX; i++) if (CDN[i].st == CDN_FLY) CDN[i].st = CDN_FAIL;
}
static void on_cdn(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != cdn_gen) return;   /* канал сменили — таблица уже другая */
    cdn_fly = false;
    if (r->status == 200) {
        cJSON *j = json_parse(r->body.p ? r->body.p : "", r->body.len);
        const cJSON *e;
        cJSON_ArrayForEach(e, jget(j, "refreshed_urls")) {
            const char *o = jstr(e, "original"), *f = jstr(e, "refreshed");
            if (!o || !f || !cdn_host_ok(f) || strlen(f) >= 1024) continue;
            for (int i = 0; i < CDN_MAX; i++)
                if (CDN[i].st == CDN_FLY && !strcmp(CDN[i].orig, o)) {
                    free(CDN[i].fresh); CDN[i].fresh = san(f, false); CDN[i].st = CDN_DONE;
                    break;
                }
        }
        json_done();
    }
    cdn_fail_flying();                          /* не вернулись в ответе — не повторять */
    ui_dirty();
}
const char *cdn_fresh(const char *url) {
    if (!url || cdn_ch != S.open_ch) return NULL;
    for (int i = 0; i < CDN_MAX; i++)
        if (CDN[i].st == CDN_DONE && !strcmp(CDN[i].orig, url)) return CDN[i].fresh;
    return NULL;
}
void cdn_want(const char *url) {
    if (!url || !cdn_host_ok(url) || strlen(url) > 1000 || !cdn_expired(url)) return;
    cdn_check_ch();
    int fr = -1;
    for (int i = 0; i < CDN_MAX; i++) {
        if (CDN[i].st == CDN_FREE) { if (fr < 0) fr = i; continue; }
        if (!strcmp(CDN[i].orig, url)) return;  /* уже в очереди, в полёте, готова или отказ */
    }
    if (fr < 0 || cdn_sent >= CDN_CAP) return;  /* таблица полна / предел — без обновления */
    CDN[fr].orig = xstrdup(url);
    CDN[fr].st = CDN_WANT;
    cdn_nwant++;
}
static void cdn_flush(i64 now) {
    if (cdn_demo_at && now >= cdn_demo_at) {    /* демо: «ответ сети» пришёл — ссылка та же */
        cdn_demo_at = 0;
        cdn_fly = false;
        for (int i = 0; i < CDN_MAX; i++)
            if (CDN[i].st == CDN_FLY) { CDN[i].fresh = xstrdup(CDN[i].orig); CDN[i].st = CDN_DONE; }
        ui_dirty();
    }
    if (!cdn_nwant || cdn_fly || now < cdn_last + CDN_GAP) return;
    if (cdn_ch != S.open_ch) { cdn_reset(); return; }
    if (cdn_sent >= CDN_CAP) {                  /* предел сессии канала: остаток — отказ */
        for (int i = 0; i < CDN_MAX; i++) if (CDN[i].st == CDN_WANT) CDN[i].st = CDN_FAIL;
        cdn_nwant = 0;
        return;
    }
    Buf b = {0};
    buf_puts(&b, "{\"attachment_urls\":[");
    int n = 0;
    for (int i = 0; i < CDN_MAX && n < CDN_BATCH; i++) {
        if (CDN[i].st != CDN_WANT) continue;
        if (n++) buf_puts(&b, ",");
        buf_json_str(&b, CDN[i].orig);
        CDN[i].st = CDN_FLY;
        cdn_nwant--;
    }
    buf_puts(&b, "]}");
    cdn_fly = true;
    cdn_sent++;
    cdn_requests++;
    cdn_last = now;
    if (O.demo) cdn_demo_at = now + 400;        /* демо имитирует задержку сети */
    else http_async("POST", "/attachments/refresh-urls", b.p, on_cdn, (void *)(uintptr_t)cdn_gen);
    buf_free(&b);
}
static i64 cdn_next_wake(void) {
    if (cdn_demo_at) return cdn_demo_at;
    if (cdn_nwant && !cdn_fly) return cdn_last + CDN_GAP;
    return INT64_MAX;
}

/* ---------- READY: отсев полей до cJSON ----------
 * cJSON тратит ~10x от размера текста (узел на каждое значение). Большая часть READY клиенту не
 * нужна (experiments, emojis, stickers, сессии, настройки-protobuf, чужие присутствия...). Фильтр
 * идёт по тексту один раз, по «белому списку» ключей, и пишет результат на месте (выход не длиннее
 * входа): дерево cJSON строится только из нужного. Схема: ключ -> подсхема (NULL — значение целиком);
 * подсхема применяется к объекту или к каждому элементу массива (в том числе массива массивов). */
typedef struct Sch { const char *key; const struct Sch *sub; } Sch;
#define END { NULL, NULL }
static const Sch S_USER[] = { {"id", NULL}, {"username", NULL}, {"global_name", NULL}, {"avatar", NULL}, END };
static const Sch S_OW[] = { {"id", NULL}, {"type", NULL}, {"allow", NULL}, {"deny", NULL}, END };
static const Sch S_CHAN[] = { {"id", NULL}, {"type", NULL}, {"name", NULL}, {"position", NULL}, {"parent_id", NULL},
    {"last_message_id", NULL}, {"guild_id", NULL}, {"permission_overwrites", S_OW}, {"recipient_ids", NULL},
    {"recipients", S_USER}, {"rate_limit_per_user", NULL}, {"owner_id", NULL}, {"is_message_request", NULL}, {"is_spam", NULL},
    {"status", NULL}, END };
static const Sch S_TMETA[] = { {"archived", NULL}, {"locked", NULL}, END };
static const Sch S_TMEM[] = { {"user_id", NULL}, {"id", NULL}, END };
static const Sch S_THR[] = { {"id", NULL}, {"type", NULL}, {"name", NULL}, {"parent_id", NULL}, {"guild_id", NULL},
    {"last_message_id", NULL}, {"owner_id", NULL}, {"rate_limit_per_user", NULL}, {"thread_metadata", S_TMETA}, {"member", S_TMEM}, END };
static const Sch S_ROLE[] = { {"id", NULL}, {"permissions", NULL}, {"color", NULL}, {"position", NULL}, END };
static const Sch S_MEMBER[] = { {"user_id", NULL}, {"nick", NULL}, {"roles", NULL}, {"user", S_USER},
    {"communication_disabled_until", NULL}, {"pending", NULL}, END };
static const Sch S_VS[] = { {"user_id", NULL}, {"channel_id", NULL}, {"self_mute", NULL}, {"self_deaf", NULL}, {"self_stream", NULL}, {"self_video", NULL},
    {"mute", NULL}, {"deaf", NULL}, {"member", S_MEMBER}, END };
static const Sch S_GPROP[] = { {"name", NULL}, {"owner_id", NULL}, {"icon", NULL}, {"banner", NULL}, END };
static const Sch S_GUILD[] = { {"id", NULL}, {"unavailable", NULL}, {"properties", S_GPROP}, {"name", NULL},
    {"owner_id", NULL}, {"icon", NULL}, {"banner", NULL}, {"roles", S_ROLE}, {"channels", S_CHAN}, {"voice_states", S_VS}, {"threads", S_THR}, END };
static const Sch S_RSE[] = { {"id", NULL}, {"last_message_id", NULL}, {"mention_count", NULL}, {"read_state_type", NULL}, END };
static const Sch S_RS[] = { {"entries", S_RSE}, {"id", NULL}, {"last_message_id", NULL}, {"mention_count", NULL},
    {"read_state_type", NULL}, END };   /* read_state: объект {entries} или сразу массив записей */
static const Sch S_CO[] = { {"channel_id", NULL}, {"muted", NULL}, {"message_notifications", NULL}, END };
static const Sch S_UGSE[] = { {"guild_id", NULL}, {"muted", NULL}, {"message_notifications", NULL}, {"channel_overrides", S_CO}, END };
static const Sch S_UGS[] = { {"entries", S_UGSE}, {"guild_id", NULL}, {"muted", NULL}, {"channel_overrides", S_CO}, END };
static const Sch S_REL[] = { {"id", NULL}, {"type", NULL}, {"user_id", NULL}, {"user", S_USER}, END };
static const Sch S_PRES[] = { {"user_id", NULL}, {"status", NULL}, {"user", S_USER}, END };
static const Sch S_MPRES[] = { {"friends", S_PRES}, END };      /* guilds (присутствия серверов) — не нужны */
static const Sch S_PROTO_HOOK[] = { END };   /* значение не копируется: папки разбираются сразу (folders_slim) */
static const Sch S_USET[] = { {"guild_folders", NULL}, END };
static const Sch S_ME[] = { {"id", NULL}, {"username", NULL}, {"global_name", NULL}, {"avatar", NULL}, {"premium_type", NULL}, END };
static const Sch S_READY[] = { {"user", S_ME}, {"session_id", NULL}, {"resume_gateway_url", NULL}, {"users", S_USER},
    {"guilds", S_GUILD}, {"merged_members", S_MEMBER}, {"private_channels", S_CHAN}, {"read_state", S_RS},
    {"user_guild_settings", S_UGS}, {"user_settings_proto", S_PROTO_HOOK}, {"user_settings", S_USET}, {"relationships", S_REL}, {"merged_presences", S_MPRES}, {"presences", S_PRES}, END };
static const Sch S_SUPG[] = { {"id", NULL}, {"voice_states", S_VS}, END };
/* статусы друзей у пользовательских аккаунтов приходят здесь (merged_presences.friends), а не в READY */
static const Sch S_SUPP[] = { {"guilds", S_SUPG}, {"merged_members", S_MEMBER}, {"merged_presences", S_MPRES}, END };
static const Sch S_TOP_READY[] = { {"t", NULL}, {"s", NULL}, {"op", NULL}, {"d", S_READY}, END };
static const Sch S_TOP_SUPP[] = { {"t", NULL}, {"s", NULL}, {"op", NULL}, {"d", S_SUPP}, END };
#undef END

/* selftest (ревью 14): ужатый READY — только ключи схемы и ни одного null; why — первое нарушение */
static bool slim_walk(const cJSON *v, const Sch *sch, char *why, size_t wn, int depth) {
    if (depth > 60) return false;
    if (cJSON_IsNull(v)) { snprintf(why, wn, "null"); return false; }
    if (cJSON_IsArray(v)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, v) if (!slim_walk(e, sch, why, wn, depth + 1)) return false;
        return true;
    }
    if (!cJSON_IsObject(v) || !sch) return true;
    const cJSON *e;
    cJSON_ArrayForEach(e, v) {
        const Sch *hit = NULL;
        for (const Sch *k = sch; k->key; k++) if (!strcmp(k->key, e->string)) hit = k;
        if (!hit || hit->sub == S_PROTO_HOOK) { snprintf(why, wn, ui_tr("лишний ключ %.40s"), e->string); return false; }
        if (!slim_walk(e, hit->sub, why, wn, depth + 1)) return false;
    }
    return true;
}
bool ready_slim_check(const char *s, size_t n, char *why, size_t wn);
bool ready_slim_check(const char *s, size_t n, char *why, size_t wn) {
    cJSON *j = json_parse(s, n);
    bool ok = j && slim_walk(j, S_TOP_READY, why, wn, 0);
    if (!j) snprintf(why, wn, "не JSON");
    json_done();
    return ok;
}
typedef struct { char *s; size_t r, w, n; bool bad; } Slim;   /* r — чтение, w — запись (всегда w <= r) */
/* Запись на месте безопасна, только пока каждый выходной байт соответствует уже прочитанному входному:
 * разделители пишутся лишь после того, как такой же прочитан (между членами ',' обязательна), и любая
 * запись проверяет w < r (sl_put) / w <= from (sl_out). Нарушение — bad, разбор прекращается. */
static void sl_ws(Slim *x) { while (x->r < x->n && (x->s[x->r] == ' ' || x->s[x->r] == '\n' || x->s[x->r] == '\r' || x->s[x->r] == '\t')) x->r++; }
static void sl_put(Slim *x, char c) { if (x->bad || x->w >= x->r) { x->bad = true; return; } x->s[x->w++] = c; }
static void sl_out(Slim *x, size_t from, size_t len) {
    if (x->bad || x->w > from || from + len > x->n) { x->bad = true; return; }
    memmove(x->s + x->w, x->s + from, len);
    x->w += len;
}
/* Конец значения, начинающегося в r (без записи): строка, объект/массив (скобки парные, глубина <= 64)
 * или скаляр (непустой, до ',' '}' ']' или пробела). Иначе — bad. */
static size_t sl_skip(Slim *x, size_t r) {
    u64 stack = 0;                              /* бит 1 — '{', 0 — '[' */
    int depth = 0;
    size_t r0 = r;
    while (r < x->n) {
        char c = x->s[r];
        if (c == '"') {
            for (r++; r < x->n && x->s[r] != '"'; r++) if (x->s[r] == '\\') r++;
            if (r >= x->n) break;                   /* строка не закрыта */
            r++;
            if (!depth) return r;
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth >= 64) break;
            stack = stack << 1 | (c == '{');
            depth++;
        } else if (c == '}' || c == ']') {
            if (!depth) { if (r == r0) break; return r; }        /* конец скаляра; пустое значение — ошибка */
            if ((stack & 1) != (u64)(c == '}')) break;           /* '{' закрыта ']' и т. п. */
            stack >>= 1;
            if (--depth == 0) return r + 1;
        } else if (!depth && (c == ',' || c == ' ' || c == '\n' || c == '\r' || c == '\t')) {
            if (r == r0) break;
            return r;
        } else if (!depth && c == ':') break;   /* в скаляре двоеточия не бывает */
        r++;
    }
    x->bad = true;
    return x->n;
}
static void sl_value(Slim *x, const Sch *sch, int lvl);
static void sl_object(Slim *x, const Sch *sch, int lvl) {
    x->r++;                                     /* '{' прочитана — теперь её можно записать */
    sl_put(x, '{');
    bool first = true, need = false;            /* need: перед следующим членом обязательна ',' */
    for (;;) {
        if (x->bad) return;
        sl_ws(x);
        if (x->r >= x->n) { x->bad = true; return; }
        char c = x->s[x->r];
        if (c == '}') { x->r++; break; }
        if (need) {
            if (c != ',') { x->bad = true; return; }
            x->r++;
            sl_ws(x);
            if (x->r >= x->n || x->s[x->r] != '"') { x->bad = true; return; }   /* ",}" и ",," — ошибка */
        } else if (c != '"') { x->bad = true; return; }
        size_t k0 = x->r, k1 = sl_skip(x, x->r);         /* ключ с кавычками: [k0, k1) */
        if (x->bad || k1 < k0 + 2 || x->s[k0] != '"' || x->s[k1 - 1] != '"') { x->bad = true; return; }
        x->r = k1;
        sl_ws(x);
        if (x->r >= x->n || x->s[x->r] != ':') { x->bad = true; return; }
        x->r++;
        sl_ws(x);
        need = true;
        const Sch *hit = NULL;
        size_t kl = k1 - k0 - 2;
        for (const Sch *e = sch; e->key; e++) if (strlen(e->key) == kl && !memcmp(e->key, x->s + k0 + 1, kl)) { hit = e; break; }
        if (hit && hit->sub == S_PROTO_HOOK) {   /* №2: protobuf настроек — папки сейчас, текст — выбросить */
            size_t e = sl_skip(x, x->r);
            if (x->bad) return;
            if (x->s[x->r] == '"' && e >= x->r + 2) folders_slim(x->s + x->r + 1, e - x->r - 2);
            x->r = e;
            continue;
        }
        if (!hit) { size_t e = sl_skip(x, x->r); if (x->bad) return; x->r = e; continue; }
        if (x->n - x->r >= 4 && !memcmp(x->s + x->r, "null", 4)) {   /* null = ключа нет (в READY разбор одинаков) */
            size_t e = sl_skip(x, x->r);
            if (x->bad) return;
            if (e == x->r + 4) { x->r = e; continue; }
        }
        if (!first) sl_put(x, ',');
        first = false;
        sl_out(x, k0, k1 - k0);
        sl_put(x, ':');
        size_t before = x->r;
        sl_value(x, hit->sub, lvl + 1);
        if (!x->bad && x->r == before) x->bad = true;          /* значение не прочитано — нет продвижения */
    }
    sl_put(x, '}');
}
static void sl_value(Slim *x, const Sch *sch, int lvl) {
    if (lvl > 60) { x->bad = true; return; }
    sl_ws(x);
    if (x->r >= x->n) { x->bad = true; return; }
    char c = x->s[x->r];
    if (sch && c == '{') { sl_object(x, sch, lvl); return; }
    if (sch && c == '[') {
        x->r++;
        sl_put(x, '[');
        bool first = true;
        for (;;) {
            if (x->bad) return;
            sl_ws(x);
            if (x->r >= x->n) { x->bad = true; return; }
            char d = x->s[x->r];
            if (d == ']') { x->r++; break; }
            if (!first) {
                if (d != ',') { x->bad = true; return; }
                x->r++;
                sl_ws(x);
                if (x->r >= x->n || x->s[x->r] == ']' || x->s[x->r] == ',') { x->bad = true; return; }
                sl_put(x, ',');
            }
            first = false;
            size_t before = x->r;
            sl_value(x, sch, lvl + 1);
            if (!x->bad && x->r == before) x->bad = true;
        }
        sl_put(x, ']');
        return;
    }
    size_t e = sl_skip(x, x->r);                    /* значение целиком (или скаляр там, где ждали объект) */
    if (x->bad) return;
    sl_out(x, x->r, e - x->r);
    x->r = e;
}
/* Ужать текст пакета READY / READY_SUPPLEMENTAL на месте. Возврат — новая длина; 0 — не READY или
 * текст не разобран (тогда он не тронут только если ошибка до первой записи — вызывающий разбирает как есть
 * лишь при возврате len). */
/* ---------- №57 слеш-команды ---------- */
static const Sch S_CHOICE[] = { {"name", NULL}, {"value", NULL}, { NULL, NULL } };
static const Sch S_COPT[] = { {"type", NULL}, {"name", NULL}, {"description", NULL}, {"required", NULL},
    {"choices", S_CHOICE}, {"options", S_COPT}, { NULL, NULL } };
static const Sch S_ACMD[] = { {"id", NULL}, {"application_id", NULL}, {"version", NULL}, {"type", NULL}, {"name", NULL},
    {"description", NULL}, {"options", S_COPT}, { NULL, NULL } };
static const Sch S_APP[] = { {"id", NULL}, {"name", NULL}, { NULL, NULL } };
static const Sch S_CMDIDX[] = { {"applications", S_APP}, {"application_commands", S_ACMD}, { NULL, NULL } };
static SlashIndex CX;
static unsigned cx_gen;
const SlashIndex *cmds_state(void) { return &CX; }
const char *cmds_str(u16 off) { return CX.s && off != 0xFFFF ? CX.s + off : ""; }
void cmds_close(void) {
    free(CX.c); free(CX.o); free(CX.s);
    memset(&CX, 0, sizeof CX);
    cx_gen++;
}
static u16 cx_put(Buf *b, const char *s, size_t max) {
    if (!s || b->len > 60000) return 0xFFFF;
    char *t = san(s, false);
    utf8_trunc(t, max);
    u16 off = (u16)b->len;
    buf_put(b, t, strlen(t) + 1);
    free(t);
    return off;
}
static u16 cx_raw(Buf *b, const char *s) {      /* значение варианта — как есть (уйдёт обратно в JSON) */
    if (!s || strlen(s) > 200 || b->len > 60000) return 0xFFFF;
    u16 off = (u16)b->len;
    buf_put(b, s, strlen(s) + 1);
    return off;
}
static bool cx_opt_ok(int t) { return t == CO_STRING || t == CO_INTEGER || t == CO_BOOLEAN || t == CO_USER || t == CO_CHANNEL || t == CO_NUMBER; }
/* одна запись команды (cmd — объект команды, sub/grp — объекты подкоманды/группы или NULL) */
static void cx_add(Buf *b, const cJSON *cmd, const cJSON *grp, const cJSON *sub, const cJSON *apps) {
    if (CX.n >= 400) return;
    const cJSON *leaf = sub ? sub : cmd;
    CX.c = xrealloc(CX.c, (size_t)(CX.n + 1) * sizeof(SlashCmd));
    SlashCmd *c = &CX.c[CX.n];
    memset(c, 0, sizeof *c);
    c->id = jid(cmd, "id");
    c->app_id = jid(cmd, "application_id");
    c->version = jid(cmd, "version");
    char full[200];
    snprintf(full, sizeof full, "%.60s%s%.60s%s%.60s", jstr(cmd, "name") ? jstr(cmd, "name") : "?", grp ? " " : "", grp ? jstr(grp, "name") : "",
             sub ? " " : "", sub ? jstr(sub, "name") : "");
    c->name = cx_put(b, full, 190);
    c->base = cx_raw(b, jstr(cmd, "name"));     /* ревью 16 М2: для отправки — сырое имя команды */
    c->desc = cx_put(b, jstr(leaf, "description"), 100);
    c->sub = sub ? cx_raw(b, jstr(sub, "name")) : 0xFFFF;
    c->grp = grp ? cx_raw(b, jstr(grp, "name")) : 0xFFFF;
    c->app = 0xFFFF;
    const cJSON *a;
    cJSON_ArrayForEach(a, apps) if (jid(a, "id") == c->app_id) c->app = cx_put(b, jstr(a, "name"), 60);
    c->opt0 = CX.no;
    c->usable = c->id && c->app_id && c->version && c->base != 0xFFFF;
    const cJSON *o;
    cJSON_ArrayForEach(o, jget(leaf, "options")) {
        int t = (int)jint(o, "type", 0);
        if (t == 1 || t == 2) continue;
        if (c->nopt >= 25) break;
        if (!cx_opt_ok(t)) { if (jbool(o, "required")) c->usable = false; continue; }
        CX.o = xrealloc(CX.o, (size_t)(CX.no + 1) * sizeof(SlashOpt));
        SlashOpt *p = &CX.o[CX.no++];
        memset(p, 0, sizeof *p);
        p->type = (u8)t;
        p->required = jbool(o, "required");
        p->name = cx_raw(b, jstr(o, "name"));
        p->name_show = cx_put(b, jstr(o, "name"), 100);   /* С-1: показ — только очищенное */
        p->desc = cx_put(b, jstr(o, "description"), 100);
        p->choice = (u16)b->len;
        const cJSON *ch;
        cJSON_ArrayForEach(ch, jget(o, "choices")) {
            if (p->nchoice >= 25) break;
            const cJSON *v = jget(ch, "value");
            char num[40];
            const char *vs = cJSON_IsString(v) ? v->valuestring : NULL;
            if (cJSON_IsNumber(v)) { snprintf(num, sizeof num, "%.17g", v->valuedouble); vs = num; }
            if (!vs || cx_put(b, jstr(ch, "name"), 100) == 0xFFFF) break;
            if (cx_raw(b, vs) == 0xFFFF) buf_put(b, "", 1);
            p->nchoice++;
        }
        c->nopt++;
    }
    if (c->name == 0xFFFF) return;
    CX.n++;
}
static void cx_fill(const cJSON *j) {
    Buf b = {0};
    buf_put(&b, "", 1);
    const cJSON *apps = jget(j, "applications"), *cmd;
    cJSON_ArrayForEach(cmd, jget(j, "application_commands")) {
        if (jint(cmd, "type", 1) != 1) continue;   /* только команды чата */
        bool has_sub = false;
        const cJSON *o;
        cJSON_ArrayForEach(o, jget(cmd, "options")) {
            int t = (int)jint(o, "type", 0);
            if (t == 1) { has_sub = true; cx_add(&b, cmd, NULL, o, apps); }
            else if (t == 2) {
                has_sub = true;
                const cJSON *s2;
                cJSON_ArrayForEach(s2, jget(o, "options")) if (jint(s2, "type", 0) == 1) cx_add(&b, cmd, o, s2, apps);
            }
        }
        if (!has_sub) cx_add(&b, cmd, NULL, NULL, apps);
    }
    CX.s = b.p;
}
static void on_cmds(Resp *r, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != cx_gen) return;
    CX.busy = false;
    if (r->status == 200 && r->body.p) {
        /* индекс бывает в сотни КБ: до cJSON — отсев по схеме (только нужные поля), на месте */
        Slim x = { r->body.p, 0, 0, r->body.len, false };
        sl_ws(&x);
        if (x.r < x.n && r->body.p[x.r] == '{') sl_value(&x, S_CMDIDX, 0);
        if (x.bad) { snprintf(CX.err, sizeof CX.err, ui_tr("список команд не разобран")); ui_dirty(); return; }
        cJSON *j = json_parse(r->body.p, x.w);
        cx_fill(j);
        json_done();
    } else http_err(CX.err, sizeof CX.err, ui_tr("команды"), r->status);
    ui_dirty();
}
void cmds_test_resp(long status, const char *body);
void cmds_test_resp(long status, const char *body) {
    Resp r = { .status = status };
    buf_puts(&r.body, body);
    CX.busy = true;
    on_cmds(&r, (void *)(uintptr_t)cx_gen);
    buf_free(&r.body);
}
void cmds_open(u64 cid) {
    if (CX.cid == cid && (CX.busy || CX.n)) return;
    cmds_close();
    Channel *c = chan_find(cid);
    if (!c || cid != S.open_ch) return;
    CX.cid = cid;
    if (O.demo) {
        static const char *const dj = "{\"applications\":[{\"id\":\"1100\",\"name\":\"MemeBot\"}],\"application_commands\":["
            "{\"id\":\"5001\",\"application_id\":\"1100\",\"version\":\"7001\",\"type\":1,\"name\":\"play\",\"description\":\"Включить трек\","
            "\"options\":[{\"type\":3,\"name\":\"запрос\",\"description\":\"название или ссылка\",\"required\":true},"
            "{\"type\":4,\"name\":\"громкость\",\"description\":\"0–100\"},{\"type\":5,\"name\":\"повтор\",\"description\":\"по кругу\"}]},"
            "{\"id\":\"5002\",\"application_id\":\"1100\",\"version\":\"7002\",\"type\":1,\"name\":\"meme\",\"description\":\"Мем дня\","
            "\"options\":[{\"type\":3,\"name\":\"тема\",\"description\":\"о чём\",\"choices\":[{\"name\":\"Коты\",\"value\":\"cats\"},{\"name\":\"Код\",\"value\":\"code\"}]}]},"
            "{\"id\":\"5003\",\"application_id\":\"1100\",\"version\":\"7003\",\"type\":1,\"name\":\"queue\",\"description\":\"Очередь\","
            "\"options\":[{\"type\":1,\"name\":\"show\",\"description\":\"Показать очередь\"},{\"type\":1,\"name\":\"clear\",\"description\":\"Очистить\"}]},"
            "{\"id\":\"5004\",\"application_id\":\"1100\",\"version\":\"7004\",\"type\":1,\"name\":\"avatar\",\"description\":\"Аватар участника\","
            "\"options\":[{\"type\":6,\"name\":\"кто\",\"description\":\"участник\",\"required\":true}]}]}";
        Resp r = { .status = 200 };
        buf_puts(&r.body, dj);
        CX.busy = true;
        on_cmds(&r, (void *)(uintptr_t)cx_gen);
        buf_free(&r.body);
        return;
    }
    char path[96];
    if (c->guild_id) snprintf(path, sizeof path, "/guilds/%llu/application-command-index", (unsigned long long)c->guild_id);
    else snprintf(path, sizeof path, "/channels/%llu/application-command-index", (unsigned long long)cid);
    CX.busy = true;
    http_async("GET", path, NULL, on_cmds, (void *)(uintptr_t)cx_gen);
}
int cmds_match(const char *prefix, int *out, int max) {
    char pl[200], nl[200];
    aw_fold(prefix ? prefix : "", strlen(prefix ? prefix : ""), pl, sizeof pl);
    size_t n = strlen(pl);
    int k = 0;
    for (int i = 0; i < CX.n && k < max; i++) {
        aw_fold(cmds_str(CX.c[i].name), strlen(cmds_str(CX.c[i].name)), nl, sizeof nl);
        if (!strncmp(nl, pl, n)) out[k++] = i;
    }
    return k;
}
bool cmd_choice(int opt, int k, const char **name, const char **value) {
    if (opt < 0 || opt >= CX.no || k < 0 || k >= CX.o[opt].nchoice) return false;
    const char *p = CX.s + CX.o[opt].choice;
    for (int q = 0; q < k; q++) { p += strlen(p) + 1; p += strlen(p) + 1; }
    *name = p;
    *value = p + strlen(p) + 1;
    return true;
}
/* значение опции -> JSON (в b), false — неверно (err) */
static bool cx_value(Buf *b, const SlashOpt *o, const char *v, const Channel *ch, char *err, size_t errn) {
    char nm[120];                               /* С-1: имя опции — только очищенным в тексте ошибки */
    snprintf(nm, sizeof nm, "%s", cmds_str(o->name_show));
    if (o->nchoice) {                           /* с вариантами: по имени или значению варианта */
        for (int k = 0; k < o->nchoice; k++) {
            const char *cn, *cv;
            cmd_choice((int)(o - CX.o), k, &cn, &cv);
            if (!strcmp(v, cn) || !strcmp(v, cv)) { v = cv; goto typed; }
        }
        snprintf(err, errn, ui_tr("%s: выберите из списка"), nm);
        return false;
    }
typed:
    switch (o->type) {
    case CO_STRING: buf_json_str(b, v); return true;
    case CO_INTEGER: case CO_NUMBER: {
        /* С-2: только десятичная запись (без inf/nan/0x…), конечное, |d| <= 2^53 */
        bool dec = *v != 0;
        for (const char *q = v; *q && dec; q++) dec = (*q >= '0' && *q <= '9') || *q == '-' || *q == '+' || *q == '.' || *q == 'e' || *q == 'E';
        char *e = NULL;
        double d = dec ? strtod(v, &e) : 0;
        if (!dec || (e && *e) || !isfinite(d) || fabs(d) > 9007199254740992.0 || (o->type == CO_INTEGER && d != (double)(long long)d)) {
            snprintf(err, errn, ui_tr("%s: нужно число"), nm);
            return false;
        }
        if (o->type == CO_INTEGER) buf_printf(b, "%lld", (long long)d);
        else buf_printf(b, "%.17g", d);
        return true;
    }
    case CO_BOOLEAN: {
        bool t = !strcmp(v, "да") || !strcmp(v, "true") || !strcmp(v, "1"), f = !strcmp(v, "нет") || !strcmp(v, "false") || !strcmp(v, "0");
        if (!t && !f) { snprintf(err, errn, ui_tr("%s: да или нет"), nm); return false; }
        buf_puts(b, t ? "true" : "false");
        return true;
    }
    case CO_USER: {
        u64 id = strtoull(v[0] == '@' ? v + 1 : v, NULL, 10);
        if (!id) for (int i = 0; i < S.nusers && !id; i++) if (S.users[i].name && !strcmp(S.users[i].name, v[0] == '@' ? v + 1 : v)) id = S.users[i].id;
        if (!id) { snprintf(err, errn, ui_tr("%s: пользователь не найден"), nm); return false; }
        buf_printf(b, "\"%llu\"", (unsigned long long)id);
        return true;
    }
    case CO_CHANNEL: {
        u64 id = strtoull(v[0] == '#' ? v + 1 : v, NULL, 10);
        if (!id) for (int i = 0; i < S.nchans && !id; i++)
            if (S.chans[i].guild_id == ch->guild_id && S.chans[i].name && !strcmp(S.chans[i].name, v[0] == '#' ? v + 1 : v)) id = S.chans[i].id;
        if (!id) { snprintf(err, errn, ui_tr("%s: канал не найден"), nm); return false; }
        buf_printf(b, "\"%llu\"", (unsigned long long)id);
        return true;
    }
    default: snprintf(err, errn, ui_tr("%s: такой тип не поддерживается"), nm); return false;
    }
}
static void on_cmd_run(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status >= 200 && r->status < 300) return;
    char e[160];
    http_err(e, sizeof e, ui_tr("команда не выполнена"), r->status);
    ui_notice("%s", e);
}
bool cmd_run(int i, const char *const *vals, int nvals, char *err, size_t errn) {
    Channel *ch = chan_find(CX.cid);
    if (i < 0 || i >= CX.n || !ch || CX.cid != S.open_ch) { snprintf(err, errn, ui_tr("команда не найдена")); return false; }
    const SlashCmd *c = &CX.c[i];
    if (!c->usable) { snprintf(err, errn, ui_tr("эту команду здесь не выполнить (нужны неподдерживаемые поля)")); return false; }
    Buf opts = {0};
    int no = 0;
    for (int k = 0; k < c->nopt; k++) {
        const SlashOpt *o = &CX.o[c->opt0 + k];
        const char *v = k < nvals && vals[k] ? vals[k] : "";
        if (!*v) { if (o->required) { snprintf(err, errn, ui_tr("%s: обязательно"), cmds_str(o->name_show)); buf_free(&opts); return false; } continue; }
        buf_printf(&opts, "%s{\"type\":%d,\"name\":", no++ ? "," : "", o->type);
        buf_json_str(&opts, cmds_str(o->name));
        buf_puts(&opts, ",\"value\":");
        if (!cx_value(&opts, o, v, ch, err, errn)) { buf_free(&opts); return false; }
        buf_puts(&opts, "}");
    }
    if (O.demo) { ui_notice(ui_tr("демо: команда /%s отправлена"), cmds_str(c->name)); buf_free(&opts); return true; }
    Buf b = {0};
    buf_printf(&b, "{\"type\":2,\"nonce\":\"%llu\",\"application_id\":\"%llu\",", (unsigned long long)snowflake_now(), (unsigned long long)c->app_id);
    if (ch->guild_id) buf_printf(&b, "\"guild_id\":\"%llu\",", (unsigned long long)ch->guild_id);
    buf_printf(&b, "\"channel_id\":\"%llu\",\"session_id\":", (unsigned long long)ch->id);
    buf_json_str(&b, S.session_id ? S.session_id : "");
    buf_printf(&b, ",\"data\":{\"version\":\"%llu\",\"id\":\"%llu\",\"name\":", (unsigned long long)c->version, (unsigned long long)c->id);
    buf_json_str(&b, cmds_str(c->base));
    buf_puts(&b, ",\"type\":1,\"options\":[");
    if (c->grp != 0xFFFF) { buf_puts(&b, "{\"type\":2,\"name\":"); buf_json_str(&b, cmds_str(c->grp)); buf_puts(&b, ",\"options\":["); }
    if (c->sub != 0xFFFF) { buf_puts(&b, "{\"type\":1,\"name\":"); buf_json_str(&b, cmds_str(c->sub)); buf_puts(&b, ",\"options\":["); }
    if (opts.p) buf_put(&b, opts.p, opts.len);
    if (c->sub != 0xFFFF) buf_puts(&b, "]}");
    if (c->grp != 0xFFFF) buf_puts(&b, "]}");
    buf_puts(&b, "]}}");
    http_async("POST", "/interactions", b.p, on_cmd_run, NULL);
    buf_free(&b);
    buf_free(&opts);
    return true;
}

/* ---------- №133/117/147 модерация ---------- */
#define P_KICK (1ULL << 1)
#define P_BAN (1ULL << 2)
#define P_MUTE_M (1ULL << 22)
#define P_DEAF_M (1ULL << 23)
#define P_MOVE_M (1ULL << 24)
#define P_MODERATE (1ULL << 40)
bool mod_can(u64 gid, int action) {
    Guild *g = guild_find(gid);
    if (!g) return false;
    u64 p = g->owner_id == S.me_id ? ~0ULL : guild_base_perms(g);
    switch (action) {
    case MOD_TIMEOUT: return (p & P_MODERATE) != 0;
    case MOD_KICK: return (p & P_KICK) != 0;
    case MOD_BAN: return (p & P_BAN) != 0;
    case MOD_ROLES: return (p & P_MANAGE_ROLES) != 0;
    case MOD_MOVE: return (p & P_MOVE_M) != 0;
    case MOD_MUTE: return (p & (P_MUTE_M | P_DEAF_M)) != 0;
    default: return false;
    }
}
static bool mod_pre(u64 gid, u64 uid, int action, const char *reason, char *err, size_t errn) {
    Guild *g = guild_find(gid);
    if (!g || !uid) { snprintf(err, errn, ui_tr("участник не найден")); return false; }
    if (!mod_can(gid, action)) { snprintf(err, errn, ui_tr("нет права на это действие")); return false; }
    if (uid == g->owner_id) { snprintf(err, errn, ui_tr("к владельцу сервера это не применить")); return false; }
    if (uid == S.me_id && action != MOD_ROLES && action != MOD_MOVE && action != MOD_MUTE) { snprintf(err, errn, ui_tr("к себе это не применить")); return false; }
    if (reason && (u8len(reason) > 512 || strlen(reason) * 3 + 32 > 8000)) { snprintf(err, errn, ui_tr("причина слишком длинная (до 512 символов)")); return false; }
    return true;
}
/* причина — в заголовок X-Audit-Log-Reason (URL-кодированная) */
static void mod_send(const char *method, const char *path, const char *body, const char *reason, const char *what) {
    if (reason && *reason) {
        Buf h = {0}, enc = {0};
        char *t = san(reason, false);
        url_encode(t, &enc);
        free(t);
        buf_puts(&h, "X-Audit-Log-Reason: ");
        if (enc.p) buf_put(&h, enc.p, enc.len);
        buf_put(&h, "", 1);
        http_async_hdr(method, path, body, h.p, on_op, (void *)what);
        buf_free(&h); buf_free(&enc);
    } else http_async(method, path, body, on_op, (void *)what);
}
bool mod_timeout(u64 gid, u64 uid, int minutes, const char *reason, char *err, size_t errn) {
    if (!mod_pre(gid, uid, MOD_TIMEOUT, reason, err, errn)) return false;
    if (minutes < 0 || minutes > 28 * 24 * 60) { snprintf(err, errn, ui_tr("тайм-аут: до 28 суток")); return false; }
    if (O.demo) { ui_notice(minutes ? ui_tr("демо: тайм-аут на %d мин") : ui_tr("демо: тайм-аут снят"), minutes); return true; }
    char path[96], body[96];
    snprintf(path, sizeof path, "/guilds/%llu/members/%llu", (unsigned long long)gid, (unsigned long long)uid);
    if (minutes) {
        i64 t = wall_ms() / 1000 + (i64)minutes * 60;
        i64 days = t / 86400, sod = t % 86400, z = days + 719468, era = z / 146097, doe = z - era * 146097;
        i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        i64 mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
        snprintf(body, sizeof body, "{\"communication_disabled_until\":\"%04d-%02d-%02dT%02d:%02d:%02d+00:00\"}",
                 (int)y, (int)m, (int)d, (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
    } else snprintf(body, sizeof body, "{\"communication_disabled_until\":null}");
    mod_send("PATCH", path, body, reason, ui_tr("тайм-аут"));
    return true;
}
bool mod_kick(u64 gid, u64 uid, const char *reason, char *err, size_t errn) {
    if (!mod_pre(gid, uid, MOD_KICK, reason, err, errn)) return false;
    if (O.demo) { ui_notice(ui_tr("демо: участник выгнан")); return true; }
    char path[96];
    snprintf(path, sizeof path, "/guilds/%llu/members/%llu", (unsigned long long)gid, (unsigned long long)uid);
    mod_send("DELETE", path, NULL, reason, ui_tr("выгнать"));
    return true;
}
bool mod_ban(u64 gid, u64 uid, const char *reason, int delete_hours, char *err, size_t errn) {
    if (!mod_pre(gid, uid, MOD_BAN, reason, err, errn)) return false;
    if (delete_hours < 0 || delete_hours > 168) { snprintf(err, errn, ui_tr("удалить сообщения: за 0–168 часов")); return false; }
    if (O.demo) { ui_notice(ui_tr("демо: участник забанен")); return true; }
    char path[96], body[64];
    snprintf(path, sizeof path, "/guilds/%llu/bans/%llu", (unsigned long long)gid, (unsigned long long)uid);
    snprintf(body, sizeof body, "{\"delete_message_seconds\":%d}", delete_hours * 3600);
    mod_send("PUT", path, body, reason, ui_tr("бан"));
    return true;
}
bool mod_role(u64 gid, u64 uid, u64 rid, bool add, char *err, size_t errn) {
    if (!mod_pre(gid, uid, MOD_ROLES, NULL, err, errn)) return false;
    Guild *g = guild_find(gid);
    int my = -1, rp = -1;
    for (int i = 0; i < g->nroles; i++) {
        if (g->roles[i].id == rid) rp = g->roles[i].pos;
        for (int k = 0; k < g->nmy; k++) if (g->roles[i].id == g->my_roles[k] && g->roles[i].pos > my) my = g->roles[i].pos;
    }
    if (rp < 0 || rid == gid) { snprintf(err, errn, ui_tr("роль не найдена")); return false; }
    if (g->owner_id != S.me_id && rp >= my) { snprintf(err, errn, ui_tr("эта роль не ниже вашей — выдать нельзя")); return false; }
    if (O.demo) { ui_notice(add ? ui_tr("демо: роль выдана") : ui_tr("демо: роль снята")); return true; }
    char path[128];
    snprintf(path, sizeof path, "/guilds/%llu/members/%llu/roles/%llu", (unsigned long long)gid, (unsigned long long)uid, (unsigned long long)rid);
    http_async(add ? "PUT" : "DELETE", path, add ? "{}" : NULL, on_op, (void *)(add ? ui_tr("выдать роль") : ui_tr("снять роль")));
    return true;
}
bool mod_voice_move(u64 gid, u64 uid, u64 to, char *err, size_t errn) {
    if (!mod_pre(gid, uid, MOD_MOVE, NULL, err, errn)) return false;
    Channel *c = to ? chan_find(to) : NULL;
    if (to && (!c || c->guild_id != gid || (c->type != CH_VOICE && c->type != CH_STAGE))) { snprintf(err, errn, ui_tr("перенести можно в голосовой канал этого сервера")); return false; }
    if (O.demo) { ui_notice(to ? ui_tr("демо: перенесён") : ui_tr("демо: отключён от голоса")); return true; }
    char path[96], body[64];
    snprintf(path, sizeof path, "/guilds/%llu/members/%llu", (unsigned long long)gid, (unsigned long long)uid);
    if (to) snprintf(body, sizeof body, "{\"channel_id\":\"%llu\"}", (unsigned long long)to);
    else snprintf(body, sizeof body, "{\"channel_id\":null}");
    http_async("PATCH", path, body, on_op, (void *)(to ? ui_tr("перенести") : ui_tr("отключить от голоса")));
    return true;
}
bool mod_voice_mute(u64 gid, u64 uid, bool mute, bool deaf, char *err, size_t errn) {
    if (!mod_pre(gid, uid, MOD_MUTE, NULL, err, errn)) return false;
    if (O.demo) { ui_notice(ui_tr("демо: заглушка %s, глухота %s"), mute ? ui_tr("вкл") : ui_tr("выкл"), deaf ? ui_tr("вкл") : ui_tr("выкл")); return true; }
    char path[96], body[64];
    snprintf(path, sizeof path, "/guilds/%llu/members/%llu", (unsigned long long)gid, (unsigned long long)uid);
    snprintf(body, sizeof body, "{\"mute\":%s,\"deaf\":%s}", mute ? "true" : "false", deaf ? "true" : "false");
    http_async("PATCH", path, body, on_op, (void *)ui_tr("заглушка участника"));
    return true;
}

size_t ready_slim(char *s, size_t len) {
    const Sch *top = NULL;
    size_t lim = len < 64 ? len : 64;
    for (size_t i = 0; i + 4 < lim; i++) if (!memcmp(s + i, "\"t\":", 4)) {
        const char *p = s + i + 4;
        size_t rest = len - (size_t)(p - s);
        if (rest > 8 && !memcmp(p, "\"READY\"", 7)) top = S_TOP_READY;
        else if (rest > 21 && !memcmp(p, "\"READY_SUPPLEMENTAL\"", 20)) top = S_TOP_SUPP;
        break;
    }
    if (!top) return len;
    Slim x = { s, 0, 0, len, false };
    sl_ws(&x);
    if (x.r >= len || s[x.r] != '{') return len;
    sl_value(&x, top, 0);
    if (x.bad) return 0;                            /* текст уже частично переписан: разбирать нельзя */
    if (x.w < len) s[x.w] = 0;
    return x.w;
}

/* ---------- READY по одному серверу ----------
 * Текст (уже ужатый ready_slim) не разбирается целиком: сканер находит границы значений ключей "d",
 * элементов guilds[] и merged_members[], и cJSON строится по частям — голова, затем каждый сервер
 * отдельно (арена освобождается после каждого), затем хвост. Пик — дерево самой большой части. */
typedef struct { size_t k0, k1, v0, v1; } KSpan;          /* ключ с кавычками [k0,k1), значение [v0,v1) */
static int obj_spans(Slim *x, size_t at, KSpan *out, int max) {
    int n = 0;
    bool need = false;
    x->r = at + 1;
    for (;;) {
        sl_ws(x);
        if (x->r >= x->n) { x->bad = true; return n; }
        char c = x->s[x->r];
        if (c == '}') { x->r++; return n; }
        if (need) {
            if (c != ',') { x->bad = true; return n; }
            x->r++;
            sl_ws(x);
            if (x->r >= x->n) { x->bad = true; return n; }
            c = x->s[x->r];
        }
        if (c != '"') { x->bad = true; return n; }
        KSpan k;
        k.k0 = x->r;
        k.k1 = sl_skip(x, x->r);
        if (x->bad || k.k1 < k.k0 + 2) { x->bad = true; return n; }
        x->r = k.k1;
        sl_ws(x);
        if (x->r >= x->n || x->s[x->r] != ':') { x->bad = true; return n; }
        x->r++;
        sl_ws(x);
        k.v0 = x->r;
        k.v1 = sl_skip(x, x->r);
        if (x->bad || k.v1 <= k.v0) { x->bad = true; return n; }
        x->r = k.v1;
        need = true;
        if (n < max) out[n++] = k;
    }
}
static bool key_is(const Slim *x, const KSpan *k, const char *name) {
    size_t l = strlen(name);
    return k->k1 - k->k0 == l + 2 && !memcmp(x->s + k->k0 + 1, name, l);
}
/* Следующий элемент массива [v0,v1): *pos — позиция чтения (в начале — на '['). false — конец или bad. */
static bool arr_next(Slim *x, size_t *pos, size_t end, size_t *e0, size_t *e1) {
    x->r = *pos;
    if (x->r >= end) { x->bad = true; return false; }
    if (x->s[x->r] == '[') {
        x->r++;
        sl_ws(x);
        if (x->r < end && x->s[x->r] == ']') return false;          /* пустой массив */
    } else {
        sl_ws(x);
        if (x->r < end && x->s[x->r] == ']') return false;          /* конец */
        if (x->r >= end || x->s[x->r] != ',') { x->bad = true; return false; }
        x->r++;
        sl_ws(x);
    }
    if (x->r >= end || x->s[x->r] == ']' || x->s[x->r] == ',') { x->bad = true; return false; }
    *e0 = x->r;
    *e1 = sl_skip(x, x->r);
    if (x->bad || *e1 > end || *e1 <= *e0) { x->bad = true; return false; }
    *pos = *e1;
    return true;
}
static cJSON *parse_span(const char *s, size_t a, size_t b) { return json_parse(s + a, b - a); }
size_t g_ready_peak;                            /* --selftest: пик частной памяти внутри store_ready_text (0 — не мерить) */
static void peak_note(void) { if (g_ready_peak) { size_t m = plat_private_bytes(); if (m > g_ready_peak) g_ready_peak = m; } }
/* Собрать объект из выбранных ключей: {"k":v,...} (tail: только ключи хвоста, иначе — все, кроме
 * серверов, участников и хвоста). */
static void build_part(const Slim *x, const KSpan *ks, int nk, bool tail, Buf *out) {
    static const char *const SKIP[] = { "guilds", "merged_members" };
    static const char *const TAIL[] = { "private_channels", "read_state", "user_guild_settings" };
    buf_put(out, "{", 1);
    bool first = true;
    for (int i = 0; i < nk; i++) {
        bool is_tail = false, skip = false;
        for (size_t t = 0; t < 3; t++) if (key_is(x, &ks[i], TAIL[t])) is_tail = true;
        for (size_t t = 0; t < 2; t++) if (key_is(x, &ks[i], SKIP[t])) skip = true;
        if (skip || is_tail != tail) continue;
        if (!first) buf_put(out, ",", 1);
        first = false;
        buf_put(out, x->s + ks[i].k0, ks[i].k1 - ks[i].k0);
        buf_put(out, ":", 1);
        buf_put(out, x->s + ks[i].v0, ks[i].v1 - ks[i].v0);
    }
    buf_put(out, "}", 1);
}
bool store_ready_text(char *s, size_t len, char *resume, size_t rn) {
    if (resume && rn) resume[0] = 0;
    Slim x = { s, 0, 0, len, false };
    KSpan top[8], ks[48];
    sl_ws(&x);
    if (x.r >= len || s[x.r] != '{') return false;
    int nt = obj_spans(&x, x.r, top, 8);
    const KSpan *d = NULL;
    for (int i = 0; i < nt; i++) if (key_is(&x, &top[i], "d")) d = &top[i];
    if (x.bad || !d || s[d->v0] != '{') return false;
    int nk = obj_spans(&x, d->v0, ks, 48);
    if (x.bad) return false;
    const KSpan *g = NULL, *m = NULL;
    for (int i = 0; i < nk; i++) {
        if (key_is(&x, &ks[i], "guilds")) g = &ks[i];
        else if (key_is(&x, &ks[i], "merged_members")) m = &ks[i];
    }
    /* голова */
    Buf part = {0};
    build_part(&x, ks, nk, false, &part);
    cJSON *j = json_parse(part.p, part.len);
    if (!j) { json_done(); buf_free(&part); return false; }
    peak_note();
    ready_head(j);
    const char *ru = jstr(j, "resume_gateway_url");
    if (ru && resume && rn) snprintf(resume, rn, "%s", ru);
    json_done();
    buf_free(&part);
    /* серверы по одному (с участниками merged_members[i]) */
    size_t gp = g ? g->v0 : 0, mp = m ? m->v0 : 0, g0, g1, m0 = 0, m1 = 0;
    while (g && s[g->v0] == '[' && arr_next(&x, &gp, g->v1, &g0, &g1)) {
        bool hm = m && s[m->v0] == '[' && arr_next(&x, &mp, m->v1, &m0, &m1);
        if (hm) {
            /* сервер и его участники — одной арене: "[guild,members]" (копия текста одного сервера) */
            Buf b = {0};
            buf_put(&b, "[", 1);
            buf_put(&b, s + g0, g1 - g0);
            buf_put(&b, ",", 1);
            buf_put(&b, s + m0, m1 - m0);
            buf_put(&b, "]", 1);
            cJSON *pair = json_parse(b.p, b.len);
            peak_note();
            if (pair) ready_guild(cJSON_GetArrayItem(pair, 0), cJSON_GetArrayItem(pair, 1));
            json_done();
            buf_free(&b);
        } else {
            cJSON *go = parse_span(s, g0, g1);
            if (go) ready_guild(go, NULL);
            json_done();
        }
    }
    if (x.bad) { S.ready = false; return false; }
    /* хвост */
    build_part(&x, ks, nk, true, &part);
    j = json_parse(part.p, part.len);
    peak_note();
    ready_tail(j ? j : NULL);
    json_done();
    buf_free(&part);
    return true;
}

/* ---------- отсев событий до разбора ---------- */
static const char *const WANTED[] = {
    "READY", "READY_SUPPLEMENTAL", "RESUMED", "MESSAGE_CREATE", "MESSAGE_UPDATE", "MESSAGE_DELETE", "MESSAGE_DELETE_BULK",
    "MESSAGE_REACTION_ADD", "MESSAGE_REACTION_REMOVE", "MESSAGE_REACTION_REMOVE_ALL", "MESSAGE_REACTION_REMOVE_EMOJI",
    "MESSAGE_ACK", "TYPING_START", "CHANNEL_CREATE", "CHANNEL_UPDATE", "CHANNEL_DELETE", "CHANNEL_RECIPIENT_ADD",
    "CHANNEL_RECIPIENT_REMOVE", "GUILD_CREATE", "GUILD_UPDATE", "GUILD_DELETE", "GUILD_ROLE_CREATE",
    "GUILD_ROLE_UPDATE", "GUILD_ROLE_DELETE", "GUILD_MEMBER_UPDATE", "VOICE_STATE_UPDATE", "VOICE_SERVER_UPDATE",
    "CALL_CREATE", "CALL_UPDATE", "CALL_DELETE", "USER_GUILD_SETTINGS_UPDATE",
#ifdef ASTRA_VIDEO
    "STREAM_CREATE", "STREAM_SERVER_UPDATE", "STREAM_DELETE",
#endif
    "RELATIONSHIP_ADD", "RELATIONSHIP_UPDATE", "RELATIONSHIP_REMOVE", "CHANNEL_PINS_UPDATE",
    "MESSAGE_POLL_VOTE_ADD", "MESSAGE_POLL_VOTE_REMOVE",
    "THREAD_CREATE", "THREAD_UPDATE", "THREAD_DELETE", "THREAD_LIST_SYNC", "THREAD_MEMBER_UPDATE", "THREAD_MEMBERS_UPDATE",
    "INTERACTION_SUCCESS", "INTERACTION_FAILURE", "USER_SETTINGS_PROTO_UPDATE",
    "VOICE_CHANNEL_STATUS_UPDATE", "CHANNEL_INFO",
};
/* PRESENCE_UPDATE: нужен, только если это друг. Без разбора JSON: находим "user":{ и его "id"
 * на первом уровне вложенности, двоичный поиск по друзьям. Не нашли — отбрасываем. */
static bool presence_is_friend(const char *raw, size_t len) {
    if (!S.nrels || len > (64u << 10)) return false;
    static const char K[] = "\"user\":{";
    const char *end = raw + len, *p = NULL;
    for (const char *q = raw; q + sizeof K - 1 <= end; q++) if (!memcmp(q, K, sizeof K - 1)) { p = q + sizeof K - 1; break; }
    if (!p) return false;
    int depth = 1;
    while (p < end && depth > 0) {
        if (*p == '"') {
            if (depth == 1 && end - p > 6 && !memcmp(p, "\"id\":\"", 6)) {
                p += 6;
                u64 id = 0;
                int n = 0;
                while (p < end && *p >= '0' && *p <= '9' && n < 20) { id = id * 10 + (u64)(*p - '0'); p++; n++; }
                if (p >= end || *p != '"') return false;
                const Relation *r = rel_find(id);
                return r && r->type == REL_FRIEND;
            }
            for (p++; p < end && *p != '"'; p++) if (*p == '\\' && p + 1 < end) p++;    /* пропустить строку */
            p++;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }
    return false;
}
bool event_wanted(const char *raw, size_t len) {
    /* Discord кладёт "t" первым полем: {"t":"MESSAGE_CREATE","s":..}. Если не нашли — разбираем. */
    size_t lim = len < 48 ? len : 48;
    for (size_t i = 0; i + 4 < lim; i++) {
        if (memcmp(raw + i, "\"t\":", 4)) continue;
        const char *p = raw + i + 4;
        if (*p != '"') return true;              /* "t":null — служебный пакет, нужен */
        p++;
        size_t n = 0;
        while (p + n < raw + len && p[n] != '"' && n < 40) n++;
        if (n == 15 && !memcmp(p, "PRESENCE_UPDATE", 15)) return presence_is_friend(raw, len);
        for (size_t k = 0; k < sizeof WANTED / sizeof *WANTED; k++)
            if (strlen(WANTED[k]) == n && !memcmp(WANTED[k], p, n)) return true;
        return false;
    }
    return true;
}
