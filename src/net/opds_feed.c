/* _GNU_SOURCE: memmem. */
#define _GNU_SOURCE
#include "opds_feed.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../doc/doc.h"
#include "net.h"

#define URL_MAX 2048
#define CAP_MAX 40000 /* сколько текста одной записи берём из ленты (до очистки от тегов) */
#define SUMMARY_MAX 3000
#define MAX_DEPTH 64

/* ---- Строки ---- */

static char *dup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *dup_s(const char *s)
{
    return dup_n(s, strlen(s));
}

static char *dup_or_empty(const char *s)
{
    return dup_s(s ? s : "");
}

/* Растущий буфер для текста. */
typedef struct {
    char *p;
    size_t len, cap;
    int full;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->len + n > CAP_MAX) {
        n = b->len < CAP_MAX ? CAP_MAX - b->len : 0;
        b->full = 1;
    }
    if (!n)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_reset(sbuf *b)
{
    b->len = 0;
    b->full = 0;
    if (b->p)
        b->p[0] = 0;
}

/* ---- Кодировки и сущности ---- */

static size_t utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static const struct {
    const char *name;
    uint32_t cp;
} entities[] = {
    { "amp", '&' },       { "lt", '<' },        { "gt", '>' },        { "quot", '"' },     { "apos", '\'' },
    { "nbsp", ' ' },      { "ndash", 0x2013 },  { "mdash", 0x2014 },  { "laquo", 0xab },   { "raquo", 0xbb },
    { "hellip", 0x2026 }, { "lsquo", 0x2018 },  { "rsquo", 0x2019 },  { "ldquo", 0x201c }, { "rdquo", 0x201d },
    { "bdquo", 0x201e },  { "bull", 0x2022 },   { "middot", 0xb7 },   { "copy", 0xa9 },    { "reg", 0xae },
    { "deg", 0xb0 },      { "times", 0xd7 },    { "shy", 0 },         { "sect", 0xa7 },    { "para", 0xb6 },
    { "euro", 0x20ac },   { "trade", 0x2122 },  { "eacute", 0xe9 },   { "egrave", 0xe8 }, { "agrave", 0xe0 },
    { "ccedil", 0xe7 },   { "ecirc", 0xea },    { "ouml", 0xf6 },     { "uuml", 0xfc },    { "auml", 0xe4 },
};

