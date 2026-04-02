#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>
#include "incPMAD.h"

typedef enum {
    TASK_WAITING     = 0,
    TASK_IN_PROGRESS = 1,
    TASK_FAILED      = 2,
    TASK_DEAD        = 3,
} TaskStatus;

typedef struct Task {
    void*           payload;
    struct Task*    next;
    uint32_t        id;
    uint8_t         status;
    uint8_t         retries_left;
    uint8_t         _pad[2];
    uint32_t        payload_len;
    char            type[32];
    uint8_t         _align[4];
} Task;

_Static_assert(sizeof(Task) == 64, "Task must be 64 bytes");

typedef struct {
    Task*       head;
    Task*       tail;
    uint32_t    count;
    uint32_t    next_id;
} Queue;

void     queue_init(Queue* q);
void     queue_push(Queue* q, Task* t);
Task*    queue_pop(Queue* q);
uint32_t queue_next_id(Queue* q);
int      queue_size(Queue* q);

#endif