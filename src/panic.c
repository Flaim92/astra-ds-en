/* Паник-кей: тройное нажатие выбранной клавиши за 1,5 с (без других клавиш между нажатиями).
 * Срабатывание: затереть токен в памяти, остановить голос, очистить экран и буфер прокрутки,
 * погасить Xray и затереть ссылку VLESS, удалить токен и ссылку из хранилища, выйти немедленно
 * (_exit / TerminateProcess — без обработчиков выхода и без ожидания потоков).
 * Аккаунт Discord не трогается.
 *
 * Имена клавиш: F1..F12, Ctrl+A..Ctrl+Z, Esc, Tab, Enter, одиночный символ (например "`" или "ё").
 * Занятые клиентом клавиши разрешены: одиночное нажатие работает как обычно, паника — только при
 * трёх нажатиях подряд. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#endif
#include <stdio.h>
#include <string.h>
#include "mc.h"
#include "img.h"
#include "login.h"
#include "share.h"
#include "cam.h"

#define PANIC_WINDOW_MS 1500

void voice_panic_stop(void);                     /* voice.c: остановить голос без ожидания потока */

static Key pk;                                   /* ожидаемая клавиша */
static bool armed;
static char pname[16];
static int presses;
static i64 first_at;
static bool dry;                                 /* самопроверка: вместо паники — счётчик */
static int dry_hits;

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (lower(*a) != lower(*b)) return false;
    return !*a && !*b;
}

bool panic_init(const char *name) {
    armed = false;
    pname[0] = 0;
    presses = 0;
    memset(&pk, 0, sizeof pk);
    if (!name || !*name) return false;
    size_t n = strlen(name);
    if ((name[0] == 'F' || name[0] == 'f') && n >= 2 && n <= 3 && name[1] >= '1' && name[1] <= '9' &&
        (n == 2 || (name[2] >= '0' && name[2] <= '9'))) {
        int f = name[1] - '0';
        if (n == 3) f = f * 10 + (name[2] - '0');
        if (f < 1 || f > 12) return false;
        pk.code = K_F1 + f - 1;
        snprintf(pname, sizeof pname, "F%d", f);
    } else if (n == 6 && lower(name[0]) == 'c' && lower(name[1]) == 't' && lower(name[2]) == 'r' && lower(name[3]) == 'l' && name[4] == '+') {
        char c = lower(name[5]);
        if (c < 'a' || c > 'z') return false;
        pk.code = K_CHAR;
        pk.ch = (u32)c;
        pk.ctrl = true;
        snprintf(pname, sizeof pname, "Ctrl+%c", (char)(c - 32));
    } else if (ieq(name, "esc") || ieq(name, "escape")) {
        pk.code = K_ESC;
        snprintf(pname, sizeof pname, "Esc");
    } else if (ieq(name, "tab")) {
        pk.code = K_TAB;
        snprintf(pname, sizeof pname, "Tab");
    } else if (ieq(name, "enter")) {
        pk.code = K_ENTER;
        snprintf(pname, sizeof pname, "Enter");
    } else {
        u32 cp;
        int k = utf8_dec(name, &cp);
        if ((size_t)k != n || cp <= 0x20 || cp == 0x7F || cp == 0xFFFD || (cp >= 0x80 && cp < 0xA0)) return false;
        pk.code = K_CHAR;
        pk.ch = cp;
        snprintf(pname, sizeof pname, "%s", name);
    }
    armed = true;
    return true;
}

const char *panic_key_name(void) { return pname; }

static bool matches(const Key *k) {
    if (k->code != pk.code) return false;
    if (pk.code == K_CHAR) {
        if (pk.ctrl) return k->ctrl && !k->alt && (k->ch == pk.ch || k->ch == pk.ch - 32);
        return !k->ctrl && !k->alt && k->ch == pk.ch;
    }
    return !k->ctrl && !k->alt;
}

bool panic_key(const Key *k) {
    if (!armed || !k || k->code == K_NONE || k->code == K_RESIZE) return false;
    if (!matches(k)) { presses = 0; return false; }   /* другая клавиша между нажатиями — сначала */
    i64 now = now_ms();
    if (!presses || now - first_at > PANIC_WINDOW_MS) { presses = 1; first_at = now; return true; }
    if (++presses >= 3) { presses = 0; panic_now(); }
    return true;
}

/* ---------- очистка терминала ---------- */
/* Выйти из альтернативного экрана, очистить экран и буфер прокрутки, сбросить терминал. */
static const char WIPE_SEQ[] =
    "\x1b[0m\x1b[2J\x1b[?2004l\x1b[?1049l\x1b[?25h"   /* альтернативный экран: очистить и выйти */
    "\x1b[H\x1b[2J\x1b[3J"                          /* основной экран и прокрутка */
    "\x1b" "c"                                      /* полный сброс (RIS): в части терминалов чистит и прокрутку */
    "\x1b[H\x1b[2J\x1b[3J";

