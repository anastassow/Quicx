#include "platform.h"

#include "daemon.h"
#include "protocol.h"
#include <stdio.h>
#include "dispacher.h"
#include <string.h>
#include "stats.h"

#define TASK_MAX_RETRIES 3

void daemon_handle_message(Connection* conn, Message* msg, Quicx* quicx) {
    switch (msg->type) {

        case MSG_SUBMIT: {
            Task* t = (Task*)pmad_alloc(&quicx->pmad, sizeof(Task));
            if (!t) {
                /* the payload was already allocated — give it back, not leak it */
                if (msg->as.submit.payload)
                    pmad_free(&quicx->pmad, msg->as.submit.payload);
                protocol_send_error(conn->fd, ERR_QUEUE_FULL, "queue full");
                return;
            }

            t->id               = queue_next_id(&quicx->queue);
            t->status           = TASK_WAITING;
            t->retries_left     = TASK_MAX_RETRIES;
            t->next             = NULL;
            t->payload          = msg->as.submit.payload;
            t->payload_len      = msg->as.submit.payload_len;

            strncpy(t->type, msg->as.submit.task_type, MAX_TASK_TYPE_SIZE - 1);
            t->type[MAX_TASK_TYPE_SIZE - 1] = '\0';

            t->producer_fd = conn->fd;

            queue_push(&quicx->queue, t);
            conn->type = CONN_PRODUCER;
            protocol_send_ok(conn->fd, t->id);
            stats_task_submitted(&quicx->stats);
            try_dispach(quicx);

            break;
        }

        case MSG_READY: {

            Worker* w = worker_add(&quicx->pool, &quicx->pmad, conn->fd);
            if(!w) {
                protocol_send_error(conn->fd, ERR_QUEUE_FULL, "worker pool full");
                return;
            }

            conn->as.worker.worker = w;
            conn->type = CONN_WORKER;
            try_dispach(quicx);

            break;
        }

        case MSG_DONE: {

            if (conn->as.worker.worker) {
                protocol_send_done(conn->as.worker.worker->current_fd, msg->as.done.task_id);
                worker_set_idle(&quicx->pool, conn->as.worker.worker);
                conn->as.worker.worker->current_fd = -1;
            }
            stats_task_completed(&quicx->stats);
            try_dispach(quicx);

            break;
        }

        case MSG_FAILED: {

            if (conn->as.worker.worker) {
                protocol_send_failed(conn->as.worker.worker->current_fd, msg->as.failed.task_id, msg->as.failed.reason);
                conn->as.worker.worker->current_fd = -1;
                worker_set_idle(&quicx->pool, conn->as.worker.worker);
            }
            stats_task_failed(&quicx->stats);
            try_dispach(quicx);

            break;
        }

        case MSG_HEARTBEAT: {
            protocol_send_pong(conn->fd);

            break;
        }

        case MSG_STATS: {
            Stats* s = &quicx->stats;

            PmadClassStats classes[MAX_PMAD_CLASSES];
            int n = pmad_get_stats(&quicx->pmad, classes, MAX_PMAD_CLASSES);

            MemoryPool* mp = quicx->pmad.pool_head;

            StatsHeader hdr;
            hdr.idle_workers       = htonl((uint32_t)workers_idle_count(&quicx->pool));
            hdr.busy_workers       = htonl((uint32_t)workers_busy_count(&quicx->pool));
            hdr.queued_tasks       = htonl((uint32_t)queue_size(&quicx->queue));
            hdr.workers_registered = htonl(quicx->pool.next_id - 1);
            hdr.tasks_submitted    = htonll(s->tasks_submitted);
            hdr.tasks_completed    = htonll(s->tasks_completed);
            hdr.tasks_failed       = htonll(s->tasks_failed);
            hdr.uptime_seconds     = htonll((uint64_t)(time(NULL) - s->start_time));
            hdr.pool_size          = htonll((uint64_t)quicx->pmad.pool_size);
            hdr.usable_bytes       = htonll(mp ? (uint64_t)mp->size : 0);
            hdr.used_bytes         = htonll(mp ? (uint64_t)mp->used : 0);
            hdr.class_count        = (uint8_t)n;

            StatsClass sc[MAX_PMAD_CLASSES];
            for (int i = 0; i < n; i++) {
                sc[i].block_size       = htonl(classes[i].block_size);
                sc[i].total_blocks     = htonl(classes[i].total_blocks);
                sc[i].allocated_blocks = htonl(classes[i].allocated_blocks);
            }

            protocol_send_stats_response(conn->fd,
                               &hdr,
                               (uint32_t)sizeof(hdr),
                               sc,
                               (uint32_t)(n * sizeof(StatsClass)));
            
            break;
        }

        default:{
            protocol_send_error(conn->fd,
                                ERR_UNKNOWN_TYPE,
                                "unknown type");
            break;
        }
    }
}

void daemon_handle_disconnect(Connection* conn, Quicx* quicx) {

    if (conn->type == CONN_WORKER && conn->as.worker.worker) {
        worker_remove(&quicx->pool, &quicx->pmad, conn->as.worker.worker);
        conn->as.worker.worker = NULL;
    }
}