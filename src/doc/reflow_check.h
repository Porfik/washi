/* Дешёвая проверка файла перетекающей книги (EPUB/FB2/HTML) до открытия в MuPDF.
 *
 * Разбор HTML5 и вёрстка в MuPDF квадратично зависят от глубины вложенности тегов, прервать их
 * нельзя (fz_layout_document не знает про cookie/abort), а «злой» файл в 2 МБ может крутить
 * ядро часами. Поэтому до открытия файл просматривается потоком: размер, число и размер
 * записей архива (EPUB), максимальная глубина вложенности тегов. Правок MuPDF не требуется. */
#ifndef WASHI_REFLOW_CHECK_H
#define WASHI_REFLOW_CHECK_H

#include <stddef.h>

/* Пределы (можно переопределить -D при сборке тестов). */
#ifndef REFLOW_MAX_DEPTH
#define REFLOW_MAX_DEPTH 1500 /* реальные книги — десятки; XML-разбор MuPDF сам режет на 4096 */
#endif
#ifndef REFLOW_MAX_FILE
#define REFLOW_MAX_FILE ((size_t)32 << 20) /* одиночный файл FB2/HTML или упакованная запись EPUB */
#endif
#ifndef REFLOW_MAX_TOTAL
#define REFLOW_MAX_TOTAL ((size_t)64 << 20) /* разобранный текст всех записей EPUB */
#endif
#ifndef REFLOW_MAX_ROWS
#define REFLOW_MAX_ROWS 8000 /* строк таблиц в одном файле: вёрстка таблиц квадратична (60 000 строк — минуты) */
#endif
#ifndef REFLOW_MAX_ENTRIES
#define REFLOW_MAX_ENTRIES 20000
#endif

/* Потоковый счётчик глубины вложенности тегов (HTML/XML). */
typedef struct {
    int state;
    int depth, max_depth;
    int rows; /* открытых <tr> */
    int closing, selfclose, counted;
    char name[16];
    int nlen;
    char quote, prev;
    char raw[8];  /* script/style: внутри тегов нет */
    int raw_pos;
} reflow_scan;

void reflow_scan_init(reflow_scan *s);
void reflow_scan_feed(reflow_scan *s, const unsigned char *buf, size_t n);

/* 0 — открывать можно; -1 — нельзя, err — причина по-русски. Файл, который не удалось прочитать
 * или распознать, пропускается (0): пусть MuPDF сам скажет, что с ним. Все чтения ограничены. */
int reflow_check(const char *path, char *err, size_t err_len);

#endif
