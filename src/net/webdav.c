/* _DEFAULT_SOURCE: timegm, strcasecmp. */
#define _DEFAULT_SOURCE
#include "webdav.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "net.h"

const char webdav_propfind_body[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
    "<d:resourcetype/><d:getcontentlength/><d:getlastmodified/>"
    "</d:prop></d:propfind>";

long long webdav_parse_date(const char *s)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    struct tm t;
    memset(&t, 0, sizeof t);
    int d, y, h, mi, se, mo = 0;
    char mon[4];
    const char *c = strchr(s, ',');
    if (sscanf(c ? c + 1 : s, " %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) == 6) {
        int found = 0;
        for (int i = 0; i < 12; i++)
            if (!strncasecmp(months + i * 3, mon, 3)) {
                mo = i;
                found = 1;
            }
        if (!found)
            return 0;
    } else if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6) {
        mo--;
    } else {
        return 0;
    }
    t.tm_mon = mo;
    t.tm_mday = d;
    t.tm_year = y - 1900;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = se;
    time_t r = timegm(&t);
    return r < 0 ? 0 : (long long)r;
}

/* ---- XML ---- */

/* Добавить в out (cap байт) text длиной n с заменой &amp; &lt; &gt; &quot; &apos; &#N; &#xH;. */
static void append_unescaped(char *out, size_t cap, const char *text, size_t n)
{
    size_t o = strlen(out);
    for (size_t i = 0; i < n && o + 5 < cap; i++) {
        if (text[i] != '&') {
            out[o++] = text[i];
            continue;
        }
        const char *semi = memchr(text + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) {
            out[o++] = '&';
            continue;
        }
        size_t k = (size_t)(semi - (text + i)) - 1; /* длина между & и ; */
        const char *e = text + i + 1;
        if (k == 3 && !strncmp(e, "amp", 3))
            out[o++] = '&';
        else if (k == 2 && !strncmp(e, "lt", 2))
            out[o++] = '<';
        else if (k == 2 && !strncmp(e, "gt", 2))
            out[o++] = '>';
        else if (k == 4 && !strncmp(e, "quot", 4))
            out[o++] = '"';
        else if (k == 4 && !strncmp(e, "apos", 4))
            out[o++] = '\'';
        else if (k >= 2 && e[0] == '#') {
            unsigned long cp = e[1] == 'x' || e[1] == 'X' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
            if (cp < 0x80) {
                out[o++] = (char)cp;
            } else if (cp < 0x800) {
                out[o++] = (char)(0xC0 | cp >> 6);
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[o++] = (char)(0xE0 | cp >> 12);
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x110000) {
                out[o++] = (char)(0xF0 | cp >> 18);
                out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
        } else {
            out[o++] = '&';
            continue;
        }
        i += k + 1;
    }
    out[o] = 0;
}

typedef struct {
    char path[2048]; /* раскодированный путь, без «/» в конце */
    int is_dir;
    long long size, mtime;
} item;

typedef struct {
    item *v;
    int n, cap;
} items;

/* Из href — путь: без схемы и хоста, без ?запроса и #, %XX раскодированы, без «/» в конце. */
static int href_to_path(const char *href, item *it)
{
    while (isspace((unsigned char)*href))
        href++;
    const char *p = href;
    const char *scheme = strstr(p, "://");
    if (scheme && scheme < p + 12) {
        p = strchr(scheme + 3, '/');
        if (!p)
            p = "";
    }
    char raw[2048];
    snprintf(raw, sizeof raw, "%.*s", (int)strcspn(p, "?#"), p);
    char dec[2048];
    net_url_decode(raw, dec, sizeof dec);
    size_t n = strlen(dec);
    while (n && isspace((unsigned char)dec[n - 1]))
        dec[--n] = 0;
    while (n > 0 && dec[n - 1] == '/') {
        dec[--n] = 0;
        it->is_dir = 1;
    }
    snprintf(it->path, sizeof it->path, "%s", dec);
    return 0;
}

static int items_add(items *l, const item *it)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        item *v = realloc(l->v, sizeof *v * (size_t)cap);
        if (!v)
            return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = *it;
    return 0;
}

enum { W_NONE, W_HREF, W_SIZE, W_DATE };

