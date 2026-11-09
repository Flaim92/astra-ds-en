/* mincord: запуск, флаги, токен, VLESS, главный цикл ожидания событий. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "plat.h"
#include "img.h"
#include "login.h"
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stddef.h>
/* Один экземпляр на пользователя Linux: абстрактный сокет (без файла на диске), держится до выхода. */
bool single_instance(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return true;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    int n = snprintf(a.sun_path + 1, sizeof a.sun_path - 1, "astra-ds-single-%u", (unsigned)getuid());
    socklen_t al = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
    if (bind(fd, (struct sockaddr *)&a, al) == 0) { listen(fd, 1); return true; }   /* держим до выхода */
    close(fd);
    /* SECURITY Н3: имя занято — проверить, что держит его наш пользователь (SO_PEERCRED); иначе это чужое
     * занятие имени (абстрактные сокеты общие для всех пользователей) — не считаем, что mincord запущен. */
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c < 0) return true;
    bool ours = false;
    if (connect(c, (struct sockaddr *)&a, al) == 0) {
        struct ucred cr;
        socklen_t cl = sizeof cr;
        ours = getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.uid == getuid();
    }
    close(c);
    return !ours;
}
#endif

/* Временно здесь, до переноса в mc.h координатором (раздел vless.c): */
i64 vless_next_wake(void);
int vless_waitables(Waitable *out, int max);
long vless_xray_rss_kb(void);
extern bool g_headless;                         /* ui.c: проверки без терминала */
/* Временно здесь, до переноса в plat.h координатором (раздел «терминал»): */
bool term_open_window(bool hidden);             /* окно-терминал вместо консоли; false — нельзя */
void term_console_puts(const char *s);          /* всегда в консоль (сообщения --shot) */
int term_mouse_seen(void);                      /* --shot: K_MOUSE, пришедших из событий окна */
#ifdef _WIN32
char **plat_utf8_argv(int *argc);               /* plat_win.c: argv в UTF-8 */
#endif
void term_inject(const Key *k);                 /* --shot: ввод по сценарию (мышь — событиями окна) */
void term_resize(int w, int h);                 /* --shot: размер сетки в клетках */
bool term_snapshot(const char *path);           /* --shot: сетка -> BMP */
bool term_read_line(const char *prompt, char *out, size_t cap);   /* видимая строка (консоль или окно) */

Opts O;

static const char USAGE[] =
    "Astra DS - минимальный клиент Discord\n"
    "\n"
    "  (без флагов)      в отдельном окне (сетка символов, мышь); вход - тоже в окне\n"
    "  --tui             полноэкранный режим в этом терминале (мышь - если терминал умеет)\n"
    "  --cli             консольный режим: построчный, как IRC (команды с /, /help - список);\n"
    "                    работает и в конвейере: строки читаются из stdin до конца ввода\n"
    "  --demo            демо-режим: фальшивые данные, без сети и без токена\n"
    "  --no-save         не сохранять токен и ссылку VLESS (вводить при каждом запуске)\n"
    "  --logout          удалить сохранённые токен и ссылку VLESS и выйти\n"
    "  --no-images       без аватарок и картинок (ничего не качать с CDN Discord)\n"
    "  --vless           спросить (сменить) ссылку vless:// своего сервера\n"
    "  --no-vless        в этом запуске без VLESS (напрямую)\n"
    "  --panic=КЛАВИША   паник-кей: тройное нажатие (F1..F12, Ctrl+X) стирает токен и ссылку\n"
    "                    из памяти и хранилища и закрывает программу\n"
    "  --voice=low|high  смена голоса: ниже или выше\n"
    "  --invisible       статус 'невидимка'\n"
    "  --typing          показывать собеседникам 'печатает...' (по умолчанию нет)\n"
    "  --no-ack          не отмечать сообщения прочитанными\n"
    "  --ptt             рация: передача только по F8 вместо активации голосом\n"
    "  --tray            окно: свернуть — в значок в трее (Windows)\n"
    "  --hq              качество голоса выше (Opus 64 кбит/с, сложность 8) ценой процессора\n"
    "  --mic=ИМЯ         микрофон (часть имени устройства)\n"
    "  --out=ИМЯ         устройство вывода (часть имени)\n"
    "  --devices         показать звуковые устройства и выйти\n"
    "  --shot=ФАЙЛ.bmp   с --demo: снимок окна в BMP без показа окна; --shot-keys=\"шаги\" - сценарий\n"
    "                    (down,enter,tab,t:текст,click:X:Y,rclick:X:Y,hover:X:Y,wheelup:X:Y,size:120x40,\n"
    "                    wait:500,snap,ctrl+q,alt+v,f1...; X/Y - клетка с 0)\n"
    "  --vlesstest=ФАЙЛ проверить туннель: ссылка из файла ('-' - без VLESS), поддельный токен,\n"
    "                    без входа; с --hold=N ещё N с держать связь (проверка обрыва сети)\n"
    "\n"
    "VLESS: весь трафик (сообщения, голос) идёт через ваш сервер. Нужен xray рядом с Astra DS\n"
    "(Xray-core v26.10.31, проверяется по SHA256 при каждом запуске).\n"
    "Сторонние клиенты нарушают правила Discord: используйте запасной аккаунт.\n";

/* Сбой: погасить Xray (в нём ключ VLESS) и затереть секреты до того, как процесс умрёт. */
static void on_crash(void) { vless_stop(); login_panic(); secret_destroy(); }

static int arg_is(const char *a, const char *name, const char **val) {
    size_t n = strlen(name);
    if (strncmp(a, name, n)) return 0;
    if (a[n] == 0) { if (val) *val = NULL; return 1; }
    if (a[n] == '=' && val) { *val = a + n + 1; return 1; }
    return 0;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    term_puts(b);
}

