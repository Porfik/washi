#include "text.h"

#include <string.h>

static gfx_style regular(float size, uint8_t gray)
{
    gfx_style st = { FONT_REGULAR, size, gray };
    return st;
}

ui_text *ui_text_new(void)
{
    return gfx_new();
}

void ui_text_free(ui_text *t)
{
    gfx_free(t);
}

int ui_text_draw(ui_text *t, pf_screen *s, int x, int baseline, float size, uint8_t gray, const char *str)
{
    return gfx_text(t, s, x, baseline, regular(size, gray), str);
}

int ui_text_draw_wrapped(ui_text *t, pf_screen *s, int x, int baseline, int max_w, float size, uint8_t gray,
                         const char *str)
{
    int line_h = (int)(size * 1.3f);
    /* Абзацы по «\n» — каждый со своей строки. */
    char para[1024];
    for (const char *p = str; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof para)
            n = sizeof para - 1;
        memcpy(para, p, n);
        para[n] = 0;
        int lines = gfx_text_wrap(t, s, x, baseline, max_w, line_h, 1000, regular(size, gray), para);
        baseline += (lines ? lines : 1) * line_h;
        if (!e)
            break;
        p = e + 1;
    }
    return baseline;
}

int ui_text_width(ui_text *t, float size, const char *str)
{
    return gfx_text_width(t, regular(size, 0), str);
}

int ui_text_draw_fit(ui_text *t, pf_screen *s, int x, int baseline, int max_w, float size, uint8_t gray,
                     const char *str)
{
    return gfx_text_fit(t, s, x, baseline, max_w, regular(size, gray), str);
}

void ui_fill(pf_screen *s, int x, int y, int w, int h, uint8_t gray)
{
    gfx_fill(s, x, y, w, h, gray);
}
