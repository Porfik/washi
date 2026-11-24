/* net: адреса (resolve, encode/decode), имена файлов для FAT32, свободный путь.
 * С NET_TEST_URL=<адрес> — ещё и запрос через libcurl (в память и в файл). */
#include "../../src/net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *pf_data_dir(void) /* net.c ищет там cacert.pem */
{
    return "/nonexistent";
}

static int fails;

static void eq(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("НЕ ТАК %s: «%s», ждали «%s»\n", what, got, want);
        fails++;
    }
}

static void resolve(const char *base, const char *href, const char *want)
{
    char out[1024];
    net_url_resolve(base, href, out, sizeof out);
    eq(href, out, want);
}

int main(void)
{
    const char *b = "http://a/b/c/d;p?q";
    resolve(b, "g", "http://a/b/c/g");
    resolve(b, "./g", "http://a/b/c/g");
    resolve(b, "g/", "http://a/b/c/g/");
    resolve(b, "/g", "http://a/g");
    resolve(b, "//g", "http://g");
    resolve(b, "?y", "http://a/b/c/d;p?y");
    resolve(b, "g?y", "http://a/b/c/g?y");
    resolve(b, "#s", "http://a/b/c/d;p?q#s");
    resolve(b, "..", "http://a/b/");
    resolve(b, "../g", "http://a/b/g");
    resolve(b, "../../g", "http://a/g");
    resolve(b, "../../../g", "http://a/g");
    resolve(b, "https://x/y", "https://x/y");
    resolve("http://h:8080/opds", "books/1.epub", "http://h:8080/books/1.epub");
    resolve("http://h", "x", "http://h/x");

    char out[512];
    net_url_encode("Книги/a b.epub", 1, out, sizeof out);
    eq("encode", out, "%D0%9A%D0%BD%D0%B8%D0%B3%D0%B8/a%20b.epub");
    char back[512];
    net_url_decode(out, back, sizeof back);
    eq("decode", back, "Книги/a b.epub");

    net_safe_name("../dir/Война и мир: том 1?.fb2", out, sizeof out);
    eq("safe", out, "Война и мир_ том 1_.fb2");
    net_safe_name(" .. ", out, sizeof out);
    eq("safe empty", out, "book");
    net_safe_name("C:\\books\\x.pdf", out, sizeof out);
    eq("safe backslash", out, "x.pdf");
    char longname[600];
    memset(longname, 0, sizeof longname);
    for (int i = 0; i < 150; i++)
        strcat(longname, "ж");
    strcat(longname, ".epub");
    net_safe_name(longname, out, sizeof out);
    if (strlen(out) > 200 || strcmp(out + strlen(out) - 5, ".epub") || ((unsigned char)out[strlen(out) - 6] & 0xC0) == 0xC0)
        printf("НЕ ТАК длинное имя: %zu байт «%s»\n", strlen(out), out), fails++;

    char dir[] = "/tmp/net-test-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    char sub[300], path[600];
    snprintf(sub, sizeof sub, "%s/a/b", dir);
    if (net_unique_path(sub, "x.epub", path, sizeof path) != 0)
        printf("НЕ ТАК unique_path\n"), fails++;
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
    net_unique_path(sub, "x.epub", path, sizeof path);
    char want[600];
    snprintf(want, sizeof want, "%s/x (2).epub", sub);
    eq("unique", path, want);
    snprintf(path, sizeof path, "%s/z.fb2.zip", sub);
    f = fopen(path, "w");
    if (f)
        fclose(f);
    net_unique_path(sub, "z.fb2.zip", path, sizeof path);
    snprintf(want, sizeof want, "%s/z (2).fb2.zip", sub);
    eq("unique fb2.zip", path, want);

    const char *url = getenv("NET_TEST_URL");
    if (url) {
        char why[160];
        if (net_http_ready(why, sizeof why) != 0) {
            printf("НЕ ТАК libcurl: %s\n", why);
            return 1;
        }
        net_request rq = { 0 };
        rq.url = url;
        net_response rs;
        int rc = net_perform(&rq, &rs);
        printf("GET %s → %d, код %ld, %zu байт, %s, «%s»\n", url, rc, rs.status, rs.len, rs.content_type, rs.error);
        if (rc != 0 || !rs.len)
            fails++;
        net_response_free(&rs);
        snprintf(path, sizeof path, "%s/got", dir);
        rq.out_path = path;
        rc = net_perform(&rq, &rs);
        struct stat st;
        printf("в файл → %d, %lld байт\n", rc, stat(path, &st) == 0 ? (long long)st.st_size : -1LL);
        if (rc != 0 || stat(path, &st) != 0)
            fails++;
        net_response_free(&rs);
        rq.url = "http://127.0.0.1:1/nothing";
        rq.out_path = NULL;
        rc = net_perform(&rq, &rs);
        printf("нет сервера → %d «%s»\n", rc, rs.error);
        if (rc == 0 || !rs.error[0])
            fails++;
        net_response_free(&rs);
    }
    printf(fails ? "net: %d ошибок\n" : "net: всё верно\n", fails);
    return fails != 0;
}
