/* Название книги по имени файла (home_book_title): расширение, «_», хвосты title_strip. */
#include "../../src/platform/platform.h"
#include "../../src/ui/home.h"

#include <stdio.h>
#include <string.h>

static int fails;

/* home.c и widgets.c тянут платформу; экрана в тесте нет. */
void pf_update(int x, int y, int w, int h, pf_update_mode mode)
{
    (void)x, (void)y, (void)w, (void)h, (void)mode;
}

int pf_battery(int *charging)
{
    *charging = 0;
    return 100;
}

static void check(const char *path, const char *want)
{
    char out[256];
    home_book_title(path, out, sizeof out);
    if (strcmp(out, want)) {
        printf("НЕ ТАК: %s → «%s», ждали «%s»\n", path, out, want);
        fails++;
    }
}

int main(void)
{
    home_set_title_strip("");
    check("/mnt/ext1/Книги/Война_и_мир.fb2", "Война и мир");
    check("/a/book (scan).pdf", "book (scan)");
    check("/a/name.with.dots", "name.with");
    home_set_title_strip(" (Scan) | (ocr)|");
    check("/a/book (scan).pdf", "book");
    check("/a/book (OCR) (scan).pdf", "book");
    check("/a/book_(scan).pdf", "book");
    check("/a/(scan).pdf", "(scan)"); /* хвост во всё название — остаётся */
    check("/a/scanner.pdf", "scanner");
    if (!fails)
        printf("все проверки прошли\n");
    return fails != 0;
}
