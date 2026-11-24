#include "reflow_check.h"

#include <mupdf/fitz.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { S_TEXT, S_LT, S_NAME, S_ATTRS, S_DECL, S_RAW, S_RAW_LT, S_RAW_SLASH, S_RAW_NAME, S_BANG, S_COMMENT };

void reflow_scan_init(reflow_scan *s)
{
    memset(s, 0, sizeof *s);
}

/* Теги, которые HTML5 закрывает сам (и часто не пишет </p>): в глубину не считаем. */
static int is_optional(const char *n)
{
    static const char *const list[] = { "p", "li", "dt", "dd", "tr", "td", "th", "thead", "tbody", "tfoot",
                                        "option", "optgroup", "colgroup", "rt", "rp", "html", "head", "body" };
    for (size_t i = 0; i < sizeof list / sizeof *list; i++)
        if (!strcmp(n, list[i]))
            return 1;
    return 0;
}

static int is_void(const char *n)
{
    static const char *const list[] = { "br", "img", "hr", "meta", "link", "input", "col", "area", "base",
                                        "embed", "param", "source", "track", "wbr" };
    for (size_t i = 0; i < sizeof list / sizeof *list; i++)
        if (!strcmp(n, list[i]))
            return 1;
    return 0;
}

static void end_tag(reflow_scan *s)
{
    s->name[s->nlen] = 0;
    if (!s->closing && !s->selfclose && !strcmp(s->name, "tr"))
        s->rows++;
    if (s->nlen == 0 || is_optional(s->name) || is_void(s->name))
        return;
    if (s->closing) {
        if (s->depth > 0)
            s->depth--;
        return;
    }
    if (s->selfclose)
        return;
    if (++s->depth > s->max_depth)
        s->max_depth = s->depth;
    if (!strcmp(s->name, "script") || !strcmp(s->name, "style")) {
        memcpy(s->raw, s->name, 7); /* "script"/"style" короче 7 знаков, name завершён нулём */
        s->raw[7] = 0;
        s->raw_pos = 0;
        s->state = S_RAW; /* внутри — текст: символы '<' не теги */
    }
}

void reflow_scan_feed(reflow_scan *s, const unsigned char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        switch (s->state) {
        case S_TEXT:
            if (c == '<')
                s->state = S_LT;
            break;
        case S_LT:
            s->closing = s->selfclose = s->nlen = 0;
            s->quote = 0;
            if (c == '/') {
                s->closing = 1;
                s->state = S_NAME;
            } else if (c == '!') {
                s->state = S_BANG;
                s->raw_pos = 0;
            } else if (c == '?') {
                s->state = S_DECL;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                s->name[s->nlen++] = (char)(c | 0x20);
                s->state = S_NAME;
            } else if (c == '<') {
                s->state = S_LT;
            } else {
                s->state = S_TEXT; /* «a < b» — не тег */
            }
            break;
        case S_NAME:
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '-' ||
                c == '_') {
                if (s->nlen < (int)sizeof s->name - 1)
                    s->name[s->nlen++] = (char)((c >= 'A' && c <= 'Z') ? c | 0x20 : c);
            } else if (c == '>') {
                end_tag(s);
                if (s->state == S_NAME)
                    s->state = S_TEXT;
            } else {
                s->prev = (char)c;
                s->state = S_ATTRS;
            }
            break;
        case S_ATTRS:
            if (s->quote) {
                if (c == (unsigned char)s->quote)
                    s->quote = 0;
            } else if (c == '"' || c == '\'') {
                s->quote = (char)c;
            } else if (c == '>') {
                s->selfclose = s->prev == '/';
                end_tag(s);
                if (s->state == S_ATTRS)
                    s->state = S_TEXT;
            } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                s->prev = (char)c;
            }
            break;
        case S_DECL: /* <!-- … -->, <!DOCTYPE …>, <?xml …?>: до первой '>' */
            if (c == '>')
                s->state = S_TEXT;
            break;
        case S_BANG: /* «<!» : «--» — комментарий (до «-->»), иначе объявление до '>' */
            if (c == '-' && ++s->raw_pos == 2) {
                s->state = S_COMMENT;
                s->raw_pos = 0;
            } else if (c == '>') {
                s->state = S_TEXT;
            } else if (c != '-') {
                s->state = S_DECL;
            }
            break;
        case S_COMMENT:
            if (c == '-') {
                s->raw_pos++;
            } else {
                if (c == '>' && s->raw_pos >= 2)
                    s->state = S_TEXT;
                s->raw_pos = 0;
            }
            break;
        case S_RAW:
            if (c == '<')
                s->state = S_RAW_LT;
            break;
        case S_RAW_LT:
            s->state = c == '/' ? S_RAW_NAME : (c == '<' ? S_RAW_LT : S_RAW);
            s->raw_pos = 0;
            break;
        case S_RAW_NAME:
            if (s->raw[s->raw_pos] == 0) { /* имя совпало целиком */
                if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '/') {
                    if (s->depth > 0)
                        s->depth--;
                    s->state = c == '>' ? S_TEXT : S_DECL;
                } else {
                    s->state = S_RAW;
                }
            } else if ((c | 0x20) == (unsigned char)s->raw[s->raw_pos]) {
                s->raw_pos++;
            } else {
                s->state = c == '<' ? S_RAW_LT : S_RAW;
            }
            break;
        default:
            s->state = S_TEXT;
            break;
        }
    }
}

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