/* Сущность в s[0..n) — без «&» и «;». Возвращает число байт UTF-8 в out (до 4); 0 — не знаем. */
static size_t entity_decode(const char *s, size_t n, char *out)
{
    if (n >= 2 && s[0] == '#') {
        uint32_t cp = 0;
        size_t i = 1;
        int hex = s[1] == 'x' || s[1] == 'X';
        if (hex)
            i = 2;
        if (i >= n)
            return 0;
        for (; i < n; i++) {
            int c = (unsigned char)s[i], d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (hex && c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return 0;
            cp = cp * (hex ? 16 : 10) + (uint32_t)d;
            if (cp > 0x10ffff)
                return 0;
        }
        if (cp == 0 || cp == 0xa0)
            cp = cp ? ' ' : 0;
        return cp ? utf8_put(cp, out) : 0;
    }
    for (size_t k = 0; k < sizeof entities / sizeof *entities; k++)
        if (strlen(entities[k].name) == n && !memcmp(entities[k].name, s, n)) {
            if (!entities[k].cp)
                return (size_t)-1; /* мягкий перенос: выбросить */
            return utf8_put(entities[k].cp, out);
        }
    return 0;
}

/* Текст s[0..n) с раскрытыми сущностями — в sb. */
static void decode_into(sbuf *b, const char *s, size_t n)
{
    size_t i = 0, start = 0;
    while (i < n) {
        if (s[i] != '&') {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < n && j - i < 12 && s[j] != ';' && s[j] != '&' && s[j] != '<' && !isspace((unsigned char)s[j]))
            j++;
        char o[4];
        size_t k = j < n && s[j] == ';' ? entity_decode(s + i + 1, j - i - 1, o) : 0;
        if (!k) {
            i++;
            continue;
        }
        sb_add(b, s + start, i - start);
        if (k != (size_t)-1)
            sb_add(b, o, k);
        i = j + 1;
        start = i;
    }
    sb_add(b, s + start, n - start);
}

/* windows-1251 → Юникод для 0x80..0xff (основное; остальное — «?»). */
static uint32_t cp1251(unsigned char c)
{
    if (c >= 0xc0)
        return 0x410 + (uint32_t)(c - 0xc0);
    switch (c) {
    case 0xa8: return 0x401;
    case 0xb8: return 0x451;
    case 0xab: return 0xab;
    case 0xbb: return 0xbb;
    case 0xa0: return ' ';
    case 0xa9: return 0xa9;
    case 0xae: return 0xae;
    case 0xb0: return 0xb0;
    case 0x85: return 0x2026;
    case 0x91: return 0x2018;
    case 0x92: return 0x2019;
    case 0x93: return 0x201c;
    case 0x94: return 0x201d;
    case 0x96: return 0x2013;
    case 0x97: return 0x2014;
    case 0xb9: return 0x2116;
    case 0xb2: return 0x406;
    case 0xb3: return 0x456;
    case 0xaa: return 0x404;
    case 0xba: return 0x454;
    case 0xaf: return 0x407;
    case 0xbf: return 0x457;
    default: return '?';
    }
}

/* Кодировка из объявления <?xml … encoding="…"?>: 0 — UTF-8, 1 — windows-1251, 2 — ISO-8859-1,
 * -1 — не поддержана. */
static int xml_encoding(const char *d, size_t len)
{
    size_t n = len < 300 ? len : 300;
    char head[301];
    memcpy(head, d, n);
    head[n] = 0;
    const char *e = strstr(head, "encoding");
    if (!e || !strstr(head, "<?xml"))
        return 0;
    e += 8;
    while (*e == ' ' || *e == '=' )
        e++;
    char q = *e;
    if (q != '"' && q != '\'')
        return 0;
    e++;
    if (!strncasecmp(e, "utf-8", 5) || !strncasecmp(e, "utf8", 4) || !strncasecmp(e, "us-ascii", 8))
        return 0;
    if (!strncasecmp(e, "windows-1251", 12) || !strncasecmp(e, "cp1251", 6) || !strncasecmp(e, "cp-1251", 7))
        return 1;
    if (!strncasecmp(e, "iso-8859-1", 10) || !strncasecmp(e, "latin1", 6) || !strncasecmp(e, "windows-1252", 12))
        return 2;
    return -1;
}

/* ---- Разбор XML потоком ---- */

typedef struct {
    char *rel, *href, *type, *title, *length, *tmpl;
} attrs;

static void attrs_free(attrs *a)
{
    free(a->rel);
    free(a->href);
    free(a->type);
    free(a->title);
    free(a->length);
    free(a->tmpl);
    memset(a, 0, sizeof *a);
}

typedef struct {
    /* name — без префикса пространства имён; foreign — префикс не «atom». depth — глубина
     * (корень — 0). */
    void (*start)(void *u, const char *name, int foreign, const attrs *a, int depth);
    void (*end)(void *u, const char *name, int foreign, int depth);
    void (*text)(void *u, const char *s, size_t n);
} xml_cb;

/* Имя тега s[0..n): отделяет префикс; false — «atom:» или без префикса. */
static int split_name(const char *s, size_t n, char *name, size_t cap, int *foreign)
{
    const char *colon = memchr(s, ':', n);
    *foreign = 0;
    if (colon) {
        *foreign = !(colon - s == 4 && !memcmp(s, "atom", 4));
        n -= (size_t)(colon - s) + 1;
        s = colon + 1;
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(name, s, n);
    name[n] = 0;
    return 1;
}

static void attr_set(attrs *a, const char *name, size_t nn, const char *val, size_t vn)
{
    const char *colon = memchr(name, ':', nn);
    if (colon) {
        nn -= (size_t)(colon - name) + 1;
        name = colon + 1;
    }
    char **slot = NULL;
    if (nn == 3 && !memcmp(name, "rel", 3))
        slot = &a->rel;
    else if (nn == 4 && !memcmp(name, "href", 4))
        slot = &a->href;
    else if (nn == 4 && !memcmp(name, "type", 4))
        slot = &a->type;
    else if (nn == 5 && !memcmp(name, "title", 5))
        slot = &a->title;
    else if (nn == 6 && !memcmp(name, "length", 6))
        slot = &a->length;
    else if (nn == 8 && !memcmp(name, "template", 8))
        slot = &a->tmpl;
    if (!slot || *slot)
        return;
    sbuf b = { 0 };
    decode_into(&b, val, vn);
    *slot = b.p ? b.p : dup_s("");
}

/* Разбирает s[0..n) (UTF-8), зовёт cb. */
static void xml_run(const char *s, size_t n, const xml_cb *cb, void *u)
{
    struct {
        const char *name;
        size_t len;
    } stack[MAX_DEPTH];
    int depth = 0;
    size_t i = 0;
    sbuf text = { 0 };
    while (i < n) {
        if (s[i] != '<') {
            size_t j = i;
            while (j < n && s[j] != '<')
                j++;
            if (depth > 0 && cb->text) {
                sb_reset(&text);
                decode_into(&text, s + i, j - i);
                if (text.len)
                    cb->text(u, text.p, text.len);
            }
            i = j;
            continue;
        }
        if (i + 3 < n && !memcmp(s + i, "<!--", 4)) {
            const char *e = memmem(s + i + 4, n - i - 4, "-->", 3);
            i = e ? (size_t)(e - s) + 3 : n;
            continue;
        }
        if (i + 8 < n && !memcmp(s + i, "<![CDATA[", 9)) {
            const char *e = memmem(s + i + 9, n - i - 9, "]]>", 3);
            size_t end = e ? (size_t)(e - s) : n;
            if (depth > 0 && cb->text && end > i + 9)
                cb->text(u, s + i + 9, end - i - 9);
            i = e ? end + 3 : n;
            continue;
        }
        if (i + 1 < n && (s[i + 1] == '?' || s[i + 1] == '!')) {
            /* объявление, <!DOCTYPE …[…]>: до закрывающей «>» вне скобок */
            int br = 0;
            size_t j = i + 2;
            for (; j < n; j++) {
                if (s[j] == '[')
                    br++;
                else if (s[j] == ']' && br)
                    br--;
                else if (s[j] == '>' && !br)
                    break;
            }
            i = j < n ? j + 1 : n;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '/') {
            size_t j = i + 2, st = j;
            while (j < n && s[j] != '>')
                j++;
            size_t ne = j;
            while (ne > st && isspace((unsigned char)s[ne - 1]))
                ne--;
            /* закрыть до ближайшего открытого с таким именем */
            for (int d = depth - 1; d >= 0; d--)
                if (stack[d].len == ne - st && !memcmp(stack[d].name, s + st, ne - st)) {
                    while (depth > d) {
                        char name[48];
                        int foreign;
                        split_name(stack[depth - 1].name, stack[depth - 1].len, name, sizeof name, &foreign);
                        depth--;
                        if (cb->end)
                            cb->end(u, name, foreign, depth);
                    }
                    break;
                }
            i = j < n ? j + 1 : n;
            continue;
        }
        /* открывающий тег */
        size_t j = i + 1, st = j;
        while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>' && s[j] != '/')
            j++;
        size_t nlen = j - st;
        attrs a;
        memset(&a, 0, sizeof a);
        int selfclose = 0;
        while (j < n && s[j] != '>') {
            if (isspace((unsigned char)s[j])) {
                j++;
                continue;
            }
            if (s[j] == '/') {
                selfclose = 1;
                j++;
                continue;
            }
            size_t as = j;
            while (j < n && !isspace((unsigned char)s[j]) && s[j] != '=' && s[j] != '>' && s[j] != '/')
                j++;
            size_t ae = j;
            while (j < n && isspace((unsigned char)s[j]))
                j++;
            if (j < n && s[j] == '=') {
                j++;
                while (j < n && isspace((unsigned char)s[j]))
                    j++;
                if (j < n && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    size_t vs = j;
                    while (j < n && s[j] != q)
                        j++;
                    attr_set(&a, s + as, ae - as, s + vs, j - vs);
                    if (j < n)
                        j++;
                } else {
                    size_t vs = j;
                    while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>')
                        j++;
                    attr_set(&a, s + as, ae - as, s + vs, j - vs);
                }
            }
        }
        i = j < n ? j + 1 : n;
        if (nlen == 0 || depth >= MAX_DEPTH) {
            attrs_free(&a);
            continue;
        }
        char name[48];
        int foreign;
        split_name(s + st, nlen, name, sizeof name, &foreign);
        if (cb->start)
            cb->start(u, name, foreign, &a, depth);
        attrs_free(&a);
        if (selfclose) {
            if (cb->end)
                cb->end(u, name, foreign, depth);
        } else {
            stack[depth].name = s + st;
            stack[depth].len = nlen;
            depth++;
        }
    }
    while (depth > 0) { /* оборванный файл: закрыть открытое */
        char name[48];
        int foreign;
        split_name(stack[depth - 1].name, stack[depth - 1].len, name, sizeof name, &foreign);
        depth--;
        if (cb->end)
            cb->end(u, name, foreign, depth);
    }
    free(text.p);
}

/* Приводит ответ к UTF-8 (копия, если нужна перекодировка) и отдаёт разборщику. */
static int xml_parse_text(const char *data, size_t len, const xml_cb *cb, void *u)
{
    if (len >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) {
        data += 3;
        len -= 3;
    }
    if (len >= 2 && ((unsigned char)data[0] == 0xff || (unsigned char)data[0] == 0xfe) &&
        ((unsigned char)data[1] == 0xff || (unsigned char)data[1] == 0xfe))
        return -1; /* UTF-16 */
    int enc = xml_encoding(data, len);
    if (enc < 0)
        return -1;
    if (enc == 0) {
        xml_run(data, len, cb, u);
        return 0;
    }
    char *conv = malloc(len * 3 + 1);
    if (!conv)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x80)
            conv[o++] = (char)c;
        else
            o += utf8_put(enc == 1 ? cp1251(c) : c, conv + o);
    }
    xml_run(conv, o, cb, u);
    free(conv);
    return 0;
}

