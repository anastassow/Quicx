/*
bench_purge — does APOOL_F_PURGE give memory back to the OS, and what does it cost?

For purge on (the default flags) and purge off, in one 8 MiB pool:

  burst    fill the pool with 964 B blocks (a 64 B record + 900 B payload),
           write every byte, free everything; measure the process's physical
           memory before, when full and after the drain
  cycles   repeat that burst 40 times and time it (alloc + free per block),
           so the price of madvise on bursty traffic is visible
  churn    keep 1,000 mixed-size tasks in flight for 5,000,000 submits, oldest
           first, like the daemon; count purges (should be 0) and time it

physical memory: phys_footprint on macOS (what Activity Monitor shows and what
MADV_FREE_REUSABLE lowers), resident set size from /proc/self/statm on Linux.

output: one key=value line per mode, easy to parse.
build:  cc -std=c17 -O2 -D_GNU_SOURCE -Iquicx/include quicx/src/tests/bench_purge.c quicx/src/apool.c -o bench_purge
*/
#include "apool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach.h>
static long phys_kb(void) {
    task_vm_info_data_t    info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return -1;
    return (long)(info.phys_footprint / 1024);
}
#define PHYS_NAME "phys_footprint"
#else
#include <unistd.h>
static long phys_kb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    long  size = 0, rss = -1;
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &size, &rss) != 2) rss = -1;
    fclose(f);
    return rss < 0 ? -1 : rss * (sysconf(_SC_PAGESIZE) / 1024);
}
#define PHYS_NAME "rss"
#endif

#define POOL   (8u * 1024 * 1024)
#define BLOCK  (64 + 900)
#define MAXB   20000

static void* v[MAXB];

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

static size_t fill_touch(Apool* p) {
    size_t n = 0;
    while (n < MAXB && (v[n] = apool_alloc(p, BLOCK)) != NULL) {
        memset(v[n], (int)(n & 0xFF), BLOCK);
        n++;
    }
    return n;
}

static void drain(Apool* p, size_t n) {
    for (size_t i = 0; i < n; i++) apool_free(p, v[i]);
}

static uint32_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static size_t mixed(void) {
    uint32_t r = rnd() % 100;
    if (r < 70) return 64 + 16 + rnd() % 113;
    if (r < 95) return 64 + 129 + rnd() % 384;
    return 64 + 513 + rnd() % 512;
}

static void run(const char* name, uint32_t flags) {
    long base = phys_kb();

    Apool p;
    if (apool_init(&p, POOL) != APOOL_OK) { printf("mode=%s error=init\n", name); return; }
    p.flags = flags;
    long init = phys_kb();

    size_t n = fill_touch(&p);
    long full = phys_kb();
    drain(&p, n);
    long drained = phys_kb();
    uint64_t burst_purges = p.purges;

    /* bursts: fill, touch, free, 40 times */
    const int cycles = 40;
    double t0 = now_ns();
    size_t ops = 0;
    for (int c = 0; c < cycles; c++) {
        size_t k = 0;
        while (k < MAXB && (v[k] = apool_alloc(&p, BLOCK)) != NULL) { ((volatile char*)v[k])[0] = 1; k++; }
        drain(&p, k);
        ops += k;
    }
    double burst_ns = (now_ns() - t0) / (double)ops;
    uint64_t cycle_purges = p.purges - burst_purges;

    /* steady churn: 1,000 in flight, oldest first */
    enum { DEPTH = 1000, OPS = 5000000 };
    static void* ring[DEPTH];
    rng = 42;
    for (int i = 0; i < DEPTH; i++) ring[i] = apool_alloc(&p, mixed());
    uint64_t before = p.purges;
    t0 = now_ns();
    for (long i = 0; i < OPS; i++) {
        int j = (int)(i % DEPTH);
        apool_free(&p, ring[j]);
        ring[j] = apool_alloc(&p, mixed());
    }
    double churn_ns = (now_ns() - t0) / (double)OPS;
    uint64_t churn_purges = p.purges - before;
    for (int i = 0; i < DEPTH; i++) apool_free(&p, ring[i]);

    printf("mode=%s measure=%s blocks=%zu base_kb=%ld init_kb=%ld full_kb=%ld drained_kb=%ld "
           "held_after_drain_kb=%ld burst_purges=%llu cycle_ns_per_block=%.1f cycle_purges=%llu "
           "churn_ns_per_task=%.1f churn_purges=%llu\n",
           name, PHYS_NAME, n, base, init - base, full - base, drained - base,
           drained - base, (unsigned long long)burst_purges, burst_ns,
           (unsigned long long)cycle_purges, churn_ns, (unsigned long long)churn_purges);
    fflush(stdout);
    apool_destroy(&p);
}

int main(void) {
    run("purge_on",  APOOL_F_DEFAULT);
    run("purge_off", APOOL_F_DEFAULT & ~APOOL_F_PURGE);
    return 0;
}
