/* memlist.c — список участников сервера справа, как в официальном клиенте. Включается в конец store.c
 * (как voice_stream.c в voice.c): пользуется его san-копиями, ролями, display_name, member_color.
 *
 * Шлюз. Подписка — op 14 {"guild_id", "channels":{"<канал>":[[0,99],[100,199]]}} (net.c gw_subscribe_ranges).
 * Ответ и дальнейшие изменения — GUILD_MEMBER_LIST_UPDATE:
 *   {"id":"everyone"|"<murmur3 прав канала>", "guild_id", "member_count", "online_count",
 *    "groups":[{"id":"<роль>"|"online"|"offline","count":N}, ...],
 *    "ops":[{"op":"SYNC","range":[a,b],"items":[...]}, {"op":"INSERT"|"UPDATE","index":i,"item":{...}},
 *           {"op":"DELETE","index":i}, {"op":"INVALIDATE","range":[a,b]}]}
 *   элемент — {"group":{"id","count"}} или {"member":{"user":{...},"roles":[...],"nick","presence":{...}}}.
 * Индексы — по всему списку, заголовок группы — тоже строка. Храним только участников в подписанных окнах
 * (до 3 окон по 100: всегда [0,99] и окна вокруг видимого); заголовки не храним — их места вычисляются из
 * groups. Смена канала с другим списком или сервера — всё затирается и освобождается. */

#define ML_MAXIDX 1000000                        /* строк в списке больше не бывает: мусорные индексы — мимо */
#define ML_MAXG   260                            /* групп: роли (до 250) + «в сети» + «не в сети» */
#define ML_MAXOPS 64                             /* операций в одном событии (обычно 1..3) */
#define ML_MAXITEMS (ML_WIN * 3)                 /* элементов в одном SYNC */
#define ML_GAP    250                            /* мс между переподписками (прокрутка колесом) */

typedef struct { int idx; MemRow r; } MLRow;
static struct {
    u64 gid, cid;                                /* сервер и открытый канал */
    char want[24];                               /* id списка, вычисленный по правам канала */
    char lid[24];                                /* id списка, принятый из SYNC ("" — данных ещё нет) */
    int sub[3], nsub;                            /* подписанные окна (номера по 100), sub[0] == 0 */
    bool pend;                                   /* нужно отправить подписку (демо — синтетический SYNC) */
    i64 sent_at;
    MLRow *rows; int n, cap;                     /* участники по возрастанию idx */
    struct { u64 id; int count; } g[ML_MAXG]; int ng;
    int total, online, members;
} ML;
static MemRow ml_tmp;                            /* заголовок группы для ml_row */

/* ---------- id списка: "everyone" или murmur3_32 от «allow:id,deny:id» по праву VIEW_CHANNEL ---------- */
static u32 ml_rotl(u32 x, int r) { return x << r | x >> (32 - r); }
static u32 ml_murmur3(const unsigned char *p, size_t n) {
    const u32 c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    u32 h = 0;
    size_t nb = n / 4;
    for (size_t i = 0; i < nb; i++) {
        u32 k = (u32)p[4 * i] | (u32)p[4 * i + 1] << 8 | (u32)p[4 * i + 2] << 16 | (u32)p[4 * i + 3] << 24;
        k *= c1; k = ml_rotl(k, 15); k *= c2;
        h ^= k; h = ml_rotl(h, 13); h = h * 5 + 0xe6546b64u;
    }
    const unsigned char *t = p + nb * 4;
    u32 k = 0;
    switch (n & 3) {
    case 3: k ^= (u32)t[2] << 16; /* fall through */
    case 2: k ^= (u32)t[1] << 8;  /* fall through */
    case 1: k ^= t[0]; k *= c1; k = ml_rotl(k, 15); k *= c2; h ^= k; break;
    default: break;
    }
    h ^= (u32)n;
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}
static void ml_list_id(const Channel *c, u64 gid, char out[24]) {
    snprintf(out, 24, "everyone");
    if (c && (c->type == CH_THREAD || c->type == CH_PRIV_THREAD || c->type == CH_NEWS_THREAD) && c->parent_id) {
        const Channel *p = chan_find(c->parent_id);
        if (p) c = p;                            /* у ветки — список родителя */
    }
    if (!c) return;
    const Guild *g = guild_find(gid);
    u64 ev = 0;
    for (int i = 0; g && i < g->nroles; i++) if (g->roles[i].id == gid) ev = g->roles[i].perms;
    if (ev & P_ADMIN) return;
    for (int i = 0; i < c->now_; i++) if (c->ow[i].type == 0 && c->ow[i].id == gid) { ev &= ~c->ow[i].deny; ev |= c->ow[i].allow; }
    if (ev & P_VIEW) return;                     /* канал виден всем — общий список сервера */
    Buf b = {0};
    for (int i = 0; i < c->now_; i++) {
        const Overwrite *w = &c->ow[i];
        const char *k = w->allow & P_VIEW ? "allow" : w->deny & P_VIEW ? "deny" : NULL;
        if (k) buf_printf(&b, "%s%s:%llu", b.len ? "," : "", k, (unsigned long long)w->id);
    }
    snprintf(out, 24, "%u", (unsigned)ml_murmur3((const unsigned char *)(b.p ? b.p : ""), b.len));
    buf_free(&b);
}

