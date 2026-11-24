#!/bin/sh
# Отрисовка набора страниц в PGM для побайтного сравнения сборок. Запускать в контейнере:
#   scripts/dev.sh scripts/render-set.sh BIN OUTDIR [опции bench]
# Набор — строки «книга страницы» (testdata/<книга>.pdf) со стандартного ввода, обычно scripts/render-set.txt.
set -e
BIN=${1:?нужен бинарник washi}
OUT=${2:?нужна папка}
shift 2
while read -r book pages; do
    [ -n "$book" ] || continue
    "$BIN" --bench "testdata/$book.pdf" "$pages" --pgm "$OUT/$book" "$@" |
        awk -v b="$book" '/^summary/ {print b ": " $0}'
done
