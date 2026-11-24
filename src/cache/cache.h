/* cache — LRU-кэш готовых страниц с лимитом по байтам. Только для одного потока (интерфейса). */
#ifndef WASHI_CACHE_H
#define WASHI_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* Готовая страница в формате буфера экрана. */
typedef struct page_image {
    int page;
    int failed; /* отрисовка с ошибкой: картинка может быть пустой */
    uint8_t *pixels;
    int width, height, stride;
    size_t bytes;
    double render_ms; /* загрузка + отрисовка */
    int npages;            /* страниц в картинке: 1, в развороте — 2 */
    int lpage[2];          /* какие страницы */
    float lscale[2], lx0[2], ly0[2]; /* как они вписаны: пиксель → точка страницы */
    int lx[2];             /* столбец картинки, с которого начинается страница */
    struct page_image *prev, *next;
} page_image;

page_image *page_image_new(int page, int width, int height, int stride);
void page_image_free(page_image *img);

typedef struct cache cache;

cache *cache_new(size_t limit_bytes);
void cache_free(cache *c);

/* Выбрасывает все страницы (например, сменился масштаб). */
void cache_clear(cache *c);

/* Страница из кэша (становится самой свежей) или NULL. */
page_image *cache_get(cache *c, int page);

/* Есть ли страница, без изменения порядка. */
int cache_has(const cache *c, int page);

/* Забирает img во владение; вытесняет самые старые страницы сверх лимита,
 * кроме страниц из защищённого диапазона. */
void cache_put(cache *c, page_image *img);

/* Страницы from..to включительно не вытесняются (окно вокруг текущей). */
void cache_protect(cache *c, int from, int to);

int cache_count(const cache *c);
size_t cache_bytes(const cache *c);

#endif
