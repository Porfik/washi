# Тесты (ctest) для ПК.
# Несколько тестов включают проверяемый .c как исходник — им нужны внутренние функции.

# ob_test(<имя ctest> <исходник> [LIBS библиотеки...] [ARGS аргументы запуска...])
function(ob_test name src)
    cmake_parse_arguments(T "" "" "LIBS;ARGS" ${ARGN})
    add_executable(${name}_test ${src})
    target_link_libraries(${name}_test PRIVATE ${T_LIBS} washi_base)
    add_test(NAME ${name} COMMAND ${name}_test ${T_ARGS})
endfunction()

ob_test(reflow_check doc/reflow_check_test.c LIBS washi_core)
ob_test(covers doc/covers_test.c LIBS washi_core)
ob_test(net net/net_test.c LIBS washi_core)
ob_test(bookfiles base/bookfiles_test.c LIBS washi_core)
ob_test(secret base/secret_test.c LIBS washi_core)
ob_test(cred base/cred_test.c LIBS washi_core)
ob_test(dirpath base/dirpath_test.c)
ob_test(title ui/title_test.c LIBS washi_core)
ob_test(recv net/recv_test.c LIBS washi_core)
ob_test(sync net/sync_test.c LIBS washi_core)
ob_test(opds net/opds_test.c LIBS washi_core ARGS ${CMAKE_SOURCE_DIR}/tests/net/opds)

# Синхронизация без интерфейса — для проверки против rclone serve webdav / ftp (не ctest).
add_executable(sync_run net/sync_run.c)
target_link_libraries(sync_run PRIVATE washi_core washi_base)
