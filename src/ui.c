/* Интерфейс: раскладка как у Discord, вся графика ASCII, чёрно-белый (стиль - DESIGN.md).
 * Один поток VT-последовательностей для терминала и для окна (окно - сетка символов, мини-терминал).
 * Кадр собирается в сетке клеток (4 байта на клетку), на экран уходят только изменившиеся строки.
 * Мышь: при отрисовке кадр заполняет таблицу «область -> действие» (фиксированный массив). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mc.h"
#include "plat.h"
#include "img.h"
#include "login.h"
#include "ui_en.h"
#ifdef ASTRA_VIDEO
#include "share.h"
#include "golive.h"
#include "cam.h"
static ShareState SHS;                          /* снимок состояния трансляции (ui_tick) */
static int GL_phase, GL_prev;                    /* фаза Go Live для индикатора */
static bool GL_wait;                             /* в эфире, но зрителей ещё нет */
#ifdef ASTRA_VIDEO
static GoWatchState WS;                          /* просмотр чужой трансляции: снимок для индикатора */
/* панель просмотра: chat — скрыта (виден чат, просмотр идёт), full — на всё окно, ch/gd — где открыли
 * (ушли в другой канал — панель прячется); su — кто транслирует в моём голосовом канале (кадр) */
static struct { bool chat, full; int n, gd; u64 su[8], ch; u8 sk[8]; u64 cam; } WV;   /* sk: 0 трансляция, 1 камера; cam — смотрим камеру (не 0) */
static u64 demo_watch;                           /* --demo: подставной просмотр (сети нет) */
static CamState CS;                              /* своя камера (cam.h): снимок для индикатора (ui_tick) */
/* Анонимизация своей камеры (anon.h): глаза под прямоугольником; лицо не найдено — собеседники видят значок
 * Astra DS вместо кадра, у себя в предпросмотре — предупреждение (кадр рисует anon_preview_warning; ui.c — то же
 * клетками под растром: в демо, в --tui и пока кадра нет). Только то,
 * что уходит от вас; чужие камеры не трогаются. Флажок — anon_default()/anon_set_default(), действует сразу. */
#ifdef ASTRA_ANON
#include "anon.h"
#else                                            /* сборка без src/anon.c: скрывать нечем — флажок и пометки молчат */
enum { ANON_OFF, ANON_TRACKING, ANON_NOFACE };
static bool anon_default(void) { return false; }
static void anon_set_default(bool on) { (void)on; }
static int anon_status(void) { return ANON_OFF; }
#endif
enum { CA_OFF, CA_EYES, CA_FULL };               /* в уходящем кадре: ничего не скрыто / глаза / лица нет — значок */
static int cu_anon_st(void) {
    if (!anon_default()) return CA_OFF;
    if (O.demo) return now_ms() / 4000 % 4 == 3 ? CA_FULL : CA_EYES;   /* демо: иногда «лицо не найдено» */
    return anon_status() == ANON_TRACKING ? CA_EYES : CA_FULL;   /* ещё нет кадров — тоже закрыто целиком (fail-closed) */
}
#endif
static char GL_err[128];
static bool sh_prev_streamer, sh_running;
static u64 sh_ch;
static u64 sh_gid;
static bool sh_gl_wait;                          /* Wayland: ждём ответа системного окна выбора, Go Live ещё не начат */
#endif

bool g_quit;
/* Язык интерфейса: строки пишутся по-русски, при английском tr() находит пару в TR_EN (двоичный поиск по
 * отсортированной таблице в .rodata; памяти не занимает). Нет пары — показывается русская строка. */
bool g_lang_en;
static const char *tr(const char *ru) __attribute__((format_arg(1)));   /* форматы в tr("...") проверяются как литералы */
static const char *tr(const char *ru) {
    if (!g_lang_en || !ru) return ru;
    int lo = 0, hi = (int)(sizeof TR_EN / sizeof *TR_EN) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(ru, TR_EN[mid].ru);
        if (!c) return TR_EN[mid].en;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return ru;
}
#define TR(s) tr(s)
/* «Astra DS Video» — отдельная сборка (build.sh VIDEO=1 -> -DASTRA_VIDEO): демонстрация экрана и камера. */
#ifdef ASTRA_VIDEO
#define APP_NAME "Astra DS Video"
#else
#define APP_NAME "Astra DS"
#endif
/* Для store.c/net.c/main.c/login.c: та же таблица (объявление — в mc.h у разработчика). */
const char *ui_tr(const char *ru) __attribute__((format_arg(1)));
const char *ui_tr(const char *ru) { return tr(ru); }

enum { F_GUILDS, F_CHANNELS, F_MESSAGES, F_INPUT };
enum { M_NORMAL, M_REPLY, M_EDIT, M_REACT, M_DELETE,
       M_VLINK,                                  /* ввод новой ссылки VLESS (скрыт звёздочками) */
       M_VLOFF,                                  /* подтверждение «отключить VLESS» */
       M_FILE,                                   /* ввод пути к файлу («+» в поле ввода, Ctrl+O) */
       M_UNFRIEND };                             /* подтверждение «удалить из друзей» (U.target — id друга) */
enum { R_HEADER, R_CHAN, R_MEMBER, R_GAP, R_VSTAT };   /* R_VSTAT — «Выбрать статус канала» под голосовым, где вы */

typedef struct { int kind; u64 id; u64 user; } Row;

/* ---- таблица кликов: что нарисовано в клетке ---- */
enum {
    A_NONE,
    A_AR_RAIL, A_AR_CHANS, A_AR_FEED, A_AR_HELP, /* области прокрутки колесом */
    A_AR_LIST,                                   /* arg: 0 список друзей, 1 результаты поиска, 2 выбор друга */
    A_AR_TXT,                                    /* блок текстового вложения (колесо): arg — индекс в S.msgs */
    A_AR_MEMB,                                   /* список участников сервера справа (колесо) */
    A_BLOCK,                                     /* фон карточки или меню: клик не проходит насквозь */
    A_GUILD, A_ROW, A_MSG, A_REACT, A_MENU, A_BTN, A_INPUT,
    A_OPENCH,                                    /* arg — индекс в S.chans (экран «Друзья») */
    A_SETSEC,                                    /* arg — раздел настроек */
    A_DEV,                                       /* «Голос и видео»: sub — 0 ввод / 1 вывод, arg — -1 назад / +1 вперёд */
    A_IMGV,                                      /* превью картинки: arg — индекс в ivh (открыть просмотр) */
    A_CODECP,                                    /* «[копировать]» блока кода: arg — индекс в cbt */
    A_FPOST,                                     /* пост форума: arg — индекс в forum_state()->p */
    A_LINK,                                      /* ссылка в тексте: arg — индекс в lkt (щелчок копирует адрес) */
    A_POLL,                                      /* ответ опроса: arg — индекс сообщения << 4 | номер ответа */
    A_POLLB,                                     /* кнопка опроса: sub 0 — убрать голос, 1 — завершить; arg — сообщение */
    A_COMP,                                      /* кнопка/список бота: arg — индекс сообщения << 5 | номер */
    A_CSOPT,                                     /* вариант в окне списка бота: arg — номер */
    A_VMSG,                                      /* №69 голосовое сообщение: arg — индекс сообщения (играть/стоп) */
    A_FOLDER,                                    /* №2 папка серверов: arg — номер папки (свернуть/развернуть) */
    A_FILTSHOW,                                  /* №120 «показать» скрытое фильтром: arg — индекс сообщения */
    A_TAB,                                       /* №9 вкладка: sub 0 — открыть, 1 — закрыть (x); arg — номер */
    A_SPLIT,                                     /* №11 граница панели: arg 0 — каналы, 1 — участники */
    A_SLASH,                                     /* №57: sub 0 — команда (arg — индекс), 1 — вариант опции (arg — номер) */
    A_FRIEND,                                    /* экран «Друзья»: sub — FA_*, arg — индекс в rel_list */
    A_FRTAB,                                     /* вкладка «Друзья»: arg — 0..3 */
    A_SRES,                                      /* результат поиска: arg — индекс в hits */
    A_PICK,                                      /* «Выберите друзей»: arg — индекс в rel_list */
    A_SPOIL,                                     /* спойлер в сообщении: arg — индекс в S.msgs */
    A_MENT,                                      /* @упоминание: arg — индекс в S.users */
    A_CHLINK,                                    /* #канал в тексте: arg — индекс в S.chans */
    A_REPLYJ,                                    /* строка ответа: arg — индекс в S.msgs (переход к оригиналу) */
    A_QS,                                        /* Ctrl+K: arg — строка списка */
    A_EMOJI,                                     /* пикер эмодзи: arg — индекс в EMO */
    A_AC,                                        /* автодополнение: arg — строка списка */
    A_FORMF,                                     /* окно-форма: поле (arg), sub — часть поля */
    A_FORMB,                                     /* окно-форма: кнопка (sub — FB_*) */
    A_RVSEL,                                     /* «Реакции»: arg — реакция */
    A_MEMB                                       /* участник в списке справа: arg — индекс строки списка (карточка) */
};
enum { FA_ROW, FA_MSG, FA_MORE, FA_ACCEPT, FA_DECLINE, FA_UNBLOCK };
enum {
    B_MIC = 1, B_DEAF, B_FX, B_LEAVE, B_ATTACH, B_CANCEL, B_CALL_OK, B_CALL_NO, B_CALL,
    B_VL_R, B_VL_C, B_VL_D, B_VL_CHECK, B_VL_BACK, B_YES, B_NO, B_HELP, B_HELP_CLOSE, B_BOTTOM, B_OLDER,
    B_JOIN, B_SETTINGS, B_PLUS, B_SERVER,
    B_NOP,                                       /* «нет в mincord»: щелчок показывает подсказку словом */
    B_MEMBERS, B_CHATX, B_VOPEN, B_FRIENDS, B_SET_CLOSE, B_VADDN, B_VADUP, B_QUIT,
    B_SEARCH, B_SRCH_X, B_SRCH_CLR, B_ADDDM, B_PICK_GO, B_PICK_X,
    B_CARD_MSG, B_CARD_ID, B_CARD_X, B_MB_REACT, B_MB_REPLY, B_MB_EDIT, B_MB_MORE,
    B_PING, B_PINS, B_INBOX, B_PRESENT, B_STATUS, B_CARD_BLOCK, B_CARD_ADD, B_NEWMSG,
    B_PINS_MORE, B_INBOX_T0, B_INBOX_T1, B_ACK_ALL, B_EP_X, B_EMOJI, B_CAP_GO, B_CAP_NO,
    B_ADDSRV, B_GMENU, B_GINVITE, B_ADDFRIEND, B_TXT_EXP, B_TXT_DL, B_STARTASK, B_RV_X, B_EPH_HIDE, B_EMBEDS, B_MRETRY, B_MDISCARD,
    B_MICTEST, B_ZOOM_DN, B_ZOOM_0, B_ZOOM_UP, B_IV_X, B_IV_PREV, B_IV_NEXT, B_FSORT, B_FPREV, B_FNEXT, B_CSEL_X, B_CSEL_OK, B_ALERTS, B_REQS, B_RQTAB, B_RQ_OK, B_RQ_NO, B_CARD_KICK, B_CARD_FULL, B_RING_AGAIN, B_RING_STOP, B_RING_ALL_STOP, B_FULL, B_FILTER, B_STREAMER, B_NOTIFY, B_AVBIG, B_ARCH_X, B_FNEW, B_MEPROF, B_TM_X, B_RAMBAR, B_IMGCLICK, B_FSPOIL, B_FALT, B_MOD, B_FSEND, B_CARD_IGN, B_CARD_NOTE, B_NOTE_EDIT, B_RULES, B_RULES_OK, B_RULES_X, B_SOUNDS, B_HPF, B_LANG, B_UP_APPLY, B_UP_LATER, B_UP_RESTART, B_UP_AUTO, B_UP_CHECK, B_DUCK, B_SHARE, B_UP_ANYWAY, B_UP_CANCEL, B_PTT_SET, B_PTT_CLR, B_VADAUTO, B_VSTATUS, B_NS_OFF, B_NS_SPX, B_NS_RNN, B_WATCH, B_WATCH_STOP, B_WATCH_PICK, B_WATCH_FULL, B_WATCH_CHAT, B_WATCH_SHOW, B_CAM, B_CAM_PV, B_CAM_VIEW, B_CAM_TILE, B_ANON, B_MICG_DN, B_MICG_UP, B_MICG_0,
    B_SP_PREV, B_SP_NEXT
};
enum { MI_REPLY = 1, MI_EDIT, MI_DELETE, MI_REACT, MI_COPY,
       MI_FX, MI_VADDN, MI_VADUP, MI_HELP, MI_QUIT,             /* меню сообщения и меню настроек */
       MI_FR_MSG, MI_FR_DEL, MI_FR_UNBLOCK,                      /* меню «...» друга */
       MI_COPYLINK, MI_COPYID, MI_PIN, MI_UNREAD,             /* полное меню сообщения */
       MI_ST_ON, MI_ST_IDLE, MI_ST_DND, MI_ST_INV,            /* выбор статуса */
       MI_G_INV, MI_G_SET, MI_G_CH, MI_G_CAT, MI_G_MOVE, MI_G_LEAVE,   /* меню сервера */
       MI_C_EDIT, MI_C_PERMS, MI_C_INV, MI_C_DEL, MI_C_ID,    /* меню канала */
       MI_FORWARD, MI_REACTS,                                /* пакет А: переслать, кто поставил реакцию */
       MI_G_MUTE, MI_G_NOTIFY, MI_C_MUTE, MI_C_NOTIFY,       /* заглушить, уровень уведомлений */
       MI_THREAD, MI_T_JOIN, MI_GDM_ADD, MI_GDM_LEAVE, MI_FOCUS30, MI_FOCUS60, MI_FOCUS120, MI_FOCUSOFF, MI_C_VSTATUS, MI_STATUS_SET, MI_C_ARCH, MI_T_RENAME, MI_T_ARCH, MI_T_LOCK, MI_T_MEMB, MI_GDM_RENAME,                                 /* пакет Б: создать ветку; вступить/покинуть ветку */
       MI_MRETRY, MI_MDISCARD,
       MI_V_VOL, MI_V_UP, MI_V_DN, MI_V_MUTE, MI_V_PROF, MI_V_MOVE, MI_V_SMUTE, MI_V_MIC, MI_V_DEAF };   /* меню участника голосового канала */                             /* неотправленное: повторить, отменить */
enum { MENU_MSG = 1, MENU_SET, MENU_FRIEND, MENU_STATUS, MENU_GUILD, MENU_CHAN, MENU_VMEM };
#define MENU_LEFT(k) ((k) == MENU_SET || (k) == MENU_STATUS || (k) == MENU_GUILD || (k) == MENU_CHAN || (k) == MENU_VMEM)
enum { RP_NONE, RP_PINS, RP_INBOX };
/* как подсвечивать при наведении */
enum { HV_BG, HV_ACC, HV_MSG, HV_RED, HV_UL, HV_PILL, HV_ICON };
typedef struct { short x, y, w, h; u8 act, sub, hv, tip; int arg; } Hit;
/* подсказки словом при наведении на значки (Hit.tip) */
enum { T_NONE, T_INVITE, T_EVENTS, T_BOOST, T_MIC, T_DEAF, T_SET, T_LEAVE, T_CAM, T_SCREEN, T_FX, T_SOUNDB,
       T_NOISE, T_ADDSRV, T_MEMBERS, T_HELP, T_CALL, T_GIFT, T_GIF, T_STICKER, T_EMOJI, T_HOME, T_ATTACH,
       T_CHATX, T_SEARCH, T_E2EE, T_ADDDM, T_FRTAB, T_NITRO, T_VSTAT, T_CHSET, T_FIND, T_MORE, T_WRITE, T_MB_REACT, T_MB_REPLY, T_MB_EDIT, T_PING, T_PINS, T_INBOX, T_STATUS, T_NEWMSG, T_DATE
#ifdef ASTRA_VIDEO
       , T_WSTOP, T_WFULL, T_WCHAT, T_WPICK, T_WSHOW, T_CAMPV, T_CAMOFF, T_CAMVIEW, T_ANON
#endif
       , T_END };
#ifdef ASTRA_VIDEO
#define TIP_SCREEN "Демонстрация экрана"     /* в Video-сборке работает (в обычной — «нет в Astra DS») */
#define TIP_CAM "Камера (F12, Alt+C)"
#else
#define TIP_SCREEN "Демонстрация экрана: нет в Astra DS"
#define TIP_CAM "Камера: только в Astra DS Video"
#endif
static const char *const TIPS[] = {
    "", "Пригласить: нет в Astra DS", "Мероприятия: нет в Astra DS", "Бусты сервера: нет в Astra DS",
    "Микрофон (F5)", "Звук (F6)", "Настройки пользователя", "Отключиться (F7)", TIP_CAM,
    TIP_SCREEN, "Смена голоса (F11, Alt+V)", "Звуковая панель: нет в Astra DS",
    "Подавление шума: в настройках - Голос и видео", "Добавить сервер: нет в Astra DS", "Список участников",
    "Справка (F1)", "Позвонить (F4)", "Подарок: нет в Astra DS", "GIF: нет в Astra DS", "Стикеры: нет в Astra DS",
    "Эмодзи: пишите :код: или вставьте эмодзи", "Личные сообщения", "Отправить файл (Ctrl+O), до 10 МБ",
    "Скрыть чат", "Поиск (Ctrl+F)", "Сквозное шифрование голоса", "Новая личная беседа",
    "Добавить в друзья: нет в Astra DS", "Nitro: нет в Astra DS", "Статус канала: нет в Astra DS",
    "Настройки канала: нет в Astra DS", "Начать беседу с другом", "Ещё", "Написать", "Добавить реакцию", "Ответить", "Изменить", "Упоминать автора ответа",
    "Закреплённые сообщения", "Входящие (Ctrl+I)", "Сменить статус", "К первому непрочитанному", "",
#ifdef ASTRA_VIDEO
    "Выйти из просмотра", "Во весь экран и обратно (Alt+F, Esc)", "Показать чат, просмотр идёт (Alt+W)",
    "Смотреть эту трансляцию (Alt+стрелки)", "Показать трансляцию (Alt+W)",
    "Скрыть или показать себя", "Выключить камеру (F12, Alt+C)", "Смотреть камеру крупно (Alt+стрелки - соседняя)", "Скрывать глаза: вкл/выкл (Alt+E)",
#endif
};
_Static_assert(sizeof TIPS / sizeof *TIPS == T_END, "TIPS и T_* разошлись");
/* клетка кадра: символ, цвет текста (младшие 24 бита) + атрибуты (старший байт), цвет фона */
typedef struct { u32 cp, fg, bg; } Cell;
#define MAX_HITS 384                             /* 6 КБ; не хватило - лишние области просто не кликаются */

/* строка ленты: байты текста не копируются, только смещения в m->content */
enum { L_GAP, L_REPLY, L_HEAD, L_TEXT, L_EDITED, L_REACT, L_PREVIEW, L_DATE, L_NEW, L_FWD, L_TXTB, L_EPH, L_FAIL, L_EMB, L_POLL, L_COMP, L_VMSG, L_FILT };
typedef struct { int mi, off, len; u8 kind, ed, st; } MLine;
enum { ST_CODE = 1, ST_QUOTE = 2, ST_SPOIL = 4, ST_FWD = 8 };   /* MLine.st: блок кода ``` / цитата > / закрытый спойлер-превью / пересланное */
/* Открытые щелчком спойлеры: сообщения (кольцо из 16, только в памяти). */
static u64 spoil_ids[16];
static unsigned spoil_n;
static u64 filt_shown[16];                       /* №120: «показать» для скрытых фильтром (в памяти) */
static unsigned filt_n;
static bool filt_open(u64 id) { for (int i = 0; i < 16; i++) if (filt_shown[i] == id && id) return true; return false; }
static bool spoil_open(u64 id) { for (int i = 0; i < 16; i++) if (spoil_ids[i] == id && id) return true; return false; }
static void spoil_reveal(u64 id) { if (!spoil_open(id)) spoil_ids[spoil_n++ & 15] = id; }

static struct {
    int W, H;
    bool dirty, full;
    int focus;
    int sel_guild, guild_scroll;
    Row *rows; int nrows, caprows;
    int sel_row, row_scroll;
    bool row_free, guild_free;                   /* список листали колесом: не тянуть к выбору */
    int sel_msg;                                 /* индекс в S.msgs; -1 — нет */
    int msg_scroll;                              /* строк от низа */
    bool follow_sel;                             /* лента следует за выбранным сообщением (сбрасывает PgUp/PgDn) */
    bool at_top;                                 /* в прошлом кадре лента показывала самый верх */
    /* ввод */
    Buf in;
    size_t cur;                                  /* позиция курсора (байт) */
    int mode;
    u64 target;                                  /* сообщение для ответа/правки/реакции/удаления */
    char notice[200];
    i64 notice_until;
    bool help;
    int help_scroll;                             /* строк справки, пролистанных вниз */
    i64 ack_at;
    i64 last_typing_sent;
    bool loading;
    char hist_state[200];                        /* что с историей открытого канала */
    /* VLESS: карточка «нет связи», ввод ссылки, подтверждение отключения */
    Buf draft;                                   /* недописанное сообщение на время ввода ссылки */
    size_t draft_cur;
    int prev_mode, prev_focus;
    u64 prev_target;
    char vl_note[200];                           /* итог последней проверки ссылки, если не подошла */
    char busy[96];                               /* идёт блокирующая проверка: что показать */
    /* паник-кей: нажатия паник-клавиши придержаны, пока не ясно, будет ли третье */
    Key pk[2];
    int npk;
    i64 pk_until;
    /* свёрнутые категории */
    u64 fold[64];
    int nfold;
    /* контекстное меню сообщения (правый щелчок) */
    bool menu;
    int menu_x, menu_y, menu_sel;
    u64 menu_msg;
    int menu_kind;
    /* экран голосового канала — когда открыт (S.open_ch) голосовой канал; справа его чат */
    bool vchat_off;                              /* чат голосового канала скрыт кнопкой x */
    bool members_off;                            /* колонка участников скрыта кнопкой в шапке */
    bool settings;                               /* экран настроек пользователя */
    int set_sec;
    /* экран «Друзья»: вкладка, выбранный и первый видимый (индексы в отсортированном списке вкладки) */
    int fr_tab, fr_sel, fr_scroll;
    u64 menu_uid;                                /* меню «...» друга */
    /* «Выберите друзей» (новая ЛС): фильтр по имени и выбранный */
    bool pick;
    int pick_sel;
    char pick_q[64];
    /* поиск: строка в шапке, панель результатов справа */
    bool sq_edit, sr_open, sr_focus;
    char sq[128];
    int sr_sel, sr_scroll;
    char file_err[200];                          /* итог проверки пути к файлу (M_FILE) */
    /* карточка пользователя (щелчок по @нику, имени, аватару) */
    u64 card_uid;
    int card_x, card_y;
    u64 bar_msg;                                 /* панель действий при наведении: над каким сообщением */
    bool no_ping;                                /* ответ без упоминания автора («@ ВЫКЛ») */
    bool no_embeds;                              /* следующее сообщение — без превью ссылок */
    int rp;                                      /* правая панель, кроме поиска: RP_PINS / RP_INBOX */
    int inbox_tab, inbox_n;                      /* «Входящие»: вкладка, строк «Непрочитанное» */
    u64 new_from;                                /* линия «Новые сообщения»: прочитано до этого id (на момент открытия) */
    bool qs;                                     /* Ctrl+K: быстрый переход */
    char qs_q[64];
    int qs_sel;
    bool ep;                                     /* пикер эмодзи; ep_msg — реакция на это сообщение (0 — вставить в поле) */
    char ep_q[32];
    int ep_sel;
    u64 ep_msg;
    u64 jump_seen;                               /* переход к сообщению уже применён (сброс в jump_to) */
    bool rp_full;                                /* правая панель занимает весь центр (узкое окно) */
    bool moving;                                 /* перестановка каналов (Alt+Up/Down) */
    u64 menu_cid;                                /* меню канала: какой канал */
    u64 menu_vch;                                /* меню участника голоса (MENU_VMEM): его канал; кто — menu_uid */
    bool fr_seen;                                /* «Друзья» уже показаны с момента входа (выбор вкладки по умолчанию) */
    u64 fwd_msg, fwd_ch;                         /* «Переслать»: какое сообщение (Ctrl+K в режиме выбора) */
    u64 eph_hidden[8];                           /* скрытые «видно только вам» (кольцо) */
    unsigned eph_n;
    int btn_arg;                                 /* arg щёлкнутой кнопки (B_PLUS — строка категории) */
    bool ac_hide;                                /* автодополнение скрыто Esc до следующей буквы */
    int ac_sel;
    /* мышь: клетка под курсором (-1 — нет) и подсвеченная область */
    int hx, hy;
    Hit hov;
    bool hov_on;
    Hit hit[MAX_HITS];
    int nhit;
    /* кадр: сетка клеток, строки ленты, хэши строк прошлого кадра */
    Cell *cell; int gw, gh;
    MLine *ml; int mlcap;
    u32 *hash; int hash_n;
    Buf frame, line;
} U = { .sel_msg = -1, .hx = -1, .hy = -1 };

/* В консольном режиме (--cli) события модели идут в cli.c, полноэкранный интерфейс не работает. */
void ui_dirty(void) { if (O.cli) { cli_dirty(); return; } U.dirty = true; }

void ui_notice(const char *fmt, ...) {
    va_list ap;
    if (O.cli) {
        char b[512];
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        cli_notice(b);
        return;
    }
    va_start(ap, fmt);
    vsnprintf(U.notice, sizeof U.notice, fmt, ap);
    va_end(ap);
    if (U.notice[0] == '\a') { term_write("\a", 1); memmove(U.notice, U.notice + 1, strlen(U.notice)); }
    U.notice_until = now_ms() + 6000;
    U.dirty = true;
}
/* Убрать уведомление сразу (selftest: подсказка внизу видна без ожидания 6 с). */
void ui_notice_clear(void) { U.notice_until = 0; U.notice[0] = 0; U.dirty = true; }

/* ======================= текущие объекты ======================= */
static u64 cur_guild_id(void) {
    if (U.sel_guild <= 0 || U.sel_guild > S.nguilds) return 0;
    return S.guilds[U.sel_guild - 1].id;
}
static bool folded(u64 id) {
    for (int i = 0; i < U.nfold; i++) if (U.fold[i] == id) return true;
    return false;
}
static void fold_toggle(u64 id) {
    for (int i = 0; i < U.nfold; i++) if (U.fold[i] == id) { U.fold[i] = U.fold[--U.nfold]; return; }
    if (U.nfold < (int)(sizeof U.fold / sizeof *U.fold)) U.fold[U.nfold++] = id;
}
static int chan_cmp_pos(const void *a, const void *b) {
    const Channel *x = chan_find(((const Row *)a)->id), *y = chan_find(((const Row *)b)->id);
    if (!x || !y) return 0;
    /* голос после текста, как в официальном клиенте */
    bool vx = x->type == CH_VOICE || x->type == CH_STAGE, vy = y->type == CH_VOICE || y->type == CH_STAGE;
    if (vx != vy) return vx ? 1 : -1;
    if (x->position != y->position) return x->position - y->position;
    return x->id < y->id ? -1 : x->id > y->id;
}
static void row_add(int kind, u64 id, u64 user) {
    if (U.nrows == U.caprows) {
        U.caprows = U.caprows ? U.caprows * 2 : 64;
        U.rows = xrealloc(U.rows, (size_t)U.caprows * sizeof(Row));
    }
    U.rows[U.nrows++] = (Row){ kind, id, user };
}
static bool chan_listed(const Channel *c) {
    return c->visible && c->type != CH_CATEGORY &&
           (c->type == CH_TEXT || c->type == CH_NEWS || c->type == CH_VOICE || c->type == CH_STAGE ||
            c->type == CH_FORUM || c->type == CH_MEDIA);
}
/* №90–95: ветка (канал-обсуждение под текстовым каналом или пост форума) и форум */
static bool is_thread(const Channel *c) { return c && (c->type == CH_THREAD || c->type == CH_PRIV_THREAD || c->type == CH_NEWS_THREAD); }
static bool is_forum(const Channel *c) { return c && (c->type == CH_FORUM || c->type == CH_MEDIA); }
/* Ветки под каналом в списке, как в Discord: те, где я участник (не в архиве), и открытая сейчас. */
static void add_thread_rows(const Channel *parent) {
    const Channel *th[16];
    int k = chan_threads(parent->id, th, 16);
    for (int j = 0; j < k; j++) {
        bool mine = (th[j]->tflags & TF_JOINED) && !(th[j]->tflags & TF_ARCHIVED);
        if (mine || th[j]->id == S.open_ch) row_add(R_CHAN, th[j]->id, 0);
    }
}
static bool is_voice_t(const Channel *c) { return c && (c->type == CH_VOICE || c->type == CH_STAGE); }
static void add_channel_rows(Row *list, int n) {
    qsort(list, (size_t)n, sizeof(Row), chan_cmp_pos);
    VoiceStatus v;
    voice_status(&v);
    for (int i = 0; i < n; i++) {
        row_add(R_CHAN, list[i].id, 0);
        Channel *c = chan_find(list[i].id);
        if (c && ((v.phase != VS_IDLE && v.channel_id == c->id) || (is_voice_t(c) && voice_channel_status(c->id)[0]))) row_add(R_VSTAT, c->id, 0);
        if (c && (c->type == CH_VOICE || c->type == CH_STAGE)) {
            const VoiceState *vs[32];
            int k = voice_members(c->id, vs, 32);
            for (int j = 0; j < k; j++) row_add(R_MEMBER, c->id, vs[j]->user_id);
        }
        if (c && c->type != CH_VOICE && c->type != CH_STAGE) add_thread_rows(c);
    }
}
static bool dm_held(const Channel *c);
static void rebuild_rows(void) {
    u64 keep = (U.sel_row >= 0 && U.sel_row < U.nrows) ? U.rows[U.sel_row].id : 0;
    u64 keep_user = (U.sel_row >= 0 && U.sel_row < U.nrows) ? U.rows[U.sel_row].user : 0;
    int keep_kind = (U.sel_row >= 0 && U.sel_row < U.nrows) ? U.rows[U.sel_row].kind : -1;
    U.nrows = 0;
    u64 gid = cur_guild_id();
    if (!gid) {
        /* ЛС уже отсортированы по свежести в store_sort_dms */
        for (int i = 0; i < S.nchans; i++) {
            Channel *c = &S.chans[i];
            if (c->guild_id || (c->type != CH_DM && c->type != CH_GROUP)) continue;
            if (dm_held(c) && c->id != S.open_ch) continue;   /* запросы и спам — в разделе «Запросы» */
            row_add(R_CHAN, c->id, 0);
            const VoiceState *vs[16];
            int k = voice_members(c->id, vs, 16);
            for (int j = 0; j < k; j++) row_add(R_MEMBER, c->id, vs[j]->user_id);
        }
    } else {
        VoiceStatus v;
        voice_status(&v);
        u64 vch = v.phase != VS_IDLE ? v.channel_id : 0;
        static u64 thr_seen;
        if (gid != thr_seen) { thr_seen = gid; threads_open(gid); }   /* активные ветки сервера (store) */
        const Channel *ot = chan_find(S.open_ch);
        u64 opar = is_thread(ot) ? ot->parent_id : 0;
        Row *tmp = xmalloc((size_t)(S.nchans + 1) * sizeof(Row));
        int n = 0;
        /* каналы без категории */
        for (int i = 0; i < S.nchans; i++) {
            Channel *c = &S.chans[i];
            if (c->guild_id == gid && !c->parent_id && chan_listed(c)) tmp[n++] = (Row){ R_CHAN, c->id, 0 };
        }
        add_channel_rows(tmp, n);
        /* категории по позиции; свёрнутая показывает только открытый канал и голосовой, где вы */
        Row *cats = xmalloc((size_t)(S.nchans + 1) * sizeof(Row));
        int nc = 0;
        for (int i = 0; i < S.nchans; i++) if (S.chans[i].guild_id == gid && S.chans[i].type == CH_CATEGORY) cats[nc++] = (Row){ R_HEADER, S.chans[i].id, 0 };
        qsort(cats, (size_t)nc, sizeof(Row), chan_cmp_pos);
        for (int k = 0; k < nc; k++) {
            n = 0;
            int all = 0;
            bool fo = folded(cats[k].id);
            for (int i = 0; i < S.nchans; i++) {
                Channel *c = &S.chans[i];
                if (c->guild_id != gid || c->parent_id != cats[k].id || !chan_listed(c)) continue;
                all++;
                if (!fo || c->id == S.open_ch || c->id == vch || c->id == opar) tmp[n++] = (Row){ R_CHAN, c->id, 0 };
            }
            if (!all) continue;
            row_add(R_GAP, 0, 0);
            row_add(R_HEADER, cats[k].id, 0);
            add_channel_rows(tmp, n);
        }
        free(cats);
        free(tmp);
    }
    U.sel_row = -1;
    for (int i = 0; i < U.nrows; i++)
        if (U.rows[i].id == keep && U.rows[i].user == keep_user && U.rows[i].kind == keep_kind) { U.sel_row = i; break; }
    if (U.sel_row < 0) for (int i = 0; i < U.nrows; i++) if (U.rows[i].kind == R_CHAN) { U.sel_row = i; break; }
}

/* ======================= сеть: действия ======================= */
static void notice_http(const char *what, Resp *r) {
    const char *m = NULL;
    cJSON *j = r->body.p ? json_parse(r->body.p, r->body.len) : NULL;
    if (j) m = jstr(j, "message");
    char *safe = san(m ? m : "", false);
    if (r->status == 0) ui_notice(TR("%s: нет связи"), what);
    else if (*safe) ui_notice("%s: %s", what, safe);
    else ui_notice("%s (HTTP %ld)", what, r->status);
    free(safe);
    json_done();
}

static void on_messages(Resp *r, void *ctx) {
    u64 cid = (u64)(uintptr_t)ctx;
    bool older = (cid & 1ULL << 63) != 0;
    cid &= ~(1ULL << 63);
    U.loading = false;
    U.dirty = true;
    if (cid != S.open_ch) return;
    if (r->status != 200) {
        /* ошибка остаётся видна в панели, а не исчезает через 6 секунд */
        cJSON *e = r->body.p ? json_parse(r->body.p, r->body.len) : NULL;
        char *m = san(jstr(e, "message") ? jstr(e, "message") : "", false);
        json_done();
        if (r->status == 0) snprintf(U.hist_state, sizeof U.hist_state, TR("история не загрузилась: нет связи"));
        else snprintf(U.hist_state, sizeof U.hist_state, TR("история не загрузилась: HTTP %ld %s"), r->status, m);
        free(m);
        return;
    }
    cJSON *j = json_parse(r->body.p, r->body.len);
    if (cJSON_IsArray(j)) store_set_messages(j, older);
    else snprintf(U.hist_state, sizeof U.hist_state, TR("история: неожиданный ответ"));
    json_done();
    if (!older && S.nmsgs == 0 && !U.hist_state[0]) snprintf(U.hist_state, sizeof U.hist_state, "в канале пока нет сообщений");
    if (!older) { U.msg_scroll = 0; U.sel_msg = -1; }
    U.ack_at = now_ms() + 300;
    U.dirty = true;
}
static void fetch_messages(bool older) {
    if (!S.open_ch || U.loading) return;
    if (older && (S.history_end || !S.nmsgs)) return;
    char path[128];
    if (older) snprintf(path, sizeof path, "/channels/%llu/messages?before=%llu&limit=50",
                        (unsigned long long)S.open_ch, (unsigned long long)S.msgs[0].id);
    else snprintf(path, sizeof path, "/channels/%llu/messages?limit=50", (unsigned long long)S.open_ch);
    U.loading = true;
    U.hist_state[0] = 0;
    http_async("GET", path, NULL, on_messages, (void *)(uintptr_t)(S.open_ch | (older ? 1ULL << 63 : 0)));
}
static void send_ack(bool expl) {
    Channel *c = chan_find(S.open_ch);
    if (!c || O.no_ack) return;
    if (c->last_msg <= c->last_read && !c->mentions) return;
    if (expl) ack_channel(c->id);                /* явное «прочитано» (Esc): снимает и ручное «непрочитанное» */
    else ack_seen(c->id);                        /* авто-отметка при просмотре: store ограничивает частоту и окно */
}
/* Черновики по каналам: до 16 последних, только в памяти (при выходе затираются в ui_shutdown). */
typedef struct { u64 ch; char *t; } Draft;
static Draft drafts[16];
static int draft_rr;
static void in_set(const char *s);
static void in_clear(void);
static void draft_drop(int i) { if (drafts[i].t) { wipe(drafts[i].t, strlen(drafts[i].t)); free(drafts[i].t); } drafts[i] = (Draft){0}; }
static void draft_swap(u64 from, u64 to) {
    if (U.mode == M_VLINK || U.mode == M_FILE) return;   /* в строке ссылка/путь — не черновик */
    if (from) {
        int slot = -1;
        for (int i = 0; i < 16; i++) if (drafts[i].ch == from) slot = i;
        if (U.in.len && (U.mode == M_NORMAL || U.mode == M_REPLY)) {
            if (slot < 0) for (int i = 0; i < 16 && slot < 0; i++) if (!drafts[i].ch) slot = i;
            if (slot < 0) slot = draft_rr++ & 15;
            draft_drop(slot);
            drafts[slot] = (Draft){ from, xstrdup(U.in.p) };
        } else if (slot >= 0) draft_drop(slot);
    }
    in_clear();
    for (int i = 0; i < 16; i++) if (drafts[i].ch == to && to) { in_set(drafts[i].t); draft_drop(i); break; }
}
/* ревью 10 М1 (store.c при очистке ленты): «не отправлено» — в черновик канала, если там пусто */
bool ui_draft_restore(u64 ch, const char *text) {
    if (!ch || !text || !*text) return false;
    if (ch == S.open_ch) {
        if (U.in.len || (U.mode != M_NORMAL && U.mode != M_REPLY)) return false;
        in_set(text);
        return true;
    }
    int slot = -1;
    for (int i = 0; i < 16; i++) if (drafts[i].ch == ch) return false;   /* свой черновик есть — не трогаем */
    for (int i = 0; i < 16 && slot < 0; i++) if (!drafts[i].ch) slot = i;
    if (slot < 0) slot = draft_rr++ & 15;
    draft_drop(slot);
    drafts[slot] = (Draft){ ch, xstrdup(text) };
    return true;
}
static struct { int sel, top; } FV;              /* №95: выбранный и первый видимый пост форума */
static u64 arch_view;                            /* №93: открыт архив веток этого канала (лента скрыта) */
/* №9 вкладки каналов (не Discord): открытые каналы и беседы, до 9; Ctrl+Tab / Ctrl+Shift+Tab, Alt+1..9,
 * Ctrl+W — закрыть, средний щелчок по вкладке — закрыть. Только в памяти. */
static struct { u64 id[9]; int n; } TB;
static struct { int side, memb, drag; i64 last; int last_arg; } PW;   /* №11: ширины панелей (0 — по умолчанию) */
static void tab_add(u64 id) {
    for (int i = 0; i < TB.n; i++) if (TB.id[i] == id) return;
    if (TB.n == 9) { memmove(TB.id, TB.id + 1, 8 * sizeof(u64)); TB.n = 8; }   /* самая старая уходит */
    TB.id[TB.n++] = id;
}
static void open_channel(u64 id) {
    Channel *c = chan_find(id);
    if (!c || c->type == CH_CATEGORY) return;
    if (S.open_ch != id) {
        /* черновик: недописанное в прошлом канале остаётся в памяти до возврата (на диск — никогда) */
        draft_swap(S.open_ch, id);
        S.open_ch = id;
        /* линия «Новые сообщения»: прочитано до этого id на момент открытия (0 — линии нет) */
        U.new_from = c->last_read && c->last_msg > c->last_read ? c->last_read : 0;
        msgs_clear();
        S.typing_until = 0;
        U.loading = false;
        U.sel_msg = -1;
        U.msg_scroll = 0;
        U.mode = M_NORMAL;
        U.menu = false;
        forum_close();                           /* посты прошлого форума больше не нужны */
        arch_view = 0;
        tab_add(id);
        if (is_forum(c)) { ml_close(); forum_open(id, 0, 0); FV.sel = FV.top = 0; U.focus = F_MESSAGES; U.dirty = true; return; }
        if (O.demo) demo_send(id, NULL);
        else fetch_messages(false);
        ml_open(c->guild_id, c->id);             /* список участников справа + op 14 (печатает, окна по 100); ЛС — закрыть */
    }
    U.focus = is_forum(c) ? F_MESSAGES : F_INPUT;
    U.dirty = true;
}

static void on_sent(Resp *r, void *ctx) {
    (void)ctx;
    if (r->status >= 300 || r->status == 0) notice_http(TR("не отправлено"), r);
}
static int user_by_name(const char *p, size_t maxn, size_t *mlen);
static int chan_by_name(const char *p, size_t maxn, size_t *mlen);
/* Текст к отправке, как делает Discord: @Имя -> <@id>, #канал -> <#id> (иначе упоминание не доходит);
 * внутри `кода` не трогаем. */
static void wire_text(const char *s, Buf *out) {
    bool code = false;
    for (size_t p = 0; s[p]; ) {
        if (s[p] == '`') code = !code;
        bool ws = p == 0 || s[p - 1] == ' ' || s[p - 1] == '\n' || s[p - 1] == '(';
        size_t ml = 0;
        int ix;
        if (!code && ws && (!strncmp(s + p, "@everyone", 9) || !strncmp(s + p, "@here", 5))) {
            size_t l = s[p + 1] == 'e' ? 9 : 5;
            buf_put(out, s + p, l);
            p += l;
            continue;
        }
        if (!code && ws && s[p] == '@' && (ix = user_by_name(s + p + 1, strlen(s + p + 1), &ml)) >= 0) {
            buf_printf(out, "<@%llu>", (unsigned long long)S.users[ix].id);
            p += 1 + ml;
            continue;
        }
        if (!code && ws && s[p] == '#' && (ix = chan_by_name(s + p + 1, strlen(s + p + 1), &ml)) >= 0) {
            buf_printf(out, "<#%llu>", (unsigned long long)S.chans[ix].id);
            p += 1 + ml;
            continue;
        }
        buf_put(out, s + p, 1);
        p++;
    }
    if (!out->p) buf_puts(out, "");
}
/* №127: «8 с», «2 мин 05 с», «1 ч 10 мин» */
static void wait_len(i64 s, char *o, size_t n) {
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(o, n, TR("%lld ч %lld мин"), (long long)(s / 3600), (long long)(s / 60 % 60));
    else if (s >= 60) snprintf(o, n, TR("%lld мин %02lld с"), (long long)(s / 60), (long long)(s % 60));
    else snprintf(o, n, TR("%lld с"), (long long)s);
}
/* Подпись в рамке поля ввода: «Медленный режим: 8 с» или «Тайм-аут на сервере до 15:42» (до 10.11 15:42 — не сегодня). */
static void send_wait_text(i64 until, bool timeout, char *o, size_t n) {
    i64 now = wall_ms() / 1000;
    if (!timeout) {
        char w[48];
        wait_len(until - now, w, sizeof w);
        snprintf(o, n, TR("Медленный режим: %s"), w);
        return;
    }
    time_t tu = (time_t)until, tn = (time_t)now;
    struct tm a, b;
#ifdef _WIN32
    localtime_s(&a, &tu); localtime_s(&b, &tn);
#else
    localtime_r(&tu, &a); localtime_r(&tn, &b);
#endif
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) snprintf(o, n, TR("Тайм-аут на сервере до %02d:%02d"), a.tm_hour, a.tm_min);
    else snprintf(o, n, TR("Тайм-аут на сервере до %02d.%02d %02d:%02d"), a.tm_mday, a.tm_mon + 1, a.tm_hour, a.tm_min);
}
static void send_message(const char *text0) {
    if (!S.open_ch) { ui_notice(TR("сначала откройте канал")); return; }
    /* «@silent текст» — тихое сообщение (без уведомлений), как в Discord */
    bool silent = !strncmp(text0, "@silent ", 8);
    if (silent) text0 += 8;
    Buf wt = {0};
    wire_text(text0, &wt);
    const char *text = wt.p;
#ifdef MC_PACKA
    if (U.mode != M_EDIT) {
        int opts = (silent ? SEND_SILENT : 0) | (U.no_embeds ? SEND_NO_EMBEDS : 0) | (U.mode == M_REPLY && U.no_ping ? SEND_NO_PING : 0);
        msg_send(S.open_ch, text, U.mode == M_REPLY ? U.target : 0, opts);   /* ошибки и состояние — у store */
        U.no_embeds = false;                     /* «без превью ссылок» — на одно сообщение */
        buf_free(&wt);
        return;
    }
#endif
    if (O.demo) { demo_send(S.open_ch, text); buf_free(&wt); return; }
    Buf b = {0};
    char path[96];
    if (U.mode == M_EDIT) {
        snprintf(path, sizeof path, "/channels/%llu/messages/%llu", (unsigned long long)S.open_ch, (unsigned long long)U.target);
        buf_puts(&b, "{\"content\":");
        buf_json_str(&b, text);
        buf_puts(&b, "}");
        http_async("PATCH", path, b.p, on_sent, NULL);
    } else {
        snprintf(path, sizeof path, "/channels/%llu/messages", (unsigned long long)S.open_ch);
        buf_puts(&b, "{\"content\":");
        buf_json_str(&b, text);
        buf_printf(&b, ",\"nonce\":\"%llu\",\"tts\":false,\"flags\":%d", (unsigned long long)snowflake_now(), silent ? 4096 : 0);
        if (U.mode == M_REPLY && U.target) {
            Channel *c = chan_find(S.open_ch);
            buf_printf(&b, ",\"message_reference\":{\"channel_id\":\"%llu\",\"message_id\":\"%llu\"",
                       (unsigned long long)S.open_ch, (unsigned long long)U.target);
            if (c && c->guild_id) buf_printf(&b, ",\"guild_id\":\"%llu\"", (unsigned long long)c->guild_id);
            buf_puts(&b, "}");
            /* «@ ВЫКЛ» в строке ответа: не упоминать автора */
            if (U.no_ping) buf_puts(&b, ",\"allowed_mentions\":{\"parse\":[\"users\",\"roles\",\"everyone\"],\"replied_user\":false}");
        }
        buf_puts(&b, "}");
        http_async("POST", path, b.p, on_sent, NULL);
    }
    buf_free(&b);
    buf_free(&wt);
}
static void delete_message(u64 id) {
    if (O.demo) { ui_notice(TR("в демо-режиме удаление не отправляется")); return; }
    char path[96];
    snprintf(path, sizeof path, "/channels/%llu/messages/%llu", (unsigned long long)S.open_ch, (unsigned long long)id);
    http_async("DELETE", path, NULL, on_sent, NULL);
}

/* Короткие коды для реакций; остальное — вставить эмодзи напрямую. */
static const char *const SHORT[][2] = {
    {"thumbsup", "\xF0\x9F\x91\x8D"}, {"+1", "\xF0\x9F\x91\x8D"}, {"thumbsdown", "\xF0\x9F\x91\x8E"},
    {"heart", "\xE2\x9D\xA4\xEF\xB8\x8F"}, {"joy", "\xF0\x9F\x98\x82"}, {"sob", "\xF0\x9F\x98\xAD"},
    {"fire", "\xF0\x9F\x94\xA5"}, {"eyes", "\xF0\x9F\x91\x80"}, {"ok_hand", "\xF0\x9F\x91\x8C"},
    {"pray", "\xF0\x9F\x99\x8F"}, {"clap", "\xF0\x9F\x91\x8F"}, {"tada", "\xF0\x9F\x8E\x89"},
    {"thinking", "\xF0\x9F\xA4\x94"}, {"skull", "\xF0\x9F\x92\x80"}, {"smile", "\xF0\x9F\x98\x84"},
    {"rofl", "\xF0\x9F\xA4\xA3"}, {"100", "\xF0\x9F\x92\xAF"}, {"white_check_mark", "\xE2\x9C\x85"},
    {"x", "\xE2\x9D\x8C"}, {"wave", "\xF0\x9F\x91\x8B"}, {"rage", "\xF0\x9F\x98\xA1"},
};
static void react(u64 msg_id, const char *emoji_in) {
    Message *m = msg_find(msg_id, NULL);
    if (!m) return;
#ifdef MC_PACKA
    if (m->state != MS_SENT) { ui_notice(TR("сообщение ещё не отправлено")); return; }   /* id = nonce: сервер его не знает */
#endif
    char emoji[160];
    snprintf(emoji, sizeof emoji, "%s", emoji_in);
    /* номер существующей реакции */
    if (emoji[0] >= '1' && emoji[0] <= '9' && !emoji[1]) {
        int k = emoji[0] - '1';
        if (k >= m->nreacts) { ui_notice(TR("нет реакции с номером %d"), k + 1); return; }
        snprintf(emoji, sizeof emoji, "%s", m->reacts[k].emoji);
    } else if (emoji[0] == ':' && emoji[strlen(emoji) - 1] == ':' && strlen(emoji) > 2) {
        emoji[strlen(emoji) - 1] = 0;
        const char *name = emoji + 1;
        const char *hit = NULL;
        for (size_t i = 0; i < sizeof SHORT / sizeof *SHORT; i++) if (!strcmp(SHORT[i][0], name)) hit = SHORT[i][1];
        /* своя эмодзи сервера, если она уже стоит на этом сообщении */
        char tmp[160];
        if (!hit) for (int i = 0; i < m->nreacts; i++) {
            const char *e = m->reacts[i].emoji;
            size_t nl = strlen(name);
            if (e[0] == ':' && !strncmp(e + 1, name, nl) && e[nl + 1] == ':') { snprintf(tmp, sizeof tmp, "%s", e); hit = tmp; }
        }
        if (!hit) { ui_notice(TR("неизвестный код :%s: - вставьте эмодзи напрямую"), name); return; }
        snprintf(emoji, sizeof emoji, "%s", hit);
    }
    bool mine = false;
    for (int i = 0; i < m->nreacts; i++) if (!strcmp(m->reacts[i].emoji, emoji)) mine = m->reacts[i].me;
    /* в адресе: unicode как есть (url-кодировано), своя эмодзи — name:id */
    Buf e = {0}, path = {0};
    if (emoji[0] == ':') url_encode(emoji + 1, &e); else url_encode(emoji, &e);
    buf_printf(&path, "/channels/%llu/messages/%llu/reactions/%s/@me?location=Message&type=0",
               (unsigned long long)S.open_ch, (unsigned long long)msg_id, e.p);
    if (!O.demo) http_async(mine ? "DELETE" : "PUT", path.p, NULL, on_sent, NULL);
    buf_free(&e);
    buf_free(&path);
}

static void call_decline(u64 ch) {
    char path[96], body[64];
    snprintf(path, sizeof path, "/channels/%llu/call/stop-ringing", (unsigned long long)ch);
    snprintf(body, sizeof body, "{\"recipients\":[\"%llu\"]}", (unsigned long long)S.me_id);
    http_async("POST", path, body, NULL, NULL);
}

/* ======================= редактор строки ======================= */
static size_t prev_cp(const char *s, size_t i) { if (!i) return 0; do i--; while (i && ((unsigned char)s[i] & 0xC0) == 0x80); return i; }
static size_t next_cp(const char *s, size_t len, size_t i) { if (i >= len) return len; u32 c; return i + (size_t)utf8_dec(s + i, &c); }
static int in_chars(void) { int n = 0; for (size_t i = 0; i < U.in.len; i = next_cp(U.in.p, U.in.len, i)) n++; return n; }
static void in_insert(u32 cp) {
    /* Строка ввода рисуется в терминал как есть: управляющие символы (ESC, C0 кроме \n, DEL, C1)
     * и суррогаты сюда не пускаем, иначе Esc Esc c сбросил бы терминал. */
    if ((cp < 0x20 && cp != '\n') || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return;
    if (in_chars() >= 2000) { ui_notice(TR("не больше 2000 символов")); return; }
    char e[4];
    int n = utf8_enc(cp, e);
    /* Рост буфера — с затиранием старого блока: в строке ввода бывает ссылка VLESS (секрет),
     * realloc оставил бы её копию в освобождённой памяти. */
    if (U.in.len + (size_t)n + 1 > U.in.cap) {
        size_t c = U.in.cap ? U.in.cap * 2 : 256;
        while (c < U.in.len + (size_t)n + 1) c *= 2;
        char *p = xmalloc(c);
        if (U.in.p) { memcpy(p, U.in.p, U.in.len + 1); wipe(U.in.p, U.in.cap); free(U.in.p); }
        else p[0] = 0;
        U.in.p = p;
        U.in.cap = c;
    }
    memmove(U.in.p + U.cur + n, U.in.p + U.cur, U.in.len - U.cur + 1);
    memcpy(U.in.p + U.cur, e, (size_t)n);
    U.in.len += (size_t)n;
    U.cur += (size_t)n;
}
static void in_delete_range(size_t a, size_t b) {
    if (b <= a) return;
    memmove(U.in.p + a, U.in.p + b, U.in.len - b + 1);
    U.in.len -= b - a;
    U.cur = a;
}
static void in_set(const char *s) {
    buf_wipe(&U.in);
    buf_puts(&U.in, s);
    U.cur = U.in.len;
}
static void in_clear(void) { buf_wipe(&U.in); U.cur = 0; }


/* ======================= сетка клеток ======================= */
/* Стиль описан в DESIGN.md. Графика интерфейса — только символы ASCII 0x20..0x7E; цвет — палитра
 * тёмной темы Discord (SGR 38;2 / 48;2). Клетка: символ, цвет текста + атрибуты, цвет фона.
 * Код символа 0 — правая половина широкого символа слева. */
#define AT_B 1                                   /* жирный */
#define AT_U 2                                   /* подчёркнутый */
#define AT_I 4                                   /* инверсия: курсор строки ввода */
#define AT_IT 8                                  /* курсив (SGR 3) — №45 */
#define AT_ST 16                                 /* зачёркнутый (SGR 9) — №45 */
#define RGB_OF(c) ((c) & 0xFFFFFFu)
#define AT_OF(c) ((int)((c) >> 24))
#define GC(x, y) U.cell[(size_t)(y) * (size_t)U.gw + (size_t)(x)]

/* палитра: приглушённая тёмно-серая, почти монохром;
 * колонки и панели разделены тонкими светлыми линиями C_LINE, акценты — зелёный голос, красные бейджи,
 * цвета ников по ролям */
#define C_RAIL    0x1a1c1fu                      /* колонка серверов */
#define C_SIDE    0x1f2124u                      /* колонка каналов */
#define C_PANEL   0x1c1e21u                      /* панели голоса и пользователя */
#define C_FEED    0x202225u                      /* лента */
#define C_LINE    0x55595eu                      /* линии-рамки между колонками и вокруг панелей */
#define C_TEXT    0xd2d5d8u
#define C_MUTED   0x8b9096u
#define C_WHITE   0xf2f3f5u
#define C_ACC     0x5865f2u                      /* акцент: только меню и главная кнопка окон */
#define C_GREEN   0x3ba55cu
#define C_RED     0xe0393eu
#define C_DANGER  0xc8363au
#define C_GOLD    0xd9a637u
#define C_SEL     0x35383cu                      /* выбранный канал */
#define C_HOV     0x2a2d31u                      /* наведение */
#define C_MSGHOV  0x26292cu                      /* наведение и выбор сообщения */
#define C_MENT    0x2e2c25u                      /* сообщение с упоминанием вас */
#define C_INPUT   0x1a1c1fu                      /* поле ввода */
#define C_MENU    0x111214u                      /* контекстное меню */
#define C_BTN2    0x383b40u                      /* вторичная кнопка, плашки */
#define C_PH      0x3a3d42u                      /* заглушка картинки, пока качается: серый круг */
#define C_REACTME 0x34374au                      /* ваша реакция */
#define C_REACTFG 0xc9cdfbu
#define C_THUMB   0x45494eu

/* Картинки (аватары, превью вложений) даёт img.c: клетки {символ ASCII, цвет символа, цвет фона}.
 * Пока картинки нет (качается, модуль выключен) — интерфейс рисует инициалы / рамку превью сам. */
static u32 rgb3(const uint8_t c[3]) { return (u32)c[0] << 16 | (u32)c[1] << 8 | c[2]; }

/* ---- мелкая сетка для картинок (окно mincord) ----
 * Прямоугольник клеток под картинку окно рисует шрифтом в FINE_K раз мельче: в превью 32x8 клеток
 * помещается 128x32 символа — в 16 раз детальнее при той же площади, по-прежнему цветной ASCII.
 * Поток: после строк кадра ui.c пишет
 *   ESC _ mincord-img ; X ; Y ; W ; H ; K ; <K*W x K*H клеток по строкам> ESC \
 * X, Y — левая верхняя клетка (с 0), W x H — клетки; каждая мелкая клетка — 13 символов: символ
 * 0x20..0x7E, цвет символа RRGGBB, цвет фона RRGGBB (hex). Окно держит такой слой поверх клеток, пока в
 * его прямоугольник не придёт обычная запись (тогда слой снимается — ui.c пришлёт его заново).
 * Терминал так не умеет: там остаётся картинка в обычных клетках. Поддержку объявляет окно сильным
 * символом term_fine_images() (gui_*.c); слабая заглушка здесь — «не умею». */
bool term_fine_images(void) __attribute__((weak));
bool term_fine_images(void) { return false; }
int term_subcell_scale(void) __attribute__((weak));
int term_subcell_scale(void) { return term_fine_images() ? 4 : 0; }
bool term_zoom(int step) __attribute__((weak));       /* №15: окно (plat_win/plat_posix); без него — консоль */
bool term_zoom(int step) { (void)step; return false; }
int term_zoom_pct(void) __attribute__((weak));
int term_zoom_pct(void) { return 100; }
#define FINE_K 4                                 /* желаемый масштаб; окно может уметь меньше */
#define MAX_OVL 16                               /* слоёв-кандидатов за кадр */
#define MAX_SEND 6                               /* столько слоёв держит окно (gui.h LAYER_MAX) */
#define SEND_BYTES (180u << 10)                  /* и не больше этого (окно: 192 КБ, 7 байт на мелкую клетку) */
#define MAX_PART 40
/* Слой — прямоугольник клеток. OV_ATT — превью вложения (видимые строки r0..r1 из ph). OV_QR — QR-код.
 * OV_STRIP — группа картинок одним слоем (все значки колонки серверов, все аватары колонки каналов, ленты,
 * панель пользователя...): прямоугольник — охват картинок группы. При выводе он режется на участки строк,
 * где вне картинок только фон и линии ('-' '_'): строка с текстом (название канала между участниками голоса,
 * значок [CS]) прерывает слой. В окно уходят MAX_SEND участков с наибольшим приоритетом группы. */
enum { OV_ATT, OV_STRIP, OV_QR };
enum { PK_AVR, PK_ICON };                        /* аватар (круг) / значок сервера (квадрат) */
enum { G_NONE, G_RAIL, G_SIDE, G_PANEL, G_FEED, G_MEMB, G_LOGO, G_TILES, G_LOGIN, G_HDR, G_SRCH, G_POP };
static const u8 G_PRIO[] = { 0, 6, 4, 5, 7, 3, 8, 8, 9, 1, 7, 10 };
typedef struct { short x, y, w, ih, r0, nr; u8 kind; u64 id; } Part;   /* строки r0..r0+nr-1 картинки w x ih с клетки (x, y) */
typedef struct {
    int kind, group, prio, x, y, w, h;
    int ph, r0, r1; char *url;                   /* OV_ATT, OV_QR (подпись — текст кода); копия на кадр, free в render */
    int np; Part p[MAX_PART];                    /* OV_STRIP */
} Ovl;
static Ovl ovl[MAX_OVL];
static int novl;
static u32 ovl_sent[MAX_SEND];                   /* подписи слоёв, отправленных в прошлом кадре */
static int nsent;
static u8 row_emitted[512];                      /* бит s — участок s строки выведен в этом кадре */
#define NSEG 8
static u32 *seg_hash;                            /* хэши участков прошлого кадра, H * NSEG */
static int seg_n;
static int seg_x(int s, int W) { return s * W / NSEG; }
/* Участок [x0, x1) строки y выведен в этом кадре? */
static bool span_dirty(int y, int x0, int x1) {
    if (y < 0 || y >= (int)sizeof row_emitted) return true;
    for (int sg = 0; sg < NSEG; sg++)
        if ((row_emitted[y] >> sg & 1) && seg_x(sg, U.gw) < x1 && seg_x(sg + 1, U.gw) > x0) return true;
    return false;
}
static bool fine_on;                             /* этот кадр рисуется в окне с мелкой сеткой */
/* Строка r превью на экране в (x, y): запомнить для слоя мелкой сетки. */
static void ovl_note(int x, int y, int w, int ph, int r, const char *url) {
    for (int i = 0; i < novl; i++)
        if (ovl[i].kind == OV_ATT && ovl[i].x == x && ovl[i].w == w && ovl[i].y + (r - ovl[i].r0) == y && !strcmp(ovl[i].url, url)) {
            if (r < ovl[i].r0) { ovl[i].r0 = r; ovl[i].y = y; }
            if (r > ovl[i].r1) ovl[i].r1 = r;
            return;
        }
    if (novl >= MAX_OVL) return;
    Ovl *o = &ovl[novl++];
    memset(o, 0, sizeof *o);
    o->kind = OV_ATT;
    o->prio = 9;
    o->x = x; o->y = y; o->w = w; o->ph = ph; o->r0 = o->r1 = r;
    o->url = xstrdup(url);
}
/* Строка r картинки w x ih, нарисованная с клетки (x, y), — в слой группы grp. */
static void strip_part(int grp, int x, int y, int w, int ih, int r, int kind, u64 id) {
    if (!fine_on) return;
    Ovl *o = NULL;
    for (int i = 0; i < novl; i++) if (ovl[i].kind == OV_STRIP && ovl[i].group == grp) { o = &ovl[i]; break; }
    if (!o) {
        if (novl >= MAX_OVL) return;
        o = &ovl[novl++];
        memset(o, 0, sizeof *o);
        o->kind = OV_STRIP;
        o->group = grp;
        o->prio = G_PRIO[grp];
    }
    for (int q = o->np - 1; q >= 0 && q >= o->np - 8; q--) {   /* следующая строка уже начатой картинки */
        Part *pt = &o->p[q];
        if (pt->id == id && pt->kind == kind && pt->x == x && pt->w == w && pt->ih == ih && pt->r0 + pt->nr == r && pt->y + pt->nr == y) { pt->nr++; return; }
    }
    if (o->np >= MAX_PART) return;
    o->p[o->np++] = (Part){ (short)x, (short)y, (short)w, (short)ih, (short)r, 1, (u8)kind, id };
}

/* Окно, меню или карточка поверх (x, y, w, h): картинки слоёв под ним не показывать мелкой сеткой
 * (слой окна лежит поверх клеток и закрыл бы окно); они остаются обычными клетками под окном. */
static void ovl_cut(int x, int y, int w, int h) {
    for (int i = 0; i < novl; i++) {
        Ovl *o = &ovl[i];
        if (o->kind == OV_STRIP) {
            /* часть картинки, задетая окном, режется по строкам: строки выше и ниже окна остаются мелкой сеткой */
            Part keep[MAX_PART];
            int k = 0;
            for (int q = 0; q < o->np; q++) {
                Part p = o->p[q];
                if (!(p.x < x + w && p.x + p.w > x && p.y < y + h && p.y + p.nr > y)) { if (k < MAX_PART) keep[k++] = p; continue; }
                if (p.y < y && k < MAX_PART) { Part a = p; a.nr = (short)(y - p.y); keep[k++] = a; }
                if (p.y + p.nr > y + h && k < MAX_PART) {
                    Part b = p;
                    int skip = y + h - p.y;
                    b.y = (short)(y + h); b.r0 = (short)(p.r0 + skip); b.nr = (short)(p.nr - skip);
                    keep[k++] = b;
                }
            }
            memcpy(o->p, keep, sizeof(Part) * (size_t)k);
            o->np = k;
        } else if (o->kind == OV_ATT && o->x < x + w && o->x + o->w > x && o->y < y + h && o->y + (o->r1 - o->r0) >= y)
            o->w = 0;                            /* emit_fine пропускает слой нулевой ширины */
    }
}

/* ---- синтетические картинки демо (--demo без сети): чтобы видеть, как выглядит настоящая ASCII-картинка ----
 * Не рожицы: значки — эмблемы (косая черта, кристалл, пламя, череп, звезда, кот), аватары — «фото»
 * (освещённая сфера на фоне), вложения — мем (кричащее лицо, ч/б) и мелкие картинки. */
enum { SY_AVATAR, SY_ICON, SY_MEME, SY_PHOTO };
static u32 hsh32(u64 v) { u32 h = (u32)(v ^ v >> 29) * 2654435761u; h ^= h >> 15; h *= 0x2c1b3c6du; return h ^ h >> 12; }
static float sq(float a) { return a * a; }
static void synth(u64 seed, int kind, int w, int h, ImgCell *out) {
    static const char ramp[] = " .,:;-=+*%#@";
    u32 hs = hsh32(seed);
    int var = (int)(hs % 6);
    float hr = 0.35f + (float)(hs >> 8 & 255) / 512.0f, hg = 0.35f + (float)(hs >> 16 & 255) / 512.0f, hb = 0.35f + (float)(hs >> 24) / 512.0f;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        float u = ((float)i + 0.5f) / (float)w, v = ((float)j + 0.5f) / (float)h, L = 0, r = 1, g = 1, b = 1;
        float du = u - 0.5f, dv = v - 0.5f;
        switch (kind) {
        case SY_AVATAR: {                        /* шар на тёмном фоне, свет слева сверху */
            float d = sq(du) + sq(dv);
            if (d < 0.16f) { float z = 1 - d / 0.16f; L = 0.35f + 0.9f * z * (0.6f - du * 0.8f - dv * 0.8f); }
            else L = 0.22f + 0.15f * v;
            r = hr; g = hg; b = hb;
            break;
        }
        case SY_ICON: {
            r = hr + 0.2f; g = hg; b = hb;
            float ad = du < 0 ? -du : du, av = dv < 0 ? -dv : dv;
            bool in = ad < 0.44f && av < 0.44f && sq(ad > 0.3f ? ad - 0.3f : 0) + sq(av > 0.3f ? av - 0.3f : 0) < 0.02f;
            L = 0.08f;
            switch (seed == 2000 ? 0 : var) {
            case 0:                              /* эмблема: плита с косой чертой и вырезами, серая в полоску */
                r = g = b = 0.9f;
                if (in) {
                    L = (j % 2) ? 0.62f : 0.78f;
                    float dd = (u - v) < 0 ? v - u : u - v;
                    if (dd < 0.11f && u > 0.18f && u < 0.84f) L = 0.08f;
                    if (u > 0.58f && v < 0.42f && (u - 0.58f) + (0.42f - v) > 0.14f && u < 0.86f && v > 0.14f) L = 0.08f;
                    if (u < 0.42f && v > 0.58f && (0.42f - u) + (v - 0.58f) > 0.14f && u > 0.14f && v < 0.86f) L = 0.08f;
                }
                break;
            case 1: { float dd = ad + av * 0.8f; if (dd < 0.42f) L = 0.4f + 0.6f * (1 - dd / 0.42f) * (du < 0 ? 1 : 0.7f); r = 0.75f; g = 0.45f; b = 1; break; }
            case 2: { float f = 0.42f - av * (dv < 0 ? 0.6f : 0.2f) - sq(du) * 3; if (f > 0 && v > 0.12f) L = 0.3f + f * 1.6f; r = 1; g = 0.45f + 0.5f * v; b = 0.15f; break; }
            case 3: {                            /* череп */
                r = g = b = 0.95f;
                if (sq(du) + sq(dv + 0.08f) < 0.11f || (ad < 0.16f && dv > 0.1f && dv < 0.32f)) L = 0.85f;
                if (sq(ad - 0.13f) + sq(dv + 0.06f) < 0.006f) L = 0.05f;
                if (dv > 0.2f && dv < 0.3f && (int)(u * 9) % 2) L = 0.2f;
                break;
            }
            case 4: { float s = ad + av; if (s < 0.25f || (ad < 0.07f && av < 0.42f) || (av < 0.07f && ad < 0.42f)) L = 0.95f - s; r = 1; g = 0.85f; b = 0.25f; break; }
            default: if (in) L = 0.35f + 0.5f * (1 - v); if (sq(ad - 0.16f) + sq(dv + 0.05f) < 0.01f) L = 0.05f; break;
            }
            break;
        }
        case SY_MEME: {                          /* кричащее лицо, ч/б */
            r = g = b = 1;
            float ad = du < 0 ? -du : du;
            L = 0.18f + 0.08f * (float)((i * 7 + j * 13) % 5) / 5.0f;
            float hd = sq(du / 0.42f) + sq((dv + 0.02f) / 0.5f);
            if (hd < 1) L = 0.45f + 0.35f * (1 - hd) - 0.25f * (du > 0 ? du : 0);
            if (sq((ad - 0.17f) / 0.09f) + sq((dv + 0.2f) / 0.05f) < 1) L = 0.06f;
            float md = sq(du / 0.26f) + sq((dv - 0.17f) / 0.24f);
            if (md < 1) {
                L = 0.03f;
                bool tooth = (int)((du + 0.3f) * 30) % 2 == 0;
                if (tooth && (dv - 0.17f < -0.15f || dv - 0.17f > 0.17f)) L = 0.95f;
            }
            if (hd >= 1 && hd < 1.12f) L = 0.75f;
            break;
        }
        default: {                               /* «фото»: небо, солнце, холмы */
            float hill = 0.62f + 0.12f * (float)((int)(u * 5 + seed) % 3) / 3.0f - 0.08f * du;
            if (v > hill) { L = 0.35f + 0.2f * (1 - v); r = 0.3f; g = 0.8f; b = 0.3f; }
            else { L = 0.45f + 0.4f * v; r = 0.45f; g = 0.65f; b = 1; }
            if (sq(u - 0.72f) + sq(v - 0.28f) < 0.012f) { L = 1; r = 1; g = 0.9f; b = 0.4f; }
            break;
        }
        }
        if (L < 0) L = 0;
        if (L > 1) L = 1;
        ImgCell *c = &out[j * w + i];
        int k = (int)(L * (float)(sizeof ramp - 2) + 0.5f);
        c->ch = ramp[k];
        for (int q = 0; q < 3; q++) {
            float cc = q == 0 ? r : q == 1 ? g : b;
            float f = (0.35f + 0.65f * L) * cc * 255, bk = L * 0.45f * cc * 255;
            c->fg[q] = (uint8_t)(f > 255 ? 255 : f);
            c->bg[q] = (uint8_t)(bk > 255 ? 255 : bk);
        }
    }
}

/* Картинки: настоящие из img.c (CDN Discord -> ASCII), в демо — синтетические. false — ещё качается или
 * нет картинки: рисуется заглушка (серый круг без букв). */
/* №109/111 (совет безопасника): беседа-запрос или спам — до «Принять» ничего не грузить с CDN (иначе отправитель
 * узнаёт по запросу, что сообщение открыли): превью, просмотр, текстовые вложения и аватары — заглушки. */
static bool dm_held(const Channel *c) { return c && !c->guild_id && (c->dmflags & (DMF_REQUEST | DMF_SPAM)); }
static bool img_hold(void) { return dm_held(chan_find(S.open_ch)); }
static bool avatar_hold(u64 uid) {
    if (!uid || uid == S.me_id) return false;
    for (int i = 0; i < S.nchans; i++) {
        const Channel *c = &S.chans[i];
        if (!dm_held(c)) continue;
        for (int k = 0; k < c->nrecip; k++) if (c->recipients[k] == uid) return true;
    }
    return false;
}
static u64 av_ovr_id;                            /* пользователь не из store (вход по QR): хэш аватара отдельно */
static char av_ovr_hash[40];
static bool pic_avatar(u64 uid, int w, int h, bool round, u32 under, ImgCell *out) {
    uint8_t cb[3] = { (uint8_t)(under >> 16), (uint8_t)(under >> 8), (uint8_t)under };
    if (avatar_hold(uid)) return false;          /* запрос/спам: аватар — заглушка */
    if (O.demo) { synth(uid, SY_AVATAR, w, h, out); if (round) img_round(out, w, h, cb); return true; }
    const char *hash = uid && uid == av_ovr_id ? av_ovr_hash : user_avatar(uid);
    return round ? img_avatar_round(uid, hash, w, h, cb, out) : img_avatar(uid, hash, w, h, out);
}
static bool pic_icon(const Guild *g, int w, int h, ImgCell *out) {
    if (!g || !g->icon[0]) return false;
    if (O.demo) { synth(g->id, SY_ICON, w, h, out); return true; }
    return img_icon(g->id, g->icon, w, h, out);
}
static bool pic_attach(const char *url, int maxw, int maxh, ImgCell *out, int *ow, int *oh) {
    if (O.demo) {
        bool meme = strstr(url, "meme") != NULL;
        int w = meme ? maxw * 3 / 4 : maxw / 3, hh = meme ? maxh : maxh / 3;
        if (w < 2) w = 2;
        if (hh < 1) hh = 1;
        u64 seed = 0;
        for (const char *p = url; *p; p++) seed = seed * 131 + (unsigned char)*p;
        synth(seed, meme ? SY_MEME : strstr(url, "face") ? SY_AVATAR : SY_PHOTO, w, hh, out);
        *ow = w; *oh = hh;
        return true;
    }
    return img_attachment(url, 0, 0, maxw, maxh, out, ow, oh);
}

/* Невидимые модификаторы эмодзи (ZWJ, вариантные селекторы, тон кожи, теги флагов) терминалы
 * считают по-разному — на экран они не выводятся, чтобы сетка не съезжала. Текст в Discord не меняется. */
static bool display_skip(u32 c) {
    return c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F) ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F || c == 0x061C;   /* С-1: BiDi не выводится */
}
/* Ширина символа на экране; 0 — не выводится (управляющие, модификаторы, нулевой ширины). */
static int shown_w(u32 c) {
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) || display_skip(c)) return 0;
    int w = cp_width(c);
    return w > 0 ? w : 0;
}
static int vwn(const char *s, size_t n) {
    int w = 0;
    if (!s) return 0;
    const char *e = n == (size_t)-1 ? s + strlen(s) : s + n;
    while (s < e && *s) { u32 c; s += utf8_dec(s, &c); w += shown_w(c); }
    return w;
}
static int vw(const char *s) { return vwn(s, (size_t)-1); }

static void grid_ensure(int w, int h) {
    /* сетка не уменьшается при смене размера (меньше дроблений кучи); растёт — по точному размеру */
    static size_t cap;
    size_t need = (size_t)w * (size_t)h;
    if (need > cap || !U.cell) {
        free(U.cell);
        U.cell = xmalloc(need * sizeof(Cell));
        cap = need;
    }
    U.gw = w;
    U.gh = h;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) U.cell[i] = (Cell){ ' ', C_TEXT, C_FEED };
}
/* Символ шириной cw (1 или 2) цветом fg с атрибутами a; фон клетки сохраняется. */
static void g_put(int x, int y, u32 cp, int cw, u32 fg, int a) {
    if (y < 0 || y >= U.gh || x < 0 || x + cw > U.gw || cw < 1) return;
    if (!GC(x, y).cp && x > 0) GC(x - 1, y).cp = ' ';
    int last = x + cw - 1;
    if (last + 1 < U.gw && !GC(last + 1, y).cp) GC(last + 1, y).cp = ' ';
    GC(x, y).cp = cp;
    GC(x, y).fg = RGB_OF(fg) | (u32)a << 24;
    if (cw == 2) { GC(x + 1, y).cp = 0; GC(x + 1, y).fg = GC(x, y).fg; GC(x + 1, y).bg = GC(x, y).bg; }
}
static void g_ch(int x, int y, char c, u32 fg, int a) { g_put(x, y, (u32)(unsigned char)c, 1, fg, a); }
/* Прямоугольник фона (пробелы). */
static void g_rect(int x, int y, int w, int h, u32 bg) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) {
        if (i < 0 || j < 0 || i >= U.gw || j >= U.gh) continue;
        if (!GC(i, j).cp && i > 0) GC(i - 1, j).cp = ' ';
        if (i + 1 < U.gw && !GC(i + 1, j).cp && i + 1 >= x + w) GC(i + 1, j).cp = ' ';
        GC(i, j) = (Cell){ ' ', C_TEXT, bg };
    }
}
/* Только фон, символы остаются. */
static void g_bg(int x, int y, int w, u32 bg) {
    if (y < 0 || y >= U.gh) return;
    for (int i = x; i < x + w && i < U.gw; i++) if (i >= 0) GC(i, y).bg = bg;
}
static void g_fgset(int x, int y, int w, u32 fg, int a) {
    if (y < 0 || y >= U.gh) return;
    for (int i = x; i < x + w && i < U.gw; i++) if (i >= 0) GC(i, y).fg = RGB_OF(fg) | (u32)a << 24;
}
/* Не больше n байт строки s и не шире w клеток; вернуть занятые клетки. */
static int g_textn(int x, int y, const char *s, size_t n, int w, u32 fg, int a) {
    int used = 0;
    if (!s) return 0;
    const char *e = n == (size_t)-1 ? s + strlen(s) : s + n;
    while (s < e && *s && used < w) {
        u32 c;
        int k = utf8_dec(s, &c);
        int cw = shown_w(c);
        if (used + cw > w) break;
        s += k;
        if (!cw) continue;
        g_put(x + used, y, c, cw, fg, a);
        used += cw;
    }
    return used;
}
static int g_text(int x, int y, const char *s, int w, u32 fg, int a) { return g_textn(x, y, s, (size_t)-1, w, fg, a); }
/* Текст не шире w: не влез — сокращение по целым словам с «...» (одно длинное слово — по символам).
 * words = false — по символам (плейсхолдер «Написать #кан...»: важен конец, а не целые слова). */
static int g_texte_w(int x, int y, const char *s, int w, u32 fg, int a, bool words);
static int g_texte(int x, int y, const char *s, int w, u32 fg, int a) { return g_texte_w(x, y, s, w, fg, a, true); }
static int g_texte_w(int x, int y, const char *s, int w, u32 fg, int a, bool words) {
    if (!s || w <= 0) return 0;
    if (vw(s) <= w || w <= 4) return g_text(x, y, s, w, fg, a);
    size_t cut = 0, sp = 0;
    int used = 0;
    for (const char *p = s; *p; ) {
        u32 c;
        int k = utf8_dec(p, &c), cw = shown_w(c);
        if (used + cw > w - 3) break;
        if (c == ' ' && p > s) sp = (size_t)(p - s);
        used += cw;
        p += k;
        cut = (size_t)(p - s);
    }
    size_t n = words && sp && sp * 2 >= cut ? sp : cut;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '/' || s[n - 1] == ',' || s[n - 1] == '-')) n--;
    int u = g_textn(x, y, s, n, w - 3, fg, a);
    return u + g_text(x + u, y, "...", 3, fg, a);
}
/* Текст справа: последняя клетка — x_end-1. Вернуть x начала. */
static int g_rtext(int x_end, int y, const char *s, int w, u32 fg, int a) {
    int sw = vw(s);
    if (sw > w) sw = w;
    g_text(x_end - sw, y, s, sw, fg, a);
    return x_end - sw;
}
/* По центру отрезка [x, x+w). */
static int g_ctext(int x, int y, int w, const char *s, u32 fg, int a) {
    int sw = vw(s);
    if (sw > w) sw = w;
    int x0 = x + (w - sw) / 2;
    g_text(x0, y, s, sw, fg, a);
    return x0;
}
/* Затемнить область (подложка модального окна, как в Discord). */
static u32 mix(u32 a, u32 b, int pb) {          /* pb из 256 — доля b */
    u32 r = 0;
    for (int s = 0; s < 24; s += 8) {
        int ca = (int)(a >> s & 0xFF), cb = (int)(b >> s & 0xFF);
        r |= (u32)((ca * (256 - pb) + cb * pb) >> 8) << s;
    }
    return r;
}
static void g_dim(int x, int y, int w, int h) {
    for (int j = y; j < y + h && j < U.gh; j++) for (int i = x; i < x + w && i < U.gw; i++) {
        if (i < 0 || j < 0) continue;
        GC(i, j).bg = mix(GC(i, j).bg, 0, 150);
        GC(i, j).fg = mix(RGB_OF(GC(i, j).fg), 0, 150) | (GC(i, j).fg & 0xFF000000u);
    }
}
static void sgr_out(Buf *b, int a, u32 fg, u32 bg) {
    buf_puts(b, "\x1b[0");
    if (a & AT_B) buf_puts(b, ";1");
    if (a & AT_U) buf_puts(b, ";4");
    if (a & AT_I) buf_puts(b, ";7");
    if (a & AT_IT) buf_puts(b, ";3");
    if (a & AT_ST) buf_puts(b, ";9");
    buf_printf(b, ";38;2;%u;%u;%u;48;2;%u;%u;%um", fg >> 16 & 0xFF, fg >> 8 & 0xFF, fg & 0xFF, bg >> 16 & 0xFF, bg >> 8 & 0xFF, bg & 0xFF);
}
/* Строка сетки в поток VT: lim клеток (последняя строка — W-1, чтобы терминал не прокрутился). */
static void g_line_out_from(Buf *b, int y, int x0, int lim) {
    int cur = -1;
    u32 cfg = 0, cbg = 0;
    for (int x = x0; x < lim; x++) {
        Cell c = GC(x, y);
        if (!c.cp) continue;
        int a = AT_OF(c.fg) & (AT_B | AT_U | AT_I | AT_IT | AT_ST);
        u32 fg = RGB_OF(c.fg);
        bool wide = x + 1 < U.gw && !GC(x + 1, y).cp;
        if (a != cur || fg != cfg || c.bg != cbg) { sgr_out(b, a, fg, c.bg); cur = a; cfg = fg; cbg = c.bg; }
        if (wide && x + 1 >= lim) { buf_put(b, " ", 1); continue; }
        char e[4];
        int k = utf8_enc(c.cp, e);
        buf_put(b, e, (size_t)k);
    }
    buf_puts(b, "\x1b[0m");
}
static void g_line_out(Buf *b, int y, int lim) { g_line_out_from(b, y, 0, lim); }

/* ---- клик-области ---- */
static void hit_add(int x, int y, int w, int h, int act, int sub, int arg, int hv) {
    if (U.nhit >= MAX_HITS || w <= 0 || h <= 0) return;
    U.hit[U.nhit++] = (Hit){ (short)x, (short)y, (short)w, (short)h, (u8)act, (u8)sub, (u8)hv, 0, arg };
}
static bool hit_in(const Hit *h, int x, int y) { return x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h; }
static bool hit_same(const Hit *a, const Hit *b) { return a->act == b->act && a->sub == b->sub && a->arg == b->arg; }
/* Кликабельное под клеткой: последнее добавленное сверху; фон карточки закрывает то, что под ним. */
static const Hit *hit_item(int x, int y) {
    for (int i = U.nhit - 1; i >= 0; i--) {
        const Hit *h = &U.hit[i];
        if (!hit_in(h, x, y)) continue;
        if (h->act >= A_GUILD) return h;
        if (h->act == A_BLOCK) return NULL;
    }
    return NULL;
}
static int hit_area(int x, int y) {
    for (int i = U.nhit - 1; i >= 0; i--) {
        const Hit *h = &U.hit[i];
        if (hit_in(h, x, y) && h->act != A_NONE && h->act <= A_BLOCK) return h->act;
    }
    return A_NONE;
}
/* Наведение: всё кликабельное с тем же действием (например, все строки сообщения) подсвечивается.
 * Без наведения (терминал без мыши) интерфейс полностью тот же. */
static void g_pill(int x, int y, int w, u32 bg);
static int g_text(int x, int y, const char *s, int w, u32 fg, int a);
static void date_full(i64 ts, char *out, size_t n);
static void hover_paint(void) {
    U.hov_on = false;
    if (U.hx < 0 || U.hy < 0) return;
    const Hit *h = hit_item(U.hx, U.hy);
    if (!h || h->act == A_INPUT) return;
    U.hov = *h;
    U.hov_on = true;
    for (int i = 0; i < U.nhit; i++) {
        const Hit *q = &U.hit[i];
        if (!hit_same(q, &U.hov)) continue;
        /* плашка: цвет наведения от фона середины (панель -> C_HOV, цветная кнопка -> светлее) */
        u32 inner = q->x + q->w / 2 < U.gw && q->y < U.gh ? GC(q->x + q->w / 2, q->y).bg : C_FEED;
        u32 pc = inner == C_SIDE || inner == C_FEED || inner == C_PANEL || inner == C_RAIL || inner == C_MENU ? C_HOV :
                 inner == C_SEL ? C_SEL : mix(inner, C_WHITE, 40);
        for (int y = q->y; y < q->y + q->h && y < U.gh; y++)
            for (int x = q->x; x < q->x + q->w && x < U.gw; x++) {
                if (x < 0 || y < 0) continue;
                Cell *c = &GC(x, y);
                int a = AT_OF(c->fg);
                u32 fg = RGB_OF(c->fg);
                switch (q->hv) {
                case HV_ACC: c->bg = C_ACC; fg = C_WHITE; break;
                case HV_RED: c->bg = C_DANGER; fg = C_WHITE; break;
                case HV_MSG: if (c->bg == C_MENT) c->bg = 0x45412fu; else if (c->bg == C_FEED) c->bg = C_MSGHOV; break;
                case HV_UL: a |= AT_U; if (fg == C_MUTED) fg = C_WHITE; break;
                case HV_PILL:
                    if (q->w >= 3 && (x == q->x || x == q->x + q->w - 1)) {
                        if (c->cp == ' ' || c->cp == '(' || c->cp == ')') { c->cp = x == q->x ? '(' : ')'; fg = pc; a |= AT_B; }
                    } else { c->bg = pc; if (fg == C_MUTED) fg = C_TEXT; }
                    break;
                case HV_ICON:
                    if (c->bg == C_RAIL) { if (c->cp != ' ') fg = C_WHITE; }
                    else { c->bg = mix(c->bg, C_WHITE, 40); fg = C_WHITE; }
                    break;
                default: if (c->bg != C_SEL) c->bg = C_HOV; if (fg == C_MUTED) fg = C_TEXT; break;
                }
                c->fg = fg | (u32)a << 24;
            }
    }
    /* подсказка словом над значком (или под ним у верхнего края) */
    if (U.hov.tip && U.hov.tip < sizeof TIPS / sizeof *TIPS) {
        const char *t = tr(TIPS[U.hov.tip]);
        char dbuf[96];
        if (U.hov.tip == T_DATE && U.hov.arg >= 0 && U.hov.arg < S.nmsgs) { date_full(S.msgs[U.hov.arg].ts, dbuf, sizeof dbuf); t = dbuf; }
        int w = vw(t) + 4, y = U.hov.y > 0 ? U.hov.y - 1 : U.hov.y + U.hov.h;
        int x = U.hov.x + U.hov.w / 2 - w / 2;
        if (x + w > U.gw) x = U.gw - w;
        if (x < 0) x = 0;
        if (y >= 0 && y < U.gh) { g_pill(x, y, w, C_MENU); g_text(x + 2, y, t, w - 4, C_WHITE, AT_B); }
    }
}

/* ======================= мелочи оформления ======================= */
/* Служебные строки из других модулей (статус сети, уведомления) — к ASCII-оформлению:
 * многоточие -> "...", длинное и среднее тире -> "-". */
static void deco_ascii(const char *s, char *out, size_t n) {
    size_t o = 0;
    while (s && *s && o + 4 < n) {
        u32 c;
        int k = utf8_dec(s, &c);
        if (c == 0x2026) { memcpy(out + o, "...", 3); o += 3; }
        else if (c == 0x2014 || c == 0x2013) out[o++] = '-';
        else { memcpy(out + o, s, (size_t)k); o += (size_t)k; }
        s += k;
    }
    out[o] = 0;
}
/* Значок сервера: первые буквы слов (не шире 3 клеток); одно слово — две первые буквы. */
static void abbr(const char *s, char *out, size_t n) {
    size_t o = 0;
    int cells = 0, words = 0;
    bool start = true;
    const char *first = NULL;
    for (const char *p = s; p && *p; ) {
        u32 c;
        int k = utf8_dec(p, &c);
        if (c == ' ' || c == '-' || c == '_' || c == '.') { start = true; p += k; continue; }
        int w = shown_w(c);
        if (start && w) {
            if (!first) first = p;
            if (cells + w <= 3 && o + (size_t)k + 1 < n) { memcpy(out + o, p, (size_t)k); o += (size_t)k; cells += w; }
            words++;
            start = false;
        }
        p += k;
    }
    out[o] = 0;
    if (words == 1 && first) {
        u32 c, c2;
        const char *p = first + utf8_dec(first, &c);
        int k2 = *p ? utf8_dec(p, &c2) : 0;
        if (k2 && c2 != ' ' && shown_w(c2) && cells + shown_w(c2) <= 3 && o + (size_t)k2 + 1 < n) {
            memcpy(out + o, p, (size_t)k2);
            o += (size_t)k2;
            out[o] = 0;
        }
    }
    if (!o) snprintf(out, n, "?");
}
static const char *plural(int n, const char *one, const char *few, const char *many) {
    int a = n % 100, b = n % 10;
    if (a >= 11 && a <= 14) return many;
    return b == 1 ? one : b >= 2 && b <= 4 ? few : many;
}
static bool is_voice(const Channel *c) { return c->type == CH_VOICE || c->type == CH_STAGE; }
/* Значок канала: # текст, <) голос (динамик), @ ЛС, & группа. */
static const char *chan_mark(const Channel *c) {
    if (is_voice(c)) return "<)";
    if (c->type == CH_FORUM) return "[=";
    if (c->type == CH_MEDIA) return "[^";
    if (c->guild_id) return "#";
    return c->type == CH_GROUP ? "&" : "@";
}

/* ---- скругления, как в Discord: концы плашек и углы кругов — ASCII-символ цветом фигуры на фоне под ней ---- */
static u32 bg_at(int x, int y) { return x >= 0 && y >= 0 && x < U.gw && y < U.gh ? GC(x, y).bg : C_FEED; }
static void g_cap(int x, int y, char ch, u32 col, u32 under) {
    if (x < 0 || y < 0 || x >= U.gw || y >= U.gh) return;
    g_rect(x, y, 1, 1, under);
    g_ch(x, y, ch, col, AT_B);
}
/* Плашка в одну строку со скруглёнными концами '(' ')'. */
static void g_pill(int x, int y, int w, u32 bg) {
    if (w < 3) { g_rect(x, y, w, 1, bg); return; }
    u32 ul = bg_at(x, y), ur = bg_at(x + w - 1, y);
    g_rect(x + 1, y, w - 2, 1, bg);
    g_cap(x, y, '(', bg, ul);
    g_cap(x + w - 1, y, ')', bg, ur);
}
/* Углы строки r круга w x h (h >= 2): '/' '\' цветом круга на фоне under. */
static void g_round_corners(int x, int y, int w, int h, int r, u32 under) {
    if (h < 2 || (r != 0 && r != h - 1)) return;
    for (int side = 0; side < 2; side++) {
        int cx = side ? x + w - 1 : x;
        g_cap(cx, y, (r == 0) == (side == 0) ? '/' : '\\', bg_at(cx, y), under);
    }
}
/* Строка r круга w x h (w = 2h) цвета bg с подписью t по центру средней строки. */
static void g_circle_row(int x, int y, int w, int h, int r, u32 bg, const char *t, u32 fg) {
    u32 under = bg_at(x, y);
    g_rect(x, y, w, 1, bg);
    if (t && r == (h - 1) / 2) g_ctext(x, y, w, t, fg, AT_B);
    g_round_corners(x, y, w, h, r, under);
}
/* Строка r готовых клеток картинки (img.c): ch == 0 — прозрачно (фон колонки). */
static void g_imgrow(int x, int y, int w, int r, const ImgCell *c) {
    for (int i = 0; i < w; i++) {
        const ImgCell *q = &c[r * w + i];
        if (!q->ch || x + i < 0 || x + i >= U.gw || y < 0 || y >= U.gh) continue;
        GC(x + i, y).bg = rgb3(q->bg);
        g_put(x + i, y, q->ch >= 0x20 && q->ch < 0x7F ? (u32)(unsigned char)q->ch : ' ', 1, rgb3(q->fg), 0);
    }
}
static u32 av_color(u64 uid) {
    static const u32 pal[] = { 0x5865f2u, 0x3ba55cu, 0xfaa61au, 0xed4245u, 0xeb459eu, 0x747f8du };
    return pal[(uid >> 22 ^ uid) % (sizeof pal / sizeof *pal)];
}
/* Круглый аватар w x h (4x2 в ленте и панели пользователя, 6x3 на экране голоса), строка r:
 * картинка (img.c, в демо — синтетическая) с круглой маской; пока она качается — серый круг без букв. */
static void g_avatar(int x, int y, int w, int h, int r, u64 uid, const char *name) {
    ImgCell av[128];
    (void)name;
    if (w * h > 128 || r < 0 || r >= h) return;
    if (pic_avatar(uid, w, h, true, bg_at(x, y), av)) { g_imgrow(x, y, w, r, av); return; }
    g_circle_row(x, y, w, h, r, C_PH, NULL, C_WHITE);
}
/* Аватар в ленте: то же, плюс строка в полосу мелкой сетки (в окне — картинка в 16 раз детальнее). */
static void g_feedav(int x, int y, int r, u64 uid) {
    g_avatar(x, y, 4, 2, r, uid, NULL);
    strip_part(G_FEED, x, y, 4, 2, r, PK_AVR, uid);
}
/* Круглый аватар в списках (участники, ЛС): 4 клетки "(" + 2 + ")"; в окне — 2x1 клетки мелкой сеткой
 * (8x4 мелких символа) в слое группы grp, без скобок (круг рисует сама картинка). */
static void g_listav(int x, int y, u64 uid, const char *name, int grp) {
    u32 under = bg_at(x, y), cl, cr;
    ImgCell av[2];
    (void)name;
    if (pic_avatar(uid, 2, 1, false, under, av)) {
        g_imgrow(x + 1, y, 2, 0, av);
        cl = rgb3(av[0].bg);
        cr = rgb3(av[1].bg);
        if (fine_on) { strip_part(grp, x + 1, y, 2, 1, 0, PK_AVR, uid); return; }
    } else {
        cl = cr = C_PH;
        g_rect(x + 1, y, 2, 1, cl);
    }
    g_cap(x, y, '(', cl, under);
    g_cap(x + 3, y, ')', cr, under);
}

/* Тонкая линия-рамка по низу строки y: пустые клетки [x, x+w) -> '_' (подписи строки её прерывают). */
static void g_uline(int x, int y, int w) {
    if (y < 0 || y >= U.gh) return;
    for (int i = x; i < x + w && i < U.gw; i++) {
        if (i < 0 || GC(i, y).cp != ' ') continue;
        u32 bg = GC(i, y).bg;                    /* только по фону колонки: плашки (поиск, кнопки) не трогаем */
        if (bg != C_FEED && bg != C_SIDE && bg != C_PANEL) continue;
        /* одиночный пробел между словами — часть подписи, его не трогаем */
        bool l = i > 0 && GC(i - 1, y).cp != ' ' && GC(i - 1, y).cp != '_', r = i + 1 < U.gw && GC(i + 1, y).cp != ' ';
        if (l && r) continue;
        g_ch(i, y, '_', C_LINE, 0);
    }
}
/* Рамка ASCII в строку: '+' + '-'... + '+' (верх или низ коробки). */
static void g_hbox(int x, int y, int w) {
    if (w < 2) return;
    g_ch(x, y, '+', C_LINE, 0);
    for (int i = x + 1; i < x + w - 1; i++) g_ch(i, y, '-', C_LINE, 0);
    g_ch(x + w - 1, y, '+', C_LINE, 0);
}
/* Кнопка-плашка со скруглёнными концами; вернуть ширину. style: 0 вторичная, 1 главная, 2 опасная, 3 белая. */
static int g_button(int x, int y, const char *t, int maxw, int style, int id) {
    static const u32 bgs[] = { C_BTN2, C_ACC, C_DANGER, C_WHITE };
    u32 fg = style == 3 ? 0x060607u : C_WHITE;
    int w = vw(t) + 4;
    if (w > maxw) w = maxw;
    if (w < 5) return 0;
    g_pill(x, y, w, bgs[style & 3]);
    g_text(x + 2, y, t, w - 4, fg, AT_B);
    hit_add(x, y, w, 1, A_BTN, id, 0, HV_PILL);
    return w;
}
/* Значок-кнопка (пиктограмма из 2-3 символов) с подсказкой словом при наведении. */
static int g_icon_btn(int x, int y, const char *t, u32 fg, int id, int tip) {
    int w = g_text(x, y, t, 4, fg, AT_B);
    hit_add(x, y, w, 1, A_BTN, id, 0, HV_BG);
    if (U.nhit) U.hit[U.nhit - 1].tip = (u8)tip;
    return w;
}

/* Модальное окно посреди области (ax, ay, aw, ah): подложка затемнена, скруглённые углы, строки (первая —
 * заголовок), кнопки в ряд справа (или столбиком, если ряд не влезает). ytop >= 0 — верх окна. */
typedef struct { const char *t; int id, style; } CBtn;
static void g_card(int ax, int ay, int aw, int ah, int ytop, const char *const *ln, int n, const CBtn *b, int nb, u32 tfg, bool dim) {
    int iwid = 0, row = 0, bmax = 0;
    for (int i = 0; i < n; i++) { int x = vw(ln[i]); if (x > iwid) iwid = x; }
    for (int i = 0; i < nb; i++) { int x = vw(b[i].t) + 4; row += x + (i ? 1 : 0); if (x > bmax) bmax = x; }
    int inner = aw - 8;
    if (inner < 4) return;
    bool stack = nb > 1 && row > inner;
    int bw = stack ? bmax : row;
    if (bw > iwid) iwid = bw;
    if (iwid > inner) iwid = inner;
    int nbr = nb ? (stack ? nb : 1) : 0;
    bool gap = n && nb;
    int bh = 2 + n + nbr + (gap ? 1 : 0);
    if (bh > ah && gap) { gap = false; bh--; }
    while (bh > ah && n > 1) { n--; bh--; }
    if (bh > ah) return;
    int bwid = iwid + 6;
    int left = ax + (aw - bwid) / 2;
    int top = ytop >= 0 ? ytop : ay + (ah - bh) / 2;
    if (top + bh > ay + ah) top = ay + ah - bh;
    if (dim) g_dim(ax, ay, aw, ah);
    ovl_cut(left, top, bwid, bh);
    u32 ut = bg_at(left, top), ub = bg_at(left, top + bh - 1);
    g_rect(left, top, bwid, bh, C_FEED);
    g_round_corners(left, top, bwid, bh, 0, ut);
    g_round_corners(left, top + bh - 1, bwid, bh, bh - 1, ub);
    hit_add(left, top, bwid, bh, A_BLOCK, 0, 0, 0);
    int y = top + 1;
    for (int i = 0; i < n; i++, y++) g_text(left + 3, y, ln[i], iwid, i == 0 ? tfg : C_TEXT, i == 0 ? AT_B : 0);
    if (gap) y++;
    if (stack) {
        for (int i = 0; i < nb; i++, y++) g_button(left + 3, y, b[i].t, iwid, b[i].style, b[i].id);
    } else {
        int x = left + 3 + iwid - row;           /* кнопки прижаты вправо, как в Discord */
        for (int i = 0; i < nb; i++) x += g_button(x, y, b[i].t, iwid, b[i].style, b[i].id) + 1;
    }
}

/* ======================= лента сообщений ======================= */
static bool mentions_me(const Message *m) {
    if (m->author_id != S.me_id && msg_alert(m)) return true;   /* №119: слово-тревога — как упоминание */
    if (!m->content || !S.me_name || !*S.me_name || m->author_id == S.me_id) return false;
    const char *p = m->content;
    size_t nl = strlen(S.me_name);
    while ((p = strchr(p, '@')) != NULL) { if (!strncmp(p + 1, S.me_name, nl)) return true; p++; }
    return false;
}
/* Следующая строка переноса по словам с байта i (не дальше n), ширина width.
 * *e — конец строки; вернуть начало следующей. */
static size_t wrap1(const char *s, size_t i, size_t n, int width, size_t *e) {
    int used = 0;
    size_t sp = (size_t)-1, j = i;
    while (j < n && s[j] != '\n') {
        u32 c;
        int k = utf8_dec(s + j, &c);
        int cw = shown_w(c);
        if (used + cw > width) break;
        if (c == ' ') sp = j;
        used += cw;
        j += (size_t)k;
    }
    if (j < n && s[j] != '\n' && sp != (size_t)-1 && sp > i) { *e = sp; return sp + 1; }
    if (j == i && j < n && s[j] != '\n') { u32 c; j += (size_t)utf8_dec(s + j, &c); }
    *e = j;
    if (j < n && s[j] == '\n') j++;
    return j;
}
/* Реакция-плашка: " 👍 2 "; своя эмодзи сервера — ":pepe:". */
static void react_label(const Reaction *r, char *out, size_t n) {
    const char *e = r->emoji;
    char shown[96];
    if (e[0] == ':') { const char *c2 = strchr(e + 1, ':'); snprintf(shown, sizeof shown, "%.*s", c2 ? (int)(c2 - e + 1) : 20, e); }
    else snprintf(shown, sizeof shown, "%s", e);
    snprintf(out, n, " %s %d ", shown, r->count);
}
static void ml_push(int *n, int mi, int kind, int off, int len) {
    if (*n == U.mlcap) {
        U.mlcap = U.mlcap ? U.mlcap * 2 : 256;
        U.ml = xrealloc(U.ml, (size_t)U.mlcap * sizeof(MLine));
    }
    U.ml[(*n)++] = (MLine){ mi, off, len, (u8)kind, 0 };
}
/* Место под превью картинки-вложения (до 48x16 клеток). */
static void preview_size(int tw, int *pw, int *ph) {
    *pw = tw < 32 ? tw : 32;                    /* клетка примерно 1:2: 32x16 — квадрат, картинка вписывается */
    if (*pw < 8) *pw = 8;
    *ph = *pw / 2;
    if (*ph > 15) *ph = 15;
    if (*ph < 3) *ph = 3;
}
/* Строка содержимого — вложение-картинка "[файл: имя.png] url"? Вернуть смещение url или -1. */
static int image_line(const char *s, size_t a, size_t e) {
    static const char pre[] = "[файл: ";
    if (a >= e || s[a] != ATT_MARK) return -1;   /* А1: «[файл: ...]», набранное руками, — просто текст */
    a++;
    size_t pl = sizeof pre - 1;
    if (e - a < pl || strncmp(s + a, pre, pl)) return -1;
    const char *rb = memchr(s + a, ']', e - a);
    if (!rb) return -1;
    static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp" };
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        size_t el = strlen(ext[i]);
        if ((size_t)(rb - (s + a)) >= pl + el && !strncmp(rb - el, ext[i], el)) return (int)(rb - s + 2 <= (long)e ? rb - s + 2 : (long)e);
    }
    return -1;
}
/* url вложения с байта off строки s (до пробела или конца строки). */
/* vis — строка видна на экране (отрисовка): только тогда просроченная ссылка ставится в очередь
 * обновления (ревью 9 К1: из разметки ленты — никогда, иначе лавина запросов). Не CDN Discord — пусто (А1). */
static void att_url(const char *s, size_t off, char *out, size_t n, bool vis) {
    size_t ul = 0;
    const char *url = s ? s + off : "";
    while (url[ul] && url[ul] != '\n' && url[ul] != ' ' && ul + 1 < n) { out[ul] = url[ul]; ul++; }
    out[ul] = 0;
    if (out[0] && !cdn_host_ok(out)) { out[0] = 0; return; }
#ifdef MC_PACKA
    if (out[0] && cdn_expired(out)) {
        if (vis) cdn_want(out);
        const char *f = cdn_fresh(out);
        if (f) snprintf(out, n, "%s", f);
    }
#else
    (void)vis;
#endif
}
/* Клетки превью вложения url (до pw x ph): живой режим — прямо из кэша img.c (до конца кадра), демо —
 * синтетика; последняя картинка кадра запоминается (строки одного превью рисуются подряд — без пересчёта). */
static unsigned frame_no;
static u32 fnv(const char *s, size_t n);
/* №65/67: свой файл — спойлером и с описанием (alt); сбрасываются после отправки */
static bool file_spoil;
static char file_alt[1024];
static bool img_click;                           /* №71: картинки грузятся только по щелчку */
static u32 img_seen[32];                         /* открытые в просмотре (хэш адреса) — их превью показываем */
static unsigned img_seen_n;
static u32 fnv(const char *s, size_t n);
static bool img_allowed(const char *url) {
    if (!img_click) return true;
    u32 h = fnv(url, strlen(url));
    for (int i = 0; i < 32; i++) if (img_seen[i] == h && h) return true;
    return false;
}
static const ImgCell *att_cells(const char *url, int pw, int ph, int *ow, int *oh) {
    if (img_hold()) return NULL;                 /* запрос/спам: картинка — заглушка */
    if (!img_allowed(url)) return NULL;          /* №71: по щелчку */
    if (!O.demo) return img_attachment_peek(url, 0, 0, pw, ph, ow, oh);
    static ImgCell pc[32 * 16];
    static u32 key;
    static unsigned fr;
    static int kw, kh;
    u32 k = fnv(url, strlen(url)) ^ (u32)(pw * 131 + ph);
    if (k == key && fr == frame_no) { *ow = kw; *oh = kh; return pc; }
    if (pw * ph > (int)(sizeof pc / sizeof *pc) || !pic_attach(url, pw, ph, pc, ow, oh)) return NULL;
    key = k; fr = frame_no; kw = *ow; kh = *oh;
    return pc;
}
/* Сколько строк занимает превью: картинка готова — её высота, иначе место ph под заглушку. */
static int preview_rows(const char *s, size_t off, int pw, int ph) {
    char u2[2048];
    int ow = 0, oh = 0;
    att_url(s, off, u2, sizeof u2, false);
    if (att_cells(u2, pw, ph, &ow, &oh) && oh > 0 && oh <= ph) return oh;
    return ph;
}
/* ---- пакет Б: №38 выделение текста, №46 «Копировать» у блока кода, №61 просмотр картинки ---- */
/* Карта кадра: какой байт content сообщения нарисован в клетке ленты (-1 — не текст), сообщение строки.
 * Заполняется при рисовании (g_mdline, строки кода); по ней щелчок становится концом выделения. */
static int *colmap;
static int *rowmi;
static size_t cm_cap, rm_cap;
static void cm_frame(int W, int H) {
    size_t n = (size_t)W * (size_t)H;
    if (n > cm_cap) { free(colmap); colmap = xmalloc(n * sizeof *colmap); cm_cap = n; }
    if ((size_t)H > rm_cap) { free(rowmi); rowmi = xmalloc((size_t)H * sizeof *rowmi); rm_cap = (size_t)H; }
    memset(colmap, 0xff, n * sizeof *colmap);
    for (int y = 0; y < H; y++) rowmi[y] = -1;
}
static void cm_set(int x, int y, int cw, int mi, int p) {
    if (!colmap || y < 0 || y >= U.gh) return;
    for (int i = 0; i < cw; i++) if (x + i >= 0 && x + i < U.gw) colmap[y * U.gw + x + i] = p;
    rowmi[y] = mi;
}
/* Клетки строки правее текста — «конец строки» (щелчок туда выделяет до конца). */
static void cm_tail(int x0, int y, int x1, int mi, int p) {
    for (int x = x0; x < x1; x++) cm_set(x, y, 1, mi, p);
}
/* Выделение: концы — (сообщение, байт в content); a — где щёлкнули сначала, b — Shift+щелчок или протяжка. */
static struct { u64 a_msg, b_msg; int a_off, b_off; bool on; int lo_mi, lo_off, hi_mi, hi_off; } SEL;
#define C_SELTXT 0x2d4a87u                       /* фон выделенного текста */
static void sel_clear(void) { if (SEL.on || SEL.a_msg) U.dirty = true; SEL.on = false; SEL.a_msg = SEL.b_msg = 0; }
/* На кадр: концы по порядку ленты; сообщения не стало (другой канал) — выделение снято. */
static void sel_norm(void) {
    if (!SEL.on) return;
    int ia = -1, ib = -1;
    if (!msg_find(SEL.a_msg, &ia) || !msg_find(SEL.b_msg, &ib) || ia < 0 || ib < 0) { sel_clear(); return; }
    bool ab = ia < ib || (ia == ib && SEL.a_off <= SEL.b_off);
    SEL.lo_mi = ab ? ia : ib; SEL.lo_off = ab ? SEL.a_off : SEL.b_off;
    SEL.hi_mi = ab ? ib : ia; SEL.hi_off = ab ? SEL.b_off : SEL.a_off;
}
static bool sel_in(int mi, int p) {
    if (!SEL.on) return false;
    if (mi < SEL.lo_mi || mi > SEL.hi_mi) return false;
    if (mi == SEL.lo_mi && p < SEL.lo_off) return false;
    if (mi == SEL.hi_mi && p >= SEL.hi_off) return false;
    return true;
}
/* Щелчок (или протяжка) по ленте: ext — продолжить от начала, иначе — новое начало. */
static void sel_point(int x, int y, bool ext) {
    int p = colmap && x >= 0 && y >= 0 && x < U.gw && y < U.gh ? colmap[y * U.gw + x] : -1;
    int mi = p >= 0 ? rowmi[y] : -1;
    if (p < 0 || mi < 0 || mi >= S.nmsgs) { if (!ext) sel_clear(); return; }
    u64 id = S.msgs[mi].id;
    if (ext && SEL.a_msg) {
        SEL.b_msg = id; SEL.b_off = p;
        SEL.on = !(SEL.a_msg == SEL.b_msg && SEL.a_off == SEL.b_off);
    } else { SEL.a_msg = SEL.b_msg = id; SEL.a_off = SEL.b_off = p; SEL.on = false; }
    U.dirty = true;
}
static void copy_text(const char *s);
/* Кусок [a, e) текста сообщения в буфер; ревью 12 М5: нераскрытый спойлер (||...|| и вложение SPOILER_)
 * не копируется — вместо него «[спойлер]». */
static void sel_put(Buf *b, const Message *m, size_t a, size_t e) {
    const char *s = m->content;
    bool reveal = spoil_open(m->id);
    if (reveal) { buf_put(b, s + a, e - a); return; }
    for (size_t ls = a; ls < e; ) {
        size_t l0 = ls;
        while (l0 > 0 && s[l0 - 1] != '\n') l0--;        /* начало исходной строки */
        size_t le = ls;
        while (s[le] && s[le] != '\n') le++;
        size_t pe = le < e ? le : e;
        if (s[l0] == ATT_MARK && !strncmp(s + l0 + 1 + strlen(TR("[файл: ")), "SPOILER_", 8)) buf_puts(b, TR("[спойлер]"));
        else {
            MdLine ln;
            MdSpan sp[64];
            int k = md_line(s + l0, le - l0, &ln, sp, 64);
            bool in = false;
            for (size_t p = ls; p < pe; p++) {
                int si = -1;
                for (int i = 0; i < k; i++) if (p - l0 >= sp[i].off && p - l0 < (size_t)sp[i].off + sp[i].len) { si = i; break; }
                bool spl = si >= 0 && (sp[si].attr & MD_SPOIL);
                if (spl) { if (!in) buf_puts(b, TR("[спойлер]")); in = true; continue; }
                in = false;
                if (si < 0 && s[p] == '|') continue;     /* маркеры || спойлера */
                buf_put(b, s + p, 1);
            }
        }
        if (le < e) buf_put(b, "\n", 1);
        ls = le < e ? le + 1 : e;
    }
}
/* Ctrl+C: выделенное как есть (между сообщениями — перевод строки). */
static void sel_copy(void) {
    sel_norm();
    if (!SEL.on) return;
    Buf b = {0};
    for (int mi = SEL.lo_mi; mi <= SEL.hi_mi && mi < S.nmsgs; mi++) {
        const char *s = S.msgs[mi].content ? S.msgs[mi].content : "";
        size_t len = strlen(s), a = mi == SEL.lo_mi ? (size_t)SEL.lo_off : 0, e = mi == SEL.hi_mi ? (size_t)SEL.hi_off : len;
        if (a > len) a = len;
        if (e > len) e = len;
        if (mi > SEL.lo_mi) buf_puts(&b, "\n");
        if (e > a) sel_put(&b, &S.msgs[mi], a, e);
    }
    if (b.p && b.len) { copy_text(b.p); ui_notice(TR("выделенный текст скопирован")); }
    buf_free(&b);
}
/* Строка блока кода: как g_textn, плюс карта клеток и подсветка выделения. */
static int g_codeline(int x, int y, int mi, const char *s, size_t off, size_t len, int w, u32 fg) {
    int used = 0;
    size_t p = off, e = off + len;
    while (p < e && s[p] && used < w) {
        u32 c;
        int k = utf8_dec(s + p, &c), cw = shown_w(c);
        if (used + cw > w) break;
        if (cw) {
            if (sel_in(mi, (int)p)) g_bg(x + used, y, cw, C_SELTXT);
            g_put(x + used, y, c, cw, fg, 0);
            cm_set(x + used, y, cw, mi, (int)p);
            used += cw;
        }
        p += (size_t)k;
    }
    cm_tail(x + used, y, x + w, mi, (int)p);
    return used;
}
/* №46: «[копировать]» на первой строке блока кода — весь блок до закрывающей ``` (без оград). */
static struct { int mi, off; } cbt[16];
static int n_cbt;
static void code_copy(int k) {
    if (k < 0 || k >= n_cbt || cbt[k].mi < 0 || cbt[k].mi >= S.nmsgs) return;
    const char *s = S.msgs[cbt[k].mi].content;
    if (!s) return;
    /* номер блока для md_code_block (store): пары ``` по порядку, наш — тот, что охватывает off */
    const char *at = s + cbt[k].off, *p = s;
    int idx = 0, want = -1;
    for (;;) {
        const char *a = strstr(p, "```");
        if (!a) break;
        const char *z = strstr(a + 3, "```");
        if (!z) break;
        if (a < at && z >= at) { want = idx; break; }
        idx++;
        p = z + 3;
    }
    Buf b = {0};
    if (want >= 0 && md_code_block(s, want, &b) && b.p && b.len) {
        copy_text(b.p);
        ui_notice(TR("код скопирован"));
    }
    buf_free(&b);
}
/* №61: просмотр картинки на весь экран. Клетки — заново под размер окна (живой режим — кэш img.c только
 * на кадр: закрыли — ушло при следующем img_frame; демо — свой буфер, освобождается при закрытии). */
static struct { bool on; u64 msg; int off; ImgCell *buf; size_t cap; } IV;
static struct { int mi, off; } ivh[32];          /* картинки ленты под щелчком (на кадр) */
static int n_ivh;
#define C_IVBG 0x0d0e10u                         /* фон просмотра: почти чёрный, как в Discord */
static void iv_hit(int x, int y, int w, int mi, int off) {
    int k = n_ivh - 1;
    if (k < 0 || ivh[k].mi != mi || ivh[k].off != off) {
        if (n_ivh >= (int)(sizeof ivh / sizeof *ivh)) return;
        k = n_ivh++;
        ivh[k].mi = mi; ivh[k].off = off;
    }
    hit_add(x, y, w, 1, A_IMGV, 0, k, HV_MSG);
}
/* №101 аватар крупно: квадрат на всё окно (img.c — заново под размер), Esc или щелчок — закрыть */
static struct { u64 uid; ImgCell *buf; size_t cap; } AVB;
static void avb_close(void) { AVB.uid = 0; free(AVB.buf); AVB.buf = NULL; AVB.cap = 0; U.full = U.dirty = true; }
static void draw_avbig(int W, int H) {
    ovl_cut(0, 0, W, H);
    g_rect(0, 0, W, H, C_IVBG);
    hit_add(0, 0, W, H, A_BTN, B_IV_X, 0, HV_MSG);
    const char *nm = user_name(AVB.uid);
    char *sn = san(nm ? nm : "", false);
    g_texte(2, 0, sn, W - 8, C_WHITE, AT_B);
    free(sn);
    g_text(W - 4, 0, " x ", 3, C_TEXT, AT_B);
    int ah = H - 4 > IMG_MAX_H ? IMG_MAX_H : H - 4, aw = ah * 2;
    if (aw > W - 4) { aw = (W - 4) & ~1; ah = aw / 2; }
    if (aw > IMG_MAX_W) { aw = IMG_MAX_W; ah = aw / 2; }
    if (ah < 2) return;
    size_t need = (size_t)aw * (size_t)ah;
    if (need > AVB.cap) { free(AVB.buf); AVB.buf = xmalloc(need * sizeof *AVB.buf); AVB.cap = need; }
    int ax = (W - aw) / 2, ay = 2 + (H - 4 - ah) / 2;
    if (pic_avatar(AVB.uid, aw, ah, false, C_IVBG, AVB.buf)) for (int r = 0; r < ah; r++) g_imgrow(ax, ay + r, aw, r, AVB.buf);
    else g_ctext(0, H / 2, W, avatar_hold(AVB.uid) ? TR("аватар скрыт, пока запрос не принят") : TR("загрузка..."), C_MUTED, 0);
    g_ctext(0, H - 1, W, TR("Esc - закрыть"), C_MUTED, 0);
}
static void iv_close(void) {
    IV.on = false;
    free(IV.buf);
    IV.buf = NULL; IV.cap = 0;
    U.full = U.dirty = true;
}
static bool spoil_open(u64 id);
/* Картинки открытого канала по порядку ленты: всего; cur — номер открытой; n-я (target) -> tmi, toff. */
static int iv_list(int *cur, int target, int *tmi, int *toff) {
    int n = 0;
    *cur = -1;
    for (int mi = 0; mi < S.nmsgs; mi++) {
        const char *s = S.msgs[mi].content;
        if (!s) continue;
        size_t len = strlen(s);
        for (size_t at = 0; at < len; ) {
            size_t le = at;
            while (le < len && s[le] != '\n') le++;
            int url = image_line(s, at, le);
            bool sp = url >= 0 && !strncmp(s + at + 1 + strlen(TR("[файл: ")), "SPOILER_", 8) && !spoil_open(S.msgs[mi].id);
            if (url >= 0 && !sp) {
                if (S.msgs[mi].id == IV.msg && url == IV.off) *cur = n;
                if (n == target) { *tmi = mi; *toff = url; }
                n++;
            }
            at = le < len ? le + 1 : le;
        }
    }
    return n;
}
static void iv_open(int k) {
    if (k < 0 || k >= n_ivh || ivh[k].mi < 0 || ivh[k].mi >= S.nmsgs) return;
    IV.on = true;
    IV.msg = S.msgs[ivh[k].mi].id;
    IV.off = ivh[k].off;
    U.menu = false;
    sel_clear();
    U.full = U.dirty = true;
}
static void iv_step(int d) {
    int cur, tmi = -1, toff = 0;
    int n = iv_list(&cur, -1, &tmi, &toff);
    if (cur < 0 || cur + d < 0 || cur + d >= n) return;
    iv_list(&cur, cur + d, &tmi, &toff);
    if (tmi < 0) return;
    IV.msg = S.msgs[tmi].id;
    IV.off = toff;
    U.dirty = true;
}
/* Имя файла строки вложения (между «[файл: » и «]»). */
static void iv_name(const char *s, int off, char *out, size_t n) {
    size_t a = (size_t)off;
    while (a > 0 && s[a - 1] != '\n') a--;
    a += 1 + strlen(TR("[файл: "));
    const char *rb = strchr(s + a, ']');
    size_t l = rb ? (size_t)(rb - (s + a)) : 0;
    if (l >= n) l = n - 1;
    memcpy(out, s + a, l);
    out[l] = 0;
}
static void draw_iv(int W, int H) {
    int cur, tmi = -1, toff = 0;
    int n = iv_list(&cur, -1, &tmi, &toff);
    Message *m = msg_find(IV.msg, NULL);
    if (!m || !m->content || cur < 0) { iv_close(); return; }
    ovl_cut(0, 0, W, H);
    g_rect(0, 0, W, H, C_IVBG);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    hit_add(0, 1, W, H - 2, A_BTN, B_IV_X, 0, HV_MSG);   /* щелчок по фону — закрыть, как в Discord */
    /* верх: имя файла, «N из M», x */
    char nm[160], t[200];
    iv_name(m->content, IV.off, nm, sizeof nm);
    char *sn = san(nm, false);
    int u = g_texte(2, 0, sn, W - 24, C_WHITE, AT_B);
    free(sn);
    snprintf(t, sizeof t, TR("  %d из %d"), cur + 1, n);
    g_text(2 + u, 0, t, W - 8 - u, C_MUTED, 0);
    g_text(W - 4, 0, " x ", 3, C_TEXT, AT_B);
    hit_add(W - 4, 0, 3, 1, A_BTN, B_IV_X, 0, HV_RED);
    /* картинка по центру, под размер области */
    int ax = 7, ay = 2, aw = W - 14, ah = H - 5;
    if (aw > IMG_MAX_W) aw = IMG_MAX_W;
    if (ah > IMG_MAX_H) ah = IMG_MAX_H;
    if (aw >= 4 && ah >= 2) {
        char url[2048];
        att_url(m->content, (size_t)IV.off, url, sizeof url, true);
        const ImgCell *pc = NULL;
        int ow = 0, oh = 0;
        if (url[0] && img_click && !img_allowed(url)) img_seen[img_seen_n++ % 32] = fnv(url, strlen(url));   /* открыли — дальше и превью */
        if (!url[0] || img_hold()) ;
        else if (!O.demo) pc = img_attachment_peek(url, 0, 0, aw, ah, &ow, &oh);
        else {
            size_t need = (size_t)aw * (size_t)ah;
            if (need > IV.cap) { free(IV.buf); IV.buf = xmalloc(need * sizeof *IV.buf); IV.cap = need; }
            if (pic_attach(url, aw, ah, IV.buf, &ow, &oh)) pc = IV.buf;
        }
        if (pc && ow > 0 && oh > 0 && ow <= aw && oh <= ah) {
            int ix = ax + (aw - ow) / 2, iy = ay + (ah - oh) / 2;
            for (int r = 0; r < oh; r++) g_imgrow(ix, iy + r, ow, r, pc);
            hit_add(ix, iy, ow, oh, A_BLOCK, 0, 0, 0);   /* по самой картинке — не закрывать */
        } else g_ctext(0, ay + ah / 2, W, img_hold() ? TR("картинки скрыты, пока запрос не принят") : url[0] ? TR("загрузка...") : TR("картинка недоступна"), C_MUTED, 0);
    }
    /* соседние картинки канала: круглые кнопки по бокам */
    int my = H / 2 - 1;
    if (cur > 0) {
        for (int r = 0; r < 2; r++) g_circle_row(1, my + r, 4, 2, r, C_BTN2, "<", C_WHITE);
        hit_add(1, my, 4, 2, A_BTN, B_IV_PREV, 0, HV_ICON);
    }
    if (cur < n - 1) {
        for (int r = 0; r < 2; r++) g_circle_row(W - 5, my + r, 4, 2, r, C_BTN2, ">", C_WHITE);
        hit_add(W - 5, my, 4, 2, A_BTN, B_IV_NEXT, 0, HV_ICON);
    }
    g_ctext(0, H - 1, W, W >= 60 ? TR("Left/Right - соседние картинки   Esc - закрыть") : "Left/Right  Esc", C_MUTED, 0);
}

/* ---- №53 эмбеды карточкой и №41 опросы: раскладка строк (без сети и без картинок — только счёт строк) ---- */
/* Строка r текста str при ширине w (с переносом по словам, '\n' — новая строка): [*a, *e), *src — начало
 * исходной строки (для разметки). false — строк меньше. */
static bool ew_line(const char *str, int w, int r, size_t *src, size_t *a, size_t *e) {
    size_t n = strlen(str), ls = 0;
    int k = 0;
    if (w < 4) w = 4;
    for (;;) {
        size_t le = ls;
        while (le < n && str[le] != '\n') le++;
        size_t q = ls;
        for (;;) {
            size_t ee, nx = wrap1(str, q, le, w, &ee);
            if (k++ == r) { *src = ls; *a = q; *e = ee; return true; }
            if (nx <= q || nx >= le) break;
            q = nx;
        }
        if (le >= n) break;
        ls = le + 1;
    }
    return false;
}
static int ew_rows(const char *str, int w) {
    if (!str || !*str) return 0;
    size_t s0, a, e;
    int r = 0;
    while (ew_line(str, w, r, &s0, &a, &e)) r++;
    return r;
}
/* Картинка эмбеда — только вложение с CDN Discord (Б-2: не media.discordapp.net/external/ — это запрос
 * к стороннему серверу по выбору отправителя). */
static bool emb_img_ok(const char *u) {
    return u && *u && cdn_host_ok(u) && strstr(u, "/attachments/") && !strstr(u, "/external/");
}
enum { ER_PAD, ER_STR, ER_FIELD, ER_FROW, ER_IMG };
typedef struct { u8 kind, k; u16 r; u8 cnt; } ERow;   /* ER_STR: k — EM_*, r — строка; ER_FIELD: k — поле, r — строка;
                                                          ER_FROW: k — первое поле ряда, cnt — сколько, r — строка; ER_IMG: r */
/* Ширина карточки и текста в ней при ширине ленты tw. */
static int emb_w(int tw) { int ew = tw - 1; return ew > 64 ? 64 : ew; }
static void emb_imgsize(const Embed *e, int cw, int *pw, int *ph, const char **url) {
    const char *im = embed_str(e, EM_IMAGE), *th = embed_str(e, EM_THUMB);
    *url = emb_img_ok(im) ? im : emb_img_ok(th) ? th : NULL;
    *pw = *ph = 0;
    if (!*url) return;
    preview_size(*url == im ? cw : cw / 2, pw, ph);
    /* высота — без знания картинки (раскладка не делает запросов): не больше 10 строк, ширина вдвое */
    if (*ph > 10) *ph = 10;
    if (*pw > *ph * 2) *pw = *ph * 2;
}
/* Строки поля (имя, затем значение) в колонке ширины w. */
static int ef_rows(const Embed *e, int i, int w) {
    const char *nm, *val;
    bool inl;
    if (!embed_field(e, i, &nm, &val, &inl)) return 0;
    return ew_rows(nm, w) + ew_rows(val, w);
}
static int emb_layout(const Embed *e, int tw, ERow *out, int max) {
    int cw = emb_w(tw) - 3, n = 0;
    if (cw < 8) return 0;
#define EPUSH(K, k_, r_, c_) do { if (n < max) out[n++] = (ERow){ (u8)(K), (u8)(k_), (u16)(r_), (u8)(c_) }; } while (0)
    EPUSH(ER_PAD, 0, 0, 0);
    static const int order[] = { EM_PROVIDER, EM_AUTHOR, EM_TITLE, EM_DESC };
    for (size_t q = 0; q < sizeof order / sizeof *order; q++) {
        int rr = ew_rows(embed_str(e, order[q]), cw);
        if (order[q] == EM_TITLE && rr > 2) rr = 2;
        if (order[q] == EM_DESC && rr > 12) rr = 12;
        for (int r = 0; r < rr; r++) EPUSH(ER_STR, order[q], r, 0);
    }
    for (int i = 0; i < e->nf; ) {
        bool inl = (e->f_inline >> i & 1) != 0;
        int cnt = 1;
        if (inl && cw >= 48) while (cnt < 3 && i + cnt < e->nf && (e->f_inline >> (i + cnt) & 1)) cnt++;
        int colw = cnt > 1 ? (cw - 2 * (cnt - 1)) / cnt : cw, rows = 0;
        for (int j = 0; j < cnt; j++) { int fr = ef_rows(e, i + j, colw); if (fr > rows) rows = fr; }
        if (rows > 8) rows = 8;
        for (int r = 0; r < rows; r++) EPUSH(ER_FROW, i, r, cnt);
        i += cnt;
    }
    int pw, ph;
    const char *iu;
    emb_imgsize(e, cw, &pw, &ph, &iu);
    for (int r = 0; r < ph; r++) EPUSH(ER_IMG, 0, r, 0);
    int fr = ew_rows(embed_str(e, EM_FOOTER), cw);
    if (fr > 2) fr = 2;
    for (int r = 0; r < fr; r++) EPUSH(ER_STR, EM_FOOTER, r, 0);
    EPUSH(ER_PAD, 0, 0, 0);
#undef EPUSH
    return n;
}
static int comp_rows(const Message *m, int tw);
/* Опрос: вопрос, подпись, ответы, итог; ширина карточки. */
static int poll_w(int tw) { return tw - 1 > 60 ? 60 : tw - 1; }
static int poll_nrows(const Poll *p) { return p ? 2 + p->n + 2 : 0; }

/* День сообщения (местное время) — для разделителей дат. */
static int day_key(i64 ts) { struct tm t; plat_localtime(ts, &t); return t.tm_year * 400 + t.tm_yday; }
static void date_text(i64 ts, char *out, size_t n) {
    static const char *const mon[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                                       "сентября", "октября", "ноября", "декабря" };
    struct tm t;
    plat_localtime(ts, &t);
    snprintf(out, n, TR("%d %s %d г."), t.tm_mday, tr(mon[(t.tm_mon % 12 + 12) % 12]), t.tm_year + 1900);
}
/* Строки ленты с группировкой как в Discord: аватар, имя и время — у первого сообщения серии;
 * следующие сообщения того же автора в течение 7 минут — без заголовка. Ответ и новый день начинают серию. */
typedef struct Message_ Message_;
static int text_line(const char *s, size_t a, size_t e);
static int txt_rows(const Message *m, const char *s, size_t url_off, int tw);
static int build_mlines(int tw) {
    int n = 0, width = tw;
    if (width < 4) width = 4;
    int pw, ph;
    preview_size(width, &pw, &ph);
    bool new_done = false;
    for (int i = 0; i < S.nmsgs; i++) {
        const Message *m = &S.msgs[i], *p = i ? &S.msgs[i - 1] : NULL;
        bool hid = false;
        for (int q = 0; q < 8; q++) if (U.eph_hidden[q] == m->id && m->id) hid = true;
        if (hid) continue;                       /* «видно только вам» — скрыто пользователем */
        bool cont = p && !m->ref_id && p->author_id == m->author_id && m->ts >= p->ts && m->ts - p->ts < 7 * 60;
        if (!p || day_key(m->ts) != day_key(p->ts)) { ml_push(&n, i, L_DATE, 0, 0); cont = false; }
        else if (!cont) ml_push(&n, i, L_GAP, 0, 0);
        if (U.new_from && !new_done && m->id > U.new_from && m->author_id != S.me_id) { ml_push(&n, i, L_NEW, 0, 0); new_done = true; cont = false; }
        if (m->ref_id) ml_push(&n, i, L_REPLY, 0, 0);
        if (!cont) ml_push(&n, i, L_HEAD, 0, 0);
        if (m->forwarded) ml_push(&n, i, L_FWD, 0, 0);   /* «-> Переслано», текст оригинала — с полоской */
        if ((msg_filtered(m) || (m->author_id != S.me_id && ignored(m->author_id))) && !filt_open(m->id)) {   /* №120 фильтр, №108 игнор — одна строка */
            ml_push(&n, i, L_FILT, 0, 0);
            continue;
        }
        const char *s = m->content ? m->content : "";
        size_t len = strlen(s), at = 0;
        int lastw = 0, before = n;
        bool blk = false;                        /* внутри ``` ... ``` */
        while (at < len) {
            size_t le = at;
            while (le < len && s[le] != '\n') le++;  /* исходная строка [at, le) */
            /* ограда блока кода: строка с ``` и без второй ``` — сама не показывается (язык после неё — тоже) */
            if (le - at >= 3 && !strncmp(s + at, "```", 3)) {
                bool closed = false;
                for (size_t k = at + 3; k + 3 <= le; k++) if (!strncmp(s + k, "```", 3)) closed = true;
                if (!closed) { blk = !blk; at = le < len ? le + 1 : le; continue; }
            }
            u8 st = (u8)((blk ? ST_CODE : 0) | (m->forwarded ? ST_FWD : 0));
            size_t q = at;
            int ww = (blk ? width - 2 : width) - (m->forwarded ? 2 : 0);
            if (!blk && le > at && s[at] == '>' && (le - at == 1 || s[at + 1] == ' ')) {   /* > цитата */
                st |= ST_QUOTE;
                q = at + (le - at >= 2 ? 2 : 1);
                ww -= 2;
            }
            if (ww < 4) ww = 4;
            if (m->voice_secs && s[q] == ATT_MARK) { at = le < len ? le + 1 : le; continue; }   /* №69: файл — в плеере */
            int url = blk ? -1 : image_line(s, q, le);
            /* текстовый файл — блок превью (кроме спойлера: он остаётся строкой, содержимое не грузится) */
            int turl = blk || url >= 0 || (s[q] == ATT_MARK && le - q > 12 && !strncmp(s + q + 1 + strlen(TR("[файл: ")), "SPOILER_", 8)) ? -1 : text_line(s, q, le);
            if (turl >= 0) {
                int tr = txt_rows(m, s, (size_t)turl, width);
                for (int r = 0; r < tr; r++) ml_push(&n, i, L_TXTB, turl, r);
                lastw = 1000;
                at = le < len ? le + 1 : le;
                continue;
            }
            /* картинка-вложение: как в Discord, видна сама картинка, без строки «[файл: ...] url» */
            if (url < 0) do {
                size_t e, nx = wrap1(s, q, le, ww, &e);
                ml_push(&n, i, L_TEXT, (int)q, (int)(e - q));
                U.ml[n - 1].st = st;
                lastw = vwn(s + q, e - q) + (st ? 2 : 0);
                q = nx;
            } while (q < le);
            if (url >= 0) {
                /* вложение-спойлер (SPOILER_ в имени): до щелчка картинка не запрашивается вовсе */
                bool sp = !strncmp(s + q + 1 + strlen(TR("[файл: ")), "SPOILER_", 8) && !spoil_open(m->id);
                int pr = sp ? (ph > 6 ? 6 : ph) : preview_rows(s, (size_t)url, pw, ph);
                for (int r = 0; r < pr; r++) { ml_push(&n, i, L_PREVIEW, url, r); if (sp) U.ml[n - 1].st = ST_SPOIL; }
                lastw = 1000;
            }
            at = le < len ? le + 1 : le;
        }
        bool any = n > before;
        if (m->voice_secs) ml_push(&n, i, L_VMSG, 0, 0);
        else if (!any && !m->nemb && !m->poll) ml_push(&n, i, L_TEXT, 0, 0);  /* под аватаром всегда есть вторая строка */
        for (int ei = 0; ei < m->nemb; ei++) {   /* №53: карточки (строки считаются без картинок и сети) */
            ERow er[96];
            int nr = emb_layout(&m->emb[ei], width, er, 96);
            for (int r = 0; r < nr; r++) ml_push(&n, i, L_EMB, ei, r);
        }
        for (int r = 0; r < poll_nrows(m->poll); r++) ml_push(&n, i, L_POLL, 0, r);   /* №41 */
        for (int r = 0, nr = comp_rows(m, width); r < nr; r++) ml_push(&n, i, L_COMP, 0, r);   /* №55 */
        if (m->flags & 64) ml_push(&n, i, L_EPH, 0, 0);
#ifdef MC_PACKA
        if (m->state == MS_FAILED) ml_push(&n, i, L_FAIL, 0, 0);
#endif
        if (m->edited) {
            if (any && lastw + 11 <= width) U.ml[n - 1].ed = 1;
            else ml_push(&n, i, L_EDITED, 0, 0);
        }
        for (int r = 0; r < m->nreacts; ) {
            int used = 0, k = r;
            while (k < m->nreacts) {
                char lb[128];
                react_label(&m->reacts[k], lb, sizeof lb);
                int rw = vw(lb) + 1;
                if (used && used + rw > width + 1) break;
                used += rw;
                k++;
            }
            ml_push(&n, i, L_REACT, r, k - r);
            r = k;
        }
    }
    return n;
}
/* Выбранное сообщение должно быть видно; прокрутка в допустимых пределах. total = pre + nml. */
static void scroll_fix(int pre, int nml, int rows) {
    int total = pre + nml;
    if (U.focus == F_MESSAGES && U.follow_sel && U.sel_msg >= 0 && U.sel_msg < S.nmsgs) {
        int first = -1, last = -1;
        for (int i = 0; i < nml; i++) if (U.ml[i].mi == U.sel_msg && U.ml[i].kind != L_GAP && U.ml[i].kind != L_DATE) { if (first < 0) first = pre + i; last = pre + i; }
        if (first >= 0) {
            int bottom_idx = total - 1 - U.msg_scroll;
            if (last > bottom_idx) U.msg_scroll = total - 1 - last;
            if (first < bottom_idx - rows + 1) U.msg_scroll = total - 1 - (first + rows - 1);
        }
    }
    if (U.msg_scroll > total - rows) U.msg_scroll = total - rows;
    if (U.msg_scroll < 0) U.msg_scroll = 0;
}
/* Строка ленты idx (индекс в U.ml) в позиции y; x, w — лента целиком. */
/* ---- разметка Discord в строке сообщения ---- */
#define C_CODE   0xdcdde0u                       /* `код` и ```блок``` */
#define C_CODEBG 0x2c2f34u                       /* их фон: светлее ленты, как в Discord */
#define C_QUOTE  0x4e5058u                       /* полоска цитаты */
#define C_MENTBG 0x3c4270u                       /* @упоминание, #канал */
#define C_MENTFG 0xc9cdfbu
#define C_URL    0x4ea8deu
#define C_SPOILV 0x2e3035u                       /* открытый спойлер */
/* Самое длинное имя пользователя / канала (открытого сервера), с которого начинается p. -1 — нет. */
/* Конец имени в тексте: дальше конец строки, пробел или ASCII-знак (не буква и не цифра). */
static bool name_end(const char *p, size_t l, size_t maxn) {
    if (l >= maxn) return true;
    unsigned char c = (unsigned char)p[l];
    return !c || c == ' ' || c == '\n' || (c < 0x80 && !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'));
}
static int user_by_name(const char *p, size_t maxn, size_t *mlen) {
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < S.nusers; i++) {
        const char *nm = S.users[i].name;
        if (!nm || !*nm) continue;
        size_t l = strlen(nm);
        if (l > bl && l <= maxn && !strncmp(p, nm, l) && name_end(p, l, maxn)) { best = i; bl = l; }
    }
    *mlen = bl;
    return best;
}
static int user_index(u64 uid) {
    for (int i = 0; i < S.nusers; i++) if (S.users[i].id == uid) return i;
    return -1;
}
static int chan_by_name(const char *p, size_t maxn, size_t *mlen) {
    const Channel *oc = chan_find(S.open_ch);
    u64 gid = oc ? oc->guild_id : 0;
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < S.nchans; i++) {
        const Channel *c = &S.chans[i];
        if (!gid || c->guild_id != gid || c->type == CH_CATEGORY || !c->name || !*c->name) continue;
        size_t l = strlen(c->name);
        if (l > bl && l <= maxn && !strncmp(p, c->name, l) && name_end(p, l, maxn)) { best = i; bl = l; }
    }
    *mlen = bl;
    return best;
}
/* Строка [off, off+len) текста s сообщения mi с разметкой Discord: **жирный**, __подчёркнутый__, `код`,
 * ||спойлер|| (закрыт до щелчка), @упоминание и #канал плашкой (щелчок), ссылки синим. Состояние разметки
 * считается с начала исходной строки src: жирный, разорванный переносом, продолжается. Маркеры не рисуются;
 * незакрытый маркер — обычный текст (как в Discord). Вернуть занятые клетки. */
/* №45 разметка: строка разбирается store-функцией md_line (заголовки, -# мелкий, списки; куски с MD_*),
 * разбор кэшируется на одну исходную строку (строки одной исходной идут подряд). Маркеры не рисуются. */
static struct { const char *s; size_t src, n; MdLine ln; MdSpan sp[64]; int k; } MDC;
static void md_prep(const char *s, size_t src, size_t le) {
    if (MDC.s == s && MDC.src == src && MDC.n == le - src) return;
    MDC.s = s; MDC.src = src; MDC.n = le - src;
    MDC.k = md_line(s + src, le - src, &MDC.ln, MDC.sp, 64);
}
static int md_at(size_t rel) {
    for (int i = 0; i < MDC.k; i++) if (rel >= MDC.sp[i].off && rel < (size_t)MDC.sp[i].off + MDC.sp[i].len) return i;
    return -1;
}
/* Ссылки под щелчком (на кадр): щелчок копирует НАСТОЯЩИЙ адрес (в браузере не открываем). Б-1: у маскированной
 * ссылки, чей текст похож на адрес другого хоста, рядом показан настоящий хост и щелчок предупреждает. */
typedef struct { int mi, uoff, ulen; bool warn; char *url; } LkEnt;   /* url — копия (строка не из content) */
static LkEnt lkt[32];
static int n_lkt;
/* Хост из адреса: без схемы, до / ? # : или пробела, в нижнем регистре (только ASCII). */
static void url_host(const char *u, size_t n, char *out, size_t on) {
    size_t a = 0, o = 0;
    if (n >= 8 && !strncmp(u, "https://", 8)) a = 8;
    else if (n >= 7 && !strncmp(u, "http://", 7)) a = 7;
    const char *at = NULL;                       /* user@host — берём после @ */
    for (size_t i = a; i < n && u[i] != '/' && u[i] != '\\' && u[i] != '?' && u[i] != '#'; i++) if (u[i] == '@') at = u + i;
    if (at) a = (size_t)(at - u) + 1;
    for (size_t i = a; i < n && o + 1 < on; i++) {
        char c = u[i];
        if (c == '/' || c == '\\' || c == '?' || c == '#' || c == ':' || c == ' ' || c == ')') break;
        out[o++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    out[o] = 0;
    if (o > 4 && !strncmp(out, "www.", 4)) memmove(out, out + 4, o - 3);
}
/* Текст ссылки похож на адрес (есть точка, нет пробелов, или со схемой)? */
static bool looks_url(const char *t, size_t n) {
    if (n >= 7 && (!strncmp(t, "http://", 7) || !strncmp(t, "https://", 8))) return true;
    bool dot = false;
    for (size_t i = 0; i < n; i++) { if (t[i] == ' ') return false; if (t[i] == '.' && i > 0 && i + 1 < n) dot = true; }
    return dot;
}
/* Б-1: предупреждать, если хост адреса не латиницей (омоглифы «dіscord.com») или текст ссылки — адрес другого хоста. */
static bool lk_warn(const char *t, size_t tn, const char *u, size_t un) {
    char ht[96], hu[96];
    url_host(u, un, hu, sizeof hu);
    for (const char *q = hu; *q; q++) if ((unsigned char)*q >= 0x80) return true;
    if (!t || !tn) return false;
    url_host(t, tn, ht, sizeof ht);
    return looks_url(t, tn) && strcmp(ht, hu);
}
static int lk_add(int mi, int uoff, int ulen, bool warn) {
    if (n_lkt >= (int)(sizeof lkt / sizeof *lkt)) return -1;
    lkt[n_lkt] = (LkEnt){ mi, uoff, ulen, warn, NULL };
    return n_lkt++;
}
/* mi < 0 — строка не из content сообщения (эмбед): адрес копируется в таблицу кадра */
static int lk_any(int mi, const char *s, int uoff, int ulen, bool warn) {
    if (mi >= 0) return lk_add(mi, uoff, ulen, warn);
    int k = lk_add(-1, 0, ulen, warn);
    if (k >= 0) { lkt[k].url = xmalloc((size_t)ulen + 1); memcpy(lkt[k].url, s + uoff, (size_t)ulen); lkt[k].url[ulen] = 0; }
    return k;
}
static void jump_to(u64 ch, u64 mid);
/* https://(ptb.|canary.)discord(app).com/channels/<сервер|@me>/<канал>[/<сообщение>] — открыть у себя. */
static bool discord_link(const char *u, u64 *ch, u64 *mid) {
    char h[96];
    url_host(u, strlen(u), h, sizeof h);
    if (strcmp(h, "discord.com") && strcmp(h, "ptb.discord.com") && strcmp(h, "canary.discord.com") && strcmp(h, "discordapp.com")) return false;
    const char *p = strstr(u, "/channels/");
    if (!p) return false;
    p += 10;
    if (!strncmp(p, "@me/", 4)) p += 4;
    else { while (*p >= '0' && *p <= '9') p++; if (*p != '/') return false; p++; }
    char *e;
    *ch = strtoull(p, &e, 10);
    if (!*ch || e == p) return false;
    *mid = *e == '/' ? strtoull(e + 1, NULL, 10) : 0;
    return true;
}
static void link_click(int k) {
    if (k < 0 || k >= n_lkt) return;
    const char *s = lkt[k].url;
    if (s) { lkt[k].mi = 0; lkt[k].uoff = 0; }
    else if (lkt[k].mi >= 0 && lkt[k].mi < S.nmsgs) s = S.msgs[lkt[k].mi].content;
    if (!s || (size_t)(lkt[k].uoff + lkt[k].ulen) > strlen(s)) return;
    char u[2048], h[96];
    size_t n = (size_t)lkt[k].ulen < sizeof u - 1 ? (size_t)lkt[k].ulen : sizeof u - 1;
    memcpy(u, s + lkt[k].uoff, n);
    u[n] = 0;
    url_host(u, n, h, sizeof h);
    u64 dch = 0, dmid = 0;
    if (!lkt[k].warn && discord_link(u, &dch, &dmid)) {   /* №51: своя ссылка Discord — переход, не браузер */
        if (chan_find(dch) || O.demo) {
            if (dmid) jump_to(dch, dmid); else ui_open_channel(dch);
            return;
        }
        ui_notice(TR("канал из ссылки вам недоступен - ссылка скопирована"));
        copy_text(u);
        return;
    }
    char *sh = san(h, false);
    copy_text(u);
    bool nonlat = false;
    for (const char *q = h; *q; q++) if ((unsigned char)*q >= 0x80) nonlat = true;
    if (nonlat) ui_notice(TR("ОСТОРОЖНО: нелатинские буквы в адресе %s (скопирован)"), sh);
    else if (lkt[k].warn) ui_notice(TR("ОСТОРОЖНО: ссылка ведёт на %s (скопирован)"), sh);
    else ui_notice(TR("ссылка скопирована (%s)"), sh);
    free(sh);
}
static int g_mdline(int x, int y, int mi, const char *s, size_t src, size_t off, size_t len, int w) {
    size_t end = off + len, le = src;
    while (s[le] && s[le] != '\n') le++;
    md_prep(s, src, le);
    bool reveal = mi >= 0 && mi < S.nmsgs && spoil_open(S.msgs[mi].id);
    int lk = MDC.ln.kind;
    bool head = lk == ML_H1 || lk == ML_H2 || lk == ML_H3;
    u32 basefg = lk == ML_SUB ? C_MUTED : head ? C_WHITE : C_TEXT;
    int basea = head ? AT_B : 0;
    if (lk == ML_H1) basea |= AT_U;              /* крупнее терминал не умеет: H1 — жирный с чертой */
    int used = 0;
    if ((lk == ML_LIST || lk == ML_OLIST) && off <= src + 1) {   /* маркер списка — на первой строке пункта */
        int ind = MDC.ln.indent * 2;
        if (lk == ML_LIST) { if (ind + 1 < w) g_ch(x + ind, y, '*', C_MUTED, AT_B); used = ind + 2; }
        else used = ind + g_textn(x + ind, y, s + src + MDC.ln.num_off, MDC.ln.num_len, w - ind, C_MUTED, 0) + 1;
        if (used > w) used = w;
    }
    size_t tok_end = 0;
    int tok_kind = 0, tok_arg = -1, sp_x0 = -1, tx0 = -1, ln_x0 = -1, ln_si = -1, bare_lk = -1;
    for (size_t p = src; p < end && s[p]; ) {
        int si = md_at(p - src);
        if (si < 0) {                            /* маркер разметки: не рисуется */
            p++;
            continue;
        }
        const MdSpan *sp = &MDC.sp[si];
        int at = sp->attr;
        bool code = at & MD_CODE, link = at & MD_LINK, spl = at & MD_SPOIL;
        if (!code && !link && p >= tok_end) {
            tok_kind = 0;
            bool ws = p == src || s[p - 1] == ' ' || s[p - 1] == '(';
            size_t ml = 0;
            int ix;
            if (s[p] == '@' && ws) {
                if (!strncmp(s + p, "@everyone", 9) && name_end(s + p, 9, le - p)) { tok_kind = 2; tok_end = p + 9; }
                else if (!strncmp(s + p, "@here", 5) && name_end(s + p, 5, le - p)) { tok_kind = 2; tok_end = p + 5; }
                else if ((ix = user_by_name(s + p + 1, le - p - 1, &ml)) >= 0) { tok_kind = 1; tok_arg = ix; tok_end = p + 1 + ml; }
            } else if (s[p] == '#' && ws && (ix = chan_by_name(s + p + 1, le - p - 1, &ml)) >= 0) {
                tok_kind = 3; tok_arg = ix; tok_end = p + 1 + ml;
            } else if (!strncmp(s + p, "https://", 8) || !strncmp(s + p, "http://", 7)) {
                size_t q = p;
                while (q < le && s[q] != ' ') q++;
                tok_kind = 4; tok_end = q;
                bare_lk = lk_any(mi, s, (int)p, (int)(q - p), lk_warn(NULL, 0, s + p, q - p));
            }
            tx0 = -1;
        }
        u32 c;
        int k = utf8_dec(s + p, &c), cw = shown_w(c);
        bool intok = p < tok_end && tok_kind && !code && !link;
        if (p >= off && cw) {
            if (used + cw > w) break;
            u32 fg = basefg, bg = 0;
            int a = basea | (at & MD_B ? AT_B : 0) | (at & MD_U ? AT_U : 0) | (at & MD_I ? AT_IT : 0) | (at & MD_S ? AT_ST : 0);
            if (code) { fg = C_CODE; bg = C_CODEBG; }
            else if (link || (intok && tok_kind == 4)) { fg = C_URL; a |= AT_U; }
            else if (intok) { fg = C_MENTFG; bg = C_MENTBG; }
            if (spl && !reveal) fg = bg = C_BTN2;
            else if (spl && !bg) bg = C_SPOILV;
            if (sel_in(mi, (int)p) && !(spl && !reveal)) bg = C_SELTXT;
            if (bg) g_bg(x + used, y, cw, bg);
            g_put(x + used, y, c, cw, fg, a);
            cm_set(x + used, y, cw, mi, (int)p);
            if (spl && !reveal && sp_x0 < 0) sp_x0 = x + used;
            if (intok && tx0 < 0) tx0 = x + used;
            if (link && ln_x0 < 0) { ln_x0 = x + used; ln_si = si; }
            used += cw;
        }
        p += (size_t)k;
        /* конец спойлера / ссылки / упоминания в этой строке — кликабельная область */
        int nsi = p < le ? md_at(p - src) : -1;
        bool sp_end = spl && (nsi < 0 || !(MDC.sp[nsi].attr & MD_SPOIL) || p >= end);
        if (sp_end && sp_x0 >= 0 && !reveal) { hit_add(sp_x0, y, x + used - sp_x0, 1, A_SPOIL, 0, mi, HV_BG); sp_x0 = -1; }
        bool ln_end = link && (p >= (size_t)src + sp->off + sp->len || p >= end);
        if (ln_end && ln_x0 >= 0) {
            /* Б-1: текст-«адрес» ведёт на другой хост — показать настоящий */
            char hu[96];
            url_host(s + src + sp->url_off, sp->url_len, hu, sizeof hu);
            bool warn = lk_warn(s + src + sp->off, sp->len, s + src + sp->url_off, sp->url_len);
            int li = lk_any(mi, s, (int)(src + sp->url_off), sp->url_len, warn);
            if (li >= 0) hit_add(ln_x0, y, x + used - ln_x0, 1, A_LINK, 0, li, HV_UL);
            if (warn && p >= (size_t)src + sp->off + sp->len) {
                char t[120];
                char *sh = san(hu, false);
                snprintf(t, sizeof t, " -> %s", sh);
                free(sh);
                used += g_textn(x + used, y, t, (size_t)-1, w - used, C_GOLD, AT_B);
            }
            ln_x0 = -1;
        }
        if (intok && p >= tok_end && tx0 >= 0) {
            if (tok_kind == 1 || tok_kind == 3) hit_add(tx0, y, x + used - tx0, 1, tok_kind == 1 ? A_MENT : A_CHLINK, 0, tok_arg, HV_UL);
            else if (tok_kind == 4 && bare_lk >= 0) {
                hit_add(tx0, y, x + used - tx0, 1, A_LINK, 0, bare_lk, HV_UL);
                if (lkt[bare_lk].warn) used += g_textn(x + used, y, " (!)", 4, w - used, C_GOLD, AT_B);
            }
            tx0 = -1;
        }
    }
    if (sp_x0 >= 0 && !reveal) hit_add(sp_x0, y, x + used - sp_x0, 1, A_SPOIL, 0, mi, HV_BG);
    if (tx0 >= 0 && (tok_kind == 1 || tok_kind == 3)) hit_add(tx0, y, x + used - tx0, 1, tok_kind == 1 ? A_MENT : A_CHLINK, 0, tok_arg, HV_UL);
    else if (tx0 >= 0 && tok_kind == 4 && bare_lk >= 0) hit_add(tx0, y, x + used - tx0, 1, A_LINK, 0, bare_lk, HV_UL);
    if (ln_x0 >= 0 && ln_si >= 0) {              /* ссылка переносится на следующую строку: щелчок и здесь */
        const MdSpan *sp = &MDC.sp[ln_si];
        int li = lk_any(mi, s, (int)(src + sp->url_off), sp->url_len, lk_warn(s + src + sp->off, sp->len, s + src + sp->url_off, sp->url_len));
        if (li >= 0) hit_add(ln_x0, y, x + used - ln_x0, 1, A_LINK, 0, li, HV_UL);
    }
    cm_tail(x + used, y, x + w, mi, (int)end);
    return used;
}
/* ---- превью текстового вложения (как в Discord): рамка с именем и размером, первые строки моноширинно,
 * «Развернуть»/«Свернуть» (до 64 КБ, прокрутка в блоке), «Скачать» (копирует ссылку). Текст даёт
 * text_attachment_peek (разработчик): только видимое, без кэша; ui держит лишь номера строк на кадр. ---- */
#ifndef MC_TXTPEEK
/* ВРЕМЕННО: API ещё не объявлен (MC_TXTPEEK) — в демо синтетический текст, вживую — «ещё не подключено» */
typedef struct { bool busy; char error[160]; const char *text; size_t len; long size; bool truncated; } TextPeek;
static const TextPeek *text_attachment_peek(const char *url, size_t want) {
    static TextPeek tp;
    static char buf[4096];
    memset(&tp, 0, sizeof tp);
    (void)want;
    if (!O.demo) { snprintf(tp.error, sizeof tp.error, TR("просмотр текста ещё не подключён")); return &tp; }
    if (strstr(url, "404")) { snprintf(tp.error, sizeof tp.error, TR("файл недоступен (404)")); return &tp; }
    size_t o = 0;
    for (int i = 1; i <= 40 && o + 100 < sizeof buf; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, TR("%d\tстрока текста номер %d: The quick brown fox, съешь же ещё этих булок\n"), i, i);
    tp.text = buf; tp.len = o; tp.size = (long)o;
    return &tp;
}
#endif
static const TextPeek *txt_peek_g(const char *url, size_t want) {
    static TextPeek held;
    if (img_hold()) { memset(&held, 0, sizeof held); snprintf(held.error, sizeof held.error, TR("скрыто, пока запрос не принят")); return &held; }
    return text_attachment_peek(url, want);
}
#define TXT_ROWS 20                               /* свёрнуто: столько строк, развёрнуто — окно 30 строк с прокруткой */
#define TXT_ROWS_X 30
static struct { u64 id; int scroll; } txt_open[8];  /* развёрнутые превью: сообщение и прокрутка */
static unsigned txt_n;
static int txt_find(u64 id) { for (int i = 0; i < 8; i++) if (txt_open[i].id == id && id) return i; return -1; }
static void txt_toggle(u64 id) {
    int k = txt_find(id);
    if (k >= 0) txt_open[k].id = 0;
    else { k = (int)(txt_n++ & 7); txt_open[k].id = id; txt_open[k].scroll = 0; }
}
/* Строка вложения «[файл: имя.txt] url» — текстовый файл? Вернуть смещение url или -1. */
static int text_line(const char *s, size_t a, size_t e) {
    static const char pre[] = "[файл: ";
    if (a >= e || s[a] != ATT_MARK) return -1;   /* А1: только настоящие вложения */
    a++;
    size_t pl = sizeof pre - 1;
    if (e - a < pl || strncmp(s + a, pre, pl)) return -1;
    const char *rb = memchr(s + a, ']', e - a);
    if (!rb) return -1;
    static const char *const ext[] = { ".txt", ".log", ".md", ".csv", ".json", ".xml", ".ini", ".cfg", ".conf", ".yml",
        ".yaml", ".toml", ".c", ".h", ".cpp", ".py", ".js", ".ts", ".sh", ".bat", ".ps1", ".go", ".rs", ".java", ".lua",
        ".sql", ".diff", ".patch" };
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        size_t el = strlen(ext[i]);
        if ((size_t)(rb - (s + a)) >= pl + el && !strncmp(rb - el, ext[i], el)) return (int)(rb - s + 2 <= (long)e ? rb - s + 2 : (long)e);
    }
    return -1;
}
/* Начала видимых строк текста при ширине w (перенос по ширине, табуляция — до кратного 4). Запоминается на кадр. */
static int txt_wrap(const char *t, size_t len, int w, const int **starts) {
    static int *st;
    static int cap, n;
    static const char *kt;
    static size_t kl;
    static int kw;
    static unsigned kf;
    if (kt == t && kl == len && kw == w && kf == frame_no) { *starts = st; return n; }
    n = 0;
    int col = 0;
    for (size_t i = 0; i <= len; ) {
        if (col == 0) { if (n == cap) { cap = cap ? cap * 2 : 64; st = xrealloc(st, (size_t)cap * sizeof *st); } st[n++] = (int)i; }
        if (i == len) break;
        u32 c;
        int k = utf8_dec(t + i, &c);
        if (c == '\n') { i += (size_t)k; col = 0; if (i == len) break; continue; }
        int cw = c == '\t' ? 4 - col % 4 : shown_w(c);
        if (col + cw > w && col > 0) { col = 0; continue; }
        col += cw;
        i += (size_t)k;
    }
    if (n > 1 && st[n - 1] == (int)len) n--;
    kt = t; kl = len; kw = w; kf = frame_no;
    *starts = st;
    return n;
}
/* Сколько строк ленты займёт блок: рамка сверху, строки текста (или одна — загрузка/ошибка), рамка снизу. */
static int txt_rows(const Message *m, const char *s, size_t url_off, int tw) {
    char u2[2048];
    att_url(s, url_off, u2, sizeof u2, false);
    bool open = txt_find(m->id) >= 0;
    const TextPeek *tp = txt_peek_g(u2, open ? 65536 : 4096);
    if (!tp || tp->busy || tp->error[0] || !tp->text) return 3;
    int bw = tw < 82 ? tw : 82;
    const int *st;
    int n = txt_wrap(tp->text, tp->len, bw - 4, &st);
    int lim = open ? TXT_ROWS_X : TXT_ROWS;
    return 2 + (n < lim ? (n ? n : 1) : lim);
}
static void fmt_size(long b, char *out, size_t n) {
    if (b < 0) snprintf(out, n, "?");
    else if (b < 1024) snprintf(out, n, TR("%ld Б"), b);
    else if (b < 1024 * 1024) snprintf(out, n, TR("%ld,%ld КБ"), b / 1024, (b % 1024) * 10 / 1024);
    else snprintf(out, n, TR("%ld,%ld МБ"), b >> 20, ((b >> 10) % 1024) * 10 / 1024);
}
/* Строка r блока текстового вложения сообщения mi (url — смещение ссылки в content) в клетках (x, y), ширина tw. */
static void draw_txt(int mi, const char *s, size_t url_off, int r, int x, int y, int tw) {
    const Message *m = &S.msgs[mi];
    char u2[2048], name[160] = "", t[200];
    att_url(s, url_off, u2, sizeof u2, true);
    {   /* имя файла — из «[файл: имя]» перед ссылкой */
        size_t a = url_off;
        while (a > 0 && s[a - 1] != '\n') a--;
        if (s[a] == ATT_MARK) a++;
        const char *nb = s + a + strlen(TR("[файл: ")), *ne = memchr(nb, ']', (size_t)(s + url_off - nb));
        if (ne) snprintf(name, sizeof name, "%.*s", (int)(ne - nb), nb);
    }
    int k = txt_find(m->id);
    bool open = k >= 0;
    const TextPeek *tp = txt_peek_g(u2, open ? 65536 : 4096);
    int bw = tw < 82 ? tw : 82, iw = bw - 4;
    const int *st = NULL;
    int n = tp && tp->text && !tp->busy && !tp->error[0] ? txt_wrap(tp->text, tp->len, iw, &st) : 0;
    int lim = open ? TXT_ROWS_X : TXT_ROWS, vis = n < lim ? (n ? n : 1) : lim;
    if (open) {
        if (txt_open[k].scroll > n - vis) txt_open[k].scroll = n - vis;
        if (txt_open[k].scroll < 0) txt_open[k].scroll = 0;
        hit_add(x, y, bw, 1, A_AR_TXT, 0, mi, 0);      /* колесо над блоком — прокрутка текста */
    }
    int last = 1 + vis;
    if (r == 0) {
        /* верх рамки: +- имя.txt ---------- 2,1 КБ -+ */
        g_hbox(x, y, bw);
        int u = g_text(x + 2, y, " ", 1, C_TEXT, 0);
        u += g_texte(x + 2 + u, y, name, bw - 18, C_WHITE, AT_B);
        g_text(x + 2 + u, y, " ", 1, C_TEXT, 0);
        if (tp && !tp->busy && !tp->error[0] && tp->size >= 0) {   /* недоступный/загружаемый — без размера */
            char sz[32];
            fmt_size(tp->size, sz, sizeof sz);
            snprintf(t, sizeof t, " %s ", sz);
            g_rtext(x + bw - 2, y, t, 14, C_MUTED, 0);
        }
        return;
    }
    if (r == last) {
        /* низ рамки: +- (Развернуть) (Скачать) -- строки a-b из N -+ */
        g_hbox(x, y, bw);
        int bx = x + 2;
        if (n > TXT_ROWS || open) {
            int w = g_button(bx, y, open ? TR("Свернуть") : TR("Развернуть"), bw - 4, 0, B_NOP);
            if (w > 0) { U.hit[U.nhit - 1].sub = B_TXT_EXP; U.hit[U.nhit - 1].arg = mi; bx += w + 1; }
        }
        int w = g_button(bx, y, TR("Скачать"), x + bw - 2 - bx, 0, B_NOP);
        if (w > 0) { U.hit[U.nhit - 1].sub = B_TXT_DL; U.hit[U.nhit - 1].arg = mi; bx += w + 1; }
        if (n > vis) {
            int a = open ? txt_open[k].scroll + 1 : 1;
            snprintf(t, sizeof t, TR(" строки %d-%d из %d%s "), a, a + vis - 1, n, tp && tp->truncated ? "+" : "");
            if (vw(t) < x + bw - 2 - bx) g_rtext(x + bw - 2, y, t, x + bw - 2 - bx, C_MUTED, 0);
        }
        return;
    }
    g_ch(x, y, '|', C_LINE, 0);
    g_ch(x + bw - 1, y, '|', C_LINE, 0);
    g_rect(x + 1, y, bw - 2, 1, C_CODEBG);
    if (!tp || tp->busy) { if (r == 1) g_text(x + 2, y, TR("загрузка..."), iw, C_MUTED, 0); return; }
    if (tp->error[0]) { if (r == 1) { char *e = san(tp->error, false); g_texte(x + 2, y, e, iw, C_RED, 0); free(e); } return; }
    int li = (open ? txt_open[k].scroll : 0) + r - 1;
    if (li >= n) return;
    size_t a = (size_t)st[li], b = li + 1 < n ? (size_t)st[li + 1] : tp->len;
    int col = 0;
    for (size_t i = a; i < b && col < iw; ) {
        u32 c;
        int kk = utf8_dec(tp->text + i, &c);
        i += (size_t)kk;
        if (c == '\n' || c == '\r') continue;
        if (c == '\t') { col += 4 - col % 4; continue; }
        int cw = shown_w(c);
        if (!cw || col + cw > iw) continue;
        g_put(x + 2 + col, y, c, cw, C_CODE, 0);
        col += cw;
    }
}

/* ---- №53 эмбед: строка row карточки ei. Б-4: цвет — только полоской слева; Б-3: адрес — только показ и
 * копирование щелчком; Б-2: картинка — только вложение CDN (emb_img_ok), и только для видимой строки. ---- */
#define C_EMBBG 0x2b2d31u
#define C_POLLBAR 0x3b4270u
static void draw_emb_row(int tx, int y, int tw, const Message *m, int ei, int row) {
    if (ei < 0 || ei >= m->nemb) return;
    const Embed *e = &m->emb[ei];
    ERow rows[96];
    int n = emb_layout(e, tw, rows, 96);
    if (row < 0 || row >= n) return;
    int ew = emb_w(tw), cw = ew - 3, x = tx + 2;
    u32 under = bg_at(tx + ew - 1, y);
    g_rect(tx, y, ew, 1, C_EMBBG);
    g_round_corners(tx, y, ew, n, row, under);
    g_rect(tx, y, 1, 1, e->has_color ? (e->color & 0xFFFFFFu) : 0x4e5058u);
    const ERow *r = &rows[row];
    switch (r->kind) {
    case ER_STR: {
        const char *str = embed_str(e, r->k);
        size_t s0, a, b;
        if (!ew_line(str, cw, r->r, &s0, &a, &b)) break;
        if (r->k == EM_DESC) { g_mdline(x, y, -1, str, s0, a, b - a, cw); break; }
        const char *url = embed_str(e, EM_URL);
        bool tl = r->k == EM_TITLE && url[0];
        u32 fg = r->k == EM_TITLE ? (tl ? C_URL : C_WHITE) : r->k == EM_AUTHOR ? C_WHITE : C_MUTED;
        int u = g_textn(x, y, str + a, b - a, cw, fg, r->k == EM_TITLE || r->k == EM_AUTHOR ? AT_B : 0);
        if (tl) {
            int li = lk_any(-1, url, 0, (int)strlen(url), lk_warn(str, strlen(str), url, strlen(url)));
            if (li >= 0) hit_add(x, y, u, 1, A_LINK, 0, li, HV_UL);
        }
        break;
    }
    case ER_FROW: {
        int cnt = r->cnt ? r->cnt : 1, colw = cnt > 1 ? (cw - 2 * (cnt - 1)) / cnt : cw;
        for (int j = 0; j < cnt; j++) {
            const char *nm, *val;
            bool inl;
            if (!embed_field(e, r->k + j, &nm, &val, &inl)) continue;
            int cx = x + j * (colw + 2), nr = ew_rows(nm, colw);
            size_t s0, a, b;
            if (r->r < nr) { if (ew_line(nm, colw, r->r, &s0, &a, &b)) g_textn(cx, y, nm + a, b - a, colw, C_WHITE, AT_B); }
            else if (ew_line(val, colw, r->r - nr, &s0, &a, &b)) g_mdline(cx, y, -1, val, s0, a, b - a, colw);
        }
        break;
    }
    case ER_IMG: {
        int pw, ph, ow = 0, oh = 0;
        const char *url;
        emb_imgsize(e, cw, &pw, &ph, &url);
        if (!url) break;
        const ImgCell *pc = att_cells(url, pw, ph, &ow, &oh);
        if (pc && ow > 0 && ow <= pw && oh <= ph) {
            if (r->r < oh) { g_imgrow(x, y, ow, r->r, pc); ovl_note(x, y, ow, oh, r->r, url); }
        } else {
            g_rect(x, y, pw, 1, C_PH);
            if (r->r == ph / 2) g_ctext(x, y, pw, TR("[ картинка ]"), C_MUTED, 0);
        }
        break;
    }
    default: break;
    }
}
/* ---- №69 голосовое сообщение: [>] полоса 0:03 / 0:07 (как плеер Discord); играет store (vmsg_*). ---- */
static void vmsg_toggle(int mi) {
    if (mi < 0 || mi >= S.nmsgs || !S.msgs[mi].voice_secs) return;
    const VoiceMsgState *vs = vmsg_state();
    if (vs && vs->mid == S.msgs[mi].id && (vs->active || vs->loading)) { vmsg_stop(); return; }
    if (!vmsg_play(S.msgs[mi].id)) {
        vs = vmsg_state();
        if (vs && vs->err[0]) { char *e = san(vs->err, false); ui_notice(TR("голосовое: %s"), e); free(e); }
    }
}
static void draw_vmsg_row(int tx, int y, int tw, int mi) {
    const Message *m = &S.msgs[mi];
    const VoiceMsgState *vs = vmsg_state();
    bool mine = vs && vs->mid == m->id;
    bool play = mine && vs->active, load = mine && vs->loading;
    int dur = mine && vs->dur_ms > 0 ? vs->dur_ms : m->voice_secs * 1000, pos = play ? vs->pos_ms : 0;
    int w = tw < 44 ? tw : 44;
    if (w < 20) return;
    u32 under = bg_at(tx + w - 1, y);
    g_pill(tx, y, w, C_EMBBG);
    (void)under;
    g_text(tx + 1, y, play || load ? "[#]" : "[>]", 3, C_WHITE, AT_B);
    hit_add(tx, y, 5, 1, A_VMSG, 0, mi, HV_BG);
    char t[48];
    snprintf(t, sizeof t, "%d:%02d / %d:%02d", pos / 60000, pos / 1000 % 60, dur / 60000, (dur + 999) / 1000 % 60);
    int tw2 = vw(t), bx = tx + 5, bw = w - 5 - tw2 - 2;
    if (bw >= 4) {
        int fill = dur > 0 ? (int)((i64)bw * pos / dur) : 0;
        for (int i = 0; i < bw; i++) g_ch(bx + i, y, i < fill ? '=' : '-', i < fill ? C_WHITE : C_MUTED, i < fill ? AT_B : 0);
    }
    if (load) g_text(bx, y, TR("загрузка..."), bw, C_MUTED, 0);
    else if (mine && !vs->active && vs->err[0]) { char *e = san(vs->err, false); g_texte(bx, y, e, bw, C_RED, 0); free(e); }
    g_text(tx + w - tw2 - 1, y, t, tw2, C_MUTED, 0);
}
/* ---- №41 опрос: вопрос, подпись, ответы (до голоса — кружки/квадраты; после голоса или по итогу — доли
 * полосой), итог и кнопки. Голос — щелчком по ответу или цифрой у выбранного сообщения. ---- */
static bool poll_closed(const Poll *p) { return p->final || (p->expiry && wall_ms() / 1000 >= p->expiry); }
static void poll_click(int mi, int ai) {
    if (mi < 0 || mi >= S.nmsgs || !S.msgs[mi].poll) return;
    Message *m = &S.msgs[mi];
    const Poll *p = m->poll;
    if (ai < 0 || ai >= p->n) return;
    if (poll_closed(p)) { ui_notice(TR("опрос завершён")); return; }
    u32 ids[10];
    int n = 0;
    if (p->multi) { for (int i = 0; i < p->n; i++) { bool on = p->a[i].me; if (i == ai) on = !on; if (on) ids[n++] = p->a[i].id; } }
    else if (!p->a[ai].me) ids[n++] = p->a[ai].id;   /* щелчок по своему ответу — снять голос */
    char err[160] = "";
    if (!poll_vote(S.open_ch, m->id, ids, n, err, sizeof err) && err[0]) { char *se = san(err, false); ui_notice("%s", se); free(se); }
}
static void poll_btn(int mi, int what) {
    if (mi < 0 || mi >= S.nmsgs || !S.msgs[mi].poll) return;
    char err[160] = "";
    bool ok = what ? poll_end(S.open_ch, S.msgs[mi].id, err, sizeof err) : poll_vote(S.open_ch, S.msgs[mi].id, NULL, 0, err, sizeof err);
    if (!ok && err[0]) { char *se = san(err, false); ui_notice("%s", se); free(se); }
}
static void draw_poll_row(int tx, int y, int tw, int mi, int row) {
    const Message *m = &S.msgs[mi];
    const Poll *p = m->poll;
    if (!p) return;
    int pw = poll_w(tw), n = poll_nrows(p), x = tx + 2, w = pw - 4;
    u32 under = bg_at(tx + pw - 1, y);
    g_rect(tx, y, pw, 1, C_EMBBG);
    g_round_corners(tx, y, pw, n, row, under);
    bool closed = poll_closed(p), mine = false;
    int total = 0, best = 0;
    for (int i = 0; i < p->n; i++) { total += p->a[i].count; if (p->a[i].me) mine = true; if (p->a[i].count > best) best = p->a[i].count; }
    bool res = closed || mine;
    char t[200];
    if (row == 0) { g_texte(x, y, p->question ? p->question : "", w, C_WHITE, AT_B); return; }
    if (row == 1) {
        g_text(x, y, closed ? TR("Опрос завершён") : p->multi ? TR("Можно выбрать несколько ответов") : TR("Выберите один ответ"), w, C_MUTED, 0);
        return;
    }
    int ai = row - 2;
    if (ai < p->n) {
        const PollAnswer *a = &p->a[ai];
        int pct = total ? a->count * 100 / total : 0;
        if (res && w > 0) g_bg(x - 1, y, (w + 2) * pct / 100, C_POLLBAR);
        const char *mk = p->multi ? (a->me ? "[x]" : "[ ]") : (a->me ? "(*)" : "( )");
        bool win = closed && best > 0 && a->count == best;
        int u = g_text(x, y, mk, 3, a->me ? C_WHITE : C_MUTED, AT_B) + 1;
        if (res) snprintf(t, sizeof t, "%d%% (%d)", pct, a->count);
        else t[0] = 0;
        int rw = res ? vw(t) : 0;
        g_texte(x + u, y, a->text ? a->text : "", w - u - rw - 1, C_TEXT, win || a->me ? AT_B : 0);
        if (res) g_text(x + w - rw, y, t, rw, win ? C_WHITE : C_MUTED, win ? AT_B : 0);
        if (!closed) hit_add(tx, y, pw, 1, A_POLL, 0, mi << 4 | ai, HV_BG);
        return;
    }
    if (ai == p->n) {                            /* голоса и время */
        char left[48] = "";
        if (!closed && p->expiry) {
            i64 s = p->expiry - wall_ms() / 1000;
            if (s >= 86400) snprintf(left, sizeof left, TR(" - осталось %lld дн."), (long long)(s / 86400));
            else if (s >= 3600) snprintf(left, sizeof left, TR(" - осталось %lld ч"), (long long)(s / 3600));
            else snprintf(left, sizeof left, TR(" - осталось %lld мин"), (long long)(s / 60 + 1));
        }
        snprintf(t, sizeof t, TR("%d голос(ов)%s"), total, closed ? TR(" - итог") : left);
        g_texte(x, y, t, w, C_MUTED, 0);
        return;
    }
    /* кнопки: убрать голос, завершить (автор) */
    int u = 0;
    if (!closed && mine) {
        u = g_text(x, y, TR("[Убрать голос]"), w, C_URL, 0);
        hit_add(x, y, u, 1, A_POLLB, 0, mi, HV_UL);
        u += 2;
    }
    if (!closed && m->author_id == S.me_id) {
        int v = g_text(x + u, y, TR("[Завершить опрос]"), w - u, C_RED, 0);
        hit_add(x + u, y, v, 1, A_POLLB, 1, mi, HV_UL);
    }
}

/* ---- №55 кнопки и списки ботов: ряды (Comp.row) под сообщением. Кнопка-ссылка (style 5) в браузере не
 * открывается: щелчок копирует адрес (с той же проверкой Б-1). Список — окно выбора вариантов. ---- */
/* Видимый ряд каждого компонента при ширине tw: ряды бота, длинный ряд переносится; вернуть число рядов. */
static int comp_w(const Message *m, int i, int tw) {
    const Comp *c = &m->comp->c[i];
    if (c->type == CT_SELECT) return tw < 48 ? tw : 48;
    const char *em = comp_str(m, i, CS_EMOJI), *lb = comp_str(m, i, CS_LABEL);
    char t[160];
    snprintf(t, sizeof t, "%s%s%s%s", em, *em && *lb ? " " : "", lb, c->style == 5 ? " ^" : "");
    return (t[0] ? vw(t) : 1) + 4;
}
static int comp_layout(const Message *m, int tw, u8 *vr) {
    if (!m->comp) return 0;
    int row = -1, cur = -1, x = 0;
    for (int i = 0; i < m->comp->n; i++) {
        int w = comp_w(m, i, tw);
        if (m->comp->c[i].row != cur) { cur = m->comp->c[i].row; row++; x = 0; }
        else if (x + w > tw) { row++; x = 0; }
        vr[i] = (u8)row;
        x += w + 1;
    }
    return row + 1;
}
static int comp_rows(const Message *m, int tw) {
    u8 vr[64];
    if (!m->comp || m->comp->n > 64) return 0;
    return comp_layout(m, tw, vr);
}
static struct { bool on; u64 msg; int ci, sel; u32 chosen; } CSEL;   /* открытый список бота */
static void comp_press(int mi, int ci) {
    if (mi < 0 || mi >= S.nmsgs || !S.msgs[mi].comp || ci < 0 || ci >= S.msgs[mi].comp->n) return;
    const Message *m = &S.msgs[mi];
    const Comp *c = &m->comp->c[ci];
    if (c->disabled) { ui_notice(TR("кнопка недоступна")); return; }
    if (c->type == CT_SELECT) { CSEL.on = true; CSEL.msg = m->id; CSEL.ci = ci; CSEL.sel = 0; CSEL.chosen = 0; U.dirty = true; return; }
    if (c->style == 5) {                         /* ссылка: показать и скопировать, не открывать */
        const char *u = comp_str(m, ci, CS_URL);
        if (!*u) return;
        int li = lk_any(-1, u, 0, (int)strlen(u), lk_warn(NULL, 0, u, strlen(u)));
        link_click(li);
        return;
    }
    char err[200] = "";
    if (!comp_click(S.open_ch, m->id, ci, err, sizeof err) && err[0]) { char *se = san(err, false); ui_notice("%s", se); free(se); }
}
static void draw_comp_row(int tx, int y, int tw, int mi, int row) {
    const Message *m = &S.msgs[mi];
    if (!m->comp || m->comp->n > 64) return;
    u8 vr[64];
    comp_layout(m, tw, vr);
    int u = 0;
    for (int i = 0; i < m->comp->n; i++) {
        const Comp *c = &m->comp->c[i];
        if (vr[i] != row) continue;
        char t[160];
        if (c->type == CT_SELECT) {
            const char *ph = comp_str(m, i, CS_PLACEHOLDER);
            snprintf(t, sizeof t, "%s", *ph ? ph : TR("Выберите вариант"));
            int w = tw - u < 48 ? tw - u : 48;
            if (w < 10) break;
            g_rect(tx + u, y, w, 1, C_RAIL);
            g_texte(tx + u + 1, y, t, w - 4, c->disabled ? 0x5c6066u : C_MUTED, 0);
            g_text(tx + u + w - 2, y, "v", 1, C_TEXT, AT_B);
            hit_add(tx + u, y, w, 1, A_COMP, 0, mi << 5 | i, HV_BG);
            u += w + 1;
            continue;
        }
        const char *em = comp_str(m, i, CS_EMOJI), *lb = comp_str(m, i, CS_LABEL);
        snprintf(t, sizeof t, "%s%s%s%s", em, *em && *lb ? " " : "", lb, c->style == 5 ? " ^" : "");
        if (!t[0]) snprintf(t, sizeof t, "?");
        int w = vw(t) + 4;
        if (u + w > tw) break;
        u32 bg = c->style == 1 ? C_ACC : c->style == 3 ? 0x248046u : c->style == 4 ? C_DANGER : C_BTN2;
        if (c->disabled) bg = mix(bg, C_FEED, 140);
        g_pill(tx + u, y, w, bg);
        g_texte(tx + u + 2, y, t, w - 4, c->disabled ? C_MUTED : C_WHITE, AT_B);
        hit_add(tx + u, y, w, 1, A_COMP, 0, mi << 5 | i, HV_PILL);
        u += w + 1;
    }
}
/* Окно выбора варианта списка бота: Up/Down, Enter/щелчок — выбрать (несколько — отметить и «Готово»). */
static const char *un(u64 uid);
static int help_wrap(const char *s, int w, char ***out);
/* Обновление: окно появляется само, когда найдена новая версия (раз за сессию на версию; «Позже» — не показывать
 * её снова до перезапуска), во время загрузки и когда готово. Ссылки из «что нового» не открываются. */
static int lg_para(int x, int y, int w, const char *t, u32 fg, bool center);
static bool upd_on;
static char upd_shown[32], upd_later[32];
static void draw_update(int W, int H) {
    const UpdateState *us = update_state();
    if (!us || (us->phase != UP_AVAILABLE && us->phase != UP_DOWNLOADING && us->phase != UP_READY && us->phase != UP_FAILED)) { upd_on = false; return; }
    int bw = W - 4 < 64 ? W - 4 : 64, bh = H - 2 < 20 ? H - 2 : 20;
    if (bw < 30 || bh < 6) return;
    bool compact = bh < 12;                      /* маленькое окно (50x12): заголовок, текст сколько влезет, кнопки */
    int warn_rows = 0;
    if (us->phase == UP_AVAILABLE && us->shared_dir) {   /* предупреждение — целиком: окно выше, если нужно */
        char *wt = san(us->warn, true), **ls;
        warn_rows = help_wrap(wt, bw - 6, &ls);
        for (int i = 0; i < warn_rows; i++) free(ls[i]);
        free(ls); free(wt);
        int need = (compact ? 4 : 8) + warn_rows;
        if (need > bh) bh = need < H - 2 ? need : H - 2;
    }
    int px = (W - bw) / 2, py = (H - bh) / 2;
    g_dim(0, 0, W, H);
    ovl_cut(0, 0, W, H);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_FEED);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    char t[200];
    char *ver = san(us->version, false);
    if (us->phase == UP_READY) snprintf(t, sizeof t, TR("Версия %s загружена"), ver);
    else if (us->phase == UP_FAILED) snprintf(t, sizeof t, "%s", TR("Обновление не удалось"));
    else if (us->phase == UP_DOWNLOADING) snprintf(t, sizeof t, TR("Загрузка версии %s"), ver);
    else snprintf(t, sizeof t, TR("Доступна версия %s"), ver);
    free(ver);
    g_texte(px + 3, py + 1, t, bw - 6, C_WHITE, AT_B);
    snprintf(t, sizeof t, TR("сейчас у вас %s"), ASTRA_VERSION);
    if (!compact) g_texte(px + 3, py + 2, t, bw - 6, C_MUTED, 0);
    int y = compact ? py + 2 : py + 4, yb = py + bh - (compact ? 3 : 4);
    if (us->phase == UP_AVAILABLE && us->shared_dir) {   /* каталог программы доступен на запись другим — спросить */
        if (!compact) g_text(px + 3, y++, TR("ВНИМАНИЕ"), bw - 6, C_GOLD, AT_B);
        char *wt = san(us->warn, true), **ls;
        int n = help_wrap(wt, bw - 6, &ls);
        for (int i = 0; i < n; i++) { if (y <= yb) g_text(px + 3, y++, ls[i], bw - 6, compact ? C_GOLD : C_TEXT, 0); free(ls[i]); }
        free(ls); free(wt);
    } else if (us->phase == UP_FAILED) {
        char *e = san(us->err[0] ? us->err : TR("причина неизвестна"), false);
        y += lg_para(px + 3, y, bw - 6, e, C_RED, false);
        free(e);
    } else {
        if (!compact) g_text(px + 3, y++, TR("ЧТО НОВОГО"), bw - 6, C_MUTED, AT_B);
        char *nt = san(us->notes[0] ? us->notes : TR("(описания нет)"), true), **ls;
        int n = help_wrap(nt, bw - 6, &ls);
        for (int i = 0; i < n; i++) { if (y < yb) g_text(px + 3, y++, ls[i], bw - 6, C_TEXT, 0); free(ls[i]); }
        if (n > 0 && y >= yb) g_text(px + 3, yb, "...", 3, C_MUTED, 0);
        free(ls); free(nt);
    }
    int by = py + bh - 2;
    if (us->phase == UP_DOWNLOADING) {           /* полоса загрузки */
        int barw = bw - 22, fill = us->size > 0 ? (int)(us->got * barw / us->size) : 0;
        if (fill > barw) fill = barw;
        for (int i = 0; i < barw; i++) g_ch(px + 3 + i, by, i < fill ? '#' : '-', i < fill ? C_GREEN : C_LINE, i < fill ? AT_B : 0);
        snprintf(t, sizeof t, TR("%.1f / %.1f МБ"), us->got / 1048576.0, us->size / 1048576.0);
        g_text(px + 4 + barw, by, t, bw - barw - 6, C_MUTED, 0);
        return;
    }
    if (us->phase == UP_AVAILABLE && us->shared_dir) {
        int u = g_button(px + bw - 3 - 36, by, TR("Отмена"), 12, 0, B_UP_CANCEL);
        g_button(px + bw - 3 - 36 + u + 1, by, TR("Обновить всё равно"), 24, 2, B_UP_ANYWAY);
    } else if (us->phase == UP_READY) {
        int u = g_button(px + bw - 3 - 30, by, TR("Позже"), 12, 0, B_UP_LATER);
        g_button(px + bw - 3 - 30 + u + 1, by, TR("Перезапустить"), 18, 1, B_UP_RESTART);
    } else if (us->phase == UP_FAILED) {
        g_button(px + bw - 3 - 12, by, TR("Закрыть"), 12, 0, B_UP_LATER);
    } else {
        int u = g_button(px + bw - 3 - 26, by, TR("Позже"), 12, 0, B_UP_LATER);
        g_button(px + bw - 3 - 26 + u + 1, by, TR("Обновить"), 14, 1, B_UP_APPLY);
    }
}
static bool rules_on;                            /* №128: открыт экран правил */
static void draw_rules(int W, int H) {
    const RulesState *rs = guild_rules_state();
    if (!rs) { rules_on = false; return; }
    int bw = W - 4 < 70 ? W - 4 : 70, bh = H - 4 < 26 ? H - 4 : 26;
    int px = (W - bw) / 2, py = (H - bh) / 2;
    g_dim(0, 0, W, H);
    ovl_cut(0, 0, W, H);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_FEED);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    Guild *g = guild_find(rs->gid);
    char t[160];
    snprintf(t, sizeof t, TR("Правила сервера %s"), g && g->name ? g->name : "");
    g_texte(px + 3, py + 1, t, bw - 8, C_WHITE, AT_B);
    g_text(px + bw - 3, py + 1, "x", 1, C_TEXT, AT_B);
    hit_add(px + bw - 4, py + 1, 3, 1, A_BTN, B_RULES_X, 0, HV_RED);
    int y = py + 3, yb = py + bh - 3;
    if (rs->busy) g_text(px + 3, y, TR("загрузка..."), bw - 6, C_MUTED, 0);
    else if (rs->err[0]) { char *e = san(rs->err, false); g_texte(px + 3, y, e, bw - 6, C_RED, 0); free(e); }
    else {
        if (rs->desc && *rs->desc) {
            char *d = san(rs->desc, false), **ls;
            int n = help_wrap(d, bw - 6, &ls);
            for (int i = 0; i < n; i++) { if (y < yb) g_text(px + 3, y++, ls[i], bw - 6, C_MUTED, 0); free(ls[i]); }
            free(ls); free(d);
            y++;
        }
        for (int r = 0; r < rs->n && y < yb; r++) {
            char *rt = san(rs->rules[r] ? rs->rules[r] : "", false), **ls;
            snprintf(t, sizeof t, "%d. ", r + 1);
            int n = help_wrap(rt, bw - 10, &ls);
            for (int i = 0; i < n; i++) {
                if (y < yb) { if (!i) g_text(px + 3, y, t, 4, C_WHITE, AT_B); g_text(px + 7, y++, ls[i], bw - 10, C_TEXT, 0); }
                free(ls[i]);
            }
            free(ls); free(rt);
        }
    }
    int u = g_button(px + bw - 3 - 24, py + bh - 2, TR("Позже"), 10, 0, B_RULES_X);
    g_button(px + bw - 3 - 24 + u + 1, py + bh - 2, TR("Принимаю"), 14, 1, B_RULES_OK);
}
static bool tm_on;                               /* №97: открыто окно участников ветки */
static void draw_tmembers(int W, int H) {
    const ThreadMembers *tm = thread_members_state();
    if (!tm) { tm_on = false; return; }
    int bw = W - 4 < 44 ? W - 4 : 44, rows = H - 10, bh;
    if (rows > 16) rows = 16;
    bh = rows + 4;
    int px = (W - bw) / 2, py = (H - bh) / 2;
    g_dim(0, 0, W, H);
    ovl_cut(0, 0, W, H);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_FEED);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    char t[64];
    snprintf(t, sizeof t, TR("Участники ветки - %d"), tm->n);
    g_text(px + 3, py + 1, t, bw - 8, C_WHITE, AT_B);
    g_text(px + bw - 3, py + 1, "x", 1, C_TEXT, AT_B);
    hit_add(px + bw - 4, py + 1, 3, 1, A_BTN, B_TM_X, 0, HV_RED);
    if (tm->busy) g_text(px + 3, py + 3, TR("загрузка..."), bw - 6, C_MUTED, 0);
    else if (tm->err[0]) { char *e = san(tm->err, false); g_texte(px + 3, py + 3, e, bw - 6, C_RED, 0); free(e); }
    else for (int i = 0; i < tm->n && i < rows; i++) g_texte(px + 3, py + 3 + i, un(tm->ids[i]), bw - 6, C_TEXT, 0);
    if (tm->n > rows) { snprintf(t, sizeof t, TR("и ещё %d"), tm->n - rows); g_text(px + 3, py + bh - 1, t, bw - 6, C_MUTED, 0); }
}
static void comp_sel_close(void) { CSEL.on = false; U.dirty = true; }
static void comp_sel_done(void) {
    Message *m = msg_find(CSEL.msg, NULL);
    if (!m || !m->comp || CSEL.ci >= m->comp->n) { comp_sel_close(); return; }
    int opts[25], n = 0;
    for (int k = 0; k < m->comp->c[CSEL.ci].nopt && k < 25; k++) if (CSEL.chosen >> k & 1) opts[n++] = k;
    const Comp *c = &m->comp->c[CSEL.ci];
    if (n < (c->min ? c->min : 1)) { ui_notice(TR("выберите не меньше %d"), c->min ? c->min : 1); return; }
    char err[200] = "";
    if (!comp_select(S.open_ch, m->id, CSEL.ci, opts, n, err, sizeof err) && err[0]) { char *se = san(err, false); ui_notice("%s", se); free(se); }
    comp_sel_close();
}
static void comp_sel_pick(int k) {
    Message *m = msg_find(CSEL.msg, NULL);
    if (!m || !m->comp || CSEL.ci >= m->comp->n) { comp_sel_close(); return; }
    const Comp *c = &m->comp->c[CSEL.ci];
    if (k < 0 || k >= c->nopt) return;
    int mx = c->max ? c->max : 1;
    if (mx <= 1) { CSEL.chosen = 1u << k; comp_sel_done(); return; }
    if (CSEL.chosen >> k & 1) CSEL.chosen &= ~(1u << k);
    else {
        int cnt = 0;
        for (int q = 0; q < 25; q++) cnt += CSEL.chosen >> q & 1;
        if (cnt >= mx) { ui_notice(TR("можно выбрать не больше %d"), mx); return; }
        CSEL.chosen |= 1u << k;
    }
    U.dirty = true;
}
static void draw_comp_sel(int W, int H) {
    Message *m = msg_find(CSEL.msg, NULL);
    if (!m || !m->comp || CSEL.ci >= m->comp->n) { CSEL.on = false; return; }
    const Comp *c = &m->comp->c[CSEL.ci];
    int n = c->nopt, mx = c->max ? c->max : 1;
    int bw = W - 4 < 56 ? W - 4 : 56, rows = n < H - 8 ? n : H - 8, bh = rows + 5;
    if (rows < 1 || bw < 20) return;
    int px = (W - bw) / 2, py = (H - bh) / 2;
    g_dim(0, 0, W, H);
    ovl_cut(0, 0, W, H);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_FEED);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    const char *ph = comp_str(m, CSEL.ci, CS_PLACEHOLDER);
    g_texte(px + 3, py + 1, *ph ? ph : TR("Выберите вариант"), bw - 8, C_WHITE, AT_B);
    g_text(px + bw - 3, py + 1, "x", 1, C_TEXT, AT_B);
    hit_add(px + bw - 4, py + 1, 3, 1, A_BTN, B_CSEL_X, 0, HV_RED);
    if (CSEL.sel >= n) CSEL.sel = n - 1;
    if (CSEL.sel < 0) CSEL.sel = 0;
    int top = CSEL.sel >= rows ? CSEL.sel - rows + 1 : 0;
    for (int k = top; k < n && k - top < rows; k++) {
        const char *lb, *val;
        if (!comp_option(m, CSEL.ci, k, &lb, &val)) continue;
        int y = py + 3 + k - top;
        if (k == CSEL.sel) g_pill(px + 1, y, bw - 2, C_SEL);
        const char *mk = mx > 1 ? (CSEL.chosen >> k & 1 ? "[x]" : "[ ]") : "   ";
        g_text(px + 3, y, mk, 3, C_TEXT, AT_B);
        g_texte(px + 7, y, lb, bw - 10, C_TEXT, k == CSEL.sel ? AT_B : 0);
        hit_add(px + 1, y, bw - 2, 1, A_CSOPT, 0, k, HV_PILL);
    }
    char t[96];
    if (mx > 1) {
        snprintf(t, sizeof t, TR("Space отметить  Enter готово (до %d)  Esc отмена"), mx);
        int u = g_button(px + bw - 12, py + bh - 1, TR("Готово"), 10, 1, B_CSEL_OK);
        (void)u;
    } else snprintf(t, sizeof t, TR("Up/Down выбор  Enter выбрать  Esc отмена"));
    g_texte(px + 3, py + 3 + rows, t, bw - 16, C_MUTED, 0);
}
static bool comp_sel_key(const Key *k) {
    Message *m = msg_find(CSEL.msg, NULL);
    if (!m || !m->comp || CSEL.ci >= m->comp->n) { comp_sel_close(); return true; }
    const Comp *c = &m->comp->c[CSEL.ci];
    int mx = c->max ? c->max : 1;
    U.dirty = true;
    if (k->code == K_ESC) comp_sel_close();
    else if (k->code == K_UP) { if (CSEL.sel > 0) CSEL.sel--; }
    else if (k->code == K_DOWN) { if (CSEL.sel < c->nopt - 1) CSEL.sel++; }
    else if (k->code == K_ENTER) { if (mx > 1) comp_sel_done(); else comp_sel_pick(CSEL.sel); }
    else if (k->code == K_CHAR && k->ch == ' ' && mx > 1) comp_sel_pick(CSEL.sel);
    return true;
}

static void draw_mline(int idx, int x, int y, int w) {
    const MLine *l = &U.ml[idx];
    const Message *m = &S.msgs[l->mi];
    if (l->kind == L_GAP) return;
    if (l->kind == L_DATE) {
        char t[64];
        date_text(m->ts, t, sizeof t);
        {
            for (int i = x + 2; i < x + w - 2; i++) g_ch(i, y, '-', C_LINE, 0);
            int tx0 = g_ctext(x, y, w, t, C_MUTED, AT_B);
            g_ch(tx0 - 1, y, ' ', C_MUTED, 0);
            g_ch(tx0 + vw(t), y, ' ', C_MUTED, 0);
        }
        return;
    }
    if (l->kind == L_NEW) {
        /* «Новые сообщения»: красная черта и плашка «НОВОЕ» справа, как в Discord */
        for (int i = x + 2; i < x + w - 9; i++) g_ch(i, y, '-', C_RED, 0);
        g_pill(x + w - 9, y, 7, C_RED);
        g_text(x + w - 8, y, TR("НОВОЕ"), 5, C_WHITE, AT_B);
        return;
    }
    bool sel = U.focus == F_MESSAGES && U.sel_msg == l->mi;
    bool ment = mentions_me(m);
    int tx = x + 6, tw = w - 7;
    if (ment) { g_bg(x, y, w, C_MENT); g_bg(x, y, 1, C_GOLD); }
    if (sel) g_bg(x + (ment ? 1 : 0), y, w - (ment ? 1 : 0), C_SEL);
    hit_add(x, y, w, 1, A_MSG, 0, l->mi, HV_MSG);
    /* нижняя строка круглого аватара — под заголовком серии */
    if (idx > 0 && U.ml[idx - 1].kind == L_HEAD && U.ml[idx - 1].mi == l->mi)
        g_feedav(x + 1, y, 1, m->author_id);
    switch (l->kind) {
    case L_REPLY: {
        g_text(tx - 1, y, ",", 1, C_MUTED, 0);   /* столбец аватаров свободен: он под полосой мелкой сетки */
        int u = g_text(tx, y, "@", tw, C_MUTED, 0);
        u += g_text(tx + u, y, m->ref_author ? m->ref_author : "?", tw - u, C_TEXT, AT_B);
        u += g_text(tx + u, y, " ", tw - u, C_MUTED, 0);
        g_text(tx + u, y, m->ref_text ? m->ref_text : "", tw - u, C_MUTED, 0);
        hit_add(tx - 1, y, tw + 1, 1, A_REPLYJ, 0, l->mi, HV_UL);   /* щелчок — к исходному сообщению */
        break;
    }
    case L_FWD: g_text(tx, y, TR("-> Переслано"), tw, C_MUTED, AT_B); break;
    case L_FAIL: {
        int u = g_text(tx, y, TR("Не отправлено. "), tw, C_RED, AT_B);
        int w2 = g_text(tx + u, y, TR("Повторить"), tw - u, C_URL, AT_U);
        hit_add(tx + u, y, w2, 1, A_BTN, B_MRETRY, l->mi, HV_UL);
        u += w2 + g_text(tx + u + w2, y, " - ", tw - u - w2, C_MUTED, 0);
        int w3 = g_text(tx + u, y, TR("Удалить"), tw - u, C_URL, AT_U);
        hit_add(tx + u, y, w3, 1, A_BTN, B_MDISCARD, l->mi, HV_UL);
        break;
    }
    case L_EPH: {
        int u = g_text(tx, y, TR("(i) Видно только вам - "), tw, C_MUTED, 0);
        int w2 = g_text(tx + u, y, TR("Скрыть"), tw - u, C_URL, AT_U);
        hit_add(tx + u, y, w2, 1, A_BTN, B_EPH_HIDE, l->mi, HV_UL);
        break;
    }
    case L_TXTB: draw_txt(l->mi, m->content ? m->content : "", (size_t)l->off, l->len, tx, y, tw); break;
    case L_HEAD: {
        char t[24];
        fmt_time(m->ts, t, sizeof t);
        g_feedav(x + 1, y, 0, m->author_id);
        u32 nc = m->color ? m->color : C_WHITE;
        int tw_t = vw(t) + 1;                    /* время видно всегда: не влезает — сокращается ник */
        int u = g_text(tx, y, "<", 1, nc, AT_B);
        u += g_texte(tx + u, y, m->author ? m->author : "?", tw - tw_t - 3 > 4 ? tw - tw_t - 3 : 4, nc, AT_B);
        u += g_text(tx + u, y, ">", 1, nc, AT_B);
        int ui = user_index(m->author_id);
        if (ui >= 0) { hit_add(tx, y, u, 1, A_MENT, 0, ui, HV_UL); hit_add(x + 1, y, 4, 1, A_MENT, 0, ui, HV_UL); }   /* имя, аватар -> карточка */
#ifdef MC_WAVE1
        if (m->bot || m->webhook) { u += 1; g_pill(tx + u, y, 5, C_ACC); g_text(tx + u + 1, y, "BOT", 3, C_WHITE, AT_B); u += 5; }
#endif
        u += 1;
        {
            int tu = g_text(tx + u, y, t, tw - u, C_MUTED, 0);   /* наведение на время — полная дата */
            hit_add(tx + u, y, tu, 1, A_BTN, B_NOP, l->mi, HV_UL);
            U.hit[U.nhit - 1].tip = T_DATE;
            u += tu;
        }
        if (m->flags & 4096) g_text(tx + u, y, TR("  [тихое]"), tw - u, C_MUTED, 0);   /* без уведомлений */
        break;
    }
    case L_TEXT: {
        const char *s = m->content ? m->content : "";
        int u;
        if (l->st & ST_CODE) {
            /* блок кода: плашка C_CODEBG на всю ширину, текст как есть (без разметки) */
            g_rect(tx, y, tw, 1, C_CODEBG);
            u = 1 + g_codeline(tx + 1, y, l->mi, s, (size_t)l->off, (size_t)l->len, tw - 2, C_CODE);
            const MLine *pl = idx > 0 ? &U.ml[idx - 1] : NULL;
            bool first = !(pl && pl->mi == l->mi && pl->kind == L_TEXT && (pl->st & ST_CODE));
            if (first && tw >= 30 && n_cbt < (int)(sizeof cbt / sizeof *cbt)) {
                cbt[n_cbt].mi = l->mi; cbt[n_cbt].off = l->off;
                g_text(tx + tw - 13, y, TR("[копировать]"), 12, C_MUTED, 0);
                hit_add(tx + tw - 13, y, 12, 1, A_CODECP, 0, n_cbt++, HV_UL);
            }
        } else {
            /* полоска слева: пересланное (текст оригинала) и/или цитата «> » (её маркер не рисуется) */
            int qx = tx, qw = tw;
            size_t src = (size_t)l->off;
            while (src > 0 && s[src - 1] != '\n') src--;
            if (l->st & ST_FWD) { g_ch(qx, y, '|', C_QUOTE, AT_B); qx += 2; qw -= 2; }
            if (l->st & ST_QUOTE) { g_ch(qx, y, '|', C_QUOTE, AT_B); qx += 2; qw -= 2; src += src + 2 <= (size_t)l->off ? 2 : 1; }
            u = (qx - tx) + g_mdline(qx, y, l->mi, s, src, (size_t)l->off, (size_t)l->len, qw);
        }
        if (l->ed) g_text(tx + u, y, TR(" (изменено)"), tw - u, C_MUTED, 0);
#ifdef MC_PACKA
        if (m->state == MS_PENDING) g_fgset(tx, y, u, C_MUTED, 0);        /* ещё не подтверждено сервером */
        else if (m->state == MS_FAILED) g_fgset(tx, y, u, C_RED, 0);       /* не отправлено */
#endif
        break;
    }
    case L_EDITED: g_text(tx, y, TR("(изменено)"), tw, C_MUTED, 0); break;
    case L_EMB: draw_emb_row(tx, y, tw, m, l->off, l->len); break;
    case L_POLL: draw_poll_row(tx, y, tw, l->mi, l->len); break;
    case L_COMP: draw_comp_row(tx, y, tw, l->mi, l->len); break;
    case L_VMSG: draw_vmsg_row(tx, y, tw, l->mi); break;
    case L_FILT: {
        int u = g_text(tx, y, ignored(m->author_id) ? TR("сообщение от того, кого вы игнорируете ") : TR("скрыто фильтром "), tw, C_MUTED, 0);
        int v = g_text(tx + u, y, TR("[показать]"), tw - u, C_URL, 0);
        hit_add(tx + u, y, v, 1, A_FILTSHOW, 0, l->mi, HV_UL);
        break;
    }
    case L_PREVIEW: {
        int pw, ph;
        preview_size(tw, &pw, &ph);
        if (l->st & ST_SPOIL) {
            /* закрытый спойлер: серая плашка «СПОЙЛЕР», картинка не запрошена; щелчок — открыть */
            int sh = ph > 6 ? 6 : ph;
            u32 under = bg_at(tx, y);
            g_rect(tx, y, pw, 1, C_BTN2);
            if (l->len == sh / 2) g_ctext(tx, y, pw, TR("СПОЙЛЕР"), C_WHITE, AT_B);
            g_round_corners(tx, y, pw, sh, l->len, under);
            hit_add(tx, y, pw, 1, A_SPOIL, 0, l->mi, HV_BG);
            break;
        }
        /* превью картинки-вложения (l->off — смещение url в тексте, l->len — строка превью): ASCII из
         * img.c в обычных клетках; в окне поверх — та же картинка мелкой сеткой x4 (128x32) */
        char u2[2048];
        att_url(m->content, (size_t)l->off, u2, sizeof u2, true);
        u32 under = bg_at(tx, y);
        int ow = 0, oh = 0;
        const ImgCell *pc = att_cells(u2, pw, ph, &ow, &oh);
        if (pc && ow > 0 && ow <= pw && oh <= ph) {
            if (l->len < oh) g_imgrow(tx, y, ow, l->len, pc);
            ovl_note(tx, y, ow, oh, l->len, u2);
            iv_hit(tx, y, ow, l->mi, l->off);
        } else {
            iv_hit(tx, y, pw, l->mi, l->off);
            g_rect(tx, y, pw, 1, C_PH);
            if (l->len == ph / 2) g_ctext(tx, y, pw, img_click && !img_hold() ? TR("[ картинка: щелчок ]") : TR("[ картинка ]"), C_MUTED, 0);
            g_round_corners(tx, y, pw, ph, l->len, under);
        }
        break;
    }
    case L_REACT: {
        int u = 0;
        for (int r = l->off; r < l->off + l->len && r < m->nreacts; r++) {
            char lb[128];
            react_label(&m->reacts[r], lb, sizeof lb);
            int rw = vw(lb);
            if (u + rw > tw) break;
            bool me = m->reacts[r].me;
            g_pill(tx + u, y, rw, me ? C_REACTME : C_SIDE);
            g_textn(tx + u + 1, y, lb + 1, strlen(lb) - 2, rw - 2, me ? C_REACTFG : C_TEXT, me ? AT_B : 0);
            hit_add(tx + u, y, rw, 1, A_REACT, (u8)r, l->mi, HV_PILL);
            u += rw + 1;
        }
        break;
    }
    default: break;
    }
}

/* ======================= справка F1 ======================= */
/* k == NULL — заголовок раздела; d == PANIC_HERE — место строки паник-кея (если задан). */
typedef struct { const char *k, *d; } HelpItem;
static const char PANIC_HERE[] = "";
static const HelpItem HELP[] = {
    { NULL, "МЫШЬ" },
    { "левый щелчок", "сервер, канал, сообщение, кнопка; категория - свернуть/развернуть; голосовой канал - экран канала с кнопкой \"Присоединиться\"" },
    { "правый щелчок", "по сообщению - меню: ответить, изменить, удалить, реакция, копировать" },
    { "колесо", "листать ленту, каналы, серверы, справку" },
    { "щелчок по реакции", "поставить или снять свою" },
    { NULL, "ПАНЕЛИ" },
    { "Tab / Shift+Tab", "переключить колонку: серверы, каналы, сообщения, ввод" },
    { "Up / Down", "выбор в колонке" },
    { "Enter", "открыть канал или категорию; на голосовом - экран канала, ещё Enter - войти" },
    { "PgUp / PgDn", "листать сообщения; PgUp вверху подгружает более старые" },
    { "Ctrl+Q", "выход; Ctrl+= - 0 - масштаб окна" },
    { NULL, "СООБЩЕНИЯ" },
    { "r / e / d", "ответить, изменить своё, удалить своё" },
    { "+", "поставить реакцию" },
    { "1..9", "снять/поставить реакцию с этим номером (слева направо)" },
    { "Esc", "закрыть меню, вернуться в строку ввода" },
    { "Shift+щелчок", "выделить текст, Ctrl+C копирует" },
    { NULL, "СТРОКА ВВОДА" },
    { "Enter", "отправить" },
    { "Shift+Enter", "новая строка (Linux: Ctrl+J, Alt+Enter)" },
    { "Up в пустой", "изменить последнее своё" },
    { "Esc", "отменить ответ, правку или реакцию (кнопка [x])" },
    { "реакция", "эмодзи напрямую, :thumbsup: :heart: :fire: ... или номер" },
    { "Ctrl+O / +", "отправить файл до 10 МБ: путь к файлу, Enter; набранный текст уйдёт подписью" },
    { NULL, "ДРУЗЬЯ И ПОИСК" },
    { "Ctrl+F", "поиск сообщений сервера или беседы: Enter искать, Up/Down и Enter - к каналу, Esc закрыть" },
    { "Друзья", "Left/Right вкладки, Up/Down, Enter написать или принять, Del удалить или отклонить, . меню" },
    { "+", "в колонке ЛС: новая личная беседа - выбрать друга, Enter" },
    { NULL, "ГОЛОС И ЗВОНКИ" },
    { "F5 / F6 / F7", "микрофон, звук, отключиться (кнопки внизу слева; выключенное - красным с x)" },
    { "F8", "рация (с --ptt)" },
#ifdef ASTRA_VIDEO
    { "F12 / Alt+C", "камера вкл/выкл в звонке (кнопка []/ - с выбором камеры; щелчок по []/ КАМЕРА внизу - выключить)" },
    { "Alt+E", "скрывать глаза на своей камере вкл/выкл (анонимизация; не уверен в лице - закрывается весь кадр)" },
#endif
    { "F9 / F10", "порог активации голосом -/+ 3 дБ (и в меню настроек *)" },
    { "+ / -", "громкость участника голосового канала (в списке каналов)" },
    { "F4 / F2 / F3", "позвонить в ЛС ([позвонить]), принять входящий, отклонить" },
    { NULL, "АНОНИМНОСТЬ" },
    { "F11 / Alt+V", "смена голоса по кругу: выкл, ниже, выше (кнопка [голос: ...]); работает и вне звонка" },
    { "R / C / D", "при VLESS: нет связи - переподключиться, сменить ссылку, отключить VLESS (напрямую)" },
    { "VLESS 85 мс", "в заголовке канала: трафик идёт через туннель, задержка проверки" },
    { NULL, PANIC_HERE },
};
typedef struct { char *t; int kind; } HLine;     /* kind: 0 текст, 1 раздел */
static int help_wrap(const char *s, int w, char ***out) {
    int cap = 4, n = 0;
    char **ls = xmalloc((size_t)cap * sizeof(char *));
    size_t len = strlen(s), at = 0;
    if (w < 4) w = 4;
    while (at < len) {
        size_t e, nx = wrap1(s, at, len, w, &e);
        if (n == cap) { cap *= 2; ls = xrealloc(ls, (size_t)cap * sizeof(char *)); }
        char *p = xmalloc(e - at + 1);
        memcpy(p, s + at, e - at);
        p[e - at] = 0;
        ls[n++] = p;
        at = nx;
    }
    *out = ls;
    return n;
}
/* Строки справки для колонки ширины w: клавиша слева, описание с переносом справа. */
static int help_build(int w, HLine **out, int *kwo) {
    int cap = 64, n = 0, kw = 0;
    HLine *o = xmalloc((size_t)cap * sizeof *o);
    enum { NFIX = (int)(sizeof HELP / sizeof *HELP) };
    HelpItem items[NFIX];
    char pk[64];
    int nh = 0;
    const char *pn = panic_key_name();
    for (int i = 0; i < NFIX; i++) {
        if (HELP[i].d == PANIC_HERE) {
            if (!pn || !*pn) continue;
            char pnd[40];
            deco_ascii(pn, pnd, sizeof pnd);
            snprintf(pk, sizeof pk, "%s x3", pnd);
            items[nh++] = (HelpItem){ pk, TR("паник-кей: стереть токен и ссылку VLESS и выйти (аккаунт Discord не трогается)") };
            continue;
        }
        items[nh++] = (HelpItem){ HELP[i].k ? tr(HELP[i].k) : NULL, tr(HELP[i].d) };
    }
    for (int i = 0; i < nh; i++) if (items[i].k && vw(items[i].k) > kw) kw = vw(items[i].k);
    kw += 2;
    bool stacked = w < 2 + kw + 24;              /* узко: клавиша над описанием */
    *kwo = stacked ? 0 : 2 + kw;
#define HPUSH(tt, ti) do { if (n == cap) { cap *= 2; o = xrealloc(o, (size_t)cap * sizeof *o); } o[n++] = (HLine){ tt, ti }; } while (0)
    for (int i = 0; i < nh; i++) {
        Buf b = {0};
        if (!items[i].k) { if (n) HPUSH(xstrdup(""), 0); buf_printf(&b, " %s", items[i].d); HPUSH(b.p, 1); continue; }
        int dw = stacked ? w - 6 : w - 2 - kw;
        char **ls;
        int m = help_wrap(items[i].d, dw, &ls);
        if (stacked) {
            buf_printf(&b, "  %s", items[i].k);
            HPUSH(b.p, 0);
            for (int j = 0; j < m; j++) { Buf c = {0}; buf_printf(&c, "      %s", ls[j]); HPUSH(c.p, 0); free(ls[j]); }
        } else {
            for (int j = 0; j < m; j++) {
                Buf c = {0};
                buf_puts(&c, "  ");
                if (j == 0) { buf_puts(&c, items[i].k); for (int q = vw(items[i].k); q < kw; q++) buf_puts(&c, " "); }
                else for (int q = 0; q < kw; q++) buf_puts(&c, " ");
                buf_puts(&c, ls[j]);
                HPUSH(c.p, 0);
                free(ls[j]);
            }
        }
        free(ls);
    }
#undef HPUSH
    *out = o;
    return n;
}

/* ======================= кадр ======================= */
static void handle_key(const Key *k);

/* Самопроверка: кадр пишется сюда построчно, без терминала. */
static Buf *snap_out;
static int snap_w, snap_h;

static u32 fnv(const char *s, size_t n) { u32 h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; } return h; }

/* Строка готова: в снимок — всегда, на экран — только если изменилась с прошлого кадра. */
static void emit_line(Buf *f, const Buf *line, int y) {
    if (snap_out) { buf_put(snap_out, line->p, line->len); buf_puts(snap_out, "\n"); return; }
    u32 h = fnv(line->p, line->len);
    if (y < U.hash_n && U.hash[y] != h) {
        U.hash[y] = h;
        if (y < (int)sizeof row_emitted) row_emitted[y] = 1;
        buf_printf(f, "\x1b[%d;1H", y + 1);
        buf_put(f, line->p, line->len);
    }
}

static bool friends_screen(void);
static void search_off(void);
static const char *focus_hint(void) {
    /* в особом режиме строки ввода — подсказка режима, а не колонки; Esc ближе к началу: узкая строка обрезается справа */
    switch (U.mode) {
    case M_REPLY: return TR("Enter ответить  Esc отмена ответа  Shift+Enter новая строка  F1 справка");
    case M_EDIT: return TR("Enter сохранить  Esc отмена правки  Shift+Enter новая строка  F1 справка");
    case M_REACT: return TR("Enter поставить  эмодзи, :код: или номер  Esc отмена  F1 справка");
    case M_DELETE: return TR("y удалить  любая другая клавиша - отмена");
    case M_VLINK: return TR("вставьте ссылку vless://  Enter проверить  Esc отмена  ссылка не показывается");
    case M_VLOFF: return TR("y отключить VLESS  любая другая клавиша - отмена");
    case M_FILE: return TR("Enter отправить файл  Esc отмена  путь можно вставить  до 10 МБ  F1 справка");
    case M_UNFRIEND: return TR("y удалить из друзей  любая другая клавиша - отмена");
    default: break;
    }
    if (U.menu) return TR("Up/Down выбор  Enter выполнить  Esc закрыть");
    if (U.pick) return TR("имя друга - фильтр  Up/Down выбор  Enter создать ЛС  Esc закрыть");
    if (U.sq_edit) return TR("Enter искать  Esc закрыть  Backspace стереть");
    if (U.sr_focus) return U.rp == RP_PINS ? TR("Up/Down сообщение  Enter перейти к нему  End загрузить ещё  Esc закрыть") :
                           U.rp == RP_INBOX ? TR("Left/Right вкладки  Up/Down выбор  Enter открыть  Esc закрыть") :
                           TR("Up/Down результат  Enter перейти к каналу  Esc закрыть поиск  Tab колонки");
    if (U.focus == F_MESSAGES && friends_screen())
        return TR("Left/Right вкладки  Up/Down друг  Enter написать/принять  Del удалить/отклонить  . меню  F1 справка");
    if (vless_state() == VL_DOWN && !U.help) return TR("R переподключить  C сменить ссылку  D отключить VLESS  F1 справка");
    switch (U.focus) {
    case F_GUILDS: return TR("Up/Down сервер  Enter открыть  Tab дальше  F1 справка  Ctrl+Q выход");
    case F_CHANNELS: return TR("Enter открыть/свернуть  +/- громкость  F4 звонок  Esc серверы  Tab дальше  F1 справка");
    case F_MESSAGES: return TR("r ответ  e правка  d удалить  + реакция  1-9 реакция  правый щелчок - меню  Esc ввод  F1 справка");
    default: return TR("Enter отправить  Up правка последнего  PgUp/PgDn листать  Tab колонки  F1 справка");
    }
}

/* Отрезок нижней строки: не шире остатка. */
static void st_seg(int *x, int y, int *left, const char *s, u32 fg, int a, u32 bg) {
    if (*left <= 0 || !s) return;
    int x0 = *x;
    int u = g_text(*x, y, s, *left, fg, a);
    if (bg) g_bg(x0, y, u, bg);
    *x += u;
    *left -= u;
}
/* Подсказка клавиш: отрезки разделены двумя пробелами; не помещается — выводятся только целые отрезки. */
static void st_hint(int *x, int y, int *left, const char *s) {
    if (*left <= 0 || !s) return;
    if (vw(s) <= *left) { st_seg(x, y, left, s, C_MUTED, 0, 0); return; }
    size_t best = 0, best_word = 0;
    int w = 0;
    for (const char *p = s; *p; ) {
        u32 c;
        int k = utf8_dec(p, &c);
        if (c == ' ' && w <= *left) {
            if (p[1] == ' ') best = (size_t)(p - s);
            best_word = (size_t)(p - s);
        }
        w += shown_w(c);
        if (w > *left) break;
        p += k;
    }
    size_t n = best ? best : best_word;
    if (!n) return;
    int u = g_textn(*x, y, s, n, *left, C_MUTED, 0);
    *x += u;
    *left -= u;
}
/* Строка под полем ввода (клетки x0..x1-1): входящий звонок | «печатает...» | уведомление или подсказка. */
static bool show_ram = true;                     /* №13: память в строке состояния (настройка «Производительность») */
#ifdef ASTRA_VIDEO
/* кодер: имя автора трансляции — как в списке голосового канала (ник на сервере из VoiceState), иначе глобальное */
static const char *wv_uname(u64 uid) {
    for (int i = 0; i < S.nvs; i++) if (S.vs[i].user_id == uid && S.vs[i].name && *S.vs[i].name) return S.vs[i].name;
    return user_name(uid);
}
#endif
static int draw_status(int x0, int x1, int y) {
    int x = x0, left = x1 - x0 - (x1 >= U.gw ? 1 : 0);
    bool ram = show_ram && left >= 40;           /* №13: место под память — справа, подсказки короче */
    if (ram) left -= 11;
#ifdef ASTRA_VIDEO
    if (sh_running) {                            /* постоянный индикатор эфира; щелчок — остановить */
        char lv[160];                            /* 96 не хватало: «окно свёрнуто - зрители видят последний кадр» обрезалось */
        if (sh_gl_wait) {
            snprintf(lv, sizeof lv, "%s", TR(" (o) ЭФИР: выбор источника в системном окне... "));
        } else if (GL_phase == GL_FAILED) {
            char *e = san(GL_err, true);
            snprintf(lv, sizeof lv, TR(" (o) ЭФИР: ошибка - %s "), e);
            free(e);
        } else if (SHS.minimized) {              /* как в Discord: свёрнуто — поток идёт, зрители видят последний кадр */
            snprintf(lv, sizeof lv, "%s", TR(" (o) ЭФИР: окно свёрнуто - зрители видят последний кадр "));
        } else if (SHS.paused) {                 /* поток идёт: зрители видят последний чистый кадр */
            char *c = san(SHS.cover, true);
            if (*c) snprintf(lv, sizeof lv, TR(" (o) ЭФИР: окно перекрыто (%s) - показан последний кадр "), c);
            else snprintf(lv, sizeof lv, TR(" (o) ЭФИР: %s - показан последний кадр "), SHS.pause_why[0] ? tr(SHS.pause_why) : TR("окно перекрыто"));
            free(c);
        } else if (GL_phase != GL_LIVE) snprintf(lv, sizeof lv, "%s", GL_wait ? TR(" (o) ЭФИР: ждём зрителей ") : TR(" (o) ЭФИР: подключение... "));
        else if (SHS.audio) snprintf(lv, sizeof lv, TR(" (o) ЭФИР %dp %.0f к/с %.1f Мбит/с, со звуком "), SHS.h, SHS.fps_out, SHS.mbps);
        else snprintf(lv, sizeof lv, TR(" (o) ЭФИР %dp %.0f к/с %.1f Мбит/с "), SHS.h, SHS.fps_out, SHS.mbps);
        int xs = x;
        st_seg(&x, y, &left, lv, C_WHITE, AT_B, C_RED);
        hit_add(xs, y, x - xs, 1, A_BTN, B_SHARE, 0, HV_RED);
        st_seg(&x, y, &left, "  ", C_MUTED, 0, 0);
    }
    if (CS.phase == CAM_STARTING || CS.phase == CAM_WAIT_E2EE || CS.phase == CAM_LIVE) {   /* своя камера; щелчок — выключить */
        char lv[120];
        if (CS.phase == CAM_STARTING) snprintf(lv, sizeof lv, "%s", TR(" []/ КАМЕРА: включение... "));
        else if (CS.phase == CAM_WAIT_E2EE) snprintf(lv, sizeof lv, "%s", TR(" []/ КАМЕРА: ждём E2EE - пока видно только вам "));
        else {
            int an = cu_anon_st();
            if (an == CA_FULL) snprintf(lv, sizeof lv, "%s", TR(" []/ КАМЕРА: лицо не найдено - собеседники видят значок "));
            else if (an == CA_EYES) snprintf(lv, sizeof lv, TR(" []/ КАМЕРА %dp %.0f к/с, глаза скрыты "), CS.h, CS.fps_out);
            else snprintf(lv, sizeof lv, TR(" []/ КАМЕРА %dp %.0f к/с "), CS.h, CS.fps_out);
        }
        int xs = x;
        st_seg(&x, y, &left, lv, C_WHITE, AT_B, C_RED);
        hit_add(xs, y, x - xs, 1, A_BTN, B_CAM, 0, HV_RED);
        U.hit[U.nhit - 1].tip = T_CAMOFF;
        st_seg(&x, y, &left, "  ", C_MUTED, 0, 0);
    }
    if (WV.cam) {                                /* крупный просмотр камеры; щелчок — выйти (панель скрыта — вернуть) */
        char lv[160], *nm = san(wv_uname(WV.cam), true);
        snprintf(lv, sizeof lv, TR(" [>] СМОТРЮ КАМЕРУ %s "), nm);
        free(nm);
        int xs = x;
        st_seg(&x, y, &left, lv, C_WHITE, AT_B, C_ACC);
        hit_add(xs, y, x - xs, 1, A_BTN, WV.chat ? B_WATCH_SHOW : B_WATCH_STOP, 0, HV_ACC);
        U.hit[U.nhit - 1].tip = WV.chat ? T_WSHOW : T_WSTOP;
        st_seg(&x, y, &left, "  ", C_MUTED, 0, 0);
    }
    if (WS.phase != GW_OFF) {                    /* просмотр чужой трансляции; щелчок — выйти */
        char lv[200], *nm = san(wv_uname(WS.user), true);
        if (WS.phase == GW_FAILED) { char *e = san(WS.err, true); snprintf(lv, sizeof lv, TR(" [>] %s: %s "), nm, e); free(e); }
        else if (WS.phase != GW_LIVE) snprintf(lv, sizeof lv, TR(" [>] %s: подключение... "), nm);
        else snprintf(lv, sizeof lv, TR(" [>] СМОТРЮ %s: звук %lld, видео %lld пак. "), nm, WS.apkts, WS.vpkts);
        free(nm);
        int xs = x;
        st_seg(&x, y, &left, lv, C_WHITE, AT_B, C_ACC);
        hit_add(xs, y, x - xs, 1, A_BTN, WV.chat ? B_WATCH_SHOW : B_WATCH_STOP, 0, HV_ACC);   /* панель скрыта — вернуть её */
        U.hit[U.nhit - 1].tip = WV.chat ? T_WSHOW : T_WSTOP;
        st_seg(&x, y, &left, "  ", C_MUTED, 0, 0);
    }
#endif
    st_seg(&x, y, &left, "  ", C_MUTED, 0, 0);
    if (S.call.ringing) {
        Channel *cc = chan_find(S.call.channel_id);
        char lab[96] = "", t[160];
        if (cc) chan_label(cc, lab, sizeof lab);
        snprintf(t, sizeof t, TR(" ЗВОНОК: %s  F2 принять  F3 отклонить "), lab);
        st_seg(&x, y, &left, t, C_WHITE, AT_B, C_GREEN);
        st_seg(&x, y, &left, "  ", C_MUTED, 0, 0);
    }
    if (S.typing_until > now_ms() && S.typing_name[0]) {
        char t[120];
        snprintf(t, sizeof t, TR("~ %s печатает..."), S.typing_name);
        int xs = x;
        st_seg(&x, y, &left, t, C_TEXT, 0, 0);
        g_fgset(xs + 2, y, vw(S.typing_name), C_WHITE, AT_B);
        st_seg(&x, y, &left, "   ", C_MUTED, 0, 0);
    }
    if (U.notice_until > now_ms()) {
        char t[sizeof U.notice + 16];
        deco_ascii(U.notice, t, sizeof t);
        st_seg(&x, y, &left, t, C_WHITE, AT_B, 0);
    } else st_hint(&x, y, &left, focus_hint());
    if (ram) {                                   /* №13: память процесса справа в строке состояния */
        char rm[24];
        snprintf(rm, sizeof rm, TR("%.1f МБ"), (double)plat_private_bytes() / (1024.0 * 1024.0));
        g_rtext(x1 - 2, y, rm, 10, C_MUTED, 0);
    }
    return x;
}

/* Экран меньше 50x12. */
static void draw_small(int W, int H) {
    char t[96];
    bool narrow = W < 50;                        /* узко: сначала главное — текущий размер и выход */
    if (narrow) snprintf(t, sizeof t, TR("сейчас %dx%d, нужно 50x12"), W, H);
    else snprintf(t, sizeof t, TR("нужно не меньше 50x12, сейчас %dx%d"), W, H);
    if (H > 0) g_text(0, 0, TR("Astra DS: окно слишком маленькое"), H == 1 ? W - 1 : W, C_WHITE, AT_B);
    if (H > 1) g_text(0, 1, t, H == 2 ? W - 1 : W, C_TEXT, 0);
    if (H > 2) g_text(0, 2, narrow ? TR("Ctrl+Q - выход") : TR("растяните окно или уменьшите шрифт; Ctrl+Q - выход"), H == 3 ? W - 1 : W, C_MUTED, 0);
}

/* Состояние сети для заголовка канала. */
static void net_tag(char *rtag, size_t n, u32 *rfg, bool narrow) {
    char gst[128];
    deco_ascii(gw_status_text(), gst, sizeof gst);
    if (!gst[0] && O.demo) snprintf(gst, sizeof gst, TR("демо"));
    bool bad = !O.demo && (!S.ready || strcmp(gst, "в сети"));
    *rfg = bad ? C_GOLD : C_MUTED;
    if (!bad && g_gw_ping > 0) {                 /* №148: пинг шлюза цветом — до 100 мс зелёный, до 250 жёлтый, дальше красный */
        snprintf(gst + strlen(gst), sizeof gst - strlen(gst), TR(" %d мс"), g_gw_ping);
        *rfg = g_gw_ping < 100 ? C_GREEN : g_gw_ping < 250 ? C_GOLD : C_RED;
    }
    VlessState vst = vless_state();
    char vlt[48] = "";
    if (vst == VL_UP) {
        int ms = vless_latency_ms();
        if (ms >= 0) snprintf(vlt, sizeof vlt, TR("VLESS %d мс"), ms); else snprintf(vlt, sizeof vlt, "VLESS");
    } else if (vst == VL_STARTING) snprintf(vlt, sizeof vlt, "VLESS ...");
    snprintf(rtag, n, "%s%s%s", gst, gst[0] && vlt[0] ? " | " : "", vlt);
    if (vst == VL_DOWN) { snprintf(rtag, n, "%s", narrow ? "VLESS!" : TR("VLESS: нет связи")); *rfg = C_RED; }
}

static void draw_help(int W, int H) {
    g_rect(0, 0, W, H, C_FEED);
    g_rect(0, 0, W, 1, C_SIDE);
    char rtag[240];
    u32 rfg;
    net_tag(rtag, sizeof rtag, &rfg, W < 60);
    int bx = W - 4;
    g_text(bx, 0, "[x]", 3, C_TEXT, AT_B);
    hit_add(bx, 0, 3, 1, A_BTN, B_HELP_CLOSE, 0, HV_RED);
    int rmax = W / 3, rx;
    if (vw(rtag) > rmax) { rx = bx - 1 - rmax; g_texte(rx, 0, rtag, rmax, rfg, 0); }   /* длинный ярлык — с «...», не посреди слова */
    else rx = g_rtext(bx - 2, 0, rtag, rmax, rfg, 0);
    g_texte(1, 0, TR("справка: Up/Down листать, F1 или Esc закрыть"), rx - 3, C_WHITE, AT_B);
    int body_h = H - 2;
    bool two = W - 2 >= 96;
    int hcolw = two ? (W - 2 - 2) / 2 : W - 2, kw;
    HLine *hl;
    int nhl = help_build(hcolw, &hl, &kw), hsplit = nhl;
    if (two) {
        int best = nhl;
        for (int i = 1; i < nhl; i++) if (hl[i].kind == 1) {
            int cost = i > nhl - i ? i : nhl - i;
            if (cost < best) { best = cost; hsplit = i; }
        }
    }
    int hrows = hsplit > nhl - hsplit ? hsplit : nhl - hsplit;
    int maxs = hrows - body_h;
    if (U.help_scroll > maxs) U.help_scroll = maxs;
    if (U.help_scroll < 0) U.help_scroll = 0;
    hit_add(0, 1, W, body_h, A_AR_HELP, 0, 0, 0);
    for (int ry = 0; ry < body_h; ry++) {
        int y = 1 + ry, row = ry + U.help_scroll;
        if (ry == body_h - 1 && hrows - U.help_scroll > body_h) {
            g_pill(1, y, W - 2, C_ACC);
            g_ctext(1, y, W - 2, TR("ниже ещё: Down / PgDn"), C_WHITE, AT_B);
            continue;
        }
        for (int col = 0; col < (two ? 2 : 1); col++) {
            int li = col ? hsplit + row : row;
            if ((col == 0 && li >= hsplit) || li >= nhl) continue;
            int x = 1 + col * (hcolw + 2);
            if (hl[li].kind == 1) g_text(x, y, hl[li].t, hcolw, C_WHITE, AT_B);
            else {
                g_text(x, y, hl[li].t, hcolw, C_MUTED, 0);
                if (kw) g_fgset(x, y, kw < hcolw ? kw : hcolw, C_TEXT, AT_B);
            }
        }
    }
    for (int i = 0; i < nhl; i++) free(hl[i].t);
    free(hl);
}

/* Пункты меню; вернуть число. danger — красный пункт (удалить). */
static int menu_items(int kind, const Message *m, int *ids, const char **names, bool *danger) {
    int n = 0;
    static char fxl[64];
    if (kind == MENU_SET) {
        VoiceFx fx = voicefx_get();
        snprintf(fxl, sizeof fxl, TR("Смена голоса: %s     F11"), fx == VFX_LOW ? TR("ниже") : fx == VFX_HIGH ? TR("выше") : TR("выкл"));
        ids[n] = MI_FX; danger[n] = false; names[n++] = fxl;
        ids[n] = MI_VADDN; danger[n] = false; names[n++] = TR("Порог голоса -3 дБ     F9");
        ids[n] = MI_VADUP; danger[n] = false; names[n++] = TR("Порог голоса +3 дБ    F10");
        ids[n] = MI_HELP; danger[n] = false; names[n++] = TR("Справка                F1");
        ids[n] = MI_QUIT; danger[n] = true; names[n++] = TR("Выйти              Ctrl+Q");
        return n;
    }
    if (kind == MENU_GUILD) {
        u64 g = cur_guild_id();
        ids[n] = MI_G_INV; danger[n] = false; names[n++] = TR("Пригласить людей");
        if (can_manage_guild(g)) { ids[n] = MI_G_SET; danger[n] = false; names[n++] = TR("Настройки сервера"); }
        if (can_manage_channels(g)) {
            ids[n] = MI_G_CH; danger[n] = false; names[n++] = TR("Создать канал");
            ids[n] = MI_G_CAT; danger[n] = false; names[n++] = TR("Создать категорию");
            ids[n] = MI_G_MOVE; danger[n] = false; names[n++] = U.moving ? TR("Готово: перестановка") : TR("Переставить каналы");
        }
#ifdef MC_PACKA
        {
            Guild *gg = guild_find(g);
            static char nl[64];
            static const char *const lv[] = { "все сообщения", "только упоминания", "ничего", "по умолчанию" };
            ids[n] = MI_G_MUTE; danger[n] = false; names[n++] = gg && gg->muted ? TR("Включить уведомления сервера") : TR("Заглушить сервер");
            snprintf(nl, sizeof nl, TR("Уведомления: %s"), tr(lv[gg ? gg->notify & 3 : 3]));
            ids[n] = MI_G_NOTIFY; danger[n] = false; names[n++] = nl;
        }
#endif
        if (!guild_is_owner(g)) { ids[n] = MI_G_LEAVE; danger[n] = true; names[n++] = TR("Покинуть сервер"); }
        return n;
    }
    if (kind == MENU_VMEM) {                     /* участник голосового канала (правый щелчок по строке) */
        u64 uid = U.menu_uid;
        if (uid == S.me_id) {                    /* свой участник: микрофон и звук, без громкости */
            VoiceStatus v;
            voice_status(&v);
            ids[n] = MI_V_MIC; danger[n] = false; names[n++] = v.self_mute ? TR("Включить микрофон      F5") : TR("Выключить микрофон     F5");
            ids[n] = MI_V_DEAF; danger[n] = false; names[n++] = v.self_deaf ? TR("Включить звук          F6") : TR("Выключить звук         F6");
            ids[n] = MI_V_PROF; danger[n] = false; names[n++] = TR("Профиль");
            return n;
        }
        static char vl[48];
        int pc = (int)(voice_get_volume(uid) * 100 + 0.5f);
        snprintf(vl, sizeof vl, TR("Громкость: %d%%  (сброс 100%%)"), pc);
        ids[n] = MI_V_VOL; danger[n] = false; names[n++] = vl;
        ids[n] = MI_V_UP; danger[n] = false; names[n++] = TR("Громче  +10%           +");
        ids[n] = MI_V_DN; danger[n] = false; names[n++] = TR("Тише    -10%           -");
        ids[n] = MI_V_MUTE; danger[n] = false; names[n++] = pc == 0 ? TR("Включить для меня") : TR("Заглушить для меня");
        ids[n] = MI_V_PROF; danger[n] = false; names[n++] = TR("Профиль");
        Channel *vc = chan_find(U.menu_vch);
        u64 g = vc ? vc->guild_id : 0;
        if (g && mod_can(g, MOD_MUTE)) { ids[n] = MI_V_SMUTE; danger[n] = true; names[n++] = TR("Заглушить на сервере..."); }
        if (g && mod_can(g, MOD_MOVE)) { ids[n] = MI_V_MOVE; danger[n] = true; names[n++] = TR("Перенести или отключить..."); }
        return n;
    }
    if (kind == MENU_CHAN) {
        Channel *c = chan_find(U.menu_cid);
        u64 g = c ? c->guild_id : 0;
        if (g && can_manage_channels(g)) { ids[n] = MI_C_EDIT; danger[n] = false; names[n++] = TR("Изменить канал"); }
        if (g && can_manage_roles(g)) { ids[n] = MI_C_PERMS; danger[n] = false; names[n++] = TR("Права канала"); }
#ifdef MC_WAVE2
        if (g && c->type != CH_CATEGORY && can_create_invite(c->id)) { ids[n] = MI_C_INV; danger[n] = false; names[n++] = TR("Пригласить людей"); }
#else
        if (g && c->type != CH_CATEGORY) { ids[n] = MI_C_INV; danger[n] = false; names[n++] = TR("Пригласить людей"); }
#endif
#ifdef MC_PACKA
        if (c) {
            static char cl[64];
            static const char *const lv[] = { "все сообщения", "только упоминания", "ничего", "как у сервера" };
            ids[n] = MI_C_MUTE; danger[n] = false; names[n++] = c->muted ? TR("Включить уведомления") : (g ? TR("Заглушить канал") : TR("Заглушить беседу"));
            if (g) { snprintf(cl, sizeof cl, TR("Уведомления: %s"), tr(lv[c->notify & 3])); ids[n] = MI_C_NOTIFY; danger[n] = false; names[n++] = cl; }
        }
#endif
        if (c && c->type == CH_GROUP) {
            ids[n] = MI_GDM_ADD; danger[n] = false; names[n++] = TR("Добавить друзей");
            ids[n] = MI_GDM_LEAVE; danger[n] = true; names[n++] = TR("Покинуть группу");
        }
        if (c && c->guild_id && (c->type == CH_TEXT || c->type == CH_NEWS)) { ids[n] = MI_C_ARCH; danger[n] = false; names[n++] = TR("Архив веток"); }
        if (is_thread(c)) {
            ids[n] = MI_T_RENAME; danger[n] = false; names[n++] = TR("Переименовать ветку");
            ids[n] = MI_T_ARCH; danger[n] = false; names[n++] = c->tflags & TF_ARCHIVED ? TR("Вернуть из архива") : TR("В архив");
            ids[n] = MI_T_LOCK; danger[n] = false; names[n++] = c->tflags & TF_LOCKED ? TR("Открыть ветку") : TR("Закрыть ветку");
            ids[n] = MI_T_MEMB; danger[n] = false; names[n++] = TR("Участники ветки");
        }
        if (c && c->type == CH_GROUP) { ids[n] = MI_GDM_RENAME; danger[n] = false; names[n++] = TR("Переименовать группу"); }
        if (is_voice_t(c)) { ids[n] = MI_C_VSTATUS; danger[n] = false; names[n++] = TR("Изменить статус"); }
        if (is_thread(c)) { ids[n] = MI_T_JOIN; danger[n] = false; names[n++] = c->tflags & TF_JOINED ? TR("Покинуть ветку") : TR("Вступить в ветку"); }
        ids[n] = MI_C_ID; danger[n] = false; names[n++] = TR("Копировать ID канала");
        if (g && can_manage_channels(g)) { ids[n] = MI_C_DEL; danger[n] = true; names[n++] = c->type == CH_CATEGORY ? TR("Удалить категорию") : TR("Удалить канал"); }
        return n;
    }
    if (kind == MENU_STATUS) {
        u8 cur = presence_me();
        static char st[4][48];
        static const char *const nm[4] = { "В сети", "Не активен", "Не беспокоить", "Невидимый" };
        static const u8 ps[4] = { PS_ONLINE, PS_IDLE, PS_DND, PS_INVISIBLE };
        for (int i = 0; i < 4; i++) {
            snprintf(st[i], sizeof st[i], "%s %s", cur == ps[i] ? "*" : " ", tr(nm[i]));
            ids[n] = MI_ST_ON + i; danger[n] = false; names[n++] = st[i];
        }
        ids[n] = MI_STATUS_SET; danger[n] = false; names[n++] = TR("Статус на время и текст...");
        ids[n] = MI_FOCUS30; danger[n] = false; names[n++] = TR("Режим фокуса: 30 мин");
        ids[n] = MI_FOCUS60; danger[n] = false; names[n++] = TR("Режим фокуса: 1 ч");
        ids[n] = MI_FOCUS120; danger[n] = false; names[n++] = TR("Режим фокуса: 2 ч");
        if (focus_until()) { ids[n] = MI_FOCUSOFF; danger[n] = false; names[n++] = TR("Выключить режим фокуса"); }
        return n;
    }
    if (kind == MENU_FRIEND) {
        const Relation *r = rel_find(U.menu_uid);
        if (r && r->type == REL_BLOCKED) { ids[n] = MI_FR_UNBLOCK; danger[n] = false; names[n++] = TR("Разблокировать     Enter"); return n; }
        ids[n] = MI_FR_MSG; danger[n] = false; names[n++] = TR("Написать           Enter");
        ids[n] = MI_FR_DEL; danger[n] = true; names[n++] = TR("Удалить из друзей    Del");
        return n;
    }
#ifdef MC_PACKA
    if (m && m->state != MS_SENT) {              /* ещё не на сервере: только повтор, отмена, копия */
        if (m->state == MS_FAILED) { ids[n] = MI_MRETRY; danger[n] = false; names[n++] = TR("Повторить отправку"); }
        ids[n] = MI_MDISCARD; danger[n] = true; names[n++] = m->state == MS_FAILED ? TR("Удалить неотправленное") : TR("Отменить отправку");
        ids[n] = MI_COPY; danger[n] = false; names[n++] = TR("Копировать текст");
        return n;
    }
#endif
    ids[n] = MI_REPLY; danger[n] = false; names[n++] = TR("Ответить          r");
    if (m && m->author_id == S.me_id) { ids[n] = MI_EDIT; danger[n] = false; names[n++] = TR("Изменить          e"); }
    ids[n] = MI_REACT; danger[n] = false; names[n++] = TR("Добавить реакцию  +");
#ifdef MC_WAVE1
#ifdef MC_RAW
    ids[n] = MI_PIN; danger[n] = false; names[n++] = m && m->pinned ? TR("Открепить сообщение") : TR("Закрепить сообщение");
#else
    ids[n] = MI_PIN; danger[n] = false; names[n++] = TR("Закрепить сообщение");
#endif
    ids[n] = MI_UNREAD; danger[n] = false; names[n++] = TR("Отметить непрочитанным");
#endif
    {
        const Channel *oc = chan_find(S.open_ch);
        if (oc && oc->guild_id && (oc->type == CH_TEXT || oc->type == CH_NEWS)) { ids[n] = MI_THREAD; danger[n] = false; names[n++] = TR("Создать ветку"); }
    }
    ids[n] = MI_FORWARD; danger[n] = false; names[n++] = TR("Переслать");
    if (m && m->nreacts) { ids[n] = MI_REACTS; danger[n] = false; names[n++] = TR("Просмотр реакций"); }
    ids[n] = MI_COPY; danger[n] = false; names[n++] = TR("Копировать текст");
    ids[n] = MI_COPYLINK; danger[n] = false; names[n++] = TR("Копировать ссылку на сообщение");
    ids[n] = MI_COPYID; danger[n] = false; names[n++] = TR("Копировать ID сообщения");
    if (m && m->author_id == S.me_id) { ids[n] = MI_DELETE; danger[n] = true; names[n++] = TR("Удалить сообщение d"); }
    return n;
}
static void draw_menu(int ax, int ay, int aw, int ah) {
    Message *m = U.menu_kind == MENU_MSG ? msg_find(U.menu_msg, NULL) : NULL;
    if (U.menu_kind == MENU_MSG && !m) { U.menu = false; return; }
    int ids[16];
    const char *nm[16];
    bool dg[16];
    int n = menu_items(U.menu_kind, m, ids, nm, dg), iw = 0;
    for (int i = 0; i < n; i++) if (vw(nm[i]) > iw) iw = vw(nm[i]);
    int bw = iw + 6, bh = n + 2;
    int x0 = U.menu_x, y0 = U.menu_kind == MENU_SET || U.menu_kind == MENU_STATUS ? U.menu_y - bh : U.menu_y;
    if (x0 + bw > ax + aw) x0 = ax + aw - bw;
    if (x0 < ax) x0 = ax;
    if (y0 + bh > ay + ah) y0 = ay + ah - bh;
    if (y0 < ay) y0 = ay;
    if (U.menu_sel >= n) U.menu_sel = n - 1;
    ovl_cut(x0, y0, bw, bh);
    u32 ut = bg_at(x0, y0), ub = bg_at(x0, y0 + bh - 1);
    g_rect(x0, y0, bw, bh, C_MENU);
    g_round_corners(x0, y0, bw, bh, 0, ut);
    g_round_corners(x0, y0 + bh - 1, bw, bh, bh - 1, ub);
    hit_add(x0, y0, bw, bh, A_BLOCK, 0, 0, 0);
    for (int i = 0; i < n; i++) {
        int y = y0 + 1 + i;
        if (i == U.menu_sel) g_pill(x0 + 1, y, bw - 2, dg[i] ? C_DANGER : C_ACC);
        g_text(x0 + 3, y, nm[i], iw, i == U.menu_sel ? C_WHITE : dg[i] ? C_RED : C_TEXT, 0);
        hit_add(x0 + 1, y, bw - 2, 1, A_MENU, (u8)ids[i], 0, dg[i] ? HV_RED : HV_ACC);
    }
}

/* ---- экран настроек пользователя (шестерёнка в панели пользователя) ---- */
static const char *const SET_SEC[] = { "Моя учётная запись", "Конфиденциальность", "Внешний вид", "Голос и видео", "Уведомления", "Горячие клавиши", "Производительность", "Обновления" };
enum { SS_ACC, SS_PRIV, SS_LOOK, SS_VOICE, SS_NOTIF, SS_KEYS, SS_PERF, SS_UPD, NSEC };
_Static_assert(sizeof SET_SEC / sizeof *SET_SEC == NSEC, "SET_SEC и SS_* разошлись");
/* №140/146: устройства ввода/вывода — список читается при входе в «Голос и видео» (не на каждый кадр);
 * тест микрофона открыт только пока открыт этот раздел (ui_tick закрывает при уходе). */
static struct { bool got; int n[2]; char nm[2][12][96]; } DEV;
static bool mic_test_on;
static void dev_step(int out, int d) {
    if (!DEV.got || DEV.n[out] < 0) return;
    const char *cur = voice_device(out);
    int i = -1;                                  /* -1 — «По умолчанию» */
    for (int k = 0; k < DEV.n[out]; k++) if (cur && !strcmp(cur, DEV.nm[out][k])) i = k;
    i += d;
    if (i < -1) i = DEV.n[out] - 1;
    if (i >= DEV.n[out]) i = -1;
    voice_set_device(out, i < 0 ? "" : DEV.nm[out][i]);
    U.dirty = true;
}
/* Поле выбора устройства как раскрывающийся список Discord: [<] имя [>], щелчок по имени — следующее. */
static void dev_field(int x, int y, int w, int out) {
    const char *cur = voice_device(out);
    bool none = DEV.n[out] < 0;
    g_text(x, y, out ? TR("УСТРОЙСТВО ВЫВОДА") : TR("УСТРОЙСТВО ВВОДА"), w, C_MUTED, AT_B);
    if (w < 12) return;
    g_rect(x, y + 1, w, 1, C_RAIL);
    if (none) { g_texte(x + 1, y + 1, TR("звук недоступен"), w - 2, C_MUTED, 0); return; }
    g_text(x + 1, y + 1, "<", 1, C_TEXT, AT_B);
    g_text(x + w - 2, y + 1, ">", 1, C_TEXT, AT_B);
    g_texte(x + 3, y + 1, cur && *cur ? (streamer_on() ? TR("выбрано (скрыто)") : cur) : TR("По умолчанию"), w - 6, C_WHITE, 0);
    hit_add(x, y + 1, 3, 1, A_DEV, out, -1, HV_BG);
    hit_add(x + 3, y + 1, w - 6, 1, A_DEV, out, 1, HV_BG);
    hit_add(x + w - 3, y + 1, 3, 1, A_DEV, out, 1, HV_BG);
}
/* №16 полный экран окна: Alt+Enter и кнопка в «Внешний вид»; в консоли — средствами терминала. */
#ifdef ASTRA_VIDEO
/* Демонстрация экрана (видео-версия). Пока идёт: режим стримера включён, индикатор в строке состояния,
 * автостоп — выход из звонка, смена канала звонка, закрытие окна-источника, блокировка экрана, паника.
 * Своё окно исключено из захвата всегда (term_capture_exclude с запуска). Кадры никуда не пишутся. */
static char sh_t[24][80];
static const char *sh_names[25];
static uintptr_t sh_ids[24];
static int sh_n;
static const char *const SH_MODES[] = { "720p 30 к/с, 1,8 Мбит/с", "720p 30 к/с, экономный поток 1 Мбит/с" };
static void share_ui_stop(const char *why) {
    if (!sh_running) return;
    golive_stop();
    share_stop();
    sh_running = false;
    sh_gl_wait = false;
    GL_phase = GL_prev = GL_OFF;
    streamer_set(sh_prev_streamer);
    if (why && *why) ui_notice("%s", tr(why));
    U.dirty = true;
}
#endif
static bool full_toggle(void) {
    int r = term_fullscreen(-1);
    if (r < 0) return false;
    ui_notice(r ? TR("во весь экран (Alt+Enter - вернуть)") : TR("обычное окно"));
    U.full = U.dirty = true;
    return true;
}
static bool ptt_capture;                         /* №141: ждём клавишу рации — следующее событие окна */
/* Виртуальная клавиша Windows для события (разработчик может дать точную — plat_key_vk в plat_win.c);
 * здесь — запасной перевод: F1..F12, буквы, цифры. 0 — не годится. */
int plat_key_vk(const Key *k) __attribute__((weak));
int plat_key_vk(const Key *k) {
    if (k->code >= K_F1 && k->code <= K_F12) return 0x70 + (k->code - K_F1);
    if (k->code == K_CHAR && k->ch >= 'a' && k->ch <= 'z') return (int)(k->ch - 'a' + 'A');
    if (k->code == K_CHAR && k->ch >= 'A' && k->ch <= 'Z') return (int)k->ch;
    if (k->code == K_CHAR && k->ch >= '0' && k->ch <= '9') return (int)k->ch;
    return 0;
}
/* Масштаб окна (№15): Ctrl+= / Ctrl+- / Ctrl+0 и кнопки в «Внешний вид». */
static void zoom_step(int step) {
    if (term_zoom(step)) ui_notice(TR("Масштаб %d %%"), term_zoom_pct());
    else if (term_subcell_scale() <= 0) ui_notice(TR("в консоли масштаб меняет сам терминал"));
    else if (step) ui_notice(TR("Масштаб %d %% - это предел"), term_zoom_pct());
    else ui_notice(TR("Масштаб уже %d %%"), term_zoom_pct());
    U.full = U.dirty = true;
}
/* Подпись и значение с переносом; вернуть занятые строки + 1 (промежуток). */
static int set_row(int x, int y, int w, const char *k, const char *v, u32 vfg) {
    g_text(x, y, k, w, C_MUTED, AT_B);
    int n = 1;
    if (v) {
        char **ls;
        int m = help_wrap(v, w, &ls);
        for (int i = 0; i < m; i++) { g_text(x, y + n++, ls[i], w, vfg, 0); free(ls[i]); }
        free(ls);
    }
    return n + 1;
}
static bool startup_direct(void);
static void startup_direct_set(bool on);
static void draw_settings(int W, int H) {
    VoiceStatus vs;
    voice_status(&vs);
    int ls = W * 30 / 100;
    if (ls < 22) ls = 22;
    g_rect(0, 0, ls, H, C_SIDE);
    g_rect(ls, 0, W - ls, H, C_FEED);
    /* разделы справа в левой колонке, как в Discord */
    int lw = 22, lx = ls - lw - 2;
    if (lx < 1) lx = 1;
    g_text(lx + 1, 1, TR("НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ"), lw, C_MUTED, AT_B);
    for (int i = 0; i < NSEC; i++) {
        int y = 2 + i;
        if (i == U.set_sec) g_pill(lx, y, lw, C_SEL);
        g_text(lx + 1, y, tr(SET_SEC[i]), lw - 2, i == U.set_sec ? C_WHITE : C_MUTED, i == U.set_sec ? AT_B : 0);
        hit_add(lx, y, lw, 1, A_SETSEC, 0, i, HV_PILL);
    }
    for (int i = lx + 1; i < lx + lw - 1; i++) g_ch(i, 3 + NSEC, '-', 0x3f4147u, 0);
    g_text(lx + 1, 4 + NSEC, TR("Выйти из Astra DS"), lw - 2, C_RED, 0);
    hit_add(lx, 4 + NSEC, lw, 1, A_BTN, B_QUIT, 0, HV_PILL);
    /* закрыть: круглая кнопка x и подпись ESC */
    int cxb = W - 6;
    g_circle_row(cxb, 1, 4, 2, 0, C_BTN2, "x", C_WHITE);
    g_circle_row(cxb, 2, 4, 2, 1, C_BTN2, NULL, C_WHITE);
    g_ctext(cxb, 3, 4, "ESC", C_MUTED, AT_B);
    hit_add(cxb, 1, 4, 3, A_BTN, B_SET_CLOSE, 0, HV_BG);
    int x = ls + 3, w = W - ls - 12, y = 1;
    if (w > 70) w = 70;
    g_text(x, y, tr(SET_SEC[U.set_sec]), w, C_WHITE, AT_B);
    y += 2;
    char t[200];
    switch (U.set_sec) {
    case SS_ACC: {
        /* карточка профиля: баннер, круглый аватар, имя */
        g_rect(x, y, w, 2, av_color(S.me_id));
        g_round_corners(x, y, w, 9, 0, C_FEED);
        g_rect(x, y + 2, w, 7, C_RAIL);
        g_round_corners(x, y + 8, w, 9, 8, C_FEED);
        const char *me = S.me_name && *S.me_name ? S.me_name : TR("вы");
        for (int r = 0; r < 3; r++) g_avatar(x + 2, y + 2 + r, 6, 3, r, S.me_id, me);
        g_text(x + 10, y + 3, me, w - 12, C_WHITE, AT_B);
        snprintf(t, sizeof t, "%s", O.invisible ? TR("Невидимый") : TR("В сети"));
        set_row(x + 2, y + 6, w - 4, TR("СТАТУС"), t, C_TEXT);
        y += 10;
        snprintf(t, sizeof t, TR("хранится в системном хранилище (%s), на диск не пишется"), keyring_name());
        y += set_row(x, y, w, TR("ТОКЕН"), t, C_TEXT);
        y += set_row(x, y, w, TR("ДАННЫЕ"), TR("сообщения не кэшируются, журналов нет, список участников сервера не загружается"), C_TEXT);
        g_button(x, y, TR("Изменить профиль"), w, 1, B_MEPROF);   /* №113 */
        break;
    }
    case SS_VOICE: {
        if (!DEV.got) {
            for (int o = 0; o < 2; o++) DEV.n[o] = audio_devices(o, DEV.nm[o], 12);
            DEV.got = true;
        }
        if (w >= 50) {                           /* как в Discord: ввод и вывод рядом */
            int hw = (w - 2) / 2;
            dev_field(x, y, hw, 0);
            dev_field(x + hw + 2, y, w - hw - 2, 1);
            y += 3;
        } else {
            dev_field(x, y, w, 0);
            dev_field(x, y + 3, w, 1);
            y += 6;
        }
        g_text(x, y, TR("ПРОВЕРКА МИКРОФОНА"), w, C_MUTED, AT_B);
        int u = g_button(x, y + 1, mic_test_on ? TR("Остановить") : TR("Проверить микрофон"), w, mic_test_on ? 2 : 1, B_MICTEST);
        int bx = x + u + 2, bw = w - u - 2;
        if (bw > 40) bw = 40;
        int lv = mic_test_on ? voice_mic_level() : -1;
        if (mic_test_on && lv < 0) g_texte(bx, y + 1, TR("микрофон не открылся"), w - u - 2, C_RED, 0);
        else if (bw >= 10) {
            /* полоса уровня -60..0 дБ; золотая черта — порог активации голосом */
            int fill = lv < 0 ? 0 : lv * bw / 100, thr = (voice_vad_db() + 60) * bw / 60;
            if (thr < 0) thr = 0;
            if (thr > bw - 1) thr = bw - 1;
            bool loud = lv >= 0 && fill > thr;
            for (int i = 0; i < bw; i++)
                g_ch(bx + i, y + 1, i == thr ? '|' : i < fill ? '#' : '-', i == thr ? C_GOLD : i < fill ? (loud ? C_GREEN : C_TEXT) : C_LINE, i < fill || i == thr ? AT_B : 0);
        }
        y += 3;
        snprintf(t, sizeof t, TR("ГРОМКОСТЬ МИКРОФОНА: %d %%"), voice_mic_gain());
        g_text(x, y, t, w, C_MUTED, AT_B);
        u = g_button(x, y + 1, TR("-10 %"), w, 0, B_MICG_DN);
        u += 1 + g_button(x + u + 1, y + 1, TR("+10 %"), w - u - 1, 0, B_MICG_UP);
        if (voice_mic_gain() != 100) u += 1 + g_button(x + u + 1, y + 1, TR("100 %"), w - u - 1, 0, B_MICG_0);
        if (voice_mic_gain() > 100) g_texte(x + u + 2, y + 1, TR("громкие места мягко ограничиваются"), w - u - 2, C_MUTED, 0);
        y += 3;
        g_text(x, y, TR("МИКРОФОН И ЗВУК"), w, C_MUTED, AT_B);
        u = g_button(x, y + 1, vs.self_mute ? TR("Микрофон выкл (F5)") : TR("Микрофон вкл (F5)"), w, vs.self_mute ? 2 : 0, B_MIC);
        g_button(x + u + 1, y + 1, vs.self_deaf ? TR("Звук выкл (F6)") : TR("Звук вкл (F6)"), w - u - 1, vs.self_deaf ? 2 : 0, B_DEAF);
        y += 3;
        g_text(x, y, TR("КЛАВИША РАЦИИ"), w, C_MUTED, AT_B);
#ifdef _WIN32
        {
            int vk = voice_ptt_bound();
            char kn[48] = "";
            if (vk) plat_key_name(vk, kn, sizeof kn);
            if (ptt_capture) snprintf(t, sizeof t, TR("нажмите клавишу или боковую кнопку мыши (Esc - отмена)"));
            else if (vk) snprintf(t, sizeof t, "%s", kn[0] ? kn : TR("задана"));
            else snprintf(t, sizeof t, TR("F8 (с --ptt)"));
            u = g_button(x, y + 1, ptt_capture ? TR("Отмена") : TR("Назначить"), w, 0, ptt_capture ? B_PTT_CLR : B_PTT_SET);
            if (vk && !ptt_capture) u += 1 + g_button(x + u + 1, y + 1, TR("Сбросить"), w - u - 1, 0, B_PTT_CLR);
            int tu = g_texte(x + u + 2, y + 1, t, w - u - 2, ptt_capture ? C_GOLD : C_WHITE, AT_B);
            bool global = vk == 0x05 || vk == 0x06 || (vk >= 0x7C && vk <= 0x87) || vk == 0x13 || vk == 0x91;
            if (vk && !global && !ptt_capture) g_texte(x + u + 4 + tu, y + 1, TR("работает только когда окно активно"), w - u - 4 - tu, C_MUTED, 0);
        }
#else
        g_texte(x, y + 1, TR("F8 с --ptt (выбор клавиши на Linux недоступен)"), w, C_TEXT, 0);
#endif
        y += 3;
        bool va = voice_vad_is_auto();
        snprintf(t, sizeof t, TR("ПОРОГ АКТИВАЦИИ ГОЛОСОМ: %s%d дБ%s"), va ? TR("авто, сейчас ") : "", voice_vad_db(), O.ptt ? TR(" (рация --ptt)") : "");
        g_text(x, y, t, w, C_MUTED, AT_B);
        u = g_button(x, y + 1, va ? TR("[x] Автоматически") : TR("[ ] Автоматически"), w, va ? 1 : 0, B_VADAUTO);
        if (!va) {
            u += 1 + g_button(x + u + 1, y + 1, TR("-3 дБ (F9)"), w - u - 1, 0, B_VADDN);
            g_button(x + u + 1, y + 1, TR("+3 дБ (F10)"), w - u - 1, 0, B_VADUP);
        }
        y += 3;
        g_text(x, y, TR("ЗВУКИ И ФИЛЬТР"), w, C_MUTED, AT_B);
        u = g_button(x, y + 1, voice_hpf_on() ? TR("[x] Фильтр гула (ниже 90 Гц)") : TR("[ ] Фильтр гула (ниже 90 Гц)"), w, voice_hpf_on() ? 1 : 0, B_HPF);
        g_button(x + u + 1, y + 1, sounds_enabled() ? TR("[x] Звуки звонка и событий") : TR("[ ] Звуки звонка и событий"), w - u - 1, sounds_enabled() ? 1 : 0, B_SOUNDS);
        g_button(x, y + 2, voice_duck_others_on() ? TR("[x] Приглушать другие программы во время звонка") : TR("[ ] Приглушать другие программы во время звонка"),
                 w, voice_duck_others_on() ? 1 : 0, B_DUCK);
        y += 4;
        {   /* шумоподавление микрофона: выбор как переключатель, на ходу */
            static const char *const nsl[3] = { "Выкл", "SpeexDSP (легче)", "RNNoise (лучше)" };
            int nm = voice_ns_mode(), cx = x, cy = y + 1;
            g_text(x, y, TR("ШУМОПОДАВЛЕНИЕ"), w, C_MUTED, AT_B);
            for (int i = 0; i < 3; i++) {
                char b[64];
                snprintf(b, sizeof b, "%s %s", nm == i ? "(o)" : "( )", tr(nsl[i]));
                int bl = vw(b) + 4;
                if (cx > x && cx + bl > x + w) { cx = x; cy++; }
                cx += g_button(cx, cy, b, x + w - cx, nm == i ? 1 : 0, B_NS_OFF + i) + 1;
            }
            y = cy + 2;
        }
        VoiceFx fx = voicefx_get();
        g_text(x, y, TR("СМЕНА ГОЛОСА"), w, C_MUTED, AT_B);
        snprintf(t, sizeof t, TR("Голос: %s (F11)"), fx == VFX_LOW ? TR("ниже") : fx == VFX_HIGH ? TR("выше") : TR("как есть"));
        g_button(x, y + 1, t, w, fx != VFX_OFF ? 1 : 0, B_FX);
        break;
    }
    case SS_LOOK: {
        g_text(x, y, "ЯЗЫК / LANGUAGE", w, C_MUTED, AT_B);
        g_button(x, y + 1, g_lang_en ? "English  (Русский)" : "Русский  (English)", w, 0, B_LANG);
        y += 3;
        y += set_row(x, y, w, TR("ТЕМА"), TR("тёмная"), C_TEXT);
        g_text(x, y, TR("КАРТИНКИ"), w, C_MUTED, AT_B);   /* №71 */
        g_button(x, y + 1, img_click ? TR("[x] Загружать только по щелчку") : TR("[ ] Загружать только по щелчку"), w, img_click ? 1 : 0, B_IMGCLICK);
        y += 3;
        if (term_subcell_scale() > 0) {          /* №16 */
            g_text(x, y, TR("ОКНО"), w, C_MUTED, AT_B);
            g_button(x, y + 1, TR("Во весь экран (Alt+Enter)"), w, 0, B_FULL);
            y += 3;
        }
        if (term_subcell_scale() <= 0) {
            set_row(x, y, w, TR("МАСШТАБ"), TR("в консоли размер шрифта меняет сам терминал"), C_TEXT);
            break;
        }
        snprintf(t, sizeof t, TR("МАСШТАБ: %d %%"), term_zoom_pct());
        g_text(x, y, t, w, C_MUTED, AT_B);
        int u = g_button(x, y + 1, "- (Ctrl+-)", w, 0, B_ZOOM_DN);
        u += 1 + g_button(x + u + 1, y + 1, "100 % (Ctrl+0)", w - u - 1, term_zoom_pct() == 100 ? 1 : 0, B_ZOOM_0);
        g_button(x + u + 1, y + 1, "+ (Ctrl+=)", w - u - 1, 0, B_ZOOM_UP);
        break;
    }
    case SS_UPD: {                               /* автообновление (update.c): только по явной кнопке */
        const UpdateState *us = update_state();
        snprintf(t, sizeof t, "%s %s", APP_NAME, ASTRA_VERSION);
        y += set_row(x, y, w, TR("ТЕКУЩАЯ ВЕРСИЯ"), t, C_TEXT);
        g_button(x, y, update_auto() ? TR("[x] Проверять обновления при запуске") : TR("[ ] Проверять обновления при запуске"), w, update_auto() ? 1 : 0, B_UP_AUTO);
        y += 2;
        y += lg_para(x, y, w, TR("Раз в сутки, на GitHub; скачивание и замена - только по кнопке 'Обновить'."), C_MUTED, false) + 1;
        int u = g_button(x, y, TR("Проверить сейчас"), w, 0, B_UP_CHECK);
        const char *st2 = "";
        u32 sc = C_MUTED;
        if (us) switch (us->phase) {
        case UP_CHECKING: st2 = TR("проверка..."); break;
        case UP_NONE: st2 = TR("у вас последняя версия"); sc = C_GREEN; break;
        case UP_AVAILABLE: snprintf(t, sizeof t, TR("доступна версия %s"), us->version); st2 = t; sc = C_GOLD; break;
        case UP_DOWNLOADING: st2 = TR("загрузка..."); break;
        case UP_READY: st2 = TR("готово - нужен перезапуск"); sc = C_GREEN; break;
        case UP_FAILED: st2 = us->err[0] ? us->err : TR("не получилось"); sc = C_RED; break;
        default: break;
        }
        if (*st2) { char *e = san(st2, false); g_texte(x + u + 2, y, e, w - u - 2, sc, 0); free(e); }
        if (us && (us->phase == UP_AVAILABLE || us->phase == UP_READY)) g_button(x, y + 2, us->phase == UP_READY ? TR("Перезапустить") : TR("Обновить"), w, 1, us->phase == UP_READY ? B_UP_RESTART : B_UP_APPLY);
        break;
    }
    case SS_PERF: {                              /* №153: сколько занимает программа сейчас */
        snprintf(t, sizeof t, TR("%.1f МБ (частная память процесса)"), (double)plat_private_bytes() / (1024.0 * 1024.0));
        y += set_row(x, y, w, TR("ПАМЯТЬ"), t, C_TEXT);
        snprintf(t, sizeof t, TR("картинки на экране %zu КБ; сетка %dx%d клеток %zu КБ; строк ленты %d"),
                 img_cache_bytes() / 1024, U.gw, U.gh, (size_t)U.gw * (size_t)U.gh * sizeof(Cell) / 1024, U.mlcap);
        y += set_row(x, y, w, TR("ИНТЕРФЕЙС"), t, C_TEXT);
        if (g_gw_ping > 0) snprintf(t, sizeof t, TR("%d мс"), g_gw_ping); else snprintf(t, sizeof t, TR("нет данных"));
        y += set_row(x, y, w, TR("ПИНГ ШЛЮЗА"), t, g_gw_ping <= 0 ? C_MUTED : g_gw_ping < 100 ? C_GREEN : g_gw_ping < 250 ? C_GOLD : C_RED);
        if (vless_state() == VL_UP) { snprintf(t, sizeof t, TR("%d мс"), vless_latency_ms()); y += set_row(x, y, w, "VLESS", t, C_TEXT); }
        {   /* шумоподавление и поток звонка: доля одного ядра и память (только в звонке) */
            VoiceStatus pv;
            voice_status(&pv);
            static const char *const nsn[3] = { "выкл", "SpeexDSP", "RNNoise" };
            int nm = voice_ns_mode();
            if (nm < 0 || nm > 2) nm = 0;
            if (pv.phase == VS_IDLE) snprintf(t, sizeof t, TR("%s; не в звонке"), tr(nsn[nm]));
            else {
                VoiceStats vst2;
                voice_stats(&vst2);
                if (nm == NS_OFF) snprintf(t, sizeof t, "%s", tr(nsn[0]));
                else if (!vst2.ns_mode) snprintf(t, sizeof t, TR("%s; не работает (микрофон молчит или заглушён)"), tr(nsn[nm]));
                else snprintf(t, sizeof t, TR("%s; %.1f%% ядра, %d КБ"), tr(nsn[nm]), vst2.ns_cpu_pm / 10.0, vst2.ns_mem_kb);
                y += set_row(x, y, w, TR("ШУМОПОДАВЛЕНИЕ"), t, C_TEXT);
                snprintf(t, sizeof t, TR("%.1f%% ядра"), vst2.cap_cpu_pm / 10.0);
                y += set_row(x, y, w, TR("ПОТОК ЗВОНКА"), t, C_TEXT);
                t[0] = 0;
            }
            if (t[0]) y += set_row(x, y, w, TR("ШУМОПОДАВЛЕНИЕ"), t, C_MUTED);
        }
        g_button(x, y, show_ram ? TR("[x] Память в строке состояния") : TR("[ ] Память в строке состояния"), w, show_ram ? 1 : 0, B_RAMBAR);
        break;
    }
    case SS_NOTIF: {
        {   /* №123: уведомления системы — выкл / без текста (только «новое сообщение / упоминание») / с текстом */
            static const char *const nmode[] = { "выкл", "без текста", "с текстом" };
            int nm = notify_mode();
            if (nm < 0 || nm > 2) nm = 1;
            snprintf(t, sizeof t, TR("Показывать уведомления: %s"), tr(nmode[nm]));
            g_text(x, y, TR("УВЕДОМЛЕНИЯ СИСТЕМЫ"), w, C_MUTED, AT_B);
            g_button(x, y + 1, t, w, nm == 2 ? 2 : 0, B_NOTIFY);
            y += 2;
            if (nm == 2) { g_texte(x, y, TR("Windows (и Linux) сохраняют уведомления в своей истории на диске - текст ЛС останется там."), w, C_GOLD, 0); y++; }
            else if (nm == 1) { g_texte(x, y, TR("Без имён и текста: только 'новое сообщение' / 'упоминание'."), w, C_MUTED, 0); y++; }
            y++;
        }
        const char *aw = alert_words();
        bool sm = streamer_on();
        y += set_row(x, y, w, TR("СЛОВА-ТРЕВОГИ"), sm && aw && *aw ? TR("скрыто (режим стримера)") : aw && *aw ? aw : TR("не заданы"), aw && *aw ? C_TEXT : C_MUTED);
        g_texte(x, y - 1, TR("Сообщение с таким словом подсвечивается и считается упоминанием."), w, C_MUTED, 0);
        g_button(x, y + 1, TR("Изменить слова"), w, 1, B_ALERTS);
        y += 3;
        const char *fw = filter_words();         /* №120 */
        y += set_row(x, y, w, TR("ФИЛЬТР СЛОВ"), sm && fw && *fw ? TR("скрыто (режим стримера)") : fw && *fw ? fw : TR("не задан"), fw && *fw ? C_TEXT : C_MUTED);
        g_texte(x, y - 1, TR("Сообщение с таким словом сворачивается: 'скрыто фильтром [показать]'."), w, C_MUTED, 0);
        g_button(x, y + 1, TR("Изменить фильтр"), w, 1, B_FILTER);
        break;
    }
    case SS_PRIV: {
        VlessState vst = vless_state();
        int ms = vless_latency_ms();
        if (vst == VL_UP) snprintf(t, sizeof t, TR("работает: весь трафик через туннель, %d мс"), ms);
        else if (vst == VL_STARTING) snprintf(t, sizeof t, TR("запускается..."));
        else if (vst == VL_DOWN) snprintf(t, sizeof t, TR("нет связи - клиент напрямую не ходит"));
        else snprintf(t, sizeof t, TR("выключен: трафик идёт напрямую"));
        y += set_row(x, y, w, "VLESS", t, vst == VL_DOWN ? C_RED : vst == VL_UP ? C_GREEN : C_TEXT);
        int u = g_button(x, y, TR("Сменить ссылку (C)"), w, 1, B_VL_C);
        if (vst != VL_OFF) {
            u += 1 + g_button(x + u + 1, y, TR("Переподключиться (R)"), w - u - 1, 0, B_VL_R);
            g_button(x + u + 1, y, TR("Отключить (D)"), w - u - 1, 2, B_VL_D);
        }
        y += 2;
        g_text(x, y, TR("ПРИ ЗАПУСКЕ"), w, C_MUTED, AT_B);
        g_button(x, y + 1, startup_direct() ? TR("[ ] Спрашивать про VLESS при запуске") : TR("[x] Спрашивать про VLESS при запуске"), w, 0, B_STARTASK);
        y += 3;
        g_text(x, y, TR("РЕЖИМ СТРИМЕРА"), w, C_MUTED, AT_B);
        g_button(x, y + 1, streamer_on() ? TR("[x] Скрывать личное на экране") : TR("[ ] Скрывать личное на экране"), w, streamer_on() ? 1 : 0, B_STREAMER);
        y += 3;
        const char *pn = panic_key_name();
        if (pn && *pn && streamer_on()) snprintf(t, sizeof t, TR("задан (скрыто: режим стримера)"));
        else if (pn && *pn) snprintf(t, sizeof t, TR("%s x3: стереть токен и ссылку VLESS и выйти"), pn);
        else snprintf(t, sizeof t, TR("не задан (флаг --panic=КЛАВИША, например F12)"));
        set_row(x, y, w, TR("ПАНИК-КЕЙ"), t, C_TEXT);
        break;
    }
    default: {
        HLine *hl;
        int kw, nhl = help_build(w, &hl, &kw);
        for (int i = 0; i < nhl && y < H - 1; i++, y++) {
            g_text(x, y, hl[i].t, w, hl[i].kind ? C_WHITE : C_MUTED, hl[i].kind ? AT_B : 0);
            if (!hl[i].kind && kw) g_fgset(x, y, kw < w ? kw : w, C_TEXT, AT_B);
        }
        for (int i = 0; i < nhl; i++) free(hl[i].t);
        free(hl);
        break;
    }
    }
}

/* ---- колонка участников, пока списка сервера нет (нет шлюза, ещё не пришёл): кто писал в открытом канале ---- */
static void draw_members_chat(int x, int w, int H, u64 gid) {
    u64 ids[48];
    const char *nm[48];
    u32 col[48];
    int n = 0;
    for (int i = S.nmsgs - 1; i >= 0 && n < 48; i--) {
        const Message *m = &S.msgs[i];
        bool seen = false;
        for (int k = 0; k < n; k++) if (ids[k] == m->author_id) seen = true;
        if (seen) continue;
        ids[n] = m->author_id; nm[n] = m->author ? m->author : "?"; col[n] = m->color; n++;
    }
    bool me = false;
    for (int k = 0; k < n; k++) if (ids[k] == S.me_id) me = true;
    if (!me && n < 48) { ids[n] = S.me_id; nm[n] = S.me_name ? S.me_name : TR("вы"); col[n] = user_color(gid, S.me_id); n++; }
    for (int i = 1; i < n; i++)                  /* по имени, как в Discord */
        for (int j = i; j > 0 && strcmp(nm[j - 1], nm[j]) > 0; j--) {
            u64 a = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = a;
            const char *b = nm[j]; nm[j] = nm[j - 1]; nm[j - 1] = b;
            u32 c = col[j]; col[j] = col[j - 1]; col[j - 1] = c;
        }
    char t[64];
    snprintf(t, sizeof t, TR("В ЧАТЕ - %d"), n);
    g_text(x + 2, 2, t, w - 3, C_MUTED, AT_B);
    int y = 3;
    for (int i = 0; i < n && y < H - 1; i++, y++) {
        if (y == H - 2 && i < n - 1) {
            snprintf(t, sizeof t, TR("и ещё %d"), n - i);
            g_text(x + 2, y, t, w - 3, C_MUTED, 0);
            break;
        }
        g_listav(x + 2, y, ids[i], nm[i], G_MEMB);
        g_texte(x + 7, y, nm[i], w - 8, col[i] ? col[i] : C_TEXT, 0);
        hit_add(x + 1, y, w - 2, 1, A_NONE, 0, 0, 0);
    }
}
/* ---- список участников сервера, как в Discord: группы ролей (hoist), «В сети», «Не в сети»; у участника
 * аватар 4x2 с точкой статуса, ник цветом роли, метка БОТ, под ником свой статус или игра. Строка — 2 клетки
 * в высоту. Колесо листает; store держит и подписывает только окна по 100 вокруг видимого (ml_view). ---- */
static struct { u64 cid; int top; } MB;
static u32 status_col(int st);
static void draw_members(int x, int w, int H, u64 gid, u64 cid) {
    g_rect(x, 1, w, H - 1, C_SIDE);
    int total = ml_total(cid);
    if (total < 0) { draw_members_chat(x, w, H, gid); return; }
    if (MB.cid != cid) { MB.cid = cid; MB.top = 0; }
    int y0 = 1, rows = (H - 1 - y0) / 2;
    if (rows < 1) return;
    if (MB.top > total - rows) MB.top = total - rows;
    if (MB.top < 0) MB.top = 0;
    hit_add(x, 1, w, H - 1, A_AR_MEMB, 0, 0, 0);
    ml_view(MB.top, MB.top + rows - 1);
    char t[120];
    for (int k = 0; k < rows && MB.top + k < total; k++) {
        int i = MB.top + k, y = y0 + 2 * k;
        const MemRow *r = ml_row(i);
        if (!r) {                                /* ещё не пришла: серая заглушка, как у Discord */
            for (int r2 = 0; r2 < 2; r2++) g_circle_row(x + 1, y + r2, 4, 2, r2, C_PH, NULL, C_WHITE);
            g_text(x + 6, y, "......", w - 7, C_PH, 0);
            continue;
        }
        if (r->kind == MLR_GROUP) {
            const char *gn = r->id == ML_G_ONLINE ? TR("В сети") : r->id == ML_G_OFFLINE ? TR("Не в сети") :
                             r->name[0] ? r->name : TR("Роль");
            snprintf(t, sizeof t, "%s - %d", gn, r->count);
            g_texte(x + 2, y + 1, t, w - 3, C_MUTED, AT_B);
            continue;
        }
        u64 uid = r->id;
        bool off = r->status == PS_OFFLINE;
        u8 st = r->status, act = r->act;
        bool bot = r->bot;
        u32 col = off ? C_MUTED : r->color ? r->color : C_TEXT;
        char nm[80], tx[80];
        snprintf(nm, sizeof nm, "%s", r->name[0] ? r->name : "?");
        snprintf(tx, sizeof tx, "%s", r->text);
        for (int r2 = 0; r2 < 2; r2++) { g_avatar(x + 1, y + r2, 4, 2, r2, uid, NULL); strip_part(G_MEMB, x + 1, y + r2, 4, 2, r2, PK_AVR, uid); }
        if (!off) g_cap(fine_on ? x + 5 : x + 4, y + 1, 'o', status_col(st), bg_at(x + 5, y + 1));
        int nx = x + 7, room = w - 8;
        const char *bt = TR("БОТ");
        int bw = bot ? vw(bt) + 2 : 0;
        int u = g_texte(nx, y, nm, room - (bot ? bw + 1 : 0), col, 0);
        if (bot && u + 1 + bw <= room) { g_pill(nx + u + 1, y, bw, C_ACC); g_text(nx + u + 2, y, bt, bw - 2, C_WHITE, AT_B); }
        if (tx[0] && !off) {
            switch (act) {
            case MLA_STREAM: snprintf(t, sizeof t, TR("Стримит %s"), tx); break;
            case MLA_LISTEN: snprintf(t, sizeof t, TR("Слушает %s"), tx); break;
            case MLA_WATCH: snprintf(t, sizeof t, TR("Смотрит %s"), tx); break;
            case MLA_COMPETE: snprintf(t, sizeof t, TR("Соревнуется в %s"), tx); break;
            default: snprintf(t, sizeof t, "%s", tx); break;
            }
            g_texte(nx, y + 1, t, room, C_MUTED, 0);
        }
        hit_add(x, y, w, 2, A_MEMB, 0, i, HV_BG);
    }
}

/* ---- №95 форум: посты карточками, как в Discord (заголовок, начало первого сообщения, число сообщений).
 * Посты — из store (forum_open при открытии канала, forum_close при уходе); ленту форум не грузит. ---- */
static void forum_go(int i) {
    const ForumState *fs = forum_state();
    if (!fs || i < 0 || i >= fs->n) return;
    u64 t = forum_post_open(i);
    arch_view = 0;
    if (t) open_channel(t);
    else ui_notice(TR("пост недоступен"));
}
static void forum_page(int d) {
    const ForumState *fs = forum_state();
    if (!fs || fs->busy) return;
    int pg = fs->page + d;
    if (pg < 0 || (d > 0 && !fs->more)) return;
    forum_open(fs->cid, pg, fs->sort);
    FV.sel = FV.top = 0;
}
static void draw_forum(int cx, int cw, int H, const Channel *oc) {
    const ForumState *fs = forum_state();
    g_rect(cx, 1, cw, H - 1, C_FEED);
    int x = cx + 2, w = cw - 4, y = 2;
    if (w < 20) return;
    if (arch_view == oc->id) {                   /* №93: архив веток канала */
        int u = g_text(x, y, TR("Архив веток"), w, C_WHITE, AT_B);
        g_text(x + u + 2, y, TR("[закрыть]"), w - u - 2, C_URL, 0);
        hit_add(x + u + 2, y, 9, 1, A_BTN, B_ARCH_X, 0, HV_UL);
    } else {
        const char *st = fs && fs->sort ? TR("Сортировка: по дате создания") : TR("Сортировка: по активности");
        int u = g_button(x, y, st, w, 0, B_FSORT);
        g_button(x + u + 1, y, TR("Новый пост"), w - u - 1, 1, B_FNEW);   /* №96 */
    }
    y += 2;
    int bot = H - 3;                             /* строка H-2 — страницы, H-1 — состояние */
    if (!fs || fs->cid != oc->id || (fs->busy && !fs->n)) g_text(x, y, TR("загрузка..."), w, C_MUTED, 0);
    else if (fs->err[0]) { char *e = san(fs->err, false); g_texte(x, y, e, w, C_RED, 0); free(e); }
    else if (!fs->n) g_text(x, y, arch_view == oc->id ? TR("В архиве веток нет.") : TR("Здесь пока нет постов."), w, C_MUTED, 0);
    else {
        int vis = (bot - y + 1) / 4;
        if (vis < 1) vis = 1;
        if (FV.sel >= fs->n) FV.sel = fs->n - 1;
        if (FV.sel < 0) FV.sel = 0;
        if (FV.sel < FV.top) FV.top = FV.sel;
        if (FV.sel >= FV.top + vis) FV.top = FV.sel - vis + 1;
        bool foc = U.focus == F_MESSAGES;
        for (int i = FV.top; i < fs->n && y + 2 <= bot; i++, y += 4) {
            const ForumPost *fp = &fs->p[i];
            bool sel = foc && i == FV.sel;
            g_rect(x, y, w, 3, sel ? C_SEL : C_EMBBG);
            for (int r = 0; r < 3; r++) g_round_corners(x, y + r, w, 3, r, C_FEED);
            /* 1: заголовок (закреплено / в архиве — серым после него) */
            char *nm = san(fp->name ? fp->name : "", false);
            int u = 0;
            if (fp->pinned) u += g_text(x + 2, y, TR("[закреплено] "), w - 4, C_GOLD, AT_B);
            u += g_texte(x + 2 + u, y, nm, w - 4 - u - (fp->archived ? 11 : 0), C_WHITE, AT_B);
            if (fp->archived) g_text(x + 2 + u, y, TR(" (в архиве)"), w - 4 - u, C_MUTED, 0);
            free(nm);
            /* 2: автор и начало первого сообщения */
            const char *au = fp->owner_id ? user_name(fp->owner_id) : NULL;
            char *pv = san(fp->preview ? fp->preview : "", false);
            for (char *q = pv; *q; q++) if (*q == '\n') *q = ' ';
            int a = 0;
            if (au && *au) {
                char t[120];
                char *sa = san(au, false);
                snprintf(t, sizeof t, "%s: ", sa);
                free(sa);
                a = g_texte(x + 2, y + 1, t, w - 4, C_TEXT, AT_B);
            }
            g_texte(x + 2 + a, y + 1, pv[0] ? pv : TR("(без текста)"), w - 4 - a, C_MUTED, 0);
            free(pv);
            /* 3: сколько сообщений, когда последнее, участвую ли */
            char t3[160], d[64] = "";
            if (fp->last_msg > (1ULL << 32)) {   /* id-снежинка: время в старших битах */
                i64 ts = (i64)(((fp->last_msg >> 22) + 1420070400000ULL) / 1000);
                struct tm tm;
                plat_localtime(ts, &tm);
                snprintf(d, sizeof d, TR("  посл.: %02d.%02d.%04d"), tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
            }
            snprintf(t3, sizeof t3, TR("o %d сообщ.%s%s"), fp->msg_count, d, fp->joined ? TR("  вы участник") : "");
            g_texte(x + 2, y + 2, t3, w - 4, C_MUTED, 0);
            hit_add(x, y, w, 3, A_FPOST, 0, i, HV_BG);
        }
    }
    /* страницы (по 25 постов) */
    if (fs && fs->cid == oc->id && (fs->page > 0 || fs->more)) {
        char pg[64];
        int tot = fs->total > 0 ? (fs->total + 24) / 25 : 0;
        if (tot) snprintf(pg, sizeof pg, TR("<  стр. %d из %d  >"), fs->page + 1, tot);
        else snprintf(pg, sizeof pg, TR("<  стр. %d  >"), fs->page + 1);
        int px = cx + (cw - vw(pg)) / 2;
        g_text(px, H - 2, pg, cw - 2, C_TEXT, 0);
        if (fs->page > 0) hit_add(px, H - 2, 2, 1, A_BTN, B_FPREV, 0, HV_UL);
        if (fs->more) hit_add(px + vw(pg) - 2, H - 2, 2, 1, A_BTN, B_FNEXT, 0, HV_UL);
    }
    draw_status(cx + 2, cx + cw - 3, H - 1);
}
/* Клавиши в форуме (фокус на ленте): Up/Down — пост, Enter — открыть, PgUp/PgDn — страницы. */
static bool forum_key(const Key *k) {
    const ForumState *fs = forum_state();
    if (!fs) return false;
    if (k->code == K_UP) { if (FV.sel > 0) FV.sel--; return true; }
    if (k->code == K_DOWN) { if (FV.sel < fs->n - 1) FV.sel++; return true; }
    if (k->code == K_HOME) { FV.sel = 0; return true; }
    if (k->code == K_END) { FV.sel = fs->n > 0 ? fs->n - 1 : 0; return true; }
    if (k->code == K_ENTER) { forum_go(FV.sel); return true; }
    if (k->code == K_PGDN) { forum_page(1); return true; }
    if (k->code == K_PGUP) { forum_page(-1); return true; }
    return false;
}

/* ---- лента + поле ввода в колонке x..x+w-1 (центр или чат голосового канала справа) ---- */
/* Поле ввода растёт до 10 строк (как в Discord): ширина текста поля и сколько строк в нём при этой ширине. */
static int in_width(int cx, int cw) {
    bool plain = !(U.mode == M_DELETE || U.mode == M_VLOFF || U.mode == M_VLINK || U.mode == M_FILE);
    int right = cx + cw - 3;
    if (plain) right -= cw >= 56 ? 20 : 5;
    return right - (cx + 5) - 1;
}
static int in_count_lines(int iw) {
    int ln = 1, col = 0;
    if (iw < 4) return 1;
    for (size_t i = 0; i < U.in.len; ) {
        u32 c;
        i += (size_t)utf8_dec(U.in.p + i, &c);
        if (c == '\n') { ln++; col = 0; continue; }
        int w = shown_w(c);
        if (col + w > iw - 1) { ln++; col = 0; }
        col += w;
    }
    return ln;
}
static struct { int cmd, opt, sel; char *val[25]; u64 cid; } SL = { .cmd = -1 };
static const char *cs_show(u16 off) {
    static char b[4][160];
    static int k;
    char *t = san(cmds_str(off), false);
    char *o = b[k++ & 3];
    snprintf(o, sizeof b[0], "%s", t);
    free(t);
    return o;
}
static const SlashOpt *sl_opt(void) {
    const SlashIndex *si = cmds_state();
    if (!si || SL.cmd < 0 || SL.cmd >= si->n) return NULL;
    const SlashCmd *c = &si->c[SL.cmd];
    if (SL.opt >= c->nopt || c->opt0 + SL.opt >= si->no) return NULL;
    return &si->o[c->opt0 + SL.opt];
}
static const char *sl_type(int t) {
    switch (t) {
    case CO_STRING: return TR("текст"); case CO_INTEGER: return TR("целое число"); case CO_NUMBER: return TR("число");
    case CO_BOOLEAN: return TR("да/нет"); case CO_USER: return TR("пользователь"); case CO_CHANNEL: return TR("#канал");
    default: return TR("значение");
    }
}
static void draw_ac(int x, int w, int ybot);
static void draw_sl(int x, int w, int ybot);
static const char *un(u64 uid);
static u64 rgt[16];                              /* №138: кому звонить / не звонить (на кадр) */
static int n_rgt;
/* №138: звонок в группе ЛС — полоса над лентой: кому звонят («не звонить»), кто не в звонке («позвонить
 * ещё раз»). draw=false — только посчитать строки. */
static int ring_strip(int cx, int cw, int y, const Channel *oc, bool draw) {
    VoiceStatus vs;
    voice_status(&vs);
    if (!oc || oc->guild_id || oc->type != CH_GROUP || !oc->nrecip || vs.phase == VS_IDLE || vs.channel_id != oc->id) return 0;
    const VoiceState *list[16];
    int k = voice_members(oc->id, list, 16);
    u64 ring[16];
    int nr = call_ringing(oc->id, ring, 16), rows = 1;
    if (draw) {
        n_rgt = 0;
        g_rect(cx, y, cw, 1, C_RAIL);
        char t[96];
        snprintf(t, sizeof t, TR("Звонок в группе: в звонке %d"), k);
        int u = g_text(cx + 2, y, t, cw - 4, C_GREEN, AT_B);
        if (nr) {
            u += 2 + g_text(cx + 4 + u, y, TR("[не звонить никому]"), cw - 6 - u, C_URL, 0);
            hit_add(cx + 4 + u - vw(TR("[не звонить никому]")), y, vw(TR("[не звонить никому]")), 1, A_BTN, B_RING_ALL_STOP, 0, HV_UL);
        }
    }
    for (int i = 0; i < oc->nrecip && n_rgt < 16; i++) {
        u64 u = oc->recipients[i];
        bool inv = false, rg = false;
        for (int q = 0; q < k; q++) if (list[q]->user_id == u) inv = true;
        if (inv || u == S.me_id) continue;
        for (int q = 0; q < nr; q++) if (ring[q] == u) rg = true;
        if (draw) {
            int yy = y + rows;
            g_rect(cx, yy, cw, 1, C_RAIL);
            char t[160];
            snprintf(t, sizeof t, "%s - %s  ", un(u), rg ? TR("звоним...") : TR("не в звонке"));
            const char *act = rg ? TR("[не звонить]") : TR("[позвонить ещё раз]");
            int u0 = g_text(cx + 4, yy, t, cw - 6, rg ? C_GOLD : C_MUTED, 0);
            int u1 = g_text(cx + 4 + u0, yy, act, cw - 6 - u0, C_URL, 0);
            rgt[n_rgt] = u;
            hit_add(cx + 4 + u0, yy, u1, 1, A_BTN, rg ? B_RING_STOP : B_RING_AGAIN, n_rgt++, HV_UL);
        }
        rows++;
        if (rows >= 5) break;
    }
    return rows;
}
static int tab_pos(u64 id) { for (int i = 0; i < TB.n; i++) if (TB.id[i] == id) return i; return -1; }
static void tab_go(int i) { if (i >= 0 && i < TB.n && chan_find(TB.id[i])) open_channel(TB.id[i]); }
static void tab_close(int i) {
    if (i < 0 || i >= TB.n) return;
    u64 was = TB.id[i];
    memmove(TB.id + i, TB.id + i + 1, (size_t)(TB.n - i - 1) * sizeof(u64));
    TB.n--;
    if (was == S.open_ch && TB.n) tab_go(i < TB.n ? i : TB.n - 1);
    U.dirty = true;
}
/* Полоса вкладок в шапке (когда их две и больше): «#имя x»; выбранная — подсвечена. */
static void draw_tabs(int x0, int x1) {
    int x = x0;
    for (int i = 0; i < TB.n; i++) {
        Channel *c = chan_find(TB.id[i]);
        if (!c) { tab_close(i); i--; continue; }
        char lab[96], t[64];
        chan_label(c, lab, sizeof lab);
        snprintf(t, sizeof t, "%s%.14s", chan_mark(c)[0] == '#' ? "#" : "", lab);
        int w = vw(t) + 5;
        if (x + w > x1) { g_text(x, 0, "...", x1 - x, C_MUTED, 0); break; }
        bool on = c->id == S.open_ch;
        g_pill(x, 0, w, on ? C_SEL : C_RAIL);
        g_texte(x + 1, 0, t, w - 4, on ? C_WHITE : C_MUTED, on ? AT_B : 0);
        hit_add(x, 0, w - 3, 1, A_TAB, 0, i, HV_PILL);
        g_text(x + w - 2, 0, "x", 1, C_MUTED, 0);
        hit_add(x + w - 3, 0, 3, 1, A_TAB, 1, i, HV_RED);
        x += w + 1;
    }
}
static void draw_forum(int cx, int cw, int H, const Channel *oc);
static int rq_btn(int x_end, int y, const char *t, u32 fg, int sub, int arg);
static void draw_chat(int cx, int cw, int H, Channel *oc, bool panel) {
    if (is_forum(oc) || (oc && arch_view == oc->id)) { draw_forum(cx, cw, H, oc); return; }
    int nlin = U.mode == M_VLINK ? 1 : in_count_lines(in_width(cx, cw));
    int ex = nlin - 1 < 9 ? nlin - 1 : 9;           /* лишние строки поля (до 10 всего), лента — не меньше 6 строк */
    if (ex > H - 12) ex = H - 12;
    if (ex < 0) ex = 0;
    int feed_top = 1 + ring_strip(cx, cw, 1, oc, true), feed_bot = H - 4 - ex, frows = feed_bot - feed_top + 1;
    hit_add(cx, feed_top, cw, frows, A_AR_FEED, 0, 0, 0);
    /* переход к сообщению (msgs_jump) пришёл: выбрать его, лента следует за выбором */
    if (!panel && S.jump_id && S.jump_id != U.jump_seen) {
        int idx = -1;
        if (msg_find(S.jump_id, &idx) && idx >= 0) { U.sel_msg = idx; U.focus = F_MESSAGES; U.follow_sel = true; U.msg_scroll = 0; U.jump_seen = S.jump_id; }
    }
    int tw = cw - 7;
    int nml = build_mlines(tw);
    bool empty_ok = !U.loading && (!U.hist_state[0] || !strcmp(U.hist_state, "в канале пока нет сообщений"));
    bool welcome = oc && (S.history_end || (!nml && empty_ok));
    /* приветствие: круг 3 строки, заголовок и подпись с переносом по словам, пустая строка */
    char wt[240], ws[240], **wl = NULL;
    int nwt = 0, nwl = 0;
    if (welcome) {
        char lab0[96];
        chan_label(oc, lab0, sizeof lab0);
        const char *mk0 = oc->guild_id && !is_voice(oc) ? "#" : "";
        if (is_thread(oc)) snprintf(wt, sizeof wt, "%s", lab0);
        else if (oc->guild_id) snprintf(wt, sizeof wt, TR("Добро пожаловать на канал %s%s!"), mk0, lab0);
        else snprintf(wt, sizeof wt, "%s", lab0);
        /* нет права «Читать историю сообщений»: Discord отдаёт [] — как в Discord, говорим об этом
         * вместо «Это начало канала»; новые сообщения при этом приходят и показываются ниже */
        if (oc->guild_id && !oc->can_history && !is_voice(oc))
            snprintf(ws, sizeof ws, TR("У вас нет прав на просмотр истории сообщений в этом канале."));
        else if (is_thread(oc)) {
            const Channel *pc = chan_find(oc->parent_id);
            char pl[96] = "";
            if (pc) chan_label(pc, pl, sizeof pl);
            snprintf(ws, sizeof ws, TR("Это начало ветки%s%s%s."), pl[0] ? TR(" в ") : "", pl[0] && !is_forum(pc) ? "#" : "", pl);
        }
        else if (oc->guild_id) snprintf(ws, sizeof ws, TR("Это начало канала %s%s!"), mk0, lab0);
        else snprintf(ws, sizeof ws, TR("Это начало истории ваших личных сообщений с %s."), lab0);
        char **a1, **a2;
        int n1 = help_wrap(wt, cw - 3, &a1), n2 = help_wrap(ws, cw - 3, &a2);
        wl = xmalloc((size_t)(n1 + n2 + 1) * sizeof *wl);
        for (int i = 0; i < n1; i++) wl[nwl++] = a1[i];
        for (int i = 0; i < n2; i++) wl[nwl++] = a2[i];
        nwt = n1;
        free(a1);
        free(a2);
    }
    bool chset = welcome && oc->guild_id;          /* кнопка «Настроить канал» под приветствием канала */
    int pre = !oc ? 0 : welcome ? 3 + nwl + 1 + (chset ? 2 : 0) : 1;   /* приветствие или «загрузить старые» */
    int rows = frows;
    /* полоса «Вы просматриваете старые сообщения» занимает строку ленты: ограничивать прокрутку надо уже по
     * высоте без неё, иначе лента останавливается на строку ниже верха, U.at_top не наступает и старые
     * сообщения не подгружаются (оставалось первые 50) */
    bool below = U.msg_scroll > 0 || S.detached;   /* detached: загружены старые, самых новых нет */
    if (below) rows--;
    scroll_fix(pre, nml, rows);
    if (below && !(U.msg_scroll > 0 || S.detached)) { rows++; scroll_fix(pre, nml, rows); below = false; }   /* всё влезло: полосы нет */
    int total = pre + nml;
    int v_start = total - rows - U.msg_scroll;  /* виртуальный индекс верхней видимой строки */
    if (total < rows) v_start = total - rows;   /* мало строк: прижаты к низу, как в Discord */
    U.at_top = v_start <= 0;

    const char *cl[14];
    int cn = 0, nb = 0;
    u32 tfg = C_WHITE;
    CBtn cb[4] = {{0}};
    char c1[200], c2[200], **why = NULL;
    int nwhy = 0;
    VlessState vst = vless_state();
    if (vst == VL_DOWN && !panel) {
        /* VLESS не работает: окно висит, пока пользователь не выберет, что делать (напрямую клиент
         * сам не ходит). Причина — итог последней проверки ссылки или слова модуля vless. */
        char *r = san(streamer_on() ? TR("подробности скрыты (режим стримера)") : U.vl_note[0] ? U.vl_note : vless_error(), false);
        char rd[200];
        deco_ascii(*r ? r : TR("туннель не отвечает"), rd, sizeof rd);
        free(r);
        bool wide = cw - 12 >= 46;
        nwhy = help_wrap(rd, cw - 12, &why);
        int nr = nwhy < 3 ? nwhy : 3;
        if (frows < 12 && nr > 1) nr = 1;
        cl[cn++] = TR("VLESS: нет связи");
        for (int i = 0; i < nr; i++) cl[cn++] = why[i];
        tfg = C_RED;
        if (U.busy[0]) {
            cl[cn++] = U.busy;
            cl[cn++] = wide ? TR("ждите до 10 с: экран не отвечает до конца проверки") : TR("ждите до 10 с");
        } else if (U.mode == M_VLINK) {
            cl[cn++] = wide ? TR("вставьте ссылку в поле ввода внизу") : TR("вставьте ссылку внизу");
            cl[cn++] = wide ? TR("на экране она не показывается: только звёздочки") : TR("она не показывается");
            cb[nb++] = (CBtn){ TR("Назад (Esc)"), B_VL_BACK, 0 };
            cb[nb++] = (CBtn){ TR("Проверить (Enter)"), B_VL_CHECK, 1 };
        } else if (U.mode == M_VLOFF) {
            cl[cn++] = wide ? TR("трафик пойдёт напрямую, Discord увидит ваш IP") : TR("Discord увидит ваш IP");
            cb[nb++] = (CBtn){ TR("Отмена"), B_NO, 0 };
            cb[nb++] = (CBtn){ TR("Отключить (y)"), B_YES, 2 };
        } else {
            cl[cn++] = U.focus == F_INPUT && U.in.len ? (wide ? TR("в поле ввода текст: Esc, затем R / C / D") : TR("сначала Esc"))
                                                      : (wide ? TR("напрямую клиент не ходит - выберите:") : TR("выберите:"));
            cb[nb++] = (CBtn){ wide ? TR("Отключить VLESS (D)") : TR("Отключить (D)"), B_VL_D, 2 };
            cb[nb++] = (CBtn){ TR("Сменить ссылку (C)"), B_VL_C, 0 };
            cb[nb++] = (CBtn){ TR("Переподключиться (R)"), B_VL_R, 1 };
        }
    } else if (!oc) {
        if (!S.ready && !O.demo) {
            char gst[128];
            deco_ascii(gw_status_text(), gst, sizeof gst);
            snprintf(c1, sizeof c1, "%s", gst[0] ? gst : TR("подключение..."));
            cl[cn++] = APP_NAME; cl[cn++] = c1; cl[cn++] = TR("Ctrl+Q - выход");
        } else {
            cl[cn++] = TR("Выберите канал слева");
            cl[cn++] = TR("щелчок мышью или Tab, Up/Down, Enter");
        }
    } else if (!nml && !welcome) {
        if (U.loading || !U.hist_state[0]) cl[cn++] = TR("загрузка истории...");
        else {
            const char *sp = strstr(U.hist_state, ": ");
            if (sp) {
                snprintf(c1, sizeof c1, "%.*s", (int)(sp - U.hist_state), U.hist_state);
                snprintf(c2, sizeof c2, "%s", sp + 2);
                cl[cn++] = c1;
                cl[cn++] = c2;
                tfg = C_RED;
            } else cl[cn++] = tr(U.hist_state);   /* «в канале пока нет сообщений» хранится по-русски (сравнение выше) */
        }
    }
    if (cn) g_card(cx, feed_top, cw, frows, -1, cl, cn, cb, nb, tfg, vst == VL_DOWN);
    else {
        char lab[96] = "";
        if (oc) chan_label(oc, lab, sizeof lab);
        for (int ry = 0; ry < rows; ry++) {
            int v = v_start + ry, y = feed_top + ry;
            if (v < 0 || v >= total) continue;
            if (v >= pre) { draw_mline(v - pre, cx, y, cw); continue; }
            if (welcome) {
                /* как в Discord: большой круг со значком канала, «Добро пожаловать», «Это начало» */
                if (v <= 2) {
                    if (oc && !oc->guild_id && oc->type == CH_DM && oc->nrecip > 0) g_avatar(cx + 2, y, 6, 3, v, oc->recipients[0], lab);
                    else g_circle_row(cx + 2, y, 6, 3, v, C_BTN2, is_voice(oc) ? "<)" : oc->guild_id ? "#" : "@", C_WHITE);
                } else if (v - 3 < nwl) {
                    bool title = v - 3 < nwt;
                    g_text(cx + 2, y, wl[v - 3], cw - 3, title ? C_WHITE : C_MUTED, title ? AT_B : 0);
                } else if (chset && v - 3 == nwl + 1) {
                    int bw = vw(TR("| Настроить канал")) + 4;
                    if (bw > cw - 3) bw = cw - 3;
                    g_pill(cx + 2, y, bw, C_BTN2);
                    g_text(cx + 4, y, TR("| Настроить канал"), bw - 4, C_TEXT, 0);
                    hit_add(cx + 2, y, bw, 1, A_BTN, B_NOP, 0, HV_PILL); U.hit[U.nhit - 1].tip = T_CHSET;
                }
            } else {
                g_ctext(cx, y, cw, U.loading ? TR("загрузка...") : TR("загрузить более старые сообщения (PgUp)"), C_MUTED, 0);
                if (!U.loading) hit_add(cx, y, cw, 1, A_BTN, B_OLDER, 0, HV_UL);
            }
        }
        /* панель действий над сообщением под мышью (или выбранным с клавиатуры), как в Discord: справа на
         * первой видимой строке сообщения — реакция, ответить, изменить (своё), «...» (полное меню) */
        U.bar_msg = 0;
        if (!panel || cw >= 40) {
            int hm = -1, hy = 1 << 30;
            for (int i = 0; i < U.nhit; i++)
                if (U.hit[i].act == A_MSG && U.hit[i].x == cx && hit_in(&U.hit[i], U.hx, U.hy)) hm = U.hit[i].arg;
            if (hm < 0 && U.focus == F_MESSAGES) hm = U.sel_msg;
            for (int i = 0; i < U.nhit && hm >= 0; i++)
                if (U.hit[i].act == A_MSG && U.hit[i].x == cx && U.hit[i].arg == hm && U.hit[i].y < hy) hy = U.hit[i].y;
#ifdef MC_PACKA
            if (hm >= 0 && hm < S.nmsgs && S.msgs[hm].state != MS_SENT) hm = -1;   /* у неотправленного — только строка «Повторить - Удалить» */
#endif
            if (hm >= 0 && hm < S.nmsgs && hy < feed_top + rows) {
                const Message *bm = &S.msgs[hm];
                bool own = bm->author_id == S.me_id;
                static const char *const ic[4] = { "+:)", "<-", "e/", "..." };
                static const int ids[4] = { B_MB_REACT, B_MB_REPLY, B_MB_EDIT, B_MB_MORE }, tips[4] = { T_MB_REACT, T_MB_REPLY, T_MB_EDIT, T_MORE };
                int bw = 2;
                for (int i = 0; i < 4; i++) if (i != 2 || own) bw += vw(ic[i]) + 2;
                int bx = cx + cw - 2 - bw;
                g_pill(bx, hy, bw, C_MENU);
                int x = bx + 2;
                for (int i = 0; i < 4; i++) {
                    if (i == 2 && !own) continue;
                    g_icon_btn(x, hy, ic[i], C_TEXT, ids[i], tips[i]);
                    U.hit[U.nhit - 1].hv = HV_PILL;
                    x += vw(ic[i]) + 2;
                }
                U.bar_msg = bm->id;
            }
        }
        if (below && S.detached) {
            /* после перехода к старому сообщению: как в Discord — «Вернуться к настоящему» */
            g_pill(cx + 1, feed_bot, cw - 2, C_ACC);
            int u = g_text(cx + 2, feed_bot, TR(" Вы просматриваете старые сообщения"), cw - 4, C_WHITE, 0);
            if (cw - 4 - u > 24) g_rtext(cx + cw - 3, feed_bot, TR("Вернуться к настоящему"), 22, C_WHITE, AT_B);
            hit_add(cx + 1, feed_bot, cw - 2, 1, A_BTN, B_PRESENT, 0, HV_UL);
        } else if (below) {
            /* самый длинный текст, рядом с которым помещается «Перейти к новым»; иначе — самый
             * длинный, который помещается сам; «ниже ещё N строк: PgDn» видно всегда */
            static const char *const pre_txt[] = { " Вы просматриваете старые сообщения:", " Старые сообщения:", "" };
            const char *ns = plural(U.msg_scroll, TR("строка"), TR("строки"), TR("строк"));
            char t[200];
            int pick = -1;
            for (int q = 0; q < 3; q++) {
                snprintf(t, sizeof t, TR("%s ниже ещё %d %s: PgDn"), tr(pre_txt[q]), U.msg_scroll, ns);
                if (vw(t) + 19 <= cw - 4) { pick = q; break; }
            }
            if (pick < 0)
                for (int q = 0; q < 3; q++) {
                    snprintf(t, sizeof t, TR("%s ниже ещё %d %s: PgDn"), tr(pre_txt[q]), U.msg_scroll, ns);
                    if (vw(t) <= cw - 4 || q == 2) { pick = q; break; }
                }
            snprintf(t, sizeof t, TR("%s ниже ещё %d %s: PgDn"), pre_txt[pick], U.msg_scroll, ns);
            g_pill(cx + 1, feed_bot, cw - 2, C_ACC);
            int u = g_text(cx + 2, feed_bot, t, cw - 4, C_WHITE, 0);
            if (cw - 4 - u > 18) g_rtext(cx + cw - 3, feed_bot, TR("Перейти к новым"), 15, C_WHITE, AT_B);
            hit_add(cx + 1, feed_bot, cw - 2, 1, A_BTN, B_BOTTOM, 0, HV_UL);
        }
        /* полоса прокрутки: тонкая, справа */
        if (total > rows && rows >= 2) {
            int tl = rows * rows / total;
            if (tl < 1) tl = 1;
            int maxf = total - rows, f = v_start < 0 ? 0 : v_start;
            int t0 = maxf ? (f * (rows - tl) + maxf / 2) / maxf : 0;
            for (int i = 0; i < rows; i++) g_bg(cx + cw - 1, feed_top + i, 1, i >= t0 && i < t0 + tl ? C_THUMB : C_SIDE);
        }
    }
    for (int i = 0; i < nwhy; i++) free(why[i]);
    free(why);
    for (int i = 0; i < nwl; i++) free(wl[i]);
    free(wl);

    /* входящий звонок и подтверждение удаления — окна поверх ленты */
    if (!panel && S.call.ringing && vst != VL_DOWN) {
        Channel *cc = chan_find(S.call.channel_id);
        char lab[96] = "?", t[140];
        if (cc) chan_label(cc, lab, sizeof lab);
        snprintf(t, sizeof t, TR("звонит: %s"), lab);
        const char *l2[2] = { TR("Входящий звонок"), t };
        CBtn b2[2] = { { TR("Отклонить (F3)"), B_CALL_NO, 2 }, { TR("Принять (F2)"), B_CALL_OK, 1 } };
        g_card(cx, feed_top, cw, frows, feed_top, l2, 2, b2, 2, C_WHITE, false);
    }
    if (U.mode == M_DELETE) {
        Message *m = msg_find(U.target, NULL);
        char pv[200];
        snprintf(pv, sizeof pv, "%s", m && m->content ? m->content : "");
        for (char *p = pv; *p; p++) if (*p == '\n') *p = ' ';
        const char *l2[3] = { TR("Удалить сообщение"), TR("Вы точно хотите удалить это сообщение?"), pv };
        CBtn b2[2] = { { TR("Отмена"), B_NO, 0 }, { TR("Удалить (y)"), B_YES, 2 } };
        g_card(cx, feed_top, cw, frows, -1, l2, pv[0] ? 3 : 2, b2, 2, C_WHITE, true);
    }

    if (U.mode == M_FILE && U.in.len && !panel && cw >= 40) {   /* №65/67: что отправится — имя, спойлер, описание, кнопка */
        size_t a0 = 0, b0 = U.in.len, nm0;
        while (a0 < b0 && (U.in.p[a0] == ' ' || U.in.p[a0] == '"' || U.in.p[a0] == '\'')) a0++;
        while (b0 > a0 && (U.in.p[b0 - 1] == ' ' || U.in.p[b0 - 1] == '"' || U.in.p[b0 - 1] == '\'')) b0--;
        nm0 = b0;
        while (nm0 > a0 && U.in.p[nm0 - 1] != '/' && U.in.p[nm0 - 1] != '\\') nm0--;
        int y0 = H - 3 - ex - 1;
        if (y0 > feed_top + 1 && b0 > nm0) {
            g_rect(cx + 2, y0, cw - 4, 1, C_EMBBG);
            const char *t1 = file_spoil ? TR("[x] спойлер") : TR("[ ] спойлер"), *t2 = file_alt[0] ? TR("[описание: есть]") : TR("[описание]");
            int rx2 = cx + cw - 4;
            rx2 -= g_button(rx2 - 13, y0, TR("Отправить"), 13, 1, B_FSEND) + 1;
            int w2 = vw(t2); rx2 -= w2 + 1;
            g_text(rx2, y0, t2, w2, file_alt[0] ? C_TEXT : C_URL, 0); hit_add(rx2, y0, w2, 1, A_BTN, B_FALT, 0, HV_UL);
            int w1 = vw(t1); rx2 -= w1 + 2;
            g_text(rx2, y0, t1, w1, file_spoil ? C_GOLD : C_URL, 0); hit_add(rx2, y0, w1, 1, A_BTN, B_FSPOIL, 0, HV_UL);
            int nw = vwn(U.in.p + nm0, b0 - nm0), ny = y0;
            if (nw + 8 > rx2 - cx - 4 && y0 - 1 > feed_top + 1) { ny = y0 - 1; g_rect(cx + 2, ny, cw - 4, 1, C_EMBBG); rx2 = cx + cw - 4; }
            int u0 = g_text(cx + 3, ny, TR("файл: "), rx2 - cx - 4, C_MUTED, 0);
            u0 += g_textn(cx + 3 + u0, ny, U.in.p + nm0, b0 - nm0, rx2 - cx - 5 - u0, C_WHITE, AT_B);
            char pth[1024], sz[48];
            size_t pl = b0 - a0 < sizeof pth - 1 ? b0 - a0 : sizeof pth - 1;
            memcpy(pth, U.in.p + a0, pl);
            pth[pl] = 0;
            long long fsz = plat_file_size(pth);
            wipe(pth, sizeof pth);
            if (fsz < 0) snprintf(sz, sizeof sz, TR("  нет такого файла"));
            else if (fsz >= 1024 * 1024) snprintf(sz, sizeof sz, TR("  %.1f МБ%s"), (double)fsz / (1024.0 * 1024.0), fsz > FILE_MAX ? TR(" - больше 10 МБ") : "");
            else snprintf(sz, sizeof sz, TR("  %lld КБ"), (fsz + 1023) / 1024);
            g_text(cx + 3 + u0, ny, sz, rx2 - cx - 4 - u0, fsz < 0 || fsz > FILE_MAX ? C_RED : C_MUTED, 0);
        }
    }
    /* №47 предпросмотр разметки: набранное с маркерами — как будет выглядеть (до 3 строк над полем) */
    if ((U.mode == M_NORMAL || U.mode == M_REPLY || U.mode == M_EDIT) && U.in.len && U.in.p && strpbrk(U.in.p, "*_~`|#>-[") && !panel) {
        int pw2 = cw - 8, nr = ew_rows(U.in.p, pw2);
        if (nr > 3) nr = 3;
        int y0 = H - 3 - ex - nr - 1;
        if (nr > 0 && y0 > feed_top + 2) {
            g_rect(cx + 2, y0, cw - 4, nr + 1, C_EMBBG);
            g_text(cx + 4, y0, TR("предпросмотр"), cw - 6, C_MUTED, 0);
            for (int r = 0; r < nr; r++) {
                size_t s0, a, e;
                if (ew_line(U.in.p, pw2, r, &s0, &a, &e)) g_mdline(cx + 4, y0 + 1 + r, -1, U.in.p, s0, a, e - a, pw2);
            }
        }
    }
    /* ---- поле ввода: строка режима, плашка «Написать #канал», строка под ней ---- */
    char lab[200] = "";
    Message *t = U.target ? msg_find(U.target, NULL) : NULL;
    bool cancel = false;
    if (U.mode == M_REPLY) { snprintf(lab, sizeof lab, TR("Ответ пользователю %s"), t && t->author ? t->author : "?"); cancel = true; }
    else if (U.mode == M_EDIT) { snprintf(lab, sizeof lab, TR("Изменение сообщения: Enter - сохранить, Esc - отмена")); cancel = true; }
    else if (U.mode == M_REACT) { snprintf(lab, sizeof lab, TR("Реакция: эмодзи, :код: или номер (Esc - отмена)")); cancel = true; }
    else if (U.mode == M_DELETE) snprintf(lab, sizeof lab, TR("удалить сообщение? y - да, любая другая - нет"));
    else if (U.mode == M_VLINK) snprintf(lab, sizeof lab, TR("ссылка VLESS (скрыта), %d симв.: Enter - проверить, Esc - отмена"), in_chars());
    else if (U.mode == M_VLOFF) snprintf(lab, sizeof lab, TR("трафик пойдёт напрямую, Discord увидит ваш IP. Отключить? y - да"));
    else if (U.mode == M_FILE) {
        if (U.file_err[0] && streamer_on()) snprintf(lab, sizeof lab, TR("Файл не отправлен (подробности скрыты: режим стримера)"));
        else if (U.file_err[0]) snprintf(lab, sizeof lab, TR("Файл не отправлен: %.150s"), U.file_err);
        else snprintf(lab, sizeof lab, cw >= 90 ? TR("Отправка файла (до 10 МБ): путь или перетащите в окно, Enter") : TR("Отправка файла (до 10 МБ): путь, Enter"));
        cancel = true;
    }
    else if (oc && is_thread(oc) && (oc->tflags & TF_DELETED)) snprintf(lab, sizeof lab, TR("эту ветку удалили"));
    else if (oc && oc->guild_id && guild_find(oc->guild_id) && guild_find(oc->guild_id)->pending) {
        snprintf(lab, sizeof lab, TR("Писать можно после принятия правил сервера"));
    }
    else if (dm_held(oc)) snprintf(lab, sizeof lab, TR("%s: картинки и аватары скрыты до ответа"), oc->dmflags & DMF_SPAM ? TR("Возможный спам") : TR("Запрос на общение"));
    else if (oc && oc->guild_id && !oc->can_send) snprintf(lab, sizeof lab, TR("нет права писать в этот канал"));
    u32 labfg = U.mode == M_FILE && U.file_err[0] ? C_RED : C_TEXT;
    if (!lab[0] && SL.cmd >= 0 && cmds_state() && SL.cmd < cmds_state()->n) {   /* №57: какую опцию вводим */
        const SlashCmd *sc = &cmds_state()->c[SL.cmd];
        const SlashOpt *so = sl_opt();
        if (so) snprintf(lab, sizeof lab, TR("/%s: %s (%s%s) %d из %d - %s; Esc - отмена"), cs_show(sc->name), cs_show(so->name_show),
                         sl_type(so->type), so->required ? TR(", обязательно") : TR(", Enter - пропустить"), SL.opt + 1, sc->nopt, cs_show(so->desc));
        labfg = C_URL;
        cancel = true;
    }
    SendLimit sl = {0};
    if (oc && !is_voice(oc)) send_limit(oc->id, &sl);
    if (!lab[0] && sl.timeout_until) {
        send_wait_text(sl.timeout_until, true, lab, sizeof lab);
        labfg = C_RED;
    } else if (!lab[0] && sl.slow_until) {
        send_wait_text(sl.slow_until, false, lab, sizeof lab);
        labfg = C_GOLD;
    } else if (!lab[0] && sl.slowmode > 0 && U.mode == M_NORMAL && !U.in.len) {
        char w[48];
        wait_len(sl.slowmode, w, sizeof w);
        snprintf(lab, sizeof lab, TR("Медленный режим: раз в %s"), w);
        labfg = C_MUTED;
    }
    int pct = 0;
    if (!lab[0] && file_sending(&pct)) {
        /* идёт отправка файла: полоска из '#' и '-' в верхней линии рамки */
        char bar[24];
        int k = pct < 0 ? 0 : pct > 100 ? 10 : pct / 10;
        for (int i = 0; i < 10; i++) bar[i] = i < k ? '#' : '-';
        bar[10] = 0;
        snprintf(lab, sizeof lab, TR("Отправка файла [%s] %d%%"), bar, pct);
    }
    if (U.busy[0]) { snprintf(lab, sizeof lab, "%s", U.busy); cancel = false; }
    g_hbox(cx + 1, H - 3 - ex, cw - 2);
#ifdef MC_PACKA
    if (!lab[0] && U.mode == M_NORMAL && U.in.len && (strstr(U.in.p, "http://") || strstr(U.in.p, "https://")) && cw >= 40) {
        const char *pt = U.no_embeds ? TR(" [ ] превью ссылок ") : TR(" [x] превью ссылок ");
        int bx2 = cx + cw - 4 - vw(pt);
        g_text(bx2, H - 3 - ex, pt, vw(pt), U.no_embeds ? C_MUTED : C_TEXT, 0);
        hit_add(bx2 + 1, H - 3 - ex, vw(pt) - 2, 1, A_BTN, B_EMBEDS, 0, HV_UL);
    }
#endif
    if (lab[0]) {
        int bx = cx + cw - 7;
        if (U.mode == M_REPLY && cw >= 44) {
            /* «@ ВКЛ» / «@ ВЫКЛ» — упоминать ли автора ответа (щелчок переключает) */
            const char *pt = U.no_ping ? TR(" @ ВЫКЛ ") : TR(" @ ВКЛ ");
            bx -= vw(pt);
            g_text(bx, H - 3 - ex, pt, vw(pt), U.no_ping ? C_MUTED : C_URL, AT_B);
            hit_add(bx + 1, H - 3 - ex, vw(pt) - 2, 1, A_BTN, B_PING, 0, HV_UL);
            U.hit[U.nhit - 1].tip = T_PING;
            bx += vw(pt);
        }
        int u = g_text(cx + 3, H - 3 - ex, " ", 1, C_TEXT, 0);
        int lim = (cancel ? bx - 1 : cx + cw - 5) - (U.mode == M_REPLY && cw >= 44 ? (U.no_ping ? 8 : 7) : 0);
        u += (U.mode == M_FILE ? g_texte : g_text)(cx + 3 + u, H - 3 - ex, lab, lim - cx - 3 - u, labfg, 0);
        g_text(cx + 3 + u, H - 3 - ex, " ", 1, C_TEXT, 0);
        if (U.mode == M_REPLY) g_fgset(cx + 4 + vw(TR("Ответ пользователю ")), H - 3 - ex, vw(t && t->author ? t->author : "?"), C_WHITE, AT_B);
        if (cancel) { g_text(bx, H - 3 - ex, " (x) ", 5, C_TEXT, AT_B); hit_add(bx + 1, H - 3 - ex, 3, 1, A_BTN, B_CANCEL, 0, HV_RED); }
        if (oc && oc->guild_id && guild_find(oc->guild_id) && guild_find(oc->guild_id)->pending)
            rq_btn(cx + cw - 3, H - 3 - ex, TR("Правила"), C_GOLD, B_RULES, 0);   /* №128 */
        if (dm_held(oc) && cw >= 60) {           /* №109: ответ на запрос прямо из беседы */
            int ci = -1;
            for (int q = 0; q < S.nchans; q++) if (S.chans[q].id == oc->id) ci = q;
            int rx = rq_btn(cx + cw - 3, H - 3 - ex, TR("Отклонить"), C_RED, B_RQ_NO, ci);
            rq_btn(rx - 1, H - 3 - ex, TR("Принять"), C_GREEN, B_RQ_OK, ci);
        }
    }
    int y = H - 2 - ex;
    bool ask = U.mode == M_DELETE || U.mode == M_VLOFF, vlink = U.mode == M_VLINK, file = U.mode == M_FILE;
    bool act = U.focus == F_INPUT || ask;
    for (int r = 0; r <= ex; r++) {
        g_rect(cx + 2, y + r, cw - 4, 1, C_INPUT);
        g_ch(cx + 1, y + r, '|', C_LINE, 0);
        g_ch(cx + cw - 2, y + r, '|', C_LINE, 0);
    }
    int ix = cx + 5, right = cx + cw - 3;        /* текст до значков справа */
    if (ask || vlink) g_text(cx + 3, y, ask ? "?" : "*", 1, ask ? C_GOLD : C_TEXT, AT_B);
    else if (file) {
        g_text(cx + 3, y, "+", 1, C_GOLD, AT_B);
        hit_add(cx + 2, y, 3, 1, A_BTN, B_CANCEL, 0, HV_ICON);
    } else {
        /* «+» слева — файлы (нет в mincord) */
        g_text(cx + 3, y, "+", 1, C_TEXT, AT_B);
        hit_add(cx + 2, y, 3, 1, A_BTN, B_ATTACH, 0, HV_ICON);
        if (U.nhit) U.hit[U.nhit - 1].tip = T_ATTACH;
        static const char *const ic[4] = { "[#]", "GIF", "[/]", ":)" };
        static const int tips[4] = { T_GIFT, T_GIF, T_STICKER, T_EMOJI };
        int xr = right - 1;
        for (int i = 3; i >= (cw >= 56 ? 0 : 3); i--) {   /* узко — только эмодзи, как на эталоне */
            xr -= vw(ic[i]);
            g_icon_btn(xr, y, ic[i], i == 3 && U.ep ? C_WHITE : C_MUTED, i == 3 ? B_EMOJI : B_NOP, tips[i]);
            xr -= 2;
        }
        right = xr;
    }
    int iw = right - ix - 1;
    hit_add(ix, y, iw, ex + 1, A_INPUT, 0, 0, 0);
    if (!panel || cw >= 40) { draw_ac(cx + 2, cw - 4, H - 3 - ex); draw_sl(cx + 2, cw - 4, H - 3 - ex); }
    if (nlin > 1) {
        /* несколько строк: перенос по ширине и по Shift+Enter; видно до 10 строк вокруг курсора */
        int ln = 0, col = 0, curln = 0;
        for (size_t i = 0; i < U.in.len; ) {
            u32 c;
            if (i == U.cur) curln = ln;
            i += (size_t)utf8_dec(U.in.p + i, &c);
            if (c == '\n') { ln++; col = 0; continue; }
            int w = shown_w(c);
            if (col + w > iw - 1) { ln++; col = 0; }
            col += w;
        }
        if (U.cur >= U.in.len) curln = ln;
        int vrows = ex + 1, first = curln >= vrows ? curln - vrows + 1 : 0;
        ln = 0; col = 0;
        for (size_t i = 0; i < U.in.len; ) {
            u32 c;
            int k = utf8_dec(U.in.p + i, &c), w = c == '\n' ? 0 : shown_w(c);
            int a = act && i == U.cur ? AT_I : 0;
            if (c != '\n' && col + w > iw - 1) { ln++; col = 0; }
            bool vis = ln >= first && ln < first + vrows;
            if (c == '\n') { if (a && vis) g_ch(ix + col, y + ln - first, ' ', C_TEXT, AT_I); ln++; col = 0; i += (size_t)k; continue; }
            if (w && vis) g_put(ix + col, y + ln - first, c, w, C_TEXT, a);
            col += w;
            i += (size_t)k;
        }
        if (act && U.cur >= U.in.len && ln >= first && ln < first + vrows) g_ch(ix + col, y + ln - first, ' ', C_TEXT, AT_I);
        g_hbox(cx + 1, H - 1, cw - 2);
        int se2 = draw_status(cx + 2, cx + cw - 3, H - 1);
        if (se2 < cx + cw - 3) g_ch(se2, H - 1, ' ', C_MUTED, 0);
        return;
    }
    /* ссылка VLESS - секрет: каждый символ звёздочкой, открыт только префикс vless:// */
    size_t open_n = vlink && U.in.len >= 8 && !strncmp(U.in.p, "vless://", 8) ? 8 : 0;
    int cur_w = 0;
    for (size_t i = 0; i < U.cur; ) { u32 c; i += (size_t)utf8_dec(U.in.p + i, &c); cur_w += vlink ? 1 : c == '\n' ? 1 : shown_w(c); }
    int skip = cur_w > iw - 1 ? cur_w - (iw - 1) : 0;
    int used = 0, acc = 0;
    size_t fname_off = 0;                        /* №125: в режиме стримера путь к файлу скрыт, видно имя */
    if (file && streamer_on()) for (size_t q = 0; q < U.in.len; q++) if (U.in.p[q] == '/' || U.in.p[q] == '\\') fname_off = q + 1;
    for (size_t i = 0; i < U.in.len; ) {
        u32 c;
        int k = utf8_dec(U.in.p + i, &c);
        int w = vlink || i < fname_off ? 1 : c == '\n' ? 1 : shown_w(c);
        if (!w) { i += (size_t)k; continue; }
        if (acc >= skip) {
            if (used + w > iw) break;
            int a = act && i == U.cur ? AT_I : 0;
            if ((vlink && i >= open_n) || i < fname_off) g_ch(ix + used, y, '*', C_TEXT, a);
            else if (c == '\n') g_ch(ix + used, y, '\\', C_MUTED, a);
            else g_put(ix + used, y, c, w, C_TEXT, a);
            used += w;
        }
        acc += w;
        i += (size_t)k;
    }
    if (act && U.cur >= U.in.len && used < iw) { g_ch(ix + used, y, ' ', C_TEXT, AT_I); used++; }
    if (!U.in.len && used < iw) {
        char ph[160];
        if (U.mode == M_REACT) snprintf(ph, sizeof ph, TR("эмодзи, :код: или номер реакции"));
        else if (vlink) snprintf(ph, sizeof ph, TR("вставьте ссылку vless://... - на экране она не покажется"));
        else if (file) snprintf(ph, sizeof ph, TR("путь к файлу, например C:\\Users\\Вы\\Desktop\\кот.png (кавычки можно)"));
        else if (U.mode == M_VLOFF) snprintf(ph, sizeof ph, TR("y - отключить VLESS, любая другая клавиша - отмена"));
        else if (ask) snprintf(ph, sizeof ph, TR("y - удалить, любая другая клавиша - отмена"));
        else if (!oc) snprintf(ph, sizeof ph, TR("откройте канал слева"));
        else {
            char l2[96];
            chan_label(oc, l2, sizeof l2);
            if (is_thread(oc) && (oc->tflags & TF_DELETED)) snprintf(ph, sizeof ph, TR("Эту ветку удалили - писать в неё нельзя."));
            else if (oc->guild_id && guild_find(oc->guild_id) && guild_find(oc->guild_id)->pending) snprintf(ph, sizeof ph, TR("Сначала примите правила сервера (кнопка 'Правила')."));
            else if (oc->guild_id && !oc->can_send) snprintf(ph, sizeof ph, TR("У вас нет прав на отправку сообщений в этом канале."));
            else if (sl.timeout_until) snprintf(ph, sizeof ph, TR("Вы не можете писать до конца тайм-аута."));
            else snprintf(ph, sizeof ph, TR("+ Написать %s%s"), oc->guild_id && !is_voice(oc) ? "#" : oc->guild_id ? "" : "@", l2);
        }
        g_texte_w(ix + used, y, ph, iw - used, C_MUTED, 0, false);
    }
    g_hbox(cx + 1, H - 1, cw - 2);
    int se = draw_status(cx + 2, cx + cw - 3, H - 1);
    if (se < cx + cw - 3) g_ch(se, H - 1, ' ', C_MUTED, 0);
}

#ifdef ASTRA_VIDEO
/* ---- камера (cam.h): кнопка []/ , диалог «Камера», плитки в центре голосового канала, предпросмотр ----
 * Плитка участника с камерой (кадр — растр окна поверх клеток: g_cam_area в каждом render(); не вызвана в
 * кадре — плитка не собирается и не декодируется). Без камеры — та же рамка, внутри аватар:
 *   +----------------------+
 *   |       кадр 16:9      |
 *   +----------------------+
 *    Имя  (/)           []/
 * Предпросмотр своей камеры — над голосовой панелью в колонке каналов (если своя плитка не видна в центре).
 * --demo: настоящая камера не открывается; «камеры» у 1002, 1006 и у вас (кнопкой) — движущийся узор клетками. */
static bool cu_demo;                             /* --demo: своя «камера» включена */
static bool cu_pv_hide;                          /* предпросмотр в колонке каналов скрыт */
static bool cu_center;                           /* в этом кадре своя плитка — в центре: в колонке не дублировать */
static bool cu_anim;                             /* в этом кадре нарисован демо-узор: ui_tick перерисует */
static int cu_areas;                             /* g_cam_area за этот кадр (не больше CAM_TILES) */
static CamDevice cu_dev[8];
static const char *cu_names[8];
static int cu_ndev;
static u64 cu_tu[16];                            /* плитки с камерой этого кадра (щелчок — крупно), 0 — без камеры */
static bool cu_covered(void);                    /* поверх центра открыто окно/меню — растр спрятать */
static const char *cu_anon_label(u32 *col);
static bool cu_on(void) { return O.demo ? cu_demo : cam_active(); }
static bool cu_user(const VoiceState *v) {       /* у участника включена камера */
    if (v->user_id == S.me_id) return cu_on();
    if (O.demo) return v->user_id == 1002 || v->user_id == 1006;
    CamTile t;
    return v->video || (cam_tile(v->user_id, &t) && t.video);   /* video — self_video (store.c) */
}
/* Раскладка n плиток в w x h клеток. Клетка окна ~1:2, кадр 16:9 -> высота кадра в клетках = ширина * 9/32.
 * Плитка = кадр iw x ih + рамка (2 по ширине, 2 по высоте) + строка имени; между плитками 1 клетка.
 * Наибольший кадр; не влезают все — меньше плиток. Возврат — сколько плиток помещается (0 — ни одной). */
typedef struct { int cols, rows, tw, th, iw, ih; } CamGrid;
static int cam_grid(int n, int w, int h, CamGrid *g) {
    memset(g, 0, sizeof *g);
    for (int fit = n; fit >= 1; fit--) {
        int best = 0;
        for (int cols = 1; cols <= fit; cols++) {
            int rows = (fit + cols - 1) / cols;
            int tw = (w - (cols - 1)) / cols, th = (h - (rows - 1)) / rows;
            int iw = tw - 2, ih = th - 3;
            if (iw < 10 || ih < 3) continue;
            if (iw * 9 / 32 < ih) ih = iw * 9 / 32;
            else iw = ih * 32 / 9;
            if (iw < 10 || ih < 3) continue;
            if (iw * ih > best) { best = iw * ih; *g = (CamGrid){ cols, rows, iw + 2, ih + 3, iw, ih }; }
        }
        if (best) return fit;
    }
    return 0;
}
static int cu_tri(i64 t, int period, int amp) {  /* треугольная волна 0..amp */
    if (period < 2 || amp < 1) return 0;
    int p = (int)(t % (2 * period));
    return (p < period ? p : 2 * period - p) * amp / period;
}
/* --demo: подставной «кадр» клетками — полосы цвета участника и светлое пятно, которое ходит по кадру */
static void cu_pattern(int x, int y, int w, int h, u64 uid) {
    i64 t = now_ms() / 80;
    static const u32 hue[4] = { 0x2b4a7au, 0x6a3b6eu, 0x2f6a4cu, 0x7a5a2bu };
    u32 base = hue[(uid / 2) % 4];
    int bx = cu_tri(t + (int)(uid % 7) * 9, 40, w - 1), by = cu_tri(t + (int)(uid % 5) * 5, 23, h - 1);
    for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) {
        int band = (int)((c + r + t / 2) % 12);
        u32 col = mix(0x0b0c10u, base, 90 + band * 10);
        int dx = c - bx, dy = (r - by) * 2, d2 = dx * dx + dy * dy, rr = h > 4 ? h : 4;
        if (d2 < rr * rr) col = mix(col, 0xe8e2d0u, 200 - 200 * d2 / (rr * rr));
        g_rect(x + c, y + r, 1, 1, col);
    }
    cu_anim = true;
}
/* Кадр камеры uid в клетках (x, y, w, h): заглушка под растром + заявка g_cam_area. */
/* Своя камера, лицо не найдено: вместо кадра — перечёркнутое лицо и предупреждение (собеседники видят значок). */
static void cu_noface(int x, int y, int w, int h) {
    static const char *const face[3] = { "\\ .--. /", " |x  x| ", "/ '--' \\" };
    g_rect(x, y, w, h, 0x1a1410u);
    bool wide = w >= 40;
    int nt = wide ? 2 : 3, nf = h >= nt + 3 ? 3 : 0, top = y + (h - nf - (h >= nt ? nt : 1)) / 2;
    for (int i = 0; i < nf; i++) g_ctext(x, top + i, w, face[i], C_RED, AT_B);
    int ty = top + nf;
    if (h < nt) { g_ctext(x, y + (h - 1) / 2, w, TR("лицо не найдено"), C_GOLD, AT_B); return; }
    g_ctext(x, ty, w, TR("Соединение не защищено:"), C_GOLD, AT_B);
    if (wide) g_ctext(x, ty + 1, w, TR("проверьте, находится ли лицо в камере"), C_TEXT, 0);
    else { g_ctext(x, ty + 1, w, TR("проверьте, находится ли"), C_TEXT, 0); g_ctext(x, ty + 2, w, TR("лицо в камере"), C_TEXT, 0); }
}
static void cu_frame(int x, int y, int w, int h, u64 uid) {
    bool noface = uid == S.me_id && cu_anon_st() == CA_FULL;   /* своя камера без лица: предупреждение */
    if (noface) { cu_noface(x, y, w, h); if (O.demo) { cu_anim = true; return; } }
    else g_rect(x, y, w, h, 0x000000u);
    if (O.demo) { cu_pattern(x, y, w, h, uid); return; }
    CamTile t;
    bool have = cam_tile(uid, &t);
    bool ras = g_video_supported() && !(have && t.unsupported);
    if (!noface && (!have || !t.shown || !ras)) {
        const char *m = have && t.unsupported ? TR("формат видео не поддерживается") : !g_video_supported() ? TR("камера включена")
                      : uid == S.me_id ? TR("включение камеры...") : TR("подключение камеры...");
        int cy = y + (h - 1) / 2;
        if (h >= 3) g_ctext(x, cy - 1, w, "[]/", C_MUTED, AT_B);
        g_ctext(x, h >= 3 ? cy + 1 : cy, w, m, have && t.unsupported ? C_RED : C_MUTED, 0);
    }
    if (ras && !cu_covered() && cu_areas < CAM_TILES) { g_cam_area(uid, x, y, w, h); cu_areas++; }
}
/* Плитка участника: рамка (говорит — зелёная), кадр или аватар, строка имени. */
static void cu_tile(int x, int y, const CamGrid *g, const VoiceState *v, bool cam, bool sp, int idx) {
    const char *nm = v->name && *v->name ? v->name : user_name(v->user_id);
    u32 fr = sp ? C_GREEN : C_LINE;
    int a = sp ? AT_B : 0, ix = x + 1, iy = y + 1;
    g_ch(x, y, '+', fr, a); g_ch(x + g->tw - 1, y, '+', fr, a);
    g_ch(x, y + g->ih + 1, '+', fr, a); g_ch(x + g->tw - 1, y + g->ih + 1, '+', fr, a);
    for (int i = 1; i < g->tw - 1; i++) { g_ch(x + i, y, '-', fr, a); g_ch(x + i, y + g->ih + 1, '-', fr, a); }
    for (int r = 1; r <= g->ih; r++) { g_ch(x, y + r, '|', fr, a); g_ch(x + g->tw - 1, y + r, '|', fr, a); }
    if (cam) cu_frame(ix, iy, g->iw, g->ih, v->user_id);
    else {
        g_rect(ix, iy, g->iw, g->ih, C_PANEL);
        int lh = g->ih - 2 > 6 ? 6 : g->ih - 2;
        if (lh < 1) lh = 1;
        int aw = 2 * lh;
        if (aw > g->iw) { aw = g->iw & ~1; lh = aw / 2; }
        int ax = ix + (g->iw - aw) / 2, ay = iy + (g->ih - lh) / 2;
        for (int r = 0; r < lh; r++) { g_avatar(ax, ay + r, aw, lh, r, v->user_id, nm); strip_part(G_TILES, ax, ay + r, aw, lh, r, PK_AVR, v->user_id); }
    }
    int ny = y + g->ih + 2, right = x + g->tw - 1;
    if (cam) right = g_rtext(right, ny, "[]/", 3, C_TEXT, AT_B) - 1;
    const char *mute = v->self_deaf || v->deaf ? "/x\\" : v->self_mute || v->mute ? "(/)" : NULL;
    if (mute) right = g_rtext(right, ny, mute, 3, C_RED, AT_B) - 1;
    if (cam && v->user_id == S.me_id && g->tw >= 26) {   /* своя камера: что скрыто в уходящем кадре */
        u32 ac;
        const char *al = cu_anon_label(&ac);
        right = g_rtext(right, ny, al, right - x - 8, ac, 0) - 1;
    }
    g_texte(x + 1, ny, nm, right - x - 1, sp ? C_GREEN : C_TEXT, sp ? AT_B : 0);
    if (cam && idx >= 0) { hit_add(x, y, g->tw, g->th, A_BTN, B_CAM_TILE, idx, HV_UL); U.hit[U.nhit - 1].tip = T_CAMVIEW; }
}
/* Центр голосового канала, где вы, когда хоть у кого-то камера: плитки сеткой (камеры первыми), снизу
 * «Отключиться». false — камер нет, рисуется обычный вид. */
static bool cu_voice_grid(int x, int y, int w, int h, const VoiceState *const *list, int k, const VoiceStatus *vs) {
    const VoiceState *ord[16];
    bool cam[16];
    int n = 0, nc = 0;
    for (int i = 0; i < k && i < 16; i++) if (cu_user(list[i])) { ord[n] = list[i]; cam[n++] = true; nc++; }
    if (!nc) return false;
    for (int i = 0; i < k && i < 16; i++) if (!cu_user(list[i])) { ord[n] = list[i]; cam[n++] = false; }
    if (h < 12 || w < 16) return false;
    CamGrid g;
    int gh = h - 5, fit = cam_grid(n, w - 2, gh, &g);
    if (!fit) return false;
    int gw = g.cols * g.tw + g.cols - 1, gt = g.rows * g.th + g.rows - 1;
    int x0 = x + (w - gw) / 2, y0 = y + 1 + (gh - gt) / 2;
    for (int i = 0; i < fit; i++) {
        bool sp = false;
        for (int q = 0; q < vs->nspeaking; q++) if (vs->speaking[q] == ord[i]->user_id) sp = true;
        int row = i / g.cols, col = i % g.cols;
        int inrow = row == g.rows - 1 ? fit - row * g.cols : g.cols;   /* неполный последний ряд — по центру */
        int rx = x0 + (g.cols - inrow) * (g.tw + 1) / 2;
        cu_tu[i] = cam[i] ? ord[i]->user_id : 0;
        cu_tile(rx + col * (g.tw + 1), y0 + row * (g.th + 1), &g, ord[i], cam[i], sp, i);
        if (ord[i]->user_id == S.me_id && cam[i]) cu_center = true;
    }
    if (fit < n) { char t[48]; snprintf(t, sizeof t, TR("и ещё %d"), n - fit); g_ctext(x, y0 + gt, w, t, C_MUTED, 0); }
    const char *bt = TR("Отключиться");
    int bw = vw(bt) + 6, bx = x + (w - bw) / 2, by = y + h - 2;
    g_hbox(bx, by - 1, bw);
    g_hbox(bx, by + 1, bw);
    g_ch(bx, by, '|', C_LINE, 0);
    g_ch(bx + bw - 1, by, '|', C_LINE, 0);
    g_text(bx + 3, by, bt, bw - 6, C_RED, 0);
    hit_add(bx + 1, by, bw - 2, 1, A_BTN, B_LEAVE, 0, HV_RED);
    return true;
}
/* Предпросмотр своей камеры в колонке каналов: сколько строк занять (0 — не показывать). */
static int cu_pv_rows(int sw, int H, bool vpanel, int *iw, int *ih) {
    *iw = *ih = 0;
    if (!vpanel || !cu_on() || cu_center || H < 22 || sw < 16) return 0;
    if (cu_pv_hide) return 1;
    *iw = sw - 4 > 24 ? 24 : sw - 4;
    *ih = *iw * 9 / 32;
    if (*ih < 3) { *iw = *ih = 0; return 1; }
    return 2 + *ih;                              /* заголовок, кадр, строка анонимизации */
}
/* пометка анонимизации своей камеры: текст и цвет (предпросмотр, своя плитка, крупный вид) */
static const char *cu_anon_label(u32 *col) {
    int st = cu_anon_st();
    if (st == CA_FULL) { *col = C_GOLD; return TR("[x] лицо не найдено"); }
    if (st == CA_EYES) { *col = C_GREEN; return TR("[=] глаза скрыты"); }
    if (anon_default()) { *col = C_MUTED; return TR("[=] скрывать глаза: вкл"); }
    *col = C_RED; return TR("[ ] глаза видны");
}
static void cu_pv_draw(int sx, int sw, int py, int iw, int ih) {
    g_text(sx + 1, py, "[]/", 3, C_MUTED, AT_B);
    int r = g_rtext(sx + sw - 2, py, cu_pv_hide || !ih ? TR("показать") : TR("скрыть"), 10, C_MUTED, 0);
    g_texte(sx + 5, py, TR("Ваша камера"), r - sx - 6, C_TEXT, 0);
    hit_add(sx + 1, py, sw - 2, 1, A_BTN, B_CAM_PV, 0, HV_UL);
    U.hit[U.nhit - 1].tip = T_CAMPV;
    if (!ih) return;
    cu_frame(sx + (sw - iw) / 2, py + 1, iw, ih, S.me_id);
    u32 ac;
    const char *al = cu_anon_label(&ac);
    int ar = g_rtext(sx + sw - 2, py + 1 + ih, "Alt+E", 5, C_MUTED, 0);
    g_texte(sx + 1, py + 1 + ih, al, ar - sx - 2, ac, AT_B);
    hit_add(sx + 1, py + 1 + ih, sw - 2, 1, A_BTN, B_ANON, 0, HV_UL);
    U.hit[U.nhit - 1].tip = T_ANON;
}
/* selftest: раскладка плиток и подсказки камеры */
int ui_cam_selftest(void (*check)(bool ok, const char *what)) {
    CamGrid g;
    int f = cam_grid(1, 60, 24, &g);
    check(f == 1 && g.cols == 1 && g.iw <= 58 && g.ih == g.iw * 9 / 32 && g.th <= 24, "камера: одна плитка - кадр 16:9 во всю ширину или высоту");
    f = cam_grid(4, 60, 24, &g);
    check(f == 4 && g.cols * g.rows >= 4 && g.cols * g.tw + g.cols - 1 <= 60 && g.rows * g.th + g.rows - 1 <= 24, "камера: 4 плитки помещаются в 60x24");
    f = cam_grid(3, 80, 20, &g);
    check(f == 3 && g.rows == 1, "камера: 3 плитки в широком центре - одним рядом");
    f = cam_grid(16, 30, 12, &g);
    check(f >= 1 && f < 16 && g.cols * g.tw + g.cols - 1 <= 30 && g.rows * g.th + g.rows - 1 <= 12, "камера: 16 плиток в 30x12 - сколько влезло, без выхода за край");
    check(cam_grid(2, 8, 4, &g) == 0, "камера: слишком мало места - плиток нет");
    bool ok = true;
    for (int n = 1; n <= 16; n++) for (int w = 16; w <= 120; w += 13) for (int h = 6; h <= 40; h += 7) {
        int k = cam_grid(n, w, h, &g);
        if (k && (k > n || g.cols * g.tw + g.cols - 1 > w || g.rows * g.th + g.rows - 1 > h || g.iw < 10 || g.ih < 3 || g.cols * g.rows < k)) ok = false;
    }
    check(ok, "камера: раскладка 1..16 плиток в 16..120 x 6..40 клеток - внутри области");
    check(!strcmp(TIPS[T_CAM], TIP_CAM) && strstr(TIPS[T_CAM], "F12") && strstr(TIPS[T_CAMOFF], "F12"), "камера: подсказки кнопки и индикатора называют клавишу F12");
    bool a0 = anon_default();
    u32 ac = 0;
    anon_set_default(false);
    check(!strcmp(cu_anon_label(&ac), "[ ] глаза видны") && ac == C_RED && cu_anon_st() == CA_OFF, "камера: анонимизация выключена - пометка красным «глаза видны»");
    anon_set_default(a0);
    check(strstr(TIPS[T_ANON], "Alt+E") != NULL, "камера: подсказка анонимизации называет Alt+E");
    bool en = g_lang_en;
    g_lang_en = true;
    const char *e1 = tr(TIP_CAM), *e2 = tr(" []/ КАМЕРА %dp %.0f к/с ");
    check(strcmp(e1, TIP_CAM) && strstr(e1, "F12") && strcmp(e2, " []/ КАМЕРА %dp %.0f к/с "), "камера: подсказка и индикатор переведены");
    g_lang_en = en;
    return 0;
}
#endif
/* Экран голосового канала (эталон владельца): тот же тёмно-серый фон, крупный ASCII-логотип — значок сервера
 * (в окне мелкой сеткой), название, «Сейчас в голосовом чате никого нет» или плитки участников,
 * кнопка «Присоединиться к голосовому каналу» в рамке +----+. */
static void draw_voice_view(int x, int y, int w, int h, const Channel *c, const VoiceStatus *vs) {
    g_rect(x, y, w, h, C_FEED);
    char lab[96];
    chan_label(c, lab, sizeof lab);
    const VoiceState *list[16];
    int k = voice_members(c->id, list, 16);
    bool here = vs->phase != VS_IDLE && vs->channel_id == c->id;
#ifdef ASTRA_VIDEO
    if (here && cu_voice_grid(x, y, w, h, list, k, vs)) return;   /* у кого-то камера — плитки сеткой */
#endif
    Guild *g = c->guild_id ? guild_find(c->guild_id) : NULL;
    int lh = k ? (h >= 24 ? 6 : 3) : h >= 30 ? 9 : h >= 24 ? 7 : h >= 18 ? 5 : 0;   /* высота логотипа или аватаров плиток (12x6) */
    int lw = 2 * lh;
    if (lw > w - 4) { lh = (w - 4) / 2; lw = 2 * lh; }
    int blk = lh + (lh ? 1 : 0) + 6;               /* логотип, промежуток, название, подпись, промежуток, кнопка 3 строки */
    int top = y + (h - blk) / 2;
    if (top < y) top = y;
    if (k) {
        /* участники плитками: круглый аватар 6x3 и имя */
        int aw = 2 * lh, tile = aw + 6, per = w / tile, shown = k < per ? k : per;
        int x0 = x + (w - shown * tile) / 2;
        for (int i = 0; i < shown; i++) {
            const char *nm = list[i]->name ? list[i]->name : user_name(list[i]->user_id);
            bool sp = false;
            for (int q = 0; q < vs->nspeaking; q++) if (vs->speaking[q] == list[i]->user_id) sp = true;
            int tx = x0 + i * tile;
            for (int r = 0; r < lh; r++) {
                g_avatar(tx + 3, top + r, aw, lh, r, list[i]->user_id, nm);
                strip_part(G_TILES, tx + 3, top + r, aw, lh, r, PK_AVR, list[i]->user_id);
            }
            /* говорит: зелёные скобки (в окне аватар под слоем — скобки снаружи плитки, имя зелёным) */
            if (sp && !fine_on) for (int r = 0; r < lh; r++) { g_ch(tx + 2, top + r, '(', C_GREEN, AT_B); g_ch(tx + 3 + aw, top + r, ')', C_GREEN, AT_B); }
            g_ctext(tx, top + lh, tile, nm, sp ? C_GREEN : C_TEXT, sp ? AT_B : 0);
        }
    } else if (lh) {
        int lx = x + (w - lw) / 2;
        static ImgCell ic[24 * 12];
        bool pic = lw * lh <= (int)(sizeof ic / sizeof *ic) && pic_icon(g, lw, lh, ic);
        char ab[32];
        abbr(g ? g->name : lab, ab, sizeof ab);
        for (int r = 0; r < lh; r++) {
            if (pic) { g_imgrow(lx, top + r, lw, r, ic); strip_part(G_LOGO, lx, top + r, lw, lh, r, PK_ICON, g->id); }
            else {
                /* нет значка — сокращение названия в скобках; значок качается — серый квадрат без букв */
                g_rect(lx, top + r, lw, 1, g && g->icon[0] ? C_PH : C_BTN2);
                if (!(g && g->icon[0])) {
                    g_ch(lx, top + r, '[', C_MUTED, AT_B); g_ch(lx + lw - 1, top + r, ']', C_MUTED, AT_B);
                    if (r == lh / 2) g_ctext(lx, top + r, lw, ab, C_WHITE, AT_B);
                }
            }
        }
    }
    int ty = top + lh + (lh ? 1 : 0) + (k ? 1 : 0);
    g_ctext(x, ty, w, lab, C_WHITE, AT_B);
    if (!k) g_ctext(x, ty + 1, w, TR("Сейчас в голосовом чате никого нет"), C_TEXT, 0);
    const char *bt = here ? TR("Отключиться") : TR("Присоединиться к голосовому каналу");
    if (vw(bt) + 6 > w - 2) bt = here ? TR("Отключиться") : TR("Присоединиться");
    int bw = vw(bt) + 6;
    if (bw > w - 2) bw = w - 2;
    int bx = x + (w - bw) / 2, by = ty + 3;
    g_hbox(bx, by - 1, bw);
    g_hbox(bx, by + 1, bw);
    g_ch(bx, by, '|', C_LINE, 0);
    g_ch(bx + bw - 1, by, '|', C_LINE, 0);
    g_text(bx + 3, by, bt, bw - 6, here ? C_RED : C_WHITE, 0);
    hit_add(bx + 1, by, bw - 2, 1, A_BTN, here ? B_LEAVE : B_JOIN, 0, here ? HV_RED : HV_BG);
    if (!here && !c->can_connect) g_ctext(x, by + 2, w, TR("нет права подключаться к этому каналу"), C_RED, 0);
}

/* ======================= друзья, новая ЛС, поиск (фаза 4) =======================
 * Ничего не кэшируется: списки собираются в каждом кадре из rel_list()/search_state() (store.c), временный
 * массив индексов освобождается сразу. Аватары — те же круги img.c (в окне мелкой сеткой). */
static const char *const FR_TABS[] = { "В сети", "Все", "Ожидание", "Заблокированные" };
static const char *const FR_TABS_S[] = { "В сети", "Все", "Ожид.", "Блок." };
static const char *un(u64 uid) { const char *n = user_name(uid); return n && *n ? n : "?"; }
static const char *status_text(const Relation *r) {
    switch (r->type) {
    case REL_IN: return TR("Входящий запрос дружбы");
    case REL_OUT: return TR("Исходящий запрос дружбы");
    case REL_BLOCKED: return TR("Заблокирован");
    default: break;
    }
    return r->status == PS_ONLINE ? TR("В сети") : r->status == PS_IDLE ? TR("Не активен") : r->status == PS_DND ? TR("Не беспокоить") : TR("Не в сети");
}
static u32 status_col(int st) { return st == PS_ONLINE ? C_GREEN : st == PS_IDLE ? C_GOLD : st == PS_DND ? C_RED : 0x80848eu; }
static bool fr_in_tab(const Relation *r, int tab) {
    switch (tab) {
    case 0: return r->type == REL_FRIEND && r->status != PS_OFFLINE;
    case 1: return r->type == REL_FRIEND;
    case 2: return r->type == REL_IN || r->type == REL_OUT;
    default: return r->type == REL_BLOCKED;
    }
}
/* Строчная буква (латиница, кириллица) — для поиска друга по имени без учёта регистра. */
static u32 lower_cp(u32 c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c == 0x401) return 0x451;
    return c;
}
static bool name_has(const char *s, const char *q) {
    if (!q || !*q) return true;
    for (const char *p = s; *p; ) {
        const char *a = p, *b = q;
        while (*a && *b) {
            u32 ca, cb;
            int ka = utf8_dec(a, &ca), kb = utf8_dec(b, &cb);
            if (lower_cp(ca) != lower_cp(cb)) break;
            a += ka; b += kb;
        }
        if (!*b) return true;
        u32 c;
        p += utf8_dec(p, &c);
    }
    return false;
}
static const Relation *fr_rels;
static int fr_cmp(const void *a, const void *b) {
    const Relation *x = &fr_rels[*(const int *)a], *y = &fr_rels[*(const int *)b];
    if (x->type != y->type) return x->type == REL_IN ? -1 : y->type == REL_IN ? 1 : x->type - y->type;   /* входящие выше */
    return strcmp(un(x->id), un(y->id));
}
/* Индексы (в rel_list) записей вкладки tab, по имени; filter — часть имени. *out освобождает вызывающий. */
static int fr_collect(int tab, const char *filter, int **out) {
    const Relation *r;
    int n = rel_list(&r), k = 0;
    int *ix = xmalloc((size_t)(n + 1) * sizeof(int));
    for (int i = 0; i < n; i++) if (fr_in_tab(&r[i], tab) && name_has(un(r[i].id), filter)) ix[k++] = i;
    fr_rels = r;
    qsort(ix, (size_t)k, sizeof(int), fr_cmp);
    *out = ix;
    return k;
}
/* Экран «Друзья» виден: главная ЛС без открытой беседы. */
static bool friends_screen(void) { return !U.settings && !U.help && U.sel_guild == 0 && !chan_find(S.open_ch) && S.ready; }
/* Кнопка-плашка в строке друга справа (x_end — клетка после неё); вернуть x начала. */
static int fr_btn(int x_end, int y, const char *t, u32 fg, int sub, int arg, int tip) {
    int w = vw(t) + 4, x = x_end - w;
    g_pill(x, y, w, C_RAIL);
    g_text(x + 2, y, t, w - 4, fg, AT_B);
    hit_add(x, y, w, 1, A_FRIEND, sub, arg, HV_PILL);
    if (tip) U.hit[U.nhit - 1].tip = (u8)tip;
    return x;
}
/* №109: «Запросы на общение» — вкладки «Запросы» и «Спам»; принять / отклонить; щелчок по строке — открыть
 * беседу (картинки и аватары в ней — заглушки до «Принять»). */
static struct { bool on; int tab; } RQ;
static int rq_list(int tab, int *out, int max) {
    int n = 0;
    for (int i = 0; i < S.nchans && n < max; i++) {
        const Channel *c = &S.chans[i];
        if (c->guild_id || !dm_held(c)) continue;
        bool spam = (c->dmflags & DMF_SPAM) != 0;
        if (spam == (tab == 1)) out[n++] = i;
    }
    return n;
}
static void rq_answer(int ci, bool ok) {
    if (ci < 0 || ci >= S.nchans) return;
    char err[160] = "";
    u64 cid = S.chans[ci].id;
    bool r = ok ? dm_request_accept(cid, err, sizeof err) : dm_request_reject(cid, err, sizeof err);
    if (!r && err[0]) { char *e = san(err, false); ui_notice("%s", e); free(e); }
    else ui_notice(ok ? TR("запрос принят") : TR("запрос отклонён"));
    rebuild_rows();
}
static int rq_btn(int x_end, int y, const char *t, u32 fg, int sub, int arg) {
    int w = vw(t) + 4, x = x_end - w;
    g_pill(x, y, w, C_RAIL);
    g_text(x + 2, y, t, w - 4, fg, AT_B);
    hit_add(x, y, w, 1, A_BTN, sub, arg, HV_PILL);
    return x;
}
static void draw_requests(int cx, int cw, int H) {
    int ix[64];
    int nr = rq_list(0, ix, 64), ns = rq_list(1, ix, 64);
    g_text(cx + 2, 0, "?!", 2, C_MUTED, AT_B);
    int x = cx + 5 + g_text(cx + 5, 0, TR("Запросы на общение"), cw - 8, C_WHITE, AT_B) + 2;
    char t[64];
    for (int tb = 0; tb < 2; tb++) {
        snprintf(t, sizeof t, "%s %d", tb ? TR("Спам") : TR("Запросы"), tb ? ns : nr);
        int w = vw(t) + 2;
        if (x + w > cx + cw - 2) break;
        if (RQ.tab == tb) g_pill(x, 0, w, C_SEL);
        g_text(x + 1, 0, t, w - 2, RQ.tab == tb ? C_WHITE : C_MUTED, AT_B);
        hit_add(x, 0, w, 1, A_BTN, B_RQTAB, tb, HV_PILL);
        x += w + 1;
    }
    int n = rq_list(RQ.tab, ix, 64), y = 2;
    g_texte(cx + 2, y, RQ.tab ? TR("Спам: картинки и аватары не загружаются, пока вы не примете запрос.") :
                               TR("Сообщения от людей не из друзей. Картинки и аватары - после 'Принять'."), cw - 4, C_MUTED, 0);
    y += 2;
    if (!n) { g_text(cx + 2, y, RQ.tab ? TR("Спама нет.") : TR("Запросов нет."), cw - 4, C_MUTED, 0); }
    for (int i = 0; i < n && y < H - 2; i++, y += 2) {
        const Channel *c = &S.chans[ix[i]];
        char lab[96];
        chan_label(c, lab, sizeof lab);
        g_text(cx + 2, y, "[?]", 3, C_MUTED, AT_B);  /* аватар — заглушка */
        int bx = rq_btn(cx + cw - 2, y, TR("Отклонить"), C_RED, B_RQ_NO, ix[i]);
        bx = rq_btn(bx - 1, y, TR("Принять"), C_GREEN, B_RQ_OK, ix[i]);
        g_texte(cx + 6, y, lab, bx - cx - 8, C_WHITE, AT_B);
        hit_add(cx + 1, y, bx - cx - 2, 1, A_OPENCH, 0, ix[i], HV_PILL);
        for (int q = cx + 2; q < cx + cw - 2; q++) g_ch(q, y + 1, '-', C_LINE, 0);
    }
    draw_status(cx + 2, cx + cw - 3, H - 1);
}
static void draw_friends(int cx, int cw, int H) {
    g_rect(cx, 0, cw, H, C_FEED);
    const Relation *rl;
    int nrl = rel_list(&rl), nin = 0;
    for (int i = 0; i < nrl; i++) if (rl[i].type == REL_IN) nin++;
    bool foc = U.focus == F_MESSAGES;
    /* шапка: oo Друзья | В сети  Все  Ожидание (1)  Заблокированные  (Добавить в друзья) */
    int x = cx + 1;
    x += g_text(x, 0, "oo", 2, C_MUTED, AT_B) + 1;
    x += g_text(x, 0, TR("Друзья"), 8, C_WHITE, AT_B) + 1;
    g_ch(x, 0, '|', C_LINE, 0);
    x += 2;
    char nb[12];
    snprintf(nb, sizeof nb, "%d", nin > 99 ? 99 : nin);
    int full = 0;
    for (int i = 0; i < 4; i++) full += vw(tr(FR_TABS[i])) + 3 + (i == 2 && nin ? vw(nb) + 3 : 0);
    const char *const *tabs = x + full + 20 <= cx + cw ? FR_TABS : FR_TABS_S;
    int fulls = 0;
    for (int i = 0; i < 4; i++) fulls += vw(tr(FR_TABS_S[i])) + 3 + (i == 2 && nin ? vw(nb) + 3 : 0);
    if (x + fulls > cx + cw - 1) {
        /* узко: не все вкладки влезают — переключатель «< Вкладка (1) >», стрелки — соседние вкладки */
        int t = U.fr_tab & 3;
        int bw = t == 2 && nin ? vw(nb) + 3 : 0, w = vw(tr(FR_TABS[t])) + 2 + bw;
        if (x + 4 + w + 2 > cx + cw) { tabs = FR_TABS_S; w = vw(tr(FR_TABS_S[t])) + 2 + bw; }
        if (x + 4 + w + 2 > cx + cw) {           /* совсем узко: без «oo Друзья |» — только «< Вкладка >» */
            g_rect(cx + 1, 0, x - cx - 1, 1, bg_at(cx + 1, 0));
            x = cx + 1;
        }
        if (x + 4 + w + 2 > cx + cw) w = cx + cw - x - 6;   /* и подпись — с «...» */
        g_text(x, 0, "<", 1, C_TEXT, AT_B);
        hit_add(x, 0, 1, 1, A_FRTAB, 0, (t + 3) % 4, HV_UL);
        x += 2;
        g_pill(x, 0, w, C_SEL);
        g_texte(x + 1, 0, tr(tabs[t]), w - 2 - bw, C_WHITE, AT_B | (foc ? AT_U : 0));
        if (bw) { g_pill(x + w - bw, 0, bw, C_RED); g_text(x + w - bw + 1, 0, nb, bw - 2, C_WHITE, AT_B); }
        x += w + 1;
        g_text(x, 0, ">", 1, C_TEXT, AT_B);
        hit_add(x, 0, 1, 1, A_FRTAB, 0, (t + 1) % 4, HV_UL);
        x += 2;
        tabs = NULL;
    }
    for (int i = 0; i < 4 && tabs; i++) {
        int bw = i == 2 && nin ? vw(nb) + 3 : 0, w = vw(tr(tabs[i])) + 2 + bw;
        if (x + w > cx + cw - 1) break;
        bool on = i == U.fr_tab;
        if (on) g_pill(x, 0, w, C_SEL);
        g_text(x + 1, 0, tr(tabs[i]), w - 2 - bw, on ? C_WHITE : C_MUTED, AT_B | (on && foc ? AT_U : 0));
        if (bw) { g_pill(x + w - bw, 0, bw, C_RED); g_text(x + w - bw + 1, 0, nb, bw - 2, C_WHITE, AT_B); }
        hit_add(x, 0, w, 1, A_FRTAB, 0, i, HV_PILL);
        x += w + 1;
    }
    if (x + 20 <= cx + cw) {
        g_pill(x, 0, 19, C_GREEN);
        g_text(x + 1, 0, TR("Добавить в друзья"), 17, C_WHITE, AT_B);
        hit_add(x, 0, 19, 1, A_BTN, B_ADDFRIEND, 0, HV_PILL);
        U.hit[U.nhit - 1].tip = T_FRTAB;
        x += 20;
    }
    g_uline(x, 0, cx + cw - x);                  /* линия по низу шапки — правее вкладок (между ними не рисуется) */
    /* справа «Активные контакты» (широкое окно), как в Discord */
    int acw = cw >= 110 ? 32 : 0;
    if (acw) {
        int ax = cx + cw - acw;
        for (int y = 1; y < H; y++) g_ch(ax - 1, y, '|', C_LINE, 0);
        g_text(ax + 1, 2, TR("Активные контакты"), acw - 2, C_WHITE, AT_B);
        g_ctext(ax, 4, acw, TR("Пока что тут тихо..."), C_WHITE, AT_B);
        char **ls;
        int m = help_wrap(TR("Когда друг начнёт общаться в голосовом чате, мы покажем это здесь."), acw - 4, &ls);
        for (int i = 0; i < m; i++) { if (5 + i < H - 1) g_ctext(ax, 5 + i, acw, ls[i], C_MUTED, 0); free(ls[i]); }
        free(ls);
    }
    int lx = cx + 2, lw = cw - 4 - (acw ? acw + 1 : 0);
    int *ix;
    int nfr = 0;
    for (int i = 0; i < nrl; i++) if (rl[i].type == REL_FRIEND) nfr++;
    int n = fr_collect(U.fr_tab, NULL, &ix);
    if (!U.fr_seen) {
        U.fr_seen = true;
        if (U.fr_tab == 0 && !n && nfr) { free(ix); U.fr_tab = 1; U.fr_sel = U.fr_scroll = 0; n = fr_collect(1, NULL, &ix); }
    }
    static const char *const cap[] = { "В СЕТИ", "ВСЕ ДРУЗЬЯ", "ОЖИДАНИЕ", "ЗАБЛОКИРОВАНО" };
    char t[96];
    snprintf(t, sizeof t, "%s - %d", tr(cap[U.fr_tab & 3]), n);
    g_text(lx + 1, 2, t, lw - 2, C_MUTED, AT_B);
    int top = 3, bot = H - 2, per = (bot - top + 1) / 3;
    if (per < 1) per = 1;
    if (U.fr_sel >= n) U.fr_sel = n - 1;
    if (U.fr_sel < 0) U.fr_sel = 0;
    if (U.fr_sel < U.fr_scroll) U.fr_scroll = U.fr_sel;
    if (U.fr_sel >= U.fr_scroll + per) U.fr_scroll = U.fr_sel - per + 1;
    if (U.fr_scroll > n - per) U.fr_scroll = n - per;
    if (U.fr_scroll < 0) U.fr_scroll = 0;
    hit_add(cx, top, lw + 4, bot - top + 1, A_AR_LIST, 0, 0, 0);
    if (!n) {
        /* пусто: ASCII-заглушка (вместо картинки Discord) и подпись; «В сети» при друзьях не в сети — кнопка «Все» */
        static const char *const art[] = { "   .-\"\"-.   ", "  / o  o \\  ", " |   __   | ", "  \\      /  ", "   '-..-'   " };
        static const char *const empty[] = { "Никого нет в сети. Ждём друзей!", "Друзей пока нет.",
                                             "Нет ожидающих запросов дружбы.", "Вы никого не заблокировали." };
        int ay = top + (bot - top) / 2 - 5;
        if (ay < top) ay = top;
        if (bot - top >= 10) for (int i = 0; i < 5; i++) g_ctext(lx, ay + i, lw, art[i], C_MUTED, 0);
        else ay = top - 6;
        g_ctext(lx, ay + 6, lw, tr(empty[U.fr_tab & 3]), C_MUTED, 0);
        if (U.fr_tab == 0 && nfr) {
            char tt[96];
            snprintf(tt, sizeof tt, TR("Всего друзей: %d - вкладка 'Все'"), nfr);
            g_ctext(lx, ay + 7, lw, tt, C_MUTED, 0);
            snprintf(tt, sizeof tt, TR("Все друзья (%d)"), nfr);
            int bw = vw(tt) + 4, bx = lx + (lw - bw) / 2;
            if (ay + 9 <= bot) { g_button(bx, ay + 9, tt, lw, 1, B_NOP); if (U.nhit) { U.hit[U.nhit - 1].act = A_FRTAB; U.hit[U.nhit - 1].arg = 1; } }
        }
    }
    for (int k = 0; k < per && U.fr_scroll + k < n; k++) {
        int i = ix[U.fr_scroll + k], y = top + k * 3;
        const Relation *r = &rl[i];
        for (int q = lx; q < lx + lw; q++) g_ch(q, y, '-', 0x3f4147u, 0);
        bool sel = foc && U.fr_scroll + k == U.fr_sel;
        if (sel) { g_pill(lx, y + 1, lw, C_HOV); g_rect(lx + 1, y + 2, lw - 2, 1, C_HOV); }
        hit_add(lx, y + 1, lw, 2, A_FRIEND, FA_ROW, i, HV_MSG);
        for (int r2 = 0; r2 < 2; r2++) { g_avatar(lx + 1, y + 1 + r2, 4, 2, r2, r->id, NULL); strip_part(G_MEMB, lx + 1, y + 1 + r2, 4, 2, r2, PK_AVR, r->id); }
        if (r->type == REL_FRIEND) g_cap(fine_on ? lx + 5 : lx + 4, y + 2, 'o', status_col(r->status), bg_at(lx + 5, y + 2));
        /* кнопки справа; имя и статус — что осталось. Узко (< 34) — кнопки короче и в строке статуса,
         * чтобы имя было видно целиком */
        bool nar = lw < 34;
        int by = nar ? y + 2 : y + 1, bx = lx + lw - 1;
        switch (r->type) {
        case REL_FRIEND:
            bx = fr_btn(bx, by, "...", C_TEXT, FA_MORE, i, T_MORE) - 1;
            if (bx - lx >= 30) bx = fr_btn(bx, by, TR("Написать"), C_TEXT, FA_MSG, i, T_WRITE) - 1;
            break;
        case REL_IN:
            bx = fr_btn(bx, by, nar ? "x" : TR("Отклонить"), C_RED, FA_DECLINE, i, 0) - 1;
            if (nar || bx - lx >= 28) bx = fr_btn(bx, by, nar ? "+" : TR("Принять"), C_GREEN, FA_ACCEPT, i, 0) - 1;
            break;
        case REL_OUT: bx = fr_btn(bx, by, nar ? "x" : TR("Отменить"), C_TEXT, FA_DECLINE, i, 0) - 1; break;
        default: bx = fr_btn(bx, by, nar ? TR("Разбл.") : TR("Разблокировать"), C_TEXT, FA_UNBLOCK, i, 0) - 1; break;
        }
        g_texte(lx + 7, y + 1, un(r->id), (nar ? lx + lw - 1 : bx) - lx - 8, C_WHITE, AT_B);
        g_texte(lx + 7, y + 2, status_text(r), (nar ? bx : lx + lw - 1) - lx - 8, C_MUTED, 0);
    }
    free(ix);
    /* VLESS: окно «нет связи» / ввод ссылки / подтверждение — на «Друзьях» нет поля ввода, поэтому ссылка
     * показывается в самом окне (звёздочками, открыт только vless://) */
    if (vless_state() == VL_DOWN || U.mode == M_VLINK || U.mode == M_VLOFF) {
        const char *cl[8];
        int cn = 0, nbt = 0;
        CBtn cb[3];
        char why[200] = "", fld[96], cnt[80];
        bool down = vless_state() == VL_DOWN;
        cl[cn++] = down ? TR("VLESS: нет связи") : U.mode == M_VLOFF ? TR("Отключить VLESS") : TR("Ссылка VLESS");
        if (down) {
            char *r = san(streamer_on() ? TR("подробности скрыты (режим стримера)") : U.vl_note[0] ? U.vl_note : vless_error(), false);
            deco_ascii(*r ? r : TR("туннель не отвечает"), why, sizeof why);
            free(r);
            cl[cn++] = why;
        }
        if (U.busy[0]) { cl[cn++] = U.busy; cl[cn++] = TR("ждите до 10 с"); }
        else if (U.mode == M_VLINK) {
            int nch = in_chars(), shown = 0;
            size_t o = 0;
            bool pre = U.in.len >= 8 && !strncmp(U.in.p, "vless://", 8);
            if (pre) { memcpy(fld, "vless://", 8); o = 8; shown = 8; }
            for (; shown < nch && o + 4 < sizeof fld && shown < 48; shown++) fld[o++] = '*';
            if (shown < nch) { memcpy(fld + o, "...", 3); o += 3; }
            fld[o++] = '_';
            fld[o] = 0;
            snprintf(cnt, sizeof cnt, TR("%d симв., на экране не видна"), nch);
            cl[cn++] = TR("вставьте ссылку (Ctrl+V) или наберите:");
            cl[cn++] = fld;
            cl[cn++] = cnt;
            cb[nbt++] = (CBtn){ TR("Назад (Esc)"), B_VL_BACK, 0 };
            cb[nbt++] = (CBtn){ TR("Проверить (Enter)"), B_VL_CHECK, 1 };
        } else if (U.mode == M_VLOFF) {
            cl[cn++] = TR("трафик пойдёт напрямую, Discord увидит ваш IP");
            cb[nbt++] = (CBtn){ TR("Отмена"), B_NO, 0 };
            cb[nbt++] = (CBtn){ TR("Отключить (y)"), B_YES, 2 };
        } else {
            cl[cn++] = TR("напрямую клиент не ходит - выберите:");
            cb[nbt++] = (CBtn){ TR("Отключить (D)"), B_VL_D, 2 };
            cb[nbt++] = (CBtn){ TR("Сменить ссылку (C)"), B_VL_C, 0 };
            cb[nbt++] = (CBtn){ TR("Переподключиться (R)"), B_VL_R, 1 };
        }
        g_card(cx, 1, cw, H - 2, -1, cl, cn, cb, nbt, down ? C_RED : C_WHITE, true);
    }
    if (U.mode == M_UNFRIEND) {
        char l1[160];
        snprintf(l1, sizeof l1, TR("Вы уверены, что хотите удалить %s из друзей?"), un(U.target));
        const char *l2[2] = { TR("Удалить из друзей"), l1 };
        CBtn b2[2] = { { TR("Отмена"), B_NO, 0 }, { TR("Удалить друга (y)"), B_YES, 2 } };
        g_card(cx, 1, cw, H - 2, -1, l2, 2, b2, 2, C_WHITE, true);
    }
    draw_status(cx, cx + cw, H - 1);
}

/* «Выберите друзей» — новая личная беседа («+» у «Личные сообщения», «Найти или начать беседу»):
 * окно под «+», фильтр по имени, список друзей, «Создать ЛС». Группы mincord не создаёт: один друг. */
static void draw_picker(int W, int H, int px, int py) {
    int *ix;
    int n = fr_collect(1, U.pick_q, &ix);
    int bw = 46;
    if (bw > W - 2) bw = W - 2;
    if (px + bw > W - 1) px = W - 1 - bw;
    if (px < 0) px = 0;
    int rows = n < 1 ? 1 : n > 8 ? 8 : n, bh = rows + 9;
    if (bh > H - 2) { rows -= bh - (H - 2); bh = H - 2; }
    if (rows < 1) { free(ix); return; }
    if (py + bh > H - 1) py = H - 1 - bh;
    if (py < 1) py = 1;
    ovl_cut(px, py, bw, bh);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_MENU);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    hit_add(px, py, bw, bh, A_BLOCK, 0, 0, 0);
    g_text(px + 3, py + 1, TR("Выберите друзей"), bw - 8, C_WHITE, AT_B);
    g_text(px + bw - 4, py + 1, "x", 1, C_TEXT, AT_B);
    hit_add(px + bw - 5, py + 1, 3, 1, A_BTN, B_PICK_X, 0, HV_RED);
    g_text(px + 3, py + 2, TR("Личная беседа с одним другом"), bw - 6, C_MUTED, 0);
    /* поле фильтра: курсор всегда в конце */
    g_pill(px + 2, py + 4, bw - 4, C_RAIL);
    int fw = bw - 8, qw = vw(U.pick_q), skip = qw > fw - 1 ? qw - (fw - 1) : 0, acc = 0, used = 0;
    for (const char *p = U.pick_q; *p; ) {
        u32 c;
        p += utf8_dec(p, &c);
        int cw2 = shown_w(c);
        if (acc >= skip && used + cw2 <= fw - 1) { g_put(px + 4 + used, py + 4, c, cw2, C_TEXT, 0); used += cw2; }
        acc += cw2;
    }
    g_ch(px + 4 + used, py + 4, ' ', C_TEXT, AT_I);
    if (!U.pick_q[0]) g_texte_w(px + 5, py + 4, TR("Введите имя пользователя друга"), fw - 1, C_MUTED, 0, false);
    if (U.pick_sel >= n) U.pick_sel = n - 1;
    if (U.pick_sel < 0) U.pick_sel = 0;
    int ps = U.pick_sel >= rows ? U.pick_sel - rows + 1 : 0;
    if (!n) g_ctext(px, py + 6, bw, U.pick_q[0] ? TR("Друзей с таким именем нет") : TR("У вас пока нет друзей"), C_MUTED, 0);
    for (int k = 0; k < rows && ps + k < n; k++) {
        int idx = ps + k, y = py + 6 + k;
        const Relation *r = &fr_rels[ix[idx]];
        bool s = idx == U.pick_sel;
        if (s) g_pill(px + 2, y, bw - 4, C_SEL);
        g_listav(px + 3, y, r->id, NULL, G_POP);
        g_texte(px + 8, y, un(r->id), bw - 16, s ? C_WHITE : C_TEXT, s ? AT_B : 0);
        g_text(px + bw - 7, y, s ? "[x]" : "[ ]", 3, s ? C_WHITE : C_MUTED, AT_B);
        hit_add(px + 2, y, bw - 4, 1, A_PICK, 0, ix[idx], HV_PILL);
    }
    int by = py + bh - 2;
    g_pill(px + 2, by, bw - 4, n ? C_ACC : C_BTN2);
    g_ctext(px + 2, by, bw - 4, TR("Создать ЛС"), n ? C_WHITE : C_MUTED, AT_B);
    if (n) hit_add(px + 2, by, bw - 4, 1, A_BTN, B_PICK_GO, 0, HV_PILL);
    free(ix);
}

/* Карточка профиля (как всплывающий профиль Discord): баннер цветом профиля, круглый аватар 6x3, имя, логин и
 * местоимения, отношение и статус, «О СЕБЕ», «В DISCORD С» / «НА СЕРВЕРЕ С», роли; кнопки «Написать», «В друзья»,
 * «Блок», «ID». Профиль запрошен по щелчку (profile_open), без кэша; пока грузится — то, что есть в store. */
static void date_short(i64 unix_s, char *out, size_t n) {
    static const char *const mon[] = { "янв.", "февр.", "мар.", "апр.", "мая", "июн.", "июл.", "авг.", "сент.", "окт.", "нояб.", "дек." };
    struct tm t;
    plat_localtime(unix_s, &t);
    snprintf(out, n, TR("%d %s %d г."), t.tm_mday, tr(mon[(t.tm_mon % 12 + 12) % 12]), t.tm_year + 1900);
}
static void draw_card(int W, int H) {
    u64 uid = U.card_uid;
    const Profile *pf = profile_state();
    bool have = pf && pf->id == uid && !pf->busy && !pf->error[0];
    int bw = 42;
    if (bw > W - 2) bw = W - 2;
    char **bio = NULL;
    int nbio = 0;
    if (have && pf->bio && *pf->bio) {
        char *b = san(pf->bio, false);
        nbio = help_wrap(b, bw - 4, &bio);
        free(b);
        if (nbio > 4) { for (int i = 4; i < nbio; i++) free(bio[i]); nbio = 4; }
    }
    /* №99: полный профиль — по кнопке; общие серверы, друзья, связи — строками внизу */
    int ncn = have && pf->full ? (pf->nconn < 3 ? pf->nconn : 3) : 0;
    int extra = uid == S.me_id ? 0 : !(have && pf->full) ? 1 : 2 + (ncn ? 1 + ncn : 0);
    const Channel *mc0 = chan_find(S.open_ch);
    u64 mgid = mc0 ? mc0->guild_id : 0;
    bool modrow = mgid && uid != S.me_id && (mod_can(mgid, MOD_TIMEOUT) || mod_can(mgid, MOD_KICK) || mod_can(mgid, MOD_BAN) || mod_can(mgid, MOD_ROLES));
    if (modrow) extra += 2;
    const NoteState *ns = note_state();           /* №100 личная заметка: по кнопке, живёт, пока открыта карточка */
    bool noteon = uid != S.me_id && ns && ns->uid == uid;
    int nnote = 0;
    char **nl = NULL;
    if (noteon && !ns->busy && !ns->err[0] && ns->text[0]) { char *nt = san(ns->text, false); nnote = help_wrap(nt, bw - 4, &nl); free(nt); if (nnote > 3) { for (int i = 3; i < nnote; i++) free(nl[i]); nnote = 3; } }
    int note_rows = uid != S.me_id ? (noteon ? 1 + (nnote ? nnote : 1) : 1) : 0;
    extra += note_rows;
    int bh = 13 + extra + (nbio ? nbio + 2 : 0) + (have && pf->nroles ? 1 : 0) + (have && pf->member_since_unix > 0 ? 1 : 0);
    if (bh > H - 2) bh = H - 2;
    if (bh < 10) { for (int i = 0; i < nbio; i++) free(bio[i]); free(bio); return; }
    int px = U.card_x + 2, py = U.card_y - bh / 2;
    if (px + bw > W - 1) px = U.card_x - bw - 1;
    if (px < 0) px = 0;
    if (py + bh > H - 1) py = H - 1 - bh;
    if (py < 1) py = 1;
    ovl_cut(px, py, bw, bh);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_MENU);
    g_rect(px, py, bw, 2, have && pf->accent ? pf->accent : av_color(uid));   /* баннер — цвет профиля */
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    hit_add(px, py, bw, bh, A_BLOCK, 0, 0, 0);
    g_text(px + bw - 3, py, "x", 1, C_WHITE, AT_B);
    hit_add(px + bw - 4, py, 3, 1, A_BTN, B_CARD_X, 0, HV_RED);
    for (int r = 0; r < 3; r++) { g_avatar(px + 2, py + 1 + r, 6, 3, r, uid, NULL); strip_part(G_POP, px + 2, py + 1 + r, 6, 3, r, PK_AVR, uid); }
    hit_add(px + 2, py + 1, 6, 3, A_BTN, B_AVBIG, 0, HV_ICON);   /* №101: открыть крупно */
    const char *nm = have && pf->name && *pf->name ? pf->name : uid == S.me_id && S.me_name ? S.me_name : user_name(uid);
    int y = py + 5;
    int u = g_texte(px + 2, y, nm && *nm ? nm : "?", bw - 10, C_WHITE, AT_B);
    if (have && pf->bot) { g_pill(px + 3 + u, y, 5, C_ACC); g_text(px + 4 + u, y, "BOT", 3, C_WHITE, AT_B); }
    y++;
    char t[200];
    if (have && pf->username && *pf->username) {
        snprintf(t, sizeof t, "%s%s%s", pf->username, pf->pronouns && *pf->pronouns ? "  " : "", pf->pronouns ? pf->pronouns : "");
        g_texte(px + 2, y++, t, bw - 4, C_TEXT, 0);
    }
    const Relation *r = rel_find(uid);
    const char *rel = uid == S.me_id ? TR("это вы") : !r ? "" : r->type == REL_FRIEND ? TR("друг") : r->type == REL_IN ? TR("входящая заявка в друзья") :
                      r->type == REL_OUT ? TR("исходящая заявка в друзья") : TR("заблокирован");
    snprintf(t, sizeof t, "%s%s%s", rel, *rel && r && r->type == REL_FRIEND ? " - " : "", r && r->type == REL_FRIEND ? status_text(r) : "");
    if (r && r->type == REL_FRIEND) g_ch(px + 2, y, 'o', status_col(r->status), AT_B);
    g_texte(px + (r && r->type == REL_FRIEND ? 4 : 2), y++, t, bw - 6, C_MUTED, 0);
    for (int i = px + 2; i < px + bw - 2; i++) g_ch(i, y, '-', 0x3f4147u, 0);
    y++;
    if (pf && pf->id == uid && pf->busy) g_text(px + 2, y++, TR("загрузка профиля..."), bw - 4, C_MUTED, 0);
    else if (pf && pf->id == uid && pf->error[0]) { char *e = san(pf->error, false); g_texte(px + 2, y++, e, bw - 4, C_RED, 0); free(e); }
    if (nbio) {
        g_text(px + 2, y++, TR("О СЕБЕ"), bw - 4, C_WHITE, AT_B);
        for (int i = 0; i < nbio; i++) { g_text(px + 2, y++, bio[i], bw - 4, C_TEXT, 0); free(bio[i]); }
        y++;
    }
    free(bio);
    {
        /* создание аккаунта видно по id и без запроса */
        char d1[40], d2[40] = "";
        date_short((i64)((uid >> 22) + (u64)DISCORD_EPOCH_MS) / 1000, d1, sizeof d1);
        if (have && pf->member_since_unix > 0) date_short(pf->member_since_unix, d2, sizeof d2);
        snprintf(t, sizeof t, TR("В DISCORD С  %s"), d1);
        g_texte(px + 2, y++, t, bw - 4, C_MUTED, AT_B);
        if (d2[0]) { snprintf(t, sizeof t, TR("НА СЕРВЕРЕ С  %s"), d2); g_texte(px + 2, y++, t, bw - 4, C_MUTED, AT_B); }
    }
    if (have && pf->nroles) {
        Guild *g = guild_find(pf->guild_id);
        /* имён ролей в store нет (память): число ролей и кружки их цветов, как точки ролей в Discord */
        snprintf(t, sizeof t, TR("РОЛИ: %d"), pf->nroles);
        int x = px + 2 + g_text(px + 2, y, t, bw - 4, C_WHITE, AT_B) + 2;
        for (int i = 0; i < pf->nroles && g; i++) {
            u32 col = 0;
            for (int k = 0; k < g->nroles; k++) if (g->roles[k].id == pf->roles[i]) col = g->roles[k].color;
            if (x + 3 > px + bw - 2) break;
            g_ch(x, y, 'o', col ? col : C_MUTED, AT_B);
            x += 2;
        }
        y++;
    }
    int by = py + bh - 2, x = px + 2;
    if (uid != S.me_id) {                        /* №100: заметка (только мне видна) */
        int ny0 = by - (modrow ? 2 : 0) - note_rows;
        if (!noteon) {
            int w0 = g_text(px + 2, ny0, TR("> Личная заметка"), bw - 4, C_URL, 0);
            hit_add(px + 2, ny0, w0, 1, A_BTN, B_CARD_NOTE, 0, HV_UL);
        } else {
            int u2 = g_text(px + 2, ny0, TR("ЗАМЕТКА (видна только вам) "), bw - 4, C_MUTED, AT_B);
            g_text(px + 2 + u2, ny0, TR("[изменить]"), bw - 4 - u2, C_URL, 0);
            hit_add(px + 2 + u2, ny0, 10, 1, A_BTN, B_NOTE_EDIT, 0, HV_UL);
            if (ns->busy) g_text(px + 2, ny0 + 1, TR("загрузка..."), bw - 4, C_MUTED, 0);
            else if (ns->err[0]) { char *e = san(ns->err, false); g_texte(px + 2, ny0 + 1, e, bw - 4, C_RED, 0); free(e); }
            else if (!nnote) g_text(px + 2, ny0 + 1, TR("пусто"), bw - 4, C_MUTED, 0);
            for (int i = 0; i < nnote; i++) { g_text(px + 2, ny0 + 1 + i, nl[i], bw - 4, C_TEXT, 0); free(nl[i]); }
        }
        free(nl); nl = NULL;
        extra -= note_rows;
    }
    free(nl);
    if (modrow) {                                /* №133/117/147: модерация — каждое действие с подтверждением и причиной */
        int my = by - 2, mx = px + 2;
        static const char *const ml[] = { "Тайм-аут", "Выгнать", "Бан", "Роли", "Перенести", "Заглушить" };
        VoiceStatus v3;
        voice_status(&v3);
        bool invoice = false;
        const VoiceState *vl[32];
        for (int i = 0; i < S.nchans && !invoice; i++) if (S.chans[i].guild_id == mgid && is_voice(&S.chans[i])) {
            int kk = voice_members(S.chans[i].id, vl, 32);
            for (int q = 0; q < kk; q++) if (vl[q]->user_id == uid) invoice = true;
        }
        for (int a2 = 0; a2 < 6; a2++) {
            if (!mod_can(mgid, a2) || (a2 >= MOD_MOVE && !invoice)) continue;
            int bw2 = g_button(mx, my, tr(ml[a2]), px + bw - 2 - mx, a2 == MOD_BAN || a2 == MOD_KICK ? 2 : 0, B_MOD);
            if (!bw2) break;
            U.hit[U.nhit - 1].arg = a2;
            mx += bw2 + 1;
        }
        extra -= 2;
    }
    if (extra) {
        int ey = by - extra - (modrow ? 2 : 0) - note_rows;
        if (!(have && pf->full)) {
            int w = g_text(px + 2, ey, TR("> Полный профиль (общие серверы)"), bw - 4, C_URL, 0);
            hit_add(px + 2, ey, w, 1, A_BTN, B_CARD_FULL, 0, HV_UL);
        } else if (streamer_on()) {
            g_text(px + 2, ey, TR("общие и связи скрыты (режим стримера)"), bw - 4, C_MUTED, 0);
        } else {
            Buf b = {0};
            buf_printf(&b, TR("ОБЩИЕ СЕРВЕРЫ (%d): "), pf->nmutual);
            for (int i = 0; i < pf->nmutual && i < 6; i++) { Guild *gg = guild_find(pf->mutual[i]); buf_printf(&b, "%s%s", i ? ", " : "", gg && gg->name ? gg->name : "?"); }
            char *sb = san(b.p ? b.p : "", false);
            g_texte(px + 2, ey++, sb, bw - 4, C_TEXT, 0);
            free(sb); buf_free(&b);
            buf_printf(&b, TR("ОБЩИЕ ДРУЗЬЯ (%d): "), pf->nfriends);
            for (int i = 0; i < pf->nfriends && i < 6; i++) buf_printf(&b, "%s%s", i ? ", " : "", user_name(pf->friends[i]));
            sb = san(b.p ? b.p : "", false);
            g_texte(px + 2, ey++, sb, bw - 4, C_TEXT, 0);
            free(sb); buf_free(&b);
            if (ncn) {
                g_text(px + 2, ey++, TR("СВЯЗИ"), bw - 4, C_MUTED, AT_B);
                for (int i = 0; i < ncn; i++) { char *c2 = san(pf->conn[i] ? pf->conn[i] : "", false); g_texte(px + 2, ey++, c2, bw - 4, C_TEXT, 0); free(c2); }
            }
        }
    }
    if (uid != S.me_id) {
        x += g_button(x, by, TR("Написать"), bw - 4, 1, B_CARD_MSG) + 1;
        if (!r) x += g_button(x, by, TR("В друзья"), px + bw - 2 - x, 0, B_CARD_ADD) + 1;
        if (!r || r->type != REL_BLOCKED) x += g_button(x, by, TR("Блок"), px + bw - 2 - x, 2, B_CARD_BLOCK) + 1;
        x += g_button(x, by, ignored(uid) ? TR("Не игнор.") : TR("Игнор"), px + bw - 2 - x, 0, B_CARD_IGN) + 1;   /* №108 */
        const Channel *gc = chan_find(S.open_ch);
        if (gc && gc->type == CH_GROUP && gdm_owner(gc->id) == S.me_id) x += g_button(x, by, TR("Исключить"), px + bw - 2 - x, 2, B_CARD_KICK) + 1;
    }
    if (!streamer_on()) g_button(x, by, "ID", px + bw - 2 - x, 0, B_CARD_ID);
}

static void select_guild(int idx);
/* ---- Ctrl+K: быстрый переход (как Quick Switcher Discord) — беседы, каналы, серверы по части имени ----
 * Список собирается в кадре из store (ничего не хранится и не запрашивается). */
typedef struct { u8 kind; int idx; } QsItem;     /* kind 0 — канал/беседа (S.chans), 1 — сервер (S.guilds) */
#define QS_MAX 10
static int qs_collect(QsItem *out) {
    int n = 0;
    const char *q = U.qs_q;
    u64 gid = cur_guild_id();
    char lab[96];
    for (int pass = 0; pass < 4 && n < QS_MAX; pass++) {
        if (pass == 3) {
            for (int i = 0; i < S.nguilds && n < QS_MAX; i++)
                if (*q && name_has(S.guilds[i].name ? S.guilds[i].name : "", q)) out[n++] = (QsItem){ 1, i };
            continue;
        }
        for (int i = 0; i < S.nchans && n < QS_MAX; i++) {
            const Channel *c = &S.chans[i];
            bool dm = !c->guild_id && (c->type == CH_DM || c->type == CH_GROUP);
            if (pass == 0 ? !dm : pass == 1 ? (dm || c->guild_id != gid || !gid) : (dm || c->guild_id == gid)) continue;
            if (!dm && !chan_listed(c)) continue;
            if (!*q && pass == 2) continue;          /* пустой запрос: беседы и каналы текущего сервера */
            chan_label(c, lab, sizeof lab);
            if (name_has(lab, q)) out[n++] = (QsItem){ 0, i };
        }
    }
    return n;
}
static void draw_qs(int W, int H) {
    QsItem it[QS_MAX];
    int n = qs_collect(it);
    int bw = W - 4 < 64 ? W - 4 : 64, rows = n ? n : 1, bh = rows + 7;
    if (bh > H - 2) { rows -= bh - (H - 2); bh = H - 2; }
    if (rows < 1) return;
    int px = (W - bw) / 2, py = H / 6;
    if (py + bh > H - 1) py = H - 1 - bh;
    if (py < 1) py = 1;
    g_dim(0, 0, W, H);
    ovl_cut(0, 0, W, H);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_FEED);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    g_text(px + 3, py + 1, U.fwd_msg ? TR("Переслать в...") : TR("Куда отправимся?"), bw - 6, C_WHITE, AT_B);
    g_pill(px + 2, py + 2, bw - 4, C_RAIL);
    int fw = bw - 8, qw = vw(U.qs_q), skip = qw > fw - 1 ? qw - (fw - 1) : 0, acc = 0, used = 0;
    for (const char *p = U.qs_q; *p; ) {
        u32 c;
        p += utf8_dec(p, &c);
        int cw2 = shown_w(c);
        if (acc >= skip && used + cw2 <= fw - 1) { g_put(px + 4 + used, py + 2, c, cw2, C_WHITE, 0); used += cw2; }
        acc += cw2;
    }
    g_ch(px + 4 + used, py + 2, ' ', C_TEXT, AT_I);
    if (!U.qs_q[0]) g_texte_w(px + 5, py + 2, TR("имя канала, беседы или сервера"), fw - 1, C_MUTED, 0, false);
    if (U.qs_sel >= n) U.qs_sel = n - 1;
    if (U.qs_sel < 0) U.qs_sel = 0;
    if (!n) g_ctext(px, py + 4, bw, TR("Ничего не нашлось"), C_MUTED, 0);
    for (int k = 0; k < rows && k < n; k++) {
        int y = py + 4 + k;
        bool s = k == U.qs_sel;
        if (s) g_pill(px + 2, y, bw - 4, C_SEL);
        char lab[96], t[160];
        const char *right = "";
        if (it[k].kind == 1) {
            const Guild *g = &S.guilds[it[k].idx];
            abbr(g->name, lab, sizeof lab);
            snprintf(t, sizeof t, "[%s] %s", lab, g->name ? g->name : "?");
            right = TR("сервер");
        } else {
            const Channel *c = &S.chans[it[k].idx];
            chan_label(c, lab, sizeof lab);
            snprintf(t, sizeof t, "%s %s", chan_mark(c), lab);
            const Guild *g = c->guild_id ? guild_find(c->guild_id) : NULL;
            right = g ? g->name : TR("личные сообщения");
            if (chan_unread(c) || c->mentions) g_ch(px + 3, y, '*', C_WHITE, AT_B);
        }
        int rw = vw(right) < (bw - 10) / 2 ? vw(right) : (bw - 10) / 2;
        g_texte(px + 5, y, t, bw - 10 - rw - 1, s ? C_WHITE : C_TEXT, AT_B);
        g_rtext(px + bw - 4, y, right, rw, C_MUTED, 0);
        hit_add(px + 2, y, bw - 4, 1, A_QS, 0, k, HV_PILL);
    }
    g_text(px + 3, py + bh - 2, U.fwd_msg ? TR("Up/Down выбор  Enter переслать  Esc отмена") : TR("Up/Down выбор  Enter открыть  Esc закрыть"), bw - 6, C_MUTED, 0);
}
static void forward_to(u64 target);
static void qs_go(int k) {
    QsItem it[QS_MAX];
    int n = qs_collect(it);
    if (k < 0 || k >= n) return;
    U.qs = false;
    if (U.fwd_msg) {                             /* «Переслать»: выбран канал или беседа */
        if (it[k].kind == 1) { ui_notice(TR("выберите канал или беседу, а не сервер")); U.qs = true; return; }
        forward_to(S.chans[it[k].idx].id);
        return;
    }
    if (it[k].kind == 1) { select_guild(it[k].idx + 1); U.focus = F_CHANNELS; }
    else ui_open_channel(S.chans[it[k].idx].id);
}

/* ---- пикер эмодзи: константная таблица (в образе программы, частной памяти не занимает), поиск по имени ----
 * Один пикер для ввода (вставить в поле) и для реакции (U.ep_msg). */
static const struct { const char *e, *n; } EMO[] = {
    {"\xF0\x9F\x98\x80", "grinning улыбка"}, {"\xF0\x9F\x98\x83", "smiley улыбка"}, {"\xF0\x9F\x98\x84", "smile улыбка смех"},
    {"\xF0\x9F\x98\x81", "grin ухмылка"}, {"\xF0\x9F\x98\x86", "laughing смех"}, {"\xF0\x9F\x98\x85", "sweat_smile пот"},
    {"\xF0\x9F\xA4\xA3", "rofl ржу"}, {"\xF0\x9F\x98\x82", "joy слёзы смех"}, {"\xF0\x9F\x99\x82", "slight_smile"},
    {"\xF0\x9F\x99\x83", "upside_down перевёрнутый"}, {"\xF0\x9F\x98\x89", "wink подмигнуть"}, {"\xF0\x9F\x98\x8A", "blush румянец"},
    {"\xF0\x9F\x98\x87", "innocent ангел"}, {"\xF0\x9F\xA5\xB0", "smiling_hearts влюблён"}, {"\xF0\x9F\x98\x8D", "heart_eyes влюблён"},
    {"\xF0\x9F\xA4\xA9", "star_struck звёзды"}, {"\xF0\x9F\x98\x98", "kissing_heart поцелуй"}, {"\xF0\x9F\x98\x8B", "yum вкусно"},
    {"\xF0\x9F\x98\x9B", "tongue язык"}, {"\xF0\x9F\x98\x9C", "wink_tongue"}, {"\xF0\x9F\xA4\xAA", "zany псих"},
    {"\xF0\x9F\xA4\x94", "thinking думаю"}, {"\xF0\x9F\xA4\xA8", "raised_eyebrow бровь"}, {"\xF0\x9F\x98\x90", "neutral нейтрально"},
    {"\xF0\x9F\x98\x91", "expressionless"}, {"\xF0\x9F\x98\xB6", "no_mouth молчу"}, {"\xF0\x9F\x99\x84", "rolling_eyes закатить"},
    {"\xF0\x9F\x98\x8F", "smirk ухмылка"}, {"\xF0\x9F\x98\xAC", "grimacing"}, {"\xF0\x9F\x98\x8C", "relieved облегчение"},
    {"\xF0\x9F\x98\x94", "pensive грусть"}, {"\xF0\x9F\x98\xB4", "sleeping сон"}, {"\xF0\x9F\x98\xB7", "mask маска"},
    {"\xF0\x9F\xA4\x92", "thermometer болею"}, {"\xF0\x9F\xA4\xAF", "exploding_head взрыв мозга"}, {"\xF0\x9F\xA5\xB3", "partying праздник"},
    {"\xF0\x9F\x98\x8E", "sunglasses очки круто"}, {"\xF0\x9F\xA4\x93", "nerd ботан"}, {"\xF0\x9F\x98\x95", "confused"},
    {"\xF0\x9F\x98\x9F", "worried беспокоюсь"}, {"\xF0\x9F\x98\xAE", "open_mouth удивлён"}, {"\xF0\x9F\x98\xB2", "astonished"},
    {"\xF0\x9F\x98\xB3", "flushed смущён"}, {"\xF0\x9F\xA5\xBA", "pleading умоляю"}, {"\xF0\x9F\x98\xA2", "cry плачу"},
    {"\xF0\x9F\x98\xAD", "sob рыдаю"}, {"\xF0\x9F\x98\xB1", "scream крик"}, {"\xF0\x9F\x98\x96", "confounded"},
    {"\xF0\x9F\x98\xA9", "weary устал"}, {"\xF0\x9F\x98\xA4", "triumph пар"}, {"\xF0\x9F\x98\xA1", "rage злой"},
    {"\xF0\x9F\x98\xA0", "angry сердит"}, {"\xF0\x9F\xA4\xAC", "cursing мат"}, {"\xF0\x9F\x98\x88", "smiling_imp чертёнок"},
    {"\xF0\x9F\x92\x80", "skull череп"}, {"\xF0\x9F\x92\xA9", "poop какашка"}, {"\xF0\x9F\xA4\xA1", "clown клоун"},
    {"\xF0\x9F\x91\xBB", "ghost призрак"}, {"\xF0\x9F\x91\xBD", "alien пришелец"}, {"\xF0\x9F\xA4\x96", "robot робот"},
    {"\xF0\x9F\x98\xBA", "smiley_cat кот"}, {"\xF0\x9F\x98\xB9", "joy_cat кот"}, {"\xF0\x9F\x99\x88", "see_no_evil обезьяна"},
    {"\xF0\x9F\x91\x8B", "wave привет"}, {"\xF0\x9F\x91\x8C", "ok_hand ок"}, {"\xE2\x9C\x8C", "v победа"},
    {"\xF0\x9F\xA4\x9E", "crossed_fingers удачи"}, {"\xF0\x9F\x91\x8D", "thumbsup +1 лайк да"}, {"\xF0\x9F\x91\x8E", "thumbsdown -1 нет"},
    {"\xF0\x9F\x91\x8A", "punch кулак"}, {"\xF0\x9F\x91\x8F", "clap хлопать"}, {"\xF0\x9F\x99\x8C", "raised_hands ура"},
    {"\xF0\x9F\x99\x8F", "pray пожалуйста спасибо"}, {"\xF0\x9F\x92\xAA", "muscle сила"}, {"\xF0\x9F\x91\x80", "eyes глаза"},
    {"\xF0\x9F\xA7\xA0", "brain мозг"}, {"\xF0\x9F\x91\x91", "crown корона"}, {"\xE2\x9D\xA4", "heart сердце"},
    {"\xF0\x9F\xA7\xA1", "orange_heart"}, {"\xF0\x9F\x92\x9B", "yellow_heart"}, {"\xF0\x9F\x92\x9A", "green_heart"},
    {"\xF0\x9F\x92\x99", "blue_heart"}, {"\xF0\x9F\x92\x9C", "purple_heart"}, {"\xF0\x9F\x96\xA4", "black_heart"},
    {"\xF0\x9F\x92\x94", "broken_heart разбитое"}, {"\xF0\x9F\x92\xAF", "100 сто"}, {"\xF0\x9F\x94\xA5", "fire огонь"},
    {"\xE2\x9C\xA8", "sparkles блеск"}, {"\xE2\xAD\x90", "star звезда"}, {"\xF0\x9F\x8C\x9F", "star2"},
    {"\xF0\x9F\x92\xA5", "boom бум"}, {"\xF0\x9F\x92\xA4", "zzz сплю"}, {"\xF0\x9F\x92\xAC", "speech речь"},
    {"\xF0\x9F\x8E\x89", "tada ура праздник"}, {"\xF0\x9F\x8E\x81", "gift подарок"}, {"\xF0\x9F\x8E\xAE", "video_game игра"},
    {"\xF0\x9F\x8E\xB5", "music музыка"}, {"\xF0\x9F\x8D\xBA", "beer пиво"}, {"\xE2\x98\x95", "coffee кофе чай"},
    {"\xF0\x9F\x8D\x95", "pizza пицца"}, {"\xF0\x9F\x8D\xBF", "popcorn попкорн"}, {"\xF0\x9F\x90\xB1", "cat кот"},
    {"\xF0\x9F\x90\xB6", "dog собака"}, {"\xF0\x9F\xA6\x8A", "fox лиса"}, {"\xF0\x9F\x90\xB8", "frog лягушка"},
    {"\xF0\x9F\x9A\x80", "rocket ракета"}, {"\xE2\x9A\xA1", "zap молния"}, {"\xF0\x9F\x8C\x88", "rainbow радуга"},
    {"\xE2\x9C\x85", "white_check_mark готово да"}, {"\xE2\x9D\x8C", "x нет крест"}, {"\xE2\x9D\x93", "question вопрос"},
    {"\xE2\x9D\x97", "exclamation восклицание"}, {"\xE2\x9A\xA0", "warning внимание"}, {"\xF0\x9F\x86\x97", "ok ок"},
};
enum { NEMO = (int)(sizeof EMO / sizeof *EMO), EP_COLS = 10, EP_GMAX = 256 };
/* №32: сначала эмодзи сервера (код -(j+1) — индекс в emoji_state()->e; загружаются при открытии пикера,
 * emoji_close — при закрытии), затем стандартные (код — индекс в EMO). out — на NEMO + EP_GMAX. */
static int ep_collect(int *out) {
    int n = 0;
    const EmojiState *es = emoji_state();
    if (es && es->e) for (int j = 0; j < es->n && n < EP_GMAX; j++) if (name_has(es->e[j].name, U.ep_q)) out[n++] = -(j + 1);
    for (int i = 0; i < NEMO; i++) if (name_has(EMO[i].n, U.ep_q)) out[n++] = i;
    return n;
}
static const GuildEmoji *ep_guild(int code) {
    const EmojiState *es = emoji_state();
    int j = -code - 1;
    return code < 0 && es && es->e && j < es->n ? &es->e[j] : NULL;
}
static void draw_ep(int W, int H) {
    int ix[NEMO + EP_GMAX];
    int n = ep_collect(ix);
    int bw = EP_COLS * 4 + 4, gr = 6, bh = gr + 7;
    if (bw > W - 2) bw = W - 2;
    if (bh > H - 2) { gr -= bh - (H - 2); bh = H - 2; }
    if (gr < 1) return;
    int cols = (bw - 4) / 4;
    int px = W - bw - 2, py = H - 4 - bh;
    if (px < 0) px = 0;
    if (py < 1) py = 1;
    ovl_cut(px, py, bw, bh);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_MENU);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    hit_add(px, py, bw, bh, A_BLOCK, 0, 0, 0);
    g_text(px + 2, py + 1, U.ep_msg ? TR("Реакция") : TR("Эмодзи"), bw - 8, C_WHITE, AT_B);
    g_text(px + bw - 3, py + 1, "x", 1, C_TEXT, AT_B);
    hit_add(px + bw - 4, py + 1, 3, 1, A_BTN, B_EP_X, 0, HV_RED);
    g_pill(px + 1, py + 2, bw - 2, C_RAIL);
    int used = g_text(px + 3, py + 2, U.ep_q, bw - 7, C_WHITE, 0);
    g_ch(px + 3 + used, py + 2, ' ', C_TEXT, AT_I);
    if (!U.ep_q[0]) g_text(px + 4, py + 2, TR("найти: smile, сердце, огонь..."), bw - 8, C_MUTED, 0);
    if (U.ep_sel >= n) U.ep_sel = n - 1;
    if (U.ep_sel < 0) U.ep_sel = 0;
    int first = U.ep_sel / cols >= gr ? (U.ep_sel / cols - gr + 1) * cols : 0;
    if (!n) g_ctext(px, py + 4, bw, TR("Не найдено"), C_MUTED, 0);
    for (int k = first; k < n && (k - first) / cols < gr; k++) {
        int r = (k - first) / cols, c = (k - first) % cols;
        int x = px + 2 + c * 4, y = py + 4 + r;
        if (k == U.ep_sel) g_rect(x, y, 4, 1, C_SEL);
        const GuildEmoji *ge = ep_guild(ix[k]);
        if (ge) {                                /* своя эмодзи сервера: картинку клетка не покажет — начало имени */
            char t[8];
            u32 c0;
            int b0 = utf8_dec(ge->name, &c0);
            u32 c1 = 0;
            if (ge->name[b0]) utf8_dec(ge->name + b0, &c1);
            bool off = !ge->available || emoji_need_nitro(ge, emoji_state()->gid, S.open_ch);
            g_ch(x, y, '{', off ? 0x5c6066u : C_GOLD, AT_B);
            if (c0 && shown_w(c0) == 1) g_put(x + 1, y, c0, 1, off ? 0x5c6066u : C_WHITE, 0);
            if (c1 && shown_w(c1) == 1) g_put(x + 2, y, c1, 1, off ? 0x5c6066u : C_WHITE, 0);
            g_ch(x + 3, y, '}', off ? 0x5c6066u : C_GOLD, AT_B);
            (void)t;
        } else {
            u32 cp;
            utf8_dec(EMO[ix[k]].e, &cp);
            int w2 = shown_w(cp);
            if (w2 > 0) g_put(x + 1, y, cp, w2, C_WHITE, 0);
        }
        hit_add(x, y, 4, 1, A_EMOJI, 0, ix[k], HV_BG);
    }
    const EmojiState *es = emoji_state();
    if (n) {
        char t[120];
        const GuildEmoji *ge = ep_guild(ix[U.ep_sel]);
        if (ge) {
            const char *why = !ge->available ? TR(" - недоступна") : emoji_need_nitro(ge, es->gid, S.open_ch) ? TR(" - нужен Nitro") : TR(" - сервер");
            snprintf(t, sizeof t, ":%s:%s", ge->name, why);
        } else {
            const char *nm = EMO[ix[U.ep_sel]].n;
            int l = 0;
            while (nm[l] && nm[l] != ' ') l++;
            snprintf(t, sizeof t, ":%.*s:", l, nm);
        }
        g_texte(px + 2, py + bh - 2, t, bw - 4, C_MUTED, AT_B);
    }
    if (es && es->busy) g_rtext(px + bw - 2, py + bh - 2, TR("эмодзи сервера..."), 18, C_MUTED, 0);
}
static void ep_apply(int ei) {
    const GuildEmoji *ge = ep_guild(ei);
    if (ge) {
        const EmojiState *es = emoji_state();
        if (!ge->available) { ui_notice(TR("эта эмодзи сейчас недоступна")); return; }
        if (emoji_need_nitro(ge, es->gid, S.open_ch)) { ui_notice(TR("эмодзи другого сервера или анимированная - нужен Nitro")); return; }
        u64 mid = U.ep_msg;
        U.ep = false;
        U.ep_msg = 0;
        char code[96];
        if (mid) { snprintf(code, sizeof code, ":%s:%llu", ge->name, (unsigned long long)ge->id); react(mid, code); return; }
        emoji_code(ge, code, sizeof code);
        for (const char *p = code; *p; ) { u32 c; p += utf8_dec(p, &c); in_insert(c); }
        U.focus = F_INPUT;
        return;
    }
    if (ei < 0 || ei >= NEMO) return;
    u64 mid = U.ep_msg;
    U.ep = false;
    U.ep_msg = 0;
    if (mid) { react(mid, EMO[ei].e); return; }
    for (const char *p = EMO[ei].e; *p; ) { u32 c; p += utf8_dec(p, &c); in_insert(c); }
    U.focus = F_INPUT;
}
static void ep_open(u64 msg) {
    const Channel *oc = chan_find(S.open_ch);
    if (oc && oc->guild_id) emoji_open(oc->guild_id);   /* №32: список сервера — только пока открыт пикер */
    U.ep = true;
    U.ep_msg = msg;
    U.ep_q[0] = 0;
    U.ep_sel = 0;
    U.menu = U.pick = U.qs = false;
}
/* Строка фильтра окна (Ctrl+K, эмодзи): добавить символ / стереть последний. */
static void q_key(char *q, size_t cap, const Key *k) {
    size_t n = strlen(q);
    if (k->code == K_BS) { if (n) q[prev_cp(q, n)] = 0; return; }
    if (k->code != K_CHAR || k->ctrl || k->alt) return;
    u32 c = k->ch;
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return;
    char e[4];
    int m = utf8_enc(c, e);
    if (n + (size_t)m + 1 > cap) return;
    memcpy(q + n, e, (size_t)m);
    q[n + (size_t)m] = 0;
}


/* ---- автодополнение над полем ввода (как в Discord): @ник, #канал, :эмодзи (от 2 букв) ----
 * Слово перед курсором; список до 8 совпадений из store / EMO; Up/Down — выбор, Tab или Enter — подставить
 * (@Имя и #канал при отправке станут <@id> и <#id> — wire_text), Esc — скрыть до следующей буквы. */
#define AC_MAX 8
typedef struct { char kind; size_t start; char q[64]; int n; int idx[AC_MAX]; } AcState;
static bool ac_token(AcState *a) {
    a->n = 0;
    if (U.focus != F_INPUT || U.ac_hide || !(U.mode == M_NORMAL || U.mode == M_REPLY || U.mode == M_EDIT) || !U.in.len) return false;
    size_t s = U.cur;
    while (s > 0 && U.in.p[s - 1] != ' ' && U.in.p[s - 1] != '\n') s--;
    if (s >= U.cur) return false;
    char k = U.in.p[s];
    if (k != '@' && k != '#' && k != ':') return false;
    size_t ql = U.cur - s - 1;
    if (ql >= sizeof a->q || (k == ':' && ql < 2) || memchr(U.in.p + s + 1, k, ql)) return false;
    memcpy(a->q, U.in.p + s + 1, ql);
    a->q[ql] = 0;
    a->kind = k;
    a->start = s;
    if (k == '@') {
        Channel *oc = chan_find(S.open_ch);
        if (oc && oc->guild_id) {                     /* как в Discord: @everyone и @here — в начале списка */
            if (name_has("everyone", a->q)) a->idx[a->n++] = -1;
            if (name_has("here", a->q)) a->idx[a->n++] = -2;
        }
        for (int pass = 0; pass < 2 && a->n < AC_MAX; pass++)     /* сначала авторы открытой ленты, затем все */
            for (int i = 0; i < S.nusers && a->n < AC_MAX; i++) {
                const User *u = &S.users[i];
                if (!u->name || !*u->name || !name_has(u->name, a->q)) continue;
                const Relation *rr = rel_find(u->id);          /* заблокированных и людей из заявок не предлагаем */
                if (rr && rr->type != REL_FRIEND) continue;
                bool here = false;
                for (int m = 0; m < S.nmsgs && !here; m++) if (S.msgs[m].author_id == u->id) here = true;
                if (oc && !oc->guild_id) for (int r = 0; r < oc->nrecip && !here; r++) if (oc->recipients[r] == u->id) here = true;
                if (here != (pass == 0)) continue;
                if (avatar_hold(u->id)) continue;                 /* собеседник из запроса/спама — не источник */
                if (pass == 1 && (!oc || !oc->guild_id || u->color_gid != oc->guild_id)) continue;   /* только участники сервера */
                a->idx[a->n++] = i;
            }
    } else if (k == '#') {
        u64 gid = cur_guild_id();
        for (int i = 0; i < S.nchans && a->n < AC_MAX; i++) {
            const Channel *c = &S.chans[i];
            if (!gid || c->guild_id != gid || !chan_listed(c) || !c->name || !name_has(c->name, a->q)) continue;
            a->idx[a->n++] = i;
        }
    } else {
        for (int i = 0; i < NEMO && a->n < AC_MAX; i++) if (name_has(EMO[i].n, a->q)) a->idx[a->n++] = i;
    }
    return a->n > 0;
}
static void ac_accept(int k) {
    AcState a;
    if (!ac_token(&a) || k < 0 || k >= a.n) return;
    char rep[160];
    if (a.kind == '@' && a.idx[k] < 0) snprintf(rep, sizeof rep, "@%s ", a.idx[k] == -1 ? "everyone" : "here");
    else if (a.kind == '@') snprintf(rep, sizeof rep, "@%s ", S.users[a.idx[k]].name);
    else if (a.kind == '#') snprintf(rep, sizeof rep, "#%s ", S.chans[a.idx[k]].name);
    else snprintf(rep, sizeof rep, "%s ", EMO[a.idx[k]].e);
    in_delete_range(a.start, U.cur);
    for (const char *p = rep; *p; ) { u32 c; p += utf8_dec(p, &c); in_insert(c); }
    U.ac_sel = 0;
}
/* ---- №57 слеш-команды: «/» в начале поля — список команд (индекс по требованию: cmds_open, при уходе —
 * cmds_close); выбор — Tab/Enter/щелчок; затем опции по одной в том же поле (имя, тип, обязательность в рамке;
 * варианты — списком); последняя — cmd_run. Esc — отмена. Неподдерживаемые — серым. ---- */
static void sl_reset(void) {
    for (int i = 0; i < 25; i++) { free(SL.val[i]); SL.val[i] = NULL; }
    SL.cmd = -1; SL.opt = 0; SL.sel = 0;
}
static bool sl_typing(void) {                    /* в поле «/кмд» и команда ещё не выбрана */
    return SL.cmd < 0 && U.focus == F_INPUT && U.mode == M_NORMAL && U.in.len && U.in.p[0] == '/' && !memchr(U.in.p, ' ', U.in.len);
}
static int sl_list(int *out, int max) {
    char q[64];
    size_t n = U.in.len - 1 < sizeof q - 1 ? U.in.len - 1 : sizeof q - 1;
    memcpy(q, U.in.p + 1, n);
    q[n] = 0;
    return cmds_match(q, out, max);
}
static void sl_run(void) {
    const SlashIndex *si = cmds_state();
    if (!si || SL.cmd < 0 || SL.cmd >= si->n) { sl_reset(); return; }
    const char *v[25];
    int n = si->c[SL.cmd].nopt;
    for (int i = 0; i < n && i < 25; i++) v[i] = SL.val[i] ? SL.val[i] : "";
    char err[200] = "";
    if (!cmd_run(SL.cmd, v, n, err, sizeof err) && err[0]) { char *e = san(err, false); ui_notice("%s", e); free(e); }
    sl_reset();
}
static void sl_pick(int i) {                     /* команда выбрана: дальше опции */
    const SlashIndex *si = cmds_state();
    if (!si || i < 0 || i >= si->n) return;
    if (!si->c[i].usable) { ui_notice(TR("эту команду Astra DS выполнить не может (нужна роль или вложение)")); return; }
    sl_reset();
    SL.cmd = i;
    in_clear();
    if (!si->c[i].nopt) sl_run();
}
static void sl_next(const char *v) {             /* значение текущей опции принято */
    const SlashOpt *o = sl_opt();
    if (!o) { sl_run(); return; }
    if (o->required && (!v || !*v)) { ui_notice(TR("опция %s обязательна"), cs_show(o->name_show)); return; }
    SL.val[SL.opt] = xstrdup(v ? v : "");
    SL.opt++;
    SL.sel = 0;
    in_clear();
    if (!sl_opt()) sl_run();
}
static bool sl_key(const Key *k) {
    if (U.focus != F_INPUT || U.mode != M_NORMAL) return false;
    if (SL.cmd < 0) {
        if (!sl_typing()) return false;
        int ix[12], n = sl_list(ix, 12);
        if (!n) return false;
        if (k->code == K_UP) { if (SL.sel > 0) SL.sel--; return true; }
        if (k->code == K_DOWN) { if (SL.sel < n - 1) SL.sel++; return true; }
        if (k->code == K_TAB || k->code == K_ENTER) { sl_pick(ix[SL.sel < n ? SL.sel : 0]); return true; }
        return false;
    }
    const SlashOpt *o = sl_opt();
    if (k->code == K_ESC) { sl_reset(); in_clear(); ui_notice(TR("команда отменена")); return true; }
    if (o && o->nchoice) {
        if (k->code == K_UP) { if (SL.sel > 0) SL.sel--; return true; }
        if (k->code == K_DOWN) { if (SL.sel < o->nchoice - 1) SL.sel++; return true; }
        if (k->code == K_ENTER || k->code == K_TAB) {
            const char *nm, *val;
            if (cmd_choice(SL.cmd >= 0 ? cmds_state()->c[SL.cmd].opt0 + SL.opt : 0, SL.sel, &nm, &val)) sl_next(val);
            return true;
        }
        return true;                             /* варианты — только из списка */
    }
    if (k->code == K_ENTER) {
        char *v = xmalloc(U.in.len + 1);
        memcpy(v, U.in.p ? U.in.p : "", U.in.len);
        v[U.in.len] = 0;
        sl_next(v);
        free(v);
        return true;
    }
    return false;
}
/* Окно над полем: список команд или вариантов опции; подпись опции — в рамке поля (draw_chat). */
static void draw_sl(int x, int w, int ybot) {
    const SlashIndex *si = cmds_state();
    int bw = w < 70 ? w : 70;
    char t[200];
    if (SL.cmd < 0) {
        if (!sl_typing()) return;
        if (!si || si->cid != S.open_ch) return;
        int ix[12], n = sl_list(ix, 12);
        int bh = (si->busy || si->err[0] || !n ? 1 : n) + 2, py = ybot - bh;
        if (py < 1) return;
        if (SL.sel >= n) SL.sel = n > 0 ? n - 1 : 0;
        ovl_cut(x, py, bw, bh);
        g_rect(x, py, bw, bh, C_MENU);
        g_round_corners(x, py, bw, bh, 0, bg_at(x, py - 1));
        hit_add(x, py, bw, bh, A_BLOCK, 0, 0, 0);
        g_text(x + 2, py, TR("КОМАНДЫ ПРИЛОЖЕНИЙ"), bw - 4, C_MUTED, AT_B);
        if (si->busy) { g_text(x + 2, py + 1, TR("загрузка команд..."), bw - 4, C_MUTED, 0); return; }
        if (si->err[0]) { char *e = san(si->err, false); g_texte(x + 2, py + 1, e, bw - 4, C_RED, 0); free(e); return; }
        if (!n) { g_text(x + 2, py + 1, TR("нет подходящих команд"), bw - 4, C_MUTED, 0); return; }
        for (int k = 0; k < n; k++) {
            const SlashCmd *c = &si->c[ix[k]];
            int y = py + 1 + k;
            if (k == SL.sel) g_pill(x + 1, y, bw - 2, C_SEL);
            snprintf(t, sizeof t, "/%s", cs_show(c->name));
            int u = g_texte(x + 2, y, t, bw / 2, c->usable ? C_WHITE : 0x5c6066u, AT_B);
            const char *d = c->usable ? cs_show(c->desc) : TR("не поддерживается");
            snprintf(t, sizeof t, "%s - %s", d, cs_show(c->app));
            g_texte(x + 4 + u, y, t, bw - 6 - u, C_MUTED, 0);
            hit_add(x + 1, y, bw - 2, 1, A_SLASH, 0, ix[k], HV_PILL);
        }
        return;
    }
    const SlashOpt *o = sl_opt();
    if (!o || !o->nchoice) return;
    int n = o->nchoice > 12 ? 12 : o->nchoice, bh = n + 1, py = ybot - bh;
    if (py < 1) return;
    ovl_cut(x, py, bw, bh);
    g_rect(x, py, bw, bh, C_MENU);
    hit_add(x, py, bw, bh, A_BLOCK, 0, 0, 0);
    g_text(x + 2, py, TR("ВАРИАНТЫ"), bw - 4, C_MUTED, AT_B);
    int opt = cmds_state()->c[SL.cmd].opt0 + SL.opt;
    for (int k = 0; k < n; k++) {
        const char *nm, *val;
        if (!cmd_choice(opt, k, &nm, &val)) continue;
        int y = py + 1 + k;
        if (k == SL.sel) g_pill(x + 1, y, bw - 2, C_SEL);
        char *sn = san(nm, false);
        g_texte(x + 3, y, sn, bw - 6, C_TEXT, k == SL.sel ? AT_B : 0);
        free(sn);
        hit_add(x + 1, y, bw - 2, 1, A_SLASH, 1, k, HV_PILL);
    }
}
/* Окно над рамкой поля: x, w — поле; ybot — строка верхней линии рамки. */
static void draw_ac(int x, int w, int ybot) {
    AcState a;
    if (!ac_token(&a)) return;
    if (U.ac_sel >= a.n) U.ac_sel = a.n - 1;
    if (U.ac_sel < 0) U.ac_sel = 0;
    int bw = w < 52 ? w : 52, bh = a.n + 2, py = ybot - bh;
    if (py < 1) return;
    ovl_cut(x, py, bw, bh);
    u32 ut = bg_at(x, py);
    g_rect(x, py, bw, bh, C_MENU);
    g_round_corners(x, py, bw, bh, 0, ut);
    hit_add(x, py, bw, bh, A_BLOCK, 0, 0, 0);
    char t[160];
    snprintf(t, sizeof t, TR("%s, СОВПАДАЮЩИЕ С %c%s"), a.kind == '@' ? TR("УЧАСТНИКИ") : a.kind == '#' ? TR("КАНАЛЫ") : TR("ЭМОДЗИ"), a.kind, a.q);
    g_texte(x + 2, py, t, bw - 4, C_MUTED, AT_B);
    for (int k = 0; k < a.n; k++) {
        int y = py + 1 + k;
        bool s = k == U.ac_sel;
        if (s) g_pill(x + 1, y, bw - 2, C_SEL);
        if (a.kind == '@' && a.idx[k] < 0) {
            g_text(x + 3, y, "@", 1, C_MENTFG, AT_B);
            g_text(x + 5, y, a.idx[k] == -1 ? "everyone" : "here", 10, s ? C_WHITE : C_TEXT, AT_B);
            g_texte(x + 17, y, a.idx[k] == -1 ? TR("уведомить всех на канале") : TR("уведомить всех в сети"), bw - 19, C_MUTED, 0);
        } else if (a.kind == '@') {
            const User *u = &S.users[a.idx[k]];
            g_listav(x + 2, y, u->id, NULL, G_POP);
            g_texte(x + 7, y, u->name, bw - 9, s ? C_WHITE : C_TEXT, AT_B);
        } else if (a.kind == '#') {
            g_text(x + 3, y, "#", 1, C_MUTED, AT_B);
            g_texte(x + 5, y, S.chans[a.idx[k]].name, bw - 7, s ? C_WHITE : C_TEXT, AT_B);
        } else {
            u32 cp;
            utf8_dec(EMO[a.idx[k]].e, &cp);
            int w2 = shown_w(cp);
            if (w2 > 0) g_put(x + 3, y, cp, w2, C_WHITE, 0);
            const char *nm = EMO[a.idx[k]].n;
            int l = 0;
            while (nm[l] && nm[l] != ' ') l++;
            snprintf(t, sizeof t, ":%.*s:", l, nm);
            g_texte(x + 7, y, t, bw - 9, s ? C_WHITE : C_TEXT, 0);
        }
        hit_add(x + 1, y, bw - 2, 1, A_AC, 0, k, HV_PILL);
    }
}


/* ======================= волна 2: серверы, каналы, приглашения (окна-формы) =======================
 * Одно окно-форма на всё (FM): заголовок, пояснение, поля (текст / выбор < > / флажок [x] / три состояния права),
 * ошибка красным, кнопки справа. Tab/Up/Down — поле или кнопка, Left/Right — выбор, Enter — кнопка в фокусе или
 * главная, Esc — закрыть. Ничего не хранится сверх открытого окна (~3 КБ в U-подобной структуре). */
#ifndef MC_WAVE2
/* ВРЕМЕННО: API волны 2 ещё не объявлен в mc.h (разработчик объявит MC_WAVE2) — заглушки для сборки и демо */
enum { CHK_TEXT = 0, CHK_VOICE = 2, CHK_CATEGORY = 4 };
enum { OW_ROLE = 0, OW_MEMBER = 1 };
typedef struct { const char *name, *topic; int slowmode, nsfw, bitrate, user_limit; } ChanEdit;
typedef struct { bool busy; char error[160]; char code[32]; u64 guild_id; const char *guild_name, *channel_name, *inviter; char icon[35]; int members, online; bool joined; } InviteState;
static InviteState w2_inv;
static bool w2_own(u64 gid) { return O.demo && gid == 2000; }
static bool can_manage_guild(u64 gid) { return w2_own(gid); }
static bool can_manage_channels(u64 gid) { return w2_own(gid); }
static bool can_manage_roles(u64 gid) { return w2_own(gid); }
static bool guild_is_owner(u64 gid) { return w2_own(gid); }
static bool w2_nyi(char *err, size_t n) { snprintf(err, n, TR("ещё не подключено (ждём store.c)")); return false; }
static bool guild_create(const char *name, const char *icon, char *err, size_t n) { (void)name; (void)icon; return w2_nyi(err, n); }
static bool guild_edit(u64 g, const char *a, const char *b, const char *c, const char *d, char *err, size_t n) { (void)g; (void)a; (void)b; (void)c; (void)d; return w2_nyi(err, n); }
static void guild_leave(u64 g) { (void)g; ui_notice(TR("покинуть сервер: ещё не подключено")); }
static bool guild_delete(u64 g, const char *t, char *err, size_t n) { (void)g; (void)t; return w2_nyi(err, n); }
static bool chan_create(u64 g, int k, const char *nm, u64 par, char *err, size_t n) { (void)g; (void)k; (void)nm; (void)par; return w2_nyi(err, n); }
static bool chan_edit(u64 c, const ChanEdit *e, char *err, size_t n) { (void)c; (void)e; return w2_nyi(err, n); }
static void chan_delete(u64 c) { (void)c; ui_notice(TR("удаление канала: ещё не подключено")); }
static void chan_move(u64 c, u64 par, int pos) { (void)c; (void)par; (void)pos; ui_notice(TR("перестановка: ещё не подключено")); }
static void chan_perm_set(u64 c, u64 t, int ty, u64 a, u64 d) { (void)c; (void)t; (void)ty; (void)a; (void)d; ui_notice(TR("права: ещё не подключено")); }
static void chan_perm_clear(u64 c, u64 t) { (void)c; (void)t; }
static bool invite_parse(const char *t, char *code, size_t n) {
    const char *p = strrchr(t, '/');
    snprintf(code, n, "%s", p ? p + 1 : t);
    return code[0] != 0;
}
static void invite_open(const char *code) {
    memset(&w2_inv, 0, sizeof w2_inv);
    snprintf(w2_inv.code, sizeof w2_inv.code, "%s", code);
    if (!strcmp(code, "abc") || !strcmp(code, "Astra DS")) { w2_inv.guild_name = TR("Котики 2"); w2_inv.channel_name = "general"; w2_inv.inviter = TR("Алиса"); w2_inv.members = 1234; w2_inv.online = 321; w2_inv.guild_id = 777; }
    else snprintf(w2_inv.error, sizeof w2_inv.error, "%s", !strcmp(code, "429") ? TR("слишком часто: попробуйте через минуту") : TR("приглашение недействительно или истекло"));
}
static const InviteState *invite_state(void) { return &w2_inv; }
static void invite_join(void) { ui_notice(TR("вступление: ещё не подключено")); }
static void invite_close(void) { memset(&w2_inv, 0, sizeof w2_inv); }
static char w2_link[64];
static void invite_create(u64 c, int age, int uses) { (void)c; (void)age; (void)uses; snprintf(w2_link, sizeof w2_link, "%s", O.demo ? "https://discord.gg/Astra DS" : ""); }
static const char *invite_created(void) { return w2_link[0] ? w2_link : NULL; }
static const char *role_name(u64 g, u64 r) { (void)g; (void)r; return NULL; }
static bool chan_info(u64 c, ChanEdit *o) { Channel *ch = chan_find(c); if (!ch) return false; memset(o, 0, sizeof *o); o->name = ch->name; o->topic = ch->topic; o->bitrate = 64000; return true; }
static const char *guild_description(u64 g) { (void)g; return ""; }
#endif

enum { FF_TEXT, FF_CHOICE, FF_TOGGLE, FF_TRI, FF_INFO };
enum { FM_NONE, FM_ADDSRV, FM_GCREATE, FM_JOIN, FM_JOINCARD, FM_INVITE, FM_GSET, FM_GDELETE, FM_LEAVE, FM_CHCREATE,
       FM_CATCREATE, FM_CHEDIT, FM_CHDELETE, FM_PERMS, FM_ADDFRIEND, FM_THREAD, FM_ALERTS, FM_FILTER, FM_VSTATUS, FM_TRENAME, FM_FPOST, FM_GRENAME, FM_MEPROF, FM_FALT, FM_MOD, FM_STATUS, FM_NOTE, FM_SHARE, FM_CAM };
enum { FB_OK = 1, FB_CANCEL, FB_X1, FB_X2, FB_RETRY };     /* кнопки формы (sub у A_FORMB) */
/* буфер поля — в куче на время окна: лимит поля в символах x 4 байта (тема 1024 символа целиком), иначе 256 */
static void utf8_cpy(char *d, size_t n, const char *s);
typedef struct { u8 kind; const char *label; char *val; size_t cap; int ch, nch; const char *const *chs; u32 oh; int och; int maxc; } FField;
static struct {
    int id, nf, foc;
    bool loading;                                /* значения ещё грузятся: поля не заполнены, «Сохранить» недоступно */
    bool confirm;                                /* «Открыть в браузере» мимо VLESS: ждём второе нажатие */
    u64 o_al, o_de;                              /* «Права»: исходная перезапись цели (меняем только свои биты) */
    char title[96], sub[400], err[200];
    FField f[9];
    struct { const char *t; int id, style; } b[4];
    int nb;
    u64 gid, cid, tgt;
    int tgt_type, ntg;
    u64 tg[24]; u8 tgty[24];
    char tgn[24][64];
    const char *tgp[24];
} FM;
static const char *const AGE_N[] = { "30 минут", "1 час", "6 часов", "12 часов", "1 день", "7 дней", "никогда" };
static const int AGE_S[] = { 1800, 3600, 21600, 43200, 86400, 604800, 0 };
static const char *const USES_N[] = { "без ограничений", "1", "5", "10", "25", "50", "100" };
static const int USES_V[] = { 0, 1, 5, 10, 25, 50, 100 };
static const char *const CHK_N[] = { "Текстовый", "Голосовой" };
static const char *const SLOW_N[] = { "выкл", "5 с", "10 с", "30 с", "1 мин", "5 мин", "15 мин", "1 ч", "6 ч" };
static const int SLOW_S[] = { 0, 5, 10, 30, 60, 300, 900, 3600, 21600 };
static const char *const BIT_N[] = { "8 кбит/с", "32 кбит/с", "64 кбит/с", "96 кбит/с" };
static const int BIT_V[] = { 8000, 32000, 64000, 96000 };
static const char *const LIM_N[] = { "без лимита", "2", "5", "10", "25", "99" };
static const int LIM_V[] = { 0, 2, 5, 10, 25, 99 };
static const char *const TRI_N[] = { "запретить", "как в сервере", "разрешить" };
/* права канала, которые можно менять в окне «Права» (биты Discord) */
static const struct { const char *n; u64 bit; bool voice; } PERM_ROWS[] = {
    { "Просматривать канал", 1ull << 10, false }, { "Отправлять сообщения", 1ull << 11, false },
    { "Прикреплять файлы", 1ull << 15, false }, { "Добавлять реакции", 1ull << 6, false },
    { "Читать историю сообщений", 1ull << 16, false }, { "Подключаться", 1ull << 20, true }, { "Говорить", 1ull << 21, true },
};
enum { NPERM = (int)(sizeof PERM_ROWS / sizeof *PERM_ROWS) };
/* Освободить буферы полей и обнулить окно (вместо memset: буферы — в куче). */
static void fm_reset(void) {
    for (int i = 0; i < 9; i++) if (FM.f[i].val) { wipe(FM.f[i].val, FM.f[i].cap); free(FM.f[i].val); }
    memset(&FM, 0, sizeof FM);
}
static void fm_close(void) {
    if (FM.id == FM_JOINCARD || FM.id == FM_JOIN) invite_close();
#ifdef MC_WAVE2
    if (FM.id) forms_closed();                   /* store освобождает роли/описание/настройки окна */
#endif
    fm_reset();
}
static FField *fm_add(u8 kind, const char *label, const char *val) {
    if (FM.nf >= 9) return &FM.f[8];
    FField *f = &FM.f[FM.nf++];
    memset(f, 0, sizeof *f);
    f->kind = kind; f->label = label;
    f->maxc = strstr(label, "ОПИСАНИЕ") ? 120 : strstr(label, "ТЕМА") ? 1024 : strstr(label, "НАЗВАНИЕ") ? 100 : 0;
    f->cap = f->maxc ? (size_t)f->maxc * 4 + 1 : 256;
    f->val = xcalloc(1, f->cap);
    if (val) utf8_cpy(f->val, f->cap, val);
    return f;
}
static void fm_choice(FField *f, const char *const *chs, int n, int cur) { f->chs = chs; f->nch = n; f->ch = cur; }
/* Строка в буфер не длиннее n-1 байт, без обрыва посреди символа UTF-8. */
static void utf8_cpy(char *d, size_t n, const char *s) {
    size_t l = s ? strlen(s) : 0;
    if (l >= n) { l = n - 1; while (l && ((unsigned char)s[l] & 0xC0) == 0x80) l--; }
    if (l) memcpy(d, s, l);
    d[l] = 0;
}
static void fm_set(FField *f, const char *v) { utf8_cpy(f->val, f->cap, v); f->oh = fnv(f->val, strlen(f->val)); }
static bool fm_changed(const FField *f) { return f->kind == FF_TEXT ? fnv(f->val, strlen(f->val)) != f->oh : f->ch != f->och; }
static void fm_mark(void) { for (int i = 0; i < FM.nf; i++) { FM.f[i].oh = fnv(FM.f[i].val, strlen(FM.f[i].val)); FM.f[i].och = FM.f[i].ch; } }
static void fm_btn(const char *t, int id, int style) { if (FM.nb < 4) { FM.b[FM.nb].t = t; FM.b[FM.nb].id = id; FM.b[FM.nb].style = style; FM.nb++; } }
static int pick_idx(const int *v, int n, int x) { int b = 0; for (int i = 0; i < n; i++) if (v[i] == x) b = i; return b; }
/* значения прав выбранной цели из перезаписей канала */
static int perm_row(const FField *f) { for (int k = 0; k < NPERM; k++) if (!strcmp(PERM_ROWS[k].n, f->label)) return k; return -1; }
static void perms_load(void) {
    Channel *c = chan_find(FM.cid);
    u64 al = 0, de = 0;
    for (int i = 0; c && i < c->now_; i++) if (c->ow[i].id == FM.tgt) { al = c->ow[i].allow; de = c->ow[i].deny; }
    FM.o_al = al; FM.o_de = de;
    for (int i = 1; i < FM.nf; i++) {
        int r = perm_row(&FM.f[i]);
        if (r >= 0) FM.f[i].ch = (al & PERM_ROWS[r].bit) ? 2 : (de & PERM_ROWS[r].bit) ? 0 : 1;
    }
}
static u64 fm_mid;                               /* «Создать ветку»: из какого сообщения */
static u64 fm_note_uid;                          /* №100: чья заметка в форме */
static const char *const ST_CH[] = { "В сети", "Не активен", "Не беспокоить", "Невидимый" };
static const char *const ST_DUR[] = { "бессрочно", "15 минут", "1 час", "8 часов", "24 часа", "3 дня" };
static const int ST_DUR_MIN[] = { 0, 15, 60, 480, 1440, 4320 };
static u64 fm_mod_uid;                           /* №133: кого модерируем и что делаем (MOD_*) */
static int fm_mod_kind;
static const char *const MOD_TO[] = { "60 секунд", "5 минут", "10 минут", "1 час", "1 день", "1 неделя", "снять тайм-аут" };
static const int MOD_TO_MIN[] = { 1, 5, 10, 60, 1440, 10080, 0 };
static const char *const MOD_DEL[] = { "не удалять", "за 1 час", "за 24 часа", "за 7 дней" };
static const int MOD_DEL_H[] = { 0, 1, 24, 168 };
static const char *const MOD_RA[] = { "выдать", "снять" };
static char mod_rn[32][48];                      /* роли ниже моей / голосовые каналы — варианты выбора */
static const char *mod_rp[33];
static u64 mod_rid[33];
static int mod_nr;
static void fm_open(int id, u64 gid, u64 cid) {
    fm_close();
    FM.id = id; FM.gid = gid; FM.cid = cid;
    Channel *c = chan_find(cid);
    Guild *g = guild_find(gid);
    char lab[96] = "";
    if (c) chan_label(c, lab, sizeof lab);
    switch (id) {
    case FM_ADDSRV:
        snprintf(FM.title, sizeof FM.title, TR("Добавить сервер"));
        snprintf(FM.sub, sizeof FM.sub, TR("Создайте свой сервер или присоединитесь по приглашению."));
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Присоединиться"), FB_X1, 0); fm_btn(TR("Создать свой"), FB_OK, 1);
        break;
    case FM_GCREATE:
        snprintf(FM.title, sizeof FM.title, TR("Создайте свой сервер"));
        snprintf(FM.sub, sizeof FM.sub, TR("Значок - путь к файлу PNG/JPG до 256 КБ, можно пусто."));
        { char nm[96]; snprintf(nm, sizeof nm, TR("Сервер %s"), S.me_name ? S.me_name : ""); fm_add(FF_TEXT, "НАЗВАНИЕ СЕРВЕРА", nm); }
        fm_add(FF_TEXT, "ЗНАЧОК (ПУТЬ К ФАЙЛУ)", "");
        fm_btn(TR("Назад"), FB_CANCEL, 0); fm_btn(TR("Создать"), FB_OK, 1);
        break;
    case FM_JOIN:
        snprintf(FM.title, sizeof FM.title, TR("Присоединиться к серверу"));
        snprintf(FM.sub, sizeof FM.sub, TR("Введите приглашение: https://discord.gg/hTKzmak, hTKzmak или ссылку discord.com/invite/..."));
        fm_add(FF_TEXT, "ССЫЛКА-ПРИГЛАШЕНИЕ", "");
        fm_btn(TR("Назад"), FB_CANCEL, 0); fm_btn(TR("Найти сервер"), FB_OK, 1);
        break;
    case FM_JOINCARD:
        snprintf(FM.title, sizeof FM.title, TR("Вас пригласили на сервер"));
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Вступить на сервер"), FB_OK, 1);
        break;
    case FM_INVITE:
        snprintf(FM.title, sizeof FM.title, TR("Пригласить друзей на %s"), g && g->name ? g->name : TR("сервер"));
        snprintf(FM.sub, sizeof FM.sub, TR("Ссылка ведёт в канал %s%s."), c && c->guild_id ? "#" : "", lab);
        fm_choice(fm_add(FF_CHOICE, "ИСТЕКАЕТ ЧЕРЕЗ", NULL), AGE_N, 7, 4);
        fm_choice(fm_add(FF_CHOICE, "МАКС. ЧИСЛО ИСПОЛЬЗОВАНИЙ", NULL), USES_N, 7, 0);
        fm_add(FF_INFO, "ССЫЛКА", TR("нажмите 'Создать ссылку'"));
        fm_btn(TR("Закрыть"), FB_CANCEL, 0); fm_btn(TR("Копировать"), FB_X1, 0); fm_btn(TR("Создать ссылку"), FB_OK, 1);
        break;
    case FM_GSET: {
        snprintf(FM.title, sizeof FM.title, TR("Настройки сервера"));
        snprintf(FM.sub, sizeof FM.sub, TR("Обзор. Пути к файлам - PNG/JPG до 256 КБ; пусто - не менять, '-' - убрать."));
        fm_add(FF_TEXT, "НАЗВАНИЕ СЕРВЕРА", g && g->name ? g->name : "");
        const char *d = guild_description(gid);
        fm_add(FF_TEXT, "ОПИСАНИЕ", d ? d : "");
        FM.loading = !d;                         /* описание ещё грузится — draw_form дозаполнит */
        fm_add(FF_TEXT, "ЗНАЧОК (ПУТЬ К ФАЙЛУ)", "");
        fm_add(FF_TEXT, "БАННЕР (ПУТЬ К ФАЙЛУ)", "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0);
        if (guild_is_owner(gid)) fm_btn(TR("Удалить сервер"), FB_X1, 2);
        fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    }
    case FM_GDELETE:
        snprintf(FM.title, sizeof FM.title, TR("Удалить '%s'"), g && g->name ? g->name : TR("сервер"));
        snprintf(FM.sub, sizeof FM.sub, TR("Вы точно хотите удалить %s? Это действие нельзя отменить. Введите название сервера."), g && g->name ? g->name : TR("сервер"));
        fm_add(FF_TEXT, "НАЗВАНИЕ СЕРВЕРА", "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Удалить сервер"), FB_OK, 2);
        break;
    case FM_LEAVE:
        snprintf(FM.title, sizeof FM.title, TR("Покинуть '%s'"), g && g->name ? g->name : TR("сервер"));
        snprintf(FM.sub, sizeof FM.sub, TR("Вы уверены, что хотите покинуть %s? Вернуться можно только по новому приглашению."), g && g->name ? g->name : TR("сервер"));
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Покинуть сервер"), FB_OK, 2);
        break;
    case FM_CHCREATE:
        snprintf(FM.title, sizeof FM.title, TR("Создать канал"));
        if (c && c->type == CH_CATEGORY) snprintf(FM.sub, sizeof FM.sub, TR("в категории %s"), c->name ? c->name : "");
        fm_choice(fm_add(FF_CHOICE, "ТИП КАНАЛА", NULL), CHK_N, 2, 0);
        fm_add(FF_TEXT, "НАЗВАНИЕ КАНАЛА", TR("новый-канал"));
        fm_add(FF_TOGGLE, "Приватный канал (виден только выбранным ролям)", NULL);
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Создать канал"), FB_OK, 1);
        break;
    case FM_MOD: {
        const char *who = user_name(fm_mod_uid);
        static const char *const tt[] = { "Тайм-аут", "Выгнать с сервера", "Забанить", "Роли", "Перенести в голосовой канал", "Заглушить на сервере" };
        snprintf(FM.title, sizeof FM.title, "%s: %s", tr(tt[fm_mod_kind < 6 ? fm_mod_kind : 0]), who && *who ? who : "?");
        mod_nr = 0;
        if (fm_mod_kind == MOD_TIMEOUT) {
            snprintf(FM.sub, sizeof FM.sub, TR("Не сможет писать, отвечать и говорить в голосовых каналах."));
            fm_choice(fm_add(FF_CHOICE, "НА СКОЛЬКО", NULL), MOD_TO, 7, 1);
        } else if (fm_mod_kind == MOD_KICK) {
            snprintf(FM.sub, sizeof FM.sub, TR("Вернуться сможет по новому приглашению."));
        } else if (fm_mod_kind == MOD_BAN) {
            snprintf(FM.sub, sizeof FM.sub, TR("Не сможет вернуться, пока бан не снимут."));
            fm_choice(fm_add(FF_CHOICE, "УДАЛИТЬ ЕГО СООБЩЕНИЯ", NULL), MOD_DEL, 4, 0);
        } else if (fm_mod_kind == MOD_ROLES) {
            const RoleName *rl = NULL;
            int nrl = guild_roles(gid, &rl), top = 0;
            if (g) for (int i = 0; i < g->nmy; i++) for (int q = 0; q < nrl; q++) if (rl[q].id == g->my_roles[i] && rl[q].pos > top) top = rl[q].pos;
            bool owner = g && g->owner_id == S.me_id;
            for (int q = 0; q < nrl && mod_nr < 32; q++) {
                if (rl[q].id == gid || (!owner && rl[q].pos >= top)) continue;   /* @everyone и не ниже моей — нельзя */
                snprintf(mod_rn[mod_nr], sizeof mod_rn[0], "%s", rl[q].name ? rl[q].name : "?");
                mod_rp[mod_nr] = mod_rn[mod_nr]; mod_rid[mod_nr] = rl[q].id; mod_nr++;
            }
            snprintf(FM.sub, sizeof FM.sub, mod_nr ? TR("Роли ниже вашей.") : TR("Нет ролей ниже вашей."));
            if (mod_nr) { fm_choice(fm_add(FF_CHOICE, "РОЛЬ", NULL), mod_rp, mod_nr, 0); fm_choice(fm_add(FF_CHOICE, "ДЕЙСТВИЕ", NULL), MOD_RA, 2, 0); }
        } else if (fm_mod_kind == MOD_MOVE) {
            snprintf(mod_rn[0], sizeof mod_rn[0], TR("отключить от голоса"));
            mod_rp[0] = mod_rn[0]; mod_rid[0] = 0; mod_nr = 1;
            for (int i = 0; i < S.nchans && mod_nr < 32; i++) if (S.chans[i].guild_id == gid && is_voice(&S.chans[i])) {
                chan_label(&S.chans[i], mod_rn[mod_nr], sizeof mod_rn[0]);
                mod_rp[mod_nr] = mod_rn[mod_nr]; mod_rid[mod_nr] = S.chans[i].id; mod_nr++;
            }
            fm_choice(fm_add(FF_CHOICE, "КУДА", NULL), mod_rp, mod_nr, 0);
        } else {
            fm_add(FF_TOGGLE, "Заглушить микрофон", NULL);
            fm_add(FF_TOGGLE, "Заглушить звук", NULL);
        }
        if (fm_mod_kind <= MOD_BAN) fm_add(FF_TEXT, "ПРИЧИНА (в журнал аудита, можно пусто)", "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0);
        fm_btn(fm_mod_kind == MOD_BAN ? TR("Забанить") : fm_mod_kind == MOD_KICK ? TR("Выгнать") : TR("Применить"), FB_OK, fm_mod_kind == MOD_BAN || fm_mod_kind == MOD_KICK ? 2 : 1);
        break;
    }
    case FM_NOTE: {
        const NoteState *ns = note_state();
        snprintf(FM.title, sizeof FM.title, TR("Заметка: %s"), user_name(fm_note_uid));
        snprintf(FM.sub, sizeof FM.sub, TR("Видна только вам. До 256 символов, пусто - убрать."));
        fm_add(FF_TEXT, "ЗАМЕТКА", ns && ns->uid == fm_note_uid ? ns->text : "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    }
    case FM_STATUS: {                            /* №103/104 */
        static const u8 ps[4] = { PS_ONLINE, PS_IDLE, PS_DND, PS_INVISIBLE };
        int cur = 0;
        for (int i = 0; i < 4; i++) if (presence_me() == ps[i]) cur = i;
        snprintf(FM.title, sizeof FM.title, TR("Статус"));
        snprintf(FM.sub, sizeof FM.sub, TR("По истечении срока - снова 'В сети'; текст - видят все."));
        fm_choice(fm_add(FF_CHOICE, "СТАТУС", NULL), ST_CH, 4, cur);
        fm_choice(fm_add(FF_CHOICE, "НА СКОЛЬКО", NULL), ST_DUR, 6, 0);
        fm_add(FF_TEXT, "ТЕКСТОВЫЙ СТАТУС (до 128)", custom_status());
        fm_choice(fm_add(FF_CHOICE, "ТЕКСТ УБРАТЬ ЧЕРЕЗ", NULL), ST_DUR, 6, 0);
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    }
#ifdef ASTRA_VIDEO
    case FM_SHARE: {                             /* источник (по умолчанию — окно) и режим */
        sh_n = share_windows(sh_t, sh_ids, 24);
        for (int i = 0; i < sh_n; i++) { char *t2 = san(sh_t[i], false); snprintf(sh_t[i], sizeof sh_t[i], "%s", t2); free(t2); sh_names[i] = sh_t[i]; }
        sh_names[sh_n] = "Весь экран";
        snprintf(FM.title, sizeof FM.title, "%s", TR("Демонстрация экрана"));
#ifdef _WIN32
        snprintf(FM.sub, sizeof FM.sub, "%s", TR("Эфир 1280x720, 30 к/с. Окно: видно только оно, даже под другими окнами; свёрнутое - зрители видят последний кадр. Весь экран: видно всё. Astra DS в эфир не попадает, включится режим стримера."));
#else
        if (share_wayland()) {                   /* Wayland: экран или окно выбирается в системном окне компоновщика */
            sh_n = 0;
            sh_names[0] = "Выбрать экран или окно (системное окно Wayland)";
            snprintf(FM.sub, sizeof FM.sub, "%s", TR("Wayland: экран или окно выберите в системном окне, оно откроется после нажатия. При показе экрана видно и окно Astra DS."));
        } else
        snprintf(FM.sub, sizeof FM.sub, "%s", TR("Внимание: в X11 при показе всего экрана видны все окна, в том числе это."));
#endif
        fm_choice(fm_add(FF_CHOICE, "ИСТОЧНИК", NULL), sh_names, sh_n + 1, 0);
        fm_choice(fm_add(FF_CHOICE, "КАЧЕСТВО", NULL), SH_MODES, 2, 0);
        fm_add(FF_TOGGLE, "Транслировать звук", NULL)->ch = 1;   /* окно — звук его программы; экран — всё, кроме Astra DS */
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Начать трансляцию"), FB_OK, 1);
        break;
    }
#endif
#ifdef ASTRA_VIDEO
    case FM_CAM: {                               /* камера: какая и показывать ли себя; список — cu_devices() */
        int cur = 0;
        const char *sel = O.demo ? "" : cam_selected();
        for (int i = 0; i < cu_ndev; i++) if (sel && *sel && !strcmp(cu_dev[i].id, sel)) cur = i;
        snprintf(FM.title, sizeof FM.title, "%s", TR("Камера"));
        snprintf(FM.sub, sizeof FM.sub, "%s", TR("Видео видят участники звонка, только со сквозным шифрованием (E2EE). Выключить - кнопка []/, F12 или Alt+C (сразу, без этого окна)."));
        fm_choice(fm_add(FF_CHOICE, "КАМЕРА", NULL), cu_names, cu_ndev, cur);
        fm_add(FF_TOGGLE, "Скрывать глаза (анонимизация)", NULL)->ch = anon_default();
        fm_add(FF_TOGGLE, "Показывать себя в колонке каналов", NULL)->ch = !cu_pv_hide;
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Включить камеру"), FB_OK, 1);
        break;
    }
#endif
    case FM_FALT:
        snprintf(FM.title, sizeof FM.title, TR("Описание картинки (alt)"));
        snprintf(FM.sub, sizeof FM.sub, TR("Для тех, кто не видит картинку. Пусто - без описания."));
        fm_add(FF_TEXT, "ОПИСАНИЕ", file_alt);
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Готово"), FB_OK, 1);
        break;
    case FM_TRENAME: case FM_GRENAME:
        snprintf(FM.title, sizeof FM.title, id == FM_TRENAME ? TR("Переименовать ветку") : TR("Переименовать группу"));
        fm_add(FF_TEXT, id == FM_TRENAME ? TR("НАЗВАНИЕ ВЕТКИ") : TR("НАЗВАНИЕ ГРУППЫ"), c && c->name ? c->name : "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    case FM_FPOST:
        snprintf(FM.title, sizeof FM.title, TR("Новый пост в %s"), lab);
        fm_add(FF_TEXT, "НАЗВАНИЕ ПОСТА", "");
        fm_add(FF_TEXT, "ТЕКСТ", "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Опубликовать"), FB_OK, 1);
        break;
    case FM_MEPROF: {
        snprintf(FM.title, sizeof FM.title, TR("Изменить профиль"));
        snprintf(FM.sub, sizeof FM.sub, TR("Отображаемое имя и 'О себе' видят все. Пусто - не менять."));
        fm_add(FF_TEXT, "ОТОБРАЖАЕМОЕ ИМЯ", S.me_name ? S.me_name : "");
        fm_add(FF_TEXT, "ОПИСАНИЕ: О СЕБЕ", "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    }
    case FM_FILTER:
        snprintf(FM.title, sizeof FM.title, TR("Фильтр слов"));
        snprintf(FM.sub, sizeof FM.sub, TR("Через запятую, до 16 слов. Такие сообщения сворачиваются. Пусто - выключить."));
        fm_add(FF_TEXT, "СЛОВА", filter_words());
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    case FM_VSTATUS: {
        const char *vsx = voice_channel_status(cid);
        snprintf(FM.title, sizeof FM.title, TR("Статус голосового канала"));
        snprintf(FM.sub, sizeof FM.sub, TR("Видят все на сервере. Пусто - убрать статус."));
        fm_add(FF_TEXT, "СТАТУС", vsx ? vsx : "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    }
    case FM_ALERTS:
        snprintf(FM.title, sizeof FM.title, TR("Слова-тревоги"));
        snprintf(FM.sub, sizeof FM.sub, TR("Через запятую, до 16 слов. Регистр не важен, ищется целое слово. Пусто - выключить."));
        fm_add(FF_TEXT, "СЛОВА", alert_words());
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    case FM_THREAD: {
        Message *m = msg_find(fm_mid, NULL);
        char nm[101] = "";
        size_t o = 0;
        for (const char *q = m && m->content ? m->content : ""; *q && o + 1 < sizeof nm; q++) {
            if (*q == ATT_MARK) continue;
            nm[o++] = *q == '\n' ? ' ' : *q;
        }
        nm[o] = 0;
        char *sn = san(nm, false);
        snprintf(FM.title, sizeof FM.title, TR("Создать ветку"));
        snprintf(FM.sub, sizeof FM.sub, TR("Ветка - отдельное обсуждение под каналом #%s."), lab);
        fm_add(FF_TEXT, "НАЗВАНИЕ ВЕТКИ", sn[0] ? sn : TR("новая ветка"));
        free(sn);
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Создать ветку"), FB_OK, 1);
        break;
    }
    case FM_CATCREATE:
        snprintf(FM.title, sizeof FM.title, TR("Создать категорию"));
        fm_add(FF_TEXT, "НАЗВАНИЕ КАТЕГОРИИ", TR("новая категория"));
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Создать категорию"), FB_OK, 1);
        break;
    case FM_CHEDIT: {
        ChanEdit ce;
        bool have = chan_info(cid, &ce);
        FM.loading = !have;                      /* настройки ещё грузятся — draw_form дозаполнит */
        snprintf(FM.title, sizeof FM.title, TR("Изменить канал %s%s"), c && !is_voice(c) ? "#" : "", lab);
        snprintf(FM.sub, sizeof FM.sub, "%s", have ? TR("Обзор канала. Сохраняются только изменённые поля.") : TR("загрузка настроек канала..."));
        fm_add(FF_TEXT, "НАЗВАНИЕ КАНАЛА", have && ce.name ? ce.name : lab);
        if (c && is_voice(c)) {
            fm_choice(fm_add(FF_CHOICE, "БИТРЕЙТ", NULL), BIT_N, 4, pick_idx(BIT_V, 4, have ? ce.bitrate : 64000));
            fm_choice(fm_add(FF_CHOICE, "ЛИМИТ ПОЛЬЗОВАТЕЛЕЙ", NULL), LIM_N, 6, pick_idx(LIM_V, 6, have ? ce.user_limit : 0));
        } else {
            fm_add(FF_TEXT, "ТЕМА КАНАЛА", have && ce.topic ? ce.topic : "");
            fm_choice(fm_add(FF_CHOICE, "МЕДЛЕННЫЙ РЕЖИМ", NULL), SLOW_N, 9, pick_idx(SLOW_S, 9, have ? ce.slowmode : 0));
            fm_add(FF_TOGGLE, "Канал 18+ (NSFW)", NULL)->ch = have && ce.nsfw > 0;
        }
        fm_btn(TR("Отмена"), FB_CANCEL, 0);
        if (can_manage_roles(gid)) fm_btn(TR("Права"), FB_X2, 0);
        fm_btn(TR("Удалить"), FB_X1, 2);
        fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    }
    case FM_CHDELETE:
        snprintf(FM.title, sizeof FM.title, TR("Удалить канал"));
        snprintf(FM.sub, sizeof FM.sub, TR("Вы точно хотите удалить %s%s? Это действие нельзя отменить."), c && !is_voice(c) ? "#" : "", lab);
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Удалить канал"), FB_OK, 2);
        break;
    case FM_PERMS: {
        snprintf(FM.title, sizeof FM.title, TR("Права канала %s%s"), c && !is_voice(c) ? "#" : "", lab);
        snprintf(FM.sub, sizeof FM.sub, TR("Для кого и что: запретить / как в сервере / разрешить."));
        /* цели: @everyone, роли сервера, участники с уже заданными правами */
        FM.ntg = 0;
        FM.tg[FM.ntg] = gid; FM.tgty[FM.ntg] = OW_ROLE; snprintf(FM.tgn[FM.ntg], 64, "@everyone"); FM.ntg++;
#ifdef MC_WAVE2
        const RoleName *rl = NULL;
        int nrl = guild_roles(gid, &rl);         /* имена ролей — только для открытого окна «Права» */
        for (int i = 0; i < nrl && FM.ntg < 24; i++) {
            if (rl[i].id == gid) continue;
            FM.tg[FM.ntg] = rl[i].id; FM.tgty[FM.ntg] = OW_ROLE;
            snprintf(FM.tgn[FM.ntg], 64, "@%s", rl[i].name ? rl[i].name : "?");
            FM.ntg++;
        }
#else
        for (int i = 0; g && i < g->nroles && FM.ntg < 24; i++) {
            if (g->roles[i].id == gid) continue;
            FM.tg[FM.ntg] = g->roles[i].id; FM.tgty[FM.ntg] = OW_ROLE;
            snprintf(FM.tgn[FM.ntg], 64, TR("роль %llu"), (unsigned long long)g->roles[i].id);
            FM.ntg++;
        }
#endif
        for (int i = 0; c && i < c->now_ && FM.ntg < 24; i++) if (c->ow[i].type == OW_MEMBER) {
            FM.tg[FM.ntg] = c->ow[i].id; FM.tgty[FM.ntg] = OW_MEMBER; snprintf(FM.tgn[FM.ntg], 64, "%s", user_name(c->ow[i].id)); FM.ntg++;
        }
        for (int i = 0; i < FM.ntg; i++) FM.tgp[i] = FM.tgn[i];
        fm_choice(fm_add(FF_CHOICE, "ДЛЯ КОГО", NULL), FM.tgp, FM.ntg, 0);
        for (int i = 0; i < NPERM; i++) {
            if (PERM_ROWS[i].voice != (c && is_voice(c)) && i >= 5) continue;
            if (FM.nf >= 9) break;
            fm_choice(fm_add(FF_TRI, PERM_ROWS[i].n, NULL), TRI_N, 3, 1);
        }
        FM.tgt = gid; FM.tgt_type = OW_ROLE;
        perms_load();
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Сбросить"), FB_X1, 0); fm_btn(TR("Сохранить"), FB_OK, 1);
        break;
    }
    case FM_ADDFRIEND:
        snprintf(FM.title, sizeof FM.title, TR("Добавить в друзья"));
        snprintf(FM.sub, sizeof FM.sub, TR("Добавьте друга по имени пользователя (латиница, цифры, _ и .)."));
        fm_add(FF_TEXT, "ИМЯ ПОЛЬЗОВАТЕЛЯ", "");
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Отправить запрос дружбы"), FB_OK, 1);
        break;
    default: break;
    }
    if (!FM.nf) FM.foc = FM.nb - 1;              /* без полей — фокус на главной кнопке (справа) */
    fm_mark();                                   /* исходные значения: отправляются только изменённые */
    U.menu = U.pick = U.qs = U.ep = false;
}
/* Окно формы посреди экрана, подложка затемнена. */
static void draw_form(int W, int H) {
    if (!FM.id) return;
    int bw = W - 4 < 66 ? W - 4 : 66, iw = bw - 6;
    if (FM.id == FM_PERMS && FM.nf > 0 && FM.f[0].ch >= 0 && FM.f[0].ch < FM.ntg && FM.tg[FM.f[0].ch] != FM.tgt) {
        FM.tgt = FM.tg[FM.f[0].ch]; FM.tgt_type = FM.tgty[FM.f[0].ch]; perms_load();
    }
    if (FM.id == FM_INVITE && FM.nf >= 3) { const char *l = invite_created(); if (l) utf8_cpy(FM.f[2].val, FM.f[2].cap, l); }
    if (FM.loading && FM.id == FM_GSET && FM.nf >= 2) {
        const char *d = guild_description(FM.gid);
        if (d) { if (!fm_changed(&FM.f[1])) fm_set(&FM.f[1], d); else FM.f[1].oh = fnv(d, strlen(d)); FM.loading = false; }
    }
    if (FM.loading && FM.id == FM_CHEDIT) {
        ChanEdit ce;
        Channel *c = chan_find(FM.cid);
        if (c && chan_info(FM.cid, &ce)) {
            /* пришли настоящие значения: заполнить нетронутые поля, исходные — от сервера */
            if (!fm_changed(&FM.f[0])) fm_set(&FM.f[0], ce.name ? ce.name : "");
            if (is_voice(c) && FM.nf >= 3) {
                FM.f[1].ch = FM.f[1].och = pick_idx(BIT_V, 4, ce.bitrate);
                FM.f[2].ch = FM.f[2].och = pick_idx(LIM_V, 6, ce.user_limit);
            } else if (FM.nf >= 4) {
                if (!fm_changed(&FM.f[1])) fm_set(&FM.f[1], ce.topic ? ce.topic : "");
                FM.f[2].ch = FM.f[2].och = pick_idx(SLOW_S, 9, ce.slowmode);
                FM.f[3].ch = FM.f[3].och = ce.nsfw > 0;
            }
            snprintf(FM.sub, sizeof FM.sub, TR("Обзор канала. Сохраняются только изменённые поля."));
            FM.loading = false;
        }
    }
    char **sub = NULL;
    int nsub = FM.sub[0] ? help_wrap(FM.sub, iw, &sub) : 0;
    if (nsub > 3) { for (int i = 3; i < nsub; i++) free(sub[i]); nsub = 3; }
    const InviteState *is = FM.id == FM_JOINCARD ? invite_state() : NULL;
    int body = 0;
    for (int i = 0; i < FM.nf; i++) body += FM.f[i].kind == FF_TOGGLE || FM.f[i].kind == FF_TRI ? 1 : 2;
    char **risk = NULL;
    int nrisk = 0;
#ifdef MC_WAVE2
    if (is && is->risk_warning && invite_risk_text()) nrisk = help_wrap(invite_risk_text(), iw, &risk);
    if (nrisk > 6) { for (int i = 6; i < nrisk; i++) free(risk[i]); nrisk = 6; }
    if (is && FM.nb >= 2) {                      /* главная кнопка — по состоянию приглашения */
        FM.b[1].t = is->need_browser ? (vless_state() != VL_OFF ? TR("Открыть в браузере (мимо VLESS)") : TR("Открыть в браузере")) : is->risk_warning ? TR("Понятно, вступить") : is->joined ? TR("Готово") : TR("Вступить на сервер");
        FM.b[1].id = is->need_browser ? FB_X1 : FB_OK;
    }
#endif
    if (is) body = 6 + (nrisk ? nrisk + 1 : 0);
    int bh = 2 + 1 + nsub + 1 + body + (FM.err[0] || FM.loading ? 1 : 0) + 2;
    if (bh > H - 2) bh = H - 2;
    int px = (W - bw) / 2, py = (H - bh) / 2;
    g_dim(0, 0, W, H);
    ovl_cut(0, 0, W, H);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_FEED);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    bool danger = FM.id == FM_GDELETE || FM.id == FM_LEAVE || FM.id == FM_CHDELETE;
    int y = py + 1, x = px + 3;
    g_texte(x, y, FM.title, iw - 3, danger ? C_RED : C_WHITE, AT_B);
    g_text(px + bw - 3, y, "x", 1, C_TEXT, AT_B);
    hit_add(px + bw - 4, y, 3, 1, A_FORMB, FB_CANCEL, 0, HV_RED);
    y++;
    for (int i = 0; i < nsub; i++) { if (y < py + bh - 3) g_text(x, y++, sub[i], iw, C_MUTED, 0); free(sub[i]); }
    free(sub);
    y++;
    if (is) {
        /* карточка сервера из приглашения: значок-сокращение, имя, в сети / участники, канал, кто пригласил */
        if (is->busy) g_text(x, y, TR("загрузка приглашения..."), iw, C_MUTED, 0);
        else if (is->error[0]) { char *e = san(is->error, false); g_texte(x, y, e, iw, C_RED, 0); free(e); }
        else {
            char ab[16], t[160];
            abbr(is->guild_name ? is->guild_name : "?", ab, sizeof ab);
            for (int r = 0; r < 3; r++) g_circle_row(x, y + r, 6, 3, r, C_BTN2, r == 1 ? ab : NULL, C_WHITE);
            g_texte(x + 8, y, is->guild_name ? is->guild_name : "?", iw - 8, C_WHITE, AT_B);
            g_ch(x + 8, y + 1, 'o', C_GREEN, AT_B);
            snprintf(t, sizeof t, TR("%d в сети   "), is->online);
            int u = g_text(x + 10, y + 1, t, iw - 10, C_MUTED, 0);
            g_ch(x + 10 + u, y + 1, 'o', C_MUTED, AT_B);
            snprintf(t, sizeof t, "%d %s", is->members, plural(is->members, TR("участник"), TR("участника"), TR("участников")));
            g_text(x + 12 + u, y + 1, t, iw - 12 - u, C_MUTED, 0);
            snprintf(t, sizeof t, TR("канал #%s%s%s"), is->channel_name ? is->channel_name : "?", is->inviter ? TR(", пригласил(а) ") : "", is->inviter ? is->inviter : "");
            g_texte(x + 8, y + 2, t, iw - 8, C_MUTED, 0);
            const char *st = is->joined ? TR("Вы на этом сервере.") : TR("Вступление - только щелчком по кнопке ниже (или Tab и пробел).");
#ifdef MC_WAVE2
            if (is->need_browser) st = TR("Discord требует вступить через браузер.");
#endif
            g_text(x, y + 4, st, iw, is->joined ? C_GREEN : C_TEXT, 0);
        }
        y += 6;
        for (int i = 0; i < nrisk; i++) { if (y < py + bh - 3) g_text(x, y++, risk[i], iw, C_GOLD, 0); free(risk[i]); }
        if (nrisk) y++;
        free(risk);
    }
    for (int i = 0; i < FM.nf && y < py + bh - 2; i++) {
        FField *f = &FM.f[i];
        bool foc = FM.foc == i;
        if (f->kind == FF_TOGGLE) {
            g_text(x, y, f->ch ? "[x]" : "[ ]", 3, foc ? C_WHITE : C_TEXT, AT_B);
            g_texte(x + 4, y, tr(f->label), iw - 4, foc ? C_WHITE : C_TEXT, foc ? AT_U : 0);
            hit_add(x, y, iw, 1, A_FORMF, 0, i, HV_UL);
            y++;
            continue;
        }
        if (f->kind == FF_TRI) {
            g_texte(x, y, tr(f->label), iw - 20, foc ? C_WHITE : C_TEXT, foc ? AT_U : 0);
            static const u32 tc[3] = { C_RED, C_MUTED, C_GREEN };
            static const char *const ts[3] = { " x ", " / ", " v " };
            for (int k = 0; k < 3; k++) {
                int bx = x + iw - 15 + k * 5;
                bool on = f->ch == k;
                g_pill(bx, y, 5, on ? (k == 0 ? C_DANGER : k == 2 ? C_GREEN : C_BTN2) : C_RAIL);
                g_text(bx + 1, y, ts[k], 3, on ? C_WHITE : tc[k], AT_B);
                hit_add(bx, y, 5, 1, A_FORMF, (u8)(1 + k), i, HV_PILL);
            }
            y++;
            continue;
        }
        g_text(x, y++, tr(f->label), iw, C_MUTED, AT_B);
        g_pill(x - 1, y, iw + 2, C_RAIL);
        if (f->kind == FF_CHOICE) {
            g_text(x, y, "<", 1, C_TEXT, AT_B);
            g_ctext(x + 2, y, iw - 4, f->nch ? tr(f->chs[f->ch]) : "", foc ? C_WHITE : C_TEXT, AT_B);
            g_text(x + iw - 1, y, ">", 1, C_TEXT, AT_B);
            hit_add(x - 1, y, 3, 1, A_FORMF, 4, i, HV_UL);
            hit_add(x + iw - 2, y, 3, 1, A_FORMF, 5, i, HV_UL);
            hit_add(x + 2, y, iw - 4, 1, A_FORMF, 0, i, HV_UL);
        } else {
            /* текст: виден конец, курсор в конце у поля в фокусе */
            int qw = vw(f->val), skip = qw > iw - 2 ? qw - (iw - 2) : 0, acc = 0, used = 0;
            for (const char *p = f->val; *p; ) {
                u32 c;
                p += utf8_dec(p, &c);
                int cw2 = shown_w(c);
                if (f->kind == FF_INFO && FM.id == FM_INVITE && streamer_on() && strstr(f->val, "discord")) { g_text(x, y, TR("скрыто (режим стримера) - Копировать работает"), iw - 2, C_MUTED, 0); break; }
                if (acc >= skip && used + cw2 <= iw - 2) { g_put(x + used, y, c, cw2, f->kind == FF_INFO ? C_URL : C_WHITE, 0); used += cw2; }
                acc += cw2;
            }
            if (foc && f->kind == FF_TEXT) g_ch(x + used, y, ' ', C_TEXT, AT_I);
            hit_add(x - 1, y, iw + 2, 1, A_FORMF, 0, i, 0);
        }
        y++;
    }
    if (FM.err[0]) { g_texte(x, py + bh - 3, FM.err, iw, C_RED, 0); }
#ifdef MC_WAVE2
    else if (FM.loading && FM.id == FM_CHEDIT && chan_info_failed(FM.cid)) {
        char t[120];
        snprintf(t, sizeof t, TR("настройки канала не загрузились (код %ld): 'Повторить' - r"), chan_info_failed(FM.cid));
        g_texte(x, py + bh - 3, t, iw, C_RED, 0);
        hit_add(x, py + bh - 3, iw, 1, A_FORMB, FB_RETRY, 0, HV_UL);
    }
#endif
    else if (FM.loading) g_texte(x, py + bh - 3, TR("загрузка текущих значений... 'Сохранить' - после загрузки"), iw, C_MUTED, 0);
    /* кнопки справа */
    int tot = 0;
    for (int i = 0; i < FM.nb; i++) tot += vw(FM.b[i].t) + 5;
    int bx = px + bw - 3 - tot + 1;
    if (bx < x) bx = x;
    for (int i = 0; i < FM.nb; i++) {
        int w = g_button(bx, py + bh - 2, FM.b[i].t, px + bw - 2 - bx, FM.b[i].style, B_NOP);
        if (w > 0 && U.nhit) { U.hit[U.nhit - 1].act = A_FORMB; U.hit[U.nhit - 1].sub = (u8)FM.b[i].id; U.hit[U.nhit - 1].tip = 0; }
        if (FM.foc == FM.nf + i) g_fgset(bx + 2, py + bh - 2, w - 4, C_WHITE, AT_B | AT_U);
        bx += w + 1;
    }
}

/* ---- пакет А: «Просмотр реакций» (кто поставил) и «Переслать» — запросы и состояние в store.c
 * (reactors_open/reactors_state/reactors_close, msg_forward); здесь только окно и вызовы. ---- */
static void rv_close(void) { reactors_close(); U.dirty = true; }
static void rv_open(Message *m) {
    if (!m || !m->nreacts) { ui_notice(TR("у сообщения нет реакций")); return; }
    U.menu = false;
    reactors_open(S.open_ch, m->id, 0);
}
/* Окно «Реакции»: слева реакции (выбор Up/Down или щелчком), справа — кто поставил. */
static void draw_rv(int W, int H) {
    const ReactorsState *rs = reactors_state();
    Message *m = rs ? msg_find(rs->msg, NULL) : NULL;
    if (!m || rs->ch != S.open_ch) { reactors_close(); return; }
    int bw = W - 4 < 60 ? W - 4 : 60, bh = H - 4 < 16 ? H - 4 : 16;
    if (bh < 6) return;
    int px = (W - bw) / 2, py = (H - bh) / 2;
    g_dim(0, 0, W, H);
    ovl_cut(0, 0, W, H);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    u32 ut = bg_at(px, py), ub = bg_at(px, py + bh - 1);
    g_rect(px, py, bw, bh, C_FEED);
    g_round_corners(px, py, bw, bh, 0, ut);
    g_round_corners(px, py + bh - 1, bw, bh, bh - 1, ub);
    g_text(px + 3, py + 1, TR("Реакции"), bw - 8, C_WHITE, AT_B);
    g_text(px + bw - 3, py + 1, "x", 1, C_TEXT, AT_B);
    hit_add(px + bw - 4, py + 1, 3, 1, A_BTN, B_RV_X, 0, HV_RED);
    int lw = 16, rows = bh - 4;
    g_rect(px + 1, py + 3, lw, rows, C_SIDE);
    for (int i = 0; i < m->nreacts && i < rows; i++) {
        char lb[128];
        react_label(&m->reacts[i], lb, sizeof lb);
        int y = py + 3 + i;
        if (i == rs->sel) g_pill(px + 1, y, lw, C_SEL);
        g_texte(px + 3, y, lb + 1, lw - 4, i == rs->sel ? C_WHITE : C_TEXT, i == rs->sel ? AT_B : 0);
        hit_add(px + 1, y, lw, 1, A_RVSEL, 0, i, HV_PILL);
    }
    int rx = px + lw + 3, rw = bw - lw - 6;
    if (rs->busy) g_text(rx, py + 3, TR("загрузка..."), rw, C_MUTED, 0);
    else if (rs->err[0]) { char *e = san(rs->err, false); g_texte(rx, py + 3, e, rw, C_RED, 0); free(e); }
    else for (int i = 0; i < rs->n && i < rows; i++) g_texte(rx, py + 3 + i, rs->names[i], rw, C_TEXT, 0);
    if (!rs->busy && !rs->err[0] && rs->sel < m->nreacts && m->reacts[rs->sel].count > rs->n && rs->n == 25 && rows > 0)
        g_text(rx, py + bh - 2, TR("показаны первые 25"), rw, C_MUTED, 0);
}
/* «Переслать»: Ctrl+K в режиме выбора беседы/канала; пересылку делает store (msg_forward), итог — уведомлением. */
static void forward_to(u64 target) {
    Message *m = msg_find(U.fwd_msg, NULL);
    Channel *dst = chan_find(target);
    u64 src = U.fwd_ch;
    U.fwd_msg = 0;
    if (!m || !dst || is_voice(dst)) { ui_notice(TR("переслать можно в текстовый канал или беседу")); return; }
    msg_forward(target, src, m->id);
}
/* Полная дата для подсказки при наведении на время сообщения: «понедельник, 9 ноября 2026 г. в 16:01». */
static void date_full(i64 ts, char *out, size_t n) {
    static const char *const wd[] = { "воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота" };
    static const char *const mon[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                                       "сентября", "октября", "ноября", "декабря" };
    struct tm t;
    plat_localtime(ts, &t);
    snprintf(out, n, TR("%s, %d %s %d г. в %02d:%02d"), tr(wd[(t.tm_wday % 7 + 7) % 7]), t.tm_mday, tr(mon[(t.tm_mon % 12 + 12) % 12]), t.tm_year + 1900, t.tm_hour, t.tm_min);
}

/* Текст результата поиска: до 3 строк переноса (more — не всё влезло). */
static int sr_text_lines(const char *s, int tw, size_t st[3], size_t en[3], bool *more) {
    size_t len = strlen(s), at = 0;
    int n = 0;
    *more = false;
    if (tw < 4) tw = 4;
    while (at < len) {
        if (n == 3) { *more = true; break; }
        size_t e, nx = wrap1(s, at, len, tw, &e);
        st[n] = at; en[n] = e; n++;
        at = nx;
    }
    return n;
}
/* Правая панель сообщений (как в Discord): результаты поиска, закреплённые, упоминания («Входящие»).
 * Карточки: канал (когда сменился), аватар 4x2, ник цветом роли, время, до 3 строк текста. Щелчок или Enter —
 * поиск: открыть канал; закреплённые и упоминания: перейти к сообщению (msgs_jump). Состояния — словами. */
enum { MP_SEARCH, MP_PINS, MP_MENT };
static const Message *mp_msg(int kind, int i, u64 *ch) {
    if (kind == MP_SEARCH) { const SearchState *s = search_state(); *ch = s->hits[i].channel_id; return &s->hits[i].m; }
    if (kind == MP_PINS) { const PinsState *p = pins_state(); *ch = p->channel_id; return &p->m[i]; }
    const MentionsState *m = mentions_state();
    *ch = m->hits[i].channel_id;
    return &m->hits[i].m;
}
/* №125: в «Входящих» и списках упоминаний текст ЛС в режиме стримера не показывается */
static const char *mp_text(const Message *m, u64 ch) {
    const Channel *c = chan_find(ch);
    if (streamer_on() && c && !c->guild_id) return TR("сообщение в ЛС скрыто (режим стримера)");
    return m->content ? m->content : "";
}
static void draw_mpanel(int x, int w, int H, int kind, int top) {
    char t[220];
    bool busy, idx = false;
    const char *err, *ttl0, *msg0;
    int n, total;
    if (kind == MP_SEARCH) {
        const SearchState *st = search_state();
        busy = st->busy; idx = st->indexing; err = st->error; n = st->nhits < SEARCH_PAGE ? st->nhits : SEARCH_PAGE; total = st->total;
        ttl0 = TR("Ничего не найдено"); msg0 = TR("Попробуйте другие слова.");
    } else if (kind == MP_PINS) {
        const PinsState *ps = pins_state();
        busy = ps->busy; err = ps->error; n = ps->n; total = n;
        ttl0 = TR("Здесь пока нет закреплённых сообщений"); msg0 = TR("Закрепить: меню сообщения (правый щелчок или ...).");
    } else {
        const MentionsState *ms = mentions_state();
        busy = ms->busy; err = ms->error; n = ms->n; total = n;
        ttl0 = TR("Упоминаний нет"); msg0 = TR("Здесь появятся сообщения, где вас упомянули.");
    }
    int bot = H - 2;
#ifdef MC_PACKA
    bool pager = kind == MP_SEARCH && search_state()->page_count > 1;
    if (pager) bot--;                            /* строка H-2 — «< стр. 1 из N >» */
#endif
    hit_add(x, top, w, bot - top + 1, A_AR_LIST, 0, 1, 0);
    if (busy || idx || err[0] || !n) {
        const char *ttl, *msg;
        u32 tc = C_WHITE;
        char ed[200];
        if (busy) { ttl = kind == MP_SEARCH ? TR("Ищем сообщения...") : TR("Загрузка..."); msg = ""; }
        else if (idx) { ttl = TR("Идёт индексация"); msg = TR("Discord ещё готовит поиск по этому серверу. Повторим сами через несколько секунд."); tc = C_GOLD; }
        else if (err[0]) {
            char *e = san(err, false);
            deco_ascii(e, ed, sizeof ed);
            free(e);
            ttl = kind == MP_SEARCH ? TR("Поиск не удался") : TR("Не загрузилось"); msg = ed; tc = C_RED;
        } else { ttl = ttl0; msg = msg0; }
        int y = top + 2;
        char **ls;
        int m = help_wrap(ttl, w - 4, &ls);
        for (int i = 0; i < m; i++) { if (y <= bot) g_ctext(x, y++, w, ls[i], tc, AT_B); free(ls[i]); }
        free(ls);
        m = help_wrap(msg, w - 4, &ls);
        for (int i = 0; i < m; i++) { if (y <= bot) g_ctext(x, y++, w, ls[i], C_MUTED, 0); free(ls[i]); }
        free(ls);
        return;
    }
    if (n > 64) n = 64;
    if (U.sr_sel >= n) U.sr_sel = n - 1;
    if (U.sr_sel < 0) U.sr_sel = 0;
    int tw = w - 10;
    int hh[64];
    u64 ch;
    for (int i = 0; i < n; i++) {
        size_t s0[3], e0[3];
        bool more;
        const Message *m = mp_msg(kind, i, &ch);
        int nl = sr_text_lines(mp_text(m, ch), tw, s0, e0, &more);
        hh[i] = 1 + (nl < 1 ? 1 : nl) + 1 + 1;   /* заголовок, текст, промежуток, строка канала (с запасом) */
    }
    int avail = bot - top + 1;
    if (U.sr_scroll > U.sr_sel) U.sr_scroll = U.sr_sel;
    for (;;) {
        int sum = 0;
        for (int i = U.sr_scroll; i <= U.sr_sel; i++) sum += hh[i];
        if (sum <= avail || U.sr_scroll >= U.sr_sel) break;
        U.sr_scroll++;
    }
    if (U.sr_scroll > n - 1) U.sr_scroll = n - 1;
    if (U.sr_scroll < 0) U.sr_scroll = 0;
    int y = top;
    u64 prev_ch = 0;
    for (int i = U.sr_scroll; i < n && y <= bot; i++) {
        const Message *m = mp_msg(kind, i, &ch);
        if (kind != MP_PINS && (i == U.sr_scroll || ch != prev_ch)) {
            const Channel *c = chan_find(ch);
            char lab[96] = "?";
            if (c) chan_label(c, lab, sizeof lab);
            const Guild *g = c && c->guild_id ? guild_find(c->guild_id) : NULL;
            snprintf(t, sizeof t, "%s %s%s%s", c ? chan_mark(c) : "#", lab, g ? "  " : "", g ? g->name : "");
            g_texte(x + 2, y++, t, w - 4, C_MUTED, AT_B);
        }
        prev_ch = ch;
        size_t s0[3], e0[3];
        bool more;
        const char *body = mp_text(m, ch);
        int nl = sr_text_lines(body, tw, s0, e0, &more), chh = 1 + (nl < 1 ? 1 : nl);
        if (y + chh - 1 > bot) break;
        bool sel = U.sr_focus && i == U.sr_sel;
        u32 cb = sel ? C_SEL : C_FEED;
        g_rect(x + 1, y, w - 2, chh, cb);
        g_round_corners(x + 1, y, w - 2, chh, 0, C_SIDE);
        g_round_corners(x + 1, y + chh - 1, w - 2, chh, chh - 1, C_SIDE);
        hit_add(x + 1, y, w - 2, chh, A_SRES, (u8)kind, i, HV_MSG);
        for (int r = 0; r < 2; r++) { g_avatar(x + 3, y + r, 4, 2, r, m->author_id, NULL); strip_part(G_SRCH, x + 3, y + r, 4, 2, r, PK_AVR, m->author_id); }
        char tm[24];
        fmt_time(m->ts, tm, sizeof tm);
        u32 nc = m->color ? m->color : C_WHITE;
        int u = g_texte(x + 8, y, m->author ? m->author : "?", tw - vw(tm) - 1 > 4 ? tw - vw(tm) - 1 : 4, nc, AT_B);
        g_text(x + 9 + u, y, tm, tw - u - 1, C_MUTED, 0);
        if (!nl) g_text(x + 8, y + 1, TR("(вложение)"), tw, C_MUTED, 0);
        for (int k = 0; k < nl; k++) {
            int uu = g_textn(x + 8, y + 1 + k, body + s0[k], e0[k] - s0[k], more && k == nl - 1 ? tw - 3 : tw, C_TEXT, 0);
            if (more && k == nl - 1) g_text(x + 8 + uu, y + 1 + k, "...", 3, C_MUTED, 0);
        }
        y += chh + 1;
    }
#ifdef MC_PACKA
    if (kind == MP_SEARCH && search_state()->page_count > 1) {
        const SearchState *ss = search_state();
        if (w >= 24) snprintf(t, sizeof t, TR("<  стр. %d из %d  >"), ss->page + 1, ss->page_count);
        else snprintf(t, sizeof t, "< %d/%d >", ss->page + 1, ss->page_count);
        int bx = g_ctext(x, H - 2, w, t, C_TEXT, AT_B);
        if (ss->page > 0) hit_add(bx, H - 2, 2, 1, A_BTN, B_SP_PREV, 0, HV_UL);
        if (ss->page + 1 < ss->page_count) hit_add(bx + vw(t) - 2, H - 2, 2, 1, A_BTN, B_SP_NEXT, 0, HV_UL);
    } else
#endif
    if (kind == MP_SEARCH && total > n) {
        snprintf(t, sizeof t, TR("показаны первые %d из %d"), n, total);
        g_ctext(x, H - 1, w, t, C_MUTED, 0);
    }
    if (kind == MP_PINS && pins_state()->more) {
        g_ctext(x, H - 1, w, TR("Загрузить ещё (End)"), C_URL, AT_U);
        hit_add(x + 1, H - 1, w - 2, 1, A_BTN, B_PINS_MORE, 0, HV_UL);
    }
}
/* k-я строка «Непрочитанное» (сначала каналы с упоминаниями): индекс в S.chans или -1. */
static bool inbox_row(const Channel *c, int pass) {
    return c->type != CH_CATEGORY && c->visible && !c->muted && chan_unread(c) && (pass == 0) == (c->mentions > 0);
}
static int inbox_nth(int k) {
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < S.nchans; i++) if (inbox_row(&S.chans[i], pass) && k-- == 0) return i;
    return -1;
}
/* «Входящие» (Ctrl+I): вкладки «Непрочитанное» (из счётчиков store — без запросов) и «Упоминания». */
static void draw_inbox(int x, int w, int H) {
    int tx = x + 2;
    static const char *const tabs[2] = { "Непрочитанное", "Упоминания" };
    for (int i = 0; i < 2; i++) {
        int tw0 = vw(tr(tabs[i])) + 2;
        if (tx + tw0 > x + w - 2) break;
        if (i == U.inbox_tab) g_pill(tx, 3, tw0, C_SEL);
        g_text(tx + 1, 3, tr(tabs[i]), tw0 - 2, i == U.inbox_tab ? C_WHITE : C_MUTED, AT_B);
        hit_add(tx, 3, tw0, 1, A_BTN, i ? B_INBOX_T1 : B_INBOX_T0, 0, HV_PILL);
        tx += tw0 + 1;
    }
    if (U.inbox_tab == 1) { draw_mpanel(x, w, H, MP_MENT, 5); return; }
    /* непрочитанные каналы: сначала с упоминаниями */
    int y = 5, bot = H - 3, n = 0;
    hit_add(x, y, w, bot - y + 1, A_AR_LIST, 0, 1, 0);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < S.nchans; i++) {
            const Channel *c = &S.chans[i];
            if (!inbox_row(c, pass)) continue;
            if (y + 1 > bot) { n++; continue; }
            bool sel = U.sr_focus && n == U.sr_sel;
            char lab[96], t[200];
            chan_label(c, lab, sizeof lab);
            const Guild *g = c->guild_id ? guild_find(c->guild_id) : NULL;
            g_rect(x + 1, y, w - 2, 2, sel ? C_SEL : C_FEED);
            g_round_corners(x + 1, y, w - 2, 2, 0, C_SIDE);
            g_round_corners(x + 1, y + 1, w - 2, 2, 1, C_SIDE);
            snprintf(t, sizeof t, "%s %s", chan_mark(c), lab);
            int right = x + w - 3;
            if (c->mentions) {
                char mb[12];
                snprintf(mb, sizeof mb, "%d", c->mentions);
                int mw = vw(mb) + 2;
                g_pill(right - mw, y, mw, C_RED);
                g_text(right - mw + 1, y, mb, mw - 2, C_WHITE, AT_B);
                right -= mw + 1;
            }
            g_texte(x + 3, y, t, right - x - 3, C_WHITE, AT_B);
            g_texte(x + 3, y + 1, g ? g->name : TR("личные сообщения"), w - 6, C_MUTED, 0);
            hit_add(x + 1, y, w - 2, 2, A_SRES, 3, i, HV_MSG);
            y += 3;
            n++;
        }
    if (!n) {
        g_ctext(x, 8, w, TR("Всё прочитано"), C_WHITE, AT_B);
        g_ctext(x, 9, w, TR("Новые сообщения появятся здесь."), C_MUTED, 0);
    } else {
        g_button(x + 2, H - 1, TR("Прочитать всё"), w - 4, 0, B_ACK_ALL);
    }
    U.inbox_n = n;
}

/* Правая панель: поиск / закреплённые / входящие — шапка с заголовком и x, под ней содержимое. */
static void draw_search(int x, int w, int H) {
    g_rect(x, 1, w, H - 1, C_SIDE);
    char t[220];
    if (U.rp == RP_PINS) snprintf(t, sizeof t, TR("Закреплённые сообщения"));
    else if (U.rp == RP_INBOX) snprintf(t, sizeof t, TR("Входящие"));
    else {
        const SearchState *st = search_state();
        if (st->busy) snprintf(t, sizeof t, TR("Поиск..."));
        else if (st->indexing) snprintf(t, sizeof t, TR("Индексация..."));
        else if (st->error[0]) snprintf(t, sizeof t, TR("Ошибка поиска"));
        else snprintf(t, sizeof t, "%d %s", st->total, plural(st->total, TR("результат"), TR("результата"), TR("результатов")));
    }
    g_texte(x + 2, 1, t, w - 7, C_WHITE, AT_B);
    g_text(x + w - 3, 1, "x", 1, C_TEXT, AT_B);
    hit_add(x + w - 4, 1, 3, 1, A_BTN, B_SRCH_X, 0, HV_RED);
    g_uline(x, 1, w);
    if (U.rp == RP_PINS) draw_mpanel(x, w, H, MP_PINS, 3);
    else if (U.rp == RP_INBOX) draw_inbox(x, w, H);
    else draw_mpanel(x, w, H, MP_SEARCH, 3);
}

/* ---- колонка серверов (10 клеток): кнопка «домой», черта, значки 6x3, «+», справа тонкая линия ----
 * столбец 0 — «пилюля» (выбран / непрочитано), 1..6 — значок, 7..8 — бейдж упоминаний, 9 — линия-рамка.
 * Значок с картинкой — квадрат из img.c (в окне поверх — мелкая сетка, полосой подряд идущих значков);
 * без картинки — сокращение названия в скобках, как на эталоне: [CS] [M] [GT]. */
#define RAIL_W 10
/* №2: развёрнутые папки (по id) — только в памяти */
static i64 fold_open_ids[16];
static bool folder_open(i64 id) { for (int i = 0; i < 16; i++) if (fold_open_ids[i] == id && id) return true; return false; }
static void folder_toggle(int k) {
    const GuildFolder *fd = NULL;
    int n = guild_folders(&fd);
    if (k < 0 || k >= n) return;
    i64 id = fd[k].id ? fd[k].id : -(i64)(k + 1);
    for (int i = 0; i < 16; i++) if (fold_open_ids[i] == id) { fold_open_ids[i] = 0; return; }
    for (int i = 0; i < 16; i++) if (!fold_open_ids[i]) { fold_open_ids[i] = id; return; }
}
static void draw_rail(int H) {
    g_rect(0, 0, RAIL_W, H, C_RAIL);
    for (int y = 0; y < H; y++) g_ch(RAIL_W - 1, y, '|', C_LINE, 0);
    hit_add(0, 0, RAIL_W - 1, H, A_AR_RAIL, 0, 0, 0);
    int top = 6, step = 4;
    int vg = (H - top - step) / step;            /* значков помещается (3 строки + промежуток), снизу «+» */
    if (vg < 1) vg = 1;
    /* №2: порядок полосы — серверы по порядку store; сервер из папки показывается в месте папки: свёрнутая —
     * одна плитка папки, развёрнутая — шапка папки и её серверы на подложке цвета папки */
    const GuildFolder *fd = NULL;
    int nfd = guild_folders(&fd);
    typedef struct { short kind, idx, fk; } RItem;   /* kind 0 — сервер (idx в S.guilds + 1), 1 — шапка папки fk */
    int cap = S.nguilds + nfd + 1;              /* ревью 15 К1: каждый сервер — не больше одного раза, запись — в пределах cap */
    RItem *it = xmalloc((size_t)cap * sizeof(RItem));
    int ni = 0;
    bool *done = xcalloc((size_t)(nfd > 0 ? nfd : 1), sizeof(bool));
    bool *shown = xcalloc((size_t)(S.nguilds > 0 ? S.nguilds : 1), sizeof(bool));
    for (int gi = 0; gi < S.nguilds; gi++) {
        int fk = -1;
        for (int k = 0; k < nfd && fk < 0; k++)
            if (fd[k].n > 1 || fd[k].id) for (int q = 0; q < fd[k].n; q++) if (fd[k].gids[q] == S.guilds[gi].id) fk = k;
        if (fk < 0) { if (!shown[gi] && ni < cap) { shown[gi] = true; it[ni++] = (RItem){ 0, (short)(gi + 1), -1 }; } continue; }
        if (done[fk]) continue;
        done[fk] = true;
        if (ni < cap) it[ni++] = (RItem){ 1, 0, (short)fk };
        if (folder_open(fd[fk].id ? fd[fk].id : -(i64)(fk + 1)))
            for (int q = 0; q < fd[fk].n; q++)
                for (int gj = 0; gj < S.nguilds; gj++)
                    if (S.guilds[gj].id == fd[fk].gids[q] && !shown[gj] && ni < cap) { shown[gj] = true; it[ni++] = (RItem){ 0, (short)(gj + 1), (short)fk }; }
    }
    free(done);
    free(shown);
    int selpos = -1;
    for (int i = 0; i < ni; i++) if (it[i].kind == 0 && it[i].idx == U.sel_guild) selpos = i;
    if (selpos < 0 && U.sel_guild >= 1)          /* выбранный сервер в свёрнутой папке — на её плитке */
        for (int i = 0; i < ni; i++) if (it[i].kind == 1) for (int q = 0; q < fd[it[i].fk].n; q++)
            if (U.sel_guild - 1 < S.nguilds && fd[it[i].fk].gids[q] == S.guilds[U.sel_guild - 1].id) selpos = i;
    if (!U.guild_free && selpos >= 0) {
        if (selpos < U.guild_scroll) U.guild_scroll = selpos;
        if (selpos >= U.guild_scroll + vg) U.guild_scroll = selpos - vg + 1;
    }
    if (U.guild_scroll > ni - vg) U.guild_scroll = ni - vg;
    if (U.guild_scroll < 0) U.guild_scroll = 0;
    for (int i = 2; i < 6; i++) g_ch(i, 4, '-', C_LINE, 0);
    int last_y = 0;
    for (int ry = -1; ry < vg; ry++) {
        int pos = ry < 0 ? -1 : U.guild_scroll + ry;
        int y = ry < 0 ? 0 : top + ry * step;
        if (pos >= ni || y + 2 >= H) break;
        last_y = y;
        if (pos >= 0 && it[pos].kind == 1) {     /* плитка папки */
            const GuildFolder *f = &fd[it[pos].fk];
            u32 fc = f->has_color && f->color ? f->color : 0x5865f2u;
            bool open = folder_open(f->id ? f->id : -(i64)(it[pos].fk + 1)), fsel = pos == selpos;
            int ment = 0;
            bool unr = false;
            for (int q = 0; q < f->n; q++) { int mm = 0; Guild *gg = guild_find(f->gids[q]); if (gg && guild_unread(gg->id, &mm)) unr = true; ment += mm; }
            g_rect(1, y, 6, 3, mix(fc, C_RAIL, open ? 170 : 120));
            if (open) g_ctext(1, y + 1, 6, "v", C_WHITE, AT_B);
            else {
                char t2[8];
                snprintf(t2, sizeof t2, "%d", f->n);
                for (int r = 0; r < 3; r++) { g_ch(1, y + r, '[', fc, AT_B); g_ch(6, y + r, ']', fc, AT_B); }
                g_ctext(2, y + 1, 4, t2, C_WHITE, AT_B);
                if (fsel) g_rect(0, y, 1, 3, C_WHITE);
                else if (unr) g_rect(0, y + 1, 1, 1, C_WHITE);
                if (ment) {
                    char mb[8];
                    snprintf(mb, sizeof mb, ment > 99 ? "99" : "%d", ment);
                    int bw = vw(mb);
                    g_rect(RAIL_W - 1 - bw, y + 2, bw, 1, C_RED);
                    g_text(RAIL_W - 1 - bw, y + 2, mb, bw, C_WHITE, AT_B);
                }
            }
            hit_add(1, y, 6, 3, A_FOLDER, 0, it[pos].fk, HV_ICON);
            if (open) for (int r = 3; r < step && y + r < H; r++) g_rect(1, y + r, 6, 1, mix(fc, C_RAIL, 200));
            continue;
        }
        int idx = pos < 0 ? 0 : it[pos].idx;
        if (pos >= 0 && it[pos].fk >= 0) {       /* сервер в развёрнутой папке — на подложке её цвета */
            const GuildFolder *f = &fd[it[pos].fk];
            u32 fc = f->has_color && f->color ? f->color : 0x5865f2u;
            bool lastf = pos + 1 >= ni || it[pos + 1].fk != it[pos].fk;
            for (int r = 0; r < (lastf ? 3 : step) && y + r < H; r++) g_rect(1, y + r, 6, 1, mix(fc, C_RAIL, 200));
        }
        char lab[32];
        bool unread;
        int ment = 0;
        Guild *g = idx ? &S.guilds[idx - 1] : NULL;
        if (!g) {
            for (int i = 0; i < S.nchans; i++) if (!S.chans[i].guild_id) ment += S.chans[i].mentions;
            unread = ment > 0;
        } else unread = guild_unread(g->id, &ment);
        bool sel = idx == U.sel_guild;
        u32 tc = sel ? C_WHITE : C_TEXT;
        ImgCell ic[18];
        if (!g) {
            /* «домой» (личные сообщения): кошачья мордочка из ASCII, как на эталоне */
            static const char *const home[3] = { "/\\_/\\ ", "(=.=) ", " >^<  " };
            if (sel) g_rect(1, y, 6, 3, C_SEL);
            for (int r = 0; r < 3; r++) g_text(1, y + r, home[r], 6, sel ? C_WHITE : C_TEXT, AT_B);
        } else if (g->icon[0]) {
            bool pic = pic_icon(g, 6, 3, ic);
            for (int r = 0; r < 3; r++) {
                if (pic) { g_imgrow(1, y + r, 6, r, ic); strip_part(G_RAIL, 1, y + r, 6, 3, r, PK_ICON, g->id); }
                else g_rect(1, y + r, 6, 1, C_PH);   /* качается: серый квадрат без букв */
            }
        } else {
            abbr(g->name, lab, sizeof lab);
            g_rect(1, y, 6, 3, sel ? C_SEL : C_BTN2);
            for (int r = 0; r < 3; r++) { g_ch(1, y + r, '[', C_MUTED, AT_B); g_ch(6, y + r, ']', C_MUTED, AT_B); }
            g_ctext(2, y + 1, 4, lab, tc, AT_B);
        }
        if (sel && U.focus == F_GUILDS) g_fgset(1, y + 2, 6, C_WHITE, AT_B | AT_U);
        /* «пилюля» слева: выбран — во всю высоту значка, непрочитанное — маленькая */
        if (sel) g_rect(0, y, 1, 3, C_WHITE);
        else if (unread) g_rect(0, y + 1, 1, 1, C_WHITE);
        if (ment) {
            char mb[8];
            snprintf(mb, sizeof mb, ment > 99 ? "99" : "%d", ment);
            int bw = vw(mb);
            g_rect(RAIL_W - 1 - bw, y + 2, bw, 1, C_RED);
            g_text(RAIL_W - 1 - bw, y + 2, mb, bw, C_WHITE, AT_B);
        }
        hit_add(1, y, 6, 3, A_GUILD, 0, idx, HV_ICON);
        if (!g) U.hit[U.nhit - 1].tip = T_HOME;
    }
    free(it);
    /* «+» добавить сервер: серый квадрат с зелёным плюсом */
    int py = last_y + step;
    if (py + 2 < H) {
        g_rect(1, py, 6, 3, C_BTN2);
        for (int r = 0; r < 3; r++) { g_ch(1, py + r, '[', C_MUTED, AT_B); g_ch(6, py + r, ']', C_MUTED, AT_B); }
        g_ctext(2, py + 1, 4, "+", C_GREEN, AT_B);
        hit_add(1, py, 6, 3, A_BTN, B_ADDSRV, 0, HV_ICON);
        U.hit[U.nhit - 1].tip = T_ADDSRV;
    }
}

/* ---- колонка каналов: справа тонкая линия; шапка сервера с линией снизу, плашка бустов, каналы,
 * внизу в рамках панель «Голосовая связь подключена» и панель пользователя (эталон владельца) ---- */
static int draw_sidebar(int sx, int sw0, int H, const VoiceStatus *vs) {
    g_rect(sx, 0, sw0, H, C_SIDE);
    for (int y = 0; y < H; y++) g_ch(sx + sw0 - 1, y, '|', C_LINE, 0);
    int sw = sw0 - 1;                            /* без линии справа */
    Guild *cg = cur_guild_id() ? guild_find(cur_guild_id()) : NULL;
    int list_top = 1;
    if (cg) {
        char t[160];
        snprintf(t, sizeof t, "%s v", cg->name);
        if (cg->banner_color) g_rect(sx, 0, sw, 1, mix(cg->banner_color & 0xFFFFFFu, C_SIDE, 110));   /* №4: баннер — цветом */
        g_texte(sx + 2, 0, t, sw - 6, C_WHITE, AT_B | (U.focus == F_CHANNELS ? AT_U : 0));
        hit_add(sx + 1, 0, sw - 6, 1, A_BTN, B_GMENU, 0, HV_UL);   /* «Имя v» — меню сервера */
        g_icon_btn(sx + sw - 3, 0, "o+", C_TEXT, B_GINVITE, T_INVITE);
        g_uline(sx, 0, sw);
        if (H >= 20) {
            /* плашка «Нужно бустов   Бусты 0/33 >» */
            g_pill(sx + 1, 1, sw - 2, C_BTN2);
            g_text(sx + 2, 1, TR("Нужно бустов"), sw - 4, C_WHITE, AT_B);
            if (sw >= 30) g_rtext(sx + sw - 2, 1, TR("Бусты 0/33 >"), 12, C_MUTED, 0);
            else if (sw >= 22) g_rtext(sx + sw - 2, 1, "0/33 >", 6, C_MUTED, 0);
            hit_add(sx + 1, 1, sw - 2, 1, A_BTN, B_NOP, 0, HV_PILL); U.hit[U.nhit - 1].tip = T_BOOST;
            g_text(sx + 2, 2, "[=]", 3, C_MUTED, 0);
            g_text(sx + 6, 2, TR("Мероприятия"), sw - 7, C_MUTED, 0);
            hit_add(sx + 1, 2, sw - 2, 1, A_BTN, B_NOP, 0, HV_PILL); U.hit[U.nhit - 1].tip = T_EVENTS;
            g_text(sx + 2, 3, "<>", 2, C_MUTED, 0);
            g_text(sx + 6, 3, TR("Бусты сервера"), sw - 7, C_MUTED, 0);
            hit_add(sx + 1, 3, sw - 2, 1, A_BTN, B_NOP, 0, HV_PILL); U.hit[U.nhit - 1].tip = T_BOOST;
            list_top = 4;
        }
    } else {
        g_pill(sx + 1, 0, sw - 2, C_RAIL);
        g_texte(sx + 3, 0, TR("Найти или начать беседу"), sw - 5, C_MUTED, U.focus == F_CHANNELS ? AT_U : 0);
        hit_add(sx + 1, 0, sw - 2, 1, A_BTN, B_ADDDM, 0, HV_UL); U.hit[U.nhit - 1].tip = T_FIND;
        if (H >= 16) {
            bool home = !S.open_ch;
            if (home) g_pill(sx + 1, 1, sw - 2, C_SEL);
            g_text(sx + 2, 1, "oo", 2, home ? C_WHITE : C_MUTED, AT_B);
            g_text(sx + 6, 1, TR("Друзья"), sw - 7, home ? C_WHITE : C_MUTED, AT_B);
            hit_add(sx + 1, 1, sw - 2, 1, A_BTN, B_FRIENDS, 0, HV_PILL);
            g_text(sx + 2, 2, "<>", 2, C_MUTED, AT_B);
            g_text(sx + 6, 2, "Nitro", sw - 7, C_MUTED, 0);
            hit_add(sx + 1, 2, sw - 2, 1, A_BTN, B_NOP, 0, HV_PILL); U.hit[U.nhit - 1].tip = T_NITRO;
            {
                int rqi[64];
                int nrq = rq_list(0, rqi, 64) + rq_list(1, rqi, 64);
                if (nrq) {                       /* №109: «Запросы на общение» со счётчиком */
                    bool rh = !S.open_ch && RQ.on;
                    if (rh) g_pill(sx + 1, 3, sw - 2, C_SEL);
                    g_text(sx + 2, 3, "?!", 2, rh ? C_WHITE : C_MUTED, AT_B);
                    char rb[12];
                    snprintf(rb, sizeof rb, "%d", nrq);
                    int mw = vw(rb) + 2;
                    g_texte(sx + 6, 3, TR("Запросы"), sw - 9 - mw, rh ? C_WHITE : C_MUTED, AT_B);
                    g_pill(sx + sw - 2 - mw, 3, mw, C_RED);
                    g_text(sx + sw - 1 - mw, 3, rb, mw - 2, C_WHITE, AT_B);
                    hit_add(sx + 1, 3, sw - 2, 1, A_BTN, B_REQS, 0, HV_PILL);
                }
            }
            g_text(sx + 2, 4, TR("Личные сообщения"), sw - 6, C_MUTED, 0);
            g_icon_btn(sx + sw - 3, 4, "+", U.pick ? C_WHITE : C_MUTED, B_ADDDM, T_ADDDM);
            list_top = 5;
        }
    }
    /* низ: [линия] панель голоса 3 строки [линия] панель пользователя 2 строки */
    bool vpanel = vs->phase != VS_IDLE;
    int user_top = H - 2, voice_top = vpanel ? H - 6 : user_top;
    int list_bot = voice_top - 2;
#ifdef ASTRA_VIDEO
    int pv_iw, pv_ih, pv_n = cu_pv_rows(sw, H, vpanel, &pv_iw, &pv_ih);   /* предпросмотр своей камеры над панелью голоса */
    if (pv_n) list_bot -= pv_n + 1;
#endif
    int ch_h = list_bot - list_top + 1;
    if (!U.row_free && U.sel_row >= 0) {
        if (U.sel_row < U.row_scroll) U.row_scroll = U.sel_row;
        if (U.sel_row >= U.row_scroll + ch_h) U.row_scroll = U.sel_row - ch_h + 1;
    }
    if (U.row_scroll > U.nrows - ch_h) U.row_scroll = U.nrows - ch_h;
    if (U.row_scroll < 0) U.row_scroll = 0;
    hit_add(sx, list_top, sw, ch_h > 0 ? ch_h : 0, A_AR_CHANS, 0, 0, 0);
    if (!U.nrows && ch_h > 0) g_text(sx + 2, list_top + 1, cg ? TR("нет видимых каналов") : TR("бесед пока нет"), sw - 3, C_MUTED, 0);
    for (int cy = 0; cy < ch_h; cy++) {
        int ri = U.row_scroll + cy, y = list_top + cy;
        if (ri >= U.nrows) break;
        Row *r = &U.rows[ri];
        if (r->kind == R_GAP) {
            if (ri == 0 && cg && list_top == 4) for (int i = sx + 1; i < sx + sw - 1; i++) g_ch(i, y, '-', C_LINE, 0);
            continue;
        }
        Channel *c = chan_find(r->id);
        bool cur = U.focus == F_CHANNELS && ri == U.sel_row;
        if (r->kind == R_VSTAT) {
            const char *cs = voice_channel_status(r->id);
            bool mine = vs->phase != VS_IDLE && vs->channel_id == r->id;
            if (cs && cs[0]) { char *sc = san(cs, false); g_texte(sx + 6, y, sc, sw - 7, C_MUTED, AT_IT); free(sc); }
            else g_texte(sx + 6, y, TR("Выбрать статус канала /"), sw - 7, C_MUTED, 0);   /* сокращается по словам */
            if (mine) hit_add(sx + 5, y, sw - 6, 1, A_BTN, B_VSTATUS, 0, HV_UL);
            continue;
        }
        if (r->kind == R_HEADER) {
            /* категория: мелко серым, как в названии; «v»/«>»; «+» справа */
            char t[160];
            snprintf(t, sizeof t, "%s %s", c && c->name ? c->name : "", folded(r->id) ? ">" : "v");
            g_texte(sx + 1, y, t, sw - 4, cur ? C_WHITE : C_MUTED, cur ? AT_U : 0);
            hit_add(sx, y, sw - 3, 1, A_ROW, 0, ri, HV_UL);
            g_icon_btn(sx + sw - 2, y, "+", C_MUTED, B_PLUS, T_NONE);
            U.hit[U.nhit - 1].arg = ri;
            continue;
        }
        if (r->kind == R_MEMBER) {
            bool speaking = false;
            for (int k = 0; k < vs->nspeaking; k++) if (vs->speaking[k] == r->user) speaking = true;
            const VoiceState *v = NULL, *list[32];
            int k = voice_members(r->id, list, 32);
            for (int q = 0; q < k; q++) if (list[q]->user_id == r->user) v = list[q];
            const char *nm = v && v->name ? v->name : user_name(r->user);
            float vol = voice_get_volume(r->user);
            char tail[48] = "";
            if (vol < 0.99f || vol > 1.01f) snprintf(tail, sizeof tail, "%d%%", (int)(vol * 100 + 0.5f));
            const char *mute = v && (v->self_deaf || v->deaf) ? "/x\\" : v && (v->self_mute || v->mute) ? "(/)" : NULL;
            if (cur) g_pill(sx + 1, y, sw - 2, C_HOV);
            g_listav(sx + 5, y, r->user, nm, G_SIDE);
            if (speaking) { g_fgset(sx + 5, y, 1, C_GREEN, AT_B); g_fgset(sx + 8, y, 1, C_GREEN, AT_B); g_bg(sx + 5, y, 1, C_GREEN); g_bg(sx + 8, y, 1, C_GREEN); }
            int right = sx + sw - 2;
            if (mute) right = g_rtext(right, y, mute, 3, C_RED, AT_B) - 1;
            if (tail[0]) right = g_rtext(right, y, tail, 5, C_MUTED, 0) - 1;
#ifdef ASTRA_VIDEO
            int live_x = 0;
            if (v && v->stream) { live_x = g_rtext(right, y, "LIVE", 4, C_WHITE, AT_B); g_bg(live_x, y, 4, C_RED); right = live_x - 1; }
            int cam_x = v && cu_user(v) ? g_rtext(right, y, "[]/", 3, C_WHITE, AT_B) : 0;   /* камера включена */
            if (cam_x) right = cam_x - 1;
#endif
            g_texte(sx + 10, y, nm, right - sx - 10, speaking ? C_WHITE : C_TEXT, speaking ? AT_B : 0);
            hit_add(sx + 1, y, sw - 2, 1, A_ROW, 0, ri, HV_PILL);
#ifdef ASTRA_VIDEO
            if (live_x && r->user != S.me_id) hit_add(live_x, y, 4, 1, A_BTN, B_WATCH, ri, HV_RED);   /* щелчок по LIVE — смотреть */
            if (cam_x) { hit_add(cam_x, y, 3, 1, A_BTN, B_CAM_VIEW, ri, HV_ACC); U.hit[U.nhit - 1].tip = T_CAMVIEW; }   /* по []/ — камеру крупно */
#endif
            continue;
        }
        if (!c) continue;
        char lab[96];
        chan_label(c, lab, sizeof lab);
        bool here = vs->phase != VS_IDLE && vs->channel_id == c->id, unread = chan_unread(c);
        bool open = c->id == S.open_ch;
        if (open) g_pill(sx + 1, y, sw - 2, C_SEL);
        else if (cur) g_pill(sx + 1, y, sw - 2, C_HOV);
        bool muted = c->muted;
        if (!open && !muted && (unread || c->mentions)) g_rect(sx, y, 1, 1, C_WHITE);
        u32 nfg = open || here ? C_WHITE : muted ? 0x5c6066u : unread || c->mentions ? C_WHITE : C_MUTED;
        int right = sx + sw - 3;
        if (c->mentions) {
            char mb[12];
            snprintf(mb, sizeof mb, "%d", c->mentions);
            int mw = vw(mb) + 2;
            g_pill(right - mw + 1, y, mw, C_RED);
            g_text(right - mw + 2, y, mb, mw - 2, C_WHITE, AT_B);
            right -= mw + 1;
        }
        int nx = sx + 5;
        if (is_thread(c)) { g_text(sx + 3, y, "'-", 2, C_LINE, 0); nx = sx + 6; if (!open && !unread && !c->mentions) nfg = C_MUTED; }
        else if (!c->guild_id && c->type == CH_DM && c->nrecip > 0) { g_listav(sx + 2, y, c->recipients[0], lab, G_SIDE); nx = sx + 7; }
        else if (is_voice(c) && !c->can_connect) g_text(sx + 2, y, "<!", 2, 0x5c6066u, AT_B);   /* замок: нет права входа */
        else g_text(sx + 2, y, chan_mark(c), 2, here ? C_GREEN : C_MUTED, AT_B);
        g_texte(nx, y, lab, right - nx + 1, nfg, unread || c->mentions || open || here ? AT_B : 0);
        hit_add(sx + 1, y, sw - 2, 1, A_ROW, 0, ri, HV_PILL);
    }

#ifdef ASTRA_VIDEO
    if (pv_n) {
        g_rect(sx, list_bot + 1, sw, pv_n + 1, C_PANEL);
        g_uline(sx, list_bot + 1, sw);
        cu_pv_draw(sx, sw, list_bot + 2, pv_iw, pv_ih);
    }
#endif
    /* ---- панель «Голосовая связь подключена» в рамке ---- */
    g_rect(sx, voice_top - 1, sw, H - voice_top + 1, C_PANEL);
    if (vpanel) {
        g_uline(sx, voice_top - 1, sw);
        u32 pfg = vs->phase == VS_CONNECTED ? C_GREEN : vs->phase == VS_FAILED ? C_RED : C_GOLD;
        int y = voice_top;
        g_icon_btn(sx + 1, y, ".:|", pfg, B_NOP, T_E2EE);
        int bx = sx + sw - 4;
        g_icon_btn(bx, y, "\\_/", C_TEXT, B_LEAVE, T_LEAVE);
        U.hit[U.nhit - 1].hv = HV_RED;
        int tx_end = bx - 1;
        if (sw >= 32) { g_icon_btn(bx - 4, y, "((o", C_GOLD, B_NOP, T_NOISE); tx_end = bx - 5; }
        int room = tx_end - sx - 5;
        const char *ph = vs->phase == VS_CONNECTED ? (room >= 26 ? TR("Голосовая связь подключена") : room >= 22 ? TR("Голосовая связь подкл.") : TR("Голос подключён")) :
                         vs->phase == VS_FAILED ? TR("Ошибка голоса") : vs->phase == VS_DAVE ? TR("Шифрование...") : TR("Подключение...");
        g_texte(sx + 5, y, ph, tx_end - sx - 5, pfg, 0);
        Channel *vc = chan_find(vs->channel_id);
        Guild *vg2 = vc && vc->guild_id ? guild_find(vc->guild_id) : NULL;
        char lab[96] = "?", t[200], enc[48] = "";
        if (vc) chan_label(vc, lab, sizeof lab);
        if (vs->error[0]) deco_ascii(vs->error, t, sizeof t);
        else snprintf(t, sizeof t, "%s%s%s", lab, vg2 ? " / " : "", vg2 ? vg2->name : "");
        if (vs->phase == VS_CONNECTED) snprintf(enc, sizeof enc, TR("%s %d мс"), O.demo ? TR("петля") : vs->dave ? "E2EE" : TR("без E2EE"), vs->ping);
        int ex = sw >= 30 ? g_rtext(sx + sw - 1, y + 1, enc, 14, vs->dave || O.demo ? C_GREEN : C_GOLD, 0) : sx + sw;
        int tw2 = g_texte_w(sx + 1, y + 1, t, ex - sx - 2, vs->error[0] ? C_RED : C_MUTED, 0, false);   /* по символам: «Канал / Серв...» */
        if (vc) hit_add(sx + 1, y + 1, tw2, 1, A_BTN, B_VOPEN, 0, HV_UL);
        /* ряд из 4 кнопок в рамках: камера, экран, смена голоса, звуковая панель */
        static const char *const ic[4] = { "[]/", "[^]", "~v~", "<))" };
#ifdef ASTRA_VIDEO
        static const int ids[4] = { B_CAM, B_SHARE, B_FX, B_NOP }, tips[4] = { T_CAM, T_SCREEN, T_FX, T_SOUNDB };
#else
        static const int ids[4] = { B_NOP, B_NOP, B_FX, B_NOP }, tips[4] = { T_CAM, T_SCREEN, T_FX, T_SOUNDB };
#endif
        int bw = (sw - 2 - 3) / 4;
        VoiceFx fx = voicefx_get();
        for (int i = 0; i < 4; i++) {
            int x = sx + 1 + i * (bw + 1);
            bool on = ids[i] == B_FX && fx != VFX_OFF;
#ifdef ASTRA_VIDEO
            if (ids[i] == B_SHARE && sh_running) on = true;
            if (ids[i] == B_CAM && cu_on()) on = true;
#endif
            g_pill(x, y + 2, bw, on ? C_ACC : C_BTN2);
            g_ctext(x, y + 2, bw, ic[i], C_TEXT, AT_B);
            hit_add(x, y + 2, bw, 1, A_BTN, ids[i], 0, HV_PILL);
            U.hit[U.nhit - 1].tip = (u8)tips[i];
        }
    }
    g_uline(sx, user_top - 1, sw);
    /* ---- панель пользователя: аватар 4x2 со статусом, имя, статус, мик / наушники / шестерёнка ---- */
    const char *me = S.me_name && *S.me_name ? S.me_name : TR("вы");
    for (int r = 0; r < 2; r++) { g_avatar(sx + 1, user_top + r, 4, 2, r, S.me_id, me); strip_part(G_PANEL, sx + 1, user_top + r, 4, 2, r, PK_AVR, S.me_id); }
    char gst[128];
    deco_ascii(gw_status_text(), gst, sizeof gst);
    bool online = O.demo || (S.ready && (!gst[0] || !strcmp(gst, "в сети")));
    /* свой статус (выбор — щелчком по аватару/имени): цвет точки и подпись */
    u8 me_st = presence_me();
    bool inv = me_st == PS_INVISIBLE;
    static const char *const stn[] = { "Не в сети", "В сети", "Не активен", "Не беспокоить", "Невидимый" };
    u32 stc = !online ? (S.ready ? C_GOLD : 0x80848eu) : inv || me_st == PS_OFFLINE ? 0x80848eu : status_col(me_st);
    g_cap(fine_on ? sx + 5 : sx + 4, user_top + 1, 'o', stc, C_PANEL);   /* в окне аватар под слоем: точка рядом */
    if (vs->mic_live) g_cap(sx, user_top, '~', C_GREEN, C_PANEL);
    char st[140];
    snprintf(st, sizeof st, "(%s)", online ? tr(stn[me_st <= PS_INVISIBLE ? me_st : 1]) : gst);
    if (custom_status()[0]) {                    /* №104: свой текстовый статус */
        char *cs2 = san(custom_status(), false);
        snprintf(st, sizeof st, "%s", cs2);
        free(cs2);
    } else if (presence_until() > wall_ms() / 1000) {   /* №103: статус до времени */
        struct tm pt;
        plat_localtime(presence_until(), &pt);
        snprintf(st + strlen(st), sizeof st - strlen(st), TR(" до %02d:%02d"), pt.tm_hour, pt.tm_min);
    }
    if (focus_until() > wall_ms() / 1000) {      /* №121: значок фокуса и до какого времени */
        struct tm ft;
        plat_localtime(focus_until(), &ft);
        snprintf(st + strlen(st), sizeof st - strlen(st), TR(" (z)фокус до %02d:%02d"), ft.tm_hour, ft.tm_min);
    }
    hit_add(sx + 1, user_top, 5, 2, A_BTN, B_STATUS, 0, HV_UL);
    U.hit[U.nhit - 1].tip = T_STATUS;
    /* пиктограммы: микрофон (|), наушники /^\, шестерёнка (*); выключенное — красным.
     * Стоят в строке имени, статус — под именем целиком; в узкой колонке (имя не влезло бы) — наоборот. */
    int iy = sw - 7 - 11 >= 6 ? user_top : user_top + 1;
    int x = sx + sw;
    x -= 3; g_icon_btn(x, iy, "(*)", C_TEXT, B_SETTINGS, T_SET);
    x -= 4; g_icon_btn(x, iy, vs->self_deaf ? "/x\\" : "/^\\", vs->self_deaf ? C_RED : C_TEXT, B_DEAF, T_DEAF);
    x -= 4; g_icon_btn(x, iy, vs->self_mute ? "(/)" : "(|)", vs->self_mute ? C_RED : C_TEXT, B_MIC, T_MIC);
    g_texte(sx + 6, user_top, me, (iy == user_top ? x : sx + sw) - sx - 7, C_WHITE, AT_B);
    g_texte(sx + 6, user_top + 1, st, (iy == user_top ? sx + sw : x) - sx - 7, C_MUTED, 0);
    return list_top;
}

/* Шапка колонки: подпись слева, тонкая линия по низу строки. */
#ifdef ASTRA_VIDEO
/* камера: включение/выключение (cam.h), окно поверх — растр плиток прячется */
static bool cu_covered(void) {
    return U.menu || U.pick || U.card_uid || U.ep || U.qs || FM.id || IV.on || AVB.uid || tm_on || rules_on || upd_on || CSEL.on;
}
static void cu_devices(void) {
    if (O.demo) {                                /* демо: настоящие камеры не трогаем */
        static const char *const dn[2] = { "Встроенная камера (демо)", "USB-камера (демо)" };
        cu_ndev = 2;
        for (int i = 0; i < 2; i++) { snprintf(cu_dev[i].id, sizeof cu_dev[i].id, "demo%d", i); snprintf(cu_dev[i].name, sizeof cu_dev[i].name, "%s", tr(dn[i])); }
    } else cu_ndev = cam_list(cu_dev, 8);
    if (cu_ndev < 0) cu_ndev = 0;
    for (int i = 0; i < cu_ndev; i++) cu_names[i] = cu_dev[i].name;
}
static void cu_start(void) {
    if (O.demo) { cu_demo = true; ui_notice("%s", TR("камера включена (демо)")); U.dirty = true; return; }
    char err[160] = "";
    if (!cam_start(err, sizeof err)) ui_notice("%s", err[0] ? err : TR("камера не включилась"));
    U.dirty = true;
}
/* Alt+E и щелчок по пометке под предпросмотром: скрывать глаза вкл/выкл (действует сразу, и на идущую камеру) */
static void cu_anon_toggle(void) {
#ifndef ASTRA_ANON
    ui_notice("%s", TR("анонимизация: нет в этой сборке"));
    return;
#endif
    anon_set_default(!anon_default());           /* всем открытым контекстам — с ближайшего кадра */
    ui_notice("%s", anon_default() ? TR("анонимизация: глаза скрыты (Alt+E - выключить)") : TR("анонимизация выключена: глаза видны (Alt+E - включить)"));
    U.dirty = true;
}
/* кнопка []/ (ask — с окном выбора камеры) и F12 / Alt+C (сразу, выбранной камерой) */
static void cu_toggle(bool ask) {
    U.dirty = true;
    if (cu_on()) { if (O.demo) cu_demo = false; else cam_stop(); ui_notice("%s", TR("камера выключена")); return; }
    VoiceStatus v;
    voice_status(&v);
    if (v.phase == VS_IDLE) { ui_notice("%s", TR("камера: сначала подключитесь к голосовому каналу или звонку")); return; }
    cu_devices();
    if (!cu_ndev) { ui_notice("%s", TR("камера не найдена: подключите камеру и попробуйте снова")); return; }
    if (ask) fm_open(FM_CAM, 0, 0);
    else cu_start();
}
#endif
#ifdef ASTRA_VIDEO
/* ---- просмотр чужой трансляции: панель видео вместо центра окна (или на всё окно) ----
 *  [>] Имя LIVE                              [Чат] [Во весь экран] [Выйти]
 *  Трансляции в канале: [Вася] [Петя]                       Alt+<- Alt+->   (если смотреть есть кого ещё)
 *  +--------------------------------------------------------------------+
 *  |              растр (g_video_area) или текст состояния             |
 *  строка состояния
 * Растр рисует окно (gui*, кодер): g_video_area — прямоугольник в клетках на этот кадр; не вызвана — скрыто.
 * Объявления — golive.h, реализация — gui_grid.c. */
/* что можно смотреть в моём голосовом канале: трансляции (kind 0), затем камеры (kind 1); своё — только камера */
static int wv_sources(u64 *out, u8 *kind, int max) {
    VoiceStatus v;
    voice_status(&v);
    if (v.phase == VS_IDLE) return 0;
    const VoiceState *l[32];
    int k = voice_members(v.channel_id, l, 32), n = 0;
    for (int i = 0; i < k && n < max; i++) if (l[i]->stream && l[i]->user_id != S.me_id) { out[n] = l[i]->user_id; kind[n++] = 0; }
    for (int i = 0; i < k && n < max; i++) if (cu_user(l[i])) { out[n] = l[i]->user_id; kind[n++] = 1; }
    return n;
}
static bool wv_on(void) { return WS.phase != GW_OFF || WV.cam; }   /* панель просмотра открыта (трансляция или камера) */
static void wv_watch(u64 uid) {                 /* смотреть uid; golive_watch сам закрывает текущий просмотр */
    U.full = U.dirty = true;
    WV.chat = false; WV.ch = S.open_ch; WV.gd = U.sel_guild;
    WV.cam = 0;                                  /* панель одна: трансляция вместо камеры */
    if (WS.phase != GW_OFF && WS.phase != GW_FAILED && WS.user == uid) return;   /* уже смотрим — только показать */
    VoiceStatus v;
    voice_status(&v);
    if (v.phase == VS_IDLE) { ui_notice("%s", TR("чтобы смотреть трансляцию, сначала войдите в этот голосовой канал")); return; }
    if (O.demo) { demo_watch = uid; return; }
    Channel *wc = chan_find(v.channel_id);
    char err[128] = "";
    if (!golive_watch(wc ? wc->guild_id : 0, v.channel_id, uid, err, sizeof err)) ui_notice("%s", err);
}
static void wv_stop(void) { golive_unwatch(); demo_watch = 0; WS.phase = GW_OFF; WV.cam = 0; WV.full = WV.chat = false; U.full = U.dirty = true; }
/* крупно камеру uid в той же панели (трансляция, если смотрели, закрывается); кадр — полный, не плиточный */
static void cv_open(u64 uid) {
    VoiceStatus v;
    voice_status(&v);
    if (v.phase == VS_IDLE) { ui_notice("%s", TR("чтобы смотреть камеру, сначала войдите в этот голосовой канал")); return; }
    if (WS.phase != GW_OFF) { golive_unwatch(); demo_watch = 0; WS.phase = GW_OFF; }
    WV.cam = uid;
    WV.chat = false; WV.ch = S.open_ch; WV.gd = U.sel_guild;
    U.full = U.dirty = true;
}
static void wv_pick(int i) { if (i >= 0 && i < WV.n) { if (WV.sk[i]) cv_open(WV.su[i]); else wv_watch(WV.su[i]); } }
static void wv_step(int d) {                    /* Alt+стрелки: соседняя трансляция или камера в канале */
    int n = wv_sources(WV.su, WV.sk, 8), cur = -1;
    WV.n = n;
    for (int i = 0; i < n; i++) if (WV.cam ? WV.sk[i] && WV.su[i] == WV.cam : !WV.sk[i] && WV.su[i] == WS.user) cur = i;
    if (n > 0) wv_pick(cur < 0 ? 0 : (cur + d + n) % n);
}
static bool wv_key(const Key *k) {
    bool alt = k->alt && !k->ctrl;
    if (k->code == K_ESC && WV.full && !WV.chat && !U.menu && !U.card_uid && !U.ep && !U.qs && !FM.id) { WV.full = false; U.full = U.dirty = true; return true; }
    if (alt && (k->code == K_LEFT || k->code == K_RIGHT)) { wv_step(k->code == K_LEFT ? -1 : 1); return true; }
    if (alt && k->code == K_CHAR) {
        u32 c = k->ch;
        if (c == 'w' || c == 'W' || c == 0x446 || c == 0x426) { WV.chat = !WV.chat; WV.ch = S.open_ch; WV.gd = U.sel_guild; U.full = U.dirty = true; return true; }   /* ц */
        if ((c == 'f' || c == 'F' || c == 0x430 || c == 0x410) && !WV.chat) { WV.full = !WV.full; U.full = U.dirty = true; return true; }   /* а */
    }
    return false;
}
static void wv_rbtn(int *right, const char *t, int style, int id, int tip) {   /* кнопка справа налево */
    int w = vw(t) + 4;
    *right -= w;
    if (g_button(*right, 0, t, w, style, id)) U.hit[U.nhit - 1].tip = (u8)tip;
    *right -= 1;
}
static void draw_watch(int x, int w, int H) {
    g_rect(x, 0, w, H, C_FEED);
    WV.n = wv_sources(WV.su, WV.sk, 8);
    bool narrow = w < 72;
    u64 who = WV.cam ? WV.cam : WS.user;
    int right = x + w - 1;
    wv_rbtn(&right, narrow ? "x" : TR("Выйти"), 2, B_WATCH_STOP, T_WSTOP);
    wv_rbtn(&right, WV.full ? (narrow ? "][" : TR("Обычный вид")) : (narrow ? "[ ]" : TR("Во весь экран")), 0, B_WATCH_FULL, T_WFULL);
    wv_rbtn(&right, narrow ? "#" : TR("Чат"), 0, B_WATCH_CHAT, T_WCHAT);
    char *nm = san(wv_uname(who), true);
    g_text(x + 1, 0, "[>]", 3, C_MUTED, AT_B);
    int px = x + 5;
    px += g_texte(px, 0, nm, right - px - 5, C_WHITE, AT_B) + 1;
    free(nm);
    if (WV.cam && px + 4 < right) {
        g_text(px, 0, "[]/", 3, C_WHITE, AT_B); px += 4;
        u32 ac = C_MUTED;
        const char *cl = WV.cam == S.me_id ? cu_anon_label(&ac) : TR("камера");
        if (right - px > 8) g_texte(px, 0, cl, right - px - 1, ac, 0);
    }
    else if (WS.phase == GW_LIVE && px + 4 < right) { g_text(px, 0, "LIVE", 4, C_WHITE, AT_B); g_bg(px, 0, 4, C_RED); px += 5; }
    const char *ph = WV.cam ? NULL : WS.phase == GW_WAIT ? TR("ожидание сервера") : WS.phase == GW_CONNECTING ? TR("подключение")
                   : WS.phase == GW_FAILED ? TR("ошибка") : NULL;
    if (ph && right - px > 4) g_texte(px, 0, ph, right - px - 1, WS.phase == GW_FAILED ? C_RED : C_MUTED, 0);
    g_uline(x, 0, w);
    /* список трансляций и камер канала — только если есть на кого переключиться */
    int y = 1;
    bool other = false, cams = false;
    for (int i = 0; i < WV.n; i++) { if (WV.cam ? !(WV.sk[i] && WV.su[i] == WV.cam) : WV.sk[i] || WV.su[i] != WS.user) other = true; if (WV.sk[i]) cams = true; }
    if (other) {
        int lx = x + 1, lr = x + w - 1;
        const char *hint = "Alt+<- Alt+->";
        bool hs = !narrow;
        if (hs) lr -= vw(hint) + 2;
        lx += g_text(lx, y, narrow ? (cams ? TR("Смотреть:") : "LIVE:") : cams ? TR("Трансляции и камеры:") : TR("Трансляции в канале:"), lr - lx, C_MUTED, AT_B) + 1;
        for (int i = 0; i < WV.n; i++) {
            char *sn = san(wv_uname(WV.su[i]), true);
            bool cur = WV.sk[i] ? WV.cam == WV.su[i] : !WV.cam && WV.su[i] == WS.user;
            int pre = WV.sk[i] ? 4 : 0;              /* камера — со значком []/ перед именем */
            int tw = vw(sn) > 16 ? 16 : vw(sn), bw = tw + 4 + pre;
            if (lx + bw > lr) { if (lr - lx >= 3) g_text(lx, y, "...", 3, C_MUTED, 0); free(sn); break; }
            g_pill(lx, y, bw, cur ? C_ACC : C_BTN2);
            if (pre) g_text(lx + 2, y, "[]/", 3, cur ? C_WHITE : C_TEXT, AT_B);
            g_texte(lx + 2 + pre, y, sn, tw, cur ? C_WHITE : C_TEXT, cur ? AT_B : 0);
            if (!cur) { hit_add(lx, y, bw, 1, A_BTN, B_WATCH_PICK, i, HV_PILL); U.hit[U.nhit - 1].tip = T_WPICK; }
            lx += bw + 1;
            free(sn);
        }
        if (hs) g_rtext(x + w - 2, y, hint, vw(hint), C_MUTED, 0);
        y++;
    }
    /* область видео: поля в 1 клетку, снизу пустая строка и строка состояния */
    int vx = x + 1, vy = y + 1, vwd = w - 2, vh = H - 2 - vy;
    if (vh < 1) vh = 1;
    g_rect(vx, vy, vwd, vh, 0x000000u);
    int cy = vy + vh / 2;
    if (WV.cam) cu_frame(vx, vy, vwd, vh, WV.cam);   /* камера крупно: окно отдаёт полный кадр под размер области */
    else if (WS.phase == GW_FAILED) {
        char *e = san(WS.err, true);
        g_ctext(vx, cy - 1, vwd, TR("Не удалось подключиться к трансляции"), C_RED, AT_B);
        if (*e) g_ctext(vx, cy, vwd, e, C_TEXT, 0);
        g_ctext(vx, cy + 2, vwd, TR("Щёлкните LIVE у участника, чтобы попробовать снова"), C_MUTED, 0);
        free(e);
    } else if (WS.phase != GW_LIVE) g_ctext(vx, cy, vwd, WS.phase == GW_WAIT ? TR("Ждём ответ сервера трансляции...") : TR("Подключение к трансляции..."), C_MUTED, 0);
    else if (!g_video_supported()) {
        g_ctext(vx, cy - 1, vwd, TR("Видео в терминале не показывается, звук идёт"), C_TEXT, AT_B);
        g_ctext(vx, cy + 1, vwd, TR("Чат - Alt+W, выйти - кнопка «Выйти»"), C_MUTED, 0);
    } else if (WS.v.unsupported) {               /* кодер: растр не заявляем — иначе вместо текста чёрное поле */
        g_ctext(vx, cy - 1, vwd, TR("Формат трансляции не поддерживается"), C_RED, AT_B);
        g_ctext(vx, cy + 1, vwd, TR("Звук трансляции идёт"), C_MUTED, 0);
    } else {
        if (!WS.vpkts) g_ctext(vx, cy, vwd, TR("Ждём первый кадр..."), C_MUTED, 0);
        else if (O.demo) g_ctext(vx, cy, vwd, TR("демо: здесь будет видео трансляции"), C_MUTED, 0);
        /* растр поверх клеток закрыл бы меню, карточки и окна — пока они открыты, видео прячется */
        bool cover = U.menu || U.pick || U.card_uid || U.ep || U.qs || FM.id || IV.on || AVB.uid || tm_on || rules_on || upd_on || CSEL.on;
        if (!cover) g_video_area(vx, vy, vwd, vh);
    }
    draw_status(x, x + w, H - 1);
}
#endif

static void draw_main(int W, int H) {
    VoiceStatus vs;
    voice_status(&vs);
    int sw = PW.side ? PW.side : W / 4 + 3;      /* с линией справа; №11 — своя ширина */
    if (sw < 20) sw = 20;
    if (sw > (PW.side ? W / 2 : 34)) sw = PW.side ? W / 2 : 34;
    int sx = RAIL_W, cx = sx + sw, cw = W - cx;
#ifdef ASTRA_VIDEO
    if (wv_on() && !WV.chat && (S.open_ch != WV.ch || U.sel_guild != WV.gd)) WV.chat = true;   /* ушли в другой канал — к чату */
    {
        cu_areas = 0; cu_anim = false;
        Channel *oc0 = chan_find(S.open_ch);      /* своя плитка будет в центре — предпросмотр в колонке не нужен */
        cu_center = cu_on() && oc0 && is_voice(oc0) && vs.phase != VS_IDLE && vs.channel_id == oc0->id && !(wv_on() && !WV.chat);
        if (WV.cam == S.me_id && !WV.chat && cu_on()) cu_center = true;   /* себя смотрим крупно — в колонке не дублировать */
    }
    if (wv_on() && !WV.chat) {                   /* просмотр трансляции или камеры: панель видео вместо центра */
        if (WV.full || cw < 44) draw_watch(0, W, H);
        else {
            draw_rail(H);
            draw_sidebar(sx, sw, H, &vs);
            hit_add(cx - 1, 1, 1, H - 3, A_SPLIT, 0, 0, HV_ACC);
            draw_watch(cx, cw, H);
        }
        if (U.menu) draw_menu(MENU_LEFT(U.menu_kind) ? sx : cx, 1, MENU_LEFT(U.menu_kind) ? W - sx : cw, H - 1);
        if (U.pick) draw_picker(W, H, sx + sw - 4, 5);
        if (U.card_uid) draw_card(W, H);
        if (U.ep) draw_ep(W, H);
        if (U.qs) draw_qs(W, H);
        return;
    }
#endif
    draw_rail(H);
    draw_sidebar(sx, sw, H, &vs);
    hit_add(cx - 1, 1, 1, H - 3, A_SPLIT, 0, 0, HV_ACC);   /* №11: граница колонки каналов */
    Channel *oc = chan_find(S.open_ch);
    g_rect(cx, 0, cw, H, C_FEED);
    if (!(!oc && U.sel_guild == 0 && S.ready)) U.fr_seen = false;
    if (!oc && U.sel_guild == 0 && S.ready) {
        if (RQ.on) draw_requests(cx, cw, H);
        else draw_friends(cx, cw, H);
        if (U.menu) draw_menu(sx, 1, W - sx, H - 1);
        if (U.pick) draw_picker(W, H, sx + sw - 4, 5);
        if (U.card_uid) draw_card(W, H);
    if (U.ep) draw_ep(W, H);
    if (U.qs) draw_qs(W, H);
        return;
    }

    if (oc && is_voice(oc)) {
        /* экран голосового канала; справа за линией — его текстовый чат (скрывается x) */
        int chw = !U.vchat_off && cw >= 60 ? cw * 40 / 100 : 0;
        if (chw && chw < 33) chw = 33;
        if (chw > 46) chw = 46;
        int vw0 = cw - chw;
        char lab[96];
        chan_label(oc, lab, sizeof lab);
        g_text(cx + 1, 0, "<)", 2, C_MUTED, AT_B);
        g_texte(cx + 4, 0, lab, vw0 - 12, C_WHITE, AT_B | (U.focus == F_MESSAGES ? AT_U : 0));
        if (!chw) { g_icon_btn(cx + vw0 - 4, 0, "[#]", C_TEXT, B_CHATX, T_CHATX); U.hit[U.nhit - 1].tip = T_NONE; }
        g_uline(cx, 0, vw0);
        draw_voice_view(cx, 1, vw0, chw ? H - 1 : H - 2, oc, &vs);
        if (chw) {
            int px = cx + vw0;
            for (int y = 0; y < H; y++) g_ch(px, y, '|', C_LINE, 0);
            px++; chw--;
            g_rect(px, 0, chw, H, C_FEED);
            g_text(px + 1, 0, "#", 1, C_MUTED, AT_B);
            g_texte(px + 3, 0, lab, chw - 8, C_WHITE, AT_B);
            g_icon_btn(px + chw - 3, 0, "x", C_TEXT, B_CHATX, T_CHATX);
            g_uline(px, 0, chw);
            draw_chat(px, chw, H, oc, true);
        } else draw_status(cx, cx + vw0, H - 1);
        if (U.menu) draw_menu(MENU_LEFT(U.menu_kind) ? sx : cx, 1, MENU_LEFT(U.menu_kind) ? W - sx : cw, H - 1);
        if (U.card_uid) draw_card(W, H);
    if (U.ep) draw_ep(W, H);
    if (U.qs) draw_qs(W, H);
        return;
    }

    /* ---- текстовый канал: шапка, лента, справа за линией — участники ---- */
    /* панель результатов поиска справа (вместо участников); узко — на весь центр */
    int srw = U.sr_open || U.rp ? (cw >= 80 ? cw * 42 / 100 : cw) : 0;
    if (srw && srw < cw && srw < 34) srw = 34;
    if (srw > 56) srw = 56;
    if (srw && cw - srw < 50) srw = cw;          /* лента уже 50 клеток нечитаема — панель на весь центр */
    U.rp_full = srw && srw == cw;
    int memw = !srw && oc && oc->guild_id && !U.members_off && W >= 100 ? (PW.memb ? PW.memb : W * 18 / 100) : 0;
    if (memw > 28 && !PW.memb) memw = 28;
    if (memw > cw - 40) memw = cw - 40 > 0 ? cw - 40 : 0;
    int fw = cw - memw - srw;
    if (memw) hit_add(cx + fw, 1, 1, H - 2, A_SPLIT, 0, 1, HV_ACC);   /* №11: граница участников */
    {
        char rtag[240];
        u32 rfg;
        net_tag(rtag, sizeof rtag, &rfg, cw < 60);
        int right = W - 1;
        right -= 3; g_icon_btn(right, 0, "(?)", C_TEXT, B_HELP, T_HELP);
        /* «Входящие» (Ctrl+I): лоток; красная точка — есть упоминания */
        {
            bool ment = false;
            for (int i = 0; i < S.nchans && !ment; i++) if (S.chans[i].mentions && !S.chans[i].muted) ment = true;
            right -= 4; g_icon_btn(right, 0, "[_]", U.rp == RP_INBOX ? C_WHITE : C_TEXT, B_INBOX, T_INBOX);
            if (ment) g_ch(right + 3, 0, '*', C_RED, AT_B);
        }
        bool sact = U.sq_edit || U.sq[0];
        if (cw >= 70 || sact) {
            /* «Поиск»: при вводе плашка шире (как в Discord), видно конец запроса, курсор; x — очистить */
            int pw = sact && cw >= 90 ? 26 : 15;
            right -= pw + 1;
            g_pill(right, 0, pw, C_RAIL);
            int tw2 = pw - 5;
            if (sact) {
                int qw = vw(U.sq), skip = qw > tw2 - 1 ? qw - (tw2 - 1) : 0, acc = 0, used = 0;
                for (const char *p = U.sq; *p; ) {
                    u32 c;
                    p += utf8_dec(p, &c);
                    int cw2 = shown_w(c);
                    if (acc >= skip && used + cw2 <= tw2 - 1) { g_put(right + 2 + used, 0, c, cw2, C_WHITE, 0); used += cw2; }
                    acc += cw2;
                }
                if (U.sq_edit) g_ch(right + 2 + used, 0, ' ', C_TEXT, AT_I);
                if (!U.sq[0]) {                  /* пустая строка — подсказка фильтров, целыми словами */
                    const char *hs = tw2 - 1 >= 22 ? "from: in: has: before:" : tw2 - 1 >= 14 ? "from: in: has:" : TR("Поиск");
                    g_text(right + 3, 0, hs, tw2 - 1, C_MUTED, 0);
                }
            } else g_text(right + 2, 0, TR("Поиск"), tw2, C_MUTED, 0);
            hit_add(right, 0, pw - 3, 1, A_BTN, B_SEARCH, 0, HV_UL); U.hit[U.nhit - 1].tip = T_SEARCH;
            if (U.sq[0]) { g_text(right + pw - 3, 0, "x", 1, C_TEXT, AT_B); hit_add(right + pw - 3, 0, 1, 1, A_BTN, B_SRCH_CLR, 0, HV_RED); }
        }
        if (oc && oc->guild_id) {
            right -= 3; g_icon_btn(right, 0, "oo", U.members_off ? C_MUTED : C_WHITE, B_MEMBERS, T_MEMBERS);
        }
        if (oc) { right -= 3; g_icon_btn(right, 0, "-o", U.rp == RP_PINS ? C_WHITE : C_TEXT, B_PINS, T_PINS); }   /* закреплённые: булавка */
        bool dm = oc && !oc->guild_id && !(vs.phase != VS_IDLE && vs.channel_id == oc->id);
        if (dm) { right -= 5; g_icon_btn(right, 0, "(~)", C_TEXT, B_CALL, T_CALL); }
        int lw = oc ? vw(oc->name ? oc->name : "") + 6 : 8;
        int room = right - 2 - (cx + lw + 2), rx = right - 2;
        if (oc && TB.n >= 2) room = 0;           /* №9: вкладкам — вся шапка (состояние сети — в строке статуса) */
        if (room >= 8) rx = g_rtext(right - 2, 0, rtag, room, rfg, 0);
        if (oc && TB.n >= 2) draw_tabs(cx + 1, rx - 1);   /* №9 */
        else if (oc) {
            char lab[96];
            chan_label(oc, lab, sizeof lab);
            if (!oc->guild_id && oc->type == CH_DM && oc->nrecip > 0) g_listav(cx + 1, 0, oc->recipients[0], lab, G_HDR);
            else g_text(cx + 2, 0, chan_mark(oc), 2, C_MUTED, AT_B);
            int nx0 = cx + 2 + vw(chan_mark(oc)) + 1;
            if (!oc->guild_id && oc->type == CH_DM) nx0 = cx + 6;
            int nu = g_texte(nx0, 0, lab, right - nx0 - 2, C_WHITE, AT_B | (U.focus == F_MESSAGES ? AT_U : 0));
            /* тема канала после имени, через тонкую черту (как в Discord), сколько влезет до статуса сети */
            if (oc->topic && *oc->topic && rx - (nx0 + nu + 3) >= 8) {
                g_ch(nx0 + nu + 1, 0, '|', C_LINE, 0);
                g_texte(nx0 + nu + 3, 0, oc->topic, rx - (nx0 + nu + 3) - 2, C_MUTED, 0);
            }
        } else g_text(cx + 2, 0, APP_NAME, cw - 3, C_WHITE, AT_B);
        g_uline(cx, 0, cw);
    }
    if (fw > 0) draw_chat(cx, fw, H, oc, false);
    if (memw) {
        for (int y = 1; y < H; y++) g_ch(cx + fw, y, '|', C_LINE, 0);
        draw_members(cx + fw + 1, memw - 1, H, oc->guild_id, oc->id);
    }
    if (srw) {
        if (fw > 0) { for (int y = 1; y < H; y++) g_ch(cx + fw, y, '|', C_LINE, 0); draw_search(cx + fw + 1, srw - 1, H); }
        else { draw_search(cx, cw, H); draw_status(cx, cx + cw, H - 1); }
    }
    if (U.pick) draw_picker(W, H, sx + sw - 4, 5);
    if (U.card_uid) draw_card(W, H);
    if (U.ep) draw_ep(W, H);
    if (U.qs) draw_qs(W, H);
    if (U.menu) draw_menu(MENU_LEFT(U.menu_kind) ? sx : cx, 1, MENU_LEFT(U.menu_kind) ? W - sx : cw, H - 1);
}

/* ======================= QR-код (для входа по QR) =======================
 * Кодер QR (ISO/IEC 18004): байтовый режим, версии 1..10, коррекция M (если не влезает — L), маска по
 * штрафам. Матрица — до 57x57 модулей, статическая. Рисуется модулями: 2 клетки по ширине на 1 по высоте
 * (квадрат на экране), тёмный модуль — тёмный фон, светлый — белый, белая рамка 4 модуля. В окне — мелкой
 * сеткой (модуль 2K/2 x K/2 мелких клеток), в терминале — обычными клетками, если влезает. */
#define QR_MAXN 57
static u8 qr_m[QR_MAXN * QR_MAXN], qr_f[QR_MAXN * QR_MAXN];
static int qr_n;
static char qr_for[200];                         /* что закодировано сейчас */
static u8 gf_mul(u8 a, u8 b) {
    int r = 0;
    for (int i = 7; i >= 0; i--) { r = (r << 1) ^ ((r >> 7) * 0x11D); r ^= ((b >> i) & 1) * a; }
    return (u8)r;
}
static void qr_set(int x, int y, bool dark, bool func) {
    qr_m[y * qr_n + x] = dark;
    if (func) qr_f[y * qr_n + x] = 1;
}
static int qr_raw_modules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) { int na = ver / 7 + 2; r -= (25 * na - 10) * na - 55; if (ver >= 7) r -= 36; }
    return r;
}
static int qr_align(int ver, int *pos) {
    if (ver == 1) return 0;
    int na = ver / 7 + 2, step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
    pos[0] = 6;
    for (int i = na - 1, p = ver * 4 + 10; i >= 1; i--, p -= step) pos[i] = p;
    return na;
}
static void qr_format(int ecl_bits, int mask) {
    int data = ecl_bits << 3 | mask, rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412, n = qr_n;
    for (int i = 0; i <= 5; i++) qr_set(8, i, bits >> i & 1, true);
    qr_set(8, 7, bits >> 6 & 1, true);
    qr_set(8, 8, bits >> 7 & 1, true);
    qr_set(7, 8, bits >> 8 & 1, true);
    for (int i = 9; i < 15; i++) qr_set(14 - i, 8, bits >> i & 1, true);
    for (int i = 0; i < 8; i++) qr_set(n - 1 - i, 8, bits >> i & 1, true);
    for (int i = 8; i < 15; i++) qr_set(8, n - 15 + i, bits >> i & 1, true);
    qr_set(8, n - 8, true, true);
}
static bool qr_mask_bit(int m, int x, int y) {
    switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}
static void qr_apply_mask(int m) {
    for (int y = 0; y < qr_n; y++) for (int x = 0; x < qr_n; x++)
        if (!qr_f[y * qr_n + x] && qr_mask_bit(m, x, y)) qr_m[y * qr_n + x] ^= 1;
}
static int qr_penalty(void) {
    int p = 0, n = qr_n, dark = 0;
    for (int pass = 0; pass < 2; pass++)          /* серии одного цвета >= 5 */
        for (int a = 0; a < n; a++) {
            int run = 1;
            for (int b = 1; b < n; b++) {
                u8 c = pass ? qr_m[b * n + a] : qr_m[a * n + b], d = pass ? qr_m[(b - 1) * n + a] : qr_m[a * n + b - 1];
                if (c == d) { run++; if (run == 5) p += 3; else if (run > 5) p++; } else run = 1;
            }
        }
    for (int y = 0; y + 1 < n; y++) for (int x = 0; x + 1 < n; x++) {   /* квадраты 2x2 */
        u8 c = qr_m[y * n + x];
        if (c == qr_m[y * n + x + 1] && c == qr_m[(y + 1) * n + x] && c == qr_m[(y + 1) * n + x + 1]) p += 3;
    }
    for (int i = 0; i < n * n; i++) dark += qr_m[i];
    int k = (abs(dark * 20 - n * n * 10) + n * n - 1) / (n * n) - 1;
    return p + (k > 0 ? k : 0) * 10;
}
/* Закодировать строку s (байты). false — длиннее, чем влезает в версию 10. */
static bool qr_encode(const char *s) {
    static const signed char ECC[2][11] = { { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 },    /* M */
                                            { 0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18 } };   /* L */
    static const signed char NB[2][11] = { { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 }, { 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4 } };
    int len = (int)strlen(s), ver = 0, e = 0;
    for (e = 0; e < 2 && !ver; e++)
        for (int v = 1; v <= 10; v++) {
            int cap = qr_raw_modules(v) / 8 - ECC[e][v] * NB[e][v];
            if (4 + (v < 10 ? 8 : 16) + len * 8 <= cap * 8) { ver = v; break; }
        }
    if (!ver) return false;
    e--;
    int ecc = ECC[e][ver], nb = NB[e][ver], raw = qr_raw_modules(ver) / 8, dcw = raw - ecc * nb;
    static u8 data[400], out[400];
    memset(data, 0, sizeof data);
    int bl = 0;
#define QBITS(val, cnt) do { for (int q_ = (cnt) - 1; q_ >= 0; q_--, bl++) if ((val) >> q_ & 1) data[bl >> 3] |= (u8)(0x80 >> (bl & 7)); } while (0)
    QBITS(4, 4);
    QBITS(len, ver < 10 ? 8 : 16);
    for (int i = 0; i < len; i++) QBITS((unsigned char)s[i], 8);
    int term = dcw * 8 - bl < 4 ? dcw * 8 - bl : 4;
    bl += term;
    bl = (bl + 7) / 8 * 8;
    for (u8 pad = 0xEC; bl < dcw * 8; pad ^= 0xEC ^ 0x11) QBITS(pad, 8);
#undef QBITS
    /* Рида-Соломона по блокам и чередование */
    u8 div[32] = {0}, root = 1;
    div[ecc - 1] = 1;
    for (int i = 0; i < ecc; i++) {
        for (int j = 0; j < ecc; j++) { div[j] = gf_mul(div[j], root); if (j + 1 < ecc) div[j] ^= div[j + 1]; }
        root = gf_mul(root, 2);
    }
    int nshort = nb - raw % nb, slen = raw / nb;
    static u8 blk[8][160];
    for (int i = 0, k = 0; i < nb; i++) {
        int dl = slen - ecc + (i < nshort ? 0 : 1);
        u8 *b = blk[i];
        memset(b, 0, 160);
        memcpy(b, data + k, (size_t)dl);
        k += dl;
        u8 rem[32] = {0};
        for (int q = 0; q < dl; q++) {
            u8 f = b[q] ^ rem[0];
            memmove(rem, rem + 1, (size_t)ecc - 1);
            rem[ecc - 1] = 0;
            for (int j = 0; j < ecc; j++) rem[j] ^= gf_mul(div[j], f);
        }
        if (i < nshort) { memcpy(b + dl + 1, rem, (size_t)ecc); b[dl] = 0; } else memcpy(b + dl, rem, (size_t)ecc);
    }
    int ol = 0;
    for (int i = 0; i <= slen; i++)
        for (int j = 0; j < nb; j++)
            if (i != slen - ecc || j >= nshort) out[ol++] = blk[j][i];
    /* функциональные узоры */
    qr_n = ver * 4 + 17;
    int n = qr_n;
    memset(qr_m, 0, sizeof qr_m);
    memset(qr_f, 0, sizeof qr_f);
    for (int i = 0; i < n; i++) { qr_set(6, i, i % 2 == 0, true); qr_set(i, 6, i % 2 == 0, true); }
    int fc[3][2] = { { 3, 3 }, { n - 4, 3 }, { 3, n - 4 } };
    for (int f = 0; f < 3; f++)
        for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) {
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, d = ax > ay ? ax : ay, x = fc[f][0] + dx, y = fc[f][1] + dy;
            if (x >= 0 && x < n && y >= 0 && y < n) qr_set(x, y, d != 2 && d != 4, true);
        }
    int ap[8], na = qr_align(ver, ap);
    for (int i = 0; i < na; i++) for (int j = 0; j < na; j++) {
        if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;
        for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            qr_set(ap[i] + dx, ap[j] + dy, (ax > ay ? ax : ay) != 1, true);
        }
    }
    int eb = e == 0 ? 0 : 1;                     /* биты уровня: M = 00, L = 01 */
    qr_format(eb, 0);
    if (ver >= 7) {
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = (long)ver << 12 | rem;
        for (int i = 0; i < 18; i++) {
            bool bt = bits >> i & 1;
            int a = n - 11 + i % 3, b = i / 3;
            qr_set(a, b, bt, true);
            qr_set(b, a, bt, true);
        }
    }
    /* данные змейкой */
    int i = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool up = ((right + 1) & 2) == 0;
                int y = up ? n - 1 - vert : vert;
                if (!qr_f[y * n + x] && i < ol * 8) { qr_m[y * n + x] = out[i >> 3] >> (7 - (i & 7)) & 1; i++; }
            }
    }
    int best = 0, bp = 1 << 30;
    for (int m = 0; m < 8; m++) {
        qr_apply_mask(m);
        qr_format(eb, m);
        int p = qr_penalty();
        if (p < bp) { bp = p; best = m; }
        qr_apply_mask(m);
    }
    qr_apply_mask(best);
    qr_format(eb, best);
    wipe(data, sizeof data);
    wipe(out, sizeof out);
    wipe(blk, sizeof blk);
    return true;
}
/* Модуль (x, y) с рамкой 4 модуля: true — тёмный. */
static bool qr_dark(int x, int y) {
    x -= 4; y -= 4;
    return x >= 0 && y >= 0 && x < qr_n && y < qr_n && qr_m[y * qr_n + x];
}
#define QR_DARK  0x101114u
#define QR_LIGHT 0xffffffu
/* QR в прямоугольнике qw x qh клеток (qw = 2 qh). Окно — слой мелкой сетки; терминал — модуль 2x1 клетки,
 * если влезает, иначе подпись. */
static void ovl_qr(int x, int y, int w, int h) {
    if (novl >= MAX_OVL) return;
    Ovl *o = &ovl[novl++];
    memset(o, 0, sizeof *o);
    o->kind = OV_QR;
    o->prio = 10;
    o->x = x; o->y = y; o->w = w; o->h = h;
    o->url = xstrdup(qr_for);
}
static int g_qr(int x, int y, int maxw, int maxh, const char *text) {
    if (strcmp(qr_for, text)) { if (!qr_encode(text)) return 0; snprintf(qr_for, sizeof qr_for, "%s", text); }
    int nm = qr_n + 8;
    if (term_fine_images() && term_subcell_scale() >= 2) {
        int K = term_subcell_scale() > FINE_K ? FINE_K : term_subcell_scale();
        int qh = (nm * 2 + K - 1) / K;           /* модуль — 2 мелкие строки, если влезает, иначе 1 */
        if (qh > maxh || qh * 2 > maxw) qh = (nm + K - 1) / K;
        if (qh > maxh || qh * 2 > maxw) return 0;
        x += (maxw - qh * 2) / 2;                /* по центру отведённой ширины */
        g_rect(x, y, qh * 2, qh, QR_LIGHT);
        /* под слоем — грубая копия (каждая клетка — цвет модуля в её середине) */
        for (int r = 0; r < qh; r++) for (int c = 0; c < qh * 2; c++)
            if (qr_dark((c * 2 + 1) * nm / (qh * 4), (r * 2 + 1) * nm / (qh * 2))) g_rect(x + c, y + r, 1, 1, QR_DARK);
        ovl_qr(x, y, qh * 2, qh);
        return qh;
    }
    if (nm > maxh || nm * 2 > maxw) return 0;
    x += (maxw - nm * 2) / 2;
    for (int r = 0; r < nm; r++) for (int c = 0; c < nm; c++) g_rect(x + c * 2, y + r, 2, 1, qr_dark(c, r) ? QR_DARK : QR_LIGHT);
    return nm;
}

/* ======================= окно входа =======================
 * Шаги: VLESS (ссылка или «без VLESS», только интерфейс) -> экран входа по снимку login_view() (login.c):
 * форма + QR, капча, 2FA, письмо, телефон, восстановление аккаунта, «Проверьте телефон» (QR отсканирован).
 * В --demo движка нет (сети нет): тот же LoginView заполняет заглушка ниже — экраны и снимки те же.
 * Пароль, коды и ссылка живут в буферах ui.c только до передачи движку и затираются сразу (buf_wipe). */
static void busy(const char *what);
enum { LS_VLESS, LS_FORM, LS_CAPTCHA, LS_MFA, LS_EMAIL, LS_QRSCAN, LS_QR, LS_PHONE, LS_UNDEL };
enum { LFI_NONE, LFI_LINK, LFI_USER, LFI_PASS, LFI_CODE };
enum { B_LG_VCHECK = 200, B_LG_VSKIP, B_LG_LOGIN, B_LG_FORGOT, B_LG_REGISTER, B_LG_QRSHOW, B_LG_BACK, B_LG_QRGO,
       B_LG_CODE, B_LG_SMS, B_LG_BACKUP, B_LG_TOTP, B_LG_RETRY, B_LG_RESTART, B_LG_QRDEMO, B_LG_QRNEW, B_LG_UNDEL,
       B_LG_SMSAGAIN, B_LG_CAPOPEN, B_LG_CAPCANCEL, B_LG_NOASK, B_LG_VRETRY, B_LG_VQUIT };
/* кнопки входа и общие B_* — одна таблица щелчков (Hit.sub — u8): номера не должны пересекаться */
_Static_assert((int)B_SP_NEXT < (int)B_LG_VCHECK, "B_* дошли до B_LG_*: поднимите B_LG_VCHECK");
_Static_assert(B_LG_VQUIT < 256, "B_LG_* не влезают в Hit.sub (u8)");
#define C_LINK 0x4ea8deu
static struct {
    bool on, vless, qr_full, qr_req;
    int scr, foc;                                /* экран прошлого кадра, фокус (индекс в items) */
    Buf user, pass, code, link;
    char err_link[200], note[200];
    int items[10], nitems;                       /* что выбирается Tab: LFI_* (поле) или 1000 + B_LG_* */
    bool was_busy, want_err_focus;               /* ответ движка пришёл: фокус — на поле с ошибкой */
    bool vonly;                                  /* стартовый экран: только шаг VLESS (токен уже есть) */
    bool noask;                                  /* галочка «Больше не спрашивать при запуске» */
    char verr[200];                              /* сохранённая ссылка не работает: причина (экран ошибки) */
} LG;
/* «Больше не спрашивать» + «Без VLESS» = явный выбор «напрямую», хранится в системном хранилище секретов
 * (запись "startup" = ровно "direct-confirmed"), как токен и ссылка: никаких файлов. Требования безопасности:
 * сохранённая ссылка важнее (main.c поднимает её первой, флаг — только если ссылки нет); любая успешная ссылка
 * флаг стирает (пропала ссылка — будет вопрос, а не «напрямую»); значение — точное совпадение; --logout и паника
 * удаляют запись; пропажа флага — лишь лишний вопрос. */
#define STARTUP_DIRECT "direct-confirmed"
static int startup_direct_mem = -1;               /* демо и кэш на время работы: -1 — не читали */
/* Язык: forced 0/1 — флаг --lang=ru|en (main.c); -1 — сохранённый выбор (хранилище «lang»: «en»/«ru»),
 * без него — язык системы (LANG/LC_ALL; Windows без них — русский; разработчик может дать plat_ui_lang). */
static int lang_inited;
void ui_lang_init(int forced) {
    lang_inited = 1;
    if (forced == 0 || forced == 1) { g_lang_en = forced == 1; return; }
    char b[8] = "";
    int n = O.demo || O.no_save ? -1 : keyring_load("lang", b, sizeof b);
    if (n == 2 && !memcmp(b, "en", 2)) { g_lang_en = true; return; }
    if (n == 2 && !memcmp(b, "ru", 2)) { g_lang_en = false; return; }
    g_lang_en = plat_ui_lang() == 1;            /* язык системы (plat_win/plat_posix) */
}
static void lang_set(bool en) {
    g_lang_en = en;
    if (!O.demo && !O.no_save) keyring_save("lang", en ? "en" : "ru", 2);
    U.full = U.dirty = true;
}
static bool startup_direct(void) {
    if (startup_direct_mem < 0) {
        char b[32] = "";
        int n = O.demo || O.no_save ? -1 : keyring_load("startup", b, sizeof b);
        startup_direct_mem = n == (int)strlen(STARTUP_DIRECT) && !memcmp(b, STARTUP_DIRECT, (size_t)n);
        wipe(b, sizeof b);
    }
    return startup_direct_mem == 1;
}
static void startup_direct_set(bool on) {
    startup_direct_mem = on;
    if (O.demo || O.no_save) return;
    if (on) keyring_save("startup", STARTUP_DIRECT, strlen(STARTUP_DIRECT)); else keyring_delete("startup");
}

/* ---- заглушка движка для --demo (тот же LoginView) ---- */
static LoginView dv;
static char dv_e1[160], dv_e2[160], dv_e3[160], dv_err[160];
static void dv_reset_errs(void) { dv_e1[0] = dv_e2[0] = dv_e3[0] = dv_err[0] = 0; }
static void dv_fix(void) {
    dv.err_login = dv_e1; dv.err_pass = dv_e2; dv.err_code = dv_e3; dv.err_token = "";
    dv.error = dv_err; dv.error_field = LF_NONE;
    if (!dv.busy_text) dv.busy_text = "";
    if (!dv.notice) dv.notice = "";
    if (!dv.phone) dv.phone = "";
    if (!dv.captcha_service) dv.captcha_service = "";
    if (!dv.qr_text) dv.qr_text = "";
    if (!dv.qr_user_name) dv.qr_user_name = "";
    if (!dv.qr_discrim) dv.qr_discrim = "0";
    if (!dv.qr_avatar) dv.qr_avatar = "";
    if (!dv.qr_url) dv.qr_url = "";
}
static const LoginView *lv(void) { if (O.demo) { dv_fix(); return &dv; } return login_view(); }
static void lgx_password(const char *u, const char *p) {
    if (!O.demo) { login_start_password(u, p); return; }
    dv_reset_errs();
    if (!*u) snprintf(dv_e1, sizeof dv_e1, TR("Обязательное поле"));
    if (!*p) snprintf(dv_e2, sizeof dv_e2, TR("Обязательное поле"));
    if (!*u || !*p) return;
    if (strstr(u, "captcha")) {
        dv.stage = LG_CAPTCHA; dv.captcha_service = "hcaptcha";
#ifdef LOGIN_CAPTCHA_UI
        dv.captcha = dv.captcha_supported = true; dv.captcha_qr = dv.captcha_open = false;
#endif
    }
    else if (strstr(u, "mail")) dv.stage = LG_EMAIL;
    else if (strstr(u, "phone")) dv.stage = LG_PHONE;
    else if (strstr(u, "undel")) { dv.stage = LG_UNDELETE; dv.notice = TR("Аккаунт отключён. Его можно восстановить и войти."); }
    else if (strstr(u, "bad")) { snprintf(dv_e1, sizeof dv_e1, TR("Неверный логин или пароль.")); snprintf(dv_e2, sizeof dv_e2, TR("Неверный логин или пароль.")); }
    else { dv.stage = LG_MFA; dv.mfa_methods = LM_TOTP | LM_SMS | LM_BACKUP; dv.mfa_method = LM_TOTP; dv.sms_sent = false; }
}
static void lgx_code(const char *c, int kind) {
    if (!O.demo) { login_submit_code(c, kind); return; }
    dv_reset_errs();
    if (strcmp(c, "123456")) snprintf(dv_e3, sizeof dv_e3, TR("Неверный код. Попробуйте ещё раз."));
    else dv.stage = LG_DONE;
}
static void lgx_mfa_method(unsigned m) {
    if (!O.demo) { login_mfa_method(m); return; }
    dv.mfa_method = m; dv_reset_errs();
    if (m == LM_SMS) { dv.sms_sent = true; dv.phone = "+7*******0012"; }
}
static void lgx_sms(void) { if (!O.demo) login_sms_send(); else { dv.sms_sent = true; dv.phone = "+7*******0012"; } }
static void lgx_retry(void) {
    if (!O.demo) { login_email_retry(); return; }
    dv.stage = LG_MFA; dv.mfa_methods = LM_TOTP; dv.mfa_method = LM_TOTP;
}
static void lgx_undelete(void) { if (!O.demo) login_undelete(); else dv.stage = LG_DONE; }
static void lgx_back(void) { if (!O.demo) login_back(); else { dv_reset_errs(); dv.stage = LG_FORM; dv.notice = ""; } }
static void lgx_qr_start(void) {
    if (!O.demo) { login_qr_start(); return; }
    /* настоящая ссылка — https://discord.com/ra/<отпечаток> от шлюза удалённой авторизации */
    dv.qr = QR_SHOW; dv.qr_url = "https://discord.com/ra/astra-demo-0123456789abcdefghijklmnopq";
    dv.qr_user_id = 0;
}

static void lg_done(void) {
    buf_wipe(&LG.user); buf_wipe(&LG.pass); buf_wipe(&LG.code); buf_wipe(&LG.link);
    LG.on = false;
    U.full = U.dirty = true;
    ui_notice(TR("вход выполнен%s"), O.demo ? TR(" (демо)") : "");   /* main.c сам вызовет login_finish() */
}

/* ---- решение капчи в окне WebView2 (login.c; поля LoginView появятся с макросом LOGIN_CAPTCHA_UI) ---- */
static int lg_btn(int x, int y, int w, const char *t, int style, int id);
static int lg_para(int x, int y, int w, const char *t, u32 fg, bool center);
/* Кнопки проверки для экрана капчи (qr = false) или блока «Проверьте телефон» (qr = true). Вернуть строки;
 * 0 — проверка здесь не нужна или не поддерживается (тогда остаётся совет войти по QR). */
static int lg_captcha_ui(const LoginView *v, int x, int y, int w, bool qr) {
#ifdef LOGIN_CAPTCHA_UI
    if (!v->captcha || !v->captcha_supported || v->captcha_qr != qr) return 0;
    if (v->captcha_open) {
        int n = lg_para(x, y, w, TR("Решите проверку в открывшемся окне."), C_WHITE, true);
        int bw = vw(TR("Отмена")) + 4;
        lg_btn(x + (w - bw) / 2, y + n + 1, 0, TR("Отмена"), 0, B_LG_CAPCANCEL);
        return n + 2;
    }
    lg_btn(x, y, w, TR("Пройти проверку"), 1, B_LG_CAPOPEN);
    return 2;
#else
    (void)v; (void)x; (void)y; (void)w; (void)qr;
    return 0;
#endif
}
static void lgx_captcha(bool open) {
#ifdef LOGIN_CAPTCHA_UI
    if (O.demo) { dv.captcha_open = open; return; }
    if (open) login_captcha_open(); else login_captcha_cancel();
#else
    (void)open;
#endif
}

/* ---- рисование ---- */
static void lg_item(int id) { if (LG.nitems < 10) LG.items[LG.nitems++] = id; }
static bool lg_focused(void) { return LG.foc == LG.nitems; }   /* до lg_item(): этот элемент — следующий в списке Tab */
/* Подпись поля: обычная — светлая с «*»; ошибка — красная «Подпись - текст ошибки» (как в Discord). */
static void lg_label(int x, int y, int w, const char *t, const char *err) {
    int u = g_text(x, y, t, w, err && *err ? C_RED : C_TEXT, AT_B);
    if (err && *err) { u += g_text(x + u, y, " - ", w - u, C_RED, 0); g_texte(x + u, y, err, w - u, C_RED, 0); }
    else g_text(x + u, y, " *", w - u, C_RED, AT_B);
}
/* Поле ввода в рамке |...|: secret — звёздочки (2 — у ссылки открыт только префикс vless://). */
static void lg_field(int x, int y, int w, const Buf *b, int fid, int secret, const char *ph) {
    bool f = lg_focused();
    g_rect(x, y, w, 1, C_INPUT);
    g_ch(x, y, '|', f ? C_WHITE : C_LINE, AT_B);
    g_ch(x + w - 1, y, '|', f ? C_WHITE : C_LINE, AT_B);
    int iw = w - 4, n = 0;
    for (size_t i = 0; i < b->len; ) { u32 c; i += (size_t)utf8_dec(b->p + i, &c); n++; }
    int skip = n + 1 > iw ? n + 1 - iw : 0, k = 0, used = 0;
    size_t open_n = secret == 2 && b->len >= 8 && !strncmp(b->p, "vless://", 8) ? 8 : 0;
    for (size_t i = 0; i < b->len; k++) {
        u32 c;
        size_t at = i;
        i += (size_t)utf8_dec(b->p + i, &c);
        if (k < skip) continue;
        bool hide = secret || (fid == LFI_USER && streamer_on());   /* №125: почта/телефон на входе */
        int cw = hide ? 1 : shown_w(c);
        if (!cw || used + cw > iw) continue;
        if (hide && at >= open_n) g_ch(x + 2 + used, y, '*', C_TEXT, 0);
        else g_put(x + 2 + used, y, c, cw, C_TEXT, 0);
        used += cw;
    }
    if (f && used < iw) g_ch(x + 2 + used, y, ' ', C_TEXT, AT_I);
    if (!b->len && ph) g_texte(x + (f ? 3 : 2), y, ph, iw - 1, C_MUTED, 0);
    hit_add(x, y, w, 1, A_INPUT, 0, 1000 + fid, 0);
    lg_item(fid);
}
/* Кнопка: style 1 — главная (во всю ширину w), 0 — вторичная, 4 — ссылка (синим текстом), 5 — неактивная. */
static int lg_btn(int x, int y, int w, const char *t, int style, int id) {
    bool f = lg_focused();
    int tw = vw(t);
    if (style == 4) {
        int u = g_texte(x, y, t, w, C_LINK, f ? AT_U | AT_B : 0);
        hit_add(x, y, u, 1, A_BTN, (u8)id, 0, HV_UL);
        lg_item(1000 + id);
        return u;
    }
    if (w <= 0) w = tw + 4;
    if (w > tw + 4 && style != 1 && style != 5) w = tw + 4;
    g_pill(x, y, w, style == 1 ? C_ACC : style == 5 ? 0x3c4270u : C_BTN2);
    g_ctext(x, y, w, t, style == 5 ? C_MUTED : C_WHITE, AT_B | (f ? AT_U : 0));
    if (f) { g_ch(x, y, '>', C_WHITE, AT_B); g_ch(x + w - 1, y, '<', C_WHITE, AT_B); }
    if (style != 5) hit_add(x, y, w, 1, A_BTN, (u8)id, 0, HV_PILL);
    lg_item(1000 + id);
    return w;
}
static int lg_para(int x, int y, int w, const char *t, u32 fg, bool center) {
    char **ls;
    int m = help_wrap(t, w, &ls);
    for (int i = 0; i < m; i++) {
        if (center) g_ctext(x, y + i, w, ls[i], fg, 0); else g_text(x, y + i, ls[i], w, fg, 0);
        free(ls[i]);
    }
    free(ls);
    return m;
}
/* Колонка QR по состоянию QR-ветки движка. false — код не влез. */
static bool lg_qr_col(const LoginView *v, int x, int y, int w, int h) {
    if (v->qr == QR_OFF) { if (!LG.qr_req) { LG.qr_req = true; lgx_qr_start(); } }   /* один раз, не в каждом кадре */
    else LG.qr_req = false;
    if (v->qr == QR_EXPIRED || v->qr == QR_ERROR) {
        int yy = y + h / 2 - 2;
        g_ctext(x, yy, w, v->qr == QR_EXPIRED ? TR("QR-код устарел") : TR("QR-код недоступен"), C_WHITE, AT_B);
        yy += 1 + lg_para(x, yy + 1, w, v->qr_text && *v->qr_text ? v->qr_text : TR("нажмите \"Обновить\""), C_MUTED, true);
        int bw = vw(TR("Обновить")) + 4;
        lg_btn(x + (w - bw) / 2, yy + 1, 0, TR("Обновить"), 0, B_LG_QRNEW);
        return true;
    }
    if (v->qr != QR_SHOW || !v->qr_url || !*v->qr_url) {
        g_ctext(x, y + h / 2, w, TR("QR-код загружается..."), C_MUTED, 0);
        return true;
    }
    int qh = g_qr(x, y, w, h - 4, v->qr_url);
    if (!qh) return false;
    int yy = y + qh + 1;
    g_ctext(x, yy, w, TR("Войти с помощью QR-кода"), C_WHITE, AT_B);
    lg_para(x, yy + 1, w, TR("Отсканируйте его камерой в мобильном приложении Discord, чтобы войти сразу."), C_MUTED, true);
    if (O.demo) hit_add(x, y, w, qh, A_BTN, B_LG_QRDEMO, 0, HV_BG);   /* демо: щелчок по коду — «телефон отсканировал» */
    return true;
}

/* Ошибка у поля ссылки: после попытки «Подключить» (LG.err_link) или если начатый текст уже не может стать
 * vless://... (до любого ввода — обычная подпись, без красного). */
static const char *lg_link_err(void) {
    if (LG.err_link[0]) return LG.err_link;
    size_t a = 0, n = LG.link.len;
    while (a < n && LG.link.p[a] == ' ') a++;
    if (a >= n) return NULL;
    size_t k = n - a < 8 ? n - a : 8;
    if (strncmp(LG.link.p + a, "vless://", k)) return TR("ссылка должна начинаться с vless://");
    return NULL;
}
static void draw_login(int W, int H) {
    const LoginView *v = lv();
    if (v->stage == LG_DONE && !LG.vless) { lg_done(); draw_main(W, H); return; }
    /* какой экран: шаг VLESS и полный QR — интерфейс; остальное — этап движка (LG_BUSY — прежний экран) */
    int scr = LG.vless ? LS_VLESS : v->qr == QR_SCANNED ? LS_QRSCAN : LG.qr_full ? LS_QR : LG.scr;
    if (!LG.vless && v->qr != QR_SCANNED && !LG.qr_full)
        switch (v->stage) {
        case LG_FORM: case LG_TOKEN: scr = LS_FORM; break;
        case LG_CAPTCHA: scr = LS_CAPTCHA; break;
        case LG_MFA: scr = LS_MFA; break;
        case LG_EMAIL: scr = LS_EMAIL; break;
        case LG_PHONE: scr = LS_PHONE; break;
        case LG_UNDELETE: scr = LS_UNDEL; break;
        default: break;                          /* LG_BUSY: тот же экран */
        }
    if (scr != LG.scr) { LG.scr = scr; LG.foc = 0; }
    bool busyst = v->stage == LG_BUSY;
    if (LG.was_busy && !busyst) LG.want_err_focus = true;
    LG.was_busy = busyst;
    g_rect(0, 0, W, H, C_RAIL);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        u32 hs = hsh32((u64)(x * 977 + y * 131));
        if (hs % 41 == 0) g_ch(x, y, hs % 3 ? '.' : '+', 0x2c2f33u, 0);
    }
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    LG.nitems = 0;
    bool qr_side = (scr == LS_FORM || scr == LS_CAPTCHA) && W >= 96 && H >= 26;
    int cw = qr_side ? (W - 6 < 104 ? W - 6 : 104) : (W - 4 < 64 ? W - 4 : 64);
    static const int want[] = { 21, 21, 16, 20, 18, 16, 0, 16, 14 };   /* высота карточки по экрану (LS_*) */
    int ch = scr == LS_QR ? H - 2 : qr_side ? 28 : want[scr];
    if (ch > H - 2) ch = H - 2;
    int cx = (W - cw) / 2, cy = (H - ch) / 2;
    g_rect(cx, cy, cw, ch, C_FEED);
    g_hbox(cx, cy, cw);
    g_hbox(cx, cy + ch - 1, cw);
    for (int y = cy + 1; y < cy + ch - 1; y++) { g_ch(cx, y, '|', C_LINE, 0); g_ch(cx + cw - 1, y, '|', C_LINE, 0); }
    hit_add(cx, cy, cw, ch, A_BLOCK, 0, 0, 0);
    if (cw >= 30) {                              /* язык — до входа (англоязычному пользователю) */
        int lxx = cx + cw - 10;
        g_text(lxx, cy, " RU|EN ", 7, C_MUTED, 0);
        g_fgset(lxx + (g_lang_en ? 4 : 1), cy, 2, C_WHITE, AT_B | AT_U);
        hit_add(lxx, cy, 7, 1, A_BTN, B_LANG, 0, HV_UL);
    }
    int lx = cx + 4, lw = qr_side ? cw * 58 / 100 - 6 : cw - 8, y = cy + 2;
    const char *gerr = v->error && *v->error && v->error_field == LF_NONE ? v->error : NULL;
    bool waiting = v->wait_until && now_ms() < v->wait_until;
    switch (scr) {
    case LS_VLESS: {
        /* низкое окно: сначала уходят пояснение и подсказка, потом подзаголовок и пустые строки;
         * поле, галочка и кнопки видны всегда (av — строк внутри карточки) */
        int av = cy + ch - 1 - y;
        bool roomy = av >= 13, tall = av >= 16;
        g_ctext(cx, y++, cw, APP_NAME, C_WHITE, AT_B);
#ifdef ASTRA_VIDEO
        g_ctext(cx, y++, cw, TR("с демонстрацией экрана и камерой"), C_MUTED, 0);
#endif
        if (LG.verr[0]) {
            /* сохранённая ссылка не работает: причина и выбор — повторить, другая ссылка, напрямую, выход */
            g_ctext(cx, y++, cw, TR("VLESS не работает"), C_RED, AT_B);
            if (tall) y += lg_para(lx, y, lw, LG.verr, C_TEXT, true) + 1;
            else g_texte(lx, y++, LG.verr, lw, C_TEXT, 0);
            int bw = (lw - 1) / 2;
            lg_btn(lx, y, bw, TR("Повторить"), 1, B_LG_VRETRY);
            lg_btn(lx + bw + 1, y, lw - bw - 1, TR("Выход"), 0, B_LG_VQUIT);
            y += roomy ? 2 : 1;
            lg_label(lx, y++, lw, TR("Другая ссылка vless://"), LG.err_link);
            lg_field(lx, y++, lw, &LG.link, LFI_LINK, 2, "vless://...");
            if (roomy) {
                y++;
                lg_btn(lx, y, lw, TR("Подключить"), 0, B_LG_VCHECK);
                y += 2;
                lg_btn(lx, y, lw, TR("Без VLESS: напрямую (Discord увидит ваш IP)"), 0, B_LG_VSKIP);
            } else {
                lg_btn(lx, y, bw, TR("Подключить"), 0, B_LG_VCHECK);
                lg_btn(lx + bw + 1, y, lw - bw - 1, TR("Без VLESS"), 0, B_LG_VSKIP);
            }
            break;
        }
        if (av >= 9) g_ctext(cx, y++, cw, TR("Подключение"), C_MUTED, AT_B);
        if (tall) y += lg_para(lx, y, lw, TR("Весь трафик Astra DS (сообщения, голос) может идти через ваш сервер VLESS (Reality). Ссылка проверяется сразу: пробный запрос к Discord через туннель."), C_MUTED, true) + 1;
        lg_label(lx, y++, lw, TR("Ссылка vless://"), lg_link_err());
        lg_field(lx, y++, lw, &LG.link, LFI_LINK, 2, "vless://...");
        if (roomy) { g_text(lx, y++, TR("на экране ссылка не показывается: только звёздочки"), lw, C_MUTED, 0); y++; }
        {
            /* галочка «Больше не спрашивать при запуске» — пункт Tab, пробел/Enter/щелчок переключают */
            bool f = lg_focused();
            g_text(lx, y, LG.noask ? "[x]" : "[ ]", 3, f ? C_WHITE : C_TEXT, AT_B);
            g_texte(lx + 4, y, TR("Больше не спрашивать при запуске"), lw - 4, f ? C_WHITE : C_TEXT, f ? AT_U : 0);
            hit_add(lx, y, lw, 1, A_BTN, B_LG_NOASK, 0, HV_UL);
            lg_item(1000 + B_LG_NOASK);
            y += roomy ? 2 : 1;
        }
        {
            int bw = (lw - 1) / 2;
            lg_btn(lx, y, bw, TR("Подключить"), 1, B_LG_VCHECK);
            lg_btn(lx + bw + 1, y, lw - bw - 1, TR("Без VLESS"), 0, B_LG_VSKIP);
            y++;
            if (tall) y += lg_para(lx, y + 1, lw, TR("без VLESS Discord увидит ваш IP; вернуть вопрос: --vless или Настройки"), C_MUTED, true);
        }
        break;
    }
    case LS_FORM: case LS_CAPTCHA:
        if (scr == LS_FORM) {
            g_ctext(lx, y++, lw, TR("С возвращением!"), C_WHITE, AT_B);
            g_ctext(lx, y++, lw, TR("Мы так рады видеть вас снова!"), C_MUTED, 0);
            if (gerr) g_texte(lx, y, gerr, lw, C_RED, 0);
            y++;
            lg_label(lx, y++, lw, TR("Адрес электронной почты или номер телефона"), v->err_login);
            lg_field(lx, y++, lw, &LG.user, LFI_USER, 0, NULL);
            y++;
            lg_label(lx, y++, lw, TR("Пароль"), v->err_pass);
            lg_field(lx, y++, lw, &LG.pass, LFI_PASS, 1, NULL);
            lg_btn(lx, y++, lw, TR("Забыли пароль?"), 4, B_LG_FORGOT);
            y++;
            lg_btn(lx, y, lw, busyst ? (v->busy_text && *v->busy_text ? v->busy_text : TR("Вход...")) : TR("Вход"), busyst || waiting ? 5 : 1, B_LG_LOGIN);
            y += 2;
            int u = g_text(lx, y, TR("Нужна учётная запись? "), lw, C_MUTED, 0);
            lg_btn(lx + u, y, lw - u, TR("Зарегистрироваться"), 4, B_LG_REGISTER);
            if (!qr_side) { y += 2; lg_btn(lx, y, lw, TR("Войти с помощью QR-кода"), 0, B_LG_QRSHOW); }
        } else {
            g_ctext(lx, y++, lw, TR("Подтвердите, что вы человек"), C_WHITE, AT_B);
            y++;
            bool cap_ok = false;
#ifdef LOGIN_CAPTCHA_UI
            cap_ok = v->captcha_supported;
#endif
            const char *ct = cap_ok ? TR("Discord просит пройти проверку (hCaptcha). Она откроется в отдельном окне только на время проверки.")
                                    : TR("Discord просит пройти капчу (hCaptcha). Здесь её не показать: нужен компонент WebView2 (Windows).");
            y += lg_para(lx, y, lw, v->notice && *v->notice ? v->notice : ct, C_TEXT, false) + 1;
            if (gerr) y += lg_para(lx, y, lw, gerr, C_RED, false) + 1;   /* «Discord не принял проверку...» */
            y += lg_para(lx, y, lw, cap_ok ? TR("Или войдите по QR-коду: откройте Discord на телефоне, Настройки -> Сканировать QR-код.")
                                           : TR("Войдите по QR-коду: откройте Discord на телефоне, Настройки -> Сканировать QR-код."), C_MUTED, false) + 1;
            y += lg_captcha_ui(v, lx, y, lw, false);
            if (!qr_side) { lg_btn(lx, y, lw, TR("Войти с помощью QR-кода"), 1, B_LG_QRSHOW); y += 2; }
            lg_btn(lx, y, lw, TR("Назад"), 0, B_LG_BACK);
        }
        if (qr_side) {
            int qx = cx + cw * 58 / 100, qw = cw - (qx - cx) - 3;
            for (int yy = cy + 2; yy < cy + ch - 2; yy++) g_ch(qx - 2, yy, '|', C_LINE, 0);
            if (!lg_qr_col(v, qx, cy + 2, qw, ch - 4)) g_ctext(qx, cy + ch / 2, qw, TR("QR-код не влезает: растяните окно"), C_MUTED, 0);
        }
        break;
    case LS_QR:
        if (!lg_qr_col(v, lx, cy + 1, lw, ch - 4)) lg_para(lx, y + 2, lw, TR("QR-код не влезает: растяните окно (в терминале нужно от 86x45 клеток) или уменьшите шрифт."), C_MUTED, true);
        lg_btn(lx, cy + ch - 2, 0, TR("Назад (Esc)"), 0, B_LG_BACK);
        break;
    case LS_MFA: case LS_PHONE: {
        int kind = scr == LS_PHONE ? 1 : v->mfa_method == LM_SMS ? 1 : v->mfa_method == LM_BACKUP ? 2 : 0;
        static const char *const ttl[3] = { "Введите код из приложения аутентификации.", "Введите код из SMS.", "Введите один из резервных кодов (8 символов)." };
        char sm[120];
        g_ctext(cx, y++, cw, scr == LS_PHONE ? TR("Подтвердите новое место входа") : TR("Многофакторная аутентификация"), C_WHITE, AT_B);
        if (kind == 1 && v->sms_sent) { snprintf(sm, sizeof sm, TR("Код отправлен на %s."), v->phone && *v->phone ? v->phone : TR("ваш телефон")); y += lg_para(lx, y, lw, sm, C_MUTED, true); }
        else y += lg_para(lx, y, lw, tr(ttl[kind]), C_MUTED, true);
        if (gerr) g_texte(lx, y, gerr, lw, C_RED, 0);
        y++;
        lg_label(lx, y++, lw, kind == 2 ? TR("Резервный код") : TR("Код (6 цифр)"), v->err_code);
        lg_field(lx, y++, lw, &LG.code, LFI_CODE, 0, kind == 2 ? "xxxx-xxxx" : "123456");
        y++;
        lg_btn(lx, y, lw, busyst ? (v->busy_text && *v->busy_text ? v->busy_text : TR("Проверка кода...")) : TR("Подтвердить"), busyst ? 5 : 1, B_LG_CODE);
        y += 2;
        if (scr == LS_MFA) {
            if ((v->mfa_methods & LM_SMS) && kind != 1) lg_btn(lx, y++, lw, TR("Отправить код по SMS"), 4, B_LG_SMS);
            if (kind == 1 && v->sms_sent) lg_btn(lx, y++, lw, TR("Отправить SMS ещё раз"), 4, B_LG_SMSAGAIN);
            if ((v->mfa_methods & LM_BACKUP) && kind != 2) lg_btn(lx, y++, lw, TR("Использовать резервный код"), 4, B_LG_BACKUP);
            if ((v->mfa_methods & LM_TOTP) && kind != 0) lg_btn(lx, y++, lw, TR("Код из приложения аутентификации"), 4, B_LG_TOTP);
        }
        lg_btn(lx, y++, lw, TR("Назад к входу"), 4, B_LG_BACK);
        break;
    }
    case LS_EMAIL:
        g_ctext(cx, y++, cw, TR("Проверьте почту"), C_WHITE, AT_B);
        g_ctext(cx, y++, cw, TR("Подтвердите новое место входа"), C_TEXT, AT_B);
        y++;
        y += lg_para(lx, y, lw, v->notice && *v->notice ? v->notice : TR("Discord заметил вход из нового места и отправил письмо на вашу почту. Откройте его, нажмите \"Подтвердить вход\", затем вернитесь сюда и нажмите \"Повторить\"."), C_MUTED, true) + 1;
        if (v->err_code && *v->err_code) lg_label(lx, y++, lw, TR("Ссылка из письма"), v->err_code);
        else g_text(lx, y++, TR("Или вставьте ссылку из письма"), lw, C_TEXT, AT_B);   /* необязательное: без «*» */
        lg_field(lx, y++, lw, &LG.code, LFI_CODE, 0, "https://discord.com/...");
        y++;
        lg_btn(lx, y, lw, busyst ? TR("Вход...") : TR("Повторить"), busyst ? 5 : 1, B_LG_RETRY);
        y += 2;
        lg_btn(lx, y, lw, TR("Назад к входу"), 4, B_LG_BACK);
        break;
    case LS_UNDEL:
        g_ctext(cx, y++, cw, TR("Аккаунт отключён"), C_WHITE, AT_B);
        y++;
        y += lg_para(lx, y, lw, v->notice && *v->notice ? v->notice : TR("Аккаунт отключён или помечен к удалению."), C_TEXT, true) + 1;
        lg_btn(lx, y, lw, TR("Восстановить и войти"), busyst ? 5 : 1, B_LG_UNDEL);
        y += 2;
        lg_btn(lx, y, lw, TR("Назад к входу"), 4, B_LG_BACK);
        break;
    case LS_QRSCAN: {
        g_ctext(cx, y++, cw, TR("Проверьте телефон"), C_WHITE, AT_B);
        y++;
        int ax = cx + (cw - 6) / 2;
        u64 uid = v->qr_user_id;
        av_ovr_id = uid;                         /* аватар этого пользователя — по хэшу из ответа шлюза QR */
        snprintf(av_ovr_hash, sizeof av_ovr_hash, "%s", v->qr_avatar ? v->qr_avatar : "");
        for (int r = 0; r < 3; r++) { g_avatar(ax, y + r, 6, 3, r, uid, NULL); strip_part(G_LOGIN, ax, y + r, 6, 3, r, PK_AVR, uid); }
        y += 4;
        g_ctext(cx, y++, cw, v->qr_user_name && *v->qr_user_name ? v->qr_user_name : "?", C_WHITE, AT_B);
        int cn = lg_captcha_ui(v, lx, y, lw, true);
        if (cn) y += cn + 1;
        else if (v->qr_text && *v->qr_text) y += lg_para(lx, y, lw, v->qr_text, C_GOLD, true) + 1;
        else y += lg_para(lx, y, lw, TR("Подтвердите вход в мобильном приложении Discord."), C_MUTED, true) + 1;
        int u = g_text(lx + (lw - 26) / 2, y, TR("Не вы? "), lw, C_MUTED, 0);
        lg_btn(lx + (lw - 26) / 2 + u, y, lw - u, TR("Начать заново"), 4, B_LG_RESTART);
        break;
    }
    default: break;
    }
    if (LG.want_err_focus) {                     /* как в Discord: курсор — в поле с ошибкой */
        LG.want_err_focus = false;
        int fe = scr == LS_FORM && v->err_login && *v->err_login ? LFI_USER : scr == LS_FORM && v->err_pass && *v->err_pass ? LFI_PASS :
                   v->err_code && *v->err_code ? LFI_CODE : 0;
        for (int i = 0; fe && i < LG.nitems; i++) if (LG.items[i] == fe) LG.foc = i;
    }
    if (LG.foc >= LG.nitems) LG.foc = LG.nitems ? LG.nitems - 1 : 0;
    /* под карточкой: проверка/ожидание, уведомление или подсказка клавиш */
    const char *hint = U.busy[0] ? U.busy : busyst && v->busy_text && *v->busy_text ? v->busy_text :
                       v->route_wait ? TR("ожидание VLESS: туннель не отвечает") : LG.note[0] ? LG.note :
                       scr == LS_VLESS ? (W < 64 ? TR("Tab - выбор  Enter - нажать  Ctrl+Q - выход") : TR("Tab - поле или кнопка  Enter/пробел - нажать  Ctrl+Q - выход")) :
                       TR("Tab - следующее поле  Enter - далее  Esc - назад  Ctrl+Q - выход");
    bool loud = hint != NULL && strncmp(hint, "Tab", 3);
    char hd[220];
    deco_ascii(hint, hd, sizeof hd);
    if (cy + ch < H) g_ctext(0, cy + ch < H - 1 ? cy + ch : H - 1, W - 1, hd, loud ? C_WHITE : C_MUTED, loud ? AT_B : 0);
}

/* ---- действия ---- */
static Buf *lg_buf(int fid) {
    return fid == LFI_LINK ? &LG.link : fid == LFI_USER ? &LG.user : fid == LFI_PASS ? &LG.pass : fid == LFI_CODE ? &LG.code : NULL;
}
static void lg_go(int id) {
    const LoginView *v = lv();
    if (v->stage == LG_BUSY && id != B_LG_BACK && id != B_LG_QRSHOW) return;   /* запрос идёт: кнопки не активны */
    LG.note[0] = 0;
    switch (id) {
    case B_LG_VCHECK: {
        size_t a = 0, b = LG.link.len;
        while (a < b && LG.link.p[a] == ' ') a++;
        while (b > a && LG.link.p[b - 1] == ' ') b--;
        LG.err_link[0] = 0;
        if (b - a < 9 || strncmp(LG.link.p + a, "vless://", 8)) { snprintf(LG.err_link, sizeof LG.err_link, TR("ссылка должна начинаться с vless://")); return; }
        if (O.demo) snprintf(LG.note, sizeof LG.note, TR("VLESS работает, 85 мс (демо)"));
        else {
            char *lk = xmalloc(b - a + 1), err[200] = "";
            memcpy(lk, LG.link.p + a, b - a);
            lk[b - a] = 0;
            int ms = -1;
            busy(TR("проверка ссылки..."));
            bool ok = vless_set_link(lk, err, sizeof err, &ms);
            busy(NULL);
            wipe(lk, b - a);
            free(lk);
            if (!ok) { char *e = san(err[0] ? err : TR("туннель не отвечает"), false); snprintf(LG.err_link, sizeof LG.err_link, TR("не подошла: %s"), e); free(e); return; }
            snprintf(LG.note, sizeof LG.note, TR("VLESS работает, %d мс"), ms);
        }
        buf_wipe(&LG.link);
        startup_direct_set(false);               /* ссылка есть — «напрямую» больше не действует (пропадёт ссылка — вопрос) */
        LG.vless = false; LG.foc = 0; LG.verr[0] = 0;
        if (LG.vonly) { LG.on = false; U.full = U.dirty = true; }
        break;
    }
    case B_LG_VSKIP:
        buf_wipe(&LG.link); LG.err_link[0] = 0;
        if (!O.demo && vless_state() != VL_OFF) vless_disable();   /* иначе запросы ждали бы туннель вечно */
        if (LG.noask) startup_direct_set(true);   /* «больше не спрашивать» + «без VLESS» — всегда напрямую */
        LG.vless = false; LG.foc = 0; LG.verr[0] = 0;
        if (LG.vonly) { LG.on = false; U.full = U.dirty = true; }
        break;
    case B_LG_NOASK: LG.noask = !LG.noask; break;
    case B_LG_VQUIT: g_quit = true; break;
    case B_LG_VRETRY: {
        if (O.demo) { snprintf(LG.verr, sizeof LG.verr, TR("демо: повтор не помог (туннель не отвечает)")); break; }
        busy(TR("переподключение..."));
        bool ok = vless_restart();
        busy(NULL);
        if (ok) { LG.verr[0] = 0; LG.vless = false; if (LG.vonly) { LG.on = false; U.full = U.dirty = true; } }
        else { char *e = san(streamer_on() ? "" : vless_error(), false); snprintf(LG.verr, sizeof LG.verr, "%s", *e ? e : TR("туннель не отвечает")); free(e); }
        break;
    }
    case B_LG_LOGIN: {
        if (v->wait_until && now_ms() < v->wait_until) return;
        char *u = xstrdup(LG.user.p ? LG.user.p : ""), *p = xstrdup(LG.pass.p ? LG.pass.p : "");
        lgx_password(u, p);
        wipe(p, strlen(p));
        free(p);
        wipe(u, strlen(u));
        free(u);
        buf_wipe(&LG.pass);                      /* пароль ушёл движку — в ui.c его больше нет */
        LG.want_err_focus = true;                /* ошибка сразу (пустое поле) или после ответа */
        break;
    }
    case B_LG_FORGOT: snprintf(LG.note, sizeof LG.note, TR("восстановление пароля - на discord.com (в Astra DS нет)")); break;
    case B_LG_REGISTER: snprintf(LG.note, sizeof LG.note, TR("регистрация - на discord.com (в Astra DS нет)")); break;
    case B_LG_QRSHOW: case B_LG_QRGO: LG.qr_full = true; LG.foc = 0; break;
    case B_LG_BACK:
        if (LG.qr_full) LG.qr_full = false;
        else if (LG.scr == LS_FORM) LG.vless = true;
        else { buf_wipe(&LG.code); lgx_back(); }
        LG.foc = 0;
        break;
    case B_LG_CODE: case B_LG_RETRY: {
        if (id == B_LG_RETRY && !LG.code.len) { if (O.demo) lgx_retry(); else login_email_retry(); break; }
        int kind = v->stage == LG_MFA ? (v->mfa_method == LM_SMS ? 1 : v->mfa_method == LM_BACKUP ? 2 : 0) : 0;
        char *c = xstrdup(LG.code.p ? LG.code.p : "");
        lgx_code(c, kind);
        LG.want_err_focus = true;
        wipe(c, strlen(c));
        free(c);
        buf_wipe(&LG.code);
        break;
    }
    case B_LG_SMS: buf_wipe(&LG.code); lgx_mfa_method(LM_SMS); LG.foc = 0; break;
    case B_LG_SMSAGAIN: lgx_sms(); break;
    case B_LG_BACKUP: buf_wipe(&LG.code); lgx_mfa_method(LM_BACKUP); LG.foc = 0; break;
    case B_LG_TOTP: buf_wipe(&LG.code); lgx_mfa_method(LM_TOTP); LG.foc = 0; break;
    case B_LG_UNDEL: lgx_undelete(); break;
    case B_LG_RESTART: case B_LG_QRNEW: lgx_qr_start(); LG.foc = 0; break;
    case B_LG_CAPOPEN: lgx_captcha(true); break;
    case B_LG_CAPCANCEL: lgx_captcha(false); break;
    case B_LG_QRDEMO:
        if (O.demo) {
            dv.qr = QR_SCANNED; dv.qr_user_id = 1004; dv.qr_user_name = user_name(1004);
#ifdef LOGIN_CAPTCHA_UI
            /* демо: логин с «qrcap» — Discord требует проверку и для входа по QR */
            if (LG.user.p && strstr(LG.user.p, "qrcap")) { dv.captcha = dv.captcha_qr = dv.captcha_supported = true; dv.captcha_open = false; }
#endif
        }
        break;
    default: break;
    }
}
static int lg_primary(void) {
    switch (LG.scr) {
    case LS_VLESS: return B_LG_VCHECK;
    case LS_FORM: return B_LG_LOGIN;
    case LS_MFA: case LS_PHONE: return B_LG_CODE;
    case LS_EMAIL: return B_LG_RETRY;
    case LS_CAPTCHA: return B_LG_QRGO;
    case LS_UNDEL: return B_LG_UNDEL;
    default: return 0;
    }
}
static void login_key(const Key *k) {
    U.dirty = true;
    int it = LG.foc >= 0 && LG.foc < LG.nitems ? LG.items[LG.foc] : 0;
    if (k->code == K_TAB || k->code == K_DOWN) { if (LG.nitems) LG.foc = (LG.foc + 1) % LG.nitems; return; }
    if (k->code == K_BTAB || k->code == K_UP) { if (LG.nitems) LG.foc = (LG.foc + LG.nitems - 1) % LG.nitems; return; }
    if (k->code == K_ESC) { if (LG.scr != LS_VLESS) lg_go(B_LG_BACK); return; }
    if (k->code == K_CHAR && k->ch == ' ' && it >= 1000) { lg_go(it - 1000); return; }   /* пробел — кнопка/галочка */
    if (k->code == K_ENTER) {
        if (it >= 1000) lg_go(it - 1000);
        else if (lg_primary()) lg_go(lg_primary());
        return;
    }
    if (lv()->stage == LG_BUSY) return;          /* запрос идёт: ввод заблокирован */
    Buf *b = it > 0 && it < 1000 ? lg_buf(it) : NULL;
    if (!b) return;
    if (it == LFI_LINK) LG.err_link[0] = 0;      /* правка ссылки — прошлая ошибка проверки снимается */
    if (k->code == K_BS && b->len) { size_t i = prev_cp(b->p, b->len); wipe(b->p + i, b->len - i); b->len = i; }
    else if (k->code == K_CHAR && k->ctrl && k->ch == 'u') buf_wipe(b);
    else if (k->code == K_CHAR && !k->ctrl && !k->alt && k->ch >= 0x20 && k->ch != 0x7F) {
        char e[4];
        int n = utf8_enc(k->ch, e);
        if (b->len + (size_t)n < 512) buf_put(b, e, (size_t)n);
        const LoginView *v = lv();
        bool six = (LG.scr == LS_MFA && v->mfa_method != LM_BACKUP) || LG.scr == LS_PHONE;
        if (it == LFI_CODE && six && b->len == 6) lg_go(B_LG_CODE);   /* 6 цифр — сразу проверить */
    }
}
static void login_mouse(const Key *k) {
    const Hit *h = hit_item(k->x, k->y);
    if (!h || k->button != 1) return;
    if (h->act == A_INPUT && h->arg >= 1000) {
        for (int i = 0; i < LG.nitems; i++) if (LG.items[i] == h->arg - 1000) LG.foc = i;
    } else if (h->act == A_BTN && h->sub == B_LANG) {
        lang_set(!g_lang_en);
    } else if (h->act == A_BTN) {
        for (int i = 0; i < LG.nitems; i++) if (LG.items[i] == 1000 + h->sub) LG.foc = i;
        lg_go(h->sub);
    }
}
/* Начать вход в окне (main.c: вместо консольного ввода токена, после login_init()). vless_step — первым
 * шагом спросить ссылку VLESS. */
void ui_login_begin(bool vless_step) {
    LG.on = true;
    LG.vless = vless_step;
    LG.vonly = false;                            /* «только VLESS» ставит ui_vless_begin; обычный вход его не наследует */
    LG.qr_full = LG.qr_req = false;
    LG.scr = vless_step ? LS_VLESS : LS_FORM;
    LG.foc = 0;
    if (O.demo) { memset(&dv, 0, sizeof dv); dv_reset_errs(); dv.stage = LG_FORM; dv.qr = QR_OFF; }
    U.full = U.dirty = true;
}
bool ui_login_active(void) { return LG.on; }
/* Стартовый экран Astra DS (токен уже есть): только шаг VLESS — ссылка, «Больше не спрашивать», «Подключить» /
 * «Без VLESS»; err — сохранённая ссылка не работает (экран ошибки: повторить / другая ссылка / напрямую / выход). */
void ui_vless_begin(const char *err, bool only) {
    ui_login_begin(true);
    LG.vonly = only;                             /* only — потом сразу клиент; иначе дальше форма входа */
    snprintf(LG.verr, sizeof LG.verr, "%s", err ? err : "");
}
/* main.c: нужен ли вопрос о VLESS при запуске (есть сохранённое «больше не спрашивать» — нет). */
bool ui_startup_direct(void) { return startup_direct(); }
void ui_startup_direct_set(bool on) { startup_direct_set(on); }

/* Слои мелкой сетки (только окно). Кандидаты: превью, QR, участки групп картинок; уходят MAX_SEND с
 * наибольшим приоритетом в пределах SEND_BYTES. Картинки для отправляемых слоёв запрашиваются у img.c в
 * каждом кадре (кэша нет: что не запросили — забыто); слой уходит в поток, только если он новый, сдвинулся
 * или его строки перерисованы (окно его сняло). Картинка ещё качается — слой не шлём (видны обычные клетки). */
#define FINE_MAXW (FINE_K * 48)
#define FINE_MAXH (FINE_K * 40)
typedef struct { int oi, x, y, w, h, prio; } Cand;
/* Клетка (x, y) под картинкой части слоя? */
static bool part_covers(const Ovl *o, int x, int y) {
    for (int q = 0; q < o->np; q++) {
        const Part *pt = &o->p[q];
        if (x >= pt->x && x < pt->x + pt->w && y >= pt->y && y < pt->y + pt->nr) return true;
    }
    return false;
}
/* Вне картинок в строке только фон и линии — строку можно накрыть слоем. */
static bool row_clean(const Ovl *o, int x0, int x1, int y) {
    if (y < 0 || y >= U.gh) return false;
    for (int x = x0; x < x1; x++) {
        if (x < 0 || x >= U.gw) return false;
        u32 c = GC(x, y).cp;
        if (c != ' ' && c != '-' && c != '_' && !part_covers(o, x, y)) return false;
    }
    return true;
}
/* Слой пишется в окно кусками (FINE_CHUNK) по мере сборки строк мелкой сетки: ни целого слоя, ни целого
 * кадра со слоями в памяти нет (раньше — сетка big 215 КБ и буфер кадра до 512 КБ). */
#define FINE_CHUNK (16u << 10)
static void fine_put(Buf *c, const void *p, size_t n) {
    buf_put(c, p, n);
    if (c->len >= FINE_CHUNK) { term_write(c->p, c->len); c->len = 0; }
}
static void fine_cell(char *e, u32 ch, u32 fg, u32 bg) {
    static const char hx[] = "0123456789abcdef";
    e[0] = ch >= 0x20 && ch < 0x7F ? (char)ch : ' ';
    for (int k = 0; k < 3; k++) {
        int s = 16 - 8 * k;
        e[1 + 2 * k] = hx[fg >> (s + 4) & 15]; e[2 + 2 * k] = hx[fg >> s & 15];
        e[7 + 2 * k] = hx[bg >> (s + 4) & 15]; e[8 + 2 * k] = hx[bg >> s & 15];
    }
}
/* Готовые мелкие картинки аватаров и значков для слоёв: до FC_N последних (каждая <= 24x12 мелких клеток, ~2 КБ),
 * выделяются по мере нужды. Наведение мыши перекрашивает строку ленты, и слой аватаров уходит заново —
 * без этого запаса круглая маска (а в демо и синтетика) пересчитывалась бы на каждое движение мыши. */
#define FC_N 24
static struct { u64 id; u32 under; short w, h; u8 kind; unsigned age; ImgCell *px; } fcache[FC_N];
static unsigned fc_clock;
static bool fine_pic(int kind, u64 id, int pw, int ph, u32 under, ImgCell *dst) {
    size_t n = (size_t)pw * (size_t)ph;
    int lru = 0;
    for (int i = 0; i < FC_N; i++) {
        if (fcache[i].px && fcache[i].id == id && fcache[i].kind == kind && fcache[i].w == pw && fcache[i].h == ph &&
            (kind != PK_AVR || fcache[i].under == under)) {
            fcache[i].age = ++fc_clock;
            memcpy(dst, fcache[i].px, n * sizeof(ImgCell));
            return true;
        }
        if (fcache[i].age < fcache[lru].age) lru = i;
    }
    bool ok = kind == PK_AVR ? pic_avatar(id, pw, ph, true, under, dst) : pic_icon(guild_find(id), pw, ph, dst);
    if (!ok || n > 24 * 12 * 2) return ok;      /* качается — не запоминаем; крупное (логотип) — тоже */
    if (!fcache[lru].px || (size_t)fcache[lru].w * (size_t)fcache[lru].h < n) { free(fcache[lru].px); fcache[lru].px = xmalloc(n * sizeof(ImgCell)); }
    memcpy(fcache[lru].px, dst, n * sizeof(ImgCell));
    fcache[lru].id = id; fcache[lru].kind = (u8)kind; fcache[lru].w = (short)pw; fcache[lru].h = (short)ph;
    fcache[lru].under = under; fcache[lru].age = ++fc_clock;
    return true;
}
static void fine_cache_drop(void) { for (int i = 0; i < FC_N; i++) { free(fcache[i].px); fcache[i].px = NULL; fcache[i].age = 0; } }
/* c — буфер для кусков потока (сам кадр уже выведен). */
static bool sync_open;                           /* ?2026h уже выведен в этом кадре */
static void sync_begin(void) { if (!sync_open) { term_write("\x1b[?2026h", 8); sync_open = true; } }
static void emit_fine(Buf *c) {
    u32 sent[MAX_SEND];
    int ns = 0;
    int K = term_subcell_scale();
    if (K > FINE_K) K = FINE_K;
    if (!term_fine_images() || K < 2) { nsent = 0; return; }
    int maxr = FINE_MAXH / K, maxc = FINE_MAXW / K;   /* участок больше — режется на несколько слоёв */
    /* кандидаты */
    Cand cd[32];
    int nc = 0;
    for (int i = 0; i < novl && nc < 32; i++) {
        Ovl *o = &ovl[i];
        if (o->kind != OV_STRIP) {
            if (o->w > 0) cd[nc++] = (Cand){ i, o->x, o->y, o->w, o->kind == OV_ATT ? o->r1 - o->r0 + 1 : o->h, o->prio };
            continue;
        }
        if (!o->np) continue;
        int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
        for (int q = 0; q < o->np; q++) {
            const Part *pt = &o->p[q];
            if (pt->x < x0) x0 = pt->x;
            if (pt->x + pt->w > x1) x1 = pt->x + pt->w;
            if (pt->y < y0) y0 = pt->y;
            if (pt->y + pt->nr > y1) y1 = pt->y + pt->nr;
        }
        for (int y = y0; y < y1 && nc < 32; ) {
            if (!row_clean(o, x0, x1, y)) { y++; continue; }
            int e = y;
            while (e < y1 && e - y < maxr && row_clean(o, x0, x1, e)) e++;
            for (int xa = x0; xa < x1 && nc < 32; xa += maxc) {
                int xb = xa + maxc < x1 ? xa + maxc : x1;
                bool any = false;                /* в участке есть картинка */
                for (int q = 0; q < o->np && !any; q++)
                    if (o->p[q].y < e && o->p[q].y + o->p[q].nr > y && o->p[q].x < xb && o->p[q].x + o->p[q].w > xa) any = true;
                if (any) cd[nc++] = (Cand){ i, xa, y, xb - xa, e - y, o->prio };
            }
            y = e;
        }
    }
    for (int i = 1; i < nc; i++)                 /* по приоритету, порядок кадра сохраняется */
        for (int j = i; j > 0 && cd[j - 1].prio < cd[j].prio; j--) { Cand t = cd[j]; cd[j] = cd[j - 1]; cd[j - 1] = t; }
    size_t bytes = 0;
    for (int ci = 0; ci < nc && ns < MAX_SEND; ci++) {
        Cand *cn = &cd[ci];
        Ovl *o = &ovl[cn->oi];
        int fw = K * cn->w, fh = K * cn->h;
        if (fw > FINE_MAXW || fh > FINE_MAXH || cn->h <= 0 || cn->w <= 0) continue;
        size_t need = (size_t)fw * (size_t)fh * 7;
        if (bytes + need > SEND_BYTES) continue;
        /* подпись: место и содержимое */
        u32 key;
        {
            char sig[160];
            snprintf(sig, sizeof sig, "%d;%d;%d;%d;%d;%d;%d;", o->kind, K, cn->x, cn->y, cn->w, cn->h, o->r0);
            key = fnv(sig, strlen(sig));
            if (o->url) { key ^= fnv(o->url, strlen(o->url)); key *= 16777619u; }
            for (int q = 0; q < o->np; q++) {
                const Part *pt = &o->p[q];
                if (pt->y + pt->nr <= cn->y || pt->y >= cn->y + cn->h || pt->x + pt->w <= cn->x || pt->x >= cn->x + cn->w) continue;
                key ^= (u32)pt->id * 2654435761u + (u32)(pt->x * 131 + pt->y * 31 + pt->r0 * 7 + pt->nr);
                key *= 16777619u;
            }
        }
        bool dirty = true;
        for (int k = 0; k < nsent; k++) if (ovl_sent[k] == key) dirty = false;
        for (int y = cn->y; y < cn->y + cn->h && !dirty; y++) if (span_dirty(y, cn->x, cn->x + cn->w)) dirty = true;
        if (!dirty) { bytes += need; sent[ns++] = key; continue; }   /* окно держит его: ни картинок, ни потока */
        /* картинки слоя: превью — готовая сетка img.c; полоса — картинки её частей подряд в одном блоке
         * (только то, что попало в участок: аватары 16x8, значки 24x12 мелких клеток), блок — на этот слой */
        /* превью: в живом режиме — клетки прямо из кэша img.c (img_attachment_peek, до конца кадра), в демо —
         * синтетика в буфер на этот слой; статического буфера (57 КБ) больше нет */
        const ImgCell *att = NULL;
        int ow = 0, oh = 0, nm = 0, msh = 0, ox = 0, oy = 0;
        int poff[MAX_PART];
        ImgCell *pool = NULL;
        bool ok = true;
        if (o->kind == OV_QR) {
            nm = qr_n + 8; msh = fh / nm;
            if (msh < 1) continue;
            ox = (fw - nm * 2 * msh) / 2; oy = (fh - nm * msh) / 2;
        } else if (o->kind == OV_ATT) {
            if (!o->url || fw > FINE_K * 32 || K * o->ph > FINE_K * 16) ok = false;
            else if (O.demo) {
                pool = xmalloc((size_t)fw * (size_t)(K * o->ph) * sizeof(ImgCell));
                if (!pic_attach(o->url, fw, K * o->ph, pool, &ow, &oh)) ok = false;
                att = pool;
            } else att = img_attachment_peek(o->url, 0, 0, fw, K * o->ph, &ow, &oh);
            if (!att || ow > fw) ok = false;
        } else {
            size_t tot = 0;
            for (int q = 0; q < o->np; q++) {
                const Part *pt = &o->p[q];
                bool in = pt->y < cn->y + cn->h && pt->y + pt->nr > cn->y && pt->x < cn->x + cn->w && pt->x + pt->w > cn->x;
                poff[q] = in ? (int)tot : -1;
                if (in) tot += (size_t)(K * pt->w) * (size_t)(K * pt->ih);
            }
            pool = tot ? xmalloc(tot * sizeof(ImgCell)) : NULL;
            for (int q = 0; q < o->np && ok; q++) {
                if (poff[q] < 0) continue;
                const Part *pt = &o->p[q];
                int pw = K * pt->w, ph = K * pt->ih;
                u32 under = pt->x < U.gw && pt->y < U.gh ? GC(pt->x, pt->y).bg : C_FEED;
                ImgCell *dst = pool + poff[q];
                ok = fine_pic(pt->kind, pt->id, pw, ph, under, dst);
            }
        }
        if (!ok) { free(pool); continue; }
        bytes += need;
        sent[ns++] = key;
        if (!dirty) { free(pool); continue; }
        char hdr[96];
        int hn = snprintf(hdr, sizeof hdr, "\x1b_astra-img;%d;%d;%d;%d;%d;", cn->x, cn->y, cn->w, cn->h, K);
        sync_begin();
        fine_put(c, hdr, (size_t)hn);
        char e[13];
        for (int fy = 0; fy < fh; fy++) {
            int cy = cn->y + fy / K;
            for (int fx = 0; fx < fw; fx++) {
                int cx = cn->x + fx / K;
                if (o->kind == OV_QR) {
                    int mx = fx - ox, my = fy - oy;
                    bool d = mx >= 0 && my >= 0 && mx < nm * 2 * msh && my < nm * msh && qr_dark(mx / (2 * msh), my / msh);
                    u32 col = d ? QR_DARK : QR_LIGHT;
                    fine_cell(e, ' ', col, col);
                    fine_put(c, e, 13);
                    continue;
                }
                const ImgCell *px = NULL;
                if (o->kind == OV_ATT) {
                    int sy = o->r0 * K + fy;
                    if (sy < oh && fx < ow) px = &att[sy * ow + fx];
                } else {
                    for (int q = o->np - 1; q >= 0 && !px; q--) {
                        const Part *pt = &o->p[q];
                        if (poff[q] < 0 || cy < pt->y || cy >= pt->y + pt->nr) continue;
                        int lx = (cn->x - pt->x) * K + fx, pw = K * pt->w;
                        if (lx < 0 || lx >= pw) continue;
                        int sy = (pt->r0 + cy - pt->y) * K + fy % K;
                        if (sy >= K * pt->ih) continue;
                        const ImgCell *s = &pool[poff[q] + sy * pw + lx];
                        if (s->ch) px = s;
                    }
                }
                if (px) fine_cell(e, (u32)(unsigned char)px->ch, rgb3(px->fg), rgb3(px->bg));
                else {
                    /* фон: клетка под слоем (линии '-' '_' — тонкой чертой мелкого шрифта) */
                    Cell ce = cx < U.gw && cy < U.gh ? GC(cx, cy) : (Cell){ ' ', C_TEXT, C_FEED };
                    char ch = (ce.cp == '-' && fy % K == K / 2) ? '-' : (ce.cp == '_' && fy % K == K - 1) ? '_' : ' ';
                    fine_cell(e, (u32)ch, RGB_OF(ce.fg), ce.bg);
                }
                fine_put(c, e, 13);
            }
        }
        fine_put(c, "\x1b\\", 2);
        free(pool);
    }
    if (c->len) { term_write(c->p, c->len); c->len = 0; }
    memcpy(ovl_sent, sent, sizeof(u32) * (size_t)ns);
    nsent = ns;
}

/* Discord просит проверку (капча) для запроса (net.c держит запрос, пока пользователь не решит): окно поверх
 * всего экрана. Enter / «Пройти проверку» — окно hCaptcha (captcha.c), Esc / «Отмена» — отказ (запрос получит 400). */
static bool cap_cancelled;                       /* проверку отменил пользователь (Esc / «Отмена») */
static void draw_captcha(int W, int H) {
    const CaptchaState *cs = net_captcha();
    if (!cs || !cs->pending) return;
    char l1[160], er[200] = "";
    const char *ln[5];
    int n = 0, nb = 0;
    CBtn b[2];
    snprintf(l1, sizeof l1, TR("Discord просит проверку: %s"), cs->what[0] ? cs->what : TR("запрос к Discord"));
    ln[n++] = TR("Подтвердите, что вы человек");
    ln[n++] = l1;
    if (cs->error[0]) { char *e = san(cs->error, false); deco_ascii(e, er, sizeof er); free(e); ln[n++] = er; }
    if (cs->open) ln[n++] = TR("Окно проверки открыто - пройдите её там");
    else {
        ln[n++] = TR("запрос подождёт: после проверки он уйдёт сам");
        b[nb++] = (CBtn){ TR("Отмена (Esc)"), B_CAP_NO, 0 };
        if (cs->supported) b[nb++] = (CBtn){ TR("Пройти проверку (Enter)"), B_CAP_GO, 1 };
    }
    ovl_cut(0, 0, W, H);
    hit_add(0, 0, W, H, A_BLOCK, 0, 0, 0);
    g_card(0, 0, W, H, -1, ln, n, b, nb, C_GOLD, true);
}
static void render(void) {
    int W, H;
    if (snap_out) { W = snap_w; H = snap_h; } else term_size(&W, &H);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (W != U.W || H != U.H) { U.W = W; U.H = H; U.full = true; }
    if (U.full || snap_out) {
        free(U.hash);
        U.hash = xcalloc((size_t)H, sizeof(u32));
        U.hash_n = H;
        free(seg_hash);
        seg_hash = xcalloc((size_t)H * NSEG, sizeof(u32));   /* 0 — «не было»: хэш участка всегда | 1 */
        seg_n = H * NSEG;
        if (!snap_out) term_write("\x1b[0m\x1b[2J", 8);
        U.full = false;
    }
    grid_ensure(W, H);
    U.nhit = 0;
    novl = 0;
    for (int i = 0; i < n_lkt; i++) free(lkt[i].url);
    n_ivh = n_cbt = n_lkt = 0;
    MDC.s = NULL;
    cm_frame(W, H);
    sel_norm();
    frame_no++;
    fine_on = !snap_out && term_fine_images() && term_subcell_scale() >= 2;
    memset(row_emitted, 0, sizeof row_emitted);
    img_frame();                                 /* качается только видимое */
    if (W < 50 || H < 12) draw_small(W, H);
    else {
        if (LG.on) draw_login(W, H);
        else if (U.help) { draw_help(W, H); draw_status(0, W, H - 1); }
        else if (U.settings) draw_settings(W, H);
        else draw_main(W, H);
        if (!LG.on) { if (tm_on) draw_tmembers(W, H); if (rules_on) draw_rules(W, H); if (upd_on) draw_update(W, H); if (AVB.uid) draw_avbig(W, H); else if (IV.on && !U.help && !U.settings) draw_iv(W, H); if (CSEL.on && !U.help && !U.settings) draw_comp_sel(W, H); if (reactors_state() && reactors_state()->on) draw_rv(W, H); draw_form(W, H); draw_captcha(W, H); }
        hover_paint();
    }
    Buf *f = &U.frame;
    f->len = 0;
    for (int y = 0; y < H; y++) {
        U.line.len = 0;
        int lim = y == H - 1 ? W - 1 : W;
        if (snap_out || seg_n != H * NSEG) { g_line_out(&U.line, y, lim); emit_line(f, &U.line, y); continue; }
        u8 ch = 0;
        for (int sg = 0; sg < NSEG; sg++) {
            int a = seg_x(sg, W), b = seg_x(sg + 1, W);
            u32 h = fnv((const char *)&GC(a, y), (size_t)(b - a) * sizeof(Cell)) | 1;
            if (seg_hash[y * NSEG + sg] != h) { seg_hash[y * NSEG + sg] = h; ch |= (u8)(1 << sg); }
        }
        if (y < (int)sizeof row_emitted) row_emitted[y] = ch;
        for (int sg = 0; sg < NSEG; ) {
            if (!(ch >> sg & 1)) { sg++; continue; }
            int e = sg;
            while (e < NSEG && (ch >> e & 1)) e++;
            int x0 = seg_x(sg, W), x1 = seg_x(e, W);
            while (x0 > 0 && !GC(x0, y).cp) x0--;          /* правая половина широкого символа — с левой */
            if (x1 < W && x1 > x0 && !GC(x1, y).cp) x1++;  /* широкий символ на границе — целиком */
            if (x1 > lim) x1 = lim;
            buf_printf(f, "\x1b[%d;%dH", y + 1, x0 + 1);
            g_line_out_from(f, y, x0, x1);
            sg = e;
        }
    }
    if (U.line.cap > (16u << 10)) buf_free(&U.line);
    if (U.mlcap > 4096) { free(U.ml); U.ml = NULL; U.mlcap = 0; }
    if (!snap_out) {
        /* синхронный вывод: строки кадра, затем слои мелкой сетки кусками (буфер кадра — под куски) */
        sync_open = false;
        if (f->len) { sync_begin(); term_write(f->p, f->len); }
        f->len = 0;
        emit_fine(f);
        if (sync_open) term_write("\x1b[?2026l", 8);
        if (f->cap > (64u << 10)) buf_free(f);
    }
    for (int i = 0; i < novl; i++) { free(ovl[i].url); ovl[i].url = NULL; }
    novl = 0;
}

void ui_snapshot(int w, int h, Buf *out) {
    snap_w = w;
    snap_h = h;
    snap_out = out;
    rebuild_rows();
    render();
    snap_out = NULL;
    U.full = true;
}
void ui_feed_key(const Key *k) { handle_key(k); }

bool g_headless;                                 /* проверки без терминала: не рисуем */

void ui_render_if_dirty(void) {
    if (O.cli) return;
    if (!U.dirty || g_headless) return;
    U.dirty = false;
    rebuild_rows();                              /* дёшево: каналы одного сервера */
    render();
}

/* ======================= ввод ======================= */
/* Исходный текст своего сообщения для правки: Message.raw от store (как пришёл с сервера: <@id>, без
 * «[файл: ...]» и встроек). Без него — content, только если в нём нет строк вложений/встроек (иначе NULL). */
static const char *edit_src(const Message *m) {
#ifdef MC_RAW
    return m->raw;
#else
    const char *c = m->content ? m->content : "";
    if (strchr(c, ATT_MARK) || strstr(c, "[встройка]") || strstr(c, "[стикер]")) return NULL;
    return c;
#endif
}
/* Исходный текст -> вид для поля правки: <@id>/<@!id> -> @Имя, <#id> -> #канал (остальное как есть). */
static void edit_set(const char *raw) {
    Buf b = {0};
    for (const char *p = raw; *p; ) {
        if (p[0] == '<' && (p[1] == '@' || p[1] == '#')) {
            const char *q = p + 2;
            if (*q == '!') q++;
            u64 id = 0;
            const char *d = q;
            while (*q >= '0' && *q <= '9') { id = id * 10 + (u64)(*q - '0'); q++; }
            if (q > d && *q == '>') {
                const char *nm = NULL;
                if (p[1] == '@') { nm = id == S.me_id && S.me_name ? S.me_name : user_name(id); }
                else { Channel *c = chan_find(id); nm = c ? c->name : NULL; }
                if (nm && *nm) { buf_printf(&b, "%c%s", p[1], nm); p = q + 1; continue; }
            }
        }
        buf_put(&b, p, 1);
        p++;
    }
    in_set(b.p ? b.p : "");
    buf_wipe(&b);
}
static Message *last_own(void) {
#ifdef MC_PACKA
    for (int i = S.nmsgs - 1; i >= 0; i--) if (S.msgs[i].author_id == S.me_id && S.msgs[i].state == MS_SENT) return &S.msgs[i];
#else
    for (int i = S.nmsgs - 1; i >= 0; i--) if (S.msgs[i].author_id == S.me_id) return &S.msgs[i];
#endif
    return NULL;
}
static void select_guild(int idx) {
    if (idx < 0) idx = 0;
    if (idx > S.nguilds) idx = S.nguilds;
    if (idx != U.sel_guild) { search_off(); U.pick = false; }   /* результаты поиска — по прошлому серверу */
    U.sel_guild = idx;
    U.sel_row = -1;
    U.row_scroll = 0;
    U.row_free = U.guild_free = false;
    rebuild_rows();
}
static void move_row(int d) {
    if (!U.nrows) return;
    int i = U.sel_row;
    for (int k = 0; k < U.nrows; k++) {
        i += d;
        if (i < 0 || i >= U.nrows) return;
        if (U.rows[i].kind != R_GAP && U.rows[i].kind != R_VSTAT) { U.sel_row = i; U.row_free = false; return; }
    }
}
static void join_voice(const Channel *c) {
    VoiceStatus vs;
    voice_status(&vs);
    if (vs.phase != VS_IDLE && vs.channel_id == c->id) { ui_notice(TR("вы уже в этом канале; F7 - отключиться")); return; }
    if (!c->can_connect) { ui_notice(TR("нет права подключаться к этому каналу")); return; }
    voice_join(c->guild_id, c->id);
}
static void activate_row(void) {
    if (U.sel_row < 0 || U.sel_row >= U.nrows) return;
    Row *r = &U.rows[U.sel_row];
    Channel *c = chan_find(r->id);
    if (!c) return;
    if (r->kind == R_HEADER) { fold_toggle(r->id); rebuild_rows(); return; }
    if (r->kind != R_CHAN) return;
    if (is_voice(c)) {
        /* как в Discord: сначала экран канала с кнопкой «Присоединиться» и его чатом; повторно - войти */
        if (S.open_ch == c->id) join_voice(c);
        else { int f = U.focus; open_channel(c->id); U.focus = f; }
        U.menu = false;
        return;
    }
    open_channel(c->id);
}

/* Alt+Up/Down: соседний канал списка (текстовый или беседа); с Shift — следующий непрочитанный. */
static void chan_step(int d, bool unread) {
    rebuild_rows();
    int cur = -1;
    for (int i = 0; i < U.nrows; i++) if (U.rows[i].kind == R_CHAN && U.rows[i].id == S.open_ch) cur = i;
    if (cur < 0) cur = d > 0 ? -1 : U.nrows;
    for (int i = cur + d; i >= 0 && i < U.nrows; i += d) {
        if (U.rows[i].kind != R_CHAN) continue;
        Channel *c = chan_find(U.rows[i].id);
        if (!c || is_voice(c) || (unread && !chan_unread(c) && !c->mentions)) continue;
        U.sel_row = i;
        U.row_free = false;
        open_channel(c->id);
        return;
    }
    if (unread) ui_notice(TR("непрочитанных каналов %s нет"), d > 0 ? TR("ниже") : TR("выше"));
}
static void submit(void) {
    if (U.mode == M_REACT) {
        if (U.in.len) react(U.target, U.in.p);
        in_clear();
        U.mode = M_NORMAL;
        U.focus = F_MESSAGES;
        return;
    }
    /* пустые строки по краям не шлём */
    size_t a = 0, b = U.in.len;
    while (a < b && (U.in.p[a] == ' ' || U.in.p[a] == '\n')) a++;
    while (b > a && (U.in.p[b - 1] == ' ' || U.in.p[b - 1] == '\n')) b--;
    if (a == b) {
        if (U.mode == M_EDIT) { U.mode = M_NORMAL; }
        return;
    }
    {
        const Channel *dc = chan_find(S.open_ch);
        if (dc && is_thread(dc) && (dc->tflags & TF_DELETED)) { ui_notice(TR("эту ветку удалили")); return; }
        if (dc && dc->guild_id && guild_find(dc->guild_id) && guild_find(dc->guild_id)->pending && U.mode != M_EDIT) { ui_notice(TR("сначала примите правила сервера")); return; }
    }
    if (U.mode != M_EDIT && S.open_ch) {
        SendLimit sl;
        send_limit(S.open_ch, &sl);
        if (sl.timeout_until || sl.slow_until) {
            char w[64];
            send_wait_text(sl.timeout_until ? sl.timeout_until : sl.slow_until, sl.timeout_until != 0, w, sizeof w);
            ui_notice(TR("%s - текст остался в поле"), w);
            return;
        }
    }
    char *text = xmalloc(b - a + 1);
    memcpy(text, U.in.p + a, b - a);
    text[b - a] = 0;
    send_message(text);
    wipe(text, b - a);
    free(text);
    in_clear();
    U.mode = M_NORMAL;
    U.target = 0;
    U.msg_scroll = 0;
}

static void call_dm(void) {
    Channel *c = chan_find(S.open_ch);
    if (U.focus == F_CHANNELS && U.sel_row >= 0 && U.sel_row < U.nrows) c = chan_find(U.rows[U.sel_row].id);
    if (!c || c->guild_id) { ui_notice(TR("звонок - из личной переписки")); return; }
    call_start(c->id);                          /* op 4, звонок собеседнику — после CALL_CREATE (store.c) */
}
static void voice_keys(const Key *k) {
    switch (k->code) {
    case K_F5: voice_toggle_mute(); break;
    case K_F6: voice_toggle_deaf(); break;
    case K_F7: voice_leave(); break;
    case K_F8: if (O.ptt) { static bool on; on = !on; voice_ptt(on); ui_notice(on ? TR("рация: передача") : TR("рация: молчание")); } else ui_notice(TR("рация включается флагом --ptt")); break;
    case K_F9: voice_vad_adjust(-3); ui_notice(TR("порог голоса: %d дБ"), voice_vad_db()); break;
    case K_F10: voice_vad_adjust(3); ui_notice(TR("порог голоса: %d дБ"), voice_vad_db()); break;
    case K_F2:
        if (S.call.ringing) { voice_join(0, S.call.channel_id); S.call.ringing = false; }
        break;
    case K_F3:
        if (S.call.ringing) { call_decline(S.call.channel_id); S.call.ringing = false; }
        break;
    case K_F4: call_dm(); break;
    default: return;
    }
    U.dirty = true;
}
/* Смена голоса по кругу: F11, Alt+V, кнопка «голос: ...». */
static void fx_cycle(void) {
    voicefx_cycle();
    VoiceFx fx = voicefx_get();
    VoiceStatus vs;
    voice_status(&vs);
    ui_notice(TR("смена голоса: %s%s"), fx == VFX_LOW ? TR("ниже") : fx == VFX_HIGH ? TR("выше") : TR("выкл, ваш голос как есть"),
              fx != VFX_OFF && vs.phase == VS_IDLE ? TR(" (включится в звонке)") : "");
}

/* ---- VLESS: окно «нет связи» ---- */
/* Блокирующий вызов (проверка туннеля до ~10 с): сначала показать, что идёт проверка. */
static void busy(const char *what) {
    snprintf(U.busy, sizeof U.busy, "%s", what ? what : "");
    U.dirty = true;
    ui_render_if_dirty();
}
static void vl_leave(void);
static void vl_enter(int mode) {
    if (U.mode == M_FILE || U.mode == M_VLINK) vl_leave();   /* черновик уже отложен: не затереть */
    U.prev_mode = U.mode;
    U.prev_target = U.target;
    U.prev_focus = U.focus;
    U.mode = mode;
    U.menu = false;
    if (mode == M_VLINK || mode == M_FILE) {
        /* недописанное сообщение откладываем целиком, строка ввода — только для ссылки (пути к файлу) */
        U.draft = U.in;
        U.draft_cur = U.cur;
        U.in = (Buf){0};
        U.cur = 0;
        U.focus = F_INPUT;
    }
}
static void vl_leave(void) {
    if (U.mode == M_VLINK || U.mode == M_FILE) {
        buf_wipe(&U.in);                         /* ссылка — секрет */
        U.in = U.draft;
        U.cur = U.draft_cur;
        U.draft = (Buf){0};
    }
    U.mode = U.prev_mode;
    U.target = U.prev_target;
    U.focus = U.prev_focus;
}
static void vl_restart(void) {
    busy(TR("переподключение..."));
    bool ok = vless_restart();
    busy(NULL);
    U.vl_note[0] = 0;
    if (ok) ui_notice(TR("VLESS снова работает, %d мс"), vless_latency_ms());
    else ui_notice(TR("VLESS: переподключиться не удалось"));
}
static void vl_submit_link(void) {
    size_t a = 0, b = U.in.len;
    while (a < b && (U.in.p[a] == ' ' || U.in.p[a] == '\t')) a++;
    while (b > a && (U.in.p[b - 1] == ' ' || U.in.p[b - 1] == '\t')) b--;
    if (a == b) { vl_leave(); return; }
    memmove(U.in.p, U.in.p + a, b - a);
    wipe(U.in.p + (b - a), U.in.len - (b - a));  /* хвост после сдвига — тоже куски ссылки */
    U.in.len = b - a;
    busy(TR("проверка ссылки..."));
    char err[200] = "";
    int ms = -1;
    bool ok = vless_set_link(U.in.p, err, sizeof err, &ms);
    busy(NULL);
    vl_leave();                                  /* затирает буфер со ссылкой */
    if (ok) { U.vl_note[0] = 0; startup_direct_set(false); ui_notice(TR("VLESS работает, %d мс"), ms); }
    else {
        char *e = san(err[0] ? err : TR("причина неизвестна"), false);
        snprintf(U.vl_note, sizeof U.vl_note, TR("ссылка не подошла: %s"), e);
        free(e);
        if (streamer_on()) ui_notice(TR("VLESS: ссылка не подошла (подробности скрыты)"));
        else ui_notice("VLESS: %s", U.vl_note);
    }
}
static void vl_off_answer(bool yes) {
    vl_leave();
    if (yes) { vless_disable(); U.vl_note[0] = 0; ui_notice(TR("VLESS отключён: трафик идёт напрямую")); }
}
/* R / C / D окна VLESS (и русская раскладка: к / с / в). true — нажатие съедено. */
static bool vl_card_key(const Key *k) {
    if (vless_state() != VL_DOWN || U.help || k->code != K_CHAR || k->ctrl || k->alt) return false;
    if (U.mode == M_DELETE || U.mode == M_VLINK || U.mode == M_VLOFF || U.mode == M_FILE || U.mode == M_UNFRIEND) return false;
    if (U.pick || U.sq_edit) return false;
    if (U.focus == F_INPUT && U.in.len) return false;   /* набирается текст - буквы идут в текст */
    u32 c = k->ch;
    if (c == 'r' || c == 'R' || c == 0x43A || c == 0x41A) vl_restart();
    else if (c == 'c' || c == 'C' || c == 0x441 || c == 0x421) vl_enter(M_VLINK);
    else if (c == 'd' || c == 'D' || c == 0x432 || c == 0x412) vl_enter(M_VLOFF);
    else return false;
    U.dirty = true;
    return true;
}

/* ---- фаза 4: файл, друзья, новая ЛС, поиск ---- */
/* «+» в поле ввода или Ctrl+O: строка ввода — путь к файлу, недописанный текст отложен и уйдёт подписью. */
static void file_enter(void) {
    Channel *c = chan_find(S.open_ch);
    if (!c) { ui_notice(TR("сначала откройте канал")); return; }
    if (c->guild_id && !c->can_send) { ui_notice(TR("нет права писать в этот канал")); return; }
    U.file_err[0] = 0;
    vl_enter(M_FILE);
}
static void file_submit(void) {
    size_t a = 0, b = U.in.len;
    while (a < b && (U.in.p[a] == ' ' || U.in.p[a] == '\t' || U.in.p[a] == '"' || U.in.p[a] == '\'')) a++;
    while (b > a && (U.in.p[b - 1] == ' ' || U.in.p[b - 1] == '\t' || U.in.p[b - 1] == '"' || U.in.p[b - 1] == '\'')) b--;
    if (a == b) { snprintf(U.file_err, sizeof U.file_err, TR("укажите путь к файлу")); return; }
    char path[1024], err[200] = "";
    if (b - a >= sizeof path) { snprintf(U.file_err, sizeof U.file_err, TR("слишком длинный путь")); return; }
    memcpy(path, U.in.p + a, b - a);
    path[b - a] = 0;
    /* подпись — отложенный черновик без пустых краёв */
    size_t ta = 0, tb = U.draft.len;
    while (ta < tb && (U.draft.p[ta] == ' ' || U.draft.p[ta] == '\n')) ta++;
    while (tb > ta && (U.draft.p[tb - 1] == ' ' || U.draft.p[tb - 1] == '\n')) tb--;
    char *text = xmalloc(tb - ta + 1);
    if (tb > ta) memcpy(text, U.draft.p + ta, tb - ta);
    text[tb - ta] = 0;
    bool ok = send_file_ex(S.open_ch, path, text, file_spoil, file_alt[0] ? file_alt : NULL, err, sizeof err);
    wipe(path, sizeof path);
    free(text);
    if (!ok) {
        char *e = san(err[0] ? err : TR("причина неизвестна"), false);
        snprintf(U.file_err, sizeof U.file_err, "%s", e);
        free(e);
        return;
    }
    U.file_err[0] = 0;
    file_spoil = false;
    wipe(file_alt, sizeof file_alt);
    buf_free(&U.draft);                          /* подпись ушла вместе с файлом */
    U.draft_cur = 0;
    vl_leave();
    U.msg_scroll = 0;
}
/* Беседа готова (dm_open в store.c): главная ЛС, открыть её. */
void ui_open_dm(u64 channel_id) {
    if (O.cli) { cli_open_dm(channel_id); return; }
    if (!chan_find(channel_id)) return;
    U.pick = false;
    if (U.sel_guild != 0) { U.sel_guild = 0; U.sel_row = -1; U.row_scroll = 0; }
    rebuild_rows();
    for (int i = 0; i < U.nrows; i++) if (U.rows[i].kind == R_CHAN && U.rows[i].id == channel_id) { U.sel_row = i; break; }
    open_channel(channel_id);
    U.dirty = true;
}
static void user_card(u64 uid, int x, int y) {
    if (!uid) return;
    if (U.card_uid) profile_close();
    U.card_uid = uid;
    U.card_x = x;
    U.card_y = y;
    U.menu = U.pick = false;
    Channel *c = chan_find(S.open_ch);
    profile_open(uid, c ? c->guild_id : 0);     /* о себе, местоимения, даты — по щелчку, без кэша */
}
/* Открыть канал по id из любого места (#канал в тексте, Ctrl+K, переходы): выбрать его сервер. */
void ui_open_channel(u64 id) {
    Channel *c = chan_find(id);
    if (O.cli || !c || c->type == CH_CATEGORY) return;
    int gi = 0;
    for (int i = 0; i < S.nguilds; i++) if (S.guilds[i].id == c->guild_id) gi = i + 1;
    if (gi != U.sel_guild) select_guild(gi);
    rebuild_rows();
    for (int i = 0; i < U.nrows; i++) if (U.rows[i].kind == R_CHAN && U.rows[i].id == id) { U.sel_row = i; U.row_free = false; break; }
    U.pick = false;
    open_channel(id);
    U.dirty = true;
}
/* Закреплённые / «Входящие» в правой панели (вместо поиска); повторное нажатие — закрыть. */
static void rpanel_close(void) {
    if (U.rp == RP_PINS) pins_close();
    else if (U.rp == RP_INBOX) mentions_close();
    U.rp = RP_NONE;
    U.sr_focus = false;
}
static void search_off(void);
static void rpanel_open(int kind) {
    if (U.rp == kind) { rpanel_close(); return; }
    Channel *c = chan_find(S.open_ch);
    if (kind == RP_PINS && (!c || is_voice(c))) { ui_notice(TR("закреплённые - в текстовом канале или беседе")); return; }
    search_off();
    rpanel_close();
    U.rp = kind;
    U.sr_sel = U.sr_scroll = 0;
    U.sr_focus = true;
    U.menu = U.pick = false;
    if (kind == RP_PINS) pins_open(S.open_ch);
    else if (U.inbox_tab == 1) mentions_open();
}
/* Перейти к сообщению (закреплённые, упоминания, ответ): канал откроется, история — вокруг него. */
static void jump_to(u64 ch, u64 mid) {
    if (!ch || !mid) return;
    if (O.demo || chan_find(ch)) {
        if (ch != S.open_ch) ui_open_channel(ch);
        U.jump_seen = 0;                         /* повторный переход к тому же сообщению — снова выбрать его */
        msgs_jump(ch, mid);
        U.msg_scroll = 0;
        U.follow_sel = true;
        U.focus = F_MESSAGES;
    }
}
static void search_off(void) {
    if (U.rp) rpanel_close();
    if (U.sr_open || U.sq[0]) search_close();
    U.sr_open = U.sr_focus = U.sq_edit = false;
    U.sq[0] = 0;
    U.sr_sel = U.sr_scroll = 0;
}
static void search_go(void) {
    Channel *c = chan_find(S.open_ch);
    if (!U.sq[0]) { U.sq_edit = false; return; }
    if (!c || is_voice(c)) { ui_notice(TR("поиск - из текстового канала или личной беседы")); return; }
    search_start(c->guild_id, c->guild_id ? 0 : c->id, U.sq);
    U.sr_open = true;
    U.sr_focus = true;
    U.sq_edit = false;
    U.sr_sel = U.sr_scroll = 0;
}
static void search_begin(void) {
    Channel *c = chan_find(S.open_ch);
    if (!c || is_voice(c) || friends_screen()) { ui_notice(TR("поиск - из текстового канала или личной беседы")); return; }
    U.sq_edit = true;
    U.menu = false;
}
/* Карточка правой панели: kind — MP_* или 3 (строка «Непрочитанное»: i — индекс в S.chans). */
static int panel_kind(void) { return U.rp == RP_PINS ? MP_PINS : U.rp == RP_INBOX ? (U.inbox_tab ? MP_MENT : 3) : MP_SEARCH; }
static void search_open_hit(int kind, int i) {
    if (kind == 3) {
        /* «Непрочитанное»: i — порядковый номер строки при кадре -> канал ищем тем же обходом */
        if (i >= 0 && i < S.nchans) ui_open_channel(S.chans[i].id);
        return;
    }
    int n = kind == MP_SEARCH ? search_state()->nhits : kind == MP_PINS ? pins_state()->n : mentions_state()->n;
    if (i < 0 || i >= n) return;
    U.sr_sel = i;
    u64 ch;
    const Message *m = mp_msg(kind, i, &ch);
    jump_to(ch, m->id);
    if (U.rp_full) { search_off(); return; }     /* панель на весь центр закрыла бы ленту с результатом перехода */
    U.sr_focus = true;
}
/* Строка поиска: только добавить символ / стереть последний (курсор в конце). */
static void sq_key(const Key *k) {
    size_t n = strlen(U.sq);
    if (k->code == K_BS) { if (n) U.sq[prev_cp(U.sq, n)] = 0; return; }
    if (k->code != K_CHAR || k->ctrl || k->alt) return;
    u32 c = k->ch;
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return;
    char e[4];
    int m = utf8_enc(c, e);
    if (n + (size_t)m + 1 > sizeof U.sq) return;
    memcpy(U.sq + n, e, (size_t)m);
    U.sq[n + (size_t)m] = 0;
}
static void pick_open(void) {
    U.pick = true;
    U.pick_sel = 0;
    U.pick_q[0] = 0;
    U.menu = false;
    U.sq_edit = false;
}
static u64 pick_gdm;                             /* «Выберите друзей» добавляет в эту группу (0 — новая беседа) */
static void pick_go(void) {
    int *ix;
    int n = fr_collect(1, U.pick_q, &ix);
    if (U.pick_sel >= 0 && U.pick_sel < n && pick_gdm) {   /* №111: добавить в группу */
        u64 uid = fr_rels[ix[U.pick_sel]].id, g = pick_gdm;
        char err[160] = "";
        U.pick = false; pick_gdm = 0;
        if (gdm_add(g, uid, err, sizeof err)) ui_notice(TR("добавлен в группу: %s"), user_name(uid));
        else if (err[0]) { char *e = san(err, false); ui_notice("%s", e); free(e); }
    }
    else if (U.pick_sel >= 0 && U.pick_sel < n) { u64 uid = fr_rels[ix[U.pick_sel]].id; U.pick = false; dm_open(uid); }
    else ui_notice(TR("выберите друга"));
    free(ix);
}
static void pick_key(const Key *k) {
    size_t n = strlen(U.pick_q);
    switch (k->code) {
    case K_ESC: U.pick = false; return;
    case K_UP: if (U.pick_sel > 0) U.pick_sel--; return;
    case K_DOWN: U.pick_sel++; return;          /* верхняя граница — в draw_picker */
    case K_ENTER: pick_go(); return;
    case K_BS: if (n) U.pick_q[prev_cp(U.pick_q, n)] = 0; U.pick_sel = 0; return;
    case K_CHAR: {
        u32 c = k->ch;
        if (k->ctrl || k->alt || c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return;
        char e[4];
        int m = utf8_enc(c, e);
        if (n + (size_t)m + 1 > sizeof U.pick_q) return;
        memcpy(U.pick_q + n, e, (size_t)m);
        U.pick_q[n + (size_t)m] = 0;
        U.pick_sel = 0;
        return;
    }
    default: return;
    }
}
/* Действие над записью rel_list[idx] (кнопка, Enter/Del на экране «Друзья»). */
static void fr_action(int sub, int idx, int mx, int my) {
    const Relation *rl;
    int n = rel_list(&rl);
    if (idx < 0 || idx >= n) return;
    Relation r = rl[idx];
    if (sub == FA_ROW) sub = r.type == REL_FRIEND ? FA_MSG : r.type == REL_IN ? FA_ACCEPT : r.type == REL_BLOCKED ? FA_UNBLOCK : -1;
    switch (sub) {
    case FA_MSG: if (r.type == REL_FRIEND) dm_open(r.id); break;
    case FA_ACCEPT: if (r.type == REL_IN) { rel_accept(r.id); ui_notice(TR("заявка в друзья принята: %s"), un(r.id)); } break;
    case FA_UNBLOCK: if (r.type == REL_BLOCKED) { rel_remove(r.id); ui_notice(TR("разблокирован: %s"), un(r.id)); } break;
    case FA_DECLINE:
        if (r.type == REL_FRIEND) { U.mode = M_UNFRIEND; U.target = r.id; }
        else if (r.type == REL_IN || r.type == REL_OUT || r.type == REL_BLOCKED) {
            rel_remove(r.id);
            ui_notice(r.type == REL_IN ? TR("заявка отклонена: %s") : r.type == REL_OUT ? TR("заявка отменена: %s") : TR("разблокирован: %s"), un(r.id));
        }
        break;
    case FA_MORE:
        U.menu_uid = r.id;
        U.menu = true;
        U.menu_kind = MENU_FRIEND;
        U.menu_x = mx;
        U.menu_y = my;
        U.menu_sel = 0;
        break;
    default: if (r.type == REL_OUT) ui_notice(TR("заявка отправлена, ждём ответа; Del - отменить")); break;
    }
}
/* Клавиатура на экране «Друзья» (фокус F_MESSAGES). */
static void friends_key(const Key *k) {
    int *ix;
    int n = fr_collect(U.fr_tab, NULL, &ix);
    int cur = U.fr_sel >= 0 && U.fr_sel < n ? ix[U.fr_sel] : -1;
    free(ix);
    switch (k->code) {
    case K_LEFT: U.fr_tab = (U.fr_tab + 3) % 4; U.fr_sel = U.fr_scroll = 0; return;
    case K_RIGHT: U.fr_tab = (U.fr_tab + 1) % 4; U.fr_sel = U.fr_scroll = 0; return;
    case K_UP: if (U.fr_sel > 0) U.fr_sel--; return;
    case K_DOWN: if (U.fr_sel < n - 1) U.fr_sel++; return;
    case K_HOME: U.fr_sel = 0; return;
    case K_END: U.fr_sel = n - 1; return;
    case K_ESC: U.focus = F_CHANNELS; return;
    case K_ENTER: fr_action(FA_ROW, cur, 0, 0); return;
    case K_DEL: fr_action(FA_DECLINE, cur, 0, 0); return;
    case K_CHAR:
        if (k->ctrl || k->alt) return;
        if (k->ch == 'x' || k->ch == 0x447 /* ч */) fr_action(FA_DECLINE, cur, 0, 0);
        else if ((k->ch == '.' || k->ch == 0x44E /* ю */) && cur >= 0) {
            const Relation *rl;
            rel_list(&rl);
            if (rl[cur].type == REL_FRIEND || rl[cur].type == REL_BLOCKED) fr_action(FA_MORE, cur, U.W - 30, 3 + (U.fr_sel - U.fr_scroll) * 3 + 2);
        }
        return;
    default: return;
    }
}

static void copy_text(const char *s);
/* ---- волна 2: действия окон-форм ---- */
static const char *fm_path(const char *v) { return !v[0] ? NULL : !strcmp(v, "-") ? "" : v; }   /* пусто — не менять, «-» — убрать */
static void fm_submit(int bid) {
    char err[200] = "";
    bool ok = true;
    if (bid == FB_CANCEL) { fm_close(); return; }
#ifdef MC_WAVE2
    if (bid == FB_RETRY) { chan_info_retry(); return; }
#endif
    if (FM.loading && bid == FB_OK && (FM.id == FM_GSET || FM.id == FM_CHEDIT)) { snprintf(FM.err, sizeof FM.err, TR("подождите: текущие значения ещё загружаются")); return; }
    switch (FM.id) {
    case FM_ADDSRV: fm_open(bid == FB_X1 ? FM_JOIN : FM_GCREATE, 0, 0); return;
    case FM_GCREATE: ok = guild_create(FM.f[0].val, FM.f[1].val, err, sizeof err); break;
    case FM_JOIN: {
        char code[64];
        if (!invite_parse(FM.f[0].val, code, sizeof code)) { snprintf(FM.err, sizeof FM.err, TR("это не похоже на приглашение")); return; }
        invite_open(code);
        fm_reset();               /* не fm_open: он закрыл бы только что открытое приглашение */
        FM.id = FM_JOINCARD;
        snprintf(FM.title, sizeof FM.title, TR("Вас пригласили на сервер"));
        fm_btn(TR("Отмена"), FB_CANCEL, 0); fm_btn(TR("Вступить на сервер"), FB_OK, 1);
        FM.foc = 0;                              /* вступление — только щелчком или Tab + пробел */
        return;
    }
    case FM_JOINCARD: {
        const InviteState *is = invite_state();
        if (is->joined) { fm_close(); return; }
#ifdef MC_WAVE2
        if (bid == FB_X1 && is->need_browser) {
            /* браузер идёт мимо VLESS: при включённом туннеле — явное подтверждение вторым нажатием */
            if (vless_state() != VL_OFF && !FM.confirm) {
                FM.confirm = true;
                snprintf(FM.err, sizeof FM.err, TR("браузер откроет ссылку мимо VLESS: Discord увидит ваш IP. Нажмите ещё раз"));
                return;
            }
            FM.confirm = false;
            if (!invite_open_browser()) snprintf(FM.err, sizeof FM.err, TR("браузер не открылся"));
            return;
        }
        if (is->busy) return;
        if (is->risk_warning) invite_risk_ack();  /* «Понятно, вступить» — согласие с предупреждением */
#else
        if (is->busy || is->error[0]) return;
#endif
        invite_join();                           /* явное подтверждение пользователя: кнопка «Вступить» */
        return;                                  /* окно остаётся: итог (вступили / ошибка / браузер) — в карточке */
    }
    case FM_INVITE:
        if (bid == FB_X1) { const char *l = invite_created(); if (l) copy_text(l); else ui_notice(TR("сначала создайте ссылку")); return; }
        invite_create(FM.cid, AGE_S[FM.f[0].ch], USES_V[FM.f[1].ch]);
        snprintf(FM.f[2].val, FM.f[2].cap, TR("создаётся..."));
        return;
    case FM_GSET:
        if (bid == FB_X1) { u64 g = FM.gid; fm_open(FM_GDELETE, g, 0); return; }
        ok = guild_edit(FM.gid, fm_changed(&FM.f[0]) ? FM.f[0].val : NULL, fm_changed(&FM.f[1]) ? FM.f[1].val : NULL,
                        fm_path(FM.f[2].val), fm_path(FM.f[3].val), err, sizeof err);   /* NULL — не менять */
        break;
    case FM_GDELETE: ok = guild_delete(FM.gid, FM.f[0].val, err, sizeof err); break;
    case FM_LEAVE: guild_leave(FM.gid); select_guild(0); break;
    case FM_CHCREATE: {
        Channel *par = chan_find(FM.cid);
        ok = chan_create(FM.gid, FM.f[0].ch ? CHK_VOICE : CHK_TEXT, FM.f[1].val, par && par->type == CH_CATEGORY ? par->id : 0, err, sizeof err);
        if (ok && FM.f[2].ch) ui_notice(TR("приватность: задайте, кому виден канал, в 'Изменить канал' -> 'Права'"));
        break;
    }
    case FM_CATCREATE: ok = chan_create(FM.gid, CHK_CATEGORY, FM.f[0].val, 0, err, sizeof err); break;
    case FM_ALERTS: alert_words_set(FM.f[0].val); ui_notice(TR("слова-тревоги сохранены")); break;
    case FM_FILTER: filter_words_set(FM.f[0].val); ui_notice(TR("фильтр сохранён")); break;
    case FM_FALT: snprintf(file_alt, sizeof file_alt, "%s", FM.f[0].val); break;
#ifdef ASTRA_VIDEO
    case FM_SHARE: {
        int src = FM.f[0].ch;
        uintptr_t id = src < sh_n ? sh_ids[src] : 0;
        VoiceStatus v5;
        voice_status(&v5);
        share_audio_enable(FM.f[2].ch != 0);
        ok = share_start(FM.f[1].ch, id, err, sizeof err);
        if (!ok && err[0]) { char t2[sizeof err]; snprintf(t2, sizeof t2, "%s", tr(err)); snprintf(err, sizeof err, "%s", t2); }   /* ошибки захвата — на языке интерфейса */
#ifndef _WIN32
        if (ok && share_wayland()) {             /* Go Live — после выбора в системном окне (иначе отмена мелькнула бы эфиром) */
            if (!v5.channel_id) { share_stop(); ok = false; snprintf(err, sizeof err, "%s", TR("трансляция: сначала войдите в голосовой канал")); break; }
            sh_gl_wait = true;
            GL_phase = GL_prev = GL_OFF;
            sh_running = true;
            sh_ch = v5.channel_id; sh_gid = v5.guild_id;
            sh_prev_streamer = streamer_on();
            streamer_set(true);
            ui_notice("%s", TR("выберите экран или окно в системном окне"));
            break;
        }
#endif
        if (ok && !golive_start(v5.guild_id, v5.channel_id, err, sizeof err)) { share_stop(); ok = false; }   /* Go Live в этот канал/звонок */
        if (ok) {
            GL_phase = GL_prev = GL_CREATING;
            sh_running = true;
            sh_ch = v5.channel_id;
            sh_prev_streamer = streamer_on();
            streamer_set(true);
            ui_notice("%s", TR("трансляция началась"));
        }
        break;
    }
#endif
#ifdef ASTRA_VIDEO
    case FM_CAM: {
        int d = FM.f[0].ch;
        if (!O.demo && d >= 0 && d < cu_ndev) cam_select(cu_dev[d].id);
        anon_set_default(FM.f[1].ch != 0);       /* до cam_start: первый же кадр уходит уже закрытым */
        cu_pv_hide = !FM.f[2].ch;
        cu_start();
        break;
    }
#endif
    case FM_NOTE: ok = note_set(fm_note_uid, FM.f[0].val, err, sizeof err); if (ok) { note_open(fm_note_uid); ui_notice(TR("заметка сохранена")); } break;
    case FM_STATUS: {
        static const u8 ps[4] = { PS_ONLINE, PS_IDLE, PS_DND, PS_INVISIBLE };
        presence_set_for(ps[FM.f[0].ch & 3], ST_DUR_MIN[FM.f[1].ch]);
        if (fm_changed(&FM.f[2]) || FM.f[3].ch) ok = custom_status_set(FM.f[2].val, ST_DUR_MIN[FM.f[3].ch], err, sizeof err);
        if (ok) ui_notice(TR("статус сохранён"));
        break;
    }
    case FM_MOD: {
        const char *why = fm_mod_kind <= MOD_BAN ? FM.f[FM.nf - 1].val : "";
        switch (fm_mod_kind) {
        case MOD_TIMEOUT: ok = mod_timeout(FM.gid, fm_mod_uid, MOD_TO_MIN[FM.f[0].ch], why, err, sizeof err); break;
        case MOD_KICK: ok = mod_kick(FM.gid, fm_mod_uid, why, err, sizeof err); break;
        case MOD_BAN: ok = mod_ban(FM.gid, fm_mod_uid, why, MOD_DEL_H[FM.f[0].ch], err, sizeof err); break;
        case MOD_ROLES: ok = mod_nr && mod_role(FM.gid, fm_mod_uid, mod_rid[FM.f[0].ch], FM.f[1].ch == 0, err, sizeof err); break;
        case MOD_MOVE: ok = mod_voice_move(FM.gid, fm_mod_uid, mod_rid[FM.f[0].ch], err, sizeof err); break;
        default: ok = mod_voice_mute(FM.gid, fm_mod_uid, FM.f[0].ch != 0, FM.f[1].ch != 0, err, sizeof err); break;
        }
        if (ok) ui_notice(TR("готово"));
        break;
    }
    case FM_TRENAME: ok = thread_edit(FM.cid, FM.f[0].val, -1, -1, err, sizeof err); break;
    case FM_GRENAME: ok = gdm_rename(FM.cid, FM.f[0].val, err, sizeof err); break;
    case FM_FPOST: ok = forum_post_create(FM.cid, FM.f[0].val, FM.f[1].val, err, sizeof err); if (ok) ui_notice(TR("пост опубликован")); break;
    case FM_MEPROF: ok = profile_me_set(FM.f[0].val[0] ? FM.f[0].val : NULL, FM.f[1].val[0] ? FM.f[1].val : NULL, err, sizeof err); if (ok) ui_notice(TR("профиль сохранён")); break;
    case FM_VSTATUS: ok = voice_channel_status_set(FM.cid, FM.f[0].val, err, sizeof err); break;
    case FM_THREAD: ok = thread_create(FM.cid, fm_mid, FM.f[0].val, err, sizeof err); if (ok) ui_notice(TR("ветка создана")); break;
    case FM_CHEDIT: {
        if (bid == FB_X1) { u64 g = FM.gid, c = FM.cid; fm_open(FM_CHDELETE, g, c); return; }
        if (bid == FB_X2) { u64 g = FM.gid, c = FM.cid; fm_open(FM_PERMS, g, c); return; }
        Channel *c = chan_find(FM.cid);
        ChanEdit e = { fm_changed(&FM.f[0]) ? FM.f[0].val : NULL, NULL, -1, -1, -1, -1 };   /* только изменённое */
        if (c && is_voice(c)) {
            if (fm_changed(&FM.f[1])) e.bitrate = BIT_V[FM.f[1].ch];
            if (fm_changed(&FM.f[2])) e.user_limit = LIM_V[FM.f[2].ch];
        } else if (FM.nf >= 4) {
            if (fm_changed(&FM.f[1])) e.topic = FM.f[1].val;
            if (fm_changed(&FM.f[2])) e.slowmode = SLOW_S[FM.f[2].ch];
            if (fm_changed(&FM.f[3])) e.nsfw = FM.f[3].ch;
        }
        if (!e.name && !e.topic && e.slowmode < 0 && e.nsfw < 0 && e.bitrate < 0 && e.user_limit < 0) { fm_close(); return; }
        ok = chan_edit(FM.cid, &e, err, sizeof err);
        break;
    }
    case FM_CHDELETE: chan_delete(FM.cid); break;
    case FM_PERMS: {
        u64 al = FM.o_al, de = FM.o_de;          /* сервер заменяет перезапись целиком: прочие биты сохраняем */
        for (int i = 1; i < FM.nf; i++) {
            int r = perm_row(&FM.f[i]);
            if (r < 0) continue;
            u64 b = PERM_ROWS[r].bit;
            al &= ~b; de &= ~b;
            if (FM.f[i].ch == 2) al |= b;
            else if (FM.f[i].ch == 0) de |= b;
        }
        if (bid == FB_X1 || (!al && !de)) chan_perm_clear(FM.cid, FM.tgt);
        else chan_perm_set(FM.cid, FM.tgt, FM.tgt_type, al, de);
        if (bid == FB_X1) { perms_load(); return; }
        break;
    }
    case FM_ADDFRIEND: ok = rel_add_name(FM.f[0].val, err, sizeof err); break;
    default: break;
    }
    if (!ok) { char *e = san(err[0] ? err : TR("не получилось"), false); snprintf(FM.err, sizeof FM.err, "%s", e); free(e); return; }
    fm_close();
}
/* Клавиатура окна-формы (модально). */
static void fm_key(const Key *k) {
    int nfoc = FM.nf + FM.nb;
    FField *f = FM.foc < FM.nf ? &FM.f[FM.foc] : NULL;
    switch (k->code) {
    case K_ESC: fm_close(); return;
    case K_TAB: case K_DOWN: if (nfoc) FM.foc = (FM.foc + 1) % nfoc; return;
    case K_BTAB: case K_UP: if (nfoc) FM.foc = (FM.foc + nfoc - 1) % nfoc; return;
    case K_LEFT: case K_RIGHT:
        if (f && (f->kind == FF_CHOICE || f->kind == FF_TRI) && f->nch) f->ch = (f->ch + (k->code == K_RIGHT ? 1 : f->nch - 1)) % f->nch;
        else if (f && f->kind == FF_TOGGLE) f->ch = !f->ch;
        return;
    case K_ENTER:
        if (FM.id == FM_JOINCARD) {              /* случайный Enter не вступает на сервер */
            if (FM.foc >= FM.nf && FM.foc - FM.nf < FM.nb && FM.b[FM.foc - FM.nf].id == FB_CANCEL) fm_close();
            else snprintf(FM.err, sizeof FM.err, TR("вступление - щелчком по кнопке или Tab и пробел"));
            return;
        }
        if (FM.foc >= FM.nf && FM.foc - FM.nf < FM.nb) fm_submit(FM.b[FM.foc - FM.nf].id);
        else fm_submit(FB_OK);
        return;
    case K_CHAR:
        if (k->ch == ' ' && FM.foc >= FM.nf && FM.foc - FM.nf < FM.nb) { fm_submit(FM.b[FM.foc - FM.nf].id); return; }   /* пробел — кнопка в фокусе */
#ifdef MC_WAVE2
        if (FM.loading && FM.id == FM_CHEDIT && chan_info_failed(FM.cid) && (k->ch == 'r' || k->ch == 0x43A) && !(f && f->kind == FF_TEXT)) { chan_info_retry(); return; }
#endif
        if (f && f->kind == FF_TOGGLE && k->ch == ' ') { f->ch = !f->ch; return; }
        if (f && f->kind == FF_TEXT) {
            int nc = 0;
            for (const char *p = f->val; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) nc++;
            if (f->maxc && nc >= f->maxc) { snprintf(FM.err, sizeof FM.err, TR("не больше %d символов"), f->maxc); return; }
            q_key(f->val, f->cap, k); FM.err[0] = 0;
        }
        return;
    case K_BS: if (f && f->kind == FF_TEXT) { q_key(f->val, f->cap, k); FM.err[0] = 0; } return;
    default: return;
    }
}
/* Щелчок в окне-форме: поле — фокус (< > и три состояния — значение), кнопка — действие. */
static void fm_mouse(const Hit *h) {
    if (!h) return;
    if (h->act == A_FORMB) { fm_submit(h->sub); return; }
    if (h->act != A_FORMF || h->arg < 0 || h->arg >= FM.nf) return;
    FField *f = &FM.f[h->arg];
    FM.foc = h->arg;
    if (f->kind == FF_TOGGLE) f->ch = !f->ch;
    else if (f->kind == FF_TRI && h->sub >= 1 && h->sub <= 3) f->ch = h->sub - 1;
    else if (f->kind == FF_CHOICE && f->nch && (h->sub == 4 || h->sub == 5)) f->ch = (f->ch + (h->sub == 5 ? 1 : f->nch - 1)) % f->nch;
}
/* Перестановка каналов (меню сервера -> «Переставить каналы»): Alt+Up/Down двигает выбранный канал. */
static void move_step(int d) {
    if (U.sel_row < 0 || U.sel_row >= U.nrows || U.rows[U.sel_row].kind != R_CHAN) { ui_notice(TR("выберите канал Up/Down")); return; }
    Channel *c = chan_find(U.rows[U.sel_row].id);
    if (!c || !c->guild_id) return;
#ifdef MC_WAVE2
    chan_swap(c->id, d);                         /* обмен с соседом по списку (позиции с пропусками — тоже) */
#else
    chan_move(c->id, c->parent_id, c->position + d);
#endif
}

/* Копировать текст в буфер обмена: OSC 52 (терминал и окно mincord). */
static void copy_text(const char *s) {
    if (!s || !*s) return;
    Buf b = {0}, t = {0};
    for (const char *p = s; *p; p++) if (*p != ATT_MARK) buf_put(&t, p, 1);   /* маркер вложения — не в буфер */
    buf_puts(&b, "\x1b]52;c;");
    if (t.p) base64_enc(t.p, t.len, &b);
    buf_free(&t);
    buf_puts(&b, "\x07");
    term_write(b.p, b.len);
    buf_free(&b);
    ui_notice(TR("текст скопирован"));
}
/* ---- действия над сообщением (клавиши r/e/d/+ и пункты меню) ---- */
static void user_card(u64 uid, int x, int y);
static struct { u64 u; float v; } lmute[16];     /* «Заглушить для меня»: громкость до заглушения (в памяти) */
static void vmem_action(int what) {
    u64 uid = U.menu_uid;
    if (!uid) return;
    float v = voice_get_volume(uid);
    switch (what) {
    case MI_V_VOL: voice_set_volume(uid, 1.0f); break;
    case MI_V_UP: case MI_V_DN: {
        int pc = (int)(v * 100 + 0.5f) + (what == MI_V_UP ? 10 : -10);   /* шаг ровно 10% без дрейфа float */
        voice_set_volume(uid, (pc < 0 ? 0 : pc > 200 ? 200 : pc) / 100.0f);
        break;
    }
    case MI_V_MUTE: {
        int k = -1, fr = -1;
        for (int i = 0; i < 16; i++) { if (lmute[i].u == uid) k = i; if (!lmute[i].u && fr < 0) fr = i; }
        if (v < 0.005f) { voice_set_volume(uid, k >= 0 && lmute[k].v > 0.005f ? lmute[k].v : 1.0f); if (k >= 0) lmute[k].u = 0; }
        else { if (k < 0) k = fr >= 0 ? fr : 0; lmute[k].u = uid; lmute[k].v = v; voice_set_volume(uid, 0); }
        U.menu = false;
        return;
    }
    case MI_V_PROF: U.menu = false; user_card(uid, U.menu_x, U.menu_y); return;
    case MI_V_MIC: voice_toggle_mute(); U.menu = false; return;
    case MI_V_DEAF: voice_toggle_deaf(); U.menu = false; return;
    case MI_V_SMUTE: case MI_V_MOVE: {
        Channel *vc = chan_find(U.menu_vch);
        U.menu = false;
        if (!vc || !vc->guild_id) return;
        fm_mod_uid = uid; fm_mod_kind = what == MI_V_SMUTE ? MOD_MUTE : MOD_MOVE;   /* та же форма, что в карточке (B_MOD) */
        fm_open(FM_MOD, vc->guild_id, vc->id);
        return;
    }
    }
    U.menu = true;                               /* громкость: меню остаётся открытым, можно жать ещё */
}
static void msg_action(Message *m, int what) {
    if (what >= MI_V_VOL && what <= MI_V_DEAF) { vmem_action(what); return; }
    switch (what) {
    case MI_FX: fx_cycle(); return;
    case MI_VADDN: voice_vad_adjust(-3); ui_notice(TR("порог голоса: %d дБ"), voice_vad_db()); return;
    case MI_VADUP: voice_vad_adjust(3); ui_notice(TR("порог голоса: %d дБ"), voice_vad_db()); return;
    case MI_HELP: U.help = true; U.help_scroll = 0; U.full = true; return;
    case MI_QUIT: g_quit = true; return;
    case MI_FOCUS30: case MI_FOCUS60: case MI_FOCUS120: {
        int mn = what == MI_FOCUS30 ? 30 : what == MI_FOCUS60 ? 60 : 120;
        focus_set(mn);
        ui_notice(TR("режим фокуса на %d мин: упоминания - только ЛС и @вы"), mn);
        return;
    }
    case MI_FOCUSOFF: focus_set(0); ui_notice(TR("режим фокуса выключен")); return;
    case MI_STATUS_SET: U.menu = false; fm_open(FM_STATUS, 0, 0); fm_mark(); return;
    case MI_ST_ON: case MI_ST_IDLE: case MI_ST_DND: case MI_ST_INV: {
        static const u8 ps[4] = { PS_ONLINE, PS_IDLE, PS_DND, PS_INVISIBLE };
        presence_set(ps[what - MI_ST_ON]);
        return;
    }
    case MI_G_INV: case MI_G_SET: case MI_G_CH: case MI_G_CAT: case MI_G_LEAVE: {
        u64 g = cur_guild_id();
        if (!g) return;
        u64 ch = 0;
        Channel *oc = chan_find(S.open_ch);
        if (oc && oc->guild_id == g && !is_voice(oc)) ch = oc->id;
        for (int i = 0; i < S.nchans && !ch; i++) if (S.chans[i].guild_id == g && chan_listed(&S.chans[i]) && !is_voice(&S.chans[i])) ch = S.chans[i].id;
        fm_open(what == MI_G_INV ? FM_INVITE : what == MI_G_SET ? FM_GSET : what == MI_G_CH ? FM_CHCREATE : what == MI_G_CAT ? FM_CATCREATE : FM_LEAVE,
                g, what == MI_G_INV ? ch : 0);
        return;
    }
    case MI_G_MOVE: U.moving = !U.moving; U.focus = F_CHANNELS; ui_notice(U.moving ? TR("перестановка: Up/Down - канал, Alt+Up/Down - переставить, Esc - готово") : TR("перестановка закончена")); return;
    case MI_C_EDIT: case MI_C_PERMS: case MI_C_INV: case MI_C_DEL: {
        Channel *c = chan_find(U.menu_cid);
        if (!c) return;
        fm_open(what == MI_C_EDIT ? FM_CHEDIT : what == MI_C_PERMS ? FM_PERMS : what == MI_C_INV ? FM_INVITE : FM_CHDELETE, c->guild_id, c->id);
        return;
    }
#ifdef MC_PACKA
    case MI_G_MUTE: { Guild *g = guild_find(cur_guild_id()); if (g) mute_guild(g->id, !g->muted); return; }
    case MI_G_NOTIFY: { Guild *g = guild_find(cur_guild_id()); if (g) notify_level(g->id, 0, ((g->notify & 3) + 1) % 4); return; }
    case MI_C_MUTE: { Channel *c = chan_find(U.menu_cid); if (c) mute_channel(c->id, !c->muted); return; }
    case MI_C_ARCH: { Channel *c = chan_find(U.menu_cid); U.menu = false; if (!c) return; if (S.open_ch != c->id) open_channel(c->id); threads_archived_open(c->id); arch_view = c->id; FV.sel = FV.top = 0; U.focus = F_MESSAGES; return; }
    case MI_T_RENAME: { Channel *c = chan_find(U.menu_cid); U.menu = false; if (c) fm_open(FM_TRENAME, c->guild_id, c->id); return; }
    case MI_GDM_RENAME: { Channel *c = chan_find(U.menu_cid); U.menu = false; if (c) fm_open(FM_GRENAME, 0, c->id); return; }
    case MI_T_ARCH: case MI_T_LOCK: {
        Channel *c = chan_find(U.menu_cid);
        char err[160] = "";
        U.menu = false;
        if (!c) return;
        bool ok2 = what == MI_T_ARCH ? thread_edit(c->id, NULL, !(c->tflags & TF_ARCHIVED), -1, err, sizeof err)
                                     : thread_edit(c->id, NULL, -1, !(c->tflags & TF_LOCKED), err, sizeof err);
        if (!ok2 && err[0]) { char *e = san(err, false); ui_notice("%s", e); free(e); }
        return;
    }
    case MI_T_MEMB: { Channel *c = chan_find(U.menu_cid); U.menu = false; if (c) { thread_members_open(c->id); tm_on = true; } return; }
    case MI_C_VSTATUS: { Channel *c = chan_find(U.menu_cid); U.menu = false; if (c) fm_open(FM_VSTATUS, c->guild_id, c->id); return; }
    case MI_GDM_ADD: pick_gdm = U.menu_cid; U.menu = false; pick_open(); return;
    case MI_GDM_LEAVE: {
        char err[160] = "";
        if (gdm_leave(U.menu_cid, err, sizeof err)) ui_notice(TR("вы покинули группу"));
        else if (err[0]) { char *e = san(err, false); ui_notice("%s", e); free(e); }
        return;
    }
    case MI_T_JOIN: {
        Channel *c = chan_find(U.menu_cid);
        char e[160] = "";
        if (!c) return;
        bool join = !(c->tflags & TF_JOINED);
        if (thread_member(c->id, join, e, sizeof e)) ui_notice(join ? TR("вы вступили в ветку") : TR("вы покинули ветку"));
        else if (e[0]) { char *se = san(e, false); ui_notice("%s", se); free(se); }
        return;
    }
    case MI_C_NOTIFY: { Channel *c = chan_find(U.menu_cid); if (c) notify_level(c->guild_id, c->id, ((c->notify & 3) + 1) % 4); return; }
#endif
    case MI_C_ID: { char t[32]; snprintf(t, sizeof t, "%llu", (unsigned long long)U.menu_cid); copy_text(t); return; }
    case MI_FR_MSG: if (rel_find(U.menu_uid)) dm_open(U.menu_uid); return;
    case MI_FR_DEL: if (rel_find(U.menu_uid)) { U.mode = M_UNFRIEND; U.target = U.menu_uid; } return;
    case MI_FR_UNBLOCK: if (rel_find(U.menu_uid)) { rel_remove(U.menu_uid); ui_notice(TR("разблокирован: %s"), un(U.menu_uid)); } return;
    default: break;
    }
    if (!m) return;
#ifdef MC_PACKA
    if (what == MI_MRETRY) { msg_retry(m->id); return; }
    if (what == MI_MDISCARD) { msg_discard(m->id); return; }
    if (m->state != MS_SENT && what != MI_COPY) { ui_notice(TR("сообщение ещё не отправлено: можно повторить, отменить или скопировать")); return; }
#endif
    switch (what) {
    case MI_REPLY: U.mode = M_REPLY; U.target = m->id; U.focus = F_INPUT; break;
    case MI_EDIT:
        if (m->author_id != S.me_id) { ui_notice(TR("изменять можно только свои сообщения")); return; }
        if (!edit_src(m)) { ui_notice(TR("текст этого сообщения недоступен для правки")); return; }
        U.mode = M_EDIT; U.target = m->id; edit_set(edit_src(m)); U.focus = F_INPUT;
        break;
    case MI_DELETE:
        if (m->author_id != S.me_id) { ui_notice(TR("удалять можно только свои сообщения")); return; }
        U.mode = M_DELETE; U.target = m->id;
        break;
    case MI_REACT: U.mode = M_REACT; U.target = m->id; in_clear(); U.focus = F_INPUT; break;
    case MI_COPY: copy_text(m->content); break;
    case MI_FORWARD: U.fwd_msg = m->id; U.fwd_ch = S.open_ch; U.qs = true; U.qs_q[0] = 0; U.qs_sel = 0; U.menu = false; break;
    case MI_REACTS: rv_open(m); break;
    case MI_THREAD: { const Channel *oc = chan_find(S.open_ch); fm_mid = m->id; U.menu = false; if (oc) fm_open(FM_THREAD, oc->guild_id, oc->id); break; }
    case MI_COPYID: case MI_COPYLINK: {
        char t[160];
        Channel *c = chan_find(S.open_ch);
        if (what == MI_COPYID) snprintf(t, sizeof t, "%llu", (unsigned long long)m->id);
        else if (c && c->guild_id) snprintf(t, sizeof t, "https://discord.com/channels/%llu/%llu/%llu", (unsigned long long)c->guild_id, (unsigned long long)S.open_ch, (unsigned long long)m->id);
        else snprintf(t, sizeof t, "https://discord.com/channels/@me/%llu/%llu", (unsigned long long)S.open_ch, (unsigned long long)m->id);
        copy_text(t);
        break;
    }
#ifdef MC_WAVE1
#ifdef MC_RAW
    case MI_PIN: msg_pin(S.open_ch, m->id, !m->pinned); break;   /* итог (или 403) сообщает store по ответу */
#else
    case MI_PIN: msg_pin(S.open_ch, m->id, true); break;
#endif
    case MI_UNREAD: mark_unread(S.open_ch, m->id); ui_notice(TR("канал отмечен непрочитанным с этого сообщения")); break;
#endif
    default: break;
    }
}
static void menu_open(int kind, int mi, int x, int y) {
    if (kind == MENU_MSG) {
        if (mi < 0 || mi >= S.nmsgs) return;
        U.sel_msg = mi;
        U.focus = F_MESSAGES;
        U.follow_sel = false;
        U.menu_msg = S.msgs[mi].id;
    }
    U.menu = true;
    U.menu_kind = kind;
    U.menu_x = x;
    U.menu_y = y;
    U.menu_sel = 0;
}
static void menu_do(int id) {
    U.menu = false;
    msg_action(U.menu_kind == MENU_MSG ? msg_find(U.menu_msg, NULL) : NULL, id);
}
static void cancel_mode(void) {
    if (U.mode == M_FILE) { vl_leave(); return; }
    if (U.mode == M_EDIT || U.mode == M_REACT) in_clear();
    U.mode = M_NORMAL;
    U.target = 0;
}

/* ---- мышь ---- */
static void wheel(int x, int y, bool up) {
    if (U.menu) { U.menu = false; return; }
    switch (hit_area(x, y)) {
    case A_AR_HELP: U.help_scroll += up ? -3 : 3; if (U.help_scroll < 0) U.help_scroll = 0; break;
    case A_AR_RAIL: U.guild_free = true; U.guild_scroll += up ? -1 : 1; break;
    case A_AR_CHANS: U.row_free = true; U.row_scroll += up ? -3 : 3; break;
    case A_AR_MEMB: MB.top += up ? -3 : 3; if (MB.top < 0) MB.top = 0; break;   /* верх — draw_members */
    case A_AR_TXT: {
        /* колесо над развёрнутым текстом — прокрутка внутри блока */
        for (int i = U.nhit - 1; i >= 0; i--) if (U.hit[i].act == A_AR_TXT && hit_in(&U.hit[i], x, y)) {
            int mi = U.hit[i].arg, k = mi >= 0 && mi < S.nmsgs ? txt_find(S.msgs[mi].id) : -1;
            if (k >= 0) { txt_open[k].scroll += up ? -3 : 3; if (txt_open[k].scroll < 0) txt_open[k].scroll = 0; }
            break;
        }
        break;
    }
    case A_AR_LIST: {
        /* колесо двигает выбор (список сам держит его видимым) */
        const Hit *h = NULL;
        for (int i = U.nhit - 1; i >= 0 && !h; i--) if (U.hit[i].act == A_AR_LIST && hit_in(&U.hit[i], x, y)) h = &U.hit[i];
        if (h && h->arg == 0) { U.fr_sel += up ? -1 : 1; if (U.fr_sel < 0) U.fr_sel = 0; }
        else if (h && h->arg == 1) { U.sr_sel += up ? -1 : 1; if (U.sr_sel < 0) U.sr_sel = 0; U.sr_focus = true; }
        break;
    }
    case A_AR_FEED:
        U.follow_sel = false;
        if (up) { if (U.at_top) fetch_messages(true); U.msg_scroll += 3; }
        else { U.msg_scroll -= 3; if (U.msg_scroll < 0) U.msg_scroll = 0; }
        break;
    default: break;
    }
}
static void button(int id, int x, int y) {
    switch (id) {
    case B_MIC: voice_toggle_mute(); break;
    case B_DEAF: voice_toggle_deaf(); break;
    case B_FX: fx_cycle(); break;
    case B_LEAVE: voice_leave(); break;
    case B_ATTACH: file_enter(); break;
    case B_SEARCH: search_begin(); break;
    case B_SRCH_CLR: search_off(); break;
    case B_SRCH_X: search_off(); break;
    case B_ADDDM: pick_gdm = 0; if (U.pick) U.pick = false; else pick_open(); break;
    case B_PICK_GO: pick_go(); break;
    case B_PICK_X: U.pick = false; break;
    case B_CARD_X: U.card_uid = 0; profile_close(); break;
    case B_PING: U.no_ping = !U.no_ping; break;
    case B_ADDSRV: fm_open(FM_ADDSRV, 0, 0); break;
    case B_EMBEDS: U.no_embeds = !U.no_embeds; break;
    case B_MICTEST: mic_test_on = !mic_test_on; voice_mic_test(mic_test_on); break;
    case B_ZOOM_DN: zoom_step(-1); break;
    case B_ZOOM_0: zoom_step(0); break;
    case B_ZOOM_UP: zoom_step(1); break;
    case B_FSORT: { const ForumState *fs = forum_state(); if (fs && fs->cid) { forum_open(fs->cid, 0, !fs->sort); FV.sel = FV.top = 0; } break; }
    case B_FPREV: forum_page(-1); break;
    case B_ALERTS: U.settings = false; U.full = true; fm_open(FM_ALERTS, 0, 0); break;
    case B_AVBIG: AVB.uid = U.card_uid; U.card_uid = 0; profile_close(); U.full = true; break;
    case B_ARCH_X: arch_view = 0; forum_close(); break;
    case B_FNEW: { const Channel *oc = chan_find(S.open_ch); if (oc) fm_open(FM_FPOST, oc->guild_id, oc->id); break; }
    case B_MEPROF: U.settings = false; U.full = true; fm_open(FM_MEPROF, 0, 0); break;
    case B_TM_X: tm_on = false; thread_members_close(); break;
    case B_RAMBAR: show_ram = !show_ram; break;
    case B_FSPOIL: file_spoil = !file_spoil; break;
    case B_CARD_IGN: if (U.card_uid) { bool on = !ignored(U.card_uid); ignore_set(U.card_uid, on); ui_notice(on ? TR("сообщения скрыты (только у вас)") : TR("больше не игнорируется")); } break;
    case B_CARD_NOTE: if (U.card_uid) note_open(U.card_uid); break;
    case B_NOTE_EDIT: if (U.card_uid) { fm_note_uid = U.card_uid; fm_open(FM_NOTE, 0, 0); } break;
    case B_RULES: { const Channel *oc = chan_find(S.open_ch); if (oc && oc->guild_id) { guild_rules_open(oc->guild_id); rules_on = true; } break; }
    case B_RULES_OK: {
        const RulesState *rs = guild_rules_state();
        char err[160] = "";
        if (rs && guild_rules_accept(rs->gid, err, sizeof err)) { ui_notice(TR("правила приняты - можно писать")); rules_on = false; guild_rules_close(); }
        else if (err[0]) { char *e = san(err, false); ui_notice("%s", e); free(e); }
        break;
    }
    case B_RULES_X: rules_on = false; guild_rules_close(); break;
    case B_SOUNDS: sounds_enable(!sounds_enabled()); break;
    case B_LANG: lang_set(!g_lang_en); break;
    case B_UP_APPLY: update_apply(); if (O.demo) ui_notice("%s", TR("в демо-режиме обновления не скачиваются")); break;
    case B_UP_LATER: { const UpdateState *us = update_state(); if (us) snprintf(upd_later, sizeof upd_later, "%s", us->version); upd_on = false; break; }
    case B_UP_RESTART: if (!update_restart()) ui_notice("%s", TR("перезапуск не удался - запустите программу заново")); break;
    case B_UP_AUTO: update_auto_set(!update_auto()); break;
    case B_UP_ANYWAY: update_apply_anyway(); break;
    case B_UP_CANCEL: update_apply_cancel(); break;
    case B_UP_CHECK: update_check(true); if (O.demo) ui_notice("%s", TR("в демо-режиме обновления не проверяются")); break;
    case B_DUCK: voice_duck_others(!voice_duck_others_on()); break;
#ifdef ASTRA_VIDEO
    case B_SHARE: {
        if (sh_running) { share_ui_stop("трансляция остановлена"); break; }
        VoiceStatus v4;
        voice_status(&v4);
        if (v4.phase == VS_IDLE) { ui_notice("%s", TR("сначала подключитесь к голосовому каналу или звонку")); break; }
        fm_open(FM_SHARE, 0, 0);
        break;
    }
#endif
#ifdef ASTRA_VIDEO
    case B_WATCH: {
        int ri = U.btn_arg;
        if (ri < 0 || ri >= U.nrows || U.rows[ri].kind != R_MEMBER) break;
        VoiceStatus v8;
        voice_status(&v8);
        if (v8.phase == VS_IDLE || v8.channel_id != U.rows[ri].id) { ui_notice("%s", TR("чтобы смотреть трансляцию, сначала войдите в этот голосовой канал")); break; }
        wv_watch(U.rows[ri].user);
        break;
    }
    case B_WATCH_STOP: wv_stop(); break;
    case B_WATCH_PICK: wv_pick(U.btn_arg); break;
    case B_WATCH_FULL: WV.full = !WV.full; U.full = true; break;
    case B_WATCH_CHAT: WV.chat = true; U.full = true; break;
    case B_WATCH_SHOW: WV.chat = false; WV.ch = S.open_ch; WV.gd = U.sel_guild; U.full = true; break;
    case B_CAM: cu_toggle(!cu_on()); break;
    case B_CAM_PV: cu_pv_hide = !cu_pv_hide; U.full = true; break;
    case B_ANON: cu_anon_toggle(); break;
    case B_CAM_VIEW:                             /* значок []/ у участника в списке канала */
        if (U.btn_arg >= 0 && U.btn_arg < U.nrows && U.rows[U.btn_arg].kind == R_MEMBER) {
            VoiceStatus v8;
            voice_status(&v8);
            if (v8.phase == VS_IDLE || v8.channel_id != U.rows[U.btn_arg].id) ui_notice("%s", TR("чтобы смотреть камеру, сначала войдите в этот голосовой канал"));
            else cv_open(U.rows[U.btn_arg].user);
        }
        break;
    case B_CAM_TILE: if (U.btn_arg >= 0 && U.btn_arg < 16 && cu_tu[U.btn_arg]) cv_open(cu_tu[U.btn_arg]); break;
#endif
    case B_HPF: voice_hpf(!voice_hpf_on()); break;
    case B_MICG_DN: voice_mic_gain_set(voice_mic_gain() - 10); break;
    case B_MICG_UP: voice_mic_gain_set(voice_mic_gain() + 10); break;
    case B_MICG_0: voice_mic_gain_set(100); break;
    case B_NS_OFF: case B_NS_SPX: case B_NS_RNN: voice_ns_set(NS_OFF + (id - B_NS_OFF)); break;
    case B_FSEND: if (U.mode == M_FILE) file_submit(); break;
    case B_MOD: {
        const Channel *oc = chan_find(S.open_ch);
        fm_mod_uid = U.card_uid; fm_mod_kind = U.btn_arg; U.card_uid = 0;
        profile_close();
        if (oc && oc->guild_id) fm_open(FM_MOD, oc->guild_id, oc->id);
        break;
    }
    case B_FALT: fm_open(FM_FALT, 0, 0); break;
    case B_IMGCLICK: img_click = !img_click; ui_notice(img_click ? TR("картинки - по щелчку") : TR("картинки - сразу")); break;
    case B_NOTIFY: { int nm = notify_mode(); nm = nm == NOTIFY_OFF ? NOTIFY_PLAIN : nm == NOTIFY_PLAIN ? NOTIFY_TEXT : NOTIFY_OFF; notify_set(nm); break; }
    case B_FILTER: U.settings = false; U.full = true; fm_open(FM_FILTER, 0, 0); break;
    case B_FULL: full_toggle(); break;
    case B_STREAMER: streamer_set(!streamer_on()); ui_notice(streamer_on() ? TR("режим стримера включён") : TR("режим стримера выключен")); break;
    case B_PTT_SET: ptt_capture = true; break;   /* следующее событие окна (не опрос клавиатуры) */
    case B_PTT_CLR: voice_ptt_bind(0); ptt_capture = false; break;
    case B_VADAUTO: voice_vad_auto(!voice_vad_is_auto()); break;
    case B_VSTATUS: { VoiceStatus v2; voice_status(&v2); Channel *vc = chan_find(v2.channel_id); if (vc) fm_open(FM_VSTATUS, vc->guild_id, vc->id); break; }
    case B_FNEXT: forum_page(1); break;
#ifdef MC_PACKA
    case B_MRETRY: if (U.btn_arg >= 0 && U.btn_arg < S.nmsgs) msg_retry(S.msgs[U.btn_arg].id); break;
    case B_MDISCARD: if (U.btn_arg >= 0 && U.btn_arg < S.nmsgs) msg_discard(S.msgs[U.btn_arg].id); break;
    case B_SP_PREV: case B_SP_NEXT: {
        const SearchState *st = search_state();
        int pg = st->page + (id == B_SP_NEXT ? 1 : -1);
        if (pg >= 0 && pg < st->page_count) { search_page(pg); U.sr_sel = U.sr_scroll = 0; }
        break;
    }
#endif
    case B_EPH_HIDE: if (U.btn_arg >= 0 && U.btn_arg < S.nmsgs) U.eph_hidden[U.eph_n++ & 7] = S.msgs[U.btn_arg].id; break;
    case B_STARTASK:
        /* безопасность П3-а: «напрямую» записывается только при выключенном VLESS (явный выбор без туннеля);
         * при работающей ссылке вопроса и так нет, а запись пустила бы напрямую, пропади ссылка */
        if (!startup_direct() && vless_state() != VL_OFF) { ui_notice(TR("пока работает ссылка VLESS, вопроса при запуске и так нет")); break; }
        startup_direct_set(!startup_direct());
        ui_notice(startup_direct() ? TR("при запуске: без вопроса (сохранённая ссылка или напрямую)") : TR("при запуске: спрашивать про VLESS"));
        break;
    case B_TXT_EXP: if (U.btn_arg >= 0 && U.btn_arg < S.nmsgs) txt_toggle(S.msgs[U.btn_arg].id); break;
    case B_TXT_DL:
        if (U.btn_arg >= 0 && U.btn_arg < S.nmsgs && S.msgs[U.btn_arg].content) {
            const char *c = S.msgs[U.btn_arg].content;
            for (size_t a = 0; c[a]; ) {             /* первая строка-вложение текстового файла */
                size_t e = a;
                while (c[e] && c[e] != '\n') e++;
                int t = text_line(c, a, e);
                if (t >= 0) { char u2[2048]; att_url(c, (size_t)t, u2, sizeof u2, true); if (!u2[0]) break; copy_text(u2); ui_notice(TR("ссылка на файл скопирована: откройте её в браузере")); break; }
                a = c[e] ? e + 1 : e;
            }
        }
        break;
    case B_ADDFRIEND: fm_open(FM_ADDFRIEND, 0, 0); break;
    case B_GMENU: menu_open(MENU_GUILD, -1, x, y + 1); break;
    case B_GINVITE: U.menu_kind = MENU_GUILD; msg_action(NULL, MI_G_INV); break;
    case B_EP_X: U.ep = false; U.ep_msg = 0; break;
    case B_EMOJI: if (U.ep) { U.ep = false; U.ep_msg = 0; } else ep_open(0); break;
    case B_PINS: rpanel_open(RP_PINS); break;
    case B_INBOX: rpanel_open(RP_INBOX); break;
    case B_PINS_MORE: pins_more(); break;
    case B_INBOX_T0: case B_INBOX_T1:
        U.inbox_tab = id == B_INBOX_T1;
        U.sr_sel = U.sr_scroll = 0;
        if (U.inbox_tab) mentions_open(); else mentions_close();
        break;
    case B_ACK_ALL: ack_all(); ui_notice(TR("всё отмечено прочитанным")); break;
    case B_PRESENT: msgs_present(); U.msg_scroll = 0; U.sel_msg = -1; break;
    case B_NEWMSG: if (U.new_from) jump_to(S.open_ch, U.new_from); break;
    case B_STATUS: menu_open(MENU_STATUS, -1, x, y); break;
    case B_CARD_BLOCK: { u64 u = U.card_uid; U.card_uid = 0; profile_close(); if (u && u != S.me_id) { rel_block(u); ui_notice(TR("заблокирован: %s"), user_name(u)); } break; }
    case B_CARD_ADD: { u64 u = U.card_uid; U.card_uid = 0; profile_close(); if (u && u != S.me_id) { rel_add(u); ui_notice(TR("заявка в друзья отправлена: %s"), user_name(u)); } break; }
    case B_MB_REACT: case B_MB_REPLY: case B_MB_EDIT: case B_MB_MORE: {
        int mi = -1;
        Message *m = msg_find(U.bar_msg, &mi);
        if (!m) break;
        U.sel_msg = mi;
        U.follow_sel = false;
        if (id == B_MB_MORE) menu_open(MENU_MSG, mi, x, y + 1);
        else if (id == B_MB_REACT) ep_open(m->id);
        else msg_action(m, id == B_MB_REPLY ? MI_REPLY : MI_EDIT);
        break;
    }
    case B_CARD_MSG: { u64 u = U.card_uid; U.card_uid = 0; profile_close(); if (u && u != S.me_id) dm_open(u); break; }
    case B_CARD_ID: { char t[32]; snprintf(t, sizeof t, "%llu", (unsigned long long)U.card_uid); U.card_uid = 0; profile_close(); copy_text(t); break; }
    case B_PLUS: {
        /* «+» у категории: создать канал в ней */
        if (U.btn_arg >= 0 && U.btn_arg < U.nrows) {
            Channel *cat = chan_find(U.rows[U.btn_arg].id);
            if (cat && can_manage_channels(cat->guild_id)) fm_open(FM_CHCREATE, cat->guild_id, cat->id);
            else ui_notice(TR("нет права управлять каналами"));
        }
        break;
    }
    case B_CANCEL: if (SL.cmd >= 0) { sl_reset(); in_clear(); } else cancel_mode(); U.focus = F_INPUT; break;
    case B_CALL_OK: { Key k = { .code = K_F2 }; voice_keys(&k); break; }
    case B_CALL_NO: { Key k = { .code = K_F3 }; voice_keys(&k); break; }
    case B_CALL: { int f = U.focus; U.focus = F_INPUT; call_dm(); U.focus = f; break; }
    case B_JOIN: { Channel *c = chan_find(S.open_ch); if (c && is_voice(c)) join_voice(c); break; }
    case B_VOPEN: {
        VoiceStatus vs;
        voice_status(&vs);
        if (vs.phase != VS_IDLE) open_channel(vs.channel_id);
        break;
    }
    case B_SETTINGS: U.settings = true; U.full = true; (void)x; (void)y; break;
    case B_SET_CLOSE: U.settings = false; U.full = true; break;
    case B_VADDN: voice_vad_adjust(-3); ui_notice(TR("порог голоса: %d дБ"), voice_vad_db()); break;
    case B_VADUP: voice_vad_adjust(3); ui_notice(TR("порог голоса: %d дБ"), voice_vad_db()); break;
    case B_QUIT: g_quit = true; break;
    case B_MEMBERS: U.members_off = !U.members_off; break;
    case B_CHATX: U.vchat_off = !U.vchat_off; break;
    case B_REQS: RQ.on = true; RQ.tab = 0; search_off(); if (S.open_ch) { S.open_ch = 0; msgs_clear(); U.sel_msg = -1; U.msg_scroll = 0; U.mode = M_NORMAL; } break;
    case B_RQTAB: RQ.tab = U.btn_arg ? 1 : 0; break;
    case B_RQ_OK: rq_answer(U.btn_arg, true); break;
    case B_RQ_NO: rq_answer(U.btn_arg, false); break;
    case B_RING_AGAIN: if (U.btn_arg >= 0 && U.btn_arg < n_rgt) call_ring_user(S.open_ch, rgt[U.btn_arg]); break;
    case B_RING_STOP: if (U.btn_arg >= 0 && U.btn_arg < n_rgt) call_stop_ring(S.open_ch, rgt[U.btn_arg]); break;
    case B_RING_ALL_STOP: call_stop_ring(S.open_ch, 0); break;
    case B_CARD_FULL: { const Channel *oc = chan_find(S.open_ch); profile_open_full(U.card_uid, oc ? oc->guild_id : 0); break; }
    case B_CARD_KICK: {
        u64 u = U.card_uid;
        char err[160] = "";
        U.card_uid = 0; profile_close();
        if (u && gdm_remove(S.open_ch, u, err, sizeof err)) ui_notice(TR("исключён из группы: %s"), user_name(u));
        else if (err[0]) { char *e = san(err, false); ui_notice("%s", e); free(e); }
        break;
    }
    case B_FRIENDS: RQ.on = false; search_off(); if (S.open_ch) { S.open_ch = 0; msgs_clear(); U.sel_msg = -1; U.msg_scroll = 0; U.mode = M_NORMAL; } break;
    case B_VL_R: vl_restart(); break;
    case B_VL_C: U.settings = false; vl_enter(M_VLINK); break;
    case B_VL_D: U.settings = false; vl_enter(M_VLOFF); break;
    case B_HELP: U.help = true; U.help_scroll = 0; U.full = true; break;
    case B_HELP_CLOSE: U.help = false; U.full = true; break;
    case B_BOTTOM: U.msg_scroll = 0; U.follow_sel = false; break;
    case B_OLDER: fetch_messages(true); break;
    default: break;
    }
}
static void mouse(const Key *k) {
    if (k->button == 0) {
        /* наведение: перерисовка, только если под курсором другая область */
        U.hx = k->x;
        U.hy = k->y;
        const Hit *h = hit_item(k->x, k->y);
        bool on = h && h->act != A_INPUT;
        if (on != U.hov_on || (on && !hit_same(h, &U.hov))) U.dirty = true;
        return;
    }
    U.dirty = true;
    if (U.busy[0]) return;
    if (AVB.uid) { if (k->button == 1 || k->button == 3) avb_close(); return; }   /* №101: щелчок — закрыть */
    if (upd_on) { const Hit *uh = hit_item(k->x, k->y); if (k->button == 1 && uh && uh->act == A_BTN && (uh->sub == B_UP_APPLY || uh->sub == B_UP_LATER || uh->sub == B_UP_RESTART || uh->sub == B_UP_ANYWAY || uh->sub == B_UP_CANCEL)) button(uh->sub, 0, 0); return; }
    if (rules_on) { const Hit *rh = hit_item(k->x, k->y); if (k->button == 1 && rh && rh->act == A_BTN && (rh->sub == B_RULES_OK || rh->sub == B_RULES_X)) button(rh->sub, 0, 0); return; }
    if (tm_on) { const Hit *th = hit_item(k->x, k->y); if (k->button == 1 && th && th->act == A_BTN && th->sub == B_TM_X) { tm_on = false; thread_members_close(); } return; }
    if (CSEL.on && !U.help && !U.settings) {     /* окно списка бота модально */
        const Hit *ch = hit_item(k->x, k->y);
        if (k->button != 1 || !ch) return;
        if (ch->act == A_CSOPT) { CSEL.sel = ch->arg; comp_sel_pick(ch->arg); }
        else if (ch->act == A_BTN && ch->sub == B_CSEL_X) comp_sel_close();
        else if (ch->act == A_BTN && ch->sub == B_CSEL_OK) comp_sel_done();
        return;
    }
    if (IV.on && !U.help && !U.settings) {       /* просмотр картинки модален */
        const Hit *ih = hit_item(k->x, k->y);
        if (k->button != 1 || !ih || ih->act != A_BTN) return;
        if (ih->sub == B_IV_X) iv_close();
        else if (ih->sub == B_IV_PREV) iv_step(-1);
        else if (ih->sub == B_IV_NEXT) iv_step(1);
        return;
    }
    if (k->button == 4 || k->button == 5) { wheel(k->x, k->y, k->button == 4); return; }
    /* протяжка с зажатой левой (button 6, когда окно её присылает) — продолжить выделение */
    if (k->button == 6) { if (!U.menu && !FM.id && !U.help && !U.settings) sel_point(k->x, k->y, true); return; }
    if (k->button == 2) { const Hit *th = hit_item(k->x, k->y); if (th && th->act == A_TAB) tab_close(th->arg); return; }
    if (k->button != 1 && k->button != 3) return;
    const Hit *h = hit_item(k->x, k->y);
    if (U.menu) {                                /* открыто меню: пункт или щелчок мимо - закрыть */
        if (h && h->act == A_MENU && k->button == 1) menu_do(h->sub);
        else if (h && h->act == A_BTN && h->sub == B_SETTINGS) U.menu = false;
        else U.menu = false;
        return;
    }
    if (U.mode == M_DELETE || U.mode == M_VLOFF || U.mode == M_UNFRIEND) {
        /* подтверждение: «да» - только кнопкой, щелчок мимо - отмена (как любая другая клавиша) */
        bool yes = h && h->act == A_BTN && h->sub == B_YES && k->button == 1;
        if (U.mode == M_DELETE) { if (yes) delete_message(U.target); U.mode = M_NORMAL; }
        else if (U.mode == M_UNFRIEND) {
            if (yes && rel_find(U.target)) { rel_remove(U.target); ui_notice(TR("удалён из друзей: %s"), un(U.target)); }
            U.mode = M_NORMAL; U.target = 0;
        }
        else vl_off_answer(yes);
        return;
    }
    {
        const CaptchaState *cs = net_captcha();
        if (cs && cs->pending) {                 /* окно капчи: только его кнопки */
            if (h && k->button == 1 && h->act == A_BTN && h->sub == B_CAP_GO && !cs->open) net_captcha_open();
            else if (h && k->button == 1 && h->act == A_BTN && h->sub == B_CAP_NO && !cs->open) { cap_cancelled = true; net_captcha_cancel(); }
            return;
        }
    }
    if (FM.id) { if (k->button == 1) fm_mouse(h); return; }   /* окно-форма модально */
    if (reactors_state() && reactors_state()->on) {   /* «Реакции»: выбор реакции, x или щелчок мимо — закрыть */
        const ReactorsState *rs = reactors_state();
        if (h && k->button == 1 && h->act == A_RVSEL) reactors_open(rs->ch, rs->msg, h->arg);
        else if (!(h && h->act == A_BLOCK) || (h && h->act == A_BTN && h->sub == B_RV_X)) rv_close();
        return;
    }
    if (U.fwd_msg && !U.qs) U.fwd_msg = 0;       /* окно выбора закрыто щелчком — «Переслать» отменено */
    if (U.qs && !U.help && !U.settings) {        /* Ctrl+K: строка списка; щелчок мимо — закрыть */
        if (h && k->button == 1 && h->act == A_QS) qs_go(h->arg); else U.qs = false;
        return;
    }
    if (U.ep && !U.help && !U.settings) {        /* эмодзи: выбрать; щелчок мимо — закрыть */
        if (h && k->button == 1 && h->act == A_EMOJI) ep_apply(h->arg);
        else if (h && k->button == 1 && h->act == A_BTN && (h->sub == B_EP_X || h->sub == B_EMOJI)) button(h->sub, h->x, h->y);
        else if (!(h && h->act == A_BLOCK)) { U.ep = false; U.ep_msg = 0; }
        return;
    }
    if (U.card_uid && !U.help && !U.settings) {  /* карточка пользователя: её кнопки; щелчок мимо — закрыть */
        if (h && k->button == 1 && h->act == A_BTN && (h->sub == B_CARD_MSG || h->sub == B_CARD_ID || h->sub == B_CARD_X || h->sub == B_CARD_BLOCK || h->sub == B_CARD_ADD || h->sub == B_CARD_KICK || h->sub == B_CARD_FULL || h->sub == B_AVBIG || h->sub == B_CARD_IGN || h->sub == B_CARD_NOTE || h->sub == B_NOTE_EDIT)) button(h->sub, h->x, h->y);
        else if (h && k->button == 1 && h->act == A_BTN && h->sub == B_MOD) { U.btn_arg = h->arg; button(B_MOD, h->x, h->y); }
        else { U.card_uid = 0; profile_close(); }
        return;
    }
    if (U.pick && !U.help && !U.settings) {      /* «Выберите друзей»: строка, кнопки; щелчок мимо — закрыть */
        if (k->button != 1 || !h) { U.pick = false; return; }
        if (h->act == A_PICK) {
            int *ix;
            int n = fr_collect(1, U.pick_q, &ix), pos = -1;
            for (int i = 0; i < n; i++) if (ix[i] == h->arg) pos = i;
            free(ix);
            if (pos >= 0 && pos == U.pick_sel) pick_go();   /* второй щелчок по выбранному — создать */
            else if (pos >= 0) U.pick_sel = pos;
        } else if (h->act == A_BTN && (h->sub == B_PICK_GO || h->sub == B_PICK_X || h->sub == B_ADDDM)) button(h->sub, h->x, h->y);
        else U.pick = false;
        return;
    }
    if (U.sq_edit && !(h && h->act == A_BTN && (h->sub == B_SEARCH || h->sub == B_SRCH_CLR))) U.sq_edit = false;
    if (U.sr_focus && !(h && (h->act == A_SRES || (h->act == A_BTN && h->sub == B_SRCH_X)))) U.sr_focus = false;
    if (U.mode == M_FILE && k->button == 1 && !(h && (h->act == A_INPUT || (h->act == A_BTN && (h->sub == B_CANCEL || h->sub == B_ATTACH))))) {
        if (h && h->act == A_BTN && h->sub == B_NOP) { if (h->tip && h->tip < sizeof TIPS / sizeof *TIPS) ui_notice("%s", tr(TIPS[h->tip])); return; }
        vl_leave();                              /* щелчок в другое место (и в пустое) — выбор файла отменён, черновик вернулся */
        if (!h) return;
    }
    if (U.help) { if (h && h->act == A_BTN && k->button == 1) button(h->sub, k->x, k->y); return; }
    if (U.settings) {                            /* настройки: разделы и кнопки */
        if (!h || k->button != 1) return;
        if (h->act == A_SETSEC) { if (h->arg == SS_KEYS) { U.settings = false; U.help = true; U.help_scroll = 0; U.full = true; } else U.set_sec = h->arg; }
        else if (h->act == A_BTN) button(h->sub, h->x, h->y);
        else if (h->act == A_DEV) dev_step(h->sub, h->arg);
        return;
    }
    if (!h) return;
    if (U.mode == M_VLINK) {                     /* ввод ссылки: только его кнопки */
        if (h->act == A_BTN && h->sub == B_VL_CHECK) vl_submit_link();
        else if (h->act == A_BTN && h->sub == B_VL_BACK) vl_leave();
        return;
    }
    if (k->button == 1 && h->act != A_CODECP && h->act != A_IMGV && h->act != A_LINK && h->act != A_POLL && h->act != A_POLLB && h->act != A_COMP && h->act != A_VMSG && h->act != A_SPLIT) sel_point(k->x, k->y, k->shift);   /* №38: начало / Shift — конец */
    if (k->button == 3) {
        if (h->act == A_MSG || h->act == A_REACT) menu_open(MENU_MSG, h->arg, k->x, k->y);
        else if (h->act == A_ROW && h->arg >= 0 && h->arg < U.nrows && (U.rows[h->arg].kind == R_CHAN || U.rows[h->arg].kind == R_HEADER)) {
            U.menu_cid = U.rows[h->arg].id;          /* правый щелчок по каналу/категории — меню канала */
            menu_open(MENU_CHAN, -1, k->x, k->y);
        } else if (h->act == A_ROW && h->arg >= 0 && h->arg < U.nrows && U.rows[h->arg].kind == R_MEMBER) {
            U.menu_uid = U.rows[h->arg].user;    /* правый щелчок по участнику голоса — его меню */
            U.menu_vch = U.rows[h->arg].id;
            U.sel_row = h->arg;
            menu_open(MENU_VMEM, -1, k->x, k->y);
        }
        return;
    }
    switch (h->act) {
    case A_GUILD: select_guild(h->arg); U.focus = F_CHANNELS; break;
    case A_FOLDER: folder_toggle(h->arg); break;
    case A_SLASH:
        if (!h->sub) sl_pick(h->arg);
        else if (SL.cmd >= 0) { const char *nm, *val; SL.sel = h->arg; if (cmd_choice(cmds_state()->c[SL.cmd].opt0 + SL.opt, h->arg, &nm, &val)) sl_next(val); }
        U.focus = F_INPUT;
        break;
    case A_TAB: if (h->sub) tab_close(h->arg); else tab_go(h->arg); break;
    case A_SPLIT: {
        i64 t0 = now_ms();
        if (t0 - PW.last < 450 && PW.last_arg == h->arg) {   /* двойной щелчок — ширина по умолчанию */
            if (h->arg) PW.memb = 0; else PW.side = 0;
            PW.drag = 0; PW.last = 0; U.full = true;
        } else { PW.drag = h->arg + 1; PW.last = t0; PW.last_arg = h->arg; }
        break;
    }
    case A_ROW:
        if (h->arg < 0 || h->arg >= U.nrows) break;
        U.sel_row = h->arg;
        U.focus = F_CHANNELS;
        activate_row();
        break;
    case A_MSG:
        if (h->arg >= 0 && h->arg < S.nmsgs) { U.sel_msg = h->arg; U.focus = F_MESSAGES; U.follow_sel = false; }
        break;
    case A_IMGV: iv_open(h->arg); break;
    case A_LINK: link_click(h->arg); break;
    case A_POLL: poll_click(h->arg >> 4, h->arg & 15); break;
    case A_POLLB: poll_btn(h->arg, h->sub); break;
    case A_COMP: comp_press(h->arg >> 5, h->arg & 31); break;
    case A_VMSG: vmsg_toggle(h->arg); break;
    case A_FILTSHOW: if (h->arg >= 0 && h->arg < S.nmsgs) { filt_shown[filt_n++ % 16] = S.msgs[h->arg].id; } break;
    case A_FPOST: U.focus = F_MESSAGES; FV.sel = h->arg; forum_go(h->arg); break;
    case A_CODECP: code_copy(h->arg); break;
    case A_REACT:
        if (h->arg >= 0 && h->arg < S.nmsgs && h->sub < S.msgs[h->arg].nreacts) {
            char e[160];
            snprintf(e, sizeof e, "%s", S.msgs[h->arg].reacts[h->sub].emoji);
            react(S.msgs[h->arg].id, e);
        }
        break;
    case A_BTN:
        if (h->sub == B_NOP && h->tip == T_DATE && h->arg >= 0 && h->arg < S.nmsgs) { char d[96]; date_full(S.msgs[h->arg].ts, d, sizeof d); ui_notice("%s", d); }
        else if (h->sub == B_NOP) { if (h->tip && h->tip < sizeof TIPS / sizeof *TIPS) ui_notice("%s", tr(TIPS[h->tip])); }
        else { U.btn_arg = h->arg; button(h->sub, h->x, h->y); }
        break;
    case A_INPUT: U.focus = F_INPUT; break;
    case A_OPENCH: if (h->arg >= 0 && h->arg < S.nchans) open_channel(S.chans[h->arg].id); break;
    case A_FRIEND: {
        int *ix;
        int n = fr_collect(U.fr_tab, NULL, &ix);
        for (int i = 0; i < n; i++) if (ix[i] == h->arg) U.fr_sel = i;
        free(ix);
    }
        U.focus = F_MESSAGES; fr_action(h->sub, h->arg, h->x, h->y + 1); break;
    case A_FRTAB: U.fr_tab = h->arg & 3; U.fr_sel = U.fr_scroll = 0; U.focus = F_MESSAGES; break;
    case A_SRES: search_open_hit(h->sub, h->arg); break;
    case A_AC: ac_accept(h->arg); U.focus = F_INPUT; break;
    case A_SPOIL: if (h->arg >= 0 && h->arg < S.nmsgs) spoil_reveal(S.msgs[h->arg].id); break;
    case A_REPLYJ: if (h->arg >= 0 && h->arg < S.nmsgs) jump_to(S.open_ch, S.msgs[h->arg].ref_id); break;
    case A_CHLINK: if (h->arg >= 0 && h->arg < S.nchans) ui_open_channel(S.chans[h->arg].id); break;
    case A_MENT: if (h->arg >= 0 && h->arg < S.nusers) user_card(S.users[h->arg].id, k->x, k->y); break;
    case A_MEMB: { const MemRow *r = ml_row(h->arg); if (r && r->kind == MLR_MEMBER) user_card(r->id, k->x, k->y); break; }
    default: break;
    }
}

static void editor_key(const Key *k);

static void handle_key(const Key *k) {
    if (k->code == K_RESIZE) { U.full = true; U.dirty = true; return; }
    if (k->code == K_MOUSE) {
        if (ptt_capture && (k->button == 8 || k->button == 9)) {   /* №141: боковая кнопка мыши — рация */
            ptt_capture = false;
            int vk = plat_key_vk(k);
            if (vk) voice_ptt_bind(vk);
            U.dirty = true;
            return;
        }
        if (k->button == 7) { PW.drag = 0; return; }   /* отпускание левой: конец протяжки границы панели */
        if (k->button == 6 && PW.drag) {         /* №11: протяжка границы панели */
            int W2 = U.gw;
            if (PW.drag == 1) { int v = k->x - 10 + 1; PW.side = v < 20 ? 20 : v > W2 / 2 ? W2 / 2 : v; }
            else { int v = W2 - k->x; PW.memb = v < 16 ? 16 : v > 50 ? 50 : v; }
            U.full = U.dirty = true;
            return;
        }
        if (LG.on && k->button == 6) return;
        if (LG.on && k->button != 0) { U.dirty = true; if (!U.busy[0]) login_mouse(k); return; }
        mouse(k);
        return;
    }
    if (k->code == K_CHAR && k->ctrl && k->ch == 'q') { g_quit = true; return; }
    if (ptt_capture && ((k->code != K_MOUSE && k->code != K_RESIZE) || (k->code == K_MOUSE && (k->button == 8 || k->button == 9)))) {   /* №141 */
        U.dirty = true;
        ptt_capture = false;
        if (k->code == K_ESC) return;
        int vk = plat_key_vk(k);
        if (vk) voice_ptt_bind(vk);
        else ui_notice(TR("эту клавишу нельзя назначить рации"));
        return;
    }
    if (k->code == K_ENTER && k->alt && !k->ctrl && full_toggle()) return;
    if (!LG.on && k->code == K_CHAR && k->alt && !k->ctrl && k->ch >= '1' && k->ch <= '9' && (int)(k->ch - '1') < TB.n) { tab_go((int)(k->ch - '1')); return; }
#ifdef ASTRA_VIDEO
    if (!LG.on && !U.help && !U.settings && wv_on() && wv_key(k)) return;   /* просмотр трансляции: Alt+W, Alt+F, Alt+стрелки, Esc */
#endif
    if (!LG.on && k->code == K_CHAR && k->ctrl && !k->alt && (k->ch == 'w' || k->ch == 'W' || k->ch == 0x446 || k->ch == 0x426) && TB.n && !(U.focus == F_INPUT && U.in.len)) { tab_close(tab_pos(S.open_ch)); return; }   /* в поле с текстом Ctrl+W стирает слово */   /* №16; в консоли Alt+Enter — перенос строки */
    if (k->code == K_CHAR && k->ctrl && !k->alt && (k->ch == '=' || k->ch == '+' || k->ch == '-' || k->ch == '_' || k->ch == '0')) {
        zoom_step(k->ch == '0' ? 0 : k->ch == '-' || k->ch == '_' ? -1 : 1);
        return;
    }
    if (LG.on) { if (!U.busy[0]) login_key(k); return; }
    if (FM.id && !(net_captcha() && net_captcha()->pending)) { U.dirty = true; fm_key(k); return; }
    if (reactors_state() && reactors_state()->on) {   /* «Реакции»: Up/Down — реакция, Esc — закрыть */
        const ReactorsState *rs = reactors_state();
        Message *rm = msg_find(rs->msg, NULL);
        U.dirty = true;
        if (k->code == K_ESC || !rm) { rv_close(); return; }
        if (k->code == K_UP && rs->sel > 0) reactors_open(rs->ch, rs->msg, rs->sel - 1);
        else if (k->code == K_DOWN && rs->sel < rm->nreacts - 1) reactors_open(rs->ch, rs->msg, rs->sel + 1);
        return;
    }
    if (AVB.uid) { if (k->code == K_ESC || k->code == K_ENTER) avb_close(); U.dirty = true; return; }   /* №101 */
    if (tm_on) { if (k->code == K_ESC) { tm_on = false; thread_members_close(); } U.dirty = true; return; }   /* №97 */
    if (rules_on) { if (k->code == K_ESC) { rules_on = false; guild_rules_close(); } U.dirty = true; return; }   /* №128 */
    if (upd_on) {                                /* обновление: Enter — главная кнопка, Esc — «Позже» */
        const UpdateState *us = update_state();
        U.dirty = true;
        if (k->code == K_ESC) button(us && us->shared_dir ? B_UP_CANCEL : B_UP_LATER, 0, 0);
        else if (k->code == K_ENTER && us && us->phase == UP_AVAILABLE && !us->shared_dir) button(B_UP_APPLY, 0, 0);
        else if (k->code == K_ENTER && us && us->phase == UP_READY) button(B_UP_RESTART, 0, 0);
        return;
    }
    if (CSEL.on && !U.help && !U.settings) { comp_sel_key(k); return; }   /* окно списка бота */
    if (IV.on && !U.help && !U.settings) {       /* просмотр картинки: Left/Right — соседние, Esc — закрыть */
        U.dirty = true;
        if (k->code == K_ESC) iv_close();
        else if (k->code == K_LEFT) iv_step(-1);
        else if (k->code == K_RIGHT) iv_step(1);
        return;
    }
    {
        const CaptchaState *cs = net_captcha();
        if (cs && cs->pending) {
            U.dirty = true;
            if (k->code == K_ENTER && !cs->open && cs->supported) net_captcha_open();
            else if (k->code == K_ESC && !cs->open) { cap_cancelled = true; net_captcha_cancel(); }
            return;
        }
    }
    /* демо: Ctrl+L — показать окно входа (движок входа — login.c) */
    if (O.demo && k->code == K_CHAR && k->ctrl && (k->ch == 'l' || k->ch == 'L')) { ui_login_begin(true); return; }
    if (U.menu) {
        /* меню: Up/Down/Enter/Esc; другая клавиша закрывает его и работает как обычно */
        Message *m = U.menu_kind == MENU_MSG ? msg_find(U.menu_msg, NULL) : NULL;
        int ids[16];
        const char *nm[16];
        bool dg[16];
        int n = menu_items(U.menu_kind, m, ids, nm, dg);
        U.dirty = true;
        if (k->code == K_UP) { if (U.menu_sel > 0) U.menu_sel--; return; }
        if (k->code == K_DOWN) { if (U.menu_sel < n - 1) U.menu_sel++; return; }
        if (k->code == K_ENTER) { menu_do(ids[U.menu_sel < n ? U.menu_sel : 0]); return; }
        U.menu = false;
        if (k->code == K_ESC) return;
    }
    if (U.settings && !U.help) {
        U.dirty = true;
        if (k->code == K_ESC) { U.settings = false; U.full = true; return; }
        if (k->code == K_UP && U.set_sec > 0) { U.set_sec--; return; }
        if (k->code == K_DOWN && U.set_sec < NSEC - 1) { U.set_sec++; return; }
    }
    /* №38: Ctrl+C — скопировать выделенный текст; Esc снимает выделение (и работает дальше как обычно) */
    if (SEL.on && k->code == K_CHAR && k->ctrl && !k->alt && (k->ch == 'c' || k->ch == 'C' || k->ch == 0x441 || k->ch == 0x421)) { sel_copy(); return; }
    if (k->code == K_ESC && (SEL.on || SEL.a_msg)) sel_clear();
    if (k->code == K_CHAR && k->ctrl && !k->alt && (k->ch == 'k' || k->ch == 'K' || k->ch == 0x43B || k->ch == 0x41B) && !U.help && !U.settings) {
        U.qs = !U.qs; U.qs_q[0] = 0; U.qs_sel = 0; U.menu = U.pick = U.ep = false; U.dirty = true;
        return;
    }
    if (U.qs && !U.help && !U.settings) {
        U.dirty = true;
        switch (k->code) {
        case K_ESC: U.qs = false; U.fwd_msg = 0; return;
        case K_UP: if (U.qs_sel > 0) U.qs_sel--; return;
        case K_DOWN: U.qs_sel++; return;
        case K_ENTER: qs_go(U.qs_sel); return;
        case K_TAB: U.qs_sel++; return;
        default: q_key(U.qs_q, sizeof U.qs_q, k); U.qs_sel = 0; return;
        }
    }
    if (U.ep && !U.help && !U.settings) {
        U.dirty = true;
        int cols = EP_COLS;
        switch (k->code) {
        case K_ESC: U.ep = false; U.ep_msg = 0; return;
        case K_LEFT: if (U.ep_sel > 0) U.ep_sel--; return;
        case K_RIGHT: U.ep_sel++; return;
        case K_UP: if (U.ep_sel >= cols) U.ep_sel -= cols; return;
        case K_DOWN: U.ep_sel += cols; return;
        case K_ENTER: { int ix[NEMO + EP_GMAX]; int n = ep_collect(ix); if (U.ep_sel >= 0 && U.ep_sel < n) ep_apply(ix[U.ep_sel]); return; }
        default: q_key(U.ep_q, sizeof U.ep_q, k); U.ep_sel = 0; return;
        }
    }
    if (U.card_uid && !U.help && !U.settings) {
        U.dirty = true;
        if (k->code == K_ENTER) { button(B_CARD_MSG, 0, 0); return; }
        U.card_uid = 0; profile_close();
        if (k->code == K_ESC) return;
    }
    if (U.pick && !U.help && !U.settings) {
        U.dirty = true;
        if (k->code == K_CHAR || k->code == K_BS || k->code == K_UP || k->code == K_DOWN || k->code == K_ENTER || k->code == K_ESC) { pick_key(k); return; }
        U.pick = false;
    }
    if (U.sq_edit && !U.help && !U.settings) {
        U.dirty = true;
        if (k->code == K_ENTER) { search_go(); return; }
        if (k->code == K_ESC) { if (U.sr_open) U.sq_edit = false; else search_off(); return; }
        if (k->code == K_BS || (k->code == K_CHAR && !k->ctrl && !k->alt)) { sq_key(k); return; }
        U.sq_edit = false;
    }
    /* клавиши Discord: Ctrl+I входящие, Ctrl+Shift+M микрофон, Ctrl+Shift+D звук, Alt+Up/Down канал,
     * Alt+Shift+Up/Down непрочитанный канал, Shift+Esc прочитать сервер (Esc в поле ввода — канал) */
    if (!U.help && !U.settings && U.mode == M_NORMAL) {
        u32 c = k->ch;
        if (k->code == K_CHAR && k->ctrl && !k->alt && !k->shift && (c == 'i' || c == 'I' || c == 0x448 || c == 0x428)) { rpanel_open(RP_INBOX); U.dirty = true; return; }
        if (k->code == K_CHAR && k->ctrl && k->shift && (c == 'm' || c == 'M' || c == 0x44C || c == 0x42C)) { voice_toggle_mute(); U.dirty = true; return; }
        if (k->code == K_CHAR && k->ctrl && k->shift && (c == 'd' || c == 'D' || c == 0x432 || c == 0x412)) { voice_toggle_deaf(); U.dirty = true; return; }
        if (k->alt && (k->code == K_UP || k->code == K_DOWN) && U.moving) { move_step(k->code == K_UP ? -1 : 1); U.dirty = true; return; }
        if (k->alt && (k->code == K_UP || k->code == K_DOWN)) { chan_step(k->code == K_UP ? -1 : 1, k->shift); U.dirty = true; return; }
        if (k->code == K_ESC && U.moving) { U.moving = false; ui_notice(TR("перестановка закончена")); U.dirty = true; return; }
        if (k->code == K_ESC && k->shift) {
            Channel *oc = chan_find(S.open_ch);
            if (oc && oc->guild_id) { ack_guild(oc->guild_id); ui_notice(TR("сервер отмечен прочитанным")); }
            else if (cur_guild_id()) { ack_guild(cur_guild_id()); ui_notice(TR("сервер отмечен прочитанным")); }
            U.dirty = true;
            return;
        }
    }
    /* Ctrl+F — поиск, Ctrl+O — отправить файл (кириллическая раскладка: а / щ) */
    if (k->code == K_CHAR && k->ctrl && !k->alt && !U.help && !U.settings && (U.mode == M_NORMAL || U.mode == M_REPLY)) {
        u32 c = k->ch;
        if (c == 'f' || c == 'F' || c == 0x430 || c == 0x410) { search_begin(); U.dirty = true; return; }
        if (c == 'o' || c == 'O' || c == 0x449 || c == 0x429) { file_enter(); U.dirty = true; return; }
    }
    if ((U.sr_open || U.rp) && U.mode == M_NORMAL && !U.help && !U.settings && !U.menu) {
        if (k->code == K_ESC && (U.sr_focus || U.focus != F_INPUT || !U.in.len)) { search_off(); U.dirty = true; return; }
        if (U.sr_focus) {
            U.dirty = true;
            int kind = panel_kind();
            int cnt = kind == MP_SEARCH ? search_state()->nhits : kind == MP_PINS ? pins_state()->n : kind == MP_MENT ? mentions_state()->n : U.inbox_n;
            switch (k->code) {
            case K_UP: if (U.sr_sel > 0) U.sr_sel--; return;
            case K_DOWN: if (U.sr_sel < cnt - 1) U.sr_sel++; return;
            case K_HOME: U.sr_sel = 0; return;
            case K_END: U.sr_sel = cnt - 1; if (kind == MP_PINS && pins_state()->more) pins_more(); return;
            case K_PGUP:
#ifdef MC_PACKA
                if (kind == MP_SEARCH && U.sr_sel == 0 && search_state()->page > 0) {
                    search_page(search_state()->page - 1); U.sr_sel = U.sr_scroll = 0; return;
                }
#endif
                U.sr_sel -= 4; if (U.sr_sel < 0) U.sr_sel = 0; return;
            case K_PGDN:
#ifdef MC_PACKA
                if (kind == MP_SEARCH && U.sr_sel >= cnt - 1 && search_state()->page + 1 < search_state()->page_count) {
                    search_page(search_state()->page + 1); U.sr_sel = U.sr_scroll = 0; return;
                }
#endif
                U.sr_sel += 4; if (U.sr_sel > cnt - 1) U.sr_sel = cnt - 1; return;
            case K_LEFT: case K_RIGHT:
                if (U.rp == RP_INBOX) { U.inbox_tab ^= 1; U.sr_sel = U.sr_scroll = 0; if (U.inbox_tab) mentions_open(); else mentions_close(); }
                return;
            case K_ENTER: search_open_hit(kind, kind == 3 ? inbox_nth(U.sr_sel) : U.sr_sel); return;
            default: U.sr_focus = false; break;   /* остальное — как обычно (Tab, набор текста) */
            }
        }
    }
    if (k->code == K_F1) { U.help = !U.help; U.help_scroll = 0; U.full = true; U.dirty = true; return; }
    /* смена голоса: F11 (в Windows Terminal F11 — полный экран, поэтому ещё Alt+V / Alt+м) */
    if (k->code == K_F11 || (k->code == K_CHAR && k->alt && !k->ctrl && (k->ch == 'v' || k->ch == 'V' || k->ch == 0x43C || k->ch == 0x41C))) {
        fx_cycle();
        return;
    }
#ifdef ASTRA_VIDEO
    /* камера: F12 или Alt+C (Alt+с) — сразу, выбранной камерой; окно выбора — кнопкой []/ */
    if (k->code == K_F12 || (k->code == K_CHAR && k->alt && !k->ctrl && (k->ch == 'c' || k->ch == 'C' || k->ch == 0x441 || k->ch == 0x421))) { cu_toggle(false); return; }
    if (k->code == K_CHAR && k->alt && !k->ctrl && (k->ch == 'e' || k->ch == 'E' || k->ch == 0x443 || k->ch == 0x423)) { cu_anon_toggle(); return; }   /* Alt+E (Alt+у) */
#endif
    if (k->code >= K_F2 && k->code <= K_F10) { voice_keys(k); return; }
    if (U.help && k->code == K_ESC) { U.help = false; U.full = true; U.dirty = true; return; }
    /* открытая справка листается стрелками и PgUp/PgDn (верхняя граница - в render) */
    if (U.help && (k->code == K_UP || k->code == K_DOWN || k->code == K_PGUP || k->code == K_PGDN)) {
        int step = k->code == K_UP || k->code == K_DOWN ? 1 : (U.H > 10 ? U.H - 8 : 4);
        U.help_scroll += k->code == K_UP || k->code == K_PGUP ? -step : step;
        if (U.help_scroll < 0) U.help_scroll = 0;
        U.dirty = true;
        return;
    }
    U.dirty = true;
    if (U.busy[0]) return;                       /* защитно: идёт проверка VLESS */
    if (U.mode == M_VLOFF) {
        vl_off_answer(k->code == K_CHAR && !k->ctrl && !k->alt && (k->ch == 'y' || k->ch == 'Y' || k->ch == 0x43D /* н */));
        return;
    }
    if (U.mode == M_UNFRIEND) {
        if (k->code == K_CHAR && !k->ctrl && !k->alt && (k->ch == 'y' || k->ch == 'Y' || k->ch == 0x43D /* н */) && rel_find(U.target)) {
            rel_remove(U.target);
            ui_notice(TR("удалён из друзей: %s"), un(U.target));
        }
        U.mode = M_NORMAL;
        U.target = 0;
        return;
    }
    if (U.mode == M_VLINK || U.mode == M_FILE) {
        /* ввод ссылки (пути к файлу): только правка строки, Enter - проверить, Esc - назад; колонки не переключаются */
        U.focus = F_INPUT;
        if (k->code == K_ENTER && U.mode == M_FILE) file_submit();
        else if (k->code == K_ESC && U.mode == M_FILE) vl_leave();
        else if (k->code == K_ENTER) vl_submit_link();
        else if (k->code == K_ESC) vl_leave();
        else if (k->code == K_CHAR && !k->ctrl && (k->ch == '\n' || k->ch == '\r' || k->ch == '\t')) return;
        else if (k->code == K_CHAR || k->code == K_BS || k->code == K_DEL || k->code == K_LEFT ||
                 k->code == K_RIGHT || k->code == K_HOME || k->code == K_END) {
            editor_key(k);
            if (U.mode == M_FILE && (k->code == K_CHAR || k->code == K_BS || k->code == K_DEL)) U.file_err[0] = 0;
        }
        return;
    }
    if (vl_card_key(k)) return;
    if (U.mode == M_DELETE) {
        if (k->code == K_CHAR && (k->ch == 'y' || k->ch == 0x43D /* н */)) delete_message(U.target);
        U.mode = M_NORMAL;
        return;
    }
    if (sl_key(k)) { U.dirty = true; return; }   /* №57 */
    if (k->code == K_TAB && U.focus == F_INPUT) { AcState a; if (ac_token(&a)) { ac_accept(U.ac_sel); return; } }
    if ((k->code == K_TAB || k->code == K_BTAB) && k->ctrl && TB.n >= 2) {   /* №9: Ctrl+Tab / Ctrl+Shift+Tab */
        int p0 = tab_pos(S.open_ch), d = k->code == K_BTAB || k->shift ? -1 : 1;
        tab_go(((p0 < 0 ? 0 : p0) + d + TB.n) % TB.n);
        return;
    }
    if (k->code == K_TAB || k->code == K_BTAB) {
        U.focus = (U.focus + (k->code == K_TAB ? 1 : 3)) % 4;
        if (U.focus == F_MESSAGES && U.sel_msg < 0 && S.nmsgs) U.sel_msg = S.nmsgs - 1;
        if (U.focus == F_MESSAGES) U.follow_sel = true;
        return;
    }
    if (U.focus == F_MESSAGES || U.focus == F_INPUT) {
        Channel *fc = chan_find(S.open_ch);
        if (fc && arch_view == fc->id && k->code == K_ESC) { arch_view = 0; forum_close(); return; }
        if ((is_forum(fc) || (fc && arch_view == fc->id)) && forum_key(k)) return;
    }
    if (k->code == K_PGUP || k->code == K_PGDN) {
        int step = U.H > 10 ? U.H - 8 : 4;
        U.follow_sel = false;                    /* листаем свободно; Up/Down/End вернут привязку */
        if (k->code == K_PGUP) {
            U.msg_scroll += step;
            fetch_messages(true);                /* дошли до верха — подгружаем более старые */
        } else {
            U.msg_scroll -= step;
            if (U.msg_scroll < 0) U.msg_scroll = 0;
        }
        return;
    }
    switch (U.focus) {
    case F_GUILDS:
        if (k->code == K_UP) select_guild(U.sel_guild - 1);
        else if (k->code == K_DOWN) select_guild(U.sel_guild + 1);
        else if (k->code == K_HOME) select_guild(0);
        else if (k->code == K_END) select_guild(S.nguilds);
        else if (k->code == K_ENTER || k->code == K_RIGHT) {
            /* Enter на «домой» при открытой беседе — экран «Друзья», как щелчок по «Друзья» */
            if (k->code == K_ENTER && U.sel_guild == 0 && S.open_ch) { button(B_FRIENDS, 0, 0); U.focus = F_MESSAGES; }
            else U.focus = F_CHANNELS;
        }
        return;
    case F_CHANNELS:
        if (k->code == K_UP) move_row(-1);
        else if (k->code == K_DOWN) move_row(1);
        else if (k->code == K_LEFT || k->code == K_ESC) U.focus = F_GUILDS;
        else if (k->code == K_ENTER || k->code == K_RIGHT) activate_row();
        else if (k->code == K_CHAR && k->ch == '+' && !cur_guild_id() &&
                 !(U.sel_row >= 0 && U.sel_row < U.nrows && U.rows[U.sel_row].kind == R_MEMBER)) pick_open();   /* новая ЛС */
        else if (k->code == K_CHAR && (k->ch == '+' || k->ch == '-' || k->ch == '=')) {
            if (U.sel_row >= 0 && U.sel_row < U.nrows && U.rows[U.sel_row].kind == R_MEMBER) {
                u64 uid = U.rows[U.sel_row].user;
                float v = voice_get_volume(uid) + (k->ch == '-' ? -0.1f : 0.1f);
                voice_set_volume(uid, v);
            }
        }
        return;
    case F_MESSAGES: {
        if (friends_screen()) { friends_key(k); return; }
        if (!S.nmsgs) { if (k->code == K_ESC) U.focus = F_INPUT; return; }
        if (U.sel_msg < 0 || U.sel_msg >= S.nmsgs) U.sel_msg = S.nmsgs - 1;
        Message *m = &S.msgs[U.sel_msg];
        if (k->code == K_UP || k->code == K_DOWN || k->code == K_END) U.follow_sel = true;
        if (k->code == K_UP) { if (U.sel_msg > 0) U.sel_msg--; else fetch_messages(true); }
        else if (k->code == K_DOWN) { if (U.sel_msg < S.nmsgs - 1) U.sel_msg++; }
        else if (k->code == K_END) { U.sel_msg = S.nmsgs - 1; U.msg_scroll = 0; }
        else if (k->code == K_ESC || k->code == K_ENTER) U.focus = F_INPUT;
        else if (k->code == K_CHAR && !k->ctrl) {
            u32 c = k->ch;
            if (c == 'r' || c == 0x43A /* к */) msg_action(m, MI_REPLY);
            else if (c == 'e' || c == 0x443 /* у */) msg_action(m, MI_EDIT);
            else if (c == 'd' || c == 0x432 /* в */) msg_action(m, MI_DELETE);
            else if (c == '+') msg_action(m, MI_REACT);
            else if (c == 'v' || c == 0x43C /* м */) txt_toggle(m->id);
            else if ((c == '[' || c == ']') && txt_find(m->id) >= 0) {
                int tk = txt_find(m->id);
                txt_open[tk].scroll += c == '[' ? -5 : 5;
                if (txt_open[tk].scroll < 0) txt_open[tk].scroll = 0;
            }
            else if (c >= '1' && c <= '9' && m->poll && (int)(c - '1') < m->poll->n) poll_click((int)(m - S.msgs), (int)(c - '1'));
            else if (c >= '1' && c <= '9') { char d[2] = { (char)c, 0 }; react(m->id, d); }
        }
        return;
    }
    case F_INPUT:
    default:
        break;
    }
    editor_key(k);
}

/* Редактор строки ввода (и ввод ссылки VLESS). */
static void editor_key(const Key *k) {
    {
        AcState a;
        if (ac_token(&a)) {
            if (k->code == K_UP) { U.ac_sel = U.ac_sel > 0 ? U.ac_sel - 1 : a.n - 1; return; }
            if (k->code == K_DOWN) { U.ac_sel = (U.ac_sel + 1) % a.n; return; }
            if (k->code == K_TAB || (k->code == K_ENTER && !k->shift)) { ac_accept(U.ac_sel); return; }
            if (k->code == K_ESC) { U.ac_hide = true; return; }
        }
    }
    if (k->code == K_CHAR || k->code == K_BS) { U.ac_hide = false; U.ac_sel = 0; }
    switch (k->code) {
    case K_ENTER: submit(); return;
    case K_ESC:
        if (U.mode != M_NORMAL) cancel_mode();
        else { send_ack(true); U.focus = F_CHANNELS; }   /* Esc, как в Discord, — канал прочитан */
        return;
    case K_LEFT: U.cur = prev_cp(U.in.p, U.cur); return;
    case K_RIGHT: U.cur = next_cp(U.in.p, U.in.len, U.cur); return;
    case K_HOME: U.cur = 0; return;
    case K_END: U.cur = U.in.len; return;
    case K_BS: if (U.cur) in_delete_range(prev_cp(U.in.p, U.cur), U.cur); return;
    case K_DEL: if (U.cur < U.in.len) in_delete_range(U.cur, next_cp(U.in.p, U.in.len, U.cur)); return;
    case K_UP:
        if (!U.in.len && U.mode == M_NORMAL) {
            Message *m = last_own();
            if (S.detached) ui_notice(TR("сначала вернитесь к настоящему: внизу ленты"));   /* М5: последнее своё не загружено */
            else if (m && edit_src(m)) { U.mode = M_EDIT; U.target = m->id; edit_set(edit_src(m)); }
            else if (m) ui_notice(TR("текст этого сообщения недоступен для правки"));
        } else if (U.in.len == 0) U.focus = F_MESSAGES;
        return;
    case K_DOWN: return;
    case K_CHAR:
        if (k->ctrl) {
            if (k->ch == 'u') in_clear();
            else if (k->ch == 'w') { size_t e = U.cur, s = e; while (s && U.in.p[s - 1] == ' ') s--; while (s && U.in.p[s - 1] != ' ') s--; in_delete_range(s, e); }
            return;
        }
        if (k->ch == '\n' && U.mode == M_REACT) return;
        if (k->alt) return;                      /* Alt+клавиша — сочетание, а не ввод текста */
        in_insert(k->ch);
        if (O.typing && S.open_ch && !O.demo && now_ms() - U.last_typing_sent > 8000 &&
            (U.mode == M_NORMAL || U.mode == M_REPLY || U.mode == M_EDIT)) {
            char path[96];
            snprintf(path, sizeof path, "/channels/%llu/typing", (unsigned long long)S.open_ch);
            http_async("POST", path, NULL, NULL, NULL);
            U.last_typing_sent = now_ms();
        }
        return;
    default: return;
    }
}

/* Паник-кей. Каждое нажатие сначала отдаётся panic_key(); на третьем нажатии подряд он не возвращается.
 * true от panic_key() значит «это паник-клавиша, нажатие засчитано»: её обычное действие придерживается
 * (до 2 нажатий, 1,5 с после последнего), чтобы тройное нажатие не успело, например, отправить сообщение
 * Enter'ом или переключить колонку Tab'ом. Не было третьего — придержанные нажатия выполняются как обычно
 * (с задержкой), другая клавиша выпускает их сразу и по порядку. Движение мыши — не нажатие: оно
 * в паник-модуль не идёт и серию не сбрасывает; щелчок и колесо — идут (это другая «клавиша»). */
#define PANIC_HOLD_MS 1500
static void pk_release(void) {
    int n = U.npk;
    U.npk = 0;
    U.pk_until = 0;
    for (int i = 0; i < n && !g_quit; i++) handle_key(&U.pk[i]);
}
void ui_handle_input(void) {
    Key k[64];
    int n;
    while ((n = term_read(k, 64)) > 0) for (int i = 0; i < n && !g_quit; i++) {
        if (k[i].code == K_MOUSE && k[i].button == 0) { handle_key(&k[i]); continue; }
        bool is_panic = panic_key(&k[i]);
        if (is_panic && k[i].code != K_RESIZE) {
            if (U.npk == 2) pk_release();        /* окно паник-модуля уже сброшено */
            U.pk[U.npk++] = k[i];
            U.pk_until = now_ms() + PANIC_HOLD_MS;
            continue;
        }
        if (U.npk) pk_release();
        handle_key(&k[i]);
    }
}

void ui_tick(void) {
    i64 now = now_ms();
    {
        static bool ep_was;
        if (ep_was && !U.ep) emoji_close();      /* №32: эмодзи сервера живут, пока открыт пикер */
#ifdef ASTRA_VIDEO
        {
            static i64 ws_next;
            GoWatchState nw;
            golive_wstate(&nw);
            if (O.demo && demo_watch) {          /* --demo: подставной просмотр для снимков (сети нет) */
                static i64 dw0;
                static u64 dwu;
                if (dwu != demo_watch) { dwu = demo_watch; dw0 = now; }
                nw = (GoWatchState){ .phase = now - dw0 < 600 ? GW_CONNECTING : GW_LIVE, .user = demo_watch, .apkts = (now - dw0) / 20, .vpkts = (now - dw0) / 8 };
            }
            if (nw.phase != WS.phase || nw.user != WS.user || (nw.phase != GW_OFF && now >= ws_next)) { WS = nw; U.dirty = true; ws_next = now + 1000; }
            if (WS.phase != GW_OFF) {
                VoiceStatus v7;
                voice_status(&v7);
                if (v7.phase == VS_IDLE) wv_stop();
            }
            if (WV.cam) {                        /* камера крупно: участник выключил камеру или звонок кончился */
                VoiceStatus v7;
                voice_status(&v7);
                const VoiceState *l7[32];
                int k7 = v7.phase == VS_IDLE ? 0 : voice_members(v7.channel_id, l7, 32);
                bool has = false;
                for (int i = 0; i < k7; i++) if (l7[i]->user_id == WV.cam && cu_user(l7[i])) has = true;
                if (!has) { if (k7) ui_notice("%s", TR("камера выключена у участника")); wv_stop(); }
            }
        }
        if (sh_running) {
            ShareState ns2;
            share_state(&ns2);
            if (ns2.fps_out != SHS.fps_out || ns2.mbps != SHS.mbps || ns2.active != SHS.active || ns2.paused != SHS.paused || strcmp(ns2.cover, SHS.cover)) U.dirty = true;
            if (strcmp(ns2.pause_why, SHS.pause_why)) U.dirty = true;   /* на паузе эфир идёт (последний кадр) — golive_pause не зовём */
            if (ns2.audio_err[0] && strcmp(ns2.audio_err, SHS.audio_err) && strcmp(ns2.audio_err, "без звука"))
                ui_notice("%s", tr(ns2.audio_err));   /* звук не пошёл (старая Windows) — эфир без звука */
            if (ns2.audio != SHS.audio) U.dirty = true;
            if (ns2.active && ns2.minimized != SHS.minimized) {   /* свернули/развернули источник — сказать в окне */
                if (ns2.minimized) ui_notice("%s", TR("Окно трансляции свёрнуто - зрители видят последний кадр. Разверните окно, и эфир продолжится"));
                else ui_notice("%s", TR("Окно развёрнуто - трансляция продолжается"));
            }
            SHS = ns2;
            GoLiveState gl;
            golive_state(&gl);
            if (gl.phase != GL_phase) { GL_prev = GL_phase; GL_phase = gl.phase; snprintf(GL_err, sizeof GL_err, "%s", gl.err); U.dirty = true; }
            if (gl.waiting != GL_wait) { GL_wait = gl.waiting; U.dirty = true; }
            VoiceStatus v6;
            voice_status(&v6);
            if (sh_gl_wait && ns2.active && !ns2.choosing) {   /* Wayland: источник выбран — теперь Go Live */
                char ge[128] = "";
                sh_gl_wait = false;
                if (!golive_start(sh_gid, sh_ch, ge, sizeof ge)) share_ui_stop(ge);
                else { GL_phase = GL_prev = GL_CREATING; ui_notice("%s", TR("трансляция началась")); }
                U.dirty = true;
            }
            if (!sh_running) { /* остановлено выше */ }
            else if (!ns2.active) share_ui_stop(ns2.reason[0] ? ns2.reason : "трансляция остановлена");
            else if (!sh_gl_wait && GL_phase == GL_OFF && GL_prev != GL_OFF) share_ui_stop("трансляция остановлена: Discord завершил эфир");
            else if (v6.phase == VS_IDLE) share_ui_stop("трансляция остановлена: вы вышли из звонка");
            else if (v6.channel_id != sh_ch) share_ui_stop("трансляция остановлена: другой канал");
        }
        {                                        /* своя камера: индикатор раз в секунду, сбой — словами */
            static i64 cs_next, an_next;
            CamState nc;
            if (O.demo) {
                VoiceStatus v9;
                voice_status(&v9);
                if (v9.phase == VS_IDLE) cu_demo = false;
                nc = cu_demo ? (CamState){ .phase = CAM_LIVE, .w = 1280, .h = 720, .fps = 30, .fps_out = 30 } : (CamState){ .phase = CAM_OFF };
            } else cam_state(&nc);
            if (nc.phase != CS.phase) {
                if (nc.phase == CAM_FAILED && nc.err[0]) ui_notice(TR("камера выключена: %s"), nc.err);
                CS = nc; U.full = U.dirty = true;
            } else if (nc.phase != CAM_OFF && now >= cs_next) {
                if (nc.h != CS.h || (int)(nc.fps_out + 0.5) != (int)(CS.fps_out + 0.5)) U.dirty = true;
                CS = nc; cs_next = now + 1000;
            }
            if (cu_anim && now >= an_next) { an_next = now + 100; U.dirty = true; }   /* --demo: узор плиток движется */
        }
#endif
        {
            const UpdateState *us = update_state();
            static int up_was = -1;
            if (us && us->phase != up_was) {
                up_was = us->phase;
                U.dirty = true;
                if (us->phase == UP_AVAILABLE && strcmp(upd_shown, us->version) && strcmp(upd_later, us->version) && !LG.on) {
                    snprintf(upd_shown, sizeof upd_shown, "%s", us->version);
                    upd_on = true;
                }
                if (us->phase == UP_DOWNLOADING || us->phase == UP_READY || (us->phase == UP_FAILED && upd_on)) upd_on = !LG.on;
            }
            if (us && us->phase == UP_DOWNLOADING) U.dirty = true;   /* полоса загрузки */
            static bool told;                    /* один раз за всё время: о проверке обновлений */
            if (!told && S.ready && !LG.on) {
                told = true;
                char b2[4] = "";
                if (!O.demo && !O.no_save && keyring_load("upd_told", b2, sizeof b2) <= 0 && update_auto()) {
                    ui_notice("%s", TR("Astra DS раз в сутки проверяет обновления на GitHub. Отключить - Настройки > Обновления или --no-update"));
                    keyring_save("upd_told", "1", 1);
                }
            }
        }
        /* №57: индекс команд — пока в поле «/...» или вводятся опции; иначе освобождён */
        bool slw = SL.cmd >= 0 || sl_typing();
        if (SL.cmd >= 0 && SL.cid && SL.cid != S.open_ch) { sl_reset(); slw = false; }
        if (slw && SL.cid != S.open_ch && S.open_ch) { cmds_open(S.open_ch); SL.cid = S.open_ch; U.dirty = true; }
        else if (!slw && SL.cid) { cmds_close(); SL.cid = 0; }
        ep_was = U.ep;
        static int vm_pos = -1;
        static u64 vm_mid;
        static bool vm_act, vm_load;
        const VoiceMsgState *vm = vmsg_state();
        if (vm && (vm->pos_ms / 250 != vm_pos || vm->mid != vm_mid || vm->active != vm_act || vm->loading != vm_load)) {
            vm_pos = vm->pos_ms / 250; vm_mid = vm->mid; vm_act = vm->active; vm_load = vm->loading;
            U.dirty = true;
        }
    }
    {
        static int lv_was = -2;
        bool here = U.settings && !U.help && !LG.on && U.set_sec == SS_VOICE;
        if (!here) DEV.got = false;              /* при следующем входе — свежий список устройств */
        if (ptt_capture && !here) ptt_capture = false;
        { const NoteState *nst = note_state(); if (nst && nst->uid && nst->uid != U.card_uid && !FM.id) note_close(); }

        if (mic_test_on && !here) { mic_test_on = false; voice_mic_test(false); }
        int lv = mic_test_on ? voice_mic_level() : -1;
        if (lv != lv_was) { lv_was = lv; if (here) U.dirty = true; }
    }
    if (U.npk && now >= U.pk_until) pk_release();
    /* VLESS меняется не по событию gateway (процесс Xray, проверки): сравнить и перерисовать */
    static int vl_was = -1, vl_ms_was = -2;
    static u32 vl_err_was;
    VlessState vst = vless_state();
    int vms = vless_latency_ms();
    const char *ve = vless_error();
    u32 veh = fnv(ve ? ve : "", ve ? strlen(ve) : 0);
    if ((int)vst != vl_was || vms != vl_ms_was || veh != vl_err_was) {
        if (vst != VL_DOWN) U.vl_note[0] = 0;
        vl_was = (int)vst;
        vl_ms_was = vms;
        vl_err_was = veh;
        U.dirty = true;
    }
    if (U.ack_at && now >= U.ack_at) { U.ack_at = 0; send_ack(false); U.dirty = true; }
    /* капча: обратная связь, когда окно проверки закрылось (иначе непонятно, прошла ли) */
    {
        /* по исходу последней проверки (net.c: CaptchaState.outcome), сообщение — один раз при смене исхода */
        static bool cap_was;
        static u8 cap_out;
        static char cap_what[64];
        const CaptchaState *cs = net_captcha();
        bool pend = cs && cs->pending;
        if (cs && cs->what[0]) snprintf(cap_what, sizeof cap_what, "%s", cs->what);
        u8 out = cs ? cs->outcome : CAP_NONE;
        if (out != cap_out) {
            if (out == CAP_PASSED) ui_notice(TR("Проверка пройдена, повторяю действие: %s..."), cap_what[0] ? cap_what : TR("запрос к Discord"));
            else if (out == CAP_FAILED) {
                char *e = san(cs->error[0] ? cs->error : TR("окно закрыто"), false);
                ui_notice(TR("Проверка не пройдена: %s. Enter - открыть снова, Esc - отменить"), e);
                free(e);
            }
            /* CAP_CANCELLED — итог уже пишет net.c («...: отменено - проверка не пройдена») */
            cap_out = out;
            U.dirty = true;
        }
        cap_cancelled = false;
        if (pend != cap_was) U.dirty = true;
        cap_was = pend;
    }
    static bool typing_was;
    bool typing = S.typing_until > now;
    if (typing != typing_was) { typing_was = typing; U.dirty = true; }
    static bool notice_was;
    bool notice = U.notice_until > now;
    if (notice != notice_was) { notice_was = notice; U.dirty = true; }
    static int last_vs_hash;
    VoiceStatus vs;
    voice_status(&vs);
    int h = vs.phase * 7 + vs.nspeaking * 131 + (vs.mic_live ? 3 : 0) + vs.ping / 10 + (vs.self_mute ? 11 : 0) + (vs.self_deaf ? 13 : 0) + (int)vs.dave * 17 + (vs.error[0] ? 19 : 0);
    for (int i = 0; i < vs.nspeaking; i++) h += (int)(vs.speaking[i] % 1000);
    if (h != last_vs_hash) { last_vs_hash = h; rebuild_rows(); U.dirty = true; }
}

i64 ui_next_wake(void) {
    i64 t = INT64_MAX, now = now_ms();
    if (U.ack_at) t = U.ack_at;
    if (S.typing_until > now && S.typing_until < t) t = S.typing_until;
    if (U.notice_until > now && U.notice_until < t) t = U.notice_until;
    if (U.npk && U.pk_until < t) t = U.pk_until;
    return t;
}

void ui_on_ready(void) {
    if (O.cli) { cli_on_ready(); return; }
    if (U.sel_guild > S.nguilds) U.sel_guild = 0;
    rebuild_rows();
    if (S.open_ch && chan_find(S.open_ch)) {
        u64 ch = S.open_ch;
        S.open_ch = 0;
        open_channel(ch);
    } else S.open_ch = 0;
    U.dirty = true;
}

void ui_on_message(u64 channel_id, bool mention) {
    if (O.cli) { cli_on_message(channel_id, mention); return; }
    if (channel_id == S.open_ch) {
        U.ack_at = now_ms() + 1500;
        if (U.msg_scroll == 0) U.sel_msg = U.focus == F_MESSAGES ? U.sel_msg : -1;
    } else if (mention) term_write("\a", 1);
    if (!cur_guild_id() || mention) rebuild_rows();
    U.dirty = true;
}

static void ui_drop_file(const char *path) {
    if (!path || !*path || LG.on) return;
    if (!chan_find(S.open_ch)) { ui_notice(TR("перетащите файл в открытый канал или беседу")); return; }
    if (U.mode != M_FILE) file_enter();
    if (U.mode != M_FILE) return;
    in_clear();
    for (const char *q = path; *q; ) { u32 c; q += utf8_dec(q, &c); in_insert(c); }
    U.focus = F_INPUT;
    U.dirty = true;
}
void ui_init(void) {
    g_ui_drop = ui_drop_file;
#ifdef ASTRA_VIDEO
    share_self_excluded(term_capture_exclude(true));   /* окно видео-версии не попадает ни в чей захват; не вышло — свои окна считаются перекрытием */
#endif
    if (!lang_inited) ui_lang_init(-1);
    U.focus = F_GUILDS;
    U.full = true;
    U.dirty = true;
    U.hx = U.hy = -1;
}
void ui_shutdown(void) {
    buf_wipe(&LG.user); buf_wipe(&LG.pass); buf_wipe(&LG.code); buf_wipe(&LG.link);
    wipe(qr_m, sizeof qr_m); wipe(qr_for, sizeof qr_for);
    buf_wipe(&U.in);
    buf_wipe(&U.draft);
    for (int i = 0; i < 16; i++) draft_drop(i);
    fm_reset();                                  /* буферы полей окна-формы */
    buf_free(&U.frame);
    buf_free(&U.line);
    free(U.rows);
    free(U.hash);
    free(U.cell);
    free(U.ml);
    free(seg_hash);
    seg_hash = NULL;
    seg_n = 0;
    fine_cache_drop();
    U.rows = NULL; U.hash = NULL; U.cell = NULL; U.ml = NULL;
    U.mlcap = U.caprows = U.nrows = 0;
    U.gw = U.gh = 0;
}
