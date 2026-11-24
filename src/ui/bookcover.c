#include "bookcover.h"

#include "home.h"

/* Подложка вместо обложки: название по центру или значок книги. */
static void placeholder(gfx *g, pf_screen *s, ui_rect b, int radius, const char *path)
{
    gfx_rrect(g, s, b.x, b.y, b.w, b.h, radius, UI_FAINT);
    if (b.w < 200) {
        int is = b.w / 2;
        gfx_icon(g, s, ICON_BOOK, b.x + (b.w - is) / 2, b.y + (b.h - is) / 2, is, 0, UI_LINE);
        return;
    }
    char title[512];
    home_book_title(path, title, sizeof title);
    float size = b.w / 9.0f;
    size = size < 26 ? 26 : size > 44 ? 44 : size;
    int line = (int)(size * 1.25f), pad = b.w / 10, max_lines = (b.h - 2 * pad) / line;
    max_lines = max_lines > 6 ? 6 : max_lines < 1 ? 1 : max_lines;
    gfx_style st = { FONT_MEDIUM, size, UI_TEXT2 };
    int n = gfx_text_wrap_center(g, NULL, b.x + pad, 0, b.w - 2 * pad, line, max_lines, st, title);
    int base = b.y + (b.h - n * line) / 2 + (int)(size * 0.95f);
    gfx_text_wrap_center(g, s, b.x + pad, base, b.w - 2 * pad, line, max_lines, st, title);
}

int ui_book_cover(gfx *g, pf_screen *s, covers *cv, ui_rect b, int radius, const char *path, uint8_t bg)
{
    const cover_info *ci = NULL;
    int rc = cv ? covers_get(cv, path, &ci) : -1;
    gfx_fill(s, b.x, b.y, b.w, b.h, bg);
    if (rc != 0 || !ci->img || b.w <= 0 || b.h <= 0) {
        placeholder(g, s, b, radius, path);
        return rc == 1;
    }
    /* Часть картинки с пропорциями рамки: шире рамки — срезаем бока, выше — низ. */
    int sx = 0, sw = ci->w, sh = ci->h;
    if ((long long)ci->w * b.h > (long long)ci->h * b.w) {
        sw = (int)((long long)ci->h * b.w / b.h);
        sx = (ci->w - sw) / 2;
    } else {
        sh = (int)((long long)ci->w * b.h / b.w);
    }
    sw = sw < 1 ? 1 : sw;
    sh = sh < 1 ? 1 : sh;
    gfx_image(s, b.x, b.y, b.w, b.h, ci->img + sx, sw, sh, ci->w);
    gfx_round_corners(s, b.x, b.y, b.w, b.h, radius, bg);
    gfx_rrect_line(g, s, b.x, b.y, b.w, b.h, radius, 2, UI_LINE);
    return 0;
}
