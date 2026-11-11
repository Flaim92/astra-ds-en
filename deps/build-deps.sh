#!/usr/bin/env bash
# Скачивает и собирает зависимости mincord из исходников (Git Bash + WinLibs GCC).
# Каждый архив сверяется с закреплённым SHA256. Хэши проверены 08.11.2026:
#   openssl, opus      — по контрольным суммам авторов;
#   curl, zlib, json   — по GPG-подписям авторов;
#   mlspp              — по SHA512, закреплённому в самой libdave (vcpkg overlay).
# Результат: deps/prefix/{include,lib}. Запуск: bash deps/build-deps.sh [имя...]
set -euo pipefail
cd "$(dirname "$0")"
DEPS=$(pwd)
SRC=$DEPS/src
BLD=$DEPS/build
PFX=$DEPS/prefix
mkdir -p "$SRC" "$BLD" "$PFX"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) OS=win; MAKE=mingw32-make ;;
    Darwin) OS=mac; MAKE=make ;;
    *) OS=linux; MAKE=make ;;
esac
if [ "$OS" = mac ]; then
    # macOS: скрипт рассчитан на GNU sed и sha256sum (brew install gnu-sed coreutils); компилятор — clang
    for g in gnu-sed coreutils; do
        d=$(brew --prefix "$g" 2>/dev/null)/libexec/gnubin
        [ -d "$d" ] && PATH="$d:$PATH"
    done
    sed --version > /dev/null 2>&1 || { echo "нужен GNU sed: brew install gnu-sed coreutils"; exit 1; }
    command -v sha256sum > /dev/null || { echo "нужен sha256sum: brew install coreutils"; exit 1; }
    export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-12.0}
    export CC=clang CXX=clang++
    gcc() { clang "$@"; }                      # сборка мелких библиотек ниже вызывает gcc напрямую
    JOBS=$(sysctl -n hw.ncpu)
else
    JOBS=$(nproc)
fi

# Общие флаги защиты и размера для всего, что попадает в .exe.
HARDEN="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections"
[ "$OS" = linux ] && HARDEN="$HARDEN -fPIC"   # статические библиотеки внутри PIE
export CFLAGS="$HARDEN"
export CXXFLAGS="$HARDEN"
# macOS: Apple clang ищет /usr/local/include РАНЬШЕ каталогов -isystem, а CMake подключает найденные библиотеки
# через -isystem. На Intel там Homebrew (openssl@3 и др.): curl собирался с чужими заголовками OpenSSL.
# Каталоги -I идут раньше /usr/local/include — свой prefix первым.
if [ "$OS" = mac ]; then
    export CFLAGS="$CFLAGS -I$PFX/include"
    export CXXFLAGS="$CXXFLAGS -I$PFX/include"
fi

fetch() { # файл url sha256
    local f=$SRC/$1
    [ -f "$f" ] || curl -fsSL --proto '=https' --retry 5 --retry-all-errors -o "$f.part" "$2" && { [ -f "$f" ] || mv "$f.part" "$f"; }
    echo "$3  $f" | sha256sum -c --quiet - || { echo "ХЭШ НЕ СОВПАЛ: $1"; rm -f "$f"; exit 1; }
}

unpack() { # архив каталог
    rm -rf "$BLD/$2"
    mkdir -p "$BLD/$2"
    tar -xf "$SRC/$1" -C "$BLD/$2" --strip-components=1
}

cmake_static() { # исходники билд-каталог [опции...]
    local s=$1 b=$2; shift 2
    # macOS: Homebrew (/usr/local на Intel, /opt/homebrew на arm64) не должен подменять наши библиотеки.
    # Пути к OpenSSL — явно. Порядок заголовков (свой prefix раньше /usr/local/include) — в CFLAGS выше.
    local mac=()
    [ "$OS" = mac ] && mac=(-DCMAKE_IGNORE_PREFIX_PATH="/usr/local;/opt/homebrew" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
        -DOPENSSL_INCLUDE_DIR="$PFX/include" -DOPENSSL_CRYPTO_LIBRARY="$PFX/lib/libcrypto.a"
        -DOPENSSL_SSL_LIBRARY="$PFX/lib/libssl.a" --no-warn-unused-cli)
    cmake -S "$s" -B "$b" -G Ninja "${mac[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PFX" \
        -DCMAKE_PREFIX_PATH="$PFX" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        "$@" > "$b.configure.txt" 2>&1 || { tail -40 "$b.configure.txt"; exit 1; }
    cmake --build "$b" -j "$JOBS" > "$b.build.txt" 2>&1 || { grep -m20 -E 'error|Error' "$b.build.txt"; tail -20 "$b.build.txt"; exit 1; }
    cmake --install "$b" > /dev/null
}