/* ---------- хранение строк ---------- */
static bool ml_in_sub(int idx) {
    int c = idx / ML_WIN;
    for (int k = 0; k < ML.nsub; k++) if (ML.sub[k] == c) return true;
    return false;
}
static int ml_lower(int idx) {                   /* первая строка с idx >= заданного */
    int lo = 0, hi = ML.n;
    while (lo < hi) { int m = (lo + hi) / 2; if (ML.rows[m].idx < idx) lo = m + 1; else hi = m; }
    return lo;
}
static void ml_remove_at(int i, int cnt) {       /* убрать cnt строк с позиции i, хвост затереть */
    if (cnt <= 0) return;
    memmove(&ML.rows[i], &ML.rows[i + cnt], (size_t)(ML.n - i - cnt) * sizeof(MLRow));
    ML.n -= cnt;
    wipe(&ML.rows[ML.n], (size_t)cnt * sizeof(MLRow));
}
static void ml_del_range(int a, int b) {
    int i = ml_lower(a), j = ml_lower(b + 1);
    ml_remove_at(i, j - i);
}
static void ml_shift(int from, int d) {
    for (int i = ml_lower(from); i < ML.n; i++) ML.rows[i].idx += d;
}
static void ml_prune(void) {                     /* строки вне подписанных окон не храним */
    int o = 0;
    for (int i = 0; i < ML.n; i++) {
        if (ML.rows[i].idx < 0 || ML.rows[i].idx >= ML_MAXIDX || !ml_in_sub(ML.rows[i].idx)) continue;
        if (o != i) ML.rows[o] = ML.rows[i];
        o++;
    }
    if (o < ML.n) wipe(&ML.rows[o], (size_t)(ML.n - o) * sizeof(MLRow));
    ML.n = o;
}
static void ml_put(int idx, const MemRow *r) {
    if (idx < 0 || idx >= ML_MAXIDX || !ml_in_sub(idx)) return;
    int i = ml_lower(idx);
    if (i < ML.n && ML.rows[i].idx == idx) { ML.rows[i].r = *r; return; }
    if (ML.n == ML.cap) {
        int nc = ML.cap ? ML.cap * 2 : 64;
        if (nc > ML_WIN * 3 + 4) nc = ML_WIN * 3 + 4;
        if (nc <= ML.cap) return;                /* не бывает: строк не больше, чем в трёх окнах */
        MLRow *nr = xcalloc((size_t)nc, sizeof(MLRow));
        if (ML.n) memcpy(nr, ML.rows, (size_t)ML.n * sizeof(MLRow));
        if (ML.rows) { wipe(ML.rows, (size_t)ML.cap * sizeof(MLRow)); free(ML.rows); }
        ML.rows = nr;
        ML.cap = nc;
    }
    memmove(&ML.rows[i + 1], &ML.rows[i], (size_t)(ML.n - i) * sizeof(MLRow));
    ML.rows[i].idx = idx;
    ML.rows[i].r = *r;
    ML.n++;
}

