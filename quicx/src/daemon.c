#include "daemon.h"
#include "protocol.h"
#include <stdio.h>
#include "dispacher.h"
#include <string.h>

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
            try_dispach(q, pool);

            break;
        }

        case MSG_FAILED: {
            
            if (conn->worker) {
                worker_set_idle(pool, conn->worker);
            }
            try_dispach(q, pool);

            break;
        }

        case MSG_HEARTBEAT: {
            protocol_send_pong(conn->fd);

            break;
        }

        case MSG_STATS: {
            char json[256];
            int len = snprintf(json, sizeof(json),
                "{\"idle\":%d,\"busy\":%d,\"queued\":%d}",
                workers_idle_count(pool),
                workers_busy_count(pool),
                queue_size(q));
            protocol_send_stats_response(conn->fd,
                                        (const uint8_t*)json,
                                        (uint32_t)len);
            
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