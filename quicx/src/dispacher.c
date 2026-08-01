#include "dispacher.h"
#include <string.h>
#include <unistd.h>
#include "protocol.h"

void try_dispach(Quicx* quicx) {
    while (1) {
        Worker* w = worker_get_idle(&quicx->pool);
        if (!w) return;

        Task* t = queue_pop(&quicx->queue);
        if (!t) {
            worker_return_idle(&quicx->pool, w);

            return;
        }

        worker_set_busy(&quicx->pool, w);

        w->current_fd = t->producer_fd;

        protocol_send_task(w->fd, t->id, t->type, t->payload, t->payload_len);

        if (t->payload) pmad_free(&quicx->pmad, t->payload);
        pmad_free(&quicx->pmad, t);
    }

}