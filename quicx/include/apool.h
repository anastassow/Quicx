#ifndef APOOL_H
#define APOOL_H

/*
apool — an adaptive slab pool (experiment).

The total memory is fixed: one mmap at init, never grown. What changes
at runtime is its *shape*: how much of the pool each block size gets.

    pool  = [ page metadata | page 0 | page 1 | ... | page N-1 ]

- every page is PAGE_SIZE bytes and, while in use, belongs to exactly one
  size class; it is carved into blocks of that class
- a class takes a page from the shared free list only when someone asks
  for that size and the class has no free block left
- when the last live block of a page is freed, the page goes back to the
  shared free list and ANY class can take it next — that is the reshaping
- blocks carry no header: the page they live in says which class they
  are, so a 64 B request costs 64 B, not 64 + 16

Live blocks are never moved (the caller holds pointers to them), so a page
can only change class once it is completely empty. A task queue frees in
roughly the order it allocates (FIFO), so pages filled together empty
together — the access pattern this design relies on.
*/

#include <stddef.h>
#include <stdint.h>

#define APOOL_ALIGN        16
#define APOOL_MAX_BLOCK    1024
#define APOOL_NUM_CLASSES  (APOOL_MAX_BLOCK / APOOL_ALIGN)   /* 16, 32, ... 1024 */
#ifndef APOOL_PAGE_SIZE
#define APOOL_PAGE_SIZE    (16 * 1024)   /* override with -DAPOOL_PAGE_SIZE=... to experiment */
#endif
#define APOOL_NONE         UINT32_MAX
#define APOOL_FREE_PAGE    0xFF

typedef enum {
    APOOL_OK = 0,
    APOOL_ERR_MAP,          /* mmap failed */
    APOOL_ERR_TOO_SMALL,    /* pool cannot hold even one page */
    APOOL_ERR_SIZE,         /* request is 0 or larger than APOOL_MAX_BLOCK */
    APOOL_ERR_FULL,         /* no free block in the class and no free page */
    APOOL_ERR_INVALID_PTR,  /* not a live block handed out by this pool */
} ApoolStatus;

/* per page bookkeeping, kept in the metadata area at the front of the pool */
typedef struct {
    uint32_t prev, next;    /* links in the owning class's partial list, or the free list */
    void*    free_list;     /* blocks of this page that were freed (intrusive list) */
    uint32_t bump;          /* offset of the next never-used block in this page */
    uint16_t live;          /* blocks currently handed out */
    uint16_t capacity;      /* blocks of this class that fit in one page */
    uint8_t  cls;           /* owning class, APOOL_FREE_PAGE when unowned */
    uint8_t  last_cls;      /* class that owned it before, to count reshapes */
    uint8_t  on_partial;    /* 1 while linked in the class's partial list */
    uint8_t  _pad[5];
} ApoolPage;

/* per size class state and demand counters */
typedef struct {
    uint32_t partial_head;  /* pages of this class with at least one free block */
    uint32_t pages;         /* pages owned right now */
    uint64_t live;          /* blocks handed out right now */
    uint64_t allocs;        /* successful allocations since init */
    uint64_t fails;         /* failed allocations since init */
} ApoolClass;

typedef struct {
    uint8_t*    map;        /* start of the whole mapping */
    size_t      map_size;   /* bytes requested from the OS — never changes */
    uint8_t*    data;       /* first byte of page 0 */
    uint32_t    num_pages;
    uint32_t    free_pages; /* pages owned by no class */
    uint32_t    free_head;  /* shared free page list */
    uint64_t    reshapes;   /* pages handed to a different class than their last owner */
    ApoolPage*  pages;
    ApoolClass  classes[APOOL_NUM_CLASSES];
} Apool;

/* maps pool_size bytes once and lays out metadata + pages */
ApoolStatus apool_init(Apool* p, size_t pool_size);

/* unmaps the pool */
void apool_destroy(Apool* p);

/* returns a block of at least size bytes (16-byte aligned), or NULL */
void* apool_alloc(Apool* p, size_t size);

/* same as apool_alloc but reports why it failed */
ApoolStatus apool_alloc_ex(Apool* p, size_t size, void** out);

/* returns a block to the pool; empty pages go back to the shared list */
ApoolStatus apool_free(Apool* p, void* ptr);

/* block size of a class index */
static inline size_t apool_class_size(int c) { return (size_t)(c + 1) * APOOL_ALIGN; }

/* bytes apool_init needs to get exactly n pages (records + pages) */
static inline size_t apool_bytes_for_pages(uint32_t n) {
    size_t records = ((size_t)n * sizeof(ApoolPage) + 63) & ~(size_t)63;
    return records + (size_t)n * APOOL_PAGE_SIZE;
}

/* bytes held by live blocks (payload only, no metadata) */
size_t apool_live_bytes(const Apool* p);

#endif
