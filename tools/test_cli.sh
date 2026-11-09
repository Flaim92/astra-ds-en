#!/usr/bin/env bash
# Проверка консольного режима (--cli) снаружи: команды через конвейер в демо-режиме.
#   bash tools/test_cli.sh [путь к клиенту]
# Без аргумента: $MC_BIN, иначе ./build/astra-ds|mincord (Linux) или первый ./build*/astra-ds.exe|mincord.exe (Windows).
# Проверяется: код выхода 0; в выводе нет ESC (0x1b), BEL (0x07), прочих C0 (кроме \t \n \r), DEL, C1
# (U+0080-U+009F), нет «…» и «—»/«–» в оформлении; ожидаемые строки на месте.
# Ничего не пишет на диск, кроме временного файла вывода (удаляется).
set -u
cd "$(dirname "$0")/.."

BIN=${1:-${MC_BIN:-}}
if [ -z "$BIN" ]; then
    for f in ./build/astra-ds ./build/mincord ./build/astra-ds.exe ./build-*/astra-ds.exe ./build/mincord.exe ./build-*/mincord.exe; do
        [ -x "$f" ] && { BIN=$f; break; }
    done
fi
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "нет исполняемого файла клиента (укажите путь аргументом)"; exit 2; }
command -v perl >/dev/null || { echo "нужен perl"; exit 2; }

OUTF=$(mktemp 2>/dev/null || echo "/tmp/cli-test-$$.txt")
trap 'rm -f "$OUTF"' EXIT
pass=0; fail=0
ok()  { pass=$((pass + 1)); echo "  [ ok ] $1"; }
bad() { fail=$((fail + 1)); echo "  [FAIL] $1"; }

# $1 — ввод (printf-формат), остальное — флаги
run() {
    local input=$1; shift
    if command -v timeout >/dev/null; then
        printf "$input" | timeout 30 "$BIN" --demo --cli "$@" >"$OUTF" 2>&1
    else
        printf "$input" | "$BIN" --demo --cli "$@" >"$OUTF" 2>&1
    fi
}

# Управляющие байты и типографика: число вхождений каждого класса
check_clean() {
    local tag=$1
    LC_ALL=C PERL_BADLANG=0 perl -e '
        local $/; open my $f, "<:raw", $ARGV[0] or die; my $s = <$f>;
        my %n;
        $n{"ESC"}   = () = $s =~ /\x1b/g;
        $n{"BEL"}   = () = $s =~ /\x07/g;
        $n{"C0"}    = () = $s =~ /[\x00-\x06\x08\x0b\x0c\x0e-\x1a\x1c-\x1f\x7f]/g;
        $n{"C1"}    = () = $s =~ /\xc2[\x80-\x9f]/g;
        my $c = $s; $n{"BADUTF8"} = utf8::decode($c) ? 0 : 1;   # одиночные байты 0x80-0x9F и прочий битый UTF-8
        $n{"ELLIPSIS"} = () = $s =~ /\xe2\x80\xa6/g;
        $n{"DASH"}  = () = $s =~ /\xe2\x80[\x93\x94]/g;
        $n{"BOXDRAW"} = () = $s =~ /\xe2\x94[\x80-\xbf]|\xe2\x95[\x80-\xbf]/g;
        print join(" ", map { "$_=$n{$_}" } sort keys %n), "\n";
        exit((grep { $n{$_} } keys %n) ? 1 : 0);
    ' "$OUTF" >"$OUTF.cnt"
    local rc=$?
    local cnt; cnt=$(cat "$OUTF.cnt"); rm -f "$OUTF.cnt"
    if [ $rc -eq 0 ]; then ok "$tag: нет ESC, BEL, C0, C1, «…», «—/–», псевдографики"
    else bad "$tag: найдены запрещённые байты: $cnt"; fi
}

expect() {   # expect "строка" — должна быть в выводе
    if grep -qF -- "$1" "$OUTF"; then ok "есть: $1"; else bad "нет строки: $1"; fi
}
expect_not() {
    if grep -qF -- "$1" "$OUTF"; then bad "лишнее в выводе: $1"; else ok "нет: $1"; fi
}

echo "--cli: $BIN"

# Демо-данные (src/demo.c, с 09.11.2026): сервер 1 «Уютный уголок» (#general — 5 сообщений, голосовой
# ~ОБЩИЙ (/join ОБЩИЙ — по имени: номера сдвигаются), в нём «Сонный Кот» и Боб с выкл. микрофоном), сервер 2
# «Другой сервер»; ЛС: «Алиса, Боб» (1), «Алиса» (2); друзья: 1 Алиса ... 5 Сонный Кот,
# 6 Незнакомец (входящая), 7 Ваня (исходящая), 8 Спамер (заблокирован).

