#ifndef DAEMON_H
#define DAEMON_H

#include "server.h"
#include "protocol.h"
#include "queue.h"
#include "workers.h"

/* Function that handles every message type */
void daemon_handle_message(Connection* conn, Message* msg, Queue* q, WorkerPool* pool);

/* Function to disconnect and clean up a worker connection */
void daemon_handle_disconnect(Connection* conn, WorkerPool* pool);

#endif