/* ---------- разбор ---------- */
static bool ml_num(const cJSON *v, int lim, int *out) {
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0) || v->valuedouble >= lim) return false;
    *out = (int)v->valuedouble;
    return true;
}
static bool ml_range(const cJSON *o, int *a, int *b) {
    const cJSON *r = jget(o, "range");
    if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 2) return false;
    if (!ml_num(cJSON_GetArrayItem(r, 0), ML_MAXIDX, a) || !ml_num(cJSON_GetArrayItem(r, 1), ML_MAXIDX, b)) return false;
    return *a <= *b && *b - *a < 1000;
}
static void ml_copy(char *dst, size_t n, const char *s) {
    char *t = san(s ? s : "", false);
    for (char *q = t; *q; q++) if (*q == '\n' || *q == '\t') *q = ' ';
    utf8_trunc(t, n - 1);
    snprintf(dst, n, "%s", t);
    free(t);
}
static u8 ml_status(const char *s) {
    if (!s) return PS_OFFLINE;
    if (!strcmp(s, "online")) return PS_ONLINE;
    if (!strcmp(s, "idle")) return PS_IDLE;
    if (!strcmp(s, "dnd")) return PS_DND;
    return PS_OFFLINE;
}
static u64 ml_group_id(const cJSON *v) {
    if (!cJSON_IsString(v) || !v->valuestring) return 0;
    if (!strcmp(v->valuestring, "online")) return ML_G_ONLINE;
    if (!strcmp(v->valuestring, "offline")) return ML_G_OFFLINE;
    u64 id = id_of(v);
    return id > ML_G_OFFLINE ? id : 0;
}
static void ml_groups(const cJSON *arr) {
    if (!cJSON_IsArray(arr)) return;             /* нет поля — прежние группы в силе */
    ML.ng = 0;
    i64 tot = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (ML.ng >= ML_MAXG) break;
        u64 id = ml_group_id(jget(e, "id"));
        int cnt = 0;
        if (!id || !ml_num(jget(e, "count"), ML_MAXIDX, &cnt)) continue;
        ML.g[ML.ng].id = id;
        ML.g[ML.ng].count = cnt;
        ML.ng++;
        if (cnt > 0) tot += cnt + 1;
    }
    ML.total = tot > ML_MAXIDX ? ML_MAXIDX : (int)tot;
}
/* Что показать под именем: свой статус (тип 4) главнее, иначе первая игра/музыка/трансляция. */
static void ml_activity(MemRow *r, const cJSON *pres) {
    static const u8 KIND[6] = { MLA_PLAY, MLA_STREAM, MLA_LISTEN, MLA_WATCH, MLA_CUSTOM, MLA_COMPETE };
    const char *best = NULL;
    u8 kind = MLA_NONE;
    int k = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, jget(pres, "activities")) {
        if (k++ >= 16) break;
        i64 ty = jint(a, "type", -1);
        if (ty < 0 || ty > 5) continue;
        if (ty == 4) {
            const char *st = jstr(a, "state");
            if (st && *st) { best = st; kind = MLA_CUSTOM; break; }
            continue;
        }
        const char *nm = jstr(a, "name");
        if (!best && nm && *nm) { best = nm; kind = KIND[ty]; }
    }
    r->act = kind;
    r->text[0] = 0;
    if (best) ml_copy(r->text, sizeof r->text, best);
    if (!r->text[0]) r->act = MLA_NONE;
}
static bool ml_member(MemRow *r, const cJSON *m, const Guild *g) {
    const cJSON *u = jget(m, "user");
    memset(r, 0, sizeof *r);
    r->id = jid(u, "id");
    if (!r->id || !cJSON_IsObject(u)) return false;
    r->kind = MLR_MEMBER;
    const char *nick = jstr(m, "nick");
    ml_copy(r->name, sizeof r->name, nick && *nick ? nick : display_name(u));
    hash_copy(r->avatar, jstr(u, "avatar"));
    r->bot = jbool(u, "bot");
    r->color = g ? member_color(g, m) : 0;
    const cJSON *p = jget(m, "presence");
    r->status = ml_status(jstr(p, "status"));
    ml_activity(r, p);
    return true;
}
/* Элемент списка: участник -> строка; группа -> ничего (заголовки — из groups); мусор -> false. */
static bool ml_item(const cJSON *it, const Guild *g, MemRow *out) {
    const cJSON *m = jget(it, "member");
    return m && ml_member(out, m, g);
}

