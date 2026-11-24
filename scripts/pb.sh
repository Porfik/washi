#!/bin/sh
# Работа с книгой, подключённой к ПК как накопитель (том FAT32 с меткой PB740;
# другая модель — PB_LABEL=PB743G scripts/pb.sh ...).
#   scripts/pb.sh deploy <файл.app>...  — скопировать в applications/ и sync
#   scripts/pb.sh put <файл>...         — скопировать в washi/ (папка приложения) и sync
#   scripts/pb.sh putbin ИМЯ=<файл>...  — скопировать в washi/bin/washi-ИМЯ (варианты для bench)
#   scripts/pb.sh eject                 — sync, размонтировать, обесточить
#   scripts/pb.sh log [имя]             — показать /mnt/ext1/washi/<имя> (по умолчанию washi.log)
# На книге пишем только в applications/ и washi/. Ничего не удаляем.
set -e

LABEL=${PB_LABEL:-PB740}

mountpoint() {
    findmnt -n -o TARGET -S "LABEL=$LABEL" || { echo "Книга ($LABEL) не смонтирована" >&2; exit 1; }
}

case "$1" in
deploy)
    shift
    [ $# -gt 0 ] || { echo "Нужен хотя бы один файл" >&2; exit 1; }
    mnt=$(mountpoint)
    [ -d "$mnt/applications" ] || { echo "Нет $mnt/applications" >&2; exit 1; }
    for f in "$@"; do
        case "$f" in *.app) ;; *) echo "Не .app: $f" >&2; exit 1 ;; esac
        cp -v "$f" "$mnt/applications/"
    done
    sync
    echo "Скопировано и записано на диск."
    ;;
put)
    shift
    [ $# -gt 0 ] || { echo "Нужен хотя бы один файл" >&2; exit 1; }
    mnt=$(mountpoint)
    mkdir -p "$mnt/washi"
    for f in "$@"; do
        cp -v "$f" "$mnt/washi/"
    done
    sync
    echo "Скопировано и записано на диск."
    ;;
putbin)
    shift
    [ $# -gt 0 ] || { echo "Нужен хотя бы один ИМЯ=файл" >&2; exit 1; }
    mnt=$(mountpoint)
    mkdir -p "$mnt/washi/bin"
    for a in "$@"; do
        name=${a%%=*}
        file=${a#*=}
        [ "$name" != "$a" ] && [ -f "$file" ] || { echo "Нужно ИМЯ=файл: $a" >&2; exit 1; }
        cp -v "$file" "$mnt/washi/bin/washi-$name"
    done
    sync
    echo "Скопировано и записано на диск."
    ;;
eject)
    dev=$(findmnt -n -o SOURCE -S "LABEL=$LABEL") || { echo "Книга ($LABEL) не смонтирована" >&2; exit 1; }
    # Том может быть разделом (sda1) или всем диском (sda, без таблицы разделов).
    disk=$(lsblk -no PKNAME "$dev" | head -1)
    disk=${disk:+/dev/$disk}
    disk=${disk:-$dev}
    sync
    udisksctl unmount -b "$dev"
    udisksctl power-off -b "$disk"
    echo "Книгу можно отключать."
    ;;
log)
    mnt=$(mountpoint)
    cat "$mnt/washi/${2:-washi.log}"
    ;;
*)
    sed -n '2,8p' "$0"
    exit 1
    ;;
esac
