#ifndef APOOL_H
#define APOOL_H

/*
apool — the adaptive pool behind Quicx.

The total memory is fixed: one mmap at init, never grown. What changes
at runtime is its shape: how many pages each block size owns.

    mapping = [ page 0 | page 1 | ... | page N-1 | page records ]

- every page is APOOL_PAGE_SIZE bytes and, while in use, belongs to one
  size class and one lifetime class; it is carved into blocks of that size
- a size takes a page from the shared free list only when it has no room
  left, and a page whose last block is freed goes back to that list, where
  any size can take it next — that is the reshaping
- blocks carry no header: the page record says what they are

Ideas taken from published allocators, each behind a flag so its effect
can be measured on its own:

  APOOL_F_FULLEST  pages of a size are kept in four fullness groups and
                   allocation uses the fullest one, so the emptier pages
                   drain and can be reshaped (Hoard, Berger et al. 2000)
  APOOL_F_RESERVE  the last few free pages only go to sizes that own no
                   page yet, so one size cannot starve all the others
                   (Linux zone watermarks, memcached's per-class minimum)
  APOOL_F_HARDEN   links inside freed blocks are XOR-encoded with a
                   per-pool key, so corruption is detected on use, and
                   double frees are caught (mimalloc secure mode,
                   Leijen et al. 2019)
  APOOL_F_PURGE    free pages are reused most-recent first; when more
                   than `warm` of them are free, the coldest give their
                   physical memory back to the OS, so a burst's memory is
                   returned without busy traffic ever paying for it
                   (jemalloc / mimalloc purging)

  lifetime classes: callers can mark allocations as long-lived; those get
                   their own pages, so a few long-lived blocks cannot pin
                   pages full of short-lived ones (LLAMA, Maas et al. 2020)

Live blocks are never moved (the caller holds pointers to them).
*/

#include <stddef.h>
#include <stdint.h>

#define APOOL_ALIGN        16
#define APOOL_MAX_BLOCK    1088                              /* a 64 B Task record + up to 1,024 B payload */
#define APOOL_NUM_CLASSES  (APOOL_MAX_BLOCK / APOOL_ALIGN)   /* 68 sizes: 16, 32, ... 1088 */
#ifndef APOOL_PAGE_SIZE
#define APOOL_PAGE_SIZE    (16 * 1024)                       /* override with -DAPOOL_PAGE_SIZE=... to experiment */
#endif
#define APOOL_LIFETIMES    2                                 /* 0 = short-lived (default), 1 = long-lived */
#define APOOL_BINS         4                                 /* fullness groups: <25%, <50%, <75%, <100% */
#define APOOL_NONE         UINT32_MAX
#define APOOL_FREE_PAGE    0xFF

enum {
    APOOL_F_FULLEST = 1u << 0,
    APOOL_F_RESERVE = 1u << 1,
    APOOL_F_HARDEN  = 1u << 2,
    APOOL_F_PURGE   = 1u << 3,
    APOOL_F_ALL     = 0xFu,
    /*
    what apool_init turns on. fullest-first is left off: measured on queue
    traffic it did not help, because tasks are freed almost in arrival
    order and pages already drain on their own (see bench_apool)
    */
    APOOL_F_DEFAULT = APOOL_F_RESERVE | APOOL_F_HARDEN | APOOL_F_PURGE,
};

typedef enum {
    APOOL_OK = 0,
    APOOL_ERR_MAP,          /* mmap failed */
    APOOL_ERR_TOO_SMALL,    /* pool cannot hold even one page */
    APOOL_ERR_SIZE,         /* request is 0 or larger than APOOL_MAX_BLOCK */
    APOOL_ERR_FULL,         /* no room in the size and no free page it may take */
    APOOL_ERR_INVALID_PTR,  /* not a live block handed out by this pool */
    APOOL_ERR_DOUBLE_FREE,  /* block is already free */
} ApoolStatus;

/* per page bookkeeping, kept in the records area at the end of the mapping */
typedef struct {
    uint32_t prev, next;    /* links in the list the page is on (page numbers) */
    void*    free_list;     /* first freed block of this page */
    uint32_t bump;          /* offset of the next never-used block */
    uint16_t live;          /* blocks handed out */
    uint16_t capacity;      /* blocks of this size that fit */
    uint8_t  cls;           /* owning size class, APOOL_FREE_PAGE when unowned */
    uint8_t  last_cls;      /* previous owner, to count reshapes */
    uint8_t  lt;            /* lifetime class */
    uint8_t  where;         /* list it is on: a fullness group, the free list, or none */
    uint8_t  purged;        /* physical memory was given back to the OS */
    uint8_t  _pad[3];
} ApoolPage;

_Static_assert(sizeof(ApoolPage) == 32, "ApoolPage must be 32 bytes");

/* per size class: one list per lifetime class and fullness group */
typedef struct {
    uint32_t head[APOOL_LIFETIMES][APOOL_BINS];
    uint32_t pages;         /* pages owned right now */
    uint64_t live;          /* blocks handed out right now */
    uint64_t allocs;        /* successful allocations */
    uint64_t fails;         /* refused allocations */
} ApoolClass;

typedef struct {
    uint8_t*   map;         /* start of the mapping — page 0 */
    size_t     map_size;    /* bytes requested from the OS, never changes */
    uint8_t*   data;        /* same as map: pages start at the mapping */
    uint32_t   num_pages;
    uint32_t   free_pages;  /* pages owned by no size, warm + cold */
    uint32_t   free_head, free_tail;   /* warm free pages, most recently freed at the head */
    uint32_t   warm_pages;             /* how many are on the warm list */
    uint32_t   cold_head;              /* cold free pages: purged, or never used yet */

    uint32_t   flags;       /* APOOL_F_*, set before the first allocation */
    uint32_t   reserve;     /* free pages kept for sizes that own none */
    uint32_t   warm;        /* free pages kept backed before purging */
    uintptr_t  key;         /* encodes free-list links when hardened */

    uint64_t   reshapes;    /* pages handed to a different size than before */
    uint64_t   purges;      /* pages whose memory went back to the OS */
    uint64_t   double_frees;
    uint64_t   corruptions; /* broken free-list links found and cut */

    ApoolPage*  pages;
    ApoolClass  classes[APOOL_NUM_CLASSES];
} Apool;

/* maps pool_size bytes once; APOOL_F_DEFAULT on, reserve and warm set from the size */
ApoolStatus apool_init(Apool* p, size_t pool_size);

/* unmaps the pool */
void apool_destroy(Apool* p);

/* returns a block of at least size bytes (16-byte aligned), or NULL */
void* apool_alloc(Apool* p, size_t size);

/* same, reporting why it failed */
ApoolStatus apool_alloc_ex(Apool* p, size_t size, void** out);

/* same, for a lifetime class: 0 short-lived, 1 long-lived */
ApoolStatus apool_alloc_hint(Apool* p, size_t size, int lifetime, void** out);

/* returns a block; a page with no live blocks left goes back to the shared list */
ApoolStatus apool_free(Apool* p, void* ptr);

/* block size of a class index */
static inline size_t apool_class_size(int c) { return (size_t)(c + 1) * APOOL_ALIGN; }

/* bytes apool_init needs to get exactly n pages (pages + records) */
static inline size_t apool_bytes_for_pages(uint32_t n) {
    size_t records = ((size_t)n * sizeof(ApoolPage) + 63) & ~(size_t)63;
    return (size_t)n * APOOL_PAGE_SIZE + records;
}

/* bytes held by live blocks */
size_t apool_live_bytes(const Apool* p);

#endif
