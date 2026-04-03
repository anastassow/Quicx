#ifndef WORKERS_H
#define WORKERS_H

#include <stdint.h>
#include "incPMAD.h"

typedef struct Worker {
    struct Worker*  next;
    struct Worker*  prev;
    int             fd;
    uint32_t        id;
    uint8_t         is_idle;
    uint8_t         _pad[7];
} Worker;

_Static_assert(sizeof(Worker) == 32, "Task must be 32 bytes");

typedef struct {
    Worker*     idle_head;
    Worker*     idle_tail;
    uint32_t    idle_count;

    Worker*     busy_head;
    Worker*     busy_tail;
    uint32_t    busy_count;

    uint32_t    next_id;
} WorkerPool;

void    workers_init(WorkerPool* pool);
Worker* worker_add(WorkerPool* pool, int fd);
Worker* worker_get_idle(WorkerPool* pool);
void    worker_set_idle(WorkerPool* pool, Worker* w);
void    worker_set_busy(WorkerPool* pool, Worker* w);
void    worker_remove(WorkerPool* peel, Worker* w);

int workers_idle_count(WorkerPool* pool);
int workers_busy_count(WorkerPool* pool);

#endif