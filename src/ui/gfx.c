#include "gfx.h"

#include <mupdf/fitz.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct gfx {
    fz_context *ctx;
    fz_font *font[FONT_COUNT];
};

/* Файлы шрифтов по порядку предпочтения: Roboto есть в прошивке, Noto Sans — на ПК. */
static const char *const font_files[FONT_COUNT][3] = {
    [FONT_REGULAR] = { "Roboto-Regular.ttf", "NotoSans-Regular.ttf", NULL },
    [FONT_MEDIUM] = { "Roboto-Medium.ttf", "NotoSans-Medium.ttf", "NotoSans-Bold.ttf" },
    [FONT_BOLD] = { "Roboto-Bold.ttf", "NotoSans-Bold.ttf", NULL },
    [FONT_LIGHT] = { "Roboto-Light.ttf", "NotoSans-Light.ttf", "NotoSans-Regular.ttf" },
};

static fz_font *font_from_file(fz_context *ctx, const char *path)
{
    fz_font *font = NULL;
    fz_var(font);
    fz_try(ctx) font = fz_new_font_from_file(ctx, NULL, path, 0, 0);
    fz_catch(ctx) font = NULL;
    return font;
}

static fz_font *load_font(fz_context *ctx, gfx_font f)
{
    const char *dirs[] = { getenv("WASHI_FONTS"), "/ebrmain/fonts", "/mnt/ext1/system/fonts" };
    for (int i = 0; i < 3; i++) {
        if (!font_files[f][i])
            continue;
        for (int d = 0; d < 3; d++) {
            char path[512];
            struct stat st;
            if (!dirs[d])
                continue;
            snprintf(path, sizeof path, "%s/%s", dirs[d], font_files[f][i]);
            if (stat(path, &st) != 0)
                continue;
            fz_font *font = font_from_file(ctx, path);
            if (font)
                return font;
        }
    }
    fz_font *font = NULL;
    fz_var(font);
    fz_try(ctx) font = fz_new_base14_font(ctx, f == FONT_BOLD || f == FONT_MEDIUM ? "Helvetica-Bold" : "Helvetica");
    fz_catch(ctx) font = NULL;
    return font;
}

gfx *gfx_new(void)
{
    gfx *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->ctx = fz_new_context(NULL, NULL, 4 << 20);
    if (!g->ctx) {
        free(g);
        return NULL;
    }
    for (int f = 0; f < FONT_COUNT; f++)
        if (!(g->font[f] = load_font(g->ctx, (gfx_font)f))) {
            gfx_free(g);
            return NULL;
        }
    return g;
}

void gfx_free(gfx *g)
{
    if (!g)
        return;
    for (int f = 0; f < FONT_COUNT; f++)
        fz_drop_font(g->ctx, g->font[f]);
    fz_drop_context(g->ctx);
    free(g);
}

/* ---- Рисование через устройство MuPDF прямо в буфер экрана ---- */

typedef struct {
    fz_pixmap *pix;
    fz_device *dev;
} target;

static void target_open(fz_context *ctx, target *t, pf_screen *s)
{
    t->pix = fz_new_pixmap_with_data(ctx, fz_device_gray(ctx), s->width, s->height, NULL, 0, s->stride, s->pixels);
    t->dev = fz_new_draw_device(ctx, fz_identity, t->pix);
}

static void target_close(fz_context *ctx, target *t)
{
    if (t->dev)
        fz_close_device(ctx, t->dev);
    fz_drop_device(ctx, t->dev);
    fz_drop_pixmap(ctx, t->pix);
    t->dev = NULL;
    t->pix = NULL;
}

