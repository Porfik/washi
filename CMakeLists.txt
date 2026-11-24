cmake_minimum_required(VERSION 3.16)
project(washi VERSION 0.1.0 LANGUAGES C)
# Версия выпуска: в «Настройках» хаба и в логе. Пока бета.
set(WASHI_VERSION "${PROJECT_VERSION}-beta")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# По плану -O2, CMake по умолчанию ставит -O3.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_options(-Wall -Wextra)

# MuPDF: статические библиотеки из scripts/build-mupdf.sh.
set(MUPDF_VERSION 1.28.6)
set(MUPDF_SRC ${CMAKE_SOURCE_DIR}/third_party/mupdf-${MUPDF_VERSION}-source)
if(PB740)
    set(MUPDF_BUILD_DEFAULT ${CMAKE_SOURCE_DIR}/build/mupdf/arm)
else()
    set(MUPDF_BUILD_DEFAULT ${CMAKE_SOURCE_DIR}/build/mupdf/pc)
endif()
# Для профилирования: -DMUPDF_BUILD=.../build/mupdf/pc-prof
set(MUPDF_BUILD ${MUPDF_BUILD_DEFAULT} CACHE PATH "Папка со статическими библиотеками MuPDF")
if(NOT EXISTS ${MUPDF_BUILD}/libmupdf.a)
    message(FATAL_ERROR "Нет ${MUPDF_BUILD}/libmupdf.a: сначала scripts/dev.sh scripts/build-mupdf.sh pc|arm")
endif()
add_library(mupdf INTERFACE)
target_include_directories(mupdf SYSTEM INTERFACE ${MUPDF_SRC}/include)
target_link_libraries(mupdf INTERFACE ${MUPDF_BUILD}/libmupdf.a ${MUPDF_BUILD}/libmupdf-third.a m)

find_package(Threads REQUIRED)

# Основа без зависимостей: настройки, позиции, закладки, журнал, файловые мелочи.
add_library(washi_base STATIC
    src/base/bookmarks.c
    src/base/config.c
    src/base/dirpath.c
    src/base/log.c
    src/base/positions.c
    src/base/sysinfo.c
    src/base/util.c
)
target_compile_definitions(washi_base PUBLIC _POSIX_C_SOURCE=200809L WASHI_VERSION="${WASHI_VERSION}")
target_include_directories(washi_base PUBLIC src)

# Всё остальное, что не зависит от платформы.
add_library(washi_core STATIC
    src/base/bookfiles.c
    src/base/cred.c
    src/base/secret.c
    src/bench/bench.c
    src/bench/prof.c
    src/cache/cache.c
    src/doc/covers.c
    src/doc/doc.c
    src/doc/margins.c
    src/doc/reflow_check.c
    src/net/ftpd.c
    src/net/httpd.c
    src/net/net.c
    src/net/opds_feed.c
    src/net/remote.c
    src/net/syncjob.c
    src/net/syncnotes.c
    src/net/webdav.c
    src/net/xfer.c
    src/platform/rotate.c
    src/render/render.c
    src/ui/app.c
    src/ui/apps.c
    src/ui/bookcover.c
    src/ui/bookinfo.c
    src/ui/filedlg.c
    src/ui/folderpick.c
    src/ui/gesture.c
    src/ui/gfx.c
    src/ui/home.c
    src/ui/icons_data.c
    src/ui/library.c
    src/ui/opds.c
    src/ui/quick.c
    src/ui/reader.c
    src/ui/recv.c
    src/ui/settings.c
    src/ui/sync.c
    src/ui/text.c
    src/ui/widgets.c
    src/vendor/qrcodegen.c
)
# Страница приёма (src/net/web/index.html) встраивается в httpd.c массивом байт: web_index.h в build-папке.
# Перегенерируется при конфигурации (файл — зависимость конфигурации), пишется только если изменился.
file(READ src/net/web/index.html WEB_INDEX_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," WEB_INDEX_HEX "${WEB_INDEX_HEX}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n" WEB_INDEX_HEX "${WEB_INDEX_HEX}")
set(WEB_INDEX_NEW "/* Сгенерировано из src/net/web/index.html — не править. */\nstatic const unsigned char web_index_html[] = {\n${WEB_INDEX_HEX}0x00 };\n")
set(WEB_INDEX_FILE ${CMAKE_BINARY_DIR}/gen/web_index.h)
set(WEB_INDEX_OLD "")
if(EXISTS ${WEB_INDEX_FILE})
    file(READ ${WEB_INDEX_FILE} WEB_INDEX_OLD)
endif()
if(NOT WEB_INDEX_NEW STREQUAL WEB_INDEX_OLD)
    file(WRITE ${WEB_INDEX_FILE} "${WEB_INDEX_NEW}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/net/web/index.html)
target_include_directories(washi_core PRIVATE ${CMAKE_BINARY_DIR}/gen)

# libcurl — через dlopen (src/net/net.c): заголовки из системы или SDK, библиотека — на месте.
target_link_libraries(washi_core PUBLIC mupdf washi_base Threads::Threads ${CMAKE_DL_LIBS})

if(PB740)
    find_path(INKVIEW_INCLUDE_DIR inkview.h REQUIRED)
    find_library(INKVIEW_LIB inkview REQUIRED)
    add_library(inkview_sdk INTERFACE)
    target_include_directories(inkview_sdk INTERFACE ${INKVIEW_INCLUDE_DIR})
    # inkview.h тянет sys/ipc.h, которому нужен _GNU_SOURCE.
    target_compile_definitions(inkview_sdk INTERFACE _GNU_SOURCE)
    target_link_libraries(inkview_sdk INTERFACE ${INKVIEW_LIB} ${CMAKE_DL_LIBS})
    set(PLATFORM_SOURCES
        src/platform/inkview.c
        # glibc 2.23 на книге без getentropy(), нужной MuPDF. Исходником в каждый
        # исполняемый файл, а не в washi_core: из статической библиотеки объект
        # не подтянулся бы, ссылка на него появляется позже, в libmupdf.a.
        src/compat/getentropy.c
    )
    set(PLATFORM_LIBS inkview_sdk)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(PLATFORM_SOURCES src/platform/sdl.c)
    set(PLATFORM_LIBS PkgConfig::SDL2)
endif()

foreach(app washi washi-bench)
    if(app STREQUAL washi)
        add_executable(${app} src/main.c ${PLATFORM_SOURCES})
    else()
        add_executable(${app} src/bench/bench_app.c ${PLATFORM_SOURCES})
    endif()
    target_link_libraries(${app} washi_core ${PLATFORM_LIBS})
    target_link_options(${app} PRIVATE -Wl,--gc-sections)
    if(PB740)
        set_target_properties(${app} PROPERTIES SUFFIX ".app")
    endif()
endforeach()

# Тесты — только на ПК.
if(NOT PB740)
    enable_testing()
    add_subdirectory(tests)
endif()
