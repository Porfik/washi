/* opds_feed — разбор каталогов OPDS 1.x (Atom): разделы, книги со ссылками на файлы, листание
 * (next/previous), поиск (OpenSearch). Не зависит от сети и интерфейса.
 *
 * XML разбирается своим небольшим разборщиком (не MuPDF): он идёт потоком, держит в памяти
 * только готовые записи, понимает пространства имён (atom:, dc:, opds: — чужие теги пропускаются),
 * сущности (&amp; &#1234; &nbsp;…), CDATA и кодировки UTF-8, windows-1251, ISO-8859-1.
 * OPDS 2 (JSON) не поддержан. Все адреса в результате — абсолютные (от адреса ленты).
 */
#ifndef WASHI_NET_OPDS_FEED_H
#define WASHI_NET_OPDS_FEED_H

#include <stddef.h>

typedef struct {
    char *rel;   /* "" — нет */
    char *href;  /* абсолютный адрес */
    char *type;  /* MIME ("application/epub+zip"), "" — нет */
    char *title; /* подпись ссылки, "" — нет */
    long long length; /* размер файла, байт (атрибут length), -1 — неизвестен */
} opds_link;

typedef struct {
    char *id;
    char *title;
    char *author;  /* авторы через «, » */
    char *summary; /* аннотация: обычный текст, без тегов и сущностей */
    opds_link *links;
    int nlinks;
} opds_entry;

typedef struct {
    char *title;
    opds_entry *entries;
    int nentries;
    /* Ссылки самой ленты ("" — нет): листание и поиск. */
    char *next, *prev;
    char *search;      /* шаблон поиска с {searchTerms}, прямо в ленте */
    char *search_desc; /* или описание OpenSearch (application/opensearchdescription+xml) */
    long long bytes;   /* примерно сколько занимает в памяти */
} opds_feed;

/* Разбор. base — адрес ленты (для относительных ссылок). NULL — не лента Atom (нет <feed>).
 * data не обязана заканчиваться нулём; len — её длина. */
opds_feed *opds_feed_parse(const char *data, size_t len, const char *base);
void opds_feed_free(opds_feed *f);

/* Подходит ли ответ: 1 — похоже на XML, 0 — нет; json = 1, если это JSON (OPDS 2),
 * html = 1, если это страница сайта. */
int opds_looks_like_xml(const char *data, size_t len, int *json, int *html);

/* Описание OpenSearch → шаблон поиска с {searchTerms} (абсолютный адрес). 0 — нашёлся. */
int opds_osd_template(const char *data, size_t len, const char *base, char *out, size_t out_len);

/* Подставить запрос в шаблон: {searchTerms} → запрос с процентным кодированием, необязательные
 * параметры {имя?} убираются, {startPage}/{startIndex} → 1, {count} → 20, остальное — пусто. */
void opds_search_url(const char *tmpl, const char *terms, char *out, size_t out_len);

/* Записи. */
/* Раздел каталога (ссылка на другую ленту) — не книга; NULL, если у записи такой ссылки нет. */
const opds_link *opds_entry_nav(const opds_entry *e);
/* Книга — есть ссылки на файлы (acquisition: скачать/открыть бесплатно). */
int opds_entry_is_book(const opds_entry *e);
/* Обложка: "thumbnail" — мелкая, иначе крупная (если нет нужной — любая). NULL — нет. */
const char *opds_entry_cover(const opds_entry *e, int thumbnail);

/* Форматы книги в порядке предпочтения: сначала то, что открывает наша читалка (doc_supported),
 * затем EPUB, FB2, PDF, DjVu, остальное. */
typedef struct {
    const opds_link *link;
    char ext[12];   /* ".epub", ".fb2.zip"… («.bin» — тип неизвестен) */
    char label[12]; /* "EPUB", "FB2", "FB2.ZIP"… («Файл» — неизвестен) */
    int rank;       /* меньше — лучше */
} opds_format;
int opds_formats(const opds_entry *e, opds_format *out, int max);

/* Расширение по MIME ("" — не знаем) и по имени/адресу (".epub" из «…/a.epub?x»; "" — нет). */
const char *opds_ext_for_type(const char *mime);
const char *opds_ext_for_path(const char *path_or_url);

/* Расширение по первым байтам файла ("" — не узнали). */
const char *opds_ext_sniff(const unsigned char *head, size_t len);

#endif