/* Выбор в меню: цифра или пустая строка (-1 — конец ввода). */
static int read_choice(void) {
    char b[32];
    if (!term_read_line("", b, sizeof b)) return -1;
    return b[0] >= '1' && b[0] <= '9' ? b[0] - '0' : 0;
}

static bool shot_live;                          /* --shot без --demo: живой аккаунт, только просмотр */
/* Стартовый экран Astra DS (ui.c): шаг VLESS карточкой в окне; «Больше не спрашивать» — запись хранилища */
void ui_vless_begin(const char *err, bool only);
bool ui_startup_direct(void);
void ui_startup_direct_set(bool on);
static int demo_start;                          /* --demo-start: 1 — стартовый экран, 2 — экран ошибки VLESS */
/* Нужен ли вопрос о VLESS при запуске: 0 — нет, 1 — спросить, 2 — сохранённая ссылка не работает (err).
 * Сохранённая ссылка важнее «больше не спрашивать»: флаг действует, только если ссылки нет. */
static int vless_plan(char *err, size_t n) {
    if (O.no_vless) return 0;
    if (O.demo) { if (demo_start == 2) snprintf(err, n, "%s", "туннель не отвечает: проверьте сервер VLESS (демо)"); return demo_start; }
    if (O.vless_ask || O.no_save) return 1;
    if (vless_load_saved()) return 0;
    if (vless_state() != VL_OFF) { snprintf(err, n, "%s", vless_error()); return 2; }
    if (ui_startup_direct()) return 0;          /* явный выбор «напрямую», ссылки нет */
    return 1;
}

/* VLESS при входе (до интерфейса, в обычной консоли). false — выйти: туннель не работает, а
 * пользователь не выбрал явно «без VLESS». Молча напрямую не идём. */
static bool vless_login(void) {
    if (O.demo || O.no_vless) return true;
    char err[256] = "";
    int lat = -1;
    bool ok = false, ask = O.vless_ask || O.no_save;
    if (!ask) {
        ok = vless_load_saved();
        if (ok) lat = vless_latency_ms();
        else if (vless_state() == VL_OFF) ask = !ui_startup_direct();   /* ссылки нет: спросить, если не выбрано «напрямую» */
        else snprintf(err, sizeof err, "%s", vless_error());
    }
    if (ask && !O.vless_ask && (shot_live || !term_interactive())) {
        if (!O.no_save) return true;            /* сохранённой ссылки нет: VLESS не настроен — напрямую */
        /* SECURITY М8: --no-save без консоли — спросить негде; есть сохранённая ссылка — через неё,
         * не работает — стоп (молча напрямую не идём) */
        if (vless_load_saved()) return true;
        if (vless_state() == VL_OFF) return true;
        say("VLESS настроен, но не работает: %s. Напрямую не подключаюсь.\n", vless_error());
        return false;
    }
    for (;;) {
        if (ask) {
            ask = false;
            size_t cap;
            char *buf = secret_scratch(&cap);
            term_puts("Весь трафик можно пустить через свой сервер VLESS (Xray: Reality или TLS).\n");
            bool rd = term_read_secret("Ссылка vless:// (ввод скрыт; Enter - без VLESS): ", buf, cap);
            if (rd && !buf[0]) {
                plat_wipe(buf, cap);
                term_puts("Без VLESS.\n");
                if (!O.no_save && term_interactive()) {
                    term_puts("Больше не спрашивать при запуске (всегда напрямую; вернуть: --vless)? y/N: ");
                    char yn[8];
                    if (term_read_line("", yn, sizeof yn) && (yn[0] == 'y' || yn[0] == 'Y' || !strncmp(yn, "\xd0\xbd", 2))) ui_startup_direct_set(true);
                }
                return true;
            }
            if (!rd) {
                ok = false;
                snprintf(err, sizeof err, "ссылку не удалось прочитать (нет консоли или длиннее %zu символов)", cap - 1);
            } else {
                term_puts("Проверка: запуск Xray и запрос к Discord через туннель...\n");
                ok = vless_set_link(buf, err, sizeof err, &lat);
            }
            plat_wipe(buf, cap);
        }
        if (ok) {
            ui_startup_direct_set(false);           /* ссылка есть — «напрямую» больше не действует */
            say("VLESS работает: задержка %d мс\n", lat);
            if (err[0]) say("%s\n", err);          /* работает, но не сохранилась */
            return true;
        }
        say("VLESS не работает: %s\n", err);
        if (shot_live) return false;            /* скрытый снимок: спросить негде — выход */
        bool have = vless_state() != VL_OFF;       /* ссылка разобрана: можно повторить */
        say("%s2 - другая ссылка, 3 - без VLESS (напрямую), Enter - выход: ", have ? "1 - повторить, " : "");
        int c = read_choice();
        if (c == 1 && have) {
            term_puts("Повтор...\n");
            ok = vless_restart();
            lat = vless_latency_ms();
            snprintf(err, sizeof err, "%s", ok ? "" : vless_error());
        } else if (c == 2) {
            ask = true;
        } else if (c == 3) {
            vless_disable();
            term_puts("Без VLESS: трафик идёт напрямую.\n");
            return true;
        } else if (c <= 0) {
            return false;
        }
    }
}

/* Удержание (--hold=N): N секунд REST каждые 5 с и gateway, заново после каждого 4004; печатаются
 * только смены состояний. Для проверки обрыва сети и восстановления (с туннелем и без). */
