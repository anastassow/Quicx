#include "admission.h"
#include "quicx.h"
#include "protocol.h"
#include "stats.h"

typedef struct {
    const Apool* pool;
    size_t       size;   /* the new task's block size */
} RoomCtx;

/* a waiting task may go only if freeing it lets the new block in */
static int frees_usable_memory(void* ctx, const Task* t) {
    const RoomCtx* rc = (const RoomCtx*)ctx;
    return apool_free_makes_room(rc->pool, t, rc->size);
}

int admission_make_room(struct Quicx* quicx, size_t size, uint8_t level) {
    if (quicx->admission == ADMISSION_REFUSE) return 0;

    RoomCtx ctx = { &quicx->mem, size };
    int   by_value = quicx->admission == ADMISSION_VALUE;
    Task* victim   = queue_take_victim(&quicx->queue, by_value, level, ADMISSION_SCAN,
                                       frees_usable_memory, &ctx);
    if (!victim) return 0;

    /* the producer learns its task will not run, as if a worker had failed it */
    if (victim->producer_fd >= 0)
        protocol_send_failed(victim->producer_fd, victim->id, "evicted");
    stats_task_evicted(&quicx->stats);
    apool_free(&quicx->mem, victim);
    return 1;
}
