#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>

#define MAX_TASK_TYPE_SIZE 32

/*
value levels. A producer can give a task a value from 0 to 15 (MSG_SUBMIT_VALUED);
a plain MSG_SUBMIT gets QUEUE_LEVEL_DEFAULT. Higher levels are dispatched first,
oldest first within a level, so with a single level the queue is a plain FIFO.
When the budget is full, admission.c may evict a lower-level task to make room
for a higher one. QUEUE_LEVEL_CRITICAL is never evicted.
*/
#define QUEUE_LEVELS         16
#define QUEUE_LEVEL_DEFAULT  8
#define QUEUE_LEVEL_CRITICAL 15

typedef enum {
    TASK_WAITING     = 0,
    TASK_IN_PROGRESS = 1,
    TASK_FAILED      = 2,
    TASK_DEAD        = 3,
} TaskStatus;

/* Structure foe tasks that will go into the queue. */
typedef struct Task {
    void*           payload;        /* the payload, stored right behind this record in the same block */
    struct Task*    next;           /* next task in the queue */
    uint32_t        id;             /* id of the current task in the queue */
    uint8_t         status;         /* status of the task from the enum */
    uint8_t         retries_left;   /* retries that had happened */
    uint8_t         level;          /* value level, 0..QUEUE_LEVELS-1 */
    uint8_t         _pad;
    uint32_t        payload_len;                /* len of the payload */
    char            type[MAX_TASK_TYPE_SIZE];   /* type of the message (user defined) */
    int             producer_fd;                /* the fd of the producer */
} Task;

_Static_assert(sizeof(Task) == 64, "Task must be 64 bytes");

/* Structure for the queue: one FIFO per value level */
typedef struct {
    Task*       head[QUEUE_LEVELS];   /* oldest task of each level */
    Task*       tail[QUEUE_LEVELS];   /* newest task of each level */
    uint32_t    count;                /* tasks in the queue, all levels */
    uint32_t    next_id;              /* id the next task gets */
    uint16_t    nonempty;             /* bit l set when level l has tasks */
} Queue;

/*
decides whether a waiting task may be evicted: returns nonzero to take it.
ctx is passed through unchanged.
*/
typedef int (*QueueVictimFn)(void* ctx, const Task* t);

/* Function for initializing the queue. */
void queue_init(Queue* q);

/* Function to push a task in the queue (into the list of t->level). */
void queue_push(Queue* q, Task* t);

/* Function to pop a task from the queue: the oldest of the highest level. */
Task* queue_pop(Queue* q);

/*
takes one waiting task out of the queue for eviction and returns it, or NULL.
by_value: the oldest acceptable task of the lowest level below `below`
          (levels 0 .. below-1, lowest first).
!by_value: the oldest acceptable task of any level below QUEUE_LEVEL_CRITICAL.
Each level is searched from its oldest task, at most `scan` tasks deep.
*/
Task* queue_take_victim(Queue* q, int by_value, uint8_t below, int scan,
                        QueueVictimFn ok, void* ctx);

/* calls fn for every waiting task (used when a producer disconnects) */
void queue_for_each(Queue* q, void (*fn)(void* ctx, Task* t), void* ctx);

/* Function to update the next id. */
uint32_t queue_next_id(Queue* q);

/* Returns the size of the queue. */
int queue_size(Queue* q);

#endif