#include "dispacher.h"
#include <string.h>
#include <unistd.h>
#include "protocol.h"

void try_dispach(Queue* q, WorkerPool* pool) {
    while (1) {
        Worker* w = worker_get_idle(pool);
        if (!w) return;

        Task* t = queue_pop(q);
        if (!t) {
            worker_return_idle(pool, w);

            return;
        }

        worker_set_busy(pool, w);

        protocol_send_task(w->fd, t->id, t->type, t->payload, t->payload_len);

        if (t->payload) pmad_free(t->payload);
        pmad_free(t);
    }

}