static void ml_event(const cJSON *d) {
    if (!ML.gid || !cJSON_IsObject(d) || jid(d, "guild_id") != ML.gid) return;
    const char *id = jstr(d, "id");
    if (!id || !*id || strlen(id) >= sizeof ML.lid) return;
    const cJSON *ops = jget(d, "ops"), *o;
    if (!cJSON_IsArray(ops)) return;
    if (strcmp(id, ML.lid)) {
        /* Чужой список: принимаем только SYNC и только ожидаемого id (или первого после смены канала:
         * вдруг вычисление id по правам разошлось с Discord). Иначе — мимо, ничего не трогаем. */
        bool sync = false;
        cJSON_ArrayForEach(o, ops) { const char *t = jstr(o, "op"); if (t && !strcmp(t, "SYNC")) sync = true; }
        if (!sync || (ML.lid[0] && strcmp(id, ML.want))) return;
        if (ML.n) ml_remove_at(0, ML.n);
        snprintf(ML.lid, sizeof ML.lid, "%s", id);
    }
    int v;
    if (ml_num(jget(d, "member_count"), ML_MAXIDX * 100, &v)) ML.members = v;
    if (ml_num(jget(d, "online_count"), ML_MAXIDX * 100, &v)) ML.online = v;
    ml_groups(jget(d, "groups"));
    const Guild *g = guild_find(ML.gid);
    int nops = 0;
    cJSON_ArrayForEach(o, ops) {
        if (nops++ >= ML_MAXOPS) break;
        const char *t = jstr(o, "op");
        if (!t) continue;
        int a, b, i;
        MemRow r;
        if (!strcmp(t, "SYNC")) {
            if (!ml_range(o, &a, &b)) continue;
            ml_del_range(a, b);
            int k = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, jget(o, "items")) {
                if (k >= ML_MAXITEMS || a + k > b) break;
                if (ml_item(it, g, &r)) ml_put(a + k, &r);
                k++;
            }
        } else if (!strcmp(t, "INVALIDATE")) {
            if (ml_range(o, &a, &b)) ml_del_range(a, b);
        } else if (!strcmp(t, "INSERT")) {
            if (!ml_num(jget(o, "index"), ML_MAXIDX, &i)) continue;
            ml_shift(i, 1);
            if (ml_item(jget(o, "item"), g, &r)) ml_put(i, &r);
        } else if (!strcmp(t, "UPDATE")) {
            if (!ml_num(jget(o, "index"), ML_MAXIDX, &i)) continue;
            if (ml_item(jget(o, "item"), g, &r)) ml_put(i, &r);
            else ml_del_range(i, i);             /* стала группой или мусор — строки участника там больше нет */
        } else if (!strcmp(t, "DELETE")) {
            if (!ml_num(jget(o, "index"), ML_MAXIDX, &i)) continue;
            ml_del_range(i, i);
            ml_shift(i + 1, -1);
        }
    }
    ml_prune();
    ui_dirty();
}

/* Шлюз шлёт списки и старых подписок: отсев до разбора — только событие нашего сервера. */
static bool ml_want_raw(const char *raw, size_t len) {
    if (!ML.gid || O.cli) return false;
    char k[48];
    int kn = snprintf(k, sizeof k, "\"guild_id\":\"%llu\"", (unsigned long long)ML.gid);
    if (kn <= 0 || (size_t)kn > len) return false;
    for (const char *p = raw, *end = raw + len - kn; p <= end; p++) {
        p = memchr(p, '"', (size_t)(end - p) + 1);
        if (!p) return false;
        if (!memcmp(p, k, (size_t)kn)) return true;
    }
    return false;
}

