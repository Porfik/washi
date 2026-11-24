/* covers — обложки и сведения о книгах для хаба: первая страница мелко, число страниц,
 * автор и название из PDF.
 *
 * Рисует фоновый поток своей копией документа; результат кэшируется на диске:
 * <dir>/<ключ>.pgm (обложка) и <dir>/<ключ>.inf (страниц, автор, название). Ключ — хэш пути,
 * размера и времени изменения файла: файл изменился — обложка рисуется заново.
 * Запрошенные последними рисуются первыми (то, что сейчас на экране).
 */
#ifndef WASHI_DOC_COVERS_H
#define WASHI_DOC_COVERS_H

#include <stdint.h>

#include "doc.h"

#define COVER_W 330
#define COVER_H 440

typedef struct covers covers;

typedef struct {
    const uint8_t *img; /* w×h, шаг w; NULL — обложки нет (не нарисовалась) */
    int w, h;
    int pages; /* 0 — неизвестно */
    char author[160];
    char title[256];
} cover_info;

/* opt — те же параметры документа, что у читалки (переключатели патчей MuPDF общие). */
covers *covers_new(const char *dir, const doc_options *opt);
void covers_free(covers *c);

/* Для потока интерфейса. 0 — готово, 1 — рисуется, -1 — не вышло. *out годится до следующего
 * covers_get: давно не нужные обложки выгружаются из памяти (и читаются с диска снова). */
int covers_get(covers *c, const char *path, const cover_info **out);

/* Книга переименована (файл уже лежит по пути to, размер и время те же): обложка и сведения
 * переезжают на новый ключ, чтобы не рисоваться заново. */
void covers_move(covers *c, const char *from, const char *to);

/* Книга сейчас будет удалена (файл ещё на месте): убрать её обложку и сведения с диска и из памяти. */
void covers_forget(covers *c, const char *path);

/* Появились ли готовые с прошлого вызова. */
int covers_take_ready(covers *c);

/* Есть ли запрошенные и ещё не готовые. */
int covers_pending(covers *c);

/* 1 — приостановить (начатая обложка прерывается и будет нарисована позже), 0 — продолжить.
 * На время чтения: не отнимать процессор у страниц книги. */
void covers_pause(covers *c, int pause);

#endif
