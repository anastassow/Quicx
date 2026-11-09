#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>
#include <stdint.h>

#define MAX_TASK_TYPE_SIZE 32

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
    uint8_t         _pad[2];        
    uint32_t        payload_len;                /* len of the payload */
    char            type[MAX_TASK_TYPE_SIZE];   /* type of the message (user defined) */
    int             producer_fd;                /* the fd of the producer */
} Task;

_Static_assert(sizeof(Task) == 64, "Task must be 64 bytes");

/* Structure for the queue */
typedef struct {
    Task*       head;       /* start node of the queue*/
    Task*       tail;       /* end node of the queue */
    uint32_t    count;      /* count for the tasks in the queue */
    uint32_t    next_id;    /* counter in order to know that the id of the next should be */
} Queue;

/* Function for initializing the queue. */
void queue_init(Queue* q);

/* Function to push a task in the queue. */
void queue_push(Queue* q, Task* t);

/* Function to pop a task from the queue. */
Task* queue_pop(Queue* q);

/* Function to update the next id. */
uint32_t queue_next_id(Queue* q);

/* Returns the size of the queue. */
int queue_size(Queue* q);

#endif