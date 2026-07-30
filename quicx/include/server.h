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
    CONN_SERVER   = 3,
    CONN_CLI      = 4
} ConnType;

/* Structure for when it is a worker's connection */
typedef struct {
    Worker* worker;     /* pointer to worker, used when connection type is worker */
} WorkerConnectionBody;

/* structure for connections */
typedef struct Connection {
    int      fd;        /* fd of the connection */
    ConnType type;      /* current type of the connection */
    union {
        WorkerConnectionBody worker; 
    } as;
} Connection;

_Static_assert(sizeof(Connection) == 16, "Connection struct is not 16");

/*
Function that starts the server on 
a given port with the given pool and queue.
*/
int server_start(int port, Queue* q, WorkerPool* pool);

#endif