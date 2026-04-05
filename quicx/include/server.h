#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include "workers.h"
#include "queue.h"

#define MAX_CONNECTIONS 1024

typedef enum {
    CONN_UNKNOWN  = 0,
    CONN_PRODUCER = 1,
    CONN_WORKER   = 2,
} ConnType;

typedef struct Connection {
    int      fd;
    ConnType type;
    Worker*  worker;
} Connection;

int server_start(int port, Queue* q, WorkerPool* pool);

#endif