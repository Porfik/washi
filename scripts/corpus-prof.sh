#!/bin/sh
# Профиль callgrind по всему набору testdata/: 11 страниц, равномерно по каждой книге (в контейнере).
# $1 — каталог результата, $2 — бинарь (по умолчанию build/pc-prof/washi), далее — опции бенча.
# Сводка: python3 -I scripts/cg-sum.py $1/*.ann
OUT=${1:?каталог}; shift
BIN=${1:-build/pc-prof/washi}; [ $# -gt 0 ] && shift
mkdir -p "$OUT"
for f in testdata/*.pdf; do
    name=$(basename "$f" .pdf)
    n=$($BIN --bench "$f" 1 --no-icc 2>/dev/null | sed -n 's/.*pages: \([0-9]*\).*/\1/p')
    pages=$(awk -v n="$n" 'BEGIN { s=""; for (k = 1; k <= 11; k++) { p = int(n * k / 12); if (p < 1) p = 1; s = s (k > 1 ? "," : "") p } print s }')
    valgrind --tool=callgrind --callgrind-out-file="$OUT/$name.cg" $BIN --bench "$f" "$pages" --no-icc --jpx-min 70 "$@" > "$OUT/$name.txt" 2>&1
    callgrind_annotate --inclusive=no "$OUT/$name.cg" > "$OUT/$name.ann" 2>/dev/null
    echo "$name: $n стр., $pages"
done
