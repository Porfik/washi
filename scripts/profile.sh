#!/bin/sh
# Профиль отрисовки страниц (callgrind) по функциям. Запускать в контейнере:
#   scripts/dev.sh scripts/profile.sh FILE PAGES [строк] [опции bench]
# Нужны build/mupdf/pc-prof (scripts/build-mupdf.sh pc-prof) и build/pc-prof:
#   cmake -G Ninja -S . -B build/pc-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer" -DMUPDF_BUILD=/work/build/mupdf/pc-prof
set -e

FILE=${1:?нужен PDF}
PAGES=${2:?нужны страницы}
LINES=${3:-25}
shift 3 2>/dev/null || shift $#

OUT=/work/build/prof/$(basename "$FILE" .pdf)-$(echo "$PAGES" | tr ',' '_').out
mkdir -p /work/build/prof
valgrind --tool=callgrind --callgrind-out-file="$OUT" \
    /work/build/pc-prof/washi --bench "$FILE" "$PAGES" "$@" >/dev/null 2>&1
echo "== $FILE $PAGES $*: собственное время функций"
callgrind_annotate --inclusive=no "$OUT" 2>/dev/null |
    sed -n '/^Ir  *file:function/,$p' | sed -n "3,$((LINES + 2))p" |
    sed -E 's|/work/third_party/mupdf-[^/]*/||; s|/usr/include/[^:]*:||; s| \[[^]]*\]$||' | cut -c1-150
