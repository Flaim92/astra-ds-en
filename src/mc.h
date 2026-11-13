/* mincord — минимальный клиент Discord на C. Общие типы и интерфейсы модулей.
 * Принципы: безопасность > нулевое логирование > минимум памяти и процессора. */
#ifndef MC_H
#define MC_H
#ifdef __APPLE__
/* macOS: эти имена уже объявлены в заголовках mach (их тянут Cocoa и mach/mach.h) — у нас свои символы */
#define thread_create astra_thread_create
#define panic_init astra_panic_init
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../third_party/cJSON.h"
#include "plat.h"

typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define DISCORD_EPOCH_MS 1420070400000LL

/* ======================= util.c ======================= */
typedef struct { char *p; size_t len, cap; } Buf;
void buf_put(Buf *b, const void *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_json_str(Buf *b, const char *s);       /* "строка" с экранированием JSON */
void buf_free(Buf *b);
void buf_wipe(Buf *b);                          /* затереть и освободить */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
i64 now_ms(void);                               /* монотонные миллисекунды */
extern i64 g_clock_skew;                        /* selftest: сдвиг now_ms */
extern long g_http_fake_status;                 /* selftest: ответ на запрос, отклонённый g_http_guard */
i64 wall_ms(void);                              /* Unix-время в мс */
void wipe(void *p, size_t n);

/* UTF-8 и ширина в клетках терминала */
int utf8_dec(const char *s, u32 *cp);           /* длина символа (>=1); на ошибке cp = U+FFFD */
int utf8_enc(u32 cp, char *out);
int cp_width(u32 cp);
int str_width(const char *s);
/* Очистка текста из сети: вырезает управляющие символы (ESC и пр.) и смену направления
 * текста, чинит битый UTF-8. multiline: сохранять '\n'. Результат — новая строка. */
char *san(const char *s, bool multiline);
bool text_unsafe(const char *s);                /* есть C0/C1/DEL/BiDi/битый UTF-8 (для путей перетаскивания) */
/* А1: строка-вложение в Message.content начинается с ATT_MARK (san его всегда вырезает из текста сети). */
#define ATT_MARK '\x01'
#define ATT_MARK_S "\x01"
bool cdn_host_ok(const char *url);              /* https://cdn.discordapp.com/ или media.discordapp.net/ */

/* JSON: разбор в одноразовой куче потока; json_done() уничтожает её целиком */
cJSON *json_parse(const char *text, size_t len);
void json_done(void);
const char *jstr(const cJSON *o, const char *key);
u64 jid(const cJSON *o, const char *key);
i64 jint(const cJSON *o, const char *key, i64 def);
bool jbool(const cJSON *o, const char *key);
u64 id_of(const cJSON *v);
const cJSON *jget(const cJSON *o, const char *key);

void base64_enc(const void *in, size_t n, Buf *out);
void url_encode(const char *s, Buf *out);
i64 iso_to_unix(const char *iso);
void fmt_time(i64 unix_s, char *out, size_t n);
u64 snowflake_now(void);

/* ======================= secret.c (токен в закреплённой памяти) ======================= */
bool secret_init(void);                          /* выделить страницы и VirtualLock */
void secret_set(const char *token, size_t n);    /* скопировать токен в защищённую память */
const char *secret_token(void);
const char *secret_auth_header(void);            /* "Authorization: <token>" */
char *secret_scratch(size_t *cap);               /* защищённый буфер для IDENTIFY/RESUME */
void secret_destroy(void);
bool cred_load(void);                            /* из системного хранилища секретов */
bool cred_save(void);
void cred_delete(void);
bool read_hidden(const char *prompt);            /* скрытый ввод токена в консоли */

/* ======================= store.c ======================= */
enum { CH_TEXT = 0, CH_DM = 1, CH_VOICE = 2, CH_GROUP = 3, CH_CATEGORY = 4, CH_NEWS = 5, CH_STAGE = 13,
       CH_NEWS_THREAD = 10, CH_THREAD = 11, CH_PRIV_THREAD = 12, CH_FORUM = 15, CH_MEDIA = 16 };

typedef struct { u64 id; char *name; char avatar[35]; u32 color; u64 color_gid; } User;  /* color: цвет ника (0xRRGGBB, 0 — обычный) на сервере color_gid */   /* avatar: 32 hex или a_+32 hex, иначе "" */
typedef struct { u64 id; u64 perms; u32 color; int pos; char *name; bool hoist; } Role;   /* name — только у ролей, показанных отдельно (hoist), иначе NULL */
typedef struct { u64 id; u64 allow, deny; u8 type; } Overwrite;
typedef struct {
    u64 id, guild_id, parent_id, last_msg, last_read;
    u64 owner_id;                               /* №111: владелец группы ЛС / ветки (0 — нет) */
    int type, position, mentions;
    bool visible, can_send, can_connect, can_speak, can_history, muted;   /* can_history: право читать историю */
    u8 notify;                                  /* уровень уведомлений канала: NOTIFY_* (3 — как у сервера) */
    bool slow_exempt;                           /* №127: право «управлять сообщениями/каналом» — медленный режим не действует */
    u16 slowmode;                               /* №127: rate_limit_per_user, секунды (0 — нет) */
    u8 tflags;                                  /* №90: ветка — TF_JOINED / TF_ARCHIVED / TF_LOCKED */
    u8 dmflags;                                 /* №109: ЛС — DMF_REQUEST (запрос на переписку) / DMF_SPAM */
    char *name;
    char *topic;                                /* тема (san, одна строка, <= TOPIC_MAX байт); только у ОТКРЫТОГО канала, NULL — нет/ещё не пришла */
    u64 *recipients; int nrecip;
    Overwrite *ow; int now_;
} Channel;
#define TOPIC_MAX 96
typedef struct {
    u64 id, owner_id;
    char *name;
    Role *roles; int nroles;
    u64 *my_roles; int nmy;
    bool muted;
    u8 notify;                                  /* уровень уведомлений сервера: NOTIFY_* (3 — по умолчанию сервера) */
    u32 banner_color;                           /* №4: у сервера есть баннер — цвет полосы (0xRRGGBB), 0 — нет; картинку не грузим */
    bool pending;                               /* №128: правила сервера ещё не приняты (писать нельзя) */
    i64 timeout_until;                          /* №127: мой тайм-аут (communication_disabled_until), unix-секунды; 0 — нет */
    char icon[35];                              /* хэш значка сервера: 32 hex или a_+32 hex, иначе "" */
    i64 joined;                                 /* когда я вступил (joined_at, unix-секунды; 0 — неизвестно) */
} Guild;

typedef struct {                                /* кто в каком голосовом канале */
    u64 guild_id, channel_id, user_id;
    char *name;
    bool self_mute, self_deaf, mute, deaf;
    bool stream;                                /* транслирует экран (self_stream) */
    bool video;                                 /* включена камера (self_video) */
} VoiceState;

typedef struct { char *emoji; int count; bool me; } Reaction;
typedef struct Embed Embed;                     /* пакет Б №53 (ниже) */
typedef struct Poll Poll;                       /* пакет Б №41 (ниже) */
typedef struct Comps Comps;                     /* пакет Б №55 (ниже) */
typedef struct {
    u64 id, author_id, ref_id;
    char *author, *content, *ref_author, *ref_text;
    char *raw;                                  /* только у СВОИХ: исходный текст для правки (san, <@id> <#id> <:e:id> не раскрыты), иначе NULL */
    i64 ts;
    bool edited;
    u8 state;                                   /* MS_SENT / MS_PENDING (ушло, ждём) / MS_FAILED (не отправлено) */
    bool bot, webhook, forwarded;               /* author.bot; webhook_id; пересланное (текст оригинала — в content) */
    bool pinned;                                /* закреплено (поле pinned; обновляется по ответу msg_pin) */
    u32 flags;                                  /* флаги сообщения Discord (4096 — тихое, 8192 — голосовое) */
    u32 color;                                  /* цвет ника автора по старшей роли, 0 — обычный */
    Reaction *reacts; int nreacts;
    Embed *emb; u8 nemb;                        /* №53: карточки эмбедов (NULL — нет) */
    u16 voice_secs;                             /* №69: голосовое сообщение (flags 8192) — длина, с; 0 — не голосовое */
    Poll *poll;                                 /* №41: опрос (NULL — нет) */
    Comps *comp;                                /* №55: кнопки и списки бота (NULL — нет) */
} Message;

typedef struct {
    u64 channel_id, caller;                     /* входящий звонок в ЛС */
    bool ringing;
} Call;

/* ---- Фаза 4: друзья, новая ЛС, поиск, файлы. API согласован с дизайнером (ui.c). ---- */
#define MC_SOCIAL 1
/* Друзья: хранится только id + тип + статус (16 байт на запись), имя и аватар — user_name()/user_avatar(). */
enum { REL_FRIEND = 1, REL_BLOCKED = 2, REL_IN = 3, REL_OUT = 4 };   /* друг, заблокирован, входящая, исходящая заявка */
enum { PS_OFFLINE, PS_ONLINE, PS_IDLE, PS_DND };                     /* статус — только у друзей (REL_FRIEND) */
typedef struct { u64 id; u8 type, status; } Relation;

/* Поиск сообщений: одна страница (<= 25), живёт до search_close(). */
#define SEARCH_PAGE 25
typedef struct { Message m; u64 channel_id; } SearchHit;
typedef struct {
    bool busy;                                  /* запрос в пути */
    bool indexing;                              /* 202: индекс строится, повтор делает сам модуль */
    char error[160];                            /* "" или текст ошибки по-русски (429, нет связи, ...) */
    int total, nhits; SearchHit *hits;          /* total — всего найдено Discord; hits — до 25 */
    int page, page_count;                       /* страница (с 0) и сколько их (по 25) */
} SearchState;

typedef struct {
    bool ready;
    u8 me_premium;                              /* premium_type из READY: 0 — без Nitro */
    u64 me_id;
    char *me_name;
    char *session_id;                           /* сессия gateway (нужна голосу) */
    User *users; int nusers, capusers;
    Guild *guilds; int nguilds;
    Channel *chans; int nchans, capchans;
    VoiceState *vs; int nvs, capvs;
    Call call;
    u64 open_ch;
    Message *msgs; int nmsgs, capmsgs;
    bool history_end;
    char typing_name[64]; i64 typing_until;
    Relation *rels; int nrels, caprels;         /* отсортированы по id */
    u64 jump_id;                                /* переход к сообщению: его id (подсветить), 0 — нет */
    bool detached;                              /* загружено не до самых новых: плашка «Вернуться к настоящему» */
} Store;
extern Store S;

void store_reset(void);
void store_ready(const cJSON *d);
void store_event(const char *t, const cJSON *d);
Channel *chan_find(u64 id);
Guild *guild_find(u64 id);
const char *user_name(u64 id);
const char *user_avatar(u64 id);                /* хэш аватара или NULL */
u32 user_color(u64 guild_id, u64 id);           /* цвет ника на сервере, 0 — обычный/неизвестен */
void store_set_messages(const cJSON *arr, bool older);
void msgs_clear(void);

/* ---- Список участников сервера справа, как в Discord (memlist.c, включён в store.c) ----
 * Подписка op 14 на открытый канал, приём GUILD_MEMBER_LIST_UPDATE (SYNC/INSERT/UPDATE/DELETE/INVALIDATE).
 * Хранится только подписанное окно (до 3 окон по 100 строк) одного открытого канала; смена канала или
 * сервера — всё затирается и освобождается. Строки — копии ограниченной длины после san(). */
#define MC_MEMLIST 1
enum { MLR_GROUP = 1, MLR_MEMBER = 2 };          /* строка списка: заголовок группы / участник */
enum { ML_G_ONLINE = 1, ML_G_OFFLINE = 2 };      /* id групп «В сети» и «Не в сети»; прочие id — роли (hoist) */
enum { MLA_NONE, MLA_PLAY, MLA_STREAM, MLA_LISTEN, MLA_WATCH, MLA_CUSTOM, MLA_COMPETE };   /* что под именем */
#define ML_WIN 100                               /* окно подписки, как у официального клиента */
typedef struct {
    u64 id;                                      /* участник — id пользователя; группа — ML_G_* или id роли */
    u8 kind, status, act;                        /* MLR_*, PS_* (у группы 0), MLA_* */
    bool bot;
    u32 color;                                   /* цвет ника по старшей роли с цветом (0 — обычный) */
    int count;                                   /* группа: сколько в ней участников */
    char name[72];                               /* ник / имя роли (у «В сети»/«Не в сети» пусто — подпись в ui) */
    char text[72];                               /* свой статус или игра (пусто — нет) */
    char avatar[35];                             /* хэш аватара или "" */
} MemRow;
void ml_open(u64 guild_id, u64 channel_id);      /* открыт канал (guild_id 0 — ЛС: список закрыть) */
void ml_close(void);                             /* затереть и освободить */
void ml_view(int first, int last);               /* видимые строки (индексы списка): переподписка окнами по 100 */
int ml_total(u64 channel_id);                    /* строк в списке этого канала (группы + участники); -1 — списка нет */
const MemRow *ml_row(int index);                 /* NULL — строка ещё не пришла (рисовать заглушку) */
int ml_online(void);                             /* online_count сервера */
int ml_member_count(void);                       /* member_count сервера */
int ml_rows_held(void);                          /* сколько строк участников в памяти (для проверок) */
Message *msg_find(u64 id, int *idx);
void chan_label(const Channel *c, char *out, size_t n);
bool guild_unread(u64 gid, int *mentions);
bool chan_unread(const Channel *c);
void store_sort_dms(void);
int users_gc(bool force);                       /* вытеснить пользователей без ссылок; возврат — сколько удалено */
int voice_members(u64 channel_id, const VoiceState **out, int max);
bool event_wanted(const char *raw, size_t len);  /* отсев событий до разбора JSON */
/* READY/READY_SUPPLEMENTAL: вырезать ненужные поля на месте до cJSON; возврат — новая длина
 * (для прочих пакетов — len без изменений), 0 — текст испорчен, не разбирать. */
size_t ready_slim(char *s, size_t len);
/* Пакет READY (после ready_slim) -> модель по частям: дерево cJSON только одного сервера за раз.
 * resume — resume_gateway_url (без проверки, net.c проверяет сам). false — не разобран. */
bool store_ready_text(char *s, size_t len, char *resume, size_t rn);

/* ---- Фаза 4 (store.c, net.c). Записи (rel_accept/rel_remove/dm_open/send_file) — в демо без сети. ---- */
int rel_list(const Relation **out);             /* все отношения (массив по id); возврат — сколько */
const Relation *rel_find(u64 uid);              /* NULL — нет отношения */
const char *presence_name(u8 st);               /* "в сети" / "не активен" / "не беспокоить" / "не в сети" */
void rel_accept(u64 uid);                       /* PUT /users/@me/relationships/uid {} — принять заявку */
void rel_remove(u64 uid);                       /* DELETE: удалить друга / отклонить / отменить / разблокировать */
/* Есть ЛС один на один — сразу ui_open_dm(id); нет — POST /users/@me/channels {recipient_id},
 * канал в store, затем ui_open_dm(id). Ошибка — ui_notice. */
void dm_open(u64 uid);
void ui_open_dm(u64 channel_id);                /* ui.c: открыть беседу (в --cli — cli.c через ui.c) */

/* Поиск: guild_id != 0 — GET /guilds/{g}/messages/search (channel_id 0 — весь сервер, иначе этот канал);
 * guild_id 0 — GET /channels/{channel_id}/messages/search (ЛС). query кодируется (URL-encode),
 * всё из ответа — через san(). 202 (индекс строится) — повтор сам, indexing = true. По ответу — ui_dirty(). */
void search_start(u64 guild_id, u64 channel_id, const char *query);
const SearchState *search_state(void);
void search_close(void);                        /* освободить результаты, отменить ожидаемый ответ */

/* Файл (net.c): multipart, читается с диска потоком кусками по 16 КБ (в память целиком не грузится;
 * curl собран без MIME, поэтому тело собирает net.c через CURLOPT_READFUNCTION), не больше FILE_MAX.
 * false — сразу ошибка в err ("файл не найден", "больше 10 МБ", "это папка", "уже идёт отправка");
 * true — пошло, итог — ui_notice. Одновременно — одна отправка. */
#define FILE_MAX (10u << 20)
bool send_file(u64 channel_id, const char *path, const char *text, char *err, size_t errn);
bool send_file_ex(u64 channel_id, const char *path, const char *text, bool spoiler, const char *alt, char *err, size_t errn);   /* №65/67 */
long long plat_file_size(const char *utf8_path);   /* размер локального файла; -1 — нет/не файл/сетевой путь */
/* Перетаскивание файла в окно (Windows WM_DROPFILES, X11 XDND): окно зовёт g_ui_drop(путь) — только первый
 * файл, только локальный путь; строка живёт на время вызова и затирается после. Нигде не пишется. */
extern void (*g_ui_drop)(const char *path);   /* ui.c: g_ui_drop = своя функция (слабые символы в MinGW ненадёжны) */
bool file_sending(int *percent);                /* идёт ли отправка; percent 0..100 */
#include <stdio.h>
FILE *fopen_u8(const char *path, const char *mode);   /* net.c: fopen по пути UTF-8 (Windows — _wfopen) */
FILE *fopen_local(const char *path, char *err, size_t errn);   /* net.c: только локальный файл (не UNC/сетевой диск), rb */

/* ---- Волна 1 (store.c). API согласован с дизайнером. Записи — в демо локально, без сети. ---- */
#define MC_WAVE1 1
#define MC_RAW 1                                /* Message.raw и Message.pinned (ревью 5) */
/* Переход к сообщению: открыть канал (ui_open_channel), GET ?around=mid&limit=50, S.msgs заменяются,
 * S.jump_id = mid; S.detached — нет самых новых (новые сообщения в ленту не добавляются). */
void msgs_jump(u64 channel_id, u64 message_id);
void msgs_present(void);                        /* «Вернуться к настоящему»: последние 50, detached = false */
void ui_open_channel(u64 channel_id);           /* ui.c: открыть канал из любого места */
/* Закреплённые: страница до 25, «ещё» дописывает, всего не больше PINS_MAX. */
#define PINS_MAX 50
typedef struct { bool busy; char error[160]; int n; bool more; Message *m; u64 channel_id; } PinsState;
void pins_open(u64 channel_id);
void pins_more(void);
const PinsState *pins_state(void);
void pins_close(void);
void msg_pin(u64 channel_id, u64 msg_id, bool pin);   /* PUT/DELETE; CHANNEL_PINS_UPDATE -> перечитать открытые */
/* Входящие: «Упоминания» (GET /users/@me/mentions, 25). */
typedef struct { bool busy; char error[160]; int n; SearchHit *hits; } MentionsState;
void mentions_open(void);
const MentionsState *mentions_state(void);
void mentions_close(void);
/* Прочитано. Отметка в сеть — не чаще раза в 3 с на канал (лишнее откладывается) и только при активном окне. */
void ack_channel(u64 channel_id);               /* явное «прочитано» (снимает ручное «непрочитанное») */
void ack_seen(u64 channel_id);                  /* авто-отметка при просмотре (ручное «непрочитанное» не снимает) */
void ack_guild(u64 guild_id);
void ack_all(void);
void mark_unread(u64 channel_id, u64 msg_id);   /* непрочитанным начиная с msg_id */
void store_tick(void);                          /* main.c: отложенные отметки и смена статуса */
i64 store_next_wake(void);
bool win_focused(void);                         /* окно активно (gui_win.c; без окна — true) */
/* Карточка профиля: только по щелчку, без кэша; строки живут до profile_close. */
typedef struct {
    bool busy; char error[160];
    u64 id, guild_id;
    const char *name, *username, *pronouns, *bio;
    u32 accent;
    i64 since_unix, member_since_unix;          /* аккаунт создан (по id), на сервере с (0 — нет) */
    int nroles; const u64 *roles;
    bool bot;
    /* №99 полный профиль (profile_open_full): общие серверы и друзья, привязанные аккаунты */
    bool full;
    int nmutual; const u64 *mutual;             /* id общих серверов */
    int nfriends; const u64 *friends;           /* id общих друзей (имена — user_name) */
    int nconn; const char *conn[8];             /* «тип: имя» привязанных аккаунтов */
} Profile;
void profile_open_full(u64 uid, u64 guild_id);  /* №99: то же + общие серверы/друзья (по требованию) */
void profile_open(u64 uid, u64 guild_id);
const Profile *profile_state(void);
void profile_close(void);
/* Свой статус: op 3 не чаще 5 раз за 20 с (лишнее откладывается, побеждает последнее). */
enum { PS_INVISIBLE = 4 };
void presence_set(u8 st);
u8 presence_me(void);
/* Меню человека */
void rel_block(u64 uid);                        /* PUT relationships {"type":2} */
void rel_add(u64 uid);                          /* PUT relationships {} — заявка в друзья */
bool rel_add_name(const char *username, char *err, size_t errn);   /* «Добавить в друзья» по имени; false — имя неверно */
/* Один экземпляр программы на аккаунт (живой режим): false — уже запущен (его окно поднято) */
bool single_instance(void);
void single_instance_release(void);
#ifdef ASTRA_VIDEO
/* «Astra DS Video»: прототип трансляции, только замеры (streamtest.c, скрытый флаг --streamtest) */
int streamtest_run(int secs, int ow, int oh, int fps, int scene, int kbps);
/* Скрыть окна программы (главное — с входом, и окно капчи) из захвата экрана: WDA_EXCLUDEFROMCAPTURE
 * (Windows 10 2004+), где не поддерживается — WDA_MONITOR (в захвате чёрный прямоугольник). Действует и на окна,
 * созданные позже. false — не применилось (X11: всегда false). */
bool term_capture_exclude(bool on);
void capture_exclude_window(void *hwnd);        /* gui_win.c: новое окно (капча) — по текущему выбору */
#endif             /* перед перезапуском после обновления: новый процесс не должен увидеть нас */
void ui_lang_init(int forced);                  /* ui.c: 0 — русский, 1 — английский, -1 — сохранённый/системный */
int plat_ui_lang(void);                         /* язык системы: 0 — русский (ru/uk/be), 1 — английский (иначе) */
/* №15 (plat_*.c -> gui_*.c): масштаб окна, шаг 10 %, 0 — сброс; false — консоль или предел 10..28 px */
bool term_zoom(int step);
int term_zoom_pct(void);

/* ---- Пакет А (store.c). API согласован через координатора. Записи в демо — локально. ---- */
#define MC_PACKA 1
enum { NOTIFY_ALL = 0, NOTIFY_MENTIONS = 1, NOTIFY_NONE = 2, NOTIFY_DEFAULT = 3 };
void mute_guild(u64 gid, bool mute);
void mute_channel(u64 cid, bool mute);          /* и беседы (ЛС) */
void notify_level(u64 gid, u64 cid, int level); /* cid 0 — сервер целиком */
enum { MS_SENT = 0, MS_PENDING = 1, MS_FAILED = 2 };
#define SEND_SILENT 1                           /* тихое (flags 4096) */
#define SEND_NO_EMBEDS 2                        /* без превью ссылок (flags 4) */
#define SEND_NO_PING 4                          /* ответ без упоминания автора */
#define SEND_NO_MENTIONS 8                      /* никого не упоминать */
u64 msg_send(u64 cid, const char *text, u64 reply_to, int opts);   /* локальный id (nonce) или 0 */
void msg_retry(u64 local_id);
void msg_discard(u64 local_id);
/* №127: ограничения отправки в канале cid (всё в unix-секундах, 0 — нет; прошедшее время — тоже 0).
 * slowmode — медленный режим канала (0, если на меня не действует); slow_until — до какого момента нельзя
 * писать (после своего сообщения или по 429 медленного режима); timeout_until — тайм-аут на сервере.
 * msg_send в это время не шлёт (уведомление). Пока slow_until впереди — store будит раз в секунду
 * (только для открытого канала), для обратного отсчёта; по окончании — ui_dirty. */
typedef struct { int slowmode; i64 slow_until, timeout_until; } SendLimit;
void send_limit(u64 cid, SendLimit *out);
bool ui_draft_restore(u64 ch, const char *text);   /* ui.c: текст в черновик канала ch, если он пуст; true — положен */
void search_page(int page);                     /* повтор последнего поиска со страницы page (по 25) */
bool cdn_expired(const char *url);              /* у ссылки CDN срок (ex=) истёк */
const char *cdn_fresh(const char *url);         /* обновлённая ссылка или NULL; сама запрос НЕ шлёт */
void cdn_want(const char *url);                 /* видимая строка: поставить в очередь обновления (пачка, store_tick) */
extern int cdn_requests;                        /* сколько POST refresh-urls ушло (selftest) */
/* Ревью 9 М3: «Просмотр реакций» и «Переслать» — запросы и разбор в store.c, ui.c только зовёт и рисует. */
typedef struct {
    bool on, busy;
    u64 ch, msg;
    int sel;                                    /* выбранная реакция (индекс в m->reacts) */
    char err[120];
    int n;
    char names[25][64];                         /* первые 25 поставивших */
} ReactorsState;
void reactors_open(u64 ch, u64 msg, int sel);   /* открыть/сменить реакцию: GET .../reactions/{эмодзи}?limit=25 */
void reactors_close(void);
const ReactorsState *reactors_state(void);
void msg_forward(u64 target, u64 src_ch, u64 msg);   /* итог — ui_notice «сообщение переслано» / «не переслано: ...» */
/* Исходящий звонок в ЛС: вход в голос (op 4), звонок собеседнику (POST /call/ring) — после CALL_CREATE этой ЛС
 * (не раньше: иначе ring приходит до создания звонка и собеседнику не звонит), не позже чем через 5 с. */
void call_start(u64 channel_id);               /* ЛС и группа ЛС (№138: звонят всем участникам группы) */
/* №138 групповой звонок: кому сейчас звонят (CALL_CREATE/UPDATE.ringing), позвонить ещё раз одному, перестать звонить */
int call_ringing(u64 cid, u64 *out, int max);
void call_ring_user(u64 cid, u64 uid);
void call_stop_ring(u64 cid, u64 uid);          /* uid 0 — всем */
u64 call_ring_pending(void);                    /* ЛС, где ждём CALL_CREATE для звонка (0 — нет) */
void wv2_cleanup(void);                         /* gui_win.c: удалить %TEMP%\\mincord-wv2-* старше 10 мин (остатки капчи) */
/* ---- Пакет Б (store.c). API — через координатора дизайнеру. Записи в демо — локально. ---- */
#define MC_PACKB 1
/* №32 эмодзи серверов: не из READY, а GET /guilds/{id}/emojis при открытии пикера; живут до emoji_close. */
typedef struct { u64 id; char name[33]; bool animated, available; } GuildEmoji;
typedef struct { u64 gid; bool busy; char err[96]; int n; GuildEmoji *e; } EmojiState;
void emoji_open(u64 gid);
void emoji_close(void);
const EmojiState *emoji_state(void);
/* №55 кнопки и меню ботов (components): только кнопки и выбор из списка, без модальных окон.
 * Строки — comp_str (CS_*), варианты списка — comp_option. Нажатие — POST /interactions (type 3, nonce);
 * итог — ui_notice (INTERACTION_SUCCESS молча, INTERACTION_FAILURE / нет ответа 5 с — «бот не ответил»). */
enum { CT_BUTTON = 2, CT_SELECT = 3 };
enum { CS_LABEL, CS_ID, CS_URL, CS_EMOJI, CS_PLACEHOLDER, CS_N };
typedef struct { u8 type, style, row, nopt, min, max; bool disabled; u16 s[CS_N]; u16 opt; } Comp;   /* style 5 — ссылка (url) */
struct Comps { u64 app_id; char *s; u8 n; Comp c[]; };   /* c — ровно n штук */
const char *comp_str(const Message *m, int i, int k);                          /* "" — нет */
bool comp_option(const Message *m, int i, int k, const char **label, const char **value);
bool comp_click(u64 cid, u64 mid, int i, char *err, size_t errn);             /* кнопка */
bool comp_select(u64 cid, u64 mid, int i, const int *opts, int n, char *err, size_t errn);   /* список: индексы вариантов */
/* №109/111 запросы на переписку и спам (флаги ЛС), участники группы ЛС */
enum { DMF_REQUEST = 1, DMF_SPAM = 2 };
bool dm_request_accept(u64 cid, char *err, size_t errn);   /* принять запрос (consent) */
bool dm_request_reject(u64 cid, char *err, size_t errn);   /* отклонить: закрыть беседу */
u64 gdm_owner(u64 cid);                                    /* владелец группы ЛС (0 — неизвестен) */
bool gdm_add(u64 cid, u64 uid, char *err, size_t errn);    /* добавить в группу (друга) */
bool gdm_remove(u64 cid, u64 uid, char *err, size_t errn); /* исключить (только владелец) */
bool gdm_leave(u64 cid, char *err, size_t errn);           /* выйти из группы */
/* №2 папки серверов (READY user_settings_proto, поле guild_folders; или старый user_settings.guild_folders) */
typedef struct { i64 id; char *name; u32 color; bool has_color; int n; u64 *gids; u8 *meta; int nmeta; } GuildFolder;   /* meta — прочие поля папки как пришли */
int guild_folders(const GuildFolder **out);                /* число папок (папка из одного сервера без id — просто сервер) */
/* Полоса серверов в порядке Discord: сверху серверы без записи в guild_folders (новые: позже вступили — выше),
 * дальше по guild_folders. Каждый сервер S.guilds ровно один раз. fk — индекс папки или RAIL_SINGLE / RAIL_NEW. */
typedef struct { u64 gid; int fk; } RailEnt;
enum { RAIL_SINGLE = -1, RAIL_NEW = -2 };
int guild_rail(RailEnt **out);                             /* *out — в куче (free) */
/* Перестановка: сразу на полосе, затем PATCH /users/@me/settings-proto/1 (только поле 14); отказ — откат и причина.
 * src — сервер; whole — двигать его папку целиком. ref — сервер-ориентир; ref_top — относительно записи верхнего
 * уровня, где ref (его папки целиком), иначе — внутри папки ref. GM_INTO — в конец папки ref; GM_NEWFOLDER —
 * новая папка из ref и src. false — не изменилось (err — причина, если есть). */
enum { GM_BEFORE, GM_AFTER, GM_INTO, GM_NEWFOLDER };
bool guild_move(u64 src, bool whole, int how, u64 ref, bool ref_top, char *err, size_t errn);
void guild_folders_pb(Buf *out);                           /* protobuf PreloadedUserSettings только с полем 14 */
/* Настройки папки, где сервер gid (только настоящая папка: с id или из нескольких серверов). name: NULL — не менять,
 * "" — убрать название (не длиннее FOLDER_NAME_MAX символов, края без пробелов). color: 0..0xFFFFFF,
 * FOLDER_COLOR_KEEP — не менять, FOLDER_COLOR_DEFAULT — убрать (цвет по умолчанию). Запись и откат — как у guild_move. */
#define FOLDER_NAME_MAX 100
#define FOLDER_COLOR_KEEP (-1)
#define FOLDER_COLOR_DEFAULT (-2)
bool guild_folder_edit(u64 gid, const char *name, i64 color, char *err, size_t errn);
bool guild_folder_ungroup(u64 gid, char *err, size_t errn);   /* распустить: серверы остаются на месте по порядку */
bool guild_unfold(u64 gid, char *err, size_t errn);        /* «Убрать из папки»: сервер — отдельно, сразу после папки */
bool guild_in_folder(u64 gid);                             /* сервер в папке (не одиночная запись) */
/* Новая папка из серверов вне папок: на месте ref, в ней ref и src; name - как у guild_folder_edit (пусто - без
 * названия). Новый id, одна запись поля 14, откат при отказе. Окно подтверждения - в ui.c (бросок на сервер). */
bool guild_folder_create(u64 src, u64 ref, const char *name, char *err, size_t errn);
bool guild_folder_muted(u64 gid);                          /* все серверы папки, где gid, заглушены */
int guild_folder_mute(u64 gid, bool mute);                 /* mute_guild каждому серверу папки, где состояние другое */
void guild_folder_name(u64 gid, char *out, size_t n);         /* название папки целиком (GuildFolder.name — до 64 байт) */
bool guild_order_busy(void);                               /* запись порядка ещё не подтверждена */
/* №119 слова-тревоги: только в памяти; сообщение со словом — как упоминание (подсветка и счётчик) */
void alert_words_set(const char *words);                   /* через запятую, до 16 слов по 32 байта; "" — выключить */
const char *alert_words(void);
bool msg_alert(const Message *m);
/* №90–92 ветки: каналы типа CH_THREAD* в S.chans (parent_id — текстовый канал, права — от него).
 * Хранятся активные ветки открытого сервера (threads_open при входе на сервер) и те, где я участник. */
enum { TF_JOINED = 1, TF_ARCHIVED = 2, TF_LOCKED = 4, TF_DELETED = 8 };   /* TF_DELETED: открытую ветку удалили — ввод закрыт */
bool chan_is_thread(const Channel *c);
void threads_open(u64 gid);                     /* GET /guilds/{id}/threads/active; ветки других серверов (не мои) — убрать */
int chan_threads(u64 parent, const Channel **out, int max);   /* ветки под каналом, свежие сверху */
bool thread_create(u64 cid, u64 mid, const char *name, char *err, size_t errn);   /* mid 0 — без сообщения */
bool thread_member(u64 tid, bool join, char *err, size_t errn);   /* вступить / выйти */
/* №95 форумы (CH_FORUM / CH_MEDIA): посты — страница по 25 из threads/search, живёт до forum_close. */
#define FORUM_PAGE 25
typedef struct { u64 id, owner_id, last_msg; char *name, *preview; int msg_count; bool archived, pinned, joined; } ForumPost;
typedef struct { u64 cid; bool busy; char err[120]; int total, n, page, sort; bool more; ForumPost *p; } ForumState;
void forum_open(u64 cid, int page, int sort);   /* sort 0 — по последней активности, 1 — по дате создания */
void forum_close(void);
const ForumState *forum_state(void);
u64 forum_post_open(int i);                     /* пост -> ветка в S.chans (открыть как канал); 0 — нет */
bool emoji_need_nitro(const GuildEmoji *e, u64 emoji_gid, u64 target_ch);   /* без Nitro не отправить */
void emoji_code(const GuildEmoji *e, char *out, size_t n);                   /* <:имя:id> / <a:имя:id> для текста */
/* №45/46 разметка одной строки (после render_content): вид строки и куски текста с атрибутами.
 * Смещения — от начала строки s, маркеры (**, _, ~~, ||, `, #, -#, [..](..)) в куски не входят. */
enum { MD_B = 1, MD_I = 2, MD_U = 4, MD_S = 8, MD_CODE = 16, MD_SPOIL = 32, MD_LINK = 64 };
enum { ML_TEXT, ML_H1, ML_H2, ML_H3, ML_SUB, ML_LIST, ML_OLIST };
typedef struct { u16 off, len; u8 attr; u16 url_off, url_len; } MdSpan;      /* url — у MD_LINK (только http/https) */
typedef struct { u8 kind; u8 indent; u16 num_off, num_len; } MdLine;         /* ML_OLIST: номер «12.» — num_off/num_len */
int md_line(const char *s, size_t n, MdLine *ln, MdSpan *sp, int max);       /* число кусков (<= max) */
bool md_code_block(const char *content, int k, Buf *out);                    /* текст k-го блока ``` без ограды */
/* №41 опросы: у сообщения с опросом Message.poll; голоса — PUT .../polls/{mid}/answers/@me, события VOTE_ADD/REMOVE. */
typedef struct { u32 id; int count; bool me; char *text; } PollAnswer;
struct Poll { char *question; i64 expiry; bool multi, final; u8 n; PollAnswer a[10]; };   /* expiry — unix-с, 0 — нет */
bool poll_vote(u64 cid, u64 mid, const u32 *ids, int n, char *err, size_t errn);   /* n = 0 — снять голос */
bool poll_end(u64 cid, u64 mid, char *err, size_t errn);                     /* досрочно (только автор) */
/* №53 эмбеды карточкой: до EMB_MAX на сообщение, текст одного <= EMB_BYTES (обрезка по UTF-8).
 * Строки — embed_str (EM_*), поля — embed_field. Картинки (EM_IMAGE/EM_THUMB) — только с CDN/медиа Discord. */
#define EMB_MAX 3
#define EMB_BYTES 1024
enum { EM_AUTHOR, EM_TITLE, EM_URL, EM_DESC, EM_FOOTER, EM_IMAGE, EM_THUMB, EM_PROVIDER, EM_NSTR };
struct Embed { u32 color; bool has_color; u8 nf; u32 f_inline; char *s; u16 o[EM_NSTR]; u16 fo; };
const char *embed_str(const Embed *e, int k);                                /* "" — нет */
bool embed_field(const Embed *e, int i, const char **name, const char **value, bool *inl);
/* №57 слеш-команды: индекс по требованию (при вводе «/»), только для открытого канала, до cmds_close.
 * Подкоманды развёрнуты: «музыка играть» — отдельная запись. Опции: строка, целое, bool, пользователь, канал, число;
 * команда с обязательной опцией другого типа (роль, вложение…) — usable=false. Строки — cmds_str(смещение). */
enum { CO_STRING = 3, CO_INTEGER = 4, CO_BOOLEAN = 5, CO_USER = 6, CO_CHANNEL = 7, CO_ROLE = 8, CO_MENTIONABLE = 9, CO_NUMBER = 10, CO_ATTACHMENT = 11 };
typedef struct { u8 type; bool required; u16 name, name_show, desc; u8 nchoice; u16 choice; } SlashOpt;   /* name — сырое (JSON), name_show — для показа; варианты: name\0value\0 */
typedef struct { u64 id, app_id, version; u16 name, desc, app; u16 sub, grp, base; int opt0; u8 nopt; bool usable; } SlashCmd;   /* base — сырое имя команды */   /* name — полное («кмд под») */
typedef struct { u64 cid; bool busy; char err[120]; int n, no; SlashCmd *c; SlashOpt *o; char *s; } SlashIndex;
void cmds_open(u64 cid);
void cmds_close(void);
const SlashIndex *cmds_state(void);
const char *cmds_str(u16 off);
int cmds_match(const char *prefix, int *out, int max);   /* по началу имени (без «/»), без учёта регистра */
bool cmd_choice(int opt, int k, const char **name, const char **value);
/* vals[i] — значение i-й опции команды ("" — не задавать необязательную): целое/число — цифрами, bool — да/нет,
 * пользователь — id или имя, канал — id или #имя. Итог — ui_notice; ответ бота придёт сообщением. */
bool cmd_run(int i, const char *const *vals, int nvals, char *err, size_t errn);
/* №133/117/147 модерация (только при правах; подтверждение — в интерфейсе). reason — в журнал аудита (до 512). */
enum { MOD_TIMEOUT, MOD_KICK, MOD_BAN, MOD_ROLES, MOD_MOVE, MOD_MUTE };
bool mod_can(u64 gid, int action);              /* есть ли у меня право (иерархию ролей проверит Discord) */
bool mod_timeout(u64 gid, u64 uid, int minutes, const char *reason, char *err, size_t errn);   /* 0 — снять; до 28 суток */
bool mod_kick(u64 gid, u64 uid, const char *reason, char *err, size_t errn);
bool mod_ban(u64 gid, u64 uid, const char *reason, int delete_hours, char *err, size_t errn);  /* удалить его сообщения за 0..168 ч */
bool mod_role(u64 gid, u64 uid, u64 role_id, bool add, char *err, size_t errn);
bool mod_voice_move(u64 gid, u64 uid, u64 to_channel, char *err, size_t errn);  /* 0 — отключить от голоса */
bool mod_voice_mute(u64 gid, u64 uid, bool mute, bool deaf, char *err, size_t errn);   /* серверные заглушка/глухота */
/* ---- Автообновление (update.c; SECURITY.md «Автообновление») ----
 * Проверка — при запуске не чаще раза в сутки (если включено; дата — в хранилище секретов) или по кнопке.
 * Загрузка и замена — только после явного «Обновить» (update_apply). Затем «Перезапустить» (update_restart). */
#define ASTRA_VERSION "0.2.9"                   /* версия этой сборки: релиз поднимает её (обновление — только на строго большую) */
enum { UP_IDLE, UP_CHECKING, UP_NONE, UP_AVAILABLE, UP_DOWNLOADING, UP_READY, UP_FAILED };
typedef struct {
    int phase;                                  /* UP_* */
    char version[32];                           /* доступная (из подписанного манифеста) */
    char notes[4100];                           /* «что нового» — очищено, <= 4 КБ, ссылки не открывать */
    char err[160];
    long long size, got;                        /* к загрузке / загружено, байт */
    i64 checked_at;                             /* unix-с последней проверки этой сессии (0 — не было) */
    bool shared_dir;                            /* «Обновить» отложено: каталог/exe доступен на запись другим — спросить
                                                 * «обновить всё равно?» -> update_apply_anyway() или update_apply_cancel();
                                                 * спрашивать КАЖДЫЙ раз, без «запомнить» (безопасник) */
    char warn[600];                             /* при shared_dir: готовый текст предупреждения с путём каталога */
} UpdateState;
void update_init(void);                         /* при старте: убрать .old и брошенные временные файлы */
void update_check(bool manual);                 /* manual — по кнопке; иначе не чаще раза в сутки и если включено */
void update_apply(void);                        /* «Обновить»: только при UP_AVAILABLE (может выставить shared_dir) */
void update_apply_anyway(void);                 /* «Обновить всё равно» после предупреждения shared_dir */
void update_apply_cancel(void);                 /* «Отмена»: снять shared_dir */
bool update_restart(void);                      /* «Перезапустить»: при UP_READY; запускает новый exe и выходит */
void update_service(void);                      /* главный цикл */
const UpdateState *update_state(void);
bool update_demo(const char *phase);            /* --demo-update=available|confirm|downloading|ready|failed (только --demo) */
void update_auto_set(bool on);                  /* настройка «проверять обновления при запуске» (по умолчанию да) */
bool update_auto(void);
int update_selftest(void (*check)(bool ok, const char *what));
/* ---- Очередь 1 аналитика ---- */
/* №103/104 свой статус со сроком и текстовый статус. presence_set_for: статус на minutes (0 — бессрочно), по
 * истечении — «в сети». custom_status_set: текст (<= 128 символов, "" — убрать) на minutes (0 — бессрочно):
 * PATCH /users/@me/settings {custom_status} + в присутствии (op 3, activity type 4). Из READY — user_settings_proto. */
void presence_set_for(u8 st, int minutes);
i64 presence_until(void);                       /* unix-с, 0 — бессрочно */
bool custom_status_set(const char *text, int minutes, char *err, size_t errn);
const char *custom_status(void);                /* "" — нет (истёкший — тоже "") */
i64 custom_status_until(void);
/* №108 игнорировать (только у себя, только в памяти): сообщения свернуть, упоминания не считать */
void ignore_set(u64 uid, bool on);
bool ignored(u64 uid);
/* №100 личная заметка о человеке: note_open (GET /users/@me/notes/{id}), note_set (PUT, <= 256 символов, "" — убрать) */
typedef struct { u64 uid; bool busy; char err[120]; char text[1100]; } NoteState;
void note_open(u64 uid);
void note_close(void);
const NoteState *note_state(void);
bool note_set(u64 uid, const char *text, char *err, size_t errn);
/* №128 правила сервера и вступление: Guild.pending — я ещё не принял правила (писать нельзя). guild_rules_open —
 * GET /guilds/{id}/member-verification; guild_rules_accept — PUT /guilds/{id}/requests/@me (принять). */
typedef struct { u64 gid; bool busy; char err[120]; char *desc; int n; char **rules; char *version; } RulesState;
void guild_rules_open(u64 gid);
void guild_rules_close(void);
const RulesState *guild_rules_state(void);
bool guild_rules_accept(u64 gid, char *err, size_t errn);
/* ---- Пакет В (мелочи ~0 КБ). API — через координатора. ---- */
/* №123/124 уведомления (только ЛС и упоминания, не в открытом канале при активном окне; заглушённые — нет):
 * NOTIFY_OFF — нет; NOTIFY_PLAIN (по умолчанию) — «Astra DS: новое сообщение / новое упоминание», без имён и каналов:
 * Windows хранит уведомления на диске (wpndatabase.db); NOTIFY_TEXT — имя, канал и начало текста (интерфейс
 * предупреждает «Windows сохраняет текст уведомлений на диске»); в режиме стримера — всегда как PLAIN. Windows — значок в трее и всплывашка
 * (Shell_NotifyIcon); Linux — /usr/bin/notify-send, только если он есть. --tray: свернуть — в трей. */
enum { NOTIFY_OFF = 0, NOTIFY_PLAIN = 1, NOTIFY_TEXT = 2 };
void notify_set(int mode);
int notify_mode(void);
void plat_notify(const char *title, const char *text);   /* окно Windows / notify-send; без окна — ничего */
extern void (*g_notify_hook)(const char *title, const char *text);   /* selftest */
/* №93 архив веток канала: страница через forum_state() (те же ForumPost), forum_close — закрыть */
void threads_archived_open(u64 cid);
/* №94 правка ветки: name NULL — не менять; archived/locked: -1 — не менять, 0/1 */
bool thread_edit(u64 tid, const char *name, int archived, int locked, char *err, size_t errn);
/* №96 новый пост форума: заголовок (1..100) и первое сообщение (1..2000) */
bool forum_post_create(u64 cid, const char *title, const char *text, char *err, size_t errn);
/* №97 участники ветки: по запросу, до 100, живут до thread_members_close */
typedef struct { u64 tid; bool busy; char err[120]; int n; u64 *ids; } ThreadMembers;
void thread_members_open(u64 tid);
void thread_members_close(void);
const ThreadMembers *thread_members_state(void);
/* №112 имя группы ЛС; №113 свой профиль: отображаемое имя (1..32, NULL — не менять), «о себе» (<= 190, NULL — не менять) */
bool gdm_rename(u64 cid, const char *name, char *err, size_t errn);
bool profile_me_set(const char *display_name, const char *bio, char *err, size_t errn);

#define MC_PACKV 1
/* №120 фильтр слов: сообщение со словом сворачивается (только в памяти, как слова-тревоги) */
void filter_words_set(const char *words);       /* через запятую, до 16 по 32 байта; "" — выключить */
const char *filter_words(void);
bool msg_filtered(const Message *m);
/* №121 режим фокуса: minutes 0 — выключить; пока включён, упоминанием считаются только ЛС и @меня
 * (слова-тревоги — нет). По окончании — ui_dirty. */
void focus_set(int minutes);
i64 focus_until(void);                          /* unix-с, 0 — выключен */
/* №125 режим стримера: флаг для интерфейса (прятать имя пользователя, почту/телефон, коды приглашений) */
void streamer_set(bool on);
bool streamer_on(void);
/* №151 статус голосового канала (VOICE_CHANNEL_STATUS_UPDATE / CHANNEL_INFO), до 500 символов */
const char *voice_channel_status(u64 cid);      /* "" — нет */
bool voice_channel_status_set(u64 cid, const char *text, char *err, size_t errn);   /* "" — убрать */
/* №16 полный экран окна: mode -1 — переключить, 0/1 — выключить/включить; возврат — состояние,
 * -1 — без окна (консоль: полный экран у терминала). Клавиша — предлагается Alt+Enter в окне. */
int term_fullscreen(int mode);
/* №141 рация на выбранной клавише (Windows: виртуальная клавиша или кнопка мыши 4/5 — VK_XBUTTON1/2;
 * опрос состояния только этой клавиши, только в звонке). 0 — по-старому (F8 переключает, с --ptt). */
void voice_ptt_bind(int vk);
int voice_ptt_bound(void);
/* №141: VK клавиши из события окна (назначение рации; боковые кнопки мыши — K_MOUSE button 8/9).
 * Только Windows; на Linux — запасной перевод в ui.c (weak), глобальной рации там нет. */
bool plat_key_down(int vk);                     /* №141: зажата ли клавиша (печатные — только при окне в фокусе; Linux — false) */
void plat_key_name(int vk, char *out, size_t n);/* имя клавиши по-человечески ("" — нет) */
/* №142 авточувствительность: порог = шумовой фон + 9 дБ (в пределах -60..-20), считается в звонке */
void voice_vad_auto(bool on);
bool voice_vad_is_auto(void);
/* ---- Волна 2 (store.c): серверы, каналы, приглашения. Записи в демо — локально. Итог — ui_notice + ui_dirty;
 * функции с err — проверка до сети (false и причина по-русски). Всё — по запросу, постоянной памяти нет. ---- */
#define MC_WAVE2 1
#define P_CREATE_INVITE   (1ULL << 0)
#define P_VIEW_CHANNEL    (1ULL << 10)
#define P_SEND_MESSAGES   (1ULL << 11)
#define P_MANAGE_CHANNELS (1ULL << 4)
#define P_MANAGE_GUILD    (1ULL << 5)
#define P_MANAGE_ROLES    (1ULL << 28)
bool can_manage_guild(u64 gid);
bool can_manage_channels(u64 gid);
bool can_manage_roles(u64 gid);
bool can_create_invite(u64 channel_id);
bool guild_is_owner(u64 gid);
#define ICON_MAX (256u << 10)                   /* значок/баннер: JPEG/PNG/GIF до 256 КБ */
bool guild_create(const char *name, const char *icon_path, char *err, size_t errn);
/* NULL — не менять, "" — убрать (значок/баннер/описание) */
bool guild_edit(u64 gid, const char *name, const char *description, const char *icon_path, const char *banner_path, char *err, size_t errn);
void guild_leave(u64 gid);                      /* подтверждение — в ui; владелец выйти не может */
bool guild_delete(u64 gid, const char *typed_name, char *err, size_t errn);   /* только владелец; имя набрано точно */
const char *guild_description(u64 gid);         /* первый вызов — GET /guilds/{id}, NULL пока грузится (ui_dirty по ответу) */
/* роли сервера с именами — для окна «Права»: первый вызов — GET /guilds/{id}/roles, -1 пока грузится */
typedef struct { u64 id; char *name; u32 color; int pos; } RoleName;
int guild_roles(u64 gid, const RoleName **out);
const char *role_name(u64 gid, u64 role_id);    /* из загруженного списка; NULL — нет */
enum { CHK_TEXT = 0, CHK_VOICE = 2, CHK_CATEGORY = 4 };
bool chan_create(u64 gid, int kind, const char *name, u64 parent_id, char *err, size_t errn);
typedef struct { const char *name, *topic; int slowmode, nsfw, bitrate, user_limit; } ChanEdit;   /* NULL / -1 — не менять */
bool chan_edit(u64 cid, const ChanEdit *e, char *err, size_t errn);
/* текущие значения для «Изменить канал»: первый вызов — GET /channels/{id}, false пока грузится; строки живут до следующего вызова */
bool chan_info(u64 cid, ChanEdit *out);
long chan_info_failed(u64 cid);                 /* != 0 — загрузка настроек не удалась (код), повтора нет */
void chan_info_retry(void);                     /* явный повтор после ошибки */
void forms_closed(void);                        /* закрыты формы — освободить кэши ролей/описания/настроек */
void chan_delete(u64 cid);
void chan_move(u64 cid, u64 new_parent, int new_position);   /* тот же родитель и ±1 — обмен с соседом (chan_swap) */
void chan_swap(u64 cid, int dir);               /* поменять местами с соседом: dir -1 выше, +1 ниже */
enum { OW_ROLE = 0, OW_MEMBER = 1 };
void chan_perm_set(u64 cid, u64 target, int type, u64 allow, u64 deny);   /* target @everyone = id сервера */
void chan_perm_clear(u64 cid, u64 target);
bool chan_overwrite(u64 cid, u64 target, u64 *allow, u64 *deny, int *type);   /* исходная перезапись (false — нет) */
void chan_perm_edit(u64 cid, u64 target, int type, u64 mask, u64 allow, u64 deny);  /* только биты mask, остальное — исходное */
/* приглашения */
bool invite_parse(const char *text, char *code, size_t n);   /* discord.gg/КОД, discord.com/invite/КОД, КОД */
typedef struct {
    bool busy; char error[256]; char code[40];     /* error: полный текст отказа Discord по-русски */
    u64 guild_id; const char *guild_name, *channel_name, *inviter; char icon[35];
    int members, online;
    bool joined;                                /* вы уже на этом сервере */
    bool need_browser;                          /* Discord требует вступить через браузер (капча/отказ): кнопка «Открыть в браузере» */
    bool risk_warning;                          /* показать разовое предупреждение о риске; после согласия — invite_risk_ack() и снова invite_join() */
} InviteState;
#define INVITE_GAP_MS 30000                     /* пауза между вступлениями */
const char *invite_risk_text(void);             /* текст разового предупреждения */
void invite_risk_ack(void);                     /* пользователь согласился (до выхода из программы) */
bool invite_open_browser(void);                 /* открыть https://discord.gg/КОД в браузере (только проверенный код) */
bool open_url_browser(const char *url);         /* gui_win.c / main.c: системный браузер; только https://discord.gg/ и discord.com/invite/ */
void invite_open(const char *code_or_url);      /* GET /invites/{code}?with_counts=true */
const InviteState *invite_state(void);
void invite_join(void);                         /* ТОЛЬКО по явному подтверждению; капча — net_captcha */
void invite_close(void);
void invite_create(u64 channel_id, int max_age_s, int max_uses);   /* 0 — бессрочно / без лимита */
const char *invite_created(void);               /* "https://discord.gg/КОД" или NULL */


/* ======================= net.c ======================= */
typedef struct { long status; Buf body; } Resp;
typedef void (*HttpCb)(Resp *r, void *ctx);
bool net_init(void);
void net_set_ca(void *curl);                    /* корни доверия TLS для хэндла curl */
void net_cleanup(void);
/* Асинхронный REST (curl_multi в главном цикле). cb может быть NULL. */
void http_async(const char *method, const char *path, const char *body, HttpCb cb, void *ctx);
void http_async_hdr(const char *method, const char *path, const char *body, const char *hdr, HttpCb cb, void *ctx);   /* + один заголовок */
/* запрос не раньше чем через delay_ms (повтор поиска при 202) */
void http_async_delayed(const char *method, const char *path, const char *body, HttpCb cb, void *ctx, int delay_ms);
bool http_busy(void);
/* Капча на любом REST-запросе (net.c): запрос ждёт, пока пользователь пройдёт проверку в окне captcha.c. */
typedef struct {
    bool pending;                               /* есть запрос, ждущий проверки */
    bool open;                                  /* окно проверки открыто */
    bool supported;                             /* есть чем показать (WebView2) */
    char what[64];                              /* что ждёт: «отправка сообщения», «заявка в друзья», ... */
    char error[160];                            /* "" или причина по-русски (не принято, нет WebView2, окно закрыто) */
    u8 outcome;                                 /* исход последней проверки: CAP_NONE / PASSED / CANCELLED / FAILED */
} CaptchaState;
enum { CAP_NONE, CAP_PASSED, CAP_CANCELLED, CAP_FAILED };
const CaptchaState *net_captcha(void);
void net_captcha_open(void);                    /* открыть окно проверки (по кнопке/клавише пользователя) */
void net_captcha_cancel(void);                  /* отказаться: запрос завершится ошибкой 400 */
void net_captcha_test(const char *method, const char *path, const char *resp, HttpCb cb, void *ctx);   /* selftest/демо: подставной 400 с капчей */
int http_waitables(Waitable *out, int max);     /* сокеты отправки файла для главного цикла */
void http_service(void);
i64 http_next_wake(void);

void gw_start(void);
void gw_stop(void);
int gw_waitables(Waitable *out, int max);
void gw_service(void);
i64 gw_next_wake(void);
void gw_send(const char *json);
void gw_subscribe(u64 guild, u64 channel);
/* op 14 с окнами списка участников по 100: chunk[i] — номер окна (0 — строки 0..99), n <= 3 */
void gw_subscribe_ranges(u64 guild, u64 channel, const int *chunk, int n);
void gw_voice_state(u64 guild, u64 channel, bool mute, bool deaf);
const char *gw_status_text(void);
bool gw_online(void);                           /* gateway подключён (можно слать op) */
const char *presence_word(void);                /* store.c: "online"/"idle"/"dnd"/"invisible" — для IDENTIFY */
/* для login.c: свои хэндлы входа, но маршрут (VLESS) и X-Super-Properties — общие */
bool net_route(void *curl, bool *via);          /* false — VLESS включён, но не работает: не идти */
const char *net_super_header(void);             /* "X-Super-Properties: ..." */
bool net_path_error(int curl_rc);               /* сбой самого пути (для vless_report_failure) */
extern bool g_auth_failed;
extern int g_gw_ping;

/* ======================= voice.c ======================= */
typedef enum { VS_IDLE, VS_WAITING, VS_CONNECTING, VS_DAVE, VS_CONNECTED, VS_FAILED } VoicePhase;
typedef struct {
    VoicePhase phase;
    u64 guild_id, channel_id;
    int ping;
    bool dave;                                  /* сквозное шифрование активно */
    bool mic_live;                              /* микрофон открыт и не заглушён */
    bool self_mute, self_deaf;
    u64 speaking[16]; int nspeaking;
    float mic_level;                            /* 0..1 */
    char error[128];
} VoiceStatus;
/* Ревью 17: счётчики приёма для страницы «Производительность» (живая проверка треска). */
typedef struct { u64 user; u32 underruns, stops, trims, dave_fail, late, plc, fec; } VoiceSrcStats;
typedef struct {
    int nsrc; VoiceSrcStats src[8];
    int min_queued_ms;                          /* наименьший запас в устройстве вывода за звонок */
    u32 tick_hist[6];                           /* интервал такта: <=22, <=27, <=32, <=40, <=60, >60 мс */
    int ns_mode;                                /* NS_* в работе (0 — выкл или микрофон молчит) */
    int ns_cpu_pm;                              /* шумоподавление: доля одного ядра, промилле (за последнюю секунду) */
    int cap_cpu_pm;                             /* поток звонка целиком (GetThreadTimes), промилле одного ядра */
    int ns_mem_kb;                              /* память состояния шумоподавления, КБ */
} VoiceStats;
/* Шумоподавление микрофона (SpeexDSP preprocess / RNNoise, статически): только в звонке, пока микрофон открыт и
 * не заглушён (в режиме рации — пока нажата клавиша). После ФВЧ, до смены голоса, VAD и Opus. Переключается на ходу;
 * состояние создаётся при первом кадре звонка и освобождается при выходе. По умолчанию выкл. */
enum { NS_OFF, NS_SPEEX, NS_RNNOISE };
void voice_ns_set(int mode);
int voice_ns_mode(void);
void voice_stats(VoiceStats *out);
void voice_diag(char *host, size_t n, int *port, int *curl_rc, int *ws_close);   /* хост:порт сервера звонка и коды ошибок */              /* снимок (обновляется раз в секунду), обнуляется при входе в звонок */
void plat_timer_hires(bool on);                 /* Windows: timeBeginPeriod(1) на время звонка; Linux — ничего */
extern bool g_audio_busy;                       /* звонок идёт: HeapCompact при закрытии других потоков звука — нет */
void voice_init(void);
void voice_join(u64 guild_id, u64 channel_id);
void voice_leave(void);
void voice_toggle_mute(void);
void voice_toggle_deaf(void);
void voice_set_volume(u64 user, float vol);
float voice_get_volume(u64 user);
/* звук чужой трансляции при просмотре: своя громкость 0..2 (отдельно от голоса того же человека) и «выключен»;
 * сохраняется с настройками голоса (sv=ID:pct, выключенный — sv=ID:pct:m) */
void voice_stream_volume_set(u64 user, float vol);
float voice_stream_volume(u64 user);
void voice_stream_mute_set(u64 user, bool mute);
bool voice_stream_muted(u64 user);
void voice_vad_adjust(int delta_db);
int voice_vad_db(void);
void voice_ptt(bool down);
extern int voice_restarts;                      /* selftest: входов через полный выход (№149) */
/* №140/146: устройства звука для настроек. out: 1 — вывод, 0 — микрофон. names — до max имён (UTF-8),
 * вернуть число (-1 — список недоступен). Имя "" — устройство по умолчанию. Выбор действует сразу (в звонке
 * устройство переоткрывается) и только до выхода — на диск не пишется. */
int audio_devices(int out, char (*names)[96], int max);
void voice_set_device(int out, const char *name);
const char *voice_device(int out);              /* выбранное имя; "" — по умолчанию */
/* Тест микрофона: on — открыть микрофон (и только его поток), уровень — voice_mic_level(); off — закрыть.
 * Ничего не пишется и не отправляется; в звонке тест не открывает устройство второй раз (уровень — из звонка).
 * При смене уровня главный цикл будится (не чаще 10 раз в секунду): ui_tick сравнивает и рисует. */
void voice_mic_test(bool on);
/* №143 фильтр микрофона: срез ниже 90 Гц (гул, стук по столу) — до шумоподавления/порога и Opus; по умолчанию вкл. */
void voice_hpf(bool on);
bool voice_hpf_on(void);
/* громкость микрофона 0..400 % (по умолчанию 100; --mic-gain=N); выше 100 — с мягким ограничением */
void voice_mic_gain_set(int pct);
int voice_mic_gain(void);
void voice_mic_gain_apply(int16_t *s, int n);
/* настройки голоса в системном хранилище (запись "voice"): загрузка при старте, запись при изменении и выходе */
void voice_settings_load(void);
void voice_settings_flush(void);
void voice_settings_forget(void);               /* паника, --logout */
int voice_settings_format(char *b, size_t cap); /* самопроверка */
bool voice_settings_parse(const char *s);
/* окно Video скрыто от снимков экрана и чужой демонстрации (по умолчанию нет; сохраняется с настройками) */
void app_hide_capture_set(bool on);
bool app_hide_capture(void);
void voice_duck_others(bool on);                /* приглушать другие программы в звонке (по умолчанию нет) */
bool voice_duck_others_on(void);
/* №145 звуки звонка — синтез (без файлов): звонок (повторяется до sound_stop), вход/выход, микрофон вкл/выкл,
 * сообщение. Вывод открывается только на время звука. sounds_enable(false) — все выключены. */
enum { SOUND_RING, SOUND_JOIN, SOUND_LEAVE, SOUND_MUTE, SOUND_UNMUTE, SOUND_MSG };
void sound_play(int which);
void sound_stop(void);
void sounds_enable(bool on);
bool sounds_enabled(void);
int sound_synth(int which, int16_t *out, int max);   /* PCM 48 кГц моно, сэмплов (один проход; для selftest) */
/* №69 голосовые сообщения: воспроизведение потоком (ogg/opus с CDN по кнопке, файл целиком не грузится,
 * пик <= 0,6 МБ, всё освобождается по окончании). Записи и отправки нет. Один за раз. */
typedef struct { u64 mid; bool active, loading; char err[120]; int pos_ms, dur_ms; } VoiceMsgState;
bool vmsg_play(u64 mid);                        /* играть голосовое сообщение mid открытого канала (прежнее — стоп) */
void vmsg_stop(void);
const VoiceMsgState *vmsg_state(void);          /* снимок; active=false — закончилось/остановлено (err — почему) */
extern bool g_audio_fake;                       /* selftest: звук не открывать (кроме ST_REALAUDIO=1) */
int voice_mic_level(void);                      /* 0..100 (-60..0 дБ); -1 — тест закрыт или микрофон не открылся */
void voice_status(VoiceStatus *out);
void voice_on_state(u64 guild, u64 channel, u64 user, const char *session_id);
void voice_on_server(u64 guild, const char *token, const char *endpoint);
void voice_shutdown(void);
int voice_selftest(void (*check)(bool ok, const char *what));
extern PlatWake *g_wake;                        /* будит главный цикл из других потоков */

/* ======================= audio.c ======================= */
typedef struct Audio Audio;
Audio *audio_open(const char *mic_hint, const char *out_hint, char *err, size_t errn);
Audio *audio_open_out(const char *out_hint, char *err, size_t errn);   /* только вывод (микрофон не открывается) */
void audio_close(Audio *a);
int audio_waitables(Audio *a, Waitable *out, int max);
/* Забрать накопленный звук микрофона: кадры по 960 моно-сэмплов (20 мс, 48 кГц). */
bool audio_read_frame(Audio *a, int16_t *pcm960);
/* Сколько стерео-кадров (48 кГц) устройство вывода готово принять, и отдать их. */
int audio_render_space(Audio *a);
int audio_render_queued(Audio *a);               /* кадров уже в устройстве (держим ~50 мс по часам устройства) */
void audio_render_write(Audio *a, const int16_t *stereo, int frames);
bool audio_lost(Audio *a);                      /* устройство отключили */
void audio_list_devices(void);

/* ======================= ui.c ======================= */
void ui_init(void);
void ui_shutdown(void);
void ui_handle_input(void);
void ui_dirty(void);
void ui_notice_clear(void);
void ui_render_if_dirty(void);
void ui_notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Перевод видимой строки на язык интерфейса (ui.c, таблица ui_en.h); нет перевода — русская как есть.
 * format_arg: проверки printf-формата сохраняются (snprintf(err, n, ui_tr("... %s"), x)). */
const char *ui_tr(const char *ru) __attribute__((format_arg(1)));
void ui_on_ready(void);
void ui_on_message(u64 channel_id, bool mention);
void ui_tick(void);
i64 ui_next_wake(void);
void ui_snapshot(int w, int h, Buf *out);
void ui_feed_key(const Key *k);
/* Окно входа (ui.c рисует, движок — login.c/login.h). vless_step — первым шагом спросить ссылку VLESS. */
void ui_login_begin(bool vless_step);
bool ui_login_active(void);
int selftest(void);
int selftest_gateway(void);
int selftest_keyring(void);
extern bool g_quit;

/* ======================= vless.c: туннель VLESS (разработчик сети) =======================
 * Туннель вкомпонован в mincord: по умолчанию свой на C (xlite_c.c + tls13.c, VLESS_IMPL=c), вариант —
 * Xray-core на Go (tools/xlite, VLESS_IMPL=go). Конфигурация — в памяти (на диск ничего), журналов нет. Локальный
 * SOCKS5 на 127.0.0.1 (TCP + UDP). Когда VLESS включён, ВЕСЬ трафик идёт через него; без явного
 * согласия пользователя клиент напрямую не ходит. */
typedef enum { VL_OFF, VL_STARTING, VL_UP, VL_DOWN } VlessState;
VlessState vless_state(void);
const char *vless_error(void);                  /* причина VL_DOWN по-русски, "" если нет */
const char *vless_proxy_url(void);              /* "socks5h://логин:пароль@127.0.0.1:N" при VL_UP/VL_STARTING, иначе NULL */
/* Локальный SOCKS5 с логином и случайным паролем на сессию (чужие программы туннелем не воспользуются):
 * адрес и учётные данные для UDP ASSOCIATE голоса. false — туннель не поднят. */
bool vless_socks(char *ip, size_t ipn, int *port, char *user, size_t usern, char *pass, size_t passn);
int vless_latency_ms(void);                     /* последняя проверка, -1 нет данных */
void vless_report_failure(const char *what);    /* сеть/голос: через туннель не идёт; NULL — прошло, счётчик сбоев сброшен */
bool vless_restart(void);                       /* перезапуск туннеля + проверка */
/* Разобрать ссылку, поднять туннель, сделать пробный запрос к Discord через него (сразу при вводе).
 * true — работает (latency_ms заполнен); false — err по-русски, туннель не поднят. */
bool vless_set_link(const char *link, char *err, size_t errn, int *latency_ms);
void vless_disable(void);                       /* пользователь явно выбрал «напрямую» */
void vless_stop(void);                          /* выход/паника: затереть ссылку и ключи туннеля */
void vless_forget(void);                        /* удалить сохранённую ссылку из хранилища */
bool vless_load_saved(void);                    /* при входе: ссылка из хранилища, проверить и поднять */
void vless_service(void);                       /* главный цикл: следить за туннелем */
i64 vless_next_wake(void);                      /* когда разбудить vless_service (автопроверка туннеля) */
int vless_waitables(Waitable *out, int max);    /* события туннеля для главного цикла (сейчас нет) */
long vless_xray_rss_kb(void);                   /* память отдельного процесса туннеля (его нет: -1) */
void *secret_vless_area(size_t *cap);           /* secret.c: закреплённая область для данных VLESS */
extern bool g_readonly;                         /* net.c: только GET (скрытый снимок живого аккаунта) */
extern bool (*g_http_guard)(const char *method, const char *path);          /* --livetest: false — запрос не ставится */
extern void (*g_http_trace)(const char *method, const char *path, long status); /* --livetest: журнал ответов */
int selftest_live(void);                        /* --livetest: проверка записей на своём тестовом сервере */
extern bool g_headless;                         /* ui.c: проверки без терминала */

/* ======================= voicefx.c: смена голоса (разработчик голоса) ======================= */
typedef enum { VFX_OFF, VFX_LOW, VFX_HIGH } VoiceFx;
void voicefx_set(VoiceFx m);                    /* из главного потока; голосовой поток читает атомарно */
VoiceFx voicefx_get(void);
void voicefx_cycle(void);                       /* выкл -> ниже -> выше -> выкл */
void voicefx_process(int16_t *mono, int n);     /* голосовой поток: кадр микрофона 48 кГц до Opus */

/* ======================= panic.c: паник-кей (разработчик голоса) =======================
 * Тройное нажатие выбранной клавиши (за 1,5 с): затереть токен и ссылку VLESS в памяти, удалить их
 * из хранилища, погасить голос и туннель, очистить экран и буфер прокрутки, выйти. Аккаунт не трогается. */
bool panic_init(const char *key_name);          /* "F12", "Ctrl+X", ...; false — не распознано */
const char *panic_key_name(void);               /* "" если не задан */
/* Скармливать каждое нажатие. true — это паник-клавиша и нажатие засчитано в серию (вызывающий
 * откладывает её обычное действие до 1,5 с: на третьем нажатии произойдёт паника, и отложенное не
 * выполнится). false — не паник-клавиша (серия сбрасывается). На третьем нажатии не возвращается. */
bool panic_key(const Key *k);
void panic_now(void);                           /* не возвращается */
void voice_panic_stop(void);                    /* паника: заглушить голос без ожидания потока */
int panic_selftest(void (*check)(bool ok, const char *what));

/* ======================= cli.c: консольный (построчный) интерфейс ======================= */
void cli_init(void);
void cli_shutdown(void);
void cli_handle_input(void);                    /* читать готовые строки ввода, выполнять команды */
int cli_waitables(Waitable *out, int max);
void cli_tick(void);
i64 cli_next_wake(void);
/* вызываются из ui.c при O.cli */
void cli_dirty(void);
void cli_notice(const char *text);
void cli_on_ready(void);
void cli_on_message(u64 channel_id, bool mention);
void cli_open_dm(u64 channel_id);               /* из ui_open_dm при O.cli */

/* ======================= настройки (main.c) ======================= */
typedef struct {
    bool no_save, invisible, typing, no_ack, demo, logout, ptt, hq, cli, vless_ask, no_vless, no_images;
    bool tray;                                  /* --tray: свернуть окно — в значок трея (Windows) */
    bool no_update;                             /* --no-update: не проверять обновления */
    const char *panic;                          /* --panic=КЛАВИША */
    const char *voice;                          /* --voice=low|high */
    const char *mic, *out;
} Opts;
extern Opts O;

/* ======================= demo.c ======================= */
void demo_load(void);
char *demo_en_json(const char *s, size_t n, size_t *out_n);   /* i18n: английский демо-набор (NULL — без подмены) */
const char *demo_tr(const char *ru);
int demo_en_selftest(void (*check)(bool ok, const char *what));
void demo_send(u64 channel, const char *text);
void demo_around(u64 channel, u64 msg_id);      /* переход к сообщению (#флуд — 50 вокруг) */
void demo_older(u64 channel);                   /* подгрузка старых в демо (ui.c при прокрутке вверх) */
/* демо: синтетический GUILD_MEMBER_LIST_UPDATE (SYNC окон chunk[0..n)) -> store_event, как с шлюза */
void demo_memlist(u64 guild_id, const char *list_id, const int *chunk, int n);
u64 demo_new_id(void);                          /* демо: новый id (сервер/канал/приглашение) */
void demo_search(u64 guild_id, u64 channel_id, const char *query);   /* фальшивые результаты в search_apply */
void demo_file(u64 channel, const char *filename, long long size, const char *text);  /* «[файл: имя]» в ленту */
void search_apply(const cJSON *resp);           /* store.c: разобрать ответ поиска (и демо) */
void invite_apply(const cJSON *resp);           /* store.c: карточка приглашения (и демо) */
void pins_apply(const cJSON *resp);             /* store.c: ответ закреплённых (и демо) */
void mentions_apply(const cJSON *arr);          /* store.c: ответ упоминаний (и демо) */
void profile_apply(const cJSON *resp);          /* store.c: ответ профиля (и демо) */
void topic_set(u64 channel_id, const char *topic);   /* store.c: тема открытого канала (и демо), без JSON */
void demo_topic(u64 channel_id);                /* -> topic_set */
void store_vs_self(u64 gid, u64 cid, bool mute, bool deaf);   /* демо: вы в голосовом канале (cid 0 — вышли) */
void demo_pins(u64 channel_id);                 /* demo.c: фальшивые закреплённые -> pins_apply */
void demo_pin(u64 channel_id, u64 msg_id, bool pin);
void demo_mentions(void);                       /* -> mentions_apply */
void demo_profile(u64 uid, u64 guild_id);       /* -> profile_apply */

#endif
