/* opds_feed: разбор лент OPDS (файлы tests/net/opds/, несколько лент прямо здесь), форматы,
 * шаблоны поиска. Аргумент — папка с лентами. */
#include "../../src/net/opds_feed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *pf_data_dir(void) /* net.c ищет там cacert.pem */
{
    return "/nonexistent";
}

static int fails;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("НЕ ТАК (строка %d): %s\n", __LINE__, #cond);             \
            fails++;                                                         \
        }                                                                    \
    } while (0)

static void eq(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("НЕ ТАК %s: «%s», ждали «%s»\n", what, got, want);
        fails++;
    }
}

static char *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("нет файла %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        exit(2);
    buf[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static opds_feed *load(const char *dir, const char *name, const char *base)
{
    size_t n;
    char *d = slurp(dir, name, &n);
    opds_feed *f = opds_feed_parse(d, n, base);
    free(d);
    if (!f) {
        printf("не разобралась лента %s\n", name);
        fails++;
    }
    return f;
}

static const opds_entry *find(const opds_feed *f, const char *title)
{
    for (int i = 0; i < f->nentries; i++)
        if (!strcmp(f->entries[i].title, title))
            return &f->entries[i];
    printf("нет записи «%s»\n", title);
    fails++;
    return NULL;
}

static void test_root(const char *dir)
{
    opds_feed *f = load(dir, "root.xml", "http://h:8099/opds/");
    if (!f)
        return;
    eq("title", f->title, "Тестовый каталог");
    CHECK(f->nentries == 4);
    eq("search", f->search, "http://h:8099/opds/search?q={searchTerms}");
    eq("next", f->next, "");
    const opds_entry *e = find(f, "Новинки");
    if (e) {
        CHECK(!opds_entry_is_book(e));
        const opds_link *l = opds_entry_nav(e);
        CHECK(l != NULL);
        if (l)
            eq("nav", l->href, "http://h:8099/opds/new");
        eq("summary", e->summary, "Свежие поступления: 14 книг на двух страницах");
    }
    e = find(f, "Пустой раздел");
    if (e && opds_entry_nav(e))
        eq("subsection", opds_entry_nav(e)->href, "http://h:8099/opds/empty");
    opds_feed_free(f);
}

static void test_genres(const char *dir)
{
    /* префикс atom:, чужой dc:title пропускается, поиск — через описание OpenSearch */
    opds_feed *f = load(dir, "genres.xml", "http://h/opds/genres");
    if (!f)
        return;
    eq("title", f->title, "Разделы");
    eq("search_desc", f->search_desc, "http://h/opds/osd.xml");
    eq("search", f->search, "");
    CHECK(f->nentries == 2);
    const opds_entry *e = find(f, "Проза");
    if (e)
        eq("summary", e->summary, "Романы и повести");
    opds_feed_free(f);

    size_t n;
    char *osd = slurp(dir, "osd.xml", &n);
    char t[512];
    CHECK(opds_osd_template(osd, n, "http://h/opds/genres", t, sizeof t) == 0);
    eq("osd", t, "http://h/opds/search?q={searchTerms}&page={startPage?}");
    char url[512];
    opds_search_url(t, "война и мир", url, sizeof url);
    eq("search url", url, "http://h/opds/search?q=%D0%B2%D0%BE%D0%B9%D0%BD%D0%B0%20%D0%B8%20%D0%BC%D0%B8%D1%80&page=");
    free(osd);
}

static void test_books(const char *dir)
{
    opds_feed *f = load(dir, "new.xml", "http://h:8099/opds/new");
    if (!f)
        return;
    CHECK(f->nentries == 11);
    eq("next", f->next, "http://h:8099/opds/new2");
    eq("prev", f->prev, "");
    const opds_entry *e = find(f, "Мцыри");
    if (e) {
        CHECK(opds_entry_is_book(e));
        eq("author", e->author, "Лермонтов Михаил");
        CHECK(strstr(e->summary, "Классика. Мцыри — книга") == e->summary);
        CHECK(strlen(e->summary) <= 3000);
        eq("cover", opds_entry_cover(e, 0), "http://h:8099/img/cover0.png");
        eq("thumb", opds_entry_cover(e, 1), "http://h:8099/img/thumb0.png");
        opds_format fm[8];
        int n = opds_formats(e, fm, 8);
        /* купить (buy) — не формат; EPUB раньше PDF; FB2.ZIP — после */
        CHECK(n == 3);
        if (n == 3) {
            eq("fmt0", fm[0].label, "EPUB");
            eq("fmt0 href", fm[0].link->href, "http://h:8099/dl/test.epub");
            eq("fmt1", fm[1].label, "PDF");
            CHECK(fm[1].link->length == 3000000);
            eq("fmt2", fm[2].label, "FB2.ZIP");
            eq("fmt2 ext", fm[2].ext, ".fb2.zip");
        }
        const opds_link *nav = opds_entry_nav(e);
        CHECK(nav && !strcmp(nav->href, "http://h:8099/opds/author0")); /* related — запасной раздел */
    }
    e = find(f, "Герой нашего времени");
    if (e) { /* xhtml: теги и абзацы убраны */
        eq("xhtml", e->summary, "Первый абзац про Герой нашего времени. Второй абзац & ещё текст после переноса.");
        opds_format fm[8];
        CHECK(opds_formats(e, fm, 8) == 1 && !strcmp(fm[0].label, "PDF"));
    }
    e = find(f, "Война и мир");
    if (e) { /* html в экранированном виде, &nbsp; и &mdash; */
        eq("html", e->summary, "Аннотация в HTML: Война и мир Ещё абзац — с тире.");
        opds_format fm[8];
        CHECK(opds_formats(e, fm, 8) == 1 && !strcmp(fm[0].label, "DJVU"));
    }
    e = find(f, "Анна Каренина");
    if (e) { /* CDATA: тег внутри — тоже убирается */
        eq("cdata", e->summary, "Текст в CDATA: не тег Анна Каренина & прочее");
        opds_format fm[8];
        int n = opds_formats(e, fm, 8);
        CHECK(n == 2);
        if (n == 2) {
            eq("epub", fm[0].label, "EPUB");
            eq("octet", fm[1].label, "Файл"); /* неизвестный тип — в конец */
        }
    }
    e = find(f, "Мастер и Маргарита");
    if (e) { /* тип application/zip ничего не говорит — расширение берётся из адреса */
        opds_format fm[8];
        CHECK(opds_formats(e, fm, 8) == 1 && !strcmp(fm[0].label, "EPUB"));
    }
    e = find(f, "Серии и циклы");
    if (e) {
        CHECK(!opds_entry_is_book(e));
        CHECK(opds_entry_nav(e) != NULL);
    }
    opds_feed_free(f);

    f = load(dir, "new2.xml", "http://h:8099/opds/new2");
    if (f) {
        CHECK(f->nentries == 4);
        eq("prev2", f->prev, "http://h:8099/opds/new");
        eq("next2", f->next, "");
        opds_feed_free(f);
    }
}

static void test_inline(void)
{
    /* windows-1251 + сущности + относительная ссылка с «..», запись без ссылок и названия — пропуск */
    const char head[] = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\n<feed xmlns=\"http://www.w3.org/2005/Atom\">"
                        "<title>";
    unsigned char ru[] = { 0xcf, 0xf0, 0xe8, 0xe2, 0xe5, 0xf2, 0 }; /* Привет */
    char doc[1024];
    int n = snprintf(doc, sizeof doc, "%s%s &amp; &#1081; &#x44F;</title><entry><id>x</id></entry>"
                     "<entry><title>T</title><link rel=\"http://opds-spec.org/acquisition\" href=\"../b/1.fb2\"/></entry></feed>",
                     head, (const char *)ru);
    opds_feed *f = opds_feed_parse(doc, (size_t)n, "http://h/a/b/c");
    CHECK(f != NULL);
    if (f) {
        eq("cp1251", f->title, "Привет & й я");
        CHECK(f->nentries == 1);
        opds_format fm[4];
        CHECK(opds_formats(&f->entries[0], fm, 4) == 1);
        eq("ext по адресу", fm[0].ext, ".fb2");
        eq("href", f->entries[0].links[0].href, "http://h/a/b/1.fb2");
        opds_feed_free(f);
    }
    /* не лента */
    CHECK(opds_feed_parse("<html><body/></html>", 20, "http://h/") == NULL);
    CHECK(opds_feed_parse("", 0, "http://h/") == NULL);
    int js, ht;
    CHECK(!opds_looks_like_xml("{\"metadata\":{}}", 15, &js, &ht) && js);
    CHECK(!opds_looks_like_xml("<!DOCTYPE html><html>", 21, &js, &ht) && ht);
    CHECK(opds_looks_like_xml("\xef\xbb\xbf<?xml?><feed/>", 19, &js, &ht));
    /* оборванный файл не падает */
    const char cut[] = "<feed><title>A</title><entry><title>B</title><link rel=\"http://opds-spec.org/acquisition\" href=\"/x.pdf\"";
    f = opds_feed_parse(cut, strlen(cut), "http://h/");
    if (f)
        opds_feed_free(f);
    /* вложенность глубже предела */
    char deep[4096] = "<feed>";
    for (int i = 0; i < 200; i++)
        strcat(deep, "<a>");
    f = opds_feed_parse(deep, strlen(deep), "http://h/");
    if (f)
        opds_feed_free(f);

    eq("ext type", opds_ext_for_type("application/epub+zip; charset=x"), ".epub");
    eq("ext type", opds_ext_for_type("text/html"), "");
    eq("ext path", opds_ext_for_path("http://h/a/B.EPUB?x=1"), ".epub");
    eq("ext path", opds_ext_for_path("http://h/a/b.fb2.zip"), ".fb2.zip");
    eq("ext path", opds_ext_for_path("http://h/a/b/download"), "");
    CHECK(!strcmp(opds_ext_sniff((const unsigned char *)"%PDF-1.4....", 12), ".pdf"));
    CHECK(!strcmp(opds_ext_sniff((const unsigned char *)"AT&TFORM....", 12), ".djvu"));

    char url[256];
    opds_search_url("http://h/s?q={searchTerms}&n={count?}&p={startPage}&l={language}", "a b&c", url, sizeof url);
    eq("search url", url, "http://h/s?q=a%20b%26c&n=&p=1&l=");
    opds_search_url("http://h/s/{searchTerms}", "x", url, 12);
    CHECK(strlen(url) < 12);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/net/opds";
    test_root(dir);
    test_genres(dir);
    test_books(dir);
    test_inline();
    if (fails)
        printf("ошибок: %d\n", fails);
    else
        printf("opds: всё хорошо\n");
    return fails ? 1 : 0;
}