static long t_rest = -1;
static void on_me(Resp *r, void *ctx) { (void)ctx; t_rest = r->status; }
static void hold_net(int hold) {
    say("  удержание %d с: REST каждые 5 с, gateway заново через 5 с после каждого отказа 4004\n", hold);
    i64 t0 = now_ms(), end = t0 + (i64)hold * 1000, next_rest = 0, regw = t0;
    char lg[128] = "", lv[600] = "";
    long lr = -2;
    gw_stop();
    while (now_ms() < end) {
        i64 now = now_ms();
        if (regw && now >= regw) { regw = 0; g_auth_failed = false; gw_start(); }
        if (t_rest != -1 && t_rest != lr) { lr = t_rest; say("  [%6.1f с] REST: HTTP %ld\n", (double)(now - t0) / 1000, t_rest); }
        if (!http_busy() && now >= next_rest) { t_rest = -1; http_async("GET", "/users/@me", NULL, on_me, NULL); next_rest = now + 5000; }
        const char *st = gw_status_text();
        if (strcmp(st, lg)) { snprintf(lg, sizeof lg, "%s", st); say("  [%6.1f с] gateway: %s\n", (double)(now - t0) / 1000, st); }
        if (g_auth_failed && strstr(st, "токен") && !regw) regw = now + 5000;
        char vs[600];
        static const char *const names[] = { "выключен", "проверка", "работает", "НЕ работает" };
        snprintf(vs, sizeof vs, "%s %s", names[vless_state()], vless_error());
        if (strcmp(vs, lv)) { snprintf(lv, sizeof lv, "%s", vs); say("  [%6.1f с] VLESS: %s\n", (double)(now - t0) / 1000, vs); }
        Waitable w[4];
        int k = gw_waitables(w, 4);
        k += vless_waitables(w + k, 4 - k);
        i64 t = gw_next_wake(), v = vless_next_wake(), h = http_next_wake();
        if (v < t) t = v;
        if (h < t) t = h;
        if (regw && regw < t) t = regw;
        if (next_rest < t) t = next_rest;
        if (t > now + 500) t = now + 500;
        plat_wait(w, k, t > now ? (int)(t - now) : 0);
        vless_service();
        gw_service();
        http_service();
    }
}

/* --vlesstest=ФАЙЛ: ссылка из файла (не из аргументов — их видно в списке процессов), туннель,
 * REST и gateway через него с поддельным токеном. Ничего не сохраняет. ФАЙЛ "-" — без VLESS. */
