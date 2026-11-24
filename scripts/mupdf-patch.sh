#!/bin/sh
# Новый патч MuPDF из правок в third_party/. Запускать в контейнере:
#   scripts/dev.sh scripts/mupdf-patch.sh ИМЯ ФАЙЛ...
# ИМЯ — например 0016-что-делает; ФАЙЛ — путь внутри исходников MuPDF (source/fitz/draw-device.c).
# Патч — разница между чистой копией build/mupdf-pristine (архив + все прошлые патчи) и
# third_party/. Он проверяется наложением на чистую копию; после этого и копия, и отметка
# third_party/.../.washi-patches считают его наложенным. Чистой копии нет — она создаётся.
set -e

V=1.28.6
SRC=/work/third_party/mupdf-$V-source
PRISTINE=${PRISTINE:-/work/build/mupdf-pristine}
NAME=${1:?нужно имя патча, например 0016-что-делает}
shift
[ $# -gt 0 ] || { echo "Нужны изменённые файлы" >&2; exit 1; }
OUT=/work/patches/mupdf/$NAME.patch
[ ! -e "$OUT" ] || { echo "Уже есть $OUT" >&2; exit 1; }

if [ ! -d "$PRISTINE" ]; then
    echo "Чистая копия: архив + patches/mupdf/*.patch → $PRISTINE"
    tmp=$(mktemp -d /work/build/pristine.XXXXXX)
    tar -xzf /work/third_party/mupdf-$V-source.tar.gz -C "$tmp"
    for p in /work/patches/mupdf/*.patch; do
        patch -s -d "$tmp/mupdf-$V-source" -p1 --forward < "$p"
    done
    mv "$tmp/mupdf-$V-source" "$PRISTINE"
    rmdir "$tmp"
fi

TMP=$OUT.tmp
: > "$TMP"
for f in "$@"; do
    # -N: новый файл (нет в чистой копии) — разница с пустым.
    diff -uN --label "a/$f" --label "b/$f" "$PRISTINE/$f" "$SRC/$f" >> "$TMP" || [ $? -eq 1 ]
done
[ -s "$TMP" ] || { rm -f "$TMP"; echo "Изменений нет" >&2; exit 1; }
patch -s --dry-run -d "$PRISTINE" -p1 < "$TMP"
patch -s -d "$PRISTINE" -p1 < "$TMP"
mv "$TMP" "$OUT"
echo "$NAME.patch" >> "$SRC/.washi-patches"
echo "Готово: $OUT ($(grep -c '^+++ ' "$OUT") файлов)"
