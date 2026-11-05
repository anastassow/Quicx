/* correctness tests for apool */
#include "apool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, failed = 0;

static void check(const char* name, int ok) {
    if (ok) { passed++; }
    else    { failed++; printf("  FAIL: %s\n", name); }
}

/* stress: random sizes, random alloc/free order, every block filled with a
   pattern and verified on free — any overlap between blocks breaks a pattern */
static void test_stress(void) {
    printf("--- random stress ---\n");
    Apool p;
    check("init", apool_init(&p, 8u * 1024 * 1024) == APOOL_OK);

    enum { SLOTS = 20000, ROUNDS = 2000000 };
    static void*    ptr[SLOTS];
    static uint16_t len[SLOTS];
    static uint8_t  tag[SLOTS];
    int bad_pattern = 0, misaligned = 0, unexpected_fail = 0;

    srand(12345);
    for (int r = 0; r < ROUNDS; r++) {
        int i = rand() % SLOTS;
        if (ptr[i]) {
            for (int k = 0; k < len[i]; k++)
                if (((uint8_t*)ptr[i])[k] != tag[i]) { bad_pattern++; break; }
            if (apool_free(&p, ptr[i]) != APOOL_OK) unexpected_fail++;
            ptr[i] = NULL;
        } else {
            size_t sz = 1 + (size_t)(rand() % APOOL_MAX_BLOCK);
            void*  b;
            ApoolStatus s = apool_alloc_ex(&p, sz, &b);
            if (s == APOOL_ERR_FULL) continue;
            if (s != APOOL_OK) { unexpected_fail++; continue; }
            if ((uintptr_t)b % APOOL_ALIGN) misaligned++;
            ptr[i] = b; len[i] = (uint16_t)sz; tag[i] = (uint8_t)(r * 31 + 7);
            memset(b, tag[i], sz);
        }
    }
    for (int i = 0; i < SLOTS; i++)
        if (ptr[i]) { if (apool_free(&p, ptr[i]) != APOOL_OK) unexpected_fail++; ptr[i] = NULL; }

    check("no block overlapped another (patterns intact)", bad_pattern == 0);
    check("every block 16-byte aligned", misaligned == 0);
    check("no unexpected alloc/free errors", unexpected_fail == 0);
    check("all pages back on the free list", p.free_pages == p.num_pages);
    int leftover = 0;
    for (int c = 0; c < APOOL_NUM_CLASSES; c++)
        if (p.classes[c].live || p.classes[c].pages || p.classes[c].partial_head != APOOL_NONE) leftover++;
    check("no class holds pages or live blocks after full drain", leftover == 0);
    printf("  %d rounds, %u pages, %llu page reshapes\n",
           ROUNDS, p.num_pages, (unsigned long long)p.reshapes);
    apool_destroy(&p);
}

/* a pool filled with one size, drained, then filled with another */
static void test_reshape(void) {
    printf("--- reshape: all 64 B, drain, all 1024 B ---\n");
    Apool p;
    apool_init(&p, 8u * 1024 * 1024);

    size_t cap64 = (size_t)p.num_pages * (APOOL_PAGE_SIZE / 64);
    void** v = malloc(cap64 * sizeof(void*));
    size_t n = 0;
    while (n < cap64 + 10 && (v[n] = apool_alloc(&p, 64))) n++;
    check("64 B fills every page exactly", n == cap64 && p.free_pages == 0);
    for (size_t i = 0; i < n; i++) apool_free(&p, v[i]);
    check("drained back to all free pages", p.free_pages == p.num_pages);

    size_t cap1k = (size_t)p.num_pages * (APOOL_PAGE_SIZE / 1024);
    n = 0;
    while (n < cap1k + 10 && (v[n] = apool_alloc(&p, 1024))) n++;
    check("1024 B then fills every page exactly", n == cap1k);
    check("pages changed class", p.reshapes == p.num_pages);
    printf("  %zu x 64 B, then %zu x 1024 B in the same %u pages\n", cap64, n, p.num_pages);
    for (size_t i = 0; i < n; i++) apool_free(&p, v[i]);
    free(v);
    apool_destroy(&p);
}

static void test_invalid(void) {
    printf("--- invalid input ---\n");
    Apool p;
    apool_init(&p, 1024 * 1024);
    void* out;
    int   on_stack;

    check("size 0 rejected",    apool_alloc_ex(&p, 0, &out) == APOOL_ERR_SIZE && !out);
    check("size 1025 rejected", apool_alloc_ex(&p, 1025, &out) == APOOL_ERR_SIZE);
    check("size 1024 accepted", apool_alloc_ex(&p, 1024, &out) == APOOL_OK && out);
    void* keep = out;

    uint8_t* a = apool_alloc(&p, 100);   /* class 112 */
    check("free NULL",                 apool_free(&p, NULL) == APOOL_ERR_INVALID_PTR);
    check("free pointer outside pool", apool_free(&p, &on_stack) == APOOL_ERR_INVALID_PTR);
    check("free pointer inside block", apool_free(&p, a + 16) == APOOL_ERR_INVALID_PTR);
    check("free never-used block",     apool_free(&p, a + 112 * 5) == APOOL_ERR_INVALID_PTR);
    check("free into an unowned page", apool_free(&p, p.data + (size_t)(p.num_pages - 1) * APOOL_PAGE_SIZE) == APOOL_ERR_INVALID_PTR);
    check("valid free",                apool_free(&p, a) == APOOL_OK);
    check("free after its page was released", apool_free(&p, a) == APOOL_ERR_INVALID_PTR);
    apool_free(&p, keep);
    apool_destroy(&p);
}

int main(void) {
    test_stress();
    test_reshape();
    test_invalid();
    printf("\npassed: %d  failed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
