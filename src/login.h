/* login.c — движок входа в аккаунт Discord (без интерфейса): пароль, 2FA, SMS, проверка нового места
 * входа, восстановление отключённого аккаунта, вход по QR-коду (remote auth), запасной путь — токен.
 *
 * Экраны рисует ui.c по снимку login_view(); ввод пароля, кода и токена ui.c может вести прямо в закреплённых
 * буферах login_field() и отправлять login_submit(), или передавать строки login_start_password() /
 * login_submit_code(). При каждой смене состояния движок зовёт ui_dirty(). Всё асинхронно: главный цикл
 * зовёт login_waitables() / login_next_wake() / login_service(), опроса без запроса нет.
 *
 * Парольная ветка и QR работают одновременно, как на экране входа Discord: QR показывается рядом с формой,
 * кто первым получил токен — тот и победил (stage = LG_DONE, токен уже в secret.c и, без --no-save, в
 * хранилище). Капча (hCaptcha) решается в окне WebView2 (captcha.c, Windows); где его нет — интерфейс предлагает QR.
 *
 * Безопасность: пароль, коды, тикеты, тело запросов и ответов, расшифрованный токен — только в закреплённых
 * страницах этого модуля (plat_lock), затираются сразу после использования; закрытый ключ RSA — в
 * защищённой куче OpenSSL (CRYPTO_secure_malloc, закреплена). Пароль уходит только в POST /auth/login.
 * Все строки из сети — через san(). Ни вывода, ни файлов. Маршрут — тот же, что у всего трафика (VLESS). */
#ifndef LOGIN_H
#define LOGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "plat.h"

/* Этап парольной ветки (форма слева на экране входа Discord). */
typedef enum {
    LG_FORM,        /* ввод email/телефона и пароля (начальный; сюда же возвращают ошибки полей) */
    LG_BUSY,        /* запрос идёт: busy_text («Вход...», «Проверка кода...»); ввод заблокирован */
    LG_CAPTCHA,     /* Discord требует капчу: «Пройти проверку» (окно WebView2), рядом — вход по QR */
    LG_MFA,         /* нужен код 2FA: mfa_methods, выбранный mfa_method; для SMS — sms_sent, phone */
    LG_EMAIL,       /* новое место входа: «проверьте почту»; подтвердить письмом и login_retry(), или вставить ссылку из письма в LF_CODE и login_submit() */
    LG_PHONE,       /* новое место входа, телефон (код 70007): код из SMS в LF_CODE и login_submit() */
    LG_UNDELETE,    /* аккаунт отключён (20013) или помечен к удалению (20011): login_undelete() — восстановить и войти */
    LG_TOKEN,       /* запасной путь: токен в LF_TOKEN и login_submit() (проверка GET /users/@me) */
    LG_DONE         /* токен в secret.c: закрыть экран входа, login_finish(), дальше gw_start() */
} LoginStage;

/* Поля ввода в закреплённой памяти (ui.c редактирует строку на месте, всегда с завершающим нулём). */
typedef enum { LF_LOGIN, LF_PASSWORD, LF_CODE, LF_TOKEN, LF_COUNT } LoginField;
enum { LF_NONE = -1 };

/* Способы 2FA (битовая маска mfa_methods). */
enum { LM_TOTP = 1, LM_SMS = 2, LM_BACKUP = 4, LM_WEBAUTHN = 8 };

/* QR-ветка (справа на экране входа Discord). */
typedef enum {
    QR_OFF,         /* не запущен (login_qr_start) */
    QR_CONNECTING,  /* ключ RSA, соединение со шлюзом, рукопожатие */
    QR_SHOW,        /* показать QR-код ссылки qr_url; подпись «Войти по QR-коду» */
    QR_SCANNED,     /* отсканирован: «Проверьте телефон» + qr_user_name, аватар qr_user_id/qr_avatar (img_avatar) */
    QR_EXPIRED,     /* сессия истекла или телефон отменил: «QR-код устарел» + кнопка login_qr_start() */
    QR_ERROR        /* шлюз недоступен: qr_text + кнопка login_qr_start() */
} QrState;

