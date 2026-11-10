#include <queue.h>
#include <string.h>

// private---------------------------------

static void list_push(Queue* q, Task* t) {
    uint8_t l = t->level < QUEUE_LEVELS ? t->level : QUEUE_LEVEL_DEFAULT;
    t->level = l;
    t->next  = NULL;

    if (!q->tail[l]) {
        q->head[l] = t;
        q->tail[l] = t;
    } else {
        q->tail[l]->next = t;
        q->tail[l]       = t;
    }

    q->nonempty |= (uint16_t)(1u << l);
    q->count++;
}

/* unlinks t, which follows prev in level l (prev NULL: t is the head) */
static void list_unlink(Queue* q, uint8_t l, Task* prev, Task* t) {
    if (prev) prev->next = t->next;
    else      q->head[l] = t->next;
    if (q->tail[l] == t) q->tail[l] = prev;
    if (!q->head[l]) q->nonempty &= (uint16_t)~(1u << l);

    q->count--;
    t->next = NULL;
}

static int highest_level(uint16_t mask) {
    for (int l = QUEUE_LEVELS - 1; l >= 0; l--)
        if (mask & (1u << l)) return l;
    return -1;
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
    int l = highest_level(q->nonempty);
    if (l < 0) return NULL;

    Task* t = q->head[l];
    list_unlink(q, (uint8_t)l, NULL, t);
    return t;
}

Task* queue_take_victim(Queue* q, int by_value, uint8_t below, int scan,
                        QueueVictimFn ok, void* ctx) {
    if (by_value) {
        /* the lowest level first, and within it the oldest task */
        for (uint8_t l = 0; l < below && l < QUEUE_LEVEL_CRITICAL; l++) {
            Task* prev = NULL;
            int   seen = 0;
            for (Task* t = q->head[l]; t && seen < scan; prev = t, t = t->next, seen++) {
                if (ok(ctx, t)) { list_unlink(q, l, prev, t); return t; }
            }
        }
        return NULL;
    }

    /* the oldest acceptable task of any level (task ids grow with age) */
    Task*   best = NULL;
    Task*   best_prev = NULL;
    uint8_t best_l = 0;
    for (uint8_t l = 0; l < QUEUE_LEVEL_CRITICAL; l++) {
        Task* prev = NULL;
        int   seen = 0;
        for (Task* t = q->head[l]; t && seen < scan; prev = t, t = t->next, seen++) {
            if (ok(ctx, t)) {
                if (!best || t->id < best->id) { best = t; best_prev = prev; best_l = l; }
                break;   /* later tasks in this level are younger */
            }
        }
    }
    if (best) list_unlink(q, best_l, best_prev, best);
    return best;
}

void queue_for_each(Queue* q, void (*fn)(void* ctx, Task* t), void* ctx) {
    for (int l = 0; l < QUEUE_LEVELS; l++)
        for (Task* t = q->head[l]; t; t = t->next) fn(ctx, t);
}

uint32_t queue_next_id(Queue* q) {
    return q->next_id++;
}

int queue_size(Queue* q) {
    return (int)q->count;
}
