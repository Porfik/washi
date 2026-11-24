/* washi-bench: замер на книге без командной строки.
 *
 * Список прогонов — <папка данных>/bench.txt, по строке на прогон:
 *     СТРАНИЦЫ [опции bench] [bin=ИМЯ] | /mnt/ext1/путь/к/файлу.pdf
 * С bin=ИМЯ прогон идёт отдельным процессом <папка данных>/bin/washi-ИМЯ --bench …
 * (другая сборка — например, MuPDF с другими флагами компилятора).
 * Строка «shell | путь.sh» — выполнить скрипт /bin/sh (сведения о системе), вывод — в лог.
 * Строка «screen СТР1,СТР2 | путь» — тест экрана: две страницы показываются по очереди во всех
 * режимах обновления e-ink (время вызова и до конца обновления — в лог, название режима —
 * внизу экрана), чтобы подобрать режим на глаз и по времени.
 * Пустые строки и строки с # пропускаются. Результат дописывается в bench-log.txt.
 * Замер идёт в отдельном потоке; поток интерфейса раз в секунду обновляет экран
 * и не даёт книге уснуть.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../base/clock.h"
#include "../base/util.h"
#include "../doc/doc.h"
#include "../platform/platform.h"
#include "../ui/text.h"
#include "bench.h"

#define MAX_RUNS 64
#define MAX_ARGS 32
#define TICK_MS 1000
#define KEEP_AWAKE_EVERY_TICKS 30
#define KEEP_AWAKE_SEC 120

typedef struct {
    char spec[256];
    char path[768];
} bench_run;

static struct {
    bench_run runs[MAX_RUNS];
    int count;
    atomic_int current;
    atomic_int done;
    FILE *out;
    pthread_t thread;
    int thread_started;
    ui_text *text;
    double t_start;
    double t_end;
    int shown_current;
    int shown_done;
    int ticks;
    char error[1280];
    int sw, sh, sstride; /* экран — для теста экрана в потоке замера */
} app;

/* ---- Тест экрана ---- */

#define SCREEN_HOLD_MS 2500

static const pf_update_mode screen_modes[] = {
    PF_UPDATE_FULL,         PF_UPDATE_FULL_HQ,      PF_UPDATE_SOFT,       PF_UPDATE_SOFT_HQ,
    PF_UPDATE_PARTIAL,      PF_UPDATE_PARTIAL_HQ,   PF_UPDATE_PARTIAL_BW, PF_UPDATE_PARTIAL_BLACK,
    PF_UPDATE_PARTIAL_DU4,  PF_UPDATE_FAST,         PF_UPDATE_FAST_BW,    PF_UPDATE_A2,
};
#define SCREEN_MODES (int)(sizeof screen_modes / sizeof *screen_modes)

/* Задание потока замера для потока интерфейса: показывать страницы во всех режимах. */
static struct {
    atomic_int active;
    atomic_int done;
    uint8_t *pages[2];
    int page_no[2];
    int step; /* 0 — первая страница полным обновлением, далее по две на режим */
} sj;

