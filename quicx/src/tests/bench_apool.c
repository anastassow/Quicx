/*
bench_apool — allocator-only benchmark and ablation.

Simulates Quicx's memory traffic in one 8 MiB pool and compares:
  PMAD        the original static allocator, default config, 2 blocks per task
  apool v1    adaptive pages, no extra features, 2 blocks per task
  apool v2    default features, 1 block per task (64 B record + payload)
  v2 - X      v2 with one feature switched off (or + X: switched on)

build: make bench_apool   (links PMAD and pmad_layout.c for the baseline)
*/
#include "apool.h"
#include "incPMAD.h"
#include "pmad_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POOL   (8u * 1024 * 1024)
#define RECORD 64

typedef struct {
    const char* name;
    int         pmad;      /* use PMAD instead of apool */
    int         inline_;   /* one block per task instead of record + payload */
    uint32_t    flags;     /* apool features */
    int         hint;      /* pass the lifetime hint for long-lived tasks */
} Cfg;

typedef struct { const Cfg* cfg; PMAD pm; Apool ap; } A;

static void a_init(A* a, const Cfg* c) {
    memset(a, 0, sizeof(*a));
    a->cfg = c;
    if (c->pmad) {
        PmadLayout k; pmad_layout_default(&k);
        pmad_init(&a->pm, k.sizes, (uint8_t)k.class_count, k.percentages, POOL);
    } else {
        apool_init(&a->ap, POOL);
        a->ap.flags = c->flags;
    }
}
static void a_done(A* a) { if (a->cfg->pmad) pmad_destroy(&a->pm); else apool_destroy(&a->ap); }

static void* blk(A* a, size_t n, int lt) {
    void* out = NULL;
    if (a->cfg->pmad) { PMAD_alloc(&a->pm, n, &out); return out; }
    apool_alloc_hint(&a->ap, n, a->cfg->hint ? lt : 0, &out);
    return out;
}
static void unblk(A* a, void* b) { if (!b) return; if (a->cfg->pmad) PMAD_free(&a->pm, b); else apool_free(&a->ap, b); }

typedef struct { void* rec; void* pay; } Task;

static int task_new(A* a, size_t payload, int lt, Task* t) {
    if (a->cfg->inline_) { t->pay = NULL; t->rec = blk(a, RECORD + payload, lt); return t->rec != NULL; }
    t->pay = blk(a, payload, lt);
    if (!t->pay) return 0;
    t->rec = blk(a, RECORD, lt);
    if (!t->rec) { unblk(a, t->pay); return 0; }
    return 1;
}
static void task_free(A* a, Task* t) { unblk(a, t->rec); unblk(a, t->pay); t->rec = t->pay = NULL; }

static Task q[200000];
#define QCAP (sizeof(q) / sizeof(q[0]))

static size_t fill(A* a, size_t payload, size_t from) {
    size_t n = from;
    while (n < QCAP && task_new(a, payload, 0, &q[n])) n++;
    return n - from;
}
static void drain(A* a, size_t n) { for (size_t i = 0; i < n; i++) task_free(a, &q[i]); }

static uint32_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static size_t mixed_size(void) {
    uint32_t r = rnd() % 100;
    if (r < 70) return 16 + rnd() % 113;
    if (r < 95) return 129 + rnd() % 384;
    return 513 + rnd() % 512;
}

/* ---- workloads ---- */

static size_t w_capacity(const Cfg* c, size_t payload) {
    A a; a_init(&a, c);
    size_t n = fill(&a, payload, 0);
    a_done(&a);
    return n;
}

static void w_phases(const Cfg* c, const size_t* sizes, int k, size_t* out) {
    A a; a_init(&a, c);
    for (int i = 0; i < k; i++) { out[i] = fill(&a, sizes[i], 0); drain(&a, out[i]); }
    a_done(&a);
}

/* a flood of 40 B fills the pool, nothing finishes, then 900 B tasks arrive */
static size_t w_starve(const Cfg* c) {
    A a; a_init(&a, c);
    size_t n = fill(&a, 40, 0);
    size_t m = fill(&a, 900, n);
    a_done(&a);
    return m;
}

