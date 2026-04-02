#include <queue.h>
#include <string.h>

// private---------------------------------

static void list_push(Queue* q, Task* t) {
    t->next = NULL;

    if (!q->tail) {
        q->head = t;
        q->tail = t;
    } else {
        q->tail->next = t;
        q->tail       = t;
    }

    q->count++;
}

static Task* list_pop(Queue* q) {
    if (!q->head) return NULL;

    Task* t = q->head;
    q->head = t->next;

    if (!q->head) q->tail = NULL;

    q->count--;
    t->next = NULL;

    return t;
}

// public API-----------------------------

void queue_init(Queue* q) {
    memset(q, 0, sizeof(Queue));
    q->next_id = 1;
}

void queue_push(Queue* q, Task* t) {
    list_push(q, t);
}

Task* queue_pop(Queue* q) {
    return list_pop(q);
}

uint32_t queue_next_id(Queue* q) {
    return q->next_id++;
}

int queue_size(Queue* q) {
    return (int)q->count;
}