static int screen_run(const char *spec, const char *path)
{
    int a = 0, b = 0;
    if (sscanf(spec, "screen %d,%d", &a, &b) != 2 || a < 1 || b < 1) {
        fprintf(app.out, "тест экрана: нужно «screen СТР1,СТР2 | путь»\n");
        return 2;
    }
    doc_options opt;
    memset(&opt, 0, sizeof opt);
    opt.store_bytes = (size_t)64 << 20;
    opt.aa_level = -1;
    opt.no_icc = 1;
    opt.jpx_min_scale = 70;
    char err[256];
    doc *d = doc_open(path, &opt, err, sizeof err);
    if (!d) {
        fprintf(app.out, "тест экрана: %s\n", err);
        return 1;
    }
    int pages[2] = { a - 1, b - 1 }, rc = 0;
    for (int k = 0; k < 2 && rc == 0; k++) {
        doc_rect rect;
        sj.pages[k] = malloc((size_t)app.sstride * app.sh);
        sj.page_no[k] = pages[k] + 1;
        if (!sj.pages[k] || doc_page_bounds(d, pages[k], &rect, NULL) < 0) {
            rc = 1;
            break;
        }
        doc_view v = doc_fit_page(&rect, app.sw, app.sh);
        if (doc_render(d, pages[k], &v, sj.pages[k], app.sw, app.sh, app.sstride, NULL, NULL) < 0)
            rc = 1;
    }
    doc_close(d);
    if (rc == 0) {
        fprintf(app.out, "тест экрана: стр. %d и %d, по %.1f с на показ; режимы, которых нет в прошивке, пропускаются\n",
                a, b, SCREEN_HOLD_MS / 1000.0);
        sj.step = 0;
        atomic_store(&sj.done, 0);
        atomic_store(&sj.active, 1);
        while (!atomic_load(&sj.done)) {
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        atomic_store(&sj.active, 0);
    } else {
        fprintf(app.out, "тест экрана: страницы не нарисовались\n");
    }
    free(sj.pages[0]);
    free(sj.pages[1]);
    sj.pages[0] = sj.pages[1] = NULL;
    return rc;
}

/* Один шаг теста экрана в потоке интерфейса; 0 — тест закончен. */
static int screen_step(void)
{
    pf_screen *s = pf_get_screen();
    if (!s || !app.text)
        return 0;
    int k, mode_i;
    pf_update_mode mode;
    for (;;) {
        if (sj.step == 0) {
            k = 0;
            mode = PF_UPDATE_FULL;
            mode_i = -1;
        } else {
            mode_i = (sj.step - 1) / 2;
            if (mode_i >= SCREEN_MODES)
                return 0;
            mode = screen_modes[mode_i];
            k = (sj.step - 1) % 2 == 0 ? 1 : 0;
            if (!pf_update_mode_available(mode)) {
                if (k == 1)
                    fprintf(app.out, "экран: %-18s нет в прошивке\n", pf_update_mode_name(mode));
                sj.step++;
                continue;
            }
        }
        break;
    }
    for (int y = 0; y < s->height; y++)
        memcpy(s->pixels + (size_t)y * s->stride, sj.pages[k] + (size_t)y * app.sstride, (size_t)s->width);
    char label[96];
    snprintf(label, sizeof label, "%s — стр. %d", mode_i < 0 ? "исходная (FullUpdate)" : pf_update_mode_name(mode),
             sj.page_no[k]);
    ui_fill(s, 0, s->height - 70, s->width, 70, 255);
    ui_text_draw(app.text, s, 40, s->height - 22, 40, 0, label);
    double t0 = clock_ms();
    pf_update(0, 0, s->width, s->height, mode);
    double t1 = clock_ms();
    pf_wait_update();
    double t2 = clock_ms();
    if (mode_i >= 0)
        fprintf(app.out, "экран: %-18s стр. %4d: вызов %6.1f ms, до конца %6.1f ms\n", pf_update_mode_name(mode),
                sj.page_no[k], t1 - t0, t2 - t0);
    sj.step++;
    return 1;
}

static int load_runs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    while (fgets(line, sizeof line, f) && app.count < MAX_RUNS) {
        char *s = str_trim(line);
        char *bar = strchr(s, '|');
        if (*s == '#' || !bar)
            continue;
        *bar = 0;
        bench_run *r = &app.runs[app.count++];
        snprintf(r->spec, sizeof r->spec, "%s", str_trim(s));
        snprintf(r->path, sizeof r->path, "%s", str_trim(bar + 1));
    }
    fclose(f);
    return 0;
}

static void log_file_line(FILE *out, const char *path, const char *prefix)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f))
        if (!prefix || !strncmp(line, prefix, strlen(prefix))) {
            fprintf(out, "%s: %s", path, line);
            break;
        }
    fclose(f);
}

