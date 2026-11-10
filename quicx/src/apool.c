#include "apool.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define ROUND_UP(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))

/* values of ApoolPage.where besides a fullness group 0..APOOL_BINS-1 */
#define ON_COLD 0xFD
#define ON_FREE 0xFE
#define ON_NONE 0xFF

static inline uint8_t* page_base(const Apool* p, uint32_t pi) {
    return p->data + (size_t)pi * APOOL_PAGE_SIZE;
}

/* is b the start of a block of page pi that has been handed out at some point */
static int is_block(const Apool* p, uint32_t pi, const void* b) {
    const ApoolPage* pg   = &p->pages[pi];
    const uint8_t*   base = page_base(p, pi);
    const uint8_t*   q    = (const uint8_t*)b;
    if (q < base || q >= base + APOOL_PAGE_SIZE) return 0;
    size_t off = (size_t)(q - base);
    return off % apool_class_size(pg->cls) == 0 && off < pg->bump;
}

/* ---------- free-list links (mimalloc secure mode) ---------- */

static inline uintptr_t encode(const Apool* p, const void* next) {
    return (p->flags & APOOL_F_HARDEN) ? ((uintptr_t)next ^ p->key) : (uintptr_t)next;
}

static inline void* decode(const Apool* p, uintptr_t v) {
    return (void*)((p->flags & APOOL_F_HARDEN) ? (v ^ p->key) : v);
}

/* ---------- lists of pages (doubly linked by page number) ---------- */

static void list_push(Apool* p, uint32_t* head, uint32_t pi, uint8_t where) {
    ApoolPage* pg = &p->pages[pi];
    pg->prev = APOOL_NONE;
    pg->next = *head;
    if (*head != APOOL_NONE) p->pages[*head].prev = pi;
    *head = pi;
    pg->where = where;
}

static void list_unlink(Apool* p, uint32_t* head, uint32_t pi) {
    ApoolPage* pg = &p->pages[pi];
    if (pg->prev != APOOL_NONE) p->pages[pg->prev].next = pg->next;
    else                        *head = pg->next;
    if (pg->next != APOOL_NONE) p->pages[pg->next].prev = pg->prev;
    pg->prev = pg->next = APOOL_NONE;
    pg->where = ON_NONE;
}

static uint32_t* group_head(Apool* p, const ApoolPage* pg, uint8_t group) {
    return &p->classes[pg->cls].head[pg->lt][group];
}

/* fullness group of an owned, not full page (Hoard's emptiness groups) */
static uint8_t group_of(const Apool* p, const ApoolPage* pg) {
    if (!(p->flags & APOOL_F_FULLEST)) return 0;
    return (uint8_t)((uint32_t)pg->live * APOOL_BINS / pg->capacity);
}

/* moves an owned page to the group its fullness says, or off every list when full */
static void regroup(Apool* p, uint32_t pi) {
    ApoolPage* pg   = &p->pages[pi];
    uint8_t    want = (pg->live == pg->capacity) ? ON_NONE : group_of(p, pg);
    if (pg->where == want) return;
    if (pg->where != ON_NONE) list_unlink(p, group_head(p, pg, pg->where), pi);
    if (want != ON_NONE)      list_push(p, group_head(p, pg, want), pi, want);
}

/*
the shared free list is two lists:
  warm  pages freed recently and still backed by memory, reused most recent first
  cold  pages whose memory went back to the OS, or that were never used yet
*/
static void warm_push(Apool* p, uint32_t pi) {
    list_push(p, &p->free_head, pi, ON_FREE);
    if (p->free_tail == APOOL_NONE) p->free_tail = pi;
    p->warm_pages++;
}

static void warm_unlink(Apool* p, uint32_t pi) {
    if (p->free_tail == pi) p->free_tail = p->pages[pi].prev;
    list_unlink(p, &p->free_head, pi);
    p->warm_pages--;
}

/* takes the most recently freed warm page, or a cold one when none is warm */
static uint32_t free_take(Apool* p) {
    uint32_t pi = p->free_head;
    if (pi != APOOL_NONE) { warm_unlink(p, pi); return pi; }
    pi = p->cold_head;
    if (pi != APOOL_NONE) list_unlink(p, &p->cold_head, pi);
    return pi;
}

/* ---------- giving memory back (jemalloc / mimalloc purging) ---------- */

static void purge(Apool* p, uint32_t pi) {
#if defined(__APPLE__) && defined(MADV_FREE_REUSABLE)
    madvise(page_base(p, pi), APOOL_PAGE_SIZE, MADV_FREE_REUSABLE);
#else
    madvise(page_base(p, pi), APOOL_PAGE_SIZE, MADV_DONTNEED);
#endif
    p->pages[pi].purged = 1;
    p->purges++;
}