/* 100 B tasks, one in 20 is long-lived; the rest finish; then 900 B tasks arrive */
static size_t w_pin(const Cfg* c) {
    A a; a_init(&a, c);
    size_t n = 0;
    while (n < QCAP && task_new(&a, 100, n % 20 == 0, &q[n])) n++;
    for (size_t i = 0; i < n; i++) if (i % 20) task_free(&a, &q[i]);
    size_t m = 0;
    Task t;
    while (task_new(&a, 900, 0, &t)) m++;
    a_done(&a);
    return m;
}

/* keeps depth tasks in flight for ops submits; window 1 = oldest first */
static int churn(A* a, size_t depth, size_t ops, size_t window) {
    rng = 42;
    size_t head = 0, count = 0;
    for (size_t i = 0; i < ops + depth; i++) {
        if (i < ops) {
            if (!task_new(a, mixed_size(), 0, &q[(head + count) % QCAP])) {
                while (count) { task_free(a, &q[head]); head = (head + 1) % QCAP; count--; }
                return 0;
            }
            count++;
        }
        if (count > depth || (i >= ops && count > 0)) {
            size_t w = window < count ? window : count;
            size_t k = w > 1 ? rnd() % w : 0;
            Task tmp = q[(head + k) % QCAP]; q[(head + k) % QCAP] = q[head]; q[head] = tmp;
            task_free(a, &q[head]); head = (head + 1) % QCAP; count--;
        }
    }
    return 1;
}

static size_t w_depth(const Cfg* c, size_t window) {
    size_t lo = 0, hi = 1024;
    for (;;) {
        A a; a_init(&a, c); int ok = churn(&a, hi, 300000, window); a_done(&a);
        if (!ok || hi >= 150000) break;
        lo = hi; hi *= 2;
    }
    while (hi - lo > lo / 100 + 1) {
        size_t mid = (lo + hi) / 2;
        A a; a_init(&a, c); int ok = churn(&a, mid, 300000, window); a_done(&a);
        if (ok) lo = mid; else hi = mid;
    }
    return lo;
}

static double w_speed(const Cfg* c) {
    A a; a_init(&a, c);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    churn(&a, 1000, 5000000, 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    a_done(&a);
    return ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 5e6;
}

int main(void) {
    const uint32_t D = APOOL_F_DEFAULT;
    const Cfg cfgs[] = {
        { "PMAD, default config",  1, 0, 0, 0 },
        { "apool v1",              0, 0, 0, 0 },
        { "apool v2",              0, 1, D, 1 },
        { "v2 + fullest first",    0, 1, D | APOOL_F_FULLEST, 1 },
        { "v2 - reserve",          0, 1, D & ~APOOL_F_RESERVE, 1 },
        { "v2 - hardening",        0, 1, D & ~APOOL_F_HARDEN, 1 },
        { "v2 - lifetime hints",   0, 1, D, 0 },
        { "v2 - one block/task",   0, 0, D, 1 },
    };
    const int N = (int)(sizeof(cfgs) / sizeof(cfgs[0]));
    const size_t ph[] = { 100, 900, 40, 300 };

    printf("8 MiB pool · a task is a 64 B record + payload · page %d KiB\n\n", APOOL_PAGE_SIZE / 1024);
    printf("%-22s %8s | %7s %7s %7s %7s | %7s %7s | %8s %8s %8s | %6s\n",
           "", "100 B", "100 B", "900 B", "40 B", "300 B", "starve", "pinned",
           "FIFO", "8 out", "random", "ns");
    printf("%-22s %8s | %31s | %7s %7s | %26s | %6s\n",
           "allocator", "capacity", "phases on one pool", "900 B", "900 B", "deepest queue, mixed", "/task");
    for (int i = 0; i < N; i++) {
        const Cfg* c = &cfgs[i];
        size_t p[4]; w_phases(c, ph, 4, p);
        printf("%-22s %8zu | %7zu %7zu %7zu %7zu | %7zu %7zu | %8zu %8zu %8zu | %6.1f\n",
               c->name, w_capacity(c, 100), p[0], p[1], p[2], p[3],
               w_starve(c), w_pin(c), w_depth(c, 1), w_depth(c, 8), w_depth(c, 1000000), w_speed(c));
        fflush(stdout);
    }
    printf("\nstarve: 40 B tasks fill the pool and none finish, then 900 B tasks arrive\n");
    printf("pinned: 100 B tasks, 1 in 20 long-lived, the rest finish, then 900 B tasks arrive\n");
    return 0;
}
