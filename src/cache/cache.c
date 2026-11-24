#include "cache.h"

#include <stdlib.h>

struct cache {
    size_t limit, bytes;
    int count;
    page_image *head, *tail; /* head — самая свежая */
    int protect_from, protect_to;
};

page_image *page_image_new(int page, int width, int height, int stride)
{
    page_image *img = calloc(1, sizeof *img);
    if (!img)
        return NULL;
    img->bytes = (size_t)stride * height;
    img->pixels = malloc(img->bytes);
    if (!img->pixels) {
        free(img);
        return NULL;
    }
    img->page = page;
    img->width = width;
    img->height = height;
    img->stride = stride;
    return img;
}

void page_image_free(page_image *img)
{
    if (!img)
        return;
    free(img->pixels);
    free(img);
}

cache *cache_new(size_t limit_bytes)
{
    cache *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->limit = limit_bytes;
    c->protect_from = 1;
    c->protect_to = 0;
    return c;
}

static void unlink_image(cache *c, page_image *img)
{
    if (img->prev)
        img->prev->next = img->next;
    else
        c->head = img->next;
    if (img->next)
        img->next->prev = img->prev;
    else
        c->tail = img->prev;
    img->prev = img->next = NULL;
    c->bytes -= img->bytes;
    c->count--;
}

static void push_front(cache *c, page_image *img)
{
    img->prev = NULL;
    img->next = c->head;
    if (c->head)
        c->head->prev = img;
    c->head = img;
    if (!c->tail)
        c->tail = img;
    c->bytes += img->bytes;
    c->count++;
}

void cache_clear(cache *c)
{
    while (c->head) {
        page_image *img = c->head;
        unlink_image(c, img);
        page_image_free(img);
    }
}

void cache_free(cache *c)
{
    if (!c)
        return;
    cache_clear(c);
    free(c);
}

static page_image *find(const cache *c, int page)
{
    for (page_image *img = c->head; img; img = img->next)
        if (img->page == page)
            return img;
    return NULL;
}

page_image *cache_get(cache *c, int page)
{
    page_image *img = find(c, page);
    if (img && img != c->head) {
        unlink_image(c, img);
        push_front(c, img);
    }
    return img;
}

int cache_has(const cache *c, int page)
{
    return find(c, page) != NULL;
}

static int is_protected(const cache *c, int page)
{
    return page >= c->protect_from && page <= c->protect_to;
}

static void evict(cache *c)
{
    page_image *img = c->tail;
    while (c->bytes > c->limit && img) {
        page_image *prev = img->prev;
        if (!is_protected(c, img->page)) {
            unlink_image(c, img);
            page_image_free(img);
        }
        img = prev;
    }
}

void cache_put(cache *c, page_image *img)
{
    page_image *old = find(c, img->page);
    if (old) {
        unlink_image(c, old);
        page_image_free(old);
    }
    push_front(c, img);
    evict(c);
}

void cache_protect(cache *c, int from, int to)
{
    c->protect_from = from;
    c->protect_to = to;
}

int cache_count(const cache *c)
{
    return c->count;
}

size_t cache_bytes(const cache *c)
{
    return c->bytes;
}