static int vless_selftest(const char *path, int hold) {
    term_puts("Astra DS --vlesstest: туннель VLESS, живой Discord, поддельный токен\n");
    g_headless = true;
    O.no_save = true;                           /* ссылку из файла в хранилище не пишем */
    if (!secret_init()) return 1;
    plat_net_init();
    if (!net_init()) { term_puts("сеть не запустилась\n"); return 1; }
    ui_init();
    size_t cap;
    char *buf = secret_scratch(&cap);
    int lat = -1, fails = 0;
    i64 t0 = now_ms();
    if (!strcmp(path, "-")) { term_puts("  без VLESS (напрямую)\n"); goto direct; }
    FILE *f = fopen_u8(path, "rb");
    if (!f) { term_puts("файл со ссылкой не открылся\n"); return 1; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
    buf[n] = 0;
    char err[256];
    bool ok = vless_set_link(buf, err, sizeof err, &lat);
    plat_wipe(buf, cap);
    if (!ok) { say("  [FAIL] туннель: %s\n", err); net_cleanup(); secret_destroy(); return 1; }
    say("  [ok] туннель поднят за %lld мс, задержка до Discord через туннель: %d мс\n", (long long)(now_ms() - t0), lat);
    const char *px = vless_proxy_url();
    const char *portp = px ? strrchr(px, ':') : NULL;
    say("  маршрут: socks5h, 127.0.0.1%s, логин и пароль случайные на сессию\n", portp ? portp : "?");
direct:;
    static const char FAKE[] = "MTAwMDAwMDAwMDAwMDAwMDAw.AAAAAA.mincord-vlesstest-not-a-real-token";
    secret_set(FAKE, sizeof FAKE - 1);
    /* REST через туннель: поддельный токен — ожидается 401 */
    t0 = now_ms();
    http_async("GET", "/users/@me", NULL, on_me, NULL);
    for (i64 rd = now_ms() + 20000; t_rest < 0 && now_ms() < rd; ) {
        i64 t = http_next_wake(), now = now_ms();
        plat_wait(NULL, 0, t == INT64_MAX ? 200 : (int)(t > now ? (t - now > 200 ? 200 : t - now) : 0));
        http_service();
    }
    say("  [%s] REST /users/@me через туннель: HTTP %ld за %lld мс (ожидается 401)\n",
        t_rest == 401 ? "ok" : "FAIL", t_rest, (long long)(now_ms() - t0));
    fails += t_rest != 401;
    g_auth_failed = false;
    /* gateway через туннель: Hello, затем отказ 4004 на поддельный токен */
    gw_start();
    char last[128] = "";
    t0 = now_ms();
    i64 deadline = t0 + 45000, t_login = -1;
    while (now_ms() < deadline) {
        const char *st = gw_status_text();
        if (strcmp(st, last)) {
            snprintf(last, sizeof last, "%s", st);
            if (t_login < 0 && strstr(st, "вход")) t_login = now_ms() - t0;
            say("  [%5lld мс] gateway: %s\n", (long long)(now_ms() - t0), st);
        }
        if (g_auth_failed && strstr(st, "токен")) break;
        Waitable w[4];
        int k = gw_waitables(w, 4);
        k += vless_waitables(w + k, 4 - k);
        i64 t = gw_next_wake(), v = vless_next_wake(), now = now_ms();
        if (v < t) t = v;
        if (t > deadline) t = deadline;
        plat_wait(w, k, t > now ? (int)(t - now) : 0);
        vless_service();
        gw_service();
    }
    bool gw_ok = t_login >= 0 && g_auth_failed;
    say("  [%s] gateway через туннель: Hello за %lld мс, отказ 4004 на поддельный токен\n", gw_ok ? "ok" : "FAIL", (long long)t_login);
    fails += !gw_ok;
    if (hold > 0) hold_net(hold);
    say("  VLESS: %s; память: Xray %ld КБ, Astra DS %llu КБ\n",
        vless_state() == VL_UP ? "работает" : "НЕ работает", vless_xray_rss_kb(), (unsigned long long)(plat_private_bytes() / 1024));
    gw_stop();
    vless_stop();
    net_cleanup();
    secret_destroy();
    say("%s\n", fails ? "ЕСТЬ ОШИБКИ" : "всё ok");
    return fails ? 1 : 0;
}

/* Один оборот главного цикла: отрисовать, ждать событий (без опроса: объекты, сокеты, очередь
 * окна — до ближайшего таймера, но не дольше deadline), обработать. */
static void relogin_step(void);
static void loop_once(i64 deadline) {
    ui_render_if_dirty();
    Waitable w[12];                             /* последнее место — под g_wake */
    int n = O.cli ? cli_waitables(w, 4) : term_waitables(w, 4);
    n += gw_waitables(w + n, 11 - n);
    n += http_waitables(w + n, 11 - n);         /* сокеты отправки файла (0, если не идёт) */
    n += vless_waitables(w + n, 11 - n);
    n += img_waitables(w + n, 11 - n);
    n += login_waitables(w + n, 11 - n);         /* окно входа: сокет шлюза QR (0, если входа нет) */
    if (g_wake) w[n++] = wake_waitable(g_wake);
    i64 now = now_ms(), t = gw_next_wake();
    i64 h = http_next_wake(), u = O.cli ? cli_next_wake() : ui_next_wake(), v = vless_next_wake();
    if (h < t) t = h;
    if (u < t) t = u;
    if (v < t) t = v;
    i64 im = img_next_wake();
    if (im < t) t = im;
    i64 lg = login_next_wake();
    if (lg < t) t = lg;
    i64 st = store_next_wake();                 /* отложенные отметки о прочтении и смена статуса */
    if (st < t) t = st;
    if (deadline < t) t = deadline;
    int timeout = t == INT64_MAX ? -1 : (t <= now ? 0 : (int)(t - now > 60000 ? 60000 : t - now));
    plat_wait(w, n, timeout);
    wake_clear(g_wake);
    if (O.cli) cli_handle_input(); else ui_handle_input();
    vless_service();
    gw_service();
    http_service();
    store_tick();
    img_service();
    login_service();
    if (O.cli) cli_tick(); else ui_tick();
    relogin_step();                             /* токен отозван — окно входа, без выхода (окно/терминал) */
    if (g_auth_failed && O.cli) {               /* --cli: прежнее поведение строкой */
        ui_notice("Discord не принял токен. Удалить сохранённый: Astra DS --logout, затем войти заново");
        g_quit = true;
    }
    /* окно/терминал: сессию снова откроет relogin() в main — без выхода */
}

/* Discord не принял сохранённый токен (4004 / 401): удалить его из хранилища и памяти, сбросить модель,
 * открыть то же окно входа (без выхода); после входа — снова gateway. Неблокирующе: состояние в цикле. */
static bool relog_on;
static void relogin_start(void) {
    gw_stop();
    voice_leave();
    if (!O.no_save && !O.demo) cred_delete();   /* в демо (проверка) хранилище не трогаем */
    secret_set("", 0);                          /* затереть токен в закреплённой памяти */
    store_reset();
    g_auth_failed = false;
    if (!login_init()) { g_quit = true; return; }
    ui_login_begin(false);
    ui_notice("Сессия завершена Discord - войдите заново");
    relog_on = true;
}
static void relogin_step(void) {
    if (g_auth_failed && !O.cli && !relog_on && (!O.demo || getenv("MC_TEST_RELOGIN"))) relogin_start();
    if (!relog_on || ui_login_active()) return;
    relog_on = false;
    login_finish();
    if (O.demo) { demo_load(); ui_on_ready(); return; }   /* проверка: вход в демо «удался» */
    if (!secret_token()) { g_quit = true; return; }        /* окно входа закрыто — выход */
    gw_start();
    ui_dirty();
}

/* ---------- --shot: снимок окна в --demo без показа окна (для дизайнера) ----------
 * --shot-keys: шаги через запятую:
 *   up down left right enter tab btab esc bs del home end pgup pgdn ins f1..f12
 *   ctrl+X alt+X          — сочетание с буквой X
 *   t:ТЕКСТ               — набрать текст (до следующей запятой; «\,» — сама запятая, «\\» — обратная черта)
 *   click:X:Y rclick:X:Y mclick:X:Y hover:X:Y wheelup:X:Y wheeldown:X:Y — мышь, X/Y — клетка с 0
 *   shift+click:X:Y ctrl+click:X:Y — мышь с модификаторами; drag:X1:Y1:X2:Y2 — протяжка левой
 *   (нажатие, button 6 по пути, отпускание button 7); shift+enter — перенос строки ('\n'), как в окне
 *   size:ШxВ              — размер сетки в клетках (по умолчанию 110x34)
 *   wait:МС               — прокрутить цикл событий МС миллисекунд (таймеры интерфейса)
 *   snap                  — промежуточный снимок ФАЙЛ-1.bmp, ФАЙЛ-2.bmp, ...
 *   fine:X:Y:Ш:В          — тестовый слой мелкой сетки (K=4, градиент и ASCII) в клетках X,Y,ШxВ
 * Итоговый снимок — в ФАЙЛ после всех шагов. Файл пишется только по этому явному флагу.
 * Без --demo (живой аккаунт, токен только из хранилища) — только просмотр: разрешены wait, size, snap,
 * fine, hover, wheelup/wheeldown, up/down/left/right/home/end/pgup/pgdn/tab/btab/esc/enter/f1;
 * набор текста, сочетания, щелчки и прочие клавиши (голос F2..F12, удаление) отклоняются. */
static int exit_rc;
static bool shot_text;                          /* --shot-text: рядом со снимком .txt — текст сетки (стенд тестировщика) */
static void shot_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Текст сетки (без SGR) в файл ПУТЬ.txt вместо .bmp — только по явному флагу --shot-text. */
static void shot_write_text(const char *bmp) {
    if (!shot_text) return;
    char p[520];
    const char *dot = strrchr(bmp, '.');
    int base = dot && !strpbrk(dot, "/\\") ? (int)(dot - bmp) : (int)strlen(bmp);
    snprintf(p, sizeof p, "%.*s.txt", base, bmp);
    int w, h;
    term_size(&w, &h);
    Buf r = {0}, o = {0};
    ui_snapshot(w, h, &r);
    for (size_t i = 0; i < r.len; i++) {
        if (r.p[i] == 0x1b) {                    /* CSI ... буква; курсор в начало строки — перевод строки */
            size_t j = i + 1;
            if (j < r.len && r.p[j] == '[') { j++; while (j < r.len && !((r.p[j] >= 'A' && r.p[j] <= 'Z') || (r.p[j] >= 'a' && r.p[j] <= 'z'))) j++; }
            if (j < r.len && r.p[j] == 'H' && o.len && o.p[o.len - 1] != '\n') buf_put(&o, "\n", 1);
            i = j;
            continue;
        }
        buf_put(&o, r.p + i, 1);
    }
    buf_put(&o, "\n", 1);
    FILE *f = fopen_u8(p, "wb");
    if (f) { fwrite(o.p, 1, o.len, f); fclose(f); shot_say("текст: %s\n", p); }
    else shot_say("--shot-text: не записан %s\n", p);
    buf_free(&r);
    buf_free(&o);
}
/* Тот же главный цикл (plat_wait с очередью окна), но не дольше ms; ms = 0 — один оборот. */
static void shot_spin(int ms) {
    i64 end = now_ms() + ms;
    do loop_once(end); while (!g_quit && now_ms() < end);
    ui_render_if_dirty();
}
static void shot_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void shot_say(const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    term_console_puts(b);
}
static int shot_name(const char *t) {
    static const struct { const char *n; int k; } N[] = {
        {"up", K_UP}, {"down", K_DOWN}, {"left", K_LEFT}, {"right", K_RIGHT}, {"enter", K_ENTER}, {"tab", K_TAB},
        {"btab", K_BTAB}, {"esc", K_ESC}, {"bs", K_BS}, {"del", K_DEL}, {"home", K_HOME}, {"end", K_END},
        {"pgup", K_PGUP}, {"pgdn", K_PGDN}, {"ins", K_INS} };
    for (size_t i = 0; i < sizeof N / sizeof *N; i++) if (!strcmp(t, N[i].n)) return N[i].k;
    if (t[0] == 'f' && t[1] >= '1' && t[1] <= '9') { int f = atoi(t + 1); if (f >= 1 && f <= 12) return K_F1 + f - 1; }
    return K_NONE;
}
/* Тестовый слой мелкой сетки: та же последовательность, что шлёт ui.c (см. gui.h). */
static void shot_fine(int x, int y, int w, int h) {
    const int K = 4;
    static const char ramp[] = " .:-=+*#%@";
    static const char hx[] = "0123456789abcdef";
    Buf b = {0};
    buf_printf(&b, "\x1b_astra-img;%d;%d;%d;%d;%d;", x, y, w, h, K);
    int fw = K * w, fh = K * h;
    for (int fy = 0; fy < fh; fy++)
        for (int fx = 0; fx < fw; fx++) {
            int r = fx * 255 / (fw > 1 ? fw - 1 : 1), g = fy * 255 / (fh > 1 ? fh - 1 : 1), bl = 255 - r;
            u8 fg[3] = { (u8)r, (u8)g, (u8)bl }, bg[3] = { (u8)(r / 4), (u8)(g / 4), (u8)(bl / 4) };
            char e[13];
            e[0] = ramp[((fx / 2 + fy) % 10)];
            for (int i = 0; i < 3; i++) {
                e[1 + 2 * i] = hx[fg[i] >> 4]; e[2 + 2 * i] = hx[fg[i] & 15];
                e[7 + 2 * i] = hx[bg[i] >> 4]; e[8 + 2 * i] = hx[bg[i] & 15];
            }
            buf_put(&b, e, 13);
        }
    buf_puts(&b, "\x1b\\");
    term_write(b.p, b.len);
    buf_free(&b);
}
/* Живой аккаунт: только шаги просмотра. */
static bool shot_safe(const char *t) {
    static const char *const ok[] = { "up", "down", "left", "right", "home", "end", "pgup", "pgdn", "tab", "btab",
                                      "esc", "enter", "f1", "snap" };
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) if (!strcmp(t, ok[i])) return true;
    static const char *const pre[] = { "wait:", "size:", "fine:", "hover:", "wheelup:", "wheeldown:" };
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(t, pre[i], strlen(pre[i]))) return true;
    return false;
}
static bool shot_step(const char *t, const char *path, int *snaps) {
    Key k = {0};
    int a = 0, b = 0, rw = 0, rh = 0, kc;
    if (shot_live && !shot_safe(t)) { shot_say("--shot-keys: шаг '%s' без --demo запрещён (только просмотр)\n", t); return false; }
    if (!strncmp(t, "fine:", 5) && sscanf(t + 5, "%d:%d:%d:%d", &a, &b, &rw, &rh) == 4) {
        shot_fine(a, b, rw, rh);
        shot_spin(0);
        return true;
    }
    if (!strncmp(t, "wait:", 5)) { shot_spin(atoi(t + 5)); return true; }
    if (!strncmp(t, "size:", 5) && sscanf(t + 5, "%dx%d", &a, &b) == 2 && a >= 10 && b >= 5) {
        term_resize(a, b);
        shot_spin(0);
        return true;
    }
    if (!strcmp(t, "snap")) {
        char p[512];
        const char *dot = strrchr(path, '.');
        int base = dot && !strpbrk(dot, "/\\") ? (int)(dot - path) : (int)strlen(path);
        snprintf(p, sizeof p, "%.*s-%d%s", base, path, ++*snaps, path + base);
        shot_spin(0);
        if (!term_snapshot(p)) { shot_say("--shot: не записан %s\n", p); return false; }
        shot_say("снимок: %s; частная память %llu КБ\n", p, (unsigned long long)(plat_private_bytes() / 1024));
        shot_write_text(p);
        return true;
    }
    if (!strncmp(t, "t:", 2)) {
        for (const char *s = t + 2; *s; ) {
            u32 c;
            s += utf8_dec(s, &c);
            k = (Key){ .code = K_CHAR, .ch = c };
            term_inject(&k);
        }
        shot_spin(0);
        return true;
    }
    if (!strncmp(t, "ctrl+", 5) || !strncmp(t, "alt+", 4) || !strncmp(t, "shift+", 6)) {
        /* модификаторы в любом порядке (ctrl+, alt+, shift+), затем клавиша по имени (up, f5, ...) или буква */
        bool ctrl = false, alt = false, shift = false;
        const char *s = t;
        for (;;) {
            if (!strncmp(s, "ctrl+", 5)) { ctrl = true; s += 5; }
            else if (!strncmp(s, "alt+", 4)) { alt = true; s += 4; }
            else if (!strncmp(s, "shift+", 6)) { shift = true; s += 6; }
            else break;
        }
        if (!*s) return false;
        static const struct { const char *n; int b; } MM[] = { {"click:", 1}, {"mclick:", 2}, {"rclick:", 3} };
        for (size_t i = 0; i < sizeof MM / sizeof *MM; i++) {   /* мышь с модификаторами */
            size_t n = strlen(MM[i].n);
            if (!strncmp(s, MM[i].n, n) && sscanf(s + n, "%d:%d", &a, &b) == 2) {
                k = (Key){ .code = K_MOUSE, .x = a, .y = b, .button = MM[i].b, .ctrl = ctrl, .alt = alt, .shift = shift };
                term_inject(&k);
                shot_spin(0);
                return true;
            }
        }
        int named = shot_name(s);
        if (named == K_ENTER && shift && !ctrl && !alt) {   /* как окно: Shift+Enter — перенос строки */
            k = (Key){ .code = K_CHAR, .ch = '\n' };
            term_inject(&k);
            shot_spin(0);
            return true;
        }
        if (named != K_NONE) k = (Key){ .code = named, .ctrl = ctrl, .alt = alt, .shift = shift };
        else {
            u32 c;
            utf8_dec(s, &c);
            k = (Key){ .code = K_CHAR, .ch = c, .ctrl = ctrl, .alt = alt, .shift = shift };
        }
        term_inject(&k);
        shot_spin(0);
        return true;
    }
    if (!strncmp(t, "drag:", 5) && sscanf(t + 5, "%d:%d:%d:%d", &a, &b, &rw, &rh) == 4) {
        /* протяжка: нажатие (9 — только нажатие), движение с зажатой (6) по шагам, отпускание (7) */
        k = (Key){ .code = K_MOUSE, .x = a, .y = b, .button = 99 };   /* 99 — только нажатие (окно -> button 1) */
        term_inject(&k);
        int steps = abs(rw - a) > abs(rh - b) ? abs(rw - a) : abs(rh - b);
        if (steps > 40) steps = 40;
        for (int i = 1; i <= steps; i++) {
            k = (Key){ .code = K_MOUSE, .x = a + (rw - a) * i / steps, .y = b + (rh - b) * i / steps, .button = 6 };
            term_inject(&k);
        }
        k = (Key){ .code = K_MOUSE, .x = rw, .y = rh, .button = 7 };
        term_inject(&k);
        shot_spin(0);
        return true;
    }
    static const struct { const char *n; int b; } M[] = {
        {"click:", 1}, {"mclick:", 2}, {"rclick:", 3}, {"hover:", 0}, {"wheelup:", 4}, {"wheeldown:", 5}, {"x1click:", 8}, {"x2click:", 9} };
    for (size_t i = 0; i < sizeof M / sizeof *M; i++) {
        size_t n = strlen(M[i].n);
        if (!strncmp(t, M[i].n, n) && sscanf(t + n, "%d:%d", &a, &b) == 2) {
            k = (Key){ .code = K_MOUSE, .x = a, .y = b, .button = M[i].b };
            term_inject(&k);
            shot_spin(0);
            return true;
        }
    }
    if ((kc = shot_name(t)) != K_NONE) {
        k = (Key){ .code = kc };
        term_inject(&k);
        shot_spin(0);
        return true;
    }
    shot_say("--shot-keys: непонятный шаг '%s'\n", t);
    return false;
}
static int shot_run(const char *path, const char *keys) {
    shot_spin(100);                             /* первый кадр */
    int snaps = 0;
    char t[1024];
    /* --shot-keys=@ФАЙЛ: сценарий из файла (длинные сценарии не влезают в командную строку);
     * шаги — через запятую или с новой строки. */
    char *file_keys = NULL;
    if (keys && keys[0] == '@') {
        FILE *f = fopen_u8(keys + 1, "rb");
        if (!f) { shot_say("--shot-keys: нет файла %s\n", keys + 1); return 1; }
        Buf b = {0};
        char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof chunk, f)) > 0 && b.len < (1u << 20)) buf_put(&b, chunk, got);
        fclose(f);
        for (size_t i = 0; i < b.len; i++) if (b.p[i] == '\n' || b.p[i] == '\r') b.p[i] = ',';
        file_keys = b.p ? b.p : xstrdup("");
        keys = file_keys;
    }
    for (const char *s = keys ? keys : ""; *s; ) {
        /* шаги через запятую; «\,» — запятая внутри шага (t:один\,два), «\\» — обратная черта */
        size_t n = 0;
        while (*s && *s != ',') {
            if (*s == '\\' && (s[1] == ',' || s[1] == '\\')) s++;
            if (n + 1 < sizeof t) t[n++] = *s;
            s++;
        }
        t[n] = 0;
        if (n && !shot_step(t, path, &snaps)) { free(file_keys); return 1; }
        if (g_quit) break;
        if (*s == ',') s++;
    }
    free(file_keys);
    shot_spin(50);
    int w, h;
    term_size(&w, &h);
    shot_write_text(path);
    if (!term_snapshot(path)) { shot_say("--shot: не записан %s\n", path); return 1; }
    shot_say("снимок: %s (%dx%d клеток); событий мыши дошло до интерфейса: %d; частная память %llu КБ\n", path, w, h,
             term_mouse_seen(), (unsigned long long)(plat_private_bytes() / 1024));
    return 0;
}

