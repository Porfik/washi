#!/bin/sh
# Сборка статических libmupdf.a + libmupdf-third.a. Запускать в контейнере:
#   scripts/dev.sh scripts/build-mupdf.sh pc|pc-prof|pc-tsan|arm [mutool]
# pc-prof — как pc, но с отладочной информацией для профилировщика (callgrind).
# pc-tsan — с ThreadSanitizer: проверка гонок в самой MuPDF и OpenJPEG.
# Результат: build/mupdf/<цель>/; с mutool — ещё и build/mupdf/<цель>/mutool (анализ PDF).
# Опции взяты из Makerules и include/mupdf/fitz/config.h MuPDF 1.28.6.
set -e

MUPDF_VERSION=1.28.6
SRC=/work/third_party/mupdf-$MUPDF_VERSION-source
TARGET=${1:?нужна цель: pc, pc-prof, pc-tsan или arm}
OUT=/work/build/mupdf/$TARGET
MAKE_TARGETS=libs
[ "$2" = mutool ] && MAKE_TARGETS="libs $OUT/mutool"

[ -d "$SRC" ] || { echo "Нет $SRC: распакуйте mupdf-$MUPDF_VERSION-source.tar.gz в third_party/" >&2; exit 1; }

# Патчи washi к MuPDF (patches/mupdf/*.patch) накладываются один раз;
# наложенные перечислены в $SRC/.washi-patches.
STAMP=$SRC/.washi-patches
touch "$STAMP"
for p in /work/patches/mupdf/*.patch; do
    [ -e "$p" ] || continue
    name=$(basename "$p")
    grep -qxF "$name" "$STAMP" && continue
    echo "Патч $name"
    patch -d "$SRC" -p1 --forward < "$p"
    echo "$name" >> "$STAMP"
done

# Без JS, XPS, вывода DOCX (extract), шрифтов Noto и CJK. OCR и штрихкоды
# выключены по умолчанию. HTML-движок включён: он нужен для EPUB/FB2/HTML — хаб
# открывает их своей читалкой (вёрстка fz_layout_document; harfbuzz из thirdparty).
# SVG включён: в EPUB (Calibre, Project Gutenberg, многие магазины) обложка — картинка внутри
# <svg>; без SVG вместо неё серая заглушка «[image]».
# Кириллица остаётся: для неё MuPDF использует base14-шрифты URW, а не Noto.
FEATURES="mujs=no html=yes xps=no svg=yes extract=no tofu=yes tofu_cjk=yes"
FEATURE_CFLAGS="-DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 -DFZ_ENABLE_DOCX_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0 -DFZ_ENABLE_OCR_OUTPUT=0"
NO_SYSTEM="HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_LIBCRYPTO=no USE_SYSTEM_LIBS=no"
# OpenJPEG с потоками (в MuPDF по умолчанию MUTEX_pthread=0): JPX распаковывается на двух ядрах,
# число потоков задаёт патч 0002 (fz_washi_jpx_threads).
OPENJPEG_THREADS="-Ithirdparty/openjpeg/src/lib/openjp2 -DMUTEX_pthread=1"

case "$TARGET" in
pc)
    TOOLCHAIN=
    ARCH_CFLAGS=
    ;;
pc-prof)
    TOOLCHAIN=
    ARCH_CFLAGS="-g -fno-omit-frame-pointer"
    ;;
pc-tsan)
    TOOLCHAIN=
    ARCH_CFLAGS="-fsanitize=thread -g -O1"
    ;;
arm)
    T=arm-obreey-linux-gnueabi
    # Шрифты встраиваются C-массивами из generated/, а не через ld -r -b binary.
    TOOLCHAIN="CC=$T-gcc CXX=$T-g++ AR=$T-ar LD=$T-ld RANLIB=$T-ranlib HAVE_OBJCOPY=no OS=Linux"
    # Те же флаги, что в cmake/pb740.cmake.
    ARCH_CFLAGS="-march=armv7ve -mcpu=cortex-a7 -mfpu=neon -mfloat-abi=softfp"
    ;;
*)
    echo "Неизвестная цель: $TARGET" >&2
    exit 1
    ;;
esac

# make не следит за флагами: если флаги или патчи изменились — сборка с нуля.
FLAGS_ID="$FEATURES|$FEATURE_CFLAGS|$NO_SYSTEM|$TOOLCHAIN|$ARCH_CFLAGS|$OPENJPEG_THREADS|$(cat "$STAMP")"
if [ -d "$OUT" ] && [ "$(cat "$OUT/.flags" 2>/dev/null)" != "$FLAGS_ID" ]; then
    echo "Флаги или патчи изменились — пересобираю $OUT с нуля"
    rm -rf "$OUT"
fi
mkdir -p "$OUT"
printf '%s' "$FLAGS_ID" > "$OUT/.flags"

# shellcheck disable=SC2086
make -C "$SRC" -j"$(nproc)" $MAKE_TARGETS build=release OUT="$OUT" \
    $FEATURES $NO_SYSTEM $TOOLCHAIN OPENJPEG_BUILD_CFLAGS="$OPENJPEG_THREADS" \
    XCFLAGS="$ARCH_CFLAGS $FEATURE_CFLAGS"

ls -l "$OUT"/*.a
