#!/usr/bin/env bash
# АО-6: зашифровать закрытые ключи подписи релизов паролем (PKCS#8, AES-256-CBC, PBKDF2-HMAC-SHA256, 600000 итераций).
#   bash tools/protect_keys.sh [КАТАЛОГ]        (по умолчанию E:/projects/astra-ds-signing — ВНЕ репозитория)
# Пароль вводит человек с терминала (дважды); скрипт его не придумывает и не хранит: openssl получает его через
# канал stdin — не в аргументах (их видно в списке процессов) и не в окружении. Уже зашифрованные ключи не трогает.
# После шифрования: проверка (тот же открытый ключ), затем открытый текст затирается случайными байтами и заменяется.
# Запасной ключ (astra-ds-ed25519-2.pem) после этого — на отдельный носитель, отсюда удалить.
set -euo pipefail
dir=${1:-/e/projects/astra-ds-signing}
[ -d "$dir" ] || { echo "нет каталога $dir" >&2; exit 1; }
[ -r /dev/tty ] || { echo "нужен терминал: пароль вводит человек" >&2; exit 1; }
keys=()
for k in "$dir"/*.pem; do
    [ -f "$k" ] && grep -q -- "-----BEGIN PRIVATE KEY-----" "$k" && keys+=("$k")
done
if [ ${#keys[@]} -eq 0 ]; then echo "незашифрованных ключей в $dir нет"; exit 0; fi
echo "Будут зашифрованы: ${keys[*]}"
read -rs -p "Пароль (не короче 12 знаков): " pw < /dev/tty; echo
[ ${#pw} -ge 12 ] || { unset pw; echo "пароль короче 12 знаков" >&2; exit 1; }
read -rs -p "Ещё раз: " pw2 < /dev/tty; echo
[ "$pw" = "$pw2" ] || { unset pw pw2; echo "пароли не совпали" >&2; exit 1; }
unset pw2
umask 077
for k in "${keys[@]}"; do
    t="$k.enc.tmp"
    rm -f "$t"
    printf '%s\n' "$pw" | openssl pkcs8 -topk8 -v2 aes-256-cbc -v2prf hmacWithSHA256 -iter 600000 -in "$k" -out "$t" -passout stdin
    a=$(openssl pkey -in "$k" -pubout)
    b=$(printf '%s\n' "$pw" | openssl pkey -in "$t" -pubout -passin stdin)
    if [ "$a" != "$b" ] || ! grep -q -- "-----BEGIN ENCRYPTED PRIVATE KEY-----" "$t"; then
        rm -f "$t"; unset pw
        echo "проверка $k не прошла — ключ оставлен как был" >&2
        exit 1
    fi
    sz=$(wc -c < "$k" | tr -d ' ')
    head -c "$sz" /dev/urandom 1<> "$k"         # затереть открытый текст на месте (на SSD — по возможности)
    sync
    mv -f "$t" "$k"
    echo "зашифрован: $k"
done
unset pw
echo "Готово. tools/sign_release.sh теперь спрашивает пароль при каждой подписи."
echo "Запасной ключ ($dir/astra-ds-ed25519-2.pem) унесите на отдельный носитель и удалите отсюда."