# ---- 1. основной набор команд ----
CMDS='/help\n/servers\n/channels 1\n/open general\nпривет из теста\n/history\n/reply 1 x\n/edit 6 y\n/del 6\n/del 6 yes\n/react 5 1\n/join ОБЩИЙ\n/status\n/mute\n/deaf\n/leave\n/dms\n/open Алиса, Боб\n/call неизвестная\n//слэш\n/quit\n'
run "$CMDS"
rc=$?
[ $rc -eq 0 ] && ok "код выхода 0 (основной набор)" || bad "код выхода $rc (основной набор)"
check_clean "основной набор"
expect "консольный режим"
expect "демо-режим"
expect "/servers"
expect "/history [N]"
expect "/friends [online|pending|blocked]"
expect "/search текст"
expect "/file путь"
expect "1  Уютный уголок"
expect "2  Другой сервер"
expect "Уютный уголок: каналы"
expect "#general"
expect "~ОБЩИЙ"
expect "~Отдых"
expect "- Боб (мик. выкл)"
expect "открыт #general [Уютный уголок]"
expect "(начало истории)"
expect "Алиса: всем привет :wave:"
expect "Боб: @Вы держи ссылку"
expect "+ [1] "
expect "Вы: привет из теста"
expect "Вы: x"
expect "в демо-режиме правка не отправляется"
expect "подтвердите: /del 6 yes"
expect "в демо-режиме удаление не отправляется"
expect "в демо-режиме реакции не отправляются"
expect "подключаюсь к ~ОБЩИЙ"
expect "голос: ~ОБЩИЙ"
expect "участники"
expect "микрофон выключен"
expect "звук выключен"
expect "голос: отключено"
expect "личные сообщения ("
expect "открыт @Алиса, Боб"
expect "Вы: /слэш"
expect_not "Вы: //слэш"
expect_not "неизвестная команда"
# вредный текст из «сети» (демо-сообщение mallory) показан очищенным
expect "mallory:  очистка экрана"     # OSC-заголовок «взлом» вырезан целиком, ESC[2J — тоже

# ---- 2. ошибки ввода и конец ввода без /quit ----
run '/foo\n/open 99\n/del 1 yes\nтекст без канала\n/open general\n/edit 1 z\n/del 1 yes\n/react 99 1\n/history 2\n'
rc=$?
[ $rc -eq 0 ] && ok "код выхода 0 (конец ввода без /quit)" || bad "код выхода $rc (конец ввода без /quit)"
check_clean "ошибки ввода"
expect "неизвестная команда /foo"
expect "нет номера 99"
expect "сначала откройте канал"
expect "править можно только свои"
expect "удалять можно только свои"
expect "нет сообщения (99)"
expect "2 из 5 загруженных"

# ---- 3. фаза 4: друзья, новая ЛС, поиск, файл, смена голоса, VLESS ----
run '/friends\n/friends pending\n/friends blocked\n/friends online\n/friends xyz\n/friend accept 6\n/friend accept 1\n/friend remove 2\n/friend remove 2 yes\n/friend\n/dm 99\n/dm\n/dm 8\n/dm 1\n/status\n/search\n/search привет\n/file\n/file Z:/нет/такого.bin\n/voice low\n/voice\n/vless\n/quit\n'
rc=$?
[ $rc -eq 0 ] && ok "код выхода 0 (фаза 4)" || bad "код выхода $rc (фаза 4)"
check_clean "фаза 4"
expect "1  Алиса - в сети"
expect "2  Боб - не активен"
expect "6  Незнакомец - входящая заявка"
expect "7  Ваня - исходящая заявка"
expect "8  Спамер - заблокирован"
expect "использование: /friends"
expect "заявка принята (демо)"
expect "принимать нечего"
expect_not "нет входящей заявки"
expect "подтвердите: /friend remove N yes"
expect "удалено (демо)"
expect "использование: /friend accept N"
expect "нет записи 99 в /friends"
expect "использование: /dm N"
expect "заблокирован"
expect "открыт @Алиса"
expect_not "канал не открыт"
expect "использование: /search"
expect "использование: /file"
expect "голос: ниже"
expect "VLESS: выключен"

# ---- 4. поиск и файл в открытом канале ----
run '/open general\n/search привет\n/search close\n/search qqqzzz\n/file Z:/нет/такого.bin\n'
rc=$?
[ $rc -eq 0 ] && ok "код выхода 0 (поиск и файл)" || bad "код выхода $rc (поиск и файл)"
check_clean "поиск и файл"
expect 'поиск "привет"'
expect "поиск закрыт"
expect "файл не найден"
TMPF=$(mktemp 2>/dev/null || echo "/tmp/cli-test-big-$$.bin")
truncate -s 11M "$TMPF" 2>/dev/null || dd if=/dev/zero of="$TMPF" bs=1M count=11 2>/dev/null
NATF=$TMPF
command -v cygpath >/dev/null && NATF=$(cygpath -m "$TMPF")   # Windows: путь для .exe, прямые слэши
run "/open general\n/file \"$NATF\" подпись\n"
rm -f "$TMPF"
expect "10 МБ"

# ---- 3. пустой ввод ----
run ''
rc=$?
[ $rc -eq 0 ] && ok "код выхода 0 (пустой ввод)" || bad "код выхода $rc (пустой ввод)"
check_clean "пустой ввод"

echo
echo "Итог: $pass ok, $fail FAIL"
[ $fail -eq 0 ]
