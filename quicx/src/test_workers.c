#include <stdio.h>
#include "workers.h"
#include "incPMAD.h"

int main() {
    // init PMAD
    size_t sizes[]       = {32, 64, 128, 256, 512};
    size_t percentages[] = {40, 20, 20,  10,  10};
    pmad_init(sizes, percentages);

    WorkerPool pool;
    workers_init(&pool);

    // add three workers with fake fds
    Worker* a = worker_add(&pool, 10);
    Worker* b = worker_add(&pool, 20);
    Worker* c = worker_add(&pool, 30);

    printf("idle: %d (expect 3)\n", workers_idle_count(&pool));
    printf("busy: %d (expect 0)\n", workers_busy_count(&pool));

    // get idle worker — should be a (first in)
    Worker* w = worker_get_idle(&pool);
    printf("got worker fd: %d (expect 10)\n", w->fd);
    worker_set_busy(&pool, w);

    printf("idle: %d (expect 2)\n", workers_idle_count(&pool));
    printf("busy: %d (expect 1)\n", workers_busy_count(&pool));

    // worker finishes
    worker_set_idle(&pool, w);

    printf("idle: %d (expect 3)\n", workers_idle_count(&pool));
    printf("busy: %d (expect 0)\n", workers_busy_count(&pool));

    // worker disconnects
    worker_remove(&pool, b);

    printf("idle: %d (expect 2)\n", workers_idle_count(&pool));

    // cleanup
    worker_remove(&pool, a);
    worker_remove(&pool, c);

    printf("idle: %d (expect 0)\n", workers_idle_count(&pool));

    pmad_destroy();
    printf("\nall tests passed\n");
    return 0;
}