static void unpurge(Apool* p, uint32_t pi) {
#if defined(__APPLE__) && defined(MADV_FREE_REUSE)
    madvise(page_base(p, pi), APOOL_PAGE_SIZE, MADV_FREE_REUSE);
#endif
    p->pages[pi].purged = 0;
}

/* ---------- pages changing hands ---------- */

/* gives a page from the shared list to size c, lifetime lt */
static uint32_t take_page(Apool* p, int c, int lt) {
    /* the reserve is only for sizes that own no page yet */
    if ((p->flags & APOOL_F_RESERVE) && p->free_pages <= p->reserve && p->classes[c].pages > 0)
        return APOOL_NONE;

    uint32_t pi = free_take(p);
    if (pi == APOOL_NONE) return APOOL_NONE;
    p->free_pages--;

    ApoolPage* pg = &p->pages[pi];
    if (pg->purged) unpurge(p, pi);
    if (pg->last_cls != APOOL_FREE_PAGE && pg->last_cls != (uint8_t)c) p->reshapes++;

    pg->cls       = (uint8_t)c;
    pg->last_cls  = (uint8_t)c;
    pg->lt        = (uint8_t)lt;
    pg->free_list = NULL;
    pg->bump      = 0;
    pg->live      = 0;
    pg->capacity  = (uint16_t)(APOOL_PAGE_SIZE / apool_class_size(c));

    p->classes[c].pages++;
    list_push(p, group_head(p, pg, 0), pi, 0);
    return pi;
}

/*
returns an empty page to the warm list. when more than `warm` pages are
warm, the coldest one gives its memory back to the OS — so busy traffic
that keeps reusing the same few pages never pays for a purge
*/
static void release_page(Apool* p, uint32_t pi) {
    ApoolPage* pg = &p->pages[pi];
    if (pg->where != ON_NONE) list_unlink(p, group_head(p, pg, pg->where), pi);
    p->classes[pg->cls].pages--;
    pg->cls = APOOL_FREE_PAGE;
    p->free_pages++;
    warm_push(p, pi);

    if ((p->flags & APOOL_F_PURGE) && p->warm_pages > p->warm) {
        uint32_t coldest = p->free_tail;
        warm_unlink(p, coldest);
        purge(p, coldest);
        list_push(p, &p->cold_head, coldest, ON_COLD);
    }
}

/* ---------- public API ---------- */

ApoolStatus apool_init(Apool* p, size_t pool_size) {
    memset(p, 0, sizeof(*p));

    uint32_t n = (uint32_t)(pool_size / (APOOL_PAGE_SIZE + sizeof(ApoolPage)));
    while (n > 0 && apool_bytes_for_pages(n) > pool_size) n--;
    if (n == 0) return APOOL_ERR_TOO_SMALL;

    void* mem = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return APOOL_ERR_MAP;

    /* pages first, so every page starts on an OS page boundary and can be purged */
    p->map       = (uint8_t*)mem;
    p->map_size  = pool_size;
    p->data      = p->map;
    p->pages     = (ApoolPage*)(p->map + (size_t)n * APOOL_PAGE_SIZE);
    p->num_pages = n;

    for (int c = 0; c < APOOL_NUM_CLASSES; c++)
        for (int lt = 0; lt < APOOL_LIFETIMES; lt++)
            for (int g = 0; g < APOOL_BINS; g++)
                p->classes[c].head[lt][g] = APOOL_NONE;

    /* every page starts cold (never used), handed out in address order */
    p->free_head = p->free_tail = p->cold_head = APOOL_NONE;
    for (uint32_t i = n; i-- > 0;) {
        ApoolPage* pg = &p->pages[i];
        pg->cls      = APOOL_FREE_PAGE;
        pg->last_cls = APOOL_FREE_PAGE;
        list_push(p, &p->cold_head, i, ON_COLD);
    }
    p->free_pages = n;

    p->flags   = APOOL_F_DEFAULT;
    p->reserve = n >= 64 ? n / 64 : 0;  /* 7 of 508 pages */
    p->warm    = n / 8 > 4 ? n / 8 : 4; /* 63 of 508 pages stay backed */

    /* key: mixed from the mapping address (ASLR), the time and the pid; the
       top bit is set so a decoded live word almost never lands inside the pool */
    uint64_t k = (uint64_t)(uintptr_t)mem ^ ((uint64_t)time(NULL) << 32) ^ (uint64_t)getpid();
    k *= 0x9E3779B97F4A7C15ull;
    k ^= k >> 29;
    p->key = (uintptr_t)(k | (1ull << 63));

    return APOOL_OK;
}

void apool_destroy(Apool* p) {
    if (p->map) munmap(p->map, p->map_size);
    memset(p, 0, sizeof(*p));
}