/* ---------- жизненный цикл ---------- */
void ml_close(void) {
    if (ML.rows) { wipe(ML.rows, (size_t)ML.cap * sizeof(MLRow)); free(ML.rows); }
    wipe(&ML, sizeof ML);
    wipe(&ml_tmp, sizeof ml_tmp);
    ML.total = -1;
}
void ml_open(u64 gid, u64 cid) {
    if (!gid || !cid || O.cli) { if (ML.gid) ml_close(); return; }
    char want[24];
    ml_list_id(chan_find(cid), gid, want);
    if (ML.gid == gid && !strcmp(want, ML.want)) {
        /* тот же список (например, «everyone»): строки остаются, подписку переносим на новый канал */
        if (ML.cid != cid && !O.demo) ML.pend = true;
        ML.cid = cid;
        return;
    }
    ml_close();
    ML.gid = gid;
    ML.cid = cid;
    snprintf(ML.want, sizeof ML.want, "%s", want);
    ML.sub[0] = 0;
    ML.nsub = 1;
    ML.pend = true;
}
void ml_view(int first, int last) {
    if (!ML.gid) return;
    if (first < 0) first = 0;
    if (last < first) last = first;
    if (first >= ML_MAXIDX) first = ML_MAXIDX - 1;
    if (last >= ML_MAXIDX) last = ML_MAXIDX - 1;
    int a = first / ML_WIN, b = last / ML_WIN;
    if (b > a + 1) b = a + 1;                    /* на экране меньше 100 строк — хватает двух окон */
    int s[3] = { 0 }, ns = 1;
    if (a != 0) s[ns++] = a;
    if (b != 0 && b != a) s[ns++] = b;
    bool same = ns == ML.nsub;
    for (int k = 0; same && k < ns; k++) if (s[k] != ML.sub[k]) same = false;
    if (same) return;
    memcpy(ML.sub, s, sizeof s);
    ML.nsub = ns;
    ml_prune();
    ML.pend = true;
}
static void ml_tick(i64 now) {
    if (!ML.gid || !ML.pend || now - ML.sent_at < ML_GAP) return;
    if (O.demo) {
        ML.pend = false;
        ML.sent_at = now;
        demo_memlist(ML.gid, ML.want, ML.sub, ML.nsub);
        return;
    }
    if (!S.ready || !gw_online()) return;        /* нет шлюза: подписка уйдёт после READY (ml_resub) */
    gw_subscribe_ranges(ML.gid, ML.cid, ML.sub, ML.nsub);
    ML.pend = false;
    ML.sent_at = now;
}
static i64 ml_next_wake(void) {
    if (!ML.gid || !ML.pend) return INT64_MAX;
    if (!O.demo && !(S.ready && gw_online())) return INT64_MAX;
    return ML.sent_at + ML_GAP;
}
static void ml_resub(void) {
    if (O.cli) return;
    if (ML.gid) { ML.pend = true; ML.sent_at = 0; return; }
    const Channel *c = chan_find(S.open_ch);     /* READY пересоздал модель: открытый канал подписать снова */
    if (c && c->guild_id && c->type != CH_FORUM && c->type != CH_MEDIA) ml_open(c->guild_id, c->id);
}
static void ml_guild_gone(u64 gid) { if (ML.gid && ML.gid == gid) ml_close(); }

/* ---------- для интерфейса ---------- */
int ml_total(u64 cid) {
    if (!ML.gid || cid != ML.cid || !ML.lid[0]) return -1;
    return ML.total;
}
int ml_online(void) { return ML.online; }
int ml_member_count(void) { return ML.members; }
int ml_rows_held(void) { return ML.n; }
const MemRow *ml_row(int idx) {
    if (!ML.gid || idx < 0 || idx >= ML.total) return NULL;
    int pos = 0;
    for (int k = 0; k < ML.ng; k++) {
        if (ML.g[k].count <= 0) continue;
        if (idx == pos) {
            memset(&ml_tmp, 0, sizeof ml_tmp);
            ml_tmp.kind = MLR_GROUP;
            ml_tmp.id = ML.g[k].id;
            ml_tmp.count = ML.g[k].count;
            if (ML.g[k].id > ML_G_OFFLINE) {
                const Guild *g = guild_find(ML.gid);
                const char *nm = NULL;
                for (int i = 0; g && i < g->nroles; i++) if (g->roles[i].id == ML.g[k].id) { nm = g->roles[i].name; ml_tmp.color = g->roles[i].color; }
                if (!nm) nm = role_name(ML.gid, ML.g[k].id);
                if (nm) snprintf(ml_tmp.name, sizeof ml_tmp.name, "%s", nm);
            }
            return &ml_tmp;
        }
        pos += ML.g[k].count + 1;
        if (idx < pos) break;
    }
    int i = ml_lower(idx);
    return i < ML.n && ML.rows[i].idx == idx ? &ML.rows[i].r : NULL;
}
static const MemRow *ml_find_user(u64 uid) {
    for (int i = 0; uid && i < ML.n; i++) if (ML.rows[i].r.id == uid) return &ML.rows[i].r;
    return NULL;
}
static const char *ml_avatar(u64 uid) {
    const MemRow *r = ml_find_user(uid);
    return r && r->avatar[0] ? r->avatar : NULL;
}
static const char *ml_name(u64 uid) {
    const MemRow *r = ml_find_user(uid);
    return r && r->name[0] ? r->name : NULL;
}

/* только для selftest */
u32 ml_test_murmur(const char *s);
const char *ml_test_list_id(void);
u32 ml_test_murmur(const char *s) { return ml_murmur3((const unsigned char *)s, strlen(s)); }
const char *ml_test_list_id(void) { return ML.want; }
size_t ml_test_bytes(void);
size_t ml_test_bytes(void) { return (size_t)ML.cap * sizeof(MLRow) + sizeof ML; }
