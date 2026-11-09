/* correctness tests for apool */
#include "apool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, failed = 0;

static void check(const char* name, int ok) {
    if (ok) { passed++; printf("  PASS: %s\n", name); }
    else    { failed++; printf("  FAIL: %s\n", name); }
}

#define POOL (8u * 1024 * 1024)

static int lists_empty(const Apool* p) {
    for (int c = 0; c < APOOL_NUM_CLASSES; c++) {
        if (p->classes[c].live || p->classes[c].pages) return 0;
        for (int lt = 0; lt < APOOL_LIFETIMES; lt++)
            for (int g = 0; g < APOOL_BINS; g++)
                if (p->classes[c].head[lt][g] != APOOL_NONE) return 0;
    }
    return 1;
}

static uint32_t page_of(const Apool* p, const void* b) {
    return (uint32_t)(((const uint8_t*)b - p->data) / APOOL_PAGE_SIZE);
}

/*
random sizes, random order, 1 in 8 blocks marked long-lived; patterns catch any
overlap. run twice: with the features the daemon uses (APOOL_F_DEFAULT) and
with every feature on together (APOOL_F_ALL, which adds fullest-first)
*/
static void test_stress(uint32_t flags, const char* name) {
    printf("--- random stress, %s ---\n", name);
    Apool p;
    check("init", apool_init(&p, POOL) == APOOL_OK);
    p.flags = flags;   /* allowed: no allocation yet */

    enum { SLOTS = 20000, ROUNDS = 2000000 };
    static void*    ptr[SLOTS];
    static uint16_t len[SLOTS];
    static uint8_t  tag[SLOTS];
    int bad = 0, misaligned = 0, unexpected = 0;

    srand(12345);
    for (int r = 0; r < ROUNDS; r++) {
        int i = rand() % SLOTS;
        if (ptr[i]) {
            for (int k = 0; k < len[i]; k++)
                if (((uint8_t*)ptr[i])[k] != tag[i]) { bad++; break; }
            if (apool_free(&p, ptr[i]) != APOOL_OK) unexpected++;
            ptr[i] = NULL;
        } else {
            size_t sz = 1 + (size_t)(rand() % APOOL_MAX_BLOCK);
            void*  b;
            ApoolStatus s = apool_alloc_hint(&p, sz, rand() % 8 == 0, &b);
            if (s == APOOL_ERR_FULL) continue;
            if (s != APOOL_OK) { unexpected++; continue; }
            if ((uintptr_t)b % APOOL_ALIGN) misaligned++;
            ptr[i] = b; len[i] = (uint16_t)sz; tag[i] = (uint8_t)(r * 31 + 7);
            memset(b, tag[i], sz);
        }
    }
    for (int i = 0; i < SLOTS; i++)
        if (ptr[i]) { if (apool_free(&p, ptr[i]) != APOOL_OK) unexpected++; ptr[i] = NULL; }

    check("no block overlapped another", bad == 0);
    check("every block 16-byte aligned", misaligned == 0);
    check("no unexpected errors", unexpected == 0);
    check("all pages back on the free list", p.free_pages == p.num_pages);
    check("no size holds pages, blocks or list entries", lists_empty(&p));
    check("no double free or corruption reported", p.double_frees == 0 && p.corruptions == 0);
    check("pages changed size during the run", p.reshapes > 0);
    check("purge ran during the run", !(flags & APOOL_F_PURGE) || p.purges > 0);
    printf("  flags 0x%x · %u pages · %llu reshapes · %llu purges\n", (unsigned)flags, p.num_pages,
           (unsigned long long)p.reshapes, (unsigned long long)p.purges);
    apool_destroy(&p);
}