int main(int argc, char **argv) {
    plat_harden(on_crash);
    /* Нулевое логирование: всё, что любая библиотека попытается написать в stderr, пропадает. */
#ifdef _WIN32
    if (!freopen("NUL", "w", stderr)) return 1;
    {   /* аргументы в UTF-8 (CRT отдаёт их в кодовой странице ANSI) */
        int un = 0;
        char **uv = plat_utf8_argv(&un);
        if (uv) { argc = un; argv = uv; }
    }
#else
    if (!freopen("/dev/null", "w", stderr)) return 1;
#endif
    bool list_devices = false, tui = false;
    const char *vlesstest = NULL, *shot = NULL, *shot_keys = NULL;
    int hold = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = NULL;
        if (arg_is(a, "--demo", NULL)) O.demo = true;
        else if (arg_is(a, "--cli", NULL)) O.cli = true;
        else if (arg_is(a, "--tui", NULL)) tui = true;
        else if (arg_is(a, "--shot", &v) && v && *v) shot = v;
        else if (arg_is(a, "--shot-keys", &v) && v) shot_keys = v;
        else if (arg_is(a, "--shot-text", NULL)) shot_text = true;
        else if (arg_is(a, "--no-save", NULL)) O.no_save = true;
        else if (arg_is(a, "--no-images", NULL)) O.no_images = true;
        else if (arg_is(a, "--logout", NULL)) O.logout = true;
        else if (arg_is(a, "--demo-start", &v)) demo_start = v && !strcmp(v, "err") ? 2 : 1;   /* с --demo: стартовый экран */
        else if (arg_is(a, "--vless", NULL)) O.vless_ask = true;
        else if (arg_is(a, "--no-vless", NULL)) O.no_vless = true;
        else if (arg_is(a, "--panic", &v) && v && *v) O.panic = v;
        else if (arg_is(a, "--voice", &v) && v && *v) O.voice = v;
        else if (arg_is(a, "--invisible", NULL)) O.invisible = true;
        else if (arg_is(a, "--typing", NULL)) O.typing = true;
        else if (arg_is(a, "--no-ack", NULL)) O.no_ack = true;
        else if (arg_is(a, "--ptt", NULL)) O.ptt = true;
        else if (arg_is(a, "--tray", NULL)) O.tray = true;
        else if (arg_is(a, "--hq", NULL)) O.hq = true;
        else if (arg_is(a, "--mic", &v) && v) O.mic = v;
        else if (arg_is(a, "--out", &v) && v) O.out = v;
        else if (arg_is(a, "--devices", NULL)) list_devices = true;
        else if (arg_is(a, "--vlesstest", &v) && v && *v) vlesstest = v;
        else if (arg_is(a, "--hold", &v) && v && *v) hold = atoi(v);
        else if (arg_is(a, "--selftest", NULL)) { g_wake = wake_new(); voice_init(); return selftest(); }
        else if (arg_is(a, "--keyringtest", NULL)) return selftest_keyring();
        else if (arg_is(a, "--gwtest", NULL)) { g_wake = wake_new(); voice_init(); return selftest_gateway(); }
        else if (arg_is(a, "--livetest", NULL)) { g_wake = wake_new(); voice_init(); return selftest_live(); }
        else { term_puts(USAGE); return strcmp(a, "--help") && strcmp(a, "-h") ? 2 : 0; }
    }
    if (O.vless_ask && O.no_vless) { term_puts("--vless и --no-vless вместе не имеют смысла\n"); return 2; }
    if ((shot || shot_keys) && (!shot || O.cli || tui || O.vless_ask)) {
        term_puts("--shot=ФАЙЛ.bmp - без --cli/--tui/--vless; --shot-keys - только с --shot\n");
        return 2;
    }
    if (shot && !O.demo) {
        /* SECURITY М6: снимок живого аккаунта записал бы переписку на диск — только с --demo.
         * Живые проверки разработчика — --livetest (ничего не пишет на диск). */
        term_puts("--shot только с --demo: снимок живого аккаунта записал бы переписку на диск\n");
        return 2;
    }
    if (list_devices) { audio_list_devices(); return 0; }
    if (O.voice) {
        if (!strcmp(O.voice, "low")) voicefx_set(VFX_LOW);
        else if (!strcmp(O.voice, "high")) voicefx_set(VFX_HIGH);
        else { term_puts("--voice: допустимо low или high\n"); return 2; }
    }
    if (O.panic && !panic_init(O.panic)) {
        term_puts("--panic: неизвестная клавиша (примеры: F12, Ctrl+X)\n");
        return 2;
    }
    if (vlesstest) return vless_selftest(vlesstest, hold);