/* Ширина строки или рисование, если s не NULL. */
static int text_run_raw(gfx *g, pf_screen *s, int x, int baseline, gfx_style st, const char *str)
{
    fz_context *ctx = g->ctx;
    fz_text *text = NULL;
    target t = { NULL, NULL };
    int width = 0;

    fz_var(text);
    fz_var(t);
    fz_var(width);
    fz_try(ctx)
    {
        text = fz_new_text(ctx);
        fz_matrix trm = fz_make_matrix(st.size, 0, 0, -st.size, (float)x, (float)baseline);
        fz_matrix end = fz_show_string(ctx, text, g->font[st.font], trm, str, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
        width = (int)(end.e - x + 0.5f);
        if (s) {
            target_open(ctx, &t, s);
            float color = st.gray / 255.0f;
            fz_fill_text(ctx, t.dev, text, fz_identity, fz_device_gray(ctx), &color, 1, fz_default_color_params);
        }
    }
    fz_always(ctx)
    {
        target_close(ctx, &t);
        fz_drop_text(ctx, text);
    }
    fz_catch(ctx) width = 0;
    return width;
}

static int text_run(gfx *g, pf_screen *s, int x, int baseline, gfx_style st, const char *str)
{
    /* Неразрывного дефиса (U+2011, «Wi‑Fi») в шрифте нет — без замены на его месте пробел. */
    char fixed[1024];
    if (strstr(str, "\xE2\x80\x91") && strlen(str) < sizeof fixed) {
        size_t o = 0;
        for (const char *p = str; *p; p++) {
            if (!strncmp(p, "\xE2\x80\x91", 3)) {
                fixed[o++] = '-';
                p += 2;
            } else {
                fixed[o++] = *p;
            }
        }
        fixed[o] = 0;
        str = fixed;
    }

    return text_run_raw(g, s, x, baseline, st, str);
}

int gfx_text(gfx *g, pf_screen *s, int x, int baseline, gfx_style st, const char *str)
{
    return text_run(g, s, x, baseline, st, str);
}

int gfx_text_width(gfx *g, gfx_style st, const char *str)
{
    return text_run(g, NULL, 0, 0, st, str);
}

/* Длина самого длинного начала str (по целым символам UTF-8), которое с «…» влезает в max_w. */
static size_t fit_prefix(gfx *g, gfx_style st, const char *str, size_t len, int max_w)
{
    char buf[1024];
    if (len > sizeof buf - 4)
        len = sizeof buf - 4;
    size_t lo = 0, hi = len;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > lo && ((unsigned char)str[mid] & 0xc0) == 0x80)
            mid--;
        if (mid == lo)
            break;
        memcpy(buf, str, mid);
        memcpy(buf + mid, "\xe2\x80\xa6", 4);
        if (text_run(g, NULL, 0, 0, st, buf) <= max_w)
            lo = mid;
        else
            hi = mid - 1;
    }
    while (lo > 0 && ((unsigned char)str[lo] & 0xc0) == 0x80)
        lo--;
    while (lo > 0 && str[lo - 1] == ' ')
        lo--;
    return lo;
}

static int draw_ellipsized(gfx *g, pf_screen *s, int x, int baseline, int max_w, gfx_style st, const char *str,
                           size_t len)
{
    char buf[1024];
    size_t n = fit_prefix(g, st, str, len, max_w);
    memcpy(buf, str, n);
    memcpy(buf + n, "\xe2\x80\xa6", 4);
    return text_run(g, s, x, baseline, st, buf);
}

int gfx_text_fit(gfx *g, pf_screen *s, int x, int baseline, int max_w, gfx_style st, const char *str)
{
    if (text_run(g, NULL, 0, 0, st, str) <= max_w)
        return text_run(g, s, x, baseline, st, str);
    return draw_ellipsized(g, s, x, baseline, max_w, st, str, strlen(str));
}