/* exact capacity with every feature off: the reserve would keep pages back */
static void test_reshape(void) {
    printf("--- reshape: all 64 B, drain, all 1024 B ---\n");
    Apool p;
    apool_init(&p, POOL);
    p.flags = 0;

    size_t cap64 = (size_t)p.num_pages * (APOOL_PAGE_SIZE / 64);
    void** v = malloc((cap64 + 16) * sizeof(void*));
    size_t n = 0;
    while (n < cap64 + 10 && (v[n] = apool_alloc(&p, 64))) n++;
    check("64 B fills every page exactly", n == cap64 && p.free_pages == 0);
    for (size_t i = 0; i < n; i++) apool_free(&p, v[i]);
    check("drained back to all free pages", p.free_pages == p.num_pages);

    size_t cap1k = (size_t)p.num_pages * (APOOL_PAGE_SIZE / 1024);
    n = 0;
    while (n < cap1k + 10 && (v[n] = apool_alloc(&p, 1024))) n++;
    check("1024 B then fills every page exactly", n == cap1k);
    check("every page changed size", p.reshapes == p.num_pages);
    for (size_t i = 0; i < n; i++) apool_free(&p, v[i]);
    free(v);
    apool_destroy(&p);
}

/* a flood of one size leaves the reserve for sizes that own nothing */
static void test_reserve(void) {
    printf("--- reserve ---\n");
    Apool p;
    apool_init(&p, POOL);
    p.flags = APOOL_F_RESERVE;

    size_t n = 0;
    while (apool_alloc(&p, 48)) n++;
    check("flood stops with the reserve still free", p.free_pages == p.reserve && p.reserve > 0);

    size_t big = 0;
    while (apool_alloc(&p, 912)) big++;
    check("a new size still gets exactly one page", big == APOOL_PAGE_SIZE / 912);
    check("that page came out of the reserve", p.free_pages == p.reserve - 1);
    printf("  48 B: %zu blocks · 912 B afterwards: %zu blocks\n", n, big);
    apool_destroy(&p);
}

/* allocation goes to the fullest page so the emptier ones drain */
static void test_fullest(void) {
    printf("--- fullest page first ---\n");
    Apool p;
    apool_init(&p, POOL);
    p.flags = APOOL_F_FULLEST;

    enum { PER = APOOL_PAGE_SIZE / 64 };
    static void* v[2 * PER];
    for (int i = 0; i < 2 * PER; i++) v[i] = apool_alloc(&p, 64);
    uint32_t a = page_of(&p, v[0]), b = page_of(&p, v[PER]);
    check("two full pages of 64 B", a != b && page_of(&p, v[PER - 1]) == a);

    for (int i = 0; i < 200; i++) apool_free(&p, v[i]);          /* page a: 56 left */
    for (int i = PER; i < PER + 10; i++) apool_free(&p, v[i]);   /* page b: 246 left */

    void* x = apool_alloc(&p, 64);
    check("next block comes from the fuller page", page_of(&p, x) == b);
    apool_destroy(&p);
}

/* long-lived blocks never share a page with short-lived ones */
static void test_lifetime(void) {
    printf("--- lifetime classes ---\n");
    Apool p;
    apool_init(&p, POOL);

    void *s[20], *l[20];
    int mixed = 0;
    for (int i = 0; i < 20; i++) {
        apool_alloc_hint(&p, 100, 0, &s[i]);
        apool_alloc_hint(&p, 100, 1, &l[i]);
    }
    for (int i = 0; i < 20; i++)
        for (int j = 0; j < 20; j++)
            if (page_of(&p, s[i]) == page_of(&p, l[j])) mixed++;
    check("short and long blocks are on different pages", mixed == 0);
    for (int i = 0; i < 20; i++) { apool_free(&p, s[i]); apool_free(&p, l[i]); }
    check("all pages returned", p.free_pages == p.num_pages);
    apool_destroy(&p);
}

static void test_double_free(void) {
    printf("--- double free ---\n");
    Apool p;
    apool_init(&p, POOL);

    void* keep = apool_alloc(&p, 64);
    void* a    = apool_alloc(&p, 64);
    void* b    = apool_alloc(&p, 64);
    check("first free is fine", apool_free(&p, a) == APOOL_OK);
    check("second free of the same block is caught", apool_free(&p, a) == APOOL_ERR_DOUBLE_FREE);
    check("it is counted", p.double_frees == 1);
    check("a live neighbour still frees normally", apool_free(&p, b) == APOOL_OK);
    void* again = apool_alloc(&p, 64);
    check("the free list still works", again == b);
    apool_free(&p, again);
    apool_free(&p, keep);
    check("freeing into a returned page is rejected", apool_free(&p, keep) == APOOL_ERR_INVALID_PTR);
    apool_destroy(&p);
}