/* ---- Текст из HTML ---- */

/* Теги, после которых начинается новая строка (в аннотации — пробел); остальные (b, i, a, span) —
 * часть строки. name — без «<» и «/», n — длина. */
static int block_tag(const char *name, size_t n)
{
    static const char *const names[] = { "br", "p", "div", "li", "ul", "ol", "tr", "td", "th", "table", "blockquote",
                                         "h1", "h2", "h3", "h4", "h5", "h6", "hr", "section", "pre" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (strlen(names[i]) == n && !strncasecmp(names[i], name, n))
            return 1;
    return 0;
}

/* Аннотация: теги и сущности убираются, пробелы сворачиваются. */
static char *html_to_text(const char *in, size_t max_out)
{
    size_t n = strlen(in);
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    int space = 1; /* перед этим пробел или начало */
    for (size_t i = 0; i < n && o < max_out;) {
        unsigned char c = (unsigned char)in[i];
        if (c == '<' && i + 1 < n && (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '/' || in[i + 1] == '!')) {
            const char *e = memchr(in + i, '>', n - i < 600 ? n - i : 600);
            if (e) {
                const char *nm = in + i + 1 + (in[i + 1] == '/');
                size_t nl = 0;
                while (nm + nl < e && isalnum((unsigned char)nm[nl]))
                    nl++;
                if (!space && block_tag(nm, nl)) {
                    out[o++] = ' ';
                    space = 1;
                }
                i = (size_t)(e - in) + 1;
                continue;
            }
        }
        if (c == '[' && i + 2 < n) { /* BBCode: [b] [/b] [i] [u] [p] */
            size_t k = i + 1;
            if (in[k] == '/')
                k++;
            if (k + 1 < n && strchr("biup", in[k]) && in[k + 1] == ']') {
                i = k + 2;
                continue;
            }
        }
        if (c == '&') {
            size_t j = i + 1;
            while (j < n && j - i < 12 && in[j] != ';' && in[j] != '&' && !isspace((unsigned char)in[j]))
                j++;
            char b[4];
            size_t k = j < n && in[j] == ';' ? entity_decode(in + i + 1, j - i - 1, b) : 0;
            if (k) {
                i = j + 1;
                if (k == (size_t)-1)
                    continue;
                if (k == 1 && (b[0] == ' ' || b[0] == '\n')) {
                    if (!space)
                        out[o++] = ' ';
                    space = 1;
                    continue;
                }
                memcpy(out + o, b, k);
                o += k;
                space = 0;
                continue;
            }
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || (c == 0xc2 && i + 1 < n && (unsigned char)in[i + 1] == 0xa0)) {
            if (!space)
                out[o++] = ' ';
            space = 1;
            i += c == 0xc2 ? 2 : 1;
            continue;
        }
        out[o++] = (char)c;
        space = 0;
        i++;
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
    /* не резать UTF-8 посередине, если упёрлись в предел */
    if (o >= max_out) {
        while (o && ((unsigned char)out[o] & 0xc0) == 0x80)
            o--;
        out[o] = 0;
    }
    return out;
}

/* Название и автор: пробелы по краям и внутри сворачиваются. */
static char *plain_text(const char *in)
{
    char *t = html_to_text(in, 1000);
    return t ? t : dup_s("");
}

/* ---- Лента ---- */

enum { CAP_NONE, CAP_FEED_TITLE, CAP_TITLE, CAP_ID, CAP_SUMMARY, CAP_CONTENT, CAP_AUTHOR };

typedef struct {
    opds_feed *f;
    const char *base;
    int is_feed;
    int in_entry;
    opds_entry cur;
    int links_cap;
    sbuf cap;
    int cap_kind, cap_depth;
    char *summary, *content, *author_acc;
    int fail;
    int in_author; /* depth+1 тега author, 0 — не внутри */
} fctx;

static void resolve_href(fctx *c, const char *href, char *out, size_t len)
{
    net_url_resolve(c->base, href, out, len);
}

static int bad_href(const char *h)
{
    return !*h || !strncasecmp(h, "data:", 5) || !strncasecmp(h, "javascript:", 11);
}

static void link_add(fctx *c, const attrs *a, opds_link **arr, int *n, int *cap)
{
    if (!a->href || bad_href(a->href))
        return;
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 8;
        opds_link *p = realloc(*arr, (size_t)nc * sizeof **arr);
        if (!p) {
            c->fail = 1;
            return;
        }
        *arr = p;
        *cap = nc;
    }
    char url[URL_MAX];
    resolve_href(c, a->href, url, sizeof url);
    opds_link *l = &(*arr)[(*n)++];
    l->rel = dup_or_empty(a->rel);
    l->href = dup_s(url);
    l->type = dup_or_empty(a->type);
    l->title = dup_or_empty(a->title);
    l->length = a->length ? atoll(a->length) : -1;
    if (l->length <= 0)
        l->length = -1;
    c->f->bytes += (long long)(sizeof *l + strlen(url) + strlen(l->rel) + strlen(l->type) + strlen(l->title));
}

static int rel_is(const char *rel, const char *want)
{
    return !strcasecmp(rel, want);
}

static void feed_link(fctx *c, const attrs *a)
{
    opds_feed *f = c->f;
    const char *rel = a->rel ? a->rel : "", *type = a->type ? a->type : "", *href = a->href;
    if (!href || bad_href(href))
        return;
    char url[URL_MAX];
    if (rel_is(rel, "next") && !*f->next) {
        resolve_href(c, href, url, sizeof url);
        free(f->next);
        f->next = dup_s(url);
    } else if ((rel_is(rel, "previous") || rel_is(rel, "prev")) && !*f->prev) {
        resolve_href(c, href, url, sizeof url);
        free(f->prev);
        f->prev = dup_s(url);
    } else if (rel_is(rel, "search")) {
        if (strstr(type, "opensearchdescription")) {
            if (!*f->search_desc) {
                resolve_href(c, href, url, sizeof url);
                free(f->search_desc);
                f->search_desc = dup_s(url);
            }
        } else if (strstr(href, "{searchTerms}") || strstr(href, "%7BsearchTerms%7D")) {
            if (!*f->search) {
                resolve_href(c, href, url, sizeof url);
                free(f->search);
                f->search = dup_s(url);
            }
        }
    }
}

static void entry_reset(fctx *c)
{
    memset(&c->cur, 0, sizeof c->cur);
    c->links_cap = 0;
    free(c->summary);
    free(c->content);
    free(c->author_acc);
    c->summary = c->content = c->author_acc = NULL;
}

static void entry_finish(fctx *c)
{
    opds_entry *e = &c->cur;
    opds_feed *f = c->f;
    /* без названия и без ссылок запись ни к чему */
    if (!e->nlinks && !e->title) {
        free(e->id);
        free(e->title);
        free(e->links);
        entry_reset(c);
        return;
    }
    const char *sum = c->summary && *c->summary ? c->summary : c->content;
    e->summary = sum ? html_to_text(sum, SUMMARY_MAX) : dup_s("");
    e->author = dup_or_empty(c->author_acc);
    if (!e->title)
        e->title = dup_s("");
    if (!e->id)
        e->id = dup_s("");
    opds_entry *p = realloc(f->entries, (size_t)(f->nentries + 1) * sizeof *p);
    if (!p) {
        c->fail = 1;
        return;
    }
    f->entries = p;
    f->entries[f->nentries++] = *e;
    f->bytes += (long long)(sizeof *e + strlen(e->title) + strlen(e->summary) + strlen(e->author) + strlen(e->id));
    free(c->summary);
    free(c->content);
    free(c->author_acc);
    c->summary = c->content = c->author_acc = NULL;
    memset(&c->cur, 0, sizeof c->cur);
    c->links_cap = 0;
}

static void f_start(void *u, const char *name, int foreign, const attrs *a, int depth)
{
    fctx *c = u;
    if (c->cap_kind) { /* внутри content type="xhtml": абзацы и переносы — разделители */
        if (block_tag(name, strlen(name)))
            sb_add(&c->cap, " ", 1);
        return;
    }
    if (foreign)
        return;
    if (depth == 0) {
        c->is_feed = !strcmp(name, "feed");
        return;
    }
    if (!c->is_feed)
        return;
    if (depth == 1) {
        if (!strcmp(name, "entry")) {
            c->in_entry = 1;
            entry_reset(c);
        } else if (!strcmp(name, "title")) {
            sb_reset(&c->cap);
            c->cap_kind = CAP_FEED_TITLE;
            c->cap_depth = depth;
        } else if (!strcmp(name, "link")) {
            feed_link(c, a);
        }
        return;
    }
    if (depth == 2 && c->in_entry) {
        int kind = CAP_NONE;
        if (!strcmp(name, "title"))
            kind = CAP_TITLE;
        else if (!strcmp(name, "id"))
            kind = CAP_ID;
        else if (!strcmp(name, "summary"))
            kind = CAP_SUMMARY;
        else if (!strcmp(name, "content"))
            kind = CAP_CONTENT;
        else if (!strcmp(name, "link"))
            link_add(c, a, &c->cur.links, &c->cur.nlinks, &c->links_cap);
        else if (!strcmp(name, "author"))
            c->in_author = depth + 1;
        if (kind) {
            sb_reset(&c->cap);
            c->cap_kind = kind;
            c->cap_depth = depth;
        }
        return;
    }
    if (depth == 3 && c->in_author && !strcmp(name, "name")) {
        sb_reset(&c->cap);
        c->cap_kind = CAP_AUTHOR;
        c->cap_depth = depth;
    }
}

static void f_text(void *u, const char *s, size_t n)
{
    fctx *c = u;
    if (c->cap_kind)
        sb_add(&c->cap, s, n);
}

static void f_end(void *u, const char *name, int foreign, int depth)
{
    fctx *c = u;
    (void)foreign;
    if (c->cap_kind) {
        if (depth > c->cap_depth) {
            if (block_tag(name, strlen(name)))
                sb_add(&c->cap, " ", 1);
            return;
        }
        if (depth == c->cap_depth) {
            const char *t = c->cap.p ? c->cap.p : "";
            switch (c->cap_kind) {
            case CAP_FEED_TITLE:
                free(c->f->title);
                c->f->title = plain_text(t);
                break;
            case CAP_TITLE:
                free(c->cur.title);
                c->cur.title = plain_text(t);
                break;
            case CAP_ID:
                free(c->cur.id);
                c->cur.id = dup_n(t, strlen(t) < 300 ? strlen(t) : 300);
                break;
            case CAP_SUMMARY:
                free(c->summary);
                c->summary = dup_s(t);
                break;
            case CAP_CONTENT:
                free(c->content);
                c->content = dup_s(t);
                break;
            case CAP_AUTHOR: {
                char *who = plain_text(t);
                if (who && *who) {
                    size_t have = c->author_acc ? strlen(c->author_acc) : 0;
                    char *p = realloc(c->author_acc, have + strlen(who) + 3);
                    if (p) {
                        if (!have)
                            p[0] = 0;
                        else
                            strcat(p, ", ");
                        strcat(p, who);
                        c->author_acc = p;
                    }
                }
                free(who);
                break;
            }
            }
            c->cap_kind = CAP_NONE;
        }
        return;
    }
    if (depth == 1 && c->in_entry && !strcmp(name, "entry")) {
        c->in_entry = 0;
        c->in_author = 0;
        entry_finish(c);
    } else if (c->in_author && depth + 1 == c->in_author) {
        c->in_author = 0;
    }
}

opds_feed *opds_feed_parse(const char *data, size_t len, const char *base)
{
    opds_feed *f = calloc(1, sizeof *f);
    fctx *c = calloc(1, sizeof *c);
    if (!f || !c) {
        free(f);
        free(c);
        return NULL;
    }
    c->f = f;
    c->base = base ? base : "";
    f->title = dup_s("");
    f->next = dup_s("");
    f->prev = dup_s("");
    f->search = dup_s("");
    f->search_desc = dup_s("");
    xml_cb cb = { f_start, f_end, f_text };
    int rc = xml_parse_text(data, len, &cb, c);
    int ok = rc == 0 && c->is_feed && !c->fail;
    if (c->in_entry) /* оборванная запись — как есть */
        entry_finish(c);
    entry_reset(c);
    free(c->cap.p);
    free(c);
    if (!ok) {
        opds_feed_free(f);
        return NULL;
    }
    return f;
}

void opds_feed_free(opds_feed *f)
{
    if (!f)
        return;
    for (int i = 0; i < f->nentries; i++) {
        opds_entry *e = &f->entries[i];
        free(e->id);
        free(e->title);
        free(e->author);
        free(e->summary);
        for (int k = 0; k < e->nlinks; k++) {
            free(e->links[k].rel);
            free(e->links[k].href);
            free(e->links[k].type);
            free(e->links[k].title);
        }
        free(e->links);
    }
    free(f->entries);
    free(f->title);
    free(f->next);
    free(f->prev);
    free(f->search);
    free(f->search_desc);
    free(f);
}

int opds_looks_like_xml(const char *data, size_t len, int *json, int *html)
{
    if (json)
        *json = 0;
    if (html)
        *html = 0;
    size_t i = 0;
    if (len >= 3 && !memcmp(data, "\xef\xbb\xbf", 3))
        i = 3;
    while (i < len && isspace((unsigned char)data[i]))
        i++;
    if (i >= len)
        return 0;
    if (data[i] == '{' || data[i] == '[') {
        if (json)
            *json = 1;
        return 0;
    }
    if (data[i] != '<')
        return 0;
    size_t n = len - i < 400 ? len - i : 400;
    char head[401];
    memcpy(head, data + i, n);
    head[n] = 0;
    if (!strncasecmp(head, "<!doctype html", 14) || !strncasecmp(head, "<html", 5)) {
        if (html)
            *html = 1;
        return 0;
    }
    return 1;
}

/* ---- OpenSearch ---- */

typedef struct {
    int best;
    char tmpl[URL_MAX];
} osd_ctx;

static void o_start(void *u, const char *name, int foreign, const attrs *a, int depth)
{
    osd_ctx *c = u;
    (void)foreign;
    (void)depth;
    if (strcmp(name, "Url") || !a->tmpl)
        return;
    const char *type = a->type ? a->type : "";
    int score = 0;
    if (strstr(type, "atom+xml"))
        score = strstr(type, "opds-catalog") ? 3 : 2;
    else if (strstr(type, "xml") && !strstr(type, "opensearch"))
        score = 1;
    if (a->rel && *a->rel && strcmp(a->rel, "results") && strcmp(a->rel, "http://opds-spec.org/search"))
        score = 0;
    if (score > c->best) {
        c->best = score;
        snprintf(c->tmpl, sizeof c->tmpl, "%s", a->tmpl);
    }
}

int opds_osd_template(const char *data, size_t len, const char *base, char *out, size_t out_len)
{
    osd_ctx *c = calloc(1, sizeof *c);
    if (!c)
        return -1;
    xml_cb cb = { o_start, NULL, NULL };
    int rc = xml_parse_text(data, len, &cb, c);
    int ok = rc == 0 && c->best > 0 && strstr(c->tmpl, "searchTerms");
    if (ok)
        net_url_resolve(base, c->tmpl, out, out_len);
    free(c);
    return ok ? 0 : -1;
}

void opds_search_url(const char *tmpl, const char *terms, char *out, size_t out_len)
{
    size_t o = 0;
    char enc[768];
    net_url_encode(terms, 0, enc, sizeof enc);
    if (!out_len)
        return;
    for (const char *p = tmpl; *p && o + 1 < out_len;) {
        const char *val = NULL;
        size_t skip = 0;
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (e) {
                char name[48];
                size_t n = (size_t)(e - p - 1);
                if (n >= sizeof name)
                    n = sizeof name - 1;
                memcpy(name, p + 1, n);
                name[n] = 0;
                char *q = strchr(name, '?');
                if (q)
                    *q = 0;
                char *colon = strchr(name, ':');
                const char *nm = colon ? colon + 1 : name;
                if (!strcmp(nm, "searchTerms"))
                    val = enc;
                else if (q)
                    val = "";
                else if (!strcmp(nm, "startPage") || !strcmp(nm, "startIndex"))
                    val = "1";
                else if (!strcmp(nm, "count"))
                    val = "20";
                else
                    val = "";
                skip = (size_t)(e - p) + 1;
            }
        } else if (!strncmp(p, "%7BsearchTerms%7D", 17)) {
            val = enc;
            skip = 17;
        }
        if (val) {
            for (const char *v = val; *v && o + 1 < out_len; v++)
                out[o++] = *v;
            p += skip;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

/* ---- Записи ---- */

static int starts_with(const char *s, const char *prefix)
{
    return !strncmp(s, prefix, strlen(prefix));
}

static int is_acquisition(const opds_link *l)
{
    if (!starts_with(l->rel, "http://opds-spec.org/acquisition"))
        return 0;
    const char *tail = l->rel + strlen("http://opds-spec.org/acquisition");
    return !strcmp(tail, "") || !strcmp(tail, "/open-access");
}

int opds_entry_is_book(const opds_entry *e)
{
    for (int i = 0; i < e->nlinks; i++)
        if (is_acquisition(&e->links[i]))
            return 1;
    return 0;
}

const opds_link *opds_entry_nav(const opds_entry *e)
{
    const opds_link *weak = NULL;
    for (int i = 0; i < e->nlinks; i++) {
        const opds_link *l = &e->links[i];
        int atom = strstr(l->type, "atom+xml") != NULL;
        if (!atom && !(*l->type == 0 && !strcmp(l->rel, "subsection")))
            continue;
        if (!strcmp(l->rel, "alternate") || !strcmp(l->rel, "self"))
            continue;
        if (!strcmp(l->rel, "related") || !strcmp(l->rel, "up") || !strcmp(l->rel, "start")) {
            if (!weak)
                weak = l;
            continue;
        }
        return l;
    }
    return weak;
}

const char *opds_entry_cover(const opds_entry *e, int thumbnail)
{
    const char *img = NULL, *thumb = NULL;
    for (int i = 0; i < e->nlinks; i++) {
        const opds_link *l = &e->links[i];
        if (!strcmp(l->rel, "http://opds-spec.org/image/thumbnail") || !strcmp(l->rel, "http://opds-spec.org/thumbnail") ||
            !strcmp(l->rel, "x-stanza-cover-image-thumbnail"))
            thumb = thumb ? thumb : l->href;
        else if (!strcmp(l->rel, "http://opds-spec.org/image") || !strcmp(l->rel, "x-stanza-cover-image"))
            img = img ? img : l->href;
    }
    if (thumbnail)
        return thumb ? thumb : img;
    return img ? img : thumb;
}

/* ---- Форматы ---- */

static const struct {
    const char *mime, *ext;
} mimes[] = {
    { "application/epub+zip", ".epub" },
    { "application/x-fictionbook+xml", ".fb2" },
    { "application/fb2", ".fb2" },
    { "text/fb2+xml", ".fb2" },
    { "application/x-fictionbook", ".fb2" },
    { "application/fb2+zip", ".fb2.zip" },
    { "application/x-zip-compressed-fb2", ".fb2.zip" },
    { "application/x-fictionbook+zip", ".fb2.zip" },
    { "application/pdf", ".pdf" },
    { "application/x-pdf", ".pdf" },
    { "image/vnd.djvu", ".djvu" },
    { "image/x-djvu", ".djvu" },
    { "image/djvu", ".djvu" },
    { "application/djvu", ".djvu" },
    { "application/x-mobipocket-ebook", ".mobi" },
    { "application/x-mobi8-ebook", ".azw3" },
    { "application/vnd.amazon.mobi8-ebook", ".azw3" },
    { "application/vnd.amazon.ebook", ".azw3" },
    { "application/x-cbz", ".cbz" },
    { "application/vnd.comicbook+zip", ".cbz" },
    { "application/x-cbr", ".cbr" },
    { "application/vnd.comicbook-rar", ".cbr" },
    { "application/html+zip", ".html.zip" },
    { "application/txt+zip", ".txt.zip" },
    { "application/rtf+zip", ".rtf.zip" },
    { "application/rtf", ".rtf" },
    { "text/rtf", ".rtf" },
    { "text/plain", ".txt" },
    { "application/msword", ".doc" },
    { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx" },
};

const char *opds_ext_for_type(const char *mime)
{
    size_t n = strcspn(mime, "; ");
    for (size_t i = 0; i < sizeof mimes / sizeof *mimes; i++)
        if (strlen(mimes[i].mime) == n && !strncasecmp(mimes[i].mime, mime, n))
            return mimes[i].ext;
    return "";
}

static const char *const path_exts[] = { ".fb2.zip", ".html.zip", ".txt.zip", ".rtf.zip", ".epub", ".fb2", ".pdf", ".djvu", ".djv", ".mobi", ".azw3",
                                         ".azw", ".cbz", ".cbr", ".rtf", ".txt", ".docx", ".doc" };

const char *opds_ext_for_path(const char *path)
{
    size_t n = strcspn(path, "?#");
    for (size_t i = 0; i < sizeof path_exts / sizeof *path_exts; i++) {
        size_t l = strlen(path_exts[i]);
        if (n > l && !strncasecmp(path + n - l, path_exts[i], l))
            return !strcmp(path_exts[i], ".djv") ? ".djvu" : path_exts[i];
    }
    return "";
}

const char *opds_ext_sniff(const unsigned char *h, size_t len)
{
    if (len >= 5 && !memcmp(h, "%PDF-", 5))
        return ".pdf";
    if (len >= 8 && !memcmp(h, "AT&TFORM", 8))
        return ".djvu";
    if (len >= 4 && !memcmp(h, "PK\3\4", 4)) {
        if (len >= 58 && !memcmp(h + 30, "mimetype", 8) && !memcmp(h + 38, "application/epub+zip", 20))
            return ".epub";
        return ".zip";
    }
    if (len >= 68 && !memcmp(h + 60, "BOOKMOBI", 8))
        return ".mobi";
    if (len >= 5 && !memcmp(h, "<?xml", 5)) {
        size_t n = len < 2048 ? len : 2048;
        if (memmem(h, n, "FictionBook", 11))
            return ".fb2";
    }
    return "";
}

/* Порядок: что открывает наша читалка; внутри — EPUB, FB2, PDF; потом DjVu, MOBI… */
static int ext_rank(const char *ext)
{
    static const char *const order[] = { ".epub", ".fb2", ".pdf", ".djvu", ".mobi", ".azw3", ".fb2.zip",
                                         ".cbz",  ".cbr", ".rtf", ".docx", ".doc", ".txt" };
    for (size_t i = 0; i < sizeof order / sizeof *order; i++)
        if (!strcmp(ext, order[i])) {
            char probe[24];
            snprintf(probe, sizeof probe, "book%s", ext);
            return (int)i + (doc_supported(probe) ? 0 : 20);
        }
    return 60;
}

static int skip_type(const char *type)
{
    return starts_with(type, "text/html") || starts_with(type, "application/atom+xml") ||
           starts_with(type, "application/xml") || starts_with(type, "application/zip") ||
           starts_with(type, "application/x-zip") || starts_with(type, "image/jpeg") ||
           starts_with(type, "image/png") || starts_with(type, "application/opds");
}

int opds_formats(const opds_entry *e, opds_format *out, int max)
{
    int n = 0;
    for (int i = 0; i < e->nlinks && n < max; i++) {
        const opds_link *l = &e->links[i];
        if (!is_acquisition(l))
            continue;
        const char *ext = opds_ext_for_type(l->type);
        if (!*ext)
            ext = opds_ext_for_path(l->href);
        if (!*ext && skip_type(l->type))
            continue;
        opds_format f;
        memset(&f, 0, sizeof f);
        f.link = l;
        if (*ext) {
            snprintf(f.ext, sizeof f.ext, "%s", ext);
            for (const char *p = ext + 1; *p && strlen(f.label) + 1 < sizeof f.label; p++)
                f.label[strlen(f.label)] = (char)toupper((unsigned char)*p);
        } else {
            snprintf(f.ext, sizeof f.ext, ".bin");
            snprintf(f.label, sizeof f.label, "Файл");
        }
        f.rank = ext_rank(f.ext);
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (!strcmp(out[k].ext, f.ext))
                dup = 1;
        if (dup)
            continue;
        /* вставка по рангу, стабильно */
        int at = n;
        while (at > 0 && out[at - 1].rank > f.rank) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = f;
        n++;
    }
    return n;
}
