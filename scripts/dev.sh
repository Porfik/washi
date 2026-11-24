#!/bin/sh
# Запуск команды в контейнере washi-dev с проектом в /work.
# Файлы создаются от имени текущего пользователя, а не root.
#   scripts/dev.sh                 — интерактивная оболочка
#   scripts/dev.sh make -C build   — одна команда
set -e

IMAGE=washi-dev
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -t "$IMAGE" "$ROOT/docker"
fi

TTY=
[ -t 0 ] && [ -t 1 ] && TTY=-it

[ $# -eq 0 ] && set -- bash

exec docker run --rm $TTY \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$ROOT:/work" \
    -w /work \
    "$IMAGE" "$@"
