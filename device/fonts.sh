#!/bin/sh
# Копия шрифтов интерфейса из прошивки для разработки хаба на ПК (только чтение прошивки).
OUT=/mnt/ext1/washi/probe/fonts
mkdir -p "$OUT"
for f in /ebrmain/fonts/Roboto-*.ttf /ebrmain/fonts/PTSans*.ttf /ebrmain/fonts/PTSerif*.ttf; do
    [ -e "$f" ] && cp "$(readlink -f "$f")" "$OUT/$(basename "$f")" && echo "$f"
done
du -s "$OUT"
