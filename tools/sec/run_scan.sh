#!/usr/bin/env bash
# Стенд: фаззер самописного сканера JSON из src/store.c. Код сканера вырезается из store.c по маркерам
# (всегда актуальная версия), собирается вместе с tools/sec/fuzz_scan.c в build-sec/.
#   bash tools/sec/run_scan.sh [итераций]
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build-sec
mkdir -p "$OUT/crash"
{
  echo '/* вырезано из src/store.c скриптом tools/sec/run_scan.sh — не править */'
  awk '/^typedef struct Sch \{/{on=1} /^static cJSON \*parse_span/{on=0} on' src/store.c
  awk '/^static const char \*const WANTED\[\]/{on=1} on{print} on && /^bool event_wanted/{ev=1} ev && /^}/{exit}' src/store.c
} > "$OUT/scan_extract.c"
PFX=deps/prefix
gcc -std=gnu11 -O1 -g -w -DCJSON_NESTING_LIMIT=64 -D_WIN32_WINNT=0x0A00 -I$PFX/include -Isrc -Ithird_party \
    tools/sec/fuzz_scan.c third_party/cJSON.c -o "$OUT/fuzz_scan.exe"
"$OUT/fuzz_scan.exe" "${1:-200000}"