/* a freed block whose link is overwritten is detected, and the pool keeps working */
static void test_corruption(void) {
    printf("--- corrupted free list ---\n");
    Apool p;
    apool_init(&p, POOL);

    void* keep = apool_alloc(&p, 64);
    void* x    = apool_alloc(&p, 64);
    void* y    = apool_alloc(&p, 64);
    apool_free(&p, x);
    apool_free(&p, y);                       /* free list: y -> x */
    memset(y, 0x41, 16);                     /* use after free overwrites y's link */

    void* first  = apool_alloc(&p, 64);
    void* second = apool_alloc(&p, 64);
    check("the overwritten block is handed out", first == y);
    check("the broken link is detected", p.corruptions == 1);
    check("the bad address is never handed out", second != NULL && second != x &&
          page_of(&p, second) == page_of(&p, keep));
    apool_free(&p, first);
    apool_free(&p, second);
    apool_free(&p, keep);
    check("the page still drains and returns", p.free_pages == p.num_pages);
    apool_destroy(&p);
}

/* empty pages beyond the warm set give memory back, and work again when reused */
static void test_purge(void) {
    printf("--- purge ---\n");
    Apool p;
    apool_init(&p, POOL);
    p.flags = APOOL_F_PURGE;
    p.warm  = 2;

    enum { N = 10 * (APOOL_PAGE_SIZE / 1024) };
    static void* v[N];
    for (int i = 0; i < N; i++) { v[i] = apool_alloc(&p, 1024); memset(v[i], 7, 1024); }
    for (int i = 0; i < N; i++) apool_free(&p, v[i]);
    check("pages beyond the warm set were purged", p.purges >= 8);

    int ok = 1;
    for (int i = 0; i < N; i++) {
        v[i] = apool_alloc(&p, 1024);
        if (!v[i]) { ok = 0; break; }
        memset(v[i], 9, 1024);
        if (((uint8_t*)v[i])[1023] != 9) ok = 0;
    }
    check("purged pages are usable again", ok);
    for (int i = 0; i < N; i++) apool_free(&p, v[i]);
    apool_destroy(&p);
}

static void test_invalid(void) {
    printf("--- invalid input ---\n");
    Apool p;
    apool_init(&p, 1024 * 1024);
    void* out;
    int   on_stack;

    check("size 0 rejected",    apool_alloc_ex(&p, 0, &out) == APOOL_ERR_SIZE && !out);
    check("size 1089 rejected", apool_alloc_ex(&p, APOOL_MAX_BLOCK + 1, &out) == APOOL_ERR_SIZE);
    check("size 1088 accepted", apool_alloc_ex(&p, APOOL_MAX_BLOCK, &out) == APOOL_OK && out);
    void* keep = out;

    uint8_t* a = apool_alloc(&p, 100);   /* 112 B blocks */
    check("free NULL",                 apool_free(&p, NULL) == APOOL_ERR_INVALID_PTR);
    check("free pointer outside pool", apool_free(&p, &on_stack) == APOOL_ERR_INVALID_PTR);
    check("free pointer inside block", apool_free(&p, a + 16) == APOOL_ERR_INVALID_PTR);
    check("free never-used block",     apool_free(&p, a + 112 * 5) == APOOL_ERR_INVALID_PTR);
    check("free into an unowned page", apool_free(&p, p.data + (size_t)(p.num_pages - 1) * APOOL_PAGE_SIZE) == APOOL_ERR_INVALID_PTR);
    check("valid free",                apool_free(&p, a) == APOOL_OK);
    apool_free(&p, keep);
    apool_destroy(&p);
}

int main(void) {
    test_stress(APOOL_F_DEFAULT, "daemon features");
    test_stress(APOOL_F_ALL, "all features");
    test_reshape();
    test_reserve();
    test_fullest();
    test_lifetime();
    test_double_free();
    test_corruption();
    test_purge();
    test_invalid();
    printf("\npassed: %d  failed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
