#include "folderpick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/dirpath.h"
#include "../base/log.h"
#include "../net/net.h"
#include "widgets.h"

#define BTN_H 100
#define CRUMB_H 120

/* ---- Подписи ---- */

void folder_label(const char *rel, char *out, size_t len)
{
    if (!rel || !*rel || !strcmp(rel, "."))
        snprintf(out, len, "Вся библиотека");
    else
        ui_path_crumbs(rel, strlen(rel), out, len);
}

/* Короткая подпись общей папки (полный путь → от корня; вне корня — последнее имя). */
static void shared_label(const char *root, const char *shared, char *out, size_t len)
{
    char rel[1024];
    if (shared && dirpath_relative(root, shared, rel, sizeof rel) == 0) {
        folder_label(rel, out, len);
        return;
    }
    const char *slash = shared ? strrchr(shared, '/') : NULL;
    snprintf(out, len, "%s", slash && slash[1] ? slash + 1 : shared ? shared : "");
}

/* ---- folderpick ---- */

struct folderpick {
    gfx *g;
    char root[1024], hide[1024];
    void (*redraw)(void *, int);
    void *user;
    char title[64];
    char rel[1024]; /* текущая папка */
    char **names;
    int n, page, per_page;
    ui_header_hits hits;
    ui_rect up, rows[24], btn_new, btn_pick;
    int nrows;
    char result[1024];
};

folderpick *folderpick_new(gfx *g, const char *root, const char *hide, void (*redraw)(void *, int), void *user)
{
    folderpick *fp = calloc(1, sizeof *fp);
    if (!fp)
        return NULL;
    fp->g = g;
    snprintf(fp->root, sizeof fp->root, "%s", root);
    snprintf(fp->hide, sizeof fp->hide, "%s", hide ? hide : "");
    fp->redraw = redraw;
    fp->user = user;
    return fp;
}

void folderpick_free(folderpick *fp)
{
    if (!fp)
        return;
    dirpath_free(fp->names, fp->n);
    free(fp);
}

static void reload(folderpick *fp)
{
    dirpath_free(fp->names, fp->n);
    fp->names = NULL;
    fp->n = 0;
    if (dirpath_subdirs(fp->root, fp->hide, fp->rel, &fp->names, &fp->n) != 0) {
        fp->names = NULL;
        fp->n = 0;
    }
    fp->page = 0;
}

/* Убрать последнюю часть пути. */
static void go_up(char *rel)
{
    char *slash = strrchr(rel, '/');
    if (slash)
        *slash = 0;
    else
        rel[0] = 0;
}

void folderpick_open(folderpick *fp, const char *title, const char *start)
{
    snprintf(fp->title, sizeof fp->title, "%s", title);
    char abs[1024];
    /* start, приведённый к «чистому» виду; нет такой папки — выше, пока не найдётся */
    if (!start || dirpath_join(fp->root, start, abs, sizeof abs) != 0 || dirpath_relative(fp->root, abs, fp->rel, sizeof fp->rel) != 0)
        fp->rel[0] = 0;
    for (;;) {
        char **names;
        int n;
        if (dirpath_subdirs(fp->root, fp->hide, fp->rel, &names, &n) == 0) {
            dirpath_free(names, n);
            break;
        }
        if (!fp->rel[0])
            break;
        go_up(fp->rel);
    }
    reload(fp);
}

const char *folderpick_result(const folderpick *fp)
{
    return fp->result;
}

