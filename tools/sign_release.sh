#!/usr/bin/env bash
# Подпись релиза для автообновления (SECURITY.md «Автообновление»).
#   bash tools/sign_release.sh ФАЙЛ ПРОДУКТ ПЛАТФОРМА ВЕРСИЯ [КЛЮЧ]
#   ПРОДУКТ: astra-ds | astra-ds-video;  ПЛАТФОРМА: win-x64 | linux-x64;  ВЕРСИЯ: x.y.z
#   (строго больше ASTRA_VERSION прошлой сборки этого продукта)
#   КЛЮЧ по умолчанию: E:/projects/astra-ds-signing/astra-ds-ed25519.pem (ВНЕ репозитория; в git не класть)
# Создаёт рядом с ФАЙЛОМ: manifest-ПРОДУКТ-ПЛАТФОРМА.json (<= 4 КБ) и .json.sig (ed25519, 64 Б).
# В релиз GitHub выкладываются: сам ФАЙЛ (имя — как в манифесте), манифест и подпись — для каждого продукта
# и каждой платформы (обычный и Video подписываются отдельно, каждый своим вызовом).
# АО-6: ключ зашифрован (tools/protect_keys.sh) — пароль спрашивается с терминала при каждом вызове и уходит в
# openssl через stdin (не в аргументах и не в окружении). Незашифрованный ключ — отказ (для проверочного
# одноразового ключа: SIGN_PLAIN_OK=1).
set -euo pipefail
f=${1:?файл}; prod=${2:?продукт}; plat=${3:?платформа}; ver=${4:?версия}
key=${5:-/e/projects/astra-ds-signing/astra-ds-ed25519.pem}
case "$prod" in astra-ds|astra-ds-video) ;; *) echo "продукт: astra-ds или astra-ds-video" >&2; exit 1;; esac
case "$plat" in win-x64|linux-x64) ;; *) echo "платформа: win-x64 или linux-x64" >&2; exit 1;; esac
[[ "$ver" =~ ^[0-9]{1,5}\.[0-9]{1,5}\.[0-9]{1,5}$ ]] || { echo "версия: x.y.z" >&2; exit 1; }
[ -f "$f" ] || { echo "нет файла $f" >&2; exit 1; }
[ -f "$key" ] || { echo "нет ключа $key" >&2; exit 1; }
pw=""
if grep -q -- "-----BEGIN ENCRYPTED PRIVATE KEY-----" "$key"; then
    [ -r /dev/tty ] || { echo "нужен терминал: пароль ключа вводит человек" >&2; exit 1; }
    read -rs -p "Пароль ключа подписи: " pw < /dev/tty; echo >&2
elif [ "${SIGN_PLAIN_OK:-0}" != 1 ]; then
    echo "ключ $key не зашифрован — сначала bash tools/protect_keys.sh" >&2; exit 1
fi
trap 'unset pw' EXIT
name=$(basename "$f")
[[ "$name" =~ ^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$ ]] || { echo "имя файла: [A-Za-z0-9._-], до 64" >&2; exit 1; }
size=$(wc -c < "$f" | tr -d ' ')
sha=$(sha256sum "$f" | cut -d' ' -f1)
dir=$(dirname "$f")
man="$dir/manifest-$prod-$plat.json"
printf '{"product":"%s","platform":"%s","version":"%s","file":"%s","size":%s,"sha256":"%s"}' \
    "$prod" "$plat" "$ver" "$name" "$size" "$sha" > "$man"
printf '%s\n' "$pw" | openssl pkeyutl -sign -rawin -inkey "$key" -passin stdin -in "$man" -out "$man.sig"
[ "$(wc -c < "$man.sig" | tr -d ' ')" = 64 ] || { echo "подпись не 64 байта" >&2; exit 1; }
printf '%s\n' "$pw" | openssl pkey -in "$key" -passin stdin -pubout > "$dir/.pub.pem"   # проверка открытым ключом
openssl pkeyutl -verify -rawin -pubin -inkey "$dir/.pub.pem" -in "$man" -sigfile "$man.sig" >/dev/null
rm -f "$dir/.pub.pem"
echo "готово: $man (+ .sig): $prod $plat $ver, $size байт, sha256 $sha"