typedef struct {
    LoginStage stage;
    const char *busy_text;      /* для LG_BUSY, по-русски */
    const char *error;          /* ошибка последнего действия, по-русски; "" — нет */
    int error_field;            /* LF_LOGIN / LF_PASSWORD / LF_CODE / LF_TOKEN или LF_NONE — общая ошибка */
    const char *notice;         /* пояснение к этапу (подсказки LG_EMAIL/LG_CAPTCHA/LG_UNDELETE...); "" — нет */
    int64_t wait_until;         /* лимит запросов: до этого now_ms() кнопка «Войти» неактивна; 0 — нет */
    bool route_wait;            /* VLESS включён, но не работает: запросы ждут туннель */
    /* 2FA */
    unsigned mfa_methods;       /* LM_* из ответа Discord */
    unsigned mfa_method;        /* выбранный способ (один бит) */
    bool sms_sent;
    const char *phone;          /* маскированный номер из ответа (+*******0085), "" — нет */
    /* капча: проверка hCaptcha в окне WebView2 (captcha.c), затем повтор запроса */
    const char *captcha_service;/* "hcaptcha" и т. п., "" — нет */
    bool captcha;               /* нужна проверка: кнопка «Пройти проверку» = login_captcha_open() */
    bool captcha_qr;            /* проверка нужна для QR-входа (кнопка — в блоке «Проверьте телефон»), иначе — на LG_CAPTCHA */
    bool captcha_open;          /* окно проверки открыто: «Решите проверку в открывшемся окне» + «Отмена» = login_captcha_cancel() */
    bool captcha_supported;     /* есть WebView2 (Windows); false — кнопку не показывать, только совет войти по QR */
    const char *err_login, *err_pass, *err_code, *err_token;   /* то же error, разложенное по полям ("" — нет) */
    /* QR */
    QrState qr;
    const char *qr_url;         /* QR_SHOW: https://discord.com/ra/<отпечаток> — закодировать в QR (кодер в ui.c); иначе "" */
    const char *qr_text;        /* пояснение/ошибка QR по-русски */
    int64_t qr_expires;         /* когда сессия QR истечёт (now_ms()), 0 — неизвестно */
    uint64_t qr_user_id;        /* QR_SCANNED: кто отсканировал */
    const char *qr_user_name;   /* имя (san), "" — нет */
    const char *qr_discrim;     /* "0" у новых имён */
    const char *qr_avatar;      /* хэш аватара (32 hex / a_+32 hex) или "" — стандартный */
} LoginView;

bool login_init(void);                          /* начать вход: закреплённая память, защищённая куча OpenSSL */
bool login_active(void);                        /* экран входа нужен (init был, finish ещё нет) */
const LoginView *login_view(void);              /* снимок для отрисовки; указатели живут до следующего вызова login_* */
char *login_field(LoginField f, size_t *cap);   /* закреплённый буфер поля (ui.c редактирует на месте) */
void login_field_clear(LoginField f);           /* затереть поле */

void login_submit(void);                        /* «Войти»/«Подтвердить» по текущему этапу */
/* То же для ui.c, который держит поля у себя: строки копируются в закреплённые буферы (ui.c затирает свои
 * сразу после вызова). kind: 0 — код из приложения, 1 — SMS, 2 — резервный код. */
void login_start_password(const char *login, const char *pass);
void login_submit_code(const char *code, int kind);
void login_email_retry(void);                   /* = login_retry() */
void login_mfa_method(unsigned m);              /* LG_MFA: выбрать способ (LM_SMS сам отправит SMS) */
void login_sms_send(void);                      /* LG_MFA: отправить SMS (ещё раз) */
void login_retry(void);                         /* LG_EMAIL / LG_CAPTCHA / лимит: повторить вход с тем же паролем */
void login_undelete(void);                      /* LG_UNDELETE: войти с undelete:true */
void login_use_token(void);                     /* перейти на LG_TOKEN (запасной путь) */
void login_back(void);                          /* назад к форме: тикеты и коды затираются, пароль — тоже */

void login_captcha_open(void);                  /* открыть окно проверки hCaptcha (после решения запрос повторится сам) */
void login_captcha_cancel(void);                /* закрыть окно проверки */
#define LOGIN_CAPTCHA_UI 1                      /* для ui.c: поля и функции капчи есть */

void login_qr_start(void);                      /* новый QR (и при QR_EXPIRED/QR_ERROR) */
void login_qr_stop(void);

void login_cancel(void);                        /* всё прервать и затереть, вернуться к LG_FORM */
void login_finish(void);                        /* после LG_DONE или выхода: затереть всё, освободить страницы */
void login_panic(void);                         /* паника/сбой: только затереть закреплённые страницы (без curl и OpenSSL) */

void login_service(void);
int login_waitables(Waitable *out, int max);
int64_t login_next_wake(void);

/* Самопроверка без сети: разбор ответов и ветки на записанных ответах, затирание, ссылка QR,
 * RSA-OAEP-SHA256 туда-обратно, разбор данных пользователя QR. */
int login_selftest(void (*check)(bool ok, const char *what));
/* С сетью, без входа: рукопожатие со шлюзом QR (hello -> init -> nonce_proof -> pending_remote_init,
 * совпадение отпечатка). Ни отпечаток, ни ссылка не выводятся. */
int login_qr_probe(void (*check)(bool ok, const char *what));
/* С сетью и окном: WebView2 с тестовым ключом hCaptcha (решается сам), ответ приходит через мост. */
int login_captcha_probe(void (*check)(bool ok, const char *what));

#endif