void folderpick_draw(folderpick *fp, pf_screen *s)
{
    gfx *g = fp->g;
    int top = ui_content_top(), bottom = ui_content_bottom(s);
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    gfx_fill(s, 0, UI_STATUS_H, s->width, bottom - UI_STATUS_H, UI_BG);

    int y = top - 8, list_y = y + CRUMB_H + 16, by = bottom - 16 - BTN_H;
    int per = (by - 16 - list_y) / UI_ROW_H;
    if (per < 1)
        per = 1;
    if (per > 24)
        per = 24;
    fp->per_page = per;
    int pages = (fp->n + per - 1) / per;
    if (pages < 1)
        pages = 1;
    if (fp->page >= pages)
        fp->page = pages - 1;
    fp->hits = ui_header(g, s, fp->title, 1, fp->page, pages);

    /* Путь: «‹» выше и крошки */
    ui_card(g, s, x, y, w, CRUMB_H);
    int tx = x + 40;
    fp->up = (ui_rect){ 0, 0, 0, 0 };
    if (fp->rel[0]) {
        gfx_icon(g, s, ICON_BACK, x + 28, y + CRUMB_H / 2 - 32, 64, 0, UI_BLACK);
        gfx_fill(s, x + 120, y + 24, 2, CRUMB_H - 48, UI_FAINT);
        fp->up = (ui_rect){ x, y, 130, CRUMB_H };
        tx = x + 150;
    }
    char crumbs[1100];
    folder_label(fp->rel, crumbs, sizeof crumbs);
    gfx_style cs = { FONT_MEDIUM, 40, UI_BLACK };
    gfx_text_fit_middle(g, s, tx, y + CRUMB_H / 2 + 14, x + w - 40 - tx, cs, crumbs);

    /* Подпапки */
    int first = fp->page * per, cnt = fp->n - first;
    if (cnt > per)
        cnt = per;
    fp->nrows = cnt;
    ui_card(g, s, x, list_y, w, (cnt > 0 ? cnt : 1) * UI_ROW_H);
    gfx_style ns = { FONT_REGULAR, 40, UI_BLACK }, es = { FONT_REGULAR, 36, UI_TEXT2 };
    if (cnt <= 0) {
        gfx_text_fit(g, s, x + 40, list_y + UI_ROW_H / 2 + 12, w - 80, es, "Вложенных папок нет");
    }
    for (int i = 0; i < cnt; i++) {
        int ry = list_y + i * UI_ROW_H, cy = ry + UI_ROW_H / 2;
        gfx_icon(g, s, ICON_FOLDER, x + 40, cy - 30, 60, 0, UI_BLACK);
        gfx_icon(g, s, ICON_NEXT, x + w - 40 - 40, cy - 20, 40, 0, UI_TEXT3);
        gfx_text_fit(g, s, x + 128, cy + 14, w - 128 - 112, ns, fp->names[first + i]);
        if (i + 1 < cnt)
            gfx_fill(s, x + 40, ry + UI_ROW_H - 2, w - 40, 2, UI_FAINT);
        fp->rows[i] = (ui_rect){ x, ry, w, UI_ROW_H };
    }

    /* Кнопки внизу */
    int bw = (w - 16) / 2;
    ui_pill(g, s, x, by, bw, BTN_H, "Новая папка", 0);
    ui_pill(g, s, x + bw + 16, by, w - bw - 16, BTN_H, "Выбрать эту папку", 1);
    fp->btn_new = (ui_rect){ x, by, bw, BTN_H };
    fp->btn_pick = (ui_rect){ x + bw + 16, by, w - bw - 16, BTN_H };
}

/* Ответ клавиатуры: создать папку внутри текущей и войти в неё. */
static void kb_done(const char *text, void *user)
{
    folderpick *fp = user;
    if (text && *text) {
        char raw[256], name[256], rel[1024], abs[1024];
        snprintf(raw, sizeof raw, "%s", text);
        raw[strcspn(raw, "\r\n")] = 0;
        net_safe_name(raw, name, sizeof name);
        int w = fp->rel[0] ? snprintf(rel, sizeof rel, "%s/%s", fp->rel, name) : snprintf(rel, sizeof rel, "%s", name);
        if (w > 0 && (size_t)w < sizeof rel && dirpath_join(fp->root, rel, abs, sizeof abs) == 0 && net_mkdirs(abs) == 0) {
            log_msg("папка: создана %s", rel);
            snprintf(fp->rel, sizeof fp->rel, "%s", rel);
            reload(fp);
        } else {
            log_msg("папка: не удалось создать «%s»", name);
        }
    }
    if (fp->redraw)
        fp->redraw(fp->user, 1); /* после экрана прошивки — полное обновление */
}