int gfx_text_fit_middle(gfx *g, pf_screen *s, int x, int baseline, int max_w, gfx_style st, const char *str)
{
    if (text_run(g, NULL, 0, 0, st, str) <= max_w)
        return text_run(g, s, x, baseline, st, str);
    /* Хвост — до трети ширины, по целым символам; начало — сколько влезет перед «…». */
    size_t n = strlen(str), t = n;
    while (t > 0) {
        size_t p = t - 1;
        while (p > 0 && ((unsigned char)str[p] & 0xc0) == 0x80)
            p--;
        if (text_run(g, NULL, 0, 0, st, str + p) > max_w / 3)
            break;
        t = p;
    }
    int tw = text_run(g, NULL, 0, 0, st, str + t);
    size_t h = fit_prefix(g, st, str, t, max_w - tw);
    char buf[1024];
    snprintf(buf, sizeof buf, "%.*s\xe2\x80\xa6%s", (int)h, str, str + t);
    return text_run(g, s, x, baseline, st, buf);
}

static int wrap(gfx *g, pf_screen *s, int x, int baseline, int max_w, int line_h, int max_lines, gfx_style st,
                const char *str, int center);

int gfx_text_wrap(gfx *g, pf_screen *s, int x, int baseline, int max_w, int line_h, int max_lines,
                  gfx_style st, const char *str)
{
    return wrap(g, s, x, baseline, max_w, line_h, max_lines, st, str, 0);
}

int gfx_text_wrap_center(gfx *g, pf_screen *s, int x, int baseline, int max_w, int line_h, int max_lines,
                         gfx_style st, const char *str)
{
    return wrap(g, s, x, baseline, max_w, line_h, max_lines, st, str, 1);
}

/* Строка по центру ширины max_w (или от x), с «…», если не влезает. */
static void line_at(gfx *g, pf_screen *s, int x, int baseline, int max_w, gfx_style st, const char *str, size_t len,
                    int center)
{
    char buf[1024];
    if (len >= sizeof buf - 4)
        len = sizeof buf - 5;
    memcpy(buf, str, len);
    buf[len] = 0;
    int w = text_run(g, NULL, 0, 0, st, buf);
    if (w > max_w) {
        size_t n = fit_prefix(g, st, str, len, max_w);
        memcpy(buf + n, "\xe2\x80\xa6", 4);
        w = text_run(g, NULL, 0, 0, st, buf);
    }
    text_run(g, s, center ? x + (max_w - w) / 2 : x, baseline, st, buf);
}

static int wrap(gfx *g, pf_screen *s, int x, int baseline, int max_w, int line_h, int max_lines, gfx_style st,
                const char *str, int center)
{
    int lines = 0;
    const char *p = str;
    char line[1024];
    while (*p == ' ')
        p++;
    while (*p && lines < max_lines) {
        if (lines == max_lines - 1) {
            /* Последняя строка: всё оставшееся, при нехватке места — с «…». */
            line_at(g, s, x, baseline, max_w, st, p, strlen(p), center);
            return lines + 1;
        }
        /* Набираем слова, пока влезают. */
        size_t best = 0;
        const char *q = p;
        for (;;) {
            const char *e = q;
            while (*e && *e != ' ')
                e++;
            size_t n = (size_t)(e - p);
            if (n >= sizeof line)
                break;
            memcpy(line, p, n);
            line[n] = 0;
            if (best && text_run(g, NULL, 0, 0, st, line) > max_w)
                break;
            best = n;
            if (!*e)
                break;
            q = e + 1;
        }
        if (!best) {
            /* Одно слово длиннее строки — обрезаем. */
            line_at(g, s, x, baseline, max_w, st, p, strlen(p), center);
            return lines + 1;
        }
        line_at(g, s, x, baseline, max_w, st, p, best, center);
        lines++;
        baseline += line_h;
        p += best;
        while (*p == ' ')
            p++;
    }
    return lines;
}

void gfx_fill(pf_screen *s, int x, int y, int w, int h, uint8_t gray)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s->width)
        w = s->width - x;
    if (y + h > s->height)
        h = s->height - y;
    if (w <= 0 || h <= 0)
        return;
    for (int row = y; row < y + h; row++)
        memset(s->pixels + (size_t)row * s->stride + x, gray, (size_t)w);
}

/* ---- Фигуры ---- */

#define KAPPA 0.5523f

