# Образ для разработки washi: сборка под PC (SDL2) и кросс-сборка под PocketBook B288.
# Всё окружение живёт здесь, на хост ничего не ставится.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        curl xz-utils file python3 libsdl2-dev \
    && rm -rf /var/lib/apt/lists/*

# SDK PocketBook, ветка 6.5 (прошивка 6.5). Коммит зафиксирован, чтобы образ был воспроизводимым.
# .git удаляется, update_path.sh пишет абсолютные пути только внутрь SDK.
ARG SDK_COMMIT=dd8de463733a2bc0d53d1214b64734586fac427e
RUN git clone --depth 1 --branch 6.5 https://github.com/pocketbook/SDK_6.3.0.git /opt/pb-sdk \
    && test "$(git -C /opt/pb-sdk rev-parse HEAD)" = "$SDK_COMMIT" \
    && rm -rf /opt/pb-sdk/.git \
    && /opt/pb-sdk/SDK-B288/bin/update_path.sh

# Бинарники gcc из SDK ищут свои libmpfr/libgmp/libmpc по RUNPATH сборочного сервера PocketBook,
# а сами библиотеки лежат в SDK-B288/usr/lib. Ссылка восстанавливает этот путь.
# clang из SDK (запасной компилятор) требует libtinfo.so.5.
RUN mkdir -p /srv/jenkins/workspace/SDK-GEN/output-B288 \
    && ln -s /opt/pb-sdk/SDK-B288 /srv/jenkins/workspace/SDK-GEN/output-B288/host \
    && apt-get update && apt-get install -y --no-install-recommends libtinfo5 \
    && rm -rf /var/lib/apt/lists/*

# Профилирование на ПК: valgrind --tool=callgrind.
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*

# Сеть: заголовки libcurl для сборки на ПК (сама библиотека берётся через dlopen, на книге —
# из прошивки), rclone — тестовые серверы WebDAV и FTP, с которыми проверяются приём и синхронизация.
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4-openssl-dev rclone \
    && rm -rf /var/lib/apt/lists/*

ENV PB_SDK=/opt/pb-sdk/SDK-B288
ENV PB_SYSROOT=/opt/pb-sdk/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot
# В конец: в SDK есть свой pkg-config (для ARM sysroot) и утилиты glib/ncurses,
# они не должны перекрывать системные. Кросс-инструменты с префиксом найдутся и так.
ENV PATH=$PATH:/opt/pb-sdk/SDK-B288/usr/bin

WORKDIR /work
