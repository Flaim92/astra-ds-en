#!/usr/bin/env bash
# Сборка Astra DS. Windows: Git Bash + WinLibs GCC. Linux: bash + gcc.
# Сначала зависимости: bash deps/build-deps.sh
#   ./build.sh          — релизная сборка
#   ./build.sh check    — только компиляция объектов (быстрая проверка)
set -euo pipefail
cd "$(dirname "$0")"
PFX=deps/prefix
OUT=${OUT:-build}   # свой каталог для параллельных сборок: OUT=build-dev bash build.sh
mkdir -p "$OUT/obj"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) OS=win ;;
    Linux) OS=linux ;;
    *) echo "неизвестная система"; exit 1 ;;
esac

WARN="-Wall -Wextra -Werror -Wno-missing-field-initializers -Wshadow -Wformat=2 -Wformat-security"
HARDEN="-fstack-protector-strong -D_FORTIFY_SOURCE=2"
OPT="-O2 -ffunction-sections -fdata-sections"
DEFS="-DCURL_STATICLIB -DCJSON_NESTING_LIMIT=64"
INC="-I$PFX/include"

if [ "$OS" = win ]; then
    PLAT_SRC="src/plat_win.c src/audio_wasapi.c src/gui_win.c src/gui_grid.c"
    DEFS="$DEFS -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE"
    LDFLAGS="-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -s"
    SYSLIBS="-lws2_32 -lcrypt32 -lsecur32 -lbcrypt -ladvapi32 -lole32 -loleaut32 -luuid -lavrt -lwer -lwinmm -liphlpapi -lksuser -luser32 -lgdi32 -ldwmapi -lshell32 -lshlwapi -lwintrust"
    INC="$INC -isystem third_party/webview2"   # WebView2.h (капча при входе, src/captcha.c): официальный заголовок, без -Werror
    EXE=$OUT/astra-ds.exe
else
    PLAT_SRC="src/plat_posix.c src/audio_alsa.c src/gui_x11.c src/gui_grid.c"
    DEFS="$DEFS -D_GNU_SOURCE"
    HARDEN="$HARDEN -fPIE"
    LDFLAGS="-pie -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-z,relro,-z,now -Wl,-z,noexecstack -s"
    SYSLIBS="-lasound -lX11 -lpthread -ldl -lm"
    EXE=$OUT/astra-ds
fi

# VIDEO=1: «Astra DS Video» (демонстрация экрана и камера) — отдельный продукт со своим обновлением
if [ "${VIDEO:-0}" = 1 ]; then
    DEFS="$DEFS -DASTRA_VIDEO"
    EXE=${EXE/astra-ds/astra-ds-video}
fi
SRC="src/util.c src/update.c src/secret.c src/net.c src/store.c src/ui.c src/cli.c src/voice.c src/voicefx.c src/panic.c src/vless.c src/img.c src/login.c src/captcha.c src/impcurl.c src/demo.c src/selftest.c src/main.c $PLAT_SRC"
[ "${VIDEO:-0}" = 1 ] && SRC="$SRC src/streamtest.c src/share_x11.c src/golive.c"      # обычная сборка его не компилирует вовсе
# Туннель VLESS: VLESS_IMPL=c (по умолчанию) — свой на C (src/xlite_c.c + src/tls13.c, декодер brotli
# для сжатых сертификатов), без среды Go; VLESS_IMPL=go — libxlite.a (Xray-core на Go, tools/xlite).
VLESS_IMPL=${VLESS_IMPL:-c}
case "$VLESS_IMPL" in
    go) XLITE_LIB="-lxlite" ;;
    c)  XLITE_LIB="-lbrotlidec"; SRC="$SRC src/xlite_c.c src/tls13.c"; DEFS="$DEFS -DVLESS_C" ;;
    *)  echo "VLESS_IMPL: go или c"; exit 1 ;;
esac
CFLAGS="-std=gnu11 $OPT $HARDEN $DEFS $INC"

objs=()
for f in $SRC; do
    [ -f "$f" ] || { echo "нет $f — пропуск"; continue; }
    o=$OUT/obj/$(basename "${f%.c}").o
    gcc $CFLAGS $WARN -c "$f" -o "$o"
    objs+=("$o")
done
# cJSON — сторонний код: без -Werror, но с теми же флагами защиты
gcc $CFLAGS -w -c third_party/cJSON.c -o $OUT/obj/cJSON.o
objs+=("$OUT/obj/cJSON.o")
# Windows: значок (ID 1), манифест asInvoker и libcurl-impersonate.dll (RCDATA 10) — ресурсом в exe
if [ "$OS" = win ]; then
    windres -O coff -o $OUT/obj/astra_res.o src/astra.rc
    objs+=("$OUT/obj/astra_res.o")
else
    # Linux: libcurl-impersonate.so запекается секцией в ELF (символы _binary_libcurl_impersonate_so_*).
    # cd в каталог, чтобы имя символа было чистым (без путей сборки). Грузится в impcurl.c через memfd+dlopen.
    ( cd third_party/impcurl && ld -r -b binary -o "$OLDPWD/$OUT/obj/impcurl_blob.o" libcurl-impersonate.so )
    objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents "$OUT/obj/impcurl_blob.o" "$OUT/obj/impcurl_blob.o" 2>/dev/null || true
    objs+=("$OUT/obj/impcurl_blob.o")
fi

[ "${1:-}" = check ] && { echo "== объекты собраны"; exit 0; }

DEPLIBS="-L$PFX/lib $XLITE_LIB -ldave -lmlspp -lhpke -lbytes -ltls_syntax -lcurl -lopus -lspeexdsp -lrnnoise -ljpeg"
if [ "$OS" = linux ]; then DEPLIBS="$DEPLIBS -lssl"; fi
DEPLIBS="$DEPLIBS -lcrypto -lz"
g++ -o "$EXE" "${objs[@]}" $LDFLAGS $DEPLIBS $SYSLIBS
echo "== готово: $EXE ($(du -h "$EXE" | cut -f1))"

# Туннель вкомпонован в Astra DS (C или libxlite.a): отдельного файла рядом нет.
rm -f "$OUT/xray" "$OUT/xray.exe" "$OUT/xlite" "$OUT/xlite.exe"
