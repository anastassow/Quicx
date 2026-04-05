#ifndef DAEMON_H
#define DAEMON_H

#include "server.h"
#include "protocol.h"
#include "queue.h"
#include "workers.h"

void daemon_handle_message(Connection* conn, Message* msg, Queue* q, WorkerPool* pool);

void daemon_handle_disconnect(Connection* conn, WorkerPool* pool);

void daemon_inflight_add(Task* t);

#endif