build_zlib() {
    fetch zlib-1.3.2.tar.gz https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz \
        bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    unpack zlib-1.3.2.tar.gz zlib
    cmake_static "$BLD/zlib" "$BLD/zlib-b" -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON
    # Разные версии CMake-скрипта zlib называют библиотеку по-разному; приводим к libz.a.
    for n in libzlibstatic.a libzs.a libzlib.a; do [ -f "$PFX/lib/$n" ] && cp "$PFX/lib/$n" "$PFX/lib/libz.a"; done
    rm -f "$PFX"/lib/libzlib.dll.a "$PFX"/bin/*zlib*.dll
    [ -f "$PFX/lib/libz.a" ]
}

build_openssl() {
    # В Perl из Git для Windows нет Locale::Maketext::Simple (нужен IPC::Cmd -> Params::Check).
    # Модуль только переводит сообщения, поэтому хватает заглушки в своей папке.
    mkdir -p "$DEPS/perl5/Locale/Maketext"
    cat > "$DEPS/perl5/Locale/Maketext/Simple.pm" <<'PM'
package Locale::Maketext::Simple;
sub import {
    my $caller = caller;
    no strict 'refs';
    *{"${caller}::loc"} = sub { my $s = shift; $s =~ s/\[_(\d+)\]/$_[$1-1]/g; $s };
    *{"${caller}::loc_lang"} = sub { };
}
1;
PM
    # Из ExtUtils::MakeMaker IPC::Cmd берёт только MM->maybe_command (поиск программы).
    mkdir -p "$DEPS/perl5/ExtUtils"
    cat > "$DEPS/perl5/ExtUtils/MakeMaker.pm" <<'PM'
package ExtUtils::MakeMaker;
our $VERSION = '7.70';
package MM;
sub maybe_command {
    my ($self, $f) = @_;
    return $f if -f $f && -x _;
    for my $e ('.exe', '.bat', '.cmd') { return "$f$e" if -f "$f$e"; }
    return;
}
1;
PM
    # Pod::Usage нужен configdata.pm только для вывода справки.
    mkdir -p "$DEPS/perl5/Pod"
    cat > "$DEPS/perl5/Pod/Usage.pm" <<'PM'
package Pod::Usage;
use Exporter 'import';
our @EXPORT = ('pod2usage');
sub pod2usage { exit(ref $_[0] ? ($_[0]{-exitval} // 2) : 2); }
1;
PM
    fetch openssl-3.5.9.tar.gz https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz \
        603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    unpack openssl-3.5.9.tar.gz openssl
    if [ "$OS" = win ]; then
    # PERL5LIB не годится: Git Bash отдаёт mingw32-make путь вида E:/..., и Perl режет его по ':'.
    # Кладём заглушки в util/perl исходников: этот каталог OpenSSL сам добавляет в @INC.
    cp -r "$DEPS/perl5/." "$BLD/openssl/util/perl/"
    # configdata.pm запускается без util/perl в @INC; Pod::Usage там нужен только для справки.
    sed -i 's/use Pod::Usage;/sub pod2usage { exit 2 }/' "$BLD/openssl/configdata.pm.in"
    fi
    cd "$BLD/openssl"
    # Только libcrypto: без TLS-протоколов, движков, модулей, утилит, документации и тестов.
    # TLS в клиенте делает Schannel; OpenSSL нужен только для криптографии DAVE/MLS и AES-GCM голоса.
    # Windows: только libcrypto (TLS делает Schannel). Linux: ещё libssl для TLS в curl.
    local target=mingw64 tls="no-ssl no-tls no-sock"
    [ "$OS" = linux ] && { target=linux-x86_64; tls="no-ssl3 no-tls1 no-tls1_1"; }
    if [ "$OS" = mac ]; then
        tls="no-ssl3 no-tls1 no-tls1_1"
        case "$(uname -m)" in arm64) target=darwin64-arm64-cc ;; *) target=darwin64-x86_64-cc ;; esac
    fi
    perl Configure $target no-shared no-module no-dso no-engine no-apps no-docs no-tests \
        no-ui-console no-legacy no-deprecated $tls no-dtls no-quic no-srtp no-ct \
        no-cms no-ts no-comp no-autoload-config no-http \
        --prefix="$PFX" --libdir=lib --openssldir="$PFX/ssl" $HARDEN > "$BLD/openssl.configure.txt" 2>&1 \
        || { tail -30 "$BLD/openssl.configure.txt"; exit 1; }
    $MAKE -j "$JOBS" build_libs > "$BLD/openssl.build.txt" 2>&1 || { grep -m20 -iE 'error' "$BLD/openssl.build.txt"; tail -20 "$BLD/openssl.build.txt"; exit 1; }
    $MAKE install_dev > "$BLD/openssl.install.txt" 2>&1 || { tail -20 "$BLD/openssl.install.txt"; exit 1; }
    if [ "$OS" = win ]; then rm -f "$PFX/lib/libssl.a"; fi
    cd "$DEPS"
}

build_curl() {
    # TLS: Windows — Schannel (системный), Linux — наш OpenSSL; набор CA задаётся при запуске.
    local CURL_TLS
    if [ "$OS" = win ]; then CURL_TLS="-DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF"
    else CURL_TLS="-DCURL_USE_SCHANNEL=OFF -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR=$PFX -DOPENSSL_USE_STATIC_LIBS=ON"; fi
    fetch curl-8.23.0.tar.xz https://curl.se/download/curl-8.23.0.tar.xz \
        8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
    unpack curl-8.23.0.tar.xz curl
    # Только HTTP(S) и WebSocket, TLS через Schannel. Выключено всё, что клиенту не нужно:
    # прочие протоколы, cookies, HSTS/alt-svc (кэш-файлы), netrc, авторизация HTTP,
    # поиск файла CA рядом с .exe и в PATH (подложенный файл подменил бы доверенные центры).
    # Прокси включён ради туннеля VLESS (SOCKS5 с логином к локальному Xray). Переменные окружения
    # http_proxy/all_proxy/no_proxy клиент обезвреживает: на каждом хэндле явно задаёт
    # CURLOPT_PROXY и CURLOPT_NOPROXY ("" - без прокси и без исключений).
    cmake_static "$BLD/curl" "$BLD/curl-b" \
        -DBUILD_CURL_EXE=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
        -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
        -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON -DCURL_DISABLE_GOPHER=ON \
        -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_IPFS=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
        -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_SMTP=ON \
        -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON \
        -DCURL_DISABLE_HTTP_AUTH=ON -DCURL_DISABLE_BINDLOCAL=ON -DCURL_DISABLE_GETOPTIONS=ON \
        -DCURL_DISABLE_LIBCURL_OPTION=ON -DCURL_DISABLE_CA_SEARCH=ON \
        $CURL_TLS -DCURL_CA_FALLBACK=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none \
        -DCURL_ZLIB=ON -DZLIB_ROOT="$PFX" -DZLIB_LIBRARY="$PFX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PFX/include" \
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF -DUSE_QUICHE=OFF \
        -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
        -DCURL_USE_GSSAPI=OFF -DENABLE_UNIX_SOCKETS=OFF \
        -DCURL_DISABLE_PROXY=OFF -DCURL_DISABLE_COOKIES=ON -DCURL_DISABLE_HSTS=ON -DCURL_DISABLE_ALTSVC=ON \
        -DCURL_DISABLE_NETRC=ON -DCURL_DISABLE_BASIC_AUTH=ON -DCURL_DISABLE_BEARER_AUTH=ON \
        -DCURL_DISABLE_DIGEST_AUTH=ON -DCURL_DISABLE_KERBEROS_AUTH=ON -DCURL_DISABLE_NEGOTIATE_AUTH=ON \
        -DCURL_DISABLE_AWS=ON -DCURL_DISABLE_DOH=ON -DCURL_DISABLE_MIME=ON \
        -DCURL_DISABLE_FORM_API=ON -DCURL_DISABLE_HTTPSIG=ON -DCURL_DISABLE_PROGRESS_METER=ON \
        -DCURL_DISABLE_INSTALL=OFF -DCURL_DISABLE_VERBOSE_STRINGS=ON -DCURL_DISABLE_WEBSOCKETS=OFF \
        -DCURL_DISABLE_SHUFFLE_DNS=ON -DCURL_DISABLE_SOCKETPAIR=OFF -DCURL_DISABLE_OPENSSL_AUTO_LOAD_CONFIG=ON
    # Установка CMake кладёт только заголовки; библиотеку берём из каталога сборки.
    cp "$BLD/curl-b/lib/libcurl.a" "$PFX/lib/libcurl.a"
    # macOS: OpenSSL должен быть нашим (deps/prefix), а не из Homebrew
    if [ "$OS" = mac ]; then
        grep -q "Found OpenSSL: $PFX" "$BLD/curl-b.configure.txt" || { grep 'OpenSSL' "$BLD/curl-b.configure.txt"; echo "curl: чужой OpenSSL"; exit 1; }
        # наш OpenSSL собран no-engine (и без OCSP): ссылки на ENGINE_/OCSP_ значат, что заголовки были чужие
        if nm -u "$PFX/lib/libcurl.a" 2>/dev/null | grep -qE '^_(ENGINE_|OCSP_)'; then
            echo "curl: собран с чужими заголовками OpenSSL"
            local cmd
            cmd=$(ninja -C "$BLD/curl-b" -t commands | grep -m1 'vtls/openssl.c' || true)
            echo "$cmd"
            ls -la /usr/local/include/openssl 2>&1 | head -3; echo "CPATH=${CPATH:-} C_INCLUDE_PATH=${C_INCLUDE_PATH:-} SDKROOT=${SDKROOT:-}"
            grep -n 'OPENSSL_NO_ENGINE\|OPENSSL_NO_OCSP' "$PFX/include/openssl/configuration.h" || echo "в configuration.h нет OPENSSL_NO_ENGINE/OCSP"
            # какие заголовки openssl реально подключаются и порядок поиска
            ( cd "$BLD/curl-b" && eval "${cmd/ -o lib\/CMakeFiles\/libcurl_static.dir\/vtls\/openssl.c.o/ -o /dev/null} -H -v -fsyntax-only" 2>&1 | grep -E 'openssl/(ssl|opensslconf|configuration)\.h|^ /|search starts' | head -20 ) || true
            exit 1
        fi
    fi
    grep -q 'Protocols: http https ws wss$' "$BLD/curl-b.configure.txt" || { grep 'Protocols:' "$BLD/curl-b.configure.txt"; echo "curl: лишние протоколы"; exit 1; }
}

build_opus() {
    fetch opus-1.5.2.tar.gz https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz \
        65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
    unpack opus-1.5.2.tar.gz opus
    cmake_static "$BLD/opus" "$BLD/opus-b" -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_DRED=OFF -DOPUS_DEEP_PLC=OFF -DOPUS_OSCE=OFF
}

# Шумоподавление микрофона (разовое разрешение координатора 10.11.2026: только эти две функции).
# SpeexDSP 1.2.1 — только препроцессор (preprocess + filterbank + kiss_fft), плавающая точка; хэш — по
# SHA256SUMS.txt авторов (downloads.xiph.org/releases/speex). BSD-3, см. third_party/LICENSE.
build_speexdsp() {
    fetch speexdsp-1.2.1.tar.gz https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz \
        8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d
    unpack speexdsp-1.2.1.tar.gz speexdsp
    local b=$BLD/speexdsp o=$BLD/speexdsp-o
    rm -rf "$o"; mkdir -p "$o" "$PFX/include/speex"
    cat > "$b/include/speex/speexdsp_config_types.h" <<'EOT'
#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__
#include <stdint.h>
typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;
#endif
EOT
    for f in preprocess filterbank fftwrap kiss_fft kiss_fftr mdf; do   # mdf: preprocess ссылается на speex_echo_get_residual
        gcc $CFLAGS -std=gnu99 -DFLOATING_POINT -DUSE_KISS_FFT -DEXPORT= -I"$b/include" -I"$b/libspeexdsp" \
            -c "$b/libspeexdsp/$f.c" -o "$o/$f.o"
    done
    rm -f "$PFX/lib/libspeexdsp.a"
    ar rcs "$PFX/lib/libspeexdsp.a" "$o"/*.o
    cp "$b"/include/speex/{speex_preprocess.h,speexdsp_types.h,speexdsp_config_types.h} "$PFX/include/speex/"
}

# RNNoise 0.2 (тег v0.2, релизный архив со встроенной моделью rnnoise_data.c), без x86-SIMD (переносимый C).
# Автор не публикует хэш архива: SHA256 закреплён по архиву, скачанному с GitHub 10.11.2026. BSD-3.
build_rnnoise() {
    fetch rnnoise-0.2.tar.gz https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz \
        90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
    unpack rnnoise-0.2.tar.gz rnnoise
    local b=$BLD/rnnoise o=$BLD/rnnoise-o
    rm -rf "$o"; mkdir -p "$o"
    # Без AVX/SSE2 (ARM, Apple Silicon) vec.h берёт переносимый вариант, а он ждёт os_support.h из Opus,
    # которого в архиве нет: нужные ему макросы копирования — свои.
    [ -f "$b/src/os_support.h" ] || cat > "$b/src/os_support.h" <<'EOT'
#ifndef OS_SUPPORT_H
#define OS_SUPPORT_H
#include <string.h>
#include <stdlib.h>
#ifndef OPUS_COPY
#define OPUS_COPY(dst, src, n) (memcpy((dst), (src), (n) * sizeof(*(dst)) + 0 * ((dst) - (src))))
#endif
#ifndef OPUS_MOVE
#define OPUS_MOVE(dst, src, n) (memmove((dst), (src), (n) * sizeof(*(dst)) + 0 * ((dst) - (src))))
#endif
#ifndef OPUS_CLEAR
#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n) * sizeof(*(dst))))
#endif
#endif
EOT
    for f in denoise rnn pitch kiss_fft celt_lpc nnet nnet_default parse_lpcnet_weights rnnoise_data rnnoise_tables; do
        # DISABLE_NEON: на ARM (Apple Silicon) — тот же переносимый C (NEON-вариант ждёт os_support.h, которого нет в архиве)
        gcc $CFLAGS -std=gnu11 -DDISABLE_NEON -I"$b/include" -I"$b/src" -c "$b/src/$f.c" -o "$o/$f.o"
    done
    rm -f "$PFX/lib/librnnoise.a"
    ar rcs "$PFX/lib/librnnoise.a" "$o"/*.o
    cp "$b/include/rnnoise.h" "$PFX/include/"
}

build_json() {
    fetch json.hpp https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp \
        aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63
    mkdir -p "$PFX/include/nlohmann" "$PFX/share/nlohmann_json"
    cp "$SRC/json.hpp" "$PFX/include/nlohmann/json.hpp"
    # Однофайловый релиз идёт без CMake-конфига; даём минимальный, как у полного пакета.
    cat > "$PFX/share/nlohmann_json/nlohmann_jsonConfig.cmake" <<EOF
if(NOT TARGET nlohmann_json::nlohmann_json)
  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
  set_target_properties(nlohmann_json::nlohmann_json PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "$PFX/include")
endif()
set(nlohmann_json_FOUND TRUE)
EOF
    echo 'set(PACKAGE_VERSION 3.12.0)
set(PACKAGE_VERSION_COMPATIBLE TRUE)' > "$PFX/share/nlohmann_json/nlohmann_jsonConfigVersion.cmake"
}

build_mlspp() {
    fetch mlspp-1cc50a12.tar.gz https://github.com/cisco/mlspp/archive/1cc50a124a3bc4e143a787ec934280dc70c1034d.tar.gz \
        7a9d6318627e548903bc65c3dc5a4de4a90290983efea9a49af8561ed3f999f5
    unpack mlspp-1cc50a12.tar.gz mlspp
    # Как в overlay-порте libdave: mls_ds и mls_vectors не нужны.
    sed -i '/add_subdirectory(mls_ds)/d; /add_subdirectory(mls_vectors)/d' "$BLD/mlspp/lib/CMakeLists.txt"
    cmake_static "$BLD/mlspp" "$BLD/mlspp-b" -DDISABLE_GREASE=ON -DMLS_CXX_NAMESPACE=mlspp -DTESTING=OFF \
        -DOPENSSL_ROOT_DIR="$PFX" -DOPENSSL_USE_STATIC_LIBS=ON -Dnlohmann_json_DIR="$PFX/share/nlohmann_json"
}

build_libdave() {
    local rev=6396c90aedbb9a6ee117ea9ce1ce9832af6a3375
    if [ ! -d "$SRC/libdave/.git" ]; then git clone -q https://github.com/discord/libdave "$SRC/libdave"; fi
    git -C "$SRC/libdave" -c advice.detachedHead=false checkout -q "$rev"
    [ "$(git -C "$SRC/libdave" rev-parse HEAD)" = "$rev" ] || { echo "libdave: не тот коммит"; exit 1; }
    cmake_static "$SRC/libdave/cpp" "$BLD/libdave-b" -DTESTING=OFF -DPERSISTENT_KEYS=OFF \
        -DOPENSSL_ROOT_DIR="$PFX" -DOPENSSL_USE_STATIC_LIBS=ON \
        -Dnlohmann_json_DIR="$PFX/share/nlohmann_json" -DMLSPP_DIR="$PFX/share/MLSPP"
}

build_xray() {
    # Официальный релиз Xray-core (туннель VLESS/Reality). Хэши архивов сверены 08.11.2026 с .dgst
    # того же релиза; хэш самого бинарника закреплён ещё и в src/vless.c (проверка перед каждым запуском).
    # geoip/geosite не берём: в конфиге mincord нет гео-правил.
    local ver=v26.10.31 zip bin zsha bsha
    if [ "$OS" = win ]; then
        zip=Xray-windows-64.zip; bin=xray.exe
        zsha=2e04a9995a6f2ad1b28c0bb25c708ecb2695e43a1c9c7889c4b6ac2b48cf7a6a
        bsha=d5b558191cccb1e92a5772b68b8cab67717b26f634b0f15153215f65ae982a7e
    else
        zip=Xray-linux-64.zip; bin=xray
        zsha=e4bd9119f50ae7c41658b1abd0679ae06f98bd864a2907ff33f2f2cf7081b214
        bsha=cc4b84d3069569f297a61f04b914f2bb37e9d202fd7b84cb50def1fc30fd40e1
    fi
    fetch "xray-$ver-$zip" "https://github.com/XTLS/Xray-core/releases/download/$ver/$zip" "$zsha"
    mkdir -p "$DEPS/xray/$OS"
    local out="$DEPS/xray/$OS/$bin"
    rm -f "$out"
    if command -v unzip > /dev/null; then unzip -p "$SRC/xray-$ver-$zip" "$bin" > "$out.part"
    else python3 -I -c 'import sys,zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' \
        "$SRC/xray-$ver-$zip" "$bin" > "$out.part"; fi
    echo "$bsha  $out.part" | sha256sum -c --quiet - || { echo "ХЭШ НЕ СОВПАЛ: $bin"; rm -f "$out.part"; exit 1; }
    chmod 755 "$out.part"
    mv "$out.part" "$out"
}

build_xlite() {
    # Туннель VLESS, вкомпонованный в mincord: официальные исходники Xray-core v26.10.31 (коммит
    # a4a3dff, совпадает с тегом) + обёртка tools/xlite/main.go (только SOCKS5-вход и VLESS-выход),
    # собранные в статическую библиотеку для C (-buildmode=c-archive) -> $PFX/lib/libxlite.a.
    # Go 1.27.3 со сверкой SHA256 с go.dev. На Windows cgo компилирует тем же WinLibs gcc, что и
    # весь проект (одинаковый UCRT). DNS — чистый Go (netgo), без системного резолвера.
    local rev=a4a3dff5b20c2faae567b742c172c7b95e75c083 gover=go1.27.3 goarc gosha
    if [ "$OS" = win ]; then
        goarc=$gover.windows-amd64.zip; gosha=f26081f438319757f8a1b46d7d0b2c5ec7583d4038e485e0aa725cfde7885fff
    else
        goarc=$gover.linux-amd64.tar.gz; gosha=65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40
    fi
    fetch "$goarc" "https://go.dev/dl/$goarc" "$gosha"
    rm -rf "$BLD/go"
    if [ "$OS" = win ]; then python3 -I -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$SRC/$goarc" "$BLD"
    else tar -xzf "$SRC/$goarc" -C "$BLD"; fi
    if [ ! -d "$SRC/xray-core/.git" ]; then git clone -q https://github.com/XTLS/Xray-core "$SRC/xray-core"; fi
    git -C "$SRC/xray-core" fetch -q --depth 1 origin "$rev" 2>/dev/null || true
    git -C "$SRC/xray-core" -c advice.detachedHead=false checkout -q "$rev"
    [ "$(git -C "$SRC/xray-core" rev-parse HEAD)" = "$rev" ] || { echo "xray-core: не тот коммит"; exit 1; }
    mkdir -p "$SRC/xray-core/main/xlitelib" && cp "$DEPS/../tools/xlite/main.go" "$SRC/xray-core/main/xlitelib/main.go"
    ( cd "$SRC/xray-core" && PATH="$BLD/go/bin:$PATH" GOFLAGS=-mod=mod GOTOOLCHAIN=local CGO_ENABLED=1 CC=gcc GOARCH=amd64 \
      go build -buildmode=c-archive -tags netgo,osusergo -trimpath -buildvcs=false -ldflags "-s -w -buildid=" \
      -o "$PFX/lib/libxlite.a" ./main/xlitelib )
    rm -f "$PFX/lib/libxlite.h"
}

build_jpeg() {
    # libjpeg-turbo 3.2.1 (декодер аватарок и превью, src/img.c). Хэш сверен 09.11.2026 по GPG-подписи
    # авторов (.sig релиза, ключ 0338 C8D8 D9FD A62C F9C4 21BD 7EC2 DBB6 F4DB F434).
    # Только libjpeg API, статически: без TurboJPEG API, утилит, тестов, SIMD-ассемблера (картинки
    # крошечные — скорость не нужна, меньше кода под чужими данными) и без арифметического кодирования
    # (Discord его не выдаёт). Код кодировщика остаётся в .a, но выбрасывается --gc-sections.
    fetch libjpeg-turbo-3.2.1.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/libjpeg-turbo-3.2.1.tar.gz \
        c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f
    unpack libjpeg-turbo-3.2.1.tar.gz jpeg
    cmake_static "$BLD/jpeg" "$BLD/jpeg-b" -DCMAKE_INSTALL_LIBDIR=lib \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF \
        -DWITH_SIMD=OFF -DWITH_ARITH_DEC=OFF -DWITH_ARITH_ENC=OFF -DWITH_FUZZ=OFF
    [ -f "$PFX/lib/libjpeg.a" ] && [ -f "$PFX/include/jpeglib.h" ]
}

build_jpegcam() {
    # Вторая копия libjpeg-turbo 3.2.1 — ТОЛЬКО для камеры «Astra DS Video» (разбор MJPG своей веб-камеры, src/cam.c),
    # из того же архива с тем же закреплённым SHA-256, но с SIMD (nasm): MJPG 720p30 без SIMD — ~14 % ядра.
    # Почему не одна копия: камера — локальное доверенное устройство на ПК пользователя, а картинки и аватарки
    # Discord (src/img.c) — чужие данные из сети; для них остаётся копия без SIMD-ассемблера (меньше поверхность атак).
    # Обе копии живут в одном exe: у этой все определённые в ней глобальные символы переименованы (camj_*,
    # objcopy --redefine-syms), заголовки — в prefix/include/jpegcam/, camj.h подставляет новые имена.
    # Только libjpeg API, статически: без TurboJPEG API, утилит, тестов, арифметического кодирования.
    fetch libjpeg-turbo-3.2.1.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/libjpeg-turbo-3.2.1.tar.gz \
        c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f
    # macOS: символы Mach-O с префиксом «_», переименование — llvm-objcopy (brew install llvm); arm64 — NEON без nasm
    local us="" objcopy=objcopy
    if [ "$OS" = mac ]; then
        us=_
        objcopy="$(brew --prefix llvm 2>/dev/null)/bin/llvm-objcopy"
        [ -x "$objcopy" ] || { echo "jpegcam: нужен llvm-objcopy (brew install llvm)"; exit 1; }
    fi
    [ "$OS" = mac ] && [ "$(uname -m)" = arm64 ] || command -v nasm > /dev/null || { echo "jpegcam: нужен nasm (SIMD)"; exit 1; }
    unpack libjpeg-turbo-3.2.1.tar.gz jpegcam
    local inst=$BLD/jpegcam-inst
    rm -rf "$inst" "$BLD/jpegcam-b"
    cmake_static "$BLD/jpegcam" "$BLD/jpegcam-b" -DCMAKE_INSTALL_PREFIX="$inst" -DCMAKE_INSTALL_LIBDIR=lib \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF \
        -DWITH_SIMD=ON -DREQUIRE_SIMD=ON -DWITH_ARITH_DEC=OFF -DWITH_ARITH_ENC=OFF -DWITH_FUZZ=OFF
    [ -f "$inst/lib/libjpeg.a" ] && [ -f "$inst/include/jpeglib.h" ]
    # все глобальные символы, определённые в библиотеке -> camj_<имя>; ссылки на libc и прочее не трогаются
    # (MinGW: .refptr.<имя> — общие COMDAT-указатели; для своих имён тоже переименовываются, иначе компоновщик слил бы
    # указатель второй копии с указателем первой)
    nm -g --defined-only "$inst/lib/libjpeg.a" | awk 'NF >= 3 && $2 ~ /^[A-Z]$/ { print $3 }' | sort -u > "$BLD/jpegcam-all.txt"
    if [ "$OS" = mac ]; then   # имена C в Mach-O — «_имя»: в список идут без префикса
        grep -E '^_[A-Za-z][A-Za-z0-9_]*$' "$BLD/jpegcam-all.txt" | sed 's/^_//' > "$BLD/jpegcam-syms.txt"
    else
        grep -E '^[A-Za-z_][A-Za-z0-9_]*$' "$BLD/jpegcam-all.txt" | grep -vE '^__' > "$BLD/jpegcam-syms.txt"
    fi
    [ -s "$BLD/jpegcam-syms.txt" ]
    {
        awk -v u="$us" '{ print u $1 " " u "camj_" $1 }' "$BLD/jpegcam-syms.txt"
        [ "$OS" = mac ] || awk '{ print ".refptr." $1 " .refptr.camj_" $1 }' "$BLD/jpegcam-syms.txt"
    } > "$BLD/jpegcam-redef.txt"
    "$objcopy" --redefine-syms="$BLD/jpegcam-redef.txt" "$inst/lib/libjpeg.a" "$PFX/lib/libjpegcam.a"
    rm -rf "$PFX/include/jpegcam"; mkdir -p "$PFX/include/jpegcam"
    cp "$inst"/include/{jconfig.h,jerror.h,jmorecfg.h,jpeglib.h} "$PFX/include/jpegcam/"
    {
        echo "/* camj.h — сгенерировано deps/build-deps.sh (build_jpegcam): libjpeg-turbo с SIMD для камеры, имена camj_*. */"
        echo "#ifndef CAMJ_H"
        echo "#define CAMJ_H"
        awk '{ print "#define " $1 " camj_" $1 }' "$BLD/jpegcam-syms.txt"
        echo "#include \"jpeglib.h\""
        echo "#include \"jerror.h\""
        echo "#endif"
    } > "$PFX/include/jpegcam/camj.h"
    # проверка: в переименованной библиотеке не осталось определённого jpeg_* (иначе — конфликт с копией img.c)
    ! nm -g --defined-only "$PFX/lib/libjpegcam.a" | awk 'NF >= 3 { print $3 }' | grep -qE "^${us}(jpeg_|jsimd_|jinit_)"
}

