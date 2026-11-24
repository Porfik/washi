/* Общие мелочи тестов tests/doc: временный каталог, запись файлов, zip без сжатия. */
#ifndef TESTUTIL_H
#define TESTUTIL_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            failures++; \
        } \
    } while (0)

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        fprintf(stderr, "не записать %s\n", path);
        exit(2);
    }
    fclose(f);
}

/* Вложенные <div> глубиной n (строка в куче). */
static __attribute__((unused)) char *nested_html(int n, size_t *len)
{
    size_t cap = (size_t)n * 11 + 128;
    char *s = malloc(cap), *p = s;
    p += sprintf(p, "<html><body>");
    for (int i = 0; i < n; i++)
        p += sprintf(p, "<div>");
    p += sprintf(p, "x");
    for (int i = 0; i < n; i++)
        p += sprintf(p, "</div>");
    p += sprintf(p, "</body></html>");
    *len = (size_t)(p - s);
    return s;
}

static uint32_t crc32_of(const unsigned char *d, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

typedef struct {
    FILE *f;
    uint32_t off[64], crc[64], size[64];
    char name[64][64];
    int n;
} zipw;

static void le16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void le32(FILE *f, uint32_t v) { le16(f, v & 0xffff); le16(f, v >> 16); }

static void zip_begin(zipw *z, const char *path)
{
    memset(z, 0, sizeof *z);
    z->f = fopen(path, "wb");
    if (!z->f)
        exit(2);
}

static void zip_add(zipw *z, const char *name, const void *data, size_t n)
{
    int i = z->n++;
    snprintf(z->name[i], 64, "%s", name);
    z->off[i] = (uint32_t)ftell(z->f);
    z->crc[i] = crc32_of(data, n);
    z->size[i] = (uint32_t)n;
    le32(z->f, 0x04034b50); le16(z->f, 20); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0x21);
    le32(z->f, z->crc[i]); le32(z->f, (uint32_t)n); le32(z->f, (uint32_t)n);
    le16(z->f, (unsigned)strlen(name)); le16(z->f, 0);
    fwrite(name, 1, strlen(name), z->f);
    fwrite(data, 1, n, z->f);
}

static void zip_end(zipw *z)
{
    uint32_t start = (uint32_t)ftell(z->f);
    for (int i = 0; i < z->n; i++) {
        le32(z->f, 0x02014b50); le16(z->f, 20); le16(z->f, 20); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0);
        le16(z->f, 0x21); le32(z->f, z->crc[i]); le32(z->f, z->size[i]); le32(z->f, z->size[i]);
        le16(z->f, (unsigned)strlen(z->name[i])); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0);
        le32(z->f, 0); le32(z->f, z->off[i]);
        fwrite(z->name[i], 1, strlen(z->name[i]), z->f);
    }
    uint32_t size = (uint32_t)ftell(z->f) - start;
    le32(z->f, 0x06054b50); le16(z->f, 0); le16(z->f, 0); le16(z->f, (unsigned)z->n); le16(z->f, (unsigned)z->n);
    le32(z->f, size); le32(z->f, start); le16(z->f, 0);
    fclose(z->f);
}

/* EPUB из одной главы chapter (len байт). */
static __attribute__((unused)) void make_epub(const char *path, const char *chapter, size_t len)
{
    static const char container[] =
        "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"c.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    static const char opf[] =
        "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"i\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>T</dc:title><dc:identifier id=\"i\">x</dc:identifier>"
        "<dc:language>en</dc:language></metadata><manifest><item id=\"c0\" href=\"c0.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"c0\"/></spine></package>";
    zipw z;
    zip_begin(&z, path);
    zip_add(&z, "mimetype", "application/epub+zip", 20);
    zip_add(&z, "META-INF/container.xml", container, sizeof container - 1);
    zip_add(&z, "c.opf", opf, sizeof opf - 1);
    zip_add(&z, "c0.xhtml", chapter, len);
    zip_end(&z);
}
#endif