/* Прогон другой сборкой: отдельный процесс, вывод — в тот же лог. */
static int run_binary(const char *bin, char **bench_argv, int bench_argc)
{
    char path[1024];
    char *argv[MAX_ARGS + 3];
    snprintf(path, sizeof path, "%s/bin/washi-%s", pf_data_dir(), bin);
    argv[0] = path;
    argv[1] = "--bench";
    for (int i = 0; i < bench_argc; i++)
        argv[2 + i] = bench_argv[i];
    argv[2 + bench_argc] = NULL;

    fflush(app.out);
    pid_t pid = fork();
    if (pid == 0) {
        /* После fork в многопоточном процессе — только async-signal-safe вызовы. */
        dup2(fileno(app.out), 1);
        dup2(fileno(app.out), 2);
        execv(path, argv);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(app.out, "fork не удался\n");
        return -1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        fprintf(app.out, "не удалось запустить %s\n", path);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Скрипт оболочки: отдельный процесс, вывод — в лог. */
static int run_shell(const char *script)
{
    fflush(app.out);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fileno(app.out), 1);
        dup2(fileno(app.out), 2);
        execl("/bin/sh", "sh", script, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(app.out, "fork не удался\n");
        return -1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void *bench_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < app.count; i++) {
        bench_run *r = &app.runs[i];
        atomic_store(&app.current, i);

        char spec[256];
        char *argv[MAX_ARGS];
        int argc = 0;
        const char *bin = NULL;
        snprintf(spec, sizeof spec, "%s", r->spec);
        argv[argc++] = r->path;
        for (char *tok = strtok(spec, " \t"); tok && argc < MAX_ARGS; tok = strtok(NULL, " \t")) {
            if (!strncmp(tok, "bin=", 4))
                bin = tok + 4;
            else
                argv[argc++] = tok;
        }

        fprintf(app.out, "\n==== прогон %d/%d: %s | %s\n", i + 1, app.count, r->spec, r->path);
        log_file_line(app.out, "/proc/meminfo", "MemFree:");
        log_file_line(app.out, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", NULL);
        double t0 = clock_ms();
        int rc = !strcmp(r->spec, "shell")       ? run_shell(r->path)
                 : !strncmp(r->spec, "screen", 6) ? screen_run(r->spec, r->path)
                 : bin                          ? run_binary(bin, argv, argc)
                                                : bench_main(argc, argv, app.out);
        fprintf(app.out, "прогон %d: код %d, %.1f s\n", i + 1, rc, (clock_ms() - t0) / 1000);
        fflush(app.out);
        fsync(fileno(app.out));
    }
    atomic_store(&app.done, 1);
    return NULL;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void draw_status(int full)
{
    pf_screen *s = pf_get_screen();
    if (!s)
        return;
    if (!app.text)
        app.text = ui_text_new();
    if (!app.text)
        return;

    int cur = atomic_load(&app.current);
    int done = atomic_load(&app.done);
    double elapsed = ((done ? app.t_end : clock_ms()) - app.t_start) / 1000;
    char buf[1024];
    int x = 60, w = s->width - 120, y = 120;

    ui_fill(s, 0, 0, s->width, s->height, 255);
    y = ui_text_draw_wrapped(app.text, s, x, y, w, 56, 0, "washi: замер");
    y += 30;
    if (app.error[0]) {
        ui_text_draw_wrapped(app.text, s, x, y, w, 38, 0, app.error);
    } else {
        snprintf(buf, sizeof buf, "%s прогон %d из %d, прошло %.0f с", done ? "Готово:" : "Идёт",
                 done ? app.count : cur + 1, app.count, elapsed);
        y = ui_text_draw_wrapped(app.text, s, x, y, w, 40, 0, buf);
        if (!done && cur >= 0) {
            snprintf(buf, sizeof buf, "%s | %s", app.runs[cur].spec, base_name(app.runs[cur].path));
            y = ui_text_draw_wrapped(app.text, s, x, y + 20, w, 34, 0, buf);
        }
        snprintf(buf, sizeof buf, "Результат: %s/bench-log.txt", pf_data_dir());
        y = ui_text_draw_wrapped(app.text, s, x, y + 40, w, 34, 0, buf);
        if (done)
            ui_text_draw_wrapped(app.text, s, x, y + 40, w, 40, 0, "Нажмите любую кнопку для выхода.");
        else
            ui_text_draw_wrapped(app.text, s, x, y + 40, w, 34, 0,
                                 "Не трогайте книгу до конца замера: нажатия искажают время.");
    }
    pf_update(0, 0, s->width, s->height, full ? PF_UPDATE_FULL : PF_UPDATE_PARTIAL);
    app.shown_current = cur;
    app.shown_done = done;
}

static void on_start(void)
{
    char path[1024];
    const char *dir = pf_data_dir();
    mkdir(dir, 0755);
    atomic_store(&app.current, -1);
    app.t_start = clock_ms();

    snprintf(path, sizeof path, "%s/bench.txt", dir);
    if (load_runs(path) < 0 || app.count == 0) {
        snprintf(app.error, sizeof app.error, "Нет прогонов в %s. Формат строки: СТРАНИЦЫ [опции] | путь к PDF",
                 path);
        return;
    }
    pf_screen *scr = pf_get_screen();
    if (scr) {
        app.sw = scr->width;
        app.sh = scr->height;
        app.sstride = scr->stride;
    }
    snprintf(path, sizeof path, "%s/bench-log.txt", dir);
    app.out = fopen(path, "a");
    if (!app.out) {
        snprintf(app.error, sizeof app.error, "Не открыть %s", path);
        return;
    }
    time_t now = time(NULL);
    char date[64];
    strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(app.out, "\n######## washi-bench, сборка %s %s, запуск %s, прогонов %d\n", __DATE__,
            __TIME__, date, app.count);
    fflush(app.out);

    pf_keep_awake(KEEP_AWAKE_SEC);
    if (pthread_create(&app.thread, NULL, bench_thread, NULL) != 0) {
        snprintf(app.error, sizeof app.error, "pthread_create не удался");
        return;
    }
    app.thread_started = 1;
    pf_set_timer(TICK_MS);
}

static void on_event(const pf_event *ev, void *user)
{
    (void)user;
    /* Выход — только когда «Готово» уже на экране. */
    int finished = app.error[0] || app.shown_done;
    switch (ev->type) {
    case PF_EVENT_START:
        on_start();
        break;
    case PF_EVENT_SHOW:
        draw_status(1);
        break;
    case PF_EVENT_TIMER:
        if (atomic_load(&app.done) && !app.shown_done) {
            app.t_end = clock_ms();
            fprintf(app.out, "######## готово за %.1f s\n", (app.t_end - app.t_start) / 1000);
            fflush(app.out);
            fsync(fileno(app.out));
            draw_status(1);
            break;
        }
        if (++app.ticks % KEEP_AWAKE_EVERY_TICKS == 0)
            pf_keep_awake(KEEP_AWAKE_SEC);
        if (atomic_load(&sj.active) && !atomic_load(&sj.done)) {
            if (!app.text)
                app.text = ui_text_new();
            if (!screen_step()) {
                draw_status(1);
                atomic_store(&sj.done, 1);
            }
            pf_set_timer(SCREEN_HOLD_MS);
            break;
        }
        if (atomic_load(&app.current) != app.shown_current)
            draw_status(0);
        pf_set_timer(TICK_MS);
        break;
    case PF_EVENT_KEY_DOWN:
    case PF_EVENT_POINTER_UP:
        if (finished)
            pf_quit();
        break;
    case PF_EVENT_EXIT:
        if (app.out) {
            if (!atomic_load(&app.done))
                fprintf(app.out, "######## прервано на прогоне %d\n", atomic_load(&app.current) + 1);
            fflush(app.out);
            fsync(fileno(app.out));
        }
        break;
    default:
        break;
    }
}

int main(void)
{
    return pf_run(on_event, NULL);
}