static void rrect_path(fz_context *ctx, fz_path *p, float x, float y, float w, float h, float r)
{
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    float k = KAPPA * r;
    fz_moveto(ctx, p, x + r, y);
    fz_lineto(ctx, p, x + w - r, y);
    fz_curveto(ctx, p, x + w - r + k, y, x + w, y + r - k, x + w, y + r);
    fz_lineto(ctx, p, x + w, y + h - r);
    fz_curveto(ctx, p, x + w, y + h - r + k, x + w - r + k, y + h, x + w - r, y + h);
    fz_lineto(ctx, p, x + r, y + h);
    fz_curveto(ctx, p, x + r - k, y + h, x, y + h - r + k, x, y + h - r);
    fz_lineto(ctx, p, x, y + r);
    fz_curveto(ctx, p, x, y + r - k, x + r - k, y, x + r, y);
    fz_closepath(ctx, p);
}

/* Залить (line <= 0) или обвести путь толщиной line (в единицах пути, ctm применяется к обоим). */
static void draw_path(gfx *g, pf_screen *s, fz_path *path, fz_matrix ctm, float line, uint8_t gray)
{
    fz_context *ctx = g->ctx;
    target t = { NULL, NULL };
    fz_stroke_state *stroke = NULL;
    fz_var(t);
    fz_var(stroke);
    fz_try(ctx)
    {
        target_open(ctx, &t, s);
        float color = gray / 255.0f;
        if (line > 0) {
            stroke = fz_new_stroke_state(ctx);
            stroke->linewidth = line;
            stroke->start_cap = stroke->end_cap = stroke->dash_cap = FZ_LINECAP_ROUND;
            stroke->linejoin = FZ_LINEJOIN_ROUND;
            fz_stroke_path(ctx, t.dev, path, stroke, ctm, fz_device_gray(ctx), &color, 1, fz_default_color_params);
        } else {
            fz_fill_path(ctx, t.dev, path, 0, ctm, fz_device_gray(ctx), &color, 1, fz_default_color_params);
        }
    }
    fz_always(ctx)
    {
        target_close(ctx, &t);
        fz_drop_stroke_state(ctx, stroke);
    }
    fz_catch(ctx) {}
}

void gfx_rrect(gfx *g, pf_screen *s, int x, int y, int w, int h, float r, uint8_t gray)
{
    fz_context *ctx = g->ctx;
    fz_path *p = NULL;
    fz_var(p);
    fz_try(ctx)
    {
        p = fz_new_path(ctx);
        rrect_path(ctx, p, (float)x, (float)y, (float)w, (float)h, r);
        draw_path(g, s, p, fz_identity, 0, gray);
    }
    fz_always(ctx) fz_drop_path(ctx, p);
    fz_catch(ctx) {}
}

void gfx_rrect_line(gfx *g, pf_screen *s, int x, int y, int w, int h, float r, float line, uint8_t gray)
{
    fz_context *ctx = g->ctx;
    fz_path *p = NULL;
    float d = line / 2;
    fz_var(p);
    fz_try(ctx)
    {
        p = fz_new_path(ctx);
        rrect_path(ctx, p, x + d, y + d, w - line, h - line, r - d > 0 ? r - d : 0);
        draw_path(g, s, p, fz_identity, line, gray);
    }
    fz_always(ctx) fz_drop_path(ctx, p);
    fz_catch(ctx) {}
}

/* ---- Контуры SVG ---- */

typedef struct {
    const char *p;
} svg_reader;

