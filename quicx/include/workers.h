#ifndef WORKERS_H
#define WORKERS_H

#include <stdint.h>
#include "apool.h"

/*
    The worker struct is allocated from the adaptive pool and the size 
    of one worker is as stated in the static assert.
*/

/* Structure for the workers. */
typedef struct Worker {
    struct Worker*  next;       /* next worker */
    struct Worker*  prev;       /* previous worker */
    int             fd;         /* worker fd */
    uint32_t        id;         /* worker id */
    int             current_fd; /* fd of the producer */
    uint8_t         is_idle;    /* is the woorker idle */
    uint8_t         _pad[3];
} Worker;

_Static_assert(sizeof(Worker) == 32, "Task must be 32 bytes");

/* Structure for the worker pool. */
typedef struct {
    Worker*     idle_head;  /* the head of the idle workers */
    Worker*     idle_tail;  /* the tail of the idle workers */
    uint32_t    idle_count; /* the count of the idle workers */

    Worker*     busy_head;  /* the hread of te busy workers */
    Worker*     busy_tail;  /* the tail of rhe busy workers */
    uint32_t    busy_count; /* the count of the busy workers */

    uint32_t    next_id;    /* the next if of the worker */
} WorkerPool;

/* Function to init the workers */
void    workers_init(WorkerPool* pool);

/* Function to add a worker */
Worker* worker_add(WorkerPool* pool, Apool* mem, int fd);

/* Function to het a idle worker */
Worker* worker_get_idle(WorkerPool* pool);

/* Function to set a worker idle */
void    worker_set_idle(WorkerPool* pool, Worker* w);

/* Function to set the worker busy */
void    worker_set_busy(WorkerPool* pool, Worker* w);

/* Function to remoove a worker */
void    worker_remove(WorkerPool* pool, Apool* mem, Worker* w);

/* Function to return a worker to idle */
void    worker_return_idle(WorkerPool* pool, Worker* w);

/* Function that returns the count of idle workers */
int workers_idle_count(WorkerPool* pool);

/* Function that returns the count of busy workers */
int workers_busy_count(WorkerPool* pool);

#endif