/* Имя тега без префикса пространства имён, в нижнем регистре. */
static size_t local_name(const char *tag, size_t n, char *out, size_t len)
{
    size_t i = 0;
    while (i < n && !isspace((unsigned char)tag[i]) && tag[i] != '/' && tag[i] != '>')
        i++;
    size_t start = 0;
    for (size_t k = 0; k < i; k++)
        if (tag[k] == ':')
            start = k + 1;
    size_t o = 0;
    for (size_t k = start; k < i && o + 1 < len; k++)
        out[o++] = (char)tolower((unsigned char)tag[k]);
    out[o] = 0;
    return o;
}

int webdav_parse(const char *xml, size_t len, const char *dir_path, remote_entries *out)
{
    items all = { NULL, 0, 0 };
    const char *p = xml, *end = xml + len;
    int want = W_NONE, in_resp = 0, seen_resp = 0;
    char href[2048] = "", sz[64] = "", date[96] = "";
    int coll = 0;
    int rc = 0;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt)
            break;
        if (want != W_NONE) {
            char *dst = want == W_HREF ? href : want == W_SIZE ? sz : date;
            size_t cap = want == W_HREF ? sizeof href : want == W_SIZE ? sizeof sz : sizeof date;
            append_unescaped(dst, cap, p, (size_t)(lt - p));
        }
        if (end - lt >= 4 && !memcmp(lt, "<!--", 4)) {
            const char *e = strstr(lt + 4, "-->");
            p = e && e < end ? e + 3 : end;
            continue;
        }
        if (end - lt >= 9 && !memcmp(lt, "<![CDATA[", 9)) {
            const char *e = lt + 9;
            while (e + 3 <= end && memcmp(e, "]]>", 3))
                e++;
            if (want != W_NONE) {
                char *dst = want == W_HREF ? href : want == W_SIZE ? sz : date;
                size_t cap = want == W_HREF ? sizeof href : want == W_SIZE ? sizeof sz : sizeof date;
                size_t o = strlen(dst), n = (size_t)(e - (lt + 9));
                if (o + n + 1 > cap)
                    n = cap - o - 1;
                memcpy(dst + o, lt + 9, n);
                dst[o + n] = 0;
            }
            p = e + 3 <= end ? e + 3 : end;
            continue;
        }
        const char *gt = memchr(lt, '>', (size_t)(end - lt));
        if (!gt)
            break;
        if (lt[1] == '?' || lt[1] == '!') {
            p = gt + 1;
            continue;
        }
        int closing = lt[1] == '/';
        char name[64];
        local_name(lt + 1 + closing, (size_t)(gt - lt - 1 - closing), name, sizeof name);
        if (closing) {
            want = W_NONE;
            if (!strcmp(name, "response") && in_resp) {
                in_resp = 0;
                item it;
                memset(&it, 0, sizeof it);
                it.size = -1;
                href_to_path(href, &it);
                if (coll)
                    it.is_dir = 1;
                if (!it.is_dir && sz[0])
                    it.size = atoll(sz);
                it.mtime = date[0] ? webdav_parse_date(date) : 0;
                if (items_add(&all, &it) != 0) {
                    rc = -1;
                    break;
                }
            }
        } else if (!strcmp(name, "response")) {
            in_resp = 1;
            seen_resp = 1;
            href[0] = sz[0] = date[0] = 0;
            coll = 0;
        } else if (in_resp) {
            int self_closed = gt[-1] == '/';
            if (!strcmp(name, "href") && !self_closed)
                want = W_HREF;
            else if (!strcmp(name, "getcontentlength") && !self_closed)
                want = W_SIZE;
            else if (!strcmp(name, "getlastmodified") && !self_closed)
                want = W_DATE;
            else if (!strcmp(name, "collection"))
                coll = 1;
        }
        p = gt + 1;
    }
    if (rc != 0 || !seen_resp) {
        free(all.v);
        return -1;
    }

    size_t dl = strlen(dir_path);
    int matched = 0, before = out->count;
    for (int i = 0; i < all.n; i++) {
        const item *it = &all.v[i];
        if (strncmp(it->path, dir_path, dl) || it->path[dl] != '/' || !it->path[dl + 1])
            continue;
        const char *nm = it->path + dl + 1;
        if (strchr(nm, '/'))
            continue; /* глубже, чем Depth: 1 */
        matched++;
        remote_entries_add(out, nm, it->is_dir, it->size, it->mtime);
    }
    if (!matched && all.n > 1) {
        for (int i = 1; i < all.n; i++) {
            const item *it = &all.v[i];
            const char *nm = strrchr(it->path, '/');
            nm = nm ? nm + 1 : it->path;
            if (*nm)
                remote_entries_add(out, nm, it->is_dir, it->size, it->mtime);
        }
    }
    free(all.v);
    return out->count - before;
}