static void svg_skip(svg_reader *r)
{
    while (*r->p == ' ' || *r->p == ',' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')
        r->p++;
}

/* Следующее число; 0 — дальше не число. */
static int svg_num(svg_reader *r, float *out)
{
    svg_skip(r);
    char *end;
    float v = strtof(r->p, &end);
    if (end == r->p)
        return 0;
    r->p = end;
    *out = v;
    return 1;
}

/* Флаг дуги: одна цифра 0 или 1, может стоять вплотную к следующему числу. */
static int svg_flag(svg_reader *r, int *out)
{
    svg_skip(r);
    if (*r->p != '0' && *r->p != '1')
        return 0;
    *out = *r->p++ == '1';
    return 1;
}

static float vec_angle(float ux, float uy, float vx, float vy)
{
    return atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
}

/* Дуга эллипса SVG (A) кривыми Безье: из (x1, y1) в (x2, y2), SVG 1.1, приложение F.6.5. */
static void svg_arc(fz_context *ctx, fz_path *path, float x1, float y1, float rx, float ry, float phi_deg, int large,
                    int sweep, float x2, float y2)
{
    if (rx == 0 || ry == 0 || (x1 == x2 && y1 == y2)) {
        fz_lineto(ctx, path, x2, y2);
        return;
    }
    rx = fabsf(rx);
    ry = fabsf(ry);
    float phi = phi_deg * 3.14159265f / 180, cp = cosf(phi), sp = sinf(phi);
    float dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    float x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) {
        rx *= sqrtf(lam);
        ry *= sqrtf(lam);
    }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float coef = den > 0 && num > 0 ? sqrtf(num / den) : 0;
    if (large == sweep)
        coef = -coef;
    float cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    float cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    float t1 = vec_angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float dt = vec_angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dt > 0)
        dt -= 2 * 3.14159265f;
    else if (sweep && dt < 0)
        dt += 2 * 3.14159265f;
    int n = (int)ceilf(fabsf(dt) / 1.5708f);
    if (n < 1)
        n = 1;
    float step = dt / n, k = 4.0f / 3.0f * tanf(step / 4);
    for (int i = 0; i < n; i++) {
        float a0 = t1 + i * step, a1 = a0 + step;
        float px0 = cx + rx * cosf(a0) * cp - ry * sinf(a0) * sp, py0 = cy + rx * cosf(a0) * sp + ry * sinf(a0) * cp;
        float dx0 = -rx * sinf(a0) * cp - ry * cosf(a0) * sp, dy0 = -rx * sinf(a0) * sp + ry * cosf(a0) * cp;
        float px1 = cx + rx * cosf(a1) * cp - ry * sinf(a1) * sp, py1 = cy + rx * cosf(a1) * sp + ry * sinf(a1) * cp;
        float dx1 = -rx * sinf(a1) * cp - ry * cosf(a1) * sp, dy1 = -rx * sinf(a1) * sp + ry * cosf(a1) * cp;
        if (i == n - 1) {
            px1 = x2;
            py1 = y2;
        }
        fz_curveto(ctx, path, px0 + k * dx0, py0 + k * dy0, px1 - k * dx1, py1 - k * dy1, px1, py1);
    }
}

