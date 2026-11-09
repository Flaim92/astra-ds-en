#!/usr/bin/env bash
# Стенд безопасности: сборка и прогон фаззера декодера картинок (img.c + libjpeg-turbo).
#   bash tools/sec/run_fuzz.sh [итераций] [кол-во сгенерированных JPEG]
# Всё — в build-sec/ (свой каталог); deps/ только читается.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build-sec
PFX=deps/prefix
ITERS=${1:-200000}
NGEN=${2:-400}
mkdir -p "$OUT/corpus" "$OUT/crash"
CF="-std=gnu11 -O1 -g -DCURL_STATICLIB -DCJSON_NESTING_LIMIT=64 -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -I$PFX/include -Isrc -Ithird_party"
gcc $CF -w tools/sec/gen_jpeg.c -o "$OUT/gen_jpeg.exe" -L$PFX/lib -ljpeg
gcc $CF -Wall -Wno-unused-function tools/sec/fuzz_img.c -o "$OUT/fuzz_img.exe" -L$PFX/lib -ljpeg -lcurl -lz -lbcrypt -ladvapi32 -lcrypt32 -lsecur32 -lws2_32 -liphlpapi
"$OUT/gen_jpeg.exe" "$OUT/corpus" "$NGEN"
cp -f deps/build/jpeg/testimages/*.jpg "$OUT/corpus/" 2>/dev/null || true
"$OUT/fuzz_img.exe" "$ITERS" "$OUT/crash" "$OUT/corpus"
