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
# -fdata-sections убран (замер 10.11: -0,8 МБ exe — меньше выравнивания мелких секций); код по-прежнему по функциям
OPT="-O2 -ffunction-sections"
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
    [ -f src/anon.c ] && DEFS="$DEFS -DASTRA_ANON"   # анонимизация камеры (скрытие глаз), src/anon.c
fi
SRC="src/util.c src/update.c src/secret.c src/net.c src/store.c src/ui.c src/cli.c src/voice.c src/voicefx.c src/panic.c src/vless.c src/img.c src/login.c src/captcha.c src/impcurl.c src/demo.c src/selftest.c src/main.c $PLAT_SRC"
[ "${VIDEO:-0}" = 1 ] && SRC="$SRC src/streamtest.c src/share_x11.c src/share_pw.c src/golive.c src/viddec.c src/cam.c src/anon.c"      # обычная сборка его не компилирует вовсе; cam.c — камера (Media Foundation подгружается при первом обращении, в зависимости exe не входит; Linux — V4L2)
[ "${VIDEO:-0}" = 1 ] && [ "$OS" = linux ] && SRC="$SRC src/portal_wl.c"   # Wayland: xdg-desktop-portal ScreenCast (libdbus через dlopen)
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
    # горячий путь (звук, видео, отрисовка окна, туннель, шлюз) — -O2; остальное (интерфейс, вход, хранилище,
    # проверки) — -Os: меньше кода в памяти, скорость там не видна. Последний -O побеждает.
    case "$(basename "$f")" in
        voice.c|voicefx.c|audio_*.c|streamtest.c|share_x11.c|share_pw.c|golive.c|viddec.c|cam.c|anon.c|gui_grid.c|gui_win.c|gui_x11.c|net.c|xlite_c.c|tls13.c|vless.c|img.c) fo="" ;;
        *) fo="-Os" ;;
    esac
    gcc $CFLAGS $fo $WARN -c "$f" -o "$o"
    objs+=("$o")
done
# cJSON — сторонний код: без -Werror, но с теми же флагами защиты
gcc $CFLAGS -w -c third_party/cJSON.c -o $OUT/obj/cJSON.o
objs+=("$OUT/obj/cJSON.o")
# Windows: значок (ID 1), манифест asInvoker и libcurl-impersonate.dll (RCDATA 10) — ресурсом в exe
if [ "$OS" = win ]; then
    # libcurl-impersonate.dll — ресурсом в сжатом виде (gzip, -2,1 МБ exe); impcurl.c распаковывает zlib при входе.
    # Ресурсный файл собирается под каталог сборки: пути — абсолютные (cygpath -m), исходный src/astra.rc не меняется.
    gzip -9 -n -c third_party/impcurl/libcurl-impersonate.dll > "$OUT/obj/libcurl-impersonate.dll.gz"
    root=$(cygpath -m "$(pwd)")
    sed -e "s|\"\.\./res/|\"$root/res/|" -e "s|\"astra.manifest\"|\"$root/src/astra.manifest\"|" \
        -e "s|\"\.\./third_party/impcurl/libcurl-impersonate.dll\"|\"$(cygpath -m "$(pwd)/$OUT/obj")/libcurl-impersonate.dll.gz\"|" \
        src/astra.rc > "$OUT/obj/astra_gen.rc"
    grep -q 'libcurl-impersonate.dll.gz' "$OUT/obj/astra_gen.rc" || { echo "astra.rc: путь к DLL не заменён"; exit 1; }
    windres -O coff -I src -o $OUT/obj/astra_res.o "$OUT/obj/astra_gen.rc"
    objs+=("$OUT/obj/astra_res.o")
else
    # Linux: libcurl-impersonate.so запекается секцией в ELF (символы _binary_libcurl_impersonate_so_*).
    # cd в каталог, чтобы имя символа было чистым (без путей сборки). Грузится в impcurl.c через memfd+dlopen.
    # без отладочных и лишних символов (33 -> ~5 МБ: меньше exe и копии в memfd при входе); имя файла — то же,
    # чтобы символы _binary_libcurl_impersonate_so_* не изменились
    mkdir -p "$OUT/obj/imp"
    strip --strip-unneeded -o "$OUT/obj/imp/libcurl-impersonate.so" third_party/impcurl/libcurl-impersonate.so
    ( cd "$OUT/obj/imp" && ld -r -b binary -o ../impcurl_blob.o libcurl-impersonate.so )
    objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents "$OUT/obj/impcurl_blob.o" "$OUT/obj/impcurl_blob.o" 2>/dev/null || true
    objs+=("$OUT/obj/impcurl_blob.o")
fi

[ "${1:-}" = check ] && { echo "== объекты собраны"; exit 0; }

# Video: декодеры просмотра трансляций — FFmpeg libavcodec (h264/hevc/vp8/vp9) и dav1d (AV1), deps/build-deps.sh
VIDLIBS=""
[ "${VIDEO:-0}" = 1 ] && VIDLIBS="-lavcodec -lavutil -ldav1d"
# камера: своя копия libjpeg-turbo с SIMD (deps build_jpegcam, символы camj_*); картинки — обычная -ljpeg
[ "${VIDEO:-0}" = 1 ] && [ -f "$PFX/lib/libjpegcam.a" ] && VIDLIBS="$VIDLIBS -ljpegcam"
DEPLIBS="-L$PFX/lib $XLITE_LIB $VIDLIBS -ldave -lmlspp -lhpke -lbytes -ltls_syntax -lcurl -lopus -lspeexdsp -lrnnoise -ljpeg"
if [ "$OS" = linux ]; then DEPLIBS="$DEPLIBS -lssl"; fi
DEPLIBS="$DEPLIBS -lcrypto -lz"
g++ -o "$EXE" "${objs[@]}" $LDFLAGS $DEPLIBS $SYSLIBS
# Windows: .rodata (константы ассемблера OpenSSL) компоновщик помечает записываемой — тогда она в частной памяти
# процесса всегда (~190 КБ). Константы никто не пишет: помечаем только для чтения.
if [ "$OS" = win ]; then objcopy --set-section-flags .rodata=alloc,load,readonly,data,contents "$EXE"; fi
echo "== готово: $EXE ($(du -h "$EXE" | cut -f1))"

# Туннель вкомпонован в Astra DS (C или libxlite.a): отдельного файла рядом нет.
rm -f "$OUT/xray" "$OUT/xray.exe" "$OUT/xlite" "$OUT/xlite.exe"
