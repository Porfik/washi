/* washi: хаб и читалка. На ПК также washi --bench FILE [PAGES] [опции] (без окна). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/config.h"
#include "base/log.h"
#include "bench/bench.h"
#include "platform/platform.h"
#include "ui/app.h"
#include "ui/home.h"

/* Хаб вместо штатного главного экрана: прошивка запускает /mnt/ext1/system/bin/bookshelf.app,
 * если он есть, вместо STOCK_HOME (docs/device.md). */
#define STOCK_HOME "/ebrmain/bin/bookshelf.app"
#define HOME_MAX_FAILS 3

static int is_home_screen(const char *argv0)
{
    const char *base = strrchr(argv0, '/');
    return !strcmp(base ? base + 1 : argv0, "bookshelf.app");
}

/* Заменить себя штатным главным экраном (тот же процесс — монитор прошивки считает его
 * главным экраном). Возвращается, только если запустить не вышло. */
static void run_stock_home(char **argv, const char *log_path)
{
    log_close();
    argv[0] = (char *)STOCK_HOME;
    execv(STOCK_HOME, argv);
    log_open(log_path);
    log_msg("не запустился %s — остаюсь", STOCK_HOME);
}

/* Защита от падений на старте: в файле — сколько запусков подряд не дошли до первого экрана
 * (хаб обнуляет счётчик, показав экран). HOME_MAX_FAILS подряд — вместо хаба запускается
 * штатный главный экран; чтобы снова пробовать хаб, удалить этот файл. */
static void home_failsafe(char **argv, const char *fails_path, const char *log_path)
{
    int fails = 0;
    FILE *f = fopen(fails_path, "r");
    if (f) {
        if (fscanf(f, "%d", &fails) != 1)
            fails = 0;
        fclose(f);
    }
    if (fails >= HOME_MAX_FAILS) {
        log_msg("главный экран: хаб %d раз подряд не дошёл до экрана — запускаю штатный %s", fails, STOCK_HOME);
        run_stock_home(argv, log_path);
        return;
    }
    f = fopen(fails_path, "w");
    if (f) {
        fprintf(f, "%d\n", fails + 1);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--bench")) {
        /* Построчно: вывод может идти в общий лог вместе с stderr (washi-bench, bin=). */
        setvbuf(stdout, NULL, _IOLBF, 0);
        return bench_main(argc - 2, argv + 2, stdout);
    }

    const char *dir = pf_data_dir();
    char path[1024];
    mkdir(dir, 0755);
    /* Не .txt: индексатор прошивки считает .txt книгами. */
    char log_path[1024], fails_path[1024];
    snprintf(log_path, sizeof log_path, "%s/washi.log", dir);
    log_open(log_path);
    int home = is_home_screen(argv[0]);
    char stock_once[1024];
    snprintf(fails_path, sizeof fails_path, "%s/home-fails", dir);
    snprintf(stock_once, sizeof stock_once, "%s/stock-once", dir);
    if (home) {
        char args[512] = "";
        for (int i = 1; i < argc; i++)
            snprintf(args + strlen(args), sizeof args - strlen(args), " «%s»", argv[i]);
        log_msg("главный экран: %s%s", argv[0], args);
        if (remove(stock_once) == 0) {
            /* Просьба из «Настройки → Штатный главный экран → До перезагрузки». */
            log_msg("главный экран: по просьбе — штатный %s", STOCK_HOME);
            run_stock_home(argv, log_path);
        }
        home_failsafe(argv, fails_path, log_path);
    }

    config cfg;
    snprintf(path, sizeof path, "%s/config.txt", dir);
    int have_config = config_load(&cfg, path) == 0;

    reader_options opt;
    memset(&opt, 0, sizeof opt);
    opt.path = argc >= 2 ? argv[1] : NULL;
    struct stat st;
    if (home && opt.path && (stat(opt.path, &st) != 0 || !S_ISREG(st.st_mode)))
        opt.path = NULL; /* главному экрану прошивка может передать что-то своё — не книгу */
    opt.full_refresh_every = config_get_int(&cfg, "full_refresh", 6);
    opt.cache_bytes = (size_t)config_get_int(&cfg, "cache_mb", 64) << 20;
    opt.prefetch_ahead = config_get_int(&cfg, "prefetch_ahead", 2);
    opt.prefetch_behind = config_get_int(&cfg, "prefetch_behind", 1);
    opt.render_threads = config_get_int(&cfg, "render_threads", 2);
    opt.prefetch_while_waiting = config_get_int(&cfg, "prefetch_while_waiting", 0);
    opt.screen_busy_ms = config_get_int(&cfg, "screen_busy_ms", 470);
    opt.show_hint = !config_get_int(&cfg, "hint_shown", 0);
    opt.reflow_em = config_get_int(&cfg, "reflow_em", 28);
    opt.doc.store_bytes = (size_t)config_get_int(&cfg, "store_mb", 64) << 20;
    opt.doc.aa_level = config_get_int(&cfg, "aa", -1);
    opt.doc.no_icc = config_get_int(&cfg, "no_icc", 1);
    opt.doc.jpx_no_reduce = !config_get_int(&cfg, "jpx_reduce", 1);
    opt.doc.jpx_threads = config_get_int(&cfg, "jpx_threads", 2);
    opt.doc.mupdf_upscale = !config_get_int(&cfg, "upscale_scaler", 1);
    opt.doc.jpx_min_scale = config_get_int(&cfg, "jpx_min_scale", 100);
    opt.doc.rgb_jpeg = !config_get_int(&cfg, "jpeg_gray", 1);
    const char *env_root = getenv("WASHI_LIBRARY");
    const char *root = config_get(&cfg, "library_root", env_root ? env_root : "/mnt/ext1");
    log_msg("washi %s, сборка %s %s; config %s: library_root=%s full_refresh=%d cache_mb=%zu prefetch=+%d/-%d "
            "render_threads=%d prefetch_while_waiting=%d screen_busy_ms=%d store_mb=%zu aa=%d no_icc=%d "
            "jpx_reduce=%d jpx_threads=%d jpx_min_scale=%d upscale_scaler=%d jpeg_gray=%d",
            WASHI_VERSION, __DATE__, __TIME__, have_config ? "есть" : "нет", root,
            opt.full_refresh_every, opt.cache_bytes >> 20, opt.prefetch_ahead, opt.prefetch_behind,
            opt.render_threads, opt.prefetch_while_waiting, opt.screen_busy_ms, opt.doc.store_bytes >> 20,
            opt.doc.aa_level, opt.doc.no_icc, !opt.doc.jpx_no_reduce, opt.doc.jpx_threads,
            opt.doc.jpx_min_scale, !opt.doc.mupdf_upscale, !opt.doc.rgb_jpeg);

    home_set_title_strip(config_get(&cfg, "title_strip", ""));
    app_options ao = { opt, root, home ? fails_path : NULL, home ? argv[0] : NULL, stock_once,
                       config_get(&cfg, "download_dir", NULL) };
    app *a = app_new(&ao);
    if (!a)
        return 1;
    pf_run(app_on_event, a);
    app_free(a);
    log_close();
    return 0;
}
