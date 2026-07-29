#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include "workers.h"
#include "queue.h"

#define MAX_CONNECTIONS 1024

/* states of connections */
typedef enum {
    CONN_UNKNOWN  = 0,
    CONN_PRODUCER = 1,
    CONN_WORKER   = 2,
} ConnType;

/* structure for connections */
typedef struct Connection {
    int      fd;        /* fd of the connection */
    ConnType type;      /* current type of the connection */
    Worker*  worker;    /* pointer to worker, when connection type is worker */
} Connection;

/*
Function that starts the server on 
a given port with the given pool and queue.
*/
int server_start(int port, Queue* q, WorkerPool* pool);

#endif