#ifdef _WIN32
    wv2_cleanup();                              /* SECURITY М4: остатки временных профилей WebView2 */
#endif
    /* Живой аккаунт — только один клиент (демо и проверки не мешают друг другу) */
    if (O.no_save) keyring_readonly(true);       /* --no-save: ничего не записывать в хранилище, в т. ч. миграцию */
    if (!O.demo && !O.logout && !single_instance()) { term_puts("Astra DS уже запущен: открыто его окно\n"); return 1; }
    if (!secret_init()) { term_puts("не удалось выделить защищённую память\n"); return 1; }
    if (O.logout) {
        cred_delete();
        vless_forget();
        keyring_delete("startup");               /* «больше не спрашивать» — тоже */
        secret_destroy();
        term_puts("Сохранённые токен, ссылка VLESS и выбор «напрямую» удалены.\n");
        return 0;
    }
    if (O.cli && tui) { term_puts("--cli и --tui вместе не имеют смысла\n"); secret_destroy(); return 2; }
    /* По умолчанию — окно, и вход (токен, VLESS) тоже в нём: все term_* дальше идут в окно. */
    if (!O.cli && !tui && !shot) {
        if (term_open_window(false)) { if (!O.demo && (O.cli || tui)) term_puts("\x1b[1mAstra DS\x1b[0m - вход\n\n"); }
        else term_puts("Окно недоступно (нет $DISPLAY / X-сервера): полноэкранный режим в терминале.\n");
    }
    plat_net_init();
    bool win_login = false;                     /* вход в окне (или --tui): почта/пароль, 2FA, QR — экран ui.c, движок login.c */
    if (!O.demo) {
        bool have = !O.no_save && cred_load();
        if (!have && shot) {
            term_puts("--shot без --demo: нужен сохранённый токен (сначала войдите обычным запуском)\n");
            secret_destroy();
            return 1;
        }
        if (!have && !O.cli) win_login = true;
        else if (!have) {                       /* --cli: токен скрытым вводом в консоли, как раньше */
            term_puts("Astra DS: нужен токен Discord (запасного аккаунта). Он хранится только в\n");
            term_puts(O.no_save ? "памяти и нигде не сохраняется.\n" : keyring_name());
            if (!O.no_save) term_puts(".\n");
            if (!read_hidden("Токен (ввод скрыт): ")) {
                term_puts("Токен не похож на настоящий.\n");
                secret_destroy();
                return 1;
            }
            if (!O.no_save && !cred_save())
                term_puts("Хранилище секретов недоступно: токен не сохранён, при следующем запуске введите снова.\n");
        }
    }
    if (!net_init()) { term_puts("не удалось запустить сеть\n"); secret_destroy(); return 1; }
    /* VLESS — до любых запросов к Discord. Вход в окне: сохранённая ссылка поднимается здесь, иначе
     * (нет ссылки, не поднялась, --vless, --no-save) — первым шагом окна входа. Иначе — как раньше, в консоли. */
    bool vless_step = false, gui = !O.cli && !tui;
    char verr[256] = "";
    int vplan = 0;
    if (win_login || gui) {
        vplan = vless_plan(verr, sizeof verr);  /* окно: вопрос и ошибки VLESS — карточкой (ui.c), не консолью */
        vless_step = vplan > 0;
    } else if (!vless_login()) {
        term_puts("Выход: VLESS не работает, а работа напрямую не выбрана.\n");
        vless_stop();
        net_cleanup();
        secret_destroy();
        return 1;
    }
    g_wake = wake_new();
    voice_init();
    /* --cli: ни сырого режима, ни альтернативного экрана — обычные строки (и конвейер) */
    if (O.cli) cli_init();
    else {
        if (shot && !term_open_window(true)) { term_puts("--shot: окно не создалось\n"); vless_stop(); secret_destroy(); return 1; }
        if (!term_init()) { term_puts("нужен интерактивный терминал\n"); vless_stop(); secret_destroy(); return 1; }
        ui_init();
    }
    if (O.demo) { demo_load(); ui_on_ready(); }
    if (O.demo && getenv("MC_TEST_RELOGIN")) g_auth_failed = true;   /* проверка повторного входа без сети */
    img_enable(!O.cli && !O.no_images && !O.demo);
    if (!win_login && vless_step && !O.cli) {      /* стартовый экран: только VLESS (токен уже есть) */
        ui_vless_begin(verr, true);
        if (shot) { exit_rc = shot_run(shot, shot_keys); g_quit = true; shot = NULL; }   /* --demo-start: снимок экрана */
        while (!g_quit && ui_login_active()) loop_once(INT64_MAX);
        if (!g_quit && vless_state() == VL_DOWN) g_quit = true;   /* не выбрано ни «работает», ни «напрямую» */
    }
    if (win_login) {                            /* окно входа: до получения токена или Ctrl+Q */
        if (!login_init()) { term_shutdown(); term_puts("не удалось выделить защищённую память\n"); vless_stop(); net_cleanup(); secret_destroy(); return 1; }
        if (vless_step) ui_vless_begin(verr, false); else ui_login_begin(false);
        while (!g_quit && ui_login_active()) loop_once(INT64_MAX);
        login_finish();                         /* пароль, коды, тикеты, ключ RSA затёрты */
        if (!secret_token()) g_quit = true;
    }
    if (!g_quit) gw_start();
    ui_dirty();
    if (shot) { exit_rc = shot_run(shot, shot_keys); g_quit = true; }

    while (!g_quit) loop_once(INT64_MAX);

    voice_shutdown();
    vless_stop();                               /* погасить Xray и затереть ключ до остального */
    if (O.cli) cli_shutdown(); else term_shutdown();
    img_forget();                               /* прервать загрузку и затереть кэш до сети */
    net_cleanup();
    if (!O.cli) ui_shutdown();
    store_reset();
    secret_destroy();
    wake_free(g_wake);
    if (g_auth_failed && O.cli) term_puts("Токен не принят Discord. Удалить сохранённый: Astra DS --logout\n");
    return exit_rc;
}
