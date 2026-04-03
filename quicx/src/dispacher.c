#include "dispacher.h"
#include <string.h>
#include <unistd.h>

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

        char msg[1024];
        int len = snprintf(msg, sizeof(msg),
                           "TASK %u %s %.*s\n",
                           t->id, t->type,
                           (int)t->payload_len,
                           (char*)t->payload);
        write(w->fd, msg, len);
    }
}