fp_result folderpick_tap(folderpick *fp, int x, int y)
{
    if (ui_inside(fp->hits.back, x, y))
        return FP_CANCEL;
    if (ui_inside(fp->hits.prev, x, y) && fp->page > 0) {
        fp->page--;
        return FP_REDRAW;
    }
    if (ui_inside(fp->hits.next, x, y)) {
        fp->page++;
        return FP_REDRAW;
    }
    if (fp->up.w && ui_inside(fp->up, x, y)) {
        go_up(fp->rel);
        reload(fp);
        return FP_REDRAW;
    }
    for (int i = 0; i < fp->nrows; i++)
        if (ui_inside(fp->rows[i], x, y)) {
            const char *name = fp->names[fp->page * fp->per_page + i];
            size_t l = strlen(fp->rel);
            if (l + strlen(name) + 2 >= sizeof fp->rel)
                return FP_NONE;
            snprintf(fp->rel + l, sizeof fp->rel - l, "%s%s", l ? "/" : "", name);
            reload(fp);
            return FP_REDRAW;
        }
    if (ui_inside(fp->btn_new, x, y)) {
        pf_screen *s = pf_get_screen();
        if (s)
            ui_flash(fp->g, s, fp->btn_new);
        pf_keyboard("Новая папка", "", 60, kb_done, fp);
        return FP_NONE;
    }
    if (ui_inside(fp->btn_pick, x, y)) {
        pf_screen *s = pf_get_screen();
        if (s)
            ui_flash(fp->g, s, fp->btn_pick);
        snprintf(fp->result, sizeof fp->result, "%s", fp->rel);
        return FP_PICKED;
    }
    return FP_NONE;
}

/* ---- dirsel ---- */

enum { DS_OFF, DS_CHOICE, DS_PICK };

struct dirsel {
    gfx *g;
    char root[1024];
    const char *shared;
    char file[1200];
    folderpick *fp;
    int state;
    char custom[1024]; /* своя папка от корня; пусто — общая */
    ui_header_hits hits;
    ui_rect b_shared, b_other, b_cancel;
};

dirsel *dirsel_new(gfx *g, const char *root, const char *hide, const char *shared, const char *file,
                   void (*redraw)(void *, int), void *user)
{
    dirsel *ds = calloc(1, sizeof *ds);
    if (!ds)
        return NULL;
    ds->g = g;
    snprintf(ds->root, sizeof ds->root, "%s", root);
    ds->shared = shared;
    snprintf(ds->file, sizeof ds->file, "%s", file);
    ds->fp = folderpick_new(g, root, hide, redraw, user);
    if (!ds->fp) {
        free(ds);
        return NULL;
    }
    return ds;
}

void dirsel_free(dirsel *ds)
{
    if (!ds)
        return;
    folderpick_free(ds->fp);
    free(ds);
}

void dirsel_load(dirsel *ds)
{
    if (!dirpath_choice_load(ds->file, ds->custom, sizeof ds->custom))
        ds->custom[0] = 0;
    ds->state = DS_OFF;
}

int dirsel_custom(const dirsel *ds)
{
    return ds->custom[0] != 0;
}

void dirsel_dir(const dirsel *ds, char *out, size_t len)
{
    if (ds->custom[0] && dirpath_join(ds->root, ds->custom, out, len) == 0)
        return;
    snprintf(out, len, "%s", ds->shared ? ds->shared : "");
}

void dirsel_label(const dirsel *ds, char *out, size_t len)
{
    if (ds->custom[0]) {
        folder_label(ds->custom, out, len);
        return;
    }
    char t[400];
    shared_label(ds->root, ds->shared, t, sizeof t);
    snprintf(out, len, "%s (общая)", t);
}

void dirsel_start(dirsel *ds)
{
    ds->state = DS_CHOICE;
}

int dirsel_active(const dirsel *ds)
{
    return ds->state != DS_OFF;
}