/* Контур SVG (path d) в fz_path: M L H V C S Q T A Z, строчные — относительные. */
static void svg_path(fz_context *ctx, fz_path *path, const char *d)
{
    svg_reader r = { d };
    float cx = 0, cy = 0, sx = 0, sy = 0, qx = 0, qy = 0, ccx = 0, ccy = 0;
    char cmd = 0, last = 0;
    for (;;) {
        svg_skip(&r);
        if (!*r.p)
            break;
        if ((*r.p >= 'A' && *r.p <= 'Z') || (*r.p >= 'a' && *r.p <= 'z'))
            cmd = *r.p++;
        else if (!cmd)
            break;
        int rel = cmd >= 'a';
        float ox = rel ? cx : 0, oy = rel ? cy : 0, a[7];
        int f1, f2;
        switch (cmd | 0x20) {
        case 'm':
            if (!svg_num(&r, &a[0]) || !svg_num(&r, &a[1]))
                return;
            cx = sx = ox + a[0];
            cy = sy = oy + a[1];
            fz_moveto(ctx, path, cx, cy);
            cmd = rel ? 'l' : 'L'; /* следующие пары — линии */
            break;
        case 'l':
            if (!svg_num(&r, &a[0]) || !svg_num(&r, &a[1]))
                return;
            cx = ox + a[0];
            cy = oy + a[1];
            fz_lineto(ctx, path, cx, cy);
            break;
        case 'h':
            if (!svg_num(&r, &a[0]))
                return;
            cx = ox + a[0];
            fz_lineto(ctx, path, cx, cy);
            break;
        case 'v':
            if (!svg_num(&r, &a[0]))
                return;
            cy = oy + a[0];
            fz_lineto(ctx, path, cx, cy);
            break;
        case 'c':
            for (int i = 0; i < 6; i++)
                if (!svg_num(&r, &a[i]))
                    return;
            fz_curveto(ctx, path, ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]);
            ccx = ox + a[2];
            ccy = oy + a[3];
            cx = ox + a[4];
            cy = oy + a[5];
            break;
        case 's': {
            for (int i = 0; i < 4; i++)
                if (!svg_num(&r, &a[i]))
                    return;
            int smooth = (last | 0x20) == 'c' || (last | 0x20) == 's';
            float x1 = smooth ? 2 * cx - ccx : cx, y1 = smooth ? 2 * cy - ccy : cy;
            fz_curveto(ctx, path, x1, y1, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
            ccx = ox + a[0];
            ccy = oy + a[1];
            cx = ox + a[2];
            cy = oy + a[3];
            break;
        }
        case 'q':
        case 't': {
            int smooth = (last | 0x20) == 'q' || (last | 0x20) == 't';
            if ((cmd | 0x20) == 'q') {
                for (int i = 0; i < 4; i++)
                    if (!svg_num(&r, &a[i]))
                        return;
                qx = ox + a[0];
                qy = oy + a[1];
                a[0] = a[2];
                a[1] = a[3];
            } else {
                if (!svg_num(&r, &a[0]) || !svg_num(&r, &a[1]))
                    return;
                qx = smooth ? 2 * cx - qx : cx;
                qy = smooth ? 2 * cy - qy : cy;
            }
            float x = ox + a[0], y = oy + a[1];
            fz_curveto(ctx, path, cx + 2.0f / 3 * (qx - cx), cy + 2.0f / 3 * (qy - cy), x + 2.0f / 3 * (qx - x),
                       y + 2.0f / 3 * (qy - y), x, y);
            cx = x;
            cy = y;
            break;
        }
        case 'a':
            if (!svg_num(&r, &a[0]) || !svg_num(&r, &a[1]) || !svg_num(&r, &a[2]) || !svg_flag(&r, &f1) ||
                !svg_flag(&r, &f2) || !svg_num(&r, &a[3]) || !svg_num(&r, &a[4]))
                return;
            svg_arc(ctx, path, cx, cy, a[0], a[1], a[2], f1, f2, ox + a[3], oy + a[4]);
            cx = ox + a[3];
            cy = oy + a[4];
            break;
        case 'z':
            fz_closepath(ctx, path);
            cx = sx;
            cy = sy;
            cmd = 0; /* после Z — только новая команда */
            break;
        default:
            return;
        }
        last = cmd ? cmd : 'z';
    }
}

/* ---- Значки ---- */

#include "icons_data.h"

void gfx_svg(gfx *g, pf_screen *s, const char *d, const float vb[4], int x, int y, int size, float line, uint8_t gray)
{
    fz_context *ctx = g->ctx;
    fz_path *p = NULL;
    float big = vb[2] > vb[3] ? vb[2] : vb[3], k = size / big;
    float tx = x + (size - vb[2] * k) / 2 - vb[0] * k, ty = y + (size - vb[3] * k) / 2 - vb[1] * k;
    fz_var(p);
    fz_try(ctx)
    {
        p = fz_new_path(ctx);
        svg_path(ctx, p, d);
        draw_path(g, s, p, fz_pre_scale(fz_translate(tx, ty), k, k), line > 0 ? line / k : 0, gray);
    }
    fz_always(ctx) fz_drop_path(ctx, p);
    fz_catch(ctx) {}
}

