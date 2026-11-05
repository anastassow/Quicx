#include "apool.h"

#include <string.h>
#include <sys/mman.h>

#define ROUND_UP(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))

/* ---------- page lists ---------- */

/* pushes page pi onto the front of class c's partial list */
static void partial_push(Apool* p, int c, uint32_t pi) {
    ApoolPage* pg = &p->pages[pi];
    uint32_t   head = p->classes[c].partial_head;

    pg->prev = APOOL_NONE;
    pg->next = head;
    if (head != APOOL_NONE) p->pages[head].prev = pi;
    p->classes[c].partial_head = pi;
    pg->on_partial = 1;
}

/* unlinks page pi from class c's partial list */
static void partial_remove(Apool* p, int c, uint32_t pi) {
    ApoolPage* pg = &p->pages[pi];

    if (pg->prev != APOOL_NONE) p->pages[pg->prev].next = pg->next;
    else                        p->classes[c].partial_head = pg->next;
    if (pg->next != APOOL_NONE) p->pages[pg->next].prev = pg->prev;

    pg->prev = pg->next = APOOL_NONE;
    pg->on_partial = 0;
}

/* takes a page from the shared free list and gives it to class c */
static uint32_t take_page(Apool* p, int c) {
    uint32_t pi = p->free_head;
    if (pi == APOOL_NONE) return APOOL_NONE;

    ApoolPage* pg = &p->pages[pi];
    p->free_head  = pg->next;
    p->free_pages--;

    if (pg->last_cls != APOOL_FREE_PAGE && pg->last_cls != (uint8_t)c)
        p->reshapes++;

    pg->cls       = (uint8_t)c;
    pg->last_cls  = (uint8_t)c;
    pg->free_list = NULL;
    pg->bump      = 0;
    pg->live      = 0;
    pg->capacity  = (uint16_t)(APOOL_PAGE_SIZE / apool_class_size(c));

    p->classes[c].pages++;
    partial_push(p, c, pi);
    return pi;
}

/* gives an empty page of class c back to the shared free list */
static void release_page(Apool* p, int c, uint32_t pi) {
    ApoolPage* pg = &p->pages[pi];

    if (pg->on_partial) partial_remove(p, c, pi);
    pg->cls  = APOOL_FREE_PAGE;
    pg->prev = APOOL_NONE;
    pg->next = p->free_head;
    p->free_head = pi;
    p->free_pages++;
    p->classes[c].pages--;
}

/* ---------- public API ---------- */

ApoolStatus apool_init(Apool* p, size_t pool_size) {
    memset(p, 0, sizeof(*p));

    /* metadata for n pages + n pages must fit in pool_size */
    uint32_t n = (uint32_t)(pool_size / (APOOL_PAGE_SIZE + sizeof(ApoolPage)));
    while (n > 0 && ROUND_UP(n * sizeof(ApoolPage), 64) + (size_t)n * APOOL_PAGE_SIZE > pool_size)
        n--;
    if (n == 0) return APOOL_ERR_TOO_SMALL;

    void* mem = mmap(NULL, pool_size, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return APOOL_ERR_MAP;

    p->map       = (uint8_t*)mem;
    p->map_size  = pool_size;
    p->pages     = (ApoolPage*)mem;
    p->data      = p->map + ROUND_UP(n * sizeof(ApoolPage), 64);
    p->num_pages = n;

    for (int c = 0; c < APOOL_NUM_CLASSES; c++)
        p->classes[c].partial_head = APOOL_NONE;

    /* every page starts on the shared free list, in address order */
    p->free_head  = APOOL_NONE;
    for (uint32_t i = n; i-- > 0;) {
        ApoolPage* pg = &p->pages[i];
        pg->cls      = APOOL_FREE_PAGE;
        pg->last_cls = APOOL_FREE_PAGE;
        pg->prev     = APOOL_NONE;
        pg->next     = p->free_head;
        p->free_head = i;
    }
    p->free_pages = n;

    return APOOL_OK;
}

void apool_destroy(Apool* p) {
    if (p->map) munmap(p->map, p->map_size);
    memset(p, 0, sizeof(*p));
}

ApoolStatus apool_alloc_ex(Apool* p, size_t size, void** out) {
    *out = NULL;
    if (size == 0 || size > APOOL_MAX_BLOCK) return APOOL_ERR_SIZE;

    int         c   = (int)(ROUND_UP(size, APOOL_ALIGN) / APOOL_ALIGN) - 1;
    ApoolClass* cls = &p->classes[c];

    uint32_t pi = cls->partial_head;
    if (pi == APOOL_NONE) {
        pi = take_page(p, c);
        if (pi == APOOL_NONE) { cls->fails++; return APOOL_ERR_FULL; }
    }

    ApoolPage* pg = &p->pages[pi];
    uint8_t*   blk;

    if (pg->free_list) {
        blk = (uint8_t*)pg->free_list;
        pg->free_list = *(void**)blk;
    } else {
        blk = p->data + (size_t)pi * APOOL_PAGE_SIZE + pg->bump;
        pg->bump += (uint32_t)apool_class_size(c);
    }

    pg->live++;
    if (pg->live == pg->capacity) partial_remove(p, c, pi);

    cls->live++;
    cls->allocs++;
    *out = blk;
    return APOOL_OK;
}

void* apool_alloc(Apool* p, size_t size) {
    void* out;
    apool_alloc_ex(p, size, &out);
    return out;
}

ApoolStatus apool_free(Apool* p, void* ptr) {
    uint8_t* b = (uint8_t*)ptr;
    if (!b || b < p->data) return APOOL_ERR_INVALID_PTR;

    size_t off = (size_t)(b - p->data);
    if (off >= (size_t)p->num_pages * APOOL_PAGE_SIZE) return APOOL_ERR_INVALID_PTR;

    uint32_t   pi = (uint32_t)(off / APOOL_PAGE_SIZE);
    ApoolPage* pg = &p->pages[pi];
    if (pg->cls == APOOL_FREE_PAGE || pg->live == 0) return APOOL_ERR_INVALID_PTR;

    int    c  = pg->cls;
    size_t in = off % APOOL_PAGE_SIZE;
    if (in % apool_class_size(c) != 0 || in >= pg->bump) return APOOL_ERR_INVALID_PTR;

    int was_full = (pg->live == pg->capacity);

    *(void**)b    = pg->free_list;
    pg->free_list = b;
    pg->live--;
    p->classes[c].live--;

    if (pg->live == 0)  release_page(p, c, pi);
    else if (was_full)  partial_push(p, c, pi);

    return APOOL_OK;
}

size_t apool_live_bytes(const Apool* p) {
    size_t total = 0;
    for (int c = 0; c < APOOL_NUM_CLASSES; c++)
        total += p->classes[c].live * apool_class_size(c);
    return total;
}