void dirsel_draw(dirsel *ds, pf_screen *s)
{
    if (ds->state == DS_PICK) {
        folderpick_draw(ds->fp, s);
        return;
    }
    if (ds->state != DS_CHOICE)
        return;
    gfx *g = ds->g;
    int bottom = ui_content_bottom(s);
    gfx_fill(s, 0, UI_STATUS_H, s->width, bottom - UI_STATUS_H, UI_BG);
    ds->hits = ui_header(g, s, "Куда сохранять", 1, 0, 1);
    int x = 96, w = s->width - 192, h = 760;
    int top = ui_content_top(), y = top + (bottom - top - h) / 2 - 60;
    ui_card(g, s, x, y, w, h);
    gfx_rrect_line(g, s, x, y, w, h, UI_CARD_R, 4, UI_BLACK);
    gfx_style ts = { FONT_MEDIUM, 46, UI_BLACK }, bs = { FONT_REGULAR, 36, UI_TEXT2 };
    gfx_text(g, s, x + 48, y + 100, ts, "Папка для книг");
    char lab[1200], sh[400], msg[1300];
    dirsel_label(ds, lab, sizeof lab);
    snprintf(msg, sizeof msg, "Сейчас: %s", lab);
    gfx_text_fit_middle(g, s, x + 48, y + 176, w - 96, bs, msg);
    shared_label(ds->root, ds->shared, sh, sizeof sh);
    snprintf(msg, sizeof msg, "Общая папка загрузок — %s; её меняют в «Настройках».", sh);
    gfx_text_wrap(g, s, x + 48, y + 236, w - 96, 46, 2, bs, msg);
    int bw = w - 96, by = y + 360;
    ds->b_shared = (ui_rect){ x + 48, by, bw, BTN_H };
    ui_pill(g, s, ds->b_shared.x, by, bw, BTN_H, "Общая папка загрузок", !ds->custom[0]);
    by += BTN_H + 16;
    ds->b_other = (ui_rect){ x + 48, by, bw, BTN_H };
    ui_pill(g, s, ds->b_other.x, by, bw, BTN_H, "Другая папка…", ds->custom[0] != 0);
    by += BTN_H + 16;
    ds->b_cancel = (ui_rect){ x + 48, by, bw, BTN_H };
    ui_pill(g, s, ds->b_cancel.x, by, bw, BTN_H, "Отмена", 0);
}

ds_result dirsel_tap(dirsel *ds, int x, int y)
{
    if (ds->state == DS_PICK) {
        switch (folderpick_tap(ds->fp, x, y)) {
        case FP_REDRAW: return DS_REDRAW;
        case FP_CANCEL:
            ds->state = DS_CHOICE;
            return DS_REDRAW;
        case FP_PICKED: {
            const char *rel = folderpick_result(ds->fp);
            ds->state = DS_OFF;
            char old[sizeof ds->custom];
            memcpy(old, ds->custom, sizeof old);
            snprintf(ds->custom, sizeof ds->custom, "%s", *rel ? rel : ".");
            if (dirpath_choice_save(ds->file, ds->custom) != 0)
                log_msg("папка: не удалось записать %s", ds->file);
            return strcmp(old, ds->custom) ? DS_CHANGED : DS_REDRAW;
        }
        default: return DS_NONE;
        }
    }
    if (ds->state != DS_CHOICE)
        return DS_NONE;
    pf_screen *s = pf_get_screen();
    if (ui_inside(ds->hits.back, x, y) || ui_inside(ds->b_cancel, x, y)) {
        ds->state = DS_OFF;
        return DS_REDRAW;
    }
    if (ui_inside(ds->b_shared, x, y)) {
        if (s)
            ui_flash(ds->g, s, ds->b_shared);
        int was = ds->custom[0] != 0;
        ds->custom[0] = 0;
        ds->state = DS_OFF;
        if (dirpath_choice_save(ds->file, "") != 0)
            log_msg("папка: не удалось убрать %s", ds->file);
        return was ? DS_CHANGED : DS_REDRAW;
    }
    if (ui_inside(ds->b_other, x, y)) {
        if (s)
            ui_flash(ds->g, s, ds->b_other);
        char start[1024];
        if (ds->custom[0])
            snprintf(start, sizeof start, "%s", ds->custom);
        else if (!ds->shared || dirpath_relative(ds->root, ds->shared, start, sizeof start) != 0)
            start[0] = 0;
        folderpick_open(ds->fp, "Выбор папки", start);
        ds->state = DS_PICK;
        return DS_REDRAW;
    }
    return DS_NONE;
}