void gfx_icon_ex(gfx *g, pf_screen *s, gfx_icon_id id, int x, int y, int size, float line, int filled,
                 uint8_t gray)
{
    if (id < 0 || id >= ICON_COUNT)
        return;
    const icon_svg *ic = &icons_lucide[id];
    /* Линии: 2 из 24 — как задумано; залитого варианта нет — линия толще. */
    float stroke = line > 0 ? line : size * 2.0f / 24;
    if (filled)
        stroke *= 1.35f;
    float vb[4] = { ic->vb_x, ic->vb_y, ic->vb_w, ic->vb_h };
    gfx_svg(g, s, ic->d, vb, x, y, size, stroke, gray);
}

void gfx_icon(gfx *g, pf_screen *s, gfx_icon_id id, int x, int y, int size, float line, uint8_t gray)
{
    gfx_icon_ex(g, s, id, x, y, size, line, 0, gray);
}

/* ---- Картинки ---- */

void gfx_image(pf_screen *s, int x, int y, int dw, int dh, const uint8_t *img, int w, int h, int stride)
{
    if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0)
        return;
    /* Границы столбцов источника — один раз на картинку: 64-битное деление на каждую точку
     * на ARM — вызов библиотеки, и обложки рисовались в разы дольше. */
    int local[1025];
    int *xs = dw < 1025 ? local : malloc(sizeof *xs * (size_t)(dw + 1));
    if (!xs)
        return;
    for (int dx = 0; dx <= dw; dx++)
        xs[dx] = (int)((long long)dx * w / dw);
    int dx0 = x < 0 ? -x : 0, dx1 = x + dw > s->width ? s->width - x : dw;
    for (int dy = 0; dy < dh; dy++) {
        int ty = y + dy;
        if (ty < 0 || ty >= s->height)
            continue;
        int sy0 = (int)((long long)dy * h / dh), sy1 = (int)((long long)(dy + 1) * h / dh);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        int rows = sy1 - sy0;
        uint8_t *out = s->pixels + (size_t)ty * s->stride + x;
        const uint8_t *src = img + (size_t)sy0 * stride;
        for (int dx = dx0; dx < dx1; dx++) {
            int sx0 = xs[dx], sx1 = xs[dx + 1];
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            if (rows == 1 && sx1 - sx0 == 1) {
                out[dx] = src[sx0];
                continue;
            }
            unsigned sum = 0;
            for (int yy = 0; yy < rows; yy++) {
                const uint8_t *row = src + (size_t)yy * stride;
                for (int xx = sx0; xx < sx1; xx++)
                    sum += row[xx];
            }
            out[dx] = (uint8_t)(sum / (unsigned)(rows * (sx1 - sx0)));
        }
    }
    if (xs != local)
        free(xs);
}

void gfx_round_corners(pf_screen *s, int x, int y, int w, int h, int r, uint8_t bg)
{
    if (r <= 0 || w < 2 * r || h < 2 * r)
        return;
    for (int dy = 0; dy < r; dy++)
        for (int dx = 0; dx < r; dx++) {
            /* Покрытие точки кругом радиуса r с центром (r, r): 1 внутри, 0 снаружи, плавно на краю. */
            float fx = r - dx - 0.5f, fy = r - dy - 0.5f;
            float cov = r - sqrtf(fx * fx + fy * fy) + 0.5f;
            if (cov >= 1)
                continue;
            if (cov < 0)
                cov = 0;
            int xs[2] = { x + dx, x + w - 1 - dx }, ys[2] = { y + dy, y + h - 1 - dy };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    int px = xs[i], py = ys[j];
                    if (px < 0 || py < 0 || px >= s->width || py >= s->height)
                        continue;
                    uint8_t *q = s->pixels + (size_t)py * s->stride + px;
                    *q = (uint8_t)(*q * cov + bg * (1 - cov) + 0.5f);
                }
        }
}