build_brotli() {
    # google/brotli 1.2.1 — только ДЕКОДЕР (c/common + c/dec) для сжатых сертификатов TLS (RFC 8879) в
    # туннеле на C (src/tls13.c): ClientHello как у Chrome объявляет brotli, сервер вправе им сжать.
    # Хэш сверен 09.11.2026 с опубликованным независимо (Homebrew, formula brotli 1.2.1).
    # Собирается напрямую gcc: без CMake, без кодировщика и утилит.
    fetch brotli-1.2.1.tar.gz https://github.com/google/brotli/archive/refs/tags/v1.2.1.tar.gz \
        98d35d11a78dc453aebb7d10c3748f3f73ff78c8267c0dc697b8631f67bf7e8f
    unpack brotli-1.2.1.tar.gz brotli
    local b=$BLD/brotli o=$BLD/brotli-o
    rm -rf "$o"; mkdir -p "$o" "$PFX/include/brotli"
    for f in "$b"/c/common/*.c "$b"/c/dec/*.c; do
        gcc $CFLAGS -std=c99 -I"$b/c/include" -c "$f" -o "$o/$(basename "$(dirname "$f")")_$(basename "${f%.c}").o"
    done
    rm -f "$PFX/lib/libbrotlidec.a"
    ar rcs "$PFX/lib/libbrotlidec.a" "$o"/*.o
    cp "$b"/c/include/brotli/{decode.h,port.h,shared_dictionary.h,types.h} "$PFX/include/brotli/"
}

# ---- «Astra DS Video»: декодеры просмотра чужих трансляций (нужны только сборке VIDEO=1) ----
# dav1d 1.5.4 (AV1, BSD-2). Хэш совпадает с dav1d-1.5.4.tar.xz.sha256 авторов (downloads.videolan.org), 10.11.2026.
# Сборка — meson 1.12.1 из исходников (python meson.py, без установки); хэш архива meson закреплён по архиву
# релиза GitHub, скачанному 10.11.2026. Только библиотека: без утилит, тестов и журнала (-Dlogging=false);
# ассемблер nasm — если есть (скорость декодирования = нагрузка во время просмотра), иначе без него.
# Только 8 бит (-Dbitdepths=8, -0,7 МБ бинаря): 10-битный AV1 у трансляций Discord не встречается — отказ «не поддерживается».
build_dav1d() {
    fetch meson-1.12.1.tar.gz https://github.com/mesonbuild/meson/releases/download/1.12.1/meson-1.12.1.tar.gz \
        ab0a6ca09f8ef70c564c8241fb5a23957886a0b53fb58412b5e07eaf07dba743
    fetch dav1d-1.5.4.tar.xz https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz \
        686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5
    unpack meson-1.12.1.tar.gz meson
    unpack dav1d-1.5.4.tar.xz dav1d
    local py asm=false
    py=$(command -v python3 || command -v python)
    [ "$OS" = win ] && py=$(command -v python)   # python3 в Windows — заглушка магазина
    command -v nasm > /dev/null && asm=true
    [ "$OS" = mac ] && [ "$(uname -m)" = arm64 ] && asm=true   # macOS arm64: NEON собирает clang, nasm не нужен
    rm -rf "$BLD/dav1d-b"
    ( cd "$BLD/dav1d" && "$py" "$BLD/meson/meson.py" setup "$BLD/dav1d-b" --prefix="$PFX" --libdir=lib \
        --buildtype=release -Db_ndebug=true -Ddefault_library=static -Denable_tools=false -Denable_tests=false \
        -Denable_examples=false -Dlogging=false -Denable_asm=$asm -Dbitdepths=8 ) > "$BLD/dav1d-b.configure.txt" 2>&1 \
        || { tail -40 "$BLD/dav1d-b.configure.txt"; exit 1; }
    ninja -C "$BLD/dav1d-b" -j "$JOBS" > "$BLD/dav1d-b.build.txt" 2>&1 || { grep -m20 -E 'error' "$BLD/dav1d-b.build.txt"; exit 1; }
    "$py" "$BLD/meson/meson.py" install -C "$BLD/dav1d-b" > /dev/null
    [ -f "$PFX/lib/libdav1d.a" ] && [ -f "$PFX/include/dav1d/dav1d.h" ]
}

# FFmpeg 8.1.1 (LGPL-2.1+): только libavcodec + libavutil и только декодеры h264, hevc, vp8, vp9 (+ их парсеры).
# Хэш сверен 10.11.2026 по GPG-подписи авторов (ffmpeg-8.1.1.tar.xz.asc, ключ FCF9 86EA 15E6 E293 A564
# 4F10 B432 2F04 D676 58D8 «FFmpeg release signing key»). Без программ, сети, avformat/avfilter/avdevice/
# swscale/swresample, без потоков (декодер однопоточный, свой поток у зрителя), без автоподключения внешних
# библиотек и аппаратных ускорителей. Журнал av_log глушится в коде (src/viddec.c).
build_ffmpeg() {
    fetch ffmpeg-8.1.1.tar.xz https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz \
        b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3
    unpack ffmpeg-8.1.1.tar.xz ffmpeg
    # Патч: таблица зерна плёнки H.274 (676 КБ, static в образе) — в куче по первой нужде. Без патча она всегда
    # в частной памяти процесса (записываемая секция образа), даже без просмотра. Зерно мы не накладываем
    # (src/viddec.c: AV_CODEC_EXPORT_DATA_FILM_GRAIN), так что память не выделяется вовсе. Проверка — grep ниже.
    local h274=$BLD/ffmpeg/libavcodec/h274.c
    sed -i 's|^static H274FilmGrainDatabase film_grain_db;$|static H274FilmGrainDatabase *film_grain_dbp; /* astra-ds: в куче по первой нужде */\n#define film_grain_db (*film_grain_dbp)|' "$h274"
    sed -i '/^int ff_h274_apply_film_grain(/,/^{$/ s|^{$|{\n    if (!film_grain_dbp \&\& !(film_grain_dbp = av_mallocz(sizeof(*film_grain_dbp)))) return AVERROR(ENOMEM);|' "$h274"
    [ "$(grep -c 'film_grain_dbp' "$h274")" = 3 ] || { echo "ffmpeg: патч h274 не применился"; exit 1; }
    local asm=--disable-x86asm extra=""
    command -v nasm > /dev/null && asm=--enable-x86asm
    [ "$OS" != win ] && extra="--enable-pic"
    mkdir -p "$BLD/tmp"                          # TMPDIR Windows (C:\...) configure не понимает
    ( cd "$BLD/ffmpeg" && export TMPDIR="$BLD/tmp" && ./configure --prefix="$PFX" --libdir="$PFX/lib" --incdir="$PFX/include" \
        --enable-static --disable-shared --disable-everything --disable-autodetect --disable-programs --disable-doc \
        --disable-network --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
        --disable-pthreads --disable-w32threads --disable-os2threads --disable-debug --disable-iconv --disable-zlib \
        --disable-bzlib --disable-lzma --enable-decoder=h264,hevc,vp8,vp9 --enable-parser=h264,hevc,vp8,vp9 \
        $asm $extra --extra-cflags="$HARDEN" ) > "$BLD/ffmpeg.configure.txt" 2>&1 || { tail -40 "$BLD/ffmpeg.configure.txt"; exit 1; }
    ( cd "$BLD/ffmpeg" && $MAKE -j "$JOBS" > "$BLD/ffmpeg.build.txt" 2>&1 && $MAKE install > /dev/null 2>&1 ) \
        || { grep -m20 -E 'error|Error' "$BLD/ffmpeg.build.txt"; tail -20 "$BLD/ffmpeg.build.txt"; exit 1; }
    [ -f "$PFX/lib/libavcodec.a" ] && [ -f "$PFX/lib/libavutil.a" ]
}

ALL="zlib openssl curl opus speexdsp rnnoise json mlspp libdave xlite jpeg jpegcam brotli dav1d ffmpeg"
for step in ${*:-$ALL}; do
    echo "== $step"
    "build_$step"
done
echo "== готово: $PFX"