ApoolStatus apool_alloc_hint(Apool* p, size_t size, int lifetime, void** out) {
    *out = NULL;
    if (size == 0 || size > APOOL_MAX_BLOCK) return APOOL_ERR_SIZE;

    int         c  = (int)(ROUND_UP(size, APOOL_ALIGN) / APOOL_ALIGN) - 1;
    int         lt = lifetime ? 1 : 0;
    ApoolClass* k  = &p->classes[c];
    uint32_t    bs = (uint32_t)apool_class_size(c);

    for (;;) {
        /* fullest group first; without APOOL_F_FULLEST everything is in group 0 */
        uint32_t pi = APOOL_NONE;
        for (int g = APOOL_BINS - 1; g >= 0 && pi == APOOL_NONE; g--)
            pi = k->head[lt][g];

        if (pi == APOOL_NONE) {
            pi = take_page(p, c, lt);
            if (pi == APOOL_NONE) { k->fails++; return APOOL_ERR_FULL; }
        }

        ApoolPage* pg  = &p->pages[pi];
        uint8_t*   blk;

        if (pg->free_list) {
            blk = (uint8_t*)pg->free_list;
            void* next = decode(p, *(uintptr_t*)blk);
            if ((p->flags & APOOL_F_HARDEN) && next && !is_block(p, pi, next)) {
                /* the link was overwritten after the block was freed: cut the
                   chain here; the page heals when its live blocks are freed */
                p->corruptions++;
                next = NULL;
            }
            pg->free_list = next;
        } else if (pg->bump < (uint32_t)pg->capacity * bs) {
            blk = page_base(p, pi) + pg->bump;
            pg->bump += bs;
        } else {
            /* only after a cut chain: nothing reachable left in this page */
            list_unlink(p, group_head(p, pg, pg->where), pi);
            continue;
        }

        /*
        clear the old link: if the caller never writes the first word, a
        stale encoded link left there would make every later free look
        like a possible double free and walk the page's free list
        */
        if (p->flags & APOOL_F_HARDEN) *(uintptr_t*)blk = 0;

        pg->live++;
        k->live++;
        k->allocs++;
        regroup(p, pi);
        *out = blk;
        return APOOL_OK;
    }
}

ApoolStatus apool_alloc_ex(Apool* p, size_t size, void** out) {
    return apool_alloc_hint(p, size, 0, out);
}

void* apool_alloc(Apool* p, size_t size) {
    void* out;
    apool_alloc_hint(p, size, 0, &out);
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
    if (!is_block(p, pi, b))                         return APOOL_ERR_INVALID_PTR;

    if (p->flags & APOOL_F_HARDEN) {
        /*
        a freed block starts with an encoded link (or encoded NULL). only when
        this block looks like one, walk the page's free list to be sure — the
        walk is bounded by the page's capacity and almost never runs for a
        live block, whose first word decodes to garbage
        */
        void* maybe = decode(p, *(uintptr_t*)b);
        if (maybe == NULL || is_block(p, pi, maybe)) {
            uint8_t* q = (uint8_t*)pg->free_list;
            for (uint32_t steps = 0; q && steps <= pg->capacity; steps++) {
                if (q == b) { p->double_frees++; return APOOL_ERR_DOUBLE_FREE; }
                void* nx = decode(p, *(uintptr_t*)q);
                if (nx && !is_block(p, pi, nx)) break;
                q = (uint8_t*)nx;
            }
        }
    }

    *(uintptr_t*)b = encode(p, pg->free_list);
    pg->free_list  = b;
    pg->live--;
    p->classes[pg->cls].live--;

    if (pg->live == 0) release_page(p, pi);
    else               regroup(p, pi);

    return APOOL_OK;
}

int apool_free_makes_room(const Apool* p, const void* victim, size_t size) {
    if (size == 0 || size > APOOL_MAX_BLOCK) return 0;
    const uint8_t* b = (const uint8_t*)victim;
    if (!b || b < p->data) return 0;
    size_t off = (size_t)(b - p->data);
    if (off >= (size_t)p->num_pages * APOOL_PAGE_SIZE) return 0;

    const ApoolPage* pg = &p->pages[off / APOOL_PAGE_SIZE];
    if (pg->cls == APOOL_FREE_PAGE || pg->live == 0) return 0;

    int c = (int)(ROUND_UP(size, APOOL_ALIGN) / APOOL_ALIGN) - 1;

    /* a block of the same size, in a page apool_alloc searches (lifetime 0) */
    if (pg->cls == (uint8_t)c && pg->lt == 0) return 1;

    /* the last block of a page: the page goes back to the free list, and the
       size may take it unless the reserve keeps it for a size that owns none */
    if (pg->live == 1) {
        uint32_t free_after = p->free_pages + 1;
        int reserve_blocks = (p->flags & APOOL_F_RESERVE) && free_after <= p->reserve &&
                             p->classes[c].pages > 0;
        return !reserve_blocks;
    }
    return 0;
}

size_t apool_live_bytes(const Apool* p) {
    size_t total = 0;
    for (int c = 0; c < APOOL_NUM_CLASSES; c++)
        total += p->classes[c].live * apool_class_size(c);
    return total;
}
