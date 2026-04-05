#include "daemon.h"
#include "protocol.h"
#include <stdio.h>

void daemon_inflight_add(Task* t) {
    printf("[daemon] inflight add: task_id=%u type=%s\n",
           t->id, t->type);
}

void daemon_handle_message(Connection* conn, Message* msg,
                           Queue* q, WorkerPool* pool) {
    switch (msg->type) {

        case MSG_SUBMIT:
            printf("[daemon] SUBMIT received: "
                   "type=%s payload_len=%u fd=%d\n",
                   msg->task_type, msg->payload_len, conn->fd);
            protocol_send_ok(conn->fd, 1);
            conn->type = CONN_PRODUCER;
            break;

        case MSG_READY:
            printf("[daemon] READY received: fd=%d\n", conn->fd);
            protocol_send_wait(conn->fd);
            conn->type = CONN_WORKER;
            break;

        case MSG_DONE:
            printf("[daemon] DONE received: "
                   "task_id=%u fd=%d\n",
                   msg->task_id, conn->fd);
            break;

        case MSG_FAILED:
            printf("[daemon] FAILED received: "
                   "task_id=%u fd=%d\n",
                   msg->task_id, conn->fd);
            break;

        case MSG_HEARTBEAT:
            printf("[daemon] HEARTBEAT received: fd=%d\n", conn->fd);
            protocol_send_pong(conn->fd);
            break;

        case MSG_STATS:
            printf("[daemon] STATS requested: fd=%d\n", conn->fd);
            const char* json = "{\"idle\":0,\"busy\":0,\"queued\":0}";
            protocol_send_stats_response(conn->fd,
                                         (const uint8_t*)json,
                                         27);
            break;

        default:
            printf("[daemon] unknown message type=0x%02x fd=%d\n",
                   msg->type, conn->fd);
            protocol_send_error(conn->fd,
                                ERR_UNKNOWN_TYPE,
                                "unknown type");
            break;
    }
}

void daemon_handle_disconnect(Connection* conn,
                              WorkerPool* pool) {
    printf("[daemon] disconnect: fd=%d type=%d\n",
           conn->fd, conn->type);
    (void)pool;
}