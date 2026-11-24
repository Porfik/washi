#include "margins.h"

#include <stdlib.h>
#include <string.h>

#include "../base/clock.h"
#include "../base/log.h"

/* Мелкая отрисовка: четверть экрана книги. */
#define SAMPLE_W 351
#define SAMPLE_H 468
#define MAX_SAMPLES 8
#define DARK 200        /* точка темнее — содержимое */
#define MIN_DARK 2      /* строка или столбец с меньшим числом тёмных точек — шум */
#define BORDER 0.6      /* строка или столбец у края, тёмные больше чем на 60 %, — полоса сканера */
#define PAD 0.01f       /* запас вокруг рамки, доля страницы */

/* Рамка содержимого в точках буфера: box = x0, y0, x1, y1 (x1, y1 — после последней). 0 — есть. */
static int content_box(const uint8_t *buf, int w, int h, int box[4])
{
    int *rows = calloc((size_t)h, sizeof *rows), *cols = calloc((size_t)w, sizeof *cols);
    int found = -1;
    if (!rows || !cols)
        goto done;

    /* Полосы сканера по краям: почти сплошь тёмные строки и столбцы. */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (buf[(size_t)y * w + x] < DARK) {
                rows[y]++;
                cols[x]++;
            }
    int l = 0, r = w, t = 0, b = h;
    while (l < r && cols[l] > BORDER * h)
        l++;
    while (r > l && cols[r - 1] > BORDER * h)
        r--;
    while (t < b && rows[t] > BORDER * w)
        t++;
    while (b > t && rows[b - 1] > BORDER * w)
        b--;

    /* Внутри полос — заново. */
    memset(rows, 0, sizeof *rows * (size_t)h);
    memset(cols, 0, sizeof *cols * (size_t)w);
    for (int y = t; y < b; y++)
        for (int x = l; x < r; x++)
            if (buf[(size_t)y * w + x] < DARK) {
                rows[y]++;
                cols[x]++;
            }
    int y0 = t, y1 = b, x0 = l, x1 = r;
    while (y0 < y1 && rows[y0] < MIN_DARK)
        y0++;
    while (y1 > y0 && rows[y1 - 1] < MIN_DARK)
        y1--;
    while (x0 < x1 && cols[x0] < MIN_DARK)
        x0++;
    while (x1 > x0 && cols[x1 - 1] < MIN_DARK)
        x1--;
    if (x1 > x0 && y1 > y0) {
        box[0] = x0;
        box[1] = y0;
        box[2] = x1;
        box[3] = y1;
        found = 0;
    }
done:
    free(rows);
    free(cols);
    return found;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* Объединение рамок n страниц без одной самой выступающей с каждой стороны (если их 3 и больше). */
static void combine(float boxes[][4], int n, float out[4])
{
    float v[MAX_SAMPLES];
    int skip = n >= 3 ? 1 : 0;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < n; i++)
            v[i] = boxes[i][k];
        qsort(v, (size_t)n, sizeof *v, cmp_float);
        /* x0, y0 — наименьшее, x1, y1 — наибольшее */
        out[k] = k < 2 ? v[skip] : v[n - 1 - skip];
    }
    out[0] = out[0] - PAD < 0 ? 0 : out[0] - PAD;
    out[1] = out[1] - PAD < 0 ? 0 : out[1] - PAD;
    out[2] = out[2] + PAD > 1 ? 1 : out[2] + PAD;
    out[3] = out[3] + PAD > 1 ? 1 : out[3] + PAD;
    /* Подозрительно маленькая рамка — лучше вся страница. */
    if (out[2] - out[0] < 0.3f || out[3] - out[1] < 0.3f) {
        out[0] = out[1] = 0;
        out[2] = out[3] = 1;
    }
}

int margins_find(const char *path, const doc_options *opt, float out[8], margins_progress progress, void *user)
{
    char err[256];
    double t0 = clock_ms();
    doc *d = doc_open(path, opt, err, sizeof err);
    if (!d) {
        log_msg("поля: %s: %s", path, err);
        return -1;
    }
    int count = doc_page_count(d);
    uint8_t *buf = malloc((size_t)SAMPLE_W * SAMPLE_H);
    float boxes[2][MAX_SAMPLES][4];
    int nb[2] = { 0, 0 };
    int samples = count < MAX_SAMPLES ? count : MAX_SAMPLES;

    for (int i = 0; buf && i < samples; i++) {
        /* Середина книги: обложка, оглавление и приложения в начале и конце нетипичны. */
        int p = count <= MAX_SAMPLES ? i : (int)(count * (0.15 + 0.7 * i / (samples - 1)));
        doc_rect rect;
        if (doc_page_bounds(d, p, &rect, NULL) == 0) {
            doc_view v = doc_fit_page(&rect, SAMPLE_W, SAMPLE_H);
            int box[4];
            if (doc_render(d, p, &v, buf, SAMPLE_W, SAMPLE_H, SAMPLE_W, NULL, NULL) == 0 &&
                content_box(buf, SAMPLE_W, SAMPLE_H, box) == 0) {
                float pw = rect.x1 - rect.x0, ph = rect.y1 - rect.y0;
                float *f = boxes[p & 1][nb[p & 1]++];
                f[0] = (v.x0 + box[0] / v.scale - rect.x0) / pw;
                f[1] = (v.y0 + box[1] / v.scale - rect.y0) / ph;
                f[2] = (v.x0 + box[2] / v.scale - rect.x0) / pw;
                f[3] = (v.y0 + box[3] / v.scale - rect.y0) / ph;
                for (int k = 0; k < 4; k++)
                    f[k] = f[k] < 0 ? 0 : f[k] > 1 ? 1 : f[k];
                log_msg("поля: стр. %d: %.3f %.3f %.3f %.3f", p + 1, f[0], f[1], f[2], f[3]);
            }
        }
        if (progress)
            progress(user, i + 1, samples);
    }
    free(buf);
    doc_close(d);
    if (nb[0] + nb[1] == 0) {
        log_msg("поля: %s: содержимого не нашлось", path);
        return -1;
    }
    for (int k = 0; k < 2; k++) {
        /* У страниц одной чётности нет образцов — берётся другая. */
        int src = nb[k] ? k : 1 - k;
        combine(boxes[src], nb[src], out + 4 * k);
    }
    log_msg("поля: %s — нечётные %.3f %.3f %.3f %.3f, чётные %.3f %.3f %.3f %.3f, %.0f ms", path, out[0], out[1],
            out[2], out[3], out[4], out[5], out[6], out[7], clock_ms() - t0);
    return 0;
}