static int text_entry(const char *name)
{
    const char *e = ext_of(name);
    return !strcasecmp(e, ".xhtml") || !strcasecmp(e, ".html") || !strcasecmp(e, ".htm") || !strcasecmp(e, ".xml") ||
           !strcasecmp(e, ".opf") || !strcasecmp(e, ".ncx") || !strcasecmp(e, ".fb2");
}

/* Просмотреть поток; limit — сколько байт разрешено прочитать. 0 — всё хорошо, -1 — отказ. */
static int scan_stream(fz_context *ctx, fz_stream *st, size_t limit, size_t *total, const char *what, char *err,
                       size_t err_len)
{
    reflow_scan sc;
    unsigned char buf[16384];
    size_t got = 0;
    reflow_scan_init(&sc);
    for (;;) {
        size_t n = fz_read(ctx, st, buf, sizeof buf);
        if (n == 0)
            break;
        got += n;
        *total += n;
        if (got > limit || *total > REFLOW_MAX_TOTAL) {
            snprintf(err, err_len, "слишком большой текст (%s)", what);
            return -1;
        }
        reflow_scan_feed(&sc, buf, n);
        if (sc.rows > REFLOW_MAX_ROWS) {
            snprintf(err, err_len, "слишком большая таблица (%s)", what);
            return -1;
        }
        if (sc.max_depth > REFLOW_MAX_DEPTH) {
            snprintf(err, err_len, "слишком глубокая вложенность тегов (%s)", what);
            return -1;
        }
    }
    return 0;
}

int reflow_check(const char *path, char *err, size_t err_len)
{
    const int is_epub = !strcasecmp(ext_of(path), ".epub");
    int rc = 0;
    fz_context *ctx = fz_new_context(NULL, NULL, 4 << 20);
    if (!ctx)
        return 0;
    fz_var(rc);
    fz_try(ctx)
    {
        size_t total = 0;
        if (is_epub) {
            fz_archive *arch = NULL;
            int i = 0;
            fz_var(arch);
            fz_var(i);
            fz_try(ctx) arch = fz_open_archive(ctx, path);
            fz_catch(ctx) arch = NULL; /* не архив — пусть скажет MuPDF */
            if (arch) {
                fz_try(ctx)
                {
                    int count = fz_count_archive_entries(ctx, arch);
                    if (count > REFLOW_MAX_ENTRIES) {
                        snprintf(err, err_len, "слишком много файлов в книге (%d)", count);
                        rc = -1;
                    }
                    for (i = 0; rc == 0 && i < count; i++) {
                        const char *name = fz_list_archive_entry(ctx, arch, i);
                        if (!name || !text_entry(name))
                            continue;
                        fz_stream *st = fz_open_archive_entry(ctx, arch, name);
                        fz_try(ctx) rc = scan_stream(ctx, st, REFLOW_MAX_FILE, &total, name, err, err_len);
                        fz_always(ctx) fz_drop_stream(ctx, st);
                        fz_catch(ctx) rc = 0; /* запись не читается — разберёт MuPDF */
                    }
                }
                fz_always(ctx) fz_drop_archive(ctx, arch);
                fz_catch(ctx) rc = 0;
            }
        } else {
            fz_stream *st = NULL;
            fz_var(st);
            fz_try(ctx)
            {
                st = fz_open_file(ctx, path);
                rc = scan_stream(ctx, st, REFLOW_MAX_FILE, &total, "файл", err, err_len);
            }
            fz_always(ctx) fz_drop_stream(ctx, st);
            fz_catch(ctx) rc = 0;
        }
    }
    fz_catch(ctx) rc = 0;
    fz_drop_context(ctx);
    return rc;
}