static void screen_wipe(void) {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD m = 0, done;
    if (GetConsoleMode(out, &m)) SetConsoleMode(out, m | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    WriteFile(out, WIPE_SEQ, (DWORD)(sizeof WIPE_SEQ - 1), &done, NULL);
    /* conhost: стереть весь буфер консоли (включая прокрутку) через API */
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (GetConsoleScreenBufferInfo(out, &bi) && bi.dwSize.X > 0 && bi.dwSize.Y > 0) {
        DWORD cells = (DWORD)bi.dwSize.X * (DWORD)bi.dwSize.Y;
        COORD o = { 0, 0 };
        FillConsoleOutputCharacterW(out, L' ', cells, o, &done);
        FillConsoleOutputAttribute(out, bi.wAttributes, cells, o, &done);
        SetConsoleCursorPosition(out, o);
        SMALL_RECT win = { 0, 0, (SHORT)(bi.srWindow.Right - bi.srWindow.Left), (SHORT)(bi.srWindow.Bottom - bi.srWindow.Top) };
        SetConsoleWindowInfo(out, TRUE, &win);
    }
    SetConsoleTitleW(L"");
#else
    if (O.cli && isatty(0)) {                    /* --cli: вернуть эхо, если ждали скрытую ссылку */
        struct termios t;
        if (!tcgetattr(0, &t)) { t.c_lflag |= ECHO | ICANON; tcsetattr(0, TCSANOW, &t); }
    }
    const char *p = WIPE_SEQ;
    size_t n = sizeof WIPE_SEQ - 1;
    while (n) {
        ssize_t w = write(1, p, n);
        if (w <= 0) break;
        p += w;
        n -= (size_t)w;
    }
#endif
}

void panic_now(void) {
    if (dry) { dry_hits++; return; }
#ifdef ASTRA_VIDEO
    share_stop_nowait();                         /* трансляция экрана: только флаг + ожидание <= 150 мс (В-5-б), дальше — токен */
    cam_stop_nowait();                           /* камера: только флаг (поток захвата закроет её сам; процесс сейчас выйдет) */
#endif
    secret_destroy();                            /* токен: затереть в памяти — первым делом */
    login_panic();                               /* окно входа: пароль, коды, тикеты */
    voice_panic_stop();                          /* микрофон и голос: стоп без ожидания потока */
    img_forget();                                /* кэш картинок и загрузку с CDN */
    if (!O.cli) term_shutdown();                 /* вернуть терминал из сырого режима */
    screen_wipe();
    vless_stop();                                /* погасить Xray, затереть ключ */
    cred_delete();                               /* токен из системного хранилища */
    vless_forget();                              /* ссылку VLESS из хранилища */
    keyring_delete("startup");                  /* выбор стартового экрана (под новым и старым именем) */
    voice_settings_forget();                    /* настройки голоса: не дописать заново при выходе */
    keyring_delete("update-checked");           /* дата проверки обновлений (АО-5) */
    keyring_delete("upd_told");                 /* разовое уведомление об автопроверке */
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
#else
    _exit(0);
#endif
}

/* Самопроверка разбора имён и счёта нажатий (без настоящей паники). */
int panic_selftest(void (*check)(bool ok, const char *what));
int panic_selftest(void (*check)(bool ok, const char *what)) {
    bool names = panic_init("F12") && !strcmp(pname, "F12") && pk.code == K_F12 &&
                 panic_init("ctrl+x") && !strcmp(pname, "Ctrl+X") && pk.ctrl && pk.ch == 'x' &&
                 panic_init("Esc") && pk.code == K_ESC && panic_init("Tab") && panic_init("Enter") &&
                 panic_init("`") && panic_init("\xD1\x91") && pk.ch == 0x451 &&
                 !panic_init("F13") && !panic_init("F0") && !panic_init("Ctrl+1") && !panic_init("ab") && !panic_init(" ") && !panic_init("");
    check(names, "паник-кей: имена клавиш (F1..F12, Ctrl+буква, Esc, Tab, Enter, символ)");
    dry = true;
    dry_hits = 0;
    panic_init("F12");
    Key f12 = { .code = K_F12 }, other = { .code = K_CHAR, .ch = 'a' }, rs = { .code = K_RESIZE }, sf12 = { .code = K_F12, .ctrl = true };
    panic_key(&f12); panic_key(&f12); panic_key(&other); panic_key(&f12); panic_key(&f12);
    bool no1 = dry_hits == 0;                    /* 2 + чужая + 2: не срабатывает */
    panic_key(&sf12);                            /* Ctrl+F12 — другая клавиша, сбрасывает */
    panic_key(&f12); panic_key(&rs); panic_key(&f12); panic_key(&f12);
    bool yes = dry_hits == 1;                    /* изменение размера окна не считается нажатием */
    panic_init("Ctrl+Q");
    Key cq = { .code = K_CHAR, .ch = 'q', .ctrl = true }, q = { .code = K_CHAR, .ch = 'q' };
    panic_key(&cq); panic_key(&q); panic_key(&cq); panic_key(&cq);
    bool no2 = dry_hits == 1;
    panic_key(&cq);
    bool yes2 = dry_hits == 2;
    dry = false;
    panic_init(NULL);
    check(no1 && yes && no2 && yes2, "паник-кей: срабатывает только на 3 нажатия подряд без других клавиш");
    return 0;
}
