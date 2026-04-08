#include "daemon.h"
#include "protocol.h"
#include <stdio.h>
#include "dispacher.h"
#include <string.h>
#include "stats.h"

void daemon_inflight_add(Task* t) {
    printf("[daemon] inflight add: task_id=%u type=%s\n",
           t->id, t->type);
}

void daemon_handle_message(Connection* conn, Message* msg,
                           Queue* q, WorkerPool* pool) {
    switch (msg->type) {

        case MSG_SUBMIT: {
            Task* t = (Task*)pmad_alloc(sizeof(Task));
            if (!t) {
                protocol_send_error(conn->fd, ERR_QUEUE_FULL, "queue full pmad failed");
                return;
            }

            t->id               = queue_next_id(q);
            t->status           = TASK_WAITING;
            t->retries_left     = 3;
            t->next             = NULL;
            t->payload          = msg->payload;
            t->payload_len      = msg->payload_len;
            
            strncpy(t->type, msg->task_type, 31);
            t->type[31]         = '\0';

            queue_push(q, t);
            conn->type = CONN_PRODUCER;
            protocol_send_ok(conn->fd, t->id);
            stats_task_submitted();
            try_dispach(q, pool);

            break;
        }

        case MSG_READY: {

            Worker* w = worker_add(pool, conn->fd);
            if(!w) {
                protocol_send_error(conn->fd, ERR_QUEUE_FULL, "worker pool full");
                return;
            }

            conn->worker = w;
            conn->type = CONN_WORKER;
            try_dispach(q, pool);

            break;
        }

        case MSG_DONE: {
            
            if (conn->worker) {
                worker_set_idle(pool, conn->worker);
            }
            stats_task_completed();
            try_dispach(q, pool);

            break;
        }

        case MSG_FAILED: {
            
            if (conn->worker) {
                worker_set_idle(pool, conn->worker);
            }
            stats_task_failed();
            try_dispach(q, pool);

            break;
        }

        case MSG_HEARTBEAT: {
            protocol_send_pong(conn->fd);

            break;
        }

        case MSG_STATS: {
            Stats* s = stats_get();

            PmadClassStats classes[NUM_CLASSES];
            int n = pmad_get_stats(classes, NUM_CLASSES);

            StatsHeader hdr;
            hdr.idle_workers    = htonl((uint32_t)workers_idle_count(pool));
            hdr.busy_workers    = htonl((uint32_t)workers_busy_count(pool));
            hdr.queued_tasks    = htonl((uint32_t)queue_size(q));
            hdr.tasks_submitted = htonll(s->tasks_submitted);
            hdr.tasks_completed = htonll(s->tasks_completed);
            hdr.tasks_failed    = htonll(s->tasks_failed);
            hdr.uptime_seconds  = htonll((uint64_t)(time(NULL) - s->start_time));
            hdr.class_count     = htonl((uint32_t)n);

            StatsClass sc[NUM_CLASSES];
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

void daemon_handle_disconnect(Connection* conn, WorkerPool* pool) {
    
    if (conn->type == CONN_WORKER && conn->worker) {
        worker_remove(pool, conn->worker);
        conn->worker = NULL;
    }
}