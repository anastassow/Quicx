#include "workers.h"
#include <string.h>

// private helpers-------------------

static void list_push(Worker** head, Worker** tail, uint32_t* count, Worker* w) {
    w->next = NULL;
    w->prev = *tail;

    if (!*tail) {
        *head = w;
        *tail = w;
    } else {
        (*tail)->next = w;
        *tail = w;
    }

    (*count)++;
}

static void list_remove(Worker** head, Worker** tail, uint32_t* count, Worker* w) {
    if (w->prev) w->prev->next = w->next;
    else         *head = w->next;

    if (w->next) w->next->prev = w->prev;
    else         *tail = w->prev;

    w->next = NULL;
    w->prev = NULL;

    (*count)--;
}

static Worker* list_pop(Worker** head, Worker** tail, uint32_t* count) {
    if (!*head) return NULL;

    Worker* w = *head;
    list_remove(head, tail, count, w);

    return w;
}

// public API-------------------------------

void workers_init(WorkerPool* pool) {
    memset(pool, 0, sizeof(WorkerPool));
    pool->next_id = 1;
}

Worker* worker_add(WorkerPool* pool, int fd) {
    Worker* w = (Worker*)pmad_alloc(sizeof(Worker));
    if (!w) return NULL;

    w->fd      = fd;
    w->id      = pool->next_id++;
    w->is_idle = 1;
    w->next    = NULL;
    w->prev    = NULL;

    list_push(&pool->idle_head, &pool->idle_tail, &pool->idle_count, w);

    return w;
}

Worker* worker_get_idle(WorkerPool* pool) {
    return list_pop(&pool->idle_head, &pool->idle_tail, &pool->idle_count);
}

void worker_set_busy(WorkerPool* pool, Worker* w) {
    w->is_idle = 0;
    list_push(&pool->busy_head, &pool->busy_tail, &pool->busy_count, w);
}

void worker_set_idle(WorkerPool* pool, Worker* w) {
    list_remove(&pool->busy_head, &pool->busy_tail, &pool->busy_count, w);
    w->is_idle = 1;
    list_push(&pool->idle_head, &pool->idle_tail, &pool->idle_count, w);
}

void worker_remove(WorkerPool* pool, Worker* w) {
    if (w->is_idle) {
        list_remove(&pool->idle_head, &pool->idle_tail, &pool->idle_count, w);
    } else {
        list_remove(&pool->busy_head, &pool->busy_tail, &pool->busy_count, w);
    }

    pmad_free(w);
}

void worker_return_idle(WorkerPool* pool, Worker* w) {
    list_push(&pool->idle_head, &pool->idle_tail,
              &pool->idle_count, w);
}

int workers_idle_count(WorkerPool* pool) {
    return (int)pool->idle_count;
}

int workers_busy_count(WorkerPool* pool) {
    return (int)pool->busy_count;
}